// Every command kind: its ChangeSet classification and touched ids, plus undo/redo restoring the identical
// snapshot pointers (T3.4 acceptance: "ChangeSet classification correct for 30+ command kinds").

#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <set>
#include <optional>
#include <string>

#include "../meta/meta_test_type.h"
#include "doc_fixture.h"
#include "pq/document/meta.h"
#include "pq/document/pitch_resolve.h"
#include "pq/meta/envelope.h"

using namespace pq::doc;
using doctest_fixture::Fixture;
using doctest_fixture::makeClip;
using doctest_fixture::makeNote;
using doctest_fixture::makeTrackPtr;

namespace {

template <class T>
bool contains(const std::vector<T>& v, T x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

struct Case {
  std::string name;
  std::function<Command(Fixture&)> make;
  uint8_t flags;
  std::function<void(Fixture&, const ChangeSet&)> check;
};

TuningDef mosDef(double generator, int32_t n = 7) {
  TuningDef d;
  pq::tuning::MosParams m;
  m.generator = generator;
  m.n = n;
  d.system = m;
  return d;
}

std::vector<Case> cases() {
  constexpr uint8_t S = kChangeStructural, P = kChangeParam, Q = kChangeSequence, A = kChangeAutomation,
                    T = kChangeTimeline, M = kChangeMeta;
  auto noCheck = [](Fixture&, const ChangeSet&) {};
  std::vector<Case> c;
  // ---- tracks
  c.push_back({"AddTrack midi", [](Fixture& f) -> Command {
                 return AddTrack{makeTrackPtr(f.store.ids().track(), TrackKind::Midi, "New"), {}, 1};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.tracks.size() == 1);
                 CHECK(f.doc().tracks[1]->name == "New");
               }});
  c.push_back({"AddTrack return", [](Fixture& f) -> Command {
                 return AddTrack{makeTrackPtr(f.store.ids().track(), TrackKind::Return, "Delay"), {}, kAppend};
               }, S, [](Fixture& f, const ChangeSet&) { CHECK(f.doc().returns.size() == 2); }});
  c.push_back({"RemoveTrack (in group)", [](Fixture& f) -> Command { return RemoveTrack{f.kick}; }, S,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(contains(cs.tracks, f.kick));
                 CHECK(contains(cs.tracks, f.drums));
                 CHECK(contains(cs.clips, f.clipK));
                 CHECK(f.track(f.drums).children.empty());
               }});
  c.push_back({"RemoveTrack return (cleans sends)", [](Fixture& f) -> Command { return RemoveTrack{f.reverb}; }, S,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(contains(cs.tracks, f.reverb));
                 CHECK(contains(cs.tracks, f.lead));  // its send was removed
                 CHECK(f.track(f.lead).mixer.sends.empty());
               }});
  c.push_back({"MoveTrack into group", [](Fixture& f) -> Command { return MoveTrack{f.bass, f.drums, 0}; }, S,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(contains(cs.tracks, f.bass));
                 CHECK(contains(cs.tracks, f.drums));
                 CHECK(f.track(f.drums).children.front()->id == f.bass);
                 CHECK(f.doc().tracks.size() == 2);
               }});
  c.push_back({"RenameTrack", [](Fixture& f) -> Command { return RenameTrack{f.lead, "Solo"}; }, M,
               [](Fixture& f, const ChangeSet& cs) { CHECK(cs.tracks == std::vector<TrackId>{f.lead}); }});
  c.push_back({"SetTrackColour", [](Fixture& f) -> Command { return SetTrackColour{f.lead, Colour{0xFF0000}}; }, M,
               noCheck});
  c.push_back({"SetTrackVolume", [](Fixture& f) -> Command { return SetTrackVolume{f.lead, -3.0f}; }, P,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::track(ParamKind::TrackVolume, f.lead)});
                 CHECK(cs.paramOnly());
               }});
  c.push_back({"SetTrackPan", [](Fixture& f) -> Command { return SetTrackPan{f.lead, 0.5f}; }, P,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::track(ParamKind::TrackPan, f.lead)});
               }});
  c.push_back({"SetTrackPanMode", [](Fixture& f) -> Command { return SetTrackPanMode{f.drums, PanMode::Stereo}; }, S,
               [](Fixture& f, const ChangeSet& cs) {  // TX.pan-laws: structural, that track only
                 CHECK(cs.tracks == std::vector<TrackId>{f.drums});
                 CHECK(cs.params.empty());
                 CHECK(f.track(f.drums).mixer.panMode == PanMode::Stereo);
               }});
  c.push_back({"SetTrackMute", [](Fixture& f) -> Command { return SetTrackMute{f.kick, true}; }, P,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::track(ParamKind::TrackMute, f.kick)});
               }});
  c.push_back({"SetTrackSolo", [](Fixture& f) -> Command { return SetTrackSolo{f.drums, true}; }, P,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::track(ParamKind::TrackSolo, f.drums)});
               }});
  // T6.1: arm is a Param (the monitor gate follows it).
  c.push_back({"SetTrackArm", [](Fixture& f) -> Command { return SetTrackArm{f.bass, true}; }, P,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::track(ParamKind::TrackArm, f.bass)});
                 CHECK(f.track(f.bass).mixer.arm);
               }});
  // ---- T6.1: recording inputs, note input, punch, record settings, take lanes
  c.push_back({"SetTrackMonitor (Param)", [](Fixture& f) -> Command { return SetTrackMonitor{f.bass, MonitorMode::In}; }, P,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::track(ParamKind::TrackMonitor, f.bass)});
                 CHECK(f.track(f.bass).mixer.monitor == MonitorMode::In);
               }});
  static TrackId audioT;
  auto withAudio = [](Fixture& f) {
    audioT = f.store.ids().track();
    REQUIRE(f.store.dispatch(AddTrack{makeTrackPtr(audioT, TrackKind::Audio, "Audio"), {}, kAppend}).ok());
  };
  c.push_back({"SetTrackAudioInput (Structural: the patch reads track.in)", [withAudio](Fixture& f) -> Command {
                 withAudio(f);
                 return SetTrackAudioInput{audioT, AudioInput{2, 2}};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.tracks == std::vector<TrackId>{audioT});
                 CHECK(f.track(audioT).mixer.audioIn == AudioInput{2, 2});
               }});
  c.push_back({"SetTrackMidiInput (Meta)", [](Fixture& f) -> Command {
                 return SetTrackMidiInput{f.bass, MidiInput{MidiInputKind::Port, "Keystation", 3}};
               }, M, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.tracks == std::vector<TrackId>{f.bass});
                 CHECK(f.track(f.bass).mixer.midiIn.port == "Keystation");
               }});
  c.push_back({"SetTrackDelay (Meta)", [](Fixture& f) -> Command { return SetTrackDelay{f.bass, -12.5f}; }, M,
               [](Fixture& f, const ChangeSet&) { CHECK(f.track(f.bass).mixer.trackDelayMs == -12.5f); }});
  c.push_back({"SetNoteInput (Meta)", [](Fixture& f) -> Command {
                 return SetNoteInput{f.bass, NoteInput{NoteInputMode::Hz, InputKeyMap::TwelveTetHz}};
               }, M, [](Fixture& f, const ChangeSet&) {
                 CHECK(f.track(f.bass).noteInput == NoteInput{NoteInputMode::Hz, InputKeyMap::TwelveTetHz});
               }});
  c.push_back({"SetPunch (Meta)", [](Fixture&) -> Command {
                 return SetPunch{PunchRegion{4 * pq::kPpq, 8 * pq::kPpq, true}};
               }, M, [](Fixture& f, const ChangeSet&) { CHECK(f.doc().punch.enabled); }});
  c.push_back({"SetRecordSettings (Meta)", [](Fixture&) -> Command {
                 return SetRecordSettings{RecordSettings{2, true, -12.0f, true, pq::kPpq / 4}};
               }, M, [](Fixture& f, const ChangeSet&) { CHECK(f.doc().record.countInBars == 2); }});
  static LaneId takeL;
  static ClipId takeClip;
  auto takeLane = [](Fixture& f) {
    IdSource& ids = f.store.ids();
    auto l = std::make_shared<TakeLane>();
    l->id = takeL = ids.lane();
    l->name = "Take 1";
    ClipPtr c = makeClip(ids, 0, pq::kTicksPerWhole, {makeNote(ids, 0, 3), makeNote(ids, pq::kPpq, 5)}, "T");
    takeClip = c->id;
    l->clips.push_back(ClipSlot{c->id, c, nullptr});
    return TakeLanePtr(std::move(l));
  };
  auto withTake = [takeLane](Fixture& f) { REQUIRE(f.store.dispatch(AddTakeLane{f.lead, takeLane(f)}).ok()); };
  c.push_back({"AddTakeLane (Meta: an unheard take is never lowered)", [takeLane](Fixture& f) -> Command {
                 return AddTakeLane{f.lead, takeLane(f)};
               }, M, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.tracks == std::vector<TrackId>{f.lead});
                 CHECK(cs.clips == std::vector<ClipId>{takeClip});
                 REQUIRE(f.track(f.lead).takes.size() == 1);
                 CHECK(f.track(f.lead).takes[0]->id == takeL);
               }});
  c.push_back({"RenameTakeLane (Meta)", [withTake](Fixture& f) -> Command {
                 withTake(f);
                 return RenameTakeLane{f.lead, takeL, "Best"};
               }, M, [](Fixture& f, const ChangeSet&) { CHECK(f.track(f.lead).takes[0]->name == "Best"); }});
  c.push_back({"SetAuditionTake (Sequence: the take plays instead of the main lane)", [withTake](Fixture& f) -> Command {
                 withTake(f);
                 return SetAuditionTake{f.lead, takeL};
               }, Q, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.tracks == std::vector<TrackId>{f.lead});
                 CHECK(contains(cs.clips, takeClip));
                 CHECK(contains(cs.clips, f.clipA));
                 REQUIRE(playedClips(f.track(f.lead)).size() == 1);
                 CHECK(playedClips(f.track(f.lead))[0].id == takeClip);
               }});
  c.push_back({"RemoveTakeLane (Meta when not auditioned)", [withTake](Fixture& f) -> Command {
                 withTake(f);
                 return RemoveTakeLane{f.lead, takeL};
               }, M, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.clips == std::vector<ClipId>{takeClip});
                 CHECK(f.track(f.lead).takes.empty());
               }});
  c.push_back({"PromoteTake (Sequence: the take becomes the main lane)", [withTake](Fixture& f) -> Command {
                 withTake(f);
                 return PromoteTake{f.lead, takeL};
               }, Q, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(contains(cs.clips, takeClip));
                 const Track& t = f.track(f.lead);
                 REQUIRE(t.clips.size() == 1);
                 CHECK(t.clips[0].id == takeClip);
                 REQUIRE(t.takes.size() == 1);
                 CHECK(t.takes[0]->id == takeL);
                 CHECK(t.takes[0]->clips.size() == 2);  // A and B
               }});
  c.push_back({"SetSendLevel", [](Fixture& f) -> Command { return SetSendLevel{f.bass, f.reverb, -6.0f}; }, P,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::track(ParamKind::TrackSend, f.bass, f.reverb)});
                 CHECK(f.track(f.bass).mixer.sends.size() == 1);
               }});
  c.push_back({"SetSendPreFader", [](Fixture& f) -> Command { return SetSendPreFader{f.lead, f.reverb, true}; }, S,
               noCheck});
  c.push_back({"SetTrackOutput", [](Fixture& f) -> Command {
                 return SetTrackOutput{f.bass, Output{OutputKind::Track, f.drums}};
               }, S, noCheck});
  c.push_back({"SetTrackActive", [](Fixture& f) -> Command { return SetTrackActive{f.bass, false}; }, S, noCheck});
  c.push_back({"SetTrackFrozen", [](Fixture& f) -> Command { return SetTrackFrozen{f.drums, true}; }, S,  // FZ.1
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.tracks == std::vector<TrackId>{f.kick});  // the group's MIDI track, not the group
                 CHECK(f.track(f.kick).frozen);
                 CHECK_FALSE(f.track(f.drums).frozen);
               }});
  static TrackId emptyMidi;
  c.push_back({"SetTrackKind (FZ.4: an empty MIDI track becomes an audio track)", [](Fixture& f) -> Command {
                 emptyMidi = f.store.ids().track();
                 REQUIRE(f.store.dispatch(AddTrack{makeTrackPtr(emptyMidi, TrackKind::Midi, "E"), {}, kAppend}).ok());
                 return SetTrackKind{emptyMidi, TrackKind::Audio};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.tracks == std::vector<TrackId>{emptyMidi});
                 CHECK(f.track(emptyMidi).kind == TrackKind::Audio);
               }});
  c.push_back({"SetTrackTuning", [](Fixture& f) -> Command {
                 return SetTrackTuning{f.bass, TuningAssignment::mainTuning(f.tMos)};
               }, S, [](Fixture& f, const ChangeSet& cs) { CHECK(contains(cs.tunings, f.tMos)); }});
  c.push_back({"GroupTracks", [](Fixture& f) -> Command {
                 return GroupTracks{{f.bass, f.lead}, makeTrackPtr(f.store.ids().track(), TrackKind::Group, "G")};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(contains(cs.tracks, f.lead));
                 CHECK(contains(cs.tracks, f.bass));
                 REQUIRE(f.doc().tracks.size() == 2);
                 const Track& g = *f.doc().tracks[0];  // inserted where Lead was
                 CHECK(g.name == "G");
                 REQUIRE(g.children.size() == 2);
                 CHECK(g.children[0]->id == f.lead);  // tree order, not argument order
                 CHECK(g.children[1]->id == f.bass);
               }});
  c.push_back({"UngroupTracks", [](Fixture& f) -> Command { return UngroupTracks{f.drums}; }, S,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(contains(cs.tracks, f.drums));
                 CHECK(contains(cs.tracks, f.kick));
                 REQUIRE(f.doc().tracks.size() == 3);
                 CHECK(f.doc().tracks[1]->id == f.kick);
               }});
  // ---- clips
  c.push_back({"AddClip", [](Fixture& f) -> Command {
                 return AddClip{f.bass, makeClip(f.store.ids(), 0, pq::kTicksPerWhole, {})};
               }, Q, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.tracks == std::vector<TrackId>{f.bass});
                 CHECK(cs.clips.size() == 1);
               }});
  c.push_back({"RemoveClip", [](Fixture& f) -> Command { return RemoveClip{{f.lead, f.clipB}}; }, Q,
               [](Fixture& f, const ChangeSet& cs) { CHECK(cs.clips == std::vector<ClipId>{f.clipB}); }});
  c.push_back({"MoveClip (same track)", [](Fixture& f) -> Command {
                 return MoveClip{{f.lead, f.clipB}, f.lead, -pq::kTicksPerWhole};
               }, Q, [](Fixture& f, const ChangeSet&) {
                 CHECK(f.track(f.lead).clips.front().id == f.clipB);  // re-sorted by start
               }});
  c.push_back({"MoveClip (cross track)", [](Fixture& f) -> Command {
                 return MoveClip{{f.lead, f.clipA}, f.bass, 0};
               }, Q, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(contains(cs.tracks, f.lead));
                 CHECK(contains(cs.tracks, f.bass));
                 CHECK(f.track(f.bass).clips.size() == 1);
                 CHECK(f.track(f.lead).clips.size() == 1);
               }});
  c.push_back({"SetClipBounds", [](Fixture& f) -> Command {
                 return SetClipBounds{{f.lead, f.clipA}, pq::kPpq, 2 * pq::kTicksPerWhole, pq::kPpq};
               }, Q, noCheck});
  c.push_back({"SetClipLoop", [](Fixture& f) -> Command {
                 return SetClipLoop{{f.lead, f.clipA}, ClipLoop{true, 0, pq::kTicksPerWhole}};
               }, Q, noCheck});
  c.push_back({"SetClipTuning", [](Fixture& f) -> Command {
                 return SetClipTuning{{f.lead, f.clipB}, TuningAssignment::mainTuning(f.tMos)};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(contains(cs.tunings, f.tMos));
                 CHECK(cs.clips == std::vector<ClipId>{f.clipB});
               }});
  c.push_back({"RenameClip", [](Fixture& f) -> Command { return RenameClip{{f.lead, f.clipA}, "Intro"}; }, M,
               noCheck});
  c.push_back({"SetClipColour", [](Fixture& f) -> Command { return SetClipColour{{f.lead, f.clipA}, Colour{1}}; }, M,
               noCheck});
  c.push_back({"SetClipSignature", [](Fixture& f) -> Command {
                 return SetClipSignature{{f.lead, f.clipA}, pq::TimeSignature{7, 8}};
               }, M, noCheck});
  c.push_back({"SetClipActive", [](Fixture& f) -> Command { return SetClipActive{{f.lead, f.clipA}, false}; }, Q,
               noCheck});
  c.push_back({"SetClipFundamental", [](Fixture& f) -> Command {
                 ClipFundamental cf;
                 cf.lock = pq::tuning::Rational(1, 8);
                 return SetClipFundamental{{f.lead, f.clipA}, cf};
               }, M, [](Fixture& f, const ChangeSet&) {
                 CHECK(f.clip(f.lead, f.clipA).fundamental.lock == pq::tuning::Rational(1, 8));
               }});
  // ---- audio clips and samples (T5.3)
  auto sampleOf = [](Fixture& f) {
    SampleRef r;
    r.id = f.store.ids().sample();
    r.path = "C:/x.wav";
    r.frames = 48000;
    r.rate = 48000.0;
    r.channels = 2;
    return r;
  };
  c.push_back({"AddSample", [sampleOf](Fixture& f) -> Command { return AddSample{sampleOf(f)}; }, M,
               [](Fixture& f, const ChangeSet& cs) {
                 REQUIRE(cs.samples.size() == 1);
                 CHECK(findSample(f.doc(), cs.samples[0]));
               }});
  c.push_back({"RemoveSample", [sampleOf](Fixture& f) -> Command {
                 const SampleRef r = sampleOf(f);
                 REQUIRE(f.store.dispatch(AddSample{r}).ok());
                 return RemoveSample{r.id};
               }, M, [](Fixture& f, const ChangeSet& cs) {
                 REQUIRE(cs.samples.size() == 1);
                 CHECK(samplesOf(f.doc()).empty());
               }});
  c.push_back({"SetSample", [sampleOf](Fixture& f) -> Command {
                 SampleRef r = sampleOf(f);
                 REQUIRE(f.store.dispatch(AddSample{r}).ok());
                 r.path = "D:/moved.wav";
                 return SetSample{r};
               }, M, [](Fixture& f, const ChangeSet& cs) {
                 REQUIRE(cs.samples.size() == 1);
                 CHECK(findSample(f.doc(), cs.samples[0])->path == "D:/moved.wav");
               }});
  c.push_back({"SetAudioClip", [sampleOf](Fixture& f) -> Command {
                 IdSource& ids = f.store.ids();
                 const SampleRef r = sampleOf(f);
                 const TrackId t = ids.track();
                 AudioClip a;
                 a.id = ids.clip();
                 a.sample = r.id;
                 REQUIRE(f.store.dispatch(Batch{"setup", {AddSample{r}, AddTrack{makeTrackPtr(t, TrackKind::Audio, "A"), {}, kAppend},
                                                           AddClip{t, nullptr, std::make_shared<const AudioClip>(a)}}}).ok());
                 a.gainDb = -6.0f;
                 a.start = pq::kPpq;
                 return SetAudioClip{{t, a.id}, std::make_shared<const AudioClip>(a)};
               }, Q, [](Fixture& f, const ChangeSet& cs) {
                 REQUIRE(cs.clips.size() == 1);
                 const Track& t = *f.doc().tracks.back();
                 REQUIRE(t.clips.size() == 1);
                 CHECK(t.clips[0].audio->gainDb == -6.0f);
                 CHECK(t.clips[0].start() == pq::kPpq);
                 CHECK(t.clips[0].length() == pq::kPpq * 2);  // 1 s at 120 BPM (warp Off)
               }});
  // ---- notes
  c.push_back({"AddNotes", [](Fixture& f) -> Command {
                 return AddNotes{{f.lead, f.clipB}, {makeNote(f.store.ids(), 3 * pq::kPpq, 5)}};
               }, Q, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.notes.size() == 1);
                 CHECK(f.clip(f.lead, f.clipB).notes.size() == 4);
               }});
  c.push_back({"RemoveNotes", [](Fixture& f) -> Command {
                 return RemoveNotes{{f.lead, f.clipA}, {f.keyA(10), f.keyA(200)}};
               }, Q, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.notes.size() == 2);
                 CHECK(f.clip(f.lead, f.clipA).notes.size() == 298);
               }});
  c.push_back({"UpdateNotes", [](Fixture& f) -> Command {
                 Note n = f.notesA[5];
                 n.start += 7;
                 n.velocity = 20.0f;
                 return UpdateNotes{{f.lead, f.clipA}, {NoteUpdate{f.keyA(5), n}}};
               }, Q, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.notes == std::vector<NoteId>{f.notesA[5].id});
               }});
  c.push_back({"MoveNote", [](Fixture& f) -> Command {
                 return MoveNote{{f.lead, f.clipA}, f.keyA(150), 12345, pq::tuning::PitchId::fromDegree(3, 1)};
               }, Q, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.sequenceOnly());
                 CHECK(cs.tracks == std::vector<TrackId>{f.lead});
                 CHECK(cs.clips == std::vector<ClipId>{f.clipA});
                 CHECK(cs.notes == std::vector<NoteId>{f.notesA[150].id});
                 const Note* n = f.clip(f.lead, f.clipA).notes.find({12345, f.notesA[150].id});
                 REQUIRE(n);
                 CHECK(n->pitch == pq::tuning::PitchId::fromDegree(3, 1));
               }});
  c.push_back({"ResizeNote", [](Fixture& f) -> Command { return ResizeNote{{f.lead, f.clipA}, f.keyA(0), 99}; }, Q,
               noCheck});
  c.push_back({"SetNoteProperty velocity", [](Fixture& f) -> Command {
                 return SetNoteProperty{{f.lead, f.clipA}, {f.keyA(1), f.keyA(2)}, NoteProperty::Velocity, 64.0f};
               }, Q, [](Fixture&, const ChangeSet& cs) { CHECK(cs.notes.size() == 2); }});
  c.push_back({"SetNoteProperty chance", [](Fixture& f) -> Command {
                 return SetNoteProperty{{f.lead, f.clipA}, {f.keyA(1)}, NoteProperty::Chance, 0.25f};
               }, Q, noCheck});
  c.push_back({"SetNoteProperty enabled", [](Fixture& f) -> Command {
                 return SetNoteProperty{{f.lead, f.clipA}, {f.keyA(1)}, NoteProperty::Enabled, 0.0f};
               }, Q, [](Fixture& f, const ChangeSet&) {
                 CHECK_FALSE(f.clip(f.lead, f.clipA).notes.find(f.keyA(1))->enabled());
               }});
  // ---- automation
  c.push_back({"AddAutomationLane", [](Fixture& f) -> Command {
                 AutomationLane l;
                 l.id = f.store.ids().lane();
                 l.param = ParamRef::track(ParamKind::TrackPan, f.lead);
                 return AddAutomationLane{f.lead, std::make_shared<const AutomationLane>(l)};
               }, S, [](Fixture&, const ChangeSet& cs) { CHECK(cs.lanes.size() == 1); }});
  c.push_back({"RemoveAutomationLane", [](Fixture& f) -> Command {
                 return RemoveAutomationLane{f.lead, f.leadVolLane};
               }, S, noCheck});
  c.push_back({"AddBreakpoints", [](Fixture& f) -> Command {
                 return AddBreakpoints{f.lead, f.leadVolLane, {{pq::kPpq, -1.0f, 0.0f, f.store.ids().breakpoint()}}};
               }, A, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.automationOnly());
                 CHECK(cs.lanes == std::vector<LaneId>{f.leadVolLane});
                 CHECK(f.track(f.lead).lanes[0]->points.size() == 4);
                 CHECK(f.track(f.lead).lanes[0]->points[1].tick == pq::kPpq);  // sorted
               }});
  c.push_back({"RemoveBreakpoints", [](Fixture& f) -> Command {
                 return RemoveBreakpoints{f.lead, f.leadVolLane, {f.bp1}};
               }, A, noCheck});
  c.push_back({"UpdateBreakpoints", [](Fixture& f) -> Command {
                 return UpdateBreakpoints{f.lead, f.leadVolLane, {{5 * pq::kTicksPerWhole, 3.0f, -0.5f, f.bp0}}};
               }, A, [](Fixture& f, const ChangeSet&) {
                 CHECK(f.track(f.lead).lanes[0]->points.back().id == f.bp0);  // re-sorted
               }});
  // ---- timeline
  c.push_back({"SetTempoMap", [](Fixture&) -> Command {
                 return SetTempoMap{{{0, 120.0, pq::TempoCurve::Linear}, {pq::kTicksPerWhole, 90.0, pq::TempoCurve::Step}}};
               }, T, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.timelineOnly());
                 CHECK(f.doc().timeline->tempoMap.bpmAt(pq::kTicksPerWhole) == 90.0);
               }});
  c.push_back({"SetTimeSignatures", [](Fixture&) -> Command {
                 return SetTimeSignatures{{{0, {4, 4}}, {2, {7, 8}}}};
               }, T, [](Fixture& f, const ChangeSet&) {
                 CHECK(f.doc().timeline->sigMap.signatureAtBar(3) == pq::TimeSignature{7, 8});
               }});
  c.push_back({"SetLoop", [](Fixture&) -> Command { return SetLoop{{0, pq::kTicksPerWhole, true}}; }, T, noCheck});
  c.push_back({"AddLocator", [](Fixture& f) -> Command {
                 return AddLocator{Locator{f.store.ids().locator(), 0, "Intro"}};
               }, M, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.locators.size() == 1);
                 CHECK(f.doc().locators->front().name == "Intro");  // sorted by tick
               }});
  c.push_back({"RemoveLocator", [](Fixture& f) -> Command { return RemoveLocator{f.loc1}; }, M, noCheck});
  c.push_back({"UpdateLocator", [](Fixture& f) -> Command {
                 return UpdateLocator{Locator{f.loc1, 2 * pq::kTicksPerWhole, "Chorus"}};
               }, M, noCheck});
  // ---- tunings
  c.push_back({"AddTuning", [](Fixture& f) -> Command {
                 TuningSlot s{f.store.ids().tuning(), "22-EDO", {}, {}};
                 s.def.system = pq::tuning::Edo::make(22).value();
                 return AddTuning{s};
               }, S, [](Fixture& f, const ChangeSet&) { CHECK(f.doc().tunings->size() == 5); }});
  c.push_back({"RemoveTuning (drops its lanes)", [](Fixture& f) -> Command { return RemoveTuning{f.tMos}; }, S,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(contains(cs.lanes, f.bassGenLane));
                 CHECK(contains(cs.tracks, f.bass));
                 CHECK(f.track(f.bass).lanes.empty());
               }});
  c.push_back({"SetDefaultTuning (TU.3)", [](Fixture& f) -> Command { return SetDefaultTuning{f.tScale}; }, S,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(f.doc().defaultTuning == f.tScale);
                 CHECK(contains(cs.tunings, f.tScale));
                 CHECK(contains(cs.tunings, f.t12));
               }});
  c.push_back({"RenameTuning", [](Fixture& f) -> Command { return RenameTuning{f.t31, "31"}; }, M, noCheck});
  c.push_back({"SetTuningColour", [](Fixture& f) -> Command { return SetTuningColour{f.t31, Colour{2}}; }, M,
               noCheck});
  c.push_back({"SetTuningDef MOS generator (param)", [](Fixture& f) -> Command {
                 return SetTuningDef{f.tMos, mosDef(700.0)};
               }, P, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::tuning(ParamKind::MosGenerator, f.tMos)});
                 CHECK(cs.paramOnly());
               }});
  c.push_back({"SetTuningDef MOS size (structural)", [](Fixture& f) -> Command {
                 return SetTuningDef{f.tMos, mosDef(700.0, 5)};
               }, S, [](Fixture& f, const ChangeSet&) { CHECK(f.track(f.bass).lanes.size() == 1); }});
  c.push_back({"SetTuningDef EDO equave (param)", [](Fixture& f) -> Command {
                 TuningDef d;
                 d.system = pq::tuning::Edo::makeCents(31, 1201.0).value();
                 return SetTuningDef{f.t31, d};
               }, P, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::tuning(ParamKind::TuningEquave, f.t31)});
               }});
  c.push_back({"SetTuningDef kind change (structural, drops lanes)", [](Fixture& f) -> Command {
                 TuningDef d;
                 d.system = pq::tuning::Edo::make(19).value();
                 return SetTuningDef{f.tMos, d};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(contains(cs.lanes, f.bassGenLane));
                 CHECK(f.track(f.bass).lanes.empty());
               }});
  // Scale (T3.5): reference = Param; pitches / key mapping = discrete shape (Structural); description = Meta.
  c.push_back({"SetTuningDef scale reference (param)", [](Fixture& f) -> Command {
                 TuningDef d = findTuning(f.doc(), f.tScale)->def;
                 d.referenceHz = 300.0;
                 return SetTuningDef{f.tScale, d};
               }, P, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::tuning(ParamKind::TuningReference, f.tScale)});
                 CHECK(cs.paramOnly());
               }});
  c.push_back({"SetTuningDef scale pitches (structural)", [](Fixture& f) -> Command {
                 TuningDef d = findTuning(f.doc(), f.tScale)->def;
                 std::get<ScaleDef>(d.system).scale.pitches[0] = pq::tuning::ScalaPitch::fromRatio({10, 9});
                 return SetTuningDef{f.tScale, d};
               }, S, noCheck});
  c.push_back({"SetTuningDef scale size (structural)", [](Fixture& f) -> Command {
                 TuningDef d = findTuning(f.doc(), f.tScale)->def;
                 auto& p = std::get<ScaleDef>(d.system).scale.pitches;
                 p.erase(p.begin());
                 return SetTuningDef{f.tScale, d};
               }, S, noCheck});
  c.push_back({"SetTuningDef scale key mapping (structural)", [](Fixture& f) -> Command {
                 TuningDef d = findTuning(f.doc(), f.tScale)->def;
                 std::get<ScaleDef>(d.system).mapping.reset();
                 return SetTuningDef{f.tScale, d};
               }, S, noCheck});
  c.push_back({"SetTuningDef scale description (meta)", [](Fixture& f) -> Command {
                 TuningDef d = findTuning(f.doc(), f.tScale)->def;
                 std::get<ScaleDef>(d.system).scale.description = "renamed";
                 return SetTuningDef{f.tScale, d};
               }, M, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params.empty());
                 CHECK(cs.tunings == std::vector<TuningId>{f.tScale});
               }});
  c.push_back({"SetTuningDef scale description + reference (meta + param)", [](Fixture& f) -> Command {
                 TuningDef d = findTuning(f.doc(), f.tScale)->def;
                 std::get<ScaleDef>(d.system).scale.description = "renamed";
                 d.referenceHz = 300.0;
                 return SetTuningDef{f.tScale, d};
               }, static_cast<uint8_t>(M | P), noCheck});
  c.push_back({"SetTuningDef EDO -> scale (kind change, structural)", [](Fixture& f) -> Command {
                 return SetTuningDef{f.t31, doctest_fixture::fixtureScaleDef()};
               }, S, noCheck});
  c.push_back({"SetTuningParam scale reference", [](Fixture& f) -> Command {
                 return SetTuningParam{f.tScale, ParamKind::TuningReference, 330.0};
               }, P, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::tuning(ParamKind::TuningReference, f.tScale)});
               }});
  c.push_back({"SetTuningParam reference", [](Fixture& f) -> Command {
                 return SetTuningParam{f.t31, ParamKind::TuningReference, 440.0};
               }, P, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::tuning(ParamKind::TuningReference, f.t31)});
                 CHECK(findTuning(f.doc(), f.t31)->def.referenceHz == 440.0);
               }});
  c.push_back({"SetTuningParam MOS stretch", [](Fixture& f) -> Command {
                 return SetTuningParam{f.tMos, ParamKind::MosStretch, 3.0};
               }, P, noCheck});
  // ---- devices (T4.1): the fixture's tracks start with empty chains; withChain gives lead [an instrument group, a
  // native engine] (GU.11: blocks only; the engine is the test meta type: param gain, setting stages).
  struct Chain {
    DeviceId inst, fx;
  };
  static Chain chain;
  auto withChain = [](Fixture& f) {
    pq::meta::testing::registerTestType();
    chain.inst = f.store.ids().device();
    chain.fx = f.store.ids().device();
    REQUIRE(f.store.dispatch(Batch{"chain", {AddDevice{f.lead, makeInstrumentGroup(chain.inst), 0},
                                             AddDevice{f.lead, makeDevice(chain.fx, "meta:test_shape", DeviceKind::AudioEffect), kAppend}}})
                .ok());
  };
  c.push_back({"AddDevice", [](Fixture& f) -> Command {
                 return AddDevice{f.lead, makeInstrumentGroup(f.store.ids().device()), 0};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.tracks == std::vector<TrackId>{f.lead});
                 CHECK(cs.devices.size() == 1);
                 CHECK(f.track(f.lead).devices.size() == 1);
               }});
  c.push_back({"RemoveDevice", [withChain](Fixture& f) -> Command {
                 withChain(f);
                 return RemoveDevice{chain.inst};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.devices == std::vector<DeviceId>{chain.inst});
                 CHECK(f.track(f.lead).devices.size() == 1);
               }});
  c.push_back({"MoveDevice (to another track)", [withChain](Fixture& f) -> Command {
                 withChain(f);
                 return MoveDevice{chain.fx, f.bass, 0};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(contains(cs.tracks, f.lead));
                 CHECK(contains(cs.tracks, f.bass));
                 CHECK(f.track(f.bass).devices.at(0)->id == chain.fx);
               }});
  c.push_back({"SetDeviceActive", [withChain](Fixture& f) -> Command {
                 withChain(f);
                 return SetDeviceActive{chain.fx, false};
               }, S, [](Fixture& f, const ChangeSet&) { CHECK_FALSE(f.track(f.lead).devices.at(1)->active); }});
  c.push_back({"SetDeviceParam", [withChain](Fixture& f) -> Command {
                 withChain(f);
                 return SetDeviceParam{chain.fx, "gain", 0.375};  // GU.1: a knob position
               }, P, [](Fixture&, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::device(chain.fx, "gain")});
                 CHECK(cs.devices == std::vector<DeviceId>{chain.fx});
               }});
  c.push_back({"SetDeviceSetting", [withChain](Fixture& f) -> Command {
                 withChain(f);
                 return SetDeviceSetting{chain.fx, "stages", 3.0};
               }, S, [](Fixture& f, const ChangeSet&) {
                 CHECK(f.track(f.lead).devices.at(1)->setting("stages")->value == 3.0);
               }});
  c.push_back({"RenameDevice", [withChain](Fixture& f) -> Command {  // T4.17
                 withChain(f);
                 return RenameDevice{chain.fx, "Wide"};
               }, M, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.devices == std::vector<DeviceId>{chain.fx});
                 CHECK(f.track(f.lead).devices.at(1)->name == "Wide");
               }});
  // ---- modulation wires (T4.7): withWire adds chain.wire = inst.lfo → fx.gain.
  static WireId wire;
  auto withWire = [withChain](Fixture& f) {
    withChain(f);
    wire = f.store.ids().wire();
    REQUIRE(f.store.dispatch(AddWire{Wire{wire, chain.inst, "lfo", chain.fx, "gain", 0.25, WireReducer::Sum, {}, WireLaw::Travel}}).ok());
  };
  c.push_back({"AddWire", [withChain](Fixture& f) -> Command {
                 withChain(f);
                 wire = f.store.ids().wire();
                 return AddWire{Wire{wire, chain.inst, "lfo", chain.fx, "gain", -0.5, WireReducer::Last, {}, WireLaw::Travel}};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.wires == std::vector<WireId>{wire});
                 REQUIRE(findWire(f.doc(), wire));
                 CHECK(findWire(f.doc(), wire)->amount == -0.5);
               }});
  c.push_back({"RemoveWire", [withWire](Fixture& f) -> Command {
                 withWire(f);
                 return RemoveWire{wire};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.wires == std::vector<WireId>{wire});
                 CHECK(wiresOf(f.doc()).empty());
               }});
  c.push_back({"SetWireAmount", [withWire](Fixture& f) -> Command {
                 withWire(f);
                 return SetWireAmount{wire, -1.0};
               }, P, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::wireAmount(wire)});
                 CHECK(findWire(f.doc(), wire)->amount == -1.0);
               }});
  c.push_back({"SetWireReducer", [withWire](Fixture& f) -> Command {
                 withWire(f);
                 return SetWireReducer{wire, WireReducer::Highest};
               }, S, [](Fixture& f, const ChangeSet&) { CHECK(findWire(f.doc(), wire)->reducer == WireReducer::Highest); }});
  c.push_back({"SetWireSignal", [withWire](Fixture& f) -> Command {  // IG.1-2
                 withWire(f);
                 return SetWireSignal{wire, true, WireReducer::Last};
               }, S, [](Fixture& f, const ChangeSet&) {
                 CHECK(findWire(f.doc(), wire)->single);
                 CHECK(findWire(f.doc(), wire)->reducer == WireReducer::Last);
               }});
  c.push_back({"SetWireTap", [withChain](Fixture& f) -> Command {  // TL.1: a lane cable's tap (view only: Meta)
                 withChain(f);
                 wire = f.store.ids().wire();
                 Wire w;
                 w.id = wire;
                 w.noteSource = f.lead;
                 w.port = "hz";
                 w.to = chain.fx;
                 w.param = "gain";
                 REQUIRE(f.store.dispatch(AddWire{w}).ok());
                 return SetWireTap{wire, 96.0f};
               }, M, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.wires == std::vector<WireId>{wire});
                 CHECK(findWire(f.doc(), wire)->tap == std::optional<float>(96.0f));
               }});
  c.push_back({"RemoveDevice (takes its wires)", [withWire](Fixture& f) -> Command {
                 withWire(f);
                 return RemoveDevice{chain.fx};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.wires == std::vector<WireId>{wire});
                 CHECK(wiresOf(f.doc()).empty());
               }});
  c.push_back({"RemoveTrack (takes its devices' wires)", [withWire](Fixture& f) -> Command {
                 withWire(f);
                 return RemoveTrack{f.lead};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.wires == std::vector<WireId>{wire});
                 CHECK(wiresOf(f.doc()).empty());
               }});
  // ---- meta modules (TMM.3): withMeta adds a native engine mt.dev (the test type) holding its own state (path "",
  // with data) after chain.fx (one of the same type without a state). GU.11: a native engine keeps only its own state
  // (path ""), and nothing else holds meta-module state (no patch calls meta modules: a group's modules are blocks), so
  // the nested cases (a nested instance's params / settings, RenameMetaInstance) cannot be built any more.
  constexpr uint8_t D = kChangeMetaData;
  static struct {
    DeviceId dev;
  } mt;
  auto withMeta = [withChain](Fixture& f) {
    withChain(f);
    const pq::meta::Type& ts = pq::meta::testing::registerTestType();
    mt.dev = f.store.ids().device();
    auto dev = std::make_shared<Device>(*makeDevice(mt.dev, "meta:test_shape", DeviceKind::AudioEffect));
    dev->meta = std::make_shared<const MetaMap>(MetaMap{MetaEntry{"", defaultMetaInstance(ts)}});
    REQUIRE(f.store.dispatch(Batch{"meta", {AddDevice{f.lead, dev, kAppend}}}).ok());
  };
  auto metaAt = [](Fixture& f, DeviceId d, const char* path) { return findDevice(f.doc(), d).device->metaAt(path); };
  c.push_back({"AddMetaInstance (an engine's own state: data)", [withMeta](Fixture& f) -> Command {
                 withMeta(f);
                 return AddMetaInstance{chain.fx, "", defaultMetaInstance(pq::meta::testing::testType())};
               }, static_cast<uint8_t>(M | D), [metaAt](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.metaData == std::vector<MetaRef>{MetaRef{chain.fx, ""}});
                 CHECK(cs.devices == std::vector<DeviceId>{chain.fx});
                 REQUIRE(metaAt(f, chain.fx, "") != nullptr);
               }});
  c.push_back({"RemoveMetaInstance (an engine's own state)", [withMeta](Fixture& f) -> Command {
                 withMeta(f);
                 return RemoveMetaInstance{mt.dev, ""};
               }, static_cast<uint8_t>(M | D), [metaAt](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.metaData == std::vector<MetaRef>{MetaRef{mt.dev, ""}});
                 CHECK(metaAt(f, mt.dev, "") == nullptr);
               }});
  c.push_back({"SetMetaParam (a chain meta device's own)", [withMeta](Fixture& f) -> Command {
                 withMeta(f);
                 return SetMetaParam{mt.dev, "", "gain", 0.75};  // GU.1: a knob position
               }, P, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::device(mt.dev, "gain")});
                 CHECK(findDevice(f.doc(), mt.dev).device->param("gain")->value == 0.75);
               }});
  c.push_back({"SetMetaSetting (a chain meta device's own)", [withMeta](Fixture& f) -> Command {
                 withMeta(f);
                 return SetMetaSetting{mt.dev, "", "stages", 2.0};
               }, S, [](Fixture& f, const ChangeSet&) {
                 CHECK(findDevice(f.doc(), mt.dev).device->setting("stages")->value == 2.0);
               }});
  c.push_back({"SetMetaData", [withMeta](Fixture& f) -> Command {
                 withMeta(f);
                 return SetMetaData{mt.dev, "",
                                    std::make_shared<const pq::meta::DataValue>(pq::meta::DataValue::record(
                                        {{"values", pq::meta::DataValue::floatArray({1.0f, 2.0f})}}))};
               }, D, [metaAt](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.metaData == std::vector<MetaRef>{MetaRef{mt.dev, ""}});
                 CHECK(cs.devices == std::vector<DeviceId>{mt.dev});
                 CHECK(metaAt(f, mt.dev, "")->data->field("values")->floats.size() == 2);
               }});
  c.push_back({"SetParamCurve (GU.1: a control's curve; Structural when its class changes)", [withChain](Fixture& f) -> Command {
                 withChain(f);
                 return SetParamCurve{chain.fx, "gain", pq::dsp::curvePreset(pq::dsp::CurvePreset::Exponential, -70.0, 35.0), true};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.devices == std::vector<DeviceId>{chain.fx});
                 REQUIRE(findDevice(f.doc(), chain.fx).device->curve("gain") != nullptr);
                 CHECK(findDevice(f.doc(), chain.fx).device->curve("gain")->preset == pq::dsp::CurvePreset::Exponential);
               }});
  c.push_back({"SetParamCurve (GU.1: the same class: a Param change, no compile)", [withChain](Fixture& f) -> Command {
                 withChain(f);
                 return SetParamCurve{chain.fx, "gain", pq::dsp::curvePreset(pq::dsp::CurvePreset::Linear, -60.0, 30.0), false};
               }, P, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::device(chain.fx, "gain")});
                 REQUIRE(findDevice(f.doc(), chain.fx).device->curve("gain") != nullptr);
                 CHECK(findDevice(f.doc(), chain.fx).device->curve("gain")->min == -60.0);
               }});
  static DeviceId groupId;
  // GU.2: Structural (was Meta under TX.groups): since IG.1-2 membership decides whether a block runs per note (an
  // instrument group's member at any depth), and a nested group's variant.
  c.push_back({"SetDeviceGroup (TX.groups: a block into a group; GU.2: Structural, membership decides per note)", [withChain](Fixture& f) -> Command {
                 withChain(f);
                 groupId = f.store.ids().device();
                 REQUIRE(f.store.dispatch(Batch{"group", {AddDevice{f.lead, makeGroup(groupId), kAppend}}}).ok());
                 return SetDeviceGroup{chain.fx, groupId};
               }, S, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.devices == std::vector<DeviceId>{chain.fx});
                 CHECK(findDevice(f.doc(), chain.fx).device->group == groupId);
               }});
  // GU.3 (pq/document/panel.h): a group's panel, Meta (never lowered); its bindings inside the group.
  static DeviceId panelGroup;
  c.push_back({"SetGroupPanel (GU.3: a group's panel; Meta, a panel is never lowered)", [withChain](Fixture& f) -> Command {
                 withChain(f);
                 panelGroup = f.store.ids().device();
                 REQUIRE(f.store.dispatch(Batch{"group", {AddDevice{f.lead, makeGroup(panelGroup), kAppend}}}).ok());
                 REQUIRE(f.store.dispatch(SetDeviceGroup{chain.fx, panelGroup}).ok());
                 auto p = std::make_shared<Panel>();
                 p->width = 100.0f, p->height = 60.0f;
                 PanelElement e;
                 e.id = 1;
                 e.kind = "knob";
                 e.x = 4.0f, e.y = 4.0f, e.w = 40.0f, e.h = 50.0f;
                 e.bindings.push_back(controlBinding(chain.fx, "gain"));
                 p->elements.push_back(std::move(e));
                 return SetGroupPanel{panelGroup, std::move(p)};
               }, M, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.devices == std::vector<DeviceId>{panelGroup});
                 const Device* g = findDevice(f.doc(), panelGroup).device;
                 REQUIRE(g->panel != nullptr);
                 CHECK(g->panel->elements.size() == 1);
               }});
  // ---- compound
  c.push_back({"Batch (param + sequence)", [](Fixture& f) -> Command {
                 return Batch{"Mixed", {SetTrackVolume{f.lead, -1.0f},
                                        MoveNote{{f.lead, f.clipA}, f.keyA(3), 1, pq::tuning::PitchId::fromDegree(1)}}};
               }, static_cast<uint8_t>(P | Q), [](Fixture&, const ChangeSet& cs) {
                 CHECK_FALSE(cs.paramOnly());
                 CHECK_FALSE(cs.sequenceOnly());
                 CHECK_FALSE(cs.structural());
               }});
  c.push_back({"Batch (params only)", [](Fixture& f) -> Command {
                 return Batch{"Mix", {SetTrackVolume{f.lead, -1.0f}, SetTrackPan{f.bass, 0.2f},
                                      SetTrackVolume{f.lead, -2.0f}}};
               }, P, [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.paramOnly());
                 CHECK(cs.params.size() == 2);  // deduplicated
                 CHECK(f.track(f.lead).mixer.volumeDb == -2.0f);
               }});
  return c;
}

}  // namespace

