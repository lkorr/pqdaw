// T1.x-arch1: per-track compile units on the 30-track fixture (tests/engine/project_fixture.h: ≈ 21k schedule ops,
// 30 FM-voice instrument tracks with 28 effect devices each, 2 groups, a return, a master; a sidechain and a
// cross-track modulation merge two tracks into the group B / return units). Release build only.
//   compile/*    front end (compose + infer + optimise + schedule) and backend times of the per-unit build, the same
//                graph as one whole-project program (what an edit costs without units: SPEC §15 / M1 review H6), and
//                the JIT per unit (and, with --whole-jit, the whole-project program)
//   equal/*      the per-unit project vs the whole-project program, both interpreted, every instrument on the same
//                notes: max |difference| over 1 s (acceptance: ≤ 1e-6)
//   edit/*       steady state with every unit JIT-compiled and notes sounding, then `rounds` edits of one device
//                constant on track 10: the edit → update() → publish → the unit's new (interpreted) image current
//                at the next block ("audible"), its crossfade, the interpreted window while the JIT compiles the new
//                unit on another core, then the JIT image's crossfade. Per-block audio-thread times (ns) of the
//                whole edit window vs steady state (acceptance: peak ≤ 1.3× steady)
//   edit-interp/* the same with every unit interpreted (no JIT at all)
//   parallel/*    PF.4, with --threads-sweep (only that): the cold open of the fixture through the background compiler
//                 (quick first images, then the final ones; a fresh Compiler per run: no cache) at 1/2/4/6 workers:
//                 the time until every unit has a JIT image and until every unit has its final one, the process's peak
//                 private memory above the start
//   largest unit  PF.4, with --largest N (only that): N fresh compiles of the largest unit's final image (an edit of it
//                 waits for one): median time and its phases
// Usage: pq_bench_project [--rounds N] [--whole-jit] [--threads-sweep [--sweep-rounds N] [--sweep-threads 1|2|4|6]] [--largest N]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../tests/engine/project_fixture.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/background_compile.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/jit/compiler.h"
#include "pq_bench.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// psapi.h needs windows.h first.
#include <psapi.h>

using namespace pq;
namespace g = pq::graph;
namespace e = pq::engine;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;
constexpr uint32_t kEditTrack = 10;

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

Result<std::unique_ptr<e::ProgramImage>> jitBuild(void* user, const g::Schedule& s, uint32_t maxBlock, uint32_t groups,
                                                  const e::Resources* res) {
  jit::CompileOptions o;
  o.maxBlockFrames = maxBlock;
  o.maxVoiceGroups = groups;
  o.resources = res;
  return static_cast<jit::Compiler*>(user)->compile(s, o);
}

e::ProjectCompileOptions options(jit::Compiler* jit) {
  e::ProjectCompileOptions o;
  o.sampleRate = kSr;
  o.maxBlockFrames = kBlock;
  o.voice.maxVoices = 8;
  o.voice.silenceDb = -300.0f;
  if (jit != nullptr) o.backend = {&jitBuild, jit};
  return o;
}

// Long notes: every track holds a 3-note chord for the whole run (sameNotes: all tracks the same chord).
std::vector<e::TimedNote> chord(uint32_t k, bool sameNotes) {
  const uint32_t v = sameNotes ? 0u : k;
  const float base = 110.0f * (1.0f + 0.125f * static_cast<float>(v % 8));
  return {{0, 48000 * 600, base, 0.8f, 0.5f}, {0, 48000 * 600, base * 1.25f, 0.7f, 0.5f}, {0, 48000 * 600, base * 1.5f, 0.6f, 0.5f}};
}

struct Rig {
  std::vector<std::unique_ptr<e::NoteListSource>> notes;
  std::unique_ptr<e::ProjectProgram> program;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  int64_t time = 0;
  double block() {  // ns of one process() call
    float* outs[2] = {l.data(), r.data()};
    e::AudioBlock ab;
    ab.out = outs;
    ab.numOut = 2;
    ab.frames = kBlock;
    ab.frameTime = time;
    const auto t0 = std::chrono::steady_clock::now();
    program->process(ab);
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    time += kBlock;
    return ns;
  }
};

