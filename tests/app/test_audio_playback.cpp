// T5.4: audio clips through the playback link (pq/app/project_link.h "Audio clips"):
//   - a clip sounds at the sample its start tick rounds to; a missing sample is silent; clip edits compile nothing
//   - the golden render audio-clips (2 s: gain, fades, crossfade, reverse, ±7 st, a 44.1 kHz file, RePitch with a loop
//     at a tempo ramp), bit-identical across device buffers 32 / 64 / 256 / 1024 and interpreter == JIT
//   - a Steps transpose follows its tuning (a tuning param re-lowers the clip, no compile)
//   - streamed samples: the offline render (blocking fills) equals the resident one; a locate prefetches the streams
//   - an audio track with no clip under the playhead stays an idle unit (§7.7); playback through the engine under
//     RtScope, edits while playing
#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/store.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/offline_render.h"
#include "pq/engine/wav_file.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 48000.0;

bool updateGoldens() {
  const char* v = std::getenv("PQ_UPDATE_GOLDENS");
  return v != nullptr && *v != '\0' && *v != '0';
}

std::string utf8(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

LinkConfig linkConfig(bool jit = false) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  return c;
}

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, LinkConfig c = linkConfig()) {
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

golden::Channels render(ProjectLink& link, double seconds, uint32_t blockFrames = 0, Ticks from = 0) {
  engine::BufferSink sink;
  auto st = link.renderOffline(from, static_cast<uint64_t>(std::llround(seconds * kRate)), sink, blockFrames);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  return sink.channels;
}

// Plays the link's engine through a BlockAdapter at a device buffer (the engine block stays the link's 64 frames, as
// with a real device), under RtScope; the adapter's FIFO latency (a buffer that is not a multiple of 64) is dropped.
golden::Channels renderDevice(ProjectLink& link, double seconds, uint32_t device) {
  link.flush();
  (void)link.waitForSamples();
  engine::BlockAdapter ad;
  REQUIRE(ad.prepare(link.callback(), 0, 2, link.config().blockFrames, device));
  const auto frames = static_cast<uint64_t>(std::llround(seconds * kRate));
  const uint64_t total = frames + ad.latencyFrames();
  engine::TransportCommand play{engine::TransportCommandKind::Play, false, 0, 0, 0};
  REQUIRE(link.engine().post(play));
  golden::Channels out(2);
  std::vector<float> l(device), r(device);
  int64_t time = 0;
  for (uint64_t done = 0; done < total; done += device) {
    float* ch[2] = {l.data(), r.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      ad.process(engine::AudioBlock{nullptr, 0, ch, 2, device, time});
    }
    time += device;
    out[0].insert(out[0].end(), l.begin(), l.end());
    out[1].insert(out[1].end(), r.begin(), r.end());
  }
  for (auto& c : out) {
    c.erase(c.begin(), c.begin() + ad.latencyFrames());
    c.resize(frames);
  }
  return out;
}

float peak(const golden::Channels& a) {
  float p = 0.0f;
  for (const auto& c : a)
    for (float v : c) p = std::max(p, std::fabs(v));
  return p;
}

// A project of audio tracks over generated WAV files in a temp directory.
struct Song {
  DocumentStore store;
  fs::path dir;
  explicit Song(const char* name) : dir(fs::temp_directory_path() / name) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
  }
  ~Song() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  SampleRef sample(const char* file, int64_t frames, uint32_t channels, double rate,
                   const std::function<float(uint32_t, int64_t)>& fn) {
    golden::Channels ch(channels, std::vector<float>(static_cast<size_t>(frames)));
    for (uint32_t c = 0; c < channels; ++c)
      for (int64_t i = 0; i < frames; ++i) ch[c][static_cast<size_t>(i)] = fn(c, i);
    const fs::path p = dir / file;
    REQUIRE(engine::writeWav(p, ch, static_cast<uint32_t>(rate)).ok());
    SampleRef s;
    s.id = store.ids().sample();
    s.path = utf8(p);
    s.size = fs::file_size(p);
    s.frames = frames;
    s.rate = rate;
    s.channels = channels;
    s.displayName = file;
    return s;
  }
  TrackId track() {
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Audio);
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    return t->tracks.at(0);
  }
  ClipRef clip(TrackId track, Ticks start, const SampleRef& s) {
    const bool known = findSample(*store.current(), s.id) != nullptr;
    const ops::NewAudioClip nc{track, start, s.id, {}};
    auto c = ops::addAudioClips(*store.current(), store.ids(), known ? std::span<const SampleRef>() : std::span<const SampleRef>(&s, 1),
                                std::span<const ops::NewAudioClip>(&nc, 1));
    REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
    REQUIRE(store.dispatch(c->command).ok());
    return c->clips.at(0);
  }
  void apply(const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
  }
  // Dispatches to the store and tells the link, as the context's subscription would.
  void edit(ProjectLink& link, const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const Snapshot before = store.current();
    REQUIRE(store.dispatch(e->command).ok());
    link.onDocumentChange(StoreEvent{StoreEventKind::Dispatch, before, store.current(), store.lastChanges()});
  }
};

