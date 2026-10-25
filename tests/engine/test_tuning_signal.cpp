// T2.6: tuning as a signal — the per-region linear form (tuning::linearTuningForm → dsp::TuningTable via
// engine::makeTuningTable) against buildMOSScale, and the tuning_hz kernel: generator sweeps through several
// regions (per sample and block-rate), modulated period / stretch / reference, the exact unmodulated pitch, the
// fallbacks, and the allocation trap.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/tuning_table.h"
#include "pq/engine/tuning_signal.h"
#include "pq/tuning/mos_regions.h"
#include "pq/tuning/scala.h"
#include "pq/tuning/scale_tuning.h"
#include "pq/tuning/tuning_signal.h"
#include "tuning_signal_ref.h"

namespace dsp = pq::dsp;
namespace tun = pq::tuning;
using pq::alloc_trap::countRtViolations;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr double kRef = 261.626;

tun::Tuning mosTuning(const tun::MosParams& p, double ref = kRef) {
  auto m = tun::Mos::make(p);
  REQUIRE(m.ok());
  auto t = tun::Tuning::make(m.value(), ref);
  REQUIRE(t.ok());
  return t.value();
}

dsp::TuningTable table(const tun::Tuning& t) {
  auto r = pq::engine::makeTuningTable(t);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
  return std::move(r.value());
}

tun::MosParams mos(int32_t n, double period, int32_t ppe, int32_t rotation, double stretch, double gen) {
  tun::MosParams p;
  p.n = n;
  p.period = period;
  p.periodsPerEquave = ppe;
  p.rotation = rotation;
  p.octaveStretch = stretch;
  p.generator = gen;
  return p;
}

// The constant-order interval holding g as buildMOSScale sees it (boundaries included: the region hint and the
// nudge decide), for crossing detection in the tests.
int64_t intervalOf(const tun::MosLinearTable& t, const tun::MosParams& p, double g) {
  const tun::MosLinearSpan s = t.lookup(g, p.period, p.region ? &*p.region : nullptr);
  return static_cast<int64_t>(s.periodShift) * 100000 + s.interval;
}

// Runs the kernel over `frames` frames with the given inputs (pointer + Spl flag each), into hz / cents.
struct KIn {
  const float* p;
  bool spl;
};
// The first 8 inputs (degree … reference); select / node (T2.6-fix3) are Spl patterns, by default 0 (node 0 takes
// every lane with a degree, the T2.6 behaviour).
void runKernel(uint32_t lanes, const dsp::TuningTableView* v, const KIn (&in)[8], uint32_t frames, float* hz,
               float* cents, const float* select = nullptr, const float* node = nullptr) {
  alignas(32) static const float kZero[8] = {};
  uint32_t mask = 0b1100000000u;
  const float* ptr[10];
  for (uint32_t i = 0; i < 8; ++i) {
    ptr[i] = in[i].p;
    mask |= in[i].spl ? 1u << i : 0u;
  }
  ptr[8] = select != nullptr ? select : kZero;
  ptr[9] = node != nullptr ? node : kZero;
  float* out[2] = {hz, cents};
  dsp::KernelArgs a;
  a.in = ptr;
  a.out = out;
  a.resource = v;
  a.frames = frames;
  a.numIn = 10;
  a.numOut = 2;
  a.splMask = mask;
  const dsp::KernelFn fn = dsp::findKernel(dsp::Atom::TuningHz, lanes, mask);
  REQUIRE(fn != nullptr);
  pq::DenormalGuard dg;
  CHECK(countRtViolations([&] { fn(a); }) == 0);
}

struct Pattern {
  alignas(32) float v[8];
  explicit Pattern(float x) { std::fill(v, v + 8, x); }
  Pattern(std::initializer_list<float> xs) { std::copy(xs.begin(), xs.end(), v); }
};

}  // namespace

