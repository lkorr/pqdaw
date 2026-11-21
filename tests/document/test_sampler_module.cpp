// SM.3 (docs/plans/SM.md "SM.3"; pq/document/modules.h "the Sampler"; edit_ops.h setSamplerSample):
//   group    an instrument group around meta:sampler: voice.key into the engine's key, gateOut into the Envelope's
//            gate (a Bool input), engine → Filter (off) → Amp ← Envelope → Out, the LFO (off) on the Filter's Frequency;
//            the panel's bindings resolve inside it; spawned through the catalogue key `module:sampler`; it lowers, every
//            wire lowered or (into / out of a switched-off block) inactive
//   sample   setSamplerSample: data (sample + sorted slices) and root in one undo step that round-trips save / load;
//            no root given: 261.63 Hz; the engine itself or its group; refusals
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/midi_key.h"
#include "pq/document/meta.h"
#include "pq/document/model.h"
#include "pq/document/modules.h"
#include "pq/document/panel.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/meta/sampler.h"

using namespace pq::doc;

namespace {

const DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

struct Doc {
  DocumentStore store;
  TrackId track;
  Doc() { track = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi, {}, "M")).tracks.at(0); }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const pq::Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  // The new MIDI track's default instrument deleted: the path empty, so the Sampler takes the notes.
  void clearHead() {
    std::vector<DeviceId> heads;
    for (const Device* x : mainPath(doc(), *findTrackPtr(doc(), track)).blocks) heads.push_back(x->id);
    if (!heads.empty()) edit(ops::deleteDevices(doc(), heads));
  }
  const Device& dev(DeviceId d) const { return *findDevice(doc(), d).device; }
  SampleId addSample() {
    SampleRef s;
    s.id = store.ids().sample();
    s.path = "s.wav";
    s.frames = 48000;
    s.rate = 48000.0;
    s.channels = 2;
    s.displayName = "s.wav";
    REQUIRE(store.dispatch(Command{AddSample{s}}).ok());
    return s.id;
  }
  const Wire* wire(DeviceId from, std::string_view port, DeviceId to, std::string_view param) const {
    for (const Wire& w : wiresOf(doc()))
      if (w.from == from && w.port == port && w.to == to && w.param == param) return &w;
    return nullptr;
  }
};

double rootHz(const Device& engine) {
  const pq::meta::Type* t = pq::meta::findType("sampler");
  REQUIRE(t != nullptr);
  return metaParamValue(engine, "", *t, static_cast<size_t>(t->paramIndex("root")));
}

}  // namespace

