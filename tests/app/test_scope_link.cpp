// TX.scope: the Oscilloscope module through the playback link — its tap exists once its unit compiled; unwatched the
// audio thread copies nothing; watched, every block lands in the tap (RtScope: the engine block allocates nothing either
// way) with the transport's beat (linear in time while playing), no note after an instrument (global), the lowest
// sounding voice's pitch in a meta voice run (per voice), and the signal's L and R; the module is bit-exact
// pass-through in both places.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;

// IG.1-2: the project as an older file of it loads — its meta voice runs (and instruments) in instrument groups, so they
// play per note as these cases were written (pq/document/instrument_groups.h).
Snapshot grouped(const Snapshot& s) {
  Project p = *s;
  return migrateToInstrumentGroups(p) ? std::make_shared<const Project>(std::move(p)) : s;
}
std::unique_ptr<ProjectLink> makeLink(const Snapshot& s0) {
  const Snapshot s = grouped(s0);
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

struct Driver {
  ProjectLink& link;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  std::vector<float> out;
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      out.insert(out.end(), l.begin(), l.end());
      out.insert(out.end(), r.begin(), r.end());
    }
  }
};

struct ScopeProject {
  DocumentStore store;
  TrackId track;
  DeviceId scope;
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  // A MIDI track playing `degrees`; `instrument`: the default synth then (optionally) the Oscilloscope; otherwise a meta
  // voice run Phase accumulator → Wavetable → (optionally) Oscilloscope.
  ScopeProject(bool instrument, bool withScope, std::vector<int32_t> degrees) {
    IdSource& ids = store.ids();
    track = ids.track();
    const DeviceId inst = instrument ? ids.device() : DeviceId{};
    Batch b{"scope", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Keys", inst), {}, kAppend});
    std::vector<Note> notes;
    for (const int32_t d : degrees) notes.push_back(playback_fixture::note(ids, 0, 64 * kPpq, d));
    b.commands.push_back(AddClip{track, playback_fixture::clip(ids, 0, 64 * kPpq, notes, "chord")});
    REQUIRE(store.dispatch(b).ok());
    playback_fixture::wireSeries(store);  // TX.all-free: wired in series
    if (!instrument) {
      // No instrument: a meta voice run Envelope → Wavetable plays the notes (GU.4: was a Phase accumulator).
      edit(ops::addMetaDevice(*store.current(), store.ids(), track, "envelope", 0));
      edit(ops::addMetaDevice(*store.current(), store.ids(), track, "wavetable"));
    }
    if (withScope) scope = edit(ops::addMetaDevice(*store.current(), store.ids(), track, "oscilloscope")).devices.at(0);
  }
};

struct Tap {
  std::vector<float> ch[4];
};
Tap readTap(const TapRing& t, uint32_t n) {
  Tap out;
  const uint64_t from = t.written() - n;
  for (uint32_t c = 0; c < 4; ++c) {
    out.ch[c].resize(n);
    REQUIRE(t.read(c, from, n, out.ch[c].data()) == n);
  }
  return out;
}

}  // namespace

TEST_CASE("scope link: after an instrument — the tap carries the beat, no note, the signal; unwatched nothing") {
  ScopeProject p(true, true, {0, 4, 7});
  auto link = makeLink(p.store.current());
  CHECK(link->lastError().empty());
  const std::shared_ptr<TapRing> tap = link->deviceAnalysisTap(p.scope);
  REQUIRE(tap != nullptr);
  CHECK(tap->channels() == 4);
  Driver d{*link};
  link->play(0);
  d.run(50);
  CHECK(tap->written() == 0);  // nobody watches: one load per block, no copy
  tap->addReader();
  d.run(100);
  CHECK(tap->written() == 100 * kBlock);
  const Tap t = readTap(*tap, 2048);
  // The beat: linear in time while the transport plays (float32: within a few ulp of the line through its ends).
  const double slope = (static_cast<double>(t.ch[0].back()) - t.ch[0].front()) / 2047.0;
  const double bpm = slope * 48000.0 * 60.0;
  MESSAGE("tapped beat moves at " << bpm << " bpm");
  CHECK(bpm > 20.0);
  CHECK(bpm < 999.0);
  for (uint32_t i = 0; i < 2048; i += 97)
    CHECK(t.ch[0][i] == doctest::Approx(t.ch[0].front() + slope * i).epsilon(1e-5));
  for (const float v : t.ch[1]) CHECK(v == 0.0f);  // global: no note
  float peak = 0.0f;
  for (const float v : t.ch[2]) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.01f);  // the chord plays
  tap->removeReader();
}

