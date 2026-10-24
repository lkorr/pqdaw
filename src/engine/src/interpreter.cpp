// The interpreter backend (T1.7): pq/engine/interpreter.h says what it does; pq/graph/schedule.h says how a
// Schedule must be run. This file is the builder (Schedule → flat records with resolved pointers) and the image's
// process() (the only RT code here).
//
// Layout of what the builder produces:
//   recs_     every op of every region as {KernelFn, KernelArgs}, in schedule order; voice stages materialised once
//             per voice group (pointers to that group's per-group buffers, state and engine data)
//   pool_     the KernelArgs::in/out pointer arrays of all records
//   relocs_   per record in a Loop region (or a solve body): the Blk operands, re-pointed at base + t·lanes before
//             each sub-block / frame
//   sites_    the pointer cells that read engine data (root inputs, voice.*, track/sidechain input), rewritten once
//             per block from the ProcessContext
//   regions_  per (stage, group) region: the record range (and, with solve blocks, an item list)

#include "pq/engine/interpreter.h"

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"
#include "pq/engine/newton.h"
#include "pq/graph/atoms.h"

namespace pq::engine {

namespace {

namespace g = pq::graph;

constexpr uint32_t kFloatAlign = 16;  // 64 bytes
constexpr uint32_t kSolveItem = 0x80000000u;

uint32_t roundUp(uint32_t n, uint32_t a) noexcept { return (n + a - 1) / a * a; }

// ---- per-block state the special ops read ------------------------------------------------------------------
struct RunCtx {
  uint32_t laneMask = 0;          // active lanes of the voice group being run
  uint32_t lastLane = kNoVoice;   // voice_last's lane if it is in this group
  const float* gain = nullptr;    // the group's per-sample lane gain (VoiceGroupIO::gain), or null
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

// ---- special ops as kernels (same calling convention, so the region loops stay uniform) ---------------------

// Broadcast: 1-lane block → 8-lane block (every voice sees the global value).
void broadcastKernel(const dsp::KernelArgs& a) noexcept {  // RT-safe
  const float* in = a.in[0];
  float* out = a.out[0];
  if (a.splMask & 1u) {
    const __m256 v = _mm256_set1_ps(in[0]);
    for (uint32_t f = 0; f < a.frames; ++f) _mm256_store_ps(out + f * 8, v);
    return;
  }
  for (uint32_t f = 0; f < a.frames; ++f) _mm256_store_ps(out + f * 8, _mm256_broadcast_ss(in + f));
}

inline float horizontalSum(__m256 v) noexcept {  // RT-safe
  __m128 s = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
  s = _mm_add_ps(s, _mm_movehl_ps(s, s));
  s = _mm_add_ss(s, _mm_movehdup_ps(s));
  return _mm_cvtss_f32(s);
}

inline __m256 laneMaskVector(uint32_t mask) noexcept {  // RT-safe: bit v → lane v all ones
  const __m256i bits = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
  const __m256i m = _mm256_and_si256(_mm256_set1_epi32(static_cast<int32_t>(mask)), bits);
  return _mm256_castsi256_ps(_mm256_cmpeq_epi32(m, bits));
}

inline bool maskTrue(float x) noexcept { return std::bit_cast<uint32_t>(x) != 0u; }  // RT-safe

// One frame of 8 lanes combined into acc (active lanes only).
inline float reduceFrame(const ReduceInfo& ri, const float* x, float acc, uint32_t mask, uint32_t lastLane) noexcept {
  if (ri.mode == ReduceMode::Last) return lastLane < 8 ? x[lastLane] : acc;
  if (ri.isBool) {
    if (ri.mode == ReduceMode::Lowest) {  // all
      bool all = maskTrue(acc);
      for (uint32_t m = mask; m; m &= m - 1) all = all && maskTrue(x[std::countr_zero(m)]);
      return dsp::boolMask(all);
    }
    bool any = maskTrue(acc);  // sum / highest / loudest: any
    for (uint32_t m = mask; m; m &= m - 1) any = any || maskTrue(x[std::countr_zero(m)]);
    return dsp::boolMask(any);
  }
  switch (ri.mode) {
    case ReduceMode::Sum: return acc + horizontalSum(_mm256_and_ps(_mm256_loadu_ps(x), laneMaskVector(mask)));
    case ReduceMode::Highest:
      for (uint32_t m = mask; m; m &= m - 1) {
        const float v = x[std::countr_zero(m)];
        acc = v > acc ? v : acc;
      }
      return acc;
    case ReduceMode::Lowest:
      for (uint32_t m = mask; m; m &= m - 1) {
        const float v = x[std::countr_zero(m)];
        acc = v < acc ? v : acc;
      }
      return acc;
    case ReduceMode::Loudest:
      for (uint32_t m = mask; m; m &= m - 1) {
        const float v = x[std::countr_zero(m)];
        acc = std::fabs(v) > std::fabs(acc) ? v : acc;
      }
      return acc;
    default: return acc;
  }
}

// Reduce: combines one voice group (8 lanes, active lanes only) into the global accumulator out[0].
void reduceKernel(const dsp::KernelArgs& a) noexcept {  // RT-safe
  const ReduceInfo& ri = *static_cast<const ReduceInfo*>(a.state);
  const uint32_t mask = ri.run->laneMask;
  const uint32_t last = ri.run->lastLane;
  if (mask == 0 && !(ri.mode == ReduceMode::Last && last < 8)) return;
  const float* in = a.in[0];
  float* out = a.out[0];
  const uint32_t inStride = ri.inPattern ? 0u : 8u;
  if (ri.outPattern) {
    const float v = reduceFrame(ri, in, out[0], mask, last);
    for (uint32_t l = 0; l < 8; ++l) out[l] = v;
    return;
  }
  const float* gain = ri.run->gain;  // per-sample lane gain (VoiceGroupIO::gain), or null
  if (ri.mode == ReduceMode::Sum && !ri.isBool) {
    const __m256 m = laneMaskVector(mask);
    if (gain != nullptr) {  // select(gain ≠ 0, x, 0) · gain: a pre-onset / idle lane's NaN never gets in
      const __m256 z = _mm256_setzero_ps();
      for (uint32_t f = 0; f < a.frames; ++f) {
        const __m256 g = _mm256_load_ps(gain + f * 8);
        const __m256 x = _mm256_and_ps(_mm256_loadu_ps(in + f * inStride), _mm256_and_ps(m, _mm256_cmp_ps(g, z, _CMP_NEQ_OQ)));
        out[f] += horizontalSum(_mm256_mul_ps(x, g));
      }
      return;
    }
    for (uint32_t f = 0; f < a.frames; ++f) out[f] += horizontalSum(_mm256_and_ps(_mm256_loadu_ps(in + f * inStride), m));
    return;
  }
  if (gain != nullptr && ri.mode == ReduceMode::Last) {  // the last voice only from its onset (gain ≠ 0)
    if (last >= 8) return;
    for (uint32_t f = 0; f < a.frames; ++f)
      if (gain[f * 8 + last] != 0.0f) out[f] = in[f * inStride + last];
    return;
  }
  // Highest / Lowest / Loudest and bool any / all keep the block lane mask: their comparisons skip NaN.
  for (uint32_t f = 0; f < a.frames; ++f) out[f] = reduceFrame(ri, in + f * inStride, out[f], mask, last);
}

// Send: adds the signal (summed over the active voices for a voice op) into a bus channel.
void sendKernel(const dsp::KernelArgs& a) noexcept {  // RT-safe
  const SendInfo& si = *static_cast<const SendInfo*>(a.state);
  const RunCtx& rc = *si.run;
  if (si.bus >= rc.numSends || rc.sends == nullptr) return;
  const SendBus& bus = rc.sends[si.bus];
  if (si.channel >= bus.numChannels || bus.channels == nullptr || bus.channels[si.channel] == nullptr) return;
  float* dst = bus.channels[si.channel];
  const float* in = a.in[0];
  if (si.lanes == 1) {
    if (si.inPattern) {
      for (uint32_t f = 0; f < a.frames; ++f) dst[f] += in[0];
    } else {
      for (uint32_t f = 0; f < a.frames; ++f) dst[f] += in[f];
    }
    return;
  }
  const __m256 m = laneMaskVector(rc.laneMask);
  const uint32_t stride = si.inPattern ? 0u : 8u;
  if (rc.gain != nullptr) {  // as voice_sum: select(gain ≠ 0, x, 0) · gain
    const __m256 z = _mm256_setzero_ps();
    for (uint32_t f = 0; f < a.frames; ++f) {
      const __m256 g = _mm256_load_ps(rc.gain + f * 8);
      const __m256 x = _mm256_and_ps(_mm256_loadu_ps(in + f * stride), _mm256_and_ps(m, _mm256_cmp_ps(g, z, _CMP_NEQ_OQ)));
      dst[f] += horizontalSum(_mm256_mul_ps(x, g));
    }
    return;
  }
  for (uint32_t f = 0; f < a.frames; ++f) dst[f] += horizontalSum(_mm256_and_ps(_mm256_loadu_ps(in + f * stride), m));
}

// ---- built-in default tables -----------------------------------------------------------------------------------
const dsp::WavetableView& defaultWavetable() {
  static const dsp::Wavetable table = [] {
    const AlignedBuffer<float> cycle = dsp::Wavetable::basicCycle(dsp::BasicShape::Sine, 2048);
    auto t = dsp::Wavetable::build(cycle.span());
    return t ? std::move(t.value()) : dsp::Wavetable{};
  }();
  return table.view();
}
const dsp::ShaperView& defaultShaper() {
  static const dsp::ShaperTable table = [] {
    const float pts[2] = {-1.0f, 1.0f};
    auto t = dsp::ShaperTable::build(pts, -1.0f, 1.0f);
    return t ? std::move(t.value()) : dsp::ShaperTable{};
  }();
  return table.view();
}
const dsp::SampleView& defaultSample() {
  static const dsp::SampleData table = [] {
    const float pts[1] = {0.0f};
    auto t = dsp::SampleData::build(pts);
    return t ? std::move(t.value()) : dsp::SampleData{};
  }();
  return table.view();
}

// ---- image ------------------------------------------------------------------------------------------------------
struct Rec {
  dsp::KernelFn fn = nullptr;
  dsp::KernelArgs args;
  uint32_t inBase = 0, outBase = 0;      // into pool_
  uint32_t firstReloc = 0, numReloc = 0;
  uint32_t info = g::kNone;              // ReduceInfo / SendInfo index (fixed up at the end)
  uint8_t infoKind = 0;                  // 0 none, 1 reduce, 2 send
};

struct Reloc {
  uint32_t cell;   // pool_ index
  uint32_t stride; // floats per frame of the operand (its lanes)
  float* base;
};

struct RegionRun {
  g::RegionKind kind = g::RegionKind::Block;
  uint32_t subBlock = 0;
  uint32_t firstRec = 0, numRec = 0;
  uint32_t firstItem = 0, numItems = 0;  // with solve blocks: rec index, or kSolveItem | solve index
};

struct OperandRef {  // element (frame t, lane l) = base[t·stride + l]; stride 0 = a pattern
  float* base = nullptr;
  uint32_t stride = 0;
  float& at(uint32_t t, uint32_t l) const noexcept { return base[t * stride + l]; }
};

struct UnkRun {
  OperandRef value;
  OperandRef init;        // base null: start from `guess`
  float* guess = nullptr; // previous solution (state), 8 floats
};

struct EqRun {
  OperandRef lhs, rhs;
};

// Forward-mode tangents of a solve body (pq/engine/newton.h): per body op, where its inputs' and outputs' tangents
// live (a slot of the tangent scratch, one per distinct buffer of the block) and which directions are non-zero.
struct AdOp {
  newton::TangentStepFn step = nullptr;
  uint32_t numIn = 0, numOut = 0;
  uint32_t firstIn = 0, firstOut = 0;  // into adPorts_
  uint32_t outAny = 0;                 // OR of the outputs' direction bits
};
using AdPort = newton::TanPort;  // a tangent slot and its direction bits (0: no tangent, slot unused)

struct SolveRun {
  uint32_t iterations = 1;
  uint32_t lanes = 1;
  uint32_t n = 0;
  uint32_t firstBodyRec = 0, numBodyRec = 0;
  uint32_t firstUnk = 0, firstEq = 0;
  bool loop = false;  // body records are relocated to frame t
  bool ad = false;    // Jacobian by forward-mode tangents (else finite differences)
  bool finalEval = true;  // run the body once more after the last iteration (newton::bodyReadAfter)
  uint32_t firstAdOp = 0;   // numBodyRec AdOps
  uint32_t firstAdEq = 0;   // per equation: lhs, rhs AdPorts (into adPorts_)
  uint32_t firstAdUnk = 0;  // per unknown: its slot (into adPorts_)
};

struct StageRun {
  bool voice = false;
  uint32_t firstRegion = 0, numRegions = 0;  // voice: group g's regions at firstRegion + g · numRegions
  uint32_t firstReduce = 0, numReduce = 0;
  uint32_t firstOut = 0, numOut = 0;         // voice: group g's output copies at firstOut + g · numOut
};

struct ReduceInit {
  float* out = nullptr;
  bool pattern = false;
  float value = 0.0f;
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

struct Site {
  float** target = nullptr;
  uint32_t ext = 0;
  uint32_t group = 0;
};

struct OutCopy {
  uint32_t slot = 0;
  float* src = nullptr;      // resolved (null: an external, read through extPtr_)
  uint32_t ext = g::kNone;
  uint32_t srcLanes = 1;     // 1 or 8 for a block; 0 for a pattern
  uint32_t group = 0;
};

class InterpretedImage final : public ProgramImage {
public:
  void process(ProcessContext& ctx) noexcept override;  // RT-safe
  const g::StateLayout& stateLayout() const noexcept override { return layout_; }
  const ImageInterface& io() const noexcept override { return io_; }
  StateArena stateArena() noexcept override {
    StateArena a;
    a.global = reinterpret_cast<std::byte*>(stateMem_.data());
    a.voice = reinterpret_cast<std::byte*>(stateMem_.data()) + globalStateBytes_;
    a.voiceStride = voiceStride_;
    a.voiceGroups = io_.maxVoiceGroups;
    return a;
  }
  uint64_t rejectedBlocks() const noexcept override { return rejected_; }

