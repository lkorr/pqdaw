// T5.4: the clip player (pq/engine/clip_player.h) and the audio sequence image (pq/engine/audio_sequence.h), driven
// through the Timeline as the engine drives it:
//   - sample-exact placement (warp Off, rate 1) at 60 / 120 / 137.5 BPM and under a tempo ramp, ±0 samples
//   - renders bit-identical across engine blocks 32 / 64 / 256 / 1024 (Off clips: gain, fades, crossfade, reverse, ±7 st)
//   - RePitch within one sample of the analytic tempo integral at 10 min of a ramping tempo (loop folding included)
//   - gain, fade curves, equal-power crossfades, reverse, transpose, edge declicks, missing samples
//   - sleep: no voice and no scan before a clip's look-ahead window; the slot is inactive outside its clips
//   - click-free swaps (gain ramp, moved clip, identical republish bit-exact), stop / locate fades
//   - streamed samples (media::SamplePool + disk thread): offline blocking fills equal a resident render bit for bit;
//     a locate prefetch lets playback start with data ready; a looped RePitch clip past the resident head wraps
//     without underruns in real time (its next loop pass is claimed ahead, Q151 item 7), forward and reverse
//   - the clipPlayer atom reads its slot through ProcessContext::clipIn
//   - above rate 2 a resident pooled sample with mips is read on its decimated copies (Q151 item 1): the same audio as
//     the direct read within the resampler's error, allocation-free; without mips, or streamed, it reads directly
//   - RtScope over every RT entry point (clip boundaries, crossfades, loop wraps, swaps, underruns)
#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <chrono>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "interp_harness.h"
#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/automation_curve.h"
#include "pq/engine/audio_sequence.h"
#include "pq/engine/clip_player.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/timeline.h"
#include "pq/media/sample_pool.h"
#include "pq/media/wav_file.h"

namespace {

namespace e = pq::engine;
namespace fs = std::filesystem;
using pq::kPpq;
using pq::SampleTime;
using pq::Ticks;

constexpr double kSr = 48000.0;
constexpr int64_t kG = pq::media::kSampleGuardFrames;

struct Frames {
  pq::AlignedBuffer<float> buf;
};

// An in-memory sample (fully resident, zero guards) of `frames` frames, value fn(channel, frame).
e::ClipSample makeSample(int64_t frames, uint32_t channels, const std::function<float(uint32_t, int64_t)>& fn,
                         uint32_t rate = 48000) {
  auto o = std::make_shared<Frames>();
  const int64_t stride = frames + 2 * kG;
  o->buf = pq::AlignedBuffer<float>(static_cast<size_t>(stride) * channels);
  e::ClipSample s;
  for (uint32_t c = 0; c < channels; ++c) {
    float* d = o->buf.data() + static_cast<size_t>(c) * stride + kG;
    for (int64_t i = 0; i < frames; ++i) d[i] = fn(c, i);
    s.fixed.data[c] = d;
  }
  s.fixed.frames = s.fixed.residentFrames = frames;
  s.fixed.channels = channels;
  s.fixed.sampleRate = rate;
  s.owner = o;
  return s;
}

e::AudioClipDesc offClip(uint32_t id, Ticks start, Ticks length, double speed = kSr, double contentStartFrame = 0.0,
                         uint32_t sample = 0) {
  e::AudioClipDesc c;
  c.id = id;
  c.sample = sample;
  c.start = start;
  c.length = length;
  c.warp = e::ClipWarp::Off;
  c.speed = speed;
  c.contentStartFrame = contentStartFrame;
  return c;
}

pq::TempoMap tempoMap(std::vector<pq::TempoPoint> pts) {
  auto r = pq::TempoMap::create(pts);
  REQUIRE(r.ok());
  return std::move(r.value());
}

// A timeline driven block by block, the clip slots' audio collected.
struct Rig {
  e::TimelineConfig cfg;
  std::unique_ptr<e::Timeline> tl;
  pq::TempoMap map;
  std::vector<pq::TempoPoint> points{{0, 120.0, pq::TempoCurve::Step}};
  int64_t time = 0;

  explicit Rig(uint32_t block = 64, double declick = 0.002, pq::media::StreamPool* streams = nullptr) {
    cfg.sampleRate = kSr;
    cfg.maxBlockFrames = block;
    cfg.maxTracks = 8;
    cfg.clipDeclickSeconds = declick;
    cfg.streams = streams;
    tl = std::make_unique<e::Timeline>(cfg);
  }
  void tempo(std::vector<pq::TempoPoint> pts) {
    points = pts;
    map = tempoMap(pts);
    auto img = e::TempoImage::create(pts, {});
    REQUIRE(img.ok());
    REQUIRE(tl->publishTempo(std::move(img.value())).ok());
  }
  std::unique_ptr<e::AudioSequenceImage> build(std::vector<e::AudioTrackDesc> slots, std::vector<e::ClipSample> samples,
                                               bool edgeDeclick = true) const {
    e::AudioBuildOptions o;
    o.sampleRate = kSr;
    o.edgeDeclick = edgeDeclick;
    auto r = e::AudioSequenceImage::build(slots, std::move(samples), map, o);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return std::move(r.value());
  }
  void audio(std::vector<e::AudioTrackDesc> slots, std::vector<e::ClipSample> samples, bool edgeDeclick = true) {
    REQUIRE(tl->publishAudio(build(std::move(slots), std::move(samples), edgeDeclick)).ok());
  }
  void command(e::TransportCommandKind k, Ticks t = 0) {
    e::TransportCommand c;
    c.kind = k;
    c.tick = t;
    tl->apply(c);
  }
  // Renders `frames` frames; returns slot `slot`'s channel `ch` (and appends channel 1 to *right when given).
  std::vector<float> render(uint64_t frames, uint32_t slot = 0, uint32_t ch = 0, std::vector<float>* right = nullptr) {
    std::vector<float> out;
    out.reserve(frames);
    for (uint64_t done = 0; done < frames;) {
      const auto n = static_cast<uint32_t>(std::min<uint64_t>(cfg.maxBlockFrames, frames - done));
      tl->process(time, n);
      time += n;
      const float* const* chans = tl->clipPlayer().channels();
      const float* p = chans[slot * e::kClipChannels + ch];
      out.insert(out.end(), p, p + n);
      if (right != nullptr) {
        const float* r = chans[slot * e::kClipChannels + 1];
        right->insert(right->end(), r, r + n);
      }
      done += n;
    }
    return out;
  }
};

double maxStep(const std::vector<float>& v, size_t from, size_t to) {
  double m = 0.0;
  for (size_t i = std::max<size_t>(from, 1); i < std::min(to, v.size()); ++i) m = std::max(m, std::abs(static_cast<double>(v[i]) - v[i - 1]));
  return m;
}

}  // namespace

