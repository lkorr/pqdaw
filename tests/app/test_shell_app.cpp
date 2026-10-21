// T3.7 app side: the --project / --idle-check CLI, `pqdaw --screenshot shell --project tests/views/fixtures/empty`
// against its golden (the same function main() calls), and the SimulatedTransport (the app's TransportPort until
// T3.17) on a fake clock.

#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "pq/app/cli.h"
#include "pq/app/commands.h"
#include "pq/app/sim_transport.h"
#include "pq/ui/image.h"
#include "../ui/golden_check.h"

using namespace pq;
using namespace pq::app;

namespace {
Result<CliOptions> parse(std::vector<std::string_view> a) { return parseCli(a); }
std::string fixture(std::string_view name) { return std::string(PQ_TEST_DATA_DIR) + "/views/fixtures/" + std::string(name); }
}  // namespace

TEST_CASE("cli: --project opens the shell; --idle-check") {
  auto o = parse({"--project", "C:/x"});
  REQUIRE(o.ok());
  CHECK(o->mode == Mode::Window);
  CHECK(o->scene == "shell");
  CHECK(o->project == "C:/x");
  o = parse({"--scene", "shell"});  // the DAW with a new project
  REQUIRE(o.ok());
  CHECK(o->project.empty());
  o = parse({"--screenshot", "shell", "s.png", "--project", "p"});
  REQUIRE(o.ok());
  CHECK(o->mode == Mode::Screenshot);
  CHECK(o->project == "p");
  o = parse({"--project", "p", "--idle-check", "10"});
  REQUIRE(o.ok());
  CHECK(o->idleCheckSeconds == 10.0);
  CHECK_FALSE(parse({"--project"}).ok());
  CHECK_FALSE(parse({"--project", "p", "--scene", "widgets"}).ok());  // not a view scene
  CHECK_FALSE(parse({"--screenshot", "text", "t.png", "--project", "p"}).ok());
  CHECK_FALSE(parse({"--list-devices", "--project", "p"}).ok());
  CHECK_FALSE(parse({"--idle-check", "0"}).ok());
  CHECK_FALSE(parse({"--screenshot", "shell", "s.png", "--idle-check", "1"}).ok());
}

TEST_CASE("app: --screenshot shell --project tests/views/fixtures/empty matches its golden") {
  CliOptions o;
  o.mode = Mode::Screenshot;
  o.scene = "shell";
  o.project = fixture("empty");
  o.outPath = "app_shell_screenshot.png";
  REQUIRE(runScreenshot(o) == 0);
  auto img = ui::readPng(o.outPath);
  REQUIRE(img.ok());
  CHECK(img->width == 1280);
  CHECK(img->height == 800);
  auto ref = ui::readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/shell.png");
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ui::ImageDiff d;
  CHECK(ui::test::matchesGolden("app_shell", *img, *ref, true, &d));
  MESSAGE("pqdaw --screenshot shell: " << ui::test::describe(d));
  // A project that does not load is an error, not a screenshot of something else.
  o.project = fixture("does-not-exist");
  CHECK(runScreenshot(o) == 1);
}

TEST_CASE("sim transport: play / stop / continue / locate follow the clock and the tempo map") {
  double now = 100.0;
  SimulatedTransport t([&] { return now; });
  CHECK_FALSE(t.playhead().playing);
  CHECK(t.playhead().tick == 0);
  t.play(kTicksPerWhole);  // from bar 2
  now += 0.5;              // 120 BPM: one beat
  auto p = t.playhead();
  CHECK(p.playing);
  CHECK(p.tick == kTicksPerWhole + kPpq);
  const uint64_t seq = p.sequence;
  CHECK(t.playhead().sequence > seq);  // a running transport reports new positions
  t.stop();
  now += 3.0;
  CHECK_FALSE(t.playhead().playing);
  CHECK(t.playhead().tick == kTicksPerWhole + kPpq);  // stopped where it was
  t.continuePlayback();
  now += 1.0;
  CHECK(t.playhead().tick == kTicksPerWhole + 3 * kPpq);
  t.locate(0);  // while playing: jumps and keeps playing
  now += 0.5;
  CHECK(t.playhead().tick == kPpq);
  // Tempo change while playing: the playhead keeps its tick and continues at the new tempo.
  TempoMap fast = *TempoMap::create(std::vector<TempoPoint>{{0, 240.0, TempoCurve::Step}});
  t.setTempoMap(fast);
  CHECK(t.playhead().tick == kPpq);
  now += 0.5;  // 240 BPM: two beats
  CHECK(t.playhead().tick == 3 * kPpq);
  t.stop();
  t.locate(5 * kPpq);  // while stopped: where continue resumes
  t.continuePlayback();
  CHECK(t.playhead().tick == 5 * kPpq);
}

TEST_CASE("sim transport: the loop wraps when playback started before its end") {
  double now = 0.0;
  SimulatedTransport t([&] { return now; });
  t.setLoop({kTicksPerWhole, 2 * kTicksPerWhole, true});  // bar 2, one bar long (2 s at 120 BPM)
  t.play(0);
  now += 3.0;  // 6 beats: bar 2 beat 3
  CHECK(t.playhead().tick == kTicksPerWhole + 2 * kPpq);
  now += 2.0;  // 4 more beats: wrapped once, same place
  CHECK(t.playhead().tick == kTicksPerWhole + 2 * kPpq);
  t.locate(3 * kTicksPerWhole);  // after the loop: no wrap
  now += 2.0;
  CHECK(t.playhead().tick == 4 * kTicksPerWhole);
  t.setLoop({kTicksPerWhole, 2 * kTicksPerWhole, false});
  t.locate(0);
  now += 6.0;
  CHECK(t.playhead().tick == 3 * kTicksPerWhole);
}
