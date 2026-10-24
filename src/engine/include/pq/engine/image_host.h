#pragma once

// Engine-internal, for ProgramImage backends only (T1.x-arch2, M1 review M15): everything an image does AROUND its op
// code, implemented once and used by both backends — the interpreter (src/engine/src/interpreter.cpp) and the JIT's
// host (src/jit/src/jit_image.cpp). Before this file each backend had its own copy, kept equal by bit-identity tests.
//
//   validation   the schedule checks every backend relies on (operand indices, Spl masks vs operands, lane counts,
//                arities, state entries, solve blocks, outputs). Kernel availability is the backend's (each looks
//                kernels up for its own lane choice).
//   memory       one layout: scratch buffers (Schedule::buffers; per-group copies for Buffer::perGroup) + backend
//                scratch after them (BuildOptions::extraBufFloats), the aux block (param patterns, constant
//                patterns, global engine-source patterns / transport ramps, zeros), the state arena (global, then one
//                block per voice group) initialised from StateInit (noise seeds mixed with the group index).
//   interface    ImageInterface (slots, params, geometry), StateLayout, StateArena, rejected blocks.
//   engine data  per block: params clamped to their declared ranges (Q44), NaN → default, bools → masks, splatted into
//                8-float patterns; engine sources (root inputs, voice.*, transport.*, track.in, sidechain.in) resolved
//                into one pointer table [ext · maxVoiceGroups + group] (no copies; param-rate sources splatted).
//   special ops  broadcast, voice reducers and sends as dsp::KernelFn with their info record in KernelArgs::state
//                (one record per schedule op, at a fixed address); RunCtx says which voice group is running.
//   resources    the Resources tables by index, or the built-in defaults (sine wavetable, identity shaper, a silent
//                sample; tuning_hz has none: its kernel passes the fallback through).
//   stage loop   run(): global stages once, voice stages once per occupied voice group (T1.8) with the group's lane
//                mask / gain / voice_last lane in RunCtx, reducer accumulators initialised before the group loop (and
//                zeroed when no group ran), voice outputs copied after each group, global outputs after the last stage.
// A backend supplies only how one stage runs: run(ctx, global(k), voice(k, group)) calls its two callables (inlined:
// templates, no std::function, no virtual call).
//
// Threads: build() and the build-time accessors are not RT-safe (they allocate). begin(), run() and the special-op
// kernels are RT-safe. The object must not move after build() (the special-op records point at its RunCtx): backends
// hold it as a member of a heap-allocated image.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/result.h"
#include "pq/dsp/kernel.h"
#include "pq/dsp/tables.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/program_image.h"
#include "pq/graph/schedule.h"

namespace pq::engine::host {

// ---- Special ops ------------------------------------------------------------------------------------------------

// Per-block state the special ops read (one per image; updated before each voice group).
struct RunCtx {
  uint32_t laneMask = 0;           // active lanes of the voice group being run (0 in global stages)
  uint32_t lastLane = kNoVoice;    // voice_last's lane if it is in this group
  const float* gain = nullptr;     // the group's per-sample lane gain (VoiceGroupIO::gain), or null
  const SendBus* sends = nullptr;
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

// The special ops as kernels (same calling convention as dsp kernels; info record in KernelArgs::state).
void broadcastKernel(const dsp::KernelArgs& a) noexcept;  // RT-safe: 1-lane block → 8-lane block
void reduceKernel(const dsp::KernelArgs& a) noexcept;     // RT-safe: one voice group into the global accumulator
void sendKernel(const dsp::KernelArgs& a) noexcept;       // RT-safe: adds into a send bus channel

// ---- Operands ---------------------------------------------------------------------------------------------------
bool isPatternOperand(const graph::Schedule& s, const graph::Operand& o) noexcept;  // an 8-float pattern
uint32_t lanesOfOperand(const graph::Schedule& s, const graph::Operand& o) noexcept;  // floats per frame of a block operand
bool validOperand(const graph::Schedule& s, const graph::Operand& o) noexcept;

// The table a wavetable / shaper / sample / tuning_hz op reads: Resources by index, else the built-in default (none
// for tuning). Not RT-safe (the defaults are built on first use).
const void* resourceFor(const Resources* resources, dsp::Resource kind, uint32_t index);

// ---- The host ---------------------------------------------------------------------------------------------------

struct BuildOptions {
  const char* who = "image";        // error prefix ("interpreter", "jit")
  ImageBackend backend = ImageBackend::Unknown;
  uint32_t maxBlockFrames = 1024;   // [1, 2^20]
  uint32_t maxVoiceGroups = 8;      // [1, 1024]
  const Resources* resources = nullptr;
  uint32_t extraBufFloats = 0;      // backend scratch in the buffer block, after the schedule's buffers (64-byte aligned)
};

// Where everything lives: floats into buf / aux, bytes into the state block.
struct Layout {
  uint32_t maxBlock = 0;
  uint32_t groups = 1;
  std::vector<uint32_t> bufOff, bufSize;  // per Schedule::buffers entry (perGroup: groups copies, bufSize apart)
  uint32_t extraOff = 0;                  // BuildOptions::extraBufFloats
  uint32_t bufFloats = 0;
  uint32_t paramOff = 0, constOff = 0, zeroOff = 0, auxFloats = 0;
  std::vector<uint32_t> extOwnOff;        // per external: its own pattern / ramp in aux (global ones)
  uint32_t globalStateBytes = 0, voiceStride = 0;
  uint32_t maxSolveN = 0;                 // unknowns of the largest solve block
};

class ImageHost {
public:
  ImageHost() = default;
  ImageHost(const ImageHost&) = delete;
  ImageHost& operator=(const ImageHost&) = delete;

