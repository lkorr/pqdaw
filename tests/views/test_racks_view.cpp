// T4.8 racks in the Device view through the real shell: a rack's frame (macro panel, chain list with the chain mixer,
// the zone editor, the frames of the chain it shows), showing another chain, dragging devices into chains (a row, the
// empty list: a new chain), Ctrl+G, macro map mode (macro, then knob: one undo step; again: unmapped), a macro drag
// (one step of Param changes), a chain volume drag, a zone drag, variations; opening a rack in the circuit view.
// Goldens: audio-rack, instrument-rack.

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/ui/controls.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/circuit_viewer.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/fake_transport.h"
#include "../../src/views/src/shell/shell_widgets.h"
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

// The shell over one MIDI track: BasicSynth → Utility → Saturator, the saturator grouped into an Audio Effect Rack (one
// chain), shown in the Device view.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  doc::TrackId keys;
  doc::DeviceId inst, sat, util, rack;

  Ui() {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks[0];
    inst = track().devices.at(0)->id;
    util = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "utility", doc::DeviceKind::AudioEffect)).devices[0];
    sat = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "saturator", doc::DeviceKind::AudioEffect)).devices[0];
    rack = run(doc::ops::groupDevices(*st.current(), st.ids(), std::vector<doc::DeviceId>{sat})).devices[0];
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
  const doc::Track& track() const { return *doc::findTrackPtr(*ctx.doc(), keys); }
  const doc::Rack& rackOf(doc::DeviceId id) const { return *doc::findDevice(*ctx.doc(), id).device->rack; }
  size_t history() const { return ctx.store().history().cursor(); }
  IRect frame(doc::DeviceId id) {
    d.tree().layoutNow();
    Widget* f = view().frameOf(id);
    REQUIRE(f != nullptr);
    return f->rect();
  }
  Widget* in(doc::DeviceId id, std::string_view path) {
    d.tree().layoutNow();
    Widget* f = view().frameOf(id);
    REQUIRE(f != nullptr);
    return f->find(path);
  }
  // A point in a frame's title bar (below the jack strip of a device with exports).
  Vec2 title(doc::DeviceId id) {
    const IRect r = frame(id);
    devices::DevicePanel* p = view().panelOf(id);
    const int32_t strip = p != nullptr && !p->signature().exports.empty() ? 10 : 0;
    return {static_cast<float>(r.x + r.w / 2), static_cast<float>(r.y + strip + 10)};
  }
};

}  // namespace

TEST_CASE("rack view: the frame shows macros, chain rows with their mixer, and the shown chain's devices") {
  Ui u;
  DeviceChainView& v = u.view();
  REQUIRE(v.frameOf(u.rack) != nullptr);
  // The macro panel is the rack's panel. M4 review UI item 2: a rack with no macro in use shows one column of four
  // (always one free macro past the highest mapped or named one, up to visibleMacros); the chain selector is the zone
  // editor's (its Chain tab), not a knob.
  devices::DevicePanel* p = v.panelOf(u.rack);
  REQUIRE(p != nullptr);
  CHECK(p->controlFor("macro1") != nullptr);
  CHECK(p->controlFor("macro4") != nullptr);
  CHECK(p->controlFor("macro5") == nullptr);
  CHECK(p->controlFor("select") == nullptr);
  u.run(doc::ops::mapMacro(*u.ctx.doc(), u.ctx.store().ids(), u.rack, 3, u.sat, "drive"));  // macro 4 in use
  p = v.panelOf(u.rack);
  REQUIRE(p != nullptr);
  CHECK(p->controlFor("macro5") != nullptr);  // the next column: a free macro to map next
  CHECK(p->controlFor("macro8") != nullptr);
  CHECK(p->controlFor("macro9") == nullptr);  // visibleMacros (8) caps it
  REQUIRE(u.ctx.undo());
  p = v.panelOf(u.rack);
  REQUIRE(p != nullptr);
  // The chain shown is the first; its device has a frame and a panel nested in the rack's frame.
  const doc::ChainId c1 = u.rackOf(u.rack).chains[0]->id;
  CHECK(v.shownChain(u.rack) == c1);
  REQUIRE(v.frameOf(u.sat) != nullptr);
  CHECK(v.frameOf(u.rack)->rect().contains(mid(u.frame(u.sat))));
  // A second chain (+ in the chain list header), then a click on its row shows it (view state: no history).
  Widget* list = u.in(u.rack, "chains");
  REQUIRE(list != nullptr);
  const size_t h0 = u.history();
  u.d.click({static_cast<float>(list->rect().right() - 8), static_cast<float>(list->rect().y + 10)});
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.rackOf(u.rack).chains.size() == 2);
  const doc::ChainId c2 = u.rackOf(u.rack).chains[1]->id;
  Widget* row2 = u.in(u.rack, "chains/chain[" + std::to_string(c2.value) + "]");
  REQUIRE(row2 != nullptr);
  u.d.click({static_cast<float>(row2->rect().x + 60), static_cast<float>(row2->rect().y + row2->rect().h / 2)});
  CHECK(v.shownChain(u.rack) == c2);
  CHECK(u.history() == h0 + 1);
  CHECK(v.frameOf(u.sat) == nullptr);  // not in the chain shown
  // The row's mixer: its activator mutes the chain (structural), one undo step.
  Widget* on = u.in(u.rack, "chains/chain[" + std::to_string(c2.value) + "]/on");
  REQUIRE(on != nullptr);
  u.d.click(mid(on->rect()));
  CHECK(u.history() == h0 + 2);
  CHECK_FALSE(u.rackOf(u.rack).chains[1]->active);
}