TEST_CASE("commands: classification, touched ids and identical-pointer undo/redo for every kind") {
  std::set<size_t> kinds;
  for (const Case& tc : cases()) {
    CAPTURE(tc.name);
    Fixture f;
    const Command cmd = tc.make(f);
    kinds.insert(cmd.v.index());
    const Snapshot before = f.store.current();
    const size_t historyBefore = f.store.history().entries().size();
    const pq::Status st = f.store.dispatch(cmd);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    const Snapshot after = f.store.current();
    REQUIRE(after != before);
    const ChangeSet& cs = f.store.lastChanges();
    CHECK(static_cast<int>(cs.flags) == static_cast<int>(tc.flags));
    tc.check(f, cs);
    CHECK(f.store.history().entries().size() == historyBefore + 1);
    CHECK(f.store.history().undoLabel() == label(cmd));
    // The ChangeSet is normalized (sorted, unique).
    CHECK(std::is_sorted(cs.tracks.begin(), cs.tracks.end()));
    CHECK(std::adjacent_find(cs.tracks.begin(), cs.tracks.end()) == cs.tracks.end());

    REQUIRE(f.store.undo());
    CHECK(f.store.current() == before);  // identical pointer
    CHECK(static_cast<int>(f.store.lastChanges().flags) == static_cast<int>(tc.flags));
    REQUIRE(f.store.redo());
    CHECK(f.store.current() == after);
    // Re-applying the same command to the original snapshot is deterministic (deep-equal result).
    ChangeSet again;
    auto r = apply(before, cmd, again);
    REQUIRE(r.ok());
    CHECK(**r == *after);
  }
  // Every command kind is covered except RenameMetaInstance (GU.11: no block holds nested meta-module state, so no
  // rename can succeed; see the meta modules' cases).
  CHECK(kinds.size() == std::variant_size_v<CommandVariant> - 1);
  CHECK(kinds.count(CommandVariant{RenameMetaInstance{}}.index()) == 0);
}

