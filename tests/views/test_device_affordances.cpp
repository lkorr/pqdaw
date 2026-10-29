// T4.16c Device view affordances through the real shell: Live's arrow keys (Left / Right walk the chain into and out of
// rack chains, Shift extends the selection — then Ctrl+G groups it —, Home / End, Ctrl+Left / Right move a device one
// step = one undo step, Up / Down step through a rack's chains), the hint a multi-selection shows, and the mouse cursor
// over every interactive part of a device and a rack frame (each checked against what a press there does). Renaming a
// chain in place.

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/fake_transport.h"
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

// One MIDI track: BasicSynth → Utility → an Audio Effect Rack holding a Saturator (one chain), in the Device view.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  doc::TrackId keys;
  doc::DeviceId inst, util, sat, rack;

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
  std::vector<doc::DeviceId> chain() const {
    std::vector<doc::DeviceId> v;
    for (const doc::DevicePtr& dev : track().devices) v.push_back(dev->id);
    return v;
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
  Vec2 title(doc::DeviceId id) {
    const IRect r = frame(id);
    devices::DevicePanel* p = view().panelOf(id);
    const int32_t strip = p != nullptr && !p->signature().exports.empty() ? 10 : 0;
    return {static_cast<float>(r.x + r.w / 2), static_cast<float>(r.y + strip + 10)};
  }
  std::vector<doc::DeviceId> selection() const { return {view().selection().begin(), view().selection().end()}; }
  // The cursor with the pointer at p (a move there first).
  Cursor at(Vec2 p) {
    d.move(p);
    return d.tree().cursor();
  }
};

using Ids = std::vector<doc::DeviceId>;

}  // namespace

TEST_CASE("device keys: Left / Right walk the chain into and out of a rack's chain; Home / End; no history") {
  Ui u;
  DeviceChainView& v = u.view();
  CHECK(v.shownOrder() == Ids{u.inst, u.util, u.rack, u.sat});  // display order: the rack, then the chain it shows
  const size_t h0 = u.history();
  u.d.click(u.title(u.util));
  REQUIRE(u.selection() == Ids{u.util});
  CHECK(u.d.press("Right") == KeyResult::Action);
  CHECK(u.selection() == Ids{u.rack});
  CHECK(u.d.press("Right") == KeyResult::Action);
  CHECK(u.selection() == Ids{u.sat});  // into the rack's chain
  u.d.press("Right");
  CHECK(u.selection() == Ids{u.sat});  // the end: stays
  u.d.press("Left");
  CHECK(u.selection() == Ids{u.rack});  // out of it again
  u.d.press("Left");
  u.d.press("Left");
  CHECK(u.selection() == Ids{u.inst});
  u.d.press("Left");
  CHECK(u.selection() == Ids{u.inst});
  // End / Home: the selected device's list — the track's chain here, the rack's chain from inside it.
  u.d.press("End");
  CHECK(u.selection() == Ids{u.rack});
  u.d.press("Home");
  CHECK(u.selection() == Ids{u.inst});
  v.select(Ids{u.sat});
  u.d.press("Home");
  CHECK(u.selection() == Ids{u.sat});
  // A folded rack hides its devices: Right steps over them.
  v.select(Ids{u.util});
  v.panelOf(u.rack)->setFolded(true);
  u.d.tree().layoutNow();
  CHECK(v.shownOrder() == Ids{u.inst, u.util, u.rack});
  v.panelOf(u.rack)->setFolded(false);
  // Nothing selected (a click on empty space: the gap after Utility): Right takes the first device.
  const IRect fu = u.frame(u.util);
  const Vec2 empty{static_cast<float>(fu.right() + 2), static_cast<float>(fu.y + fu.h / 2)};
  u.d.click(empty);
  REQUIRE(u.selection().empty());
  u.d.press("Right");
  CHECK(u.selection() == Ids{u.inst});
  CHECK(u.history() == h0);  // selection is view state
}

