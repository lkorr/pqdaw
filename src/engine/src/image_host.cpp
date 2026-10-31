// The shared image host (T1.x-arch2): pq/engine/image_host.h says what it does. begin(), run() (header) and the
// special-op kernels are the RT code here; the rest runs when an image is built.

#include "pq/engine/image_host.h"

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

#include "pq/dsp/atoms.h"
#include "pq/graph/atoms.h"

namespace pq::engine::host {

namespace g = pq::graph;

namespace {

constexpr uint32_t kFloatAlign = 16;  // 64 bytes

uint32_t roundUp(uint32_t n, uint32_t a) noexcept { return (n + a - 1) / a * a; }

inline void splat8(float* p, float v) noexcept { _mm256_store_ps(p, _mm256_set1_ps(v)); }  // RT-safe

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

// Built-in default tables: a 2048-point sine wavetable, the identity shaper on [−1, 1], a silent one-frame sample.
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

// ---- special ops ------------------------------------------------------------------------------------------------

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
        const __m256 gv = _mm256_load_ps(gain + f * 8);
        const __m256 x = _mm256_and_ps(_mm256_loadu_ps(in + f * inStride), _mm256_and_ps(m, _mm256_cmp_ps(gv, z, _CMP_NEQ_OQ)));
        out[f] += horizontalSum(_mm256_mul_ps(x, gv));
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
      const __m256 gv = _mm256_load_ps(rc.gain + f * 8);
      const __m256 x = _mm256_and_ps(_mm256_loadu_ps(in + f * stride), _mm256_and_ps(m, _mm256_cmp_ps(gv, z, _CMP_NEQ_OQ)));
      dst[f] += horizontalSum(_mm256_mul_ps(x, gv));
    }
    return;
  }
  for (uint32_t f = 0; f < a.frames; ++f) dst[f] += horizontalSum(_mm256_and_ps(_mm256_loadu_ps(in + f * stride), m));
}

// ---- operands, resources ----------------------------------------------------------------------------------------

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

bool validOperand(const g::Schedule& s, const g::Operand& o) noexcept {
  switch (o.kind) {
    case g::Operand::Kind::Buffer: return o.index < s.buffers.size();
    case g::Operand::Kind::Param: return o.index < s.params.size();
    case g::Operand::Kind::Const: return o.index < s.constants.size();
    case g::Operand::Kind::External: return o.index < s.externals.size();
    default: return false;
  }
}

const void* resourceFor(const Resources* r, dsp::Resource kind, uint32_t index) {
  switch (kind) {
    case dsp::Resource::Wavetable:
      if (r && index < r->wavetables.size() && r->wavetables[index]) return r->wavetables[index];
      if (index >= dsp::kBuiltinWavetableBase)  // T4.12: Operator's built-in waveforms (pq/dsp/tables.h)
        if (const dsp::WavetableView* b = dsp::builtinWavetable(index - dsp::kBuiltinWavetableBase)) return b;
      return &defaultWavetable();
    case dsp::Resource::Shaper:
      if (r && index < r->shapers.size() && r->shapers[index]) return r->shapers[index];
      return &defaultShaper();
    case dsp::Resource::Sample:
      if (r && index < r->samples.size() && r->samples[index]) return r->samples[index];
      return &defaultSample();
    case dsp::Resource::Tuning:  // T2.6: no default, the kernel passes its fallback through
      return r && index < r->tunings.size() ? r->tunings[index] : nullptr;
    default: return nullptr;
  }
}

// ---- build ------------------------------------------------------------------------------------------------------

Error ImageHost::error(const std::string& message) const {
  return Error{ErrorCode::Unsupported, who_ + ": " + message, {}};
}

bool ImageHost::fail(std::string msg) {
  err_ = error(msg);
  return false;
}

Status ImageHost::build(const g::Schedule& s, const BuildOptions& o) {
  who_ = o.who != nullptr ? o.who : "image";
  if (o.maxBlockFrames == 0 || o.maxBlockFrames > (1u << 20) || o.maxVoiceGroups == 0 || o.maxVoiceGroups > 1024)
    return Error{ErrorCode::InvalidArgument, who_ + ": maxBlockFrames must be in [1, 2^20], maxVoiceGroups in [1, 1024]", {}};
  L_.maxBlock = o.maxBlockFrames;
  L_.groups = o.maxVoiceGroups;
  resources_ = o.resources;
  stateLayout_ = s.state;
  io_.backend = o.backend;
  io_.sampleRate = s.sampleRate;
  io_.maxBlockFrames = o.maxBlockFrames;
  io_.maxVoiceGroups = o.maxVoiceGroups;
  io_.params = s.params;
  io_.voiceTailFrames = s.sleep.voiceTailFrames;
  if (!validate(s) || !layoutMemory(s, o.extraBufFloats) || !externals(s)) return err_;
  specialOps(s);
  if (!stages(s) || !sleepLayout(s, o.sleep) || !probeLayout(s)) return err_;
  meterFlag_ = reinterpret_cast<uint32_t*>(auxMem_.data() + L_.meterFlagOff);
  *meterFlag_ = 0;
  meterUnits_.resize(numRegions_);
  for (uint32_t r = 0; r < numRegions_; ++r) meterUnits_[r] = r;
  return Ok{};
}

