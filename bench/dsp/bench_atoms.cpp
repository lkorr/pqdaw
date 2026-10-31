// Per-atom kernel cost (T1.6): every leaf atom at 1 lane (global signal) and 8 lanes (one AVX2 voice group),
// all inputs audio rate (Blk), 64-frame blocks, called through the KernelFn pointer exactly as the interpreter
// will. Also buffer ⊗ param (Blk ⊗ Spl) for the hot binary atoms.
//   L1: ns per sample.  L8: ns per frame (8 voices) and ns per voice-sample (= frame / 8).
// Prints BENCH lines and a markdown table for bench/RESULTS.md. T1.11: one runner benchmark per atom variant
// (`atom/<name>[ (spl…)]`, rows `… L1` / `… L8`): pq_bench_dsp [--filter REGEX] [--csv FILE] [--pad N] (pq_bench.h).
// Q94: `atom/<exact fn> (1-lane tail)`: the S1 scalar path of the exact transcendentals alone.

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/cpu.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/tables.h"
#include "pq_bench.h"

using pq::dsp::Atom;

void addResampleBenches(pq::bench::Runner& runner);  // bench_resample.cpp (T5.4)

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

// Wavetable phase increment per sample and its per-lane spread. Default: 0.013 cycles (624 Hz at 48 kHz, inside a
// mip crossfade), lanes 10 % apart; the " (steady 1 kHz)" variant: one pitch where only the upper level is read.
float gWavetableInc = 0.013f, gWavetableSpread = 0.1f;

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
    case Atom::DelayReadLinear: case Atom::DelayReadCubic: case Atom::DelayReadAllpass:
      return 10.0f + 40.0f * r;
    case Atom::Phasor: case Atom::SineHz:
      return 100.0f + 1000.0f * r;
    case Atom::Wavetable:
      return std::fmod(gWavetableInc * static_cast<float>(t) * (1.0f + gWavetableSpread * l), 1.0f);
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

struct TableRow {
  std::string name;
  double l1 = 0, l8 = 0;
};

TableRow benchAtom(pq::bench::Context& ctx, Fixture& fx, Atom a, uint32_t splMask, const char* suffix) {
  const auto& info = pq::dsp::kernelInfo(a);
  const uint32_t nIn = info.variadicIn ? 4u : info.numIn;
  const uint32_t nOut = info.variadicOut ? 4u : info.numOut;
  const void* resource = info.resource == pq::dsp::Resource::Wavetable ? static_cast<const void*>(&fx.wt.view())
                         : info.resource == pq::dsp::Resource::Shaper  ? static_cast<const void*>(&fx.sh.view())
                         : info.resource == pq::dsp::Resource::Sample  ? static_cast<const void*>(&fx.sd.view())
                                                                       : nullptr;
  TableRow row;
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
    const auto s = ctx.measure([&] {
      fn(args);
      pq::bench::doNotOptimize(fx.out[0][0]);
    }, kFrames, lanes == 1 ? "ns/sample" : "ns/frame(8 voices)", lanes == 1 ? "L1" : "L8");
    (lanes == 1 ? row.l1 : row.l8) = s.medianNs;
  }
  return row;
}

// The 1-lane scalar path alone (QUESTIONS Q94): an element-wise kernel at L = 1 runs 8 samples at a time with AVX2
// and only a block's remainder (or a per-sample loop's single frame) with the S1 functions. Nine 7-frame calls over
// the 64-frame inputs are all remainder, so this is the S1 cost per sample plus a ninth of a call each.
void benchScalarTail(pq::bench::Context& ctx, Fixture& fx, Atom a) {
  const auto& info = pq::dsp::kernelInfo(a);
  const uint32_t nIn = info.numIn, nOut = info.numOut;
  for (uint32_t i = 0; i < nIn; ++i)
    for (uint32_t t = 0; t < kFrames; ++t) fx.in[i][t] = inputValue(a, i, t, 0, false);
  constexpr uint32_t kTail = 7, kCalls = 9;
  const float* ins[kCalls][kMaxIn]{};
  float* outs[kCalls][kMaxOut]{};
  pq::dsp::KernelArgs args[kCalls];
  const pq::dsp::KernelFn fn = pq::dsp::findKernel(a, 1, info.splRequired);
  for (uint32_t c = 0; c < kCalls; ++c) {
    for (uint32_t i = 0; i < nIn; ++i) ins[c][i] = fx.in[i] + c * kTail;
    for (uint32_t o = 0; o < nOut; ++o) outs[c][o] = fx.out[o] + c * kTail;
    args[c] = pq::dsp::KernelArgs{ins[c], outs[c], nullptr, nullptr, kTail, static_cast<uint16_t>(nIn),
                                  static_cast<uint16_t>(nOut), info.splRequired};
  }
  ctx.measure([&] {
    for (const auto& ar : args) fn(ar);
    pq::bench::doNotOptimize(fx.out[0][0]);
  }, kTail * kCalls, "ns/sample", "S1 (7-frame calls)");
}

}  // namespace

