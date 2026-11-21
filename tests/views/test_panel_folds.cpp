// SM.9 (docs/plans/SM.md "Face", SM.9): two general panel mechanisms on a test-only panel (the panel project's group,
// its panel replaced) — the OVAL face shape (a `shape` element "ellipse" at the bottom of a panel: the plate drawn as
// it, a press in the rect's corners outside it misses the block, the orbiting main ports sit just outside the ellipse)
// and FOLD sections (`fold` elements own the elements below them up to the next fold; folded they hide them; with the
// panel's `fit` the panel shrinks to the open sections and what lies below moves up). Fold state is view state: saved
// with the project, never an undo step; the project patch lays the block out again at its new size, its top-left and its
// neighbours staying where they were. Goldens folded / unfolded at 1x and 2x and the oval; every gesture through real
// dispatch (window → canvas → face → element).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/serialize.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/orbit.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/element.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
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

// The fold panel's element ids.
enum : uint32_t { kTitle = 1, kPlate, kFoldA, kKnobA1, kKnobA2, kFoldB, kSliderB, kFoldC, kKnobC };
constexpr float kFoldW = 200.0f, kFoldH = 244.0f;
constexpr float kBandB = 40.0f;  // fold B's section: [120, 160)

doc::PanelElement el(uint32_t id, const char* kind, float x, float y, float w, float h, std::vector<doc::Binding> b = {}) {
  doc::PanelElement e;
  e.id = id;
  e.kind = kind;
  e.x = x, e.y = y, e.w = w, e.h = h;
  e.bindings = std::move(b);
  return e;
}

// A title above the folds, a plate spanning the panel (it shrinks with the folded sections), and three sections:
// Sample (two knobs, open), Warp (a slider, folded by default), Granular (a knob, open).
doc::PanelPtr foldPanel(const PanelProjectIds& ids) {
  const std::string vp(doc::kValueParam);
  auto p = std::make_shared<doc::Panel>();
  p->width = kFoldW;
  p->height = kFoldH;
  p->fit = true;
  {
    doc::PanelElement plate = el(kPlate, "shape", 4, 4, 192, 236);
    plate.setField(doc::styleText("stroke", "none"));
    plate.setField(doc::styleNumber("opacity", 0.35));
    p->elements.push_back(plate);
  }
  {
    doc::PanelElement t = el(kTitle, "label", 10, 6, 120, 12);
    t.setField(doc::styleText("text", "FOLDS"));
    t.setField(doc::styleText("align", "left"));
    p->elements.push_back(t);
  }
  auto fold = [&](uint32_t id, float y, const char* title, bool open) {
    doc::PanelElement f = el(id, "fold", 8, y, 184, 16);
    f.setField(doc::styleText("title", title));
    if (!open) f.setField(doc::styleNumber("open", 0.0));
    p->elements.push_back(f);
  };
  fold(kFoldA, 20, "Sample", true);
  p->elements.push_back(el(kKnobA1, "knob", 16, 42, 48, 60, {doc::controlBinding(ids.level, vp)}));
  p->elements.push_back(el(kKnobA2, "knob", 76, 42, 48, 60, {doc::controlBinding(ids.cutoff, vp)}));
  fold(kFoldB, 104, "Warp", false);
  p->elements.push_back(el(kSliderB, "slider", 16, 126, 168, 26, {doc::controlBinding(ids.drive, vp)}));
  fold(kFoldC, 160, "Granular", true);
  p->elements.push_back(el(kKnobC, "knob", 16, 180, 48, 60, {doc::controlBinding(ids.mix, vp)}));
  return p;
}