TEST_CASE("clip player: sample-exact placement at 60 / 120 / 137.5 BPM and under a tempo ramp (warp Off, rate 1)") {
  const std::vector<std::vector<pq::TempoPoint>> maps = {
      {{0, 60.0, pq::TempoCurve::Step}},
      {{0, 120.0, pq::TempoCurve::Step}},
      {{0, 137.5, pq::TempoCurve::Step}},
      {{0, 90.0, pq::TempoCurve::Linear}, {kPpq * 16, 150.0, pq::TempoCurve::Linear}, {kPpq * 24, 70.0, pq::TempoCurve::Step}},
  };
  for (const auto& pts : maps) {
    for (const bool declick : {false, true}) {
      for (const Ticks T : {Ticks{0}, kPpq * 3 + 12345, kPpq * 17 + 777}) {
        const int64_t k = declick ? 1000 : 0;  // with edge declicks the impulse sits past the 2 ms fade in
        CAPTURE(pts.front().bpm);
        CAPTURE(declick);
        CAPTURE(T);
        Rig rig(64, declick ? 0.002 : 0.0);
        rig.tempo(pts);
        const int64_t frames = 4000;
        rig.audio({{1, {offClip(1, T, kPpq * 4)}}}, {makeSample(frames, 1, [&](uint32_t, int64_t i) { return i == k ? 1.0f : 0.0f; })},
                  declick);
        rig.command(e::TransportCommandKind::Play, 0);
        const SampleTime S0 = rig.map.ticksToSamples(T, kSr);
        const std::vector<float> out = rig.render(static_cast<uint64_t>(S0 + k + 200));
        for (size_t i = 0; i < out.size(); ++i) {
          if (static_cast<int64_t>(i) == S0 + k) REQUIRE(out[i] == 1.0f);
          else REQUIRE(out[i] == 0.0f);
        }
      }
    }
  }
}

TEST_CASE("clip player: renders are bit-identical across engine blocks 32 / 64 / 256 / 1024 (Off clips)") {
  auto noise = [](uint32_t c, int64_t i) {
    return static_cast<float>(std::sin(0.0137 * static_cast<double>(i) + c) * 0.5 + 0.3 * std::sin(0.71 * static_cast<double>(i)));
  };
  std::vector<std::vector<float>> results;
  for (uint32_t block : {32u, 64u, 256u, 1024u}) {
    Rig rig(block);
    rig.tempo({{0, 128.0, pq::TempoCurve::Step}});
    std::vector<e::AudioClipDesc> clips;
    e::AudioClipDesc a = offClip(1, kPpq / 2, kPpq * 2, kSr, 100.0);
    a.gain = 0.7f;
    a.fadeIn = kPpq / 4;
    a.fadeInCurve = 0.5f;
    e::AudioClipDesc b = offClip(2, kPpq / 2 + kPpq * 2, kPpq * 2, kSr * std::pow(2.0, 7.0 / 12.0), 2000.3);
    b.crossfadeIn = kPpq / 3;
    b.fadeOut = kPpq / 2;
    b.fadeOutCurve = -0.7f;
    e::AudioClipDesc c = offClip(3, kPpq * 5, kPpq * 2, kSr * std::pow(2.0, -7.0 / 12.0), 500.0);
    c.reverse = true;
    e::AudioClipDesc d = offClip(4, kPpq * 7 + 13, kPpq, 44100.0, 9.25, 1);  // a 44.1 kHz file
    rig.audio({{1, {a, b, c, d}}}, {makeSample(200000, 2, noise), makeSample(100000, 1, noise, 44100)});
    rig.command(e::TransportCommandKind::Play, 0);
    std::vector<float> r;
    std::vector<float> l = rig.render(static_cast<uint64_t>(kSr * 4.2), 0, 0, &r);
    l.insert(l.end(), r.begin(), r.end());
    results.push_back(std::move(l));
  }
  for (size_t i = 1; i < results.size(); ++i) CHECK(results[i] == results[0]);
  double peak = 0.0;
  for (float v : results[0]) peak = std::max(peak, static_cast<double>(std::abs(v)));
  CHECK(peak > 0.3);  // it played
}

// M5 review M3: Re-Pitch content positions were linearised between the ends of each run, so they depended on the
// engine block (−20 dBFS differences at 1024 under a ramp, 0.008 at a constant tempo). They are now linear on a fixed
// grid of song samples: bit-identical across blocks, at a constant tempo, under a ramp, looped (wraps declicked) and
// reversed.
TEST_CASE("clip player: Re-Pitch renders are bit-identical across engine blocks 32 / 64 / 256 / 1024 (ramps, loops)") {
  auto noise = [](uint32_t c, int64_t i) {
    return static_cast<float>(std::sin(0.0137 * static_cast<double>(i) + c) * 0.5 + 0.3 * std::sin(0.71 * static_cast<double>(i)));
  };
  const std::vector<std::vector<pq::TempoPoint>> tempos = {
      {{0, 128.0, pq::TempoCurve::Step}},
      {{0, 90.0, pq::TempoCurve::Linear}, {kPpq * 8, 170.0, pq::TempoCurve::Linear}, {kPpq * 16, 60.0, pq::TempoCurve::Step}}};
  for (const auto& tp : tempos) {
    std::vector<std::vector<float>> results;
    for (uint32_t block : {32u, 64u, 256u, 1024u}) {
      CAPTURE(block);
      Rig rig(block);
      rig.tempo(tp);
      const double fpt = kSr * 60.0 / (100.0 * static_cast<double>(kPpq));  // a 100 BPM source
      e::AudioClipDesc a;
      a.id = 1;
      a.warp = e::ClipWarp::RePitch;
      a.start = kPpq / 3;
      a.length = kPpq * 6;
      a.contentStart = kPpq / 7;
      a.framesPerTick = fpt;
      a.gain = 0.8f;
      e::AudioClipDesc b = a;  // looped, crossfaded in
      b.id = 2;
      b.start = a.start + a.length;
      b.length = kPpq * 6;
      b.loop = {true, kPpq / 2, kPpq / 2 + kPpq * 3 / 5};
      b.contentStart = kPpq / 2;
      b.crossfadeIn = kPpq / 3;
      e::AudioClipDesc r = a;  // reversed
      r.id = 3;
      r.start = b.start + b.length;
      r.length = kPpq * 3;
      r.reverse = true;
      rig.audio({{1, {a, b, r}}}, {makeSample(400000, 2, noise)});
      rig.command(e::TransportCommandKind::Play, 0);
      std::vector<float> rr;
      std::vector<float> l = rig.render(static_cast<uint64_t>(kSr * 9.0), 0, 0, &rr);
      l.insert(l.end(), rr.begin(), rr.end());
      results.push_back(std::move(l));
    }
    for (size_t i = 1; i < results.size(); ++i) {
      size_t diff = 0;
      for (size_t k = 0; k < results[0].size(); ++k) diff += results[i][k] != results[0][k] ? 1u : 0u;
      CHECK(diff == 0);
    }
    double peak = 0.0;
    for (float v : results[0]) peak = std::max(peak, static_cast<double>(std::abs(v)));
    CHECK(peak > 0.3);
  }
}

