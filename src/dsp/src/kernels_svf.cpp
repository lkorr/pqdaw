// svf (EQ.1): N cascaded TPT state-variable sections with an output mix (pq/dsp/svf_design.h; semantics in atoms.h).
//
// Coefficients: designed per lane (svfDesign, double) when the lane's inputs differ bitwise from the ones its
// coefficients were made for, so a param-rate filter designs once per change and costs nothing per block at rest; an
// audio-rate cutoff / q / gain (a Blk input: modulation, or the compiler's param ramp while a knob moves) designs per
// frame, which with the matched prewarp is the expensive case (use bilinear for audio-rate sweeps; bench/RESULTS.md).
// 8 lanes whose inputs agree share one design (copied lane to lane).
//
// Per sample and section (svf_fast.pqp's shallow form: each new state two multiply-adds deep):
//   v = x − ic2;  n1 = b1·v + (d1·ic1 + ic1);  n2 = b2·v + (b1·ic1 + ic2);  y = h1·(ic1 + n1) + (h2·(ic2 + n2) + m0·x)
// Param rate: sections outer in passes of two (their states in registers over the block; the two recursions
// interleave), frames inner. Audio rate: frames outer (each frame's design first), sections inner. 8 lanes: one AVX2
// vector per section and frame for the whole voice group.
//
// Precision: up to kDoubleFrom − 1 sections run in float. A deep cascade amplifies its float rounding noise by the gain
// of the sections after it (a 64-section Butterworth lowpass: the float output is noise near the cutoff; svf_design.h
// orders the sections to keep partial gains low, which leaves 16 sections accurate to ~1e−6), so from kDoubleFrom
// sections on the whole cascade runs in double (states kept as exact doubles, split into two [8] arrays of 32-bit
// halves so that every state word stays per lane): 8 lanes as two __m256d halves, so about twice the float cost.
//
// Any split of a block into calls gives the same bits: the designs depend only on each frame's inputs, states are kept
// exactly between calls, and the per-call guard below only touches non-finite or runaway states.
//
// Denormals: FTZ / DAZ on the audio threads flush the decaying states to 0; a state that became NaN or ±inf (a NaN
// input) or exceeds 1e30 is reset to 0 at the end of the call, so one bad sample does not silence the filter forever.

#include <bit>
#include <cmath>
#include <cstring>
#include <type_traits>

#include "kernels_common.h"
#include "pq/dsp/svf_design.h"

