// Colormaps (PQ Meter theme ramps), Gradient and PaletteTrack.

#include <doctest/doctest.h>

#include <cstdlib>
#include <ostream>
#include <vector>

#include "pq/ui/colormap.h"

using namespace pq::ui;

TEST_CASE("colormap: the ten PQ Meter ramps, names and LUT ends") {
  CHECK(colormapName(Colormap::Magma) == "Magma");
  CHECK(colormapName(Colormap::Copper) == "Copper");
  CHECK(colormapStops(Colormap::Inferno).size() == 8);
  CHECK(colormapStops(Colormap::Grayscale).size() == 2);
  for (int m = 0; m < kColormapCount; ++m) {
    const auto stops = colormapStops(static_cast<Colormap>(m));
    CHECK(stops.front().t == 0.0f);
    CHECK(stops.back().t == 1.0f);
    for (size_t i = 1; i < stops.size(); ++i) CHECK(stops[i].t > stops[i - 1].t);
  }
  const ColorLut& magma = colormapLut(Colormap::Magma);
  CHECK(magma[0] == rgba(0, 0, 0));
  CHECK(magma[255] == rgba(255, 255, 237));  // (1.0, 1.0, 0.93)
  // Pastel is the one ramp whose floor is not black.
  CHECK(colormapLut(Colormap::Pastel)[0] == rgba(23, 18, 33));
  const ColorLut& gray = colormapLut(Colormap::Grayscale);
  for (int i = 0; i < 256; ++i) CHECK(gray[static_cast<size_t>(i)] == rgba(static_cast<uint32_t>(i), static_cast<uint32_t>(i), static_cast<uint32_t>(i)));
  CHECK(lutAt(gray, 0.5f) == rgba(128, 128, 128));
  CHECK(lutAt(gray, -1.0f) == rgba(0, 0, 0));
  CHECK(lutAt(gray, 2.0f) == rgba(255, 255, 255));
}

TEST_CASE("colormap: sampleRamp is piecewise linear and clamps") {
  const RampStop s[3] = {{0.0f, 0.0f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 0.0f}};
  ColorF c = sampleRamp(s, 0.25f);
  CHECK(c.r == doctest::Approx(0.5f));
  c = sampleRamp(s, 0.75f);
  CHECK(c.r == doctest::Approx(1.0f));
  CHECK(c.g == doctest::Approx(0.5f));
  c = sampleRamp(s, 1.5f);
  CHECK(c.g == doctest::Approx(1.0f));
  c = sampleRamp(s, -1.0f);
  CHECK(c.r == doctest::Approx(0.0f));
}

TEST_CASE("gradient: seeds from a preset, edits keep stops ordered, activeLut selects") {
  Gradient g;
  g.seed(Colormap::Ice);
  // Stops at their default positions take the preset's colours, and the LUT matches the preset at the stops.
  const auto ice = colormapStops(Colormap::Ice);
  for (int i = 0; i < Gradient::kStops; ++i) {
    const ColorF c = sampleRamp(ice, g.pos[i]);
    CHECK(g.colors[i][0] == doctest::Approx(c.r));
    CHECK(g.colors[i][2] == doctest::Approx(c.b));
  }
  CHECK(g.lut[0] == colormapLut(Colormap::Ice)[0]);
  CHECK(g.lut[255] == colormapLut(Colormap::Ice)[255]);
  // A middle stop cannot pass its neighbours.
  g.setStop(2, {1.0f, 0.0f, 0.0f}, 0.95f);
  CHECK(g.pos[2] == doctest::Approx(g.pos[3] - 0.01f));
  g.setStop(2, {1.0f, 0.0f, 0.0f}, 0.0f);
  CHECK(g.pos[2] == doctest::Approx(g.pos[1] + 0.01f));
  // End stops keep their positions.
  g.setStop(0, {0.2f, 0.2f, 0.2f}, 0.5f);
  CHECK(g.pos[0] == 0.0f);
  CHECK(g.lut[0] == rgba(51, 51, 51));
  CHECK(&g.activeLut(Gradient::kCustomIndex) == &g.lut);
  CHECK(&g.activeLut(2) == &colormapLut(Colormap::Viridis));
  const ColorF mid = g.sample(g.pos[2]);
  CHECK(mid.r == doctest::Approx(1.0f));
}

