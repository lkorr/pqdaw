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
