// TMM.14: the VCA face — goldens at zoom 0.1 / 0.5 / 1 / 2 and 1x / 2x with four voices at modulated knob positions,
// the same primitives at every zoom, tiles equal to direct drawing, every gesture through the project patch view as
// one undo step and no rebuild (the knob, min and max drags, the curve box's shape drag, a preset that switches the
// curve, typing into min / max / the readout, double-click resets, Esc), the info view and cursors, the chain and
// nested faces bound to their state, the scenes device-meta-vca and project-meta-vca, and the live layer's paint cost.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"
#include "pq/meta/vca.h"
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

const meta::Type& vcaType() { return meta::vcaType(); }

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// Fixed state (the goldens): the default range and curve, the knob at 0.76 (×5.6, in the log part).
class VcaTarget final : public MetaTarget {
public:
  double v[meta::kVcaParams] = {0.76, -20000.0, 20000.0, 0.4, 0.0};
  double curve = 0.0;
  double value(size_t p) const override { return p < meta::kVcaParams ? v[p] : 0.0; }
  void begin(const char*) override {}
  void set(size_t p, double x) override {
    if (p < meta::kVcaParams) v[p] = x;
  }
  void end() override {}
  void cancel() override {}
  void reset(size_t p) override {
    if (p < meta::kVcaParams) v[p] = vcaType().params[p].def;
  }
  double setting(size_t) const override { return curve; }
  void setSetting(size_t, double x) override { curve = x; }
  const meta::Type* targetType() const noexcept override { return &vcaType(); }
};

// Four voices, each knob position moved a little by a modulation during the last displayed frame.
class KnobVoices final : public VoiceTapReader {
public:
  struct V {
    uint32_t slot;
    double from, to;
  };
  std::vector<V> v = {{0, 0.70, 0.72}, {1, 0.75, 0.77}, {2, 0.80, 0.79}, {3, 0.86, 0.84}};
  uint64_t serialNow = 1;
  uint32_t read(std::span<FaceVoice> out) override {
    uint32_t n = 0;
    for (const V& x : v) {
      if (n >= out.size()) break;
      FaceVoice& f = out[n++];
      f.slot = x.slot;
      f.sampleCount = 12;
      for (uint32_t k = 0; k < 12; ++k) f.samples[k] = static_cast<float>(x.from + (x.to - x.from) * k / 11.0);
      f.phase = f.samples[11];
    }
    return n;
  }
  uint64_t serial() const noexcept override { return serialNow; }
  bool live() const noexcept override { return false; }
};

std::shared_ptr<const circuit::CircuitScene> vcaScene() {
  const std::string text = "patch VcaDemo {\n  in    x   : audio\n  out   y   : audio\n  vca1 = meta_vca(x)\n  y = vca1\n}\n";
  auto doc = circuit::loadPatchDocument((fs::temp_directory_path() / "pq_meta_vca_face_test.pqp").string(), text);
  REQUIRE_MESSAGE(doc.ok(), (doc.ok() ? "" : doc.error().message));
  const graph::Patch* p = (*doc)->find("VcaDemo");
  REQUIRE(p != nullptr);
  return circuit::buildScene(*p);
}

struct VcaRig {
  std::unique_ptr<GpuContext> gpu;
  std::unique_ptr<Renderer> renderer;
  SceneTextures tex;
  PaintResources res, resNoTiles;
  UiTree tree;
  circuit::CircuitCanvas* canvas = nullptr;
  std::unique_ptr<OffscreenTarget> target;
  DrawList dl;
  uint32_t w = 0, h = 0;
  double now = 0.0;
  KnobVoices voices;
  float scale = 1.0f;

