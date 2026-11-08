// svf (EQ.1): N cascaded TPT state-variable sections with an output mix (pq/dsp/svf_design.h; semantics in atoms.h).
//
// Coefficients, per lane (svfDesign in double; the inputs a design sees are its own, so every design is EXACT):
//   - param rate (cutoff / q / gain Spl, or a Blk call whose inputs are constant and were constant the frame before):
//     designed when the lane's inputs differ bitwise from the ones its coefficients are the exact design of; nothing
//     per block at rest. Lanes whose inputs agree share one design (copied lane to lane).
//   - audio rate (a Blk input: modulation, or the compiler's 20 ms param ramp while a knob moves), EQ.1b: a lane whose
//     inputs move ON A LINE (each input's second difference ≤ kSegTol of its value: a param ramp, a linear sweep) runs
//     a RAMP SEGMENT: designs at the inputs kSegLen / 2 and kSegLen frames ahead on that line, and the six kernel
//     coefficients of every section follow the quadratic through (now, middle, end) by forward differences (two adds
//     each per frame) while each frame's inputs stay on the predicted line within kSegTol (else: an exact design at
//     once). The length is ≤ 32 frames and bounded by the relative step of what the design depends on (cutoff:
//     tan(π·fc/sr); q; gain) so that the segment moves it by ≤ kSegStep, ends before an input clamp and a frame before a
//     gain reaches 0 dB (where a bell / shelf is the identity, a form without recursion that the designs on either side
//     do not approach; none starts there either); a segment that completes chains the next one from
//     where it is (one design pair per segment). Measured against a design per frame (tests/dsp/test_svf.cpp): ≤ −90 dB
//     on 20 ms ramps of cutoff (20 Hz ↔ 20 kHz included), q and gain, every mode, 1 and 8 stages. When the inputs stop,
//     the lane is designed exactly again (`exact`), so a filter at rest always runs its exact design (svfResponse).
//     Inputs that are not on a line (a curved modulation) design per frame, exactly: the costly case (bench/RESULTS.md
//     "matched audio cutoff"; the bilinear prewarp is cheaper).
//   Gain is ignored (held at 1) by the modes without one, so a gain ramp never redesigns a lowpass.
//
// Per sample and section (svf_fast.pqp's shallow form: each new state two multiply-adds deep):
//   v = x − ic2;  n1 = b1·v + (d1·ic1 + ic1);  n2 = b2·v + (b1·ic1 + ic2);  y = h1·(ic1 + n1) + (h2·(ic2 + n2) + m0·x)
// Param rate: sections outer in passes of two (their states in registers over the block; the two recursions
// interleave), frames inner. Audio rate: frames outer (each frame's coefficients first), sections inner. 8 lanes: one
// AVX2 vector per section and frame for the whole voice group.
//
// Precision: up to kDoubleFrom − 1 sections run in float. A deep cascade amplifies its float rounding noise by the gain
// of the sections after it (a 64-section Butterworth lowpass: the float output is noise near the cutoff; svf_design.h
// orders the sections to keep partial gains low, which leaves 16 sections accurate to ~1e−6), so from kDoubleFrom
// sections on the whole cascade runs in double (states kept as exact doubles, each split into its 32-bit halves: the
// float state's field holds lo, the next field hi, so that every state word stays per lane): 8 lanes as two __m256d
// halves, so about twice the float cost.
//
// State: sized by the sections its consts give (StateInit::svfSections; atom_state.h SvfState), every word per lane.
//
// Any split of a block into calls gives the same bits: designs and segments depend only on each frame's inputs and the
// ones before it (kept in the state), states are kept exactly between calls, and the per-call guard below only
// touches non-finite or runaway states.
//
// Denormals: FTZ / DAZ on the audio threads flush the decaying states to 0; a state that became NaN or ±inf (a NaN
// input) or exceeds 1e30 is reset to 0 at the end of the call, so one bad sample does not silence the filter forever.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <type_traits>

#include "kernels_common.h"
#include "pq/dsp/svf_design.h"