namespace pq::dsp::detail {
namespace {

static_assert(kSvfStateSections == kSvfMaxStages);

// Per section: the float states, the six coefficients, the double states as 32-bit halves (lo, hi; adjacent for 1 lane,
// so a 1-lane double state is one aligned 8-byte word).
enum Field : uint32_t { kIc1, kIc2, kD1, kB1, kB2, kM0, kH1, kH2, kIc1Lo, kIc1Hi, kIc2Lo, kIc2Hi, kFields };
constexpr uint32_t kDoubleFrom = 17;  // sections from which the cascade runs in double

template <uint32_t L>
struct Layout {
  static constexpr uint32_t kSec = L == 1 ? kFields : kFields * 8;  // floats per section
  static constexpr uint32_t kFld = L == 1 ? 1 : 8;                   // floats per field
  static float* at(SvfState& st, uint32_t s, uint32_t f, uint32_t lane) noexcept {
    return st.sec + s * kSec + f * kFld + lane;
  }
};

struct LaneKey {
  SvfParams p;     // mode, prewarp, stages (the rest is filled per design)
  uint32_t key;
  uint32_t count;
};
LaneKey laneKey(float mode, float stages, float prewarp, double sr) noexcept {
  LaneKey k{};
  k.p = svfParamsFromInputs(1000.0f, 1.0f, 1.0f, mode, stages, prewarp, sr);
  k.count = svfSectionCount(k.p.mode, k.p.stages);
  k.key = 1u | static_cast<uint32_t>(k.p.mode) << 1 | static_cast<uint32_t>(k.p.prewarp) << 4 | k.p.stages << 8;
  return k;
}

bool same(float a, float b) noexcept { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

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

// Makes lane `l`'s coefficients those of (k, cutoff, q, gain): nothing when they already are, a copy of lane l − 1's
// when its inputs are the same, else a design. A new key also writes the identity into the sections past its count.
template <uint32_t L>
void ensure(SvfState& st, uint32_t l, const LaneKey& k, float cutoff, float q, float gain) noexcept {
  if (st.key[l] == k.key && same(st.cutoff[l], cutoff) && same(st.q[l], q) && same(st.gain[l], gain)) return;
  const bool newKey = st.key[l] != k.key;
  const uint32_t upto = newKey ? kSvfMaxStages : k.count;
  if constexpr (L > 1) {
    if (l > 0 && st.key[l - 1] == k.key && same(st.cutoff[l - 1], cutoff) && same(st.q[l - 1], q) &&
        same(st.gain[l - 1], gain)) {
      for (uint32_t s = 0; s < upto; ++s)
        for (uint32_t f = kD1; f <= kH2; ++f) *Layout<L>::at(st, s, f, l) = *Layout<L>::at(st, s, f, l - 1);
      st.key[l] = k.key;
      st.count[l] = k.count;
      st.cutoff[l] = cutoff;
      st.q[l] = q;
      st.gain[l] = gain;
      return;
    }
  }
  const SvfParams p = svfParamsFromInputs(cutoff, q, gain, static_cast<float>(k.p.mode), static_cast<float>(k.p.stages),
                                          k.p.prewarp == SvfPrewarp::Bilinear ? 1.0f : 0.0f, st.sampleRate);
  SvfSection secs[kSvfMaxStages];
  const uint32_t n = svfDesign(p, secs);
  for (uint32_t s = 0; s < n; ++s) writeCoefs<L>(st, s, l, svfKernelCoefs(secs[s]));
  for (uint32_t s = n; s < upto; ++s) writeCoefs<L>(st, s, l, SvfKernelCoefs{});
  st.key[l] = k.key;
  st.count[l] = k.count;
  st.cutoff[l] = cutoff;
  st.q[l] = q;
  st.gain[l] = gain;
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
      ic1[p] = D::state(Ly::at(st, s0 + p, kIc1Lo, lane0), Ly::at(st, s0 + p, kIc1Hi, lane0));
      ic2[p] = D::state(Ly::at(st, s0 + p, kIc2Lo, lane0), Ly::at(st, s0 + p, kIc2Hi, lane0));
      c[p] = loadCoefsD<D, L>(st, s0 + p, lane0);
    }
    for (uint32_t t = 0; t < frames; ++t) {
      F v = D::ld(buf + t * D::kW);
      for (uint32_t p = 0; p < P; ++p) v = step<D>(v, ic1[p], ic2[p], c[p]);
      D::st(buf + t * D::kW, v);
    }
    for (uint32_t p = 0; p < P; ++p) {
      D::setState(Ly::at(st, s0 + p, kIc1Lo, lane0), Ly::at(st, s0 + p, kIc1Hi, lane0), ic1[p]);
      D::setState(Ly::at(st, s0 + p, kIc2Lo, lane0), Ly::at(st, s0 + p, kIc2Hi, lane0), ic2[p]);
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
      auto ic1 = D::state(Ly::at(st, s, kIc1Lo, lane0), Ly::at(st, s, kIc1Hi, lane0));
      auto ic2 = D::state(Ly::at(st, s, kIc2Lo, lane0), Ly::at(st, s, kIc2Hi, lane0));
      v = step<D>(v, ic1, ic2, loadCoefsD<D, L>(st, s, lane0));
      D::setState(Ly::at(st, s, kIc1Lo, lane0), Ly::at(st, s, kIc1Hi, lane0), ic1);
      D::setState(Ly::at(st, s, kIc2Lo, lane0), Ly::at(st, s, kIc2Hi, lane0), ic2);
    }
    D::put(out + lane0, v);
  }

