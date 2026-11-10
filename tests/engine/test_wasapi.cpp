// WASAPI backend (T0.6-fix1): sample conversion, underrun accounting with a fake clock, and device checks that
// need no running stream; T6.7b: the capture FIFO (bit-exact hand-over, priming at the target delay, capture times,
// underflow / drift bounds, packet formats, RtScope). The device-level runs (open/play on real hardware) are opt-in:
// set PQ_TEST_AUDIO_DEVICE=1 to run them; they play silence, and the duplex run a −60 dBFS click (loopback capture).
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/rt_scope.h"
#include "pq/engine/capture_fifo.h"
#include "pq/engine/sample_convert.h"
#include "pq/engine/underrun_tracker.h"
#include "pq/engine/wasapi_device.h"

using namespace pq::engine;

namespace {

// Independent reference: double math, explicit round-half-even, explicit byte layout.
std::vector<uint8_t> reference(const std::vector<std::vector<float>>& in, uint32_t numIn, uint32_t ch, uint32_t frames,
                               SampleFormat f) {
  std::vector<uint8_t> out;
  for (uint32_t i = 0; i < frames; ++i) {
    for (uint32_t c = 0; c < ch; ++c) {
      const float x = c < numIn ? in[c][i] : 0.0f;
      if (f == SampleFormat::Float32) {
        uint8_t b[4];
        std::memcpy(b, &x, 4);
        out.insert(out.end(), b, b + 4);
        continue;
      }
      double scale = 0, lo = 0, hi = 0;
      switch (f) {
        case SampleFormat::Int32: scale = 2147483648.0, lo = -2147483648.0, hi = 2147483520.0; break;
        case SampleFormat::Int24In32:
        case SampleFormat::Int24: scale = 8388608.0, lo = -8388608.0, hi = 8388607.0; break;
        case SampleFormat::Int16: scale = 32768.0, lo = -32768.0, hi = 32767.0; break;
        case SampleFormat::Float32: break;
      }
      double v = static_cast<double>(x) * scale;
      if (std::isnan(v) || v < lo) v = lo;
      if (v > hi) v = hi;
      const int64_t q = static_cast<int64_t>(std::nearbyint(v));  // default rounding: nearest-even
      const uint32_t u = static_cast<uint32_t>(f == SampleFormat::Int24In32 ? q * 256 : q);
      const uint32_t bytes = sampleFormatBytes(f);
      for (uint32_t k = 0; k < bytes; ++k) out.push_back(static_cast<uint8_t>(u >> (8 * k)));
    }
  }
  return out;
}

}  // namespace

TEST_CASE("wasapi: planar float to interleaved device formats matches the reference") {
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> dist(-1.2f, 1.2f);
  const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.99999994f, -0.99999994f, 1.5f, -7.0f,
                            0.5f / 8388608.0f, 1.5f / 32768.0f, 2.5f / 32768.0f, std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 1e-30f, -0.5f};
  for (SampleFormat f : {SampleFormat::Float32, SampleFormat::Int32, SampleFormat::Int24In32, SampleFormat::Int24,
                         SampleFormat::Int16}) {
    for (uint32_t ch : {1u, 2u, 3u, 6u}) {
      for (uint32_t numIn : {0u, 1u, ch, ch + 1}) {
        for (uint32_t frames : {0u, 1u, 7u, 8u, 9u, 16u, 64u, 67u, 144u}) {
          std::vector<std::vector<float>> in(numIn, std::vector<float>(frames));
          uint32_t s = 0;
          for (auto& chan : in)
            for (float& x : chan) x = (s++ % 3 == 0) ? specials[(s / 3) % std::size(specials)] : dist(rng);
          std::vector<const float*> ptrs;
          for (auto& chan : in) ptrs.push_back(chan.data());
          const std::vector<uint8_t> want = reference(in, numIn, ch, frames, f);
          // Guard bytes after the output catch overruns (pack24 uses masked stores; nothing may spill).
          std::vector<uint8_t> got(want.size() + 64, 0xAB);
          const uint64_t violations = pq::alloc_trap::countRtViolations(
              [&] { planarToInterleaved(ptrs.data(), numIn, ch, frames, f, got.data()); });
          CHECK(violations == 0);
          INFO("format ", std::string(sampleFormatName(f)), " ch ", ch, " numIn ", numIn, " frames ", frames);
          CHECK(std::memcmp(got.data(), want.data(), want.size()) == 0);
          bool guardIntact = true;
          for (size_t i = want.size(); i < got.size(); ++i) guardIntact = guardIntact && got[i] == 0xAB;
          CHECK(guardIntact);
        }
      }
    }
  }
}

