// T1b.5: the interpreter → JIT live swap. A project plays interpreted units; the background compiler (one low-priority
// worker, pq/engine/background_compile.h) builds their JIT images while it plays — here with a SIMULATED compile
// delay: the worker is held in its beforeBuild hook until the render has passed a chosen block — and each image is
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
  std::atomic<int> published{0}, stale{0}, failed{0};
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
    if (bound.ok() && l.rig->program->publish(u, std::move(bound.value()), l.policy).ok()) ++l.published;
    else ++l.failed;
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
    {  // the worker holds the first job at the gate: the compile is "slow"
      std::unique_lock lock(gate.m);
      REQUIRE(gate.cv.wait_for(lock, std::chrono::seconds(30), [&] { return gate.waiting == 1; }));
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
