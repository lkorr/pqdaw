// T4.1 the Device view through the real shell: the selected track's chain left → right, one T4.6 device panel per
// device; add (the device menu), reorder (dragging a title bar), remove (Delete), deactivate (`0`), copy / paste /
// duplicate (keys), a param drag and a structural const — each exactly one undo step; a param change from elsewhere
// repaints only its control. Golden: device-chain.

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
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
  TestDriver d{1280, 800};
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
  std::vector<std::string> chain(doc::TrackId id) const {
    std::vector<std::string> v;
    for (const doc::DevicePtr& dev : track(id).devices) v.push_back(dev->patch + (dev->active ? "" : "(off)"));
    return v;
  }
  size_t history() const { return ctx.store().history().cursor(); }
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

TEST_CASE("device view: shows the selected track's chain, one T4.6 device panel per device") {
  Ui u;
  DeviceChainView& v = u.view();
  CHECK(v.track() == u.keys);
  const doc::DeviceId inst = u.track(u.keys).devices.at(0)->id;
  REQUIRE(v.frameOf(inst) != nullptr);
  devices::DevicePanel* p = v.panelOf(inst);
  REQUIRE(p != nullptr);
  CHECK(p->signature().patch == "BasicSynth");
  CHECK(p->desc().generated);  // no panel description: the generic panel, one control per param
  for (const devices::ParamInfo& pi : p->signature().params) CHECK(p->controlFor(pi.name) != nullptr);
  // A stock effect gets its own panel description (stdlib/devices/utility.panel.json).
  u.addFromMenu("Utility");
  const doc::DeviceId util = u.track(u.keys).devices.at(1)->id;
  REQUIRE(v.panelOf(util) != nullptr);
  CHECK_FALSE(v.panelOf(util)->desc().generated);
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
  CHECK(u.chain(u.keys) == std::vector<std::string>{"BasicSynth", "utility", "saturator"});
  const doc::DeviceId util = u.track(u.keys).devices[1]->id, sat = u.track(u.keys).devices[2]->id;
  CHECK(v.selection().size() == 1);
  CHECK(v.selection()[0] == sat);  // the new device is selected

  // Reorder: drag Saturator's title bar left of Utility's.
  const IRect fs = u.frame(sat), fu = u.frame(util);
  u.d.drag({static_cast<float>(fs.x + fs.w / 2), static_cast<float>(fs.y + 18)}, {static_cast<float>(fu.x + 10), static_cast<float>(fu.y + 18)}, 8);
  CHECK(u.history() == h0 + 3);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"BasicSynth", "saturator", "utility"});
  // An effect never moves before the instrument: dragged to the far left it lands after it.
  const IRect fu2 = u.frame(util), fi = u.frame(u.track(u.keys).devices[0]->id);
  u.d.drag({static_cast<float>(fu2.x + fu2.w / 2), static_cast<float>(fu2.y + 18)}, {static_cast<float>(fi.x + 2), static_cast<float>(fi.y + 18)}, 8);
  CHECK(u.history() == h0 + 4);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"BasicSynth", "utility", "saturator"});

  // Deactivate: select Saturator (its title bar), `0`.
  const IRect fs1 = u.frame(sat);
  u.d.click({static_cast<float>(fs1.x + fs1.w / 2), static_cast<float>(fs1.y + 18)});
  REQUIRE(v.selection().size() == 1);
  CHECK(v.selection()[0] == sat);
  CHECK(u.d.press("0") == KeyResult::Action);
  CHECK(u.history() == h0 + 5);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"BasicSynth", "utility", "saturator(off)"});
  CHECK_FALSE(v.panelOf(sat)->active());  // the panel shows it

  // Keys: select Utility (a click on its title), Ctrl+D duplicates, 0 toggles, Ctrl+C / Ctrl+V on the group, Delete.
  const IRect fu3 = u.frame(util);
  u.d.click({static_cast<float>(fu3.x + fu3.w / 2), static_cast<float>(fu3.y + 18)});
  REQUIRE(v.selection().size() == 1);
  CHECK(v.selection()[0] == util);
  CHECK(u.d.press("Ctrl+D") == KeyResult::Action);
  CHECK(u.history() == h0 + 6);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"BasicSynth", "utility", "utility", "saturator(off)"});
  CHECK(u.d.press("0") == KeyResult::Action);  // the duplicate is selected now
  CHECK(u.history() == h0 + 7);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"BasicSynth", "utility", "utility(off)", "saturator(off)"});
  CHECK(u.d.press("Ctrl+C") == KeyResult::Action);
  u.ctx.setSelectedTrack(u.grp);
  u.d.click(mid(v.rect()));  // focus the view (empty space)
  CHECK(u.d.press("Ctrl+V") == KeyResult::Action);
  CHECK(u.history() == h0 + 8);
  CHECK(u.chain(u.grp) == std::vector<std::string>{"utility(off)"});
  u.ctx.setSelectedTrack(u.keys);
  const IRect fs2 = u.frame(sat);
  u.d.click({static_cast<float>(fs2.x + fs2.w / 2), static_cast<float>(fs2.y + 18)});
  CHECK(u.d.press("Delete") == KeyResult::Action);
  CHECK(u.history() == h0 + 9);
  CHECK(u.chain(u.keys) == std::vector<std::string>{"BasicSynth", "utility", "utility(off)"});

  // Every step undoes on its own, back to the start.
  for (size_t k = 0; k < 9; ++k) REQUIRE(u.ctx.undo());
  CHECK(u.chain(u.keys) == std::vector<std::string>{"BasicSynth"});
  CHECK(u.chain(u.grp).empty());
}

