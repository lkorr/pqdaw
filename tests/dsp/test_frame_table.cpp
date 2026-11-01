// Frame tables (TMM.1): layout and mip rule, the interpolator's response, the builder (spectra, spectral morph,
// crossfade, bake accuracy, no allocation once sized), the readers against the double reference, RT safety.
// The alias suite is in test_frame_table_alias.cpp.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "frame_table_fixtures.h"
#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/frame_table_build.h"
#include "../../src/dsp/src/fft_mixed.h"  // the builder's private mixed-radix FFT

using namespace pq::dsp;

namespace {

constexpr double kPi = std::numbers::pi;

uint32_t smoothPart(uint32_t n) {
  for (const uint32_t p : {2u, 3u, 5u, 7u})
    while (n % p == 0) n /= p;
  return n;
}

FrameTable buildOrFail(FrameTableBuilder& b, const FrameTableSpec& spec) {
  auto t = b.build(spec);
  REQUIRE_MESSAGE(t.ok(), (t.ok() ? "" : t.error().message));
  return std::move(t.value());
}

// Largest |c16 − c64| over every dense frame and harmonic, in dB (re full scale: an amplitude difference of 2|Δc|).
double bakeDifferenceDb(const std::vector<ftfix::Cycle>& cycles, FrameBake bake, uint32_t resolution = 16,
                        uint32_t reference = 64, uint32_t dense = 4) {
  FrameTableBuilder bLo, bHi;
  ftfix::SpecHolder lo(cycles, 48000, dense), hi(cycles, 48000, dense);
  lo.spec.bake = bake;
  lo.spec.bakeResolution = resolution;
  hi.spec.bake = bake;
  hi.spec.bakeResolution = reference;
  (void)buildOrFail(bLo, lo.spec);
  (void)buildOrFail(bHi, hi.spec);
  double worst = 0;
  for (uint32_t d = 0; d < dense; ++d) {
    const auto a = bLo.frameSpectrum(d), c = bHi.frameSpectrum(d);
    for (uint32_t h = 0; h < a.size(); ++h) worst = std::max(worst, 2.0 * std::abs(a[h] - c[h]));
  }
  return 20.0 * std::log10(std::max(worst, 1e-30));
}

std::vector<ftfix::Cycle> bakeCycles() {
  return {ftfix::basic(BasicCycle::Saw), ftfix::basic(BasicCycle::Square), ftfix::basic(BasicCycle::Triangle),
          ftfix::drawnFixture(1)[0]};
}

uint32_t resolutionFor(FrameBake bake) {
  FrameTableSpec spec;
  spec.bake = bake;
  return frameBakeResolution(spec);
}

}  // namespace

