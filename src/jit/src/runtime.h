#pragma once

// Native helpers that JIT-compiled code calls (T1b.1), beyond what it shares with the interpreter: the special ops
// (broadcast, voice reducers, sends) are the shared image host's kernels (pq/engine/image_host.h) and the Newton
// update for n > 1 is newton::linearUpdate (pq/engine/newton.h), so generated code calls the very functions the
// interpreter calls (T1.x-arch2). Everything here is RT-safe. Generated code reaches these through the image's
// pointer table (never through symbols), so a compiled module has no relocations against process code (codegen.h).
//
//   "Exact" transcendentals: the very functions the dsp kernels use, so JIT and interpreter agree bit for bit: the
//   scalar forms are the CRT calls of the kernels' 1-lane path (pq_dsp simd.h S1), the 8-wide forms MSVC's SVML calls
//   of their AVX2 path (S8). Scalar: float(float) / float(float, float). 8-wide: void(float* io) with the input(s)
//   at io[0..8) (and io[8..16)), the result written to io[0..8).

#include <cstdint>

namespace pq::jit::rt {

enum class Exact : uint8_t { Sin, Tan, Tanh, Exp, Log, Pow, CentsToHz, HzToCents, DbToGain, GainToDb, Count_ };
inline constexpr bool exactIsBinary(Exact e) noexcept {
  return e == Exact::Pow || e == Exact::CentsToHz || e == Exact::HzToCents;
}
const void* exactScalar(Exact e) noexcept;  // RT-safe lookup: float(float) or float(float, float)
const void* exactVector(Exact e) noexcept;  // RT-safe lookup: void(float* io)

}  // namespace pq::jit::rt
