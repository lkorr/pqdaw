#pragma once

// A small project used by the document tests, built through commands:
//   master
//   returns: Reverb (return)
//   tracks:  Lead (MIDI, clip "A" with 300 notes in 31-EDO degrees, clip "B" with 3 notes)
//            Drums (group) > Kick (MIDI, clip "K", 4 notes)
//            Bass (MIDI, no clips)
//   tunings: 12-EDO (default), 31-EDO, 5L2s (MOS)
//   Lead has a volume lane with 3 breakpoints; Bass a lane on 5L2s's generator.

#include <doctest/doctest.h>

#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/store.h"

// CHECK(a == b) on string_views and shared_ptrs (doctest cannot print either by itself here; std's operator<< does
// not compile for shared_ptr<T[]>).
namespace doctest {
template <>
struct StringMaker<std::string_view> {
  static String convert(std::string_view v) { return String(v.data(), static_cast<String::size_type>(v.size())); }
};
template <class T>
struct StringMaker<std::shared_ptr<T>> {
  static String convert(const std::shared_ptr<T>& p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%p", static_cast<const void*>(p.get()));
    return buf;
  }
};
}  // namespace doctest

namespace doctest_fixture {

using namespace pq::doc;

inline Note makeNote(IdSource& ids, pq::Ticks start, int32_t degree, pq::Ticks length = pq::kPpq / 2) {
  Note n;
  n.id = ids.note();
  n.start = start;
  n.length = length;
  n.pitch = pq::tuning::PitchId::fromDegree(degree);
  return n;
}

inline ClipPtr makeClip(IdSource& ids, pq::Ticks start, pq::Ticks length, std::vector<Note> notes,
                        std::string name = "clip") {
  NoteClip c;
  c.id = ids.clip();
  c.name = std::move(name);
  c.start = start;
  c.length = length;
  auto seq = NoteSeq::fromNotes(std::move(notes));
  REQUIRE(seq.ok());
  c.notes = std::move(seq).value();
  return std::make_shared<const NoteClip>(std::move(c));
}

inline TrackPtr makeTrackPtr(TrackId id, TrackKind kind, std::string name) {
  return std::make_shared<const Track>(makeTrack(id, kind, std::move(name)));
}

struct Fixture {
  DocumentStore store;
  TrackId master, reverb, lead, drums, kick, bass;
  ClipId clipA, clipB, clipK;
  TuningId t12, t31, tMos;
  LaneId leadVolLane, bassGenLane;
  BreakpointId bp0, bp1, bp2;
  std::vector<Note> notesA;  // as added (unsorted order = sorted here)
  LocatorId loc1;

  Fixture() {
    IdSource& ids = store.ids();
    const Snapshot& p = store.current();
    master = p->master->id;
    t12 = p->tunings->front()->id;
    reverb = ids.track();
    lead = ids.track();
    drums = ids.track();
    kick = ids.track();
    bass = ids.track();
    t31 = ids.tuning();
    tMos = ids.tuning();

    Batch b{"setup", {}};
    TuningSlot s31{t31, "31-EDO", Colour{0x22AA44}, {}};
    s31.def.system = pq::tuning::Edo::make(31).value();
    b.commands.push_back(AddTuning{s31});
    TuningSlot sMos{tMos, "5L2s", Colour{0xAA4422}, {}};
    sMos.def.system = pq::tuning::MosParams{};
    b.commands.push_back(AddTuning{sMos});
    b.commands.push_back(AddTrack{makeTrackPtr(reverb, TrackKind::Return, "Reverb"), {}, kAppend});
    b.commands.push_back(AddTrack{makeTrackPtr(lead, TrackKind::Midi, "Lead"), {}, kAppend});
    b.commands.push_back(AddTrack{makeTrackPtr(drums, TrackKind::Group, "Drums"), {}, kAppend});
    b.commands.push_back(AddTrack{makeTrackPtr(kick, TrackKind::Midi, "Kick"), drums, kAppend});
    b.commands.push_back(AddTrack{makeTrackPtr(bass, TrackKind::Midi, "Bass"), {}, kAppend});
    b.commands.push_back(SetTrackTuning{lead, TuningAssignment::mainTuning(t31)});
    b.commands.push_back(SetSendLevel{lead, reverb, -10.0f});

    for (int i = 0; i < 300; ++i) notesA.push_back(makeNote(ids, i * (pq::kPpq / 4), i % 31));
    ClipPtr a = makeClip(ids, 0, 4 * pq::kTicksPerWhole, notesA, "A");
    clipA = a->id;
    b.commands.push_back(AddClip{lead, a});
    ClipPtr cb = makeClip(ids, 8 * pq::kTicksPerWhole, pq::kTicksPerWhole,
                          {makeNote(ids, 0, 0), makeNote(ids, pq::kPpq, 2), makeNote(ids, 2 * pq::kPpq, 4)}, "B");
    clipB = cb->id;
    b.commands.push_back(AddClip{lead, cb});
    std::vector<Note> kn;
    for (int i = 0; i < 4; ++i) kn.push_back(makeNote(ids, i * pq::kPpq, 0));
    ClipPtr ck = makeClip(ids, 0, pq::kTicksPerWhole, kn, "K");
    clipK = ck->id;
    b.commands.push_back(AddClip{kick, ck});

    AutomationLane vol;
    vol.id = leadVolLane = ids.lane();
    vol.param = ParamRef::track(ParamKind::TrackVolume, lead);
    bp0 = ids.breakpoint();
    bp1 = ids.breakpoint();
    bp2 = ids.breakpoint();
    vol.points = {{0, -6.0f, 0.0f, bp0}, {pq::kTicksPerWhole, 0.0f, 0.5f, bp1}, {2 * pq::kTicksPerWhole, -12.0f, 0.0f, bp2}};
    b.commands.push_back(AddAutomationLane{lead, std::make_shared<const AutomationLane>(vol)});
    AutomationLane gen;
    gen.id = bassGenLane = ids.lane();
    gen.param = ParamRef::tuning(ParamKind::MosGenerator, tMos);
    gen.points = {{0, 696.0f, 0.0f, ids.breakpoint()}};
    b.commands.push_back(AddAutomationLane{bass, std::make_shared<const AutomationLane>(gen)});
    loc1 = ids.locator();
    b.commands.push_back(AddLocator{Locator{loc1, pq::kTicksPerWhole, "Verse"}});

    const pq::Status st = store.dispatch(b);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }

  const Project& doc() const { return *store.current(); }
  const Track& track(TrackId id) const { return *findTrackPtr(doc(), id); }
  const NoteClip& clip(TrackId t, ClipId c) const {
    const Track& tr = track(t);
    return *tr.clips[static_cast<size_t>(findClipIndex(tr, c))].clip;
  }
  NoteKey keyA(size_t i) const { return keyOf(notesA[i]); }
};

}  // namespace doctest_fixture