TEST_CASE("device keys: the selection scrolls into view") {
  Ui u;
  DeviceChainView& v = u.view();
  for (int k = 0; k < 6; ++k) u.run(doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.keys, "eq_eight", doc::DeviceKind::AudioEffect));
  u.d.tree().layoutNow();
  const doc::DeviceId last = u.track().devices.back()->id;
  const IRect vp = v.chainViewport();
  REQUIRE(u.frame(last).x > vp.right());  // scrolled away
  u.d.click(u.title(u.inst));
  u.d.press("End");
  REQUIRE(u.selection() == Ids{last});
  const IRect f = u.frame(last);
  CHECK(f.x >= vp.x);
  CHECK(f.right() <= vp.right());
  u.d.press("Home");
  const IRect fi = u.frame(u.inst);
  CHECK(fi.x >= vp.x);
}

TEST_CASE("device keys: Shift+Left / Right extend the selection along one list; the hint names Ctrl+G, which groups it") {
  Ui u;
  const size_t h0 = u.history();
  u.d.click(u.title(u.util));
  CHECK(u.d.press("Shift+Right") == KeyResult::Action);
  CHECK(u.selection() == Ids{u.util, u.rack});
  CHECK(u.ctx.status().find("Ctrl+G") != std::string::npos);  // discoverability: what to do with several devices
  u.d.press("Shift+Right");
  CHECK(u.selection() == Ids{u.util, u.rack});  // the list's end (Shift stays in the track's chain)
  u.d.press("Shift+Left");
  CHECK(u.selection() == Ids{u.util});  // back over the selection: it shrinks
  u.d.press("Shift+Right");
  REQUIRE(u.selection() == Ids{u.util, u.rack});
  CHECK(u.d.press("Ctrl+G") == KeyResult::Action);
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.track().devices.size() == 2);  // the instrument and the new rack around Utility and the old rack
  const doc::Rack& outer = u.rackOf(u.track().devices[1]->id);
  REQUIRE(outer.chains.size() == 1);
  CHECK(outer.chains[0]->devices.size() == 2);
  // A Ctrl+click multi-selection shows the hint too.
  REQUIRE(u.ctx.undo());
  u.ctx.setStatus("");
  u.d.click(u.title(u.util));
  u.d.click(u.title(u.rack), mod::kCtrl);
  CHECK(u.selection() == Ids{u.util, u.rack});
  CHECK(u.ctx.status().find("Ctrl+G") != std::string::npos);
}

TEST_CASE("device keys: Ctrl+Left / Right move the selected device one step (one undo step); the instrument stays first") {
  Ui u;
  const size_t h0 = u.history();
  u.d.click(u.title(u.rack));
  CHECK(u.d.press("Ctrl+Left") == KeyResult::Action);
  CHECK(u.history() == h0 + 1);
  CHECK(u.chain() == Ids{u.inst, u.rack, u.util});
  CHECK(u.selection() == Ids{u.rack});  // it stays selected
  u.d.press("Ctrl+Left");                // not before the instrument
  CHECK(u.history() == h0 + 1);
  CHECK(u.ctx.status().find("instrument") != std::string::npos);
  u.d.press("Ctrl+Right");
  CHECK(u.history() == h0 + 2);
  CHECK(u.chain() == Ids{u.inst, u.util, u.rack});
  u.d.press("Ctrl+Right");  // the end: nothing
  CHECK(u.history() == h0 + 2);
  u.view().select(Ids{u.inst});  // the instrument does not move either
  u.d.press("Ctrl+Right");
  CHECK(u.history() == h0 + 2);
  // Inside a rack chain: along that chain.
  const doc::ChainId c1 = u.rackOf(u.rack).chains[0]->id;
  const doc::DeviceId eq = u.run(doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), {}, "eq_eight", doc::DeviceKind::AudioEffect,
                                                     doc::kAppend, c1)).devices[0];
  const size_t h1 = u.history();
  u.view().select(Ids{eq});
  u.d.press("Ctrl+Left");
  CHECK(u.history() == h1 + 1);
  REQUIRE(u.rackOf(u.rack).chains[0]->devices.size() == 2);
  CHECK(u.rackOf(u.rack).chains[0]->devices[0]->id == eq);
  // Two selected: not moved (the status says why).
  u.view().select(Ids{u.util, u.rack});
  u.d.press("Ctrl+Left");
  CHECK(u.history() == h1 + 1);
  for (int k = 0; k < 4; ++k) REQUIRE(u.ctx.undo());  // the move in the chain, the add, the two key moves
  CHECK(u.chain() == Ids{u.inst, u.util, u.rack});
}

