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
//   resources    the Resources tables by index, or the built-in defaults (sine wavetable — at an index
//                ≥ dsp::kBuiltinWavetableBase the built-in waveform there, T4.12 —, identity shaper, a silent sample;
//                tuning_hz has none: its kernel passes the fallback through).
//   stage loop   run(): global stages once, voice stages once per occupied voice group (T1.8) with the group's lane
//                mask / gain / voice_last lane in RunCtx, reducer accumulators initialised before the group loop (and
//                zeroed when no group ran), voice outputs copied after each group, global outputs after the last stage.
// A backend supplies only how one stage runs: run(ctx, global(k), voice(k, group)) calls its two callables (inlined:
// templates, no std::function, no virtual call).
//   pairs        (T1b.2) a backend may also run two occupied voice groups in one call, pair(k, a, b), for the voice
//                stages it marks with enablePairs(): the second group's reducers use their own records (runB(),
//                reduceInfoB()), and its voice outputs that live in a stage-local buffer are read from the
//                backend's second copy. Each reducer still combines group a before group b, so the result is the
//                one the sequential loop computes, bit for bit. Stages with sends cannot be paired (a bus channel
//                adds the sends of one group in op order).
//
//   sleep        (T1.x-silence, SPEC §7.7) the schedule's sleep plan (pq/graph/schedule.h "Sleep plan") at run time.
//                At the start of each global stage, per segment of that stage: its sources are scanned (exactly ±0
//                over the block? ProcessContext::inSilent answers for silent root inputs), its waking params compared
//                with the previous block's, and, awake with silent sources, its states checked: memory words ≤ ε,
//                a delay line's last `tail` written frames ≤ ε (the frames written since the last check are scanned,
//                once the whole span), smoother residuals exactly 0. Quiescent → flushed (the ≤ ε residue set to 0)
//                and asleep. Asleep with silent sources and unchanged params → stays asleep (state untouched).
//                A stage in its param-ramp mode (decideRamps) counts as a param change for its segments
//                (EX.x-fix-settle: a smoother falls asleep at its target, not mid-ramp).
//                Anything else → awake for the whole block. The backends read regionAsleep (the interpreter) or
//                the flag word in aux (layout().sleepOff + segment, generated code) per sleepable region and run
//                it or zero its fill buffers instead (fillRegion), one branch per region per block. ε = kSleepEpsilon
//                (2^-30, −180 dBFS on the state, so ≤ −120 dBFS at the output through 60 dB of gain): the tail cut a
//                decaying (feedback) segment gets; a feed-forward one ends exactly.
//                After the last stage: ProcessContext::outSilent / tailsDone / dormant.
//
//   inspection   (T4.5, pq/engine/inspect.h) probes: after each voice group of a probed voice value's stage and after
//                the last stage for global values, the bound probes' values are copied into their TapRing / ProbeLevel
//                (a sleeping region's value reads 0). Cost meter: while the bound CostMeter wants this image, begin()
//                reads the TSC and raises the aux flag word (layout().meterFlagOff) that tells the backends to time
//                their regions into regionCycles() (buf, layout().meterOff: per schedule region its cycles and how
//                often it was timed, zeroed per block); after the last stage the block's cycles go to the meter.
// Threads: build() and the build-time accessors are not RT-safe (they allocate). begin(), run() and the special-op
// kernels are RT-safe. The object must not move after build() (the special-op records point at its RunCtx): backends
// hold it as a member of a heap-allocated image.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/rt_heap.h"
#include "pq/core/result.h"
#include "pq/dsp/kernel.h"
#include "pq/dsp/tables.h"
#include "pq/engine/inspect.h"
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

// The table a wavetable / shaper / sample / tuning_hz / wavetable_pos / voice_tap op reads: Resources by index, else
// the built-in default (none for tuning and voice taps; for a frame table the one-frame sine table built for
// `sampleRate`). Not RT-safe (the defaults are built on first use).
const void* resourceFor(const Resources* resources, dsp::Resource kind, uint32_t index, double sampleRate = 48000.0);