  // ---- built by Builder ----
  g::StateLayout layout_;
  ImageInterface io_;
  AlignedBuffer<float> bufMem_, auxMem_, stateMem_, solveMem_, tanMem_;
  uint32_t globalStateBytes_ = 0, voiceStride_ = 0;
  std::vector<float*> pool_;
  std::vector<Rec> recs_;
  std::vector<Reloc> relocs_;
  std::vector<uint32_t> items_;
  std::vector<RegionRun> regions_;
  std::vector<StageRun> stages_;
  std::vector<SolveRun> solves_;
  std::vector<UnkRun> unks_;
  std::vector<EqRun> eqs_;
  std::vector<AdOp> adOps_;
  std::vector<AdPort> adPorts_;
  std::vector<newton::Tan<float>> adIn_, adOut_;  // scratch: one op's input / output tangents (largest arity)
  std::vector<ReduceInfo> reduceInfos_;
  std::vector<SendInfo> sendInfos_;
  std::vector<ReduceInit> reduceInits_;
  std::vector<ParamRun> params_;
  std::vector<ExtRun> exts_;
  std::vector<Site> sites_;          // global sites first, then voice sites sorted by group
  uint32_t numGlobalSites_ = 0;
  std::vector<uint32_t> voiceSiteBegin_;  // per group: first site (size maxGroups + 1)
  std::vector<float*> extPtr_;       // [ext · maxGroups + group]
  std::vector<OutCopy> outs_;        // voice stage copies (via StageRun), then the global ones
  uint32_t firstGlobalOut_ = 0;
  float* zeros_ = nullptr;           // maxBlock · 8 zeros (absent inputs)
  uint32_t maxSolveN_ = 0;
  RunCtx run_;
  uint64_t rejected_ = 0;

private:
  void runRegion(const RegionRun& r, uint32_t frames) noexcept;
  void relocate(const Rec& rec, uint32_t t) noexcept {  // RT-safe
    for (uint32_t k = rec.firstReloc; k < rec.firstReloc + rec.numReloc; ++k) {
      const Reloc& rl = relocs_[k];
      pool_[rl.cell] = rl.base + static_cast<size_t>(t) * rl.stride;
    }
  }
  void runSolve(const SolveRun& s, uint32_t t) noexcept;
  void evalBody(const SolveRun& s, uint32_t t) noexcept {  // RT-safe
    for (uint32_t i = s.firstBodyRec; i < s.firstBodyRec + s.numBodyRec; ++i) {
      Rec& rec = recs_[i];
      if (s.loop) {
        relocate(rec, t);
        rec.args.frames = 1;
      }
      rec.fn(rec.args);
    }
  }
  void evalBodyTangents(const SolveRun& s, uint32_t t) noexcept;
  void finiteDifferenceJacobian(const SolveRun& s, uint32_t t, const float* r0, float* J) noexcept;
  float* resolveGlobal(ExtRun& e, const ProcessContext& ctx, uint32_t frames) noexcept;
  void copyOut(const OutCopy& c, float* dst, uint32_t frames, bool voice) noexcept;
  void zeroOutputs(const ProcessContext& ctx) noexcept;
};

inline void splat8(float* p, float v) noexcept { _mm256_store_ps(p, _mm256_set1_ps(v)); }  // RT-safe

float* InterpretedImage::resolveGlobal(ExtRun& e, const ProcessContext& ctx, uint32_t frames) noexcept {  // RT-safe
  const TransportSnapshot& tr = ctx.transport;
  auto channelOf = [&](const float* const* chans, uint32_t count) noexcept -> const float* {
    return chans != nullptr && e.index < count && chans[e.index] != nullptr ? chans[e.index] : zeros_;
  };
  auto rampValue = [&](uint32_t f) noexcept -> float {
    const double beat = tr.beat + static_cast<double>(f) * tr.beatsPerFrame;
    if (e.src == Src::TransportBeat) return static_cast<float>(beat);
    const double bars = (beat - tr.barStartBeat) / (tr.beatsPerBar > 0.0 ? tr.beatsPerBar : 4.0);
    const float ph = static_cast<float>(bars - std::floor(bars));
    return ph < 1.0f ? ph : 0.99999994f;
  };
  const float* src = zeros_;
  switch (e.src) {
    case Src::RootGlobal: src = channelOf(ctx.in, ctx.numIn); break;
    case Src::TrackIn: src = channelOf(ctx.trackIn, ctx.numTrackIn); break;
    case Src::Sidechain: src = channelOf(ctx.sidechainIn, ctx.numSidechainIn); break;
    case Src::TransportBpm:
      if (e.pattern) {
        splat8(e.own, static_cast<float>(tr.bpm));
        return e.own;
      }
      for (uint32_t f = 0; f < frames; ++f) e.own[f] = static_cast<float>(tr.bpm);
      return e.own;
    case Src::TransportPlaying:
      if (e.pattern) {
        splat8(e.own, dsp::boolMask(tr.playing));
        return e.own;
      }
      for (uint32_t f = 0; f < frames; ++f) e.own[f] = dsp::boolMask(tr.playing);
      return e.own;
    case Src::TransportBeat:
    case Src::TransportBarPhase:
      if (e.pattern) {
        splat8(e.own, rampValue(0));
        return e.own;
      }
      for (uint32_t f = 0; f < frames; ++f) e.own[f] = rampValue(f);
      return e.own;
    default: return zeros_;
  }
  if (!e.pattern) return const_cast<float*>(src);  // read-only by contract: kernels never write their inputs
  splat8(e.own, src[0]);
  return e.own;
}

void InterpretedImage::copyOut(const OutCopy& c, float* dst, uint32_t frames, bool voice) noexcept {  // RT-safe
  const float* src = c.src ? c.src : extPtr_[static_cast<size_t>(c.ext) * io_.maxVoiceGroups + c.group];
  if (!voice) {
    if (c.srcLanes == 0) {
      for (uint32_t f = 0; f < frames; ++f) dst[f] = src[0];
    } else {
      std::memcpy(dst, src, sizeof(float) * frames);
    }
    return;
  }
  if (c.srcLanes == 8) {
    std::memcpy(dst, src, sizeof(float) * frames * 8);
  } else if (c.srcLanes == 0) {
    const __m256 v = _mm256_loadu_ps(src);
    for (uint32_t f = 0; f < frames; ++f) _mm256_storeu_ps(dst + f * 8, v);
  } else {
    for (uint32_t f = 0; f < frames; ++f) _mm256_storeu_ps(dst + f * 8, _mm256_set1_ps(src[f]));
  }
}

void InterpretedImage::zeroOutputs(const ProcessContext& ctx) noexcept {  // RT-safe
  for (uint32_t i = 0; i < ctx.numOut && ctx.out; ++i)
    if (ctx.out[i]) std::memset(ctx.out[i], 0, sizeof(float) * ctx.frames);
  if (ctx.voices && ctx.voices->groups)
    for (uint32_t gi = 0; gi < ctx.voices->numGroups; ++gi) {
      const VoiceGroupIO& gr = ctx.voices->groups[gi];
      for (uint32_t i = 0; i < gr.numOut && gr.out; ++i)
        if (gr.out[i]) std::memset(gr.out[i], 0, sizeof(float) * ctx.frames * 8);
    }
}

void InterpretedImage::runRegion(const RegionRun& r, uint32_t frames) noexcept {  // RT-safe
  switch (r.kind) {
    case g::RegionKind::Control:
      if (r.numItems == 0) {
        for (uint32_t i = r.firstRec; i < r.firstRec + r.numRec; ++i) recs_[i].fn(recs_[i].args);
      } else {
        for (uint32_t k = r.firstItem; k < r.firstItem + r.numItems; ++k) {
          const uint32_t it = items_[k];
          if (it & kSolveItem) runSolve(solves_[it & ~kSolveItem], 0);
          else recs_[it].fn(recs_[it].args);
        }
      }
      return;
    case g::RegionKind::Block:
      for (uint32_t i = r.firstRec; i < r.firstRec + r.numRec; ++i) {
        Rec& rec = recs_[i];
        rec.args.frames = frames;
        rec.fn(rec.args);
      }
      return;
    case g::RegionKind::Loop: {
      const uint32_t sub = r.subBlock ? r.subBlock : 1u;
      for (uint32_t t = 0; t < frames; t += sub) {
        const uint32_t n = std::min(sub, frames - t);
        if (r.numItems == 0) {
          for (uint32_t i = r.firstRec; i < r.firstRec + r.numRec; ++i) {
            Rec& rec = recs_[i];
            relocate(rec, t);
            rec.args.frames = n;
            rec.fn(rec.args);
          }
        } else {
          for (uint32_t k = r.firstItem; k < r.firstItem + r.numItems; ++k) {
            const uint32_t it = items_[k];
            if (it & kSolveItem) {
              runSolve(solves_[it & ~kSolveItem], t);  // regions with solves run per sample (n = 1)
              continue;
            }
            Rec& rec = recs_[it];
            relocate(rec, t);
            rec.args.frames = n;
            rec.fn(rec.args);
          }
        }
      }
      return;
    }
  }
}

// Newton per lane (pq/engine/newton.h has the scheme; the JIT generates the same arithmetic). Scratch: solveMem_
// holds u, r0, rp, rm (n·8 each), up, um (8 each), J and Jfd (n·n·8 each) and the elimination matrix (n·(n+1)
// doubles); tanMem_ the tangents (slots · n · 8).
void InterpretedImage::evalBodyTangents(const SolveRun& s, uint32_t t) noexcept {  // RT-safe
  const uint32_t n = s.n, L = s.lanes;
  float* T = tanMem_.data();
  for (uint32_t j = 0; j < n; ++j) {
    const AdPort& u = adPorts_[s.firstAdUnk + j];
    for (uint32_t l = 0; l < L; ++l) T[(u.slot * n + j) * 8 + l] = 1.0f;
  }
  alignas(32) float snap[3 * 8] = {};
  for (uint32_t k = 0; k < s.numBodyRec; ++k) {
    Rec& rec = recs_[s.firstBodyRec + k];
    if (s.loop) {
      relocate(rec, t);
      rec.args.frames = 1;
    }
    const AdOp& op = adOps_[s.firstAdOp + k];
    if (op.outAny == 0) {
      rec.fn(rec.args);
      continue;
    }
    // The rules read input values; an in-place op overwrites one, so take them before the call.
    const uint32_t na = std::min<uint32_t>(op.numIn, 3);
    for (uint32_t i = 0; i < na; ++i)
      for (uint32_t l = 0; l < L; ++l) snap[i * 8 + l] = rec.args.in[i][l];
    rec.fn(rec.args);
    newton::TangentStep g;
    g.numIn = op.numIn;
    g.numOut = op.numOut;
    g.n = n;
    g.lanes = L;
    g.outAny = op.outAny;
    g.a = snap;
    g.y = rec.args.out[0];
    g.in = adPorts_.data() + op.firstIn;
    g.out = adPorts_.data() + op.firstOut;
    g.T = T;
    g.dScratch = adIn_.data();
    g.dyScratch = adOut_.data();
    op.step(g);
  }
}

// J by finite differences around the current u (the body's values are those at u and r0 its residuals).
void InterpretedImage::finiteDifferenceJacobian(const SolveRun& s, uint32_t t, const float* r0, float* J) noexcept {  // RT-safe
  const uint32_t n = s.n, L = s.lanes, N8 = maxSolveN_ * 8;
  const float* u = solveMem_.data();
  float* rp = solveMem_.data() + 2 * N8;
  float* rm = rp + N8;
  float* up = rm + N8;
  float* um = up + 8;
  const UnkRun* unk = unks_.data() + s.firstUnk;
  const EqRun* eq = eqs_.data() + s.firstEq;
  auto residuals = [&](float* r) noexcept {
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t l = 0; l < L; ++l) r[i * 8 + l] = eq[i].lhs.at(t, l) - eq[i].rhs.at(t, l);
  };
  for (uint32_t j = 0; j < n; ++j) {
    for (uint32_t l = 0; l < L; ++l) {
      const float saved = u[j * 8 + l];
      const float step = newton::kStep * std::max(std::fabs(saved), newton::kStepFloor);
      const float h = std::isfinite(step) ? step : 0.0f;
      up[l] = saved + h;
      um[l] = saved - h;
    }
    for (uint32_t l = 0; l < L; ++l) unk[j].value.at(t, l) = up[l];
    evalBody(s, t);
    residuals(rp);
    for (uint32_t l = 0; l < L; ++l) unk[j].value.at(t, l) = um[l];
    evalBody(s, t);
    residuals(rm);
    for (uint32_t l = 0; l < L; ++l) unk[j].value.at(t, l) = u[j * 8 + l];
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t l = 0; l < L; ++l) {
        const float p = rp[i * 8 + l], m = rm[i * 8 + l], saved = u[j * 8 + l];
        // Central difference over the step actually taken in float; one-sided where a side is not finite.
        J[(i * n + j) * 8 + l] = std::isfinite(p) && std::isfinite(m) ? (p - m) / (up[l] - um[l])
                                 : std::isfinite(p)                    ? (p - r0[i * 8 + l]) / (up[l] - saved)
                                                                       : (r0[i * 8 + l] - m) / (saved - um[l]);
      }
  }
}

void InterpretedImage::runSolve(const SolveRun& s, uint32_t t) noexcept {  // RT-safe
  const uint32_t n = s.n, L = s.lanes, N8 = maxSolveN_ * 8;
  float* u = solveMem_.data();
  float* r0 = u + N8;
  float* J = u + 4 * N8 + 16;
  float* Jfd = J + static_cast<size_t>(maxSolveN_) * N8;
  double* M = reinterpret_cast<double*>(Jfd + static_cast<size_t>(maxSolveN_) * N8);
  const float* T = tanMem_.data();
  const UnkRun* unk = unks_.data() + s.firstUnk;
  const EqRun* eq = eqs_.data() + s.firstEq;
  auto writeU = [&]() noexcept {
    for (uint32_t j = 0; j < n; ++j)
      for (uint32_t l = 0; l < L; ++l) unk[j].value.at(t, l) = u[j * 8 + l];
  };
  auto residuals = [&](float* r) noexcept {
    for (uint32_t i = 0; i < n; ++i)
      for (uint32_t l = 0; l < L; ++l) r[i * 8 + l] = eq[i].lhs.at(t, l) - eq[i].rhs.at(t, l);
  };
  for (uint32_t j = 0; j < n; ++j)
    for (uint32_t l = 0; l < L; ++l) u[j * 8 + l] = unk[j].init.base ? unk[j].init.at(t, l) : unk[j].guess[l];
  writeU();
  for (uint32_t it = 0; it < s.iterations; ++it) {
    if (s.ad) {
      evalBodyTangents(s, t);
      residuals(r0);
      uint32_t bad = 0;  // lanes whose tangent Jacobian is not finite
      for (uint32_t i = 0; i < n; ++i) {
        const AdPort& lp = adPorts_[s.firstAdEq + 2 * i];
        const AdPort& rp = adPorts_[s.firstAdEq + 2 * i + 1];
        for (uint32_t j = 0; j < n; ++j) {
          const bool lo = ((lp.nz >> j) & 1u) != 0, ro = ((rp.nz >> j) & 1u) != 0;
          for (uint32_t l = 0; l < L; ++l) {
            const float tl = lo ? T[(lp.slot * n + j) * 8 + l] : 0.0f;
            const float tr = ro ? T[(rp.slot * n + j) * 8 + l] : 0.0f;
            const float x = lo && ro ? tl - tr : lo ? tl : ro ? -tr : 0.0f;
            J[(i * n + j) * 8 + l] = x;
            if (!std::isfinite(x)) bad |= 1u << l;
          }
        }
      }
      if (bad != 0) {
        finiteDifferenceJacobian(s, t, r0, Jfd);
        for (uint32_t q = 0; q < n * n; ++q)
          for (uint32_t l = 0; l < L; ++l)
            if ((bad >> l) & 1u) J[q * 8 + l] = Jfd[q * 8 + l];
      }
    } else {
      evalBody(s, t);
      residuals(r0);
      finiteDifferenceJacobian(s, t, r0, J);
    }
    // Per lane: J·Δ = r0 (Gaussian elimination with partial pivoting, in double), u −= Δ when finite.
    for (uint32_t l = 0; l < L; ++l) {
      if (n == 1) {
        const double d = static_cast<double>(r0[l]) / static_cast<double>(J[l]);
        const float nu = static_cast<float>(static_cast<double>(u[l]) - d);
        if (std::isfinite(d) && std::isfinite(nu)) u[l] = nu;
        continue;
      }
      const uint32_t w = n + 1;
      for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t j = 0; j < n; ++j) M[i * w + j] = J[(i * n + j) * 8 + l];
        M[i * w + n] = r0[i * 8 + l];
      }
      bool ok = true;
      for (uint32_t c = 0; c < n && ok; ++c) {
        uint32_t piv = c;
        for (uint32_t i = c + 1; i < n; ++i)
          if (std::fabs(M[i * w + c]) > std::fabs(M[piv * w + c])) piv = i;
        if (!(std::fabs(M[piv * w + c]) > 0.0)) {
          ok = false;
          break;
        }
        if (piv != c)
          for (uint32_t j = 0; j < w; ++j) std::swap(M[c * w + j], M[piv * w + j]);
        for (uint32_t i = c + 1; i < n; ++i) {
          const double f = M[i * w + c] / M[c * w + c];
          for (uint32_t j = c; j < w; ++j) M[i * w + j] -= f * M[c * w + j];
        }
      }
      if (!ok) continue;
      for (uint32_t i = n; i-- > 0;) {
        double v = M[i * w + n];
        for (uint32_t j = i + 1; j < n; ++j) v -= M[i * w + j] * M[j * w + n];
        M[i * w + n] = v / M[i * w + i];
      }
      for (uint32_t i = 0; i < n; ++i) {
        const double nu = static_cast<double>(u[i * 8 + l]) - M[i * w + n];
        if (!std::isfinite(nu) || !std::isfinite(static_cast<float>(nu))) ok = false;
      }
      if (!ok) continue;
      for (uint32_t i = 0; i < n; ++i) u[i * 8 + l] = static_cast<float>(static_cast<double>(u[i * 8 + l]) - M[i * w + n]);
    }
    writeU();
  }
  if (s.finalEval) evalBody(s, t);
  for (uint32_t j = 0; j < n; ++j)
    if (unk[j].guess)
      for (uint32_t l = 0; l < L; ++l) unk[j].guess[l] = u[j * 8 + l];
}

