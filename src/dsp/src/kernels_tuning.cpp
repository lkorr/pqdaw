// The tuning node's kernel (T2.6, SPEC §3.3): tuning_hz(degree, equave, hz, cents, generator, period, stretch,
// reference, select, node) → (hz, cents), resource = TuningTableView (pq/dsp/tuning_table.h).
//
// Per lane (T2.6-fix3 adds `select` / `node` and the reference and ratio cases, M3 review H4 / M9):
//   mine      = node == 0 || select == node (a track patch chains one node per tuning signal; voice.tuning says
//               which one a note follows: QUESTIONS Q105);
//   degree    = mine and a finite degree / equave: a Degree note in this tuning, or a Ratio note anchored (through
//               notes) to one, carrying the root's degree;
//   reference = mine, node ≥ 1 and no degree: a Ratio note on the reference or a Cents note;
//   started   = hz is not 0 (the VoiceEngine's voice.hz is 0 before a note's onset in its first block).
// With D the absolute degree, tc(t) its stretched cents at the inputs' generator / period / stretch, tc₀ the same at
// the table's definition values, hz₀ = reference₀ · 2^(tc₀/1200) its static pitch there and r(t) = reference(t):
//   degree:    out.hz = r · 2^(tc/1200) · E,   E = hz / hz₀ (1 when within 4e-7 of it: a Degree note's own static
//                                             Hz; otherwise the note's ratio to its root and any glide, M9)
//              out.cents = cents + (tc − tc₀) + 1200·log2(r / reference₀)
//   reference: out.hz = hz · r / reference₀,  out.cents = cents + 1200·log2(r / reference₀)
//   otherwise (another node's voice, a static note, not started): hz, cents unchanged.
// So voice.cents is the engine's cents of the note (VoiceEngine: from VoiceConfig::referenceHz) moved by what the
// tuning did to it; its reference term uses the reference at frame 0 of the block (a per-sample reference moves
// voice.cents at block rate; voice.hz follows it per sample).
// `unmodulated` (T2.6-fix1) = every tuning param input is a Spl pattern equal to float32 of the table's definition
// value (generator — not read by an EDO —, period, stretch, reference): the tuning is exactly the one the notes'
// static Hz were resolved in, so every lane's output is its input, bit for bit (the project lowering resolves a
// following note's NoteDesc::hz in the table's definition: pq/app/project_link.h). A project whose tunings are not
// modulated therefore renders bit-identically with and without its tuning nodes, at the cost of a copy.
//
// Three paths, chosen per block from KernelArgs::splMask (read at run time instead of template-specialising the
// 2^10 masks; both backends set it):
//   - unmodulated: a copy;
//   - every tuning input (degree, equave, select, node, generator, period, stretch, reference) constant over the
//     block: each lane's pitch is computed once per block in double, and the per-sample work is a few float
//     multiplies, a compare and blends per 8 lanes;
//   - otherwise, per sample in AVX2 doubles (4 lanes per __m256d): the range check of the cached (K, M) against the
//     generator, the linear form (1 FMA), the stretch, exp2 (round, 10-term polynomial, exponent add), the
//     reference and E. A lane leaves the cached coefficients only at a region crossing or a degree change, where
//     they are re-derived (scalar binary search) at that very sample.
// Double precision is needed: k·g − m·P reaches ~10^4 ¢, where float32 would be ~1e-3 ¢; in double the only error
// of a Degree note is the final float32 rounding of the Hz value (≤ 0.5 ulp; a ratio note adds its hz input's).
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
constexpr double kSnap = 4e-7;       // |hz / hz₀ − 1| below this is the note's own static pitch (a few float ulps)

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

// An input: the value of lane `lane` at frame t is p[t·ts + lane·ls]. A Spl pattern: ts = 0, ls = 1; a block buffer:
// ts = lanes, ls = 1; a live automation buffer (global, per sample: TuningLiveInputs): ts = 1, ls = 0.
struct In8 {
  const float* p;
  uint32_t ts;
  uint32_t ls;
  float at(uint32_t t, uint32_t lane) const noexcept { return p[t * ts + lane * ls]; }
  bool constant() const noexcept { return ts == 0; }
};

