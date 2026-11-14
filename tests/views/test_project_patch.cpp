// T4.13 the project patch view: the model is the lowered ProjectGraph (checked node for node, wire for wire, groups and
// sends), the layout mirrors the arrangement, nudges are saved view state, the morph keeps every lane's vertical place,
// every edit kind made in the patch view is one undo step and shows in the Ableton view, semantic zoom, idle costs
// nothing. Goldens: project-patch-overview, project-patch-band. Wall-clock checks (64 tracks: full paint, morph frame)
// are in the perf suite.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "all_free_helpers.h"
#include "../support/blocks.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/meta/envelope.h"
#include "pq/meta/envelope.h"
#include "pq/meta/wavetable.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/computer_keyboard.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
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

// GU.11: the block at `slot` of a track's main path (a module is a group: its members follow it in Track::devices).
doc::DeviceId pathAt(const doc::Project& p, const doc::Track& t, size_t slot) {
  const doc::MainPath path = doc::mainPath(p, t);
  REQUIRE(slot < path.blocks.size());
  return path.blocks[slot]->id;
}
// The first top-level group of a track (the free LFO module of the sketch's Mod track).
doc::DeviceId firstGroup(const doc::Track& t) {
  for (const doc::DevicePtr& d : t.devices)
    if (doc::isGroupDevice(*d) && !d->group.valid()) return d->id;
  FAIL("no group");
  return {};
}

bool orthogonal(const std::vector<graph::Point2>& pts) {
  for (size_t i = 0; i + 1 < pts.size(); ++i)
    if (pts[i].x != pts[i + 1].x && pts[i].y != pts[i + 1].y) return false;
  return pts.size() >= 2;
}
// TX.cables: a bus (route / send) is an orthogonal trace; every other edge is a cable: its two ends (it hangs between).
bool routedAsDrawn(const Edge& e) { return e.cable ? e.points.size() == 2 : orthogonal(e.points); }

// TL.4: a band title strip's buttons (fold, automation) take presses only where they are readable (≥ 8 px titles); the
// band fits (2× mixers make bands larger) may leave them smaller: zoom in on world point `w` first.
inline void readableAt(ProjectPatchView& v, graph::Point2 w) {
  ProjectCanvas& c = v.canvas();
  if (metrics::kTitle * c.deviceScale() >= 12.0f) return;
  const IRect r = c.rect();
  const double us = static_cast<double>(c.deviceScale()) / c.view().zoom;  // device px per logical px
  circuit::CircuitCanvas::View cv;
  cv.zoom = 0.75;
  cv.x = static_cast<double>(w.x) - static_cast<double>(r.w) * 0.5 / (cv.zoom * us);
  cv.y = static_cast<double>(w.y) - static_cast<double>(r.h) * 0.5 / (cv.zoom * us);
  c.setView(cv);
  if (ui::UiTree* t = v.tree()) t->layoutNow();
}

// The shell over the sketch project (or a given builder), the patch view shown without the morph.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;

  explicit Ui(bool sketch = true) {
    ctx.setDeviceLibrary(&library());
    if (sketch) {
      const pq::Status built = buildSketchProject(ctx.store());
      REQUIRE_MESSAGE(built.ok(), (built.ok() ? std::string() : built.error().message));
    }
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
  doc::DeviceId onPath(std::string_view name, size_t slot) const { return pathAt(*ctx.doc(), track(name), slot); }
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
    return n.panel || n.groupPanel ? dev(n.x + n.w * 0.5f, n.y + 10.0f) : centre(node);  // (GU.11: a module's panel too)
  }
  Vec2 outPin(uint32_t node, std::string_view name) const {
    const Node& n = pp().nodes[node];
    const int32_t k = n.output(name);
    REQUIRE(k >= 0);
    if (n.kind == NodeKind::Lane) {  // TL.1: a lane's output is its port chip (an audio lane: a spot on its top edge)
      const LaneRect c = view().canvas().lanePortChip(n.band, static_cast<uint32_t>(k));
      if (c.x1 > c.x0) return dev((c.x0 + c.x1) * 0.5f, (c.y0 + c.y1) * 0.5f);
      return dev(pp().laneX0 + (pp().laneX1 - pp().laneX0) * 0.9f, n.y);
    }
    (void)n;  // TX.orbit-ports: where the port is drawn (an orbiting main port sits on the block's outline)
    return view().canvas().toDevice(view().canvas().portWorld(node, static_cast<uint32_t>(k), false));
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
  // TL.1: a band framed from its lane's left end (its port chips, its first blocks), at zoom 1.
  void frameLane(std::string_view track) {
    const ProjectPatch& p = pp();
    const Band& b = p.bands[p.bandOf(named(*ctx.doc(), track)->id)];
    circuit::CircuitCanvas::View cv;
    cv.zoom = 1.0;
    cv.x = static_cast<double>(p.laneX0) - 20.0 / cv.zoom;
    cv.y = static_cast<double>(b.y) - 60.0 / cv.zoom;
    view().canvas().setView(cv);
    d.tree().layoutNow();
  }
  // Zoomed so blocks and pins are big enough to hit reliably.
  void zoomTo(std::string_view track) {
    view().focusBand(named(*ctx.doc(), track)->id, false);
    d.tree().layoutNow();
  }
  // A point of band `b` over nothing (no block, cable, trace or lane window; below its title strip), on the canvas.
  Vec2 emptyIn(uint32_t b) const {
    const ProjectCanvas& c = view().canvas();
    const Band& band = pp().bands[b];
    const IRect r = c.rect();
    for (float y = band.y + metrics::kTitle + 4.0f; y < band.y + band.h - 2.0f; y += 5.0f)
      for (float x = band.x0 + 4.0f; x < band.x1; x += 7.0f) {
        const Vec2 p = dev(x, y);
        if (p.x < r.x + 4 || p.y < r.y + 4 || p.x > r.right() - 4 || p.y > r.bottom() - 4) continue;
        if (c.nodeAt(p) == kNone && c.bandAt(p) == b && c.hitTest(p).kind == circuit::CanvasHit::Kind::None && view().laneAt(p) == kNone &&
            c.laneEdgeAt(p).kind == ProjectCanvas::LaneEdgeHit::Kind::None && !(view().ruler() && view().ruler()->visible() && view().ruler()->rect().contains(p)))
          return p;  // (TL.1: nor a lane's edge, nor the ruler over the canvas's top)
      }
    FAIL("no empty point in the band");
    return {};
  }
  // TX.mixer-look: a point of a mixer's face (MixerFace's layout, face-local world units from its top-left).
  // (x, y) in the face's own units (TL.4: the face is drawn at the node's scale, 2× by default).
  Vec2 onMixer(uint32_t node, float x, float y) const {
    const Node& n = pp().nodes[node];
    return dev(n.x + x * n.scale, n.y + y * n.scale);
  }
  Vec2 mixerMute(uint32_t node) const { return onMixer(node, pp().nodes[node].w / pp().nodes[node].scale - 39.5f, 9.5f); }
  Vec2 mixerSolo(uint32_t node) const { return onMixer(node, pp().nodes[node].w / pp().nodes[node].scale - 19.5f, 9.5f); }
  Vec2 mixerPan(uint32_t node) const { return onMixer(node, 27.0f, 45.0f); }
  // On the volume fader's groove, `u` of the way along it (0 … 1).
  Vec2 mixerFader(uint32_t node, float u) const { return onMixer(node, 52.0f + (pp().nodes[node].w / pp().nodes[node].scale - 66.0f) * u, 41.0f); }
  Vec2 mixerFrame(uint32_t node) const { return onMixer(node, 40.0f, 64.0f); }  // between the readouts: the block's
};

}  // namespace

// BR.patch-io (user 2026-11-13: "inputting sound, either into an audio track or to the master, should create a long cable
// that stretches from the very left (the input port) to the very right (the mixer)"; "the current cable that goes into
// the master should instead go into the very left of the master before turning into a normal fluid looking cable").
TEST_CASE("project patch (BR.patch-io): an audio track's input and the master's buses enter at the band's left edge") {
  Ui u;
  doc::DocumentStore& st = u.ctx.store();
  auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "Mic");
  REQUIRE(t.ok());
  REQUIRE(st.dispatch(t->command).ok());
  const doc::TrackId mic = t->tracks.at(0);
  u.show();
  auto bandOf = [&](doc::TrackId id) -> const Band& { return u.pp().bands[u.pp().bandOf(id)]; };
  // BR.input-port: every audio track has its socket (its lane's clips enter there), an input set or not.
  REQUIRE(bandOf(mic).input != kNone);
  REQUIRE(st.dispatch(doc::SetTrackAudioInput{mic, doc::AudioInput{0, 2}}).ok());
  REQUIRE(st.dispatch(doc::SetTrackMonitor{mic, doc::MonitorMode::In}).ok());
  u.d.step();
  u.view().focusBand(mic, false);
  u.d.tree().layoutNow();
  {
    const ProjectPatch& pp = u.pp();
    CHECK(checkAgainstGraph(pp, *u.ctx.doc(), *u.view().lowered()) == "");
    const Band& b = bandOf(mic);
    REQUIRE(b.input != kNone);
    const Node& in = pp.nodes[b.input];
    CHECK(in.kind == NodeKind::Input);
    CHECK(in.x == doctest::Approx(b.x0 + metrics::kInputInset));  // the band's very left, left of its lane window
    CHECK(in.x < pp.laneX0);
    const Node& mx = pp.nodes[b.mixer];
    CHECK(in.y == doctest::Approx(mx.y + pinOffset(mx, 0)));  // on the spine: level with the mixer's input
    // One cable from it to where the clips' signal enters the chain (no blocks: the mixer), as the lane's own cable.
    uint32_t cables = 0;
    for (const Edge& e : pp.edges) {
      if (e.from != b.input) continue;
      ++cables;
      CHECK(e.kind == EdgeKind::Chain);
      CHECK(e.cable);
      CHECK(e.to == b.mixer);
      CHECK_FALSE(e.silent);
      CHECK(e.points.front() == graph::Point2{in.x, in.y});
      CHECK(e.points.back().x >= mx.x - 0.01f);
    }
    CHECK(cables == 1);
    // Its rest shape is the tight long-cable one: it hangs over the band's circuit, never into its lane.
    const circuit::CircuitScene& sc = *u.view().canvas().scene();
    bool found = false;
    for (uint32_t w = 0; w < sc.wires.size(); ++w) {
      if (w >= u.view().scene().modelEdge.size() || u.view().scene().modelEdge[w] == kNone) continue;
      const Edge& e = pp.edges[u.view().scene().modelEdge[w]];
      if (e.from != b.input) continue;
      found = true;
      for (uint32_t i = 0; i < sc.wires[w].pointCount; ++i) CHECK(sc.points[sc.wires[w].firstPoint + i].y < b.laneY - 4.0f);
      // The socket is drawn as an input port.
      CHECK(sc.pins[sc.wires[w].fromPin].socket == circuit::ScenePin::kSocketIn);
    }
    CHECK(found);
  }
  // BR.input-port: the lane's signal is drawn into the socket — a trace out of the lane's left end, round the band's left
  // edge (left of it), up and in to the socket; the edge itself still ends where the clips enter the chain.
  {
    const ProjectPatch& pp = u.pp();
    const Band& b = bandOf(mic);
    const Node& in = pp.nodes[b.input];
    uint32_t lane = 0;
    for (const Edge& e : pp.edges) {
      if (e.from != b.source || e.kind != EdgeKind::Chain) continue;
      ++lane;
      CHECK(e.to == b.mixer);
      CHECK(e.drawnTo == b.input);
      CHECK_FALSE(e.cable);
      REQUIRE(e.points.size() == 4);
      CHECK(e.points.front().x == doctest::Approx(pp.laneX0));
      CHECK(e.points.front().y > b.laneY + metrics::kLaneRulerH);
      CHECK(e.points.front().y < b.laneY + b.laneH);
      CHECK(e.points[1].x < b.x0);
      CHECK(e.points[2].x < b.x0);
      CHECK(e.points[2].y == doctest::Approx(in.y));
      CHECK(e.points.back() == graph::Point2{in.x, in.y});
    }
    CHECK(lane == 1);
  }
  // Not monitored (Off): the socket's cable still carries the clips (not dim).
  REQUIRE(st.dispatch(doc::SetTrackMonitor{mic, doc::MonitorMode::Off}).ok());
  u.d.step();
  for (const Edge& e : u.pp().edges)
    if (e.from == bandOf(mic).input) CHECK_FALSE(e.silent);
  // A device on its path: the input goes into it (where the clips' signal enters the chain), not past it to the mixer.
  auto d = doc::ops::addDevice(*st.current(), st.ids(), mic, "module:utility", doc::DeviceKind::AudioEffect);
  REQUIRE(d.ok());
  REQUIRE(st.dispatch(d->command).ok());
  u.d.step();
  {
    const ProjectPatch& pp = u.pp();
    const Band& b = bandOf(mic);
    const uint32_t util = pp.nodeOf(d->devices.at(0));
    REQUIRE(util != kNone);
    uint32_t lane = kNone, input = kNone;
    for (const Edge& e : pp.edges) {
      if (e.from == b.source && e.fromPort == 0 && e.kind == EdgeKind::Chain) lane = e.to;
      if (e.from == b.input) input = e.to;
    }
    CHECK(lane == util);
    CHECK(input == lane);
    CHECK(pp.nodes[util].x >= pp.nodes[b.input].x + metrics::kInputRoom - 8.0f);  // room for the socket and its cable
  }
  // The master: every route into it is a trace that ends on its input socket at its left edge; a cable goes on into
  // its Σ.
  {
    const ProjectPatch& pp = u.pp();
    const Band& m = pp.bands[pp.master];
    REQUIRE(m.input != kNone);
    const Node& in = pp.nodes[m.input];
    CHECK(in.x == doctest::Approx(m.x0 + metrics::kInputInset));
    uint32_t routes = 0;
    for (const Edge& e : pp.edges) {
      if (e.kind != EdgeKind::Route || e.to != m.source) continue;
      ++routes;
      CHECK_FALSE(e.cable);
      CHECK(e.drawnTo == m.input);
      CHECK(e.points.back() == graph::Point2{in.x, in.y});
      CHECK(e.points[e.points.size() - 2].x < in.x);  // it comes in from the band's left
      CHECK(e.points[e.points.size() - 2].y == in.y);
    }
    CHECK(routes >= 3);
    bool intoSum = false;
    for (const Edge& e : pp.edges) intoSum = intoSum || (e.from == m.input && e.to == m.source && e.cable);
    CHECK(intoSum);
    // In the scene the routes end on the socket (it moves with them).
    const circuit::CircuitScene& sc = *u.view().canvas().scene();
    const uint32_t sock = u.view().canvas().scenePin(m.input, 0, false);
    REQUIRE(sock != kNone);
    for (uint32_t w = 0; w < sc.wires.size() && w < u.view().scene().modelEdge.size(); ++w)
      if (const uint32_t k = u.view().scene().modelEdge[w]; k != kNone && pp.edges[k].kind == EdgeKind::Route && pp.edges[k].to == m.source)
        CHECK(sc.wires[w].toPin == sock);
  }
}

TEST_CASE("project patch: an audio track's source is its clip player (T5.3), the view still matches the graph") {
  doc::DocumentStore store;
  REQUIRE(buildSketchProject(store).ok());
  auto t = doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Audio, {}, "Loop");
  REQUIRE(t.ok());
  REQUIRE(store.dispatch(t->command).ok());
  auto d = doc::ops::addDevice(*store.current(), store.ids(), t->tracks.at(0), "module:utility", doc::DeviceKind::AudioEffect);
  REQUIRE(d.ok());
  REQUIRE(store.dispatch(d->command).ok());
  const doc::Project& p = *store.current();
  doc::LowerOptions o;
  o.library = &library().library();
  o.clipSlotOf = [](doc::TrackId) { return 0u; };
  auto low = doc::lower(p, o);
  REQUIRE(low.ok());
  const ProjectPatch pp = buildProjectPatch(p, *low, &library());
  CHECK(checkAgainstGraph(pp, p, *low) == "");
  const auto band = std::find_if(pp.bands.begin(), pp.bands.end(), [](const Band& b) { return b.name == "Loop"; });
  REQUIRE(band != pp.bands.end());
  CHECK(band->kind == doc::TrackKind::Audio);
  REQUIRE(band->source != kNone);
  CHECK(pp.nodes[band->source].kind == NodeKind::Lane);  // TL.1: an audio track's source is its lane
  CHECK(pp.nodes[band->source].title == "Audio");
  REQUIRE(band->devices.size() == 1);
}

TEST_CASE("project patch: the sketch project's view is its lowered ProjectGraph, node for node and wire for wire") {
  doc::DocumentStore store;
  REQUIRE(buildSketchProject(store).ok());
  const doc::Project& p = *store.current();
  const doc::LoweredProject low = lowerOf(p);
  const ProjectPatch pp = buildProjectPatch(p, low, &library());
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
    // (GU.11: not the modules' inner wires; BR.patch-io: a wire into an opened group is drawn as a stretch per frame
    // socket, each its wire's: counted once)
    mods += e.kind == EdgeKind::Modulation && e.graphWire != kNone && e.hop == 0;
    CHECK(routedAsDrawn(e));
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

TEST_CASE("project patch: deactivated tracks and devices, nested groups, return sends and loops match the graph") {
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
  const doc::DeviceId util = run(doc::ops::addDevice(*store.current(), store.ids(), keys, "module:utility", doc::DeviceKind::AudioEffect)).devices[0];
  run(doc::ops::setDevicesActive(*store.current(), std::vector<doc::DeviceId>{util}, false));
  // (GU.11: a device whose patch is not in the library cannot be in the model any more: checkList refuses it.)
  CHECK_FALSE(store.dispatch(doc::AddDevice{keys, doc::makeDevice(store.ids().device(), "nosuch", doc::DeviceKind::AudioEffect)}).ok());
  // A second return; return → return sends both ways (stored: the second closes a loop and is delayed); a pre send.
  const doc::TrackId echo = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Return, space, "Echo")).tracks[0];
  REQUIRE(store.dispatch(doc::SetSendLevel{space, echo, -3.0f}).ok());
  REQUIRE(store.dispatch(doc::SetSendLevel{echo, space, -9.0f}).ok());
  run(doc::ops::setReturnSendsPreFader(*store.current(), echo, true));
  // A wire into a deactivated device: inactive (drawn dim, not in the graph).
  const doc::DeviceId lfo = firstGroup(*named(*store.current(), "Mod"));
  run(doc::ops::connectTravelWire(*store.current(), store.ids(), lfo, "y", test_blocks::member(*store.current(), util, "Width"),
                                  std::string(doc::kValueParam), 0.3));
  // The group routed to sends only.
  REQUIRE(store.dispatch(doc::SetTrackOutput{grp, doc::Output{doc::OutputKind::None, {}}}).ok());

  const doc::Project& p = *store.current();
  const doc::LoweredProject low = lowerOf(p);
  REQUIRE_FALSE(low.delayedSends.empty());
  const ProjectPatch pp = buildProjectPatch(p, low, &library());
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
  bool delayed = false, inactive = false, pre = false;
  for (const Node& n : pp.nodes) CHECK_FALSE((n.kind == NodeKind::Device && n.missing));
  for (const Edge& e : pp.edges) {
    delayed = delayed || (e.kind == EdgeKind::Send && e.delayed);
    inactive = inactive || (e.kind == EdgeKind::Modulation && e.status == doc::WireStatus::Inactive && e.graphWire == kNone);
    pre = pre || (e.kind == EdgeKind::Send && e.pre);
  }
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
  {  // (GU.11: without its default instrument group — a meta module at slot 0 would go into it)
    const doc::Track& tr = *doc::findTrackPtr(*store.current(), t);
    std::vector<doc::DeviceId> all;
    for (const doc::DevicePtr& d : tr.devices) all.push_back(d->id);
    run(doc::ops::deleteDevices(*store.current(), all));
  }
  const doc::DeviceId acc = run(doc::ops::addMetaDevice(*store.current(), store.ids(), t, "envelope", 0)).devices.at(0);  // (GU.4: was a Phase accumulator)
  const doc::DeviceId wt = run(doc::ops::addMetaDevice(*store.current(), store.ids(), t, "wavetable")).devices.at(0);
  auto notesTo = [&](const ProjectPatch& pp, doc::DeviceId d) {
    for (const Edge& e : pp.edges)
      if (e.kind == EdgeKind::Notes && e.to == pp.nodeOf(d)) return true;
    return false;
  };
  {
    const doc::LoweredProject low = lowerOf(*store.current());
    const ProjectPatch pp = buildProjectPatch(*store.current(), low, &library());
    CHECK(checkAgainstGraph(pp, *store.current(), low) == "");
    CHECK(pp.nodes[pp.nodeOf(acc)].instrument);
    CHECK(notesTo(pp, acc));
    CHECK_FALSE(notesTo(pp, wt));
  }
  // GU.11 (no chain inputs): taken off the path, no note wire reaches it; the notes reach what heads the path now.
  run(doc::ops::takeOffPath(*store.current(), store.ids(), &library(), std::vector<doc::DeviceId>{acc}));
  {
    const doc::LoweredProject low = lowerOf(*store.current());
    const ProjectPatch pp = buildProjectPatch(*store.current(), low, &library());
    CHECK(checkAgainstGraph(pp, *store.current(), low) == "");
    CHECK_FALSE(pp.nodes[pp.nodeOf(acc)].instrument);
    CHECK_FALSE(notesTo(pp, acc));
  }
}