  // Not RT-safe. Validates the schedule, lays out and initialises memory, builds the interface, the engine-source
  // table, the special-op records and the per-stage reducer / output lists. Errors: ErrorCode::InvalidArgument for
  // bad options, ErrorCode::Unsupported for a malformed schedule, both "<who>: …".
  Status build(const graph::Schedule& s, const BuildOptions& options);
  Error error(const std::string& message) const;  // Unsupported, "<who>: message"

  // ---- build-time accessors (not RT code) ----
  const Layout& layout() const noexcept { return L_; }
  float* buf() noexcept { return bufMem_.data(); }
  float* aux() noexcept { return auxMem_.data(); }
  float* zeros() const noexcept { return zeros_; }
  std::byte* globalState() noexcept { return reinterpret_cast<std::byte*>(stateMem_.data()); }
  std::byte* voiceState(uint32_t group) noexcept {
    return globalState() + L_.globalStateBytes + static_cast<size_t>(group) * L_.voiceStride;
  }
  std::byte* statePtr(const graph::StateEntry& e, uint32_t group) noexcept {
    return e.arena == graph::Poly::Voice ? voiceState(group) + e.offset : globalState() + e.offset;
  }
  // A buffer / param / const operand's pointer for voice group `group` (externals: zeros(); they are resolved per
  // block through extPtr()).
  float* resolve(const graph::Operand& o, uint32_t group) noexcept;
  const void* resource(dsp::Resource kind, uint32_t index) const { return resourceFor(resources_, kind, index); }
  const ReduceInfo* reduceInfo(uint32_t op) const noexcept;  // null unless op is a Reduce
  const SendInfo* sendInfo(uint32_t op) const noexcept;      // null unless op is a Send

  // ---- the ProgramImage accessors ----
  const ImageInterface& io() const noexcept { return io_; }
  const graph::StateLayout& stateLayout() const noexcept { return stateLayout_; }
  StateArena stateArena() noexcept {
    StateArena a;
    a.global = globalState();
    a.voice = globalState() + L_.globalStateBytes;
    a.voiceStride = L_.voiceStride;
    a.voiceGroups = io_.maxVoiceGroups;
    return a;
  }
  uint64_t rejectedBlocks() const noexcept { return rejected_; }

  // ---- per block (RT-safe) ----
  // Starts a block: false for frames == 0 (nothing to do) or frames > maxBlockFrames (every output zeroed, counted
  // in rejectedBlocks()). Otherwise splats the params and resolves the engine sources (extPtr / extTable).
  bool begin(const ProcessContext& ctx) noexcept;  // RT-safe
  // Voice groups of this block's voice table that the image has records for (after begin()).
  uint32_t groups() const noexcept { return groups_; }
  float* extPtr(uint32_t ext, uint32_t group) const noexcept {  // RT-safe
    return extPtr_[static_cast<size_t>(ext) * L_.groups + group];
  }
  float* const* extTable() const noexcept { return extPtr_.data(); }  // RT-safe: [ext · maxVoiceGroups + group]
  // Runs every stage (after a successful begin()): global(k) for global stage k, voice(k, g) for voice stage k and
  // each occupied group g; reducers, RunCtx and output copies around them. RT-safe when the callables are.
  template <class GlobalStage, class VoiceStage>
  void run(const ProcessContext& ctx, GlobalStage&& global, VoiceStage&& voice) noexcept;

private:
  enum class Src : uint8_t {
    RootGlobal, RootVoice, VoiceSig, TransportBpm, TransportBeat, TransportBarPhase, TransportPlaying, TrackIn,
    Sidechain, Silence
  };
  struct ExtRun {
    Src src = Src::Silence;
    uint32_t index = 0;    // slot / voice signal / channel
    bool pattern = false;  // param/const rate: an 8-float pattern
    bool voice = false;
    float* own = nullptr;  // global: own pattern (8 floats) or frames buffer for transport ramps
  };
  struct ParamRun {
    float* pattern = nullptr;
    float def = 0.0f, lo = 0.0f, hi = 0.0f;
    bool hasRange = false;
    bool isBool = false;
  };
  struct OutCopy {
    uint32_t slot = 0;
    float* src = nullptr;      // resolved (null: an external, read through extPtr)
    uint32_t ext = graph::kNone;
    uint32_t srcLanes = 1;     // 1 or 8 for a block; 0 for a pattern
    uint32_t group = 0;
  };
  struct ReduceInit {
    float* out = nullptr;
    bool pattern = false;
    float value = 0.0f;
  };
  struct StageRun {
    bool voice = false;
    uint32_t firstReduce = 0, numReduce = 0;
    uint32_t firstOut = 0, numOut = 0;  // voice: group g's output copies at firstOut + g · numOut
  };