TEST_CASE("palette track: blends whole LUTs, clamps past the ends, rev tracks content") {
  PaletteTrack tr;
  Gradient g;
  // Off: passes straight through to the gradient / preset pair.
  CHECK(&tr.activeLut(g, 4) == &colormapLut(Colormap::Ice));
  tr.setEnabled(true);
  tr.setPosition(0.0f);
  CHECK(tr.activeLut(g, 4) == colormapLut(Colormap::Magma));
  tr.setPosition(1.0f);
  CHECK(tr.activeLut(g, 4) == colormapLut(Colormap::Viridis));
  tr.setPosition(0.5f);
  const ColorLut& a = colormapLut(Colormap::Magma);
  const ColorLut& b = colormapLut(Colormap::Viridis);
  for (size_t i = 0; i < 256; i += 51) {
    const Rgba c = tr.activeLut(g, 0)[i];
    CHECK(static_cast<int>(redOf(c)) == static_cast<int>(0.5f * static_cast<float>(redOf(a[i]) + redOf(b[i])) + 0.5f));
    CHECK(alphaOf(c) == 255);
  }
  CHECK(tr.evalColorAt(0.5f, 1.0f) == tr.activeLut(g, 0)[255]);

  // rev advances only when the resolved table changes: positions within one quantum don't rebuild.
  const uint32_t r0 = tr.rev();
  tr.setPosition(0.5f + 0.1f / PaletteTrack::kQuant);
  CHECK(tr.rev() == r0);
  tr.setPosition(0.75f);
  CHECK(tr.rev() > r0);
  const uint32_t r1 = tr.rev();
  tr.setEnabled(false);
  CHECK(tr.rev() > r1);  // the active table switched without its content changing
}

TEST_CASE("palette track: inserting a waypoint is invisible until it is reassigned; limits hold") {
  PaletteTrack tr;
  tr.setEnabled(true);
  tr.setPosition(0.5f);
  const ColorLut before = tr.activeLut(Gradient{}, 0);
  const int idx = tr.insertWaypoint();
  REQUIRE(idx == 1);
  CHECK(tr.waypoints().size() == 3);
  CHECK(tr.waypoints()[1].t == doctest::Approx(0.5f));
  // Seeded from the blend at its own position: its six stops sample that blend, so at the stop positions
  // (including both ends) the table is unchanged to within rounding.
  const ColorLut& after = tr.activeLut(Gradient{}, 0);
  for (size_t i : {size_t(0), size_t(255)}) {
    CHECK(std::abs(static_cast<int>(redOf(after[i])) - static_cast<int>(redOf(before[i]))) <= 1);
    CHECK(std::abs(static_cast<int>(greenOf(after[i])) - static_cast<int>(greenOf(before[i]))) <= 1);
    CHECK(std::abs(static_cast<int>(blueOf(after[i])) - static_cast<int>(blueOf(before[i]))) <= 1);
  }
  tr.setWaypointPreset(1, Colormap::Ice);
  CHECK(tr.activeLut(Gradient{}, 0) == colormapLut(Colormap::Ice));
  // Moving keeps a 0.02 gap to the neighbours.
  tr.moveWaypoint(1, 0.999f);
  CHECK(tr.waypoints()[1].t == doctest::Approx(0.98f));
  // At most kMaxPoints, at least kMinPoints.
  while (tr.insertWaypoint() >= 0) {
  }
  CHECK(tr.waypoints().size() == PaletteTrack::kMaxPoints);
  while (tr.removeWaypoint(0)) {
  }
  CHECK(tr.waypoints().size() == PaletteTrack::kMinPoints);
  std::vector<PaletteTrack::Waypoint> one(1);
  CHECK_FALSE(tr.setWaypoints(one));
  std::vector<PaletteTrack::Waypoint> two(2);
  two[0].t = 0.9f;
  two[0].preset = Colormap::Gold;
  two[1].t = 0.1f;
  two[1].preset = Colormap::Neon;
  CHECK(tr.setWaypoints(two));
  CHECK(tr.waypoints()[0].preset == Colormap::Neon);  // sorted by t
}