std::unique_ptr<Rig> makeRig(e::ProjectBuild& b, uint32_t tracks, bool sameNotes) {
  auto r = std::make_unique<Rig>();
  e::ProjectConfig pc;
  pc.sampleRate = kSr;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  for (uint32_t k = 0; k < tracks; ++k) {
    r->notes.push_back(std::make_unique<e::NoteListSource>(chord(k, sameNotes)));
    pc.eventSources.push_back(r->notes.back()->source());
  }
  auto prog = e::ProjectProgram::create(pc, b.layout);
  if (!prog.ok()) {
    std::fprintf(stderr, "%s\n", prog.error().message.c_str());
    std::exit(3);
  }
  r->program = std::move(prog.value());
  for (uint32_t u = 0; u < b.images.size(); ++u)
    if (!r->program->publish(u, std::move(b.images[u])).ok()) std::exit(4);
  return r;
}

// FmSynth's `level: 0.15` constant in a track patch (edited in place on the Patch IR: ids kept).
g::NodeIndex levelNode(const project_fixture::Project& p, uint32_t track) {
  const g::Patch& tp = *p.trackPatches[track];
  for (g::NodeIndex n = 0; n < tp.nodeCount(); ++n)
    if (tp.node(n).kind == g::NodeKind::Const && tp.constant(n).literal == 0.15) return n;
  std::fprintf(stderr, "no level constant\n");
  std::exit(5);
}

struct BuildTimes {
  double frontMs = 0, backMs = 0, maxUnitFrontMs = 0, maxUnitBackMs = 0;
  uint32_t ops = 0, maxUnitOps = 0;
};

BuildTimes times(const e::ProjectCompiler& c) {
  BuildTimes t;
  for (uint32_t u = 0; u < c.numUnits(); ++u) {
    const e::CompiledUnit& cu = c.unit(u);
    t.frontMs += cu.frontEndMs;
    t.backMs += cu.buildMs;
    t.maxUnitFrontMs = std::max(t.maxUnitFrontMs, cu.frontEndMs);
    t.maxUnitBackMs = std::max(t.maxUnitBackMs, cu.buildMs);
    t.ops += cu.ops;
    t.maxUnitOps = std::max(t.maxUnitOps, cu.ops);
  }
  return t;
}

void printStats(const char* name, const std::vector<double>& v) {
  const bench::Stats s = bench::summarize(v);
  std::printf("BENCH %-44s median %9.0f  p99 %9.0f  max %9.0f ns/block  (%zu blocks)\n", name, s.medianNs, s.p99Ns,
              s.maxNs, s.samples);
}

