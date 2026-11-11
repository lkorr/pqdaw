// T5.11: user-library files (pq/document/library_files.h) and the edits that bring them into a project
// (ops::replaceDevice = hot-swap, ops::placeClip): save → load is deep-equal for every kind (device, nested rack,
// EDO / MOS / Scala tunings exact, note clip, audio clip); the category key sits at the top of a preset (Q153);
// strict reading; hot-swap keeps the id, the position, wires and mappings onto params that still exist (one undo step).
// (GU.11: user devices are gone.)

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/document/library_files.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/tuning/scala.h"

using namespace pq::doc;
using pq::Result;
namespace fs = std::filesystem;

namespace {

template <class T>
T must(Result<T> r) {
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return std::move(*r);
}

struct Lib {
  DocumentStore store;
  TrackId midi, audio, fx;
  DeviceId inst;
  Lib() {
    midi = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi)).tracks.at(0);
    audio = edit(ops::createTrack(doc(), store.ids(), TrackKind::Audio)).tracks.at(0);
    fx = edit(ops::createTrack(doc(), store.ids(), TrackKind::Return)).tracks.at(0);
    inst = findTrackPtr(doc(), midi)->devices.at(0)->id;
  }
  const Project& doc() const { return *store.current(); }
  const Device& device(DeviceId id) const { return *findDevice(doc(), id).device; }
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const size_t before = store.history().cursor();
    const pq::Status st = store.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    CHECK(store.history().cursor() == before + (e->empty() ? 0u : 1u));
    return *e;
  }
  DeviceId add(TrackId t, std::string patch, DeviceKind k = DeviceKind::AudioEffect) {
    return edit(ops::addDevice(doc(), store.ids(), t, patch, k, kAppend)).devices.at(0);
  }
};

// Deep equality of two devices up to their ids (a loaded preset gets fresh ids).
bool sameUpToIds(const Device& a, const Device& b) {
  if (a.patch != b.patch || a.kind != b.kind || a.active != b.active || a.params != b.params || a.settings != b.settings)
    return false;
  return true;
}

}  // namespace

TEST_CASE("library files: a device preset round-trips exactly; its kind is in the first bytes") {
  Lib l;
  const DeviceId sat = l.add(l.midi, std::string(kValueDevice));  // (GU.11: a value block)
  l.edit(ops::setDeviceParam(l.doc(), sat, std::string(kValueParam), 0.75));  // GU.1: a knob position
  l.edit(ops::setDevicesActive(l.doc(), std::span<const DeviceId>(&sat, 1), false));
  const std::string text = must(serializePreset(l.device(sat), "Warm"));
  CHECK(text.find("\"pqpreset\"") < 64);
  const size_t kind = text.find("\"kind\": \"audioEffect\"");
  CHECK(kind != std::string::npos);
  CHECK(kind < 256);
  const PresetFile f = must(parsePreset(text, "Warm.pqpreset"));
  CHECK(f.name == "Warm");
  CHECK(*f.device == l.device(sat));  // ids included: the file keeps them, the loader re-ids
  // Strict: unknown keys, a wrong format, a newer schema, two devices.
  std::string bad = text;
  bad.replace(bad.find("\"pqpreset\""), 10, "\"pqclip\"");
  CHECK_FALSE(parsePreset(bad).ok());
  bad = text;
  bad.insert(bad.find("\"name\""), "\"extra\": 1, ");
  CHECK_FALSE(parsePreset(bad).ok());
  bad = text;
  bad.replace(bad.find("\"schemaVersion\": ") + 17, 1, "99");
  const auto newer = parsePreset(bad);
  REQUIRE_FALSE(newer.ok());
  CHECK(newer.error().message.find("newer") != std::string::npos);
}

