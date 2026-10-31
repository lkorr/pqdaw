// T5.4: the polyphase windowed-sinc resampler (pq/dsp/resample.h).
//   - the prototype's response: passband ±0.1 dB to 20 kHz at 48 kHz, stopband from Nyquist ≤ −80 dB
//   - every table row has a DC gain of exactly 1; q follows the rate
//   - full-scale sines read at transposes −24 … +24 semitones (and file-rate conversions): the expected sine within
//     ±0.1 dB in the passband, everything else (images, aliases, interpolation error) ≤ −80 dBFS; the ±48 st extremes
//     measured and printed
//   - reads stay inside sincReach; reverse reads mirror forward reads; the straight copies; RtScope
//   - T5.x-perf-audio (Q151 item 1): the mip decimator's response (flat to 0.21, below −110 dB from 0.25 cycles per
//     input frame); sines read at +13 … +48 st through 2^L× decimated copies meet the same criteria as direct reads
#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/dsp/resample.h"

using namespace pq::dsp;

namespace {

constexpr double kSr = 48000.0;

int64_t fixedOf(double v) { return static_cast<int64_t>(std::llround(v * 4294967296.0)); }

// Least-squares fit of a·cos(w n) + b·sin(w n) + c to y; returns the amplitude and the residual RMS.
struct Fit {
  double amplitude = 0.0;
  double residualRms = 0.0;
  double rms = 0.0;
};
Fit fitSine(const std::vector<double>& y, double w) {
  double scc = 0, sss = 0, ssc = 0, sc = 0, ss = 0, n = static_cast<double>(y.size());
  double yc = 0, ys = 0, y1 = 0;
  for (size_t i = 0; i < y.size(); ++i) {
    const double c = std::cos(w * static_cast<double>(i)), s = std::sin(w * static_cast<double>(i));
    scc += c * c, sss += s * s, ssc += s * c, sc += c, ss += s;
    yc += y[i] * c, ys += y[i] * s, y1 += y[i];
  }
  // Normal equations for [a b c].
  double m[3][4] = {{scc, ssc, sc, yc}, {ssc, sss, ss, ys}, {sc, ss, n, y1}};
  for (int col = 0; col < 3; ++col) {
    int piv = col;
    for (int r = col + 1; r < 3; ++r)
      if (std::abs(m[r][col]) > std::abs(m[piv][col])) piv = r;
    for (int k = 0; k < 4; ++k) std::swap(m[col][k], m[piv][k]);
    for (int r = 0; r < 3; ++r) {
      if (r == col || m[col][col] == 0.0) continue;
      const double f = m[r][col] / m[col][col];
      for (int k = 0; k < 4; ++k) m[r][k] -= f * m[col][k];
    }
  }
  const double a = m[0][3] / m[0][0], b = m[1][3] / m[1][1], c = m[2][3] / m[2][2];
  Fit f;
  f.amplitude = std::hypot(a, b);
  double e = 0, t = 0;
  for (size_t i = 0; i < y.size(); ++i) {
    const double v = a * std::cos(w * static_cast<double>(i)) + b * std::sin(w * static_cast<double>(i)) + c;
    e += (y[i] - v) * (y[i] - v);
    t += y[i] * y[i];
  }
  f.residualRms = std::sqrt(e / n);
  f.rms = std::sqrt(t / n);
  return f;
}

double dbfs(double rms) { return 20.0 * std::log10(std::max(rms, 1e-30) / std::sqrt(0.5)); }  // re a full-scale sine

// Reads a full-scale sine of fSrc Hz (source rate kSr) at `rate`, `outFrames` frames; returns the left output.
std::vector<double> readSine(double fSrc, double rate, uint32_t outFrames, ResampleQuality quality = ResampleQuality::High) {
  const SincTable& t = prepareSincTable(quality, sincStretchQ(rate));
  const int64_t pos = fixedOf(5000.37);
  const int64_t step = fixedOf(rate);
  const SincReach reach = sincReach(t, pos, step, outFrames);
  REQUIRE(reach.first >= 0);
  std::vector<float> x(static_cast<size_t>(reach.last) + 16);
  for (size_t i = 0; i < x.size(); ++i)
    x[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * fSrc / kSr * static_cast<double>(i)));
  std::vector<float> l(outFrames, 0.0f), r(outFrames, 0.0f);
  const float* in[2] = {x.data(), x.data()};
  float* out[2] = {l.data(), r.data()};
  sincResample(t, in, 2, pos, step, outFrames, nullptr, 1.0f, out);
  for (uint32_t i = 0; i < outFrames; ++i) REQUIRE(l[i] == r[i]);
  return {l.begin(), l.end()};
}

}  // namespace