// The edit scenario on a running rig. `jit` null: the edited unit stays interpreted.
void editScenario(const char* prefix, project_fixture::Project& proj, e::ProjectCompiler& compiler, Rig& rig,
                  jit::Compiler* jit, int rounds, g::NodeIndex level) {
  const uint32_t unit = compiler.partition().unitOf[kEditTrack];
  const e::UnitBackend interpreter{};
  for (int i = 0; i < 400; ++i) rig.block();  // warm up
  std::vector<double> steady, window, fades, interpWindow;
  for (int i = 0; i < 3000; ++i) steady.push_back(rig.block());
  std::vector<double> audibleMs, frontMs, buildMs, jitMs;
  for (int k = 0; k < rounds; ++k) {
    for (int i = 0; i < 300; ++i) steady.push_back(rig.block());
    const auto t0 = std::chrono::steady_clock::now();
    proj.trackPatches[kEditTrack]->constant(level) = g::Constant::real(0.3 + 0.05 * (k % 2));
    const uint32_t tracks[] = {kEditTrack};
    auto upd = compiler.update(proj.graph, tracks, &interpreter);
    if (!upd.ok() || upd.value().size() != 1 || upd.value()[0].unchanged) {
      std::fprintf(stderr, "update failed: %s\n", upd.ok() ? "unchanged" : upd.error().message.c_str());
      std::exit(6);
    }
    frontMs.push_back(compiler.unit(unit).frontEndMs);
    buildMs.push_back(compiler.unit(unit).buildMs);
    const uint64_t seqBefore = rig.program->unitStats(unit).sequence;
    if (!rig.program->publish(unit, std::move(upd.value()[0].image)).ok()) std::exit(7);
    // The next block swaps: the change is audible from its first sample.
    double ns = rig.block();
    window.push_back(ns);
    fades.push_back(ns);
    if (rig.program->unitStats(unit).sequence == seqBefore) std::exit(8);
    audibleMs.push_back(msSince(t0));
    while (rig.program->crossfading(unit)) {
      ns = rig.block();
      window.push_back(ns);
      fades.push_back(ns);
    }
    if (jit != nullptr) {
      // The JIT compiles the new schedule on another core while the audio thread plays the interpreted unit.
      std::atomic<bool> done{false};
      Result<std::unique_ptr<e::ProgramImage>> img = Error{ErrorCode::Unknown, "", {}};
      double ms = 0;
      std::thread t = bench::pinnedThread(bench::cpuTopology().coreMask(3), [&] {
        const auto j0 = std::chrono::steady_clock::now();
        img = jitBuild(jit, *compiler.unit(unit).schedule, kBlock, e::voiceGroupsFor(8), nullptr);
        ms = msSince(j0);
        done.store(true, std::memory_order_release);
      });
      // Real time: one block per 1.33 ms, so the interpreted window lasts as long as the compile.
      auto next = std::chrono::steady_clock::now();
      while (!done.load(std::memory_order_acquire)) {
        ns = rig.block();
        window.push_back(ns);
        interpWindow.push_back(ns);
        next += std::chrono::microseconds(1333);
        while (std::chrono::steady_clock::now() < next && !done.load(std::memory_order_acquire)) std::this_thread::yield();
      }
      t.join();
      jitMs.push_back(ms);
      if (!img.ok()) {
        std::fprintf(stderr, "jit: %s\n", img.error().message.c_str());
        std::exit(9);
      }
      auto bound = compiler.bind(unit, std::move(img.value()));
      if (!bound.ok() || !rig.program->publish(unit, std::move(bound.value())).ok()) std::exit(10);
      ns = rig.block();
      window.push_back(ns);
      fades.push_back(ns);
      while (rig.program->crossfading(unit)) {
        ns = rig.block();
        window.push_back(ns);
        fades.push_back(ns);
      }
    } else {
      for (int i = 0; i < 100; ++i) {
        ns = rig.block();
        window.push_back(ns);
        interpWindow.push_back(ns);
      }
    }
    rig.program->collect();
  }
  const bench::Stats st = bench::summarize(steady), wi = bench::summarize(window), fa = bench::summarize(fades);
  std::string n = std::string(prefix) + "steady state";
  printStats(n.c_str(), steady);
  n = std::string(prefix) + "edit window (all blocks)";
  printStats(n.c_str(), window);
  n = std::string(prefix) + "crossfade blocks";
  printStats(n.c_str(), fades);
  if (!interpWindow.empty()) {
    n = std::string(prefix) + "edited unit interpreted";
    printStats(n.c_str(), interpWindow);
  }
  std::printf("BENCH %sratios: edit-window max / steady max %.3f, p99/p99 %.3f, median/median %.3f; crossfade median / "
              "steady median %.3f, crossfade max / steady max %.3f\n",
              prefix, wi.maxNs / st.maxNs, wi.p99Ns / st.p99Ns, wi.medianNs / st.medianNs, fa.medianNs / st.medianNs,
              fa.maxNs / st.maxNs);
  const bench::Stats au = bench::summarize(audibleMs), fr = bench::summarize(frontMs), bu = bench::summarize(buildMs);
  std::printf("BENCH %sedit -> audible: median %.2f ms, max %.2f ms (front end median %.2f ms, interpreter build %.2f ms, "
              "+ swap block; a device adds <= 1 period = 1.33 ms)\n",
              prefix, au.medianNs, au.maxNs, fr.medianNs, bu.medianNs);
  if (!jitMs.empty()) {
    const bench::Stats j = bench::summarize(jitMs);
    std::printf("BENCH %sJIT of the edited unit (%u ops): median %.0f ms, max %.0f ms (background; interpreted meanwhile)\n",
                prefix, compiler.unit(unit).ops, j.medianNs, j.maxNs);
  }
}