void InterpretedImage::process(ProcessContext& ctx) noexcept {  // RT-safe
  const uint32_t frames = ctx.frames;
  if (frames == 0) return;
  if (frames > io_.maxBlockFrames) {
    ++rejected_;
    zeroOutputs(ctx);
    return;
  }
  const uint32_t G = io_.maxVoiceGroups;
  run_.sends = ctx.sends;
  run_.numSends = ctx.sends ? ctx.numSends : 0;
  // Params: clamped to the declared range (Q44), NaN → default, splatted once per block.
  for (uint32_t i = 0; i < params_.size(); ++i) {
    const ParamRun& p = params_[i];
    float v = ctx.params != nullptr && i < ctx.numParams ? ctx.params[i] : p.def;
    if (v != v) v = p.def;
    if (p.isBool) {
      v = dsp::boolMask(v != 0.0f);
    } else if (p.hasRange) {
      v = v < p.lo ? p.lo : v > p.hi ? p.hi : v;
    }
    splat8(p.pattern, v);
  }
  // Engine data → pointer cells.
  const VoiceTable* vt = ctx.voices;
  const uint32_t groups = vt != nullptr && vt->groups != nullptr ? std::min(vt->numGroups, G) : 0u;
  for (uint32_t e = 0; e < exts_.size(); ++e) {
    ExtRun& x = exts_[e];
    if (!x.voice) {
      extPtr_[static_cast<size_t>(e) * G] = resolveGlobal(x, ctx, frames);
      continue;
    }
    for (uint32_t gi = 0; gi < groups; ++gi) {
      const VoiceGroupIO& gr = vt->groups[gi];
      const float* p = zeros_;
      if (x.src == Src::RootVoice) {
        if (gr.in != nullptr && x.index < gr.numIn && gr.in[x.index] != nullptr) p = gr.in[x.index];
      } else if (x.src == Src::VoiceSig) {
        if (gr.signals != nullptr && gr.signals[x.index] != nullptr) p = gr.signals[x.index];
      }
      extPtr_[static_cast<size_t>(e) * G + gi] = const_cast<float*>(p);  // read-only by contract
    }
  }
  for (uint32_t i = 0; i < numGlobalSites_; ++i) *sites_[i].target = extPtr_[static_cast<size_t>(sites_[i].ext) * G];
  for (uint32_t i = voiceSiteBegin_[0]; i < voiceSiteBegin_[groups]; ++i)
    *sites_[i].target = extPtr_[static_cast<size_t>(sites_[i].ext) * G + sites_[i].group];

  for (const StageRun& st : stages_) {
    if (!st.voice) {
      for (uint32_t r = st.firstRegion; r < st.firstRegion + st.numRegions; ++r) runRegion(regions_[r], frames);
      continue;
    }
    for (uint32_t k = st.firstReduce; k < st.firstReduce + st.numReduce; ++k) {
      const ReduceInit& ri = reduceInits_[k];
      const uint32_t count = ri.pattern ? 8u : frames;
      for (uint32_t f = 0; f < count; ++f) ri.out[f] = ri.value;
    }
    bool any = false;
    for (uint32_t gi = 0; gi < groups; ++gi) {
      const VoiceGroupIO& gr = vt->groups[gi];
      if ((gr.laneMask & 0xFFu) == 0) continue;  // no voice in this group: not run (T1.8; its state holds)
      run_.laneMask = gr.laneMask & 0xFFu;
      run_.gain = gr.gain;
      run_.lastLane = vt->lastGroup == gi && vt->lastLane < 8 && ((gr.laneMask >> vt->lastLane) & 1u) ? vt->lastLane
                                                                                                    : kNoVoice;
      any = any || run_.laneMask != 0;
      const uint32_t base = st.firstRegion + gi * st.numRegions;
      for (uint32_t r = base; r < base + st.numRegions; ++r) runRegion(regions_[r], frames);
      const uint32_t ob = st.firstOut + gi * st.numOut;
      for (uint32_t k = ob; k < ob + st.numOut; ++k) {
        const OutCopy& c = outs_[k];
        if (gr.out != nullptr && c.slot < gr.numOut && gr.out[c.slot] != nullptr) copyOut(c, gr.out[c.slot], frames, true);
      }
    }
    if (!any)
      for (uint32_t k = st.firstReduce; k < st.firstReduce + st.numReduce; ++k) {
        const ReduceInit& ri = reduceInits_[k];
        const uint32_t count = ri.pattern ? 8u : frames;
        for (uint32_t f = 0; f < count; ++f) ri.out[f] = 0.0f;
      }
  }
  run_.laneMask = 0;
  run_.gain = nullptr;
  for (uint32_t k = firstGlobalOut_; k < outs_.size(); ++k) {
    const OutCopy& c = outs_[k];
    if (ctx.out != nullptr && c.slot < ctx.numOut && ctx.out[c.slot] != nullptr) copyOut(c, ctx.out[c.slot], frames, false);
  }
}

