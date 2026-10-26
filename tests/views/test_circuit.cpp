// T4.2 / T4.3: the circuit canvas. Goldens (symbol sheet, the stock SVF and its core opened to atoms, 1.5×), every
// stdlib patch laid out with the symbol geometry without overlaps or wires through symbols, symbol tiles drawing the
// pixels of the direct path, wire styles, junction dots, LOD, hit testing and selection, view input, idle, the viewer
// (open a subcircuit, back, tidy → text → layout round trip).

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

#include "../ui/golden_check.h"
#include "pq/graph/parse.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/tile_atlas.h"
#include "pq/views/circuit/circuit_canvas.h"
#include "pq/views/circuit/circuit_viewer.h"
#include "pq/views/fake_transport.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::circuit;
namespace fs = std::filesystem;

namespace {

std::string stdlibPath(const char* rel) { return (fs::path(PQ_TEST_DATA_DIR) / ".." / "stdlib" / rel).string(); }

std::vector<std::string> stdlibFiles() {
  std::vector<std::string> out;
  for (const auto& e : fs::recursive_directory_iterator(fs::path(PQ_TEST_DATA_DIR) / ".." / "stdlib"))
    if (e.is_regular_file() && e.path().extension() == ".pqp") out.push_back(e.path().string());
  std::sort(out.begin(), out.end());
  return out;
}

std::shared_ptr<const PatchDocument> load(const std::string& path) {
  auto doc = loadPatchDocument(path);
  REQUIRE_MESSAGE(doc.ok(), (doc.ok() ? "" : doc.error().message));
  return *doc;
}

std::shared_ptr<const PatchDocument> fromText(std::string text) {
  auto doc = loadPatchDocument((fs::temp_directory_path() / "pq_circuit_test.pqp").string(), std::move(text));
  REQUIRE_MESSAGE(doc.ok(), (doc.ok() ? "" : doc.error().message));
  return *doc;
}

void checkGolden(const char* scene, const std::string& file, float scale = 1.0f, const char* goldenName = nullptr) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, file, w, h, UiScale(scale), o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string name = goldenName ? goldenName : scene;
  auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png");
  if (!ref) (void)writePng(name + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(name, *img, *ref, true, &d));
  MESSAGE(name << ": " << pq::ui::test::describe(d));
}

// Node rects pairwise apart, wires orthogonal, no wire segment through the interior of a symbol.
void checkScene(const CircuitScene& sc, const std::string& what) {
  CAPTURE(what);
  const size_t n = sc.nodes.size();
  for (size_t a = 0; a < n; ++a)
    for (size_t b = a + 1; b < n; ++b) {
      const SceneNode &A = sc.nodes[a], &B = sc.nodes[b];
      const bool overlap = A.x < B.x + B.w && B.x < A.x + A.w && A.y < B.y + B.h && B.y < A.y + A.h;
      CHECK_MESSAGE(!overlap, "nodes " << a << " (" << sc.specs[a].title << ") and " << b << " (" << sc.specs[b].title << ") overlap");
    }
  for (const SceneWire& w : sc.wires) {
    REQUIRE(w.pointCount >= 2);
    for (uint32_t i = 0; i + 1 < w.pointCount; ++i) {
      const graph::Point2 p = sc.points[w.firstPoint + i], q = sc.points[w.firstPoint + i + 1];
      CHECK_MESSAGE((p.x == q.x || p.y == q.y), "wire " << w.wire << " is not orthogonal");
      for (const SceneNode& v : sc.nodes) {
        const float x0 = v.x + 0.5f, x1 = v.x + v.w - 0.5f, y0 = v.y + 0.5f, y1 = v.y + v.h - 0.5f;
        const bool hits = std::min(p.x, q.x) < x1 && std::max(p.x, q.x) > x0 && std::min(p.y, q.y) < y1 && std::max(p.y, q.y) > y0;
        CHECK_MESSAGE(!hits, "wire " << w.wire << " crosses node " << v.node << " (" << sc.specs[v.spec].title << ")");
      }
    }
  }
}

