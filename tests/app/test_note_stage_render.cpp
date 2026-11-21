// AP.7 (docs/plans/AP.md "Roll preview + Flatten"; pq/app/note_stage_render.h, pq/document/note_flatten.h): the
// offline note-stage render — what the roll preview draws and Flatten writes — equals what the engine plays. For a
// set of arpeggiator configs (pattern styles incl. Random and Harmonic Walk, Pulse 8:9:10:12, glides, Chance / Tie
// lanes) the generated events of renderTrackNoteStages are compared with the events the track's slot hands its voices
// in an offline render of the playback link (Engine::setEventProbe): note-ons by song sample, Hz, identity, velocity;
// offs; Pitch ramps (target, length, shape). Then Flatten: the flattened clip with the arpeggiator off renders the
// same note-ons / offs (identities exact), and one undo restores the notes and the device.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/note_stage_render.h"
#include "pq/app/project_link.h"
#include "pq/document/note_devices.h"
#include "pq/document/note_flatten.h"
#include "pq/engine/arp.h"
#include "pq/engine/note_stage.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr double kRate = 48000.0;
constexpr Ticks kQ = kPpq;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

double position(const char* name, double value) {
  const NoteParamSpec* s = findNoteDeviceType("arpeggiator")->param(name);
  REQUIRE(s != nullptr);
  return noteParamPosition(*s, value);
}

struct Rig {
  DocumentStore store{playback_fixture::unwiredDefaultProject()};
  TrackId track;
  ClipId clip;
  DeviceId probe, arp;
  explicit Rig(std::vector<int32_t> degrees = {7, 0, 4}, Ticks len = 8 * kQ) {
    IdSource& ids = store.ids();
    track = ids.track();
    Batch b{"arp fixture", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Arp"), {}, kAppend});
    std::vector<Note> chord;
    float vel = 90.0f;
    for (const int32_t d : degrees) chord.push_back(playback_fixture::note(ids, 0, len, d, 0, vel += 7.0f));
    ClipPtr c = playback_fixture::clip(ids, 0, len, std::move(chord), "Chord");
    clip = c->id;
    b.commands.push_back(AddClip{track, c});
    REQUIRE(store.dispatch(b).ok());
    playback_fixture::wireSeries(store);
    probe = test_blocks::addProbeInstrument(store, track);
    arp = edit(ops::addDevice(doc(), ids, track, noteDevicePatch("arpeggiator"), DeviceKind::AudioEffect)).devices.at(0);
    param("rateDen", 16.0);
  }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  void param(const char* name, double value) { edit(ops::setDeviceParam(doc(), arp, name, position(name, value))); }
  void setting(const char* name, double value) { edit(ops::setDeviceSetting(doc(), arp, name, value)); }
};

struct Ev {
  SampleTime song;
  engine::NoteEvent e;
};

// What the slot hands its voices in an offline render of `seconds` from the start (song samples ≥ 0).
std::vector<Ev> engineEvents(const Rig& r, double seconds) {
  auto link = makeLink(r.store.current());
  const std::optional<uint32_t> slot = link->eventSlotOf(r.track);
  REQUIRE(slot.has_value());
  engine::Engine::EventProbe probe;
  probe.entries.reserve(1 << 16);
  link->engine().setEventProbe(*slot, &probe);
  engine::BufferSink sink;
  auto st = link->renderOffline(0, static_cast<uint64_t>(std::llround(seconds * kRate)), sink);
  link->engine().setEventProbe(*slot, nullptr);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  CHECK(probe.dropped == 0);
  std::vector<Ev> out;
  for (const auto& en : probe.entries)
    if (en.song >= 0) out.push_back({en.song, en.e});
  return out;
}

app::TrackNoteRender offline(const Rig& r) {
  app::NoteStageRenderOptions o;
  o.sampleRate = kRate;
  o.keepEvents = true;
  auto res = app::renderTrackNoteStages(r.doc(), r.track, o);
  REQUIRE_MESSAGE(res.ok(), (res.ok() ? std::string() : res.error().message));
  REQUIRE(res->any);
  return std::move(res.value());
}

bool same(float a, float b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

// The generated events (stage ids) of the engine render equal the offline render's, event for event.
void checkEqual(const std::vector<Ev>& eng, const engine::NoteStageRender& off, SampleTime until) {
  std::vector<Ev> a, b;
  for (const Ev& e : eng)
    if (engine::isStageNoteId(e.e.noteId) && e.song < until) a.push_back(e);
  for (const engine::TimedNoteEvent& e : off.output)
    if (engine::isStageNoteId(e.e.noteId) && e.song >= 0 && e.song < until) b.push_back({e.song, e.e});
  REQUIRE(a.size() == b.size());
  REQUIRE(!a.empty());
  size_t ons = 0, pitches = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    CAPTURE(i);
    const engine::NoteEvent& x = a[i].e;
    const engine::NoteEvent& y = b[i].e;
    CHECK(a[i].song == b[i].song);
    CHECK(x.kind == y.kind);
    CHECK(x.noteId == y.noteId);
    if (x.kind == engine::NoteEventKind::NoteOn) {
      ++ons;
      CHECK(x.hz == y.hz);
      CHECK(x.velocity == y.velocity);
      CHECK(x.gain == y.gain);
      CHECK(same(x.ratio, y.ratio));
      CHECK(same(x.harmonic, y.harmonic));
      CHECK(same(x.degree, y.degree));
      CHECK(same(x.equave, y.equave));
      CHECK(same(x.noteDegree, y.noteDegree));
    } else if (x.kind == engine::NoteEventKind::Pitch) {
      ++pitches;
      CHECK(x.hz == y.hz);
      CHECK(x.rampFrames == y.rampFrames);
      CHECK(x.rampShape == y.rampShape);
      CHECK(x.rampBend == y.rampBend);
    }
  }
  CHECK(ons > 0);
  MESSAGE("events " << a.size() << ", note-ons " << ons << ", pitch ramps " << pitches);
}

SampleTime barsSamples(double bars) { return static_cast<SampleTime>(std::llround(bars * 2.0 * kRate)); }  // 120 BPM

void checkConfig(Rig& r, double bars = 2.0) {
  const std::vector<Ev> eng = engineEvents(r, bars * 2.0 + 0.5);
  const app::TrackNoteRender off = offline(r);
  checkEqual(eng, off.events, barsSamples(bars));
  REQUIRE(off.clips.size() == 1);
  CHECK(!off.clips[0].notes.empty());
}

}  // namespace

