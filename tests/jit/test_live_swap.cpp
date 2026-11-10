// T1b.5: the interpreter → JIT live swap. A project plays interpreted units; the background compiler (low-priority
// workers, pq/engine/background_compile.h; PF.4: several at once) builds their JIT images while it plays — here with a SIMULATED compile
// delay: the workers are held in their beforeBuild hook until the render has passed a chosen block — and each image is
// published into its unit with an Instant swap. Rendered offline block by block, the output must equal a project
// that stays interpreted, sample for sample (JIT == interpreter bit for bit since T1b.1-fix1, and the swap transfers
// every state word): the swap is click-free by T1.9's criteria trivially (zero difference to the no-swap render).
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "../engine/project_fixture.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/background_compile.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"

namespace eng = pq::engine;
namespace g = pq::graph;

namespace {

struct Rig {
  std::unique_ptr<project_fixture::Project> proj;
  std::unique_ptr<eng::ProjectCompiler> compiler;
  std::vector<std::unique_ptr<eng::NoteListSource>> notes;
  std::unique_ptr<eng::ProjectProgram> program;
  int64_t time = 0;
  uint64_t rtViolations = 0;

  Rig() {
    const project_fixture::Options o{4, 3, 3, 1};  // 4 FM instruments, a sidechain and a modulation merge, groups, a return
    proj = project_fixture::makeProject(o);
    REQUIRE(proj != nullptr);
    eng::ProjectCompileOptions co;
    co.sampleRate = 48000.0;
    co.maxBlockFrames = 64;
    co.voice.maxVoices = 8;
    compiler = std::make_unique<eng::ProjectCompiler>(co);  // the interpreter: images at once
    auto b = compiler->build(proj->graph);
    REQUIRE_MESSAGE(b.ok(), (b.ok() ? "" : b.error().message));
    eng::ProjectConfig pc;
    pc.sampleRate = 48000.0;
    pc.maxBlockFrames = 64;
    for (uint32_t k = 0; k < o.numTracks; ++k) {  // notes sounding across the swaps (held, released, retriggered)
      const float hz = 110.0f * (1.0f + 0.25f * static_cast<float>(k));
      notes.push_back(std::make_unique<eng::NoteListSource>(std::vector<eng::TimedNote>{
          {0, 30000, hz, 0.8f, 0.5f}, {12000, 40000, hz * 1.5f, 0.6f, 0.5f}, {50000, 30000, hz * 1.25f, 0.7f, 0.5f}}));
      pc.eventSources.push_back(notes.back()->source());
    }
    auto prog = eng::ProjectProgram::create(pc, b.value().layout);
    REQUIRE(prog.ok());
    program = std::move(prog.value());
    for (uint32_t u = 0; u < b.value().images.size(); ++u) REQUIRE(program->publish(u, std::move(b.value().images[u])).ok());
  }
  void render(uint32_t blocks, std::vector<float>& out) {
    std::vector<float> l(64), r(64);
    float* outs[2] = {l.data(), r.data()};
    for (uint32_t k = 0; k < blocks; ++k) {
      eng::AudioBlock ab;
      ab.out = outs;
      ab.numOut = 2;
      ab.frames = 64;
      ab.frameTime = time;
      rtViolations += pq::alloc_trap::countRtViolations([&] {
        pq::DenormalGuard dg;
        program->process(ab);
      });
      out.insert(out.end(), l.begin(), l.end());
      out.insert(out.end(), r.begin(), r.end());
      time += 64;
    }
  }
  // Every unit's schedule as a job (what ProjectLink::wantJit asks for).
  std::vector<eng::BackgroundJob> jobs() const {
    std::vector<eng::BackgroundJob> j;
    for (uint32_t u = 0; u < compiler->numUnits(); ++u) {
      eng::BackgroundJob b;
      b.schedule = compiler->unit(u).schedule;
      b.maxBlockFrames = 64;
      b.maxVoiceGroups = compiler->imageVoiceGroups(u);
      j.push_back(std::move(b));
    }
    return j;
  }
  void editLevel(uint32_t track, double level) {
    g::Patch& tp = *proj->trackPatches[track];
    for (g::NodeIndex n = 0; n < tp.nodeCount(); ++n)
      if (tp.node(n).kind == g::NodeKind::Const && tp.constant(n).literal == 0.15) {
        tp.constant(n) = g::Constant::real(level);
        return;
      }
    FAIL("no level constant");
  }
  bool allJit() const {
    for (uint32_t u = 0; u < program->numUnits(); ++u)
      if (program->current(u) == nullptr || program->current(u)->io().backend != eng::ImageBackend::Jit) return false;
    return true;
  }
};

// The compile delay, simulated: builds wait at the gate until the test opens it.
struct Gate {
  std::mutex m;
  std::condition_variable cv;
  bool open = false;
  int waiting = 0;
};

struct Live {
  Rig* rig = nullptr;
  Gate* gate = nullptr;
  eng::SwapPolicy policy{eng::SwapMode::Instant};
  std::atomic<int> published{0}, stale{0}, failed{0}, firsts{0};
  std::mutex deliverMutex;  // PF.4: deliveries come from several workers (ProjectLink: its compile mutex)
  static void before(void* self, const eng::BackgroundJob&) {
    Gate& gt = *static_cast<Live*>(self)->gate;
    std::unique_lock lock(gt.m);
    ++gt.waiting;
    gt.cv.notify_all();
    gt.cv.wait(lock, [&] { return gt.open; });
  }
  // What ProjectLink::deliverJit does: publish into the unit only if it still runs this schedule.
  static void deliver(void* self, const eng::BackgroundJob& job, pq::Result<std::unique_ptr<eng::ProgramImage>> img) {
    Live& l = *static_cast<Live*>(self);
    std::lock_guard lock(l.deliverMutex);
    if (!img.ok()) {
      ++l.failed;
      return;
    }
    const uint32_t u = l.rig->compiler->unitOf(job.schedule.get());
    if (u == eng::kNoUnit) {
      ++l.stale;
      return;
    }
    auto bound = l.rig->compiler->bind(u, std::move(img.value()));
    if (bound.ok() && l.rig->program->publish(u, std::move(bound.value()), l.policy).ok()) {
      ++l.published;
      if (job.first) ++l.firsts;
    } else {
      ++l.failed;
    }
  }
};

}  // namespace

