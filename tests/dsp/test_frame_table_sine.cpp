// TMM.12-fix1: sine tables (frame_table.h "Sine tables") — the builder's detection and coefficients, the readers'
// sine path against the exact sine (≤ −120 dBFS peak) and the rows' read of the same table (≤ −120 dBFS RMS, within
// the rows' own 1.6e-6 peak error; the same reader state bit for bit), the switch between the two paths (a blend
// starting, per-sample positions), odd phases, and frameTableSineCoefs (what the JIT's inline frames use).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "frame_table_fixtures.h"
#include "pq/core/alloc_trap.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/frame_table_build.h"

using namespace pq::dsp;

namespace {

constexpr double kTol = 1e-6;  // −120 dBFS

FrameTable buildOrFail(FrameTableBuilder& b, const FrameTableSpec& spec) {
  auto t = b.build(spec);
  REQUIRE_MESSAGE(t.ok(), (t.ok() ? "" : t.error().message));
  return std::move(t.value());
}

FrameTable build(std::vector<ftfix::Cycle> cycles, double sr = 48000, uint32_t dense = 1) {
  FrameTableBuilder b;
  ftfix::SpecHolder s(std::move(cycles), sr, dense);
  return buildOrFail(b, s.spec);
}

ftfix::Cycle withDc(ftfix::Cycle c, float dc) {
  for (float& v : c) v += dc;
  return c;
}

// The same table read without its sine coefficients: the rows' read (the general path).
FrameTableView rowsOf(const FrameTable& t) {
  FrameTableView v = t.view();
  v.sine = nullptr;
  return v;
}

void read(const FrameTableView& t, FrameReadState& st, const float* phase, const float* pos, bool perSample,
          uint32_t lanes, uint32_t frames, float* out) {
  if (lanes == 1) readFrameTableL1(t, st, phase, pos, perSample, frames, out);
  else readFrameTableL8(t, st, phase, pos, perSample, frames, out);
}

// Blocks of phase ramps at log-random pitches from 20 Hz up to past Nyquist (increments up to 0.6 cycles), some
// starting far outside [0, 1); positions from `posFor` (Spl: 8 floats) or per sample. Measures the sine path against
// the rows' read of the same table (peak and RMS) and, in blocks that take it, against the exact sine of the stored
// coefficients (in double); and whether the reader states stayed bit-identical.
struct Compare {
  double rows = 0, rowsRms = 0, exact = 0;
  bool sameState = true;
};
template <class PosFor>
Compare compare(const FrameTable& t, uint32_t lanes, bool perSample, PosFor&& posFor, uint32_t blocks = 40) {
  std::mt19937 rng(17);
  std::uniform_real_distribution<float> uni(0.0f, 1.0f);
  constexpr uint32_t kFrames = 67;
  FrameReadState sa{}, sb{};
  std::vector<float> phase(kFrames * lanes), pos(std::max<uint32_t>(kFrames * lanes, 8)), a(kFrames * lanes),
      b(kFrames * lanes);
  std::vector<double> acc(lanes, 0.0), inc(lanes);
  const FrameTableView rows = rowsOf(t);
  Compare c;
  double sq = 0;
  std::size_t count = 0;
  for (uint32_t blk = 0; blk < blocks; ++blk) {
    for (uint32_t l = 0; l < lanes; ++l)
      inc[l] = std::exp(std::log(20.0 / 48000) + uni(rng) * std::log(0.6 * 48000 / 20.0));
    const double offset = blk % 7 == 3 ? 1000.0 * (uni(rng) - 0.5) : 0.0;  // phases far outside [0, 1)
    for (uint32_t n = 0; n < kFrames; ++n)
      for (uint32_t l = 0; l < lanes; ++l) {
        acc[l] += inc[l];
        acc[l] -= std::floor(acc[l]);
        phase[n * lanes + l] = static_cast<float>(acc[l] + offset);
      }
    posFor(blk, pos, rng);
    read(t.view(), sa, phase.data(), pos.data(), perSample, lanes, kFrames, a.data());
    read(rows, sb, phase.data(), pos.data(), perSample, lanes, kFrames, b.data());
    alignas(32) float dc[8], amp[8], th[8];
    const bool sinePath = frameTableSineCoefs(t.view(), perSample ? nullptr : pos.data(), lanes, dc, amp, th);
    for (std::size_t i = 0; i < a.size(); ++i) {
      const double d = double(a[i]) - b[i];
      c.rows = std::max(c.rows, std::fabs(d));
      sq += d * d;
      ++count;
      if (sinePath) {
        const uint32_t l = static_cast<uint32_t>(i % lanes);
        const double p = phase[i];
        const double want = dc[l] + double(amp[l]) * std::sin(2.0 * std::numbers::pi * (p - std::floor(p) + th[l]));
        c.exact = std::max(c.exact, std::fabs(a[i] - want));
      }
    }
    c.sameState = c.sameState && std::memcmp(&sa, &sb, sizeof(sa)) == 0;
  }
  c.rowsRms = std::sqrt(sq / static_cast<double>(count));
  return c;
}

// The rows' read deviates from the exact sine by up to 1.6e-6 (−116 dBFS peak: the interpolator's gain ripple over
// the fraction, which the pre-emphasis corrects only on average; its images ≥ 130.8 dB down per band add up at the
// peaks), the sine path by ≤ 5e-7. So the two paths agree within the rows' own error: ≤ −120 dBFS RMS, ≤ 2e-6 peak.
constexpr double kPeakVsRows = 2e-6;

}  // namespace