TEST_CASE("wasapi: integer conversion edge values") {
  const float in0[3] = {1.0f, -1.0f, 0.5f};
  const float* ptrs[1] = {in0};
  int16_t s16[3];
  planarToInterleaved(ptrs, 1, 1, 3, SampleFormat::Int16, s16);
  CHECK(s16[0] == 32767);
  CHECK(s16[1] == -32768);
  CHECK(s16[2] == 16384);
  int32_t s32[3];
  planarToInterleaved(ptrs, 1, 1, 3, SampleFormat::Int24In32, s32);
  CHECK(s32[0] == 8388607 * 256);
  CHECK(s32[1] == -8388608 * 256);
  CHECK(s32[2] == 4194304 * 256);
  planarToInterleaved(ptrs, 1, 1, 3, SampleFormat::Int32, s32);
  CHECK(s32[0] == 2147483520);
  CHECK(s32[1] == std::numeric_limits<int32_t>::min());
  CHECK(s32[2] == 1073741824);
  uint8_t p24[9];
  planarToInterleaved(ptrs, 1, 1, 3, SampleFormat::Int24, p24);
  CHECK((p24[0] == 0xFF && p24[1] == 0xFF && p24[2] == 0x7F));
  CHECK((p24[3] == 0x00 && p24[4] == 0x00 && p24[5] == 0x80));
  CHECK((p24[6] == 0x00 && p24[7] == 0x00 && p24[8] == 0x40));
  CHECK(sampleFormatBytes(SampleFormat::Int24) == 3);
  CHECK(std::string(sampleFormatName(SampleFormat::Int24In32)) == "int24-in-32");
}

namespace {

// Fake device: wakes every `period` frames of wall clock plus jitter; writes one period per wake.
struct FakeExclusive {
  UnderrunTracker t;
  uint32_t period = 64;
  int64_t written = 64;  // pre-fill
  int64_t clock = 0;     // wall clock in frames
  uint64_t episodes = 0;
  int64_t maxLateness = 0;

  FakeExclusive() { t.reset({period, period, false}); }
  UnderrunTracker::Outcome wake(int64_t jitter, int64_t position = -1) {
    UnderrunTracker::Wake w;
    w.nowFrames = clock + jitter;
    w.written = written;
    w.position = position;
    const auto o = t.onWake(w);
    if (o.newEpisode) ++episodes;
    if (o.latenessFrames > maxLateness) maxLateness = o.latenessFrames;
    written += period;
    t.onWritten(period);
    clock += period;
    return o;
  }
};

}  // namespace

