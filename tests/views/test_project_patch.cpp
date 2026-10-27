// T4.13 the project patch view: the model is the lowered ProjectGraph (checked node for node, wire for wire, groups and
// sends), the layout mirrors the arrangement, nudges are saved view state, the morph keeps every lane's vertical place,
// every edit kind made in the patch view is one undo step and shows in the Ableton view, semantic zoom, idle costs
// nothing. Goldens: project-patch-overview, project-patch-band. Wall-clock checks (64 tracks: full paint, morph frame)
// are in the perf suite.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/serialize.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
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

doc::LoweredProject lowerOf(const doc::Project& p) {
  doc::LowerOptions o;
  o.library = &library().library();
  auto r = doc::lower(p, o);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return std::move(*r);
}

const doc::Track* named(const doc::Project& p, std::string_view name) {
  const doc::Track* found = nullptr;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (!found && t.name == name) found = &t;
  });
  REQUIRE_MESSAGE(found != nullptr, "no track " << std::string(name));
  return found;
}

float pinY(const Node& n, size_t i) {
  if (n.kind == NodeKind::Sum) return 16.0f;
  if (n.kind == NodeKind::Output) return 8.0f;
  return metrics::kPinTop + metrics::kPinPitch * static_cast<float>(i);
}

bool orthogonal(const std::vector<graph::Point2>& pts) {
  for (size_t i = 0; i + 1 < pts.size(); ++i)
    if (pts[i].x != pts[i + 1].x && pts[i].y != pts[i + 1].y) return false;
  return pts.size() >= 2;
}