TEST_CASE("resample: the prototype's passband and stopband") {
  const SincPrototype pr = sincPrototype(ResampleQuality::High);
  const float* f = sincPrototypeTable(ResampleQuality::High);
  const uint32_t n = 512 * pr.halfWidth;
  auto response = [&](double freq) {  // continuous-time response of h, freq in cycles per input frame
    double sum = 0.0;
    for (uint32_t u = 0; u <= n; ++u) {
      const double d = pr.halfWidth - static_cast<double>(u) / 256.0;
      sum += f[u] * std::cos(2.0 * std::numbers::pi * freq * d);
    }
    return sum / 256.0;
  };
  double worstPass = 0.0, worstStop = -400.0;
  for (double fr = 0.0; fr <= 20000.0 / 48000.0 + 1e-12; fr += 0.002) {
    const double db = 20.0 * std::log10(std::abs(response(fr)));
    worstPass = std::max(worstPass, std::abs(db));
  }
  for (double fr = 0.5; fr <= 2.0; fr += 0.0013) worstStop = std::max(worstStop, 20.0 * std::log10(std::abs(response(fr)) + 1e-30));
  MESSAGE("prototype: passband to 20 kHz within ", worstPass, " dB; stopband from Nyquist at most ", worstStop, " dB");
  CHECK(worstPass <= 0.1);
  CHECK(worstStop <= -80.0);
}

TEST_CASE("resample: q follows the rate; every table row has a DC gain of 1") {
  CHECK(sincStretchQ(1.0) == 256);
  CHECK(sincStretchQ(0.25) == 256);
  CHECK(sincStretchQ(-0.9) == 256);
  CHECK(sincStretchQ(1.0001) == 255);
  CHECK(sincStretchQ(2.0) == 128);
  CHECK(sincStretchQ(-2.0) == 128);
  CHECK(sincStretchQ(16.0) == 16);
  CHECK(sincStretchQ(100.0) == 16);
  for (uint32_t q : {256u, 255u, 171u, 128u, 97u, 64u, 16u}) {
    const SincTable& t = prepareSincTable(ResampleQuality::High, q);
    CHECK(t.q == q);
    CHECK(t.taps % 8 == 0);
    CHECK(t.taps >= 64u * 256u / q);
    CHECK(sincTable(ResampleQuality::High, q) == &t);
    for (uint32_t phi = 0; phi <= q; ++phi) {
      double s = 0.0;
      for (uint32_t k = 0; k < t.taps; ++k) s += t.rows[static_cast<size_t>(phi) * 2 * t.taps + k];
      REQUIRE(std::abs(s - 1.0) < 2e-6);
    }
  }
  CHECK(prepareSincTable(ResampleQuality::High, 256).taps == 64);
  CHECK(prepareSincTable(ResampleQuality::Draft, 256).taps == 16);
  // A table that was not prepared: the nearest one with less stretch.
  CHECK(sincTable(ResampleQuality::High, 17) == nullptr);
  CHECK(sincTableAtLeast(ResampleQuality::High, 17).q >= 17);
}

