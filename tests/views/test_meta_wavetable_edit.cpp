// TMM.9: the Wavetable face's harmonic editor and draw tools — the editing math (a harmonic edit changes only the
// harmonics edited; strokes: pencil, line, Catmull-Rom curve, the snap grid), every gesture through the project patch
// view as one undo step (paint / line / reset across bars, phase edits, scroll and zoom to all 1024 and back, the draw
// tools on the selected keyframe and on a new keyframe made from the blended frame, the options), drawing ↔ harmonics
// lossless both ways, the goldens meta-wavetable-harmonics (zoomed to 256) and meta-wavetable-draw, and the harmonic
// rows' paint cost with 512 bars.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../../src/views/src/meta/wavetable_edit.h"
#include "../ui/golden_check.h"
#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/meta/registry.h"
#include "pq/meta/wavetable.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/theme.h"
#include "pq/views/circuit/circuit_canvas.h"
#include "pq/views/circuit/circuit_viewer.h"
#include "pq/views/fake_transport.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = std::numbers::pi;
const meta::Type& wtType() { return meta::wavetableType(); }

std::vector<float> basic(dsp::BasicCycle c) {
  const AlignedBuffer<float> b = dsp::basicCycle(c);
  return {b.span().begin(), b.span().end()};
}

// The largest change of any harmonic (0 … 1023, and Nyquist) between two cycles, skipping `skip`.
double otherHarmonicChange(const std::vector<float>& a, const std::vector<float>& b, const std::vector<uint32_t>& skip) {
  wt::Spectrum1 sa, sb;
  wt::cycleSpectrum(a, sa);
  wt::cycleSpectrum(b, sb);
  double worst = std::fabs(sa.nyquist - sb.nyquist);
  for (uint32_t h = 0; h < wt::kHarmonicCount; ++h) {
    if (std::find(skip.begin(), skip.end(), h) != skip.end()) continue;
    worst = std::max(worst, std::abs(sa.c[h] - sb.c[h]));
  }
  return worst;
}

}  // namespace

// ---- the editing math -----------------------------------------------------------------------------------------------

TEST_CASE("wavetable edit: a harmonic edit changes only the harmonics edited (others ≤ 1e-6); viewing changes nothing") {
  // A drawn-looking cycle: a saw with a bump and some noise (every harmonic present).
  std::vector<float> cycle = basic(dsp::BasicCycle::Saw);
  for (uint32_t i = 0; i < wt::kCycleLen; ++i)
    cycle[i] = 0.7f * cycle[i] + 0.2f * std::exp(-0.5f * std::pow((static_cast<float>(i) - 600.0f) / 40.0f, 2.0f)) +
               0.01f * std::sin(static_cast<float>(i) * 1.7f);
  const std::vector<float> before = cycle;
  wt::Spectrum1 s;
  wt::cycleSpectrum(cycle, s);
  CHECK(cycle == before);  // a view is a view
  // Amplitude of harmonic 3 → 0.5, the phase of harmonic 7 → +π/4, DC → 0.1, harmonic 1000 → 0.02 (new: a sine).
  std::vector<std::complex<double>> want = s.c;
  std::vector<uint8_t> edited(wt::kHarmonicCount, 0);
  want[3] = wt::withAmp(s.c[3], 3, 0.5), edited[3] = 1;
  want[7] = wt::withPhase(s.c[7], 7, 0.25 * kPi), edited[7] = 1;
  want[0] = wt::withAmp(s.c[0], 0, 0.1), edited[0] = 1;
  want[1000] = wt::withAmp({}, 1000, 0.02), edited[1000] = 1;
  std::vector<float> out(wt::kCycleLen);
  wt::HarmonicWriter w;
  w.write(cycle, s, want, edited, out);
  wt::Spectrum1 t;
  wt::cycleSpectrum(out, t);
  CHECK(wt::harmonicAmp(t, 3) == doctest::Approx(0.5).epsilon(1e-5));
  CHECK(std::arg(t.c[3]) == doctest::Approx(std::arg(s.c[3])).epsilon(1e-5));  // its phase kept
  CHECK(wt::harmonicPhase(t, 7) == doctest::Approx(0.25 * kPi).epsilon(1e-5));
  CHECK(wt::harmonicAmp(t, 7) == doctest::Approx(wt::harmonicAmp(s, 7)).epsilon(1e-5));  // its amplitude kept
  CHECK(t.c[0].real() == doctest::Approx(0.1 * (s.c[0].real() < 0 ? -1 : 1)).epsilon(1e-5));
  CHECK(wt::harmonicAmp(t, 1000) == doctest::Approx(0.02).epsilon(1e-4));
  CHECK(wt::harmonicPhase(t, 1000) == doctest::Approx(0.0).scale(1.0).epsilon(1e-3));  // a new harmonic: a sine
  const double other = otherHarmonicChange(cycle, out, {0, 3, 7, 1000});
  MESSAGE("largest change of a harmonic not edited: " << other);
  CHECK(other <= 1e-6);
  // Nothing edited: the samples come back byte-equal.
  std::fill(edited.begin(), edited.end(), uint8_t{0});
  w.write(cycle, s, want, edited, out);
  CHECK(out == cycle);
  // A phase on an absent harmonic changes nothing (it has none).
  CHECK(wt::withPhase({}, 5, 1.0) == std::complex<double>{});
}

