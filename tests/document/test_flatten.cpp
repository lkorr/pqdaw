// FZ.4 (docs/plans/RB.md D5, "### FZ.4"; edit_ops.h "Render edits"): the document side of Flatten, Bounce to New Track
// and Consolidate on audio — each one undo step that undo restores exactly, the clips placed so that song sample k
// plays file frame k − samples(origin), the flattened track keeping its id, mixer, sends, routing, mixer lanes and the
// cables from other tracks into its mixer; SetTrackKind's refusals; save / load round-trips with the new samples.
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"

using namespace pq::doc;
using pq::Ticks;

namespace {

constexpr double kRate = 48000.0;

struct Doc {
  DocumentStore store;
  TrackId keys, bass, ret;
  Doc() {
    keys = must(ops::createTrack(doc(), store.ids(), TrackKind::Midi, {}, "Keys")).tracks.at(0);
    bass = must(ops::createTrack(doc(), store.ids(), TrackKind::Midi, keys, "Bass")).tracks.at(0);
    ret = must(ops::createTrack(doc(), store.ids(), TrackKind::Return, {}, "Echo")).tracks.at(0);
  }
  const Project& doc() const { return *store.current(); }
  const Track& track(TrackId t) const { return *findTrackPtr(doc(), t); }
  ops::Edit must(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const pq::Status st = store.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    return *e;
  }
  void cmd(const Command& c) {
    const pq::Status st = store.dispatch(c);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  SampleRef sample(std::string name, int64_t frames, uint32_t channels = 2) {
    SampleRef s;
    s.id = store.ids().sample();
    s.path = "samples/frozen/" + name + ".wav";
    s.frames = frames;
    s.rate = kRate;
    s.channels = channels;
    s.size = static_cast<uint64_t>(frames) * channels * 4 + 80;
    s.displayName = std::move(name);
    return s;
  }
  ClipId noteClip(TrackId t, Ticks start, Ticks length) {
    NoteClip c;
    c.id = store.ids().clip();
    c.start = start;
    c.length = length;
    const ClipId id = c.id;
    cmd(AddClip{t, std::make_shared<const NoteClip>(std::move(c)), nullptr, {}});
    return id;
  }
  void roundTrips() {
    auto text = serializeProject(doc());
    REQUIRE(text.ok());
    auto back = parseProject(*text);
    REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
    CHECK(*back->project == doc());
  }
};

const AudioClip& audioAt(const Track& t, size_t i) {
  REQUIRE(i < t.clips.size());
  REQUIRE(t.clips[i].audio != nullptr);
  return *t.clips[i].audio;
}

}  // namespace

TEST_CASE("flatten: a frozen MIDI track becomes an audio track in place, one clip per region; one undo step") {
  Doc d;
  const Ticks bar = 4 * pq::kPpq;
  d.noteClip(d.keys, 0, bar);
  d.noteClip(d.keys, 3 * bar, bar);
  // Its mixer, a send, a routing, a mixer lane and a device lane; a take lane; a cable from Bass's instrument into its
  // mixer (another track's: it stays) and one from its notes to Bass's instrument (its source's: it goes).
  d.cmd(SetTrackVolume{d.keys, -6.0f});
  d.cmd(SetTrackPan{d.keys, 0.25f});
  d.cmd(SetSendLevel{d.keys, d.ret, -12.0f});
  const DeviceId bassInst = mainPath(d.doc(), d.track(d.bass)).blocks.at(0)->id;
  const DeviceId keysInst = mainPath(d.doc(), d.track(d.keys)).blocks.at(0)->id;
  const LaneId volLane = d.store.ids().lane();
  {
    AutomationLane v;
    v.id = volLane;
    v.param = ParamRef::track(ParamKind::TrackVolume, d.keys);
    d.cmd(AddAutomationLane{d.keys, std::make_shared<const AutomationLane>(std::move(v))});
  }
  DeviceId knob;
  for (const DevicePtr& x : d.track(d.keys).devices)
    if (isValueDevice(*x)) knob = x->id;
  REQUIRE(knob.valid());
  {
    AutomationLane l;
    l.id = d.store.ids().lane();
    l.param = ParamRef::device(knob, kValueParam);
    l.paramName = std::string(kValueParam);
    d.cmd(AddAutomationLane{d.keys, std::make_shared<const AutomationLane>(std::move(l))});
  }
  {
    NoteClip c;
    c.id = d.store.ids().clip();
    c.length = bar;
    const ClipSlot slot{c.id, std::make_shared<const NoteClip>(std::move(c)), nullptr};
    d.must(ops::addTakeLane(d.doc(), d.store.ids(), d.keys, std::span<const ClipSlot>(&slot, 1)));
  }
  Wire in;
  in.from = bassInst;
  in.mixer = d.keys;
  const WireId inWire = d.must(ops::connect(d.doc(), d.store.ids(), in)).wires.at(0);
  Wire notes;
  notes.noteSource = d.keys;
  notes.port = "gate";
  notes.to = bassInst;
  notes.param = "gain";
  notes.law = WireLaw::Add;
  (void)d.must(ops::connect(d.doc(), d.store.ids(), notes));
  d.cmd(SetTrackFrozen{d.keys, true});
  const Snapshot before = d.store.current();
  const Track keysBefore = d.track(d.keys);
  const size_t h = d.store.history().cursor();

  const SampleRef s = d.sample("Keys", 5 * 96000);
  const ops::RenderRegion regions[] = {{0, bar + pq::kPpq}, {3 * bar, 4 * bar + 2 * pq::kPpq}};
  const ops::Edit e = d.must(ops::flattenTrack(d.doc(), d.store.ids(), d.keys, s, regions));
  CHECK(d.store.history().cursor() == h + 1);
  CHECK(d.store.history().entries()[h].label == "Flatten");
  const Track& t = d.track(d.keys);
  CHECK(t.id == d.keys);
  CHECK(t.kind == TrackKind::Audio);
  CHECK_FALSE(t.frozen);
  CHECK(t.name == keysBefore.name);
  CHECK(t.colour == keysBefore.colour);
  CHECK(t.mixer == keysBefore.mixer);  // volume, pan, sends, routing
  CHECK(t.devices.empty());
  CHECK(t.takes.empty());
  REQUIRE(t.lanes.size() == 1);  // the mixer lane; the device lane went with its block
  CHECK(t.lanes[0]->id == volLane);
  CHECK(findDevice(d.doc(), keysInst).device == nullptr);
  // The cables: Bass → Keys's mixer stays; Keys's notes → Bass went; a fresh source → mixer link.
  const WireList& wires = wiresOf(d.doc());
  CHECK(std::any_of(wires.begin(), wires.end(), [&](const Wire& w) { return w.id == inWire; }));
  CHECK(std::none_of(wires.begin(), wires.end(), [&](const Wire& w) { return w.noteSource == d.keys && !w.toMixer(); }));
  CHECK(std::count_if(wires.begin(), wires.end(), [&](const Wire& w) {
          return w.noteSource == d.keys && w.mixer == d.keys && w.mainLink();
        }) == 1);
  // The sample and one warp-Off clip per region: tick t plays file frame samples(t).
  REQUIRE(findSample(d.doc(), s.id) != nullptr);
  REQUIRE(t.clips.size() == 2);
  REQUIRE(e.clips.size() == 2);
  const pq::TempoMap& tm = d.doc().timeline->tempoMap;
  for (size_t i = 0; i < 2; ++i) {
    const AudioClip& c = audioAt(t, i);
    CHECK(c.sample == s.id);
    CHECK(c.warp == AudioWarp::Off);
    CHECK(c.start == regions[i].start);
    CHECK(c.start + c.length == regions[i].end);
    CHECK(c.contentStartFrame == static_cast<double>(tm.ticksToSamples(regions[i].start, kRate)));
    CHECK(c.gainDb == 0.0f);
    CHECK(c.name == "Keys");
  }
  d.roundTrips();
  // Undo restores the frozen MIDI track exactly; redo flattens again.
  REQUIRE(d.store.undo());
  CHECK(d.store.current() == before);
  CHECK(*d.store.current() == *before);
  REQUIRE(d.store.redo());
  CHECK(d.track(d.keys).kind == TrackKind::Audio);
}

TEST_CASE("flatten: a frozen audio track keeps its kind; the regions must be sorted, disjoint and inside the render") {
  Doc d;
  const TrackId aud = d.must(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Audio, d.bass, "Vox")).tracks.at(0);
  const SampleRef src = d.sample("take", 96000, 1);
  const ops::NewAudioClip nc{aud, 0, src.id, {}};
  d.must(ops::addAudioClips(d.doc(), d.store.ids(), std::span<const SampleRef>(&src, 1), std::span<const ops::NewAudioClip>(&nc, 1)));
  d.cmd(SetTrackFrozen{aud, true});
  const SampleRef s = d.sample("Vox", 96000);
  const ops::RenderRegion bad[] = {{pq::kPpq, 2 * pq::kPpq}, {0, pq::kPpq}};
  CHECK_FALSE(ops::flattenTrack(d.doc(), d.store.ids(), aud, s, bad).ok());
  const ops::RenderRegion empty[] = {{pq::kPpq, pq::kPpq}};
  CHECK_FALSE(ops::flattenTrack(d.doc(), d.store.ids(), aud, s, empty).ok());
  const ops::RenderRegion one[] = {{pq::kPpq, 4 * pq::kPpq}};
  d.must(ops::flattenTrack(d.doc(), d.store.ids(), aud, s, one));
  const Track& t = d.track(aud);
  CHECK(t.kind == TrackKind::Audio);
  CHECK_FALSE(t.frozen);
  REQUIRE(t.clips.size() == 1);
  CHECK(audioAt(t, 0).sample == s.id);
  CHECK(audioAt(t, 0).contentStartFrame == 24000.0);  // a beat at 120 BPM
  // Return / master / group tracks cannot be flattened.
  CHECK_FALSE(ops::flattenTrack(d.doc(), d.store.ids(), d.ret, d.sample("x", 10), one).ok());
}

