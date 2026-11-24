// SM.4: dsp::WarpVoice, the Sampler's Beats / Tones / Texture voice (pq/dsp/warp_voice.h):
//   - the markers' law (beat ↔ frame) round-trips; no markers: sampleBpm
//   - tempo sync: Beats slice entries land on the analytic samples for 64 bars, at 97 BPM and at 120 then 97 BPM (drift
//     0: the law is re-anchored only when the tempo changes)
//   - Tones keeps a sine's pitch at another tempo; rate 1.5 raises it by exactly that ratio
//   - a glide (rate 1 → 2 over 0.5 s) and a vibrato are continuous in every mode: no step between adjacent samples
//     above 1.1 × the largest slope of the source sine at the glide's top rate
//   - renders are the same whatever the run length (1, 37, 128, 512 frames per call)
//   - the end stops the voice with a 2 ms fade; a loop keeps it going
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "pq/core/epoch_reclaimer.h"
#include "pq/dsp/sample_slot.h"
#include "pq/dsp/warp_voice.h"

using namespace pq;
using namespace pq::dsp;

namespace {

constexpr double kSr = 48000.0;

struct Src {
  EpochReclaimer reclaimer{1};
  SampleSlot slot;
  std::shared_ptr<const SampleData> data;
  Src(const std::vector<float>& l, std::vector<float> slices = {}, std::vector<float> markers = {}, double fileRate = kSr) {
    data = SampleData::build(l).value();
    REQUIRE(slot.publish(SampleData::source(data, fileRate, std::move(slices), std::move(markers)), kSr, reclaimer).ok());
  }
  WarpSource source() const { return warpSourceOf(*slot.acquire()); }
};

struct Scratch {
  std::vector<float> gain = std::vector<float>(kWarpVoiceChunk + 16);
  std::vector<float> a = std::vector<float>(kWarpMaxAlignLen + 64);
  std::vector<float> b = std::vector<float>(kWarpMaxAlignLen + 2 * kWarpMaxAlignLags + 64);
  std::vector<float> g = std::vector<float>(2 * kWarpGatherFrames);
  WarpScratch ws() { return {gain.data(), a.data(), b.data(), g.data()}; }
};

// Renders `frames` from a trigger in calls of `block` frames; bpmAt / rateAt per frame (bpm read per call, as a block).
struct Out {
  std::vector<float> l, r, playing;
};
template <class Bpm, class Rate>
Out play(const WarpSource& src, WarpVoiceRun run, size_t frames, uint32_t block, Bpm bpmAt, Rate rateAt) {
  Scratch sc;
  WarpVoice v;
  run.bpm = bpmAt(0);
  const WarpRunLaw law = warpRunLaw(src, run);  // (once per run record: it does not depend on the tempo)
  v.trigger(src, run, law);
  Out o;
  o.l.resize(frames), o.r.resize(frames), o.playing.resize(frames);
  std::vector<float> rate(block), pos(block);
  for (size_t t = 0; t < frames; t += block) {
    const auto n = static_cast<uint32_t>(std::min<size_t>(block, frames - t));
    run.bpm = bpmAt(t);
    for (uint32_t i = 0; i < n; ++i) rate[i] = rateAt(t + i);
    v.render(src, run, law, rate.data(), n, o.l.data() + t, o.r.data() + t, pos.data(), o.playing.data() + t, sc.ws());
  }
  return o;
}

std::vector<float> sine(size_t n, double hz, double amp = 0.5) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kSr));
  return v;
}

// Frequency from the zero crossings over [from, to) (linear interpolation of each crossing).
double crossingHz(const std::vector<float>& y, size_t from, size_t to) {
  double first = -1, last = -1;
  int n = 0;
  for (size_t i = from + 1; i < to; ++i)
    if (y[i - 1] < 0.0f && y[i] >= 0.0f) {
      const double x = static_cast<double>(i - 1) + y[i - 1] / (y[i - 1] - y[i]);
      if (first < 0) first = x;
      last = x;
      ++n;
    }
  return n > 1 ? (n - 1) * kSr / (last - first) : 0.0;
}

}  // namespace