namespace pq::dsp::detail {
namespace {

static_assert(kSvfStateSections == kSvfMaxStages);

// Per section (atom_state.h SvfState): the states (in double: lo = the float field, hi = the next), the six
// coefficients the kernel runs, and their first / second forward differences along a ramp segment.
enum Field : uint32_t { kIc1, kIc1Hi, kIc2, kIc2Hi, kD1, kB1, kB2, kM0, kH1, kH2, kF1, kF2 = kF1 + 6, kFieldEnd = kF2 + 6 };
static_assert(kFieldEnd == kSvfFields);
constexpr uint32_t kDoubleFrom = 17;  // sections from which the cascade runs in double

// Ramp segments (header): the longest, the largest relative step of a designed quantity over one, and the relative
// deviation from the predicted line an input may have (a param ramp's values are within a few ulp of their line).
constexpr uint32_t kSegLen = 32;
constexpr float kSegStep = 0.1f;
constexpr float kSegTol = 2e-6f;

template <uint32_t L>
struct Layout {
  static constexpr uint32_t kSec = L == 1 ? kSvfFields : kSvfFields * 8;  // floats per section
  static constexpr uint32_t kFld = L == 1 ? 1 : 8;                         // floats per field
  static float* at(SvfState& st, uint32_t s, uint32_t f, uint32_t lane) noexcept {
    return st.sec + s * kSec + f * kFld + lane;
  }
};

struct LaneKey {
  SvfParams p;     // mode, prewarp, stages (the rest is filled per design)
  uint32_t key;
  uint32_t count;  // ≤ the state's sections
  bool gainMode;   // bell / shelves (the others ignore the gain input: held at 1)
};
LaneKey laneKey(float mode, float stages, float prewarp, double sr, uint32_t cap) noexcept {
  LaneKey k{};
  k.p = svfParamsFromInputs(1000.0f, 1.0f, 1.0f, mode, stages, prewarp, sr);
  const uint32_t c = svfSectionCount(k.p.mode, k.p.stages);
  k.count = c < cap ? c : cap;
  k.key = 1u | static_cast<uint32_t>(k.p.mode) << 1 | static_cast<uint32_t>(k.p.prewarp) << 4 | k.p.stages << 8;
  k.gainMode = k.p.mode == SvfMode::Bell || k.p.mode == SvfMode::LowShelf || k.p.mode == SvfMode::HighShelf;
  return k;
}

bool same(float a, float b) noexcept { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

// The inputs of one lane and frame (gain 1 for the modes without one).
struct Inputs {
  float v[3];
};
bool sameInputs(const SvfState& st, uint32_t l, const Inputs& v) noexcept {
  return same(st.cutoff[l], v.v[0]) && same(st.q[l], v.v[1]) && same(st.gain[l], v.v[2]);
}

template <uint32_t L>
void writeCoefs(SvfState& st, uint32_t s, uint32_t lane, const SvfKernelCoefs& c) noexcept {
  using Ly = Layout<L>;
  *Ly::at(st, s, kD1, lane) = c.d1;
  *Ly::at(st, s, kB1, lane) = c.b1;
  *Ly::at(st, s, kB2, lane) = c.b2;
  *Ly::at(st, s, kM0, lane) = c.m0;
  *Ly::at(st, s, kH1, lane) = c.h1;
  *Ly::at(st, s, kH2, lane) = c.h2;
}

uint32_t designSections(const SvfState& st, const LaneKey& k, const Inputs& v, SvfSection* out) noexcept {
  const SvfParams p = svfParamsFromInputs(v.v[0], v.v[1], v.v[2], static_cast<float>(k.p.mode),
                                          static_cast<float>(k.p.stages),
                                          k.p.prewarp == SvfPrewarp::Bilinear ? 1.0f : 0.0f, st.sampleRate[0]);
  const uint32_t n = svfDesign(p, out);
  return n < k.count ? n : k.count;
}

// Makes lane l's coefficients the exact design of (k, v): a copy of lane l − 1's when that lane holds the exact design
// of the same key and inputs, else a design. A new key also writes the identity (and no ramp) into the sections past
// its count, up to the state's capacity. Ends any ramp segment.
template <uint32_t L>
__declspec(noinline) void designExact(SvfState& st, uint32_t l, const LaneKey& k, const Inputs& v) noexcept {
  using Ly = Layout<L>;
  const bool newKey = st.key[l] != k.key;
  const uint32_t upto = newKey ? st.sections[0] : k.count;
  bool copied = false;
  if constexpr (L > 1) {
    if (l > 0 && st.key[l - 1] == k.key && st.exact[l - 1] != 0 && sameInputs(st, l - 1, v)) {
      for (uint32_t s = 0; s < upto; ++s)
        for (uint32_t f = kD1; f <= kH2; ++f) *Ly::at(st, s, f, l) = *Ly::at(st, s, f, l - 1);
      copied = true;
    }
  }
  if (!copied) {
    SvfSection secs[kSvfMaxStages];
    const uint32_t n = designSections(st, k, v, secs);
    for (uint32_t s = 0; s < n; ++s) writeCoefs<L>(st, s, l, svfKernelCoefs(secs[s]));
    for (uint32_t s = n; s < upto; ++s) writeCoefs<L>(st, s, l, SvfKernelCoefs{});
  }
  if (newKey) {
    for (uint32_t s = 0; s < upto; ++s)
      for (uint32_t f = kF1; f < kFieldEnd; ++f) *Ly::at(st, s, f, l) = 0.0f;
    // The identity sections hold their states (no recursion): zero, so that they never keep the group from decaying
    // (decayStateSmall) and a later key that uses them starts them at rest.
    for (uint32_t s = k.count; s < upto; ++s)
      for (uint32_t f = kIc1; f <= kIc2Hi; ++f) *Ly::at(st, s, f, l) = 0.0f;
  }
  st.key[l] = k.key;
  st.count[l] = k.count;
  st.cutoff[l] = v.v[0];
  st.q[l] = v.v[1];
  st.gain[l] = v.v[2];
  st.exact[l] = 1;
  st.rem[l] = 0;
}

template <uint32_t L>
void startSegment(SvfState& st, uint32_t l, const LaneKey& k, const Inputs& v, const float* sl, uint32_t n) noexcept;

// Tries to start a ramp segment for lane l at inputs v (its coefficients are those of v now): `chained` continues a
// segment that just completed on its line (the step from its whole length), else the line is the last three frames'.
// Sets the forward differences of every section and the segment, or leaves the lane without one.
template <uint32_t L>
void openSegment(SvfState& st, uint32_t l, const LaneKey& k, const Inputs& v, bool chained) noexcept {
  st.rem[l] = 0;
  if (!chained && st.primed[l] == 0) return;
  float sl[3];
  bool moving = false;
  for (uint32_t i = 0; i < 3; ++i) {
    float d;
    if (chained) {
      d = (v.v[i] - st.base[i][l]) / static_cast<float>(st.len[l]);
    } else {
      d = v.v[i] - st.prev[i][l];
      const float d2 = v.v[i] - 2.0f * st.prev[i][l] + st.prev2[i][l];
      if (!(std::fabs(d2) <= kSegTol * std::fabs(v.v[i]))) return;  // not on a line (NaN too)
    }
    if (!std::isfinite(d)) return;
    moving = moving || d != 0.0f;
    sl[i] = d;
  }
  if (!moving) return;
  // At exactly 0 dB a bell / shelf is the identity (no recursion: its states hold), a form the designs on either side do
  // not approach: no segment starts there or reaches it (the frames around it are designed exactly).
  if (k.gainMode && v.v[2] == 1.0f) return;
  // The length: the step bound and the room before a clamp (svf_design.h's sanitising) or a 0 dB crossing.
  const float sr = st.sampleRate[0];
  float len = static_cast<float>(kSegLen);
  auto room = [&](float x, float d, float lo, float hi) {  // frames until x + d·n leaves (lo, hi); false: outside
    if (!(x > lo && x < hi)) return false;
    if (d != 0.0f) len = std::min(len, d > 0.0f ? (hi - x) / d : (x - lo) / -d);
    return true;
  };
  constexpr float kPi = 3.14159265f;
  if (sl[0] != 0.0f) {  // cutoff: the relative step of tan(π·fc / sr)
    if (!room(v.v[0], sl[0], 1e-3f, 0.49f * sr)) return;
    const float w = kPi * v.v[0] / sr;
    len = std::min(len, kSegStep * sr * std::sin(2.0f * w) / (2.0f * kPi * std::fabs(sl[0])));
  }
  if (sl[1] != 0.0f) {
    if (!room(v.v[1], sl[1], 0.01f, 100.0f)) return;
    len = std::min(len, kSegStep * v.v[1] / std::fabs(sl[1]));
  }
  if (sl[2] != 0.0f) {  // gain modes only (the others hold it at 1)
    if (!room(v.v[2], sl[2], 1e-6f, 1e6f)) return;
    len = std::min(len, kSegStep * v.v[2] / std::fabs(sl[2]));
    if ((v.v[2] - 1.0f) * sl[2] < 0.0f) len = std::min(len, (1.0f - v.v[2]) / sl[2] - 1.0f);  // end a frame before 0 dB
  }
  const uint32_t n = !(len >= 2.0f) ? 0u : static_cast<uint32_t>(len) & ~1u;
  if (n < 2) return;
  startSegment<L>(st, l, k, v, sl, n);
}

// The segment's designs and forward differences (out of line: its 6 KiB of designs are not on the stack of the cheap
// checks above, which a curved input runs every frame).
template <uint32_t L>
__declspec(noinline) void startSegment(SvfState& st, uint32_t l, const LaneKey& k, const Inputs& v, const float* sl,
                                       uint32_t n) noexcept {
  using Ly = Layout<L>;
  Inputs mid, end;
  for (uint32_t i = 0; i < 3; ++i) {
    mid.v[i] = v.v[i] + sl[i] * static_cast<float>(n / 2);
    end.v[i] = v.v[i] + sl[i] * static_cast<float>(n);
  }
  SvfSection sm[kSvfMaxStages], se[kSvfMaxStages];
  const uint32_t cm = designSections(st, k, mid, sm);
  const uint32_t ce = designSections(st, k, end, se);
  if (cm != k.count || ce != k.count) return;
  // c(j) = a + B·j + C·j² through (0, a), (n/2, m), (n, b): first difference B + C, second 2·C.
  const double inv = 1.0 / n, inv2 = inv * inv;
  for (uint32_t s = 0; s < k.count; ++s) {
    const SvfKernelCoefs m = svfKernelCoefs(sm[s]), b = svfKernelCoefs(se[s]);
    const float mv[6] = {m.d1, m.b1, m.b2, m.m0, m.h1, m.h2};
    const float bv[6] = {b.d1, b.b1, b.b2, b.m0, b.h1, b.h2};
    for (uint32_t f = 0; f < 6; ++f) {
      const double a = *Ly::at(st, s, kD1 + f, l);
      const double B = (4.0 * mv[f] - 3.0 * a - bv[f]) * inv;
      const double C = (2.0 * a - 4.0 * mv[f] + 2.0 * bv[f]) * inv2;
      *Ly::at(st, s, kF1 + f, l) = static_cast<float>(B + C);
      *Ly::at(st, s, kF2 + f, l) = static_cast<float>(2.0 * C);
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    st.base[i][l] = v.v[i];
    st.slope[i][l] = sl[i];
  }
  st.len[l] = n;
  st.rem[l] = n;
}

// Lane l − 1 → l: everything but the signal states (lane l's logic state and inputs equalled l − 1's: it would have
// done the same).
template <uint32_t L>
void followLane(SvfState& st, uint32_t l, uint32_t sections) noexcept {
  using Ly = Layout<L>;
  for (uint32_t s = 0; s < sections; ++s)
    for (uint32_t f = kD1; f < kFieldEnd; ++f) *Ly::at(st, s, f, l) = *Ly::at(st, s, f, l - 1);
  auto* w = reinterpret_cast<uint32_t*>(&st);
  for (uint32_t a = 0; a < kSvfHeaderBytes / 32; ++a) w[a * 8 + l] = w[a * 8 + l - 1];
}

// Whether lanes l − 1 and l are in the same logic state (everything a frame's decisions read) with the same inputs.
bool sameLogic(const SvfState& st, uint32_t l, const Inputs& v, const Inputs& vPrev) noexcept {
  for (uint32_t i = 0; i < 3; ++i)
    if (!same(v.v[i], vPrev.v[i])) return false;
  const auto* w = reinterpret_cast<const uint32_t*>(&st);
  for (uint32_t a = 0; a < kSvfHeaderBytes / 32; ++a)
    if (w[a * 8 + l] != w[a * 8 + l - 1]) return false;
  return true;
}

template <class S>
struct Coefs {
  typename S::F d1, b1, b2, m0, h1, h2;
};
template <class S, uint32_t L>
Coefs<S> loadCoefs(SvfState& st, uint32_t s) noexcept {
  using Ly = Layout<L>;
  return {S::load(Ly::at(st, s, kD1, 0)), S::load(Ly::at(st, s, kB1, 0)), S::load(Ly::at(st, s, kB2, 0)),
          S::load(Ly::at(st, s, kM0, 0)), S::load(Ly::at(st, s, kH1, 0)), S::load(Ly::at(st, s, kH2, 0))};
}

// One section, one frame (the header's recursion).
template <class S>
inline typename S::F step(typename S::F x, typename S::F& ic1, typename S::F& ic2, const Coefs<S>& c) noexcept {
  const auto v = S::sub(x, ic2);
  const auto n1 = S::fma(c.b1, v, S::fma(c.d1, ic1, ic1));
  const auto n2 = S::fma(c.b2, v, S::fma(c.b1, ic1, ic2));
  const auto y = S::fma(c.h1, S::add(ic1, n1), S::fma(c.h2, S::add(ic2, n2), S::mul(c.m0, x)));
  ic1 = n1;
  ic2 = n2;
  return y;
}

// Double arithmetic for the deep cascades: 4 lanes of an 8-lane group (two halves) or the 1-lane scalar.
struct D4 {
  using F = __m256d;
  static constexpr uint32_t kW = 4;
  static F cvt(const float* p) noexcept { return _mm256_cvtps_pd(_mm_loadu_ps(p)); }
  static void put(float* p, F v) noexcept { _mm_storeu_ps(p, _mm256_cvtpd_ps(v)); }
  static F ld(const double* p) noexcept { return _mm256_loadu_pd(p); }
  static void st(double* p, F v) noexcept { _mm256_storeu_pd(p, v); }
  // A double per lane from its two 32-bit halves in two [8] arrays (lo, hi), and back.
  static F state(const float* lo, const float* hi) noexcept {
    const __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i*>(lo));
    const __m128i h = _mm_loadu_si128(reinterpret_cast<const __m128i*>(hi));
    return _mm256_castsi256_pd(_mm256_set_m128i(_mm_unpackhi_epi32(l, h), _mm_unpacklo_epi32(l, h)));
  }
  static void setState(float* lo, float* hi, F v) noexcept {
    const __m256 f = _mm256_castpd_ps(v);
    const __m128 a = _mm256_castps256_ps128(f), b = _mm256_extractf128_ps(f, 1);
    _mm_storeu_ps(lo, _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0)));
    _mm_storeu_ps(hi, _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1)));
  }
  static F add(F a, F b) noexcept { return _mm256_add_pd(a, b); }
  static F sub(F a, F b) noexcept { return _mm256_sub_pd(a, b); }
  static F mul(F a, F b) noexcept { return _mm256_mul_pd(a, b); }
  static F fma(F a, F b, F c) noexcept { return _mm256_fmadd_pd(a, b, c); }
  static F zero() noexcept { return _mm256_setzero_pd(); }
  static F keepSane(F v) noexcept {  // NaN, ±inf, |v| > 1e30 → 0
    const F a = _mm256_andnot_pd(_mm256_set1_pd(-0.0), v);
    return _mm256_and_pd(v, _mm256_cmp_pd(a, _mm256_set1_pd(1e30), _CMP_LE_OQ));
  }
};
struct D1 {
  using F = __m128d;
  static constexpr uint32_t kW = 1;
  static F cvt(const float* p) noexcept { return _mm_cvtss_sd(_mm_setzero_pd(), _mm_load_ss(p)); }
  static void put(float* p, F v) noexcept { _mm_store_ss(p, _mm_cvtsd_ss(_mm_setzero_ps(), v)); }
  static F ld(const double* p) noexcept { return _mm_load_sd(p); }
  static void st(double* p, F v) noexcept { _mm_store_sd(p, v); }
  // 1 lane: lo and hi are adjacent (Layout<1>), one little-endian double.
  static F state(const float* lo, const float*) noexcept { return _mm_load_sd(reinterpret_cast<const double*>(lo)); }
  static void setState(float* lo, float*, F v) noexcept { _mm_store_sd(reinterpret_cast<double*>(lo), v); }
  static F add(F a, F b) noexcept { return _mm_add_sd(a, b); }
  static F sub(F a, F b) noexcept { return _mm_sub_sd(a, b); }
  static F mul(F a, F b) noexcept { return _mm_mul_sd(a, b); }
  static F fma(F a, F b, F c) noexcept { return _mm_fmadd_sd(a, b, c); }
  static F zero() noexcept { return _mm_setzero_pd(); }
  static F keepSane(F v) noexcept {
    const F a = _mm_andnot_pd(_mm_set1_pd(-0.0), v);
    return _mm_and_pd(v, _mm_cmp_sd(a, _mm_set1_pd(1e30), _CMP_LE_OQ));
  }
};

