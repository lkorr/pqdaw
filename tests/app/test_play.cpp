// T1.7: `pqdaw --play` — CLI parsing and the compile path (file → ProgramImage), driven without a device.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "pq/app/cli.h"
#include "pq/app/play.h"
#include "pq/core/alloc_trap.h"
#include "pq/engine/image_runner.h"

using namespace pq::app;

namespace {
std::string casePath(const char* name) {
  return (std::filesystem::path(PQ_TEST_DATA_DIR) / "graph" / "cases" / name).string();
}
}  // namespace

TEST_CASE("cli: --play") {
  std::vector<std::string_view> a{"--play", "x.pqp", "--patch", "Stack", "--seconds", "2", "--level", "-30", "--block", "64"};
  auto o = parseCli(a);
  REQUIRE(o.ok());
  CHECK(o->mode == Mode::Play);
  CHECK(o->playPath == "x.pqp");
  CHECK(o->patchName == "Stack");
  CHECK(o->seconds == 2.0);
  CHECK(o->levelDb == -30.0);
  CHECK(o->blockFrames == 64);
  std::vector<std::string_view> noFile{"--play"};
  CHECK_FALSE(parseCli(noFile).ok());
  std::vector<std::string_view> patchOnly{"--patch", "Stack"};
  CHECK_FALSE(parseCli(patchOnly).ok());
  std::vector<std::string_view> stall{"--play", "x.pqp", "--inject-stall", "3"};
  CHECK_FALSE(parseCli(stall).ok());
  std::vector<std::string_view> notes{"--play", "x.pqp", "--notes", "60 69, 330hz 61.5"};
  auto n = parseCli(notes);
  REQUIRE(n.ok());
  auto hz = parsePlayNotes(n->notes);
  REQUIRE(hz.ok());
  REQUIRE(hz->size() == 4);
  CHECK((*hz)[0] == doctest::Approx(261.6256).epsilon(1e-6));
  CHECK((*hz)[1] == 440.0f);
  CHECK((*hz)[2] == 330.0f);
  CHECK((*hz)[3] == doctest::Approx(440.0 * std::exp2((61.5 - 69.0) / 12.0)).epsilon(1e-6));
  for (const char* bad : {"", "  ", "x", "60 abc", "0.5hz", "200", "-5hz"}) {
    std::vector<std::string_view> b{"--play", "x.pqp", "--notes", bad};
    CHECK_MESSAGE(!parseCli(b).ok(), bad);
  }
  std::vector<std::string_view> notesOnly{"--notes", "60"};
  CHECK_FALSE(parseCli(notesOnly).ok());
}

TEST_CASE("play: a patch file compiles to an image that plays its global out on every channel") {
  auto c = compilePatchFile(casePath("multi_patch.pqp"), "Stack", 48000.0, 64);
  REQUIRE_MESSAGE(c.ok(), (c.ok() ? "" : c.error().message));
  CHECK(c->patchName == "Stack");
  CHECK(c->outputs == 1);
  CHECK(c->ops > 0);
  pq::engine::ImageRunner runner;
  REQUIRE(runner.prepare(c->image.get(), 0, 2, 0.5f, true));
  std::vector<float> l(64), r(64);
  float* outs[2] = {l.data(), r.data()};
  double energy = 0;
  for (int b = 0; b < 20; ++b) {
    const pq::engine::AudioBlock blk{nullptr, 0, outs, 2, 64, b * 64};
    CHECK(pq::alloc_trap::countRtViolations([&] { runner.process(blk); }) == 0);
    for (int i = 0; i < 64; ++i) {
      CHECK(std::isfinite(l[i]));
      CHECK(l[i] == r[i]);
      CHECK(std::fabs(l[i]) <= 1.0f);
      energy += l[i] * l[i];
    }
  }
  CHECK(energy > 1.0);
}

TEST_CASE("play: errors name the problem") {
  auto missing = compilePatchFile(casePath("multi_patch.pqp"), "NoSuchPatch", 48000.0, 64);
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error().message.find("NoSuchPatch") != std::string::npos);
  CHECK_FALSE(compilePatchFile(casePath("does_not_exist.pqp"), "", 48000.0, 64).ok());
}

