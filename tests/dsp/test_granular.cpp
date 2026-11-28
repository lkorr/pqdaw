// SM.6: the `granular` atom (pq/dsp/granular.h), its kernel called as the backends do (1 and 8 lanes):
//   - a reference: a scalar double model of the documented semantics (spawn times per mode, the slot grid, sprays,
//     harmonic ratios, shapes, cubic reads at the grain's mip level, equal-power pan) matches within 2e-5
//   - repeatable: the same samples at call lengths 1, 37, 64, 256 and in either lane layout
//   - silence: an untriggered lane writes zeros and holds no grain; a triggered one plays until its grains end
//   - the grain tap: nothing is written without a reader; with one, each call's record holds the live grains
//   - RtScope: 10 000 calls of triggers, mode changes and sprays allocate nothing
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/core/grain_tap.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/grain_schedule.h"
#include "pq/dsp/granular.h"
#include "pq/dsp/resample.h"
#include "pq/dsp/sample_slot.h"

using namespace pq;
using namespace pq::dsp;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kIn = 19;

std::vector<float> tone(size_t n, double hz, double phase) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i)
    v[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kSr + phase) +
                              0.1 * std::sin(0.00071 * static_cast<double>(i)));
  return v;
}

struct Src {
  EpochReclaimer reclaimer{1};
  SampleSlot slot;
  std::vector<float> l, r;
  std::shared_ptr<const SampleData> data;
  Src(size_t n, double fileRate, std::vector<float> harmonics = {}) : l(tone(n, 210.0, 0.0)), r(tone(n, 330.0, 1.0)) {
    data = SampleData::build(l, r, 3).value();
    SampleSource s = SampleData::source(data, fileRate);
    s.harmonics = std::move(harmonics);
    REQUIRE(slot.publish(std::move(s), kSr, reclaimer).ok());
  }
};

// The atom's inputs (in kernel order after hz, reset).
struct Params {
  double root = 261.63, ratio = 1.0, start = 0.1, end = 0.9, position = 0.2, scan = 1.0, size = 0.05, density = 40.0;
  double sprayTime = 0.0, sprayPitch = 0.0, sprayPan = 0.0, freeze = 0.0;
  double shape = 0.0, mode = 1.0, grains = 16.0, harmonic = 0.0;
  double movedPosition = -1.0;  // GV.1: from the first call at or after frame movedAt, Position is this (< 0: never)
  size_t movedAt = 0;
};

// Calls the kernel over `frames` in calls of `block`: hz(t) per sample, a trigger at each of `triggers`.
struct Run {
  std::vector<float> l, r, pos;
  uint64_t violations = 0;
};
template <class Hz>
Run run(const Src& src, const Params& p, uint32_t lanes, uint32_t lane, size_t frames, uint32_t block, Hz hzAt,
        const std::vector<size_t>& triggers, GrainTap* tap = nullptr, bool trap = false) {
  (void)tap;
  const uint32_t L = lanes;
  std::vector<AlignedBuffer<float>> in;
  for (uint32_t i = 0; i < kIn; ++i) in.emplace_back(static_cast<size_t>(256) * 8);
  const double vals[kIn] = {0, 0, p.root, p.ratio, p.start, p.end, p.position, p.scan, p.size, p.density, p.sprayTime,
                            p.sprayPitch, p.sprayPan, p.freeze, p.shape, p.mode, p.grains, p.harmonic, kSr};
  for (uint32_t i = 2; i < kIn; ++i)
    for (uint32_t k = 0; k < 8; ++k) in[i][k] = static_cast<float>(vals[i]);
  std::vector<AlignedBuffer<float>> out;
  for (uint32_t o = 0; o < 4; ++o) out.emplace_back(static_cast<size_t>(256) * 8);
  const float* ip[kIn];
  for (uint32_t i = 0; i < kIn; ++i) ip[i] = in[i].data();
  float* op[4] = {out[0].data(), out[1].data(), out[2].data(), out[3].data()};
  AlignedBuffer<float> state(sizeof(GranularState) / sizeof(float) + 8);
  StateInit init;
  init.lanes = L;
  init.seed = 1234;
  initState(Atom::Granular, state.data(), init);
  const uint32_t mask = 0u | kernelInfo(Atom::Granular).splRequired;  // hz and reset per sample
  const KernelFn fn = findKernel(Atom::Granular, L, mask);
  REQUIRE(fn != nullptr);
  Run o;
  o.l.resize(frames), o.r.resize(frames), o.pos.resize(frames);
  DenormalGuard dg;
  for (size_t t0 = 0; t0 < frames; t0 += block) {
    const auto n = static_cast<uint32_t>(std::min<size_t>(block, frames - t0));
    if (p.movedPosition >= 0.0 && t0 >= p.movedAt)
      for (uint32_t k = 0; k < 8; ++k) in[6][k] = static_cast<float>(p.movedPosition);
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t k = 0; k < L; ++k) {
        const bool on = k == lane && std::find(triggers.begin(), triggers.end(), t0 + i) != triggers.end();
        in[0][i * L + k] = static_cast<float>(hzAt(t0 + i));
        in[1][i * L + k] = boolMask(on);
      }
    KernelArgs args{ip, op, state.data(), &src.slot, n, kIn, 4, mask};
    if (trap) o.violations += alloc_trap::countRtViolations([&] { fn(args); });
    else fn(args);
    for (uint32_t i = 0; i < n; ++i) {
      o.l[t0 + i] = out[0][i * L + lane];
      o.r[t0 + i] = out[2][i * L + lane];
      o.pos[t0 + i] = out[3][i * L + lane];
      for (uint32_t k = 0; k < L; ++k)
        if (k != lane) {
          REQUIRE(out[0][i * L + k] == 0.0f);  // the other lanes: no voice, silent
          REQUIRE(out[2][i * L + k] == 0.0f);
        }
    }
  }
  return o;
}