  VcaRig(uint32_t lw, uint32_t lh, float sc, bool withVoices) : scale(sc) {
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
    resNoTiles = res;
    resNoTiles.tiles = nullptr;
    const UiScale s(sc);
    w = static_cast<uint32_t>(s.px(static_cast<float>(lw)));
    h = static_cast<uint32_t>(s.px(static_cast<float>(lh)));
    tree.setViewport(w, h, s);
    tree.setResources(&res);
    canvas = &tree.emplaceRoot<circuit::CircuitCanvas>("circuit");
    canvas->faceBinder = [this, withVoices](const circuit::SceneFace&) {
      circuit::CircuitCanvas::FaceBinding b;
      b.target = std::make_unique<VcaTarget>();
      b.voices = withVoices ? &voices : nullptr;
      return b;
    };
    canvas->setScene(vcaScene(), false);
    auto tg = OffscreenTarget::create(*gpu, w, h, 1);
    REQUIRE(tg.ok());
    target = std::move(*tg);
    tree.layoutNow();
  }
  void centre(double zoom) {
    REQUIRE(canvas->faceCount() == 1);
    const circuit::SceneNode& n = canvas->scene()->nodes[canvas->faceNode(0)];
    circuit::CircuitCanvas::View v;
    v.zoom = zoom;
    const double lw = canvas->rect().w / static_cast<double>(scale), lh = canvas->rect().h / static_cast<double>(scale);
    v.x = n.x + 60.0 - lw / zoom * 0.5;
    v.y = n.y + 90.0 - lh / zoom * 0.5;
    canvas->setView(v);
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
  Image draw(const PaintResources& r) {
    tex.text->beginFrame();
    dl.reset(w, h);
    tree.paint(dl, r, now);
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

void checkScene(const std::string& name) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(name, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(name, {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  checkImage(name, *img);
}

std::string zoomName(double z) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", z);
  return buf;
}

uint32_t rigW(double zoom) { return static_cast<uint32_t>(std::max(96.0, std::ceil(120.0 * zoom + 48.0))); }
uint32_t rigH(double zoom) { return static_cast<uint32_t>(std::max(96.0, std::ceil(180.0 * zoom + 48.0))); }

// The face's geometry (vca_face.cpp's layout), for gestures.
constexpr float kCx = 60.0f, kCy = 58.0f;
constexpr float kMinX = 24.0f, kMaxX = 96.0f, kRangeY = 117.0f, kReadoutY = 103.0f;
constexpr float kBoxX = 60.0f, kBoxY = 143.0f;
float chipX(int i) { return 15.0f + 24.0f * static_cast<float>(i) + 9.0f; }
constexpr float kChipY = 168.0f;

}  // namespace

TEST_CASE("vca face: registered; a nested call's symbol is its face, sized 120 x 180") {
  REQUIRE(hasFace(vcaType()));
  CHECK(faceSize(vcaType()).x == 120.0f);
  CHECK(faceSize(vcaType()).y == 180.0f);
  TestDriver d(600, 400);
  auto& canvas = d.setRoot<circuit::CircuitCanvas>("circuit");
  canvas.setScene(vcaScene(), false);
  d.tree().layoutNow();
  REQUIRE(canvas.faceCount() == 1);
  CHECK(&canvas.faceAt(0).type() == &vcaType());
}

TEST_CASE("vca face: golden at zoom 0.1 / 0.5 / 1 / 2, 1x and 2x, four voices at modulated knob positions") {
  for (const float scale : {1.0f, 2.0f})
    for (const double zoom : {0.1, 0.5, 1.0, 2.0}) {
      VcaRig rig(rigW(zoom), rigH(zoom), scale, true);
      rig.centre(zoom);
      rig.settle();
      REQUIRE(rig.canvas->faceAt(0).sounding().size() == 4u);
      const std::string name = "meta-vca-z" + zoomName(zoom) + "-" + zoomName(scale) + "x";
      CAPTURE(name);
      CHECK(rig.canvas->stats().facesPainted == 1u);
      CHECK(rig.canvas->stats().faceLayersFromTiles == 3u);
      checkImage(name, rig.draw(rig.res));
    }
}

TEST_CASE("vca face: the same primitives at every zoom; tiles equal direct drawing") {
  VcaRig rig(500, 420, 1.0f, true);
  uint32_t prims[4] = {};
  const double zooms[4] = {0.1, 0.5, 1.0, 2.0};
  for (int k = 0; k < 4; ++k) {
    rig.centre(zooms[k]);
    rig.settle();
    (void)rig.draw(rig.res);
    CHECK(rig.canvas->stats().facesPainted == 1u);
    prims[k] = rig.canvas->stats().facePrimitives;
  }
  CHECK(prims[0] == prims[1]);
  CHECK(prims[1] == prims[2]);
  CHECK(prims[2] == prims[3]);
  CHECK(prims[0] > 60u);
  for (const float scale : {1.0f, 2.0f})
    for (const double zoom : {0.5, 1.0, 2.0}) {
      VcaRig r(rigW(zoom), rigH(zoom), scale, false);
      r.centre(zoom);
      r.settle();
      const Image tiled = r.draw(r.res);
      CHECK(r.canvas->stats().faceLayersFromTiles == 3u);
      const Image direct = r.draw(r.resNoTiles);
      CHECK(r.canvas->stats().faceLayersDirect == 3u);
      const std::string name = "meta-vca-tiles-z" + zoomName(zoom) + "-" + zoomName(scale) + "x";
      CAPTURE(name);
      CHECK(pq::ui::test::matchesGolden(name, tiled, direct));
    }
}

// ---- gestures through the project patch view -------------------------------------------------------------------

namespace {

struct VcaUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  project_patch::MetaVcaIds ids;

  VcaUi() {
    ctx.setDeviceLibrary(&library());
    REQUIRE(project_patch::buildMetaVcaProject(ctx.store(), &ids).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setShown(true, false);
    d.tree().layoutNow();
  }
  ~VcaUi() { d.tree().setRoot(std::make_unique<Widget>()); }

  project_patch::ProjectPatchView& view() const {
    project_patch::ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Device& dev() const {
    const doc::Device* dv = doc::findDevice(*ctx.doc(), ids.vca).device;
    REQUIRE(dv != nullptr);
    return *dv;
  }
  double value(size_t p) const { return doc::metaParamValue(dev(), "", vcaType(), p); }
  double curve() const {
    const std::vector<double> s = doc::metaSettings(dev(), "", vcaType());
    return s.empty() ? 0.0 : s[0];
  }
  const project_patch::Node& node() const { return view().patch().nodes[view().patch().nodeOf(ids.vca)]; }
  void frame(double zoom) {
    const project_patch::Node& n = node();
    project_patch::ProjectCanvas& c = view().canvas();
    const double us = d.tree().scale();
    circuit::CircuitCanvas::View v;
    v.zoom = zoom;
    v.x = n.x + 60.0 - c.rect().w / (zoom * us) * 0.5;
    v.y = n.y + 90.0 - c.rect().h / (zoom * us) * 0.5;
    c.setView(v);
    for (int i = 0; i < 4; ++i) d.step();
  }
  Vec2 at(float x, float y) const {
    const project_patch::Node& n = node();
    return view().canvas().toDevice({n.x + x, n.y + y});
  }
  void type(const std::string& s) {
    for (const char c : s) (void)d.keyDown(static_cast<Key>(c));
  }
};

}  // namespace

TEST_CASE("vca face: a chain VCA is its face in the project patch view; the nested vca1 too, bound to its state") {
  VcaUi u;
  CHECK(u.node().face == &vcaType());
  CHECK(u.node().w == 120.0f);
  REQUIRE(u.d.enableGpu().ok());
  project_patch::ProjectCanvas& c = u.view().canvas();
  const uint32_t sn = u.view().scene().sceneNode[u.view().patch().nodeOf(u.ids.vca)];
  MetaFace* f = c.faceOn(sn);
  REQUIRE(f != nullptr);
  CHECK(f->target().editable());
  CHECK(f->target().value(meta::kVcaValue) == doctest::Approx(0.62));
  u.view().setDeviceExpanded(u.ids.tremInst, true);
  for (int i = 0; i < 4; ++i) u.d.step();
  REQUIRE(u.view().deviceExpanded(u.ids.tremInst));
  size_t vcas = 0, nested = 0;
  for (size_t i = 0; i < c.faceCount(); ++i) {
    if (&c.faceAt(i).type() != &vcaType()) continue;
    ++vcas;
    const MetaTarget& t = c.faceAt(i).target();
    if (t.value(meta::kVcaMax) == doctest::Approx(2.0) && t.setting(meta::kVcaCurveSetting) == 1.0 && t.editable()) ++nested;
  }
  CHECK(vcas == 2u);
  CHECK(nested == 1u);
}

TEST_CASE("vca face: knob, min, max and curve drags at zoom 0.1 and 1.5 — one undo step each, no rebuild") {
  for (const double zoom : {0.1, 1.5}) {
    CAPTURE(zoom);
    VcaUi u;
    u.frame(zoom);
    const double us = u.d.tree().scale();
    const auto span = [&](float extent) { return std::max(extent * zoom * us, 150.0 * us); };
    const uint64_t rebuilds = u.view().stats().rebuilds;
    // The knob: up 30 px.
    size_t h0 = u.history();
    const double p0 = u.value(meta::kVcaValue);
    Vec2 a = u.at(kCx, kCy + 12.0f);
    u.d.drag(a, {a.x, a.y - 30.0f}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kVcaValue) == doctest::Approx(p0 + 30.0 / span(66.0f)).epsilon(1e-4));
    // Shift: a tenth.
    h0 = u.history();
    const double p1 = u.value(meta::kVcaValue);
    u.d.drag(a, {a.x, a.y + 20.0f}, 6, mod::kShift);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kVcaValue) == doctest::Approx(p1 - 2.0 / span(66.0f)).epsilon(1e-4));
    // Max: down 40 px along the default scale, rounded to 3 significant digits.
    h0 = u.history();
    a = u.at(kMaxX, kRangeY);
    u.d.drag(a, {a.x, a.y + 40.0f}, 6);
    CHECK(u.history() == h0 + 1);
    const meta::VcaLaw scale = meta::vcaLaw(meta::VcaCurve::Log, -20000.0, 20000.0, 0.4, 0.0);
    const double wantMax = meta::vcaValue(scale, 1.0 - 40.0 / span(40.0f));
    CHECK(u.value(meta::kVcaMax) == doctest::Approx(wantMax).epsilon(5e-3));
    CHECK(u.value(meta::kVcaMax) < 20000.0);
    // Min: up 300 px stops at max (min ≤ max).
    h0 = u.history();
    a = u.at(kMinX, kRangeY);
    u.d.drag(a, {a.x, a.y - 300.0f}, 12);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kVcaMin) == doctest::Approx(u.value(meta::kVcaMax)));
    // The curve box: up 30 px widens the fine zone.
    h0 = u.history();
    const double z0 = u.value(meta::kVcaZone);
    a = u.at(kBoxX, kBoxY);
    u.d.drag(a, {a.x, a.y - 30.0f}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kVcaZone) == doctest::Approx(z0 + (0.9 - 0.05) * 30.0 / span(60.0f)).epsilon(1e-4));
    CHECK(u.view().stats().rebuilds == rebuilds);  // param writes: nothing re-lowers
    // Double-click the knob: ×1 under the current law, one step; undo takes it back.
    REQUIRE(u.ctx.undo());  // the zone
    REQUIRE(u.ctx.undo());  // min
    REQUIRE(u.ctx.undo());  // max
    h0 = u.history();
    u.d.doubleClick(u.at(kCx, kCy + 12.0f));
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kVcaValue) == doctest::Approx(meta::kVcaDefaultPos).epsilon(1e-9));
    REQUIRE(u.ctx.undo());
    CHECK(u.value(meta::kVcaValue) != doctest::Approx(meta::kVcaDefaultPos));
  }
}