TEST_CASE("resample: sines from −24 to +24 semitones and file-rate conversions") {
  struct Case {
    double rate;
    const char* what;
  };
  std::vector<Case> rates;
  for (int st : {-24, -19, -12, -7, -1, 1, 7, 12, 19, 24}) rates.push_back({std::pow(2.0, st / 12.0), "st"});
  rates.push_back({1.0 + 1e-4, "+0.17 cents"});
  rates.push_back({44100.0 / 48000.0, "44.1 kHz file"});
  rates.push_back({96000.0 / 48000.0, "96 kHz file"});
  rates.push_back({1.0, "rate 1, fractional position"});
  double worstResidual = -400.0, worstGain = 0.0;
  for (const Case& c : rates) {
    const SincTable& t = prepareSincTable(ResampleQuality::High, sincStretchQ(c.rate));
    const double rq = 256.0 / t.q;
    for (double fSrc : {50.0, 440.0, 1000.0, 3000.0, 7000.0, 11000.0, 15000.0, 19000.0, 20000.0}) {
      const double fOut = fSrc * c.rate;
      const std::vector<double> y = readSine(fSrc, c.rate, 8192);
      const double stopSrc = 0.5 * kSr / rq;  // the read's stopband edge, in source Hz
      if (fOut <= 20000.0) {
        const Fit fit = fitSine(y, 2.0 * std::numbers::pi * fOut / kSr);
        const double gainDb = 20.0 * std::log10(fit.amplitude);
        const double res = dbfs(fit.residualRms);
        INFO("rate ", c.rate, " (", c.what, ") f ", fSrc, " Hz → ", fOut, " Hz: gain ", gainDb, " dB, residual ", res, " dBFS");
        CHECK(std::abs(gainDb) <= 0.1);
        CHECK(res <= -80.0);
        worstResidual = std::max(worstResidual, res);
        worstGain = std::max(worstGain, std::abs(gainDb));
      } else if (fSrc >= stopSrc) {  // above the read's Nyquist: everything that comes out is an alias
        const Fit fit = fitSine(y, 0.0);
        const double all = dbfs(fit.rms);
        INFO("rate ", c.rate, " (", c.what, ") f ", fSrc, " Hz (above the read's Nyquist): output ", all, " dBFS");
        CHECK(all <= -80.0);
        worstResidual = std::max(worstResidual, all);
      }
    }
  }
  MESSAGE("±24 st and rate conversions: passband gain within ", worstGain, " dB, images / aliases / error at most ",
          worstResidual, " dBFS");
  // The ±48 semitone extremes (recorded, not asserted: the stretch is capped at 16×, so +48 st sits at the cap).
  for (int st : {-48, 48}) {
    const double rate = std::pow(2.0, st / 12.0);
    double worst = -400.0;
    for (double fSrc : {50.0, 440.0, 1000.0, 3000.0, 7000.0, 11000.0, 15000.0, 20000.0}) {
      const double fOut = fSrc * rate;
      const std::vector<double> y = readSine(fSrc, rate, 8192);
      const Fit fit = fitSine(y, fOut <= 20000.0 ? 2.0 * std::numbers::pi * fOut / kSr : 0.0);
      worst = std::max(worst, dbfs(fOut <= 20000.0 ? fit.residualRms : fit.rms));
    }
    MESSAGE(st, " st: images / aliases at most ", worst, " dBFS");
  }
}