// ---- builder ----------------------------------------------------------------------------------------------------
class Builder {
public:
  Builder(const g::Schedule& s, const InterpretOptions& o) : s_(s), o_(o) {}

  Result<std::unique_ptr<ProgramImage>> build() {
    if (o_.maxBlockFrames == 0 || o_.maxBlockFrames > (1u << 20) || o_.maxVoiceGroups == 0 || o_.maxVoiceGroups > 1024)
      return Error{ErrorCode::InvalidArgument, "interpreter: maxBlockFrames must be in [1, 2^20], maxVoiceGroups in [1, 1024]", {}};
    img_ = std::make_unique<InterpretedImage>();
    G_ = o_.maxVoiceGroups;
    img_->layout_ = s_.state;
    img_->io_.sampleRate = s_.sampleRate;
    img_->io_.maxBlockFrames = o_.maxBlockFrames;
    img_->io_.maxVoiceGroups = G_;
    img_->io_.params = s_.params;
    if (!layoutMemory()) return err_;
    if (!externals()) return err_;
    if (!stages()) return err_;
    finish();
    return std::unique_ptr<ProgramImage>(std::move(img_));
  }

private:
  bool fail(std::string msg) {
    err_ = Error{ErrorCode::Unsupported, "interpreter: " + std::move(msg), {}};
    return false;
  }