// An oval: the plate an ellipse filling 180 × 100, a label and a knob inside it.
constexpr float kOvalW = 180.0f, kOvalH = 100.0f;
doc::PanelPtr ovalPanel(const PanelProjectIds& ids) {
  auto p = std::make_shared<doc::Panel>();
  p->width = kOvalW;
  p->height = kOvalH;
  doc::PanelElement plate = el(1, "shape", 0, 0, kOvalW, kOvalH);
  plate.setField(doc::styleText("shape", "ellipse"));
  plate.setField(doc::styleText("fill", "body"));
  plate.setField(doc::styleNumber("thickness", 1.2));
  p->elements.push_back(plate);
  doc::PanelElement t = el(2, "label", 40, 14, 100, 12);
  t.setField(doc::styleText("text", "OVAL"));
  p->elements.push_back(t);
  p->elements.push_back(el(3, "knob", 66, 28, 48, 60, {doc::controlBinding(ids.level, std::string(doc::kValueParam))}));
  return p;
}

Status setPanel(doc::DocumentStore& store, doc::DeviceId group, doc::PanelPtr p) {
  doc::ops::EditResult e = doc::ops::setGroupPanel(*store.current(), group, std::move(p));
  if (!e) return std::move(e).error();
  return store.dispatch(e->command);
}

// The group's panel in a plain tree, the host the panel's size (the goldens).
struct Standalone {
  FakeTransport port;
  ViewContext ctx{port};
  PanelProjectIds ids;
  TestDriver d;
  panel::PanelHost* host = nullptr;
  Standalone(float scale, bool oval, uint32_t w, uint32_t h)
      : d(static_cast<uint32_t>(static_cast<float>(w) * scale), static_cast<uint32_t>(static_cast<float>(h) * scale), scale) {
    ctx.setDeviceLibrary(&devLib());
    REQUIRE(buildPanelProject(ctx.store(), &ids).ok());
    REQUIRE(setPanel(ctx.store(), ids.group, oval ? ovalPanel(ids) : foldPanel(ids)).ok());
    REQUIRE(ctx.store().history().cursor() > 0);
    auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, ids.group),
                                                        panel::makeDocPanelTarget(ctx, ids.group, [this]() -> Widget* { return host; }));
    host = &d.setRoot<panel::PanelHost>("panel", std::move(face));
    d.tree().layoutNow();
  }
  panel::GroupPanelFace& face() const { return static_cast<panel::GroupPanelFace&>(host->face()); }
  Image render() {
    REQUIRE(d.enableGpu().ok());
    d.advance(0.1);
    auto img = d.render();
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    return std::move(*img);
  }
  // A click on fold element `id` (its title), through the host.
  void clickFold(uint32_t id) {
    const int32_t i = face().indexOf(id);
    REQUIRE(i >= 0);
    const Rect r = face().element(static_cast<size_t>(i)).rect();
    d.click(host->transform().map({r.x0 + 30.0f, (r.y0 + r.y1) * 0.5f}));
    d.step();
  }
};

