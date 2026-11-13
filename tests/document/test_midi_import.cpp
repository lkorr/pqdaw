// MF.2: MIDI file import (pq/document/midi_import.h): files built with MF.1's MidiTrack builders, written and read back,
// imported onto Keys / Hz tracks (plain keys = live input's identity; bends and MTS → Cents or snapped Degrees), format 1
// → one track per MTrk, format 0 → one track, tempo / signatures only into a project without clips, ticks, one undo step.
#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "edit_helpers.h"
#include "pq/document/input_map.h"
#include "pq/document/midi_file.h"
#include "pq/document/midi_import.h"
#include "pq/tuning/edo.h"

using namespace pq;
using namespace pq::doc;
using edit_test::commit;
using edit_test::kBar;

namespace {

constexpr uint16_t kFilePpq = 480;

// A file of `tracks` (format `format`, PPQ 480), written and read back, resolved.
struct Imported {
  MidiFile file;
  MidiNotes notes;
};
Imported roundTrip(std::vector<MidiTrack> tracks, uint16_t format = 1, MidiDivision div = {}) {
  MidiFile f;
  f.format = format;
  f.division = div.isSmpte() ? div : MidiDivision{kFilePpq, 0, 0};
  f.tracks = std::move(tracks);
  auto parsed = parseMidiFile(writeMidiFile(f));
  REQUIRE(parsed.ok());
  Imported out{std::move(*parsed), {}};
  out.notes = resolveNotes(out.file);
  return out;
}

ops::ImportMidiOptions optionsFor(const MidiFile& f, TrackId target = {}, Ticks at = 0) {
  ops::ImportMidiOptions o;
  o.format = f.format;
  o.division = f.division;
  o.target = target;
  o.at = at;
  o.name = "song";
  return o;
}

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
  std::vector<Note> notesOf(TrackId id, size_t clip = 0) const {
    const Track& t = track(id);
    REQUIRE(clip < t.clips.size());
    REQUIRE(t.clips[clip].clip);
    return {t.clips[clip].clip->notes.begin(), t.clips[clip].clip->notes.end()};
  }
  InputPitchMapPtr map(TrackId id) const {
    auto m = buildInputPitchMap(doc(), id);
    REQUIRE(m.ok());
    return *m;
  }
  void tuning(TrackId t, TuningDef def) {
    const TuningId id = store.ids().tuning();
    REQUIRE(store.dispatch(AddTuning{TuningSlot{id, "t", Colour{}, std::move(def)}}).ok());
    REQUIRE(store.dispatch(SetTrackTuning{t, TuningAssignment::mainTuning(id)}).ok());
  }
  void mode(TrackId t, NoteInputMode m) { REQUIRE(store.dispatch(SetNoteInput{t, NoteInput{m, InputKeyMap::Tuning}}).ok()); }
};

// A C-major-ish run on channel 0: keys 60, 62, 64, 69, one beat each from beat 0.
MidiTrack plainRun() {
  MidiTrack t;
  t.name("Piano");
  int64_t tick = 0;
  for (const int k : {60, 62, 64, 69}) {
    t.noteOn(tick, 0, static_cast<uint8_t>(k), 100);
    t.noteOff(tick + kFilePpq, 0, static_cast<uint8_t>(k), 40);
    tick += kFilePpq;
  }
  return t;
}

// MTS frequency data (08 01 / 08 02): semitone, 14-bit fraction, for `abs` cents above key 0's 12-TET pitch.
void mtsFreq(std::vector<uint8_t>& b, double abs) {
  const int semi = static_cast<int>(std::floor(abs / 100.0));
  int frac = static_cast<int>(std::lround((abs - semi * 100.0) * 16384.0 / 100.0));
  int s = semi;
  if (frac >= 16384) {
    frac -= 16384;
    ++s;
  }
  b.push_back(static_cast<uint8_t>(s));
  b.push_back(static_cast<uint8_t>(frac >> 7));
  b.push_back(static_cast<uint8_t>(frac & 127));
}

}  // namespace

TEST_CASE("midi import: ticks are exact (PPQ rounding, SMPTE at 120 BPM)") {
  CHECK(ops::midiPpqTicks(480, 480) == kPpq);
  CHECK(ops::midiPpqTicks(1, 7) == kPpq / 7);  // 221760 = 7 · 31680
  CHECK(ops::midiPpqTicks(1, 1000) == 222);    // 221.76 rounds up
  CHECK(ops::midiPpqTicks(int64_t{1} << 40, 960) == (int64_t{1} << 40) / 960 * kPpq +
                                                         ((int64_t{1} << 40) % 960 * kPpq * 2 + 960) / 1920);
  // SMPTE 25 fps × 40 = 1000 ticks / s; a note at 0.5 s for 0.25 s at 120 BPM = beat 1, half a beat long.
  MidiTrack t;
  t.noteOn(500, 0, 60, 90);
  t.noteOff(750, 0, 60);
  Imported f = roundTrip({t}, 0, MidiDivision{0, 25, 40});
  Doc d;
  d.edit(ops::importMidi(d.doc(), d.store.ids(), f.notes, optionsFor(f.file, d.keys)));
  const auto n = d.notesOf(d.keys);
  REQUIRE(n.size() == 1);
  CHECK(n[0].start == kPpq);
  CHECK(n[0].length == kPpq / 2);
}

