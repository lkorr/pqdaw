// GU.5 (docs/plans/GU.md "Panels and the panel window", "Phases" 5): a group's panel window in the project patch view —
// every gesture through REAL event dispatch (window → overlay → widget; menus closing themselves; the scene rebuilding
// during the event): the capsule's header button opens it; its title moves it, its grip resizes it, – minimises it, ×
// closes it and the header reopens it where it was (view state, saved); a control is sent to the panel by the window's
// +, by P and by dragging it (a block, an output's cable) onto the window — an element of its
// default kind, the control staying where it was, both the same control; edit mode moves / resizes / snaps (4 px at zoom
// 1, Alt free) / aligns / deletes elements, each gesture one undo step; the inspector edits the binding, the style, the
// curve and the visibility; play mode edits the bound controls; a mod source dragged onto a control makes the cable and
// the control shows its modulation ring; link lines from the window's elements to their controls in the circuit (the
// hovered element, or every element while the panel is the active one); a wire into a control of a closed group ends on
// its panel's element. Goldens: panel-window-closed, -open, -edit (a selection and snap guides), -inspector, -links.
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
#include "pq/document/serialize.h"
#include "pq/ui/controls.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/element.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/panel/panel_window.h"
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

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f, static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f}; }

// GU.3's panel project ("Keys": its instrument group "Synth" holding a Wavetable, a mul atom and the value blocks Level,
// Cutoff (octave), Drive, Mix) in the shell's project patch view, the group OPEN (its capsule, its blocks visible) at
// zoom 1; by default its panel cleared (the user builds it here). Panels of stock devices off, pushes at once.
struct WinUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1800, 960};
  std::unique_ptr<Shell> shell;
  PanelProjectIds ids;
  int structural = 0;
  ViewContext::Subscription sub;

  explicit WinUi(bool keepPanel = false) {
    ctx.setDeviceLibrary(&devLib());
    REQUIRE(buildPanelProject(ctx.store(), &ids).ok());
    if (!keepPanel) {
      auto e = doc::ops::setGroupPanel(*ctx.doc(), ids.group, nullptr);
      REQUIRE(e.ok());
      REQUIRE(ctx.store().dispatch(e->command).ok());
    }
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPanelsShown(false);
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    view().setGroupOpen(ids.group, true);  // GU.5b: a group with a panel starts closed
    d.tree().layoutNow();
    REQUIRE(view().groupOpen(ids.group));
    const Node& n = node(ids.group);
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = n.x - 30.0;
    v.y = n.y - 30.0;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
    d.tree().setFocus(&view().canvas());
    sub = ctx.subscribe([this](const doc::StoreEvent& e) { structural += e.before != e.after && e.changes.structural() ? 1 : 0; });
  }
  ~WinUi() {
    sub.reset();
    d.tree().setRoot(std::make_unique<Widget>());
  }
  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const Node& node(doc::DeviceId id) const {
    const uint32_t n = view().patch().nodeOf(id);
    REQUIRE(n != kNone);
    return view().patch().nodes[n];
  }
  Vec2 centre(doc::DeviceId id) const {
    const Node& n = node(id);
    return view().canvas().toDevice({n.x + n.w * 0.5f, n.y + n.h * 0.5f});
  }
  Vec2 headerButton() const {
    const LaneRect r = ProjectPatchView::panelButtonRect(node(ids.group));
    return view().canvas().toDevice({(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f});
  }
  panel::PanelWindow* window() const { return view().panelWindow(ids.group); }
  panel::PanelWindow& win() const {
    panel::PanelWindow* w = window();
    REQUIRE(w != nullptr);
    return *w;
  }
  Vec2 button(std::string_view name) const {
    Widget* b = win().find(name);
    REQUIRE(b != nullptr);
    return mid(b->rect());
  }
  // Opens the window by the capsule's header button and puts it at (x, y), w × h (logical px from the view's corner).
  panel::PanelWindow& open(float x = 1060.0f, float y = 40.0f, float w = 0.0f, float h = 0.0f) {
    if (window() == nullptr) {
      d.click(headerButton());
      d.step();
    }
    panel::PanelWindow& pw = win();
    doc::PanelWindowView s = pw.state();
    s.x = x;
    s.y = y;
    if (w > 0.0f) s.w = w;
    if (h > 0.0f) s.h = h;
    ctx.store().view().panelWindows[ids.group.value] = s;
    pw.setState(s);
    view().invalidateArrange();
    d.tree().layoutNow();
    d.step();
    return pw;
  }
  const doc::Device& dev(doc::DeviceId id) const { return *doc::findDevice(*ctx.doc(), id).device; }
  const doc::Panel* panel() const { return dev(ids.group).panel.get(); }
  size_t history() const { return ctx.store().history().cursor(); }
  double position(doc::DeviceId id) const {
    const doc::DeviceValue* v = dev(id).param(doc::kValueParam);
    return v != nullptr ? v->value : 0.0;
  }
  // Element i of the window's panel: a point of it (fractions of its rect), device px.
  Vec2 at(size_t i, float fx = 0.5f, float fy = 0.5f) const {
    const doc::PanelElement& e = win().face().element(i).desc();
    return win().panelTransform().map({e.x + e.w * fx, e.y + e.h * fy});
  }
  Vec2 atId(uint32_t id, float fx = 0.5f, float fy = 0.5f) const {
    const int32_t i = win().face().indexOf(id);
    REQUIRE(i >= 0);
    return at(static_cast<size_t>(i), fx, fy);
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
    d.click(mid(m->itemRect(item)));
    d.step();
  }
  // Sends Level (a value block: clicked in the circuit, then P): one element, a knob.
  uint32_t sendLevel() {
    REQUIRE(view().panelWindowAt(centre(ids.level)) == nullptr);
    d.click(centre(ids.level));
    d.step();
    REQUIRE(view().selectedControl() == doc::controlBinding(ids.level, std::string(doc::kValueParam)));
    CHECK(d.press("p") != KeyResult::Unhandled);
    d.step();
    REQUIRE(panel() != nullptr);
    return panel()->elements.back().id;
  }
  Widget* inspectorRow(std::string_view row, std::string_view control) const {
    Widget* r = win().find(std::string("inspector-scroll/inspector/") + std::string(row) + "/" + std::string(control));
    REQUIRE_MESSAGE(r != nullptr, "no inspector widget " << row << "/" << control);
    return r;
  }
  Image render() {
    REQUIRE(d.enableGpu().ok());
    d.advance(0.1);
    auto img = d.render();
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    return std::move(*img);
  }
};

