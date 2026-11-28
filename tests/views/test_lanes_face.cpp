// FD.2 (docs/plans/FD.md "FD.2"): the lane modules in the views.
//   - REAL DISPATCH in the shell's project patch view (window → overlay → widget): Tab on empty canvas, the name typed,
//     Enter places Lanes, Merge, Unpack and Pack (each one undo step, a closed group drawn as its panel, 8 lanes); a click
//     on a cell of the N chip runs ops::setLaneCount (one undo step; undo restores N; Unpack's face grows a row of jacks
//     at 16); a cable dragged from an Unpack lane jack onto a Pack lane jack wires those two lanes; a cable dragged from
//     Lanes' output onto Merge's input is a LANE CABLE: its edge and its scene wire carry the lane count (the canvas
//     draws the ribbon and the badge from it), the lane wire between the jacks does not.
//   - GOLDENS: lanes-face-lanes / -merge / -unpack / -pack (8 lanes) and lanes-face-unpack16, and lanes-cable (the patch
//     view: Lanes → Delay Tap → Merge → VCA as free blocks, the lane cables as ribbons with their "8", the merged cable
//     plain).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lanes.h"
#include "pq/document/main_path.h"
#include "pq/document/modules.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/node_catalog.h"
#include "pq/views/circuit/node_menu.h"
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

// The faces' geometry (modules.cpp laneModulePanel).
constexpr float kPitch = 17.0f, kRow = 24.0f;
Vec2 laneJack(uint32_t i) { return {8.0f + kPitch * (static_cast<float>(i % 8) + 0.5f), 36.0f + kRow * static_cast<float>(i / 8)}; }
// The centre of the N chip's cell for `lanes` on a chip at (x, y), `w` wide, 14 high.
Vec2 chipCell(float x, float y, float w, uint32_t lanes) {
  const float cell = (w - 6.0f) / 4.0f;
  const uint32_t k = lanes == 2 ? 0u : lanes == 4 ? 1u : lanes == 8 ? 2u : 3u;
  return {x + static_cast<float>(k) * (cell + 2.0f) + 0.5f * cell, y + 7.0f};
}
Vec2 smallChip(uint32_t lanes) { return chipCell(10.0f, 25.0f, 100.0f, lanes); }   // Lanes, Merge
Vec2 unpackChip(uint32_t lanes) { return chipCell(152.0f - 8.0f - 76.0f, 5.0f, 76.0f, lanes); }

