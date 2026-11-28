// RT.1 (docs/plans/RT.md; pq/document/audio_in.h): an audio track's Audio In — one wire end (Wire::recordInput), its
// source as a value (AudioFrom: None / Ext / Output / Resampling), the one-step replace (ops::setAudioFrom, ops::connect
// onto it), the loop and kind refusals (and the menus' filter AudioInSources), save / load (schema 23), and the
// lowering ignoring the cable until RT.2.
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "pq/document/audio_in.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/racks.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "../support/blocks.h"

using namespace pq::doc;
using pq::Result;

namespace {

const DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// Src (audio: tanh on its path), Rec (audio, the recorder), Keys (MIDI: its instrument), Bus (a group), Verb (a return).
struct Doc {
  DocumentStore store;
  TrackId src, rec, keys, bus, verb;
  DeviceId tanh;
  Doc() {
    store.setDeviceLibrary(&library());
    src = edit(ops::createTrack(doc(), store.ids(), TrackKind::Audio, {}, "Src")).tracks.at(0);
    rec = edit(ops::createTrack(doc(), store.ids(), TrackKind::Audio, {}, "Rec")).tracks.at(0);
    keys = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi, {}, "Keys")).tracks.at(0);
    bus = edit(ops::createTrack(doc(), store.ids(), TrackKind::Group, {}, "Bus")).tracks.at(0);
    verb = edit(ops::createTrack(doc(), store.ids(), TrackKind::Return, {}, "Verb")).tracks.at(0);
    tanh = test_blocks::appendAfter(store, src, "atom:tanh");
  }
  const Project& doc() const { return *store.current(); }
  TrackId master() const { return doc().master->id; }
  ops::Edit edit(const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const size_t before = store.history().cursor();
    REQUIRE(store.dispatch(e->command).ok());
    CHECK(store.history().cursor() == before + (e->empty() ? 0u : 1u));  // one undo step
    return *e;
  }
  void set(const AudioFrom& f, TrackId t = {}) { edit(ops::setAudioFrom(doc(), store.ids(), t.valid() ? t : rec, f, &library())); }
  std::string refused(const AudioFrom& f, TrackId t = {}) {
    const auto e = ops::setAudioFrom(doc(), store.ids(), t.valid() ? t : rec, f, &library());
    return e.ok() ? std::string() : e.error().message;
  }
  size_t audioInWires() const {
    size_t n = 0;
    for (const Wire& w : wiresOf(doc())) n += w.toRecordInput() ? 1u : 0u;
    return n;
  }
  LoweredProject lowered() const {
    LowerOptions lo;
    lo.library = &library().library();
    lo.devices = &library();
    lo.slotOf = [](TrackId) { return 0u; };
    auto r = lower(doc(), lo);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }
};

Wire device(DeviceId d, std::string port = {}) {
  Wire w;
  w.from = d;
  w.port = std::move(port);
  return w;
}
Wire source(TrackId t, std::string port = {}) {
  Wire w;
  w.noteSource = t;
  w.port = std::move(port);
  return w;
}
Wire mixerOut(TrackId t) {
  Wire w;
  w.mixerOut = t;
  return w;
}
Wire onto(Wire start, TrackId t) {
  start.recordInput = t;
  return start;
}

AudioFrom reloaded(const Project& p, TrackId t) {
  auto text = serializeProject(p);
  REQUIRE(text.ok());
  auto back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == p);
  return audioFromOf(*back->project, t);
}

}  // namespace

