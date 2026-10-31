// T1.x-silence (SPEC §7.7): what sleeping saves and what it costs, per 64-frame block at 48 kHz.
//   idle/30 tracks <backend>        the acceptance fixture (tests/engine/idle_project_fixture.h: 30 stdlib tracks —
//                                   basic_synth → comb/comb_lp/allpass_delay reverb + a 375 ms feedback echo → EQ
//                                   Eight → gain — a return with a comb/allpass reverb, a master) after its chords
//                                   and every tail have died away: every unit asleep (acceptance: ≤ 1 % of a core)
//   idle/30 tracks <backend> nosleep  the same project compiled without sleep plans (what it cost before)
//   idle/30 tracks <backend> links  the idle project with every track's params linked to project params (vol, the
//                                   synth's and EQ Eight's: ~100 links per unit, as ProjectLink links device params):
//                                   M4 review L1 (an idle unit compares its links only after a param write)
//   awake/30-track fixture <backend> sleep|nosleep   tests/engine/project_fixture.h (30 FM tracks × 28 effects, chords
//                                   held): everything sounds, so every segment is awake every block: the cost of the
//                                   sleep bookkeeping on non-silent playback (the source scans, per-region flags)
// Backends: interp and jit (every unit's image from compile::jitUnitBackend()). ns per block; "% of a core" =
// ns / 1 333 333 (one block period). Release / ship build. Usage: pq_bench_silence [runner options]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../tests/engine/idle_project_fixture.h"
#include "../../tests/engine/project_fixture.h"
#include "pq/compile/compile.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq_bench.h"

using namespace pq;
namespace e = pq::engine;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;

struct Rig {
  std::unique_ptr<idle_fixture::Project> idle;
  std::unique_ptr<project_fixture::Project> busy;
  std::vector<std::unique_ptr<e::NoteListSource>> notes;
  std::unique_ptr<e::ProjectProgram> program;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  int64_t time = 0;
  uint32_t units = 0;

  Rig(bool idleProject, bool jit, bool sleep, bool links = false) {
    const graph::ProjectGraph* g = nullptr;
    uint32_t tracks = 30;
    if (idleProject) {
      idle = idle_fixture::makeProject(tracks);
      if (!idle) std::exit(1);
      g = &idle->graph;
    } else {
      busy = project_fixture::makeProject(project_fixture::Options{30, 28, 3, 5});
      if (!busy) std::exit(1);
      g = &busy->graph;
    }
    e::ProjectCompileOptions co;
    co.sampleRate = kSr;
    co.maxBlockFrames = kBlock;
    co.voice.maxVoices = 8;
    co.sleep = sleep;
    if (jit) co.backend = compile::jitUnitBackend();
    uint32_t numParams = 0;
    if (links && idleProject) {  // vol, and every param of the synth and the EQ the track patch leaves unconnected
      auto paramsOf = [&](std::string_view name) {
        std::vector<std::string> out;
        if (const graph::Patch* d = idle->library.find(name))
          for (graph::NodeIndex v = 0; v < d->nodeCount(); ++v)
            if (d->node(v).kind == graph::NodeKind::Param) out.push_back(d->nodeName(v));
        return out;
      };
      const std::vector<std::string> syn = paramsOf("BasicSynth"), eq = paramsOf("eq_eight");
      for (uint32_t t = 0; t < tracks; ++t) {
        co.paramLinks.push_back({t, "vol", numParams++});
        for (const std::string& n : syn)
          if (n != "release") co.paramLinks.push_back({t, "syn__" + n, numParams++});
        for (const std::string& n : eq) co.paramLinks.push_back({t, "eq__" + n, numParams++});
      }
    }
    e::ProjectCompiler compiler(co);
    auto b = compiler.build(*g);
    if (!b.ok()) {
      std::fprintf(stderr, "%s\n", b.error().message.c_str());
      std::exit(2);
    }
    e::ProjectConfig pc;
    pc.sampleRate = kSr;
    pc.maxBlockFrames = kBlock;
    pc.numOut = 2;
    pc.numProjectParams = numParams;
    for (uint32_t k = 0; k < tracks; ++k) {
      std::vector<e::TimedNote> n = idleProject ? idle_fixture::chord(k)
                                                : std::vector<e::TimedNote>{{0, 48000 * 600, 110.0f * (1.0f + 0.125f * (k % 8)), 0.8f, 0.5f},
                                                                            {0, 48000 * 600, 137.5f * (1.0f + 0.125f * (k % 8)), 0.7f, 0.5f}};
      notes.push_back(std::make_unique<e::NoteListSource>(n));
      pc.eventSources.push_back(notes.back()->source());
    }
    auto p = e::ProjectProgram::create(pc, b.value().layout);
    if (!p.ok()) std::exit(3);
    program = std::move(p.value());
    units = static_cast<uint32_t>(b.value().images.size());
    for (uint32_t u = 0; u < units; ++u)
      if (!program->publish(u, std::move(b.value().images[u])).ok()) std::exit(4);
    if (numParams > 0) {  // the params at their patches' defaults would do too; any value: nothing changes after this
      std::span<float> pp = program->projectParams();
      std::fill(pp.begin(), pp.end(), 0.5f);
    }
    // The idle project: past every tail (chords at 0, the echoes ring ≈ 7 s); the busy one: settled.
    const uint64_t warm = idleProject ? 48000 * 14 : 48000;
    for (uint64_t t = 0; t < warm; t += kBlock) block();
  }
  void block() {
    float* ch[2] = {l.data(), r.data()};
    e::AudioBlock ab{nullptr, 0, ch, 2, kBlock, time};
    program->process(ab);
    time += kBlock;
  }
  uint32_t asleepUnits() {
    std::vector<uint64_t> before;
    for (uint32_t u = 0; u < units; ++u) before.push_back(program->unitStats(u).idleBlocks);
    block();
    uint32_t n = 0;
    for (uint32_t u = 0; u < units; ++u) n += program->unitStats(u).idleBlocks > before[u] ? 1u : 0u;
    return n;
  }
};

}  // namespace