uint32_t lcg(uint32_t& s) { return s = s * 1664525u + 1013904223u; }

// The golden song: three audio tracks over two files (the plan's audio-clips golden).
struct GoldenSong : Song {
  GoldenSong() : Song("pq_audio_clips_golden") {
    REQUIRE(store.dispatch(SetTempoMap{{{0, 120.0, TempoCurve::Linear}, {kPpq * 4, 140.0, TempoCurve::Step}}}).ok());
    uint32_t seed = 12345;
    std::vector<float> noise(48000);
    for (float& v : noise) v = static_cast<float>(static_cast<int32_t>(lcg(seed)) / 2147483648.0) * 0.05f;
    const SampleRef a = sample("a.wav", 48000, 2, 48000.0, [&](uint32_t c, int64_t i) {
      const double t = static_cast<double>(i) / 48000.0;
      return c == 0 ? static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * 330 * t) * std::exp(-2 * t) +
                                         0.2 * std::sin(2 * std::numbers::pi * 5000 * t))
                    : static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * 440 * t)) + noise[static_cast<size_t>(i)];
    });
    const SampleRef b = sample("b.wav", 33075, 1, 44100.0, [](uint32_t, int64_t i) {
      const double t = static_cast<double>(i) / 44100.0;
      return static_cast<float>(0.6 * std::sin(2 * std::numbers::pi * 1000 * t) * (1.0 - t / 0.75) +
                                0.2 * std::sin(2 * std::numbers::pi * 9000 * t));
    });
    // Track 1: gain + curved fade in; an adjacent reversed clip with a crossfade and a curved fade out.
    t1 = track();
    a1 = clip(t1, 0, a);
    a2 = clip(t1, kPpq, a);  // overwrites a1's second half: the two are adjacent
    apply(ops::setAudioClipGain(*store.current(), std::span<const ClipRef>(&a1, 1), -3.0f));
    apply(ops::setAudioClipFades(*store.current(), a1, AudioFade{kPpq / 8, 0.5f}, AudioFade{}));
    apply(ops::setAudioClipsReverse(*store.current(), std::span<const ClipRef>(&a2, 1), true));
    apply(ops::setAudioClipCrossfade(*store.current(), a2, kPpq / 4));
    apply(ops::setAudioClipFades(*store.current(), a2, AudioFade{}, AudioFade{kPpq / 4, -0.5f}));
    // Track 2: a 44.1 kHz file at +7 st (cents) and at 2/3 (−7.02 st).
    t2 = track();
    b1 = clip(t2, kPpq / 2, b);
    b2 = clip(t2, kPpq * 5 / 2, b);
    apply(ops::setAudioClipTranspose(*store.current(), std::span<const ClipRef>(&b1, 1), AudioTranspose{TransposeUnit::Cents, 700.0, 0, {1}}));
    apply(ops::setAudioClipTranspose(*store.current(), std::span<const ClipRef>(&b2, 1),
                                     AudioTranspose{TransposeUnit::Ratio, 0.0, 0, tuning::Rational(2, 3)}));
    // Track 3: Re-Pitch (source at 100 BPM) with a loop, through the tempo ramp.
    t3 = track();
    c1 = clip(t3, kPpq / 4, a);
    apply(ops::setAudioClipWarp(*store.current(), std::span<const ClipRef>(&c1, 1), AudioWarp::RePitch, 100.0));
    apply(ops::setAudioClipLoop(*store.current(), c1, ClipLoop{true, kPpq / 8, kPpq * 5 / 8}));
    apply(ops::resizeClips(*store.current(), store.ids(), std::span<const ClipRef>(&c1, 1), ops::ClipEdge::End, kPpq * 2));
  }
  TrackId t1, t2, t3;
  ClipRef a1, a2, b1, b2, c1;
};

}  // namespace

