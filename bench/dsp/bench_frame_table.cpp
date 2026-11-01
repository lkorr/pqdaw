// TMM.1: frame-table reads and builds (pq/dsp/frame_table.h, frame_table_build.h), 48 kHz tables.
//   frametable/read L1 (blend)          a 64-frame saw → square table, one signal at 624 Hz, position swept per sample
//                                       (every step blends two frames); ns/sample
//   frametable/read L1 (single frame)   the same, position 0 (the one-frame path); ns/sample
//   frametable/read L8 (blend)          8 voices 10 % apart in pitch from 624 Hz, each at its own fractional
//                                       position (a Spl pattern); ns per 8-voice frame
//   frametable/read L8 (single frame)   the same voices, position 0
//   frametable/build 64 frames          the 64-keyframe drawn fixture → 64 dense frames, no bake; ms per build
//   frametable/build 64 frames + bake   the same with one baked control (fold 0.5; 16×); ms per build
//   frametable/build 64 frames + bake3  Mirror 0.5, fold 0.3 and sync 0.3 together (the builder raises it to 32×)
// Builds run on one thread (the caller's), into a preallocated table (the builder's arenas sized by a first build).
// TMM.2: the atoms over a live slot, through findKernel as the interpreter calls them (64-frame blocks):
//   atom/wavetable_pos L1 | L8           the reads above plus the slot's acquire load, the state copy and the tap test;
//                                        L1: per-sample position (blend), L8: per-voice positions (a Spl pattern)
//   atom/wavetable_pos … (tap read)      the same with a registered tap reader (8 samples per block written)
//   atom/wavetable_pos … (crossfade)     every block inside a swap's 64-sample fade (two reads + the mix)
//   atom/voice_tap L1 | L8 (…)           unwatched (one relaxed load) and watched
#include <chrono>
#include <cmath>
#include <random>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/frame_table_slot.h"
#include "pq/core/voice_tap.h"
#include <memory>
#include "pq_bench.h"

namespace {

constexpr uint32_t kFrames = 64;
constexpr double kRate = 48000.0;

std::vector<std::vector<float>> drawn(uint32_t count) {
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> step(-1.0, 1.0);
  std::vector<std::vector<float>> out;
  for (uint32_t k = 0; k < count; ++k) {
    std::vector<double> w(pq::dsp::kKeyframeCycle);
    double acc = 0, peak = 0;
    for (double& v : w) v = acc += step(rng);
    const double drift = (w.back() - w.front()) / (w.size() - 1);
    for (std::size_t i = 0; i < w.size(); ++i) peak = std::max(peak, std::fabs(w[i] -= drift * i));
    std::vector<float> c(w.size());
    for (std::size_t i = 0; i < w.size(); ++i) c[i] = static_cast<float>(w[i] / peak);
    out.push_back(std::move(c));
  }
  return out;
}

struct Tables {
  pq::dsp::FrameTable morph;
  Tables() {
    const auto saw = pq::dsp::basicCycle(pq::dsp::BasicCycle::Saw);
    const auto sq = pq::dsp::basicCycle(pq::dsp::BasicCycle::Square);
    const pq::dsp::Keyframe keys[] = {{0.0f, saw.span()}, {1.0f, sq.span()}};
    pq::dsp::FrameTableSpec spec;
    spec.keyframes = keys;
    spec.sampleRate = kRate;
    pq::dsp::FrameTableBuilder b;
    morph = b.build(spec).value();
  }
};

}  // namespace