// The shell over the sketch project (or a given builder), the patch view shown without the morph.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;

  explicit Ui(bool sketch = true) {
    ctx.setDeviceLibrary(&library());
    if (sketch) REQUIRE(buildSketchProject(ctx.store()).ok());
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
  ArrangementView& arrangement() const {
    auto* a = dynamic_cast<ArrangementView*>(shell->slot(ShellSlot::TrackArea).find("arrangement"));
    REQUIRE(a != nullptr);
    return *a;
  }
  void show() {
    view().setShown(true, false);
    d.tree().layoutNow();
  }
  size_t history() const { return ctx.store().history().cursor(); }
  circuit::CircuitEditor* editor() const {
    for (size_t i = 0; i < shell->overlay().childCount(); ++i)
      if (auto* e = dynamic_cast<circuit::CircuitEditor*>(&shell->overlay().child(i))) return e;
    return nullptr;
  }
  const doc::Track& track(std::string_view name) const { return *named(*ctx.doc(), name); }
  const ProjectPatch& pp() const { return view().patch(); }
  uint32_t deviceNode(doc::DeviceId id) const {
    const uint32_t n = pp().nodeOf(id);
    REQUIRE(n != kNone);
    return n;
  }
  Vec2 dev(float x, float y) const { return view().canvas().toDevice({x, y}); }
  Vec2 centre(uint32_t node) const {
    const Node& n = pp().nodes[node];
    return dev(n.x + n.w * 0.5f, n.y + n.h * 0.5f);
  }
  Vec2 outPin(uint32_t node, std::string_view name) const {
    const Node& n = pp().nodes[node];
    const int32_t k = n.output(name);
    REQUIRE(k >= 0);
    return dev(n.x + n.w, n.y + pinY(n, static_cast<size_t>(k)));
  }
  Vec2 sumOf(std::string_view track) const {
    const uint32_t b = pp().bandOf(named(*ctx.doc(), track)->id);
    REQUIRE(b != kNone);
    return centre(pp().bands[b].source);
  }
  Menu* openMenu() const {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<Menu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  void choose(Menu& m, std::string_view label) {
    for (int32_t i = 0; i < static_cast<int32_t>(m.items().size()); ++i)
      if (m.items()[static_cast<size_t>(i)].label == label) {
        m.choose(i);
        d.step();
        return;
      }
    FAIL("no menu item " << std::string(label));
  }
  static Vec2 mid(const IRect& r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
  // The Ableton view (the mixer view: Ctrl+Alt+M hides the patch view) and one track's strip in it.
  MixerStrip& mixerStrip(std::string_view name) {
    MixerView* m = findMixer(*shell);
    REQUIRE(m != nullptr);
    if (!m->shown()) REQUIRE(d.press("Ctrl+Alt+M") == KeyResult::Action);
    d.tree().layoutNow();
    d.step();
    CHECK_FALSE(view().shown());
    MixerStrip* s = m->strip(named(*ctx.doc(), name)->id);
    REQUIRE(s != nullptr);
    return *s;
  }
  // The Device view of a track (the arrangement with its detail area) and the cables T4.7 draws there.
  const CableLayer::Cable* deviceViewCable(doc::TrackId t, doc::WireId w) {
    view().setShown(false, false);
    ctx.setSelectedTrack(t);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
    d.tree().layoutNow();
    d.step();
    CableLayer* l = findCableLayer(*shell);
    REQUIRE(l != nullptr);
    for (const CableLayer::Cable& c : l->cables())
      if (c.wire == w) return &c;
    return nullptr;
  }
  // Zoomed so blocks and pins are big enough to hit reliably.
  void zoomTo(std::string_view track) {
    view().focusBand(named(*ctx.doc(), track)->id, false);
    d.tree().layoutNow();
  }
};

}  // namespace

TEST_CASE("project patch: the sketch project's view is its lowered ProjectGraph, node for node and wire for wire") {
  doc::DocumentStore store;
  REQUIRE(buildSketchProject(store).ok());
  const doc::Project& p = *store.current();
  const doc::LoweredProject low = lowerOf(p);
  const ProjectPatch pp = buildProjectPatch(p, low, &library().catalog());
  CHECK(checkAgainstGraph(pp, p, low) == "");
  // One band per track in lane order: the group, its members, the LFO track, the return, the master.
  std::vector<std::string> names;
  for (const Band& b : pp.bands) names.push_back(b.name);
  CHECK(names == std::vector<std::string>{"Group", "Keys", "Pad", "Mod", "Space", "Master"});
  // Every graph wire is drawn once: routes into the Σs, sends into the return (two of them at −∞, dim), the LFO's two
  // cross-track modulation wires.
  size_t routes = 0, sends = 0, silent = 0, mods = 0;
  for (const Edge& e : pp.edges) {
    routes += e.kind == EdgeKind::Route;
    sends += e.kind == EdgeKind::Send;
    silent += e.kind == EdgeKind::Send && e.silent;
    mods += e.kind == EdgeKind::Modulation;
    CHECK(orthogonal(e.points));
  }
  size_t graphRoutes = 0, graphSends = 0, graphMods = 0;
  for (const graph::ProjectWire& w : low.graph.wires) {
    graphRoutes += w.kind == graph::WireKind::Audio && w.from.port == "out";
    graphSends += w.kind == graph::WireKind::Audio && w.from.port != "out";
    graphMods += w.kind == graph::WireKind::Modulation;
  }
  CHECK(routes == graphRoutes);
  CHECK(sends == graphSends);
  CHECK(sends == 4);   // Group, Keys, Pad, Mod → Space
  CHECK(silent == 2);  // Group and Mod send at −∞
  CHECK(mods == 2);
  CHECK(graphMods == 2);
  // A broken patch is caught: a graph wire left out, a device block that is not its call.
  ProjectPatch broken = pp;
  broken.edges.erase(std::find_if(broken.edges.begin(), broken.edges.end(), [](const Edge& e) { return e.kind == EdgeKind::Send; }));
  CHECK(checkAgainstGraph(broken, p, low) != "");
  ProjectPatch broken2 = pp;
  for (Node& n : broken2.nodes)
    if (n.kind == NodeKind::Device && n.lowered) {
      n.call = graph::NodeId{n.call.value + 1};
      break;
    }
  CHECK(checkAgainstGraph(broken2, p, low) != "");
}

TEST_CASE("project patch: deactivated tracks and devices, missing devices, nested groups, return sends and loops match the graph") {
  doc::DocumentStore store;
  REQUIRE(buildSketchProject(store).ok());
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  };
  const doc::Project& p0 = *store.current();
  const doc::TrackId keys = named(p0, "Keys")->id, pad = named(p0, "Pad")->id, grp = named(p0, "Group")->id;
  const doc::TrackId space = named(p0, "Space")->id;
  // A nested group, a deactivated track, a deactivated device, a device whose patch is not in the library.
  const doc::TrackId inner = run(doc::ops::groupTracks(*store.current(), store.ids(), std::vector<doc::TrackId>{pad}, "Inner")).tracks[0];
  const doc::TrackId off = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Off")).tracks[0];
  run(doc::ops::setTracksActive(*store.current(), std::vector<doc::TrackId>{off}, false));
  const doc::DeviceId util = run(doc::ops::addDevice(*store.current(), store.ids(), keys, "utility", doc::DeviceKind::AudioEffect)).devices[0];
  run(doc::ops::setDevicesActive(*store.current(), std::vector<doc::DeviceId>{util}, false));
  REQUIRE(store.dispatch(doc::AddDevice{keys, doc::makeDevice(store.ids().device(), "nosuch", doc::DeviceKind::AudioEffect)}).ok());
  // A second return; return → return sends both ways (stored: the second closes a loop and is delayed); a pre send.
  const doc::TrackId echo = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Return, space, "Echo")).tracks[0];
  REQUIRE(store.dispatch(doc::SetSendLevel{space, echo, -3.0f}).ok());
  REQUIRE(store.dispatch(doc::SetSendLevel{echo, space, -9.0f}).ok());
  run(doc::ops::setReturnSendsPreFader(*store.current(), echo, true));
  // A wire into a deactivated device: inactive (drawn dim, not in the graph).
  const doc::DeviceId lfo = named(*store.current(), "Mod")->devices[0]->id;
  run(doc::ops::connectWire(*store.current(), store.ids(), lfo, "lfo", util, "width", 0.3));
  // The group routed to sends only.
  REQUIRE(store.dispatch(doc::SetTrackOutput{grp, doc::Output{doc::OutputKind::None, {}}}).ok());

  const doc::Project& p = *store.current();
  const doc::LoweredProject low = lowerOf(p);
  REQUIRE_FALSE(low.delayedSends.empty());
  const ProjectPatch pp = buildProjectPatch(p, low, &library().catalog());
  CHECK(checkAgainstGraph(pp, p, low) == "");
  const uint32_t ob = pp.bandOf(off);
  REQUIRE(ob != kNone);
  CHECK_FALSE(pp.bands[ob].active);
  CHECK(pp.bands[ob].graphTrack == kNone);
  const uint32_t ib = pp.bandOf(inner);
  REQUIRE(ib != kNone);
  CHECK(pp.bands[ib].parent == static_cast<int32_t>(pp.bandOf(grp)));
  CHECK(pp.bands[pp.bandOf(pad)].parent == static_cast<int32_t>(ib));
  CHECK(pp.bands[pp.bandOf(pad)].depth == 2);
  const uint32_t un = pp.nodeOf(util);
  REQUIRE(un != kNone);
  CHECK_FALSE(pp.nodes[un].lowered);
  CHECK_FALSE(pp.nodes[un].active);
  bool missing = false, delayed = false, inactive = false, pre = false;
  for (const Node& n : pp.nodes) missing = missing || (n.kind == NodeKind::Device && n.missing);
  for (const Edge& e : pp.edges) {
    delayed = delayed || (e.kind == EdgeKind::Send && e.delayed);
    inactive = inactive || (e.kind == EdgeKind::Modulation && e.status == doc::WireStatus::Inactive && e.graphWire == kNone);
    pre = pre || (e.kind == EdgeKind::Send && e.pre);
  }
  CHECK(missing);
  CHECK(delayed);
  CHECK(inactive);
  CHECK(pre);
  // No route edge out of the group (sends only).
  for (const Edge& e : pp.edges) CHECK_FALSE((e.kind == EdgeKind::Route && pp.nodes[e.from].band == pp.bandOf(grp)));
}

