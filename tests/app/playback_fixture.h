#pragma once

// T3.17: the playback fixture project, built through document commands (and committed as
// tests/app/fixtures/playback/project.pqd, regenerated with PQ_UPDATE_GOLDENS=1; GU.11: saved by this version — the
// MIDI tracks' instruments are the default instrument group, wired in series):
//   tunings  12-EDO (default), 31-EDO, 5L2s (MOS, generator 705 c)
//   Lead     MIDI in 31-EDO: degrees 0, 10, 18, 31 (one equave) as quarter notes, then a 2-note dyad;
//            send to Echo at −6 dB (post-fader)
//   Grp      group with a volume lane (−18 dB at bar 1 → 0 dB at bar 2, curved) > Mos (MIDI in 5L2s: degrees
//            0 2 4 5 7 (the octave) as eighth notes)
//   Muted    MIDI, activator off (mute): its notes must not be heard
//   Echo     return (fed by Lead's send)
// 120 BPM 4/4: a bar is 2 s. Every time is a multiple of the PPQ constant.

#include <doctest/doctest.h>

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <string>
#include <span>
#include <vector>

#include "../support/knob_positions.h"
#include "../support/blocks.h"
#include "pq/app/project_link.h"
#include "pq/core/time.h"
#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/store.h"

namespace playback_fixture {

using namespace pq::doc;

// GU.1 (a param stores its knob position): the block library, loaded once — what these tests' values in units
// become knob positions with (tests/support/knob_positions.h).
inline const DeviceLibrary& unitsLibrary() {
  static const auto lib = pq::app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}
inline Command inUnits(const DocumentStore& s, Command c) {
  return knob_positions::inUnits(*s.current(), std::move(c), unitsLibrary());
}

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

// TX.all-free: wires every track of the store's project that has no main link in series — its source → its devices in
// list order → its mixer (doc::ops::wireSeries, what the schema 16 → 17 migration does). Builders that add tracks and
// devices with raw commands call it once they are built.
// `offPath`: devices left out of the series (blocks placed unwired: what a "free" device was before TX.all-free).
// IG.1-2: every MIDI track's instrument or meta voice run on its path into an instrument group (as an older file loads:
// pq/document/instrument_groups.h), one undo step. A meta voice run then plays per note again.
inline void asInstrumentGroups(DocumentStore& s) {
  auto e = ops::wrapInstrumentGroups(*s.current(), s.ids());
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  if (e->empty()) return;
  const pq::Status st = s.dispatch(e->command);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
}

// GU.11: the default instrument group (its id `id`) of MIDI track `t` as commands of `b` (after the AddTrack: the group,
// its blocks, the wires among them). Not wired to the track: wireSeries does it.
inline DeviceId instrument(Batch& b, IdSource& ids, TrackId t, DeviceId id) {
  return test_blocks::defaultInstrumentCommands(b, ids, t, id);
}

// GU.11: a new MIDI track's instrument group deleted (with its blocks): the path empty (source → mixer), so a native
// engine added at slot 0 takes the notes as these cases were written.
inline void clearHead(DocumentStore& s, TrackId t) {
  const MainPath path = mainPath(*s.current(), *findTrackPtr(*s.current(), t));
  if (path.blocks.empty() || !isGroupDevice(*path.blocks.front())) return;
  auto e = ops::deleteDevices(*s.current(), std::vector<DeviceId>{path.blocks.front()->id});
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
}

inline void wireSeries(DocumentStore& s, const std::vector<DeviceId>& offPath = {}) {
  if (offPath.empty()) {
    auto e = ops::wireSeries(*s.current(), s.ids());
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    if (e->empty()) return;
    const pq::Status st = s.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    return;
  }
  Batch b{"Wire in Series", {}};
  const Project& p = *s.current();
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const Wire& w : wiresOf(p))
      if ((w.fromNoteSource() && w.noteSource == t.id && w.port.empty()) || (w.toMixer() && w.mixer == t.id)) return;
    Wire at;
    at.noteSource = t.id;
    for (const DevicePtr& d : t.devices) {
      if (d->group.valid() || std::find(offPath.begin(), offPath.end(), d->id) != offPath.end()) continue;
      Wire w = at;
      w.id = s.ids().wire();
      w.to = d->id;
      b.commands.push_back(AddWire{w});
      at = Wire{};
      at.from = d->id;
    }
    Wire w = at;
    w.id = s.ids().wire();
    w.mixer = t.id;
    b.commands.push_back(AddWire{w});
  });
  if (b.commands.empty()) return;
  const pq::Status st = s.dispatch(b);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
}

// TX.all-free: of `states` (a lowering's, in id order), the wires that are not main links (the ones a test draws: the
// main paths' links are the series), and the project's such wires.
inline std::vector<WireState> drawn(std::span<const WireState> states, const Project& p) {
  std::vector<WireState> out;
  for (const WireState& s : states)
    if (const Wire* w = findWire(p, s.id); w != nullptr && !w->mainLink()) out.push_back(s);
  return out;
}
inline WireList drawnWires(const Project& p) {
  WireList out;
  for (const Wire& w : wiresOf(p))
    if (!w.mainLink()) out.push_back(w);
  return out;
}

// TX.all-free: whether `device` is on its track's main path (what "in the chain" was).
inline bool onPath(const Project& p, DeviceId device) {
  const DeviceLoc l = findDevice(p, device);
  return l.track != nullptr && mainPath(p, *l.track).contains(device);
}

// TX.all-free: a new project whose master is not wired yet (makeDefaultProject's `wireMaster` false): a builder that
// reproduces an older file's ids wires every track at the end with wireSeries, in the migration's order.
inline Snapshot unwiredDefaultProject() {
  IdSource ids;
  return makeDefaultProject(ids, false);
}

// A track with no device; GU.11: a MIDI track's instrument is added after it (instrument(): the default instrument
// group's blocks and inner wires; TX.all-free: wired into its track by wireSeries).
inline TrackPtr track(TrackId id, TrackKind kind, std::string name) {
  Track t = makeTrack(id, kind, std::move(name));
  // TX.pan-laws: a MIDI track of these tests pans with constant power, the law their references (hand-built graphs,
  // the committed fixtures, goldens) were made with before Balance became every track's default.
  if (kind == TrackKind::Midi) t.mixer.panMode = PanMode::ConstantPower;
  return std::make_shared<const Track>(std::move(t));
}

struct Fixture {
  DocumentStore store{unwiredDefaultProject()};  // TX.all-free: the committed file's ids (its links come last)
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

    // T4.1 / GU.11: the default instrument groups, with ids after every other entity and in track order (Lead, Mos
    // inside Grp, Muted).
    for (const TrackId t : {lead, mos, muted}) (void)instrument(b, ids, t, ids.device());

    const pq::Status st = store.dispatch(b);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    wireSeries(store);  // TX.all-free: every track's chain as cables, as the migration gives the committed file
  }
  const Snapshot& snap() const { return store.current(); }
};

}  // namespace playback_fixture