TEST_CASE("clip player: gain, fade curves, equal-power crossfade, edge declicks") {
  Rig rig(64);
  rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
  // DC sources: what comes out is the envelope itself (rate 1 from integer positions: straight copies).
  e::AudioClipDesc a = offClip(1, 0, kPpq * 2);
  a.gain = 0.5f;
  a.fadeIn = kPpq / 2;
  a.fadeInCurve = 0.6f;
  e::AudioClipDesc b = offClip(2, kPpq * 2, kPpq * 2, kSr, 100000.0, 1);  // frames before its start: the pre-roll
  b.crossfadeIn = kPpq / 2;
  b.fadeOut = kPpq / 4;
  b.fadeOutCurve = -0.4f;
  rig.audio({{1, {a, b}}}, {makeSample(400000, 1, [](uint32_t, int64_t) { return 1.0f; }),
                            makeSample(400000, 1, [](uint32_t, int64_t) { return 0.25f; })});
  rig.command(e::TransportCommandKind::Play, 0);
  const std::vector<float> out = rig.render(static_cast<uint64_t>(kSr * 2.2));
  const SampleTime fadeEnd = rig.map.ticksToSamples(kPpq / 2, kSr);
  for (SampleTime t = 0; t < fadeEnd; t += 37) {
    const double x = static_cast<double>(t) / static_cast<double>(fadeEnd);
    REQUIRE(out[t] == doctest::Approx(0.5 * pq::automationShape(x, 0.6)).epsilon(1e-6));
  }
  const SampleTime x0 = rig.map.ticksToSamples(kPpq * 2 - kPpq / 4, kSr), x1 = rig.map.ticksToSamples(kPpq * 2 + kPpq / 4, kSr);
  CHECK(out[fadeEnd + 10] == 0.5f);  // flat: exactly the gain
  for (SampleTime t = x0; t < x1; t += 11) {
    const double u = (static_cast<double>(t - x0) + 0.5) / static_cast<double>(x1 - x0);
    const double want = 0.5 * std::cos(0.5 * std::numbers::pi * u) + 0.25 * std::sin(0.5 * std::numbers::pi * u);
    REQUIRE(out[t] == doctest::Approx(want).epsilon(1e-6));
  }
  const SampleTime bEnd = rig.map.ticksToSamples(kPpq * 4, kSr), fo = rig.map.ticksToSamples(kPpq * 4 - kPpq / 4, kSr);
  for (SampleTime t = fo; t < bEnd; t += 23) {
    const double x = static_cast<double>(t - fo) / static_cast<double>(bEnd - fo);
    REQUIRE(out[t] == doctest::Approx(0.25 * (1.0 - pq::automationShape(x, -0.4))).epsilon(1e-6));
  }
  CHECK(out[bEnd] == 0.0f);
  // Edge declick: a clip without fades ramps over 2 ms (96 frames) at both ends.
  Rig r2(64);
  r2.tempo({{0, 120.0, pq::TempoCurve::Step}});
  r2.audio({{1, {offClip(1, kPpq, kPpq)}}}, {makeSample(400000, 1, [](uint32_t, int64_t) { return 1.0f; })});
  r2.command(e::TransportCommandKind::Play, 0);
  const std::vector<float> o2 = r2.render(static_cast<uint64_t>(kSr * 1.2));
  const SampleTime s0 = r2.map.ticksToSamples(kPpq, kSr), s1 = r2.map.ticksToSamples(kPpq * 2, kSr);
  CHECK(o2[s0] == 0.0f);
  CHECK(o2[s0 + 48] == doctest::Approx(0.5).epsilon(1e-6));
  CHECK(o2[s0 + 96] == 1.0f);
  CHECK(o2[s1 - 97] == 1.0f);
  CHECK(o2[s1 - 1] == doctest::Approx(1.0 / 96.0).epsilon(1e-5));
  CHECK(o2[s1] == 0.0f);
  CHECK(maxStep(o2, 0, o2.size()) <= 1.0 / 96.0 + 1e-6);
}

// M5 review M7: a clip split and crossfaded back together (one sample, one continuous source law) reads the same
// frames from both sides of the window; equal power summed them to +3 dB (a DC source peaked at 1.4142). Equal gain
// keeps it at unity. A crossfade between different material stays equal power (the case above).
TEST_CASE("clip player: a crossfade across a split is equal gain (no swell); a cut clip ends at the next one's start") {
  Rig rig(64);
  rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
  // The left half plays frames [0, 2·24000), the right half continues at 48000 (a split at 2 beats).
  e::AudioClipDesc a = offClip(1, 0, kPpq * 2);
  e::AudioClipDesc b = offClip(2, kPpq * 2, kPpq * 2, kSr, 48000.0);
  b.crossfadeIn = kPpq / 2;
  rig.audio({{1, {a, b}}}, {makeSample(400000, 1, [](uint32_t, int64_t) { return 1.0f; })});
  rig.command(e::TransportCommandKind::Play, 0);
  const std::vector<float> out = rig.render(static_cast<uint64_t>(kSr * 1.8));
  const SampleTime x0 = rig.map.ticksToSamples(kPpq * 2 - kPpq / 4, kSr), x1 = rig.map.ticksToSamples(kPpq * 2 + kPpq / 4, kSr);
  float peak = 0.0f, low = 2.0f;
  for (SampleTime t = x0 - 100; t < x1 + 100; ++t) {
    peak = std::max(peak, out[static_cast<size_t>(t)]);
    low = std::min(low, out[static_cast<size_t>(t)]);
  }
  CHECK(peak <= 1.0f + 1e-6f);
  CHECK(low >= 1.0f - 1e-6f);
  // A different content position on the right (not a continuation): equal power, the +3 dB of identical DC shows.
  Rig r2(64);
  r2.tempo({{0, 120.0, pq::TempoCurve::Step}});
  e::AudioClipDesc b2 = b;
  b2.contentStartFrame = 9000.0;
  r2.audio({{1, {a, b2}}}, {makeSample(400000, 1, [](uint32_t, int64_t) { return 1.0f; })});
  r2.command(e::TransportCommandKind::Play, 0);
  const std::vector<float> o2 = r2.render(static_cast<uint64_t>(kSr * 1.8));
  CHECK(o2[static_cast<size_t>((x0 + x1) / 2)] == doctest::Approx(std::sqrt(2.0)).epsilon(1e-3));
  // M5 review H7: a clip cut at the next clip's start (cutEnd) stops there with the edge declick and keeps its law
  // (a reversed clip still plays, at every instant, what the whole clip would).
  Rig r3(64, 0.0);
  r3.tempo({{0, 120.0, pq::TempoCurve::Step}});
  e::AudioClipDesc rev = offClip(1, 0, kPpq * 2, kSr, 1000.0);
  rev.reverse = true;
  e::AudioClipDesc whole = rev;
  rev.cutEnd = kPpq;  // half of it
  auto ramp = [](uint32_t, int64_t i) { return static_cast<float>(i) * 1e-5f; };
  r3.audio({{1, {rev}}}, {makeSample(100000, 1, ramp)}, false);
  r3.command(e::TransportCommandKind::Play, 0);
  const std::vector<float> cut = r3.render(48000);
  Rig r4(64, 0.0);
  r4.tempo({{0, 120.0, pq::TempoCurve::Step}});
  r4.audio({{1, {whole}}}, {makeSample(100000, 1, ramp)}, false);
  r4.command(e::TransportCommandKind::Play, 0);
  const std::vector<float> full = r4.render(48000);
  for (size_t i = 0; i < 24000; ++i) REQUIRE(cut[i] == full[i]);
  for (size_t i = 24000; i < cut.size(); ++i) REQUIRE(cut[i] == 0.0f);
}

