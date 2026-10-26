#pragma once

// The JIT backend's ProgramImage (T1b.1): the shared image host (pq/engine/image_host.h, T1.x-arch2 — validation,
// memory, interface, params, engine sources, reducers / sends / broadcast, the stage loop and output copies, the same
// code the interpreter runs) plus the generated stage functions. process() is the host's begin() + run() with one call
// of the stage's generated function per global stage, or per occupied voice group with that group's voice-state block.
// Everything inside a stage (ops, loops, solves, special-op calls) is generated code (codegen.h).
//
// Memory (all the host's, allocated when the image is built):
//   buf    scratch buffers (Schedule::buffers; per-group copies for Buffer::perGroup), then the JIT's own scratch:
//          Newton for n > 1 unknowns (u, r0 (maxN·8 each), J (maxN²·8), M (doubles)) and the pointer arrays of wide
//          kernel calls. Written by generated code.
//   aux    param patterns, constant patterns, global engine-source patterns/ramps, zeros. Read-only for generated
//          code (the host writes params/sources before the stage calls).
//   state  the global state arena, then one voice-state block per voice group (graph::StateLayout offsets).
//   tab    the pointer table: kernel and helper function pointers, resources, special-op records.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "pq/core/result.h"
#include "pq/engine/image_host.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/program_image.h"
#include "pq/graph/schedule.h"
#include "runtime.h"

namespace pq::jit::detail {

using engine::host::isPatternOperand;  // shared with codegen
using engine::host::lanesOfOperand;

// The generated function of one stage. RT-safe (generated code: no allocation, locks, syscalls or exceptions).
// vstate1 / group1: a pair function's second group (null / 0 otherwise); lane: the scalar baseline's voice lane.
using StageFn = void (*)(float* buf, const float* aux, std::byte* gstate, std::byte* vstate, std::byte* vstate1,
                         float* const* ext, const void* const* tab, uint32_t frames, uint32_t group, uint32_t group1,
                         uint32_t lane) noexcept;

// Kernel calls with at most this many inputs and outputs keep their KernelArgs pointer arrays on the stage
// function's stack; wider ones (a mux over many inputs) use HostLayout::callOff, so no frame needs stack probes.
inline constexpr uint32_t kStackCallOperands = 64;

// What codegen addresses (offsets in floats into buf / aux): the host's layout plus the JIT's scratch and table slots.
struct HostLayout {
  uint32_t maxBlock = 0;
  uint32_t groups = 1;                       // voice groups with state
  std::vector<uint32_t> bufOff, bufSize;     // per Schedule::buffers entry (perGroup: groups copies, bufSize apart)
  uint32_t solveOff = 0;                     // Newton scratch (n > 1): u, r0 (maxN·8 each), J (maxN²·8), M (doubles)
  uint32_t maxSolveN = 0;
  uint32_t callOff = 0, callPtrs = 0;        // pointer arrays of wide kernel calls (callPtrs pointers; 0: none)
  uint32_t paramOff = 0, constOff = 0;
  uint32_t sleepOff = 0;                     // aux: the sleep segments' flag words (T1.x-silence, image_host.h)
  uint32_t meterOff = 0;                     // buf: per region (cycles, timings) as two uint64: the cost meter (T4.5)
  uint32_t meterFlagOff = 0;                 // aux: non-zero while the block is metered (T4.5)
  // T1b.x-perf (Q126): buf: one word, non-zero when this block's params equal the previous block's (written by
  // JitImage::process): the memo Control regions (codegen.h) are then skipped, their buffers still hold the values.
  uint32_t memoOff = 0;
  std::vector<uint32_t> opInfo;              // per op: pointer-table slot of its resource / info record, or kNone
  // Pairs (T1b.2, codegen.h): per stage, whether it gets a two-group function; per buffer, the float offset of the
  // second group's copy of a pairable stage's own voice value (kNone: none); per op, the second group's record slot
  // (reducers).
  std::vector<uint8_t> pairable;
  std::vector<uint32_t> altOff;
  std::vector<uint32_t> opInfoB;
};

// Atoms whose voice ops codegen generates inline wherever they are (the "simple stateful" atoms, codegen.h).
bool inlineStateful(dsp::Atom a) noexcept;

// Whether buffer b is written in stage k by a voice-lane op (not a reducer's global accumulator) or as a solve
// unknown: the stage's own voice values, which a pair function keeps per group (HostLayout::altOff).
std::vector<uint8_t> stageLocalBuffers(const graph::Schedule& s, uint32_t stage);

// Pointer table under construction: one slot per distinct pointer.
class PtrTable {
public:
  uint32_t slot(const void* p) {
    auto it = index_.find(p);
    if (it != index_.end()) return it->second;
    const auto s = static_cast<uint32_t>(ptrs_.size());
    ptrs_.push_back(p);
    index_.emplace(p, s);
    return s;
  }
  std::vector<const void*>& ptrs() noexcept { return ptrs_; }

private:
  std::vector<const void*> ptrs_;
  std::unordered_map<const void*, uint32_t> index_;
};

class JitImage final : public engine::ProgramImage {
public:
  JitImage() = default;
  ~JitImage() override;
  JitImage(const JitImage&) = delete;
  JitImage& operator=(const JitImage&) = delete;

  void process(engine::ProcessContext& ctx) noexcept override;  // RT-safe
  const graph::StateLayout& stateLayout() const noexcept override { return host_.stateLayout(); }
  const engine::ImageInterface& io() const noexcept override { return host_.io(); }
  engine::StateArena stateArena() noexcept override { return host_.stateArena(); }
  uint64_t rejectedBlocks() const noexcept override { return host_.rejectedBlocks(); }
  const engine::SleepStats* sleepStats() const noexcept override { return &host_.sleepStats(); }
  bool bindInspect(const engine::InspectBinding& b) override { return host_.bindInspect(b); }

  // ---- built by buildHost() / Compiler ----
  engine::host::ImageHost host_;
  HostLayout layout_;
  std::vector<StageFn> fns_;                 // per Schedule::stages entry (set by the Compiler)
  std::vector<StageFn> pairFns_;             // per stage: the two-group function, or null (T1b.2)
  bool scalarLanes_ = false;                 // the scalar baseline: voice stages run once per lane
  std::vector<const void*> tab_;
  // Releases the machine code (set by the Compiler; runs in the destructor, off the audio thread).
  std::shared_ptr<void> codeOwner_;
  // T1b.x-perf (Q126): the previous block's params, bitwise (NaN: absent), for the memo flag (HostLayout::memoOff);
  // the interpreter's rule (pq/engine/interpreter.h), so both skip the same blocks.
  std::vector<float> lastParams_;
  bool memoValid_ = false;
};

// Builds the shared host (validation, memory, state, params, engine sources, outputs, special-op records,
// resources) with the JIT's scratch, checks that every kernel the generated code calls exists, and fills the layout
// and pointer-table slots codegen needs. The stage functions are filled in by the Compiler. Not RT-safe.
Result<std::unique_ptr<JitImage>> buildHost(const graph::Schedule& s, uint32_t maxBlockFrames, uint32_t maxVoiceGroups,
                                            const engine::Resources* resources, bool pairs, PtrTable& tab,
                                            engine::SleepMode sleep = engine::SleepMode::On);

// After the stage functions are in (T1b.2): tells the host which voice stages have a pair function (and where the
// second group's outputs are). Not RT-safe.
void finishPairs(JitImage& img, const graph::Schedule& s);

}  // namespace pq::jit::detail