TEST_CASE("SM.4 warp voice: the markers' law round-trips; without markers it is sampleBpm") {
  const std::vector<float> l(48000 * 4, 0.0f);
  Src none(l);
  const double fpb = 60.0 * kSr / 120.0;
  // (The voice's law: warp.h WarpLaw over the entry's segments, fpb frames per beat without markers.)
  const auto lawOf = [&](const WarpSource& w) { return warpBeatLaw(w.law, w.lawCount, w.markers, w.markerCount, fpb); };
  const auto warpFrameOfBeat = [&](const WarpSource& w, double q, double) { return lawOf(w).frameAt(q); };
  const auto warpBeatOfFrame = [&](const WarpSource& w, double f, double) { return lawOf(w).contentAt(f); };
  CHECK(warpFrameOfBeat(none.source(), 3.0, fpb) == doctest::Approx(72000.0));
  CHECK(warpBeatOfFrame(none.source(), 36000.0, fpb) == doctest::Approx(1.5));
  // Rubato: 0 → 0, 30000 → 1, 54000 → 2, 96000 → 3 (one unsorted, one non-monotone pair dropped by publish).
  Src m(l, {}, {54000.0f, 2.0f, 0.0f, 0.0f, 30000.0f, 1.0f, 40000.0f, 0.5f, 96000.0f, 3.0f});
  const WarpSource s = m.source();
  REQUIRE(s.markerCount == 4);
  CHECK(warpFrameOfBeat(s, 1.5, fpb) == doctest::Approx(42000.0));
  CHECK(warpFrameOfBeat(s, 4.0, fpb) == doctest::Approx(138000.0));  // past the last: the last segment continues
  CHECK(warpFrameOfBeat(s, -1.0, fpb) == doctest::Approx(-30000.0));
  for (double q = -0.5; q < 4.0; q += 0.37) CHECK(warpBeatOfFrame(s, warpFrameOfBeat(s, q, fpb), fpb) == doctest::Approx(q));
}

TEST_CASE("SM.4 warp voice: Beats stays on the tempo for 64 bars at 97 BPM and at 120 → 97 BPM (drift 0)") {
  // A 120 BPM click track: a click 200 frames into every beat (after the slice's 2 ms fade-in), slices on the beats.
  constexpr int kBeats = 258;
  std::vector<float> clicks(static_cast<size_t>(kBeats) * 24000, 0.0f);
  std::vector<float> slices;
  for (int k = 0; k < kBeats; ++k) {
    clicks[static_cast<size_t>(k) * 24000 + 200] = 1.0f;
    slices.push_back(static_cast<float>(k * 24000));
  }
  Src src(clicks, slices);
  WarpVoiceRun run;
  run.mode = AudioWarp::Beats;
  run.start = 0.0;
  run.end = static_cast<double>(clicks.size());
  run.sampleBpm = 120.0;
  run.sampleRate = kSr;
  run.set.sliceLoop = WarpSliceLoop::Off;
  for (const bool change : {false, true}) {
    CAPTURE(change);
    // 64 bars of 4 beats; the change after 32 bars (3 072 000 samples at 120 BPM: a block boundary).
    const int64_t changeAt = change ? 32 * 4 * 24000 : 0;
    auto bpmAt = [&](size_t t) { return static_cast<int64_t>(t) < changeAt ? 120.0 : 97.0; };
    auto expected = [&](int k) -> int64_t {  // the sample beat k enters (exact rational arithmetic)
      const int k0 = change ? 128 : 0;
      if (k <= k0) return static_cast<int64_t>(k) * 24000;
      const int64_t num = static_cast<int64_t>(k - k0) * 2880000;  // beats × 60 · 48000 / 97
      return (change ? changeAt : 0) + (num + 96) / 97;
    };
    const int64_t total = expected(256) + 2000;
    const Out o = play(src.source(), run, static_cast<size_t>(total), 64, bpmAt, [](size_t) { return 1.0f; });
    int worst = 0;
    for (int k = 1; k <= 256; ++k) {
      const int64_t e = expected(k) + 200;
      // The click is the largest sample near e (a copy at rate 1: exactly 1 on its frame).
      int64_t at = e - 300;
      for (int64_t t = e - 300; t <= e + 300; ++t)
        if (o.l[static_cast<size_t>(t)] > o.l[static_cast<size_t>(at)]) at = t;
      worst = std::max(worst, static_cast<int>(std::abs(at - e)));
      if (at != e) MESSAGE("beat ", k, ": click at ", at, ", expected ", e);
    }
    CHECK(worst == 0);
    CHECK(o.l[static_cast<size_t>(expected(256) + 200)] == doctest::Approx(1.0f).epsilon(1e-6));
  }
}

