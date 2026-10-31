#include "pq/media/hash.h"

#include <algorithm>
#include <vector>

#include "file_io.h"

namespace pq::media {

namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

inline uint64_t fnv(uint64_t h, const uint8_t* b, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * kFnvPrime;
  return h;
}

// Window offsets of doc::fileIdentity: [0] always; [1], [2] only when the file is longer than one window.
struct Windows {
  uint64_t offset[3];
  std::size_t count;
  std::size_t length;
};
Windows windowsOf(uint64_t size) noexcept {
  constexpr uint64_t W = kContentHashWindow;
  if (size <= W) return {{0, 0, 0}, 1, static_cast<std::size_t>(size)};
  return {{0, (size - W) / 2, size - W}, 3, static_cast<std::size_t>(W)};
}

uint64_t seed(uint64_t size) noexcept {
  uint8_t le[8];
  for (int i = 0; i < 8; ++i) le[i] = static_cast<uint8_t>(size >> (8 * i));
  return fnv(kFnvOffset, le, 8);
}

}  // namespace

uint64_t contentHashBytes(std::span<const uint8_t> file) noexcept {
  const Windows w = windowsOf(file.size());
  uint64_t h = seed(file.size());
  for (std::size_t i = 0; i < w.count; ++i) h = fnv(h, file.data() + w.offset[i], w.length);
  return h;
}

Result<uint64_t> contentHash(const std::filesystem::path& path) {
  auto f = detail::File::open(path, detail::File::Access::Random);
  if (!f) return std::move(f).error();
  const Windows w = windowsOf(f->size());
  std::vector<uint8_t> buf(w.length);
  uint64_t h = seed(f->size());
  for (std::size_t i = 0; i < w.count; ++i) {
    if (auto s = f->readExact(w.offset[i], buf.data(), buf.size()); !s) return std::move(s).error();
    h = fnv(h, buf.data(), buf.size());
  }
  return h;
}

}  // namespace pq::media