// ---- The host ---------------------------------------------------------------------------------------------------

struct BuildOptions {
  const char* who = "image";        // error prefix ("interpreter", "jit")
  SleepMode sleep = SleepMode::On;  // T1.x-silence
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
  uint32_t sideGlobalBytes = 0, sideVoiceStride = 0;  // TX.smooth-fix1: the ramp side table
  uint32_t maxSolveN = 0;                // unknowns of the largest solve block
  uint32_t sleepOff = 0;                  // aux: one flag word per sleep segment (non-zero bits: skip its regions)
  uint32_t meterOff = 0;                  // buf: per schedule region two uint64: cycles, timings (4 floats), T4.5
  uint32_t meterFlagOff = 0;              // aux: one word, non-zero while this block is metered, T4.5
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
  float* buf() noexcept { return bufMem_; }
  float* aux() noexcept { return auxMem_; }
  float* zeros() const noexcept { return zeros_; }
  std::byte* globalState() noexcept { return reinterpret_cast<std::byte*>(stateMem_); }
  std::byte* voiceState(uint32_t group) noexcept {
    return globalState() + L_.globalStateBytes + static_cast<size_t>(group) * L_.voiceStride;
  }
  // TX.smooth-fix1: the ramp side table (its own allocation: the state arenas stay the ramp-free ones).
  std::byte* sideGlobalState() noexcept { return reinterpret_cast<std::byte*>(sideMem_.data()); }
  std::byte* sideVoiceState(uint32_t group) noexcept {
    return sideGlobalState() + L_.sideGlobalBytes + static_cast<size_t>(group) * L_.sideVoiceStride;
  }
  std::byte* statePtr(const graph::StateEntry& e, uint32_t group) noexcept {
    if (e.side) return e.arena == graph::Poly::Voice ? sideVoiceState(group) + e.offset : sideGlobalState() + e.offset;
    return e.arena == graph::Poly::Voice ? voiceState(group) + e.offset : globalState() + e.offset;
  }
  // A buffer / param / const operand's pointer for voice group `group` (externals: zeros(); they are resolved per
  // block through extPtr()).
  float* resolve(const graph::Operand& o, uint32_t group) noexcept;
  const void* resource(dsp::Resource kind, uint32_t index) const {
    return resourceFor(resources_, kind, index, io_.sampleRate);
  }
  const ReduceInfo* reduceInfo(uint32_t op) const noexcept;  // null unless op is a Reduce
  const SendInfo* sendInfo(uint32_t op) const noexcept;      // null unless op is a Send
  // Pairs (T1b.2; build time, not RT-safe): pairableStages[k] marks voice stages run(…, pair) may pair (stages with
  // sends are refused: false is stored); alt(k, buffer) is where the second group of a pair keeps a stage-local
  // voice value of stage k (null: in the buffer itself, i.e. per-group or not written in the stage).
  void enablePairs(const std::vector<uint8_t>& pairableStages, const std::function<float*(uint32_t, uint32_t)>& alt);
  const ReduceInfo* reduceInfoB(uint32_t op) const noexcept;  // a reducer's record for a pair's second group
  bool pairable(uint32_t stage) const noexcept { return stage < pairable_.size() && pairable_[stage] != 0; }

  // ---- the ProgramImage accessors ----
  const ImageInterface& io() const noexcept { return io_; }
  const graph::StateLayout& stateLayout() const noexcept { return stateLayout_; }
  StateArena stateArena() noexcept {
    StateArena a;
    a.global = globalState();
    a.voice = globalState() + L_.globalStateBytes;
    a.voiceStride = L_.voiceStride;
    a.voiceGroups = io_.maxVoiceGroups;
    if (!sideMem_.empty()) {
      a.sideGlobal = sideGlobalState();
      a.sideVoice = sideGlobalState() + L_.sideGlobalBytes;
      a.sideVoiceStride = L_.sideVoiceStride;
    }
    return a;
  }
  uint64_t rejectedBlocks() const noexcept { return rejected_; }

