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

// Adapts an element-wise Op to the Impl interface used by the tables below.
template <class Op>
struct Ew {
  static constexpr uint32_t kSpec = Op::kArity;
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    elementwise<Op, SM>(a, a.frames * S::kW);
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
// tuning_hz (kernels_tuning.cpp): one function per lane count, any Spl mask.
KernelFn findTuningKernel(uint32_t lanes) noexcept;

}  // namespace pq::dsp::detail
