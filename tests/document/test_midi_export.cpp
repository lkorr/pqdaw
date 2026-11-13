// MF.3: MIDI file export (pq/document/midi_export.h): clips → SMF bytes, read back with MF.1 (parseMidiFile /
// resolveNotes) and imported with MF.2 (importMidi): 12edo keys exact, MPE pitches within 0.3 ¢ (half a 14-bit step at
// ±48), Degrees identical degrees (with and without a .kbm), the loop unrolled, disabled notes skipped, the tempo map
// (a ramp stepped, durations kept) and signatures from the clip's start, > 15 MPE voices stolen and counted.
#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "doc_fixture.h"
#include "edit_helpers.h"
#include "pq/document/input_map.h"
#include "pq/document/midi_export.h"
#include "pq/document/midi_file.h"
#include "pq/document/midi_import.h"
#include "pq/document/pitch_resolve.h"
#include "pq/tuning/edo.h"

using namespace pq;
using namespace pq::doc;
using edit_test::commit;
using edit_test::kBar;

namespace {

constexpr int64_t kDiv = 960;
constexpr int64_t kFileBar = 4 * kDiv;

struct Doc {
  DocumentStore store;
  TrackId keys;
  Doc() { keys = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi, {}, "Keys")).tracks.at(0); }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    commit(store, e);
    return *e;
  }
  const Track& track(TrackId id) const {
    const Track* t = findTrackPtr(doc(), id);
    REQUIRE(t);
    return *t;
  }
  void tuning(TrackId t, TuningDef def) {
    const TuningId id = store.ids().tuning();
    REQUIRE(store.dispatch(AddTuning{TuningSlot{id, "t", Colour{}, std::move(def)}}).ok());
    REQUIRE(store.dispatch(SetTrackTuning{t, TuningAssignment::mainTuning(id)}).ok());
  }
  // A clip of `notes` (content ticks) on `track`, [start, start + length), looped over its length when `loop`.
  ClipRef clip(TrackId track, Ticks start, Ticks length, std::vector<Note> notes, bool loop = false,
               std::string name = "Clip") {
    ops::CapturedClip c{track, start, length, loop, std::move(notes), std::move(name)};
    const ops::Edit e = edit(ops::captureClip(doc(), store.ids(), std::span<const ops::CapturedClip>(&c, 1)));
    return e.clips.at(0);
  }
  Note note(Ticks start, Ticks length, tuning::PitchId pitch, float velocity = 100.0f) {
    Note n;
    n.id = store.ids().note();
    n.start = start;
    n.length = length;
    n.pitch = pitch;
    n.velocity = velocity;
    return n;
  }
};

struct Exported {
  MidiFile file;
  MidiNotes notes;
  ExportMidiReport report;
};
Exported exportAndRead(const Project& p, std::vector<ClipRef> clips, MidiExportMode mode) {
  Exported out;
  auto bytes = exportMidi(p, clips, ExportMidiOptions{mode, static_cast<uint16_t>(kDiv)}, &out.report);
  REQUIRE_MESSAGE(bytes.ok(), (bytes.ok() ? std::string() : bytes.error().message));
  auto f = parseMidiFile(*bytes);
  REQUIRE_MESSAGE(f.ok(), (f.ok() ? std::string() : f.error().message));
  out.file = std::move(*f);
  out.notes = resolveNotes(out.file);
  CHECK(out.file.format == 1);
  CHECK(out.file.division.ppq == kDiv);
  CHECK(out.file.tracks.size() == clips.size() + 1);
  return out;
}

ops::ImportMidiOptions importOptions(const MidiFile& f, TrackId target) {
  ops::ImportMidiOptions o;
  o.format = f.format;
  o.division = f.division;
  o.target = target;
  o.name = "song";
  return o;
}

// The played notes' static Hz of a clip, in note order.
std::vector<double> hzOf(const Project& p, ClipRef c) {
  auto cp = resolveClipPitches(p, c.track, c.clip);
  REQUIRE(cp.ok());
  std::vector<double> out;
  for (const NotePitch& n : cp->notes) {
    REQUIRE(n.hz);
    out.push_back(*n.hz);
  }
  return out;
}

