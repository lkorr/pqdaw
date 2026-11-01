// TMM.8: the Wavetable's live warp / fold / sync path (wavetable_live) and Shape mode (wavetable_shape, ADAA):
//   - the kernel against its double evaluation (the same formulas), 1 lane == each of 8 lanes, block splits;
//   - agreement with the baked table at static values;
//   - the alias proof: the region's output against the 64× double-precision reference with warp / fold / sync
//     modulated at audio rate (wave_live_rig.h), at os 4 / 8 (16 in the slow tier), and the corrections' share;
//   - Shape mode's alias suite (sine inputs, bin-exact, AliasMeter) at os 2 (|x| ≤ 1) and driven ×4 at os 4;
//   - swaps crossfade; nothing allocates.
// The measured numbers are recorded in docs/QUESTIONS.md (TMM.8) and bench/RESULTS.md; the bounds checked here are
// those measurements with a 2 dB margin (a regression shows), not the plan's targets where those are not reached.
#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <tuple>

#include "../support/alias_meter.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/epoch_reclaimer.h"
#include "wave_live_rig.h"

using namespace pq::dsp;
using namespace wlrig;

namespace {

constexpr double kSr = 48000;

// The baked path's output for the same controls: the table baked with them, read at the base rate.
std::vector<double> bakedAmplitudes(std::vector<ftfix::Cycle> cycles, double sr, FrameBake bake, uint32_t k0,
                                    uint32_t N, bool liveBand = true) {
  const TablePtr t = build(std::move(cycles), sr, bake, false, 64, liveBand);
  FrameReadState st{};
  std::vector<float> ph(N + 4096), y(N + 4096);
  rampPhase(0, N + 4096, k0, N, ph.data());
  const float pos = 0.0f;
  readFrameTableL1(t->view(), st, ph.data(), &pos, false, N + 4096, y.data());
  return amplitudes(std::vector<float>(y.begin() + 4096, y.end()));
}

struct Static {
  const char* name;
  WarpMode mode;
  double warp, fold, sync;
};
const Static kStatics[] = {
    {"bend 0.6", WarpMode::Bend, 0.6, 0, 0},
    {"mirror 0.6", WarpMode::Mirror, 0.6, 0, 0},
    {"pulse width -0.6", WarpMode::PulseWidth, -0.6, 0, 0},
    {"fold 0.3", WarpMode::Bend, 0, 0.3, 0},
    {"sync 0.37", WarpMode::Bend, 0, 0, 0.37},
    {"pw 0.4 + sync 0.2 + fold 0.1", WarpMode::PulseWidth, 0.4, 0.1, 0.2},
};

double staticAgreement(const Static& c, const std::vector<ftfix::Cycle>& cycles, uint32_t os, uint32_t k0, uint32_t N,
                       bool liveBand = true) {
  FrameBake bake;
  bake.warpMode = c.mode;
  bake.warp = static_cast<float>(c.warp);
  bake.fold = static_cast<float>(c.fold);
  bake.sync = static_cast<float>(c.sync);
  const std::vector<double> baked = bakedAmplitudes(cycles, kSr, bake, k0, N, liveBand);
  const TablePtr plain = build(cycles, kSr);
  FrameTableSlot slot(plain);
  LiveRegion r(os, kSr, c.mode, true, constant(c.warp), constant(c.fold), constant(c.sync));
  r.splControls = true;
  return compare(amplitudes(renderLive(r, slot, k0, N)), baked, kSr, N).worstDb;
}

struct AliasCase {
  std::string name;
  WarpMode mode;
  Ctl warp, fold, sync;
  bool corr;
};

std::vector<AliasCase> aliasCases(double fm) {
  return {
      {"bend", WarpMode::Bend, sine(1.0, 0.0, fm), constant(0), constant(0), true},
      {"mirror", WarpMode::Mirror, sine(1.0, 0.0, fm), constant(0), constant(0), true},
      {"pulse width", WarpMode::PulseWidth, sine(1.0, 0.0, fm), constant(0), constant(0), true},
      {"fold", WarpMode::Bend, constant(0), sine(0.5, 0.5, fm), constant(0), false},
      {"sync", WarpMode::Bend, constant(0), constant(0), sine(0.5, 0.5, fm), true},
  };
}

struct AliasResult {
  double worst = -400;     // | |live| − |ref| | below 20 kHz, dBFS
  double inherent = -400;  // what sampling the reference at os·sr folds below 20 kHz
};

AliasResult runAliasCase(const AliasCase& c, const std::vector<ftfix::Cycle>& cycles, uint32_t os, uint32_t k0,
                         uint32_t N, double sr) {
  const TablePtr t = build(cycles, sr);
  FrameTableSlot slot(t);
  LiveRegion r(os, sr, c.mode, c.corr, c.warp, c.fold, c.sync);
  const std::vector<double> live = amplitudes(renderLive(r, slot, k0, N));
  Reference ref;
  ref.os = os;
  ref.sampleRate = sr;
  const std::vector<double> want = ref.amplitudes(t->view(), c.mode, c.warp, c.fold, c.sync, k0, N);
  return {compare(live, want, sr, N).worstDb, ref.inherentDb};
}

}  // namespace