double privateMb() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
}

Result<std::unique_ptr<e::ProgramImage>> jitQuick(void* user, const g::Schedule& s, uint32_t maxBlock, uint32_t groups,
                                                  const e::Resources* res, bool* refine) {
  jit::CompileOptions o;
  o.maxBlockFrames = maxBlock;
  o.maxVoiceGroups = groups;
  o.resources = res;
  o.quickFirst = true;
  jit::CompileStats st;
  auto img = static_cast<jit::Compiler*>(user)->compile(s, o, &st);
  *refine = img.ok() && st.refine;
  return img;
}

// PF.4: the fixture's cold open through BackgroundCompiler at 1/2/4/6 workers (see the header).
int threadsSweep(const e::ProjectCompiler& units, int rounds, uint32_t only) {
  struct Run {
    std::mutex m;
    uint32_t units = 0, firstOrFinal = 0, finals = 0;
    std::chrono::steady_clock::time_point t0, allFirst, allFinal;
    std::vector<std::unique_ptr<e::ProgramImage>> keep;
  };
  for (int round = 0; round < rounds; ++round)
    for (uint32_t threads : {1u, 2u, 4u, 6u}) {
      if (only != 0 && threads != only) continue;
      auto jc = jit::Compiler::create();
      if (!jc.ok()) return 20;
      Run run;
      run.units = units.numUnits();
      e::BackgroundCompilerConfig bc;
      bc.backend = {&jitBuild, jc.value().get(), &jitQuick};
      bc.threads = threads;
      bc.user = &run;
      bc.deliver = [](void* user, const e::BackgroundJob& job, Result<std::unique_ptr<e::ProgramImage>> img) {
        Run& r = *static_cast<Run*>(user);
        if (!img.ok()) std::exit(21);
        std::lock_guard lock(r.m);
        const auto now = std::chrono::steady_clock::now();
        if (!job.refine && ++r.firstOrFinal == r.units) r.allFirst = now;
        if (!job.first && ++r.finals == r.units) r.allFinal = now;
        r.keep.push_back(std::move(img.value()));  // keeps every code alive (no release while compiling)
      };
      std::vector<e::BackgroundJob> jobs;
      for (uint32_t u = 0; u < units.numUnits(); ++u) {
        e::BackgroundJob j;
        j.schedule = units.unit(u).schedule;
        j.maxBlockFrames = kBlock;
        j.maxVoiceGroups = units.imageVoiceGroups(u);
        j.tag = u;
        jobs.push_back(std::move(j));
      }
      const double mb0 = privateMb();
      std::atomic<bool> sampling{true};
      std::atomic<double> peak{mb0};
      std::thread sampler([&] {
        while (sampling.load()) {
          const double mb = privateMb();
          if (mb > peak.load()) peak.store(mb);
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      });
      {
        e::BackgroundCompiler worker(bc);
        run.t0 = std::chrono::steady_clock::now();
        worker.want(std::move(jobs));
        if (!worker.waitIdle(600.0)) return 22;
      }
      sampling.store(false);
      sampler.join();
      const jit::CacheCounters cc = jc.value()->counters();
      auto ms = [&](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(t - run.t0).count();
      };
      std::printf("BENCH parallel/%u threads: every unit on a JIT image after %.0f ms, every final image after %.0f ms; "
                  "%llu LLVM compiles; peak private memory %.0f MB (+%.0f MB over the start)\n",
                  threads, ms(run.allFirst), ms(run.allFinal), static_cast<unsigned long long>(cc.compiles), peak.load(),
                  peak.load() - mb0);
      std::fflush(stdout);
    }
  return 0;
}

// PF.4: the largest unit's final JIT image (what an edit of it waits for), `rounds` fresh compiles: median times.
int largestUnit(const e::ProjectCompiler& units, int rounds) {
  uint32_t big = 0;
  for (uint32_t u = 1; u < units.numUnits(); ++u)
    if (units.unit(u).ops > units.unit(big).ops) big = u;
  std::vector<double> total, gen, opt, mc, quick;
  jit::CompileStats st;
  for (int r = 0; r < rounds; ++r) {
    auto jc = jit::Compiler::create();
    if (!jc.ok()) return 30;
    jit::CompileOptions o;
    o.maxBlockFrames = kBlock;
    o.maxVoiceGroups = units.imageVoiceGroups(big);
    o.quickFirst = true;
    jit::CompileStats qs;
    auto first = jc.value()->compile(*units.unit(big).schedule, o, &qs);
    if (!first.ok()) return 31;
    quick.push_back(qs.totalMs);
    o.quickFirst = false;
    auto img = jc.value()->compile(*units.unit(big).schedule, o, &st);
    if (!img.ok()) return 31;
    total.push_back(st.totalMs);
    gen.push_back(st.codegenMs);
    opt.push_back(st.optimiseMs);
    mc.push_back(st.machineCodeMs);
  }
  auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  std::printf("BENCH largest unit (%u ops, %u IR instructions, %llu B of code): JIT median %.1f ms (min %.1f): IR generation "
              "%.1f, IR passes %.1f, machine code %.1f ms (%d compiles); its quick first image %.1f ms\n",
              units.unit(big).ops, st.irInstructions, static_cast<unsigned long long>(st.objectBytes), median(total),
              *std::min_element(total.begin(), total.end()), median(gen), median(opt), median(mc), rounds,
              median(quick));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  int rounds = 5;
  bool wholeJit = false, sweep = false;
  int sweepRounds = 1;
  uint32_t sweepOnly = 0;
  int largest = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) rounds = std::max(1, std::atoi(argv[++i]));
    if (std::strcmp(argv[i], "--whole-jit") == 0) wholeJit = true;
    if (std::strcmp(argv[i], "--threads-sweep") == 0) sweep = true;
    if (std::strcmp(argv[i], "--largest") == 0 && i + 1 < argc) largest = std::max(1, std::atoi(argv[++i]));
    if (std::strcmp(argv[i], "--sweep-rounds") == 0 && i + 1 < argc) sweepRounds = std::max(1, std::atoi(argv[++i]));
    if (std::strcmp(argv[i], "--sweep-threads") == 0 && i + 1 < argc) sweepOnly = static_cast<uint32_t>(std::atoi(argv[++i]));
  }
  bench::pinCurrentThread(bench::cpuTopology().coreMask(1));
  bench::raisePriority();
  bench::printPlacement("audio + compile thread", bench::cpuTopology().coreMask(1));
  DenormalGuard dg;
  bench::SystemLoad load;
  const project_fixture::Options fo{30, 28, 3, 5};
  auto proj = project_fixture::makeProject(fo);
  if (!proj) return 1;
  auto jc = jit::Compiler::create();
  if (!jc.ok()) return 2;
  jit::Compiler* jit = jc.value().get();

  // ---- compile ---------------------------------------------------------------------------------------------
  e::ProjectCompiler interp(options(nullptr));
  auto t0 = std::chrono::steady_clock::now();
  auto bi = interp.build(proj->graph);
  const double interpTotal = msSince(t0);
  if (!bi.ok()) {
    std::fprintf(stderr, "%s\n", bi.error().message.c_str());
    return 3;
  }
  if (largest > 0) return largestUnit(interp, largest);
  if (sweep) return threadsSweep(interp, sweepRounds, sweepOnly);  // (new threads take the process's affinity, not this one's)
  const BuildTimes ti = times(interp);
  std::printf("T1.x-arch1 per-track units, 30-track fixture: %u units, %u schedule ops (largest unit %u), block %u, 48 kHz\n",
              interp.numUnits(), ti.ops, ti.maxUnitOps, kBlock);
  for (const std::string& note : interp.partition().notes) std::printf("  note: %s\n", note.c_str());
  std::printf("BENCH compile/per-unit interpreted: %.0f ms total (front end %.0f ms, interpreter %.0f ms); largest unit "
              "front end %.2f ms, interpreter %.2f ms\n",
              interpTotal, ti.frontMs, ti.backMs, ti.maxUnitFrontMs, ti.maxUnitBackMs);
  e::ProjectCompiler jitc(options(jit));
  t0 = std::chrono::steady_clock::now();
  auto bj = jitc.build(proj->graph);
  const double jitTotal = msSince(t0);
  if (!bj.ok()) {
    std::fprintf(stderr, "%s\n", bj.error().message.c_str());
    return 4;
  }
  const BuildTimes tj = times(jitc);
  std::printf("BENCH compile/per-unit JIT: %.0f ms total (JIT %.0f ms = %.3f ms/op); largest unit JIT %.0f ms\n", jitTotal,
              tj.backMs, tj.backMs / tj.ops, tj.maxUnitBackMs);
  {
    e::ProjectCompiler whole(options(nullptr));
    t0 = std::chrono::steady_clock::now();
    auto bw = whole.buildWhole(proj->graph);
    const double wt = msSince(t0);
    if (!bw.ok()) return 5;
    std::printf("BENCH compile/whole-project interpreted: %.0f ms (front end %.0f ms, interpreter %.0f ms), %u ops: the cost "
                "of every edit without units\n",
                wt, whole.unit(0).frontEndMs, whole.unit(0).buildMs, whole.unit(0).ops);
    if (wholeJit) {
      e::ProjectCompiler wj(options(jit));
      t0 = std::chrono::steady_clock::now();
      auto bwj = wj.buildWhole(proj->graph);
      std::printf("BENCH compile/whole-project JIT: %.0f ms (%s)\n", msSince(t0), bwj.ok() ? "ok" : bwj.error().message.c_str());
    }
  }

  // ---- equal: per-unit vs whole, interpreted, same notes on every instrument --------------------------------
  {
    e::ProjectCompiler a(options(nullptr)), w(options(nullptr));
    auto ba = a.build(proj->graph);
    auto bw = w.buildWhole(proj->graph);
    if (!ba.ok() || !bw.ok()) return 6;
    auto ra = makeRig(ba.value(), fo.numTracks, true), rw = makeRig(bw.value(), fo.numTracks, true);
    double err = 0.0, pk = 0.0;
    std::vector<double> ta, tw;
    for (int i = 0; i < 750; ++i) {
      ta.push_back(ra->block());
      tw.push_back(rw->block());
      for (uint32_t f = 0; f < kBlock; ++f) {
        err = std::max(err, std::fabs(static_cast<double>(ra->l[f]) - rw->l[f]) / std::max(1.0, std::fabs(static_cast<double>(rw->l[f]))));
        pk = std::max(pk, std::fabs(static_cast<double>(rw->l[f])));
      }
    }
    std::printf("BENCH equal/per-unit vs whole-project (interpreted, 1 s): max |diff| %.3g (peak %.3f)\n", err, pk);
    printStats("equal/per-unit interpreted block", ta);
    printStats("equal/whole-project interpreted block", tw);
  }

  // ---- edit ----------------------------------------------------------------------------------------------------
  const g::NodeIndex level = levelNode(*proj, kEditTrack);
  {
    auto rig = makeRig(bj.value(), fo.numTracks, false);
    editScenario("edit/jit ", *proj, jitc, *rig, jit, rounds, level);
  }
  {
    auto rig = makeRig(bi.value(), fo.numTracks, false);
    editScenario("edit/interp ", *proj, interp, *rig, nullptr, rounds, level);
  }
  std::printf("system load during the run: %.0f %%\n", load.busyPercent());
  return 0;
}