TEST_CASE("tuning signal: the linear form reproduces buildFullScale (MOS modes, stretch, multi-period) and EDOs") {
  struct Cfg {
    int32_t n;
    double period;
    int32_t ppe;
    double stretch;
  };
  const Cfg cfgs[] = {{7, 1200.0, 1, 0.0}, {7, 1200.0, 1, 3.5}, {5, 600.0, 2, 0.0}, {12, 1200.0, 1, -2.0}, {2, 1200.0, 1, 0.0}};
  double maxErr = 0.0;
  uint64_t checked = 0, skipped = 0, bad = 0;
  for (const Cfg& c : cfgs) {
    for (int32_t rot = 0; rot < c.n; ++rot) {
      const tun::MosParams base = mos(c.n, c.period, c.ppe, rot, c.stretch, c.period * 0.58);
      const dsp::TuningTable tab = table(mosTuning(base));
      const dsp::TuningTableView& v = tab.view();
      const int32_t N = c.n * c.ppe;
      for (double g = 0.37; g < c.period; g += c.period / 301.7) {
        const auto ref = tuning_ref::mosAt(base, g);  // buildMOSScale at g
        if (!ref) {
          ++skipped;
          continue;
        }
        for (int32_t eq = -2; eq <= 2; ++eq)
          for (int32_t d = -3; d < N + 3; ++d) {
            const double got = dsp::tuningCents(v, static_cast<int64_t>(eq) * N + d, g, c.period, c.stretch);
            const double err = std::fabs(got - ref->centsOf(d, eq));
            maxErr = std::max(maxErr, err);
            bad += err > 1e-9;
            ++checked;
          }
      }
    }
  }
  MESSAGE("MOS linear form vs buildFullScale: " << checked << " pitches, max |err| " << maxErr << " cents, "
                                                 << skipped << " degenerate generators skipped");
  CHECK(bad == 0);
  CHECK(maxErr <= 1e-9);
  CHECK(skipped * 100 < checked);

  // Exact boundary generators (T2.6-fix2, M3 review H3): every interval boundary of these chain lengths, in three
  // period shifts, with no region hint and with each region hint of that size around the boundary, against
  // buildFullScale with the same hint and the transferred mode (ties of step counts without a hint are ambiguous in
  // buildMOSScale itself and skipped).
  uint64_t bChecked = 0, bSkipped = 0, bBad = 0;
  for (const Cfg& c : cfgs) {
    const tun::MosLinearTable lt(c.n);
    const tun::MosRegionSet rs = tun::buildMosRegions(c.n, c.n, c.period, 1);
    std::vector<std::optional<tun::RegionHint>> hints{std::nullopt};
    for (const tun::MosRegion& r : rs.regions)
      if (r.perPeriodSize == c.n) hints.push_back(r.hint());
    for (const std::optional<tun::RegionHint>& h : hints)
      for (int32_t rot = 0; rot < c.n; rot += 2) {
        tun::MosParams base = mos(c.n, c.period, c.ppe, rot, c.stretch, c.period * 0.58);
        base.region = h;
        const dsp::TuningTable tab = table(mosTuning(base));
        const int32_t N = c.n * c.ppe;
        for (const double b : lt.boundaries())
          for (const int32_t shift : {-1, 0, 1}) {
            const double g = (shift + b) * c.period;
            if (h && !(g >= h->lowCents - 1e-6 && g <= h->highCents + 1e-6)) continue;
            const auto ref = tuning_ref::mosAt(base, g);
            if (!ref) {
              ++bSkipped;
              continue;
            }
            for (int32_t eq = -1; eq <= 1; ++eq)
              for (int32_t d = 0; d < N; ++d) {
                const double got = dsp::tuningCents(tab.view(), static_cast<int64_t>(eq) * N + d, g, c.period, c.stretch);
                bBad += std::fabs(got - ref->centsOf(d, eq)) > 1e-9;
                ++bChecked;
              }
          }
      }
  }
  MESSAGE("exact boundary generators: " << bChecked << " pitches, " << bSkipped << " tie generators skipped");
  CHECK(bBad == 0);
  CHECK(bChecked > 1000);

  // EDOs: D · equave / n.
  for (const auto& e : {tun::Edo::make(31).value(), tun::Edo::make(13, tun::Rational{3}).value(),
                        tun::Edo::makeCents(19, 1203.0).value()}) {
    const tun::Tuning t = tun::Tuning::make(e, 440.0).value();
    const dsp::TuningTable tab = table(t);
    for (int64_t D = -40; D <= 80; ++D) {
      const double ref = 1200.0 * std::log2(*tun::resolveHzDouble(
                                     tun::PitchId::fromDegree(static_cast<int32_t>(D), 0), t, {}) / 440.0);
      CHECK(std::fabs(dsp::tuningCents(tab.view(), D, 0.0, e.equaveCents(), 0.0) - ref) <= 1e-9);
    }
  }

  // Not linear: MODMOS, generator sequences, Scala scales.
  tun::MosParams mm = mos(7, 1200.0, 1, 0, 0.0, 700.0);
  mm.modmos = {0, 0, 1};
  CHECK_FALSE(pq::engine::makeTuningTable(mosTuning(mm)).ok());
  tun::MosParams gs = mos(7, 1200.0, 1, 0, 0.0, 700.0);
  gs.genSequence = {700.0, 690.0};
  CHECK_FALSE(pq::engine::makeTuningTable(mosTuning(gs)).ok());
  const auto scl = tun::parseScl("! x.scl\nx\n3\n!\n200.0\n500.0\n2/1\n");
  REQUIRE(scl.ok());
  const tun::Tuning st = tun::Tuning::make(tun::Scale::make(scl.value()).value(), kRef).value();
  CHECK_FALSE(pq::engine::makeTuningTable(st).ok());
}