TEST_CASE("sine tables: the builder marks tables of DC + harmonic 1 and stores dc, amplitude and phase offset") {
  // A sine keyframe: dc 0, amplitude 1, θ 0 (the `sine` atom exactly).
  {
    const FrameTable t = build({ftfix::basic(BasicCycle::Sine)});
    REQUIRE(t.isSine());
    CHECK(std::fabs(t.view().sine[0]) < 1e-7);
    CHECK(std::fabs(t.view().sine[1] - 1.0f) < 1e-6);
    CHECK(t.view().sine[2] == 0.0f);
  }
  // 0.7·cos(2πφ + 0.9) + 0.2 = 0.2 + 0.7·sin(2π(φ + 0.9 / 2π + ¼)).
  {
    const FrameTable t = build({withDc(ftfix::fromHarmonics({{1, {0.7, 0.9}}}), 0.2f)}, 44100);
    REQUIRE(t.isSine());
    CHECK(std::fabs(t.view().sine[0] - 0.2f) < 1e-6);
    CHECK(std::fabs(t.view().sine[1] - 0.7f) < 1e-6);
    double th = 0.9 / (2 * std::numbers::pi) + 0.25;
    th -= std::floor(th + 0.5);
    CHECK(std::fabs(t.view().sine[2] - th) < 1e-6);
  }
  // Many frames: one sine keyframe held over 64 frames, and a spectral morph between two sines (every frame a sine).
  {
    const FrameTable held = build({ftfix::basic(BasicCycle::Sine)}, 48000, 64);
    CHECK(held.isSine());
    const FrameTable morph =
        build({ftfix::basic(BasicCycle::Sine), ftfix::fromHarmonics({{1, {0.5, 2.0}}})}, 48000, 16);
    REQUIRE(morph.isSine());
    CHECK(std::fabs(morph.view().sine[4 * 15 + 1] - 0.5f) < 1e-6);
  }
  // Not sines: other harmonics above the tolerance (a saw; a sine with harmonic 2 at −100 dB), a morph to a saw.
  CHECK_FALSE(build({ftfix::basic(BasicCycle::Saw)}).isSine());
  CHECK_FALSE(build({ftfix::fromHarmonics({{1, {1.0, 0.0}}, {2, {1e-5, 0.0}}})}).isSine());
  CHECK_FALSE(build({ftfix::basic(BasicCycle::Sine), ftfix::basic(BasicCycle::Saw)}, 48000, 8).isSine());
  // Below it: harmonic 3 at −180 dB (under the float sine's own rounding) is still a sine.
  CHECK(build({ftfix::fromHarmonics({{1, {1.0, 0.0}}, {3, {1e-9, 0.0}}})}).isSine());
  // A bake that makes harmonics (fold), and the live-band bake (its rows are the live read): not sines.
  {
    FrameTableBuilder b;
    ftfix::SpecHolder s({ftfix::basic(BasicCycle::Sine)}, 48000, 1);
    s.spec.bake = {WarpMode::Bend, 0.0f, 0.5f, 0.0f};
    CHECK_FALSE(buildOrFail(b, s.spec).isSine());
    s.spec.liveBand = true;
    CHECK_FALSE(buildOrFail(b, s.spec).isSine());
  }
  // buildInto over a table that held a sine: the flag follows the latest build.
  {
    FrameTableBuilder b;
    ftfix::SpecHolder sine({ftfix::basic(BasicCycle::Sine)}, 48000, 1), saw({ftfix::basic(BasicCycle::Saw)}, 48000, 1);
    FrameTable t = buildOrFail(b, sine.spec);
    CHECK(t.isSine());
    REQUIRE(b.buildInto(saw.spec, t).ok());
    CHECK_FALSE(t.isSine());
    REQUIRE(b.buildInto(sine.spec, t).ok());
    CHECK(t.isSine());
  }
}