template <class D, uint32_t L>
Coefs<D> loadCoefsD(SvfState& st, uint32_t s, uint32_t lane0) noexcept {
  using Ly = Layout<L>;
  return {D::cvt(Ly::at(st, s, kD1, lane0)), D::cvt(Ly::at(st, s, kB1, lane0)), D::cvt(Ly::at(st, s, kB2, lane0)),
          D::cvt(Ly::at(st, s, kM0, lane0)), D::cvt(Ly::at(st, s, kH1, lane0)), D::cvt(Ly::at(st, s, kH2, lane0))};
}

struct Svf {
  static constexpr uint32_t kSpec = 4;  // x, cutoff, q, gain: Blk or Spl; mode, stages, prewarp: Spl (consts)

  // ---- float ------------------------------------------------------------------------------------------------------
  // Sections [s0, s0 + P) over the whole call, states in registers; the first pass reads x, later ones `out` in place.
  template <class S, uint32_t P, bool kFirst, bool kXSpl>
  static void pass(SvfState& st, uint32_t s0, const float* x, float* out, uint32_t frames) noexcept {
    constexpr uint32_t L = S::kW;
    using Ly = Layout<L>;
    using F = typename S::F;
    F ic1[P], ic2[P];
    Coefs<S> c[P];
    for (uint32_t p = 0; p < P; ++p) {
      ic1[p] = S::load(Ly::at(st, s0 + p, kIc1, 0));
      ic2[p] = S::load(Ly::at(st, s0 + p, kIc2, 0));
      c[p] = loadCoefs<S, L>(st, s0 + p);
    }
    const F xs = kFirst && kXSpl ? S::load(x) : S::zero();
    for (uint32_t t = 0; t < frames; ++t) {
      F v = kFirst ? (kXSpl ? xs : S::load(x + static_cast<size_t>(t) * L)) : S::load(out + static_cast<size_t>(t) * L);
      for (uint32_t p = 0; p < P; ++p) v = step<S>(v, ic1[p], ic2[p], c[p]);
      S::store(out + static_cast<size_t>(t) * L, v);
    }
    for (uint32_t p = 0; p < P; ++p) {
      S::store(Ly::at(st, s0 + p, kIc1, 0), ic1[p]);
      S::store(Ly::at(st, s0 + p, kIc2, 0), ic2[p]);
    }
  }