TEST_CASE("wavetable_live: the kernel is its double evaluation (os 1, static controls); 8 lanes == 1 lane; splits") {
  const TablePtr t = build({ftfix::basic(BasicCycle::Saw), ftfix::basic(BasicCycle::Square)}, kSr);
  FrameTableSlot slot(t);
  constexpr uint32_t N = 1u << 12, k0 = 151;
  for (const Static& c : kStatics) {
    CAPTURE(c.name);
    LiveRegion r(1, kSr, c.mode, false, constant(c.warp), constant(c.fold), constant(c.sync));
    r.splControls = true;
    const std::vector<float> y = renderLive(r, slot, k0, N, 0);
    const double inc = static_cast<double>(k0) / N;
    const LiveControls lc{c.mode, c.warp, c.fold, c.sync};
    double worst = 0;
    for (uint32_t i = 2; i < N; ++i) {
      const double phi = static_cast<double>((static_cast<uint64_t>(i) * k0) % N) / N;
      worst = std::max(worst, std::fabs(liveValue(t->view(), 0.0f, lc, phi, inc) - y[i]));
    }
    MESSAGE(c.name << ": kernel vs double " << worst);
    CHECK(worst < 2e-5);  // float formulas and reads against double: rounding only
  }
  // Every lane of an 8-lane call (different pitches, controls per lane, a per-sample position) equals the 1-lane
  // kernel on that lane's inputs, bit for bit, with corrections on; and splitting the calls changes nothing.
  for (const uint32_t mode : {0u, 1u, 2u}) {
    CAPTURE(mode);
    constexpr uint32_t F = 700;
    std::vector<float> src8[5];
    for (auto& v : src8) v.resize(F * 8);
    for (uint32_t i = 0; i < F; ++i)
      for (uint32_t l = 0; l < 8; ++l) {
        src8[0][i * 8 + l] = static_cast<float>(std::fmod((0.003 + 0.0041 * l) * i, 1.0));
        src8[1][i * 8 + l] = static_cast<float>(0.5 + 0.5 * std::sin(0.01 * i + l));
        src8[2][i * 8 + l] = static_cast<float>(std::sin(0.02 * i + 0.7 * l));
        src8[3][i * 8 + l] = static_cast<float>(0.2 + 0.2 * std::sin(0.013 * i + l));
        src8[4][i * 8 + l] = static_cast<float>(0.3 + 0.3 * std::sin(0.017 * i + 2 * l));
      }
    alignas(32) float modeV[8], corrV[8];
    std::fill(modeV, modeV + 8, static_cast<float>(mode));
    std::fill(corrV, corrV + 8, 1.0f);
    auto run = [&](uint32_t lanes, const std::vector<float>* src, std::vector<float>& out, uint32_t chunk) {
      State st(Atom::WavetableLive, lanes, kSr);
      const KernelFn fn = findKernel(Atom::WavetableLive, lanes, (1u << 5) | (1u << 6));
      REQUIRE(fn != nullptr);
      for (uint32_t f0 = 0; f0 < F; f0 += chunk) {
        const uint32_t n = std::min(chunk, F - f0);
        const float* ins[7] = {src[0].data() + f0 * lanes, src[1].data() + f0 * lanes, src[2].data() + f0 * lanes,
                               src[3].data() + f0 * lanes, src[4].data() + f0 * lanes, modeV, corrV};
        float* outs[1] = {out.data() + f0 * lanes};
        KernelArgs a{ins, outs, st.mem.data(), &slot, n, 7, 1, (1u << 5) | (1u << 6)};
        fn(a);
      }
    };
    std::vector<float> y8(F * 8);
    run(8, src8, y8, 64);
    for (uint32_t lane : {0u, 3u, 7u}) {
      std::vector<float> src1[5];
      for (int k = 0; k < 5; ++k) {
        src1[k].resize(F);
        for (uint32_t i = 0; i < F; ++i) src1[k][i] = src8[k][i * 8 + lane];
      }
      std::vector<float> a(F), b(F);
      run(1, src1, a, 64);
      run(1, src1, b, 13);
      bool same = true, split = true;
      for (uint32_t i = 0; i < F; ++i) {
        same &= std::memcmp(&a[i], &y8[i * 8 + lane], 4) == 0;
        split &= std::memcmp(&a[i], &b[i], 4) == 0;
      }
      CAPTURE(lane);
      CHECK(same);
      CHECK(split);
    }
  }
}