TEST_CASE("project patch: the layout mirrors the arrangement (lane order, groups as containers, left → right)") {
  doc::DocumentStore store;
  REQUIRE(buildWideProject(store, 20).ok());
  const doc::Project& p = *store.current();
  const doc::LoweredProject low = lowerOf(p);
  const ProjectPatch pp = buildProjectPatch(p, low, &library().catalog());
  CHECK(checkAgainstGraph(pp, p, low) == "");
  // Bands in lane order (forEachTrack: the main tree depth-first, the returns, the master), stacked downwards.
  std::vector<doc::TrackId> order;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) { order.push_back(t.id); });
  REQUIRE(order.size() == pp.bands.size());
  for (size_t i = 0; i < order.size(); ++i) {
    CHECK(pp.bands[i].track == order[i]);
    if (i > 0) CHECK(pp.bands[i].y >= pp.bands[i - 1].y + pp.bands[i - 1].h);
  }
  float mainEnd = 0.0f, retStart = 1e9f, retEnd = 0.0f;
  for (const Band& b : pp.bands) {
    // Within a band: left → right, no overlaps, every node inside the band's strip.
    std::vector<uint32_t> chain{b.source};
    chain.insert(chain.end(), b.devices.begin(), b.devices.end());
    chain.push_back(b.mixer);
    for (size_t k = 0; k < chain.size(); ++k) {
      const Node& n = pp.nodes[chain[k]];
      CHECK(n.y >= b.y);
      CHECK(n.y + n.h <= b.y + b.h);
      if (k > 0) CHECK(n.x >= pp.nodes[chain[k - 1]].x + pp.nodes[chain[k - 1]].w);
    }
    const Node& mix = pp.nodes[b.mixer];
    if (b.kind == doc::TrackKind::Midi || b.kind == doc::TrackKind::Group) mainEnd = std::max(mainEnd, mix.x + mix.w);
    if (b.kind == doc::TrackKind::Return) {
      retStart = std::min(retStart, pp.nodes[b.source].x);
      retEnd = std::max(retEnd, mix.x + mix.w);
    }
    // A group's own chain starts right of its members' mixers; its container holds them.
    if (b.kind == doc::TrackKind::Group && b.lastMember != kNone) {
      const uint32_t gi = static_cast<uint32_t>(&b - pp.bands.data());
      for (uint32_t c = gi + 1; c <= b.lastMember; ++c) {
        const Node& m = pp.nodes[pp.bands[c].mixer];
        CHECK(pp.nodes[b.source].x > m.x + m.w);
        CHECK(pp.bands[c].x0 >= b.cx0);
        CHECK(pp.bands[c].y + pp.bands[c].h <= b.cy1);
      }
    }
  }
  CHECK(retStart > mainEnd);  // the returns' Σs right of every main track (their sends arrive by buses)
  REQUIRE(pp.master != kNone);
  CHECK(pp.nodes[pp.bands[pp.master].source].x > retEnd);  // the master on the right
  for (const Edge& e : pp.edges) CHECK(orthogonal(e.points));
}