TEST_CASE("RT.1 AudioFrom: every kind is set in one step, read back by audioFromOf and survives save / load") {
  Doc d;
  CHECK(audioFromOf(d.doc(), d.rec) == AudioFrom::none());
  // A rack on Src: its first chain (a group) and that chain's mixer (a group of role Mixer).
  const DeviceId rackChain = [&] {
    d.edit(ops::addRack(d.doc(), d.store.ids(), d.src, false));
    const std::vector<RackInfo> racks = findRacks(d.doc(), *findTrackPtr(d.doc(), d.src));
    REQUIRE(racks.size() == 1);
    REQUIRE_FALSE(racks[0].chains.empty());
    return racks[0].chains[0].id;
  }();
  DeviceId chainMixer;
  for (const DevicePtr& x : findTrackPtr(d.doc(), d.src)->devices)
    if (x->group == rackChain && rackRole(*x) == RackRole::Mixer) chainMixer = x->id;
  REQUIRE(chainMixer.valid());

  const std::vector<AudioFrom> kinds = {
      AudioFrom::extIn(AudioInput{0, 2}),         AudioFrom::extIn(AudioInput{3, 1}),
      AudioFrom::output(device(d.tanh)),          AudioFrom::output(source(d.src)),
      AudioFrom::output(mixerOut(d.src)),         AudioFrom::output(device(rackChain)),
      AudioFrom::output(device(chainMixer)),      AudioFrom::output(mixerOut(d.bus)),
      AudioFrom::resampling(),                    AudioFrom::none(),
  };
  for (const AudioFrom& f : kinds) {
    CAPTURE(static_cast<int>(f.kind));
    d.set(f);
    CHECK(audioFromOf(d.doc(), d.rec) == f);
    CHECK(d.audioInWires() == (f.kind == AudioFromKind::Output || f.kind == AudioFromKind::Resampling ? 1u : 0u));
    CHECK(findTrackPtr(d.doc(), d.rec)->mixer.audioIn == (f.kind == AudioFromKind::Ext ? f.ext : AudioInput{}));
    CHECK(reloaded(d.doc(), d.rec) == f);
  }
  // The file's words: "audioIn" onto the Audio In, "mixerOut" from a mixer's output (the master's: Resampling).
  d.set(AudioFrom::resampling());
  const auto text = serializeProject(d.doc());
  REQUIRE(text.ok());
  CHECK(text->find("\"audioIn\": " + std::to_string(d.rec.value)) != std::string::npos);
  CHECK(text->find("\"mixerOut\": " + std::to_string(d.master().value)) != std::string::npos);
  CHECK(text->rfind("{\n  \"schemaVersion\": 23,", 0) == 0);
  // An Output from the master's mixer IS Resampling.
  CHECK(audioFromOf(d.doc(), d.rec) == AudioFrom::resampling());
  CHECK(ops::setAudioFrom(d.doc(), d.store.ids(), d.rec, AudioFrom::output(mixerOut(d.master())))->empty());
}

TEST_CASE("RT.1 one source: setAudioFrom and a second cable drawn by ops::connect replace the old source in one undo "
          "step") {
  Doc d;
  d.set(AudioFrom::extIn(AudioInput{0, 2}));
  // Ext. In → a cable: the channels cleared and the wire added, one step; undo brings Ext. In back.
  d.set(AudioFrom::output(device(d.tanh)));
  CHECK(findTrackPtr(d.doc(), d.rec)->mixer.audioIn.none());
  REQUIRE(d.audioInWires() == 1);
  const WireId first = audioInWireOf(d.doc(), d.rec)->id;
  REQUIRE(d.store.undo());
  CHECK(audioFromOf(d.doc(), d.rec) == AudioFrom::extIn(AudioInput{0, 2}));
  CHECK(d.audioInWires() == 0);
  REQUIRE(d.store.redo());
  // Drawing a cable onto the Audio In (the circuit's gesture): it replaces, one step, and reads back as the pick would.
  const ops::Edit drawn = d.edit(ops::connect(d.doc(), d.store.ids(), onto(mixerOut(d.src), d.rec)));
  REQUIRE(d.audioInWires() == 1);
  CHECK(findWire(d.doc(), first) == nullptr);
  REQUIRE(drawn.wires.size() == 1);
  CHECK(audioInWireOf(d.doc(), d.rec)->id == drawn.wires[0]);
  CHECK(audioFromOf(d.doc(), d.rec) == AudioFrom::output(mixerOut(d.src)));
  REQUIRE(d.store.undo());
  CHECK(audioFromOf(d.doc(), d.rec) == AudioFrom::output(device(d.tanh)));
  REQUIRE(d.store.redo());
  // Picking what is already the source: an empty edit. Ext. In replaces the cable; None clears it.
  CHECK(ops::setAudioFrom(d.doc(), d.store.ids(), d.rec, AudioFrom::output(mixerOut(d.src)))->empty());
  d.set(AudioFrom::extIn(AudioInput{1, 1}));
  CHECK(d.audioInWires() == 0);
  d.set(AudioFrom::output(source(d.src)));
  d.set(AudioFrom::none());
  CHECK(d.audioInWires() == 0);
  CHECK(audioFromOf(d.doc(), d.rec) == AudioFrom::none());
  // The raw commands keep the rule: Ext. In while a cable feeds the Audio In, a second cable onto it.
  d.set(AudioFrom::output(device(d.tanh)));
  CHECK_FALSE(d.store.dispatch(SetTrackAudioInput{d.rec, AudioInput{0, 2}}).ok());
  Wire second = onto(source(d.src), d.rec);
  second.id = d.store.ids().wire();
  CHECK_FALSE(d.store.dispatch(AddWire{second}).ok());
  // Only an audio track has an Audio In.
  CHECK(d.refused(AudioFrom::output(device(d.tanh)), d.keys) == kAudioInTrack);
  CHECK(audioFromOf(d.doc(), d.keys) == AudioFrom::none());
  // Deleting the source's track or the recorder takes the cable with it.
  d.edit(ops::deleteTracks(d.doc(), std::vector<TrackId>{d.src}));
  CHECK(d.audioInWires() == 0);
}

