// T6.2: plugin-delay compensation costs (release / ship).
//   pdc/inputs   64 stereo sine tracks into a master, one of them latency(x, 480): the master reads the 63 others through
//                delay lines. ns per block with the plan vs the same program without it; the difference / 63 is what one
//                compensated stereo input costs per 64-frame block (plan: ≤ 40 ns).
//   pdc/plan     planCompensation on a 128-track layout (8 groups of 16, a return, the master; every 4th track late):
//                µs per plan (plan: ≤ 50 µs).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/denormal_guard.h"
#include "pq/engine/latency_plan.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/graph/parse.h"
#include "pq_bench.h"

namespace g = pq::graph;
namespace e = pq::engine;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;

const char* kText = R"(
patch Tone {
  param hz = 100 [20..2000]
  out out : audio global stereo
  x = sine(phasor(hz))
  out = (x, x * 0.5)
}
patch ToneLate {
  out out : audio global stereo
  x = sine(phasor(110Hz))
  out = latency((x, x * 0.5), 480)
}
patch Sum {
  in  in : audio global stereo
  out out : audio global stereo
  out = in
}
)";

std::unique_ptr<e::ProjectProgram> build(const g::PatchFile& f, bool compensate) {
  g::ProjectGraph gr;
  auto patch = [&](const char* n) {
    for (const g::Patch& p : f.patches)
      if (p.name() == n) return &p;
    return static_cast<const g::Patch*>(nullptr);
  };
  gr.tracks.push_back({1, "Master", patch("Sum"), g::TrackRole::Master, g::kNone});
  for (uint32_t k = 0; k < 64; ++k) {
    gr.tracks.push_back({k + 2, "T" + std::to_string(k), patch(k == 17 ? "ToneLate" : "Tone"), g::TrackRole::Audio, g::kNone});
    gr.wires.push_back({{k + 1, "out"}, {0, "in"}, g::WireKind::Audio});
  }
  gr.output = {0, "out"};
  e::ProjectCompileOptions o;
  o.sampleRate = kSr;
  o.maxBlockFrames = kBlock;
  e::ProjectCompiler c(o);
  auto b = c.build(gr);
  if (!b.ok()) {
    std::fprintf(stderr, "%s\n", b.error().message.c_str());
    return nullptr;
  }
  e::ProjectConfig pc;
  pc.sampleRate = kSr;
  pc.maxBlockFrames = kBlock;
  auto prog = e::ProjectProgram::create(pc, b.value().layout);
  for (uint32_t u = 0; u < b.value().images.size(); ++u) (void)prog.value()->publish(u, std::move(b.value().images[u]));
  if (compensate) (void)prog.value()->setCompensation(e::planCompensation(b.value().layout, c.latencies()));
  return std::move(prog.value());
}

}  // namespace

int main(int argc, char** argv) {
  pq::bench::Runner r("pq_bench_pdc");
  auto file = g::parse(kText, "pdc.pqp");
  if (!file.ok()) {
    std::fprintf(stderr, "%s\n", file.error().message.c_str());
    return 1;
  }
  r.add("pdc/inputs", [&](pq::bench::Context& c) {
    for (const bool comp : {false, true}) {
      auto prog = build(file.value(), comp);
      if (!prog) return;
      std::vector<float> l(kBlock), rr(kBlock);
      float* outs[2] = {l.data(), rr.data()};
      e::AudioBlock ab;
      ab.out = outs;
      ab.numOut = 2;
      ab.frames = kBlock;
      int64_t t = 0;
      pq::DenormalGuard dg;
      c.measureBlocks(
          [&] {
            ab.frameTime = t;
            prog->process(ab);
            t += kBlock;
          },
          1, "ns/block", comp ? "63 compensated" : "no plan");
    }
  });
  r.add("pdc/plan", [&](pq::bench::Context& c) {
    // 128 tracks → 8 groups → master, plus a return reading every track; every 4th track 64 samples late.
    e::ProjectLayout layout;
    std::vector<e::UnitLatency> units;
    const uint32_t tracks = 128, groups = 8;
    for (uint32_t k = 0; k < tracks; ++k) {
      layout.units.push_back({});
      layout.busWidth.push_back(2);
      layout.busProducer.push_back(k);
      e::UnitLatency u;
      u.out = {{k, k % 4 == 0 ? 64.0 : 0.0}};
      units.push_back(u);
    }
    for (uint32_t gi = 0; gi < groups; ++gi) {
      layout.units.push_back({});
      layout.busWidth.push_back(2);
      layout.busProducer.push_back(tracks + gi);
      e::UnitLatency u;
      u.out = {{tracks + gi, 0.0}};
      for (uint32_t k = gi * 16; k < gi * 16 + 16; ++k) u.compensated.push_back(k);
      units.push_back(u);
    }
    layout.units.push_back({});  // the return
    layout.busWidth.push_back(2);
    layout.busProducer.push_back(tracks + groups);
    e::UnitLatency ret;
    ret.out = {{tracks + groups, 0.0}};
    for (uint32_t k = 0; k < tracks; ++k) ret.compensated.push_back(k);
    units.push_back(ret);
    layout.units.push_back({});  // the master
    layout.busWidth.push_back(2);
    layout.busProducer.push_back(tracks + groups + 1);
    e::UnitLatency m;
    for (uint32_t b = tracks; b <= tracks + groups; ++b) m.compensated.push_back(b);
    units.push_back(m);
    layout.outputBus = tracks + groups + 1;
    uint32_t sink = 0;
    c.measure([&] { sink += e::planCompensation(layout, units).total; }, 1, "ns/plan", "128 tracks");
    if (sink == 0xFFFFFFFFu) std::printf("%u\n", sink);
  });
  return r.main(argc, argv);
}