TEST_CASE("note stage render (AP.7): the offline render equals the engine's events, config by config") {
  SUBCASE("Up, sixteenths") {
    Rig r;
    checkConfig(r);
  }
  SUBCASE("Random, a seed, gate 60 %, groove") {
    Rig r({0, 4, 7, 11});
    r.param("style", static_cast<double>(engine::ArpStyle::Random));
    r.param("gate", 0.6);
    r.param("groove", 0.4);
    r.setting("seed", 5.0);
    checkConfig(r);
  }
  SUBCASE("Harmonic Walk over Harmonics, 1/8 triplets, two copies by Ratio 3/2 layers") {
    Rig r({0, 7});
    r.param("style", static_cast<double>(engine::ArpStyle::HarmonicWalk));
    r.param("unit", static_cast<double>(engine::ArpUnit::Harmonics));
    r.param("walkRange", 4.0);
    r.param("rateDen", 8.0);
    r.param("tupletN", 3.0);
    r.param("tupletM", 2.0);
    r.param("layers", 1.0);
    r.param("ratioP", 3.0);
    r.param("ratioQ", 2.0);
    checkConfig(r);
  }
  SUBCASE("Pulse 8:9:10:12 (Proportion), Steps 2 by a Ratio") {
    Rig r({0, 4, 7, 10});
    r.param("pulse", 1.0);
    r.param("pulseSource", static_cast<double>(engine::ArpPulseSource::Proportion));
    r.param("proportionLen", 4.0);
    r.param("proportion1", 8.0);
    r.param("proportion2", 9.0);
    r.param("proportion3", 10.0);
    r.param("proportion4", 12.0);
    r.param("rateDen", 4.0);
    r.param("unit", static_cast<double>(engine::ArpUnit::Ratio));
    r.param("steps", 2.0);
    checkConfig(r);
  }
  SUBCASE("glides: Into from the previous step (S-curve), Out to an offset, legato gate") {
    Rig r;
    r.param("intoSource", static_cast<double>(engine::ArpGlide::Step));
    r.param("outTarget", static_cast<double>(engine::ArpGlide::Offset));
    r.param("outOffset", 2.0);
    r.param("glideMs", 30.0);
    r.param("glideShape", static_cast<double>(engine::RampShape::SCurve));
    r.param("gate", 1.3);
    checkConfig(r);
  }
  SUBCASE("lanes: Chance 0.5 and a Tie (gliding), Transpose, Velocity") {
    Rig r;
    r.setting("chanceLane", 1.0);
    r.setting("chanceLen", 3.0);
    r.param("chanceStep1", 0.5);
    r.param("chanceStep2", 0.5);
    r.setting("tieLane", 1.0);
    r.setting("tieLen", 4.0);
    r.param("tieStep3", 2.0);  // Tie
    r.param("intoSource", static_cast<double>(engine::ArpGlide::Step));
    r.setting("transLane", 1.0);
    r.setting("transLen", 5.0);
    r.param("transStep4", 12.0);
    r.setting("velLane", 1.0);
    r.setting("velLen", 2.0);
    r.param("velStep2", 0.5);
    r.setting("seed", 11.0);
    checkConfig(r);
  }
}

