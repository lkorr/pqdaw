// NZ.1: the `noise_step` atom (pq/dsp/atoms.h "Generators", kernels_noise_step.cpp) through findKernel as the
// interpreter and the JIT call it (both call the kernel), 64-frame blocks; rows L1 (ns/sample: a global module) and L8
// (ns/frame of 8 voices). Compare with atom/noise (the plain white noise atom):
//   noise_step/white              Rate at the top (every frame), uniform, colour 0, continuous, Smooth 0: the noise loop
//   noise_step/white gauss        the same, Gaussian (two hashes per step)
//   noise_step/pink               colour −3 dB/oct at audio rate (the 4-section cascade per step)
//   noise_step/brown gauss        colour −6, Gaussian, at audio rate
//   noise_step/dust               density 2 kHz at audio rate (a second hash per step, most steps dropped)
//   noise_step/1 kHz pink glide   a step every 48 frames, colour −3, Smooth 1 (a glide every frame)
//   noise_step/10 Hz hold         slow modulation, stepped: the run skip (a fill between steps)
//   noise_step/10 Hz glide        slow modulation, Smooth 1 (the glide every frame)
//   noise_step/sync 1/16          the Sync clock: transport.beat a block, grid 1/16 (the per-frame scan)
//   noise_step/note trig          the Note clock: voice.trigger a block, one note-on per 4 blocks
#include <string>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/noise_step.h"
#include "pq_bench.h"

namespace {

constexpr uint32_t kFrames = 64;
using pq::dsp::Atom;
enum : uint32_t { kRate, kTrig, kBeat, kGrid, kReset, kSmooth, kColour, kDensity, kSteps, kSeed, kDist, kLanes, kNumIn };

struct Cfg {
  float rate = 1e9f, smooth = 0.0f, colour = 0.0f, density = pq::dsp::kNoiseDensityContinuous, grid = 0.0f;
  bool gauss = false, beatBlock = false, trigBlock = false;
};

void benchNoise(pq::bench::Runner& runner, const std::string& name, Cfg c) {
  runner.add("noise_step/" + name, [c](pq::bench::Context& ctx) {
    constexpr uint32_t kCycle = 4;  // blocks per beat / note-on cycle
    pq::AlignedBuffer<float> state(sizeof(pq::dsp::NoiseStepState) / sizeof(float));
    pq::AlignedBuffer<float> in[kNumIn], out(kFrames * 8), beat(kFrames * 8 * kCycle), trig(kFrames * 8 * kCycle);
    for (auto& b : in) b = pq::AlignedBuffer<float>(kFrames * 8);
    for (uint32_t lanes : {1u, 8u}) {
      for (uint32_t l = 0; l < 8; ++l) {
        in[kRate][l] = c.rate;
        in[kTrig][l] = pq::dsp::boolMask(false);
        in[kBeat][l] = 0.0f;
        in[kGrid][l] = c.grid;
        in[kReset][l] = pq::dsp::boolMask(false);
        in[kSmooth][l] = c.smooth;
        in[kColour][l] = c.colour;
        in[kDensity][l] = c.density;
        in[kSteps][l] = 0.0f;
        in[kSeed][l] = 0.0f;
        in[kDist][l] = c.gauss ? 1.0f : 0.0f;
        in[kLanes][l] = 1.0f;
      }
      // 120 bpm: 1/24000 beat per frame; a note-on at the first frame of every 4th block
      for (uint32_t t = 0; t < kFrames * kCycle; ++t)
        for (uint32_t l = 0; l < lanes; ++l) {
          beat[t * lanes + l] = static_cast<float>(t) / 24000.0f;
          trig[t * lanes + l] = pq::dsp::boolMask(t == 0);
        }
      pq::dsp::StateInit init;
      init.lanes = lanes;
      init.seed = 9;
      pq::dsp::initState(Atom::NoiseStep, state.data(), init);
      const float* ins[kNumIn];
      for (uint32_t i = 0; i < kNumIn; ++i) ins[i] = in[i].data();
      float* outs[1] = {out.data()};
      uint32_t mask = (1u << kNumIn) - 1u;
      if (c.beatBlock) mask &= ~(1u << kBeat);
      if (c.trigBlock) mask &= ~(1u << kTrig);
      const pq::dsp::KernelFn fn = pq::dsp::findKernel(Atom::NoiseStep, lanes, mask);
      const pq::dsp::KernelArgs args{ins, outs, state.data(), nullptr, kFrames, kNumIn, 1, mask};
      uint32_t block = 0;
      auto call = [&] {
        const size_t at = static_cast<size_t>(block % kCycle) * kFrames * lanes;
        if (c.beatBlock) ins[kBeat] = beat.data() + at;
        if (c.trigBlock) ins[kTrig] = trig.data() + at;
        ++block;
        fn(args);
      };
      call();  // the first frame's draw and design outside the timed calls
      ctx.measure([&] {
        call();
        pq::bench::doNotOptimize(out[0]);
      }, kFrames, lanes == 1 ? "ns/sample" : "ns/frame(8 voices)", lanes == 1 ? "L1" : "L8");
    }
  });
}

}  // namespace

void addNoiseBenches(pq::bench::Runner& runner) {
  benchNoise(runner, "white", {});
  benchNoise(runner, "white gauss", {.gauss = true});
  benchNoise(runner, "pink", {.colour = -3.0f});
  benchNoise(runner, "brown gauss", {.colour = -6.0f, .gauss = true});
  benchNoise(runner, "dust", {.density = 2000.0f});
  benchNoise(runner, "1 kHz pink glide", {.rate = 1000.0f, .smooth = 1.0f, .colour = -3.0f});
  benchNoise(runner, "10 Hz hold", {.rate = 10.0f});
  benchNoise(runner, "10 Hz glide", {.rate = 10.0f, .smooth = 1.0f});
  benchNoise(runner, "sync 1/16", {.rate = 0.0f, .grid = 0.25f, .beatBlock = true});
  benchNoise(runner, "note trig", {.rate = 0.0f, .trigBlock = true});
}