TEST_CASE("SM.3 Sampler group: its blocks and wires, the panel, spawned by `module:sampler`, it lowers") {
  const ModuleInfo* m = findModule("sampler");
  REQUIRE(m != nullptr);
  CHECK(m->kind == ModuleKind::Sampler);
  CHECK(m->instrument);
  CHECK(moduleEngineType(ModuleKind::Sampler).empty());
  Doc d;
  pq::doc::IdSource ids;
  SamplerRoles r;
  const BlockSet set = samplerBlocks(ids, {}, &r);
  CHECK(set.devices.front()->id == r.group);
  // Spawned from the catalogue at the notes (what the browser / node menu paste).
  d.clearHead();
  const DeviceId g = test_blocks::addModule(d.store, d.track, "sampler", 0);
  CHECK(d.dev(g).name == "Sampler");
  CHECK(isInstrumentGroup(d.doc(), g));
  DeviceId engine, key, amp, filter, freq, env, envGroup, lfo;
  forEachDevice(d.doc(), [&](const Device& x, const Track&) {
    if (x.patch == "meta:sampler") engine = x.id;
    if (x.patch == "atom:voice.key") key = x.id;
    if (x.patch == "atom:mul" && x.name == "Amp") amp = x.id;
    if (x.name == "Filter") filter = x.id;
    if (x.patch == "meta:envelope") {
      env = x.id;
      envGroup = x.group;
    }
    if (x.name == "LFO" && x.group == g) lfo = x.id;
  });
  forEachDevice(d.doc(), [&](const Device& x, const Track&) {
    if (x.group == filter && bandRole(x) == BandRole::Frequency) freq = x.id;
  });
  for (const DeviceId x : {engine, key, amp, filter, freq, env, envGroup, lfo}) REQUIRE(x.valid());
  for (const DeviceId x : {engine, key, amp, filter, envGroup, lfo}) CHECK(d.dev(x).group == g);
  CHECK_FALSE(d.dev(filter).active);
  CHECK_FALSE(d.dev(lfo).active);
  CHECK(rootHz(d.dev(engine)) == doctest::Approx(pq::meta::kSamplerDefaultRoot).epsilon(1e-6));
  CHECK(d.wire(g, kGroupInPort, engine, "hz") != nullptr);
  CHECK(d.wire(key, "out", engine, "key") != nullptr);
  CHECK(d.wire(engine, "out", filter, kMainPort) != nullptr);
  CHECK(d.wire(filter, kMainPort, amp, "a") != nullptr);
  CHECK(d.wire(envGroup, kMainPort, amp, "b") != nullptr);
  CHECK(d.wire(amp, "out", g, kGroupOutPort) != nullptr);
  CHECK(d.wire(engine, "gateOut", env, "gate") != nullptr);  // a gate input: the lowering compares > 0
  CHECK(d.wire(envGroup, kGroupInPort, env, "in") == nullptr);  // the envelope itself (its In unplugged)
  const Wire* mod = nullptr;
  for (const Wire& w : wiresOf(d.doc()))
    if (w.to == freq && w.param == kValueParam) mod = &w;
  REQUIRE(mod != nullptr);
  CHECK(d.dev(mod->from).group == lfo);  // from the LFO's jack
  CHECK(mod->law == WireLaw::Travel);
  REQUIRE(d.dev(g).hasPanel());
  CHECK(bindingsProblem(findDevice(d.doc(), g).track->devices, g, *d.dev(g).panel) == "");
  for (const WireState& s : wireStates(d.doc(), library())) {
    CAPTURE(s.id.value);
    CAPTURE(static_cast<int>(s.status));
    CAPTURE(static_cast<int>(s.reason));
    for (const Wire& w : wiresOf(d.doc()))
      if (w.id == s.id && s.status != WireStatus::Lowered && s.status != WireStatus::Inactive)
        MESSAGE(d.dev(w.from).name << ":" << w.port << " -> " << (w.to.valid() ? d.dev(w.to).name : std::string("mixer")) << ":" << w.param);
    CHECK((s.status == WireStatus::Lowered || s.status == WireStatus::Inactive));
  }
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.clipSlotOf = [](TrackId) { return 0u; };
  lo.metaSlotOf = [](DeviceId, std::string_view, uint32_t) { return 3u; };
  auto low = lower(d.doc(), lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
}

TEST_CASE("SM.3 setSamplerSample: data, slices and root in one undo step; save / load round trip; refusals") {
  Doc d;
  d.clearHead();
  const DeviceId g = test_blocks::addModule(d.store, d.track, "sampler", 0);
  DeviceId engine;
  forEachDevice(d.doc(), [&](const Device& x, const Track&) {
    if (x.patch == "meta:sampler") engine = x.id;
  });
  REQUIRE(engine.valid());
  const SampleId s = d.addSample();
  const Snapshot before = d.store.current();
  const size_t cursor = d.store.history().cursor();
  d.edit(ops::setSamplerSample(d.doc(), g, s, 440.0, {24000.0f, 0.0f, 12000.0f, 12000.0f}));
  CHECK(d.store.history().cursor() == cursor + 1);
  const Snapshot after = d.store.current();
  const MetaInstance* in = d.dev(engine).metaAt("");
  REQUIRE(in != nullptr);
  CHECK(pq::meta::samplerSample(in->data.get()) == s.value);
  CHECK(pq::meta::samplerSlices(in->data.get()) == std::vector<float>{0.0f, 12000.0f, 24000.0f});
  CHECK(rootHz(d.dev(engine)) == doctest::Approx(440.0).epsilon(1e-6));
  REQUIRE(d.store.undo());
  CHECK(*d.store.current() == *before);
  REQUIRE(d.store.redo());
  CHECK(*d.store.current() == *after);
  pq::Result<std::string> text = serializeProject(d.doc());
  REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));
  pq::Result<LoadedProject> back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == d.doc());
  // Not analysed: the root is middle C (unpitched); the engine itself takes it too.
  d.edit(ops::setSamplerSample(d.doc(), engine, s, std::nullopt, {}));
  CHECK(rootHz(d.dev(engine)) == doctest::Approx(pq::meta::kSamplerDefaultRoot).epsilon(1e-6));
  CHECK(pq::meta::samplerSlices(d.dev(engine).metaAt("")->data.get()).empty());
  // Refusals: not a sampler, a missing sample, a bad slice.
  const DeviceId vca = test_blocks::addModule(d.store, d.track, "vca", ops::kOffPath);
  CHECK_FALSE(ops::setSamplerSample(d.doc(), vca, s, 440.0, {}).ok());
  CHECK_FALSE(ops::setSamplerSample(d.doc(), g, SampleId{999}, 440.0, {}).ok());
  CHECK_FALSE(ops::setSamplerSample(d.doc(), g, s, 440.0, {-1.0f}).ok());
}