  template <class S, bool kXSpl>
  static void cascade(SvfState& st, uint32_t n, const float* x, float* out, uint32_t frames) noexcept {
    uint32_t s = 0;
    if (n >= 2) {
      pass<S, 2, true, kXSpl>(st, 0, x, out, frames);
      s = 2;
    } else {
      pass<S, 1, true, kXSpl>(st, 0, x, out, frames);
      s = 1;
    }
    for (; s + 2 <= n; s += 2) pass<S, 2, false, kXSpl>(st, s, x, out, frames);
    if (s < n) pass<S, 1, false, kXSpl>(st, s, x, out, frames);
  }

  // ---- double (lanes [lane0, lane0 + D::kW)) ------------------------------------------------------------------------
  template <class D, uint32_t L, uint32_t P>
  static void passD(SvfState& st, uint32_t s0, uint32_t lane0, double* buf, uint32_t frames) noexcept {
    using Ly = Layout<L>;
    using F = typename D::F;
    F ic1[P], ic2[P];
    Coefs<D> c[P];
    for (uint32_t p = 0; p < P; ++p) {
      ic1[p] = D::state(Ly::at(st, s0 + p, kIc1, lane0), Ly::at(st, s0 + p, kIc1Hi, lane0));
      ic2[p] = D::state(Ly::at(st, s0 + p, kIc2, lane0), Ly::at(st, s0 + p, kIc2Hi, lane0));
      c[p] = loadCoefsD<D, L>(st, s0 + p, lane0);
    }
    for (uint32_t t = 0; t < frames; ++t) {
      F v = D::ld(buf + t * D::kW);
      for (uint32_t p = 0; p < P; ++p) v = step<D>(v, ic1[p], ic2[p], c[p]);
      D::st(buf + t * D::kW, v);
    }
    for (uint32_t p = 0; p < P; ++p) {
      D::setState(Ly::at(st, s0 + p, kIc1, lane0), Ly::at(st, s0 + p, kIc1Hi, lane0), ic1[p]);
      D::setState(Ly::at(st, s0 + p, kIc2, lane0), Ly::at(st, s0 + p, kIc2Hi, lane0), ic2[p]);
    }
  }

