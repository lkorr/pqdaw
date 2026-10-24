#include <doctest/doctest.h>

#include <cstdio>
#include <ostream>  // doctest streams string_view messages
#include <string_view>
#include <vector>

#include "pq/app/cli.h"
#include "pq/app/commands.h"
#include "pq/ui/image.h"

using namespace pq::app;

namespace {
pq::Result<CliOptions> parse(std::vector<std::string_view> a) { return parseCli(a); }
}  // namespace

TEST_CASE("cli: no arguments opens the window with the DAW shell (a new project); --scene picks another scene") {
  auto o = parse({});
  REQUIRE(o.ok());
  CHECK(o->mode == Mode::Window);
  CHECK(o->scene == "shell");  // T3.9 (Q60): was the T0.7 test pattern
  CHECK(o->project.empty());
  CHECK_FALSE(o->animate);
  o = parse({"--scene", "test-pattern"});
  REQUIRE(o.ok());
  CHECK(o->scene == "test-pattern");
  o = parse({"--animate", "--scene", "test-pattern"});
  REQUIRE(o.ok());
  CHECK(o->animate);
  CHECK(o->scene == "test-pattern");
  // The self-test keeps its own default scene (the test pattern).
  o = parse({"--window-selftest"});
  REQUIRE(o.ok());
  CHECK(o->scene == "test-pattern");
}

TEST_CASE("cli: --screenshot with and without --size") {
  auto o = parse({"--screenshot", "test-pattern", "out.png"});
  REQUIRE(o.ok());
  CHECK(o->mode == Mode::Screenshot);
  CHECK(o->scene == "test-pattern");
  CHECK(o->outPath == "out.png");
  CHECK(o->width == 0);

  o = parse({"--screenshot", "x", "y.png", "--size", "320x200"});
  REQUIRE(o.ok());
  CHECK(o->width == 320);
  CHECK(o->height == 200);
  o = parse({"--size", "1024X768", "--screenshot", "x", "y.png"});
  REQUIRE(o.ok());
  CHECK(o->width == 1024);
}

TEST_CASE("cli: --scale for --screenshot and the window") {
  auto o = parse({"--screenshot", "text", "t.png", "--scale", "1.5"});
  REQUIRE(o.ok());
  CHECK(o->scale == 1.5f);
  o = parse({"--scale", "1.25", "--scene", "widgets-core"});
  REQUIRE(o.ok());
  CHECK(o->mode == Mode::Window);
  CHECK(o->scale == 1.25f);
  CHECK(parse({})->scale == 0.0f);  // default: 1.0 for screenshots, the display scale for the window
  CHECK_FALSE(parse({"--scale"}).ok());
  CHECK_FALSE(parse({"--scale", "0.25"}).ok());
  CHECK_FALSE(parse({"--scale", "5"}).ok());
  CHECK_FALSE(parse({"--scale", "abc"}).ok());
  CHECK_FALSE(parse({"--window-selftest", "--scale", "2"}).ok());
}

TEST_CASE("cli: errors") {
  CHECK_FALSE(parse({"--screenshot", "x"}).ok());
  CHECK_FALSE(parse({"--screenshot", "a", "b.png", "--size"}).ok());
  CHECK_FALSE(parse({"--bogus"}).ok());
  CHECK_FALSE(parse({"--screenshot", "a", "b.png", "--window-selftest"}).ok());
  CHECK_FALSE(parse({"--window-selftest", "--size", "10x10"}).ok());
  for (std::string_view bad : {"0x10", "10x0", "10", "x10", "10x", "-5x5", "16385x2", "10x10x"}) {
    uint32_t w = 7, h = 7;
    CHECK_MESSAGE(!parseSize(bad, w, h), bad);
    CHECK(w == 7);
  }
  uint32_t w = 0, h = 0;
  CHECK(parseSize("1x16384", w, h));
  CHECK(h == 16384);
}

