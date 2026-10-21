// Replacement global operator new/delete for the allocation trap (pq/core/alloc_trap.h).
// Built as the OBJECT library pq_alloc_trap so the definitions always reach the linker ahead of the CRT's.
// Every replaceable overload is defined here, so no CRT allocation operator is mixed with ours.
// Placement new/delete are not replaceable and do not allocate.

#include <cstdlib>
#include <new>

#include <windows.h>

#include "pq/core/alloc_trap.h"

namespace {

using pq::alloc_trap::Op;

const bool kRegistered = (pq::alloc_trap::detail::markInstalled(), true);

inline void check(Op op, std::size_t size, std::size_t align, void* ptr) noexcept {
  if (pq::detail::tRtDepth != 0) [[unlikely]] {
    if (op == Op::Delete && ptr == nullptr) return;  // deleting null is a no-op, not a free
    pq::alloc_trap::reportViolation({op, size, align, ptr, static_cast<uint32_t>(GetCurrentThreadId())});
  }
}

// Default alignment (16 on x64) is malloc's; over-aligned requests use _aligned_malloc and must be
// freed with _aligned_free, which the aligned delete overloads below do.
inline void* rawAlloc(std::size_t size, std::size_t align) noexcept {
  if (size == 0) size = 1;
  return align == 0 ? std::malloc(size) : _aligned_malloc(size, align);
}

inline void rawFree(void* p, std::size_t align) noexcept {
  if (align == 0) std::free(p);
  else _aligned_free(p);
}

// [new.delete.single]: loop calling the new_handler until it succeeds or there is none, then throw.
void* allocOrThrow(std::size_t size, std::size_t align) {
  check(Op::New, size, align, nullptr);
  for (;;) {
    if (void* p = rawAlloc(size, align)) return p;
    std::new_handler h = std::get_new_handler();
    if (!h) throw std::bad_alloc();
    h();
  }
}

void* allocNoThrow(std::size_t size, std::size_t align) noexcept {
  try {
    return allocOrThrow(size, align);
  } catch (...) {
    return nullptr;
  }
}

inline void freeChecked(void* p, std::size_t size, std::size_t align) noexcept {
  check(Op::Delete, size, align, p);
  if (p) rawFree(p, align);
}

inline std::size_t al(std::align_val_t a) noexcept { return static_cast<std::size_t>(a); }

}  // namespace

// clang-format off
void* operator new(std::size_t n) { return allocOrThrow(n, 0); }
void* operator new[](std::size_t n) { return allocOrThrow(n, 0); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return allocNoThrow(n, 0); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return allocNoThrow(n, 0); }
void* operator new(std::size_t n, std::align_val_t a) { return allocOrThrow(n, al(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocOrThrow(n, al(a)); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocNoThrow(n, al(a)); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocNoThrow(n, al(a)); }

void operator delete(void* p) noexcept { freeChecked(p, 0, 0); }
void operator delete[](void* p) noexcept { freeChecked(p, 0, 0); }
void operator delete(void* p, std::size_t n) noexcept { freeChecked(p, n, 0); }
void operator delete[](void* p, std::size_t n) noexcept { freeChecked(p, n, 0); }
void operator delete(void* p, const std::nothrow_t&) noexcept { freeChecked(p, 0, 0); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { freeChecked(p, 0, 0); }
void operator delete(void* p, std::align_val_t a) noexcept { freeChecked(p, 0, al(a)); }
void operator delete[](void* p, std::align_val_t a) noexcept { freeChecked(p, 0, al(a)); }
void operator delete(void* p, std::size_t n, std::align_val_t a) noexcept { freeChecked(p, n, al(a)); }
void operator delete[](void* p, std::size_t n, std::align_val_t a) noexcept { freeChecked(p, n, al(a)); }
void operator delete(void* p, std::align_val_t a, const std::nothrow_t&) noexcept { freeChecked(p, 0, al(a)); }
void operator delete[](void* p, std::align_val_t a, const std::nothrow_t&) noexcept { freeChecked(p, 0, al(a)); }
// clang-format on