int64_t absDegree(const tuning::PitchId& p, int64_t size) {
  REQUIRE(p.kind() == tuning::PitchKind::Degree);
  return static_cast<int64_t>(p.equave()) * size + p.degree();
}

}  // namespace

TEST_CASE("midi export: 12edo — nearest keys, no bends, stored velocities, disabled notes skipped; imports back to the "
          "same degrees") {
  Doc d;
  std::vector<Note> ns;
  const int32_t degrees[] = {-12, 0, 4, 7, 19};
  for (int i = 0; i < 5; ++i)
    ns.push_back(d.note(i * kPpq, kPpq / 2, tuning::PitchId::fromDegree(degrees[i]), 20.0f + 20.0f * static_cast<float>(i)));
  Note off = d.note(5 * kPpq, kPpq, tuning::PitchId::fromDegree(2));
  off.flags |= kNoteDisabled;
  ns.push_back(off);
  ns.push_back(d.note(6 * kPpq, kPpq, tuning::PitchId::fromCents(1200.0 * std::log2(440.0 / tuning::kDefaultReferenceHz) + 30.0)));
  const ClipRef c = d.clip(d.keys, kBar, 2 * kBar, ns, false, "Lead");
  const Exported e = exportAndRead(d.doc(), {c}, MidiExportMode::TwelveEdo);
  CHECK(e.report.notes == 6);
  CHECK(e.report.clips == 1);
  CHECK(e.notes.trackNames.at(1) == "Lead");
  REQUIRE(e.notes.notes.size() == 6);
  const int keys[] = {48, 60, 64, 67, 79, 69};  // A4 + 30 ¢ → 69
  for (size_t i = 0; i < 6; ++i) {
    CAPTURE(i);
    const MidiNote& n = e.notes.notes[i];
    CHECK(n.key == keys[i]);
    CHECK(n.cents == 0.0);
    CHECK(n.channel == 0);
    CHECK(n.track == 1);
    CHECK(n.tick == (i < 5 ? static_cast<int64_t>(i) : 6) * kDiv);  // from the clip's start
    CHECK(n.length == (i < 5 ? kDiv / 2 : kDiv));
    if (i < 5) CHECK(n.velocity == 20 + 20 * i);
  }
  for (const MidiEvent& ev : e.file.tracks[1].events) CHECK(ev.kind() != 0xE0);  // no bends
  // MF.2 import onto a 12edo Keys track: the same degrees.
  Doc back;
  back.edit(ops::importMidi(back.doc(), back.store.ids(), e.notes, importOptions(e.file, back.keys)));
  const Track& t = back.track(back.keys);
  REQUIRE(t.clips.size() == 1);
  std::vector<Note> got(t.clips[0].clip->notes.begin(), t.clips[0].clip->notes.end());
  REQUIRE(got.size() == 6);
  for (size_t i = 0; i < 5; ++i) CHECK(absDegree(got[i].pitch, 12) == degrees[i]);
  CHECK(absDegree(got[5].pitch, 12) == 9);
}