TEST_CASE("T1b.5: interpreted units swap to their background JIT images after a simulated compile delay, sample-exact") {
  for (const eng::SwapMode mode : {eng::SwapMode::Instant, eng::SwapMode::Crossfade}) {
    CAPTURE(static_cast<int>(mode));
    Rig live, ref;  // ref stays interpreted
    Gate gate;
    Live ctx;
    ctx.rig = &live;
    ctx.gate = &gate;
    ctx.policy = eng::SwapPolicy{mode, 10.0f};
    eng::BackgroundCompilerConfig bc;
    bc.backend = pq::compile::jitUnitBackend();
    bc.deliver = &Live::deliver;
    bc.beforeBuild = &Live::before;
    bc.user = &ctx;
    eng::BackgroundCompiler jit(bc);
    const uint32_t units = live.compiler->numUnits();
    REQUIRE(units >= 3);
    jit.want(live.jobs());
    const int busy = static_cast<int>(std::min(jit.threads(), units));
    {  // every worker holds a job at the gate (PF.4: in parallel): the compiles are "slow"
      std::unique_lock lock(gate.m);
      REQUIRE(gate.cv.wait_for(lock, std::chrono::seconds(30), [&] { return gate.waiting == busy; }));
    }
    std::vector<float> a, b;
    live.render(400, a);  // 0.53 s of interpreted playback while the JIT "compiles"
    ref.render(400, b);
    CHECK(ctx.published == 0);
    // An edit while unit 0's JIT compile is in flight: the interpreted image of the edit plays at once; the in-flight
    // JIT image of the OLD schedule becomes stale and must be dropped, the queued jobs are replaced by the new wants.
    const uint32_t editTrack = live.compiler->partition().units[0].tracks[0];
    REQUIRE(editTrack < 4);  // an instrument track (FmSynth level 0.15)
    live.editLevel(editTrack, 0.4);
    ref.editLevel(editTrack, 0.4);
    const uint32_t tracks[] = {editTrack};
    auto ul = live.compiler->update(live.proj->graph, tracks);
    auto ur = ref.compiler->update(ref.proj->graph, tracks);
    REQUIRE(ul.ok());
    REQUIRE(ur.ok());
    REQUIRE(ul.value().size() == 1);
    REQUIRE(ul.value()[0].unit == 0);
    REQUIRE(live.program->publish(0, std::move(ul.value()[0].image)).ok());
    REQUIRE(ref.program->publish(0, std::move(ur.value()[0].image)).ok());
    jit.want(live.jobs());
    live.render(200, a);
    ref.render(200, b);
    {
      std::lock_guard lock(gate.m);
      gate.open = true;  // the compile "finishes"
    }
    gate.cv.notify_all();
    REQUIRE(jit.waitIdle(120.0));
    CHECK(ctx.stale == 1);  // the old schedule of unit 0
    CHECK(ctx.published == static_cast<int>(units));
    CHECK(ctx.failed == 0);
    live.render(800, a);  // the swaps happen at the first of these blocks
    ref.render(800, b);
    CHECK(live.allJit());
    for (uint32_t u = 0; u < units; ++u) CHECK(live.program->unitStats(u).swaps >= 2);
    REQUIRE(a.size() == b.size());
    size_t firstDiff = a.size();
    for (size_t i = 0; i < a.size() && firstDiff == a.size(); ++i)
      if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) firstDiff = i;
    CHECK_MESSAGE(firstDiff == a.size(), "first difference at sample " << firstDiff / 2);
    float peak = 0.0f;
    for (float v : a) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.01f);  // something was playing through the swaps
    CHECK(live.rtViolations == 0);
    CHECK(jit.stats().built == units + 1);
    live.program->collect();
  }
}