  // ---- sleep (T1.x-silence; RT-safe) ----
  SleepMode sleepMode() const noexcept { return sleepMode_; }
  bool hasSleep() const noexcept { return !segs_.empty(); }
  // The region's segment is asleep this block and SleepMode::On: skip it (fillRegion instead).
  bool regionAsleep(uint32_t region) const noexcept {
    const uint32_t g = region < regionSeg_.size() ? regionSeg_[region] : graph::kNone;
    return g != graph::kNone && segs_[g].skip;
  }
  // The region's segment is asleep this block (Shadow: its ops still run).
  bool regionSegmentAsleep(uint32_t region) const noexcept {
    const uint32_t g = region < regionSeg_.size() ? regionSeg_[region] : graph::kNone;
    return g != graph::kNone && segs_[g].asleep;
  }
  void fillRegion(uint32_t region, uint32_t frames) noexcept;                // zero the region's fill buffers
  bool regionFillsSilent(uint32_t region, uint32_t frames) const noexcept;   // every fill buffer is ±0 (Shadow)
  void countViolation() noexcept { ++sleepStats_.violations; }
  uint32_t numSegments() const noexcept { return static_cast<uint32_t>(segs_.size()); }
  bool segmentAsleep(uint32_t segment) const noexcept { return segment < segs_.size() && segs_[segment].asleep; }
  const SleepStats& sleepStats() const noexcept { return sleepStats_; }

  // ---- inspection (T4.5, pq/engine/inspect.h) ----
  // Not RT-safe: binds the probes by id and the cost meter (call before the image is published).
  bool bindInspect(const InspectBinding& b);
  const CostMeter* boundMeter() const noexcept { return meter_; }
  // Build time: per schedule region, the first region of the unit its time is counted in (default: itself).
  void setMeterUnits(const std::vector<uint32_t>& units) { meterUnits_.assign(units.begin(), units.end()); }
  uint32_t numRegions() const noexcept { return numRegions_; }
  // This block is metered (after begin()). RT-safe.
  bool metering() const noexcept { return metering_; }
  // Per region: [2r] cycles, [2r + 1] how many times it was timed (this block).
  uint64_t* regionCycles() noexcept { return reinterpret_cast<uint64_t*>(bufMem_ + L_.meterOff); }
  void addRegionCycles(uint32_t region, uint64_t cycles) noexcept {  // RT-safe
    uint64_t* c = regionCycles() + 2 * static_cast<size_t>(region);
    c[0] += cycles;
    c[1] += 1;
  }

