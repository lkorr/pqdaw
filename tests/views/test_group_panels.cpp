// GU.3 (docs/plans/GU.md "Phases" 3): group panels in the views — the element registry, element goldens (every generic
// kind, a few styles, 1x / 2x, tiles == direct), a closed group drawn as its panel in the project patch view and in the
// Device view (its inspector beside it), and every element's edit through REAL event dispatch (window → canvas → face →
// element; the scene rebuilding during the event): knob / slider / toggle / menu / number edit the bound control or
// setting in one undo step, a setting edit is exactly one structural change (one recompile), the tabs hide / show
// elements, the curve editor's point drag changes no class (no recompile) and its shape menu does; the panel follows a
// deleted block (and its undo) and survives paste. (GU.4's module panels: tests/views/test_module_faces.cpp since GU.5b.)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/modules.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/element.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::project_patch;

namespace {

const doc::DeviceLibrary& devLib() {
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

// The panel project in a plain tree: the group's panel in a PanelHost filling the window (the element goldens).
struct Standalone {
  FakeTransport port;
  ViewContext ctx{port};
  PanelProjectIds ids;
  TestDriver d;
  panel::PanelHost* host = nullptr;
  explicit Standalone(float scale) : d(static_cast<uint32_t>(480 * scale), static_cast<uint32_t>(220 * scale), scale) {
    ctx.setDeviceLibrary(&devLib());
    REQUIRE(buildPanelProject(ctx.store(), &ids).ok());
    auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, ids.group),
                                                        panel::makeDocPanelTarget(ctx, ids.group, [this]() -> Widget* { return host; }));
    host = &d.setRoot<panel::PanelHost>("panel", std::move(face));
    d.tree().layoutNow();
  }
  panel::GroupPanelFace& face() const { return static_cast<panel::GroupPanelFace&>(host->face()); }
  Image render() {
    REQUIRE(d.enableGpu().ok());
    d.advance(0.1);  // the element tiles built at this scale (prepare in onFrame)
    auto img = d.render();
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    return std::move(*img);
  }
};

