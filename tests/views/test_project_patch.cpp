// T4.13 the project patch view: the model is the lowered ProjectGraph (checked node for node, wire for wire, groups and
// sends), the layout mirrors the arrangement, nudges are saved view state, the morph keeps every lane's vertical place,
// every edit kind made in the patch view is one undo step and shows in the Ableton view, semantic zoom, idle costs
// nothing. Goldens: project-patch-overview, project-patch-band. Wall-clock checks (64 tracks: full paint, morph frame)
// are in the perf suite.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/meta/envelope.h"
#include "pq/meta/phase_acc.h"
#include "pq/meta/vca.h"
#include "pq/meta/wavetable.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/devices/panel_face.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../../src/views/src/common/panel_template.h"
#include "../../src/views/src/shell/shell_widgets.h"
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

float pinY(const Node& n, size_t i) { return pinOffset(n, i); }

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
  // TX.panel-faces: where a block is grabbed to select or move it — a panel's title bar (its body holds controls), any
  // other block's centre.
  Vec2 grip(uint32_t node) const {
    const Node& n = pp().nodes[node];
    return n.panel ? dev(n.x + n.w * 0.5f, n.y + 10.0f) : centre(node);
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

TEST_CASE("project patch: an audio track's source is its clip player (T5.3), the view still matches the graph") {
  doc::DocumentStore store;
  REQUIRE(buildSketchProject(store).ok());
  auto t = doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Audio, {}, "Loop");
  REQUIRE(t.ok());
  REQUIRE(store.dispatch(t->command).ok());
  auto d = doc::ops::addDevice(*store.current(), store.ids(), t->tracks.at(0), "utility", doc::DeviceKind::AudioEffect);
  REQUIRE(d.ok());
  REQUIRE(store.dispatch(d->command).ok());
  const doc::Project& p = *store.current();
  doc::LowerOptions o;
  o.library = &library().library();
  o.clipSlotOf = [](doc::TrackId) { return 0u; };
  auto low = doc::lower(p, o);
  REQUIRE(low.ok());
  const ProjectPatch pp = buildProjectPatch(p, *low, &library().catalog());
  CHECK(checkAgainstGraph(pp, p, *low) == "");
  const auto band = std::find_if(pp.bands.begin(), pp.bands.end(), [](const Band& b) { return b.name == "Loop"; });
  REQUIRE(band != pp.bands.end());
  CHECK(band->kind == doc::TrackKind::Audio);
  REQUIRE(band->source != kNone);
  CHECK(pp.nodes[band->source].kind == NodeKind::Source);
  CHECK(pp.nodes[band->source].title == "Audio");  // T5.x-one-circuit: was "Clips" (the lane window says what it plays)
  REQUIRE(band->devices.size() == 1);
}

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
  run(doc::ops::connectTravelWire(*store.current(), store.ids(), lfo, "lfo", util, "width", 0.3));
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

// TMM.10-fix1 (SPEC §15 2026-11-01 "notes are a signal"): the note wire is drawn to the chain meta device the note
// source reaches — and only there: unplugged, or after another device, it has none.
TEST_CASE("project patch: the note wire goes to the meta device the note source reaches, or nowhere") {
  doc::DocumentStore store;
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  };
  const doc::TrackId t = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi)).tracks.at(0);
  const doc::DeviceId acc = run(doc::ops::addMetaDevice(*store.current(), store.ids(), t, "phase_acc", 0)).devices.at(0);
  const doc::DeviceId wt = run(doc::ops::addMetaDevice(*store.current(), store.ids(), t, "wavetable")).devices.at(0);
  auto notesTo = [&](const ProjectPatch& pp, doc::DeviceId d) {
    for (const Edge& e : pp.edges)
      if (e.kind == EdgeKind::Notes && e.to == pp.nodeOf(d)) return true;
    return false;
  };
  {
    const doc::LoweredProject low = lowerOf(*store.current());
    const ProjectPatch pp = buildProjectPatch(*store.current(), low, &library().catalog());
    CHECK(checkAgainstGraph(pp, *store.current(), low) == "");
    CHECK(pp.nodes[pp.nodeOf(acc)].instrument);
    CHECK(notesTo(pp, acc));
    CHECK_FALSE(notesTo(pp, wt));
  }
  // Unplugged: no note wire; it runs freely (one voice) and still feeds the Wavetable.
  run(doc::ops::setDeviceChainInput(*store.current(), acc, std::string(doc::kChainUnplugged)));
  {
    const doc::LoweredProject low = lowerOf(*store.current());
    const ProjectPatch pp = buildProjectPatch(*store.current(), low, &library().catalog());
    CHECK(checkAgainstGraph(pp, *store.current(), low) == "");
    CHECK_FALSE(pp.nodes[pp.nodeOf(acc)].instrument);
    CHECK_FALSE(notesTo(pp, acc));
    CHECK_FALSE(notesTo(pp, wt));
    bool chained = false;
    for (const Edge& e : pp.edges) chained = chained || (e.kind == EdgeKind::Chain && e.from == pp.nodeOf(acc) && e.to == pp.nodeOf(wt));
    CHECK(chained);
  }
}

// T4.8: racks are drawn as plain blocks; the devices inside them (racks lower inline) belong to the rack's block, and
// wires into / out of them end on it.
TEST_CASE("project patch: racks (nested, instrument, wired into and out of) match the graph as one block each") {
  doc::DocumentStore store;
  REQUIRE(buildSketchProject(store).ok());
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  };
  const doc::TrackId keys = named(*store.current(), "Keys")->id, pad = named(*store.current(), "Pad")->id;
  // Keys: its effects grouped into an Audio Effect Rack; a second chain holding a nested rack with a Utility.
  std::vector<doc::DeviceId> fx;
  for (const doc::DevicePtr& d : named(*store.current(), "Keys")->devices)
    if (d->kind == doc::DeviceKind::AudioEffect) fx.push_back(d->id);
  REQUIRE_FALSE(fx.empty());
  const doc::DeviceId rack = run(doc::ops::groupDevices(*store.current(), store.ids(), fx)).devices.at(0);
  const doc::DeviceId util = run(doc::ops::addDevice(*store.current(), store.ids(), keys, "utility", doc::DeviceKind::AudioEffect)).devices.at(0);
  const doc::DeviceId inner = run(doc::ops::groupDevices(*store.current(), store.ids(), std::vector<doc::DeviceId>{util})).devices.at(0);
  run(doc::ops::moveDeviceToNewChain(*store.current(), store.ids(), inner, rack));
  // Pad: its instrument in an Instrument Rack.
  const doc::DeviceId padInst = named(*store.current(), "Pad")->devices.at(0)->id;
  const doc::DeviceId irack = run(doc::ops::groupDevices(*store.current(), store.ids(), std::vector<doc::DeviceId>{padInst})).devices.at(0);
  // Wires from the Mod track's LFO into the nested Utility's width and into the outer rack's first macro.
  const doc::DeviceId lfo = named(*store.current(), "Mod")->devices[0]->id;
  const doc::WireId toUtil = run(doc::ops::connectTravelWire(*store.current(), store.ids(), lfo, "lfo", util, "width", 0.3)).wires.at(0);
  const doc::WireId toMacro = run(doc::ops::connectTravelWire(*store.current(), store.ids(), lfo, "lfo", rack, "macro1", 0.2)).wires.at(0);

  const doc::Project& p = *store.current();
  const doc::LoweredProject low = lowerOf(p);
  const ProjectPatch pp = buildProjectPatch(p, low, &library().catalog());
  CHECK(checkAgainstGraph(pp, p, low) == "");
  const uint32_t rn = pp.nodeOf(rack);
  REQUIRE(rn != kNone);
  const Node& r = pp.nodes[rn];
  CHECK(r.rack);
  CHECK(r.lowered);
  CHECK_FALSE(r.missing);
  CHECK(r.title == "Audio Effect Rack");
  CHECK(r.band == pp.bandOf(keys));
  CHECK(std::find(r.inner.begin(), r.inner.end(), util) != r.inner.end());
  CHECK(std::find(r.inner.begin(), r.inner.end(), inner) != r.inner.end());
  CHECK(pp.nodeOf(util) == rn);  // a device inside a rack: the rack's block
  CHECK(pp.nodeOf(inner) == rn);
  const std::string utilPort = "width@" + std::to_string(util.value);
  CHECK(r.input(utilPort) >= 0);
  CHECK(r.input("macro1") >= 0);
  int wiresIn = 0;
  for (const Edge& e : pp.edges)
    if (e.kind == EdgeKind::Modulation && (e.wire == toUtil || e.wire == toMacro)) {
      CHECK(e.to == rn);
      ++wiresIn;
    }
  CHECK(wiresIn == 2);
  // The chain runs through the rack's block.
  bool chained = false;
  for (const Edge& e : pp.edges) chained = chained || (e.kind == EdgeKind::Chain && e.to == rn);
  CHECK(chained);
  const uint32_t ir = pp.nodeOf(irack);
  REQUIRE(ir != kNone);
  CHECK(pp.nodes[ir].rack);
  CHECK(pp.nodes[ir].instrument);
  CHECK(pp.nodes[ir].lowered);
  CHECK(pp.nodes[ir].band == pp.bandOf(pad));
  bool notes = false;
  for (const Edge& e : pp.edges) notes = notes || (e.kind == EdgeKind::Notes && e.to == ir);
  CHECK(notes);
  // A rack with nothing that plays (an Instrument Rack whose chain was muted) is drawn but not lowered.
  run(doc::ops::setChainActive(*store.current(), doc::findDevice(*store.current(), irack).device->rack->chains.at(0)->id, false));
  const doc::LoweredProject low2 = lowerOf(*store.current());
  const ProjectPatch pp2 = buildProjectPatch(*store.current(), low2, &library().catalog());
  CHECK(checkAgainstGraph(pp2, *store.current(), low2) == "");
  CHECK_FALSE(pp2.nodes[pp2.nodeOf(irack)].lowered);
  // A broken model is caught: the rack drawn as not lowered while its devices play.
  ProjectPatch broken = pp;
  broken.nodes[rn].lowered = false;
  CHECK(checkAgainstGraph(broken, p, low) != "");
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
    // Move: drag the Utility block left of the EQ Eight (TX.panel-faces: by its panel's title bar; the band framed
    // again, the new panel widened it past the window).
    u.zoomTo("Pad");
    const Node& eqn = u.pp().nodes[u.deviceNode(eq)];
    u.d.drag(u.grip(u.deviceNode(util)), u.dev(eqn.x + 4.0f, eqn.y + eqn.h * 0.5f), 8);
    CHECK(u.history() == h0 + 2);
    REQUIRE(u.track("Pad").devices.size() == 3);
    CHECK(u.track("Pad").devices[1]->id == util);
    // Move to another band: onto Keys.
    u.view().fitAll(false);
    u.d.tree().layoutNow();
    const uint32_t kb = u.pp().bandOf(keys);
    const Node& kmix = u.pp().nodes[u.pp().bands[kb].mixer];
    u.d.drag(u.grip(u.deviceNode(util)), u.dev(kmix.x - 8.0f, kmix.y + 12.0f), 8);
    CHECK(u.history() == h0 + 3);
    CHECK(doc::findDevice(*u.ctx.doc(), util).track->id == keys);
    // 0 deactivates the selected block, Delete removes it.
    u.d.click(u.grip(u.deviceNode(util)));
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