TEST_CASE("frame table layout: the sr − 20 kHz mip rule, 4× oversampled 7-smooth rows, memory") {
  for (const double sr : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0, 32000.0}) {
    CAPTURE(sr);
    auto l = frameTableLayout(sr);
    REQUIRE(l.ok());
    const auto& L = l.value();
    REQUIRE(L.levels >= 2);
    CHECK(L.top[0] == 1023);
    CHECK(L.top.back() == 1);
    CHECK(L.frameStride % 8 == 0);
    const double p = L.protectedHz, a = (sr - p) / sr;
    for (uint32_t j = 0; j < L.levels; ++j) {
      CAPTURE(j);
      CHECK(L.length[j] % 8 == 0);
      CHECK(L.length[j] >= 8 * L.top[j]);
      CHECK(smoothPart(L.length[j] / 8) == 1);
      CHECK(L.length[j] <= 8 * L.top[j] * 1.05 + 8);
      if (j > 0) CHECK(L.top[j] <= L.top[j - 1]);
      const double bj = L.b0 * std::pow(L.ratio, j);
      // Aliases: at the top of its range a level's highest harmonic folds no lower than P.
      CHECK(L.top[j] * bj * (1.0 + kFrameTableLevelEps) <= a * (1.0 + 1e-12));
      // Presence: at the bottom of its range (and with ε of estimate error) every harmonic up to 0.99·P is there.
      if (j > 0) {
        // The lowest increment of a level is below b_{j−1}·(1 + 1/128) (a bucket is at most 1/128 wide).
        const double fBottom = L.b0 * std::pow(L.ratio, j - 1.0) / (1.0 + 1.0 / 128) * (1.0 - kFrameTableLevelEps) * sr;
        if (j + 1 < L.levels || L.top[j] > 1) CHECK(std::floor(0.99 * p / fBottom) <= L.top[j]);
      }
    }
    // The lookup picks j a bucket below b_j and j + 1 just above (the bucket holding b_j takes j + 1).
    FrameTable t = FrameTable::allocate(L, 1).value();
    for (uint32_t j = 0; j + 1 < L.levels; ++j) {
      const double bj = L.b0 * std::pow(L.ratio, j);
      if (L.top[j + 1] == L.top[j]) continue;  // shared rows: either level is the same table
      CHECK(frameTableLevel(t.view(), static_cast<float>(bj * 0.99)) == j);  // buckets are ≤ 1/128 wide
      CHECK(frameTableLevel(t.view(), static_cast<float>(bj * 1.0001)) == j + 1);
    }
    CHECK(frameTableLevel(t.view(), 0.0f) == 0);
    CHECK(frameTableLevel(t.view(), std::nanf("")) == 0);
    CHECK(frameTableLevel(t.view(), 0.5f) == L.levels - 1);
    const double mib64 = 64.0 * L.frameStride * 4.0 / (1024.0 * 1024.0);
    MESSAGE("sr " << sr << ": " << L.levels << " levels, r = " << L.ratio << ", " << L.frameStride
                  << " floats/frame, 64 frames = " << mib64 << " MiB");
    if (sr == 48000.0) {
      auto t64 = FrameTable::allocate(L, 64);
      REQUIRE(t64.ok());
      CHECK(t64->bytes() <= 8u * 1024 * 1024);  // the plan's memory target
    }
  }
  CHECK_FALSE(frameTableLayout(7999.0).ok());
  CHECK_FALSE(frameTableLayout(std::nan("")).ok());
}

TEST_CASE("mixed-radix FFT (2, 3, 4, 5, 7) matches a naive DFT, one transform and four in AVX2 lanes") {
  using pq::dsp::detail::Cd;
  using pq::dsp::detail::Cd4;
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> d(-1, 1);
  for (const uint32_t n : {2u, 6u, 40u, 105u, 2048u, 1680u, 7840u}) {
    CAPTURE(n);
    pq::dsp::detail::MixedFft fft;
    REQUIRE(fft.init(n));
    std::vector<Cd> x(n), y(n), z(n);
    std::vector<Cd4> x4(n), y4(n);
    for (uint32_t i = 0; i < n; ++i) {
      x[i] = Cd(d(rng), d(rng));
      double* v = reinterpret_cast<double*>(&x4[i]);
      for (int l = 0; l < 4; ++l) {
        v[l] = x[i].real() * (l + 1);
        v[4 + l] = x[i].imag() * (l + 1);
      }
    }
    for (const bool inverse : {false, true}) {
      if (inverse) {
        fft.inverse(x.data(), y.data());
        fft.inverse(x4.data(), y4.data());
      } else {
        fft.forward(x.data(), y.data());
        fft.forward(x4.data(), y4.data());
      }
      const double sign = inverse ? 1.0 : -1.0;
      double worst = 0, worst4 = 0;
      for (uint32_t k = 0; k < n; k += n > 200 ? 37 : 1) {
        Cd acc = 0;
        for (uint32_t i = 0; i < n; ++i) acc += x[i] * std::polar(1.0, sign * 2.0 * kPi * double(i) * k / n);
        worst = std::max(worst, std::abs(acc - y[k]));
        const double* v = reinterpret_cast<const double*>(&y4[k]);
        for (int l = 0; l < 4; ++l) worst4 = std::max(worst4, std::abs(Cd(v[l], v[4 + l]) - acc * double(l + 1)));
      }
      CHECK(worst < 1e-9);
      CHECK(worst4 < 4e-9);
    }
  }
  pq::dsp::detail::MixedFft bad;
  CHECK_FALSE(bad.init(22));  // 11 is not a supported radix
}

TEST_CASE("frame interpolator: unit DC gain, ≤ 1 % passband droop, images ≥ 130 dB down (bands 1–3)") {
  CHECK(std::fabs(frameInterpResponse(0.0) - 1.0) < 1e-6);  // float coefficients
  double pmin = 2, pmax = 0;
  for (int i = 0; i <= 200; ++i) {
    const double h = frameInterpResponse(i / 1600.0);
    pmin = std::min(pmin, h);
    pmax = std::max(pmax, h);
  }
  CHECK(pmin >= 0.9899);
  CHECK(pmax <= 1.0005);
  double worst = 0;
  for (int m = 1; m <= 3; ++m)
    for (int i = -100; i <= 100; ++i) worst = std::max(worst, std::fabs(frameInterpResponse(m + i / 800.0)));
  const double db = 20.0 * std::log10(worst / pmin);
  MESSAGE("worst image (bands 1-3): " << db << " dB");
  CHECK(db <= -130.0);
}