TEST_CASE("wasapi: underrun tracker, exclusive schedule with jitter, drift and one stall") {
  FakeExclusive f;
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> jitter(0, 20);  // up to 20 frames late per wake: inside the slack
  for (int i = 0; i < 10000; ++i) f.wake(jitter(rng));
  CHECK(f.episodes == 0);
  CHECK(f.maxLateness <= 20);
  CHECK(f.t.episodes() == 0);

  // A wake within the slack but late (60 of 64 frames) is lateness, not an underrun.
  auto o = f.wake(60);
  CHECK_FALSE(o.underrun);
  CHECK(o.latenessFrames >= 40);

  // One stall: the device clock moves 3 periods on before the next wake; the stream then continues on the
  // shifted schedule (the device played silence / repeated data meanwhile). Exactly one episode.
  f.clock += 3 * f.period;
  o = f.wake(0);
  CHECK(o.underrun);
  CHECK(o.newEpisode);
  for (int i = 0; i < 5000; ++i) f.wake(jitter(rng));
  CHECK(f.episodes == 1);
  CHECK(f.t.episodes() == 1);

  // Slow drift: the device runs 200 ppm slower than the wall clock for 200 000 wakes (~0.04 frames per wake,
  // 2560 frames in total, far more than the slack): the windowed baseline follows it.
  double drift = 0;
  for (int i = 0; i < 200000; ++i) {
    drift += 0.0128;
    const int64_t whole = static_cast<int64_t>(drift);
    drift -= static_cast<double>(whole);
    f.clock += whole;
    f.wake(jitter(rng));
  }
  CHECK(f.episodes == 1);
}

TEST_CASE("wasapi: underrun tracker, exclusive late wake with data still queued is not an underrun") {
  FakeExclusive f;
  for (int i = 0; i < 40; ++i) f.wake(0, f.written - 64);  // the device holds one period beyond the wake
  f.clock += 100;                                          // a wake 100 frames late (> the 64-frame slack)
  auto o = f.wake(0, f.written - 64);                      // ... but the reserve was still there
  CHECK_FALSE(o.underrun);
  CHECK(o.latenessFrames == 100);
  CHECK(o.queueFrames == 64);
  f.clock += 100;
  o = f.wake(0, f.written);  // late again, and the device has taken everything: it ran dry
  CHECK(o.underrun);
  CHECK(f.t.episodes() == 1);
}

TEST_CASE("wasapi: underrun tracker, consecutive late wakes and timeouts are one episode each") {
  FakeExclusive f;
  for (int i = 0; i < 100; ++i) f.wake(0);
  // Two timeouts in a row (no device events), then normal: one episode.
  UnderrunTracker::Wake w;
  w.nowFrames = f.clock;
  w.written = f.written;
  w.timedOut = true;
  CHECK(f.t.onWake(w).newEpisode);
  w.nowFrames += 4 * f.period;
  const auto second = f.t.onWake(w);
  CHECK(second.underrun);
  CHECK_FALSE(second.newEpisode);
  f.clock = w.nowFrames;  // events resume on the schedule of the last timed-out wake
  CHECK_FALSE(f.wake(0).underrun);
  CHECK(f.t.episodes() == 1);
}

TEST_CASE("wasapi: underrun tracker, device clock past the frames written") {
  FakeExclusive f;
  for (int i = 0; i < 40; ++i) f.wake(0, f.written);  // position == written: nothing consumed beyond the data
  CHECK(f.t.episodes() == 0);
  const auto o = f.wake(0, f.written + 1);
  CHECK(o.underrun);
  CHECK(f.t.episodes() == 1);
}