TEST_CASE("wavetable edit: strokes — pencil linear between pointer samples, line, Catmull-Rom curve, the snap grid") {
  const std::vector<float> zero(wt::kCycleLen, 0.0f);
  wt::Stroke st;
  // Pencil from (0.25, 0.5) to (0.5, -0.5): linear across the samples between; the rest untouched.
  st.begin(zero, wt::Tool::Pencil, 0, 0.25f, 0.5f);
  st.to(0.5f, -0.5f);
  auto c = st.cycle();
  CHECK(c[512] == doctest::Approx(0.5f));
  CHECK(c[768] == doctest::Approx(0.0f).scale(1.0f));
  CHECK(c[1024] == doctest::Approx(-0.5f));
  CHECK(c[511] == 0.0f);
  CHECK(c[1025] == 0.0f);
  // ... then back left to 0.375 at 1: overwrites between, keeps the rest of the first segment.
  st.to(0.375f, 1.0f);
  c = st.cycle();
  CHECK(c[768] == doctest::Approx(1.0f));
  CHECK(c[896] == doctest::Approx(0.25f));  // halfway from (-0.5 at 1024) to (1 at 768)
  CHECK(c[600] == doctest::Approx(0.5f + (600.0f - 512.0f) / 512.0f * -1.0f));
  // Values clamp to ±1.
  st.to(0.9f, 3.0f);
  CHECK(st.cycle()[static_cast<size_t>(0.9 * 2048)] == 1.0f);
  // Line: from the press to the pointer, re-drawn from the original each step.
  st.begin(zero, wt::Tool::Line, 0, 0.0f, -1.0f);
  st.to(0.5f, 0.0f);
  st.to(1.0f, 1.0f);
  c = st.cycle();
  for (uint32_t i = 0; i < wt::kCycleLen; i += 128) CHECK(c[i] == doctest::Approx(-1.0f + 2.0f * i / 2048.0f).epsilon(1e-4));
  // Curve: passes through its points, smooth between (no kink: the second difference stays small).
  st.begin(zero, wt::Tool::Curve, 0, 0.0f, 0.0f);
  st.to(0.25f, 0.8f);
  st.to(0.5f, 0.0f);
  st.to(0.75f, -0.8f);
  st.to(1.0f, 0.0f);
  c = st.cycle();
  CHECK(c[512] == doctest::Approx(0.8f));
  CHECK(c[1024] == doctest::Approx(0.0f).scale(1.0f));
  CHECK(c[1536] == doctest::Approx(-0.8f));
  double worst2 = 0.0;
  for (uint32_t i = 1; i + 1 < wt::kCycleLen - 1; ++i) worst2 = std::max(worst2, std::fabs(double(c[i - 1]) - 2.0 * c[i] + c[i + 1]));
  CHECK(worst2 < 1e-4);  // C1 Hermite spans: no corners
  CHECK(c[256] > 0.4f);  // rises towards the first point
  // Grid 8: the pencil fills whole columns with values in steps of 0.25; line points land on column edges.
  st.begin(zero, wt::Tool::Pencil, 8, 0.3f, 0.6f);  // column 2, value 0.5
  c = st.cycle();
  for (uint32_t i = 512; i < 768; ++i) CHECK(c[i] == 0.5f);
  CHECK(c[511] == 0.0f);
  CHECK(c[768] == 0.0f);
  float x = 0.3f, v = 0.6f;
  wt::Stroke::snap(wt::Tool::Line, 8, x, v);
  CHECK(x == 0.25f);
  CHECK(v == 0.5f);
}