// T4.8: racks are drawn as plain blocks; the devices inside them (racks lower inline) belong to the rack's block, and
// wires into / out of them end on it.
TEST_CASE("project patch: the layout mirrors the arrangement (lane order, groups as containers, left → right)") {
  doc::DocumentStore store;
  REQUIRE(buildWideProject(store, 20).ok());
  const doc::Project& p = *store.current();
  const doc::LoweredProject low = lowerOf(p);
  const ProjectPatch pp = buildProjectPatch(p, low, &library());
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
    // Within a band: left → right, no overlaps, every node inside the band's strip (TL.1: a lane is no block on it).
    std::vector<uint32_t> chain;
    if (pp.nodes[b.source].kind == NodeKind::Sum) chain.push_back(b.source);
    chain.insert(chain.end(), b.devices.begin(), b.devices.end());
    chain.push_back(b.mixer);
    for (size_t k = 0; k < chain.size(); ++k) {
      const Node& n = pp.nodes[chain[k]];
      CHECK(n.y >= b.y);
      CHECK(n.y + n.h <= b.y + b.h);
      if (k > 0) CHECK(n.x >= pp.nodes[chain[k - 1]].x + pp.nodes[chain[k - 1]].w);
    }
    // TL.7: the chains' staircase is of their blocks (every unplaced mixer sits at the lanes' right end, checked below).
    float chainEnd = pp.nodes[b.source].kind == NodeKind::Lane ? -1e30f : pp.nodes[b.source].x + pp.nodes[b.source].w;
    for (const uint32_t d : b.devices) chainEnd = std::max(chainEnd, pp.nodes[d].x + pp.nodes[d].w);
    if (b.kind == doc::TrackKind::Midi || b.kind == doc::TrackKind::Group) mainEnd = std::max(mainEnd, chainEnd);
    if (b.kind == doc::TrackKind::Return) {
      retStart = std::min(retStart, pp.nodes[b.source].x);
      retEnd = std::max(retEnd, chainEnd);
    }
    // TL.7: its mixer at the lanes' right end (the master's Out after it), level with its main path.
    {
      const Node& mix = pp.nodes[b.mixer];
      const float right = b.output != kNone ? pp.nodes[b.output].x + pp.nodes[b.output].w : mix.x + mix.w;
      CHECK(right == doctest::Approx(pp.laneX1 + metrics::kLaneInset - 12.0f));
      CHECK(mix.x >= chainEnd + metrics::kNodeGap - 0.01f);
    }
    // A group's own chain starts right of its members' chains; its container holds them.
    if (b.kind == doc::TrackKind::Group && b.lastMember != kNone) {
      const uint32_t gi = static_cast<uint32_t>(&b - pp.bands.data());
      for (uint32_t c = gi + 1; c <= b.lastMember; ++c) {
        float memberEnd = -1e30f;
        for (const uint32_t d : pp.bands[c].devices) memberEnd = std::max(memberEnd, pp.nodes[d].x + pp.nodes[d].w);
        CHECK(pp.nodes[b.source].x > memberEnd);
        CHECK(pp.bands[c].x0 >= b.cx0);
        CHECK(pp.bands[c].y + pp.bands[c].h <= b.cy1);
      }
    }
  }
  CHECK(retStart > mainEnd);  // the returns' Σs right of every main track (their sends arrive by buses)
  REQUIRE(pp.master != kNone);
  CHECK(pp.nodes[pp.bands[pp.master].source].x > retEnd);  // the master on the right
  for (const Edge& e : pp.edges) CHECK(routedAsDrawn(e));
}

TEST_CASE("project patch: nudges are view state — not undoable, saved with the project, applied to the layout") {
  Ui u;
  u.show();
  const doc::Track& keys = u.track("Keys");
  const uint32_t b = u.pp().bandOf(keys.id);
  const uint32_t sat = u.deviceNode(pathAt(*u.ctx.doc(), keys, 1));
  const float by = u.pp().bands[b].y, sx = u.pp().nodes[sat].x, mx = u.pp().nodes[u.pp().bands[b].mixer].x;
  const size_t h0 = u.history();
  u.view().nudge(keys.id.value, 0.0f, 20.0f);
  u.view().nudge(pathAt(*u.ctx.doc(), keys, 1).value, 48.0f, 0.0f);
  CHECK(u.history() == h0);  // not an undo step
  CHECK(u.ctx.dirty());      // but saved
  CHECK(u.pp().bands[u.pp().bandOf(keys.id)].y == doctest::Approx(by + 20.0f));
  CHECK(u.pp().nodes[u.pp().nodeOf(pathAt(*u.ctx.doc(), keys, 1))].x == doctest::Approx(sx + 48.0f));
  // TX.mixer-look: the (unplaced) mixer stays the band's rightmost block — TL.7: at the lanes' right end, so the
  // Saturator nudged 48 right stays left of it and it does not move; nudged past it, it pushes the mixer (and the lanes).
  {
    const Node& sn = u.pp().nodes[u.pp().nodeOf(pathAt(*u.ctx.doc(), keys, 1))];
    const float mx1 = u.pp().nodes[u.pp().bands[u.pp().bandOf(keys.id)].mixer].x;
    CHECK(mx1 == doctest::Approx(std::max(mx, sn.x + sn.w + metrics::kNodeGap)));
    CHECK(mx1 == doctest::Approx(mx));
    const float far = std::round(mx - sx);  // past the mixer's old x
    u.view().nudge(pathAt(*u.ctx.doc(), keys, 1).value, far, 0.0f);
    const Node& sn2 = u.pp().nodes[u.pp().nodeOf(pathAt(*u.ctx.doc(), keys, 1))];
    const Node& m2 = u.pp().nodes[u.pp().bands[u.pp().bandOf(keys.id)].mixer];
    CHECK(m2.x == doctest::Approx(sn2.x + sn2.w + metrics::kNodeGap));
    CHECK(m2.x + m2.w == doctest::Approx(u.pp().laneX1 + metrics::kLaneInset - 12.0f));
    u.view().nudge(pathAt(*u.ctx.doc(), keys, 1).value, -far, 0.0f);  // back to +48
  }
  // Saved and loaded with the project.
  const std::string dir = pq::views::test::tempDir("project_patch_nudges");
  REQUIRE(u.ctx.saveProjectAs(dir).ok());
  FakeTransport t2;
  ViewContext other(t2);
  REQUIRE(other.openProject(dir).ok());
  const auto& n = other.store().view().patchNudges;
  REQUIRE(n.size() == 2);
  CHECK(n.at(keys.id.value).dy == 20.0f);
  CHECK(n.at(pathAt(*u.ctx.doc(), keys, 1).value).dx == 48.0f);
  // A nudge naming no entity is a load error (strict reader).
  auto text = doc::serializeProject(*u.ctx.doc(), u.ctx.store().view());
  REQUIRE(text.ok());
  std::string bad = *text;
  const size_t at = bad.find("\"patch\": [");  // the view state's nudges (not a device's "patch")
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

// ---- TX.mixer-look (bug report 2026-11-03: "positioned always at the top right … though you should be able to move
// it"): the mixer's default place, its drag (real dispatch), view state that survives re-layout, save / load and
// opening the band ----------------------------------------------------------------------------------------------------

namespace {
// Every band's mixer right of every other block of the band (its main path, the row under it, wherever their nudges put
// them) — TL.7: at the lanes' right end (the master's Out after it), its input level with the band's main path (the
// spine: its main-path blocks' main pins) and never over the title strip.
void checkMixersTopRight(const ProjectPatch& pp) {
  for (const Band& b : pp.bands) {
    if (b.mixer == kNone) continue;
    CAPTURE(b.name);
    const Node& m = pp.nodes[b.mixer];
    CHECK(m.y >= b.y + metrics::kTitle - 0.01f);
    for (const uint32_t d : b.devices)
      if (!pp.nodes[d].offPath && pp.nodes[d].capsule == kNone && !pp.nodes[d].folded) {
        CHECK(m.y + pinOffset(m, 0) == doctest::Approx(pp.nodes[d].y + pinOffset(pp.nodes[d], 0)));
        break;
      }
    const float right = b.output != kNone ? pp.nodes[b.output].x + pp.nodes[b.output].w : m.x + m.w;
    CHECK(right == doctest::Approx(pp.laneX1 + metrics::kLaneInset - 12.0f));
    CHECK(m.x + m.w <= b.x1);
    auto leftOf = [&](uint32_t n) {
      if (n != kNone && pp.nodes[n].capsule == kNone) CHECK(pp.nodes[n].x + pp.nodes[n].w + metrics::kNodeGap <= m.x + 0.01f);
    };
    if (pp.nodes[b.source].kind != NodeKind::Lane) leftOf(b.source);  // TL.1: the lane spans the band (under it all)
    for (const uint32_t d : b.devices) leftOf(d);
    // Nothing right of it (the master: Out); TL.1: the band reaches the lanes' common right end.
    if (b.output == kNone) CHECK(m.x + m.w + 12.0f <= b.x1 + 0.01f);
  }
}
}  // namespace

TEST_CASE("project patch (TX.mixer-look): the mixer is its band's top-right block; dragged, it stays where it is put") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  const doc::TrackId keys = u.track("Keys").id, pad = u.track("Pad").id;
  const doc::DeviceId sat = u.onPath("Keys", 1);
  auto mixerOf = [&](doc::TrackId t) { return u.pp().bands[u.pp().bandOf(t)].mixer; };
  auto bandOf = [&](doc::TrackId t) -> const Band& { return u.pp().bands[u.pp().bandOf(t)]; };
  // Default: the top right of every band; drawn as its face (hosted like a panel).
  checkMixersTopRight(u.pp());
  for (const Band& b : u.pp().bands)
    if (b.mixer != kNone) CHECK(v.canvas().panelOn(v.scene().sceneNode[b.mixer]) != nullptr);
  // A block placed past it pushes it right (it stays the top-right block).
  v.nudge(sat.value, 400.0f, 0.0f);
  checkMixersTopRight(u.pp());
  v.resetLayout();
  // Opened (the band's devices to their atoms), still its top right.
  v.openBand(keys, false);
  u.d.tree().layoutNow();
  checkMixersTopRight(u.pp());
  v.closeBand();
  u.d.tree().layoutNow();
  REQUIRE_FALSE(v.openedBand().valid());

  // The drag (window → canvas): a press on the face's frame (not a control), moved, released in its band.
  u.zoomTo("Keys");
  const Node m0 = u.pp().nodes[mixerOf(keys)];
  const float bx0 = bandOf(keys).x0, by0 = bandOf(keys).y;
  const size_t h0 = u.history();
  const Vec2 grip = u.mixerFrame(mixerOf(keys));
  const Vec2 to = u.dev(m0.x + 40.0f * m0.scale - 160.0f, m0.y + 64.0f * m0.scale + 24.0f);  // 160 left, 24 down (world units)
  u.d.move(grip);
  CHECK(u.d.tree().cursor() == Cursor::Move);
  u.d.down(grip);
  CHECK(v.canvas().drag() == ProjectCanvas::Drag::MaybeMoveMixer);
  for (int k = 1; k <= 8; ++k) u.d.move({grip.x + (to.x - grip.x) * static_cast<float>(k) / 8.0f, grip.y + (to.y - grip.y) * static_cast<float>(k) / 8.0f});
  CHECK(v.canvas().drag() == ProjectCanvas::Drag::MoveMixer);
  CHECK(v.canvas().lifted(mixerOf(keys)));  // drawn at the pointer meanwhile, like a block
  CHECK(u.pp().nodes[mixerOf(keys)].x == doctest::Approx(m0.x));  // no re-layout while it moves
  u.d.up(to);
  CHECK(u.history() == h0);  // view state, not an undo step
  CHECK(u.ctx.dirty());      // but saved
  REQUIRE(v.mixerPlaced(keys));
  const Node& m1 = u.pp().nodes[mixerOf(keys)];
  CHECK(std::fabs(m1.x - (m0.x - 160.0f)) <= 1.0f);
  CHECK(std::fabs(m1.y - (m0.y + 24.0f)) <= 1.0f);
  const doc::PatchNudge& e = u.ctx.store().view().patchNudges.at(keys.value);
  CHECK(e.mixerPlaced);
  CHECK(e.mixerX == doctest::Approx(m1.x - bx0));
  CHECK(e.mixerY == doctest::Approx(m1.y - by0));
  const float rx = m1.x - bx0, ry = m1.y - by0;
  auto placedStill = [&] {
    const Band& b = bandOf(keys);
    const Node& m = u.pp().nodes[b.mixer];
    CHECK(m.x - b.x0 == doctest::Approx(rx));
    CHECK(m.y - b.y == doctest::Approx(ry));
  };
  // No re-layout moves it: a block nudged past where it was, a device added to the band, the band nudged (it travels
  // with the band), the view hidden and shown again, the band opened.
  v.nudge(sat.value, 400.0f, 0.0f);
  placedStill();
  REQUIRE(v.addDevice(keys, "module:utility"));
  placedStill();
  v.nudge(keys.value, 30.0f, 12.0f);
  placedStill();
  v.setShown(false, false);
  u.d.tree().layoutNow();
  v.setShown(true, false);
  u.d.tree().layoutNow();
  placedStill();
  v.openBand(keys, false);
  u.d.tree().layoutNow();
  placedStill();
  v.closeBand();
  u.d.tree().layoutNow();
  // Saved and loaded with the project ("mixerX" / "mixerY" on the track's entry), laid out where it was put.
  auto text = doc::serializeProject(*u.ctx.doc(), u.ctx.store().view());
  REQUIRE(text.ok());
  CHECK(text->find("\"mixerX\"") != std::string::npos);
  const std::string dir = pq::views::test::tempDir("project_patch_mixer");
  REQUIRE(u.ctx.saveProjectAs(dir).ok());
  {
    FakeTransport t2;
    ViewContext other(t2);
    REQUIRE(other.openProject(dir).ok());
    const auto& n = other.store().view().patchNudges;
    REQUIRE(n.count(keys.value) == 1);
    CHECK(n.at(keys.value) == u.ctx.store().view().patchNudges.at(keys.value));
    LayoutOptions o;
    o.nudges = &n;
    const doc::LoweredProject low = lowerOf(*other.doc());
    const ProjectPatch pp = buildProjectPatch(*other.doc(), low, &library(), o);
    const Band& b = pp.bands[pp.bandOf(keys)];
    CHECK(pp.nodes[b.mixer].x - b.x0 == doctest::Approx(rx));
    CHECK(pp.nodes[b.mixer].y - b.y == doctest::Approx(ry));
    CHECK(checkAgainstGraph(pp, *other.doc(), low) == "");
  }
  // The same view reopening the saved project: the mixer where it was put.
  REQUIRE(u.ctx.openProject(dir).ok());
  u.d.tree().layoutNow();
  u.show();
  REQUIRE(v.mixerPlaced(keys));
  placedStill();
  // Strict reader: mixerX without mixerY is a load error.
  {
    std::string bad = *text;
    const size_t at = bad.find("\"mixerY\"");
    REQUIRE(at != std::string::npos);
    bad.replace(at, 8, "\"mixerZ\"");
    CHECK_FALSE(doc::parseProject(bad).ok());
  }
  // Dropped on another band: it stays (a mixer is its track's), nothing changes.
  u.view().fitAll(false);
  u.d.tree().layoutNow();
  {
    const auto before = u.ctx.store().view().patchNudges.at(keys.value);
    const Vec2 g = u.mixerFrame(mixerOf(keys));
    u.d.drag(g, u.emptyIn(u.pp().bandOf(pad)), 8);
    CHECK(u.ctx.store().view().patchNudges.at(keys.value) == before);
    placedStill();
  }
  // Its menu (a right click on the face): Mixer to Top Right puts it back where the layout puts it.
  u.zoomTo("Keys");
  const size_t hm = u.history();
  u.d.click(u.mixerFrame(mixerOf(keys)), mod::kNone, PointerButton::Right);
  Menu* menu = u.openMenu();
  REQUIRE(menu != nullptr);
  u.choose(*menu, "Mixer to Top Right");
  CHECK_FALSE(v.mixerPlaced(keys));
  checkMixersTopRight(u.pp());
  CHECK(u.history() == hm);  // view state
}

TEST_CASE("project patch (TX.mixer-look): the mixer's face — pan, double-click resets, an expanded strip's send — through dispatch") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  const doc::TrackId keys = u.track("Keys").id, space = u.track("Space").id;
  u.zoomTo("Keys");
  auto mixer = [&] { return u.pp().bands[u.pp().bandOf(keys)].mixer; };
  const size_t h0 = u.history();
  // Pan: up turns it right (one undo step), Shift is finer; a double-click centres it.
  const Vec2 pan = u.mixerPan(mixer());
  u.d.drag(pan, {pan.x, pan.y - 30.0f}, 6);
  CHECK(u.history() == h0 + 1);
  const float p1 = u.track("Keys").mixer.pan;
  CHECK(p1 > 0.0f);
  u.d.drag(pan, {pan.x, pan.y - 30.0f}, 6, mod::kShift);
  CHECK(u.history() == h0 + 2);
  CHECK(u.track("Keys").mixer.pan - p1 == doctest::Approx(0.1f * p1).epsilon(0.05));
  u.d.doubleClick(u.mixerPan(mixer()));
  CHECK(u.track("Keys").mixer.pan == 0.0f);
  // The fader: a double-click is 0 dB.
  REQUIRE(u.ctx.store().dispatch(doc::SetTrackVolume{keys, -20.0f}).ok());
  u.d.doubleClick(u.mixerFader(mixer(), static_cast<float>(volumeToFader(-20.0))));
  CHECK(u.track("Keys").mixer.volumeDb == 0.0f);
  // Expanded (a double-click on its frame): wider, the send's fader on its row (level with its pin), dragged sideways.
  const float w0 = u.pp().nodes[mixer()].w;
  u.d.doubleClick(u.mixerFrame(mixer()));
  REQUIRE(v.mixerExpanded(keys));
  const Node& mx = u.pp().nodes[mixer()];
  CHECK(mx.w > w0);
  const int32_t s = mx.output("s" + std::to_string(space.value));
  REQUIRE(s >= 1);
  CHECK(pinOffset(mx, static_cast<size_t>(s)) == (metrics::kMixerSendTop + metrics::kPinPitch * static_cast<float>(s - 1)) * mx.scale);
  CHECK(mx.h >= pinOffset(mx, static_cast<size_t>(s)) + 6.0f * mx.scale);
  float before = doc::kMinVolumeDb;
  for (const doc::Send& sd : u.track("Keys").mixer.sends)
    if (sd.target == space) before = sd.levelDb;
  const size_t h1 = u.history();
  const float fw = mx.w / mx.scale;  // the face's own width (TL.4: drawn at the node's scale)
  const Vec2 a = u.onMixer(mixer(), 24.0f + (fw - 50.0f - 24.0f) * static_cast<float>(volumeToFader(before)),
                           pinOffset(mx, static_cast<size_t>(s)) / mx.scale);
  u.d.drag(a, {a.x + 40.0f, a.y}, 6);
  CHECK(u.history() == h1 + 1);
  float after = doc::kMinVolumeDb;
  for (const doc::Send& sd : u.track("Keys").mixer.sends)
    if (sd.target == space) after = sd.levelDb;
  CHECK(after > before);
}