TEST_CASE("wasapi: underrun tracker, shared-mode padding") {
  // Shared: 1056-frame endpoint buffer, 480-frame engine period, one period written per wake (as on the
  // Focusrite: padding 576 at every on-time wake).
  UnderrunTracker t;
  t.reset({480, 1056, true});
  int64_t now = 0, written = 960;
  auto wake = [&](int32_t padding, uint32_t write) {
    UnderrunTracker::Wake w;
    w.nowFrames = now;
    w.written = written;
    w.padding = padding;
    w.position = written - padding - 100;  // device clock 100 frames behind the engine's read pointer
    const auto o = t.onWake(w);
    written += write;
    t.onWritten(write);
    now += 480;
    return o;
  };
  CHECK_FALSE(wake(96, 960).underrun);  // start-up wake: fill 96 + 960 = 1056
  for (int i = 0; i < 1000; ++i) {
    const auto o = wake(576, 480);
    CHECK_FALSE(o.underrun);
    CHECK(o.queueFrames == (i + 1 < static_cast<int>(UnderrunTracker::kWarmupWakes) ? -1 : 676));
  }
  // A stall: the engine drained the buffer (padding 0 with a fill of 1056, not a whole number of periods).
  now += 2 * 480;
  const auto o = wake(0, 960);
  CHECK(o.underrun);
  CHECK(o.newEpisode);
  for (int i = 0; i < 1000; ++i) CHECK_FALSE(wake(576, 480).underrun);
  CHECK(t.episodes() == 1);
  // The engine's pass schedule wanders against the wall clock (three periods here, beyond the 1056-frame
  // buffer) while the buffer never drains: reported as lateness, not an underrun.
  now += 3 * 480;
  const auto wander = wake(576, 480);
  CHECK_FALSE(wander.underrun);
  CHECK(wander.latenessFrames >= 1440);
  CHECK(t.episodes() == 1);

  // A fill level that is a whole number of periods (960) may drain exactly without a glitch: padding 0 alone
  // does not count then.
  UnderrunTracker t2;
  t2.reset({480, 960, true});
  UnderrunTracker::Wake w;
  w.padding = 480;
  w.written = 960;
  for (int i = 0; i < 40; ++i) {
    CHECK_FALSE(t2.onWake(w).underrun);
    t2.onWritten(480);  // fill 960
    w.written += 480;
    w.nowFrames += 480;
  }
  w.padding = 0;
  CHECK_FALSE(t2.onWake(w).underrun);  // on time, buffer drained exactly: no glitch
  t2.onWritten(960);
  w.written += 960;
  w.nowFrames += 480;
  for (int i = 0; i < 20; ++i) {
    w.padding = 480;
    CHECK_FALSE(t2.onWake(w).underrun);
    t2.onWritten(480);
    w.written += 480;
    w.nowFrames += 480;
  }
  w.padding = 0;
  w.nowFrames += 3 * 480;  // empty and three periods late: starved even though 960 is a whole number of periods
  CHECK(t2.onWake(w).underrun);
}

TEST_CASE("wasapi: underrun tracker, irregular start-up is warm-up; event-phase wander is not an underrun") {
  FakeExclusive f;
  // Start-up: the device takes several buffers back to back while it fills its pipeline (X falls, then settles).
  for (int i = 0; i < 6; ++i) {
    f.clock -= 50;
    f.wake(0);
  }
  for (int i = 0; i < 100; ++i) f.wake(0);
  CHECK(f.t.episodes() == 0);
  CHECK(f.maxLateness == 0);
  // Events on a coarse grid: the event phase moves 40 frames later for a while and back (no data lost, the
  // device's cushion absorbs it). Lateness, not an underrun.
  f.clock += 40;
  for (int i = 0; i < 50; ++i) f.wake(0);
  f.clock -= 40;
  for (int i = 0; i < 50; ++i) f.wake(0);
  CHECK(f.t.episodes() == 0);
  CHECK(f.maxLateness == 40);
}

TEST_CASE("wasapi: enumerate lists endpoints with stable ids") {
  WasapiDevice dev;
  CHECK(dev.backend() == Backend::Wasapi);
  CHECK_FALSE(dev.isOpen());
  CHECK_FALSE(dev.isRunning());
  auto list = dev.enumerate();
  REQUIRE(list.ok());  // may be empty on a machine without audio endpoints
  std::set<std::string> ids;
  int defaultOutputs = 0;
  for (const DeviceInfo& d : *list) {
    CHECK(d.id != 0);
    CHECK_FALSE(d.name.empty());
    CHECK_FALSE(d.endpointId.empty());
    ids.insert(d.endpointId);
    defaultOutputs += d.isDefaultOutput ? 1 : 0;
    CHECK((d.outputChannels == 0 || d.inputChannels == 0));
  }
  CHECK(ids.size() == list->size());
  CHECK(defaultOutputs <= 1);
}

namespace {
void silent(void*, const AudioBlock& b) noexcept {
  for (uint32_t c = 0; c < b.numOut; ++c) std::memset(b.out[c], 0, sizeof(float) * b.frames);
}
}  // namespace

