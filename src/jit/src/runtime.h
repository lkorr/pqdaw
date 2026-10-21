#pragma once

// Native helpers that JIT-compiled code calls (T1b.1). Everything here is RT-safe. The generated code reaches
// these through the image's pointer table (never through symbols), so a compiled module has no relocations
// against process code (see codegen.h).
//
//   - Special ops (broadcast, voice reducers, sends): the same semantics and calling convention as the
//     interpreter's (src/engine/src/interpreter.cpp), i.e. dsp::KernelFn with an info record in KernelArgs::state.
//     They run once per block per voice group, so a call costs nothing measurable.
//   - "Exact" transcendentals: the very functions the dsp kernels use, so JIT and interpreter agree bit for bit:
//     the scalar forms are the CRT calls of the kernels' 1-lane path (pq_dsp simd.h S1), the 8-wide forms MSVC's
//     SVML calls of their AVX2 path (S8). Scalar: float(float) / float(float, float). 8-wide: void(float* io) with
//     the input(s) at io[0..8) (and io[8..16)), the result written to io[0..8).
//   - newtonLinear: the per-lane Gaussian elimination of the interpreter's Newton step (QUESTIONS Q53), for solve
//     blocks with more than one unknown (one unknown is inlined).

#include <cstdint>

#include "pq/dsp/kernel.h"
#include "pq/engine/program_image.h"

namespace pq::jit::rt {

// Per-block state the special ops read (one per image; the image updates it before each voice group).
struct RunCtx {
  uint32_t laneMask = 0;                // active lanes of the voice group being run
  uint32_t lastLane = engine::kNoVoice; // voice_last's lane if it is in this group
  const engine::SendBus* sends = nullptr;
  uint32_t numSends = 0;
};

enum class ReduceMode : uint8_t { Sum, Last, Highest, Lowest, Loudest };

struct ReduceInfo {
  const RunCtx* run = nullptr;
  ReduceMode mode = ReduceMode::Sum;
  bool isBool = false;
  bool inPattern = false;   // input is an 8-float pattern (Spl, or a Control-region value)
  bool outPattern = false;  // output is an 8-float pattern (Control region)
};

struct SendInfo {
  const RunCtx* run = nullptr;
  uint32_t bus = 0;
  uint32_t channel = 0;
  uint8_t lanes = 1;
  bool inPattern = false;
};

void broadcastKernel(const dsp::KernelArgs& a) noexcept;  // RT-safe
void reduceKernel(const dsp::KernelArgs& a) noexcept;     // RT-safe
void sendKernel(const dsp::KernelArgs& a) noexcept;       // RT-safe

enum class Exact : uint8_t { Sin, Tan, Tanh, Exp, Log, Pow, CentsToHz, HzToCents, DbToGain, GainToDb, Count_ };
inline constexpr bool exactIsBinary(Exact e) noexcept {
  return e == Exact::Pow || e == Exact::CentsToHz || e == Exact::HzToCents;
}
const void* exactScalar(Exact e) noexcept;  // RT-safe lookup: float(float) or float(float, float)
const void* exactVector(Exact e) noexcept;  // RT-safe lookup: void(float* io)

// Newton update for n > 1 unknowns, per lane l < lanes (interpreter.cpp runSolve, verbatim arithmetic):
// solve J·Δ = r0 in double with partial pivoting and set u −= Δ when every component is finite. Layouts:
// u[j·8 + l], r0[i·8 + l], J[(i·n + j)·8 + l]; M = n·(n + 1) doubles of scratch.
void newtonLinear(float* u, const float* J, const float* r0, double* M, uint32_t n, uint32_t lanes) noexcept;  // RT-safe

}  // namespace pq::jit::rt
