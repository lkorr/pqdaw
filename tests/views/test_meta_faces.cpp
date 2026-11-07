// TMM.4: meta-module faces — the vector face toolkit (ui::FacePainter); the project patch view's meta-faces scene.
// GU.4: the Phase accumulator's face (whose drags, hit tests, pitch typing, voice colours and orbits this file tested) is
// deleted with its type: the Phase Accumulator is a group preset whose face is its panel of generic elements
// (tests/views/test_group_panels.cpp, the module panels' goldens there); the face toolkit's tiles, polls and gestures
// are exercised by the remaining faces' tests (Wavetable, Envelope, Scope, Spectrum, Spectrogram). Golden:
// project-meta-faces (its tracks headed by Phase Accumulator modules, drawn as their panels).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/theme.h"
#include "pq/ui/tile_atlas.h"
#include "pq/views/circuit/circuit_canvas.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/circuit/circuit_viewer.h"
#include "pq/views/circuit/device_circuit.h"
#include "pq/views/fake_transport.h"
#include "pq/views/meta/face_targets.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
namespace fs = std::filesystem;

namespace {


const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

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

TEST_CASE("faces: the painter maps world to device and emits the same primitives at every scale") {
  DrawList dl;
  uint32_t counts[3] = {};
  const float scales[3] = {0.05f, 1.0f, 3.0f};
  for (int k = 0; k < 3; ++k) {
    dl.reset(800, 800);
    FacePainter p(dl, {{10.0f, 20.0f}, scales[k]});
    CHECK(p.transform().map({4.0f, 8.0f}).x == doctest::Approx(10.0f + 4.0f * scales[k]));
    CHECK(p.transform().map({4.0f, 8.0f}).y == doctest::Approx(20.0f + 8.0f * scales[k]));
    const Vec2 pts[3] = {{0, 0}, {10, 5}, {20, 0}};
    p.disc({5, 5}, 3, rgba(255, 255, 255));
    p.ring({5, 5}, 3, 0.5f, rgba(255, 255, 255));
    p.arc({5, 5}, 3, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    p.line({0, 0}, {10, 10}, 1.0f, rgba(255, 255, 255));
    p.polyline(pts, 1.0f, rgba(255, 255, 255));
    p.dashed(pts, 1.0f, 2.0f, 1.0f, rgba(255, 255, 255));
    p.fillUnder(pts, 10.0f, rgba(255, 255, 255), rgba(255, 255, 255, 0));
    p.convex(pts, rgba(255, 255, 255));
    p.roundedRect({0, 0, 10, 10}, 2.0f, rgba(255, 255, 255));
    p.beginGlow();
    p.glow({5, 5}, 4.0f, rgba(255, 255, 255));
    p.endGlow();
    p.text({5, 5}, "abc", 12.0f, rgba(255, 255, 255));  // no text system: the bar
    counts[k] = p.primitives();
    CHECK_FALSE(dl.empty());  // drawn at every scale: nothing is culled
  }
  CHECK(counts[0] == counts[1]);
  CHECK(counts[1] == counts[2]);
  CHECK(counts[0] == 11);
  // Text: hinted sizes from 4 device px, quantised above 16; below 4 px a bar.
  DrawList d2;
  FacePainter far(d2, {{0, 0}, 0.1f}), near(d2, {{0, 0}, 2.0f});
  CHECK(far.textPixels(12.0f) == 0u);
  CHECK(near.textPixels(12.0f) == 24u);
  CHECK(near.textPixels(2.0f) == 4u);
}

TEST_CASE("faces: the Phase accumulator and the VCA have no face (GU.4: modules — group presets with panels)") {
  CHECK(meta::findType("phase_acc") == nullptr);
  CHECK(meta::findType("vca") == nullptr);
  for (const meta::Type* t : meta::types()) CHECK(t->id != "phase_acc");
}

TEST_CASE("render: project-meta-faces matches the golden image") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize("project-meta-faces", w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene("project-meta-faces", {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  checkImage("project-meta-faces", *img);
}
