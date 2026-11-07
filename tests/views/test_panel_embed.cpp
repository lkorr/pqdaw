// GU.6 (docs/plans/GU.md "Panels and the panel window" Nesting, "Phases" 6): a child group left whole on its parent's
// panel is part of that GUI — through REAL event dispatch in the project patch view: the child (closed: drawn as its
// panel) clicked and sent with P lands on the parent's panel as an EMBEDDED panel (one element, the child's size, one
// undo step); its knobs in the parent's panel window are the child's controls (a drag edits them, one step); the embed
// survives renames (bindings by DeviceId) and leaves with the child when the child moves out of the parent (the prune
// rule; undo brings both back); BREAK APART (Ctrl+Shift+G in edit mode) replaces it with copies of the child's elements
// at the same places, bound to the same controls (one step; undo restores the embed); a nested embed breaks apart one
// level at a time. Golden: panel-embed (an embedded child panel beside a broken-apart copy of it).
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/image.h"
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

doc::Binding valueOf(doc::DeviceId d) { return doc::controlBinding(d, std::string(doc::kValueParam)); }

// GU.3's panel project ("Synth": a Wavetable, mul, value blocks Level, Cutoff, Drive, Mix) with its panel cleared, and
// Drive + Mix grouped inside it into the child group "Tone" whose 120 × 70 panel holds two knobs (Drive, Mix). With
// `nested`, Mix is grouped once more ("Inner", a 64 × 70 panel: one knob, Mix) and Tone's panel holds Drive's knob and
// Inner's panel embedded. Synth open at zoom 1 (Tone closed: drawn as its panel); panels of stock devices off.
struct EmbedUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1800, 960};
  std::unique_ptr<Shell> shell;
  PanelProjectIds ids;
  doc::DeviceId tone, inner;

  explicit EmbedUi(bool nested = false) {
    ctx.setDeviceLibrary(&devLib());
    REQUIRE(buildPanelProject(ctx.store(), &ids).ok());
    run(doc::ops::setGroupPanel(*ctx.doc(), ids.group, nullptr));
    {
      auto e = doc::ops::groupInCircuit(ctx.doc(), ctx.store().ids(), devLib(), std::vector<doc::DeviceId>{ids.drive, ids.mix});
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
      REQUIRE(ctx.store().dispatch(e->command).ok());
      REQUIRE_FALSE(e->devices.empty());
      tone = e->devices.front();
    }
    REQUIRE(doc::findDevice(*ctx.doc(), tone).device->group == ids.group);
    run(doc::ops::renameDevice(*ctx.doc(), tone, "Tone"));
    doc::Panel p;
    p.width = 120.0f;
    p.height = 70.0f;
    if (nested) {
      auto e = doc::ops::groupInCircuit(ctx.doc(), ctx.store().ids(), devLib(), std::vector<doc::DeviceId>{ids.mix});
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
      REQUIRE(ctx.store().dispatch(e->command).ok());
      inner = e->devices.front();
      REQUIRE(doc::findDevice(*ctx.doc(), inner).device->group == tone);
      run(doc::ops::renameDevice(*ctx.doc(), inner, "Inner"));
      doc::Panel ip;
      ip.width = 64.0f;
      ip.height = 70.0f;
      ip.elements.push_back(element(1, "knob", 8, 5, 48, 60, valueOf(ids.mix)));
      run(doc::ops::setGroupPanel(*ctx.doc(), inner, std::make_shared<doc::Panel>(ip)));
      p.elements.push_back(element(1, "knob", 8, 5, 48, 60, valueOf(ids.drive)));
      p.elements.push_back(element(2, std::string(doc::kEmbedKind), 56, 0, 64, 70, doc::panelBinding(inner)));
    } else {
      p.elements.push_back(element(1, "knob", 8, 5, 48, 60, valueOf(ids.drive)));
      p.elements.push_back(element(2, "knob", 64, 5, 48, 60, valueOf(ids.mix)));
    }
    run(doc::ops::setGroupPanel(*ctx.doc(), tone, std::make_shared<doc::Panel>(p)));
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPanelsShown(false);
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    view().setGroupOpen(ids.group, true);
    d.tree().layoutNow();
    REQUIRE(view().groupOpen(ids.group));
    REQUIRE_FALSE(view().groupOpen(tone));  // a group with a panel is seen closed: its panel
    const Node& n = node(ids.group);
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = n.x - 30.0;
    v.y = n.y - 30.0;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
    d.tree().setFocus(&view().canvas());
  }
  ~EmbedUi() { d.tree().setRoot(std::make_unique<Widget>()); }

  static doc::PanelElement element(uint32_t id, std::string kind, float x, float y, float w, float h, doc::Binding b) {
    doc::PanelElement e;
    e.id = id;
    e.kind = std::move(kind);
    e.x = x, e.y = y, e.w = w, e.h = h;
    e.bindings = {std::move(b)};
    return e;
  }
  void run(const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
    if (!e->empty()) REQUIRE(ctx.store().dispatch(e->command).ok());
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
  const doc::Device& dev(doc::DeviceId id) const { return *doc::findDevice(*ctx.doc(), id).device; }
  const doc::Panel* panel(doc::DeviceId g) const { return dev(g).panel.get(); }
  const doc::Panel* panel() const { return panel(ids.group); }
  size_t history() const { return ctx.store().history().cursor(); }
  double position(doc::DeviceId id) const {
    const doc::DeviceValue* v = dev(id).param(doc::kValueParam);
    return v != nullptr ? v->value : 0.0;
  }
  panel::PanelWindow& win() const {
    panel::PanelWindow* w = view().panelWindow(ids.group);
    REQUIRE(w != nullptr);
    return *w;
  }
  Vec2 button(std::string_view name) const {
    Widget* b = win().find(name);
    REQUIRE(b != nullptr);
    return mid(b->rect());
  }
  // Clicks Tone's panel frame (above its knobs: the block, not a control) and presses P: Tone goes to Synth's panel.
  uint32_t sendTone() {
    const Node& n = node(tone);
    d.click(view().canvas().toDevice({n.x + n.w * 0.5f, n.y + 2.0f}));
    d.step();
    REQUIRE_FALSE(view().selectedControl().bound());
    REQUIRE(view().selectedDevices() == std::vector<doc::DeviceId>{tone});
    CHECK(d.press("p") != KeyResult::Unhandled);
    d.step();
    REQUIRE(panel() != nullptr);
    return panel()->elements.back().id;
  }
  // Puts Synth's window at (x, y), w × h (logical px), in play or edit mode.
  panel::PanelWindow& place(bool edit, float x = 1100.0f, float y = 40.0f, float w = 520.0f, float h = 300.0f) {
    panel::PanelWindow& pw = win();
    doc::PanelWindowView s = pw.state();
    s.x = x, s.y = y, s.w = w, s.h = h;
    s.edit = edit;
    ctx.store().view().panelWindows[ids.group.value] = s;
    pw.setState(s);
    view().invalidateArrange();
    d.tree().layoutNow();
    d.step();
    return pw;
  }
  // A point of Synth's panel (panel units) in device px.
  Vec2 onPanel(float x, float y) const { return win().panelTransform().map({x, y}); }
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

TEST_CASE("embed: a child group sent to its parent's panel is embedded (live: its knobs edit the child's controls); it "
          "survives renames, leaves with the child when the child moves out; break apart puts copies of its elements "
          "bound to the same controls; each one undo step") {
  EmbedUi u;
  const size_t h0 = u.history();
  const uint32_t id = u.sendTone();
  CHECK(u.history() == h0 + 1);
  const doc::PanelElement* e = u.panel()->find(id);
  REQUIRE(e != nullptr);
  CHECK(e->kind == doc::kEmbedKind);
  REQUIRE(e->binding() != nullptr);
  CHECK(*e->binding() == doc::panelBinding(u.tone));
  CHECK(e->w == doctest::Approx(120.0f));  // the child's panel at its own size
  CHECK(e->h == doctest::Approx(70.0f));
  const float ex = e->x, ey = e->y;

  // Play mode: Drive's knob inside the embed is Drive's control.
  u.place(false);
  const double p0 = u.position(u.ids.drive);
  const Vec2 knob = u.onPanel(ex + 8.0f + 24.0f, ey + 5.0f + 30.0f);
  u.d.drag(knob, {knob.x, knob.y - 50.0f}, 5);
  u.d.step();
  CHECK(u.position(u.ids.drive) > p0 + 0.1);
  CHECK(u.history() == h0 + 2);
  // A wire into Drive while Synth is closed ends on the embedded knob (the face finds controls inside an embed).
  CHECK(u.win().face().bindingAnchor(valueOf(u.ids.drive), true).has_value());

  // Renamed (the child and the block): still embedded, still the same control.
  u.run(doc::ops::renameDevice(*u.ctx.doc(), u.tone, "Tone 2"));
  u.run(doc::ops::renameDevice(*u.ctx.doc(), u.ids.drive, "Gain"));
  u.d.step();
  REQUIRE(u.panel() != nullptr);
  REQUIRE(u.panel()->find(id) != nullptr);
  CHECK(u.win().face().target().info(*u.panel()->find(id)->binding()).label == "Tone 2");
  const double p1 = u.position(u.ids.drive);
  u.d.drag(knob, {knob.x, knob.y + 40.0f}, 5);
  u.d.step();
  CHECK(u.position(u.ids.drive) < p1 - 0.1);

  SUBCASE("break apart: copies of the child's two knobs at the same places, bound to Drive and Mix; undo: the embed") {
    u.place(true);
    u.d.click(u.onPanel(ex + 2.0f, ey + 2.0f));  // edit mode: select the embed
    u.d.step();
    REQUIRE(u.win().selection() == std::vector<uint32_t>{id});
    const size_t hb = u.history();
    CHECK(u.d.press("Ctrl+Shift+G") != KeyResult::Unhandled);
    u.d.step();
    CHECK(u.history() == hb + 1);
    REQUIRE(u.panel() != nullptr);
    CHECK(u.panel()->find(id) == nullptr);
    REQUIRE(u.panel()->elements.size() == 2);
    const doc::PanelElement* dk = byBinding(u.panel(), valueOf(u.ids.drive));
    const doc::PanelElement* mk = byBinding(u.panel(), valueOf(u.ids.mix));
    REQUIRE(dk != nullptr);
    REQUIRE(mk != nullptr);
    CHECK(dk->kind == "knob");
    CHECK(dk->x == doctest::Approx(ex + 8.0f));
    CHECK(dk->y == doctest::Approx(ey + 5.0f));
    CHECK(mk->x == doctest::Approx(ex + 64.0f));
    CHECK(u.win().selection().size() == 2);  // the copies selected, ready to be placed
    // Each copy is its own element now: moved alone (one step), the other stays.
    const float mx = mk->x;
    u.d.click(u.onPanel(dk->x + 24.0f, dk->y + 30.0f));
    u.d.step();
    CHECK(u.d.press("Right") != KeyResult::Unhandled);
    u.d.step();
    CHECK(byBinding(u.panel(), valueOf(u.ids.drive))->x == doctest::Approx(ex + 9.0f));
    CHECK(byBinding(u.panel(), valueOf(u.ids.mix))->x == doctest::Approx(mx));
    // The copy plays the same control.
    u.place(false);
    const double q0 = u.position(u.ids.mix);
    const Vec2 m = u.onPanel(mx + 24.0f, ey + 5.0f + 30.0f);
    u.d.drag(m, {m.x, m.y + 50.0f}, 5);
    u.d.step();
    CHECK(u.position(u.ids.mix) < q0 - 0.1);
    REQUIRE(u.ctx.undo());  // the drag
    REQUIRE(u.ctx.undo());  // the nudge
    REQUIRE(u.ctx.undo());  // the break apart
    u.d.step();
    REQUIRE(u.panel() != nullptr);
    CHECK(u.panel()->elements.size() == 1);
    CHECK(u.panel()->find(id) != nullptr);
  }
  SUBCASE("the child moved out of the group takes its embed off (one step); undo brings both back") {
    const size_t hm = u.history();
    u.run(doc::ops::setDeviceGroup(*u.ctx.doc(), u.ctx.store().ids(), &devLib(), u.tone, {}));
    u.d.step();
    CHECK(u.history() == hm + 1);
    CHECK(u.panel() == nullptr);  // its only element gone: no panel
    REQUIRE(u.ctx.undo());
    u.d.step();
    REQUIRE(u.panel() != nullptr);
    CHECK(u.panel()->find(id) != nullptr);
  }
}

TEST_CASE("embed: any depth — a child whose panel embeds a grandchild's; the grandchild's knob plays through both; break "
          "apart goes one level at a time") {
  EmbedUi u(true);
  const uint32_t id = u.sendTone();
  const doc::PanelElement e = *u.panel()->find(id);
  u.place(false);
  // Mix's knob: in Inner's panel (at 56, 0 of Tone's), in Tone's (at e.x, e.y of Synth's).
  const double p0 = u.position(u.ids.mix);
  const Vec2 knob = u.onPanel(e.x + 56.0f + 8.0f + 24.0f, e.y + 5.0f + 30.0f);
  u.d.drag(knob, {knob.x, knob.y + 50.0f}, 5);
  u.d.step();
  CHECK(u.position(u.ids.mix) < p0 - 0.1);
  // One level: Drive's knob and Inner's panel, embedded still.
  u.place(true);
  u.win().setSelection({id});
  REQUIRE(u.win().breakApartSelected());
  u.d.step();
  REQUIRE(u.panel()->elements.size() == 2);
  REQUIRE(byBinding(u.panel(), valueOf(u.ids.drive)) != nullptr);
  const doc::PanelElement* in = byBinding(u.panel(), doc::panelBinding(u.inner));
  REQUIRE(in != nullptr);
  CHECK(in->kind == doc::kEmbedKind);
  CHECK(in->x == doctest::Approx(e.x + 56.0f));
  // The next level: Mix's knob.
  u.win().setSelection({in->id});
  REQUIRE(u.win().breakApartSelected());
  u.d.step();
  REQUIRE(u.panel()->elements.size() == 2);
  const doc::PanelElement* mk = byBinding(u.panel(), valueOf(u.ids.mix));
  REQUIRE(mk != nullptr);
  CHECK(mk->x == doctest::Approx(e.x + 56.0f + 8.0f));
  CHECK(mk->y == doctest::Approx(e.y + 5.0f));
}

TEST_CASE("render: panel-embed (Tone embedded in Synth's panel window beside a broken-apart copy of it)") {
  EmbedUi u;
  const uint32_t a = u.sendTone();
  const uint32_t b = u.sendTone();  // (a second view of the same child)
  REQUIRE(a != b);
  u.place(true);
  u.win().setSelection({b});
  REQUIRE(u.win().breakApartSelected());
  u.win().setSelection({});
  u.place(false);
  u.d.move({100.0f, 900.0f});
  checkImage("panel-embed", u.render());
}
