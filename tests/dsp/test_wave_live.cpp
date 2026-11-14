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
#include <functional>
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

TEST_CASE("wavetable_live (TMM.18): a backward phase (negative frequency) is the time-reversed output, corrections included") {
  // Static controls: φ = frac(−k0·n / N) is the forward render mirrored in time (the region's sub-sample grid, the
  // phase interpolation, the linear-phase residuals and the level rule are all symmetric; the half-bands are LTI), so
  // the two spectra's magnitudes agree bin for bin. Audio-rate modulated controls (their time reversed too: a sine at
  // −fm): the kernel's control deltas are backward differences, which time reversal turns into forward ones, so the
  // renders are not exact mirrors; there the backward render's alias level against the (reversal-invariant) spectrum
  // of the 64x reference must stay at the forward one's: within 2 dB at a 120 Hz modulator, within 6 dB at 2 kHz
  // (measured: sync −69.3 vs −69.8 dBFS; pulse width −89.7 vs −95.1, the one residual difference, QUESTIONS
  // Q?-TMM.18-a). Before TMM.18 the corrections assumed a forward phase (a backward wrap was no event, a backward kink
  // no crossing): static backward renders differed from the forward ones by −73.7 … −86.8 dB, and the modulated
  // backward renders aliased at −65.5 / −80.4 dBFS (120 Hz) and −35.2 / −50.9 dBFS (2 kHz).
  constexpr uint32_t N = 1u << 14, k0 = 601;
  const TablePtr t = build({ftfix::basic(BasicCycle::Saw)}, kSr);
  struct Static {
    const char* name;
    WarpMode mode;
    double warp, fold, sync;
  };
  const Static statics[] = {{"sync 0.37", WarpMode::Bend, 0, 0, 0.37},
                            {"bend 0.8", WarpMode::Bend, 0.8, 0, 0},
                            {"pulse width -0.6", WarpMode::PulseWidth, -0.6, 0, 0},
                            {"mirror 0.6", WarpMode::Mirror, 0.6, 0, 0},
                            {"pw 0.4 + sync 0.2 + fold 0.1", WarpMode::PulseWidth, 0.4, 0.1, 0.2}};
  for (const Static& c : statics)
    for (const bool corr : {true, false}) {
      FrameTableSlot slot(t);
      LiveRegion fwd(8, kSr, c.mode, corr, constant(c.warp), constant(c.fold), constant(c.sync));
      LiveRegion bwd(8, kSr, c.mode, corr, constant(c.warp), constant(c.fold), constant(c.sync));
      const double d =
          compare(amplitudes(renderLive(bwd, slot, N - k0, N)), amplitudes(renderLive(fwd, slot, k0, N)), kSr, N).worstDb;
      CAPTURE(std::string(c.name));
      CAPTURE(corr);
      MESSAGE("backward vs forward spectra (os 8, " << k0 * kSr / N << " Hz): " << std::string(c.name)
                                                    << (corr ? std::string() : std::string(" (no corr)")) << ": " << d
                                                    << " dB");
      CHECK(d <= -120.0);
    }
  for (const auto& [km, margin] : {std::pair{41.0, 2.0}, std::pair{683.0, 6.0}}) {
    const double fm = km / N;
    struct Modulated {
      const char* name;
      WarpMode mode;
      Ctl warp, sync, warpB, syncB;
    };
    const Modulated mods[] = {
        {"sync", WarpMode::Bend, constant(0), sine(0.3, 0.4, fm), constant(0), sine(0.3, 0.4, -fm)},
        {"pulse width", WarpMode::PulseWidth, sine(0.8, 0.0, fm), constant(0), sine(0.8, 0.0, -fm), constant(0)},
    };
    for (const Modulated& c : mods) {
      FrameTableSlot slot(t);
      LiveRegion fwd(8, kSr, c.mode, true, c.warp, constant(0), c.sync);
      LiveRegion bwd(8, kSr, c.mode, true, c.warpB, constant(0), c.syncB);
      Reference ref;
      ref.os = 8;
      ref.sampleRate = kSr;
      const std::vector<double> want = ref.amplitudes(t->view(), c.mode, c.warp, constant(0), c.sync, k0, N);
      const double af = compare(amplitudes(renderLive(fwd, slot, k0, N)), want, kSr, N).worstDb;
      const double ab = compare(amplitudes(renderLive(bwd, slot, N - k0, N)), want, kSr, N).worstDb;
      CAPTURE(std::string(c.name));
      CAPTURE(km);
      MESSAGE("alias against the 64x reference (os 8, " << k0 * kSr / N << " Hz, modulator " << km * kSr / N
                                                         << " Hz): " << std::string(c.name) << ": forward " << af
                                                         << " dBFS, backward " << ab << " dBFS");
      CHECK(ab <= af + margin);
    }
  }
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

// ---- TMM.8-fix (3): the common case, LFO- and envelope-rate modulation at the auto factor ------------------------

namespace {

// The control a Spl input plays: the function's values at the block ends, joined linearly (the kernel's ramp).
Ctl blockRamp(Ctl f, uint32_t block) {
  return [f, block](double t) {
    const double b = static_cast<double>(block);
    const double k = std::floor((t + 1.0) / b);  // the block whose end is at or after t: ends at k·b − 1 … (k+1)·b − 1
    const double t0 = k * b - 1.0, t1 = t0 + b;
    const double w = (t - t0) / b;
    return f(t0) + w * (f(t1) - f(t0));
  };
}

// A periodic envelope: 50 ms raised-cosine rise from lo to hi at t = 0, held, 50 ms fall at half the period.
Ctl envelope(double lo, double hi, double period, double sr) {
  return [=](double t) {
    double x = std::fmod(t, period);
    if (x < 0) x += period;
    const double ramp = 0.05 * sr;
    double g;
    if (x < ramp) g = 0.5 - 0.5 * std::cos(std::numbers::pi * x / ramp);
    else if (x < period / 2) g = 1.0;
    else if (x < period / 2 + ramp) g = 0.5 + 0.5 * std::cos(std::numbers::pi * (x - period / 2) / ramp);
    else g = 0.0;
    return lo + (hi - lo) * g;
  };
}

double runSlowCase(const AliasCase& c, uint32_t os, uint32_t k0, uint32_t N, bool spl) {
  const TablePtr t = build({ftfix::basic(BasicCycle::Saw)}, kSr);
  FrameTableSlot slot(t);
  LiveRegion r(os, kSr, c.mode, c.corr, c.warp, c.fold, c.sync);
  r.splControls = spl;
  const std::vector<double> live = amplitudes(renderLive(r, slot, k0, N, 16384));
  Reference ref;
  ref.os = os;
  ref.sampleRate = kSr;
  const auto ctl = [&](const Ctl& f) { return spl ? blockRamp(f, 64) : f; };
  const std::vector<double> want = ref.amplitudes(t->view(), c.mode, ctl(c.warp), ctl(c.fold), ctl(c.sync), k0, N);
  return compare(live, want, kSr, N).worstDb;
}

struct SlowMod {
  const char* name;
  std::function<Ctl(double lo, double hi)> make;
};

void slowModSuite(const std::vector<uint32_t>& pitches, const std::vector<SlowMod>& mods, bool spl, double bound,
                  const std::vector<std::string>& only = {}) {
  constexpr uint32_t N = 1u << 16;  // bins of 0.73 Hz: the slowest bin-exact LFO
  for (const SlowMod& m : mods) {
    const std::vector<AliasCase> cases = {
        {"bend", WarpMode::Bend, m.make(-1.0, 1.0), constant(0), constant(0), true},
        {"mirror", WarpMode::Mirror, m.make(-1.0, 1.0), constant(0), constant(0), true},
        {"pulse width", WarpMode::PulseWidth, m.make(-1.0, 1.0), constant(0), constant(0), true},
        {"fold", WarpMode::Bend, constant(0), m.make(0.0, 1.0), constant(0), false},
        {"sync", WarpMode::Bend, constant(0), constant(0), m.make(0.0, 1.0), true},
    };
    for (const AliasCase& c : cases) {
      if (!only.empty() && std::find(only.begin(), only.end(), c.name) == only.end()) continue;
      const uint32_t os = c.name == "fold" ? 8u : 16u;  // the auto factors (wavetableOversampling)
      double worst = -400, at = 0;
      for (const uint32_t k0 : pitches) {
        const double w = runSlowCase(c, os, k0, N, spl);
        if (w > worst) {
          worst = w;
          at = k0 * kSr / N;
        }
      }
      MESSAGE("slow modulation (" << (spl ? "automation, block ramps" : "audio-rate wire") << "): " << std::string(m.name)
                                  << ", " << c.name << " at x" << os << ": worst " << worst << " dBFS (f0 " << at << " Hz)");
      CAPTURE(m.name);
      CAPTURE(c.name);
      CHECK(worst <= bound);
    }
  }
}

// km: the LFO's bin (0.73 Hz each); 0: the 50 ms envelope.
std::vector<SlowMod> slowMods(std::vector<uint32_t> kms) {
  constexpr double N = 1u << 16;
  std::vector<SlowMod> v;
  for (const uint32_t km : kms) {
    if (km == 0) {
      v.push_back({"50 ms envelope", [N](double lo, double hi) { return envelope(lo, hi, N, kSr); }});
      continue;
    }
    const char* label = km == 1 ? "0.73 Hz LFO" : km == 3 ? "2.2 Hz LFO" : km == 7 ? "5.1 Hz LFO" : "19.8 Hz LFO";
    v.push_back({label, [km, N](double lo, double hi) { return sine(0.5 * (hi - lo), 0.5 * (hi + lo), km / N, 0.3); }});
  }
  return v;
}

}  // namespace

TEST_CASE("wavetable_live: LFO / envelope-rate modulation at the auto factor (48 kHz, 1760 Hz; bend and sync)") {
  slowModSuite({2405u}, slowMods({27u, 0u}), false, -100.0, {"bend", "sync"});
}

// The slow tier: every control, 0.73 / 2.2 / 5.1 / 19.8 Hz LFOs and the envelope, an audio-rate wire and automation
// (block ramps), 109 Hz and 4.4 kHz; one case per modulation and rate (each ≈ 30 s; the shards run them in parallel).
#define PQ_SLOW_MOD(km, spl, label)                                                                                    \
  TEST_CASE("wavetable_live: slow modulation " label " at the auto factor, 109 Hz / 4.4 kHz" *                         \
            doctest::test_suite("slow")) {                                                                           \
    slowModSuite({149u, 6005u}, slowMods({km}), spl, -100.0);                                                        \
  }
PQ_SLOW_MOD(1u, false, "(0.73 Hz LFO, audio rate)")
PQ_SLOW_MOD(3u, false, "(2.2 Hz LFO, audio rate)")
PQ_SLOW_MOD(7u, false, "(5.1 Hz LFO, audio rate)")
PQ_SLOW_MOD(27u, false, "(19.8 Hz LFO, audio rate)")
PQ_SLOW_MOD(0u, false, "(50 ms envelope, audio rate)")
PQ_SLOW_MOD(1u, true, "(0.73 Hz LFO, automation)")
PQ_SLOW_MOD(27u, true, "(19.8 Hz LFO, automation)")
PQ_SLOW_MOD(0u, true, "(50 ms envelope, automation)")

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

TEST_CASE("wavetable_shape (BR.wt): a small input round a low point shapes the curve there; the tap is the display's u") {
  // The bug report: a small signal biased onto a sine curve's trough. The output must be the curve over the swept
  // span (here −cos: a ripple at twice the input frequency, not DC), and the voice tap — the face's dots — must sit at
  // the u the kernel read, through the one mapping both use (shapeTablePosition).
  const TablePtr t = build({ftfix::basic(BasicCycle::Sine)}, kSr, {}, true);
  FrameTableSlot slot(t);
  slot.tap()->addReader();
  ShapeRegion r(1, kSr);
  constexpr uint32_t F = 4096;
  constexpr double kBias = 0.5, kAmp = 0.15;  // u = 0.75 ± 0.075: the trough of sin(2πu)
  std::vector<float> x(F), y(F);
  for (uint32_t i = 0; i < F; ++i) x[i] = static_cast<float>(kBias + kAmp * std::sin(kTwoPi * 100.0 * i / kSr));
  r.block(slot, x.data(), F, y.data());
  double worst = 0, worstU = 0, lo = 1e9, hi = -1e9, wantLo = 1e9, wantHi = -1e9;
  for (uint32_t i = 64; i < F; ++i) {
    const double u = shapeTablePosition(x[i]);
    worstU = std::max(worstU, std::fabs(u - (0.5 * x[i] + 0.5)));
    const double want = shapeCurve(t->view(), 0.0f, liveLevels(t->view(), 0.0f), u);
    worst = std::max(worst, std::fabs(want - y[i]));
    lo = std::min(lo, static_cast<double>(y[i]));
    hi = std::max(hi, static_cast<double>(y[i]));
    wantLo = std::min(wantLo, want);
    wantHi = std::max(wantHi, want);
  }
  MESSAGE("trough: output " << lo << " … " << hi << ", the curve " << wantLo << " … " << wantHi << ", worst " << worst);
  CHECK(worstU < 1e-6);  // u = (x + 1)/2 inside −1 … 1
  CHECK(worst < 2e-3);
  CHECK(wantHi - wantLo > 0.1);  // the curve's span there is a ripple (1 − cos(2π·0.075) ≈ 0.11), not DC
  CHECK(hi - lo == doctest::Approx(wantHi - wantLo).epsilon(0.05));
  CHECK(lo == doctest::Approx(-1.0).epsilon(2e-3));
  // Every tapped sample is shapeTablePosition of an input sample (the face draws dots at exactly these u).
  const pq::VoiceTap& tap = *slot.tap();
  REQUIRE(tap.written(0) > 0);
  uint32_t checked = 0;
  float worstTap = 0.0f, tapLo = 1.0f, tapHi = 0.0f;
  for (uint64_t k = 0; k < tap.written(0); ++k) {
    pq::VoiceTapRecord rec;
    if (!tap.read(0, k, rec)) continue;
    for (uint32_t j = 0; j < pq::kVoiceTapSamples; ++j) {
      float best = 1.0f;
      for (uint32_t i = 0; i < F; ++i) best = std::min(best, std::fabs(rec.sample[j][0] - shapeTablePosition(x[i])));
      worstTap = std::max(worstTap, best);
      tapLo = std::min(tapLo, rec.sample[j][0]);
      tapHi = std::max(tapHi, rec.sample[j][0]);
      ++checked;
    }
  }
  CHECK(checked > 0);
  CHECK(worstTap == 0.0f);
  CHECK(tapLo >= 0.675f - 1e-6f);  // within the swept span 0.75 ± 0.075
  CHECK(tapHi <= 0.825f + 1e-6f);
  slot.tap()->removeReader();
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

TEST_CASE("wavetable_live: a control that turns param-rate after an audio-rate block continues from its last value") {
  // AU.fold: the compiler's param ramp (TX.smooth) feeds a moving param per sample (an audio-rate block), then the param
  // itself once it rests. The kernel ramps a param-rate control from the previous block's value: that must be the
  // ramp's last value, not the one before the ramp (the click at the end of every knob move / lane step on a live
  // fold, warp or sync: the control jumped back and re-ramped over one block). So a param-rate block after a ramp
  // renders bit for bit as the same block given the value per sample.
  const TablePtr t = build({ftfix::basic(BasicCycle::Saw), ftfix::basic(BasicCycle::Square)}, kSr);
  FrameTableSlot slot(t);
  constexpr uint32_t F = 64;
  std::vector<float> phase(2 * F), pos(8, 0.3f);
  for (uint32_t i = 0; i < 2 * F; ++i) phase[i] = static_cast<float>(std::fmod(0.0071 * i, 1.0));
  alignas(32) float modeV[8], corrV[8];
  for (const WarpMode mode : {WarpMode::Bend, WarpMode::PulseWidth}) {
    std::fill(modeV, modeV + 8, static_cast<float>(mode));
    std::fill(corrV, corrV + 8, 1.0f);
    // Block 1: warp, fold, sync ramp per sample to (0.3, 0.6, 0.25); block 2: those values, as params or per sample.
    const float end[3] = {0.3f, 0.6f, 0.25f};
    std::vector<float> ramp[3], hold[3];
    alignas(32) float spl[3][8];
    for (int c = 0; c < 3; ++c) {
      ramp[c].resize(F);
      hold[c].assign(F, end[c]);
      for (uint32_t i = 0; i < F; ++i) ramp[c][i] = end[c] * static_cast<float>(i + 1) / F;
      std::fill(spl[c], spl[c] + 8, end[c]);
    }
    auto render = [&](bool paramRate) {
      State st(Atom::WavetableLive, 1, kSr);
      std::vector<float> out(2 * F);
      for (int blk = 0; blk < 2; ++blk) {
        const bool p = blk == 1 && paramRate;
        const uint32_t mask = (1u << 1) | (1u << 5) | (1u << 6) | (p ? (1u << 2) | (1u << 3) | (1u << 4) : 0u);
        const float* ins[7] = {phase.data() + blk * F, pos.data(),
                               p ? spl[0] : (blk == 0 ? ramp[0] : hold[0]).data(),
                               p ? spl[1] : (blk == 0 ? ramp[1] : hold[1]).data(),
                               p ? spl[2] : (blk == 0 ? ramp[2] : hold[2]).data(), modeV, corrV};
        float* outs[1] = {out.data() + blk * F};
        KernelArgs a{ins, outs, st.mem.data(), &slot, F, 7, 1, mask};
        const KernelFn fn = findKernel(Atom::WavetableLive, 1, mask);
        REQUIRE(fn != nullptr);
        fn(a);
      }
      return out;
    };
    const std::vector<float> asParams = render(true), perSample = render(false);
    CAPTURE(static_cast<int>(mode));
    CHECK(std::memcmp(asParams.data(), perSample.data(), asParams.size() * sizeof(float)) == 0);
  }
}