namespace {

// The acceptance sweep: 8 voices of different degrees/equaves while the generator ramps through several regions,
// per sample (an audio-rate generator) and at block rate (a Spl generator per block: its value at the block start),
// compared per sample with buildMOSScale at the generator that sample used.
void generatorSweep(uint32_t frames) {
  struct Sweep {
    tun::MosParams p;
    double g0, g1;
  };
  // The first sweep's tuning carries the 5L2s region hint, which decides its exact 720-cent generator (T2.6-fix2);
  // every sweep holds the generator at each interval boundary it passes that float32 represents exactly (720, 600,
  // 480 … cents: where users stop a sweep), and the mode follows the explorer's transferModeRotation.
  tun::MosParams hinted = mos(7, 1200.0, 1, 2, 2.5, 700.0);
  hinted.region = tun::RegionHint{7, 4.0 / 7.0 * 1200.0, 720.0};
  const Sweep sweeps[] = {
      {hinted, 450.0, 850.0},                             // crosses g/P = 2/5, 1/2, 3/5, 2/3 (and 7edo, 5edo inside)
      {mos(5, 600.0, 2, 1, 0.0, 120.0), 50.0, 280.0},     // two periods per equave; crosses 1/4, 1/3
      {mos(12, 1200.0, 1, 5, 0.0, 700.0), 660.0, 740.0},  // a 12-note chain: crosses 5/9, 4/7, 3/5
  };
  const int32_t deg[8] = {0, 1, 2, 3, 4, 6, 5, -1};
  const int32_t eqv[8] = {0, 0, 0, -1, 1, 0, 2, 0};
  constexpr uint32_t kBlock = 128;
  constexpr uint32_t kHold = 2 * kBlock + 40;  // frames held at each exactly representable boundary (≥ a block start)
  for (const Sweep& s : sweeps) {
    const dsp::TuningTable tab = table(mosTuning(s.p));
    const dsp::TuningTableView& v = tab.view();
    const tun::MosLinearTable lt(s.p.n);
    Pattern pd(0.0f), pe(0.0f);
    for (uint32_t l = 0; l < 8; ++l) {
      pd.v[l] = static_cast<float>(deg[l]);
      pe.v[l] = static_cast<float>(eqv[l]);
    }
    const Pattern pp(static_cast<float>(s.p.period)), pst(static_cast<float>(s.p.octaveStretch)),
        pr(static_cast<float>(kRef));
    // The fallback is what the engine gives each note (T2.6-fix3 reads it: a Degree note's hz input over its static
    // pitch is its ratio / glide): its static Hz in the definition, and the VoiceEngine's cents of that float32 Hz.
    const tun::Mos defMos = tun::Mos::make(s.p).value();
    float h0[8], c0[8];
    for (uint32_t l = 0; l < 8; ++l) {
      h0[l] = static_cast<float>(kRef * std::exp2(defMos.centsOf(deg[l], eqv[l]) / 1200.0));
      c0[l] = static_cast<float>(1200.0 * std::log2(static_cast<double>(h0[l]) / kRef));
    }
    std::vector<float> gen(frames * 8), fallHz(frames * 8), fallC(frames * 8);
    for (uint32_t t = 0; t < frames; ++t)
      for (uint32_t l = 0; l < 8; ++l) {
        fallHz[t * 8 + l] = h0[l];
        fallC[t * 8 + l] = c0[l];
      }
    std::vector<double> stops;  // the boundaries the ramp pauses on
    for (const double b : lt.boundaries()) {
      const double c = b * s.p.period;
      if (c > s.g0 && c < s.g1 && static_cast<double>(static_cast<float>(c)) == c) stops.push_back(c);
    }
    CHECK(!stops.empty());
    {
      size_t next = 0;
      uint32_t t = 0;
      for (uint32_t i = 0; t < frames; ++i) {
        const double x = std::min(s.g1, s.g0 + (s.g1 - s.g0) * i / (frames - 1));
        for (; next < stops.size() && x >= stops[next]; ++next)
          for (uint32_t k = 0; k < kHold && t < frames; ++k, ++t) std::fill_n(gen.begin() + t * 8, 8, static_cast<float>(stops[next]));
        if (t < frames) std::fill_n(gen.begin() + (t++) * 8, 8, static_cast<float>(x));
      }
    }
    for (const bool blockRate : {false, true}) {
      CAPTURE(blockRate);
      std::vector<float> hz(frames * 8), cents(frames * 8);
      for (uint32_t b = 0; b < frames; b += kBlock) {
        const uint32_t n = std::min(kBlock, frames - b);
        const KIn in[8] = {{pd.v, true},          {pe.v, true}, {fallHz.data() + b * 8, false},
                           {fallC.data() + b * 8, false},
                           {gen.data() + b * 8, blockRate}, {pp.v, true}, {pst.v, true}, {pr.v, true}};
        runKernel(8, &v, in, n, hz.data() + b * 8, cents.data() + b * 8);
      }
      double maxErr = 0.0;
      uint64_t checked = 0, skipped = 0, skippedOff = 0, onBoundary = 0, jumps = 0, crossings = 0, badHz = 0,
               badCents = 0, badJumps = 0;
      int64_t firstBad = -1;
      std::optional<tun::Mos> ref;
      auto atBoundary = [&](double g) {
        for (const double b : lt.boundaries())
          if (std::fabs(b * s.p.period - g) < 1e-9) return true;
        return false;
      };
      for (uint32_t t = 0; t < frames; ++t) {
        const uint32_t tg = blockRate ? t / kBlock * kBlock : t;  // the sample whose generator this one used
        const double g = static_cast<double>(gen[tg * 8]);
        if (t == 0 || tg != (blockRate ? (t - 1) / kBlock * kBlock : t - 1)) ref = tuning_ref::mosAt(s.p, g);
        const bool boundary = atBoundary(g);
        if (!ref) {  // only an exact boundary with a tie of step counts has no reference (tuning_ref::ambiguousGenerator)
          skipped += 8;
          skippedOff += boundary ? 0 : 8;
          continue;
        }
        onBoundary += boundary ? 8 : 0;
        const bool crossed = t > 0 && !blockRate &&
                             intervalOf(lt, s.p, static_cast<double>(gen[(t - 1) * 8])) != intervalOf(lt, s.p, g);
        crossings += crossed;
        for (uint32_t l = 0; l < 8; ++l) {
          const double c = ref->centsOf(deg[l], eqv[l]);
          const double refHz = kRef * std::exp2(c / 1200.0);
          const float out = hz[t * 8 + l];
          if (!tuning_ref::withinTolerance(out, refHz)) {
            ++badHz;
            if (firstBad < 0) firstBad = t;
          }
          // voice.cents = the engine's cents of the note's float32 Hz moved by the tuning (T2.6-fix3): it carries that
          // Hz's rounding (≤ 1.03e-4 cents) besides its own float32 rounding.
          badCents += std::fabs(cents[t * 8 + l] - c) > std::fabs(c) * 1.2e-7 + 2e-4;
          maxErr = std::max(maxErr, tuning_ref::centsError(out, refHz));
          ++checked;
          // Continuity: a step larger than the sweep can move a degree (≤ |K|·Δg ≪ 1 ¢) happens only where the
          // generator crosses into another constant-order interval (an intended region change).
          if (t > 0 && !blockRate && std::fabs(1200.0 * std::log2(hz[t * 8 + l] / hz[(t - 1) * 8 + l])) > 1.0) {
            ++jumps;
            badJumps += !crossed;
          }
        }
      }
      MESSAGE("sweep n=" << s.p.n << " " << s.g0 << "→" << s.g1 << (blockRate ? " (block rate)" : " (per sample)")
                         << ", " << frames << " frames: " << checked << " voice-samples, max error " << maxErr
                         << " cents (incl. float32 Hz rounding), " << crossings << " interval crossings, " << jumps
                         << " voice jumps, " << onBoundary << " voice-samples at exact boundaries checked, "
                         << skipped << " skipped (exact boundaries with a tie, where buildMOSScale is ambiguous)");
      // The 1-lane kernel (a global degree signal) on the same sweep: lane 4's degree, degree and generator as
      // audio-rate buffers, from a pre-onset start (hz 0 for the first 5 frames).
      if (!blockRate) {
        std::vector<float> d1(frames, static_cast<float>(deg[4])), e1(frames, static_cast<float>(eqv[4])),
            g1(frames), h1(frames, h0[4]), c1(frames, c0[4]), oh(frames), oc(frames);
        for (uint32_t t = 0; t < frames; ++t) g1[t] = gen[t * 8];
        for (uint32_t t = 0; t < 5; ++t) h1[t] = 0.0f;
        uint64_t bad1 = 0;
        for (uint32_t b = 0; b < frames; b += kBlock) {
          const uint32_t n = std::min(kBlock, frames - b);
          const KIn in1[8] = {{d1.data() + b, false}, {e1.data() + b, false}, {h1.data() + b, false},
                              {c1.data() + b, false}, {g1.data() + b, false}, {pp.v, true}, {pst.v, true}, {pr.v, true}};
          runKernel(1, &v, in1, n, oh.data() + b, oc.data() + b);
        }
        for (uint32_t t = 0; t < frames; ++t)
          bad1 += t < 5 ? oh[t] != 0.0f : std::fabs(oh[t] - hz[t * 8 + 4]) > std::fabs(hz[t * 8 + 4]) * 1.2e-7f;
        CHECK(bad1 == 0);
      }
      CAPTURE(firstBad);
      CHECK(badHz == 0);
      CHECK(badCents == 0);
      CHECK(badJumps == 0);
      if (!blockRate) CHECK(crossings >= 2);
      // (Not every crossing jumps any more: with the mode carried across it, as the explorer does, a degree whose
      // root-relative order is unchanged moves continuously. The pitch per sample is checked above.)
      CHECK(skippedOff == 0);
      CHECK(onBoundary > 0);
    }
  }
}

}  // namespace

