#include "pq/media/wav_file.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>
#include <cstring>
#include <iterator>
#include <limits>

namespace pq::media {

namespace {

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;
// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT: {0000000X-0000-0010-8000-00AA00389B71}; bytes 2..15 are shared.
constexpr uint8_t kSubtypeTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                      0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

Error ioError(const std::filesystem::path& path, const char* what) {
  return Error{ErrorCode::Io, std::string(what) + " " + path.string(), {}};
}
Error formatError(const std::string& what) { return Error{ErrorCode::Parse, "WAV: " + what, {}}; }

uint16_t rd16(const uint8_t* p) noexcept { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) noexcept {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void put16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putTag(std::vector<uint8_t>& b, const char* tag) { b.insert(b.end(), tag, tag + 4); }

// The header up to and including the data chunk's size field. factOffset: where the fact chunk's frame count is (0:
// none — EX.1's integer PCM has no fact chunk). `junk`: a 28-byte JUNK chunk right after "WAVE", the room a ds64 chunk
// takes when the file becomes RF64.
std::vector<uint8_t> header(uint32_t channels, uint32_t sampleRate, uint64_t frames, uint32_t& factOffset, bool junk,
                            WavFormat format) {
  const bool extensible = channels > 2;
  const bool isFloat = format == WavFormat::Float32;
  const uint32_t bytes = isFloat ? 4u : format == WavFormat::Pcm16 ? 2u : 3u;
  const uint32_t dataBytes = static_cast<uint32_t>(frames * channels * bytes);
  std::vector<uint8_t> b;
  putTag(b, "RIFF");
  put32(b, 0);  // patched below
  putTag(b, "WAVE");
  if (junk) {
    putTag(b, "JUNK");
    put32(b, 28);
    b.insert(b.end(), 28, uint8_t{0});
  }
  putTag(b, "fmt ");
  put32(b, extensible ? 40u : isFloat ? 18u : 16u);
  put16(b, extensible ? kFormatExtensible : isFloat ? kFormatFloat : kFormatPcm);
  put16(b, static_cast<uint16_t>(channels));
  put32(b, sampleRate);
  put32(b, sampleRate * channels * bytes);
  put16(b, static_cast<uint16_t>(channels * bytes));
  put16(b, static_cast<uint16_t>(bytes * 8u));
  if (extensible) {
    put16(b, 22);  // cbSize
    put16(b, static_cast<uint16_t>(bytes * 8u));  // valid bits
    put32(b, 0);   // channel mask: unspecified
    put16(b, isFloat ? kFormatFloat : kFormatPcm);
    b.insert(b.end(), std::begin(kSubtypeTail), std::end(kSubtypeTail));
  } else if (isFloat) {
    put16(b, 0);   // cbSize
  }
  factOffset = 0;
  if (isFloat) {
    putTag(b, "fact");
    put32(b, 4);
    factOffset = static_cast<uint32_t>(b.size());
    put32(b, static_cast<uint32_t>(frames));
  }
  putTag(b, "data");
  put32(b, dataBytes);
  const uint32_t riff = static_cast<uint32_t>(b.size() - 8 + dataBytes);
  std::memcpy(b.data() + 4, &riff, 4);  // little endian host (x86-64)
  return b;
}

// EX.1: the nearest integer step of x · scale, clamped to [−scale, scale − 1]; NaN → 0.
int32_t quantize(float x, double scale) noexcept {
  const double v = std::nearbyint(static_cast<double>(x) * scale);
  if (v != v) return 0;
  return static_cast<int32_t>(std::clamp(v, -scale, scale - 1.0));
}

}  // namespace

Result<WavData> parseWav(const uint8_t* bytes, size_t size) {
  const bool rf64 = size >= 12 && std::memcmp(bytes, "RF64", 4) == 0;  // M6 review M8: what a long recording becomes
  if (size < 12 || (std::memcmp(bytes, "RIFF", 4) != 0 && !rf64) || std::memcmp(bytes + 8, "WAVE", 4) != 0)
    return formatError("not a RIFF/WAVE file");
  WavData w;
  uint16_t format = 0, channels = 0, bits = 0, blockAlign = 0;
  bool haveFmt = false;
  const uint8_t* data = nullptr;
  size_t dataBytes = 0;
  uint64_t ds64Data = 0;
  size_t pos = 12;
  while (pos + 8 <= size) {
    const uint8_t* chunk = bytes + pos;
    uint64_t len = rd32(chunk + 4);
    const size_t avail = size - pos - 8;
    if (rf64 && std::memcmp(chunk, "ds64", 4) == 0 && len >= 24 && avail >= 24) {
      ds64Data = static_cast<uint64_t>(rd32(chunk + 16)) | (static_cast<uint64_t>(rd32(chunk + 20)) << 32);
    } else if (rf64 && std::memcmp(chunk, "data", 4) == 0 && len == 0xFFFFFFFFu) {
      len = ds64Data;
    }
    if (std::memcmp(chunk, "fmt ", 4) == 0) {
      if (len < 16 || len > avail) return formatError("truncated fmt chunk");
      format = rd16(chunk + 8);
      channels = rd16(chunk + 10);
      w.sampleRate = rd32(chunk + 12);
      blockAlign = rd16(chunk + 20);
      bits = rd16(chunk + 22);
      if (format == kFormatExtensible) {
        if (len < 40) return formatError("truncated extensible fmt chunk");
        const uint8_t* sub = chunk + 8 + 24;
        if (std::memcmp(sub + 2, kSubtypeTail, sizeof kSubtypeTail) != 0) return formatError("unknown format subtype");
        format = rd16(sub);
      }
      haveFmt = true;
    } else if (std::memcmp(chunk, "data", 4) == 0) {
      data = chunk + 8;
      dataBytes = static_cast<size_t>(std::min<uint64_t>(len, avail));  // a truncated file still yields its whole frames
    }
    if (len > avail) break;
    pos += 8 + static_cast<size_t>(len) + (len & 1u);  // chunks are word aligned
  }
  if (!haveFmt) return formatError("no fmt chunk");
  if (data == nullptr) return formatError("no data chunk");
  if (channels == 0 || channels > kMaxWavChannels) return formatError("unsupported channel count");
  if (w.sampleRate == 0) return formatError("sample rate 0");
  const bool isFloat = format == kFormatFloat;
  if (!(isFloat && bits == 32) && !(format == kFormatPcm && (bits == 16 || bits == 24 || bits == 32)))
    return formatError("unsupported sample format " + std::to_string(format) + "/" + std::to_string(bits) + " bit");
  const uint32_t bytesPer = bits / 8u;
  if (blockAlign != channels * bytesPer) return formatError("inconsistent block align");
  w.isFloat = isFloat;
  w.bitsPerSample = bits;
  const size_t frames = dataBytes / blockAlign;
  w.channels.assign(channels, std::vector<float>(frames));
  for (size_t f = 0; f < frames; ++f) {
    const uint8_t* p = data + f * blockAlign;
    for (uint32_t c = 0; c < channels; ++c, p += bytesPer) {
      float v;
      if (isFloat) {
        std::memcpy(&v, p, 4);
      } else if (bits == 16) {
        v = static_cast<float>(static_cast<int16_t>(rd16(p))) * (1.0f / 32768.0f);
      } else if (bits == 24) {
        const int32_t s = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) |
                                               (static_cast<uint32_t>(p[2]) << 24)) >> 8;
        v = static_cast<float>(s) * (1.0f / 8388608.0f);
      } else {
        v = static_cast<float>(static_cast<double>(static_cast<int32_t>(rd32(p))) * (1.0 / 2147483648.0));
      }
      w.channels[c][f] = v;
    }
  }
  return w;
}