  // ---- per block (RT-safe) ----
  // Starts a block: false for frames == 0 (nothing to do) or frames > maxBlockFrames (every output zeroed, counted
  // in rejectedBlocks()). Otherwise splats the params and resolves the engine sources (extPtr / extTable).
  bool begin(const ProcessContext& ctx) noexcept;  // RT-safe
  // T1b.x-perf (Q126): some param differs (bitwise, after clamping) from the previous block's, or this is the first
  // block (after begin(); what the JIT's memo regions test).
  bool paramsChanged() const noexcept { return paramsChanged_; }
  // TX.vca-knob2 (graph::Schedule::memoMask): bit p mod 64 set for each param p that changed this block; all bits in
  // the first block; 0 while paramsChanged() is false.
  uint64_t paramsChangedMask() const noexcept { return paramsChangedMask_; }
  // Voice groups of this block's voice table that the image has records for (after begin()).
  uint32_t groups() const noexcept { return groups_; }
  float* extPtr(uint32_t ext, uint32_t group) const noexcept {  // RT-safe
    return extPtr_[static_cast<size_t>(ext) * extGroups_ + group];
  }
  float* const* extTable() const noexcept { return extPtr_.data(); }  // RT-safe: [ext · maxVoiceGroups + group]
  // Runs every stage (after a successful begin()): global(k, ramp) for global stage k, voice(k, g, ramp) for voice
  // stage k and each occupied group g; reducers, RunCtx and output copies around them. RT-safe when the callables are.
  // `ramp` (TX.smooth-fix1, pq/graph/schedule.h "Param ramps"): run the stage's ramp mode, not its ramp-free code —
  // a param its static ramps follow changed within a ramp's length (decideRamps), or (voice) a lane of the group
  // started a note this block (VoiceGroupIO::startMask). Always false for a stage without ramps.
  // It also decides the sleep of each global stage's segments right before the stage, and writes ctx's silence
  // reports (outSilent, tailsDone, dormant) after the outputs.
  template <class GlobalStage, class VoiceStage>
  void run(ProcessContext& ctx, GlobalStage&& global, VoiceStage&& voice) noexcept {
    run(ctx, global, voice, [](uint32_t, uint32_t, uint32_t) noexcept {});
  }
  // The same, with pair(k, a, b) for two occupied groups a < b of a pairable stage (enablePairs) when neither runs
  // its ramp mode; a group left without a partner runs through voice(k, g, ramp).
  template <class GlobalStage, class VoiceStage, class PairStage>
  void run(ProcessContext& ctx, GlobalStage&& global, VoiceStage&& voice, PairStage&& pair) noexcept;

private:
  enum class Src : uint8_t {
    RootGlobal, RootVoice, VoiceSig, TransportBpm, TransportBeat, TransportBarPhase, TransportPlaying, TrackIn,
    Sidechain, Clip, Silence
  };
  struct ExtRun {
    Src src = Src::Silence;
    uint32_t index = 0;    // slot / voice signal / channel (Clip: clip slot · kClipChannels + channel)
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
    float* srcAlt = nullptr;   // pairs: where the value is when this group ran as a pair's second group (or null)
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
    uint32_t firstProbe = 0, numProbes = 0;  // voice probes read after each group (T4.5), into probes_
  };
  // T4.5: one probe of Schedule::probes.
  struct ProbeRun {
    uint32_t id = 0;
    uint32_t buffer = graph::kNone;  // a Buffer operand (per-group resolution for voice values)
    uint32_t ext = graph::kNone;     // an External operand (resolved per block)
    const float* src = nullptr;      // Param / Const pattern, or a global buffer
    uint32_t perFrame = 1;           // floats per frame: 1 (global block), 8 (voice block), 0 (8-float pattern)
    uint32_t region = graph::kNone;  // sleeping region: reads 0
    uint32_t stage = graph::kNone;
    float* alt = nullptr;            // pairs: the second group's copy of a stage-local value
    bool voice = false, isBool = false, sum = false;
    bool captured = false;           // voice: this block's value was taken (Last: the newest voice's group ran)
    TapRing* ring = nullptr;
    ProbeLevel* level = nullptr;
    float* scratch = nullptr;        // maxBlock floats
  };