// ---- gestures through the project patch view -------------------------------------------------------------------

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// Face-local geometry (world units; wavetable_face.cpp's layout).
constexpr float kHarmX0 = 16.0f, kHarmX1 = 188.0f;
constexpr float kAmpY0 = 36.0f, kAmpY1 = 58.0f;
constexpr float kPhY0 = 60.5f, kPhY1 = 71.5f;
constexpr float kDrawX0 = 20.0f, kDrawW = 138.0f, kDrawCy = 129.0f, kDrawA = 35.0f;
float barX(uint32_t h, uint32_t first = 1, uint32_t count = 32) {  // the centre of bar h
  const float pitch = (kHarmX1 - kHarmX0) / static_cast<float>(count);
  return kHarmX0 + pitch * (static_cast<float>(h - first) + 0.5f);
}
float ampY(float a) { return kAmpY1 - 0.5f - a * (kAmpY1 - kAmpY0 - 1.0f); }
float phaseY(float p) { return 0.5f * (kPhY0 + kPhY1) - p / static_cast<float>(kPi) * (0.5f * (kPhY1 - kPhY0) - 0.5f); }
float posY(float u) { return 172.0f - 94.0f * u; }
Vec2 toolAt(uint32_t i) { return {51.0f + static_cast<float>(i) * 14.5f + 6.5f, 24.0f}; }
Vec2 optAt(uint32_t i) { return {19.0f + static_cast<float>(i) * 13.5f + 6.0f, 85.5f}; }
Vec2 drawAt(float x, float v) { return {kDrawX0 + kDrawW * x, kDrawCy - kDrawA * v}; }

struct WtUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  doc::DeviceId wave, pair;

  WtUi() {
    ctx.setDeviceLibrary(&library());
    REQUIRE(project_patch::buildMetaWavetableProject(ctx.store(), &pair).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setShown(true, false);
    d.tree().layoutNow();
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (t.name == "Wave") wave = t.devices.at(0)->id;
    });
    REQUIRE(wave.valid());
    frame(2.0);
  }
  ~WtUi() { d.tree().setRoot(std::make_unique<Widget>()); }

  project_patch::ProjectPatchView& view() const {
    project_patch::ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Device& dev() const {
    const doc::Device* dv = doc::findDevice(*ctx.doc(), wave).device;
    REQUIRE(dv != nullptr);
    return *dv;
  }
  double value(const char* param) const {
    return doc::metaParamValue(dev(), "", wtType(), static_cast<size_t>(wtType().paramIndex(param)));
  }
  meta::WavetableData data() const {
    const doc::MetaInstance* inst = dev().metaAt("");
    REQUIRE(inst != nullptr);
    REQUIRE(inst->data);
    auto r = meta::parseWavetableData(*inst->data);
    REQUIRE(r.ok());
    return std::move(*r);
  }
  const project_patch::Node& node() const { return view().patch().nodes[view().patch().nodeOf(wave)]; }
  void frame(double zoom) {
    const project_patch::Node& n = node();
    project_patch::ProjectCanvas& c = view().canvas();
    const double us = d.tree().scale();
    circuit::CircuitCanvas::View v;
    v.zoom = zoom;
    v.x = n.x + 102.0 - c.rect().w / (zoom * us) * 0.5;
    v.y = n.y + 124.0 - c.rect().h / (zoom * us) * 0.5;
    c.setView(v);
    for (int i = 0; i < 4; ++i) d.step();
  }
  Vec2 at(Vec2 l) const {
    const project_patch::Node& n = node();
    return view().canvas().toDevice({n.x + l.x, n.y + l.y});
  }
  std::string infoAt(Vec2 l) {
    d.move(at(l));
    return std::string(view().canvas().info().title) + " | " + std::string(view().canvas().info().body);
  }
};

}  // namespace