Result<WavData> readWav(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return ioError(path, "cannot read");
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto w = parseWav(bytes.data(), bytes.size());
  if (!w) return Error{w.error().code, path.string() + ": " + w.error().message, {}};
  return w;
}

Status writeWav(const std::filesystem::path& path, const std::vector<std::vector<float>>& channels,
                uint32_t sampleRate) {
  if (channels.empty()) return Error{ErrorCode::InvalidArgument, "writeWav: no channels", {}};
  const size_t frames = channels.front().size();
  std::vector<const float*> ptrs;
  for (const auto& c : channels) {
    if (c.size() != frames) return Error{ErrorCode::InvalidArgument, "writeWav: channel lengths differ", {}};
    ptrs.push_back(c.data());
  }
  WavWriter w;
  if (auto s = w.open(path, static_cast<uint32_t>(channels.size()), sampleRate); !s) return s;
  constexpr size_t kChunk = 1u << 16;
  for (size_t off = 0; off < frames; off += kChunk) {
    const uint32_t n = static_cast<uint32_t>(std::min(kChunk, frames - off));
    std::vector<const float*> at(ptrs);
    for (auto& p : at) p += off;
    if (auto s = w.write(at.data(), n); !s) return s;
  }
  return w.close();
}

WavWriter::~WavWriter() {
  if (out_.is_open()) (void)close();
}

