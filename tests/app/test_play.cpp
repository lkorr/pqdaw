// T1.7: `pqdaw --play` — CLI parsing and the compile path (file → ProgramImage), driven without a device.
#include <doctest/doctest.h>

#include <cmath>
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
  auto voice = compilePatchFile(casePath("fm_voice.pqp"), "", 48000.0, 64);
  REQUIRE_FALSE(voice.ok());
  CHECK(voice.error().message.find("voice") != std::string::npos);
  CHECK_FALSE(compilePatchFile(casePath("does_not_exist.pqp"), "", 48000.0, 64).ok());
}