// An audio track in the shell's project patch view.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  doc::TrackId track;

  Ui() {
    circuit::clearRecentNodes();
    ctx.setDeviceLibrary(&devLib());
    doc::DocumentStore& st = ctx.store();
    auto e = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "Lanes");
    REQUIRE(e.ok());
    REQUIRE(st.dispatch(e->command).ok());
    track = e->tracks.at(0);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    view().focusBand(track, false);
    d.tree().layoutNow();
    frame();
    d.tree().setFocus(&view().canvas());
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }
  // Zoom 1 on the track's band, room below and right of it for the blocks placed.
  void frame() {
    view().sync();
    const Band& b = view().patch().bands[view().patch().bandOf(track)];
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = b.x0 - 40.0f;
    v.y = b.y - 40.0f;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
  }
  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const ProjectPatch& pp() const { return view().patch(); }
  size_t history() const { return ctx.store().history().cursor(); }
  const Node& node(doc::DeviceId g) const {
    const uint32_t n = pp().nodeOf(g);
    REQUIRE(n != kNone);
    return pp().nodes[n];
  }
  // A point of block `g`'s face (its panel's units) in device px.
  Vec2 at(doc::DeviceId g, Vec2 local) const {
    const Node& n = node(g);
    return view().canvas().toDevice({n.x + local.x * n.scale, n.y + local.y * n.scale});
  }
  circuit::NodeMenu* menu() const {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<circuit::NodeMenu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  doc::DeviceId newest() const {
    doc::DeviceId best;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      for (const doc::DevicePtr& x : t.devices)
        if (x->id.value > best.value) best = x->id;
    });
    if (const doc::DeviceLoc l = doc::findDevice(*ctx.doc(), best); l.device != nullptr && l.device->group.valid())
      best = doc::outermostGroup(l.track->devices, best);
    return best;
  }
  // Tab at world point `w`, `query` typed, Enter: the block placed (the highlighted entry must be `name`).
  doc::DeviceId place(graph::Point2 w, std::string_view query, std::string_view name) {
    const size_t h0 = history();
    d.move(view().canvas().toDevice(w));
    REQUIRE(d.press("Tab") == KeyResult::Widget);
    d.step();
    REQUIRE(menu() != nullptr);
    d.type(query);
    REQUIRE(menu()->highlightedEntry() != nullptr);
    CHECK(menu()->highlightedEntry()->name == name);
    d.press("Return");
    d.step();
    view().sync();
    CHECK(menu() == nullptr);
    CHECK(history() == h0 + 1);
    d.tree().layoutNow();
    d.step();
    return newest();
  }
  doc::LaneModuleInfo info(doc::DeviceId g) const {
    const std::optional<doc::LaneModuleInfo> m = doc::laneModuleInfo(*ctx.doc(), g);
    REQUIRE(m.has_value());
    return *m;
  }
  // The band's empty canvas: column `col`, row `row` of a grid right of / under its lane.
  graph::Point2 spot(uint32_t col, uint32_t row) const {
    const Band& b = pp().bands[pp().bandOf(track)];
    return {b.x0 + 200.0f + 260.0f * static_cast<float>(col), b.y + 220.0f + 180.0f * static_cast<float>(row)};
  }
  const doc::Wire* wireFromTo(doc::DeviceId from, doc::DeviceId to) const {
    for (const doc::Wire& w : doc::wiresOf(*ctx.doc()))
      if (!w.fromNoteSource() && !w.toMixer() && w.from == from && w.to == to) return &w;
    return nullptr;
  }
  uint8_t edgeLanes(doc::WireId w) const {
    for (const Edge& e : pp().edges)
      if (e.wire == w) return e.lanes;
    FAIL("no edge for the wire");
    return 0;
  }
  // The lane count on the scene wire(s) of document wire `w` (what the canvas draws from).
  uint8_t sceneLanes(doc::WireId w) const {
    const ProjectScene& ps = view().scene();
    for (uint32_t k = 0; k < ps.modelEdge.size(); ++k)
      if (ps.modelEdge[k] != kNone && ps.modelEdge[k] < pp().edges.size() && pp().edges[ps.modelEdge[k]].wire == w)
        return ps.scene->wires[k].style.lanes;
    FAIL("no scene wire for the wire");
    return 0;
  }
};

// One module's face alone in a plain tree (as Standalone does a panel).
Image faceImage(doc::LaneModule kind, uint32_t lanes) {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  doc::DocumentStore& st = ctx.store();
  auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "Face");
  REQUIRE(t.ok());
  REQUIRE(st.dispatch(t->command).ok());
  doc::IdSource local;
  doc::LaneModuleOptions o;
  o.lanes = lanes;
  auto e = doc::ops::pasteBlocks(*st.current(), st.ids(), t->tracks.at(0), doc::laneModuleBlocks(local, kind, o));
  REQUIRE(e.ok());
  REQUIRE(st.dispatch(e->command).ok());
  const doc::DeviceId g = e->devices.at(0);
  const doc::Panel* p = doc::findDevice(*ctx.doc(), g).device->panel.get();
  REQUIRE(p != nullptr);
  constexpr float kScale = 2.0f;
  TestDriver d(static_cast<uint32_t>(p->width * kScale), static_cast<uint32_t>(p->height * kScale), kScale);
  panel::PanelHost* host = nullptr;
  auto f = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, g),
                                                   panel::makeDocPanelTarget(ctx, g, [&host]() -> Widget* { return host; }));
  host = &d.setRoot<panel::PanelHost>("panel", std::move(f));
  d.tree().layoutNow();
  REQUIRE(d.enableGpu().ok());
  d.advance(0.05);
  auto img = d.render();
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  Image out = std::move(*img);
  d.tree().setRoot(std::make_unique<Widget>());
  return out;
}

}  // namespace