TEST_CASE("scope link: in a meta voice run the tap carries the lowest sounding voice's pitch") {
  auto noteOf = [](std::vector<int32_t> degrees) {
    ScopeProject p(false, true, std::move(degrees));
    auto link = makeLink(p.store.current());
    CHECK(link->lastError().empty());
    const std::shared_ptr<TapRing> tap = link->deviceAnalysisTap(p.scope);
    REQUIRE(tap != nullptr);
    Driver d{*link};
    link->play(0);
    tap->addReader();
    d.run(100);
    const Tap t = readTap(*tap, 1024);
    tap->removeReader();
    float peak = 0.0f;
    for (const float v : t.ch[2]) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.01f);  // the voices play through the run
    for (const float v : t.ch[1]) CHECK(v == t.ch[1].front());
    return t.ch[1].front();
  };
  const float root = noteOf({0}), fifth = noteOf({7}), chord = noteOf({7, 0, 4});
  MESSAGE("note pitch: root " << root << " Hz, fifth " << fifth << " Hz, chord " << chord << " Hz");
  CHECK(root > 20.0f);
  CHECK(fifth / root == doctest::Approx(std::pow(2.0, 7.0 / 12.0)).epsilon(1e-5));  // 12-EDO's fifth
  CHECK(chord == root);  // the lowest of the chord
}

TEST_CASE("scope link: the Oscilloscope is bit-exact pass-through (after an instrument, in a meta voice run)") {
  for (const bool instrument : {true, false}) {
    CAPTURE(instrument);
    ScopeProject with(instrument, true, {0, 4, 7, 11}), without(instrument, false, {0, 4, 7, 11});
    auto la = makeLink(with.store.current()), lb = makeLink(without.store.current());
    Driver da{*la}, db{*lb};
    la->play(0);
    lb->play(0);
    la->deviceAnalysisTap(with.scope)->addReader();  // watched or not, the sound is the same
    da.run(400);
    db.run(400);
    REQUIRE(da.out.size() == db.out.size());
    float peak = 0.0f;
    for (const float v : da.out) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.01f);
    CHECK(std::memcmp(da.out.data(), db.out.data(), da.out.size() * sizeof(float)) == 0);
    la->deviceAnalysisTap(with.scope)->removeReader();
  }
}

TEST_CASE("scope link: the engine block with an Oscilloscope unwatched / watched; silence sleeps as with a Spectrum" *
          doctest::test_suite("perf")) {
  ScopeProject k(true, true, {0, 4, 7, 11});
  auto link = makeLink(k.store.current());
  Driver d{*link};
  link->play(0);
  d.run(200);
  auto timeRun = [](Driver& dr, uint32_t blocks) {
    const auto t0 = std::chrono::steady_clock::now();
    dr.run(blocks);
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
  };
  const double idle = timeRun(d, 4000);
  link->deviceAnalysisTap(k.scope)->addReader();
  const double watched = timeRun(d, 4000);
  link->deviceAnalysisTap(k.scope)->removeReader();
  MESSAGE("engine block (64 frames), a synth chord + Oscilloscope: unwatched " << idle << " us, watched " << watched << " us");
  // A silent playing track (no notes) through EQ Eight then an analyser: the transport's beat the Oscilloscope taps
  // keeps nothing awake (a sink has no live output), so it costs what a Spectrum costs.
  auto silentCost = [&](const char* analyser) {
    DocumentStore store;
    const TrackId t = store.ids().track();
    Batch b{"silent", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(t, TrackKind::Midi, "Keys", store.ids().device()), {}, kAppend});
    REQUIRE(store.dispatch(b).ok());
    playback_fixture::wireSeries(store);  // TX.all-free: wired in series
    auto eq = ops::addDevice(*store.current(), store.ids(), t, "eq_eight", DeviceKind::AudioEffect);
    REQUIRE(eq.ok());
    REQUIRE(store.dispatch(eq->command).ok());
    auto an = ops::addMetaDevice(*store.current(), store.ids(), t, analyser);
    REQUIRE(an.ok());
    REQUIRE(store.dispatch(an->command).ok());
    auto l = makeLink(store.current());
    Driver dr{*l};
    l->play(0);
    dr.run(400);
    return timeRun(dr, 4000);
  };
  const double spectrum = silentCost("spectrum"), scope = silentCost("oscilloscope");
  MESSAGE("silent playing track, EQ Eight + Spectrum " << spectrum << " us/block, + Oscilloscope " << scope << " us/block");
}