// The panel project in the shell's project patch view with the group closed (drawn as its panel) at zoom 1.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  PanelProjectIds ids;

  explicit Ui(bool oval) {
    ctx.setDeviceLibrary(&devLib());
    REQUIRE(buildPanelProject(ctx.store(), &ids).ok());
    REQUIRE(setPanel(ctx.store(), ids.group, oval ? ovalPanel(ids) : foldPanel(ids)).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    view().setGroupOpen(ids.group, false);
    d.tree().layoutNow();
    const Node& n = node();
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = n.x - 60.0;
    v.y = n.y - 60.0;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
    d.tree().setFocus(&view().canvas());
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }
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
    (void)g->sync();
    return *g;
  }
  // A point of element `id` as the face shows it (fractions of its rect), device px.
  Vec2 at(uint32_t id, float fx = 0.5f, float fy = 0.5f) const {
    panel::GroupPanelFace& f = face();
    const int32_t i = f.indexOf(id);
    REQUIRE(i >= 0);
    const Rect r = f.element(static_cast<size_t>(i)).rect();
    const Node& n = node();
    return view().canvas().toDevice({n.x + r.x0 + (r.x1 - r.x0) * fx, n.y + r.y0 + (r.y1 - r.y0) * fy});
  }
  Vec2 world(float x, float y) const { return view().canvas().toDevice({node().x + x, node().y + y}); }
  size_t history() const { return ctx.store().history().cursor(); }
  double position(doc::DeviceId id) const {
    const doc::DeviceValue* v = doc::findDevice(*ctx.doc(), id).device->param(doc::kValueParam);
    REQUIRE(v != nullptr);
    return v->value;
  }
  std::optional<bool> foldState(uint32_t id) const {
    const auto& all = ctx.store().view().panelWindows;
    const auto it = all.find(ids.group.value);
    return it == all.end() ? std::nullopt : it->second.foldOpen(id);
  }
  bool selected() const {
    for (const doc::DeviceId s : view().selectedDevices())
      if (s == ids.group) return true;
    return false;
  }
  // Every other node's place (kind, device, band → x, y): blocks, mixers, the master, the bands' furniture.
  struct Place {
    NodeKind kind;
    uint32_t device, band;
    graph::Point2 at;
    bool operator==(const Place&) const = default;
  };
  std::vector<Place> others() const {
    std::vector<Place> v;
    for (const Node& n : view().patch().nodes)
      if (n.device != ids.group && n.kind != NodeKind::Lane) v.push_back({n.kind, n.device.value, n.band, {n.x, n.y}});
    return v;
  }
  // After the block's height changed by dh: every node of its band where it was, every node of a band below dy = dh
  // lower (its band moved down by the growth), x unchanged everywhere.
  void checkStill(const std::vector<Place>& before, float dh) const {
    const std::vector<Place> now = others();
    REQUIRE(now.size() == before.size());
    const uint32_t mine = node().band;
    for (size_t i = 0; i < now.size(); ++i) {
      CAPTURE(i);
      CAPTURE(static_cast<int>(now[i].kind));
      CHECK(now[i].kind == before[i].kind);
      CHECK(now[i].band == before[i].band);
      CHECK(now[i].at.x == doctest::Approx(before[i].at.x));
      const float dy = now[i].band == mine ? 0.0f : now[i].band > mine && now[i].band != kNone ? dh : 0.0f;
      CHECK(now[i].at.y == doctest::Approx(before[i].at.y + dy));
    }
  }
};

}  // namespace

TEST_CASE("panel folds: the layout — a folded section's elements hidden; fit takes its band out (what is below moves up, "
          "a spanning plate shrinks); without fit nothing moves") {
  PanelProjectIds ids;
  ids.level = ids.cutoff = ids.drive = ids.mix = doc::DeviceId{7};
  doc::Panel p = *foldPanel(ids);
  auto byDefault = [](const doc::PanelElement& e) { return doc::foldOpenByDefault(e); };
  const doc::FoldLayout l = doc::foldLayout(p, byDefault);
  REQUIRE(l.folds);
  auto idx = [&](uint32_t id) { return static_cast<size_t>(p.indexOf(id)); };
  CHECK(l.height == doctest::Approx(kFoldH - kBandB));
  CHECK(l.hidden[idx(kSliderB)] == 1);
  for (const uint32_t id : {kTitle, kPlate, kFoldA, kKnobA1, kKnobA2, kFoldB, kFoldC, kKnobC}) CHECK(l.hidden[idx(id)] == 0);
  CHECK(l.y[idx(kKnobA1)] == doctest::Approx(42.0f));         // above the folded band: in place
  CHECK(l.y[idx(kFoldB)] == doctest::Approx(104.0f));         // the folded header stays
  CHECK(l.y[idx(kFoldC)] == doctest::Approx(160.0f - kBandB));  // below it: up by its band
  CHECK(l.y[idx(kKnobC)] == doctest::Approx(180.0f - kBandB));
  CHECK(l.h[idx(kKnobC)] == doctest::Approx(60.0f));
  CHECK(l.h[idx(kPlate)] == doctest::Approx(236.0f - kBandB));  // spans the band: shrinks by it
  // Everything open: as the document has it.
  const doc::FoldLayout all = doc::foldLayout(p, [](const doc::PanelElement&) { return true; });
  CHECK(all.height == doctest::Approx(kFoldH));
  for (size_t i = 0; i < p.elements.size(); ++i) {
    CHECK(all.hidden[i] == 0);
    CHECK(all.y[i] == doctest::Approx(p.elements[i].y));
  }
  // Without fit: hidden, nothing moves, the size stays.
  p.fit = false;
  const doc::FoldLayout fixed = doc::foldLayout(p, byDefault);
  CHECK(fixed.height == doctest::Approx(kFoldH));
  CHECK(fixed.hidden[idx(kSliderB)] == 1);
  CHECK(fixed.y[idx(kKnobC)] == doctest::Approx(180.0f));
  // No folds: no layout.
  doc::Panel none;
  none.width = none.height = 10;
  none.elements.push_back(el(1, "knob", 0, 0, 10, 10));
  CHECK_FALSE(doc::foldLayout(none, byDefault).folds);
}

