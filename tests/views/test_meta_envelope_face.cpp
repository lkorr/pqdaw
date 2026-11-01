// TMM.11: the Envelope face — goldens at zoom 0.1 / 0.5 / 1 / 2 and 1x / 2x with eight voices in different stages
// (rising, at the peak, decaying, sustained, released early, releasing), the same primitives at every zoom, tiles equal
// to direct drawing, every gesture through the project patch view as one undo step and no rebuild (attack, decay +
// sustain in one 2-D drag, release, the sustain plateau, the three bends, amount, double-click reset, Esc), the info
// view and cursors, the chain and nested faces bound to their state, the scenes device-meta-envelope and
// project-meta-envelope, and the live layer's paint cost.

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
#include "pq/meta/envelope.h"
#include "pq/meta/registry.h"
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

const meta::Type& envType() { return meta::envelopeType(); }

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// Fixed state (the goldens): attack 30 ms bent up, decay 400 ms bent down a little, sustain 0.45, release 900 ms bent
// toward a line, amount 0.8.
class EnvTarget final : public MetaTarget {
public:
  double v[meta::kEnvParams] = {0.03, 0.4, 0.45, 0.9, 0.5, -0.25, 0.35, 0.8};
  double value(size_t p) const override { return p < meta::kEnvParams ? v[p] : 0.0; }
  void begin(const char*) override {}
  void set(size_t p, double x) override {
    if (p < meta::kEnvParams) v[p] = x;
  }
  void end() override {}
  void cancel() override {}
  void reset(size_t p) override {
    if (p < meta::kEnvParams) v[p] = envType().params[p].def;
  }
  const meta::Type* targetType() const noexcept override { return &envType(); }
};

// Eight voices in different stages, each with the levels its tap sampled during the last displayed frame.
class StageVoices final : public VoiceTapReader {
public:
  struct V {
    uint32_t slot;
    double from, to;
    bool released;
  };
  std::vector<V> v = {{0, 0.10, 0.28, false}, {1, 0.55, 0.78, false}, {2, 0.97, 1.0, false}, {3, 0.80, 0.66, false},
                      {4, 0.45, 0.45, false}, {5, 0.62, 0.50, true},  {6, 0.30, 0.24, true}, {7, 0.06, 0.04, true}};
  uint64_t serialNow = 1;
  uint32_t read(std::span<FaceVoice> out) override {
    uint32_t n = 0;
    for (const V& x : v) {
      if (n >= out.size()) break;
      FaceVoice& f = out[n++];
      f.slot = x.slot;
      f.sampleCount = 12;
      for (uint32_t k = 0; k < 12; ++k)
        f.samples[k] = static_cast<float>(meta::encodeEnvelopeTap(x.from + (x.to - x.from) * k / 11.0, x.released));
      f.phase = f.samples[11];
    }
    return n;
  }
  uint64_t serial() const noexcept override { return serialNow; }
  bool live() const noexcept override { return false; }
};

std::shared_ptr<const circuit::CircuitScene> envScene() {
  const std::string text = "patch EnvDemo {\n  in    x   : audio\n  out   y   : audio\n  env1 = meta_envelope(x)\n  y = env1\n}\n";
  auto doc = circuit::loadPatchDocument((fs::temp_directory_path() / "pq_meta_env_face_test.pqp").string(), text);
  REQUIRE_MESSAGE(doc.ok(), (doc.ok() ? "" : doc.error().message));
  const graph::Patch* p = (*doc)->find("EnvDemo");
  REQUIRE(p != nullptr);
  return circuit::buildScene(*p);
}

struct EnvRig {
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
  StageVoices voices;
  float scale = 1.0f;

