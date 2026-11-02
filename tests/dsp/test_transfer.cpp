// The transfer-function estimate (pq/dsp/transfer.h, TX.spectrum): measured vs exact for known filters, no curve on
// silence or on unrelated signals, allocation-free push.

#include <doctest/doctest.h>

#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/dsp/transfer.h"

using pq::dsp::TransferAnalyzer;
using pq::dsp::TransferConfig;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000.0;

// A deterministic white-noise source (xorshift; uniform in [−a, a]).
struct Noise {
  uint64_t s;
  explicit Noise(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  float next(float a) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return a * (static_cast<float>(s >> 40) / 8388608.0f - 1.0f);
  }
};

// A direct-form-I biquad (RBJ cookbook), double precision.
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  float run(float xf) {
    const double x = xf;
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return static_cast<float>(y);
  }
  double magDb(double hz) const {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * hz / kSr), z2 = z1 * z1;
    const std::complex<double> h = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    return 20.0 * std::log10(std::abs(h));
  }
  static Biquad make(double b0, double b1, double b2, double a0, double a1, double a2) {
    Biquad q;
    q.b0 = b0 / a0;
    q.b1 = b1 / a0;
    q.b2 = b2 / a0;
    q.a1 = a1 / a0;
    q.a2 = a2 / a0;
    return q;
  }
  static Biquad lowpass(double f, double Q) {
    const double w = 2 * kPi * f / kSr, c = std::cos(w), al = std::sin(w) / (2 * Q);
    return make((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
  }
  static Biquad highpass(double f, double Q) {
    const double w = 2 * kPi * f / kSr, c = std::cos(w), al = std::sin(w) / (2 * Q);
    return make((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
  }
  static Biquad peak(double f, double Q, double db) {
    const double A = std::pow(10.0, db / 40.0), w = 2 * kPi * f / kSr, c = std::cos(w), al = std::sin(w) / (2 * Q);
    return make(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
  }
};

std::vector<float> logAxis(uint32_t n, float lo, float hi) {
  std::vector<float> a(n);
  for (uint32_t i = 0; i < n; ++i) a[i] = lo * std::pow(hi / lo, static_cast<float>(i) / static_cast<float>(n - 1));
  return a;
}

// Runs `seconds` of noise through `f` (any callable float → float) into the analyser in 512-frame blocks.
template <class F>
void feed(TransferAnalyzer& t, F f, double seconds, uint64_t seed = 1, float amp = 0.5f) {
  Noise n(seed);
  std::vector<float> x(512), y(512);
  const uint32_t blocks = static_cast<uint32_t>(seconds * kSr / 512.0);
  for (uint32_t b = 0; b < blocks; ++b) {
    for (uint32_t i = 0; i < 512; ++i) {
      x[i] = n.next(amp);
      y[i] = f(x[i]);
    }
    t.push(x.data(), y.data(), 512);
  }
}

// Worst |measured − exact| (dB) over the confident points in [lo, hi] Hz, and the share of those points confident.
struct Fit {
  double worst = 0.0;
  double confidentShare = 0.0;
  double worstHz = 0.0;
};
Fit fit(const TransferAnalyzer& t, const Biquad& q, double lo, double hi) {
  Fit r;
  uint32_t in = 0, conf = 0;
  for (uint32_t p = 0; p < t.points(); ++p) {
    const double hz = t.pointHz(p);
    if (hz < lo || hz > hi) continue;
    ++in;
    if (t.confidence()[p] < 0.99f) continue;
    ++conf;
    const double e = std::fabs(t.magnitudeDb()[p] - q.magDb(hz));
    if (e > r.worst) {
      r.worst = e;
      r.worstHz = hz;
    }
  }
  r.confidentShare = in ? static_cast<double>(conf) / in : 0.0;
  return r;
}

}  // namespace

TEST_CASE("transfer: identity reads 0 dB, fully coherent") {
  const std::vector<float> axis = logAxis(512, 20.0f, 20000.0f);
  TransferConfig c;
  TransferAnalyzer t(c, axis);
  feed(t, [](float x) { return x; }, 0.5);
  // After half a second the main window shows; the long one (the lows) is still filling: nothing below its crossover.
  uint32_t early = 0, earlyLow = 0;
  for (uint32_t p = 0; p < t.points(); ++p) {
    if (axis[p] > 600.0f) early += t.confidence()[p] == 1.0f ? 1 : 0;
    if (axis[p] < 280.0f) earlyLow += t.confidence()[p] > 0.0f ? 1 : 0;
  }
  CHECK(early > 0);
  CHECK(earlyLow == 0);
  feed(t, [](float x) { return x; }, 1.5, 2);
  float worstConf = 1.0f, worstDb = 0.0f;
  for (uint32_t p = 0; p < t.points(); ++p) {
    if (axis[p] < 30.0f) continue;
    worstConf = std::min(worstConf, t.confidence()[p]);
    worstDb = std::max(worstDb, std::fabs(t.magnitudeDb()[p]));
  }
  CHECK(worstConf == doctest::Approx(1.0f));
  CHECK(worstDb < 1e-3f);
}

TEST_CASE("transfer: measured matches exact responses of known filters") {
  const std::vector<float> axis = logAxis(1024, 20.0f, 20000.0f);
  struct Case {
    const char* name;
    Biquad q;
    double tol;
  };
  const Case cases[] = {
      {"lowpass 1 kHz Q 0.707", Biquad::lowpass(1000, 0.707), 0.5},
      {"highpass 200 Hz Q 2", Biquad::highpass(200, 2.0), 0.75},  // 0.54 dB at 45 Hz, 28 dB down its 12 dB/oct slope
      {"peak 3 kHz Q 1 +12 dB", Biquad::peak(3000, 1.0, 12.0), 0.5},
      {"peak 120 Hz Q 4 -18 dB", Biquad::peak(120, 4.0, -18.0), 0.75},
  };
  for (const Case& cs : cases) {
    const std::string name = cs.name;
    CAPTURE(name);
    TransferAnalyzer t(TransferConfig{}, axis);
    Biquad q = cs.q;
    feed(t, [&](float x) { return q.run(x); }, 3.0);
    // Where the output is within 60 dB of the input (a filter's deep stop band is below the averages' noise floor
    // there, and coherence rightly drops), the estimate is exact to within the tolerance.
    Fit f = fit(t, cs.q, 40.0, 16000.0);
    MESSAGE(name << ": worst " << f.worst << " dB at " << f.worstHz << " Hz, " << 100.0 * f.confidentShare << " % confident");
    CHECK(f.worst <= cs.tol);
    CHECK(f.confidentShare > 0.6);
    // In the pass band every point is confident.
    uint32_t pass = 0, passConf = 0;
    for (uint32_t p = 0; p < t.points(); ++p) {
      const double hz = axis[p];
      if (hz < 40.0 || hz > 16000.0 || cs.q.magDb(hz) < -24.0) continue;
      ++pass;
      passConf += t.confidence()[p] > 0.99f ? 1 : 0;
    }
    CHECK(passConf == pass);
  }
}

TEST_CASE("transfer: a gain change of the device is followed within the averaging time") {
  const std::vector<float> axis = logAxis(256, 20.0f, 20000.0f);
  TransferAnalyzer t(TransferConfig{}, axis);
  feed(t, [](float x) { return x; }, 1.0);
  feed(t, [](float x) { return 0.5f * x; }, 5.0, 7);  // the lows average over 0.7 s
  float worst = 0.0f;
  for (uint32_t p = 0; p < t.points(); ++p)
    if (axis[p] > 50.0f) worst = std::max(worst, std::fabs(t.magnitudeDb()[p] + 6.0206f));
  CHECK(worst < 0.1f);
}

TEST_CASE("transfer: silence shows no curve") {
  const std::vector<float> axis = logAxis(512, 20.0f, 20000.0f);
  TransferAnalyzer t(TransferConfig{}, axis);
  std::vector<float> z(512, 0.0f);
  for (int i = 0; i < 400; ++i) t.push(z.data(), z.data(), 512);
  CHECK(t.hops() > 50);
  CHECK_FALSE(t.anyConfident());
  // Denormal-level input (dither far below the floor) neither.
  Noise n(3);
  std::vector<float> x(512), y(512);
  for (int b = 0; b < 200; ++b) {
    for (uint32_t i = 0; i < 512; ++i) x[i] = y[i] = n.next(1e-7f);
    t.push(x.data(), y.data(), 512);
  }
  CHECK_FALSE(t.anyConfident());
}

TEST_CASE("transfer: unrelated input and output show no curve") {
  const std::vector<float> axis = logAxis(512, 20.0f, 20000.0f);
  TransferAnalyzer t(TransferConfig{}, axis);
  Noise a(11), b(12);
  std::vector<float> x(512), y(512);
  for (int k = 0; k < 400; ++k) {
    for (uint32_t i = 0; i < 512; ++i) {
      x[i] = a.next(0.5f);
      y[i] = b.next(0.5f);  // a device that ignores its input (a generator)
    }
    t.push(x.data(), y.data(), 512);
  }
  uint32_t shown = 0;
  for (uint32_t p = 0; p < t.points(); ++p) shown += t.confidence()[p] > 0.0f ? 1 : 0;
  CHECK(shown <= t.points() / 100);  // ≤ 1 % of the points (single-bin lows) may flicker past the gate
}

TEST_CASE("transfer: a sine excites only its own frequency") {
  const std::vector<float> axis = logAxis(512, 20.0f, 20000.0f);
  TransferAnalyzer t(TransferConfig{}, axis);
  std::vector<float> x(512), y(512);
  Biquad q = Biquad::lowpass(1000, 0.707);
  uint64_t n = 0;
  for (int k = 0; k < 300; ++k) {
    for (uint32_t i = 0; i < 512; ++i, ++n) {
      x[i] = static_cast<float>(0.5 * std::sin(2 * kPi * 2000.0 * static_cast<double>(n) / kSr));
      y[i] = q.run(x[i]);
    }
    t.push(x.data(), y.data(), 512);
  }
  for (uint32_t p = 0; p < t.points(); ++p) {
    const double hz = axis[p];
    if (std::fabs(std::log2(hz / 2000.0)) > 0.5) CHECK(t.confidence()[p] == 0.0f);
  }
  // At the sine the measured gain is the filter's.
  uint32_t best = 0;
  for (uint32_t p = 1; p < t.points(); ++p)
    if (std::fabs(std::log(axis[p] / 2000.0)) < std::fabs(std::log(axis[best] / 2000.0))) best = p;
  CHECK(t.confidence()[best] > 0.99f);
  CHECK(t.magnitudeDb()[best] == doctest::Approx(q.magDb(2000.0)).epsilon(0.05));
}

TEST_CASE("transfer: push allocates nothing") {
  const std::vector<float> axis = logAxis(1024, 10.0f, 22000.0f);
  TransferAnalyzer t(TransferConfig{}, axis);
  std::vector<float> x(4096, 0.25f), y(4096, 0.125f);
  {
    pq::RtScope rt;
    for (int i = 0; i < 8; ++i) t.push(x.data(), y.data(), 4096);
  }
  CHECK(t.hops() > 0);
}

// ---- signal_tap (the analysis-tap sink the transfer estimate reads) --------------------------------------------------

#include "pq/core/tap_ring.h"
#include "pq/dsp/atoms.h"

TEST_CASE("signal_tap: nothing unwatched; watched, the four channels per call; Spl inputs repeated; 8 lanes summed") {
  using pq::dsp::Atom;
  pq::TapRing tap(pq::dsp::kSignalTapChannels, 4096, 1024);
  std::vector<float> a(256), b(256), c(256), d(256);
  for (uint32_t i = 0; i < 256; ++i) {
    a[i] = static_cast<float>(i);
    b[i] = -static_cast<float>(i);
    c[i] = 0.5f * static_cast<float>(i);
    d[i] = 2.0f;
  }
  const float spl[8] = {7, 7, 7, 7, 7, 7, 7, 7};
  pq::dsp::KernelArgs args;
  args.resource = &tap;
  args.frames = 256;
  const float* in[4] = {a.data(), b.data(), c.data(), d.data()};
  args.in = in;
  const pq::dsp::KernelFn k = pq::dsp::findKernel(Atom::SignalTap, 1, 0);
  REQUIRE(k != nullptr);
  {
    pq::RtScope rt;  // RT-safe: no allocation either way
    k(args);         // unwatched: one load, no copy
  }
  CHECK(tap.written() == 0);
  tap.addReader();
  {
    pq::RtScope rt;
    k(args);
  }
  REQUIRE(tap.written() == 256);
  std::vector<float> out(256);
  for (uint32_t ch = 0; ch < 4; ++ch) {
    REQUIRE(tap.read(ch, 0, 256, out.data()) == 256);
    const std::vector<float>& want = ch == 0 ? a : ch == 1 ? b : ch == 2 ? c : d;
    CHECK(out == want);
  }
  // Input 3 as a param-rate pattern (Spl): its value repeated.
  const pq::dsp::KernelFn kSpl = pq::dsp::findKernel(Atom::SignalTap, 1, 8u);
  REQUIRE(kSpl != nullptr);
  const float* in2[4] = {a.data(), b.data(), c.data(), spl};
  args.in = in2;
  kSpl(args);
  REQUIRE(tap.read(3, 256, 256, out.data()) == 256);
  for (const float v : out) CHECK(v == 7.0f);
  // A voice group: the lanes summed per frame.
  std::vector<float> lanes(64 * 8);
  for (uint32_t f = 0; f < 64; ++f)
    for (uint32_t l = 0; l < 8; ++l) lanes[f * 8 + l] = static_cast<float>(l + 1) * static_cast<float>(f);
  const float* in8[4] = {lanes.data(), lanes.data(), lanes.data(), lanes.data()};
  args.in = in8;
  args.frames = 64;
  const pq::dsp::KernelFn k8 = pq::dsp::findKernel(Atom::SignalTap, 8, 0);
  REQUIRE(k8 != nullptr);
  {
    pq::RtScope rt;
    k8(args);
  }
  REQUIRE(tap.read(0, 512, 64, out.data()) == 64);
  for (uint32_t f = 0; f < 64; ++f) CHECK(out[f] == 36.0f * static_cast<float>(f));
  tap.removeReader();
}