void addFrameTableBenches(pq::bench::Runner& runner) {
  static Tables tables;
  auto readL1 = [](bool blend) {
    return [blend](pq::bench::Context& c) {
      // Precomputed phases and positions (a ring of 16 blocks), so the timed call is the read alone.
      constexpr uint32_t kBlocks = 16;
      pq::AlignedBuffer<float> phase(kBlocks * kFrames), pos(kBlocks * kFrames), out(kFrames);
      double acc = 0.1;
      for (uint32_t n = 0; n < kBlocks * kFrames; ++n) {
        acc += 624.0 / kRate;
        acc -= std::floor(acc);
        phase[n] = static_cast<float>(acc);
        pos[n] = blend ? 0.1f + 0.8f * static_cast<float>(n) / (kBlocks * kFrames) : 0.0f;
      }
      pq::dsp::FrameReadState st{};
      uint32_t blk = 0;
      c.measure(
          [&] {
            const std::size_t o = static_cast<std::size_t>(blk) * kFrames;
            pq::dsp::readFrameTableL1(tables.morph.view(), st, phase.data() + o, pos.data() + o, true, kFrames,
                                      out.data());
            blk = (blk + 1) % kBlocks;
            pq::bench::doNotOptimize(out[kFrames - 1]);
          },
          kFrames, "ns/sample");
    };
  };
  runner.add("frametable/read L1 (blend)", readL1(true));
  runner.add("frametable/read L1 (single frame)", readL1(false));
  auto readL8 = [](bool blend) {
    return [blend](pq::bench::Context& c) {
      // Precomputed phases (a ring of 16 blocks), so the timed call is the read alone.
      constexpr uint32_t kBlocks = 16;
      pq::AlignedBuffer<float> phase(kBlocks * kFrames * 8), out(kFrames * 8);
      alignas(32) float pos[8];
      for (uint32_t l = 0; l < 8; ++l) pos[l] = blend ? 0.13f + 0.097f * l : 0.0f;
      double acc[8];
      for (uint32_t l = 0; l < 8; ++l) acc[l] = 0.1 * l;
      for (uint32_t n = 0; n < kBlocks * kFrames; ++n)
        for (uint32_t l = 0; l < 8; ++l) {
          acc[l] += 624.0 * (1.0 + 0.1 * l) / kRate;
          acc[l] -= std::floor(acc[l]);
          phase[n * 8 + l] = static_cast<float>(acc[l]);
        }
      pq::dsp::FrameReadState st{};
      uint32_t blk = 0;
      c.measure(
          [&] {
            pq::dsp::readFrameTableL8(tables.morph.view(), st, phase.data() + blk * kFrames * 8, pos, false, kFrames,
                                      out.data());
            blk = (blk + 1) % kBlocks;
            pq::bench::doNotOptimize(out[kFrames * 8 - 1]);
          },
          kFrames, "ns/frame");
    };
  };
  runner.add("frametable/read L8 (blend)", readL8(true));
  runner.add("frametable/read L8 (single frame)", readL8(false));

  auto build = [](int bake) {
    return [bake](pq::bench::Context& c) {
      const auto cycles = drawn(64);
      std::vector<pq::dsp::Keyframe> keys;
      for (std::size_t i = 0; i < cycles.size(); ++i) keys.push_back({static_cast<float>(i / 63.0), cycles[i]});
      pq::dsp::FrameTableSpec spec;
      spec.keyframes = keys;
      spec.sampleRate = kRate;
      if (bake == 1) spec.bake = {pq::dsp::WarpMode::Bend, 0.0f, 0.5f, 0.0f};
      if (bake == 3) spec.bake = {pq::dsp::WarpMode::Mirror, 0.5f, 0.3f, 0.3f};
      pq::dsp::FrameTableBuilder b;
      auto t = b.build(spec).value();  // sizes the arenas
      std::vector<double> ms;
      for (int i = 0; i < 15; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!b.buildInto(spec, t).ok()) std::abort();
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      }
      c.record(pq::bench::summarize(ms), "ms/build");
      std::printf("frame table: %.2f MiB for 64 frames at 48 kHz\n", t.bytes() / (1024.0 * 1024.0));
    };
  };
  // TMM.2: wavetable_pos over a slot (the morph table), and voice_tap.
  static pq::dsp::FrameTableSlot* slot = [] {
    pq::dsp::FrameTableBuilder b;
    const auto saw = pq::dsp::basicCycle(pq::dsp::BasicCycle::Saw);
    const auto sq = pq::dsp::basicCycle(pq::dsp::BasicCycle::Square);
    const pq::dsp::Keyframe keys[] = {{0.0f, saw.span()}, {1.0f, sq.span()}};
    pq::dsp::FrameTableSpec spec;
    spec.keyframes = keys;
    spec.sampleRate = kRate;
    return new pq::dsp::FrameTableSlot(std::make_shared<const pq::dsp::FrameTable>(b.build(spec).value()));
  }();
  enum class Mode { Plain, TapRead, Crossfade };
  auto wavetablePos = [](uint32_t lanes, Mode mode) {
    return [lanes, mode](pq::bench::Context& c) {
      constexpr uint32_t kBlocks = 16;
      const uint32_t L = lanes;
      pq::AlignedBuffer<float> phase(kBlocks * kFrames * L), pos(kBlocks * kFrames * L + 8), out(kFrames * L);
      double acc[8];
      for (uint32_t l = 0; l < 8; ++l) acc[l] = 0.1 * l;
      for (uint32_t n = 0; n < kBlocks * kFrames; ++n)
        for (uint32_t l = 0; l < L; ++l) {
          acc[l] += 624.0 * (1.0 + 0.1 * l) / kRate;
          acc[l] -= std::floor(acc[l]);
          phase[n * L + l] = static_cast<float>(acc[l]);
          pos[n * L + l] = L == 1 ? 0.1f + 0.8f * static_cast<float>(n) / (kBlocks * kFrames) : 0.0f;
        }
      if (L == 8)
        for (uint32_t l = 0; l < 8; ++l) pos[l] = 0.13f + 0.097f * l;
      const uint32_t mask = L == 1 ? 0u : 2u;  // L1: per-sample position; L8: a Spl pattern
      const pq::dsp::KernelFn fn = pq::dsp::findKernel(pq::dsp::Atom::WavetablePos, L, mask);
      pq::AlignedBuffer<float> state(32);
      pq::dsp::StateInit si;
      si.lanes = L;
      pq::dsp::initState(pq::dsp::Atom::WavetablePos, state.data(), si);
      auto& st = *reinterpret_cast<pq::dsp::WavetablePosState*>(state.data());
      if (mode == Mode::Crossfade) {  // a swap to the same table: the fade path runs (the cost does not depend on it)
        static pq::EpochReclaimer rec;
        slot->publish(slot->table(), rec);
      }
      if (mode == Mode::TapRead) slot->tap()->addReader();
      const float* in[2] = {nullptr, nullptr};
      float* o[1] = {out.data()};
      uint32_t blk = 0;
      c.measure(
          [&] {
            in[0] = phase.data() + static_cast<std::size_t>(blk) * kFrames * L;
            in[1] = L == 1 ? pos.data() + static_cast<std::size_t>(blk) * kFrames : pos.data();
            if (mode == Mode::Crossfade) {
              st.generation = slot->generation() - 1;  // as if the swap had just happened: a full 64-frame fade
              st.fadeLeft = 0;
            }
            const pq::dsp::KernelArgs a{in, o, state.data(), slot, kFrames, 2, 1, mask};
            fn(a);
            blk = (blk + 1) % kBlocks;
            pq::bench::doNotOptimize(out[kFrames * L - 1]);
          },
          kFrames, L == 1 ? "ns/sample" : "ns/frame(8 voices)");
      if (mode == Mode::TapRead) slot->tap()->removeReader();
    };
  };
  runner.add("atom/wavetable_pos L1", wavetablePos(1, Mode::Plain));
  runner.add("atom/wavetable_pos L8", wavetablePos(8, Mode::Plain));
  runner.add("atom/wavetable_pos L1 (tap read)", wavetablePos(1, Mode::TapRead));
  runner.add("atom/wavetable_pos L8 (tap read)", wavetablePos(8, Mode::TapRead));
  runner.add("atom/wavetable_pos L1 (crossfade)", wavetablePos(1, Mode::Crossfade));
  runner.add("atom/wavetable_pos L8 (crossfade)", wavetablePos(8, Mode::Crossfade));
  auto voiceTap = [](uint32_t lanes, bool watched) {
    return [lanes, watched](pq::bench::Context& c) {
      static pq::VoiceTap tap(pq::VoiceTapConfig{});
      pq::AlignedBuffer<float> x(kFrames * 8), state(16);
      for (std::size_t i = 0; i < x.size(); ++i) x[i] = std::fmod(0.013f * static_cast<float>(i), 1.0f);
      pq::dsp::StateInit si;
      si.lanes = lanes;
      pq::dsp::initState(pq::dsp::Atom::VoiceTap, state.data(), si);
      const pq::dsp::KernelFn fn = pq::dsp::findKernel(pq::dsp::Atom::VoiceTap, lanes, 0);
      if (watched) tap.addReader();
      const float* in[1] = {x.data()};
      const pq::dsp::KernelArgs a{in, nullptr, state.data(), &tap, kFrames, 1, 0, 0};
      c.measure([&] { fn(a); pq::bench::doNotOptimize(state[0]); }, kFrames, lanes == 1 ? "ns/sample" : "ns/frame(8 voices)");
      if (watched) tap.removeReader();
    };
  };
  runner.add("atom/voice_tap L1 (unwatched)", voiceTap(1, false));
  runner.add("atom/voice_tap L8 (unwatched)", voiceTap(8, false));
  runner.add("atom/voice_tap L1 (watched)", voiceTap(1, true));
  runner.add("atom/voice_tap L8 (watched)", voiceTap(8, true));

  runner.add("frametable/build 64 frames", build(0));
  runner.add("frametable/build 64 frames + bake", build(1));
  runner.add("frametable/build 64 frames + bake3", build(3));
}