TEST_CASE("lanes: Tab, the name, Enter places Lanes / Merge / Unpack / Pack; the N chip, the jacks and a lane cable — "
          "real dispatch") {
  Ui u;
  const doc::DeviceId fan = u.place(u.spot(0, 0), "lanes", "Lanes");
  const doc::DeviceId merge = u.place(u.spot(1, 0), "merge", "Merge");
  const doc::DeviceId unpack = u.place(u.spot(0, 1), "unpack", "Unpack");
  const doc::DeviceId pack = u.place(u.spot(1, 1), "lane_pack", "Pack");
  const std::pair<doc::DeviceId, doc::LaneModule> placed[] = {
      {fan, doc::LaneModule::Lanes}, {merge, doc::LaneModule::Merge}, {unpack, doc::LaneModule::Unpack}, {pack, doc::LaneModule::Pack}};
  for (const auto& [g, kind] : placed) {
    CAPTURE(doc::laneModuleName(kind));
    const doc::LaneModuleInfo li = u.info(g);
    CHECK(li.kind == kind);
    CHECK(li.lanes == doc::kDefaultLaneCount);
    CHECK_FALSE(u.view().groupOpen(g));  // closed: drawn as its panel
    CHECK(u.node(g).groupPanel);
  }
  CHECK(circuit::recentNodes().front() == "module:lane_pack");

  // The N chip: a click on "4" of Lanes — one undo step, the inside rebuilt; undo restores 8.
  size_t h0 = u.history();
  u.d.click(u.at(fan, smallChip(4)));
  u.d.step();
  CHECK(u.info(fan).lanes == 4);
  CHECK(u.history() == h0 + 1);
  CHECK(u.info(fan).roles.inner.size() == 2 + 2);  // L, R and two packs
  u.d.click(u.at(fan, smallChip(4)));  // the lit cell: nothing
  u.d.step();
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.ctx.undo());
  u.d.step();
  CHECK(u.info(fan).lanes == 8);
  // Unpack to 16: a second row of jacks, the face a row taller.
  const float tall = u.node(unpack).h;
  h0 = u.history();
  u.d.click(u.at(unpack, unpackChip(16)));
  u.d.step();
  u.view().sync();
  u.d.step();
  CHECK(u.info(unpack).lanes == 16);
  CHECK(u.info(unpack).roles.lane.size() == 16);
  CHECK(u.history() == h0 + 1);
  CHECK(u.node(unpack).h > tall + 20.0f * u.node(unpack).scale);
  {
    const Node& n = u.node(unpack);
    uint32_t jacks = 0;
    for (const Port& p : n.outputs) jacks += p.jack ? 1u : 0u;
    CHECK(jacks == 16);
  }

  // A cable from Unpack's lane 4 jack onto Pack's lane 1 jack: those two lane blocks wired, one undo step.
  const doc::LaneModuleInfo ui = u.info(unpack), pi = u.info(pack);
  h0 = u.history();
  REQUIRE(u.view().canvas().pinAt(u.at(unpack, laneJack(3))).valid());
  CHECK_FALSE(u.view().canvas().pinAt(u.at(unpack, laneJack(3))).input);
  REQUIRE(u.view().canvas().pinAt(u.at(pack, laneJack(0))).valid());
  CHECK(u.view().canvas().pinAt(u.at(pack, laneJack(0))).input);
  u.d.drag(u.at(unpack, laneJack(3)), u.at(pack, laneJack(0)), 8);
  u.d.step();
  const doc::Wire* lane = u.wireFromTo(ui.roles.lane[3], pi.roles.lane[0]);
  REQUIRE(lane != nullptr);
  CHECK(u.history() == h0 + 1);
  const doc::WireId laneWire = lane->id;

  // A cable from Lanes' output onto Merge's input: a lane cable of 8.
  bool docked = false;
  const std::optional<Vec2> from = u.view().outputPoint(fan, "y", &docked);
  REQUIRE(from);
  REQUIRE(docked);
  const Node& mn = u.node(merge);
  REQUIRE(mn.mainIn >= 0);
  const Vec2 to = u.view().canvas().toDevice(u.view().canvas().portWorld(u.pp().nodeOf(merge), static_cast<uint32_t>(mn.mainIn), true));
  h0 = u.history();
  u.d.drag(*from, to, 8);
  u.d.step();
  const doc::Wire* cable = u.wireFromTo(fan, merge);
  REQUIRE(cable != nullptr);
  CHECK(u.history() == h0 + 1);
  u.view().sync();
  u.d.step();
  CHECK(doc::laneWidths(*u.ctx.doc()).of(cable->id) == 8);
  CHECK(u.edgeLanes(cable->id) == 8);
  CHECK(u.sceneLanes(cable->id) == 8);
  CHECK(u.edgeLanes(laneWire) == 0);  // one lane: an ordinary cable
  CHECK(u.sceneLanes(laneWire) == 0);
  // Lanes set to 16: the cable follows.
  u.d.click(u.at(fan, smallChip(16)));
  u.d.step();
  u.view().sync();
  u.d.step();
  CHECK(u.edgeLanes(cable->id) == 16);
  CHECK(u.sceneLanes(cable->id) == 16);
}

