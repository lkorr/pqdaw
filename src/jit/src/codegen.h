#pragma once

// Schedule → LLVM IR (T1b.1). One function per stage:
//
//   void @<prefix>_stage<k>[_pair](ptr noalias buf, ptr noalias readonly aux, ptr noalias gstate, ptr noalias vstate,
//                                  ptr noalias vstate1, ptr noalias readonly ext, ptr noalias readonly tab,
//                                  i32 frames, i32 group, i32 group1, i32 lane)
//
// called by JitImage::process once per block (global stage) or once per voice group (voice stage). The function
// does everything the interpreter's records do for that stage, with every op either GENERATED INLINE or CALLED.
//
// Voice stages (T1b.2): a voice value is one vector across voice lanes, <8 x float> for the 8 voices of a group.
// A `_pair` function runs TWO groups (group, group1; their state blocks vstate, vstate1) as <16 x float>: on one
// 512-bit register per value where the host has AVX-512 (GenOptions::pairsZmm), else on two 256-bit halves. Lanes
// 0–7 are the first group, 8–15 the second; a per-group buffer is read and written per half (two 256-bit memory
// accesses per value), a value of this stage that is not per-group lives in its buffer for the first group and in
// a second copy (HostLayout::altOff) for the second, reducers call the host's kernel once per half with the
// second group's record (HostLayout::opInfoB), and every kernel call is made once per half. Every lane computes
// exactly the operations of the 8-lane code (exact transcendentals: one 8-wide SVML call per half), so pairs are
// bit-identical to groups. The scalar baseline (GenOptions::scalarLanes) runs one lane: scalar code at lane `lane`
// of every 8-lane buffer, pattern and state array.
//   Regions of a voice stage whose ops are all inline and that run frame by frame (Block regions of inline ops,
// including the simple stateful atoms since T1b.2, and per-sample Loop regions) are FUSED into one loop over
// frames ("chains"): a per-sample feedback loop is latency-bound, and the oscillators feeding it fill its idle
// issue slots; values cross the regions in registers.
//
//   Block regions and sub-blocked Loop regions ("slices" of frames):
//     - runs of consecutive stateless element-wise ops (math, logic, mux/demux, select, sine, fast_ functions,
//       exact transcendentals) are FUSED into one loop over the slice: for 1-lane (global) ops 8 frames per
//       iteration (<8 x float>, vectorised over time) plus a scalar tail loop, for 8-lane (voice) ops one frame
//       (one <8 x float>, the 8 voices) per iteration. Values flow in registers between the ops of a run; a buffer
//       is stored only when something outside the run reads it (store elimination below).
//     - stateful atoms and table atoms are one direct call of the dsp kernel for the whole slice (those kernels
//       are already vectorised per block; the call costs ~nothing per sample), except the simple stateful atoms
//       of VOICE runs (T1b.2): one frame of voices per chunk, so they are inline like in a per-sample loop, their
//       state carried in registers through the run's loop.
//   Control regions and per-sample Loop regions ("per frame"):
//     - every op is generated inline for one frame (scalar for 1-lane ops, <8 x float> for 8-lane and Control
//       ops), including the simple stateful atoms (z1 and its split read/write, rise/fall/change, sr/d/t
//       flip-flops, schmitt, samplehold, latch, counter, clockdiv, pulse, timer, phasor, sine_hz), whose state is
//       loaded and stored per frame so LLVM can keep it in registers across the loop.
//     - the rest (delay lines, wavetable, noise, shaper, sample_*) is a call of the dsp kernel for one frame.
//     - solve blocks: Newton exactly as the interpreter (pq/engine/newton.h, QUESTIONS Q53), the body generated
//       inline with its forward-mode tangents (one evaluation per iteration: newton::tangentOp instantiated on IR);
//       finite differences (1 + 2n evaluations) for bodies without tangent rules and, in a cold branch, for lanes
//       whose tangent Jacobian is not finite; the update for n = 1 inline, for n > 1 a call of newton::linearUpdate
//       (the interpreter's own).
//   Special ops (broadcast, voice reducers, sends): a call of the shared image host's kernels (pq/engine/image_host.h).
//
// Exact transcendentals call the same CRT (scalar) / SVML (8-wide) functions as the kernels; fast_ functions are
// generated from pq/core/fast_math.h's own templates (ir_ops.h). So JIT and interpreter agree bit for bit unless
// fast-math contraction (fmul + fadd → fma) is opted into with CompileOptions::contract (off by default).
//
// Store elimination: an op output inside a fused run / per-frame region is stored to its buffer only when a
// later op in another run (or a call, a special op, a solve, an output copy) reads that buffer before it is
// written again.
//
// No relocations against process code: functions, resources and special-op records are reached through `tab`
// (PtrTable slots), buffers/params/state through the base pointers. Not RT code: runs on the compiler thread.

#include <llvm/IR/Module.h>

#include <cstdint>
#include <string>
#include <vector>

#include "jit_image.h"
#include "pq/core/result.h"
#include "pq/graph/schedule.h"

namespace pq::jit::detail {

struct GenOptions {
  bool contract = false;      // fast-math `contract` + `afn` on generated arithmetic (never nnan/ninf/nsz/reassoc)
  std::string cpu;            // "target-cpu" function attribute
  std::string features;       // "target-features" function attribute
  std::string prefix;         // function name prefix (unique per image)
  // Voice stages (T1b.2). scalarLanes: the benchmark baseline, voice stages run one voice lane per call with scalar
  // code (an error when a voice stage needs a kernel or special-op call). pairs: also generate a two-group function
  // (<16 x float>) for every voice stage that HostLayout::pairable allows; pairsZmm: on 512-bit registers
  // (prefer-vector-width 512, AVX-512 hosts only), else on two 256-bit halves.
  bool scalarLanes = false;
  bool pairs = false;
  bool pairsZmm = false;
};

struct GenResult {
  std::vector<std::string> stageFunctions;  // per Schedule::stages entry
  std::vector<std::string> pairFunctions;   // per Schedule::stages entry: the two-group function, or empty
  uint32_t inlinedOps = 0;   // ops generated inline
  uint32_t calledOps = 0;    // ops that call a dsp kernel or runtime helper
};

Result<GenResult> generate(const graph::Schedule& s, const HostLayout& layout, PtrTable& tab, llvm::Module& m,
                           const GenOptions& options);

}  // namespace pq::jit::detail