// T1b.x-perf (QUESTIONS Q111 option C): with UnitBackend::quick, each instrument unit gets a voice-group image first
// and its pair image after every unit's first image; both swaps are Instant and the output stays the interpreted
// project's, sample for sample.
TEST_CASE("T1b.x-perf: quick first JIT images (voice groups), then the pair images, all Instant and sample-exact") {
  Rig live, ref;
  Live ctx;
  ctx.rig = &live;
  eng::BackgroundCompilerConfig bc;
  bc.backend = pq::compile::jitUnitBackend(true);
  bc.deliver = &Live::deliver;
  bc.user = &ctx;
  eng::BackgroundCompiler jit(bc);
  const uint32_t units = live.compiler->numUnits();
  jit.want(live.jobs());
  std::vector<float> a, b;
  for (int k = 0; k < 40 && !jit.waitIdle(0.0); ++k) {  // playing while the images arrive
    live.render(25, a);
    ref.render(25, b);
  }
  REQUIRE(jit.waitIdle(120.0));
  live.render(400, a);
  ref.render(400, b);
  CHECK(ctx.failed == 0);
  CHECK(ctx.stale == 0);
  CHECK(ctx.firsts >= 1);  // the FM instruments (two voice groups each) have pair functions to add
  CHECK(ctx.published == static_cast<int>(units) + ctx.firsts);  // one final image per unit, plus the first ones
  CHECK(jit.stats().refinements == static_cast<uint64_t>(ctx.firsts.load()));
  CHECK(live.allJit());
  REQUIRE(a.size() == b.size());
  size_t firstDiff = a.size();
  for (size_t i = 0; i < a.size() && firstDiff == a.size(); ++i)
    if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) firstDiff = i;
  CHECK_MESSAGE(firstDiff == a.size(), "first difference at sample " << firstDiff / 2);
  CHECK(live.rtViolations == 0);
  live.program->collect();
}

