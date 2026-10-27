// Constant evaluation shared by the optimiser and the structural-constant analysis (fold.h).

#include "fold.h"

#include <algorithm>
#include <cmath>

#include "pq/core/denormal_guard.h"
#include "pq/dsp/atoms.h"
#include "pq/graph/kernel_map.h"

namespace pq::graph::detail {

namespace {

// Runs a stateless kernel for one frame and returns lane 0: the value a const- or param-rate evaluation of the node
// produces at runtime. Those nodes run in Control regions, which both backends execute with the 8-lane kernels
// (schedule.h; the JIT calls the same kernels there), so folding uses the 8-lane kernel too: the 1-lane kernels
// call the CRT for the exact transcendentals and differ from the 8-lane SVML ones by an ulp on about a third of
// the arguments (M1 review M4). FTZ/DAZ as on the audio thread.
bool evalKernel(dsp::Atom k, const float* in, uint32_t nIn, float* out, uint32_t nOut) {
  constexpr uint32_t kLanes = 8;
  static_assert(dsp::kPatternFloats == kLanes, "one Blk frame of 8 lanes fills one pattern slot");
  const dsp::KernelFn fn = dsp::findKernel(k, kLanes, 0);
  if (!fn) return false;
  struct alignas(32) Slot {
    float v[dsp::kPatternFloats];
  };
  Slot ib[kMaxFoldPorts], ob[kMaxFoldPorts];
  const float* ip[kMaxFoldPorts];
  float* op[kMaxFoldPorts];
  for (uint32_t i = 0; i < nIn; ++i) {
    std::fill(std::begin(ib[i].v), std::end(ib[i].v), in[i]);
    ip[i] = ib[i].v;
  }
  for (uint32_t j = 0; j < nOut; ++j) op[j] = ob[j].v;
  dsp::KernelArgs a;
  a.in = ip;
  a.out = op;
  a.frames = 1;
  a.numIn = static_cast<uint16_t>(nIn);
  a.numOut = static_cast<uint16_t>(nOut);
  a.splMask = 0;
  {
    DenormalGuard guard;
    fn(a);
  }
  for (uint32_t j = 0; j < nOut; ++j) out[j] = ob[j].v[0];
  return true;
}

}  // namespace

bool foldPureAtom(AtomId atom, const float* in, uint32_t nIn, float* out, uint32_t nOut) {
  if (nIn > kMaxFoldPorts || nOut > kMaxFoldPorts || nOut == 0) return false;
  const KernelMapping& km = kernelMapping(atom);
  if (km.lowering != Lowering::Kernel) return false;
  const dsp::KernelInfo& ki = dsp::kernelInfo(km.kernel);
  if (ki.resource != dsp::Resource::None || ki.splRequired != 0 || ki.stateSize != 0) return false;
  for (const ArgMap& am : km.args)
    if (am.use != ArgUse::Input) return false;
  return evalKernel(km.kernel, in, nIn, out, nOut);
}

uint32_t muxPick(float index, uint32_t count) noexcept {
  if (count == 0 || std::isnan(index)) return 0;
  const float f = std::floor(index);
  return f <= 0.0f ? 0u : f >= static_cast<float>(count - 1) ? count - 1 : static_cast<uint32_t>(f);
}

}  // namespace pq::graph::detail