  // Scratch buffers, patterns, state arena.
  bool layoutMemory() {
    const uint32_t B = o_.maxBlockFrames;
    uint32_t off = 0;
    bufOff_.resize(s_.buffers.size());
    bufSize_.resize(s_.buffers.size());
    for (size_t i = 0; i < s_.buffers.size(); ++i) {
      const g::Buffer& b = s_.buffers[i];
      if (b.cls == g::Buffer::Class::Block && b.lanes != 1 && b.lanes != 8) return fail("buffer lanes must be 1 or 8");
      const uint32_t size = roundUp(b.cls == g::Buffer::Class::Pattern ? 8u : B * b.lanes, kFloatAlign);
      bufOff_[i] = off;
      bufSize_[i] = size;
      off += size * (b.perGroup ? G_ : 1u);
    }
    img_->bufMem_ = AlignedBuffer<float>(std::max<uint32_t>(off, kFloatAlign));
    // aux: params, consts, own external patterns / ramps (sized later), zeros.
    uint32_t aux = 0;
    paramOff_ = aux;
    aux += static_cast<uint32_t>(s_.params.size()) * 8;
    constOff_ = aux;
    aux += static_cast<uint32_t>(s_.constants.size()) * 8;
    for (const g::External& e : s_.externals) {
      extOwnOff_.push_back(aux);
      if (e.poly == g::Poly::Voice) continue;
      aux += roundUp(e.rate == g::Rate::Audio ? B : 8u, kFloatAlign);
    }
    aux = roundUp(aux, kFloatAlign);
    zeroOff_ = aux;
    aux += B * 8;
    img_->auxMem_ = AlignedBuffer<float>(aux);
    float* auxp = img_->auxMem_.data();
    img_->zeros_ = auxp + zeroOff_;
    for (size_t i = 0; i < s_.params.size(); ++i) {
      const g::ParamSlot& p = s_.params[i];
      ParamRun pr;
      pr.pattern = auxp + paramOff_ + i * 8;
      pr.isBool = p.kind == g::Kind::Bool;
      pr.def = p.defaultValue;
      pr.hasRange = p.hasRange && !pr.isBool && p.rangeMin <= p.rangeMax;
      pr.lo = p.rangeMin;
      pr.hi = p.rangeMax;
      for (uint32_t l = 0; l < 8; ++l) pr.pattern[l] = pr.isBool ? dsp::boolMask(pr.def != 0.0f) : pr.def;
      img_->params_.push_back(pr);
    }
    for (size_t i = 0; i < s_.constants.size(); ++i)
      for (uint32_t l = 0; l < 8; ++l) auxp[constOff_ + i * 8 + l] = s_.constants[i].value;
    // State: global arena, then one block per voice group.
    const g::StateLayout& L = s_.state;
    img_->globalStateBytes_ = roundUp(L.globalBytes, 64);
    img_->voiceStride_ = roundUp(L.voiceBytes, 64);
    const size_t stateBytes = static_cast<size_t>(img_->globalStateBytes_) + static_cast<size_t>(img_->voiceStride_) * G_;
    img_->stateMem_ = AlignedBuffer<float>(std::max<size_t>(stateBytes / 4, 16));
    globalState_ = reinterpret_cast<std::byte*>(img_->stateMem_.data());
    voiceState_ = globalState_ + img_->globalStateBytes_;
    for (const g::StateEntry& e : L.entries) {
      if (e.offset + e.size > (e.arena == g::Poly::Voice ? L.voiceBytes : L.globalBytes))
        return fail("state entry outside its arena");
      if (e.tag.kernel == g::StateTag::kSolveGuess) continue;  // zeros
      const auto atom = static_cast<dsp::Atom>(e.tag.kernel);
      if (e.arena == g::Poly::Global) {
        dsp::initState(atom, globalState_ + e.offset, e.init);
        continue;
      }
      for (uint32_t gi = 0; gi < G_; ++gi) {
        dsp::StateInit init = e.init;
        // Voice groups get distinct noise: the group index is mixed into the seed (lanes are mixed by dsp).
        if (atom == dsp::Atom::Noise) init.seed += gi * 0x9E3779B9u;
        dsp::initState(atom, voiceState_ + static_cast<size_t>(gi) * img_->voiceStride_ + e.offset, init);
      }
    }
    return true;
  }