  // Sleep (T1.x-silence).
  struct SegSource {
    float* buf = nullptr;            // a buffer of an earlier stage, or null: the external `ext`
    uint32_t ext = graph::kNone;
    uint32_t inSlot = graph::kNone;  // a global root input: its io().globalIn slot (ProcessContext::inSilent)
    uint32_t perFrame = 1;           // floats per frame (0: an 8-float pattern)
  };
  struct SegState {
    std::byte* ptr = nullptr;
    graph::SleepStateKind kind = graph::SleepStateKind::Memory;
    uint16_t atom = 0;               // Decay: the dsp::Atom whose state it is (dsp::decayStateSmall)
    uint32_t tail = 0;
    bool known = false;              // delay: the ring positions below are valid
    uint32_t scanPos = 0;            // delay: writes before this ring counter are accounted for
    uint32_t loudPos = 0;            // delay: one past the last write > ε
    uint32_t nonzeroPos = 0;         // delay: one past the last write ≠ ±0
  };
  struct SegRun {
    uint32_t stage = 0;
    uint32_t firstSource = 0, numSources = 0, firstState = 0, numStates = 0, firstParam = 0, numParams = 0;
    bool asleep = false;
    bool skip = false;               // asleep and SleepMode::On: the backends skip its regions
    uint32_t* flag = nullptr;        // in aux (the generated code's copy of `skip`)
  };
  struct FillRun {
    float* ptr = nullptr;
    uint32_t perFrame = 1;           // floats per frame (0: an 8-float pattern)
  };
  bool sleepLayout(const graph::Schedule& s, SleepMode mode);
  void decideSleep(uint32_t stage, const ProcessContext& ctx, uint32_t frames) noexcept;
  bool segmentQuiescent(SegRun& g) noexcept;
  // outsSilent: every global out written this block is exactly ±0 (copyGlobalOut's reports; ctx.outSilent is set).
  void sleepReports(ProcessContext& ctx, bool anyVoice, bool outsSilent) noexcept;

  bool fail(std::string msg);
  bool validate(const graph::Schedule& s);
  bool validateFactors(const graph::Schedule& s, const graph::Region& reg, const graph::Op& op) const;  // TMM.6
  bool validateRamps(const graph::Schedule& s);  // TX.smooth
  void decideRamps() noexcept;                   // TX.smooth-fix1 (begin())
  bool layoutMemory(const graph::Schedule& s, uint32_t extraBufFloats);
  bool externals(const graph::Schedule& s);
  void specialOps(const graph::Schedule& s);
  bool stages(const graph::Schedule& s);
  bool outCopy(const graph::Schedule& s, const graph::OutputRef& o, uint32_t slot, uint32_t group);

  float* resolveGlobal(ExtRun& e, const ProcessContext& ctx, uint32_t frames) noexcept;
  void resolveExternals(const ProcessContext& ctx, uint32_t frames) noexcept;
  void copyOut(const OutCopy& c, float* dst, uint32_t frames, bool voice) noexcept;
  // A global out: the copy and its silence check in one pass (T5.x-perf-audio). Returns whether dst is all ±0.
  bool copyGlobalOut(const OutCopy& c, float* dst, uint32_t frames) noexcept;
  void zeroOutputs(const ProcessContext& ctx) noexcept;
  void initReducers(const StageRun& st, uint32_t frames, bool zero) noexcept;
  void setRun(RunCtx& rc, const VoiceTable* vt, uint32_t gi) const noexcept;  // RT-safe
  void copyGroupOut(const StageRun& st, const VoiceGroupIO& gr, uint32_t gi, uint32_t frames, bool alt) noexcept;
  // T4.5 (RT-safe).
  void probeStageBegin(const StageRun& st) noexcept;
  void probeGroup(const StageRun& st, uint32_t gi, uint32_t frames, bool second) noexcept;
  void probeStageEnd(const StageRun& st, uint32_t frames) noexcept;
  void probeGlobals(uint32_t frames) noexcept;
  void probeWrite(ProbeRun& p, const float* data, uint32_t frames) noexcept;
  void meterEnd(uint32_t frames) noexcept;
  bool probeLayout(const graph::Schedule& s);
  // TMM.2: the voice taps this image's voice ops write (wavetable_pos slots' taps, voice_tap resources) get the
  // block's lane masks, while someone reads them.
  void tapLayout(const graph::Schedule& s);
  void tapMasks(const VoiceTable* vt) noexcept;

  // PF.4b: the image's tables and its buf / aux / state block are on the RT heap (pq/core/rt_heap.h).
  template <class T>
  using Table = RtTable<T>;