TEST_CASE("project patch: nudges are view state — not undoable, saved with the project, applied to the layout") {
  Ui u;
  u.show();
  const doc::Track& keys = u.track("Keys");
  const uint32_t b = u.pp().bandOf(keys.id);
  const uint32_t sat = u.deviceNode(keys.devices[1]->id);
  const float by = u.pp().bands[b].y, sx = u.pp().nodes[sat].x, mx = u.pp().nodes[u.pp().bands[b].mixer].x;
  const size_t h0 = u.history();
  u.view().nudge(keys.id.value, 0.0f, 20.0f);
  u.view().nudge(keys.devices[1]->id.value, 48.0f, 0.0f);
  CHECK(u.history() == h0);  // not an undo step
  CHECK(u.ctx.dirty());      // but saved
  CHECK(u.pp().bands[u.pp().bandOf(keys.id)].y == doctest::Approx(by + 20.0f));
  CHECK(u.pp().nodes[u.pp().nodeOf(keys.devices[1]->id)].x == doctest::Approx(sx + 48.0f));
  CHECK(u.pp().nodes[u.pp().bands[u.pp().bandOf(keys.id)].mixer].x == doctest::Approx(mx));
  // Saved and loaded with the project.
  const std::string dir = pq::views::test::tempDir("project_patch_nudges");
  REQUIRE(u.ctx.saveProjectAs(dir).ok());
  FakeTransport t2;
  ViewContext other(t2);
  REQUIRE(other.openProject(dir).ok());
  const auto& n = other.store().view().patchNudges;
  REQUIRE(n.size() == 2);
  CHECK(n.at(keys.id.value).dy == 20.0f);
  CHECK(n.at(keys.devices[1]->id.value).dx == 48.0f);
  // A nudge naming no entity is a load error (strict reader).
  auto text = doc::serializeProject(*u.ctx.doc(), u.ctx.store().view());
  REQUIRE(text.ok());
  std::string bad = *text;
  const size_t at = bad.find("\"patch\"");
  REQUIRE(at != std::string::npos);
  const size_t id = bad.find("\"id\": ", at);
  REQUIRE(id != std::string::npos);
  bad.replace(id, 6, "\"id\": 99999");  // the id now reads 99999<digits>: no such entity
  CHECK_FALSE(doc::parseProject(bad).ok());
  // Reset Layout clears them.
  u.view().resetLayout();
  CHECK(u.ctx.store().view().patchNudges.empty());
  CHECK(u.pp().bands[u.pp().bandOf(keys.id)].y == doctest::Approx(by));
}

TEST_CASE("project patch: Ctrl+Alt+P morphs from the arrangement (lanes keep their place) and back") {
  Ui u;
  ArrangementView& arr = u.arrangement();
  u.d.step();
  CHECK(arr.visible());
  CHECK_FALSE(u.view().shown());
  // The lanes the arrangement shows now.
  std::vector<std::pair<doc::TrackId, IRect>> lanes;
  for (size_t i = 0; i < arr.rows().size(); ++i) lanes.emplace_back(arr.rows()[i].id, arr.rowLaneRect(static_cast<int32_t>(i)));
  REQUIRE(lanes.size() >= 3);
  CHECK(u.d.press("Ctrl+Alt+P") == KeyResult::Action);
  ProjectPatchView& v = u.view();
  CHECK(v.shown());
  CHECK(v.morphing());
  CHECK_FALSE(arr.visible());
  // Progress 0: every band sits exactly on its lane.
  for (const auto& [id, lane] : lanes) {
    const Rect r = v.morphBandRect(v.patch().bandOf(id));
    CHECK(r.y0 == doctest::Approx(static_cast<float>(lane.y)));
    CHECK(r.y1 == doctest::Approx(static_cast<float>(lane.bottom())));
  }
  const uint64_t f0 = u.d.framesDrawn();
  u.d.advance(ProjectPatchView::kMorphSeconds * 0.5);
  CHECK(v.morphing());
  CHECK(v.morphProgress() > 0.1f);
  CHECK(v.morphProgress() < 0.9f);
  CHECK(u.d.framesDrawn() > f0 + 10);  // a frame per display frame (the driver runs at 144 Hz)
  u.d.advance(ProjectPatchView::kMorphSeconds);
  CHECK_FALSE(v.morphing());
  CHECK(v.morphProgress() == 1.0f);
  CHECK(v.canvas().visible());
  // At the end the first lane's band is where the lane was (the zoom makes bands about lane height).
  const Band& first = v.patch().bands[v.patch().bandOf(lanes[0].first)];
  CHECK(v.canvas().toDevice({first.x0, first.y}).y == doctest::Approx(static_cast<float>(lanes[0].second.y)).epsilon(0.01));
  // Idle once settled: no frames, no timers.
  u.d.advance(0.5);
  const uint64_t f1 = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.d.framesDrawn() == f1);
  // And back.
  CHECK(u.d.press("Ctrl+Alt+P") == KeyResult::Action);
  CHECK(v.morphing());
  u.d.advance(ProjectPatchView::kMorphSeconds * 2.0);
  CHECK_FALSE(v.morphing());
  CHECK_FALSE(v.shown());
  CHECK(arr.visible());
}