TEST_CASE("library files: tunings and clips round-trip exactly") {
  // Tunings: EDO with a ratio equave, MOS, Scala with a mapping (exact ratios and cents).
  std::vector<TuningDef> defs;
  {
    TuningDef e;
    e.system = must(pq::tuning::Edo::make(13, pq::tuning::Rational(3, 1)));
    e.referenceHz = 220.5;
    defs.push_back(e);
    TuningDef m;
    pq::tuning::MosParams mp;
    mp.generator = 701.955;
    mp.n = 5;
    mp.rotation = 2;
    mp.modmos = {0.0, 1.0};
    m.system = mp;
    defs.push_back(m);
    const auto scl = must(pq::tuning::parseScl("! x.scl\nseven\n3\n9/8\n386.31371\n2/1\n"));
    defs.push_back(must(makeScaleDef(scl, std::nullopt, 261.6255653)));
  }
  for (const TuningDef& def : defs) {
    const TuningFile t{"T", Colour{0x12AB34}, def};
    const TuningFile back = must(parseTuning(must(serializeTuning(t))));
    CHECK(back.name == "T");
    CHECK(back.colour == t.colour);
    CHECK(back.def == def);
  }

  // A note clip (TU.3: its named tuning's definition travels as the file's "tuning"; the clip is written Inherit) and an
  // audio clip with its sample.
  Lib l;
  NoteClip c;
  c.id = ClipId{5};
  c.name = "Riff";
  c.length = pq::kTicksPerWhole;
  c.tuning = TuningAssignment::mainTuning((*l.doc().tunings)[0]->id);
  std::vector<Note> notes(3);
  for (size_t i = 0; i < notes.size(); ++i) {
    notes[i].id = NoteId{static_cast<uint32_t>(10 + i)};
    notes[i].start = static_cast<pq::Ticks>(i) * pq::kPpq;
    notes[i].length = pq::kPpq / 2;
    notes[i].pitch = pq::tuning::PitchId::fromRatio(pq::tuning::Rational(3 + static_cast<int64_t>(i), 2));
  }
  c.notes = must(NoteSeq::fromNotes(notes));
  ClipFile cf{"Riff", ClipSlot{c.id, std::make_shared<const NoteClip>(c), nullptr}, std::nullopt};
  const ClipFile back = must(parseClip(must(serializeClip(cf, &l.doc()))));
  REQUIRE(back.clip.clip);
  NoteClip expect = c;
  expect.tuning = TuningAssignment::inherit();
  CHECK(*back.clip.clip == expect);
  REQUIRE(back.tuning.has_value());
  CHECK(*back.tuning == (*l.doc().tunings)[0]->def);

  AudioClip a;
  a.id = ClipId{6};
  a.name = "Loop";
  a.sample = SampleId{3};
  a.warp = AudioWarp::RePitch;
  a.sourceBpm = 97.5;
  a.loop = ClipLoop{true, 0, pq::kPpq * 2};
  a.gainDb = -4.5f;
  a.reverse = true;
  a.fadeIn = AudioFade{pq::kPpq / 8, 0.25f};
  a.transpose = AudioTranspose{TransposeUnit::Ratio, 0.0, 0, pq::tuning::Rational(3, 2)};
  SampleRef s;
  s.id = SampleId{3};
  s.path = "Samples/Loop.wav";
  s.contentHash = 0x0123456789abcdefull;
  s.size = 1234;
  s.frames = 600;
  s.rate = 44100.0;
  s.channels = 2;
  s.displayName = "Loop";
  const ClipFile af{"Loop", ClipSlot{a.id, nullptr, std::make_shared<const AudioClip>(a)}, s};
  const ClipFile aback = must(parseClip(must(serializeClip(af))));
  REQUIRE(aback.clip.audio);
  CHECK(*aback.clip.audio == a);
  REQUIRE(aback.sample);
  CHECK(*aback.sample == s);
  // An audio clip without its sample is refused on both ends.
  CHECK_FALSE(serializeClip(ClipFile{"x", af.clip, std::nullopt}).ok());
}

TEST_CASE("library files: paths relative to the library file's folder") {
  CHECK(relativeLibraryPath("C:/Lib/Clips", "C:/Lib/Clips/Samples/a.wav") == "Samples/a.wav");
  CHECK(relativeLibraryPath("C:/Lib/Clips", "D:/x/a.wav") == "D:/x/a.wav");
  CHECK(resolveLibraryPath("C:/Lib/Clips", "Samples/a.wav") == "C:/Lib/Clips/Samples/a.wav");
  CHECK(resolveLibraryPath("C:/Lib/Clips", "D:\\x\\a.wav") == "D:/x/a.wav");
  const fs::path dir = fs::temp_directory_path() / "pq_library_files_io";
  std::error_code ec;
  fs::remove_all(dir, ec);
  const std::string p = (dir / "sub" / "a.pqtuning").generic_string();
  REQUIRE(writeTextFileAtomic(p, "hello").ok());
  CHECK(must(readTextFile(p)) == "hello");
  CHECK(readTextFile((dir / "none").generic_string()).error().code == pq::ErrorCode::NotFound);
  fs::remove_all(dir, ec);
}

TEST_CASE("library: a clip placed from the library gets fresh ids and overwrites; an audio clip brings its sample") {
  Lib l;
  NoteClip c;
  c.id = ClipId{1};
  c.length = pq::kTicksPerWhole;
  Note n;
  n.id = NoteId{2};
  n.length = pq::kPpq;
  n.pitch = pq::tuning::PitchId::fromDegree(0, 0);
  c.notes = must(NoteSeq::fromNotes({n}));
  const ClipSlot slot{c.id, std::make_shared<const NoteClip>(c), nullptr};
  const ClipRef first = l.edit(ops::placeClip(l.doc(), l.store.ids(), l.midi, slot, 0)).clips.at(0);
  const ClipRef second = l.edit(ops::placeClip(l.doc(), l.store.ids(), l.midi, slot, pq::kPpq * 2)).clips.at(0);
  CHECK(first.clip != second.clip);
  const Track& t = *findTrackPtr(l.doc(), l.midi);
  REQUIRE(t.clips.size() == 2);  // the second overwrote the first's second half
  CHECK(t.clips[0].end() == pq::kPpq * 2);
  CHECK_FALSE(ops::placeClip(l.doc(), l.store.ids(), l.audio, slot, 0).ok());

  AudioClip a;
  a.id = ClipId{4};
  a.sample = SampleId{900};
  a.seconds = 0.5;
  SampleRef s;
  s.id = l.store.ids().sample();
  s.path = "C:/x/a.wav";
  s.frames = 24000;
  s.rate = 48000.0;
  s.displayName = "a";
  const ClipSlot as{a.id, nullptr, std::make_shared<const AudioClip>(a)};
  const ClipRef ar = l.edit(ops::placeClip(l.doc(), l.store.ids(), l.audio, as, pq::kPpq, s)).clips.at(0);
  REQUIRE(findSample(l.doc(), s.id));
  const Track& at = *findTrackPtr(l.doc(), l.audio);
  REQUIRE(at.clips.size() == 1);
  CHECK(at.clips[0].id == ar.clip);
  CHECK(at.clips[0].audio->sample == s.id);
  CHECK(at.clips[0].audio->length == pq::kPpq);  // 0.5 s at 120 BPM, re-derived at its new start
  CHECK_FALSE(ops::placeClip(l.doc(), l.store.ids(), l.midi, as, 0, s).ok());
}