TEST_CASE("midi import: a 12edo file onto a Keys track gives Degree notes (live input's), onto an Hz track Cents at the "
          "same Hz; clip from the drop point to the last note-off, rounded up to a bar; ONE undo step") {
  Imported f = roundTrip({plainRun()});
  Doc d;
  const TrackId hz = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Midi, d.keys, "Hz")).tracks.at(0);
  d.mode(hz, NoteInputMode::Hz);
  const size_t h0 = d.store.history().cursor();
  ops::MidiImportReport rep;
  d.edit(ops::importMidi(d.doc(), d.store.ids(), f.notes, optionsFor(f.file, d.keys, 2 * kBar), &rep));
  CHECK(d.store.history().cursor() == h0 + 1);
  CHECK(rep.tracks == 1);
  CHECK(rep.newTracks == 0);
  CHECK(rep.notes == 4);
  const Track& k = d.track(d.keys);
  REQUIRE(k.clips.size() == 1);
  CHECK(k.clips[0].start() == 2 * kBar);
  CHECK(k.clips[0].clip->length == kBar);  // 4 beats exactly: one bar
  CHECK(k.clips[0].clip->name == "Piano");
  const InputPitchMapPtr km = d.map(d.keys);
  const auto kn = d.notesOf(d.keys);
  REQUIRE(kn.size() == 4);
  const uint8_t keys[] = {60, 62, 64, 69};
  for (size_t i = 0; i < 4; ++i) {
    CAPTURE(i);
    CHECK(kn[i].start == static_cast<Ticks>(i) * kPpq);
    CHECK(kn[i].length == kPpq);
    CHECK(kn[i].velocity == 100.0f);
    CHECK(kn[i].releaseVelocity == 40.0f);
    CHECK(kn[i].pitch.kind() == tuning::PitchKind::Degree);
    CHECK(kn[i].pitch == km->at(keys[i]).id);
  }
  // Onto the Hz track: Cents identities, the same Hz live input gives.
  d.edit(ops::importMidi(d.doc(), d.store.ids(), f.notes, optionsFor(f.file, hz)));
  const InputPitchMapPtr hm = d.map(hz);
  const auto hn = d.notesOf(hz);
  REQUIRE(hn.size() == 4);
  for (size_t i = 0; i < 4; ++i) {
    CHECK(hn[i].pitch.kind() == tuning::PitchKind::Cents);
    CHECK(hn[i].pitch == hm->at(keys[i]).id);
  }
  // Undo: one step back.
  const size_t h1 = d.store.history().cursor();
  REQUIRE(d.store.undo());
  CHECK(d.store.history().cursor() == h1 - 1);
  CHECK(d.track(hz).clips.empty());
  CHECK(d.track(d.keys).clips.size() == 1);
}