TEST_CASE("render: the lane modules' faces match their goldens") {
  checkImage("lanes-face-lanes", faceImage(doc::LaneModule::Lanes, 8));
  checkImage("lanes-face-merge", faceImage(doc::LaneModule::Merge, 8));
  checkImage("lanes-face-unpack", faceImage(doc::LaneModule::Unpack, 8));
  checkImage("lanes-face-pack", faceImage(doc::LaneModule::Pack, 8));
  checkImage("lanes-face-unpack16", faceImage(doc::LaneModule::Unpack, 16));
}

TEST_CASE("render: lanes-cable — lane cables are ribbons with their lane count, the merged cable is plain") {
  Ui u;
  // Free blocks a cable's length apart (placed as the node menu places them), cabled Lanes → Delay Tap → Merge → VCA.
  const doc::DeviceId fan = u.place(u.spot(0, 0), "lanes", "Lanes");
  const doc::DeviceId tap = u.place(u.spot(1, 0), "delay tap", "Delay Tap");
  const doc::DeviceId merge = u.place(u.spot(2, 0), "merge", "Merge");
  const doc::DeviceId vca = u.place(u.spot(3, 0), "vca", "VCA");
  auto cable = [&](doc::DeviceId from, doc::DeviceId to) {
    doc::Wire w;
    w.id = u.ctx.store().ids().wire();
    w.from = from;
    w.to = to;
    REQUIRE(u.ctx.store().dispatch(doc::AddWire{w}).ok());
    return w.id;
  };
  const doc::WireId a = cable(fan, tap), b = cable(tap, merge), c = cable(merge, vca);
  u.view().sync();
  {
    const Node& n = u.node(fan);
    circuit::CircuitCanvas::View v;
    v.zoom = 1.25;
    v.x = n.x - 60.0;
    v.y = n.y - 160.0;
    u.view().canvas().setView(v);
    u.d.tree().layoutNow();
    u.d.step();
  }
  CHECK(u.edgeLanes(a) == 8);
  CHECK(u.edgeLanes(b) == 8);
  CHECK(u.edgeLanes(c) == 0);  // merged: an ordinary cable
  const double t0 = u.d.now();
  while (u.d.now() - t0 < 5.0 && (u.view().canvas().cablesMoving() || u.d.tree().needsFrame())) u.d.advance(1.0 / 144.0);
  REQUIRE(u.d.enableGpu().ok());
  u.d.advance(0.05);
  auto img = u.d.render();
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  checkImage("lanes-cable", *img);
}