// ---- the reference --------------------------------------------------------------------------------------------------
struct RefGrain {
  int64_t pos = 0, step = 0;  // 32.32 at its level
  uint32_t level = 0;
  int64_t start = 0, len = 0;  // voice samples
  double gl = 0, gr = 0;
};

double cubicAt(const float* x, int64_t i, double f) {
  const double xm1 = x[i - 1], x0 = x[i], x1 = x[i + 1], x2 = x[i + 2];
  const double c1 = 0.5 * (x1 - xm1), c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2, c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
  return ((c3 * f + c2) * f + c1) * f + x0;
}
int64_t fx(double x) { return std::llround(x * 4294967296.0); }

// The params as the kernel sees them (float32 inputs).
Params rounded(const Params& p) {
  auto f = [](double x) { return static_cast<double>(static_cast<float>(x)); };
  Params q = p;
  for (double* x : {&q.root, &q.ratio, &q.start, &q.end, &q.position, &q.scan, &q.size, &q.density, &q.sprayTime,
                    &q.sprayPitch, &q.sprayPan, &q.freeze, &q.shape, &q.mode, &q.grains, &q.harmonic})
    *x = f(*x);
  return q;
}

template <class Hz>
void reference(const Src& src, const Params& pIn, uint32_t voiceSlot, size_t frames, Hz hzAt, std::vector<double>& L,
               std::vector<double>& R) {
  const Params p = rounded(pIn);
  const SampleSlot::Entry& e = *src.slot.acquire();
  const double len0 = e.level[0].length;
  const double startF = p.start * len0, endF = p.end * len0;  // (as the kernel: clamped to [0, 1] first)
  const int64_t s32 = fx(startF), e32 = fx(endF);
  auto wrap = [&](int64_t x) {
    const int64_t Lr = e32 - s32;
    int64_t d = (x - s32) % Lr;
    if (d < 0) d += Lr;
    return s32 + d;
  };
  const float* shape = grainShapeTable(static_cast<GrainShape>(static_cast<int>(p.shape)));
  const auto mode = static_cast<GranularMode>(static_cast<int>(p.mode));
  const int64_t scanStep = p.freeze >= 0.5 ? 0 : fx(p.scan * e.rateScale);
  std::vector<RefGrain> slots(static_cast<size_t>(p.grains));
  std::vector<bool> used(slots.size(), false);
  int64_t pos = fx(startF + p.position * (endF - startF));
  uint32_t harm = 0;
  const uint32_t defH[] = {1, 1, 2, 1, 3, 2, 3, 1, 4, 1, 5, 1};
  L.assign(frames, 0.0);
  R.assign(frames, 0.0);
  // BR.J: the onsets, lengths and slots from the shared spawn law (pq/dsp/grain_schedule.h): this model matching the
  // kernel's output is what ties that law to the kernel (the Sampler face draws its grainstream from it).
  auto hzOf = [&](int64_t t) { return std::max(static_cast<double>(static_cast<float>(hzAt(static_cast<size_t>(t)))), 1.0); };
  std::vector<GrainSpawn> spawns;
  GrainSpawnLaw law;
  law.mode = mode;
  law.density = p.density;
  law.size = p.size;
  law.grains = static_cast<uint32_t>(p.grains);
  law.sr = kSr;
  law.seed = 1234;
  law.voiceSlot = voiceSlot;
  (void)grainSchedule(law, static_cast<int64_t>(frames), hzOf, [&](const GrainSpawn& g) { spawns.push_back(g); });
  size_t nextSpawn = 0;
  for (int64_t t = 0; t < static_cast<int64_t>(frames); ++t) {
    for (; nextSpawn < spawns.size() && spawns[nextSpawn].start == t; ++nextSpawn) {  // the spawns at this sample
      const GrainSpawn& sp = spawns[nextSpawn];
      if (sp.skipped()) continue;
      const uint32_t k = sp.k;
      auto rnd = [&](uint32_t d) { return 2.0 * static_cast<double>(grainUniform(1234, voiceSlot, k, d)) - 1.0; };
      const double hz = hzOf(t);
      const size_t q = sp.slot;
      double rate = (mode == GranularMode::Classic ? p.ratio : hz / p.root * p.ratio) * e.rateScale;
      if (p.harmonic >= 0.5) {
        const uint32_t nh = e.harmonicCount > 0 ? e.harmonicCount : 6u;
        const uint32_t hi = harm++ % nh;
        const double n = e.harmonicCount > 0 ? e.harmonics[2 * hi] : defH[2 * hi];
        const double d = e.harmonicCount > 0 ? e.harmonics[2 * hi + 1] : defH[2 * hi + 1];
        rate = rate * n / d;
      }
      if (p.sprayPitch > 0) rate *= std::exp2(rnd(2) * p.sprayPitch / 12.0);
      rate = std::clamp(rate, 1.0 / 64.0, 64.0);
      const double st = static_cast<double>(pos) / 4294967296.0 + rnd(1) * p.sprayTime * e.fileRate;
      RefGrain g;
      g.level = mipLevelFor(rate, e.levels);
      const double sc = std::ldexp(1.0, -static_cast<int>(g.level));
      g.pos = fx(st * sc);
      g.step = fx(rate * sc);
      g.start = t;
      g.len = sp.len;
      const double pan = p.sprayPan > 0 ? rnd(3) * p.sprayPan : 0.0;
      const double th = (pan + 1.0) * std::numbers::pi / 4.0;
      g.gl = static_cast<float>(std::cos(th) * std::numbers::sqrt2);
      g.gr = static_cast<float>(std::sin(th) * std::numbers::sqrt2);
      slots[q] = g;
      used[q] = true;
    }
    for (size_t j = 0; j < slots.size(); ++j) {
      RefGrain& g = slots[j];
      if (!used[j] || t < g.start || t >= g.start + g.len) continue;
      const int64_t age = t - g.start;
      const SampleLevelView& lv = e.level[g.level];
      const int64_t i = std::clamp<int64_t>(g.pos >> 32, -kSampleSlotGuard + 1, lv.length + kSampleSlotGuard - 3);
      const double f = static_cast<double>(static_cast<uint32_t>(g.pos & 0xFFFFFFFF) >> 8) * 0x1p-24;
      const float winc = static_cast<float>(kGrainShapePoints) / static_cast<float>(g.len);
      const double wx = static_cast<double>(static_cast<float>(age) * winc);
      const auto w0 = std::clamp<int64_t>(static_cast<int64_t>(wx), 0, kGrainShapePoints - 1);
      const double w = shape[w0] + (wx - static_cast<double>(w0)) * (shape[w0 + 1] - shape[w0]);
      L[static_cast<size_t>(t)] += cubicAt(lv.ch[0], i, f) * w * g.gl;
      R[static_cast<size_t>(t)] += cubicAt(lv.ch[1], i, f) * w * g.gr;
      g.pos += g.step;
    }
    pos = wrap(pos + scanStep);
  }
}