// TX.pan-laws (bug report 2026-11-05): the law's tag above the pan knob opens the law menu; a pick (a click on the
// item, through the window and the overlay) closes the menu, is one undo step, recompiles the track and the scene
// rebuilds during the event; the tag shows the new law; the mixer's right-click menu carries Pan Law too.
TEST_CASE("project patch (TX.pan-laws): the mixer face's pan law tag — a click, the menu, a pick, undo — through dispatch") {
  Ui u;
  u.show();
  const doc::TrackId keys = u.track("Keys").id;
  u.zoomTo("Keys");
  auto mixer = [&] { return u.pp().bands[u.pp().bandOf(keys)].mixer; };
  CHECK(u.track("Keys").mixer.panMode == doc::PanMode::Balance);
  const size_t h0 = u.history();
  u.d.click(u.onMixer(mixer(), 27.0f, 24.0f));
  Menu* m = u.openMenu();
  REQUIRE(m != nullptr);
  REQUIRE(m->items().size() == std::size(doc::kPanModes));
  CHECK(m->items()[0].checked);  // Balance
  int32_t stereo = -1;
  for (int32_t i = 0; i < static_cast<int32_t>(m->items().size()); ++i)
    if (m->items()[static_cast<size_t>(i)].label == doc::panModeName(doc::PanMode::Stereo)) stereo = i;
  REQUIRE(stereo >= 0);
  const IRect r = m->itemRect(stereo);
  u.d.click(Ui::mid(r));
  u.d.step();
  CHECK(u.openMenu() == nullptr);  // the menu closed itself
  CHECK(u.track("Keys").mixer.panMode == doc::PanMode::Stereo);
  CHECK(u.history() == h0 + 1);
  CHECK(u.ctx.store().history().undoLabel() == "Pan Law");
  // The face still works after the rebuild: the pan knob drags, the tag opens the menu again with True Stereo checked.
  const Vec2 pan = u.mixerPan(mixer());
  u.d.drag(pan, {pan.x, pan.y - 30.0f}, 6);
  CHECK(u.track("Keys").mixer.pan > 0.0f);
  u.d.click(u.onMixer(mixer(), 27.0f, 24.0f));
  m = u.openMenu();
  REQUIRE(m != nullptr);
  CHECK(m->items()[static_cast<size_t>(stereo)].checked);
  u.d.click(u.emptyIn(u.pp().bandOf(keys)));  // a press outside dismisses it
  u.d.step();
  CHECK(u.openMenu() == nullptr);
  REQUIRE(u.ctx.store().undo());  // the pan drag
  REQUIRE(u.ctx.store().undo());  // the law
  CHECK(u.track("Keys").mixer.panMode == doc::PanMode::Balance);
  // Right-click on the mixer: its menu has Pan Law ▸ first.
  u.d.click(u.mixerFrame(mixer()), mod::kNone, PointerButton::Right);
  m = u.openMenu();
  REQUIRE(m != nullptr);
  CHECK(m->items()[0].label == "Pan Law");
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
  // BR.patch-io (user 2026-11-13: it "should spawn proportional to the project; it should at least horizontally take up
  // the screen and vertically be showing the same modules that were open and visible"): at the end the project's width
  // fills the canvas (a margin each side) and the first lane's band is at the canvas's top, under the time ruler.
  {
    const ProjectPatch& pp = v.patch();
    const IRect cr = v.canvas().rect();
    const float l = v.canvas().toDevice({pp.x0, 0.0f}).x, r = v.canvas().toDevice({pp.x1, 0.0f}).x;
    CHECK(l >= static_cast<float>(cr.x));
    CHECK(r <= static_cast<float>(cr.right()));
    CHECK(r - l >= 0.95f * static_cast<float>(cr.w));
    const Band& first = pp.bands[pp.bandOf(lanes[0].first)];
    const float top = v.canvas().toDevice({first.x0, first.kind == doc::TrackKind::Group ? first.cy0 : first.y}).y;
    CHECK(top > static_cast<float>(cr.y));
    CHECK(top < static_cast<float>(cr.y) + 64.0f);
  }
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
  // BR.patch-io: scrolled down in the arrangement, the band of the first lane it shows is the one at the canvas's top
  // (more tracks first: room to scroll).
  for (int i = 0; i < 10; ++i) {
    doc::DocumentStore& st = u.ctx.store();
    auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "More " + std::to_string(i));
    REQUIRE(t.ok());
    REQUIRE(st.dispatch(t->command).ok());
  }
  u.d.step();
  u.d.tree().layoutNow();
  arr.scrollToY(arr.rowLaneRect(3).y - arr.rowLaneRect(0).y);
  u.d.tree().layoutNow();
  const ui::IRect vp = arr.lanesViewport();
  int32_t firstShown = -1;
  for (size_t i = 0; i < arr.rows().size() && firstShown < 0; ++i)
    if (arr.rowLaneRect(static_cast<int32_t>(i)).bottom() > vp.y) firstShown = static_cast<int32_t>(i);
  REQUIRE(firstShown >= 1);
  CHECK(u.d.press("Ctrl+Alt+P") == KeyResult::Action);
  u.d.advance(ProjectPatchView::kMorphSeconds * 2.0);
  REQUIRE(v.shown());
  {
    const ProjectPatch& pp = v.patch();
    const Band& b = pp.bands[pp.bandOf(arr.rows()[static_cast<size_t>(firstShown)].id)];
    const float top = v.canvas().toDevice({b.x0, b.kind == doc::TrackKind::Group ? b.cy0 : b.y}).y;
    const IRect cr = v.canvas().rect();
    CHECK(top > static_cast<float>(cr.y));
    CHECK(top < static_cast<float>(cr.y) + 64.0f);
  }
}

TEST_CASE("project patch: a wire, a re-route, add / remove / move devices, mixer changes — one undo step each, shown in the Ableton view") {
  Ui u;
  u.show();
  doc::DocumentStore& st = u.ctx.store();
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  const doc::TrackId keys = u.track("Keys").id, pad = u.track("Pad").id, mod = u.track("Mod").id, grp = u.track("Group").id;
  const doc::TrackId space = u.track("Space").id;
  const doc::DeviceId lfo = firstGroup(u.track("Mod")), eq = u.onPath("Pad", 1);

  SUBCASE("wire: an export dragged onto a device, a param chosen") {
    u.zoomTo("Pad");
    u.view().fitAll(false);
    u.d.tree().layoutNow();
    const uint32_t lfoNode = u.deviceNode(lfo), eqNode = u.deviceNode(eq);
    u.d.drag(u.outPin(lfoNode, "y"), u.centre(eqNode), 6);
    // (GU.11: onto a module's panel — a menu of what it takes when there is a choice, else the control under the drop)
    if (Menu* m = u.openMenu()) {
      MESSAGE("first menu item: " << m->items().at(0).label);
      m->choose(0);
    }
    u.d.step();
    CHECK(u.history() == h0 + 1);
    // GU.11: Pad's VCA is a module — the wire ends on the block of its group the menu item names (its control's value
    // block, or the group's main input).
    const doc::Wire made0 = doc::wiresOf(*u.ctx.doc()).back();
    CHECK(made0.from == lfo);
    const doc::Device* target = doc::findDevice(*u.ctx.doc(), made0.to).device;
    REQUIRE(target != nullptr);
    CHECK((made0.to == eq || doc::groupContains(u.track("Pad").devices, eq, made0.to)));
    CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
    // The Device view (Ableton view) draws the new wire's cable into Pad's VCA (docked on its knob, or on the
    // panel's frame).
    const doc::WireId made = made0.id;
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
    // TX.mixer-look: the band's empty space (no block, cable or trace: the mixer's buses leave its top right).
    const Vec2 empty = u.emptyIn(pb);
    u.d.click(empty, mod::kNone, PointerButton::Right);
    Menu* m = u.openMenu();
    REQUIRE(m != nullptr);
    m->setHighlighted(0);
    Menu* sub = m->openSubmenu();
    REQUIRE(sub != nullptr);
    u.choose(*sub, "Utility");
    CHECK(u.history() == h0 + 1);
    REQUIRE(doc::mainPath(*u.ctx.doc(), u.track("Pad")).blocks.size() == 3);
    const doc::DeviceId util = u.onPath("Pad", 2);
    CHECK(doc::findDevice(*u.ctx.doc(), util).device->name == "Utility");
    // The Device view shows it.
    u.ctx.setSelectedTrack(pad);
    DeviceChainView* chain = findDeviceChain(*u.shell);
    REQUIRE(chain != nullptr);
    CHECK(chain->frameOf(util) != nullptr);
    // Move within the band: drag the Utility block left of the EQ Eight (TX.panel-faces: by its panel's title bar; the
    // band framed again, the new panel widened it past the window). TX.all-free: a block is placed anywhere — the move
    // is view state (a nudge), the cables keep the path as it was.
    u.zoomTo("Pad");
    const std::vector<const doc::Device*> before = doc::mainPath(*u.ctx.doc(), u.track("Pad")).blocks;
    const Node& eqn = u.pp().nodes[u.deviceNode(eq)];
    u.d.drag(u.grip(u.deviceNode(util)), u.dev(eqn.x + 4.0f, eqn.y + eqn.h * 0.5f), 8);
    CHECK(u.history() == h0 + 1);
    CHECK(doc::mainPath(*u.ctx.doc(), u.track("Pad")).blocks == before);
    CHECK(u.ctx.store().view().patchNudges.count(util.value) == 1);
    // Move to another band: onto Keys (one step: it leaves the Pad's path, joining its neighbours).
    u.view().fitAll(false);
    u.d.tree().layoutNow();
    const uint32_t kb = u.pp().bandOf(keys);
    const Node& kmix = u.pp().nodes[u.pp().bands[kb].mixer];
    u.d.drag(u.grip(u.deviceNode(util)), u.dev(kmix.x - 8.0f, kmix.y + 12.0f), 8);
    CHECK(u.history() == h0 + 2);
    CHECK(doc::findDevice(*u.ctx.doc(), util).track->id == keys);
    CHECK(doc::mainPath(*u.ctx.doc(), u.track("Pad")).complete);
    // 0 deactivates the selected block, Delete removes it.
    // (Framed on Keys first: zoomed out to the whole project, once TX.mixer-look's bigger mixer pushed the dropped block
    // elsewhere, the click on its title bar opened a menu instead — at that zoom a pin's reach covers the grip.)
    u.zoomTo("Keys");
    u.d.click(u.grip(u.deviceNode(util)));
    REQUIRE(u.view().selectedDevices() == std::vector<doc::DeviceId>{util});
    CHECK(u.d.press("0") != KeyResult::Unhandled);
    CHECK(u.history() == h0 + 3);
    CHECK_FALSE(doc::findDevice(*u.ctx.doc(), util).device->active);
    CHECK(u.d.press("Delete") != KeyResult::Unhandled);
    CHECK(u.history() == h0 + 4);
    CHECK(doc::findDevice(*u.ctx.doc(), util).device == nullptr);
    for (int k = 0; k < 4; ++k) REQUIRE(u.ctx.undo());
    CHECK(u.ctx.doc() == start);
  }
  SUBCASE("mixer: mute and solo clicks, a volume drag — one step each; a value drag only repaints") {
    u.zoomTo("Keys");
    const uint32_t kb = u.pp().bandOf(keys);
    const uint32_t mixer = u.pp().bands[kb].mixer;
    u.d.click(u.mixerMute(mixer));  // M (TX.mixer-look: on the face's header)
    CHECK(u.history() == h0 + 1);
    CHECK(u.track("Keys").mixer.mute);
    u.d.click(u.mixerSolo(u.pp().bands[u.pp().bandOf(keys)].mixer));  // S
    CHECK(u.history() == h0 + 2);
    CHECK(u.track("Keys").mixer.solo);
    const uint32_t mixer2 = u.pp().bands[u.pp().bandOf(keys)].mixer;
    const uint64_t rebuilds = u.view().stats().rebuilds;
    const size_t subs = u.view().subLanes().size();
    const float vol = u.track("Keys").mixer.volumeDb;
    const Vec2 a = u.mixerFader(mixer2, static_cast<float>(volumeToFader(vol))), b = {a.x - 40.0f, a.y};  // the cap, left
    u.d.drag(a, b, 6);
    CHECK(u.history() == h0 + 3);
    CHECK(u.track("Keys").mixer.volumeDb < vol);
    CHECK(st.lastChanges().paramOnly());
    // Param changes repaint; no lowering, no new scene. TL.2: the press on the fader showed Keys' volume sub-lane (one
    // re-layout, the lowering kept); a second drag re-lays out nothing.
    CHECK(u.view().subLanes().size() == subs + 1);
    CHECK(u.view().stats().rebuilds == rebuilds + 1);
    const uint64_t rebuilds2 = u.view().stats().rebuilds;
    const Vec2 a2 = u.mixerFader(u.pp().bands[u.pp().bandOf(keys)].mixer, static_cast<float>(volumeToFader(u.track("Keys").mixer.volumeDb)));
    u.d.drag(a2, {a2.x + 10.0f, a2.y}, 4);
    CHECK(u.history() == h0 + 4);
    CHECK(u.view().stats().rebuilds == rebuilds2);
    REQUIRE(u.ctx.undo());
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
    size_t drawnBefore = 0;  // TX.all-free: beside the main paths' links (GU.11: the modules hold wires of their own)
    for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc())) drawnBefore += w.mainLink() ? 0 : 1;
    for (uint32_t w = 0; w < ps.modelEdge.size(); ++w)  // (one of the LFO's)
      if (ps.modelEdge[w] != kNone && u.pp().edges[ps.modelEdge[w]].kind == EdgeKind::Modulation)
        if (const doc::Wire* dw = doc::findWire(*u.ctx.doc(), u.pp().edges[ps.modelEdge[w]].wire); dw != nullptr && dw->from == lfo) {
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
    size_t drawn = 0;  // TX.all-free: beside the main paths' links
    for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc())) drawn += w.mainLink() ? 0 : 1;
    CHECK(drawn == drawnBefore - 1);
    REQUIRE(u.ctx.undo());
    CHECK(u.ctx.doc() == start);
  }
}

TEST_CASE("project patch cursors (T4.16c): blocks Move, out pins Crosshair, M / S Pointer, bars ResizeEW; a refused drop NotAllowed") {
  Ui u;
  u.show();
  const doc::TrackId keys = u.track("Keys").id, mod = u.track("Mod").id;
  const doc::DeviceId lfo = firstGroup(u.track("Mod")), eq = u.onPath("Pad", 1);
  auto at = [&](Vec2 p) {
    u.d.move(p);
    return u.d.tree().cursor();
  };
  // A mixer strip: M and S are buttons, the volume fader a sideways drag (also while it runs); TX.mixer-look: the pan
  // knob an up / down drag, the face's frame moves the mixer.
  u.zoomTo("Keys");
  const uint32_t mn = u.pp().bands[u.pp().bandOf(keys)].mixer;
  CHECK(at(u.mixerMute(mn)) == Cursor::Pointer);
  CHECK(at(u.mixerSolo(mn)) == Cursor::Pointer);
  CHECK(at(u.mixerPan(mn)) == Cursor::ResizeNS);
  CHECK(at(u.mixerFrame(mn)) == Cursor::Move);
  const Vec2 vol = u.mixerFader(mn, 0.5f);
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
  {  // (GU.11: Pad's instrument group, open: its capsule's title strip moves it)
    const Node& g = u.pp().nodes[u.deviceNode(u.onPath("Pad", 0))];
    CHECK(at(u.dev(g.x + 30.0f, g.y + 5.0f)) == Cursor::Move);
  }
  CHECK(at(u.outPin(u.deviceNode(lfo), "y")) == Cursor::Crosshair);
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
  {  // the Saturator (a module) closed: drawn as its group's panel
    const uint32_t sn = v.scene().sceneNode[v.patch().nodeOf(u.onPath("Keys", 1))];
    CHECK(v.canvas().panelOn(sn) != nullptr);
  }
  // Open Keys' Saturator (a group) to its blocks: drawn inside its capsule, the view zooms onto it.
  const doc::DeviceId sat = u.onPath("Keys", 1);
  const size_t nodes = v.scene().scene->nodes.size();
  v.setDeviceExpanded(sat, true);
  u.d.advance(0.5);
  CHECK(v.deviceExpanded(sat));
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
  v.setDeviceExpanded(sat, false);
  u.d.advance(0.5);
  CHECK(v.scene().scene->nodes.size() == nodes);
  // Its menu's Open in Circuit View opens its band here, framed on it and focused (T5.x-one-circuit: one circuit view).
  REQUIRE(v.openNodeMenu(v.patch().nodeOf(sat), u.centre(v.patch().nodeOf(sat))) != nullptr);
  Menu* m = u.openMenu();
  REQUIRE(m != nullptr);
  u.choose(*m, "Open in Circuit View");
  u.d.advance(0.5);
  CHECK(v.focusedDevice() == sat);
  CHECK(v.openedBand() == u.track("Keys").id);
  CHECK(u.shell->overlay().childCount() == 0);
  // (GU.11: no device-level circuit editor: a module opens in place as a group.)
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

// WI.3: an output's cable whose DECLARED type clashes with the input under the pointer (a value block's real `y` onto an
// `and` block's bool input) shows the no-drop state (NotAllowed, not snapped), the status line says why while hovering,
// and the release connects nothing; a bool output onto the same kind of input connects.
TEST_CASE("project patch (WI.3): a real output onto a bool input is refused at the drop; bool onto bool connects") {
  Ui u;
  u.show();
  doc::DocumentStore& st = u.ctx.store();
  const doc::TrackId keys = u.track("Keys").id;
  auto add = [&](std::string patch) {
    auto e = doc::ops::addDevice(*st.current(), st.ids(), keys, std::move(patch), doc::DeviceKind::AudioEffect, doc::ops::kOffPath);
    REQUIRE(e.ok());
    REQUIRE(st.dispatch(e->command).ok());
    return e->devices.at(0);
  };
  const doc::DeviceId value = add(std::string(doc::kValueDevice));
  const doc::DeviceId and1 = add("atom:and"), and2 = add("atom:and");
  u.zoomTo("Keys");
  u.d.step();
  auto inPin = [&](doc::DeviceId d, std::string_view name) {
    const uint32_t node = u.deviceNode(d);
    const int32_t k = u.pp().nodes[node].input(name);
    REQUIRE(k >= 0);
    return u.view().canvas().toDevice(u.view().canvas().portWorld(node, static_cast<uint32_t>(k), true));
  };
  const size_t h0 = u.history();
  const size_t wires0 = doc::wiresOf(*u.ctx.doc()).size();
  {
    const Vec2 from = u.outPin(u.deviceNode(value), "y"), to = inPin(and1, "a");
    u.d.down(from);
    for (int i = 1; i <= 8; ++i)
      u.d.move({from.x + (to.x - from.x) * static_cast<float>(i) / 8.0f, from.y + (to.y - from.y) * static_cast<float>(i) / 8.0f});
    CHECK(u.view().canvas().typeClashAt(to) == doc::WireClash::RealIntoBool);
    CHECK(u.d.tree().cursor() == Cursor::NotAllowed);
    CHECK(u.ctx.status() == doc::wireClashText(doc::WireClash::RealIntoBool));
    CHECK_FALSE(u.view().canvas().looseCableSnapped());  // not drawn onto the pin
    u.d.up(to);
    CHECK(u.history() == h0);
    CHECK(doc::wiresOf(*u.ctx.doc()).size() == wires0);
    CHECK(u.ctx.status() == doc::wireClashText(doc::WireClash::RealIntoBool));
  }
  {  // a bool (`and`'s out) onto a bool input: connected
    const Vec2 from = u.outPin(u.deviceNode(and1), "out"), to = inPin(and2, "b");
    u.d.down(from);
    for (int i = 1; i <= 8; ++i)
      u.d.move({from.x + (to.x - from.x) * static_cast<float>(i) / 8.0f, from.y + (to.y - from.y) * static_cast<float>(i) / 8.0f});
    CHECK(u.view().canvas().typeClashAt(to) == doc::WireClash::None);
    CHECK(u.d.tree().cursor() == Cursor::Crosshair);
    u.d.up(to);
    CHECK(u.history() == h0 + 1);
    REQUIRE(doc::wiresOf(*u.ctx.doc()).size() == wires0 + 1);
    const doc::Wire& w = doc::wiresOf(*u.ctx.doc()).back();
    CHECK(w.from == and1);
    CHECK(w.to == and2);
    CHECK(w.param == "b");
  }
}

// TMM.10-fix2 (SPEC §15 2026-11-01 "the note source is a Hz piano roll"): a MIDI track's lane (TL.1) is its note source
// ("Notes"); its note outputs (Hz, gate, velocity: its port chips) are wire sources like exports: Crosshair on them, a drop on a device a
// param menu (one undo step), a drop on a param pin a wire already drives that param, the wire drawn from the window
// (per voice) and read on hover, the chain's own `notes` pin and a drop off the devices refused (NotAllowed, status).
TEST_CASE("project patch (TMM.10-fix2): a note output dragged onto a device wires it — menu, undo, drawn, readable, refusals") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());  // TX.mod-all: the canvas paints (the panels lay out: a drop onto a knob)
  u.show();
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  const doc::TrackId keys = u.track("Keys").id;
  const doc::DeviceId sat = u.onPath("Keys", 1);
  u.zoomTo("Keys");
  u.frameLane("Keys");  // TL.1: from the lane's left end, its port chips readable
  const uint32_t src = u.pp().bands[u.pp().bandOf(keys)].source;
  const Node& sn = u.pp().nodes[src];
  CHECK(sn.title == "Notes");
  CHECK(sn.kind == NodeKind::Lane);
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
    CHECK(infoView->body().find("lane") != std::string::npos);  // TL.1: the note source is the track's lane
  }
  // velocity → the Saturator's Drive (GU.11: a module, drawn as its panel; its controls are value blocks: the drop on
  // the knob wires that control, no menu), one step.
  const doc::DeviceId drive = test_blocks::member(*u.ctx.doc(), sat, "Drive");
  REQUIRE(drive.valid());
  u.d.advance(0.5);  // the panel is laid out at this zoom
  auto driveKnob = [&]() -> Vec2 {
    const uint32_t sn = u.deviceNode(sat);
    auto* pf = dynamic_cast<const panel::GroupPanelFace*>(u.view().canvas().panelOn(u.view().scene().sceneNode[sn]));
    REQUIRE(pf != nullptr);
    const std::optional<ControlAnchor> k = pf->bindingAnchor(doc::controlBinding(drive, std::string(doc::kValueParam)), true);
    REQUIRE(k.has_value());
    const Node& tn = u.pp().nodes[sn];
    return u.dev(tn.x + k->at.x * tn.scale, tn.y + k->at.y * tn.scale);
  };
  size_t drawnBefore = 0;  // TX.all-free: beside the main paths' links (the modules hold wires of their own)
  for (const doc::Wire& x : doc::wiresOf(*u.ctx.doc())) drawnBefore += x.mainLink() ? 0 : 1;
  u.d.drag(u.outPin(src, "velocity"), driveKnob(), 6);
  CHECK(u.openMenu() == nullptr);
  CHECK(u.history() == h0 + 1);
  size_t drawnWires = 0;
  for (const doc::Wire& x : doc::wiresOf(*u.ctx.doc())) drawnWires += x.mainLink() ? 0 : 1;
  REQUIRE(drawnWires == drawnBefore + 1);  // this one
  const doc::Wire w = doc::wiresOf(*u.ctx.doc()).back();
  CHECK(w.noteSource == keys);
  CHECK(w.port == "velocity");
  CHECK(w.to == drive);
  CHECK(w.param == doc::kValueParam);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Drawn from the window's velocity pin onto the Saturator's block (its Drive knob), per voice, lowered.
  uint32_t edge = kNone;
  for (uint32_t k = 0; k < u.pp().edges.size(); ++k)
    if (u.pp().edges[k].kind == EdgeKind::Modulation && u.pp().edges[k].wire == w.id) edge = k;
  REQUIRE(edge != kNone);
  const Edge& e = u.pp().edges[edge];
  CHECK(e.from == src);
  CHECK(u.pp().nodes[src].outputs[e.fromPort].name == "velocity");
  CHECK(e.to == u.deviceNode(sat));
  CHECK(e.voice);
  CHECK(e.status == doc::WireStatus::Lowered);
  CHECK(routedAsDrawn(e));
  u.view().updateReadout(edge);
  CHECK(u.view().readout().title.find("Keys notes: velocity") != std::string::npos);
  // TX.knob-curves: an added wire says what it adds in (knob travel, the knob's units, dB: the target knob's curve).
  CHECK(u.view().readout().value.find("adds in ") != std::string::npos);
  u.view().updateReadout(kNone);
  // Hz onto the same knob: that control directly (no menu), one step. TX.mod-all: the param has no pin — the wire ends
  // on the knob (its port anchored there), and dropping onto the knob wires it.
  {
    const Node& tn = u.pp().nodes[e.to];
    const Port& port = tn.inputs[e.toPort];
    CHECK(port.anchored);
    const Vec2 pin = driveKnob();
    const size_t into = doc::ops::wiresInto(*u.ctx.doc(), drive, w.param).size();  // the sketch's LFO drives it too
    u.d.drag(u.outPin(src, "hz"), pin, 6);
    CHECK(u.openMenu() == nullptr);
    CHECK(u.history() == h0 + 2);
    CHECK(doc::ops::wiresInto(*u.ctx.doc(), drive, w.param).size() == into + 1);
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
  auto ua = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), keys, "module:utility", doc::DeviceKind::AudioEffect);
  REQUIRE(ua.ok());
  REQUIRE(u.ctx.store().dispatch(ua->command).ok());
  const doc::DeviceId inst = u.onPath("Keys", 0), util = ua->devices.at(0);
  u.show();
  u.view().setGroupOpen(inst, false);  // (GU.11: the instrument group closed: one block, its ports on its outline)
  u.d.tree().layoutNow();
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  // Add Free Device: one step, out of the series, under the chain.
  REQUIRE(u.view().addFreeDevice(keys, "meta:envelope"));  // (GU.4: was a Phase accumulator)
  u.d.step();
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId pa = u.track("Keys").devices.back()->id;
  REQUIRE(doc::isMetaDevice(*u.track("Keys").devices.back()));
  CHECK(all_free::offPath(*u.ctx.doc(), pa));
  CHECK(u.onPath("Keys", 0) == inst);  // the instrument still first: nothing replaced
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  u.zoomTo("Keys");
  const Node& pn = u.pp().nodes[u.deviceNode(pa)];
  const Node& un = u.pp().nodes[u.deviceNode(util)];
  CHECK(pn.offPath);
  CHECK(pn.y >= un.y + un.h);  // its row is under the chain's
  for (const Edge& e : u.pp().edges) CHECK_FALSE((e.kind == EdgeKind::Chain && (e.from == u.deviceNode(pa) || e.to == u.deviceNode(pa))));
  // The accumulator's output dropped on the Utility: its menu offers its inputs too; its audio input wires it there.
  u.d.drag(u.outPin(u.deviceNode(pa), "out"), u.centre(u.deviceNode(util)), 6);
  Menu* m = u.openMenu();
  REQUIRE(m != nullptr);
  MESSAGE("first item: " << m->items().at(0).label);
  u.choose(*m, "Input: Audio In");
  CHECK(u.history() == h0 + 2);
  const doc::Wire w1 = doc::wiresOf(*u.ctx.doc()).back();
  CHECK(w1.from == pa);
  CHECK(w1.port.empty());  // TX.all-free: "out" is the accumulator's main output, "x" the Utility's main input (kMainPort)
  CHECK(w1.to == util);
  CHECK(w1.param.empty());
  CHECK(all_free::offPath(*u.ctx.doc(), pa));  // a main link into the path from off it: a sum into that input
  CHECK(w1.law == doc::WireLaw::Add);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // The instrument's main output onto the Envelope face's sustain control: that param, no menu.
  {
    const uint32_t node = u.deviceNode(pa);
    const uint32_t sn = u.view().scene().sceneNode[node];
    MetaFace* face = u.view().canvas().faceOn(sn);
    REQUIRE(face != nullptr);
    const std::optional<ui::Rect> r = face->paramRect(static_cast<size_t>(face->type().paramIndex("sustain")));
    REQUIRE(r.has_value());
    const Node& n = u.pp().nodes[node];
    const Vec2 on = u.dev(n.x + 0.5f * (r->x0 + r->x1), n.y + 0.5f * (r->y0 + r->y1));
    u.d.drag(u.outPin(u.deviceNode(inst), "y"), on, 6);
    CHECK(u.openMenu() == nullptr);
    CHECK(u.history() == h0 + 3);
    const doc::Wire w2 = doc::wiresOf(*u.ctx.doc()).back();
    CHECK(w2.from == inst);
    CHECK(w2.port.empty());  // the instrument's main output
    CHECK(w2.to == pa);
    CHECK(w2.param == "sustain");
  }
  // (GU.11: the instrument is a group: its main input takes any signal — the stock instrument's `notes` pin that refused
  // audio is gone.)
  // The Utility dragged down out of its place: TX.all-free — placed there, nothing rewired (a nudge, no step); its
  // block menu's Take Off Path takes it off (one step; the instrument joined to the mixer), its other wires stay.
  {
    const Node& n = u.pp().nodes[u.deviceNode(util)];
    const Vec2 from = u.centre(u.deviceNode(util));
    const Vec2 below = u.dev(n.x + 0.5f * n.w, n.y + n.h + 3.0f * metrics::kFreeRowGap);
    u.d.drag(from, below, 8);
    u.d.step();
    CHECK(u.history() == h0 + 3);
    CHECK_FALSE(all_free::offPath(*u.ctx.doc(), util));
    REQUIRE(u.view().takeOffPath(std::vector<doc::DeviceId>{util}));
    u.d.step();
    CHECK(u.history() == h0 + 4);
    CHECK(all_free::offPath(*u.ctx.doc(), util));
    size_t drawn = 0;
    for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc())) drawn += w.from == pa || w.to == pa ? 1 : 0;
    CHECK(drawn == 2);
    CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  }
  for (int i = 0; i < 4; ++i) REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
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
  const doc::DeviceId sat = u.onPath("Keys", 1);
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
  auto e = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.track("Pad").id, "module:utility", doc::DeviceKind::AudioEffect);
  REQUIRE(e.ok());
  REQUIRE(u.ctx.store().dispatch(e->command).ok());
  CHECK(v.stats().rebuilds == r0);  // hidden: nothing built
  u.show();
  CHECK(v.stats().rebuilds == r0 + 1);
  auto e2 = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.track("Pad").id, "module:saturator", doc::DeviceKind::AudioEffect);
  REQUIRE(e2.ok());
  REQUIRE(u.ctx.store().dispatch(e2->command).ok());
  CHECK(v.stats().rebuilds == r0 + 2);
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");
}

