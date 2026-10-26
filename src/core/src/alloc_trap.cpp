#include "pq/core/alloc_trap.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>

namespace pq {
namespace detail {
thread_local uint32_t tRtDepth = 0;
}  // namespace detail

namespace alloc_trap {
namespace {

std::atomic<bool> gInstalled{false};
std::atomic<uint64_t> gViolations{0};
std::atomic<Handler> gHandler{nullptr};
// Innermost ExpectViolations on this thread (constant-initialised: safe inside operator new).
thread_local ExpectViolations* tExpect = nullptr;

// Prints the calling thread's stack. Runs only on the way to abort(), so it may allocate and lock.
void printStackTrace() noexcept {
  void* frames[62];
  const USHORT n = CaptureStackBackTrace(2, 62, frames, nullptr);
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  const bool haveSyms = SymInitialize(process, nullptr, TRUE) != FALSE;

  alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256];
  std::fprintf(stderr, "stack trace (%u frames):\n", static_cast<unsigned>(n));
  for (USHORT i = 0; i < n; ++i) {
    const auto addr = reinterpret_cast<DWORD64>(frames[i]);
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 symDisp = 0;
    if (haveSyms && SymFromAddr(process, addr, &symDisp, sym)) {
      IMAGEHLP_LINE64 line{};
      line.SizeOfStruct = sizeof(line);
      DWORD lineDisp = 0;
      if (SymGetLineFromAddr64(process, addr, &lineDisp, &line))
        std::fprintf(stderr, "  #%-2u 0x%016llx %s+0x%llx  %s:%lu\n", static_cast<unsigned>(i),
                     static_cast<unsigned long long>(addr), sym->Name,
                     static_cast<unsigned long long>(symDisp), line.FileName, line.LineNumber);
      else
        std::fprintf(stderr, "  #%-2u 0x%016llx %s+0x%llx\n", static_cast<unsigned>(i),
                     static_cast<unsigned long long>(addr), sym->Name,
                     static_cast<unsigned long long>(symDisp));
    } else {
      std::fprintf(stderr, "  #%-2u 0x%016llx\n", static_cast<unsigned>(i),
                   static_cast<unsigned long long>(addr));
    }
  }
  if (haveSyms) SymCleanup(process);
}

void defaultHandler(const Violation& v) noexcept {
  char msg[256];
  describe(v, msg, sizeof msg);
  std::fprintf(stderr, "pqdaw: FATAL: %s\n", msg);
  printStackTrace();
  std::fflush(stderr);
  std::abort();
}

}  // namespace

bool installed() noexcept { return gInstalled.load(std::memory_order_acquire); }

Handler setHandler(Handler h) noexcept { return gHandler.exchange(h, std::memory_order_acq_rel); }

uint64_t violationCount() noexcept { return gViolations.load(std::memory_order_relaxed); }

void describe(const Violation& v, char* buf, std::size_t n) noexcept {
  if (n == 0) return;
  if (v.op == Op::New)
    std::snprintf(buf, n, "heap allocation of %zu bytes (align %zu) inside RtScope on thread %u", v.size,
                  v.align, v.threadId);
  else
    std::snprintf(buf, n, "heap free of %p (size %zu, align %zu) inside RtScope on thread %u", v.ptr, v.size,
                  v.align, v.threadId);
}

ExpectViolations::ExpectViolations() noexcept : outer_(tExpect) { tExpect = this; }

ExpectViolations::~ExpectViolations() noexcept { tExpect = outer_; }

void reportViolation(const Violation& v) noexcept {
  // Suspend the trap so the handler (doctest, stderr, DbgHelp) may allocate; restore afterwards.
  const uint32_t depth = pq::detail::tRtDepth;
  pq::detail::tRtDepth = 0;
  gViolations.fetch_add(1, std::memory_order_relaxed);
  if (ExpectViolations* e = tExpect) {
    ++e->count_;
    e->last_ = v;
  } else {
    const Handler h = gHandler.load(std::memory_order_acquire);
    (h ? h : &defaultHandler)(v);
  }
  pq::detail::tRtDepth = depth;
}

// ---- PoisonFrees -------------------------------------------------------------------------------------------------
namespace {
struct Quarantined {
  void* p;
  std::size_t align;
};
std::atomic<bool> gPoison{false};
std::atomic<uint64_t> gPoisoned{0};
std::atomic<std::size_t> gQuarantineCount{0};
// Static storage: the quarantine itself must not allocate (it runs inside operator delete).
Quarantined gQuarantine[PoisonFrees::kQuarantineBlocks];
}  // namespace

PoisonFrees::PoisonFrees() noexcept {
  gPoisoned.store(0, std::memory_order_relaxed);
  gQuarantineCount.store(0, std::memory_order_relaxed);
  gPoison.store(true, std::memory_order_release);
}

PoisonFrees::~PoisonFrees() noexcept {
  gPoison.store(false, std::memory_order_release);
  const std::size_t count = gQuarantineCount.load(std::memory_order_acquire);
  const std::size_t n = count < kQuarantineBlocks ? count : kQuarantineBlocks;
  for (std::size_t i = 0; i < n; ++i) {
    if (gQuarantine[i].align == 0) std::free(gQuarantine[i].p);
    else _aligned_free(gQuarantine[i].p);
  }
  gQuarantineCount.store(0, std::memory_order_relaxed);
}

uint64_t PoisonFrees::poisoned() noexcept { return gPoisoned.load(std::memory_order_relaxed); }

namespace detail {
void markInstalled() noexcept { gInstalled.store(true, std::memory_order_release); }

bool poisonFree(void* p, std::size_t size, std::size_t align) noexcept {
  if (!gPoison.load(std::memory_order_acquire)) return false;
  const std::size_t bytes = size != 0 ? size : align == 0 ? _msize(p) : _aligned_msize(p, align, 0);
  std::memset(p, PoisonFrees::kPoisonByte, bytes);
  gPoisoned.fetch_add(1, std::memory_order_relaxed);
  const std::size_t slot = gQuarantineCount.fetch_add(1, std::memory_order_acq_rel);
  if (slot >= PoisonFrees::kQuarantineBlocks) return false;  // full: poisoned, then freed by the caller
  gQuarantine[slot] = Quarantined{p, align};
  return true;
}
}  // namespace detail

}  // namespace alloc_trap
}  // namespace pq