// The checks every backend relies on, once per op (the generated code and the records assume them).
bool ImageHost::validate(const g::Schedule& s) {
  auto opName = [](uint32_t i) { return "op " + std::to_string(i); };
  for (const g::Buffer& b : s.buffers)
    if (b.cls == g::Buffer::Class::Block && b.lanes != 1 && b.lanes != 8) return fail("buffer lanes must be 1 or 8");
  for (const g::Stage& st : s.stages) {
    if (st.firstRegion + st.numRegions > s.regions.size()) return fail("stage regions out of range");
    for (uint32_t ri = st.firstRegion; ri < st.firstRegion + st.numRegions; ++ri) {
      const g::Region& reg = s.regions[ri];
      if (reg.firstOp + reg.numOps > s.ops.size()) return fail("region ops out of range");
      const bool control = reg.kind == g::RegionKind::Control;
      for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i) {
        const g::Op& op = s.ops[i];
        if (op.firstOperand + op.numIn + op.numOut > s.operands.size()) return fail(opName(i) + ": operands out of range");
        if (st.poly == g::Poly::Voice && op.kind == g::OpKind::Kernel && op.lanes != 8) return fail("a 1-lane op in a voice stage");
        if (op.kind == g::OpKind::Kernel && op.lanes != 1 && op.lanes != 8) return fail("op lanes must be 1 or 8");
        const auto ins = s.inputs(op);
        const auto outs = s.outputsOf(op);
        for (const g::Operand& x : ins)
          if (!validOperand(s, x)) return fail(opName(i) + " has an invalid operand");
        for (const g::Operand& x : outs) {
          if (!validOperand(s, x)) return fail(opName(i) + " has an invalid operand");
          if (x.kind != g::Operand::Kind::Buffer) return fail(opName(i) + " writes a non-buffer");
        }
        if (!control) {
          for (uint32_t j = 0; j < ins.size(); ++j) {
            const bool spl = ((op.splMask >> j) & 1u) != 0;
            if (spl != isPatternOperand(s, ins[j]))
              return fail(opName(i) + " input " + std::to_string(j) + ": Spl mask disagrees with the operand");
            if (!spl && op.kind == g::OpKind::Kernel && lanesOfOperand(s, ins[j]) != op.lanes)
              return fail(opName(i) + " input " + std::to_string(j) + ": lane count disagrees");
          }
          for (const g::Operand& x : outs)
            if (op.kind == g::OpKind::Kernel && (isPatternOperand(s, x) || lanesOfOperand(s, x) != op.lanes))
              return fail(opName(i) + ": output buffer disagrees with the op's lanes");
        } else {
          for (const g::Operand& x : outs)
            if (!isPatternOperand(s, x)) return fail(opName(i) + ": control op writes a block buffer");
          for (const g::Operand& x : ins)
            if (!isPatternOperand(s, x)) return fail(opName(i) + ": control op reads a block buffer");
        }
        switch (op.kind) {
          case g::OpKind::Kernel: {
            if (static_cast<uint32_t>(op.kernel) >= dsp::kAtomCount) return fail(opName(i) + " has no kernel");
            const dsp::KernelInfo& ki = dsp::kernelInfo(op.kernel);
            if (!ki.variadicIn && op.numIn != ki.numIn) return fail(opName(i) + ": wrong input count");
            if (!ki.variadicOut && op.numOut != ki.numOut) return fail(opName(i) + ": wrong output count");
            if (ki.variadicIn && op.numIn < 2) return fail(opName(i) + ": mux without data inputs");
            if (ki.variadicOut && op.numOut < 1) return fail(opName(i) + ": demux without outputs");
            if (ki.stateSize > 0 && op.state == g::kNone) return fail(opName(i) + " has no state entry");
            if (op.state != g::kNone && op.state >= s.state.entries.size()) return fail(opName(i) + " has an invalid state entry");
            break;
          }
          case g::OpKind::Broadcast:
            if (op.numIn != 1 || op.numOut != 1) return fail("broadcast with wrong arity");
            break;
          case g::OpKind::Reduce: {
            if (op.numIn != 1 || op.numOut != 1) return fail("reducer with wrong arity");
            if (!g::atomSignature(op.atom).name.starts_with("voice_")) return fail("unknown reducer");
            const bool inPat = isPatternOperand(s, ins[0]), outPat = isPatternOperand(s, outs[0]);
            if (!inPat && lanesOfOperand(s, ins[0]) != 8) return fail("reducer input is not a voice block");
            if (!outPat && lanesOfOperand(s, outs[0]) != 1) return fail("reducer output is not a global block");
            break;
          }
          case g::OpKind::Send:
            if (op.numIn != 1) return fail("send with wrong arity");
            break;
        }
      }
      for (uint32_t k = reg.firstSolve; k < reg.firstSolve + reg.numSolves; ++k) {
        if (k >= s.solves.size()) return fail("solve index out of range");
        const g::SolveBlock& sb = s.solves[k];
        if (sb.numUnknowns != sb.numEquations || sb.numUnknowns == 0)
          return fail("solve block with " + std::to_string(sb.numUnknowns) + " unknowns and " +
                      std::to_string(sb.numEquations) + " equations");
        if (sb.firstOp < reg.firstOp || sb.firstOp + sb.numOps > reg.firstOp + reg.numOps) return fail("solve body outside its region");
        if (reg.kind == g::RegionKind::Block) return fail("solve block in a block region");
        if (reg.kind == g::RegionKind::Loop && reg.subBlock > 1) return fail("solve block in a sub-blocked loop");
        if (sb.firstUnknown + sb.numUnknowns > s.unknowns.size() || sb.firstEquation + sb.numEquations > s.equations.size())
          return fail("solve block out of range");
        for (uint32_t u = sb.firstUnknown; u < sb.firstUnknown + sb.numUnknowns; ++u) {
          const g::SolveUnknown& su = s.unknowns[u];
          if (su.value.kind != g::Operand::Kind::Buffer || !validOperand(s, su.value)) return fail("solve unknown without a buffer");
          if (su.init.kind != g::Operand::Kind::None && !validOperand(s, su.init)) return fail("invalid initial guess");
          if (su.state != g::kNone && su.state >= s.state.entries.size()) return fail("invalid solve state");
          if (su.init.kind == g::Operand::Kind::None && su.state == g::kNone)
            return fail("solve unknown with neither an initial guess nor a state entry");
        }
        for (uint32_t q = sb.firstEquation; q < sb.firstEquation + sb.numEquations; ++q)
          if (!validOperand(s, s.equations[q].lhs) || !validOperand(s, s.equations[q].rhs)) return fail("invalid equation operand");
        L_.maxSolveN = std::max(L_.maxSolveN, sb.numUnknowns);
      }
    }
  }
  for (const g::OutputRef& x : s.outputs)
    if (!validOperand(s, x.value)) return fail("output with an invalid operand");
  return true;
}

bool ImageHost::layoutMemory(const g::Schedule& s, uint32_t extraBufFloats) {
  const uint32_t B = L_.maxBlock, G = L_.groups;
  uint32_t off = 0;
  L_.bufOff.resize(s.buffers.size());
  L_.bufSize.resize(s.buffers.size());
  bufPerGroup_.resize(s.buffers.size());
  for (size_t i = 0; i < s.buffers.size(); ++i) {
    const g::Buffer& b = s.buffers[i];
    bufPerGroup_[i] = b.perGroup ? 1 : 0;
    const uint32_t size = roundUp(b.cls == g::Buffer::Class::Pattern ? 8u : B * b.lanes, kFloatAlign);
    L_.bufOff[i] = off;
    L_.bufSize[i] = size;
    off += size * (b.perGroup ? G : 1u);
  }
  L_.extraOff = off;
  off += roundUp(extraBufFloats, kFloatAlign);
  L_.meterOff = off;  // T4.5: per region (cycles, timings) as two uint64 (the backends time their regions into it)
  numRegions_ = static_cast<uint32_t>(s.regions.size());
  off += roundUp(std::max<uint32_t>(numRegions_, 1u) * 4u, kFloatAlign);
  L_.bufFloats = std::max<uint32_t>(off, kFloatAlign);
  bufMem_ = AlignedBuffer<float>(L_.bufFloats);
  uint32_t aux = 0;
  L_.paramOff = aux;
  aux += static_cast<uint32_t>(s.params.size()) * 8;
  L_.constOff = aux;
  aux += static_cast<uint32_t>(s.constants.size()) * 8;
  L_.extOwnOff.clear();
  for (const g::External& x : s.externals) {
    L_.extOwnOff.push_back(aux);
    if (x.poly == g::Poly::Voice) continue;
    aux += roundUp(x.rate == g::Rate::Audio ? B : 8u, kFloatAlign);
  }
  aux = roundUp(aux, kFloatAlign);
  L_.sleepOff = aux;  // one flag word per sleep segment (T1.x-silence)
  aux += roundUp(std::max<uint32_t>(static_cast<uint32_t>(s.sleep.segments.size()), 1u), kFloatAlign);
  L_.meterFlagOff = aux;  // T4.5
  aux += kFloatAlign;
  L_.zeroOff = aux;
  aux += B * 8;
  L_.auxFloats = aux;
  auxMem_ = AlignedBuffer<float>(aux);
  float* auxp = auxMem_.data();
  zeros_ = auxp + L_.zeroOff;
  for (size_t i = 0; i < s.params.size(); ++i) {
    const g::ParamSlot& p = s.params[i];
    ParamRun pr;
    pr.pattern = auxp + L_.paramOff + i * 8;
    pr.isBool = p.kind == g::Kind::Bool;
    pr.def = p.defaultValue;
    pr.hasRange = p.hasRange && !pr.isBool && p.rangeMin <= p.rangeMax;
    pr.lo = p.rangeMin;
    pr.hi = p.rangeMax;
    for (uint32_t l = 0; l < 8; ++l) pr.pattern[l] = pr.isBool ? dsp::boolMask(pr.def != 0.0f) : pr.def;
    params_.push_back(pr);
  }
  for (size_t i = 0; i < s.constants.size(); ++i)
    for (uint32_t l = 0; l < 8; ++l) auxp[L_.constOff + i * 8 + l] = s.constants[i].value;
  // State: global arena, then one block per voice group.
  const g::StateLayout& SL = s.state;
  L_.globalStateBytes = roundUp(SL.globalBytes, 64);
  L_.voiceStride = roundUp(SL.voiceBytes, 64);
  const size_t stateBytes = static_cast<size_t>(L_.globalStateBytes) + static_cast<size_t>(L_.voiceStride) * G;
  stateMem_ = AlignedBuffer<float>(std::max<size_t>(stateBytes / 4, 16));
  for (const g::StateEntry& e : SL.entries) {
    if (e.offset + e.size > (e.arena == g::Poly::Voice ? SL.voiceBytes : SL.globalBytes)) return fail("state entry outside its arena");
    if (e.tag.kernel == g::StateTag::kSolveGuess) continue;  // zeros
    const auto atom = static_cast<dsp::Atom>(e.tag.kernel);
    if (e.arena == g::Poly::Global) {
      dsp::initState(atom, globalState() + e.offset, e.init);
      continue;
    }
    for (uint32_t gi = 0; gi < G; ++gi) {
      dsp::StateInit init = e.init;
      // Voice groups get distinct noise: the group index is mixed into the seed (lanes are mixed by dsp).
      if (atom == dsp::Atom::Noise) init.seed += gi * 0x9E3779B9u;
      dsp::initState(atom, voiceState(gi) + e.offset, init);
    }
  }
  return true;
}