TEST_CASE("project patch cursors (T4.16c): blocks Move, out pins Crosshair, M / S Pointer, bars ResizeEW; a refused drop NotAllowed") {
  Ui u;
  u.show();
  const doc::TrackId keys = u.track("Keys").id, mod = u.track("Mod").id;
  const doc::DeviceId lfo = u.track("Mod").devices[0]->id, eq = u.track("Pad").devices[1]->id;
  auto at = [&](Vec2 p) {
    u.d.move(p);
    return u.d.tree().cursor();
  };
  // A mixer strip: M and S are buttons, the volume bar a sideways drag (also while it runs).
  u.zoomTo("Keys");
  const Node& mn = u.pp().nodes[u.pp().bands[u.pp().bandOf(keys)].mixer];
  CHECK(at(u.dev(mn.x + 16.0f, mn.y + 25.0f)) == Cursor::Pointer);
  CHECK(at(u.dev(mn.x + 30.0f, mn.y + 25.0f)) == Cursor::Pointer);
  const Vec2 vol = u.dev(mn.x + 30.0f, mn.y + 39.0f);
  CHECK(at(vol) == Cursor::ResizeEW);
  const size_t h0 = u.history();
  u.d.down(vol);
  u.d.move({vol.x - 15.0f, vol.y + 30.0f});
  CHECK(u.d.tree().cursor() == Cursor::ResizeEW);
  u.d.up({vol.x - 15.0f, vol.y + 30.0f});
  CHECK(u.history() == h0 + 1);  // the drag set the volume, as the cursor said
  // The band's title strip nudges the band.
  const Band& kb = u.pp().bands[u.pp().bandOf(keys)];
  CHECK(at(u.dev(kb.x0 + 4.0f, kb.y + 2.0f)) == Cursor::Move);
  // Device blocks move; an export pin (and a mixer's out) starts a wire.
  u.view().fitAll(false);
  u.d.tree().layoutNow();
  CHECK(at(u.centre(u.deviceNode(eq))) == Cursor::Move);
  CHECK(at(u.outPin(u.deviceNode(lfo), "lfo")) == Cursor::Crosshair);
  const Vec2 out = u.outPin(u.pp().bands[u.pp().bandOf(mod)].mixer, "out");
  CHECK(at(out) == Cursor::Crosshair);
  // Dragging Mod's out: over the Group's Σ a route (Crosshair); over a MIDI track (Keys) refused: NotAllowed, and the
  // release there changes nothing.
  u.d.down(out);
  u.d.move(u.sumOf("Group"));
  CHECK(u.d.tree().cursor() == Cursor::Crosshair);
  u.d.move(u.sumOf("Keys"));
  CHECK(u.d.tree().cursor() == Cursor::NotAllowed);
  u.d.up(u.sumOf("Keys"));
  CHECK(u.history() == h0 + 1);
  CHECK(u.ctx.status().find("Route") != std::string::npos);
  // What a pointer move costs with the cursor worked out (indicative: a dev build on a shared machine).
  {
    const Vec2 p0 = u.centre(u.deviceNode(eq));
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < 400; ++k) {
      u.d.move({p0.x + static_cast<float>(k % 40), p0.y});
      (void)u.d.tree().cursor();
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 400.0;
    MESSAGE("project patch: pointer move + cursor: " << us << " us (" << u.pp().nodes.size() << " nodes)");
  }
  // A middle drag pans: Move.
  const Vec2 c = u.centre(u.deviceNode(eq));
  u.d.down(c, PointerButton::Middle);
  u.d.move({c.x + 20.0f, c.y});
  CHECK(u.d.tree().cursor() == Cursor::Move);
  u.d.up({c.x + 20.0f, c.y}, PointerButton::Middle);
}

TEST_CASE("project patch: a device edited in the circuit editor (a user device) — drawn as it plays, one model") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  const doc::DeviceId sat = u.track("Keys").devices[1]->id;
  REQUIRE(v.editDeviceCircuit(sat));  // T5.x-circuit-chain: the Device level (openInCircuitView: the track's chain)
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
  // TX.panel-faces: the band is framed whole; its blocks are panels (Operator's is about 600 units wide), so in this
  // window the band fits below the canvas's Near level — the panels draw their own text at the canvas's scale.
  CHECK(v.canvas().lod() != circuit::CircuitCanvas::Lod::Far);
  CHECK(v.panelOf(u.track("Keys").devices[1]->id) != nullptr);
  // Open Keys' Saturator to its atoms: its patch's nodes are drawn inside its frame, the view zooms onto it.
  const doc::DeviceId sat = u.track("Keys").devices[1]->id;
  const size_t nodes = v.scene().scene->nodes.size();
  v.setDeviceExpanded(sat, true);
  u.d.advance(0.5);
  CHECK(v.deviceExpanded(sat));
  CHECK(v.scene().sceneNode[v.patch().nodeOf(sat)] == kNone);
  CHECK(v.scene().scene->nodes.size() > nodes + 10);
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");  // the model is the same graph
  // Hovering one of its atoms names it in the info view (the atom and what it does), not the view's general help:
  // the shell's info view itself, which re-reads when the hovered item inside the canvas changes.
  {
    auto* infoView = dynamic_cast<shell::InfoView*>(u.d.tree().find("main/body/bottom/info"));
    REQUIRE(infoView != nullptr);
    const circuit::CircuitScene& sc = *v.scene().scene;
    const IRect cr = v.canvas().rect();
    uint32_t named = 0;
    for (uint32_t i = 0; i < sc.nodes.size() && named < 4; ++i) {
      const circuit::SceneNode& n = sc.nodes[i];
      const circuit::SymbolSpec& spec = sc.specs[n.spec];
      if (n.hidden || spec.face != nullptr || spec.help.empty() || spec.symbol == circuit::Symbol::Param) continue;
      const Vec2 c = v.canvas().toDevice({n.x + n.w * 0.5f, n.y + n.h * 0.5f});
      if (c.x <= cr.x || c.y <= cr.y || c.x >= cr.right() || c.y >= cr.y + cr.h) continue;
      if (v.canvas().hitTest(c) != circuit::CanvasHit{circuit::CanvasHit::Kind::Node, i}) continue;
      u.d.move(c);
      CAPTURE(spec.kind);
      CHECK(infoView->title() != "Project Patch");
      CHECK(infoView->title().find(spec.kind) != std::string::npos);
      CHECK(infoView->body().find(spec.help) != std::string::npos);
      ++named;
    }
    CHECK(named > 0);
  }
  // The chain still reaches the device: the wires into and out of its frame.
  const graph::Patch* callee = library().library().find("saturator");
  REQUIRE(callee != nullptr);
  CHECK(v.scene().scene->nodes.size() - (nodes - 1) == callee->nodeCount());
  v.setDeviceExpanded(sat, false);
  u.d.advance(0.5);
  CHECK(v.scene().scene->nodes.size() == nodes);
  // Its menu's Open in Circuit View opens its band here, framed on it and focused (T5.x-one-circuit: one circuit view);
  // Edit Device Circuit the project-device circuit editor on it (T4.4-fix1; the next case edits).
  REQUIRE(v.openNodeMenu(v.patch().nodeOf(sat), u.centre(v.patch().nodeOf(sat))) != nullptr);
  Menu* m = u.openMenu();
  REQUIRE(m != nullptr);
  u.choose(*m, "Open in Circuit View");
  u.d.advance(0.5);
  CHECK(v.focusedDevice() == sat);
  CHECK(v.openedBand() == u.track("Keys").id);
  CHECK(u.shell->overlay().childCount() == 0);
  v.fitAll(false);
  u.d.tree().layoutNow();
  REQUIRE(v.openNodeMenu(v.patch().nodeOf(sat), u.centre(v.patch().nodeOf(sat))) != nullptr);
  m = u.openMenu();
  REQUIRE(m != nullptr);
  u.choose(*m, "Edit Device Circuit");
  circuit::CircuitEditor* ed = u.editor();
  REQUIRE(ed != nullptr);
  CHECK(ed->host() != nullptr);  // on the project's device (no Save, no Play)
  CHECK(ed->shownPatch() == "saturator");
  ed->closeSoon();
  u.d.step();
  CHECK(u.editor() == nullptr);
  // Double-click on a band's empty space opens it (T5.x-circuit-chain: zoomed onto it, its devices opened to their
  // atoms, so the band is larger than its blocks were: the view frames it, it does not zoom in by a fixed factor); F
  // back to the project (the band's devices closed again).
  v.fitAll(false);
  u.d.tree().layoutNow();
  const Band& pad = v.patch().bands[v.patch().bandOf(u.track("Pad").id)];
  u.d.doubleClick(u.dev(pad.x1 - 6.0f, pad.y + pad.h - 4.0f));
  u.d.advance(0.5);
  CHECK(v.openedBand() == u.track("Pad").id);
  {
    const Band& opened = v.patch().bands[v.patch().bandOf(u.track("Pad").id)];
    const Vec2 a = u.dev(opened.x0, opened.y), b = u.dev(opened.x1, opened.y + opened.h);
    const IRect cr = v.canvas().rect();
    CHECK(a.x >= static_cast<float>(cr.x) - 1.0f);
    CHECK(b.x <= static_cast<float>(cr.right()) + 1.0f);
    CHECK(std::max(b.x - a.x, b.y - a.y) >= std::min(static_cast<float>(cr.w), static_cast<float>(cr.h)) * 0.5f);
  }
  CHECK(u.d.press("F") != KeyResult::Unhandled);
  u.d.advance(0.5);
  CHECK(v.canvas().view().zoom == doctest::Approx(whole));
}

// TMM.10-fix2 (SPEC §15 2026-11-01 "the note source is a Hz piano roll"): a MIDI track's lane window is its note source
// ("Notes"); its note outputs (Hz, gate, velocity) are wire sources like exports: Crosshair on them, a drop on a device a
// param menu (one undo step), a drop on a param pin a wire already drives that param, the wire drawn from the window
// (per voice) and read on hover, the chain's own `notes` pin and a drop off the devices refused (NotAllowed, status).
TEST_CASE("project patch (TMM.10-fix2): a note output dragged onto a device wires it — menu, undo, drawn, readable, refusals") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());  // TX.mod-all: the canvas paints (the panels lay out: a drop onto a knob)
  u.show();
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  const doc::TrackId keys = u.track("Keys").id;
  const doc::DeviceId sat = u.track("Keys").devices[1]->id;
  u.zoomTo("Keys");
  const uint32_t src = u.pp().bands[u.pp().bandOf(keys)].source;
  const Node& sn = u.pp().nodes[src];
  CHECK(sn.title == "Notes");
  for (const char* port : {"notes", "hz", "gate", "velocity"}) CHECK(sn.output(port) >= 0);
  auto at = [&](Vec2 p) {
    u.d.move(p);
    return u.d.tree().cursor();
  };
  CHECK(at(u.outPin(src, "velocity")) == Cursor::Crosshair);
  // Hovering a note output names it in the shell's info view.
  {
    auto* infoView = dynamic_cast<shell::InfoView*>(u.d.tree().find("main/body/bottom/info"));
    REQUIRE(infoView != nullptr);
    u.d.move(u.outPin(src, "hz"));
    CHECK(infoView->title() == "Keys notes: Hz");
    CHECK(infoView->body().find("piano roll") != std::string::npos);
  }
  // velocity → the Saturator: its param menu, one step.
  u.d.drag(u.outPin(src, "velocity"), u.centre(u.deviceNode(sat)), 6);
  Menu* m = u.openMenu();
  REQUIRE(m != nullptr);
  m->choose(0);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  REQUIRE(doc::wiresOf(*u.ctx.doc()).size() == 3);  // the sketch's two LFO wires and this one
  const doc::Wire w = doc::wiresOf(*u.ctx.doc()).back();
  CHECK(w.noteSource == keys);
  CHECK(w.port == "velocity");
  CHECK(w.to == sat);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Drawn from the window's velocity pin into the Saturator's param pin, per voice, lowered.
  uint32_t edge = kNone;
  for (uint32_t k = 0; k < u.pp().edges.size(); ++k)
    if (u.pp().edges[k].kind == EdgeKind::Modulation && u.pp().edges[k].wire == w.id) edge = k;
  REQUIRE(edge != kNone);
  const Edge& e = u.pp().edges[edge];
  CHECK(e.from == src);
  CHECK(u.pp().nodes[src].outputs[e.fromPort].name == "velocity");
  CHECK(e.to == u.deviceNode(sat));
  CHECK(u.pp().nodes[e.to].inputs[e.toPort].param);
  CHECK(e.voice);
  CHECK(e.status == doc::WireStatus::Lowered);
  CHECK(orthogonal(e.points));
  u.view().updateReadout(edge);
  CHECK(u.view().readout().title.find("Keys notes: velocity") != std::string::npos);
  CHECK(u.view().readout().value.find("adds in the target's units") != std::string::npos);
  u.view().updateReadout(kNone);
  // Hz onto the param the first wire made: that param directly (no menu), one step. TX.mod-all: the Saturator is drawn
  // as its panel and the param's knob is visible, so the param has no pin: the wire ends on the knob, and dropping
  // onto the knob wires it.
  {
    const Node& tn = u.pp().nodes[e.to];
    const Port& port = tn.inputs[e.toPort];
    CHECK(port.anchored);
    u.d.advance(0.5);  // the panel is laid out at this zoom
    const Face* panel = u.view().canvas().panelOn(u.view().scene().sceneNode[e.to]);
    REQUIRE(panel != nullptr);
    const std::optional<ControlAnchor> knob = panel->controlAnchor(port.name);
    REQUIRE(knob.has_value());
    // The layout placed the wire's entry by the same control (a prototype of the panel, laid out at scale 1).
    CHECK(std::fabs(port.ax - knob->at.x) <= 2.0f);
    CHECK(std::fabs(port.ay - knob->at.y) <= 2.0f);
    const Vec2 pin = u.dev(tn.x + knob->at.x, tn.y + knob->at.y);
    const size_t into = doc::ops::wiresInto(*u.ctx.doc(), sat, w.param).size();  // the sketch's LFO may drive it too
    u.d.drag(u.outPin(src, "hz"), pin, 6);
    CHECK(u.openMenu() == nullptr);
    CHECK(u.history() == h0 + 2);
    CHECK(doc::ops::wiresInto(*u.ctx.doc(), sat, w.param).size() == into + 1);
    CHECK(doc::wiresOf(*u.ctx.doc()).back().port == "hz");
  }
  // Refused: the chain's own `notes` pin (NotAllowed over a device), a note output onto a mixer.
  u.d.down(u.outPin(src, "notes"));
  u.d.move(u.centre(u.deviceNode(sat)));
  CHECK(u.d.tree().cursor() == Cursor::NotAllowed);
  u.d.up(u.centre(u.deviceNode(sat)));
  CHECK(u.history() == h0 + 2);
  CHECK(u.ctx.status().find("note output") != std::string::npos);
  const uint32_t mixer = u.pp().bands[u.pp().bandOf(keys)].mixer;
  u.d.down(u.outPin(src, "gate"));
  u.d.move(u.centre(mixer));
  CHECK(u.d.tree().cursor() == Cursor::NotAllowed);
  u.d.up(u.centre(mixer));
  CHECK(u.history() == h0 + 2);
  // Undo, twice: the project as it was.
  REQUIRE(u.ctx.undo());
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
}