int main(int argc, char** argv) {
  DenormalGuard dg;
  bench::Runner runner("pq_bench_silence");
  struct Case {
    bool idle, jit, sleep, links = false;
  };
  const Case cases[] = {{true, false, true},  {true, true, true},   {true, false, false}, {true, true, false},
                        {true, false, true, true}, {true, true, true, true},
                        {false, false, true}, {false, false, false}, {false, true, true}, {false, true, false}};
  for (const Case& k : cases) {
    const std::string name = std::string(k.idle ? "idle/30 tracks" : "awake/30-track fixture") + (k.jit ? " jit" : " interp") +
                             (k.idle ? (k.sleep ? "" : " nosleep") : (k.sleep ? " sleep" : " nosleep")) + (k.links ? " links" : "");
    runner.add(name, [k, name](bench::Context& c) {
      Rig rig(k.idle, k.jit, k.sleep, k.links);
      const uint32_t asleep = rig.asleepUnits();
      const bench::Stats s = c.measure([&] { rig.block(); }, 1, "ns/block");
      std::printf("  %s: %.3f %% of a core (median), %u of %u units asleep\n", name.c_str(),
                  100.0 * s.medianNs / (1e9 * kBlock / kSr), asleep, rig.units);
    });
  }
  // The awake overhead as an interleaved A/B (the machine's load moves between separate rows): rounds of 100 blocks
  // of the sleep-plan program, then 100 of the plan-less one, same notes; the median of the per-round ratios.
  for (bool jit : {false, true}) {
    const std::string name = std::string("awake/overhead A/B ") + (jit ? "jit" : "interp");
    runner.add(name, [jit, name](bench::Context& c) {
      Rig a(false, jit, true), b(false, jit, false);
      std::vector<double> ratio, ta, tb;
      for (int round = 0; round < 60; ++round) {
        auto time = [](Rig& r) {
          const auto t0 = std::chrono::steady_clock::now();
          for (int i = 0; i < 100; ++i) r.block();
          return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / 100.0;
        };
        const double x = round % 2 ? time(a) : 0.0, y = time(b), z = round % 2 ? 0.0 : time(a);
        const double av = x + z;
        ta.push_back(av);
        tb.push_back(y);
        ratio.push_back(av / y);
      }
      c.record(bench::summarize(ta), "ns/block", "sleep");
      c.record(bench::summarize(tb), "ns/block", "nosleep");
      const bench::Stats r = bench::summarize(ratio);
      c.record(r, "ratio", "sleep/nosleep");
      std::printf("  %s: sleep / nosleep = %.4f (median of 60 interleaved rounds; min %.4f, max %.4f)\n", name.c_str(),
                  r.medianNs, r.minNs, r.maxNs);
    });
  }
  return runner.main(argc, argv);
}
