// T2.6: tuning as a signal — the per-region linear form (tuning::linearTuningForm → dsp::TuningTable via
// engine::makeTuningTable) against buildMOSScale, and the tuning_hz kernel: generator sweeps through several
// regions (per sample and block-rate), modulated period / stretch / reference, the exact unmodulated pitch, the
// fallbacks, and the allocation trap.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/tuning_table.h"
#include "pq/engine/tuning_signal.h"
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

// Index of the constant-order interval holding g (for crossing detection in the tests).
int64_t intervalOf(const dsp::TuningTableView& v, double g, double P) {
  const dsp::TuningCoefficients c = dsp::tuningCoefficients(v, 1, g, P);
  return static_cast<int64_t>(std::llround(c.lo * 1e6));
}

// Runs the kernel over `frames` frames with the given inputs (pointer + Spl flag each), into hz / cents.
struct KIn {
  const float* p;
  bool spl;
};
void runKernel(uint32_t lanes, const dsp::TuningTableView* v, const KIn (&in)[8], uint32_t frames, float* hz,
               float* cents) {
  uint32_t mask = 0;
  const float* ptr[8];
  for (uint32_t i = 0; i < 8; ++i) {
    ptr[i] = in[i].p;
    mask |= in[i].spl ? 1u << i : 0u;
  }
  float* out[2] = {hz, cents};
  dsp::KernelArgs a;
  a.in = ptr;
  a.out = out;
  a.resource = v;
  a.frames = frames;
  a.numIn = 8;
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
  const Sweep sweeps[] = {
      {mos(7, 1200.0, 1, 2, 2.5, 700.0), 450.0, 850.0},  // crosses g/P = 2/5, 1/2, 3/5, 2/3 (and 7edo, 5edo inside)
      {mos(5, 600.0, 2, 1, 0.0, 120.0), 50.0, 280.0},     // two periods per equave; crosses 1/4, 1/3
      {mos(12, 1200.0, 1, 5, 0.0, 700.0), 660.0, 740.0},  // a 12-note chain: crosses 5/9, 4/7, 3/5
  };
  const int32_t deg[8] = {0, 1, 2, 3, 4, 6, 5, -1};
  const int32_t eqv[8] = {0, 0, 0, -1, 1, 0, 2, 0};
  constexpr uint32_t kBlock = 128;
  for (const Sweep& s : sweeps) {
    const dsp::TuningTable tab = table(mosTuning(s.p));
    const dsp::TuningTableView& v = tab.view();
    Pattern pd(0.0f), pe(0.0f);
    for (uint32_t l = 0; l < 8; ++l) {
      pd.v[l] = static_cast<float>(deg[l]);
      pe.v[l] = static_cast<float>(eqv[l]);
    }
    const Pattern pp(static_cast<float>(s.p.period)), pst(static_cast<float>(s.p.octaveStretch)),
        pr(static_cast<float>(kRef));
    std::vector<float> gen(frames * 8), fallHz(frames * 8, 440.0f), fallC(frames * 8, 0.0f);
    for (uint32_t t = 0; t < frames; ++t)
      std::fill_n(gen.begin() + t * 8, 8, static_cast<float>(s.g0 + (s.g1 - s.g0) * t / (frames - 1)));
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
      uint64_t checked = 0, skipped = 0, jumps = 0, crossings = 0, badHz = 0, badCents = 0, badJumps = 0;
      int64_t firstBad = -1;
      std::optional<tun::Mos> ref;
      for (uint32_t t = 0; t < frames; ++t) {
        const uint32_t tg = blockRate ? t / kBlock * kBlock : t;  // the sample whose generator this one used
        const double g = static_cast<double>(gen[tg * 8]);
        if (t == 0 || tg != (blockRate ? (t - 1) / kBlock * kBlock : t - 1)) ref = tuning_ref::mosAt(s.p, g);
        if (!ref) {
          skipped += 8;
          continue;
        }
        const bool crossed = t > 0 && !blockRate &&
                             intervalOf(v, static_cast<double>(gen[(t - 1) * 8]), s.p.period) != intervalOf(v, g, s.p.period);
        crossings += crossed;
        for (uint32_t l = 0; l < 8; ++l) {
          const double c = ref->centsOf(deg[l], eqv[l]);
          const double refHz = kRef * std::exp2(c / 1200.0);
          const float out = hz[t * 8 + l];
          if (!tuning_ref::withinTolerance(out, refHz)) {
            ++badHz;
            if (firstBad < 0) firstBad = t;
          }
          badCents += std::fabs(cents[t * 8 + l] - c) > std::fabs(c) * 1.2e-7 + 1e-9;
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
                         << " voice jumps, " << skipped << " boundary voice-samples skipped");
      // The 1-lane kernel (a global degree signal) on the same sweep: lane 4's degree, degree and generator as
      // audio-rate buffers, from a pre-onset start (hz 0 for the first 5 frames).
      if (!blockRate) {
        std::vector<float> d1(frames, static_cast<float>(deg[4])), e1(frames, static_cast<float>(eqv[4])),
            g1(frames), h1(frames, 440.0f), c1(frames, 0.0f), oh(frames), oc(frames);
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
      if (!blockRate) CHECK(jumps >= crossings);
      CHECK(skipped * 1000 < checked);
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
    std::vector<float> gen(kFrames * 8), per(kFrames * 8), st(kFrames * 8), ref(kFrames * 8), fh(kFrames * 8, 1.0f),
        fc(kFrames * 8, 0.0f), hz(kFrames * 8), cents(kFrames * 8);
    for (uint32_t t = 0; t < kFrames; ++t)
      for (uint32_t l = 0; l < 8; ++l) {
        const double u = static_cast<double>(t) / kFrames;
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
    // degree, lane 4 starts at frame 10 (voice.hz 0 before).
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
        // Every tuning input at its definition value: the note's own static Hz, bit for bit; cents still the node's.
        CHECK(hz[f * 8 + l] == stat[l]);
        CHECK(std::fabs(std::exp2(cents[f * 8 + l] / 1200.0) * v.referenceHz - statD[l]) <= statD[l] * 2e-7);
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

  // No table: the fallback passes through. A degree that changes mid-block takes effect at that sample.
  {
    const dsp::TuningTable tab = table(edo);
    constexpr uint32_t kF = 32;
    std::vector<float> d(kF * 8), e(kF * 8, 0.0f), fh(kF * 8, 200.0f), fc(kF * 8, 1.0f), hz(kF * 8), c(kF * 8);
    for (uint32_t f = 0; f < kF; ++f)
      for (uint32_t l = 0; l < 8; ++l) d[f * 8 + l] = f < 13 ? 0.0f : (l == 2 ? kNaN : 22.0f);
    const Pattern pp(1200.0f), z(0.0f), pr(261.626f);
    const KIn in[8] = {{d.data(), false}, {e.data(), false}, {fh.data(), false}, {fc.data(), false},
                       {z.v, true}, {pp.v, true}, {z.v, true}, {pr.v, true}};
    runKernel(8, nullptr, in, kF, hz.data(), c.data());
    for (uint32_t i = 0; i < kF * 8; ++i) CHECK(hz[i] == 200.0f);
    runKernel(8, &tab.view(), in, kF, hz.data(), c.data());
    for (uint32_t f = 0; f < kF; ++f)
      for (uint32_t l = 0; l < 8; ++l) {
        const float want = f < 13 ? 261.626f : (l == 2 ? 200.0f : 523.252f);
        CHECK(hz[f * 8 + l] == doctest::Approx(want).epsilon(1e-6));
      }
  }
}
