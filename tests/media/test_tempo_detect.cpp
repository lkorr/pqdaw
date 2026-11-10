// T6.8: tempo detection (pq/media/tempo_detect.h) on 20 labelled drum loops synthesised here (four-on-the-floor,
// back-beats with 8th / 16th hats, broken beats over a bass line; 62 … 178 BPM, 44.1 / 48 kHz, ±3 ms of timing jitter
// and velocity variation): within ±1 BPM on at least 18 of 20 (half / double tempo counts as a miss), never confidently
// wrong, confident (≥ 0.6, the auto-warp threshold) where no other octave reads the same onsets (T6.x-fix-warp);
// T6.x-fix-warp (M6 review H3): one-bar broken-beat / two-step loops (the length prior), half-time grooves (ambiguous,
// never confidently wrong), 16th hats alone (right from 104 BPM; doubled below, documented); scattered onsets read
// unconfident; the decoder path equals the in-memory one; the
// result is cached on disk (a second run ≤ 1 ms, no decode) and in memory, and the transients found on the way land in
// the transient cache.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

#include "pq/media/decoder.h"
#include "pq/media/tempo_detect.h"
#include "pq/media/wav_file.h"

namespace {

namespace fs = std::filesystem;

struct Loop {
  double bpm = 120.0;
  uint32_t rate = 48000;
  std::vector<std::vector<float>> ch;
};

struct Rng {
  uint32_t s;
  double next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<double>(s >> 8) / 16777216.0;
  }
  double signedNext() { return 2.0 * next() - 1.0; }
};

enum class Hit : uint8_t { Kick, Snare, Hat };

void addHit(Loop& l, Rng& r, Hit h, double atSeconds, double gain) {
  const double sr = l.rate;
  const auto s0 = static_cast<int64_t>(std::llround(atSeconds * sr));
  const auto n = static_cast<int64_t>(l.ch[0].size());
  const double len = h == Hit::Kick ? 0.35 : h == Hit::Snare ? 0.25 : 0.06;
  double prevNoise = 0.0;
  for (int64_t i = std::max<int64_t>(0, s0); i < std::min(n, s0 + static_cast<int64_t>(len * sr)); ++i) {
    const double t = static_cast<double>(i - s0) / sr;
    double v = 0.0;
    if (h == Hit::Kick) {
      v = 0.9 * std::exp(-t * 14.0) * std::sin(2.0 * std::numbers::pi * (50.0 * t + 3.0 * (1.0 - std::exp(-t * 30.0))));
    } else if (h == Hit::Snare) {
      v = 0.55 * r.signedNext() * std::exp(-t / 0.06) + 0.3 * std::sin(2.0 * std::numbers::pi * 185.0 * t) * std::exp(-t / 0.05);
    } else {
      const double noise = r.signedNext();
      v = 0.22 * (noise - prevNoise) * std::exp(-t / 0.015);  // a crude high-pass: hats are bright
      prevNoise = noise;
    }
    l.ch[0][static_cast<size_t>(i)] += static_cast<float>(gain * v);
    l.ch[1][static_cast<size_t>(i)] += static_cast<float>(gain * v * 0.9);
  }
}