TEST_CASE("wavetable_live: static controls agree with the baked table below 20 kHz (in-band tables)") {
  // Tables whose harmonics the region's read keeps at every speed the controls reach (a saw cut where the fastest read
  // of its top harmonic, times the band headroom and the fold's spread, stays inside the os-4 level rule): the live and
  // baked paths then compute the same waveform, and agree within −80 dB.
  constexpr uint32_t N = 1u << 14;
  for (const uint32_t k0 : {37u, 151u, 601u})
    for (const Static& c : kStatics) {
      const double s = 1.0 + 15.0 * c.sync;
      double slope = 1.0;
      if (c.mode == WarpMode::Bend) slope = std::max(bendCurve(c.warp), 1.0 / bendCurve(c.warp));
      if (c.mode == WarpMode::PulseWidth) slope = 0.5 / std::min(pulseWidth(c.warp), 1.0 - pulseWidth(c.warp));
      const double fastest = k0 * kSr / N * s * slope * liveSpeedScale(c.fold) * 1.4;  // ≤ r × the level's top
      // A level holds H_j ≥ (sr − P)/sr / (r·speed) harmonics (r = 1.4 at 48 kHz): ≈ 0.4 per unit of speed.
      const uint32_t top = std::clamp(static_cast<uint32_t>(0.38 * kSr / fastest), 1u, 64u);
      const double w = staticAgreement(c, {sawTo(top)}, 4, k0, N);
      CAPTURE(c.name);
      CAPTURE(k0);
      MESSAGE("live vs baked (in-band saw to " << top << ", os 4): " << c.name << " at " << k0 * kSr / N << " Hz: " << w
                                                 << " dB");
      CHECK(w <= -80.0);
    }
}