TEST_CASE("SM.4 warp voice: Tones keeps a sine's pitch at another tempo; the rate moves it by exactly that ratio") {
  const std::vector<float> s = sine(48000 * 8, 220.0);
  Src src(s);
  WarpVoiceRun run;
  run.mode = AudioWarp::Tones;
  run.end = static_cast<double>(s.size());
  run.sampleRate = kSr;
  for (const double bpm : {60.0, 240.0})
    for (const float rate : {1.0f, 1.5f}) {
      CAPTURE(bpm);
      CAPTURE(rate);
      const Out o = play(src.source(), run, 48000 * 3, 64, [&](size_t) { return bpm; }, [&](size_t) { return rate; });
      const double hz = crossingHz(o.l, 4800, 48000 * 3 - 4800);
      MESSAGE("Tones at ", bpm, " BPM, rate ", rate, ": ", hz, " Hz");
      CHECK(std::abs(1200.0 * std::log2(hz / (220.0 * rate))) < 0.5);
    }
}

TEST_CASE("SM.4 warp voice: a glide and a vibrato are continuous in every mode (no step above the threshold)") {
  // A 440 Hz sine of amplitude 0.5: at rate 2 its largest per-sample step is 2π·880/48000·0.5 ≈ 0.0576. A position
  // jump of a grain / slice reader would show as a step of up to 1. Threshold: 1.1 × the smooth maximum (measured: 1.00 ×).
  const std::vector<float> s = sine(48000 * 6, 440.0);
  std::vector<float> slices;
  for (int k = 0; k < 12; ++k) slices.push_back(static_cast<float>(k * 24000));
  Src src(s, slices);
  const double threshold = 1.1 * 2.0 * std::numbers::pi * 880.0 / kSr * 0.5;
  for (const AudioWarp mode : {AudioWarp::Beats, AudioWarp::Tones, AudioWarp::Texture})
    for (const bool vibrato : {false, true}) {
      CAPTURE(static_cast<int>(mode));
      CAPTURE(vibrato);
      WarpVoiceRun run;
      run.mode = mode;
      run.end = static_cast<double>(s.size());
      run.sampleRate = kSr;
      run.set.sliceLoop = WarpSliceLoop::Forward;
      auto rateAt = [&](size_t t) {
        const double x = static_cast<double>(t) / kSr;
        if (vibrato) return static_cast<float>(1.5 * std::exp2(0.5 / 12.0 * std::sin(2.0 * std::numbers::pi * 6.0 * x)));
        return static_cast<float>(x < 0.5 ? 1.0 : x < 1.0 ? 1.0 + (x - 0.5) * 2.0 : 2.0);  // a 0.5 s glide 1 → 2
      };
      const Out o = play(src.source(), run, 48000 * 2, 64, [](size_t) { return 120.0; }, rateAt);
      double worst = 0.0;
      size_t at = 0;
      for (size_t t = 2; t < o.l.size(); ++t) {
        const double d = std::abs(static_cast<double>(o.l[t]) - o.l[t - 1]);
        if (d > worst) worst = d, at = t;
      }
      MESSAGE("worst step ", worst, " at ", at, " (threshold ", threshold, ")");
      CHECK(worst < threshold);
      double peak = 0.0;
      for (float y : o.l) peak = std::max(peak, static_cast<double>(std::abs(y)));
      CHECK(peak > 0.4);
    }
}