TEST_CASE("clip player: reverse reads the forward frames backwards; transpose resamples; stereo and mono") {
  Rig rig(64, 0.0);
  rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
  e::AudioClipDesc r = offClip(1, 0, kPpq, kSr, 1000.0);
  r.reverse = true;
  auto ramp = [](uint32_t c, int64_t i) { return static_cast<float>(i) * 1e-4f + (c == 1 ? 0.5f : 0.0f); };
  rig.audio({{1, {r}}}, {makeSample(100000, 2, ramp)}, false);
  rig.command(e::TransportCommandKind::Play, 0);
  std::vector<float> right;
  const std::vector<float> left = rig.render(24000, 0, 0, &right);  // one beat at 120 BPM = 24000 frames
  for (int64_t i = 0; i < 24000; ++i) {  // f(s) = 1000 + (S1 − 1 − s)
    REQUIRE(left[i] == ramp(0, 1000 + 23999 - i));
    REQUIRE(right[i] == ramp(1, 1000 + 23999 - i));
  }
  // +7 semitones: a 1 kHz sine comes out at 1.4983 kHz, within the resampler's error.
  Rig t(64, 0.0);
  t.tempo({{0, 120.0, pq::TempoCurve::Step}});
  const double ratio = std::pow(2.0, 7.0 / 12.0);
  t.audio({{1, {offClip(1, 0, kPpq * 2, kSr * ratio)}}},
          {makeSample(200000, 1, [](uint32_t, int64_t i) { return static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * i / kSr)); })},
          false);
  t.command(e::TransportCommandKind::Play, 0);
  std::vector<float> rr;
  const std::vector<float> l2 = t.render(20000, 0, 0, &rr);
  double worst = 0.0;
  for (int64_t i = 100; i < 20000; ++i) {
    const double want = std::sin(2.0 * std::numbers::pi * 1000.0 * (static_cast<double>(i) * ratio) / kSr);
    worst = std::max(worst, std::abs(l2[i] - want));
    REQUIRE(rr[i] == l2[i]);  // mono on both channels
  }
  CHECK(worst < 1e-4);
}

TEST_CASE("clip player: RePitch follows the analytic tempo integral within a sample at 10 min of a ramp") {
  // A looped RePitch clip over a sine whose period divides the loop: the output's phase tells the source position.
  const double P = 64.0;
  const int64_t loopFrames = 64 * 1000;
  const double sourceBpm = 100.0;
  const double fpt = kSr * 60.0 / (sourceBpm * static_cast<double>(kPpq));
  Rig rig(64);
  // A ramp 100 → 140 BPM over 20 minutes of ticks, then constant.
  rig.tempo({{0, 100.0, pq::TempoCurve::Linear}, {kPpq * 2400, 140.0, pq::TempoCurve::Step}});
  e::AudioClipDesc c;
  c.id = 1;
  c.warp = e::ClipWarp::RePitch;
  c.start = kPpq;
  c.length = kPpq * 4000;
  c.contentStart = 0;
  c.loop = {true, 0, static_cast<Ticks>(std::llround(static_cast<double>(loopFrames) / fpt))};
  c.framesPerTick = fpt;
  const double loopEndFrames = static_cast<double>(c.loop.end) * fpt;  // ≈ loopFrames (whole ticks)
  rig.audio({{1, {c}}}, {makeSample(loopFrames + 1000, 1, [&](uint32_t, int64_t i) {
                           return static_cast<float>(std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / P));
                         })});
  for (const double seconds : {30.0, 600.0}) {
    // Locate there and play a little: positions come from the tempo map at every block, never accumulated.
    const Ticks at = static_cast<Ticks>(rig.map.secondsToTicks(seconds));
    rig.command(e::TransportCommandKind::Play, at);
    const SampleTime s0 = rig.map.ticksToSamples(at, kSr);
    std::vector<float> out = rig.render(4096);
    // Fit a window past the start fade and away from loop wraps: expected source position from the closed form.
    auto expected = [&](SampleTime s) {
      const double ticks = rig.map.secondsToTicks(static_cast<double>(s) / kSr) - static_cast<double>(c.start);
      double p = ticks;  // content tick (contentStart 0)
      const double end = static_cast<double>(c.loop.end);
      if (p >= end) p = std::fmod(p - end, end);
      return p * fpt;
    };
    bool measured = false;
    for (int64_t w = 512; w + 256 < 4096 && !measured; w += 256) {
      const double fA = expected(s0 + w), fB = expected(s0 + w + 255);
      if (fB < fA || fA < 200.0 || fB > loopEndFrames - 200.0) continue;  // a wrap in the window
      const double rate = (fB - fA) / 255.0;
      const double omega = 2.0 * std::numbers::pi * rate / P;
      double sc = 0, ss = 0, cc = 0, cs = 0, s2 = 0;
      for (int i = 0; i < 256; ++i) {
        const double y = out[w + i], co = std::cos(omega * i), si = std::sin(omega * i);
        sc += y * co, ss += y * si, cc += co * co, s2 += si * si, cs += co * si;
      }
      const double det = cc * s2 - cs * cs;
      const double a = (sc * s2 - ss * cs) / det, b = (ss * cc - sc * cs) / det;  // y ≈ a cos + b sin = sin(φ0 + ωi)
      const double phase = std::atan2(a, b);
      double meas = phase / (2.0 * std::numbers::pi) * P;
      double err = std::fmod(meas - std::fmod(fA, P) + 10.0 * P, P);
      if (err > P / 2) err -= P;
      const double errSamples = std::abs(err) / rate;
      MESSAGE("RePitch at ", seconds, " s: source position error ", err, " frames = ", errSamples, " output samples");
      CHECK(errSamples <= 1.0);
      measured = true;
    }
    CHECK(measured);
  }
}