TEST_CASE("render: panel-folds folded / unfolded at 1x and 2x, panel-oval; a fold click through the host unfolds it "
          "(the face grows; no undo step)") {
  for (const float scale : {1.0f, 2.0f}) {
    CAPTURE(scale);
    const std::string sfx = scale == 1.0f ? "" : "-2x";
    Standalone s(scale, false, static_cast<uint32_t>(kFoldW), static_cast<uint32_t>(kFoldH));
    CHECK(s.face().worldSize().y == doctest::Approx(kFoldH - kBandB));
    checkImage("panel-folds-folded" + sfx, s.render());
    const size_t h0 = s.ctx.store().history().cursor();
    s.clickFold(kFoldB);
    CHECK(s.face().worldSize().y == doctest::Approx(kFoldH));
    CHECK(s.face().shown(static_cast<size_t>(s.face().indexOf(kSliderB))));
    CHECK(s.ctx.store().history().cursor() == h0);
    checkImage("panel-folds-open" + sfx, s.render());
  }
  for (const float scale : {1.0f, 2.0f}) {
    CAPTURE(scale);
    Standalone s(scale, true, static_cast<uint32_t>(kOvalW), static_cast<uint32_t>(kOvalH));
    const FaceShape sh = s.face().shape();
    CHECK(sh.round);
    CHECK(sh.radius == doctest::Approx(kOvalW * 0.5f - 0.6f));
    CHECK(sh.radiusY() == doctest::Approx(kOvalH * 0.5f - 0.6f));
    checkImage(scale == 1.0f ? "panel-oval" : "panel-oval-2x", s.render());
  }
}

