// Every command kind: its ChangeSet classification and touched ids, plus undo/redo restoring the identical
// snapshot pointers (T3.4 acceptance: "ChangeSet classification correct for 30+ command kinds").

#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <set>
#include <string>

#include "doc_fixture.h"

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
  c.push_back({"SetTrackMute", [](Fixture& f) -> Command { return SetTrackMute{f.kick, true}; }, P,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::track(ParamKind::TrackMute, f.kick)});
               }});
  c.push_back({"SetTrackSolo", [](Fixture& f) -> Command { return SetTrackSolo{f.drums, true}; }, P,
               [](Fixture& f, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::track(ParamKind::TrackSolo, f.drums)});
               }});
  c.push_back({"SetTrackArm", [](Fixture& f) -> Command { return SetTrackArm{f.bass, true}; }, M, noCheck});
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
  // ---- devices (T4.1): the fixture's tracks start with empty chains; withChain gives lead [instrument, utility].
  struct Chain {
    DeviceId inst, fx;
  };
  static Chain chain;
  auto withChain = [](Fixture& f) {
    chain.inst = f.store.ids().device();
    chain.fx = f.store.ids().device();
    REQUIRE(f.store.dispatch(Batch{"chain", {AddDevice{f.lead, makeDefaultInstrument(chain.inst), 0},
                                             AddDevice{f.lead, makeDevice(chain.fx, "utility", DeviceKind::AudioEffect), kAppend}}})
                .ok());
  };
  c.push_back({"AddDevice", [](Fixture& f) -> Command {
                 return AddDevice{f.lead, makeDefaultInstrument(f.store.ids().device()), 0};
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
                 return SetDeviceParam{chain.fx, "width", 1.5};
               }, P, [](Fixture&, const ChangeSet& cs) {
                 CHECK(cs.params == std::vector<ParamRef>{ParamRef::device(chain.fx, "width")});
                 CHECK(cs.devices == std::vector<DeviceId>{chain.fx});
               }});
  c.push_back({"SetDeviceSetting", [withChain](Fixture& f) -> Command {
                 withChain(f);
                 return SetDeviceSetting{chain.fx, "bassMono", 1.0};
               }, S, [](Fixture& f, const ChangeSet&) {
                 CHECK(f.track(f.lead).devices.at(1)->setting("bassMono")->value == 1.0);
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
  // Every command kind except none is covered.
  CHECK(kinds.size() == std::variant_size_v<CommandVariant>);
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
      RemoveTrack{f.master},
      MoveTrack{f.drums, f.kick, 0},                                  // into a MIDI track
      MoveTrack{f.drums, f.drums, 0},                                 // into itself
      AddTrack{makeTrackPtr(f.lead, TrackKind::Midi, "dup"), {}, 0},  // id in use
      AddTrack{makeTrackPtr(ids.track(), TrackKind::Audio, "a"), {}, 0},
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
  TuningDef priv;
  priv.system = pq::tuning::Edo::make(22).value();
  REQUIRE(f.store.dispatch(SetClipTuning{{f.lead, f.clipB}, TuningAssignment::privateTuning(priv)}).ok());
  const TuningDef* d = effectiveTuning(f.doc(), pathOf(f.lead), &f.clip(f.lead, f.clipB));
  REQUIRE(d);
  CHECK(d->edo()->n() == 22);
  CHECK(buildTuning(*d).ok());
}
