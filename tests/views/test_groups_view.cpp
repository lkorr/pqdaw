// TX.groups (SPEC §15 2026-11-01 "resize, groups, and the free-device rack"): groups in the views. In the project patch
// view: select → Ctrl+G makes the group (one undo step; the capsule opened with In, the members, Out, the series as
// wires; the model matches the graph), wiring inside and across the boundary, a member dragged out leaves, closed and
// opened, Ctrl+Shift+G ungroups (the same document). In the Device view: the group is one device of the chain, its
// members are not frames, and its panel is the scoped, embedded circuit of its contents, edited in place. Goldens
// project-patch-group and device-group-panel. Perf: a group panel's frame.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "all_free_helpers.h"
#include "pq/core/alloc_trap.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/fake_transport.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::project_patch;

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  GroupProjectIds ids;

  Ui() {
    ctx.setDeviceLibrary(&library());
    REQUIRE(buildGroupProject(ctx.store(), &ids).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }
  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  void show() {
    view().setShown(true, false);
    d.tree().layoutNow();
  }
  const ProjectPatch& pp() const { return view().patch(); }
  size_t history() const { return ctx.store().history().cursor(); }
  Vec2 dev(float x, float y) const { return view().canvas().toDevice({x, y}); }
  uint32_t node(doc::DeviceId id) const {
    const uint32_t n = pp().nodeOf(id);
    REQUIRE(n != kNone);
    return n;
  }
  // TX.orbit-ports: where the port is drawn (an orbiting main port sits on the block's outline).
  Vec2 outPin(uint32_t n, size_t k = 0) const { return view().canvas().toDevice(view().canvas().portWorld(n, static_cast<uint32_t>(k), false)); }
  Vec2 inPin(uint32_t n, size_t k = 0) const { return view().canvas().toDevice(view().canvas().portWorld(n, static_cast<uint32_t>(k), true)); }
  void selectBlocks(std::initializer_list<doc::DeviceId> ds) {
    std::vector<uint32_t> sn;
    for (const doc::DeviceId d0 : ds) sn.push_back(view().scene().sceneNode[node(d0)]);
    std::sort(sn.begin(), sn.end());
    view().canvas().select(sn, {});
    d.tree().setFocus(&view().canvas());
  }
  doc::DeviceId groupOf(doc::DeviceId member) const { return doc::findDevice(*ctx.doc(), member).device->group; }
};

// Every embedded (scoped) project patch view under `w`.
void scopedViews(Widget& w, std::vector<ProjectPatchView*>& out) {
  if (auto* v = dynamic_cast<ProjectPatchView*>(&w); v != nullptr && v->scope().valid()) out.push_back(v);
  for (size_t i = 0; i < w.childCount(); ++i) scopedViews(w.child(i), out);
}

}  // namespace

TEST_CASE("groups view: select → Ctrl+G groups in one step, the capsule holds In, the members and Out; Ctrl+Shift+G undoes it exactly") {
  Ui u;
  u.show();
  const doc::Snapshot before = u.ctx.doc();
  const size_t h0 = u.history();
  u.selectBlocks({u.ids.pa1, u.ids.wt, u.ids.pa2});
  CHECK(u.d.press("Ctrl+G") != KeyResult::Unhandled);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId g = u.groupOf(u.ids.wt);
  REQUIRE(g.valid());
  CHECK(u.groupOf(u.ids.pa1) == g);
  CHECK(u.groupOf(u.ids.pa2) == g);
  CHECK_FALSE(u.groupOf(u.ids.lfo).valid());  // the free LFO stayed outside
  CHECK(u.view().selectedGroup() == g);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  const uint32_t cap = u.node(g);
  const Node& c = u.pp().nodes[cap];
  REQUIRE(c.group);
  CHECK(c.groupOpen);
  CHECK(c.members.size() == 3);
  REQUIRE(c.groupIn != kNone);
  REQUIRE(c.groupOut != kNone);
  CHECK(u.view().scene().sceneNode[cap] == kNone);  // the capsule is its frame; its contents are scene nodes
  // The keys band's chain is now notes → group → mixer.
  const Band& band = u.pp().bands[c.band];
  size_t chain = 0;  // (GU.11: the series inside the capsule is drawn as chain edges too: not counted here)
  for (const Edge& e : u.pp().edges)
    if (e.kind == EdgeKind::Chain && u.pp().nodes[e.from].band == c.band && u.pp().nodes[e.from].capsule != cap &&
        u.pp().nodes[e.to].capsule != cap)
      ++chain;
  CHECK(chain == 1);  // (GU.11: no instrument heads Keys — the notes reach the group's In: group → mixer)
  CHECK(std::find(band.devices.begin(), band.devices.end(), cap) != band.devices.end());
  // Its series as edges from In to Out through the three modules; the LFO's wire crosses the boundary onto the Wavetable.
  auto edgeBetween = [&](uint32_t a, uint32_t b) {
    for (const Edge& e : u.pp().edges)
      if (e.from == a && e.to == b) return true;
    return false;
  };
  CHECK(edgeBetween(c.groupIn, u.node(u.ids.pa1)));
  CHECK(edgeBetween(u.node(u.ids.pa1), u.node(u.ids.wt)));
  CHECK(edgeBetween(u.node(u.ids.wt), u.node(u.ids.pa2)));
  CHECK(edgeBetween(u.node(u.ids.pa2), c.groupOut));
  // BR.patch-io: the LFO's wire is drawn as two cables through a socket on the frame — into the capsule's port
  // "<param>@<wavetable>" (the closed block's port), on from the In terminal's port of that key onto the Wavetable.
  CHECK_FALSE(edgeBetween(u.node(u.ids.lfo), u.node(u.ids.wt)));
  CHECK(edgeBetween(u.node(u.ids.lfo), cap));
  CHECK(edgeBetween(c.groupIn, u.node(u.ids.wt)));
  for (const Edge& e : u.pp().edges)
    if (e.from == u.node(u.ids.lfo) && e.to == cap) {
      CHECK(e.hops == 2);
      CHECK(e.hop == 0);
      const std::string& key = c.inputs[e.toPort].name;
      CHECK(key.ends_with("@" + std::to_string(u.ids.wt.value)));
      CHECK(u.pp().nodes[c.groupIn].output(key) == static_cast<int32_t>(e.toPort));  // the same socket inside
    }
  for (const uint32_t k : c.members) CHECK(u.pp().nodes[k].capsule == cap);
  // Ctrl+Shift+G with the group selected: the document as before grouping.
  CHECK(u.d.press("Ctrl+Shift+G") != KeyResult::Unhandled);
  u.d.step();
  CHECK(u.history() == h0 + 2);
  CHECK(*u.ctx.doc() == *before);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  REQUIRE(u.ctx.undo());
  CHECK(u.groupOf(u.ids.wt) == g);
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == before);
}