// The kernel's inputs for this block: the tuning params replaced by the live automation buffers that move in it (an
// EDO reads neither the generator nor the stretch).
struct Ins {
  In8 deg, eq, hz, ce, gen, per, st, ref, sel, node;
};
inline Ins inputs(const KernelArgs& a, const TuningTableView* v, uint32_t lanes) noexcept {
  auto in = [&](uint32_t i) { return In8{a.in[i], ((a.splMask >> i) & 1u) ? 0u : lanes, 1u}; };
  Ins r{in(0), in(1), in(2), in(3), in(4), in(5), in(6), in(7), in(8), in(9)};
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

// Every tuning param input constant and at float32 of the definition's value (an EDO does not read the generator
// or the stretch) for every lane: the block is a copy (see the file comment).
inline bool unmodulated(const TuningTableView& v, const Ins& x, uint32_t lanes) noexcept {
  const bool edo = v.kind == TuningTableKind::Edo;
  if (!x.per.constant() || !x.ref.constant() || (!edo && (!x.gen.constant() || !x.st.constant()))) return false;
  for (uint32_t l = 0; l < lanes; ++l)
    if (x.per.p[l] != v.periodF || x.ref.p[l] != v.referenceF ||
        (!edo && (x.gen.p[l] != v.generatorF || x.st.p[l] != v.stretchF)))
      return false;
  return true;
}

// What a lane is to this node (see the file comment).
enum class Mode : uint8_t { Pass, Degree, Reference };

// One lane's role from its degree / equave / select and the node number; D = its absolute degree.
inline Mode laneMode(const TuningTableView& v, float deg, float eq, float sel, float node, int64_t& D) noexcept {
  if (node != 0.0f && sel != node) return Mode::Pass;
  if (std::isfinite(deg) && std::isfinite(eq) && std::fabs(deg) <= 16777216.0f && std::fabs(eq) <= 16777216.0f) {
    // |values| ≤ 2^24 keep the products exact; a degree beyond that is not a note.
    const int64_t N = static_cast<int64_t>(v.n) * v.periodsPerEquave;
    D = static_cast<int64_t>(std::floor(eq)) * N + static_cast<int64_t>(std::floor(deg));
    return Mode::Degree;
  }
  return node >= 1.0f ? Mode::Reference : Mode::Pass;
}

// Stretched cents of degree D at generator g, period P, stretch st (the EDO's generator follows its equave).
inline double centsAt(const TuningTableView& v, int64_t D, double g, double P, double st) noexcept {
  if (v.kind == TuningTableKind::Edo) g = P / static_cast<double>(v.n);
  const TuningCoefficients c = tuningCoefficients(v, D, g, P);
  return std::fma(c.K, g + c.nudge, -(c.M * P)) * (1200.0 + st) * kInv1200;
}

// The degree's cents and 1 / Hz at the table's definition (tc₀, 1 / hz₀).
inline void definitionPitch(const TuningTableView& v, int64_t D, double& tc0, double& invHz0) noexcept {
  tc0 = centsAt(v, D, v.generator, v.period, v.stretch);
  invHz0 = 1.0 / (v.referenceHz * exp2Scalar(tc0 * kInv1200));
}

inline bool finitePositive(double hz) noexcept { return hz > 0.0 && hz <= 3.4e38; }

// The cached coefficients still hold for generator g at period P.
inline bool inRange(const TuningCoefficients& c, double g, double P) noexcept {
  return g >= std::fma(c.lo, P, c.loMargin) && g < std::fma(c.hi, P, -c.hiMargin);
}

inline void copyBlock(const KernelArgs& a, const Ins& x, uint32_t lanes) noexcept {
  const uint32_t n = a.frames * lanes;
  for (uint32_t i = 0; i < n; ++i) a.out[0][i] = x.hz.constant() ? x.hz.p[i % lanes] : x.hz.p[i];
  for (uint32_t i = 0; i < n; ++i) a.out[1][i] = x.ce.constant() ? x.ce.p[i % lanes] : x.ce.p[i];
}

// ---- 1 lane ----------------------------------------------------------------------------------------------------

void tuningHz1(const KernelArgs& a) noexcept {  // RT-safe
  const auto* v = static_cast<const TuningTableView*>(a.resource);
  const Ins x = inputs(a, v, 1);
  if (v == nullptr || unmodulated(*v, x, 1)) {
    copyBlock(a, x, 1);
    return;
  }
  float* oh = a.out[0];
  float* oc = a.out[1];
  const auto pick = [](const In8& in, uint32_t t, float f, double d) {
    return in.constant() ? exactOr(in.p[0], f, d) : static_cast<double>(in.at(t, 0));
  };
  // voice.cents' reference term: the reference at frame 0 of the block.
  const double refC = 1200.0 * std::log2(pick(x.ref, 0, v->referenceF, v->referenceHz) / v->referenceHz);
  TuningCoefficients co;
  int64_t D = 0;
  Mode mode = Mode::Pass;
  double tc0 = 0.0, invHz0 = 0.0;
  bool have = false;
  uint32_t bits[4] = {};
  for (uint32_t t = 0; t < a.frames; ++t) {
    const float h = x.hz.at(t, 0);
    const float c = x.ce.at(t, 0);
    if (h == 0.0f) {
      oh[t] = h;
      oc[t] = c;
      continue;
    }
    const double P = pick(x.per, t, v->periodF, v->period);
    const double g = v->kind == TuningTableKind::Edo ? P / static_cast<double>(v->n)
                                                     : pick(x.gen, t, v->generatorF, v->generator);
    const float df = x.deg.at(t, 0), ef = x.eq.at(t, 0), sf = x.sel.at(t, 0), nf = x.node.at(t, 0);
    const uint32_t now[4] = {std::bit_cast<uint32_t>(df), std::bit_cast<uint32_t>(ef), std::bit_cast<uint32_t>(sf),
                             std::bit_cast<uint32_t>(nf)};
    const bool same = have && now[0] == bits[0] && now[1] == bits[1] && now[2] == bits[2] && now[3] == bits[3];
    if (!same) {
      have = true;
      std::memcpy(bits, now, sizeof bits);
      mode = laneMode(*v, df, ef, sf, nf, D);
      if (mode == Mode::Degree) {
        definitionPitch(*v, D, tc0, invHz0);
        co = tuningCoefficients(*v, D, g, P);
      }
    } else if (mode == Mode::Degree && !inRange(co, g, P)) {
      co = tuningCoefficients(*v, D, g, P);
    }
    const double r = pick(x.ref, t, v->referenceF, v->referenceHz);
    double outHz = h, outC = c;
    if (mode == Mode::Degree) {
      const double tc = std::fma(co.K, g + co.nudge, -(co.M * P)) * (1200.0 + pick(x.st, t, v->stretchF, v->stretch)) * kInv1200;
      double e = static_cast<double>(h) * invHz0;
      if (std::fabs(e - 1.0) < kSnap) e = 1.0;
      outHz = r * exp2Scalar(tc * kInv1200) * e;
      outC = static_cast<double>(c) + (tc - tc0) + refC;
    } else if (mode == Mode::Reference) {
      outHz = static_cast<double>(h) * (r / v->referenceHz);
      outC = static_cast<double>(c) + refC;
    }
    const bool use = mode != Mode::Pass && finitePositive(outHz);
    oh[t] = use ? static_cast<float>(outHz) : h;
    oc[t] = use ? static_cast<float>(outC) : c;
  }
}

// ---- 8 lanes ---------------------------------------------------------------------------------------------------

void tuningHz8(const KernelArgs& a) noexcept {  // RT-safe
  const auto* v = static_cast<const TuningTableView*>(a.resource);
  const Ins x = inputs(a, v, 8);
  const In8 &deg = x.deg, &eq = x.eq, &hz = x.hz, &ce = x.ce, &gen = x.gen, &per = x.per, &st = x.st, &ref = x.ref,
            &sel = x.sel, &node = x.node;
  float* oh = a.out[0];
  float* oc = a.out[1];
  const uint32_t frames = a.frames;

  if (v == nullptr || unmodulated(*v, x, 8)) {  // no table, or exactly the definition: the notes' own pitches
    for (uint32_t t = 0; t < frames; ++t) {
      _mm256_storeu_ps(oh + t * 8, _mm256_loadu_ps(hz.p + t * hz.ts));
      _mm256_storeu_ps(oc + t * 8, _mm256_loadu_ps(ce.p + t * ce.ts));
    }
    return;
  }
  const __m256 zero = _mm256_setzero_ps();

  if (deg.constant() && eq.constant() && sel.constant() && node.constant() && gen.constant() && per.constant() &&
      st.constant() && ref.constant()) {
    // Every tuning input is constant over the block: one pitch per lane, then out.hz = h · mul (Reference, Pass:
    // mul = r/r₀, 1) or th · snap(h / hz₀) (Degree), out.cents = c + dc.
    alignas(32) float th[8], invB[8], mul[8], dc[8], isDeg[8], use[8];
    for (uint32_t l = 0; l < 8; ++l) {
      int64_t D = 0;
      const Mode mode = laneMode(*v, deg.p[l], eq.p[l], sel.p[l], node.p[l], D);
      const double r = exactOr(ref.p[l], v->referenceF, v->referenceHz);
      const double refC = 1200.0 * std::log2(r / v->referenceHz);
      th[l] = invB[l] = dc[l] = 0.0f;
      mul[l] = 1.0f;
      isDeg[l] = use[l] = boolMask(false);
      if (mode == Mode::Degree) {
        double tc0 = 0.0, invHz0 = 0.0;
        definitionPitch(*v, D, tc0, invHz0);
        const double tc = centsAt(*v, D, exactOr(gen.p[l], v->generatorF, v->generator),
                                  exactOr(per.p[l], v->periodF, v->period), exactOr(st.p[l], v->stretchF, v->stretch));
        const double outHz = r * exp2Scalar(tc * kInv1200);
        if (finitePositive(outHz) && finitePositive(1.0 / invHz0)) {
          th[l] = static_cast<float>(outHz);
          invB[l] = static_cast<float>(invHz0);
          dc[l] = static_cast<float>((tc - tc0) + refC);
          isDeg[l] = use[l] = boolMask(true);
        }
      } else if (mode == Mode::Reference && finitePositive(r)) {
        mul[l] = static_cast<float>(r / v->referenceHz);
        dc[l] = static_cast<float>(refC);
        use[l] = boolMask(true);
      }
    }
    const __m256 vth = _mm256_load_ps(th), vib = _mm256_load_ps(invB), vmul = _mm256_load_ps(mul),
                 vdc = _mm256_load_ps(dc), vdeg = _mm256_load_ps(isDeg), vuse = _mm256_load_ps(use);
    const __m256 one = _mm256_set1_ps(1.0f), snap = _mm256_set1_ps(static_cast<float>(kSnap));
    const __m256 absMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    for (uint32_t t = 0; t < frames; ++t) {
      const __m256 h = _mm256_loadu_ps(hz.p + t * hz.ts);
      const __m256 c = _mm256_loadu_ps(ce.p + t * ce.ts);
      __m256 e = _mm256_mul_ps(h, vib);
      e = _mm256_blendv_ps(e, one, _mm256_cmp_ps(_mm256_and_ps(_mm256_sub_ps(e, one), absMask), snap, _CMP_LT_OQ));
      const __m256 hzT = _mm256_blendv_ps(_mm256_mul_ps(h, vmul), _mm256_mul_ps(vth, e), vdeg);
      const __m256 on = _mm256_and_ps(vuse, _mm256_cmp_ps(h, zero, _CMP_NEQ_OQ));
      _mm256_storeu_ps(oh + t * 8, _mm256_blendv_ps(h, hzT, on));
      _mm256_storeu_ps(oc + t * 8, _mm256_blendv_ps(c, _mm256_add_ps(c, vdc), on));
    }
    return;
  }

  // Per-sample path. Lane caches: role, absolute degree, the definition pitch, coefficients and their range.
  alignas(32) double K[8], M[8], lo[8], hi[8], loM[8], hiM[8], nudge[8], tc0[8], invHz0[8];
  alignas(32) float degF[8], refF[8];  // masks: a Degree lane, a Reference lane
  alignas(32) uint32_t degBits[8], eqBits[8], selBits[8];
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
  auto lanesOf = [](const In8& in, uint32_t t, const double* spl, __m256d& lo4, __m256d& hi4) {
    if (in.constant()) {
      lo4 = _mm256_load_pd(spl);
      hi4 = _mm256_load_pd(spl + 4);
    } else if (in.ls == 0) {  // a live automation buffer: one value for every lane
      lo4 = hi4 = _mm256_set1_pd(static_cast<double>(in.p[t]));
    } else {
      const __m256 f = _mm256_loadu_ps(in.p + t * in.ts);
      lo4 = _mm256_cvtps_pd(_mm256_castps256_ps128(f));
      hi4 = _mm256_cvtps_pd(_mm256_extractf128_ps(f, 1));
    }
  };
  // voice.cents' reference term, per lane, from the reference at frame 0.
  alignas(32) float refC[8];
  for (uint32_t l = 0; l < 8; ++l)
    refC[l] = static_cast<float>(1200.0 * std::log2((ref.constant() ? rS[l] : static_cast<double>(ref.at(0, l))) /
                                                    v->referenceHz));
  alignas(32) double gNow[8], pNow[8];
  // (Re-)derives lane l's role and coefficients at sample t from its current degree / select and generator.
  auto setupLane = [&](uint32_t l, uint32_t t, bool roleChanged) {
    if (roleChanged) {
      const float df = deg.at(t, l), ef = eq.at(t, l), sf = sel.at(t, l);
      degBits[l] = std::bit_cast<uint32_t>(df);
      eqBits[l] = std::bit_cast<uint32_t>(ef);
      selBits[l] = std::bit_cast<uint32_t>(sf);
      const Mode mode = laneMode(*v, df, ef, sf, node.at(t, l), D[l]);
      degF[l] = boolMask(mode == Mode::Degree);
      refF[l] = boolMask(mode == Mode::Reference);
      if (mode == Mode::Degree) definitionPitch(*v, D[l], tc0[l], invHz0[l]);
    }
    if (!maskIsTrue(degF[l])) {
      K[l] = M[l] = nudge[l] = loM[l] = hiM[l] = tc0[l] = invHz0[l] = 0.0;
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
  const bool roleSpl = deg.constant() && eq.constant() && sel.constant();
  const __m256d k1200 = _mm256_set1_pd(1200.0), inv = _mm256_set1_pd(kInv1200), oneD = _mm256_set1_pd(1.0),
                snapD = _mm256_set1_pd(kSnap), invRef0 = _mm256_set1_pd(1.0 / v->referenceHz);
  const __m256d absD = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFll));
  for (uint32_t t = 0; t < frames; ++t) {
    __m256d g0, g1, p0, p1, s0, s1, r0, r1;
    lanesOf(per, t, pS, p0, p1);
    if (edo) {
      g0 = _mm256_mul_pd(p0, _mm256_set1_pd(invN));
      g1 = _mm256_mul_pd(p1, _mm256_set1_pd(invN));
    } else {
      lanesOf(gen, t, gS, g0, g1);
    }
    // Which lanes need new coefficients: first sample, a role change, or the generator outside their range.
    uint32_t redo = 0, roles = 0;
    if (t == 0) {
      redo = roles = 0xFFu;
    } else {
      if (!roleSpl) {
        auto sameBits = [&](const In8& in, const uint32_t* prev) {
          const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in.p + t * in.ts));
          return _mm256_cmpeq_epi32(b, _mm256_load_si256(reinterpret_cast<const __m256i*>(prev)));
        };
        const __m256i same = _mm256_and_si256(_mm256_and_si256(sameBits(deg, degBits), sameBits(eq, eqBits)),
                                              sameBits(sel, selBits));
        roles = ~static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(same))) & 0xFFu;
        redo |= roles;
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
      for (uint32_t b = redo; b != 0; b &= b - 1) {
        const auto l = static_cast<uint32_t>(std::countr_zero(b));
        setupLane(l, t, ((roles >> l) & 1u) != 0);
      }
    }
    lanesOf(st, t, sS, s0, s1);
    lanesOf(ref, t, rS, r0, r1);
    const __m256 h = _mm256_loadu_ps(hz.p + t * hz.ts);
    const __m256 c = _mm256_loadu_ps(ce.p + t * ce.ts);
    const __m256d h0 = _mm256_cvtps_pd(_mm256_castps256_ps128(h)), h1 = _mm256_cvtps_pd(_mm256_extractf128_ps(h, 1));
    // Degree: tc = (K·(g + nudge) − M·P) · (1200 + st) / 1200;  hz = ref · 2^(tc / 1200) · snap(h / hz₀)
    auto degreeHz = [&](int o, __m256d g, __m256d P, __m256d S, __m256d R, __m256d H, __m256d& tc) {
      tc = _mm256_mul_pd(_mm256_mul_pd(_mm256_fmsub_pd(_mm256_load_pd(K + o), _mm256_add_pd(g, _mm256_load_pd(nudge + o)),
                                                       _mm256_mul_pd(_mm256_load_pd(M + o), P)),
                                       _mm256_add_pd(k1200, S)),
                         inv);
      __m256d e = _mm256_mul_pd(H, _mm256_load_pd(invHz0 + o));
      e = _mm256_blendv_pd(e, oneD, _mm256_cmp_pd(_mm256_and_pd(_mm256_sub_pd(e, oneD), absD), snapD, _CMP_LT_OQ));
      return _mm256_mul_pd(_mm256_mul_pd(R, exp2Vec(_mm256_mul_pd(tc, inv))), e);
    };
    __m256d tcA, tcB;
    const __m256d dA = degreeHz(0, g0, p0, s0, r0, h0, tcA), dB = degreeHz(4, g1, p1, s1, r1, h1, tcB);
    // Reference: hz · ref / ref₀.
    const __m256d rA = _mm256_mul_pd(h0, _mm256_mul_pd(r0, invRef0)), rB = _mm256_mul_pd(h1, _mm256_mul_pd(r1, invRef0));
    const __m256 degM = _mm256_load_ps(degF), refM = _mm256_load_ps(refF);
    const __m256 hzDeg = _mm256_set_m128(_mm256_cvtpd_ps(dB), _mm256_cvtpd_ps(dA));
    const __m256 hzRef = _mm256_set_m128(_mm256_cvtpd_ps(rB), _mm256_cvtpd_ps(rA));
    const __m256 hzT = _mm256_blendv_ps(hzRef, hzDeg, degM);
    // cents: c + (tc − tc₀) + refC (Degree), c + refC (Reference).
    const __m256d dcA = _mm256_sub_pd(tcA, _mm256_load_pd(tc0)), dcB = _mm256_sub_pd(tcB, _mm256_load_pd(tc0 + 4));
    const __m256 dcDeg = _mm256_set_m128(_mm256_cvtpd_ps(dcB), _mm256_cvtpd_ps(dcA));
    const __m256 dcT = _mm256_add_ps(_mm256_and_ps(dcDeg, degM), _mm256_load_ps(refC));
    // Mine, started, and a finite positive result (invalid inputs fall back to the engine's value).
    const __m256 good = _mm256_and_ps(_mm256_cmp_ps(hzT, zero, _CMP_GT_OQ),
                                      _mm256_cmp_ps(hzT, _mm256_set1_ps(3.4e38f), _CMP_LE_OQ));
    const __m256 use = _mm256_and_ps(_mm256_and_ps(_mm256_or_ps(degM, refM), good), _mm256_cmp_ps(h, zero, _CMP_NEQ_OQ));
    _mm256_storeu_ps(oh + t * 8, _mm256_blendv_ps(h, hzT, use));
    _mm256_storeu_ps(oc + t * 8, _mm256_blendv_ps(c, _mm256_add_ps(c, dcT), use));
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