// ---- Flatten ----------------------------------------------------------------------------------------------------

namespace {

// Note-ons and offs (in order) of a render: what a flatten must keep.
struct NoteRec {
  SampleTime on, off;
  float hz, velocity;
  float degree, equave, ratio;
};
std::vector<NoteRec> notesOf(const std::vector<Ev>& evs, bool stageIds) {
  std::vector<NoteRec> out;
  std::vector<std::pair<uint32_t, size_t>> open;
  for (const Ev& e : evs) {
    if (engine::isStageNoteId(e.e.noteId) != stageIds) continue;
    if (e.e.kind == engine::NoteEventKind::NoteOn) {
      open.emplace_back(e.e.noteId, out.size());
      out.push_back({e.song, -1, e.e.hz, e.e.velocity, e.e.degree, e.e.equave, e.e.ratio});
    } else if (e.e.kind == engine::NoteEventKind::NoteOff) {
      const auto it = std::find_if(open.begin(), open.end(), [&](const auto& o) { return o.first == e.e.noteId; });
      if (it == open.end()) continue;
      out[it->second].off = e.song;
      open.erase(it);
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const NoteRec& a, const NoteRec& b) {
    return a.on != b.on ? a.on < b.on : a.hz < b.hz;
  });
  return out;
}

void flattenRoundTrip(Rig& r, bool degreesExact) {
  const std::vector<Ev> before = engineEvents(r, 4.5);
  const app::TrackNoteRender off = offline(r);
  const Project& p0 = r.doc();
  const Snapshot snap = r.store.current();
  const ops::Edit e = r.edit(ops::flattenNoteStages(p0, r.store.ids(), r.track, off.clips));
  CHECK(off.inexact == 0);
  CHECK(!findDevice(r.doc(), r.arp).device->active);
  const std::vector<Ev> after = engineEvents(r, 4.5);
  const std::vector<NoteRec> a = notesOf(before, true), b = notesOf(after, false);
  REQUIRE(a.size() == b.size());
  REQUIRE(!a.empty());
  for (size_t i = 0; i < a.size(); ++i) {
    CAPTURE(i);
    CHECK(a[i].on == b[i].on);
    CHECK(a[i].off == b[i].off);
    CHECK(std::fabs(a[i].hz - b[i].hz) <= 2e-6f * a[i].hz);
    CHECK(std::fabs(a[i].velocity - b[i].velocity) < 1e-5f);
    if (degreesExact) {  // the Degree identities, exactly
      CHECK(same(a[i].degree, b[i].degree));
      CHECK(same(a[i].equave, b[i].equave));
    }
    // voice.ratio: a Ratio identity's ratio to its anchor (the arp's ratio of a shifted Degree note is its interval,
    // a flattened Degree note's is 1: a Degree identity has no anchor).
    if (!degreesExact) CHECK(std::fabs(a[i].ratio - b[i].ratio) <= 1e-6f * a[i].ratio);
  }
  // One undo: the notes and the device back.
  REQUIRE(r.store.undo());
  CHECK(findDevice(r.doc(), r.arp).device->active);
  const NoteClip* c0 = nullptr;
  const NoteClip* c1 = nullptr;
  for (const ClipSlot& s : playedClips(*findTrackPtr(*snap, r.track))) c0 = s.clip.get();
  for (const ClipSlot& s : playedClips(*findTrackPtr(r.doc(), r.track))) c1 = s.clip.get();
  REQUIRE(c0 != nullptr);
  REQUIRE(c1 != nullptr);
  CHECK(c0->notes == c1->notes);
  (void)e;
}

}  // namespace

