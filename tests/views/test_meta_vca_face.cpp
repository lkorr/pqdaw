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
#include <numbers>
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
    v.y = n.y + 60.0 - lh / zoom * 0.5;  // TX.orbit-ports: the round face is 120 x 120
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
uint32_t rigH(double zoom) { return static_cast<uint32_t>(std::max(96.0, std::ceil(120.0 * zoom + 48.0))); }

// The face's geometry (vca_face.cpp's layout), for gestures. TX.orbit-ports: a circle (120 x 120) — the readout at the
// top (inside the dial since TX.vca-knob: the knob fills most of the circle), the knob, the chevron under it; the range,
// the curve box and the presets in the floating panel the chevron opens under the face (its top at 126).
constexpr float kCx = 60.0f, kCy = 61.0f;
constexpr float kReadoutY = 37.0f, kChevX = 60.0f, kChevY = 85.0f;  // the chevron inside the dial (TX.vca-knob)
constexpr float kPopTop = 126.0f;
constexpr float kMinX = 24.0f, kMaxX = 96.0f, kRangeY = kPopTop + 17.0f;
constexpr float kBoxX = 60.0f, kBoxY = kPopTop + 43.0f;
float chipX(int i) { return 7.0f + 18.0f * static_cast<float>(i) + 8.0f; }  // TX.vca-knob2: six chips
constexpr float kChipY = kPopTop + 68.0f;
constexpr float kKnobExtent = 93.0f;  // 2 × the track's radius: a drag's travel per px

}  // namespace

TEST_CASE("vca face: registered; a nested call's symbol is its face, sized 120 x 120, round") {
  REQUIRE(hasFace(vcaType()));
  CHECK(faceSize(vcaType()).x == 120.0f);
  CHECK(faceSize(vcaType()).y == 120.0f);
  CHECK(faceShape(vcaType()).round);  // TX.orbit-ports: the ports go round its circle
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
    v.y = n.y + 90.0 - c.rect().h / (zoom * us) * 0.5;  // the face and its open panel
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
  // TX.orbit-ports: the chevron opens the floating panel (the range, the curve box, the presets); no edit, no rebuild.
  MetaFace& face() const {
    MetaFace* f = view().canvas().faceOn(view().scene().sceneNode[view().patch().nodeOf(ids.vca)]);
    REQUIRE(f != nullptr);
    return *f;
  }
  void openPanel() {
    const size_t h0 = history();
    if (!face().popupRect()) d.click(at(kChevX, kChevY));
    REQUIRE(face().popupRect().has_value());
    CHECK(history() == h0);
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
    CHECK(u.value(meta::kVcaValue) == doctest::Approx(p0 + 30.0 / span(kKnobExtent)).epsilon(1e-4));
    // Shift: a tenth.
    h0 = u.history();
    const double p1 = u.value(meta::kVcaValue);
    u.d.drag(a, {a.x, a.y + 20.0f}, 6, mod::kShift);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kVcaValue) == doctest::Approx(p1 - 2.0 / span(kKnobExtent)).epsilon(1e-4));
    // Max: down 40 px along the default scale, rounded to 3 significant digits (in the floating panel).
    u.openPanel();
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
  u.openPanel();
  CHECK(u.curve() == 0.0);
  // Fader: the curve becomes Bend (a setting: its unit recompiles), 0 … 1, bend 0.5, one step.
  size_t h0 = u.history();
  u.d.click(u.at(chipX(4), kChipY));
  CHECK(u.history() == h0 + 1);
  CHECK(u.curve() == 1.0);
  CHECK(u.value(meta::kVcaBend) == doctest::Approx(0.5));
  // Up 20 px in the box: toward logarithmic (bend down).
  h0 = u.history();
  const double us = u.d.tree().scale();
  const Vec2 a = u.at(kBoxX, kBoxY);
  u.d.drag(a, {a.x, a.y - 20.0f}, 6);
  CHECK(u.history() == h0 + 1);
  CHECK(u.value(meta::kVcaBend) == doctest::Approx(0.5 - 2.0 * 20.0 / std::max(60.0 * 1.5 * us, 150.0 * us)).epsilon(1e-4));
  // 0 -> 1: bend 0, still Bend; Default: back to Log.
  u.d.click(u.at(chipX(1), kChipY));
  CHECK(u.value(meta::kVcaBend) == doctest::Approx(0.0));
  CHECK(u.curve() == 1.0);
  h0 = u.history();
  u.d.click(u.at(chipX(0), kChipY));
  CHECK(u.history() == h0 + 1);
  CHECK(u.curve() == 0.0);
  REQUIRE(u.ctx.undo());
  CHECK(u.curve() == 1.0);
}