  // ---- PF.4b: what every block of a plain image reads (begin, run, sleepReports), together: 7 cache lines, not 15
  // spread over the 2 KiB of this object (64 units' images otherwise crowd the L2 cache). Then the rest, colder.
  const VoiceTable* voices_ = nullptr;
  float* zeros_ = nullptr;  // maxBlock · 8 zeros (absent inputs)
  float* bufMem_ = nullptr;  // in mem_
  float* auxMem_ = nullptr;
  CostMeter* meter_ = nullptr;
  uint32_t* meterFlag_ = nullptr;          // in aux
  uint64_t paramsChangedMask_ = ~uint64_t{0};
  uint32_t maxFrames_ = 0;                 // io_.maxBlockFrames
  uint32_t extGroups_ = 1;                 // L_.groups (extPtr_ rows)
  uint32_t groups_ = 0;
  uint32_t firstGlobalOut_ = 0;
  uint32_t firstGlobalProbe_ = 0, numGlobalProbes_ = 0;
  SleepMode sleepMode_ = SleepMode::On;    // Sleep (T1.x-silence)
  bool firstBlock_ = true;
  bool paramsChanged_ = true;
  bool dormantCapable_ = false;
  bool anyRamps_ = false;                  // TX.smooth-fix1: param ramps (below)
  bool planSegments_ = false;              // the schedule has sleep segments (whatever the mode)
  bool meterActivated_ = false;            // activate() done for meterKey_ (first block after the bind)
  bool metering_ = false;
  RunCtx run_;
  SleepStats sleepStats_;
  Table<ExtRun> exts_{rtHeap()};
  Table<float*> extPtr_{rtHeap()};
  Table<StageRun> stages_{rtHeap()};
  Table<uint32_t> stageSegFirst_{rtHeap()};  // per stage + 1 sentinel: its first segment (segments in stage order)
  Table<OutCopy> outs_{rtHeap()};            // voice stage copies (via StageRun), then the global ones
  Table<SegRun> segs_{rtHeap()};
  Table<const float*> globalRampRem_{rtHeap()};  // TX.smooth: lane 0's frames left of each global param ramp
  Table<VoiceTap*> voiceTaps_{rtHeap()};     // TMM.2 (tapLayout)
  // ---- colder: params (read when they change), ramps, voice stages, special ops, build-time data.
  Table<ParamRun> params_{rtHeap()};
  Table<uint32_t> paramBits_{rtHeap()};      // per param: lane 0 of its pattern in the previous block
  Table<uint8_t> paramChanged_{rtHeap()};    // per param: changed this block (all of them in the first block)
  // TX.smooth-fix1: param ramps, per stage (decideRamps): has ramps, ramp mode this block, frames of ramp mode left,
  // a ramp's length, its static ramps' params (into rampParams_).
  Table<uint8_t> rampStage_{rtHeap()};
  Table<uint8_t> rampOn_{rtHeap()};
  Table<uint32_t> rampLeft_{rtHeap()};
  Table<uint32_t> rampFrames_{rtHeap()};
  Table<uint32_t> rampFirst_{rtHeap()};
  Table<uint32_t> rampCount_{rtHeap()};
  Table<uint32_t> rampParams_{rtHeap()};
  // ... and the global param-rate externals they follow (into rampExts_), every such external once (rampWatch_), and
  // per external lane 0 of its value in the previous block / changed this block.
  Table<uint32_t> rampExtFirst_{rtHeap()};
  Table<uint32_t> rampExtCount_{rtHeap()};
  Table<uint32_t> rampExts_{rtHeap()};
  Table<uint32_t> rampWatch_{rtHeap()};
  Table<uint32_t> extBits_{rtHeap()};
  Table<uint8_t> extChanged_{rtHeap()};
  RunCtx runB_;                              // a pair's second group (T1b.2)
  Table<ReduceInfo> reduceInfos_{rtHeap()};  // fixed addresses (reserved once): records point at them
  Table<ReduceInfo> reduceInfosB_{rtHeap()}; // per reducer, reading runB_ (fixed addresses, reserved once)
  Table<SendInfo> sendInfos_{rtHeap()};
  Table<uint32_t> opInfo_{rtHeap()};         // per op: index into reduceInfos_ / sendInfos_, or kNone
  Table<graph::OpKind> opKind_{rtHeap()};
  Table<uint8_t> bufPerGroup_{rtHeap()};     // per buffer: Buffer::perGroup
  Table<ReduceInit> reduceInits_{rtHeap()};
  Table<uint32_t> outBuffer_{rtHeap()};      // per outs_ entry: its source buffer if that is not per-group, else kNone
  Table<uint32_t> opStage_{rtHeap()};        // per op: its Schedule::stages index
  Table<uint8_t> pairable_{rtHeap()};        // per stage
  Table<SegSource> segSources_{rtHeap()};
  Table<SegState> segStates_{rtHeap()};
  Table<uint32_t> segParams_{rtHeap()};
  Table<uint32_t> regionSeg_{rtHeap()};        // per Schedule::regions entry
  Table<uint32_t> regionFillFirst_{rtHeap()};  // per region + 1 sentinel: into fills_
  Table<FillRun> fills_{rtHeap()};
  float* stateMem_ = nullptr;
  RtHeapArray<float> mem_;                 // buf, aux, state
  uint64_t rejected_ = 0;
  AlignedBuffer<float> sideMem_;            // the ramp side table (graph::StateEntry::side)
  // Inspection (T4.5).
  Table<ProbeRun> probes_{rtHeap()};  // voice probes grouped by stage (StageRun::firstProbe), then the global ones
  AlignedBuffer<float> probeScratch_;
  uint32_t numRegions_ = 0;
  Table<uint32_t> meterUnits_{rtHeap()};
  uint32_t meterKey_ = 0;
  uint64_t meterT0_ = 0;
  std::string who_ = "image";
  Error err_;
  Layout L_;
  const Resources* resources_ = nullptr;
  graph::StateLayout stateLayout_;
  ImageInterface io_;
};