  // The whole call in chunks of 64 frames through a double buffer (x in, out at the end).
  template <class D, uint32_t L, bool kXSpl>
  static void cascadeD(SvfState& st, uint32_t n, uint32_t lane0, const float* x, float* out, uint32_t frames) noexcept {
    constexpr uint32_t kChunk = 64;
    alignas(32) double buf[kChunk * D::kW];
    for (uint32_t c0 = 0; c0 < frames; c0 += kChunk) {
      const uint32_t m = frames - c0 < kChunk ? frames - c0 : kChunk;
      for (uint32_t t = 0; t < m; ++t)
        D::st(buf + t * D::kW, D::cvt(x + (kXSpl ? 0 : static_cast<size_t>(c0 + t) * L) + lane0));
      uint32_t s = 0;
      for (; s + 2 <= n; s += 2) passD<D, L, 2>(st, s, lane0, buf, m);
      if (s < n) passD<D, L, 1>(st, s, lane0, buf, m);
      for (uint32_t t = 0; t < m; ++t) D::put(out + static_cast<size_t>(c0 + t) * L + lane0, D::ld(buf + t * D::kW));
    }
  }

  // One frame through every section, states in memory (the audio-rate path).
  template <class D, uint32_t L>
  static void frameD(SvfState& st, uint32_t n, uint32_t lane0, const float* x, float* out) noexcept {
    using Ly = Layout<L>;
    typename D::F v = D::cvt(x + lane0);
    for (uint32_t s = 0; s < n; ++s) {
      auto ic1 = D::state(Ly::at(st, s, kIc1, lane0), Ly::at(st, s, kIc1Hi, lane0));
      auto ic2 = D::state(Ly::at(st, s, kIc2, lane0), Ly::at(st, s, kIc2Hi, lane0));
      v = step<D>(v, ic1, ic2, loadCoefsD<D, L>(st, s, lane0));
      D::setState(Ly::at(st, s, kIc1, lane0), Ly::at(st, s, kIc1Hi, lane0), ic1);
      D::setState(Ly::at(st, s, kIc2, lane0), Ly::at(st, s, kIc2Hi, lane0), ic2);
    }
    D::put(out + lane0, v);
  }

