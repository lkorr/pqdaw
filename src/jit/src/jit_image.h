#pragma once

// The JIT backend's ProgramImage (T1b.1): the host half of a compiled program. It owns the memory the generated
// code works on and does the per-block bookkeeping that is not worth generating code for — the same work the
// interpreter does around its records (src/engine/src/interpreter.cpp):
//   - params clamped to their declared ranges (Q44), NaN → default, splatted into 8-float patterns;
//   - engine data (root inputs, voice.*, transport, track/sidechain input) resolved into a pointer table that the
//     generated code reads once per stage call (no copies);
//   - per stage: one call of the stage's generated function (global), or one call per active voice group with that
//     group's voice-state block and lane mask (voice), reducer accumulators initialised before the group loop,
//     voice outputs copied at the end of their stage, global outputs after the last stage.
// Everything inside a stage (ops, loops, solves, special ops) is generated code (codegen.h).
//
// Memory, all allocated when the image is built:
//   buf    scratch buffers (Schedule::buffers; per-group copies for Buffer::perGroup) + Newton scratch. Written by
//          generated code.
//   aux    param patterns, constant patterns, global engine-source patterns/ramps, zeros. Read-only for generated
//          code (the host writes params/sources before the stage calls).
//   state  the global state arena, then one voice-state block per voice group (graph::StateLayout offsets).
//   tab    the pointer table: kernel and helper function pointers, resources, special-op info records.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/result.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/program_image.h"
#include "pq/graph/schedule.h"
#include "runtime.h"

namespace pq::jit::detail {

// The generated function of one stage. RT-safe (generated code: no allocation, locks, syscalls or exceptions).
using StageFn = void (*)(float* buf, const float* aux, std::byte* gstate, std::byte* vstate, float* const* ext,
                         const void* const* tab, uint32_t frames, uint32_t group) noexcept;

// Where every schedule object lives (offsets in floats into buf / aux, bytes into state).
struct HostLayout {
  uint32_t maxBlock = 0;
  uint32_t groups = 1;                       // InterpretOptions::maxVoiceGroups equivalent
  std::vector<uint32_t> bufOff, bufSize;     // per Schedule::buffers entry (perGroup: groups copies, bufSize apart)
  uint32_t bufFloats = 0;
  uint32_t solveOff = 0;                     // Newton scratch (n > 1): u, r0 (maxN·8 each), J (maxN²·8), M (doubles)
  uint32_t maxSolveN = 0;
  uint32_t paramOff = 0, constOff = 0, zeroOff = 0;
  std::vector<uint32_t> extOwnOff;           // per external: its own pattern / ramp in aux (global ones)
  uint32_t auxFloats = 0;
  uint32_t globalStateBytes = 0, voiceStride = 0;
  std::vector<uint32_t> opInfo;              // per op: pointer-table slot of its resource / info record, or kNone
};

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
  float* src = nullptr;          // resolved (null: an external, read through extPtr)
  uint32_t ext = graph::kNone;
  uint32_t srcLanes = 1;         // 1 or 8 for a block; 0 for a pattern
  uint32_t group = 0;
};

struct ReduceInit {
  float* out = nullptr;
  bool pattern = false;
  float value = 0.0f;
};

struct StageRun {
  bool voice = false;
  StageFn fn = nullptr;
  uint32_t firstReduce = 0, numReduce = 0;
  uint32_t firstOut = 0, numOut = 0;  // voice: group g's output copies at firstOut + g · numOut
};

class JitImage final : public engine::ProgramImage {
public:
  JitImage() = default;
  ~JitImage() override;
  JitImage(const JitImage&) = delete;
  JitImage& operator=(const JitImage&) = delete;

  void process(engine::ProcessContext& ctx) noexcept override;  // RT-safe
  const graph::StateLayout& stateLayout() const noexcept override { return stateLayout_; }
  const engine::ImageInterface& io() const noexcept override { return io_; }
  engine::StateArena stateArena() noexcept override {
    engine::StateArena a;
    a.global = reinterpret_cast<std::byte*>(stateMem_.data());
    a.voice = reinterpret_cast<std::byte*>(stateMem_.data()) + layout_.globalStateBytes;
    a.voiceStride = layout_.voiceStride;
    a.voiceGroups = io_.maxVoiceGroups;
    return a;
  }
  uint64_t rejectedBlocks() const noexcept override { return rejected_; }

  // ---- built by buildHost() / Compiler ----
  graph::StateLayout stateLayout_;
  engine::ImageInterface io_;
  HostLayout layout_;
  AlignedBuffer<float> buf_, aux_, stateMem_;
  std::vector<const void*> tab_;
  std::vector<rt::ReduceInfo> reduceInfos_;  // reserved up front: the table holds their addresses
  std::vector<rt::SendInfo> sendInfos_;
  std::vector<ReduceInit> reduceInits_;
  std::vector<ParamRun> params_;
  std::vector<ExtRun> exts_;
  std::vector<float*> extPtr_;               // [ext · groups + group]
  std::vector<OutCopy> outs_;                // voice stage copies (via StageRun), then the global ones
  uint32_t firstGlobalOut_ = 0;
  std::vector<StageRun> stages_;
  float* zeros_ = nullptr;                   // maxBlock · 8 zeros (absent inputs)
  rt::RunCtx run_;
  uint64_t rejected_ = 0;
  // Releases the machine code (set by the Compiler; runs in the destructor, off the audio thread).
  std::shared_ptr<void> codeOwner_;

private:
  float* resolveGlobal(ExtRun& e, const engine::ProcessContext& ctx, uint32_t frames) noexcept;
  void copyOut(const OutCopy& c, float* dst, uint32_t frames, bool voice) noexcept;
  void zeroOutputs(const engine::ProcessContext& ctx) noexcept;
};

// Validates the schedule (the interpreter's build checks), lays out memory, initialises state, params, engine
// sources, outputs, special-op records and resources. The stage functions are filled in by the Compiler. Not
// RT-safe.
Result<std::unique_ptr<JitImage>> buildHost(const graph::Schedule& s, uint32_t maxBlockFrames, uint32_t maxVoiceGroups,
                                            const engine::Resources* resources, PtrTable& tab);

// Helpers shared with codegen.
bool isPatternOperand(const graph::Schedule& s, const graph::Operand& o) noexcept;
uint32_t lanesOfOperand(const graph::Schedule& s, const graph::Operand& o) noexcept;  // floats per frame of a Blk operand

}  // namespace pq::jit::detail
