// M6 review (warp) Q3: the quality claims at tempos / sources the tests do not use.
//  1. Tones pitch (claim 0.004 ¢; test bound 0.5 ¢): 220 / 331.7 / 61.3 Hz sines at 97 / 143 BPM, ratio 1 and +3\31,
//     and a 3-note chord (each partial's error).
//  2. Beats hit placement (claim ±0.15 ms, test ±1 ms) at 97 / 143 BPM, grid and transient slices.
//  3. Complex onsets (claim ±2.5 ms) at 97 / 143 BPM.
//  4. Clicks at Beats slice boundaries: a sustained sine (no transients: the 1/16 grid) at 90 / 150 BPM in each loop
//     mode — the largest step against the sine's own, and the minimum of the short-time RMS (dips at crossfades).
#include "probe_common.h"
#include "pq/media/transients.h"

constexpr int64_t kHitFrames = 12000;
constexpr int kHits = 16;
static float drum(int64_t i) {
  const int64_t h = i / kHitFrames;
  const double t = static_cast<double>(i - h * kHitFrames) / kSr;
  uint32_t z = static_cast<uint32_t>(i) * 747796405u + 2891336453u;
  z = ((z >> ((z >> 28u) + 4u)) ^ z) * 277803737u;
  const double noise = static_cast<double>((z >> 22u) ^ z) / 4294967296.0 * 2.0 - 1.0;
  switch (h % 4) {
    case 0: {
      const double ph = 2.0 * std::numbers::pi * (50.0 * t + 70.0 * 0.04 * (1.0 - std::exp(-t / 0.04)));
      return static_cast<float>(0.8 * std::sin(ph + 0.6) * std::exp(-t / 0.08) + 0.3 * noise * std::exp(-t / 0.002));
    }
    case 2:
      return static_cast<float>(0.5 * noise * std::exp(-t / 0.05) + 0.3 * std::sin(2.0 * std::numbers::pi * 190.0 * t + 0.4) * std::exp(-t / 0.03));
    default:
      return static_cast<float>(0.35 * noise * std::exp(-t / 0.012));
  }
}
static int64_t hitNear(const std::vector<float>& y, int64_t e, const std::vector<float>& src, int64_t srcStart) {
  constexpr int64_t kLen = 1920, kReach = 480;
  double best = -1e300;
  int64_t at = 0;
  for (int64_t lag = -kReach; lag <= kReach; ++lag) {
    const int64_t o = e + lag;
    if (o < 0 || o + kLen > static_cast<int64_t>(y.size())) continue;
    double num = 0.0, en = 0.0;
    for (int64_t i = 0; i < kLen; ++i) {
      const double a = y[static_cast<size_t>(o + i)], b = src[static_cast<size_t>(srcStart + i)];
      num += a * b;
      en += a * a;
    }
    const double sc = en > 0.0 ? num / std::sqrt(en) : 0.0;
    if (sc > best) {
      best = sc;
      at = lag;
    }
  }
  return at;
}