// Interface slots and engine sources.
bool ImageHost::externals(const g::Schedule& s) {
  // Root input slots: per poly, sorted by (ordinal, channel).
  std::vector<uint32_t> gIn, vIn;
  for (uint32_t x = 0; x < s.externals.size(); ++x)
    if (s.externals[x].source == g::External::Source::RootInput) (s.externals[x].poly == g::Poly::Voice ? vIn : gIn).push_back(x);
  auto byOrd = [&](uint32_t a, uint32_t b) {
    const g::External &x = s.externals[a], &y = s.externals[b];
    return x.index != y.index ? x.index < y.index : x.channel < y.channel;
  };
  std::sort(gIn.begin(), gIn.end(), byOrd);
  std::sort(vIn.begin(), vIn.end(), byOrd);
  std::vector<uint32_t> slotOf(s.externals.size(), 0);
  auto addSlots = [&](const std::vector<uint32_t>& list, std::vector<IoSlot>& slots) {
    for (uint32_t x : list) {
      const g::External& ex = s.externals[x];
      slotOf[x] = static_cast<uint32_t>(slots.size());
      IoSlot sl;
      sl.node = ex.node;
      sl.channel = ex.channel;
      sl.rate = ex.rate;
      sl.ordinal = ex.index;
      slots.push_back(sl);
    }
  };
  addSlots(gIn, io_.globalIn);
  addSlots(vIn, io_.voiceIn);
  float* auxp = auxMem_.data();
  for (uint32_t x = 0; x < s.externals.size(); ++x) {
    const g::External& ex = s.externals[x];
    ExtRun r;
    r.voice = ex.poly == g::Poly::Voice;
    r.pattern = ex.rate != g::Rate::Audio;
    r.own = r.voice ? nullptr : auxp + L_.extOwnOff[x];
    if (ex.source == g::External::Source::RootInput) {
      r.src = r.voice ? Src::RootVoice : Src::RootGlobal;
      r.index = slotOf[x];
    } else {
      const std::string_view name = g::atomSignature(ex.atom).name;
      static constexpr std::string_view kVoice[kVoiceSignalCount] = {
          "voice.hz", "voice.cents", "voice.gate", "voice.trigger", "voice.velocity", "voice.release",
          "voice.ratio", "voice.harmonic", "voice.unisonCount", "voice.index", "voice.degree", "voice.equave",
          "voice.tuning"};
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
        r.index = ex.channel;
        if (name == "transport.bpm") r.src = Src::TransportBpm;
        else if (name == "transport.beat") r.src = Src::TransportBeat;
        else if (name == "transport.barPhase") r.src = Src::TransportBarPhase;
        else if (name == "transport.playing") r.src = Src::TransportPlaying;
        else if (name == "track.in") r.src = Src::TrackIn;
        else if (name == "sidechain.in") r.src = Src::Sidechain;
        else if (name == "clipPlayer") {  // T5.4: the clip slot's audio, rendered by the timeline's clip player
          r.src = ex.channel < kClipChannels ? Src::Clip : Src::Silence;
          r.index = ex.clip * kClipChannels + ex.channel;
          if (std::find(io_.clipSlots.begin(), io_.clipSlots.end(), ex.clip) == io_.clipSlots.end())
            io_.clipSlots.insert(std::upper_bound(io_.clipSlots.begin(), io_.clipSlots.end(), ex.clip), ex.clip);
        } else return fail("unknown engine source '" + std::string(name) + "'");
      }
    }
    exts_.push_back(r);
  }
  extPtr_.assign(std::max<size_t>(s.externals.size() * L_.groups, 1), zeros_);
  for (const g::OutputRef& o : s.outputs) {
    IoSlot sl;
    sl.node = o.node;
    sl.channel = o.channel;
    sl.isExport = o.isExport;
    (o.poly == g::Poly::Voice ? io_.voiceOut : io_.globalOut).push_back(sl);
  }
  return true;
}