// The panel project in the shell's project patch view, the group closed (drawn as its panel) at zoom 1, panels of stock
// devices off, pushes at once.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  PanelProjectIds ids;
  int structural = 0, events = 0;
  ViewContext::Subscription sub;

  Ui() {
    ctx.setDeviceLibrary(&devLib());
    REQUIRE(buildPanelProject(ctx.store(), &ids).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPanelsShown(false);
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    view().setGroupOpen(ids.group, false);
    d.tree().layoutNow();
    // The group's panel at zoom 1, 40 px in from the canvas's top-left.
    const Node& n = node();
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = n.x - 40.0;
    v.y = n.y - 40.0;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
    d.tree().setFocus(&view().canvas());
    // What recompiles: a document change that is structural (a gesture's commit changes nothing: the link compiled
    // its steps as they came).
    sub = ctx.subscribe([this](const doc::StoreEvent& e) {
      ++events;
      structural += e.before != e.after && e.changes.structural() ? 1 : 0;
    });
  }
  ~Ui() {
    sub.reset();
    d.tree().setRoot(std::make_unique<Widget>());
  }
  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const Node& node() const {
    const uint32_t n = view().patch().nodeOf(ids.group);
    REQUIRE(n != kNone);
    return view().patch().nodes[n];
  }
  panel::GroupPanelFace& face() const {
    const uint32_t mn = view().patch().nodeOf(ids.group);
    REQUIRE(mn != kNone);
    Face* f = view().canvas().panelOn(view().scene().sceneNode[mn]);
    REQUIRE(f != nullptr);
    auto* g = dynamic_cast<panel::GroupPanelFace*>(f);
    REQUIRE(g != nullptr);
    (void)g->sync();  // (what its paint does first)
    return *g;
  }
  // The first element of `kind` (and `x` ≥ minX), its index in the face.
  int32_t element(std::string_view kind, float minX = -1.0f) const {
    panel::GroupPanelFace& f = face();
    for (size_t i = 0; i < f.elementCount(); ++i)
      if (f.element(i).desc().kind == kind && f.element(i).desc().x >= minX) return static_cast<int32_t>(i);
    FAIL("no element " << kind);
    return -1;
  }
  // A point of element i (fractions of its rect), device px.
  Vec2 at(int32_t i, float fx = 0.5f, float fy = 0.5f) const {
    const doc::PanelElement& e = face().element(static_cast<size_t>(i)).desc();
    const Node& n = node();
    return view().canvas().toDevice({n.x + e.x + e.w * fx, n.y + e.y + e.h * fy});
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Device& dev(doc::DeviceId id) const { return *doc::findDevice(*ctx.doc(), id).device; }
  double position(doc::DeviceId id) const {
    const doc::DeviceValue* v = dev(id).param(doc::kValueParam);
    REQUIRE(v != nullptr);
    return v->value;
  }
  double setting(std::string_view name) const {
    const doc::DeviceValue* v = dev(ids.group).setting(name);
    return v != nullptr ? v->value : -1.0;
  }
  Menu* menu() const {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<Menu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  void choose(std::string_view label) {
    Menu* m = menu();
    REQUIRE(m != nullptr);
    int32_t item = -1;
    for (size_t i = 0; i < m->items().size(); ++i)
      if (m->items()[i].label == label) item = static_cast<int32_t>(i);
    REQUIRE_MESSAGE(item >= 0, "no menu item " << label);
    const IRect r = m->itemRect(item);
    d.click({static_cast<float>(r.x + r.w / 2), static_cast<float>(r.y + r.h / 2)});
    d.step();
  }
};

}  // namespace

TEST_CASE("group panels: the registry — every generic kind with its schema; an unknown kind is a placeholder; the static "
          "key follows kind + style + size only") {
  const std::vector<std::string_view> kinds = panel::elementKinds();
  for (const std::string_view k : {"knob", "slider", "number", "toggle", "button", "menu", "xy", "label", "shape", "tabs",
                                   "curve", "modsource"}) {
    CAPTURE(std::string(k));
    CHECK(std::find(kinds.begin(), kinds.end(), k) != kinds.end());
    const panel::ElementSchema* s = panel::elementSchema(k);
    REQUIRE(s != nullptr);
    CHECK(s->defaultW > 0.0f);
  }
  CHECK(panel::elementSchema("knob")->style.size() >= 5);  // pointer, track, sweep, colour, readout, label
  CHECK(panel::elementSchema("xy")->slots.size() == 2);
  doc::PanelElement a;
  a.id = 1;
  a.kind = "knob";
  a.w = 48, a.h = 60;
  doc::PanelElement b = a;
  b.id = 2;
  b.x = 100;  // elsewhere: the same tile
  b.bindings.push_back(doc::controlBinding(doc::DeviceId{7}, "value"));
  doc::PanelElement c = a;
  c.setField(doc::styleText("pointer", "pie"));
  CHECK(panel::makeElement(a)->staticKey() == panel::makeElement(b)->staticKey());
  CHECK(panel::makeElement(a)->staticKey() != panel::makeElement(c)->staticKey());
  doc::PanelElement u = a;
  u.kind = "from-a-newer-build";
  auto ue = panel::makeElement(u);
  REQUIRE(ue != nullptr);
  CHECK_FALSE(ue->interactive());
  // A new kind registers once.
  static constexpr std::string_view kSlots[] = {"value"};
  panel::ElementSchema s;
  s.kind = "test-kind";
  s.label = "Test";
  s.slots = kSlots;
  CHECK(panel::registerElement(s, [](const doc::PanelElement& d) { return panel::makeElement([&] { auto x = d; x.kind = "label"; return x; }()); }));
  CHECK_FALSE(panel::registerElement(s, nullptr));
}

TEST_CASE("render: panel-elements — every generic kind in a few styles (the Curve tab and the XY tab), 1x and 2x; "
          "static layers from the shared tiles equal direct drawing") {
  {
    Standalone s(1.0f);
    const Image img = s.render();
    CHECK(s.face().stats().drawn > 20u);
    CHECK(s.face().stats().fromTiles > 0u);
    CHECK(s.face().stats().direct == 0u);  // settled: every static layer from its tile
    checkImage("panel-elements", img);
    // Tiles == direct.
    PaintResources noTiles = s.d.resources();
    noTiles.tiles = nullptr;
    s.d.tree().setResources(&noTiles);
    auto direct = s.d.render();
    REQUIRE(direct.ok());
    CHECK(s.face().stats().fromTiles == 0u);
    CHECK(pq::ui::test::matchesGolden("panel-elements-direct", img, *direct));
    s.d.tree().setResources(&s.d.resources());
    // The XY tab: a press on its tab (real dispatch into the host).
    const int32_t tabs = s.face().indexOf(s.ids.tabs);
    REQUIRE(tabs >= 0);
    const doc::PanelElement& t = s.face().element(static_cast<size_t>(tabs)).desc();
    s.d.click(s.host->transform().map({t.x + t.w * 0.75f, t.y + t.h * 0.5f}));
    s.d.step();
    checkImage("panel-elements-xy", s.render());
  }
  {
    Standalone s(2.0f);
    checkImage("panel-elements-2x", s.render());
  }
}

TEST_CASE("render: project-patch-group-panel and device-group-panel-elements match the golden images") {
  for (const char* scene : {"project-patch-group-panel", "device-group-panel-elements"}) {
    CAPTURE(scene);
    uint32_t w = 0, h = 0;
    viewSceneDefaultSize(scene, w, h);
    GpuOptions o;
    o.validation = true;
    auto img = renderViewScene(scene, {}, w, h, {}, o, &devLib());
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    checkImage(scene, *img);
  }
}

TEST_CASE("group panels: a closed group with a panel is its panel in the circuit (the model and the face); an empty "
          "panel is the miniature again") {
  Ui u;
  const Node& n = u.node();
  CHECK(n.groupPanel);
  CHECK(n.mini.empty());
  CHECK(n.w == doctest::Approx(480.0f));
  CHECK(n.h == doctest::Approx(220.0f));
  panel::GroupPanelFace& f = u.face();
  CHECK(f.elementCount() == doc::findDevice(*u.ctx.doc(), u.ids.group).device->panel->elements.size());
  // Every element removed (one step): the miniature again; undo: the panel again.
  std::vector<uint32_t> all;
  for (const doc::PanelElement& e : u.dev(u.ids.group).panel->elements) all.push_back(e.id);
  REQUIRE(u.ctx.store().dispatch(doc::ops::removePanelElements(*u.ctx.doc(), u.ids.group, all)->command).ok());
  u.d.step();
  CHECK_FALSE(u.node().groupPanel);
  CHECK_FALSE(u.node().mini.empty());
  REQUIRE(u.ctx.undo());
  u.d.step();
  CHECK(u.node().groupPanel);
}

TEST_CASE("group panels: knob, slider and number drags edit the bound control in one undo step, never a recompile — "
          "real pointer dispatch") {
  Ui u;
  SUBCASE("knob") {
    const int32_t k = u.element("knob");
    const double p0 = u.position(u.ids.level);
    const size_t h0 = u.history();
    const Vec2 c = u.at(k);
    u.d.drag(c, {c.x, c.y + 60.0f}, 6);  // down: 0.3 of the travel (200 px per range)
    u.d.step();
    CHECK(u.position(u.ids.level) < p0 - 0.2);
    CHECK(u.history() == h0 + 1);
    CHECK(u.structural == 0);
    REQUIRE(u.ctx.undo());
    CHECK(u.position(u.ids.level) == doctest::Approx(p0));
  }
  SUBCASE("slider") {
    const int32_t s = u.element("slider");
    const double p0 = u.position(u.ids.level);
    const size_t h0 = u.history();
    const Vec2 c = u.at(s, 0.5f, 0.75f);
    u.d.drag(c, {c.x - 50.0f, c.y}, 5);
    u.d.step();
    CHECK(u.position(u.ids.level) < p0 - 0.2);
    CHECK(u.history() == h0 + 1);
    CHECK(u.structural == 0);
  }
  SUBCASE("number box: drag, then a typed value") {
    const int32_t nb = u.element("number");
    const double p0 = u.position(u.ids.cutoff);
    const size_t h0 = u.history();
    const Vec2 c = u.at(nb, 0.5f, 0.75f);
    u.d.drag(c, {c.x, c.y - 30.0f}, 3);
    u.d.step();
    CHECK(u.position(u.ids.cutoff) > p0);
    CHECK(u.history() == h0 + 1);
    // Double-click: type 440, Enter → 440 Hz (one more step).
    u.d.doubleClick(c);
    u.d.step();
    CHECK(u.face().wantsText());
    REQUIRE(u.d.type("440"));
    (void)u.d.press("Return");
    u.d.step();
    CHECK(u.history() == h0 + 2);
    const dsp::Curve* cv = u.dev(u.ids.cutoff).curve(doc::kValueParam);
    REQUIRE(cv != nullptr);
    CHECK(dsp::evalCurve(*cv, u.position(u.ids.cutoff)) == doctest::Approx(440.0).epsilon(1e-6));
    CHECK(u.structural == 0);
  }
}

TEST_CASE("group panels: a setting bound on a panel — a toggle click, a menu choice (through the overlay) and a number "
          "drag each edit the group's polyphony in one undo step and ONE structural change (one recompile)") {
  Ui u;
  SUBCASE("toggle (mono)") {
    const int32_t t = u.element("toggle");
    const size_t h0 = u.history();
    u.d.click(u.at(t, 0.15f));
    u.d.step();
    CHECK(u.setting(doc::kGroupMono) == 1.0);
    CHECK(doc::groupPolyphony(u.dev(u.ids.group)).mono);
    CHECK(u.history() == h0 + 1);
    CHECK(u.structural == 1);
    // The scene rebuilt during the event (a structural edit); the face stayed and still works.
    u.d.click(u.at(u.element("toggle"), 0.15f));
    u.d.step();
    CHECK(u.setting(doc::kGroupMono) == 0.0);
    CHECK(u.structural == 2);
  }
  SUBCASE("menu (steal) through the overlay") {
    const int32_t m = u.element("menu");
    const size_t h0 = u.history();
    u.d.click(u.at(m));
    u.d.step();
    REQUIRE(u.menu() != nullptr);
    CHECK(u.structural == 0);  // opening changes nothing
    u.choose("None");
    CHECK(u.menu() == nullptr);
    CHECK(u.setting(doc::kGroupSteal) == 2.0);
    CHECK(u.history() == h0 + 1);
    CHECK(u.structural == 1);
  }
  SUBCASE("number (voices): a drag is a preview, committed once at the release") {
    const int32_t nb = u.element("number", 340.0f);
    const size_t h0 = u.history();
    const Vec2 c = u.at(nb);
    u.d.down(c);
    for (int k = 1; k <= 6; ++k) u.d.move({c.x, c.y - 8.0f * static_cast<float>(k)});
    CHECK(u.structural == 0);  // nothing written while dragging
    CHECK(u.setting(doc::kGroupVoices) == 8.0);
    u.d.up({c.x, c.y - 48.0f});
    u.d.step();
    CHECK(u.setting(doc::kGroupVoices) > 8.0);
    CHECK(u.history() == h0 + 1);
    CHECK(u.structural == 1);
    REQUIRE(u.ctx.undo());
    CHECK(u.setting(doc::kGroupVoices) == 8.0);
  }
}

TEST_CASE("group panels: the tabs hide and show elements (the curve editor on one tab, the XY pad on the other); a "
          "press goes to what is shown") {
  Ui u;
  panel::GroupPanelFace& f = u.face();
  const int32_t curve = u.element("curve"), xy = u.element("xy"), tabs = f.indexOf(u.ids.tabs);
  REQUIRE(tabs >= 0);
  CHECK(f.shown(static_cast<size_t>(curve)));
  CHECK_FALSE(f.shown(static_cast<size_t>(xy)));
  const size_t h0 = u.history();
  u.d.click(u.at(tabs, 0.75f));
  u.d.step();
  CHECK(u.history() == h0);  // an unbound tab is the panel's own state, not an edit
  CHECK_FALSE(u.face().shown(static_cast<size_t>(curve)));
  CHECK(u.face().shown(static_cast<size_t>(xy)));
  // A press where both lie now drives the XY pad: drive and mix move together, one step.
  const double d0 = u.position(u.ids.drive), m0 = u.position(u.ids.mix);
  u.d.drag(u.at(xy, 0.2f, 0.2f), u.at(xy, 0.9f, 0.9f), 4);
  u.d.step();
  CHECK(u.position(u.ids.drive) != doctest::Approx(d0));
  CHECK(u.position(u.ids.mix) != doctest::Approx(m0));
  CHECK(u.history() == h0 + 1);
  // Back to the curve.
  u.d.click(u.at(tabs, 0.25f));
  u.d.step();
  CHECK(u.face().shown(static_cast<size_t>(curve)));
}

TEST_CASE("group panels: the curve editor — dragging a point keeps the curve's class (a Param change: no recompile, one "
          "step); a shape chosen from its menu is one Structural step") {
  Ui u;
  const int32_t ce = u.element("curve");
  const dsp::Curve before = *u.dev(u.ids.cutoff).curve(doc::kValueParam);
  REQUIRE(before.segments == 1);
  const size_t h0 = u.history();
  // The end point (t = 1, y = 1): its handle is at the plot's top-right corner (the plot is inset 6 units).
  const doc::PanelElement& e = u.face().element(static_cast<size_t>(ce)).desc();
  const Node& n = u.node();
  const Vec2 end = u.view().canvas().toDevice({n.x + e.x + e.w - 6.0f, n.y + e.y + 6.0f});
  u.d.drag(end, {end.x, end.y + 30.0f}, 4);
  u.d.step();
  const dsp::Curve after = *u.dev(u.ids.cutoff).curve(doc::kValueParam);
  CHECK(after.points[1].y < 0.9);
  CHECK(dsp::curveClass(after) == dsp::curveClass(before));
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);  // no recompile
  // Right-click the segment: Linear (a new class).
  u.d.click(u.at(ce, 0.5f, 0.85f), mod::kNone, PointerButton::Right);
  u.d.step();
  REQUIRE(u.menu() != nullptr);
  u.choose("Linear");
  const dsp::Curve lin = *u.dev(u.ids.cutoff).curve(doc::kValueParam);
  CHECK(lin.shapes[0].shape == dsp::CurveShape::Linear);
  CHECK(u.history() == h0 + 2);
  CHECK(u.structural == 1);
  // Double-click the curve's middle: a point (two segments), one step.
  u.d.doubleClick(u.at(ce, 0.5f, 0.5f));
  u.d.step();
  // (the double-click lands on the plot: a point at that position)
  CHECK(u.dev(u.ids.cutoff).curve(doc::kValueParam)->segments == 2);
}