TEST_CASE("RT.1 refusals: itself, a loop through outputs / sends / cables / Audio Ins / its group, and starts that are "
          "not audio; the menus' filter agrees; later edits cannot close a loop either") {
  Doc d;
  // Itself: a block, its own source, its own mixer.
  const DeviceId own = test_blocks::appendAfter(d.store, d.rec, "atom:tanh");
  CHECK(d.refused(AudioFrom::output(device(own))) == kAudioInSelf);
  CHECK(d.refused(AudioFrom::output(source(d.rec))) == kAudioInSelf);
  CHECK(d.refused(AudioFrom::output(mixerOut(d.rec))) == kAudioInSelf);
  // Not audio: a MIDI track's notes and note outputs, a value block (a control), a bool output.
  CHECK(d.refused(AudioFrom::output(source(d.keys))) == kAudioInNotAudio);
  CHECK(d.refused(AudioFrom::output(source(d.keys, "hz"))) == kAudioInNotAudio);
  const DeviceId val = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.src, std::string(kValueDevice), DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  CHECK(d.refused(AudioFrom::output(device(val, std::string(kValueOutput)))) == kAudioInNotAudio);
  const DeviceId gt = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.src, "atom:gt", DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  CHECK(d.refused(AudioFrom::output(device(gt))) == kAudioInNotAudio);
  CHECK(ops::connect(d.doc(), d.store.ids(), onto(source(d.keys), d.rec)).error().message == kAudioInNotAudio);
  // A MIDI track's instrument output is audio (Live's Pre FX of a MIDI track).
  const DeviceId inst = findTrackPtr(d.doc(), d.keys)->devices.front()->id;
  CHECK(d.refused(AudioFrom::output(device(inst))).empty());

  // Rec → Bus (its output) → master: Bus and the master are fed by Rec; Resampling is not (the master without Rec).
  REQUIRE(d.store.dispatch(SetTrackOutput{d.rec, Output{OutputKind::Track, d.bus}}).ok());
  const DeviceId busFx = test_blocks::appendAfter(d.store, d.bus, "atom:tanh");
  CHECK(d.refused(AudioFrom::output(device(busFx))) == kAudioInLoop);
  CHECK(d.refused(AudioFrom::output(mixerOut(d.bus))) == kAudioInLoop);
  CHECK(d.refused(AudioFrom::output(source(d.master()))) == kAudioInLoop);
  CHECK(d.refused(AudioFrom::resampling()).empty());
  {
    const AudioInSources menu(d.doc(), d.rec, &library());
    CHECK_FALSE(menu.offersTrack(d.rec));
    CHECK_FALSE(menu.offersTrack(d.bus));
    CHECK_FALSE(menu.offersTrack(d.master()));
    CHECK(menu.offersTrack(d.src));
    CHECK(menu.offersTrack(d.keys));
    CHECK(menu.offersTrack(d.verb));
    CHECK(menu.offersResampling());
    CHECK(menu.allows(AudioFrom::output(device(d.tanh))));
    CHECK(menu.refusal(AudioFrom::output(device(busFx))) == kAudioInLoop);
  }
  // A stored send Rec → Verb: Verb is fed, and Resampling now loops (Verb reaches the master besides Rec's own route).
  REQUIRE(d.store.dispatch(SetSendLevel{d.rec, d.verb, -6.0f}).ok());
  CHECK(d.refused(AudioFrom::output(mixerOut(d.verb))) == kAudioInLoop);
  CHECK(d.refused(AudioFrom::resampling()) == kAudioInLoop);
  REQUIRE(d.store.dispatch(SetSendLevel{d.rec, d.verb, kMinVolumeDb}).ok());
  CHECK(d.refused(AudioFrom::output(mixerOut(d.verb))).empty());
  CHECK(d.refused(AudioFrom::resampling()).empty());
  // A cable from Rec into Src's block: Src is fed.
  Wire mod;
  mod.from = own;
  mod.to = d.tanh;
  mod.param = kMainPort;
  const WireId cable = d.edit(ops::connect(d.doc(), d.store.ids(), mod)).wires.at(0);
  CHECK(d.refused(AudioFrom::output(device(d.tanh))) == kAudioInLoop);
  d.edit(ops::deleteWires(d.doc(), std::vector<WireId>{cable}));
  CHECK(d.refused(AudioFrom::output(device(d.tanh))).empty());
  // An Audio In is a feed too: Rec records Src, so Src cannot record Rec.
  d.set(AudioFrom::output(device(d.tanh)));
  CHECK(d.refused(AudioFrom::output(mixerOut(d.rec)), d.src) == kAudioInLoop);

  // Later edits that would close the loop are refused by the final pass (Rec records Verb's mixer).
  d.set(AudioFrom::output(mixerOut(d.verb)));
  auto send = d.store.dispatch(SetSendLevel{d.rec, d.verb, -12.0f});
  REQUIRE_FALSE(send.ok());
  CHECK(send.error().message.find(kAudioInLoop) != std::string::npos);
  CHECK(d.refused(AudioFrom::output(device(busFx))) == kAudioInLoop);  // (still fed through its output)
  CHECK(audioFromOf(d.doc(), d.rec) == AudioFrom::output(mixerOut(d.verb)));
}