// TX.vca-knob2: the six presets — each sets the curve, the range, its shape and the markers in one undo step (the
// values each one names), lights while the state matches it, and draws its own curve.
TEST_CASE("vca face (TX.vca-knob2): each preset's values in one undo step; the current one lit; tooltips") {
  VcaUi u;
  u.frame(1.5);
  u.openPanel();
  project_patch::ProjectCanvas& c = u.view().canvas();
  struct Want {
    const char* title;
    double curve, min, max, zone, bend;
  };
  const Want want[] = {{"Preset: Default", 0.0, -20000.0, 20000.0, 0.4, -9.0},
                       {"Preset: 0 -> 1", 1.0, 0.0, 1.0, -9.0, 0.0},
                       {"Preset: -1 -> +1", 1.0, -1.0, 1.0, -9.0, 0.0},
                       {"Preset: x0 ... x2", 1.0, 0.0, 2.0, -9.0, 0.0},
                       {"Preset: Fader", 1.0, 0.0, 1.0, -9.0, 0.5},
                       {"Preset: 20 Hz -> 20 kHz", 0.0, 20.0, 20000.0, 0.4, -9.0}};
  // A moved marker and zone first: a Log preset puts them back.
  u.d.drag(u.at(kBoxX - 20.0f, kPopTop + 33.0f), u.at(kBoxX - 20.0f, kPopTop + 13.0f), 6);
  REQUIRE(u.value(meta::kVcaZone) != doctest::Approx(0.4));
  for (int round = 0; round < 2; ++round)
    for (int i = 5; i >= 0; --i) {  // from the last: each click changes something
      CAPTURE(i);
      const Want& w = want[i];
      u.d.move(u.at(chipX(i), kChipY));
      CHECK(std::string(c.info().title) == w.title);
      CHECK(c.cursorAt(u.at(chipX(i), kChipY)) == Cursor::Pointer);
      const size_t h0 = u.history();
      u.d.click(u.at(chipX(i), kChipY));
      CHECK(u.history() == h0 + 1);
      CHECK(u.curve() == w.curve);
      // GU.1: params are stored as knob positions: a value reads back within float rounding (of the range's ends).
      CHECK(std::fabs(u.value(meta::kVcaMin) - w.min) <= 1e-12 * meta::kVcaRange);
      CHECK(std::fabs(u.value(meta::kVcaMax) - w.max) <= 1e-12 * meta::kVcaRange);
      if (w.zone >= 0.0) CHECK(u.value(meta::kVcaZone) == doctest::Approx(w.zone));
      if (w.bend > -2.0) CHECK(u.value(meta::kVcaBend) == doctest::Approx(w.bend));
      for (uint32_t m = 0; m < meta::kVcaMarks; ++m) CHECK(u.value(meta::kVcaMark0 + m) == 0.0);
      // The law each one names.
      std::vector<double> v(meta::kVcaParams);
      for (size_t k = 0; k < v.size(); ++k) v[k] = u.value(k);
      const meta::VcaLaw l = meta::vcaLaw(v, u.curve());
      CHECK(meta::vcaValue(l, 0.0) == doctest::Approx(w.min));
      CHECK(meta::vcaValue(l, 1.0) == doctest::Approx(w.max));
      if (i == 2) CHECK(meta::vcaValue(l, 0.5) == doctest::Approx(0.0).scale(1.0));  // -1 -> +1: 0 at 12 o'clock
      if (i == 3) CHECK(meta::vcaValue(l, 0.5) == doctest::Approx(1.0));             // x0 ... x2: unity at 12 o'clock
      if (i == 5) CHECK(meta::vcaValue(l, 0.1) == doctest::Approx(20.0 * std::pow(1000.0, 0.1)));  // equal ratios per travel
      // Undo takes the whole preset back; redo it.
      REQUIRE(u.ctx.undo());
      CHECK(u.history() == h0);
      REQUIRE(u.ctx.redo());
    }
  // A click on the preset the state matches (the lit one) writes nothing.
  const size_t h1 = u.history();
  u.d.click(u.at(chipX(5), kChipY));
  CHECK(u.history() == h1 + 1);  // Default was current: 20 Hz -> 20 kHz is a change
  u.d.click(u.at(chipX(5), kChipY));
  CHECK(u.history() == h1 + 1);  // the current one again: nothing to change, no step
}