TEST_CASE("group panels: a deleted block takes its elements off the face (undo brings them back); a pasted group keeps "
          "its panel, remapped") {
  Ui u;
  const size_t n0 = u.face().elementCount();
  REQUIRE(u.ctx.store().dispatch(doc::ops::deleteDevices(*u.ctx.doc(), std::vector<doc::DeviceId>{u.ids.level})->command).ok());
  u.d.step();
  CHECK(u.face().elementCount() == n0 - 4);  // two knobs, the slider and the mod source on Level
  REQUIRE(u.ctx.undo());
  u.d.step();
  CHECK(u.face().elementCount() == n0);
  // Ctrl+C on the selected group (its block), Ctrl+V at the pointer below: a second panel, bound to the copies.
  const uint32_t sn = u.view().scene().sceneNode[u.view().patch().nodeOf(u.ids.group)];
  u.view().canvas().select(std::vector<uint32_t>{sn}, {});
  u.d.tree().setFocus(&u.view().canvas());
  CHECK(u.d.press("Ctrl+C") != KeyResult::Unhandled);
  REQUIRE_FALSE(u.ctx.blockClipboard().devices.empty());
  // Empty canvas on Keys' band, right of its blocks.
  const ProjectPatch& pp = u.view().patch();
  const Band& b = pp.bands[pp.bandOf(u.ids.keys)];
  float right = b.x0;
  for (const uint32_t k : b.devices) right = std::max(right, pp.nodes[k].x + pp.nodes[k].w);
  circuit::CircuitCanvas::View v = u.view().canvas().view();
  v.x = right - 200.0;
  u.view().canvas().setView(v);
  u.d.tree().layoutNow();
  u.d.move(u.view().canvas().toDevice({right + 120.0f, b.y + b.h * 0.5f}));
  CHECK(u.d.press("Ctrl+V") != KeyResult::Unhandled);
  u.d.step();
  doc::DeviceId copy;
  for (const doc::DevicePtr& d : doc::findTrackPtr(*u.ctx.doc(), u.ids.keys)->devices)
    if (doc::isGroupDevice(*d) && d->id != u.ids.group) copy = d->id;
  REQUIRE(copy.valid());
  const doc::Device& g2 = u.dev(copy);
  REQUIRE(g2.panel != nullptr);
  CHECK(g2.panel->elements.size() == n0);
  for (const doc::PanelElement& e : g2.panel->elements)
    for (const doc::Binding& bd : e.bindings) {
      const bool inside = doc::groupContains(doc::findTrackPtr(*u.ctx.doc(), u.ids.keys)->devices, copy, bd.device) || bd.device == copy;
      CHECK(inside);
    }
}

