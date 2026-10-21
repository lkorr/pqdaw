#pragma once

// The scheduler (T1.5, SPEC §7.4 steps 4–5): an optimised TypedPatch (optimise.h) → a flat, backend-neutral
// Schedule that the interpreter (T1.7) turns into an op list and the JIT (T1b) into LLVM IR. Not RT-safe: compiler
// thread. Nothing here runs on the audio thread; the Schedule only describes what the audio thread will run.
//
// ---- What schedule() does ------------------------------------------------------------------------------------
// 1. Dependency graph: one edge per wire, plus an implicit edge from every equation of a solve block to every one of
//    its unknowns (the solution depends on the equations). Edges into DELAYED inputs (PortSpec::delayed: z1.x,
//    delay*.x, phasor.hz, sine_hz.hz, sample_read rate) are marked.
// 2. Tarjan SCCs. A strongly connected component with more than one node (or a self-loop) is feedback. Inside it:
//    - edges into z1.x / delay*.x are CUT: the node becomes a split pair (z1:read … z1:write, delay*:read …
//      delay:write, pq/dsp/atoms.h); the read runs where the node would, the write at the end of the iteration;
//    - what is left without the solve edges must be acyclic. Otherwise the cycle has no delay: an error (text,
//      located at a node of the cycle) or, with ScheduleOptions::visualMode, a z1 is inserted on one of its wires
//      and a warning is recorded (Schedule::warnings, insertedDelays). A cycle whose only delay is a delayed input
//      without a split kernel (phasor.hz, sine_hz.hz, sample_read rate) is reported the same way (QUESTIONS
//      Q43);
//    - each solve block's BODY (the nodes on a path from its unknowns to its equations) must not hold stateful
//      fused atoms (they would advance once per Newton iteration) and must not depend on another block that depends
//      on it within the same sample.
// 3. Stages (voice vs global). Global nodes run once per block (lanes = 1); voice nodes run once per voice group
//    (lanes = 8, SPEC §7.4 step 5). Stages alternate global (even index) / voice (odd index); a node goes into the
//    earliest stage of its poly after everything it reads. A voice reducer (voice_sum …) runs inside the voice stage
//    of its input, accumulating over the groups, and its global result is available from the next stage on. A
//    global audio-rate value read by voice ops is broadcast once per block into an 8-lane buffer at the end of the
//    preceding global stage. A feedback loop may not cross the voice/global boundary (error).
// 4. Regions inside a stage, in this order:
//      Control  — every param/const-rate op of the stage, once per block (frames = 1, lanes = 8: inputs and outputs
//                 are 8-float patterns, kernel.h), then
//      Block    — acyclic audio ops, block-wise, and
//      Loop     — feedback SCCs and audio-rate solve blocks: for (t = 0; t < frames; t += subBlock) run the region's
//                 ops on frames t … t + subBlock − 1. subBlock = 1 (per sample) unless every cut in the loop is a
//                 delay line whose delay has a constant lower bound D, in which case subBlock = the smallest whole
//                 number of samples that every tap is old (linear ⌊D⌋, cubic ⌊D⌋ − 1, allpass ⌊D − 0.5⌋): a 250 ms
//                 echo then runs in sub-blocks of 12000 frames, i.e. block-wise. Lower bounds are evaluated in
//                 float32 with the runtime's operations (a double bound can round above the runtime value and
//                 make the tap read an unwritten frame). Lower bounds come from literals,
//                 `sr`, products/sums of non-negative bounds and, with ScheduleOptions::trustParamRanges, the
//                 declared range minimum of a param (the engine must clamp params to their ranges).
//    Within a stage and class, ready SCCs are taken depth-first (most recently readied first), so a value is read
//    soon after it is written and few buffers are live; consecutive loops with the same subBlock share one region.
//    Solve blocks of param/const rate are solved once per block inside the Control region.
// 5. Buffers. Every op output is a virtual value; values get physical buffers by interval reuse (linear scan in op
//    order, LIFO free lists per class: 8-float pattern, block × 1 lane, block × 8 lanes). Element-wise ops
//    (dsp::KernelInfo::inPlaceOk) write over an input whose last read is by that op. A read that happens AFTER an
//    op rather than by it keeps the value alive past that op (it is never taken in place there): the solver's reads
//    of every equation lhs/rhs and every unknown after each body evaluation, the engine's read of a voice output or
//    export at the end of its stage (per group), a reducer accumulator over its whole stage, and a value read inside
//    a loop (Loop region, solve body, or the per-group loop of a voice stage) but defined before it, which lives to
//    after the loop's last op (the next iteration reads it again). A voice value read in a later voice stage gets a
//    per-group buffer (Buffer::perGroup). tests/graph/sched_replay.h replays a schedule to prove no buffer is
//    overwritten while live.
// 6. State layout (StateLayout): one entry per (state key, channel) of every stateful kernel node (and per
//    unknown without an initial guess: its previous solution). Keys are TypedPatch::stateKey (call-path hashes, stable
//    across recompiles), tags are (kernel, lanes, size-affecting init: the delay ring's frames). Given the previous
//    layout (ScheduleOptions::previous), every entry whose (key, channel, arena, tag, size) is unchanged keeps its
//    offset; new entries fill holes first-fit, then append. So T1.9's state transfer is a copy of identical ranges
//    (coalesced into a few memcpys), and an unchanged node's state never moves. When the holes make an arena more
//    than twice its live size (+ 4 KiB), it is laid out afresh (StateLayout::compacted; transfer by key still works).
//
// ---- How a backend runs a Schedule (T1.7 interpreter, T1b JIT) -------------------------------------------------
//   for each stage: if global, run its regions once; if voice, for each active voice group g run its regions with
//   the group's voice state (voice arena base + g · voiceStride), externals and per-group buffers. Before the group
//   loop, every Reduce op's output is initialised (sum: 0, highest: −inf …); each group's run combines into it.
//   Region: Control → each op with frames = 1, lanes = 8; Block → each op with the block's frames; Loop → slices of
//   subBlock frames (Blk operands offset by t · lanes floats, Spl operands not offset); split writes come last in a
//   loop's op list. A SolveBlock inside a region: per frame (Loop) or once (Control) run Newton `iterations` times
//   on its unknowns: write the guess into each unknown's value, run the body ops, residual_i = lhs_i − rhs_i, solve
//   J·Δ = residual (J by finite differences or forward AD: a backend choice) and update; then run the body once more
//   with the final values (body values read after the solve must match the solution) and, for unknowns without an
//   initial guess, store the solution in their state (the next frame's guess).
//   Operands: Buffer (a buffer of Schedule::buffers; patterns are 8 floats), Param (the param block slot, read as an
//   8-float pattern: 8 copies, bools as masks), Const (constants[i], as an 8-float pattern), External (engine-provided
//   data: root inputs, voice.*, transport.*, track/sidechain inputs, clip players; block buffers for audio rate, 8-float
//   patterns for param rate, per group for voice externals). An op input is Spl (bit set in Op::splMask) when its
//   operand is a pattern; kernel inputs that cannot be Spl get a Fill op (the dsp `const` kernel) in front of them.
//   Outputs/exports: global ones are read after the last stage; voice ones at the end of OutputRef::stage, per group.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/dsp/atoms.h"
#include "pq/graph/infer.h"