// ---- SM.12: slicing, Slice to Notes ------------------------------------------------------------------------------------

TEST_CASE("SM.12 sliceStarts: transient by sensitivity, beat through markers / sampleBpm, region, manual") {
  using ops::SliceBy;
  using ops::SliceSpec;
  const std::vector<double> onsets = {4800.0, 12000.0, 30000.0};
  const std::vector<float> strengths = {6.0f, 41.0f, 12.0f};
  SliceSpec s;
  s.by = SliceBy::Transient;
  s.sensitivity = 1.0;  // every detected onset
  CHECK(ops::sliceStarts(s, 48000, 48000.0, onsets, strengths, {}, 120.0) == std::vector<float>{0.0f, 4800.0f, 12000.0f, 30000.0f});
  s.sensitivity = 0.5;  // threshold 5 · 2^1.5 ≈ 14.1
  CHECK(ops::sliceStarts(s, 48000, 48000.0, onsets, strengths, {}, 120.0) == std::vector<float>{0.0f, 12000.0f});
  s.sensitivity = 0.0;  // threshold 40
  CHECK(ops::sliceStarts(s, 48000, 48000.0, onsets, strengths, {}, 120.0) == std::vector<float>{0.0f, 12000.0f});
  // Beat: sampleBpm 120 at 48 kHz: 24000 frames a beat.
  s = {};
  s.by = SliceBy::Beat;
  s.beats = 1.0;
  CHECK(ops::sliceStarts(s, 96000, 48000.0, {}, {}, {}, 120.0) == std::vector<float>{0.0f, 24000.0f, 48000.0f, 72000.0f});
  s.beats = 0.5;
  CHECK(ops::sliceStarts(s, 48000, 48000.0, {}, {}, {}, 120.0) == std::vector<float>{0.0f, 12000.0f, 24000.0f, 36000.0f});
  // Through two markers: beat 0 at frame 0, beat 2 at frame 30000 (15000 frames a beat, then that slope on).
  const std::vector<float> markers = {0.0f, 0.0f, 30000.0f, 2.0f};
  s.beats = 1.0;
  CHECK(ops::sliceStarts(s, 48000, 48000.0, {}, {}, markers, 120.0) == std::vector<float>{0.0f, 15000.0f, 30000.0f, 45000.0f});
  // Region: n equal parts.
  s = {};
  s.by = SliceBy::Region;
  s.count = 4;
  CHECK(ops::sliceStarts(s, 48000, 48000.0, {}, {}, {}, 120.0) == std::vector<float>{0.0f, 12000.0f, 24000.0f, 36000.0f});
  s.by = SliceBy::Manual;
  CHECK(ops::sliceStarts(s, 48000, 48000.0, {}, {}, {}, 120.0).empty());
}

TEST_CASE("SM.12 sliceSampler: one undo step, the way kept in the data (no recompile); Manual keeps the slices") {
  Doc d;
  d.clearHead();
  const DeviceId g = test_blocks::addModule(d.store, d.track, "sampler", 0);
  const DeviceId engine = ops::samplerEngineOf(d.doc(), g);
  REQUIRE(engine.valid());
  CHECK(ops::samplerEngineOf(d.doc(), engine) == engine);
  CHECK_FALSE(ops::samplerEngineOf(d.doc(), DeviceId{999999}).valid());
  CHECK_FALSE(ops::sliceSampler(d.doc(), g, {}).ok());  // no sample yet
  const SampleId s = d.addSample();
  d.edit(ops::setSamplerSample(d.doc(), g, s, std::nullopt, {}));
  const size_t cursor = d.store.history().cursor();
  ops::SliceSpec spec;
  spec.by = ops::SliceBy::Region;
  spec.count = 6;
  d.edit(ops::sliceSampler(d.doc(), g, spec));
  CHECK(d.store.history().cursor() == cursor + 1);
  const pq::meta::DataPtr data = d.dev(engine).metaAt("")->data;
  CHECK(pq::meta::samplerSlices(data.get()).size() == 6);
  CHECK(data->field("sliceBy")->number == 3.0);
  CHECK(data->field("sliceCount")->number == 6.0);
  spec.by = ops::SliceBy::Manual;
  d.edit(ops::sliceSampler(d.doc(), g, spec));
  CHECK(pq::meta::samplerSlices(d.dev(engine).metaAt("")->data.get()).size() == 6);
  CHECK(d.dev(engine).metaAt("")->data->field("sliceBy")->number == 0.0);
}

