// The tone preview's oscillator (pq/dsp/tone_preview.h; VZ.3): pitch, glide, the release ramp (no clicks), the owner
// token, the band-limited shapes, RT safety.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/dsp/tone_preview.h"

using pq::dsp::TonePreview;
using pq::dsp::ToneWave;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> run(TonePreview& t, uint32_t frames) {
  std::vector<float> l(frames, 0.0f), r(frames, 0.0f);
  float* out[2] = {l.data(), r.data()};
  for (uint32_t at = 0; at < frames; at += 128) {
    float* o[2] = {out[0] + at, out[1] + at};
    t.render(o, 2, std::min<uint32_t>(128, frames - at), kSr);
  }
  CHECK(l == r);
  return l;
}

// The frequency of x by upward zero crossings (interpolated).
double pitchOf(const std::vector<float>& x, size_t from) {
  double first = -1, last = -1;
  int n = 0;
  for (size_t i = from + 1; i < x.size(); ++i)
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double c = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
      if (first < 0) first = c;
      else ++n;
      last = c;
    }
  return n > 0 ? n * kSr / (last - first) : 0.0;
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
  CHECK(pitchOf(x, 2400) == doctest::Approx(440.0).epsilon(1e-3));
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
  const auto x = run(t, 9600);
  // 12 ms time constant: half-way (in octaves) after ~8 ms, there after ~100 ms.
  CHECK(pitchOf(x, 4800) == doctest::Approx(880.0).epsilon(2e-3));
  float maxStep = 0.0f;
  for (size_t i = 1; i < 960; ++i) maxStep = std::max(maxStep, std::fabs(x[i] - x[i - 1]));
  CHECK(maxStep < 0.5f * static_cast<float>(2.0 * kPi * 880.0 / kSr) * 1.05f);  // continuous phase: no jump
  t.hold(&b, 330.0f, -6.0f, ToneWave::Sine);  // b takes over
  t.release(&a);                               // a's stale release: the tone stays
  CHECK(t.heldBy(&b));
  const auto y = run(t, 9600);
  CHECK(pitchOf(y, 4800) == doctest::Approx(330.0).epsilon(2e-3));
  t.release(&b);
  (void)run(t, 4800);
  CHECK_FALSE(t.sounding());
}

TEST_CASE("tone preview: Semisine and Triangle keep their pitch and stay band-limited at the top") {
  for (ToneWave w : {ToneWave::SemiSine, ToneWave::Triangle})
    for (float hz : {55.0f, 1000.0f, 7000.0f}) {
      TonePreview t;
      int o = 0;
      t.hold(&o, hz, -6.0f, w);
      const auto x = run(t, 48000);
      CAPTURE(static_cast<int>(w));
      CAPTURE(hz);
      // Energy above 20.5 kHz (a DFT of the last 4096 samples): nothing folded back from harmonics past Nyquist.
      const size_t n = 4096, base = x.size() - n;
      double total = 0.0, high = 0.0;
      for (size_t k = 1; k < n / 2; k += 3) {
        std::complex<double> s = 0.0;
        for (size_t i = 0; i < n; ++i) {
          const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / n);
          s += win * x[base + i] * std::polar(1.0, -2.0 * kPi * static_cast<double>(k * i) / n);
        }
        const double p = std::norm(s);
        total += p;
        if (static_cast<double>(k) * kSr / n > 20500.0) high += p;
      }
      CHECK(high < 1e-6 * total);
      if (w == ToneWave::Triangle) CHECK(pitchOf(x, 4800) == doctest::Approx(hz).epsilon(2e-3));
    }
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