template <size_t... I>
std::vector<std::string> allLabels(std::index_sequence<I...>) {
  return {std::string(label(Command{std::variant_alternative_t<I, CommandVariant>{}}))...};
}

TEST_CASE("commands: labels exist for every kind") {
  // One default-constructed command of each kind: every label is non-empty.
  for (const std::string& l : allLabels(std::make_index_sequence<std::variant_size_v<CommandVariant>>{}))
    CHECK_FALSE(l.empty());
}

TEST_CASE("commands: no-op commands keep the snapshot and record nothing") {
  Fixture f;
  const Snapshot s = f.store.current();
  const size_t h = f.store.history().entries().size();
  const std::vector<Command> noops = {
      SetTrackVolume{f.lead, 0.0f},
      SetTrackMute{f.lead, false},
      RenameTrack{f.lead, "Lead"},
      SetSendLevel{f.lead, f.reverb, -10.0f},
      SetSendLevel{f.bass, f.reverb, kMinVolumeDb},
      MoveTrack{f.lead, {}, 0},
      MoveNote{{f.lead, f.clipA}, f.keyA(4), f.notesA[4].start, f.notesA[4].pitch},
      SetNoteProperty{{f.lead, f.clipA}, {f.keyA(4)}, NoteProperty::Velocity, 100.0f},
      AddNotes{{f.lead, f.clipA}, {}},
      SetLoop{f.doc().loop},
      SetTuningParam{f.t31, ParamKind::TuningReference, pq::tuning::kDefaultReferenceHz},
      RenameClip{{f.lead, f.clipA}, "A"},
      Batch{"nothing", {SetTrackPan{f.lead, 0.0f}}},
      SetTrackPanMode{f.lead, PanMode::Balance},  // TX.pan-laws: the default
      SetTrackMonitor{f.lead, MonitorMode::Auto},  // T6.1: the defaults
      SetTrackMidiInput{f.lead, MidiInput{}},
      SetNoteInput{f.lead, NoteInput{}},
      SetAuditionTake{f.lead, LaneId{}},
      SetPunch{PunchRegion{}},
      SetRecordSettings{RecordSettings{}},
  };
  for (const Command& c : noops) {
    CAPTURE(std::string(label(c)));
    REQUIRE(f.store.dispatch(c).ok());
    CHECK(f.store.current() == s);
    CHECK(f.store.lastChanges().empty());
    CHECK(f.store.history().entries().size() == h);
  }
}

