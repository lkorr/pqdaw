// Host half of a JIT-compiled program (jit_image.h): the shared image host (pq/engine/image_host.h) driving the
// generated stage functions. process() is the only RT code here.

#include "jit_image.h"

#include <algorithm>
#include <string>

#include "pq/dsp/atoms.h"

namespace pq::jit::detail {

namespace g = pq::graph;
namespace e = pq::engine;

namespace {
constexpr uint32_t kFloatAlign = 16;  // 64 bytes
uint32_t roundUp(uint32_t n, uint32_t a) noexcept { return (n + a - 1) / a * a; }
}  // namespace

JitImage::~JitImage() = default;

void JitImage::process(e::ProcessContext& ctx) noexcept {  // RT-safe
  if (!host_.begin(ctx)) return;
  float* buf = host_.buf();
  const float* aux = host_.aux();
  std::byte* gstate = host_.globalState();
  std::byte* vstate = host_.voiceState(0);
  float* const* ext = host_.extTable();
  const void* const* tab = tab_.data();
  const uint32_t frames = ctx.frames;
  const size_t stride = host_.layout().voiceStride;
  auto global = [&](uint32_t k) noexcept { fns_[k](buf, aux, gstate, vstate, nullptr, ext, tab, frames, 0, 0, 0); };
  if (scalarLanes_) {  // the benchmark baseline: one call per voice lane
    host_.run(ctx, global, [&](uint32_t k, uint32_t gi) noexcept {
      for (uint32_t lane = 0; lane < 8; ++lane)
        fns_[k](buf, aux, gstate, vstate + gi * stride, nullptr, ext, tab, frames, gi, 0, lane);
    });
    return;
  }
  host_.run(
      ctx, global,
      [&](uint32_t k, uint32_t gi) noexcept {
        fns_[k](buf, aux, gstate, vstate + gi * stride, nullptr, ext, tab, frames, gi, 0, 0);
      },
      [&](uint32_t k, uint32_t a, uint32_t b) noexcept {
        pairFns_[k](buf, aux, gstate, vstate + a * stride, vstate + b * stride, ext, tab, frames, a, b, 0);
      });
}

bool inlineStateful(dsp::Atom a) noexcept {
  switch (a) {
    case dsp::Atom::Schmitt: case dsp::Atom::Rise: case dsp::Atom::Fall: case dsp::Atom::Change:
    case dsp::Atom::SrFlipFlop: case dsp::Atom::DFlipFlop: case dsp::Atom::TFlipFlop: case dsp::Atom::Counter:
    case dsp::Atom::ClockDiv: case dsp::Atom::Pulse: case dsp::Atom::Timer: case dsp::Atom::Z1:
    case dsp::Atom::Z1Read: case dsp::Atom::Z1Write: case dsp::Atom::SampleHold: case dsp::Atom::Latch:
    case dsp::Atom::Phasor: case dsp::Atom::SineHz:
      return true;
    default:
      return false;
  }
}

void finishPairs(JitImage& img, const g::Schedule& s) {
  std::vector<uint8_t> paired(s.stages.size(), 0);
  for (uint32_t k = 0; k < s.stages.size() && k < img.pairFns_.size(); ++k) paired[k] = img.pairFns_[k] != nullptr;
  const HostLayout& L = img.layout_;
  img.host_.enablePairs(paired, [&](uint32_t k, uint32_t b) -> float* {
    if (b >= L.altOff.size() || L.altOff[b] == g::kNone || !stageLocalBuffers(s, k)[b]) return nullptr;
    return img.host_.buf() + L.altOff[b];
  });
}

std::vector<uint8_t> stageLocalBuffers(const g::Schedule& s, uint32_t stage) {
  std::vector<uint8_t> local(s.buffers.size(), 0);
  const g::Stage& st = s.stages[stage];
  if (st.poly != g::Poly::Voice) return local;
  auto mark = [&](const g::Operand& o) {
    if (o.kind != g::Operand::Kind::Buffer || o.index >= s.buffers.size()) return;
    const g::Buffer& b = s.buffers[o.index];
    if (!b.perGroup && (b.cls == g::Buffer::Class::Pattern || b.lanes == 8)) local[o.index] = 1;
  };
  for (uint32_t ri = st.firstRegion; ri < st.firstRegion + st.numRegions && ri < s.regions.size(); ++ri) {
    const g::Region& reg = s.regions[ri];
    for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps && i < s.ops.size(); ++i)
      if (s.ops[i].kind != g::OpKind::Reduce)  // a reducer's output is the global accumulator, shared by the groups
        for (const g::Operand& o : s.outputsOf(s.ops[i])) mark(o);
    for (uint32_t k = reg.firstSolve; k < reg.firstSolve + reg.numSolves && k < s.solves.size(); ++k) {
      const g::SolveBlock& sb = s.solves[k];
      for (uint32_t u = sb.firstUnknown; u < sb.firstUnknown + sb.numUnknowns && u < s.unknowns.size(); ++u)
        mark(s.unknowns[u].value);
    }
  }
  return local;
}

