// TMM.2: the wavetable_pos kernel (live frame-table slots) and the voice_tap sink — the kernel against TMM.1's readers
// (bit for bit) and the double reference (≤ 1e-6), independence of the call partition, the swap crossfade (formula,
// no click), slot generations, the taps' samples, RT safety, and a two-thread swap stress with poisoned retirements.
// TMM.12-fix1: sine tables through the kernel (vs the rows' read, the JIT's sineOn, swaps between the two paths).

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "frame_table_fixtures.h"
#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/core/rt_scope.h"
#include "pq/core/voice_tap.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/dsp/frame_table_slot.h"

using namespace pq::dsp;
using pq::AlignedBuffer;

namespace {

using TablePtr = std::shared_ptr<const FrameTable>;

TablePtr makeTable(std::vector<ftfix::Cycle> cycles, double sr = 48000, uint32_t dense = 64) {
  FrameTableBuilder b;
  ftfix::SpecHolder s(std::move(cycles), sr, dense);
  auto t = b.build(s.spec);
  REQUIRE_MESSAGE(t.ok(), (t.ok() ? "" : t.error().message));
  return std::make_shared<const FrameTable>(std::move(t.value()));
}

TablePtr sawSquare() { return makeTable({ftfix::basic(BasicCycle::Saw), ftfix::basic(BasicCycle::Square)}); }

// AU.fold: the swap fade's frames at the tables' 48 kHz (kFrameTableFadeSeconds).
const uint32_t kF = frameTableFadeFrames(48000.0);
TablePtr drawn() { return makeTable(ftfix::drawnFixture(8)); }

// One wavetable_pos instance: its state block and the kernel for (lanes, Spl mask: bit 0 phase, bit 1 position).
struct Wp {
  uint32_t lanes, mask;
  AlignedBuffer<float> state = AlignedBuffer<float>(sizeof(WavetablePosState) / sizeof(float));
  KernelFn fn;
  Wp(uint32_t l, uint32_t m, uint32_t group = 0) : lanes(l), mask(m), fn(findKernel(Atom::WavetablePos, l, m)) {
    REQUIRE(fn != nullptr);
    StateInit si;
    si.lanes = l;
    si.voiceGroup = group;
    initState(Atom::WavetablePos, state.data(), si);
  }
  void run(const FrameTableSlot& slot, const float* phase, const float* pos, uint32_t frames, float* out) {
    const float* in[2] = {phase, pos};
    float* o[1] = {out};
    const KernelArgs a{in, o, state.data(), &slot, frames, 2, 1, mask};
    fn(a);
  }
  WavetablePosState& st() { return *reinterpret_cast<WavetablePosState*>(state.data()); }
};

// Per-lane phase ramps at log-random pitches (20 Hz … 12 kHz at 48 kHz), and positions.
struct Inputs {
  uint32_t lanes;
  std::mt19937 rng{5};
  std::uniform_real_distribution<float> uni{0.0f, 1.0f};
  std::vector<double> acc, inc;
  explicit Inputs(uint32_t l) : lanes(l), acc(l, 0.0), inc(l) { repitch(); }
  void repitch() {
    for (uint32_t l = 0; l < lanes; ++l) inc[l] = std::exp(std::log(20.0 / 48000) + uni(rng) * std::log(600.0));
  }
  void fill(std::vector<float>& phase, std::vector<float>& pos, uint32_t frames, bool posSpl) {
    phase.assign(static_cast<size_t>(frames) * lanes, 0.0f);
    pos.assign(std::max<size_t>(static_cast<size_t>(frames) * lanes, 8), 0.0f);
    for (uint32_t n = 0; n < frames; ++n)
      for (uint32_t l = 0; l < lanes; ++l) {
        acc[l] += inc[l];
        acc[l] -= std::floor(acc[l]);
        phase[n * lanes + l] = static_cast<float>(acc[l]);
        pos[n * lanes + l] = posSpl ? 0.0f : uni(rng);
      }
    if (posSpl) {
      const float v = uni(rng);
      for (uint32_t l = 0; l < 8; ++l) pos[l] = lanes == 1 ? v : uni(rng);
    }
  }
};

void readBlock(const FrameTable& t, FrameReadState& st, const float* phase, const float* pos, bool perSample,
               uint32_t lanes, uint32_t frames, float* out) {
  if (lanes == 1) readFrameTableL1(t.view(), st, phase, pos, perSample, frames, out);
  else readFrameTableL8(t.view(), st, phase, pos, perSample, frames, out);
}

bool bitsEqual(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

}  // namespace

TEST_CASE("wavetable_pos: the kernel is TMM.1's reader bit for bit and the double reference within 1e-6") {
  const TablePtr table = drawn();
  FrameTableSlot slot(table);
  for (const uint32_t lanes : {1u, 8u})
    for (const uint32_t mask : {0u, 1u, 2u, 3u}) {
      const bool phaseSpl = (mask & 1u) != 0, posSpl = (mask & 2u) != 0;
      CAPTURE(lanes);
      CAPTURE(mask);
      Wp k(lanes, mask);
      FrameReadState rd{}, ref{};
      Inputs in(lanes);
      constexpr uint32_t kFrames = 67, kBlocks = 40;
      std::vector<float> phase, pos, out(kFrames * lanes), want(kFrames * lanes), expanded(kFrames * lanes);
      std::vector<double> refOut(kFrames * lanes);
      double worst = 0;
      bool exact = true;
      for (uint32_t b = 0; b < kBlocks; ++b) {
        in.repitch();
        in.fill(phase, pos, kFrames, posSpl);
        if (phaseSpl) {  // a constant phase pattern (8 per-lane values; the 1-lane form repeats lane 0)
          for (uint32_t l = 0; l < 8; ++l) phase[l] = lanes == 1 ? phase[0] : phase[l % lanes];
          for (uint32_t n = 0; n < kFrames; ++n)
            for (uint32_t l = 0; l < lanes; ++l) expanded[n * lanes + l] = phase[l];
        } else {
          expanded = phase;
        }
        k.run(slot, phase.data(), pos.data(), kFrames, out.data());
        readBlock(*table, rd, expanded.data(), pos.data(), !posSpl, lanes, kFrames, want.data());
        readFrameTableReferenceBlock(table->view(), ref, expanded.data(), pos.data(), !posSpl, lanes, kFrames, refOut.data());
        exact = exact && bitsEqual(out, want);
        for (size_t i = 0; i < out.size(); ++i) worst = std::max(worst, std::fabs(out[i] - refOut[i]));
      }
      CHECK(exact);
      CHECK(worst <= 1e-6);
    }
}

TEST_CASE("wavetable_pos: any split of a block into calls gives the same bits, through a swap crossfade") {
  const TablePtr a = drawn(), b = sawSquare();
  for (const uint32_t lanes : {1u, 8u})
    for (const uint32_t mask : {0u, 2u, 1u}) {
      CAPTURE(lanes);
      CAPTURE(mask);
      pq::EpochReclaimer rec;
      FrameTableSlot s1(a), s2(a);
      Wp whole(lanes, mask), split(lanes, mask);
      Inputs in(lanes);
      std::mt19937 rng(9);
      constexpr uint32_t kFrames = 256;
      std::vector<float> phase, pos, o1(kFrames * lanes), o2(kFrames * lanes);
      bool same = true;
      for (uint32_t blk = 0; blk < 12; ++blk) {
        if (blk == 3 || blk == 7) {  // swaps (the second one back to a)
          s1.publish(blk == 3 ? b : a, rec);
          s2.publish(blk == 3 ? b : a, rec);
        }
        in.repitch();
        in.fill(phase, pos, kFrames, (mask & 2u) != 0);
        whole.run(s1, phase.data(), pos.data(), kFrames, o1.data());
        for (uint32_t f = 0; f < kFrames;) {
          const uint32_t n = std::min<uint32_t>(kFrames - f, 1 + rng() % 70);
          const float* pp = (mask & 1u) ? phase.data() : phase.data() + f * lanes;
          const float* pu = (mask & 2u) ? pos.data() : pos.data() + f * lanes;
          split.run(s2, pp, pu, n, o2.data() + f * lanes);
          f += n;
        }
        same = same && bitsEqual(o1, o2);
      }
      CHECK(same);
    }
}

TEST_CASE("wavetable_pos: a swap crossfades linearly over kFrameTableFadeSeconds (out = old + g·(new − old)), then plays the new table") {
  const TablePtr a = drawn(), b = sawSquare();
  for (const uint32_t lanes : {1u, 8u}) {
    CAPTURE(lanes);
    pq::EpochReclaimer rec;
    FrameTableSlot slot(a);
    Wp k(lanes, 2);  // Spl position
    Inputs in(lanes);
    std::vector<float> phase, pos, out;
    // 100 frames on a, then the swap, then 7-frame calls (the fade spans calls).
    in.fill(phase, pos, 100, true);
    out.resize(phase.size());
    k.run(slot, phase.data(), pos.data(), 100, out.data());
    CHECK(k.st().generation == 1);
    FrameReadState ra{}, rb{};
    std::memcpy(&ra, &k.st(), sizeof(ra));
    rb = ra;
    slot.publish(b, rec);
    CHECK(slot.generation() == 2);
    const float posV = pos[0];
    std::vector<float> posPat(8);
    for (uint32_t l = 0; l < 8; ++l) posPat[l] = pos[l];
    bool formula = true;
    uint32_t t = 0;
    for (uint32_t call = 0; call < (kF + 20) / 7 + 1; ++call) {
      in.fill(phase, pos, 7, true);
      std::copy(posPat.begin(), posPat.end(), pos.begin());
      std::vector<float> wa(7 * lanes), wb(7 * lanes);
      out.assign(7 * lanes, 0.0f);
      k.run(slot, phase.data(), pos.data(), 7, out.data());
      readBlock(*a, ra, phase.data(), pos.data(), false, lanes, 7, wa.data());
      readBlock(*b, rb, phase.data(), pos.data(), false, lanes, 7, wb.data());
      for (uint32_t f = 0; f < 7; ++f, ++t)
        for (uint32_t l = 0; l < lanes; ++l) {
          const float x = wa[f * lanes + l], y = wb[f * lanes + l];
          const float g = static_cast<float>(t + 1) * (1.0f / static_cast<float>(kF));
          const float want = t + 1 < kF ? std::fma(g, y - x, x) : y;
          formula = formula && std::memcmp(&want, &out[f * lanes + l], 4) == 0;
        }
    }
    (void)posV;
    CHECK(formula);
    CHECK(k.st().fadeLeft == 0);
    CHECK(k.st().generation == 2);
  }
}

TEST_CASE("wavetable_pos: generations — a fresh state does not fade, a state behind fades from the table it played, "
          "a publish during a fade waits for it, a state whose table is gone reads the newest at once") {
  // AU.fold (frame_table_slot.h "Publication"): a fade is never cut, so the output never jumps between tables.
  const TablePtr a = drawn(), b = sawSquare(), c = makeTable({ftfix::basic(BasicCycle::Triangle)});
  pq::EpochReclaimer rec;
  FrameTableSlot slot(a);
  slot.publish(b, rec);
  Wp fresh(1, 2);
  std::vector<float> phase(16, 0.25f), pos(8, 0.0f), out(16);
  fresh.run(slot, phase.data(), pos.data(), 16, out.data());
  CHECK(fresh.st().generation == 2);
  CHECK(fresh.st().fadeLeft == 0);
  // Two publishes behind: it fades from the table it played (generation 2, still on the chain) to the newest.
  slot.publish(a, rec);
  slot.publish(c, rec);
  CHECK(slot.generation() == 4);
  fresh.run(slot, phase.data(), pos.data(), 16, out.data());
  CHECK(fresh.st().generation == 4);
  CHECK(fresh.st().fromGen == 2);
  CHECK(fresh.st().fadeLeft == kF - 16);
  // A publish inside the fade waits: the fade 2 -> 4 runs on, then 4 -> 5.
  slot.publish(a, rec);
  fresh.run(slot, phase.data(), pos.data(), 16, out.data());
  CHECK(fresh.st().generation == 4);
  CHECK(fresh.st().fadeLeft == kF - 32);
  std::vector<float> longPhase(kF, 0.25f), longOut(kF);
  fresh.run(slot, longPhase.data(), pos.data(), kF - 32 + 10, longOut.data());  // the fade's end, 10 frames into the next
  CHECK(fresh.st().generation == 5);
  CHECK(fresh.st().fromGen == 4);
  CHECK(fresh.st().fadeLeft == kF - 10);
  fresh.run(slot, longPhase.data(), pos.data(), kF, longOut.data());
  CHECK(fresh.st().fadeLeft == 0);
  CHECK(fresh.st().generation == 5);
  // A state behind by more entries than the slot keeps (kFrameTableMaxEntries): its table is gone; the newest at once.
  for (uint32_t i = 0; i < kFrameTableMaxEntries + 4; ++i) slot.publish(i % 2 ? a : b, rec);
  CHECK(FrameTableSlot::find(slot.acquire(), 5) == nullptr);
  fresh.run(slot, phase.data(), pos.data(), 16, out.data());
  CHECK(fresh.st().generation == slot.generation());
  CHECK(fresh.st().fadeLeft == 0);
  rec.collect();
}

TEST_CASE("wavetable_pos: publishes faster than the fade (a knob drag) give one continuous crossfade: never a jump") {
  // AU.fold: a table every 300 frames (≈ 6 ms, faster than any drag) while the kernel plays. The output's largest step
  // stays within the tables' own largest step plus the fade's ramp: the fades queue, so there is no step neither table
  // makes (a cut fade jumped from the mix to the table it was fading to).
  // Smooth tables (a cut fade's jump would stand out above their own small steps): a sine, a phase-shifted sine with
  // a third harmonic, a triangle.
  const TablePtr t[3] = {makeTable({ftfix::basic(BasicCycle::Sine)}, 48000, 1),
                         makeTable({ftfix::fromHarmonics({{1, {0.6, 1.3}}, {3, {0.3, 0.2}}})}, 48000, 1),
                         makeTable({ftfix::basic(BasicCycle::Triangle)}, 48000, 1)};
  for (const double hz : {110.0, 1234.5}) {
    constexpr uint32_t kN = 12000;
    std::vector<float> phase(kN), pos(8, 0.0f), y(kN), r[3];
    double acc = 0.37;
    for (uint32_t n = 0; n < kN; ++n) {
      phase[n] = static_cast<float>(acc);
      acc += hz / 48000.0;
      acc -= std::floor(acc);
    }
    double own = 0, ramp = 0;
    for (int i = 0; i < 3; ++i) {
      FrameTableSlot si(t[i]);
      Wp ki(1, 2);
      r[i].resize(kN);
      ki.run(si, phase.data(), pos.data(), kN, r[i].data());
      for (uint32_t n = 257; n < kN; ++n) own = std::max(own, std::fabs(double(r[i][n]) - r[i][n - 1]));
    }
    for (uint32_t n = 0; n < kN; ++n)
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) ramp = std::max(ramp, std::fabs(double(r[i][n]) - r[j][n]) / kF);
    pq::EpochReclaimer rec;
    FrameTableSlot slot(t[0]);
    Wp k(1, 2);
    uint32_t next = 1;
    for (uint32_t n = 0; n < kN; n += 100) {
      if (n >= 1000 && n % 300 == 0 && n < 8000) slot.publish(t[next++ % 3], rec);
      k.run(slot, phase.data() + n, pos.data(), 100, y.data() + n);
    }
    double step = 0;
    for (uint32_t n = 257; n < kN; ++n) step = std::max(step, std::fabs(double(y[n]) - y[n - 1]));
    CAPTURE(hz);
    MESSAGE("publishes every 300 frames, fade " << kF << ": largest step " << step << ", tables' own " << own
                                                << ", fade ramp " << ramp);
    CHECK(step <= own + ramp + 1e-6);
    rec.collect();
  }
}