  EnvRig(uint32_t lw, uint32_t lh, float sc, bool withVoices) : scale(sc) {
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
      b.target = std::make_unique<EnvTarget>();
      b.voices = withVoices ? &voices : nullptr;
      return b;
    };
    canvas->setScene(envScene(), false);
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
    v.x = n.x + 104.0 - lw / zoom * 0.5;
    v.y = n.y + 64.0 - lh / zoom * 0.5;
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

uint32_t rigW(double zoom) { return static_cast<uint32_t>(std::max(96.0, std::ceil(208.0 * zoom + 48.0))); }
uint32_t rigH(double zoom) { return static_cast<uint32_t>(std::max(96.0, std::ceil(128.0 * zoom + 48.0))); }

// The face's geometry (envelope_face.cpp's layout), for gestures.
constexpr float kX0 = 20.0f, kY0 = 104.0f, kY1 = 26.0f, kSeg = 44.0f, kHold = 16.0f;  // before the fit
float travelOf(size_t p, double v) { return static_cast<float>(meta::valueToTravel(envType().params[p], v)); }
float levelY(double l) { return kY0 + (kY1 - kY0) * static_cast<float>(l); }

}  // namespace

TEST_CASE("envelope face: registered; a nested call's symbol is its face, sized 208 x 128") {
  REQUIRE(hasFace(envType()));
  CHECK(faceSize(envType()).x == 208.0f);
  CHECK(faceSize(envType()).y == 128.0f);
  TestDriver d(600, 400);
  auto& canvas = d.setRoot<circuit::CircuitCanvas>("circuit");
  canvas.setScene(envScene(), false);
  d.tree().layoutNow();
  REQUIRE(canvas.faceCount() == 1);
  CHECK(&canvas.faceAt(0).type() == &envType());
}

TEST_CASE("envelope face: golden at zoom 0.1 / 0.5 / 1 / 2, 1x and 2x, eight voices in different stages") {
  for (const float scale : {1.0f, 2.0f})
    for (const double zoom : {0.1, 0.5, 1.0, 2.0}) {
      EnvRig rig(rigW(zoom), rigH(zoom), scale, true);
      rig.centre(zoom);
      rig.settle();
      REQUIRE(rig.canvas->faceAt(0).sounding().size() == 8u);
      const std::string name = "meta-envelope-z" + zoomName(zoom) + "-" + zoomName(scale) + "x";
      CAPTURE(name);
      CHECK(rig.canvas->stats().facesPainted == 1u);
      CHECK(rig.canvas->stats().faceLayersFromTiles == 3u);
      checkImage(name, rig.draw(rig.res));
    }
}

TEST_CASE("envelope face: the same primitives at every zoom; tiles equal direct drawing") {
  EnvRig rig(500, 360, 1.0f, true);
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
  CHECK(prims[0] > 100u);
  for (const float scale : {1.0f, 2.0f})
    for (const double zoom : {0.5, 1.0, 2.0}) {
      EnvRig r(rigW(zoom), rigH(zoom), scale, false);
      r.centre(zoom);
      r.settle();
      const Image tiled = r.draw(r.res);
      CHECK(r.canvas->stats().faceLayersFromTiles == 3u);
      const Image direct = r.draw(r.resNoTiles);
      CHECK(r.canvas->stats().faceLayersDirect == 3u);
      const std::string name = "meta-envelope-tiles-z" + zoomName(zoom) + "-" + zoomName(scale) + "x";
      CAPTURE(name);
      CHECK(pq::ui::test::matchesGolden(name, tiled, direct));
    }
}

// ---- gestures through the project patch view -------------------------------------------------------------------

namespace {

struct EnvUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  project_patch::MetaEnvelopeIds ids;