namespace {

// The same read as readSine, through mip levels as the clip player does it: levels 1 … L of the sine built by
// decimate2 (each from the one before), then the read at rate / 2^L from position / 2^L.
std::vector<double> readSineMip(double fSrc, double rate, uint32_t outFrames, uint32_t* levelOut) {
  const uint32_t L = mipLevelFor(rate, kMaxMipLevels);
  *levelOut = L;
  const double scale = std::ldexp(1.0, -static_cast<int>(L));
  const int64_t pos = fixedOf(5000.37) >> L;  // the player's shift of the 32.32 position and step
  const int64_t step = fixedOf(rate) >> L;
  const SincTable& t = prepareSincTable(ResampleQuality::High, sincStretchQ(rate * scale));
  const SincReach reach = sincReach(t, pos, step, outFrames);
  REQUIRE(reach.first >= 0);
  // Level 0 long enough that level L covers the reach with room for the decimators' spread at the end.
  const auto n0 = static_cast<int64_t>((reach.last + 512) << L);
  std::vector<float> x(static_cast<size_t>(n0));
  for (size_t i = 0; i < x.size(); ++i)
    x[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * fSrc / kSr * static_cast<double>(i)));
  int64_t n = n0;
  for (uint32_t l = 0; l < L; ++l) {
    std::vector<float> y(static_cast<size_t>(n / 2));
    decimate2(x.data(), 0, n, y.data(), 0, n / 2);
    x = std::move(y);
    n /= 2;
  }
  REQUIRE(reach.last + 16 <= n);
  std::vector<float> l(outFrames, 0.0f), r(outFrames, 0.0f);
  const float* in[2] = {x.data(), x.data()};
  float* out[2] = {l.data(), r.data()};
  sincResample(t, in, 2, pos, step, outFrames, nullptr, 1.0f, out);
  return {l.begin(), l.end()};
}

}  // namespace

TEST_CASE("resample: the mip decimator's passband and stopband") {
  const double* h = decimatorTaps();
  const auto J = static_cast<int>(kDecimatorHalfTaps);
  double dc = 0.0;
  for (int k = 0; k <= 2 * J; ++k) dc += h[k];
  CHECK(std::abs(dc - 1.0) < 1e-12);
  for (int k = 0; k < J; ++k) REQUIRE(h[k] == h[2 * J - k]);  // symmetric: zero phase about input frame 2m
  auto response = [&](double f) {  // cycles per input frame
    double s = 0.0;
    for (int k = 0; k <= 2 * J; ++k) s += h[k] * std::cos(2.0 * std::numbers::pi * f * (k - J));
    return s;
  };
  double worstPass = 0.0, worstStop = -400.0;
  for (double f = 0.0; f <= 0.21 + 1e-12; f += 0.0005) worstPass = std::max(worstPass, std::abs(20.0 * std::log10(std::abs(response(f)))));
  for (double f = 0.25; f <= 0.5 + 1e-12; f += 0.0002) worstStop = std::max(worstStop, 20.0 * std::log10(std::abs(response(f)) + 1e-30));
  MESSAGE("mip decimator: passband to 0.21 within ", worstPass, " dB; stopband from 0.25 at most ", worstStop, " dB");
  CHECK(worstPass <= 1e-3);
  CHECK(worstStop <= -110.0);
}