TEST_CASE("note stage render (AP.7): Flatten to clip plays the same notes with the device off; one undo restores") {
  SUBCASE("Degrees: Up over two copies a fifth apart (Degree identities)") {
    Rig r;
    r.param("steps", 2.0);
    r.param("distance", 7.0);
    r.param("gate", 0.8);
    flattenRoundTrip(r, true);
  }
  SUBCASE("Harmonics: Harmonic Walk (exact ratios anchored to the deactivated sources)") {
    Rig r({0, 7});
    r.param("style", static_cast<double>(engine::ArpStyle::HarmonicWalk));
    r.param("unit", static_cast<double>(engine::ArpUnit::Harmonics));
    r.param("walkRange", 3.0);
    r.param("rateDen", 8.0);
    flattenRoundTrip(r, false);
  }
  SUBCASE("lanes: Chance and Velocity") {
    Rig r;
    r.setting("chanceLane", 1.0);
    r.setting("chanceLen", 3.0);
    r.param("chanceStep2", 0.4);
    r.setting("velLane", 1.0);
    r.param("velStep1", 0.5);
    r.setting("seed", 3.0);
    flattenRoundTrip(r, true);
  }
}

TEST_CASE("note stage render (AP.7): flattened identities — Degree shifts, ratios to the source, curves from glides") {
  Rig r({0, 7});
  r.param("style", static_cast<double>(engine::ArpStyle::HarmonicWalk));
  r.param("unit", static_cast<double>(engine::ArpUnit::Harmonics));
  r.param("walkRange", 2.0);
  r.param("rateDen", 8.0);
  r.param("intoSource", static_cast<double>(engine::ArpGlide::Offset));
  r.param("intoOffset", -1.0);
  r.param("glideMs", 40.0);
  const app::TrackNoteRender off = offline(r);
  REQUIRE(off.clips.size() == 1);
  const NoteClip* src = playedClips(*findTrackPtr(r.doc(), r.track)).front().clip.get();
  bool ratio = false, curve = false;
  for (const GeneratedNote& g : off.clips[0].notes) {
    CHECK(g.exact);
    if (g.pitch.kind() == tuning::PitchKind::Ratio) {
      ratio = true;
      CHECK(g.pitch.anchorKind() == tuning::AnchorKind::Note);
      CHECK(g.pitch.anchorId() == g.source.value);  // a ratio of its own source note
      CHECK(src->notes.findById(g.source) != nullptr);
      const tuning::Rational q = g.pitch.ratio();
      CHECK(8 % q.den() == 0);  // h/8 (no group: Base 8), reduced
    }
    if (!g.curve.empty()) {
      curve = true;
      CHECK(g.curve.front().tick == 0);
      CHECK(std::fabs(g.curve.back().value) < 0.01f);  // it glides onto its own pitch
    }
  }
  CHECK(ratio);
  CHECK(curve);
}
