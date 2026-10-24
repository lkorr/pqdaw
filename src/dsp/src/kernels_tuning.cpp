// The tuning node's kernel (T2.6, SPEC §3.3): tuning_hz(degree, equave, hz, cents, generator, period, stretch,
// reference) → (hz, cents), resource = TuningTableView (pq/dsp/tuning_table.h).
//
// Per lane and sample:
//   tuned   = the lane has a finite degree and equave (a Degree note in this tuning) and the table exists;
//   started = the fallback hz is not 0 (the VoiceEngine's voice.hz is 0 before a note's onset in its first block);
//   out.hz    = tuned && started && !unmodulated ? reference · 2^(cents/1200) : hz
//   out.cents = tuned && started                 ? cents (stretched, from the reference)  : cents
// where `unmodulated` (T2.6-fix1) = every tuning param input is a Spl pattern equal to float32 of the table's
// definition value (generator — not read by an EDO —, period, stretch, reference): the tuning is exactly the one the
// note's static Hz was resolved in, so the node passes that Hz through bit for bit (the project lowering resolves
// a Degree note's NoteDesc::hz in the table's definition: pq/app/project_link.h). A project whose tunings are not
// modulated therefore renders bit-identically with and without its tuning nodes, and saves the exp2 per lane.
// so a lane starting mid-block keeps the engine's "0 Hz until the onset" rule (its output from the onset on does
// not depend on block boundaries), and notes without a degree (Ratio, Cents, Harmonic, other tunings) pass through.
//
// Two paths, chosen per block from KernelArgs::splMask (this kernel reads the mask at run time instead of being
// template-specialised for its 2^8 masks; both backends set it):
//   - every tuning input (degree, equave, generator, period, stretch, reference) is a Spl pattern: the pitch of
//     each lane is computed once per block in double (the common, unmodulated case), and the per-sample work is
//     one compare and two blends per 8 lanes;
//   - otherwise, per sample in AVX2 doubles (4 lanes per __m256d): the range check of the cached (K, M) against the
//     generator (2 mul + 2 cmp), the linear form (1 FMA), the stretch, exp2 (round, 9-term polynomial, exponent
//     add) and the reference. A lane leaves the cached coefficients only at a region crossing or a degree change,
//     where its coefficients are re-derived (scalar binary search) at that very sample.
// Double precision is needed: k·g − m·P reaches ~10^4 ¢, where float32 would be ~1e-3 ¢; in double the only
// error is the final float32 rounding of the Hz value (≤ 0.5 ulp).
//
// T2.6-fix2: the coefficients come from tuningCoefficients (pq/dsp/tuning_table.h): the mode carried across
// intervals (the root's chain position), exact boundaries decided like buildMOSScale (the cached range is shrunk by
// the boundary windows, so a lane re-derives at every sample within 1e-6/q ¢ of a boundary, and the nudge enters
// the form). A table's live inputs (TuningLiveInputs: automation buffers bound by the timeline) replace the
// corresponding param inputs for a block in which they move, which makes that block take the per-sample path.

#include <immintrin.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "kernels_common.h"
#include "pq/dsp/tuning_table.h"