TEST_CASE("tuning signal: a generator sweep through several regions matches buildMOSScale per sample") {
  generatorSweep(6000);
}

TEST_CASE("tuning signal: the same sweeps at 48 kHz resolution (one second each)" * doctest::test_suite("slow")) {
  generatorSweep(48000);
}

TEST_CASE("tuning signal: modulated period, stretch and reference (EDO equave and MOS) per sample") {
  constexpr uint32_t kFrames = 4096;
  const tun::Tuning edo = tun::Tuning::make(tun::Edo::make(31).value(), kRef).value();
  const tun::MosParams mp = mos(7, 1200.0, 1, 4, 0.0, 696.6);
  const tun::Tuning mt = mosTuning(mp);
  for (const bool isEdo : {true, false}) {
    CAPTURE(isEdo);
    const dsp::TuningTable tab = table(isEdo ? edo : mt);
    Pattern pd{0, 1, 5, 9, 17, 30, 3, 2}, pe{0, 0, 0, 1, -1, 1, 2, -2};
    std::vector<float> gen(kFrames * 8), per(kFrames * 8), st(kFrames * 8), ref(kFrames * 8), fh(kFrames * 8),
        fc(kFrames * 8, 0.0f), hz(kFrames * 8), cents(kFrames * 8);
    for (uint32_t t = 0; t < kFrames; ++t)
      for (uint32_t l = 0; l < 8; ++l) {
        const double u = static_cast<double>(t) / kFrames;
        // The engine's hz for the note: its static pitch in the definition (T2.6-fix3 reads it).
        fh[t * 8 + l] = static_cast<float>(*tun::resolveHzDouble(
            tun::PitchId::fromDegree(static_cast<int32_t>(pd.v[l]), static_cast<int32_t>(pe.v[l])), isEdo ? edo : mt, {}));
        gen[t * 8 + l] = static_cast<float>(696.6 + 2.0 * std::sin(17.0 * u + l));
        per[t * 8 + l] = static_cast<float>(1200.0 + 9.0 * std::sin(5.0 * u + 0.3 * l));
        st[t * 8 + l] = static_cast<float>(4.0 * std::sin(3.0 * u));
        ref[t * 8 + l] = static_cast<float>(kRef * (1.0 + 0.05 * std::sin(7.0 * u)));
      }
    const KIn in[8] = {{pd.v, true}, {pe.v, true}, {fh.data(), false}, {fc.data(), false},
                       {gen.data(), false}, {per.data(), false}, {st.data(), false}, {ref.data(), false}};
    runKernel(8, &tab.view(), in, kFrames, hz.data(), cents.data());
    for (uint32_t t = 0; t < kFrames; t += 7)
      for (uint32_t l = 0; l < 8; ++l) {
        const auto d = static_cast<int32_t>(pd.v[l]), e = static_cast<int32_t>(pe.v[l]);
        const double P = per[t * 8 + l], S = st[t * 8 + l], R = ref[t * 8 + l];
        double c = 0.0;
        if (isEdo) {
          c = (static_cast<double>(e) * 31 + d) * P / 31.0 * (1200.0 + S) / 1200.0;
        } else {
          tun::MosParams q = mp;
          q.period = P;
          q.octaveStretch = S;
          const auto rc = tuning_ref::mosCents(q, gen[t * 8 + l], d, e);
          if (!rc) continue;
          c = *rc;
        }
        CHECK(tuning_ref::withinTolerance(hz[t * 8 + l], R * std::exp2(c / 1200.0)));
      }
  }
}