TEST_CASE("wavetable_pos: a swap between two different tables mid-note does not click") {
  // A click would be a step neither table makes and the linear fade (kF frames) does not explain. Per sample in the
  // fade, out = a + g·(b − a) with g rising by 1/kF, so |Δout| ≤ (1 − g)|Δa| + g|Δb| + |b − a|/kF: the bound checked is
  // the larger table's own largest step plus the fade's own ramp (max |b − a| / kF) — nothing else (a wrong state, a
  // jump at the fade's end, a dangling table) may add to it. The plan's stricter "+ 1e-3" criterion is measured and
  // reported (QUESTIONS Q?-TMM.2-a): smooth tables at low pitches exceed it by up to the ramp term.
  const TablePtr a = makeTable({ftfix::basic(BasicCycle::Saw)}), b = makeTable(ftfix::drawnFixture(1, 3));
  const TablePtr c = makeTable({ftfix::basic(BasicCycle::Square)}), d = makeTable({ftfix::basic(BasicCycle::Sine)});
  const std::pair<TablePtr, TablePtr> pairs[] = {{a, b}, {a, c}, {d, a}, {c, d}, {b, d}};
  double worstExcess = -1e9, worstRamp = 0;
  uint32_t renders = 0, overPlan = 0;
  for (const auto& [from, to] : pairs)
    for (const double hz : {55.0, 220.0, 1234.5, 5000.0})
      for (const uint32_t swapAt : {1000u, 1013u, 1039u, 1057u, 1100u, 1301u}) {
        constexpr uint32_t kN = 4096;
        std::vector<float> phase(kN), pos(8, 0.0f), y(kN), ya(kN), yb(kN);
        double acc = 0.37;
        for (uint32_t n = 0; n < kN; ++n) {
          phase[n] = static_cast<float>(acc);
          acc += hz / 48000.0;
          acc -= std::floor(acc);
        }
        pq::EpochReclaimer rec;
        FrameTableSlot slot(from), sa(from), sb(to);
        Wp k(1, 2), ka(1, 2), kb(1, 2);
        k.run(slot, phase.data(), pos.data(), swapAt, y.data());
        slot.publish(to, rec);
        k.run(slot, phase.data() + swapAt, pos.data(), kN - swapAt, y.data() + swapAt);
        ka.run(sa, phase.data(), pos.data(), kN, ya.data());
        kb.run(sb, phase.data(), pos.data(), kN, yb.data());
        double step = 0, stepA = 0, stepB = 0, diff = 0;
        for (uint32_t n = 257; n < kN; ++n) {  // past the first period's level settling
          step = std::max(step, std::fabs(double(y[n]) - y[n - 1]));
          stepA = std::max(stepA, std::fabs(double(ya[n]) - ya[n - 1]));
          stepB = std::max(stepB, std::fabs(double(yb[n]) - yb[n - 1]));
        }
        for (uint32_t n = swapAt; n < swapAt + kF; ++n) diff = std::max(diff, std::fabs(double(ya[n]) - yb[n]));
        // Outside the fade the render is exactly one table or the other.
        bool exactOutside = true;
        for (uint32_t n = 0; n < kN; ++n)
          if (n < swapAt || n >= swapAt + kF - 1) exactOutside = exactOutside && y[n] == (n < swapAt ? ya[n] : yb[n]);
        CHECK(exactOutside);
        const double own = std::max(stepA, stepB);
        worstExcess = std::max(worstExcess, step - own);
        worstRamp = std::max(worstRamp, diff / kF);
        overPlan += step > own + 1e-3 ? 1u : 0u;
        ++renders;
        CHECK_MESSAGE(step <= own + diff / kF + 1e-6, hz << " Hz swap at " << swapAt << ": step " << step << " vs "
                                                        << stepA << " / " << stepB << ", ramp " << diff / kF);
      }
  MESSAGE(renders << " swaps: largest step − the larger table's own largest step = " << worstExcess
                  << " (largest fade ramp max|b − a|/kF = " << worstRamp << "); over the plan's +1e-3: " << overPlan);
}