  // ---- precision switches and the guard ------------------------------------------------------------------------------
  // A group whose section count crosses kDoubleFrom carries its states over (float <-> double) for sections [0, n),
  // in place (the float field becomes the double's lo half and back).
  template <uint32_t L>
  static void switchPrecision(SvfState& st, uint32_t n, bool toDouble) noexcept {
    using Ly = Layout<L>;
    for (uint32_t s = 0; s < n; ++s)
      for (uint32_t l = 0; l < L; ++l)
        for (uint32_t f : {kIc1, kIc2}) {
          float* lo = Ly::at(st, s, f, l);
          float* hi = Ly::at(st, s, f + 1, l);
          uint32_t w0, w1;
          if (toDouble) {
            const uint64_t b = std::bit_cast<uint64_t>(static_cast<double>(*lo));
            w0 = static_cast<uint32_t>(b);
            w1 = static_cast<uint32_t>(b >> 32);
            std::memcpy(lo, &w0, 4);
            std::memcpy(hi, &w1, 4);
          } else {
            std::memcpy(&w0, lo, 4);
            std::memcpy(&w1, hi, 4);
            *lo = static_cast<float>(std::bit_cast<double>(static_cast<uint64_t>(w1) << 32 | w0));
          }
        }
  }

  // Non-finite or runaway states (> 1e30) back to 0 (header).
  template <class S>
  static void guard(SvfState& st, uint32_t n) noexcept {
    constexpr uint32_t L = S::kW;
    using Ly = Layout<L>;
    const auto lim = S::set(1e30f);
    for (uint32_t s = 0; s < n; ++s)
      for (uint32_t f : {kIc1, kIc2}) {
        float* p = Ly::at(st, s, f, 0);
        const auto v = S::load(p);
        S::store(p, S::band(v, S::le(S::abs(v), lim)));
      }
  }
  template <class D, uint32_t L>
  static void guardD(SvfState& st, uint32_t n, uint32_t lane0) noexcept {
    using Ly = Layout<L>;
    for (uint32_t s = 0; s < n; ++s)
      for (uint32_t f : {kIc1, kIc2}) {
        float* lo = Ly::at(st, s, f, lane0);
        float* hi = Ly::at(st, s, f + 1, lane0);
        D::setState(lo, hi, D::keepSane(D::state(lo, hi)));
      }
  }

  // ---- coefficients ----------------------------------------------------------------------------------------------
  // The lane's inputs at frame offset o (gain 1 for the modes without one).
  template <bool kCSpl, bool kQSpl, bool kGSpl>
  static Inputs inputsAt(const KernelArgs& a, const LaneKey& k, size_t o, uint32_t l) noexcept {
    return {{a.in[1][(kCSpl ? 0 : o) + l], a.in[2][(kQSpl ? 0 : o) + l], k.gainMode ? a.in[3][(kGSpl ? 0 : o) + l] : 1.0f}};
  }

  // A frame's history (the line a segment starts on is the last three frames').
  static void pushHistory(SvfState& st, uint32_t l, const Inputs& v) noexcept {
    for (uint32_t i = 0; i < 3; ++i) {
      st.prev2[i][l] = st.prev[i][l];
      st.prev[i][l] = v.v[i];
    }
    st.primed[l] = 1;
  }

  // Param rate: lane l at inputs v for the whole call (`frames` frames): its exact design, no segment.
  template <uint32_t L>
  static void settle(SvfState& st, uint32_t l, const LaneKey& k, const Inputs& v, uint32_t frames) noexcept {
    if (st.key[l] != k.key || st.exact[l] == 0 || st.rem[l] != 0 || !sameInputs(st, l, v)) designExact<L>(st, l, k, v);
    pushHistory(st, l, v);
    if (frames >= 2) pushHistory(st, l, v);
  }

  // Per call: each lane's key word and whether its mode has a gain (all ones / 0), as [8] arrays.
  struct CallKeys {
    alignas(32) uint32_t key[8];
    alignas(32) uint32_t gainMode[8];
  };

  template <class S>
  static uint32_t bits(typename S::F m) noexcept {
    if constexpr (S::kW == 1) return static_cast<uint32_t>(_mm_movemask_ps(m)) & 1u;
    else return static_cast<uint32_t>(_mm256_movemask_ps(m));
  }

