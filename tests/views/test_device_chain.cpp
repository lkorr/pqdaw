// T4.1 the Device view through the real shell: the selected track's chain left → right, one frame per block of its
// main path (GU.11: a group — a module — shows its panel, a leaf its generated DevicePanel); add (the device menu), reorder (dragging a title bar), remove (Delete), deactivate (`0`), copy / paste /
// duplicate (keys), a param drag and a structural const — each exactly one undo step; a param change from elsewhere
// repaints only its control. Golden: device-chain.

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/racks.h"
#include "pq/ui/controls.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + static_cast<float>(r.w) / 2.0f, static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f}; }

// The shell over a project with one MIDI track (the default instrument), shown in the Device view.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{2200, 800};  // (GU.11: the default instrument group's frame is wide: the + slot in view)
  std::unique_ptr<Shell> shell;
  doc::TrackId keys, grp;

  Ui() {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    auto run = [&](const doc::ops::EditResult& e) {
      REQUIRE(e.ok());
      REQUIRE(st.dispatch(e->command).ok());
      return *e;
    };
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks[0];
    grp = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Group, {}, "Bus")).tracks[0];
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(300.0f);
    ctx.setSelectedTrack(keys);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
    d.tree().layoutNow();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  DeviceChainView& view() const {
    DeviceChainView* v = findDeviceChain(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const doc::Track& track(doc::TrackId id) const { return *doc::findTrackPtr(*ctx.doc(), id); }
  // TX.all-free: the track's main path (what the Device view lists), in signal order.
  std::vector<std::string> chain(doc::TrackId id) const {
    std::vector<std::string> v;
    for (const doc::Device* dev : doc::mainPath(*ctx.doc(), track(id)).blocks) v.push_back(dev->name + (dev->active ? "" : "(off)"));
    return v;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  // TX.all-free: the block at `slot` of the main path.
  doc::DeviceId onPath(doc::TrackId id, size_t slot) const { return doc::mainPath(*ctx.doc(), track(id)).blocks.at(slot)->id; }
  // GU.11: the element of group `group`'s panel (its frame) bound to block `control`'s value: its rect, device px.
  IRect element(doc::DeviceId group, doc::DeviceId control) {
    d.tree().layoutNow();
    Widget* f = view().frameOf(group);
    REQUIRE(f != nullptr);
    auto* host = dynamic_cast<panel::PanelHost*>(f->find("panel[" + std::to_string(group.value) + "]"));
    REQUIRE(host != nullptr);
    auto& face = static_cast<panel::GroupPanelFace&>(host->face());
    for (size_t i = 0; i < face.elementCount(); ++i)
      if (const doc::Binding* b = face.element(i).binding(); b != nullptr && b->device == control && b->name == doc::kValueParam) {
        const doc::PanelElement& e = face.element(i).desc();
        const Vec2 a = host->transform().map({e.x, e.y}), z = host->transform().map({e.x + e.w, e.y + e.h});
        return {static_cast<int32_t>(a.x), static_cast<int32_t>(a.y), static_cast<int32_t>(z.x - a.x), static_cast<int32_t>(z.y - a.y)};
      }
    FAIL("no element bound to the control");
    return {};
  }
  IRect frame(doc::DeviceId id) {
    d.tree().layoutNow();
    Widget* f = view().frameOf(id);
    REQUIRE(f != nullptr);
    return f->rect();
  }
  // Chooses the device menu item labelled `name` (the + slot's menu).
  void addFromMenu(const std::string& name) {
    Widget* slot = view().addSlot();
    REQUIRE(slot != nullptr);
    d.tree().layoutNow();
    d.click(mid(slot->rect()));
    Menu* menu = nullptr;
    for (size_t i = 0; i < shell->overlay().childCount() && !menu; ++i) menu = dynamic_cast<Menu*>(&shell->overlay().child(i));
    REQUIRE(menu != nullptr);
    for (int32_t i = 0; i < static_cast<int32_t>(menu->items().size()); ++i)
      if (menu->items()[static_cast<size_t>(i)].label == "   " + name) {
        menu->choose(i);
        d.step();
        return;
      }
    FAIL("no menu item " << name);
  }
};

}  // namespace

TEST_CASE("device view: shows the selected track's chain, one frame per block — a group's panel, a leaf's DevicePanel") {
  Ui u;
  DeviceChainView& v = u.view();
  CHECK(v.track() == u.keys);
  // The default instrument: a group (GU.10) — its frame shows its panel / mini circuit, no DevicePanel of its own.
  const doc::DeviceId inst = u.onPath(u.keys, 0);
  REQUIRE(doc::isGroupDevice(*doc::findDevice(*u.ctx.doc(), inst).device));
  REQUIRE(v.frameOf(inst) != nullptr);
  CHECK(v.panelOf(inst) == nullptr);
  // A module from the menu: its group's panel (elements bound to its value blocks).
  u.addFromMenu("Utility");
  const doc::DeviceId util = u.onPath(u.keys, 1);
  REQUIRE(v.frameOf(util) != nullptr);
  CHECK(v.panelOf(util) == nullptr);
  CHECK(u.element(util, test_blocks::member(*u.ctx.doc(), util, "Width")).w > 0);
  // A leaf (an atom block): the generated body, one control per param and setting (GU.11 autoPanel).
  auto e = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.keys, "atom:gain", doc::DeviceKind::AudioEffect);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
  REQUIRE(u.ctx.store().dispatch(e->command).ok());
  const doc::DeviceId atom = e->devices.at(0);
  devices::DevicePanel* p = v.panelOf(atom);
  REQUIRE(p != nullptr);
  CHECK(doc::findDevice(*u.ctx.doc(), atom).device->patch == "atom:gain");
  CHECK_FALSE(p->desc().fitFace);
  for (const devices::ParamInfo& pi : p->signature().params) CHECK_MESSAGE(p->controlFor(pi.name) != nullptr, pi.name);
  for (const devices::ConstInfo& ci : p->signature().consts) CHECK_MESSAGE(p->control(ci.name) != nullptr, ci.name);
  // Selecting another track shows its (empty) chain.
  u.ctx.setSelectedTrack(u.grp);
  CHECK(v.track() == u.grp);
  CHECK(v.frameOf(inst) == nullptr);
}

