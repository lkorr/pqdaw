// T5.3: audio tracks through the lowering and the playback link — the golden lowering text (clipPlayer → chain →
// mixer stages), the clip-player slot, audio clip edits change runtime data only (no compile), routing / sends /
// meters as a MIDI track's. Audible playback (T5.4) is tests/app/test_audio_playback.cpp's; here the sample file does
// not exist, so the track renders silence.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/document/audio_sequence.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/engine/offline_render.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

bool updateGoldens() {
  const char* v = std::getenv("PQ_UPDATE_GOLDENS");
  return v != nullptr && *v != '\0' && *v != '0';
}

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

// An audio track "Loop" (Utility after the clip player, a send to the return "Echo") with one clip of a 2 s sample.
struct AudioSong {
  DocumentStore store;
  TrackId audio, echo;
  ClipRef clip;
  AudioSong() {
    IdSource& ids = store.ids();
    echo = ids.track();
    REQUIRE(store.dispatch(AddTrack{playback_fixture::track(echo, TrackKind::Return, "Echo"), {}, kAppend}).ok());
    auto t = ops::createTrack(*store.current(), ids, TrackKind::Audio, {}, "Loop");
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    audio = t->tracks.at(0);
    auto d = ops::addDevice(*store.current(), ids, audio, "utility", DeviceKind::AudioEffect);
    REQUIRE(d.ok());
    REQUIRE(store.dispatch(d->command).ok());
    REQUIRE(store.dispatch(SetSendLevel{audio, echo, -6.0f}).ok());
    SampleRef s;
    s.id = ids.sample();
    s.path = "C:/nowhere/loop.wav";
    s.frames = 96000;
    s.rate = 48000.0;
    s.channels = 2;
    s.displayName = "loop";
    const ops::NewAudioClip nc{audio, 0, s.id, {}};
    auto c = ops::addAudioClips(*store.current(), ids, std::span<const SampleRef>(&s, 1),
                                std::span<const ops::NewAudioClip>(&nc, 1));
    REQUIRE(c.ok());
    REQUIRE(store.dispatch(c->command).ok());
    clip = c->clips.at(0);
  }
};

}  // namespace

TEST_CASE("audio tracks: golden lowering text audio-track.pqp (clipPlayer → chain → mixer), role Audio, its slot") {
  AudioSong song;
  auto link = makeLink(song.store.current());
  LowerOptions lo;
  lo.library = &link->devices().library();
  lo.devices = &link->devices();
  lo.clipSlotOf = [](TrackId) { return 3u; };
  auto lowered = lower(*song.store.current(), lo);
  REQUIRE_MESSAGE(lowered.ok(), (lowered.ok() ? std::string() : lowered.error().message));
  const LoweredProject& lp = *lowered;
  uint32_t gi = graph::kNone;
  for (uint32_t i = 0; i < lp.trackIds.size(); ++i)
    if (lp.trackIds[i] == song.audio) gi = i;
  REQUIRE(gi != graph::kNone);
  CHECK(lp.graph.tracks[gi].role == graph::TrackRole::Audio);
  CHECK(lp.graph.tracks[gi].eventSlot == graph::kNone);
  CHECK(lp.missingDevices.empty());
  const std::string text = patchText(*lp.patches[gi]);
  CHECK(text.find("clipPlayer(") != std::string::npos);
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/app/fixtures/lowering/audio-track.pqp";
  if (updateGoldens()) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
    MESSAGE("rewrote " << path);
  }
  CHECK(readFile(path) == text);
  // The send reaches the return; the mixer params are a MIDI track's.
  bool send = false;
  for (const graph::ProjectWire& w : lp.graph.wires) send = send || (w.from.track == gi && w.from.port == "post");
  CHECK(send);
  bool volume = false, mute = false;
  for (const LoweredParam& p : lp.params) {
    volume = volume || p.key == std::to_string(song.audio.value) + ":volume";
    mute = mute || p.key == std::to_string(song.audio.value) + ":mute";
  }
  CHECK(volume);
  CHECK(mute);
  // Without a slot the track lowers without its clips (and says so).
  lo.clipSlotOf = nullptr;
  auto noSlot = lower(*song.store.current(), lo);
  REQUIRE(noSlot.ok());
  CHECK(noSlot->withoutSlot == std::vector<TrackId>{song.audio});
  CHECK(patchText(*noSlot->patches[gi]).find("clipPlayer(") == std::string::npos);
}

