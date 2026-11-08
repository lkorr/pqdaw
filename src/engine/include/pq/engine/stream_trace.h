#pragma once

// AU.stream: the audio stream's flight recorder. A process-wide, fixed-size, lock-free ring of the events that shape
// what reaches the speakers: stream open / start / stop / close, every device buffer (ASIO bufferSwitch: its half
// index, the driver's sample position, whether our callback filled it, the peak / first / last sample of output 1),
// every ASIO message and rate change, driver resets and their reopen, and the audio output's own decisions (suspend,
// resume, the cold-start pre-roll, a deferred transport, a fade before a stop).
//
// Writers: any thread (the driver's audio thread, its message callback, the device control threads), RT-safe: one
// fetch_add on the head and a seqlock'd slot (six relaxed stores, two release stores), no allocation, no locks, no
// syscalls. The ring overwrites its oldest events (it is a recorder, not a queue). Readers (off the audio thread)
// copy the events they still find (read()) and format them (describeStreamTrace()): the audio output prints new
// notable events to stdout as it drains, and the F12 report shows the recent history.
//
// Cost: 8192 slots × 48 B = 384 KiB static; ≈ 20 ns per event (one contended-free atomic add on x64), i.e. one
// event per device buffer, ≈ 0.001 % of a core at 48 kHz / 1024 frames, ≈ 0.02 % at 64 frames.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pq::engine {

enum class TraceKind : uint16_t {
  None = 0,
  // Device control threads.
  Open,           // a = buffer frames, b = sample rate, x = output latency (frames)
  Start,          // before the driver's start: a = backend (engine::Backend)
  Started,        // after it: a = 0 ok / 1 failed
  Stop,           // before the driver's stop
  Stopped,        // after it (no callback runs any more)
  Close,
  Reopen,         // a driver request is being handled: a = request bits (1 reset, 2 latencies, 4 rate), b = new rate
  Reopened,       // a = buffer frames, b = sample rate, x = output latency; flags 1 = failed
  Latencies,      // re-queried: a = input, b = output (frames)
  // The driver's audio thread.
  Buffer,         // a = ASIO half index (-1: not ASIO), b = sample position (kNoPosition: none); x / y / z = peak / first /
                  // last sample of output 1; flags: kBufFilled (our callback ran), kBufTimeInfo, kBufMissed (position jump)
  // Driver messages (any thread).
  Message,        // a = selector, b = value, x = our answer
  RateChanged,    // b = rate (Hz)
  // The audio output (pq/app/audio_output.h).
  Suspend,        // the idle stream is stopped
  Resume,         // a suspended stream is started again (a = why: 0 wake, 1 tone / preview)
  PrerollBegin,   // a cold start: silence until the driver settles (a = buffers wanted, b = bound in ms)
  PrerollEnd,     // the engine runs from here: a = buffers of silence, b = elapsed µs, flags 1 = bounded out (not settled)
  FadeOut,        // a fade before a stop while audio sounds: a = fade frames, b = waited µs, flags 1 = timed out
  Recover,        // WASAPI exclusive: the stream restarted itself after an underrun
};

constexpr int64_t kNoPosition = INT64_MIN;
constexpr uint16_t kBufFilled = 1, kBufTimeInfo = 2, kBufMissed = 4;

struct TraceEvent {
  uint64_t seq = 0;  // position in the ring's history (0, 1, 2, ...)
  uint64_t tsc = 0;  // __rdtsc() when it was recorded
  TraceKind kind = TraceKind::None;
  uint16_t flags = 0;
  int32_t a = 0;
  int64_t b = 0;
  float x = 0, y = 0, z = 0;
};

class StreamTrace {
public:
  static constexpr uint32_t kCapacity = 8192;  // power of two

  // RT-safe, any thread.
  void push(TraceKind kind, uint16_t flags = 0, int32_t a = 0, int64_t b = 0, float x = 0, float y = 0,
            float z = 0) noexcept;
  // Events recorded so far (the next event's seq). RT-safe.
  uint64_t head() const noexcept { return head_.load(std::memory_order_acquire); }
  // Off the audio thread (appends to `out`): the events with seq in [from, head()) that are still in the ring, in
  // order. Returns the new cursor (head at the call); `lost` (if given) counts events overwritten before they were read.
  uint64_t read(uint64_t from, std::vector<TraceEvent>& out, uint64_t* lost = nullptr) const;

private:
  struct Slot {
    std::atomic<uint64_t> stamp{0};  // seq + 1 once written, 0 while being written
    std::atomic<uint64_t> tsc{0};
    std::atomic<uint64_t> kindFlagsA{0};  // kind << 48 | flags << 32 | uint32(a)
    std::atomic<int64_t> b{0};
    std::atomic<uint64_t> xy{0};
    std::atomic<uint32_t> z{0};
  };
  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) Slot slots_[kCapacity];
};

// The process's recorder.
StreamTrace& streamTrace() noexcept;

// How describeStreamTrace() lists buffers. frames / rate: the stream's, updated by Open / Reopened events as they pass
// (keep the struct between calls that format consecutive batches).
struct TraceFormat {
  bool allBuffers = false;         // every buffer, not only the notable ones
  bool summarise = true;           // a "... N buffers" line for the ones not listed
  uint32_t buffersAfterStart = 6;  // list the first N after a Start / PrerollEnd / Reopened
  float jump = 0.25f;              // output 1 stepping by more than this between buffers is notable
  uint32_t frames = 0, rate = 0;
  int32_t lastHalf = -2;           // carried between batches, like frames / rate
  bool alternates = false;         // the driver alternates its half index (FL Studio ASIO always passes 0)
};

// Text for the log and the F12 report (allocates; off the audio thread). Times are milliseconds relative to `t0`
// (a TSC; 0 = the first event). Unless f.allBuffers, buffers are listed only where something happens: the first
// f.buffersAfterStart after a start, a repeated half index from a driver that alternates, a position step other than
// the buffer size, an interval under a quarter or over 1.75 periods (drivers on a 10 ms WASAPI grid deliver 1024
// frames at 48 kHz in 20 / 30 ms steps), our callback not filling it, a missed buffer, or output 1 jumping between
// buffers by more than f.jump.
std::string describeStreamTrace(const std::vector<TraceEvent>& events, uint64_t t0, TraceFormat& f);
const char* traceKindName(TraceKind k) noexcept;

}  // namespace pq::engine