TEST_CASE("device keys: after Delete the next device (else the previous) is selected, so the keys go on from there") {
  Ui u;
  u.d.click(u.title(u.util));
  CHECK(u.d.press("Delete") == KeyResult::Action);
  CHECK(u.chain() == Ids{u.inst, u.rack});
  CHECK(u.selection() == Ids{u.rack});  // the one after it
  u.d.press("Delete");                  // the rack and the Saturator in it
  CHECK(u.chain() == Ids{u.inst});
  CHECK(u.selection() == Ids{u.inst});  // nothing after: the one before
}

TEST_CASE("device keys: Up / Down show the previous / next chain of the selected rack (or of the rack around the device)") {
  Ui u;
  DeviceChainView& v = u.view();
  const doc::DeviceId util2 = u.run(doc::ops::addChainWithDevice(*u.ctx.doc(), u.ctx.store().ids(), u.rack, "utility",
                                                                 doc::DeviceKind::AudioEffect)).devices[0];
  const doc::ChainId c1 = u.rackOf(u.rack).chains[0]->id, c2 = u.rackOf(u.rack).chains[1]->id;
  v.showChain(u.rack, c1);
  const size_t h0 = u.history();
  v.select(Ids{u.sat});
  u.d.tree().setFocus(&v);
  CHECK(u.d.press("Down") == KeyResult::Action);
  CHECK(v.shownChain(u.rack) == c2);
  CHECK(u.selection() == Ids{util2});  // the first device of the chain now shown
  u.d.press("Down");
  CHECK(v.shownChain(u.rack) == c2);  // the last chain
  u.d.press("Up");
  CHECK(v.shownChain(u.rack) == c1);
  CHECK(u.selection() == Ids{u.sat});
  // The rack selected: it stays selected while its chains change.
  v.select(Ids{u.rack});
  u.d.press("Down");
  CHECK(v.shownChain(u.rack) == c2);
  CHECK(u.selection() == Ids{u.rack});
  CHECK(u.history() == h0);  // view state
}

TEST_CASE("device view cursors: title Move, activator / fold Pointer, knobs ResizeNS, add slot Pointer — each as a press does") {
  Ui u;
  DeviceChainView& v = u.view();
  devices::DevicePanel* p = v.panelOf(u.util);
  REQUIRE(p != nullptr);
  Widget* title = p->find("title");
  REQUIRE(title != nullptr);
  const IRect t = title->rect();
  const float cy = static_cast<float>(t.y + t.h / 2);
  const Vec2 activator{static_cast<float>(t.x + 10), cy}, fold{static_cast<float>(t.x + 25), cy};
  CHECK(u.at(activator) == Cursor::Pointer);
  CHECK(u.at(fold) == Cursor::Pointer);
  CHECK(u.at(u.title(u.util)) == Cursor::Move);
  // The activator: a click switches the device off (one step); the fold triangle folds it.
  const size_t h0 = u.history();
  u.d.click(activator);
  CHECK(u.history() == h0 + 1);
  CHECK_FALSE(doc::findDevice(*u.ctx.doc(), u.util).device->active);
  u.d.click(fold);
  CHECK(p->folded());
  u.d.click(fold);
  CHECK_FALSE(p->folded());
  // The title: a drag reorders (Move while dragging, also off the frame).
  const Vec2 from = u.title(u.util);
  const IRect rf = u.frame(u.rack);
  u.d.down(from);
  u.d.move({from.x + 40, from.y});
  u.d.move({static_cast<float>(rf.right() - 30), from.y});
  CHECK(u.d.tree().cursor() == Cursor::Move);
  u.d.up({static_cast<float>(rf.right() - 30), from.y});
  // Knobs: a vertical drag sets them (the drop may have rebuilt the frames: the panel is looked up again).
  p = v.panelOf(u.util);
  REQUIRE(p != nullptr);
  u.d.tree().layoutNow();
  Widget* knob = p->controlFor("width");
  REQUIRE(knob != nullptr);
  CHECK(u.at(mid(knob->rect())) == Cursor::ResizeNS);
  // The add slot opens the device menu (scrolled into view first: the chain may be wider than the window).
  u.d.tree().layoutNow();
  v.scrollChainBy(100000);
  u.d.tree().layoutNow();
  const IRect slot = v.addSlot()->rect();
  CHECK(u.at(mid(slot)) == Cursor::Pointer);
  // Empty space (the gap before the slot): Arrow.
  CHECK(u.at({static_cast<float>(slot.x - 2), static_cast<float>(slot.y + 20)}) == Cursor::Arrow);
}