TEST_CASE("frame interpolator: the impulse response read back through the reader matches the response formula") {
  // A table whose level-0 row is a single impulse: reading it at phases around the impulse gives h(t) directly, and
  // its continuous transform must equal frameInterpResponse (checks the reader's polynomial and the formula agree).
  auto l = frameTableLayout(48000.0);
  REQUIRE(l.ok());
  FrameTable t = FrameTable::allocate(l.value(), 1).value();
  const uint32_t len = l->length[0];
  float* row = t.mutableRow(0, 0);
  row[len / 2] = 1.0f;  // guards stay 0: the impulse is far from the ends
  const int steps = 6000;
  for (const double f : {0.0, 0.05, 0.125, 0.875, 1.1, 2.0}) {
    double q = 0;
    for (int s = 0; s < steps; ++s) {
      const double t0 = -3.0 + (s + 0.5) * 6.0 / steps;  // offset from the impulse, in samples
      const double ph = (len / 2 + t0) / len;
      q += readFrameTableReference(t.view(), ph, 0.0, 0) * std::cos(2.0 * kPi * f * t0) * 6.0 / steps;
    }
    CAPTURE(f);
    CHECK(std::fabs(q - frameInterpResponse(f)) < 2e-7);
  }
}

TEST_CASE("frame table builder: a sine keyframe is a sine at every level, through the reference reader") {
  FrameTableBuilder b;
  ftfix::SpecHolder s({ftfix::basic(BasicCycle::Sine)}, 48000, 1);
  FrameTable t = buildOrFail(b, s.spec);
  const auto c = b.frameSpectrum(0);
  REQUIRE(c.size() == 1024);
  CHECK(std::abs(c[1] - std::complex<double>(0, -0.5)) < 1e-8);
  for (uint32_t h = 2; h < c.size(); ++h) CHECK(std::abs(c[h]) < 1e-8);  // the float sine's rounding
  for (uint32_t j = 0; j < t.view().levels; ++j)
    for (int i = 0; i < 97; ++i) {
      const double ph = i / 97.0 + 0.0031;
      CHECK(std::fabs(readFrameTableReference(t.view(), ph, 0.0, j) - std::sin(2.0 * kPi * ph)) < 2e-6);
    }
}

TEST_CASE("frame table builder: spectral morph interpolates magnitude and phase (shorter arc); crossfade is linear") {
  // Harmonic 3: amplitude 1 at phase 0, then amplitude 0.5 at phase π/2. The middle dense frame of 3 is t = ½.
  const auto a = ftfix::fromHarmonics({{3, {1.0, 0.0}}});
  const auto c = ftfix::fromHarmonics({{3, {0.5, kPi / 2}}});
  for (const GapMode mode : {GapMode::Spectral, GapMode::Crossfade}) {
    FrameTableBuilder b;
    ftfix::SpecHolder s({a, c}, 48000, 3, mode);
    (void)buildOrFail(b, s.spec);
    const auto mid = b.frameSpectrum(1)[3];
    if (mode == GapMode::Spectral) {
      CHECK(std::fabs(2.0 * std::abs(mid) - 0.75) < 1e-8);
      CHECK(std::fabs(std::arg(mid) - kPi / 4) < 1e-8);
    } else {
      const std::complex<double> want = 0.5 * (std::polar(0.5, 0.0) + std::polar(0.25, kPi / 2));
      CHECK(std::abs(mid - want) < 1e-8);
    }
    // The ends are the keyframes exactly.
    CHECK(std::abs(b.frameSpectrum(0)[3] - std::polar(0.5, 0.0)) < 1e-8);
    CHECK(std::abs(b.frameSpectrum(2)[3] - std::polar(0.25, kPi / 2)) < 1e-8);
  }
  // Shorter arc across ±π: phases 0.9π and −0.9π meet at π, not at 0.
  FrameTableBuilder b;
  ftfix::SpecHolder s({ftfix::fromHarmonics({{2, {1.0, 0.9 * kPi}}}), ftfix::fromHarmonics({{2, {1.0, -0.9 * kPi}}})},
                      48000, 3);
  (void)buildOrFail(b, s.spec);
  const auto mid = b.frameSpectrum(1)[2];
  CHECK(std::fabs(std::fabs(std::arg(mid)) - kPi) < 1e-8);
  CHECK(std::fabs(2.0 * std::abs(mid) - 1.0) < 1e-8);
  // TMM.16: opposite phases up to the float cycles' rounding turn the same way, every harmonic: a saw's odd harmonics
  // and a square's are exactly opposite (both sampled cycles' phases are π/2 ∓ π/2 + πh/N), so halfway each is the
  // saw's phase + π/2 (counter-clockwise), not − π/2 for those whose rounding fell the other way.
  FrameTableBuilder b2;
  ftfix::SpecHolder s2({ftfix::basic(BasicCycle::Saw), ftfix::basic(BasicCycle::Square)}, 48000, 3);
  (void)buildOrFail(b2, s2.spec);
  const std::vector<std::complex<double>> saw(b2.frameSpectrum(0).begin(), b2.frameSpectrum(0).end());
  const auto m2 = b2.frameSpectrum(1);
  uint32_t turnedOtherWay = 0;
  for (uint32_t h = 1; h < 1024; h += 2)
    if (std::abs(std::remainder(std::arg(m2[h]) - std::arg(saw[h]) - kPi / 2, 2 * kPi)) > 1e-6) ++turnedOtherWay;
  CHECK(turnedOtherWay == 0u);
}