// `bars` bars of 4/4 of a pattern at `bpm` (the file exactly that long, as a loop is cut).
Loop makePattern(int pattern, double bpm, uint32_t rate, int bars, uint32_t seed) {
  Loop l;
  l.bpm = bpm;
  l.rate = rate;
  const double beat = 60.0 / bpm;
  const auto frames = static_cast<size_t>(std::ceil(bars * 4.0 * beat * l.rate));
  l.ch.assign(2, std::vector<float>(frames, 0.0f));
  Rng r{seed};
  auto hit = [&](Hit h, double beatPos, double gain) {
    const double jitter = 0.003 * r.signedNext();
    addHit(l, r, h, beatPos * beat + jitter + 0.01, gain * (0.85 + 0.15 * r.next()));
  };
  if (pattern == 3) {  // a bass line under the broken beat: sustained notes, soft attacks (no onsets of their own)
    for (size_t i = 0; i < frames; ++i) {
      const double t = static_cast<double>(i) / l.rate;
      const double fade = std::min(1.0, t / 0.3);
      const float v = static_cast<float>(0.12 * fade * std::sin(2.0 * std::numbers::pi * 55.0 * t));
      l.ch[0][i] += v;
      l.ch[1][i] += v;
    }
  }
  for (int bar = 0; bar < bars; ++bar)
    for (int b = 0; b < 4; ++b) {
      const double at = bar * 4.0 + b;
      switch (pattern) {
        case 0:  // four on the floor, clap on 2 and 4, off-beat hats
          hit(Hit::Kick, at, 1.0);
          if (b % 2 == 1) hit(Hit::Snare, at, 0.8);
          hit(Hit::Hat, at + 0.5, 0.7);
          break;
        case 1:  // back-beat, 8th hats
          if (b % 2 == 0) hit(Hit::Kick, at, 1.0);
          else hit(Hit::Snare, at, 0.9);
          hit(Hit::Hat, at, 0.5);
          hit(Hit::Hat, at + 0.5, 0.4);
          break;
        case 2:  // back-beat, quiet 16th hats
          if (b % 2 == 0) hit(Hit::Kick, at, 1.0);
          else hit(Hit::Snare, at, 0.9);
          for (int s = 0; s < 4; ++s) hit(Hit::Hat, at + 0.25 * s, s == 0 ? 0.4 : 0.25);
          break;
        case 3:  // broken beat: kick on 1 and the and of 2, snare on 2 and 4, 8th hats
          if (b == 0) hit(Hit::Kick, at, 1.0);
          if (b == 1) hit(Hit::Kick, at + 0.5, 0.8);
          if (b % 2 == 1) hit(Hit::Snare, at, 0.9);
          hit(Hit::Hat, at, 0.35);
          hit(Hit::Hat, at + 0.5, 0.3);
          break;
        case 4:  // T6.x-fix-warp (M6 review H3) — half-time: kick on 1, snare on 3, 8th hats
          if (b == 0) hit(Hit::Kick, at, 1.0);
          if (b == 2) hit(Hit::Snare, at, 0.9);
          hit(Hit::Hat, at, 0.4);
          hit(Hit::Hat, at + 0.5, 0.3);
          break;
        case 5:  // two-step: kick on 1 and the and of 3, snare on 2 and 4, 8th hats
          if (b == 0) hit(Hit::Kick, at, 1.0);
          if (b == 2) hit(Hit::Kick, at + 0.5, 0.9);
          if (b % 2 == 1) hit(Hit::Snare, at, 0.95);
          hit(Hit::Hat, at, 0.3);
          hit(Hit::Hat, at + 0.5, 0.25);
          break;
        default:  // 16th hats alone, accented on the beat
          for (int s = 0; s < 4; ++s) hit(Hit::Hat, at + 0.25 * s, s == 0 ? 0.8 : 0.4);
          break;
      }
    }
  return l;
}

// Loop k of the labelled set: its pattern, tempo, rate and length (at least 8.5 s, whole bars).
Loop makeLoop(int k, double bpm) {
  const int bars = static_cast<int>(std::ceil(8.5 / (4.0 * 60.0 / bpm)));
  return makePattern(k % 4, bpm, (k % 2) == 0 ? 48000 : 44100, bars, static_cast<uint32_t>(1000 + 77 * k));
}

constexpr double kLabels[20] = {62.0,  68.0,  72.0,  75.0,  80.0,  85.0,  88.0,  92.0,  96.0,  100.0,
                                105.0, 110.0, 118.0, 124.0, 128.0, 135.0, 140.0, 150.0, 165.0, 178.0};

pq::media::TempoEstimate detect(const Loop& l) {
  const float* ch[2] = {l.ch[0].data(), l.ch[1].data()};
  return pq::media::detectTempo(ch, 2, static_cast<int64_t>(l.ch[0].size()), l.rate);
}

}  // namespace