// The canvas alone in a tree, painted through a real renderer (tiles on or off).
struct Rig {
  std::unique_ptr<GpuContext> gpu;
  std::unique_ptr<Renderer> renderer;
  SceneTextures tex;
  PaintResources res, resNoTiles;
  UiTree tree;
  CircuitCanvas* canvas = nullptr;
  std::unique_ptr<OffscreenTarget> target;
  DrawList dl;
  uint32_t w = 0, h = 0;
  double now = 0.0;

  Rig(uint32_t lw, uint32_t lh, float scale) {
    GpuOptions o;
    o.validation = true;
    auto g = GpuContext::create(o);
    REQUIRE_MESSAGE(g.ok(), (g.ok() ? "" : g.error().message));
    gpu = std::move(*g);
    auto r = Renderer::create(*gpu, OffscreenTarget::kFormat, 1);
    REQUIRE(r.ok());
    renderer = std::move(*r);
    auto t = createSceneTextures(*renderer, UiScale(scale));
    REQUIRE(t.ok());
    tex = std::move(*t);
    res = paintResources(tex);
    resNoTiles = res;
    resNoTiles.tiles = nullptr;
    const UiScale s(scale);
    w = static_cast<uint32_t>(s.px(static_cast<float>(lw)));
    h = static_cast<uint32_t>(s.px(static_cast<float>(lh)));
    tree.setViewport(w, h, s);
    tree.setResources(&res);
    canvas = &tree.emplaceRoot<CircuitCanvas>("circuit");
    auto tg = OffscreenTarget::create(*gpu, w, h, 1);
    REQUIRE(tg.ok());
    target = std::move(*tg);
  }
  // Frames until nothing is pending (tiles built, zoom settled).
  void settle() {
    for (int i = 0; i < 400 && (tree.needsFrame() || tree.nextWakeTime() <= now + 1.0); ++i) {
      now = std::max(now + 1.0 / 144.0, std::min(tree.nextWakeTime(), now + 1.0));
      tree.update(now);
      tex.text->beginFrame();
      dl.reset(w, h);
      tree.paint(dl, res, now);
      if (res.tiles) {  // what the renderer does for the frame (the tiles reach the GPU at draw())
        res.tiles->beginFrame();
      }
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

}  // namespace

// ---- goldens ------------------------------------------------------------------------------------------------------

TEST_CASE("circuit: golden of the symbol sheet") { checkGolden("circuit-symbols", ""); }

TEST_CASE("circuit: golden of the stock SVF (its cores as filter blocks)") {
  checkGolden("circuit-svf", stdlibPath("filters/svf.pqp"));
}

TEST_CASE("circuit: golden of the SVF core opened to atoms (z1 feedback, summing amps, multipliers)") {
  checkGolden("circuit-svf-core", stdlibPath("filters/svf.pqp"));
}

TEST_CASE("circuit: golden of the SVF core at 1.5x UI scale") {
  checkGolden("circuit-svf-core", stdlibPath("filters/svf.pqp"), 1.5f, "circuit-svf-core-1.5x");
}

// ---- T4.3: every stdlib patch opens laid out ------------------------------------------------------------------------

TEST_CASE("circuit: every stdlib patch opens laid out without overlaps, wires orthogonal and clear of symbols") {
  size_t patches = 0;
  for (const std::string& file : stdlibFiles()) {
    const auto doc = load(file);
    for (const graph::Patch& p : doc->parsed.file.patches) {
      graph::Patch laid;
      const auto scene = buildScene(p, {}, &laid);
      CHECK(graph::fullyPlaced(laid));
      CHECK(scene->autoPlaced == p.nodeCount());  // stdlib files store no layout: everything auto-placed
      checkScene(*scene, fs::path(file).filename().string() + ":" + p.name());
      ++patches;
    }
  }
  MESSAGE(patches << " stdlib patches");
  CHECK(patches >= 20);
}

TEST_CASE("circuit: tidy, then the layout spliced into the file text reads back identically (byte-stable)") {
  const auto doc = load(stdlibPath("filters/svf.pqp"));
  CircuitViewer viewer(doc);
  REQUIRE(viewer.show("svf_core"));
  const graph::AutoLayoutStats st = viewer.tidy();
  CHECK(st.layers > 2);
  auto text = viewer.laidOutText();
  REQUIRE(text.ok());
  CHECK(text->find("  layout {") != std::string::npos);  // (in the file's own line endings: CRLF in a Windows checkout)
  CHECK(text->find("    node ") != std::string::npos);
  CHECK(text->find("// State-variable filter, zero-delay feedback (TPT).") == 0);  // the hand-written text is kept
  // The text parses back with exactly the drawn layout, and splicing again changes no byte.
  const auto again = fromText(*text);
  const graph::Patch* p = again->parsed.file.find("svf_core");
  REQUIRE(p != nullptr);
  CHECK(*p == viewer.laidOut());
  CHECK(graph::fullyPlaced(*p));
  auto twice = graph::spliceLayout(again->source, again->parsed, again->parsed.file);
  REQUIRE(twice.ok());
  CHECK(*twice == *text);
  // Opened again, the stored layout is used as is (nothing auto-placed).
  CHECK(buildScene(*p)->autoPlaced == 0);
}

// ---- styles, junctions ----------------------------------------------------------------------------------------------

TEST_CASE("circuit: wire styles follow kind, rate, polyphony and modulation") {
  const auto doc = fromText(R"(patch Styles {
  in x : audio
  param k = 0.5
  out y : audio
  out g : bool
  out v : audio voice
  out f : audio
  y = x * k
  g = x > 0.25
  v = phasor(voice.hz)
  f = lp(x, cutoff: sine_hz(2))
}
patch lp {
  in x : audio
  param cutoff = 1000
  out y : audio
  y = x * cutoff
}
)");
  const graph::Patch& p = *doc->find("Styles");
  const std::vector<WireStyle> st = wireStyles(p);
  auto style = [&](const char* from, const char* toKind) -> WireStyle {
    for (graph::WireIndex w = 0; w < p.wireCount(); ++w) {
      const graph::Wire& wr = p.wire(w);
      const graph::NodeKind fk = p.node(wr.from.node).kind;
      const std::string src = !p.nodeName(wr.from.node).empty() ? p.nodeName(wr.from.node)
                              : fk == graph::NodeKind::Atom     ? std::string(graph::atomSignature(p.node(wr.from.node).atom).name)
                                                                : std::string(graph::toText(fk));
      const graph::NodeKind tk = p.node(wr.to.node).kind;
      const std::string to = tk == graph::NodeKind::Atom ? std::string(graph::atomSignature(p.node(wr.to.node).atom).name)
                                                         : std::string(graph::toText(tk));
      if (src == from && to == toKind) return st[w];
    }
    FAIL("no wire " << from << " -> " << toKind);
    return {};
  };
  CHECK(style("x", "mul") == WireStyle{false, WireRate::Audio, false, false});
  CHECK(style("k", "mul") == WireStyle{false, WireRate::Param, false, false});
  CHECK(style("gt", "out").boolean);                                           // comparator output: bool
  CHECK(style("voice.hz", "phasor") == WireStyle{false, WireRate::Audio, true, false});  // per voice: doubled
  CHECK(style("phasor", "out").poly);
  CHECK(style("subcircuit", "out") == WireStyle{false, WireRate::Audio, false, false});
  CHECK(style("sine_hz", "subcircuit").modulation);  // audio into a param pin: dashed
  CHECK_FALSE(style("x", "subcircuit").modulation);  // audio into an `in`: not modulation
}

TEST_CASE("circuit: a fan-out has junction dots where it branches; a crossing has none") {
  const auto doc = fromText(R"(patch Fan {
  in x : audio
  in a : audio
  in b : audio
  out p : audio
  out q : audio
  out r : audio
  out s : audio
  out t : audio
  p = tanh(x)
  q = abs(x)
  r = x * 2
  s = a
  t = b
}
)");
  const auto sc = buildScene(*doc->find("Fan"));
  CHECK(sc->junctions.size() >= 1);
  CHECK(sc->junctions.size() == sc->junctionWire.size());
  // Every junction is on a wire of x's net; none on a or b (single wires).
  for (size_t i = 0; i < sc->junctions.size(); ++i) {
    const graph::Wire& wr = doc->find("Fan")->wire(sc->wires[sc->junctionWire[i]].wire);
    CHECK(doc->find("Fan")->nodeName(wr.from.node) == "x");
  }
}

