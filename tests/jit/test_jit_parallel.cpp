// PF.4: parallel JIT compiles (the BackgroundCompiler pool on one jit::Compiler) and big units split by function into
// two objects. On a generated per-track project (tests/engine/project_fixture.h) whose units are big enough to be
// split:
//   - the same machine code whatever the number of compile threads: the disk-cache entries written by one worker and
//     by six are the same files, byte for byte (the cache key unchanged);
//   - split images, and split images reloaded from the disk cache, play exactly what the interpreted project plays;
//   - none of them needs stack probes.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../engine/project_fixture.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/background_compile.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/jit/compiler.h"

namespace eng = pq::engine;
namespace g = pq::graph;
namespace jit = pq::jit;
namespace fs = std::filesystem;

namespace {

const project_fixture::Options kFixture{4, 12, 3, 1};  // 4 FM instruments, 12 effects each: units over the split size

struct TempDir {
  fs::path path;
  explicit TempDir(const char* tag) {
    static std::atomic<uint32_t> n{0};
    path = fs::temp_directory_path() / ("pq_jit_parallel_" + std::string(tag) + "_" +
                                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                                        std::to_string(n++));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  // name → bytes of every entry
  std::vector<std::pair<std::string, std::string>> entries() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& e : fs::directory_iterator(path))
      if (e.path().extension() == ".pqjit") {
        std::ifstream f(e.path(), std::ios::binary);
        out.emplace_back(e.path().filename().string(), std::string(std::istreambuf_iterator<char>(f), {}));
      }
    std::sort(out.begin(), out.end());
    return out;
  }
};

std::unique_ptr<jit::Compiler> compilerWith(const fs::path& dir) {
  jit::CompilerConfig cfg;
  cfg.cache.dir = dir;
  auto c = jit::Compiler::create(cfg);
  REQUIRE(c.ok());
  return std::move(c.value());
}

// What a unit's image is built with, and what the builds reported.
struct Backend {
  jit::Compiler* compiler = nullptr;
  std::mutex m;
  uint32_t images = 0, split = 0, probes = 0;
  std::vector<jit::CompileStats::Source> sources;
  static pq::Result<std::unique_ptr<eng::ProgramImage>> build(void* self, const g::Schedule& s, uint32_t maxBlock,
                                                              uint32_t groups, const eng::Resources* res) {
    Backend& b = *static_cast<Backend*>(self);
    jit::CompileOptions o;
    o.maxBlockFrames = maxBlock;
    o.maxVoiceGroups = groups;
    o.resources = res;
    jit::CompileStats st;
    auto img = b.compiler->compile(s, o, &st);
    std::lock_guard lock(b.m);
    ++b.images;
    b.split += st.objects >= 2 ? 1u : 0u;
    b.probes += st.stackProbes ? 1u : 0u;
    b.sources.push_back(st.source);
    return img;
  }
};

// The fixture played for a while; `backend` null: interpreted.
std::vector<float> render(Backend* backend) {
  auto proj = project_fixture::makeProject(kFixture);
  REQUIRE(proj != nullptr);
  eng::ProjectCompileOptions co;
  co.sampleRate = 48000.0;
  co.maxBlockFrames = 64;
  co.voice.maxVoices = 8;
  if (backend != nullptr) co.backend = {&Backend::build, backend};
  eng::ProjectCompiler compiler(co);
  auto b = compiler.build(proj->graph);
  REQUIRE_MESSAGE(b.ok(), (b.ok() ? "" : b.error().message));
  std::vector<std::unique_ptr<eng::NoteListSource>> notes;
  eng::ProjectConfig pc;
  pc.sampleRate = 48000.0;
  pc.maxBlockFrames = 64;
  for (uint32_t k = 0; k < kFixture.numTracks; ++k) {
    const float hz = 110.0f * (1.0f + 0.25f * static_cast<float>(k));
    notes.push_back(std::make_unique<eng::NoteListSource>(
        std::vector<eng::TimedNote>{{0, 6000, hz, 0.8f, 0.5f}, {2400, 8000, hz * 1.5f, 0.6f, 0.5f}}));
    pc.eventSources.push_back(notes.back()->source());
  }
  auto prog = eng::ProjectProgram::create(pc, b.value().layout);
  REQUIRE(prog.ok());
  for (uint32_t u = 0; u < b.value().images.size(); ++u) REQUIRE(prog.value()->publish(u, std::move(b.value().images[u])).ok());
  std::vector<float> out, l(64), r(64);
  float* outs[2] = {l.data(), r.data()};
  uint64_t rt = 0;
  for (int64_t t = 0; t < 12000; t += 64) {
    eng::AudioBlock ab;
    ab.out = outs;
    ab.numOut = 2;
    ab.frames = 64;
    ab.frameTime = t;
    rt += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      prog.value()->process(ab);
    });
    out.insert(out.end(), l.begin(), l.end());
    out.insert(out.end(), r.begin(), r.end());
  }
  CHECK(rt == 0);
  prog.value()->collect();
  return out;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// Every unit of the fixture through a BackgroundCompiler of `threads` workers (final images, no quick ones).
