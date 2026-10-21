// Per-atom kernel cost (T1.6): every leaf atom at 1 lane (global signal) and 8 lanes (one AVX2 voice group),
// all inputs audio rate (Blk), 64-frame blocks, called through the KernelFn pointer exactly as the interpreter
// will. Also buffer ⊗ param (Blk ⊗ Spl) for the hot binary atoms.
//   L1: ns per sample.  L8: ns per frame (8 voices) and ns per voice-sample (= frame / 8).
// Prints BENCH lines and a markdown table for bench/RESULTS.md.

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/cpu.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/tables.h"
#include "pq_bench.h"

using pq::dsp::Atom;

namespace {

constexpr uint32_t kFrames = 64;
constexpr uint32_t kMaxIn = 5, kMaxOut = 4;

struct Fixture {
  // All block buffers are slices of one arena, like the interpreter's scratch memory. `pad` floats between
  // consecutive buffers: with pad = 0 the 2 KiB buffers sit at multiples of 4 KiB from each other in pairs,
  // which provokes 4K aliasing between loads and stores.
  pq::AlignedBuffer<float> arena;
  float* in[kMaxIn]{};
  float* out[kMaxOut]{};
  pq::AlignedBuffer<float> state{1 << 18};  // 1 MiB: an 8-lane delay with 4096 frames needs 256 KiB + header
  const float* inPtrs[kMaxIn]{};
  float* outPtrs[kMaxOut]{};
  pq::dsp::Wavetable wt;
  pq::dsp::ShaperTable sh;
  pq::dsp::SampleData sd;
  explicit Fixture(uint32_t pad) : arena((kMaxIn + kMaxOut) * (kFrames * 8 + pad)) {
    const uint32_t stride = kFrames * 8 + pad;
    for (uint32_t i = 0; i < kMaxIn; ++i) in[i] = arena.data() + i * stride;
    for (uint32_t o = 0; o < kMaxOut; ++o) out[o] = arena.data() + (kMaxIn + o) * stride;
    for (uint32_t o = 0; o < kMaxOut; ++o) outPtrs[o] = out[o];
    const auto cycle = pq::dsp::Wavetable::basicCycle(pq::dsp::BasicShape::Saw, 2048);
    wt = pq::dsp::Wavetable::build(cycle.span()).value();
    std::vector<float> pts(1025);
    for (size_t i = 0; i < pts.size(); ++i) pts[i] = static_cast<float>(std::tanh(-3.0 + 6.0 * i / 1024.0));
    sh = pq::dsp::ShaperTable::build(pts, -1.0f, 1.0f).value();
    std::vector<float> smp(48000);
    for (size_t i = 0; i < smp.size(); ++i) smp[i] = static_cast<float>(std::sin(0.01 * i));
    sd = pq::dsp::SampleData::build(smp).value();
  }
};

// Realistic input values per atom/input (frame t, lane l).
float inputValue(Atom a, uint32_t i, uint32_t t, uint32_t l, bool isBool) {
  // Oscillator / playhead resets: none in the measured blocks (the common case; a firing reset switches the
  // block to the per-sample path).
  const bool isReset = i == 1 && (a == Atom::Phasor || a == Atom::SineHz || a == Atom::SampleReadLinear ||
                                  a == Atom::SampleReadCubic);
  if (isReset) return pq::dsp::boolMask(false);
  if (isBool) return pq::dsp::boolMask(((t + l) / 5) % 3 == 0);
  const float r = 0.1f + 0.8f * static_cast<float>((t * 37 + l * 11) % 101) / 100.0f;  // [0.1, 0.9]
  switch (a) {
    case Atom::DelayLinear: case Atom::DelayCubic: case Atom::DelayAllpass:
      return i == 1 ? 10.0f + 40.0f * r : r;
    case Atom::Phasor: case Atom::SineHz:
      return 100.0f + 1000.0f * r;
    case Atom::Wavetable:
      return std::fmod(0.013f * static_cast<float>(t) * (1.0f + 0.1f * l), 1.0f);
    case Atom::SampleReadLinear: case Atom::SampleReadCubic:
      return i == 0 ? 0.5f + r : i == 2 ? 0.0f : i == 3 ? 1000.0f : 40000.0f;
    case Atom::SamplePosLinear: case Atom::SamplePosCubic:
      return 100.0f + 30000.0f * r;
    case Atom::Mux: case Atom::Demux:
      return i == 0 ? 3.0f * r : r;
    case Atom::Counter: case Atom::ClockDiv:
      return 8.0f;
    case Atom::Pulse: return 16.0f;
    case Atom::Timer: return 8.0f;
    case Atom::Shaper: return 2.0f * r - 1.0f;
    case Atom::CentsToHz: case Atom::CentsToHzFast: return i == 0 ? 2400.0f * r - 1200.0f : 261.626f;
    case Atom::HzToCents: case Atom::HzToCentsFast: return i == 0 ? 1000.0f * r : 261.626f;
    case Atom::Clip: return i == 0 ? 2.0f * r - 1.0f : i == 1 ? -0.5f : 0.5f;
    default: return r;
  }
}

struct Row {
  std::string name;
  double l1 = 0, l8 = 0;
};

Row benchAtom(Fixture& fx, Atom a, uint32_t splMask, const char* suffix) {
  const auto& info = pq::dsp::kernelInfo(a);
  const uint32_t nIn = info.variadicIn ? 4u : info.numIn;
  const uint32_t nOut = info.variadicOut ? 4u : info.numOut;
  const void* resource = info.resource == pq::dsp::Resource::Wavetable ? static_cast<const void*>(&fx.wt.view())
                         : info.resource == pq::dsp::Resource::Shaper  ? static_cast<const void*>(&fx.sh.view())
                         : info.resource == pq::dsp::Resource::Sample  ? static_cast<const void*>(&fx.sd.view())
                                                                       : nullptr;
  Row row;
  row.name = std::string(info.name) + suffix;
  for (uint32_t lanes : {1u, 8u}) {
    const uint32_t mask = splMask | info.splRequired;
    for (uint32_t i = 0; i < nIn; ++i) {
      const bool isBool = (info.boolIn >> i) & 1u;
      const bool spl = (mask >> i) & 1u;
      for (uint32_t t = 0; t < kFrames; ++t)
        for (uint32_t l = 0; l < lanes; ++l) {
          const float v = inputValue(a, i, spl ? 0 : t, l, isBool);
          fx.in[i][static_cast<size_t>(t) * lanes + l] = v;
        }
      if (spl && lanes == 1)
        for (uint32_t k = 1; k < 8; ++k) fx.in[i][k] = fx.in[i][0];
      fx.inPtrs[i] = fx.in[i];
    }
    pq::dsp::StateInit init;
    init.lanes = lanes;
    init.maxDelayFrames = 4096;
    init.seed = 7;
    if (pq::dsp::stateBytes(a, init) > fx.state.size() * sizeof(float)) {
      std::printf("state buffer too small for %s\n", info.name);
      std::abort();
    }
    pq::dsp::initState(a, fx.state.data(), init);
    const pq::dsp::KernelFn fn = pq::dsp::findKernel(a, lanes, mask);
    pq::dsp::KernelArgs args{fx.inPtrs, fx.outPtrs, fx.state.data(), resource, kFrames,
                             static_cast<uint16_t>(nIn), static_cast<uint16_t>(nOut), mask};
    const auto s = pq::bench::measure([&] {
      fn(args);
      pq::bench::doNotOptimize(fx.out[0][0]);
    }, kFrames, 9, 2, 1e6);
    char label[96];
    if (lanes == 1) {
      row.l1 = s.medianNs;
      std::snprintf(label, sizeof label, "%s/L1", row.name.c_str());
      pq::bench::report(label, s, "ns/sample");
    } else {
      row.l8 = s.medianNs;
      std::snprintf(label, sizeof label, "%s/L8", row.name.c_str());
      pq::bench::report(label, s, "ns/frame(8 voices)");
    }
  }
  return row;
}

}  // namespace

