// MM review (link / lifetimes): the table builder through the playback link.
//   A  loose end (e): TMM.5 "a keyframe edit is audible without recompile: <= 30 ms (64 frames), <= 160 ms with bake".
//      Times, per edit, the first publish into the slot (what the perf test measures) AND the full table (stats.built).
//   B  project open with N baked Wavetables: ProjectLink::create (compileNow waits <= 2 s for the builder): how long it
//      blocks and how many instances start on the 1-frame default sine (the TMM.8-fix guarantee).
//   C  a structural change while another instance is being dragged (baked knob at 60 Hz): how long the compile path
//      (compileThread = true) is held in waitForMetaTables(2.0) and so how late the change is heard.
// Usage: probe_link [A|B|C]   (default: all)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/document/store.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/meta/table_builder.h"
#include "pq/meta/wavetable.h"

using namespace pq;
using namespace pq::doc;
using Clock = std::chrono::steady_clock;

namespace {

double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

[[noreturn]] void die(const char* what, const std::string& m = {}) {
  std::printf("FATAL %s %s\n", what, m.c_str());
  std::exit(2);
}

std::vector<float> cycleOf(dsp::BasicCycle c, uint64_t seed = 1) {
  const AlignedBuffer<float> b = dsp::basicCycle(c, dsp::kKeyframeCycle, seed);
  return {b.span().begin(), b.span().end()};
}

meta::DataPtr twoKeys(dsp::BasicCycle a, dsp::BasicCycle b, uint64_t seed, dsp::GapMode gap = dsp::GapMode::Spectral) {
  meta::WavetableData d;
  d.keyframes.push_back({0.0f, cycleOf(a, seed)});
  d.keyframes.push_back({1.0f, cycleOf(b, seed)});
  if (seed != 1) d.keyframes.back().cycle[7] += 1e-3f * static_cast<float>(seed);  // distinct data per instance
  d.gaps = {gap};
  return meta::wavetableData(d);
}

struct Song {
  DocumentStore store;
  std::vector<DeviceId> wts;
  Song() { must(SetTempoMap{{{0, 120.0, TempoCurve::Step}}}); }
  void must(Command c) {
    const Status st = store.dispatch(std::move(c));
    if (!st.ok()) die("dispatch", st.error().message);
  }
  void edit(Result<ops::Edit> e) {
    if (!e.ok()) die("edit", e.error().message);
    must(e->command);
  }
  const Project& doc() const { return *store.current(); }
  DeviceId track() {
    auto t = ops::createTrack(doc(), store.ids(), TrackKind::Midi);
    if (!t.ok()) die("createTrack");
    must(t->command);
    const TrackId id = t->tracks.at(0);
    auto w = ops::addMetaDevice(doc(), store.ids(), id, "wavetable", 0);
    if (!w.ok()) die("addMetaDevice", w.error().message);
    must(w->command);
    Note n;
    n.id = store.ids().note();
    n.start = 0;
    n.length = 64 * kPpq;
    n.pitch = tuning::PitchId::fromDegree(0, 0);
    n.velocity = 100.0f;
    NoteClip c;
    c.id = store.ids().clip();
    c.name = "n";
    c.start = 0;
    c.length = 64 * kPpq;
    c.notes = NoteSeq::fromNotes({n}).value();
    must(AddClip{id, std::make_shared<const NoteClip>(std::move(c))});
    wts.push_back(w->devices.at(0));
    return w->devices.at(0);
  }
  void data(DeviceId d, const meta::DataPtr& v) { edit(ops::setMetaData(doc(), d, "", "wavetable", v)); }
  void param(DeviceId d, const char* name, double v) { edit(ops::setMetaParam(doc(), d, "", "wavetable", name, v)); }
};

std::unique_ptr<app::ProjectLink> makeLink(const Snapshot& s, bool thread) {
  app::LinkConfig c;
  c.compileThread = thread;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  c.sampleRate = 48000.0;
  auto l = app::ProjectLink::create(c, s);
  if (!l.ok()) die("create", l.error().message);
  return std::move(l.value());
}

// ---- A ----------------------------------------------------------------------------------------------------------
void probeA() {
  std::printf("== A: edit -> audible table (check preset: asserts on; indicative under load)\n");
  Song s;
  const DeviceId wt = s.track();
  s.data(wt, twoKeys(dsp::BasicCycle::Saw, dsp::BasicCycle::Square, 1));
  auto link = makeLink(s.store.current(), false);
  [[maybe_unused]] auto sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  if (!link->waitForMetaTables()) die("waitForMetaTables");
  const auto slot = link->metaFrameTable(wt, "", 0);
  if (!slot) die("no slot");
  // Returns {first publish ms, full table ms (stats.built incremented and published)}.
  auto time = [&](auto&& edit) {
    const uint64_t gen = slot->generation();
    const uint64_t built0 = link->metaTableStats().built, shared0 = link->metaTableStats().shared;
    const auto t0 = Clock::now();
    edit();
    double first = -1, full = -1;
    for (;;) {
      const auto now = Clock::now();
      if (first < 0 && slot->generation() != gen) first = ms(t0, now);
      const auto st = link->metaTableStats();
      if (full < 0 && (st.built != built0 || st.shared != shared0) && link->waitForMetaTables(0.0)) full = ms(t0, Clock::now());
      if (first >= 0 && full >= 0) break;
      if (ms(t0, now) > 5000) break;
      std::this_thread::yield();
    }
    return std::pair{first, full};
  };
  auto report = [](const char* what, std::vector<std::pair<double, double>> v) {
    double bf = 1e9, bF = 1e9, wf = 0, wF = 0;
    for (auto [f, F] : v) bf = std::min(bf, f), bF = std::min(bF, F), wf = std::max(wf, f), wF = std::max(wF, F);
    std::printf("  %-44s first publish best %7.1f worst %7.1f ms | full table best %7.1f worst %7.1f ms\n", what, bf, wf, bF, wF);
  };
  std::vector<std::pair<double, double>> plain, foldEdit, keyBaked;
  for (int i = 0; i < 5; ++i)
    plain.push_back(time([&] { s.data(wt, twoKeys(dsp::BasicCycle::Saw, dsp::BasicCycle::Square, 100 + i, i % 2 ? dsp::GapMode::Crossfade : dsp::GapMode::Spectral)); }));
  report("keyframe edit, 64 frames, no bake", plain);
  for (int i = 0; i < 5; ++i) foldEdit.push_back(time([&] { s.param(wt, "fold", 0.2 + 0.1 * i); }));
  report("fold knob step (the perf test's 'bake')", foldEdit);
  s.param(wt, "fold", 0.5);
  (void)link->waitForMetaTables();
  for (int i = 0; i < 5; ++i)
    keyBaked.push_back(time([&] { s.data(wt, twoKeys(dsp::BasicCycle::Saw, dsp::BasicCycle::Square, 200 + i)); }));
  report("keyframe edit with fold 0.5 baked", keyBaked);
  const auto st = link->metaTableStats();
  std::printf("  builder: built %llu previews %llu (focused %llu) maxBuild %.1f ms maxPreview %.1f ms\n",
              (unsigned long long)st.built, (unsigned long long)st.previews, (unsigned long long)st.focused, st.maxBuildMs,
              st.maxPreviewMs);
}

// ---- B ----------------------------------------------------------------------------------------------------------
void probeB(int n) {
  std::printf("== B: ProjectLink::create with %d baked Wavetables (distinct data, fold 0.5)\n", n);
  Song s;
  for (int i = 0; i < n; ++i) {
    const DeviceId d = s.track();
    s.data(d, twoKeys(dsp::BasicCycle::Saw, dsp::BasicCycle::Square, 300 + i));
    s.param(d, "fold", 0.5);
  }
  const auto t0 = Clock::now();
  auto link = makeLink(s.store.current(), false);
  const double createMs = ms(t0, Clock::now());
  int defaults = 0, previews = 0;
  for (const DeviceId d : s.wts) {
    const auto slot = link->metaFrameTable(d, "", 0);
    const uint32_t f = slot ? slot->table()->frames() : 0;
    if (f <= 1) ++defaults;  // still the 1-frame sine the slot starts with
    else if (f < meta::kWtDefaultFrames) ++previews;  // a TMM.9 preview (8 dense frames), not the full table
  }
  const auto st = link->metaTableStats();
  std::printf("  create blocked %.0f ms; when the first image is live: %d / %d on the default sine, %d / %d on an 8-frame "
              "preview; maxBuild %.1f ms\n",
              createMs, defaults, n, previews, n, st.maxBuildMs);
  (void)link->waitForMetaTables();
}

// ---- C ----------------------------------------------------------------------------------------------------------
void probeC() {
  std::printf("== C: structural change while another instance's baked knob is dragged at 60 Hz (compile thread)\n");
  Song s;
  const DeviceId a = s.track();
  s.data(a, twoKeys(dsp::BasicCycle::Saw, dsp::BasicCycle::Square, 500));
  s.param(a, "fold", 0.3);
  auto link = makeLink(s.store.current(), true);
  [[maybe_unused]] auto sub = s.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  (void)link->waitForMetaTables();
  // The audio thread (as the app's output): 64-frame blocks at real time, so published images are picked up (without
  // it the compile path's drainDeferred waits 5000 x sleep_for(1 ms), ~80 s on Windows' default timer).
  std::atomic<bool> audioQuit{false};
  std::thread audio([&] {
    std::vector<float> l(64), r(64);
    int64_t time = 0;
    const auto start = Clock::now();
    while (!audioQuit.load()) {
      const auto due = static_cast<int64_t>(ms(start, Clock::now()) * 48.0);
      while (time < due) {
        float* ch[2] = {l.data(), r.data()};
        link->engine().process(engine::AudioBlock{nullptr, 0, ch, 2, 64, time});
        time += 64;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  struct Join {
    std::atomic<bool>& q;
    std::thread& t;
    ~Join() {
      q = true;
      t.join();
    }
  } join{audioQuit, audio};
  link->flush();
  {  // baseline: the same structural change with the builder idle
    const uint64_t c0 = link->stats().compiles;
    const auto a0 = Clock::now();
    const DeviceId z = s.track();
    while (!(link->metaFrameTable(z, "", 0) && link->stats().compiles > c0) && ms(a0, Clock::now()) < 5000)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::printf("  baseline (builder idle): structural change -> compile done ~%.0f ms\n", ms(a0, Clock::now()));
    link->flush();
  }
  const uint64_t compiles0 = link->stats().compiles;
  const auto st0 = link->metaTableStats();
  // Drag for 3 s; at 0.5 s add a track with a Wavetable (a new frame-table slot: metaFresh_ -> waitForMetaTables(2.0)).
  const auto t0 = Clock::now();
  Clock::time_point added{};
  double heardMs = -1;
  DeviceId b{};
  for (int step = 0; ms(t0, Clock::now()) < 3000; ++step) {
    s.param(a, "fold", 0.3 + 0.2 * ((step % 40) / 40.0));
    if (step == 30) {
      b = s.track();
      added = Clock::now();
    }
    if (b.valid() && heardMs < 0) {
      // The compile is done once the new instance's slot exists AND the compile counter moved (read racy: indicative).
      if (link->metaFrameTable(b, "", 0) && link->stats().compiles > compiles0) heardMs = ms(added, Clock::now());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  const auto f0 = Clock::now();
  const uint64_t cBefore = link->stats().compiles;
  std::atomic<bool> flushed{false};
  std::thread mon([&] {  // what the builder does while flush() waits
    while (!flushed.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5000));
      if (flushed.load()) break;
      const auto m = link->metaTableStats();
      std::printf("    [%.0f ms in flush] builder submitted %llu built %llu previews %llu shared %llu replaced %llu errors %llu lastBuild %.0f ms\n",
                  ms(f0, Clock::now()), (unsigned long long)m.submitted, (unsigned long long)m.built,
                  (unsigned long long)m.previews, (unsigned long long)m.shared, (unsigned long long)m.replaced,
                  (unsigned long long)m.errors, m.lastBuildMs);
    }
  });
  link->flush();
  flushed = true;
  mon.join();
  std::printf("  after the drag: flush() took %.0f ms (compiles before %llu, after %llu, at start %llu)\n",
              ms(f0, Clock::now()), (unsigned long long)cBefore, (unsigned long long)link->stats().compiles,
              (unsigned long long)compiles0);
  if (heardMs < 0) heardMs = ms(added, Clock::now());
  const auto st = link->metaTableStats();
  std::printf("  structural change -> compile done: ~%.0f ms (debounce %.0f ms)\n", heardMs, app::LinkConfig{}.debounceMs);
  std::printf("  during the drag: submitted %llu, previews %llu, full builds %llu (a full build per step when someone waits)\n",
              (unsigned long long)(st.submitted - st0.submitted), (unsigned long long)(st.previews - st0.previews),
              (unsigned long long)(st.built - st0.built));
  (void)link->waitForMetaTables();
}

// ---- D ----------------------------------------------------------------------------------------------------------
// File > Open of another project whose Wavetable has the same device id (ids restart per project): does the new
// project's image read the OLD project's slot and table?
void probeD() {
  std::printf("== D: Reset (File > Open) to a project whose Wavetable reuses the device id\n");
  Song s1;
  const DeviceId d1 = s1.track();
  s1.data(d1, twoKeys(dsp::BasicCycle::Saw, dsp::BasicCycle::Square, 700));
  Song s2;  // built the same way: the same ids
  const DeviceId d2 = s2.track();
  s2.data(d2, twoKeys(dsp::BasicCycle::Sine, dsp::BasicCycle::Triangle, 701));
  s2.param(d2, "fold", 0.6);  // baked: a preview first, the full table >= 100 ms later
  std::printf("  device ids: old %u new %u\n", d1.value, d2.value);
  auto link = makeLink(s1.store.current(), false);
  [[maybe_unused]] auto sub = s1.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  (void)link->waitForMetaTables();
  const auto oldSlot = link->metaFrameTable(d1, "", 0);
  const auto oldTable = oldSlot->table();
  const auto st0 = link->metaTableStats();
  const auto t0 = Clock::now();
  s1.store.reset(s2.store.current());
  link->flush();  // the Reset's compile (on this thread): the new project's images are published now
  const auto newSlot = link->metaFrameTable(d2, "", 0);
  const bool sameSlot = newSlot.get() == oldSlot.get();
  const bool oldTablePlays = newSlot && newSlot->table().get() == oldTable.get();
  std::printf("  after the compile: same slot object %s; the new project's instance reads the OLD project's table: %s\n",
              sameSlot ? "YES" : "no", oldTablePlays ? "YES" : "no");
  double firstMs = -1;
  while (ms(t0, Clock::now()) < 3000) {
    if (newSlot->table().get() != oldTable.get()) {
      firstMs = ms(t0, Clock::now());
      break;
    }
    std::this_thread::yield();
  }
  const auto st1 = link->metaTableStats();
  std::printf("  first own table after %.1f ms (preview %llu, focused over the old project's table %llu)\n", firstMs,
              (unsigned long long)(st1.previews - st0.previews), (unsigned long long)(st1.focused - st0.focused));
  (void)link->waitForMetaTables();
}

// dataHash runs under the link's mutex_ (refreshMetaTables) for every new data value: its cost for a full table.
void probeHash() {
  meta::WavetableData d;
  for (int k = 0; k < 256; ++k) d.keyframes.push_back({k / 255.0f, cycleOf(dsp::BasicCycle::Saw, 900 + k)});
  d.gaps.assign(255, dsp::GapMode::Spectral);
  d.frames = 256;
  const meta::DataPtr v = meta::wavetableData(d);
  double best = 1e9;
  uint64_t h = 0;
  for (int i = 0; i < 5; ++i) {
    const auto a = Clock::now();
    h ^= meta::dataHash(*v);
    best = std::min(best, ms(a, Clock::now()));
  }
  std::printf("== hash: dataHash of 256 keyframes x 2048 floats: %.2f ms (best of 5; %llx)\n", best, (unsigned long long)h);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string which = argc > 1 ? argv[1] : "ABCDH";
  if (which.find('D') != std::string::npos) probeD();
  if (which.find('H') != std::string::npos) probeHash();
  if (which.find('A') != std::string::npos) probeA();
  if (which.find('B') != std::string::npos) {
    probeB(4);
    probeB(12);
  }
  if (which.find('C') != std::string::npos) probeC();
  return 0;
}