TEST_CASE("rack view: dragging devices into a chain row, onto the empty chain list (a new chain), out to the track") {
  Ui u;
  const size_t h0 = u.history();
  Widget* list = u.in(u.rack, "chains");
  REQUIRE(list != nullptr);
  // Utility (on the track) onto the empty area below the rows: a new chain holding it, one step.
  const Vec2 empty{static_cast<float>(list->rect().x + 40), static_cast<float>(list->rect().y + 90)};
  u.d.drag(u.title(u.util), empty, 10);
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.rackOf(u.rack).chains.size() == 2);
  CHECK(u.rackOf(u.rack).chains[1]->devices.at(0)->id == u.util);
  CHECK(u.track().devices.size() == 2);  // instrument, rack
  // The saturator (in chain 1) onto chain 2's row: moved there, one step.
  const doc::ChainId c2 = u.rackOf(u.rack).chains[1]->id;
  Widget* row2 = u.in(u.rack, "chains/chain[" + std::to_string(c2.value) + "]");
  REQUIRE(row2 != nullptr);
  u.d.drag(u.title(u.sat), mid(row2->rect()), 10);
  CHECK(u.history() == h0 + 2);
  CHECK(u.rackOf(u.rack).chains[0]->devices.empty());
  CHECK(u.rackOf(u.rack).chains[1]->devices.size() == 2);
  // Out of the rack again: onto the track's chain, just left of the rack.
  u.view().showChain(u.rack, c2);
  const IRect rf = u.frame(u.rack);
  u.d.drag(u.title(u.util), {static_cast<float>(rf.x + 3), static_cast<float>(rf.y + 40)}, 10);
  CHECK(u.history() == h0 + 3);
  REQUIRE(u.track().devices.size() == 3);
  CHECK(u.track().devices[1]->id == u.util);
  for (int k = 0; k < 3; ++k) REQUIRE(u.ctx.undo());
  CHECK(u.track().devices[1]->id == u.util);  // the start: [instrument, utility, rack]
  CHECK(u.rackOf(u.rack).chains.size() == 1);
}

TEST_CASE("rack view: Ctrl+G groups the selection; the rack opens in the circuit view") {
  Ui u;
  const size_t h0 = u.history();
  u.d.click(u.title(u.util));
  REQUIRE(u.view().selection().size() == 1);
  CHECK(u.d.press("Ctrl+G") == KeyResult::Action);
  CHECK(u.history() == h0 + 1);
  const doc::DevicePtr last = u.track().devices.at(1);
  REQUIRE(last->isRack());
  CHECK(last->rack->chains.at(0)->devices.at(0)->id == u.util);
  // Open in Circuit View: the rack's lowered subcircuit, read-only (a viewer in the overlay).
  CHECK(u.view().openRackCircuit(u.rack));
  bool viewer = false;
  for (size_t i = 0; i < u.shell->overlay().childCount(); ++i)
    if (auto* cv = dynamic_cast<circuit::CircuitViewer*>(&u.shell->overlay().child(i))) {
      viewer = true;
      REQUIRE(cv->shownPatch() != nullptr);
      CHECK(cv->shownPatch()->name() == "AudioEffectRack" + std::to_string(u.rack.value));
      // Its calls are the rack's devices (the same lowering as the track's: Device ids keep their NodeIds).
      CHECK(cv->shownPatch()->indexOf(graph::NodeId{doc::nodeIdFor(doc::NodeRole::Device, u.sat.value, 0)}).has_value());
    }
  CHECK(viewer);
}

