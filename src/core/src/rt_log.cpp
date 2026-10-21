#include "pq/core/rt_log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <utility>

#include <windows.h>

namespace pq {

namespace {

void appendArg(std::string& out, const RtLogRecord& r, uint32_t i) {
  char buf[48];
  switch (r.kinds[i]) {
    case RtLogRecord::Kind::Int: std::snprintf(buf, sizeof buf, "%" PRId64, r.args[i].i); break;
    case RtLogRecord::Kind::UInt: std::snprintf(buf, sizeof buf, "%" PRIu64, r.args[i].u); break;
    case RtLogRecord::Kind::Float: std::snprintf(buf, sizeof buf, "%.9g", r.args[i].d); break;
    case RtLogRecord::Kind::Bool: std::snprintf(buf, sizeof buf, "%s", r.args[i].u ? "true" : "false"); break;
    case RtLogRecord::Kind::None: std::snprintf(buf, sizeof buf, "?"); break;
  }
  out += buf;
}

void writeStderr(void*, const std::string& line) {
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fputc('\n', stderr);
}

}  // namespace

std::string formatRtLogRecord(const RtLogRecord& r) {
  std::string out;
  const uint32_t argc = r.argc <= RtLogRecord::kMaxArgs ? r.argc : RtLogRecord::kMaxArgs;
  uint32_t next = 0;
  if (r.format) {
    for (const char* p = r.format; *p; ++p) {
      if (p[0] == '{' && p[1] == '{') {
        out += '{';
        ++p;
      } else if (p[0] == '}' && p[1] == '}') {
        out += '}';
        ++p;
      } else if (p[0] == '{' && p[1] == '}') {
        if (next < argc)
          appendArg(out, r, next++);
        else
          out += "{?}";
        ++p;
      } else {
        out += *p;
      }
    }
  }
  // Arguments without a placeholder are still shown rather than silently lost.
  for (; next < argc; ++next) {
    out += next == 0 && !r.format ? "" : " ";
    appendArg(out, r, next);
  }
  return out;
}

RtLogger::RtLogger(LineSink sink, void* user, uint32_t periodMs)
    : sink_(sink ? sink : &writeStderr), user_(user), periodMs_(periodMs ? periodMs : 1) {}

RtLogger::~RtLogger() { stop(); }

void RtLogger::add(RtLog& log, std::string name) { sources_.push_back({&log, std::move(name), 0}); }

size_t RtLogger::drainOnce() {
  size_t written = 0;
  std::string line;
  for (Source& src : sources_) {
    written += src.log->drain([&](const RtLogRecord& r) {
      line.clear();
      line += '[';
      line += src.name;
      line += " #";
      line += std::to_string(r.sequence);
      line += "] ";
      line += formatRtLogRecord(r);
      sink_(user_, line);
    });
    // After the drain: drops counted so far precede (or are interleaved with) what was just written; the gap in
    // the sequence numbers shows where.
    const uint64_t dropped = src.log->dropped();
    if (dropped != src.reportedDrops) {
      line = "[" + src.name + "] dropped " + std::to_string(dropped - src.reportedDrops) + " records (ring full; " +
             std::to_string(dropped) + " in total)";
      sink_(user_, line);
      src.reportedDrops = dropped;
      ++written;
    }
  }
  lines_.store(lines_.load(std::memory_order_relaxed) + written, std::memory_order_relaxed);
  drains_.store(drains_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  return written;
}

void RtLogger::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void RtLogger::stop() {
  if (running_.exchange(false) && thread_.joinable()) thread_.join();
}

void RtLogger::run() {
  // Light work, but it has to keep up with the rings: above-normal priority, and absolute deadlines on a
  // high-resolution timer so the drain rate is the configured one even when a wake-up is late.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  LARGE_INTEGER freq{}, start{}, now{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  uint64_t k = 0;
  while (running_.load(std::memory_order_acquire)) {
    drainOnce();
    ++k;
    const int64_t deadline = start.QuadPart + static_cast<int64_t>(k * periodMs_ * freq.QuadPart / 1000);
    QueryPerformanceCounter(&now);
    if (now.QuadPart >= deadline) continue;
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<int64_t>(1, (deadline - now.QuadPart) * 10'000'000 / freq.QuadPart);
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
      WaitForSingleObject(timer, INFINITE);
    else
      Sleep(periodMs_);
  }
  drainOnce();  // whatever arrived since the last pass
  if (timer) CloseHandle(timer);
}

}  // namespace pq