// One record per Reduce / Send op (not per voice group: the content is the same for every group).
void ImageHost::specialOps(const g::Schedule& s) {
  uint32_t numReduce = 0, numSend = 0;
  for (const g::Op& op : s.ops) {
    numReduce += op.kind == g::OpKind::Reduce;
    numSend += op.kind == g::OpKind::Send;
  }
  reduceInfos_.reserve(numReduce);  // records keep their addresses: no reallocation below
  sendInfos_.reserve(numSend);
  opInfo_.assign(s.ops.size(), g::kNone);
  opKind_.resize(s.ops.size());
  opStage_.assign(s.ops.size(), g::kNone);
  for (uint32_t k = 0; k < s.stages.size(); ++k)
    for (uint32_t ri = s.stages[k].firstRegion; ri < s.stages[k].firstRegion + s.stages[k].numRegions; ++ri)
      for (uint32_t i = s.regions[ri].firstOp; i < s.regions[ri].firstOp + s.regions[ri].numOps; ++i) opStage_[i] = k;
  for (uint32_t i = 0; i < s.ops.size(); ++i) {
    const g::Op& op = s.ops[i];
    opKind_[i] = op.kind;
    if (op.kind == g::OpKind::Reduce && op.numIn >= 1 && op.numOut >= 1) {
      ReduceInfo ri;
      const std::string_view name = g::atomSignature(op.atom).name;
      ri.mode = name == "voice_sum"       ? ReduceMode::Sum
                : name == "voice_last"    ? ReduceMode::Last
                : name == "voice_highest" ? ReduceMode::Highest
                : name == "voice_lowest"  ? ReduceMode::Lowest
                : name == "voice_loudest" ? ReduceMode::Loudest
                                          : ReduceMode::Sum;
      ri.isBool = op.valueKind == g::Kind::Bool;
      ri.inPattern = isPatternOperand(s, s.inputs(op)[0]);
      ri.outPattern = isPatternOperand(s, s.outputsOf(op)[0]);
      ri.run = &run_;
      opInfo_[i] = static_cast<uint32_t>(reduceInfos_.size());
      reduceInfos_.push_back(ri);
    } else if (op.kind == g::OpKind::Send && op.numIn >= 1) {
      SendInfo si;
      si.bus = op.resource;
      si.channel = op.channel;
      si.lanes = op.lanes;
      si.inPattern = isPatternOperand(s, s.inputs(op)[0]);
      si.run = &run_;
      opInfo_[i] = static_cast<uint32_t>(sendInfos_.size());
      sendInfos_.push_back(si);
    }
  }
  // The records of a pair's second group (T1b.2): the same, reading runB_ (one allocation, fixed addresses).
  reduceInfosB_ = reduceInfos_;
  for (ReduceInfo& ri : reduceInfosB_) ri.run = &runB_;
}

const ReduceInfo* ImageHost::reduceInfo(uint32_t op) const noexcept {
  if (op >= opKind_.size() || opKind_[op] != g::OpKind::Reduce || opInfo_[op] == g::kNone) return nullptr;
  return &reduceInfos_[opInfo_[op]];
}

const SendInfo* ImageHost::sendInfo(uint32_t op) const noexcept {
  if (op >= opKind_.size() || opKind_[op] != g::OpKind::Send || opInfo_[op] == g::kNone) return nullptr;
  return &sendInfos_[opInfo_[op]];
}

void ImageHost::enablePairs(const std::vector<uint8_t>& pairableStages, const std::function<float*(uint32_t, uint32_t)>& alt) {
  pairable_.assign(stages_.size(), 0);
  for (uint32_t k = 0; k < stages_.size() && k < pairableStages.size(); ++k) {
    if (!pairableStages[k] || !stages_[k].voice) continue;
    bool sends = false;
    for (uint32_t i = 0; i < opStage_.size(); ++i) sends = sends || (opStage_[i] == k && opKind_[i] == g::OpKind::Send);
    if (sends) continue;  // a bus channel adds one group's sends in op order: a pair would interleave two groups
    pairable_[k] = 1;
    const StageRun& st = stages_[k];
    for (uint32_t gi = 0; gi < L_.groups; ++gi)
      for (uint32_t c = st.firstOut + gi * st.numOut; c < st.firstOut + (gi + 1) * st.numOut; ++c)
        if (outBuffer_[c] != g::kNone) outs_[c].srcAlt = alt(k, outBuffer_[c]);
    for (uint32_t i = st.firstProbe; i < st.firstProbe + st.numProbes; ++i)  // T4.5
      if (probes_[i].buffer != g::kNone && !bufPerGroup_[probes_[i].buffer]) probes_[i].alt = alt(k, probes_[i].buffer);
  }
}

const ReduceInfo* ImageHost::reduceInfoB(uint32_t op) const noexcept {
  if (op >= opKind_.size() || opKind_[op] != g::OpKind::Reduce || opInfo_[op] == g::kNone) return nullptr;
  return &reduceInfosB_[opInfo_[op]];
}

float* ImageHost::resolve(const g::Operand& o, uint32_t gi) noexcept {
  float* auxp = auxMem_.data();
  switch (o.kind) {
    case g::Operand::Kind::Buffer:
      return bufMem_.data() + L_.bufOff[o.index] + (bufPerGroup_[o.index] ? static_cast<size_t>(gi) * L_.bufSize[o.index] : 0u);
    case g::Operand::Kind::Param: return auxp + L_.paramOff + static_cast<size_t>(o.index) * 8;
    case g::Operand::Kind::Const: return auxp + L_.constOff + static_cast<size_t>(o.index) * 8;
    default: return zeros_;
  }
}

bool ImageHost::outCopy(const g::Schedule& s, const g::OutputRef& o, uint32_t slot, uint32_t gi) {
  OutCopy c;
  c.slot = slot;
  c.group = gi;
  if (o.value.kind == g::Operand::Kind::External) {
    c.ext = o.value.index;
    if (s.externals[o.value.index].poly == g::Poly::Global) c.group = 0;
  } else {
    c.src = resolve(o.value, gi);
  }
  outBuffer_.push_back(o.value.kind == g::Operand::Kind::Buffer && !bufPerGroup_[o.value.index] ? o.value.index : g::kNone);
  c.srcLanes = isPatternOperand(s, o.value) ? 0u : lanesOfOperand(s, o.value);
  if (o.poly == g::Poly::Global && c.srcLanes == 8) return fail("global output reads a voice value");
  outs_.push_back(c);
  return true;
}

bool ImageHost::stages(const g::Schedule& s) {
  for (const g::Stage& st : s.stages) {
    StageRun sr;
    sr.voice = st.poly == g::Poly::Voice;
    if (sr.voice) io_.hasVoiceStages = true;
    // Reducer accumulators of this stage, initialised once per block before its group loop.
    sr.firstReduce = static_cast<uint32_t>(reduceInits_.size());
    for (uint32_t ri = st.firstRegion; ri < st.firstRegion + st.numRegions; ++ri) {
      const g::Region& reg = s.regions[ri];
      for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i) {
        if (s.ops[i].kind != g::OpKind::Reduce) continue;
        const ReduceInfo& info = reduceInfos_[opInfo_[i]];
        ReduceInit init;
        init.out = resolve(s.outputsOf(s.ops[i])[0], 0);
        init.pattern = info.outPattern;
        const float inf = std::numeric_limits<float>::infinity();
        init.value = info.isBool ? dsp::boolMask(info.mode == ReduceMode::Lowest)
                     : info.mode == ReduceMode::Highest ? -inf
                     : info.mode == ReduceMode::Lowest  ? inf
                                                        : 0.0f;
        reduceInits_.push_back(init);
      }
    }
    sr.numReduce = static_cast<uint32_t>(reduceInits_.size()) - sr.firstReduce;
    // Voice outputs read at the end of this stage, per group.
    sr.firstOut = static_cast<uint32_t>(outs_.size());
    if (sr.voice) {
      for (uint32_t gi = 0; gi < L_.groups; ++gi) {
        uint32_t count = 0, slot = 0;
        for (const g::OutputRef& o : s.outputs) {
          if (o.poly != g::Poly::Voice) continue;
          if (o.stage == st.index) {
            if (!outCopy(s, o, slot, gi)) return false;
            ++count;
          }
          ++slot;
        }
        sr.numOut = count;
      }
    }
    stages_.push_back(sr);
  }
  // Global outputs, after the last stage.
  firstGlobalOut_ = static_cast<uint32_t>(outs_.size());
  uint32_t slot = 0;
  for (const g::OutputRef& o : s.outputs) {
    if (o.poly == g::Poly::Voice) continue;
    if (!outCopy(s, o, slot, 0)) return false;
    ++slot;
  }
  // A voice output whose stage does not exist cannot happen (the scheduler adds an empty voice stage for them).
  for (const g::OutputRef& o : s.outputs) {
    if (o.poly != g::Poly::Voice) continue;
    bool found = false;
    for (const g::Stage& st : s.stages) found = found || (st.index == o.stage && st.poly == g::Poly::Voice);
    if (!found) return fail("voice output read at a stage that does not exist");
  }
  return true;
}

