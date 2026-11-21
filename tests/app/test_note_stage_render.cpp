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
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"

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
    r.param("intoMs", 30.0);
    r.param("intoShape", static_cast<double>(engine::RampShape::SCurve));
    r.param("outMs", 20.0);  // AP.10: Out its own time and shape
    r.param("outShape", static_cast<double>(engine::RampShape::Overshoot));
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
  r.param("intoMs", 40.0);
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

// ---- AP.8b: automation, cables, the played span, muted clips, the real-renderer golden ------------------------------

namespace {

// A lane on the arp's param `name` ({tick, value in the param's units}: positions written as the lane holds them).
void automate(Rig& r, const char* name, std::vector<std::pair<Ticks, double>> points) {
  auto l = std::make_shared<AutomationLane>();
  l->id = r.store.ids().lane();
  l->param = ParamRef::device(r.arp, name);
  l->paramName = name;
  for (const auto& [t, v] : points) {
    Breakpoint b;
    b.id = r.store.ids().breakpoint();
    b.tick = t;
    b.value = static_cast<float>(position(name, v));
    l->points.push_back(b);
  }
  REQUIRE(r.store.dispatch(AddAutomationLane{r.track, l}).ok());
}

}  // namespace

TEST_CASE("note stage render (AP.8b, review H3): automated Gate and Rate — the render and Flatten follow them") {
  Rig r;
  automate(r, "gate", {{0, 0.3}, {8 * kQ, 0.95}});  // a ramp (below 1: a tail past the clip end is cut, Q?-AP.7-a (3))
  automate(r, "rateDen", {{0, 16.0}, {4 * kQ, 16.0}, {4 * kQ, 8.0}});  // sixteenths, then eighths (a jump at bar 2)
  const app::TrackNoteRender off = offline(r);
  CHECK(off.automated);
  checkEqual(engineEvents(r, 4.5), off.events, barsSamples(2.0));
  // The automation is what plays: an unautomated render differs (the jump halves the step count of bar 2).
  size_t bar1 = 0, bar2 = 0;
  for (const GeneratedNote& g : off.clips.at(0).notes) (g.start < 4 * kQ ? bar1 : bar2) += 1;
  CHECK(bar1 == 16);
  CHECK(bar2 == 8);
  flattenRoundTrip(r, true);
}

TEST_CASE("note stage render (AP.8b, review H3): a cable into a param is named (the preview leaves it out)") {
  Rig r;
  const DeviceId v = r.edit(ops::addDevice(r.doc(), r.store.ids(), r.track, std::string(kValueDevice),
                                           DeviceKind::AudioEffect, ops::kOffPath))
                         .devices.at(0);
  Wire w;
  w.from = v;
  w.port = std::string(kValueOutput);
  w.to = r.arp;
  w.param = "gate";
  (void)r.edit(ops::connect(r.doc(), r.store.ids(), w));
  const app::TrackNoteRender off = offline(r);
  CHECK(off.cable == "Arpeggiator Gate");
  // The views' source carries it to Flatten (which refuses: tests/views/test_note_preview.cpp, real dispatch).
  views::NotePreviewRequest q;
  q.until = kMaxTicks;
  q.flatten = true;
  q.config.sampleRate = kRate;
  auto pv = app::notePreviewSource()(r.doc(), r.track, q);
  REQUIRE(pv.ok());
  CHECK(pv->cable == "Arpeggiator Gate");
}