TEST_CASE("frame table builder: normalise, removeDc, holds before the first and after the last keyframe") {
  ftfix::Cycle half = ftfix::basic(BasicCycle::Saw);
  for (float& v : half) v = 0.5f * v + 0.25f;  // peak 0.75, DC 0.25
  FrameTableBuilder b;
  std::vector<Keyframe> keys = {{0.25f, half}, {0.75f, half}};
  FrameTableSpec spec;
  spec.keyframes = keys;
  spec.denseFrames = 5;
  spec.normalise = true;
  spec.removeDc = true;
  (void)buildOrFail(b, spec);
  float peak = 0;
  for (const float v : half) peak = std::max(peak, std::fabs(v));  // 0.75 − 1/2048: the last sample
  // Harmonic 1 of the sampled ±1 saw x_i = 2i/N − 1: |X_1|·2/N = 2 / (N·sin(π/N)).
  const double h1 = 0.5 * 2.0 / (2048.0 * std::sin(kPi / 2048.0)) / peak;
  for (uint32_t d = 0; d < 5; ++d) {
    CHECK(std::abs(b.frameSpectrum(d)[0]) == 0.0);
    CHECK(std::fabs(2.0 * std::abs(b.frameSpectrum(d)[1]) - h1) < 1e-8);
  }
}

TEST_CASE("frame table builder: a bake at 16× is within −110 dB of one at 64× (each control)") {
  const auto cycles = bakeCycles();
  struct Case {
    const char* name;
    FrameBake bake;
  };
  const Case cases[] = {
      {"bend +0.7", {WarpMode::Bend, 0.7f, 0, 0}},           {"bend -1", {WarpMode::Bend, -1.0f, 0, 0}},
      {"mirror 0.4", {WarpMode::Mirror, 0.4f, 0, 0}},         {"mirror -1", {WarpMode::Mirror, -1.0f, 0, 0}},
      {"pulse width 0.6", {WarpMode::PulseWidth, 0.6f, 0, 0}}, {"pulse width 1", {WarpMode::PulseWidth, 1.0f, 0, 0}},
      {"fold 0.05", {WarpMode::Bend, 0, 0.05f, 0}},           {"fold 1", {WarpMode::Bend, 0, 1.0f, 0}},
      {"sync 0.55", {WarpMode::Bend, 0, 0, 0.55f}},           {"sync 1", {WarpMode::Bend, 0, 0, 1.0f}},
  };
  for (const Case& c : cases) {
    const std::string name = c.name;
    CAPTURE(name);
    CHECK(resolutionFor(c.bake) == 16);  // single controls never raise the resolution
    const double db = bakeDifferenceDb(cycles, c.bake);
    MESSAGE("bake 16x vs 64x, " << name << ": " << db << " dB");
    CHECK(db <= -110.0);
  }
}