TEST_CASE("SetTrackKind: MIDI <-> audio only on an empty, unfrozen track") {
  Doc d;
  CHECK_FALSE(d.store.dispatch(SetTrackKind{d.keys, TrackKind::Audio}).ok());  // its instrument
  CHECK_FALSE(d.store.dispatch(SetTrackKind{d.ret, TrackKind::Audio}).ok());
  const TrackId aud = d.must(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Audio, {}, "A")).tracks.at(0);
  CHECK_FALSE(d.store.dispatch(SetTrackKind{aud, TrackKind::Group}).ok());
  d.cmd(SetTrackAudioInput{aud, AudioInput{2, 2}});
  d.cmd(SetTrackKind{aud, TrackKind::Midi});
  CHECK(d.track(aud).kind == TrackKind::Midi);
  CHECK(d.track(aud).mixer.audioIn.none());  // what only an audio track has is reset
  d.cmd(SetTrackFrozen{aud, true});
  CHECK_FALSE(d.store.dispatch(SetTrackKind{aud, TrackKind::Audio}).ok());
}

TEST_CASE("bounce to new track: an audio track right below with the render, colour and mixer; the source deactivated") {
  Doc d;
  const Ticks bar = 4 * pq::kPpq;
  d.noteClip(d.keys, 0, bar);
  d.cmd(SetTrackColour{d.keys, Colour{0x336699}});
  d.cmd(SetTrackVolume{d.keys, -4.0f});
  d.cmd(SetSendLevel{d.keys, d.ret, -9.0f});
  d.cmd(SetTrackSolo{d.keys, true});
  const TrackId grp = d.must(ops::groupTracks(d.doc(), d.store.ids(), std::vector<TrackId>{d.keys, d.bass}, "G")).tracks.at(0);
  const Snapshot before = d.store.current();
  const size_t h = d.store.history().cursor();
  const SampleRef s = d.sample("Keys Bounce", 96000);
  const ops::RenderRegion r[] = {{bar, 2 * bar}};
  const ops::Edit e = d.must(ops::bounceToNewTrack(d.doc(), d.store.ids(), d.keys, s, bar, r));
  CHECK(d.store.history().cursor() == h + 1);
  CHECK(d.store.history().entries()[h].label == "Bounce to New Track");
  REQUIRE(e.tracks.size() == 1);
  const Track& n = d.track(e.tracks[0]);
  const Track& g = d.track(grp);
  REQUIRE(g.children.size() == 3);
  CHECK(g.children[0]->id == d.keys);
  CHECK(g.children[1]->id == n.id);  // right below
  CHECK(n.kind == TrackKind::Audio);
  CHECK(n.name == "Keys Bounce");
  CHECK(n.colour == Colour{0x336699});
  CHECK(n.mixer.volumeDb == -4.0f);
  CHECK(n.mixer.sends == d.track(d.keys).mixer.sends);
  CHECK(n.mixer.output == d.track(d.keys).mixer.output);
  CHECK_FALSE(n.mixer.solo);
  CHECK_FALSE(d.track(d.keys).active);
  REQUIRE(n.clips.size() == 1);
  CHECK(audioAt(n, 0).start == bar);
  CHECK(audioAt(n, 0).contentStartFrame == 0.0);  // file frame 0 = the selection's start
  CHECK(audioAt(n, 0).colour == Colour{0x336699});
  d.roundTrips();
  REQUIRE(d.store.undo());
  CHECK(d.store.current() == before);
}