TEST_CASE("groups view: wiring inside a capsule and across it, a block dragged out leaves, closed and opened again") {
  Ui u;
  u.show();
  const doc::DeviceId sel[3] = {u.ids.pa1, u.ids.wt, u.ids.pa2};
  REQUIRE(u.view().groupDevices(sel));
  u.d.step();
  const doc::DeviceId g = u.groupOf(u.ids.wt);
  REQUIRE(g.valid());
  u.view().focusBand(u.ids.keys, false);
  u.d.tree().layoutNow();
  const size_t h0 = u.history();
  // In → Out directly (a dry path beside the series): a drag from the In terminal's pin to the Out terminal's.
  {
    const Node& c = u.pp().nodes[u.node(g)];
    u.d.drag(u.outPin(c.groupIn), u.inPin(u.pp().nodes[u.node(g)].groupOut), 8);
    u.d.step();
    CHECK(u.history() == h0 + 1);
    const doc::Wire& w = doc::wiresOf(*u.ctx.doc()).back();
    CHECK(w.from == g);
    CHECK(w.port == doc::kGroupInPort);
    CHECK(w.to == g);
    CHECK(w.param == doc::kGroupOutPort);
    CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  }
  // A member's output onto a device outside the group (across the boundary): the Bass Saturator's drive.
  {
    u.view().fitAll(false);
    u.d.tree().layoutNow();
    const doc::Track* bass = doc::findTrackPtr(*u.ctx.doc(), u.ids.bass);
    REQUIRE(bass != nullptr);
    REQUIRE(u.view().connect(u.ids.pa2, "out", u.ids.sat, "drive"));
    CHECK(u.history() == h0 + 2);
    CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  }
  // A member dragged out of the capsule (down, under the band) leaves the group (it stays free).
  {
    u.view().focusBand(u.ids.keys, false);
    u.d.tree().layoutNow();
    const Node& c = u.pp().nodes[u.node(g)];
    const Node& p2 = u.pp().nodes[u.node(u.ids.pa2)];
    const Vec2 from = u.dev(p2.x + 6.0f, p2.y + 6.0f);
    const Vec2 to = u.dev(p2.x + 6.0f, c.y + c.h + 40.0f);
    u.d.drag(from, to, 8);
    u.d.step();
    CHECK(u.history() == h0 + 3);
    CHECK_FALSE(u.groupOf(u.ids.pa2).valid());
    CHECK(all_free::offPath(*u.ctx.doc(), u.ids.pa2));
    CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  }
  // Closed: one block with a miniature; its members' wires end on it; opened again.
  u.view().setGroupOpen(g, false);
  u.d.step();
  {
    const Node& c = u.pp().nodes[u.node(g)];
    CHECK_FALSE(c.groupOpen);
    CHECK(c.inner.size() == 2);
    CHECK(c.mini.size() == 4);  // In, two members, Out
    CHECK_FALSE(c.miniWires.empty());
    CHECK(u.view().scene().sceneNode[u.node(g)] != kNone);
    CHECK(u.pp().nodeOf(u.ids.wt) == u.node(g));
    CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  }
  u.view().setGroupOpen(g, true);
  u.d.step();
  CHECK(u.pp().nodes[u.node(g)].groupOpen);
  // Delete with the group selected: the group and its members, one step.
  u.view().selectGroup(g);
  REQUIRE(u.view().deleteSelection());
  CHECK(doc::findDevice(*u.ctx.doc(), g).device == nullptr);
  CHECK(doc::findDevice(*u.ctx.doc(), u.ids.wt).device == nullptr);
  CHECK(doc::findDevice(*u.ctx.doc(), u.ids.pa2).device != nullptr);  // it had left the group
}