TEST_CASE("midi export: MPE — MCM + RPN 0 ±48, one member channel per sounding note, bends within 0.3 ¢ of the exact "
          "pitch (31edo, a 7/4 ratio, cents)") {
  Doc d;
  TuningDef edo31;
  edo31.system = tuning::Edo::make(31).value();
  d.tuning(d.keys, edo31);
  std::vector<Note> ns;
  for (int i = 0; i < 40; ++i)  // overlapping runs: four notes sound at once
    ns.push_back(d.note(i * kPpq / 4, kPpq, tuning::PitchId::fromDegree(i * 3 - 30)));
  ns.push_back(d.note(11 * kPpq, kPpq, tuning::PitchId::fromRatio(tuning::Rational(7, 4))));
  ns.push_back(d.note(11 * kPpq, kPpq, tuning::PitchId::fromCents(-1234.5678)));
  const ClipRef c = d.clip(d.keys, 0, 4 * kBar, ns);
  const std::vector<double> hz = hzOf(d.doc(), c);
  const Exported e = exportAndRead(d.doc(), {c}, MidiExportMode::Mpe);
  CHECK(e.report.stolen == 0);
  CHECK(e.report.maxVoices == 4);
  REQUIRE(e.notes.notes.size() == hz.size());
  CHECK(e.notes.midNoteBends == 0);
  // The setup: RPN 6 = 15 on channel 1 at tick 0, RPN 0 = 48 on each member.
  int mcm = 0, ranges = 0;
  const auto& ev = e.file.tracks[1].events;
  for (size_t i = 0; i + 2 < ev.size(); ++i)
    if (ev[i].tick == 0 && ev[i].kind() == 0xB0 && ev[i].data1 == 101 && ev[i + 1].data1 == 100 && ev[i + 2].data1 == 6) {
      if (ev[i].data2 == 0 && ev[i + 1].data2 == 6 && ev[i].channel() == 0 && ev[i + 2].data2 == 15) ++mcm;
      if (ev[i].data2 == 0 && ev[i + 1].data2 == 0 && ev[i].channel() >= 1 && ev[i + 2].data2 == 48) ++ranges;
    }
  CHECK(mcm == 1);
  CHECK(ranges == 15);
  double worst = 0.0;
  for (size_t i = 0; i < hz.size(); ++i) {
    const MidiNote& n = e.notes.notes[i];
    CHECK(n.channel >= 1);
    const double got = twelveTetHz(n.key) * std::exp2(n.cents / 1200.0);
    worst = std::max(worst, std::fabs(1200.0 * std::log2(got / hz[i])));
  }
  MESSAGE("MPE worst error: " << worst << " cents");
  CHECK(worst <= 0.3);
  // Overlapping notes never share a channel.
  for (size_t i = 0; i < e.notes.notes.size(); ++i)
    for (size_t j = i + 1; j < e.notes.notes.size(); ++j) {
      const MidiNote& a = e.notes.notes[i];
      const MidiNote& b = e.notes.notes[j];
      if (a.tick < b.tick + b.length && b.tick < a.tick + a.length) CHECK(a.channel != b.channel);
    }
}

TEST_CASE("midi export: MPE with 17 notes at once — the oldest channel is reused (its note ends), counted") {
  Doc d;
  std::vector<Note> ns;
  for (int i = 0; i < 17; ++i) ns.push_back(d.note(i * kPpq / 4, 2 * kBar, tuning::PitchId::fromDegree(i)));
  const ClipRef c = d.clip(d.keys, 0, 4 * kBar, ns);
  const Exported e = exportAndRead(d.doc(), {c}, MidiExportMode::Mpe);
  CHECK(e.report.maxVoices == 17);
  CHECK(e.report.stolen == 2);
  CHECK(e.report.notes == 17);
  CHECK(describeMidiExport(e.report, MidiExportMode::Mpe) == "Exported 17 notes (MPE), 2 voices over 15 ended early");
  REQUIRE(e.notes.notes.size() == 17);
  CHECK(e.notes.midNoteBends == 0);  // a stolen channel's note ended before the new bend
  CHECK(e.notes.notes[0].length == 15 * kDiv / 4);  // ended at note 15's start
  CHECK(e.notes.notes[1].length == 15 * kDiv / 4);  // note 16's
  CHECK(e.notes.notes[15].channel == e.notes.notes[0].channel);
  CHECK(e.notes.notes[16].channel == e.notes.notes[1].channel);
  for (size_t i = 0; i < 17; ++i) CHECK(std::fabs(e.notes.notes[i].cents) < 0.3);
}