// ---- TMM.12-fix1: sine tables (frame_table.h "Sine tables") ------------------------------------------------------

namespace {

// The same table without its sine coefficients: the kernel then reads the rows (the general path).
TablePtr rowsTable(std::vector<ftfix::Cycle> cycles, uint32_t dense = 1) {
  FrameTableBuilder b;
  ftfix::SpecHolder s(std::move(cycles), 48000, dense);
  auto t = b.build(s.spec);
  REQUIRE(t.ok());
  FrameTable table = std::move(t.value());
  table.setSine(false);
  return std::make_shared<const FrameTable>(std::move(table));
}

ftfix::Cycle shiftedSine() { return ftfix::fromHarmonics({{1, {0.6, 1.3}}}); }

}  // namespace

TEST_CASE("wavetable_pos: on a sine table the kernel is the rows' read within −120 dBFS RMS, with the same reader state") {
  const TablePtr sine = makeTable({shiftedSine()}, 48000, 1), rows = rowsTable({shiftedSine()});
  REQUIRE(sine->isSine());
  REQUIRE_FALSE(rows->isSine());
  for (const uint32_t lanes : {1u, 8u})
    for (const uint32_t mask : {0u, 1u, 2u, 3u}) {
      FrameTableSlot ss(sine), sr(rows);
      Wp a(lanes, mask), b(lanes, mask);
      Inputs in(lanes);
      std::vector<float> phase, pos, ya, yb;
      double worst = 0, sq = 0;
      std::size_t count = 0;
      bool sameState = true;
      for (uint32_t blk = 0; blk < 60; ++blk) {
        const uint32_t n = 1 + (blk * 37) % 97;  // 1-frame calls (a feedback loop) to whole blocks
        in.repitch();
        in.fill(phase, pos, n, (mask & 2u) != 0);
        ya.assign(phase.size(), 0.0f);
        yb.assign(phase.size(), 0.0f);
        a.run(ss, phase.data(), pos.data(), n, ya.data());
        b.run(sr, phase.data(), pos.data(), n, yb.data());
        for (std::size_t i = 0; i < ya.size(); ++i) {
          const double d = double(ya[i]) - yb[i];
          worst = std::max(worst, std::fabs(d));
          sq += d * d;
          ++count;
        }
        sameState = sameState && std::memcmp(a.state.data(), b.state.data(), sizeof(FrameReadState)) == 0;
      }
      CAPTURE(lanes);
      CAPTURE(mask);
      // Within the rows' own deviation from the exact sine (1.6e-6 peak; test_frame_table_sine.cpp).
      CHECK(std::sqrt(sq / static_cast<double>(count)) <= 1e-6);
      CHECK(worst <= 2e-6);
      CHECK(sameState);
    }
}