TEST_CASE("device view: add, reorder, remove, deactivate, copy / paste and duplicate are one undo step each") {
  Ui u;
  DeviceChainView& v = u.view();
  const size_t h0 = u.history();
  // Add from the device menu (after the selected device, else at the end).
  u.addFromMenu("Utility");
  CHECK(u.history() == h0 + 1);
  u.addFromMenu("Saturator");
  CHECK(u.history() == h0 + 2);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"Basic Synth", "Utility", "Saturator"});
  const doc::DeviceId util = u.onPath(u.keys, 1), sat = u.onPath(u.keys, 2);
  CHECK(v.selection().size() == 1);
  CHECK(v.selection()[0] == sat);  // the new device is selected

  // Reorder: drag Saturator's title bar left of Utility's.
  const IRect fs = u.frame(sat), fu = u.frame(util);
  u.d.drag({static_cast<float>(fs.x + fs.w / 2), static_cast<float>(fs.y + 18)}, {static_cast<float>(fu.x + 10), static_cast<float>(fu.y + 18)}, 8);
  CHECK(u.history() == h0 + 3);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"Basic Synth", "Saturator", "Utility"});
  // TX.all-free: the path has no order rule — dragged to the far left, the Utility goes before the instrument (one
  // step; undone here).
  const IRect fu2 = u.frame(util), fi = u.frame(u.onPath(u.keys, 0));
  u.d.drag({static_cast<float>(fu2.x + fu2.w / 2), static_cast<float>(fu2.y + 18)}, {static_cast<float>(fi.x + 2), static_cast<float>(fi.y + 18)}, 8);
  CHECK(u.history() == h0 + 4);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"Utility", "Basic Synth", "Saturator"});
  REQUIRE(u.ctx.undo());
  CHECK(u.chain(u.keys) == std::vector<std::string>{"Basic Synth", "Saturator", "Utility"});
  // Onto the Saturator's left half: between the instrument and the Saturator.
  const IRect fu4 = u.frame(util), fs4 = u.frame(sat);
  u.d.drag({static_cast<float>(fu4.x + fu4.w / 2), static_cast<float>(fu4.y + 18)}, {static_cast<float>(fs4.x + 10), static_cast<float>(fs4.y + 18)}, 8);
  CHECK(u.history() == h0 + 4);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"Basic Synth", "Utility", "Saturator"});

  // Deactivate: select Saturator (its title bar), `0`.
  const IRect fs1 = u.frame(sat);
  u.d.click({static_cast<float>(fs1.x + fs1.w / 2), static_cast<float>(fs1.y + 18)});
  REQUIRE(v.selection().size() == 1);
  CHECK(v.selection()[0] == sat);
  CHECK(u.d.press("0") == KeyResult::Action);
  CHECK(u.history() == h0 + 5);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"Basic Synth", "Utility", "Saturator(off)"});
  CHECK_FALSE(doc::findDevice(*u.ctx.doc(), sat).device->active);

  // Keys: select Utility (a click on its title), Ctrl+D duplicates, 0 toggles, Ctrl+C / Ctrl+V on the group, Delete.
  const IRect fu3 = u.frame(util);
  u.d.click({static_cast<float>(fu3.x + fu3.w / 2), static_cast<float>(fu3.y + 18)});
  REQUIRE(v.selection().size() == 1);
  CHECK(v.selection()[0] == util);
  CHECK(u.d.press("Ctrl+D") == KeyResult::Action);
  CHECK(u.history() == h0 + 6);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"Basic Synth", "Utility", "Utility", "Saturator(off)"});
  CHECK(u.d.press("0") == KeyResult::Action);  // the duplicate is selected now
  CHECK(u.history() == h0 + 7);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"Basic Synth", "Utility", "Utility(off)", "Saturator(off)"});
  CHECK(u.d.press("Ctrl+C") == KeyResult::Action);
  u.ctx.setSelectedTrack(u.grp);
  u.d.click(mid(v.rect()));  // focus the view (empty space)
  CHECK(u.d.press("Ctrl+V") == KeyResult::Action);
  CHECK(u.history() == h0 + 8);
  CHECK(u.chain(u.grp) == std::vector<std::string>{"Utility(off)"});
  u.ctx.setSelectedTrack(u.keys);
  const IRect fs2 = u.frame(sat);
  u.d.click({static_cast<float>(fs2.x + fs2.w / 2), static_cast<float>(fs2.y + 18)});
  CHECK(u.d.press("Delete") == KeyResult::Action);
  CHECK(u.history() == h0 + 9);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"Basic Synth", "Utility", "Utility(off)"});

  // Every step undoes on its own, back to the start.
  for (size_t k = 0; k < 9; ++k) REQUIRE(u.ctx.undo());
  CHECK(u.chain(u.keys) == std::vector<std::string>{"Basic Synth"});
  CHECK(u.chain(u.grp).empty());
}