namespace pq::graph {

struct Operand {
  enum class Kind : uint8_t { None, Buffer, Param, Const, External };
  Kind kind = Kind::None;
  uint32_t index = kNone;  // into Schedule::buffers / params / constants / externals
  friend bool operator==(const Operand&, const Operand&) = default;
};

enum class OpKind : uint8_t {
  Kernel,     // dsp kernel `kernel` (incl. split z1/delay reads/writes and the `const` block fill)
  Broadcast,  // in[0]: a 1-lane block → out[0]: the same value in all 8 lanes of a voice-group block (once per block)
  Reduce,     // voice reducer `atom` (voice_sum …): combines in[0] (voice) of every group into out[0] (global)
  Send,       // engine sink `atom` (send): in[0] → bus `resource`
};

struct Op {
  OpKind kind = OpKind::Kernel;
  uint8_t lanes = 1;                    // 1 (global) or 8 (voice group, and every Control-region op)
  dsp::Atom kernel = dsp::Atom::Count_; // Kernel only
  AtomId atom = 0xFFFF;                 // registry atom (Reduce/Send, and Kernel ops of atom nodes); 0xFFFF: none
  uint16_t numIn = 0, numOut = 0;       // operands: operands[firstOperand …] = inputs then outputs
  uint16_t channel = 0;                 // channel of the node this op computes
  uint32_t splMask = 0;                 // bit i: input i is an 8-float pattern (Spl); findKernel's mask
  uint32_t firstOperand = 0;
  uint32_t state = kNone;               // index into StateLayout::entries (shared by a split read/write pair)
  uint32_t resource = kNone;            // Kernel: resource index (dsp::kernelInfo(kernel).resource says which
                                        // table kind); Send: bus index
  NodeIndex node = kNone;               // the node it computes (in Schedule::patch(): see `rewritten`)
  Kind valueKind = Kind::Real;          // Reduce: kind of the reduced value (bool reducers combine lane masks:
                                        // sum/highest/loudest = any, lowest = all)
};

enum class RegionKind : uint8_t { Control, Block, Loop };

struct Region {
  RegionKind kind = RegionKind::Block;
  Poly poly = Poly::Global;
  uint32_t stage = 0;
  uint32_t firstOp = 0, numOps = 0;
  uint32_t subBlock = 0;                 // Loop: frames per iteration (≥ 1); 0 for Control/Block
  uint32_t firstSolve = 0, numSolves = 0; // into Schedule::solves
};

struct Stage {
  Poly poly = Poly::Global;               // even stage index: Global; odd: Voice
  uint32_t index = 0;                     // stage number (stages without ops are omitted from Schedule::stages)
  uint32_t firstRegion = 0, numRegions = 0;
  uint32_t firstOp = 0, numOps = 0;
};

// One `solve(iterations: n) { … }` block (or one channel set of it), solved inside `region`.
struct SolveBlock {
  SolveIndex solve = kNone;
  uint32_t iterations = 1;
  uint32_t region = 0;
  uint32_t firstOp = 0, numOps = 0;        // the body: evaluated every Newton iteration
  uint32_t firstUnknown = 0, numUnknowns = 0;
  uint32_t firstEquation = 0, numEquations = 0;
};

struct SolveUnknown {
  NodeIndex node = kNone;
  uint16_t channel = 0;
  Operand value;   // a Buffer: the solver writes the guess here (frame t) before each body evaluation
  Operand init;    // initial guess, or None: the previous frame's solution (kept in `state`)
  uint32_t state = kNone;
};

struct SolveEquation {
  NodeIndex node = kNone;
  uint16_t channel = 0;
  Operand lhs, rhs;  // residual = lhs − rhs
};

struct Buffer {
  enum class Class : uint8_t { Pattern, Block };
  Class cls = Class::Block;
  uint8_t lanes = 1;      // Block: 1 or 8 (frames · lanes floats); Pattern: always 8 floats
  bool perGroup = false;  // a voice value read in a later voice stage: one copy per voice group
};

struct External {
  enum class Source : uint8_t { RootInput, Engine };
  Source source = Source::RootInput;
  uint32_t index = 0;     // RootInput: ordinal of the root `in` (node order, as InferOptions::inputs); Engine: 0
  AtomId atom = 0xFFFF;   // Engine: voice.*, transport.*, track.in, sidechain.in, clipPlayer
  uint16_t channel = 0;
  Rate rate = Rate::Audio;  // Audio: a block buffer; Param/Const: an 8-float pattern
  Poly poly = Poly::Global; // Voice: per voice group (8 lanes)
  NodeIndex node = kNone;
};

struct ConstValue {
  float value = 0.0f;  // bools: the lane mask (dsp::boolMask)
  Kind kind = Kind::Real;
};

struct ParamSlot {
  NodeIndex node = kNone;
  uint64_t key = 0;          // TypedPatch::stateKey of the param node (stable across recompiles)
  Kind kind = Kind::Real;    // bool params are written as masks
  float defaultValue = 0.0f; // normalised (Constant::value); bools 0/1
  bool hasRange = false;
  float rangeMin = 0.0f, rangeMax = 0.0f;
};

struct OutputRef {
  NodeIndex node = kNone;    // the Output or Export node
  bool isExport = false;
  uint16_t channel = 0;
  Operand value;
  Poly poly = Poly::Global;
  uint32_t stage = 0;        // Voice: the engine reads it at the end of this stage, per group
};

// The hot-swap type tag of a state block (SPEC §7.5): state is transferred only between equal tags.
struct StateTag {
  static constexpr uint16_t kSolveGuess = 0xFFFE;  // `kernel` of an unknown's previous-solution block
  uint16_t kernel = 0;    // dsp::Atom of the fused kernel (split pairs use the fused atom's tag), or kSolveGuess
  uint8_t lanes = 1;
  uint32_t extra = 0;     // delay lines: StateInit::maxDelayFrames (the ring size); 0 otherwise
  friend bool operator==(const StateTag&, const StateTag&) = default;
};

struct StateEntry {
  uint64_t key = 0;        // TypedPatch::stateKey of the node
  uint16_t channel = 0;
  Poly arena = Poly::Global;  // Global: the program arena; Voice: each voice group's block
  StateTag tag;
  uint32_t offset = 0;     // bytes, a multiple of dsp::kStateAlign
  uint32_t size = 0;       // bytes, a multiple of dsp::kStateAlign
  dsp::StateInit init;     // what dsp::initState needs (seed already mixed with key and channel); zeros for a guess
  NodeIndex node = kNone;
};

struct StateLayout {
  std::vector<StateEntry> entries;  // in op order (the order the program touches them)
  uint32_t globalBytes = 0;         // arena size (multiple of 64)
  uint32_t voiceBytes = 0;          // per voice group (multiple of 64)
  bool compacted = false;           // laid out afresh although a previous layout was given (fragmentation)
  // The entry with this key/channel/arena, or nullptr. O(n) (tests, tools); T1.9 builds its own index.
  const StateEntry* find(uint64_t key, uint16_t channel, Poly arena) const noexcept;
};

// A z1 inserted by visualMode into a delay-free cycle.
struct InsertedDelay {
  NodeIndex z1 = kNone;        // the new node (in *Schedule::rewritten)
  NodeIndex consumer = kNone;  // whose input it now feeds
  uint16_t input = 0;
};

struct Schedule {
  std::vector<Stage> stages;
  std::vector<Region> regions;
  std::vector<Op> ops;
  std::vector<Operand> operands;
  std::vector<SolveBlock> solves;
  std::vector<SolveUnknown> unknowns;
  std::vector<SolveEquation> equations;
  std::vector<Buffer> buffers;
  std::vector<External> externals;
  std::vector<ConstValue> constants;
  std::vector<ParamSlot> params;
  std::vector<OutputRef> outputs;
  StateLayout state;
  double sampleRate = 0.0;
  std::vector<Diagnostic> warnings;          // visualMode: one per inserted z1
  std::vector<InsertedDelay> insertedDelays;
  // Set when visualMode inserted z1s: the patch every NodeIndex above refers to (the input patch plus the z1s,
  // whose indices follow the input's). Null otherwise: NodeIndex values refer to the input patch.
  std::shared_ptr<const TypedPatch> rewritten;

