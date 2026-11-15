// bug-curve-render: moving notes (glides, drawn curves, vibratos) drawn cleanly at every zoom — in the roll, the
// cached path flattened to a fraction of a device px and simplified in device px, a vibrato faster than a few px per
// cycle drawn as its envelope band; in the arrangement / lane-window clip previews, anti-aliased strokes through the
// curve (tile and direct alike) instead of one rect per pixel column.
// Goldens: pianoroll-curves-far, pianoroll-curves-mid, arrangement-curve-preview.

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <vector>

#include "../../src/views/src/common/pitch_path.h"
#include "piano_roll_fix.h"

using namespace roll_fix;

namespace {

// How far (vertically) point p is from the output polyline: the nearest of the segments over its x (a vertical step
// covers the y between its ends).
float distance(const std::vector<ui::Vec2>& out, ui::Vec2 p) {
  float best = 1e30f;
  for (size_t i = 0; i + 1 < out.size(); ++i) {
    const ui::Vec2 a = out[i], b = out[i + 1];
    if (p.x < a.x || p.x > b.x) continue;
    if (b.x - a.x < 1e-3f) {
      best = std::min(best, p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y) ? 0.0f : std::min(std::fabs(a.y - p.y), std::fabs(b.y - p.y)));
      continue;
    }
    best = std::min(best, std::fabs(a.y + (b.y - a.y) * (p.x - a.x) / (b.x - a.x) - p.y));
  }
  return best;
}

}  // namespace

TEST_CASE("pitch paths: PxPolyline keeps every point within its tolerance in a few vertices, keeps jumps and collapses flat runs") {
  std::vector<ui::Vec2> in, out;
  // A slow swell under a fast wiggle, 10 points per px (what a 1 ¢ flattening of a far zoomed-out note hands it),
  // a flat run, and a jump.
  for (int i = 0; i <= 3000; ++i) {
    const float x = 0.1f * static_cast<float>(i);
    in.push_back({x, 40.0f + 20.0f * std::sin(x / 40.0f) + 3.0f * std::sin(x * 0.9f)});
  }
  for (int i = 1; i <= 500; ++i) in.push_back({300.0f + 0.1f * static_cast<float>(i), 60.0f});
  in.push_back({350.0f, 20.0f});
  for (int i = 1; i <= 100; ++i) in.push_back({350.0f + 0.1f * static_cast<float>(i), 20.0f});
  {
    views::PxPolyline line(out);
    for (const ui::Vec2 p : in) line.add(p);
    line.finish();
  }
  MESSAGE(in.size() << " points in, " << out.size() << " out");
  CHECK(out.size() < in.size() / 10);
  REQUIRE(out.size() >= 2);
  CHECK(out.front().x == in.front().x);
  CHECK(out.back().x == in.back().x);
  for (size_t i = 1; i < out.size(); ++i) CHECK(out[i].x >= out[i - 1].x);
  float worst = 0.0f;
  for (const ui::Vec2 p : in) worst = std::max(worst, distance(out, p));
  CHECK(worst <= views::kPathPxTolerance + 1e-3f);
  // The flat run is its two ends (the first after the corner the wiggle turns into it), and the jump a vertical step.
  size_t onFlat = 0;
  for (const ui::Vec2 p : out) onFlat += (p.x > 300.15f && p.x < 349.95f) ? 1 : 0;
  CHECK(onFlat == 0);
  bool step = false;
  for (size_t i = 0; i + 1 < out.size(); ++i)
    step = step || (out[i].x == 350.0f && out[i + 1].x == 350.0f && out[i].y == 60.0f && out[i + 1].y == 20.0f);
  CHECK(step);
}

TEST_CASE("pitch paths: a vibrato's envelope (doc::VibratoEnvelope) is the swing of its wave, with its corners") {
  doc::NoteVibrato v;
  v.depthCents = -40.0f;  // inverted: the same swing
  v.rate = 6.0f;
  v.onset = 100;
  v.fadeIn = 300;
  v.end = 1000;
  v.fadeOut = 200;
  const doc::VibratoEnvelope e(v, 2000);
  CHECK(e.depth == 40.0);
  CHECK(e.at(100.0) == 0.0);
  CHECK(e.at(250.0) == doctest::Approx(20.0));
  CHECK(e.at(600.0) == doctest::Approx(40.0));
  CHECK(e.at(900.0) == doctest::Approx(20.0));
  CHECK(e.at(1000.0) == 0.0);
  double c[5];
  REQUIRE(e.corners(c) == 4);
  CHECK(c[0] == 100.0);
  CHECK(c[1] == 400.0);
  CHECK(c[2] == 800.0);
  CHECK(c[3] == 1000.0);
  // The wave never leaves the envelope and touches it at its swings.
  doc::NoteCurves rec;
  rec.vibrato = v;
  const double period = 160.0;
  doc::NoteBendTargets t;
  t.vibratoPeriod = period;
  double touch = 0.0;
  for (int i = 0; i <= 2000; ++i) {
    const double x = static_cast<double>(i);
    const double w = doc::noteCurveCents(rec, 2000, t, x);
    CHECK(std::fabs(w) <= e.at(x) + 1e-9);
    touch = std::max(touch, std::fabs(w));
  }
  CHECK(touch == doctest::Approx(40.0).epsilon(0.01));
  // within(): the band's amplitude over [onset, stop], its ends included — a vibrato without fades starts and stops
  // at full depth there (at() is 0 at both ends).
  doc::NoteVibrato plain = v;
  plain.fadeIn = plain.fadeOut = 0;
  const doc::VibratoEnvelope p(plain, 2000);
  CHECK(p.at(100.0) == 0.0);
  CHECK(p.within(100.0) == 40.0);
  CHECK(p.within(1000.0) == 40.0);
  CHECK(e.within(100.0) == 0.0);
  CHECK(e.within(250.0) == doctest::Approx(20.0));
  // A swell: the fades overlap and meet in one corner.
  v.fadeIn = 600;
  v.fadeOut = 600;
  const doc::VibratoEnvelope s(v, 2000);
  REQUIRE(s.corners(c) == 3);
  CHECK(c[1] == doctest::Approx(550.0));
  CHECK(s.at(550.0) == doctest::Approx(40.0 * 450.0 / 600.0));
}

TEST_CASE("render: pianoroll-curves-far (glides, a pen curve and vibratos 32 bars across: fast vibratos as clean bands) matches the golden image") {
  checkGolden("pianoroll-curves-far");
}

TEST_CASE("render: pianoroll-curves-mid (the same clip 8 bars across: every wave drawn as a clean line) matches the golden image") {
  checkGolden("pianoroll-curves-mid");
}

TEST_CASE("render: arrangement-curve-preview (a tall clip preview of moving notes: smooth anti-aliased strokes) matches the golden image") {
  checkGolden("arrangement-curve-preview");
}
