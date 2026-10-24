// Host half of a JIT-compiled program (jit_image.h). process() is the only RT code here; buildHost() mirrors the
// interpreter's builder (src/engine/src/interpreter.cpp) for everything outside the stage functions, so both
// backends agree on slots, memory, initial state and engine data.

#include "jit_image.h"

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "pq/dsp/atoms.h"
#include "pq/dsp/tables.h"
#include "pq/graph/atoms.h"

namespace pq::jit::detail {

namespace g = pq::graph;
namespace e = pq::engine;

namespace {

constexpr uint32_t kFloatAlign = 16;  // 64 bytes

uint32_t roundUp(uint32_t n, uint32_t a) noexcept { return (n + a - 1) / a * a; }

inline void splat8(float* p, float v) noexcept { _mm256_store_ps(p, _mm256_set1_ps(v)); }  // RT-safe

// Built-in default tables (the interpreter's): a 2048-point sine wavetable, the identity shaper on [−1, 1], a
// silent one-frame sample.
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

}  // namespace

bool isPatternOperand(const g::Schedule& s, const g::Operand& o) noexcept {
  switch (o.kind) {
    case g::Operand::Kind::Buffer: return s.buffers[o.index].cls == g::Buffer::Class::Pattern;
    case g::Operand::Kind::External: return s.externals[o.index].rate != g::Rate::Audio;
    default: return true;
  }
}

uint32_t lanesOfOperand(const g::Schedule& s, const g::Operand& o) noexcept {
  if (o.kind == g::Operand::Kind::Buffer) return s.buffers[o.index].lanes;
  if (o.kind == g::Operand::Kind::External) return s.externals[o.index].poly == g::Poly::Voice ? 8u : 1u;
  return 8u;
}

// ---- process ----------------------------------------------------------------------------------------------------

JitImage::~JitImage() = default;

float* JitImage::resolveGlobal(ExtRun& x, const e::ProcessContext& ctx, uint32_t frames) noexcept {  // RT-safe
  const e::TransportSnapshot& tr = ctx.transport;
  auto channelOf = [&](const float* const* chans, uint32_t count) noexcept -> const float* {
    return chans != nullptr && x.index < count && chans[x.index] != nullptr ? chans[x.index] : zeros_;
  };
  auto rampValue = [&](uint32_t f) noexcept -> float {
    const double beat = tr.beat + static_cast<double>(f) * tr.beatsPerFrame;
    if (x.src == Src::TransportBeat) return static_cast<float>(beat);
    const double bars = (beat - tr.barStartBeat) / (tr.beatsPerBar > 0.0 ? tr.beatsPerBar : 4.0);
    const float ph = static_cast<float>(bars - std::floor(bars));
    return ph < 1.0f ? ph : 0.99999994f;
  };
  const float* src = zeros_;
  switch (x.src) {
    case Src::RootGlobal: src = channelOf(ctx.in, ctx.numIn); break;
    case Src::TrackIn: src = channelOf(ctx.trackIn, ctx.numTrackIn); break;
    case Src::Sidechain: src = channelOf(ctx.sidechainIn, ctx.numSidechainIn); break;
    case Src::TransportBpm:
      if (x.pattern) {
        splat8(x.own, static_cast<float>(tr.bpm));
        return x.own;
      }
      for (uint32_t f = 0; f < frames; ++f) x.own[f] = static_cast<float>(tr.bpm);
      return x.own;
    case Src::TransportPlaying:
      if (x.pattern) {
        splat8(x.own, dsp::boolMask(tr.playing));
        return x.own;
      }
      for (uint32_t f = 0; f < frames; ++f) x.own[f] = dsp::boolMask(tr.playing);
      return x.own;
    case Src::TransportBeat:
    case Src::TransportBarPhase:
      if (x.pattern) {
        splat8(x.own, rampValue(0));
        return x.own;
      }
      for (uint32_t f = 0; f < frames; ++f) x.own[f] = rampValue(f);
      return x.own;
    default: return zeros_;
  }
  if (!x.pattern) return const_cast<float*>(src);  // read-only by contract: generated code never writes inputs
  splat8(x.own, src[0]);
  return x.own;
}

void JitImage::copyOut(const OutCopy& c, float* dst, uint32_t frames, bool voice) noexcept {  // RT-safe
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

void JitImage::zeroOutputs(const e::ProcessContext& ctx) noexcept {  // RT-safe
  for (uint32_t i = 0; i < ctx.numOut && ctx.out; ++i)
    if (ctx.out[i]) std::memset(ctx.out[i], 0, sizeof(float) * ctx.frames);
  if (ctx.voices && ctx.voices->groups)
    for (uint32_t gi = 0; gi < ctx.voices->numGroups; ++gi) {
      const e::VoiceGroupIO& gr = ctx.voices->groups[gi];
      for (uint32_t i = 0; i < gr.numOut && gr.out; ++i)
        if (gr.out[i]) std::memset(gr.out[i], 0, sizeof(float) * ctx.frames * 8);
    }
}

void JitImage::process(e::ProcessContext& ctx) noexcept {  // RT-safe
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
  // Engine data → the pointer table the stage functions read.
  const e::VoiceTable* vt = ctx.voices;
  const uint32_t groups = vt != nullptr && vt->groups != nullptr ? std::min(vt->numGroups, G) : 0u;
  for (uint32_t x = 0; x < exts_.size(); ++x) {
    ExtRun& er = exts_[x];
    if (!er.voice) {
      extPtr_[static_cast<size_t>(x) * G] = resolveGlobal(er, ctx, frames);
      continue;
    }
    for (uint32_t gi = 0; gi < groups; ++gi) {
      const e::VoiceGroupIO& gr = vt->groups[gi];
      const float* p = zeros_;
      if (er.src == Src::RootVoice) {
        if (gr.in != nullptr && er.index < gr.numIn && gr.in[er.index] != nullptr) p = gr.in[er.index];
      } else if (er.src == Src::VoiceSig) {
        if (gr.signals != nullptr && gr.signals[er.index] != nullptr) p = gr.signals[er.index];
      }
      extPtr_[static_cast<size_t>(x) * G + gi] = const_cast<float*>(p);  // read-only by contract
    }
  }
  float* buf = buf_.data();
  const float* aux = aux_.data();
  std::byte* gstate = reinterpret_cast<std::byte*>(stateMem_.data());
  std::byte* vstate = gstate + layout_.globalStateBytes;
  float* const* ext = extPtr_.data();
  const void* const* tab = tab_.data();

  for (const StageRun& st : stages_) {
    if (!st.voice) {
      run_.laneMask = 0;
      run_.gain = nullptr;
      run_.lastLane = e::kNoVoice;
      st.fn(buf, aux, gstate, vstate, ext, tab, frames, 0);
      continue;
    }
    for (uint32_t k = st.firstReduce; k < st.firstReduce + st.numReduce; ++k) {
      const ReduceInit& ri = reduceInits_[k];
      const uint32_t count = ri.pattern ? 8u : frames;
      for (uint32_t f = 0; f < count; ++f) ri.out[f] = ri.value;
    }
    bool any = false;
    for (uint32_t gi = 0; gi < groups; ++gi) {
      const e::VoiceGroupIO& gr = vt->groups[gi];
      if ((gr.laneMask & 0xFFu) == 0) continue;  // no voice in this group: not run (T1.8; its state holds)
      run_.laneMask = gr.laneMask & 0xFFu;
      run_.gain = gr.gain;
      run_.lastLane = vt->lastGroup == gi && vt->lastLane < 8 && ((gr.laneMask >> vt->lastLane) & 1u) ? vt->lastLane
                                                                                                     : e::kNoVoice;
      any = any || run_.laneMask != 0;
      st.fn(buf, aux, gstate, vstate + static_cast<size_t>(gi) * layout_.voiceStride, ext, tab, frames, gi);
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

namespace {

class HostBuilder {
public:
  HostBuilder(const g::Schedule& s, uint32_t maxBlock, uint32_t maxGroups, const e::Resources* res, PtrTable& tab)
      : s_(s), B_(maxBlock), G_(maxGroups), res_(res), tab_(tab) {}

  Result<std::unique_ptr<JitImage>> build() {
    if (B_ == 0 || B_ > (1u << 20) || G_ == 0 || G_ > 1024)
      return Error{ErrorCode::InvalidArgument, "jit: maxBlockFrames must be in [1, 2^20], maxVoiceGroups in [1, 1024]", {}};
    img_ = std::make_unique<JitImage>();
    JitImage& im = *img_;
    im.layout_.maxBlock = B_;
    im.layout_.groups = G_;
    im.stateLayout_ = s_.state;
    im.io_.sampleRate = s_.sampleRate;
    im.io_.maxBlockFrames = B_;
    im.io_.maxVoiceGroups = G_;
    im.io_.params = s_.params;
    if (!validate()) return err_;
    if (!layoutMemory()) return err_;
    if (!externals()) return err_;
    if (!ops()) return err_;
    if (!stages()) return err_;
    return std::move(img_);
  }

private:
  bool fail(std::string msg) {
    err_ = Error{ErrorCode::Unsupported, "jit: " + std::move(msg), {}};
    return false;
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

  // The interpreter's per-record checks, once per op (the generated code relies on them).
  bool validate() {
    for (const g::Buffer& b : s_.buffers)
      if (b.cls == g::Buffer::Class::Block && b.lanes != 1 && b.lanes != 8) return fail("buffer lanes must be 1 or 8");
    for (const g::Stage& st : s_.stages) {
      if (st.firstRegion + st.numRegions > s_.regions.size()) return fail("stage regions out of range");
      for (uint32_t ri = st.firstRegion; ri < st.firstRegion + st.numRegions; ++ri) {
        const g::Region& reg = s_.regions[ri];
        if (reg.firstOp + reg.numOps > s_.ops.size()) return fail("region ops out of range");
        const bool control = reg.kind == g::RegionKind::Control;
        for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i) {
          const g::Op& op = s_.ops[i];
          if (op.firstOperand + op.numIn + op.numOut > s_.operands.size()) return fail("op operands out of range");
          if (st.poly == g::Poly::Voice && op.kind == g::OpKind::Kernel && op.lanes != 8)
            return fail("a 1-lane op in a voice stage");
          if (op.kind == g::OpKind::Kernel && op.lanes != 1 && op.lanes != 8) return fail("op lanes must be 1 or 8");
          const auto ins = s_.inputs(op);
          const auto outs = s_.outputsOf(op);
          for (const g::Operand& o : ins)
            if (!validOperand(o)) return fail("op " + std::to_string(i) + " has an invalid operand");
          for (const g::Operand& o : outs) {
            if (!validOperand(o)) return fail("op " + std::to_string(i) + " has an invalid operand");
            if (o.kind != g::Operand::Kind::Buffer) return fail("op " + std::to_string(i) + " writes a non-buffer");
          }
          if (!control) {
            for (uint32_t j = 0; j < ins.size(); ++j) {
              const bool spl = ((op.splMask >> j) & 1u) != 0;
              if (spl != isPatternOperand(s_, ins[j]))
                return fail("op " + std::to_string(i) + " input " + std::to_string(j) + ": Spl mask disagrees with the operand");
              if (!spl && op.kind == g::OpKind::Kernel && lanesOfOperand(s_, ins[j]) != op.lanes)
                return fail("op " + std::to_string(i) + " input " + std::to_string(j) + ": lane count disagrees");
            }
            for (const g::Operand& o : outs)
              if (op.kind == g::OpKind::Kernel && (isPatternOperand(s_, o) || lanesOfOperand(s_, o) != op.lanes))
                return fail("op " + std::to_string(i) + ": output buffer disagrees with the op's lanes");
          } else {
            for (const g::Operand& o : outs)
              if (!isPatternOperand(s_, o)) return fail("op " + std::to_string(i) + ": control op writes a block buffer");
            for (const g::Operand& o : ins)
              if (!isPatternOperand(s_, o)) return fail("op " + std::to_string(i) + ": control op reads a block buffer");
          }
          switch (op.kind) {
            case g::OpKind::Kernel: {
              if (static_cast<uint32_t>(op.kernel) >= dsp::kAtomCount) return fail("op " + std::to_string(i) + " has no kernel");
              const uint32_t lanes = control ? 8u : op.lanes;
              if (!dsp::findKernel(op.kernel, lanes, op.splMask))
                return fail(std::string("no ") + dsp::kernelInfo(op.kernel).name + " kernel for " + std::to_string(lanes) +
                            " lanes with Spl mask " + std::to_string(op.splMask));
              const dsp::KernelInfo& ki = dsp::kernelInfo(op.kernel);
              if (!ki.variadicIn && op.numIn != ki.numIn) return fail("op " + std::to_string(i) + ": wrong input count");
              if (!ki.variadicOut && op.numOut != ki.numOut) return fail("op " + std::to_string(i) + ": wrong output count");
              if (ki.variadicIn && op.numIn < 2) return fail("op " + std::to_string(i) + ": mux without data inputs");
              if (ki.variadicOut && op.numOut < 1) return fail("op " + std::to_string(i) + ": demux without outputs");
              if (ki.stateSize > 0 && op.state == g::kNone) return fail("op " + std::to_string(i) + " has no state entry");
              if (op.state != g::kNone && op.state >= s_.state.entries.size())
                return fail("op " + std::to_string(i) + " has an invalid state entry");
              break;
            }
            case g::OpKind::Broadcast:
              if (op.numIn != 1 || op.numOut != 1) return fail("broadcast with wrong arity");
              break;
            case g::OpKind::Reduce: {
              if (op.numIn != 1 || op.numOut != 1) return fail("reducer with wrong arity");
              if (!g::atomSignature(op.atom).name.starts_with("voice_")) return fail("unknown reducer");
              const bool inPat = isPatternOperand(s_, ins[0]), outPat = isPatternOperand(s_, outs[0]);
              if (!inPat && lanesOfOperand(s_, ins[0]) != 8) return fail("reducer input is not a voice block");
              if (!outPat && lanesOfOperand(s_, outs[0]) != 1) return fail("reducer output is not a global block");
              break;
            }
            case g::OpKind::Send:
              if (op.numIn != 1) return fail("send with wrong arity");
              break;
          }
        }
        for (uint32_t k = reg.firstSolve; k < reg.firstSolve + reg.numSolves; ++k) {
          if (k >= s_.solves.size()) return fail("solve index out of range");
          const g::SolveBlock& sb = s_.solves[k];
          if (sb.numUnknowns != sb.numEquations || sb.numUnknowns == 0)
            return fail("solve block with " + std::to_string(sb.numUnknowns) + " unknowns and " +
                        std::to_string(sb.numEquations) + " equations");
          if (sb.firstOp < reg.firstOp || sb.firstOp + sb.numOps > reg.firstOp + reg.numOps)
            return fail("solve body outside its region");
          if (reg.kind == g::RegionKind::Block) return fail("solve block in a block region");
          if (reg.kind == g::RegionKind::Loop && reg.subBlock > 1) return fail("solve block in a sub-blocked loop");
          if (sb.firstUnknown + sb.numUnknowns > s_.unknowns.size() || sb.firstEquation + sb.numEquations > s_.equations.size())
            return fail("solve block out of range");
          for (uint32_t u = sb.firstUnknown; u < sb.firstUnknown + sb.numUnknowns; ++u) {
            const g::SolveUnknown& su = s_.unknowns[u];
            if (su.value.kind != g::Operand::Kind::Buffer || !validOperand(su.value)) return fail("solve unknown without a buffer");
            if (su.init.kind != g::Operand::Kind::None && !validOperand(su.init)) return fail("invalid initial guess");
            if (su.state != g::kNone && su.state >= s_.state.entries.size()) return fail("invalid solve state");
            if (su.init.kind == g::Operand::Kind::None && su.state == g::kNone)
              return fail("solve unknown with neither an initial guess nor a state entry");
          }
          for (uint32_t q = sb.firstEquation; q < sb.firstEquation + sb.numEquations; ++q)
            if (!validOperand(s_.equations[q].lhs) || !validOperand(s_.equations[q].rhs)) return fail("invalid equation operand");
          maxN_ = std::max(maxN_, sb.numUnknowns);
        }
      }
    }
    for (const g::OutputRef& o : s_.outputs)
      if (!validOperand(o.value)) return fail("output with an invalid operand");
    return true;
  }

  bool layoutMemory() {
    JitImage& im = *img_;
    HostLayout& L = im.layout_;
    uint32_t off = 0;
    L.bufOff.resize(s_.buffers.size());
    L.bufSize.resize(s_.buffers.size());
    for (size_t i = 0; i < s_.buffers.size(); ++i) {
      const g::Buffer& b = s_.buffers[i];
      const uint32_t size = roundUp(b.cls == g::Buffer::Class::Pattern ? 8u : B_ * b.lanes, kFloatAlign);
      L.bufOff[i] = off;
      L.bufSize[i] = size;
      off += size * (b.perGroup ? G_ : 1u);
    }
    L.maxSolveN = maxN_;
    L.solveOff = off;
    if (maxN_ > 0) off += roundUp(maxN_ * 8 * 2 + maxN_ * maxN_ * 8 + 2 * maxN_ * (maxN_ + 1) + 16, kFloatAlign);
    L.bufFloats = std::max<uint32_t>(off, kFloatAlign);
    im.buf_ = AlignedBuffer<float>(L.bufFloats);
    uint32_t aux = 0;
    L.paramOff = aux;
    aux += static_cast<uint32_t>(s_.params.size()) * 8;
    L.constOff = aux;
    aux += static_cast<uint32_t>(s_.constants.size()) * 8;
    for (const g::External& x : s_.externals) {
      L.extOwnOff.push_back(aux);
      if (x.poly == g::Poly::Voice) continue;
      aux += roundUp(x.rate == g::Rate::Audio ? B_ : 8u, kFloatAlign);
    }
    aux = roundUp(aux, kFloatAlign);
    L.zeroOff = aux;
    aux += B_ * 8;
    L.auxFloats = aux;
    im.aux_ = AlignedBuffer<float>(aux);
    float* auxp = im.aux_.data();
    im.zeros_ = auxp + L.zeroOff;
    for (size_t i = 0; i < s_.params.size(); ++i) {
      const g::ParamSlot& p = s_.params[i];
      ParamRun pr;
      pr.pattern = auxp + L.paramOff + i * 8;
      pr.isBool = p.kind == g::Kind::Bool;
      pr.def = p.defaultValue;
      pr.hasRange = p.hasRange && !pr.isBool && p.rangeMin <= p.rangeMax;
      pr.lo = p.rangeMin;
      pr.hi = p.rangeMax;
      for (uint32_t l = 0; l < 8; ++l) pr.pattern[l] = pr.isBool ? dsp::boolMask(pr.def != 0.0f) : pr.def;
      im.params_.push_back(pr);
    }
    for (size_t i = 0; i < s_.constants.size(); ++i)
      for (uint32_t l = 0; l < 8; ++l) auxp[L.constOff + i * 8 + l] = s_.constants[i].value;
    // State: global arena, then one block per voice group.
    const g::StateLayout& SL = s_.state;
    L.globalStateBytes = roundUp(SL.globalBytes, 64);
    L.voiceStride = roundUp(SL.voiceBytes, 64);
    const size_t stateBytes = static_cast<size_t>(L.globalStateBytes) + static_cast<size_t>(L.voiceStride) * G_;
    im.stateMem_ = AlignedBuffer<float>(std::max<size_t>(stateBytes / 4, 16));
    std::byte* globalState = reinterpret_cast<std::byte*>(im.stateMem_.data());
    std::byte* voiceState = globalState + L.globalStateBytes;
    for (const g::StateEntry& se : SL.entries) {
      if (se.offset + se.size > (se.arena == g::Poly::Voice ? SL.voiceBytes : SL.globalBytes))
        return fail("state entry outside its arena");
      if (se.tag.kernel == g::StateTag::kSolveGuess) continue;  // zeros
      const auto atom = static_cast<dsp::Atom>(se.tag.kernel);
      if (se.arena == g::Poly::Global) {
        dsp::initState(atom, globalState + se.offset, se.init);
        continue;
      }
      for (uint32_t gi = 0; gi < G_; ++gi) {
        dsp::StateInit init = se.init;
        // Voice groups get distinct noise: the group index is mixed into the seed (as the interpreter does).
        if (atom == dsp::Atom::Noise) init.seed += gi * 0x9E3779B9u;
        dsp::initState(atom, voiceState + static_cast<size_t>(gi) * L.voiceStride + se.offset, init);
      }
    }
    return true;
  }

  bool externals() {
    JitImage& im = *img_;
    e::ImageInterface& io = im.io_;
    std::vector<uint32_t> gIn, vIn;
    for (uint32_t x = 0; x < s_.externals.size(); ++x)
      if (s_.externals[x].source == g::External::Source::RootInput)
        (s_.externals[x].poly == g::Poly::Voice ? vIn : gIn).push_back(x);
    auto byOrd = [&](uint32_t a, uint32_t b) {
      const g::External &x = s_.externals[a], &y = s_.externals[b];
      return x.index != y.index ? x.index < y.index : x.channel < y.channel;
    };
    std::sort(gIn.begin(), gIn.end(), byOrd);
    std::sort(vIn.begin(), vIn.end(), byOrd);
    std::vector<uint32_t> slotOf(s_.externals.size(), 0);
    auto addSlots = [&](const std::vector<uint32_t>& list, std::vector<e::IoSlot>& slots) {
      for (uint32_t x : list) {
        const g::External& ex = s_.externals[x];
        slotOf[x] = static_cast<uint32_t>(slots.size());
        e::IoSlot sl;
        sl.node = ex.node;
        sl.channel = ex.channel;
        sl.rate = ex.rate;
        sl.ordinal = ex.index;
        slots.push_back(sl);
      }
    };
    addSlots(gIn, io.globalIn);
    addSlots(vIn, io.voiceIn);
    float* auxp = im.aux_.data();
    for (uint32_t x = 0; x < s_.externals.size(); ++x) {
      const g::External& ex = s_.externals[x];
      ExtRun r;
      r.voice = ex.poly == g::Poly::Voice;
      r.pattern = ex.rate != g::Rate::Audio;
      r.own = r.voice ? nullptr : auxp + im.layout_.extOwnOff[x];
      if (ex.source == g::External::Source::RootInput) {
        r.src = r.voice ? Src::RootVoice : Src::RootGlobal;
        r.index = slotOf[x];
      } else {
        const std::string_view name = g::atomSignature(ex.atom).name;
        static constexpr std::string_view kVoice[e::kVoiceSignalCount] = {
            "voice.hz", "voice.cents", "voice.gate", "voice.trigger", "voice.velocity", "voice.release",
            "voice.ratio", "voice.harmonic", "voice.unisonCount", "voice.index"};
        bool found = false;
        for (uint32_t k = 0; k < e::kVoiceSignalCount; ++k)
          if (name == kVoice[k]) {
            r.src = Src::VoiceSig;
            r.index = k;
            found = true;
          }
        if (found && !r.voice) return fail("voice source '" + std::string(name) + "' outside a voice stage");
        if (!found) {
          if (r.voice) return fail("per-voice engine source '" + std::string(name) + "' is not supported");
          r.index = ex.channel;
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
      im.exts_.push_back(r);
    }
    im.extPtr_.assign(std::max<size_t>(s_.externals.size() * G_, 1), im.zeros_);
    for (const g::OutputRef& o : s_.outputs) {
      e::IoSlot sl;
      sl.node = o.node;
      sl.channel = o.channel;
      sl.isExport = o.isExport;
      (o.poly == g::Poly::Voice ? io.voiceOut : io.globalOut).push_back(sl);
    }
    return true;
  }

  const void* resource(dsp::Resource kind, uint32_t index) const {
    switch (kind) {
      case dsp::Resource::Wavetable:
        if (res_ && index < res_->wavetables.size() && res_->wavetables[index]) return res_->wavetables[index];
        return &defaultWavetable();
      case dsp::Resource::Shaper:
        if (res_ && index < res_->shapers.size() && res_->shapers[index]) return res_->shapers[index];
        return &defaultShaper();
      case dsp::Resource::Sample:
        if (res_ && index < res_->samples.size() && res_->samples[index]) return res_->samples[index];
        return &defaultSample();
      default: return nullptr;
    }
  }

  // Resources and special-op records: one table slot per op that needs one.
  bool ops() {
    JitImage& im = *img_;
    uint32_t numReduce = 0, numSend = 0;
    for (const g::Op& op : s_.ops) {
      numReduce += op.kind == g::OpKind::Reduce;
      numSend += op.kind == g::OpKind::Send;
    }
    im.reduceInfos_.reserve(numReduce);  // the table keeps their addresses: no reallocation below
    im.sendInfos_.reserve(numSend);
    im.layout_.opInfo.assign(s_.ops.size(), g::kNone);
    reduceIdx_.assign(s_.ops.size(), g::kNone);
    for (uint32_t i = 0; i < s_.ops.size(); ++i) {
      const g::Op& op = s_.ops[i];
      const auto ins = s_.inputs(op);
      const auto outs = s_.outputsOf(op);
      switch (op.kind) {
        case g::OpKind::Kernel: {
          const dsp::Resource r = dsp::kernelInfo(op.kernel).resource;
          if (r != dsp::Resource::None) im.layout_.opInfo[i] = tab_.slot(resource(r, op.resource));
          break;
        }
        case g::OpKind::Reduce: {
          rt::ReduceInfo ri;
          const std::string_view name = g::atomSignature(op.atom).name;
          ri.mode = name == "voice_sum"       ? rt::ReduceMode::Sum
                    : name == "voice_last"    ? rt::ReduceMode::Last
                    : name == "voice_highest" ? rt::ReduceMode::Highest
                    : name == "voice_lowest"  ? rt::ReduceMode::Lowest
                    : name == "voice_loudest" ? rt::ReduceMode::Loudest
                                              : rt::ReduceMode::Sum;
          ri.isBool = op.valueKind == g::Kind::Bool;
          ri.inPattern = isPatternOperand(s_, ins[0]);
          ri.outPattern = isPatternOperand(s_, outs[0]);
          ri.run = &im.run_;
          reduceIdx_[i] = static_cast<uint32_t>(im.reduceInfos_.size());
          im.reduceInfos_.push_back(ri);
          im.layout_.opInfo[i] = tab_.slot(&im.reduceInfos_.back());
          break;
        }
        case g::OpKind::Send: {
          rt::SendInfo si;
          si.bus = op.resource;
          si.channel = op.channel;
          si.lanes = op.lanes;
          si.inPattern = isPatternOperand(s_, ins[0]);
          si.run = &im.run_;
          im.sendInfos_.push_back(si);
          im.layout_.opInfo[i] = tab_.slot(&im.sendInfos_.back());
          break;
        }
        case g::OpKind::Broadcast: break;
      }
    }
    return true;
  }

  float* resolve(const g::Operand& o, uint32_t gi) {
    JitImage& im = *img_;
    const HostLayout& L = im.layout_;
    float* auxp = im.aux_.data();
    switch (o.kind) {
      case g::Operand::Kind::Buffer: {
        const g::Buffer& b = s_.buffers[o.index];
        return im.buf_.data() + L.bufOff[o.index] + (b.perGroup ? static_cast<size_t>(gi) * L.bufSize[o.index] : 0u);
      }
      case g::Operand::Kind::Param: return auxp + L.paramOff + static_cast<size_t>(o.index) * 8;
      case g::Operand::Kind::Const: return auxp + L.constOff + static_cast<size_t>(o.index) * 8;
      default: return im.zeros_;
    }
  }

  bool outCopy(const g::OutputRef& o, uint32_t slot, uint32_t gi) {
    OutCopy c;
    c.slot = slot;
    c.group = gi;
    if (o.value.kind == g::Operand::Kind::External) {
      c.ext = o.value.index;
      if (s_.externals[o.value.index].poly == g::Poly::Global) c.group = 0;
    } else {
      c.src = resolve(o.value, gi);
    }
    c.srcLanes = isPatternOperand(s_, o.value) ? 0u : lanesOfOperand(s_, o.value);
    if (o.poly == g::Poly::Global && c.srcLanes == 8) return fail("global output reads a voice value");
    img_->outs_.push_back(c);
    return true;
  }

  bool stages() {
    JitImage& im = *img_;
    for (const g::Stage& st : s_.stages) {
      StageRun sr;
      sr.voice = st.poly == g::Poly::Voice;
      if (sr.voice) im.io_.hasVoiceStages = true;
      sr.firstReduce = static_cast<uint32_t>(im.reduceInits_.size());
      for (uint32_t i = st.firstOp; i < st.firstOp + st.numOps && i < s_.ops.size(); ++i) {
        const g::Op& op = s_.ops[i];
        if (op.kind != g::OpKind::Reduce) continue;
        const rt::ReduceInfo& ri = im.reduceInfos_[reduceIdx_[i]];
        ReduceInit init;
        init.out = resolve(s_.outputsOf(op)[0], 0);
        init.pattern = ri.outPattern;
        const float inf = std::numeric_limits<float>::infinity();
        init.value = ri.isBool ? dsp::boolMask(ri.mode == rt::ReduceMode::Lowest)
                     : ri.mode == rt::ReduceMode::Highest ? -inf
                     : ri.mode == rt::ReduceMode::Lowest  ? inf
                                                          : 0.0f;
        im.reduceInits_.push_back(init);
      }
      sr.numReduce = static_cast<uint32_t>(im.reduceInits_.size()) - sr.firstReduce;
      sr.firstOut = static_cast<uint32_t>(im.outs_.size());
      if (sr.voice) {
        for (uint32_t gi = 0; gi < G_; ++gi) {
          uint32_t count = 0, slot = 0;
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
    im.firstGlobalOut_ = static_cast<uint32_t>(im.outs_.size());
    uint32_t slot = 0;
    for (const g::OutputRef& o : s_.outputs) {
      if (o.poly == g::Poly::Voice) continue;
      if (!outCopy(o, slot, 0)) return false;
      ++slot;
    }
    for (const g::OutputRef& o : s_.outputs) {
      if (o.poly != g::Poly::Voice) continue;
      bool found = false;
      for (const g::Stage& st : s_.stages) found = found || (st.index == o.stage && st.poly == g::Poly::Voice);
      if (!found) return fail("voice output read at a stage that does not exist");
    }
    return true;
  }

  const g::Schedule& s_;
  std::vector<uint32_t> reduceIdx_;  // per op: index into reduceInfos_ (Reduce ops)
  uint32_t B_, G_;
  const e::Resources* res_;
  PtrTable& tab_;
  std::unique_ptr<JitImage> img_;
  Error err_;
  uint32_t maxN_ = 0;
};

}  // namespace

Result<std::unique_ptr<JitImage>> buildHost(const g::Schedule& s, uint32_t maxBlockFrames, uint32_t maxVoiceGroups,
                                            const e::Resources* resources, PtrTable& tab) {
  return HostBuilder(s, maxBlockFrames, maxVoiceGroups, resources, tab).build();
}

}  // namespace pq::jit::detail