// TX.group-resize: an opened group's factor is saved with the project (its nudge's "scale", as a block's) and laid out
// at it when loaded: its members at the factor.
// BR.patch-io (user suggestion 2026-11-13_19-10-25: the group's input and output "should be consistent with the general
// look and feel of the cables and module input/output nodes … if you connect something else to modulate something in the
// group it'll just add another input port"): an opened group's frame carries one socket per connection crossing it (and
// its main In / Out), the closed block's ports, drawn as ports; a crossing wire is two cables through its socket; a press
// and a drop on a socket wire as the In / Out terminals and the closed block's ports did — through real dispatch.
TEST_CASE("groups view (BR.patch-io): an opened group's frame sockets — one per crossing connection, as the closed block's "
          "ports; drags from and onto them — real dispatch") {
  Ui u;
  u.show();
  const doc::DeviceId sel[3] = {u.ids.pa1, u.ids.wt, u.ids.pa2};
  REQUIRE(u.view().groupDevices(sel));
  u.d.step();
  const doc::DeviceId g = u.groupOf(u.ids.wt);
  REQUIRE(g.valid());
  // A second connection out of the group: its last member into the Bass Utility's input (across tracks).
  REQUIRE(u.view().connect(u.ids.pa2, "out", u.ids.util, ""));
  u.d.step();
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Closed: the block's ports. Opened again: the same ports, the same order, on the frame.
  u.view().setGroupOpen(g, false);
  u.d.step();
  std::vector<std::string> closedIn, closedOut;
  {
    const Node& b = u.pp().nodes[u.node(g)];
    REQUIRE_FALSE(b.groupOpen);
    for (const Port& q : b.inputs) closedIn.push_back(q.name);
    for (const Port& q : b.outputs) closedOut.push_back(q.name);
  }
  REQUIRE(closedIn.size() == 2);   // its main input, the LFO's wire onto the Wavetable
  REQUIRE(closedOut.size() == 2);  // its main output, its member's into the Utility
  u.view().setGroupOpen(g, true);
  u.d.step();
  u.view().focusBand(u.ids.keys, false);
  u.d.tree().layoutNow();
  const ProjectPatch& pp = u.pp();
  const uint32_t ci = u.node(g);
  const Node& c = pp.nodes[ci];
  REQUIRE(c.groupOpen);
  std::vector<std::string> openIn, openOut;
  for (const Port& q : c.inputs) openIn.push_back(q.name);
  for (const Port& q : c.outputs) openOut.push_back(q.name);
  CHECK(openIn == closedIn);
  CHECK(openOut == closedOut);
  const Node& in = pp.nodes[c.groupIn];
  const Node& out = pp.nodes[c.groupOut];
  // The terminals are the frame's edges (no block): each socket's inside, at the capsule's port of the same key and row.
  CHECK(in.x == c.x);
  CHECK(out.x == c.x + c.w);
  CHECK(in.w == 0.0f);
  CHECK(out.w == 0.0f);
  REQUIRE(in.outputs.size() == c.inputs.size());
  REQUIRE(out.inputs.size() == c.outputs.size());
  for (size_t k = 0; k < c.inputs.size(); ++k) {
    CHECK(in.outputs[k].name == (k == 0 ? std::string(doc::kGroupInPort) : c.inputs[k].name));
    CHECK(inPortPoint(c, k).x == outPortPoint(in, k).x);
    CHECK(inPortPoint(c, k).y == outPortPoint(in, k).y);
    CHECK(c.inputs[k].y >= metrics::kCapsuleTitle);  // under the title strip, on the frame
    CHECK(c.inputs[k].y <= c.h);
  }
  for (size_t k = 0; k < c.outputs.size(); ++k) {
    CHECK(out.inputs[k].name == (k == 0 ? std::string(doc::kGroupOutPort) : c.outputs[k].name));
    CHECK(outPortPoint(c, k).x == inPortPoint(out, k).x);
    CHECK(outPortPoint(c, k).y == inPortPoint(out, k).y);
  }
  CHECK(std::fabs(c.inputs[1].y - c.inputs[0].y) >= 8.0f);  // two sockets, apart
  CHECK(std::fabs(c.outputs[1].y - c.outputs[0].y) >= 8.0f);
  // Each crossing wire: two cables through its socket (one wire: hop 0 then 1).
  auto stretches = [&](uint32_t a, uint32_t b) {
    std::vector<const Edge*> v;
    for (size_t i = 0; i + 1 < pp.edges.size(); ++i)
      if (pp.edges[i].from == a && pp.edges[i + 1].to == b && pp.edges[i].hops == 2) v = {&pp.edges[i], &pp.edges[i + 1]};
    return v;
  };
  const std::vector<const Edge*> lfo = stretches(u.node(u.ids.lfo), u.node(u.ids.wt));
  REQUIRE(lfo.size() == 2);
  CHECK(lfo[0]->to == ci);
  CHECK(lfo[1]->from == c.groupIn);
  CHECK(lfo[0]->wire == lfo[1]->wire);
  CHECK((lfo[0]->cable && lfo[1]->cable));
  CHECK(lfo[0]->points.back() == lfo[1]->points.front());  // they meet at the socket
  const std::vector<const Edge*> toUtil = stretches(u.node(u.ids.pa2), u.node(u.ids.util));
  REQUIRE(toUtil.size() == 2);
  CHECK(toUtil[0]->to == c.groupOut);
  CHECK(toUtil[1]->from == ci);
  CHECK(toUtil[0]->points.back() == toUtil[1]->points.front());
  // In the scene: each socket once, drawn as a port (the In side a ring, the Out side a disc), the cables plugged into it.
  const circuit::CircuitScene& sc = *u.view().canvas().scene();
  for (size_t k = 0; k < c.inputs.size(); ++k) {
    const uint32_t p = u.view().canvas().scenePin(c.groupIn, static_cast<uint32_t>(k), false);
    REQUIRE(p != kNone);
    CHECK(sc.pins[p].socket == circuit::ScenePin::kSocketIn);
    CHECK(sc.pins[p].visible);
  }
  for (size_t k = 0; k < c.outputs.size(); ++k) {
    const uint32_t p = u.view().canvas().scenePin(c.groupOut, static_cast<uint32_t>(k), true);
    REQUIRE(p != kNone);
    CHECK(sc.pins[p].socket == circuit::ScenePin::kSocketOut);
  }
  CHECK(sc.nodes[u.view().scene().sceneNode[c.groupIn]].hidden);  // no body (no pentagon)
  const std::string lfoKey = c.inputs[1].name;  // (the model is built again by each edit below)
  // A drop onto the LFO's socket from another block's output: a wire into the param inside (the Bass Saturator's output).
  u.view().fitAll(false);
  u.d.tree().layoutNow();
  {
    const size_t h0 = u.history();
    const Vec2 from = u.outPin(u.node(u.ids.sat));
    const Vec2 to = u.inPin(ci, 1);
    u.d.drag(from, to, 12);
    u.d.step();
    CHECK(u.history() == h0 + 1);
    const doc::Wire& w = doc::wiresOf(*u.ctx.doc()).back();
    CHECK(w.from == u.ids.sat);
    CHECK(w.to == u.ids.wt);
    CHECK(lfoKey == w.param + "@" + std::to_string(u.ids.wt.value));
    CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
    REQUIRE(u.ctx.undo());
    u.d.step();
  }
  // A press on the Utility socket's outside (the capsule's output) draws from the member inside: onto the Bass
  // Saturator's input, a wire from that member.
  {
    u.view().fitAll(false);
    u.d.tree().layoutNow();
    const size_t h0 = u.history();
    const uint32_t cap = u.node(g);
    const Vec2 from = u.outPin(cap, 1);
    const Vec2 to = u.inPin(u.node(u.ids.sat), static_cast<size_t>(std::max(0, u.pp().nodes[u.node(u.ids.sat)].mainIn)));
    u.d.drag(from, to, 12);
    u.d.step();
    CHECK(u.history() == h0 + 1);
    const doc::Wire& w = doc::wiresOf(*u.ctx.doc()).back();
    CHECK(w.from == u.ids.pa2);
    CHECK(w.to == u.ids.sat);
    REQUIRE(u.ctx.undo());
    u.d.step();
  }
  // The main sockets: from In (its inside) onto Out (its inside) — the dry path, as the terminals did.
  {
    u.view().focusBand(u.ids.keys, false);
    u.d.tree().layoutNow();
    const size_t h0 = u.history();
    const Node& c2 = u.pp().nodes[u.node(g)];
    u.d.drag(u.outPin(c2.groupIn), u.inPin(c2.groupOut), 8);
    u.d.step();
    CHECK(u.history() == h0 + 1);
    const doc::Wire& w = doc::wiresOf(*u.ctx.doc()).back();
    CHECK(w.from == g);
    CHECK(w.port == doc::kGroupInPort);
    CHECK(w.to == g);
    CHECK(w.param == doc::kGroupOutPort);
  }
}