TEST_CASE("cli: --screenshot writes a PNG of the requested size") {
  CliOptions o;
  o.mode = Mode::Screenshot;
  o.scene = "test-pattern";
  o.outPath = "cli_test_screenshot.png";
  o.width = 160;
  o.height = 96;
  REQUIRE(runScreenshot(o) == 0);
  auto img = pq::ui::readPng(o.outPath);
  REQUIRE(img.ok());
  CHECK(img->width == 160);
  CHECK(img->height == 96);
  std::remove(o.outPath.c_str());

  // --scale: --size is logical px, the PNG is device px.
  o.scale = 1.5f;
  REQUIRE(runScreenshot(o) == 0);
  img = pq::ui::readPng(o.outPath);
  REQUIRE(img.ok());
  CHECK(img->width == 240);
  CHECK(img->height == 144);
  std::remove(o.outPath.c_str());

  o.scene = "nope";
  CHECK(runScreenshot(o) == 1);
}

TEST_CASE("cli: --list-devices and --sine") {
  auto o = parse({"--list-devices"});
  REQUIRE(o.ok());
  CHECK(o->mode == Mode::ListDevices);
  CHECK_FALSE(o->asio);
  o = parse({"--list-devices", "--asio"});
  REQUIRE(o.ok());
  CHECK(o->asio);

  o = parse({"--sine", "440"});
  REQUIRE(o.ok());
  CHECK(o->mode == Mode::Sine);
  CHECK(o->sineHz == 440.0);
  CHECK(o->seconds == 3.0);
  CHECK(o->device == 0xFFFFFFFFu);
  CHECK(o->bufferFrames == 256);
  CHECK(o->blockFrames == 32);
  CHECK(o->sampleRate == 0);
  CHECK_FALSE(o->exclusive);

  o = parse({"--sine", "261.626", "--seconds", "0.5", "--device", "7", "--asio", "--rate", "44100", "--buffer", "37",
             "--block", "64", "--exclusive"});
  REQUIRE(o.ok());
  CHECK(o->sineHz == doctest::Approx(261.626));
  CHECK(o->seconds == 0.5);
  CHECK(o->device == 7);
  CHECK(o->asio);
  CHECK(o->sampleRate == 44100);
  CHECK(o->bufferFrames == 37);
  CHECK(o->blockFrames == 64);
  CHECK(o->exclusive);
  // T0.6-fix2: --backend asio (= --asio), ASIO drivers by name, --asio-panel.
  o = parse({"--sine", "440", "--backend", "asio", "--device", "Focusrite USB ASIO", "--buffer", "64"});
  REQUIRE(o.ok());
  CHECK(o->asio);
  CHECK(o->deviceEndpoint == "Focusrite USB ASIO");
  CHECK(o->bufferFrames == 64);
  o = parse({"--list-devices", "--backend", "asio"});
  REQUIRE(o.ok());
  CHECK(o->asio);
  o = parse({"--asio-panel", "--device", "2", "--seconds", "5"});
  REQUIRE(o.ok());
  CHECK(o->mode == Mode::AsioPanel);
  CHECK(o->asio);
  CHECK(o->device == 2);
  CHECK(o->secondsGiven);
  o = parse({"--asio-panel"});
  REQUIRE(o.ok());
  CHECK_FALSE(o->secondsGiven);
  // Flags may come before the mode flag.
  o = parse({"--seconds", "2", "--sine", "100"});
  REQUIRE(o.ok());
  CHECK(o->seconds == 2.0);
}