TEST_CASE("wasapi: invalid stream requests fail cleanly") {
  WasapiDevice dev;
  StreamConfig c;
  c.outputDevice = 0x7FFFFFF0u;
  CHECK(dev.open(c, {&silent, nullptr}).error().code == pq::ErrorCode::NotFound);
  c = StreamConfig{};
  c.outputEndpointId = "{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}";
  CHECK(dev.open(c, {&silent, nullptr}).error().code == pq::ErrorCode::NotFound);
  c = StreamConfig{};
  CHECK(dev.open(c, AudioCallback{}).error().code == pq::ErrorCode::InvalidArgument);
  c.outputChannels = 0;
  CHECK(dev.open(c, {&silent, nullptr}).error().code == pq::ErrorCode::InvalidArgument);
  c = StreamConfig{};
  c.bufferFrames = 0;
  CHECK(dev.open(c, {&silent, nullptr}).error().code == pq::ErrorCode::InvalidArgument);
  c = StreamConfig{};
  c.inputChannels = kMaxChannels + 1;  // T6.7b: inputs open (shared capture), but not more than the engine takes
  CHECK(dev.open(c, {&silent, nullptr}).error().code == pq::ErrorCode::InvalidArgument);
  CHECK_FALSE(dev.isOpen());
  CHECK(dev.start().error().code == pq::ErrorCode::InvalidArgument);
  CHECK(dev.stop().ok());
  auto created = createAudioDevice(Backend::Wasapi);
  REQUIRE(created.ok());
  CHECK((*created)->backend() == Backend::Wasapi);
}

namespace {

bool deviceTestsEnabled() {
  const char* v = std::getenv("PQ_TEST_AUDIO_DEVICE");
  return v && std::string(v) == "1";
}

struct SilentProbe {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> notRt{0};
  static void process(void* user, const AudioBlock& b) noexcept {
    auto& p = *static_cast<SilentProbe*>(user);
    for (uint32_t c = 0; c < b.numOut; ++c) std::memset(b.out[c], 0, sizeof(float) * b.frames);
    if (!pq::inRtScope()) p.notRt.fetch_add(1, std::memory_order_relaxed);
    p.calls.fetch_add(1, std::memory_order_relaxed);
  }
};

// Opens the default output, plays silence for `seconds`, returns the stats.
CallbackStats::Snapshot runSilence(ShareMode mode, uint32_t frames, double seconds, uint64_t stallAfterWake,
                                   StreamInfo& info, SilentProbe& probe) {
  WasapiDevice dev;
  dev.injectStallAfterWake(stallAfterWake);
  StreamConfig c;
  c.shareMode = mode;
  c.bufferFrames = frames;
  auto opened = dev.open(c, {&SilentProbe::process, &probe});
  REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? "" : opened.error().message));
  info = *opened;
  REQUIRE(dev.start().ok());
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  CHECK(dev.stop().ok());
  const auto st = dev.stats().snapshot();
  dev.close();
  CHECK_FALSE(dev.isOpen());
  return st;
}

}  // namespace

TEST_CASE("wasapi device (opt-in, PQ_TEST_AUDIO_DEVICE=1): exclusive and shared streams, stall detection") {
  if (!deviceTestsEnabled()) {
    MESSAGE("skipped: set PQ_TEST_AUDIO_DEVICE=1 to open the default output device (plays silence)");
    return;
  }
  for (ShareMode mode : {ShareMode::Exclusive, ShareMode::Shared}) {
    const char* name = mode == ShareMode::Exclusive ? "exclusive" : "shared";
    const uint64_t violationsBefore = pq::alloc_trap::violationCount();
    SilentProbe probe;
    StreamInfo info;
    auto st = runSilence(mode, 288, 1.0, 0, info, probe);
    INFO(std::string(name), ": period ", info.bufferFrames, " frames, ", info.sampleRate, " Hz, ",
         std::string(sampleFormatName(info.deviceFormat)));
    CHECK(pq::alloc_trap::violationCount() == violationsBefore);  // whole wake path inside RtScope, no allocation
    CHECK(probe.notRt.load() == 0);
    CHECK(st.callbacks > 0);
    CHECK(st.wakes > 0);
    CHECK(st.outputUnderflows == 0);
    CHECK(st.waitTimeouts == 0);

    // Injected 3-period stall after wake 100: exactly one underrun (exclusive: and one recovery restart).
    // 288 frames (6 ms): the Focusrite's 144-frame minimum period glitches on its own now and then (see
    // docs/QUESTIONS.md), which would make "exactly one" depend on luck.
    SilentProbe probe2;
    st = runSilence(mode, 288, 1.5, 100, info, probe2);
    CHECK(st.outputUnderflows == 1);
    CHECK(st.restarts == (mode == ShareMode::Exclusive ? 1u : 0u));
  }
}

