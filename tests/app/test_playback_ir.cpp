// T3.17-fix2: the mixer at block rate, IR lowering with stable ids (M3 review H1, H2), idle units.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/engine/program_image.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, bool idle = true) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  c.idleUnits = idle;
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
      for (uint32_t f = 0; f < kBlock; ++f) {
        out.push_back(l[f]);
        out.push_back(r[f]);
      }
    }
  }
};

double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t i = 0; i < a.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}

// A held chord on one track, a return it sends to, the master.
struct Chord {
  DocumentStore store;
  TrackId track, echo;
  Chord() {
    IdSource& ids = store.ids();
    track = ids.track();
    echo = ids.track();
    Batch b{"chord", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(echo, TrackKind::Return, "Echo"), {}, kAppend});
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Keys", ids.device()), {}, kAppend});
    b.commands.push_back(SetSendLevel{track, echo, -6.0f});
    std::vector<Note> notes;
    for (int32_t d : {0, 4, 7}) notes.push_back(playback_fixture::note(ids, 0, 16 * kPpq, d));
    b.commands.push_back(AddClip{track, playback_fixture::clip(ids, 0, 16 * kPpq, notes, "chord")});
    REQUIRE(store.dispatch(b).ok());
  }
};

// The state keys of every unit's current image, by unit name.
std::map<std::string, std::set<uint64_t>> stateKeys(engine::ProjectProgram& program) {
  std::map<std::string, std::set<uint64_t>> out;
  for (uint32_t u = 0; u < program.numUnits(); ++u)
    if (const engine::ProgramImage* img = program.current(u))
      for (const graph::StateEntry& e : img->stateLayout().entries) out[program.layout().units[u].name].insert(e.key);
  return out;
}

}  // namespace

// H2: adding a return used to rewrite every track's generated text (a send param and a tap export each), so every
// unit recompiled and crossfaded and every state key changed: held voices restarted their envelopes. Now the return
// set is not part of a track's patch: only the new return and the master (one more input) compile.
TEST_CASE("adding a return while a chord is held compiles only the return and the master; voices carry on") {
  Chord a, b;
  auto la = makeLink(a.store.current()), lb = makeLink(b.store.current());
  [[maybe_unused]] const ListenerId sub = a.store.subscribe([&](const StoreEvent& e) { la->onDocumentChange(e); });
  Driver da{*la}, db{*lb};
  la->play(0);
  lb->play(0);
  da.run(300);  // the chord sounds (0.4 s)
  db.run(300);
  engine::ProjectProgram& program = *la->engine().program();
  const auto keysBefore = stateKeys(program);
  uint64_t keysSwapsBefore = 0;
  for (uint32_t u = 0; u < program.numUnits(); ++u)
    if (program.layout().units[u].voices) keysSwapsBefore = program.unitStats(u).swaps;
  const app::LinkStats s0 = la->stats();
  REQUIRE(a.store.dispatch(AddTrack{playback_fixture::track(a.store.ids().track(), TrackKind::Return, "Verb"), {}, kAppend}).ok());
  la->flush();
  CHECK(la->stats().compiles == s0.compiles + 1);
  CHECK(la->stats().unitCompiles == s0.unitCompiles + 2);  // Verb and the master; Keys and Echo run on untouched
  da.run(300);
  db.run(300);
  CHECK(program.stats().topologySwitches == 1);
  // The instrument unit got no new image at all: its voices (envelopes, phases) were never touched.
  for (uint32_t u = 0; u < program.numUnits(); ++u)
    if (program.layout().units[u].voices) {
      CHECK(program.unitStats(u).swaps == keysSwapsBefore);
      CHECK(program.voices(u)->liveVoices() == 3);
    }
  // Every unit that existed keeps every state key it had (equal keys: the master's smoothers carry over).
  const auto keysAfter = stateKeys(program);
  for (const auto& [name, keys] : keysBefore) {
    CAPTURE(name);
    REQUIRE(keysAfter.count(name) == 1);
    for (uint64_t k : keys) CHECK(keysAfter.at(name).count(k) == 1);
  }
  // The new return is silent (no send to it): the output equals the run without the edit, sample for sample.
  CHECK(maxDiff(da.out, db.out) == 0.0);
}