TEST_CASE("consolidate audio: the range's clips replaced by one render clip per track, edges trimmed; one undo step") {
  Doc d;
  const TrackId a = d.must(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Audio, {}, "A")).tracks.at(0);
  const TrackId b = d.must(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Audio, a, "B")).tracks.at(0);
  const SampleRef src = d.sample("loop", 4 * 96000);  // 8 s
  const Ticks bar = 4 * pq::kPpq;  // 2 s
  const ops::NewAudioClip ncs[] = {{a, 0, src.id, {}}, {b, 2 * bar, src.id, {}}};
  d.must(ops::addAudioClips(d.doc(), d.store.ids(), std::span<const SampleRef>(&src, 1), ncs));
  const Snapshot before = d.store.current();
  const size_t h = d.store.history().cursor();
  const SampleRef ra = d.sample("A 1", 96000), rb = d.sample("B 1", 96000);
  const ops::ConsolidatedAudio cs[] = {{a, ra}, {b, rb}};
  const Ticks s = bar, e = 3 * bar;  // A's clip [0, 4 bars) is cut at both ends; B's [2, 6) at the end
  const ops::Edit ed = d.must(ops::consolidateAudio(d.doc(), d.store.ids(), cs, s, e));
  CHECK(d.store.history().cursor() == h + 1);
  CHECK(ed.clips.size() == 2);
  const Track& ta = d.track(a);
  REQUIRE(ta.clips.size() == 3);  // left piece, the render, right piece
  CHECK(ta.clips[0].end() == s);
  CHECK(audioAt(ta, 1).sample == ra.id);
  CHECK(audioAt(ta, 1).start == s);
  CHECK(ta.clips[1].end() == e);
  CHECK(audioAt(ta, 1).contentStartFrame == 0.0);
  CHECK(ta.clips[2].start() == e);
  const Track& tb = d.track(b);
  REQUIRE(tb.clips.size() == 2);
  CHECK(audioAt(tb, 0).sample == rb.id);
  CHECK(tb.clips[0].start() == s);
  CHECK(tb.clips[1].start() == e);
  d.roundTrips();
  REQUIRE(d.store.undo());
  CHECK(d.store.current() == before);
  // MIDI tracks are not rendered here.
  const ops::ConsolidatedAudio midi[] = {{d.keys, d.sample("k", 10)}};
  CHECK_FALSE(ops::consolidateAudio(d.doc(), d.store.ids(), midi, s, e).ok());
}