double worstDiff(const std::vector<float>& a, const std::vector<double>& b) {
  double w = 0;
  for (size_t i = 0; i < a.size(); ++i) w = std::max(w, std::abs(static_cast<double>(a[i]) - b[i]));
  return w;
}

}  // namespace

TEST_CASE("SM.6 granular: Classic / Loop / Cloud with sprays, shapes and harmonic grains match the reference") {
  Src src(96000, 44100.0, {1.0f, 1.0f, 3.0f, 2.0f, 5.0f, 4.0f});
  auto glide = [](size_t t) { return 220.0 * (1.0 + 0.5 * std::min(1.0, static_cast<double>(t) / 30000.0)); };
  struct Case {
    double mode, shape, sprayT, sprayP, sprayPan, harmonic, freeze, scan;
  };
  for (const Case c : {Case{1, 0, 0, 0, 0, 0, 0, 1}, Case{2, 1, 0.02, 3, 0.8, 0, 0, 0.5}, Case{0, 2, 0.01, 0, 0.5, 0, 0, 1},
                       Case{1, 3, 0, 0, 0, 1, 0, -0.7}, Case{2, 4, 0.05, 7, 1, 1, 1, 1}}) {
    CAPTURE(c.mode);
    CAPTURE(c.shape);
    Params p;
    p.mode = c.mode, p.shape = c.shape, p.sprayTime = c.sprayT, p.sprayPitch = c.sprayP, p.sprayPan = c.sprayPan;
    p.harmonic = c.harmonic, p.freeze = c.freeze, p.scan = c.scan;
    p.size = c.mode == 0 ? 0.012 : 0.06;
    std::vector<double> rl, rr;
    reference(src, p, 0, 40000, glide, rl, rr);
    const Run k1 = run(src, p, 1, 0, 40000, 64, glide, {0});
    CHECK(worstDiff(k1.l, rl) < 2e-5);
    CHECK(worstDiff(k1.r, rr) < 2e-5);
    double peak = 0;
    for (double x : rl) peak = std::max(peak, std::abs(x));
    CHECK(peak > 0.1);
  }
}