  // Audio rate, one frame: each lane's coefficients for the frame at offset o (see the header). The per-lane decisions
  // are made for all lanes at once (one vector each); designs, segment starts and follows are per lane.
  template <class S, bool kCSpl, bool kQSpl, bool kGSpl>
  static void frameCoefs(SvfState& st, const KernelArgs& a, const LaneKey* keys, const CallKeys& ck, uint32_t n,
                         size_t o) noexcept {
    constexpr uint32_t L = S::kW;
    using Ly = Layout<L>;
    using F = typename S::F;
    const F one = S::set(1.0f);
    const F tol = S::set(kSegTol);
    // The inputs (gain 1 for the modes without one), and the decisions (header): on the predicted line, needs a design.
    F v[3] = {S::load(a.in[1] + (kCSpl ? 0 : o)), S::load(a.in[2] + (kQSpl ? 0 : o)),
              S::select(S::asF(S::loadu32(ck.gainMode)), S::load(a.in[3] + (kGSpl ? 0 : o)), one)};
    const float* designed[3] = {st.cutoff, st.q, st.gain};
    // On the line: |v − (base + slope·j)| ≤ tol·|base + slope·j| for each input, in the same operations for any lane
    // count (no contraction), so 1 and 8 lanes decide alike.
    auto onLine = [&](F j) {
      F on = S::ones();
      for (uint32_t i = 0; i < 3; ++i) {
        const F pred = S::add(S::load(st.base[i]), S::mul(S::load(st.slope[i]), j));
        on = S::band(on, S::le(S::abs(S::sub(v[i], pred)), S::mul(tol, S::abs(pred))));
      }
      return on;
    };
    uint32_t adv = 0, evt = 0, done = 0;
    F advM;
    if constexpr (L == 1) {  // branches: one lane's case is known early
      if (st.key[0] != ck.key[0]) {
        evt = 1;
      } else if (st.rem[0] != 0) {
        if (bits<S>(onLine(S::set(static_cast<float>(st.len[0] - st.rem[0] + 1)))) != 0) {
          adv = 1;
          done = st.rem[0] == 1 ? 1u : 0u;
        } else {
          evt = 1;
        }
      } else if (st.exact[0] == 0 || !sameInputs(st, 0, Inputs{{_mm_cvtss_f32(v[0]), _mm_cvtss_f32(v[1]), _mm_cvtss_f32(v[2])}})) {
        evt = 1;
      }
      advM = adv != 0 ? S::ones() : S::zero();
    } else {
      const auto rem = S::loadu32(st.rem);
      const F inSeg = S::bnot(S::asF(S::eqi(rem, S::seti(0))));
      const F keyOk = S::asF(S::eqi(S::loadu32(st.key), S::loadu32(ck.key)));
      const F on = onLine(S::add(S::toFloat(S::subi(S::loadu32(st.len), rem)), one));
      F same = S::ones();
      for (uint32_t i = 0; i < 3; ++i) same = S::band(same, S::asF(S::eqi(S::asI(v[i]), S::asI(S::load(designed[i])))));
      const F exact = S::bnot(S::asF(S::eqi(S::loadu32(st.exact), S::seti(0))));
      advM = S::band(keyOk, S::band(inSeg, on));
      const F evtM = S::bor(S::bnot(keyOk), S::bor(S::band(inSeg, S::bnot(on)), S::andnot(inSeg, S::bnot(S::band(same, exact)))));
      adv = bits<S>(advM);
      evt = bits<S>(evtM);
      done = adv & bits<S>(S::asF(S::eqi(rem, S::seti(1))));
    }
    alignas(32) float vv[3][8];
    for (uint32_t i = 0; i < 3; ++i) S::store(vv[i], v[i]);
    auto inputs = [&](uint32_t l) { return Inputs{{vv[0][l], vv[1][l], vv[2][l]}}; };
    uint32_t follow = 0;
    if constexpr (L > 1) {  // a lane in lane l − 1's state with its inputs follows it (one design for a voice group)
      for (uint32_t m = (evt | done) & ~1u; m != 0; m &= m - 1) {
        const uint32_t l = static_cast<uint32_t>(std::countr_zero(m));
        if (sameLogic(st, l, inputs(l), inputs(l - 1))) follow |= 1u << l;
      }
    }
    if (adv != 0) {
      // c += Δ1, Δ1 += Δ2 for the lanes on their line.
      for (uint32_t s = 0; s < n; ++s)
        for (uint32_t f = 0; f < 6; ++f) {
          float* c = Ly::at(st, s, kD1 + f, 0);
          float* d1 = Ly::at(st, s, kF1 + f, 0);
          const F dv = S::load(d1);
          S::store(c, S::add(S::load(c), S::band(dv, advM)));
          S::store(d1, S::add(dv, S::band(S::load(Ly::at(st, s, kF2 + f, 0)), advM)));
        }
      // One frame further along: the inputs are those of the (interpolated) coefficients now.
      S::storeu32(st.rem, S::subi(S::loadu32(st.rem), S::andi(S::asI(advM), S::seti(1))));
      for (uint32_t i = 0; i < 3; ++i) S::store(const_cast<float*>(designed[i]), S::select(advM, v[i], S::load(designed[i])));
      S::storeu32(st.exact, S::andi(S::loadu32(st.exact), S::asI(S::bnot(advM))));
    }
    for (uint32_t m = evt | done | follow; m != 0; m &= m - 1) {
      const uint32_t l = static_cast<uint32_t>(std::countr_zero(m));
      const uint32_t bit = 1u << l;
      if ((follow & bit) != 0) {
        followLane<L>(st, l, n);
      } else if ((done & bit) != 0) {
        openSegment<L>(st, l, keys[l], inputs(l), true);
      } else {
        designExact<L>(st, l, keys[l], inputs(l));
        openSegment<L>(st, l, keys[l], inputs(l), false);
      }
    }
    // The history, after the follows (they copy lane l − 1's from before it).
    for (uint32_t i = 0; i < 3; ++i) {
      S::store(st.prev2[i], S::load(st.prev[i]));
      S::store(st.prev[i], v[i]);
    }
    S::storeu32(st.primed, S::seti(1));
  }

  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    constexpr uint32_t L = S::kW;
    constexpr bool kXSpl = splBit(SM, 0);
    constexpr bool kCSpl = splBit(SM, 1), kQSpl = splBit(SM, 2), kGSpl = splBit(SM, 3);
    constexpr bool kAllSpl = kCSpl && kQSpl && kGSpl;
    using D = std::conditional_t<L == 1, D1, D4>;
    constexpr uint32_t kHalves = L == 1 ? 1 : 2;
    if (a.frames == 0) return;
    SvfState& st = *static_cast<SvfState*>(a.state);
    const uint32_t cap = st.sections[0] < 1 ? 1u : st.sections[0] > kSvfStateSections ? kSvfStateSections : st.sections[0];
    LaneKey keys[L];
    uint32_t n = 1, precision = 0;
    for (uint32_t l = 0; l < L; ++l) {
      keys[l] = laneKey(a.in[4][l], a.in[5][l], a.in[6][l], st.sampleRate[0], cap);
      n = keys[l].count > n ? keys[l].count : n;
      precision |= st.precision[l];  // a lane reset to the initial state holds zeros: valid in either precision
    }
    const bool dbl = n >= kDoubleFrom;
    if ((dbl ? 1u : 0u) != precision) switchPrecision<L>(st, n, dbl);
    for (uint32_t l = 0; l < L; ++l) st.precision[l] = dbl ? 1u : 0u;
    // Sections past a lane's count are the identity once that lane's key is designed; a lane whose key is new gets
    // them written by its design below, before any section runs.
    const float* x = a.in[0];
    float* out = a.out[0];
    // A Blk call whose inputs are constant over it and equal to the frame before, no lane in a segment: param rate
    // (exactly what the frame loop would do, without it).
    bool param = kAllSpl;
    if constexpr (!kAllSpl) {
      param = true;
      for (uint32_t l = 0; l < L && param; ++l) {
        const Inputs v0 = inputsAt<kCSpl, kQSpl, kGSpl>(a, keys[l], 0, l);
        param = st.rem[l] == 0 && (st.primed[l] == 0 || (same(v0.v[0], st.prev[0][l]) && same(v0.v[1], st.prev[1][l]) &&
                                                         same(v0.v[2], st.prev[2][l])));
      }
      auto constant = [&](const float* p) {
        for (uint32_t t = 1; t < a.frames; ++t)
          for (uint32_t l = 0; l < L; ++l)
            if (!same(p[static_cast<size_t>(t) * L + l], p[l])) return false;
        return true;
      };
      if (param && !kCSpl) param = constant(a.in[1]);
      if (param && !kQSpl) param = constant(a.in[2]);
      if (param && !kGSpl) {
        bool anyGain = false;
        for (uint32_t l = 0; l < L; ++l) anyGain = anyGain || keys[l].gainMode;
        if (anyGain) param = constant(a.in[3]);
      }
    }
    if (param) {
      for (uint32_t l = 0; l < L; ++l)
        settle<L>(st, l, keys[l], inputsAt<kCSpl, kQSpl, kGSpl>(a, keys[l], 0, l), a.frames);
      if (!dbl) {
        cascade<S, kXSpl>(st, n, x, out, a.frames);
      } else {
        for (uint32_t h = 0; h < kHalves; ++h) cascadeD<D, L, kXSpl>(st, n, h * D::kW, x, out, a.frames);
      }
    } else if constexpr (!kAllSpl) {
      using Ly = Layout<L>;
      using F = typename S::F;
      CallKeys ck;
      for (uint32_t l = 0; l < 8; ++l) {
        ck.key[l] = keys[l < L ? l : 0].key;
        ck.gainMode[l] = keys[l < L ? l : 0].gainMode ? 0xFFFFFFFFu : 0u;
      }
      for (uint32_t t = 0; t < a.frames; ++t) {
        const size_t o = static_cast<size_t>(t) * L;
        frameCoefs<S, kCSpl, kQSpl, kGSpl>(st, a, keys, ck, n, o);
        const float* xt = x + (kXSpl ? 0 : o);
        if (dbl) {
          for (uint32_t h = 0; h < kHalves; ++h) frameD<D, L>(st, n, h * D::kW, xt, out + o);
          continue;
        }
        F v = S::load(xt);
        for (uint32_t s = 0; s < n; ++s) {
          F ic1 = S::load(Ly::at(st, s, kIc1, 0)), ic2 = S::load(Ly::at(st, s, kIc2, 0));
          v = step<S>(v, ic1, ic2, loadCoefs<S, L>(st, s));
          S::store(Ly::at(st, s, kIc1, 0), ic1);
          S::store(Ly::at(st, s, kIc2, 0), ic2);
        }
        S::store(out + o, v);
      }
    }
    if (!dbl) {
      guard<S>(st, n);
    } else {
      for (uint32_t h = 0; h < kHalves; ++h) guardD<D, L>(st, n, h * D::kW);
    }
  }
};