TEST_CASE("vca face: double-click a range end or the readout and type; Enter commits, Esc cancels, empty restores") {
  VcaUi u;
  u.frame(1.5);
  u.openPanel();
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
  u.d.move(u.at(kChevX, kChevY));
  CHECK(std::string(c.info().title) == "Range and Curve");
  CHECK(c.cursorAt(u.at(kChevX, kChevY)) == Cursor::Pointer);
  u.openPanel();
  u.d.move(u.at(kMinX, kRangeY));
  CHECK(std::string(c.info().title) == "Min");
  u.d.move(u.at(kMaxX, kRangeY));
  CHECK(std::string(c.info().title) == "Max");
  u.d.move(u.at(kBoxX, kBoxY));
  CHECK(std::string(c.info().title) == "Curve: bipolar log");
  u.d.move(u.at(chipX(2), kChipY));
  CHECK(std::string(c.info().title) == "Preset: -1 -> +1");
  CHECK(c.cursorAt(u.at(chipX(2), kChipY)) == Cursor::Pointer);
}

// ---- TX.vca-knob: 360° travel, the markers dragged alone (on the track and in the box), through dispatch ---------------

namespace {

constexpr float kTickR = 46.5f + 2.25f + 1.0f + 2.0f;  // a point on a marker's tick (vca_face.cpp: outside the track)
constexpr float kBoxL = 14.0f + 3.0f, kBoxW = 92.0f - 6.0f;  // the curve box's inner x span (its padding 3)

// The project VCA's law, from its state in the document.
meta::VcaLaw lawOf(const VcaUi& u) {
  std::vector<double> v(meta::kVcaParams);
  for (size_t i = 0; i < v.size(); ++i) v[i] = u.value(i);
  return meta::vcaLaw(v, u.curve());
}
Vec2 trackLocal(double p, float r = kTickR) {
  const double a = (90.0 + 360.0 * p) * std::numbers::pi / 180.0;
  return {kCx + r * static_cast<float>(std::cos(a)), kCy + r * static_cast<float>(std::sin(a))};
}
// A marker's corner point in the open panel's curve box (TX.vca-knob2: the symmetric-log display scale of vca_face.cpp,
// linear inside ±1, a step per decade beyond).
double symLog(double a) { return a <= 1.0 ? a / std::numbers::ln10 : (1.0 + std::log(a)) / std::numbers::ln10; }
Vec2 cornerLocal(const meta::VcaLaw& l, double p, double v) {
  const double m = std::max({std::abs(l.lo), std::abs(l.hi), 1.0}), sc = symLog(m);
  auto shown = [&](double x) { return (x < 0.0 ? -1.0 : 1.0) * symLog(std::abs(x)) / sc; };
  const double t = (shown(v) - shown(l.lo)) / (shown(l.hi) - shown(l.lo));
  const float y1 = kPopTop + 58.0f;
  return {kBoxL + static_cast<float>(p) * kBoxW, y1 - 3.0f - static_cast<float>(t) * (30.0f - 6.0f)};
}
// A drag round the track from knob position p0 to p1 (the pointer follows the circle), one press.
void arcDrag(VcaUi& u, double p0, double p1, int steps = 10, Mods m = mod::kNone) {
  const Vec2 a = u.at(trackLocal(p0).x, trackLocal(p0).y);
  u.d.move(a, m);
  u.d.down(a, PointerButton::Left, m);
  for (int k = 1; k <= steps; ++k) {
    const Vec2 q = trackLocal(p0 + (p1 - p0) * k / steps);
    u.d.move(u.at(q.x, q.y), m);
  }
  const Vec2 e = trackLocal(p1);
  u.d.up(u.at(e.x, e.y), PointerButton::Left, m);
}
double offsetOf(const VcaUi& u, uint32_t i) { return u.value(meta::kVcaMark0 + i); }

}  // namespace

