// The frame-table alias suite (TMM.1 Accept; docs/plans/MM.md): through the AVX2 L1 reader, bin-exact fundamentals
// (f0 = k·sr / 65536), rectangular window, every non-harmonic bin below 20 kHz must be ≤ −100 dBFS, and every
// harmonic up to 19.8 kHz present within 0.1 dB (tests/support/alias_meter.h). Inputs: saw, square, triangle, a
// saw → square spectral morph read at 16 positions, and the 64-keyframe drawn fixture at 8 positions; 25 log-spaced
// pitches from 20 Hz to 12 kHz plus the two bins on either side of every level switch below 12 kHz; at 44.1, 48 and
// 96 kHz. The default tier runs a subset at 48 kHz; the slow tier the whole suite.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "../support/alias_meter.h"
#include "frame_table_fixtures.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/frame_table_build.h"

using namespace pq::dsp;

namespace {

constexpr uint32_t kN = 65536;

struct Worst {
  double alias = -400, aliasHz = 0, aliasF0 = 0;
  double harm = 0, harmF0 = 0;
  uint32_t harmH = 0;
  double quiet = 0;
  std::string aliasWhere, harmWhere;
  uint32_t renders = 0;
};

// The bins to play at one rate: 25 log-spaced pitches 20 Hz – 12 kHz, and both bins around every level switch.
std::vector<uint32_t> pitchBins(const FrameTableLayout& l, bool full) {
  std::set<uint32_t> bins;
  const double sr = l.sampleRate;
  const int count = full ? 25 : 9;
  for (int i = 0; i < count; ++i) {
    const double f = 20.0 * std::pow(12000.0 / 20.0, i / (count - 1.0));
    bins.insert(std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(f * kN / sr))));
  }
  if (full)
    for (uint32_t j = 0; j + 1 < l.levels; ++j) {
      const double b = l.b0 * std::pow(l.ratio, j) * kN;  // the switch, in bins (inc = k / N)
      const auto k = static_cast<uint32_t>(std::floor(b));
      if (k >= 1 && (k + 1) * sr / kN <= 12000.0) {
        bins.insert(k);
        bins.insert(k + 1);
      }
    }
  return {bins.begin(), bins.end()};
}

void play(const FrameTable& t, const FrameTableBuilder& b, float position, uint32_t k, double sr,
          pq::test::AliasMeter& meter, std::vector<float>& phase, std::vector<float>& out, const char* what, Worst& w) {
  // Exact phases (k·n mod N) / N, as a phasor at this bin-exact pitch produces them; the state as if it had been
  // running (the previous phase and delta), so the whole render is one steady level.
  for (uint32_t n = 0; n < kN; ++n) phase[n] = static_cast<float>((static_cast<uint64_t>(k) * n) % kN) / kN;
  FrameReadState st{};
  st.prevPhase[0] = static_cast<float>((static_cast<uint64_t>(kN) - k) % kN) / kN;
  st.prevDelta[0] = static_cast<float>(k) / kN;
  readFrameTableL1(t.view(), st, phase.data(), &position, false, kN, out.data());
  // Expected harmonics: the blend of the two dense frames at this position (the reader's frame choice).
  const uint32_t frames = t.frames();
  uint32_t f0 = 0;
  double wgt = 0;
  if (frames > 1) {
    const double fp = static_cast<double>(position) * (frames - 1);
    f0 = std::min(static_cast<uint32_t>(fp), frames - 2);
    wgt = fp - f0;
  }
  const auto ca = b.frameSpectrum(f0), cb = b.frameSpectrum(std::min(f0 + 1, frames - 1));
  auto expected = [&](uint32_t h) {
    if (h > kFrameTableMaxHarmonic) return -1.0;
    return 2.0 * std::abs(ca[h] + wgt * (cb[h] - ca[h]));
  };
  const auto r = meter.measure(out, k, sr, expected);
  const double hz = k * sr / kN;
  ++w.renders;
  char where[160];
  std::snprintf(where, sizeof where, "%s pos %.3f f0 %.2f Hz @ %.0f", what, position, hz, sr);
  if (r.worstAliasDb > w.alias) {
    w.alias = r.worstAliasDb;
    w.aliasHz = r.worstAliasHz;
    w.aliasWhere = where;
  }
  if (r.worstHarmonicDb > w.harm) {
    w.harm = r.worstHarmonicDb;
    w.harmH = r.worstHarmonic;
    w.harmWhere = where;
  }
  w.quiet = std::max(w.quiet, r.worstQuietError);
}

void suite(double sr, bool full) {
  auto layout = frameTableLayout(sr);
  REQUIRE(layout.ok());
  const auto bins = pitchBins(layout.value(), full);
  pq::test::AliasMeter meter(kN);
  std::vector<float> phase(kN), out(kN);
  Worst w;
  auto run = [&](std::vector<ftfix::Cycle> cycles, uint32_t dense, std::vector<float> positions, const char* what) {
    FrameTableBuilder b;
    ftfix::SpecHolder s(std::move(cycles), sr, dense);
    auto t = b.build(s.spec);
    REQUIRE(t.ok());
    for (const float u : positions)
      for (const uint32_t k : bins) play(t.value(), b, u, k, sr, meter, phase, out, what, w);
  };
  run({ftfix::basic(BasicCycle::Saw)}, 1, {0.0f}, "saw");
  if (full) {
    run({ftfix::basic(BasicCycle::Square)}, 1, {0.0f}, "square");
    run({ftfix::basic(BasicCycle::Triangle)}, 1, {0.0f}, "triangle");
  }
  std::vector<float> morphPos;
  for (int i = 0; i < (full ? 16 : 3); ++i) morphPos.push_back(full ? i / 15.0f : 0.2f + 0.3f * i);
  run({ftfix::basic(BasicCycle::Saw), ftfix::basic(BasicCycle::Square)}, 64, morphPos, "saw->square");
  std::vector<float> fixturePos = full ? std::vector<float>{0.0f, 0.1f, 0.25f, 0.33f, 0.5f, 0.71f, 0.9f, 1.0f}
                                       : std::vector<float>{0.37f};
  run(ftfix::drawnFixture(64), 64, fixturePos, "drawn");
  MESSAGE(sr << " Hz, " << w.renders << " renders: worst alias " << w.alias << " dBFS at " << w.aliasHz << " Hz ("
              << w.aliasWhere << "); worst harmonic error " << w.harm << " dB (h " << w.harmH << ", " << w.harmWhere
              << "); quiet harmonics |err| ≤ " << w.quiet);
  CHECK(w.alias <= -100.0);
  CHECK(w.harm <= 0.1);
  CHECK(w.quiet <= 1e-5);
}

}  // namespace

TEST_CASE("frame table alias suite (48 kHz subset): aliases ≤ −100 dBFS below 20 kHz, harmonics within 0.1 dB") {
  suite(48000.0, false);
}

TEST_CASE("frame table alias suite: 44.1 / 48 / 96 kHz, every input, pitch and level switch" * doctest::test_suite("slow")) {
  for (const double sr : {44100.0, 48000.0, 96000.0}) suite(sr, true);
}
