// TMM.4: meta-module faces — the vector face toolkit (ui::FacePainter, static layers in coverage tiles), hosting in the
// circuit canvas at every zoom and in the project patch view (a chain Phase accumulator's block is its face), the
// Phase accumulator face's drags at zoom 0.05 and 3 (one undo step each, the right values), double-click reset, the
// expand / close gestures, nested faces in the Device-level editor (a typed `acc1 = meta_phase_acc()` gets a face that
// edits the device's state), voice dots from a synthetic tap (damage only the live rect, idle when silent). Goldens:
// meta-phase-acc at zoom 0.05 / 0.25 / 1 / 3 and 1x / 2x, project-meta-faces.

#include <doctest/doctest.h>

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

#include "../../src/views/src/meta/faces.h"  // TMM.17: the voice colour / orbit maps (tests)
#include "../ui/golden_check.h"
#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/meta/phase_acc.h"
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

constexpr float kPiF = std::numbers::pi_v<float>;

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

const meta::Type& phaseAcc() { return meta::phaseAccType(); }
size_t paramOf(const char* name) { return static_cast<size_t>(phaseAcc().paramIndex(name)); }

// Fixed values (the goldens): 440 Hz, phase 0.3, amplitude 2, pitch +7 semitones (TMM.15: the readout shows).
class FixedTarget final : public MetaTarget {
public:
  double v[4] = {440.0, 0.3, 2.0, 700.0};
  double value(size_t p) const override {
    const std::string_view n = phaseAcc().params[p].name;
    return n == "freq" ? v[0] : (n == "phase" ? v[1] : (n == "amp" ? v[2] : v[3]));
  }
  void begin(const char*) override {}
  void set(size_t, double) override {}
  void end() override {}
  void cancel() override {}
  void reset(size_t) override {}
};

// A small patch whose one meta call is drawn as a face: two inputs wired into its hz and pm, its output to `y`.
std::shared_ptr<const circuit::CircuitScene> faceScene() {
  const std::string text =
      "patch FaceDemo {\n"
      "  in    note : audio\n"
      "  in    fm   : audio\n"
      "  out   y    : audio\n"
      "  acc1 = meta_phase_acc(note, fm)\n"
      "  y = acc1\n"
      "}\n";
  auto doc = circuit::loadPatchDocument((fs::temp_directory_path() / "pq_meta_face_test.pqp").string(), text);
  REQUIRE_MESSAGE(doc.ok(), (doc.ok() ? "" : doc.error().message));
  const graph::Patch* p = (*doc)->find("FaceDemo");
  REQUIRE(p != nullptr);
  return circuit::buildScene(*p);
}

// The canvas alone in a tree, painted through a real renderer (as test_circuit.cpp's rig), showing faceScene() with
// fixed values and still voices, the face centred.
struct FaceRig {
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
  SyntheticVoices voices;
  float scale = 1.0f;

  FaceRig(uint32_t lw, uint32_t lh, float sc) : scale(sc) {
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
    // TMM.13-fix2 (MM review "UI and aesthetic" 8): an A major chord at 48 kHz (each dot in its note's colour, not 0 Hz red).
    voices.setVoices({{0, 7.0, 0.04, 0.0, 220.0 / 48000.0}, {1, 5.0, 0.31, 0.0, 277.183 / 48000.0}, {2, 9.0, 0.57, 0.0, 329.628 / 48000.0},
                      {3, 6.0, 0.83, 0.0, 440.0 / 48000.0}});
    voices.setRate(48000.0);
    voices.advance(1.0 / 60.0);
    voices.freeze();
    canvas->faceBinder = [this](const circuit::SceneFace&) {
      circuit::CircuitCanvas::FaceBinding b;
      b.target = std::make_unique<FixedTarget>();
      b.voices = &voices;
      return b;
    };
    canvas->setScene(faceScene(), false);
    auto tg = OffscreenTarget::create(*gpu, w, h, 1);
    REQUIRE(tg.ok());
    target = std::move(*tg);
    tree.layoutNow();
  }
  // The face's centre at the canvas's centre, at `zoom`.
  void centre(double zoom) {
    REQUIRE(canvas->faceCount() == 1);
    const circuit::SceneNode& n = canvas->scene()->nodes[canvas->faceNode(0)];
    circuit::CircuitCanvas::View v;
    v.zoom = zoom;
    const double lw = canvas->rect().w / static_cast<double>(scale), lh = canvas->rect().h / static_cast<double>(scale);
    v.x = n.x + 60.0 - lw / zoom * 0.5;
    v.y = n.y + 60.0 - lh / zoom * 0.5;
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

std::string zoomName(double z) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", z);
  return buf;
}

// The canvas size for a zoom: the face plus a margin, at least 96 logical px.
uint32_t rigSize(double zoom) { return static_cast<uint32_t>(std::max(96.0, std::ceil(120.0 * zoom + 48.0))); }

}  // namespace

// ---- the toolkit ----------------------------------------------------------------------------------------------------

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