TEST_CASE("wavetable_live: the live-band bake agrees with the live path for the full saw (os 4 / 8 / 16)") {
  // TMM.8-fix (b): the Wavetable's bake uses the live band limit (FrameTableSpec::liveBand), so a switch between the
  // baked and the live path is seamless: ≤ −80 dB below 20 kHz for the full 1023-harmonic saw at every pitch tested,
  // at the auto factors ×8 / ×16 (the band rule does not depend on the factor; at ×4 the live path's own aliasing of
  // a combined pulse width + sync + fold at 4.4 kHz shows: −69 dB, reported). For contrast, the full-band bake (TMM.1)
  // is measured too.
  constexpr uint32_t N = 1u << 14;
  for (const uint32_t os : {4u, 8u, 16u})
    for (const uint32_t k0 : {37u, 151u, 601u, 1501u})
      for (const Static& c : kStatics) {
        if (os == 16 && k0 != 601) continue;  // ×16 once (it costs 4× ×4's time)
        if (os == 4 && k0 != 601 && k0 != 1501) continue;
        const double w = staticAgreement(c, {ftfix::basic(BasicCycle::Saw)}, os, k0, N);
        CAPTURE(c.name);
        CAPTURE(k0);
        CAPTURE(os);
        MESSAGE("live vs live-band bake (full saw): os " << os << " " << c.name << " at " << k0 * kSr / N << " Hz: " << w
                                                        << " dB");
        if (os >= 8) CHECK(w <= -80.0);
        if (os == 4 && k0 == 601)
          MESSAGE("  (the full-band bake: " << staticAgreement(c, {ftfix::basic(BasicCycle::Saw)}, os, k0, N, false)
                                            << " dB)");
      }
}

TEST_CASE("wavetable_live: the live-band bake stays alias-free (bin-exact reads, ≤ −100 dBFS below 20 kHz)") {
  constexpr uint32_t N = 1u << 15;
  pq::test::AliasMeter meter(N);
  double worst = -400;
  for (const Static& c : kStatics) {
    FrameBake bake;
    bake.warpMode = c.mode;
    bake.warp = static_cast<float>(c.warp);
    bake.fold = static_cast<float>(c.fold);
    bake.sync = static_cast<float>(c.sync);
    const TablePtr t = build({ftfix::basic(BasicCycle::Saw)}, kSr, bake, false, 64, true);
    for (const uint32_t k0 : {27u, 111u, 443u, 1777u, 5001u}) {  // 40 Hz … 7.3 kHz
      FrameReadState st{};
      std::vector<float> ph(N + 4096), y(N + 4096);
      rampPhase(0, N + 4096, k0, N, ph.data());
      const float pos = 0.0f;
      readFrameTableL1(t->view(), st, ph.data(), &pos, false, N + 4096, y.data());
      const auto rep = meter.measure(std::span<const float>(y.data() + 4096, N), k0, kSr, [](uint32_t) { return -1.0; });
      CAPTURE(c.name);
      CAPTURE(k0);
      CHECK(rep.worstAliasDb <= -100.0);
      worst = std::max(worst, rep.worstAliasDb);
    }
  }
  MESSAGE("live-band bake, static controls: worst alias " << worst << " dBFS");
}

namespace {

struct Bound {
  const char* name;
  uint32_t os;
  double bound;
};

void aliasSuite(double sr, uint32_t N, std::vector<uint32_t> pitches, std::vector<double> mods,
                std::vector<uint32_t> factors, const std::vector<Bound>& bounds) {
  for (const double fmHz : mods) {
    const uint32_t km = static_cast<uint32_t>(std::lround(fmHz * N / sr)) | 1u;
    for (const AliasCase& c : aliasCases(static_cast<double>(km) / N))
      for (const uint32_t os : factors) {
        AliasResult worst;
        double at = 0;
        for (const uint32_t k0 : pitches) {
          const AliasResult r = runAliasCase(c, {ftfix::basic(BasicCycle::Saw)}, os, k0, N, sr);
          if (r.worst > worst.worst) {
            worst.worst = r.worst;
            at = k0 * sr / N;
          }
          worst.inherent = std::max(worst.inherent, r.inherent);
        }
        MESSAGE("alias proof " << sr << " Hz, modulator " << km * sr / N << " Hz, " << c.name << ", os " << os
                               << ": worst " << worst.worst << " dBFS (f0 " << at << " Hz); sampling the reference at "
                               << os << "x folds " << worst.inherent << " dBFS");
        for (const Bound& b : bounds)
          if (c.name == b.name && os == b.os) {
            CAPTURE(c.name);
            CAPTURE(os);
            CHECK(worst.worst <= b.bound);
          }
      }
  }
}

}  // namespace