  // ---- precision switches and the guard ------------------------------------------------------------------------------
  // A group whose section count crosses kDoubleFrom carries its states over (float <-> double) for sections [0, n).
  template <uint32_t L>
  static void switchPrecision(SvfState& st, uint32_t n, bool toDouble) noexcept {
    using Ly = Layout<L>;
    constexpr uint32_t kPairs[2][3] = {{kIc1, kIc1Lo, kIc1Hi}, {kIc2, kIc2Lo, kIc2Hi}};
    for (uint32_t s = 0; s < n; ++s)
      for (uint32_t l = 0; l < L; ++l)
        for (const auto& f : kPairs) {
          float* fl = Ly::at(st, s, f[0], l);
          float* lo = Ly::at(st, s, f[1], l);
          float* hi = Ly::at(st, s, f[2], l);
          uint32_t w0, w1;
          if (toDouble) {
            const uint64_t b = std::bit_cast<uint64_t>(static_cast<double>(*fl));
            w0 = static_cast<uint32_t>(b);
            w1 = static_cast<uint32_t>(b >> 32);
            std::memcpy(lo, &w0, 4);
            std::memcpy(hi, &w1, 4);
          } else {
            std::memcpy(&w0, lo, 4);
            std::memcpy(&w1, hi, 4);
            *fl = static_cast<float>(std::bit_cast<double>(static_cast<uint64_t>(w1) << 32 | w0));
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
      for (uint32_t f : {kIc1Lo, kIc2Lo}) {
        float* lo = Ly::at(st, s, f, lane0);
        float* hi = Ly::at(st, s, f + 1, lane0);
        D::setState(lo, hi, D::keepSane(D::state(lo, hi)));
      }
  }

  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    constexpr uint32_t L = S::kW;
    constexpr bool kXSpl = splBit(SM, 0);
    constexpr bool kCSpl = splBit(SM, 1), kQSpl = splBit(SM, 2), kGSpl = splBit(SM, 3);
    using D = std::conditional_t<L == 1, D1, D4>;
    constexpr uint32_t kHalves = L == 1 ? 1 : 2;
    if (a.frames == 0) return;
    SvfState& st = *static_cast<SvfState*>(a.state);
    LaneKey keys[L];
    uint32_t n = 1;
    for (uint32_t l = 0; l < L; ++l) {
      keys[l] = laneKey(a.in[4][l], a.in[5][l], a.in[6][l], st.sampleRate);
      n = keys[l].count > n ? keys[l].count : n;
    }
    const bool dbl = n >= kDoubleFrom;
    if ((dbl ? 1u : 0u) != st.precision) {
      switchPrecision<L>(st, n, dbl);
      st.precision = dbl ? 1u : 0u;
    }
    // Sections past a lane's count are the identity once that lane's key is designed; a lane whose key is new gets
    // them written by its design below, before any section runs.
    const float* x = a.in[0];
    float* out = a.out[0];
    if constexpr (kCSpl && kQSpl && kGSpl) {
      for (uint32_t l = 0; l < L; ++l) ensure<L>(st, l, keys[l], a.in[1][l], a.in[2][l], a.in[3][l]);
      if (!dbl) {
        cascade<S, kXSpl>(st, n, x, out, a.frames);
      } else {
        for (uint32_t h = 0; h < kHalves; ++h) cascadeD<D, L, kXSpl>(st, n, h * D::kW, x, out, a.frames);
      }
    } else {
      using Ly = Layout<L>;
      using F = typename S::F;
      for (uint32_t t = 0; t < a.frames; ++t) {
        const size_t o = static_cast<size_t>(t) * L;
        for (uint32_t l = 0; l < L; ++l)
          ensure<L>(st, l, keys[l], a.in[1][(kCSpl ? 0 : o) + l], a.in[2][(kQSpl ? 0 : o) + l],
                    a.in[3][(kGSpl ? 0 : o) + l]);
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

}  // namespace

KernelFn findSvfKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept {
  return a == Atom::Svf ? pick<Svf>(lanes, specMask) : nullptr;
}

}  // namespace pq::dsp::detail