TEST_CASE("project patch (TMM.10-fix2): note-source wires on the band and across tracks match the graph; saved and loaded") {
  doc::DocumentStore store;
  MetaChainIds ids;
  REQUIRE(buildMetaChainProject(store, &ids).ok());
  const doc::DeviceId filter = doc::findTrackPtr(*store.current(), ids.lead)->devices.at(2)->id;
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
  };
  run(doc::ops::connectTravelNoteWire(*store.current(), store.ids(), ids.lead, "velocity", ids.wt, "position"));  // per voice
  run(doc::ops::connectTravelNoteWire(*store.current(), store.ids(), ids.lead, "hz", filter, "freq"));           // reduced
  run(doc::ops::connectTravelNoteWire(*store.current(), store.ids(), ids.bass, "gate", filter, "res"));          // across tracks
  // Saved and loaded: the same project (the wires' note sources included).
  Result<std::string> text = doc::serializeProject(*store.current(), store.view());
  REQUIRE(text.ok());
  Result<doc::LoadedProject> back = doc::parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == *store.current());
  const doc::Project& p = *back->project;
  const doc::LoweredProject low = lowerOf(p);
  for (const doc::WireState& s : low.wires) CHECK(s.status == doc::WireStatus::Lowered);
  const ProjectPatch pp = buildProjectPatch(p, low, &library().catalog());
  CHECK(checkAgainstGraph(pp, p, low) == "");
  size_t fromWindows = 0, across = 0;
  for (const Edge& e : pp.edges) {
    if (e.kind != EdgeKind::Modulation || pp.nodes[e.from].kind != NodeKind::Source) continue;
    ++fromWindows;
    CHECK(e.voice);
    CHECK(orthogonal(e.points));
    across += e.graphWire != kNone ? 1u : 0u;
  }
  CHECK(fromWindows == 3);
  CHECK(across == 1);  // Bass → Lead: a graph wire m<id> (reduced in Bass's patch)
}

// A chain meta device has no patch text: "Edit Device Circuit" is not offered (its atoms are generated from its face),
// and asked directly the status says so — not "'meta:wavetable' is not in the device library" (coordinator report).
// TX.patching (SPEC §15 2026-11-01 "free patching in the circuit"): a device added unconnected sits under the chain;
// any output (a device's main one too) goes onto any input or param — a face's control too —, the chain's `notes` pin
// is refused; a block dragged out of the series below the chain becomes free. Each edit one undo step, the view still
// the lowered graph.
TEST_CASE("project patch (TX.patching): free devices, any output onto any input, param or face control") {
  Ui u(false);
  auto t = doc::ops::createTrack(*u.ctx.doc(), u.ctx.store().ids(), doc::TrackKind::Midi, {}, "Keys");
  REQUIRE(t.ok());
  REQUIRE(u.ctx.store().dispatch(t->command).ok());
  const doc::TrackId keys = t->tracks.at(0);
  auto ua = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), keys, "utility", doc::DeviceKind::AudioEffect);
  REQUIRE(ua.ok());
  REQUIRE(u.ctx.store().dispatch(ua->command).ok());
  const doc::DeviceId inst = u.track("Keys").devices.at(0)->id, util = ua->devices.at(0);
  u.show();
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  // Add Free Device: one step, out of the series, under the chain.
  REQUIRE(u.view().addFreeDevice(keys, "meta:phase_acc"));
  u.d.step();
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.track("Keys").devices.size() == 3);
  const doc::DeviceId pa = u.track("Keys").devices.back()->id;
  CHECK(u.track("Keys").devices.back()->free);
  CHECK(u.track("Keys").devices.at(0)->id == inst);  // the instrument still first: nothing replaced
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  u.zoomTo("Keys");
  const Node& pn = u.pp().nodes[u.deviceNode(pa)];
  const Node& un = u.pp().nodes[u.deviceNode(util)];
  CHECK(pn.free);
  CHECK(pn.y >= un.y + un.h);  // its row is under the chain's
  for (const Edge& e : u.pp().edges) CHECK_FALSE((e.kind == EdgeKind::Chain && (e.from == u.deviceNode(pa) || e.to == u.deviceNode(pa))));
  // The accumulator's output dropped on the Utility: its menu offers its inputs too; its audio input wires it there.
  u.d.drag(u.outPin(u.deviceNode(pa), "out"), u.centre(u.deviceNode(util)), 6);
  Menu* m = u.openMenu();
  REQUIRE(m != nullptr);
  u.choose(*m, "Input: Audio In");
  CHECK(u.history() == h0 + 2);
  const doc::Wire w1 = doc::wiresOf(*u.ctx.doc()).back();
  CHECK(w1.from == pa);
  CHECK(w1.port == "out");
  CHECK(w1.to == util);
  CHECK(w1.param == "x");
  CHECK(w1.law == doc::WireLaw::Add);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // The instrument's main output onto the accumulator face's frequency control: that param, no menu.
  {
    const uint32_t node = u.deviceNode(pa);
    const uint32_t sn = u.view().scene().sceneNode[node];
    MetaFace* face = u.view().canvas().faceOn(sn);
    REQUIRE(face != nullptr);
    const std::optional<ui::Rect> r = face->paramRect(static_cast<size_t>(face->type().paramIndex("freq")));
    REQUIRE(r.has_value());
    const Node& n = u.pp().nodes[node];
    const Vec2 on = u.dev(n.x + 0.5f * (r->x0 + r->x1), n.y + 0.5f * (r->y0 + r->y1));
    u.d.drag(u.outPin(u.deviceNode(inst), "out"), on, 6);
    CHECK(u.openMenu() == nullptr);
    CHECK(u.history() == h0 + 3);
    const doc::Wire w2 = doc::wiresOf(*u.ctx.doc()).back();
    CHECK(w2.from == inst);
    CHECK(w2.port == "out");
    CHECK(w2.to == pa);
    CHECK(w2.param == "freq");
  }
  // Refused: onto the instrument's own `notes` pin (the chain's note wire).
  {
    const Node& in = u.pp().nodes[u.deviceNode(inst)];
    REQUIRE(in.input("notes") == 0);
    const Vec2 notes = u.dev(in.x, in.y + pinY(in, 0));
    u.d.down(u.outPin(u.deviceNode(util), "out"));
    u.d.move(notes);
    CHECK(u.d.tree().cursor() == Cursor::NotAllowed);
    u.d.up(notes);
    CHECK(u.history() == h0 + 3);
  }
  // The Utility dragged down out of the series: free, one step; its wires stay.
  {
    const Node& n = u.pp().nodes[u.deviceNode(util)];
    const Vec2 from = u.centre(u.deviceNode(util));
    const Vec2 below = u.dev(n.x + 0.5f * n.w, n.y + n.h + 3.0f * metrics::kFreeRowGap);
    u.d.drag(from, below, 8);
    u.d.step();
    CHECK(u.history() == h0 + 4);
    CHECK(doc::findDevice(*u.ctx.doc(), util).device->free);
    CHECK(doc::wiresOf(*u.ctx.doc()).size() == 2);
    CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  }
  for (int i = 0; i < 4; ++i) REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
}

TEST_CASE("project patch: a chain meta device's circuit is not edited as a library device") {
  Ui u(false);
  MetaChainIds ids;
  REQUIRE(buildMetaChainProject(u.ctx.store(), &ids).ok());
  u.show();
  u.view().fitAll(false);
  u.d.tree().layoutNow();
  CHECK_FALSE(u.view().editDeviceCircuit(ids.wt));
  CHECK(u.ctx.status().find("not in the device library") == std::string::npos);
  CHECK(u.ctx.status().find("generated") != std::string::npos);
  CHECK(u.editor() == nullptr);
  REQUIRE(u.view().openNodeMenu(u.deviceNode(ids.wt), u.centre(u.deviceNode(ids.wt))) != nullptr);
  Menu* m = u.openMenu();
  REQUIRE(m != nullptr);
  bool found = false;
  for (const MenuItem& it : m->items())
    if (it.label == "Edit Device Circuit") {
      found = true;
      CHECK_FALSE(it.enabled);
    }
  CHECK(found);
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


// ---- T5.x-one-circuit: one circuit view, lane windows ---------------------------------------------------------------

TEST_CASE("one circuit: from a device's opened band, zooming out alone reaches every lane, group, return, the master and Out") {
  Ui u;
  const doc::DeviceId sat = u.track("Keys").devices[1]->id;
  REQUIRE(u.ctx.openProjectCircuit({}, sat));  // Open in Circuit View from the arrangement: the view comes up on it
  u.d.tree().layoutNow();
  ProjectPatchView& v = u.view();
  CHECK(v.shown());
  CHECK(v.focusedDevice() == sat);
  CHECK(v.openedBand() == u.track("Keys").id);
  // Ctrl+wheel out at the canvas centre (on the device, not on a lane window) until the zoom stops changing; no pan.
  const IRect cr = v.canvas().rect();
  const Vec2 c{cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f};
  REQUIRE(v.laneAt(c) == kNone);
  for (int i = 0; i < 80; ++i) (void)u.d.wheel(c, -1.0f, 0.0f, mod::kCtrl);
  u.d.step();
  const graph::Point2 w0 = v.canvas().toWorld({static_cast<float>(cr.x), static_cast<float>(cr.y)});
  const graph::Point2 w1 = v.canvas().toWorld({static_cast<float>(cr.right()), static_cast<float>(cr.bottom())});
  const ProjectPatch& pp = v.patch();
  size_t kinds[5] = {};
  for (const Band& b : pp.bands) {
    const float x0 = b.kind == doc::TrackKind::Group ? b.cx0 : b.x0, x1 = b.kind == doc::TrackKind::Group ? b.cx1 : b.x1;
    CHECK_MESSAGE((x0 >= w0.x && x1 <= w1.x && b.y >= w0.y && b.y + b.h <= w1.y), "band " << b.name << " out of view");
    ++kinds[static_cast<size_t>(b.kind)];
  }
  CHECK(kinds[static_cast<size_t>(doc::TrackKind::Midi)] == 3);
  CHECK(kinds[static_cast<size_t>(doc::TrackKind::Group)] == 1);
  CHECK(kinds[static_cast<size_t>(doc::TrackKind::Return)] == 1);
  CHECK(kinds[static_cast<size_t>(doc::TrackKind::Master)] == 1);
  REQUIRE(pp.master != kNone);
  const Node& out = pp.nodes[pp.bands[pp.master].output];
  CHECK((out.x >= w0.x && out.x + out.w <= w1.x));
  // The opened band stays open while zooming out (the focus too): the circuit is one continuous view.
  CHECK(v.openedBand() == u.track("Keys").id);
}

TEST_CASE("lane windows: what each window shows is its lane's clips over the shown time window (4 bars by default)") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  const doc::Project& p = *u.ctx.doc();
  const Ticks bar = ticksPerBar(p.timeline->sigMap.signatureAt(0));
  CHECK(v.laneStart() == 0);
  CHECK(v.laneSpan() == ProjectPatchView::kLaneBars * bar);
  CHECK(v.laneWidth() == metrics::kSourceW);
  // Every lane window lines up in one column (whatever the group depth) and is as wide as the view says.
  float x = -1.0f;
  for (const Band& b : v.patch().bands) {
    if (!hasLaneWindow(b)) continue;
    const Node& n = v.patch().nodes[b.source];
    if (x < 0.0f) x = n.x;
    CHECK(n.x == x);
    CHECK(n.w == metrics::kSourceW);
  }
  auto check = [&] {
    const doc::Project& q = *u.ctx.doc();
    const Ticks a = v.laneStart(), e = v.laneStart() + v.laneSpan();
    for (uint32_t bi = 0; bi < v.patch().bands.size(); ++bi) {
      const Band& b = v.patch().bands[bi];
      if (!hasLaneWindow(b)) continue;
      const doc::Track* t = doc::findTrackPtr(q, b.track);
      REQUIRE(t != nullptr);
      const LaneRect r = laneBody(v.patch().nodes[b.source]);
      const double k = static_cast<double>(r.x1 - r.x0) / static_cast<double>(v.laneSpan());
      std::vector<ProjectPatchView::LaneClip> want;
      for (const doc::ClipSlot& c : t->clips)
        if (c.end() > a && c.start() < e)
          want.push_back({c.id, r.x0 + static_cast<float>(static_cast<double>(std::max(c.start(), a) - a) * k),
                          r.x0 + static_cast<float>(static_cast<double>(std::min(c.end(), e) - a) * k), c.isAudio()});
      const std::vector<ProjectPatchView::LaneClip> got = v.laneClips(bi);
      REQUIRE(got.size() == want.size());
      for (size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i].clip == want[i].clip);
        CHECK(got[i].x0 == doctest::Approx(want[i].x0));
        CHECK(got[i].x1 == doctest::Approx(want[i].x1));
        CHECK(got[i].x0 >= r.x0);
        CHECK(got[i].x1 <= r.x1 + 0.01f);
      }
    }
  };
  check();
  // Keys' Riff (bars 1–4) fills its window; Riff 2 (from bar 7) is not in it; Pad starts at bar 3.
  const uint32_t keys = v.patch().bandOf(u.track("Keys").id), pad = v.patch().bandOf(u.track("Pad").id);
  CHECK(v.laneClips(keys).size() == 1);
  CHECK(v.laneClips(pad).size() == 1);
  // Wheel over a window scrolls time (an eighth of the span per notch); no re-layout, the windows repaint.
  const uint64_t relayouts = v.stats().relayouts;
  const Node& kn = v.patch().nodes[v.patch().bands[keys].source];
  u.zoomTo("Keys");
  const Vec2 over = u.dev(kn.x + kn.w * 0.5f, kn.y + kn.h * 0.5f);
  REQUIRE(v.laneAt(over) == keys);
  const circuit::CircuitCanvas::View before = v.canvas().view();
  (void)u.d.wheel(over, -8.0f);  // eight notches towards the end: one span later
  CHECK(v.laneStart() == v.laneSpan());
  CHECK(v.canvas().view() == before);  // the canvas did not pan
  CHECK(v.stats().relayouts == relayouts);
  check();
  CHECK(v.laneClips(keys).size() == 1);  // Riff 2 (bars 7–10) now
  CHECK(v.laneClips(keys)[0].clip == u.track("Keys").clips[1].id);
  // Ctrl+wheel out: a longer span, a wider window, the bands re-laid out (the chain moved right), still the lanes.
  const float devX = v.patch().nodes[u.deviceNode(u.track("Keys").devices[0]->id)].x;
  (void)u.d.wheel(u.dev(kn.x + 8.0f, kn.y + kn.h * 0.5f), -3.0f, 0.0f, mod::kCtrl);
  CHECK(v.laneSpan() > ProjectPatchView::kLaneBars * bar);
  CHECK(v.laneWidth() > metrics::kSourceW);
  CHECK(v.stats().relayouts == relayouts + 1);
  CHECK(v.patch().nodes[v.patch().bands[keys].source].w == v.laneWidth());
  CHECK(v.patch().nodes[u.deviceNode(u.track("Keys").devices[0]->id)].x > devX);
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");
  check();
  // Far out: clamped to the widest window, the span keeps growing to its limit.
  v.zoomLanes(-60.0);
  CHECK(v.laneWidth() == metrics::kSourceMaxW);
  CHECK(v.laneSpan() == ProjectPatchView::kLaneMaxBars * bar);
  check();
  // Zoomed in below the default: the default width, a span down to one beat.
  v.zoomLanes(60.0);
  CHECK(v.laneWidth() == metrics::kSourceW);
  CHECK(v.laneSpan() == ticksPerBeat(p.timeline->sigMap.signatureAt(v.laneStart())));
  check();
}