TEST_CASE("wavetable_pos: sineOn and the lanes' coefficients (the JIT's inline frames) only while the next frames may be inline") {
  const TablePtr sine = makeTable({shiftedSine()}, 48000, 1);
  const TablePtr other = makeTable({ftfix::basic(BasicCycle::Sine)}, 48000, 1);
  const TablePtr morph = makeTable({ftfix::basic(BasicCycle::Sine), shiftedSine()}, 48000, 5);
  REQUIRE(morph->isSine());
  std::vector<float> phase(64 * 8), pos(64 * 8, 0.0f), out(64 * 8);
  for (size_t i = 0; i < phase.size(); ++i) phase[i] = std::fmod(0.0123f * static_cast<float>(i), 1.0f);
  pq::EpochReclaimer rec;
  FrameTableSlot slot(sine);
  Wp k(8, 2);
  k.run(slot, phase.data(), pos.data(), 1, out.data());
  REQUIRE(k.st().sineOn == 1u);
  for (uint32_t l = 0; l < 8; ++l) {
    CHECK(k.st().sineDc[l] == sine->view().sine[0]);
    CHECK(k.st().sineAmp[l] == sine->view().sine[1]);
    CHECK(k.st().sineTh[l] == sine->view().sine[2]);
  }
  // A tap reader: the kernel writes the tap, so every frame must call it.
  slot.tap()->addReader();
  k.run(slot, phase.data(), pos.data(), 1, out.data());
  CHECK(k.st().sineOn == 0u);
  slot.tap()->removeReader();
  k.run(slot, phase.data(), pos.data(), 1, out.data());
  CHECK(k.st().sineOn == 1u);
  // A swap: off while the crossfade runs (the kernel mixes two tables), on again after its kF frames.
  slot.publish(other, rec);
  k.run(slot, phase.data(), pos.data(), 16, out.data());
  CHECK(k.st().fadeLeft == kF - 16);
  CHECK(k.st().sineOn == 0u);
  for (uint32_t left = kF - 17; left > 0;) {
    const uint32_t m = std::min<uint32_t>(left, 64);
    k.run(slot, phase.data(), pos.data(), m, out.data());
    left -= m;
  }
  CHECK(k.st().sineOn == 0u);
  k.run(slot, phase.data(), pos.data(), 1, out.data());
  CHECK(k.st().fadeLeft == 0u);
  CHECK(k.st().sineOn == 1u);
  CHECK(k.st().sineTh[3] == other->view().sine[2]);
  // Not a sine table: off.
  slot.publish(sawSquare(), rec);
  for (uint32_t f = 0; f < kF + 6; ++f) k.run(slot, phase.data(), pos.data(), 1, out.data());
  CHECK(k.st().sineOn == 0u);
  // Many sine frames: on with every lane on a frame (each its frame's coefficients), off between frames and with a
  // per-sample position.
  FrameTableSlot ms(morph);
  Wp spl(8, 2), per(8, 0);
  const float onFrames[8] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.0f, 0.5f, 1.0f};
  std::copy_n(onFrames, 8, pos.begin());
  spl.run(ms, phase.data(), pos.data(), 3, out.data());
  REQUIRE(spl.st().sineOn == 1u);
  for (uint32_t l = 0; l < 8; ++l)
    CHECK(spl.st().sineAmp[l] == morph->view().sine[4 * static_cast<uint32_t>(onFrames[l] * 4) + 1]);
  pos[2] = 0.6f;
  spl.run(ms, phase.data(), pos.data(), 3, out.data());
  CHECK(spl.st().sineOn == 0u);
  per.run(ms, phase.data(), pos.data(), 3, out.data());
  CHECK(per.st().sineOn == 0u);
  // One frame: any position, per sample too.
  Wp perOne(1, 0);
  perOne.run(slot, phase.data(), pos.data(), 3, out.data());  // the slot holds the saw → square table now
  CHECK(perOne.st().sineOn == 0u);
  FrameTableSlot one(sine);
  perOne.run(one, phase.data(), pos.data(), 3, out.data());
  CHECK(perOne.st().sineOn == 1u);
}