TEST_CASE("commands: invalid commands fail and leave the document untouched") {
  Fixture f;
  const Snapshot s = f.store.current();
  const size_t h = f.store.history().entries().size();
  IdSource& ids = f.store.ids();
  std::vector<Command> bad = {
      SetTrackVolume{f.lead, 12.0f},                                  // above +6 dB
      SetTrackVolume{TrackId{9999}, 0.0f},                            // unknown track
      SetTrackMute{f.master, true},                                   // master has no mute
      SetSendLevel{f.lead, f.lead, 0.0f},                             // send to a non-return
      SetTrackArm{f.drums, true},                                     // groups cannot be armed
      SetTrackMonitor{f.drums, MonitorMode::In},                      // T6.1: groups do not monitor
      SetTrackAudioInput{f.bass, AudioInput{0, 2}},                   //        a MIDI track has no audio input
      SetTrackMidiInput{f.bass, MidiInput{MidiInputKind::Port, "", 0}},  //     a port without a name
      SetTrackMidiInput{f.bass, MidiInput{MidiInputKind::All, "", 17}},  //     channel 17
      SetTrackDelay{f.bass, 2000.0f},
      SetNoteInput{f.drums, NoteInput{}},                             //        not a MIDI track
      SetPunch{PunchRegion{10, 10, true}},
      SetRecordSettings{RecordSettings{3, false, 0.0f, false, 0}},    //        count-in 3 bars
      RemoveTakeLane{f.lead, LaneId{4242}},
      SetAuditionTake{f.lead, LaneId{4242}},
      PromoteTake{f.lead, LaneId{4242}},
      AddTakeLane{f.lead, std::make_shared<const TakeLane>(TakeLane{LaneId{}, "no id", {}})},
      AddTakeLane{f.lead, std::make_shared<const TakeLane>(TakeLane{f.leadVolLane, "automation lane's id", {}})},
      AddTakeLane{f.lead, std::make_shared<const TakeLane>(TakeLane{ids.lane(), "clip id in use",
                                                                    {ClipSlot{f.clipA, f.track(f.lead).clips[0].clip, nullptr}}})},
      SetTrackPanMode{TrackId{9999}, PanMode::Linear},                // TX.pan-laws: unknown track
      SetTrackPanMode{f.lead, static_cast<PanMode>(9)},               //              unknown law
      RemoveTrack{f.master},
      MoveTrack{f.drums, f.kick, 0},                                  // into a MIDI track
      MoveTrack{f.drums, f.drums, 0},                                 // into itself
      AddTrack{makeTrackPtr(f.lead, TrackKind::Midi, "dup"), {}, 0},  // id in use
      AddClip{f.lead, makeClip(ids, 0, 10, {}), std::make_shared<const AudioClip>()},  // T5.3: both kinds at once
      AddTrack{makeTrackPtr(ids.track(), TrackKind::Master, "m"), {}, 0},
      AddTrack{makeTrackPtr(ids.track(), TrackKind::Return, "r"), f.drums, 0},
      AddClip{f.drums, makeClip(ids, 0, 10, {})},                     // clips only on MIDI tracks
      AddClip{f.lead, makeClip(ids, 0, 0, {})},                       // zero length
      MoveClip{{f.lead, f.clipA}, f.drums, 0},
      MoveNote{{f.lead, f.clipA}, {0, NoteId{424242}}, 0, {}},        // unknown note
      ResizeNote{{f.lead, f.clipA}, f.keyA(0), 0},
      SetNoteProperty{{f.lead, f.clipA}, {f.keyA(0)}, NoteProperty::Chance, 2.0f},
      AddNotes{{f.lead, f.clipA}, {f.notesA[0]}},                     // already present
      SetClipLoop{{f.lead, f.clipA}, ClipLoop{true, 10, 10}},
      SetClipSignature{{f.lead, f.clipA}, pq::TimeSignature{4, 512}},
      SetTempoMap{{{0, 0.5, pq::TempoCurve::Step}}},
      SetTimeSignatures{{{1, {4, 4}}}},                               // must start at bar 0
      SetLoop{{10, 10, true}},
      RemoveTuning{f.t31},                                            // used by Lead
      SetTuningParam{f.t31, ParamKind::MosGenerator, 700.0},          // not an EDO param
      SetTuningParam{f.tMos, ParamKind::MosPeriod, -5.0},
      SetTrackTuning{f.bass, TuningAssignment::mainTuning(TuningId{777})},
      SetTrackOutput{f.drums, Output{OutputKind::Track, f.drums}},
      UpdateBreakpoints{f.lead, f.leadVolLane, {{0, 0.0f, 0.0f, BreakpointId{5555}}}},
      AddBreakpoints{f.lead, f.leadVolLane, {{0, 100.0f, 0.0f, ids.breakpoint()}}},  // above the volume range
      Batch{"half bad", {SetTrackVolume{f.lead, -1.0f}, SetTrackVolume{f.lead, 99.0f}}},
  };
  {
    AutomationLane l;  // a second lane for the same parameter
    l.id = ids.lane();
    l.param = ParamRef::track(ParamKind::TrackVolume, f.lead);
    bad.push_back(AddAutomationLane{f.lead, std::make_shared<const AutomationLane>(l)});
    l.param = ParamRef::track(ParamKind::TrackPan, f.lead);  // a track param on another track
    bad.push_back(AddAutomationLane{f.bass, std::make_shared<const AutomationLane>(l)});
    l.param = ParamRef::track(ParamKind::TrackSolo, f.lead);  // not automatable
    bad.push_back(AddAutomationLane{f.lead, std::make_shared<const AutomationLane>(l)});
  }
  {
    // Scale definitions: not an EDO (no equave param), invalid UTF-8 description, no pitches, unmapped .kbm
    // reference key.
    bad.push_back(SetTuningParam{f.tScale, ParamKind::TuningEquave, 1201.0});
    const TuningDef good = findTuning(f.doc(), f.tScale)->def;
    TuningDef d = good;
    std::get<ScaleDef>(d.system).scale.description = "caf\xE9";  // Latin-1, not UTF-8
    bad.push_back(SetTuningDef{f.tScale, d});
    d = good;
    std::get<ScaleDef>(d.system).scale.pitches.clear();
    bad.push_back(SetTuningDef{f.tScale, d});
    d = good;
    std::get<ScaleDef>(d.system).mapping->mapping[1] = pq::tuning::kUnmappedKey;  // the entry key 69 uses
    bad.push_back(SetTuningDef{f.tScale, d});
  }
  for (const Command& c : bad) {
    CAPTURE(std::string(label(c)));
    CHECK_FALSE(f.store.dispatch(c).ok());
    CHECK(f.store.current() == s);
    CHECK(f.store.history().entries().size() == h);
  }
  // A failed batch step names the step.
  const pq::Status st = f.store.dispatch(Batch{"half bad", {SetTrackVolume{f.lead, -1.0f}, SetTrackVolume{f.lead, 99.0f}}});
  REQUIRE_FALSE(st.ok());
  CHECK(st.error().message.find("step 2") != std::string::npos);
}

