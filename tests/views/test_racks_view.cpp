// T4.8 racks in the Device view through the real shell — GU.9 v2: the same tests over racks that are sets of parallel
// groups (pq/document/racks.h; the frame is their Device-view form): a rack's frame (macro panel, chain list with the
// chain mixer, the zone editor, the frames of the chain it shows), showing another chain, dragging devices into chains
// (a row, the empty list: a new chain), Ctrl+G, macro map mode (macro, then knob: one undo step; again: unmapped), a
// macro drag (one step of Param changes on the macro's value block), a chain volume drag (its Mixer's Volume block), a
// zone drag (a zone group made), the circuit showing the chains as parallel group capsules. Goldens: audio-rack,
// instrument-rack.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/lower.h"
#include "pq/document/racks.h"
#include "pq/views/project_patch/project_patch.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/ui/controls.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/circuit_viewer.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/panel/block_frame.h"
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

// The shell over one MIDI track: its instrument group → Utility → Saturator (GU.11: modules, i.e. groups; their knobs are
// their panels' elements, bound to value blocks), the saturator grouped into an Audio Effect Rack (one
// chain: a chain group with its mixer, spliced where the saturator was), shown in the Device view.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{2600, 900};  // (GU.11: the instrument group's frame is wide: the rack in view)
  std::unique_ptr<Shell> shell;
  doc::TrackId keys;
  doc::DeviceId inst, sat, util, rack;
  doc::DeviceId drive;  // the Saturator's Drive (a value block of its group)

  Ui() {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks[0];
    inst = doc::mainPath(*st.current(), track()).blocks.at(0)->id;
    util = test_blocks::addModule(st, keys, "utility");
    sat = test_blocks::addModule(st, keys, "saturator");
    drive = test_blocks::member(*st.current(), sat, "Drive");
    REQUIRE(drive.valid());
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
  doc::RackInfo rackOf(doc::DeviceId id) const {
    std::optional<doc::RackInfo> r = doc::rackOf(*ctx.doc(), id);
    REQUIRE(r.has_value());
    return *r;
  }
  std::vector<doc::DeviceId> path() const {
    std::vector<doc::DeviceId> out;
    for (const doc::Device* b : doc::mainPath(*ctx.doc(), track()).blocks) out.push_back(b->id);
    return out;
  }
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
  // The knob of a module's control (its group panel's element bound to the value block), device px.
  Vec2 knob(doc::DeviceId control) {
    d.tree().layoutNow();
    for (const DeviceChainView::ParamControl& pc : view().paramControls())
      if (pc.device == control && pc.param == doc::kValueParam) return mid(pc.rect);
    FAIL("no shown control");
    return {};
  }
  // A point in a frame's title bar (below the jack strip of a device with exports).
  Vec2 title(doc::DeviceId id) {
    const IRect r = frame(id);
    if (const panel::BlockFrame* p = view().panelOf(id); p != nullptr && !view().frameOf(id)->find("chains")) {
      const IRect t = p->titleRect();  // GU.13: below its jack strip
      return {static_cast<float>(t.x + t.w / 2), static_cast<float>(t.y + t.h / 2)};
    }
    return {static_cast<float>(r.x + r.w / 2), static_cast<float>(r.y + 10)};
  }
  // GU.13: whether the rack's macro panel shows macro `k` (1-based) now.
  bool macroShown(doc::DeviceId r, int k) {
    d.tree().layoutNow();
    return view().controlRect(r, "macro" + std::to_string(k)).has_value();
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
  REQUIRE(v.panelOf(u.rack) != nullptr);
  CHECK(u.macroShown(u.rack, 1));
  CHECK(u.macroShown(u.rack, 4));
  CHECK_FALSE(u.macroShown(u.rack, 5));
  CHECK_FALSE(v.controlRect(u.rack, "select").has_value());
  u.run(doc::ops::mapMacro(*u.ctx.doc(), u.ctx.store().ids(), u.rack, 3, u.drive, std::string(doc::kValueParam)));  // macro 4 in use
  REQUIRE(v.panelOf(u.rack) != nullptr);
  CHECK(u.macroShown(u.rack, 5));  // the next column: a free macro to map next
  CHECK(u.macroShown(u.rack, 8));
  CHECK_FALSE(u.macroShown(u.rack, 9));  // visibleMacros (8) caps it
  REQUIRE(u.ctx.undo());
  REQUIRE(v.panelOf(u.rack) != nullptr);
  // The chain shown is the first; its device has a frame and a panel nested in the rack's frame.
  const doc::ChainId c1 = u.rackOf(u.rack).chains[0].id;
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
  const doc::ChainId c2 = u.rackOf(u.rack).chains[1].id;
  CHECK(doc::isGroupDevice(*doc::findDevice(*u.ctx.doc(), c2).device));  // a chain is a group
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
  CHECK_FALSE(u.rackOf(u.rack).chains[1].active);
}

TEST_CASE("rack view: a renamed chain mixer keeps the rack and its chain row (GU.f1: roles, not names)") {
  Ui u;
  const doc::RackInfo r0 = u.rackOf(u.rack);
  REQUIRE(r0.chains.size() == 1);  // one chain: a rack only because it holds a chain mixer
  const doc::RackChain& c = r0.chains[0];
  REQUIRE(c.mixer.valid());
  u.run(doc::ops::renameDevice(*u.ctx.doc(), c.mixer, "Blend"));
  u.run(doc::ops::renameDevice(*u.ctx.doc(), c.volumeBlock, "Level"));
  const doc::RackInfo r1 = u.rackOf(u.rack);
  REQUIRE(r1.chains.size() == 1);
  CHECK(r1.chains[0].mixer == c.mixer);
  CHECK(r1.chains[0].volumeBlock == c.volumeBlock);
  CHECK(r1.chains[0].devices.size() == r0.chains[0].devices.size());  // the mixer is still no chain device
  // The Device view still shows the rack frame with the chain's row and its mixer.
  REQUIRE(u.view().frameOf(u.rack) != nullptr);
  Widget* vol = u.in(u.rack, "chains/chain[" + std::to_string(c.id.value) + "]/volume");
  REQUIRE(vol != nullptr);
  const size_t h0 = u.history();
  u.run(doc::ops::setChainMixer(*u.ctx.doc(), c.id, -6.0f, 0.0f));
  CHECK(u.history() == h0 + 1);
  CHECK(std::fabs(u.rackOf(u.rack).chains[0].volumeDb + 6.0f) < 1e-4f);
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
  CHECK(u.rackOf(u.rack).chains[1].devices.at(0)->id == u.util);
  CHECK(u.path() == std::vector<doc::DeviceId>{u.inst, u.rack});  // instrument, rack
  // The saturator (in chain 1) onto chain 2's row: moved there, one step.
  const doc::ChainId c2 = u.rackOf(u.rack).chains[1].id;
  Widget* row2 = u.in(u.rack, "chains/chain[" + std::to_string(c2.value) + "]");
  REQUIRE(row2 != nullptr);
  u.d.drag(u.title(u.sat), mid(row2->rect()), 10);
  CHECK(u.history() == h0 + 2);
  CHECK(u.rackOf(u.rack).chains[0].devices.empty());
  CHECK(u.rackOf(u.rack).chains[1].devices.size() == 2);
  // Out of the rack again: onto the track's chain, just left of the rack.
  u.view().showChain(u.rack, c2);
  const IRect rf = u.frame(u.rack);
  u.d.drag(u.title(u.util), {static_cast<float>(rf.x + 3), static_cast<float>(rf.y + 40)}, 10);
  CHECK(u.history() == h0 + 3);
  // TX.all-free: spliced into the main path, right before the rack
  CHECK(u.path() == std::vector<doc::DeviceId>{u.inst, u.util, u.rack});
  for (int k = 0; k < 3; ++k) REQUIRE(u.ctx.undo());
  CHECK(u.path() == std::vector<doc::DeviceId>{u.inst, u.util, u.rack});  // the start: [instrument, utility, rack]
  CHECK(u.rackOf(u.rack).chains.size() == 1);
}

TEST_CASE("rack view: Ctrl+G groups the selection; the circuit shows the rack's chains as parallel group capsules") {
  Ui u;
  const size_t h0 = u.history();
  u.d.click(u.title(u.util));
  REQUIRE(u.view().selection().size() == 1);
  CHECK(u.d.press("Ctrl+G") == KeyResult::Action);
  CHECK(u.history() == h0 + 1);
  const std::vector<doc::DeviceId> p = u.path();
  REQUIRE(p.size() == 3);
  const doc::RackInfo r = u.rackOf(p[1]);
  CHECK(r.chains.at(0).devices.at(0)->id == u.util);
  // A second chain in the first rack: the circuit draws both chains as group capsules side by side in the band, fed from
  // the same block and summed into the same one (the rack is their Device-view form, not a block of its own).
  u.run(doc::ops::addChain(*u.ctx.doc(), u.ctx.store().ids(), u.rack));
  const doc::RackInfo two = u.rackOf(u.rack);
  REQUIRE(two.chains.size() == 2);
  doc::LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  const auto low = doc::lower(*u.ctx.doc(), lo);
  REQUIRE(low.ok());
  const project_patch::ProjectPatch pm = project_patch::buildProjectPatch(*u.ctx.doc(), *low, &library());
  std::vector<const project_patch::Node*> caps;
  for (const project_patch::Node& n : pm.nodes)
    if (n.kind == project_patch::NodeKind::Device && (n.device == two.chains[0].id || n.device == two.chains[1].id))
      caps.push_back(&n);
  REQUIRE(caps.size() == 2);
  CHECK(caps[0]->group);
  CHECK(caps[1]->group);
  CHECK(!caps[0]->offPath);  // both laid out on the band's path (stacked), neither in the free row
  CHECK(!caps[1]->offPath);
  CHECK(std::fabs(caps[0]->x - caps[1]->x) < 1.0f);  // parallel: one above the other
  CHECK(caps[0]->y != caps[1]->y);
}

TEST_CASE("rack view: map mode — a macro, then a knob maps it (one step); again unmaps; Esc ends map mode") {
  Ui u;
  DeviceChainView& v = u.view();
  v.setMapMode(u.rack);
  CHECK(v.mapRack() == u.rack);
  u.d.tree().layoutNow();
  // Click macro 2, then the saturator's drive knob.
  const std::optional<IRect> m2 = v.controlRect(u.rack, "macro2");
  REQUIRE(m2.has_value());
  const size_t h0 = u.history();
  u.d.click(mid(*m2));
  CHECK(v.mapMacro() == 1);
  CHECK(u.history() == h0);  // picking a macro is view state
  u.d.click(u.knob(u.drive));
  CHECK(u.history() == h0 + 1);
  {
    const doc::RackInfo r = u.rackOf(u.rack);
    REQUIRE(r.mappings.size() == 1);
    CHECK(r.mappings[0].device == u.drive);
    CHECK(r.mappings[0].param == doc::kValueParam);
    CHECK(r.mappings[0].macro == 1);
    // The macro took over the knob's position (its first mapping: Live): a value block made for it.
    REQUIRE(r.macros[1].block.valid());
    CHECK(r.macros[1].value == doctest::Approx(0.5 * doc::kMacroMax).epsilon(0.02));  // drive 0 dB: near mid travel
  }
  // The mapped knob shows the mapping's range as a ring.
  // A second click on it unmaps (one step).
  u.d.click(u.knob(u.drive));
  CHECK(u.history() == h0 + 2);
  CHECK(u.rackOf(u.rack).mappings.empty());
  // Knobs outside the rack are not targets: a click on the Utility's Width knob changes nothing.
  u.d.click(u.knob(test_blocks::member(*u.ctx.doc(), u.util, "Width")));
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
  u.run(doc::ops::mapMacro(*u.ctx.doc(), u.ctx.store().ids(), u.rack, 0, u.drive, std::string(doc::kValueParam)));
  REQUIRE(v.panelOf(u.rack) != nullptr);
  u.d.tree().layoutNow();
  const size_t h0 = u.history();
  const std::optional<IRect> m1 = v.controlRect(u.rack, "macro1");
  REQUIRE(m1.has_value());
  const Vec2 c = mid(*m1);
  u.d.drag(c, {c.x, c.y - 40.0f}, 6);
  CHECK(u.history() == h0 + 1);
  const doc::ChangeSet& cs = u.ctx.store().history().entries()[h0].changes;
  CHECK(cs.paramOnly());
  REQUIRE(cs.params.size() == 1);
  CHECK(cs.params[0] == doc::ParamRef::device(u.rackOf(u.rack).macros[0].block, std::string(doc::kValueParam)));
  CHECK(u.rackOf(u.rack).macros[0].value > 10.0);
  // The chain row's volume box.
  const doc::ChainId c1 = u.rackOf(u.rack).chains[0].id;
  Widget* vol = u.in(u.rack, "chains/chain[" + std::to_string(c1.value) + "]/volume");
  REQUIRE(vol != nullptr);
  const Vec2 vc = mid(vol->rect());
  u.d.drag(vc, {vc.x, vc.y + 30.0f}, 6);
  CHECK(u.history() == h0 + 2);
  CHECK(u.ctx.store().history().entries()[h0 + 1].changes.paramOnly());
  CHECK(u.rackOf(u.rack).chains[0].volumeDb < 0.0f);
  // The zone editor (Chain tab): drag the zone's right end left — the zone switches on (structural), one step.
  v.showZones(u.rack, doc::ZoneKind::Select);
  Widget* zones = u.in(u.rack, "zones");
  REQUIRE(zones != nullptr);
  REQUIRE(zones->visible());
  const IRect zr = zones->rect();
  const float rowY = static_cast<float>(zr.y + 20 + 15);
  u.d.drag({static_cast<float>(zr.right() - 6), rowY}, {static_cast<float>(zr.x + zr.w / 2), rowY}, 6);
  CHECK(u.history() == h0 + 3);
  const doc::Zone z = u.rackOf(u.rack).chains[0].select;
  CHECK(u.rackOf(u.rack).chains[0].selectZone.valid());  // the zone is a group in the chain now
  CHECK(u.rackOf(u.rack).selector.valid());              // and the rack has its selector
  CHECK(z.hi < 100.0);
  CHECK(z.hi > 40.0);
  CHECK_FALSE(doc::zoneOff(z, doc::ZoneKind::Select, doc::KeyZoneUnit::Hz));
  for (int k = 0; k < 3; ++k) REQUIRE(u.ctx.undo());
  CHECK(doc::zoneOff(u.rackOf(u.rack).chains[0].select, doc::ZoneKind::Select, doc::KeyZoneUnit::Hz));
  CHECK_FALSE(u.rackOf(u.rack).chains[0].selectZone.valid());
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
    CHECK_FALSE(doc::zoneOff(u.rackOf(u.rack).chains[0].select, doc::ZoneKind::Select, doc::KeyZoneUnit::Hz));
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
  // Rename (GU.9: a device inside the rack's chain; the rack itself has no name of its own; GU.11: a leaf — an atom
  // block after the Saturator — whose frame is a leaf's BlockFrame): a field over the title; typing replaces the selected
  // name; Enter: one step.
  const doc::DeviceId leaf =
      u.run(doc::ops::addToChain(*u.ctx.doc(), u.ctx.store().ids(), u.rackOf(u.rack).chains[0].id, "atom:tanh", doc::DeviceKind::AudioEffect))
          .devices.at(0);
  u.d.tree().layoutNow();
  const size_t h0 = u.history();
  const doc::DeviceId leafOnly[1] = {leaf};
  v.select(leafOnly);
  CHECK(u.d.press("Ctrl+R") == KeyResult::Action);
  panel::BlockFrame* p = v.panelOf(leaf);
  REQUIRE(p != nullptr);
  CHECK(p->renaming());
  CHECK(u.d.type("Dirt"));
  u.d.press("Return");
  u.d.step();
  CHECK_FALSE(p->renaming());
  CHECK(u.history() == h0 + 1);
  CHECK(doc::findDevice(*u.ctx.doc(), leaf).device->name == "Dirt");
  CHECK(v.panelOf(leaf)->title() == "Dirt");
  // Esc cancels.
  u.d.tree().setFocus(&v);
  u.d.press("Ctrl+R");
  CHECK(u.d.type("Nope"));
  u.d.press("Escape");
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(doc::findDevice(*u.ctx.doc(), leaf).device->name == "Dirt");
  // Ungroup: the Saturator (and the leaf) back in the track's chain where the rack was, selected; one step.
  u.d.tree().setFocus(&v);
  v.select(one);
  CHECK(u.d.press("Ctrl+Shift+G") == KeyResult::Action);
  CHECK(u.history() == h0 + 2);
  CHECK_FALSE(doc::findDevice(*u.ctx.doc(), u.rack).device);
  CHECK(u.path() == std::vector<doc::DeviceId>{u.inst, u.util, u.sat, leaf});
  REQUIRE(v.selection().size() == 2);
  CHECK(v.selection()[0] == u.sat);
  REQUIRE(u.ctx.undo());
  CHECK(doc::findDevice(*u.ctx.doc(), u.rack).device);
}

// M4 review L3: a knob mapped to a macro that an outer rack's macro maps shows the value as played (down the chain of
// mappings from the outer macro), not the inner macro's stored value.
TEST_CASE("rack view: a knob under nested macro mappings shows the played value (M4 review L3)") {
  // GU.11 / GU.13: on a leaf's knob in the rack's chain (a Wavetable's position, its face's control: its panel's target
  // shows the played value — the position the outer macro puts it at, down the chain of mappings).
  Ui u;
  DeviceChainView& v = u.view();
  const doc::DeviceId wt =
      u.run(doc::ops::addToChain(*u.ctx.doc(), u.ctx.store().ids(), u.rackOf(u.rack).chains[0].id, "meta:wavetable", doc::DeviceKind::AudioEffect))
          .devices.at(0);
  const doc::DeviceId outer = u.run(doc::ops::groupDevices(*u.ctx.doc(), u.ctx.store().ids(), std::vector<doc::DeviceId>{u.rack})).devices.at(0);
  u.run(doc::ops::mapMacro(*u.ctx.doc(), u.ctx.store().ids(), u.rack, 0, wt, "position", 0.0));  // inner macro 1 -> position
  const doc::DeviceId innerMacro = u.rackOf(u.rack).macros[0].block;
  // outer macro 1 -> inner macro 1 (its value block's knob)
  u.run(doc::ops::mapMacro(*u.ctx.doc(), u.ctx.store().ids(), outer, 0, innerMacro, std::string(doc::kValueParam), 0.0));
  u.run(doc::ops::setMacroValue(*u.ctx.doc(), outer, 0, doc::kMacroMax));  // the inner macro's stored knob stays 0
  u.d.tree().layoutNow();
  v.refreshRings();
  panel::BlockFrame* panel = v.panelOf(wt);
  REQUIRE(panel != nullptr);
  const doc::RackInfo inner = u.rackOf(u.rack);
  REQUIRE(inner.mappings.size() == 1);
  const doc::RackMapping* mapping = &inner.mappings[0];
  CHECK(panel->target().position(doc::controlBinding(wt, "position")) == doctest::Approx(mapping->max).epsilon(1e-6));
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
  const std::vector<doc::DeviceId> p = u.path();
  REQUIRE(p.size() == 4);
  CHECK(p[1] == u.util);
  const std::optional<doc::RackInfo> made = doc::rackOf(*u.ctx.doc(), p[2]);
  REQUIRE(made.has_value());
  CHECK_FALSE(made->instrument);
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