TEST_CASE("rack view: map mode — a macro, then a knob maps it (one step); again unmaps; Esc ends map mode") {
  Ui u;
  DeviceChainView& v = u.view();
  v.setMapMode(u.rack);
  CHECK(v.mapRack() == u.rack);
  u.d.tree().layoutNow();
  // Click macro 2, then the saturator's drive knob.
  devices::DevicePanel* rp = v.panelOf(u.rack);
  devices::DevicePanel* sp = v.panelOf(u.sat);
  REQUIRE(rp != nullptr);
  REQUIRE(sp != nullptr);
  const size_t h0 = u.history();
  u.d.click(mid(rp->controlFor("macro2")->rect()));
  CHECK(v.mapMacro() == 1);
  CHECK(u.history() == h0);  // picking a macro is view state
  u.d.click(mid(sp->controlFor("drive")->rect()));
  CHECK(u.history() == h0 + 1);
  const doc::MappingLoc m = doc::mappingOf(*u.ctx.doc(), u.sat, "drive");
  REQUIRE(m.mapping != nullptr);
  CHECK(m.mapping->macro == 1);
  CHECK(m.rack->id == u.rack);
  // The macro took over the knob's position (its first mapping: Live).
  CHECK(doc::rackParamValue(*m.rack, "macro2") == doctest::Approx(0.5 * doc::kMacroMax));  // drive 0 dB: mid travel
  // The mapped knob shows the mapping's range as a ring.
  // A second click on it unmaps (one step).
  u.d.click(mid(sp->controlFor("drive")->rect()));
  CHECK(u.history() == h0 + 2);
  CHECK(doc::mappingOf(*u.ctx.doc(), u.sat, "drive").mapping == nullptr);
  // Knobs outside the rack are not targets: a click on the instrument's knob changes nothing.
  devices::DevicePanel* ip = v.panelOf(u.inst);
  REQUIRE(ip != nullptr);
  u.d.click(mid(ip->controlFor("bright")->rect()));
  CHECK(u.history() == h0 + 2);
  // Esc ends map mode.
  u.d.click(mid(v.rect()));
  u.d.tree().setFocus(&v);
  u.d.press("Escape");
  CHECK_FALSE(v.mapRack().valid());
}

TEST_CASE("rack view: a macro drag is one step of Param changes; a chain volume drag one step; a zone drag one step") {
  Ui u;
  DeviceChainView& v = u.view();
  u.run(doc::ops::mapMacro(*u.ctx.doc(), u.ctx.store().ids(), u.rack, 0, u.sat, "drive"));
  devices::DevicePanel* rp = v.panelOf(u.rack);
  REQUIRE(rp != nullptr);
  u.d.tree().layoutNow();
  const size_t h0 = u.history();
  const Vec2 c = mid(rp->controlFor("macro1")->rect());
  u.d.drag(c, {c.x, c.y - 40.0f}, 6);
  CHECK(u.history() == h0 + 1);
  const doc::ChangeSet& cs = u.ctx.store().history().entries()[h0].changes;
  CHECK(cs.paramOnly());
  REQUIRE(cs.params.size() == 1);
  CHECK(cs.params[0] == doc::ParamRef::device(u.rack, "macro1"));
  CHECK(doc::rackParamValue(*doc::findDevice(*u.ctx.doc(), u.rack).device, "macro1") > 10.0);
  // The chain row's volume box.
  const doc::ChainId c1 = u.rackOf(u.rack).chains[0]->id;
  Widget* vol = u.in(u.rack, "chains/chain[" + std::to_string(c1.value) + "]/volume");
  REQUIRE(vol != nullptr);
  const Vec2 vc = mid(vol->rect());
  u.d.drag(vc, {vc.x, vc.y + 30.0f}, 6);
  CHECK(u.history() == h0 + 2);
  CHECK(u.ctx.store().history().entries()[h0 + 1].changes.paramOnly());
  CHECK(u.rackOf(u.rack).chains[0]->volumeDb < 0.0f);
  // The zone editor (Chain tab): drag the zone's right end left — the zone switches on (structural), one step.
  v.showZones(u.rack, doc::ZoneKind::Select);
  Widget* zones = u.in(u.rack, "zones");
  REQUIRE(zones != nullptr);
  REQUIRE(zones->visible());
  const IRect zr = zones->rect();
  const float rowY = static_cast<float>(zr.y + 20 + 15);
  u.d.drag({static_cast<float>(zr.right() - 6), rowY}, {static_cast<float>(zr.x + zr.w / 2), rowY}, 6);
  CHECK(u.history() == h0 + 3);
  const doc::Zone z = u.rackOf(u.rack).chains[0]->select;
  CHECK(z.hi < 100.0);
  CHECK(z.hi > 40.0);
  CHECK_FALSE(doc::zoneOff(z, doc::ZoneKind::Select, doc::KeyZoneUnit::Hz));
  for (int k = 0; k < 3; ++k) REQUIRE(u.ctx.undo());
  CHECK(doc::zoneOff(u.rackOf(u.rack).chains[0]->select, doc::ZoneKind::Select, doc::KeyZoneUnit::Hz));
}