// T6.x-fix-warp (M6 review H3): the confidence part of this test changed. It asked for ≥ 18 of 20 confident, but the
// loops below 100 BPM are back-beats / four-on-the-floor with 8th or 16th hats, onset for onset the same as a
// half-time groove at twice their tempo (in range up to 200): the detector cannot know the octave, and a confident
// wrong octave makes auto-warp play the clip at double / half speed (the review's H3: half-time grooves read as half,
// confidence 1). Those now read ambiguous (< 0.6, the user's ×2 / ÷2); the rest must stay confident, and none may be
// confidently wrong.
TEST_CASE("tempo: ±1 BPM on at least 18 of 20 labelled loops (half / double = miss); confident unless the octave is "
          "ambiguous, never confidently wrong") {
  int hits = 0, confident = 0, confidentWrong = 0, unambiguous = 0, unambiguousConfident = 0;
  for (int k = 0; k < 20; ++k) {
    const Loop l = makeLoop(k, kLabels[k]);
    const pq::media::TempoEstimate e = detect(l);
    const bool hit = std::abs(e.bpm - l.bpm) <= 1.0;
    const bool sure = e.confidence >= 0.6f;
    hits += hit ? 1 : 0;
    confident += sure ? 1 : 0;
    confidentWrong += sure && !hit ? 1 : 0;
    if (l.bpm >= 100.0) {  // no other octave in 60 … 200 that reads the same onsets
      ++unambiguous;
      unambiguousConfident += sure ? 1 : 0;
    }
    MESSAGE("loop ", k, " (pattern ", k % 4, ", ", l.rate, " Hz): label ", l.bpm, " BPM, detected ", e.bpm,
            " confidence ", e.confidence, std::string(hit ? "" : "  MISS"));
  }
  MESSAGE(hits, "/20 right, ", confident, "/20 confident (", unambiguousConfident, "/", unambiguous, " of those >= 100 BPM)");
  CHECK(hits >= 18);
  CHECK(confidentWrong == 0);
  CHECK(unambiguousConfident == unambiguous);
}

// T6.x-fix-warp (M6 review H3): the families the review's probe found wrong, 62 … 188 BPM in 14 BPM steps at 48 and
// 44.1 kHz. Measured before the fix (same generator) / after:
//   one-bar broken beat      8/40 right, 32 confidently wrong (mostly 2/3 of the tempo)  →  39/40, 0
//   one-bar two-step        19/40, 21 confidently wrong                                  →  40/40, 0
//   half-time, 4 bars       18/40 (the rest half), 22 confidently wrong                  →  18/40, 0 (the rest flagged)
//   16th hats alone, 4 bars 28/40 (the rest double), 12 confidently wrong               →  28/40, 12 (unchanged: see
//                           tempo_detect.h; their octave cannot be told from a back-beat's by the comb)
TEST_CASE("tempo (M6 H3): one-bar broken-beat / two-step loops, half-time grooves, 16th hats — rates per family") {
  struct Family {
    const char* name;
    int pattern, bars;
  };
  for (const Family f : {Family{"one-bar broken beat", 3, 1}, Family{"one-bar two-step", 5, 1}, Family{"half-time", 4, 4},
                         Family{"16th hats", 6, 4}}) {
    int n = 0, right = 0, confidentWrong = 0, rightAbove = 0, nAbove = 0;
    for (double bpm = 62.0; bpm <= 190.0; bpm += 14.0)
      for (const uint32_t rate : {48000u, 44100u}) {
        const Loop l = makePattern(f.pattern, bpm, rate, f.bars, 7000u + static_cast<uint32_t>(bpm) * 3u + rate);
        const pq::media::TempoEstimate e = detect(l);
        const bool hit = std::abs(e.bpm - bpm) <= 1.0;
        ++n;
        right += hit ? 1 : 0;
        confidentWrong += !hit && e.confidence >= 0.6f ? 1 : 0;
        if (bpm >= 104.0) {
          ++nAbove;
          rightAbove += hit ? 1 : 0;
        }
        if (!hit) MESSAGE(f.name, " at ", bpm, " BPM (", rate, " Hz): ", e.bpm, " confidence ", e.confidence);
      }
    MESSAGE(f.name, ": ", right, "/", n, " right, ", confidentWrong, " confidently wrong");
    if (f.pattern == 3 || f.pattern == 5) {  // the length prior: whole bars at the right tempo, 2/3 of a bar at 2/3 of it
      CHECK(right >= n - 2);
      CHECK(confidentWrong == 0);
    } else if (f.pattern == 4) {  // half-time: the octave is ambiguous, never confidently wrong
      CHECK(confidentWrong == 0);
    } else {  // 16th hats alone: right from 104 BPM on; below, still doubled (known, documented)
      CHECK(rightAbove == nAbove);
    }
  }
}