// T1.8: an instrument patch (voice outs only) plays the --notes arpeggio through the voice engine, quietly.
TEST_CASE("play: a voice patch plays an arpeggio through the VoiceEngine at <= -24 dBFS") {
  auto c = compilePatchFile(casePath("fm_voice.pqp"), "", 48000.0, 64);
  REQUIRE_MESSAGE(c.ok(), (c.ok() ? "" : c.error().message));
  CHECK(c->outputs == 0);
  CHECK(c->voiceOutputs == 1);
  pq::engine::VoiceEngine voices;
  pq::engine::VoiceConfig vc;
  vc.maxVoices = kPlayVoices;
  REQUIRE(voices.prepare(c->image.get(), vc).ok());
  Arpeggio arp(parsePlayNotes("60 64 67 72").value(), 48000.0);
  const float level = static_cast<float>(std::pow(10.0, -24.0 / 20.0));
  pq::engine::ImageRunner runner;
  REQUIRE(runner.prepare(c->image.get(), 0, 2, level * kPlayVoiceHeadroom, true));  // as runPlay does
  runner.setVoices(&voices, arp.source());
  std::vector<float> l(256), r(256);
  float* outs[2] = {l.data(), r.data()};
  double energy = 0, peak = 0;
  for (int b = 0; b < 48000 * 6 / 256; ++b) {  // 6 s: 24 notes, device blocks of 256 split into 64-frame images
    const pq::engine::AudioBlock blk{nullptr, 0, outs, 2, 256, b * 256};
    CHECK(pq::alloc_trap::countRtViolations([&] { runner.process(blk); }) == 0);
    for (int i = 0; i < 256; ++i) {
      REQUIRE(std::isfinite(l[i]));
      CHECK(l[i] == r[i]);
      energy += l[i] * l[i];
      peak = std::max(peak, static_cast<double>(std::fabs(l[i])));
    }
  }
  CHECK(energy > 1.0);
  CHECK(peak <= level);
  CHECK(voices.stats().noteOns == 24);
  // FmVoice's exponential release (τ = 300 ms) takes ≈ 3 s to reach −90 dB, so each pitch is still sounding when
  // it comes round again (every 1 s): the same-note retrigger takes over its voice. Four voices, never more.
  CHECK(voices.stats().retriggers == 20);
  CHECK(voices.liveVoices() == 4);
  MESSAGE("arpeggio peak " << 20.0 * std::log10(peak) << " dBFS");
}

// T1.x-arch2: --play compiles through pq_compile (the pipeline pqc runs) for the JIT (default) or the interpreter; the
// two images play the same bits (T1b.1-fix1: no contraction by default).
TEST_CASE("play: --backend jit|interp, and both backends play the same samples") {
  std::vector<std::string_view> def{"--play", "x.pqp"};
  REQUIRE(parseCli(def).ok());
  CHECK(parseCli(def)->playBackend == PlayBackend::Jit);
  std::vector<std::string_view> interp{"--play", "x.pqp", "--backend", "interp"};
  REQUIRE(parseCli(interp).ok());
  CHECK(parseCli(interp)->playBackend == PlayBackend::Interpreter);
  std::vector<std::string_view> both{"--play", "x.pqp", "--backend", "jit", "--backend", "rtaudio"};
  REQUIRE(parseCli(both).ok());
  CHECK(parseCli(both)->playBackend == PlayBackend::Jit);
  CHECK(parseCli(both)->wasapi == WasapiImpl::RtAudio);
  std::vector<std::string_view> notPlay{"--sine", "440", "--backend", "jit"};
  CHECK_FALSE(parseCli(notPlay).ok());

  auto j = compilePatchFile(casePath("fm_voice.pqp"), "", 48000.0, 64, pq::compile::Backend::Jit);
  auto i = compilePatchFile(casePath("fm_voice.pqp"), "", 48000.0, 64, pq::compile::Backend::Interpreter);
  REQUIRE_MESSAGE(j.ok(), (j.ok() ? "" : j.error().message));
  REQUIRE_MESSAGE(i.ok(), (i.ok() ? "" : i.error().message));
  CHECK(j->image->io().backend == pq::engine::ImageBackend::Jit);
  CHECK(i->image->io().backend == pq::engine::ImageBackend::Interpreter);
  CHECK(j->ops == i->ops);
  std::vector<float> out[2];
  for (int k = 0; k < 2; ++k) {
    CompiledPlay& c = k == 0 ? *j : *i;
    pq::engine::VoiceEngine voices;
    pq::engine::VoiceConfig vc;
    vc.maxVoices = kPlayVoices;
    REQUIRE(voices.prepare(c.image.get(), vc).ok());
    Arpeggio arp(parsePlayNotes("60 64 67 72").value(), 48000.0);
    pq::engine::ImageRunner runner;
    REQUIRE(runner.prepare(c.image.get(), 0, 1, 1.0f, false));
    runner.setVoices(&voices, arp.source());
    std::vector<float> l(256);
    float* outs[1] = {l.data()};
    for (int b = 0; b < 48000 / 256; ++b) {  // 1 s
      const pq::engine::AudioBlock blk{nullptr, 0, outs, 1, 256, b * 256};
      CHECK(pq::alloc_trap::countRtViolations([&] { runner.process(blk); }) == 0);
      out[k].insert(out[k].end(), l.begin(), l.end());
    }
  }
  REQUIRE(out[0].size() == out[1].size());
  CHECK(std::memcmp(out[0].data(), out[1].data(), out[0].size() * sizeof(float)) == 0);
}
