// T1.10: WAV files, the timed note list, and renderOffline (fixed block, deterministic, the ImageRunner path).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "pq/engine/interpreter.h"
#include "pq/engine/note_list.h"
#include "pq/engine/offline_render.h"
#include "pq/engine/program_host.h"
#include "pq/engine/wav_file.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"

namespace {

namespace fs = std::filesystem;
namespace e = pq::engine;
namespace g = pq::graph;

struct Built {
  g::TypedPatch typed;
  g::Schedule sched;
  std::unique_ptr<e::ProgramImage> image;
};

Built build(const char* text, uint32_t maxBlock = 256, uint32_t groups = 3) {
  auto file = g::parse(text, "t.pqp");
  REQUIRE(file);
  g::PatchLibrary lib;
  lib.add(*file);
  g::InferOptions io;
  io.library = &lib;
  auto typed = g::infer(file->patches[0], io);
  REQUIRE(typed);
  g::OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  auto opt = g::optimise(std::move(typed.value()), oo);
  REQUIRE(opt);
  Built b;
  b.typed = std::move(opt.value());
  auto s = g::schedule(b.typed);
  REQUIRE(s);
  b.sched = std::move(s.value());
  e::InterpretOptions eo;
  eo.maxBlockFrames = maxBlock;
  eo.maxVoiceGroups = groups;
  auto img = e::buildInterpreted(b.sched, eo);
  REQUIRE(img);
  b.image = std::move(img.value());
  return b;
}

const char* kStereo = R"(patch St {
  param amp = 0.5 [0..1]
  out l : audio
  out r : audio
  export e = n
  n = noise(seed: 5)
  l = sine_hz(440Hz) * amp
  lp = z1(lp * 0.9 + n * 0.1)
  r = lp
})";

const char* kVoice = R"(patch V {
  out o : audio voice
  o = sine(phasor(voice.hz)) * gate_to_real(voice.gate) * voice.velocity
})";

}  // namespace

TEST_CASE("wav: 32-bit float round trip, mono / stereo / extensible, and integer PCM reading") {
  const fs::path p = fs::temp_directory_path() / "pq_test_wav_roundtrip.wav";
  for (uint32_t ch : {1u, 2u, 3u}) {
    std::vector<std::vector<float>> data(ch, std::vector<float>(1001));
    for (uint32_t c = 0; c < ch; ++c)
      for (size_t i = 0; i < data[c].size(); ++i) data[c][i] = std::sin(0.01f * static_cast<float>(i) * (c + 1)) - 0.25f * c;
    data[0][7] = 1e-30f;  // denormal-ish and odd values survive bit for bit
    data[0][8] = -3.5f;
    REQUIRE(e::writeWav(p, data, 44100));
    CHECK(fs::file_size(p) == (ch > 2 ? 80u : 58u) + 1001u * 4u * ch);
    auto w = e::readWav(p);
    REQUIRE(w);
    CHECK(w->sampleRate == 44100);
    CHECK(w->isFloat);
    REQUIRE(w->numChannels() == ch);
    REQUIRE(w->frames() == 1001);
    for (uint32_t c = 0; c < ch; ++c) CHECK(std::memcmp(w->channels[c].data(), data[c].data(), 1001 * 4) == 0);
  }
  fs::remove(p);
  // A 16-bit PCM file (as other tools write them) reads scaled to [−1, 1).
  const uint8_t pcm16[] = {'R', 'I', 'F', 'F', 40, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                           1, 0, 1, 0, 0x80, 0xBB, 0, 0, 0, 0x77, 1, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 4, 0, 0, 0,
                           0x00, 0x40, 0x00, 0x80};
  auto w = e::parseWav(pcm16, sizeof pcm16);
  REQUIRE(w);
  CHECK(w->sampleRate == 48000);
  CHECK_FALSE(w->isFloat);
  REQUIRE(w->frames() == 2);
  CHECK(w->channels[0][0] == 0.5f);
  CHECK(w->channels[0][1] == -1.0f);
  // Garbage and unsupported formats are errors, not crashes.
  const uint8_t junk[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 200, 0, 0, 0};
  CHECK_FALSE(e::parseWav(junk, sizeof junk));
  CHECK_FALSE(e::parseWav(pcm16, 11));
  CHECK_FALSE(e::readWav(fs::temp_directory_path() / "pq_no_such_file.wav"));
}