int main(int argc, char** argv) {
  pq::DenormalGuard dg;
  // Optional argument: padding (floats) between block buffers; default 16 (64 bytes), see Fixture.
  const uint32_t pad = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 16u;
  std::printf("CPU: %s, buffer pad %u floats\n", pq::cpuFeatures().brand.c_str(), pad);
  Fixture fx(pad);
  std::vector<Row> rows;
  for (uint32_t ai = 0; ai < pq::dsp::kAtomCount; ++ai) rows.push_back(benchAtom(fx, static_cast<Atom>(ai), 0, ""));
  // Buffer ⊗ param: the second input is a Spl pattern (one vector load per block).
  for (Atom a : {Atom::Add, Atom::Mul, Atom::Div, Atom::Gt, Atom::CentsToHzFast})
    rows.push_back(benchAtom(fx, a, 0b10, " (blk,spl)"));
  rows.push_back(benchAtom(fx, Atom::MulAdd, 0b110, " (blk,spl,spl)"));
  rows.push_back(benchAtom(fx, Atom::Phasor, 0b11, " (spl hz, spl reset)"));
  rows.push_back(benchAtom(fx, Atom::Phasor, 0b10, " (blk hz, spl reset)"));
  rows.push_back(benchAtom(fx, Atom::SineHz, 0b10, " (blk hz, spl reset)"));
  rows.push_back(benchAtom(fx, Atom::SineHz, 0b11, " (spl hz, spl reset)"));
  rows.push_back(benchAtom(fx, Atom::DelayLinear, 0b10, " (spl delay)"));
  rows.push_back(benchAtom(fx, Atom::SampleReadCubic, 0b11, " (spl rate, spl reset)"));

  std::printf("\n| Atom | L1 ns/sample | L8 ns/frame | L8 ns/voice-sample |\n|---|---|---|---|\n");
  for (const Row& r : rows) std::printf("| %s | %.3f | %.3f | %.3f |\n", r.name.c_str(), r.l1, r.l8, r.l8 / 8.0);
  return 0;
}