const doc::PanelElement* byBinding(const doc::Panel* p, const doc::Binding& b) {
  if (p == nullptr) return nullptr;
  for (const doc::PanelElement& e : p->elements)
    if (e.binding() != nullptr && *e.binding() == b) return &e;
  return nullptr;
}

}  // namespace

TEST_CASE("panel window: an element's default kind follows its binding; free places never overlap; a panel grows to "
          "hold what is sent to it") {
  panel::BoundInfo cont;
  cont.valid = true;
  CHECK(panel::defaultElement(doc::controlBinding(doc::DeviceId{3}, "value"), cont).kind == "knob");
  panel::BoundInfo sw = cont;
  sw.isSwitch = true;
  CHECK(panel::defaultElement(doc::controlBinding(doc::DeviceId{3}, "on"), sw).kind == "toggle");
  panel::BoundInfo ch = cont;
  ch.choices = {"A", "B", "C"};
  CHECK(panel::defaultElement(doc::controlBinding(doc::DeviceId{3}, "mode"), ch).kind == "menu");
  panel::BoundInfo set = cont;
  set.setting = true;
  CHECK(panel::defaultElement(doc::settingBinding(doc::DeviceId{3}, "voices"), set).kind == "number");
  CHECK(panel::defaultElement(doc::signalBinding(doc::DeviceId{3}, "y"), cont).kind == "modsource");
  // Five knobs onto a new panel: each where none is, the panel grown to hold them.
  std::vector<doc::PanelElement> add(5);
  for (doc::PanelElement& e : add) e.kind = "knob", e.w = 48, e.h = 60;
  const doc::Panel p = panel::panelWithElements(nullptr, add, std::nullopt);
  REQUIRE(p.elements.size() == 5);
  CHECK(doc::checkPanel(p).ok());
  for (size_t i = 0; i < p.elements.size(); ++i)
    for (size_t j = i + 1; j < p.elements.size(); ++j) {
      const doc::PanelElement &a = p.elements[i], &b = p.elements[j];
      CHECK_FALSE((a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h));
    }
  for (const doc::PanelElement& e : p.elements) {
    CHECK(e.x + e.w <= p.width);
    CHECK(e.y + e.h <= p.height);
  }
  // At a point: centred there (whole units).
  const doc::Panel q = panel::panelWithElements(&p, {add.front()}, Vec2{100.0f, 50.0f});
  CHECK(q.elements.back().x == doctest::Approx(76.0f));
  CHECK(q.elements.back().y == doctest::Approx(20.0f));
}

