// TMM.8: the Wavetable's live path and Shape mode (pq/dsp/wave_live.h), 48 kHz, 64-frame blocks, 8 voices (L8), through
// findKernel as the interpreter calls them; ns per BASE-rate frame of 8 voices (divide by 8 for one voice):
//   wavelive/region os N (pw + sync + fold)  the whole live region at ×N: the phase up (oversample_phase), wavetable_live
//                                            at N·64 frames (Pulse width, warp / sync / fold modulated per sample,
//                                            corrections on), oversample_down — what a modulated Wavetable pays
//   wavelive/atom os 4 (bend only, no corr)  the phase up and the kernel at ×4 frames (no decimation): warp modulated,
//                                            no sync, no corrections
//   wavelive/atom os 4 (fold only)           the same with fold modulated (two sines per sample)
//   wavelive/shape region os 2 (ADAA)        Shape mode's region at ×2: oversample_up, wavetable_shape, oversample_down
//   wavelive/shape atom (ADAA)               wavetable_shape alone at the base rate
//   wavelive/shaper atom (plain, no ADAA)    the plain `shaper` atom at the base rate (a 1025-point curve), for scale
#include <cmath>
#include <memory>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/dsp/frame_table_slot.h"
#include "pq/dsp/tables.h"
#include "pq_bench.h"

namespace {

constexpr uint32_t kFrames = 64;
constexpr double kRate = 48000.0;
using pq::dsp::Atom;

std::shared_ptr<const pq::dsp::FrameTable> table(bool antiderivative) {
  pq::dsp::FrameTableBuilder b;
  const auto saw = pq::dsp::basicCycle(pq::dsp::BasicCycle::Saw);
  const auto sq = pq::dsp::basicCycle(pq::dsp::BasicCycle::Square);
  const pq::dsp::Keyframe keys[] = {{0.0f, saw.span()}, {1.0f, sq.span()}};
  pq::dsp::FrameTableSpec spec;
  spec.keyframes = keys;
  spec.sampleRate = kRate;
  spec.antiderivative = antiderivative;
  return std::make_shared<const pq::dsp::FrameTable>(b.build(spec).value());
}

struct Kernel {
  Atom atom;
  pq::AlignedBuffer<float> state;
  pq::dsp::KernelFn fn;
  Kernel(Atom a, uint32_t mask, double sr)
      : atom(a), state(std::max<size_t>(pq::dsp::kernelInfo(a).stateSize / 4, 8)), fn(pq::dsp::findKernel(a, 8, mask)) {
    pq::dsp::StateInit si;
    si.lanes = 8;
    si.sampleRate = sr;
    pq::dsp::initState(a, state.data(), si);
  }
};

}  // namespace

