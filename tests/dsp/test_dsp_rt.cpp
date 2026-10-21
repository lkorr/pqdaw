// RtScope allocation-trap tests: every leaf-atom kernel (every atom × lane count × Spl combination) and
// initState run without heap allocation.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/tables.h"

using pq::alloc_trap::countRtViolations;
using pq::dsp::Atom;

TEST_CASE("rt: every atom kernel and initState are allocation-free") {
  REQUIRE(pq::alloc_trap::installed());
  constexpr uint32_t kFrames = 67;  // not a multiple of 8: exercises the scalar tails too
  constexpr uint32_t kMaxIn = 5, kMaxOut = 4;

  // Resources and buffers are built outside the RtScope (that allocates by design).
  const auto cycle = pq::dsp::Wavetable::basicCycle(pq::dsp::BasicShape::Saw, 256);
  auto wt = pq::dsp::Wavetable::build(cycle.span());
  REQUIRE(wt.ok());
  std::vector<float> pts = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
  auto sh = pq::dsp::ShaperTable::build(pts, -1.0f, 1.0f);
  REQUIRE(sh.ok());
  std::vector<float> smp(300, 0.25f);
  auto sd = pq::dsp::SampleData::build(smp);
  REQUIRE(sd.ok());

  std::vector<pq::AlignedBuffer<float>> in, out;
  for (uint32_t i = 0; i < kMaxIn; ++i) in.emplace_back(static_cast<size_t>(kFrames) * 8);
  for (uint32_t o = 0; o < kMaxOut; ++o) out.emplace_back(static_cast<size_t>(kFrames) * 8);
  pq::AlignedBuffer<float> state(1 << 16);
  const float* inPtrs[kMaxIn];
  float* outPtrs[kMaxOut];
  for (uint32_t o = 0; o < kMaxOut; ++o) outPtrs[o] = out[o].data();

  uint64_t kernelsRun = 0;
  for (uint32_t ai = 0; ai < pq::dsp::kAtomCount; ++ai) {
    const auto a = static_cast<Atom>(ai);
    const auto& info = pq::dsp::kernelInfo(a);
    const uint32_t nIn = info.variadicIn ? 4u : info.numIn;
    const uint32_t nOut = info.variadicOut ? 3u : info.numOut;
    for (uint32_t i = 0; i < nIn; ++i) {
      const bool isBool = (info.boolIn >> i) & 1u;
      for (uint32_t k = 0; k < kFrames * 8; ++k)
        in[i][k] = isBool ? pq::dsp::boolMask((k / 8) % 3 == 0) : (i == 0 ? 0.25f + 0.001f * static_cast<float>(k % 50) : 2.0f);
      inPtrs[i] = in[i].data();
    }
    const void* resource = info.resource == pq::dsp::Resource::Wavetable ? static_cast<const void*>(&wt->view())
                           : info.resource == pq::dsp::Resource::Shaper  ? static_cast<const void*>(&sh->view())
                           : info.resource == pq::dsp::Resource::Sample  ? static_cast<const void*>(&sd->view())
                                                                         : nullptr;
    const uint32_t specMax = info.variadicIn ? 15u : info.splSpecMask;
    for (uint32_t lanes : {1u, 8u}) {
      pq::dsp::StateInit init;
      init.lanes = lanes;
      init.maxDelayFrames = 64;
      init.seed = ai;
      REQUIRE(pq::dsp::stateBytes(a, init) <= state.size() * sizeof(float));
      for (uint32_t m = 0; m <= specMax; ++m) {
        const uint32_t splMask = m | info.splRequired;
        const pq::dsp::KernelFn fn = pq::dsp::findKernel(a, lanes, splMask);
        REQUIRE(fn != nullptr);
        pq::dsp::KernelArgs args{inPtrs, outPtrs, state.data(), resource, kFrames, static_cast<uint16_t>(nIn),
                                 static_cast<uint16_t>(nOut), splMask};
        bool lookupsOk = false;
        const uint64_t violations = countRtViolations([&] {
          // Lookups are RT-safe too (table reads), so a voice or region can switch kernels at a block boundary.
          lookupsOk = pq::dsp::findKernel(a, lanes, splMask) == fn && pq::dsp::kernelInfo(a).name == info.name &&
                      (pq::dsp::stateBytes(a, init) != 0) == (info.stateSize != 0);
          pq::dsp::initState(a, state.data(), init);
          fn(args);
          fn(args);
          args.frames = 0;
          fn(args);
        });
        CHECK(lookupsOk);
        CHECK_MESSAGE(violations == 0, std::string(info.name) << " lanes=" << lanes << " spl=" << splMask);
        ++kernelsRun;
      }
    }
  }
  CHECK(kernelsRun > 400);
}
