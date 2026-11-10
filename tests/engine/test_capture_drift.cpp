// T6.x-fix-ui-b (M6 review M7): the capture FIFO under clock drift between two WASAPI devices — the review's probe
// (docs/reviews/M6/recording/probe_capture_drift.cpp) as a test. Capture packets of 480 frames are pushed at the
// capture clock (1 + ppm), periods pulled at the render clock, with the target / high WasapiDevice::Impl::openCapture
// uses (drift reserve included). The capture timestamps and play times are QPC with jitter. The signal is a quadrature
// pair (sin, cos) at an irrational frequency, so every output frame gives the input position it was read at: a drop,
// an inserted silence or a phase jump shows as a step ≠ the ratio, or as an amplitude ≠ 1. The delay a frame makes
// (play time − capture time) is the timing alignment the recorder relies on (it takes the round trip once per session).
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/rt_scope.h"
#include "pq/engine/capture_fifo.h"

using namespace pq::engine;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

struct DriftResult {
  uint64_t dropped = 0, underflows = 0, silentAfterStart = 0;
  double maxStepError = 0.0;   // max |Δposition − ratio| between consecutive output frames, input frames
  double maxAmpError = 0.0;    // max |amplitude − 1|
  double maxDelayDev = 0.0;    // max |delay − first delay|, frames (the timing alignment)
  double finalDelayDev = 0.0;  // the same over the last second
  double ratio = 1.0;
  bool resampled = false;
  double nsPerPull = 0.0;  // while resampling (2 channels)
  uint64_t rtViolations = 0;
};

// `seconds` of render periods of `period` frames; capture packets of `capPeriod` at 1 + ppm·1e−6 times the render
// rate. `timestamps`: pull() gets play times (the device always has them); jitter: uniform ± on each QPC time.
DriftResult simulate(double ppm, double seconds, uint32_t period, uint32_t capPeriod, bool timestamps,
                     double jitterUs = 200.0) {
  DriftResult res;
  const uint32_t capBuffer = 4800, channels = 2;
  CaptureFifo fifo;
  const uint32_t target = capPeriod + period + CaptureFifo::driftReserve(kRate);
  fifo.prepare(channels, 2 * capBuffer + 4 * (capPeriod + period), target, target + capPeriod + period, kRate);
  const double ratio = 1.0 + ppm * 1e-6;
  const double omega = 2.0 * kPi * 1000.0 * std::sqrt(2.0) / kRate;  // ≈ 1414 Hz: no frame jump hides in a cycle
  std::mt19937_64 rng(static_cast<uint64_t>(ppm * 1000.0) + period);
  std::uniform_real_distribution<double> jitter(-jitterUs * 10.0, jitterUs * 10.0);  // 100 ns units
  const double t0 = 1e9, latency = 0.02 * 1e7;                                      // QPC start; output latency

  std::vector<float> packet(static_cast<size_t>(capPeriod) * channels), a(period), b(period);
  float* outs[2] = {a.data(), b.data()};
  double zr = 1.0, zi = 0.0;  // e^{iωn} for the next capture frame n
  const double cr = std::cos(omega), ci = std::sin(omega);
  double capClock = 0.0;
  uint64_t pushed = 0;
  const uint64_t periods = static_cast<uint64_t>(seconds * kRate / period);
  bool started = false;
  double pos = 0.0, lastPhase = 0.0, firstDelay = 0.0;
  bool havePrev = false;
  std::chrono::nanoseconds resampleTime{0};
  uint64_t resamplePulls = 0;
  const uint64_t violations = pq::alloc_trap::violationCount();
  {
    pq::RtScope rt;
    for (uint64_t k = 0; k < periods; ++k) {
      capClock += period * ratio;
      while (pushed + capPeriod <= capClock) {
        for (uint32_t i = 0; i < capPeriod; ++i) {
          packet[2 * i] = static_cast<float>(zi);
          packet[2 * i + 1] = static_cast<float>(zr);
          const double r = zr * cr - zi * ci;
          zi = zr * ci + zi * cr;
          zr = r;
        }
        const double g = 1.0 / std::sqrt(zr * zr + zi * zi);  // keep |z| = 1
        zr *= g;
        zi *= g;
        const double tCap = t0 + static_cast<double>(pushed) / (kRate * ratio) * 1e7 + jitter(rng);
        fifo.push(packet.data(), SampleFormat::Float32, channels, capPeriod, static_cast<uint64_t>(tCap));
        pushed += capPeriod;
      }
      const double tPlay = t0 + latency + static_cast<double>(k * period) / kRate * 1e7;
      const bool wasResampling = fifo.resampling();
      const double used = fifo.ratio();  // the ratio this pull reads at (the loop updates it after)
      const auto c0 = std::chrono::steady_clock::now();
      (void)fifo.pull(outs, period, timestamps ? static_cast<uint64_t>(tPlay + jitter(rng) * 0.25) : 0);
      const auto c1 = std::chrono::steady_clock::now();
      if (wasResampling) {
        resampleTime += c1 - c0;
        ++resamplePulls;
      }
      if (!fifo.primed()) {
        if (started) ++res.silentAfterStart;
        continue;
      }
      for (uint32_t i = 0; i < period; ++i) {
        const double amp = std::hypot(static_cast<double>(a[i]), static_cast<double>(b[i]));
        res.maxAmpError = std::max(res.maxAmpError, std::fabs(amp - 1.0));
        const double phase = std::atan2(static_cast<double>(a[i]), static_cast<double>(b[i]));
        if (havePrev) {
          double dphi = phase - lastPhase;
          dphi -= 2.0 * kPi * std::floor(dphi / (2.0 * kPi) + 0.5);
          const double step = dphi / omega;
          res.maxStepError = std::max(res.maxStepError, std::fabs(step - used));
          pos += step;
        }
        // The first frame read after priming is capture frame 0 (nothing was dropped before it).
        havePrev = true;
        lastPhase = phase;
        // The delay of this frame: when it plays − when it was captured (without jitter), in render frames.
        const double play = static_cast<double>(k * period + i);
        const double delay = play - pos / ratio;
        if (!started) firstDelay = delay;
        started = true;
        const double dev = std::fabs(delay - firstDelay);
        res.maxDelayDev = std::max(res.maxDelayDev, dev);
        if (static_cast<double>(k * period) > (seconds - 1.0) * kRate) res.finalDelayDev = std::max(res.finalDelayDev, dev);
      }
    }
  }
  res.rtViolations = pq::alloc_trap::violationCount() - violations;
  res.dropped = fifo.dropped();
  res.underflows = fifo.underflows();
  res.ratio = fifo.ratio();
  res.resampled = fifo.resampling();
  if (resamplePulls > 0) res.nsPerPull = static_cast<double>(resampleTime.count()) / static_cast<double>(resamplePulls);
  return res;
}