// ---- inspection (T4.5) -----------------------------------------------------------------------------------------

bool ImageHost::probeLayout(const g::Schedule& s) {
  probes_.clear();
  std::vector<ProbeRun> globals;
  std::vector<std::vector<ProbeRun>> perStage(s.stages.size());
  for (const g::ProbeRef& pr : s.probes) {
    if (!validOperand(s, pr.value)) return fail("probe with an invalid operand");
    ProbeRun r;
    r.id = pr.id;
    r.isBool = pr.kind == g::Kind::Bool;
    r.sum = pr.voiceSum;
    r.region = pr.region < s.regions.size() ? pr.region : g::kNone;
    switch (pr.value.kind) {
      case g::Operand::Kind::Buffer:
        r.buffer = pr.value.index;
        r.perFrame = isPatternOperand(s, pr.value) ? 0u : lanesOfOperand(s, pr.value);
        r.src = resolve(pr.value, 0);
        break;
      case g::Operand::Kind::External: {
        const g::External& x = s.externals[pr.value.index];
        r.ext = pr.value.index;
        r.perFrame = x.rate == g::Rate::Audio ? (x.poly == g::Poly::Voice ? 8u : 1u) : 0u;
        break;
      }
      case g::Operand::Kind::Param:
      case g::Operand::Kind::Const:
        r.src = resolve(pr.value, 0);
        r.perFrame = 0;
        break;
      default: return fail("probe with an invalid operand");
    }
    r.voice = pr.poly == g::Poly::Voice;
    if (r.voice) {
      if (pr.stage >= s.stages.size() || s.stages[pr.stage].poly != g::Poly::Voice) return fail("voice probe outside a voice stage");
      if (r.perFrame == 1) return fail("voice probe of a global block");
      r.stage = pr.stage;
      perStage[pr.stage].push_back(r);
    } else {
      if (r.perFrame == 8) return fail("global probe of a voice value");
      globals.push_back(r);
    }
  }
  for (uint32_t k = 0; k < perStage.size(); ++k) {
    stages_[k].firstProbe = static_cast<uint32_t>(probes_.size());
    stages_[k].numProbes = static_cast<uint32_t>(perStage[k].size());
    probes_.insert(probes_.end(), perStage[k].begin(), perStage[k].end());
  }
  firstGlobalProbe_ = static_cast<uint32_t>(probes_.size());
  numGlobalProbes_ = static_cast<uint32_t>(globals.size());
  probes_.insert(probes_.end(), globals.begin(), globals.end());
  const size_t stride = roundUp(L_.maxBlock, kFloatAlign);
  probeScratch_ = AlignedBuffer<float>(std::max<size_t>(probes_.size(), 1) * stride);
  for (size_t i = 0; i < probes_.size(); ++i) probes_[i].scratch = probeScratch_.data() + i * stride;
  return true;
}

bool ImageHost::bindInspect(const InspectBinding& b) {
  for (ProbeRun& p : probes_) {
    p.ring = nullptr;
    p.level = nullptr;
    for (const ProbeTap& t : b.probes)
      if (t.id == p.id) {
        p.ring = t.ring;
        p.level = t.level;
        break;
      }
  }
  meter_ = b.meter;
  meterKey_ = b.meter ? b.meter->attach(meterUnits_) : 0u;
  meterActivated_ = false;
  return true;
}

void ImageHost::probeStageBegin(const StageRun& st) noexcept {  // RT-safe
  for (uint32_t i = st.firstProbe; i < st.firstProbe + st.numProbes; ++i) probes_[i].captured = false;
}

// After voice group gi ran: the newest voice's lane (or the sum over the group's sounding lanes) into the scratch.
void ImageHost::probeGroup(const StageRun& st, uint32_t gi, uint32_t frames, bool second) noexcept {  // RT-safe
  const VoiceTable* vt = voices_;
  const uint32_t mask = vt->groups[gi].laneMask & 0xFFu;
  const uint32_t G = L_.groups;
  for (uint32_t i = st.firstProbe; i < st.firstProbe + st.numProbes; ++i) {
    ProbeRun& p = probes_[i];
    if (p.level == nullptr && (p.ring == nullptr || !p.ring->wanted())) continue;
    uint32_t lane = 8;
    if (!p.sum) {
      if (vt->lastGroup != gi || vt->lastLane >= 8 || ((mask >> vt->lastLane) & 1u) == 0) continue;
      lane = vt->lastLane;
    }
    const float* x = nullptr;
    if (p.buffer != g::kNone) {
      x = second && p.alt != nullptr ? p.alt
                                     : bufMem_.data() + L_.bufOff[p.buffer] +
                                           (bufPerGroup_[p.buffer] ? static_cast<size_t>(gi) * L_.bufSize[p.buffer] : 0u);
    } else if (p.ext != g::kNone) {
      x = extPtr_[static_cast<size_t>(p.ext) * G + gi];
    } else {
      x = p.src;
    }
    if (x == nullptr) continue;
    const uint32_t step = p.perFrame == 0 ? 0u : 8u;
    float* d = p.scratch;
    if (!p.sum) {
      for (uint32_t f = 0; f < frames; ++f) d[f] = x[f * step + lane];
    } else if (p.isBool) {  // any sounding voice high
      for (uint32_t f = 0; f < frames; ++f) {
        const uint32_t bits = static_cast<uint32_t>(_mm256_movemask_ps(_mm256_loadu_ps(x + f * step))) & mask;
        const bool high = bits != 0 || (p.captured && std::signbit(d[f]));
        d[f] = dsp::boolMask(high);
      }
    } else {
      const __m256 m = laneMaskVector(mask);
      for (uint32_t f = 0; f < frames; ++f) {
        const float v = horizontalSum(_mm256_and_ps(_mm256_loadu_ps(x + f * step), m));
        d[f] = p.captured ? d[f] + v : v;
      }
    }
    p.captured = true;
  }
}

void ImageHost::probeStageEnd(const StageRun& st, uint32_t frames) noexcept {  // RT-safe
  for (uint32_t i = st.firstProbe; i < st.firstProbe + st.numProbes; ++i) {
    ProbeRun& p = probes_[i];
    if (p.level == nullptr && (p.ring == nullptr || !p.ring->wanted())) continue;
    if (!p.captured) std::memset(p.scratch, 0, frames * sizeof(float));  // no voice: silence
    probeWrite(p, p.scratch, frames);
  }
}