int main(int argc, char** argv) {
  pq::DenormalGuard dg;
  // --pad N: padding (floats) between block buffers; default 16 (64 bytes), see Fixture. The other options are the
  // runner's (pq_bench.h).
  uint32_t pad = 16;
  std::vector<char*> args{argv[0]};
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--pad") == 0 && i + 1 < argc) pad = static_cast<uint32_t>(std::atoi(argv[++i]));
    else args.push_back(argv[i]);
  }
  std::printf("CPU: %s, buffer pad %u floats\n", pq::cpuFeatures().brand.c_str(), pad);
  Fixture fx(pad);
  std::vector<TableRow> rows;
  pq::bench::Runner runner("pq_bench_dsp");
  auto add = [&](Atom a, uint32_t splMask, const char* suffix) {
    runner.add(std::string("atom/") + pq::dsp::kernelInfo(a).name + suffix,
               [&fx, &rows, a, splMask, suffix](pq::bench::Context& c) {
                 rows.push_back(benchAtom(c, fx, a, splMask, suffix));
               });
  };
  for (uint32_t ai = 0; ai < pq::dsp::kAtomCount; ++ai)
    if (static_cast<Atom>(ai) != Atom::TuningHz)  // T2.6: needs a tuning table; pq_bench_tuning_signal measures it
      add(static_cast<Atom>(ai), 0, "");
  // Buffer ⊗ param: the second input is a Spl pattern (one vector load per block).
  for (Atom a : {Atom::Add, Atom::Mul, Atom::Div, Atom::Gt, Atom::CentsToHzFast}) add(a, 0b10, " (blk,spl)");
  add(Atom::MulAdd, 0b110, " (blk,spl,spl)");
  add(Atom::Phasor, 0b11, " (spl hz, spl reset)");
  add(Atom::Phasor, 0b10, " (blk hz, spl reset)");
  add(Atom::SineHz, 0b10, " (blk hz, spl reset)");
  add(Atom::SineHz, 0b11, " (spl hz, spl reset)");
  add(Atom::DelayLinear, 0b10, " (spl delay)");
  add(Atom::SampleReadCubic, 0b11, " (spl rate, spl reset)");
  runner.add("atom/wavetable (steady 1 kHz)", [&fx, &rows](pq::bench::Context& c) {
    gWavetableInc = 1000.0f / 48000.0f;
    gWavetableSpread = 0.0f;
    rows.push_back(benchAtom(c, fx, Atom::Wavetable, 0, " (steady 1 kHz)"));
    gWavetableInc = 0.013f;
    gWavetableSpread = 0.1f;
  });
  for (Atom a : {Atom::Sin, Atom::Tan, Atom::Tanh, Atom::Exp, Atom::Log, Atom::Pow, Atom::CentsToHz, Atom::HzToCents,
                 Atom::DbToGain, Atom::GainToDb})
    runner.add(std::string("atom/") + pq::dsp::kernelInfo(a).name + " (1-lane tail)",
               [&fx, a](pq::bench::Context& c) { benchScalarTail(c, fx, a); });
  addResampleBenches(runner);
  const int code = runner.main(static_cast<int>(args.size()), args.data());
  if (code != 0 || rows.empty()) return code;

  std::printf("\n| Atom | L1 ns/sample | L8 ns/frame | L8 ns/voice-sample |\n|---|---|---|---|\n");
  for (const TableRow& r : rows) std::printf("| %s | %.3f | %.3f | %.3f |\n", r.name.c_str(), r.l1, r.l8, r.l8 / 8.0);
  return 0;
}