TEST_CASE("groups view (TX.group-resize): an opened group's scale is saved and loaded with the project") {
  Ui u;
  u.show();
  const doc::DeviceId sel[3] = {u.ids.pa1, u.ids.wt, u.ids.pa2};
  REQUIRE(u.view().groupDevices(sel));
  u.d.step();
  const doc::DeviceId g = u.groupOf(u.ids.wt);
  REQUIRE(g.valid());
  REQUIRE(u.pp().nodes[u.node(g)].groupOpen);
  const float w0 = u.pp().nodes[u.node(u.ids.wt)].w;
  REQUIRE(u.view().setBlockScale(g, 0.6f));
  CHECK(u.pp().nodes[u.node(u.ids.wt)].w == doctest::Approx(w0 * 0.6f));
  CHECK(u.pp().nodes[u.node(u.ids.wt)].scale == doctest::Approx(0.6f));
  const std::string dir = pq::views::test::tempDir("group_resize_save");
  REQUIRE(u.ctx.saveProjectAs(dir).ok());
  {
    FakeTransport t2;
    ViewContext other(t2);
    const auto opened = other.openProject(dir);
    REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
    REQUIRE(other.store().view().patchNudges.count(g.value) == 1);
    CHECK(other.store().view().patchNudges.at(g.value).scale == doctest::Approx(0.6f));
  }
  REQUIRE(u.view().setBlockScale(g, 1.0f));
  CHECK(u.pp().nodes[u.node(u.ids.wt)].w == doctest::Approx(w0));
  REQUIRE(u.ctx.openProject(dir).ok());  // (no sync here: the view lays it out with the loaded view state itself)
  CHECK(u.view().blockScale(g) == doctest::Approx(0.6f));
  CHECK(u.pp().nodes[u.node(g)].groupOpen);
  CHECK(u.pp().nodes[u.node(u.ids.wt)].scale == doctest::Approx(0.6f));  // (a project opened while the view is shown:
  CHECK(u.pp().nodes[u.node(u.ids.wt)].w == doctest::Approx(w0 * 0.6f));     // laid out with its loaded view state)
}