Status WavWriter::open(const std::filesystem::path& path, uint32_t numChannels, uint32_t sampleRate, WavFormat format,
                       uint64_t patchEveryFrames) {
  if (out_.is_open()) return Error{ErrorCode::InvalidArgument, "WavWriter: already open", {}};
  if (numChannels == 0 || numChannels > kMaxWavChannels || sampleRate == 0)
    return Error{ErrorCode::InvalidArgument, "WavWriter: bad format", {}};
  out_.open(path, std::ios::binary | std::ios::trunc);
  if (!out_) return ioError(path, "cannot write");
  path_ = path;
  channels_ = numChannels;
  format_ = format;
  bytesPer_ = format == WavFormat::Float32 ? 4u : format == WavFormat::Pcm16 ? 2u : 3u;
  frames_ = 0;
  patched_ = 0;
  patchEvery_ = patchEveryFrames;
  const std::vector<uint8_t> h = header(numChannels, sampleRate, 0, factOffset_, rf64Above_ != 0, format);
  headerBytes_ = static_cast<uint32_t>(h.size());
  isRf64_ = false;
  out_.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
  if (!out_) return ioError(path, "cannot write");
  return Ok{};
}

Status WavWriter::write(const float* const* channels, uint32_t frames) {
  if (!out_.is_open()) return Error{ErrorCode::InvalidArgument, "WavWriter: not open", {}};
  // RIFF sizes are 32-bit: refuse to grow past them rather than write a corrupt file (unless it may become RF64).
  if (rf64Above_ == 0 && (frames_ + frames) * channels_ * bytesPer_ + headerBytes_ > std::numeric_limits<uint32_t>::max())
    return Error{ErrorCode::Overflow, "WAV file would exceed 4 GiB: " + path_.string(), {}};
  if (format_ != WavFormat::Float32) {  // EX.1: integer PCM, interleaved little endian
    const double scale = format_ == WavFormat::Pcm16 ? 32768.0 : 8388608.0;
    packed_.resize(static_cast<size_t>(frames) * channels_ * bytesPer_);
    uint8_t* o = packed_.data();
    for (uint32_t f = 0; f < frames; ++f)
      for (uint32_t c = 0; c < channels_; ++c) {
        const int32_t q = quantize(channels[c][f], scale);
        *o++ = static_cast<uint8_t>(q);
        *o++ = static_cast<uint8_t>(q >> 8);
        if (bytesPer_ == 3) *o++ = static_cast<uint8_t>(q >> 16);
      }
    out_.write(reinterpret_cast<const char*>(packed_.data()), static_cast<std::streamsize>(packed_.size()));
  } else if (channels_ == 1) {
    out_.write(reinterpret_cast<const char*>(channels[0]), static_cast<std::streamsize>(frames) * 4);
  } else {
    interleaved_.resize(static_cast<size_t>(frames) * channels_);
    for (uint32_t c = 0; c < channels_; ++c) {
      const float* src = channels[c];
      float* dst = interleaved_.data() + c;
      for (uint32_t f = 0; f < frames; ++f) dst[static_cast<size_t>(f) * channels_] = src[f];
    }
    out_.write(reinterpret_cast<const char*>(interleaved_.data()), static_cast<std::streamsize>(interleaved_.size()) * 4);
  }
  if (!out_) return ioError(path_, "cannot write");
  frames_ += frames;
  if (patchEvery_ != 0 && frames_ - patched_ >= patchEvery_) return patchHeader();
  return Ok{};
}

