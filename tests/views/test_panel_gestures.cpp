// GU.13 (GU.f2 / GU.f3: one panel system): a LEAF's knob (a native engine's: its synthetic panel) and a GROUP's knob (a
// module's: its panel's element bound to a value block) take the same gestures through real window / overlay dispatch —
// the wheel (a run of notches one undo step), a typed value, Delete (the default), the right-click menu (Show
// Automation, Knob Curve), macro map mode (pick a macro, click the knob: mapped) — and show the same things (the
// automation LED, the live modulation ring). The fixture: Keys (MIDI): instrument → Utility → an Audio Effect Rack whose
// chain holds the Saturator (a module) and a Wavetable (a native engine: a leaf).

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/racks.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/arrangement/automation.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/devices/device_model.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/block_frame.h"
#include "pq/views/shell/shell.h"
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

struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{2600, 900};
  std::unique_ptr<Shell> shell;
  doc::TrackId keys;
  doc::DeviceId sat, rack, drive, leaf;
  std::string leafParam;  // a knob of the leaf

  Ui() {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks[0];
    (void)test_blocks::addModule(st, keys, "utility");
    sat = test_blocks::addModule(st, keys, "saturator");
    drive = test_blocks::member(*st.current(), sat, "Drive");
    REQUIRE(drive.valid());
    rack = run(doc::ops::groupDevices(*st.current(), st.ids(), std::vector<doc::DeviceId>{sat})).devices[0];
    // The leaf: a native engine (its synthetic panel: its module's, the Wavetable's fold knob a knob element).
    leaf = run(doc::ops::addToChain(*st.current(), st.ids(), doc::rackOf(*st.current(), rack)->chains[0].id, "meta:wavetable",
                                    doc::DeviceKind::AudioEffect))
               .devices.at(0);
    leafParam = "fold";
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

  doc::ops::Edit run(const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(ctx.store().dispatch(e->command).ok());
    return *e;
  }
  DeviceChainView& view() const {
    DeviceChainView* v = findDeviceChain(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  // The two knobs under test: (block, param) — the leaf's, the module's Drive.
  struct Knob {
    doc::DeviceId device;
    std::string param;
  };
  std::vector<Knob> knobs() const { return {{leaf, leafParam}, {drive, std::string(doc::kValueParam)}}; }
  Vec2 at(const Knob& k) {
    d.tree().layoutNow();
    const std::optional<IRect> r = view().controlRect(k.device, k.param);
    REQUIRE(r.has_value());
    return mid(*r);
  }
  // The knob position its panel shows (through the panel's own target).
  double pos(const Knob& k) {
    panel::PanelHost* h = view().controlHost(k.device, k.param);
    REQUIRE(h != nullptr);
    return h->panelFace()->target().position(doc::controlBinding(k.device, k.param));
  }
  double stored(const Knob& k) const {
    const doc::DeviceValue* v = doc::findDevice(*ctx.doc(), k.device).device->param(k.param);
    return v != nullptr ? v->value : -1.0;
  }
  Menu* lastMenu() {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<Menu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
};

}  // namespace

TEST_CASE("panel gestures: a leaf's and a group's knob — the wheel (a run = one undo step), a typed value, Delete") {
  Ui u;
  REQUIRE(u.view().panelOf(u.leaf) != nullptr);  // the leaf: a framed panel of the one system
  REQUIRE(u.view().panelOf(u.sat) == nullptr);   // the module: its group frame's panel
  for (const Ui::Knob& k : u.knobs()) {
    CAPTURE(k.param);
    const Vec2 c = u.at(k);
    // The wheel: two notches up, one run, one undo step after the run ends.
    const double p0 = u.pos(k);
    const size_t h0 = u.history();
    u.d.move(c);
    u.d.wheel(c, 1.0f);
    u.d.wheel(c, 1.0f);
    CHECK(u.pos(k) > p0);
    CHECK(u.stored(k) == doctest::Approx(u.pos(k)));  // applied at once (live), the step still open
    u.d.advance(panel::PanelHost::kWheelRunSeconds + 0.1);
    CHECK(u.history() == h0 + 1);
    REQUIRE(u.ctx.undo());
    CHECK(u.pos(k) == doctest::Approx(p0));
    // A typed value: a press gives the knob the keys; digits open the type-in, Enter commits one step.
    u.d.click(c);
    const size_t h1 = u.history();
    u.d.press("5");
    CHECK(u.d.type("0"));
    u.d.press("Return");
    u.d.step();
    CHECK(u.history() == h1 + 1);
    CHECK(u.pos(k) != doctest::Approx(p0));
    // Delete: back to its default (one step).
    u.d.press("Delete");
    u.d.step();
    CHECK(u.history() == h1 + 2);
  }
}

TEST_CASE("panel gestures: a leaf's and a group's knob — right-click Show Automation and Knob Curve; the LED") {
  Ui u;
  for (const Ui::Knob& k : u.knobs()) {
    CAPTURE(k.param);
    u.d.click(u.at(k), mod::kNone, PointerButton::Right);
    Menu* m = u.lastMenu();
    REQUIRE(m != nullptr);
    int32_t show = -1;
    bool curve = false;
    for (size_t i = 0; i < m->items().size(); ++i) {
      if (m->items()[i].label == "Show Automation") show = static_cast<int32_t>(i);
      curve = curve || m->items()[i].label == "Knob Curve";
    }
    REQUIRE(show >= 0);
    CHECK(curve);
    const size_t h0 = u.history();
    m->choose(show);
    u.d.step();
    auto* arr = dynamic_cast<ArrangementView*>(u.d.find("main/body/top/track-area/arrangement"));
    REQUIRE(arr != nullptr);
    const std::vector<doc::ParamRef> shown = arr->automation().shownLanes(u.keys);
    CHECK(std::find(shown.begin(), shown.end(), doc::ParamRef::device(k.device, k.param)) != shown.end());
    CHECK(u.history() == h0);  // view state
    // An automation lane on it: its LED (the frame that shows the control marks it).
    auto l = std::make_shared<doc::AutomationLane>();
    l->id = u.ctx.store().ids().lane();
    l->param = doc::ParamRef::device(k.device, k.param);
    l->paramName = k.param;
    l->points.push_back({0, 0.2f, 0.0f, u.ctx.store().ids().breakpoint()});
    l->points.push_back({4 * kPpq, 0.8f, 0.0f, u.ctx.store().ids().breakpoint()});
    REQUIRE(u.ctx.store().dispatch(doc::AddAutomationLane{u.keys, l}).ok());
    u.d.tree().layoutNow();
    const auto marks = u.view().automationMarks(k.device);
    REQUIRE(marks.size() == 1);
    CHECK(marks[0].first == k.param);
    // Automated, the knob shows the envelope at the playhead (not its stored position).
    u.port.setPlayheadTick(2 * kPpq);
    u.view().applyAutomation();
    CHECK(u.pos(k) == doctest::Approx(0.5).epsilon(0.02));
  }
}

TEST_CASE("panel gestures: map mode maps a leaf's and a group's knob alike (a macro, then the knob: one step)") {
  Ui u;
  DeviceChainView& v = u.view();
  v.setMapMode(u.rack);
  u.d.tree().layoutNow();
  int macro = 1;
  for (const Ui::Knob& k : u.knobs()) {
    CAPTURE(k.param);
    const std::optional<IRect> m = v.controlRect(u.rack, "macro" + std::to_string(macro));
    REQUIRE(m.has_value());
    const size_t h0 = u.history();
    u.d.click(mid(*m));
    CHECK(v.mapMacro() == macro - 1);
    u.d.click(u.at(k));
    CHECK(u.history() == h0 + 1);
    bool mapped = false;
    const std::optional<doc::RackInfo> ri = doc::rackOf(*u.ctx.doc(), u.rack);
    REQUIRE(ri.has_value());
    for (const doc::RackMapping& x : ri->mappings)
      mapped = mapped || (x.device == k.device && x.param == k.param && static_cast<int>(x.macro) == macro - 1);
    CHECK(mapped);
    ++macro;
  }
  u.d.tree().setFocus(&v);
  u.d.press("Escape");
  CHECK_FALSE(v.mapRack().valid());
  // Mapped, each knob shows the mapping's range as its live ring, and turning it turns its macro.
  v.refreshRings();
  for (const Ui::Knob& k : u.knobs()) {
    CAPTURE(k.param);
    panel::PanelHost* h = v.controlHost(k.device, k.param);
    REQUIRE(h != nullptr);
    const ui::ModulationRing* ring = h->panelFace()->target().modulation(doc::controlBinding(k.device, k.param));
    REQUIRE(ring != nullptr);
    CHECK(ring->active);
  }
}