TEST_CASE("wavetable face: harmonic rows — paint across bars, Shift line, right-click / double-click reset, phase") {
  WtUi u;
  // Keyframe 0 (the saw) is selected. Paint bars 2 … 5 to 0.5 in one drag: one undo step, only they change.
  const std::vector<float> saw = u.data().keyframes[0].cycle;
  size_t h0 = u.history();
  u.d.drag(u.at({barX(2), ampY(0.5f)}), u.at({barX(5), ampY(0.5f)}), 6);
  CHECK(u.history() == h0 + 1);
  std::vector<float> now = u.data().keyframes[0].cycle;
  wt::Spectrum1 s;
  wt::cycleSpectrum(now, s);
  for (uint32_t h = 2; h <= 5; ++h) CHECK(wt::harmonicAmp(s, h) == doctest::Approx(0.5).epsilon(0.02));
  CHECK(otherHarmonicChange(saw, now, {2, 3, 4, 5}) <= 1e-6);
  CHECK(u.data().keyframes[1].cycle == basic(dsp::BasicCycle::Square));  // other keyframes untouched
  // Shift: a straight line from bar 6 at 0.8 to bar 10 at 0 (bars between on the line).
  h0 = u.history();
  u.d.drag(u.at({barX(6), ampY(0.8f)}), u.at({barX(10), ampY(0.0f)}), 7, mod::kShift);
  CHECK(u.history() == h0 + 1);
  wt::cycleSpectrum(u.data().keyframes[0].cycle, s);
  CHECK(wt::harmonicAmp(s, 6) == doctest::Approx(0.8).epsilon(0.03));
  CHECK(wt::harmonicAmp(s, 8) == doctest::Approx(0.4).epsilon(0.05));
  CHECK(wt::harmonicAmp(s, 10) < 0.02f);
  // Right-click resets a bar (amplitude 0): one step; the host's menu does not open.
  h0 = u.history();
  u.d.click(u.at({barX(3), ampY(0.9f)}), mod::kNone, PointerButton::Right);
  CHECK(u.history() == h0 + 1);
  wt::cycleSpectrum(u.data().keyframes[0].cycle, s);
  CHECK(wt::harmonicAmp(s, 3) < 1e-6f);
  CHECK(wt::harmonicAmp(s, 2) == doctest::Approx(0.5).epsilon(0.02));
  // A right-drag resets every bar it crosses.
  u.d.drag(u.at({barX(4), ampY(0.5f)}), u.at({barX(6), ampY(0.5f)}), 4, mod::kNone, PointerButton::Right);
  wt::cycleSpectrum(u.data().keyframes[0].cycle, s);
  for (uint32_t h = 4; h <= 6; ++h) CHECK(wt::harmonicAmp(s, h) < 1e-6f);
  // Phase row: harmonic 1 to +π/2 (its amplitude kept, the others untouched).
  const std::vector<float> beforePhase = u.data().keyframes[0].cycle;
  const float a1 = wt::harmonicAmp(s, 1);
  h0 = u.history();
  u.d.drag(u.at({barX(1), phaseY(0.5f * static_cast<float>(kPi))}), u.at({barX(1), phaseY(0.5f * static_cast<float>(kPi)) + 0.01f}), 2);
  CHECK(u.history() == h0 + 1);
  wt::cycleSpectrum(u.data().keyframes[0].cycle, s);
  CHECK(wt::harmonicPhase(s, 1) == doctest::Approx(0.5 * kPi).epsilon(0.03));
  CHECK(wt::harmonicAmp(s, 1) == doctest::Approx(a1).epsilon(1e-5));
  CHECK(otherHarmonicChange(beforePhase, u.data().keyframes[0].cycle, {1}) <= 1e-6);
  // Double-click resets a bar's phase (back to a sine's, 0).
  u.d.doubleClick(u.at({barX(1), phaseY(1.0f)}));
  wt::cycleSpectrum(u.data().keyframes[0].cycle, s);
  CHECK(wt::harmonicPhase(s, 1) == doctest::Approx(0.0).scale(1.0).epsilon(1e-4));
  // Undo walks back every step; the saw again.
  while (u.data().keyframes[0].cycle != saw) REQUIRE(u.ctx.undo());
  CHECK(u.data().keyframes[0].cycle == saw);
}

TEST_CASE("wavetable face: harmonic view — wheel scrolls, Ctrl+wheel zooms to all 1024 (DC included) and back") {
  WtUi u;
  const Vec2 rows{kHarmX0 + 0.05f, ampY(0.5f)};  // at the left edge: zooming keeps the first bar first
  const size_t h0 = u.history();
  CHECK(u.infoAt(rows).find("showing 1-32 of") != std::string::npos);
  // Scroll right two notches (an eighth of the view each), then back past the start: DC appears.
  u.d.wheel(u.at(rows), -2.0f);
  CHECK(u.infoAt(rows).find("showing 9-40 of") != std::string::npos);
  u.d.wheel(u.at(rows), 5.0f);
  CHECK(u.infoAt(rows).find("showing 0-31 of") != std::string::npos);
  CHECK(u.infoAt(rows).rfind("DC |", 0) == 0);  // the first bar is DC
  // Zoom out (√2 per notch) until every harmonic shows, then back in to 32.
  for (int i = 0; i < 12; ++i) u.d.wheel(u.at(rows), -1.0f, 0.0f, mod::kCtrl);
  CHECK(u.infoAt(rows).find("showing 0-1023 of") != std::string::npos);
  for (int i = 0; i < 10; ++i) u.d.wheel(u.at(rows), 1.0f, 0.0f, mod::kCtrl);
  CHECK(u.infoAt(rows).find("showing 0-31 of") != std::string::npos);
  // The view is not the document: nothing recorded, and the canvas did not zoom (the face took the wheel).
  CHECK(u.history() == h0);
  CHECK(u.view().canvas().view().zoom == doctest::Approx(2.0));
  // Elsewhere on the face the wheel is the canvas's.
  u.d.wheel(u.at({89.0f, 212.0f}), 1.0f, 0.0f, mod::kCtrl);
  CHECK(u.view().canvas().view().zoom > 2.0);
}

