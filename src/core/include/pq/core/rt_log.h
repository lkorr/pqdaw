#pragma once

// Realtime log ring (T0.2-fix2, ARCHITECTURE §1 "RT log ring"). A realtime thread pushes fixed-size POD records
// (a static format string, a numeric code, up to four numeric arguments, a TSC timestamp) into an SPSC ring; a
// logger thread drains the ring, formats the records and writes them out. Pushing is a handful of stores and one
// release store: no allocation, no locks, no syscalls, no formatting on the realtime thread.
//
//   RtLog log(4096);                                   // off the audio thread (allocates)
//   log.log(kUnderrun, "underrun at wake {} ({} frames late)", wake, lateness);   // audio thread, RT-safe
//   RtLogger logger(&writeToStderr, nullptr);         // non-RT: drains every 10 ms on its own thread
//   logger.add(log); logger.start();
//
// One producer per RtLog (one per realtime thread) and one consumer (the logger, or a direct drain() caller).
// When the ring is full the record is dropped and counted: dropped() is exact, and the logger writes a
// "dropped N records" line the next time it drains that ring, so a gap in the log is always visible.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <intrin.h>

#include "pq/core/cache_line.h"
#include "pq/core/spsc_ring.h"

namespace pq {

struct RtLogRecord {
  enum class Kind : uint8_t { None, Int, UInt, Float, Bool };
  union Arg {
    int64_t i;
    uint64_t u;
    double d;
  };
  static constexpr uint32_t kMaxArgs = 4;

  // Static-storage text with "{}" placeholders, filled in order with the arguments ("{{" / "}}" print braces).
  // Never a pointer to a temporary: the logger reads it later on another thread.
  const char* format = nullptr;
  uint64_t tsc = 0;          // __rdtsc() at push (0 if not stamped)
  uint64_t sequence = 0;     // per-RtLog push sequence (counts dropped records too), set by RtLog
  uint32_t code = 0;         // caller-defined numeric code (for filtering and tests)
  uint8_t argc = 0;
  Kind kinds[kMaxArgs] = {};
  Arg args[kMaxArgs] = {};
};
static_assert(std::is_trivially_copyable_v<RtLogRecord>, "RtLogRecord must be POD (copied through the ring)");

// Formats a record into text (no trailing newline). Not RT-safe (allocates).
std::string formatRtLogRecord(const RtLogRecord& r);

class RtLog {
public:
  // Allocates: construct off the realtime thread. capacityPow2: power of two in [2, 2^31].
  explicit RtLog(uint32_t capacityPow2) : ring_(capacityPow2) {}
  RtLog(const RtLog&) = delete;
  RtLog& operator=(const RtLog&) = delete;

  // Producer side. Stamps sequence (and tsc if 0). Returns false (and counts a drop) if the ring is full.
  // RT-safe.
  bool push(RtLogRecord r) noexcept {  // RT-safe
    const uint64_t seq = p_.pushed.load(std::memory_order_relaxed);
    p_.pushed.store(seq + 1, std::memory_order_relaxed);
    r.sequence = seq;
    if (r.tsc == 0) r.tsc = readTsc();
    if (ring_.tryPush(r)) return true;
    p_.dropped.store(p_.dropped.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    return false;
  }

  // Producer side: builds a record from up to four arithmetic arguments. RT-safe.
  template <class... A>
  bool log(uint32_t code, const char* format, A... args) noexcept {  // RT-safe
    static_assert(sizeof...(A) <= RtLogRecord::kMaxArgs, "RtLog: at most 4 arguments");
    static_assert((std::is_arithmetic_v<A> && ...), "RtLog: arguments must be numbers or bools");
    RtLogRecord r;
    r.format = format;
    r.code = code;
    (setArg(r, args), ...);
    return push(r);
  }

  // Consumer side: pops every record currently in the ring and calls sink(user, record) for each. Returns the
  // number of records popped. RT-safe itself (the sink decides what it costs).
  template <class Sink>
  size_t drain(Sink&& sink) noexcept(noexcept(sink(std::declval<const RtLogRecord&>()))) {
    size_t n = 0;
    while (const RtLogRecord* r = ring_.front()) {
      sink(*r);
      ring_.popFront();
      ++n;
    }
    return n;
  }

  // Records pushed (including dropped ones) and dropped so far. Any thread.
  uint64_t pushed() const noexcept { return p_.pushed.load(std::memory_order_relaxed); }
  uint64_t dropped() const noexcept { return p_.dropped.load(std::memory_order_acquire); }
  uint32_t capacity() const noexcept { return ring_.capacity(); }

private:
  static uint64_t readTsc() noexcept { return __rdtsc(); }  // RT-safe

  template <class T>
  static void setArg(RtLogRecord& r, T v) noexcept {
    RtLogRecord::Arg& a = r.args[r.argc];
    RtLogRecord::Kind& k = r.kinds[r.argc];
    if constexpr (std::is_same_v<T, bool>) {
      a.u = v ? 1u : 0u;
      k = RtLogRecord::Kind::Bool;
    } else if constexpr (std::is_floating_point_v<T>) {
      a.d = static_cast<double>(v);
      k = RtLogRecord::Kind::Float;
    } else if constexpr (std::is_signed_v<T>) {
      a.i = static_cast<int64_t>(v);
      k = RtLogRecord::Kind::Int;
    } else {
      a.u = static_cast<uint64_t>(v);
      k = RtLogRecord::Kind::UInt;
    }
    ++r.argc;
  }

  SpscRing<RtLogRecord> ring_;
  struct alignas(kCacheLineSize) ProducerCounters {
    std::atomic<uint64_t> pushed{0};   // written by the producer only
    std::atomic<uint64_t> dropped{0};
  };
  ProducerCounters p_;
};

// Drains registered RtLogs on its own (non-realtime, above-normal priority) thread every `periodMs` (absolute
// deadlines on a high-resolution timer) and writes formatted lines through a sink (default: stderr). Lines are
// "[log name #sequence] text"; drops are reported as "[log name] dropped N records (...)" after the drain that
// noticed them, and show as a gap in the sequence numbers.
class RtLogger {
public:
  using LineSink = void (*)(void* user, const std::string& line);

  // sink == nullptr writes lines to stderr.
  explicit RtLogger(LineSink sink = nullptr, void* user = nullptr, uint32_t periodMs = 10);
  ~RtLogger();
  RtLogger(const RtLogger&) = delete;
  RtLogger& operator=(const RtLogger&) = delete;

  // Before start(). The log must outlive the logger (or its stop()).
  void add(RtLog& log, std::string name);
  void start();
  // Stops the thread after a final drain.
  void stop();
  // Drains every log once on the calling thread (when not started). Returns records written.
  size_t drainOnce();

  uint64_t linesWritten() const noexcept { return lines_.load(std::memory_order_relaxed); }
  uint64_t drains() const noexcept { return drains_.load(std::memory_order_relaxed); }
  // Longest time between the starts of two drains by the logger thread (its health: a ring of capacity C fed at
  // R records/s overflows only if this exceeds C / R). 0 before two drains.
  double maxDrainIntervalMs() const noexcept { return maxIntervalUs_.load(std::memory_order_relaxed) / 1000.0; }

private:
  struct Source {
    RtLog* log;
    std::string name;
    uint64_t reportedDrops;
  };
  void run();

  LineSink sink_;
  void* user_;
  uint32_t periodMs_;
  std::vector<Source> sources_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> lines_{0};
  std::atomic<uint64_t> drains_{0};
  std::atomic<double> maxIntervalUs_{0};
};

}  // namespace pq