  EnvUi() {
    ctx.setDeviceLibrary(&library());
    REQUIRE(project_patch::buildMetaEnvelopeProject(ctx.store(), &ids).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setShown(true, false);
    d.tree().layoutNow();
  }
  ~EnvUi() { d.tree().setRoot(std::make_unique<Widget>()); }

  project_patch::ProjectPatchView& view() const {
    project_patch::ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Device& dev() const {
    const doc::Device* dv = doc::findDevice(*ctx.doc(), ids.env).device;
    REQUIRE(dv != nullptr);
    return *dv;
  }
  double value(size_t p) const { return doc::metaParamValue(dev(), "", envType(), p); }
  const project_patch::Node& node() const { return view().patch().nodes[view().patch().nodeOf(ids.env)]; }
  void frame(double zoom) {
    const project_patch::Node& n = node();
    project_patch::ProjectCanvas& c = view().canvas();
    const double us = d.tree().scale();
    circuit::CircuitCanvas::View v;
    v.zoom = zoom;
    v.x = n.x + 104.0 - c.rect().w / (zoom * us) * 0.5;
    v.y = n.y + 64.0 - c.rect().h / (zoom * us) * 0.5;
    c.setView(v);
    for (int i = 0; i < 4; ++i) d.step();
  }
  Vec2 at(float x, float y) const {
    const project_patch::Node& n = node();
    return view().canvas().toDevice({n.x + x, n.y + y});
  }
  // The fit (the time segments fill 136 units, ×1 … ×4) and a full travel's width.
  float seg() const {
    const float raw = kSeg * (travelOf(meta::kEnvAttack, value(meta::kEnvAttack)) + travelOf(meta::kEnvDecay, value(meta::kEnvDecay)) +
                              travelOf(meta::kEnvRelease, value(meta::kEnvRelease)));
    return kSeg * std::clamp(136.0f / std::max(raw, 1e-3f), 1.0f, 4.0f);
  }
  // The breakpoints and bend rings now (face-local).
  Vec2 peak() const { return {kX0 + seg() * travelOf(meta::kEnvAttack, value(meta::kEnvAttack)), kY1}; }
  Vec2 decayEnd() const {
    return {peak().x + seg() * travelOf(meta::kEnvDecay, value(meta::kEnvDecay)), levelY(value(meta::kEnvSustain))};
  }
  Vec2 relEnd() const { return {decayEnd().x + kHold + seg() * travelOf(meta::kEnvRelease, value(meta::kEnvRelease)), kY0}; }
  Vec2 attackBend() const {
    const meta::EnvSegmentLaw l = meta::envAttackLaw(value(meta::kEnvACurve));
    return {kX0 + 0.5f * (peak().x - kX0), levelY(meta::envAttackLevel(l, 0.5))};
  }
  Vec2 decayBend() const {
    const meta::EnvSegmentLaw l = meta::envFallLaw(value(meta::kEnvDCurve));
    const double sus = value(meta::kEnvSustain);
    return {peak().x + 0.5f * (decayEnd().x - peak().x), levelY(meta::envFallLevel(l, 1.0, sus, 1.0 - sus, 0.5))};
  }
};

}  // namespace

TEST_CASE("envelope face: a chain Envelope is its face in the project patch view; the nested env1 too") {
  EnvUi u;
  CHECK(u.node().face == &envType());
  CHECK(u.node().w == 208.0f);
  REQUIRE(u.d.enableGpu().ok());
  project_patch::ProjectCanvas& c = u.view().canvas();
  const uint32_t sn = u.view().scene().sceneNode[u.view().patch().nodeOf(u.ids.env)];
  MetaFace* f = c.faceOn(sn);
  REQUIRE(f != nullptr);
  CHECK(f->target().editable());
  CHECK(f->target().value(meta::kEnvSustain) == doctest::Approx(0.35));
  CHECK(f->target().value(meta::kEnvACurve) == doctest::Approx(0.45));
  u.view().setDeviceExpanded(u.ids.shapeInst, true);
  for (int i = 0; i < 4; ++i) u.d.step();
  REQUIRE(u.view().deviceExpanded(u.ids.shapeInst));
  size_t envs = 0, nested = 0;
  for (size_t i = 0; i < c.faceCount(); ++i) {
    if (&c.faceAt(i).type() != &envType()) continue;
    ++envs;
    if (c.faceAt(i).target().value(meta::kEnvAttack) == doctest::Approx(0.4) && c.faceAt(i).target().editable()) ++nested;
  }
  CHECK(envs == 2u);
  CHECK(nested == 1u);
}

TEST_CASE("envelope face: breakpoint, plateau, bend and amount drags at zoom 0.1 and 1.5 — one undo step each, no rebuild") {
  for (const double zoom : {0.1, 1.5}) {
    CAPTURE(zoom);
    EnvUi u;
    u.frame(zoom);
    const double us = u.d.tree().scale();
    const auto span = [&](float extent) { return std::max(extent * zoom * us, 150.0 * us); };
    const auto& P = envType().params;
    const uint64_t rebuilds = u.view().stats().rebuilds;
    // Attack: the peak right by 30 px.
    size_t h0 = u.history();
    double t0 = travelOf(meta::kEnvAttack, u.value(meta::kEnvAttack));
    float w = u.seg();  // the fit is frozen while a drag runs
    Vec2 a = u.at(u.peak().x, u.peak().y);
    u.d.drag(a, {a.x + 30.0f, a.y}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kEnvAttack) ==
          doctest::Approx(meta::travelToValue(P[meta::kEnvAttack], t0 + 30.0 / span(w))).epsilon(1e-4));
    // Decay + sustain: the decay's end left 20 px and up 25 px — one step for both.
    h0 = u.history();
    t0 = travelOf(meta::kEnvDecay, u.value(meta::kEnvDecay));
    const double s0 = u.value(meta::kEnvSustain);
    w = u.seg();
    a = u.at(u.decayEnd().x, u.decayEnd().y);
    u.d.drag(a, {a.x - 20.0f, a.y - 25.0f}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kEnvDecay) ==
          doctest::Approx(meta::travelToValue(P[meta::kEnvDecay], t0 - 20.0 / span(w))).epsilon(1e-4));
    CHECK(u.value(meta::kEnvSustain) == doctest::Approx(s0 + 25.0 / span(kY0 - kY1)).epsilon(1e-4));
    // Release: right 15 px with Shift (a tenth).
    h0 = u.history();
    t0 = travelOf(meta::kEnvRelease, u.value(meta::kEnvRelease));
    w = u.seg();
    a = u.at(u.relEnd().x, u.relEnd().y);
    u.d.drag(a, {a.x + 15.0f, a.y}, 6, mod::kShift);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kEnvRelease) ==
          doctest::Approx(meta::travelToValue(P[meta::kEnvRelease], t0 + 1.5 / span(w))).epsilon(1e-4));
    // The plateau: down 20 px (sustain).
    h0 = u.history();
    const double s1 = u.value(meta::kEnvSustain);
    a = u.at(u.decayEnd().x + kHold * 0.5f, u.decayEnd().y);
    u.d.drag(a, {a.x, a.y + 20.0f}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kEnvSustain) == doctest::Approx(s1 - 20.0 / span(kY0 - kY1)).epsilon(1e-4));
    // Bends: the attack's ring down 20 px (travel over −1 … 1), the decay's up 30 px.
    h0 = u.history();
    const double c0 = u.value(meta::kEnvACurve);
    a = u.at(u.attackBend().x, u.attackBend().y);
    u.d.drag(a, {a.x, a.y + 20.0f}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kEnvACurve) == doctest::Approx(c0 - 2.0 * 20.0 / span(kY0 - kY1)).epsilon(1e-4));
    h0 = u.history();
    const double d0 = u.value(meta::kEnvDCurve);
    a = u.at(u.decayBend().x, u.decayBend().y);
    u.d.drag(a, {a.x, a.y - 30.0f}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kEnvDCurve) == doctest::Approx(d0 + 2.0 * 30.0 / span(kY0 - kY1)).epsilon(1e-4));
    // Amount: down 40 px.
    h0 = u.history();
    a = u.at(192.0f, 40.0f);
    u.d.drag(a, {a.x, a.y + 40.0f}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kEnvAmount) == doctest::Approx(1.0 - 40.0 / span(92.0f)).epsilon(1e-4));
    CHECK(u.view().stats().rebuilds == rebuilds);  // param writes: nothing re-lowers
    // Double-click the decay's end: decay and sustain back to their defaults in one step; undo takes it back.
    h0 = u.history();
    u.d.doubleClick(u.at(u.decayEnd().x, u.decayEnd().y));
    CHECK(u.history() == h0 + 1);
    CHECK(u.value(meta::kEnvDecay) == doctest::Approx(P[meta::kEnvDecay].def));
    CHECK(u.value(meta::kEnvSustain) == doctest::Approx(P[meta::kEnvSustain].def));
    REQUIRE(u.ctx.undo());
    CHECK(u.value(meta::kEnvSustain) != doctest::Approx(P[meta::kEnvSustain].def));
  }
}

