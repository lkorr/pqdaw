#pragma once

// Allocation trap (T0.3). Catches heap allocation and free on threads marked realtime by pq::RtScope.
//
// How it is wired:
// - The replacement global operator new/delete (every replaceable overload: plain, array, nothrow,
//   sized, aligned, and their combinations) live in the CMake OBJECT library `pq_alloc_trap`.
//   `pq_add_test` links it into every test executable in every preset. `pq_add_executable` links it into
//   apps only when the CMake option PQ_ALLOC_TRAP_IN_APPS is ON (default: ON for dev, OFF for release),
//   so release apps use the CRT's operator new/delete untouched: zero overhead.
// - The bookkeeping below (violation counter, handler, expectation scopes) lives in pq_core so code can
//   query it whether or not the operators are linked; installed() says whether they are.
//
// What it cannot catch: direct calls to malloc/free/realloc/_aligned_malloc, HeapAlloc, VirtualAlloc,
// and allocations made inside the CRT or the OS (e.g. by a syscall). The CRT's malloc family is not
// interposable with a static CRT (/MT), only operator new/delete are. Code on the RT path must not call
// those functions at all; reviews and the RT rules in CLAUDE.md cover that gap.
//
// On a violation, the operator still performs the allocation/free (so a test can keep running) and
// calls the handler. Default handler: print the violation and a symbolised stack trace to stderr, then
// std::abort(). Test executables install a handler that fails the current doctest case instead.

#include <cstddef>
#include <cstdint>
#include <utility>

#include "pq/core/rt_scope.h"

namespace pq::alloc_trap {

enum class Op : uint8_t { New, Delete };

struct Violation {
  Op op = Op::New;
  std::size_t size = 0;   // requested bytes for New; size passed to sized delete, or 0 if unknown
  std::size_t align = 0;  // alignment for aligned overloads, 0 for the default alignment
  void* ptr = nullptr;    // freed pointer for Delete, nullptr for New (the call has not returned yet)
  uint32_t threadId = 0;  // OS thread id of the offending thread
};

// Called on the offending thread with the trap suspended (it may allocate, print, or abort).
using Handler = void (*)(const Violation&) noexcept;

// True when this binary links the replacement operators (pq_alloc_trap).
bool installed() noexcept;

// Replaces the handler; nullptr restores the default (print stack trace, abort). Returns the previous one.
Handler setHandler(Handler h) noexcept;

// Total violations reported in this process, including expected ones.
uint64_t violationCount() noexcept;

// Writes a one-line description of v into buf (NUL-terminated, truncated to n bytes).
void describe(const Violation& v, char* buf, std::size_t n) noexcept;

// While alive, violations on the constructing thread are counted here instead of going to the handler.
// This is how tests check that a block did, or did not, allocate. Nestable (the innermost one counts).
class ExpectViolations {
public:
  ExpectViolations() noexcept;
  ~ExpectViolations() noexcept;
  ExpectViolations(const ExpectViolations&) = delete;
  ExpectViolations& operator=(const ExpectViolations&) = delete;

  uint64_t count() const noexcept { return count_; }
  // The most recent violation counted here (meaningful when count() > 0).
  const Violation& last() const noexcept { return last_; }

private:
  friend void reportViolation(const Violation&) noexcept;
  uint64_t count_ = 0;
  Violation last_{};
  ExpectViolations* outer_ = nullptr;
};

// Runs f() inside an RtScope and returns how many allocations/frees it made through operator new/delete.
// Usage in tests:  CHECK(pq::alloc_trap::countRtViolations([&] { ring.tryPush(x); }) == 0);
template <class F>
uint64_t countRtViolations(F&& f) {
  ExpectViolations expect;
  {
    RtScope rt;
    std::forward<F>(f)();
  }
  return expect.count();
}

// Entry point for the replacement operators: records v and dispatches it. Not for general use.
void reportViolation(const Violation& v) noexcept;

// A poisoning deleter for use-after-free tests (T3.17-fix1, M3 review C1). While one is alive (process-wide, every
// thread), every block freed through the replacement operator delete is overwritten with kPoisonByte and kept
// allocated ("quarantined": no later allocation can reuse it), so a read of freed memory deterministically sees
// 0xDDDD… (a non-null pointer, a huge size, a NaN) instead of whatever was there or whatever reused it. Up to
// kQuarantineBlocks blocks are kept (later ones are poisoned and freed); the destructor frees them. Requires
// installed() (test executables); a no-op otherwise. Not nestable.
class PoisonFrees {
public:
  static constexpr unsigned char kPoisonByte = 0xDD;
  static constexpr std::size_t kQuarantineBlocks = 1u << 16;
  PoisonFrees() noexcept;
  ~PoisonFrees() noexcept;
  PoisonFrees(const PoisonFrees&) = delete;
  PoisonFrees& operator=(const PoisonFrees&) = delete;
  // Blocks freed (poisoned) while this scope was alive.
  static uint64_t poisoned() noexcept;
};

namespace detail {
// Called once by pq_alloc_trap's static initialiser.
void markInstalled() noexcept;
// Called by the replacement operator delete for a non-null block before it frees it. True: the block was poisoned
// and quarantined (do not free it). size 0 = unknown (unsized delete: the CRT's block size is used).
bool poisonFree(void* p, std::size_t size, std::size_t align) noexcept;
}  // namespace detail

}  // namespace pq::alloc_trap