TEST_CASE("resample: pitch-up above an octave through mip levels meets the direct read's criteria") {
  double worstResidual = -400.0, worstGain = 0.0, worstDirect = -400.0;
  for (int st : {13, 19, 24, 31, 36, 43, 47}) {
    const double rate = std::pow(2.0, st / 12.0);
    uint32_t L = 0;
    for (double fSrc : {50.0, 440.0, 1000.0, 3000.0, 7000.0, 11000.0, 15000.0, 19000.0, 20000.0}) {
      const double fOut = fSrc * rate;
      const std::vector<double> y = readSineMip(fSrc, rate, 8192, &L);
      const std::vector<double> direct = readSine(fSrc, rate, 8192);
      const double rq = 256.0 / sincStretchQ(rate);  // the direct read's stretch (the mip read's band edge is the same)
      const double stopSrc = 0.5 * kSr / rq;
      if (fOut <= 20000.0) {
        const Fit fit = fitSine(y, 2.0 * std::numbers::pi * fOut / kSr);
        const double gainDb = 20.0 * std::log10(fit.amplitude);
        const double res = dbfs(fit.residualRms);
        INFO("+", st, " st (level ", L, ") f ", fSrc, " Hz → ", fOut, " Hz: gain ", gainDb, " dB, residual ", res, " dBFS");
        CHECK(std::abs(gainDb) <= 0.1);
        CHECK(res <= -80.0);
        worstResidual = std::max(worstResidual, res);
        worstGain = std::max(worstGain, std::abs(gainDb));
        worstDirect = std::max(worstDirect, dbfs(fitSine(direct, 2.0 * std::numbers::pi * fOut / kSr).residualRms));
      } else if (fSrc >= stopSrc) {
        const double all = dbfs(fitSine(y, 0.0).rms);
        INFO("+", st, " st (level ", L, ") f ", fSrc, " Hz (above the read's Nyquist): output ", all, " dBFS");
        CHECK(all <= -80.0);
        worstResidual = std::max(worstResidual, all);
        worstDirect = std::max(worstDirect, dbfs(fitSine(direct, 0.0).rms));
      }
    }
    CHECK(L == mipLevelFor(rate, kMaxMipLevels));
    CHECK(L >= 1);
  }
  MESSAGE("+13 … +47 st through mips: passband gain within ", worstGain, " dB, images / aliases / error at most ",
          worstResidual, " dBFS (direct reads of the same sines: ", worstDirect, " dBFS)");
  CHECK(mipLevelFor(2.0, 3) == 0);
  CHECK(mipLevelFor(2.01, 3) == 1);
  CHECK(mipLevelFor(-4.0, 3) == 1);
  CHECK(mipLevelFor(4.5, 3) == 2);
  CHECK(mipLevelFor(16.0, 3) == 3);
  CHECK(mipLevelFor(40.0, 3) == 3);
  CHECK(mipLevelFor(40.0, 1) == 1);
}

TEST_CASE("resample: reads stay inside sincReach") {
  for (double rate : {0.3, 1.0, 1.7, 5.0, -0.8, -3.0}) {
    const SincTable& t = prepareSincTable(ResampleQuality::High, sincStretchQ(rate));
    const int64_t pos = fixedOf(3000.81), step = fixedOf(rate);
    const uint32_t frames = 333;
    const SincReach reach = sincReach(t, pos, step, frames);
    std::vector<float> x(static_cast<size_t>(reach.last) + 4096, std::numeric_limits<float>::quiet_NaN());
    for (int64_t j = reach.first; j < reach.last; ++j) x[static_cast<size_t>(j)] = std::sin(0.01f * static_cast<float>(j));
    std::vector<float> l(frames, 0.0f), r(frames, 0.0f);
    const float* in[2] = {x.data(), x.data()};
    float* out[2] = {l.data(), r.data()};
    sincResample(t, in, 2, pos, step, frames, nullptr, 1.0f, out);
    for (uint32_t i = 0; i < frames; ++i) REQUIRE(std::isfinite(l[i]));
  }
}

TEST_CASE("resample: a reverse read mirrors a forward read; mono adds to both outputs") {
  const uint32_t n = 20000, frames = 500;
  std::vector<float> x(n), xr(n);
  for (uint32_t i = 0; i < n; ++i) x[i] = std::sin(0.013f * static_cast<float>(i)) + 0.3f * std::sin(0.71f * static_cast<float>(i));
  for (uint32_t i = 0; i < n; ++i) xr[i] = x[n - 1 - i];
  for (double rate : {0.77, 1.31}) {
    const SincTable& t = prepareSincTable(ResampleQuality::High, sincStretchQ(rate));
    const double p0 = 4000.123;
    std::vector<float> a(frames, 0.0f), b(frames, 0.0f), z(frames, 0.0f);
    const float* in[1] = {x.data()};
    const float* inr[1] = {xr.data()};
    float* outA[2] = {a.data(), z.data()};
    sincResample(t, in, 1, fixedOf(p0), fixedOf(rate), frames, nullptr, 1.0f, outA);
    for (uint32_t i = 0; i < frames; ++i) REQUIRE(a[i] == z[i]);  // mono: the same sum on both outputs
    std::fill(z.begin(), z.end(), 0.0f);
    float* outB[2] = {b.data(), z.data()};
    sincResample(t, inr, 1, fixedOf(n - 1 - p0), -fixedOf(rate), frames, nullptr, 1.0f, outB);
    double worst = 0.0;
    for (uint32_t i = 0; i < frames; ++i) worst = std::max(worst, static_cast<double>(std::abs(a[i] - b[i])));
    CHECK(worst < 2e-5);
  }
}

