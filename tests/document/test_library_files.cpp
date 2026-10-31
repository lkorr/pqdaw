// T5.11: user-library files (pq/document/library_files.h) and the edits that bring them into a project
// (ops::replaceDevice = hot-swap, ops::placeClip): save → load is deep-equal for every kind (device, nested rack, user
// device, EDO / MOS / Scala tunings exact, note clip, audio clip); the category key sits at the top of a preset (Q153);
// strict reading; hot-swap keeps the id, the position, wires and mappings onto params that still exist (one undo step);
// a project using a library user device needs no library to open.

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
  DeviceId add(TrackId t, const char* patch, DeviceKind k = DeviceKind::AudioEffect, ChainId chain = {}) {
    return edit(ops::addDevice(doc(), store.ids(), t, patch, k, kAppend, chain)).devices.at(0);
  }
};

// Deep equality of two devices up to their ids (a loaded preset gets fresh ids).
bool sameUpToIds(const Device& a, const Device& b) {
  if (a.patch != b.patch || a.kind != b.kind || a.active != b.active || a.params != b.params || a.settings != b.settings)
    return false;
  if ((a.source == nullptr) != (b.source == nullptr) || (a.source && a.source->text != b.source->text)) return false;
  if ((a.rack == nullptr) != (b.rack == nullptr)) return false;
  if (!a.rack) return true;
  const Rack &ra = *a.rack, &rb = *b.rack;
  if (ra.chains.size() != rb.chains.size() || ra.mappings.size() != rb.mappings.size() || ra.macroNames != rb.macroNames ||
      ra.variations != rb.variations || ra.visibleMacros != rb.visibleMacros || ra.keyUnit != rb.keyUnit)
    return false;
  for (size_t i = 0; i < ra.chains.size(); ++i) {
    const Chain &ca = *ra.chains[i], &cb = *rb.chains[i];
    if (ca.name != cb.name || ca.volumeDb != cb.volumeDb || ca.pan != cb.pan || ca.devices.size() != cb.devices.size() ||
        ca.key != cb.key || ca.velocity != cb.velocity || ca.select != cb.select)
      return false;
    for (size_t j = 0; j < ca.devices.size(); ++j)
      if (!sameUpToIds(*ca.devices[j], *cb.devices[j])) return false;
  }
  for (size_t i = 0; i < ra.mappings.size(); ++i)
    if (ra.mappings[i].macro != rb.mappings[i].macro || ra.mappings[i].param != rb.mappings[i].param ||
        ra.mappings[i].min != rb.mappings[i].min || ra.mappings[i].max != rb.mappings[i].max)
      return false;
  return true;
}

}  // namespace