// ---- canvas: tiles, LOD, idle ---------------------------------------------------------------------------------------

TEST_CASE("circuit: symbol tiles draw what the direct path draws (1x, 1.5x, 2x; near and mid zoom)") {
  for (float scale : {1.0f, 1.5f, 2.0f}) {
    CAPTURE(scale);
    Rig rig(900, 600, scale);
    const auto doc = load(stdlibPath("filters/svf.pqp"));
    rig.canvas->setScene(buildScene(*doc->find("svf_core")));
    for (double zoom : {1.6, 1.0, 0.5, 0.2}) {  // 0.2: the Far level's chunks
      CAPTURE(zoom);
      CircuitCanvas::View v = rig.canvas->view();
      v.zoom = zoom;
      rig.canvas->setView(v);
      rig.settle();
      CHECK_FALSE(rig.canvas->zooming());
      CHECK_FALSE(rig.canvas->tilesPending());
      const Image tiled = rig.draw(rig.res);
      const CircuitCanvas::Stats st = rig.canvas->stats();
      const bool far = zoom < CircuitCanvas::kFarZoom;
      if (far) {  // settled: every chunk from its tiles
        CHECK(st.chunksFromTiles > 0);
        CHECK(st.chunksDirect == 0);
        CHECK(st.chunksStretched == 0);
      } else {  // settled: every body from its tile
        CHECK(st.nodesFromTiles == st.nodesPainted);
        CHECK(st.nodesDirect == 0);
      }
      const Image direct = rig.draw(rig.resNoTiles);
      if (far) CHECK(rig.canvas->stats().chunksDirect == st.chunksFromTiles);
      else CHECK(rig.canvas->stats().nodesDirect == st.nodesPainted);
      ImageDiff d;
      const bool same = pq::ui::test::matchesGolden("circuit-tiles-vs-direct", tiled, direct, true, &d);
      if (!same) (void)writePng("circuit-tiles-vs-direct.direct.png", direct);
      CHECK(same);
      MESSAGE("tiles vs direct: " << pq::ui::test::describe(d));
    }
  }
}