TEST_CASE("midi import: a ±2 bend file gives exact Cents; a bend onto a degree snaps to it in Keys mode") {
  MidiTrack t;
  t.pitchBend(0, 0, 8192 + 2048);  // +50 ¢ (default range ±2 semitones)
  t.noteOn(0, 0, 60, 100);
  t.noteOff(kFilePpq, 0, 60);
  t.pitchBend(kFilePpq, 0, 8192 + 4096);  // +100 ¢: exactly key 61
  t.noteOn(kFilePpq, 0, 60, 100);
  t.noteOff(2 * kFilePpq, 0, 60);
  t.pitchBend(2 * kFilePpq, 0, 8192 - 1);  // −0.0244 ¢: within the snap of key 60's degree
  t.noteOn(2 * kFilePpq, 0, 60, 100);
  t.noteOff(3 * kFilePpq, 0, 60);
  Imported f = roundTrip({t}, 0);
  Doc d;
  ops::MidiImportReport rep;
  d.edit(ops::importMidi(d.doc(), d.store.ids(), f.notes, optionsFor(f.file, d.keys), &rep));
  CHECK(rep.bentNotes == 3);
  const auto n = d.notesOf(d.keys);
  REQUIRE(n.size() == 3);
  const TuningDef* def = effectiveTuning(d.doc(), *findTrack(d.doc(), d.keys));
  REQUIRE(def);
  const double ref = def->referenceHz;  // the default tuning's
  REQUIRE(n[0].pitch.kind() == tuning::PitchKind::Cents);
  CHECK(std::fabs(n[0].pitch.cents() - (1200.0 * std::log2(twelveTetHz(60.0) / ref) + 50.0)) < 1e-9);
  const InputPitchMapPtr m = d.map(d.keys);
  CHECK(n[1].pitch == m->at(61).id);
  CHECK(n[2].pitch == m->at(60).id);
  // On an Hz track every bent note is Cents: the key's 12-TET Hz with the bend.
  const TrackId hz = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Midi, d.keys, "Hz")).tracks.at(0);
  d.mode(hz, NoteInputMode::Hz);
  d.edit(ops::importMidi(d.doc(), d.store.ids(), f.notes, optionsFor(f.file, hz)));
  const auto h = d.notesOf(hz);
  REQUIRE(h.size() == 3);
  const double c60 = 1200.0 * std::log2(twelveTetHz(60.0) / ref);
  const double want[] = {c60 + 50.0, c60 + 100.0, c60 - 200.0 / 8192.0};
  for (size_t i = 0; i < 3; ++i) {
    REQUIRE(h[i].pitch.kind() == tuning::PitchKind::Cents);
    CHECK(std::fabs(h[i].pitch.cents() - want[i]) < 1e-9);
  }
}

TEST_CASE("midi import: an MTS 31edo file onto a 31edo Keys track gives its degrees") {
  // Bulk dump (08 01, program 0): key k → degree k − 60 of 31-EDO at the default reference (key 60 = 261.626 Hz).
  const double ref = tuning::kDefaultReferenceHz;
  std::vector<uint8_t> dump = {0x7E, 0x7F, 0x08, 0x01, 0x00};
  for (int i = 0; i < 16; ++i) dump.push_back(' ');
  for (int k = 0; k < 128; ++k) {
    const double hz = ref * std::exp2((k - 60) / 31.0);
    mtsFreq(dump, 6900.0 + 1200.0 * std::log2(hz / 440.0));
  }
  dump.push_back(0x00);  // checksum (not checked)
  dump.push_back(0xF7);
  MidiTrack t;
  t.sysex(0, 0xF0, dump);
  for (int k = 55; k < 95; ++k) {
    t.noteOn((k - 55) * 120, 0, static_cast<uint8_t>(k), 90);
    t.noteOff((k - 55) * 120 + 100, 0, static_cast<uint8_t>(k));
  }
  Imported f = roundTrip({t}, 0);
  REQUIRE(f.notes.notes.size() == 40);
  Doc d;
  TuningDef edo31;
  edo31.system = tuning::Edo::make(31).value();
  d.tuning(d.keys, edo31);
  ops::MidiImportReport rep;
  d.edit(ops::importMidi(d.doc(), d.store.ids(), f.notes, optionsFor(f.file, d.keys), &rep));
  CHECK(rep.bentNotes >= 39);  // key 60 is the only 12-TET-exact one
  const InputPitchMapPtr m = d.map(d.keys);
  const auto n = d.notesOf(d.keys);
  REQUIRE(n.size() == 40);
  for (size_t i = 0; i < n.size(); ++i) {
    CAPTURE(i);
    CHECK(n[i].pitch.kind() == tuning::PitchKind::Degree);
    CHECK(n[i].pitch == m->at(static_cast<uint8_t>(55 + i)).id);
  }
}