TEST_CASE("note stage render (AP.8b, review M5 / M6): Flatten keeps unplayed notes; a muted clip is flattened as if it played") {
  Rig r({7, 0, 4}, 4 * kQ);
  IdSource& ids = r.store.ids();
  // An unplayed note: past the clip's window (length 4 quarters).
  const Note outside = playback_fixture::note(ids, 6 * kQ, kQ, 11, 0, 100.0f);
  REQUIRE(r.store.dispatch(AddNotes{ClipRef{r.track, r.clip}, {outside}}).ok());
  // A muted clip after it (bar 3), its own chord.
  std::vector<Note> chord;
  for (const int32_t d : {2, 5, 9}) chord.push_back(playback_fixture::note(ids, 0, 4 * kQ, d, 0, 100.0f));
  ClipPtr c2 = playback_fixture::clip(ids, 8 * kQ, 4 * kQ, std::move(chord), "Muted");
  const ClipId muted = c2->id;
  REQUIRE(r.store.dispatch(AddClip{r.track, c2}).ok());
  REQUIRE(r.store.dispatch(SetClipActive{ClipRef{r.track, muted}, false}).ok());
  // What the muted clip plays with the arp when unmuted (the reference), then back.
  REQUIRE(r.store.dispatch(SetClipActive{ClipRef{r.track, muted}, true}).ok());
  const std::vector<NoteRec> want = notesOf(engineEvents(r, 6.5), true);
  REQUIRE(r.store.undo());
  // Flatten's render: every main-lane clip, the muted one as if active.
  app::NoteStageRenderOptions o;
  o.sampleRate = kRate;
  o.clips = app::NoteStageRenderOptions::Clips::All;
  auto all = app::renderTrackNoteStages(r.doc(), r.track, o);
  REQUIRE(all.ok());
  REQUIRE(all->clips.size() == 2);
  CHECK(all->clips[1].clip == muted);
  CHECK(!all->clips[1].notes.empty());
  (void)r.edit(ops::flattenNoteStages(r.doc(), ids, r.track, all->clips));
  const Track* t = findTrackPtr(r.doc(), r.track);
  const NoteClip& c1 = *t->clips.at(0).clip;
  CHECK(c1.notes.findById(outside.id) != nullptr);  // kept: it never played
  CHECK_FALSE(t->clips.at(1).clip->active);          // still muted
  // Unmuted, the flattened track plays what the arp played (the device off).
  REQUIRE(r.store.dispatch(SetClipActive{ClipRef{r.track, muted}, true}).ok());
  const std::vector<NoteRec> got = notesOf(engineEvents(r, 6.5), false);
  REQUIRE(want.size() == got.size());
  for (size_t i = 0; i < want.size(); ++i) {
    CAPTURE(i);
    CHECK(want[i].on == got[i].on);
    CHECK(want[i].off == got[i].off);
    CHECK(std::fabs(want[i].hz - got[i].hz) <= 2e-6f * want[i].hz);
  }
  // A take lane holding notes: refused (its notes would lose the devices; no note edit reaches a take lane).
  REQUIRE(r.store.undo());  // the unmute
  REQUIRE(r.store.undo());  // the flatten
  const std::vector<ClipSlot> take = {findTrackPtr(r.doc(), r.track)->clips.at(0)};
  (void)r.edit(ops::addTakeLane(r.doc(), ids, r.track, take, "Take 1"));
  CHECK(ops::flattenRefusal(r.doc(), r.track).find("take lane \"Take 1\"") != std::string::npos);
  CHECK_FALSE(ops::flattenNoteStages(r.doc(), ids, r.track, all->clips).ok());
}

TEST_CASE("note stage render (AP.8b, review L14): golden pianoroll-arp-preview — the ghosts from the real render") {
  const char* scene = "pianoroll-arp-preview";
  views::setSceneNotePreviewSource(app::notePreviewSource());
  uint32_t w = 0, h = 0;
  views::viewSceneDefaultSize(scene, w, h);
  ui::GpuOptions o;
  o.validation = true;
  auto img = views::renderViewScene(scene, std::string(PQ_TEST_DATA_DIR) + "/views/fixtures/empty", w, h, {}, o);
  views::setSceneNotePreviewSource({});
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
  auto ref = ui::readPng(path);
  if (!ref) (void)ui::writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ui::ImageDiff diff;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &diff));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(diff));
}