// PF.4: the background compiler's pool. Workers build at once (up to `threads`), a schedule never runs on two workers
// at once, a schedule new to the queue (the unit the user just edited) starts before those already queued, and every
// first image starts before any refinement. The backend is the interpreter with a "quick" path that always asks for a
// refinement; the workers are held at a gate until every one of them has a job.
TEST_CASE("PF.4: background compiler pool: parallel builds, one worker per schedule, the edited unit first, refinements last") {
  Rig rig, other;  // `other`'s schedules stand in for an edit's new ones
  struct Pool {
    Gate gate;
    std::mutex m;
    std::vector<std::pair<const g::Schedule*, bool>> starts;  // (schedule, refine) in start order
    std::vector<const g::Schedule*> active;
    int concurrent = 0, maxConcurrent = 0, twice = 0, delivered = 0;
  } pool;
  pq::engine::UnitBackend backend;
  backend.quick = [](void*, const g::Schedule& s, uint32_t maxBlock, uint32_t groups, const eng::Resources* res,
                     bool* refine) -> pq::Result<std::unique_ptr<eng::ProgramImage>> {
    eng::InterpretOptions io;
    io.maxBlockFrames = maxBlock;
    io.maxVoiceGroups = groups;
    io.resources = res;
    *refine = true;
    return eng::buildInterpreted(s, io);
  };
  eng::BackgroundCompilerConfig bc;
  bc.backend = backend;
  bc.threads = 2;
  bc.user = &pool;
  bc.beforeBuild = [](void* user, const eng::BackgroundJob& job) {
    Pool& p = *static_cast<Pool*>(user);
    {
      std::lock_guard lock(p.m);
      p.starts.emplace_back(job.schedule.get(), job.refine);
      p.twice += std::count(p.active.begin(), p.active.end(), job.schedule.get()) > 0 ? 1 : 0;
      p.active.push_back(job.schedule.get());
      p.maxConcurrent = std::max(p.maxConcurrent, ++p.concurrent);
    }
    std::unique_lock lock(p.gate.m);
    ++p.gate.waiting;
    p.gate.cv.notify_all();
    p.gate.cv.wait(lock, [&] { return p.gate.open; });
  };
  bc.deliver = [](void* user, const eng::BackgroundJob& job, pq::Result<std::unique_ptr<eng::ProgramImage>> img) {
    Pool& p = *static_cast<Pool*>(user);
    REQUIRE(img.ok());
    std::lock_guard lock(p.m);
    p.active.erase(std::find(p.active.begin(), p.active.end(), job.schedule.get()));
    --p.concurrent;
    ++p.delivered;
  };
  eng::BackgroundCompiler jit(bc);
  REQUIRE(jit.threads() == 2);
  std::vector<eng::BackgroundJob> jobs = rig.jobs();
  const size_t n = jobs.size();
  REQUIRE(n >= 3);
  jit.want(jobs);
  {
    std::unique_lock lock(pool.gate.m);
    REQUIRE(pool.gate.cv.wait_for(lock, std::chrono::seconds(30), [&] { return pool.gate.waiting == 2; }));
  }
  // An "edit": the same jobs plus a new schedule, listed last; the two running jobs listed again (not re-queued).
  std::vector<eng::BackgroundJob> edited = rig.jobs();
  eng::BackgroundJob fresh = other.jobs().front();
  edited.push_back(fresh);
  jit.want(edited);
  {
    std::lock_guard lock(pool.gate.m);
    pool.gate.open = true;
  }
  pool.gate.cv.notify_all();
  REQUIRE(jit.waitIdle(60.0));
  std::lock_guard lock(pool.m);
  CHECK(pool.maxConcurrent == 2);
  CHECK(pool.twice == 0);
  REQUIRE(pool.starts.size() == 2 * (n + 1));  // a first image and a refinement per schedule
  CHECK(pool.starts[2].first == fresh.schedule.get());  // the edit's schedule before the queued ones
  CHECK_FALSE(pool.starts[2].second);
  for (size_t k = 0; k < n + 1; ++k) {
    CAPTURE(k);
    CHECK_FALSE(pool.starts[k].second);  // every first image …
    CHECK(pool.starts[n + 1 + k].second);  // … before any refinement
  }
  CHECK(pool.delivered == static_cast<int>(2 * (n + 1)));
  CHECK(jit.stats().refinements == n + 1);
}