TEST_CASE("envelope face: Esc during a drag undoes it") {
  EnvUi u;
  u.frame(1.0);
  const double before = u.value(meta::kEnvAttack);
  const Vec2 a = u.at(u.peak().x, u.peak().y);
  u.d.down(a);
  u.d.move({a.x + 20.0f, a.y});
  u.d.move({a.x + 40.0f, a.y});
  CHECK(u.value(meta::kEnvAttack) > before);
  const size_t h0 = u.history();
  (void)u.d.keyDown(key::kEscape);
  (void)u.d.up({a.x + 40.0f, a.y});
  CHECK(u.value(meta::kEnvAttack) == doctest::Approx(before));
  CHECK(u.history() == h0);
}

TEST_CASE("envelope face: the info view names the hovered control; cursors follow it") {
  EnvUi u;
  u.frame(1.5);
  project_patch::ProjectCanvas& c = u.view().canvas();
  u.d.move(u.at(u.peak().x, u.peak().y));
  CHECK(std::string(c.info().title) == "Attack");
  CHECK(c.cursorAt(u.at(u.peak().x, u.peak().y)) == Cursor::ResizeEW);
  u.d.move(u.at(u.decayEnd().x, u.decayEnd().y));
  CHECK(std::string(c.info().title) == "Decay and sustain");
  CHECK(c.cursorAt(u.at(u.decayEnd().x, u.decayEnd().y)) == Cursor::Move);
  u.d.move(u.at(u.decayEnd().x + kHold * 0.5f, u.decayEnd().y));
  CHECK(std::string(c.info().title) == "Sustain");
  u.d.move(u.at(u.decayBend().x, u.decayBend().y));
  CHECK(std::string(c.info().title) == "Decay curve");
  CHECK(c.cursorAt(u.at(u.decayBend().x, u.decayBend().y)) == Cursor::ResizeNS);
  u.d.move(u.at(u.relEnd().x, u.relEnd().y));
  CHECK(std::string(c.info().title) == "Release");
  u.d.move(u.at(192.0f, 60.0f));
  CHECK(std::string(c.info().title) == "Amount");
}

