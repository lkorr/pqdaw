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

TEST_CASE("cli: no arguments opens the window") {
  auto o = parse({});
  REQUIRE(o.ok());
  CHECK(o->mode == Mode::Window);
  CHECK(o->scene == "test-pattern");
  CHECK_FALSE(o->animate);
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
  CHECK_FALSE(parse({"--device", "3"}).ok());
  CHECK_FALSE(parse({"--asio"}).ok());
  CHECK_FALSE(parse({"--screenshot", "a", "b.png", "--asio"}).ok());
}