TEST_CASE("SM.4 warp voice: renders do not depend on the call length; the end fades and stops the voice; loops go on") {
  std::vector<float> s(48000 * 2);
  for (size_t i = 0; i < s.size(); ++i)
    s[i] = static_cast<float>(0.4 * std::sin(0.031 * i) + 0.3 * std::sin(0.0071 * i * (1.0 + i / 96000.0)));
  std::vector<float> slices = {0.0f, 7000.0f, 19000.0f, 30000.0f, 52000.0f, 70000.0f};
  Src src(s, slices, {0.0f, 0.0f, 30000.0f, 1.0f, 96000.0f, 3.5f});
  for (const AudioWarp mode : {AudioWarp::Beats, AudioWarp::Tones, AudioWarp::Texture})
    for (const WarpSliceLoop sl : {WarpSliceLoop::Off, WarpSliceLoop::Forward, WarpSliceLoop::BackForward})
      for (const bool loop : {false, true}) {
        if (mode != AudioWarp::Beats && sl != WarpSliceLoop::Forward) continue;
        CAPTURE(static_cast<int>(mode));
        CAPTURE(static_cast<int>(sl));
        CAPTURE(loop);
        WarpVoiceRun run;
        run.mode = mode;
        run.start = 3000.0;
        run.end = 60000.0;
        run.loopStart = loop ? 20000.0 : 0.0;
        run.loopEnd = loop ? 50000.0 : 0.0;
        run.sampleBpm = 100.0;
        run.sampleRate = kSr;
        run.set.flux = 60.0;
        run.set.envelope = 70.0f;
        run.set.sliceLoop = sl;
        auto rateAt = [](size_t t) { return static_cast<float>(0.8 + 0.5 * std::sin(static_cast<double>(t) * 1e-4)); };
        // Constant tempo: identical for every call length.
        std::vector<Out> flat;
        for (const uint32_t block : {64u, 1u, 37u, 128u, 512u})
          flat.push_back(play(src.source(), run, 48000 * 3, block, [](size_t) { return 97.0; }, rateAt));
        for (size_t k = 1; k < flat.size(); ++k) {
          CHECK(flat[k].l == flat[0].l);
          CHECK(flat[k].r == flat[0].r);
          CHECK(flat[k].playing == flat[0].playing);
        }
        // A tempo change (read per call, as per engine block) on a sample every call length here shares: the same.
        auto bpmAt = [](size_t t) { return t < 30720 ? 85.0 : 140.0; };
        const Out c64 = play(src.source(), run, 48000 * 3, 64, bpmAt, rateAt);
        CHECK(play(src.source(), run, 48000 * 3, 512, bpmAt, rateAt).l == c64.l);
        CHECK(play(src.source(), run, 48000 * 3, 128, bpmAt, rateAt).l == c64.l);
        CHECK(c64.l != flat[0].l);
        double peak = 0.0;
        for (float y : flat[0].l) peak = std::max(peak, static_cast<double>(std::abs(y)));
        CHECK(peak > 0.1);
        const Out& o = flat[0];
        if (loop) {
          CHECK(o.playing.back() == 1.0f);
        } else {
          // Stopped well before 3 s (57 000 frames at 97 / 100 BPM ≈ 1.2 s of content), with a fade, then silent.
          auto off = std::find(o.playing.begin(), o.playing.end(), 0.0f);
          REQUIRE(off != o.playing.end());
          const auto at = static_cast<size_t>(off - o.playing.begin());
          for (size_t t = at; t < o.l.size(); ++t) REQUIRE(o.l[t] == 0.0f);
        }
      }
}

