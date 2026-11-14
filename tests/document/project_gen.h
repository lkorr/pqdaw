#pragma once

// Projects for the serialization tests (T3.5), built through commands so they satisfy the model's invariants:
//   richProject: the doc_fixture project plus one of every entity kind, every PitchId kind and anchor kind, every
//                tuning kind (a MOS with MODMOS/generator sequence/region, a Scala scale
//                with a ratio beyond int64 and a .kbm), every ParamKind lane, nested groups, every output kind,
//                non-default note fields and number edge cases (-0.0, denormals, extreme exponents), view state, and
//                every MIDI track's instrument (a Wavetable engine in an instrument group).
//   randomProject: richProject plus seeded random notes, breakpoints, tempo and mixer values.

#include <bit>
#include <cfloat>
#include <cmath>
#include <limits>
#include <random>

#include "doc_fixture.h"
#include "pq/document/edit_ops.h"
#include "../support/blocks.h"

namespace doctest_fixture {

inline const DeviceLibrary& deviceLibrary() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// A Wavetable engine Ctrl+G'd into an instrument group at a MIDI track's notes (test_blocks::addEngineInstrument), as a
// block set: the group, the engine and the wires inside the group.
inline const BlockSet& engineInstrument() {
  static const BlockSet set = [] {
    DocumentStore s;
    auto t = ops::createTrack(*s.current(), s.ids(), TrackKind::Midi);
    REQUIRE(t.ok());
    REQUIRE(s.dispatch(t->command).ok());
    const DeviceId g = test_blocks::addEngineInstrument(s, t->tracks.at(0), "wavetable", deviceLibrary(), true);
    auto copied = ops::copyBlocks(*s.current(), std::vector<DeviceId>{g});
    REQUIRE(copied.ok());
    return std::move(*copied);
  }();
  return set;
}

struct RichProject : Fixture {
  TrackId inner, innerMidi;
  ClipId clipR;
  LaneId lanes[8];

