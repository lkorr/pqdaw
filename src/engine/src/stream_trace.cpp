// AU.stream: the audio stream's flight recorder (pq/engine/stream_trace.h).
#include "pq/engine/stream_trace.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <intrin.h>

#include "pq/engine/asio_abi.h"
#include "pq/engine/callback_stats.h"

namespace pq::engine {

StreamTrace& streamTrace() noexcept {
  static StreamTrace t;
  return t;
}

void StreamTrace::push(TraceKind kind, uint16_t flags, int32_t a, int64_t b, float x, float y, float z) noexcept {  // RT-safe
  const uint64_t seq = head_.fetch_add(1, std::memory_order_acq_rel);
  Slot& s = slots_[seq & (kCapacity - 1)];
  s.stamp.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  s.tsc.store(__rdtsc(), std::memory_order_relaxed);
  s.kindFlagsA.store(static_cast<uint64_t>(kind) << 48 | static_cast<uint64_t>(flags) << 32 | static_cast<uint32_t>(a),
                     std::memory_order_relaxed);
  s.b.store(b, std::memory_order_relaxed);
  s.xy.store(static_cast<uint64_t>(std::bit_cast<uint32_t>(x)) << 32 | std::bit_cast<uint32_t>(y), std::memory_order_relaxed);
  s.z.store(std::bit_cast<uint32_t>(z), std::memory_order_relaxed);
  s.stamp.store(seq + 1, std::memory_order_release);
}

uint64_t StreamTrace::read(uint64_t from, std::vector<TraceEvent>& out, uint64_t* lost) const {
  const uint64_t h = head();
  uint64_t first = from;
  if (h > kCapacity && first < h - kCapacity) first = h - kCapacity;
  uint64_t gone = first - from;
  for (uint64_t seq = first; seq < h; ++seq) {
    const Slot& s = slots_[seq & (kCapacity - 1)];
    const uint64_t st = s.stamp.load(std::memory_order_acquire);
    TraceEvent e;
    e.seq = seq;
    e.tsc = s.tsc.load(std::memory_order_relaxed);
    const uint64_t kfa = s.kindFlagsA.load(std::memory_order_relaxed);
    e.b = s.b.load(std::memory_order_relaxed);
    const uint64_t xy = s.xy.load(std::memory_order_relaxed);
    const uint32_t z = s.z.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (st != seq + 1 || s.stamp.load(std::memory_order_relaxed) != st) {  // being written, or already overwritten
      ++gone;
      continue;
    }
    e.kind = static_cast<TraceKind>(kfa >> 48);
    e.flags = static_cast<uint16_t>(kfa >> 32);
    e.a = static_cast<int32_t>(static_cast<uint32_t>(kfa));
    e.x = std::bit_cast<float>(static_cast<uint32_t>(xy >> 32));
    e.y = std::bit_cast<float>(static_cast<uint32_t>(xy));
    e.z = std::bit_cast<float>(z);
    out.push_back(e);
  }
  if (lost) *lost = gone;
  return h;
}

const char* traceKindName(TraceKind k) noexcept {
  switch (k) {
    case TraceKind::None: return "none";
    case TraceKind::Open: return "open";
    case TraceKind::Start: return "start";
    case TraceKind::Started: return "started";
    case TraceKind::Stop: return "stop";
    case TraceKind::Stopped: return "stopped";
    case TraceKind::Close: return "close";
    case TraceKind::Reopen: return "driver request: reopen";
    case TraceKind::Reopened: return "reopened";
    case TraceKind::Latencies: return "latencies";
    case TraceKind::Buffer: return "buffer";
    case TraceKind::Message: return "asioMessage";
    case TraceKind::RateChanged: return "sampleRateDidChange";
    case TraceKind::Suspend: return "output: suspend";
    case TraceKind::Resume: return "output: resume";
    case TraceKind::PrerollBegin: return "output: pre-roll begin";
    case TraceKind::PrerollEnd: return "output: pre-roll end";
    case TraceKind::FadeOut: return "output: fade-out";
    case TraceKind::Recover: return "wasapi: restart after an underrun";
  }
  return "?";
}

namespace {

const char* selectorName(int32_t s) {
  switch (s) {
    case asio::kMsgSelectorSupported: return "selectorSupported";
    case asio::kMsgEngineVersion: return "engineVersion";
    case asio::kMsgResetRequest: return "resetRequest";
    case asio::kMsgBufferSizeChange: return "bufferSizeChange";
    case asio::kMsgResyncRequest: return "resyncRequest";
    case asio::kMsgLatenciesChanged: return "latenciesChanged";
    case asio::kMsgSupportsTimeInfo: return "supportsTimeInfo";
    case asio::kMsgSupportsTimeCode: return "supportsTimeCode";
    case asio::kMsgOverload: return "overload";
    default: return "selector";
  }
}

}  // namespace

std::string describeStreamTrace(const std::vector<TraceEvent>& ev, uint64_t t0, TraceFormat& f) {
  std::string s;
  if (ev.empty()) return s;
  const double msPerTick = 1000.0 / tscFrequencyHz();
  if (t0 == 0) t0 = ev.front().tsc;
  auto ms = [&](uint64_t tsc) { return static_cast<double>(static_cast<int64_t>(tsc - t0)) * msPerTick; };
  char line[256];
  // Buffer run state (within this batch).
  uint32_t sinceStart = 1u << 30;
  int64_t lastPos = kNoPosition;
  uint64_t lastTsc = 0;
  float lastSample = 0;
  bool haveLast = false;
  uint32_t quiet = 0;  // buffers summarised since the last listed line
  double quietMaxMs = 0, quietMinMs = 1e30;
  float quietPeak = 0;
  auto flushQuiet = [&] {
    if (quiet == 0) return;
    if (f.summarise) {
      std::snprintf(line, sizeof line, "           ... %u buffers, interval %.2f-%.2f ms, peak %.4f\n", quiet,
                    quietMinMs < 1e29 ? quietMinMs : 0.0, quietMaxMs, static_cast<double>(quietPeak));
      s += line;
    }
    quiet = 0;
    quietMaxMs = 0;
    quietMinMs = 1e30;
    quietPeak = 0;
  };
  for (const TraceEvent& e : ev) {
    const double t = ms(e.tsc);
    if (e.kind != TraceKind::Buffer) {
      flushQuiet();
      switch (e.kind) {
        case TraceKind::Open:
        case TraceKind::Reopened:
          if (!(e.flags & 1)) {
            f.frames = static_cast<uint32_t>(e.a);
            f.rate = static_cast<uint32_t>(e.b);
          }
          std::snprintf(line, sizeof line, "%9.2f ms %s%s: %u frames, %u Hz, output latency %.0f frames\n", t,
                        traceKindName(e.kind), e.flags & 1 ? " FAILED" : "", static_cast<uint32_t>(e.a),
                        static_cast<uint32_t>(e.b), static_cast<double>(e.x));
          break;
        case TraceKind::Message:
          std::snprintf(line, sizeof line, "%9.2f ms %s %s (%d), value %lld, answer %g\n", t, traceKindName(e.kind),
                        selectorName(e.a), e.a, static_cast<long long>(e.b), static_cast<double>(e.x));
          break;
        case TraceKind::RateChanged:
          std::snprintf(line, sizeof line, "%9.2f ms %s %lld Hz\n", t, traceKindName(e.kind), static_cast<long long>(e.b));
          break;
        case TraceKind::Reopen:
          std::snprintf(line, sizeof line, "%9.2f ms %s (%s%s%s), new rate %lld\n", t, traceKindName(e.kind),
                        e.a & 1 ? "reset " : "", e.a & 2 ? "latencies " : "", e.a & 4 ? "rate" : "",
                        static_cast<long long>(e.b));
          break;
        case TraceKind::Latencies:
          std::snprintf(line, sizeof line, "%9.2f ms %s: in %d, out %lld frames\n", t, traceKindName(e.kind), e.a,
                        static_cast<long long>(e.b));
          break;
        case TraceKind::Started:
          std::snprintf(line, sizeof line, "%9.2f ms %s%s\n", t, traceKindName(e.kind), e.a ? " FAILED" : "");
          break;
        case TraceKind::PrerollBegin:
          std::snprintf(line, sizeof line, "%9.2f ms %s: silence for >= %d frames, at most %lld\n", t,
                        traceKindName(e.kind), e.a, static_cast<long long>(e.b));
          break;
        case TraceKind::PrerollEnd:
          std::snprintf(line, sizeof line, "%9.2f ms %s: %d buffers of silence over %.1f ms%s; the engine runs\n", t,
                        traceKindName(e.kind), e.a, static_cast<double>(e.b) / 1000.0,
                        e.flags & 1 ? " (bounded: not settled)" : "");
          break;
        case TraceKind::FadeOut:
          std::snprintf(line, sizeof line, "%9.2f ms %s over %d frames, then silence; waited %.1f ms%s\n", t,
                        traceKindName(e.kind), e.a, static_cast<double>(e.b) / 1000.0, e.flags & 1 ? " (timed out)" : "");
          break;
        default: std::snprintf(line, sizeof line, "%9.2f ms %s\n", t, traceKindName(e.kind)); break;
      }
      s += line;
      if (e.kind == TraceKind::Start || e.kind == TraceKind::PrerollEnd || e.kind == TraceKind::Reopened) {
        sinceStart = 0;
        lastPos = kNoPosition;
        lastTsc = 0;
        haveLast = false;
        if (e.kind != TraceKind::PrerollEnd) f.lastHalf = -2;
      }
      continue;
    }
    // A buffer.
    const double interval = lastTsc ? static_cast<double>(e.tsc - lastTsc) * msPerTick : 0.0;
    const double period = f.rate && f.frames ? 1000.0 * f.frames / f.rate : 0.0;
    if (e.a >= 0 && f.lastHalf >= 0 && e.a != f.lastHalf) f.alternates = true;
    std::string why;
    if (sinceStart < f.buffersAfterStart) why += " first";
    if (!(e.flags & kBufFilled)) why += " NOT-FILLED";
    if (e.flags & kBufMissed) why += " MISSED(position jump)";
    if (f.alternates && e.a >= 0 && e.a == f.lastHalf) why += " SAME-HALF";
    if (e.b != kNoPosition && lastPos != kNoPosition && f.frames && e.b - lastPos != static_cast<int64_t>(f.frames)) {
      char w[64];
      std::snprintf(w, sizeof w, " POSITION-STEP %lld", static_cast<long long>(e.b - lastPos));
      why += w;
    }
    if (lastTsc && period > 0 && (interval > 1.75 * period || interval < 0.25 * period)) why += " LATE/EARLY";
    if (haveLast && (e.flags & kBufFilled) && std::fabs(e.y - lastSample) > f.jump) why += " OUTPUT-JUMP";
    if (f.allBuffers || !why.empty()) {
      flushQuiet();
      std::snprintf(line, sizeof line, "%9.2f ms buffer half %d pos %lld (+%.2f ms) peak %.4f first %.4f last %.4f%s\n",
                    t, e.a, e.b == kNoPosition ? -1LL : static_cast<long long>(e.b), interval, static_cast<double>(e.x),
                    static_cast<double>(e.y), static_cast<double>(e.z), why.c_str());
      s += line;
    } else {
      ++quiet;
      if (lastTsc) {
        quietMaxMs = std::max(quietMaxMs, interval);
        quietMinMs = std::min(quietMinMs, interval);
      }
      quietPeak = std::max(quietPeak, e.x);
    }
    ++sinceStart;
    f.lastHalf = e.a;
    if (e.b != kNoPosition) lastPos = e.b;
    lastTsc = e.tsc;
    if (e.flags & kBufFilled) {
      lastSample = e.z;
      haveLast = true;
    }
  }
  flushQuiet();
  return s;
}

}  // namespace pq::engine