TEST_CASE("panel window: the capsule's header button opens it; its title moves it, its grip resizes it, – minimises "
          "and restores it, × closes it and the header reopens it where it was — view state (saved, not undoable)") {
  WinUi u;
  CHECK(u.window() == nullptr);
  const size_t h0 = u.history();
  u.d.click(u.headerButton());
  u.d.step();
  panel::PanelWindow& w = u.win();
  CHECK(w.visible());
  CHECK(w.rect().w > 200);
  CHECK(w.editMode());  // an empty panel opens in edit mode (with its inspector)
  CHECK(u.ctx.store().view().panelWindows.at(u.ids.group.value).open);
  CHECK(u.history() == h0);  // view state: no undo step
  // Move: drag the title bar.
  const IRect r0 = w.rect();
  const Vec2 title{static_cast<float>(r0.x) + 40.0f, static_cast<float>(r0.y) + 10.0f};
  u.d.drag(title, {title.x - 300.0f, title.y + 120.0f}, 6);
  u.d.step();
  CHECK(w.rect().x == r0.x - 300);
  CHECK(w.rect().y == r0.y + 120);
  const doc::PanelWindowView moved = u.ctx.store().view().panelWindows.at(u.ids.group.value);
  CHECK(moved.y == doctest::Approx(w.state().y));
  CHECK(u.ctx.dirty());
  // Resize: drag the grip.
  const Widget* grip = w.find("grip");
  REQUIRE(grip != nullptr);
  const Vec2 g = mid(grip->rect());
  u.d.drag(g, {g.x + 80.0f, g.y + 60.0f}, 5);
  u.d.step();
  CHECK(w.rect().w == r0.w + 80);
  CHECK(w.rect().h == r0.h + 60);
  CHECK(u.ctx.store().view().panelWindows.at(u.ids.group.value).w == doctest::Approx(w.state().w));
  // Minimise: the title bar only; again: back.
  u.d.click(u.button("min"));
  u.d.step();
  CHECK(w.minimised());
  CHECK(w.rect().h == static_cast<int32_t>(panel::PanelWindow::kTitleH));
  CHECK_FALSE(w.host().visible());
  u.d.click(u.button("min"));
  u.d.step();
  CHECK_FALSE(w.minimised());
  CHECK(w.rect().h == r0.h + 60);
  // Close: hidden; the header button reopens it at its place and size.
  const IRect placed = w.rect();
  u.d.click(u.button("close"));
  u.d.step();
  CHECK(u.window() == nullptr);
  CHECK_FALSE(u.ctx.store().view().panelWindows.at(u.ids.group.value).open);
  u.d.click(u.headerButton());
  u.d.step();
  REQUIRE(u.window() != nullptr);
  CHECK(u.win().rect() == placed);
  // The view state is saved with the project and read back.
  auto text = doc::serializeProject(*u.ctx.doc(), u.ctx.store().view());
  REQUIRE(text.ok());
  auto back = doc::parseProject(*text);
  REQUIRE(back.ok());
  CHECK(back->view.panelWindows.at(u.ids.group.value) == u.win().state());
  // The block menu's Panel Window toggles it too (through the overlay; the menu closes itself).
  REQUIRE(u.view().panelWindowAt(u.centre(u.ids.mix)) == nullptr);
  u.d.click(u.centre(u.ids.mix), mod::kNone, PointerButton::Right);
  u.d.step();
  REQUIRE(u.menu() != nullptr);
  CHECK(u.history() == h0);
}