Result<std::unique_ptr<JitImage>> buildHost(const g::Schedule& s, uint32_t maxBlockFrames, uint32_t maxVoiceGroups,
                                            const e::Resources* resources, bool pairs, PtrTable& tab,
                                            e::SleepMode sleep) {
  // The JIT's scratch after the buffers: Newton (n > 1) and the pointer arrays of wide kernel calls (codegen.h).
  uint32_t maxN = 0;
  for (const g::SolveBlock& sb : s.solves)
    if (sb.numUnknowns <= s.unknowns.size()) maxN = std::max(maxN, sb.numUnknowns);  // larger ones fail validation
  const uint32_t solveFloats = maxN > 0 ? roundUp(maxN * 8 * 2 + maxN * maxN * 8 + 2 * maxN * (maxN + 1) + 16, kFloatAlign) : 0u;
  uint32_t wide = 0;
  for (const g::Op& op : s.ops)
    if (op.numIn > kStackCallOperands || op.numOut > kStackCallOperands) wide = std::max<uint32_t>(wide, op.numIn + op.numOut);
  const uint32_t callFloats = roundUp(wide * 2, kFloatAlign);  // 8-byte pointers
  // Pairs (T1b.2): voice stages without sends, when there are at least two groups to pair; the second group's copy
  // of each of such a stage's own voice values (sized as the host sizes buffers: image_host.cpp layoutMemory).
  std::vector<uint8_t> pairable(s.stages.size(), 0);
  std::vector<uint32_t> altOff(s.buffers.size(), g::kNone);
  uint32_t altFloats = 0;
  if (pairs && maxVoiceGroups >= 2 && maxBlockFrames >= 1 && maxBlockFrames <= (1u << 20)) {
    for (uint32_t k = 0; k < s.stages.size(); ++k) {
      const g::Stage& st = s.stages[k];
      if (st.poly != g::Poly::Voice || st.firstRegion + st.numRegions > s.regions.size()) continue;
      bool ok = true;
      for (uint32_t ri = st.firstRegion; ri < st.firstRegion + st.numRegions && ok; ++ri)
        for (uint32_t i = s.regions[ri].firstOp; i < s.regions[ri].firstOp + s.regions[ri].numOps && ok; ++i)
          ok = i < s.ops.size() && s.ops[i].kind != g::OpKind::Send;
      if (!ok) continue;
      pairable[k] = 1;
      const std::vector<uint8_t> local = stageLocalBuffers(s, k);
      for (uint32_t b = 0; b < s.buffers.size(); ++b) {
        if (!local[b] || altOff[b] != g::kNone) continue;
        altOff[b] = altFloats;  // relative to the pair block for now
        altFloats += roundUp(s.buffers[b].cls == g::Buffer::Class::Pattern ? 8u : maxBlockFrames * s.buffers[b].lanes, kFloatAlign);
      }
    }
  }

  auto img = std::make_unique<JitImage>();
  e::host::ImageHost& host = img->host_;
  e::host::BuildOptions bo;
  bo.who = "jit";
  bo.backend = e::ImageBackend::Jit;
  bo.maxBlockFrames = maxBlockFrames;
  bo.maxVoiceGroups = maxVoiceGroups;
  bo.resources = resources;
  bo.sleep = sleep;
  bo.extraBufFloats = solveFloats + callFloats + altFloats;
  if (Status st = host.build(s, bo); !st.ok()) return st.error();

  // Every kernel the generated code calls must exist for the lanes it uses (Control regions: 8-float patterns).
  for (const g::Stage& st : s.stages)
    for (uint32_t ri = st.firstRegion; ri < st.firstRegion + st.numRegions; ++ri) {
      const g::Region& reg = s.regions[ri];
      const uint32_t lanesOverride = reg.kind == g::RegionKind::Control ? 8u : 0u;
      for (uint32_t i = reg.firstOp; i < reg.firstOp + reg.numOps; ++i) {
        const g::Op& op = s.ops[i];
        if (op.kind != g::OpKind::Kernel) continue;
        const uint32_t lanes = lanesOverride ? lanesOverride : op.lanes;
        if (!dsp::findKernel(op.kernel, lanes, op.splMask))
          return host.error(std::string("no ") + dsp::kernelInfo(op.kernel).name + " kernel for " + std::to_string(lanes) +
                            " lanes with Spl mask " + std::to_string(op.splMask));
      }
    }

  const e::host::Layout& HL = host.layout();
  HostLayout& L = img->layout_;
  L.maxBlock = HL.maxBlock;
  L.groups = HL.groups;
  L.bufOff = HL.bufOff;
  L.bufSize = HL.bufSize;
  L.maxSolveN = HL.maxSolveN;
  L.solveOff = HL.extraOff;
  L.callOff = HL.extraOff + solveFloats;
  L.callPtrs = wide;
  L.paramOff = HL.paramOff;
  L.constOff = HL.constOff;
  L.sleepOff = HL.sleepOff;
  L.pairable = pairable;
  L.altOff = altOff;
  for (uint32_t& a : L.altOff)
    if (a != g::kNone) a += HL.extraOff + solveFloats + callFloats;
  // Resources and special-op records: one table slot per op that needs one.
  L.opInfo.assign(s.ops.size(), g::kNone);
  L.opInfoB.assign(s.ops.size(), g::kNone);
  for (uint32_t i = 0; i < s.ops.size(); ++i) {
    const g::Op& op = s.ops[i];
    switch (op.kind) {
      case g::OpKind::Kernel: {
        const dsp::Resource r = dsp::kernelInfo(op.kernel).resource;
        if (r != dsp::Resource::None) L.opInfo[i] = tab.slot(host.resource(r, op.resource));
        break;
      }
      case g::OpKind::Reduce:
        if (const auto* ri = host.reduceInfo(i)) L.opInfo[i] = tab.slot(ri);
        if (const auto* rb = host.reduceInfoB(i)) L.opInfoB[i] = tab.slot(rb);
        break;
      case g::OpKind::Send:
        if (const auto* si = host.sendInfo(i)) L.opInfo[i] = tab.slot(si);
        break;
      case g::OpKind::Broadcast: break;
    }
  }
  img->fns_.assign(s.stages.size(), nullptr);
  img->pairFns_.assign(s.stages.size(), nullptr);
  return img;
}

}  // namespace pq::jit::detail
