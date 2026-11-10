// T6.3: transient detection (pq/media/transients.h) on a labelled fixture synthesised here (plucked notes, drum hits
// and soft-attack notes at irregular times over a vibrato pad): F-measure ≥ 0.9 with a ±10 ms tolerance; sharp
// attacks placed within 2 ms (median) / 5 ms (90 %); the decoder path equals the in-memory path; the cache: a second request is served from the disk
// file (memory cache cleared) in ≤ 1 ms, and from memory after that.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <vector>

#include "pq/media/decoder.h"
#include "pq/media/transients.h"
#include "pq/media/wav_file.h"

namespace {

namespace fs = std::filesystem;
constexpr double kSr = 48000.0;

struct Fixture {
  std::vector<std::vector<float>> ch;
  std::vector<double> onsets;  // labels (frames)
  std::vector<bool> sharp;     // a hard attack (the 1 ms check)
};

Fixture makeFixture() {
  Fixture f;
  const auto frames = static_cast<size_t>(kSr * 24.0);
  f.ch.assign(2, std::vector<float>(frames, 0.0f));
  uint32_t rng = 12345u;
  auto next = [&]() {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<double>(rng >> 8) / 16777216.0;
  };
  // The pad: three sines with a 5 Hz, ±25 ¢ vibrato, faded in over 0.25 s (no onsets).
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / kSr;
    const double fade = std::min(1.0, t / 0.25);
    const double vib = std::exp2(25.0 / 1200.0 * std::sin(2.0 * std::numbers::pi * 5.0 * t));
    double v = 0.0;
    for (const double hz : {196.0, 247.0, 294.0}) v += 0.05 * std::sin(2.0 * std::numbers::pi * hz * vib * t);
    f.ch[0][i] = f.ch[1][i] = static_cast<float>(v * fade);
  }
  double at = 0.3;
  int k = 0;
  while (at < 23.0) {
    const auto s0 = static_cast<size_t>(at * kSr);
    const int kind = k % 5;
    const double amp = 0.25 + 0.5 * next();
    const double hz = 110.0 * std::exp2(std::floor(next() * 24.0) / 12.0);
    for (size_t i = s0; i < std::min(frames, s0 + static_cast<size_t>(kSr * 1.2)); ++i) {
      const double t = static_cast<double>(i - s0) / kSr;
      double v = 0.0;
      if (kind == 0 || kind == 3) {  // a pluck: harmonics, instant attack
        for (int h = 1; h <= 5; ++h) v += std::sin(2.0 * std::numbers::pi * hz * h * t) / h;
        v *= amp * 0.5 * std::exp(-t / 0.25);
      } else if (kind == 1) {  // a drum hit: noise + a falling tone
        rng = rng * 1664525u + 1013904223u;
        const double n = static_cast<double>(rng >> 8) / 8388608.0 - 1.0;
        v = amp * (0.6 * n * std::exp(-t / 0.04) + 0.5 * std::sin(2.0 * std::numbers::pi * 80.0 * t) * std::exp(-t / 0.1));
      } else if (kind == 2) {  // a soft note: harmonics, 8 ms attack
        const double env = std::min(1.0, t / 0.008) * std::exp(-t / 0.3);
        for (int h = 1; h <= 4; ++h) v += std::sin(2.0 * std::numbers::pi * hz * h * t) / h;
        v *= amp * 0.5 * env;
      } else {  // a short hat
        rng = rng * 1664525u + 1013904223u;
        v = amp * 0.4 * (static_cast<double>(rng >> 8) / 8388608.0 - 1.0) * std::exp(-t / 0.01);
      }
      f.ch[0][i] += static_cast<float>(v);
      f.ch[1][i] += static_cast<float>(v * 0.8);
    }
    f.onsets.push_back(static_cast<double>(s0));
    f.sharp.push_back(kind != 2);
    at += 0.15 + 0.5 * next();
    ++k;
  }
  return f;
}

struct Score {
  int tp = 0, fp = 0, fn = 0;
  std::vector<double> sharpErr;  // frames, matched sharp attacks
};

Score score(const std::vector<double>& got, const Fixture& f, double tol) {
  Score s;
  std::vector<bool> used(got.size(), false);
  for (size_t i = 0; i < f.onsets.size(); ++i) {
    int best = -1;
    double bd = tol + 1.0;
    for (size_t j = 0; j < got.size(); ++j) {
      const double d = std::abs(got[j] - f.onsets[i]);
      if (!used[j] && d <= tol && d < bd) {
        bd = d;
        best = static_cast<int>(j);
      }
    }
    if (best < 0) {
      ++s.fn;
      continue;
    }
    used[static_cast<size_t>(best)] = true;
    ++s.tp;
    if (f.sharp[i]) s.sharpErr.push_back(bd);
  }
  for (bool u : used) s.fp += u ? 0 : 1;
  return s;
}

}  // namespace