TEST_CASE("wavetable face: draw tools — pencil, line, curve on the selected keyframe; one undo step per stroke; lossless both ways") {
  WtUi u;
  // Choose the pencil (view state: no undo step) and draw across the selected keyframe (0) from 0.1 to 0.4.
  size_t h0 = u.history();
  u.d.click(u.at(toolAt(0)));
  CHECK(u.history() == h0);
  CHECK(u.infoAt(drawAt(0.5f, 0.0f)).rfind("Draw keyframe 1", 0) == 0);
  const std::vector<float> saw = u.data().keyframes[0].cycle;
  u.d.drag(u.at(drawAt(0.1f, 0.6f)), u.at(drawAt(0.4f, -0.6f)), 8);
  CHECK(u.history() == h0 + 1);
  std::vector<float> drawn = u.data().keyframes[0].cycle;
  CHECK(drawn[static_cast<size_t>(0.25 * 2048)] == doctest::Approx(0.0f).scale(1.0f).epsilon(0.03));
  CHECK(drawn[static_cast<size_t>(0.12 * 2048)] > 0.4f);
  CHECK(drawn[1800] == saw[1800]);  // outside the stroke: untouched
  // Lossless both ways: the harmonic view (scrolled, zoomed, hovered) writes nothing; the cycle stays byte-equal.
  const Vec2 rows{kHarmX0 + 20.0f, ampY(0.5f)};
  for (int i = 0; i < 12; ++i) u.d.wheel(u.at(rows), -1.0f, 0.0f, mod::kCtrl);
  (void)u.infoAt(rows);
  for (int i = 0; i < 10; ++i) u.d.wheel(u.at(rows), 1.0f, 0.0f, mod::kCtrl);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(u.data().keyframes[0].cycle == drawn);
  // ... and a harmonic edit after the drawing keeps every other harmonic of the drawing (the view scrolled to DC).
  u.d.wheel(u.at(rows), 40.0f);
  REQUIRE(u.infoAt(rows).find("showing 0-31 of") != std::string::npos);
  u.d.drag(u.at({barX(2, 0), ampY(0.3f)}), u.at({barX(2, 0), ampY(0.3f) + 0.01f}), 2);
  CHECK(otherHarmonicChange(drawn, u.data().keyframes[0].cycle, {2}) <= 1e-6);
  REQUIRE(u.ctx.undo());
  CHECK(u.data().keyframes[0].cycle == drawn);
  // Line with the 8-column grid: endpoints on column edges, values in quarter steps.
  u.d.click(u.at(toolAt(1)));
  h0 = u.history();
  u.d.click(u.at(optAt(0)));  // grid 8
  CHECK(u.history() == h0);
  CHECK(u.infoAt(optAt(0)).find("8 columns") != std::string::npos);
  u.d.drag(u.at(drawAt(0.51f, -0.9f)), u.at(drawAt(0.74f, 0.55f)), 5);
  CHECK(u.history() == h0 + 1);
  drawn = u.data().keyframes[0].cycle;
  CHECK(drawn[1024] == doctest::Approx(-1.0f));  // (0.5, -1) …
  CHECK(drawn[1536] == doctest::Approx(0.5f));   // … to (0.75, 0.5)
  CHECK(drawn[1280] == doctest::Approx(-0.25f).epsilon(1e-3));
  // Curve: a smooth stroke through the pointer's path.
  u.d.click(u.at(optAt(0)));  // grid 16
  u.d.click(u.at(optAt(0)));  // 32
  u.d.click(u.at(optAt(0)));  // off
  u.d.click(u.at(toolAt(2)));
  h0 = u.history();
  const Vec2 a = u.at(drawAt(0.05f, 0.0f));
  u.d.down(a);
  for (int i = 1; i <= 20; ++i) {
    const float x = 0.05f + 0.04f * static_cast<float>(i);
    u.d.move(u.at(drawAt(x, 0.8f * std::sin(2.0f * static_cast<float>(kPi) * x))));
  }
  u.d.up(u.at(drawAt(0.85f, 0.8f * std::sin(2.0f * static_cast<float>(kPi) * 0.85f))));
  CHECK(u.history() == h0 + 1);
  drawn = u.data().keyframes[0].cycle;
  CHECK(drawn[512] == doctest::Approx(0.8f).epsilon(0.05));   // sin at 0.25
  CHECK(drawn[1536] == doctest::Approx(-0.8f).epsilon(0.05));  // at 0.75
  // Normalise and remove DC: table flags, one step each.
  h0 = u.history();
  u.d.click(u.at(optAt(1)));
  u.d.click(u.at(optAt(2)));
  CHECK(u.history() == h0 + 2);
  CHECK(u.data().normalise);
  CHECK(u.data().removeDc);
  // The lit tool again (or Esc) leaves draw mode: the display is the block again.
  u.d.click(u.at(toolAt(2)));
  CHECK(u.infoAt(drawAt(0.5f, 0.0f)).rfind("Wavetable |", 0) == 0);
  u.d.click(u.at(toolAt(0)));
  CHECK(u.d.keyDown(key::kEscape));
  CHECK(u.infoAt(drawAt(0.5f, 0.0f)).rfind("Wavetable |", 0) == 0);
}