// The RIFF / fact / data sizes of what was written; past rf64Above_ the RF64 form (EBU Tech 3306): "RF64", the 32-bit
// sizes 0xFFFFFFFF and the real ones in the ds64 chunk that replaces the JUNK chunk (once RF64, always RF64).
void WavWriter::writeSizes() {
  const uint64_t dataBytes = frames_ * channels_ * bytesPer_;
  const uint64_t riff = headerBytes_ - 8 + dataBytes;
  if (rf64Above_ != 0 && (isRf64_ || riff > rf64Above_)) {
    isRf64_ = true;
    const uint32_t ff = 0xFFFFFFFFu, ds64Size = 28, table = 0;
    out_.seekp(0);
    out_.write("RF64", 4);
    out_.write(reinterpret_cast<const char*>(&ff), 4);
    out_.seekp(12);
    out_.write("ds64", 4);
    out_.write(reinterpret_cast<const char*>(&ds64Size), 4);
    out_.write(reinterpret_cast<const char*>(&riff), 8);
    out_.write(reinterpret_cast<const char*>(&dataBytes), 8);
    out_.write(reinterpret_cast<const char*>(&frames_), 8);
    out_.write(reinterpret_cast<const char*>(&table), 4);
    if (factOffset_ != 0) {
      out_.seekp(factOffset_);
      out_.write(reinterpret_cast<const char*>(&ff), 4);
    }
    out_.seekp(headerBytes_ - 4);
    out_.write(reinterpret_cast<const char*>(&ff), 4);
    return;
  }
  const auto riff32 = static_cast<uint32_t>(riff), data32 = static_cast<uint32_t>(dataBytes);
  const auto frames32 = static_cast<uint32_t>(frames_);
  out_.seekp(4);
  out_.write(reinterpret_cast<const char*>(&riff32), 4);
  if (factOffset_ != 0) {
    out_.seekp(factOffset_);
    out_.write(reinterpret_cast<const char*>(&frames32), 4);
  }
  out_.seekp(headerBytes_ - 4);
  out_.write(reinterpret_cast<const char*>(&data32), 4);
}

Status WavWriter::patchHeader() {
  if (!out_.is_open()) return Error{ErrorCode::InvalidArgument, "WavWriter: not open", {}};
  // The data first (a reader that sees the new sizes finds the frames they cover), then the sizes, then back to the end.
  out_.flush();
  writeSizes();
  out_.seekp(0, std::ios::end);
  out_.flush();
  if (!out_) return ioError(path_, "cannot write");
  patched_ = frames_;
  return Ok{};
}

Status WavWriter::close() {
  if (!out_.is_open()) return Ok{};
  writeSizes();
  const bool ok = static_cast<bool>(out_);
  patched_ = frames_;
  out_.close();
  if (!ok || out_.fail()) return ioError(path_, "cannot write");
  return Ok{};
}

// T5.12: 24-bit integer PCM through the streaming writer (EX.1): rounded, clamped, no dither.
Status writeWavPcm24(const std::filesystem::path& path, const std::vector<std::vector<float>>& channels,
                     uint32_t sampleRate) {
  if (channels.empty() || channels.size() > kMaxWavChannels || sampleRate == 0)
    return Error{ErrorCode::InvalidArgument, "writeWavPcm24: bad format", {}};
  const size_t frames = channels.front().size();
  for (const auto& c : channels)
    if (c.size() != frames) return Error{ErrorCode::InvalidArgument, "writeWavPcm24: channel lengths differ", {}};
  WavWriter w;
  if (auto s = w.open(path, static_cast<uint32_t>(channels.size()), sampleRate, WavFormat::Pcm24); !s) return s;
  constexpr size_t kChunk = 1u << 15;
  std::vector<const float*> at(channels.size());
  for (size_t off = 0; off < frames; off += kChunk) {
    for (size_t c = 0; c < channels.size(); ++c) at[c] = channels[c].data() + off;
    if (auto s = w.write(at.data(), static_cast<uint32_t>(std::min(kChunk, frames - off))); !s) return s;
  }
  return w.close();
}

}  // namespace pq::media
