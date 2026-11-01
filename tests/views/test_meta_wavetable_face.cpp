// TMM.7: the Wavetable face — its display model (dense frames by the builder's rules at display resolution, the bake
// through the shared formulas, keyframe harmonics, the source cycle a new keyframe takes), goldens at zoom 0.1 / 0.5 /
// 1 / 2 and 1x / 2x (and with eight synthetic voices spread over the frames), the same primitives at every zoom, tiles
// equal to direct drawing, voice fades, taps → dots within one displayed frame, every gesture through the project patch
// view as one undo step (position, phase, knobs, warp mode, keyframe select / move / snap / add / delete, gap mode,
// basic shapes, the refused Shape mode), and the live layer's paint cost.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../../src/views/src/meta/wavetable_model.h"
#include "../ui/golden_check.h"
#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/dsp/wave_warp.h"
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

const meta::Type& wtType() { return meta::wavetableType(); }

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

std::vector<float> basic(dsp::BasicCycle c) {
  const AlignedBuffer<float> b = dsp::basicCycle(c);
  return {b.span().begin(), b.span().end()};
}

// Fixed state (the goldens): phase 0.3, position 0.4, warp 0.25 (bend), the demo table.
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

// A patch whose one meta call is a Wavetable: its phase input wired (and its position input when `posWired`).
std::shared_ptr<const circuit::CircuitScene> wtScene(bool posWired) {
  const std::string text = std::string("patch WtDemo {\n  in    ph  : audio\n") + (posWired ? "  in    pm  : audio\n" : "") +
                           "  out   y   : audio\n  wt1 = meta_wavetable(ph" + (posWired ? ", pm" : "") + ")\n  y = wt1\n}\n";
  auto doc = circuit::loadPatchDocument((fs::temp_directory_path() / "pq_meta_wt_face_test.pqp").string(), text);
  REQUIRE_MESSAGE(doc.ok(), (doc.ok() ? "" : doc.error().message));
  const graph::Patch* p = (*doc)->find("WtDemo");
  REQUIRE(p != nullptr);
  return circuit::buildScene(*p);
}

// Eight voices: phases spread, each at its own position (the pos input modulated) when `spread`.
std::vector<SyntheticVoices::Voice> eightVoices(bool spread) {
  std::vector<SyntheticVoices::Voice> v;
  for (uint32_t k = 0; k < 8; ++k)
    v.push_back({k, 3.0 + 1.3 * k, 0.06 + 0.117 * k, spread ? 0.08 + 0.12 * k : 0.4});
  return v;
}

struct WtRig {
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
  WtTarget* bound = nullptr;