TEST_CASE("project patch: a wire, a re-route, add / remove / move devices, mixer changes — one undo step each, shown in the Ableton view") {
  Ui u;
  u.show();
  doc::DocumentStore& st = u.ctx.store();
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  const doc::TrackId keys = u.track("Keys").id, pad = u.track("Pad").id, mod = u.track("Mod").id, grp = u.track("Group").id;
  const doc::TrackId space = u.track("Space").id;
  const doc::DeviceId lfo = u.track("Mod").devices[0]->id, eq = u.track("Pad").devices[1]->id;

  SUBCASE("wire: an export dragged onto a device, a param chosen") {
    u.zoomTo("Pad");
    u.view().fitAll(false);
    u.d.tree().layoutNow();
    const uint32_t lfoNode = u.deviceNode(lfo), eqNode = u.deviceNode(eq);
    u.d.drag(u.outPin(lfoNode, "lfo"), u.centre(eqNode), 6);
    Menu* m = u.openMenu();
    REQUIRE(m != nullptr);
    const std::string first = m->items().at(0).label;
    m->choose(0);
    u.d.step();
    CHECK(u.history() == h0 + 1);
    const doc::DeviceInfo* info = library().catalog().find("eq_eight");
    REQUIRE(info != nullptr);
    std::string param;
    for (const doc::DeviceParamInfo& pi : info->params)
      if ((pi.label.empty() ? pi.name : pi.label) == first) param = pi.name;
    CHECK_FALSE(doc::ops::wiresInto(*u.ctx.doc(), eq, param).empty());
    CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
    // The Device view (Ableton view) draws the new wire's cable into Pad's EQ Eight (docked on its knob, or on the
    // panel's frame when that knob is on another band's page).
    const doc::WireId made = doc::ops::wiresInto(*u.ctx.doc(), eq, param).at(0);
    const CableLayer::Cable* cable = u.deviceViewCable(pad, made);
    CHECK(cable != nullptr);
    REQUIRE(u.ctx.undo());
    CHECK(u.ctx.doc() == start);
  }
  SUBCASE("re-route: a mixer's out dragged onto a group's Σ, onto a return's Σ") {
    u.view().fitAll(false);
    u.d.tree().layoutNow();
    u.d.drag(u.outPin(u.pp().bands[u.pp().bandOf(mod)].mixer, "out"), u.sumOf("Group"), 6);
    CHECK(u.history() == h0 + 1);
    CHECK(u.track("Mod").mixer.output.kind == doc::OutputKind::Track);
    CHECK(u.track("Mod").mixer.output.track == grp);
    // The model now draws the route into the group's Σ.
    bool routed = false;
    for (const Edge& e : u.pp().edges)
      routed = routed || (e.kind == EdgeKind::Route && u.pp().nodes[e.from].band == u.pp().bandOf(mod) &&
                          u.pp().nodes[e.to].band == u.pp().bandOf(grp));
    CHECK(routed);
    u.d.drag(u.outPin(u.pp().bands[u.pp().bandOf(mod)].mixer, "out"), u.sumOf("Space"), 6);
    CHECK(u.history() == h0 + 2);
    bool on = false;
    for (const doc::Send& s : u.track("Mod").mixer.sends) on = on || (s.target == space && s.levelDb == 0.0f);
    CHECK(on);
    // The mixer (Ableton view) shows both: Mod's output menu has Group checked, its send to Space is at 0 dB.
    MixerStrip& ms = u.mixerStrip("Mod");
    REQUIRE(ms.output() != nullptr);
    u.d.click(u.mid(ms.output()->rect()));
    Menu* om = u.openMenu();
    REQUIRE(om != nullptr);
    bool groupChecked = false;
    for (const MenuItem& it : om->items()) groupChecked = groupChecked || (it.label == "Group" && it.checked);
    CHECK(groupChecked);
    u.d.press("Escape");
    bool sendShown = false;
    for (size_t i = 0; i < ms.sendCount(); ++i)
      if (ms.sendTarget(i) == space) sendShown = ms.send(i)->valueText() == formatVolume(0.0);
    CHECK(sendShown);
    REQUIRE(u.ctx.undo());
    REQUIRE(u.ctx.undo());
    CHECK(u.ctx.doc() == start);
  }
  SUBCASE("devices: add from the band menu, delete with Delete, move by dragging, deactivate with 0") {
    u.zoomTo("Pad");
    const uint32_t pb = u.pp().bandOf(pad);
    const Node& padMixer = u.pp().nodes[u.pp().bands[pb].mixer];
    const Vec2 empty = u.dev(padMixer.x + padMixer.w + 20.0f, u.pp().bands[pb].y + u.pp().bands[pb].h * 0.5f);
    u.d.click(empty, mod::kNone, PointerButton::Right);
    Menu* m = u.openMenu();
    REQUIRE(m != nullptr);
    m->setHighlighted(0);
    Menu* sub = m->openSubmenu();
    REQUIRE(sub != nullptr);
    u.choose(*sub, "Utility");
    CHECK(u.history() == h0 + 1);
    REQUIRE(u.track("Pad").devices.size() == 3);
    const doc::DeviceId util = u.track("Pad").devices[2]->id;
    CHECK(u.track("Pad").devices[2]->patch == "utility");
    // The Device view shows it.
    u.ctx.setSelectedTrack(pad);
    DeviceChainView* chain = findDeviceChain(*u.shell);
    REQUIRE(chain != nullptr);
    CHECK(chain->frameOf(util) != nullptr);
    // Move: drag the Utility block left of the EQ Eight.
    u.d.tree().layoutNow();
    const Node& eqn = u.pp().nodes[u.deviceNode(eq)];
    u.d.drag(u.centre(u.deviceNode(util)), u.dev(eqn.x + 4.0f, eqn.y + eqn.h * 0.5f), 8);
    CHECK(u.history() == h0 + 2);
    REQUIRE(u.track("Pad").devices.size() == 3);
    CHECK(u.track("Pad").devices[1]->id == util);
    // Move to another band: onto Keys.
    u.view().fitAll(false);
    u.d.tree().layoutNow();
    const uint32_t kb = u.pp().bandOf(keys);
    const Node& kmix = u.pp().nodes[u.pp().bands[kb].mixer];
    u.d.drag(u.centre(u.deviceNode(util)), u.dev(kmix.x - 8.0f, kmix.y + 12.0f), 8);
    CHECK(u.history() == h0 + 3);
    CHECK(doc::findDevice(*u.ctx.doc(), util).track->id == keys);
    // 0 deactivates the selected block, Delete removes it.
    u.d.click(u.centre(u.deviceNode(util)));
    REQUIRE(u.view().selectedDevices() == std::vector<doc::DeviceId>{util});
    CHECK(u.d.press("0") != KeyResult::Unhandled);
    CHECK(u.history() == h0 + 4);
    CHECK_FALSE(doc::findDevice(*u.ctx.doc(), util).device->active);
    CHECK(u.d.press("Delete") != KeyResult::Unhandled);
    CHECK(u.history() == h0 + 5);
    CHECK(doc::findDevice(*u.ctx.doc(), util).device == nullptr);
    for (int k = 0; k < 5; ++k) REQUIRE(u.ctx.undo());
    CHECK(u.ctx.doc() == start);
  }
  SUBCASE("mixer: mute and solo clicks, a volume drag — one step each; a value drag only repaints") {
    u.zoomTo("Keys");
    const uint32_t kb = u.pp().bandOf(keys);
    const uint32_t mixer = u.pp().bands[kb].mixer;
    const Node& mn = u.pp().nodes[mixer];
    u.d.click(u.dev(mn.x + 16.0f, mn.y + 25.0f));  // M
    CHECK(u.history() == h0 + 1);
    CHECK(u.track("Keys").mixer.mute);
    u.d.click(u.dev(mn.x + 30.0f, mn.y + 25.0f));  // S
    CHECK(u.history() == h0 + 2);
    CHECK(u.track("Keys").mixer.solo);
    const uint64_t rebuilds = u.view().stats().rebuilds;
    const float vol = u.track("Keys").mixer.volumeDb;
    const Vec2 a = u.dev(mn.x + 30.0f, mn.y + 39.0f), b = u.dev(mn.x + 10.0f, mn.y + 39.0f);
    u.d.drag(a, b, 6);
    CHECK(u.history() == h0 + 3);
    CHECK(u.track("Keys").mixer.volumeDb < vol);
    CHECK(st.lastChanges().paramOnly());
    CHECK(u.view().stats().rebuilds == rebuilds);  // param changes repaint; no lowering, no new scene
    // The mixer (Ableton view) shows all three.
    MixerStrip& ks = u.mixerStrip("Keys");
    CHECK_FALSE(ks.activator()->on());  // muted
    CHECK(ks.solo()->on());
    CHECK(ks.volume()->valueText() == formatVolume(u.track("Keys").mixer.volumeDb));
    for (int k = 0; k < 3; ++k) REQUIRE(u.ctx.undo());
    CHECK(u.ctx.doc() == start);
  }
  SUBCASE("delete: a selected modulation wire, a send") {
    u.view().fitAll(false);
    u.d.tree().layoutNow();
    const ProjectScene& ps = u.view().scene();
    std::vector<uint32_t> wires;
    for (uint32_t w = 0; w < ps.modelEdge.size(); ++w)
      if (ps.modelEdge[w] != kNone && u.pp().edges[ps.modelEdge[w]].kind == EdgeKind::Modulation) {
        wires.push_back(w);
        break;
      }
    for (uint32_t w = 0; w < ps.modelEdge.size(); ++w)
      if (ps.modelEdge[w] != kNone) {
        const Edge& e = u.pp().edges[ps.modelEdge[w]];
        if (e.kind == EdgeKind::Send && !e.silent) {
          wires.push_back(w);
          break;
        }
      }
    REQUIRE(wires.size() == 2);
    u.view().canvas().select({}, wires);
    u.d.tree().setFocus(&u.view().canvas());
    CHECK(u.d.press("Delete") != KeyResult::Unhandled);
    CHECK(u.history() == h0 + 1);  // both in one step
    CHECK(doc::wiresOf(*u.ctx.doc()).size() == 1);
    REQUIRE(u.ctx.undo());
    CHECK(u.ctx.doc() == start);
  }
}