void checkClean(const DriftResult& r, double ppm) {
  CHECK(r.rtViolations == 0);
  CHECK(r.dropped == 0);
  CHECK(r.underflows == 0);
  CHECK(r.silentAfterStart == 0);
  CHECK(r.maxAmpError < 1e-3);          // never a silent / half-silent frame (the sinc is flat ±0.001 dB here)
  CHECK(r.maxStepError < 2e-3);         // every frame continues the last one (a 1-frame jump would be ≥ 1)
  CHECK(r.maxDelayDev < 48.0);          // the alignment never moves by more than 1 ms
  CHECK(r.finalDelayDev < 6.0);         // and is back where it started (the jitter moves it by a few frames)
  CHECK(std::fabs(r.ratio - (1.0 + ppm * 1e-6)) < 2e-5);  // the loop found the clock ratio
}

}  // namespace

TEST_CASE("T6.x-fix-ui-b capture drift: one clock (an interface's in and out), with timestamps and jitter, is a plain "
          "copy: never resampled, bit-exact, 480 packets into 128 periods") {
  CaptureFifo f;
  f.prepare(1, 2 * 4800 + 4 * (480 + 128), 480 + 128, 2 * (480 + 128), kRate);
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<double> jitter(-2000.0, 2000.0);  // ±200 µs
  std::vector<float> p(480), o(128);
  float* out[1] = {o.data()};
  uint64_t pushed = 0, pulled = 0, bad = 0;
  const uint64_t violations = pq::alloc_trap::violationCount();
  {
    pq::RtScope rt;
    for (uint64_t k = 0; k < 60 * 375; ++k) {  // 60 s of 128-frame periods
      while (pushed + 480 <= (k + 1) * 128) {
        for (uint32_t i = 0; i < 480; ++i) p[i] = static_cast<float>(pushed + i + 1);
        f.push(p.data(), SampleFormat::Float32, 1, 480,
               static_cast<uint64_t>(1e9 + static_cast<double>(pushed) / kRate * 1e7 + jitter(rng)));
        pushed += 480;
      }
      f.pull(out, 128, static_cast<uint64_t>(1e9 + 2e5 + static_cast<double>(k * 128) / kRate * 1e7 + jitter(rng)));
      if (!f.primed()) continue;
      for (uint32_t i = 0; i < 128; ++i)
        if (o[i] != static_cast<float>(pulled + i + 1)) ++bad;
      pulled += 128;
    }
  }
  CHECK(pq::alloc_trap::violationCount() == violations);
  CHECK(pulled > 59 * 48000);
  CHECK(bad == 0);
  CHECK_FALSE(f.resampling());
  CHECK(f.ratio() == 1.0);
  CHECK(std::fabs(f.delayError()) < f.kEngageSeconds * kRate);
  CHECK(f.dropped() == 0);
  CHECK(f.underflows() == 0);
}

TEST_CASE("T6.x-fix-ui-b capture drift: +200 ppm for 60 s: the loop engages and nothing is dropped; RtScope") {
  const DriftResult r = simulate(+200.0, 60.0, 480, 480, true);
  MESSAGE("ns per 480-frame stereo pull while resampling: " << r.nsPerPull);
  CHECK(r.resampled);
  checkClean(r, +200.0);
}

TEST_CASE("T6.x-fix-ui-b capture drift: without play times there is no tracking (the device always passes them): "
          "+100 ppm drops a packet's worth back to the target, as before") {
  const DriftResult r = simulate(+100.0, 600.0, 480, 480, false);
  CHECK_FALSE(r.resampled);
  CHECK(r.dropped > 0);
}

TEST_CASE("T6.x-fix-ui-b capture drift: 10 min at ±20, ±100, ±200 ppm: 0 drops, 0 underflows, the sine continuous, "
          "the alignment within 1 ms" * doctest::test_suite("slow")) {
  for (double ppm : {+20.0, -20.0, +100.0, -100.0, +200.0, -200.0}) {
    CAPTURE(ppm);
    const DriftResult r = simulate(ppm, 600.0, 480, 480, true);
    MESSAGE(ppm << " ppm: max step error " << r.maxStepError << ", max amplitude error " << r.maxAmpError
                << ", max delay deviation " << r.maxDelayDev << " frames (last second " << r.finalDelayDev
                << "), ratio " << r.ratio << ", ns per pull " << r.nsPerPull);
    CHECK(r.resampled);
    checkClean(r, ppm);
  }
  // 128-frame periods (an IAudioClient3 render period) against 480-frame capture packets.
  const DriftResult r = simulate(-200.0, 600.0, 128, 480, true);
  CHECK(r.resampled);
  checkClean(r, -200.0);
}