TEST_CASE("vca face (TX.vca-knob): a full 360° travel — 6 o'clock at both ends, 12 o'clock at 0; a drag clamps, never wraps") {
  for (const double zoom : {0.5, 1.5}) {
    CAPTURE(zoom);
    VcaUi u;
    u.frame(zoom);
    project_patch::ProjectCanvas& c = u.view().canvas();
    const meta::VcaLaw l = lawOf(u);
    // The markers' ticks where 360° puts them: +1 (0.7) a little above 3 o'clock, -1 (0.3) above 9 o'clock; +10, +100
    // further round toward 6 o'clock.
    for (uint32_t i = 0; i < meta::kVcaMarks; ++i) {
      CAPTURE(i);
      REQUIRE(l.marks[i].shown);
      const Vec2 t = trackLocal(l.marks[i].at);
      u.d.move(u.at(t.x, t.y));
      CHECK(std::string(c.info().title) == "Marker " + std::string(meta::kVcaMarkValue[i] > 0 ? "+" : "-") +
                                                std::to_string(static_cast<int>(std::abs(meta::kVcaMarkValue[i]))));
      CHECK(c.cursorAt(u.at(t.x, t.y)) == Cursor::Move);
    }
    CHECK(trackLocal(0.5).y < kCy - 40.0f);  // value 0 (p 0.5) at 12 o'clock
    CHECK(meta::vcaValue(l, 0.5) == doctest::Approx(0.0).scale(1.0));
    // Drag up far past the end: the knob stops at p = 1 (max, 6 o'clock); on up, it does not wrap round to min.
    size_t h0 = u.history();
    const Vec2 a = u.at(kCx, kCy + 12.0f);
    u.d.down(a);
    for (int k = 1; k <= 30; ++k) u.d.move({a.x, a.y - 60.0f * k});
    CHECK(u.value(meta::kVcaValue) == 1.0);
    CHECK(meta::vcaValue(l, u.value(meta::kVcaValue)) == doctest::Approx(20000.0));
    // Back down a little: it moves off the end at once (the overshoot was not accumulated).
    u.d.move({a.x, a.y - 60.0f * 30 + 20.0f});
    const double us = u.d.tree().scale(), span = std::max(kKnobExtent * zoom * us, 150.0 * us);
    CHECK(u.value(meta::kVcaValue) == doctest::Approx(1.0 - 20.0 / span).epsilon(1e-4));
    u.d.up({a.x, a.y - 60.0f * 30 + 20.0f});
    CHECK(u.history() == h0 + 1);
    // Down far past the other end: p = 0 (min, 6 o'clock from the other side).
    u.d.drag(a, {a.x, a.y + 4000.0f}, 30);
    CHECK(u.value(meta::kVcaValue) == 0.0);
    CHECK(meta::vcaValue(l, 0.0) == doctest::Approx(-20000.0));
    CHECK(u.history() == h0 + 2);
  }
}

TEST_CASE("vca face (TX.vca-knob): each marker Ctrl+dragged alone round the track — one undo step, the others stay") {
  VcaUi u;
  u.frame(1.5);
  const meta::VcaLaw l0 = lawOf(u);
  const uint64_t rebuilds = u.view().stats().rebuilds;
  for (uint32_t i = 0; i < meta::kVcaMarks; ++i) {
    CAPTURE(i);
    const double from = l0.marks[i].at;
    // Toward the fine zone's middle by a little (within the neighbours).
    const double to = from + (meta::kVcaMarkValue[i] > 0 ? -0.02 : 0.02);
    const size_t h0 = u.history();
    arcDrag(u, from, to, 10, mod::kCtrl);
    CHECK(u.history() == h0 + 1);
    CHECK(offsetOf(u, i) == doctest::Approx(to - l0.marks[i].base).epsilon(1e-3).scale(1.0));
    const meta::VcaLaw l = lawOf(u);
    CHECK(l.marks[i].at == doctest::Approx(to).epsilon(1e-3));
    CHECK(meta::vcaValue(l, l.marks[i].at) == doctest::Approx(meta::kVcaMarkValue[i]).epsilon(1e-9));
    for (uint32_t j = 0; j < meta::kVcaMarks; ++j)
      if (j != i) CHECK(l.marks[j].at == l0.marks[j].at);
    // Undo: back where it was; redo for the next one is not needed (each starts from the default).
    REQUIRE(u.ctx.undo());
    CHECK(offsetOf(u, i) == 0.0);
  }
  CHECK(u.view().stats().rebuilds == rebuilds);  // param writes: nothing re-lowers
  // Past its neighbour: +1 dragged to beyond +10 stops a gap short of it (the drag clamps, +10 does not move).
  arcDrag(u, l0.marks[3].at, 0.95, 16, mod::kCtrl);
  meta::VcaLaw l = lawOf(u);
  CHECK(l.marks[3].at == doctest::Approx(l0.marks[4].at - meta::kVcaMarkGap).epsilon(1e-9));
  CHECK(l.marks[4].at == doctest::Approx(l0.marks[4].at).epsilon(1e-12));  // GU.1: positions, float rounding
  // An asymmetric fine zone: -1 alone toward 9 o'clock; +1 stays where the last drag left it.
  const double plus = l.marks[3].at;
  arcDrag(u, l.marks[2].at, 0.26, 10, mod::kCtrl);
  l = lawOf(u);
  CHECK(l.marks[2].at == doctest::Approx(0.26).epsilon(1e-3));
  CHECK(l.marks[3].at == plus);
  CHECK(meta::vcaValue(l, 0.5 * (l.marks[2].at + l.marks[3].at)) == doctest::Approx(0.0).scale(1.0).epsilon(1e-9));
  // Ctrl+double-click a marker, Enter on an empty entry: its default alone (one step); undo takes it back.
  const size_t h0 = u.history();
  const Vec2 t = trackLocal(l.marks[3].at);
  u.d.doubleClick(u.at(t.x, t.y), mod::kCtrl);
  CHECK(u.history() == h0);  // a type-in opens; nothing yet
  (void)u.d.keyDown(key::kReturn);
  CHECK(u.history() == h0 + 1);
  CHECK(offsetOf(u, 3) == 0.0);
  CHECK(offsetOf(u, 2) != 0.0);
  REQUIRE(u.ctx.undo());
  CHECK(offsetOf(u, 3) != 0.0);
}