TEST_CASE("device view: a param drag is one undo step of Param changes; a const is one structural step") {
  Ui u;
  u.addFromMenu("Saturator");
  const doc::DeviceId sat = u.onPath(u.keys, 1);
  const doc::DeviceId drive = test_blocks::member(*u.ctx.doc(), sat, "Drive");
  REQUIRE(drive.valid());
  const double v0 = doc::findDevice(*u.ctx.doc(), drive).device->param(doc::kValueParam) != nullptr
                        ? doc::findDevice(*u.ctx.doc(), drive).device->param(doc::kValueParam)->value
                        : 0.5;
  const size_t h0 = u.history();
  // GU.11: the Drive knob of the Saturator's panel (an element bound to its Drive value block).
  const Vec2 c = mid(u.element(sat, drive));
  u.d.drag(c, {c.x, c.y - 40.0f}, 6);
  CHECK(u.history() == h0 + 1);
  const doc::ChangeSet& cs = u.ctx.store().history().entries()[h0].changes;
  CHECK(cs.paramOnly());
  REQUIRE(cs.params.size() == 1);
  CHECK(cs.params[0] == doc::ParamRef::device(drive, std::string(doc::kValueParam)));
  const doc::Device& d = *doc::findDevice(*u.ctx.doc(), drive).device;
  REQUIRE(d.param(doc::kValueParam) != nullptr);
  CHECK(d.param(doc::kValueParam)->value > v0);  // up (GU.1: the param stores the knob position)
  // A structural const through a leaf's panel target (an atom block's setting): one structural step, shown back.
  auto e = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.keys, "atom:gain", doc::DeviceKind::AudioEffect);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
  REQUIRE(u.ctx.store().dispatch(e->command).ok());
  const doc::DeviceId atom = e->devices.at(0);
  const size_t h1 = u.history();
  devices::DeviceTarget* target = u.view().targetOf(atom);
  REQUIRE(target != nullptr);
  REQUIRE(u.view().panelOf(atom) != nullptr);
  const double s0 = u.view().panelOf(atom)->constValue("start");
  target->setConst("start", s0 == 1.0 ? 0.0 : 1.0);
  CHECK(u.history() == h1 + 1);
  CHECK(u.ctx.store().history().entries()[h1].changes.structural());
  REQUIRE(doc::findDevice(*u.ctx.doc(), atom).device->setting("start") != nullptr);
  CHECK(doc::findDevice(*u.ctx.doc(), atom).device->setting("start")->value == (s0 == 1.0 ? 0.0 : 1.0));
  CHECK(u.view().panelOf(atom)->constValue("start") == (s0 == 1.0 ? 0.0 : 1.0));
  REQUIRE(u.ctx.undo());
  CHECK(u.view().panelOf(atom)->constValue("start") == s0);  // undo comes back to the panel
}

