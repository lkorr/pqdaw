// T1b.4: the JIT compile cache on the 30-track fixture (tests/engine/project_fixture.h: 32 units, ≈ 21k schedule ops)
// — QUESTIONS Q108's cold open: the project starts on interpreted units, the background compiler (one below-normal
// worker, as ProjectLink) builds every unit's JIT image while the "audio thread" plays at real-time pace, and each
// image is published with an Instant swap as it arrives. Three opens, each with a new jit::Compiler (a new process):
//   nocache   no disk cache (T1b.5's situation)
//   cold      an empty disk cache: every unit compiles and is stored
//   warm      the same directory again: every unit loads (no LLVM codegen)
// Per open: the time from the want() until every unit runs its JIT image, the blocks meanwhile, and per unit the
// compile or load time (CompileStats). Plus: equal = the warm open's output vs the cold open's (must be 0).
// Release / ship build. Usage: pq_bench_jit_cache [--blocks N] [--dir <cache dir>] (default: a temp directory)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
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
#include "pq/jit/compiler.h"
#include "pq_bench.h"

using namespace pq;
namespace e = pq::engine;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;

std::vector<e::TimedNote> chord(uint32_t k) {
  const float base = 110.0f * (1.0f + 0.125f * static_cast<float>(k % 8));
  return {{0, 48000 * 600, base, 0.8f, 0.5f}, {0, 48000 * 600, base * 1.25f, 0.7f, 0.5f}, {0, 48000 * 600, base * 1.5f, 0.6f, 0.5f}};
}

// The compiler's per-image statistics, collected by the backend.
struct Backend {
  jit::Compiler* compiler = nullptr;
  std::mutex m;
  std::vector<jit::CompileStats> stats;
  static Result<std::unique_ptr<e::ProgramImage>> build(void* self, const graph::Schedule& s, uint32_t maxBlock, uint32_t groups,
                                                         const e::Resources* resources) {
    Backend& b = *static_cast<Backend*>(self);
    jit::CompileOptions o;
    o.maxBlockFrames = maxBlock;
    o.maxVoiceGroups = groups;
    o.resources = resources;
    jit::CompileStats st;
    auto img = b.compiler->compile(s, o, &st);
    std::lock_guard lock(b.m);
    b.stats.push_back(st);
    return img;
  }
};

struct Rig {
  std::unique_ptr<project_fixture::Project> proj;
  std::unique_ptr<e::ProjectCompiler> compiler;
  std::vector<std::unique_ptr<e::NoteListSource>> notes;
  std::unique_ptr<e::ProjectProgram> program;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  std::vector<float> out;
  int64_t time = 0;
  std::atomic<uint32_t> published{0}, stale{0};

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
  void block() {
    float* outs[2] = {l.data(), r.data()};
    e::AudioBlock ab;
    ab.out = outs;
    ab.numOut = 2;
    ab.frames = kBlock;
    ab.frameTime = time;
    program->process(ab);
    time += kBlock;
    out.insert(out.end(), l.begin(), l.end());
    out.insert(out.end(), r.begin(), r.end());
  }
  bool allJit() const {
    for (uint32_t u = 0; u < compiler->numUnits(); ++u)
      if (program->current(u) == nullptr || program->current(u)->io().backend != e::ImageBackend::Jit) return false;
    return true;
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
    if (bound.ok() && rig.program->publish(u, std::move(bound.value()), e::SwapPolicy{e::SwapMode::Instant}).ok()) ++rig.published;
    else ++rig.stale;
  }
};

struct Open {
  double promoteMs = 0, workerMs = 0;
  size_t blocks = 0;
  std::vector<jit::CompileStats> stats;
  jit::CacheCounters counters;
  std::vector<float> out;
};

Open openProject(const project_fixture::Options& fo, const jit::CacheConfig& cache, uint32_t afterBlocks) {
  jit::CompilerConfig cc;
  cc.cache = cache;
  auto jc = jit::Compiler::create(cc);
  if (!jc) std::exit(5);
  Backend be;
  be.compiler = jc->get();
  Rig rig(fo);
  e::BackgroundCompilerConfig bc;
  bc.backend = {&Backend::build, &be};
  bc.deliver = &Rig::deliver;
  bc.user = &rig;
  e::BackgroundCompiler worker(bc);
  std::vector<e::BackgroundJob> jobs;
  for (uint32_t u = 0; u < rig.compiler->numUnits(); ++u) {
    e::BackgroundJob j;
    j.schedule = rig.compiler->unit(u).schedule;
    j.maxBlockFrames = kBlock;
    j.maxVoiceGroups = rig.compiler->imageVoiceGroups(u);
    jobs.push_back(std::move(j));
  }
  Open o;
  rig.block();  // the interpreted images become current (the project plays at once, as after ProjectLink's compile)
  const auto t0 = std::chrono::steady_clock::now();
  worker.want(std::move(jobs));
  auto next = std::chrono::steady_clock::now();
  while (!rig.allJit()) {  // real time: one block per 1.333 ms
    rig.block();
    ++o.blocks;
    next += std::chrono::microseconds(1333);
    while (std::chrono::steady_clock::now() < next) std::this_thread::yield();
  }
  o.promoteMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  worker.waitIdle();
  o.workerMs = worker.stats().totalBuildMs;
  rig.program->collect();
  // A fixed number of blocks in total, so the opens' outputs line up sample for sample.
  while (rig.out.size() < static_cast<size_t>(afterBlocks) * kBlock * 2) rig.block();
  o.out = std::move(rig.out);
  o.stats = be.stats;
  o.counters = (*jc)->counters();
  return o;
}