TEST_CASE("wavetable_pos: a one-frame sine table swapped to many frames (preview builds, keyframe edits) and back does not click") {
  // As the swap test above: outside the fade (kF frames) the render is exactly one table or the other, and inside it
  // the largest step is at most the larger table's own plus the fade's ramp. Here the tables switch the readers
  // between the sine path and the rows: a one-frame sine, an 8-frame preview of sine → saw (frame 0 a sine, the table
  // not: the rows), a 64-frame saw → square, a 9-frame morph of sines (the sine path with gathered coefficients).
  const TablePtr sine = makeTable({ftfix::basic(BasicCycle::Sine)}, 48000, 1);
  const TablePtr preview = makeTable({ftfix::basic(BasicCycle::Sine), ftfix::basic(BasicCycle::Saw)}, 48000, 8);
  const TablePtr morph = makeTable({ftfix::basic(BasicCycle::Sine), shiftedSine()}, 48000, 9);
  const TablePtr big = sawSquare();
  REQUIRE(sine->isSine());
  REQUIRE_FALSE(preview->isSine());
  REQUIRE(morph->isSine());
  const std::pair<TablePtr, TablePtr> pairs[] = {{sine, preview}, {preview, sine}, {sine, big}, {big, sine},
                                                 {sine, morph},   {morph, preview}};
  uint32_t renders = 0;
  for (const auto& [from, to] : pairs)
    for (const float u : {0.0f, 0.5f})
      for (const double hz : {55.0, 440.0, 5000.0})
        for (const uint32_t swapAt : {1000u, 1037u}) {
          constexpr uint32_t kN = 4096;
          std::vector<float> phase(kN), pos(8, u), y(kN), ya(kN), yb(kN);
          double acc = 0.37;
          for (uint32_t n = 0; n < kN; ++n) {
            phase[n] = static_cast<float>(acc);
            acc += hz / 48000.0;
            acc -= std::floor(acc);
          }
          pq::EpochReclaimer rec;
          FrameTableSlot slot(from), sa(from), sb(to);
          Wp k(1, 2), ka(1, 2), kb(1, 2);
          // In 1-frame calls up to the swap (a feedback loop's shape), then whole blocks.
          for (uint32_t n = 0; n < swapAt; ++n) k.run(slot, phase.data() + n, pos.data(), 1, y.data() + n);
          slot.publish(to, rec);
          k.run(slot, phase.data() + swapAt, pos.data(), kN - swapAt, y.data() + swapAt);
          ka.run(sa, phase.data(), pos.data(), kN, ya.data());
          kb.run(sb, phase.data(), pos.data(), kN, yb.data());
          double step = 0, stepA = 0, stepB = 0, diff = 0;
          for (uint32_t n = 257; n < kN; ++n) {
            step = std::max(step, std::fabs(double(y[n]) - y[n - 1]));
            stepA = std::max(stepA, std::fabs(double(ya[n]) - ya[n - 1]));
            stepB = std::max(stepB, std::fabs(double(yb[n]) - yb[n - 1]));
          }
          for (uint32_t n = swapAt; n < swapAt + kF; ++n) diff = std::max(diff, std::fabs(double(ya[n]) - yb[n]));
          bool exactOutside = true;
          for (uint32_t n = 0; n < kN; ++n)
            if (n < swapAt || n >= swapAt + kF - 1) exactOutside = exactOutside && y[n] == (n < swapAt ? ya[n] : yb[n]);
          CHECK(exactOutside);
          CHECK_MESSAGE(step <= std::max(stepA, stepB) + diff / kF + 1e-6,
                        hz << " Hz, position " << u << ", swap at " << swapAt << ": step " << step << " vs " << stepA
                           << " / " << stepB << ", ramp " << diff / kF);
          ++renders;
        }
  MESSAGE(renders << " swaps between the sine path and the rows");
}