// M4 review M6 / L2: a zone drag and a device reorder drag cancel on a lost focus (alt-tab) and on Esc: the document is
// as before, no undo step, and a pointer that comes back with the button down drives nothing.
TEST_CASE("rack view: zone and reorder drags cancel on focus loss and Esc (M4 review M6, L2)") {
  Ui u;
  DeviceChainView& v = u.view();
  v.showZones(u.rack, doc::ZoneKind::Select);
  Widget* zones = u.in(u.rack, "zones");
  REQUIRE(zones != nullptr);
  const IRect zr = zones->rect();
  const float rowY = static_cast<float>(zr.y + 20 + 15);
  const Vec2 end{static_cast<float>(zr.right() - 6), rowY};
  const doc::Snapshot before = u.ctx.doc();
  const size_t h0 = u.history();
  auto zoneDrag = [&] {
    u.d.move(end);
    u.d.down(end);
    u.d.move({end.x - 80.0f, rowY});
    REQUIRE(u.ctx.store().inGesture());
    CHECK_FALSE(doc::zoneOff(u.rackOf(u.rack).chains[0]->select, doc::ZoneKind::Select, doc::KeyZoneUnit::Hz));
  };
  // Focus loss mid-drag.
  zoneDrag();
  u.shell->cancelInteraction();
  CHECK_FALSE(u.ctx.store().inGesture());
  CHECK(u.d.tree().captured() == nullptr);
  CHECK(u.ctx.doc() == before);
  u.d.move({end.x - 160.0f, rowY});  // hover moves after the cancel: nothing follows the pointer
  u.d.up({end.x - 160.0f, rowY});
  CHECK(u.ctx.doc() == before);
  CHECK(u.history() == h0);
  // Esc mid-drag (the focus elsewhere: Esc goes to the widget holding the pointer).
  zoneDrag();
  CHECK(u.d.keyDown(key::kEscape) == KeyResult::Widget);
  CHECK_FALSE(u.ctx.store().inGesture());
  u.d.move({end.x - 160.0f, rowY});
  u.d.up({end.x - 160.0f, rowY});
  CHECK(u.ctx.doc() == before);
  CHECK(u.history() == h0);
  // A device reorder drag (Utility to the end of the chain): focus loss, then Esc; nothing moves, a later click does
  // not drop it.
  for (int k = 0; k < 2; ++k) {
    CAPTURE(k);
    const Vec2 from = u.title(u.util);
    const IRect last = u.frame(u.rack);
    const Vec2 to{static_cast<float>(last.right() + 30), from.y};
    u.d.move(from);
    u.d.down(from);
    u.d.move({from.x + 40.0f, from.y});
    u.d.move(to);
    if (k == 0) u.shell->cancelInteraction();
    else CHECK(u.d.keyDown(key::kEscape) == KeyResult::Widget);
    u.d.up(to);
    u.d.click(to);
    CHECK(u.ctx.doc() == before);
    CHECK(u.history() == h0);
  }
}