void ImageHost::probeGlobals(uint32_t frames) noexcept {  // RT-safe
  for (uint32_t i = firstGlobalProbe_; i < firstGlobalProbe_ + numGlobalProbes_; ++i) {
    ProbeRun& p = probes_[i];
    if (p.level == nullptr && (p.ring == nullptr || !p.ring->wanted())) continue;
    const float* x = p.ext != g::kNone ? extPtr_[static_cast<size_t>(p.ext) * L_.groups] : p.src;
    if (p.region != g::kNone && regionAsleep(p.region)) {  // its region did not run: it outputs 0
      std::memset(p.scratch, 0, frames * sizeof(float));
      x = p.scratch;
    } else if (p.perFrame == 0) {
      const float v = x[0];
      for (uint32_t f = 0; f < frames; ++f) p.scratch[f] = v;
      x = p.scratch;
    }
    probeWrite(p, x, frames);
  }
}

void ImageHost::probeWrite(ProbeRun& p, const float* data, uint32_t frames) noexcept {  // RT-safe
  if (p.ring != nullptr) p.ring->write(&data, 1, frames);
  if (p.level == nullptr || frames == 0) return;
  if (p.isBool) {
    bool any = false, all = true;
    for (uint32_t f = 0; f < frames; ++f) {
      const bool h = std::signbit(data[f]);
      any = any || h;
      all = all && h;
    }
    p.level->write(std::signbit(data[frames - 1]) ? 1.0f : 0.0f, all ? 1.0f : 0.0f, any ? 1.0f : 0.0f);
    return;
  }
  float lo = data[0], hi = data[0];
  for (uint32_t f = 1; f < frames; ++f) {
    lo = std::min(lo, data[f]);
    hi = std::max(hi, data[f]);
  }
  p.level->write(data[frames - 1], lo, hi);
}

void ImageHost::meterEnd(uint32_t frames) noexcept {  // RT-safe
  const uint64_t total = __rdtsc() - meterT0_;
  meter_->add(regionCycles(), numRegions_, total, frames);
}

// ---- sleep (T1.x-silence) ---------------------------------------------------------------------------------------

bool ImageHost::sleepLayout(const g::Schedule& s, SleepMode mode) {
  sleepMode_ = mode;
  const g::SleepPlan& plan = s.sleep;
  planSegments_ = !plan.segments.empty();
  stageSegFirst_.assign(s.stages.size() + 1, 0);
  regionSeg_.assign(s.regions.size(), g::kNone);
  regionFillFirst_.assign(s.regions.size() + 1, 0);
  paramBits_.assign(params_.size(), 0);
  paramChanged_.assign(params_.size(), 1);
  dormantCapable_ = plan.dormantCapable;
  if (mode == SleepMode::Off || plan.segments.empty() || plan.regions.size() != s.regions.size()) {
    segs_.clear();
    return true;
  }
  float* auxp = auxMem_.data();
  for (uint32_t gi = 0; gi < plan.segments.size(); ++gi) {
    const g::SleepSegment& sg = plan.segments[gi];
    if (sg.stage >= s.stages.size() || s.stages[sg.stage].poly != g::Poly::Global) return fail("sleep segment outside a global stage");
    if (gi > 0 && sg.stage < plan.segments[gi - 1].stage) return fail("sleep segments out of stage order");
    if (sg.firstSource + sg.numSources > plan.sources.size() || sg.firstState + sg.numStates > plan.states.size() ||
        sg.firstParam + sg.numParams > plan.params.size())
      return fail("sleep segment out of range");
    SegRun r;
    r.stage = sg.stage;
    r.firstSource = static_cast<uint32_t>(segSources_.size());
    for (uint32_t k = sg.firstSource; k < sg.firstSource + sg.numSources; ++k) {
      const g::Operand& o = plan.sources[k];
      if (!validOperand(s, o)) return fail("sleep source out of range");
      SegSource src;
      if (o.kind == g::Operand::Kind::External) {
        const g::External& x = s.externals[o.index];
        if (x.poly != g::Poly::Global) return fail("sleep source is a voice signal");
        src.ext = o.index;
        src.perFrame = x.rate == g::Rate::Audio ? 1u : 0u;
        if (exts_[o.index].src == Src::RootGlobal) src.inSlot = exts_[o.index].index;
      } else if (o.kind == g::Operand::Kind::Buffer) {
        if (bufPerGroup_[o.index]) return fail("sleep source is a per-group buffer");
        src.buf = resolve(o, 0);
        src.perFrame = isPatternOperand(s, o) ? 0u : lanesOfOperand(s, o);
      } else {
        return fail("sleep source is neither an engine input nor a buffer");
      }
      segSources_.push_back(src);
    }
    r.numSources = static_cast<uint32_t>(segSources_.size()) - r.firstSource;
    r.firstState = static_cast<uint32_t>(segStates_.size());
    for (uint32_t k = sg.firstState; k < sg.firstState + sg.numStates; ++k) {
      const g::SleepState& st = plan.states[k];
      if (st.entry >= s.state.entries.size() || s.state.entries[st.entry].arena != g::Poly::Global)
        return fail("sleep state is not a global state entry");
      const g::StateEntry& e = s.state.entries[st.entry];
      const size_t need = st.kind == g::SleepStateKind::Delay      ? sizeof(dsp::DelayState)
                          : st.kind == g::SleepStateKind::Smoother ? sizeof(dsp::GainState)
                                                                   : sizeof(float);
      if (e.size < need) return fail("sleep state smaller than its kind");
      SegState ss;
      ss.ptr = globalState() + e.offset;
      ss.kind = st.kind;
      ss.tail = std::max<uint32_t>(st.tail, 1u);
      segStates_.push_back(ss);
    }
    r.numStates = static_cast<uint32_t>(segStates_.size()) - r.firstState;
    r.firstParam = static_cast<uint32_t>(segParams_.size());
    for (uint32_t k = sg.firstParam; k < sg.firstParam + sg.numParams; ++k) {
      if (plan.params[k] >= params_.size()) return fail("sleep param out of range");
      segParams_.push_back(plan.params[k]);
    }
    r.numParams = static_cast<uint32_t>(segParams_.size()) - r.firstParam;
    r.flag = reinterpret_cast<uint32_t*>(auxp + L_.sleepOff + gi);
    *r.flag = 0;
    segs_.push_back(r);
  }
  for (uint32_t k = 0; k <= s.stages.size(); ++k) {
    uint32_t first = static_cast<uint32_t>(segs_.size());
    for (uint32_t gi = 0; gi < segs_.size(); ++gi)
      if (segs_[gi].stage >= k) {
        first = gi;
        break;
      }
    stageSegFirst_[k] = first;
  }
  for (uint32_t r = 0; r < s.regions.size(); ++r) {
    const g::RegionSleep& rs = plan.regions[r];
    regionFillFirst_[r] = static_cast<uint32_t>(fills_.size());
    if (rs.segment == g::kNone) continue;
    if (rs.segment >= segs_.size() || rs.firstFill + rs.numFills > plan.fills.size()) return fail("region sleep out of range");
    if (s.regions[r].stage != s.stages[segs_[rs.segment].stage].index) return fail("region sleeps in another stage's segment");
    regionSeg_[r] = rs.segment;
    for (uint32_t k = rs.firstFill; k < rs.firstFill + rs.numFills; ++k) {
      const uint32_t b = plan.fills[k];
      if (b >= s.buffers.size() || bufPerGroup_[b]) return fail("sleep fill is not a shared buffer");
      FillRun f;
      f.ptr = buf() + L_.bufOff[b];
      f.perFrame = s.buffers[b].cls == g::Buffer::Class::Pattern ? 0u : s.buffers[b].lanes;
      fills_.push_back(f);
    }
  }
  regionFillFirst_[s.regions.size()] = static_cast<uint32_t>(fills_.size());
  return true;
}

