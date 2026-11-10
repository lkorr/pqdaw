#include "pq/core/rt_heap.h"

#include <cstdint>
#include <new>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "pq/core/alloc_trap.h"

namespace pq {

namespace {

// HeapAlloc returns 16-byte aligned blocks (x64); a larger alignment over-allocates and keeps the block's start in the
// 8 bytes before the aligned pointer.
constexpr std::size_t kHeapAlign = 16;

struct Heap {
  HANDLE h = HeapCreate(0, 0, 0);
  SRWLOCK lock = SRWLOCK_INIT;
};
Heap& heap() {
  static Heap* h = new Heap();  // never destroyed: images may be freed during static destruction
  return *h;
}

void trap(alloc_trap::Op op, std::size_t bytes, std::size_t align, void* p) noexcept {
  if (inRtScope()) [[unlikely]]
    alloc_trap::reportViolation({op, bytes, align, p, static_cast<uint32_t>(GetCurrentThreadId())});
}

class Resource final : public std::pmr::memory_resource {
  void* do_allocate(std::size_t bytes, std::size_t align) override { return rtHeapAllocate(bytes, align); }
  void do_deallocate(void* p, std::size_t, std::size_t align) override { rtHeapFree(p, align); }
  bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override { return this == &o; }
};

}  // namespace

std::pmr::memory_resource* rtHeap() noexcept {
  static Resource* r = new Resource();  // never destroyed (see heap())
  return r;
}

void* rtHeapAllocate(std::size_t bytes, std::size_t align) {
  trap(alloc_trap::Op::New, bytes, align, nullptr);
  Heap& H = heap();
  const std::size_t extra = align > kHeapAlign ? align : 0;
  AcquireSRWLockExclusive(&H.lock);
  void* raw = H.h != nullptr ? HeapAlloc(H.h, 0, bytes + extra) : nullptr;
  ReleaseSRWLockExclusive(&H.lock);
  if (raw == nullptr) throw std::bad_alloc();
  if (extra == 0) return raw;
  // (raw is 16-aligned: the aligned pointer is 16 … align bytes in, room for the start before it.)
  const auto u = (reinterpret_cast<uintptr_t>(raw) + align) & ~static_cast<uintptr_t>(align - 1);
  reinterpret_cast<void**>(u)[-1] = raw;
  return reinterpret_cast<void*>(u);
}

void rtHeapFree(void* p, std::size_t align) noexcept {
  if (p == nullptr) return;
  trap(alloc_trap::Op::Delete, 0, align, p);
  if (align > kHeapAlign) p = reinterpret_cast<void**>(p)[-1];
  Heap& H = heap();
  AcquireSRWLockExclusive(&H.lock);
  HeapFree(H.h, 0, p);
  ReleaseSRWLockExclusive(&H.lock);
}

}  // namespace pq