// BR.J (bug 2026-11-27_21-16-56: the grainstream display drifted from the kernel): the shared spawn law
// (pq/dsp/grain_schedule.h, which the face draws) where it matters most — at the slot cap, where whole-sample onsets and
// grid-cell slot freeing decide which spawns are skipped. The kernel's output must equal the model spawning by it.
TEST_CASE("BR.J granular: at the grain cap the kernel skips exactly the spawns the shared schedule skips") {
  Src src(48000, 48000.0);
  struct Case {
    double mode, grains, size, density;
  };
  for (const Case c : {Case{1.0, 2.0, 0.05, 41.3}, Case{1.0, 4.0, 0.0101, 400.0}, Case{2.0, 3.0, 0.05, 70.0}, Case{0.0, 1.0, 0.004, 0.0}}) {
    CAPTURE(c.mode);
    CAPTURE(c.density);
    Params p;
    p.mode = c.mode, p.grains = c.grains, p.size = c.size, p.density = c.density > 0.0 ? c.density : 30.0;
    auto hz = [](size_t t) { return 300.0 + 40.0 * std::sin(static_cast<double>(t) * 2e-4); };
    std::vector<double> rl, rr;
    reference(src, p, 0, 24000, hz, rl, rr);
    for (const uint32_t block : {64u, 37u}) {
      const Run k = run(src, p, 1, 0, 24000, block, hz, {0});
      CHECK(worstDiff(k.l, rl) < 2e-5);
      CHECK(worstDiff(k.r, rr) < 2e-5);
    }
    // The case is at the cap: some spawns skipped, most not.
    const Params q = rounded(p);
    GrainSpawnLaw law;
    law.mode = static_cast<GranularMode>(static_cast<int>(q.mode));
    law.density = q.density, law.size = q.size, law.grains = static_cast<uint32_t>(q.grains), law.seed = 1234;
    uint32_t n = 0;
    const uint32_t skipped = grainSchedule(law, 24000, [&](int64_t t) { return static_cast<double>(static_cast<float>(hz(static_cast<size_t>(t)))); },
                                           [&](const GrainSpawn&) { ++n; });
    CHECK(skipped > 0);
    CHECK(skipped < n);
  }
}

TEST_CASE("SM.6 granular: repeatable across call lengths and lane layouts; silence; retrigger") {
  Src src(60000, 48000.0);
  Params p;
  p.mode = 2.0, p.sprayTime = 0.03, p.sprayPitch = 2.0, p.sprayPan = 0.6, p.grains = 24, p.density = 90;
  auto hz = [](size_t t) { return 180.0 + 60.0 * std::sin(static_cast<double>(t) * 3e-4); };
  const std::vector<size_t> trig = {5, 21000};
  const Run ref = run(src, p, 1, 0, 30000, 64, hz, trig);
  for (const uint32_t block : {1u, 37u, 256u}) {
    CAPTURE(block);
    const Run o = run(src, p, 1, 0, 30000, block, hz, trig);
    CHECK(o.l == ref.l);
    CHECK(o.r == ref.r);
  }
  // Lane 5 of an 8-lane group: the same samples (its voice slot differs: another random stream — so compare to a
  // single lane whose slot is 5 is not possible; compare the 8-lane layout with itself across blocks instead).
  const Run g = run(src, p, 8, 5, 30000, 64, hz, trig);
  CHECK(run(src, p, 8, 5, 30000, 37, hz, trig).l == g.l);
  CHECK(g.l != ref.l);  // another voice slot draws other grains
  // Before the trigger: zeros.
  for (size_t t = 0; t < 5; ++t) CHECK(ref.l[t] == 0.0f);
  // No trigger at all: silent.
  const Run none = run(src, p, 1, 0, 4000, 64, hz, {});
  for (const float x : none.l) CHECK(x == 0.0f);
}

