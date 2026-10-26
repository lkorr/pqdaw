// T1b.5: the interpreter → JIT live swap on the 30-track fixture (tests/engine/project_fixture.h: 32 units, ≈ 21k
// schedule ops, 30 FM-voice instruments holding chords). The project starts on interpreted units (what a structural
// compile publishes at once), the background compiler (pq/engine/background_compile.h: one below-normal worker, the
// JIT through pq::compile::jitUnitBackend) builds every unit's JIT image while the "audio thread" plays at real-time
// pace, and each image is published with an Instant swap as it arrives (what ProjectLink does).
//   interp/steady     ns per 64-frame block, every unit interpreted
//   transition        every block from the first want() until the last JIT image is current (real-time pace), and the
//                     time that takes (wall clock; the worker shares the machine)
//   jit/steady        ns per block once every unit runs its JIT image (acceptance: below interp/steady)
//   equal             the whole run vs a project that stays interpreted: samples that differ (acceptance: 0)
//   --quick           T1b.x-perf (QUESTIONS Q111 option C): the backend builds quick first images (voice groups) and
//                     the pair images after them (compile::jitUnitBackend(true), what ProjectLink does); "promotion"
//                     is then the time until every unit runs a JIT image, "final" until every unit runs its final one
// Release / ship build. Usage: pq_bench_live_swap [--blocks N] [--quick]
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
#include "pq/compile/compile.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/background_compile.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq_bench.h"

using namespace pq;
namespace e = pq::engine;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;

std::vector<e::TimedNote> chord(uint32_t k) {
  const float base = 110.0f * (1.0f + 0.125f * static_cast<float>(k % 8));
  return {{0, 48000 * 600, base, 0.8f, 0.5f}, {0, 48000 * 600, base * 1.25f, 0.7f, 0.5f}, {0, 48000 * 600, base * 1.5f, 0.6f, 0.5f}};
}

struct Rig {
  std::unique_ptr<project_fixture::Project> proj;
  std::unique_ptr<e::ProjectCompiler> compiler;
  std::vector<std::unique_ptr<e::NoteListSource>> notes;
  std::unique_ptr<e::ProjectProgram> program;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  std::vector<float> out;
  int64_t time = 0;
  std::atomic<uint32_t> published{0}, stale{0}, finals{0};

  explicit Rig(const project_fixture::Options& fo) {
    proj = project_fixture::makeProject(fo);
    if (!proj) std::exit(1);
    e::ProjectCompileOptions co;
    co.sampleRate = kSr;
    co.maxBlockFrames = kBlock;
    co.voice.maxVoices = 8;
    co.voice.silenceDb = -300.0f;
    compiler = std::make_unique<e::ProjectCompiler>(co);
    auto b = compiler->build(proj->graph);
    if (!b.ok()) {
      std::fprintf(stderr, "%s\n", b.error().message.c_str());
      std::exit(2);
    }
    e::ProjectConfig pc;
    pc.sampleRate = kSr;
    pc.maxBlockFrames = kBlock;
    for (uint32_t k = 0; k < fo.numTracks; ++k) {
      notes.push_back(std::make_unique<e::NoteListSource>(chord(k)));
      pc.eventSources.push_back(notes.back()->source());
    }
    auto prog = e::ProjectProgram::create(pc, b.value().layout);
    if (!prog.ok()) std::exit(3);
    program = std::move(prog.value());
    for (uint32_t u = 0; u < b.value().images.size(); ++u)
      if (!program->publish(u, std::move(b.value().images[u])).ok()) std::exit(4);
  }
  double block() {  // ns of one process() call; the output is kept for the comparison
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
    out.insert(out.end(), l.begin(), l.end());
    out.insert(out.end(), r.begin(), r.end());
    return ns;
  }
  static void deliver(void* self, const e::BackgroundJob& job, Result<std::unique_ptr<e::ProgramImage>> img) {
    Rig& rig = *static_cast<Rig*>(self);
    const uint32_t u = img.ok() ? rig.compiler->unitOf(job.schedule.get()) : e::kNoUnit;
    if (u == e::kNoUnit) {
      ++rig.stale;
      return;
    }
    auto bound = rig.compiler->bind(u, std::move(img.value()));
    while (bound.ok() && !rig.program->canPublish(u)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (bound.ok() && rig.program->publish(u, std::move(bound.value()), e::SwapPolicy{e::SwapMode::Instant}).ok()) {
      ++rig.published;
      if (!job.first) ++rig.finals;
    } else {
      ++rig.stale;
    }
  }
};

void printStats(const char* name, const std::vector<double>& v) {
  const bench::Stats s = bench::summarize(v);
  std::printf("BENCH %-28s median %9.0f  p99 %9.0f  max %9.0f ns/block  (%zu blocks)\n", name, s.medianNs, s.p99Ns,
              s.maxNs, s.samples);
}

}  // namespace