void addWaveLiveBenches(pq::bench::Runner& runner) {
  static pq::dsp::FrameTableSlot* phaseSlot = new pq::dsp::FrameTableSlot(table(false));
  static pq::dsp::FrameTableSlot* shapeSlot = new pq::dsp::FrameTableSlot(table(true));
  // Base-rate inputs, a ring of 16 blocks: 8 voices 10 % apart from 220 Hz, a modulator per voice.
  constexpr uint32_t kBlocks = 16;
  auto phases = [] {
    std::vector<float> ph(kBlocks * kFrames * 8);
    double acc[8] = {};
    for (uint32_t n = 0; n < kBlocks * kFrames; ++n)
      for (uint32_t l = 0; l < 8; ++l) {
        acc[l] += 220.0 * (1.0 + 0.1 * l) / kRate;
        acc[l] -= std::floor(acc[l]);
        ph[n * 8 + l] = static_cast<float>(acc[l]);
      }
    return ph;
  };
  auto region = [phases](uint32_t os, int what) {
    return [os, what, phases](pq::bench::Context& c) {
      const std::vector<float> ph = phases();
      const uint32_t m = kFrames * os;
      // Controls at the oversampled rate (per sample, per voice).
      pq::AlignedBuffer<float> w(m * 8), f(m * 8), s(m * 8), up(m * 8), y(m * 8), out(kFrames * 8);
      for (uint32_t i = 0; i < m; ++i)
        for (uint32_t l = 0; l < 8; ++l) {
          const double t = static_cast<double>(i) / os / kRate;
          w[i * 8 + l] = static_cast<float>(std::sin(2.0 * 3.14159265 * 2000.0 * t + l));
          f[i * 8 + l] = static_cast<float>(what == 2 ? 0.5 + 0.5 * std::sin(2.0 * 3.14159265 * 1500.0 * t + l) : 0.2);
          s[i * 8 + l] = static_cast<float>(0.3 + 0.3 * std::sin(2.0 * 3.14159265 * 900.0 * t + l));
        }
      alignas(32) float factor[8], zero[8] = {}, modeV[8], corrV[8], pos[8];
      for (uint32_t l = 0; l < 8; ++l) {
        factor[l] = static_cast<float>(os);
        modeV[l] = what == 0 ? 2.0f : 0.0f;  // Pulse width; Bend
        corrV[l] = what == 0 ? 1.0f : 0.0f;
        pos[l] = 0.13f + 0.097f * l;
      }
      Kernel phUp(Atom::OversamplePhase, 2u, kRate), down(Atom::OversampleDown, 2u, kRate);
      // what 0: warp + sync + fold per sample (Spl mask: position, mode, corr); 1: warp only; 2: fold only.
      const uint32_t mask = (1u << 1) | (1u << 5) | (1u << 6) | (what == 1 ? (1u << 3) | (1u << 4) : 0u) |
                            (what == 2 ? (1u << 2) | (1u << 4) : 0u);
      Kernel live(Atom::WavetableLive, mask, kRate * os);
      const bool full = what == 0;
      uint32_t blk = 0;
      c.measure(
          [&] {
            const float* p = ph.data() + static_cast<size_t>(blk) * kFrames * 8;
            {
              const float* ui[2] = {p, factor};
              float* uo[1] = {up.data()};
              phUp.fn(pq::dsp::KernelArgs{ui, uo, phUp.state.data(), nullptr, m, 2, 1, 2u});
            }
            const float* li[7] = {up.data(), pos, what == 2 ? zero : w.data(), what == 1 ? zero : f.data(),
                                  what == 0 ? s.data() : zero, modeV, corrV};
            float* lo[1] = {y.data()};
            live.fn(pq::dsp::KernelArgs{li, lo, live.state.data(), phaseSlot, m, 7, 1, mask});
            if (full) {
              const float* di[2] = {y.data(), factor};
              float* dout[1] = {out.data()};
              down.fn(pq::dsp::KernelArgs{di, dout, down.state.data(), nullptr, m, 2, 1, 2u});
            }
            blk = (blk + 1) % kBlocks;
            pq::bench::doNotOptimize(y[0]);
          },
          kFrames, "ns/frame(8 voices)");
    };
  };
  runner.add("wavelive/region os 2 (pw + sync + fold)", region(2, 0));
  runner.add("wavelive/region os 4 (pw + sync + fold)", region(4, 0));
  runner.add("wavelive/region os 8 (pw + sync + fold)", region(8, 0));
  runner.add("wavelive/region os 16 (pw + sync + fold)", region(16, 0));
  runner.add("wavelive/atom os 4 (bend only, no corr)", region(4, 1));
  runner.add("wavelive/atom os 4 (fold only)", region(4, 2));

  auto shape = [](int what) {
    return [what](pq::bench::Context& c) {
      constexpr uint32_t os = 2;
      const uint32_t m = kFrames * os;
      pq::AlignedBuffer<float> x(kBlocks * kFrames * 8), up(m * 8), y(m * 8), out(kFrames * 8);
      for (uint32_t n = 0; n < kBlocks * kFrames; ++n)
        for (uint32_t l = 0; l < 8; ++l)
          x[n * 8 + l] = static_cast<float>(0.9 * std::sin(2.0 * 3.14159265 * 300.0 * (1.0 + 0.1 * l) * n / kRate));
      alignas(32) float factor[8], pos[8];
      for (uint32_t l = 0; l < 8; ++l) {
        factor[l] = static_cast<float>(os);
        pos[l] = 0.13f + 0.097f * l;
      }
      std::vector<float> pts(1025);
      for (size_t i = 0; i < pts.size(); ++i) pts[i] = static_cast<float>(std::tanh(-3.0 + 6.0 * i / 1024.0));
      static const pq::dsp::ShaperTable sh = pq::dsp::ShaperTable::build(pts, -1.0f, 1.0f).value();
      Kernel xUp(Atom::OversampleUp, 2u, kRate), down(Atom::OversampleDown, 2u, kRate);
      Kernel sk(Atom::WavetableShape, 2u, what == 0 ? kRate * os : kRate);
      const pq::dsp::KernelFn shaper = pq::dsp::findKernel(Atom::Shaper, 8, 0u);
      uint32_t blk = 0;
      c.measure(
          [&] {
            const float* p = x.data() + static_cast<size_t>(blk) * kFrames * 8;
            if (what == 0) {
              const float* ui[2] = {p, factor};
              float* uo[1] = {up.data()};
              xUp.fn(pq::dsp::KernelArgs{ui, uo, xUp.state.data(), nullptr, m, 2, 1, 2u});
              const float* si[2] = {up.data(), pos};
              float* so[1] = {y.data()};
              sk.fn(pq::dsp::KernelArgs{si, so, sk.state.data(), shapeSlot, m, 2, 1, 2u});
              const float* di[2] = {y.data(), factor};
              float* dout[1] = {out.data()};
              down.fn(pq::dsp::KernelArgs{di, dout, down.state.data(), nullptr, m, 2, 1, 2u});
            } else if (what == 1) {
              const float* si[2] = {p, pos};
              float* so[1] = {y.data()};
              sk.fn(pq::dsp::KernelArgs{si, so, sk.state.data(), shapeSlot, kFrames, 2, 1, 2u});
            } else {
              const float* si[1] = {p};
              float* so[1] = {y.data()};
              shaper(pq::dsp::KernelArgs{si, so, nullptr, &sh.view(), kFrames, 1, 1, 0u});
            }
            blk = (blk + 1) % kBlocks;
            pq::bench::doNotOptimize(y[0]);
          },
          kFrames, "ns/frame(8 voices)");
    };
  };
  runner.add("wavelive/shape region os 2 (ADAA)", shape(0));
  runner.add("wavelive/shape atom (ADAA)", shape(1));
  runner.add("wavelive/shaper atom (plain, no ADAA)", shape(2));
}