TEST_CASE("sine tables: the sine path matches the rows' read within −120 dBFS with the same reader state (L1, L8)") {
  struct Case {
    const char* name;
    std::vector<ftfix::Cycle> cycles;
    double sr;
    uint32_t dense;
  };
  const Case cases[] = {
      {"sine 48k", {ftfix::basic(BasicCycle::Sine)}, 48000, 1},
      {"sine 44.1k", {ftfix::basic(BasicCycle::Sine)}, 44100, 1},
      {"sine 96k", {ftfix::basic(BasicCycle::Sine)}, 96000, 1},
      {"0.2 + 0.7 cos(+0.9) 48k", {withDc(ftfix::fromHarmonics({{1, {0.7, 0.9}}}), 0.2f)}, 48000, 1},
      {"sine held over 64 frames", {ftfix::basic(BasicCycle::Sine)}, 48000, 64},
      {"sine -> 0.5 sine(+2) over 9 frames", {ftfix::basic(BasicCycle::Sine), ftfix::fromHarmonics({{1, {0.5, 2.0}}})}, 48000, 9},
  };
  for (const Case& cs : cases) {
    const FrameTable t = build(cs.cycles, cs.sr, cs.dense);
    REQUIRE(t.isSine());
    const uint32_t fm1 = cs.dense - 1;
    for (const uint32_t lanes : {1u, 8u})
      for (const bool perSample : {false, true}) {
        if (perSample && cs.dense > 1) continue;  // per-sample positions over many frames: the rows (next case)
        // Spl positions on frames (every lane its own), the sine path for every block.
        const Compare c = compare(t, lanes, perSample, [&](uint32_t, std::vector<float>& pos, std::mt19937& rng) {
          for (float& u : pos) u = fm1 == 0 ? 0.3f : static_cast<float>(rng() % (fm1 + 1)) / static_cast<float>(fm1);
        });
        MESSAGE(std::string(cs.name) << " lanes " << lanes << std::string(perSample ? " per-sample" : " spl")
                                     << ": |sine − exact| " << c.exact << ", |sine − rows| peak " << c.rows << " RMS "
                                     << c.rowsRms);
        CHECK(c.exact <= kTol);
        CHECK(c.rowsRms <= kTol);
        CHECK(c.rows <= kPeakVsRows);
        CHECK(c.sameState);
      }
  }
}

TEST_CASE("sine tables: blocks between frames and per-sample positions take the rows' read; the switch is seamless") {
  const FrameTable t = build({ftfix::basic(BasicCycle::Sine), ftfix::fromHarmonics({{1, {0.5, 2.0}}})}, 48000, 9);
  REQUIRE(t.isSine());
  for (const uint32_t lanes : {1u, 8u}) {
    // Blocks alternate: every lane on a frame (sine path), one lane between frames (the whole block: rows), all between.
    const Compare spl = compare(t, lanes, false, [&](uint32_t blk, std::vector<float>& pos, std::mt19937& rng) {
      for (uint32_t l = 0; l < pos.size(); ++l) pos[l] = static_cast<float>(rng() % 9) / 8.0f;
      if (blk % 3 == 1) pos[lanes == 1 ? 0 : 5] = 0.4321f;
      if (blk % 3 == 2)
        for (float& u : pos) u = 0.05f + 0.9f * static_cast<float>(rng() % 1000) / 1000.0f;
    });
    // Per sample: always the rows (the reader cannot choose the frames once per block).
    const Compare per = compare(t, lanes, true, [&](uint32_t, std::vector<float>& pos, std::mt19937& rng) {
      for (float& u : pos) u = static_cast<float>(rng() % 9) / 8.0f;
    });
    MESSAGE("lanes " << lanes << ": spl |sine − rows| peak " << spl.rows << " RMS " << spl.rowsRms << ", |sine − exact| "
                     << spl.exact << ", per-sample " << per.rows);
    CHECK(spl.rows <= kPeakVsRows);
    CHECK(spl.rowsRms <= kTol);
    CHECK(spl.exact <= kTol);
    CHECK(spl.sameState);
    CHECK(per.rows == 0.0);  // the same code
    CHECK(per.sameState);
  }
  // One signal crossing from the sine path to a blend and back, block by block (a position glide that rests on a
  // frame, leaves it and lands on the next): the step between consecutive samples never exceeds the rows' own.
  FrameReadState st{}, sr{};
  const FrameTableView rows = rowsOf(t);
  constexpr uint32_t kBlock = 32, kBlocks = 64;
  std::vector<float> phase(kBlock), y, yr, out(kBlock);
  double acc = 0.25;
  for (uint32_t blk = 0; blk < kBlocks; ++blk) {
    for (uint32_t n = 0; n < kBlock; ++n) {
      acc += 330.0 / 48000;
      acc -= std::floor(acc);
      phase[n] = static_cast<float>(acc);
    }
    const float pos[8] = {blk < 20 ? 0.25f : blk < 44 ? 0.25f + 0.125f * (blk - 20) / 24.0f : 0.375f};
    readFrameTableL1(t.view(), st, phase.data(), pos, false, kBlock, out.data());
    y.insert(y.end(), out.begin(), out.end());
    readFrameTableL1(rows, sr, phase.data(), pos, false, kBlock, out.data());
    yr.insert(yr.end(), out.begin(), out.end());
  }
  double worst = 0, step = 0, stepRows = 0;
  for (std::size_t n = 0; n < y.size(); ++n) {
    worst = std::max(worst, std::fabs(double(y[n]) - yr[n]));
    if (n > 0) {
      step = std::max(step, std::fabs(double(y[n]) - y[n - 1]));
      stepRows = std::max(stepRows, std::fabs(double(yr[n]) - yr[n - 1]));
    }
  }
  CHECK(worst <= kPeakVsRows);
  CHECK(step <= stepRows + 2 * kPeakVsRows);
}