TEST_CASE("frame table builder: bake sweeps of every control, and combinations" * doctest::test_suite("slow")) {
  const auto cycles = bakeCycles();
  double worstSingle = -400;
  std::string worstName;
  auto single = [&](const char* what, FrameBake b, float v) {
    const double db = bakeDifferenceDb(cycles, b);
    if (db > worstSingle) worstSingle = db, worstName = std::string(what) + " " + std::to_string(v);
    CHECK(db <= -110.0);
  };
  for (int i = 0; i <= 8; ++i) {
    const float w = -1.0f + 0.25f * i, u = 0.125f * i;
    if (w != 0.0f) {
      single("bend", {WarpMode::Bend, w, 0, 0}, w);
      single("mirror", {WarpMode::Mirror, w, 0, 0}, w);
      single("pulse width", {WarpMode::PulseWidth, w, 0, 0}, w);
    }
    if (u != 0.0f) {
      single("fold", {WarpMode::Bend, 0, u, 0}, u);
      single("sync", {WarpMode::Bend, 0, 0, u}, u);
    }
  }
  MESSAGE("single-control sweeps, 16x vs 64x: worst " << worstSingle << " dB (" << worstName << ")");
  // Combinations: the builder's resolution (raised by frameBakeResolution) against a 128x bake. Within the 64x cap
  // they meet -110 dB; sync 1 + pulse width 1 (read slope 256) exceeds what an affordable resolution resolves and is
  // recorded only (QUESTIONS Q?-TMM.1-a).
  struct Combo {
    const char* name;
    FrameBake bake;
    bool bounded;
  };
  const Combo combos[] = {
      {"sync 0.3 + fold 0.3", {WarpMode::Bend, 0, 0.3f, 0.3f}, true},
      {"sync 0.3 + mirror 0.5", {WarpMode::Mirror, 0.5f, 0, 0.3f}, true},
      {"sync 0.3 + pw 0.5", {WarpMode::PulseWidth, 0.5f, 0, 0.3f}, true},
      {"bend 0.5 + fold 0.3", {WarpMode::Bend, 0.5f, 0.3f, 0}, true},
      {"pw 0.5 + fold 0.5", {WarpMode::PulseWidth, 0.5f, 0.5f, 0}, true},
      {"mirror 0.5 + fold 0.3 + sync 0.3", {WarpMode::Mirror, 0.5f, 0.3f, 0.3f}, true},
      {"sync 1 + pw 1", {WarpMode::PulseWidth, 1.0f, 0, 1.0f}, false},
      {"sync 0.5 + fold 1", {WarpMode::Bend, 0, 1.0f, 0.5f}, true},
  };
  for (const Combo& c : combos) {
    const std::string name = c.name;
    CAPTURE(name);
    const double db = bakeDifferenceDb(cycles, c.bake, 16, 128);
    MESSAGE("bake combination " << name << ": resolution " << resolutionFor(c.bake) << "x, vs 128x: " << db << " dB");
    if (c.bounded) CHECK(db <= -110.0);
  }
}

TEST_CASE("frame table builder: a baked hard sync of a sine matches its exact Fourier series") {
  // y(φ) = sin(2π·s·φ) on [0, 1) (sync of a sine: the wrap inside is continuous, the period end jumps).
  for (const float sync : {0.1f, 0.5f, 0.77f}) {
    const double s = 1.0 + 15.0 * static_cast<double>(sync);
    FrameTableBuilder b;
    ftfix::SpecHolder spec({ftfix::basic(BasicCycle::Sine)}, 48000, 1);
    spec.spec.bake.sync = sync;
    (void)buildOrFail(b, spec.spec);
    const auto c = b.frameSpectrum(0);
    double worst = 0;
    for (uint32_t h = 0; h < c.size(); ++h) {
      // ∫_0^1 sin(2πsφ) e^{−2πihφ} dφ = (1/2i)[∫ e^{2πi(s−h)φ} − ∫ e^{−2πi(s+h)φ}],  ∫_0^1 e^{2πiaφ} = (e^{2πia} − 1)/(2πia)
      auto e = [](double a) {  // (e^{2πia} − 1) / (2πia) = (sin 2πa + i(1 − cos 2πa)) / (2πa)
        if (std::abs(a) < 1e-12) return std::complex<double>(1.0);
        const double w = 2.0 * kPi * a;
        return std::complex<double>(std::sin(w) / w, (1.0 - std::cos(w)) / w);
      };
      const std::complex<double> d = e(s - h) - e(-(s + h));
      const std::complex<double> want(d.imag() / 2.0, -d.real() / 2.0);  // d / 2i
      worst = std::max(worst, 2.0 * std::abs(c[h] - want));
    }
    const double db = 20.0 * std::log10(worst);
    MESSAGE("sync " << sync << " of a sine vs exact series: " << db << " dB");
    CHECK(db <= -110.0);
  }
}