TEST_CASE("panel window: send to panel — P on a control selected in the circuit, the window's + button, a block dragged "
          "onto the window, an output's cable dropped on it (a mod source); each one "
          "undo step; the control stays where it was and both are the same control") {
  WinUi u;
  SUBCASE("P: a value block clicked, then P — a knob on the panel bound to it; the window opens on it, selected") {
    const Vec2 before = u.centre(u.ids.level);
    const size_t h0 = u.history();
    const uint32_t id = u.sendLevel();
    CHECK(u.history() == h0 + 1);
    CHECK(u.structural == 0);  // a panel edit is Meta: no recompile
    const doc::PanelElement* e = u.panel()->find(id);
    REQUIRE(e != nullptr);
    CHECK(e->kind == "knob");
    REQUIRE(u.window() != nullptr);
    CHECK(u.win().selection() == std::vector<uint32_t>{id});
    // The block stays where it was, and the knob is the same control: turned in the window (play mode), Level moves.
    CHECK(u.centre(u.ids.level).x == doctest::Approx(before.x));
    CHECK(u.centre(u.ids.level).y == doctest::Approx(before.y));
    u.open();
    u.d.click(u.button("mode"));  // → play
    u.d.step();
    REQUIRE_FALSE(u.win().editMode());
    const double p0 = u.position(u.ids.level);
    const Vec2 c = u.atId(id);
    u.d.drag(c, {c.x, c.y + 50.0f}, 5);
    u.d.step();
    CHECK(u.position(u.ids.level) < p0 - 0.1);
    CHECK(u.history() == h0 + 2);
    REQUIRE(u.ctx.undo());
    REQUIRE(u.ctx.undo());
    u.d.step();
    CHECK(u.panel() == nullptr);
  }
  SUBCASE("the window's + sends the selected control there; a second one goes beside the first") {
    u.open();
    u.d.click(u.centre(u.ids.cutoff));
    u.d.step();
    const size_t h0 = u.history();
    u.d.click(u.button("send"));
    u.d.step();
    CHECK(u.history() == h0 + 1);
    const doc::PanelElement* a = byBinding(u.panel(), doc::controlBinding(u.ids.cutoff, std::string(doc::kValueParam)));
    REQUIRE(a != nullptr);
    u.d.click(u.centre(u.ids.drive));
    u.d.step();
    u.d.click(u.button("send"));
    u.d.step();
    const doc::PanelElement* b = byBinding(u.panel(), doc::controlBinding(u.ids.drive, std::string(doc::kValueParam)));
    REQUIRE(b != nullptr);
    a = byBinding(u.panel(), doc::controlBinding(u.ids.cutoff, std::string(doc::kValueParam)));
    CHECK_FALSE((a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h));
    CHECK(u.history() == h0 + 2);
  }
  SUBCASE("a value block dragged onto the window: its control there (at the drop), the block back in place") {
    panel::PanelWindow& w = u.open();
    // (GU.5b: a value block is drawn as its knob: it is moved by its label strip, above the dial)
    const float mx0 = u.node(u.ids.mix).x, my0 = u.node(u.ids.mix).y, mw = u.node(u.ids.mix).w;
    const Vec2 from = u.view().canvas().toDevice({mx0 + mw * 0.5f, my0 + 4.0f});
    const IRect area = w.host().rect();
    const Vec2 drop{static_cast<float>(area.x) + 70.0f, static_cast<float>(area.y) + 50.0f};
    const size_t h0 = u.history();
    u.d.down(from);
    for (int k = 1; k <= 8; ++k) u.d.move({from.x + (drop.x - from.x) * static_cast<float>(k) / 8.0f, from.y + (drop.y - from.y) * static_cast<float>(k) / 8.0f});
    CHECK(w.dropTarget());  // highlighted while the block is over it
    u.d.up(drop);
    u.d.step();
    CHECK_FALSE(w.dropTarget());
    CHECK(u.history() == h0 + 1);
    const doc::PanelElement* e = byBinding(u.panel(), doc::controlBinding(u.ids.mix, std::string(doc::kValueParam)));
    REQUIRE(e != nullptr);
    Vec2 p{0, 0};
    REQUIRE(w.panelPoint(drop, p));
    CHECK(std::fabs(e->x + e->w * 0.5f - p.x) <= 1.0f);
    CHECK(std::fabs(e->y + e->h * 0.5f - p.y) <= 1.0f);
    CHECK(u.node(u.ids.mix).x == doctest::Approx(mx0));  // not moved
    CHECK(u.node(u.ids.mix).y == doctest::Approx(my0));
    CHECK(doc::findDevice(*u.ctx.doc(), u.ids.mix).device->group == u.ids.group);
  }
  SUBCASE("an output's cable dropped on the window: a mod source bound to that output") {
    panel::PanelWindow& w = u.open();
    const Node& n = u.node(u.ids.drive);
    const int32_t out = n.output("y");
    REQUIRE(out >= 0);
    const Vec2 pin = u.view().canvas().toDevice(outPortPoint(n, static_cast<size_t>(out)));
    const Vec2 drop = mid(w.host().rect());
    const size_t h0 = u.history();
    u.d.drag(pin, drop, 8);
    u.d.step();
    CHECK(u.history() == h0 + 1);
    const doc::PanelElement* e = byBinding(u.panel(), doc::signalBinding(u.ids.drive, "y"));
    REQUIRE(e != nullptr);
    CHECK(e->kind == "modsource");
  }
  SUBCASE("a control nested one group deeper (the Wavetable's face knob) goes to the outer group's panel") {
    // The Wavetable inside the group draws its face; a press on one of its knobs selects that control.
    const Node& wt = u.node(u.ids.pa);
    REQUIRE(wt.face != nullptr);
    MetaFace* f = u.view().canvas().faceOn(u.view().scene().sceneNode[u.view().patch().nodeOf(u.ids.pa)]);
    REQUIRE(f != nullptr);
    std::optional<ControlAnchor> a;
    std::string param;
    for (const meta::ParamSpec& ps : f->metaType()->params)
      if (!a)
        if ((a = f->controlAnchor(ps.name))) param = std::string(ps.name);
    REQUIRE(a.has_value());
    const Vec2 knob = u.view().canvas().toDevice({wt.x + a->at.x * wt.scale, wt.y + a->at.y * wt.scale});
    u.d.click(knob);
    u.d.step();
    CHECK(u.view().selectedControl() == doc::controlBinding(u.ids.pa, param));
    CHECK(u.d.press("p") != KeyResult::Unhandled);
    u.d.step();
    CHECK(byBinding(u.panel(), doc::controlBinding(u.ids.pa, param)) != nullptr);
  }
}

