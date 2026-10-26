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
  // The macro panel is the rack's panel: 8 macros and the chain selector.
  devices::DevicePanel* p = v.panelOf(u.rack);
  REQUIRE(p != nullptr);
  CHECK(p->controlFor("macro1") != nullptr);
  CHECK(p->controlFor("macro8") != nullptr);
  CHECK(p->controlFor("macro9") == nullptr);
  CHECK(p->controlFor("select") != nullptr);
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

TEST_CASE("render: audio-rack and instrument-rack match the golden images") {
  for (const char* scene : {"audio-rack", "instrument-rack"}) {
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