TEST_CASE("resample: straight copies, forward and reverse, with a gain or an envelope") {
  const uint32_t frames = 77;
  std::vector<float> x0(300), x1(300), env(frames);
  for (uint32_t i = 0; i < 300; ++i) x0[i] = static_cast<float>(i), x1[i] = -static_cast<float>(i) * 0.5f;
  for (uint32_t i = 0; i < frames; ++i) env[i] = 0.01f * static_cast<float>(i);
  const float* in[2] = {x0.data(), x1.data()};
  std::vector<float> l(frames, 1.0f), r(frames, 2.0f);
  float* out[2] = {l.data(), r.data()};
  gainCopy(in, 2, 100, 1, frames, nullptr, 0.5f, out);
  for (uint32_t i = 0; i < frames; ++i) {
    CHECK(l[i] == 1.0f + x0[100 + i] * 0.5f);
    CHECK(r[i] == 2.0f + x1[100 + i] * 0.5f);
  }
  std::fill(l.begin(), l.end(), 0.0f);
  std::fill(r.begin(), r.end(), 0.0f);
  gainCopy(in, 2, 250, -1, frames, env.data(), 0.0f, out);
  for (uint32_t i = 0; i < frames; ++i) {
    CHECK(l[i] == x0[250 - i] * env[i]);
    CHECK(r[i] == x1[250 - i] * env[i]);
  }
  std::fill(l.begin(), l.end(), 0.0f);
  std::fill(r.begin(), r.end(), 0.0f);
  gainCopy(in, 1, 10, 1, frames, nullptr, 1.0f, out);
  for (uint32_t i = 0; i < frames; ++i) CHECK((l[i] == x0[10 + i] && r[i] == x0[10 + i]));
}

TEST_CASE("resample: the draft quality reads a sine") {
  const std::vector<double> y = readSine(1000.0, 44100.0 / 48000.0, 4096, ResampleQuality::Draft);
  const Fit fit = fitSine(y, 2.0 * std::numbers::pi * 1000.0 * (44100.0 / 48000.0) / kSr);
  CHECK(std::abs(20.0 * std::log10(fit.amplitude)) < 0.1);
  CHECK(dbfs(fit.residualRms) < -50.0);
}

TEST_CASE("rt: the resampler is allocation-free") {
  REQUIRE(pq::alloc_trap::installed());
  const SincTable& t = prepareSincTable(ResampleQuality::High, 200);
  (void)prepareSincTable(ResampleQuality::High, 256);
  std::vector<float> x(10000, 0.25f), l(256, 0.0f), r(256, 0.0f), env(256, 1.0f);
  const float* in[2] = {x.data(), x.data()};
  float* out[2] = {l.data(), r.data()};
  CHECK(pq::alloc_trap::countRtViolations([&] {
          sincResample(t, in, 2, fixedOf(2000.5), fixedOf(1.2), 256, env.data(), 0.0f, out);
          sincResample(*sincTable(ResampleQuality::High, 256), in, 1, fixedOf(5000.5), -fixedOf(0.7), 256, nullptr, 1.0f, out);
          gainCopy(in, 2, 100, 1, 256, nullptr, 1.0f, out);
          gainCopy(in, 2, 5000, -1, 256, env.data(), 0.0f, out);
          (void)sincStretchQ(3.3);
          (void)sincTableAtLeast(ResampleQuality::High, 20);
          (void)sincReach(t, 0, 1, 1);
        }) == 0);
}
