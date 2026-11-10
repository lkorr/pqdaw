// M6 review (warp) Q4: tempo detection beyond the test's 20 fixed (pattern, tempo) pairs. The test's generator (same
// hits) over every pattern × 60…200 BPM, both rates, plus patterns it never sees: half-time (kick 1, snare 3),
// drum & bass two-step at 170–176, kick-only four-on-the-floor, 16th hats only, a shuffle (triplet hats), piano-like
// chords on every beat (no drums); and short loops (1 / 2 bars, the common sample-pack length). Half / double and
// confidence reported.
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

#include "pq/media/tempo_detect.h"

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
enum class Hit : uint8_t { Kick, Snare, Hat, Chord };

static void addHit(Loop& l, Rng& r, Hit h, double atSeconds, double gain, double pitch = 220.0) {
  const double sr = l.rate;
  const auto s0 = static_cast<int64_t>(std::llround(atSeconds * sr));
  const auto n = static_cast<int64_t>(l.ch[0].size());
  const double len = h == Hit::Kick ? 0.35 : h == Hit::Snare ? 0.25 : h == Hit::Chord ? 0.45 : 0.06;
  double prevNoise = 0.0;
  for (int64_t i = std::max<int64_t>(0, s0); i < std::min(n, s0 + static_cast<int64_t>(len * sr)); ++i) {
    const double t = static_cast<double>(i - s0) / sr;
    double v = 0.0;
    if (h == Hit::Kick) {
      v = 0.9 * std::exp(-t * 14.0) * std::sin(2.0 * std::numbers::pi * (50.0 * t + 3.0 * (1.0 - std::exp(-t * 30.0))));
    } else if (h == Hit::Snare) {
      v = 0.55 * r.signedNext() * std::exp(-t / 0.06) + 0.3 * std::sin(2.0 * std::numbers::pi * 185.0 * t) * std::exp(-t / 0.05);
    } else if (h == Hit::Chord) {
      const double a = std::min(1.0, t / 0.004) * std::exp(-t * 4.0);
      v = 0.25 * a * (std::sin(2 * std::numbers::pi * pitch * t) + std::sin(2 * std::numbers::pi * pitch * 1.25 * t) +
                      std::sin(2 * std::numbers::pi * pitch * 1.5 * t));
    } else {
      const double noise = r.signedNext();
      v = 0.22 * (noise - prevNoise) * std::exp(-t / 0.015);
      prevNoise = noise;
    }
    l.ch[0][static_cast<size_t>(i)] += static_cast<float>(gain * v);
    l.ch[1][static_cast<size_t>(i)] += static_cast<float>(gain * v * 0.9);
  }
}

