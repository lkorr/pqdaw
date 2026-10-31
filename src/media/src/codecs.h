#pragma once

// The third-party decoders' declarations (compiled in src/media/third_party/*.c) plus a byte-capped heap for them.

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "codec_config.h"
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

namespace pq::media::detail {

// malloc with a running byte total and a hard limit: a malicious header cannot make a decoder allocate more than
// `limit` bytes in all (it fails to open or to decode instead). Not thread-safe: one per decoder.
class CappedHeap {
public:
  explicit CappedHeap(std::size_t limit) noexcept : limit_(limit) {}

  void* alloc(std::size_t n) noexcept {
    if (n > limit_ - used_) return nullptr;
    auto* p = static_cast<std::size_t*>(std::malloc(n + kHeader));
    if (!p) return nullptr;
    p[0] = n;
    used_ += n;
    return reinterpret_cast<uint8_t*>(p) + kHeader;
  }
  void* realloc(void* q, std::size_t n) noexcept {
    if (!q) return alloc(n);
    auto* p = reinterpret_cast<std::size_t*>(static_cast<uint8_t*>(q) - kHeader);
    const std::size_t old = p[0];
    if (n > old && n - old > limit_ - used_) return nullptr;
    auto* r = static_cast<std::size_t*>(std::realloc(p, n + kHeader));
    if (!r) return nullptr;
    r[0] = n;
    used_ = used_ - old + n;
    return reinterpret_cast<uint8_t*>(r) + kHeader;
  }
  void free(void* q) noexcept {
    if (!q) return;
    auto* p = reinterpret_cast<std::size_t*>(static_cast<uint8_t*>(q) - kHeader);
    used_ -= p[0];
    std::free(p);
  }
  std::size_t used() const noexcept { return used_; }

  template <class Callbacks>
  Callbacks callbacks() noexcept {
    Callbacks c{};
    c.pUserData = this;
    c.onMalloc = [](std::size_t n, void* u) -> void* { return static_cast<CappedHeap*>(u)->alloc(n); };
    c.onRealloc = [](void* p, std::size_t n, void* u) -> void* { return static_cast<CappedHeap*>(u)->realloc(p, n); };
    c.onFree = [](void* p, void* u) { static_cast<CappedHeap*>(u)->free(p); };
    return c;
  }

private:
  static constexpr std::size_t kHeader = 16;  // keeps the 16-byte malloc alignment
  std::size_t limit_;
  std::size_t used_ = 0;
};

}  // namespace pq::media::detail