TEST_CASE("note list: sample-accurate events in block-relative frames, offs before ons, re-seek on jumps") {
  std::vector<e::TimedNote> notes(3);
  notes[0] = {0, 100, 220.0f, 0.5f, 0.5f};
  notes[1] = {100, 50, 330.0f, 0.6f, 0.5f};   // starts where note 0 ends
  notes[2] = {130, 1000, 440.0f, 0.7f, 0.5f};
  e::NoteListSource src(notes);
  CHECK(src.numEvents() == 6);
  CHECK(src.endTime() == 1130);
  e::EventBuffer buf(16);
  auto block = [&](int64_t t, uint32_t n) {
    buf.clear();
    src.source().fn(src.source().user, t, n, buf);
    return buf.list();
  };
  auto l = block(0, 64);
  REQUIRE(l.count == 1);
  CHECK(l.events[0].kind == e::NoteEventKind::NoteOn);
  CHECK(l.events[0].noteId == 1);
  l = block(64, 64);  // frames 64..127: note 0 off at 100 (frame 36) BEFORE note 1 on at 100, then nothing else
  REQUIRE(l.count == 2);
  CHECK(l.events[0].kind == e::NoteEventKind::NoteOff);
  CHECK(l.events[0].frame == 36);
  CHECK(l.events[1].kind == e::NoteEventKind::NoteOn);
  CHECK(l.events[1].noteId == 2);
  CHECK(l.events[1].hz == 330.0f);
  l = block(128, 64);
  REQUIRE(l.count == 2);
  CHECK(l.events[0].frame == 2);  // note 3 on at 130
  CHECK(l.events[1].frame == 22);  // note 2 off at 150
  l = block(0, 64);  // a jump back (a second render) re-seeks
  REQUIRE(l.count == 1);
  CHECK(l.events[0].noteId == 1);
}

TEST_CASE("note list: a note starting before 0 is clipped to its own end; one ending by 0 is left out (M1 L9)") {
  std::vector<e::TimedNote> notes(3);
  notes[0] = {-100, 150, 220.0f, 0.5f, 0.5f};  // [−100, 50) → plays [0, 50)
  notes[1] = {-100, 100, 330.0f, 0.5f, 0.5f};  // [−100, 0): not played
  notes[2] = {10, 20, 440.0f, 0.5f, 0.5f};
  e::NoteListSource src(notes);
  CHECK(src.numEvents() == 4);
  e::EventBuffer buf(16);
  buf.clear();
  src.source().fn(src.source().user, 0, 64, buf);
  const auto l = buf.list();
  REQUIRE(l.count == 4);
  CHECK(l.events[0].kind == e::NoteEventKind::NoteOn);
  CHECK(l.events[0].noteId == 1);
  CHECK(l.events[0].frame == 0);
  CHECK(l.events[1].noteId == 3);  // ids stay 1 + list index
  CHECK(l.events[1].frame == 10);
  CHECK(l.events[2].kind == e::NoteEventKind::NoteOff);
  CHECK(l.events[2].noteId == 3);
  CHECK(l.events[2].frame == 30);
  CHECK(l.events[3].kind == e::NoteEventKind::NoteOff);
  CHECK(l.events[3].noteId == 1);
  CHECK(l.events[3].frame == 50);  // was 150: the full length from the clamped start
}