TEST_CASE("panel window: edit mode — click selects, a drag moves (one undo step), edges snap to a neighbour within 4 px at "
          "zoom 1 with a guide, Alt moves freely, a handle resizes, Shift+click and a frame select several, Align from the "
          "right-click menu (through the overlay), Delete removes and undo restores") {
  WinUi u;
  const uint32_t a = u.sendLevel();
  u.d.click(u.centre(u.ids.cutoff));
  u.d.step();
  u.view().sendSelectionToPanel();
  u.d.step();
  const uint32_t b = u.panel()->elements.back().id;
  panel::PanelWindow& w = u.open(1060.0f, 40.0f, 700.0f, 420.0f);
  REQUIRE(w.editMode());
  // Put b 40 units right of a, level with it, to have something to snap to.
  {
    doc::Panel p = *u.panel();
    doc::PanelElement& ea = p.elements[static_cast<size_t>(p.indexOf(a))];
    doc::PanelElement& eb = p.elements[static_cast<size_t>(p.indexOf(b))];
    ea.x = 8, ea.y = 8;
    eb.x = 110, eb.y = 40;  // (its right edge 158: clear of the panel's centre line 150)
    p.width = 300, p.height = 160;
    auto e = doc::ops::setGroupPanel(*u.ctx.doc(), u.ids.group, std::make_shared<doc::Panel>(p));
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    u.d.step();
  }
  u.d.tree().layoutNow();
  const float sc = w.panelTransform().scale;
  SUBCASE("click, move with a snap (and its guide), Alt free; one step each") {
    u.d.click(u.atId(b));
    u.d.step();
    CHECK(w.selection() == std::vector<uint32_t>{b});
    const size_t h0 = u.history();
    // Move b up by 30 units and 2 units further: its top lands 2 units below a's top (y 8) → snapped onto it.
    const Vec2 from = u.atId(b);
    u.d.down(from);
    u.d.move({from.x, from.y - 10.0f * sc});
    u.d.move({from.x, from.y - 30.0f * sc});
    CHECK(w.previewing());
    CHECK_FALSE(w.guidesY().empty());  // the guide of the snap shows while dragging
    CHECK(u.history() == h0);           // nothing written while dragging
    u.d.up({from.x, from.y - 30.0f * sc});
    u.d.step();
    CHECK(u.history() == h0 + 1);
    CHECK(u.panel()->find(b)->y == doctest::Approx(8.0f));
    CHECK(u.panel()->find(b)->x == doctest::Approx(110.0f));
    // Alt: no snap — exactly the drag (3 units right and down).
    const Vec2 f2 = u.atId(b);
    u.d.down(f2);
    u.d.move({f2.x + 1.5f * sc, f2.y + 1.5f * sc}, mod::kAlt);
    u.d.move({f2.x + 3.0f * sc, f2.y + 3.0f * sc}, mod::kAlt);
    CHECK(w.guidesX().empty());
    CHECK(w.guidesY().empty());
    u.d.up({f2.x + 3.0f * sc, f2.y + 3.0f * sc}, PointerButton::Left, mod::kAlt);
    u.d.step();
    CHECK(u.panel()->find(b)->x == doctest::Approx(113.0f).epsilon(1e-3));
    CHECK(u.panel()->find(b)->y == doctest::Approx(11.0f).epsilon(1e-3));
    CHECK(u.history() == h0 + 2);
    REQUIRE(u.ctx.undo());
    u.d.step();
    CHECK(u.panel()->find(b)->y == doctest::Approx(8.0f));
  }
  SUBCASE("a handle resizes (bottom-right), snapping to the panel's edge") {
    u.d.click(u.atId(b));
    u.d.step();
    const doc::PanelElement e0 = *u.panel()->find(b);
    const Vec2 br = w.panelTransform().map({e0.x + e0.w, e0.y + e0.h});
    const size_t h0 = u.history();
    u.d.drag(br, {br.x + 20.0f * sc, br.y + 11.0f * sc}, 4);
    u.d.step();
    CHECK(u.history() == h0 + 1);
    CHECK(u.panel()->find(b)->w == doctest::Approx(e0.w + 20.0f));
    CHECK(u.panel()->find(b)->h == doctest::Approx(e0.h + 11.0f));
    CHECK(u.panel()->find(b)->x == doctest::Approx(e0.x));
  }
  SUBCASE("Shift+click adds; Align Top from the right-click menu; Delete removes both, undo brings them back") {
    u.d.click(u.atId(a));
    u.d.step();
    u.d.click(u.atId(b), mod::kShift);
    u.d.step();
    CHECK(w.selection().size() == 2);
    const size_t h0 = u.history();
    u.d.click(u.atId(b), mod::kNone, PointerButton::Right);
    u.d.step();
    REQUIRE(u.menu() != nullptr);
    u.choose("Align");  // the submenu opens
    u.choose("Top");
    CHECK(u.menu() == nullptr);
    CHECK(u.history() == h0 + 1);
    CHECK(u.panel()->find(a)->y == doctest::Approx(u.panel()->find(b)->y));
    u.d.tree().setFocus(w.find("editor"));
    CHECK(u.d.press("Delete") != KeyResult::Unhandled);
    u.d.step();
    CHECK(u.history() == h0 + 2);
    CHECK(u.panel() == nullptr);  // the last elements gone: no panel
    REQUIRE(u.ctx.undo());
    u.d.step();
    REQUIRE(u.panel() != nullptr);
    CHECK(u.panel()->elements.size() == 2);
  }
  SUBCASE("a frame selects what it touches") {
    const Vec2 o = w.panelTransform().map({2.0f, 2.0f}), e = w.panelTransform().map({160.0f, 150.0f});
    u.d.drag(o, e, 5);
    u.d.step();
    CHECK(w.selection().size() == 2);
  }
}