TEST_CASE("lane windows: shown again, they start at the arrangement's visible bar (or the playhead's while playing)") {
  Ui u;
  const Ticks bar = ticksPerBar(u.ctx.doc()->timeline->sigMap.signatureAt(0));
  u.arrangement().setTimeView(static_cast<double>(5 * bar + bar / 3), u.arrangement().pxPerTick());
  u.show();
  CHECK(u.view().laneStart() == 5 * bar);
  u.view().setShown(false, false);
  u.port.play(9 * bar + 7);
  u.show();
  CHECK(u.view().laneStart() == 9 * bar);
}

TEST_CASE("lane windows: idle costs nothing; the playhead repaints only the windows' columns and pages them") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  u.zoomTo("Pad");
  u.d.advance(0.5);
  const uint64_t frames0 = u.d.framesDrawn();
  u.d.advance(5.0);  // stopped: no frames, no timers
  CHECK(u.d.framesDrawn() == frames0);
  CHECK(std::isinf(u.d.tree().nextWakeTime()));
  const Ticks bar = ticksPerBar(u.ctx.doc()->timeline->sigMap.signatureAt(0));
  u.ctx.setStartMarker(bar);
  u.ctx.togglePlay();  // plays from the start marker through the fake port, notifies kTransport
  u.d.advance(0.2);
  REQUIRE(v.lanePlayhead().has_value());
  u.d.step();
  // A move: the damage is the old and new playhead columns of the windows (and the control bar's readout).
  u.port.setPlayheadTick(*v.lanePlayhead() + bar / 2);
  u.d.tree().update(u.d.now() + 0.15);
  const IRect bar0 = u.d.rect("main/control-bar");
  std::vector<Rect> windows;
  for (const Band& b : v.patch().bands)
    if (hasLaneWindow(b)) {
      const Node& n = v.patch().nodes[b.source];
      windows.push_back(v.canvas().deviceRect(n.x, n.y, n.w, n.h));
    }
  size_t columns = 0;
  for (const ClipRect& r : u.d.tree().damage().rects()) {
    if (r.y1 <= bar0.bottom()) continue;  // the control bar's position readout
    bool inWindow = false;
    for (const Rect& w : windows)
      inWindow = inWindow || (static_cast<float>(r.x0) >= std::floor(w.x0) - 1.0f && static_cast<float>(r.x1) <= std::ceil(w.x1) + 1.0f &&
                              static_cast<float>(r.y0) >= std::floor(w.y0) - 1.0f && static_cast<float>(r.y1) <= std::ceil(w.y1) + 1.0f);
    CHECK_MESSAGE(inWindow, r.x0 << "," << r.y0 << " to " << r.x1 << "," << r.y1);
    CHECK(r.x1 - r.x0 <= 8);
    ++columns;
  }
  CHECK(columns > 0);
  u.d.step();
  // Running out of the window pages it on (follow).
  u.port.setPlayheadTick(v.laneStart() + v.laneSpan() + bar);
  u.d.advance(0.3);
  CHECK(v.laneStart() == v.laneSpan());
  // Stopped: no playhead, idle again.
  u.ctx.togglePlay();
  u.d.advance(0.5);
  CHECK_FALSE(v.lanePlayhead().has_value());
  const uint64_t frames1 = u.d.framesDrawn(), reads = v.stats().playheadReads;
  u.d.advance(5.0);
  CHECK(u.d.framesDrawn() == frames1);
  CHECK(v.stats().playheadReads == reads);
}

TEST_CASE("lane windows: a double-click on a MIDI clip opens its piano roll; on an audio clip its Clip View") {
  Ui u;
  // An audio track with an audio clip next to the sketch's MIDI tracks.
  doc::DocumentStore& st = u.ctx.store();
  auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, u.track("Mod").id, "Loop");
  REQUIRE(t.ok());
  REQUIRE(st.dispatch(t->command).ok());
  const doc::TrackId loop = t->tracks.at(0);
  doc::SampleRef sr;
  sr.id = st.ids().sample();
  sr.path = "loop.wav";  // missing on disk: the clip draws its body (no waveform) and still opens
  sr.rate = 48000.0;
  sr.frames = 4 * 48000;
  sr.channels = 2;
  const doc::ops::NewAudioClip nc{loop, 0, sr.id, {}};
  auto a = doc::ops::addAudioClips(*st.current(), st.ids(), std::span<const doc::SampleRef>(&sr, 1), std::span<const doc::ops::NewAudioClip>(&nc, 1));
  REQUIRE_MESSAGE(a.ok(), (a.ok() ? "" : a.error().message));
  REQUIRE(st.dispatch(a->command).ok());
  u.show();
  ProjectPatchView& v = u.view();
  u.zoomTo("Keys");
  // MIDI: Keys' Riff.
  const uint32_t keys = v.patch().bandOf(u.track("Keys").id);
  REQUIRE(v.laneClips(keys).size() == 1);
  const ProjectPatchView::LaneClip riff = v.laneClips(keys)[0];
  const Node& kn = v.patch().nodes[v.patch().bands[keys].source];
  const LaneRect kr = laneBody(kn);
  u.d.doubleClick(u.dev((riff.x0 + riff.x1) * 0.5f, (kr.y0 + kr.y1) * 0.5f));
  u.d.step();
  CHECK(u.ctx.detailClip().clip == riff.clip);
  CHECK(u.ctx.detailClip().track == u.track("Keys").id);
  CHECK(u.ctx.detailVisible());
  CHECK(u.ctx.detailMode() == DetailMode::Clip);
  CHECK(u.ctx.store().history().cursor() == u.history());  // display only: nothing changed
  // Audio: the Loop's clip.
  u.zoomTo("Loop");
  const uint32_t lb = v.patch().bandOf(loop);
  REQUIRE(v.laneClips(lb).size() == 1);
  CHECK(v.laneClips(lb)[0].audio);
  const ProjectPatchView::LaneClip ac = v.laneClips(lb)[0];
  const LaneRect lr = laneBody(v.patch().nodes[v.patch().bands[lb].source]);
  u.d.doubleClick(u.dev((ac.x0 + ac.x1) * 0.5f, (lr.y0 + lr.y1) * 0.5f));
  u.d.step();
  CHECK(u.ctx.detailClip().clip == ac.clip);
  CHECK(u.ctx.detailClip().track == loop);
  CHECK(u.ctx.detailMode() == DetailMode::Clip);
  // Outside any clip (Keys' window past bar 4 when scrolled): nothing opens, nothing changes.
  const doc::ClipRef before = u.ctx.detailClip();
  u.zoomTo("Pad");
  const uint32_t pad = v.patch().bandOf(u.track("Pad").id);
  const LaneRect pr = laneBody(v.patch().nodes[v.patch().bands[pad].source]);
  u.d.doubleClick(u.dev(pr.x0 + 2.0f, (pr.y0 + pr.y1) * 0.5f));  // bar 1: Pad starts at bar 3
  u.d.step();
  CHECK(u.ctx.detailClip().clip == before.clip);
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