TEST_CASE("vca face (TX.vca-knob): the curve box's corner points Ctrl+drag each marker alone; the box drag stays symmetric") {
  VcaUi u;
  u.frame(1.5);
  u.openPanel();
  project_patch::ProjectCanvas& c = u.view().canvas();
  const meta::VcaLaw l0 = lawOf(u);
  // The -1 corner, left by 0.05 of the travel: one step, only -1 moves.
  const Vec2 c0 = cornerLocal(l0, l0.marks[2].at, -1.0);
  u.d.move(u.at(c0.x, c0.y));
  CHECK(std::string(c.info().title) == "Marker -1");
  CHECK(c.cursorAt(u.at(c0.x, c0.y)) == Cursor::ResizeEW);
  size_t h0 = u.history();
  const float dx = 0.05f * kBoxW;
  u.d.drag(u.at(c0.x, c0.y), u.at(c0.x - dx, c0.y + 7.0f), 6, mod::kCtrl);  // the height does not matter
  CHECK(u.history() == h0 + 1);
  meta::VcaLaw l = lawOf(u);
  CHECK(l.marks[2].at == doctest::Approx(l0.marks[2].at - 0.05).epsilon(1e-3));
  for (const uint32_t j : {0u, 1u, 3u, 4u, 5u}) CHECK(l.marks[j].at == l0.marks[j].at);
  // The +10 corner, right by 0.03.
  const Vec2 c4 = cornerLocal(l, l.marks[4].at, 10.0);
  u.d.move(u.at(c4.x, c4.y));
  CHECK(std::string(c.info().title) == "Marker +10");
  u.d.drag(u.at(c4.x, c4.y), u.at(c4.x + 0.03f * kBoxW, c4.y), 6, mod::kCtrl);
  l = lawOf(u);
  CHECK(l.marks[4].at == doctest::Approx(l0.marks[4].at + 0.03).epsilon(1e-3));
  CHECK(u.history() == h0 + 2);
  // The box body (away from the corners): the zone, symmetric — both ±1 bases move apart by the same travel, every
  // marker keeps its offset.
  const std::vector<double> offs = {offsetOf(u, 0), offsetOf(u, 1), offsetOf(u, 2), offsetOf(u, 3), offsetOf(u, 4), offsetOf(u, 5)};
  const Vec2 a = u.at(kBoxX - 20.0f, kPopTop + 33.0f);
  u.d.move(a);
  CHECK(std::string(c.info().title) == "Curve: bipolar log");
  u.d.drag(a, {a.x, a.y - 30.0f}, 6);
  CHECK(u.history() == h0 + 3);
  const meta::VcaLaw w = lawOf(u);
  CHECK(u.value(meta::kVcaZone) > 0.4);
  for (uint32_t i = 0; i < meta::kVcaMarks; ++i) CHECK(offsetOf(u, i) == offs[i]);
  CHECK(w.marks[2].base + w.marks[3].base == doctest::Approx(1.0));
  CHECK(l.marks[3].base - w.marks[3].base == doctest::Approx(w.marks[2].base - l.marks[2].base));
  CHECK(w.marks[3].base > l.marks[3].base);
  // Double-click the box: the zone and every marker back to their defaults (one step).
  u.d.doubleClick(a);
  CHECK(u.history() == h0 + 4);
  CHECK(u.value(meta::kVcaZone) == doctest::Approx(0.4));
  for (uint32_t i = 0; i < meta::kVcaMarks; ++i) CHECK(offsetOf(u, i) == 0.0);
  REQUIRE(u.ctx.undo());
  CHECK(offsetOf(u, 2) != 0.0);
}