TEST_CASE("transients: F ≥ 0.9 at ±10 ms on a labelled fixture; sharp attacks placed within a few ms") {
  const Fixture f = makeFixture();
  const float* ch[2] = {f.ch[0].data(), f.ch[1].data()};
  const std::vector<double> got = pq::media::detectTransients(ch, 2, static_cast<int64_t>(f.ch[0].size()), 48000);
  const Score s = score(got, f, 0.010 * kSr);
  const double p = s.tp / static_cast<double>(s.tp + s.fp), r = s.tp / static_cast<double>(s.tp + s.fn);
  const double F = 2.0 * p * r / (p + r);
  std::vector<double> e = s.sharpErr;
  std::sort(e.begin(), e.end());
  REQUIRE(!e.empty());
  const double median = e[e.size() / 2], p90 = e[e.size() * 9 / 10];
  MESSAGE("labels ", f.onsets.size(), ", detected ", got.size(), ": tp ", s.tp, " fp ", s.fp, " fn ", s.fn, ", F = ", F,
          "; sharp attacks: median ", median / 48.0, " ms, 90 % ", p90 / 48.0, " ms, worst ", e.back() / 48.0, " ms");
  CHECK(f.onsets.size() > 35);
  CHECK(F >= 0.9);
  CHECK(median <= 2.0 * 48.0);
  CHECK(p90 <= 5.0 * 48.0);
  for (size_t i = 1; i < got.size(); ++i) CHECK(got[i] > got[i - 1]);
  // Silence and a steady tone have none.
  std::vector<float> quiet(48000, 0.0f), tone(96000);
  for (size_t i = 0; i < tone.size(); ++i)  // faded out (an abrupt stop is a click: an onset)
    tone[i] = static_cast<float>(0.3 * std::min(1.0, static_cast<double>(tone.size() - i) / 4800.0) *
                                 std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kSr));
  const float* q = quiet.data();
  CHECK(pq::media::detectTransients(&q, 1, 48000, 48000).empty());
  const float* tn = tone.data();
  CHECK(pq::media::detectTransients(&tn, 1, 96000, 48000).size() <= 1);  // its start, at most
}

TEST_CASE("transients: the decoder path equals the in-memory one; cached on disk (second run ≤ 1 ms) and in memory") {
  const Fixture f = makeFixture();
  const fs::path dir = fs::temp_directory_path() / "pq_transients_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const fs::path wav = dir / "fixture.wav";
  REQUIRE(pq::media::writeWav(wav, f.ch, 48000).ok());
  const float* ch[2] = {f.ch[0].data(), f.ch[1].data()};
  const std::vector<double> mem = pq::media::detectTransients(ch, 2, static_cast<int64_t>(f.ch[0].size()), 48000);
  auto dec = pq::media::Decoder::open(wav);
  REQUIRE(dec.ok());
  auto fromFile = pq::media::detectTransients(**dec);
  REQUIRE(fromFile.ok());
  CHECK(*fromFile == mem);  // float32 WAV: the same frames

  pq::media::transientCache().clear();
  pq::media::TransientRequest r;
  r.path = wav;
  r.contentHash = 0x1234ABCDull;
  r.frames = static_cast<int64_t>(f.ch[0].size());
  r.cacheDir = dir / "cache" / "analysis";
  auto first = pq::media::loadOrDetectTransients(r);
  REQUIRE(first.ok());
  CHECK((*first)->onsets == mem);
  CHECK(fs::exists(pq::media::transientCacheFile(r.cacheDir, r.contentHash, r.frames)));
  double best = 1e9;
  for (int i = 0; i < 3; ++i) {
    pq::media::transientCache().clear();  // from the disk file
    const auto t0 = std::chrono::steady_clock::now();
    auto again = pq::media::loadOrDetectTransients(r);
    best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    REQUIRE(again.ok());
    CHECK((*again)->onsets == mem);
  }
  MESSAGE("cached transients: ", best, " ms from disk");
  CHECK(best <= 1.0);
  auto mem2 = pq::media::loadOrDetectTransients(r);  // memory: the same object
  REQUIRE(mem2.ok());
  CHECK(mem2->get() == pq::media::transientCache().find(r.contentHash, r.frames).get());
  // A corrupt cache file is ignored (re-detected and rewritten).
  pq::media::transientCache().clear();
  {
    std::FILE* fp = std::fopen(pq::media::transientCacheFile(r.cacheDir, r.contentHash, r.frames).string().c_str(), "wb");
    REQUIRE(fp != nullptr);
    std::fputs("junk", fp);
    std::fclose(fp);
  }
  auto redo = pq::media::loadOrDetectTransients(r);
  REQUIRE(redo.ok());
  CHECK((*redo)->onsets == mem);
  pq::media::transientCache().clear();
  fs::remove_all(dir, ec);
}