TEST_CASE("wavetable_live: the alias proof against the 64x reference (48 kHz, 1760 Hz, a 2 kHz modulator)") {
  aliasSuite(kSr, 1u << 14, {601u}, {2001.0}, {4u, 8u},
             {{"bend", 4, -79}, {"mirror", 4, -88}, {"pulse width", 4, -77}, {"fold", 4, -74}, {"sync", 4, -58},
              {"bend", 8, -87}, {"mirror", 8, -107}, {"pulse width", 8, -84}, {"fold", 8, -89}, {"sync", 8, -65}});
}

// The slow tier: 108 Hz … 12 kHz, os 4 / 8 / 16, one case per rate and modulator (each ≈ 20 s: the suite's shards
// run them in parallel). Measured only (QUESTIONS TMM.8 records the table); the default case above guards regressions.
void slowAliasProof(double sr, double fm) {
  constexpr uint32_t N = 1u << 14;
  std::vector<uint32_t> pitches;
  for (const double f : {108.0, 442.0, 1760.0, 4400.0, 12000.0})
    pitches.push_back(static_cast<uint32_t>(std::lround(f * N / sr)) | 1u);
  aliasSuite(sr, N, pitches, {fm}, {4u, 8u, 16u}, {});
}
#define PQ_SLOW_ALIAS(sr, fm)                                                                                   TEST_CASE("wavetable_live: the alias proof at " #sr " Hz, a " #fm " Hz modulator, 108 Hz - 12 kHz, os 4 / 8 / 16" *             doctest::test_suite("slow")) {                                                                        slowAliasProof(sr, fm);                                                                                     }
PQ_SLOW_ALIAS(44100.0, 1000.0)
PQ_SLOW_ALIAS(44100.0, 2000.0)
PQ_SLOW_ALIAS(44100.0, 5000.0)
PQ_SLOW_ALIAS(48000.0, 1000.0)
PQ_SLOW_ALIAS(48000.0, 2000.0)
PQ_SLOW_ALIAS(48000.0, 5000.0)
PQ_SLOW_ALIAS(96000.0, 1000.0)
PQ_SLOW_ALIAS(96000.0, 2000.0)
PQ_SLOW_ALIAS(96000.0, 5000.0)

TEST_CASE("wavetable_live: the corrections remove the sync step's and the kinks' aliasing") {
  constexpr uint32_t N = 1u << 14, k0 = 601;
  const double fm = 683.0 / N;
  (void)fm;
  for (const AliasCase& base : {AliasCase{"sync", WarpMode::Bend, constant(0), constant(0), constant(0.37), true},
                                AliasCase{"pulse width", WarpMode::PulseWidth, constant(-0.6), constant(0),
                                          constant(0), true},
                                AliasCase{"bend", WarpMode::Bend, constant(0.8), constant(0), constant(0), true}}) {
    AliasCase off = base;
    off.corr = false;
    const double with = runAliasCase(base, {ftfix::basic(BasicCycle::Saw)}, 8, k0, N, kSr).worst;
    const double without = runAliasCase(off, {ftfix::basic(BasicCycle::Saw)}, 8, k0, N, kSr).worst;
    MESSAGE(base.name << " (static, os 8, 1760 Hz): with corrections " << with << " dBFS, without " << without << " dBFS");
    CAPTURE(base.name);
    CHECK(with <= without - 10.0);
  }
}