TEST_CASE("rack cursors: strip buttons, chain list +, rows, zone ends / body / tabs, the rack's title, map mode") {
  Ui u;
  DeviceChainView& v = u.view();
  // The rack's title band (over the chain list) moves the rack; the list's + adds a chain.
  Widget* list = u.in(u.rack, "chains");
  REQUIRE(list != nullptr);
  const IRect lr = list->rect();
  CHECK(u.at({static_cast<float>(lr.x + 40), static_cast<float>(lr.y + 10)}) == Cursor::Move);
  const Vec2 plus{static_cast<float>(lr.right() - 8), static_cast<float>(lr.y + 10)};
  CHECK(u.at(plus) == Cursor::Pointer);
  const size_t h0 = u.history();
  u.d.click(plus);
  CHECK(u.history() == h0 + 1);
  // A chain row: a click shows that chain.
  const doc::ChainId c2 = u.rackOf(u.rack).chains[1]->id;
  Widget* row2 = u.in(u.rack, "chains/chain[" + std::to_string(c2.value) + "]");
  REQUIRE(row2 != nullptr);
  const Vec2 rowName{static_cast<float>(row2->rect().x + 60), static_cast<float>(row2->rect().y + row2->rect().h / 2)};
  CHECK(u.at(rowName) == Cursor::Pointer);
  u.d.click(rowName);
  CHECK(v.shownChain(u.rack) == c2);
  // The strip: its buttons (M: macros shown or hidden, one step); its empty part takes presses but does nothing.
  Widget* strip = u.in(u.rack, "strip");
  REQUIRE(strip != nullptr);
  const IRect sr = strip->rect();
  const Vec2 m{static_cast<float>(sr.x + sr.w / 2), static_cast<float>(sr.y + 24 + 9)};
  CHECK(u.at(m) == Cursor::Pointer);
  CHECK(u.at({static_cast<float>(sr.x + sr.w / 2), static_cast<float>(sr.bottom() - 10)}) == Cursor::Arrow);
  // The zone editor (Chain tab): a bar's right end ↔, its body Move, the header drags the selector ↔, a tab Pointer.
  v.showZones(u.rack, doc::ZoneKind::Select);
  Widget* zones = u.in(u.rack, "zones");
  REQUIRE(zones != nullptr);
  const IRect zr = zones->rect();
  const float rowY = static_cast<float>(zr.y + 20 + 15);
  CHECK(u.at({static_cast<float>(zr.right() - 6), rowY}) == Cursor::ResizeEW);
  CHECK(u.at({static_cast<float>(zr.x + zr.w / 2), rowY}) == Cursor::Move);
  CHECK(u.at({static_cast<float>(zr.x + 20), static_cast<float>(zr.y + 10)}) == Cursor::Pointer);  // the Chain tab
  CHECK(u.at({static_cast<float>(zr.right() - 20), static_cast<float>(zr.y + 10)}) == Cursor::ResizeEW);
  CHECK(u.at({static_cast<float>(zr.x + zr.w / 2), static_cast<float>(zr.bottom() - 4)}) == Cursor::Arrow);  // below the rows
  // A drag on the body keeps Move (the zone editor holds the pointer).
  u.d.down({static_cast<float>(zr.x + zr.w / 2), rowY});
  u.d.move({static_cast<float>(zr.x + zr.w / 2 - 30), rowY + 40});
  CHECK(u.d.tree().cursor() == Cursor::Move);
  u.d.up({static_cast<float>(zr.x + zr.w / 2 - 30), rowY + 40});
  // Map mode: the macros and the knobs inside the rack are Pointer (a click picks / maps), the rest Arrow.
  v.setMapMode(u.rack);
  u.d.tree().layoutNow();
  devices::DevicePanel* rp = v.panelOf(u.rack);
  REQUIRE(rp != nullptr);
  CHECK(u.at(mid(rp->controlFor("macro1")->rect())) == Cursor::Pointer);
  CHECK(u.at(mid(v.panelOf(u.inst)->controlFor("bright")->rect())) == Cursor::Arrow);  // outside the rack: not a target
  v.setMapMode({});
  CHECK(u.at(mid(rp->controlFor("macro1")->rect())) == Cursor::ResizeNS);  // a knob again
}