TEST_CASE("midi import: a 3-track format-1 file → 3 tracks (the first on the drop's MIDI track, the rest new after it, "
          "named); format 0 → one track; a non-MIDI target → all new") {
  MidiTrack conductor;
  conductor.tempo(0, 500000);
  std::vector<MidiTrack> tracks{conductor};
  const char* names[] = {"Bass", "", "Lead"};
  for (int i = 0; i < 3; ++i) {
    MidiTrack t;
    if (names[i][0]) t.name(names[i]);
    t.noteOn(i * kFilePpq, static_cast<uint8_t>(i), static_cast<uint8_t>(40 + 12 * i), 100);
    t.noteOff(i * kFilePpq + 5 * kFilePpq, static_cast<uint8_t>(i), static_cast<uint8_t>(40 + 12 * i));  // to beat 7
    tracks.push_back(t);
  }
  Imported f = roundTrip(tracks, 1);
  Doc d;
  const TrackId after = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Midi, d.keys, "After")).tracks.at(0);
  ops::MidiImportReport rep;
  const ops::Edit e = d.edit(ops::importMidi(d.doc(), d.store.ids(), f.notes, optionsFor(f.file, d.keys), &rep));
  CHECK(rep.tracks == 3);
  CHECK(rep.newTracks == 2);
  REQUIRE(e.tracks.size() == 3);
  CHECK(e.tracks[0] == d.keys);
  CHECK(e.clips.size() == 3);
  std::vector<std::string> order;
  forEachTrack(d.doc(), [&](const Track& t, const Track*) {
    if (t.kind == TrackKind::Midi) order.push_back(t.name);
  });
  CHECK(order == std::vector<std::string>{"Keys", "song", "Lead", "After"});
  CHECK(d.track(d.keys).clips[0].clip->name == "Bass");
  for (size_t i = 0; i < 3; ++i) {
    const auto n = d.notesOf(e.tracks[i]);
    REQUIRE(n.size() == 1);
    CHECK(n[0].start == static_cast<Ticks>(i) * kPpq);
    CHECK(d.track(e.tracks[i]).clips[0].clip->length == 2 * kBar);  // ends at beat 5 … 7 → 2 bars
  }
  // Format 0 with three channels: one track. Onto an audio track (not MIDI): a new track after it.
  MidiTrack all;
  for (uint8_t ch = 0; ch < 3; ++ch) {
    all.noteOn(0, ch, static_cast<uint8_t>(60 + ch), 100);
    all.noteOff(kFilePpq, ch, static_cast<uint8_t>(60 + ch));
  }
  Imported f0 = roundTrip({all}, 0);
  const TrackId audio = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Audio, {}, "Drums")).tracks.at(0);
  const ops::Edit e0 = d.edit(ops::importMidi(d.doc(), d.store.ids(), f0.notes, optionsFor(f0.file, audio)));
  REQUIRE(e0.tracks.size() == 1);
  CHECK(e0.tracks[0] != audio);
  CHECK(d.track(e0.tracks[0]).name == "song");
  CHECK(d.notesOf(e0.tracks[0]).size() == 3);
}

TEST_CASE("midi import: tempo and signatures only into a project without clips; a signature off a bar moves to the "
          "next bar line") {
  MidiTrack c;
  c.tempo(0, 600000);                // 100 BPM
  c.tempo(4 * kFilePpq, 400000);     // 150 BPM at beat 4
  c.timeSignature(0, 3, 4);          // 3/4
  c.timeSignature(7 * kFilePpq, 7, 8);  // 7/8 at beat 7: bar 2 of 3/4 starts at beat 6 → moves to beat 9
  MidiTrack n;
  n.noteOn(0, 0, 60, 100);
  n.noteOff(10 * kFilePpq, 0, 60);
  Imported f = roundTrip({c, n}, 1);
  Doc d;
  REQUIRE_FALSE(ops::projectHasClips(d.doc()));
  ops::MidiImportReport rep;
  d.edit(ops::importMidi(d.doc(), d.store.ids(), f.notes, optionsFor(f.file, d.keys), &rep));
  CHECK(rep.tempoImported);
  CHECK(rep.signaturesImported);
  CHECK(rep.movedSignatures == 1);
  const Timeline& tl = *d.doc().timeline;
  REQUIRE(tl.tempo.size() == 2);
  CHECK(tl.tempo[0].tick == 0);
  CHECK(tl.tempo[0].bpm == doctest::Approx(100.0));
  CHECK(tl.tempo[1].tick == 4 * kPpq);
  CHECK(tl.tempo[1].bpm == doctest::Approx(150.0));
  REQUIRE(tl.signatures.size() == 2);
  CHECK(tl.signatures[0].bar == 0);
  CHECK(tl.signatures[0].sig == TimeSignature{3, 4});
  CHECK(tl.signatures[1].bar == 3);
  CHECK(tl.signatures[1].sig == TimeSignature{7, 8});
  // The clip ends on a bar line of the imported meter: 10 beats → bar 3 (3/4) ends at beat 9; bar 4 (7/8) at 12.5.
  CHECK(d.track(d.keys).clips[0].clip->length == 9 * kPpq + 7 * kPpq / 2);
  // A second import: the project has clips now; its tempo stays.
  MidiTrack c2;
  c2.tempo(0, 1000000);  // 60 BPM
  Imported g = roundTrip({c2, n}, 1);
  ops::MidiImportReport rep2;
  d.edit(ops::importMidi(d.doc(), d.store.ids(), g.notes, optionsFor(g.file, {}, 0), &rep2));
  CHECK_FALSE(rep2.tempoImported);
  CHECK(rep2.tempoKept);
  CHECK(d.doc().timeline->tempo[0].bpm == doctest::Approx(100.0));
  // Errors: a file without notes.
  Imported empty = roundTrip({c}, 1);
  CHECK_FALSE(ops::importMidi(d.doc(), d.store.ids(), empty.notes, optionsFor(empty.file)).ok());
}