inline void ImageHost::setRun(RunCtx& rc, const VoiceTable* vt, uint32_t gi) const noexcept {  // RT-safe
  const VoiceGroupIO& gr = vt->groups[gi];
  rc.laneMask = gr.laneMask & 0xFFu;
  rc.gain = gr.gain;
  rc.lastLane = vt->lastGroup == gi && vt->lastLane < 8 && ((gr.laneMask >> vt->lastLane) & 1u) ? vt->lastLane : kNoVoice;
  rc.sends = run_.sends;
  rc.numSends = run_.numSends;
}

inline void ImageHost::copyGroupOut(const StageRun& st, const VoiceGroupIO& gr, uint32_t gi, uint32_t frames,
                                    bool alt) noexcept {  // RT-safe
  const uint32_t ob = st.firstOut + gi * st.numOut;
  for (uint32_t c = ob; c < ob + st.numOut; ++c) {
    const OutCopy& oc = outs_[c];
    if (gr.out == nullptr || oc.slot >= gr.numOut || gr.out[oc.slot] == nullptr) continue;
    if (alt && oc.srcAlt != nullptr) {
      OutCopy a = oc;
      a.src = oc.srcAlt;
      copyOut(a, gr.out[oc.slot], frames, true);
    } else {
      copyOut(oc, gr.out[oc.slot], frames, true);
    }
  }
}

