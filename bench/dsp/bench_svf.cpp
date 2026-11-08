// EQ.1: the `svf` atom (pq/dsp/svf_design.h, kernels_svf.cpp) through findKernel as the interpreter and the JIT call it
// (both call the kernel), 64-frame blocks, white-noise input:
//   svf/<mode> s<N>              param-rate cutoff / q / gain (settled: no design in the timed calls), matched prewarp,
//                                N = 1, 8, 64 stages (64: the double cascade); rows L1 (ns/sample, a global filter) and
//                                L8 (ns/frame of 8 voices, one AVX2 vector per section and frame)
//   svf/lowpass s1 bilinear      the same with the bilinear prewarp (identical per-sample cost: only designs differ)
//   svf/<mode> s<N> audio cutoff the cutoff at audio rate (a design per frame and lane): matched / bilinear, 1 and 8
//                                stages: what a modulated filter (or a param ramp while a knob moves) costs
#include <cmath>
#include <random>
#include <string>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/svf_design.h"
#include "pq_bench.h"

namespace {

constexpr uint32_t kFrames = 64;
using pq::dsp::Atom;
constexpr const char* kModeNames[] = {"lowpass", "highpass", "bandpass", "notch", "bell", "lowshelf", "highshelf", "allpass"};

void benchSvf(pq::bench::Runner& runner, uint32_t mode, uint32_t stages, uint32_t prewarp, bool audioCutoff) {
  std::string name = std::string("svf/") + kModeNames[mode] + " s" + std::to_string(stages);
  if (prewarp) name += " bilinear";
  if (audioCutoff) name += prewarp ? " audio cutoff" : " matched audio cutoff";
  runner.add(name, [=](pq::bench::Context& ctx) {
    pq::AlignedBuffer<float> state(sizeof(pq::dsp::SvfState) / sizeof(float));
    pq::AlignedBuffer<float> in[7], out(kFrames * 8);
    for (auto& b : in) b = pq::AlignedBuffer<float>(kFrames * 8);
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (uint32_t i = 0; i < kFrames * 8; ++i) in[0][i] = u(rng);
    for (uint32_t lanes : {1u, 8u}) {
      for (uint32_t l = 0; l < 8; ++l) {
        in[1][l] = 1000.0f;
        in[2][l] = 0.9f;
        in[3][l] = 2.0f;  // +6 dB (bell, shelves)
        in[4][l] = static_cast<float>(mode);
        in[5][l] = static_cast<float>(stages);
        in[6][l] = static_cast<float>(prewarp);
      }
      if (audioCutoff)  // a sweep per lane, a different cutoff every frame
        for (uint32_t t = 0; t < kFrames; ++t)
          for (uint32_t l = 0; l < lanes; ++l) in[1][t * lanes + l] = 500.0f + 30.0f * static_cast<float>(t) + 7.0f * l;
      pq::dsp::StateInit init;
      init.lanes = lanes;
      pq::dsp::initState(Atom::Svf, state.data(), init);
      const float* ins[7];
      for (uint32_t i = 0; i < 7; ++i) ins[i] = in[i].data();
      float* outs[1] = {out.data()};
      const uint32_t mask = audioCutoff ? 0x7Cu : 0x7Eu;  // x a block; cutoff a block or a pattern
      const pq::dsp::KernelFn fn = pq::dsp::findKernel(Atom::Svf, lanes, mask);
      const pq::dsp::KernelArgs args{ins, outs, state.data(), nullptr, kFrames, 7, 1, mask};
      fn(args);  // the first design is outside the timed calls (param rate: settled from here on)
      ctx.measure([&] {
        fn(args);
        pq::bench::doNotOptimize(out[0]);
      }, kFrames, lanes == 1 ? "ns/sample" : "ns/frame(8 voices)", lanes == 1 ? "L1" : "L8");
    }
  });
}

}  // namespace

void addSvfBenches(pq::bench::Runner& runner) {
  for (uint32_t mode = 0; mode < pq::dsp::kSvfModeCount; ++mode)
    for (uint32_t stages : {1u, 8u, 64u}) {
      if ((mode == 3 || mode == 4) && stages > 1) continue;  // notch, bell: always one section
      benchSvf(runner, mode, stages, 0, false);
    }
  benchSvf(runner, 0, 1, 1, false);
  for (uint32_t prewarp : {0u, 1u}) {
    benchSvf(runner, 0, 1, prewarp, true);
    benchSvf(runner, 0, 8, prewarp, true);
    benchSvf(runner, 4, 1, prewarp, true);
    benchSvf(runner, 5, 1, prewarp, true);
  }
}