TEST_CASE("wavetable_pos: the voice tap gets the phases read (exact), the position and increment, only with a reader") {
  const TablePtr table = drawn();
  pq::VoiceTapConfig tc;
  tc.groups = 4;
  tc.stride = 8;
  FrameTableSlot slot(table, tc);
  pq::VoiceTap& tap = *slot.tap();
  for (const uint32_t lanes : {1u, 8u}) {
    CAPTURE(lanes);
    const uint32_t group = lanes == 8 ? 3u : 0u;
    Wp k(lanes, 0, group);
    Inputs in(lanes);
    std::vector<float> phase, pos, out(64 * lanes);
    const uint64_t w0 = tap.written(group);
    in.fill(phase, pos, 64, false);
    k.run(slot, phase.data(), pos.data(), 64, out.data());
    CHECK(tap.written(group) == w0);  // nobody reads: nothing written
    tap.addReader();
    tap.setLaneMask(group, 0x5Au);
    bool exact = true;
    for (uint32_t blk = 0; blk < 10; ++blk) {
      in.fill(phase, pos, 64, false);
      // Phases beyond [0, 1) are read as their fraction: the tap stores what was read.
      for (uint32_t i = 0; i < phase.size(); i += 5) phase[i] += 3.0f;
      k.run(slot, phase.data(), pos.data(), 64, out.data());
      REQUIRE(tap.written(group) == w0 + blk + 1);
      pq::VoiceTapRecord r;
      REQUIRE(tap.read(group, w0 + blk, r));
      CHECK(r.group == group);
      CHECK(r.lanes == lanes);
      CHECK(r.laneMask == (lanes == 8 ? 0x5Au : 1u));
      for (uint32_t s = 0; s < pq::kVoiceTapSamples; ++s)
        for (uint32_t l = 0; l < lanes; ++l) {
          const float p = phase[(s * 8) * lanes + l];
          const float want = p - std::floor(p);
          exact = exact && std::memcmp(&want, &r.sample[s][l], 4) == 0;
        }
      for (uint32_t l = 0; l < lanes; ++l) {
        exact = exact && r.position[l] == pos[63 * lanes + l];
        exact = exact && r.increment[l] == k.st().prevDelta[l];
      }
    }
    CHECK(exact);
    tap.removeReader();
  }
}