namespace {

// Exactly ±0 over n floats (bools: false). Looks at the last float first: a sounding signal is caught at once.
inline bool allSilent(const float* p, uint32_t n) noexcept {  // RT-safe
  if (n == 0) return true;
  if ((std::bit_cast<uint32_t>(p[n - 1]) & 0x7FFFFFFFu) != 0) return false;
  const __m256i abs = _mm256_set1_epi32(0x7FFFFFFF);
  __m256i acc = _mm256_setzero_si256();
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) acc = _mm256_or_si256(acc, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i)));
  uint32_t tail = 0;
  for (; i < n; ++i) tail |= std::bit_cast<uint32_t>(p[i]);
  return _mm256_testz_si256(acc, abs) != 0 && (tail & 0x7FFFFFFFu) == 0;
}

inline bool small(float v) noexcept { return std::fabs(v) <= kSleepEpsilon; }  // NaN: false

}  // namespace

void ImageHost::fillRegion(uint32_t region, uint32_t frames) noexcept {  // RT-safe
  for (uint32_t k = regionFillFirst_[region]; k < regionFillFirst_[region + 1]; ++k) {
    const FillRun& f = fills_[k];
    std::memset(f.ptr, 0, sizeof(float) * (f.perFrame == 0 ? 8u : frames * f.perFrame));
  }
}

bool ImageHost::regionFillsSilent(uint32_t region, uint32_t frames) const noexcept {  // RT-safe
  for (uint32_t k = regionFillFirst_[region]; k < regionFillFirst_[region + 1]; ++k) {
    const FillRun& f = fills_[k];
    if (!allSilent(f.ptr, f.perFrame == 0 ? 8u : frames * f.perFrame)) return false;
  }
  return true;
}

// Whether the segment's states have decayed (see the header); flushes them when they have.
bool ImageHost::segmentQuiescent(SegRun& g) noexcept {  // RT-safe
  bool quiet = true;
  for (uint32_t k = g.firstState; k < g.firstState + g.numStates; ++k) {
    SegState& st = segStates_[k];
    switch (st.kind) {
      case g::SleepStateKind::Memory:
        quiet = quiet && small(*reinterpret_cast<const float*>(st.ptr));
        break;
      case g::SleepStateKind::Smoother:
        quiet = quiet && (std::bit_cast<uint32_t>(reinterpret_cast<const dsp::GainState*>(st.ptr)->d[0]) & 0x7FFFFFFFu) == 0;
        break;
      case g::SleepStateKind::Settled: break;
      case g::SleepStateKind::Delay: {
        auto& d = *reinterpret_cast<dsp::DelayState*>(st.ptr);
        if (d.lanes != 1) {
          quiet = false;
          break;
        }
        const uint32_t reach = std::min(st.tail, d.capacity);
        const float* ring = d.ring();
        const uint32_t w = d.writePos;
        if (!st.known) {  // the whole reachable history once (a fresh image, or one whose state was transferred)
          st.known = true;
          st.scanPos = w - reach;
          st.loudPos = st.nonzeroPos = w - reach;
        }
        // The frames written since the last look (at most the ring: older ones are overwritten anyway).
        uint32_t from = st.scanPos;
        if (static_cast<int32_t>(w - from) > static_cast<int32_t>(d.capacity)) from = w - d.capacity;
        for (uint32_t pos = from; pos != w; ++pos) {
          const float v = ring[pos & d.mask];
          const uint32_t bits = std::bit_cast<uint32_t>(v) & 0x7FFFFFFFu;
          if (bits != 0) {
            st.nonzeroPos = pos + 1;
            if (!small(v)) st.loudPos = pos + 1;
          }
        }
        st.scanPos = w;
        quiet = quiet && static_cast<int32_t>(w - st.loudPos) >= static_cast<int32_t>(reach) && small(d.apY1[0]);
        break;
      }
    }
  }
  if (!quiet) return false;
  for (uint32_t k = g.firstState; k < g.firstState + g.numStates; ++k) {  // flush the ≤ ε residue
    SegState& st = segStates_[k];
    if (st.kind == g::SleepStateKind::Memory) {
      float& v = *reinterpret_cast<float*>(st.ptr);
      if ((std::bit_cast<uint32_t>(v) & 0x7FFFFFFFu) != 0) {
        v = 0.0f;
        ++sleepStats_.flushes;
      }
    } else if (st.kind == g::SleepStateKind::Delay) {
      auto& d = *reinterpret_cast<dsp::DelayState*>(st.ptr);
      const uint32_t reach = std::min(st.tail, d.capacity);
      const uint32_t w = d.writePos;
      if (static_cast<int32_t>(w - st.nonzeroPos) < static_cast<int32_t>(reach)) {
        float* ring = d.ring();
        for (uint32_t pos = w - reach; pos != w; ++pos) ring[pos & d.mask] = 0.0f;
        ++sleepStats_.flushes;
      }
      if ((std::bit_cast<uint32_t>(d.apY1[0]) & 0x7FFFFFFFu) != 0) {
        d.apY1[0] = 0.0f;
        ++sleepStats_.flushes;
      }
      st.nonzeroPos = st.loudPos = w - reach;
    }
  }
  return true;
}

void ImageHost::decideSleep(uint32_t stage, const ProcessContext& ctx, uint32_t frames) noexcept {  // RT-safe
  for (uint32_t gi = stageSegFirst_[stage]; gi < stageSegFirst_[stage + 1]; ++gi) {
    SegRun& g = segs_[gi];
    bool silentIn = true;
    for (uint32_t k = g.firstSource; k < g.firstSource + g.numSources && silentIn; ++k) {
      const SegSource& src = segSources_[k];
      if (src.inSlot != g::kNone && ctx.inSilent != nullptr && src.inSlot < ctx.numIn && ctx.inSilent[src.inSlot] != 0) continue;
      const float* p = src.buf != nullptr ? src.buf : extPtr(src.ext, 0);
      silentIn = allSilent(p, src.perFrame == 0 ? 8u : frames * src.perFrame);
    }
    bool changed = false;
    for (uint32_t k = g.firstParam; k < g.firstParam + g.numParams && !changed; ++k) changed = paramChanged_[segParams_[k]] != 0;
    if (!silentIn || changed) {
      if (g.asleep) ++sleepStats_.wakes;
      g.asleep = false;
      // This block writes the delay lines: count it as loud (the next look starts after it).
      for (uint32_t k = g.firstState; k < g.firstState + g.numStates; ++k) {
        SegState& st = segStates_[k];
        if (st.kind != g::SleepStateKind::Delay) continue;
        const uint32_t w = reinterpret_cast<const dsp::DelayState*>(st.ptr)->writePos + frames;
        st.known = true;
        st.scanPos = st.loudPos = st.nonzeroPos = w;
      }
    } else if (!g.asleep && segmentQuiescent(g)) {
      g.asleep = true;
      ++sleepStats_.sleeps;
    }
    if (g.asleep) ++sleepStats_.asleepBlocks;
    g.skip = g.asleep && sleepMode_ == SleepMode::On;
    *g.flag = g.skip ? 0xFFFFFFFFu : 0u;
  }
}