TEST_CASE("groups view: the Device view shows the group as one device; its panel is the embedded circuit, edited in place") {
  Ui u;
  const doc::DeviceId sel[3] = {u.ids.pa1, u.ids.wt, u.ids.pa2};
  REQUIRE(u.view().groupDevices(sel));
  const doc::DeviceId g = u.groupOf(u.ids.wt);
  u.ctx.setSelectedTrack(u.ids.keys);
  u.ctx.setDetailVisible(true);
  u.ctx.setDetailMode(DetailMode::Device);
  u.d.tree().layoutNow();
  u.d.step();
  DeviceChainView* chain = findDeviceChain(*u.shell);
  REQUIRE(chain != nullptr);
  // Frames: the synth, the group, the free LFO — not the members.
  std::vector<ProjectPatchView*> embedded;
  scopedViews(*chain, embedded);
  REQUIRE(embedded.size() == 1);
  ProjectPatchView& pv = *embedded.front();
  CHECK(pv.scope() == g);
  REQUIRE(pv.shown());
  const ProjectPatch& pp = pv.patch();
  CHECK(pp.scoped);
  REQUIRE(pp.bands.size() == 1);
  REQUIRE_FALSE(pp.nodes.empty());
  CHECK(pp.nodes[0].group);
  CHECK(pp.nodes.size() == 6);  // the capsule, In, three members, Out
  size_t wires = 0;
  for (const Edge& e : pp.edges) wires += e.kind != EdgeKind::Route && e.kind != EdgeKind::Send ? 1 : 0;
  // In → … → Out, its series (GU.11: the notes into it, chain edges), and (BR.patch-io) the LFO's wire from outside as
  // its stretch inside: from its socket on the frame onto the Wavetable.
  CHECK(wires == 5);
  CHECK(pv.canvas().rect().w > 100);
  // An edit inside the panel: a cable dragged on the embedded canvas; one undo step; the main view's document too.
  const size_t h0 = u.history();
  const Node& c = pp.nodes[0];
  auto devOf = [&](float x, float y) { return pv.canvas().toDevice({x, y}); };
  const Node& in = pp.nodes[c.groupIn];
  const Node& out = pp.nodes[c.groupOut];
  (void)in;
  (void)out;
  (void)devOf;
  // (TX.orbit-ports: ports are drawn where they orbit. GU.11: the notes reach this group's In — an instrument group —
  // so the edit is the Wavetable's output into Out, a second route into the sum, not In → Out.)
  const uint32_t wtn = pp.nodeOf(u.ids.wt);
  REQUIRE(wtn != kNone);
  const Vec2 a = pv.canvas().toDevice(pv.canvas().portWorld(wtn, 0, false));
  const Vec2 b = pv.canvas().toDevice(pv.canvas().portWorld(c.groupOut, 0, true));
  u.d.drag(a, b, 8);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(doc::wiresOf(*u.ctx.doc()).back().to == g);
  CHECK(doc::wiresOf(*u.ctx.doc()).back().from == u.ids.wt);
  CHECK(pv.patch().edges.size() == pp.edges.size());  // rebuilt (the reference above is the new model)
  REQUIRE(u.ctx.undo());
  // Ungrouped elsewhere: the panel goes, the members are frames again.
  {
    auto e = doc::ops::ungroupInCircuit(u.ctx.doc(), u.ctx.store().ids(), library(), g);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
  }
  u.d.tree().layoutNow();
  u.d.step();
  embedded.clear();
  scopedViews(*chain, embedded);
  CHECK(embedded.empty());
}