TEST_CASE("vca face: presets switch the curve in one undo step; the Bend curve's drag bends it") {
  VcaUi u;
  u.frame(1.5);
  CHECK(u.curve() == 0.0);
  // Exponential: the curve becomes Bend (a setting: its unit recompiles) and bend 0.6, one step.
  size_t h0 = u.history();
  u.d.click(u.at(chipX(2), kChipY));
  CHECK(u.history() == h0 + 1);
  CHECK(u.curve() == 1.0);
  CHECK(u.value(meta::kVcaBend) == doctest::Approx(0.6));
  // Up 20 px in the box: toward logarithmic (bend down).
  h0 = u.history();
  const double us = u.d.tree().scale();
  const Vec2 a = u.at(kBoxX, kBoxY);
  u.d.drag(a, {a.x, a.y - 20.0f}, 6);
  CHECK(u.history() == h0 + 1);
  CHECK(u.value(meta::kVcaBend) == doctest::Approx(0.6 - 2.0 * 20.0 / std::max(60.0 * 1.5 * us, 150.0 * us)).epsilon(1e-4));
  // Linear: bend 0, still Bend; bipolar log: back to Log.
  u.d.click(u.at(chipX(0), kChipY));
  CHECK(u.value(meta::kVcaBend) == doctest::Approx(0.0));
  CHECK(u.curve() == 1.0);
  h0 = u.history();
  u.d.click(u.at(chipX(1), kChipY));
  CHECK(u.history() == h0 + 1);
  CHECK(u.curve() == 0.0);
  REQUIRE(u.ctx.undo());
  CHECK(u.curve() == 1.0);
}

