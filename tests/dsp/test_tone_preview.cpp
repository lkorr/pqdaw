// The tone preview's oscillator (pq/dsp/tone_preview.h; VZ.3): pitch, glide, the release ramp (no clicks), the owner
// token, the band-limited shapes, RT safety.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/dsp/fft.h"
#include "pq/dsp/tone_preview.h"

using pq::dsp::TonePreview;
using pq::dsp::ToneWave;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> run(TonePreview& t, uint32_t frames, double sr = kSr) {
  std::vector<float> l(frames, 0.0f), r(frames, 0.0f);
  float* out[2] = {l.data(), r.data()};
  for (uint32_t at = 0; at < frames; at += 128) {
    float* o[2] = {out[0] + at, out[1] + at};
    t.render(o, 2, std::min<uint32_t>(128, frames - at), sr);
  }
  CHECK(l == r);
  return l;
}

// The frequency of x by upward zero crossings (interpolated).
double pitchOf(const std::vector<float>& x, size_t from, double sr = kSr) {
  double first = -1, last = -1;
  int n = 0;
  for (size_t i = from + 1; i < x.size(); ++i)
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double c = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
      if (first < 0) first = c;
      else ++n;
      last = c;
    }
  return n > 0 ? n * sr / (last - first) : 0.0;
}

// Power spectrum of the last n samples (4-term Blackman-Harris: sidelobes under −92 dB, so a spur is the signal's).
std::vector<double> spectrum(const std::vector<float>& x, uint32_t n) {
  std::vector<float> in(n), re(n / 2 + 1), im(n / 2 + 1);
  const size_t base = x.size() - n;
  for (uint32_t i = 0; i < n; ++i) {
    const double a = 2.0 * kPi * i / n;
    const double w = 0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a);
    in[i] = static_cast<float>(w * x[base + i]);
  }
  pq::dsp::RealFft fft(n);
  fft.forward(in.data(), re.data(), im.data());
  std::vector<double> p(n / 2 + 1);
  for (size_t k = 0; k < p.size(); ++k) p[k] = static_cast<double>(re[k]) * re[k] + static_cast<double>(im[k]) * im[k];
  return p;
}

float peak(const std::vector<float>& x, size_t a, size_t b) {
  float m = 0.0f;
  for (size_t i = a; i < b; ++i) m = std::max(m, std::fabs(x[i]));
  return m;
}

}  // namespace

TEST_CASE("tone preview: a hold plays the pitch at the level; a release fades it out in 30 ms without a click") {
  TonePreview t;
  int owner = 0;
  CHECK_FALSE(t.sounding());
  t.hold(&owner, 440.0f, -12.0f, ToneWave::Sine);
  CHECK(t.sounding());
  const auto x = run(t, 24000);
  CHECK(pitchOf(x, 2400) == doctest::Approx(440.0).epsilon(2e-5));
  CHECK(peak(x, 2400, 24000) == doctest::Approx(std::pow(10.0, -12.0 / 20.0)).epsilon(0.01));
  CHECK(peak(x, 0, 24) < peak(x, 2400, 24000) * 0.3f);  // the 5 ms attack ramps in
  // Release: down to nothing within the 30 ms ramp, and no step larger than the sine's own slope allows.
  t.release(&owner);
  CHECK(t.sounding());  // still fading
  const auto y = run(t, 4800);
  CHECK(peak(y, 1500, 4800) == 0.0f);
  float worst = 0.0f;
  for (size_t i = 1; i < y.size(); ++i) worst = std::max(worst, std::fabs(y[i] - y[i - 1]));
  CHECK(worst < 2.0f * static_cast<float>(2.0 * kPi * 440.0 / kSr) * 0.26f);  // ≤ the tone's own slope (+ margin)
  CHECK_FALSE(t.sounding());
}

TEST_CASE("tone preview: a retune glides in pitch; another owner takes the tone over; a stale release is ignored") {
  TonePreview t;
  int a = 0, b = 0;
  t.hold(&a, 220.0f, -6.0f, ToneWave::Sine);
  (void)run(t, 4800);
  t.hold(&a, 880.0f, -6.0f, ToneWave::Sine);
  const auto x = run(t, 14400);
  // 12 ms time constant: half-way (in octaves) after ~8 ms, within 1e-5 octave (snapped onto it) after ~150 ms.
  CHECK(pitchOf(x, 9600) == doctest::Approx(880.0).epsilon(2e-5));  // 0.035 cent
  CHECK(t.currentHz() == 880.0f);  // on the target exactly (M1: a float glide parked short of it)
  float maxStep = 0.0f;
  for (size_t i = 1; i < 960; ++i) maxStep = std::max(maxStep, std::fabs(x[i] - x[i - 1]));
  CHECK(maxStep < 0.5f * static_cast<float>(2.0 * kPi * 880.0 / kSr) * 1.05f);  // continuous phase: no jump
  t.hold(&b, 330.0f, -6.0f, ToneWave::Sine);  // b takes over
  t.release(&a);                               // a's stale release: the tone stays
  CHECK(t.heldBy(&b));
  const auto y = run(t, 14400);
  CHECK(pitchOf(y, 9600) == doctest::Approx(330.0).epsilon(2e-5));
  t.release(&b);
  (void)run(t, 4800);
  CHECK_FALSE(t.sounding());
}

