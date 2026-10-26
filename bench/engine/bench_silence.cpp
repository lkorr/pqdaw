// T1.x-silence (SPEC §7.7): what sleeping saves and what it costs, per 64-frame block at 48 kHz.
//   idle/30 tracks <backend>        the acceptance fixture (tests/engine/idle_project_fixture.h: 30 stdlib tracks —
//                                   basic_synth → comb/comb_lp/allpass_delay reverb + a 375 ms feedback echo → EQ
//                                   Eight → gain — a return with a comb/allpass reverb, a master) after its chords
//                                   and every tail have died away: every unit asleep (acceptance: ≤ 1 % of a core)
//   idle/30 tracks <backend> nosleep  the same project compiled without sleep plans (what it cost before)
//   awake/30-track fixture <backend> sleep|nosleep   tests/engine/project_fixture.h (30 FM tracks × 28 effects, chords
//                                   held): everything sounds, so every segment is awake every block: the cost of the
//                                   sleep bookkeeping on non-silent playback (the source scans, per-region flags)
// Backends: interp and jit (every unit's image from compile::jitUnitBackend()). ns per block; "% of a core" =
// ns / 1 333 333 (one block period). Release / ship build. Usage: pq_bench_silence [runner options]
#include <cstdio>
#include <memory>
#include <string>
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

  Rig(bool idleProject, bool jit, bool sleep) {
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
    bool idle, jit, sleep;
  };
  const Case cases[] = {{true, false, true},  {true, true, true},   {true, false, false}, {true, true, false},
                        {false, false, true}, {false, false, false}, {false, true, true}, {false, true, false}};
  for (const Case& k : cases) {
    const std::string name = std::string(k.idle ? "idle/30 tracks" : "awake/30-track fixture") + (k.jit ? " jit" : " interp") +
                             (k.idle ? (k.sleep ? "" : " nosleep") : (k.sleep ? " sleep" : " nosleep"));
    runner.add(name, [k, name](bench::Context& c) {
      Rig rig(k.idle, k.jit, k.sleep);
      const uint32_t asleep = rig.asleepUnits();
      const bench::Stats s = c.measure([&] { rig.block(); }, 1, "ns/block");
      std::printf("  %s: %.3f %% of a core (median), %u of %u units asleep\n", name.c_str(),
                  100.0 * s.medianNs / (1e9 * kBlock / kSr), asleep, rig.units);
    });
  }
  return runner.main(argc, argv);
}