TEST_CASE("project patch: a device edited in the circuit editor (a user device) — drawn as it plays, one model") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  const doc::DeviceId sat = u.track("Keys").devices[1]->id;
  REQUIRE(v.openInCircuitView(sat));
  circuit::CircuitEditor* ed = u.editor();
  REQUIRE(ed != nullptr);
  // One edit in the text pane forks the device (SPEC §15 2026-10-26): one undo step; the patch view follows.
  const size_t h0 = u.history();
  const std::string& text = ed->document().text();
  const size_t at = text.find("param output     = 0dB");
  REQUIRE(at != std::string::npos);
  ed->document().replaceText(static_cast<uint32_t>(at + 19), 1, "1", true);  // 0dB -> 1dB
  ed->checkNow();
  REQUIRE(doc::findDevice(*u.ctx.doc(), sat).device->edited());
  CHECK(u.history() == h0 + 1);
  ed->closeSoon();
  u.d.step();
  const uint32_t n = v.patch().nodeOf(sat);
  REQUIRE(n != kNone);
  CHECK(v.patch().nodes[n].edited);
  CHECK(v.patch().nodes[n].lowered);
  CHECK_FALSE(v.patch().nodes[n].missing);
  CHECK(v.patch().nodes[n].title.find("(edited)") != std::string::npos);
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");
  // Opened to its atoms: the user device's own patch (as resolved for the lowering).
  const size_t nodes = v.scene().scene->nodes.size();
  v.setDeviceExpanded(sat, true);
  u.d.advance(0.5);
  CHECK(v.scene().scene->nodes.size() > nodes + 10);
  v.setDeviceExpanded(sat, false);
  u.d.advance(0.5);
  // Revert to Stock from its block's menu: one step, back to the stock device.
  REQUIRE(v.openNodeMenu(v.patch().nodeOf(sat), u.centre(v.patch().nodeOf(sat))) != nullptr);
  Menu* m = u.openMenu();
  REQUIRE(m != nullptr);
  u.choose(*m, "Revert to Stock");
  CHECK(u.history() == h0 + 2);
  CHECK_FALSE(doc::findDevice(*u.ctx.doc(), sat).device->edited());
  u.d.step();
  CHECK_FALSE(v.patch().nodes[v.patch().nodeOf(sat)].edited);
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");
}