TEST_CASE("device view: a param drag is one undo step of Param changes; a const is one structural step") {
  Ui u;
  u.addFromMenu("Saturator");
  const doc::DeviceId sat = u.track(u.keys).devices[1]->id;
  const size_t h0 = u.history();
  devices::DevicePanel* panel = u.view().panelOf(sat);
  REQUIRE(panel != nullptr);
  ui::Widget* drive = panel->controlFor("drive");
  REQUIRE(drive != nullptr);
  u.d.tree().layoutNow();
  const Vec2 c = mid(drive->rect());
  u.d.drag(c, {c.x, c.y - 40.0f}, 6);
  CHECK(u.history() == h0 + 1);
  const doc::ChangeSet& cs = u.ctx.store().history().entries()[h0].changes;
  CHECK(cs.paramOnly());
  REQUIRE(cs.params.size() == 1);
  CHECK(cs.params[0] == doc::ParamRef::device(sat, "drive"));
  const doc::Device& d = *doc::findDevice(*u.ctx.doc(), sat).device;
  REQUIRE(d.param("drive") != nullptr);
  CHECK(d.param("drive")->value > 1.0);  // up from 0 dB (a linear gain above 1)
  // A structural const through the panel's target: one structural step, shown back in the panel.
  devices::DeviceTarget* target = u.view().targetOf(sat);
  REQUIRE(target != nullptr);
  target->setConst("type", 6.0);
  CHECK(u.history() == h0 + 2);
  CHECK(u.ctx.store().history().entries()[h0 + 1].changes.structural());
  CHECK(doc::findDevice(*u.ctx.doc(), sat).device->setting("type")->value == 6.0);
  CHECK(u.view().panelOf(sat)->constValue("type") == 6.0);
  REQUIRE(u.ctx.undo());
  CHECK(u.view().panelOf(sat)->constValue("type") == 3.0);  // undo comes back to the panel
}

TEST_CASE("device view: a param change from elsewhere repaints only its control") {
  Ui u;
  u.addFromMenu("Utility");
  const doc::DeviceId util = u.track(u.keys).devices[1]->id;
  u.d.step();
  u.d.step();  // settle: nothing damaged
  devices::DevicePanel* panel = u.view().panelOf(util);
  REQUIRE(panel != nullptr);
  ui::Widget* width = panel->controlFor("width");
  REQUIRE(width != nullptr);
  REQUIRE(u.ctx.store().dispatch(doc::SetDeviceParam{util, "width", 2.0}).ok());
  CHECK(panel->paramValue("width") == 2.0);
  IRect rects[Damage::kMaxRects];
  int64_t area = 0;
  const uint32_t n = u.d.tree().damageRects(rects, area);
  REQUIRE(n >= 1);
  for (uint32_t i = 0; i < n; ++i) {  // inside the control (a knob may damage its value readout with it)
    const IRect& r = rects[i];
    const IRect& w = width->rect();
    CHECK(r.x >= w.x - 40);
    CHECK(r.right() <= w.right() + 40);
    CHECK(r.y >= w.y - 40);
    CHECK(r.bottom() <= w.bottom() + 40);
  }
  CHECK(area < static_cast<int64_t>(panel->rect().w) * panel->rect().h / 4);
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
