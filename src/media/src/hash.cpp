#include "pq/media/hash.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

#include "file_io.h"

namespace pq::media {

namespace {

// XXH64's construction (four multiply-rotate lanes over 32-byte stripes, a merge, an avalanche), streaming.
constexpr uint64_t P1 = 0x9E3779B185EBCA87ull;
constexpr uint64_t P2 = 0xC2B2AE3D27D4EB4Full;
constexpr uint64_t P3 = 0x165667B19E3779F9ull;
constexpr uint64_t P4 = 0x85EBCA77C2B2AE63ull;
constexpr uint64_t P5 = 0x27D4EB2F165667C5ull;

inline uint64_t rd64(const uint8_t* p) noexcept {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
inline uint32_t rd32(const uint8_t* p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
inline uint64_t round(uint64_t acc, uint64_t in) noexcept { return std::rotl(acc + in * P2, 31) * P1; }
inline uint64_t merge(uint64_t acc, uint64_t v) noexcept { return (acc ^ round(0, v)) * P1 + P4; }

class Hasher {
public:
  explicit Hasher(uint64_t seed) noexcept : v_{seed + P1 + P2, seed + P2, seed, seed - P1}, seed_(seed) {}

  void update(const uint8_t* p, std::size_t n) noexcept {
    total_ += n;
    if (fill_ + n < 32) {
      std::memcpy(buf_ + fill_, p, n);
      fill_ += static_cast<uint32_t>(n);
      return;
    }
    if (fill_ != 0) {
      const std::size_t take = 32 - fill_;
      std::memcpy(buf_ + fill_, p, take);
      stripe(buf_);
      p += take;
      n -= take;
      fill_ = 0;
    }
    const uint8_t* end = p + (n & ~std::size_t{31});
    for (; p < end; p += 32) stripe(p);
    fill_ = static_cast<uint32_t>(n & 31);
    std::memcpy(buf_, p, fill_);
  }

  uint64_t digest() const noexcept {
    uint64_t h;
    if (total_ >= 32) {
      h = std::rotl(v_[0], 1) + std::rotl(v_[1], 7) + std::rotl(v_[2], 12) + std::rotl(v_[3], 18);
      for (uint64_t v : v_) h = merge(h, v);
    } else {
      h = seed_ + P5;
    }
    h += total_;
    const uint8_t* p = buf_;
    const uint8_t* end = buf_ + fill_;
    for (; p + 8 <= end; p += 8) h = std::rotl(h ^ round(0, rd64(p)), 27) * P1 + P4;
    if (p + 4 <= end) {
      h = std::rotl(h ^ (uint64_t{rd32(p)} * P1), 23) * P2 + P3;
      p += 4;
    }
    for (; p < end; ++p) h = std::rotl(h ^ (uint64_t{*p} * P5), 11) * P1;
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
  }

private:
  void stripe(const uint8_t* p) noexcept {
    v_[0] = round(v_[0], rd64(p));
    v_[1] = round(v_[1], rd64(p + 8));
    v_[2] = round(v_[2], rd64(p + 16));
    v_[3] = round(v_[3], rd64(p + 24));
  }

  uint64_t v_[4];
  uint64_t seed_;
  uint64_t total_ = 0;
  uint8_t buf_[32]{};
  uint32_t fill_ = 0;
};

constexpr uint64_t kSeedHi = 0x5851F42D4C957F2Dull;

}  // namespace

uint64_t hashBytes(const void* data, std::size_t size, uint64_t seed) noexcept {
  Hasher h(seed);
  h.update(static_cast<const uint8_t*>(data), size);
  return h.digest();
}

uint64_t contentHashBytes(std::span<const uint8_t> file) noexcept {
  const uint64_t size = file.size();
  constexpr std::size_t W = kContentHashWindow;
  if (size <= 3 * W) return hashBytes(file.data(), file.size(), size);
  uint64_t h = hashBytes(file.data(), W, size);
  h = hashBytes(file.data() + (size - W) / 2, W, h);
  return hashBytes(file.data() + size - W, W, h);
}

Result<uint64_t> contentHash(const std::filesystem::path& path) {
  auto f = detail::File::open(path, detail::File::Access::Sequential);
  if (!f) return std::move(f).error();
  const uint64_t size = f->size();
  constexpr std::size_t W = kContentHashWindow;
  std::vector<uint8_t> buf(static_cast<std::size_t>(std::min<uint64_t>(size, 3 * W)));
  if (size <= 3 * W) {
    if (auto s = f->readExact(0, buf.data(), buf.size()); !s) return std::move(s).error();
    return hashBytes(buf.data(), buf.size(), size);
  }
  if (auto s = f->readExact(0, buf.data(), W); !s) return std::move(s).error();
  if (auto s = f->readExact((size - W) / 2, buf.data() + W, W); !s) return std::move(s).error();
  if (auto s = f->readExact(size - W, buf.data() + 2 * W, W); !s) return std::move(s).error();
  uint64_t h = hashBytes(buf.data(), W, size);
  h = hashBytes(buf.data() + W, W, h);
  return hashBytes(buf.data() + 2 * W, W, h);
}

Hash128 fullContentHashBytes(std::span<const uint8_t> file) noexcept {
  return {hashBytes(file.data(), file.size(), file.size()), hashBytes(file.data(), file.size(), file.size() ^ kSeedHi)};
}

Result<Hash128> fullContentHash(const std::filesystem::path& path) {
  auto f = detail::File::open(path, detail::File::Access::Sequential);
  if (!f) return std::move(f).error();
  const uint64_t size = f->size();
  Hasher lo(size), hi(size ^ kSeedHi);
  std::vector<uint8_t> buf(1u << 20);
  for (uint64_t off = 0; off < size;) {
    const std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(buf.size(), size - off));
    if (auto s = f->readExact(off, buf.data(), n); !s) return std::move(s).error();
    lo.update(buf.data(), n);
    hi.update(buf.data(), n);
    off += n;
  }
  return Hash128{lo.digest(), hi.digest()};
}

}  // namespace pq::media