TEST_CASE("tuning signal: unmodulated inputs sound at the static pitch; fallbacks; 1-lane kernel") {
  const tun::MosParams mp = mos(7, 1200.0, 1, 3, 1.5, 696.578428);
  const tun::Tuning mt = mosTuning(mp, 261.626);
  const tun::Tuning edo = tun::Tuning::make(tun::Edo::make(22).value(), 261.626).value();
  for (const tun::Tuning* t : {&mt, &edo}) {
    const dsp::TuningTable tab = table(*t);
    const dsp::TuningTableView& v = tab.view();
    // Params as the image holds them: float32 of the document's values.
    const Pattern pg(static_cast<float>(v.generator)), pp(static_cast<float>(v.period)),
        ps(static_cast<float>(v.stretch)), pr(static_cast<float>(v.referenceHz));
    Pattern pd{0, 1, 2, 6, 3, kNaN, 4, 10}, pe{0, 0, -1, 2, 0, 0, 1, -3};
    constexpr uint32_t kF = 64;
    // The fallback is what the engine gives a note: its static Hz, float(resolveHzDouble) (T2.6-fix1); lane 5 has no
    // degree, lane 4 starts at frame 10 (voice.hz 0 before). The fallback cents are arbitrary (-7): T2.6-fix3's
    // voice.cents is the input's moved by the tuning, so an unmodulated node passes them through too.
    std::vector<float> stat(8, 123.0f);
    std::vector<double> statD(8, 0.0);
    for (uint32_t l = 0; l < 8; ++l) {
      if (l == 5) continue;
      const auto st = tun::resolveHzDouble(
          tun::PitchId::fromDegree(static_cast<int32_t>(pd.v[l]), static_cast<int32_t>(pe.v[l])), *t, {});
      REQUIRE(st);
      statD[l] = *st;
      stat[l] = static_cast<float>(*st);
    }
    std::vector<float> fh(kF * 8), fc(kF * 8, -7.0f), hz(kF * 8), cents(kF * 8);
    for (uint32_t f = 0; f < kF; ++f)
      for (uint32_t l = 0; l < 8; ++l) fh[f * 8 + l] = l == 4 && f < 10 ? 0.0f : stat[l];
    const KIn in[8] = {{pd.v, true}, {pe.v, true}, {fh.data(), false}, {fc.data(), false},
                       {pg.v, true}, {pp.v, true}, {ps.v, true}, {pr.v, true}};
    runKernel(8, &v, in, kF, hz.data(), cents.data());
    for (uint32_t l = 0; l < 8; ++l) {
      CAPTURE(l);
      for (uint32_t f = 0; f < kF; ++f) {
        if (l == 5) {  // no degree: the fallback passes through
          CHECK(hz[f * 8 + l] == 123.0f);
          CHECK(cents[f * 8 + l] == -7.0f);
          continue;
        }
        if (l == 4 && f < 10) {  // before the onset: 0 Hz, like voice.hz
          CHECK(hz[f * 8 + l] == 0.0f);
          continue;
        }
        // Every tuning input at its definition value: the note's own static Hz and cents, bit for bit.
        CHECK(hz[f * 8 + l] == stat[l]);
        CHECK(cents[f * 8 + l] == -7.0f);
      }
    }
    // An unmodulated tuning passes ANY fallback through (e.g. a voice.hz moved by a Pitch event): the pass-through
    // trusts the lowering to give a following note its static Hz in the table's definition.
    {
      std::vector<float> other(kF * 8, 333.0f), hz3(kF * 8), c3(kF * 8);
      const KIn in3[8] = {{pd.v, true}, {pe.v, true}, {other.data(), false}, {fc.data(), false},
                          {pg.v, true}, {pp.v, true}, {ps.v, true}, {pr.v, true}};
      runKernel(8, &v, in3, kF, hz3.data(), c3.data());
      for (uint32_t i = 0; i < kF * 8; ++i) CHECK(hz3[i] == 333.0f);
    }
    // A modulated input (the reference one float32 ulp away from its definition value) computes the pitch: within an
    // ulp of the static Hz of that reference, and for most samples bit-equal to it.
    {
      const float refUp = std::nextafter(static_cast<float>(v.referenceHz), 1e30f);
      const Pattern pr2(refUp);
      std::vector<float> hz4(kF * 8), c4(kF * 8);
      const KIn in4[8] = {{pd.v, true}, {pe.v, true}, {fh.data(), false}, {fc.data(), false},
                          {pg.v, true}, {pp.v, true}, {ps.v, true}, {pr2.v, true}};
      runKernel(8, &v, in4, kF, hz4.data(), c4.data());
      uint32_t exact = 0, total = 0;
      for (uint32_t l = 0; l < 8; ++l)
        for (uint32_t f = 0; f < kF; ++f) {
          if (l == 5 || (l == 4 && f < 10)) continue;
          const float want = static_cast<float>(statD[l] * (static_cast<double>(refUp) / v.referenceHz));
          CHECK(std::fabs(hz4[f * 8 + l] - want) <= std::nextafter(want, 1e30f) - want);  // ≤ 1 ulp
          exact += hz4[f * 8 + l] == want;
          ++total;
        }
      MESSAGE("modulated by one ulp: " << exact << " of " << total << " samples bit-equal to the static pitch");
      CHECK(exact * 10 >= total * 9);
    }

    // The per-sample path agrees with the block path for constant inputs (an audio-rate generator buffer that holds
    // the parameter's value).
    std::vector<float> gb(kF * 8, static_cast<float>(v.generator)), hz2(kF * 8), c2(kF * 8);
    const KIn in2[8] = {{pd.v, true}, {pe.v, true}, {fh.data(), false}, {fc.data(), false},
                        {gb.data(), false}, {pp.v, true}, {ps.v, true}, {pr.v, true}};
    runKernel(8, &v, in2, kF, hz2.data(), c2.data());
    for (uint32_t i = 0; i < kF * 8; ++i) CHECK(std::fabs(hz2[i] - hz[i]) <= std::fabs(hz[i]) * 2.4e-7f);

    // The 1-lane kernel: lane 3's degree as a global signal (its fallback is lane 3's static Hz, as above).
    std::vector<float> d1(kF, pd.v[3]), e1(kF, pe.v[3]), h1(kF, stat[3]), c1(kF, 0.0f), oh(kF), oc(kF);
    const Pattern g1(pg.v[0]), p1(pp.v[0]), s1(ps.v[0]), r1(pr.v[0]);
    const KIn in1[8] = {{d1.data(), false}, {e1.data(), false}, {h1.data(), false}, {c1.data(), false},
                        {g1.v, true}, {p1.v, true}, {s1.v, true}, {r1.v, true}};
    runKernel(1, &v, in1, kF, oh.data(), oc.data());
    for (uint32_t f = 0; f < kF; ++f) CHECK(std::fabs(oh[f] - hz[f * 8 + 3]) <= std::fabs(hz[f * 8 + 3]) * 1.2e-7f);
  }

  // No table: the fallback passes through. A degree that changes mid-block takes effect at that sample (a modulated
  // reference, one ulp up, so the node computes; the fallback is each note's static pitch, as the engine has it).
  {
    const dsp::TuningTable tab = table(edo);
    constexpr uint32_t kF = 32;
    std::vector<float> d(kF * 8), e(kF * 8, 0.0f), fh(kF * 8), fc(kF * 8, 1.0f), hz(kF * 8), c(kF * 8);
    for (uint32_t f = 0; f < kF; ++f)
      for (uint32_t l = 0; l < 8; ++l) {
        d[f * 8 + l] = f < 13 ? 0.0f : (l == 2 ? kNaN : 22.0f);
        fh[f * 8 + l] = f < 13 ? 261.626f : (l == 2 ? 200.0f : 523.252f);
      }
    const Pattern pp(1200.0f), z(0.0f), pr(std::nextafter(261.626f, 1e30f));
    const KIn in[8] = {{d.data(), false}, {e.data(), false}, {fh.data(), false}, {fc.data(), false},
                       {z.v, true}, {pp.v, true}, {z.v, true}, {pr.v, true}};
    runKernel(8, nullptr, in, kF, hz.data(), c.data());
    for (uint32_t i = 0; i < kF * 8; ++i) CHECK(hz[i] == fh[i]);
    runKernel(8, &tab.view(), in, kF, hz.data(), c.data());
    for (uint32_t f = 0; f < kF; ++f)
      for (uint32_t l = 0; l < 8; ++l) {
        const float want = f < 13 ? 261.626f : (l == 2 ? 200.0f : 523.252f);
        CHECK(hz[f * 8 + l] == doctest::Approx(want).epsilon(1e-6));
      }
  }
}