TEST_CASE("project patch: 64 tracks build, lay out and match their graph" * doctest::test_suite("slow")) {
  doc::DocumentStore store;
  REQUIRE(buildWideProject(store, 64).ok());
  const doc::LoweredProject low = lowerOf(*store.current());
  const ProjectPatch pp = buildProjectPatch(*store.current(), low, &library());
  CHECK(checkAgainstGraph(pp, *store.current(), low) == "");
  CHECK(pp.bands.size() == 64 + 8 + 2 + 1);
}


// ---- T5.x-one-circuit: one circuit view, lane windows ---------------------------------------------------------------

TEST_CASE("one circuit: from a device's opened band, zooming out alone reaches every lane, group, return, the master and Out") {
  Ui u;
  const doc::DeviceId sat = u.onPath("Keys", 1);
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

TEST_CASE("lanes (TL.1): every band ends in its lane, over one x range, showing its clips over the shared time window") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  const ProjectPatch& pp0 = v.patch();
  REQUIRE(pp0.laneX1 > pp0.laneX0);
  // Every band (MIDI, return, master) ends in a lane inside it, at its bottom, all over the same x range; BR.input-port:
  // a group's band has none (its members' buses into its input socket feed it).
  for (const Band& b : pp0.bands) {
    CAPTURE(b.name);
    if (b.kind == doc::TrackKind::Group) {
      CHECK(b.laneH == 0.0f);
      CHECK(b.subH == 0.0f);
      CHECK(b.input != kNone);
      for (const uint32_t n : b.devices) CHECK(pp0.nodes[n].y + pp0.nodes[n].h <= b.y + b.h);
      continue;
    }
    CHECK(b.laneH == metrics::kLaneH);
    CHECK(b.laneY + b.laneH + metrics::kLaneGapBelow == doctest::Approx(b.y + b.h));
    CHECK(b.x0 <= pp0.laneX0);
    CHECK(b.x1 >= pp0.laneX1);
    for (const uint32_t n : b.devices) CHECK(pp0.nodes[n].y + pp0.nodes[n].h <= b.laneY);  // the circuit above it
    // A MIDI / audio track's source IS its lane (no block); a group, return, master keeps its Σ.
    if (hasClipRow(b)) {
      const Node& s = pp0.nodes[b.source];
      CHECK(s.kind == NodeKind::Lane);
      CHECK(s.x == pp0.laneX0);
      CHECK(s.y == b.laneY);
      CHECK(s.w == pp0.laneX1 - pp0.laneX0);
      CHECK(v.scene().scene->nodes[v.scene().sceneNode[b.source]].hidden);
    } else {
      CHECK(pp0.nodes[b.source].kind == NodeKind::Sum);
    }
  }
  auto check = [&] {
    const doc::Project& q = *u.ctx.doc();
    const ProjectPatch& pp = v.patch();
    const double a = v.laneStart(), e = v.laneStart() + v.laneSpan();
    for (uint32_t bi = 0; bi < pp.bands.size(); ++bi) {
      const Band& b = pp.bands[bi];
      if (!hasClipRow(b)) {
        CHECK(v.laneClips(bi).empty());
        continue;
      }
      const doc::Track* t = doc::findTrackPtr(q, b.track);
      REQUIRE(t != nullptr);
      const LaneRect r = laneBody(pp, b);
      const double k = static_cast<double>(r.x1 - r.x0) / v.laneSpan();
      std::vector<ProjectPatchView::LaneClip> want;
      for (const doc::ClipSlot& c : t->clips)
        if (static_cast<double>(c.end()) > a && static_cast<double>(c.start()) < e)
          want.push_back({c.id, r.x0 + static_cast<float>((std::max(static_cast<double>(c.start()), a) - a) * k),
                          r.x0 + static_cast<float>((std::min(static_cast<double>(c.end()), e) - a) * k), c.isAudio()});
      const std::vector<ProjectPatchView::LaneClip> got = v.laneClips(bi);
      REQUIRE(got.size() == want.size());
      for (size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i].clip == want[i].clip);
        CHECK(got[i].x0 == doctest::Approx(want[i].x0));
        CHECK(got[i].x1 == doctest::Approx(want[i].x1));
      }
    }
  };
  const doc::Project& p = *u.ctx.doc();
  const Ticks bar = ticksPerBar(p.timeline->sigMap.signatureAt(0));
  v.setLaneTime(0.0, static_cast<double>(4 * bar));
  check();
  const uint32_t keys = v.patch().bandOf(u.track("Keys").id), pad = v.patch().bandOf(u.track("Pad").id);
  CHECK(v.laneClips(keys).size() == 1);  // Riff (bars 1–4); Riff 2 starts at bar 7
  CHECK(v.laneClips(pad).size() == 1);
  // Over a lane (real dispatch): Shift+wheel scrolls time (an eighth of the span per notch), Ctrl+wheel zooms it about the
  // pointer; the lanes keep their width (no re-layout), the canvas does not pan; the arrangement shows the same range.
  const uint64_t relayouts = v.stats().relayouts;
  u.zoomTo("Keys");
  const Band& kb = v.patch().bands[keys];
  const Vec2 over = u.dev((v.patch().laneX0 + v.patch().laneX1) * 0.5f, kb.laneY + kb.laneH * 0.5f);
  REQUIRE(v.laneAt(over) == keys);
  const circuit::CircuitCanvas::View before = v.canvas().view();
  (void)u.d.wheel(over, -8.0f, 0.0f, mod::kShift);  // eight notches towards the end: one span later
  CHECK(v.laneStart() == doctest::Approx(static_cast<double>(4 * bar)));
  CHECK(v.canvas().view() == before);
  CHECK(v.stats().relayouts == relayouts);
  check();
  CHECK(v.laneClips(keys).size() == 1);  // Riff 2 (bars 7–10) now
  CHECK(v.laneClips(keys)[0].clip == u.track("Keys").clips[1].id);
  CHECK(u.arrangement().originTick() == doctest::Approx(v.laneStart()));
  const float w0 = v.patch().laneX1 - v.patch().laneX0;
  const double at = v.laneTickAt(over.x);
  (void)u.d.wheel(over, -3.0f, 0.0f, mod::kCtrl);  // zoomed out about the pointer
  CHECK(v.laneSpan() == doctest::Approx(static_cast<double>(4 * bar) * std::pow(ProjectPatchView::kLaneZoomStep, 3.0)));
  CHECK(v.laneTickAt(over.x) == doctest::Approx(at));
  CHECK(v.patch().laneX1 - v.patch().laneX0 == w0);
  CHECK(v.stats().relayouts == relayouts);
  CHECK(v.canvas().view() == before);
  check();
  // The plain wheel over a lane pans the canvas (as the arrangement's plain wheel scrolls its tracks).
  const double s0 = v.laneStart();
  (void)u.d.wheel(over, -1.0f);
  CHECK(v.laneStart() == s0);
  CHECK_FALSE(v.canvas().view() == before);
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");
}

TEST_CASE("lanes (TL.1): one time window with the arrangement — set in either, shown by both on their own widths") {
  Ui u;
  const Ticks bar = ticksPerBar(u.ctx.doc()->timeline->sigMap.signatureAt(0));
  ArrangementView& arr = u.arrangement();
  arr.setTimeView(static_cast<double>(5 * bar + bar / 3), arr.pxPerTick());
  u.show();
  ProjectPatchView& v = u.view();
  const double logicalW = static_cast<double>(arr.lanesViewport().w) / static_cast<double>(u.d.tree().scale());
  CHECK(v.laneStart() == doctest::Approx(static_cast<double>(5 * bar + bar / 3)));
  CHECK(v.laneSpan() == doctest::Approx(logicalW / arr.pxPerTick()));
  // Zoomed in the patch view (hidden arrangement): the arrangement follows — same start, its px per tick from the span.
  v.zoomLanes(2.0, 0.0f);
  CHECK(arr.originTick() == doctest::Approx(v.laneStart()));
  CHECK(arr.pxPerTick() == doctest::Approx(logicalW / v.laneSpan()));
  // Back in the arrangement: the range the patch view left; its own zoom moves the lanes.
  v.setShown(false, false);
  u.d.tree().layoutNow();
  CHECK(arr.originTick() == doctest::Approx(v.laneStart()));
  const double span = v.laneSpan();
  arr.zoomIn();
  CHECK(v.laneSpan() == doctest::Approx(span * 0.5));
  CHECK(arr.pxPerTick() == doctest::Approx(logicalW / v.laneSpan()));
}

TEST_CASE("lanes (TL.1): one ruler over the lanes — its bar lines on the lanes' axis; a click locates; Ctrl+wheel zooms") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  Widget* ruler = v.ruler();
  REQUIRE(ruler != nullptr);
  CHECK(ruler->visible());
  const IRect cr = v.canvas().rect();
  CHECK(ruler->rect().y == cr.y);  // pinned to the canvas's top
  CHECK(ruler->rect().w == cr.w);
  const Ticks bar = ticksPerBar(u.ctx.doc()->timeline->sigMap.signatureAt(0));
  v.setLaneTime(0.0, static_cast<double>(8 * bar));
  u.zoomTo("Keys");
  // A click on the ruler over the lanes, at bar 3: the start marker (and, stopped, the playhead) there — the lanes' axis.
  const ProjectPatch& pp = v.patch();
  const Rect lanes = v.canvas().deviceRect(pp.laneX0, 0.0f, pp.laneX1 - pp.laneX0, 0.0f);
  const float x3 = lanes.x0 + (lanes.x1 - lanes.x0) * 2.0f / 8.0f;
  REQUIRE(x3 > static_cast<float>(cr.x));
  REQUIRE(x3 < static_cast<float>(cr.right()));
  const Vec2 onRuler{x3 + 0.5f, static_cast<float>(ruler->rect().y) + 4.0f};
  u.d.click(onRuler);
  u.d.step();
  CHECK(u.ctx.startMarker() == 2 * bar);
  CHECK(u.port.playhead().tick == 2 * bar);
  // Ctrl+wheel over the ruler zooms the shared window.
  const double span = v.laneSpan();
  (void)u.d.wheel(onRuler, 1.0f, 0.0f, mod::kCtrl);
  CHECK(v.laneSpan() == doctest::Approx(span / ProjectPatchView::kLaneZoomStep));
  // Off the lanes' x range the ruler takes nothing: a press there is the canvas's.
  if (lanes.x0 - 20.0f > static_cast<float>(cr.x)) {
    const Ticks marker = u.ctx.startMarker();
    u.d.click({lanes.x0 - 10.0f, static_cast<float>(ruler->rect().y) + 4.0f});
    u.d.step();
    CHECK(u.ctx.startMarker() == marker);
  }
}

TEST_CASE("lanes (TL.1): idle costs nothing; the playhead repaints only the lanes' columns and pages the shared window") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  const Ticks bar = ticksPerBar(u.ctx.doc()->timeline->sigMap.signatureAt(0));
  v.setLaneTime(0.0, static_cast<double>(4 * bar));
  u.zoomTo("Pad");
  u.d.advance(0.5);
  const uint64_t frames0 = u.d.framesDrawn();
  u.d.advance(5.0);  // stopped: no frames, no timers
  CHECK(u.d.framesDrawn() == frames0);
  CHECK(std::isinf(u.d.tree().nextWakeTime()));
  u.ctx.setStartMarker(bar);
  u.ctx.togglePlay();  // plays from the start marker through the fake port, notifies kTransport
  u.d.advance(0.2);
  REQUIRE(v.lanePlayhead().has_value());
  u.d.step();
  // A move: the damage is the old and new playhead columns of the lanes, the ruler's strip (its playhead line) and the
  // control bar's readout.
  u.port.setPlayheadTick(*v.lanePlayhead() + bar / 2);
  u.d.tree().update(u.d.now() + 0.15);
  const IRect bar0 = u.d.rect("main/control-bar");
  std::vector<Rect> lanes;
  for (const Band& b : v.patch().bands) {
    const LaneRect r = laneStrip(v.patch(), b);  // TL.4: its ruler strip too (the playhead runs through it)
    lanes.push_back(v.canvas().deviceRect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0 + b.subH));
  }
  const IRect ru = v.ruler()->rect();
  size_t columns = 0;
  for (const ClipRect& r : u.d.tree().damage().rects()) {
    if (r.y1 <= bar0.bottom()) continue;  // the control bar's position readout
    if (r.y0 >= ru.y && r.y1 <= ru.bottom()) continue;  // the ruler
    // TL.2: every lane's column is damaged (a group / return / master lane draws the playhead too), so the damage list may
    // merge the columns of lanes above one another into one column from the first lane's top to the last one's bottom.
    bool inLane = false;
    float top = 1e9f, bottom = -1e9f;
    for (const Rect& w : lanes) {
      inLane = inLane || (static_cast<float>(r.x0) >= std::floor(w.x0) - 1.0f && static_cast<float>(r.x1) <= std::ceil(w.x1) + 1.0f &&
                          static_cast<float>(r.y0) >= std::floor(w.y0) - 1.0f && static_cast<float>(r.y1) <= std::ceil(w.y1) + 1.0f);
      top = std::min(top, std::floor(w.y0) - 1.0f);
      bottom = std::max(bottom, std::ceil(w.y1) + 1.0f);
    }
    inLane = inLane || (static_cast<float>(r.x0) >= std::floor(lanes.front().x0) - 1.0f &&
                        static_cast<float>(r.x1) <= std::ceil(lanes.front().x1) + 1.0f && static_cast<float>(r.y0) >= top &&
                        static_cast<float>(r.y1) <= bottom);
    CHECK_MESSAGE(inLane, r.x0 << "," << r.y0 << " to " << r.x1 << "," << r.y1);
    CHECK(r.x1 - r.x0 <= 8);
    ++columns;
  }
  CHECK(columns > 0);
  u.d.step();
  // Running out of the window pages it on (follow: the playhead at its left edge), the arrangement with it.
  const Ticks out = static_cast<Ticks>(v.laneStart() + v.laneSpan()) + bar;
  u.port.setPlayheadTick(out);
  u.d.advance(0.3);
  CHECK(v.laneStart() == doctest::Approx(static_cast<double>(out)));
  CHECK(u.arrangement().originTick() == doctest::Approx(v.laneStart()));
  // Stopped: no playhead, idle again.
  u.ctx.togglePlay();
  u.d.advance(0.5);
  CHECK_FALSE(v.lanePlayhead().has_value());
  const uint64_t frames1 = u.d.framesDrawn(), reads = v.stats().playheadReads;
  u.d.advance(5.0);
  CHECK(u.d.framesDrawn() == frames1);
  CHECK(v.stats().playheadReads == reads);
}