TEST_CASE("panel folds (project patch, real dispatch): a fold click unfolds / folds its section — the block grows / "
          "shrinks in place (top-left kept, its band's blocks stay, the bands below follow), elements below move with it and take presses where "
          "they are shown; view state: saved, never an undo step") {
  Ui u(false);
  const Node n0 = u.node();
  CHECK(n0.h == doctest::Approx(kFoldH - kBandB));
  CHECK_FALSE(u.face().shown(static_cast<size_t>(u.face().indexOf(kSliderB))));

  // An edit first (one undo step): the Granular knob, below the folded band, dragged where it is shown.
  const size_t h0 = u.history();
  const double m0 = u.position(u.ids.mix);
  u.d.drag(u.at(kKnobC), {u.at(kKnobC).x, u.at(kKnobC).y - 40.0f}, 6);
  u.d.step();
  CHECK(u.position(u.ids.mix) != doctest::Approx(m0));
  CHECK(u.history() == h0 + 1);
  const double m1 = u.position(u.ids.mix);

  // Unfold Warp: view state, no step; the block grows downwards from the same top-left; nothing in its band moves (the
  // bands below move down by its growth, as for any block that grows its band).
  const auto others0 = u.others();
  REQUIRE(others0.size() >= 4);
  u.d.click(u.at(kFoldB, 0.2f));
  u.d.step();
  CHECK(u.foldState(kFoldB) == std::optional<bool>(true));
  CHECK(u.history() == h0 + 1);
  CHECK(u.node().h == doctest::Approx(kFoldH));
  CHECK(u.node().x == doctest::Approx(n0.x));
  CHECK(u.node().y == doctest::Approx(n0.y));
  u.checkStill(others0, kBandB);
  CHECK(u.face().shown(static_cast<size_t>(u.face().indexOf(kSliderB))));
  CHECK(u.face().element(static_cast<size_t>(u.face().indexOf(kKnobC))).rect().y0 == doctest::Approx(180.0f));
  // The slider takes presses where it now is (one step).
  const double d0 = u.position(u.ids.drive);
  u.d.drag(u.at(kSliderB, 0.2f), u.at(kSliderB, 0.8f), 4);
  u.d.step();
  CHECK(u.position(u.ids.drive) != doctest::Approx(d0));
  CHECK(u.history() == h0 + 2);

  // Undo takes back the slider edit, then the knob edit — the fold stays open (it was never a step).
  REQUIRE(u.ctx.undo());
  u.d.step();
  CHECK(u.position(u.ids.drive) == doctest::Approx(d0));
  CHECK(u.foldState(kFoldB) == std::optional<bool>(true));
  CHECK(u.node().h == doctest::Approx(kFoldH));
  REQUIRE(u.ctx.undo());
  u.d.step();
  CHECK(u.position(u.ids.mix) == doctest::Approx(m0));
  CHECK(u.position(u.ids.mix) != doctest::Approx(m1));
  CHECK(u.foldState(kFoldB) == std::optional<bool>(true));

  // Saved with the project and read back.
  auto text = doc::serializeProject(*u.ctx.doc(), u.ctx.store().view());
  REQUIRE(text.ok());
  CHECK(text->find("\"fit\"") != std::string::npos);
  auto back = doc::parseProject(*text);
  REQUIRE(back.ok());
  REQUIRE(back->view.panelWindows.count(u.ids.group.value) == 1);
  CHECK(back->view.panelWindows.at(u.ids.group.value).foldOpen(kFoldB) == std::optional<bool>(true));
  const doc::Device* g = doc::findDevice(*back->project, u.ids.group).device;
  REQUIRE(g != nullptr);
  REQUIRE(g->hasPanel());
  CHECK(g->panel->fit);

  // Fold Sample: its knobs go, Warp and Granular move up by its band; the block shrinks in place again.
  const auto others1 = u.others();
  u.d.click(u.at(kFoldA, 0.2f));
  u.d.step();
  CHECK(u.foldState(kFoldA) == std::optional<bool>(false));
  CHECK_FALSE(u.face().shown(static_cast<size_t>(u.face().indexOf(kKnobA1))));
  constexpr float kBandA = 104.0f - 36.0f;
  CHECK(u.node().h == doctest::Approx(kFoldH - kBandA));
  CHECK(u.face().element(static_cast<size_t>(u.face().indexOf(kFoldB))).rect().y0 == doctest::Approx(104.0f - kBandA));
  CHECK(u.node().x == doctest::Approx(n0.x));
  CHECK(u.node().y == doctest::Approx(n0.y));
  u.checkStill(others1, -kBandA);
  // A press where the Sample knobs were is no knob now (Warp's slider is there): no edit of the level.
  const double l0 = u.position(u.ids.level);
  u.d.click(u.world(16.0f + 24.0f, 42.0f + 30.0f));
  u.d.step();
  CHECK(u.position(u.ids.level) == doctest::Approx(l0));
}