TEST_CASE("frame table readers match the double reference within 1e-6 (L1, L8, Spl and per-sample position)") {
  const auto fixture = ftfix::drawnFixture(8);
  std::vector<ftfix::Cycle> cycles = {ftfix::basic(BasicCycle::Saw), ftfix::basic(BasicCycle::Square)};
  cycles.insert(cycles.end(), fixture.begin(), fixture.end());
  for (const uint32_t dense : {1u, 64u}) {
    FrameTableBuilder b;
    ftfix::SpecHolder s(cycles, 48000, dense);
    FrameTable t = buildOrFail(b, s.spec);
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    for (const uint32_t lanes : {1u, 8u})
      for (const bool perSample : {false, true}) {
        CAPTURE(dense);
        CAPTURE(lanes);
        CAPTURE(perSample);
        constexpr uint32_t kFrames = 67;
        constexpr uint32_t kBlocks = 40;
        FrameReadState fast{}, ref{};
        std::vector<float> phase(kFrames * lanes), pos(kFrames * lanes), out(kFrames * lanes);
        std::vector<double> want(kFrames * lanes);
        std::vector<double> acc(lanes, 0.0), inc(lanes);
        double worst = 0;
        for (uint32_t blk = 0; blk < kBlocks; ++blk) {
          for (uint32_t l = 0; l < lanes; ++l) inc[l] = std::exp(std::log(20.0 / 48000) + uni(rng) * std::log(600.0));
          for (uint32_t n = 0; n < kFrames; ++n)
            for (uint32_t l = 0; l < lanes; ++l) {
              acc[l] += inc[l];
              acc[l] -= std::floor(acc[l]);
              phase[n * lanes + l] = static_cast<float>(acc[l]);
              pos[n * lanes + l] = perSample ? uni(rng) : 0.0f;
            }
          if (!perSample)
            for (uint32_t l = 0; l < 8 && l < pos.size(); ++l) pos[l] = blk % 5 == 0 ? 0.0f : uni(rng);
          if (lanes == 1) readFrameTableL1(t.view(), fast, phase.data(), pos.data(), perSample, kFrames, out.data());
          else readFrameTableL8(t.view(), fast, phase.data(), pos.data(), perSample, kFrames, out.data());
          readFrameTableReferenceBlock(t.view(), ref, phase.data(), pos.data(), perSample, lanes, kFrames, want.data());
          for (std::size_t i = 0; i < out.size(); ++i) worst = std::max(worst, std::fabs(out[i] - want[i]));
        }
        MESSAGE("dense " << dense << " lanes " << lanes << std::string(perSample ? " per-sample" : " spl") << ": max |fast − ref| = "
                         << worst);
        CHECK(worst <= 1e-6);
      }
  }
}

TEST_CASE("frame table readers: odd inputs stay finite and in range; one-frame tables ignore the position") {
  FrameTableBuilder b;
  ftfix::SpecHolder s({ftfix::basic(BasicCycle::Saw), ftfix::basic(BasicCycle::Sine)}, 44100, 16);
  FrameTable t = buildOrFail(b, s.spec);
  const float odd[] = {std::nanf(""), INFINITY, -INFINITY, -1e-9f, -3.25f, 1e7f, 0.9999999f, 1.0f, 0.0f, 2.5f};
  std::vector<float> phase(80), pos(80), out(80);
  for (int i = 0; i < 80; ++i) {
    phase[i] = odd[i % 10];
    pos[i] = odd[(i * 3) % 10];
  }
  FrameReadState st{};
  readFrameTableL8(t.view(), st, phase.data(), pos.data(), true, 10, out.data());
  for (const float v : out) CHECK(std::fabs(v) < 2.0f);  // NaN / inf phases read phase 0, NaN positions frame 0
  FrameReadState st1{};
  readFrameTableL1(t.view(), st1, phase.data(), pos.data(), true, 80, out.data());
  for (const float v : out) CHECK(std::fabs(v) < 2.0f);
}