TEST_CASE("panel window: the inspector — the binding from its menu (a submenu per block), a style choice and a style "
          "number (one step at the release), the visibility condition, the curve through its curve editor; each one "
          "undo step, never a recompile but the curve's class change") {
  WinUi u;
  const uint32_t k = u.sendLevel();
  panel::PanelWindow& w = u.open(1000.0f, 30.0f, 760.0f, 600.0f);
  REQUIRE(w.editMode());
  u.d.click(u.atId(k));
  u.d.step();
  u.d.step();
  CHECK(w.selection() == std::vector<uint32_t>{k});
  SUBCASE("binding") {
    const size_t h0 = u.history();
    u.d.click(mid(u.inspectorRow("row:bind0", "bind")->rect()));
    u.d.step();
    REQUIRE(u.menu() != nullptr);
    u.choose("Mix");  // the block's submenu
    Menu* sub = u.menu();
    REQUIRE(sub != nullptr);
    int32_t item = -1;
    for (size_t i = 0; i < sub->items().size(); ++i)
      if (sub->items()[i].label.find("Output") == std::string::npos && !sub->items()[i].separator) item = static_cast<int32_t>(i);
    REQUIRE(item >= 0);
    u.d.click(mid(sub->itemRect(item)));
    u.d.step();
    CHECK(u.menu() == nullptr);
    CHECK(u.history() == h0 + 1);
    REQUIRE(u.panel()->find(k)->binding() != nullptr);
    CHECK(u.panel()->find(k)->binding()->device == u.ids.mix);
    CHECK(u.structural == 0);
  }
  SUBCASE("style: the knob's pointer (a choice) and its track (a number)") {
    const size_t h0 = u.history();
    u.d.click(mid(u.inspectorRow("row:style:pointer", "style:pointer")->rect()));
    u.d.step();
    u.choose("pie");
    CHECK(u.history() == h0 + 1);
    CHECK(u.panel()->find(k)->text("pointer", "") == "pie");
    u.d.step();
    const Vec2 c = mid(u.inspectorRow("row:style:track", "style:track")->rect());
    u.d.drag(c, {c.x, c.y - 40.0f}, 5);
    u.d.step();
    CHECK(u.history() == h0 + 2);  // one step for the whole drag
    CHECK(u.panel()->find(k)->number("track", 3.0) > 3.0);
    CHECK(u.structural == 0);
  }
  SUBCASE("visibility: shown while a tabs element is on its second tab") {
    // A tabs element first (sent as a label kind changed in the inspector would do; built here for brevity).
    doc::Panel p = *u.panel();
    doc::PanelElement t;
    t.id = p.nextId();
    t.kind = "tabs";
    t.x = 120, t.y = 8, t.w = 100, t.h = 16;
    t.setField(doc::styleText("items", "Osc|Env"));
    p.elements.push_back(t);
    p.width = std::max(p.width, 240.0f);
    auto e = doc::ops::setGroupPanel(*u.ctx.doc(), u.ids.group, std::make_shared<doc::Panel>(p));
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    u.d.step();
    u.d.step();
    const size_t h0 = u.history();
    u.d.click(mid(u.inspectorRow("row:when", "when")->rect()));
    u.d.step();
    u.choose("Tabs " + std::to_string(t.id) + " = Env");
    CHECK(u.history() == h0 + 1);
    REQUIRE(u.panel()->find(k)->when.has_value());
    CHECK(u.panel()->find(k)->when->element == t.id);
    CHECK(u.panel()->find(k)->when->values == std::vector<double>{1.0});
    CHECK_FALSE(u.win().face().shown(static_cast<size_t>(u.win().face().indexOf(k))));  // tab 0 is showing
  }
  SUBCASE("curve: the inspector's curve editor edits Level's curve (a point dragged: its class kept, no recompile)") {
    auto* host = dynamic_cast<panel::PanelHost*>(u.win().find("inspector-scroll/inspector/curve"));
    REQUIRE(host != nullptr);
    const dsp::Curve* c0 = u.dev(u.ids.level).curve(doc::kValueParam);
    const dsp::Curve before = c0 != nullptr ? *c0 : dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 1.0);
    const size_t h0 = u.history();
    const auto& ce = static_cast<panel::GroupPanelFace&>(host->face()).element(0).desc();
    const Vec2 end = host->transform().map({ce.x + ce.w - 6.0f, ce.y + 6.0f});
    u.d.drag(end, {end.x, end.y + 25.0f}, 4);
    u.d.step();
    const dsp::Curve* c1 = u.dev(u.ids.level).curve(doc::kValueParam);
    REQUIRE(c1 != nullptr);
    CHECK(c1->points[c1->segments].y < before.points[before.segments].y);
    CHECK(u.history() == h0 + 1);
    CHECK(u.structural == 0);
  }
}