TEST_CASE("clip player: a RePitch loop wraps where the content passes the loop end (declicked)") {
  Rig rig(64);
  rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
  const double fpt = kSr * 60.0 / (120.0 * static_cast<double>(kPpq));  // rate 1 at 120 BPM
  e::AudioClipDesc c;
  c.id = 1;
  c.warp = e::ClipWarp::RePitch;
  c.start = 0;
  c.length = kPpq * 4;
  c.contentStart = kPpq / 4;
  c.loop = {true, kPpq / 4, kPpq * 3 / 4};  // half a beat = 12000 frames
  c.framesPerTick = fpt;
  rig.audio({{1, {c}}}, {makeSample(100000, 1, [](uint32_t, int64_t) { return 1.0f; })});
  rig.command(e::TransportCommandKind::Play, 0);
  const std::vector<float> out = rig.render(96000);
  // Wraps every 12000 frames: a dip to ~0 at each, flat 1 between.
  for (SampleTime w : {SampleTime{12000}, SampleTime{24000}, SampleTime{36000}}) {
    CHECK(out[w] < 0.05f);
    CHECK(out[w - 1] < 0.05f);
    CHECK(out[w + 200] == doctest::Approx(1.0).epsilon(1e-5));
    CHECK(out[w - 200] == doctest::Approx(1.0).epsilon(1e-5));
  }
  CHECK(maxStep(out, 100, 95900) < 0.02);
}

TEST_CASE("clip player: missing samples are silent; nothing runs before a clip's look-ahead window (sleep)") {
  Rig rig(64);
  rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
  e::AudioClipDesc missing = offClip(1, 0, kPpq * 2);
  missing.playable = false;
  e::AudioClipDesc later = offClip(2, kPpq * 16, kPpq);  // 8 s in
  rig.audio({{1, {missing}}, {2, {later}}}, {makeSample(100000, 1, [](uint32_t, int64_t) { return 1.0f; })});
  rig.command(e::TransportCommandKind::Play, 0);
  const std::vector<float> a = rig.render(static_cast<uint64_t>(kSr * 2));
  for (float v : a) REQUIRE(v == 0.0f);
  const e::ClipPlayer& cp = rig.tl->clipPlayer();
  CHECK(cp.liveVoices() == 0);
  CHECK(cp.active()[0] == 0);
  CHECK(cp.active()[1] == 0);
  const uint64_t scans = cp.stats().scans;
  CHECK(scans <= 2);  // the first block found the next start; every block after it cost one compare
  (void)rig.render(static_cast<uint64_t>(kSr * 5));  // up to 7 s: before the look-ahead of the clip at 8 s
  CHECK(cp.stats().scans == scans);
  CHECK(cp.liveVoices() == 0);
  std::vector<float> b = rig.render(static_cast<uint64_t>(kSr * 1.5), 1);  // 7 … 8.5 s: the clip starts at 8 s
  CHECK(cp.stats().voicesStarted == 1);
  CHECK(b[static_cast<size_t>(kSr) - 1] == 0.0f);
  CHECK(b[static_cast<size_t>(kSr) + 200] == 1.0f);
  CHECK(cp.active()[1] == 1);
}

TEST_CASE("clip player: edits while playing are click-free; an identical republish is bit-exact") {
  auto sine = [](uint32_t, int64_t i) { return static_cast<float>(0.8 * std::sin(2.0 * std::numbers::pi * 220.0 * i / kSr)); };
  auto run = [&](int mode) {
    Rig rig(64);
    rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
    rig.audio({{1, {offClip(1, 0, kPpq * 8)}}}, {makeSample(500000, 1, sine)});
    rig.command(e::TransportCommandKind::Play, 0);
    std::vector<float> out = rig.render(24000);
    e::AudioClipDesc c = offClip(1, 0, kPpq * 8);
    if (mode == 1) c.gain = 0.25f;                  // a gain edit
    if (mode == 2) c.start = kPpq / 3;              // the clip moved under the playhead
    if (mode == 3) c.speed = kSr * 1.5;             // transposed
    rig.audio({{1, {c}}}, {makeSample(500000, 1, sine)});
    std::vector<float> more = rig.render(24000);
    out.insert(out.end(), more.begin(), more.end());
    return out;
  };
  // The sine's own largest step is 0.8 · 2π · 220 / 48000 ≈ 0.023 (330 Hz after the 1.5× transpose: 0.035); a hard cut
  // would step by up to 1.6.
  for (int mode : {0, 1, 2, 3}) {
    const std::vector<float> o = run(mode);
    CAPTURE(mode);
    CHECK(maxStep(o, 23900, 24400) < 0.07);
  }
  // A republish of the same clips on the same sample keeps every voice: bit-identical to no swap at all.
  Rig rig(64);
  rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
  rig.audio({{1, {offClip(1, 0, kPpq * 8)}}}, {makeSample(500000, 1, sine)});
  rig.command(e::TransportCommandKind::Play, 0);
  const std::vector<float> base = rig.render(48000);
  Rig rig2(64);
  rig2.tempo({{0, 120.0, pq::TempoCurve::Step}});
  const e::ClipSample shared = makeSample(500000, 1, sine);
  rig2.audio({{1, {offClip(1, 0, kPpq * 8)}}}, {shared});
  rig2.command(e::TransportCommandKind::Play, 0);
  std::vector<float> o2 = rig2.render(24000);
  rig2.audio({{1, {offClip(1, 0, kPpq * 8)}}}, {shared});
  std::vector<float> m2 = rig2.render(24000);
  o2.insert(o2.end(), m2.begin(), m2.end());
  CHECK(o2 == base);
  CHECK(rig2.tl->clipPlayer().stats().kept == 1);
  CHECK(rig2.tl->clipPlayer().stats().faded == 0);
}