TEST_CASE("lanes: a double-click on a MIDI clip opens its piano roll; on an audio clip its Clip View") {
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
  const Ticks bar = ticksPerBar(u.ctx.doc()->timeline->sigMap.signatureAt(0));
  v.setLaneTime(0.0, static_cast<double>(4 * bar));
  u.zoomTo("Keys");
  // MIDI: Keys' Riff.
  const uint32_t keys = v.patch().bandOf(u.track("Keys").id);
  REQUIRE(v.laneClips(keys).size() == 1);
  const ProjectPatchView::LaneClip riff = v.laneClips(keys)[0];
  const LaneRect kr = laneBody(v.patch(), v.patch().bands[keys]);
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
  const LaneRect lr = laneBody(v.patch(), v.patch().bands[lb]);
  u.d.doubleClick(u.dev((ac.x0 + ac.x1) * 0.5f, (lr.y0 + lr.y1) * 0.5f));
  u.d.step();
  CHECK(u.ctx.detailClip().clip == ac.clip);
  CHECK(u.ctx.detailClip().track == loop);
  CHECK(u.ctx.detailMode() == DetailMode::Clip);
  // Outside any clip (bar 1 of Pad, which starts at bar 3): nothing opens.
  const doc::ClipRef before = u.ctx.detailClip();
  u.zoomTo("Pad");
  const uint32_t pad = v.patch().bandOf(u.track("Pad").id);
  const LaneRect pr = laneBody(v.patch(), v.patch().bands[pad]);
  u.d.doubleClick(u.dev(pr.x0 + 4.0f, (pr.y0 + pr.y1) * 0.5f));
  u.d.step();
  CHECK(u.ctx.detailClip().clip == before.clip);
}

TEST_CASE("lanes (TL.1): a click on empty lane space moves where Space plays from — in both views (real dispatch)") {
  Ui u;
  GridSpec g = u.ctx.grid();  // a fixed grid of one beat: the clicked ticks do not depend on the zoom
  g.mode = GridMode::Fixed;
  g.fixedExp = 2;
  g.off = false;
  g.snap = true;
  u.ctx.setGrid(g);
  const Ticks bar = ticksPerBar(u.ctx.doc()->timeline->sigMap.signatureAt(0));
  // The patch view: a click on Pad's empty lane at bar 2 (Pad's clip starts at bar 3).
  u.show();
  ProjectPatchView& v = u.view();
  v.setLaneTime(0.0, static_cast<double>(4 * bar));
  u.zoomTo("Pad");
  const uint32_t pad = v.patch().bandOf(u.track("Pad").id);
  const LaneRect pr = laneBody(v.patch(), v.patch().bands[pad]);
  const float k = (pr.x1 - pr.x0) / static_cast<float>(4 * bar);
  u.d.click(u.dev(pr.x0 + k * static_cast<float>(bar), (pr.y0 + pr.y1) * 0.5f));
  u.d.step();
  CHECK(u.ctx.startMarker() == bar);
  CHECK(u.port.playhead().tick == bar);  // stopped: the playhead moved there
  CHECK(u.arrangement().selection().time.has_value());
  CHECK(u.arrangement().selection().time->start == bar);  // the insert marker
  REQUIRE(u.d.press("Space") == KeyResult::Action);
  u.d.step();
  CHECK(u.port.playhead().playing);
  CHECK(u.port.playhead().tick == bar);  // played from the clicked tick
  REQUIRE(u.d.press("Space") == KeyResult::Action);  // stop
  u.d.step();
  // TL.4: while playing a click moves the start marker too (playback goes on); the next Play starts there.
  REQUIRE(u.d.press("Space") == KeyResult::Action);
  u.d.step();
  REQUIRE(u.port.playhead().playing);
  u.d.click(u.dev(pr.x0 + k * static_cast<float>(bar / 2), (pr.y0 + pr.y1) * 0.5f));
  u.d.step();
  CHECK(u.ctx.startMarker() == bar / 2);
  CHECK(u.port.playhead().playing);
  REQUIRE(u.d.press("Space") == KeyResult::Action);  // stop
  u.d.step();
  REQUIRE(u.d.press("Space") == KeyResult::Action);  // play: from the click
  u.d.step();
  CHECK(u.port.playhead().tick == bar / 2);
  REQUIRE(u.d.press("Space") == KeyResult::Action);  // stop
  u.d.step();
  // The arrangement: a click on Pad's empty lane at bar 2 + 2 beats.
  v.setShown(false, false);
  u.d.tree().layoutNow();
  ArrangementView& arr = u.arrangement();
  arr.setTimeView(0.0, arr.pxPerTick());
  u.d.tree().layoutNow();
  int32_t row = -1;
  for (int32_t i = 0; i < static_cast<int32_t>(arr.rows().size()); ++i)
    if (arr.rows()[static_cast<size_t>(i)].id == u.track("Pad").id) row = i;
  REQUIRE(row >= 0);
  const IRect lr = arr.rowLaneRect(row);
  const Ticks beat = ticksPerBeat(u.ctx.doc()->timeline->sigMap.signatureAt(0));
  const Ticks want = bar + 2 * beat;
  u.d.click({arr.xOfTick(static_cast<double>(want)) + 0.5f, static_cast<float>(lr.y) + static_cast<float>(lr.h) * 0.7f});
  u.d.step();
  CHECK(u.ctx.startMarker() == want);
  CHECK(u.port.playhead().tick == want);
  REQUIRE(u.d.press("Space") == KeyResult::Action);
  u.d.step();
  CHECK(u.port.playhead().playing);
  CHECK(u.port.playhead().tick == want);
}

TEST_CASE("lanes (TL.1): a lane's bottom edge resizes it; the fold triangle collapses the circuit and leaves the lane") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  u.zoomTo("Keys");
  const doc::TrackId keysId = u.track("Keys").id;
  uint32_t keys = v.patch().bandOf(keysId);
  const Band b0 = v.patch().bands[keys];
  const float s = v.canvas().deviceScale();
  // Drag the bottom edge 30 units down: the lane is 30 taller (rounded to 4), saved as view state, the band grows.
  const Vec2 edge = u.dev((v.patch().laneX0 + v.patch().laneX1) * 0.5f, b0.laneY + b0.laneH + 1.0f / s);
  REQUIRE(v.canvas().laneEdgeAt(edge).kind == ProjectCanvas::LaneEdgeHit::Kind::Bottom);
  u.d.move(edge);
  CHECK(u.d.tree().cursor() == Cursor::ResizeNS);
  const size_t h0 = u.history();
  u.d.drag(edge, {edge.x, edge.y + 30.0f * s}, 6);
  u.d.step();
  keys = v.patch().bandOf(keysId);
  CHECK(v.patch().bands[keys].laneH == doctest::Approx(b0.laneH + 32.0f).epsilon(0.08));
  CHECK(v.laneHeight(keysId) == v.patch().bands[keys].laneH);
  CHECK(v.patch().bands[keys].h - b0.h == doctest::Approx(v.patch().bands[keys].laneH - b0.laneH));
  CHECK(u.ctx.store().view().patchNudges.at(keysId.value).laneH == v.patch().bands[keys].laneH);
  CHECK(u.history() == h0);  // view state: no undo step
  // Saved with the project (and read back).
  {
    const auto json = doc::serializeProject(*u.ctx.doc(), u.ctx.store().view());
    REQUIRE(json.ok());
    doc::LoadOptions lo;
    lo.devices = &library();
    auto back = doc::parseProject(*json, lo);
    REQUIRE_MESSAGE(back.ok(), (back.ok() ? "" : back.error().message));
    CHECK(back->view.patchNudges.at(keysId.value).laneH == v.patch().bands[keys].laneH);
  }
  // The fold triangle: Keys' blocks are hidden (not in the scene's drawn nodes, not hit), its band is its title strip and
  // its lane; its lane still shows its clips; the bands under it move up.
  const uint32_t pad = v.patch().bandOf(u.track("Pad").id);
  const float padY = v.patch().bands[pad].y;
  const LaneRect fr = ProjectCanvas::foldButtonRect(v.patch().bands[keys]);
  readableAt(v, {(fr.x0 + fr.x1) * 0.5f, (fr.y0 + fr.y1) * 0.5f});  // the title strip's buttons take presses when readable
  u.d.click(u.dev((fr.x0 + fr.x1) * 0.5f, (fr.y0 + fr.y1) * 0.5f));
  u.d.step();
  keys = v.patch().bandOf(keysId);
  const Band& kb = v.patch().bands[keys];
  CHECK(kb.folded);
  CHECK(v.bandFolded(keysId));
  CHECK(kb.h == doctest::Approx(metrics::kTitle + laneBlock(kb)));
  CHECK(v.patch().bands[v.patch().bandOf(u.track("Pad").id)].y < padY);
  for (const uint32_t n : kb.devices) {
    CHECK(v.patch().nodes[n].folded);
    CHECK(v.scene().scene->nodes[v.scene().sceneNode[n]].hidden);
    CHECK(v.canvas().nodeAt(u.centre(n)) == kNone);
  }
  CHECK(v.patch().nodes[kb.mixer].folded);
  CHECK_FALSE(v.laneClips(keys).empty());
  CHECK(u.history() == h0);
  // Fold All: every band its title and lane (the view reads as the arrangement); Unfold All brings them back.
  v.setAllFolded(true);
  for (const Band& b : v.patch().bands) CHECK(b.folded);
  v.setAllFolded(false);
  for (const Band& b : v.patch().bands) CHECK_FALSE(b.folded);
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");
}

TEST_CASE("lanes (TL.1): cables leave the lane's top edge at their own spots — draw new ones, slide one (one undo step)") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());
  u.show();
  ProjectPatchView& v = u.view();
  const doc::TrackId keysId = u.track("Keys").id;
  const uint32_t keys = v.patch().bandOf(keysId);
  // BR.tuning-block: the notes link into the instrument is drawn into the band's tuning block, a cable out of the lane's
  // left end (BR.tuning-outside; not a tap on the lane's edge: it does not slide); the lane's other cables do.
  const doc::DeviceId inst = u.onPath("Keys", 0);
  const uint32_t instNode = u.deviceNode(inst);
  uint32_t ek = kNone;
  for (uint32_t k = 0; k < v.patch().edges.size(); ++k)
    if (v.patch().edges[k].from == v.patch().bands[keys].source && v.patch().edges[k].to == instNode) ek = k;
  REQUIRE(ek != kNone);
  CHECK(v.patch().edges[ek].drawnTo == v.patch().bands[keys].tuning);
  // A new cable drawn up from the lane's Hz chip onto the Saturator's drive pin... or its face: a note-output wire
  // leaving the lane at the chip.
  u.frameLane("Keys");  // from the lane's left end, its port chips readable
  const LaneRect hz = v.canvas().lanePortChip(keys, 1);
  REQUIRE(hz.x1 > hz.x0);
  const Vec2 chip = u.dev((hz.x0 + hz.x1) * 0.5f, (hz.y0 + hz.y1) * 0.5f);
  CHECK(v.canvas().laneEdgeAt(chip).kind == ProjectCanvas::LaneEdgeHit::Kind::Port);
  const size_t wires0 = doc::wiresOf(*u.ctx.doc()).size();
  const doc::DeviceId sat = u.onPath("Keys", 1);
  const uint32_t satNode = u.deviceNode(sat);
  u.d.drag(chip, u.grip(satNode), 10);
  u.d.step();
  // Dropped on a block's body: its params' menu; pick its first param.
  if (Menu* m = u.openMenu()) {
    REQUIRE_FALSE(m->items().empty());
    int32_t first = -1;
    for (int32_t i = 0; i < static_cast<int32_t>(m->items().size()) && first < 0; ++i)
      if (m->items()[static_cast<size_t>(i)].enabled && !m->items()[static_cast<size_t>(i)].separator && m->items()[static_cast<size_t>(i)].submenu.empty())
        first = i;
    REQUIRE(first >= 0);
    m->choose(first);
    u.d.step();
  }
  REQUIRE(doc::wiresOf(*u.ctx.doc()).size() == wires0 + 1);
  const doc::Wire made = doc::wiresOf(*u.ctx.doc()).back();
  CHECK(made.noteSource == keysId);
  CHECK(made.port == "hz");
  REQUIRE(made.tap.has_value());
  CHECK(*made.tap == doctest::Approx((hz.x0 + hz.x1) * 0.5f - v.patch().bands[keys].x0).epsilon(0.02));
  uint32_t e0k = kNone;
  for (uint32_t k = 0; k < v.patch().edges.size(); ++k)
    if (v.patch().edges[k].wire == made.id) e0k = k;
  REQUIRE(e0k != kNone);
  const Edge e0 = v.patch().edges[e0k];
  CHECK(e0.tapPinned);
  CHECK(e0.points.front().x == doctest::Approx(e0.tapX));
  CHECK(e0.points.front().y == doctest::Approx(v.patch().bands[keys].laneY));
  // Slide its lane end 60 units right: saved on the wire (band-local), one undo step; the lowering untouched.
  const Vec2 tap = u.dev(e0.tapX, v.patch().bands[keys].laneY);
  REQUIRE(v.canvas().laneEdgeAt(tap).kind == ProjectCanvas::LaneEdgeHit::Kind::Tap);
  const size_t h0 = u.history();
  const float s = v.canvas().deviceScale();
  u.d.drag(tap, {tap.x + 60.0f * s, tap.y}, 8);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  const doc::Wire* w = doc::findWire(*u.ctx.doc(), e0.wire);
  REQUIRE(w != nullptr);
  REQUIRE(w->tap.has_value());
  CHECK(*w->tap == doctest::Approx(e0.tapX + 60.0f - v.patch().bands[keys].x0).epsilon(0.02));
  uint32_t ek2 = kNone;
  for (uint32_t k = 0; k < v.patch().edges.size(); ++k)
    if (v.patch().edges[k].wire == e0.wire) ek2 = k;
  REQUIRE(ek2 != kNone);
  CHECK(v.patch().edges[ek2].tapPinned);
  CHECK(v.patch().edges[ek2].tapX == doctest::Approx(e0.tapX + 60.0f).epsilon(0.02));
  CHECK(v.patch().edges[ek2].points.front().x == doctest::Approx(v.patch().edges[ek2].tapX));
  // Saved with the project.
  {
    const auto json = doc::serializeProject(*u.ctx.doc(), u.ctx.store().view());
    REQUIRE(json.ok());
    doc::LoadOptions lo;
    lo.devices = &library();
    auto back = doc::parseProject(*json, lo);
    REQUIRE_MESSAGE(back.ok(), (back.ok() ? "" : back.error().message));
    const doc::Wire* bw = doc::findWire(*back->project, e0.wire);
    REQUIRE(bw != nullptr);
    CHECK(bw->tap == w->tap);
  }
  const std::optional<float> slid = w->tap;
  REQUIRE(u.ctx.undo());
  CHECK(doc::findWire(*u.ctx.doc(), e0.wire)->tap == made.tap);
  REQUIRE(u.ctx.redo());
  CHECK(doc::findWire(*u.ctx.doc(), e0.wire)->tap == slid);
  // Double-click a cable's lane end: back to the default, the lane's left end right of its chips (one undo step).
  const size_t h1 = u.history();
  uint32_t ek3 = kNone;
  for (uint32_t k = 0; k < v.patch().edges.size(); ++k)
    if (v.patch().edges[k].wire == e0.wire) ek3 = k;
  REQUIRE(ek3 != kNone);
  const Vec2 tap3 = u.dev(v.patch().edges[ek3].tapX, v.patch().bands[keys].laneY);
  u.d.doubleClick(tap3);
  u.d.step();
  CHECK_FALSE(doc::findWire(*u.ctx.doc(), e0.wire)->tap.has_value());
  CHECK(u.history() == h1 + 1);
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