TEST_CASE("device drags: a drop the document would refuse shows NotAllowed and does nothing") {
  Ui u;
  const size_t h0 = u.history();
  // The instrument dragged onto the Audio Effect Rack's chain row (it holds audio effects only).
  const doc::ChainId c1 = u.rackOf(u.rack).chains[0]->id;
  Widget* row = u.in(u.rack, "chains/chain[" + std::to_string(c1.value) + "]");
  REQUIRE(row != nullptr);
  const Vec2 fi = u.title(u.inst);
  u.d.down(fi);
  u.d.move({fi.x + 10, fi.y + 5});
  u.d.move(mid(row->rect()));
  CHECK(u.d.tree().cursor() == Cursor::NotAllowed);
  u.d.up(mid(row->rect()));
  CHECK(u.history() == h0);
  // An allowed drop (Utility onto that row) shows Move and moves it.
  const Vec2 fu = u.title(u.util);
  u.d.down(fu);
  u.d.move({fu.x + 10, fu.y + 5});
  u.d.move(mid(row->rect()));
  CHECK(u.d.tree().cursor() == Cursor::Move);
  u.d.up(mid(row->rect()));
  CHECK(u.history() == h0 + 1);
  // The rack dragged into a rack inside it (the Saturator grouped into its own rack in chain 1): refused.
  const doc::DeviceId inner = u.run(doc::ops::groupDevices(*u.ctx.doc(), u.ctx.store().ids(), Ids{u.sat})).devices[0];
  Widget* innerList = u.in(inner, "chains");
  REQUIRE(innerList != nullptr);
  const Vec2 innerEmpty{static_cast<float>(innerList->rect().x + 40), static_cast<float>(innerList->rect().y + 70)};
  const size_t h1 = u.history();
  const IRect rr = u.frame(u.rack);
  const Vec2 from{static_cast<float>(rr.x + 80), static_cast<float>(rr.y + 10)};  // the macro panel's title, right of its fold
  CHECK(u.at(from) == Cursor::Move);
  u.d.down(from);
  u.d.move({from.x + 10, from.y + 5});
  u.d.move(innerEmpty);
  CHECK(u.d.tree().cursor() == Cursor::NotAllowed);
  u.d.up(innerEmpty);
  CHECK(u.history() == h1);
  CHECK(u.ctx.status().find("itself") != std::string::npos);
}

TEST_CASE("device menus: every add menu lists the racks; the device menu names Group with its key") {
  Ui u;
  // An empty Audio Effect Rack: its slot's menu (a device makes its first chain) offers a nested rack too.
  const doc::DeviceId empty = u.run(doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.keys, std::string(doc::kAudioEffectRack),
                                                        doc::DeviceKind::AudioEffect)).devices[0];
  Widget* slot = u.in(empty, "chain-slot");
  REQUIRE(slot != nullptr);
  u.view().revealDevice(empty);
  u.d.tree().layoutNow();
  CHECK(u.at(mid(slot->rect())) == Cursor::Pointer);
  u.d.click(mid(slot->rect()));
  auto lastMenu = [&]() -> Menu* {
    for (size_t i = u.shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<Menu*>(&u.shell->overlay().child(i))) return m;
    return nullptr;
  };
  Menu* m = lastMenu();
  REQUIRE(m != nullptr);
  bool audioRack = false, instrumentRack = false;
  for (const MenuItem& it : m->items()) {
    audioRack = audioRack || (it.label == "Audio Effect Rack" && it.enabled);
    instrumentRack = instrumentRack || (it.label == "Instrument Rack" && !it.enabled);  // not in an Audio Effect Rack
  }
  CHECK(audioRack);
  CHECK(instrumentRack);
  u.d.press("Escape");
  u.d.step();
  // A device's menu: Group, with Ctrl+G.
  u.view().revealDevice(u.util);
  u.d.click(u.title(u.util), mod::kNone, PointerButton::Right);
  m = lastMenu();
  REQUIRE(m != nullptr);
  bool group = false;
  for (const MenuItem& it : m->items()) group = group || (it.label == "Group" && it.shortcut == "Ctrl+G");
  CHECK(group);
}