TEST_CASE("vca face: double-click a range end or the readout and type; Enter commits, Esc cancels, empty restores") {
  VcaUi u;
  u.frame(1.5);
  // Max: "150" → 150, one step.
  size_t h0 = u.history();
  u.d.doubleClick(u.at(kMaxX, kRangeY));
  u.type("150");
  CHECK(u.value(meta::kVcaMax) == doctest::Approx(20000.0));  // nothing until Enter
  (void)u.d.keyDown(key::kReturn);
  CHECK(u.history() == h0 + 1);
  CHECK(u.value(meta::kVcaMax) == doctest::Approx(150.0));
  // Min: "-2.5k" → −2500.
  u.d.doubleClick(u.at(kMinX, kRangeY));
  u.type("-2.5k");
  (void)u.d.keyDown(key::kReturn);
  CHECK(u.value(meta::kVcaMin) == doctest::Approx(-2500.0));
  // Esc cancels.
  h0 = u.history();
  u.d.doubleClick(u.at(kMinX, kRangeY));
  u.type("7");
  (void)u.d.keyDown(key::kEscape);
  CHECK(u.history() == h0);
  CHECK(u.value(meta::kVcaMin) == doctest::Approx(-2500.0));
  // Backspace edits; typing into the readout sets the knob to the typed value's position.
  u.d.doubleClick(u.at(kCx, kReadoutY));
  u.type("33");
  (void)u.d.keyDown(key::kBackspace);
  (void)u.d.keyDown(key::kReturn);
  const meta::VcaLaw law = meta::vcaLaw(meta::VcaCurve::Log, -2500.0, 150.0, 0.4, 0.0);
  CHECK(meta::vcaValue(law, u.value(meta::kVcaValue)) == doctest::Approx(3.0).epsilon(1e-6));
  // An empty entry restores the default.
  u.d.doubleClick(u.at(kMaxX, kRangeY));
  (void)u.d.keyDown(key::kReturn);
  CHECK(u.value(meta::kVcaMax) == doctest::Approx(20000.0));
  // A press elsewhere commits.
  u.d.doubleClick(u.at(kMinX, kRangeY));
  u.type("-40");
  u.d.click(u.at(kBoxX, kBoxY));
  CHECK(u.value(meta::kVcaMin) == doctest::Approx(-40.0));
}