TEST_CASE("lanes: 64 tracks — a time zoom repaints the lanes only (no re-layout), a full paint of the lanes" * doctest::test_suite("perf")) {
  Ui u(false);
  REQUIRE(buildWideProject(u.ctx.store(), 64).ok());
  REQUIRE(u.d.enableGpu().ok());
  u.show();
  ProjectPatchView& v = u.view();
  v.fitAll(false);
  u.d.advance(1.0);
  const uint64_t relayouts = v.stats().relayouts;
  for (int i = 0; i < 20; ++i) v.zoomLanes(i % 2 == 0 ? -4.0 : 4.0);  // TL.1: the lanes keep their width
  CHECK(v.stats().relayouts == relayouts);
  // Zoomed onto the lanes (Mid / Near): repaint them all.
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
// TX.mixer-look: the mixers' faces close up (Keys expanded, panned, its send set; Pad soloed under it).
TEST_CASE("render: project-mixer matches the golden image") { golden("project-mixer"); }
// T5.x-one-circuit: Open in Circuit View on a device, zoomed out around it (the opened band in context); the lane
// windows (MIDI notes, audio waveforms) over the audio fixture.
TEST_CASE("render: project-circuit-device matches the golden image") { golden("project-circuit-device"); }
TEST_CASE("render: project-patch-lanes matches the golden image") { golden("project-patch-lanes", views::test::fixtureDir("audio")); }
// TMM.10-fix2: wires from the note sources' outputs (per voice into a voice run's Wavetable, reduced into the Auto
// Filter, across tracks).
TEST_CASE("render: project-patch-notes matches the golden image") { golden("project-patch-notes"); }
// TX.patching: free devices under the Lead chain, a wire onto a face control, an output into an audio input.
TEST_CASE("render: project-patch-free matches the golden image") { golden("project-patch-free"); }
// TX.panel-faces (GU.11: closed modules drawn as their group panels — a Saturator, a Utility — an LFO wired onto the
// Utility's Gain).
TEST_CASE("render: project-patch-panels matches the golden image") { golden("project-patch-panels"); }
// TX.resize: a Wavetable face at 1.5×, an Auto Filter panel at 1.25×, a Wavetable at 0.6× below; note-velocity cables
// onto the scaled Wavetable's position and the scaled filter's frequency.
TEST_CASE("render: project-patch-resize matches the golden image") { golden("project-patch-resize"); }
// TX.block-drag: the Auto Filter panel held mid-drag over the Wavetable (drawn at the pointer, its cables on its ports),
// Meta Bass's Phase accumulator at 0.05x (a miniature, tiny plugs).
TEST_CASE("render: project-block-drag matches the golden image") { golden("project-block-drag"); }

// ---- TX.mod-all: a modulation wire ends on the control it modulates ------------------------------------------------

namespace {

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
  {  // GU.4: a phasor atom block at the notes (the Phase accumulator's core: the note's Hz in, its phase out — small, as
     // its face was), in place of the lone instrument; a Wavetable after it.
    const doc::Track& tr = *doc::findTrackPtr(*st.current(), s.track);
    // (GU.11: the default instrument group and its blocks)
    const doc::DeviceId g = doc::mainPath(*st.current(), tr).blocks.at(0)->id;
    std::vector<doc::DeviceId> one{g};
    for (const doc::Device* m : doc::groupMembersDeep(tr.devices, g)) one.push_back(m->id);
    auto d = doc::ops::deleteDevices(*st.current(), one);
    REQUIRE(d.ok());
    REQUIRE(st.dispatch(d->command).ok());
    auto e = doc::ops::addDevice(*st.current(), st.ids(), s.track, "atom:phasor", doc::DeviceKind::AudioEffect, 0);
    REQUIRE(e.ok());
    REQUIRE(st.dispatch(e->command).ok());
    s.pa = e->devices.at(0);
    REQUIRE(doc::mainPath(*st.current(), *doc::findTrackPtr(*st.current(), s.track)).blocks.at(0)->id == s.pa);
  }
  s.wt = add("wavetable", doc::kAppend);
  {  // TX.all-free: a block off the path (GU.4: a phasor atom block — small and light, as the Phase accumulator was)
    auto e = doc::ops::addDevice(*st.current(), st.ids(), s.track, "atom:phasor", doc::DeviceKind::AudioEffect, doc::ops::kOffPath);
    REQUIRE(e.ok());
    REQUIRE(st.dispatch(e->command).ok());
    s.mod = e->devices.at(0);
  }
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
  // The scene wire: a cable (TX.cables) hanging onto the knob's centre as laid out, naming the param; the canvas draws it
  // to the knob's rim where the face puts the knob now.
  const uint32_t w = sceneWireOf(v, s.wire);
  REQUIRE(w != kNone);
  const circuit::SceneWire& sw = sc.wires[w];
  REQUIRE(sw.control != UINT32_MAX);
  CHECK(sw.cable);
  CHECK(sc.strings[sw.control] == "warp");
  const graph::Point2 end = sc.points[sw.firstPoint + sw.pointCount - 1];
  CHECK(end.x == doctest::Approx(n.x + knob->at.x));
  CHECK(end.y == doctest::Approx(n.y + knob->at.y));
  const std::span<const graph::Point2> rope = v.canvas().cablePoints(w);
  REQUIRE(rope.size() >= 2);
  CHECK(std::hypot(rope.back().x - end.x, rope.back().y - end.y) == doctest::Approx(knob->radius).epsilon(0.02));
  // Hover: the cable halfway along (off the face) is the wire.
  const graph::Point2 half = v.canvas().cableMidpoint(w);
  const Vec2 mid = u.dev(half.x, half.y);
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

// Which params keep a pin: those no control shows. Every param of every meta face, asked for its control; the ones
// without one are listed (they keep a pin on the block's left edge when wired). (GU.11: the stock devices' panels are
// retired; a module's controls are its group panel's elements, each bound to a value block.)
TEST_CASE("project patch (TX.mod-all): every face param has a control to end on, except the listed ones") {
  std::vector<std::string> without;
  uint32_t asked = 0;
  for (const meta::Type* type : {&meta::wavetableType(), &meta::envelopeType()}) {
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
  MESSAGE(asked << " params asked; " << without.size() << " have no control (a pin when wired):" << list);
  CHECK(asked >= 13);  // the Wavetable's and the Envelope's
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
    // GU.11: an LFO module wired onto two of a Utility module's knobs (Gain and Width: their value blocks).
    const doc::DeviceId lfo = test_blocks::addModule(st, tr, "lfo", doc::ops::kOffPath);
    const doc::DeviceId util = test_blocks::addModule(st, tr, "utility");
    for (const char* control : {"Gain", "Width"}) {
      auto w = doc::ops::connectWire(*st.current(), st.ids(), lfo, "y", test_blocks::member(*st.current(), util, control),
                                     std::string(doc::kValueParam));
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
  // The LFO modulation wires only: since TX.cable-look each track's notes cable also ends on a control (the Phase
  // Accumulator's frequency, the control its main input is added to), but it is a signal path, not modulation.
  for (const circuit::SceneWire& w : v.scene().scene->wires) legs += w.control != UINT32_MAX && w.style.modulation ? 1 : 0;
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

// warpSetup's track plus a Saturator module after its Wavetable (GU.11: where an Auto Filter was) whose Drive the free
// modulator drives (a cable onto a panel's knob: the Drive value block's).
struct ResizeSetup {
  WarpSetup w;
  doc::DeviceId filter, drive;
  doc::WireId filterWire;
};
ResizeSetup resizeSetup(Ui& u) {
  ResizeSetup r;
  r.w = warpSetup(u);
  doc::DocumentStore& st = u.ctx.store();
  r.filter = test_blocks::addModule(st, r.w.track, "saturator");
  r.drive = test_blocks::member(*st.current(), r.filter, "Drive");
  REQUIRE(r.drive.valid());
  auto w = doc::ops::connectWire(*st.current(), st.ids(), r.w.mod, "out", r.drive, std::string(doc::kValueParam));
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
    CHECK(n.x == doctest::Approx(n0.x));  // its left kept, its main pin on the band's spine (TL.7: the spine is at least
    CHECK(n.y + pinOffset(n, 0) == doctest::Approx(n0.y + pinOffset(n0, 0)));  // the mixer's pin row down: unmoved)
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
        // TX.orbit-ports: its chain cables end on its orbiting main ports, just outside its outline at its scale.
        const bool into = e.to == u.deviceNode(wt);
        const graph::Point2 p = into ? e.points.back() : e.points.front();
        const Node::Orbit& o = into ? n.orbitIn : n.orbitOut;
        REQUIRE(o.on);
        CHECK(p.x == doctest::Approx(o.at.x));
        CHECK(p.y == doctest::Approx(o.at.y));
        CHECK(circuit::outlineDistance(n.outline, p) == doctest::Approx((circuit::kOrbitPortR + circuit::kOrbitGap) * 1.5f).epsilon(0.01));
      }
    CHECK(chains == 2);
    // Room made: the filter after it moved right by the growth, the band grew; the scene node draws the face at 1.5.
    const Node& f = u.pp().nodes[u.deviceNode(r.filter)];
    CHECK(f.x == doctest::Approx(f0.x + n0.w * 0.5f));
    CHECK(f.x >= n.x + n.w + metrics::kNodeGap - 0.01f);
    CHECK(u.pp().bands[n.band].h > bandH0);
    CHECK(v.scene().scene->nodes[v.scene().sceneNode[u.deviceNode(wt)]].scale == doctest::Approx(1.5f));
    CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");
    for (const Edge& e : u.pp().edges) CHECK(routedAsDrawn(e));
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
  REQUIRE(u.ctx.store().dispatch(doc::ops::setDeviceParam(*u.ctx.doc(), r.drive, std::string(doc::kValueParam), 0.8)->command).ok());
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
    const ProjectPatch pp = buildProjectPatch(*other.doc(), low, &library(), o);
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
  (void)z;
  // Crisp: the group's panel drawn at its scale; the faces' layers from tiles, none stretched.
  const uint32_t fn = u.deviceNode(r.filter);
  Face* pfFace = v.canvas().panelOn(v.scene().sceneNode[fn]);
  REQUIRE(pfFace != nullptr);
  auto* pf = dynamic_cast<panel::GroupPanelFace*>(pfFace);
  REQUIRE(pf != nullptr);
  CHECK(pf->worldSize().x == doctest::Approx(u.pp().nodes[fn].w / 1.5f));
  v.canvas().markDirty();
  u.d.step();
  CHECK(v.canvas().stats().faceLayersFromTiles > 0);
  CHECK(v.canvas().stats().faceLayersStretched == 0);
  CHECK(v.canvas().stats().panelsPainted >= 1);
  CHECK_FALSE(v.canvas().tilesPending());
  // The cable onto the scaled panel's Drive knob: it ends on the knob where the panel shows it (× 1.5), and a drag
  // there turns it (one undo step): hit-testing follows the scale.
  {
    const Node& n = u.pp().nodes[fn];
    const std::optional<ControlAnchor> a = pf->bindingAnchor(doc::controlBinding(r.drive, std::string(doc::kValueParam)), true);
    REQUIRE(a.has_value());
    const Vec2 knob = u.dev(n.x + a->at.x * n.scale, n.y + a->at.y * n.scale);
    const uint32_t w = sceneWireOf(v, r.filterWire);
    REQUIRE(w != kNone);
    const circuit::CircuitScene& sc = *v.scene().scene;
    const graph::Point2 end = sc.points[sc.wires[w].firstPoint + sc.wires[w].pointCount - 1];
    CHECK(std::fabs(end.x - (n.x + a->at.x * n.scale)) < 2.0f);
    CHECK(std::fabs(end.y - (n.y + a->at.y * n.scale)) < 2.0f);
    auto driveOf = [&] {
      const doc::DeviceValue* dv = doc::findDevice(*u.ctx.doc(), r.drive).device->param(doc::kValueParam);
      return dv != nullptr ? dv->value : -1.0;
    };
    const size_t h0 = u.history();
    const double f0 = driveOf();
    u.d.drag(knob, {knob.x, knob.y - 40.0f}, 4);
    CHECK(u.history() == h0 + 1);
    CHECK(driveOf() > f0);
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
  const doc::DeviceId dev = u.onPath("Track 9", 1);
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
  // PF.2: a structural edit (a device switched off and on): the model and scene after the lowering (nothing of the
  // last build reused but the opened groups' layouts) stay in the tens of ms. The lowering (pq/document) is reported.
  std::vector<double> edits, lowers;
  for (int i = 0; i < 6; ++i) {
    auto e = doc::ops::setDevicesActive(*u.ctx.doc(), std::vector<doc::DeviceId>{dev}, i % 2 != 0);
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    u.d.step();
    REQUIRE(v.stats().lastLowerMs > 0.0);
    edits.push_back(v.stats().lastRebuildMs - v.stats().lastLowerMs);
    lowers.push_back(v.stats().lastLowerMs);
  }
  std::sort(edits.begin(), edits.end());
  std::sort(lowers.begin(), lowers.end());
  MESSAGE("64 tracks: structural edit: model + scene median " << edits[edits.size() / 2] << " ms (max " << edits.back()
                                                               << "), lowering median " << lowers[lowers.size() / 2] << " ms");
  CHECK(edits[edits.size() / 2] < 50.0);
}

// ---- TX.block-drag: live block drag, blocks never overlap, tiny resize -------------------------------------------------

namespace {

// The cables of scene node `sn` and which end is on it (true: the cable's last point).
std::vector<std::pair<uint32_t, bool>> cablesAt(const ProjectPatchView& v, uint32_t sn) {
  std::vector<std::pair<uint32_t, bool>> out;
  const circuit::CircuitScene& sc = *v.scene().scene;
  for (uint32_t w = 0; w < sc.wires.size(); ++w) {
    if (!sc.wires[w].cable) continue;
    if (sc.pins[sc.wires[w].fromPin].node == sn) out.emplace_back(w, false);
    else if (sc.pins[sc.wires[w].toPin].node == sn) out.emplace_back(w, true);
  }
  return out;
}

// The largest channel difference between the r-px square around (ax, ay) of `a` and around (bx, by) of `b`, the best
// over ±1 px of b (pixel snapping).
uint32_t patchDiff(const Image& a, int ax, int ay, const Image& b, int bx, int by, int r) {
  uint32_t best = 255;
  for (int oy = -1; oy <= 1; ++oy)
    for (int ox = -1; ox <= 1; ++ox) {
      uint32_t worst = 0;
      for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x) {
          const uint8_t* p = a.pixel(static_cast<uint32_t>(ax + x), static_cast<uint32_t>(ay + y));
          const uint8_t* q = b.pixel(static_cast<uint32_t>(bx + ox + x), static_cast<uint32_t>(by + oy + y));
          for (int c = 0; c < 3; ++c) worst = std::max<uint32_t>(worst, static_cast<uint32_t>(std::abs(p[c] - q[c])));
        }
      best = std::min(best, worst);
    }
  return best;
}

bool apart(const Node& a, const Node& b, float gap) {
  return a.x + a.w + gap <= b.x || b.x + b.w + gap <= a.x || a.y + a.h + gap <= b.y || b.y + b.h + gap <= a.y;
}

}  // namespace

// The user (2026-11-03): "clickdragging a module around right now makes an outline move around with the cables attached
// to the outline rather than the module itself". Through the window: the panel itself is drawn at the pointer every frame
// (its pixels move with it), hit there, its cables plugged into its real ports there; nothing is re-laid out while it
// moves; the drop is the move (a nudge), then idle. Esc puts it back.
TEST_CASE("project patch (TX.block-drag): a dragged block is drawn at the pointer each frame, its cables on its ports") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());
  const ResizeSetup r = resizeSetup(u);
  u.show();
  u.zoomTo("Lead WT");
  ProjectPatchView& v = u.view();
  // TL.4: 2× mixers make the band larger, its fit smaller: zoom back in on the block (its title bar several px tall).
  {
    const Node& b = u.pp().nodes[u.deviceNode(r.filter)];
    readableAt(v, {b.x + b.w * 0.5f, b.y + b.h * 0.5f});
  }
  u.d.advance(0.5);
  ProjectCanvas& cv = v.canvas();
  v.canvas().setCablesAnimated(false);  // ends land at once: exact positions to compare
  const uint32_t mn = u.deviceNode(r.filter);
  const Node n0 = u.pp().nodes[mn];
  REQUIRE(n0.groupPanel);  // (GU.11: the Saturator module, closed: its group's panel)
  const uint32_t sn = v.scene().sceneNode[mn];
  const auto mine = cablesAt(v, sn);
  REQUIRE(mine.size() >= 2);  // the Wavetable's into its input, the modulator's onto its frequency knob
  std::vector<graph::Point2> ends0;
  for (const auto& [w, last] : mine) ends0.push_back(last ? cv.cablePoints(w).back() : cv.cablePoints(w).front());
  const Vec2 grip = u.grip(mn);
  auto before = u.d.render();
  REQUIRE(before.ok());
  const uint64_t rebuilds0 = v.stats().rebuilds, relayouts0 = v.stats().relayouts;
  const float s = cv.deviceScale();
  u.d.move(grip);
  u.d.down(grip);
  const Vec2 steps[3] = {{30.0f, 20.0f}, {90.0f, 60.0f}, {150.0f, 110.0f}};
  for (const Vec2 dlt : steps) {
    const Vec2 at{grip.x + dlt.x, grip.y + dlt.y};
    u.d.move(at);
    u.d.step();
    CHECK(cv.drag() == ProjectCanvas::Drag::MoveDevice);
    // Lifted: its scene node drawn at the drag's offset; the model and scene are as they were (no re-layout).
    const std::span<const uint32_t> lifted = cv.draggedNodes();
    CHECK(std::find(lifted.begin(), lifted.end(), sn) != lifted.end());
    CHECK(cv.nodeDragOffset().x == doctest::Approx(dlt.x / s).epsilon(0.01));
    CHECK(cv.nodeDragOffset().y == doctest::Approx(dlt.y / s).epsilon(0.01));
    CHECK(u.pp().nodes[mn].x == n0.x);
    CHECK(v.stats().rebuilds == rebuilds0);
    CHECK(v.stats().relayouts == relayouts0);
    // Its cables end on its ports where it is drawn now (the other ends stay). TX.orbit-ports: an orbiting port's end
    // is on the port where it turned to (at the drag's offset); a control's moves with the block.
    for (size_t i = 0; i < mine.size(); ++i) {
      const auto [w, last] = mine[i];
      const graph::Point2 e = last ? cv.cablePoints(w).back() : cv.cablePoints(w).front();
      const circuit::SceneWire& swr = cv.scene()->wires[w];
      const uint32_t pin = last ? swr.toPin : swr.fromPin;
      if (cv.pinOrbits(pin)) {
        const graph::Point2 pp = cv.pinPoint(pin);
        CHECK(e.x == doctest::Approx(pp.x + cv.nodeDragOffset().x).epsilon(0.005));
        CHECK(e.y == doctest::Approx(pp.y + cv.nodeDragOffset().y).epsilon(0.005));
        CHECK(circuit::outlineDistance(cv.scene()->nodes[sn].outline, pp) == doctest::Approx(circuit::kOrbitPortR + circuit::kOrbitGap).epsilon(0.01));
        continue;
      }
      CHECK(e.x == doctest::Approx(ends0[i].x + dlt.x / s).epsilon(0.005));
      CHECK(e.y == doctest::Approx(ends0[i].y + dlt.y / s).epsilon(0.005));
    }
    // Hit where it is drawn, not where it was.
    const circuit::CanvasHit there = cv.hitTest({grip.x + dlt.x, grip.y + dlt.y + 20.0f});
    CHECK(there.kind == circuit::CanvasHit::Kind::Node);
    CHECK(there.index == sn);
    const Vec2 left = u.dev(n0.x + 6.0f, n0.y + n0.h * 0.5f);  // inside where it was, left of where it is now
    const circuit::CanvasHit old = cv.hitTest(left);
    CHECK_FALSE((old.kind == circuit::CanvasHit::Kind::Node && old.index == sn));
    CHECK(cv.stats().panelsPainted >= 1);
  }
  // Its pixels moved with it: its title bar now where the pointer has it (and no longer where it was).
  auto during = u.d.render();
  REQUIRE(during.ok());
  const Vec2 last = steps[2];
  const int gx = static_cast<int>(u.dev(n0.x + 40.0f, n0.y).x), gy = static_cast<int>(grip.y);  // its title bar's left part
  CHECK(patchDiff(*before, gx, gy, *during, gx + static_cast<int>(last.x), gy + static_cast<int>(last.y), 3) <= 24);
  CHECK(patchDiff(*before, gx, gy, *during, gx, gy, 3) > 24);
  // Esc: back where it was, nothing changed.
  const size_t h0 = u.history();
  u.d.keyDown(key::kEscape);
  u.d.keyUp(key::kEscape);
  CHECK(cv.draggedNodes().empty());
  u.d.up({grip.x + last.x, grip.y + last.y});
  CHECK(u.pp().nodes[u.deviceNode(r.filter)].x == n0.x);
  CHECK(u.history() == h0);
  // Again, released: the block is where it was dropped (view state: no undo step), its cables at rest on its ports.
  u.d.drag(grip, {grip.x + 40.0f, grip.y + 160.0f}, 6);
  u.d.advance(2.0);
  const Node& n1 = u.pp().nodes[u.deviceNode(r.filter)];
  CHECK(cv.draggedNodes().empty());
  CHECK(v.overlappingBlocks().empty());
  CHECK(n1.y > n0.y + 60.0f / s);
  CHECK(u.history() == h0);
  const uint64_t f0 = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.d.framesDrawn() == f0);  // idle: no frames
}

// "if i drop a module on top of another module it's supposed to have physics and repel from the other module": dropped
// on the Wavetable, the modulator and the Wavetable push each other apart (the dropped one gives way most) until they
// are a gap apart, gliding a few frames; then no frames. A selection drags together; a band drags by its title.
TEST_CASE("project patch (TX.block-drag): a block dropped on another repels it — no overlap, a gap, settled; selections and bands drag live") {
  Ui u;
  const ResizeSetup r = resizeSetup(u);
  u.show();
  u.zoomTo("Lead WT");
  u.d.advance(0.5);
  ProjectPatchView& v = u.view();
  ProjectCanvas& cv = v.canvas();
  CHECK(v.overlappingBlocks().empty());
  const uint32_t modN = u.deviceNode(r.w.mod), wtN = u.deviceNode(r.w.wt);
  const Node wt0 = u.pp().nodes[wtN];
  // Grab the modulator by its middle and drop its middle on the Wavetable's.
  const Vec2 from = u.centre(modN), to = u.centre(wtN);
  u.d.drag(from, to, 8);
  // At the drop they overlap; the push starts and glides.
  const Node dropped = u.pp().nodes[u.deviceNode(r.w.mod)];
  CHECK_FALSE(apart(dropped, u.pp().nodes[u.deviceNode(r.w.wt)], 0.0f));
  CHECK(v.pushing());
  const uint64_t pushes = v.pushes();
  u.d.advance(2.0);
  CHECK_FALSE(v.pushing());
  CHECK(v.pushFrames() > 2);
  CHECK(v.pushFrames() < 240);
  const Node& mod1 = u.pp().nodes[u.deviceNode(r.w.mod)];
  const Node& wt1 = u.pp().nodes[u.deviceNode(r.w.wt)];
  CHECK(apart(mod1, wt1, ProjectPatchView::kPushGap - 1.0f));
  CHECK(v.overlappingBlocks().empty());
  // Both moved; the dropped (lighter) one more.
  const float dm = std::hypot(mod1.x - dropped.x, mod1.y - dropped.y), dw = std::hypot(wt1.x - wt0.x, wt1.y - wt0.y);
  CHECK(dm > dw);
  CHECK(dw > 0.5f);
  CHECK(v.pushes() >= pushes);
  // Settled: no frames.
  const uint64_t f0 = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.d.framesDrawn() == f0);

  // A selection of blocks drags together (both lifted while it moves) and lands by the same delta.
  const uint32_t paN = u.deviceNode(r.w.pa), fN = u.deviceNode(r.filter);
  u.d.click(u.grip(paN));
  u.d.click(u.grip(fN), mod::kShift);
  REQUIRE(v.selectedDevices().size() == 2);
  const Node pa0 = u.pp().nodes[paN], f0n = u.pp().nodes[fN];
  const Vec2 g = u.grip(paN);
  u.d.move(g);
  u.d.down(g);
  u.d.move({g.x + 10.0f, g.y + 10.0f});
  u.d.move({g.x + 30.0f, g.y + 24.0f});
  u.d.step();
  const ProjectScene& ps = v.scene();
  const std::span<const uint32_t> lifted = cv.draggedNodes();
  CHECK(std::find(lifted.begin(), lifted.end(), ps.sceneNode[paN]) != lifted.end());
  CHECK(std::find(lifted.begin(), lifted.end(), ps.sceneNode[fN]) != lifted.end());
  CHECK(v.canvas().dragDevices().size() == 2);
  u.d.up({g.x + 30.0f, g.y + 24.0f});
  const float s = cv.deviceScale();
  const Node& pa1 = u.pp().nodes[u.deviceNode(r.w.pa)];
  const Node& f1 = u.pp().nodes[u.deviceNode(r.filter)];
  CHECK(pa1.x - pa0.x == doctest::Approx(30.0f / s).epsilon(0.05));
  CHECK(f1.x - f0n.x == doctest::Approx(30.0f / s).epsilon(0.05));
  CHECK(pa1.y - pa0.y == doctest::Approx(f1.y - f0n.y).epsilon(0.05));
  u.d.advance(2.0);
  CHECK(v.overlappingBlocks().empty());

  // A band by its title strip: every node of the band is lifted (its lane, its blocks, its mixer).
  const uint32_t b = u.pp().bandOf(r.w.track);
  const Band& band = u.pp().bands[b];
  const Vec2 title = u.dev(band.x0 + 120.0f, band.y + metrics::kTitle * 0.5f);
  u.d.move(title);
  u.d.down(title);
  u.d.move({title.x + 4.0f, title.y + 30.0f});
  u.d.step();
  CHECK(cv.drag() == ProjectCanvas::Drag::Nudge);
  const std::span<const uint32_t> lb = cv.draggedNodes();
  CHECK(std::find(lb.begin(), lb.end(), v.scene().sceneNode[band.source]) != lb.end());
  CHECK(std::find(lb.begin(), lb.end(), v.scene().sceneNode[band.mixer]) != lb.end());
  CHECK(cv.bandLifted(b));
  u.d.keyDown(key::kEscape);
  u.d.keyUp(key::kEscape);
  u.d.up({title.x + 4.0f, title.y + 30.0f});
  CHECK(cv.draggedNodes().empty());
}