  bool fail(std::string msg);
  bool validate(const graph::Schedule& s);
  bool layoutMemory(const graph::Schedule& s, uint32_t extraBufFloats);
  bool externals(const graph::Schedule& s);
  void specialOps(const graph::Schedule& s);
  bool stages(const graph::Schedule& s);
  bool outCopy(const graph::Schedule& s, const graph::OutputRef& o, uint32_t slot, uint32_t group);

  float* resolveGlobal(ExtRun& e, const ProcessContext& ctx, uint32_t frames) noexcept;
  void copyOut(const OutCopy& c, float* dst, uint32_t frames, bool voice) noexcept;
  void zeroOutputs(const ProcessContext& ctx) noexcept;
  void initReducers(const StageRun& st, uint32_t frames, bool zero) noexcept;

  std::string who_ = "image";
  Error err_;
  Layout L_;
  const Resources* resources_ = nullptr;
  graph::StateLayout stateLayout_;
  ImageInterface io_;
  AlignedBuffer<float> bufMem_, auxMem_, stateMem_;
  float* zeros_ = nullptr;  // maxBlock · 8 zeros (absent inputs)
  std::vector<ParamRun> params_;
  std::vector<ExtRun> exts_;
  std::vector<float*> extPtr_;
  std::vector<ReduceInfo> reduceInfos_;  // fixed addresses (reserved once): records point at them
  std::vector<SendInfo> sendInfos_;
  std::vector<uint32_t> opInfo_;         // per op: index into reduceInfos_ / sendInfos_, or kNone
  std::vector<graph::OpKind> opKind_;
  std::vector<uint8_t> bufPerGroup_;     // per buffer: Buffer::perGroup
  std::vector<ReduceInit> reduceInits_;
  std::vector<OutCopy> outs_;            // voice stage copies (via StageRun), then the global ones
  uint32_t firstGlobalOut_ = 0;
  std::vector<StageRun> stages_;
  RunCtx run_;
  uint32_t groups_ = 0;
  const VoiceTable* voices_ = nullptr;
  uint64_t rejected_ = 0;
};

template <class GlobalStage, class VoiceStage>
void ImageHost::run(const ProcessContext& ctx, GlobalStage&& global, VoiceStage&& voice) noexcept {  // RT-safe
  const uint32_t frames = ctx.frames;
  const VoiceTable* vt = voices_;
  for (uint32_t k = 0; k < stages_.size(); ++k) {
    const StageRun& st = stages_[k];
    if (!st.voice) {
      run_.laneMask = 0;
      run_.gain = nullptr;
      run_.lastLane = kNoVoice;
      global(k);
      continue;
    }
    initReducers(st, frames, false);
    bool any = false;
    for (uint32_t gi = 0; gi < groups_; ++gi) {
      const VoiceGroupIO& gr = vt->groups[gi];
      if ((gr.laneMask & 0xFFu) == 0) continue;  // no voice in this group: not run (T1.8; its state holds)
      run_.laneMask = gr.laneMask & 0xFFu;
      run_.gain = gr.gain;
      run_.lastLane = vt->lastGroup == gi && vt->lastLane < 8 && ((gr.laneMask >> vt->lastLane) & 1u) ? vt->lastLane : kNoVoice;
      any = true;
      voice(k, gi);
      const uint32_t ob = st.firstOut + gi * st.numOut;
      for (uint32_t c = ob; c < ob + st.numOut; ++c) {
        const OutCopy& oc = outs_[c];
        if (gr.out != nullptr && oc.slot < gr.numOut && gr.out[oc.slot] != nullptr) copyOut(oc, gr.out[oc.slot], frames, true);
      }
    }
    if (!any) initReducers(st, frames, true);
  }
  run_.laneMask = 0;
  run_.gain = nullptr;
  for (uint32_t c = firstGlobalOut_; c < outs_.size(); ++c) {
    const OutCopy& oc = outs_[c];
    if (ctx.out != nullptr && oc.slot < ctx.numOut && ctx.out[oc.slot] != nullptr) copyOut(oc, ctx.out[oc.slot], frames, false);
  }
}

}  // namespace pq::engine::host