// H1 (3) / T1.x-silence's start: a unit whose outputs are provably zero while its inputs are silent and no voice
// sounds, and whose gains have settled, is skipped; the skip is exact, so a render with it equals one without.
TEST_CASE("idle units: silent units are skipped, bit-exactly; events, params and signal wake them") {
  playback_fixture::Fixture fa, fb;
  auto on = makeLink(fa.snap(), true), off = makeLink(fb.snap(), false);
  [[maybe_unused]] const ListenerId sa = fa.store.subscribe([&](const StoreEvent& e) { on->onDocumentChange(e); });
  [[maybe_unused]] const ListenerId sb = fb.store.subscribe([&](const StoreEvent& e) { off->onDocumentChange(e); });
  Driver da{*on}, db{*off};
  on->play(0);
  off->play(0);
  // The fixture plays 2 bars (4 s); then silence, a param change in the silence, and a note added to a silent track.
  da.run(3750);  // 5 s
  db.run(3750);
  engine::ProjectProgram& program = *on->engine().program();
  uint64_t idle = 0;
  for (uint32_t u = 0; u < program.numUnits(); ++u) idle += program.unitStats(u).idleBlocks;
  CHECK(idle > 0);
  for (auto* f : {&fa, &fb}) {
    REQUIRE(f->store.dispatch(SetTrackVolume{f->lead, -12.0f}).ok());
    REQUIRE(f->store.dispatch(SetTrackPan{f->grp, 0.3f}).ok());
  }
  da.run(400);
  db.run(400);
  for (auto* f : {&fa, &fb}) {
    std::vector<Note> n{playback_fixture::note(f->store.ids(), 0, kPpq, 3)};
    REQUIRE(f->store.dispatch(AddClip{f->mos, playback_fixture::clip(f->store.ids(), 12 * kPpq, kPpq, n, "late")}).ok());
  }
  on->flush();
  off->flush();
  da.run(1200);
  db.run(1200);
  float peak = 0.0f;
  for (size_t i = 2 * 48000 * 5; i < da.out.size(); ++i) peak = std::max(peak, std::fabs(da.out[i]));
  CHECK(peak > 0.001f);  // the late note sounds (its track woke)
  CHECK(maxDiff(da.out, db.out) == 0.0);
}

// H1: the mixer's own cost does not grow per sample with the returns: a closed send is a settled gain_add at 0.
TEST_CASE("lowering: the mixer is gain / gain_add at block rate; a return holds one gain_add per sender") {
  playback_fixture::Fixture f;
  // The patches: Lead's has three gain stages (out, post, pre); Echo's one gain_add per sending track.
  auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  LowerOptions lo;
  lo.library = &(*lib)->library();
  uint32_t slot = 0;
  lo.slotOf = [&](TrackId) { return slot++; };
  auto low = lower(*f.snap(), lo);
  REQUIRE(low.ok());
  auto count = [&](TrackId id, const char* atom) {
    uint32_t n = 0;
    for (size_t i = 0; i < low.value().trackIds.size(); ++i)
      if (low.value().trackIds[i] == id) {
        const graph::Patch& p = low.value().patches[i]->patch;
        for (graph::NodeIndex k = 0; k < p.nodeCount(); ++k)
          n += p.node(k).kind == graph::NodeKind::Atom && p.node(k).atom == graph::atomId(atom);
      }
    return n;
  };
  CHECK(count(f.lead, "gain") == 3);
  CHECK(count(f.lead, "z1") == 0);  // no per-sample smoothing loop any more
  CHECK(count(f.echo, "gain_add") == 4);  // Lead, Grp, Mos, Muted
  CHECK(count(f.master, "gain") == 1);
}