TEST_CASE("SM.12 sliceToNotes: a clip at the marker, a note per slice on keys 36 + n, timed in seconds (warp Off, tempo "
          "map) or beats (warp on); one undo step; refusals") {
  Doc d;
  d.clearHead();
  const DeviceId g = test_blocks::addModule(d.store, d.track, "sampler", 0);
  const DeviceId engine = ops::samplerEngineOf(d.doc(), g);
  CHECK_FALSE(ops::sliceToNotes(d.doc(), d.store.ids(), g, 0).ok());  // no sample
  const SampleId s = d.addSample();  // 48000 frames at 48 kHz: 1 s
  d.edit(ops::setSamplerSample(d.doc(), g, s, std::nullopt, {6000.0f, 18000.0f, 42000.0f}));
  const Ticks at = 4 * pq::kPpq;
  const size_t cursor = d.store.history().cursor();
  const ops::Edit e = d.edit(ops::sliceToNotes(d.doc(), d.store.ids(), g, at));
  CHECK(d.store.history().cursor() == cursor + 1);
  const Track& t = *findTrackPtr(d.doc(), d.track);
  REQUIRE(t.clips.size() == 1);
  REQUIRE(t.clips.front().clip != nullptr);
  const NoteClip& c = *t.clips.front().clip;
  CHECK(c.start == at);
  std::vector<Note> notes;
  for (const Note& n : c.notes) notes.push_back(n);
  REQUIRE(notes.size() == 3);
  // 120 BPM: 1 s = 2 beats. Slices at 0.125, 0.375, 0.875 s: notes at 0, 0.5, 1.5 beats; lengths to the next / the end.
  const DegreeKeyMap keys(nullptr);
  const double beat = static_cast<double>(pq::kPpq);
  const double starts[3] = {0.0, 0.5, 1.5}, lens[3] = {0.5, 1.0, 0.25};
  for (size_t i = 0; i < 3; ++i) {
    CAPTURE(i);
    CHECK(static_cast<double>(notes[i].start) == doctest::Approx(starts[i] * beat).epsilon(1e-9));
    CHECK(static_cast<double>(notes[i].length) == doctest::Approx(lens[i] * beat).epsilon(1e-9));
    REQUIRE(notes[i].pitch.kind() == pq::tuning::PitchKind::Degree);
    CHECK(keys.key(notes[i].pitch.degree()) == 36 + static_cast<int64_t>(i));
  }
  // Warp on (Beats) with sampleBpm 60: 1 beat = 48000 frames; notes in content beats.
  d.edit(ops::setMetaSetting(d.doc(), engine, "", "sampler", "warp", 2.0));
  d.edit(ops::setMetaParamValue(d.doc(), engine, "", "sampler", "sampleBpm", 60.0));
  d.edit(ops::sliceToNotes(d.doc(), d.store.ids(), g, 0));
  const Track& t2 = *findTrackPtr(d.doc(), d.track);
  const NoteClip* c2 = nullptr;
  for (const ClipSlot& x : t2.clips)
    if (x.clip != nullptr && x.clip->start == 0) c2 = x.clip.get();
  REQUIRE(c2 != nullptr);
  std::vector<Note> n2(c2->notes.begin(), c2->notes.end());
  REQUIRE(n2.size() == 3);
  CHECK(static_cast<double>(n2[1].start) == doctest::Approx(0.25 * beat).epsilon(1e-9));  // 12000 frames later
  CHECK(static_cast<double>(n2[2].start) == doctest::Approx(0.75 * beat).epsilon(1e-9));
  // Refused on an audio track.
  const TrackId audio = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Audio, {}, "A")).tracks.at(0);
  const DeviceId ga = test_blocks::addModule(d.store, audio, "sampler", 0);
  d.edit(ops::setSamplerSample(d.doc(), ga, s, std::nullopt, {0.0f}));
  CHECK_FALSE(ops::sliceToNotes(d.doc(), d.store.ids(), ga, 0).ok());
}