TEST_CASE("vca face (TX.vca-knob): a marker outside min … max is hidden — not drawn, not hit, no effect") {
  VcaUi u;
  u.frame(1.5);
  project_patch::ProjectCanvas& c = u.view().canvas();
  const meta::VcaLaw l0 = lawOf(u);
  u.openPanel();
  // min -50: -100 is outside the range.
  u.d.doubleClick(u.at(kMinX, kRangeY));
  u.type("-50");
  (void)u.d.keyDown(key::kReturn);
  REQUIRE(u.value(meta::kVcaMin) == doctest::Approx(-50.0));
  (void)u.d.keyDown(key::kEscape);  // close the panel
  const meta::VcaLaw l = lawOf(u);
  CHECK_FALSE(l.marks[0].shown);
  CHECK(l.marks[1].shown);
  // Where -100's tick was (p ≈ l0's) the band is only the knob's now; -10's tick still answers.
  const Vec2 t0 = trackLocal(l0.marks[0].at);
  u.d.move(u.at(t0.x, t0.y));
  CHECK(std::string(c.info().title).find("Marker -100") == std::string::npos);
  const Vec2 t1 = trackLocal(l.marks[1].at);
  u.d.move(u.at(t1.x, t1.y));
  CHECK(std::string(c.info().title) == "Marker -10");
}

// ---- TX.vca-knob2: paired markers, typed places, the display scale ------------------------------------------------