TEST_CASE("sine tables: odd phases read as the rows read them (±inf / NaN: phase 0); no allocation") {
  const FrameTable t = build({withDc(ftfix::fromHarmonics({{1, {0.7, 0.9}}}), 0.2f)});
  const float odd[] = {std::nanf(""), INFINITY, -INFINITY, -1e-9f, -3.25f, 1e7f, 0.9999999f, 1.0f, 0.0f, 2.5f,
                       0.5f, -0.5f, 1e-30f, 8388609.0f, -16777215.0f, 0.75f};
  constexpr uint32_t kN = 16 * 8;
  std::vector<float> phase(kN), pos(kN, 0.0f), a(kN), b(kN);
  for (uint32_t i = 0; i < kN; ++i) phase[i] = odd[(i * 7) % 16];
  for (const uint32_t lanes : {1u, 8u}) {
    FrameReadState sa{}, sb{};
    uint64_t violations = pq::alloc_trap::countRtViolations([&] {
      read(t.view(), sa, phase.data(), pos.data(), false, lanes, kN / lanes, a.data());
    });
    CHECK(violations == 0);
    read(rowsOf(t), sb, phase.data(), pos.data(), false, lanes, kN / lanes, b.data());
    double worst = 0;
    for (uint32_t i = 0; i < kN; ++i) worst = std::max(worst, std::fabs(double(a[i]) - b[i]));
    CAPTURE(lanes);
    CHECK(worst <= kPeakVsRows);  // ±½ ties may read sin(−π) for sin(π): 1.7e-7 · 0.7
    CHECK(std::memcmp(&sa, &sb, sizeof(sa)) == 0);
  }
}

TEST_CASE("frameTableSineCoefs: the lanes' coefficients when the readers take the sine path, false otherwise") {
  alignas(32) float dc[8], amp[8], th[8];
  const FrameTable one = build({withDc(ftfix::fromHarmonics({{1, {0.7, 0.9}}}), 0.2f)});
  const float pos8[8] = {0.0f, 0.5f, 1.0f, 0.25f, 0.0f, 0.0f, 0.0f, 0.0f};
  REQUIRE(frameTableSineCoefs(one.view(), nullptr, 8, dc, amp, th));  // F = 1: any position, even per sample
  for (uint32_t l = 0; l < 8; ++l) {
    CHECK(dc[l] == one.view().sine[0]);
    CHECK(amp[l] == one.view().sine[1]);
    CHECK(th[l] == one.view().sine[2]);
  }
  REQUIRE(frameTableSineCoefs(one.view(), pos8, 1, dc, amp, th));
  const FrameTable many = build({ftfix::basic(BasicCycle::Sine), ftfix::fromHarmonics({{1, {0.5, 2.0}}})}, 48000, 5);
  CHECK_FALSE(frameTableSineCoefs(many.view(), nullptr, 8, dc, amp, th));  // per-sample position over 5 frames
  REQUIRE(frameTableSineCoefs(many.view(), pos8, 8, dc, amp, th));          // lanes on frames 0, 2, 4, 1, 0 …
  const uint32_t frameOf[8] = {0, 2, 4, 1, 0, 0, 0, 0};
  for (uint32_t l = 0; l < 8; ++l) {
    CHECK(dc[l] == many.view().sine[4 * frameOf[l]]);
    CHECK(amp[l] == many.view().sine[4 * frameOf[l] + 1]);
    CHECK(th[l] == many.view().sine[4 * frameOf[l] + 2]);
  }
  REQUIRE(frameTableSineCoefs(many.view(), pos8 + 1, 1, dc, amp, th));  // one lane: replicated
  CHECK(amp[7] == many.view().sine[4 * 2 + 1]);
  const float between[8] = {0.0f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  CHECK_FALSE(frameTableSineCoefs(many.view(), between, 8, dc, amp, th));
  CHECK_FALSE(frameTableSineCoefs(build({ftfix::basic(BasicCycle::Saw)}).view(), pos8, 8, dc, amp, th));
}