  // Interface slots and engine sources.
  bool externals() {
    ImageInterface& io = img_->io_;
    // Root input slots: per poly, sorted by (ordinal, channel).
    std::vector<uint32_t> gIn, vIn;
    for (uint32_t e = 0; e < s_.externals.size(); ++e)
      if (s_.externals[e].source == g::External::Source::RootInput)
        (s_.externals[e].poly == g::Poly::Voice ? vIn : gIn).push_back(e);
    auto byOrd = [&](uint32_t a, uint32_t b) {
      const g::External &x = s_.externals[a], &y = s_.externals[b];
      return x.index != y.index ? x.index < y.index : x.channel < y.channel;
    };
    std::sort(gIn.begin(), gIn.end(), byOrd);
    std::sort(vIn.begin(), vIn.end(), byOrd);
    std::vector<uint32_t> slotOf(s_.externals.size(), 0);
    auto addSlots = [&](const std::vector<uint32_t>& list, std::vector<IoSlot>& slots) {
      for (uint32_t e : list) {
        const g::External& x = s_.externals[e];
        slotOf[e] = static_cast<uint32_t>(slots.size());
        IoSlot sl;
        sl.node = x.node;
        sl.channel = x.channel;
        sl.rate = x.rate;
        sl.ordinal = x.index;
        slots.push_back(sl);
      }
    };
    addSlots(gIn, io.globalIn);
    addSlots(vIn, io.voiceIn);
    float* auxp = img_->auxMem_.data();
    for (uint32_t e = 0; e < s_.externals.size(); ++e) {
      const g::External& x = s_.externals[e];
      ExtRun r;
      r.voice = x.poly == g::Poly::Voice;
      r.pattern = x.rate != g::Rate::Audio;
      r.own = r.voice ? nullptr : auxp + extOwnOff_[e];
      if (x.source == g::External::Source::RootInput) {
        r.src = r.voice ? Src::RootVoice : Src::RootGlobal;
        r.index = slotOf[e];
      } else {
        const std::string_view name = g::atomSignature(x.atom).name;
        static constexpr std::string_view kVoice[kVoiceSignalCount] = {
            "voice.hz", "voice.cents", "voice.gate", "voice.trigger", "voice.velocity", "voice.release",
            "voice.ratio", "voice.harmonic", "voice.unisonCount", "voice.index", "voice.degree", "voice.equave"};
        bool found = false;
        for (uint32_t k = 0; k < kVoiceSignalCount; ++k)
          if (name == kVoice[k]) {
            r.src = Src::VoiceSig;
            r.index = k;
            found = true;
          }
        if (found && !r.voice) return fail("voice source '" + std::string(name) + "' outside a voice stage");
        if (!found) {
          if (r.voice) return fail("per-voice engine source '" + std::string(name) + "' is not supported");
          r.index = x.channel;
          if (name == "transport.bpm") r.src = Src::TransportBpm;
          else if (name == "transport.beat") r.src = Src::TransportBeat;
          else if (name == "transport.barPhase") r.src = Src::TransportBarPhase;
          else if (name == "transport.playing") r.src = Src::TransportPlaying;
          else if (name == "track.in") r.src = Src::TrackIn;
          else if (name == "sidechain.in") r.src = Src::Sidechain;
          else if (name == "clipPlayer") r.src = Src::Silence;  // clip players arrive with the warp engine (§9.4)
          else return fail("unknown engine source '" + std::string(name) + "'");
        }
      }
      img_->exts_.push_back(r);
    }
    img_->extPtr_.assign(s_.externals.size() * G_, img_->zeros_);
    // Output slots.
    for (const g::OutputRef& o : s_.outputs) {
      IoSlot sl;
      sl.node = o.node;
      sl.channel = o.channel;
      sl.isExport = o.isExport;
      (o.poly == g::Poly::Voice ? io.voiceOut : io.globalOut).push_back(sl);
    }
    return true;
  }

  // ---- operands ----
  bool isPattern(const g::Operand& o) const {
    switch (o.kind) {
      case g::Operand::Kind::Buffer: return s_.buffers[o.index].cls == g::Buffer::Class::Pattern;
      case g::Operand::Kind::External: return s_.externals[o.index].rate != g::Rate::Audio;
      default: return true;
    }
  }
  uint32_t lanesOf(const g::Operand& o) const {  // floats per frame of a Blk operand
    if (o.kind == g::Operand::Kind::Buffer) return s_.buffers[o.index].lanes;
    if (o.kind == g::Operand::Kind::External) return s_.externals[o.index].poly == g::Poly::Voice ? 8u : 1u;
    return 8u;
  }
  bool validOperand(const g::Operand& o) const {
    switch (o.kind) {
      case g::Operand::Kind::Buffer: return o.index < s_.buffers.size();
      case g::Operand::Kind::Param: return o.index < s_.params.size();
      case g::Operand::Kind::Const: return o.index < s_.constants.size();
      case g::Operand::Kind::External: return o.index < s_.externals.size();
      default: return false;
    }
  }
  // Pointer for group gi (externals: a placeholder until the first block; the caller records a site).
  float* resolve(const g::Operand& o, uint32_t gi) {
    float* auxp = img_->auxMem_.data();
    switch (o.kind) {
      case g::Operand::Kind::Buffer: {
        const g::Buffer& b = s_.buffers[o.index];
        return img_->bufMem_.data() + bufOff_[o.index] + (b.perGroup ? static_cast<size_t>(gi) * bufSize_[o.index] : 0u);
      }
      case g::Operand::Kind::Param: return auxp + paramOff_ + static_cast<size_t>(o.index) * 8;
      case g::Operand::Kind::Const: return auxp + constOff_ + static_cast<size_t>(o.index) * 8;
      default: return img_->zeros_;
    }
  }

  enum class SiteKind : uint8_t { Pool, Reloc, UnkInit, EqLhs, EqRhs };
  struct PendingSite {
    SiteKind kind;
    uint32_t index;
    uint32_t ext;
    uint32_t group;
  };
  void site(SiteKind k, uint32_t index, const g::Operand& o, uint32_t gi) {
    if (o.kind != g::Operand::Kind::External) return;
    const bool voice = s_.externals[o.index].poly == g::Poly::Voice;
    pending_.push_back({k, index, o.index, voice ? gi : 0u});
  }

  // One record for schedule op i in group gi.
  bool makeRec(uint32_t i, uint32_t gi, bool loop, bool control) {
    const g::Op& op = s_.ops[i];
    InterpretedImage& im = *img_;
    Rec rec;
    rec.args.numIn = op.numIn;
    rec.args.numOut = op.numOut;
    rec.args.splMask = op.splMask;
    rec.args.frames = control ? 1u : 0u;
    rec.inBase = static_cast<uint32_t>(im.pool_.size());
    rec.firstReloc = static_cast<uint32_t>(im.relocs_.size());
    const auto ins = s_.inputs(op);
    const auto outs = s_.outputsOf(op);
    auto addOperand = [&](const g::Operand& o, bool isOut) -> bool {
      if (!validOperand(o)) return fail("op " + std::to_string(i) + " has an invalid operand");
      if (isOut && o.kind != g::Operand::Kind::Buffer) return fail("op " + std::to_string(i) + " writes a non-buffer");
      const uint32_t cell = static_cast<uint32_t>(im.pool_.size());
      float* p = resolve(o, gi);
      im.pool_.push_back(p);
      if (loop && !isPattern(o)) {
        im.relocs_.push_back({cell, lanesOf(o), p});
        site(SiteKind::Reloc, static_cast<uint32_t>(im.relocs_.size() - 1), o, gi);
      } else {
        site(SiteKind::Pool, cell, o, gi);
      }
      return true;
    };
    for (const g::Operand& o : ins)
      if (!addOperand(o, false)) return false;
    rec.outBase = static_cast<uint32_t>(im.pool_.size());
    for (const g::Operand& o : outs)
      if (!addOperand(o, true)) return false;
    rec.numReloc = static_cast<uint32_t>(im.relocs_.size()) - rec.firstReloc;
    // Rate/lane checks: what the kernel will read must be what the schedule says.
    if (!control) {
      for (uint32_t j = 0; j < ins.size(); ++j) {
        const bool spl = ((op.splMask >> j) & 1u) != 0;
        if (spl != isPattern(ins[j]))
          return fail("op " + std::to_string(i) + " input " + std::to_string(j) + ": Spl mask disagrees with the operand");
        if (!spl && op.kind == g::OpKind::Kernel && lanesOf(ins[j]) != op.lanes)
          return fail("op " + std::to_string(i) + " input " + std::to_string(j) + ": lane count disagrees");
      }
    }
    switch (op.kind) {
      case g::OpKind::Kernel: {
        rec.fn = dsp::findKernel(op.kernel, op.lanes, op.splMask);
        if (!rec.fn)
          return fail(std::string("no ") + dsp::kernelInfo(op.kernel).name + " kernel for " + std::to_string(op.lanes) +
                      " lanes with Spl mask " + std::to_string(op.splMask));
        if (op.state != g::kNone) {
          if (op.state >= s_.state.entries.size()) return fail("op " + std::to_string(i) + " has an invalid state entry");
          const g::StateEntry& e = s_.state.entries[op.state];
          rec.args.state = e.arena == g::Poly::Voice
                               ? voiceState_ + static_cast<size_t>(gi) * img_->voiceStride_ + e.offset
                               : globalState_ + e.offset;
        }
        const dsp::Resource res = dsp::kernelInfo(op.kernel).resource;
        if (res != dsp::Resource::None) rec.args.resource = resource(res, op.resource);
        break;
      }
      case g::OpKind::Broadcast:
        if (op.numIn != 1 || op.numOut != 1) return fail("broadcast with wrong arity");
        rec.fn = &broadcastKernel;
        rec.args.splMask = isPattern(ins[0]) ? 1u : 0u;
        break;
      case g::OpKind::Reduce: {
        if (op.numIn != 1 || op.numOut != 1) return fail("reducer with wrong arity");
        ReduceInfo ri;
        const std::string_view name = g::atomSignature(op.atom).name;
        ri.mode = name == "voice_sum"       ? ReduceMode::Sum
                  : name == "voice_last"    ? ReduceMode::Last
                  : name == "voice_highest" ? ReduceMode::Highest
                  : name == "voice_lowest"  ? ReduceMode::Lowest
                  : name == "voice_loudest" ? ReduceMode::Loudest
                                            : ReduceMode::Sum;
        if (!name.starts_with("voice_")) return fail("unknown reducer");
        ri.isBool = op.valueKind == g::Kind::Bool;
        ri.inPattern = isPattern(ins[0]);
        ri.outPattern = isPattern(outs[0]);
        if (!ri.inPattern && lanesOf(ins[0]) != 8) return fail("reducer input is not a voice block");
        if (!ri.outPattern && lanesOf(outs[0]) != 1) return fail("reducer output is not a global block");
        rec.fn = &reduceKernel;
        rec.info = static_cast<uint32_t>(im.reduceInfos_.size());
        rec.infoKind = 1;
        im.reduceInfos_.push_back(ri);
        if (gi == 0) {  // initialised once per block before the group loop
          ReduceInit init;
          init.out = resolve(outs[0], 0);
          init.pattern = ri.outPattern;
          const float inf = std::numeric_limits<float>::infinity();
          init.value = ri.isBool ? dsp::boolMask(ri.mode == ReduceMode::Lowest)
                       : ri.mode == ReduceMode::Highest ? -inf
                       : ri.mode == ReduceMode::Lowest  ? inf
                                                        : 0.0f;
          im.reduceInits_.push_back(init);
        }
        break;
      }
      case g::OpKind::Send: {
        if (op.numIn != 1) return fail("send with wrong arity");
        SendInfo si;
        si.bus = op.resource;
        si.channel = op.channel;
        si.lanes = op.lanes;
        si.inPattern = isPattern(ins[0]);
        rec.fn = &sendKernel;
        rec.info = static_cast<uint32_t>(im.sendInfos_.size());
        rec.infoKind = 2;
        im.sendInfos_.push_back(si);
        break;
      }
    }
    im.recs_.push_back(rec);
    return true;
  }
  const void* resource(dsp::Resource kind, uint32_t index) const {
    const Resources* r = o_.resources;
    switch (kind) {
      case dsp::Resource::Wavetable:
        if (r && index < r->wavetables.size() && r->wavetables[index]) return r->wavetables[index];
        return &defaultWavetable();
      case dsp::Resource::Shaper:
        if (r && index < r->shapers.size() && r->shapers[index]) return r->shapers[index];
        return &defaultShaper();
      case dsp::Resource::Sample:
        if (r && index < r->samples.size() && r->samples[index]) return r->samples[index];
        return &defaultSample();
      case dsp::Resource::Tuning:  // no default: the kernel passes its fallback through
        return r && index < r->tunings.size() ? r->tunings[index] : nullptr;
      default: return nullptr;
    }
  }