TEST_CASE("renderOffline: channels, params, gain; chunk size never changes a bit") {
  Built b = build(kStereo);
  e::BufferSink ref;
  e::RenderRequest rq;
  rq.image = b.image.get();
  rq.frames = 10000;
  rq.blockFrames = 64;
  rq.chunkFrames = 8192;
  auto st = e::renderOffline(rq, ref);
  REQUIRE(st);
  CHECK(st->numChannels == 2);  // two outs; the export is not played
  CHECK(st->frames == 10000);
  CHECK(st->blocks == (8192 / 64) + (10000 - 8192 + 63) / 64);
  CHECK(st->rejectedBlocks == 0);
  CHECK(st->realtimeFactor > 0.0);
  REQUIRE(ref.channels.size() == 2);
  REQUIRE(ref.channels[0].size() == 10000);
  CHECK(ref.sampleRate == 48000);
  float peak = 0.0f;
  for (float v : ref.channels[0]) peak = std::max(peak, std::abs(v));
  CHECK(peak == doctest::Approx(0.5f).epsilon(1e-3));  // the amp param's default
  // A fresh image per render (state restarts); every chunk size gives the same bits.
  for (uint32_t chunk : {1u, 64u, 100u, 4096u, 1u << 20}) {
    Built again = build(kStereo);
    rq.image = again.image.get();
    rq.chunkFrames = chunk;
    e::BufferSink s;
    REQUIRE(e::renderOffline(rq, s));
    for (size_t c = 0; c < 2; ++c) CHECK(std::memcmp(s.channels[c].data(), ref.channels[c].data(), 10000 * 4) == 0);
  }
  // Params by slot, gain, a forced channel count.
  Built p = build(kStereo);
  const float amp[] = {0.25f};
  rq.image = p.image.get();
  rq.params = amp;
  rq.gain = 2.0f;
  rq.numChannels = 3;
  e::BufferSink s;
  REQUIRE(e::renderOffline(rq, s));
  REQUIRE(s.channels.size() == 3);
  for (size_t i = 0; i < 10000; ++i) CHECK(s.channels[0][i] == doctest::Approx(ref.channels[0][i]).epsilon(1e-6));
  CHECK(std::all_of(s.channels[2].begin(), s.channels[2].end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("renderOffline: instruments play their voice mixes from a note list; RtScope-clean") {
  Built b = build(kVoice, 128, 3);
  e::VoiceEngine voices;
  e::VoiceConfig vc;
  vc.maxVoices = 16;
  REQUIRE(voices.prepare(b.image.get(), vc));
  std::vector<e::TimedNote> notes{{1000, 2000, 440.0f, 1.0f, 0.5f}, {1500, 500, 660.0f, 0.5f, 0.5f}};
  e::NoteListSource src(notes);
  e::RenderRequest rq;
  rq.image = b.image.get();
  rq.frames = 4800;
  rq.blockFrames = 100;  // not a power of two: events land mid-block
  rq.voices = &voices;
  rq.events = src.source();
  e::BufferSink s;
  // renderOffline opens an RtScope around its block loop (event source, VoiceEngine, image): any allocation or free
  // in there fails this test case (the allocation trap linked into every test executable).
  REQUIRE(e::renderOffline(rq, s));
  REQUIRE(s.channels.size() == 1);
  const auto& x = s.channels[0];
  for (size_t i = 0; i < 1000; ++i) CHECK(x[i] == 0.0f);  // silent before the first onset (sample accurate)
  CHECK(x[1001] != 0.0f);
  for (size_t i = 3000; i < 4800; ++i) CHECK(x[i] == 0.0f);  // gate off at 3000 (no release in this patch)
  CHECK(voices.stats().noteOns == 2);
  CHECK(voices.stats().noteOffs == 2);
}

TEST_CASE("renderOffline: argument errors") {
  Built b = build(kStereo, 64);
  e::BufferSink s;
  e::RenderRequest rq;
  CHECK_FALSE(e::renderOffline(rq, s));  // no image
  rq.image = b.image.get();
  rq.frames = 100;
  rq.blockFrames = 128;  // above the image's maximum
  CHECK_FALSE(e::renderOffline(rq, s));
  rq.blockFrames = 0;
  CHECK_FALSE(e::renderOffline(rq, s));
  rq.blockFrames = 64;
  const float two[] = {1.0f, 2.0f};
  rq.params = two;  // one param slot only
  CHECK_FALSE(e::renderOffline(rq, s));
}

// T1.x-arch2: renderOffline runs through a ProgramHost (the device's hot-swap path); the host form renders whatever
// a host plays, with publishes between chunks exactly as a compiler thread would make them live.
TEST_CASE("renderOffline: through a ProgramHost, swaps between chunks are the live protocol (bit-exact on equal programs)") {
  Built ref = build(kStereo);
  e::RenderRequest rq;
  rq.image = ref.image.get();
  rq.frames = 10000;
  rq.blockFrames = 64;
  e::BufferSink want;
  REQUIRE(e::renderOffline(rq, want));

  e::HostConfig hc;
  hc.sampleRate = 48000.0;
  hc.maxBlockFrames = 256;
  hc.numOut = 2;
  e::ProgramHost host(hc);
  REQUIRE(host.publish(build(kStereo).image));
  struct Swapper {
    e::ProgramHost* host;
    int64_t at;
    int published = 0;
    static void between(void* self, int64_t next) {
      auto& s = *static_cast<Swapper*>(self);
      if (next < s.at || s.published > 0) return;
      // A recompile of the same patch (same state keys): the transfer carries every state word, the crossfade is
      // exact where old == new.
      REQUIRE(s.host->publish(build(kStereo).image, e::SwapPolicy{e::SwapMode::Crossfade, 10.0f}));
      ++s.published;
    }
  } swapper{&host, 3200};
  e::HostRenderRequest hr;
  hr.frames = 10000;
  hr.blockFrames = 64;
  hr.numChannels = 2;
  hr.chunkFrames = 640;
  hr.betweenChunks = &Swapper::between;
  hr.user = &swapper;
  e::BufferSink got;
  auto st = e::renderOffline(host, hr, got);
  REQUIRE(st);
  CHECK(st->blocks == (10000 + 63) / 64);
  CHECK(swapper.published == 1);
  CHECK(host.stats().swaps == 2);
  CHECK(host.stats().crossfades == 1);
  CHECK(host.stats().fadeBlocks > 0);
  REQUIRE(got.channels.size() == 2);
  for (size_t c = 0; c < 2; ++c) CHECK(std::memcmp(got.channels[c].data(), want.channels[c].data(), 10000 * 4) == 0);
  host.collect();

  // The one-image form checks that a VoiceEngine is prepared for that image (M1 review L9).
  Built v1 = build(kVoice, 128, 3), v2 = build(kVoice, 128, 3);
  e::VoiceEngine voices;
  e::VoiceConfig vc;
  vc.maxVoices = 16;
  REQUIRE(voices.prepare(v1.image.get(), vc));
  e::RenderRequest vr;
  vr.image = v2.image.get();
  vr.frames = 256;
  vr.voices = &voices;
  e::BufferSink s;
  CHECK_FALSE(e::renderOffline(vr, s));
  vr.image = v1.image.get();
  CHECK(e::renderOffline(vr, s));
  CHECK(voices.binding() != nullptr);  // the caller's binding is attached again afterwards
  CHECK(voices.binding()->image == v1.image.get());
}