template <class GlobalStage, class VoiceStage, class PairStage>
void ImageHost::run(ProcessContext& ctx, GlobalStage&& global, VoiceStage&& voice, PairStage&& pair) noexcept {  // RT-safe
  const uint32_t frames = ctx.frames;
  const VoiceTable* vt = voices_;
  bool anyVoice = false;
  if (!voiceTaps_.empty()) tapMasks(vt);
  for (uint32_t k = 0; k < stages_.size(); ++k) {
    const StageRun& st = stages_[k];
    if (!st.voice) {
      run_.laneMask = 0;
      run_.gain = nullptr;
      run_.lastLane = kNoVoice;
      if (stageSegFirst_[k] != stageSegFirst_[k + 1]) decideSleep(k, ctx, frames);
      const bool ramp = anyRamps_ && rampOn_[k] != 0;
      sleepStats_.rampRuns += ramp ? 1u : 0u;
      global(k, ramp);
      continue;
    }
    initReducers(st, frames, false);
    if (st.numProbes) probeStageBegin(st);
    bool any = false;
    const bool pairs = pairable(k);
    const bool ramps = anyRamps_ && rampStage_[k] != 0, rampAll = ramps && rampOn_[k] != 0;
    auto rampOf = [&](const VoiceGroupIO& g) noexcept { return rampAll || (ramps && (g.startMask & 0xFFu) != 0); };
    for (uint32_t gi = 0; gi < groups_; ++gi) {
      const VoiceGroupIO& gr = vt->groups[gi];
      if ((gr.laneMask & 0xFFu) == 0) continue;  // no voice in this group: not run (T1.8; its state holds)
      any = true;
      setRun(run_, vt, gi);
      uint32_t gb = gi + 1;
      if (pairs)
        while (gb < groups_ && (vt->groups[gb].laneMask & 0xFFu) == 0) ++gb;
      const bool rampA = rampOf(gr);
      if (!pairs || gb >= groups_ || rampA || rampOf(vt->groups[gb])) {
        // TX.smooth-fix1: a group in ramp mode runs alone (a pair function is ramp-free code); its partner then
        // pairs with the next group, or runs alone.
        sleepStats_.rampRuns += rampA ? 1u : 0u;
        voice(k, gi, rampA);
        copyGroupOut(st, gr, gi, frames, false);
        if (st.numProbes) probeGroup(st, gi, frames, false);
        continue;
      }
      setRun(runB_, vt, gb);
      // TX.smooth: a backend's pair may decline a block (returns false, before it changed anything): the two groups
      // then run one by one.
      if constexpr (std::is_same_v<decltype(pair(k, gi, gb)), bool>) {
        if (!pair(k, gi, gb)) {
          voice(k, gi, false);
          copyGroupOut(st, gr, gi, frames, false);
          if (st.numProbes) probeGroup(st, gi, frames, false);
          setRun(run_, vt, gb);
          voice(k, gb, false);
          copyGroupOut(st, vt->groups[gb], gb, frames, false);
          if (st.numProbes) probeGroup(st, gb, frames, false);
          gi = gb;
          continue;
        }
      } else {
        pair(k, gi, gb);
      }
      copyGroupOut(st, gr, gi, frames, false);
      copyGroupOut(st, vt->groups[gb], gb, frames, true);
      if (st.numProbes) {
        probeGroup(st, gi, frames, false);
        probeGroup(st, gb, frames, true);
      }
      gi = gb;
    }
    if (st.numProbes) probeStageEnd(st, frames);
    if (!any) initReducers(st, frames, true);
    anyVoice = anyVoice || any;
  }
  if (anyRamps_)  // TX.smooth-fix1: what is left of the ramps after this block
    for (uint32_t k = 0; k < rampLeft_.size(); ++k) rampLeft_[k] -= rampLeft_[k] < frames ? rampLeft_[k] : frames;
  run_.laneMask = 0;
  run_.gain = nullptr;
  bool outsSilent = true;
  for (uint32_t c = firstGlobalOut_; c < outs_.size(); ++c) {
    const OutCopy& oc = outs_[c];
    if (ctx.out == nullptr || oc.slot >= ctx.numOut || ctx.out[oc.slot] == nullptr) continue;
    const bool z = copyGlobalOut(oc, ctx.out[oc.slot], frames);
    if (ctx.outSilent != nullptr) ctx.outSilent[oc.slot] = z ? 1 : 0;
    outsSilent = outsSilent && z;
  }
  sleepReports(ctx, anyVoice, outsSilent);
  if (numGlobalProbes_) probeGlobals(frames);
  if (metering_) meterEnd(frames);
}

}  // namespace pq::engine::host
