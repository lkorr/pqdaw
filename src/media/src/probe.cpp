#include "pq/media/probe.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "codecs.h"
#include "file_io.h"
#include "mp3_layout.h"
#include "pq/media/decoder.h"

namespace pq::media {

std::string_view containerName(Container c) noexcept {
  switch (c) {
    case Container::Wav: return "WAV";
    case Container::Rf64: return "RF64";
    case Container::W64: return "W64";
    case Container::Aiff: return "AIFF";
    case Container::Aifc: return "AIFC";
    case Container::Flac: return "FLAC";
    case Container::Mp3: return "MP3";
    case Container::OggVorbis: return "OGG Vorbis";
    case Container::Unknown: break;
  }
  return "unknown";
}

bool FileIdentity::hasFileId() const noexcept {
  for (uint8_t b : fileId)
    if (b != 0) return true;
  return false;
}

namespace {

using Bytes = std::span<const uint8_t>;

Error parseError(const std::string& what) { return Error{ErrorCode::Parse, what, {}}; }
Error unsupported(const std::string& what) { return Error{ErrorCode::Unsupported, what, {}}; }

// Bounds-checked little/big-endian readers over the whole file; every caller checks `has(off, n)` first.
struct View {
  Bytes b;
  bool has(uint64_t off, uint64_t n) const noexcept { return off <= b.size() && n <= b.size() - off; }
  const uint8_t* at(uint64_t off) const noexcept { return b.data() + off; }
  uint16_t le16(uint64_t o) const noexcept { return static_cast<uint16_t>(b[o] | b[o + 1] << 8); }
  uint32_t le32(uint64_t o) const noexcept {
    return uint32_t{b[o]} | uint32_t{b[o + 1]} << 8 | uint32_t{b[o + 2]} << 16 | uint32_t{b[o + 3]} << 24;
  }
  uint64_t le64(uint64_t o) const noexcept { return uint64_t{le32(o)} | uint64_t{le32(o + 4)} << 32; }
  uint16_t be16(uint64_t o) const noexcept { return static_cast<uint16_t>(b[o] << 8 | b[o + 1]); }
  uint32_t be32(uint64_t o) const noexcept {
    return uint32_t{b[o]} << 24 | uint32_t{b[o + 1]} << 16 | uint32_t{b[o + 2]} << 8 | uint32_t{b[o + 3]};
  }
  bool tag(uint64_t o, const char* t) const noexcept { return has(o, 4) && std::memcmp(at(o), t, 4) == 0; }
};

constexpr uint32_t kMaxChunks = 4096;  // a chunk walk gives up after this many chunks (malformed files loop otherwise)
// M5 review H1: a compressed file's declared length (FLAC STREAMINFO, the last Ogg granule) is capped by the file size
// at a ratio no real encoder reaches (FLAC's best case, a constant 65535-frame block, is ~4,700 frames per byte;
// Vorbis' 1-byte 4096-frame packets ~2,000), so a corrupt header can never make a consumer size buffers in terabytes.
constexpr uint64_t kMaxFramesPerByte = 8192;
// The smallest MPEG audio frame (MPEG-2 layer III at 8 kb/s, 24 kHz: 24 bytes; 16 leaves room): caps a Xing count.
constexpr uint64_t kMinMp3FrameBytes = 16;

bool sane(uint32_t rate, uint32_t channels) noexcept {
  return rate >= 1 && rate <= 1536000 && channels >= 1 && channels <= kMaxDecodeChannels;
}

// ---- WAV / RF64 ---------------------------------------------------------------------------------------------------
constexpr uint8_t kSubtypeTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                      0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

struct WavFmt {
  uint16_t tag = 0;
  uint16_t channels = 0;
  uint32_t rate = 0;
  uint16_t blockAlign = 0;
  uint16_t bits = 0;
};

// Integer / float encodings by container bytes per sample (block align / channels), so 20-bit-in-24 decodes as 24.
Status pcmFromFmt(const WavFmt& f, SampleInfo& info) {
  if (!sane(f.rate, f.channels)) return parseError("WAV: bad sample rate or channel count");
  if (f.blockAlign == 0 || f.blockAlign % f.channels != 0) return parseError("WAV: inconsistent block align");
  const uint32_t bytes = f.blockAlign / f.channels;
  info.sampleRate = f.rate;
  info.channels = f.channels;
  info.bitsPerSample = f.bits;
  info.blockAlign = f.blockAlign;
  if (f.tag == 1) {
    if (bytes == 1) info.pcm = PcmEncoding::U8;
    else if (bytes == 2) info.pcm = PcmEncoding::S16;
    else if (bytes == 3) info.pcm = PcmEncoding::S24;
    else if (bytes == 4) info.pcm = PcmEncoding::S32;
    else return unsupported("WAV: " + std::to_string(f.bits) + "-bit integer PCM");
    if (f.bits > bytes * 8 || f.bits == 0) return parseError("WAV: bits per sample exceed the block align");
  } else if (f.tag == 3) {
    info.isFloat = true;
    if (bytes == 4) info.pcm = PcmEncoding::F32;
    else if (bytes == 8) info.pcm = PcmEncoding::F64;
    else return unsupported("WAV: " + std::to_string(f.bits) + "-bit float");
  } else if (f.tag == 2 || f.tag == 6 || f.tag == 7 || f.tag == 0x11) {
    info.pcm = PcmEncoding::None;  // MS ADPCM, A-law, µ-law, IMA ADPCM: dr_wav decodes them
  } else {
    return unsupported("WAV: format tag " + std::to_string(f.tag));
  }
  return Ok{};
}

void readSmpl(const View& v, uint64_t off, uint64_t size, SampleInfo& info) {
  if (size < 36 || !v.has(off, 36)) return;
  const uint32_t unity = v.le32(off + 12);
  const uint32_t fraction = v.le32(off + 16);
  if (unity <= 127) {
    info.rootKey = static_cast<int32_t>(unity);
    info.rootCents = static_cast<float>(static_cast<double>(fraction) / 4294967296.0 * 100.0);
  }
  const uint32_t loops = v.le32(off + 28);
  if (loops >= 1 && size >= 36 + 24 && v.has(off + 36, 24)) {
    const uint32_t start = v.le32(off + 36 + 8);
    const uint32_t end = v.le32(off + 36 + 12);  // inclusive
    if (end >= start) {
      info.loopStart = start;
      info.loopEnd = uint64_t{end} + 1;
    }
  }
}

void readAcid(const View& v, uint64_t off, uint64_t size, SampleInfo& info) {
  if (size < 24 || !v.has(off, 24)) return;
  const uint32_t flags = v.le32(off);
  const uint16_t root = v.le16(off + 4);
  float tempo;
  std::memcpy(&tempo, v.at(off + 20), 4);
  if ((flags & 0x02) && root <= 127 && info.rootKey < 0) info.rootKey = root;
  if (std::isfinite(tempo) && tempo >= 1.0f && tempo <= 999.0f) {
    info.bpm = tempo;
    info.bpmSource = BpmSource::Acid;
  }
}

Status parseWav(const View& v, SampleInfo& info, bool rf64) {
  info.container = rf64 ? Container::Rf64 : Container::Wav;
  WavFmt fmt;
  bool haveFmt = false;
  uint64_t dataOff = 0, dataBytes = 0;
  bool haveData = false;
  uint64_t ds64Data = 0, ds64Samples = 0, factFrames = 0;
  bool haveFact = false;
  uint64_t pos = 12;
  for (uint32_t n = 0; n < kMaxChunks && v.has(pos, 8); ++n) {
    uint64_t size = v.le32(pos + 4);
    const uint64_t body = pos + 8;
    if (v.tag(pos, "ds64") && size >= 24 && v.has(body, 24)) {
      ds64Data = v.le64(body + 8);
      ds64Samples = v.le64(body + 16);
    } else if (v.tag(pos, "fmt ")) {
      if (size < 16 || !v.has(body, 16)) return parseError("WAV: truncated fmt chunk");
      fmt.tag = v.le16(body);
      fmt.channels = v.le16(body + 2);
      fmt.rate = v.le32(body + 4);
      fmt.blockAlign = v.le16(body + 12);
      fmt.bits = v.le16(body + 14);
      if (fmt.tag == 0xFFFE) {
        if (size < 40 || !v.has(body, 40)) return parseError("WAV: truncated extensible fmt chunk");
        if (std::memcmp(v.at(body + 26), kSubtypeTail, sizeof kSubtypeTail) != 0)
          return unsupported("WAV: unknown extensible subtype");
        fmt.tag = v.le16(body + 24);
      }
      haveFmt = true;
    } else if (v.tag(pos, "fact") && size >= 4 && v.has(body, 4)) {
      factFrames = v.le32(body);
      haveFact = true;
    } else if (v.tag(pos, "smpl")) {
      readSmpl(v, body, size, info);
    } else if (v.tag(pos, "acid")) {
      readAcid(v, body, size, info);
    } else if (v.tag(pos, "data")) {
      if (rf64 && size == 0xFFFFFFFFu) size = ds64Data;
      dataOff = body;
      dataBytes = std::min<uint64_t>(size, v.b.size() - std::min<uint64_t>(body, v.b.size()));  // capped by the file
      haveData = true;
      if (size > v.b.size() - std::min<uint64_t>(body, v.b.size())) break;  // truncated file: nothing follows
    }
    if (size > v.b.size()) break;
    pos = body + size + (size & 1);
  }
  if (!haveFmt) return parseError("WAV: no fmt chunk");
  if (!haveData) return parseError("WAV: no data chunk");
  if (auto s = pcmFromFmt(fmt, info); !s) return s;
  info.dataOffset = dataOff;
  if (info.pcm != PcmEncoding::None) {
    info.frames = dataBytes / info.blockAlign;
  } else if (fmt.tag == 6 || fmt.tag == 7) {
    info.frames = dataBytes / fmt.blockAlign;
  } else {
    // ADPCM: dr_wav knows the block layouts; its frame count from the headers (no decoding).
    drwav w;
    detail::CappedHeap heap(kCodecHeapLimit);
    auto cb = heap.callbacks<drwav_allocation_callbacks>();
    if (!drwav_init_memory(&w, v.b.data(), v.b.size(), &cb)) return parseError("WAV: unreadable ADPCM header");
    info.frames = w.totalPCMFrameCount;
    drwav_uninit(&w);
    (void)haveFact;
    (void)factFrames;
  }
  (void)ds64Samples;
  return Ok{};
}

// ---- W64 ----------------------------------------------------------------------------------------------------------
constexpr uint8_t kW64Riff[16] = {'r', 'i', 'f', 'f', 0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
constexpr uint8_t kW64Wave[16] = {'w', 'a', 'v', 'e', 0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};
constexpr uint8_t kW64Tail[12] = {0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};

Status parseW64(const View& v, SampleInfo& info) {
  info.container = Container::W64;
  WavFmt fmt;
  bool haveFmt = false, haveData = false;
  uint64_t pos = 40;
  for (uint32_t n = 0; n < kMaxChunks && v.has(pos, 24); ++n) {
    const uint64_t size = v.le64(pos + 16);  // includes the 24-byte header
    if (size < 24) return parseError("W64: bad chunk size");
    const uint64_t body = pos + 24;
    const bool known = std::memcmp(v.at(pos + 4), kW64Tail, 12) == 0;
    if (known && v.tag(pos, "fmt ")) {
      if (size < 24 + 16 || !v.has(body, 16)) return parseError("W64: truncated fmt chunk");
      fmt.tag = v.le16(body);
      fmt.channels = v.le16(body + 2);
      fmt.rate = v.le32(body + 4);
      fmt.blockAlign = v.le16(body + 12);
      fmt.bits = v.le16(body + 14);
      if (fmt.tag == 0xFFFE) {
        if (size < 24 + 40 || !v.has(body, 40)) return parseError("W64: truncated extensible fmt chunk");
        fmt.tag = v.le16(body + 24);
      }
      haveFmt = true;
    } else if (known && v.tag(pos, "data")) {
      info.dataOffset = body;
      const uint64_t avail = v.b.size() - std::min<uint64_t>(body, v.b.size());
      info.frames = std::min(size - 24, avail);  // bytes for now
      haveData = true;
      break;
    }
    if (size > v.b.size()) break;
    pos += (size + 7) & ~uint64_t{7};
  }
  if (!haveFmt) return parseError("W64: no fmt chunk");
  if (!haveData) return parseError("W64: no data chunk");
  if (auto s = pcmFromFmt(fmt, info); !s) return s;
  if (info.pcm == PcmEncoding::None) return unsupported("W64: only PCM / float");
  info.frames /= info.blockAlign;
  return Ok{};
}

// ---- AIFF / AIFC --------------------------------------------------------------------------------------------------
// IEEE 754 80-bit extended → integer Hz (0 if out of range or not finite).
uint32_t extendedRate(const uint8_t* p) noexcept {
  const int exponent = ((p[0] & 0x7F) << 8 | p[1]) - 16383;
  uint64_t mant = 0;
  for (int i = 0; i < 8; ++i) mant = mant << 8 | p[2 + i];
  if ((p[0] & 0x80) || mant == 0 || exponent < 0 || exponent > 30) return 0;
  const double rate = std::ldexp(static_cast<double>(mant), exponent - 63);
  return rate >= 1 && rate <= 1536000 ? static_cast<uint32_t>(std::lround(rate)) : 0;
}

Status parseAiff(const View& v, SampleInfo& info, bool aifc) {
  info.container = aifc ? Container::Aifc : Container::Aiff;
  bool haveComm = false, haveSsnd = false;
  uint32_t declaredFrames = 0;
  uint16_t sampleSize = 0;
  uint32_t compression = 0x4E4F4E45;  // 'NONE'
  uint64_t ssndData = 0;
  struct Marker {
    uint16_t id;
    uint32_t pos;
  };
  std::vector<Marker> markers;
  int16_t loopMode = 0;
  uint16_t loopBegin = 0, loopEnd = 0;
  bool haveInst = false;
  uint64_t pos = 12;
  for (uint32_t n = 0; n < kMaxChunks && v.has(pos, 8); ++n) {
    const uint64_t size = v.be32(pos + 4);
    const uint64_t body = pos + 8;
    if (v.tag(pos, "COMM")) {
      if (size < 18 || !v.has(body, 18)) return parseError("AIFF: truncated COMM chunk");
      info.channels = v.be16(body);
      declaredFrames = v.be32(body + 2);
      sampleSize = v.be16(body + 6);
      info.sampleRate = extendedRate(v.at(body + 8));
      if (aifc) {
        if (size < 22 || !v.has(body, 22)) return parseError("AIFC: truncated COMM chunk");
        compression = v.be32(body + 18);
      }
      haveComm = true;
    } else if (v.tag(pos, "SSND")) {
      if (size < 8 || !v.has(body, 8)) return parseError("AIFF: truncated SSND chunk");
      ssndData = body + 8 + v.be32(body);
      haveSsnd = true;
    } else if (v.tag(pos, "MARK") && size >= 2 && v.has(body, 2)) {
      const uint16_t count = v.be16(body);
      uint64_t p = body + 2;
      for (uint16_t i = 0; i < count && v.has(p, 7) && p + 7 <= body + size; ++i) {
        markers.push_back({v.be16(p), v.be32(p + 2)});
        const uint64_t nameLen = v.b[p + 6];
        p += 6 + ((1 + nameLen + 1) & ~uint64_t{1});  // pstring padded to an even total
      }
    } else if (v.tag(pos, "INST") && size >= 20 && v.has(body, 20)) {
      const auto base = static_cast<int8_t>(v.b[body]);
      const auto detune = static_cast<int8_t>(v.b[body + 1]);
      if (base >= 0) {
        info.rootKey = base;
        info.rootCents = static_cast<float>(detune);
      }
      loopMode = static_cast<int16_t>(v.be16(body + 8));
      loopBegin = v.be16(body + 10);
      loopEnd = v.be16(body + 12);
      haveInst = true;
    }
    if (size > v.b.size()) break;
    pos = body + size + (size & 1);
  }
  if (!haveComm) return parseError("AIFF: no COMM chunk");
  if (!haveSsnd) return parseError("AIFF: no SSND chunk");
  if (!sane(info.sampleRate, info.channels)) return parseError("AIFF: bad sample rate or channel count");
  info.bitsPerSample = sampleSize;
  const uint32_t intBytes = (sampleSize + 7u) / 8u;
  info.bigEndian = true;
  switch (compression) {
    case 0x4E4F4E45:  // NONE
    case 0x74776F73:  // twos
      break;
    case 0x736F7774:  // sowt (little endian)
      info.bigEndian = false;
      break;
    case 0x666C3332:  // fl32
    case 0x464C3332:  // FL32
      info.pcm = PcmEncoding::F32;
      info.isFloat = true;
      info.bitsPerSample = 32;
      break;
    case 0x666C3634:  // fl64
    case 0x464C3634:  // FL64
      info.pcm = PcmEncoding::F64;
      info.isFloat = true;
      info.bitsPerSample = 64;
      break;
    case 0x696E3234: info.pcm = PcmEncoding::S24; info.bitsPerSample = 24; break;  // in24
    case 0x696E3332: info.pcm = PcmEncoding::S32; info.bitsPerSample = 32; break;  // in32
    default: return unsupported("AIFC: compression type not supported");
  }
  if (info.pcm == PcmEncoding::None) {
    if (intBytes == 1) info.pcm = PcmEncoding::S8;
    else if (intBytes == 2) info.pcm = PcmEncoding::S16;
    else if (intBytes == 3) info.pcm = PcmEncoding::S24;
    else if (intBytes == 4) info.pcm = PcmEncoding::S32;
    else return unsupported("AIFF: " + std::to_string(sampleSize) + "-bit samples");
  }
  if (info.pcm == PcmEncoding::S8) info.bigEndian = false;  // byte order is moot; keeps the fast path
  info.blockAlign = pcmBytes(info.pcm) * info.channels;
  info.dataOffset = ssndData;
  const uint64_t avail = ssndData < v.b.size() ? v.b.size() - ssndData : 0;
  info.frames = std::min<uint64_t>(declaredFrames, avail / info.blockAlign);
  if (haveInst && loopMode != 0) {
    int64_t b = -1, e = -1;
    for (const Marker& m : markers) {
      if (m.id == loopBegin) b = m.pos;
      if (m.id == loopEnd) e = m.pos;
    }
    if (b >= 0 && e > b) {
      info.loopStart = static_cast<uint64_t>(b);
      info.loopEnd = static_cast<uint64_t>(e);
    }
  }
  return Ok{};
}

}  // namespace

// ---- shared with the MP3 decoder (src/media/src/mp3_layout.h) ------------------------------------------------------
namespace detail {

uint64_t skipId3(std::span<const uint8_t> file) noexcept {
  const View v{file};
  uint64_t pos = 0;
  while (v.has(pos, 10) && v.b[pos] == 'I' && v.b[pos + 1] == 'D' && v.b[pos + 2] == '3') {
    const uint8_t* p = v.at(pos);
    if ((p[6] | p[7] | p[8] | p[9]) & 0x80) break;
    const uint64_t size = uint64_t{p[6]} << 21 | uint64_t{p[7]} << 14 | uint64_t{p[8]} << 7 | p[9];
    pos += 10 + size + ((p[5] & 0x10) ? 10 : 0);  // footer flag
  }
  return pos;
}

// End of the audio frames: before an ID3v1 tag ("TAG", last 128 bytes) and/or an APEv2 footer.
uint64_t mp3End(std::span<const uint8_t> file) noexcept {
  const View v{file};
  uint64_t end = v.b.size();
  if (end >= 128 && std::memcmp(v.at(end - 128), "TAG", 3) == 0) end -= 128;
  if (end >= 32 && std::memcmp(v.at(end - 32), "APETAGEX", 8) == 0) {
    const uint64_t apeSize = v.le32(end - 32 + 12) + 32ull;  // tag size excludes the 32-byte header
    if (apeSize <= end) end -= apeSize;
  }
  return end;
}

}  // namespace detail

namespace {

uint64_t skipId3(const View& v) noexcept { return detail::skipId3(v.b); }

// ---- FLAC ---------------------------------------------------------------------------------------------------------

Status parseFlac(const View& v, uint64_t start, SampleInfo& info) {
  info.container = Container::Flac;
  uint64_t pos = start + 4;
  bool haveInfo = false;
  for (uint32_t n = 0; n < kMaxChunks && v.has(pos, 4); ++n) {
    const uint8_t type = v.b[pos] & 0x7F;
    const bool last = (v.b[pos] & 0x80) != 0;
    const uint32_t size = uint32_t{v.b[pos + 1]} << 16 | uint32_t{v.b[pos + 2]} << 8 | v.b[pos + 3];
    if (type == 0) {
      if (size < 34 || !v.has(pos + 4, 34)) return parseError("FLAC: truncated STREAMINFO");
      const uint8_t* p = v.at(pos + 4);
      info.sampleRate = uint32_t{p[10]} << 12 | uint32_t{p[11]} << 4 | uint32_t{p[12]} >> 4;
      info.channels = ((p[12] >> 1) & 7u) + 1u;
      info.bitsPerSample = static_cast<uint16_t>((((p[12] & 1u) << 4) | (p[13] >> 4)) + 1u);
      info.frames = uint64_t{p[13] & 0x0Fu} << 32 | uint64_t{p[14]} << 24 | uint64_t{p[15]} << 16 |
                    uint64_t{p[16]} << 8 | p[17];
      haveInfo = true;
    }
    pos += 4 + uint64_t{size};
    if (last) break;
  }
  if (!haveInfo) return parseError("FLAC: no STREAMINFO");
  if (!sane(info.sampleRate, info.channels)) return parseError("FLAC: bad sample rate or channel count");
  if (info.bitsPerSample < 4 || info.bitsPerSample > 32) return parseError("FLAC: bad bit depth");
  // A length the file cannot hold is not trusted: count by decoding, as for an unknown length.
  if (info.frames > (v.b.size() + 1) * kMaxFramesPerByte) info.frames = 0;
  if (info.frames == 0) {
    // Length not in STREAMINFO (a stream that was never finalised): count by decoding once.
    detail::CappedHeap heap(kCodecHeapLimit);
    auto cb = heap.callbacks<drflac_allocation_callbacks>();
    drflac* f = drflac_open_memory(v.b.data(), v.b.size(), &cb);
    if (!f) return parseError("FLAC: undecodable stream");
    std::vector<int32_t> scratch(size_t{4096} * f->channels);
    uint64_t total = 0;
    for (uint64_t got; (got = drflac_read_pcm_frames_s32(f, 4096, scratch.data())) > 0;) total += got;
    drflac_close(f);
    info.frames = total;
    info.framesScanned = true;
  }
  return Ok{};
}

// ---- OGG Vorbis ---------------------------------------------------------------------------------------------------
// Header length of the page at `pos`, or 0 if it is not a valid page header within the file.
uint64_t oggPageHeader(const View& v, uint64_t pos) noexcept {
  if (!v.tag(pos, "OggS") || !v.has(pos, 27) || v.b[pos + 4] != 0) return 0;
  const uint32_t segs = v.b[pos + 26];
  if (!v.has(pos + 27, segs)) return 0;
  return 27 + uint64_t{segs};
}

// The stream's first three packets (identification, comment, setup), reassembled from the pages of `serial`.
// Returns false if they are not all complete.
bool oggHeaderPackets(const View& v, uint32_t serial, std::vector<uint8_t> (&packets)[3]) {
  uint32_t done = 0;
  for (uint64_t pos = 0; done < 3;) {
    const uint64_t hdr = oggPageHeader(v, pos);
    if (hdr == 0) return false;
    const uint32_t segs = v.b[pos + 26];
    uint64_t body = pos + hdr;
    const bool mine = v.le32(pos + 14) == serial;
    for (uint32_t s = 0; s < segs && done < 3; ++s) {
      const uint32_t lace = v.b[pos + 27 + s];
      if (!v.has(body, lace)) return false;
      if (mine) {
        packets[done].insert(packets[done].end(), v.at(body), v.at(body) + lace);
        if (lace < 255) ++done;
      }
      body += lace;
    }
    pos = body;
  }
  return true;
}

// Checks the comment packet's own length fields against the packet, so a decoder that trusts them (stb_vorbis
// writes vendor[len] for a vendor length of −1) never sees an inconsistent one.
bool vorbisCommentsValid(const std::vector<uint8_t>& p) noexcept {
  if (p.size() < 7 + 8 + 1 || p[0] != 0x03 || std::memcmp(p.data() + 1, "vorbis", 6) != 0) return false;
  const View v{p};
  uint64_t pos = 7;
  const uint64_t vendor = v.le32(pos);
  pos += 4;
  if (vendor > 0x7FFFFFFF || !v.has(pos, vendor + 4)) return false;
  pos += vendor;
  const uint64_t count = v.le32(pos);
  pos += 4;
  if (count > 0x7FFFFFFF) return false;
  for (uint64_t i = 0; i < count; ++i) {
    if (!v.has(pos, 4)) return false;
    const uint64_t len = v.le32(pos);
    pos += 4;
    if (len > 0x7FFFFFFF || !v.has(pos, len)) return false;
    pos += len;
  }
  return v.has(pos, 1) && (p[pos] & 1) != 0;  // framing bit
}

Status parseOgg(const View& v, SampleInfo& info) {
  info.container = Container::OggVorbis;
  const uint64_t hdr = oggPageHeader(v, 0);
  if (hdr == 0) return parseError("OGG: bad first page");
  const uint32_t serial = v.le32(14);
  const uint64_t pkt = hdr;
  if (!v.has(pkt, 30) || v.b[pkt] != 0x01 || std::memcmp(v.at(pkt + 1), "vorbis", 6) != 0)
    return unsupported("OGG: not a Vorbis stream");
  std::vector<uint8_t> packets[3];
  if (!oggHeaderPackets(v, serial, packets)) return parseError("OGG: incomplete Vorbis headers");
  if (!vorbisCommentsValid(packets[1])) return parseError("OGG: malformed Vorbis comment header");
  if (packets[2].size() < 7 || packets[2][0] != 0x05 || std::memcmp(packets[2].data() + 1, "vorbis", 6) != 0)
    return parseError("OGG: missing Vorbis setup header");
  info.channels = v.b[pkt + 11];
  info.sampleRate = v.le32(pkt + 12);
  if (!sane(info.sampleRate, info.channels)) return parseError("OGG: bad sample rate or channel count");
  // Length = the granule position of the last page of this stream that ends a packet (scan back from the end).
  const uint64_t size = v.b.size();
  const uint64_t from = size > (64u << 10) ? size - (64u << 10) : 0;
  int64_t last = -1;
  for (uint64_t pos = size >= 27 ? size - 27 : 0; pos + 1 > from && pos + 27 <= size; --pos) {  // from the end back
    if (v.b[pos] == 'O' && oggPageHeader(v, pos) != 0 && v.le32(pos + 14) == serial) {
      const auto granule = static_cast<int64_t>(v.le64(pos + 6));
      if (granule >= 0) {
        last = granule;
        break;
      }
    }
    if (pos == 0) break;
  }
  if (last < 0) return parseError("OGG: no final granule position");
  info.frames = std::min<uint64_t>(static_cast<uint64_t>(last), (size + 1) * kMaxFramesPerByte);
  return Ok{};
}

// ---- MP3 ----------------------------------------------------------------------------------------------------------
struct Mp3Header {
  uint32_t frameBytes = 0;
  uint32_t samples = 0;  // per frame
  uint32_t rate = 0;
  uint32_t channels = 0;
  uint32_t sideInfo = 0;  // layer III side-info bytes
  bool mpeg1 = false;
  bool layer3 = false;
  bool crc = false;
};

bool mp3Header(const uint8_t* h, Mp3Header& out) noexcept {
  if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
  const uint32_t version = (h[1] >> 3) & 3;  // 0 = 2.5, 2 = 2, 3 = 1
  const uint32_t layer = (h[1] >> 1) & 3;    // 1 = III, 2 = II, 3 = I
  const uint32_t brIndex = h[2] >> 4;
  const uint32_t srIndex = (h[2] >> 2) & 3;
  if (version == 1 || layer == 0 || brIndex == 0 || brIndex == 15 || srIndex == 3) return false;
  static constexpr uint16_t kRates[3] = {44100, 48000, 32000};
  static constexpr uint16_t kBr[2][3][15] = {
      // MPEG-1: layer I, II, III
      {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
       {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
       {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
      // MPEG-2 / 2.5: layer I, II, III
      {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
       {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
       {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
  const bool mpeg1 = version == 3;
  const uint32_t layerIdx = 3 - layer;  // 0 = I, 1 = II, 2 = III
  const uint32_t br = kBr[mpeg1 ? 0 : 1][layerIdx][brIndex] * 1000u;
  uint32_t rate = kRates[srIndex];
  if (version == 2) rate /= 2;
  if (version == 0) rate /= 4;
  const uint32_t pad = (h[2] >> 1) & 1;
  out.mpeg1 = mpeg1;
  out.layer3 = layerIdx == 2;
  out.rate = rate;
  out.channels = ((h[3] >> 6) == 3) ? 1 : 2;
  out.crc = (h[1] & 1) == 0;
  if (layerIdx == 0) {
    out.samples = 384;
    out.frameBytes = (12 * br / rate + pad) * 4;
  } else if (layerIdx == 1) {
    out.samples = 1152;
    out.frameBytes = 144 * br / rate + pad;
  } else {
    out.samples = mpeg1 ? 1152 : 576;
    out.frameBytes = (mpeg1 ? 144 : 72) * br / rate + pad;
  }
  out.sideInfo = mpeg1 ? (out.channels == 1 ? 17 : 32) : (out.channels == 1 ? 9 : 17);
  return out.frameBytes >= 4;
}

uint64_t mp3End(const View& v) noexcept { return detail::mp3End(v.b); }

// The first frame at or after `pos` that is followed by another valid frame header (or the end of the data).
bool findMp3Sync(const View& v, uint64_t pos, uint64_t end, uint64_t limit, uint64_t& at, Mp3Header& h) noexcept {
  for (uint64_t p = pos; p + 4 <= end && p <= limit; ++p) {
    if (v.b[p] != 0xFF) continue;
    Mp3Header a;
    if (!mp3Header(v.at(p), a)) continue;
    const uint64_t next = p + a.frameBytes;
    Mp3Header b;
    if (next == end || (next + 4 <= end && mp3Header(v.at(next), b) && b.rate == a.rate)) {
      at = p;
      h = a;
      return true;
    }
  }
  return false;
}

Status parseMp3(const View& v, SampleInfo& info, bool extensionMp3) {
  info.container = Container::Mp3;
  const uint64_t start = skipId3(v);
  const uint64_t end = mp3End(v);
  uint64_t first = 0;
  Mp3Header h;
  // With an ID3 tag or a .mp3 extension a sync anywhere in the first 64 KiB counts; else it must be at the start.
  const uint64_t limit = (start > 0 || extensionMp3) ? start + (64u << 10) : start;
  if (!findMp3Sync(v, start, end, limit, first, h)) return unsupported("not a recognised audio file");
  info.sampleRate = h.rate;
  info.channels = h.channels;
  // Xing / Info tag in the first frame (where dr_mp3 looks: after the header, CRC and layer III side info).
  uint64_t tag = first + 4 + (h.crc ? 2 : 0) + h.sideInfo;
  const uint64_t frameEnd = std::min<uint64_t>(first + h.frameBytes, end);
  info.dataOffset = first;  // the first audio frame (moved past a Xing / Info frame below)
  if (h.layer3 && tag + 8 <= frameEnd && (v.tag(tag, "Xing") || v.tag(tag, "Info"))) {
    info.dataOffset = frameEnd;  // the tag frame carries no audio
    const uint32_t flags = v.b[tag + 7];
    uint64_t p = tag + 8;
    int64_t frames = -1;
    bool ok = true;
    if (flags & 1) {
      if (p + 4 > frameEnd) ok = false;
      else frames = v.be32(p), p += 4;
    }
    if (ok && (flags & 2)) { if (p + 4 > frameEnd) ok = false; else p += 4; }
    if (ok && (flags & 4)) { if (p + 100 > frameEnd) ok = false; else p += 100; }
    if (ok && (flags & 8)) { if (p + 4 > frameEnd) ok = false; else p += 4; }
    if (ok && p + 1 <= frameEnd && v.b[p] != 0 && p + 36 <= frameEnd) {
      const uint8_t* q = v.at(p + 21);
      const int delay = static_cast<int>((uint32_t{q[0]} << 4) | (q[1] >> 4)) + 529;
      const int padding = static_cast<int>(((q[1] & 0xFu) << 8) | q[2]) - 529;
      info.delayFrames = static_cast<uint32_t>(delay);
      info.paddingFrames = static_cast<uint32_t>(std::max(padding, 0));
    }
    // A count more frames than the file can hold is not trusted: scan the frame headers instead (below).
    const uint64_t maxFrames = (end > first ? end - first : 0) / kMinMp3FrameBytes + 1;
    if (frames >= 0 && static_cast<uint64_t>(frames) <= maxFrames) {
      const uint64_t total = static_cast<uint64_t>(frames) * h.samples;
      const uint64_t cut = uint64_t{info.delayFrames} + info.paddingFrames;
      info.frames = total > cut ? total - cut : 0;
      return Ok{};
    }
    info.delayFrames = info.paddingFrames = 0;
  }
  // No frame count: scan every frame header once (what dr_mp3 outputs when it has no Xing count).
  uint64_t total = 0;
  uint64_t p = info.dataOffset;  // past a Xing / Info frame (it carries no audio; the decoder starts after it)
  while (p + 4 <= end) {
    Mp3Header f;
    if (!mp3Header(v.at(p), f)) {
      uint64_t q = 0;
      if (!findMp3Sync(v, p + 1, end, end, q, f)) break;
      p = q;
    }
    if (p + f.frameBytes > end) break;
    total += f.samples;
    p += f.frameBytes;
  }
  info.frames = total;
  info.framesScanned = true;
  return Ok{};
}

bool iequalsExt(std::string_view name, std::string_view ext) noexcept {
  if (name.size() < ext.size()) return false;
  const std::string_view tail = name.substr(name.size() - ext.size());
  for (std::size_t i = 0; i < ext.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(tail[i])) != ext[i]) return false;
  return true;
}

Result<SampleInfo> parseAny(Bytes file, std::string_view fileName) {
  const View v{file};
  SampleInfo info;
  Status s = Ok{};
  if (file.size() < 4) return unsupported("file too short");
  if (v.has(0, 12) && (v.tag(0, "RIFF") || v.tag(0, "RF64") || v.tag(0, "BW64")) && v.tag(8, "WAVE")) {
    s = parseWav(v, info, !v.tag(0, "RIFF"));
  } else if (v.has(0, 40) && std::memcmp(file.data(), kW64Riff, 16) == 0 && std::memcmp(file.data() + 24, kW64Wave, 16) == 0) {
    s = parseW64(v, info);
  } else if (v.has(0, 12) && v.tag(0, "FORM") && (v.tag(8, "AIFF") || v.tag(8, "AIFC"))) {
    s = parseAiff(v, info, v.tag(8, "AIFC"));
  } else if (v.tag(0, "OggS")) {
    s = parseOgg(v, info);
  } else {
    const uint64_t afterId3 = skipId3(v);
    if (v.tag(afterId3, "fLaC")) s = parseFlac(v, afterId3, info);
    else s = parseMp3(v, info, iequalsExt(fileName, ".mp3"));
  }
  if (!s) return std::move(s).error();
  const std::size_t slash = fileName.find_last_of("/\\");
  info.fileNameBpm = bpmFromFileName(slash == std::string_view::npos ? fileName : fileName.substr(slash + 1));
  if (info.bpmSource == BpmSource::None && info.fileNameBpm > 0) {
    info.bpm = info.fileNameBpm;
    info.bpmSource = BpmSource::FileName;
  }
  if (info.loopEnd > info.frames) info.loopStart = info.loopEnd = 0;  // a loop past the end is not trusted
  return info;
}

}  // namespace

double bpmFromFileName(std::string_view name) noexcept {
  const std::size_t dot = name.find_last_of('.');
  if (dot != std::string_view::npos && dot > 0) name = name.substr(0, dot);
  auto lower = [&](std::size_t i) { return static_cast<char>(std::tolower(static_cast<unsigned char>(name[i]))); };
  auto isDigit = [&](std::size_t i) { return name[i] >= '0' && name[i] <= '9'; };
  auto isSep = [&](std::size_t i) { return name[i] == ' ' || name[i] == '_' || name[i] == '-'; };
  // Parses digits[.digits] in [b, e); returns the value or 0.
  auto number = [&](std::size_t b, std::size_t e) -> double {
    double v = 0, scale = 0;
    for (std::size_t i = b; i < e; ++i) {
      if (name[i] == '.') {
        if (scale != 0) return 0;
        scale = 1;
      } else {
        v = v * 10 + (name[i] - '0');
        if (scale != 0) scale *= 10;
      }
    }
    if (scale > 1) v /= scale;
    return v >= 20 && v <= 999 ? v : 0;
  };
  for (std::size_t i = 0; i + 3 <= name.size(); ++i) {
    if (lower(i) != 'b' || lower(i + 1) != 'p' || lower(i + 2) != 'm') continue;
    const bool wordEnd = i + 3 == name.size() || !std::isalpha(static_cast<unsigned char>(name[i + 3]));
    // Number before: "120bpm", "120 BPM", "98_bpm".
    if (wordEnd && i > 0) {
      std::size_t e = i;
      if (isSep(e - 1)) --e;
      std::size_t b = e;
      while (b > 0 && (isDigit(b - 1) || name[b - 1] == '.')) --b;
      if (b < e && isDigit(e - 1) && (b == 0 || !isDigit(b - 1))) {
        while (b < e && name[b] == '.') ++b;
        if (const double v = number(b, e); v > 0) return v;
      }
    }
    // Number after: "bpm120", "BPM 98", "bpm-140".
    const bool wordStart = i == 0 || !std::isalpha(static_cast<unsigned char>(name[i - 1]));
    if (wordStart) {
      std::size_t b = i + 3;
      if (b < name.size() && isSep(b)) ++b;
      std::size_t e = b;
      while (e < name.size() && (isDigit(e) || name[e] == '.')) ++e;
      while (e > b && name[e - 1] == '.') --e;
      if (e > b && isDigit(b) && (e == name.size() || !isDigit(e))) {
        if (const double v = number(b, e); v > 0) return v;
      }
    }
  }
  return 0;
}

Result<SampleInfo> probeBytes(std::span<const uint8_t> file, std::string_view fileName) {
  return parseAny(file, fileName);
}

Result<SampleInfo> probe(const std::filesystem::path& path) {
  auto m = detail::MappedFile::open(path);
  if (!m) return std::move(m).error();
  auto info = parseAny(m->bytes(), path.filename().string());
  if (!info) return Error{info.error().code, path.string() + ": " + info.error().message, {}};
  info->identity = m->identity();
  return info;
}

}  // namespace pq::media