  OperandRef operandRef(const g::Operand& o, uint32_t gi, bool loop) {
    OperandRef r;
    r.base = resolve(o, gi);
    r.stride = loop && !isPattern(o) ? lanesOf(o) : 0u;
    return r;
  }

  bool solveRun(const g::SolveBlock& sb, const g::Region& reg, uint32_t gi, uint32_t regionRecBase) {
    InterpretedImage& im = *img_;
    if (sb.numUnknowns != sb.numEquations || sb.numUnknowns == 0)
      return fail("solve block with " + std::to_string(sb.numUnknowns) + " unknowns and " +
                  std::to_string(sb.numEquations) + " equations");
    if (sb.firstOp < reg.firstOp || sb.firstOp + sb.numOps > reg.firstOp + reg.numOps)
      return fail("solve body outside its region");
    if (reg.kind == g::RegionKind::Loop && reg.subBlock > 1) return fail("solve block in a sub-blocked loop");
    const bool loop = reg.kind == g::RegionKind::Loop;
    SolveRun r;
    r.iterations = std::max<uint32_t>(sb.iterations, 1);
    r.lanes = reg.kind == g::RegionKind::Control || reg.poly == g::Poly::Voice ? 8u : 1u;
    r.n = sb.numUnknowns;
    r.firstBodyRec = regionRecBase + (sb.firstOp - reg.firstOp);
    r.numBodyRec = sb.numOps;
    r.firstUnk = static_cast<uint32_t>(im.unks_.size());
    r.firstEq = static_cast<uint32_t>(im.eqs_.size());
    r.loop = loop;
    for (uint32_t k = 0; k < sb.numUnknowns; ++k) {
      const g::SolveUnknown& su = s_.unknowns[sb.firstUnknown + k];
      if (su.value.kind != g::Operand::Kind::Buffer || !validOperand(su.value)) return fail("solve unknown without a buffer");
      UnkRun u;
      u.value = operandRef(su.value, gi, loop);
      if (su.init.kind != g::Operand::Kind::None) {
        if (!validOperand(su.init)) return fail("invalid initial guess");
        u.init = operandRef(su.init, gi, loop);
        site(SiteKind::UnkInit, static_cast<uint32_t>(im.unks_.size()), su.init, gi);
      }
      if (su.state != g::kNone) {
        if (su.state >= s_.state.entries.size()) return fail("invalid solve state");
        const g::StateEntry& e = s_.state.entries[su.state];
        u.guess = reinterpret_cast<float*>(e.arena == g::Poly::Voice
                                               ? voiceState_ + static_cast<size_t>(gi) * img_->voiceStride_ + e.offset
                                               : globalState_ + e.offset);
      } else if (su.init.kind == g::Operand::Kind::None) {
        return fail("solve unknown with neither an initial guess nor a state entry");
      }
      im.unks_.push_back(u);
    }
    for (uint32_t k = 0; k < sb.numEquations; ++k) {
      const g::SolveEquation& se = s_.equations[sb.firstEquation + k];
      if (!validOperand(se.lhs) || !validOperand(se.rhs)) return fail("invalid equation operand");
      EqRun e;
      e.lhs = operandRef(se.lhs, gi, loop);
      e.rhs = operandRef(se.rhs, gi, loop);
      site(SiteKind::EqLhs, static_cast<uint32_t>(im.eqs_.size()), se.lhs, gi);
      site(SiteKind::EqRhs, static_cast<uint32_t>(im.eqs_.size()), se.rhs, gi);
      im.eqs_.push_back(e);
    }
    adPlan(sb, r);
    r.finalEval = newton::bodyReadAfter(s_, sb);
    maxN_ = std::max(maxN_, r.n);
    im.solves_.push_back(r);
    return true;
  }

  // The tangent records of a solve block (newton::planTangents): one tangent slot per distinct buffer.
  void adPlan(const g::SolveBlock& sb, SolveRun& r) {
    InterpretedImage& im = *img_;
    const newton::TangentPlan plan = newton::planTangents(s_, sb);
    r.ad = plan.ad;
    if (!r.ad) return;
    std::unordered_map<uint32_t, uint32_t> slotOf;
    auto slot = [&](const g::Operand& o) -> uint32_t {
      if (o.kind != g::Operand::Kind::Buffer) return 0;
      return slotOf.emplace(o.index, static_cast<uint32_t>(slotOf.size())).first->second;
    };
    r.firstAdUnk = static_cast<uint32_t>(im.adPorts_.size());
    for (uint32_t j = 0; j < sb.numUnknowns; ++j) im.adPorts_.push_back({slot(s_.unknowns[sb.firstUnknown + j].value), 1u << j});
    r.firstAdOp = static_cast<uint32_t>(im.adOps_.size());
    for (uint32_t k = 0; k < sb.numOps; ++k) {
      const g::Op& op = s_.ops[sb.firstOp + k];
      AdOp a;
      a.step = newton::scalarTangentStep(static_cast<dsp::Atom>(op.kernel));
      a.numIn = op.numIn;
      a.numOut = op.numOut;
      a.firstIn = static_cast<uint32_t>(im.adPorts_.size());
      const auto ins = s_.inputs(op);
      for (uint32_t i = 0; i < op.numIn; ++i) {
        const uint32_t nz = plan.inNz[plan.firstIn[k] + i];
        im.adPorts_.push_back({nz != 0 ? slot(ins[i]) : 0u, nz});
      }
      a.firstOut = static_cast<uint32_t>(im.adPorts_.size());
      const auto outs = s_.outputsOf(op);
      for (uint32_t o = 0; o < op.numOut; ++o) {
        const uint32_t nz = plan.outNz[plan.firstOut[k] + o];
        im.adPorts_.push_back({nz != 0 ? slot(outs[o]) : 0u, nz});
        a.outAny |= nz;
      }
      maxAdIn_ = std::max(maxAdIn_, a.numIn);
      maxAdOut_ = std::max(maxAdOut_, a.numOut);
      im.adOps_.push_back(a);
    }
    r.firstAdEq = static_cast<uint32_t>(im.adPorts_.size());
    for (uint32_t q = 0; q < sb.numEquations; ++q) {
      const g::SolveEquation& e = s_.equations[sb.firstEquation + q];
      im.adPorts_.push_back({plan.lhsNz[q] != 0 ? slot(e.lhs) : 0u, plan.lhsNz[q]});
      im.adPorts_.push_back({plan.rhsNz[q] != 0 ? slot(e.rhs) : 0u, plan.rhsNz[q]});
    }
    maxTanFloats_ = std::max<size_t>(maxTanFloats_, static_cast<size_t>(slotOf.size()) * sb.numUnknowns * 8);
  }