TEST_CASE("commands: routing cycles are rejected") {
  Fixture f;
  IdSource& ids = f.store.ids();
  const TrackId outer = ids.track();
  REQUIRE(f.store.dispatch(AddTrack{makeTrackPtr(outer, TrackKind::Group, "Outer"), {}, kAppend}).ok());
  REQUIRE(f.store.dispatch(MoveTrack{f.drums, outer, 0}).ok());  // Drums inside Outer: Drums → Outer
  CHECK_FALSE(f.store.dispatch(SetTrackOutput{outer, Output{OutputKind::Track, f.drums}}).ok());
  // An explicit route to an unrelated group is fine, and moving that group inside the router then cycles.
  const TrackId side = ids.track();
  REQUIRE(f.store.dispatch(AddTrack{makeTrackPtr(side, TrackKind::Group, "Side"), {}, kAppend}).ok());
  REQUIRE(f.store.dispatch(SetTrackOutput{outer, Output{OutputKind::Track, side}}).ok());
  CHECK_FALSE(f.store.dispatch(MoveTrack{side, f.drums, 0}).ok());
}

TEST_CASE("commands: grouping nests, ungrouping restores order, depth is bounded") {
  Fixture f;
  IdSource& ids = f.store.ids();
  // Group Kick (inside Drums) with Bass (top level): the new group goes where Kick was (inside Drums).
  const TrackId g = ids.track();
  REQUIRE(f.store.dispatch(GroupTracks{{f.kick, f.bass}, makeTrackPtr(g, TrackKind::Group, "G")}).ok());
  const auto pg = findTrack(f.doc(), g);
  REQUIRE(pg);
  CHECK(pg->depth == 2);
  CHECK(parentOf(f.doc(), *pg)->id == f.drums);
  CHECK(f.doc().tracks.size() == 2);  // Lead, Drums
  REQUIRE(f.store.dispatch(UngroupTracks{g}).ok());
  CHECK(f.track(f.drums).children.size() == 2);
  CHECK(f.track(f.drums).children[0]->id == f.kick);
  CHECK(f.track(f.drums).children[1]->id == f.bass);
  // Selecting a group together with its own child is rejected.
  CHECK_FALSE(f.store.dispatch(GroupTracks{{f.drums, f.kick}, makeTrackPtr(ids.track(), TrackKind::Group, "X")}).ok());

  // Nest groups down to the depth limit.
  TrackId parent = f.drums;  // depth 1
  for (uint32_t d = 2; d <= kMaxTrackDepth; ++d) {
    const TrackId id = ids.track();
    REQUIRE(f.store.dispatch(AddTrack{makeTrackPtr(id, TrackKind::Group, "n"), parent, 0}).ok());
    parent = id;
  }
  CHECK_FALSE(f.store.dispatch(AddTrack{makeTrackPtr(ids.track(), TrackKind::Midi, "deep"), parent, 0}).ok());
}