// TMM.13-fix2 (MM review H1, probe E of docs/reviews/MM/views/probe_views.cpp): Delete / Ctrl+Shift+G pressed inside a
// group's own embedded panel removes that panel's frame from the store listener while the canvas's key handler is still
// on the stack. Freed memory is poisoned (PoisonFrees) while the key is handled, so a read of the destroyed view faults.
// M5: keys that did nothing in the embedded canvas are not reported handled (the Device view gets them).
TEST_CASE("groups view: Delete and Ctrl+Shift+G inside a group's own panel (poisoned frees); unhandled keys reach the Device view") {
  Ui u;
  const doc::DeviceId sel[3] = {u.ids.pa1, u.ids.wt, u.ids.pa2};
  REQUIRE(u.view().groupDevices(sel));
  const doc::DeviceId g = u.groupOf(u.ids.wt);
  u.ctx.setSelectedTrack(u.ids.keys);
  u.ctx.setDetailVisible(true);
  u.ctx.setDetailMode(DetailMode::Device);
  u.d.tree().layoutNow();
  u.d.advance(0.3);
  DeviceChainView* chain = findDeviceChain(*u.shell);
  REQUIRE(chain != nullptr);
  auto embedded = [&]() -> ProjectPatchView* {
    std::vector<ProjectPatchView*> e;
    scopedViews(*chain, e);
    return e.empty() ? nullptr : e.front();
  };
  ProjectPatchView* pv = embedded();
  REQUIRE(pv != nullptr);
  // M5: nothing selected inside, Ctrl+G: not the embedded canvas's ("groups do not nest" was its answer), no edit.
  u.d.tree().setFocus(&pv->canvas());
  pv->canvas().clearSelection();
  size_t h0 = u.history();
  u.ctx.setStatus("");
  (void)u.d.press("Ctrl+G");
  CHECK(u.history() == h0);
  CHECK(u.ctx.status().find("do not nest") == std::string::npos);
  CHECK(u.d.press("Delete") != KeyResult::Widget);  // nothing selected inside: unhandled there
  CHECK(u.history() == h0);
  // H1 a: the capsule (the group itself) selected by its frame inside its own panel, Delete.
  const Node& cap = pv->patch().nodes[0];
  // (BR.patch-io: mid-way along its bottom edge — its left and right edges carry its sockets)
  u.d.click(pv->canvas().toDevice({cap.x + cap.w * 0.5f, cap.y + cap.h - 4.0f}));
  u.d.tree().setFocus(&pv->canvas());
  REQUIRE(std::find(pv->selectedDevices().begin(), pv->selectedDevices().end(), g) != pv->selectedDevices().end());
  h0 = u.history();
  {
    alloc_trap::PoisonFrees poison;
    CHECK(u.d.keyDown(key::kDelete) == KeyResult::Widget);
  }
  u.d.keyUp(key::kDelete);
  CHECK(u.history() == h0 + 1);
  CHECK(doc::findDevice(*u.ctx.doc(), g).device == nullptr);
  u.d.advance(0.3);  // the retired frame goes at the next frame
  CHECK(embedded() == nullptr);
  // H1 b: undo; a member selected inside the panel, Ctrl+Shift+G ungroups the group the panel shows.
  REQUIRE(u.ctx.undo());
  u.d.tree().layoutNow();
  u.d.advance(0.3);
  pv = embedded();
  REQUIRE(pv != nullptr);
  const uint32_t n = pv->patch().nodeOf(u.ids.wt);
  REQUIRE(n != kNone);
  const uint32_t sn[1] = {pv->scene().sceneNode[n]};
  pv->canvas().select(sn, {});
  u.d.tree().setFocus(&pv->canvas());
  h0 = u.history();
  {
    alloc_trap::PoisonFrees poison;
    CHECK(u.d.keyDown('g', static_cast<Mods>(mod::kCtrl | mod::kShift)) == KeyResult::Widget);
  }
  u.d.keyUp('g', static_cast<Mods>(mod::kCtrl | mod::kShift));
  CHECK(u.history() == h0 + 1);
  CHECK_FALSE(u.groupOf(u.ids.wt).valid());
  u.d.advance(0.3);
  CHECK(embedded() == nullptr);
}