  bool stages() {
    InterpretedImage& im = *img_;
    for (const g::Stage& st : s_.stages) {
      StageRun sr;
      sr.voice = st.poly == g::Poly::Voice;
      if (sr.voice) im.io_.hasVoiceStages = true;
      const uint32_t groups = sr.voice ? G_ : 1u;
      sr.firstRegion = static_cast<uint32_t>(im.regions_.size());
      sr.numRegions = st.numRegions;
      sr.firstReduce = static_cast<uint32_t>(im.reduceInits_.size());
      for (uint32_t gi = 0; gi < groups; ++gi) {
        for (uint32_t ri = st.firstRegion; ri < st.firstRegion + st.numRegions; ++ri) {
          const g::Region& reg = s_.regions[ri];
          const bool loop = reg.kind == g::RegionKind::Loop;
          const bool control = reg.kind == g::RegionKind::Control;
          RegionRun rr;
          rr.kind = reg.kind;
          rr.subBlock = reg.subBlock;
          rr.firstRec = static_cast<uint32_t>(im.recs_.size());
          for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i) {
            const g::Op& op = s_.ops[i];
            if (sr.voice && op.kind == g::OpKind::Kernel && op.lanes != 8)
              return fail("a 1-lane op in a voice stage");
            if (!makeRec(i, gi, loop, control)) return false;
          }
          rr.numRec = static_cast<uint32_t>(im.recs_.size()) - rr.firstRec;
          if (reg.numSolves > 0) {
            rr.firstItem = static_cast<uint32_t>(im.items_.size());
            uint32_t sIdx = reg.firstSolve;
            const uint32_t sEnd = reg.firstSolve + reg.numSolves;
            uint32_t i = reg.firstOp;
            const uint32_t end = reg.firstOp + reg.numOps;
            while (i < end || sIdx < sEnd) {
              if (sIdx < sEnd && s_.solves[sIdx].firstOp <= i) {
                const g::SolveBlock& sb = s_.solves[sIdx];
                im.items_.push_back(kSolveItem | static_cast<uint32_t>(im.solves_.size()));
                if (!solveRun(sb, reg, gi, rr.firstRec)) return false;
                i = std::max(i, sb.firstOp + sb.numOps);
                ++sIdx;
                continue;
              }
              if (i >= end) return fail("solve block after the end of its region");
              im.items_.push_back(rr.firstRec + (i - reg.firstOp));
              ++i;
            }
            rr.numItems = static_cast<uint32_t>(im.items_.size()) - rr.firstItem;
          }
          im.regions_.push_back(rr);
        }
      }
      sr.numReduce = static_cast<uint32_t>(im.reduceInits_.size()) - sr.firstReduce;
      // Voice outputs read at the end of this stage, per group.
      sr.firstOut = static_cast<uint32_t>(im.outs_.size());
      if (sr.voice) {
        for (uint32_t gi = 0; gi < G_; ++gi) {
          uint32_t count = 0;
          uint32_t slot = 0;
          for (const g::OutputRef& o : s_.outputs) {
            if (o.poly != g::Poly::Voice) continue;
            if (o.stage == st.index) {
              if (!outCopy(o, slot, gi)) return false;
              ++count;
            }
            ++slot;
          }
          sr.numOut = count;
        }
      }
      im.stages_.push_back(sr);
    }
    // Global outputs, after the last stage.
    im.firstGlobalOut_ = static_cast<uint32_t>(im.outs_.size());
    uint32_t slot = 0;
    for (const g::OutputRef& o : s_.outputs) {
      if (o.poly == g::Poly::Voice) continue;
      if (!outCopy(o, slot, 0)) return false;
      ++slot;
    }
    // Voice outputs whose stage has no StageRun cannot happen (the scheduler adds an empty voice stage for them).
    for (const g::OutputRef& o : s_.outputs) {
      if (o.poly != g::Poly::Voice) continue;
      bool found = false;
      for (const g::Stage& st : s_.stages) found = found || (st.index == o.stage && st.poly == g::Poly::Voice);
      if (!found) return fail("voice output read at a stage that does not exist");
    }
    return true;
  }

  bool outCopy(const g::OutputRef& o, uint32_t slot, uint32_t gi) {
    if (!validOperand(o.value)) return fail("output with an invalid operand");
    OutCopy c;
    c.slot = slot;
    c.group = gi;
    if (o.value.kind == g::Operand::Kind::External) {
      c.ext = o.value.index;
      if (s_.externals[o.value.index].poly == g::Poly::Global) c.group = 0;
    } else {
      c.src = resolve(o.value, gi);
    }
    c.srcLanes = isPattern(o.value) ? 0u : lanesOf(o.value);
    if (o.poly == g::Poly::Global && c.srcLanes == 8) return fail("global output reads a voice value");
    img_->outs_.push_back(c);
    return true;
  }

  // Pointers into the final vectors.
  void finish() {
    InterpretedImage& im = *img_;
    for (Rec& r : im.recs_) {
      r.args.in = const_cast<const float* const*>(im.pool_.data() + r.inBase);
      r.args.out = im.pool_.data() + r.outBase;
      if (r.infoKind == 1) {
        im.reduceInfos_[r.info].run = &im.run_;
        r.args.state = &im.reduceInfos_[r.info];
      } else if (r.infoKind == 2) {
        im.sendInfos_[r.info].run = &im.run_;
        r.args.state = &im.sendInfos_[r.info];
      }
    }
    std::stable_sort(pending_.begin(), pending_.end(), [&](const PendingSite& a, const PendingSite& b) {
      const bool va = s_.externals[a.ext].poly == g::Poly::Voice, vb = s_.externals[b.ext].poly == g::Poly::Voice;
      if (va != vb) return !va;
      return a.group < b.group;
    });
    for (const PendingSite& p : pending_) {
      Site st;
      st.ext = p.ext;
      st.group = p.group;
      switch (p.kind) {
        case SiteKind::Pool: st.target = &im.pool_[p.index]; break;
        case SiteKind::Reloc: st.target = &im.relocs_[p.index].base; break;
        case SiteKind::UnkInit: st.target = &im.unks_[p.index].init.base; break;
        case SiteKind::EqLhs: st.target = &im.eqs_[p.index].lhs.base; break;
        case SiteKind::EqRhs: st.target = &im.eqs_[p.index].rhs.base; break;
      }
      im.sites_.push_back(st);
    }
    im.numGlobalSites_ = 0;
    while (im.numGlobalSites_ < im.sites_.size() && s_.externals[im.sites_[im.numGlobalSites_].ext].poly != g::Poly::Voice)
      ++im.numGlobalSites_;
    im.voiceSiteBegin_.assign(G_ + 1, static_cast<uint32_t>(im.sites_.size()));
    for (uint32_t gi = 0; gi <= G_; ++gi) {
      uint32_t k = im.numGlobalSites_;
      while (k < im.sites_.size() && im.sites_[k].group < gi) ++k;
      im.voiceSiteBegin_[gi] = k;
    }
    // Every cell starts at silence until the first block resolves it.
    for (const Site& st : im.sites_) *st.target = im.zeros_;
    im.maxSolveN_ = maxN_;
    if (maxN_ > 0) {  // runSolve's layout: u, r0, rp, rm, up + um, J, Jfd, M (doubles)
      const size_t floats = static_cast<size_t>(maxN_) * 8 * 4 + 16 + 2 * static_cast<size_t>(maxN_) * maxN_ * 8 +
                            2 * static_cast<size_t>(maxN_) * (maxN_ + 1) + 16;
      im.solveMem_ = AlignedBuffer<float>(floats);
    }
    if (maxTanFloats_ > 0) im.tanMem_ = AlignedBuffer<float>(maxTanFloats_);
    im.adIn_.assign(std::max<uint32_t>(maxAdIn_, 1), {});
    im.adOut_.assign(std::max<uint32_t>(maxAdOut_, 1), {});
  }

  const g::Schedule& s_;
  const InterpretOptions& o_;
  std::unique_ptr<InterpretedImage> img_;
  Error err_;
  uint32_t G_ = 1;
  std::vector<uint32_t> bufOff_, bufSize_, extOwnOff_;
  uint32_t paramOff_ = 0, constOff_ = 0, zeroOff_ = 0;
  std::byte* globalState_ = nullptr;
  std::byte* voiceState_ = nullptr;
  std::vector<PendingSite> pending_;
  uint32_t maxN_ = 0;
  uint32_t maxAdIn_ = 0, maxAdOut_ = 0;
  size_t maxTanFloats_ = 0;
};

}  // namespace

Result<std::unique_ptr<ProgramImage>> buildInterpreted(const graph::Schedule& schedule, const InterpretOptions& options) {
  return Builder(schedule, options).build();
}

}  // namespace pq::engine