TEST_CASE("circuit: level of detail; idle after a zoom settles; pan and zoom repaint only the canvas") {
  Rig rig(800, 500, 1.0f);
  const auto doc = load(stdlibPath("filters/svf.pqp"));
  rig.canvas->setScene(buildScene(*doc->find("svf")));
  rig.settle();
  CHECK(!rig.tree.needsFrame());
  CHECK(std::isinf(rig.tree.nextWakeTime()));
  auto lodAt = [&](double zoom) {
    CircuitCanvas::View v = rig.canvas->view();
    v.zoom = zoom;
    rig.canvas->setView(v);
    rig.settle();
    (void)rig.draw(rig.res);
    return rig.canvas->lod();
  };
  CHECK(lodAt(0.2) == CircuitCanvas::Lod::Far);
  CHECK(rig.canvas->stats().labelsPainted == 0);
  CHECK(rig.canvas->stats().junctionsPainted == 0);
  CHECK(lodAt(0.5) == CircuitCanvas::Lod::Mid);
  CHECK(rig.canvas->stats().labelsPainted == 0);
  CHECK(lodAt(1.2) == CircuitCanvas::Lod::Near);
  CHECK(rig.canvas->stats().labelsPainted > 0);
  // A zoom step: frames are requested only until the settle timer fires and the tiles are built; then idle again.
  rig.canvas->zoomAt({400, 250}, 1.25);
  CHECK(rig.canvas->zooming());
  rig.settle();
  CHECK_FALSE(rig.canvas->zooming());
  CHECK(!rig.tree.needsFrame());
  CHECK(std::isinf(rig.tree.nextWakeTime()));
  // While zooming, the previous level's tiles are drawn stretched (no direct bodies, no rasterising in paint).
  rig.canvas->zoomAt({400, 250}, 1.25);
  (void)rig.draw(rig.res);
  CHECK(rig.canvas->stats().nodesStretched == rig.canvas->stats().nodesPainted);
  rig.settle();
}