TEST_CASE("faces: the registry knows the Phase accumulator; a nested call's symbol is its face") {
  REQUIRE(hasFace(phaseAcc()));
  CHECK(faceSize(phaseAcc()).x == 120.0f);
  CHECK(faceSize(phaseAcc()).y == 120.0f);
  std::unique_ptr<MetaFace> f = makeFace(phaseAcc());
  REQUIRE(f != nullptr);
  CHECK(f->staticLayerCount() <= 3u);
  CHECK_FALSE(f->target().editable());  // unbound: the defaults, read-only
  const auto sc = faceScene();
  REQUIRE(sc->faces.size() == 1);
  const circuit::SceneFace& sf = sc->faces[0];
  CHECK(sf.type == &phaseAcc());
  CHECK(sf.path == "acc1");
  const circuit::SceneNode& n = sc->nodes[sf.node];
  CHECK(n.w == 120.0f);
  CHECK(n.h == 120.0f);
  // Its pins on the face's edges (hz, pm wired: visible), no pin stubs (the face draws its jacks).
  const circuit::ScenePin& hz = sc->pins[n.firstPin];
  CHECK(hz.x == n.x);
  CHECK(hz.y == n.y + 24.0f);
  CHECK(hz.stub == 0.0f);
  CHECK(hz.visible);
  // Label-free: no title or pin labels for the face's node (the binding name stays above it).
  for (const circuit::SceneLabel& lb : sc->labels)
    if (lb.node == sf.node) CHECK(lb.role == circuit::SceneLabel::Role::Caption);
}

TEST_CASE("faces: hit test maps the arcs to frequency, phase, amplitude and pitch, the rest to the frame") {
  std::unique_ptr<MetaFace> f = makeFace(phaseAcc());
  auto at = [](float deg, float r) {
    const float a = deg * kPiF / 180.0f;
    return Vec2{60.0f + r * std::cos(a), 64.0f + r * std::sin(a)};
  };
  for (const float s : {0.05f, 1.0f, 3.0f}) {
    CAPTURE(s);
    CHECK(f->hitTest(at(200.0f, 39.0f), s) == 0);    // left: frequency
    CHECK(f->hitTest(at(90.0f, 39.0f), s) == 1);     // bottom: phase
    // TMM.15: the pitch took the right side, the amplitude moved above it (the top).
    CHECK(f->hitTest(at(-20.0f, 39.0f), s) == 3);    // right: pitch
    CHECK(f->hitTest(at(20.0f, 39.0f), s) == 3);
    CHECK(f->hitTest(at(270.0f, 39.0f), s) == 2);    // top: amplitude
    CHECK(f->hitTest({60.0f, 64.0f}, s) == MetaFace::kFrame);  // the hub
    CHECK(f->hitTest(at(307.0f, 39.0f), s) == MetaFace::kFrame);  // the gap the output's lead leaves through
    CHECK(f->hitTest({-1.0f, 10.0f}, s) == MetaFace::kNone);
  }
}

// ---- goldens and the canvas -------------------------------------------------------------------------------------

TEST_CASE("faces: golden of the Phase accumulator face at zoom 0.05 / 0.25 / 1 / 3, 1x and 2x") {
  for (const float scale : {1.0f, 2.0f})
    for (const double zoom : {0.05, 0.25, 1.0, 3.0}) {
      const uint32_t size = rigSize(zoom);
      FaceRig rig(size, size, scale);
      rig.centre(zoom);
      rig.settle();
      const std::string name = "meta-phase-acc-z" + zoomName(zoom) + "-" + zoomName(scale) + "x";
      CAPTURE(name);
      CHECK(rig.canvas->stats().facesPainted == 1u);
      CHECK(rig.canvas->stats().faceLayersFromTiles == 3u);  // settled: every static layer from its tile
      checkImage(name, rig.draw(rig.res));
    }
}

TEST_CASE("faces: the same primitives at every zoom (Far, Mid, Near), and the live layer draws the voices") {
  FaceRig rig(400, 400, 1.0f);
  uint32_t prims[4] = {};
  const double zooms[4] = {0.05, 0.25, 1.0, 3.0};
  for (int k = 0; k < 4; ++k) {
    rig.centre(zooms[k]);
    rig.settle();
    (void)rig.draw(rig.res);
    CHECK(rig.canvas->stats().facesPainted == 1u);
    prims[k] = rig.canvas->stats().facePrimitives;
  }
  CHECK(rig.canvas->lod() == circuit::CircuitCanvas::Lod::Near);
  CHECK(prims[0] == prims[1]);
  CHECK(prims[1] == prims[2]);
  CHECK(prims[2] == prims[3]);
  CHECK(prims[0] > 40u);  // arcs, handles, hub, lead, jacks, 4 voices with their trails
  CHECK(rig.canvas->faceAt(0).sounding().size() == 4u);
}

TEST_CASE("faces: static layers from tiles equal direct drawing within the golden tolerance (1x, 2x; far, mid, near)") {
  for (const float scale : {1.0f, 2.0f})
    for (const double zoom : {0.25, 1.0, 3.0}) {
      const uint32_t size = rigSize(zoom);
      FaceRig rig(size, size, scale);
      rig.centre(zoom);
      rig.settle();
      const Image tiled = rig.draw(rig.res);
      CHECK(rig.canvas->stats().faceLayersFromTiles == 3u);
      const Image direct = rig.draw(rig.resNoTiles);
      CHECK(rig.canvas->stats().faceLayersDirect == 3u);
      const std::string name = "meta-phase-acc-tiles-z" + zoomName(zoom) + "-" + zoomName(scale) + "x";
      CAPTURE(name);
      CHECK(pq::ui::test::matchesGolden(name, tiled, direct));
    }
}