// Every lane's states of every section (in the precision they are in).
template <uint32_t L>
bool stateSmall(SvfState& st, float eps) noexcept {
  using Ly = Layout<L>;
  const uint32_t n = st.sections[0] < kSvfStateSections ? st.sections[0] : kSvfStateSections;
  const bool dbl = st.precision[0] != 0;
  for (uint32_t s = 0; s < n; ++s)
    for (uint32_t l = 0; l < L; ++l)
      for (uint32_t f : {kIc1, kIc2}) {
        double v;
        if (dbl) {
          uint32_t w0, w1;
          std::memcpy(&w0, Ly::at(st, s, f, l), 4);
          std::memcpy(&w1, Ly::at(st, s, f + 1, l), 4);
          v = std::bit_cast<double>(static_cast<uint64_t>(w1) << 32 | w0);
        } else {
          v = *Ly::at(st, s, f, l);
        }
        if (!(std::fabs(v) <= eps)) return false;
      }
  return true;
}
template <uint32_t L>
bool stateFlush(SvfState& st) noexcept {
  using Ly = Layout<L>;
  const uint32_t n = st.sections[0] < kSvfStateSections ? st.sections[0] : kSvfStateSections;
  uint32_t any = 0;
  for (uint32_t s = 0; s < n; ++s)
    for (uint32_t f : {kIc1, kIc1Hi, kIc2, kIc2Hi})
      for (uint32_t l = 0; l < L; ++l) {
        float* p = Ly::at(st, s, f, l);
        any |= std::bit_cast<uint32_t>(*p);
        *p = 0.0f;
      }
  return any != 0;
}

}  // namespace

KernelFn findSvfKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept {
  return a == Atom::Svf ? pick<Svf>(lanes, specMask) : nullptr;
}

bool svfStateSmall(const void* state, float eps) noexcept {  // RT-safe
  auto& st = *static_cast<SvfState*>(const_cast<void*>(state));
  return st.lanes[0] == 8 ? stateSmall<8>(st, eps) : stateSmall<1>(st, eps);
}

bool svfStateFlush(void* state) noexcept {  // RT-safe
  auto& st = *static_cast<SvfState*>(state);
  return st.lanes[0] == 8 ? stateFlush<8>(st) : stateFlush<1>(st);
}

}  // namespace pq::dsp::detail