TEST_CASE("wavetable face: after a position drag, edits follow the position; between keyframes the first one makes a keyframe") {
  WtUi u;
  const size_t keys = u.data().keyframes.size();
  REQUIRE(keys == 3u);
  // Drag the position from 0.35 to ≈ 0.25 (between the keyframes at 0 and 0.5).
  Vec2 a = u.at({178.0f, posY(0.35f)});
  u.d.drag(a, {a.x, a.y + 0.1f * 94.0f * 2.0f * static_cast<float>(u.d.tree().scale())}, 4);
  const float pos = static_cast<float>(u.value("position"));
  CHECK(pos < 0.3f);
  CHECK(pos > 0.2f);
  // The pencil: the display says the stroke makes a new keyframe.
  u.d.click(u.at(toolAt(0)));
  CHECK(u.infoAt(drawAt(0.5f, 0.0f)).rfind("Draw (a new keyframe)", 0) == 0);
  const size_t h0 = u.history();
  u.d.drag(u.at(drawAt(0.2f, 0.9f)), u.at(drawAt(0.3f, 0.9f)), 4);
  CHECK(u.history() == h0 + 1);  // the new keyframe and the stroke: one step
  const meta::WavetableData d = u.data();
  REQUIRE(d.keyframes.size() == keys + 1);
  CHECK(d.keyframes[1].position == doctest::Approx(pos));
  CHECK(d.keyframes[1].cycle[static_cast<size_t>(0.25 * 2048)] == doctest::Approx(0.9f));
  CHECK(d.gaps.size() == keys);
  // Now the keyframe sits at the position: the next stroke edits it (no new keyframe).
  CHECK(u.infoAt(drawAt(0.5f, 0.0f)).rfind("Draw keyframe 2", 0) == 0);
  u.d.drag(u.at(drawAt(0.6f, -0.9f)), u.at(drawAt(0.7f, -0.9f)), 4);
  CHECK(u.data().keyframes.size() == keys + 1);
  // One undo takes back the stroke, the next the new keyframe with its first stroke.
  REQUIRE(u.ctx.undo());
  REQUIRE(u.ctx.undo());
  CHECK(u.data().keyframes.size() == keys);
  // A marker click selects again: edits go to the selected keyframe (basic shape on keyframe 3).
  u.d.click(u.at({171.5f, posY(1.0f)}));
  u.d.click(u.at({188.0f - 6.5f, 24.0f}));  // noise, the last shape
  CHECK(u.data().keyframes[2].cycle == basic(dsp::BasicCycle::Noise));
  CHECK(u.data().keyframes.size() == keys);
}

// ---- goldens ----------------------------------------------------------------------------------------------------