TEST_CASE("lane windows: 64 tracks — a time-zoom re-layout ≤ 2 ms, a full paint of the windows" * doctest::test_suite("perf")) {
  Ui u(false);
  REQUIRE(buildWideProject(u.ctx.store(), 64).ok());
  REQUIRE(u.d.enableGpu().ok());
  u.show();
  ProjectPatchView& v = u.view();
  v.fitAll(false);
  u.d.advance(1.0);
  std::vector<double> ms;
  for (int i = 0; i < 20; ++i) {
    v.zoomLanes(i % 2 == 0 ? -4.0 : 4.0);  // 4 bars ↔ ~10 bars: the width changes every time
    ms.push_back(v.stats().lastRelayoutMs);
  }
  std::sort(ms.begin(), ms.end());
  MESSAGE("64 tracks: lane re-layout median " << ms[ms.size() / 2] << " ms, max " << ms.back() << " ms (" << v.stats().relayouts << " re-layouts)");
  CHECK(v.stats().relayouts >= 20);
  CHECK(ms[ms.size() / 2] <= 2.0);
  // Zoomed onto the windows (Mid / Near): repaint them all.
  u.zoomTo("Track 1");
  u.d.advance(0.5);
  std::vector<double> pm;
  for (int i = 0; i < 20; ++i) {
    v.canvas().markDirty();
    const auto t0 = std::chrono::steady_clock::now();
    u.d.step();
    pm.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(pm.begin(), pm.end());
  MESSAGE("64 tracks, a band framed: median full frame " << pm[pm.size() / 2] << " ms");
}

// ---- TX.panel-faces: closed stock devices drawn as their Device-view panels --------------------------------------

namespace {

// The device px centre of panel control `id` (DevicePanel::control: its binding's name by default) on `dev`'s block.
Vec2 controlAt(Ui& u, doc::DeviceId dev, std::string_view id) {
  devices::PanelFace* f = u.view().panelOf(dev);
  REQUIRE(f != nullptr);
  REQUIRE(f->layoutScale() > 0.0f);
  Widget* w = f->panel().control(id);
  REQUIRE_MESSAGE(w != nullptr, "no control " << std::string(id));
  REQUIRE(w->visible());
  const IRect r = w->rect();
  const Node& n = u.pp().nodes[u.deviceNode(dev)];
  const float k = n.scale / f->layoutScale();  // TX.resize: panel px → its own units → the (scaled) block's world units
  return u.dev(n.x + (static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f) * k,
               n.y + (static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f) * k);
}

double paramOf(const Ui& u, doc::DeviceId dev, std::string_view param) {
  const doc::Device* d = doc::findDevice(*u.ctx.doc(), dev).device;
  REQUIRE(d != nullptr);
  const doc::DeviceInfo* info = library().catalog().find(d->patch);
  REQUIRE(info != nullptr);
  const doc::DeviceParamInfo* p = info->param(param);
  REQUIRE(p != nullptr);
  return doc::deviceParamValue(*d, *p);
}

}  // namespace

TEST_CASE("project patch (TX.panel-faces): every closed stock device is drawn as its Device-view panel; opened, its atoms") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());  // a theme: the canvas paints (the panels lay out at their first paint)
  // Every catalog device on a track of its own, beside the sketch (instruments on MIDI tracks of their own).
  doc::DocumentStore& st = u.ctx.store();
  std::vector<std::pair<doc::DeviceId, std::string>> added;
  for (const doc::DeviceInfo& info : library().catalog().devices()) {
    auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "All " + info.name);
    REQUIRE(t.ok());
    REQUIRE(st.dispatch(t->command).ok());
    auto e = doc::ops::addDevice(*st.current(), st.ids(), t->tracks.at(0), info.patch, info.kind);
    REQUIRE_MESSAGE(e.ok(), info.patch);
    REQUIRE(st.dispatch(e->command).ok());
    added.emplace_back(e->devices.at(0), info.name);
  }
  u.show();
  u.d.advance(0.5);
  ProjectPatchView& v = u.view();
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");  // the same graph, drawn bigger
  size_t panels = 0;
  for (const Node& n : v.patch().nodes) {
    if (n.kind != NodeKind::Device) continue;
    CAPTURE(n.title);
    const bool stock = !n.rack && !n.meta && !n.edited && !n.missing && !n.expanded;
    CHECK(n.panel == stock);
    if (!stock) continue;
    ++panels;
    devices::PanelFace* f = v.panelOf(n.device);
    REQUIRE(f != nullptr);
    // The Device view's panel: its description (title, size, controls) at the block's size, its exports as pins.
    const doc::Device* d = doc::findDevice(*u.ctx.doc(), n.device).device;
    REQUIRE(d != nullptr);
    const doc::DeviceInfo* info = library().catalog().find(d->patch);
    REQUIRE(info != nullptr);
    CHECK(f->panel().title() == (d->name.empty() ? f->panel().desc().title : d->name));
    const Vec2 s = devices::PanelFace::sizeFor(f->panel().desc());
    CHECK(n.w >= s.x);
    CHECK(n.w < s.x + 4.0f);
    CHECK(n.h >= s.y);
    CHECK(f->worldSize().x == n.w);
    CHECK(f->panel().signature().exports.empty());
    CHECK_FALSE(f->panel().foldable());
  }
  CHECK(panels == v.canvas().panelCount());
  for (const auto& [id, name] : added) {
    CAPTURE(name);
    CHECK(v.panelOf(id) != nullptr);
  }
  CHECK(v.canvas().stats().panelsPainted > 0);
  // Opened to its atoms: the atoms, no panel; closed again: the panel (the same face object is not required).
  const doc::DeviceId sat = u.track("Keys").devices[1]->id;
  REQUIRE(v.panelOf(sat) != nullptr);
  v.setDeviceExpanded(sat, true);
  u.d.advance(0.5);
  CHECK(v.panelOf(sat) == nullptr);
  CHECK_FALSE(v.patch().nodes[v.patch().nodeOf(sat)].panel);
  CHECK(v.scene().sceneNode[v.patch().nodeOf(sat)] == kNone);
  v.setDeviceExpanded(sat, false);
  u.d.advance(0.5);
  CHECK(v.panelOf(sat) != nullptr);
  // A user device (its own patch text) keeps the plain block: its circuit is what it is.
  const doc::DeviceId op = u.track("Keys").devices[0]->id;
  {
    const doc::Device* d = doc::findDevice(*u.ctx.doc(), op).device;
    const std::string text = library().stockText(d->patch);
    REQUIRE_FALSE(text.empty());
    auto e = doc::ops::editDevicePatch(*u.ctx.doc(), op, text + "\n// edited\n");
    REQUIRE(e.ok());
    REQUIRE(st.dispatch(e->command).ok());
  }
  u.d.advance(0.5);
  CHECK(v.panelOf(op) == nullptr);
  CHECK(v.patch().nodes[v.patch().nodeOf(op)].edited);
  // Panels off (the A/B): plain blocks again, the same graph.
  v.setPanelsShown(false);
  u.d.advance(0.5);
  CHECK(v.canvas().panelCount() == 0);
  CHECK(v.panelOf(sat) == nullptr);
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");
}

TEST_CASE("project patch (TX.panel-faces): a panel's controls edit the document in place — one undo step per gesture") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());  // a theme: the canvas paints (the panels lay out at their first paint)
  u.show();
  const doc::Track& group = u.track("Group");
  REQUIRE(group.devices.size() == 2);
  const doc::DeviceId util = group.devices[1]->id;
  REQUIRE(group.devices[1]->patch == "utility");
  ProjectPatchView& v = u.view();
  {
    // The Utility at zoom 1.2 (the Group's container, its members included, frames far out in this window).
    const Node& n = u.pp().nodes[u.deviceNode(util)];
    circuit::CircuitCanvas::View cv;
    cv.zoom = 1.2;
    cv.x = n.x - 120.0;
    cv.y = n.y - 80.0;
    v.canvas().setView(cv);
  }
  u.d.advance(0.5);  // settled: the panels laid out at the canvas's scale
  devices::PanelFace* f = v.panelOf(util);
  REQUIRE(f != nullptr);
  CHECK(f->layoutScale() == doctest::Approx(v.canvas().deviceScale()));
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();

  SUBCASE("a knob drag: one step; undo; the panel follows the document") {
    const double g0 = paramOf(u, util, "gain");
    const Vec2 k = controlAt(u, util, "gain");
    u.d.move(k);
    CHECK(v.canvas().cursorAt(k) == Cursor::ResizeNS);
    u.d.drag(k, {k.x, k.y - 40.0f}, 8);
    CHECK(u.history() == h0 + 1);
    const double g1 = paramOf(u, util, "gain");
    CHECK(g1 > g0);
    CHECK(f->panel().paramValue("gain") == doctest::Approx(g1));
    REQUIRE(u.ctx.undo());
    u.d.step();
    CHECK(u.ctx.doc() == start);
    CHECK(f->panel().paramValue("gain") == doctest::Approx(g0));
    // Esc during a drag: the value returns, nothing is kept.
    u.d.down(k);
    u.d.move({k.x, k.y - 20.0f});
    u.d.move({k.x, k.y - 30.0f});
    CHECK(paramOf(u, util, "gain") > g0);
    CHECK(u.d.press("Escape") != KeyResult::Unhandled);
    u.d.up({k.x, k.y - 30.0f});
    CHECK(paramOf(u, util, "gain") == doctest::Approx(g0));
  }
  SUBCASE("a toggle, a chooser (its menu in the shell's overlay, under the control), the activator: one step each") {
    u.d.click(controlAt(u, util, "mute"));
    CHECK(u.history() == h0 + 1);
    CHECK(paramOf(u, util, "mute") == 1.0);
    const Vec2 c = controlAt(u, util, "chan");
    u.d.click(c);
    Menu* m = u.openMenu();
    REQUIRE(m != nullptr);
    CHECK(std::fabs(static_cast<float>(m->rect().x) - (c.x - static_cast<float>(f->panel().control("chan")->rect().w) * 0.5f /
                                                                   f->layoutScale() * v.canvas().deviceScale())) < 4.0f);
    CHECK(static_cast<float>(m->rect().y) > c.y);
    u.choose(*m, "Left");
    CHECK(u.history() == h0 + 2);
    CHECK(paramOf(u, util, "chan") == 1.0);
    CHECK(f->panel().paramValue("chan") == 1.0);
    const Node& n = v.patch().nodes[u.deviceNode(util)];
    u.d.click(u.dev(n.x + 10.0f, n.y + 10.0f));  // the title bar's activator
    CHECK(u.history() == h0 + 3);
    CHECK_FALSE(doc::findDevice(*u.ctx.doc(), util).device->active);
    for (int i = 0; i < 3; ++i) REQUIRE(u.ctx.undo());
    CHECK(u.ctx.doc() == start);
  }
  SUBCASE("the title bar and the body are the block's: select, move; a double-click opens the atoms") {
    const uint32_t node = u.deviceNode(util);
    u.d.click(u.grip(node));
    CHECK(v.selectedDevices() == std::vector<doc::DeviceId>{util});
    CHECK(u.history() == h0);
    CHECK(v.canvas().cursorAt(u.grip(node)) == Cursor::Move);
    u.d.doubleClick(u.grip(node));
    u.d.advance(0.5);
    CHECK(v.deviceExpanded(util));
    CHECK(v.panelOf(util) == nullptr);
  }
  SUBCASE("a value from elsewhere (the Device view, automation, undo) shows on the panel; a structural setting too") {
    auto e = doc::ops::setDeviceParam(*u.ctx.doc(), util, "width", 2.0);
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    u.d.step();
    CHECK(f->panel().paramValue("width") == doctest::Approx(2.0));
    auto s = doc::ops::setDeviceSetting(*u.ctx.doc(), util, "bassMono", 1.0);
    REQUIRE(s.ok());
    REQUIRE(u.ctx.store().dispatch(s->command).ok());
    u.d.advance(0.2);
    devices::PanelFace* g = v.panelOf(util);  // a structural edit rebuilt the scene: the panel is kept
    CHECK(g == f);
    CHECK(f->panel().constValue("bassMono") == 1.0);
    CHECK(f->panel().control("bassFreq")->visible());  // its conditional control shown
  }
  SUBCASE("a number box takes typed values while it has the keys") {
    // The Utility has no box: Saturator's Drive knob takes a typed value (T4.17's type-in) after a press on it.
    u.zoomTo("Keys");
    u.d.advance(0.5);
    const doc::DeviceId sat = u.track("Keys").devices[1]->id;
    const Vec2 k = controlAt(u, sat, "drive");
    u.d.click(k);
    const size_t h1 = u.history();
    CHECK(u.d.press("1") != KeyResult::Unhandled);
    CHECK(v.canvas().acceptsTextInput());
    CHECK(u.d.type("2"));
    CHECK(u.d.press("Return") != KeyResult::Unhandled);
    CHECK(u.history() == h1 + 1);
    CHECK_FALSE(v.canvas().acceptsTextInput());
  }
}

TEST_CASE("project patch (TX.panel-faces): a wire dropped on a panel's knob drives it; far out the panels are tiles and still work") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());  // a theme: the canvas paints (the panels lay out at their first paint)
  u.show();
  const doc::DeviceId lfo = u.track("Mod").devices[0]->id;
  const doc::DeviceId util = u.track("Group").devices[1]->id;
  ProjectPatchView& v = u.view();
  // Both on screen: the whole project, readable enough to aim (the overview is far out).
  v.fitAll(false);
  u.d.advance(0.5);
  CHECK(v.canvas().lod() == circuit::CircuitCanvas::Lod::Far);
  CHECK(v.canvas().deviceScale() < devices::PanelFace::kMinScale);
  // Far out the panels are drawn from their colour-layer tiles (static layers), still with their sockets.
  CHECK(v.canvas().stats().panelsPainted > 0);
  CHECK(v.canvas().stats().faceLayersFromTiles > 0);
  const size_t h0 = u.history();
  u.d.drag(u.outPin(u.deviceNode(lfo), "lfo"), controlAt(u, util, "balance"), 6);
  CHECK(u.history() == h0 + 1);
  bool wired = false;
  for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc())) wired = wired || (w.from == lfo && w.to == util && w.param == "balance");
  CHECK(wired);
  CHECK(u.openMenu() == nullptr);  // straight onto the param, no menu
  // A knob turns far out as it does near (the drag reads device px).
  const double g0 = paramOf(u, util, "gain");
  const Vec2 k = controlAt(u, util, "gain");
  u.d.drag(k, {k.x, k.y - 30.0f}, 6);
  CHECK(u.history() == h0 + 2);
  CHECK(paramOf(u, util, "gain") > g0);
  // Idle again: no frames, no timers.
  u.d.advance(1.0);
  CHECK_FALSE(u.d.tree().needsFrame());
  CHECK(std::isinf(u.d.tree().nextWakeTime()));
}