TEST_CASE("panel oval (project patch, real dispatch): a press just inside the ellipse's edge selects the block, one in "
          "the rect's corner outside it does not; its main ports orbit the ellipse") {
  Ui u(true);
  const Node& n = u.node();
  REQUIRE(n.groupPanel);
  CHECK(n.panelRound);
  CHECK(n.roundRY == doctest::Approx(kOvalH * 0.5f - 0.6f));
  CHECK(n.w == doctest::Approx(kOvalW));
  const float a = kOvalW * 0.5f, b = kOvalH * 0.5f;
  // The rect's corner (at 45° from the centre, just beyond the ellipse) farthest from the orbiting ports, and the point
  // just inside the edge toward it.
  const float c45 = std::sqrt(0.5f);
  float sx = 1.0f, sy = -1.0f, far = -1.0f;
  for (const float cx : {-1.0f, 1.0f})
    for (const float cy : {-1.0f, 1.0f}) {
      const graph::Point2 w{n.x + a + cx * a * c45, n.y + b + cy * b * c45};
      float near = 1e9f;
      for (const Node::Orbit* o : {&n.orbitIn, &n.orbitOut})
        if (o->on) near = std::min(near, std::hypot(o->at.x - w.x, o->at.y - w.y));
      if (near > far) far = near, sx = cx, sy = cy;
    }
  const Vec2 corner = u.world(a + sx * a * c45 * 1.12f, b + sy * b * c45 * 1.12f);
  const Vec2 inside = u.world(a + sx * a * c45 * 0.93f, b + sy * b * c45 * 0.93f);
  u.d.click(corner);
  u.d.step();
  CHECK_FALSE(u.selected());
  u.d.click(inside);
  u.d.step();
  CHECK(u.selected());
  u.d.click(corner);  // the canvas's background: deselects
  u.d.step();
  CHECK_FALSE(u.selected());
  // The orbiting ports sit just outside the ellipse (port radius + gap from it), the outline an ellipse.
  const Node& m = u.node();
  CHECK(m.outline.round);
  CHECK(m.outline.ry == doctest::Approx(m.outline.r * (kOvalH - 1.2f) / (kOvalW - 1.2f)).epsilon(0.01));
  bool any = false;
  for (const Node::Orbit* o : {&m.orbitIn, &m.orbitOut}) {
    if (!o->on) continue;
    any = true;
    const float dist = circuit::outlineDistance(m.outline, o->at);
    CHECK(dist == doctest::Approx(circuit::kOrbitPortR + circuit::kOrbitGap).epsilon(0.08));
  }
  CHECK(any);
}

TEST_CASE("panel oval: the ellipse outline — the ray's point on it, the normal, the signed distance") {
  circuit::Outline o;
  o.round = true;
  o.cx = 10.0f, o.cy = 20.0f, o.r = 40.0f, o.ry = 10.0f;  // 4 : 1, flatter than any oval face
  graph::Point2 nrm;
  const graph::Point2 right = circuit::outlinePoint(o, 0.0f, &nrm);
  CHECK(right.x == doctest::Approx(50.0f));
  CHECK(right.y == doctest::Approx(20.0f));
  CHECK(nrm.x == doctest::Approx(1.0f));
  const graph::Point2 down = circuit::outlinePoint(o, std::numbers::pi_v<float> * 0.5f, &nrm);
  CHECK(down.y == doctest::Approx(30.0f));
  CHECK(nrm.y == doctest::Approx(1.0f));
  for (float ang = 0.1f; ang < 6.2f; ang += 0.37f) {
    const graph::Point2 p = circuit::outlinePoint(o, ang, &nrm);
    const float u = (p.x - o.cx) / o.r, v = (p.y - o.cy) / o.ry;
    CHECK(u * u + v * v == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(std::fabs(circuit::outlineDistance(o, p)) < 1e-3f);
    // Pushed out along the normal by 6: about 6 outside.
    const graph::Point2 q{p.x + nrm.x * 6.0f, p.y + nrm.y * 6.0f};
    CHECK(circuit::outlineDistance(o, q) == doctest::Approx(6.0f).epsilon(0.01));
  }
  CHECK(circuit::outlineDistance(o, {o.cx, o.cy}) < 0.0f);
  CHECK(circuit::outlineDistance(o, {o.cx + 45.0f, o.cy}) == doctest::Approx(5.0f).epsilon(0.05));
}