TEST_CASE("panel window: play mode — a mod source dragged onto a knob makes the real cable (one undo step) and the knob "
          "shows its modulation ring; link lines from the elements to their controls in the circuit (the hovered "
          "element, every one while the panel is active)") {
  WinUi u;
  u.sendLevel();
  // Drive's output dropped on the window: a mod source.
  panel::PanelWindow& w = u.open(1060.0f, 40.0f, 600.0f, 360.0f);
  {
    const Node& n = u.node(u.ids.drive);
    const Vec2 pin = u.view().canvas().toDevice(outPortPoint(n, static_cast<size_t>(n.output("y"))));
    u.d.drag(pin, mid(w.host().rect()), 8);
    u.d.step();
  }
  const doc::PanelElement* src = byBinding(u.panel(), doc::signalBinding(u.ids.drive, "y"));
  const doc::PanelElement* knob = byBinding(u.panel(), doc::controlBinding(u.ids.level, std::string(doc::kValueParam)));
  REQUIRE(src != nullptr);
  REQUIRE(knob != nullptr);
  const uint32_t srcId = src->id, knobId = knob->id;
  u.d.click(u.button("mode"));  // play
  u.d.step();
  REQUIRE_FALSE(w.editMode());
  panel::GroupPanelFace& f = w.face();
  CHECK_FALSE(f.target().driven(*f.element(static_cast<size_t>(f.indexOf(knobId))).binding()));
  const size_t h0 = u.history();
  const Vec2 a = u.atId(srcId, 0.5f, 0.35f), b = u.atId(knobId);
  u.d.down(a);
  for (int s = 1; s <= 6; ++s) u.d.move({a.x + (b.x - a.x) * static_cast<float>(s) / 6.0f, a.y + (b.y - a.y) * static_cast<float>(s) / 6.0f});
  CHECK(f.cableDragging());
  CHECK(f.cableTarget() == f.indexOf(knobId));
  u.d.up(b);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  bool wired = false;
  for (const doc::Wire& x : doc::wiresOf(*u.ctx.doc()))
    wired = wired || (x.from == u.ids.drive && x.to == u.ids.level && x.param == doc::kValueParam);  // (y: the main output)
  CHECK(wired);
  CHECK(w.face().target().driven(*w.face().element(static_cast<size_t>(w.face().indexOf(knobId))).binding()));
  // Link lines: none while nothing in the window is hovered and the panel is not active …
  u.d.click(u.view().canvas().toDevice({u.view().patch().x0 + 2.0f, u.view().patch().y0 + 2.0f}));
  u.d.step();
  CHECK_FALSE(w.active());
  u.d.move({static_cast<float>(w.rect().x) - 30.0f, static_cast<float>(w.rect().y) + 50.0f});
  u.d.step();
  CHECK(u.view().panelLinks().empty());
  // … the hovered element's alone, ending on its control in the circuit (Level's block) …
  u.d.move(u.atId(knobId));
  u.d.step();
  std::vector<ProjectPatchView::PanelLink> links = u.view().panelLinks();
  REQUIRE(links.size() == 1);
  CHECK(links[0].element == knobId);
  const std::optional<graph::Point2> c = u.view().controlWorld(doc::controlBinding(u.ids.level, std::string(doc::kValueParam)));
  REQUIRE(c.has_value());
  const Vec2 cd = u.view().canvas().toDevice(*c);
  CHECK(links[0].to.x == doctest::Approx(cd.x));
  CHECK(links[0].to.y == doctest::Approx(cd.y));
  CHECK(links[0].from.x == doctest::Approx(u.atId(knobId).x).epsilon(0.02));
  // … every element's while the window is active (a press in it).
  u.d.click({static_cast<float>(w.rect().x) + 60.0f, static_cast<float>(w.rect().y) + 8.0f});
  u.d.step();
  CHECK(w.active());
  u.d.move({static_cast<float>(w.rect().x) - 30.0f, static_cast<float>(w.rect().y) + 50.0f});
  u.d.step();
  CHECK(u.view().panelLinks().size() == 2);
}