TEST_CASE("audio playback: a clip sounds on its start sample; a missing sample is silent; edits compile nothing") {
  Song song("pq_audio_playback_place");
  const SampleRef s = song.sample("impulse.wav", 20000, 1, 48000.0, [](uint32_t, int64_t i) { return i == 5000 ? 1.0f : 0.0f; });
  const TrackId t = song.track();
  const Ticks T = kPpq + 4321;
  const ClipRef c = song.clip(t, T, s);
  auto link = makeLink(song.store.current());
  const uint64_t compiles = link->stats().compiles;
  golden::Channels out = render(*link, 1.0);
  const SampleTime at = song.store.current()->timeline->tempoMap.ticksToSamples(T, kRate) + 5000;
  for (uint32_t ch = 0; ch < 2; ++ch)
    for (size_t i = 0; i < out[ch].size(); ++i) {
      if (static_cast<SampleTime>(i) == at) REQUIRE(out[ch][i] == doctest::Approx(1.0).epsilon(1e-6));
      else REQUIRE(std::fabs(out[ch][i]) < 1e-7f);
    }
  CHECK(link->stats().samplesLoaded + link->stats().audioPublishes >= 1);
  // A gain edit: the clip plays 6 dB lower; nothing compiled.
  song.edit(*link, ops::setAudioClipGain(*song.store.current(), std::span<const ClipRef>(&c, 1), -6.0f));
  out = render(*link, 1.0);
  CHECK(out[0][static_cast<size_t>(at)] == doctest::Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(1e-5));
  CHECK(link->stats().compiles == compiles);
  // The file disappears (the project is opened elsewhere): the clip is silent.
  {
    auto broken = std::make_shared<Project>(*song.store.current());
    SampleList list = samplesOf(*broken);
    list[0].path = utf8(song.dir / "gone.wav");
    broken->samples = std::make_shared<const SampleList>(std::move(list));
    auto l2 = makeLink(broken);
    CHECK(peak(render(*l2, 1.0)) == 0.0f);
  }
  CHECK(link->lastError().empty());
}

TEST_CASE("audio playback: golden audio-clips; bit-identical across device buffers; interpreter == JIT") {
  GoldenSong song;
  constexpr double kSeconds = 2.0;
  auto link = makeLink(song.store.current());
  const golden::Channels out = render(*link, kSeconds);
  REQUIRE(out.size() == 2);
  CHECK(peak(out) > 0.2f);
  CHECK(peak(out) < 3.0f);  // three tracks, unity mixer
  CHECK(link->lastError().empty());
  const fs::path wav = golden::goldenDir() / "audio" / "audio_clips.wav";
  if (updateGoldens()) {
    fs::create_directories(wav.parent_path());
    REQUIRE(engine::writeWav(wav, out, static_cast<uint32_t>(kRate)).ok());
    MESSAGE("updated " << wav.string());
  }
  auto gold = engine::readWav(wav);
  REQUIRE_MESSAGE(gold.ok(), (gold.ok() ? std::string() : gold.error().message + " (PQ_UPDATE_GOLDENS=1)"));
  const golden::Comparison cmp = golden::compare(*gold, out, static_cast<uint32_t>(kRate));
  CHECK_MESSAGE(cmp.ok, cmp.message);
  // Device buffers 32 / 64 / 256 / 1024 (the engine block stays 64, as behind any device: BlockAdapter): bit-identical.
  for (uint32_t b : {32u, 64u, 256u, 1024u}) {
    auto l = makeLink(song.store.current());
    CAPTURE(b);
    CHECK(renderDevice(*l, kSeconds, b) == out);
  }
  // The JIT images of the same units: bit-identical to the interpreter's.
  auto jit = makeLink(song.store.current(), linkConfig(true));
  REQUIRE(jit->waitForJit());
  CHECK(jit->jitStats().published >= 1);
  CHECK(render(*jit, kSeconds) == out);
}