TEST_CASE("faces: voice dots poll at 60 Hz while live, damage only the live rect, and stop when silent") {
  TestDriver d(640, 480);
  auto& canvas = d.setRoot<circuit::CircuitCanvas>("circuit");
  SyntheticVoices voices;
  canvas.faceBinder = [&](const circuit::SceneFace&) {
    circuit::CircuitCanvas::FaceBinding b;
    b.voices = &voices;
    return b;
  };
  canvas.setScene(faceScene(), false);
  d.tree().layoutNow();
  REQUIRE(canvas.faceCount() == 1);
  const circuit::SceneNode& n = canvas.scene()->nodes[canvas.faceNode(0)];
  canvas.setView({n.x - 40.0, n.y - 40.0, 1.5});
  for (int i = 0; i < 20; ++i) d.step();
  d.advance(0.5);
  // Silent: no timer, no frames.
  CHECK_FALSE(canvas.animatingFaces());
  CHECK_FALSE(d.tree().needsFrame());
  CHECK(std::isinf(d.tree().nextWakeTime()));
  // A voice starts: the host is woken, the face timer runs; each new record damages only the orbit.
  voices.setVoices({{0, 2.0, 0.0}});
  canvas.wakeFaces();
  CHECK(canvas.animatingFaces());
  d.step();
  double t = d.now();
  const int64_t faceArea = static_cast<int64_t>(120 * 1.5) * static_cast<int64_t>(120 * 1.5);
  for (int k = 0; k < 5; ++k) {
    voices.advance(1.0 / 60.0);
    t += 1.0 / 60.0 + 1e-4;
    d.tree().update(t);
    IRect rects[Damage::kMaxRects];
    int64_t area = 0;
    const uint32_t count = d.tree().damageRects(rects, area);
    CHECK(count >= 1u);
    CHECK(area > 0);
    CHECK(area < faceArea);  // the live rect (the orbit), not the face, not the canvas
    d.step();
  }
  CHECK(canvas.faceAt(0).sounding().size() == 1u);
  CHECK(canvas.faceAt(0).sounding()[0].sampleCount >= 8u);
  // The voice ends: one more read clears the dots, then the timer stops (idle again).
  voices.setVoices({});
  for (int k = 0; k < 3; ++k) {
    t += 1.0 / 60.0 + 1e-4;
    d.tree().update(t);
    d.step();
  }
  CHECK(canvas.faceAt(0).sounding().empty());
  CHECK_FALSE(canvas.animatingFaces());
  CHECK(std::isinf(d.tree().nextWakeTime()));
}

// ---- the project patch view -------------------------------------------------------------------------------------

namespace {

// The shell over the meta faces project, the patch view shown.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  doc::DeviceId lead;

  Ui() {
    ctx.setDeviceLibrary(&library());
    REQUIRE(project_patch::buildMetaFacesProject(ctx.store()).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setShown(true, false);
    d.tree().layoutNow();
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (t.name == "Lead") lead = t.devices.at(0)->id;
    });
    REQUIRE(lead.valid());
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  project_patch::ProjectPatchView& view() const {
    project_patch::ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  double value(const char* param) const {
    const doc::Device* dv = doc::findDevice(*ctx.doc(), lead).device;
    REQUIRE(dv != nullptr);
    return doc::metaParamValue(*dv, "", phaseAcc(), paramOf(param));
  }
  const project_patch::Node& leadNode() const { return view().patch().nodes[view().patch().nodeOf(lead)]; }
  // The Lead face's centre at the canvas's centre at `zoom`.
  void frame(double zoom) {
    const project_patch::Node& n = leadNode();
    project_patch::ProjectCanvas& c = view().canvas();
    const double us = d.tree().scale();
    circuit::CircuitCanvas::View v;
    v.zoom = zoom;
    v.x = n.x + 60.0 - c.rect().w / (zoom * us) * 0.5;
    v.y = n.y + 64.0 - c.rect().h / (zoom * us) * 0.5;
    c.setView(v);
    for (int i = 0; i < 4; ++i) d.step();
  }
  // A point of the Lead face: at `deg` degrees (clockwise from +x) and radius `r` from its centre (world units).
  Vec2 facePoint(float deg, float r) const {
    const project_patch::Node& n = leadNode();
    const float a = deg * kPiF / 180.0f;
    return view().canvas().toDevice({n.x + 60.0f + r * std::cos(a), n.y + 64.0f + r * std::sin(a)});
  }
};

}  // namespace

TEST_CASE("faces: a chain Phase accumulator is drawn as its face in the project patch view") {
  Ui u;
  const project_patch::Node& n = u.leadNode();
  CHECK(n.face == &phaseAcc());
  CHECK(n.meta);
  CHECK(n.w == 120.0f);
  CHECK(n.h == 120.0f);
  REQUIRE(u.d.enableGpu().ok());  // a theme: the canvas paints
  project_patch::ProjectCanvas& c = u.view().canvas();
  CHECK(c.faceCount() == 3);  // Lead, Bell, Sub (Keys: Operator's plain block)
  const uint32_t sn = u.view().scene().sceneNode[u.view().patch().nodeOf(u.lead)];
  REQUIRE(c.faceOn(sn) != nullptr);
  CHECK(c.faceOn(sn)->target().editable());
  CHECK(c.faceOn(sn)->target().value(paramOf("amp")) == 1.0);
  // Painted at every level of detail.
  for (const double z : {0.05, 0.25, 1.0}) {
    u.frame(z);
    c.markDirty();
    REQUIRE(u.d.step());
    CHECK(c.stats().facesPainted >= 1u);
    if (z < 0.1) CHECK(c.stats().facesPainted == 3u);  // the whole project in view: every face, once
  }
}