void report(const char* name, const Open& o) {
  std::vector<double> compileMs, loadMs, hostMs, cacheMs;
  double objectKiB = 0;
  for (const jit::CompileStats& s : o.stats) {
    if (s.source == jit::CompileStats::Source::Compiled) compileMs.push_back(s.totalMs);
    else loadMs.push_back(s.totalMs);
    hostMs.push_back(s.hostMs);
    cacheMs.push_back(s.cacheMs);
    objectKiB += static_cast<double>(s.objectBytes) / 1024.0;
  }
  auto median = [](std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  auto sum = [](const std::vector<double>& v) {
    double t = 0;
    for (double x : v) t += x;
    return t;
  };
  std::printf("BENCH %-8s all units JIT %8.0f ms after the want (%zu blocks at real time); worker %8.0f ms; %zu compiled "
              "(median %.1f ms, sum %.0f ms), %zu cached (median %.2f ms, sum %.1f ms); object %.0f KiB; counters: "
              "compiles %llu, disk hits %llu, stores %llu\n",
              name, o.promoteMs, o.blocks, o.workerMs, compileMs.size(), median(compileMs), sum(compileMs), loadMs.size(),
              median(loadMs), sum(loadMs), objectKiB, static_cast<unsigned long long>(o.counters.compiles),
              static_cast<unsigned long long>(o.counters.diskHits), static_cast<unsigned long long>(o.counters.stores));
  std::printf("BENCH %-8s per image: host (memory, state) median %.2f ms, cache (key, lookup, load/link or store) median "
              "%.2f ms\n",
              name, median(hostMs), median(cacheMs));
}

}  // namespace

int main(int argc, char** argv) {
  uint32_t blocks = 8000;
  fs::path dir;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) blocks = std::max(100, std::atoi(argv[++i]));
    else if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) dir = argv[++i];
  }
  const bool ownDir = dir.empty();
  if (ownDir) dir = fs::temp_directory_path() / ("pq_bench_jit_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  bench::pinCurrentThread(bench::cpuTopology().coreMask(1));
  bench::raisePriority();
  bench::printPlacement("audio thread", bench::cpuTopology().coreMask(1));
  DenormalGuard dg;
  bench::SystemLoad load;
  const project_fixture::Options fo{30, 28, 3, 5};
  std::printf("T1b.4 JIT compile cache, 30-track fixture, block %u, 48 kHz, cache %s\n", kBlock, dir.string().c_str());

  jit::CacheConfig none;
  jit::CacheConfig disk;
  disk.dir = dir;
  const Open nocache = openProject(fo, none, blocks);
  const Open cold = openProject(fo, disk, blocks);
  const Open warm = openProject(fo, disk, blocks);
  report("nocache", nocache);
  report("cold", cold);
  report("warm", warm);
  uint64_t bytes = 0, files = 0;
  for (const auto& f : fs::directory_iterator(dir, ec))
    if (f.path().extension() == ".pqjit") bytes += fs::file_size(f.path(), ec), ++files;
  std::printf("BENCH cache directory: %llu entries, %.1f MiB\n", static_cast<unsigned long long>(files),
              static_cast<double>(bytes) / (1024.0 * 1024.0));
  std::printf("BENCH first compile's cache overhead (includes hashing the executable once): cold %.1f ms\n",
              cold.stats.empty() ? 0.0 : cold.stats.front().cacheMs);
  size_t diff = 0;
  const size_t n = std::min(cold.out.size(), warm.out.size());
  for (size_t i = 0; i < n; ++i) diff += std::memcmp(&cold.out[i], &warm.out[i], sizeof(float)) != 0;
  for (size_t i = 0; i < std::min(n, nocache.out.size()); ++i) diff += std::memcmp(&cold.out[i], &nocache.out[i], sizeof(float)) != 0;
  std::printf("BENCH equal: %zu of %zu samples differ between the opens (cold / warm / no cache)\n", diff, n);
  std::printf("system load during the run: %.0f %%\n", load.busyPercent());
  if (ownDir) fs::remove_all(dir, ec);
  return diff == 0 && warm.counters.compiles == 0 ? 0 : 6;
}