TEST_CASE("audio tracks: the link compiles the track once; clip edits are runtime data (no compile); a missing file is silent") {
  AudioSong song;
  auto link = makeLink(song.store.current());
  const uint64_t compiles = link->stats().compiles;
  CHECK(link->lastError().empty());
  // Clip gain, transpose, fades, reverse, warp, moving the clip: Sequence changes only.
  const ClipRef c = song.clip;
  const std::span<const ClipRef> one(&c, 1);
  const Snapshot s0 = song.store.current();
  auto apply = [&](const Result<ops::Edit>& e) {
    REQUIRE(e.ok());
    const Snapshot before = song.store.current();
    REQUIRE(song.store.dispatch(e->command).ok());
    link->onDocumentChange(StoreEvent{StoreEventKind::Dispatch, before, song.store.current(), song.store.lastChanges()});
    CHECK_FALSE(song.store.lastChanges().structural());
  };
  apply(ops::setAudioClipGain(*song.store.current(), one, -12.0f));
  apply(ops::setAudioClipTranspose(*song.store.current(), one, AudioTranspose{TransposeUnit::Cents, 300.0, 0, {1}}));
  apply(ops::setAudioClipFades(*song.store.current(), c, AudioFade{kPpq / 4, 0.0f}, AudioFade{kPpq / 4, 0.0f}));
  apply(ops::setAudioClipsReverse(*song.store.current(), one, true));
  apply(ops::setAudioClipWarp(*song.store.current(), one, AudioWarp::RePitch, 110.0));
  apply(ops::moveClipsBy(*song.store.current(), song.store.ids(), one, kPpq));
  link->flush();
  CHECK(link->stats().compiles == compiles);  // nothing recompiled
  CHECK(link->lastError().empty());
  (void)s0;
  // The sample (C:/nowhere/loop.wav) is missing: the clip plays silence although it is under the playhead for the whole
  // second (T5.4: missing samples are silent).
  engine::BufferSink sink;
  auto st = link->renderOffline(0, 48000, sink);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  float peak = 0.0f;
  for (const auto& ch : sink.channels)
    for (float v : ch) peak = std::max(peak, std::fabs(v));
  CHECK(peak == 0.0f);
  // A new audio track is a structural edit: one compile.
  auto t = ops::createTrack(*song.store.current(), song.store.ids(), TrackKind::Audio);
  REQUIRE(t.ok());
  const Snapshot before = song.store.current();
  REQUIRE(song.store.dispatch(t->command).ok());
  link->onDocumentChange(StoreEvent{StoreEventKind::Dispatch, before, song.store.current(), song.store.lastChanges()});
  link->flush();
  CHECK(link->stats().compiles == compiles + 1);
  CHECK(link->lastError().empty());
}

TEST_CASE("audio tracks: the audio sequence description — missing samples and deactivated clips are silent") {
  AudioSong song;
  const auto path = findTrack(*song.store.current(), song.audio);
  REQUIRE(path);
  AudioLowerOptions o;
  AudioTrackDesc d = lowerAudioTrack(*song.store.current(), *path, o);
  REQUIRE(d.clips.size() == 1);
  CHECK(d.clips[0].playable);
  CHECK(d.clips[0].speed == 48000.0);
  CHECK(d.clips[0].gain == 1.0f);
  o.available = [](SampleId) { return false; };
  d = lowerAudioTrack(*song.store.current(), *path, o);
  CHECK_FALSE(d.clips[0].playable);
}
