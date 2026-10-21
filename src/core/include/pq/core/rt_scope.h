#pragma once

#include <cstdint>

namespace pq {

namespace detail {
// Nesting depth of RtScope on this thread. Constant-initialised (no TLS init guard), so the allocation
// trap can read it from inside operator new on any thread, including during thread start-up.
extern thread_local uint32_t tRtDepth;
}  // namespace detail

// Marks the current thread as "realtime" for its lifetime (ARCHITECTURE §3). Nestable.
// The audio callback opens one per block; tests wrap RT entry points in one. In binaries that link the
// allocation trap (every test executable and dev builds of apps, see alloc_trap.h), any heap allocation
// or free through operator new/delete while a scope is active is reported as a violation.
// Cost: one thread-local increment and decrement; there is nothing else to compile out.
struct RtScope {
  RtScope() noexcept { ++detail::tRtDepth; }   // RT-safe
  ~RtScope() noexcept { --detail::tRtDepth; }  // RT-safe
  RtScope(const RtScope&) = delete;
  RtScope& operator=(const RtScope&) = delete;
};

// True while the calling thread is inside an RtScope. RT-safe.
inline bool inRtScope() noexcept { return detail::tRtDepth != 0; }

}  // namespace pq