int main() {
  // ---- 1 ----
  for (double hz : {220.0, 331.7, 61.3}) {
    const e::ClipSample sine = makeSample(static_cast<int64_t>(kSr * 20.0), 1, [hz](uint32_t, int64_t i) {
      return static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kSr));
    });
    for (double bpm : {97.0, 143.0})
      for (double ratio : {1.0, std::exp2(3.0 / 31.0)}) {
        Rig rig(128);
        rig.tempo({{0, bpm, pq::TempoCurve::Step}});
        e::AudioClipDesc c = warped(e::ClipWarp::Tones, 1, 0, kPpq * 16, 120.0);
        c.stretch.pitchRatio = ratio;
        rig.audio({{1, {c}}}, {sine});
        rig.play(0);
        const std::vector<float> o = rig.render(static_cast<uint64_t>(kSr * 3.2));
        const double f = peakHz(o, 4800, 131072, hz * ratio);
        std::printf("1 Tones %6.1f Hz %3.0f BPM ratio %.4f: error %+.4f cents\n", hz, bpm, ratio, cents(f, hz * ratio));
      }
  }
  {
    const double fs[3] = {220.0, 277.18, 329.63};
    const e::ClipSample chord = makeSample(static_cast<int64_t>(kSr * 20.0), 1, [&](uint32_t, int64_t i) {
      double v = 0;
      for (double f : fs) v += 0.25 * std::sin(2.0 * std::numbers::pi * f * static_cast<double>(i) / kSr);
      return static_cast<float>(v);
    });
    for (double bpm : {97.0, 143.0}) {
      Rig rig(128);
      rig.tempo({{0, bpm, pq::TempoCurve::Step}});
      e::AudioClipDesc c = warped(e::ClipWarp::Tones, 1, 0, kPpq * 16, 120.0);
      rig.audio({{1, {c}}}, {chord});
      rig.play(0);
      const std::vector<float> o = rig.render(static_cast<uint64_t>(kSr * 3.2));
      std::printf("1 Tones chord %3.0f BPM: errors", bpm);
      for (double f : fs) std::printf(" %+.3f", cents(peakHz(o, 4800, 131072, f), f));
      std::printf(" cents; level of each partial vs 0.25·N/4:");
      for (double f : fs) std::printf(" %.3f", dtftMag(o, 4800, 131072, f) / (0.25 * 131072 / 4));
      std::printf("\n");
    }
  }
  // ---- 2 / 3 ----
  const e::ClipSample loop = makeSample(kHitFrames * kHits, 1, [](uint32_t, int64_t i) { return drum(i); });
  std::vector<float> src(static_cast<size_t>(kHitFrames * kHits));
  for (size_t i = 0; i < src.size(); ++i) src[i] = drum(static_cast<int64_t>(i));
  const float* sch = src.data();
  const std::vector<double> onsets = pq::media::detectTransients(&sch, 1, static_cast<int64_t>(src.size()), 48000);
  for (double bpm : {97.0, 143.0, 90.0, 150.0})
    for (Ticks gran : {kPpq / 4, Ticks{0}}) {
      Rig rig(256);
      rig.tempo({{0, bpm, pq::TempoCurve::Step}});
      e::AudioClipDesc c = warped(e::ClipWarp::Beats, 1, 0, kPpq * 8, 120.0);
      c.stretch.granulation = gran;
      c.transients = onsets;
      rig.audio({{1, {c}}}, {loop});
      rig.play(0);
      const std::vector<float> o = rig.render(static_cast<uint64_t>(rig.map.ticksToSamples(kPpq * 8, kSr)));
      double worst = 0;
      for (int h = 0; h < kHits; ++h) {
        const SampleTime ex = rig.map.ticksToSamples(static_cast<Ticks>(h) * kPpq / 2, kSr);
        worst = std::max(worst, std::abs(static_cast<double>(hitNear(o, ex, src, static_cast<int64_t>(h) * kHitFrames))));
      }
      std::printf("2 Beats %3.0f BPM %s: worst hit %.3f ms\n", bpm, gran == 0 ? "transients" : "1/16 grid ", worst / 48.0);
    }
  for (double bpm : {97.0, 143.0, 90.0, 150.0}) {
    Rig rig(256);
    rig.tempo({{0, bpm, pq::TempoCurve::Step}});
    e::AudioClipDesc c = warped(e::ClipWarp::Complex, 1, kPpq, kPpq * 8, 120.0);
    rig.audio({{1, {c}}}, {loop});
    rig.play(0);
    const std::vector<float> o = rig.render(static_cast<uint64_t>(rig.map.ticksToSamples(kPpq * 9, kSr)));
    const SampleTime s0 = rig.map.ticksToSamples(kPpq, kSr);
    const float* och = o.data() + s0;
    const std::vector<double> got = pq::media::detectTransients(&och, 1, static_cast<int64_t>(o.size()) - s0, 48000);
    double worst = 0;
    int missing = 0;
    for (int h = 1; h < kHits; ++h) {
      const double expected = static_cast<double>(
          rig.map.ticksToSamples(kPpq + static_cast<Ticks>(std::llround(onsets[static_cast<size_t>(h)] / fptAt(120.0))), kSr) - s0);
      double best = 1e30;
      for (double g : got) best = std::abs(g - expected) < std::abs(best) ? g - expected : best;
      if (std::abs(best) > 480) ++missing;
      else worst = std::max(worst, std::abs(best));
    }
    std::printf("3 Complex %3.0f BPM: worst onset %.3f ms, missing %d\n", bpm, worst / 48.0, missing);
  }
  // ---- 4 ----
  const e::ClipSample sine = makeSample(static_cast<int64_t>(kSr * 20.0), 1, [](uint32_t, int64_t i) {
    return static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / kSr));
  });
  const double own = 0.5 * 2.0 * std::numbers::pi * 220.0 / kSr;
  for (double bpm : {90.0, 150.0})
    for (e::SliceLoop lm : {e::SliceLoop::Off, e::SliceLoop::Forward, e::SliceLoop::BackForward}) {
      Rig rig(256);
      rig.tempo({{0, bpm, pq::TempoCurve::Step}});
      e::AudioClipDesc c = warped(e::ClipWarp::Beats, 1, 0, kPpq * 16, 120.0);
      c.stretch.sliceLoop = lm;
      rig.audio({{1, {c}}}, {sine});
      rig.play(0);
      const std::vector<float> o = rig.render(static_cast<uint64_t>(kSr * 4));
      double step = 0, minRms = 1e9;
      for (size_t i = 960; i + 1 < o.size(); ++i) step = std::max(step, static_cast<double>(std::abs(o[i + 1] - o[i])));
      for (size_t i = 960; i + 96 < o.size() - 4800; i += 24) {
        double e2 = 0;
        for (size_t k = 0; k < 96; ++k) e2 += static_cast<double>(o[i + k]) * o[i + k];
        minRms = std::min(minRms, std::sqrt(e2 / 96));
      }
      std::printf("4 Beats sine %3.0f BPM loop %d: max step %.4f (sine's own %.4f, %.1fx), min 2 ms RMS %.3f (steady 0.354)\n", bpm,
                  static_cast<int>(lm), step, own, step / own, minRms);
    }
  return 0;
}