// ---- canvas: input, hit testing, selection --------------------------------------------------------------------------

TEST_CASE("circuit: hit testing, click / Shift / Ctrl selection, rubber band, Esc") {
  TestDriver d(800, 600);
  auto& canvas = d.setRoot<CircuitCanvas>("circuit");
  const auto doc = load(stdlibPath("filters/svf.pqp"));
  canvas.setScene(buildScene(*doc->find("svf_core")));
  d.tree().layoutNow();
  const CircuitScene& sc = *canvas.scene();
  auto centreOf = [&](uint32_t v) {
    const SceneNode& n = sc.nodes[v];
    return canvas.toDevice({n.x + n.w * 0.5f, n.y + n.h * 0.5f});
  };
  const uint32_t a = 3, b = 7;
  CHECK(canvas.hitTest(centreOf(a)) == CanvasHit{CanvasHit::Kind::Node, a});
  d.click(centreOf(a));
  CHECK(canvas.selection().nodes == std::vector<uint32_t>{a});
  d.click(centreOf(b), mod::kShift);
  CHECK(canvas.selection().nodes == std::vector<uint32_t>{std::min(a, b), std::max(a, b)});
  d.click(centreOf(a), mod::kCtrl);
  CHECK(canvas.selection().nodes == std::vector<uint32_t>{b});
  // A pin is hit within 5 px of it; a wire segment within 4 px.
  const ScenePin& pin = sc.pins[sc.nodes[b].firstPin];
  const Vec2 pp = canvas.toDevice({pin.x, pin.y});
  CHECK(canvas.hitTest({pp.x + 3, pp.y}) == CanvasHit{CanvasHit::Kind::Pin, sc.nodes[b].firstPin});
  const SceneWire& w0 = sc.wires[0];
  const graph::Point2 q0 = sc.points[w0.firstPoint], q1 = sc.points[w0.firstPoint + 1];
  const Vec2 mid = canvas.toDevice({(q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f});
  const CanvasHit wh = canvas.hitTest({mid.x, mid.y + 2});
  CHECK(wh.kind != CanvasHit::Kind::None);
  // Clicking empty space clears; a band over everything selects every node.
  d.click({2, 2});
  CHECK(canvas.selection().nodes.empty());
  d.drag({1, 1}, {799, 599}, 6);
  CHECK(canvas.selection().nodes.size() == sc.nodes.size());
  d.press("Escape");
  CHECK(canvas.selection().nodes.empty());
}

TEST_CASE("circuit: Ctrl+wheel zooms at the pointer, the wheel pans, middle-drag pans, F fits") {
  TestDriver d(800, 600);
  auto& canvas = d.setRoot<CircuitCanvas>("circuit");
  const auto doc = load(stdlibPath("filters/svf.pqp"));
  canvas.setScene(buildScene(*doc->find("svf_core")));
  d.tree().layoutNow();
  const CircuitCanvas::View fitted = canvas.view();
  const Vec2 at{300, 200};
  const graph::Point2 before = canvas.toWorld(at);
  d.wheel(at, 2.0f, 0.0f, mod::kCtrl);
  CHECK(canvas.view().zoom == doctest::Approx(fitted.zoom * 1.15 * 1.15));
  const graph::Point2 after = canvas.toWorld(at);
  CHECK(after.x == doctest::Approx(before.x).epsilon(1e-4));
  CHECK(after.y == doctest::Approx(before.y).epsilon(1e-4));
  const CircuitCanvas::View z = canvas.view();
  d.wheel(at, -1.0f);
  CHECK(canvas.view().y > z.y);  // scrolled down
  const CircuitCanvas::View p0 = canvas.view();
  d.drag({400, 300}, {450, 330}, 3, mod::kNone, PointerButton::Middle);
  CHECK(canvas.view().x == doctest::Approx(p0.x - 50.0 / p0.zoom));
  CHECK(canvas.view().y == doctest::Approx(p0.y - 30.0 / p0.zoom));
  d.tree().setFocus(&canvas);
  d.press("F");
  CHECK(canvas.view() == fitted);
}

TEST_CASE("circuit viewer: double-click a subcircuit opens its callee, Back returns; the action needs a file") {
  TestDriver d(1000, 700);
  auto& root = d.setRoot<Widget>("root");
  root.setLayout(LayoutKind::Stack);
  auto& layer = root.add<OverlayLayer>("overlay");
  (void)layer;
  d.tree().layoutNow();
  const auto doc = load(stdlibPath("filters/svf.pqp"));
  CircuitViewer* v = openCircuitViewer(root, doc, "svf");
  REQUIRE(v != nullptr);
  d.tree().layoutNow();
  CHECK(v->title() == "svf.pqp \xE2\x80\xBA svf");
  // The first svf_core block.
  const CircuitScene& sc = *v->canvas().scene();
  uint32_t core = UINT32_MAX;
  for (uint32_t i = 0; i < sc.nodes.size() && core == UINT32_MAX; ++i)
    if (sc.specs[i].symbol == Symbol::Filter && sc.specs[i].title == "svf_core") core = i;
  REQUIRE(core != UINT32_MAX);
  const SceneNode& n = sc.nodes[core];
  d.doubleClick(v->canvas().toDevice({n.x + n.w * 0.5f, n.y + n.h * 0.5f}));
  REQUIRE(v->shownPatch() != nullptr);
  CHECK(v->shownPatch()->name() == "svf_core");
  CHECK(v->title() == "svf.pqp \xE2\x80\xBA svf \xE2\x80\xBA svf_core");
  CHECK(v->back());
  CHECK(v->shownPatch()->name() == "svf");
  CHECK_FALSE(v->back());
}

TEST_CASE("circuit viewer: an imported subcircuit opens from its own file") {
  const auto doc = load(stdlibPath("filters/allpass.pqp"));  // imports onepole.pqp
  REQUIRE_FALSE(doc->imports.empty());
  CircuitViewer viewer(doc);
  REQUIRE(viewer.show());
  const graph::Patch& p = viewer.laidOut();
  uint32_t sub = UINT32_MAX;
  for (graph::NodeIndex i = 0; i < p.nodeCount() && sub == UINT32_MAX; ++i)
    if (p.node(i).kind == graph::NodeKind::Subcircuit && doc->find(p.subcircuit(i).patch) &&
        doc->owner(*doc->find(p.subcircuit(i).patch)) != doc.get())
      sub = i;
  REQUIRE(sub != UINT32_MAX);
  REQUIRE(viewer.openNode(sub));
  CHECK(viewer.title().find(".pqp \xE2\x80\xBA ") != std::string::npos);
  CHECK(viewer.laidOutText().ok());
}
