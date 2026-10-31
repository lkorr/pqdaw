#pragma once

// What a probe learns about an audio file from its headers (T5.1). Plain data, not RT-only: the browser index,
// the sample pool and the document's sample table read it.

#include <array>
#include <cstdint>
#include <string_view>

namespace pq::media {

// The container, recognised by content (magic bytes), not by file extension (MP3 without an ID3 tag excepted: a
// frame sync is weak evidence, so a bare MP3 needs two consecutive valid frame headers or the .mp3 extension).
enum class Container : uint8_t {
  Unknown,
  Wav,        // RIFF/WAVE (plain or WAVE_FORMAT_EXTENSIBLE)
  Rf64,       // RF64 / BW64 (64-bit sizes in a ds64 chunk)
  W64,        // Sony Wave64
  Aiff,       // FORM/AIFF (big endian)
  Aifc,       // FORM/AIFC (NONE, sowt, fl32, fl64, in24, in32, ...)
  Flac,
  Mp3,        // MPEG-1/2/2.5 layer I/II/III
  OggVorbis,
};
std::string_view containerName(Container c) noexcept;

// Sample encoding of uncompressed containers: where `SampleInfo::pcm` is not None, the file holds interleaved
// frames of `blockAlign` bytes at `dataOffset`, and streaming reads them directly (no decoder).
enum class PcmEncoding : uint8_t { None, U8, S8, S16, S24, S32, F32, F64 };
uint32_t pcmBytes(PcmEncoding e) noexcept;  // bytes per sample, 0 for None

// The identity the sample pool and the browser key files by: NTFS volume serial + 128-bit file ID (stable across
// renames), size and last-write time (FILETIME ticks). A file that changes keeps its ID but not its size/mtime.
struct FileIdentity {
  uint64_t volumeSerial = 0;
  std::array<uint8_t, 16> fileId{};
  uint64_t size = 0;
  int64_t mtime = 0;  // 100 ns ticks since 1601-01-01 UTC
  bool hasFileId() const noexcept;
  friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

enum class BpmSource : uint8_t { None, Acid, FileName };

struct SampleInfo {
  Container container = Container::Unknown;
  uint32_t sampleRate = 0;
  uint32_t channels = 0;
  // Exact length in sample frames, as the decoder returns them (MP3: encoder delay and padding removed).
  uint64_t frames = 0;
  // MP3 without a Xing/Info header: the length was counted by scanning every frame header once (exact for the
  // frames the decoder outputs, but no gapless information: delay/padding are 0).
  bool framesScanned = false;
  uint16_t bitsPerSample = 0;  // as stored (lossless containers); 0 for MP3 / Vorbis
  bool isFloat = false;

  // Tempo: an acid chunk's tempo, else a BPM in the file name ("120bpm", "120 BPM", "bpm_98"), else 0.
  double bpm = 0;
  BpmSource bpmSource = BpmSource::None;
  double fileNameBpm = 0;  // the file-name BPM even when an acid chunk wins (0 = none)
  // Root pitch: MIDI note (smpl unity note, acid root note or AIFF INST base note) + cents; −1 = unknown.
  int32_t rootKey = -1;
  float rootCents = 0;
  // The first sustain loop (smpl / AIFF INST), in frames, end exclusive; loopEnd == 0 = no loop.
  uint64_t loopStart = 0;
  uint64_t loopEnd = 0;

  FileIdentity identity;  // zero for probes of memory buffers

  // Uncompressed containers: the interleaved data's layout (see PcmEncoding). Compressed: pcm = None.
  PcmEncoding pcm = PcmEncoding::None;
  bool bigEndian = false;
  uint64_t dataOffset = 0;  // PCM: the first frame's byte; MP3: the first audio frame's (after a Xing/Info frame)
  uint32_t blockAlign = 0;

  // MP3 gapless (LAME tag): frames the decoder drops at the start (encoder delay + 529 decoder delay) and the end.
  uint32_t delayFrames = 0;
  uint32_t paddingFrames = 0;

  bool hasLoop() const noexcept { return loopEnd > loopStart; }
  // Decoded size in bytes as float32.
  uint64_t decodedBytes() const noexcept { return frames * channels * sizeof(float); }
};

}  // namespace pq::media