TEST_CASE("vca face: Esc during a drag undoes it") {
  VcaUi u;
  u.frame(1.0);
  const double before = u.value(meta::kVcaValue);
  const Vec2 a = u.at(kCx, kCy + 12.0f);
  u.d.down(a);
  u.d.move({a.x, a.y - 20.0f});
  u.d.move({a.x, a.y - 40.0f});
  CHECK(u.value(meta::kVcaValue) > before);
  const size_t h0 = u.history();
  (void)u.d.keyDown(key::kEscape);
  (void)u.d.up({a.x, a.y - 40.0f});
  CHECK(u.value(meta::kVcaValue) == doctest::Approx(before));
  CHECK(u.history() == h0);
}

TEST_CASE("vca face: the info view names the hovered control; cursors follow it") {
  VcaUi u;
  u.frame(1.5);
  project_patch::ProjectCanvas& c = u.view().canvas();
  u.d.move(u.at(kCx, kCy + 12.0f));
  CHECK(std::string(c.info().title) == "Value");
  CHECK(c.cursorAt(u.at(kCx, kCy + 12.0f)) == Cursor::ResizeNS);
  u.d.move(u.at(kMinX, kRangeY));
  CHECK(std::string(c.info().title) == "Min");
  u.d.move(u.at(kMaxX, kRangeY));
  CHECK(std::string(c.info().title) == "Max");
  u.d.move(u.at(kBoxX, kBoxY));
  CHECK(std::string(c.info().title) == "Curve: bipolar log");
  u.d.move(u.at(chipX(2), kChipY));
  CHECK(std::string(c.info().title) == "Preset: Exponential");
  CHECK(c.cursorAt(u.at(chipX(2), kChipY)) == Cursor::Pointer);
}

