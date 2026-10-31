// T1b.4 in the DAW (TASKS acceptance): opening a stock device at runtime does no JIT compile — a new
// default-instrument track in a new project gets every unit's JIT image from the AOT seed (pq/app/jit_cache.h,
// compiled for the baseline CPU by the same code that `pqdaw --jit-aot` runs) — and a second load of the same user
// project hits the disk cache. Played through a NullDevice against a link that never uses the JIT: the output is
// identical sample for sample. The compile counts come from the compilers' CacheCounters (the test hook).
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/jit_cache.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/engine/null_device.h"
#include "pq/jit/compiler.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  explicit TempDir(const char* tag) {
    static std::atomic<uint32_t> n{0};
    path = fs::temp_directory_path() / ("pq_jit_aot_" + std::string(tag) + "_" +
                                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                                        std::to_string(n++));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  size_t entries() const {
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(path)) n += e.path().extension() == ".pqjit";
    return n;
  }
};

std::unique_ptr<jit::Compiler> compilerWith(const jit::CacheConfig& cache) {
  jit::CompilerConfig cfg;
  cfg.cache = cache;
  auto r = jit::Compiler::create(cfg);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return std::move(r.value());
}

std::unique_ptr<ProjectLink> link(const Snapshot& s, jit::Compiler* jit) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit != nullptr;
  if (jit != nullptr) c.jitBackend = compile::jitUnitBackend(jit);
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

struct Player {
  engine::NullDevice dev;
  std::vector<float> out;
  explicit Player(ProjectLink& l) {
    engine::StreamConfig sc;
    sc.sampleRate = 48000;
    sc.bufferFrames = 256;
    sc.outputChannels = 2;
    REQUIRE(dev.open(sc, l.callback()).ok());
  }
  ~Player() { dev.close(); }
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      dev.processBlock();
      for (uint32_t c = 0; c < 2; ++c) out.insert(out.end(), dev.output(c), dev.output(c) + 256);
    }
  }
};

uint32_t unitsOn(ProjectLink& l, engine::ImageBackend b) {
  engine::ProjectProgram* p = l.engine().program();
  uint32_t n = 0;
  for (uint32_t u = 0; u < p->numUnits(); ++u) n += p->current(u) != nullptr && p->current(u)->io().backend == b;
  return n;
}

bool identical(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

float peak(const std::vector<float>& v) {
  float p = 0.0f;
  for (float x : v) p = std::max(p, std::fabs(x));
  return p;
}

// The user's first steps in a new project: a MIDI track with the default instrument, a clip, play.
void addStockTrack(DocumentStore& store) {
  IdSource& ids = store.ids();
  const TrackId t = ids.track();
  REQUIRE(store.dispatch(AddTrack{playback_fixture::track(t, TrackKind::Midi, "MIDI", store.ids().device()), {}, kAppend}).ok());
  std::vector<Note> notes;
  for (int i = 0; i < 4; ++i) notes.push_back(playback_fixture::note(ids, i * kPpq, kPpq / 2, 4 * i, 0, 100.0f));
  REQUIRE(store.dispatch(AddClip{t, playback_fixture::clip(ids, 0, kTicksPerWhole, notes, "clip")}).ok());
}

}  // namespace

TEST_CASE("T1b.4: opening a stock device (a default-instrument track in a new project) runs no JIT compile: the AOT seed has it") {
  TempDir aot("seed"), user("user");
  app::AotSeedOptions so;
  so.sampleRates = {48000.0};
  so.verify = true;
  auto seeded = app::seedJitAot(aot.path, so);
  REQUIRE_MESSAGE(seeded.ok(), (seeded.ok() ? std::string() : seeded.error().message));
  CHECK(seeded->compiles > 0);
  CHECK(seeded->entries == aot.entries());
  MESSAGE("AOT seed (48 kHz): " << seeded->projects << " stock projects, " << seeded->images << " images, "
                                << seeded->compiles << " compiles, " << seeded->entries << " entries, " << seeded->bytes / 1024
                                << " KiB, " << seeded->seconds << " s");

  jit::CacheConfig cc;
  cc.dir = user.path;
  cc.readOnly = {aot.path};
  cc.verify = true;  // every AOT hit must regenerate the IR it was compiled from
  auto host = compilerWith(cc);
  DocumentStore sj, si;  // a new project each (master only)
  auto lj = link(sj.current(), host.get()), li = link(si.current(), nullptr);
  [[maybe_unused]] const ListenerId subj = sj.subscribe([&](const StoreEvent& e) { lj->onDocumentChange(e); });
  [[maybe_unused]] const ListenerId subi = si.subscribe([&](const StoreEvent& e) { li->onDocumentChange(e); });
  Player pj(*lj), pi(*li);
  pj.run(20);
  pi.run(20);
  addStockTrack(sj);
  addStockTrack(si);
  lj->flush();
  li->flush();
  lj->play(0);
  li->play(0);
  pj.run(40);
  pi.run(40);
  REQUIRE(lj->waitForJit(60.0));
  pj.run(400);
  pi.run(400);
  const uint32_t units = lj->engine().program()->numUnits();
  CHECK(units >= 2);  // the MIDI track and the master
  CHECK(unitsOn(*lj, engine::ImageBackend::Jit) == units);
  const jit::CacheCounters c = host->counters();
  CHECK(c.compiles == 0);  // TASKS: "opening a stock device at runtime does no JIT compile"
  CHECK(c.aotHits + c.memoryHits == lj->jitStats().built);
  CHECK(c.aotHits >= units);
  CHECK(c.verifyMismatches == 0);
  CHECK(c.verified == c.aotHits + c.memoryHits);
  CHECK(user.entries() == 0);  // nothing compiled, so nothing stored
  CHECK(identical(pj.out, pi.out));  // baseline-CPU images from the seed == the interpreter, sample for sample
  CHECK(peak(pj.out) > 0.01f);
}