TEST_CASE("faces: frequency, phase and amplitude drags at zoom 0.05 and 3 — one undo step each, the right values") {
  for (const double zoom : {0.05, 3.0}) {
    CAPTURE(zoom);
    Ui u;
    u.frame(zoom);
    const double us = u.d.tree().scale();
    const auto span = [&](float extent) { return std::max(extent * zoom * us, 150.0 * us); };
    // Frequency: up the left arc, 60 px up.
    size_t h0 = u.history();
    Vec2 a = u.facePoint(200.0f, 39.0f);
    u.d.drag(a, {a.x, a.y - 60.0f}, 6);
    CHECK(u.history() == h0 + 1);
    const double tf = 60.0 / span(56.0f);
    CHECK(u.value("freq") == doctest::Approx(meta::phaseAccFreqFromTravel(tf)).epsilon(1e-4));
    // Phase: along the bottom, 30 px right.
    h0 = u.history();
    a = u.facePoint(90.0f, 39.0f);
    u.d.drag(a, {a.x + 30.0f, a.y}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value("phase") == doctest::Approx(30.0 / span(50.0f)).epsilon(1e-4));
    // Amplitude (TMM.15: over the top, left to right): from 1 (travel 0.25), 30 px right.
    h0 = u.history();
    a = u.facePoint(270.0f, 39.0f);
    u.d.drag(a, {a.x + 30.0f, a.y}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value("amp") == doctest::Approx(4.0 * (0.25 + 30.0 / span(40.0f))).epsilon(1e-4));
    // Up drags it too (the top arc takes either direction).
    h0 = u.history();
    double amp0 = u.value("amp");
    u.d.drag(a, {a.x, a.y - 10.0f}, 4);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value("amp") == doctest::Approx(amp0 + 4.0 * 10.0 / span(40.0f)).epsilon(1e-4));
    // Shift: a tenth of the travel; Ctrl on the amplitude: whole numbers.
    h0 = u.history();
    amp0 = u.value("amp");
    u.d.drag(a, {a.x + 30.0f, a.y}, 6, mod::kShift);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value("amp") == doctest::Approx(amp0 + 4.0 * 3.0 / span(40.0f)).epsilon(1e-4));
    u.d.drag(a, {a.x + 2.0f, a.y}, 2, mod::kCtrl);
    CHECK(u.value("amp") == std::round(u.value("amp")));
    // Undo takes back one drag at a time.
    const double before = u.value("amp");
    REQUIRE(u.ctx.undo());
    CHECK(u.value("amp") != before);
    // Double-click on a control resets it (one step).
    h0 = u.history();
    u.d.doubleClick(u.facePoint(270.0f, 39.0f));
    CHECK(u.history() == h0 + 1);
    CHECK(u.value("amp") == 1.0);
    u.d.doubleClick(u.facePoint(200.0f, 39.0f));
    CHECK(u.value("freq") == 0.0);
    // Nothing was opened by those double-clicks (they hit controls, not the frame).
    CHECK_FALSE(u.view().deviceExpanded(u.lead));
  }
}

TEST_CASE("faces: a drag never rebuilds the view; Esc during a drag undoes it") {
  Ui u;
  u.frame(1.0);
  const uint64_t rebuilds = u.view().stats().rebuilds;
  const Vec2 a = u.facePoint(200.0f, 39.0f);
  u.d.down(a);
  u.d.move({a.x, a.y - 20.0f});
  u.d.move({a.x, a.y - 40.0f});
  CHECK(u.value("freq") > 0.0);
  CHECK(u.view().stats().rebuilds == rebuilds);  // a param write: the overlay repaints, nothing re-lowers
  const size_t h0 = u.history();
  (void)u.d.keyDown(key::kEscape);
  (void)u.d.up({a.x, a.y - 40.0f});
  CHECK(u.value("freq") == 0.0);
  CHECK(u.history() == h0);
}

TEST_CASE("faces: double-click the face's frame opens it to its atoms; its title strip closes it") {
  Ui u;
  u.frame(1.0);
  u.d.doubleClick(u.facePoint(0.0f, 0.0f));  // the hub: the frame, not a control
  u.d.step();
  REQUIRE(u.view().deviceExpanded(u.lead));
  const project_patch::Node& n = u.leadNode();
  CHECK(n.face == nullptr);  // opened: its expansion's atoms, read-only, in the labelled frame
  CHECK(n.expanded);
  CHECK(u.view().scene().sceneNode[u.view().patch().nodeOf(u.lead)] == project_patch::kNone);
  // A double-click inside the opened frame does not open an editor (the atoms are generated).
  u.d.doubleClick(u.view().canvas().toDevice({n.x + n.w * 0.5f, n.y + n.h * 0.5f}));
  u.d.step();
  CHECK(u.ctx.status().find("read-only") != std::string::npos);
  // The title strip closes it again: the face is back.
  const project_patch::Node& m = u.leadNode();
  u.d.doubleClick(u.view().canvas().toDevice({m.x + 40.0f, m.y + 8.0f}));
  u.d.step();
  CHECK_FALSE(u.view().deviceExpanded(u.lead));
  CHECK(u.leadNode().face == &phaseAcc());
}