TEST_CASE("audio playback: a Steps transpose follows its tuning (re-lowered, no compile)") {
  Song song("pq_audio_playback_steps");
  const SampleRef s = song.sample("tone.wav", 48000, 1, 48000.0, [](uint32_t, int64_t i) {
    return static_cast<float>(std::sin(2 * std::numbers::pi * 200 * static_cast<double>(i) / 48000.0));
  });
  const TrackId t = song.track();
  const ClipRef c = song.clip(t, 0, s);
  song.apply(ops::setAudioClipTranspose(*song.store.current(), std::span<const ClipRef>(&c, 1),
                                        AudioTranspose{TransposeUnit::Steps, 0.0, 7, {1}}));
  auto link = makeLink(song.store.current());
  auto stepOf = [&] {
    const engine::AudioSequenceImage* img = link->audioImage();
    REQUIRE(img != nullptr);
    const uint32_t slot = link->eventSlot(t);
    REQUIRE(slot < img->numSlots());
    REQUIRE(img->clips(slot).size() == 1);
    return static_cast<double>(img->clips(slot)[0].step) / 4294967296.0;
  };
  CHECK(stepOf() == doctest::Approx(std::pow(2.0, 7.0 / 12.0)).epsilon(1e-9));
  const uint64_t compiles = link->stats().compiles;
  const uint64_t publishes = link->stats().audioPublishes;
  const TuningId tid = song.store.current()->tunings->front()->id;
  const Snapshot before = song.store.current();
  REQUIRE(song.store.dispatch(SetTuningParam{tid, ParamKind::TuningEquave, 1300.0}).ok());
  link->onDocumentChange(StoreEvent{StoreEventKind::Dispatch, before, song.store.current(), song.store.lastChanges()});
  link->flush();
  CHECK(stepOf() == doctest::Approx(std::pow(2.0, 7.0 * 1300.0 / 12.0 / 1200.0)).epsilon(1e-9));
  CHECK(link->stats().audioPublishes == publishes + 1);
  CHECK(link->stats().compiles == compiles);
  // A param change that no audio clip follows republishes nothing.
  const Snapshot b2 = song.store.current();
  REQUIRE(song.store.dispatch(SetTrackVolume{t, -3.0f}).ok());
  link->onDocumentChange(StoreEvent{StoreEventKind::Dispatch, b2, song.store.current(), song.store.lastChanges()});
  CHECK(link->stats().audioPublishes == publishes + 1);
}

// T5.x-perf-audio (Q151 item 1): a clip two octaves up on a resident sample: the link asks its loader for the mip level
// the rate needs, a render waits for it (the same frames whatever the timing), and the player reads it: the expected
// sine, four times faster.
TEST_CASE("audio playback: a clip above an octave reads the mip level the link builds for it") {
  Song song("pq_audio_playback_mips");
  const SampleRef s = song.sample("tone.wav", 96000, 1, 48000.0, [](uint32_t, int64_t i) {
    return static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * 200 * static_cast<double>(i) / 48000.0));
  });
  const TrackId t = song.track();
  const ClipRef c = song.clip(t, 0, s);
  song.apply(ops::setAudioClipTranspose(*song.store.current(), std::span<const ClipRef>(&c, 1),
                                        AudioTranspose{TransposeUnit::Cents, 2400.0, 0, {1}}));
  auto link = makeLink(song.store.current());
  const golden::Channels out = render(*link, 0.4);
  CHECK(link->samplePool().stats().mipBuilds == 1);
  const media::SampleMips* m = nullptr;
  {
    const engine::AudioSequenceImage* img = link->audioImage();
    REQUIRE(img != nullptr);
    REQUIRE(img->numSamples() == 1);
    m = img->sample(0).pooled->mips();
  }
  REQUIRE(m != nullptr);
  CHECK(m->levels == 1);  // rate 4: level 1 reads it at rate 2
  CHECK(link->engine().timeline().clipPlayer().stats().mipFrames > 0);
  double worst = 0.0;
  for (size_t i = 4800; i < out[0].size(); ++i)  // after the edge declick and the new mixer stage's fade in
    worst = std::max(worst, std::fabs(out[0][i] - 0.5 * std::sin(2 * std::numbers::pi * 800 * static_cast<double>(i) / 48000.0)));
  MESSAGE("+24 st through the mip: max |error| against the ideal sine ", worst);
  CHECK(worst < 1e-4);
  CHECK(link->lastError().empty());
}

