#pragma once

// Shared machinery for the leaf-atom kernels (private to pq_dsp): the element-wise driver and the
// template-instantiation tables behind findKernel.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "pq/dsp/atoms.h"
#include "pq/dsp/kernel.h"
#include "simd.h"

namespace pq::dsp::detail {

using simd::In;
using simd::S1;
using simd::S8;

constexpr bool splBit(uint32_t mask, uint32_t i) noexcept { return ((mask >> i) & 1u) != 0; }

// True if any bool mask in p[0 .. count) is set (OR-reduction, 8 floats per step). Lets a kernel with a Blk
// bool input (reset, sync) take its no-event fast path for blocks where the input never fires. RT-safe.
inline bool blockAny(const float* p, uint32_t count) noexcept {
  const uint32_t vecEnd = count & ~7u;
  __m256 acc = _mm256_setzero_ps();
  for (uint32_t i = 0; i < vecEnd; i += 8) acc = _mm256_or_ps(acc, _mm256_loadu_ps(p + i));
  bool any = _mm256_movemask_ps(acc) != 0;
  for (uint32_t i = vecEnd; i < count; ++i) any |= std::bit_cast<uint32_t>(p[i]) != 0;
  return any;
}

// ---- Element-wise driver ----------------------------------------------------------------------------------
// Stateless atoms do not care how lanes are laid out, so one loop serves both lane counts: `count` = frames·L
// floats, processed 8 at a time with AVX2 (for L = 1 that vectorises over time), and the remainder (only
// possible for L = 1) with the S1 scalar path. Op provides `kArity` and `template <class S> F f(F...)`.
template <class Op, uint32_t SM>
inline void elementwise(const KernelArgs& a, uint32_t count) noexcept {  // RT-safe
  float* out = a.out[0];
  const uint32_t vecEnd = count & ~7u;
  if constexpr (Op::kArity == 1) {
    const In<S8, splBit(SM, 0)> x(a.in[0]);
    for (uint32_t i = 0; i < vecEnd; i += 8) S8::store(out + i, Op::template f<S8>(x.at(i >> 3)));
    for (uint32_t i = vecEnd; i < count; ++i) {
      const float* px = splBit(SM, 0) ? a.in[0] : a.in[0] + i;
      S1::store(out + i, Op::template f<S1>(S1::load(px)));
    }
  } else if constexpr (Op::kArity == 2) {
    const In<S8, splBit(SM, 0)> x(a.in[0]);
    const In<S8, splBit(SM, 1)> y(a.in[1]);
    uint32_t i0 = 0;
    for (; i0 + 16 <= vecEnd; i0 += 16) {
      const auto r0 = Op::template f<S8>(x.at(i0 >> 3), y.at(i0 >> 3));
      const auto r1 = Op::template f<S8>(x.at((i0 >> 3) + 1), y.at((i0 >> 3) + 1));
      S8::store(out + i0, r0);
      S8::store(out + i0 + 8, r1);
    }
    for (uint32_t i = i0; i < vecEnd; i += 8) S8::store(out + i, Op::template f<S8>(x.at(i >> 3), y.at(i >> 3)));
    for (uint32_t i = vecEnd; i < count; ++i) {
      const float* px = splBit(SM, 0) ? a.in[0] : a.in[0] + i;
      const float* py = splBit(SM, 1) ? a.in[1] : a.in[1] + i;
      S1::store(out + i, Op::template f<S1>(S1::load(px), S1::load(py)));
    }
  } else {
    static_assert(Op::kArity == 3);
    const In<S8, splBit(SM, 0)> x(a.in[0]);
    const In<S8, splBit(SM, 1)> y(a.in[1]);
    const In<S8, splBit(SM, 2)> z(a.in[2]);
    for (uint32_t i = 0; i < vecEnd; i += 8)
      S8::store(out + i, Op::template f<S8>(x.at(i >> 3), y.at(i >> 3), z.at(i >> 3)));
    for (uint32_t i = vecEnd; i < count; ++i) {
      const float* px = splBit(SM, 0) ? a.in[0] : a.in[0] + i;
      const float* py = splBit(SM, 1) ? a.in[1] : a.in[1] + i;
      const float* pz = splBit(SM, 2) ? a.in[2] : a.in[2] + i;
      S1::store(out + i, Op::template f<S1>(S1::load(px), S1::load(py), S1::load(pz)));
    }
  }
}

// OP.6: the same driver for an Op whose f is an expensive pure function (the SVML transcendentals: cents → Hz, exp,
// pow, log, tan, the dB conversions) of inputs that mostly hold still (a held voice's cents, a param). A vector of 8
// inputs whose bits equal the previous vector's reuses the previous result: the same function of the same values, so
// the output is bit for bit what `elementwise` writes (compared as integers: NaN payloads and ±0 count as different).
// For L = 8 that is "no lane moved since the last frame", for L = 1 "these 8 frames repeat the last 8". A moving
// input pays one xor / or / ptest and a predicted branch per vector on top of the call. Unary and binary Ops.
template <class Op, uint32_t SM>
inline void elementwiseMemo(const KernelArgs& a, uint32_t count) noexcept {  // RT-safe
  static_assert(Op::kArity == 1 || Op::kArity == 2);
  float* out = a.out[0];
  const uint32_t vecEnd = count & ~7u;
  constexpr bool xSpl = splBit(SM, 0);
  constexpr bool ySpl = Op::kArity == 1 || splBit(SM, 1);
  const In<S8, xSpl> x(a.in[0]);
  const In<S8, ySpl> y(a.in[Op::kArity - 1]);  // (unary: the same input again, never compared)
  auto f = [](__m256 vx, __m256 vy) noexcept {
    if constexpr (Op::kArity == 1) {
      (void)vy;
      return Op::template f<S8>(vx);
    } else {
      return Op::template f<S8>(vx, vy);
    }
  };
  if (vecEnd != 0) {
    __m256 px = x.at(0), py = y.at(0);
    __m256 r = f(px, py);
    S8::store(out, r);
    uint32_t i = 8;
    // Two vectors per step (as `elementwise` runs a binary Op): both repeat the last one, or both are computed. A
    // moving input then pays the compare and the spills around the calls once per 16 values.
    for (; i + 16 <= vecEnd; i += 16) {
      const __m256 ax = x.at(i >> 3), ay = y.at(i >> 3), bx = x.at((i >> 3) + 1), by = y.at((i >> 3) + 1);
      __m256i moved = _mm256_setzero_si256();
      if constexpr (!xSpl) moved = _mm256_castps_si256(_mm256_or_ps(_mm256_xor_ps(ax, px), _mm256_xor_ps(bx, px)));
      if constexpr (!ySpl)
        moved = _mm256_or_si256(moved, _mm256_castps_si256(_mm256_or_ps(_mm256_xor_ps(ay, py), _mm256_xor_ps(by, py))));
      if (!_mm256_testz_si256(moved, moved)) {
        const __m256 ra = f(ax, ay);
        r = f(bx, by);
        S8::store(out + i, ra);
        S8::store(out + i + 8, r);
        px = bx;
        py = by;
      } else {
        S8::store(out + i, r);
        S8::store(out + i + 8, r);
      }
    }
    for (; i < vecEnd; i += 8) {
      const __m256 cx = x.at(i >> 3), cy = y.at(i >> 3);
      __m256i moved = _mm256_setzero_si256();
      if constexpr (!xSpl) moved = _mm256_castps_si256(_mm256_xor_ps(cx, px));
      if constexpr (!ySpl) moved = _mm256_or_si256(moved, _mm256_castps_si256(_mm256_xor_ps(cy, py)));
      if (!_mm256_testz_si256(moved, moved)) {
        r = f(cx, cy);
        px = cx;
        py = cy;
      }
      S8::store(out + i, r);
    }
  }
  for (uint32_t i = vecEnd; i < count; ++i) {  // (L = 1 only)
    const float* px = xSpl ? a.in[0] : a.in[0] + i;
    if constexpr (Op::kArity == 1) {
      S1::store(out + i, Op::template f<S1>(S1::load(px)));
    } else {
      const float* py = ySpl ? a.in[1] : a.in[1] + i;
      S1::store(out + i, Op::template f<S1>(S1::load(px), S1::load(py)));
    }
  }
}

// Adapts an element-wise Op to the Impl interface used by the tables below. An Op that declares `kMemo` runs the
// repeated-input driver.
template <class Op>
struct Ew {
  static constexpr uint32_t kSpec = Op::kArity;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    if constexpr (requires { Op::kMemo; }) {
      elementwiseMemo<Op, SM>(a, a.frames * S::kW);
    } else {
      elementwise<Op, SM>(a, a.frames * S::kW);
    }
  }
};