TEST_CASE("frame table builder: buildInto allocates nothing once sized; readers allocate nothing") {
  const auto fixture = ftfix::drawnFixture(16);
  FrameTableBuilder b;
  ftfix::SpecHolder s(fixture, 48000, 64);
  s.spec.bake = {WarpMode::Mirror, 0.5f, 0.2f, 0.25f};
  FrameTable t = buildOrFail(b, s.spec);  // sizes the arenas
  s.spec.bake.fold = 0.25f;               // a different bake value at the same resolution: same arenas
  REQUIRE(frameBakeResolution(s.spec) == 16);
  CHECK(pq::alloc_trap::countRtViolations([&] { CHECK(b.buildInto(s.spec, t).ok()); }) == 0);
  s.spec.bake = {};
  CHECK(pq::alloc_trap::countRtViolations([&] { CHECK(b.buildInto(s.spec, t).ok()); }) == 0);
  std::vector<float> phase(64 * 8, 0.25f), pos(64 * 8, 0.3f), out(64 * 8);
  FrameReadState st{};
  CHECK(pq::alloc_trap::countRtViolations([&] {
          readFrameTableL1(t.view(), st, phase.data(), pos.data(), true, 61, out.data());
          readFrameTableL8(t.view(), st, phase.data(), pos.data(), false, 64, out.data());
        }) == 0);
}

TEST_CASE("frame table builder: buildFramesInto rebuilds only the frames listed over a clone (TMM.16 focused previews)") {
  // An earlier table A (one set of cycles) and the full build B of a later spec (one keyframe redrawn): rebuilding a
  // few frames of a clone of A from B's spec gives B's rows there (float rounding of the frame grouping) and A's
  // rows elsewhere, bit for bit. Every path: plain, the live-band bake, the full-band bake, Shape mode's arrays.
  auto fixture = ftfix::drawnFixture(5);
  struct Mode {
    const char* name;
    FrameBake bake;
    bool liveBand, antiderivative;
  };
  const Mode modes[] = {{"plain", {}, false, false},
                        {"live-band bake", {WarpMode::Mirror, -0.4f, 0.5f, 0.0f}, true, false},
                        {"full-band bake", {WarpMode::Bend, 0.3f, 0.2f, 0.1f}, false, false},
                        {"shape (antiderivative)", {WarpMode::Bend, 0.0f, 0.3f, 0.0f}, false, true}};
  for (const Mode& m : modes) {
    const std::string name = m.name;
    CAPTURE(name);
    ftfix::SpecHolder a(fixture, 48000, 16), b(fixture, 48000, 16);
    for (float& v : b.cycles[3]) v = -0.5f * v + 0.1f;  // keyframe 3 (at 0.75) redrawn
    b.keys[3].cycle = b.cycles[3];
    for (ftfix::SpecHolder* h : {&a, &b}) {
      h->spec.bake = m.bake;
      h->spec.liveBand = m.liveBand;
      h->spec.antiderivative = m.antiderivative;
    }
    FrameTableBuilder builder;
    const FrameTable ta = buildOrFail(builder, a.spec);
    const FrameTable tb = buildOrFail(builder, b.spec);
    auto c = ta.clone();
    REQUIRE(c.ok());
    FrameTable& tc = c.value();
    const std::vector<uint32_t> frames{2, 10, 11, 12};
    REQUIRE(builder.buildFramesInto(b.spec, tc, frames).ok());
    CHECK_FALSE(tc.isSine());
    double rebuilt = 0.0;
    bool keptExact = true, changed = false;
    for (uint32_t f = 0; f < 16; ++f) {
      const bool listed = std::find(frames.begin(), frames.end(), f) != frames.end();
      for (uint32_t j = 0; j < ta.layout().levels; ++j) {
        const auto rc = tc.row(f, j), ra = ta.row(f, j), rb = tb.row(f, j);
        for (size_t i = 0; i < rc.size(); ++i) {
          if (listed) {
            rebuilt = std::max(rebuilt, std::fabs(double(rc[i]) - rb[i]));
            changed = changed || ra[i] != rb[i];
          } else {
            keptExact = keptExact && rc[i] == ra[i];
          }
        }
        if (m.antiderivative) {
          const auto pc = tc.adRow(f, j), pa = ta.adRow(f, j), pb = tb.adRow(f, j);
          for (size_t i = 0; i < pc.size(); ++i) {
            if (listed) rebuilt = std::max(rebuilt, std::fabs(double(pc[i]) - pb[i]));
            else keptExact = keptExact && pc[i] == pa[i];
          }
        }
      }
    }
    MESSAGE(name << ": rebuilt frames vs a full build " << rebuilt);
    CHECK(changed);  // frames 10–12 lie in keyframe 3's gaps
    CHECK(rebuilt <= 1e-6);
    CHECK(keptExact);
    // The clone itself is an exact copy.
    auto c2 = tb.clone();
    REQUIRE(c2.ok());
    for (uint32_t f = 0; f < 16; ++f)
      for (uint32_t j = 0; j < tb.layout().levels; ++j) {
        const auto r1 = tb.row(f, j), r2 = c2.value().row(f, j);
        CHECK(std::equal(r1.begin(), r1.end(), r2.begin()));
      }
    CHECK(c2.value().isSine() == tb.isSine());
    // Allocation-free once sized; listing errors are refused.
    CHECK(pq::alloc_trap::countRtViolations([&] { CHECK(builder.buildFramesInto(b.spec, tc, frames).ok()); }) == 0);
    const std::vector<uint32_t> bad1{3, 3}, bad2{16};
    CHECK_FALSE(builder.buildFramesInto(b.spec, tc, bad1).ok());
    CHECK_FALSE(builder.buildFramesInto(b.spec, tc, bad2).ok());
  }
}