TEST_CASE("faces: the info view describes the hovered control; cursors follow the control") {
  Ui u;
  u.frame(1.0);
  project_patch::ProjectCanvas& c = u.view().canvas();
  u.d.move(u.facePoint(200.0f, 39.0f));
  CHECK(std::string(c.info().title) == "Frequency");
  CHECK(c.cursorAt(u.facePoint(200.0f, 39.0f)) == Cursor::ResizeNS);
  u.d.move(u.facePoint(90.0f, 39.0f));
  CHECK(std::string(c.info().title) == "Phase");
  CHECK(c.cursorAt(u.facePoint(90.0f, 39.0f)) == Cursor::ResizeEW);
  u.d.move(u.facePoint(270.0f, 39.0f));
  CHECK(std::string(c.info().title) == "Amplitude");
  CHECK(c.cursorAt(u.facePoint(270.0f, 39.0f)) == Cursor::ResizeEW);
  u.d.move(u.facePoint(-20.0f, 39.0f));
  CHECK(std::string(c.info().title) == "Pitch");
  CHECK(c.cursorAt(u.facePoint(-20.0f, 39.0f)) == Cursor::ResizeNS);
}

TEST_CASE("faces: pitch drags in semitones (Ctrl: cents, Shift: fine), keys step it, typing enters it (TMM.15)") {
  for (const double zoom : {0.05, 3.0}) {
    CAPTURE(zoom);
    Ui u;
    u.frame(zoom);
    const double us = u.d.tree().scale();
    const double span = std::max(46.0 * zoom * us, 300.0 * us);  // device px for the whole ±4800-cent range
    // TMM.13-fix2 (MM review L21): the arc's middle half holds +-1200 cents (these drags stay inside it).
    const auto travelCents = [&](double px) { return px / span * 4800.0; };
    // A plain drag up: whole semitones.
    size_t h0 = u.history();
    const Vec2 a = u.facePoint(0.0f, 39.0f);
    u.d.drag(a, {a.x, a.y - 40.0f}, 8);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value("pitch") == 100.0 * std::round(travelCents(40.0) / 100.0));
    CHECK(u.value("pitch") > 0.0);
    // Ctrl: single cents (an offset from the semitone grid).
    u.d.doubleClick(a);
    REQUIRE(u.value("pitch") == 0.0);
    h0 = u.history();
    u.d.drag(a, {a.x, a.y - 3.0f}, 3, mod::kCtrl);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value("pitch") == std::round(travelCents(3.0)));
    // Then a plain drag down keeps that offset, moving in whole semitones.
    const double offset = u.value("pitch");
    u.d.drag(a, {a.x, a.y + 40.0f}, 8);
    CHECK(std::fmod(u.value("pitch") - offset, 100.0) == 0.0);
    CHECK(u.value("pitch") < offset);
    // Shift: a tenth of the travel.
    u.d.doubleClick(a);
    u.d.drag(a, {a.x, a.y - 60.0f}, 8, mod::kShift);
    CHECK(u.value("pitch") == 100.0 * std::round(travelCents(6.0) / 100.0));
    // Keys (after a press on the pitch arc): Up / Down a semitone, Shift an octave, Ctrl a cent; each one undo step.
    u.d.doubleClick(a);
    REQUIRE(u.value("pitch") == 0.0);
    h0 = u.history();
    CHECK(u.d.keyDown(key::kUp) == KeyResult::Widget);
    CHECK(u.value("pitch") == 100.0);
    CHECK(u.d.keyDown(key::kUp, mod::kShift) == KeyResult::Widget);
    CHECK(u.value("pitch") == 1300.0);
    CHECK(u.d.keyDown(key::kDown, mod::kCtrl) == KeyResult::Widget);
    CHECK(u.value("pitch") == 1299.0);
    CHECK(u.d.keyDown(key::kDown, mod::kShift) == KeyResult::Widget);
    CHECK(u.d.keyDown(key::kDown, mod::kShift) == KeyResult::Widget);
    CHECK(u.value("pitch") == -1101.0);
    CHECK(u.history() == h0 + 5);
    REQUIRE(u.ctx.undo());
    CHECK(u.value("pitch") == 99.0);
    // Clamped at ±4800 (4 octaves).
    for (int i = 0; i < 6; ++i) (void)u.d.keyDown(key::kUp, mod::kShift);
    CHECK(u.value("pitch") == 4800.0);
    // Typing: cents, or semitones with an `st` suffix; Enter commits (one step), Esc cancels.
    h0 = u.history();
    for (const Key k : {Key('-'), Key('7'), Key('0'), Key('2'), Key('.'), Key('5')}) CHECK(u.d.keyDown(k) == KeyResult::Widget);
    CHECK(u.value("pitch") == 4800.0);  // nothing until Enter
    CHECK(u.d.keyDown(key::kReturn) == KeyResult::Widget);
    CHECK(u.value("pitch") == -702.5);
    CHECK(u.history() == h0 + 1);
    for (const Key k : {Key('1'), Key('2'), Key('s'), Key('t')}) CHECK(u.d.keyDown(k) == KeyResult::Widget);
    CHECK(u.d.keyDown(key::kReturn) == KeyResult::Widget);
    CHECK(u.value("pitch") == 1200.0);
    for (const Key k : {Key('5'), Key('0')}) CHECK(u.d.keyDown(k) == KeyResult::Widget);
    CHECK(u.d.keyDown(key::kEscape) == KeyResult::Widget);
    CHECK(u.value("pitch") == 1200.0);
    // Arrow keys after a press on another control do nothing to the pitch.
    u.d.click(u.facePoint(200.0f, 39.0f));
    const double before = u.value("pitch");
    (void)u.d.keyDown(key::kUp, mod::kShift);
    CHECK(u.value("pitch") == before);
  }
}