TEST_CASE("GV.1 granular: Position moved while a note plays scrubs at once; the scanned distance rides on it") {
  Src src(60000, 48000.0);
  Params p;  // Start 0.1, End 0.9: the region is 48 000 frames from frame 6 000
  p.scan = 0.5;
  p.movedPosition = 0.7;
  p.movedAt = 12000;  // (the first call from there: frame 12 032 in calls of 64)
  auto hz = [](size_t) { return 261.63; };
  const Run o = run(src, p, 1, 0, 20000, 64, hz, {5});
  const auto expect = [](double position, size_t t) { return (6000.0 + position * 48000.0 + 0.5 * static_cast<double>(t - 5)) / 60000.0; };
  // (pos is reported once per run of the grid: within 64 frames of scanning)
  CHECK(o.pos[11990] == doctest::Approx(expect(0.2, 11990)).epsilon(0.002));
  CHECK(o.pos[12100] == doctest::Approx(expect(0.7, 12100)).epsilon(0.002));
  CHECK(o.pos[19990] == doctest::Approx(expect(0.7, 19990)).epsilon(0.002));
  // Unmoved, the same note keeps reading from 0.2.
  p.movedPosition = -1.0;
  CHECK(run(src, p, 1, 0, 20000, 64, hz, {5}).pos[12100] == doctest::Approx(expect(0.2, 12100)).epsilon(0.002));
}

TEST_CASE("SM.6 granular: the grain tap — nothing without a reader; with one, the live grains per call") {
  Src src(48000, 48000.0);
  auto tap = std::make_shared<GrainTap>(GrainTapConfig{1, 8});
  src.slot.setGrainTap(tap, src.reclaimer);
  Params p;
  p.density = 100.0, p.size = 0.1, p.grains = 32;  // ≈ 10 grains overlap
  auto hz = [](size_t) { return 261.63; };
  (void)run(src, p, 1, 0, 9600, 64, hz, {0});
  CHECK(tap->written(0) == 0);
  {
    GrainTapReader reader(tap);
    (void)run(src, p, 1, 0, 9600, 64, hz, {0});
    CHECK(tap->written(0) == 150);  // one record per call
    REQUIRE(reader.poll(1.0) == 1);
    const GrainTapRecord* r = reader.group(0);
    REQUIRE(r != nullptr);
    CHECK(r->count >= 9);
    CHECK(r->count <= 11);
    for (uint32_t i = 0; i < r->count; ++i) {
      CHECK(r->grain[i].position >= 0.0f);
      CHECK(r->grain[i].position <= 1.0f);
      CHECK(r->grain[i].amplitude >= 0.0f);
      CHECK(r->grain[i].lane == 0);
    }
  }
  CHECK(!tap->wanted());
}

TEST_CASE("SM.6 granular: 10 000 calls of triggers, modes and sprays allocate nothing (RtScope)") {
  REQUIRE(alloc_trap::installed());
  Src src(30000, 44100.0);
  auto tap = std::make_shared<GrainTap>(GrainTapConfig{1, 8});
  src.slot.setGrainTap(tap, src.reclaimer);
  GrainTapReader reader(tap);
  std::vector<size_t> trig;
  for (size_t t = 0; t < 640000; t += 7919) trig.push_back(t);
  for (const double mode : {0.0, 1.0, 2.0}) {
    Params p;
    p.mode = mode, p.sprayTime = 0.02, p.sprayPitch = 5, p.sprayPan = 1, p.harmonic = 1, p.grains = 32, p.density = 200;
    const Run o = run(src, p, 8, 3, 640000, 64, [](size_t t) { return 100.0 + static_cast<double>(t % 9000) / 20.0; }, trig,
                      nullptr, true);
    CHECK(o.violations == 0);
  }
}