// "the shrinking feature should also be able to make modules way way way way way smaller": S-drag down to 0.05× (a face
// and a panel); still drawn (a miniature far below legibility), hit at a minimum size, its body grabbable (its pins do
// not take all of it), resized back up from there. Overlaps a resize makes are pushed apart on release (the resized
// block holds its place).
TEST_CASE("project patch (TX.block-drag): S-resize to 0.05x — drawn, grabbable, resizable back; overlaps after a resize resolved") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());
  const ResizeSetup r = resizeSetup(u);
  u.show();
  u.zoomTo("Lead WT");
  u.d.advance(0.5);
  ProjectPatchView& v = u.view();
  ProjectCanvas& cv = v.canvas();
  for (const doc::DeviceId dev : {r.w.wt, r.filter}) {
    const Node n0 = u.pp().nodes[u.deviceNode(dev)];
    sDrag(u, dev, -4000.0f, -4000.0f);
    CHECK(v.blockScale(dev) == doctest::Approx(doc::kMinPatchScale));
    CHECK(doc::kMinPatchScale == doctest::Approx(0.05f));
    u.d.advance(0.5);
    const uint32_t mn = u.deviceNode(dev);
    const Node& n = u.pp().nodes[mn];
    CHECK(n.w == doctest::Approx(n0.w * 0.05f).epsilon(0.02));
    CHECK(n.h == doctest::Approx(n0.h * 0.05f).epsilon(0.02));
    // Drawn (the face / the panel at its tiny scale), no frames once settled.
    cv.markDirty();
    u.d.step();
    CHECK(cv.stats().facesPainted + cv.stats().panelsPainted >= 1);
    auto img = u.d.render();
    REQUIRE(img.ok());
    // Hit at a minimum size; its middle is the block (not a pin).
    const Vec2 c = u.centre(mn);
    CHECK(cv.nodeAt(c) == mn);
    CHECK(cv.nodeAt({c.x + ProjectCanvas::kMinHitPx * 0.4f, c.y}) == mn);
    CHECK_FALSE(cv.pinAt(c).valid());
    CHECK(cv.resizableAt(c) == mn);
    // Grabbed and moved by its middle (live: lifted).
    u.d.move(c);
    u.d.down(c);
    u.d.move({c.x + 20.0f, c.y + 6.0f});
    u.d.move({c.x + 40.0f, c.y + 12.0f});
    CHECK(cv.drag() == ProjectCanvas::Drag::MoveDevice);
    CHECK_FALSE(cv.draggedNodes().empty());
    u.d.up({c.x + 40.0f, c.y + 12.0f});
    u.d.advance(1.0);
    CHECK(cv.draggedNodes().empty());
    // And back up to its own size from there (the detent at 1×).
    sDrag(u, dev, 4000.0f, 4000.0f);
    CHECK(v.blockScale(dev) == doctest::Approx(doc::kMaxPatchScale));
    REQUIRE(v.setBlockScale(dev, 1.0f));
    u.d.advance(1.0);
  }
  CHECK(v.overlappingBlocks().empty());
  // Overlaps after a resize: the modulator placed just under the first Phase accumulator; that block grown into it.
  const uint32_t paN = u.deviceNode(r.w.pa);
  {
    const Node& pa = u.pp().nodes[paN];
    const Node& mod = u.pp().nodes[u.deviceNode(r.w.mod)];
    v.nudge(r.w.mod.value, pa.x - mod.x, pa.y + pa.h + 8.0f - mod.y);
  }
  u.d.advance(1.0);
  CHECK(v.overlappingBlocks().empty());
  const Node pa0 = u.pp().nodes[u.deviceNode(r.w.pa)];
  const float z = cv.deviceScale();
  sDrag(u, r.w.pa, pa0.w * z * 0.6f, pa0.h * z * 0.6f);
  CHECK(v.blockScale(r.w.pa) > 1.4f);
  const Node paR = u.pp().nodes[u.deviceNode(r.w.pa)];  // where the resize left it (the layout made room), overlapping
  CHECK(v.pushing());
  u.d.advance(2.0);
  CHECK_FALSE(v.pushing());
  CHECK(v.overlappingBlocks().empty());
  const Node& pa1 = u.pp().nodes[u.deviceNode(r.w.pa)];
  const Node& mod1 = u.pp().nodes[u.deviceNode(r.w.mod)];
  CHECK(apart(pa1, mod1, ProjectPatchView::kPushGap - 1.0f));
  CHECK(std::hypot(pa1.x - paR.x, pa1.y - paR.y) < 8.0f);  // the resized block (heavier) held its place, the other gave way
  const uint64_t f0 = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.d.framesDrawn() == f0);
}

// Far out (the cached chunks): a block (a miniature: no controls, so any press grabs it) dragged through the window is
// drawn over the cached picture at the pointer, the chunks it was in drawn directly without it; the drop moves it; idle.
TEST_CASE("project patch (TX.block-drag): far out — the dragged block over the cached chunks, the chunks under it drawn without it") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());
  const ResizeSetup r = resizeSetup(u);
  u.show();
  ProjectPatchView& v = u.view();
  ProjectCanvas& cv = v.canvas();
  REQUIRE(v.setBlockScale(r.filter, 0.2f));
  const Node& n = u.pp().nodes[u.deviceNode(r.filter)];
  const float us = u.d.tree().scale();
  circuit::CircuitCanvas::View vw;
  vw.zoom = 0.12;
  vw.x = n.x + n.w * 0.5f - static_cast<double>(cv.rect().w) * 0.5 / (vw.zoom * us);
  vw.y = n.y + n.h * 0.5f - static_cast<double>(cv.rect().h) * 0.5 / (vw.zoom * us);
  cv.setView(vw);
  u.d.advance(1.0);  // the chunks built
  REQUIRE(cv.lod() == circuit::CircuitCanvas::Lod::Far);
  cv.markDirty();
  u.d.step();
  CHECK(cv.stats().chunksDirect == 0);
  const Node n0 = u.pp().nodes[u.deviceNode(r.filter)];
  const Vec2 c = u.centre(u.deviceNode(r.filter));
  u.d.move(c);
  u.d.down(c);
  u.d.move({c.x + 10.0f, c.y + 6.0f});
  u.d.move({c.x + 40.0f, c.y + 30.0f});
  u.d.step();
  CHECK(cv.drag() == ProjectCanvas::Drag::MoveDevice);
  CHECK_FALSE(cv.draggedNodes().empty());
  CHECK(cv.stats().chunksDirect > 0);  // where it (and its cables) were: drawn without it
  CHECK(cv.hitTest({c.x + 40.0f, c.y + 30.0f}).kind != circuit::CanvasHit::Kind::None);
  u.d.up({c.x + 40.0f, c.y + 30.0f});
  CHECK(cv.draggedNodes().empty());
  const Node& n1 = u.pp().nodes[u.deviceNode(r.filter)];
  CHECK(n1.x - n0.x == doctest::Approx(40.0f / cv.deviceScale()).epsilon(0.1));
  u.d.advance(2.0);
  CHECK(v.overlappingBlocks().empty());
  const uint64_t f0 = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.d.framesDrawn() == f0);
}

// IG.1-2 (SPEC §15 2026-11-04 "instrument groups"): a new MIDI track is drawn with its instrument group — an opened
// capsule holding In → Basic Synth → Out on the band's path, the notes into the capsule, In → the synth a note wire and
// the synth → Out a chain inside it; the view is the lowered graph (the synth's call in the track patch).
TEST_CASE("project patch (IG.1-2): a new MIDI track shows its instrument group — its modules in a capsule") {
  // GU.10: Basic Synth is the default instrument's group preset: In → Phase Accumulator → Wavetable → Envelope → Out,
  // each a module (a capsule of its own).
  doc::DocumentStore store;
  auto e = doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Keys");
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const doc::Project& p = *store.current();
  const doc::Track& t = *doc::findTrackPtr(p, e->tracks.at(0));
  doc::DeviceId group;
  for (const doc::DevicePtr& d : t.devices)
    if (doc::isGroupDevice(*d) && !d->group.valid()) group = d->id;
  REQUIRE(group.valid());
  std::vector<doc::DeviceId> modules;
  for (const doc::Device* d : doc::groupMembers(t.devices, group)) modules.push_back(d->id);
  REQUIRE(modules.size() == 3);
  const doc::LoweredProject low = lowerOf(p);
  const ProjectPatch pp = buildProjectPatch(p, low, &library());
  CHECK(checkAgainstGraph(pp, p, low) == "");
  const uint32_t cap = pp.nodeOf(group);
  REQUIRE(cap != kNone);
  const Node& c = pp.nodes[cap];
  CHECK(c.group);
  CHECK(c.instrument);  // the notes reach its In
  REQUIRE(c.members.size() == 3);
  std::vector<uint32_t> nodes;
  for (const doc::DeviceId m : modules) {
    const uint32_t n = pp.nodeOf(m);
    REQUIRE(n != kNone);
    CHECK(pp.nodes[n].group);
    CHECK(pp.nodes[n].capsule == cap);
    nodes.push_back(n);
  }
  auto edge = [&](uint32_t a, uint32_t b) {
    for (const Edge& x : pp.edges)
      if (x.from == a && x.to == b) return true;
    return false;
  };
  const Band& band = pp.bands[c.band];
  CHECK(edge(band.source, cap));     // the track's notes into the capsule
  CHECK(edge(c.groupIn, nodes[0]));  // In → Phase Accumulator
  CHECK(edge(nodes[0], nodes[1]));   // → Wavetable
  CHECK(edge(nodes[1], nodes[2]));   // → Envelope
  CHECK(edge(nodes[2], c.groupOut)); // → Out
  CHECK(edge(cap, band.mixer));      // the group's sum into the mixer
}

// TL.4 (user 2026-11-09: "if i click on bar 4 via the midi track and press play it doesnt play from there, it stays
// behind"): the gesture the user made — a click INTO a MIDI clip in a band lane (TL.1 handled only empty lane space), then
// Play — through real dispatch, in both views; a click on a clip's title strip (no drag) too; while playing, the click
// moves where the next Play starts and the Play button restarts there.
TEST_CASE("lanes (TL.4): a click on a clip in a lane moves where Play starts — body or title, stopped or playing, both views") {
  Ui u;
  GridSpec g = u.ctx.grid();
  g.mode = GridMode::Fixed;
  g.fixedExp = 2;
  g.off = false;
  g.snap = true;
  u.ctx.setGrid(g);
  const Ticks bar = ticksPerBar(u.ctx.doc()->timeline->sigMap.signatureAt(0));
  const Ticks beat = ticksPerBeat(u.ctx.doc()->timeline->sigMap.signatureAt(0));
  const doc::Track& pad = u.track("Pad");
  REQUIRE_FALSE(pad.clips.empty());
  const doc::ClipSlot clip = pad.clips.front();
  REQUIRE(clip.length() >= 4 * beat);
  u.show();
  ProjectPatchView& v = u.view();
  v.setLaneTime(0.0, static_cast<double>(clip.end() + bar));
  u.zoomTo("Pad");
  const uint32_t pb = v.patch().bandOf(pad.id);
  const LaneRect pr = laneBody(v.patch(), v.patch().bands[pb]);
  const float k = (pr.x1 - pr.x0) / static_cast<float>(clip.end() + bar);
  auto xAt = [&](Ticks t) { return pr.x0 + k * static_cast<float>(t) + 0.5f; };
  const Ticks want = clip.start() + beat;  // one beat into the clip
  // Stopped: a click into the clip's body (below its title strip).
  REQUIRE(u.ctx.startMarker() != want);
  u.d.click(u.dev(xAt(want), pr.y1 - 3.0f));
  u.d.step();
  CHECK(u.ctx.startMarker() == want);
  CHECK(u.port.playhead().tick == want);
  REQUIRE(u.d.press("Space") == KeyResult::Action);
  u.d.step();
  CHECK(u.port.playhead().playing);
  CHECK(u.port.playhead().tick == want);  // Play starts at the click, not where the start marker was
  // Playing: a click on the clip's title strip (a click, not a drag) moves the start marker; playback goes on; the Play
  // button starts again there.
  const Ticks title = clip.start() + 2 * beat;
  u.d.click(u.dev(xAt(title), pr.y0 + 2.0f));
  u.d.step();
  CHECK(u.ctx.startMarker() == title);
  CHECK(u.port.playhead().playing);
  const Vec2 play = u.d.center("main/control-bar/play");
  REQUIRE(play.x > 0.0f);
  u.port.setPlayheadTick(clip.end());  // playback went on
  u.d.click(play);
  u.d.step();
  CHECK(u.port.playhead().playing);
  CHECK(u.port.playhead().tick == title);
  REQUIRE(u.d.press("Space") == KeyResult::Action);  // stop
  u.d.step();
  // A clip drag is no click: the start marker stays.
  {
    const Ticks before = u.ctx.startMarker();
    const size_t h = u.history();
    const Vec2 a = u.dev(xAt(clip.start() + beat), pr.y0 + 2.0f);
    u.d.drag(a, {a.x + k * static_cast<float>(bar), a.y}, 6);
    u.d.step();
    CHECK(u.ctx.startMarker() == before);
    CHECK(u.history() == h + 1);  // the clip moved
    REQUIRE(u.ctx.undo());
  }
  // The arrangement: a click into the same clip's body.
  v.setShown(false, false);
  u.d.tree().layoutNow();
  ArrangementView& arr = u.arrangement();
  arr.setTimeView(0.0, arr.pxPerTick());
  u.d.tree().layoutNow();
  int32_t row = -1;
  for (int32_t i = 0; i < static_cast<int32_t>(arr.rows().size()); ++i)
    if (arr.rows()[static_cast<size_t>(i)].id == pad.id) row = i;
  REQUIRE(row >= 0);
  const IRect lr = arr.rowLaneRect(row);
  const Ticks at = clip.start() + 3 * beat;
  u.d.click({arr.xOfTick(static_cast<double>(at)) + 0.5f, static_cast<float>(lr.bottom()) - 3.0f});
  u.d.step();
  CHECK(u.ctx.startMarker() == at);
  CHECK(u.port.playhead().tick == at);
  REQUIRE(u.d.press("Space") == KeyResult::Action);
  u.d.step();
  CHECK(u.port.playhead().tick == at);
}

// TL.4 (user: "the header that you clickdrag on to scrub … a smaller thinner version of the one at the top"): every lane's
// top strip is the compact Ruler on the lanes' axis — a click locates (snapped, the start marker and the playhead), a
// drag zooms the shared window, exactly as the top ruler; the cable tap edge above it still takes its presses.
TEST_CASE("lanes (TL.4): each lane's thin scrub ruler — a click locates, a drag zooms, through real dispatch") {
  Ui u;
  GridSpec g = u.ctx.grid();
  g.mode = GridMode::Fixed;
  g.fixedExp = 2;
  g.off = false;
  g.snap = true;
  u.ctx.setGrid(g);
  const Ticks bar = ticksPerBar(u.ctx.doc()->timeline->sigMap.signatureAt(0));
  u.show();
  ProjectPatchView& v = u.view();
  v.setLaneTime(0.0, static_cast<double>(8 * bar));
  u.zoomTo("Pad");
  REQUIRE(v.laneRuler() != nullptr);
  const uint32_t pb = v.patch().bandOf(u.track("Pad").id);
  const Band& b = v.patch().bands[pb];
  const LaneRect rr = laneRuler(v.patch(), b);
  CHECK(rr.y0 == b.laneY);  // under the tap edge, the lane's top
  CHECK(rr.y1 - rr.y0 == metrics::kLaneRulerH);
  CHECK(laneBody(v.patch(), b).y0 == rr.y1);  // the clip row under it
  const float k = (rr.x1 - rr.x0) / static_cast<float>(8 * bar);
  const Vec2 at = u.dev(rr.x0 + k * static_cast<float>(3 * bar) + 0.5f, (rr.y0 + rr.y1) * 0.5f + 1.0f);
  u.d.move(at);
  CHECK(u.d.tree().cursor() == Cursor::ResizeNS);  // a drag zooms (the ruler's cursor)
  CHECK_FALSE(v.laneEditAt(at));                    // not the clip row
  const size_t h0 = u.history();
  u.d.click(at);
  u.d.step();
  CHECK(u.ctx.startMarker() == 3 * bar);
  CHECK(u.port.playhead().tick == 3 * bar);
  CHECK(u.history() == h0);
  CHECK(v.canvas().drag() == ProjectCanvas::Drag::None);
  // A vertical drag zooms the shared window (down = in), the arrangement's with it.
  const double span0 = u.ctx.timeWindow().span;
  u.d.drag(at, {at.x, at.y + 60.0f}, 6);
  u.d.step();
  CHECK(u.ctx.timeWindow().span < span0 * 0.9);
  CHECK(u.ctx.startMarker() == 3 * bar);  // a drag is no click
  CHECK(v.canvas().drag() == ProjectCanvas::Drag::None);
  // The tap edge above it is still the cable's (a press there starts a cable, not a locate).
  const Vec2 edge = u.dev(rr.x0 + (rr.x1 - rr.x0) * 0.6f, b.laneY);
  CHECK(v.canvas().laneEdgeAt(edge).kind != ProjectCanvas::LaneEdgeHit::Kind::None);
}

// TL.4 (user: "mixer needs to be resizeable, and made 2x larger by default"): the mixer face at 2× by default, its controls
// where its face draws them; S + drag resizes it as a block (saved, Esc restores, S + double-click resets).
TEST_CASE("project patch (TL.4): the mixer face is 2x by default and S-drag resizes it") {
  Ui u;
  u.show();
  ProjectPatchView& v = u.view();
  const doc::TrackId keys = u.track("Keys").id;
  u.zoomTo("Keys");
  auto mixer = [&] { return u.pp().bands[u.pp().bandOf(keys)].mixer; };
  const Node m0 = u.pp().nodes[mixer()];
  CHECK(m0.scale == metrics::kMixerScale);
  CHECK(m0.w == doctest::Approx(metrics::kMixerW * metrics::kMixerScale));
  CHECK(v.mixerScale(keys) == metrics::kMixerScale);
  CHECK(pinOffset(m0, 0) == doctest::Approx(metrics::kPinTop * metrics::kMixerScale));
  // Its pan knob where its face draws it (2×): a drag turns it (one undo step).
  const size_t h0 = u.history();
  const Vec2 pan = u.mixerPan(mixer());
  u.d.drag(pan, {pan.x, pan.y - 30.0f}, 6);
  CHECK(u.history() == h0 + 1);
  // S + drag from its centre by a quarter of its diagonal outward: 1.25 × the default.
  const float z = v.canvas().deviceScale();
  const Vec2 c = u.centre(mixer());
  const size_t h1 = u.history();
  u.d.keyDown('s');
  u.d.move(c);
  CHECK(u.d.tree().cursor() == Cursor::ResizeNWSE);
  u.d.down(c);
  for (int i = 1; i <= 4; ++i)
    u.d.move({c.x + m0.w * z * 0.25f * static_cast<float>(i) / 4.0f, c.y + m0.h * z * 0.25f * static_cast<float>(i) / 4.0f});
  u.d.up({c.x + m0.w * z * 0.25f, c.y + m0.h * z * 0.25f});
  u.d.keyUp('s');
  CHECK(v.mixerScale(keys) == doctest::Approx(metrics::kMixerScale * 1.25f).epsilon(0.02));
  const Node& m1 = u.pp().nodes[mixer()];
  CHECK(m1.scale == doctest::Approx(v.mixerScale(keys)));
  CHECK(m1.w == doctest::Approx(metrics::kMixerW * m1.scale));
  CHECK(u.history() == h1);  // view state like a block's scale: saved, no undo step
  CHECK(u.ctx.dirty());
  CHECK(u.ctx.store().view().patchNudges.at(keys.value).scale == doctest::Approx(m1.scale / metrics::kMixerScale));
  // Esc during a resize: back to the size at the press.
  {
    const float before = v.mixerScale(keys);
    const Vec2 c2 = u.centre(mixer());
    u.d.keyDown('s');
    u.d.move(c2);
    u.d.down(c2);
    u.d.move({c2.x + 40.0f, c2.y + 40.0f});
    CHECK(v.mixerScale(keys) != doctest::Approx(before));
    u.d.press("Escape");
    u.d.keyUp('s');
    u.d.up({c2.x + 40.0f, c2.y + 40.0f});
    CHECK(v.mixerScale(keys) == doctest::Approx(before));
  }
  // S + double-click: the default again.
  u.d.keyDown('s');
  u.d.doubleClick(u.centre(mixer()));
  u.d.keyUp('s');
  CHECK(v.mixerScale(keys) == metrics::kMixerScale);
}