TEST_CASE("voice_tap: samples x per lane, the last difference as the increment; no resource: nothing") {
  pq::VoiceTapConfig tc;
  tc.groups = 2;
  tc.stride = 4;
  auto tap = std::make_shared<pq::VoiceTap>(tc);
  tap->addReader();
  for (const uint32_t lanes : {1u, 8u})
    for (const uint32_t mask : {0u, 1u}) {
      CAPTURE(lanes);
      CAPTURE(mask);
      const KernelFn fn = findKernel(Atom::VoiceTap, lanes, mask);
      REQUIRE(fn != nullptr);
      AlignedBuffer<float> state(16);
      StateInit si;
      si.lanes = lanes;
      si.voiceGroup = 1;
      initState(Atom::VoiceTap, state.data(), si);
      std::vector<float> x(32 * lanes);
      for (size_t i = 0; i < x.size(); ++i) x[i] = 0.01f * static_cast<float>(i) + 2.0f;
      const float* in[1] = {x.data()};
      const uint64_t w0 = tap->written(1);
      KernelArgs a{in, nullptr, state.data(), tap.get(), 32, 1, 0, mask};
      fn(a);
      REQUIRE(tap->written(1) == w0 + 1);  // 32 frames / stride 4 = 8 samples
      pq::VoiceTapRecord r;
      REQUIRE(tap->read(1, w0, r));
      bool ok = true;
      for (uint32_t s = 0; s < 8; ++s)
        for (uint32_t l = 0; l < lanes; ++l) ok = ok && r.sample[s][l] == (mask ? x[l] : x[(s * 4) * lanes + l]);  // raw
      for (uint32_t l = 0; l < lanes; ++l) {
        const float want = mask ? x[l] - 0.0f : x[31 * lanes + l] - x[30 * lanes + l];
        ok = ok && r.increment[l] == want && r.position[l] == 0.0f;
      }
      CHECK(ok);
      KernelArgs none{in, nullptr, state.data(), nullptr, 32, 1, 0, mask};
      fn(none);  // no tap resource: nothing happens
      CHECK(tap->written(1) == w0 + 1);
    }
  tap->removeReader();
}

TEST_CASE("wavetable_pos / voice_tap: kernel calls with swaps, crossfades and tap writes allocate nothing") {
  REQUIRE(pq::alloc_trap::installed());
  const TablePtr a = drawn(), b = sawSquare();
  pq::EpochReclaimer rec;
  FrameTableSlot slot(a);
  slot.tap()->addReader();
  pq::VoiceTapConfig tc;
  pq::VoiceTap tap(tc);
  tap.addReader();
  std::vector<Wp> ks;
  for (const uint32_t lanes : {1u, 8u})
    for (const uint32_t mask : {0u, 1u, 2u, 3u}) ks.emplace_back(lanes, mask);
  std::vector<float> phase(200 * 8), pos(200 * 8), out(200 * 8);
  for (size_t i = 0; i < phase.size(); ++i) phase[i] = std::fmod(0.0123f * static_cast<float>(i), 1.0f);
  for (size_t i = 0; i < pos.size(); ++i) pos[i] = std::fmod(0.37f * static_cast<float>(i), 1.0f);
  AlignedBuffer<float> vts(16);
  initState(Atom::VoiceTap, vts.data(), {});
  uint64_t violations = 0;
  for (uint32_t round = 0; round < 6; ++round) {
    slot.publish(round % 2 ? a : b, rec);  // outside the scope (it allocates by design)
    violations += pq::alloc_trap::countRtViolations([&] {
      for (Wp& k : ks)
        for (const uint32_t n : {1u, 7u, 64u, 200u}) k.run(slot, phase.data(), pos.data(), n, out.data());
      const float* in[1] = {phase.data()};
      for (const uint32_t lanes : {1u, 8u}) {
        KernelArgs va{in, nullptr, vts.data(), &tap, 200, 1, 0, 0};
        findKernel(Atom::VoiceTap, lanes, 0)(va);
      }
    });
    rec.collect();
  }
  CHECK(violations == 0);
  CHECK(slot.tap()->written(0) > 0);
  slot.tap()->removeReader();
}

// ---- Two-thread stress: random swaps while the "audio thread" reads, retired entries and tables poisoned ----------