TEST_CASE("vca face (TX.vca-knob2): a plain drag moves the marker's pair mirrored — on the track and in the box; clamps hold for both") {
  VcaUi u;
  u.frame(1.5);
  project_patch::ProjectCanvas& c = u.view().canvas();
  const meta::VcaLaw l0 = lawOf(u);
  const uint64_t rebuilds = u.view().stats().rebuilds;
  // Each pair from each side: +k out by 0.02 of the travel moves -k out by the same (one undo step); the others stay.
  for (uint32_t i = 0; i < meta::kVcaMarks; ++i) {
    CAPTURE(i);
    const uint32_t j = meta::vcaMarkPartner(i);
    REQUIRE(j < meta::kVcaMarks);
    const double dir = meta::kVcaMarkValue[i] > 0 ? -0.02 : 0.02;  // inward, within the neighbours
    const size_t h0 = u.history();
    arcDrag(u, l0.marks[i].at, l0.marks[i].at + dir);
    CHECK(u.history() == h0 + 1);
    const meta::VcaLaw l = lawOf(u);
    CHECK(l.marks[i].at == doctest::Approx(l0.marks[i].at + dir).epsilon(1e-3));
    // Mirrored: the same travel the other way (GU.1: each stored as its knob position, within float rounding).
    CHECK(offsetOf(u, j) == doctest::Approx(-offsetOf(u, i)).epsilon(1e-12));
    CHECK(l.marks[j].at - l0.marks[j].at == doctest::Approx(-(l.marks[i].at - l0.marks[i].at)).epsilon(1e-12));
    for (uint32_t k = 0; k < meta::kVcaMarks; ++k)
      if (k != i && k != j) CHECK(offsetOf(u, k) == 0.0);
    CHECK(meta::vcaValue(l, l.marks[j].at) == doctest::Approx(meta::kVcaMarkValue[j]).epsilon(1e-9));
    REQUIRE(u.ctx.undo());
    CHECK(offsetOf(u, i) == 0.0);
    CHECK(offsetOf(u, j) == 0.0);
  }
  CHECK(u.view().stats().rebuilds == rebuilds);
  // An asymmetric fine zone (+1 alone) stays asymmetric: a plain drag of -1 moves +1 by the mirrored travel.
  arcDrag(u, l0.marks[3].at, l0.marks[3].at - 0.05, 10, mod::kCtrl);
  const double asym = offsetOf(u, 3);
  REQUIRE(asym == doctest::Approx(-0.05).epsilon(1e-3));
  meta::VcaLaw l = lawOf(u);
  arcDrag(u, l.marks[2].at, l.marks[2].at - 0.03);
  CHECK(offsetOf(u, 2) == doctest::Approx(-0.03).epsilon(1e-3));
  CHECK(offsetOf(u, 3) - asym == doctest::Approx(-offsetOf(u, 2)).epsilon(1e-12));
  REQUIRE(u.ctx.undo());
  REQUIRE(u.ctx.undo());
  // The clamps hold for both: +1 dragged in toward 12 o'clock stops where the pair meets (a gap apart), together.
  arcDrag(u, l0.marks[3].at, 0.4, 16);
  l = lawOf(u);
  CHECK(l.marks[3].at - l.marks[2].at == doctest::Approx(meta::kVcaMarkGap).epsilon(1e-6));
  CHECK(offsetOf(u, 2) == -offsetOf(u, 3));
  REQUIRE(u.ctx.undo());
  // +10 dragged out past +100: it stops a gap short of +100, and -10 stops with it (mirrored), -100 unmoved.
  arcDrag(u, l0.marks[4].at, 0.98, 16);
  l = lawOf(u);
  CHECK(l.marks[4].at == doctest::Approx(l0.marks[5].at - meta::kVcaMarkGap).epsilon(1e-6));
  CHECK(offsetOf(u, 1) == -offsetOf(u, 4));
  CHECK(offsetOf(u, 5) == 0.0);
  CHECK(offsetOf(u, 0) == 0.0);
  REQUIRE(u.ctx.undo());
  // In the box: the -10 corner dragged left moves +10 right by the same travel.
  u.openPanel();
  const Vec2 c1 = cornerLocal(l0, l0.marks[1].at, -10.0);
  u.d.move(u.at(c1.x, c1.y));
  CHECK(std::string(c.info().title) == "Marker -10");
  CHECK(std::string(c.info().body).find("with +10") != std::string::npos);
  const size_t h0 = u.history();
  u.d.drag(u.at(c1.x, c1.y), u.at(c1.x - 0.02f * kBoxW, c1.y), 6);
  CHECK(u.history() == h0 + 1);
  CHECK(offsetOf(u, 1) == doctest::Approx(-0.02).epsilon(1e-3));
  CHECK(offsetOf(u, 4) == -offsetOf(u, 1));
  // A marker whose pair is hidden moves alone (min 0: no negative markers).
  REQUIRE(u.ctx.undo());
  u.d.doubleClick(u.at(kMinX, kRangeY));
  u.type("0");
  (void)u.d.keyDown(key::kReturn);
  l = lawOf(u);
  REQUIRE(!l.marks[1].shown);
  REQUIRE(l.marks[4].shown);
  (void)u.d.keyDown(key::kEscape);
  arcDrag(u, l.marks[4].at, l.marks[4].at - 0.02);
  CHECK(offsetOf(u, 4) == doctest::Approx(-0.02).epsilon(1e-3));
  CHECK(offsetOf(u, 1) == 0.0);
}