namespace pq::dsp {

Result<std::shared_ptr<const TuningShape>> TuningShape::build(TuningTableKind kind, int32_t n,
                                                              std::span<const double> bounds,
                                                              std::span<const int32_t> k, std::span<const int32_t> m,
                                                              std::span<const int32_t> inverse,
                                                              std::span<const int8_t> sides,
                                                              std::span<const double> windows) {
  auto bad = [](const char* why) { return Error{ErrorCode::InvalidArgument, why, {}}; };
  if (n < 1) return bad("tuning shape: n must be ≥ 1");
  if (bounds.size() < 2 || bounds.front() != 0.0 || bounds.back() != 1.0)
    return bad("tuning shape: bounds must run from 0 to 1");
  for (size_t i = 1; i < bounds.size(); ++i)
    if (!(bounds[i] > bounds[i - 1])) return bad("tuning shape: bounds must ascend");
  const size_t intervals = bounds.size() - 1;
  if (kind == TuningTableKind::Edo && intervals != 1) return bad("tuning shape: an EDO has one interval");
  const size_t cells = intervals * static_cast<size_t>(n);
  if (k.size() != cells || m.size() != cells || inverse.size() != cells)
    return bad("tuning shape: k, m and inverse must hold intervals × n entries");
  if (sides.size() != bounds.size() || windows.size() != bounds.size())
    return bad("tuning shape: sides and windows must hold one entry per boundary");
  for (size_t t = 0; t < intervals; ++t)
    for (size_t j = 0; j < static_cast<size_t>(n); ++j) {
      const int32_t c = k[t * static_cast<size_t>(n) + j];
      if (c < 0 || c >= n || inverse[t * static_cast<size_t>(n) + static_cast<size_t>(c)] != static_cast<int32_t>(j))
        return bad("tuning shape: inverse must invert k in every interval");
    }
  for (size_t i = 0; i < sides.size(); ++i)
    if (sides[i] < -1 || sides[i] > 1 || !(windows[i] >= 0.0) || !std::isfinite(windows[i]))
      return bad("tuning shape: sides must be −1, 0 or 1 and windows finite and ≥ 0");
  auto s = std::make_shared<TuningShape>();
  s->kind_ = kind;
  s->n_ = n;
  s->intervals_ = static_cast<int32_t>(intervals);
  s->bounds_ = AlignedBuffer<double>(bounds.size());
  s->k_ = AlignedBuffer<int32_t>(cells);
  s->m_ = AlignedBuffer<int32_t>(cells);
  s->inv_ = AlignedBuffer<int32_t>(cells);
  s->sides_ = AlignedBuffer<int8_t>(sides.size());
  s->windows_ = AlignedBuffer<double>(windows.size());
  std::memcpy(s->bounds_.data(), bounds.data(), bounds.size_bytes());
  std::memcpy(s->k_.data(), k.data(), k.size_bytes());
  std::memcpy(s->m_.data(), m.data(), m.size_bytes());
  std::memcpy(s->inv_.data(), inverse.data(), inverse.size_bytes());
  std::memcpy(s->sides_.data(), sides.data(), sides.size_bytes());
  std::memcpy(s->windows_.data(), windows.data(), windows.size_bytes());
  return std::shared_ptr<const TuningShape>(std::move(s));
}

size_t TuningShape::bytes() const noexcept {
  return bounds_.size() * sizeof(double) + (k_.size() + m_.size() + inv_.size()) * sizeof(int32_t) + sides_.size() +
         windows_.size() * sizeof(double);
}

Result<TuningTable> TuningTable::build(std::shared_ptr<const TuningShape> shape, const TuningDefinition& d,
                                       const TuningLiveInputs* live) {
  auto bad = [](const char* why) { return Error{ErrorCode::InvalidArgument, why, {}}; };
  if (!shape) return bad("tuning table: no shape");
  if (d.periodsPerEquave < 1) return bad("tuning table: periods per equave must be ≥ 1");
  if (d.rootChain < 0 || d.rootChain >= shape->n()) return bad("tuning table: root chain position outside [0, n)");
  if (!std::isfinite(d.generator) || !std::isfinite(d.period) || !(d.period > 0.0) || !std::isfinite(d.stretch) ||
      !(d.stretch > -1200.0) || !std::isfinite(d.referenceHz) || !(d.referenceHz > 0.0))
    return bad("tuning table: invalid generator / period / stretch / reference");
  TuningTable t;
  TuningTableView& v = t.view_;
  v.kind = shape->kind_;
  v.n = shape->n_;
  v.periodsPerEquave = d.periodsPerEquave;
  v.intervals = shape->intervals_;
  v.bounds = shape->bounds_.data();
  v.k = shape->k_.data();
  v.m = shape->m_.data();
  v.inv = shape->inv_.data();
  v.sides = shape->sides_.data();
  v.windows = shape->windows_.data();
  v.rootChain = v.kind == TuningTableKind::Edo ? 0 : d.rootChain;
  v.hasHint = d.hasHint;
  v.hint = d.hint;
  v.generator = d.generator;
  v.period = d.period;
  v.stretch = d.stretch;
  v.referenceHz = d.referenceHz;
  v.generatorF = static_cast<float>(d.generator);
  v.periodF = static_cast<float>(d.period);
  v.stretchF = static_cast<float>(d.stretch);
  v.referenceF = static_cast<float>(d.referenceHz);
  v.live = live;
  t.shape_ = std::move(shape);
  return t;
}

namespace detail {
namespace {

constexpr double kInv1200 = 1.0 / 1200.0;
constexpr double kExpClamp = 512.0;  // octaves: keeps the exponent add in range; float32 Hz saturates long before

// 2^f for |f| ≤ 0.5: Taylor series of e^(f·ln2) to degree 10 (truncation < 1e-13 relative, far below the final
// float32 rounding). Coefficients ln2^i / i!.
constexpr double kC[11] = {1.0,
                           0.69314718055994530942,
                           0.24022650695910071233,
                           0.05550410866482157995,
                           0.00961812910762847716,
                           0.00133335581464284434,
                           0.00015403530393381609,
                           0.00001525273380405984,
                           0.00000132154867901443,
                           0.00000010178086009240,
                           0.00000000705491162080};

inline double exp2Scalar(double x) noexcept {  // RT-safe; same arithmetic per lane as exp2Vec
  x = x < -kExpClamp ? -kExpClamp : x > kExpClamp ? kExpClamp : x;
  const double nn = std::nearbyint(x);
  const double f = x - nn;
  double p = kC[10];
  for (int i = 9; i >= 0; --i) p = std::fma(p, f, kC[i]);
  const int64_t bits = std::bit_cast<int64_t>(p) + (static_cast<int64_t>(nn) << 52);
  return std::bit_cast<double>(bits);
}

inline __m256d exp2Vec(__m256d x) noexcept {  // RT-safe
  x = _mm256_max_pd(_mm256_min_pd(x, _mm256_set1_pd(kExpClamp)), _mm256_set1_pd(-kExpClamp));
  const __m256d nn = _mm256_round_pd(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
  const __m256d f = _mm256_sub_pd(x, nn);
  __m256d p = _mm256_set1_pd(kC[10]);
  for (int i = 9; i >= 0; --i) p = _mm256_fmadd_pd(p, f, _mm256_set1_pd(kC[i]));
  const __m256i e = _mm256_slli_epi64(_mm256_cvtepi32_epi64(_mm256_cvtpd_epi32(nn)), 52);
  return _mm256_castsi256_pd(_mm256_add_epi64(_mm256_castpd_si256(p), e));
}

// A Spl input equal to float32 of the definition's value reads the double value (see tuning_table.h).
inline double exactOr(float in, float f, double d) noexcept { return in == f ? d : static_cast<double>(in); }

// Every tuning param input at float32 of the definition's value (an EDO does not read the generator): the node
// passes the note's own static Hz through (see the file comment).
inline bool unmodulated(const TuningTableView& v, float gen, float per, float st, float ref) noexcept {
  return (v.kind == TuningTableKind::Edo || gen == v.generatorF) && per == v.periodF && st == v.stretchF &&
         ref == v.referenceF;
}

// An input: the value of lane `lane` at frame t is p[t·ts + lane·ls]. A Spl pattern: ts = 0, ls = 1; a block buffer:
// ts = lanes, ls = 1; a live automation buffer (global, per sample: TuningLiveInputs): ts = 1, ls = 0.
struct In8 {
  const float* p;
  uint32_t ts;
  uint32_t ls;
  float at(uint32_t t, uint32_t lane) const noexcept { return p[t * ts + lane * ls]; }
  bool constant() const noexcept { return ts == 0; }
};

// The tuning param inputs of this block: the node's inputs, with the live automation buffers in place of those
// that are moving this block (an EDO reads neither the generator nor the stretch).
struct TuningIns {
  In8 gen, per, st, ref;
};
inline TuningIns tuningIns(const TuningTableView* v, const In8& gen, const In8& per, const In8& st,
                           const In8& ref) noexcept {
  TuningIns r{gen, per, st, ref};
  if (v != nullptr && v->live != nullptr) {
    const TuningLiveInputs& l = *v->live;
    const bool edo = v->kind == TuningTableKind::Edo;
    if (l.generator != nullptr && !edo) r.gen = In8{l.generator, 1, 0};
    if (l.period != nullptr) r.per = In8{l.period, 1, 0};
    if (l.stretch != nullptr && !edo) r.st = In8{l.stretch, 1, 0};
    if (l.reference != nullptr) r.ref = In8{l.reference, 1, 0};
  }
  return r;
}

// One lane's setup from its degree/equave: tuned flag and absolute degree.
inline bool laneDegree(const TuningTableView& v, float deg, float eq, int64_t& D) noexcept {
  if (!(std::isfinite(deg) && std::isfinite(eq))) return false;
  // |values| ≤ 2^24 keep the products exact; a degree beyond that is not a note.
  if (std::fabs(deg) > 16777216.0f || std::fabs(eq) > 16777216.0f) return false;
  const int64_t N = static_cast<int64_t>(v.n) * v.periodsPerEquave;
  D = static_cast<int64_t>(std::floor(eq)) * N + static_cast<int64_t>(std::floor(deg));
  return true;
}

// Scalar pitch of a tuned lane (the constant-input path and the 1-lane kernel): stretched cents and Hz. `ownHz`: the
// lane passes its static Hz through (unmodulated), so only the cents are needed (hz = 1: valid, unused).
inline void lanePitch(const TuningTableView& v, int64_t D, double g, double P, double st, double ref, double& cents,
                      double& hz, bool ownHz = false) noexcept {
  if (v.kind == TuningTableKind::Edo) g = P / static_cast<double>(v.n);
  const TuningCoefficients c = tuningCoefficients(v, D, g, P);
  const double pure = std::fma(c.K, g + c.nudge, -(c.M * P));
  cents = pure * (1200.0 + st) * kInv1200;
  hz = ownHz ? 1.0 : ref * exp2Scalar(cents * kInv1200);
}

inline bool finitePositive(double hz) noexcept { return hz > 0.0 && hz <= 3.4e38; }

// The cached coefficients still hold for generator g at period P.
inline bool inRange(const TuningCoefficients& c, double g, double P) noexcept {
  return g >= std::fma(c.lo, P, c.loMargin) && g < std::fma(c.hi, P, -c.hiMargin);
}

// ---- 1 lane ----------------------------------------------------------------------------------------------------

void tuningHz1(const KernelArgs& a) noexcept {  // RT-safe
  const auto* v = static_cast<const TuningTableView*>(a.resource);
  const uint32_t mask = a.splMask;
  auto in = [&](uint32_t i) { return In8{a.in[i], ((mask >> i) & 1u) ? 0u : 1u, 1u}; };
  const In8 deg = in(0), eq = in(1), hz = in(2), ce = in(3);
  const TuningIns ti = tuningIns(v, in(4), in(5), in(6), in(7));
  const In8 &gen = ti.gen, &per = ti.per, &st = ti.st, &ref = ti.ref;
  float* oh = a.out[0];
  float* oc = a.out[1];
  const auto pick = [](const In8& x, uint32_t tt, float f, double d) {
    return x.constant() ? exactOr(x.p[0], f, d) : static_cast<double>(x.at(tt, 0));
  };
  if (v != nullptr && deg.constant() && eq.constant() && gen.constant() && per.constant() && st.constant() &&
      ref.constant()) {
    // Every tuning input is constant over the block: one pitch.
    int64_t D = 0;
    double cents = 0.0, outHz = 0.0;
    const bool ownHz = unmodulated(*v, gen.p[0], per.p[0], st.p[0], ref.p[0]);  // the note's static Hz is exact
    bool ok = laneDegree(*v, deg.p[0], eq.p[0], D);
    if (ok) {
      lanePitch(*v, D, pick(gen, 0, v->generatorF, v->generator), pick(per, 0, v->periodF, v->period),
                pick(st, 0, v->stretchF, v->stretch), pick(ref, 0, v->referenceF, v->referenceHz), cents, outHz,
                ownHz);
      ok = finitePositive(outHz);
    }
    const float th = static_cast<float>(outHz), tc = static_cast<float>(cents);
    for (uint32_t t = 0; t < a.frames; ++t) {
      const float h = hz.at(t, 0);
      const bool use = ok && h != 0.0f;
      oh[t] = use && !ownHz ? th : h;
      oc[t] = use ? tc : ce.at(t, 0);
    }
    return;
  }
  // Per sample, with the coefficients cached until the generator leaves their range or the degree changes.
  TuningCoefficients co;
  int64_t D = 0;
  bool tuned = false, have = false;
  uint32_t degBits = 0, eqBits = 0;
  for (uint32_t t = 0; t < a.frames; ++t) {
    const float h = hz.at(t, 0);
    const float c = ce.at(t, 0);
    if (v == nullptr || h == 0.0f) {
      oh[t] = h;
      oc[t] = c;
      continue;
    }
    const double P = pick(per, t, v->periodF, v->period);
    const double g = v->kind == TuningTableKind::Edo ? P / static_cast<double>(v->n)
                                                     : pick(gen, t, v->generatorF, v->generator);
    const float df = deg.at(t, 0), ef = eq.at(t, 0);
    const uint32_t db = std::bit_cast<uint32_t>(df), eb = std::bit_cast<uint32_t>(ef);
    if (!have || db != degBits || eb != eqBits || !inRange(co, g, P)) {
      have = true;
      degBits = db;
      eqBits = eb;
      tuned = laneDegree(*v, df, ef, D);
      if (tuned) co = tuningCoefficients(*v, D, g, P);
    }
    double cents = 0.0, outHz = 0.0;
    if (tuned) {
      cents = std::fma(co.K, g + co.nudge, -(co.M * P)) * (1200.0 + pick(st, t, v->stretchF, v->stretch)) * kInv1200;
      outHz = pick(ref, t, v->referenceF, v->referenceHz) * exp2Scalar(cents * kInv1200);
    }
    const bool use = tuned && finitePositive(outHz);
    oh[t] = use ? static_cast<float>(outHz) : h;
    oc[t] = use ? static_cast<float>(cents) : c;
  }
}

// ---- 8 lanes ---------------------------------------------------------------------------------------------------

void tuningHz8(const KernelArgs& a) noexcept {  // RT-safe
  const auto* v = static_cast<const TuningTableView*>(a.resource);
  const uint32_t mask = a.splMask;
  auto in = [&](uint32_t i) { return In8{a.in[i], ((mask >> i) & 1u) ? 0u : 8u, 1u}; };
  const In8 deg = in(0), eq = in(1), hz = in(2), ce = in(3);
  const TuningIns ti = tuningIns(v, in(4), in(5), in(6), in(7));
  const In8 &gen = ti.gen, &per = ti.per, &st = ti.st, &ref = ti.ref;
  float* oh = a.out[0];
  float* oc = a.out[1];
  const uint32_t frames = a.frames;

  if (v == nullptr) {  // no table: pass the fallback through
    for (uint32_t t = 0; t < frames; ++t) {
      _mm256_storeu_ps(oh + t * 8, _mm256_loadu_ps(hz.p + t * hz.ts));
      _mm256_storeu_ps(oc + t * 8, _mm256_loadu_ps(ce.p + t * ce.ts));
    }
    return;
  }

  if (deg.constant() && eq.constant() && gen.constant() && per.constant() && st.constant() && ref.constant()) {
    // Every tuning input is constant over the block: one pitch per lane.
    alignas(32) float th[8], tc[8], tm[8], tmh[8];
    for (uint32_t l = 0; l < 8; ++l) {
      int64_t D = 0;
      double cents = 0.0, outHz = 0.0;
      const bool ownHz = unmodulated(*v, gen.p[l], per.p[l], st.p[l], ref.p[l]);
      bool ok = laneDegree(*v, deg.p[l], eq.p[l], D);
      if (ok) {
        lanePitch(*v, D, exactOr(gen.p[l], v->generatorF, v->generator), exactOr(per.p[l], v->periodF, v->period),
                  exactOr(st.p[l], v->stretchF, v->stretch), exactOr(ref.p[l], v->referenceF, v->referenceHz), cents,
                  outHz, ownHz);
        ok = finitePositive(outHz);
      }
      th[l] = ok ? static_cast<float>(outHz) : 0.0f;
      tc[l] = ok ? static_cast<float>(cents) : 0.0f;
      tm[l] = boolMask(ok);
      tmh[l] = boolMask(ok && !ownHz);
    }
    const __m256 vh = _mm256_load_ps(th), vc = _mm256_load_ps(tc), vm = _mm256_load_ps(tm),
                 vmh = _mm256_load_ps(tmh);
    const __m256 zero = _mm256_setzero_ps();
    for (uint32_t t = 0; t < frames; ++t) {
      const __m256 h = _mm256_loadu_ps(hz.p + t * hz.ts);
      const __m256 c = _mm256_loadu_ps(ce.p + t * ce.ts);
      const __m256 started = _mm256_cmp_ps(h, zero, _CMP_NEQ_OQ);
      _mm256_storeu_ps(oh + t * 8, _mm256_blendv_ps(h, vh, _mm256_and_ps(vmh, started)));
      _mm256_storeu_ps(oc + t * 8, _mm256_blendv_ps(c, vc, _mm256_and_ps(vm, started)));
    }
    return;
  }

  // Per-sample path. Lane caches: absolute degree, tuned flag, coefficients and the range they hold for.
  alignas(32) double K[8], M[8], lo[8], hi[8], loM[8], hiM[8], nudge[8];
  alignas(32) float tunedF[8];
  alignas(32) uint32_t degBits[8], eqBits[8];
  int64_t D[8];
  const bool edo = v->kind == TuningTableKind::Edo;
  const double invN = 1.0 / static_cast<double>(v->n);
  // Constant tuning inputs are converted once (with the exactness rule); the others per sample.
  alignas(32) double gS[8], pS[8], sS[8], rS[8];
  for (uint32_t l = 0; l < 8; ++l) {
    gS[l] = gen.constant() ? exactOr(gen.p[l], v->generatorF, v->generator) : 0.0;
    pS[l] = per.constant() ? exactOr(per.p[l], v->periodF, v->period) : 0.0;
    sS[l] = st.constant() ? exactOr(st.p[l], v->stretchF, v->stretch) : 0.0;
    rS[l] = ref.constant() ? exactOr(ref.p[l], v->referenceF, v->referenceHz) : 0.0;
  }
  auto lanesOf = [](const In8& x, uint32_t t, const double* spl, __m256d& lo4, __m256d& hi4) {
    if (x.constant()) {
      lo4 = _mm256_load_pd(spl);
      hi4 = _mm256_load_pd(spl + 4);
    } else if (x.ls == 0) {  // a live automation buffer: one value for every lane
      lo4 = hi4 = _mm256_set1_pd(static_cast<double>(x.p[t]));
    } else {
      const __m256 f = _mm256_loadu_ps(x.p + t * x.ts);
      lo4 = _mm256_cvtps_pd(_mm256_castps256_ps128(f));
      hi4 = _mm256_cvtps_pd(_mm256_extractf128_ps(f, 1));
    }
  };
  alignas(32) double gNow[8], pNow[8];
  // (Re-)derives lane l's coefficients at sample t from its current degree and generator.
  auto setupLane = [&](uint32_t l, uint32_t t) {
    const float df = deg.at(t, l), ef = eq.at(t, l);
    degBits[l] = std::bit_cast<uint32_t>(df);
    eqBits[l] = std::bit_cast<uint32_t>(ef);
    const bool ok = laneDegree(*v, df, ef, D[l]);
    tunedF[l] = boolMask(ok);
    if (!ok) {
      K[l] = M[l] = nudge[l] = loM[l] = hiM[l] = 0.0;
      lo[l] = -std::numeric_limits<double>::infinity();
      hi[l] = std::numeric_limits<double>::infinity();
      return;
    }
    const TuningCoefficients c = tuningCoefficients(*v, D[l], gNow[l], pNow[l]);
    K[l] = c.K;
    M[l] = c.M;
    lo[l] = c.lo;
    hi[l] = c.hi;
    loM[l] = c.loMargin;
    hiM[l] = c.hiMargin;
    nudge[l] = c.nudge;
  };
  const bool degSpl = deg.constant() && eq.constant();
  const __m256 zero = _mm256_setzero_ps();
  for (uint32_t t = 0; t < frames; ++t) {
    __m256d g0, g1, p0, p1, s0, s1, r0, r1;
    lanesOf(per, t, pS, p0, p1);
    if (edo) {
      g0 = _mm256_mul_pd(p0, _mm256_set1_pd(invN));
      g1 = _mm256_mul_pd(p1, _mm256_set1_pd(invN));
    } else {
      lanesOf(gen, t, gS, g0, g1);
    }
    // Which lanes need new coefficients: first sample, a degree change, or the generator outside their range.
    uint32_t redo = 0;
    if (t == 0) {
      redo = 0xFFu;
    } else {
      if (!degSpl) {
        const __m256i db = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(deg.p + t * deg.ts));
        const __m256i eb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(eq.p + t * eq.ts));
        const __m256i same =
            _mm256_and_si256(_mm256_cmpeq_epi32(db, _mm256_load_si256(reinterpret_cast<const __m256i*>(degBits))),
                             _mm256_cmpeq_epi32(eb, _mm256_load_si256(reinterpret_cast<const __m256i*>(eqBits))));
        redo |= ~static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(same))) & 0xFFu;
      }
      const __m256d in0 = _mm256_and_pd(
          _mm256_cmp_pd(g0, _mm256_fmadd_pd(_mm256_load_pd(lo), p0, _mm256_load_pd(loM)), _CMP_GE_OQ),
          _mm256_cmp_pd(g0, _mm256_fmsub_pd(_mm256_load_pd(hi), p0, _mm256_load_pd(hiM)), _CMP_LT_OQ));
      const __m256d in1 = _mm256_and_pd(
          _mm256_cmp_pd(g1, _mm256_fmadd_pd(_mm256_load_pd(lo + 4), p1, _mm256_load_pd(loM + 4)), _CMP_GE_OQ),
          _mm256_cmp_pd(g1, _mm256_fmsub_pd(_mm256_load_pd(hi + 4), p1, _mm256_load_pd(hiM + 4)), _CMP_LT_OQ));
      const uint32_t inside = static_cast<uint32_t>(_mm256_movemask_pd(in0)) |
                              (static_cast<uint32_t>(_mm256_movemask_pd(in1)) << 4);
      redo |= ~inside & 0xFFu;
    }
    if (redo != 0) {
      _mm256_store_pd(gNow, g0);
      _mm256_store_pd(gNow + 4, g1);
      _mm256_store_pd(pNow, p0);
      _mm256_store_pd(pNow + 4, p1);
      for (uint32_t bits = redo; bits != 0; bits &= bits - 1) setupLane(static_cast<uint32_t>(std::countr_zero(bits)), t);
    }
    lanesOf(st, t, sS, s0, s1);
    lanesOf(ref, t, rS, r0, r1);
    // cents = (K·(g + nudge) − M·P) · (1200 + st) / 1200;  hz = ref · 2^(cents / 1200)
    const __m256d k1200 = _mm256_set1_pd(1200.0), inv = _mm256_set1_pd(kInv1200);
    const __m256d c0 = _mm256_mul_pd(
        _mm256_mul_pd(_mm256_fmsub_pd(_mm256_load_pd(K), _mm256_add_pd(g0, _mm256_load_pd(nudge)),
                                      _mm256_mul_pd(_mm256_load_pd(M), p0)),
                      _mm256_add_pd(k1200, s0)),
        inv);
    const __m256d c1 = _mm256_mul_pd(
        _mm256_mul_pd(_mm256_fmsub_pd(_mm256_load_pd(K + 4), _mm256_add_pd(g1, _mm256_load_pd(nudge + 4)),
                                      _mm256_mul_pd(_mm256_load_pd(M + 4), p1)),
                      _mm256_add_pd(k1200, s1)),
        inv);
    const __m256d h0 = _mm256_mul_pd(r0, exp2Vec(_mm256_mul_pd(c0, inv)));
    const __m256d h1 = _mm256_mul_pd(r1, exp2Vec(_mm256_mul_pd(c1, inv)));
    const __m256 hzT = _mm256_set_m128(_mm256_cvtpd_ps(h1), _mm256_cvtpd_ps(h0));
    const __m256 cT = _mm256_set_m128(_mm256_cvtpd_ps(c1), _mm256_cvtpd_ps(c0));
    const __m256 h = _mm256_loadu_ps(hz.p + t * hz.ts);
    const __m256 c = _mm256_loadu_ps(ce.p + t * ce.ts);
    // Tuned, started, and a finite positive result (invalid inputs fall back to the engine's value).
    const __m256 good = _mm256_and_ps(_mm256_cmp_ps(hzT, zero, _CMP_GT_OQ),
                                      _mm256_cmp_ps(hzT, _mm256_set1_ps(3.4e38f), _CMP_LE_OQ));
    const __m256 use =
        _mm256_and_ps(_mm256_and_ps(_mm256_load_ps(tunedF), good), _mm256_cmp_ps(h, zero, _CMP_NEQ_OQ));
    _mm256_storeu_ps(oh + t * 8, _mm256_blendv_ps(h, hzT, use));
    _mm256_storeu_ps(oc + t * 8, _mm256_blendv_ps(c, cT, use));
  }
}

}  // namespace

KernelFn findTuningKernel(uint32_t lanes) noexcept {
  if (lanes == 1) return &tuningHz1;
  if (lanes == kGroupLanes) return &tuningHz8;
  return nullptr;
}

}  // namespace detail
}  // namespace pq::dsp