// TX.drag-grow (the user 2026-11-08: "dragging a module should actively expand the space its in while being dragged …
// so you can see exactly how it shifts the space"). Through the window: a block dragged past its band's bottom (the
// pointer still in the band) grows the band every frame, the band under it shifting down with its blocks, no re-layout;
// what was shown is what the drop lays out; Esc puts it back. A capsule's member dragged past the capsule's corner grows
// the capsule (its Out terminal at its new right edge) and the band holding it, the same way.
TEST_CASE("groups view (TX.drag-grow): a dragged block grows its band and its capsule live; the drop lands on what was shown; Esc restores") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  ProjectCanvas& cv = v.canvas();
  cv.setCablesAnimated(false);
  const size_t h0 = u.history();
  auto rectOf = [](const Band& b) { return ProjectCanvas::WorldRect{b.x0, b.y, b.x1, b.y + b.h}; };
  auto sceneOf = [&](doc::DeviceId d) { return v.scene().sceneNode[u.node(d)]; };
  // ---- a block below its band's bottom: the free LFO of Keys, dragged down (the pointer stays in Keys) ----
  {
    v.fitAll(false);  // both bands on the canvas
    u.d.tree().layoutNow();
    u.d.step();
    const uint32_t kb = u.pp().bandOf(u.ids.keys), bb = u.pp().bandOf(u.ids.bass);
    REQUIRE(kb != kNone);
    REQUIRE(bb != kNone);
    const Band keys0 = u.pp().bands[kb], bass0 = u.pp().bands[bb];
    REQUIRE(bass0.y >= keys0.y + keys0.h);  // Bass is under Keys
    const Node lfo0 = u.pp().nodes[u.node(u.ids.lfo)];
    const Node sat0 = u.pp().nodes[u.node(u.ids.sat)];
    const Vec2 grip = u.dev(lfo0.x + lfo0.w * 0.5f, lfo0.y + 10.0f);  // its panel's title bar
    // To 6 units above the band's bottom: the block hangs below it by most of its height.
    const float dy = keys0.y + keys0.h - 6.0f - (lfo0.y + 10.0f);
    REQUIRE(dy > 0.0f);
    const float s = cv.deviceScale();
    const Vec2 to{grip.x, grip.y + dy * s};
    REQUIRE(cv.rect().contains(to));
    auto dragDown = [&] {
      u.d.move(grip);
      u.d.down(grip);
      for (int i = 1; i <= 6; ++i) {
        u.d.move({grip.x, grip.y + (to.y - grip.y) * static_cast<float>(i) / 6.0f});
        u.d.step();
      }
      REQUIRE(cv.drag() == ProjectCanvas::Drag::MoveDevice);
    };
    const uint64_t relayouts0 = v.stats().relayouts;
    dragDown();
    // Grown before the release: Keys down to the block + its pad, Bass shifted by as much, its blocks drawn with it.
    CHECK(cv.growing());
    const ProjectCanvas::WorldRect kLive = cv.bandNow(kb), bLive = cv.bandNow(bb);
    const float grow = kLive.y1 - (keys0.y + keys0.h);
    CHECK(grow > lfo0.h * 0.5f);
    CHECK(kLive.y1 >= lfo0.y + std::round(dy) + lfo0.h + metrics::kBandPad);
    CHECK(kLive.y0 == keys0.y);
    CHECK(bLive.y0 == doctest::Approx(bass0.y + grow));
    CHECK(bLive.y1 == doctest::Approx(bass0.y + bass0.h + grow));
    const uint32_t satScene = sceneOf(u.ids.sat);
    const std::span<const uint32_t> shifted = cv.shiftedNodes();
    CHECK(std::find(shifted.begin(), shifted.end(), satScene) != shifted.end());
    CHECK(cv.nodeOffset(satScene).y == doctest::Approx(grow));
    CHECK(cv.nodeOffset(satScene).x == 0.0f);
    CHECK(cv.nodeLayer(satScene) >= circuit::CircuitCanvas::kShiftLayer0);
    // Hit where it is drawn now (shifted), not where it was.
    {
      const Vec2 at = u.dev(sat0.x + sat0.w * 0.5f, sat0.y + 10.0f + grow);
      const circuit::CanvasHit h = cv.hitTest(at);
      REQUIRE(cv.rect().contains(at));
      CHECK(h.kind == circuit::CanvasHit::Kind::Node);
      CHECK(h.index == satScene);
    }
    CHECK(v.stats().relayouts == relayouts0);  // no re-layout while it moves
    CHECK(u.pp().bands[kb].h == keys0.h);
    // Esc: everything back as the model has it, nothing changed.
    u.d.keyDown(key::kEscape);
    u.d.keyUp(key::kEscape);
    CHECK_FALSE(cv.growing());
    CHECK(cv.shiftedNodes().empty());
    CHECK(cv.nodeOffset(satScene).y == 0.0f);
    CHECK(cv.bandNow(kb).y1 == keys0.y + keys0.h);
    CHECK(cv.bandNow(bb).y0 == bass0.y);
    u.d.up(to);
    u.d.step();
    CHECK(u.history() == h0);
    CHECK(u.pp().bands[kb].h == keys0.h);
    CHECK(u.pp().nodes[u.node(u.ids.lfo)].y == lfo0.y);
    // Again, released: the drop lays out exactly what was shown (Keys' strip, Bass's strip and its blocks).
    dragDown();
    const ProjectCanvas::WorldRect kShown = cv.bandNow(kb), bShown = cv.bandNow(bb);
    u.d.up(to);
    CHECK_FALSE(cv.growing());
    CHECK_FALSE(v.pushing());  // (dropped clear of every block: the layout alone)
    const Band& k1 = u.pp().bands[kb];
    const Band& b1 = u.pp().bands[bb];
    CHECK(k1.h > keys0.h);
    CHECK(rectOf(k1).y0 == kShown.y0);
    CHECK(rectOf(k1).y1 == kShown.y1);
    CHECK(rectOf(k1).x1 == kShown.x1);
    CHECK(rectOf(b1).y0 == bShown.y0);
    CHECK(rectOf(b1).y1 == bShown.y1);
    CHECK(u.pp().nodes[u.node(u.ids.sat)].y == doctest::Approx(sat0.y + grow));
    CHECK(u.history() == h0);  // a nudge: view state
  }
  // ---- a capsule's member past the capsule's bottom-right corner (the pointer stays inside it) ----
  {
    const doc::DeviceId sel[3] = {u.ids.pa1, u.ids.wt, u.ids.pa2};
    REQUIRE(v.groupDevices(sel));
    u.d.step();
    const doc::DeviceId g = u.groupOf(u.ids.wt);
    REQUIRE(g.valid());
    v.nudge(u.ids.lfo.value, 0.0f, 600.0f);  // the LFO out of the way (the grown capsule overlaps nothing)
    v.focusBand(u.ids.keys, false);
    u.d.tree().layoutNow();
    u.d.advance(1.0);
    const size_t h1 = u.history();
    const uint32_t cap = u.node(g);
    const Node c0 = u.pp().nodes[cap];
    REQUIRE(c0.groupOpen);
    const Node out0 = u.pp().nodes[c0.groupOut];
    const Node in0 = u.pp().nodes[c0.groupIn];
    const uint32_t kb = c0.band;
    const Band keys0 = u.pp().bands[kb];
    const Node p20 = u.pp().nodes[u.node(u.ids.pa2)];
    const Vec2 from = u.dev(p20.x + 6.0f, p20.y + 6.0f);
    const Vec2 to = u.dev(c0.x + c0.w - 4.0f, c0.y + c0.h - 4.0f);  // inside the capsule as laid out
    REQUIRE(cv.rect().contains(to));
    auto dragOut = [&] {
      u.d.move(from);
      u.d.down(from);
      for (int i = 1; i <= 6; ++i) {
        const float t = static_cast<float>(i) / 6.0f;
        u.d.move({from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t});
        u.d.step();
      }
      REQUIRE(cv.drag() == ProjectCanvas::Drag::MoveDevice);
    };
    dragOut();
    CHECK(cv.growing());
    const ProjectCanvas::WorldRect f = cv.frameNow(cap);
    CHECK(f.x1 > c0.x + c0.w + 20.0f);
    CHECK(f.y1 > c0.y + c0.h + 20.0f);
    CHECK(f.x0 == c0.x);
    CHECK(f.y0 == c0.y);
    // Its Out terminal at its new right edge (shifted), its In where it was; the band grown to hold it.
    const uint32_t outScene = v.scene().sceneNode[c0.groupOut], inScene = v.scene().sceneNode[c0.groupIn];
    CHECK(cv.nodeOffset(outScene).x == doctest::Approx(f.x1 - (c0.x + c0.w)));
    CHECK(cv.nodeOffset(inScene).x == 0.0f);
    CHECK(cv.bandNow(kb).y1 >= f.y1 + metrics::kBandPad);
    // Esc: as it was.
    u.d.keyDown(key::kEscape);
    u.d.keyUp(key::kEscape);
    CHECK_FALSE(cv.growing());
    CHECK(cv.frameNow(cap).x1 == c0.x + c0.w);
    CHECK(cv.nodeOffset(outScene).x == 0.0f);
    u.d.up(to);
    u.d.step();
    CHECK(u.history() == h1);
    CHECK(u.pp().nodes[cap].w == c0.w);
    // Released: the capsule, its terminals and the band as shown.
    dragOut();
    const ProjectCanvas::WorldRect fShown = cv.frameNow(cap), kShown = cv.bandNow(kb);
    const float outShift = cv.nodeOffset(outScene).x;
    const Node mixer0 = u.pp().nodes[keys0.mixer];
    const float mixerShift = cv.nodeOffset(v.scene().sceneNode[keys0.mixer]).x;
    u.d.up(to);
    CHECK_FALSE(cv.growing());
    CHECK_FALSE(v.pushing());
    CHECK(u.groupOf(u.ids.pa2) == g);  // still a member (moved inside its capsule)
    const Node& c1 = u.pp().nodes[u.node(g)];
    CHECK(c1.x == fShown.x0);
    CHECK(c1.y == fShown.y0);
    CHECK(c1.x + c1.w == fShown.x1);
    CHECK(c1.y + c1.h == fShown.y1);
    CHECK(u.pp().nodes[c1.groupOut].x == doctest::Approx(out0.x + outShift));
    CHECK(u.pp().nodes[c1.groupIn].x == in0.x);
    const Band& k1 = u.pp().bands[kb];
    CHECK(k1.y + k1.h == kShown.y1);
    CHECK(k1.x1 == kShown.x1);
    CHECK(u.pp().nodes[k1.mixer].x == doctest::Approx(mixer0.x + mixerShift));  // it followed the capsule right
    CHECK(u.history() == h1);
  }
}

