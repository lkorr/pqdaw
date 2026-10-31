#pragma once

// Decoding (T5.1): every supported format behind one interface. Not RT-safe: decoders run on worker, disk and
// render threads only, never on the audio or UI thread.
//
// Formats: WAV / RF64 / W64 / AIFF / AIFC PCM (8/16/24/32-bit integer, 32/64-bit float; our own reader, which
// converts and deinterleaves straight from the mapped file), other WAV/AIFF encodings dr_wav decodes (MS/IMA ADPCM,
// A-law, µ-law), FLAC (dr_flac), MP3 (dr_mp3; gapless from the LAME tag; a seek table built once on the first
// seek and kept), OGG Vorbis (stb_vorbis). Output is planar float32, integers scaled by 2^-(bits-1) (so lossless
// sources decode bit-exactly to their integer values).
//
// Files are memory-mapped read-only. Allocation is bounded independently of what headers declare: the third-party
// decoders get allocators capped at kCodecHeapLimit, and nothing is sized from a declared length.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include "pq/core/result.h"
#include "pq/media/sample_info.h"

namespace pq::media {

inline constexpr uint32_t kMaxDecodeChannels = 32;
// Upper bound of what one decoder's third-party code may allocate (codebooks, seek tables, frame buffers).
inline constexpr std::size_t kCodecHeapLimit = 64u << 20;

class Decoder {
public:
  // Opens and probes the file (the same SampleInfo probe() returns). The mapping lives as long as the decoder.
  static Result<std::unique_ptr<Decoder>> open(const std::filesystem::path& path);
  // Decodes a file held in memory; `file` must outlive the decoder. `fileName` as for probeBytes().
  static Result<std::unique_ptr<Decoder>> openMemory(std::span<const uint8_t> file, std::string_view fileName);

  virtual ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  const SampleInfo& info() const noexcept { return info_; }
  // The frame the next read() returns.
  uint64_t position() const noexcept { return position_; }

  // Reads up to `frames` frames into info().channels planar buffers; returns the frames read (0 = end). Fewer than
  // asked before info().frames means the stream ended early (truncated or corrupt data the decoder stops at):
  // truncated() turns true and what was decoded stays valid, so a damaged file plays as far as it can.
  Result<uint64_t> read(float* const* dst, uint64_t frames);
  // Moves to `frame` (≤ info().frames). Exact for every format: the next read() returns what a sequential decode
  // returns at that frame.
  Status seek(uint64_t frame);
  bool truncated() const noexcept { return truncated_; }

protected:
  Decoder() = default;
  virtual Result<uint64_t> readImpl(float* const* dst, uint64_t frames) = 0;
  virtual Status seekImpl(uint64_t frame) = 0;

  SampleInfo info_;
  uint64_t position_ = 0;
  bool truncated_ = false;
};

}  // namespace pq::media