TEST_CASE("faces: the pitch readout is semitones + cents") {
  // The readout's text leads the info view's description (the same formatter).
  std::unique_ptr<MetaFace> f = makeFace(phaseAcc());
  auto fixed = std::make_unique<FixedTarget>();
  FixedTarget* ft = fixed.get();
  f->bind(std::move(fixed), nullptr);
  const std::string minus = "\xE2\x88\x92", cent = "\xC2\xA2";
  const std::pair<double, std::string> cases[] = {
      {0.0, "0 st"},
      {700.0, "+7 st"},
      {-1200.0, minus + "12 st"},
      {-1186.0, minus + "12 st +14 " + cent},
      {714.0, "+7 st +14 " + cent},
      {3.5, "+3.5 " + cent},
      {-49.0, minus + "49 " + cent},
      {750.0, "+8 st " + minus + "50 " + cent},
      {701.96, "+7 st +1.96 " + cent},
  };
  for (const auto& [cents, text] : cases) {
    CAPTURE(cents);
    ft->v[3] = cents;
    std::string title, body;
    f->describe(3, title, body);
    CHECK(title == "Pitch");
    CHECK(body.starts_with(text + " ("));
  }
  ft->v[3] = 700.0;
  std::string title, body;
  f->describe(3, title, body);
  CHECK(body.find("+700.00 cents") != std::string::npos);
  CHECK(body.find("x1.49831") != std::string::npos);
}

