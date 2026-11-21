// SM.5: the Sampler's Complex / Complex Pro voice (pq/dsp/sampler_stretch.h):
//   - note-on: Tones (bit-identical to a Tones voice) while the stretcher primes, the 2 ms cross, then the vocoder;
//     priming spread (no call computes more than kComplexPrimeRate × its frames + a chunk of vocoder frames)
//   - pitch: a sine at +7 steps of 31-EDO (18\31) within 0.5 ¢, stretched to another tempo; after a glide the held
//     pitch within 0.5 ¢ (the vocoder output resampled at the per-sample rate)
//   - content timing: beat bursts stay on the song's beats (energy centroid ± 6 ms) at another tempo, through glides
//   - renders do not depend on the call length (1, 37, 128, 256)
//   - ownership: a copy of a voice's words (a Sync, a hot-swap's transfer) loses the stretcher to the first that runs
//     and plays Tones; a trigger frees what its lane held; an exhausted bank plays Tones and claims when one frees
//   - phase offsets: voices triggered together compute their chunks on different samples
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/dsp/sample_slot.h"
#include "pq/dsp/sampler_stretch.h"

using namespace pq;
using namespace pq::dsp;

namespace {

constexpr double kSr = 48000.0;

struct Src {
  EpochReclaimer reclaimer{1};
  SampleSlot slot;
  std::shared_ptr<const SampleData> data;
  explicit Src(const std::vector<float>& l, double fileRate = kSr) {
    data = SampleData::build(l).value();
    REQUIRE(slot.publish(SampleData::source(data, fileRate), kSr, reclaimer).ok());
  }
  WarpSource source() const {
    const SampleSlot::Entry& e = *slot.acquire();
    WarpSource s;
    s.level = e.level;
    s.levels = e.levels;
    s.channels = e.channels;
    s.fileRate = e.fileRate;
    s.rateScale = e.rateScale;
    return s;
  }
};

struct Scratch {
  std::vector<float> gain = std::vector<float>(kWarpVoiceChunk + 16);
  std::vector<float> a = std::vector<float>(kWarpMaxAlignLen + 64);
  std::vector<float> b = std::vector<float>(kWarpMaxAlignLen + 2 * kWarpMaxAlignLags + 64);
  std::vector<float> g = std::vector<float>(2 * kWarpGatherFrames);
  WarpScratch ws() { return {gain.data(), a.data(), b.data(), g.data()}; }
};

struct Voice {
  WarpVoice v;
  ComplexVoice cv;
};

struct Out {
  std::vector<float> l, r;
  uint64_t maxChunks = 0;  // the most chunks one call computed
};

// A voice from a trigger at frame 0, rendered in calls of `block` (≤ 256); bank null: a Tones voice (WarpVoice only).
template <class Rate>
Out play(const WarpSource& src, WarpVoiceRun run, SamplerStretch* bank, size_t frames, uint32_t block, Rate rateAt,
         const ComplexRun& cr = {}) {
  Scratch sc;
  Voice x;
  x.v.trigger(src, run);
  if (bank != nullptr) x.cv.trigger(bank, 0);
  Out o;
  o.l.resize(frames), o.r.resize(frames);
  std::vector<float> rate(block), pos(block), playing(block);
  DenormalGuard dg;
  for (size_t t = 0; t < frames; t += block) {
    const auto n = static_cast<uint32_t>(std::min<size_t>(block, frames - t));
    for (uint32_t i = 0; i < n; ++i) rate[i] = rateAt(t + i);
    const uint64_t c0 = bank != nullptr ? bank->chunks : 0;
    if (bank != nullptr)
      x.cv.render(bank, 0, x.v, src, run, cr, rate.data(), n, o.l.data() + t, o.r.data() + t, pos.data(), playing.data(), sc.ws());
    else
      x.v.render(src, run, rate.data(), n, o.l.data() + t, o.r.data() + t, pos.data(), playing.data(), sc.ws());
    if (bank != nullptr) o.maxChunks = std::max(o.maxChunks, bank->chunks - c0);
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

double cents(double a, double b) { return 1200.0 * std::log2(a / b); }

WarpVoiceRun loopRun(size_t len, double bpm, double sampleBpm) {
  WarpVoiceRun run;
  run.mode = WarpVoiceMode::Tones;
  run.start = 0.0;
  run.end = static_cast<double>(len);
  run.loopStart = 0.0;
  run.loopEnd = static_cast<double>(len);
  run.bpm = bpm;
  run.sampleBpm = sampleBpm;
  run.sampleRate = kSr;
  run.grainSeconds = 0.04;
  return run;
}

}  // namespace

TEST_CASE("SM.5 complex voice: Tones while priming (bit-identical), then the vocoder; priming never bursts") {
  const std::vector<float> s = sine(96000, 220.0);
  Src src(s);
  const WarpSource ws = src.source();
  auto bank = SamplerStretch::build(1, 1, kSr);
  const WarpVoiceRun run = loopRun(s.size(), 100.0, 120.0);
  auto one = [](size_t) { return 1.0f; };
  const Out tones = play(ws, run, nullptr, 24000, 64, one);
  const Out cx = play(ws, run, bank.get(), 24000, 64, one);
  // The first 60 ms are the Tones voice's samples exactly; by 120 ms the vocoder plays (it differs from Tones).
  size_t same = 0;
  while (same < cx.l.size() && cx.l[same] == tones.l[same]) ++same;
  CHECK(same >= 2880);
  CHECK(same < 5760);
  CHECK(cx.l[20000] != tones.l[20000]);
  // At most kComplexPrimeRate × 64 + a chunk of vocoder frames per 64-frame call while priming (no burst).
  CHECK(cx.maxChunks <= (kComplexPrimeRate * 64) / kComplexChunk + 1);
  CHECK(bank->owner(0).token != 0);  // held while it plays
}

TEST_CASE("SM.5 complex voice: pitch within 0.5 cents at +7 steps of 31-EDO, stretched; and after a glide") {
  const double hz = 440.0;
  const std::vector<float> s = sine(144000, hz);
  Src src(s);
  const WarpSource ws = src.source();
  auto bank = SamplerStretch::build(1, 1, kSr);
  const double ratio = std::exp2(18.0 / 31.0);  // 18\31: the 31-EDO fifth, ≈ +7 semitones
  for (const double bpm : {120.0, 87.0, 150.0}) {
    CAPTURE(bpm);
    const WarpVoiceRun run = loopRun(s.size(), bpm, 120.0);
    const Out o = play(ws, run, bank.get(), 96000, 128, [&](size_t) { return static_cast<float>(ratio); });
    const double f = crossingHz(o.l, 24000, 96000);
    CHECK(std::abs(cents(f, hz * ratio)) < 0.5);
  }
  // A glide 1 → 1.5 over 0.4 s from 0.2 s (a time-varying stretch inside the vocoder), then held.
  const WarpVoiceRun run = loopRun(s.size(), 97.0, 120.0);
  auto glide = [](size_t t) {
    const double u = std::clamp((static_cast<double>(t) - 9600.0) / 19200.0, 0.0, 1.0);
    return static_cast<float>(1.0 + 0.5 * u);
  };
  const Out o = play(ws, run, bank.get(), 120000, 100, glide);
  CHECK(std::abs(cents(crossingHz(o.l, 48000, 120000), hz * 1.5)) < 0.5);
  // Continuous: no step between adjacent samples beyond 1.25 × the largest slope of the source sine at rate 1.5
  // (the 2 ms cross from Tones adds two sines of the same pitch).
  const double slope = 0.5 * 2.0 * std::numbers::pi * hz * 1.5 / kSr;
  double worst = 0.0;
  for (size_t t = 1; t < o.l.size(); ++t) worst = std::max(worst, static_cast<double>(std::abs(o.l[t] - o.l[t - 1])));
  CHECK(worst < 1.25 * slope);
}

TEST_CASE("SM.5 complex voice: beat bursts stay on the song's beats at another tempo, through glides") {
  // A burst of a 440 Hz sine at every beat of 120 BPM (24 000 frames), 30 ms long.
  std::vector<float> s(24000 * 8, 0.0f);
  for (size_t b = 0; b < 8; ++b)
    for (size_t i = 0; i < 1440; ++i)
      s[b * 24000 + i] = static_cast<float>(0.6 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kSr));
  Src src(s);
  const WarpSource ws = src.source();
  auto bank = SamplerStretch::build(1, 1, kSr);
  const double bpm = 150.0;  // a beat every 19 200 output frames
  const WarpVoiceRun run = loopRun(s.size(), bpm, 120.0);
  auto wobble = [](size_t t) {  // glides up and down: 1 → 1.4 → 0.8 → 1
    const double x = static_cast<double>(t) / kSr;
    return static_cast<float>(x < 1.0 ? 1.0 + 0.4 * x : x < 2.0 ? 1.4 - 0.6 * (x - 1.0) : std::min(1.0, 0.8 + 0.2 * (x - 2.0)));
  };
  const Out o = play(ws, run, bank.get(), 19200 * 9, 256, wobble);
  // Each beat k ≥ 1 (the Tones onset is beat 0): the energy centroid of its burst, expected 15 ms of content
  // (12 ms of output at 150 / 120) after the beat. Measured: ≈ 1 ms late at a held or slowly gliding rate; ≈ 5 ms late
  // in the fast descent (1.4 → 0.8 in a second, ≈ 10 st/s): the vocoder's content is placed ≈ 100 ms ahead of the read
  // at the rate of the moment (sampler_stretch.h "t_y"), and recovers once the rate settles.
  for (size_t k = 1; k < 9; ++k) {
    CAPTURE(k);
    const size_t at = k * 19200;
    double e = 0.0, et = 0.0;
    for (size_t t = at - 9600; t < at + 9600; ++t) {
      const double x = static_cast<double>(o.l[t]) * static_cast<double>(o.l[t]);
      e += x;
      et += x * (static_cast<double>(t) - static_cast<double>(at));
    }
    REQUIRE(e > 0.0);
    CHECK(std::abs(et / e - 0.012 * kSr) < 0.006 * kSr);
  }
}

TEST_CASE("SM.5 complex voice: renders do not depend on the call length") {
  const std::vector<float> s = sine(60000, 330.0);
  Src src(s);
  const WarpSource ws = src.source();
  auto bank = SamplerStretch::build(1, 1, kSr);
  WarpVoiceRun run = loopRun(s.size(), 133.0, 100.0);
  run.loopStart = 9000.0;  // a loop inside the sample (the stream unfolds through it)
  run.loopEnd = 41000.0;
  auto rateAt = [](size_t t) { return static_cast<float>(1.2 + 0.3 * std::sin(static_cast<double>(t) * 2e-4)); };
  const ComplexRun pro{true, 70.0, 64.0};
  for (const ComplexRun& cr : {ComplexRun{}, pro}) {
    const Out ref = play(ws, run, bank.get(), 40000, 64, rateAt, cr);
    for (const uint32_t block : {1u, 37u, 128u, 256u}) {
      CAPTURE(block);
      const Out o = play(ws, run, bank.get(), 40000, block, rateAt, cr);
      CHECK(o.l == ref.l);
      CHECK(o.r == ref.r);
    }
  }
  CHECK(play(ws, run, bank.get(), 40000, 64, rateAt, pro).l != play(ws, run, bank.get(), 40000, 64, rateAt).l);
}

TEST_CASE("SM.5 complex voice: ownership — a copy loses to the first that runs; triggers free their lane; exhaustion") {
  const std::vector<float> s = sine(48000, 250.0);
  Src src(s);
  const WarpSource ws = src.source();
  auto bank = SamplerStretch::build(2, 1, kSr);
  const WarpVoiceRun run = loopRun(s.size(), 120.0, 120.0);
  Scratch sc;
  std::vector<float> rate(128, 1.0f), l(128), r(128), pos(128), pl(128);
  auto step = [&](Voice& x, uint32_t lane) {
    x.cv.render(bank.get(), lane, x.v, ws, run, {}, rate.data(), 128, l.data(), r.data(), pos.data(), pl.data(), sc.ws());
  };
  Voice a;
  a.v.trigger(ws, run);
  a.cv.trigger(bank.get(), 0);
  CHECK(a.cv.stretcher == 0);
  for (int i = 0; i < 60; ++i) step(a, 0);  // primed and crossed over
  CHECK(a.cv.phase == ComplexVoice::kOn);
  // A copy of its words in lane 5 (a Sync): the original runs first and keeps it; the copy plays Tones and claims the
  // free stretcher 1.
  Voice b = a;
  step(a, 0);
  step(b, 5);
  CHECK(a.cv.phase == ComplexVoice::kOn);
  CHECK(b.cv.stretcher == 1);
  CHECK(b.cv.phase == ComplexVoice::kPending);
  step(a, 0);
  CHECK(a.cv.phase == ComplexVoice::kOn);
  // Exhausted: a third voice gets none (Tones, counted) until one is free.
  Voice c;
  c.v.trigger(ws, run);
  const uint64_t misses = bank->misses;
  c.cv.trigger(bank.get(), 3);
  CHECK(c.cv.phase == ComplexVoice::kWait);
  CHECK(bank->misses == misses + 1);
  step(c, 3);
  CHECK(c.cv.phase == ComplexVoice::kWait);
  // A trigger in lane 0 frees what lane 0 held (its previous voice is over) and takes it again.
  Voice d;
  d.v.trigger(ws, run);
  d.cv.trigger(bank.get(), 0);
  CHECK(d.cv.stretcher == 0);
  step(a, 0);  // the old words: lost
  CHECK(a.cv.phase != ComplexVoice::kOn);
  // b's voice ends (a one-shot end): its stretcher frees; c claims it on its next call.
  b.v.stop();
  step(b, 5);
  CHECK(bank->owner(1).token == 0);
  step(c, 3);
  CHECK(c.cv.stretcher == 1);
  CHECK(c.cv.phase == ComplexVoice::kPending);
}

TEST_CASE("SM.5 complex voice: voices triggered together compute their chunks on different samples (phase offsets)") {
  const std::vector<float> s = sine(48000, 250.0);
  Src src(s);
  const WarpSource ws = src.source();
  auto bank = SamplerStretch::build(8, 1, kSr);
  const WarpVoiceRun run = loopRun(s.size(), 120.0, 120.0);
  Scratch sc;
  std::vector<Voice> v(8);
  for (uint32_t k = 0; k < 8; ++k) {
    v[k].v.trigger(ws, run);
    v[k].cv.trigger(bank.get(), k);
  }
  std::vector<float> rate(1, 1.0f), l(1), r(1), pos(1), pl(1);
  // Frame by frame, after priming: per sample how many voices computed a chunk; with the leads at most 2 at once.
  uint64_t worst = 0;
  for (int t = 0; t < 30000; ++t) {
    const uint64_t c0 = bank->chunks;
    for (uint32_t k = 0; k < 8; ++k)
      v[k].cv.render(bank.get(), k, v[k].v, ws, run, {}, rate.data(), 1, l.data(), r.data(), pos.data(), pl.data(), sc.ws());
    if (t > 12000) worst = std::max(worst, bank->chunks - c0);
  }
  for (uint32_t k = 0; k < 8; ++k) CHECK(v[k].cv.phase == ComplexVoice::kOn);
  CHECK(worst <= 2);
}

TEST_CASE("SM.5 complex voice: 10 000 calls of note-ons and glides allocate nothing (RtScope)") {
  REQUIRE(alloc_trap::installed());
  const std::vector<float> s = sine(30000, 300.0);
  Src src(s);
  const WarpSource ws = src.source();
  auto bank = SamplerStretch::build(4, 1, kSr);
  WarpVoiceRun run = loopRun(s.size(), 120.0, 110.0);
  Scratch sc;
  std::vector<Voice> v(4);
  std::vector<float> rate(64), l(64), r(64), pos(64), pl(64);
  uint64_t violations = 0;
  for (uint32_t b = 0; b < 10000; ++b) {
    run.bpm = 80.0 + (b / 50) % 7 * 20.0;
    const ComplexRun cr{(b / 300) % 2 == 1, 80.0, 128.0};
    violations += alloc_trap::countRtViolations([&] {
      for (uint32_t k = 0; k < 4; ++k) {
        if ((b + k * 13) % 97 == 0) {
          v[k].v.trigger(ws, run);
          v[k].cv.trigger(bank.get(), k);
        }
        for (uint32_t i = 0; i < 64; ++i) rate[i] = static_cast<float>(0.7 + 0.6 * ((b * 64 + i + k * 1000) % 9000) / 9000.0);
        v[k].cv.render(bank.get(), k, v[k].v, ws, run, cr, rate.data(), 64, l.data(), r.data(), pos.data(), pl.data(), sc.ws());
      }
    });
  }
  CHECK(violations == 0);
  CHECK(bank->chunks > 1000);
}