int main(int argc, char** argv) {
  uint32_t blocks = 3000;
  bool quick = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) blocks = std::max(100, std::atoi(argv[++i]));
    if (std::strcmp(argv[i], "--quick") == 0) quick = true;
  }
  bench::pinCurrentThread(bench::cpuTopology().coreMask(1));
  bench::raisePriority();
  bench::printPlacement("audio thread", bench::cpuTopology().coreMask(1));
  DenormalGuard dg;
  bench::SystemLoad load;
  const project_fixture::Options fo{30, 28, 3, 5};
  Rig live(fo);
  const uint32_t units = live.compiler->numUnits();
  uint32_t ops = 0;
  for (uint32_t u = 0; u < units; ++u) ops += live.compiler->unit(u).ops;
  std::printf("T1b.5 interpreter -> JIT live swap, 30-track fixture: %u units, %u schedule ops, block %u, 48 kHz\n", units,
              ops, kBlock);
  if (!compile::jitCompiler().ok()) return 5;  // LLVM initialised before the clock starts

  std::vector<double> interp, transition, jit;
  for (int i = 0; i < 400; ++i) live.block();  // warm up (and the notes' attacks)
  for (uint32_t i = 0; i < blocks; ++i) interp.push_back(live.block());

  e::BackgroundCompilerConfig bc;
  bc.backend = compile::jitUnitBackend(quick);
  bc.deliver = &Rig::deliver;
  bc.user = &live;
  e::BackgroundCompiler worker(bc);
  std::vector<e::BackgroundJob> jobs;
  for (uint32_t u = 0; u < units; ++u) {
    e::BackgroundJob j;
    j.schedule = live.compiler->unit(u).schedule;
    j.maxBlockFrames = kBlock;
    j.maxVoiceGroups = live.compiler->imageVoiceGroups(u);
    jobs.push_back(std::move(j));
  }
  const auto t0 = std::chrono::steady_clock::now();
  worker.want(std::move(jobs));
  // Real time: one block per 1.333 ms while the worker compiles on whatever core the OS gives it.
  auto next = std::chrono::steady_clock::now();
  auto allJit = [&] {
    for (uint32_t u = 0; u < units; ++u)
      if (live.program->current(u)->io().backend != e::ImageBackend::Jit) return false;
    return true;
  };
  while (!allJit()) {
    transition.push_back(live.block());
    next += std::chrono::microseconds(1333);
    while (std::chrono::steady_clock::now() < next) std::this_thread::yield();
  }
  const double promoteMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  while (live.finals.load() < units) {  // the final images (quick: the refinements), still at real-time pace
    transition.push_back(live.block());
    next += std::chrono::microseconds(1333);
    while (std::chrono::steady_clock::now() < next) std::this_thread::yield();
  }
  const double finalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  worker.waitIdle();
  live.program->collect();
  for (uint32_t i = 0; i < blocks; ++i) jit.push_back(live.block());

  // The same blocks on a project that never leaves the interpreter.
  Rig ref(fo);
  while (ref.out.size() < live.out.size()) ref.block();
  size_t diff = 0;
  for (size_t i = 0; i < live.out.size(); ++i) diff += std::memcmp(&live.out[i], &ref.out[i], sizeof(float)) != 0;

  printStats("interp/steady", interp);
  printStats("transition (real time)", transition);
  printStats("jit/steady", jit);
  const bench::Stats si = bench::summarize(interp), sj = bench::summarize(jit), st = bench::summarize(transition);
  const e::BackgroundStats bs = worker.stats();
  std::printf("BENCH ratio interp/jit steady: median %.2f, p99 %.2f; transition max / interp max %.3f\n",
              si.medianNs / sj.medianNs, si.p99Ns / sj.p99Ns, st.maxNs / si.maxNs);
  std::printf("BENCH promotion%s: all %u units on JIT images %.0f ms after the want, on their final images %.0f ms "
              "(background builds %.0f ms total, %u published, %u stale)\n",
              quick ? " (quick first images)" : "", units, promoteMs, finalMs, bs.totalBuildMs, live.published.load(),
              live.stale.load());
  std::printf("BENCH equal: %zu of %zu samples differ from the interpreted-only run\n", diff, live.out.size());
  std::printf("system load during the run: %.0f %%\n", load.busyPercent());
  return diff == 0 ? 0 : 6;
}
