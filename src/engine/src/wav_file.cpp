#include "pq/engine/wav_file.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>

namespace pq::engine {

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

// The header up to and including the data chunk's size field. factOffset: where the fact chunk's frame count is.
std::vector<uint8_t> header(uint32_t channels, uint32_t sampleRate, uint64_t frames, uint32_t& factOffset) {
  const bool extensible = channels > 2;
  const uint32_t dataBytes = static_cast<uint32_t>(frames * channels * 4u);
  std::vector<uint8_t> b;
  putTag(b, "RIFF");
  put32(b, 0);  // patched below
  putTag(b, "WAVE");
  putTag(b, "fmt ");
  put32(b, extensible ? 40u : 18u);
  put16(b, extensible ? kFormatExtensible : kFormatFloat);
  put16(b, static_cast<uint16_t>(channels));
  put32(b, sampleRate);
  put32(b, sampleRate * channels * 4u);
  put16(b, static_cast<uint16_t>(channels * 4u));
  put16(b, 32);
  if (extensible) {
    put16(b, 22);  // cbSize
    put16(b, 32);  // valid bits
    put32(b, 0);   // channel mask: unspecified
    put16(b, kFormatFloat);
    b.insert(b.end(), std::begin(kSubtypeTail), std::end(kSubtypeTail));
  } else {
    put16(b, 0);   // cbSize
  }
  putTag(b, "fact");
  put32(b, 4);
  factOffset = static_cast<uint32_t>(b.size());
  put32(b, static_cast<uint32_t>(frames));
  putTag(b, "data");
  put32(b, dataBytes);
  const uint32_t riff = static_cast<uint32_t>(b.size() - 8 + dataBytes);
  std::memcpy(b.data() + 4, &riff, 4);  // little endian host (x86-64)
  return b;
}

}  // namespace

Result<WavData> parseWav(const uint8_t* bytes, size_t size) {
  if (size < 12 || std::memcmp(bytes, "RIFF", 4) != 0 || std::memcmp(bytes + 8, "WAVE", 4) != 0)
    return formatError("not a RIFF/WAVE file");
  WavData w;
  uint16_t format = 0, channels = 0, bits = 0, blockAlign = 0;
  bool haveFmt = false;
  const uint8_t* data = nullptr;
  size_t dataBytes = 0;
  size_t pos = 12;
  while (pos + 8 <= size) {
    const uint8_t* chunk = bytes + pos;
    const uint32_t len = rd32(chunk + 4);
    const size_t avail = size - pos - 8;
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
      dataBytes = std::min<size_t>(len, avail);  // a truncated file still yields its whole frames
    }
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

Status WavWriter::open(const std::filesystem::path& path, uint32_t numChannels, uint32_t sampleRate) {
  if (out_.is_open()) return Error{ErrorCode::InvalidArgument, "WavWriter: already open", {}};
  if (numChannels == 0 || numChannels > kMaxWavChannels || sampleRate == 0)
    return Error{ErrorCode::InvalidArgument, "WavWriter: bad format", {}};
  out_.open(path, std::ios::binary | std::ios::trunc);
  if (!out_) return ioError(path, "cannot write");
  path_ = path;
  channels_ = numChannels;
  frames_ = 0;
  const std::vector<uint8_t> h = header(numChannels, sampleRate, 0, factOffset_);
  headerBytes_ = static_cast<uint32_t>(h.size());
  out_.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
  if (!out_) return ioError(path, "cannot write");
  return Ok{};
}

Status WavWriter::write(const float* const* channels, uint32_t frames) {
  if (!out_.is_open()) return Error{ErrorCode::InvalidArgument, "WavWriter: not open", {}};
  // RIFF sizes are 32-bit: refuse to grow past them rather than write a corrupt file.
  if ((frames_ + frames) * channels_ * 4u + headerBytes_ > std::numeric_limits<uint32_t>::max())
    return Error{ErrorCode::Overflow, "WAV file would exceed 4 GiB: " + path_.string(), {}};
  if (channels_ == 1) {
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
  return Ok{};
}

Status WavWriter::close() {
  if (!out_.is_open()) return Ok{};
  const uint32_t dataBytes = static_cast<uint32_t>(frames_ * channels_ * 4u);
  const uint32_t riff = headerBytes_ - 8 + dataBytes;
  const uint32_t frames32 = static_cast<uint32_t>(frames_);
  out_.seekp(4);
  out_.write(reinterpret_cast<const char*>(&riff), 4);
  out_.seekp(factOffset_);
  out_.write(reinterpret_cast<const char*>(&frames32), 4);
  out_.seekp(headerBytes_ - 4);
  out_.write(reinterpret_cast<const char*>(&dataBytes), 4);
  const bool ok = static_cast<bool>(out_);
  out_.close();
  if (!ok || out_.fail()) return ioError(path_, "cannot write");
  return Ok{};
}

}  // namespace pq::engine