  std::span<const Operand> inputs(const Op& op) const noexcept { return {operands.data() + op.firstOperand, op.numIn}; }
  std::span<const Operand> outputsOf(const Op& op) const noexcept {
    return {operands.data() + op.firstOperand + op.numIn, op.numOut};
  }
  uint32_t blockBuffers() const noexcept;    // buffers of class Block (the scratch-buffer count)
  uint32_t patternBuffers() const noexcept;
};

struct ScheduleOptions {
  double sampleRate = 48000.0;          // > 0: sizes delay rings, values `sr`, converts delay `max`
  bool visualMode = false;              // delay-free cycle: insert z1 + warning instead of an error
  bool trustParamRanges = true;         // param range minimums bound delay times (loop sub-blocks)
  const StateLayout* previous = nullptr;  // keep the offsets of unchanged state (hot swap, T1.9)
  // false: every value gets its own buffer and nothing runs in place (buffer index = value identity). Everything
  // else is identical to the shared schedule, op for op. For checkers (tests/graph/sched_replay.h) and debugging.
  bool shareBuffers = true;
  SourceText source{};                  // optional: errors as file:line:col (root-patch node spans)
};

struct ScheduleResult {
  Schedule schedule;                    // complete only when diagnostics is empty
  std::vector<Diagnostic> diagnostics;  // errors
  bool ok() const noexcept { return diagnostics.empty(); }
};

// Errors: delay-free cycles (text mode), feedback whose only delay has no split kernel (text mode), feedback across
// the voice/global boundary, solve blocks that are malformed for scheduling (an unknown no equation reads, an
// equation that reads no unknown, a stateful atom in a body, an initial guess that depends on its own block, blocks
// that depend on each other), non-constant state-init inputs (delay `max`, z1 `init`, noise `seed`, tables), event
// rate, and a TypedPatch whose durations are still in seconds (run optimise() first).
ScheduleResult scheduleDetailed(const TypedPatch& patch, const ScheduleOptions& options = {});
// The schedule, or an Error holding every diagnostic (formatted with the source line when options.source is set).
Result<Schedule> schedule(const TypedPatch& patch, const ScheduleOptions& options = {});

// Text form of a schedule for `pqc dump-schedule` (T1.10) and tests: stages, regions, ops with operands, solve
// blocks, buffers, externals, params, outputs, state layout. `patch` is the TypedPatch passed to schedule() (the
// dump uses Schedule::rewritten instead when it is set). State keys are printed only with `withKeys` (they are
// hashes, so expected dumps in tests leave them out).
std::string dump(const Schedule& s, const TypedPatch& patch, bool withKeys = false);

}  // namespace pq::graph