TEST_CASE("tempo: scattered onsets are not confident; too few onsets give no tempo") {
  Rng r{4242u};
  std::vector<double> onsets;
  double t = 0.1;
  while (t < 20.0) {
    onsets.push_back(t * 48000.0);
    t += 0.08 + 0.6 * r.next();
  }
  const pq::media::TempoEstimate e = pq::media::estimateTempo(onsets, {}, 48000.0, 20 * 48000);
  MESSAGE("scattered: ", e.bpm, " BPM, confidence ", e.confidence);
  CHECK(e.confidence < 0.6f);
  const double three[3] = {0.0, 24000.0, 48000.0};
  CHECK(pq::media::estimateTempo(three, {}, 48000.0, 96000).bpm == 0.0);
  // A plain metronome at 132 BPM, unweighted: 132 (the prior keeps it from 66).
  std::vector<double> click;
  for (int i = 0; i < 40; ++i) click.push_back(i * 48000.0 * 60.0 / 132.0);
  const pq::media::TempoEstimate m = pq::media::estimateTempo(click, {}, 48000.0, static_cast<int64_t>(click.back()) + 4800);
  CHECK(m.bpm == doctest::Approx(132.0).epsilon(0.002));
  CHECK(m.confidence >= 0.6f);
}

TEST_CASE("tempo: the decoder path equals the in-memory one; cached on disk (second run ≤ 1 ms) and in memory") {
  const Loop l = makeLoop(5, kLabels[5]);
  const fs::path dir = fs::temp_directory_path() / "pq_tempo_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const fs::path wav = dir / "loop.wav";
  REQUIRE(pq::media::writeWav(wav, l.ch, l.rate).ok());
  const pq::media::TempoEstimate mem = detect(l);

  pq::media::tempoCache().clear();
  pq::media::transientCache().clear();
  pq::media::TransientRequest r;
  r.path = wav;
  r.contentHash = 0x5150ABCDull;
  r.frames = static_cast<int64_t>(l.ch[0].size());
  r.cacheDir = dir / "cache" / "analysis";
  auto first = pq::media::loadOrDetectTempo(r);
  REQUIRE(first.ok());
  CHECK((*first)->estimate.bpm == mem.bpm);  // float32 WAV: the same frames
  CHECK((*first)->estimate.confidence == mem.confidence);
  CHECK(fs::exists(pq::media::tempoCacheFile(r.cacheDir, r.contentHash, r.frames)));
  // The transients found on the way are cached too (memory and disk).
  REQUIRE(pq::media::transientCache().find(r.contentHash, r.frames) != nullptr);
  CHECK(fs::exists(pq::media::transientCacheFile(r.cacheDir, r.contentHash, r.frames)));
  double best = 1e9;
  for (int i = 0; i < 3; ++i) {
    pq::media::tempoCache().clear();  // from the disk file
    const auto t0 = std::chrono::steady_clock::now();
    auto again = pq::media::loadOrDetectTempo(r);
    best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    REQUIRE(again.ok());
    CHECK((*again)->estimate.bpm == mem.bpm);
  }
  MESSAGE("cached tempo: ", best, " ms from disk");
  CHECK(best <= 1.0);
  auto mem2 = pq::media::loadOrDetectTempo(r);
  REQUIRE(mem2.ok());
  CHECK(mem2->get() == pq::media::tempoCache().find(r.contentHash, r.frames).get());
  // With the transients cached, the analysis reuses them (the same estimate).
  pq::media::tempoCache().clear();
  fs::remove(pq::media::tempoCacheFile(r.cacheDir, r.contentHash, r.frames), ec);
  auto reuse = pq::media::loadOrDetectTempo(r);
  REQUIRE(reuse.ok());
  CHECK((*reuse)->estimate.bpm == mem.bpm);
  pq::media::tempoCache().clear();
  pq::media::transientCache().clear();
  fs::remove_all(dir, ec);
}