TEST_CASE("frame table builder: spec validation, hashing, basic cycles") {
  const auto saw = ftfix::basic(BasicCycle::Saw);
  FrameTableBuilder b;
  std::vector<Keyframe> keys = {{0.0f, saw}};
  FrameTableSpec spec;
  spec.keyframes = keys;
  CHECK(b.build(spec).ok());
  std::vector<float> shortCycle(100);
  std::vector<Keyframe> bad = {{0.0f, shortCycle}};
  spec.keyframes = bad;
  CHECK_FALSE(b.build(spec).ok());
  std::vector<Keyframe> unordered = {{0.5f, saw}, {0.25f, saw}};
  spec.keyframes = unordered;
  CHECK_FALSE(b.build(spec).ok());
  spec.keyframes = keys;
  spec.bake.warp = 2.0f;
  CHECK_FALSE(b.build(spec).ok());
  spec.bake = {};
  spec.denseFrames = 0;
  CHECK_FALSE(b.build(spec).ok());
  spec.denseFrames = 64;
  spec.bakeResolution = 12;
  CHECK_FALSE(b.build(spec).ok());
  spec.bakeResolution = 16;
  auto t = b.build(spec);
  REQUIRE(t.ok());
  FrameTableSpec other = spec;
  other.denseFrames = 32;
  CHECK_FALSE(b.buildInto(other, t.value()).ok());  // allocated for 64 frames

  const uint64_t h0 = frameTableSpecHash(spec);
  CHECK(frameTableSpecHash(spec) == h0);
  auto saw2 = saw;
  saw2[100] += 1e-6f;
  std::vector<Keyframe> keys2 = {{0.0f, saw2}};
  other = spec;
  other.keyframes = keys2;
  CHECK(frameTableSpecHash(other) != h0);
  other = spec;
  other.bake.fold = 0.1f;
  CHECK(frameTableSpecHash(other) != h0);
  other = spec;
  other.sampleRate = 44100;
  CHECK(frameTableSpecHash(other) != h0);

  for (const BasicCycle shape : {BasicCycle::Sine, BasicCycle::Triangle, BasicCycle::Saw, BasicCycle::Square,
                                 BasicCycle::Pulse25, BasicCycle::Noise}) {
    const auto c = basicCycle(shape);
    REQUIRE(c.size() == kKeyframeCycle);
    float peak = 0;
    for (const float v : c) peak = std::max(peak, std::fabs(v));
    CHECK(peak == doctest::Approx(1.0f));
  }
  const auto n1 = basicCycle(BasicCycle::Noise, 2048, 1), n2 = basicCycle(BasicCycle::Noise, 2048, 2);
  CHECK(n1[5] != n2[5]);
  CHECK(basicCycle(BasicCycle::Noise, 2048, 1)[5] == n1[5]);
  CHECK(basicCycle(BasicCycle::Pulse25)[511] == 1.0f);
  CHECK(basicCycle(BasicCycle::Pulse25)[512] == -1.0f);
}