void compileAll(jit::Compiler& c, uint32_t threads, uint32_t& built) {
  auto proj = project_fixture::makeProject(kFixture);
  REQUIRE(proj != nullptr);
  eng::ProjectCompileOptions co;
  co.sampleRate = 48000.0;
  co.maxBlockFrames = 64;
  co.voice.maxVoices = 8;
  eng::ProjectCompiler units(co);  // interpreted: the schedules
  REQUIRE(units.build(proj->graph).ok());
  struct Ctx {
    std::mutex m;
    std::vector<std::unique_ptr<eng::ProgramImage>> keep;
    uint32_t failed = 0;
  } ctx;
  eng::BackgroundCompilerConfig bc;
  bc.backend.fn = [](void* user, const g::Schedule& s, uint32_t maxBlock, uint32_t groups,
                     const eng::Resources* res) -> pq::Result<std::unique_ptr<eng::ProgramImage>> {
    jit::CompileOptions o;
    o.maxBlockFrames = maxBlock;
    o.maxVoiceGroups = groups;
    o.resources = res;
    return static_cast<jit::Compiler*>(user)->compile(s, o);
  };
  bc.backend.user = &c;
  bc.threads = threads;
  bc.user = &ctx;
  bc.deliver = [](void* user, const eng::BackgroundJob&, pq::Result<std::unique_ptr<eng::ProgramImage>> img) {
    Ctx& x = *static_cast<Ctx*>(user);
    std::lock_guard lock(x.m);
    if (img.ok()) x.keep.push_back(std::move(img.value()));
    else ++x.failed;
  };
  std::vector<eng::BackgroundJob> jobs;
  for (uint32_t u = 0; u < units.numUnits(); ++u) {
    eng::BackgroundJob j;
    j.schedule = units.unit(u).schedule;
    j.maxBlockFrames = 64;
    j.maxVoiceGroups = units.imageVoiceGroups(u);
    jobs.push_back(std::move(j));
  }
  {
    eng::BackgroundCompiler worker(bc);
    REQUIRE(worker.threads() == threads);
    worker.want(std::move(jobs));
    REQUIRE(worker.waitIdle(300.0));
  }
  CHECK(ctx.failed == 0);
  built = static_cast<uint32_t>(ctx.keep.size());
}

}  // namespace

TEST_CASE("PF.4: the same machine code with 1 and 6 compile threads (disk entries byte for byte, split units included)") {
  TempDir one("t1"), six("t6");
  auto a = compilerWith(one.path), b = compilerWith(six.path);
  uint32_t builtA = 0, builtB = 0;
  compileAll(*a, 1, builtA);
  compileAll(*b, 6, builtB);
  CHECK(builtA == builtB);
  const auto ea = one.entries(), eb = six.entries();
  REQUIRE(!ea.empty());
  CHECK(ea.size() == a->counters().compiles);  // one entry per LLVM compile: units of one code compile once
  REQUIRE(ea.size() == eb.size());
  for (size_t k = 0; k < ea.size(); ++k) {
    CAPTURE(ea[k].first);
    CHECK(ea[k].first == eb[k].first);  // the same keys …
    CHECK(ea[k].second == eb[k].second);  // … and the same bytes
  }
  CHECK(a->counters().compiles == b->counters().compiles);
}

TEST_CASE("PF.4: split and disk-cached split images play exactly what the interpreter plays; no stack probes") {
  const std::vector<float> reference = render(nullptr);
  float peak = 0.0f;
  for (float v : reference) peak = std::max(peak, std::fabs(v));
  REQUIRE(peak > 0.01f);  // something plays
  TempDir dir("split");
  {
    auto c = compilerWith(dir.path);
    Backend fresh;
    fresh.compiler = c.get();
    CHECK(sameBits(render(&fresh), reference));
    CHECK(fresh.split >= 1);  // some unit is big enough to be split into two objects
    CHECK(fresh.split < fresh.images);  // and some is not
    CHECK(fresh.probes == 0);
    MESSAGE("fixture: " << fresh.images << " unit images, " << fresh.split << " split");
  }
  auto c = compilerWith(dir.path);  // a new process, as far as the cache is concerned: every image from disk
  Backend disk;
  disk.compiler = c.get();
  CHECK(sameBits(render(&disk), reference));
  CHECK(c->counters().compiles == 0);
  CHECK(std::count(disk.sources.begin(), disk.sources.end(), jit::CompileStats::Source::Disk) >= 1);
  CHECK(disk.split >= 1);  // the split units' entries hold both objects
}
