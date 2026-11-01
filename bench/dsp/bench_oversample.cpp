// TMM.6: the oversample region's rate converters (pq/dsp/oversample.h) on their own: up, down and phase at ×2 … ×16,
// 1 lane (a global signal) and 8 lanes (a voice group), 64 base frames per call (the kernel's frames = 64 · n), called
// through the KernelFn pointer as both backends call it. Rows `oversample/<kind> xN L1` (ns per base-rate sample) and
// `… L8` (ns per frame of 8 voices); the summary prints ns per channel-sample per ×2 stage (the TMM.6 target: ≤ 1.5 ns
// per stage at L8, per lane).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/oversample.h"
#include "pq_bench.h"

using pq::dsp::Atom;

namespace {

constexpr uint32_t kBase = 64;

struct Row {
  std::string name;
  uint32_t stages = 0;
  double l1 = 0, l8 = 0;
};
std::vector<Row> gRows;

void benchConverter(pq::bench::Context& ctx, Atom a, uint32_t n, Row& row) {
  const bool up = a != Atom::OversampleDown;
  for (uint32_t lanes : {1u, 8u}) {
    const size_t inCount = static_cast<size_t>(kBase) * (up ? 1 : n) * lanes;
    const size_t outCount = static_cast<size_t>(kBase) * (up ? n : 1) * lanes;
    pq::AlignedBuffer<float> in(inCount), out(outCount), state(1024);
    for (size_t i = 0; i < inCount; ++i)
      in[i] = a == Atom::OversamplePhase ? static_cast<float>(std::fmod(0.0123 * static_cast<double>(i / lanes), 1.0))
                                         : static_cast<float>(0.7 * std::sin(0.05 * static_cast<double>(i)));
    alignas(32) float factor[8];
    for (float& f : factor) f = static_cast<float>(n);
    pq::dsp::StateInit init;
    init.lanes = lanes;
    pq::dsp::initState(a, state.data(), init);
    const float* ins[2] = {in.data(), factor};
    float* outs[1] = {out.data()};
    const pq::dsp::KernelFn fn = pq::dsp::findKernel(a, lanes, 2u);
    pq::dsp::KernelArgs args{ins, outs, state.data(), nullptr, kBase * n, 2, 1, 2u};
    const auto s = ctx.measure([&] {
      fn(args);
      pq::bench::doNotOptimize(out[0]);
    }, kBase, lanes == 1 ? "ns/sample" : "ns/frame(8 voices)", lanes == 1 ? "L1" : "L8");
    (lanes == 1 ? row.l1 : row.l8) = s.medianNs;
  }
}

}  // namespace

void addOversampleBenches(pq::bench::Runner& runner) {
  for (Atom a : {Atom::OversampleUp, Atom::OversampleDown, Atom::OversamplePhase})
    for (uint32_t n : {2u, 4u, 8u, 16u}) {
      const char* kind = a == Atom::OversampleUp ? "up" : a == Atom::OversampleDown ? "down" : "phase";
      const std::string name = std::string("oversample/") + kind + " x" + std::to_string(n);
      runner.add(name, [a, n, name](pq::bench::Context& c) {
        Row row;
        row.name = name;
        row.stages = pq::dsp::os::stagesOf(n);
        benchConverter(c, a, n, row);
        gRows.push_back(row);
      });
    }
}

void printOversampleTable() {
  if (gRows.empty()) return;
  std::printf("\n| Converter | L1 ns/sample | L8 ns/voice-sample | L1 ns per x2 stage | L8 ns per x2 stage per lane |\n"
              "|---|---|---|---|---|\n");
  for (const Row& r : gRows)
    std::printf("| %s | %.2f | %.3f | %.2f | %.3f |\n", r.name.c_str(), r.l1, r.l8 / 8.0, r.l1 / r.stages,
                r.l8 / 8.0 / r.stages);
}