namespace {

class WtTarget final : public MetaTarget {
public:
  double v[5] = {0.3, 0.4, 0.0, 0.25, 0.0};
  double s[2] = {0.0, 0.0};
  meta::DataPtr d = project_patch::demoWavetableData();
  double value(size_t p) const override { return p < 5 ? v[p] : 0.0; }
  void begin(const char*) override {}
  void set(size_t p, double x) override {
    if (p < 5) v[p] = x;
  }
  void end() override {}
  void cancel() override {}
  void reset(size_t p) override {
    if (p < 5) v[p] = wtType().params[p].def;
  }
  double setting(size_t i) const override { return i < 2 ? s[i] : 0.0; }
  void setSetting(size_t i, double x) override {
    if (i < 2) s[i] = x;
  }
  meta::DataPtr data() const override { return d; }
  void setData(meta::DataPtr x) override { d = std::move(x); }
  const meta::Type* targetType() const noexcept override { return &wtType(); }
};

// A rich drawn-looking keyframe for the goldens: a saw with a formant-like bump and a little texture.
meta::DataPtr goldenData() {
  meta::WavetableData d;
  std::vector<float> c = basic(dsp::BasicCycle::Saw);
  for (uint32_t i = 0; i < wt::kCycleLen; ++i) {
    const float x = static_cast<float>(i) / wt::kCycleLen;
    c[i] = 0.62f * c[i] + 0.3f * std::sin(2.0f * static_cast<float>(kPi) * 5.0f * x) * std::exp(-8.0f * (x - 0.3f) * (x - 0.3f)) +
           0.05f * std::sin(2.0f * static_cast<float>(kPi) * 61.0f * x);
  }
  d.keyframes.push_back({0.0f, c});
  d.keyframes.push_back({1.0f, basic(dsp::BasicCycle::Square)});
  return meta::wavetableData(d);
}

std::shared_ptr<const circuit::CircuitScene> wtScene() {
  const std::string text = "patch WtDemo {\n  in    ph  : audio\n  out   y   : audio\n  wt1 = meta_wavetable(ph)\n  y = wt1\n}\n";
  auto doc = circuit::loadPatchDocument((fs::temp_directory_path() / "pq_meta_wt_edit_test.pqp").string(), text);
  REQUIRE_MESSAGE(doc.ok(), (doc.ok() ? "" : doc.error().message));
  const graph::Patch* p = (*doc)->find("WtDemo");
  REQUIRE(p != nullptr);
  return circuit::buildScene(*p);
}

struct Rig {
  std::unique_ptr<GpuContext> gpu;
  std::unique_ptr<Renderer> renderer;
  SceneTextures tex;
  PaintResources res;
  UiTree tree;
  circuit::CircuitCanvas* canvas = nullptr;
  std::unique_ptr<OffscreenTarget> target;
  DrawList dl;
  uint32_t w = 0, h = 0;
  double now = 0.0;
  float scale = 1.0f;
  WtTarget* bound = nullptr;

  Rig(float sc) : scale(sc) {
    GpuOptions o;
    o.validation = true;
    auto g = GpuContext::create(o);
    REQUIRE_MESSAGE(g.ok(), (g.ok() ? "" : g.error().message));
    gpu = std::move(*g);
    auto r = Renderer::create(*gpu, OffscreenTarget::kFormat, 1);
    REQUIRE(r.ok());
    renderer = std::move(*r);
    auto t = createSceneTextures(*renderer, UiScale(sc));
    REQUIRE(t.ok());
    tex = std::move(*t);
    res = paintResources(tex);
    const UiScale s(sc);
    w = static_cast<uint32_t>(s.px(456.0f));
    h = static_cast<uint32_t>(s.px(544.0f));
    tree.setViewport(w, h, s);
    tree.setResources(&res);
    canvas = &tree.emplaceRoot<circuit::CircuitCanvas>("circuit");
    canvas->faceBinder = [this](const circuit::SceneFace&) {
      circuit::CircuitCanvas::FaceBinding b;
      auto t = std::make_unique<WtTarget>();
      t->d = goldenData();
      t->v[1] = 0.0;  // position on the first keyframe
      bound = t.get();
      b.target = std::move(t);
      return b;
    };
    canvas->setScene(wtScene(), false);
    auto tg = OffscreenTarget::create(*gpu, w, h, 1);
    REQUIRE(tg.ok());
    target = std::move(*tg);
    tree.layoutNow();
    REQUIRE(canvas->faceCount() == 1);
    const circuit::SceneNode& n = canvas->scene()->nodes[canvas->faceNode(0)];
    circuit::CircuitCanvas::View v;
    v.zoom = 2.0;
    v.x = n.x + 102.0 - 456.0 / 2.0 * 0.5;
    v.y = n.y + 124.0 - 544.0 / 2.0 * 0.5;
    canvas->setView(v);
  }
  Vec2 dev(Vec2 l) const {
    const circuit::SceneNode& n = canvas->scene()->nodes[canvas->faceNode(0)];
    return canvas->toDevice({n.x + l.x, n.y + l.y});
  }
  void send(PointerType type, Vec2 l, PointerButton b = PointerButton::Left, Mods m = mod::kNone, float wheel = 0.0f) {
    PointerEvent e;
    e.type = type;
    e.button = type == PointerType::Down || type == PointerType::Up ? b : PointerButton::None;
    e.clickCount = 1;
    e.mods = m;
    e.pos = dev(l);
    e.wheel = {0.0f, wheel};
    (void)canvas->onPointer(e);
  }
  void settle() {
    for (int i = 0; i < 400 && (tree.needsFrame() || tree.nextWakeTime() <= now + 1.0); ++i) {
      now = std::max(now + 1.0 / 144.0, std::min(tree.nextWakeTime(), now + 1.0));
      tree.update(now);
      tex.text->beginFrame();
      dl.reset(w, h);
      tree.paint(dl, res, now);
      if (res.tiles) res.tiles->beginFrame();
    }
  }
  Image draw() {
    tex.text->beginFrame();
    dl.reset(w, h);
    tree.paint(dl, res, now);
    REQUIRE(target->draw(*renderer, dl, kSceneClearColor).ok());
    auto img = target->read();
    REQUIRE(img.ok());
    return std::move(*img);
  }
};

void checkImage(const std::string& name, const Image& img) {
  auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png");
  if (!ref) {
    (void)writePng(name + ".actual.png", img);
    FAIL_CHECK("no golden " << name << ": wrote " << name << ".actual.png");
    return;
  }
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(name, img, *ref, true, &d));
  MESSAGE(name << ": " << pq::ui::test::describe(d));
}

}  // namespace