// ---- T6.7b: the capture FIFO ---------------------------------------------------------------------------------------

namespace {

// `frames` interleaved float frames of `ch` channels; channel c frame n = value(c, start + n).
std::vector<float> packet(uint32_t ch, uint64_t start, uint32_t frames) {
  std::vector<float> v(static_cast<size_t>(ch) * frames);
  for (uint32_t n = 0; n < frames; ++n)
    for (uint32_t c = 0; c < ch; ++c) v[static_cast<size_t>(n) * ch + c] = static_cast<float>(c * 100000 + start + n + 1);
  return v;
}

}  // namespace

TEST_CASE("T6.7b capture fifo: packets of 480 into periods of 128, bit-exact after priming at the target, with the "
          "capture time of every period's first frame; RtScope") {
  CaptureFifo f;
  const double rate = 48000.0;
  f.prepare(2, 4096, 480 + 128, 480 + 128 + 480 + 128, rate);
  CHECK(f.capacity() == 4096);
  std::vector<float> a(128), b(128);
  float* out[2] = {a.data(), b.data()};
  const uint64_t t0 = 10'000'000;  // capture time of frame 0 (100 ns)
  const uint64_t violations = pq::alloc_trap::violationCount();
  uint64_t pulled = 0;
  // 10 s: a packet every 480 frames; after each, the render takes 128-frame periods while it can.
  std::vector<std::vector<float>> packets;
  for (uint64_t k = 0; k < 1000; ++k) packets.push_back(packet(2, k * 480, 480));
  bool started = false;
  {
    pq::RtScope rt;
    for (uint64_t k = 0; k < 1000; ++k) {
      f.push(packets[k].data(), SampleFormat::Float32, 2, 480, t0 + k * 100'000);
      while (f.primed() ? f.fill() >= 128 : f.fill() >= f.target()) {
        const uint64_t qpc = f.pull(out, 128);
        started = started || f.primed();
        if (qpc != t0 + static_cast<uint64_t>(std::llround(static_cast<double>(pulled) * 1e7 / rate)))
          FAIL("capture time of frame " << pulled);
        for (uint32_t n = 0; n < 128; ++n)
          if (a[n] != static_cast<float>(pulled + n + 1) || b[n] != static_cast<float>(100000 + pulled + n + 1))
            FAIL("frame " << pulled + n << " differs");
        pulled += 128;
      }
    }
  }
  CHECK(pulled > 479000);
  CHECK(pq::alloc_trap::violationCount() == violations);
  CHECK(started);
  CHECK(f.underflows() == 0);
  CHECK(f.dropped() == 0);
}

TEST_CASE("T6.7b capture fifo: silence until primed; an underflow re-primes; a capture clock running ahead is dropped "
          "back to the target; a full FIFO keeps the newest frames") {
  CaptureFifo f;
  f.prepare(1, 1024, 256, 512, 48000.0);
  std::vector<float> o(128, 7.0f);
  float* out[1] = {o.data()};
  auto push = [&](uint64_t start, uint32_t n) {
    const auto p = packet(1, start, n);
    f.push(p.data(), SampleFormat::Float32, 1, n, 0);
  };
  push(0, 200);
  CHECK(f.pull(out, 128) == 0);  // below the target: silence, nothing taken
  CHECK_FALSE(f.primed());
  CHECK(o[0] == 0.0f);
  CHECK(f.fill() == 200);
  push(200, 100);
  f.pull(out, 128);  // primed: frames 0..127
  CHECK(f.primed());
  CHECK(o[0] == 1.0f);
  CHECK(o[127] == 128.0f);
  f.pull(out, 128);  // 128..255; 44 left
  CHECK(f.fill() == 44);
  f.pull(out, 128);  // an underflow: silence, the rest dropped, unprimed
  CHECK(f.underflows() == 1);
  CHECK_FALSE(f.primed());
  CHECK(f.fill() == 0);
  CHECK(o[5] == 0.0f);
  // Ahead: 700 frames buffered → after a pull of 128, 572 > 512: back to 256.
  push(1000, 700);
  f.pull(out, 128);
  CHECK(o[0] == 1001.0f);
  CHECK(f.fill() == 256);
  CHECK(f.dropped() == 44 + 316);
  f.pull(out, 128);
  CHECK(o[0] == 1001.0f + 128 + 316);  // continuous from the newest 256
  // Full: 1100 frames into 1024 → the oldest go.
  f.reset();
  push(5000, 1100);
  CHECK(f.fill() == 1024);
  f.pull(out, 128);
  CHECK(o[0] == 5000.0f + 76 + 1);
}

TEST_CASE("T6.7b capture fifo: int16 / packed int24 / 24-in-32 / int32 packets convert to float; channels the device "
          "lacks read silence; a silent packet is zeros") {
  for (SampleFormat fmt : {SampleFormat::Int16, SampleFormat::Int24, SampleFormat::Int24In32, SampleFormat::Int32}) {
    CAPTURE(sampleFormatName(fmt));
    CaptureFifo f;
    f.prepare(3, 64, 4, 8, 48000.0);
    // 2 device channels, 4 frames: values via planarToInterleaved (the render path's conversion), back through push.
    const std::vector<float> l = {0.5f, -0.25f, 0.0f, -1.0f}, r = {0.125f, -0.5f, 0.75f, 0.25f};
    const float* planes[2] = {l.data(), r.data()};
    std::vector<uint8_t> bytes(static_cast<size_t>(4) * 2 * sampleFormatBytes(fmt));
    planarToInterleaved(planes, 2, 2, 4, fmt, bytes.data());
    f.push(bytes.data(), fmt, 2, 4, 0);
    f.push(nullptr, fmt, 2, 4, 0);  // a silent packet
    std::vector<float> a(8), b(8), c(8, 9.0f);
    float* out[3] = {a.data(), b.data(), c.data()};
    f.pull(out, 8);
    for (uint32_t n = 0; n < 4; ++n) {
      CHECK(a[n] == l[n]);
      CHECK(b[n] == r[n]);
      CHECK(c[n] == 0.0f);
      CHECK(a[n + 4] == 0.0f);
      CHECK(b[n + 4] == 0.0f);
    }
  }
}

namespace {

// T6.7b opt-in: a duplex stream whose input is the default output captured in loopback; clicks of −60 dBFS every
// 4800 frames; records what the input saw per output frame.
struct ClickProbe {
  static constexpr uint32_t kEvery = 4800;
  std::vector<float> input = std::vector<float>(48000 * 4);
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> notRt{0};
  std::atomic<uint32_t> numIn{0};
  static void process(void* user, const AudioBlock& b) noexcept {
    auto& p = *static_cast<ClickProbe*>(user);
    const uint64_t f0 = p.frames.load(std::memory_order_relaxed);
    for (uint32_t c = 0; c < b.numOut; ++c)
      for (uint32_t i = 0; i < b.frames; ++i) b.out[c][i] = (f0 + i) % kEvery == 0 ? 0.001f : 0.0f;
    p.numIn.store(b.numIn, std::memory_order_relaxed);
    if (b.numIn > 0 && b.in != nullptr)
      for (uint32_t i = 0; i < b.frames && f0 + i < p.input.size(); ++i) p.input[f0 + i] = b.in[0][i];
    if (!pq::inRtScope()) p.notRt.fetch_add(1, std::memory_order_relaxed);
    p.frames.store(f0 + b.frames, std::memory_order_relaxed);
  }
};

}  // namespace

TEST_CASE("wasapi device (opt-in, PQ_TEST_AUDIO_DEVICE=1) T6.7b: shared duplex with loopback capture; a click comes "
          "back where the measured round trip says") {
  if (!deviceTestsEnabled()) {
    MESSAGE("skipped: set PQ_TEST_AUDIO_DEVICE=1 (plays -60 dBFS clicks on the default output for 3 s)");
    return;
  }
  auto list = WasapiDevice().enumerate();
  REQUIRE(list.ok());
  std::string out;
  for (const DeviceInfo& d : *list)
    if (d.isDefaultOutput) out = d.endpointId;
  REQUIRE(!out.empty());
  WasapiDevice dev;
  ClickProbe probe;
  StreamConfig c;
  c.shareMode = ShareMode::Shared;
  c.bufferFrames = 480;
  c.outputEndpointId = out;
  c.inputEndpointId = out;  // loopback: the input is what the output plays
  c.inputChannels = 1;
  auto opened = dev.open(c, {&ClickProbe::process, &probe});
  REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? "" : opened.error().message));
  CHECK(opened->inputError.empty());
  CHECK(opened->inputChannels == 1);
  CHECK(opened->inputDeviceChannels >= 1);
  MESSAGE("rate " << opened->sampleRate << ", period " << opened->bufferFrames << ", input latency estimate "
                  << opened->inputLatencyFrames);
  const uint64_t violations = pq::alloc_trap::violationCount();
  REQUIRE(dev.start().ok());
  std::this_thread::sleep_for(std::chrono::seconds(3));
  CHECK(dev.stop().ok());
  const auto st = dev.stats().snapshot();
  dev.close();
  CHECK(pq::alloc_trap::violationCount() == violations);
  CHECK(probe.notRt.load() == 0);
  CHECK(probe.numIn.load() == 1);
  REQUIRE(st.roundTripFrames > 0);
  // Every click k (output frame k·4800) after the FIFO primed shows up at input frame k·4800 + a constant lag, which the
  // timestamp measurement predicts to within 1 ms. (Loopback's capture time is when the audio engine mixed the frame,
  // the render clock's when the endpoint plays it: the two differ by the endpoint's own small latency, so an exact
  // match is not expected; measured on the dev machine: lag 1440, round trip 1424 at 48 kHz / 480.)
  const uint64_t n = std::min<uint64_t>(probe.frames.load(), probe.input.size());
  int checked = 0, off = 0;
  int64_t firstLag = -1;
  const int64_t tolerance = opened->sampleRate / 1000;
  for (uint64_t k = 4; (k + 1) * ClickProbe::kEvery < n; ++k) {
    uint64_t best = 0;
    float peak = 0.0f;
    for (uint64_t i = k * ClickProbe::kEvery; i < (k + 1) * ClickProbe::kEvery; ++i)
      if (std::fabs(probe.input[i]) > peak) peak = std::fabs(probe.input[i]), best = i;
    if (peak < 1e-4f) continue;  // primed later / a muted endpoint
    const int64_t lag = static_cast<int64_t>(best) - static_cast<int64_t>(k * ClickProbe::kEvery);
    ++checked;
    if (firstLag < 0) firstLag = lag;
    CHECK(lag == firstLag);  // sample-continuous at a constant delay
    if (std::llabs(lag - st.roundTripFrames) > tolerance) ++off;
    if (checked <= 3) MESSAGE("click " << k << ": lag " << lag << " frames, measured round trip " << st.roundTripFrames);
  }
  MESSAGE(checked << " clicks seen, " << off << " off the measured round trip by more than " << tolerance
                  << " frames; input drops " << st.inputDrops);
  CHECK(checked > 10);
  CHECK(off == 0);
}