TEST_CASE("groups view: a group panel's frame — an idle frame costs nothing, a full repaint of the embedded circuit" * doctest::test_suite("perf")) {
  Ui u;
  const doc::DeviceId sel[3] = {u.ids.pa1, u.ids.wt, u.ids.pa2};
  REQUIRE(u.view().groupDevices(sel));
  u.ctx.setSelectedTrack(u.ids.keys);
  u.ctx.setDetailVisible(true);
  u.ctx.setDetailMode(DetailMode::Device);
  REQUIRE(u.d.enableGpu().ok());
  u.d.tree().layoutNow();
  for (int i = 0; i < 3; ++i) REQUIRE(u.d.render().ok());
  std::vector<ProjectPatchView*> embedded;
  scopedViews(*findDeviceChain(*u.shell), embedded);
  REQUIRE(embedded.size() == 1);
  ProjectPatchView& pv = *embedded.front();
  std::vector<double> ms;
  for (int i = 0; i < 20; ++i) {
    pv.canvas().markDirty();
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(u.d.render().ok());
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(ms.begin(), ms.end());
  MESSAGE("a frame repainting the group panel (whole window rendered and read back): median " << ms[ms.size() / 2] << " ms");
  CHECK(ms[ms.size() / 2] < 16.0);
  // The scoped model rebuild (lowering + model + scene) after a structural edit.
  MESSAGE("group panel rebuild: " << pv.stats().lastRebuildMs << " ms");
}

// ---- goldens --------------------------------------------------------------------------------------------------------

namespace {
void golden(const char* name) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(name, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(name, {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(name) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(name, *img, *ref, true, &d));
  MESSAGE(std::string(name) << ": " << pq::ui::test::describe(d));
}
}  // namespace

// The user's example grouped (basic synth → group: In → PA → WT → PA → Out, the free LFO outside wired in) and Bass's
// Saturator → Utility grouped and closed (its miniature).
TEST_CASE("render: project-patch-group matches the golden image") { golden("project-patch-group"); }
// The Device view of Keys: the synth, then the group whose panel is the circuit of its contents.
TEST_CASE("render: device-group-panel matches the golden image") { golden("device-group-panel"); }