TEST_CASE("vca face (TX.vca-knob2): double-click a marker to type its place (% of the knob) — paired, Ctrl alone, empty = default, Esc") {
  VcaUi u;
  u.frame(1.5);
  const meta::VcaLaw l0 = lawOf(u);
  // +10 (on the track) at 80 %: -10 mirrored, one step.
  Vec2 t = trackLocal(l0.marks[4].at);
  size_t h0 = u.history();
  u.d.doubleClick(u.at(t.x, t.y));
  u.type("80");
  CHECK(u.history() == h0);  // nothing until Enter
  (void)u.d.keyDown(key::kReturn);
  CHECK(u.history() == h0 + 1);
  meta::VcaLaw l = lawOf(u);
  CHECK(l.marks[4].at == doctest::Approx(0.80).epsilon(1e-9));
  CHECK(offsetOf(u, 1) == doctest::Approx(-offsetOf(u, 4)).epsilon(1e-12));  // GU.1: positions, float rounding
  CHECK(meta::vcaValue(l, 0.80) == doctest::Approx(10.0).epsilon(1e-9));
  // Ctrl+double-click -10 and type 20%: -10 alone.
  t = trackLocal(l.marks[1].at);
  const double plus10 = offsetOf(u, 4);
  u.d.doubleClick(u.at(t.x, t.y), mod::kCtrl);
  u.type("20%");
  (void)u.d.keyDown(key::kReturn);
  l = lawOf(u);
  CHECK(l.marks[1].at == doctest::Approx(0.20).epsilon(1e-9));
  CHECK(offsetOf(u, 4) == plus10);
  // Esc cancels.
  h0 = u.history();
  t = trackLocal(l.marks[4].at);
  u.d.doubleClick(u.at(t.x, t.y));
  u.type("90");
  (void)u.d.keyDown(key::kEscape);
  CHECK(u.history() == h0);
  CHECK(offsetOf(u, 4) == plus10);
  // A place past a neighbour: the pair moves together until one of them is a gap from its neighbour (+10 at 99 %:
  // -10 meets -100 first; the asymmetry of the pair is kept).
  const meta::VcaLaw before = lawOf(u);
  u.d.doubleClick(u.at(t.x, t.y));
  u.type("99");
  (void)u.d.keyDown(key::kReturn);
  l = lawOf(u);
  CHECK(l.marks[1].at == doctest::Approx(l.marks[0].at + meta::kVcaMarkGap).epsilon(1e-6));
  CHECK(l.marks[4].at - before.marks[4].at == doctest::Approx(before.marks[1].at - l.marks[1].at).epsilon(1e-9));
  CHECK(l.marks[4].at < l.marks[5].at - meta::kVcaMarkGap);
  // In the box (the panel open): the +1 corner, an empty entry: the pair's default places, one step.
  REQUIRE(u.ctx.undo());
  arcDrag(u, l0.marks[3].at, l0.marks[3].at - 0.04);  // move the ±1 pair (the panel stays open: the track is the face's)
  u.openPanel();
  l = lawOf(u);
  REQUIRE(offsetOf(u, 3) != 0.0);
  const Vec2 c3 = cornerLocal(l, l.marks[3].at, 1.0);
  h0 = u.history();
  u.d.doubleClick(u.at(c3.x, c3.y));
  (void)u.d.keyDown(key::kReturn);
  CHECK(u.history() == h0 + 1);
  CHECK(offsetOf(u, 3) == 0.0);
  CHECK(offsetOf(u, 2) == 0.0);
  CHECK(offsetOf(u, 4) == plus10);  // the others untouched
  // Not a number: a notice, nothing written ('k' and '-' are not accepted in a place).
  h0 = u.history();
  u.d.doubleClick(u.at(c3.x, c3.y));
  u.type(".");
  (void)u.d.keyDown(key::kReturn);
  CHECK(u.history() == h0);
}

TEST_CASE("vca face (TX.vca-knob2): the curve box's scale is a display change only — the law plays bit for bit as before") {
  // The law's values at fixed knob positions (the default state and a moved state), pinned from the law as TX.vca-knob
  // left it (TX.vca-knob2 changes no line of vcaLaw / vcaValue / the expansion: these must never move).
  const meta::VcaLaw d = meta::vcaLaw(meta::VcaCurve::Log, -20000.0, 20000.0, 0.4, 0.0);
  const std::array<double, meta::kVcaMarks> off = {0.01, -0.02, 0.03, -0.01, 0.02, -0.03};
  const meta::VcaLaw m = meta::vcaLaw(meta::VcaCurve::Log, -150.0, 2500.0, 0.55, 0.0, off);
  struct Pin {
    double p, dv, mv;
  };
  const Pin pins[] = {
      {0.05, -3833.3732421056907, -26.950831262457633},
      {0.25, -3.810502042682069, -0.82619273903858725},
      {0.5, 0.0, 0.15419941782415814},
      {0.71, 1.1110392389233588, 0.97772882958886331},
      {0.8, 24.35261693294462, 4.4432226735926132},
      {0.97, 7424.4983335083862, 1271.0011303225554},
  };
  for (const Pin& q : pins) {
    CAPTURE(q.p);
    CHECK(meta::vcaValue(d, q.p) == q.dv);
    CHECK(meta::vcaValue(m, q.p) == q.mv);
  }
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