TEST_CASE("panel window: a closed group drawn as its panel — a wire into a control inside it ends on the element bound "
          "to that control (no pin), and a cable dropped on that element drives the control inside") {
  WinUi u(true);  // the panel project's own panel
  u.view().setGroupOpen(u.ids.group, false);
  u.d.tree().layoutNow();
  u.d.step();
  // A free LFO-like source: Drive's output wired into Cutoff (both inside); then Level from outside — a wire across the
  // group's edge into a control its panel shows.
  const Node& g = u.node(u.ids.group);
  REQUIRE(g.groupPanel);
  {
    auto e = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.ids.keys, "Value", doc::DeviceKind::AudioEffect, doc::ops::kOffPath);
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    const doc::DeviceId src = e->devices.at(0);
    auto w = doc::ops::connectWire(*u.ctx.doc(), u.ctx.store().ids(), src, "y", u.ids.level, std::string(doc::kValueParam));
    REQUIRE(w.ok());
    REQUIRE(u.ctx.store().dispatch(w->command).ok());
    u.d.step();
  }
  const Node& n = u.node(u.ids.group);
  const std::string key = std::string(doc::kValueParam) + "@" + std::to_string(u.ids.level.value);
  const int32_t in = n.input(key);
  REQUIRE(in >= 0);
  CHECK(n.inputs[static_cast<size_t>(in)].anchored);  // no pin: it ends on the knob
  // It ends on an element of the panel bound to Level.
  const Port& p = n.inputs[static_cast<size_t>(in)];
  bool onIt = false;
  for (const doc::PanelElement& e : u.panel()->elements)
    if (e.binding() != nullptr && e.binding()->device == u.ids.level)
      onIt = onIt || (p.ax >= e.x && p.ax <= e.x + e.w && p.ay >= e.y && p.ay <= e.y + e.h);
  CHECK(onIt);
  // A cable from Mix's… (outside: a new value block) dropped on the Drive knob of the closed panel drives Drive.
  const doc::PanelElement* dk = nullptr;
  for (const doc::PanelElement& e : u.panel()->elements)
    if (e.binding() != nullptr && e.binding()->device == u.ids.drive && e.kind == "knob") dk = &e;
  if (dk != nullptr) {
    const ProjectPatchView::WireTarget t =
        u.view().wireTargetAt(u.view().canvas().toDevice({n.x + dk->x + dk->w * 0.5f, n.y + dk->y + dk->h * 0.5f}));
    CHECK(t.device == u.ids.drive);
    CHECK(t.param == doc::kValueParam);
  }
}

TEST_CASE("panel window: a group deleted takes its window away (removed at the next frame); undo brings both back") {
  WinUi u;
  u.sendLevel();
  REQUIRE(u.window() != nullptr);
  auto e = doc::ops::deleteDevices(*u.ctx.doc(), std::vector<doc::DeviceId>{u.ids.group});
  REQUIRE(e.ok());
  REQUIRE(u.ctx.store().dispatch(e->command).ok());
  u.d.step();
  u.d.step();
  CHECK(u.window() == nullptr);
  REQUIRE(u.ctx.undo());
  u.d.step();
  u.d.tree().layoutNow();
  CHECK(u.window() != nullptr);
}

TEST_CASE("render: panel-window-closed, -open, -edit (a selection and the snap guides mid-drag), -inspector, -links") {
  WinUi u;
  checkImage("panel-window-closed", u.render());  // the capsule with its header button, no window
  u.sendLevel();
  u.d.click(u.centre(u.ids.cutoff));
  u.d.step();
  u.view().sendSelectionToPanel();
  u.d.step();
  panel::PanelWindow& w = u.open(1060.0f, 40.0f, 640.0f, 400.0f);
  checkImage("panel-window-inspector", u.render());  // edit mode: the newest element selected, its inspector
  u.d.click(u.button("mode"));
  u.d.step();
  u.d.move(u.at(0));
  checkImage("panel-window-links", u.render());  // play mode, the knob hovered: its link line into the circuit
  u.d.click({static_cast<float>(w.rect().x) + 60.0f, static_cast<float>(w.rect().y) + 8.0f});  // active: every line
  u.d.move({static_cast<float>(w.rect().x) - 40.0f, static_cast<float>(w.rect().y) + 200.0f});
  checkImage("panel-window-open", u.render());
  u.d.click(u.button("mode"));  // edit again; drag the second element near the first's top: a guide
  u.d.step();
  const float sc = w.panelTransform().scale;
  const doc::PanelElement e0 = u.panel()->elements[0], e1 = u.panel()->elements[1];
  const Vec2 from = u.atId(e1.id);
  u.d.down(from);
  u.d.move({from.x + 5.0f * sc, from.y + (e0.y - e1.y + 2.0f) * sc * 0.5f});
  u.d.move({from.x + 10.0f * sc, from.y + (e0.y - e1.y + 2.0f) * sc});
  REQUIRE(w.previewing());
  checkImage("panel-window-edit", u.render());
  u.d.up({from.x + 10.0f * sc, from.y + (e0.y - e1.y + 2.0f) * sc});
}