static Loop makeLoop(int pattern, double bpm, uint32_t rate, int bars, uint32_t seed) {
  Loop l;
  l.bpm = bpm;
  l.rate = rate;
  const double beat = 60.0 / bpm;
  const auto frames = static_cast<size_t>(std::ceil(bars * 4.0 * beat * l.rate));
  l.ch.assign(2, std::vector<float>(frames, 0.0f));
  Rng r{seed};
  auto hit = [&](Hit h, double beatPos, double gain, double pitch = 220.0) {
    const double jitter = 0.003 * r.signedNext();
    addHit(l, r, h, beatPos * beat + jitter + 0.01, gain * (0.85 + 0.15 * r.next()), pitch);
  };
  if (pattern == 3)
    for (size_t i = 0; i < frames; ++i) {
      const double t = static_cast<double>(i) / l.rate;
      const float v = static_cast<float>(0.12 * std::min(1.0, t / 0.3) * std::sin(2.0 * std::numbers::pi * 55.0 * t));
      l.ch[0][i] += v;
      l.ch[1][i] += v;
    }
  for (int bar = 0; bar < bars; ++bar)
    for (int b = 0; b < 4; ++b) {
      const double at = bar * 4.0 + b;
      switch (pattern) {
        case 0:
          hit(Hit::Kick, at, 1.0);
          if (b % 2 == 1) hit(Hit::Snare, at, 0.8);
          hit(Hit::Hat, at + 0.5, 0.7);
          break;
        case 1:
          if (b % 2 == 0) hit(Hit::Kick, at, 1.0);
          else hit(Hit::Snare, at, 0.9);
          hit(Hit::Hat, at, 0.5);
          hit(Hit::Hat, at + 0.5, 0.4);
          break;
        case 2:
          if (b % 2 == 0) hit(Hit::Kick, at, 1.0);
          else hit(Hit::Snare, at, 0.9);
          for (int s = 0; s < 4; ++s) hit(Hit::Hat, at + 0.25 * s, s == 0 ? 0.4 : 0.25);
          break;
        case 3:
          if (b == 0) hit(Hit::Kick, at, 1.0);
          if (b == 1) hit(Hit::Kick, at + 0.5, 0.8);
          if (b % 2 == 1) hit(Hit::Snare, at, 0.9);
          hit(Hit::Hat, at, 0.35);
          hit(Hit::Hat, at + 0.5, 0.3);
          break;
        case 4:  // half-time: kick on 1, snare on 3, 8th hats
          if (b == 0) hit(Hit::Kick, at, 1.0);
          if (b == 2) hit(Hit::Snare, at, 0.9);
          hit(Hit::Hat, at, 0.4);
          hit(Hit::Hat, at + 0.5, 0.3);
          break;
        case 5:  // two-step (D&B): kick 1 and the and of 3, snare 2 and 4, 8th hats
          if (b == 0) hit(Hit::Kick, at, 1.0);
          if (b == 2) hit(Hit::Kick, at + 0.5, 0.9);
          if (b % 2 == 1) hit(Hit::Snare, at, 0.95);
          hit(Hit::Hat, at, 0.3);
          hit(Hit::Hat, at + 0.5, 0.25);
          break;
        case 6:  // kick only
          hit(Hit::Kick, at, 1.0);
          break;
        case 7:  // 16th hats only, accent on the beat
          for (int s = 0; s < 4; ++s) hit(Hit::Hat, at + 0.25 * s, s == 0 ? 0.8 : 0.4);
          break;
        case 8:  // shuffle: kick 1/3, snare 2/4, triplet hats (1st and 3rd of each triplet)
          if (b % 2 == 0) hit(Hit::Kick, at, 1.0);
          else hit(Hit::Snare, at, 0.9);
          hit(Hit::Hat, at, 0.4);
          hit(Hit::Hat, at + 2.0 / 3.0, 0.3);
          break;
        default:  // chords on every beat, accent on 1
          hit(Hit::Chord, at, b == 0 ? 1.0 : 0.6, b % 2 == 0 ? 220.0 : 196.0);
          break;
      }
    }
  return l;
}

static const char* kNames[] = {"4-floor", "backbeat8", "backbeat16", "broken", "halftime", "2step", "kick", "hats16", "shuffle", "chords"};

int main() {
  // 1. every pattern × tempo grid × rate, 4 bars (and ≥ 8.5 s like the test: bars chosen so) and 1 / 2 bars.
  for (int bars : {0, 2, 1}) {
    std::printf("---- %s ----\n", bars == 0 ? "≥ 8.5 s (as the test)" : bars == 2 ? "2 bars" : "1 bar");
    for (int p = 0; p < 10; ++p) {
      int ok = 0, half = 0, dbl = 0, other = 0, n = 0, confOk = 0, confWrong = 0;
      double worstConfWrong = 0.0;
      std::printf("%-10s:", kNames[p]);
      for (double bpm = 62.0; bpm <= 196.0; bpm += 7.0) {
        for (uint32_t rate : {48000u, 44100u}) {
          const int b = bars == 0 ? static_cast<int>(std::ceil(8.5 / (4.0 * 60.0 / bpm))) : bars;
          const Loop l = makeLoop(p, bpm, rate, b, 1000u + static_cast<uint32_t>(bpm * 7) + rate);
          const float* ch[2] = {l.ch[0].data(), l.ch[1].data()};
          const pq::media::TempoEstimate e = pq::media::detectTempo(ch, 2, static_cast<int64_t>(l.ch[0].size()), rate);
          ++n;
          const bool hit = std::abs(e.bpm - bpm) <= 1.0;
          if (hit) ++ok;
          else if (std::abs(e.bpm - bpm / 2) <= 1.0) ++half;
          else if (std::abs(e.bpm - bpm * 2) <= 1.0) ++dbl;
          else ++other;
          if (e.confidence >= 0.6f) (hit ? confOk : confWrong)++;
          if (!hit && e.confidence >= 0.6f) worstConfWrong = std::max(worstConfWrong, static_cast<double>(e.confidence));
          if (!hit && rate == 48000 && (p >= 4 || bars != 0)) std::printf(" %.0f->%.1f(%.2f)", bpm, e.bpm, e.confidence);
        }
      }
      std::printf("\n    %d/%d right, half %d, double %d, other %d; confident(>=0.6) & right %d, confident & WRONG %d (max conf %.2f)\n", ok,
                  n, half, dbl, other, confOk, confWrong, worstConfWrong);
    }
  }
  return 0;
}
