#pragma once

// FLAC files (EX.3, docs/plans/RB.md "### EX.3"): Export Audio's lossless compressed format, written through libFLAC
// (pq_flac, BSD-3-Clause). Not RT-safe (file I/O, allocation): the export worker, tools and tests only. Decoding is
// pq/media/decoder.h's (dr_flac).
//
// FlacWriter streams like WavWriter: open() → write()* → close(); 16 or 24-bit integer samples, 1..8 channels, any rate
// FLAC allows (1 Hz … 1 048 575 Hz), libFLAC's compression level 5 (its default: the size / speed balance `flac`
// uses). write() rounds each float x to the nearest step of x · 2^(bits−1), clamped to full scale, exactly as WavWriter
// does (quantizePcm: no dither, the caller adds it; a value on the grid is written exactly), so a FLAC export and a WAV
// export of the same request hold the same samples. close() finishes the stream: libFLAC rewrites the STREAMINFO block
// (total samples, MD5 of the audio) at the head of the file. A file that was never closed is not a valid FLAC file.

#include <cstdint>
#include <filesystem>
#include <vector>

#include "pq/core/result.h"

namespace pq::media {

inline constexpr uint32_t kMaxFlacChannels = 8;
inline constexpr uint32_t kFlacCompressionLevel = 5;

class FlacWriter {
public:
  FlacWriter() = default;
  ~FlacWriter();  // closes (ignoring errors)
  FlacWriter(const FlacWriter&) = delete;
  FlacWriter& operator=(const FlacWriter&) = delete;

  // `bitsPerSample`: 16 or 24.
  Status open(const std::filesystem::path& path, uint32_t numChannels, uint32_t sampleRate, uint32_t bitsPerSample);
  // `frames` frames from planar channel pointers (numChannels of them).
  Status write(const float* const* channels, uint32_t frames);
  // Finishes the stream (STREAMINFO rewritten) and closes the file.
  Status close();
  uint64_t framesWritten() const noexcept { return frames_; }
  bool isOpen() const noexcept { return enc_ != nullptr; }

private:
  void* enc_ = nullptr;  // libFLAC's FLAC__StreamEncoder (an anonymous-struct typedef: no forward declaration)
  std::filesystem::path path_;
  uint32_t channels_ = 0;
  double scale_ = 0.0;
  uint64_t frames_ = 0;
  std::vector<int32_t> interleaved_;
  Status fail(const char* what);  // the encoder's state as an error; deletes the encoder
};

}  // namespace pq::media