TEST_CASE("clip player: stop and locate fade out; a locate into a clip fades in") {
  Rig rig(64);
  rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
  rig.audio({{1, {offClip(1, 0, kPpq * 16)}}}, {makeSample(800000, 1, [](uint32_t, int64_t) { return 1.0f; })});
  rig.command(e::TransportCommandKind::Play, 0);
  std::vector<float> out = rig.render(10000);
  rig.command(e::TransportCommandKind::Stop);
  std::vector<float> tail = rig.render(1000);
  CHECK(tail[0] == 1.0f);
  CHECK(tail[95] < 0.02f);
  CHECK(tail[96] == 0.0f);
  CHECK(maxStep(tail, 0, tail.size()) <= 1.0 / 96.0 + 1e-6);
  CHECK(rig.tl->clipPlayer().liveVoices() == 0);
  rig.command(e::TransportCommandKind::Play, kPpq * 4);  // into the middle of the clip
  std::vector<float> in = rig.render(500);
  CHECK(in[0] == 0.0f);
  CHECK(in[96] == 1.0f);
  CHECK(maxStep(in, 0, in.size()) <= 1.0 / 96.0 + 1e-6);
  // An arrangement loop wrap: the old position fades out while the loop start fades in (sum stays ≤ 1, no step).
  e::TransportCommand loop;
  loop.kind = e::TransportCommandKind::SetLoop;
  loop.loopEnabled = true;
  loop.loopStart = kPpq;
  loop.loopEnd = kPpq * 2;
  rig.tl->apply(loop);
  rig.command(e::TransportCommandKind::Play, kPpq);
  std::vector<float> l = rig.render(60000);
  CHECK(maxStep(l, 200, l.size()) <= 1.0 / 96.0 + 1e-6);
  for (float v : l) REQUIRE(v <= 1.0f + 1e-6f);
}

TEST_CASE("clip player: a clipPlayer atom reads its slot through ProcessContext::clipIn") {
  const interp_test::Compiled c = interp_test::compileText("patch P {\n  out out : real audio global stereo\n  out = clipPlayer(3) * 0.5\n}\n");
  e::InterpretOptions io;
  io.maxBlockFrames = 64;
  io.maxVoiceGroups = 1;
  auto img = e::buildInterpreted(c.sched, io);
  REQUIRE(img.ok());
  REQUIRE((*img)->io().clipSlots == std::vector<uint32_t>{3});
  std::vector<float> l(64), r(64), cl(64), cr(64);
  for (int i = 0; i < 64; ++i) cl[i] = static_cast<float>(i), cr[i] = -static_cast<float>(i);
  std::vector<const float*> clipIn(8, nullptr);
  clipIn[6] = cl.data();
  clipIn[7] = cr.data();
  float* outs[2] = {l.data(), r.data()};
  e::ProcessContext ctx;
  ctx.frames = 64;
  ctx.out = outs;
  ctx.numOut = 2;
  ctx.clipIn = clipIn.data();
  ctx.numClipSlots = 4;
  (*img)->process(ctx);
  for (int i = 0; i < 64; ++i) {
    REQUIRE(l[i] == 0.5f * cl[i]);
    REQUIRE(r[i] == 0.5f * cr[i]);
  }
  ctx.numClipSlots = 3;  // slot 3 not provided: silence
  (*img)->process(ctx);
  for (int i = 0; i < 64; ++i) REQUIRE(l[i] == 0.0f);
}

namespace {

fs::path writeWav(const char* name, int64_t frames, uint32_t channels, const std::function<float(uint32_t, int64_t)>& fn) {
  const fs::path dir = fs::temp_directory_path() / "pq_clip_player";
  fs::create_directories(dir);
  const fs::path p = dir / name;
  pq::media::WavWriter w;
  REQUIRE(w.open(p, channels, 48000).ok());
  std::vector<std::vector<float>> buf(channels, std::vector<float>(4096));
  for (int64_t at = 0; at < frames; at += 4096) {
    const auto n = static_cast<uint32_t>(std::min<int64_t>(4096, frames - at));
    std::vector<const float*> ptrs;
    for (uint32_t c = 0; c < channels; ++c) {
      for (uint32_t i = 0; i < n; ++i) buf[c][i] = fn(c, at + i);
      ptrs.push_back(buf[c].data());
    }
    REQUIRE(w.write(ptrs.data(), n).ok());
  }
  REQUIRE(w.close().ok());
  return p;
}

void fillWait(void* user, pq::media::StreamId id, int64_t frames) {
  (void)static_cast<pq::media::SamplePool*>(user)->waitFilled(id, frames);
}

}  // namespace