TEST_CASE("library files: a device preset round-trips exactly; its kind is in the first bytes") {
  Lib l;
  const DeviceId sat = l.add(l.midi, "saturator");
  l.edit(ops::setDeviceParam(l.doc(), sat, "drive", 7.5));
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

TEST_CASE("library files: a nested rack preset keeps chains, mappings, variations and user-device texts") {
  Lib l;
  const DeviceId sat = l.add(l.midi, "saturator");
  const DeviceId eq = l.add(l.midi, "eq_eight");
  const DeviceId ids[] = {sat, eq};
  const DeviceId rack = l.edit(ops::groupDevices(l.doc(), l.store.ids(), ids)).devices.at(0);
  const DeviceId inner[] = {eq};
  const DeviceId nested = l.edit(ops::groupDevices(l.doc(), l.store.ids(), inner)).devices.at(0);
  l.edit(ops::mapMacro(l.doc(), l.store.ids(), rack, 0, sat, "drive"));
  l.edit(ops::mapMacro(l.doc(), l.store.ids(), nested, 1, eq, "output"));
  l.edit(ops::renameMacro(l.doc(), rack, 0, "Heat"));
  l.edit(ops::storeVariation(l.doc(), rack, "A"));
  l.edit(ops::editDevicePatch(l.doc(), sat, "// forked\n" + std::string("patch saturator {}\n")));
  const Device& r = l.device(rack);
  const std::string text = must(serializePreset(r, "Bus"));
  CHECK(text.find("\"rack\"") < 4096);  // the browser sniffs Racks from the first 4 KiB (Q153)
  const PresetFile f = must(parsePreset(text));
  CHECK(*f.device == r);
  // Loaded into a project: fresh ids, the same content; mappings follow the copies.
  const DeviceId pasted = l.edit(ops::pasteDevices(l.doc(), l.store.ids(), l.fx, std::span<const DevicePtr>(&f.device, 1))).devices.at(0);
  CHECK(pasted != rack);
  CHECK(sameUpToIds(l.device(pasted), r));
  const Rack& pr = *l.device(pasted).rack;
  REQUIRE(pr.mappings.size() == 1);
  CHECK(deviceContains(l.device(pasted), pr.mappings[0].device));
}

TEST_CASE("library files: user devices, tunings and clips round-trip exactly") {
  // A user device (CRLF text, panel).
  UserDeviceFile u;
  u.name = "Grit";
  u.patch = "saturator";
  u.kind = DeviceKind::AudioEffect;
  u.origin = "devices/saturator.pqp";
  u.text = "import \"../lib/x.pqp\"\r\npatch saturator {\r\n}\r\n";
  u.panel = "{\"rows\": []}";
  const std::string ut = must(serializeUserDevice(u));
  CHECK(must(parseUserDevice(ut)) == u);
  u.text = "patch saturator {\n}";  // LF
  CHECK(must(parseUserDevice(must(serializeUserDevice(u)))) == u);
  const DevicePtr d = makeUserDevice(DeviceId{9}, u);
  CHECK(d->source->text == u.text);
  CHECK(d->patch == "saturator");

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

  // A note clip (its main tuning becomes a private one) and an audio clip with its sample.
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
  expect.tuning = TuningAssignment::privateTuning((*l.doc().tunings)[0]->def);
  CHECK(*back.clip.clip == expect);

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

TEST_CASE("library: hot-swap keeps the id, the position, wires and mappings onto params that still exist") {
  Lib l;
  const DeviceId sat = l.add(l.midi, "saturator");
  const DeviceId eq = l.add(l.midi, "eq_eight");
  const DeviceId util = l.add(l.midi, "utility");
  const DeviceId grouped[] = {sat, eq};
  const DeviceId rack = l.edit(ops::groupDevices(l.doc(), l.store.ids(), grouped)).devices.at(0);
  l.edit(ops::mapMacro(l.doc(), l.store.ids(), rack, 0, sat, "drive"));
  l.edit(ops::mapMacro(l.doc(), l.store.ids(), rack, 1, sat, "output"));
  const WireId in = l.edit(ops::connectWire(l.doc(), l.store.ids(), util, "out", sat, "drive")).wires.at(0);
  const WireId gone = l.edit(ops::connectWire(l.doc(), l.store.ids(), util, "out", sat, "output")).wires.at(0);
  const WireId outw = l.edit(ops::connectWire(l.doc(), l.store.ids(), sat, "env", util, "gain")).wires.at(0);
  const DeviceLoc before = findDevice(l.doc(), sat);
  const uint32_t index = before.index;

  // The preset: another Saturator with other values.
  auto preset = std::make_shared<Device>(*makeDevice(DeviceId{77}, "saturator", DeviceKind::AudioEffect));
  preset->params = {{"drive", 3.0}};
  const std::vector<std::string> params = {"drive", "dryWet"};  // "output" is gone in this (pretend) patch
  const size_t steps = l.store.history().cursor();
  l.edit(ops::replaceDevice(l.doc(), l.store.ids(), sat, *preset, params));
  CHECK(l.store.history().cursor() == steps + 1);
  const DeviceLoc after = findDevice(l.doc(), sat);
  REQUIRE(after.device);
  CHECK(after.index == index);
  CHECK(after.rack == findDevice(l.doc(), rack).device);
  CHECK(after.device->params == preset->params);
  CHECK(findWire(l.doc(), in) != nullptr);
  CHECK(findWire(l.doc(), outw) != nullptr);
  CHECK(findWire(l.doc(), gone) == nullptr);
  const Rack& r = *l.device(rack).rack;
  REQUIRE(r.mappings.size() == 1);
  CHECK(r.mappings[0].param == "drive");
  // Undo restores everything.
  REQUIRE(l.store.undo());
  CHECK(findWire(l.doc(), gone) != nullptr);
  CHECK(l.device(rack).rack->mappings.size() == 2);
  // Another kind is refused.
  CHECK_FALSE(ops::replaceDevice(l.doc(), l.store.ids(), sat, *makeDevice(DeviceId{78}, "BasicSynth", DeviceKind::Instrument), params).ok());
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

TEST_CASE("library: a project using a library user device opens without the library") {
  Lib l;
  UserDeviceFile u;
  u.name = "Mine";
  u.patch = "saturator";
  u.kind = DeviceKind::AudioEffect;
  u.origin = "devices/saturator.pqp";
  u.text = "patch saturator {}\n";
  const DevicePtr d = makeUserDevice(DeviceId{1}, u);
  const DeviceId id = l.edit(ops::pasteDevices(l.doc(), l.store.ids(), l.fx, std::span<const DevicePtr>(&d, 1))).devices.at(0);
  const LoadedProject back = must(parseProject(must(serializeProject(l.doc()))));
  const DeviceLoc loc = findDevice(*back.project, id);
  REQUIRE(loc.device);
  REQUIRE(loc.device->source);
  CHECK(loc.device->source->text == u.text);
}
