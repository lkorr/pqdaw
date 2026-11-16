// Band Levels and the Waveform's capture (pq/dsp/band_levels.h, waveform.h; VZ.6): a sine lands in its band, the
// crossover tree sums to an all-pass, the ballistics; the waveform's bins, its beat-locked loop and its free run.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/dsp/band_levels.h"
#include "pq/dsp/fft.h"
#include "pq/dsp/waveform.h"

using namespace pq::dsp;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double hz, double amp, uint32_t n) {
  std::vector<float> x(n);
  for (uint32_t i = 0; i < n; ++i) x[i] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * i / kSr));
  return x;
}

// The newest column's levels after `x`.
std::vector<float> levels(BandLevels& b, const std::vector<float>& x) {
  b.push(x.data(), x.data(), static_cast<uint32_t>(x.size()));
  std::vector<float> c(BandLevels::kBands);
  REQUIRE(b.columns() > 0);
  REQUIRE(b.read(b.columns() - 1, 1, c.data()) == 1);
  return c;
}

}  // namespace

TEST_CASE("band levels: a sine lands in its band (crossovers 3 / 4 / 5 and the independent 3) at its RMS") {
  struct Case {
    int bands;
    bool indep;
    std::vector<float> xo;
    double hz;
    int band;
  };
  const Case cases[] = {
      {3, false, {250, 4000}, 80.0, 0},         {3, false, {250, 4000}, 1000.0, 1},  {3, false, {250, 4000}, 12000.0, 2},
      {4, false, {120, 800, 5000}, 350.0, 1},   {4, false, {120, 800, 5000}, 2000.0, 2},
      {5, false, {60, 250, 2000, 8000}, 25.0, 0}, {5, false, {60, 250, 2000, 8000}, 125.0, 1},
      {5, false, {60, 250, 2000, 8000}, 700.0, 2}, {5, false, {60, 250, 2000, 8000}, 15000.0, 4},
      {3, true, {}, 30.0, 0}, {3, true, {}, 550.0, 1}, {3, true, {}, 16000.0, 2}};
  for (const Case& k : cases) {
    CAPTURE(k.bands);
    CAPTURE(k.indep);
    CAPTURE(k.hz);
    BandLevelsConfig c;
    c.bands = k.bands;
    c.independent3 = k.indep;
    c.columnRate = 5.0f;  // 200 ms columns: a steady RMS of the low tones too
    for (size_t i = 0; i < k.xo.size(); ++i) c.crossovers[i] = k.xo[i];
    BandLevels b(c);
    const auto lv = levels(b, sine(k.hz, 1.0, 96000));
    // A full-scale sine's RMS is −3.01 dB less what its band's filters take at that frequency: an LR4 section is
    // |H| = 1 / (1 + (f/fc)^4) (low-pass; high-pass with fc/f), band k is the high-passes of every crossover under it
    // and its own low-pass; the independent band-pass is unity at its centre, its Butterworth low / high-pass
    // 1 / sqrt(1 + (f/fc)^4); f/fc through the bilinear map (tan(pi f / fs) / tan(pi fc / fs): the designs are
    // pre-warped at fc). Every other band at least 15 dB under (independent: overlapping 12 dB/oct, at least 10).
    double expect = -3.0103;
    auto db = [](double g) { return 20.0 * std::log10(g); };
    auto ratio = [](double f, double fc) { return std::tan(kPi * f / kSr) / std::tan(kPi * fc / kSr); };
    if (k.indep) {
      if (k.band == 0) expect += db(1.0 / std::sqrt(1.0 + std::pow(ratio(k.hz, 100.0), 4.0)));
      if (k.band == 2) expect += db(1.0 / std::sqrt(1.0 + std::pow(ratio(5000.0, k.hz), 4.0)));
    } else {
      for (int j = 0; j < k.band; ++j) expect += db(1.0 / (1.0 + std::pow(ratio(k.xo[static_cast<size_t>(j)], k.hz), 4.0)));
      if (k.band < k.bands - 1) expect += db(1.0 / (1.0 + std::pow(ratio(k.hz, k.xo[static_cast<size_t>(k.band)]), 4.0)));
    }
    CHECK(std::fabs(lv[k.band] - expect) < 0.1);
    for (int o = 0; o < k.bands; ++o)
      if (o != k.band) CHECK(lv[o] < lv[k.band] - (k.indep ? 10.0f : 15.0f));
    for (int o = k.bands; o < BandLevels::kBands; ++o) CHECK(lv[o] == BandLevels::kFloorDb);
  }
}