TEST_CASE("clip player: streamed samples — offline blocking fills equal a resident render; prefetch on locate") {
  auto fn = [](uint32_t c, int64_t i) {
    return static_cast<float>(0.5 * std::sin(0.0021 * static_cast<double>(i) + c) + 0.2 * std::sin(0.31 * static_cast<double>(i)));
  };
  const fs::path wav = writeWav("streamed.wav", 300000, 2, fn);
  auto renderWith = [&](bool streamed, bool reverse, double speed) {
    pq::media::PoolConfig pc;
    pc.residentLimitBytes = streamed ? 0 : (1ull << 30);
    pc.headFrames = 4000;
    pc.streamSlots = 16;
    pc.ringFloats = 2 * 16384;
    pq::media::SamplePool pool(pc);
    pool.startDiskThread();
    auto s = pool.load(wav);
    REQUIRE(s.ok());
    REQUIRE((*s)->view().streamed() == streamed);
    std::vector<float> out;
    {
      Rig rig(64, 0.002, &pool.streams());
      rig.tl->clipPlayer().setStreamFill(&fillWait, &pool);
      rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
      e::ClipSample cs;
      cs.pooled = *s;
      e::AudioClipDesc c = offClip(1, kPpq / 2, kPpq * 4, speed, 7000.0);
      c.reverse = reverse;
      rig.audio({{1, {c}}}, {cs});
      pool.engage();
      rig.command(e::TransportCommandKind::Play, 0);
      std::vector<float> r;
      out = rig.render(110000, 0, 0, &r);
      out.insert(out.end(), r.begin(), r.end());
      pool.disengage();
      CHECK(rig.tl->clipPlayer().stats().missingFrames == 0);
    }
    CHECK(pool.stats().underruns == 0);
    pool.stopDiskThread();
    return out;
  };
  for (const bool reverse : {false, true})
    for (const double speed : {kSr, kSr * 1.3, kSr * 0.77}) {
      CAPTURE(reverse);
      CAPTURE(speed);
      const std::vector<float> res = renderWith(false, reverse, speed);
      const std::vector<float> str = renderWith(true, reverse, speed);
      CHECK(str == res);
      double peak = 0.0;
      for (float v : res) peak = std::max(peak, static_cast<double>(std::abs(v)));
      CHECK(peak > 0.3);
    }
  // A locate prefetch: the stream the player claims at Play is the prefetched one (data ready).
  {
    pq::media::PoolConfig pc;
    pc.residentLimitBytes = 0;
    pc.headFrames = 4000;
    pc.streamSlots = 16;
    pq::media::SamplePool pool(pc);
    pool.startDiskThread();
    auto s = pool.load(wav);
    REQUIRE(s.ok());
    Rig rig(64, 0.002, &pool.streams());
    rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
    e::ClipSample cs;
    cs.pooled = *s;
    auto img = rig.build({{1, {offClip(1, 0, kPpq * 8, kSr, 0.0)}}}, {cs});
    const Ticks at = kPpq * 2;  // 1 s into the clip: past the 4000-frame head
    const SampleTime sAt = rig.map.ticksToSamples(at, kSr);
    const auto st = e::ClipPlayer::streamStart(*img, img->clips(0)[0], sAt, 64);
    REQUIRE(st.has_value());
    REQUIRE(pool.prefetch(**s, st->frame, st->dir));
    REQUIRE(rig.tl->publishAudio(std::move(img)).ok());
    // Wait (off the audio thread, as the UI would between a locate and Play) until the prefetch is filled.
    for (int i = 0; i < 500 && pool.stats().diskReads == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    pool.engage();
    rig.command(e::TransportCommandKind::Play, at);
    const uint64_t before = pool.stats().underruns;
    (void)rig.render(4800);
    CHECK(pool.stats().underruns == before);
    CHECK(rig.tl->clipPlayer().stats().missingFrames == 0);
    pool.disengage();
    pool.clearPrefetches();
    rig.tl.reset();
    pool.stopDiskThread();
  }
  std::error_code ec;
  fs::remove_all(wav.parent_path(), ec);
}

// T5.x-perf-audio (Q151 item 1): +19 st and +40 st Off clips (rates 3.0 and 10.1) and a reverse +24 st one on a
// resident pooled sample: with mips (levels 1 … 3) the player reads the decimated copies (ClipPlayerStats::mipFrames)
// and renders what the direct read renders within the resampler's error (both are band-limited reads of the same
// signal: a low sine and a mid one, far from the band edge), allocation-free; a pool that has no mips reads directly.
TEST_CASE("clip player: above rate 2 a resident sample's mips are read (same audio, allocation-free)") {
  REQUIRE(pq::alloc_trap::installed());
  auto fn = [](uint32_t c, int64_t i) {
    return static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * 97.0 * static_cast<double>(i) / kSr + c) +
                              0.3 * std::sin(2.0 * std::numbers::pi * 613.0 * static_cast<double>(i) / kSr));
  };
  const fs::path wav = writeWav("mips.wav", 48000 * 12, 2, fn);
  auto render = [&](bool mips, double semitones, bool reverse, uint64_t* mipFrames) {
    pq::media::PoolConfig pc;
    pc.residentLimitBytes = 1ull << 30;
    pq::media::SamplePool pool(pc);
    auto s = pool.load(wav);
    REQUIRE(s.ok());
    REQUIRE_FALSE((*s)->view().streamed());
    if (mips) REQUIRE(pool.buildMips(**s, pq::dsp::kMaxMipLevels));
    Rig rig(64, 0.002, &pool.streams());
    rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
    e::ClipSample cs;
    cs.pooled = *s;
    e::AudioClipDesc c = offClip(1, 0, kPpq * 2, kSr * std::pow(2.0, semitones / 12.0), reverse ? 100000.0 : 1000.0);
    c.reverse = reverse;
    rig.audio({{1, {c}}}, {cs});
    rig.command(e::TransportCommandKind::Play, 0);
    (void)rig.render(64);  // the image is taken outside the scope
    std::vector<float> out(32768 - 64);
    CHECK(pq::alloc_trap::countRtViolations([&] {
            for (size_t at = 0; at < out.size(); at += 64) {
              rig.tl->process(rig.time, 64);
              rig.time += 64;
              std::memcpy(out.data() + at, rig.tl->clipPlayer().channels()[0], 64 * sizeof(float));
            }
          }) == 0);
    *mipFrames = rig.tl->clipPlayer().stats().mipFrames;
    return out;
  };
  for (const auto& [st, reverse] : std::vector<std::pair<double, bool>>{{19.0, false}, {40.0, false}, {24.0, true}}) {
    CAPTURE(st);
    CAPTURE(reverse);
    uint64_t withMips = 0, without = 0;
    const std::vector<float> a = render(true, st, reverse, &withMips);
    const std::vector<float> b = render(false, st, reverse, &without);
    CHECK(withMips > 30000);
    CHECK(without == 0);
    double worst = 0.0, peak = 0.0;
    for (size_t i = 200; i < a.size() - 200; ++i) {  // away from the 2 ms edge declicks
      worst = std::max(worst, static_cast<double>(std::abs(a[i] - b[i])));
      peak = std::max(peak, static_cast<double>(std::abs(b[i])));
    }
    MESSAGE("+", st, " st", std::string(reverse ? " reverse" : ""), ": mip read vs direct read, max |difference| ", worst, " (peak ", peak, ")");
    CHECK(peak > 0.3);
    CHECK(worst < 1e-4);
  }
  std::error_code ec;
  fs::remove_all(wav.parent_path(), ec);
}