TEST_CASE("midi export: Degrees — key = 60 + absolute degree (31edo), off-degree notes snap; imports back to the same "
          "degrees") {
  Doc d;
  TuningDef edo31;
  edo31.system = tuning::Edo::make(31).value();
  d.tuning(d.keys, edo31);
  std::vector<Note> ns;
  for (int i = 0; i < 20; ++i) ns.push_back(d.note(i * kPpq, kPpq, tuning::PitchId::fromDegree(i * 4 - 33)));
  // 3 ¢ above degree 5: snaps to it.
  ns.push_back(d.note(20 * kPpq, kPpq, tuning::PitchId::fromCents(5 * 1200.0 / 31.0 + 3.0)));
  ns.push_back(d.note(21 * kPpq, kPpq, tuning::PitchId::fromDegree(80)));  // key 140: out of range
  const ClipRef c = d.clip(d.keys, 0, 8 * kBar, ns);
  const Exported e = exportAndRead(d.doc(), {c}, MidiExportMode::Degrees);
  CHECK(e.report.snapped == 1);
  CHECK(e.report.outOfRange == 1);
  REQUIRE(e.notes.notes.size() == 21);
  for (int i = 0; i < 20; ++i) CHECK(e.notes.notes[static_cast<size_t>(i)].key == 60 + i * 4 - 33);
  CHECK(e.notes.notes[20].key == 65);
  Doc back;
  back.tuning(back.keys, edo31);
  back.edit(ops::importMidi(back.doc(), back.store.ids(), e.notes, importOptions(e.file, back.keys)));
  const Track& t = back.track(back.keys);
  REQUIRE(t.clips.size() == 1);
  std::vector<Note> got(t.clips[0].clip->notes.begin(), t.clips[0].clip->notes.end());
  REQUIRE(got.size() == 21);
  for (int i = 0; i < 20; ++i) CHECK(absDegree(got[static_cast<size_t>(i)].pitch, 31) == i * 4 - 33);
  CHECK(absDegree(got[20].pitch, 31) == 5);
}

TEST_CASE("midi export: Degrees through a .kbm — the lowest key mapped to the degree; a degree no key plays is skipped") {
  // The fixture .kbm: 4 keys per pattern from key 60: degrees 0, 1, x, 3 (4 per repetition).
  Doc d;
  d.tuning(d.keys, doctest_fixture::fixtureScaleDef());
  std::vector<Note> ns;
  for (int i = 0; i < 9; ++i) ns.push_back(d.note(i * kPpq, kPpq, tuning::PitchId::fromDegree(i - 2)));
  const ClipRef c = d.clip(d.keys, 0, 4 * kBar, ns);
  const Exported e = exportAndRead(d.doc(), {c}, MidiExportMode::Degrees);
  // Degrees −2 … 6: −2, 2 and 6 fall on 'x' keys (58, 62, 66): no key plays them.
  CHECK(e.report.outOfRange == 3);
  const int keys[] = {59, 60, 61, 63, 64, 65};  // degrees −1, 0, 1, 3, 4, 5
  REQUIRE(e.notes.notes.size() == 6);
  for (size_t i = 0; i < 6; ++i) CHECK(e.notes.notes[i].key == keys[i]);
  Doc back;
  back.tuning(back.keys, doctest_fixture::fixtureScaleDef());
  back.edit(ops::importMidi(back.doc(), back.store.ids(), e.notes, importOptions(e.file, back.keys)));
  const Track& t = back.track(back.keys);
  std::vector<Note> got(t.clips.at(0).clip->notes.begin(), t.clips.at(0).clip->notes.end());
  const int64_t want[] = {-1, 0, 1, 3, 4, 5};
  REQUIRE(got.size() == 6);
  for (size_t i = 0; i < 6; ++i) CHECK(absDegree(got[i].pitch, 4) == want[i]);
}

