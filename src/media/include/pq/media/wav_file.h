#pragma once

// WAV files (T1.10): the offline renderer's output and the golden-audio format (ARCHITECTURE §4: 32-bit float
// WAV). Not RT-safe (file I/O, allocation): render threads, tools and tests only.
//
// Writing: 32-bit IEEE float, any channel count 1..kMaxWavChannels (WAVE_FORMAT_IEEE_FLOAT for 1–2 channels,
// WAVE_FORMAT_EXTENSIBLE with the float subtype above that), a `fact` chunk, little endian. WavWriter streams: the
// header is written with placeholder sizes and patched by close(), so a render never holds the whole file in memory.
// Reading: 32-bit float and 16/24/32-bit integer PCM (plain or extensible); unknown chunks are skipped. Samples are
// returned planar as float (integers scaled to [−1, 1)).
// T5.1: moved from engine to media (pq/engine/wav_file.h forwards here). This whole-file reader stays the golden /
// render path; general decoding (every format, seeking, metadata) is pq/media/decoder.h.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "pq/core/result.h"

namespace pq::media {

inline constexpr uint32_t kMaxWavChannels = 32;

struct WavData {
  uint32_t sampleRate = 0;
  std::vector<std::vector<float>> channels;  // planar, equal lengths
  uint16_t bitsPerSample = 32;               // as stored in the file
  bool isFloat = true;                       // as stored in the file

  uint32_t numChannels() const noexcept { return static_cast<uint32_t>(channels.size()); }
  uint64_t frames() const noexcept { return channels.empty() ? 0 : channels.front().size(); }
};

Result<WavData> readWav(const std::filesystem::path& path);
// Parses a whole file held in memory (what readWav does after reading it).
Result<WavData> parseWav(const uint8_t* bytes, size_t size);

// Writes `channels` (planar, equal lengths) as 32-bit float.
Status writeWav(const std::filesystem::path& path, const std::vector<std::vector<float>>& channels,
                uint32_t sampleRate);

// T5.12: writes `channels` as 24-bit integer PCM (rounded to the nearest step, clamped to full scale; no dither).
Status writeWavPcm24(const std::filesystem::path& path, const std::vector<std::vector<float>>& channels,
                     uint32_t sampleRate);

// Streams a 32-bit float WAV file. open() → write()* → close(); the destructor closes (ignoring errors).
class WavWriter {
public:
  WavWriter() = default;
  ~WavWriter();
  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  Status open(const std::filesystem::path& path, uint32_t numChannels, uint32_t sampleRate);
  // `frames` frames from planar channel pointers (numChannels of them).
  Status write(const float* const* channels, uint32_t frames);
  // Patches the RIFF/data/fact sizes and closes the file.
  Status close();
  uint64_t framesWritten() const noexcept { return frames_; }

private:
  std::ofstream out_;
  std::filesystem::path path_;
  uint32_t channels_ = 0;
  uint64_t frames_ = 0;
  uint32_t headerBytes_ = 0;    // bytes before the sample data
  uint32_t factOffset_ = 0;     // file offset of the fact chunk's sample count
  std::vector<float> interleaved_;
};

}  // namespace pq::media