// ---- goldens --------------------------------------------------------------------------------------------------------

namespace {
void golden(const char* name, const std::string& project = {}) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(name, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(name, project, w, h, {}, o, &library());
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
// T5.x-one-circuit: Open in Circuit View on a device, zoomed out around it (the opened band in context); the lane
// windows (MIDI notes, audio waveforms) over the audio fixture.
TEST_CASE("render: project-circuit-device matches the golden image") { golden("project-circuit-device"); }
TEST_CASE("render: project-patch-lanes matches the golden image") { golden("project-patch-lanes", views::test::fixtureDir("audio")); }
// TMM.10-fix2: wires from the note sources' outputs (per voice into a voice run's Wavetable, reduced into the Auto
// Filter, across tracks).
TEST_CASE("render: project-patch-notes matches the golden image") { golden("project-patch-notes"); }
// TX.patching: free devices under the Lead chain, a wire onto a face control, an output into an audio input.
TEST_CASE("render: project-patch-free matches the golden image") { golden("project-patch-free"); }
// TX.panel-faces: closed stock devices as their Device-view panels (the default synth's generic panel, an Auto Filter, a
// Utility with Bass Mono on), an LFO wired onto the Utility's gain.
TEST_CASE("render: project-patch-panels matches the golden image") { golden("project-patch-panels"); }
// TX.resize: a Wavetable face at 1.5×, an Auto Filter panel at 1.25×, a Wavetable at 0.6× below; note-velocity cables
// onto the scaled Wavetable's position and the scaled filter's frequency.
TEST_CASE("render: project-patch-resize matches the golden image") { golden("project-patch-resize"); }

// ---- TX.mod-all: a modulation wire ends on the control it modulates ------------------------------------------------

namespace {

struct NullPanelTarget final : devices::DeviceTarget {
  void setParam(std::string_view, double) override {}
  void setConst(std::string_view, double) override {}
  void setActive(bool) override {}
};

// A MIDI track "Lead WT": a Phase accumulator (the notes), a Wavetable, a free Phase accumulator (the modulator) wired
// into the Wavetable's warp (the user's case: the bottom-middle knob).
struct WarpSetup {
  doc::TrackId track;
  doc::DeviceId pa, wt, mod;
  doc::WireId wire;
};
WarpSetup warpSetup(Ui& u) {
  doc::DocumentStore& st = u.ctx.store();
  WarpSetup s;
  auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Lead WT");
  REQUIRE(t.ok());
  REQUIRE(st.dispatch(t->command).ok());
  s.track = t->tracks.at(0);
  auto add = [&](std::string_view type, uint32_t index) {
    auto e = doc::ops::addMetaDevice(*st.current(), st.ids(), s.track, type, index);
    REQUIRE(e.ok());
    REQUIRE(st.dispatch(e->command).ok());
    return e->devices.at(0);
  };
  s.pa = add("phase_acc", 0);
  s.wt = add("wavetable", doc::kAppend);
  s.mod = add("phase_acc", doc::kAppend);
  auto f = doc::ops::setDevicesFree(*st.current(), std::vector<doc::DeviceId>{s.mod}, true);
  REQUIRE(f.ok());
  REQUIRE(st.dispatch(f->command).ok());
  auto w = doc::ops::connectWire(*st.current(), st.ids(), s.mod, "out", s.wt, "warp");
  REQUIRE(w.ok());
  REQUIRE(st.dispatch(w->command).ok());
  s.wire = doc::wiresOf(*st.current()).back().id;
  return s;
}

// The scene wire of document wire `id` (kNone: none).
uint32_t sceneWireOf(const ProjectPatchView& v, doc::WireId id) {
  for (uint32_t w = 0; w < v.scene().modelEdge.size(); ++w) {
    const uint32_t e = v.scene().modelEdge[w];
    if (e != kNone && e < v.patch().edges.size() && v.patch().edges[e].kind == EdgeKind::Modulation && v.patch().edges[e].wire == id)
      return w;
  }
  return kNone;
}

}  // namespace

// The user (TASKS TX.mod-all): "improve the cables so that the cables actually touch on / go directly to the location of
// the knob or slider they are modulating itself instead of added made up input ports". A wire into a meta face's param
// has no pin: the model's port is anchored at the control (the face's own controlAnchor), the scene's pin is invisible,
// the scene wire stops on the block's edge nearest the control and names the param (the canvas draws on to the knob);
// hovering, selecting and deleting the wire still work, and a drop onto another knob wires that param.
TEST_CASE("project patch (TX.mod-all): a wire into the Wavetable's warp ends on the warp knob — no pin, hover, delete, drop") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());
  const WarpSetup s = warpSetup(u);
  u.show();
  u.zoomTo("Lead WT");
  u.d.advance(0.5);
  ProjectPatchView& v = u.view();
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");
  const uint32_t mn = u.deviceNode(s.wt);
  const Node& n = u.pp().nodes[mn];
  REQUIRE(n.face != nullptr);
  const int32_t k = n.input("warp");
  REQUIRE(k >= 0);
  const Port& port = n.inputs[static_cast<size_t>(k)];
  CHECK(port.param);
  CHECK(port.anchored);
  // The face's warp knob (the middle of the three along the bottom): the anchor the layout used and the face's now.
  const uint32_t sn = v.scene().sceneNode[mn];
  const MetaFace* face = v.canvas().faceOn(sn);
  REQUIRE(face != nullptr);
  const std::optional<ControlAnchor> knob = face->controlAnchor("warp");
  REQUIRE(knob.has_value());
  CHECK(knob->at.x == doctest::Approx(port.ax));
  CHECK(knob->at.y == doctest::Approx(port.ay));
  CHECK(knob->at.y > n.h * 0.75f);                           // bottom row
  CHECK(std::fabs(knob->at.x - n.w * 0.5f) < n.w * 0.1f);  // middle
  CHECK(knob->radius > 4.0f);
  // No pin: the scene's pin for it is invisible; the face's block is no taller than the face.
  const circuit::CircuitScene& sc = *v.scene().scene;
  CHECK_FALSE(sc.pins[sc.nodes[sn].firstPin + static_cast<uint32_t>(k)].visible);
  CHECK(n.h == doctest::Approx(std::ceil(faceSize(*n.face).y / 4.0f) * 4.0f));
  // The scene wire: ends on the block's bottom edge under the knob, names the param.
  const uint32_t w = sceneWireOf(v, s.wire);
  REQUIRE(w != kNone);
  const circuit::SceneWire& sw = sc.wires[w];
  REQUIRE(sw.control != UINT32_MAX);
  CHECK(sc.strings[sw.control] == "warp");
  const graph::Point2 end = sc.points[sw.firstPoint + sw.pointCount - 1];
  CHECK(end.x == doctest::Approx(n.x + knob->at.x));
  CHECK(end.y == doctest::Approx(n.y + n.h));
  CHECK(orthogonal(std::vector<graph::Point2>(sc.points.begin() + sw.firstPoint, sc.points.begin() + sw.firstPoint + sw.pointCount)));
  // Hover: the wire's part outside the face (the middle of its next-to-last segment) is the wire.
  REQUIRE(sw.pointCount >= 3);
  const graph::Point2 a = sc.points[sw.firstPoint + sw.pointCount - 3], b = sc.points[sw.firstPoint + sw.pointCount - 2];
  const Vec2 mid = u.dev((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
  u.d.move(mid);
  CHECK(v.canvas().hovered().kind == circuit::CanvasHit::Kind::Wire);
  CHECK(v.canvas().hovered().index == w);
  // Select it and Delete: the document wire goes (one undo step).
  const size_t h0 = u.history();
  u.d.click(mid);
  CHECK(v.canvas().wireSelected(w));
  CHECK(u.d.press("Delete") != KeyResult::Unhandled);
  CHECK(u.history() == h0 + 1);
  for (const doc::Wire& x : doc::wiresOf(*u.ctx.doc())) CHECK(x.id != s.wire);
  // A drop of the modulator's output onto the fold knob wires the fold (no menu), and that wire ends on the fold knob.
  u.d.advance(0.5);
  const uint32_t mn2 = u.deviceNode(s.wt);
  const Node& n2 = u.pp().nodes[mn2];
  const MetaFace* face2 = v.canvas().faceOn(v.scene().sceneNode[mn2]);
  REQUIRE(face2 != nullptr);
  const std::optional<ControlAnchor> fold = face2->controlAnchor("fold");
  REQUIRE(fold.has_value());
  u.d.drag(u.outPin(u.deviceNode(s.mod), "out"), u.dev(n2.x + fold->at.x, n2.y + fold->at.y), 6);
  CHECK(u.openMenu() == nullptr);
  CHECK(u.history() == h0 + 2);
  const doc::Wire& nw = doc::wiresOf(*u.ctx.doc()).back();
  CHECK(nw.from == s.mod);
  CHECK(nw.to == s.wt);
  CHECK(nw.param == "fold");
  const Node& n3 = u.pp().nodes[u.deviceNode(s.wt)];
  const int32_t kf = n3.input("fold");
  REQUIRE(kf >= 0);
  CHECK(n3.inputs[static_cast<size_t>(kf)].anchored);
  CHECK(n3.input("warp") < 0);  // the deleted wire's param has no port any more
}

// Which params keep a pin: those no control shows. Every param of every stock device's panel (laid out with its
// defaults: EQ Eight's first band selected) and every meta face's, asked for its control; the ones without one are
// listed (they keep a pin on the block's left edge when wired).
TEST_CASE("project patch (TX.mod-all): every face and panel param has a control to end on, except the listed ones") {
  const doc::DeviceLibrary& lib = library();
  std::vector<std::string> without;
  uint32_t asked = 0, shownNow = 0;
  for (const doc::DeviceInfo& info : lib.catalog().devices()) {
    const doc::DevicePtr d = doc::makeDevice(doc::DeviceId{1}, info.patch, info.kind);
    const std::unique_ptr<PanelTemplate> t = buildPanelTemplate(&lib, *d);
    REQUIRE(t != nullptr);
    devices::PanelFace f(t->sig, t->desc, std::make_unique<NullPanelTarget>(), {}, devices::PanelFace::sizeFor(t->desc));
    REQUIRE(f.prepare(1.0f));
    for (const doc::DeviceParamInfo& p : info.params) {
      ++asked;
      // Its place in the panel (whatever the tab, band or setting shows now); where it shows now, the laid-out control
      // is there.
      const std::optional<ControlAnchor> home = f.controlHome(p.name);
      if (!home) {
        without.push_back(info.name + " " + p.name);
        continue;
      }
      CHECK(home->at.x >= 0.0f);
      CHECK(home->at.y >= 0.0f);
      CHECK(home->at.x <= f.worldSize().x);
      CHECK(home->at.y <= f.worldSize().y);
      if (const std::optional<ControlAnchor> now = f.controlAnchor(p.name)) {
        ++shownNow;
        // Operator's per-oscillator LFO / pitch-envelope switches have two controls (the oscillator's tab, shown by
        // default, and the LFO / pitch section, the home): the view routes to the one shown when it lays out.
        const bool twoControls = info.patch == "operator" && (p.name.starts_with("lfoTo") || p.name.starts_with("penvTo"));
        CHECK_MESSAGE((twoControls || (std::fabs(now->at.x - home->at.x) <= 2.0f && std::fabs(now->at.y - home->at.y) <= 2.0f)),
                      info.name << " " << p.name << ": shown at " << now->at.x << "," << now->at.y << ", placed at "
                                << home->at.x << "," << home->at.y);
      }
    }
  }
  for (const meta::Type* type : {&meta::phaseAccType(), &meta::wavetableType(), &meta::envelopeType(), &meta::vcaType()}) {
    std::unique_ptr<MetaFace> f = makeFace(*type);
    REQUIRE(f != nullptr);
    f->bind(std::make_unique<DefaultTarget>(*type), nullptr);
    for (const meta::ParamSpec& p : type->params) {
      ++asked;
      const std::optional<ControlAnchor> a = f->controlAnchor(p.name);
      if (!a) {
        without.push_back(std::string(type->name) + " " + std::string(p.name));
        continue;
      }
      CHECK(a->at.x >= 0.0f);
      CHECK(a->at.y >= 0.0f);
      CHECK(a->at.x <= f->worldSize().x);
      CHECK(a->at.y <= f->worldSize().y);
    }
  }
  std::string list;
  for (const std::string& s : without) list += "\n  " + s;
  MESSAGE(asked << " params asked (" << shownNow << " of the panels' shown in their default state); " << without.size()
                << " have no control (a pin when wired):" << list);
  CHECK(asked > 300);
  CHECK(shownNow > 50);
}

// TX.mod-all's cost in the project patch frame: 16 tracks, each an LFO wired onto a Utility's gain knob and a Basic
// Synth's brightness (32 wires ending on panel knobs), framed at the Mid level where all of them are drawn: the median
// full frame with the legs over the panels, and with the same scene whose wires stop on the panels' edges (no legs).
TEST_CASE("project patch (TX.mod-all): 32 wires onto knobs — a full frame with their legs vs without" * doctest::test_suite("perf")) {
  Ui u(false);
  doc::DocumentStore& st = u.ctx.store();
  for (int i = 0; i < 16; ++i) {
    auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "T" + std::to_string(i));
    REQUIRE(t.ok());
    REQUIRE(st.dispatch(t->command).ok());
    const doc::TrackId tr = t->tracks.at(0);
    const doc::DeviceId synth = named(*st.current(), "T" + std::to_string(i))->devices.front()->id;
    auto lfo = doc::ops::addDevice(*st.current(), st.ids(), tr, "lfo_device", doc::DeviceKind::AudioEffect);
    REQUIRE(lfo.ok());
    REQUIRE(st.dispatch(lfo->command).ok());
    auto util = doc::ops::addDevice(*st.current(), st.ids(), tr, "utility", doc::DeviceKind::AudioEffect);
    REQUIRE(util.ok());
    REQUIRE(st.dispatch(util->command).ok());
    for (const auto& [to, param] : {std::pair{util->devices.at(0), "gain"}, std::pair{synth, "bright"}}) {
      auto w = doc::ops::connectWire(*st.current(), st.ids(), lfo->devices.at(0), "lfo", to, param);
      REQUIRE(w.ok());
      REQUIRE(st.dispatch(w->command).ok());
    }
  }
  REQUIRE(u.d.enableGpu().ok());
  u.show();
  ProjectPatchView& v = u.view();
  v.fitAll(false);
  u.d.advance(1.0);
  circuit::CircuitCanvas::View cv;  // the project's top-left part at 0.5
  cv.x = v.patch().x0 - 20.0;
  cv.y = v.patch().y0 - 20.0;
  cv.zoom = 0.5;
  v.canvas().setView(cv);
  u.d.advance(1.0);
  REQUIRE(v.canvas().lod() == circuit::CircuitCanvas::Lod::Mid);
  uint32_t legs = 0;
  for (const circuit::SceneWire& w : v.scene().scene->wires) legs += w.control != UINT32_MAX ? 1 : 0;
  CHECK(legs == 32);
  auto median = [&] {
    std::vector<double> ms;
    for (int i = 0; i < 60; ++i) {
      v.canvas().markDirty();
      const auto t0 = std::chrono::steady_clock::now();
      u.d.step();
      ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
  };
  const double with = median();
  auto plain = std::make_shared<circuit::CircuitScene>(*v.scene().scene);
  for (circuit::SceneWire& w : plain->wires) w.control = UINT32_MAX;
  v.canvas().setScene(plain, false, true);
  v.canvas().setView(cv);
  u.d.advance(1.0);
  const double without = median();
  MESSAGE("32 wires onto knobs, Mid level: median full frame " << with << " ms with the legs, " << without << " ms without ("
          << v.canvas().stats().panelsPainted << " panels, " << v.canvas().stats().wiresPainted << " wires painted)");
  CHECK(v.canvas().stats().panelsPainted >= 16);
  CHECK(with < without * 1.25 + 0.05);
}

// ---- TX.resize: S-drag scales a block ---------------------------------------------------------------------------------

namespace {

// warpSetup's track plus an Auto Filter after its Wavetable whose frequency the free modulator drives (a cable onto a
// panel's knob).
struct ResizeSetup {
  WarpSetup w;
  doc::DeviceId filter;
  doc::WireId filterWire;
};
ResizeSetup resizeSetup(Ui& u) {
  ResizeSetup r;
  r.w = warpSetup(u);
  doc::DocumentStore& st = u.ctx.store();
  auto e = doc::ops::addDevice(*st.current(), st.ids(), r.w.track, "auto_filter", doc::DeviceKind::AudioEffect);
  REQUIRE(e.ok());
  REQUIRE(st.dispatch(e->command).ok());
  r.filter = e->devices.at(0);
  auto w = doc::ops::connectWire(*st.current(), st.ids(), r.w.mod, "out", r.filter, "freq");
  REQUIRE(w.ok());
  REQUIRE(st.dispatch(w->command).ok());
  r.filterWire = doc::wiresOf(*st.current()).back().id;
  return r;
}

// An S-drag on `dev`'s block from its centre by (dx, dy) device px: S pressed before, released after.
void sDrag(Ui& u, doc::DeviceId dev, float dx, float dy, Mods m = mod::kNone) {
  const Vec2 c = u.centre(u.deviceNode(dev));
  u.d.keyDown('s');
  u.d.move(c);
  u.d.down(c, PointerButton::Left, m);
  for (int i = 1; i <= 4; ++i) u.d.move({c.x + dx * static_cast<float>(i) / 4.0f, c.y + dy * static_cast<float>(i) / 4.0f}, m);
  u.d.up({c.x + dx, c.y + dy}, PointerButton::Left, m);
  u.d.keyUp('s');
}

}  // namespace

// SPEC §15 2026-11-01 "resize, groups, and the free-device rack" (the user: "holding down s while hovering over a module
// should let you resize it"): S over a block shows ⤡; a drag scales it uniformly, its top-left kept, its bottom-right
// corner on the pointer along the diagonal; 0.5× … 3× with a detent at 1×, Shift in steps of 25 %; Esc cancels; the
// layout makes room; S + double-click resets. View state: no undo step, but the project is dirty.
TEST_CASE("project patch (TX.resize): S + drag scales a block uniformly — clamps, Shift steps, Esc, reset, room made") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());
  const ResizeSetup r = resizeSetup(u);
  u.show();
  u.zoomTo("Lead WT");
  u.d.advance(0.5);
  ProjectPatchView& v = u.view();
  ProjectCanvas& cv = v.canvas();
  const doc::DeviceId wt = r.w.wt;
  const Node n0 = u.pp().nodes[u.deviceNode(wt)];
  REQUIRE(n0.face != nullptr);
  CHECK(n0.scale == 1.0f);
  const Node f0 = u.pp().nodes[u.deviceNode(r.filter)];
  const float bandH0 = u.pp().bands[n0.band].h;
  const size_t h0 = u.history();
  const float z = cv.deviceScale();
  // Without S: what a press there does (the face's cursor or Move); with S held: ⤡ over any closed block, not elsewhere.
  const Vec2 c = u.centre(u.deviceNode(wt));
  u.d.move(c);
  CHECK(u.d.tree().cursor() != Cursor::ResizeNWSE);
  u.d.keyDown('s');
  CHECK(u.d.tree().keyHeld('s'));
  CHECK(cv.resizeHeld());
  u.d.move(c);
  CHECK(u.d.tree().cursor() == Cursor::ResizeNWSE);
  CHECK(std::string(cv.info().title) == "Resize block");
  u.d.move(u.centre(u.deviceNode(r.filter)));
  CHECK(u.d.tree().cursor() == Cursor::ResizeNWSE);  // a panel too
  u.d.move(u.dev(n0.x - 30.0f, n0.y - 30.0f));
  CHECK(u.d.tree().cursor() != Cursor::ResizeNWSE);  // the band, not a block
  u.d.keyUp('s');
  CHECK_FALSE(cv.resizeHeld());
  // Half the block's diagonal outwards: 1.5×.
  sDrag(u, wt, n0.w * z * 0.5f, n0.h * z * 0.5f);
  CHECK(v.blockScale(wt) == doctest::Approx(1.5f));
  CHECK(u.history() == h0);  // view state: no undo step …
  CHECK(u.ctx.dirty());      // … but saved
  {
    const Node& n = u.pp().nodes[u.deviceNode(wt)];
    CHECK(n.scale == doctest::Approx(1.5f));
    CHECK(n.w == doctest::Approx(n0.w * 1.5f));
    CHECK(n.h == doctest::Approx(n0.h * 1.5f));
    CHECK(n.x == doctest::Approx(n0.x));  // its top-left kept
    CHECK(n.y == doctest::Approx(n0.y));
    // Its pins' rows and its anchors in proportion; the chain's spine through its main pin (straight chain wires).
    CHECK(pinOffset(n, 0) == doctest::Approx(pinOffset(n0, 0) * 1.5f));
    const int32_t k = n.input("warp"), k0 = n0.input("warp");
    REQUIRE(k >= 0);
    REQUIRE(k0 >= 0);
    CHECK(n.inputs[static_cast<size_t>(k)].ax == doctest::Approx(n0.inputs[static_cast<size_t>(k0)].ax * 1.5f));
    CHECK(n.inputs[static_cast<size_t>(k)].ay == doctest::Approx(n0.inputs[static_cast<size_t>(k0)].ay * 1.5f));
    uint32_t chains = 0;
    for (const Edge& e : u.pp().edges)
      if (e.kind == EdgeKind::Chain && (e.to == u.deviceNode(wt) || e.from == u.deviceNode(wt))) {
        ++chains;
        CHECK(e.points.front().y == doctest::Approx(e.points.back().y));
        CHECK(e.points.back().y == doctest::Approx(n.y + pinOffset(n, 0)));
      }
    CHECK(chains == 2);
    // Room made: the filter after it moved right by the growth, the band grew; the scene node draws the face at 1.5.
    const Node& f = u.pp().nodes[u.deviceNode(r.filter)];
    CHECK(f.x == doctest::Approx(f0.x + n0.w * 0.5f));
    CHECK(f.x >= n.x + n.w + metrics::kNodeGap - 0.01f);
    CHECK(u.pp().bands[n.band].h > bandH0);
    CHECK(v.scene().scene->nodes[v.scene().sceneNode[u.deviceNode(wt)]].scale == doctest::Approx(1.5f));
    CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");
    for (const Edge& e : u.pp().edges) CHECK(orthogonal(e.points));
  }
  // Clamped: far out 3×, far in 0.5×.
  sDrag(u, wt, 4000.0f, 4000.0f);
  CHECK(v.blockScale(wt) == doctest::Approx(doc::kMaxPatchScale));
  sDrag(u, wt, -4000.0f, -4000.0f);
  CHECK(v.blockScale(wt) == doctest::Approx(doc::kMinPatchScale));
  // Shift: steps of 25 %.
  sDrag(u, wt, 37.0f, 23.0f, mod::kShift);
  const float stepped = v.blockScale(wt);
  CHECK(std::fabs(stepped / 0.25f - std::round(stepped / 0.25f)) < 1e-5f);
  CHECK(stepped > doc::kMinPatchScale);
  // The detent at its own size: a drag that ends within 4 % of 1× is 1× (and the entry goes: nothing to save).
  {
    const Node& n = u.pp().nodes[u.deviceNode(wt)];
    const float want = 1.02f / v.blockScale(wt) - 1.0f;  // the factor to ≈ 1.02×
    const float zz = v.canvas().deviceScale();
    sDrag(u, wt, n.w * zz * want, n.h * zz * want);
  }
  CHECK(v.blockScale(wt) == 1.0f);
  CHECK(u.ctx.store().view().patchNudges.count(wt.value) == 0);
  // Esc during the drag: back to the size at the press.
  REQUIRE(v.setBlockScale(wt, 1.25f));
  {
    const Vec2 p = u.centre(u.deviceNode(wt));
    u.d.keyDown('s');
    u.d.move(p);
    u.d.down(p);
    u.d.move({p.x + 80.0f, p.y + 60.0f});
    CHECK(v.blockScale(wt) > 1.3f);  // live while dragging
    CHECK(u.d.tree().cursor() == Cursor::ResizeNWSE);
    CHECK(cv.drag() == ProjectCanvas::Drag::Resize);
    u.d.keyDown(key::kEscape);
    CHECK(v.blockScale(wt) == doctest::Approx(1.25f));
    u.d.up({p.x + 80.0f, p.y + 60.0f});
    CHECK(v.blockScale(wt) == doctest::Approx(1.25f));
    // S + double-click: its own size again.
    const Vec2 q = u.centre(u.deviceNode(wt));
    u.d.doubleClick(q);
    CHECK(v.blockScale(wt) == 1.0f);
    u.d.keyUp('s');
  }
  // The block menu's Reset Size (enabled only while scaled).
  REQUIRE(v.setBlockScale(r.filter, 2.0f));
  REQUIRE(v.openNodeMenu(u.deviceNode(r.filter), u.centre(u.deviceNode(r.filter))) != nullptr);
  Menu* m = u.openMenu();
  REQUIRE(m != nullptr);
  u.choose(*m, "Reset Size");
  CHECK(v.blockScale(r.filter) == 1.0f);
  CHECK(u.pp().nodes[u.deviceNode(r.filter)].w == doctest::Approx(f0.w));
  CHECK(u.history() == h0);
  // Without S a drag on the block does what it did (here the face's own control under the pointer): no resize.
  const float before = v.blockScale(wt);
  const Vec2 g = u.grip(u.deviceNode(wt));
  u.d.drag(g, {g.x + 6.0f, g.y + 4.0f}, 2);
  CHECK(v.blockScale(wt) == before);
  CHECK(cv.drag() == ProjectCanvas::Drag::None);
  // A held S is forgotten when the window loses focus (its release may never come).
  u.d.keyDown('s');
  u.d.tree().cancelInteraction();
  CHECK_FALSE(u.d.tree().keyHeld('s'));
}