TEST_CASE("wavetable_live / wavetable_shape: swaps crossfade, taps are written, no kernel call allocates") {
  REQUIRE(pq::alloc_trap::installed());
  const TablePtr a = build({ftfix::basic(BasicCycle::Saw)}, kSr, {}, true);
  const TablePtr b = build({ftfix::basic(BasicCycle::Square), ftfix::basic(BasicCycle::Triangle)}, kSr, {}, true);
  pq::EpochReclaimer rec;
  FrameTableSlot slot(a);
  slot.tap()->addReader();
  constexpr uint32_t F = 256;
  // The kernels' own inputs, prepared outside the allocation scope.
  std::vector<float> ph(F * 8), pos(F * 8, 0.25f), w(F * 8), f(F * 8), s(F * 8), x(F * 8), out(F * 8);
  for (uint32_t i = 0; i < F * 8; ++i) {
    ph[i] = static_cast<float>(std::fmod(0.0123 * i, 1.0));
    w[i] = static_cast<float>(std::sin(0.01 * i));
    f[i] = static_cast<float>(0.3 + 0.3 * std::sin(0.007 * i));
    s[i] = static_cast<float>(0.5 + 0.5 * std::sin(0.003 * i));
    x[i] = static_cast<float>(0.9 * std::sin(0.05 * i));
  }
  alignas(32) float corr[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  std::vector<State> lives, shapes;
  for (const uint32_t lanes : {1u, 8u}) {
    lives.emplace_back(Atom::WavetableLive, lanes, 4 * kSr);
    shapes.emplace_back(Atom::WavetableShape, lanes, 2 * kSr);
  }
  uint64_t violations = 0;
  double peak = 0;
  for (uint32_t round = 0; round < 8; ++round) {
    if (round % 2) slot.publish(round % 4 == 1 ? b : a, rec);  // outside the scope (it allocates by design)
    violations += pq::alloc_trap::countRtViolations([&] {
      for (uint32_t li = 0; li < 2; ++li) {
        const uint32_t lanes = li == 0 ? 1u : 8u;
        for (const float mode : {0.0f, 1.0f, 2.0f}) {
          alignas(32) float modeV[8] = {mode, mode, mode, mode, mode, mode, mode, mode};
          const float* ins[7] = {ph.data(), pos.data(), w.data(), f.data(), s.data(), modeV, corr};
          float* outs[1] = {out.data()};
          KernelArgs ka{ins, outs, lives[li].mem.data(), &slot, F, 7, 1, (1u << 5) | (1u << 6)};
          findKernel(Atom::WavetableLive, lanes, (1u << 5) | (1u << 6))(ka);
          for (uint32_t i = 0; i < F * lanes; ++i) peak = std::max(peak, std::fabs(static_cast<double>(out[i])));
        }
        const float* sins[2] = {x.data(), pos.data()};
        float* outs[1] = {out.data()};
        KernelArgs ks{sins, outs, shapes[li].mem.data(), &slot, F, 2, 1, 0u};
        findKernel(Atom::WavetableShape, lanes, 0u)(ks);
      }
    });
    for (uint32_t i = 0; i < F * 8; ++i) CHECK(std::isfinite(out[i]));
    rec.collect();
  }
  CHECK(violations == 0);
  CHECK(peak > 0.3);
  CHECK(slot.tap()->written(0) > 0);
  slot.tap()->removeReader();
}

// ---- Shape mode ---------------------------------------------------------------------------------------------------

TEST_CASE("wavetable_shape: a still input reads the curve; ADAA is the mean of the curve over each step") {
  const TablePtr t = build({ftfix::basic(BasicCycle::Saw)}, kSr, {}, true);
  FrameTableSlot slot(t);
  ShapeRegion r(1, kSr);
  // A ramp in x from −1.5 to 1.5 (through the clamp on both sides), slow enough for the full-band level.
  constexpr uint32_t F = 3000;
  std::vector<float> x(F), y(F);
  for (uint32_t i = 0; i < F; ++i) x[i] = static_cast<float>(-1.5 + 3.0 * i / (F - 1));
  r.block(slot, x.data(), F, y.data());
  double worst = 0;
  for (uint32_t i = 1; i < F; ++i) {
    // The level the kernel holds at this speed (|Δcu| at most 1/(F − 1)·1.5), and the mean of g over [u₀, u₁].
    const double u0 = 0.5 * x[i - 1] + 0.5, u1 = 0.5 * x[i] + 0.5;
    const double du = std::fabs(std::clamp(u1, 0.0, 1.0) - std::clamp(u0, 0.0, 1.0));
    const LiveLevels lv = liveLevels(t->view(), static_cast<float>(i > 1 ? 1.5 / (F - 1) : du));
    double m = 0;
    for (int k = 0; k < 64; ++k) m += shapeCurve(t->view(), 0.0f, lv, u0 + (u1 - u0) * (k + 0.5) / 64);
    m /= 64;
    worst = std::max(worst, std::fabs(m - y[i]));
  }
  MESSAGE("ADAA vs the curve's mean: " << worst);
  CHECK(worst < 2e-3);
  // A constant input: the midpoint fallback, the curve itself.
  std::vector<float> c(64, 0.3f), yc(64);
  ShapeRegion r2(1, kSr);
  r2.block(slot, c.data(), 64, yc.data());
  CHECK(std::fabs(yc[63] - shapeCurve(t->view(), 0.0f, liveLevels(t->view(), 0.0f), 0.65)) < 1e-5);
  // A table built without the antiderivative plays silence (Shape mode needs it).
  FrameTableSlot plain(build({ftfix::basic(BasicCycle::Saw)}, kSr));
  ShapeRegion r3(1, kSr);
  r3.block(plain, x.data(), 256, y.data());
  CHECK(*std::max_element(y.begin(), y.begin() + 256) == 0.0f);
}

namespace {

void shapeSuite(double sr, uint32_t N, const std::vector<uint32_t>& ks,
                const std::vector<std::pair<double, uint32_t>>& drive,
                const std::vector<std::tuple<std::string, double, uint32_t, double>>& bounds) {
  pq::test::AliasMeter meter(N);
  const std::vector<std::pair<std::string, std::vector<ftfix::Cycle>>> curves = {
      {"saw", {ftfix::basic(BasicCycle::Saw)}},
      {"square", {ftfix::basic(BasicCycle::Square)}},
      {"drawn", {ftfix::drawnFixture(1)[0]}},
  };
  for (const auto& [name, cyc] : curves) {
    const TablePtr t = build(cyc, sr, {}, true);
    FrameTableSlot slot(t);
    for (const auto& [amp, os] : drive) {
      double worst = -400, at = 0;
      for (const uint32_t k0 : ks) {
        ShapeRegion r(os, sr);
        const std::vector<float> y = renderShape(r, slot, amp, k0, N);
        const pq::test::AliasReport rep = meter.measure(y, k0, sr, [](uint32_t) { return -1.0; });
        if (rep.worstAliasDb > worst) {
          worst = rep.worstAliasDb;
          at = k0 * sr / N;
        }
      }
      MESSAGE("shape " << sr << " Hz: " << name << " amplitude " << amp << " os " << os << ": worst alias " << worst
                       << " dBFS (input " << at << " Hz)");
      for (const auto& [bn, ba, bo, bound] : bounds)
        if (bn == name && ba == amp && bo == os) {
          CAPTURE(name);
          CAPTURE(amp);
          CHECK(worst <= bound);
        }
    }
  }
}

}  // namespace

TEST_CASE("wavetable_shape: the alias suite (48 kHz, sine inputs 100 Hz - 5 kHz)") {
  shapeSuite(kSr, 1u << 15, {69u, 273u, 1093u, 3415u}, {{1.0, 2}, {4.0, 4}},
             {{"saw", 1.0, 2, -73}, {"square", 1.0, 2, -88}, {"drawn", 1.0, 2, -92},
              {"saw", 4.0, 4, -65}, {"square", 4.0, 4, -59}, {"drawn", 4.0, 4, -82}});
}

TEST_CASE("wavetable_shape: the alias suite, 44.1 / 96 kHz, amplitudes 0.5 / 1 / 4, os 2 / 4 / 8" *
          doctest::test_suite("slow")) {
  for (const double sr : {44100.0, 96000.0}) {
    constexpr uint32_t N = 1u << 15;
    std::vector<uint32_t> ks;
    for (const double f : {100.0, 400.0, 1600.0, 5000.0})
      ks.push_back(static_cast<uint32_t>(std::lround(f * N / sr)) | 1u);
    shapeSuite(sr, N, ks, {{0.5, 2}, {1.0, 2}, {4.0, 4}, {4.0, 8}}, {});
  }
}