TEST_CASE("render: device-meta-vca matches the golden image") { checkScene("device-meta-vca"); }

TEST_CASE("render: project-meta-vca matches the golden image") { checkScene("project-meta-vca"); }

TEST_CASE("vca face: the live layer's paint cost (2x, 4 voices) and 16 faces" * doctest::test_suite("perf")) {
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  std::unique_ptr<MetaFace> face = makeFace(vcaType());
  REQUIRE(face != nullptr);
  KnobVoices voices;
  auto target = std::make_unique<VcaTarget>();
  VcaTarget* tg = target.get();
  face->bind(std::move(target), &voices);
  FaceJack jacks[2];
  jacks[0] = {{0.0f, 58.0f}, true, true, true, "in"};
  jacks[1] = {{120.0f, 58.0f}, false, true, true, "out"};
  FaceEnv env;
  env.theme = &*theme;
  env.uiScale = 2.0f;
  env.jacks = jacks;
  DrawList dl;
  auto paintOnce = [&](uint32_t faces) {
    dl.reset(2560, 1440);
    for (uint32_t k = 0; k < faces; ++k) {
      ui::FacePainter p(dl, {{static_cast<float>(20 + 300 * (k % 8)), static_cast<float>(20 + 400 * (k / 8))}, 2.0f});
      face->paintLive(p, env);
    }
  };
  std::vector<double> one, sixteen, edit;
  for (int i = 0; i < 220; ++i) {
    ++voices.serialNow;
    (void)face->pollVoices();
    auto t0 = std::chrono::steady_clock::now();
    paintOnce(1);
    one.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    t0 = std::chrono::steady_clock::now();
    paintOnce(16);
    sixteen.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    // A drag step on max: the law and the curve are derived again.
    tg->v[meta::kVcaMax] = 100.0 + 50.0 * i;
    t0 = std::chrono::steady_clock::now();
    paintOnce(1);
    edit.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(one.begin(), one.end());
  std::sort(sixteen.begin(), sixteen.end());
  std::sort(edit.begin(), edit.end());
  MESSAGE("vca live layer, 1 face (2x, 4 voices): median " << one[110] << " ms; 16 faces: median " << sixteen[110]
                                                           << " ms; a drag step (law + curve re-derived): median " << edit[110] << " ms");
  CHECK(face->sounding().size() == 4u);
#if PQ_OPTIMIZED
  CHECK(one[110] < 0.1);
  CHECK(sixteen[110] < 1.0);
  CHECK(edit[110] < 0.2);
#endif
}