TEST_CASE("wavetable face: golden meta-wavetable-harmonics — the harmonic rows zoomed to 256, a bar hovered") {
  for (const float scale : {1.0f, 2.0f}) {
    Rig rig(scale);
    rig.settle();
    // Ctrl+wheel out three octaves about the left edge: harmonics 1 … 256.
    for (int i = 0; i < 6; ++i) rig.send(PointerType::Wheel, {kHarmX0 + 0.2f, ampY(0.5f)}, PointerButton::None, mod::kCtrl, -1.0f);
    rig.send(PointerType::Move, {barX(61, 1, 256), ampY(0.2f)});  // the texture's harmonic, hovered
    rig.settle();
    std::string title, body;
    rig.canvas->faceAt(0).describe(rig.canvas->faceAt(0).hoveredPart(), title, body);
    CHECK_MESSAGE(body.find("showing 1-256 of") != std::string::npos, title << " | " << body);
    checkImage(std::string("meta-wavetable-harmonics-") + (scale == 1.0f ? "1x" : "2x"), rig.draw());
  }
}

TEST_CASE("wavetable face: golden meta-wavetable-draw — the pencil on a 16-column grid, a stroke drawn, the cursor") {
  for (const float scale : {1.0f, 2.0f}) {
    Rig rig(scale);
    rig.settle();
    rig.send(PointerType::Down, toolAt(0));
    rig.send(PointerType::Up, toolAt(0));
    rig.send(PointerType::Down, optAt(0));  // grid 8
    rig.send(PointerType::Up, optAt(0));
    rig.send(PointerType::Down, optAt(0));  // grid 16
    rig.send(PointerType::Up, optAt(0));
    // A stroke: steps up across the second half (snapped to the grid's columns and values).
    rig.send(PointerType::Down, drawAt(0.55f, -0.2f));
    for (int i = 1; i <= 8; ++i) rig.send(PointerType::Move, drawAt(0.55f + 0.05f * static_cast<float>(i), -0.2f + 0.13f * static_cast<float>(i)));
    rig.send(PointerType::Up, drawAt(0.95f, 0.84f));
    rig.send(PointerType::Move, drawAt(0.3f, 0.45f));  // the cursor where the next stroke would start
    rig.settle();
    checkImage(std::string("meta-wavetable-draw-") + (scale == 1.0f ? "1x" : "2x"), rig.draw());
  }
}

TEST_CASE("wavetable face: the harmonic rows paint ≤ 0.2 ms with 512 bars visible (2x)" * doctest::test_suite("perf")) {
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  std::unique_ptr<MetaFace> face = makeFace(wtType());
  REQUIRE(face != nullptr);
  auto t = std::make_unique<WtTarget>();
  t->d = goldenData();
  face->bind(std::move(t), nullptr);
  // Zoom to 512 bars (Ctrl+wheel out: 32 → 512 in 8 notches).
  FacePointer w;
  w.kind = FacePointer::Kind::Wheel;
  w.local = {kHarmX0 + 0.05f, ampY(0.5f)};
  w.mods = mod::kCtrl;
  w.wheel = {0.0f, -1.0f};
  for (int i = 0; i < 8; ++i) REQUIRE(face->onPointer(w));
  std::string title, body;
  face->describe(40, title, body);
  REQUIRE(body.find("showing 1-512 of") != std::string::npos);
  DrawList dl;
  auto paintRows = [&] {
    dl.reset(1200, 1200);
    for (uint32_t layer = 4; layer <= 6; ++layer) {
      ui::FacePainter p(dl, {{20.0f, 20.0f}, 4.0f});  // zoom 2 at 2x
      face->paintStatic(p, layer);
    }
  };
  for (int i = 0; i < 20; ++i) paintRows();
  std::vector<double> ms;
  for (int i = 0; i < 200; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    paintRows();
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(ms.begin(), ms.end());
  MESSAGE("harmonic rows (amplitude, phase, slots), 512 bars at 4 device px per unit: median " << ms[100] << " ms, p90 " << ms[180]);
#if PQ_OPTIMIZED
  CHECK(ms[100] <= 0.2);
#endif
}
