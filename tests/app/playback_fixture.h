#pragma once

// T3.17: the playback fixture project, built through document commands (and committed as
// tests/app/fixtures/playback/project.pqd, regenerated with PQ_UPDATE_GOLDENS=1):
//   tunings  12-EDO (default), 31-EDO, 5L2s (MOS, generator 705 c)
//   Lead     MIDI in 31-EDO: degrees 0, 10, 18, 31 (one equave) as quarter notes, then a 2-note dyad;
//            send to Echo at −6 dB (post-fader)
//   Grp      group with a volume lane (−18 dB at bar 1 → 0 dB at bar 2, curved) > Mos (MIDI in 5L2s: degrees
//            0 2 4 5 7 (the octave) as eighth notes)
//   Muted    MIDI, activator off (mute): its notes must not be heard
//   Echo     return (fed by Lead's send)
// 120 BPM 4/4: a bar is 2 s. Every time is a multiple of the PPQ constant.

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "pq/core/time.h"
#include "pq/document/commands.h"
#include "pq/document/store.h"

namespace playback_fixture {

using namespace pq::doc;

inline Note note(IdSource& ids, pq::Ticks start, pq::Ticks length, int32_t degree, int32_t equave = 0,
                 float velocity = 100.0f) {
  Note n;
  n.id = ids.note();
  n.start = start;
  n.length = length;
  n.pitch = pq::tuning::PitchId::fromDegree(degree, equave);
  n.velocity = velocity;
  return n;
}

inline ClipPtr clip(IdSource& ids, pq::Ticks start, pq::Ticks length, std::vector<Note> notes, std::string name) {
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

inline TrackPtr track(TrackId id, TrackKind kind, std::string name) {
  return std::make_shared<const Track>(makeTrack(id, kind, std::move(name)));
}

struct Fixture {
  DocumentStore store;
  TrackId master, echo, lead, grp, mos, muted;
  TuningId t31, tMos;
  ClipId leadClip, mosClip, mutedClip;
  LaneId grpVol;

  Fixture() {
    IdSource& ids = store.ids();
    master = store.current()->master->id;
    echo = ids.track();
    lead = ids.track();
    grp = ids.track();
    mos = ids.track();
    muted = ids.track();
    t31 = ids.tuning();
    tMos = ids.tuning();
    using pq::kPpq;
    Batch b{"playback fixture", {}};
    TuningSlot s31{t31, "31-EDO", Colour{0x22AA44}, {}};
    s31.def.system = pq::tuning::Edo::make(31).value();
    b.commands.push_back(AddTuning{s31});
    TuningSlot sMos{tMos, "5L2s", Colour{0xAA4422}, {}};
    pq::tuning::MosParams mp;
    mp.generator = 705.0;
    sMos.def.system = mp;
    b.commands.push_back(AddTuning{sMos});
    b.commands.push_back(AddTrack{track(echo, TrackKind::Return, "Echo"), {}, kAppend});
    b.commands.push_back(AddTrack{track(lead, TrackKind::Midi, "Lead"), {}, kAppend});
    b.commands.push_back(AddTrack{track(grp, TrackKind::Group, "Grp"), {}, kAppend});
    b.commands.push_back(AddTrack{track(mos, TrackKind::Midi, "Mos"), grp, kAppend});
    b.commands.push_back(AddTrack{track(muted, TrackKind::Midi, "Muted"), {}, kAppend});
    b.commands.push_back(SetTrackTuning{lead, TuningAssignment::mainTuning(t31)});
    b.commands.push_back(SetTrackTuning{mos, TuningAssignment::mainTuning(tMos)});
    b.commands.push_back(SetSendLevel{lead, echo, -6.0f});
    b.commands.push_back(SetTrackVolume{lead, -3.0f});
    b.commands.push_back(SetTrackPan{lead, -0.4f});
    b.commands.push_back(SetTrackPan{mos, 0.5f});
    b.commands.push_back(SetTrackMute{muted, true});

    std::vector<Note> ln;
    const int32_t degs[] = {0, 10, 18, 31};
    for (int i = 0; i < 4; ++i) ln.push_back(note(ids, i * kPpq, kPpq / 2, degs[i] % 31, degs[i] / 31, 90.0f + 8.0f * i));
    ln.push_back(note(ids, 4 * kPpq, 2 * kPpq, 0, 0, 100.0f));
    ln.push_back(note(ids, 4 * kPpq, 2 * kPpq, 18, 0, 80.0f));
    ClipPtr lc = clip(ids, 0, 2 * pq::kTicksPerWhole, ln, "Lead 31");
    leadClip = lc->id;
    b.commands.push_back(AddClip{lead, lc});

    std::vector<Note> mn;
    const int32_t mdeg[] = {0, 2, 4, 5, 7};
    for (int i = 0; i < 5; ++i) mn.push_back(note(ids, i * (kPpq / 2), kPpq / 2, mdeg[i] % 7, mdeg[i] / 7, 110.0f));
    ClipPtr mc = clip(ids, 0, pq::kTicksPerWhole, mn, "Mos 5L2s");
    mosClip = mc->id;
    b.commands.push_back(AddClip{mos, mc});

    std::vector<Note> xn;
    for (int i = 0; i < 4; ++i) xn.push_back(note(ids, i * kPpq, kPpq / 2, 7, 1, 127.0f));
    ClipPtr xc = clip(ids, 0, pq::kTicksPerWhole, xn, "Muted");
    mutedClip = xc->id;
    b.commands.push_back(AddClip{muted, xc});

    AutomationLane vol;
    vol.id = grpVol = ids.lane();
    vol.param = ParamRef::track(ParamKind::TrackVolume, grp);
    vol.points = {{0, -18.0f, 0.4f, ids.breakpoint()}, {2 * kPpq, 0.0f, 0.0f, ids.breakpoint()}};
    b.commands.push_back(AddAutomationLane{grp, std::make_shared<const AutomationLane>(vol)});

    const pq::Status st = store.dispatch(b);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  const Snapshot& snap() const { return store.current(); }
};

}  // namespace playback_fixture