  RichProject() {
    IdSource& ids = store.ids();
    inner = ids.track();
    innerMidi = ids.track();
    Batch b{"rich", {}};
    // Nested groups (drums > inner > innerMidi), every output kind, sends, mixer flags.
    b.commands.push_back(AddTrack{makeTrackPtr(inner, TrackKind::Group, "Inner \"quoted\" \xE2\x98\x83"), drums, kAppend});
    b.commands.push_back(AddTrack{makeTrackPtr(innerMidi, TrackKind::Midi, "Tab\there\nnewline \\ \x01"), inner, kAppend});
    b.commands.push_back(SetTrackOutput{bass, Output{OutputKind::Track, drums}});
    b.commands.push_back(SetTrackOutput{reverb, Output{OutputKind::None, {}}});
    b.commands.push_back(SetTrackOutput{kick, Output{OutputKind::Master, {}}});
    b.commands.push_back(SetSendPreFader{lead, reverb, true});
    b.commands.push_back(SetSendPreFader{bass, reverb, true});  // a send at -70 dB, pre-fader (non-default)
    b.commands.push_back(SetTrackVolume{lead, -70.0f});
    b.commands.push_back(SetTrackVolume{master, 6.0f});
    b.commands.push_back(SetTrackPan{bass, -0.0f});
    b.commands.push_back(SetTrackPan{kick, 0.1f});
    b.commands.push_back(SetTrackMute{lead, true});
    b.commands.push_back(SetTrackSolo{bass, true});
    b.commands.push_back(SetTrackArm{lead, true});
    b.commands.push_back(SetTrackActive{kick, false});
    b.commands.push_back(SetTrackColour{lead, Colour{0xFF00A0}});
    // Tunings: main Scala on the group, an EDO (cents equave) on a track, a MOS on a clip (TU.3: named tunings).
    b.commands.push_back(SetTrackTuning{drums, TuningAssignment::mainTuning(tScale)});
    TuningDef stretched;
    stretched.system = pq::tuning::Edo::makeCents(19, 1200.0 / 3.0 * 3.0000001).value();
    stretched.referenceHz = 432.1;
    const TuningId tStretched = ids.tuning();
    b.commands.push_back(AddTuning{TuningSlot{tStretched, "Tuning E", Colour{0x40A080}, stretched}});
    b.commands.push_back(SetTrackTuning{innerMidi, TuningAssignment::mainTuning(tStretched)});
    pq::tuning::MosParams mos;
    mos.generator = 1.0 / 3.0 * 2100.0;
    mos.n = 7;
    mos.rotation = 3;
    mos.modmos = {0.0, -0.0, 1.0, 0.5, 5e-324};
    mos.genSequence = {700.0, 690.5};
    mos.octaveStretch = -0.5;
    mos.region = pq::tuning::RegionHint{7, 685.7142857142857, 720.0};
    TuningDef priv;
    priv.system = mos;
    priv.referenceHz = 261.6255653005986;
    const TuningId tPriv = ids.tuning();
    b.commands.push_back(AddTuning{TuningSlot{tPriv, "Tuning F", Colour{0xA04080}, priv}});
    b.commands.push_back(SetClipTuning{{kick, clipK}, TuningAssignment::mainTuning(tPriv)});
    b.commands.push_back(SetClipTuning{{lead, clipB}, TuningAssignment::mainTuning(tMos)});
    TuningDef equave3;
    equave3.system = pq::tuning::Edo::make(13, pq::tuning::Rational{3}).value();
    b.commands.push_back(SetTuningDef{t31, equave3});  // EDO with a ratio equave other than 2/1
    // Clip header fields.
    b.commands.push_back(SetClipLoop{{lead, clipA}, ClipLoop{true, pq::kPpq, 3 * pq::kPpq}});
    b.commands.push_back(SetClipSignature{{lead, clipA}, pq::TimeSignature{5, 4}});
    b.commands.push_back(SetClipActive{{lead, clipB}, false});
    b.commands.push_back(SetClipColour{{lead, clipB}, Colour{0x123456}});
    b.commands.push_back(RenameClip{{lead, clipB}, "B \xC3\xA9t\xC3\xA9"});
    b.commands.push_back(SetClipBounds{{lead, clipB}, 8 * pq::kTicksPerWhole - 7, pq::kTicksPerWhole + 7, -7});
    // Notes of every pitch kind and edge-case numbers.
    std::vector<Note> notes;
    auto note = [&](pq::Ticks start, pq::tuning::PitchId p) -> Note& {
      Note n;
      n.id = ids.note();
      n.start = start;
      n.length = 1;
      n.pitch = p;
      notes.push_back(n);
      return notes.back();
    };
    using pq::tuning::PitchId;
    using pq::tuning::Rational;
    using pq::tuning::AnchorKind;
    const NoteId anchor = notesA[7].id;
    // T7.2: two JI objects in clip B with every stop kind and field set; the JI-following notes below name them (a
    // note must follow an object of its clip, so the old dangling ids 42 / 7 / 0xFFFFFFFF are gone).
    JiObject jiA;
    jiA.id = ids.jiObject();
    jiA.start = -3;
    jiA.length = 2 * pq::kPpq;
    jiA.fundamental = PitchId::fromRatio(Rational{3, 2});
    jiA.velocity = 64.5f;
    for (int i = 0; i < 4; ++i) jiA.voiceNotes.push_back(ids.note());
    {
      JiStop s0;
      s0.terms = {4, 5, 6, 7};
      JiStop s1;
      s1.at = pq::kPpq / 2;
      s1.terms = {22, 26, 30};
      s1.scale = Rational{2, 11};
      s1.voiceTerm = {0, 0, 1, 2};
      s1.timing = JiTiming{3, 100, JiShape{JiShape::Bezier, 0.25f, -0.5f, 0.75f, 1.5f}};
      s1.voiceTiming = {std::nullopt, JiTiming{0, 5, JiShape{JiShape::LogLinear}}, std::nullopt, JiTiming{}};
      JiStop s2;  // a tuning stop
      s2.at = pq::kPpq;
      s2.pitches = {PitchId::fromDegree(3, 0), PitchId::fromCents(386.3137138648348), PitchId::fromRatio(Rational{7, 4})};
      s2.timing.glide = 10;
      jiA.stops = {s0, s1, s2};
    }
    jiA.ride = {{0, 0.0f, 0.5f, {}}, {100, 12.5f, 0.0f, {}}, {100, -3.0f, -1.0f, {}}};
    JiObject jiB;  // an undertone scaffold on a Degree fundamental
    jiB.id = ids.jiObject();
    jiB.start = kMaxTicks - 1;
    jiB.length = 1;
    jiB.fundamental = PitchId::fromDegree(-2, 1);
    jiB.undertone = true;
    jiB.sounds = false;
    jiB.voiceNotes.push_back(ids.note());
    jiB.stops.resize(1);
    jiB.stops[0].terms = {1};
    b.commands.push_back(AddJiObject{{lead, clipB}, jiA});
    b.commands.push_back(AddJiObject{{lead, clipB}, jiB});
    note(-5, PitchId::fromDegree(-5, -2)).velocity = 127.0f;
    note(1, PitchId::fromRatio(Rational{5, 4})).velocity = 0.1f;
    note(2, PitchId::fromRatio(Rational{7, 4}, AnchorKind::Note, anchor.value)).releaseVelocity = 0.0f;
    note(3, PitchId::fromRatio(Rational{3, 2}, AnchorKind::JiFundamental, jiA.id.value)).chance = 0.25f;
    note(4, PitchId::fromRatio(Rational{INT64_MAX, INT64_MAX - 1})).velocityRange = -127.0f;
    note(5, PitchId::fromRatio(Rational{9, 8}, AnchorKind::Reference, 9)).releaseVelocity = -0.0f;  // anchorId kept
    note(6, PitchId::fromCents(386.3137138648348)).velocityRange = -0.0f;
    note(7, PitchId::fromCents(-0.0)).velocity = std::numeric_limits<float>::denorm_min();
    note(8, PitchId::fromCents(std::numeric_limits<double>::denorm_min())).velocity = FLT_MIN;
    note(9, PitchId::fromCents(std::numeric_limits<double>::max())).chance = 0.0f;
    note(10, PitchId::fromCents(-1e-300)).flags = kNoteDisabled;
    note(11, PitchId::fromCents(1.0 / 3.0)).velocity = 1.0f / 3.0f;
    note(12, PitchId::fromHarmonic(jiA.id.value, 3));
    note(kMaxTicks, PitchId::fromHarmonic(jiB.id.value, 0)).length = 1;
    note(-kMaxTicks, PitchId::fromDegree(INT32_MIN, INT32_MAX)).length = kMaxTicks;
    b.commands.push_back(AddNotes{{lead, clipB}, notes});
    // T7.2: a note curve (with a jump).
    b.commands.push_back(SetNoteCurve{{lead, clipB}, keyOf(notes[1]), {{0, 0.0f, 0.0f, {}}, {50, -25.5f, 0.25f, {}}, {50, 10.0f, 0.0f, {}}}});
    // An empty clip on the nested track.
    NoteClip empty;
    empty.id = clipR = ids.clip();
    empty.name = "";
    empty.start = -pq::kTicksPerWhole;
    empty.length = 1;
    b.commands.push_back(AddClip{innerMidi, std::make_shared<const NoteClip>(empty)});
    // One lane of every automatable ParamKind (TrackVolume and MosGenerator exist in the fixture).
    auto lane = [&](TrackId host, ParamRef r, std::vector<Breakpoint> pts) {
      AutomationLane l;
      l.id = ids.lane();
      l.param = r;
      for (Breakpoint& p : pts) p.id = ids.breakpoint();
      l.points = std::move(pts);
      b.commands.push_back(AddAutomationLane{host, std::make_shared<const AutomationLane>(l)});
      return l.id;
    };
    lanes[0] = lane(bass, ParamRef::track(ParamKind::TrackPan, bass), {{0, -1.0f, -1.0f, {}}, {10, 1.0f, 1.0f, {}}});
    lanes[1] = lane(kick, ParamRef::track(ParamKind::TrackMute, kick), {{0, 1.0f, 0.0f, {}}});
    lanes[2] = lane(lead, ParamRef::track(ParamKind::TrackSend, lead, reverb), {{-100, -70.0f, -0.0f, {}}});
    lanes[3] = lane(lead, ParamRef::tuning(ParamKind::TuningReference, t31), {{0, 440.0f, 0.5f, {}}});
    lanes[4] = lane(innerMidi, ParamRef::tuning(ParamKind::TuningEquave, t31), {});
    lanes[5] = lane(bass, ParamRef::tuning(ParamKind::MosPeriod, tMos), {{5, 1200.0f, 0.0f, {}}, {5, 1100.0f, 0.0f, {}}});
    lanes[6] = lane(master, ParamRef::tuning(ParamKind::MosStretch, tMos), {{0, -1199.0f, 0.0f, {}}});
    lanes[7] = lane(drums, ParamRef::track(ParamKind::TrackVolume, drums), {{0, 0.1f, 0.0f, {}}});
    // Timeline, loop, locators.
    b.commands.push_back(SetTempoMap{{{0, 120.0, pq::TempoCurve::Linear},
                                      {4 * pq::kTicksPerWhole, 87.654321, pq::TempoCurve::Step},
                                      {8 * pq::kTicksPerWhole, 1000.0, pq::TempoCurve::Linear}}});
    b.commands.push_back(SetTimeSignatures{{{0, {4, 4}}, {4, {7, 8}}, {6, {11, 12}}}});
    b.commands.push_back(SetLoop{LoopRegion{pq::kTicksPerWhole, 3 * pq::kTicksPerWhole, true}});
    b.commands.push_back(AddLocator{Locator{ids.locator(), -pq::kPpq, "Pickup \xE6\x97\xA5\xE6\x9C\xAC"}});
    b.commands.push_back(AddLocator{Locator{ids.locator(), pq::kTicksPerWhole, ""}});  // same tick as "Verse"
    b.commands.push_back(RenameTuning{tScale, "Scala \xF0\x9F\x8E\xB5"});
    const pq::Status st = store.dispatch(b);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    // Every MIDI track's instrument: a Wavetable engine in an instrument group (engineInstrument: a small instrument
    // with no knob curves, so the serialization tests may relabel the project as an older schema; GU.11: no stock
    // instrument), ids after every other entity in track order; then every track wired in series (source → its path →
    // its mixer).
    std::vector<TrackId> midi;
    forEachTrack(*store.current(), [&](const Track& t, const Track*) {
      if (t.kind == TrackKind::Midi) midi.push_back(t.id);
    });
    for (const TrackId t : midi) {
      auto e = ops::pasteBlocks(*store.current(), ids, t, engineInstrument());
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
      const pq::Status si = store.dispatch(e->command);
      REQUIRE_MESSAGE(si.ok(), (si.ok() ? std::string() : si.error().message));
    }
    auto wired = ops::wireSeries(*store.current(), ids);
    REQUIRE_MESSAGE(wired.ok(), (wired.ok() ? std::string() : wired.error().message));
    const pq::Status sw = store.dispatch(wired->command);
    REQUIRE_MESSAGE(sw.ok(), (sw.ok() ? std::string() : sw.error().message));
    // TX.pan-laws: every MIDI track pans with constant power (a non-default law on every MIDI track).
    Batch laws{"pan laws", {}};
    forEachTrack(*store.current(), [&](const Track& t, const Track*) {
      if (t.kind == TrackKind::Midi) laws.commands.push_back(SetTrackPanMode{t.id, PanMode::ConstantPower});
    });
    const pq::Status sl = store.dispatch(laws);
    REQUIRE_MESSAGE(sl.ok(), (sl.ok() ? std::string() : sl.error().message));
    // View state (not undoable, saved with the project).
    store.view().tracks[drums] = TrackView{true, 0};
    store.view().tracks[lead] = TrackView{false, 120};
    store.view().tracks[master] = TrackView{true, 33};
    store.view().lanes[leadVolLane] = LaneView{false, 0};
    store.view().lanes[lanes[3]] = LaneView{true, 77};
  }
};

// richProject + random content from `seed`.
struct RandomProject : RichProject {
  explicit RandomProject(uint32_t seed) {
    std::mt19937_64 rng(seed);
    IdSource& ids = store.ids();
    auto uni = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<uint64_t>(n)); };
    // Doubles drawn over the whole exponent range, plus specials.
    auto wildDouble = [&]() -> double {
      switch (pick(6)) {
        case 0: return uni(-1e4, 1e4);
        case 1: return std::ldexp(uni(-1.0, 1.0), pick(2046) - 1022);
        case 2: return std::bit_cast<double>(rng() & 0x000FFFFFFFFFFFFFull);  // denormal
        case 3: return pick(2) ? -0.0 : 0.0;
        case 4: return static_cast<double>(static_cast<int64_t>(rng())) / 7.0;
        default: return uni(-1.0, 1.0) * 1e-200;
      }
    };
    auto unit = [&](float lo, float hi) {
      const float v = static_cast<float>(uni(lo, hi));
      return std::min(std::max(v, lo), hi);
    };
    Batch b{"random", {}};
    // T7.2: a random JI object in the clip; the random Harmonic / JiFundamental notes follow it.
    JiObject ji;
    ji.id = ids.jiObject();
    ji.start = static_cast<pq::Ticks>(pick(100000)) - 50000;
    ji.length = 1 + pick(1000000);
    ji.fundamental = pick(2) ? pq::tuning::PitchId::fromDegree(pick(100) - 50, pick(7) - 3) : pq::tuning::PitchId::fromCents(uni(-4800.0, 4800.0));
    ji.undertone = pick(2) != 0;
    ji.sounds = pick(2) != 0;
    const int jiVoices = 1 + pick(8);
    for (int i = 0; i < jiVoices; ++i) ji.voiceNotes.push_back(ids.note());
    const int jiStops = 1 + pick(5);
    for (int k = 0; k < jiStops; ++k) {
      JiStop st;
      st.at = k == 0 ? 0 : ji.stops.back().at + 1 + pick(5000);
      const int n = k == 0 ? jiVoices : 1 + pick(jiVoices);
      int64_t term = 0;
      for (int i = 0; i < n; ++i) {
        term += 1 + pick(40);
        if (k % 2 == 1) st.pitches.push_back(pq::tuning::PitchId::fromDegree(static_cast<int32_t>(term), pick(3)));
        else st.terms.push_back(term);
      }
      st.scale = pq::tuning::Rational{1 + pick(30), 1 + pick(30)};
      st.timing = JiTiming{pick(100), pick(3000), JiShape{static_cast<JiShape::Kind>(pick(3)), static_cast<float>(uni(0.0, 1.0)),
                                                         static_cast<float>(uni(-1.0, 2.0)), static_cast<float>(uni(0.0, 1.0)),
                                                         static_cast<float>(uni(-1.0, 2.0))}};
      if (pick(2))
        for (int i = 0; i < jiVoices; ++i) st.voiceTerm.push_back(static_cast<uint8_t>(pick(n)));
      if (pick(2))
        for (int i = 0; i < jiVoices; ++i)
          st.voiceTiming.push_back(pick(2) ? std::optional<JiTiming>(JiTiming{pick(50), pick(500), JiShape{}}) : std::nullopt);
      ji.stops.push_back(std::move(st));
    }
    for (int i = 0, tick = 0; i < pick(6); ++i) {
      tick += pick(1000);
      ji.ride.push_back({tick, static_cast<float>(uni(-1200.0, 1200.0)), static_cast<float>(uni(-1.0, 1.0)), {}});
    }
    b.commands.push_back(AddJiObject{{kick, clipK}, ji});
    std::vector<Note> notes;
    const int count = 50 + pick(400);
    for (int i = 0; i < count; ++i) {
      Note n;
      n.id = ids.note();
      n.start = static_cast<pq::Ticks>(rng() % (2 * static_cast<uint64_t>(kMaxTicks))) - kMaxTicks;
      n.length = 1 + static_cast<pq::Ticks>(rng() % 1000000);
      switch (pick(4)) {
        case 0: n.pitch = pq::tuning::PitchId::fromDegree(static_cast<int32_t>(rng()), static_cast<int32_t>(rng())); break;
        case 1: {
          auto r = pq::tuning::Rational::tryMake(1 + static_cast<int64_t>(rng() >> 2),
                                                  1 + static_cast<int64_t>((rng() >> 2) >> pick(62)));
          const auto anchorKind = static_cast<pq::tuning::AnchorKind>(pick(3));
          n.pitch = pq::tuning::PitchId::fromRatio(
              *r, anchorKind, anchorKind == pq::tuning::AnchorKind::JiFundamental ? ji.id.value : static_cast<uint32_t>(rng()));
          break;
        }
        case 2: n.pitch = pq::tuning::PitchId::fromCents(wildDouble()); break;
        default: n.pitch = pq::tuning::PitchId::fromHarmonic(ji.id.value, static_cast<uint32_t>(pick(jiVoices))); break;
      }
      n.velocity = std::max(unit(0.0f, 127.0f), FLT_MIN);
      n.releaseVelocity = unit(0.0f, 127.0f);
      n.chance = unit(0.0f, 1.0f);
      n.velocityRange = unit(-127.0f, 127.0f);
      n.flags = pick(2) ? kNoteDisabled : 0;
      notes.push_back(n);
    }
    b.commands.push_back(AddNotes{{kick, clipK}, notes});
    for (int i = 0; i < 5; ++i) {  // T7.2: random note curves
      std::vector<Breakpoint> curve;
      for (int k = 0, tick = 0; k < 1 + pick(5); ++k) {
        tick += pick(500);
        curve.push_back({tick, static_cast<float>(uni(-2400.0, 2400.0)), static_cast<float>(uni(-1.0, 1.0)), {}});
      }
      b.commands.push_back(SetNoteCurve{{kick, clipK}, keyOf(notes[static_cast<size_t>(pick(count))]), curve});
    }
    std::vector<Breakpoint> pts;
    for (int i = 0; i < 20; ++i)
      pts.push_back({static_cast<pq::Ticks>(pick(100000)), unit(-70.0f, 6.0f), unit(-1.0f, 1.0f), ids.breakpoint()});
    b.commands.push_back(AddBreakpoints{lead, leadVolLane, pts});
    b.commands.push_back(SetTrackVolume{bass, unit(-70.0f, 6.0f)});
    b.commands.push_back(SetSendLevel{kick, reverb, unit(-70.0f, 6.0f)});
    b.commands.push_back(SetTempoMap{{{0, uni(1.0, 1000.0), pq::TempoCurve::Linear},
                                      {1 + pick(1000000), uni(1.0, 1000.0), pq::TempoCurve::Step}}});
    b.commands.push_back(SetTuningParam{tMos, ParamKind::MosGenerator, uni(600.0, 720.0)});
    b.commands.push_back(SetTuningParam{tScale, ParamKind::TuningReference, uni(1.0, 20000.0)});
    const pq::Status st = store.dispatch(b);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
};

}  // namespace doctest_fixture