TEST_CASE("cli: audio flag errors") {
  CHECK_FALSE(parse({"--sine"}).ok());
  CHECK_FALSE(parse({"--sine", "abc"}).ok());
  CHECK_FALSE(parse({"--sine", "0"}).ok());
  CHECK_FALSE(parse({"--sine", "-5"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--seconds"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--seconds", "0"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--device", "x"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--device", "-1"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--buffer", "0"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--block", "4097"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--rate", "0"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--list-devices"}).ok());  // two modes
  CHECK_FALSE(parse({"--list-devices", "--seconds", "2"}).ok());  // --sine-only flag
  // T3.17: the DAW window plays through a device, so it takes the device flags (they used to be refused there).
  CHECK(parse({"--device", "3"}).ok());
  CHECK(parse({"--asio"}).ok());
  CHECK(parse({"--buffer", "128", "--exclusive", "--backend", "wasapi"}).ok());
  CHECK_FALSE(parse({"--seconds", "2"}).ok());          // the window has no duration
  CHECK_FALSE(parse({"--rate", "44100"}).ok());         // the engine runs at 48 kHz until T3.18
  CHECK_FALSE(parse({"--render", "x.wav"}).ok());       // --render needs --project
  CHECK_FALSE(parse({"--project", "p", "--render", "x.wav", "--device", "3"}).ok());
  {
    auto r = parse({"--project", "p", "--render", "x.wav", "--seconds", "4"});
    REQUIRE(r.ok());
    CHECK(r->mode == pq::app::Mode::Render);
    CHECK(r->outPath == "x.wav");
    CHECK(r->project == "p");
    CHECK(r->seconds == 4.0);
  }
  CHECK_FALSE(parse({"--screenshot", "a", "b.png", "--asio"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--backend", "bogus"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--device", "Focusrite USB ASIO"}).ok());  // names select ASIO drivers only
  CHECK_FALSE(parse({"--sine", "440", "--asio", "--device", "{0.0.0.00000000}.{x}"}).ok());  // WASAPI endpoint id
  CHECK_FALSE(parse({"--asio-panel", "--buffer", "64"}).ok());
  CHECK_FALSE(parse({"--asio-panel", "--rate", "48000"}).ok());
  CHECK_FALSE(parse({"--asio-panel", "--sine", "440"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--level", "3"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--inject-stall", "0"}).ok());
  CHECK_FALSE(parse({"--sine", "440", "--inject-stall", "5", "--backend", "rtaudio"}).ok());
  CHECK_FALSE(parse({"--latency-test", "--exclusive"}).ok());       // --sine-only flag
  CHECK_FALSE(parse({"--latency-test", "--backend", "wasapi"}).ok());
  CHECK_FALSE(parse({"--list-devices", "--level", "-20"}).ok());
}

TEST_CASE("cli: WASAPI backend selection, endpoint ids, level, stall injection, latency test") {
  auto o = parse({"--sine", "440"});
  REQUIRE(o.ok());
  CHECK(o->wasapi == WasapiImpl::Own);
  CHECK(o->levelDb == -24.0);
  CHECK(o->stallAfterWake == 0);
  CHECK(o->deviceEndpoint.empty());
  o = parse({"--sine", "440", "--backend", "rtaudio", "--level", "-60"});
  REQUIRE(o.ok());
  CHECK(o->wasapi == WasapiImpl::RtAudio);
  CHECK(o->levelDb == -60.0);
  o = parse({"--list-devices", "--backend", "wasapi"});
  REQUIRE(o.ok());
  CHECK(o->wasapi == WasapiImpl::Own);
  o = parse({"--sine", "440", "--exclusive", "--buffer", "144", "--inject-stall", "100", "--device",
             "{0.0.0.00000000}.{d1260fc0-59d5-4f42-8c39-81326683797d}"});
  REQUIRE(o.ok());
  CHECK(o->stallAfterWake == 100);
  CHECK(o->deviceEndpoint == "{0.0.0.00000000}.{d1260fc0-59d5-4f42-8c39-81326683797d}");
  CHECK(o->device == 0xFFFFFFFFu);
  o = parse({"--latency-test", "--seconds", "2", "--buffer", "480", "--device", "3"});
  REQUIRE(o.ok());
  CHECK(o->mode == Mode::LatencyTest);
  CHECK(o->seconds == 2.0);
  CHECK(o->device == 3);
}