// Save / load, the undo choice (view state like the nudges: never an undo step, untouched by undo / redo of document
// edits), Reset Layout keeps sizes, a strict reader for the saved value, and the model alone (no view) laid out with it.
TEST_CASE("project patch (TX.resize): a block's scale is saved view state — not undoable, loaded, validated, kept by Reset Layout") {
  Ui u;
  const ResizeSetup r = resizeSetup(u);
  u.show();
  ProjectPatchView& v = u.view();
  const doc::DeviceId wt = r.w.wt;
  const Node n0 = u.pp().nodes[u.deviceNode(wt)];
  const size_t h0 = u.history();
  REQUIRE(v.setBlockScale(wt, 1.75f));
  REQUIRE(v.setBlockScale(r.filter, 0.5f));
  CHECK_FALSE(v.setBlockScale(r.filter, 0.5f));  // unchanged: no re-layout
  CHECK(v.setBlockScale(r.filter, 9.0f));        // clamped
  CHECK(v.blockScale(r.filter) == doc::kMaxPatchScale);
  REQUIRE(v.setBlockScale(r.filter, 0.5f));
  v.nudge(wt.value, 12.0f, 0.0f);
  CHECK(u.history() == h0);
  CHECK(u.pp().nodes[u.deviceNode(wt)].w == doctest::Approx(n0.w * 1.75f));
  // A document edit and its undo / redo leave the scales alone (they are not in the history).
  REQUIRE(u.ctx.store().dispatch(doc::ops::setDeviceParam(*u.ctx.doc(), r.filter, "res", 0.5)->command).ok());
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.ctx.undo());
  CHECK(v.blockScale(wt) == doctest::Approx(1.75f));
  REQUIRE(u.ctx.redo());
  CHECK(v.blockScale(r.filter) == doctest::Approx(0.5f));
  CHECK(u.pp().nodes[u.deviceNode(wt)].scale == doctest::Approx(1.75f));
  // Saved and loaded with the project ("scale" on the entry; a scale-only entry keeps dx / dy 0).
  auto text = doc::serializeProject(*u.ctx.doc(), u.ctx.store().view());
  REQUIRE(text.ok());
  CHECK(text->find("\"scale\": 1.75") != std::string::npos);
  const std::string dir = pq::views::test::tempDir("project_patch_resize");
  REQUIRE(u.ctx.saveProjectAs(dir).ok());
  FakeTransport t2;
  ViewContext other(t2);
  REQUIRE(other.openProject(dir).ok());
  const auto& n = other.store().view().patchNudges;
  REQUIRE(n.count(wt.value) == 1);
  CHECK(n.at(wt.value).scale == 1.75f);
  CHECK(n.at(wt.value).dx == 12.0f);
  REQUIRE(n.count(r.filter.value) == 1);
  CHECK(n.at(r.filter.value).scale == 0.5f);
  CHECK(n.at(r.filter.value).dx == 0.0f);
  // The model laid out with the loaded view state (no view): the same scale.
  {
    LayoutOptions o;
    o.nudges = &n;
    const doc::LoweredProject low = lowerOf(*other.doc());
    const ProjectPatch pp = buildProjectPatch(*other.doc(), low, &library().catalog(), o);
    const uint32_t k = pp.nodeOf(wt);
    REQUIRE(k != kNone);
    CHECK(pp.nodes[k].scale == doctest::Approx(1.75f));
    CHECK(checkAgainstGraph(pp, *other.doc(), low) == "");
  }
  // Strict reader: a scale outside 0.5 … 3 is a load error.
  {
    std::string bad = *text;
    const size_t at = bad.find("\"scale\": 1.75");
    REQUIRE(at != std::string::npos);
    bad.replace(at, 13, "\"scale\": 4.5");
    CHECK_FALSE(doc::parseProject(bad).ok());
    std::string good = *text;
    good.replace(good.find("\"scale\": 1.75"), 13, "\"scale\": 2.5");
    CHECK(doc::parseProject(good).ok());
  }
  // Reset Layout: positions only, the sizes stay.
  v.resetLayout();
  CHECK(u.ctx.store().view().patchNudges.at(wt.value).dx == 0.0f);
  CHECK(v.blockScale(wt) == doctest::Approx(1.75f));
  // Back at 1: no entry, nothing written.
  REQUIRE(v.setBlockScale(wt, 1.0f));
  REQUIRE(v.setBlockScale(r.filter, 1.0f));
  CHECK(u.ctx.store().view().patchNudges.empty());
  auto plain = doc::serializeProject(*u.ctx.doc(), u.ctx.store().view());
  REQUIRE(plain.ok());
  CHECK(plain->find("\"scale\"") == std::string::npos);
  CHECK(u.history() == h0 + 1);
}

