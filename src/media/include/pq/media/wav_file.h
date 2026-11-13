#pragma once

// WAV files (T1.10): the offline renderer's output and the golden-audio format (ARCHITECTURE §4: 32-bit float
// WAV). Not RT-safe (file I/O, allocation): render threads, tools and tests only.
//
// Writing: 32-bit IEEE float, any channel count 1..kMaxWavChannels (WAVE_FORMAT_IEEE_FLOAT for 1–2 channels,
// WAVE_FORMAT_EXTENSIBLE with the float subtype above that), a `fact` chunk, little endian. EX.1: or 16 / 24-bit
// integer PCM (WavFormat; WAVE_FORMAT_PCM / EXTENSIBLE with the PCM subtype, no fact chunk): write() rounds each
// float x to the nearest step of x · 2^(bits−1), clamped to full scale (no dither: the caller adds it; a value already
// on the grid, q / 2^(bits−1), is written exactly). WavWriter streams: the
// header is written with placeholder sizes and patched by close(), so a render never holds the whole file in memory.
// T6.7 (recording): crash-safe streaming — open(…, patchEveryFrames) also patches the sizes (and flushes the stream to
// the OS) whenever that many frames were written since the last patch, so a process killed mid-take leaves a readable
// file holding everything up to the last patch (the recorder: one second). A reader ignores the bytes after the
// patched data size (parseWav: unknown trailing chunks are skipped).
// M6 review M8 (long recordings): enableRf64() before open() reserves a 28-byte JUNK chunk after "WAVE"; once the file
// passes 4 GiB (RIFF's 32-bit sizes) the header becomes RF64 (EBU Tech 3306: "RF64", 32-bit sizes 0xFFFFFFFF, the
// real ones in the ds64 chunk written over the JUNK chunk) instead of write() failing. pq/media/probe.h reads it.
// Reading: RIFF and RF64; 32-bit float and 16/24/32-bit integer PCM (plain or extensible); unknown chunks are skipped. Samples are
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

// EX.1: what WavWriter writes.
enum class WavFormat : uint8_t { Float32, Pcm16, Pcm24 };

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

  // M6 review M8: before open(): past `aboveRiffBytes` (the RIFF size; tests pass a small one) the file becomes RF64.
  void enableRf64(uint64_t aboveRiffBytes = 0xFFFFFFFFull) noexcept { rf64Above_ = aboveRiffBytes; }
  bool isRf64() const noexcept { return isRf64_; }
  // `patchEveryFrames` (T6.7): > 0 patches the header every that many frames written (see the file comment).
  Status open(const std::filesystem::path& path, uint32_t numChannels, uint32_t sampleRate,
              uint64_t patchEveryFrames = 0) {
    return open(path, numChannels, sampleRate, WavFormat::Float32, patchEveryFrames);
  }
  Status open(const std::filesystem::path& path, uint32_t numChannels, uint32_t sampleRate, WavFormat format,
              uint64_t patchEveryFrames = 0);
  // `frames` frames from planar channel pointers (numChannels of them).
  Status write(const float* const* channels, uint32_t frames);
  // T6.7: writes the RIFF/data/fact sizes of what was written so far and flushes the stream (the file stays open and
  // the next write appends).
  Status patchHeader();
  // Patches the RIFF/data/fact sizes and closes the file.
  Status close();
  uint64_t framesWritten() const noexcept { return frames_; }
  uint64_t framesPatched() const noexcept { return patched_; }  // T6.7: frames the header on disk covers
  bool isOpen() const noexcept { return out_.is_open(); }

private:
  std::ofstream out_;
  std::filesystem::path path_;
  uint32_t channels_ = 0;
  uint32_t bytesPer_ = 4;       // EX.1: bytes per sample (4: float, 2 / 3: PCM16 / PCM24)
  WavFormat format_ = WavFormat::Float32;
  uint64_t frames_ = 0;
  uint32_t headerBytes_ = 0;    // bytes before the sample data
  uint32_t factOffset_ = 0;     // file offset of the fact chunk's sample count
  uint64_t patchEvery_ = 0;     // T6.7
  uint64_t patched_ = 0;
  std::vector<float> interleaved_;
  std::vector<uint8_t> packed_;  // EX.1: PCM frames
  uint64_t rf64Above_ = 0;      // M8: 0 = plain RIFF only
  bool isRf64_ = false;
  void writeSizes();
};

}  // namespace pq::media
