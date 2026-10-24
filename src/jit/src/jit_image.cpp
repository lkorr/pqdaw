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
  host_.run(
      ctx, [&](uint32_t k) noexcept { fns_[k](buf, aux, gstate, vstate, ext, tab, frames, 0); },
      [&](uint32_t k, uint32_t gi) noexcept { fns_[k](buf, aux, gstate, vstate + gi * stride, ext, tab, frames, gi); });
}

Result<std::unique_ptr<JitImage>> buildHost(const g::Schedule& s, uint32_t maxBlockFrames, uint32_t maxVoiceGroups,
                                            const e::Resources* resources, PtrTable& tab) {
  // The JIT's scratch after the buffers: Newton (n > 1) and the pointer arrays of wide kernel calls (codegen.h).
  uint32_t maxN = 0;
  for (const g::SolveBlock& sb : s.solves)
    if (sb.numUnknowns <= s.unknowns.size()) maxN = std::max(maxN, sb.numUnknowns);  // larger ones fail validation
  const uint32_t solveFloats = maxN > 0 ? roundUp(maxN * 8 * 2 + maxN * maxN * 8 + 2 * maxN * (maxN + 1) + 16, kFloatAlign) : 0u;
  uint32_t wide = 0;
  for (const g::Op& op : s.ops)
    if (op.numIn > kStackCallOperands || op.numOut > kStackCallOperands) wide = std::max<uint32_t>(wide, op.numIn + op.numOut);
  const uint32_t callFloats = roundUp(wide * 2, kFloatAlign);  // 8-byte pointers

  auto img = std::make_unique<JitImage>();
  e::host::ImageHost& host = img->host_;
  e::host::BuildOptions bo;
  bo.who = "jit";
  bo.backend = e::ImageBackend::Jit;
  bo.maxBlockFrames = maxBlockFrames;
  bo.maxVoiceGroups = maxVoiceGroups;
  bo.resources = resources;
  bo.extraBufFloats = solveFloats + callFloats;
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
  // Resources and special-op records: one table slot per op that needs one.
  L.opInfo.assign(s.ops.size(), g::kNone);
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
        break;
      case g::OpKind::Send:
        if (const auto* si = host.sendInfo(i)) L.opInfo[i] = tab.slot(si);
        break;
      case g::OpKind::Broadcast: break;
    }
  }
  img->fns_.assign(s.stages.size(), nullptr);
  return img;
}

}  // namespace pq::jit::detail