TEST_CASE("band levels: the crossover tree sums to an all-pass (flat magnitude) at 3, 4 and 5 bands, 48 – 192 kHz") {
  for (const double sr : {48000.0, 96000.0, 192000.0})
    for (int n : {3, 4, 5}) {
      CAPTURE(sr);
      CAPTURE(n);
      BandLevelsConfig c;
      c.sampleRate = sr;
      c.bands = n;
      c.independent3 = false;
      const float xo[3][4] = {{250, 4000}, {120, 800, 5000}, {60, 250, 2000, 8000}};
      for (int i = 0; i < n - 1; ++i) c.crossovers[i] = xo[n - 3][i];
      BandSplit split;
      split.configure(c, /*compensate=*/true);
      const auto N = static_cast<uint32_t>(16384 * (sr / 48000.0));  // the 60 Hz sections' tails at every rate
      std::vector<float> h(N);
      float out[BandSplit::kBands];
      for (uint32_t i = 0; i < N; ++i) {
        split.process(i == 0 ? 1.0f : 0.0f, out);
        float s = 0.0f;
        for (int b = 0; b < n; ++b) s += out[b];
        h[i] = s;
      }
      RealFft fft(N);
      std::vector<float> pw(N / 2 + 1);
      fft.power(h.data(), pw.data());
      float worst = 0.0f;
      const auto top = static_cast<uint32_t>(20000.0 / sr * N);  // the audible band (the top's float tail aside)
      for (uint32_t k = 1; k < top; ++k) worst = std::max(worst, std::fabs(10.0f * std::log10(pw[k])));
      MESSAGE(n << " bands at " << sr << " Hz: the sum's magnitude within " << worst << " dB of flat");
      CHECK(worst < 0.01f);  // double sections (M9: float ones were several % off at 192 kHz, 60 Hz)
    }
}

TEST_CASE("band levels: ballistics (VU 300 ms both ways, Peak fast up and slow down), silence falls, rest") {
  BandLevelsConfig c;
  c.ballistics = BandLevelsConfig::Ballistics::Peak;
  BandLevels b(c);
  (void)levels(b, sine(550.0, 1.0, 24000));
  const float up = b.level(1);
  CHECK(std::fabs(up + 3.01f) < 0.1f);  // the band-pass is unity at its centre
  b.pushSilence(48000 / 4);  // 250 ms: Peak falls slowly (650 ms)
  const float peakFall = b.level(1);
  c.ballistics = BandLevelsConfig::Ballistics::Vu;
  BandLevels v(c);
  (void)levels(v, sine(550.0, 1.0, 24000));
  v.pushSilence(48000 / 4);
  CHECK(v.level(1) < peakFall);  // VU falls faster than Peak's release
  CHECK_FALSE(v.atRest());
  v.pushSilence(48000 * 30);
  CHECK(v.atRest());
  CHECK(v.columns() > 60 * 30);
}

TEST_CASE("band levels: push never allocates (the worker, RtScope)") {
  BandLevelsConfig c;
  c.bands = 5;
  c.independent3 = false;
  c.crossovers[0] = 60;
  c.crossovers[1] = 250;
  c.crossovers[2] = 2000;
  c.crossovers[3] = 8000;
  BandLevels b(c);
  const auto x = sine(440.0, 0.5, 8192);
  CHECK(pq::alloc_trap::countRtViolations([&] { b.push(x.data(), x.data(), 8192); }) == 0);
}

TEST_CASE("band levels: the worker's cost per second of audio (5 bands)" * doctest::test_suite("perf")) {
  BandLevelsConfig c;
  c.bands = 5;
  c.independent3 = false;
  c.crossovers[0] = 60;
  c.crossovers[1] = 250;
  c.crossovers[2] = 2000;
  c.crossovers[3] = 8000;
  BandLevels b(c);
  const auto x = sine(440.0, 0.5, 48000);
  b.push(x.data(), x.data(), 48000);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 10; ++i) b.push(x.data(), x.data(), 48000);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 10.0;
  MESSAGE("band levels (5 bands): " << ms << " ms per second of stereo audio");
  CHECK(ms > 0.0);
}

TEST_CASE("waveform capture: bins hold each channel's extremes and energy; the brightness splits at 700 Hz") {
  WaveCapture w;
  const auto lo = sine(100.0, 0.5, 128 * 64), hi = sine(5000.0, 0.5, 128 * 64);
  std::vector<float> zero(lo.size(), 0.0f);
  w.push(nullptr, lo.data(), zero.data(), static_cast<uint32_t>(lo.size()));
  w.push(nullptr, hi.data(), hi.data(), static_cast<uint32_t>(hi.size()));
  REQUIRE(w.bins() == 128);
  std::vector<WaveBin> b(128);
  REQUIRE(w.readBins(0, 128, b.data()) == 128);
  const WaveBin& a = b[40];   // 100 Hz in L only
  const WaveBin& c = b[100];  // 5 kHz in both
  CHECK(a.n == 128);
  float mxL = 0.0f, mxR = 0.0f, mxM = 0.0f;
  for (int i = 8; i < 64; ++i) {  // (a bin is a quarter of a 100 Hz period: the extremes over many)
    mxL = std::max(mxL, b[static_cast<size_t>(i)].mx[0]);
    mxR = std::max(mxR, b[static_cast<size_t>(i)].mx[1]);
    mxM = std::max(mxM, b[static_cast<size_t>(i)].mx[2]);
  }
  CHECK(mxL == doctest::Approx(0.5f).epsilon(0.01));
  CHECK(mxR == 0.0f);
  CHECK(mxM == doctest::Approx(0.25f).epsilon(0.01));  // mid = (L + R) / 2
  CHECK(a.hs[0] < 0.05f * a.ss[0]);   // a low tone: little above 700 Hz
  CHECK(c.hs[0] > 0.8f * c.ss[0]);    // a high tone: mostly above
  CHECK(c.mx[3] == 0.0f);             // L = R: no side
}