// T4.17: Ctrl+R renames the selected device in its title bar (one undo step; the title shows the name); Ctrl+Shift+G
// ungroups the selected one-chain rack (its devices take its place and become the selection). M4 review M14: with the
// Device view focused, the Edit menu acts on the devices (no arrangement-only items).
TEST_CASE("rack view: Ctrl+R renames a device in its title bar; Ctrl+Shift+G ungroups; Edit follows the focus (T4.17, M14)") {
  Ui u;
  DeviceChainView& v = u.view();
  const doc::DeviceId one[1] = {u.rack};
  v.select(one);
  u.d.tree().setFocus(&v);
  // Edit menu: the device actions.
  auto* bar = dynamic_cast<shell::MenuBar*>(u.d.find("main/menu-bar"));
  REQUIRE(bar != nullptr);
  const std::vector<MenuItem> edit = bar->items(1);
  auto has = [&](std::string_view action) {
    return std::any_of(edit.begin(), edit.end(), [&](const MenuItem& m) { return m.action == action; });
  };
  CHECK(has("device.copy"));
  CHECK(has("device.delete"));
  CHECK(has("device.ungroup"));
  CHECK_FALSE(has("arrangement.copy"));
  CHECK_FALSE(has("arrangement.split"));
  CHECK_FALSE(has("arrangement.insertTime"));
  // Rename: a field over the title; typing replaces the selected name; Enter: one step.
  const size_t h0 = u.history();
  CHECK(u.d.press("Ctrl+R") == KeyResult::Action);
  devices::DevicePanel* p = v.panelOf(u.rack);
  REQUIRE(p != nullptr);
  CHECK(p->renaming());
  CHECK(u.d.type("Dirt"));
  u.d.press("Return");
  u.d.step();
  CHECK_FALSE(p->renaming());
  CHECK(u.history() == h0 + 1);
  CHECK(doc::findDevice(*u.ctx.doc(), u.rack).device->name == "Dirt");
  CHECK(v.panelOf(u.rack)->title() == "Dirt");
  // Esc cancels.
  u.d.tree().setFocus(&v);
  u.d.press("Ctrl+R");
  CHECK(u.d.type("Nope"));
  u.d.press("Escape");
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(doc::findDevice(*u.ctx.doc(), u.rack).device->name == "Dirt");
  // Ungroup: the Saturator back in the track's chain where the rack was, selected; one step.
  u.d.tree().setFocus(&v);
  v.select(one);
  CHECK(u.d.press("Ctrl+Shift+G") == KeyResult::Action);
  CHECK(u.history() == h0 + 2);
  CHECK_FALSE(doc::findDevice(*u.ctx.doc(), u.rack).device);
  REQUIRE(u.track().devices.size() == 3);
  CHECK(u.track().devices[2]->id == u.sat);
  REQUIRE(v.selection().size() == 1);
  CHECK(v.selection()[0] == u.sat);
  REQUIRE(u.ctx.undo());
  CHECK(doc::findDevice(*u.ctx.doc(), u.rack).device);
}

// M4 review L3: a knob mapped to a macro that an outer rack's macro maps shows the value as played (down the chain of
// mappings from the outer macro), not the inner macro's stored value.
TEST_CASE("rack view: a knob under nested macro mappings shows the played value (M4 review L3)") {
  Ui u;
  DeviceChainView& v = u.view();
  const doc::DeviceId outer = u.run(doc::ops::groupDevices(*u.ctx.doc(), u.ctx.store().ids(), std::vector<doc::DeviceId>{u.rack})).devices.at(0);
  u.run(doc::ops::mapMacro(*u.ctx.doc(), u.ctx.store().ids(), u.rack, 0, u.sat, "drive"));  // inner macro 1 -> drive
  u.run(doc::ops::mapMacro(*u.ctx.doc(), u.ctx.store().ids(), outer, 0, u.rack, "macro1"));  // outer macro 1 -> inner macro 1
  u.run(doc::ops::setDeviceParam(*u.ctx.doc(), outer, "macro1", doc::kMacroMax));  // the inner macro's stored value stays 0
  u.d.tree().layoutNow();
  v.refreshRings();
  devices::DevicePanel* sat = v.panelOf(u.sat);
  REQUIRE(sat != nullptr);
  const doc::MappingLoc m = doc::mappingOf(*u.ctx.doc(), u.sat, "drive");
  REQUIRE(m.mapping != nullptr);
  const devices::ParamInfo* info = nullptr;
  for (const devices::ParamInfo& pi : sat->signature().params)
    if (pi.name == "drive") info = &pi;
  REQUIRE(info != nullptr);
  CHECK(devices::toPosition(*info, sat->paramValue("drive")) == doctest::Approx(m.mapping->max).epsilon(1e-6));
}

