#pragma once

// The interpreter backend (T1.7, SPEC §7.4 step 6): a graph::Schedule → a ProgramImage that runs a flat array of
// prebuilt op records {KernelFn, KernelArgs} (pq/dsp/kernel.h). Everything is resolved when the image is built
// (off the audio thread: this allocates): every operand pointer (scratch buffer, param/const pattern, state block,
// resource), every kernel specialisation (lanes × Spl mask), and one copy of the voice stages' records per voice
// group (per-group buffers and state). process() then only
//   - splats the param block (clamped to declared ranges) and the global param-rate sources into 8-float patterns,
//     and points the records that read engine data (root inputs, voice.* buffers, track/sidechain input) at this
//     block's buffers (one pointer store per use: no copies);
//   - runs each stage: Control regions once (frames = 1, 8 lanes), Block regions block-wise, Loop regions per
//     sample or per sub-block (each Blk operand re-pointed at t·lanes before the call), voice stages once per
//     active voice group; reducers (voice_sum …) accumulate over the groups' active lanes, sends add into buses;
//   - solves `solve` blocks per frame (Loop) or once per block (Control) with Newton (pq/engine/newton.h, shared
//     with the JIT): the Jacobian from forward-mode tangents carried through one body evaluation (finite differences
//     with a step relative to the unknown for bodies without tangent rules, and for a lane whose tangent Jacobian is
//     not finite), the n×n system per lane by Gaussian elimination with partial pivoting; a lane whose step is not
//     finite (singular J, NaN) keeps its value for that iteration. After the last iteration the body runs once
//     more, so values read after the block match the solution; an unknown without an initial guess starts from
//     the previous frame's solution (docs/QUESTIONS.md Q46, Q53).
// Cost: one indirect call per op per block (Block regions), per sub-block (Loop) or per Newton evaluation (solve
// bodies), never a virtual call; see bench/RESULTS.md (T1.7).

#include <cstdint>
#include <memory>
#include <span>

#include "pq/core/result.h"
#include "pq/dsp/tables.h"
#include "pq/dsp/tuning_table.h"
#include "pq/engine/program_image.h"
#include "pq/graph/schedule.h"

namespace pq::engine {

// Tables the patch's wavetable / shaper / sample / tuning_hz atoms name by index (their `table` / `sample` const).
// They must outlive the image. An index without a table reads the built-in default: a 2048-point sine wavetable, the
// identity shaper on [−1, 1], a silent one-frame sample; a tuning_hz without a table passes its fallback hz / cents
// through (T2.6).
struct Resources {
  std::span<const dsp::WavetableView* const> wavetables;
  std::span<const dsp::ShaperView* const> shapers;
  std::span<const dsp::SampleView* const> samples;
  std::span<const dsp::TuningTableView* const> tunings;  // T2.6
};

struct InterpretOptions {
  uint32_t maxBlockFrames = 1024;  // largest ProcessContext::frames (buffers hold this many frames)
  uint32_t maxVoiceGroups = 8;     // voice groups with records and state (8 → 64 voices)
  const Resources* resources = nullptr;
};

// Not RT-safe (allocates and initialises the state arena). Errors: an op without a kernel for its lanes/Spl mask, an
// unknown engine source, a malformed solve block (unknown and equation counts differ), invalid options.
Result<std::unique_ptr<ProgramImage>> buildInterpreted(const graph::Schedule& schedule,
                                                       const InterpretOptions& options = {});

}  // namespace pq::engine