namespace {

// Retired entries are not freed but poisoned and quarantined: a kernel that read one after its retirement would see
// generation ~0 and a table of NaNs.
struct Poison {
  std::mutex m;
  std::vector<FrameTableSlot::Entry*> quarantine;
  std::shared_ptr<const FrameTable> nanTable;
  std::atomic<uint64_t> poisoned{0};
};
Poison* gPoison = nullptr;

void poisonEntry(void* p) noexcept {
  auto* e = static_cast<FrameTableSlot::Entry*>(p);
  e->table.reset();  // a table whose last entry went: its own deleter poisons it (fresh-table run)
  e->view = gPoison->nanTable->view();
  e->prev.store(e);
  e->generation = ~uint64_t{0};
  gPoison->poisoned.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(gPoison->m);
  gPoison->quarantine.push_back(e);
}

void stress(uint32_t swaps, bool freshTables) {
  Poison poison;
  gPoison = &poison;
  {
    FrameTableBuilder b;
    ftfix::SpecHolder s({ftfix::basic(BasicCycle::Sine)}, 96000, 1);
    auto t = b.build(s.spec);
    REQUIRE(t.ok());
    FrameTable nan = std::move(t.value());
    nan.setSine(false);  // TMM.12-fix1: a sine table's readers would not read the poisoned rows
    const FrameTableView& v = nan.view();
    std::fill(const_cast<float*>(v.data), const_cast<float*>(v.data) + static_cast<size_t>(v.frameStride) * v.frames,
              std::numeric_limits<float>::quiet_NaN());
    poison.nanTable = std::make_shared<const FrameTable>(std::move(nan));
  }
  // Long-lived tables (entry poisoning), or a fresh table per swap whose deleter poisons it (table poisoning).
  const TablePtr base[2] = {makeTable({ftfix::basic(BasicCycle::Saw)}, 96000, 1),
                            makeTable({ftfix::basic(BasicCycle::Square)}, 96000, 1)};
  std::mutex tablesM;
  std::vector<std::unique_ptr<FrameTable>> deadTables;  // poisoned, kept until the end
  FrameTableBuilder builder;
  ftfix::SpecHolder spec({ftfix::basic(BasicCycle::Triangle)}, 96000, 1);
  auto fresh = [&]() -> TablePtr {
    auto t = builder.build(spec.spec);
    REQUIRE(t.ok());
    return TablePtr(new FrameTable(std::move(t.value())), [&](const FrameTable* p) {
      const FrameTableView& v = p->view();
      std::fill(const_cast<float*>(v.data), const_cast<float*>(v.data) + static_cast<size_t>(v.frameStride) * v.frames,
                std::numeric_limits<float>::quiet_NaN());
      std::lock_guard lock(tablesM);
      deadTables.emplace_back(const_cast<FrameTable*>(p));
    });
  };
  pq::EpochReclaimer rec(1);
  auto reader = rec.registerReader();
  REQUIRE(reader.has_value());
  uint64_t blocks = 0, bad = 0;
  {
    FrameTableSlot slot(base[0]);
    slot.setEntryDeleter(&poisonEntry);
    std::atomic<bool> stop{false};
    std::thread audio([&] {
      Wp k1(1, 0), k8(8, 2);
      std::vector<float> phase(16 * 8), pos(16 * 8, 0.0f), out(16 * 8);
      float acc = 0;
      while (!stop.load(std::memory_order_acquire)) {
        for (float& p : phase) {
          p = acc;
          acc += 0.0137f;
          if (acc >= 1.0f) acc -= 1.0f;
        }
        k1.run(slot, phase.data(), pos.data(), 16, out.data());
        for (uint32_t i = 0; i < 16; ++i) bad += std::isfinite(out[i]) ? 0 : 1;
        k8.run(slot, phase.data(), pos.data(), 16, out.data());
        for (float v : out) bad += std::isfinite(v) ? 0 : 1;
        bad += k1.st().generation == ~uint64_t{0} || k8.st().generation == ~uint64_t{0} ? 1 : 0;
        rec.quiescent(*reader);
        ++blocks;
      }
    });
    std::mt19937 rng(17);
    for (uint32_t i = 0; i < swaps; ++i) {
      slot.publish(freshTables ? fresh() : base[rng() & 1u], rec);
      if ((i & 63u) == 0) rec.collect();
      if ((rng() & 7u) == 0) std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    audio.join();
    rec.unregisterReader(*reader);
    rec.collect();
  }
  const std::string what = std::to_string(swaps) + (freshTables ? " swaps (fresh tables), " : " swaps, ") +
                           std::to_string(blocks) + " audio blocks, " + std::to_string(poison.poisoned.load()) +
                           " entries retired and poisoned";
  MESSAGE(what);
  CHECK(bad == 0);
  CHECK(blocks > 0);
  // Every entry went through a deleter (AU.fold: the reclaimer's, or the slot's own for those still on its chain).
  CHECK(poison.poisoned.load() >= swaps);
  REQUIRE(rec.pending() == 0);
  for (FrameTableSlot::Entry* e : poison.quarantine) delete e;
  gPoison = nullptr;
}

}  // namespace

TEST_CASE("frame-table slot: 20 000 random swaps against a reading thread, no use of a retired entry") { stress(20000, false); }
TEST_CASE("frame-table slot: 300 swaps of fresh tables, freed tables poisoned, never read") { stress(300, true); }
TEST_CASE("frame-table slot: 10^6 random swaps against a reading thread" * doctest::test_suite("slow")) { stress(1000000, false); }