TEST_CASE("waveform capture: the loop locks to the tap's beat (a slot per phase); no clock: the free run") {
  WaveConfig cfg;
  cfg.loop = true;
  cfg.loopBeats = 4.0;
  WaveCapture w(cfg);
  // 120 bpm: 2 beats a second. One bar of a tone that is loud only in its first beat.
  const uint32_t n = 48000 * 2;
  std::vector<float> beat(n), x(n);
  for (uint32_t i = 0; i < n; ++i) {
    const double b = 8.0 + static_cast<double>(i) * 2.0 / kSr;  // from bar 3
    beat[i] = static_cast<float>(b);
    x[i] = static_cast<float>((std::fmod(b, 4.0) < 1.0 ? 0.8 : 0.1) * std::sin(2.0 * kPi * 220.0 * i / kSr));
  }
  for (uint32_t i = 0; i < n; i += 4096) {
    const uint32_t k = std::min<uint32_t>(4096, n - i);
    w.push(beat.data() + i, x.data() + i, x.data() + i, k);
  }
  std::vector<WaveBin> slots(WaveCapture::kLoopSlots);
  std::vector<uint8_t> filled(WaveCapture::kLoopSlots, 0);
  WaveLoopState st;
  const uint64_t stamp = w.readLoop(0, slots.data(), filled.data(), st);
  CHECK(st.clocked);
  CHECK(st.bpm == doctest::Approx(120.0).epsilon(1e-3));
  CHECK(st.spanSamples == doctest::Approx(96000.0).epsilon(1e-3));
  CHECK(st.phase == doctest::Approx((std::fmod(beat[n - 1], 4.0f)) / 4.0).epsilon(1e-3));
  // The first quarter of the slots (beat 1) is loud, the rest quiet: the picture sits on the bar.
  const auto peakAt = [&](double phase) {  // the peak over 32 slots from `phase` (a slot is a fifth of a 220 Hz period)
    float m = 0.0f;
    for (int k = 0; k < 32; ++k) m = std::max(m, slots[static_cast<size_t>(phase * WaveCapture::kLoopSlots) + static_cast<size_t>(k)].mx[2]);
    return m;
  };
  CHECK(peakAt(0.1) > 0.7f);
  CHECK(peakAt(0.5) < 0.15f);
  CHECK(peakAt(0.9) < 0.15f);
  for (size_t s = 0; s < filled.size(); ++s) CHECK(filled[s] == 1);
  // Only what changed is copied again.
  std::fill(filled.begin(), filled.end(), uint8_t{0});
  CHECK(w.readLoop(stamp, slots.data(), filled.data(), st) == stamp);
  CHECK(std::count(filled.begin(), filled.end(), uint8_t{1}) == 0);
  // No clock (NaN beat): the free run over loopSeconds.
  std::vector<float> nan(4800, NAN);
  w.push(nan.data(), x.data(), x.data(), 4800);
  (void)w.readLoop(stamp, slots.data(), filled.data(), st);
  CHECK_FALSE(st.clocked);
  CHECK(st.spanSamples == doctest::Approx(2.0 * kSr));
  CHECK(std::count(filled.begin(), filled.end(), uint8_t{1}) > 0);
}

TEST_CASE("waveform capture: the tempo late in a song (beat 4000, a float beat lane) reads within 0.05 %") {
  for (const double bpm : {120.0, 87.5}) {
    CAPTURE(bpm);
    WaveConfig c;
    c.loop = true;
    WaveCapture w(c);
    const uint32_t n = 48000 * 3;
    std::vector<float> beat(n), x(n, 0.1f);
    for (uint32_t i = 0; i < n; ++i) beat[i] = static_cast<float>(4000.0 + static_cast<double>(i) * bpm / 60.0 / kSr);
    for (uint32_t at = 0; at < n; at += 512) w.push(beat.data() + at, x.data() + at, x.data() + at, std::min(512u, n - at));
    std::vector<WaveBin> slots(WaveCapture::kLoopSlots);
    std::vector<uint8_t> filled(WaveCapture::kLoopSlots);
    WaveLoopState st;
    (void)w.readLoop(0, slots.data(), filled.data(), st);
    CHECK(st.bpm == doctest::Approx(bpm).epsilon(5e-4));  // (one chunk's float difference: ~1 %, L10)
  }
}

TEST_CASE("waveform capture: push never allocates (the worker, RtScope)") {
  WaveConfig cfg;
  cfg.loop = true;
  WaveCapture w(cfg);
  const auto x = sine(440.0, 0.5, 8192);
  std::vector<float> beat(8192);
  for (size_t i = 0; i < beat.size(); ++i) beat[i] = static_cast<float>(i) * 2.0f / 48000.0f;
  CHECK(pq::alloc_trap::countRtViolations([&] {
          w.push(beat.data(), x.data(), x.data(), 8192);
          w.pushSilence(4096);
        }) == 0);
}