TEST_CASE("project patch: semantic zoom — the whole project, one band, a device's atoms") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  v.fitAll(false);
  u.d.tree().layoutNow();
  const double whole = v.canvas().view().zoom;
  u.zoomTo("Keys");
  const double band = v.canvas().view().zoom;
  CHECK(band > whole);
  CHECK(v.canvas().lod() == circuit::CircuitCanvas::Lod::Near);
  // Open Keys' Saturator to its atoms: its patch's nodes are drawn inside its frame, the view zooms onto it.
  const doc::DeviceId sat = u.track("Keys").devices[1]->id;
  const size_t nodes = v.scene().scene->nodes.size();
  v.setDeviceExpanded(sat, true);
  u.d.advance(0.5);
  CHECK(v.deviceExpanded(sat));
  CHECK(v.scene().sceneNode[v.patch().nodeOf(sat)] == kNone);
  CHECK(v.scene().scene->nodes.size() > nodes + 10);
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");  // the model is the same graph
  // The chain still reaches the device: the wires into and out of its frame.
  const graph::Patch* callee = library().library().find("saturator");
  REQUIRE(callee != nullptr);
  CHECK(v.scene().scene->nodes.size() - (nodes - 1) == callee->nodeCount());
  v.setDeviceExpanded(sat, false);
  u.d.advance(0.5);
  CHECK(v.scene().scene->nodes.size() == nodes);
  // Its menu's Open in Circuit View opens the project-device circuit editor on it (T4.4-fix1; the next case edits).
  REQUIRE(v.openNodeMenu(v.patch().nodeOf(sat), u.centre(v.patch().nodeOf(sat))) != nullptr);
  Menu* m = u.openMenu();
  REQUIRE(m != nullptr);
  u.choose(*m, "Open in Circuit View");
  circuit::CircuitEditor* ed = u.editor();
  REQUIRE(ed != nullptr);
  CHECK(ed->host() != nullptr);  // on the project's device (no Save, no Play)
  CHECK(ed->shownPatch() == "saturator");
  ed->closeSoon();
  u.d.step();
  CHECK(u.editor() == nullptr);
  // Double-click on a band's empty space zooms onto it; F back to the project.
  v.fitAll(false);
  u.d.tree().layoutNow();
  const Band& pad = v.patch().bands[v.patch().bandOf(u.track("Pad").id)];
  u.d.doubleClick(u.dev(pad.x1 - 6.0f, pad.y + pad.h - 4.0f));
  u.d.advance(0.5);
  CHECK(v.canvas().view().zoom > whole * 1.5);
  CHECK(u.d.press("F") != KeyResult::Unhandled);
  u.d.advance(0.5);
  CHECK(v.canvas().view().zoom == doctest::Approx(whole));
}

TEST_CASE("project patch: hovering a wire reads its signal (meters, modulation sweeps)") {
  Ui u;
  FixedMeterSource meters;
  u.view().setMeterSource(&meters);
  u.show();
  const doc::TrackId keys = u.track("Keys").id;
  ui::MeterLevels lv;
  lv.peakDb[0] = lv.peakDb[1] = -12.0f;
  meters.set(keys, lv);
  // The route Keys → Group: the readout is Keys' level.
  uint32_t edge = kNone;
  for (uint32_t k = 0; k < u.pp().edges.size(); ++k) {
    const Edge& e = u.pp().edges[k];
    if (e.kind == EdgeKind::Route && u.pp().bands[u.pp().nodes[e.from].band].track == keys) edge = k;
  }
  REQUIRE(edge != kNone);
  u.view().updateReadout(edge);
  CHECK(u.view().readout().title.find("Keys") != std::string::npos);
  CHECK(u.view().readout().value.find("-12.0 dB") != std::string::npos);
  u.view().updateReadout(kNone);
  CHECK(u.view().readout().edge == kNone);
}