  WtRig(uint32_t lw, uint32_t lh, float sc, bool withVoices, bool posWired = false) : scale(sc) {
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
    if (withVoices) {
      voices.setVoices(eightVoices(posWired));
      voices.advance(1.0 / 60.0);
      voices.freeze();
    }
    canvas->faceBinder = [this, withVoices](const circuit::SceneFace&) {
      circuit::CircuitCanvas::FaceBinding b;
      auto t = std::make_unique<WtTarget>();
      bound = t.get();
      b.target = std::move(t);
      b.voices = withVoices ? &voices : nullptr;
      return b;
    };
    canvas->setScene(wtScene(posWired), false);
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
    v.x = n.x + 102.0 - lw / zoom * 0.5;
    v.y = n.y + 124.0 - lh / zoom * 0.5;
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

uint32_t rigW(double zoom) { return static_cast<uint32_t>(std::max(96.0, std::ceil(204.0 * zoom + 48.0))); }
uint32_t rigH(double zoom) { return static_cast<uint32_t>(std::max(96.0, std::ceil(248.0 * zoom + 48.0))); }

}  // namespace

// ---- the display model ----------------------------------------------------------------------------------------------

TEST_CASE("wavetable face model: dense frames follow the builder's rules; the bake uses the shared formulas") {
  meta::WavetableData d;
  d.keyframes.push_back({0.0f, basic(dsp::BasicCycle::Sine)});
  d.keyframes.push_back({1.0f, basic(dsp::BasicCycle::Square)});
  d.frames = 5;
  wt::Model m;
  REQUIRE(m.update(meta::wavetableData(d), {}));
  REQUIRE(m.ok());
  CHECK(m.frames() == 5u);
  // Frame 0 is the sine (display-smoothed: within 1 % of the exact value), sampled at kPoints phases.
  const auto f0 = m.frame(0);
  REQUIRE(f0.size() == wt::kPoints + 1);
  for (uint32_t i = 0; i < wt::kPoints; i += 16)
    CHECK(f0[i] == doctest::Approx(std::sin(2.0 * std::numbers::pi * i / wt::kPoints)).epsilon(0.01).scale(1.0));
  CHECK(f0[wt::kPoints] == f0[0]);
  CHECK(m.shapeOf(0) == static_cast<int32_t>(dsp::BasicCycle::Sine));
  CHECK(m.shapeOf(1) == static_cast<int32_t>(dsp::BasicCycle::Square));
  // Spectral morph: the middle frame's fundamental magnitude is the average of the two keyframes'.
  float amp0[wt::kBars], amp1[wt::kBars], ph[wt::kBars];
  m.harmonics(0, amp0, ph);
  CHECK(amp0[0] == doctest::Approx(1.0));
  CHECK(amp0[2] < 1e-4f);
  CHECK(ph[0] == doctest::Approx(0.0).scale(1.0));  // a sine sits at phase 0
  m.harmonics(1, amp1, ph);
  CHECK(amp1[2] == doctest::Approx(1.0 / 3.0).epsilon(0.02));  // a square: odd harmonics, 1/h
  CHECK(amp1[1] < 1e-3f);
  // Same pointer: nothing to do.
  CHECK_FALSE(m.update(m.dataPtr(), {}));
  // The bake: fold through dsp::foldValue at every displayed phase of the sine frame (one keyframe).
  meta::WavetableData one;
  one.keyframes.push_back({0.0f, basic(dsp::BasicCycle::Sine)});
  wt::Model f;
  wt::Bake b;
  b.fold = 0.4f;
  REQUIRE(f.update(meta::wavetableData(one), b));
  CHECK(f.frames() == 1u);
  wt::Model plain;
  REQUIRE(plain.update(meta::wavetableData(one), {}));
  for (uint32_t i = 0; i < wt::kPoints; i += 8)
    CHECK(f.frame(0)[i] == doctest::Approx(dsp::foldValue(static_cast<double>(plain.frame(0)[i]), 0.4)).epsilon(1e-5).scale(1.0));
  // A louder-than-full-scale bake is drawn to fit.
  CHECK(f.gain() <= 1.0f);
}

TEST_CASE("wavetable face model: the source cycle at a keyframe is that keyframe; between, the reader's frame blend") {
  meta::WavetableData d;
  d.keyframes.push_back({0.0f, basic(dsp::BasicCycle::Saw)});
  d.keyframes.push_back({1.0f, basic(dsp::BasicCycle::Sine)});
  d.gaps = {dsp::GapMode::Crossfade};
  d.frames = 3;
  wt::Model m;
  REQUIRE(m.update(meta::wavetableData(d), {}));
  // At the last keyframe: the sine (band-limited reconstruction: exact for a sine).
  const std::vector<float> c1 = m.sourceCycle(1.0f);
  const std::vector<float> s = basic(dsp::BasicCycle::Sine);
  REQUIRE(c1.size() == s.size());
  double worst = 0.0;
  for (size_t i = 0; i < s.size(); ++i) worst = std::max(worst, std::fabs(double(c1[i]) - s[i]));
  CHECK(worst < 1e-5);
  // Crossfade gap, position 0.5 = dense frame 1 exactly: the average of the saw (band-limited) and the sine.
  const std::vector<float> c = m.sourceCycle(0.5f);
  const std::vector<float> c0 = m.sourceCycle(0.0f);
  for (size_t i = 0; i < s.size(); i += 64) CHECK(c[i] == doctest::Approx(0.5 * (c0[i] + c1[i])).epsilon(1e-4).scale(1.0));
}

TEST_CASE("wavetable face model: a data change and a baked knob step cost little (UI thread)") {
  wt::Model m;
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(m.update(project_patch::demoWavetableData(), {}));
  const auto t1 = std::chrono::steady_clock::now();
  double bakeMs = 0.0;
  for (int i = 1; i <= 10; ++i) {
    wt::Bake b;
    b.fold = 0.05f * static_cast<float>(i);
    const auto a = std::chrono::steady_clock::now();
    REQUIRE(m.update(m.dataPtr(), b));
    bakeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
  }
  const double dataMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  MESSAGE("model: data change (3 keyframes, 64 frames) " << dataMs << " ms; baked fold step " << bakeMs / 10.0 << " ms");
#if PQ_OPTIMIZED
  CHECK(dataMs < 20.0);
  CHECK(bakeMs / 10.0 < 5.0);
#endif
}

// ---- goldens and the canvas -------------------------------------------------------------------------------------

TEST_CASE("wavetable face: golden at zoom 0.1 / 0.5 / 1 / 2, 1x and 2x") {
  for (const float scale : {1.0f, 2.0f})
    for (const double zoom : {0.1, 0.5, 1.0, 2.0}) {
      WtRig rig(rigW(zoom), rigH(zoom), scale, false);
      rig.centre(zoom);
      rig.settle();
      const std::string name = "meta-wavetable-z" + zoomName(zoom) + "-" + zoomName(scale) + "x";
      CAPTURE(name);
      CHECK(rig.canvas->stats().facesPainted == 1u);
      CHECK(rig.canvas->stats().faceLayersFromTiles == 4u);  // settled: body, tracks, frame and the stacked frames
      checkImage(name, rig.draw(rig.res));
    }
}

TEST_CASE("wavetable face: golden with eight voices, each on its own frame (the pos input modulated), zoom 2") {
  for (const float scale : {1.0f, 2.0f}) {
    WtRig rig(rigW(2.0), rigH(2.0), scale, true, true);
    rig.centre(2.0);
    rig.settle();
    REQUIRE(rig.canvas->faceAt(0).sounding().size() == 8u);
    const std::string name = "meta-wavetable-poly-z2-" + zoomName(scale) + "x";
    checkImage(name, rig.draw(rig.res));
  }
}

TEST_CASE("wavetable face: the same primitives at every zoom; tiles equal direct drawing") {
  WtRig rig(500, 560, 1.0f, true, true);
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
  CHECK(prims[0] > 300u);  // cells, bars, sliders, markers, knobs, the frame, 8 voices with their reads
  for (const float scale : {1.0f, 2.0f})
    for (const double zoom : {0.5, 1.0, 2.0}) {
      WtRig r(rigW(zoom), rigH(zoom), scale, false);
      r.centre(zoom);
      r.settle();
      const Image tiled = r.draw(r.res);
      CHECK(r.canvas->stats().faceLayersFromTiles == 4u);
      const Image direct = r.draw(r.resNoTiles);
      CHECK(r.canvas->stats().faceLayersDirect == 4u);
      const std::string name = "meta-wavetable-tiles-z" + zoomName(zoom) + "-" + zoomName(scale) + "x";
      CAPTURE(name);
      CHECK(pq::ui::test::matchesGolden(name, tiled, direct));
    }
}

namespace {
double gClock = 100.0;
double testClock() { return gClock; }
}  // namespace

TEST_CASE("wavetable face: taps reach the dots within one displayed frame; voices fade in and out over 80 ms") {
  MetaFace::setClock(&testClock);
  TestDriver d(800, 700);
  auto& canvas = d.setRoot<circuit::CircuitCanvas>("circuit");
  SyntheticVoices voices;
  canvas.faceBinder = [&](const circuit::SceneFace&) {
    circuit::CircuitCanvas::FaceBinding b;
    b.target = std::make_unique<WtTarget>();
    b.voices = &voices;
    return b;
  };
  canvas.setScene(wtScene(false), false);
  d.tree().layoutNow();
  REQUIRE(canvas.faceCount() == 1);
  const circuit::SceneNode& n = canvas.scene()->nodes[canvas.faceNode(0)];
  canvas.setView({n.x - 20.0, n.y - 20.0, 1.5});
  for (int i = 0; i < 10; ++i) d.step();
  MetaFace& f = canvas.faceAt(0);
  CHECK_FALSE(canvas.animatingFaces());
  // A voice starts: the next timer tick (one displayed frame) shows its newest phase.
  voices.setVoices({{3, 2.0, 0.25, 0.4}});
  canvas.wakeFaces();
  double t = d.now();
  voices.advance(1.0 / 60.0);
  const float expect = static_cast<float>(voices.voicesNow()[0].phase);
  t += 1.0 / 60.0 + 1e-4;
  gClock += 1.0 / 60.0;
  d.tree().update(t);
  REQUIRE(f.sounding().size() == 1u);
  CHECK(f.sounding()[0].phase == doctest::Approx(expect));
  CHECK(f.sounding()[0].position == doctest::Approx(0.4));
  CHECK(f.animating());
  // Silent: it keeps animating (the fade) for 80 ms, then the timer stops.
  voices.setVoices({});
  for (int k = 0; k < 4; ++k) {
    t += 1.0 / 60.0 + 1e-4;
    gClock += 1.0 / 60.0;
    d.tree().update(t);
    d.step();
  }
  CHECK(f.sounding().empty());
  CHECK(f.animating());  // ≈ 67 ms into the fade
  for (int k = 0; k < 4; ++k) {
    t += 1.0 / 60.0 + 1e-4;
    gClock += 1.0 / 60.0;
    d.tree().update(t);
    d.step();
  }
  CHECK_FALSE(f.animating());
  CHECK_FALSE(canvas.animatingFaces());
  MetaFace::setClock(nullptr);
}

// ---- gestures through the project patch view -------------------------------------------------------------------

namespace {

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
  double setting(const char* name) const {
    return doc::metaSettings(dev(), "", wtType())[static_cast<size_t>(wtType().settingIndex(name))];
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
  // A face-local point (world units) in device px.
  Vec2 at(float x, float y) const {
    const project_patch::Node& n = node();
    return view().canvas().toDevice({n.x + x, n.y + y});
  }
};

float posY(float u) { return 172.0f - 94.0f * u; }

}  // namespace

TEST_CASE("wavetable face: a chain Wavetable is its face in the project patch view, bound to its state") {
  WtUi u;
  CHECK(u.node().face == &wtType());
  CHECK(u.node().w == 204.0f);
  REQUIRE(u.d.enableGpu().ok());
  project_patch::ProjectCanvas& c = u.view().canvas();
  const uint32_t sn = u.view().scene().sceneNode[u.view().patch().nodeOf(u.wave)];
  MetaFace* f = c.faceOn(sn);
  REQUIRE(f != nullptr);
  CHECK(f->target().editable());
  CHECK(f->target().value(meta::kWtPosition) == doctest::Approx(0.35));
  CHECK(f->target().data() == u.dev().metaAt("")->data);
  // The Pair instrument opened in place: the Phase accumulator and the Wavetable it drives, both faces, the
  // Wavetable's bound to the nested instance "wt1".
  u.view().setDeviceExpanded(u.pair, true);
  for (int i = 0; i < 4; ++i) u.d.step();
  REQUIRE(u.view().deviceExpanded(u.pair));
  {
    const doc::Device* pd = doc::findDevice(*u.ctx.doc(), u.pair).device;
    REQUIRE(pd != nullptr);
    CHECK(pd->edited());
    REQUIRE(library().resolve(*pd) != nullptr);
    CHECK(library().resolve(*pd)->error.empty());
  }
  size_t wts = 0, accs = 0, nested = 0;
  for (size_t i = 0; i < c.faceCount(); ++i) {
    if (&c.faceAt(i).type() == &wtType()) {
      ++wts;
      if (c.faceAt(i).target().value(meta::kWtPosition) == doctest::Approx(0.7) && c.faceAt(i).target().editable()) ++nested;
    }
    if (c.faceAt(i).type().id == std::string_view("phase_acc")) ++accs;
  }
  CHECK(wts == 2u);
  CHECK(accs == 1u);
  CHECK(nested == 1u);
}

TEST_CASE("wavetable face: position, phase and knob drags at zoom 0.1 and 1.5 — one undo step each, no rebuild") {
  for (const double zoom : {0.1, 1.5}) {
    CAPTURE(zoom);
    WtUi u;
    u.frame(zoom);
    const double us = u.d.tree().scale();
    const auto span = [&](float extent) { return std::max(extent * zoom * us, 150.0 * us); };
    const uint64_t rebuilds = u.view().stats().rebuilds;
    // Position: up the track (between the markers), 40 px.
    size_t h0 = u.history();
    Vec2 a = u.at(178.0f, posY(0.2f));
    u.d.drag(a, {a.x, a.y - 40.0f}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value("position") == doctest::Approx(0.35 + 40.0 / span(94.0f)).epsilon(1e-4));
    // Phase: right along its slider, 30 px.
    h0 = u.history();
    a = u.at(60.0f, 182.0f);
    u.d.drag(a, {a.x + 30.0f, a.y}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value("phase") == doctest::Approx(30.0 / span(146.0f)).epsilon(1e-4));
    // Fold: up, 30 px; warp: down from 0.3, 20 px (bipolar); sync with Shift (a tenth).
    h0 = u.history();
    a = u.at(38.0f, 212.0f);
    u.d.drag(a, {a.x, a.y - 30.0f}, 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.value("fold") == doctest::Approx(30.0 / span(60.0f)).epsilon(1e-4));
    a = u.at(89.0f, 212.0f);
    u.d.drag(a, {a.x, a.y + 20.0f}, 6);
    CHECK(u.value("warp") == doctest::Approx(0.3 - 2.0 * 20.0 / span(60.0f)).epsilon(1e-4));
    a = u.at(140.0f, 212.0f);
    u.d.drag(a, {a.x, a.y - 30.0f}, 6, mod::kShift);
    CHECK(u.value("sync") == doctest::Approx(3.0 / span(60.0f)).epsilon(1e-4));
    CHECK(u.view().stats().rebuilds == rebuilds);  // param writes: nothing re-lowers
    // Double-click a knob: reset (one step).
    h0 = u.history();
    u.d.doubleClick(u.at(38.0f, 212.0f));
    CHECK(u.history() == h0 + 1);
    CHECK(u.value("fold") == 0.0);
    // Undo takes back the reset.
    REQUIRE(u.ctx.undo());
    CHECK(u.value("fold") > 0.0);
  }
}

TEST_CASE("wavetable face: warp mode, keyframe select / move / snap / add / delete, gap mode, basic shapes — one step each") {
  WtUi u;
  u.frame(1.5);
  const double us = u.d.tree().scale();
  const double span = std::max(94.0 * 1.5 * us, 150.0 * us);
  // The warp mode switch: Mirror.
  size_t h0 = u.history();
  u.d.click(u.at(89.0f, 233.0f));
  CHECK(u.history() == h0 + 1);
  CHECK(u.setting("warpMode") == 1.0);
  // Drag the middle keyframe (0.5) down 20 px.
  h0 = u.history();
  Vec2 a = u.at(171.5f, posY(0.5f));
  u.d.drag(a, {a.x, a.y + 20.0f}, 5);
  CHECK(u.history() == h0 + 1);
  meta::WavetableData d = u.data();
  REQUIRE(d.keyframes.size() == 3u);
  CHECK(d.keyframes[1].position == doctest::Approx(0.5 - 20.0 / span).epsilon(1e-4));
  // Ctrl: on the dense frames (k / 63).
  a = u.at(171.5f, posY(d.keyframes[1].position));
  u.d.drag(a, {a.x, a.y - 13.0f}, 5, mod::kCtrl);
  d = u.data();
  const double k = d.keyframes[1].position * 63.0;
  CHECK(k == doctest::Approx(std::round(k)).epsilon(1e-4));
  // A marker never passes its neighbours.
  a = u.at(171.5f, posY(d.keyframes[1].position));
  u.d.drag(a, {a.x, a.y - 600.0f}, 8);
  CHECK(u.data().keyframes[1].position == doctest::Approx(1.0));
  REQUIRE(u.ctx.undo());
  // Double-click the track at 0.75: a keyframe there, from the frame played there.
  h0 = u.history();
  u.d.doubleClick(u.at(178.0f, posY(0.75f)));
  CHECK(u.history() == h0 + 1);
  d = u.data();
  REQUIRE(d.keyframes.size() == 4u);
  CHECK(d.keyframes[2].position == doctest::Approx(0.75).epsilon(0.02));
  CHECK(d.gaps.size() == 3u);
  CHECK(d.keyframes[2].cycle != d.keyframes[1].cycle);
  // Delete removes the selected (the new) keyframe.
  h0 = u.history();
  CHECK(u.d.keyDown(key::kDelete));
  CHECK(u.history() == h0 + 1);
  CHECK(u.data().keyframes.size() == 3u);
  // The gap glyph between keyframes 0 and 1 toggles spectral → crossfade.
  d = u.data();
  h0 = u.history();
  u.d.click(u.at(192.0f, 0.5f * (posY(d.keyframes[0].position) + posY(d.keyframes[1].position))));
  CHECK(u.history() == h0 + 1);
  CHECK(u.data().gaps.at(0) == dsp::GapMode::Crossfade);
  // Basic shapes replace the selected keyframe's cycle: select keyframe 0, click the square (the fourth of six).
  u.d.click(u.at(171.5f, posY(0.0f)));
  h0 = u.history();
  u.d.click(u.at(188.0f - 2.0f * 14.0f - 2.0f * 1.5f - 7.0f, 24.0f));
  CHECK(u.history() == h0 + 1);
  CHECK(u.data().keyframes[0].cycle == basic(dsp::BasicCycle::Square));
  // Delete down to one keyframe; the last is never removed.
  u.d.click(u.at(171.5f, posY(0.0f)));
  CHECK(u.d.keyDown(key::kDelete));
  u.d.click(u.at(171.5f, posY(u.data().keyframes[0].position)));
  CHECK(u.d.keyDown(key::kDelete));
  REQUIRE(u.data().keyframes.size() == 1u);
  h0 = u.history();
  u.d.click(u.at(171.5f, posY(u.data().keyframes[0].position)));
  CHECK(u.d.keyDown(key::kDelete));
  CHECK(u.history() == h0);
  CHECK(u.ctx.status().find("at least one keyframe") != std::string::npos);
  // Shape mode is refused (TMM.8), with the reason.
  u.d.click(u.at(14.0f + 14.0f + 1.5f + 7.0f, 24.0f));
  CHECK(u.history() == h0);
  CHECK(u.setting("mode") == 0.0);
  CHECK(u.ctx.status().find("Shape mode") != std::string::npos);
}

TEST_CASE("wavetable face: Esc during a knob drag undoes it; the stacked frames are drawn directly while it runs") {
  WtUi u;
  REQUIRE(u.d.enableGpu().ok());
  u.frame(1.0);
  const Vec2 a = u.at(38.0f, 212.0f);
  u.d.down(a);
  u.d.move({a.x, a.y - 20.0f});
  u.d.move({a.x, a.y - 40.0f});
  CHECK(u.value("fold") > 0.0);
  u.view().canvas().markDirty();
  REQUIRE(u.d.step());
  CHECK(u.view().canvas().stats().faceLayersDirect >= 1u);  // the stack re-bakes each step: no tile per step
  const size_t h0 = u.history();
  (void)u.d.keyDown(key::kEscape);
  (void)u.d.up({a.x, a.y - 40.0f});
  CHECK(u.value("fold") == 0.0);
  CHECK(u.history() == h0);
}

TEST_CASE("wavetable face: the info view names the hovered control; cursors follow it") {
  WtUi u;
  u.frame(1.5);
  project_patch::ProjectCanvas& c = u.view().canvas();
  u.d.move(u.at(178.0f, posY(0.2f)));
  CHECK(std::string(c.info().title) == "Position");
  u.d.move(u.at(171.5f, posY(0.5f)));
  CHECK(std::string(c.info().title) == "Keyframe 2");
  CHECK(c.cursorAt(u.at(171.5f, posY(0.5f))) == Cursor::ResizeNS);
  u.d.move(u.at(60.0f, 182.0f));
  CHECK(std::string(c.info().title) == "Phase");
  CHECK(c.cursorAt(u.at(60.0f, 182.0f)) == Cursor::ResizeEW);
  u.d.move(u.at(89.0f, 212.0f));
  CHECK(std::string(c.info().title) == "Warp");
  u.d.move(u.at(89.0f + 14.5f, 233.0f));
  CHECK(std::string(c.info().title) == "Warp mode: Pulse width");
  CHECK(c.cursorAt(u.at(89.0f + 14.5f, 233.0f)) == Cursor::Pointer);
}

TEST_CASE("render: project-meta-wavetable matches the golden image") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize("project-meta-wavetable", w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene("project-meta-wavetable", {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  checkImage("project-meta-wavetable", *img);
}

TEST_CASE("wavetable face: the live layer's paint cost (2x, 64 frames, 8 voices) and 16 animating faces") {
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  std::unique_ptr<MetaFace> face = makeFace(wtType());
  REQUIRE(face != nullptr);
  SyntheticVoices voices;
  voices.setVoices(eightVoices(true));
  face->bind(std::make_unique<WtTarget>(), &voices);
  FaceJack jacks[3];
  jacks[0] = {{0.0f, 24.0f}, true, true, true, "x"};
  jacks[1] = {{0.0f, 140.0f}, true, true, true, "pos"};
  jacks[2] = {{204.0f, 125.0f}, false, true, true, "out"};
  FaceEnv env;
  env.theme = &*theme;
  env.uiScale = 2.0f;
  env.jacks = jacks;
  DrawList dl;
  auto paintOnce = [&](uint32_t faces) {
    dl.reset(2560, 1440);
    for (uint32_t k = 0; k < faces; ++k) {
      ui::FacePainter p(dl, {{static_cast<float>(20 + 420 * (k % 6)), static_cast<float>(20 + 500 * (k / 6))}, 2.0f});
      face->paintLive(p, env);
    }
  };
  for (int i = 0; i < 20; ++i) {
    voices.advance(1.0 / 60.0);
    (void)face->pollVoices();
    paintOnce(1);
  }
  std::vector<double> one, sixteen;
  for (int i = 0; i < 200; ++i) {
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
  MESSAGE("live layer, 1 face (2x, 64 frames, 8 voices): median " << one[100] << " ms; 16 faces: median " << sixteen[100] << " ms");
  CHECK(face->sounding().size() == 8u);
#if PQ_OPTIMIZED
  CHECK(one[100] < 0.3);
  CHECK(sixteen[100] < 1.5);
#endif
}