// T5.x-perf-audio (Q151 item 7): a looped RePitch clip on a streamed sample whose loop lies past the resident head,
// played in real time (one 64-frame block per 1.33 ms of wall clock, the disk thread polling as in playback): every
// wrap reads data the disk thread fetched ahead (the voice's second cursor, claimed ~0.5 s before the wrap), so no
// frame is missing. Before: each wrap claimed a fresh stream at the loop start and underran.
TEST_CASE("clip player: a looped RePitch clip on a streamed sample wraps without underruns (real time)") {
  auto fn = [](uint32_t c, int64_t i) { return static_cast<float>(0.5 * std::sin(0.0137 * static_cast<double>(i) + c)); };
  const fs::path wav = writeWav("loopwrap.wav", 400000, 2, fn);
  for (const bool reverse : {false, true}) {
    CAPTURE(reverse);
    pq::media::PoolConfig pc;
    pc.residentLimitBytes = 0;
    pc.headFrames = 4000;
    pc.streamSlots = 16;
    pc.ringFloats = 2 * 65536;
    pq::media::SamplePool pool(pc);
    pool.startDiskThread();
    auto s = pool.load(wav);
    REQUIRE(s.ok());
    REQUIRE((*s)->view().streamed());
    {
      Rig rig(64, 0.002, &pool.streams());
      rig.tempo({{0, 120.0, pq::TempoCurve::Step}});
      const double fpt = 1.07 * kSr * 60.0 / (120.0 * static_cast<double>(kPpq));  // +1.2 st: the sinc path
      e::AudioClipDesc c;
      c.id = 1;
      c.warp = e::ClipWarp::RePitch;
      c.reverse = reverse;
      c.start = 0;
      // 4.1 s; not a whole number of loop passes, so a reverse clip (which starts at the content's end, folded into the
      // loop) does not begin exactly on a wrap (that would be a locate onto a wrap: the locate prefetch covers the start
      // position only).
      c.length = kPpq * 8 + kPpq / 4;
      c.contentStart = kPpq * 8;  // ≈ 4.3 s into the file: far past the 4000-frame head
      c.loop = {true, kPpq * 8, kPpq * 9};  // half a second of content: a wrap every ≈ 0.5 s
      c.framesPerTick = fpt;
      e::ClipSample cs;
      cs.pooled = *s;
      rig.audio({{1, {c}}}, {cs});
      pool.engage();
      // Data at the clip's start is ready (a locate prefetch, as the app does before Play).
      auto img = rig.build({{1, {c}}}, {cs});
      const auto st = e::ClipPlayer::streamStart(*img, img->clips(0)[0], 0, 64);
      REQUIRE(st.has_value());
      REQUIRE(pool.prefetch(**s, st->frame, st->dir));
      for (int i = 0; i < 500 && pool.stats().diskReads == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      rig.command(e::TransportCommandKind::Play, 0);
      const uint64_t before = pool.stats().underruns;
      const auto t0 = std::chrono::steady_clock::now();
      const int blocks = static_cast<int>(2.2 * kSr / 64);  // ≈ 4 wraps
      double peak = 0.0;
      for (int b = 0; b < blocks; ++b) {
        rig.tl->process(rig.time, 64);
        rig.time += 64;
        const float* l = rig.tl->clipPlayer().channels()[0];
        for (int i = 0; i < 64; ++i) peak = std::max(peak, static_cast<double>(std::abs(l[i])));
        const auto due = t0 + std::chrono::nanoseconds(static_cast<int64_t>((b + 1) * 64 * 1e9 / kSr));
        while (std::chrono::steady_clock::now() < due) std::this_thread::yield();
      }
      CHECK(peak > 0.3);
      CHECK(rig.tl->clipPlayer().stats().missingFrames == 0);
      CHECK(pool.stats().underruns == before);
      CHECK(rig.tl->clipPlayer().stats().loopAdopts >= 3);
      pool.disengage();
      pool.clearPrefetches();
      rig.tl.reset();
    }
    pool.stopDiskThread();
  }
  std::error_code ec;
  fs::remove_all(wav.parent_path(), ec);
}

TEST_CASE("rt: the clip player across boundaries, crossfades, loop wraps, swaps and underruns allocates nothing") {
  REQUIRE(pq::alloc_trap::installed());
  auto fn = [](uint32_t c, int64_t i) { return static_cast<float>(std::sin(0.01 * static_cast<double>(i) + c)); };
  const fs::path wav = writeWav("rt.wav", 200000, 2, fn);
  pq::media::PoolConfig pc;
  pc.residentLimitBytes = 0;
  pc.headFrames = 2000;
  pc.streamSlots = 16;
  pc.ringFloats = 2 * 8192;
  pq::media::SamplePool pool(pc);
  pool.startDiskThread();
  auto streamed = pool.load(wav);
  REQUIRE(streamed.ok());
  {
    Rig rig(64, 0.002, &pool.streams());
    rig.tempo({{0, 120.0, pq::TempoCurve::Linear}, {kPpq * 8, 90.0, pq::TempoCurve::Step}});
    e::ClipSample cs;
    cs.pooled = *streamed;
    e::AudioClipDesc a = offClip(1, 0, kPpq * 2, kSr * 1.2, 100.0, 1);
    e::AudioClipDesc b = offClip(2, kPpq * 2, kPpq * 2, kSr, 3000.0, 0);  // streamed: underruns without fills
    b.crossfadeIn = kPpq / 2;
    b.reverse = true;
    e::AudioClipDesc rp;
    rp.id = 3;
    rp.sample = 1;
    rp.warp = e::ClipWarp::RePitch;
    rp.start = kPpq * 4;
    rp.length = kPpq * 4;
    rp.loop = {true, 0, kPpq / 8};
    rp.framesPerTick = kSr * 60.0 / (100.0 * static_cast<double>(kPpq));
    const e::ClipSample res = makeSample(100000, 1, fn);
    auto img1 = rig.build({{1, {a, b, rp}}}, {cs, res});
    e::AudioClipDesc a2 = a;
    a2.gain = 0.3f;
    auto img2 = rig.build({{1, {a2, b, rp}}}, {cs, res});
    rig.command(e::TransportCommandKind::Play, 0);
    REQUIRE(rig.tl->publishAudio(std::move(img1)).ok());
    pool.engage();
    (void)rig.render(64);  // first block: images taken outside the scope
    uint64_t violations = 0;
    for (int round = 0; round < 3; ++round) {
      violations += pq::alloc_trap::countRtViolations([&] {
        for (int b2 = 0; b2 < 1600; ++b2) {
          rig.tl->process(rig.time, 64);
          rig.time += 64;
        }
      });
      if (round == 0) REQUIRE(rig.tl->publishAudio(std::move(img2)).ok());  // a swap inside the next scope
      if (round == 1) {
        e::TransportCommand loop;
        loop.kind = e::TransportCommandKind::SetLoop;
        loop.loopEnabled = true;
        loop.loopStart = kPpq;
        loop.loopEnd = kPpq * 3;
        REQUIRE(rig.tl->post(loop));
      }
    }
    CHECK(violations == 0);
    rig.command(e::TransportCommandKind::Stop);
    CHECK(pq::alloc_trap::countRtViolations([&] {
            for (int b2 = 0; b2 < 10; ++b2) {
              rig.tl->process(rig.time, 64);
              rig.time += 64;
            }
          }) == 0);
    CHECK(rig.tl->clipPlayer().stats().voicesStarted >= 3);
    pool.disengage();
  }
  pool.stopDiskThread();
  std::error_code ec;
  fs::remove_all(wav.parent_path(), ec);
}