TEST_CASE("tuning signal: a 5L2s mode carried across 720 cents matches the explorer per degree (fixture)") {
  // T2.6-fix2 (M3 review M7): the table carries the mode across interval boundaries like the explorer's
  // transferModeRotation (the root keeps its chain position). tests/engine/fixtures/mode_transfer.json is the
  // explorer's own output (tools/fixtures/gen_mode_transfer_fixture.mjs): every rotation of 5L2s defined at 700 cents
  // with the 5L2s region selected, at generators through 720 cents (exactly 720 too, where the hint decides the side).
  std::ifstream in(std::string(PQ_TEST_DATA_DIR) + "/engine/fixtures/mode_transfer.json", std::ios::binary);
  REQUIRE(in.good());
  const nlohmann::json fx = nlohmann::json::parse(in);
  const int32_t n = fx["n"];
  const double P = fx["period"];
  uint32_t checked = 0, carried = 0;
  double worst = 0.0;
  for (const auto& c : fx["cases"]) {
    tun::MosParams p = mos(n, P, 1, c["rotation"].get<int32_t>(), 0.0, fx["definitionGenerator"].get<double>());
    p.region = tun::RegionHint{fx["region"]["perPeriodSize"].get<int32_t>(), fx["region"]["lowCents"].get<double>(),
                               fx["region"]["highCents"].get<double>()};
    const dsp::TuningTable tab = table(mosTuning(p));
    const double g = c["generator"];
    carried += c["carried"].get<int32_t>() != c["rotation"].get<int32_t>();
    for (int32_t j = 0; j < n; ++j) {
      const double err = std::fabs(dsp::tuningCents(tab.view(), j, g, P, 0.0) - c["pitches"][static_cast<size_t>(j)].get<double>());
      worst = std::max(worst, err);
      CAPTURE(g);
      CAPTURE(j);
      CHECK(err < 1e-6);
      ++checked;
    }
  }
  MESSAGE(checked << " degree pitches, worst " << worst << " cents; " << carried << " of " << fx["cases"].size()
                  << " cases carry the mode to another rotation");
  CHECK(carried > 0);
}