// SM.15: one Beats renderer. Its constant-step pieces (events by integer division, vector ramps) must give what the
// per-sample evaluation gives (WarpVoiceRun::perSample forces it), and a render must not depend on the call length or
// on where a glide starts and stops — a chunk with a constant step takes the pieces, one with a moving step the
// per-sample form, and the chunks fall differently for every call length.
TEST_CASE("SM.15: the Beats constant-step pieces equal the per-sample evaluation; block size and glides do not matter") {
  std::vector<float> s(48000 * 2);
  for (size_t i = 0; i < s.size(); ++i)
    s[i] = static_cast<float>(0.4 * std::sin(0.031 * i) + 0.3 * std::sin(0.0071 * i * (1.0 + i / 96000.0)));
  const std::vector<float> slices = {0.0f, 7000.0f, 19000.0f, 30000.0f, 52000.0f, 70000.0f};
  Src src(s, slices, {0.0f, 0.0f, 30000.0f, 1.0f, 96000.0f, 3.5f});
  struct Shape {
    const char* name;
    float (*rate)(size_t);
  };
  const Shape shapes[] = {
      {"rate 1", [](size_t) { return 1.0f; }},
      {"rate 0.83", [](size_t) { return 0.83f; }},
      {"rate 2.3 (a mip level)", [](size_t) { return 2.3f; }},
      // Constant, a glide up over 0.2 s, constant again, a step down, a short vibrato, constant.
      {"glides", [](size_t t) {
         if (t < 20011) return 1.0f;
         if (t < 29600) return 1.0f + 0.5f * static_cast<float>(t - 20011) / 9589.0f;
         if (t < 61000) return 1.5f;
         if (t < 90000) return 0.75f;
         if (t < 97000) return 0.75f + 0.05f * std::sin(static_cast<float>(t - 90000) * 0.01f);
         return 0.75f;
       }},
  };
  for (const Shape& shape : shapes)
    for (const WarpSliceLoop sl : {WarpSliceLoop::Off, WarpSliceLoop::Forward, WarpSliceLoop::BackForward})
      for (const bool loop : {false, true})
        for (const float envelope : {100.0f, 60.0f})
          for (const double bpm : {70.0, 97.0, 150.0}) {  // slower than the sample (slices loop) … faster (cut)
            CAPTURE(shape.name);
            CAPTURE(static_cast<int>(sl));
            CAPTURE(loop);
            CAPTURE(envelope);
            CAPTURE(bpm);
            WarpVoiceRun run;
            run.mode = AudioWarp::Beats;
            run.start = 3000.0;
            run.end = 60000.0;
            run.loopStart = loop ? 20000.0 : 0.0;
            run.loopEnd = loop ? 50000.0 : 0.0;
            run.sampleBpm = 100.0;
            run.sampleRate = kSr;
            run.set.envelope = envelope;
            run.set.sliceLoop = sl;
            const size_t frames = 48000 * 5 / 2;
            auto bpmAt = [&](size_t) { return bpm; };
            const Out ref = play(src.source(), run, frames, 64, bpmAt, shape.rate);
            double peak = 0.0;
            for (float y : ref.l) peak = std::max(peak, static_cast<double>(std::abs(y)));
            CHECK(peak > 0.1);
            WarpVoiceRun per = run;
            per.perSample = true;
            const Out ps = play(src.source(), per, frames, 64, bpmAt, shape.rate);
            CHECK(ps.l == ref.l);
            CHECK(ps.r == ref.r);
            for (const uint32_t block : {1u, 37u, 128u, 500u}) {
              CAPTURE(block);
              const Out o = play(src.source(), run, frames, block, bpmAt, shape.rate);
              CHECK(o.l == ref.l);
              CHECK(o.r == ref.r);
              CHECK(o.playing == ref.playing);
            }
          }
}

// SM.15: one window (warp.h warpHannGain): a pure function of the frame, whatever the call's split; grains sum to 1.
TEST_CASE("SM.15: the Hann gain is the same for every split of a grain, and overlapping grains sum to 1") {
  for (const uint32_t N : {2u, 96u, 2880u, 48000u}) {
    CAPTURE(N);
    std::vector<float> whole(N), parts(N);
    warpHannGain(N, 0, nullptr, 1.0f, whole.data(), N);
    for (uint32_t at = 0, k = 0; at < N; ++k) {
      const uint32_t n = std::min<uint32_t>(N - at, 1 + (k * 7) % 23);
      warpHannGain(N, at, nullptr, 1.0f, parts.data() + at, n);
      at += n;
    }
    CHECK(parts == whole);
    CHECK(hannWindow(N) == whole);
    double worst = 0.0, off = 0.0;
    for (uint32_t j = 0; j < N / 2; ++j) {
      worst = std::max(worst, std::abs(static_cast<double>(whole[j]) + static_cast<double>(whole[j + N / 2]) - 1.0));
      off = std::max(off, std::abs(whole[j] - (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * j / N))));
    }
    CHECK(worst < 6e-8);  // (one rounding of 1 − s)
    CHECK(off < 2e-7);  // (float: the phase and the polynomial each round)
  }
}