TEST_CASE("KB.1: with the computer keyboard on, every note key plays whichever main view has the focus (real dispatch)") {
  Ui u;
  ComputerKeyboard& kb = u.ctx.computerKeyboard();
  const doc::TrackId keys = u.track("Keys").id;
  auto clip = doc::ops::insertEmptyClip(*u.ctx.doc(), u.ctx.store().ids(), keys, 0, kTicksPerWhole, "Riff");
  REQUIRE(clip.ok());
  REQUIRE(u.ctx.store().dispatch(clip->command).ok());
  u.arrangement().selectTrack(keys, mod::kNone);  // auto-armed (nothing is armed by hand): its layout
  u.d.press("M");
  REQUIRE(kb.active());
  const ComputerKeyboard::Layout layout = u.ctx.keyboardLayout();
  std::vector<Key> notes;
  for (const char c : std::string_view("abcdefghijklmnopqrstuvwxyz0123456789-=[]\\;',./"))
    if (ComputerKeyboard::noteOf(static_cast<Key>(c), layout, kb.scaleMode())) notes.push_back(static_cast<Key>(c));
  REQUIRE(notes.size() >= 17);
  CHECK(std::find(notes.begin(), notes.end(), Key{'f'}) != notes.end());
  auto liveKeys = [&] {
    return std::count_if(u.port.calls.begin(), u.port.calls.end(),
                         [](const FakeTransport::Record& r) { return r.call == FakeTransport::Call::LiveKey; });
  };
  auto playAll = [&](const char* where, Widget* focus) {
    REQUIRE_MESSAGE(focus != nullptr, where);
    u.d.tree().setFocus(focus);
    REQUIRE_MESSAGE(u.d.tree().focus() == focus, where);
    const size_t h = u.history();
    const auto n0 = liveKeys();
    for (const Key k : notes) {
      CHECK_MESSAGE(u.d.keyDown(k) == KeyResult::Widget, where << ": " << static_cast<char>(k));
      u.d.keyUp(k);
    }
    CHECK_MESSAGE(liveKeys() == n0 + 2 * static_cast<std::ptrdiff_t>(notes.size()), where);
    CHECK_MESSAGE(u.history() == h, where);  // no view edited anything with them
    CHECK_MESSAGE(u.d.tree().focus() == focus, where);
  };
  playAll("arrangement", &u.arrangement());
  // The project patch view: F would fit, S / P / 0 … are its keys; the view stays where it is.
  u.show();
  ProjectPatchView& v = u.view();
  const circuit::CircuitCanvas::View before = v.canvas().view();
  playAll("project patch view", &v.canvas());
  CHECK(v.canvas().view() == before);
  // The circuit editor (a band opened in place): F would go back to the project.
  REQUIRE(v.openCircuit(keys, u.onPath("Keys", 0), false));
  u.d.advance(0.5);
  REQUIRE(v.openedBand() == keys);
  const circuit::CircuitCanvas::View inBand = v.canvas().view();
  playAll("circuit editor", &v.canvas());
  CHECK(v.openedBand() == keys);
  CHECK(v.canvas().view() == inBand);
  v.setShown(false, false);
  u.d.tree().layoutNow();
  // The piano roll of the clip, then the device chain.
  u.ctx.setDetailClip(clip->clips[0]);
  u.ctx.setDetailVisible(true);
  u.ctx.setDetailMode(DetailMode::Clip);
  u.d.tree().layoutNow();
  playAll("piano roll", u.d.find("main/body/bottom/detail/clip/clip-view/notes"));
  u.ctx.setDetailMode(DetailMode::Device);
  u.d.tree().layoutNow();
  Widget* chain = nullptr;
  std::function<void(Widget&)> findChain = [&](Widget& w) {
    if (!chain && dynamic_cast<DeviceChainView*>(&w)) chain = &w;
    for (size_t i = 0; i < w.childCount() && !chain; ++i) findChain(w.child(i));
  };
  findChain(u.shell->slot(ShellSlot::DeviceDetail));
  playAll("device chain", chain);
  // Off: the keys are the views' again (F fits the focused patch view, no note).
  u.d.press("M");
  REQUIRE_FALSE(kb.active());
  const auto n0 = liveKeys();
  u.d.keyDown('a');
  u.d.keyUp('a');
  CHECK(liveKeys() == n0);
}

// BR.ui (user bug 2026-11-13_17-17-10: "the playhead leaves behind a ton of glitchy lines when it's running through a
// track"): the user's gesture — a click on a lane's empty space (the start marker and the insert marker there), Play —
// then the playhead running through the lane into a clip: every partial frame (the playhead's columns, the rulers' start
// marker) equals a full repaint of the same state. Before the fix each column's repaint drew the insert marker at the
// column's left edge (the trail), labels of the lane rulers lost slices, and the moved start marker was not repainted.
TEST_CASE("render: the lane playhead's partial repaints equal a full repaint (no trail)") {
  GpuOptions go;
  go.validation = true;
  auto gpu = GpuContext::create(go);
  REQUIRE_MESSAGE(gpu.ok(), (gpu.ok() ? "" : gpu.error().message));
  auto renderer = Renderer::create(**gpu, OffscreenTarget::kFormat, 3);
  REQUIRE(renderer.ok());
  auto tex = createSceneTextures(**renderer);
  REQUIRE(tex.ok());
  const PaintResources res = paintResources(*tex);
  constexpr uint32_t W = 1280, H = 800;

  FakeTransport port;
  ViewContext ctx(port);
  ctx.setDeviceLibrary(&library());
  REQUIRE(buildSketchProject(ctx.store()).ok());
  UiTree tree;
  tree.setViewport(W, H);
  tree.setResources(&res);
  auto shell = Shell::create(tree, ctx);
  REQUIRE(shell.ok());
  tree.layoutNow();
  ProjectPatchView* v = findProjectPatch(**shell);
  REQUIRE(v != nullptr);
  v->setShown(true, false);
  tree.layoutNow();
  const doc::Track& pad = *named(*ctx.doc(), "Pad");
  REQUIRE_FALSE(pad.clips.empty());
  const doc::ClipSlot clip = pad.clips.front();
  const Ticks bar = ticksPerBar(ctx.doc()->timeline->sigMap.signatureAt(0));
  v->setLaneTime(0.0, static_cast<double>(clip.end() + bar));
  v->focusBand(pad.id, false);
  tree.layoutNow();
  {  // its lane on the canvas, across it
    const ProjectPatch& pp = v->patch();
    const Band& band = pp.bands[pp.bandOf(pad.id)];
    const IRect cr = v->canvas().rect();
    circuit::CircuitCanvas::View cv;
    cv.zoom = std::min(1.5, (static_cast<double>(cr.w) - 40.0) / static_cast<double>(pp.laneX1 - pp.laneX0));
    cv.x = static_cast<double>(pp.laneX0) - 20.0 / cv.zoom;
    cv.y = static_cast<double>(band.laneY) - 0.5 * static_cast<double>(cr.h) / cv.zoom;
    v->canvas().setView(cv);
    tree.layoutNow();
  }
  const doc::ClipRef ref{pad.id, clip.id};
  v->openClip(ref);  // selected (its outline), in the clip view (as in the report)

  auto partial = OffscreenTarget::create(**gpu, W, H, 1);
  auto full = OffscreenTarget::create(**gpu, W, H, 1);
  REQUIRE((partial.ok() && full.ok()));
  DrawList dl;
  const DrawLayer layer{&dl, kTransientLayer, 0};
  FrameDesc ff;
  ff.layers = {&layer, 1};
  ff.clear = kSceneClearColor;
  double now = 0.0;
  for (int i = 0; i < 64 && (tree.needsFrame() || i == 0); ++i) {
    tree.update(now += 1.0 / 144.0);
    tex->text->beginFrame();
    dl.reset(W, H);
    tree.paint(dl, res, now);
  }
  REQUIRE((*partial)->draw(**renderer, ff).ok());
  uint64_t partialFrames = 0;
  auto frame = [&] {
    now += 1.0 / 144.0;
    tree.update(now);
    const Damage& d = tree.damage();
    if (!d.needsFrame()) return;
    tex->text->beginFrame();
    dl.reset(W, H);
    if (d.full()) {
      tree.paint(dl, res, now);
      REQUIRE((*partial)->draw(**renderer, ff).ok());
      return;
    }
    ClipRect rects[Damage::kMaxRects];
    const uint32_t n = views::test::paintDamage(tree, dl, res, now, rects);
    FrameDesc pf = ff;
    pf.full = false;
    pf.damage = {rects, n};
    REQUIRE((*partial)->draw(**renderer, pf).ok());
    ++partialFrames;
  };
  auto compare = [&](const std::string& what) {
    tex->text->beginFrame();
    dl.reset(W, H);
    tree.paint(dl, res, now);
    REQUIRE((*full)->draw(**renderer, ff).ok());
    auto a = (*partial)->read();
    auto b = (*full)->read();
    REQUIRE((a.ok() && b.ok()));
    const ImageDiff diff = compareImages(*a, *b, 0);
    if (diff.pixelsOverTolerance != 0) {
      (void)writePng(what + ".partial.png", *a);
      (void)writePng(what + ".full.png", *b);
    }
    CHECK_MESSAGE(diff.pixelsOverTolerance == 0, what << ": " << diff.pixelsOverTolerance << " px differ");
  };
  frame();
  compare("lane-playhead-settled");
  // The user's gesture: a click on the lane's empty space before the clip (the start marker there; the arrangement's
  // selection: the track, the insert marker), then Play.
  {
    const ProjectPatch& pp = v->patch();
    const LaneRect body = laneBody(pp, pp.bands[pp.bandOf(pad.id)]);
    const double kx = static_cast<double>(body.x1 - body.x0) / v->laneSpan();
    const Vec2 at = v->canvas().toDevice({body.x0 + static_cast<float>((static_cast<double>(clip.start() - bar / 2) - v->laneStart()) * kx),
                                          body.y0 + 0.7f * (body.y1 - body.y0)});
    for (const PointerType t : {PointerType::Move, PointerType::Down, PointerType::Up}) {
      PointerEvent e;
      e.type = t;
      e.pos = at;
      e.button = t == PointerType::Move ? PointerButton::None : PointerButton::Left;
      e.clickCount = 1;
      tree.dispatchPointer(e);
      frame();
    }
  }
  for (int i = 0; i < 4; ++i) frame();
  compare("lane-playhead-clicked");
  ctx.togglePlay();
  for (int i = 0; i < 4; ++i) frame();
  REQUIRE(v->lanePlayhead().has_value());
  // Small steps (about a device px each, as the playhead timer wakes) from before the clip into it.
  const double k = static_cast<double>(v->laneSpan()) / std::max(1.0f, (v->patch().laneX1 - v->patch().laneX0) * v->canvas().deviceScale());
  Ticks t = clip.start() - bar / 2;
  for (int i = 0; i < 160; ++i) {
    t += static_cast<Ticks>(std::max(1.0, k * 1.3));
    port.setPlayheadTick(t);
    frame();
    frame();
  }
  compare("lane-playhead-run");
  CHECK(partialFrames > 50);
  CHECK((*gpu)->validationErrorCount() == 0);
  tree.setRoot(std::make_unique<Widget>());
}

// BR.ui (user bug 2026-11-13_17-39-14: "need to be able to drag cables from mixer output"): a face-drawn port (the mixer's
// out socket, drawn jackInset() in from the face's edge) starts a cable where it is DRAWN, at any zoom — through real
// dispatch: from Mod's mixer out socket (already routed to the master) onto the Space return's Σ: a send, one undo step.
TEST_CASE("project patch (BR.ui): a cable drags out of the mixer's drawn out socket (zoomed in), onto a return: a send") {
  Ui u;
  u.ctx.setDetailVisible(false);  // the canvas takes the window's height
  u.show();
  ProjectPatchView& v = u.view();
  ProjectCanvas& c = v.canvas();
  const doc::TrackId mod = u.track("Mod").id, space = u.track("Space").id;
  const size_t h0 = u.history();
  const doc::OutputKind out0 = u.track("Mod").mixer.output.kind;
  const uint32_t mixer = u.pp().bands[u.pp().bandOf(mod)].mixer;
  const Node& n = u.pp().nodes[mixer];
  const int32_t k = n.output("out");
  REQUIRE(k >= 0);
  const uint32_t sv = v.scene().sceneNode[mixer];
  REQUIRE(sv != kNone);
  const Face* face = c.hostedFace(sv);
  REQUIRE(face != nullptr);
  const float inset = face->jackInset();
  REQUIRE(inset > 0.0f);
  // The socket as the face draws it (Face::paintJacks: worldSize().x − jackInset() in face units, at the node's scale).
  const circuit::SceneNode& sn = c.scene()->nodes[sv];
  const uint32_t pin = sn.firstPin + static_cast<uint32_t>(n.inputs.size()) + static_cast<uint32_t>(k);
  REQUIRE(pin < sn.firstPin + sn.pinCount);
  const graph::Point2 sock{sn.x + (face->worldSize().x - inset) * n.scale, c.scene()->pins[pin].y};
  // Zoomed in on the mixer (the socket well inside the 8 px pin reach of the block's edge), the Σ on the canvas too.
  const Node& sum = u.pp().nodes[u.pp().bands[u.pp().bandOf(space)].source];
  const graph::Point2 to{sum.x + sum.w * 0.5f, sum.y + sum.h * 0.5f};
  const IRect cr = c.rect();
  const double us = static_cast<double>(u.d.tree().scale());
  circuit::CircuitCanvas::View cv;
  cv.zoom = std::min(1.4, 0.9 * std::min(static_cast<double>(cr.w) / us / std::max(1.0, std::fabs(static_cast<double>(to.x - sock.x))),
                                         static_cast<double>(cr.h) / us / std::max(1.0, std::fabs(static_cast<double>(to.y - sock.y)))));
  REQUIRE(inset * n.scale * cv.zoom * us > 9.0);  // drawn more than the pin reach in from the edge
  cv.x = 0.5 * static_cast<double>(sock.x + to.x) - 0.5 * static_cast<double>(cr.w) / (cv.zoom * us);
  cv.y = 0.5 * static_cast<double>(sock.y + to.y) - 0.5 * static_cast<double>(cr.h) / (cv.zoom * us);
  c.setView(cv);
  u.d.tree().layoutNow();
  const Vec2 from = c.toDevice({sock.x, sock.y});
  const Vec2 drop = c.toDevice({to.x, to.y});
  REQUIRE(cr.contains(from));
  REQUIRE(cr.contains(drop));
  u.d.move(from);
  u.d.down(from);
  u.d.move({from.x + 12.0f, from.y + 6.0f});
  CHECK(c.drag() == ProjectCanvas::Drag::Wire);
  u.d.move(drop);
  u.d.up(drop);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  bool sent = false;
  for (const doc::Send& s : u.track("Mod").mixer.sends) sent = sent || (s.target == space && s.levelDb == 0.0f);
  CHECK(sent);
  CHECK(u.track("Mod").mixer.output.kind == out0);  // the existing route stays
}

// BR.ui (user 2026-11-13: the visual modules' settings sheet "covers the visual"; open it as an adjacent panel so the
// visual changes live while the settings are adjusted): through real dispatch on the canvas — SET opens the sheet as a
// floating panel right of the Spectrum face (its plot uncovered and still the face's), a row clicked there changes the
// option (one undo step), it follows the face's zoom, SET closes it; a face at the canvas's right edge opens it left.
TEST_CASE("project patch (BR.ui): a Spectrum's SET opens its sheet beside the face, the plot uncovered (real dispatch)") {
  Ui u(false);
  project_patch::MetaSpectrumIds ids;
  REQUIRE(buildSpectrumProject(u.ctx.store(), &ids).ok());
  u.show();
  ProjectPatchView& v = u.view();
  ProjectCanvas& c = v.canvas();
  const uint32_t mn = u.pp().nodeOf(ids.spectrum);
  REQUIRE(mn != kNone);
  auto place = [&](float fx, double zoom) {  // the face's left edge at `fx` of the canvas's width, at `zoom`
    const uint32_t sv = v.scene().sceneNode[mn];
    const circuit::SceneNode& sn = c.scene()->nodes[sv];
    const IRect cr = c.rect();
    const double us = static_cast<double>(u.d.tree().scale());
    circuit::CircuitCanvas::View cv;
    cv.zoom = zoom;
    cv.x = static_cast<double>(sn.x) - static_cast<double>(fx) * static_cast<double>(cr.w) / (zoom * us);
    cv.y = static_cast<double>(sn.y) - 0.3 * static_cast<double>(cr.h) / (zoom * us);
    c.setView(cv);
    u.d.tree().layoutNow();
    u.d.step();
  };
  struct Hit {
    MetaFace* face;
    circuit::SceneNode sn;
    float scale;
  };
  auto hit = [&] {
    const uint32_t sv = v.scene().sceneNode[mn];
    MetaFace* f = c.faceOn(sv);
    REQUIRE(f != nullptr);
    REQUIRE(f->type().id == "spectrum");
    const circuit::SceneNode& sn = c.scene()->nodes[sv];
    return Hit{f, sn, sn.scale > 0.0f ? sn.scale : 1.0f};
  };
  auto dev = [&](const Hit& h, Vec2 l) { return c.toDevice({h.sn.x + l.x * h.scale, h.sn.y + l.y * h.scale}); };
  // A face-local point of part `part` (through hitTest, inside `r`).
  auto partIn = [&](const Hit& h, int32_t part, const Rect& r) {
    for (float y = r.y0; y <= r.y1; y += 0.5f)
      for (float x = r.x0; x <= r.x1; x += 0.5f)
        if (h.face->hitTest({x, y}, 1.0f) == part) return Vec2{x + 1.0f, y + 1.0f};
    FAIL("no part " << part);
    return Vec2{};
  };
  place(0.2f, 1.2);
  Hit h = hit();
  const Vec2 fs = h.face->worldSize();
  const Rect faceR{0.0f, 0.0f, fs.x, fs.y};
  REQUIRE_FALSE(h.face->popupRect().has_value());
  const Vec2 set = partIn(h, 3, faceR);  // the SET chip (spectrum_face.cpp kSetPart)
  u.d.click(dev(h, set));
  u.d.step();
  h = hit();
  const std::optional<Rect> pr = h.face->popupRect();
  REQUIRE(pr.has_value());
  CHECK(pr->x0 >= fs.x);  // right of the face: the face (its plot) is not covered
  CHECK(pr->y0 >= 0.0f);
  CHECK(pr->y1 <= fs.y);
  const Vec2 plot{fs.x * 0.5f, fs.y * 0.6f};
  CHECK(h.face->hitTest(plot, 1.0f) == 0);  // the plot (kPlotPart), not the sheet
  // On screen, inside the canvas.
  const Vec2 p0 = dev(h, {pr->x0, pr->y0}), p1 = dev(h, {pr->x1, pr->y1});
  CHECK(c.rect().contains(p0));
  CHECK(c.rect().contains(p1));
  // A row of the sheet (DISPLAY: Peak line, a toggle) clicked through the canvas: the option changes, one undo step.
  const size_t h0 = u.history();
  const Vec2 row = partIn(h, 100, *pr);
  u.d.click(dev(h, row));
  u.d.step();
  CHECK(u.history() == h0 + 1);
  h = hit();
  REQUIRE(h.face->popupRect().has_value());  // still open
  // The sheet follows the face's zoom (face-local: the same rect at another zoom).
  place(0.2f, 0.9);
  h = hit();
  REQUIRE(h.face->popupRect().has_value());
  CHECK(h.face->popupRect()->x0 == doctest::Approx(pr->x0));
  // SET again: closed.
  u.d.click(dev(h, set));
  u.d.step();
  CHECK_FALSE(hit().face->popupRect().has_value());
  // The face near the canvas's right edge: the sheet opens on its left.
  place(0.62f, 1.2);
  h = hit();
  REQUIRE(c.rect().contains(dev(h, set)));
  u.d.click(dev(h, set));
  u.d.step();
  h = hit();
  const std::optional<Rect> pl = h.face->popupRect();
  REQUIRE(pl.has_value());
  CHECK(pl->x1 <= 0.0f);
  CHECK(c.rect().contains(dev(h, {pl->x0, pl->y0})));
}