TEST_CASE("midi export: a looped clip unrolls over its length; tempo map (a ramp stepped, seconds kept) and signatures "
          "from the clip's start; one MTrk per clip") {
  Doc d;
  REQUIRE(d.store.dispatch(SetTempoMap{{TempoPoint{0, 120.0, TempoCurve::Step}, TempoPoint{2 * kBar, 100.0, TempoCurve::Linear},
                                        TempoPoint{4 * kBar, 140.0, TempoCurve::Step}}}).ok());
  REQUIRE(d.store.dispatch(SetTimeSignatures{{TimeSigChange{0, {4, 4}}, TimeSigChange{4, {7, 8}}, TimeSigChange{5, {5, 12}}}}).ok());
  std::vector<Note> ns{d.note(0, kPpq, tuning::PitchId::fromDegree(0)), d.note(2 * kPpq, kPpq, tuning::PitchId::fromDegree(4))};
  Note dis = d.note(kPpq, kPpq, tuning::PitchId::fromDegree(7));
  dis.flags |= kNoteDisabled;
  ns.push_back(dis);
  const ClipRef loop = d.clip(d.keys, kBar, kBar, ns, true, "Loop");
  REQUIRE(d.store.dispatch(SetClipBounds{loop, kBar, 4 * kBar, 0}).ok());  // four passes
  const TrackId other = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Midi, d.keys, "B")).tracks.at(0);
  const ClipRef b = d.clip(other, 3 * kBar, kBar, {d.note(0, 4 * kPpq, tuning::PitchId::fromDegree(-12))}, false, "Bass");
  const Exported e = exportAndRead(d.doc(), {loop, b}, MidiExportMode::TwelveEdo);
  CHECK(e.notes.trackNames.at(1) == "Loop");
  CHECK(e.notes.trackNames.at(2) == "Bass");
  CHECK(e.notes.trackNotes.at(1) == 8);
  CHECK(e.notes.trackNotes.at(2) == 1);
  std::vector<int64_t> loopTicks;
  for (const MidiNote& n : e.notes.notes)
    if (n.track == 1) loopTicks.push_back(n.tick);
  const std::vector<int64_t> want = {0, 2 * kDiv, kFileBar, kFileBar + 2 * kDiv, 2 * kFileBar, 2 * kFileBar + 2 * kDiv,
                                     3 * kFileBar, 3 * kFileBar + 2 * kDiv};
  CHECK(loopTicks == want);
  CHECK(e.file.tracks[1].endTick == 4 * kFileBar);
  // Tempo: 120 at the clip's start (bar 1), the 100 → 140 ramp from file bar 1 in sixteenths, then 140.
  const auto& tempos = e.notes.tempos;
  REQUIRE(tempos.size() == 1 + 32 + 1);
  CHECK(tempos.front().tick == 0);
  CHECK(tempos.front().usPerQuarter == 500000);
  CHECK(tempos[1].tick == kFileBar);
  CHECK(tempos.back().tick == 3 * kFileBar);
  CHECK(tempos.back().usPerQuarter == 428571);
  for (size_t i = 2; i + 1 < tempos.size(); ++i) CHECK(tempos[i].usPerQuarter < tempos[i - 1].usPerQuarter);
  double seconds = 0.0;  // the file's duration of the four exported bars vs the project's
  for (size_t i = 0; i < tempos.size(); ++i) {
    const int64_t until = i + 1 < tempos.size() ? tempos[i + 1].tick : 4 * kFileBar;
    seconds += static_cast<double>(until - tempos[i].tick) / kDiv * tempos[i].usPerQuarter * 1e-6;
  }
  const TempoMap& tm = d.doc().timeline->tempoMap;
  CHECK(std::fabs(seconds - (tm.ticksToSeconds(5 * kBar) - tm.ticksToSeconds(kBar))) < 34 * 1e-6);
  // Signatures: 4/4 at the start, 7/8 at bar 4 (file bar 3); 5/12 (bar 5) cannot be written.
  REQUIRE(e.notes.signatures.size() == 2);
  CHECK(e.notes.signatures[0] == MidiTimeSignature{0, 4, 4});
  CHECK(e.notes.signatures[1] == MidiTimeSignature{3 * kFileBar, 7, 8});
  CHECK(e.report.skippedSignatures == 1);
  CHECK(e.report.signatures == 2);
}

TEST_CASE("midi export: errors — no clips, an audio clip, a bad division") {
  Doc d;
  const ClipRef c = d.clip(d.keys, 0, kBar, {d.note(0, kPpq, tuning::PitchId::fromDegree(0))});
  CHECK_FALSE(exportMidi(d.doc(), {}).ok());
  CHECK_FALSE(exportMidi(d.doc(), std::vector<ClipRef>{c}, ExportMidiOptions{MidiExportMode::Mpe, 0}).ok());
  CHECK_FALSE(exportMidi(d.doc(), std::vector<ClipRef>{ClipRef{d.keys, ClipId{999999}}}).ok());
  CHECK(exportMidi(d.doc(), std::vector<ClipRef>{c}).ok());
}