TEST_CASE("audio playback: streamed samples render like resident ones; a locate prefetches their streams") {
  GoldenSong song;
  auto resident = makeLink(song.store.current());
  const golden::Channels a = render(*resident, 2.0);
  LinkConfig sc = linkConfig();
  sc.pool.residentLimitBytes = 0;  // everything streams
  sc.pool.headFrames = 2000;
  sc.pool.streamSlots = 32;
  auto streamed = makeLink(song.store.current(), sc);
  const golden::Channels b = render(*streamed, 2.0);
  CHECK(b == a);
  // Stopped, a locate into the clips prefetches their streams (adopted by the claims Play makes).
  const uint64_t before = streamed->stats().prefetches;
  streamed->locate(kPpq / 2 + 1000);
  CHECK(streamed->stats().prefetches > before);
  CHECK(streamed->diskUnderruns() == 0);
}

TEST_CASE("audio playback: an audio track idles without a clip under the playhead; live edits under RtScope") {
  Song song("pq_audio_playback_idle");
  const SampleRef s = song.sample("tone.wav", 96000, 2, 48000.0, [](uint32_t c, int64_t i) {
    return static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * (200 + 100 * c) * static_cast<double>(i) / 48000.0));
  });
  const TrackId t = song.track();
  const ClipRef c = song.clip(t, kPpq * 8, s);  // 4 s in
  auto link = makeLink(song.store.current());
  (void)link->waitForSamples();
  engine::Engine& eng = link->engine();
  constexpr uint32_t kBlock = 64;
  std::vector<float> l(kBlock), r(kBlock);
  int64_t time = 0;
  auto run = [&](uint32_t blocks) {
    float pk = 0.0f;
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        eng.process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      for (float v : l) pk = std::max(pk, std::fabs(v));
    }
    return pk;
  };
  link->play(0);
  CHECK(run(750) == 0.0f);  // 1 s: no clip under the playhead
  const engine::ProjectProgram* prog = eng.program();
  REQUIRE(prog != nullptr);
  uint64_t idle = 0;
  for (uint32_t u = 0; u < prog->numUnits(); ++u) idle += prog->unitStats(u).idleBlocks;
  CHECK(idle > 0);  // the audio track's unit (and the master) slept
  CHECK(eng.timeline().clipPlayer().stats().voicesStarted == 0);
  link->locate(kPpq * 8);
  CHECK(run(200) > 0.3f);  // the clip plays
  // Edits while playing (gain, move, reverse): images swap under RtScope, no click (the largest step stays small).
  const ClipRef one = c;
  float worst = 0.0f;
  float last = l.back();
  auto stepRun = [&](uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        eng.process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      for (float v : l) {
        worst = std::max(worst, std::fabs(v - last));
        last = v;
      }
    }
  };
  song.edit(*link, ops::setAudioClipGain(*song.store.current(), std::span<const ClipRef>(&one, 1), -12.0f));
  stepRun(20);
  song.edit(*link, ops::setAudioClipsReverse(*song.store.current(), std::span<const ClipRef>(&one, 1), true));
  stepRun(20);
  // The tone's own largest step: 0.5 · 2π · 200 / 48000 ≈ 0.013; a cut would step by up to 1.
  CHECK(worst < 0.05f);
  link->stop();
  stepRun(10);
  CHECK(std::fabs(l.back()) == 0.0f);
}