// M4 review M16: the zone editor's hovered bar shows its range in the info view; UI item 6: Create > Insert Audio
// Effect Rack adds an empty rack after the selected device (one undo step).
TEST_CASE("rack view: a hovered zone shows its range (M16); Create inserts a rack (UI item 6)") {
  Ui u;
  DeviceChainView& v = u.view();
  v.showZones(u.rack, doc::ZoneKind::Select);
  Widget* zones = u.in(u.rack, "zones");
  REQUIRE(zones != nullptr);
  const IRect zr = zones->rect();
  const Vec2 row0{static_cast<float>(zr.x + zr.w / 2), static_cast<float>(zr.y + 20 + 11)};
  u.d.move(row0);
  const InfoText info = zones->info();
  CHECK(std::string(info.title).find("0 \xE2\x80\x93 127") != std::string::npos);
  CHECK(std::string(info.body).find("Full level") != std::string::npos);
  u.d.move({static_cast<float>(zr.x + zr.w / 2), static_cast<float>(zr.bottom() - 3)});  // the axis row: no bar
  CHECK(std::string(zones->info().title) == "Zones");
  // Create > Insert Audio Effect Rack: after the selected Utility.
  const doc::DeviceId one[1] = {u.util};
  v.select(one);
  const size_t h0 = u.history();
  REQUIRE(u.ctx.actions().invoke(u.ctx.actions().find("device.addAudioEffectRack")));
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.track().devices.size() == 4);
  CHECK(u.track().devices[2]->isRack());
  CHECK(u.track().devices[2]->kind == doc::DeviceKind::AudioEffect);
}

TEST_CASE("render: audio-rack and instrument-rack match the golden images") {
  for (const std::string scene : {"audio-rack", "instrument-rack"}) {
    CAPTURE(scene);
    uint32_t w = 0, h = 0;
    viewSceneDefaultSize(scene, w, h);
    GpuOptions o;
    o.validation = true;
    auto img = renderViewScene(scene, {}, w, h, {}, o, &library());
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
    auto ref = readPng(path);
    if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
    REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
    ImageDiff d;
    CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &d));
    MESSAGE(scene << ": " << pq::ui::test::describe(d));
  }
}

// M4 review M9: a rack nest 20,000 deep in the Device view. The frames nest inline down to kMaxNestedRackFrames
// racks (the widget tree, its layout, painting and hit tests stay shallow); deeper racks are not framed. Building,
// laying out, drawing frames and clicking work; nothing recurses through the whole nest.
TEST_CASE("racks view: a rack nest 20,000 deep shows its first levels and stays responsive (M9)" * doctest::test_suite("slow")) {
  Ui u;
  doc::DocumentStore& st = u.ctx.store();
  std::vector<doc::DeviceId> racks;  // outermost first
  {
    doc::DevicePtr inner = doc::makeDevice(st.ids().device(), "utility", doc::DeviceKind::AudioEffect);
    for (int k = 0; k < 20000; ++k) {
      inner = doc::makeRack(st.ids().device(), doc::DeviceKind::AudioEffect, {doc::makeChain(st.ids().chain(), "C", {inner})});
      racks.push_back(inner->id);
    }
    std::reverse(racks.begin(), racks.end());
    REQUIRE(st.dispatch(doc::AddDevice{u.keys, inner, doc::kAppend}).ok());
  }
  u.d.tree().layoutNow();
  u.d.advance(0.1);  // a few frames drawn
  constexpr uint32_t kMax = DeviceChainView::kMaxNestedRackFrames;
  CHECK(u.view().frameOf(racks[0]) != nullptr);
  CHECK(u.view().frameOf(racks[kMax - 1]) != nullptr);  // the deepest framed rack (its devices are not)
  CHECK(u.view().frameOf(racks[kMax]) == nullptr);
  // Pointer moves over the chain (hit tests through the nested frames) and a scroll: drawn again, no deep recursion.
  const IRect vp = u.view().chainViewport();
  const uint64_t drawn = u.d.framesDrawn();
  for (int x = vp.x + 10; x < vp.x + vp.w; x += vp.w / 8) u.d.move({static_cast<float>(x), mid(vp).y});
  u.view().scrollChainBy(vp.w);
  u.d.advance(0.05);
  CHECK(u.d.framesDrawn() > drawn);
}