TEST_CASE("rack chains: a double-click on a chain's name renames it in place (one undo step); Esc cancels") {
  Ui u;
  const doc::ChainId c1 = u.rackOf(u.rack).chains[0]->id;
  Widget* row = u.in(u.rack, "chains/chain[" + std::to_string(c1.value) + "]");
  REQUIRE(row != nullptr);
  const Vec2 name{static_cast<float>(row->rect().x + 60), static_cast<float>(row->rect().y + row->rect().h / 2)};
  const size_t h0 = u.history();
  u.d.doubleClick(name);
  auto* field = dynamic_cast<TextEdit*>(row->find("rename"));
  REQUIRE(field != nullptr);
  CHECK(u.d.tree().focus() == field);
  u.d.type("Crunch");
  u.d.press("Enter");
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(u.rackOf(u.rack).chains[0]->name == "Crunch");
  CHECK(row->find("rename") == nullptr);  // the field is gone
  // Esc leaves the name as it was.
  u.d.doubleClick(name);
  REQUIRE(row->find("rename") != nullptr);
  u.d.type("Nope");
  u.d.press("Escape");
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(u.rackOf(u.rack).chains[0]->name == "Crunch");
  // A macro: right-click its knob > Rename Macro.
  devices::DevicePanel* rp = u.view().panelOf(u.rack);
  REQUIRE(rp != nullptr);
  u.d.tree().layoutNow();
  u.d.click(mid(rp->controlFor("macro2")->rect()), mod::kNone, PointerButton::Right);
  Menu* menu = nullptr;
  for (size_t i = u.shell->overlay().childCount(); i-- > 0 && menu == nullptr;) menu = dynamic_cast<Menu*>(&u.shell->overlay().child(i));
  REQUIRE(menu != nullptr);
  REQUIRE(menu->items().at(0).label == "Rename Macro");
  menu->choose(0);
  u.d.step();
  auto* mf = dynamic_cast<TextEdit*>(u.view().frameOf(u.rack)->find("macro-rename"));
  REQUIRE(mf != nullptr);
  CHECK(u.d.tree().focus() == mf);
  u.d.type("Tone");
  u.d.press("Enter");
  u.d.step();
  CHECK(u.history() == h0 + 2);
  CHECK(doc::macroDisplayName(u.rackOf(u.rack), 1) == "Tone");
  CHECK(u.view().frameOf(u.rack)->find("macro-rename") == nullptr);
}

TEST_CASE("device panel knobs: the wheel steps them (a run = one undo step), a double-click returns the default (T4.16-fix1)") {
  Ui u;
  devices::DevicePanel* p = u.view().panelOf(u.util);
  REQUIRE(p != nullptr);
  u.d.tree().layoutNow();
  auto* knob = dynamic_cast<ui::ValueControl*>(p->controlFor("width"));
  REQUIRE(knob != nullptr);
  const double v0 = knob->value();
  const Vec2 c = mid(knob->rect());
  const size_t h0 = u.history();
  u.d.move(c);
  u.d.wheel(c, -1.0f);
  u.d.wheel(c, -1.0f);
  CHECK(knob->value() < v0);
  u.d.advance(ui::ValueControl::kWheelGestureSeconds + 0.1);
  CHECK(u.history() == h0 + 1);
  u.d.doubleClick(c);
  CHECK(knob->value() == doctest::Approx(v0));
  CHECK(u.history() == h0 + 2);
}