TEST_CASE("render: device-meta-envelope matches the golden image") { checkScene("device-meta-envelope"); }

TEST_CASE("render: project-meta-envelope matches the golden image") { checkScene("project-meta-envelope"); }

TEST_CASE("envelope face: the live layer's paint cost (2x, 8 voices) and 16 animating faces" *
          doctest::test_suite("perf")) {  // wall-clock bounds: load-sensitive
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  std::unique_ptr<MetaFace> face = makeFace(envType());
  REQUIRE(face != nullptr);
  StageVoices voices;
  auto target = std::make_unique<EnvTarget>();
  EnvTarget* tg = target.get();
  face->bind(std::move(target), &voices);
  FaceJack jacks[2];
  jacks[0] = {{0.0f, 64.0f}, true, true, true, "in"};
  jacks[1] = {{208.0f, 64.0f}, false, true, true, "out"};
  FaceEnv env;
  env.theme = &*theme;
  env.uiScale = 2.0f;
  env.jacks = jacks;
  DrawList dl;
  auto paintOnce = [&](uint32_t faces) {
    dl.reset(2560, 1440);
    for (uint32_t k = 0; k < faces; ++k) {
      ui::FacePainter p(dl, {{static_cast<float>(20 + 420 * (k % 6)), static_cast<float>(20 + 280 * (k / 6))}, 2.0f});
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
    // A drag step: a value changes, the shape is laid out again.
    tg->v[meta::kEnvDCurve] = -0.5 + 0.004 * i;
    t0 = std::chrono::steady_clock::now();
    paintOnce(1);
    edit.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(one.begin(), one.end());
  std::sort(sixteen.begin(), sixteen.end());
  std::sort(edit.begin(), edit.end());
  MESSAGE("envelope live layer, 1 face (2x, 8 voices): median " << one[110] << " ms; 16 faces: median " << sixteen[110]
                                                                << " ms; a drag step (shape re-laid): median " << edit[110] << " ms");
  CHECK(face->sounding().size() == 8u);
#if PQ_OPTIMIZED
  CHECK(one[110] < 0.1);
  CHECK(sixteen[110] < 1.0);
  CHECK(edit[110] < 0.2);
#endif
}