// Scaled faces and panels stay interactive and crisp: the panel is laid out at the scaled px per unit (its fonts and
// 1-px lines resolved at it) once the scale settles, the face's static layers are tiles at its own scale (not
// stretched); a knob drag on either edits the document (one undo step); the cables end on the scaled controls.
TEST_CASE("project patch (TX.resize): a scaled face and panel — crisp, interactive in place, cables on their controls") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());
  const ResizeSetup r = resizeSetup(u);
  u.show();
  ProjectPatchView& v = u.view();
  REQUIRE(v.setBlockScale(r.w.wt, 2.0f));
  REQUIRE(v.setBlockScale(r.filter, 1.5f));
  u.zoomTo("Lead WT");
  u.d.advance(0.5);  // the scale and zoom settle: tiles and the panel's layout at the new scale
  const float z = v.canvas().deviceScale();
  // Crisp: the panel laid out at 1.5 × the canvas's px per unit; the face's layers from tiles, none stretched.
  devices::PanelFace* pf = v.panelOf(r.filter);
  REQUIRE(pf != nullptr);
  CHECK(pf->layoutScale() == doctest::Approx(std::clamp(z * 1.5f, devices::PanelFace::kMinScale, devices::PanelFace::kMaxScale)));
  CHECK(pf->worldSize().x == doctest::Approx(u.pp().nodes[u.deviceNode(r.filter)].w / 1.5f));
  v.canvas().markDirty();
  u.d.step();
  CHECK(v.canvas().stats().faceLayersFromTiles > 0);
  CHECK(v.canvas().stats().faceLayersStretched == 0);
  CHECK(v.canvas().stats().panelsPainted >= 1);
  CHECK_FALSE(v.canvas().tilesPending());
  // The cable onto the scaled panel's knob: its anchor is the panel's control × 1.5, and the drawn end (the block's
  // origin + the face's anchor × its scale) is on the knob where the panel shows it.
  {
    const Node& n = u.pp().nodes[u.deviceNode(r.filter)];
    const int32_t k = n.input("freq");
    REQUIRE(k >= 0);
    const Port& p = n.inputs[static_cast<size_t>(k)];
    REQUIRE(p.anchored);
    const std::optional<ControlAnchor> a = pf->controlAnchor("freq");
    REQUIRE(a.has_value());
    CHECK(p.ax == doctest::Approx(a->at.x * 1.5f).epsilon(0.02));
    CHECK(p.ay == doctest::Approx(a->at.y * 1.5f).epsilon(0.02));
    const Vec2 knob = controlAt(u, r.filter, "freq");
    const Vec2 drawn = u.dev(n.x + a->at.x * n.scale, n.y + a->at.y * n.scale);
    CHECK(std::fabs(knob.x - drawn.x) < 2.0f);
    CHECK(std::fabs(knob.y - drawn.y) < 2.0f);
    const uint32_t w = sceneWireOf(v, r.filterWire);
    REQUIRE(w != kNone);
    const circuit::CircuitScene& sc = *v.scene().scene;
    const graph::Point2 end = sc.points[sc.wires[w].firstPoint + sc.wires[w].pointCount - 1];
    const graph::Point2 entry = inputEntry(n, static_cast<size_t>(k));
    CHECK(end.x == doctest::Approx(entry.x));
    CHECK(end.y == doctest::Approx(entry.y));
    // The knob under the pointer turns (one undo step): hit-testing follows the scale.
    const size_t h0 = u.history();
    const double f0 = paramOf(u, r.filter, "freq");
    u.d.drag(knob, {knob.x, knob.y - 40.0f}, 4);
    CHECK(u.history() == h0 + 1);
    CHECK(paramOf(u, r.filter, "freq") > f0);
  }
  // The scaled Wavetable face: the wire on warp ends on warp × 2; its fold knob where the face puts it × 2, and a drag
  // there edits the Wavetable (one undo step).
  {
    const uint32_t mn = u.deviceNode(r.w.wt);
    const Node& n = u.pp().nodes[mn];
    const MetaFace* face = v.canvas().faceOn(v.scene().sceneNode[mn]);
    REQUIRE(face != nullptr);
    CHECK(face->worldSize().x * 2.0f <= n.w + 4.0f);
    const std::optional<ControlAnchor> warp = face->controlAnchor("warp");
    REQUIRE(warp.has_value());
    const int32_t kw = n.input("warp");
    REQUIRE(kw >= 0);
    CHECK(n.inputs[static_cast<size_t>(kw)].ax == doctest::Approx(warp->at.x * 2.0f));
    const std::optional<ControlAnchor> fold = face->controlAnchor("fold");
    REQUIRE(fold.has_value());
    const Vec2 at = u.dev(n.x + fold->at.x * 2.0f, n.y + fold->at.y * 2.0f);
    auto foldOf = [&]() -> double {
      const doc::Device* d = doc::findDevice(*u.ctx.doc(), r.w.wt).device;
      REQUIRE(d != nullptr);
      const doc::DeviceValue* fv = d->param("fold");  // a chain meta device's params are its device params
      return fv != nullptr ? fv->value : -1.0;
    };
    const double fold0 = foldOf();
    const size_t h0 = u.history();
    u.d.drag(at, {at.x, at.y - 50.0f}, 5);
    CHECK(u.history() == h0 + 1);
    CHECK(foldOf() != fold0);
  }
}

// Smooth while dragging: each S-drag step re-lays out the project (model + scene; no lowering) and paints. 64 tracks
// with panels (Saturator / EQ Eight / Utility on every track): the step's cost, measured.
TEST_CASE("project patch (TX.resize): 64 tracks — an S-drag step (re-layout + frame) within a 120 Hz frame" * doctest::test_suite("perf")) {
  Ui u(false);
  REQUIRE(buildWideProject(u.ctx.store(), 64).ok());
  REQUIRE(u.d.enableGpu().ok());
  u.show();
  u.zoomTo("Track 9");
  u.d.advance(1.0);
  ProjectPatchView& v = u.view();
  const doc::DeviceId dev = u.track("Track 9").devices[1]->id;
  const Vec2 c = u.centre(u.deviceNode(dev));
  u.d.keyDown('s');
  u.d.move(c);
  u.d.down(c);
  std::vector<double> ms, lay;  // the steps that changed the scale (whole percent; the detent at 1× holds the first few)
  for (int i = 1; i <= 40; ++i) {
    const uint64_t before = v.stats().rescales;
    const auto t0 = std::chrono::steady_clock::now();
    u.d.move({c.x + static_cast<float>(i) * 3.0f, c.y + static_cast<float>(i) * 2.0f});
    u.d.step();
    const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (v.stats().rescales == before) continue;
    ms.push_back(t);
    lay.push_back(v.stats().lastRelayoutMs);
  }
  REQUIRE(ms.size() >= 15);
  u.d.up({c.x + 120.0f, c.y + 80.0f});
  u.d.keyUp('s');
  std::sort(ms.begin(), ms.end());
  std::sort(lay.begin(), lay.end());
  MESSAGE("64 tracks: S-drag step median " << ms[ms.size() / 2] << " ms, max " << ms.back() << " ms; re-layout median "
                                           << lay[lay.size() / 2] << " ms (" << v.stats().rescales << " rescales, "
                                           << v.canvas().panelCount() << " panels)");
  CHECK(ms[ms.size() / 2] < 1000.0 / 120.0);
}