TEST_CASE("group panels: the Device view shows the instrument group's panel with its inspector; the inspector's mono "
          "toggle is one step and one structural change — real pointer dispatch") {
  Ui u;
  u.ctx.setSelectedTrack(u.ids.keys);
  u.ctx.setDetailVisible(true);
  u.ctx.setDetailMode(DetailMode::Device);
  u.d.tree().layoutNow();
  u.d.step();
  DeviceChainView* dv = findDeviceChain(*u.shell);
  REQUIRE(dv != nullptr);
  Widget* frame = dv->frameOf(u.ids.group);
  REQUIRE(frame != nullptr);
  auto* inspector = dynamic_cast<panel::PanelHost*>(frame->find("inspector[" + std::to_string(u.ids.group.value) + "]"));
  auto* host = dynamic_cast<panel::PanelHost*>(frame->find("panel[" + std::to_string(u.ids.group.value) + "]"));
  REQUIRE(inspector != nullptr);
  REQUIRE(host != nullptr);
  CHECK(host->rect().w > 100);
  // The inspector's Mono toggle (its second row: the label, then the switch).
  auto& f = static_cast<panel::GroupPanelFace&>(inspector->face());
  int32_t mono = -1;
  for (size_t i = 0; i < f.elementCount(); ++i)
    if (f.element(i).desc().kind == "toggle" && f.element(i).binding() != nullptr && f.element(i).binding()->name == doc::kGroupMono)
      mono = static_cast<int32_t>(i);
  REQUIRE(mono >= 0);
  const doc::PanelElement& e = f.element(static_cast<size_t>(mono)).desc();
  const size_t h0 = u.history();
  const int s0 = u.structural;
  u.d.click(inspector->transform().map({e.x + 6.0f, e.y + e.h * 0.5f}));
  u.d.step();
  CHECK(u.setting(doc::kGroupMono) == 1.0);
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == s0 + 1);
}