TEST_CASE("device view: a param change from elsewhere repaints only its control") {
  // GU.11: on a generic knob — a rack's macro (its frame's DevicePanel), on the Bus track (no group frame beside it: a
  // group frame repaints its whole panel or mini circuit on any document change; a native engine's face is one
  // control).
  Ui u;
  auto add = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.grp, std::string(doc::kAudioEffectRack), doc::DeviceKind::AudioEffect);
  REQUIRE_MESSAGE(add.ok(), (add.ok() ? "" : add.error().message));
  REQUIRE(u.ctx.store().dispatch(add->command).ok());
  const doc::DeviceId rack = add->devices.at(0);
  {  // macro 1 mapped (an unmapped macro has no value): onto a Value block in the rack's first chain, off its series
    const std::optional<doc::RackInfo> ri = doc::rackOf(*u.ctx.doc(), rack);
    REQUIRE(ri.has_value());
    REQUIRE_FALSE(ri->chains.empty());
    auto v = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.grp, std::string(doc::kValueDevice), doc::DeviceKind::AudioEffect,
                                 doc::ops::kOffPath);
    REQUIRE(v.ok());
    REQUIRE(u.ctx.store().dispatch(v->command).ok());
    auto g = doc::ops::setDeviceGroup(*u.ctx.doc(), u.ctx.store().ids(), &library(), v->devices.at(0), ri->chains[0].id);
    REQUIRE_MESSAGE(g.ok(), (g.ok() ? "" : g.error().message));
    REQUIRE(u.ctx.store().dispatch(g->command).ok());
    auto m = doc::ops::mapMacro(*u.ctx.doc(), u.ctx.store().ids(), rack, 0, v->devices.at(0), std::string(doc::kValueParam));
    REQUIRE_MESSAGE(m.ok(), (m.ok() ? "" : m.error().message));
    REQUIRE(u.ctx.store().dispatch(m->command).ok());
  }
  u.ctx.setSelectedTrack(u.grp);
  u.d.tree().layoutNow();
  // (a first change: the shell shows the project changed — the change below is the one measured)
  REQUIRE(u.ctx.store().dispatch(doc::ops::setMacroValue(*u.ctx.doc(), rack, 0, 0.25 * doc::kMacroMax)->command).ok());
  u.d.step();
  u.d.step();  // settle: nothing damaged
  devices::DevicePanel* panel = u.view().panelOf(rack);
  REQUIRE(panel != nullptr);
  ui::Widget* knob = panel->controlFor("macro1");
  REQUIRE(knob != nullptr);
  auto e = doc::ops::setMacroValue(*u.ctx.doc(), rack, 0, 0.75 * doc::kMacroMax);
  REQUIRE(e.ok());
  REQUIRE(u.ctx.store().dispatch(e->command).ok());
  CHECK(panel->paramValue("macro1") == doctest::Approx(0.75 * doc::kMacroMax).epsilon(1e-9));
  IRect rects[Damage::kMaxRects];
  int64_t area = 0;
  const uint32_t n = u.d.tree().damageRects(rects, area);
  REQUIRE(n >= 1);
  const IRect rf = u.view().frameOf(rack)->rect();
  for (uint32_t i = 0; i < n; ++i) {  // inside the control (a knob may damage its value readout with it)
    const IRect& r = rects[i];
    const IRect& w = knob->rect();
    // (GU.9: the rack frame's strip — its left column — follows every edit of what the rack derives from)
    if (r.x >= rf.x && r.right() <= rf.x + 40) continue;
    CAPTURE(r.x);
    CAPTURE(r.y);
    CAPTURE(r.w);
    CAPTURE(r.h);
    CHECK(r.x >= w.x - 40);
    CHECK(r.right() <= w.right() + 40);
    CHECK(r.y >= w.y - 40);
    CHECK(r.bottom() <= w.bottom() + 40);
  }
  CHECK(area < static_cast<int64_t>(panel->rect().w) * panel->rect().h / 4 + 40LL * rf.h);
}

TEST_CASE("render: device-chain matches the golden image") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize("device-chain", w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene("device-chain", {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/device-chain.png";
  auto ref = readPng(path);
  if (!ref) (void)writePng("device-chain.actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden("device-chain", *img, *ref, true, &d));
  MESSAGE("device-chain: " << pq::ui::test::describe(d));
}