void ImageHost::sleepReports(ProcessContext& ctx, bool anyVoice, bool outsSilent) noexcept {  // RT-safe
  bool asleep = sleepMode_ != SleepMode::Off || !planSegments_;  // Off: tails are never known to be done
  for (const SegRun& g : segs_) asleep = asleep && g.asleep;
  ctx.tailsDone = asleep;
  ctx.dormant = asleep && outsSilent && !anyVoice && dormantCapable_ && sleepMode_ != SleepMode::Off;
  firstBlock_ = false;
}

// ---- per block --------------------------------------------------------------------------------------------------

float* ImageHost::resolveGlobal(ExtRun& e, const ProcessContext& ctx, uint32_t frames) noexcept {  // RT-safe
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
    case Src::Clip: src = channelOf(ctx.clipIn, ctx.numClipSlots * kClipChannels); break;
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
  if (!e.pattern) return const_cast<float*>(src);  // read-only by contract: ops never write their inputs
  splat8(e.own, src[0]);
  return e.own;
}

bool ImageHost::copyGlobalOut(const OutCopy& c, float* dst, uint32_t frames) noexcept {  // RT-safe
  const float* src = c.src ? c.src : extPtr(c.ext, c.group);
  if (c.srcLanes == 0) {
    for (uint32_t f = 0; f < frames; ++f) dst[f] = src[0];
    return frames == 0 || (std::bit_cast<uint32_t>(src[0]) & 0x7FFFFFFFu) == 0;
  }
  __m256i acc = _mm256_setzero_si256();
  uint32_t f = 0;
  for (; f + 8 <= frames; f += 8) {
    const __m256 v = _mm256_loadu_ps(src + f);
    _mm256_storeu_ps(dst + f, v);
    acc = _mm256_or_si256(acc, _mm256_castps_si256(v));
  }
  uint32_t tail = 0;
  for (; f < frames; ++f) {
    dst[f] = src[f];
    tail |= std::bit_cast<uint32_t>(src[f]);
  }
  return _mm256_testz_si256(acc, _mm256_set1_epi32(0x7FFFFFFF)) != 0 && (tail & 0x7FFFFFFFu) == 0;
}

void ImageHost::copyOut(const OutCopy& c, float* dst, uint32_t frames, bool voice) noexcept {  // RT-safe
  const float* src = c.src ? c.src : extPtr(c.ext, c.group);
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

void ImageHost::zeroOutputs(const ProcessContext& ctx) noexcept {  // RT-safe
  for (uint32_t i = 0; i < ctx.numOut && ctx.out; ++i)
    if (ctx.out[i]) std::memset(ctx.out[i], 0, sizeof(float) * ctx.frames);
  if (ctx.voices && ctx.voices->groups)
    for (uint32_t gi = 0; gi < ctx.voices->numGroups; ++gi) {
      const VoiceGroupIO& gr = ctx.voices->groups[gi];
      for (uint32_t i = 0; i < gr.numOut && gr.out; ++i)
        if (gr.out[i]) std::memset(gr.out[i], 0, sizeof(float) * ctx.frames * 8);
    }
}

void ImageHost::initReducers(const StageRun& st, uint32_t frames, bool zero) noexcept {  // RT-safe
  for (uint32_t k = st.firstReduce; k < st.firstReduce + st.numReduce; ++k) {
    const ReduceInit& ri = reduceInits_[k];
    const uint32_t count = ri.pattern ? 8u : frames;
    const float v = zero ? 0.0f : ri.value;
    for (uint32_t f = 0; f < count; ++f) ri.out[f] = v;
  }
}

bool ImageHost::begin(const ProcessContext& ctx) noexcept {  // RT-safe
  const uint32_t frames = ctx.frames;
  metering_ = false;
  if (frames == 0) return false;
  if (frames > io_.maxBlockFrames) {
    ++rejected_;
    zeroOutputs(ctx);
    return false;
  }
  // T4.5: cost meter. Unbound or not shown: this test and the flag store are all it costs.
  if (meter_ != nullptr && !meterActivated_) {  // M4 review L5: the meter follows the image that runs
    meter_->activate(meterKey_);
    meterActivated_ = true;
  }
  if (meter_ != nullptr && meter_->wants(meterKey_)) {
    metering_ = true;
    meterT0_ = __rdtsc();
    std::memset(regionCycles(), 0, static_cast<size_t>(numRegions_) * 2 * sizeof(uint64_t));
  }
  *meterFlag_ = metering_ ? 1u : 0u;
  run_.sends = ctx.sends;
  run_.numSends = ctx.sends ? ctx.numSends : 0;
  // Params: clamped to the declared range (Q44), NaN → default, splatted once per block. Sleep (T1.x-silence): which
  // of them changed since the previous block.
  // T5.x-perf-audio: the caller vouches that the params are the previous block's, and that block changed none of them
  // (paramChanged_ is all 0 already): nothing to read, clamp or splat.
  if (ctx.paramsUnchanged && !firstBlock_ && !paramsChanged_) {
    resolveExternals(ctx, frames);
    return true;
  }
  bool anyChanged = firstBlock_;
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
    const uint32_t bits = std::bit_cast<uint32_t>(v);
    paramChanged_[i] = firstBlock_ || bits != paramBits_[i] ? 1 : 0;
    anyChanged = anyChanged || bits != paramBits_[i];
    paramBits_[i] = bits;
  }
  paramsChanged_ = anyChanged;
  resolveExternals(ctx, frames);
  return true;
}

void ImageHost::resolveExternals(const ProcessContext& ctx, uint32_t frames) noexcept {  // RT-safe
  const uint32_t G = L_.groups;
  // Engine data → the pointer table.
  const VoiceTable* vt = ctx.voices;
  voices_ = vt;
  groups_ = vt != nullptr && vt->groups != nullptr ? std::min(vt->numGroups, G) : 0u;
  for (uint32_t x = 0; x < exts_.size(); ++x) {
    ExtRun& er = exts_[x];
    if (!er.voice) {
      extPtr_[static_cast<size_t>(x) * G] = resolveGlobal(er, ctx, frames);
      continue;
    }
    for (uint32_t gi = 0; gi < groups_; ++gi) {
      const VoiceGroupIO& gr = vt->groups[gi];
      const float* p = zeros_;
      if (er.src == Src::RootVoice) {
        if (gr.in != nullptr && er.index < gr.numIn && gr.in[er.index] != nullptr) p = gr.in[er.index];
      } else if (er.src == Src::VoiceSig) {
        if (gr.signals != nullptr && gr.signals[er.index] != nullptr) p = gr.signals[er.index];
      }
      extPtr_[static_cast<size_t>(x) * G + gi] = const_cast<float*>(p);  // read-only by contract
    }
  }
}

}  // namespace pq::engine::host