TEST_CASE("T1b.4: a second load of the same user project hits the disk cache: no JIT compile, the same output") {
  TempDir dir("project");
  jit::CacheConfig cc;
  cc.dir = dir.path;
  cc.verify = true;
  std::vector<float> first, second;
  uint32_t units = 0;
  for (int load = 0; load < 2; ++load) {
    CAPTURE(load);
    auto jit = compilerWith(cc);  // a new compiler per load: as a new pqdaw process would
    playback_fixture::Fixture f;  // tunings, a return, a group, three MIDI tracks: the user's own project
    auto l = link(f.snap(), jit.get());
    Player p(*l);
    l->play(0);
    p.run(10);
    REQUIRE(l->waitForJit(120.0));
    p.run(600);
    units = l->engine().program()->numUnits();
    CHECK(unitsOn(*l, engine::ImageBackend::Jit) == units);
    const jit::CacheCounters c = jit->counters();
    if (load == 0) {
      CHECK(c.compiles > 0);
      CHECK(c.stores == c.compiles);
      first = std::move(p.out);
    } else {
      CHECK(c.compiles == 0);  // TASKS: "a second load of the same user patch hits the cache"
      CHECK(c.diskHits + c.memoryHits == l->jitStats().built);
      CHECK(c.diskHits == dir.entries());
      CHECK(c.verifyMismatches == 0);
      second = std::move(p.out);
    }
  }
  CHECK(units >= 5);
  CHECK(identical(first, second));
  CHECK(peak(first) > 0.01f);
}

TEST_CASE("T1b.4: pqdaw's cache configuration: the user directory (PQ_JIT_CACHE_DIR, off) and the AOT seed next to the executable") {
  const jit::CacheConfig c = app::appJitCache();
  REQUIRE(c.readOnly.size() == 1);
  CHECK(c.readOnly[0] == app::aotSeedDir());
  CHECK(c.readOnly[0].filename() == "jit-aot");
  CHECK(!c.verify);
  if (std::getenv("PQ_JIT_CACHE_DIR") == nullptr) CHECK(c.dir == jit::defaultCacheDir());
}

TEST_CASE("T4.12 / H5: a new Operator track, and Auto Filter added from the device menu, run no JIT compile: the AOT seed has them") {
  TempDir aot("seed-op"), user("user-op");
  app::AotSeedOptions so;
  so.sampleRates = {48000.0};
  auto seeded = app::seedJitAot(aot.path, so);
  REQUIRE_MESSAGE(seeded.ok(), (seeded.ok() ? std::string() : seeded.error().message));
  // 0: a MIDI track with Operator as its instrument; 1: the default instrument and Auto Filter at its defaults after
  // it (M4 review H5: what the device menu's AddDevice makes). Each in a new project.
  for (int which = 0; which < 2; ++which) {
    CAPTURE(which);
    jit::CacheConfig cc;
    cc.dir = user.path;
    cc.readOnly = {aot.path};
    auto host = compilerWith(cc);
    DocumentStore s;
    auto l = link(s.current(), host.get());
    [[maybe_unused]] const ListenerId sub = s.subscribe([&](const StoreEvent& e) { l->onDocumentChange(e); });
    Player p(*l);
    p.run(20);
    IdSource& ids = s.ids();
    const TrackId t = ids.track();
    if (which == 0) {
      REQUIRE(s.dispatch(AddTrack{playback_fixture::track(t, TrackKind::Midi, "MIDI"), {}, kAppend}).ok());
      REQUIRE(s.dispatch(AddDevice{t, makeDevice(ids.device(), "operator", DeviceKind::Instrument), 0}).ok());
    } else {
      REQUIRE(s.dispatch(AddTrack{playback_fixture::track(t, TrackKind::Midi, "MIDI", ids.device()), {}, kAppend}).ok());
      REQUIRE(s.dispatch(AddDevice{t, makeDevice(ids.device(), "auto_filter", DeviceKind::AudioEffect), 1}).ok());
    }
    std::vector<Note> notes;
    for (int i = 0; i < 4; ++i) notes.push_back(playback_fixture::note(ids, i * kPpq, kPpq / 2, 4 * i, 0, 100.0f));
    REQUIRE(s.dispatch(AddClip{t, playback_fixture::clip(ids, 0, kTicksPerWhole, notes, "clip")}).ok());
    l->flush();
    l->play(0);
    p.run(40);
    REQUIRE(l->waitForJit(60.0));
    p.run(400);
    const uint32_t units = l->engine().program()->numUnits();
    CHECK(unitsOn(*l, engine::ImageBackend::Jit) == units);
    const jit::CacheCounters c = host->counters();
    CHECK(c.compiles == 0);
    CHECK(c.aotHits >= units);
    CHECK(peak(p.out) > 0.01f);
  }
}