TEST_CASE("project patch: CPU cost labels meter only while asked for and shown") {
  Ui u;
  FixedDeviceCostSource src;
  const doc::DeviceId sat = u.track("Keys").devices[1]->id;
  src.set(sat, 42.5f);
  u.view().setCostSource(&src);
  u.show();
  CHECK_FALSE(src.metering());  // nothing measured until the labels are asked for
  u.view().setShowCosts(true);
  CHECK(src.metering());
  const uint32_t n = u.pp().nodeOf(sat);
  CHECK(u.view().deviceCost(n) == doctest::Approx(42.5f));
  for (const Node& other : u.pp().nodes)
    if (other.kind == NodeKind::Device && other.device != sat) CHECK(u.view().deviceCost(u.pp().nodeOf(other.device)) < 0.0f);
  // Not live: one read, then no polling (idle).
  const uint64_t reads = src.reads();
  u.d.advance(1.0);
  CHECK(src.reads() == reads);
  // Hidden: metering off; shown again: on.
  u.view().setShown(false, false);
  CHECK_FALSE(src.metering());
  u.view().setShown(true, false);
  CHECK(src.metering());
  u.view().setShowCosts(false);
  CHECK_FALSE(src.metering());
  CHECK(u.view().deviceCost(n) < 0.0f);
}

TEST_CASE("project patch: a document change while hidden costs nothing; shown, a structural edit rebuilds once") {
  Ui u;
  ProjectPatchView& v = u.view();
  const uint64_t r0 = v.stats().rebuilds;
  REQUIRE(u.ctx.store().dispatch(doc::SetTrackVolume{u.track("Keys").id, -3.0f}).ok());
  auto e = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.track("Pad").id, "utility", doc::DeviceKind::AudioEffect);
  REQUIRE(e.ok());
  REQUIRE(u.ctx.store().dispatch(e->command).ok());
  CHECK(v.stats().rebuilds == r0);  // hidden: nothing built
  u.show();
  CHECK(v.stats().rebuilds == r0 + 1);
  auto e2 = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.track("Pad").id, "saturator", doc::DeviceKind::AudioEffect);
  REQUIRE(e2.ok());
  REQUIRE(u.ctx.store().dispatch(e2->command).ok());
  CHECK(v.stats().rebuilds == r0 + 2);
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");
}

TEST_CASE("project patch: 64 tracks build, lay out and match their graph" * doctest::test_suite("slow")) {
  doc::DocumentStore store;
  REQUIRE(buildWideProject(store, 64).ok());
  const doc::LoweredProject low = lowerOf(*store.current());
  const ProjectPatch pp = buildProjectPatch(*store.current(), low, &library().catalog());
  CHECK(checkAgainstGraph(pp, *store.current(), low) == "");
  CHECK(pp.bands.size() == 64 + 8 + 2 + 1);
}

// ---- wall clock (perf suite: load-sensitive) --------------------------------------------------------------------------

TEST_CASE("project patch: 64 tracks — full-project paint < 2 ms from tiles, a morph frame within 144 Hz" * doctest::test_suite("perf")) {
  Ui u(false);
  REQUIRE(buildWideProject(u.ctx.store(), 64).ok());
  REQUIRE(u.d.enableGpu().ok());
  u.show();
  ProjectPatchView& v = u.view();
  v.fitAll(false);
  u.d.advance(1.0);  // tiles built, settled
  REQUIRE(v.canvas().lod() == circuit::CircuitCanvas::Lod::Far);
  // Full repaints of the zoomed-out project.
  std::vector<double> ms;
  for (int i = 0; i < 40; ++i) {
    v.canvas().markDirty();
    const auto t0 = std::chrono::steady_clock::now();
    u.d.step();
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(ms.begin(), ms.end());
  MESSAGE("64 tracks, fitted: median full frame " << ms[ms.size() / 2] << " ms, chunks from tiles " << v.canvas().stats().chunksFromTiles);
  CHECK(ms[ms.size() / 2] < 2.0);
  CHECK(v.canvas().stats().chunksFromTiles > 0);
  // The morph back and forth: every frame within a 144 Hz period.
  v.setShown(false, false);
  u.d.advance(0.1);
  CHECK(u.d.press("Ctrl+Alt+P") == KeyResult::Action);
  std::vector<double> mf;
  while (v.morphing()) {
    const auto t0 = std::chrono::steady_clock::now();
    u.d.step();
    mf.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    u.d.advance(1.0 / 144.0 - 1e-9);
  }
  std::sort(mf.begin(), mf.end());
  REQUIRE(!mf.empty());
  MESSAGE("morph: " << mf.size() << " frames, median " << mf[mf.size() / 2] << " ms, max " << mf.back() << " ms");
  CHECK(mf[mf.size() * 9 / 10] < 1000.0 / 144.0);
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

TEST_CASE("render: project-patch-overview matches the golden image") { golden("project-patch-overview"); }
TEST_CASE("render: project-patch-band matches the golden image") { golden("project-patch-band"); }