TEST_CASE("tuning signal: glides and note ratios compose with a modulated node; select / node; reference lanes") {
  // T2.6-fix3 (M3 review M9, H4): a Degree lane's hz input over its static degree pitch (a Pitch-event glide, or a
  // Ratio note's ratio to its root Degree note) multiplies the node's pitch; with node ≥ 1 a lane is this node's only
  // when its select (voice.tuning) is the node number, and a selected lane without a degree follows the reference.
  const tun::MosParams mp = mos(7, 1200.0, 1, 0, 0.0, 700.0);
  const tun::Tuning mt = mosTuning(mp);
  const dsp::TuningTable tab = table(mt);
  const dsp::TuningTableView& v = tab.view();
  constexpr uint32_t kF = 256;
  const tun::Mos m = tun::Mos::make(mp).value();
  auto staticHz = [&](int32_t d) { return kRef * std::exp2(m.centsOf(d, 0) / 1200.0); };
  // Lanes: 0 a plain Degree note, 1 the same gliding up 100 cents over the block, 2 a 3/2 on degree 1, 3 a Cents
  // note (no degree) of this node, 4 a Degree note of another node, 5 a static note (select 0), 6 / 7 like 0.
  Pattern pd{1, 1, 1, kNaN, 1, kNaN, 2, 3}, pe{0, 0, 0, kNaN, 0, kNaN, 0, 0}, ps{2, 2, 2, 2, 1, 0, 2, 2}, pn(2.0f);
  const float h0 = static_cast<float>(staticHz(1));
  const float cHz = static_cast<float>(kRef * std::exp2(200.0 / 1200.0));
  std::vector<float> fh(kF * 8), fc(kF * 8), hz(kF * 8), cents(kF * 8);
  for (uint32_t f = 0; f < kF; ++f) {
    const double glide = std::exp2(100.0 / 1200.0 * f / (kF - 1));
    const float lane[8] = {h0,
                           static_cast<float>(h0 * glide),
                           static_cast<float>(staticHz(1) * 1.5),
                           cHz,
                           h0,
                           333.0f,
                           static_cast<float>(staticHz(2)),
                           static_cast<float>(staticHz(3))};
    for (uint32_t l = 0; l < 8; ++l) {
      fh[f * 8 + l] = lane[l];
      fc[f * 8 + l] = static_cast<float>(1200.0 * std::log2(lane[l] / kRef));
    }
  }
  // Modulated: generator 710 (constant: the per-block path; and as an audio-rate buffer: the per-sample path), the
  // reference up 2 %.
  const float g = 710.0f, R = static_cast<float>(kRef * 1.02);
  const Pattern pg(g), pp(1200.0f), pst(0.0f), pr(R);
  std::vector<float> gb(kF * 8, g);
  const auto at710 = tuning_ref::mosAt(mp, 710.0);
  REQUIRE(at710);
  auto node = [&](int32_t d) { return static_cast<double>(R) * std::exp2(at710->centsOf(d, 0) / 1200.0); };
  for (const bool perSample : {false, true}) {
    CAPTURE(perSample);
    const KIn in[8] = {{pd.v, true}, {pe.v, true}, {fh.data(), false}, {fc.data(), false},
                       {perSample ? gb.data() : pg.v, !perSample}, {pp.v, true}, {pst.v, true}, {pr.v, true}};
    runKernel(8, &v, in, kF, hz.data(), cents.data(), ps.v, pn.v);
    uint32_t bad = 0;
    for (uint32_t f = 0; f < kF; ++f) {
      const double glide = std::exp2(100.0 / 1200.0 * f / (kF - 1));
      auto near = [](float got, double want, double rel) { return std::fabs(got / want - 1.0) <= rel; };
      bad += !tuning_ref::withinTolerance(hz[f * 8 + 0], node(1));             // exact: its own static pitch
      bad += !near(hz[f * 8 + 1], node(1) * glide, 3e-7);                       // the glide on top
      bad += !near(hz[f * 8 + 2], node(1) * 1.5, 3e-7);                         // 3/2 of the moved root
      bad += !near(hz[f * 8 + 3], static_cast<double>(cHz) * (R / kRef), 2e-7);  // the reference's move
      bad += hz[f * 8 + 4] != h0;                                               // another node's voice
      bad += hz[f * 8 + 5] != 333.0f;                                           // a static note
      bad += !tuning_ref::withinTolerance(hz[f * 8 + 6], node(2));
      // voice.cents: the input's, moved by the tuning (and the reference).
      bad += std::fabs(cents[f * 8 + 3] - (fc[f * 8 + 3] + 1200.0 * std::log2(R / kRef))) > 2e-4;
      bad += cents[f * 8 + 5] != fc[f * 8 + 5];
      bad += std::fabs(cents[f * 8 + 1] - (fc[f * 8 + 1] + 1200.0 * std::log2(node(1) / staticHz(1)))) > 2e-4;
    }
    CHECK(bad == 0);
  }
  // The 1-lane kernel, lane 1 (the glide) as a global signal: the same pitch per sample.
  std::vector<float> d1(kF, 1.0f), e1(kF, 0.0f), s1(kF, 2.0f), n1(kF, 2.0f), h1(kF), c1(kF), oh(kF), oc(kF);
  for (uint32_t f = 0; f < kF; ++f) {
    h1[f] = fh[f * 8 + 1];
    c1[f] = fc[f * 8 + 1];
  }
  const KIn in1[8] = {{d1.data(), false}, {e1.data(), false}, {h1.data(), false}, {c1.data(), false},
                      {pg.v, true},       {pp.v, true},       {pst.v, true},      {pr.v, true}};
  runKernel(1, &v, in1, kF, oh.data(), oc.data(), s1.data(), n1.data());
  uint32_t bad1 = 0;
  for (uint32_t f = 0; f < kF; ++f) bad1 += std::fabs(oh[f] / (node(1) * std::exp2(100.0 / 1200.0 * f / (kF - 1))) - 1.0) > 3e-7;
  CHECK(bad1 == 0);
}