// ---- Instantiation tables ------------------------------------------------------------------------------------
// Impl provides kSpec (number of leading inputs whose Blk/Spl choice is specialised) and
// template <class S, uint32_t SM> static void run(const KernelArgs&) noexcept.
template <class Impl, class S, uint32_t SM>
void entry(const KernelArgs& a) noexcept {  // RT-safe
  Impl::template run<S, SM>(a);
}

template <class Impl, std::size_t... M>
constexpr std::array<KernelFn, 2 * sizeof...(M)> makeTable(std::index_sequence<M...>) {
  return {&entry<Impl, S1, static_cast<uint32_t>(M)>..., &entry<Impl, S8, static_cast<uint32_t>(M)>...};
}

// specMask must only contain bits below Impl::kSpec (findKernel guarantees it).
template <class Impl>
KernelFn pick(uint32_t lanes, uint32_t specMask) noexcept {
  constexpr uint32_t n = 1u << Impl::kSpec;
  static constexpr auto table = makeTable<Impl>(std::make_index_sequence<n>{});
  if (specMask >= n) return nullptr;
  if (lanes == 1) return table[specMask];
  if (lanes == kGroupLanes) return table[n + specMask];
  return nullptr;
}

// Per-family lookups (each defined in its own translation unit). specMask = Spl bits of the specialised inputs.
KernelFn findMathKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;
KernelFn findLogicKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;
KernelFn findMemoryKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;
KernelFn findGeneratorKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;
KernelFn findTableKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;
KernelFn findGainKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;  // gain, gain_add (kernels_gain.cpp)
KernelFn findOversampleKernel(Atom a, uint32_t lanes) noexcept;  // TMM.6 converters (kernels_oversample.cpp)
// tuning_hz (kernels_tuning.cpp): one function per lane count, any Spl mask.
KernelFn findTuningKernel(uint32_t lanes) noexcept;
KernelFn findFrameTableKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;  // TMM.2 (kernels_frame_table.cpp)
KernelFn findWaveLiveKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;    // TMM.8 (kernels_wave_live.cpp)
KernelFn findRampKernel(Atom a, uint32_t lanes) noexcept;  // TX.smooth: param_ramp (kernels_ramp.cpp)
KernelFn findSignalTapKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;  // TX.spectrum (kernels_signal_tap.cpp)
KernelFn findCurveKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;  // GU.1 (kernels_curve.cpp)
KernelFn findSamplerWarpKernel(uint32_t lanes, uint32_t specMask) noexcept;  // SM.4 (kernels_sampler_warp.cpp)
KernelFn findGranularKernel(uint32_t lanes, uint32_t specMask) noexcept;     // SM.6 (kernels_granular.cpp)
KernelFn findSvfKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;    // EQ.1 (kernels_svf.cpp)
KernelFn findNoiseStepKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;  // NZ.1 (kernels_noise_step.cpp)
KernelFn findSampleKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;  // SM.2 (kernels_sample.cpp)
// EQ.1b: an svf state's states (every lane) all ≤ eps / set to 0 (dsp::decayStateSmall / decayStateFlush). RT-safe.
bool svfStateSmall(const void* state, float eps) noexcept;
bool svfStateFlush(void* state) noexcept;  // true: a word was not 0 yet
// DY.1 (kernels_dynamics.cpp): follow, window; DY.5: truepeak.
KernelFn findDynamicsKernel(Atom a, uint32_t lanes, uint32_t specMask) noexcept;
bool followStateSmall(const void* state, float eps) noexcept;
bool followStateFlush(void* state) noexcept;
bool followStill(void* state, float eps) noexcept;  // AU.dsp (H4)
bool windowStateSmall(const void* state, float eps, bool historySmall) noexcept;
bool windowStateFlush(void* state) noexcept;
bool truePeakStateSmall(const void* state, float eps) noexcept;  // DY.5
bool truePeakStateFlush(void* state) noexcept;

}  // namespace pq::dsp::detail
