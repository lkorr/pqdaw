#pragma once

// Leaf-atom kernel calling convention (T1.6). Every native leaf atom (SPEC §7.2) is a block-wise kernel with
// ONE uniform signature, so the interpreter (T1.7) can store a flat op list {KernelFn, KernelArgs} and the JIT
// (T1b) can generate the same semantics inline.
//
// ---- Buffer layout -----------------------------------------------------------------------------------------
// A kernel is compiled for a lane count L, 1 or 8:
//   L = 1 (global / mono signal): a block buffer is `frames` contiguous floats.
//   L = 8 (one voice group, SPEC §7.4 step 5): a block buffer is `frames * 8` floats, frame-major with the
//          voices interleaved: sample s of voice v is at [s*8 + v], so one frame is exactly one __m256.
// A voice group always has 8 lanes; unused voices simply compute garbage-free silence (their inputs are 0).
//
// ---- Input rates (the "4 bytes per 4 bytes" rule, SPEC §7.1) ---------------------------------------------
// Each input is either
//   Blk (audio rate): points at a block buffer as above, or
//   Spl (splat, param rate): points at an 8-float PATTERN that is constant over the block. For L = 8 the
//        pattern holds one value per voice (per-voice params, e.g. velocity); for L = 1 all 8 floats must hold
//        the same value (the interpreter writes a param slot as 8 copies). A Spl input costs one vector load
//        per block.
// Which inputs are Spl is a bit mask (bit i = input i) chosen when the kernel is looked up (findKernel in
// atoms.h): the kernel is a template specialisation for that mask, so the per-sample loop has no rate branches.
// A param-rate computation (all inputs Spl) is the same kernel run with frames = 1 on Blk inputs.
//
// ---- Bool signals (SPEC §7.1) ------------------------------------------------------------------------------
// A bool is stored in a float slot as a lane MASK: all bits set (0xFFFFFFFF, true) or all clear (+0.0f, false),
// in both the 1-lane and 8-lane forms, so `and/or/xor/not` are plain bitwise ops and `select` is one blendv.
// Kernels only produce canonical masks; bool inputs must be canonical masks (voice.gate etc. included).
// `gate_to_real` turns a mask into 0.0f / 1.0f. (SPEC §7.1 mentions a packed one-bit-per-voice form: see
// docs/QUESTIONS.md Q5 for why the block buffers use 32-bit masks.)
//
// ---- Outputs, aliasing, state -------------------------------------------------------------------------------
// Outputs are Blk buffers of `frames * L` floats. An output may alias an input of the same op only for
// element-wise stateless atoms (KernelInfo::inPlaceOk); others need distinct buffers.
// Stateful atoms keep a POD state block (atom_state.h), 32-byte aligned, initialised off the audio thread with
// initState(); its size is stateBytes(). Tables (wavetable, shaper curve, sample data) are read-only RESOURCES
// built off-thread (tables.h) and passed through KernelArgs::resource.
//
// Every kernel is RT-safe: noexcept, no allocation, no locks, no syscalls, bounded loops (frames × outputs).
// Kernels assume FTZ/DAZ is set on the calling thread (pq::DenormalGuard) but are correct without it.

#include <bit>
#include <cstdint>

namespace pq::dsp {

inline constexpr uint32_t kGroupLanes = 8;    // voices per AVX2 voice group
inline constexpr uint32_t kPatternFloats = 8; // floats in a Spl (param-rate) input pattern

struct KernelArgs {
  const float* const* in = nullptr;  // numIn input pointers (Blk buffer or Spl pattern, see above)
  float* const* out = nullptr;       // numOut output buffers (frames * L floats each)
  void* state = nullptr;             // the atom's state block (nullptr for stateless atoms)
  const void* resource = nullptr;    // read-only table for wavetable / shaper / sample atoms
  uint32_t frames = 0;               // frames in this block (any count ≥ 0; no multiple-of-8 requirement)
  uint16_t numIn = 0;                // number of inputs (only read by variadic atoms: mux)
  uint16_t numOut = 0;               // number of outputs (only read by variadic atoms: demux)
  uint32_t splMask = 0;              // Spl bits of all inputs (only read by variadic atoms: mux); fixed-arity
                                     // kernels are template-specialised for their mask instead
};

using KernelFn = void (*)(const KernelArgs&) noexcept;

// Canonical bool masks as floats (for building inputs in tests and the interpreter).
inline float boolMask(bool b) noexcept {  // RT-safe
  return b ? std::bit_cast<float>(0xFFFFFFFFu) : 0.0f;
}
inline bool maskIsTrue(float m) noexcept {  // RT-safe
  return std::bit_cast<uint32_t>(m) != 0u;
}

}  // namespace pq::dsp