TEST_CASE("commands: effective tuning follows clip > track > group > project default") {
  Fixture f;
  const Project& p = f.doc();
  auto pathOf = [&](TrackId id) { return *findTrack(f.doc(), id); };
  CHECK(effectiveTuning(p, pathOf(f.lead)) == &findTuning(p, f.t31)->def);
  CHECK(effectiveTuning(p, pathOf(f.kick)) == &findTuning(p, f.t12)->def);  // Inherit → default
  REQUIRE(f.store.dispatch(SetTrackTuning{f.drums, TuningAssignment::mainTuning(f.tMos)}).ok());
  CHECK(effectiveTuning(f.doc(), pathOf(f.kick)) == &findTuning(f.doc(), f.tMos)->def);  // via the group
  TuningDef edo22;
  edo22.system = pq::tuning::Edo::make(22).value();
  const TuningId t22 = f.store.ids().tuning();
  REQUIRE(f.store.dispatch(AddTuning{TuningSlot{t22, "Tuning E", {}, edo22}}).ok());
  REQUIRE(f.store.dispatch(SetClipTuning{{f.lead, f.clipB}, TuningAssignment::mainTuning(t22)}).ok());
  const TuningDef* d = effectiveTuning(f.doc(), pathOf(f.lead), &f.clip(f.lead, f.clipB));
  REQUIRE(d);
  CHECK(d->edo()->n() == 22);
  CHECK(buildTuning(*d).ok());
  // TU.3: the project default is explicit; a top-level Inherit follows it, and it cannot be removed while default.
  REQUIRE(f.store.dispatch(SetTrackTuning{f.drums, TuningAssignment::inherit()}).ok());
  REQUIRE(f.store.dispatch(SetDefaultTuning{t22}).ok());
  CHECK(effectiveTuning(f.doc(), pathOf(f.kick)) == &findTuning(f.doc(), t22)->def);
  CHECK(effectiveTuningRef(f.doc(), pathOf(f.kick)).main == t22);
  CHECK_FALSE(f.store.dispatch(RemoveTuning{t22}).ok());
  CHECK_FALSE(f.store.dispatch(SetDefaultTuning{TuningId{999999}}).ok());
}