TEST_CASE("RT.1 cables drawn from a device's audio output, a track's mixer out, a rack chain and the master read back "
          "through audioFromOf") {
  Doc d;
  d.edit(ops::addRack(d.doc(), d.store.ids(), d.src, false));
  const DeviceId chain = findRacks(d.doc(), *findTrackPtr(d.doc(), d.src)).at(0).chains.at(0).id;
  struct Case {
    Wire start;
    AudioFrom want;
  };
  const std::vector<Case> cases = {
      {device(d.tanh), AudioFrom::output(device(d.tanh))},
      {mixerOut(d.src), AudioFrom::output(mixerOut(d.src))},
      {device(chain), AudioFrom::output(device(chain))},
      {mixerOut(d.master()), AudioFrom::resampling()},
  };
  for (const Case& c : cases) {
    d.edit(ops::connect(d.doc(), d.store.ids(), onto(c.start, d.rec)));
    CHECK(audioFromOf(d.doc(), d.rec) == c.want);
    CHECK(d.audioInWires() == 1);
  }
}

TEST_CASE("RT.1 the loader checks the Audio In rules; a mixer's output goes only onto an Audio In") {
  Doc d;
  d.set(AudioFrom::output(mixerOut(d.src)));
  const auto text = serializeProject(d.doc());
  REQUIRE(text.ok());
  // Ext. In beside the cable: two sources.
  std::string two = *text;
  const std::string rec = "\"name\": \"Rec\"";
  REQUIRE(two.find(rec) != std::string::npos);
  const size_t mx = two.find("\"volumeDb\"", two.find(rec));
  REQUIRE(mx != std::string::npos);
  two.insert(mx, "\"audioIn\": {\"first\": 0, \"channels\": 2}, ");
  auto bad = parseProject(two);
  REQUIRE_FALSE(bad.ok());
  CHECK(bad.error().message.find(kAudioInOneSource) != std::string::npos);
  // A cable from a mixer's output into a block: refused by the command and the file alike.
  Wire w;
  w.mixerOut = d.src;
  w.to = d.tanh;
  CHECK_FALSE(ops::connect(d.doc(), d.store.ids(), w).ok());
  w.id = d.store.ids().wire();
  CHECK_FALSE(d.store.dispatch(AddWire{w}).ok());
}

TEST_CASE("RT.1 the lowering ignores an Audio In cable until RT.2 (the same patches; no crash)") {
  Doc d;
  const LoweredProject before = d.lowered();
  d.set(AudioFrom::output(device(d.tanh)));
  const LoweredProject after = d.lowered();
  REQUIRE(before.patches.size() == after.patches.size());
  for (size_t i = 0; i < before.patches.size(); ++i) CHECK(patchText(*before.patches[i]) == patchText(*after.patches[i]));
  CHECK(before.graph.wires.size() == after.graph.wires.size());
  CHECK_NOTHROW((void)lowerParams(d.doc(), false, library()));
  CHECK_NOTHROW((void)wireStates(d.doc(), library()));
  d.set(AudioFrom::resampling());
  const LoweredProject resampled = d.lowered();
  for (size_t i = 0; i < before.patches.size(); ++i) CHECK(patchText(*before.patches[i]) == patchText(*resampled.patches[i]));
}
