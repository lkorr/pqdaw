// TMM.3: meta modules through the playback link — a nested Phase accumulator's freq drag is param writes only (no
// compile job, no unit compile, the unit keeps its image: the unit hash is unchanged) and audible; a chain-level
// `meta:phase_acc` instrument plays its voices; a MetaData edit compiles nothing.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../meta/meta_test_type.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/engine/project_program.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
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
    }
  }
};

std::string replaced(std::string s, const std::string& from, const std::string& to) {
  const size_t at = s.find(from);
  REQUIRE_MESSAGE(at != std::string::npos, from);
  s.replace(at, from.size(), to);
  return s;
}

// A held chord on a MIDI track with BasicSynth.
struct Keys {
  DocumentStore store;
  TrackId track;
  DeviceId inst;
  Keys() {
    IdSource& ids = store.ids();
    track = ids.track();
    inst = ids.device();
    Batch b{"keys", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Keys", inst), {}, kAppend});
    std::vector<Note> notes;
    for (int32_t d : {0, 4, 7}) notes.push_back(playback_fixture::note(ids, 0, 64 * kPpq, d));
    b.commands.push_back(AddClip{track, playback_fixture::clip(ids, 0, 64 * kPpq, notes, "chord")});
    REQUIRE(store.dispatch(b).ok());
  }
  void edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
  }
};

uint32_t voiceUnit(engine::ProjectProgram& program) {
  for (uint32_t u = 0; u < program.numUnits(); ++u)
    if (program.layout().units[u].voices) return u;
  FAIL("no instrument unit");
  return 0;
}

double rms(const std::vector<float>& v, size_t from, size_t to) {
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += static_cast<double>(v[i]) * v[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

}  // namespace

TEST_CASE("meta link: a nested Phase accumulator's freq drag compiles nothing and is audible") {
  Keys k;
  auto link = makeLink(k.store.current());
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(50);
  // Fork the synth with a Phase accumulator as its oscillator phase (one structural compile of its unit).
  const std::string text = replaced(link->devices().stockText("BasicSynth"), "ph  = phasor(voice.hz)",
                                    "acc1 = meta_phase_acc()\n  ph  = acc1");
  app::LinkStats s0 = link->stats();
  k.edit(ops::addMetaCall(*k.store.current(), k.inst, text, "acc1", "phase_acc"));
  link->flush();
  CHECK(link->stats().compiles == s0.compiles + 1);
  CHECK(link->stats().compileErrors == s0.compileErrors);
  CHECK(link->lastError().empty());
  d.run(100);
  engine::ProjectProgram& program = *link->engine().program();
  const uint32_t u = voiceUnit(program);
  CHECK(program.voices(u)->liveVoices() == 3);
  const engine::ProgramImage* image = program.current(u);
  REQUIRE(image != nullptr);
  const std::string key = metaParamKey(k.inst, "acc1", "freq");
  REQUIRE(link->paramSlot(key) != ProjectLink::kNoSlot);
  const size_t mark = d.out.size();
  d.run(100);
  const double before = rms(d.out, mark, d.out.size());
  // A drag: 16 values in one gesture. Param writes only.
  s0 = link->stats();
  REQUIRE(k.store.beginGesture().ok());
  for (int i = 1; i <= 16; ++i) {
    k.edit(ops::setMetaParam(*k.store.current(), k.inst, "acc1", "phase_acc", "freq", 250.0 * i));
    link->flush();
  }
  REQUIRE(k.store.commitGesture().ok());
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().unitCompiles == s0.unitCompiles);
  CHECK(link->stats().topologyChanges == s0.topologyChanges);
  CHECK(link->stats().paramWrites >= s0.paramWrites + 16);
  d.run(50);
  CHECK(program.current(u) == image);  // the same image: the unit (and its hash) did not change
  CHECK(program.voices(u)->liveVoices() == 3);
  // Audible: 4 kHz added to every voice's frequency changes the sound.
  const size_t mark2 = d.out.size();
  d.run(100);
  const double after = rms(d.out, mark2, d.out.size());
  CHECK(before > 1e-4);
  CHECK(after > 1e-4);
  bool differs = false;
  for (size_t i = 0; i < 6400 && !differs; ++i) differs = d.out[mark + i] != d.out[mark2 + i];
  CHECK(differs);
  // A setting-free nested param back to its default: still a write.
  s0 = link->stats();
  k.edit(ops::setMetaParam(*k.store.current(), k.inst, "acc1", "phase_acc", "freq", std::nullopt));
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().paramWrites == s0.paramWrites + 1);
}

TEST_CASE("meta link: a chain meta:phase_acc instrument plays its voices; MetaData edits compile nothing") {
  meta::testing::registerTestType();
  Keys k;
  auto link = makeLink(k.store.current());
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(20);
  // The Phase accumulator replaces the instrument (the chain rule: one instrument, first).
  k.edit(ops::addMetaDevice(*k.store.current(), k.store.ids(), k.track, "phase_acc", 0));
  link->flush();
  CHECK(link->lastError().empty());
  const DeviceId acc = findTrackPtr(*k.store.current(), k.track)->devices.at(0)->id;
  REQUIRE(isMetaDevice(*findDevice(*k.store.current(), acc).device));
  d.run(100);
  engine::ProjectProgram& program = *link->engine().program();
  const uint32_t u = voiceUnit(program);
  CHECK(program.voices(u)->liveVoices() == 3);
  const size_t mark = d.out.size();
  d.run(50);
  CHECK(rms(d.out, mark, d.out.size()) > 1e-3);  // three ramps (DC and all) through the mixer
  // Its own param is a device param: a write, no compile.
  app::LinkStats s0 = link->stats();
  k.edit(ops::setMetaParam(*k.store.current(), acc, "", "phase_acc", "amp", 0.5));
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().paramWrites == s0.paramWrites + 1);
  // A data edit (a nested test module's samples) compiles nothing either.
  k.edit(ops::setMetaSetting(*k.store.current(), acc, "x", "test_shape", "stages", 1.0));  // state only: no call
  link->flush();
  s0 = link->stats();
  k.edit(ops::setMetaData(*k.store.current(), acc, "x", "test_shape",
                          std::make_shared<const meta::DataValue>(meta::DataValue::record({{"values", meta::DataValue::floatArray({0.25f})}}))));
  CHECK(k.store.lastChanges().metaDataOnly());
  link->flush();
  CHECK(link->stats().compiles == s0.compiles);
  CHECK(link->stats().paramWrites == s0.paramWrites);
}