TEST_CASE("faces: the Device-level editor shows a typed meta call as a face that edits the device's state") {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  ctx.setDeviceLibrary(&library());
  doc::DocumentStore& st = ctx.store();
  auto e = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Synth");
  REQUIRE(e.ok());
  REQUIRE(st.dispatch(e->command).ok());
  const doc::DeviceId inst = doc::findTrackPtr(*st.current(), e->tracks[0])->devices.at(0)->id;
  auto shell = Shell::create(d.tree(), ctx);
  REQUIRE(shell.ok());
  d.tree().layoutNow();
  circuit::CircuitEditor* ed = circuit::openDeviceInCircuitView(*d.tree().root(), ctx, inst);
  REQUIRE(ed != nullptr);
  d.step();
  // Type the call into the text pane (two-way: the text is the device's).
  const std::string from = "ph  = phasor(voice.hz)";
  const std::string to = "acc1 = meta_phase_acc()\n  ph  = acc1";
  const auto at = static_cast<uint32_t>(ed->document().text().find(from));
  REQUIRE(at != static_cast<uint32_t>(std::string::npos));
  ed->textPane().replace(at, static_cast<uint32_t>(from.size()), to);
  ed->textPane().onEdit(at, static_cast<uint32_t>(from.size()), to, true);
  d.step();
  const doc::Device* dv = doc::findDevice(*ctx.doc(), inst).device;
  REQUIRE(dv != nullptr);
  REQUIRE(dv->edited());
  CHECK(dv->source->text.find("acc1 = meta_phase_acc()") != std::string::npos);
  CHECK(ed->document().text().find("acc1 = meta_phase_acc()") != std::string::npos);
  // The canvas draws the call as a face bound to the device's state at path "acc1".
  circuit::EditCanvas& c = ed->canvas();
  REQUIRE(c.faceCount() == 1);
  CHECK(c.faceAt(0).target().editable());
  // A frequency drag on it: one step, the nested instance's param (no text change: state lives in the document).
  const circuit::SceneNode& n = c.scene()->nodes[c.faceNode(0)];
  circuit::CircuitCanvas::View v;
  v.zoom = 1.0;
  v.x = n.x + 60.0 - c.rect().w * 0.5;
  v.y = n.y + 64.0 - c.rect().h * 0.5;
  c.setView(v);
  d.step();
  const std::string text = dv->source->text;
  const size_t h0 = st.history().cursor();
  const float ang = 200.0f * kPiF / 180.0f;
  const Vec2 a = c.toDevice({n.x + 60.0f + 39.0f * std::cos(ang), n.y + 64.0f + 39.0f * std::sin(ang)});
  d.drag(a, {a.x, a.y - 50.0f}, 5);
  CHECK(st.history().cursor() == h0 + 1);
  dv = doc::findDevice(*ctx.doc(), inst).device;
  REQUIRE(dv->metaAt("acc1") != nullptr);
  CHECK(doc::metaParamValue(*dv, "acc1", phaseAcc(), paramOf("freq")) > 0.0);
  CHECK(dv->source->text == text);
  d.tree().setRoot(std::make_unique<Widget>());
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

// TMM.17: the live layer's cost with 8 voices across the octaves (each with ~96 sampled phases: one 60 Hz frame of
// reads), one face and 16 faces per frame (each voice coloured and sized by its frequency).
TEST_CASE("faces: the Phase accumulator live layer's paint cost (2x, 8 voices across octaves) and 16 faces (TMM.17)" *
          doctest::test_suite("perf")) {  // wall-clock bounds: load-sensitive
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  std::unique_ptr<MetaFace> face = makeFace(phaseAcc());
  REQUIRE(face != nullptr);
  SyntheticVoices voices;
  std::vector<SyntheticVoices::Voice> vs;
  for (uint32_t v = 0; v < 8; ++v) {
    const double hz = 27.5 * std::exp2(static_cast<double>(v));
    vs.push_back({v, hz, 0.125 * v, 0.0, hz / 48000.0});
  }
  voices.setVoices(vs);
  voices.setRate(48000.0);
  face->bind(std::make_unique<FixedTarget>(), &voices);
  FaceJack jacks[3];
  jacks[0] = {{0.0f, 24.0f}, true, true, true, "hz"};
  jacks[1] = {{0.0f, 40.0f}, true, false, true, "pm"};
  jacks[2] = {{120.0f, 24.0f}, false, true, true, "out"};
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
  std::vector<double> one, sixteen;
  for (int i = 0; i < 220; ++i) {
    voices.advance(1.0 / 60.0);
    (void)face->pollVoices();
    auto t0 = std::chrono::steady_clock::now();
    paintOnce(1);
    one.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    t0 = std::chrono::steady_clock::now();
    paintOnce(16);
    sixteen.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(one.begin(), one.end());
  std::sort(sixteen.begin(), sixteen.end());
  MESSAGE("phase_acc live layer, 1 face (2x, 8 voices): median " << one[110] << " ms; 16 faces: median " << sixteen[110] << " ms");
  CHECK(face->sounding().size() == 8u);
#if PQ_OPTIMIZED
  CHECK(one[110] < 0.1);
  CHECK(sixteen[110] < 1.0);
#endif
}

// ---- TMM.17: voices by frequency ------------------------------------------------------------------------------------

TEST_CASE("faces (TMM.17): the voice colour map — red below 100 Hz, purple 100–300, blue, yellow from 1 kHz; smooth") {
  auto ch = [](Rgba c) { return std::array<int, 3>{static_cast<int>(redOf(c)), static_cast<int>(greenOf(c)), static_cast<int>(blueOf(c))}; };
  for (const double hz : {-5.0, 0.0, 10.0, 30.0, 60.0, 90.0}) {
    CAPTURE(hz);
    const auto c = ch(phaseAccVoiceColour(hz));
    CHECK(c[0] >= 220);  // red
    CHECK(c[1] < 80);
    CHECK(c[2] < 110);
  }
  for (const double hz : {150.0, 180.0, 220.0}) {  // purple: red and blue, little green
    CAPTURE(hz);
    const auto c = ch(phaseAccVoiceColour(hz));
    CHECK(c[0] > 130);
    CHECK(c[2] > 180);
    CHECK(c[1] < 100);
  }
  {
    const auto c = ch(phaseAccVoiceColour(440.0));  // blue
    CHECK(c[2] > 230);
    CHECK(c[0] < 100);
    CHECK(c[1] < 140);
  }
  for (const double hz : {1050.0, 2000.0, 8000.0, 20000.0}) {  // yellow
    CAPTURE(hz);
    const auto c = ch(phaseAccVoiceColour(hz));
    CHECK(c[0] > 240);
    CHECK(c[1] > 200);
    CHECK(c[2] < 80);
  }
  CHECK(alphaOf(phaseAccVoiceColour(440.0)) == 255u);
  // Continuous: 1/48-octave steps from 10 Hz to 20 kHz change no channel by more than 24 (no jumps between stops).
  int worst = 0;
  Rgba prev = phaseAccVoiceColour(10.0);
  for (double l = std::log2(10.0); l <= std::log2(20000.0); l += 1.0 / 48.0) {
    const Rgba c = phaseAccVoiceColour(std::exp2(l));
    const auto a = ch(prev), b = ch(c);
    for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(a[k] - b[k]));
    prev = c;
  }
  CHECK(worst <= 24);
}

TEST_CASE("faces (TMM.17): the orbit radius — 1.25x at 20 Hz and below, 0.25x at 10 kHz and above, log-linear") {
  CHECK(phaseAccOrbitScale(-1.0) == 1.25f);
  CHECK(phaseAccOrbitScale(0.0) == 1.25f);
  CHECK(phaseAccOrbitScale(10.0) == 1.25f);
  CHECK(phaseAccOrbitScale(20.0) == 1.25f);
  CHECK(phaseAccOrbitScale(10000.0) == doctest::Approx(0.25f));
  CHECK(phaseAccOrbitScale(22000.0) == 0.25f);
  CHECK(phaseAccOrbitScale(std::sqrt(20.0 * 10000.0)) == doctest::Approx(0.75f).epsilon(1e-5));  // the log middle
  float prev = 2.0f;
  for (double hz = 20.0; hz <= 10000.0; hz *= 1.05) {
    const float s = phaseAccOrbitScale(hz);
    CHECK(s < prev);
    prev = s;
  }
}

TEST_CASE("faces (TMM.17): a voice's frequency is its increment times the rate (wraps either way); unknown rate: own") {
  FaceVoice v;
  v.rate = 48000.0f;
  v.increment = static_cast<float>(440.0 / 48000.0);
  CHECK(faceVoiceHz(v, -1.0) == doctest::Approx(440.0).epsilon(1e-5));
  v.increment = static_cast<float>(440.0 / 48000.0 - 1.0);  // the phase wrapped between the two samples
  CHECK(faceVoiceHz(v, -1.0) == doctest::Approx(440.0).epsilon(1e-4));
  v.increment = static_cast<float>(-440.0 / 48000.0);  // running backwards (FM below 0 Hz): the same pitch
  CHECK(faceVoiceHz(v, -1.0) == doctest::Approx(440.0).epsilon(1e-5));
  v.increment = 0.0f;
  CHECK(faceVoiceHz(v, -1.0) == 0.0);
  v.rate = 0.0f;  // unknown: the caller's own frequency
  v.increment = 0.25f;
  CHECK(faceVoiceHz(v, 123.0) == 123.0);
}

namespace {
// The dot of one voice in a face painted at the origin, scale 1: the mean position of the vertices in exactly `col`
// (the dot's opaque disc), or nullopt.
std::optional<Vec2> dotOf(const DrawList& dl, Rgba col) {
  double x = 0.0, y = 0.0;
  uint32_t n = 0;
  for (const Vertex& v : dl.vertices())
    if (v.col == col) {
      x += v.pos.x;
      y += v.pos.y;
      ++n;
    }
  if (n == 0) return std::nullopt;
  return Vec2{static_cast<float>(x / n), static_cast<float>(y / n)};
}
}  // namespace

TEST_CASE("faces (TMM.17): each voice is drawn in its frequency's colour on its frequency's orbit; unknown rate: own") {
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  std::unique_ptr<MetaFace> face = makeFace(phaseAcc());
  REQUIRE(face != nullptr);
  SyntheticVoices voices;
  face->bind(std::make_unique<FixedTarget>(), &voices);  // 440 Hz, pitch +7 st: its own frequency 659.26 Hz
  FaceEnv env;
  env.theme = &*theme;
  DrawList dl;
  const Vec2 centre{60.0f, 64.0f};
  constexpr float kOrbit = 23.0f;
  auto paint = [&]() {
    (void)face->pollVoices();
    dl.reset(400, 400);
    ui::FacePainter p(dl, {{0.0f, 0.0f}, 1.0f});
    face->paintLive(p, env);
  };
  // A 55 Hz voice at phase 0 (the top) and a 3520 Hz voice at phase 0.25 (3 o'clock).
  voices.setRate(48000.0);
  voices.setVoices({{0, 0.0, 0.0, 0.0, 55.0 / 48000.0}, {1, 0.0, 0.25, 0.0, 3520.0 / 48000.0}});
  paint();
  REQUIRE(face->sounding().size() == 2u);
  const auto low = dotOf(dl, phaseAccVoiceColour(55.0));
  const auto high = dotOf(dl, phaseAccVoiceColour(3520.0));
  REQUIRE(low.has_value());
  REQUIRE(high.has_value());
  CHECK(low->x == doctest::Approx(centre.x).epsilon(1e-3));
  CHECK(centre.y - low->y == doctest::Approx(kOrbit * phaseAccOrbitScale(55.0)).epsilon(1e-3));
  CHECK(high->x - centre.x == doctest::Approx(kOrbit * phaseAccOrbitScale(3520.0)).epsilon(1e-3));
  CHECK(high->y == doctest::Approx(centre.y).epsilon(1e-3));
  CHECK(centre.y - low->y > high->x - centre.x);  // the low voice circles wider
  // No rate (an unknown engine rate): the accumulator's own frequency, bar × pitch.
  voices.setRate(0.0);
  voices.setVoices({{0, 0.0, 0.0, 0.0, 0.0}});
  paint();
  const double own = 440.0 * std::exp2(7.0 / 12.0);
  const auto mine = dotOf(dl, phaseAccVoiceColour(own));
  REQUIRE(mine.has_value());
  CHECK(centre.y - mine->y == doctest::Approx(kOrbit * phaseAccOrbitScale(own)).epsilon(1e-3));
}

TEST_CASE("faces (TMM.17): golden of a chord across seven octaves (each voice its colour and orbit), 1x and 2x") {
  for (const float scale : {1.0f, 2.0f}) {
    const double zoom = 3.0;
    const uint32_t size = rigSize(zoom);
    FaceRig rig(size, size, scale);
    std::vector<SyntheticVoices::Voice> chord;
    const double hz[] = {32.7, 98.0, 164.8, 261.6, 392.0, 659.3, 1318.5, 4186.0};  // C1 … C8
    for (uint32_t v = 0; v < 8; ++v) chord.push_back({v, hz[v], 0.07 + 0.125 * v, 0.0, hz[v] / 48000.0});
    rig.voices.setVoices(chord);
    rig.voices.setRate(48000.0);
    rig.voices.advance(1.0 / 60.0);
    rig.voices.freeze();
    rig.canvas->wakeFaces();
    rig.centre(zoom);
    rig.settle();
    REQUIRE(rig.canvas->faceAt(0).sounding().size() == 8u);
    const std::string name = "meta-phase-acc-chord-z3-" + zoomName(scale) + "x";
    CAPTURE(name);
    checkImage(name, rig.draw(rig.res));
  }
}