TEST_CASE("tone preview: Semisine and Triangle keep their pitch, sound up to 20 kHz and alias nowhere") {
  for (ToneWave w : {ToneWave::SemiSine, ToneWave::Triangle})
    for (float hz : {55.0f, 1000.0f, 7000.0f, 12000.0f, 18000.0f}) {
      TonePreview t;
      int o = 0;
      t.hold(&o, hz, -6.0f, w);
      const auto x = run(t, 48000);
      CAPTURE(static_cast<int>(w));
      CAPTURE(hz);
      // Everything not within ±4 bins of a harmonic k·hz under 20 kHz is a spur: harmonics past the cut, aliases
      // folded back from past Nyquist (anywhere in the band, not only above 20.5 kHz), interpolation images.
      constexpr uint32_t n = 16384;
      const auto p = spectrum(x, n);
      const double bin = kSr / n;
      double total = 0.0, spur = 0.0, fund = 0.0;
      for (size_t k = 1; k < p.size(); ++k) {
        const double f = static_cast<double>(k) * bin;
        total += p[k];
        const double h = std::round(f / hz);
        const bool harmonic = h >= 1.0 && h * hz < 20000.0 && std::fabs(f - h * hz) <= 4.0 * bin;
        if (!harmonic) spur += p[k];
        if (h == 1.0 && std::fabs(f - hz) <= 4.0 * bin) fund += p[k];
      }
      CHECK(spur < 1e-6 * total);
      // The fundamental sounds at every pitch (H2: from 10.24 kHz up the tables were silent).
      const double rms = std::sqrt(std::inner_product(x.end() - n, x.end(), x.end() - n, 0.0) / n);
      CHECK(20.0 * std::log10(rms) > -16.0);  // −6 dB peak; a lone fundamental of the semisine is ≈ −12 dB rms
      if (hz > 10000.0f) CHECK(fund > 0.95 * total);  // up there it is (almost) all fundamental
      CHECK(pitchOf(x, 4800) == doctest::Approx(hz).epsilon(2e-5));
    }
}

TEST_CASE("tone preview: the glide lands on the target at every rate (no float parking)") {
  for (const double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
    CAPTURE(sr);
    TonePreview t;
    int o = 0;
    t.hold(&o, 440.0f, -6.0f, ToneWave::Sine);
    (void)run(t, static_cast<uint32_t>(sr * 0.1), sr);
    t.hold(&o, 660.0f, -6.0f, ToneWave::Sine);
    const auto x = run(t, static_cast<uint32_t>(sr), sr);
    CHECK(t.currentHz() == 660.0f);
    CHECK(pitchOf(x, static_cast<size_t>(sr * 0.3), sr) == doctest::Approx(660.0).epsilon(1e-5));
  }
}

TEST_CASE("tone preview: a gain change ramps in its ramp time whatever the distance (take-over 0 → −40 dB)") {
  TonePreview t;
  int a = 0, b = 0;
  t.hold(&a, 440.0f, 0.0f, ToneWave::Sine);
  (void)run(t, 4800);
  CHECK(t.currentGain() == doctest::Approx(1.0f));
  t.hold(&b, 440.0f, -40.0f, ToneWave::Sine);
  (void)run(t, static_cast<uint32_t>(kSr * TonePreview::kReleaseSeconds) + 256);
  CHECK(t.currentGain() == doctest::Approx(0.01f).epsilon(1e-4));
  t.hold(&b, 440.0f, -90.0f, ToneWave::Sine);
  (void)run(t, static_cast<uint32_t>(kSr * TonePreview::kReleaseSeconds) + 256);
  CHECK(t.currentGain() == doctest::Approx(std::pow(10.0f, -90.0f / 20.0f)).epsilon(1e-3));
  t.hold(&b, 440.0f, 0.0f, ToneWave::Sine);  // and back up in the attack time
  (void)run(t, static_cast<uint32_t>(kSr * TonePreview::kAttackSeconds) + 256);
  CHECK(t.currentGain() == doctest::Approx(1.0f));
}

TEST_CASE("tone preview: render never allocates (audio thread, RtScope)") {
  TonePreview t;
  int o = 0;
  t.hold(&o, 440.0f, -18.0f, ToneWave::Triangle);
  std::vector<float> l(512, 0.0f), r(512, 0.0f);
  float* out[2] = {l.data(), r.data()};
  CHECK(pq::alloc_trap::countRtViolations([&] {
          for (int i = 0; i < 40; ++i) t.render(out, 2, 512, kSr);
          t.release(&o);
          for (int i = 0; i < 40; ++i) t.render(out, 2, 512, kSr);
        }) == 0);
}
