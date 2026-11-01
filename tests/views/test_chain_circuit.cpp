// T5.x-circuit-chain: a track's whole device chain as one circuit — the model (one boundary per device in chain order,
// each device's scene inside its box, the audio wires from each device's `out` terminal into the next one's `in`, the
// notes into the instrument and the last device into the mixer, racks as nested boundaries with their chains stacked
// and wired through split / Σ buses, modulation wires between devices), the Track-level view (opened focused on a
// device, read-only, a double-click in a boundary opens the Device level and the editor's Track button comes back,
// rebuilt on structure changes from cached device scenes), and the project patch view's opened band (every device of
// the band opened in place, racks to nested frames). T5.x-one-circuit: the Track-level view merged into the project
// patch view (one circuit view: Open in Circuit View, Ctrl+Alt+C and the editor's Track button open the band there);
// the chain model stays (opened racks are drawn with it). Golden: project-patch-zoom.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/chain_circuit.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/fake_transport.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::circuit;

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

doc::ops::Edit run(doc::DocumentStore& st, const doc::ops::EditResult& e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(st.dispatch(e->command).ok());
  return *e;
}

// The shell over one MIDI track "Keys": Operator → Auto Filter → EQ Eight → Saturator.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  doc::TrackId keys;
  std::vector<doc::DeviceId> chain;  // in chain order

  Ui() {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    keys = run(st, doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks[0];
    (void)run(st, doc::ops::addDevice(*st.current(), st.ids(), keys, "operator", doc::DeviceKind::Instrument));
    for (const char* fx : {"auto_filter", "eq_eight", "saturator"})
      (void)run(st, doc::ops::addDevice(*st.current(), st.ids(), keys, fx, doc::DeviceKind::AudioEffect));
    for (const doc::DevicePtr& dv : doc::findTrackPtr(*st.current(), keys)->devices) chain.push_back(dv->id);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    ctx.setSelectedTrack(keys);
    d.tree().layoutNow();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  ChainOptions options() const {
    ChainOptions o;
    o.devices = &library();
    return o;
  }
  template <class T>
  T* overlayOf() const {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* w = dynamic_cast<T*>(&shell->overlay().child(i))) return w;
    return nullptr;
  }
  CircuitEditor* editor() const { return overlayOf<CircuitEditor>(); }
};

graph::Point2 firstPoint(const CircuitScene& sc, uint32_t w) { return sc.points[sc.wires[w].firstPoint]; }
graph::Point2 lastPoint(const CircuitScene& sc, uint32_t w) { return sc.points[sc.wires[w].firstPoint + sc.wires[w].pointCount - 1]; }

bool orthogonal(const CircuitScene& sc, uint32_t w) {
  const SceneWire& sw = sc.wires[w];
  for (uint32_t i = 0; i + 1 < sw.pointCount; ++i) {
    const graph::Point2 a = sc.points[sw.firstPoint + i], b = sc.points[sw.firstPoint + i + 1];
    if (a.x != b.x && a.y != b.y) return false;
  }
  return sw.pointCount >= 2;
}

const ChainLink* linkBetween(const ChainCircuit& c, ChainLink::Kind k, uint32_t from, uint32_t to) {
  for (const ChainLink& l : c.links)
    if (l.kind == k && l.from == from && l.to == to) return &l;
  return nullptr;
}

}  // namespace

TEST_CASE("chain circuit: one boundary per device, in chain order, each holding its device's circuit") {
  Ui u;
  const ChainCircuit c = buildTrackCircuit(*u.ctx.doc(), u.keys, u.options());
  REQUIRE(c.boxes.size() == 4);
  REQUIRE(c.roots.size() == 4);
  CHECK(c.trackName == "Keys");
  const char* names[4] = {"Operator", "Auto Filter", "EQ Eight", "Saturator"};
  const char* patches[4] = {"operator", "auto_filter", "eq_eight", "saturator"};
  for (uint32_t i = 0; i < 4; ++i) {
    const ChainBox& b = c.boxes[c.roots[i]];
    CAPTURE(i);
    CHECK(b.kind == ChainBoxKind::Device);
    CHECK(b.device == u.chain[i]);
    CHECK(b.title == names[i]);
    CHECK(b.kindLabel == (i == 0 ? "Instrument" : "Audio Effect"));
    CHECK(b.instrument == (i == 0));
    CHECK_FALSE(b.missing);
    CHECK(c.boxOf(u.chain[i]) == c.roots[i]);
    // Left → right, not overlapping, top-aligned; the device's whole scene inside its boundary.
    if (i > 0) CHECK(b.x0 >= c.boxes[c.roots[i - 1]].x1 + chain_metrics::kGap - 0.5f);
    CHECK(b.y0 == c.boxes[c.roots[0]].y0);
    const graph::Patch* callee = library().library().find(patches[i]);
    REQUIRE(callee != nullptr);
    CHECK(b.nodeCount == callee->nodeCount());
    for (uint32_t v = b.firstNode; v < b.firstNode + b.nodeCount; ++v) {
      const SceneNode& n = c.scene->nodes[v];
      CHECK(c.boxOfNode[v] == c.roots[i]);
      if (n.hidden) continue;  // T4.15 / TMM.12: switched off by the device's settings (not drawn, no bounds)
      CHECK((n.x >= b.x0 && n.x + n.w <= b.x1 && n.y >= b.y0 + chain_metrics::kTitle && n.y + n.h <= b.y1));
    }
    // The hit tests: a point inside the boundary (its title strip) is this box.
    CHECK(c.deviceBoxAt(b.x0 + 4.0f, b.y0 + 4.0f) == c.roots[i]);
  }
  CHECK(c.deviceBoxAt(c.boxes[c.roots[0]].x1 + 4.0f, c.boxes[c.roots[0]].y0 + 4.0f) == kNoBox);  // the gap
  // The terminals: the track's note source left of everything, its mixer right of everything.
  REQUIRE(c.inputNode != kNoBox);
  REQUIRE(c.outputNode != kNoBox);
  CHECK(c.scene->specs[c.scene->nodes[c.inputNode].spec].title == "Notes");
  CHECK(c.scene->specs[c.scene->nodes[c.outputNode].spec].title == "Mixer");
  CHECK(c.scene->nodes[c.inputNode].x + c.scene->nodes[c.inputNode].w <= c.boxes[c.roots[0]].x0);
  CHECK(c.scene->nodes[c.outputNode].x >= c.boxes[c.roots[3]].x1);
  // The scene's bounds hold the track's band (what F fits).
  CHECK(c.scene->x0 <= c.x0);
  CHECK(c.scene->x1 >= c.x1);
}

TEST_CASE("chain circuit: real wires from each device's out into the next device's in, notes in, mixer out") {
  Ui u;
  const ChainCircuit c = buildTrackCircuit(*u.ctx.doc(), u.keys, u.options());
  REQUIRE(c.roots.size() == 4);
  const CircuitScene& sc = *c.scene;
  // Every effect has its audio input terminal found (the patch's first audio `in`), every device its output.
  for (uint32_t i = 0; i < 4; ++i) {
    const ChainBox& b = c.boxes[c.roots[i]];
    CAPTURE(i);
    REQUIRE(b.outNode != kNoBox);
    CHECK(sc.specs[sc.nodes[b.outNode].spec].symbol == Symbol::Output);
    if (i == 0) CHECK(b.inNode == kNoBox);  // an instrument: no audio input
    else {
      REQUIRE(b.inNode != kNoBox);
      CHECK(sc.specs[sc.nodes[b.inNode].spec].symbol == Symbol::Input);
    }
  }
  CHECK(sc.specs[sc.nodes[c.boxes[c.roots[1]].inNode].spec].title == "x");  // auto_filter's `in x : audio stereo`
  // Device i's out terminal → device i+1's in terminal: an audio wire, orthogonal, its ends on the terminals.
  for (uint32_t i = 0; i + 1 < 4; ++i) {
    CAPTURE(i);
    const ChainLink* l = linkBetween(c, ChainLink::Kind::Audio, c.roots[i], c.roots[i + 1]);
    REQUIRE(l != nullptr);
    const SceneNode& out = sc.nodes[c.boxes[c.roots[i]].outNode];
    const SceneNode& in = sc.nodes[c.boxes[c.roots[i + 1]].inNode];
    const graph::Point2 a = firstPoint(sc, l->sceneWire), b = lastPoint(sc, l->sceneWire);
    CHECK(a.x == out.x + out.w);
    CHECK(a.y == out.y + out.h * 0.5f);
    CHECK(b.x == in.x);
    CHECK(b.y == in.y + in.h * 0.5f);
    CHECK(orthogonal(sc, l->sceneWire));
    CHECK(sc.wires[l->sceneWire].style.rate == WireRate::Audio);
    CHECK_FALSE(sc.wires[l->sceneWire].style.modulation);
  }
  // The MIDI notes into the instrument (per voice: drawn doubled), the last device into the mixer.
  const ChainLink* notes = linkBetween(c, ChainLink::Kind::Notes, kNoBox, c.roots[0]);
  REQUIRE(notes != nullptr);
  CHECK(sc.wires[notes->sceneWire].style.poly);
  CHECK(lastPoint(sc, notes->sceneWire).x == c.boxes[c.roots[0]].x0);  // on the instrument's boundary
  const ChainLink* mixer = linkBetween(c, ChainLink::Kind::Audio, c.roots[3], kNoBox);
  REQUIRE(mixer != nullptr);
  CHECK(lastPoint(sc, mixer->sceneWire).x == sc.nodes[c.outputNode].x);
  // Nothing else crosses between devices without a modulation wire.
  size_t links = 0;
  for (const ChainLink& l : c.links) links += l.kind != ChainLink::Kind::Modulation ? 1u : 0u;
  CHECK(links == 5);

  // A modulation wire (the Auto Filter's LFO → the Saturator's drive): dashed, from the export terminal to the param
  // terminal, through the gutter above every box.
  doc::DocumentStore& st = u.ctx.store();
  const doc::ops::Edit w = run(st, doc::ops::connectWire(*st.current(), st.ids(), u.chain[1], "lfoSignal", u.chain[3], "drive"));
  REQUIRE(w.wires.size() == 1);
  const ChainCircuit m = buildTrackCircuit(*u.ctx.doc(), u.keys, u.options());
  const ChainLink* mod = linkBetween(m, ChainLink::Kind::Modulation, m.roots[1], m.roots[3]);
  REQUIRE(mod != nullptr);
  CHECK(mod->wire == w.wires[0]);
  const CircuitScene& ms = *m.scene;
  CHECK(ms.wires[mod->sceneWire].style.modulation);
  CHECK(orthogonal(ms, mod->sceneWire));
  float top = 0.0f;
  for (const ChainBox& b : m.boxes) top = std::min(top, b.y0);
  CHECK(ms.wires[mod->sceneWire].y0 < top);  // in the gutter
  const graph::Point2 a = firstPoint(ms, mod->sceneWire), b = lastPoint(ms, mod->sceneWire);
  const auto at = [&](graph::Point2 p, Symbol sym, const char* title) {
    for (uint32_t v = 0; v < ms.nodes.size(); ++v) {
      const SceneNode& n = ms.nodes[v];
      if (ms.specs[n.spec].symbol == sym && ms.specs[n.spec].title == title && p.y == n.y + n.h * 0.5f &&
          (p.x == n.x || p.x == n.x + n.w))
        return true;
    }
    return false;
  };
  CHECK(at(a, Symbol::Export, "lfoSignal"));
  CHECK(at(b, Symbol::Param, "drive"));
}

TEST_CASE("chain circuit: racks are nested boundaries with their chains stacked, split and summed") {
  Ui u;
  doc::DocumentStore& st = u.ctx.store();
  // Keys: Operator → Auto Filter → EQ Eight → Saturator → Audio Effect Rack [A: Utility | B: Saturator → Rack [C: EQ Eight]].
  const doc::DeviceId rack =
      run(st, doc::ops::addDevice(*st.current(), st.ids(), u.keys, std::string(doc::kAudioEffectRack), doc::DeviceKind::AudioEffect)).devices.at(0);
  (void)run(st, doc::ops::addChainWithDevice(*st.current(), st.ids(), rack, "utility", doc::DeviceKind::AudioEffect));
  (void)run(st, doc::ops::addChainWithDevice(*st.current(), st.ids(), rack, "saturator", doc::DeviceKind::AudioEffect));
  const doc::Device* r = doc::findDevice(*st.current(), rack).device;
  REQUIRE(r->rack->chains.size() == 2);
  const doc::ChainId chainB = r->rack->chains[1]->id;
  const doc::DeviceId inner = run(st, doc::ops::addDevice(*st.current(), st.ids(), {}, std::string(doc::kAudioEffectRack),
                                                          doc::DeviceKind::AudioEffect, doc::kAppend, chainB))
                                  .devices.at(0);
  (void)run(st, doc::ops::addChainWithDevice(*st.current(), st.ids(), inner, "eq_eight", doc::DeviceKind::AudioEffect));

  const ChainCircuit c = buildTrackCircuit(*u.ctx.doc(), u.keys, u.options());
  REQUIRE(c.roots.size() == 5);
  const uint32_t rb = c.roots[4];
  const ChainBox& R = c.boxes[rb];
  CHECK(R.kind == ChainBoxKind::Rack);
  CHECK(R.title == "Audio Effect Rack");
  CHECK(R.depth == 0);
  REQUIRE(R.children.size() == 2);
  // Chains stacked top → bottom inside the rack, each inside it.
  const ChainBox& A = c.boxes[R.children[0]];
  const ChainBox& B = c.boxes[R.children[1]];
  CHECK(A.kind == ChainBoxKind::Chain);
  CHECK(A.parent == rb);
  CHECK(A.depth == 1);
  CHECK(B.y0 >= A.y1);
  for (const ChainBox* ch : {&A, &B}) CHECK((ch->x0 >= R.x0 && ch->x1 <= R.x1 && ch->y0 >= R.y0 && ch->y1 <= R.y1));
  REQUIRE(A.children.size() == 1);
  CHECK(c.boxes[A.children[0]].title == "Utility");
  REQUIRE(B.children.size() == 2);
  CHECK(c.boxes[B.children[0]].title == "Saturator");
  const ChainBox& R2 = c.boxes[B.children[1]];
  CHECK(R2.kind == ChainBoxKind::Rack);
  CHECK(R2.device == inner);
  CHECK(R2.depth == 1);
  CHECK((R2.x0 >= B.x0 && R2.x1 <= B.x1 && R2.y0 >= B.y0 && R2.y1 <= B.y1));  // nested in its chain
  REQUIRE(R2.children.size() == 1);
  const ChainBox& C = c.boxes[R2.children[0]];
  CHECK(C.depth == 2);
  REQUIRE(C.children.size() == 1);
  const ChainBox& eq = c.boxes[C.children[0]];
  CHECK(eq.title == "EQ Eight");
  CHECK(eq.depth == 2);
  CHECK((eq.x0 >= R2.x0 && eq.x1 <= R2.x1 && eq.y0 >= R2.y0 && eq.y1 <= R2.y1));
  // Parents before children; the innermost box wins a hit test.
  for (uint32_t i = 0; i < c.boxes.size(); ++i)
    if (c.boxes[i].parent != kNoBox) CHECK(c.boxes[i].parent < i);
  CHECK(c.boxAt(eq.x0 + 2.0f, eq.y0 + 2.0f) == C.children[0]);
  CHECK(c.deviceBoxAt(C.x0 + 1.0f, C.y1 - 1.0f) == B.children[1]);  // a chain's padding: its rack
  // The Saturator before the rack feeds it; the rack's input splits into both chains' first devices; both chains end
  // on the rack's Σ (one summing net), which feeds the rack's output → the mixer.
  CHECK(linkBetween(c, ChainLink::Kind::Audio, c.roots[3], rb) != nullptr);
  const ChainLink* toA = linkBetween(c, ChainLink::Kind::Audio, rb, A.children[0]);
  const ChainLink* toB = linkBetween(c, ChainLink::Kind::Audio, rb, B.children[0]);
  REQUIRE(toA != nullptr);
  REQUIRE(toB != nullptr);
  CHECK(c.scene->wires[toA->sceneWire].net == c.scene->wires[toB->sceneWire].net);
  const ChainLink* fromA = linkBetween(c, ChainLink::Kind::Audio, A.children[0], rb);
  const ChainLink* fromB = linkBetween(c, ChainLink::Kind::Audio, B.children[1], rb);
  REQUIRE(fromA != nullptr);
  REQUIRE(fromB != nullptr);
  CHECK(c.scene->wires[fromA->sceneWire].net == c.scene->wires[fromB->sceneWire].net);
  CHECK(R.nodeCount == 1);
  CHECK(c.scene->specs[c.scene->nodes[R.firstNode].spec].symbol == Symbol::Summer);
  CHECK(linkBetween(c, ChainLink::Kind::Audio, B.children[0], B.children[1]) != nullptr);  // inside chain B
  CHECK(linkBetween(c, ChainLink::Kind::Audio, rb, kNoBox) != nullptr);  // the rack → the mixer
  CHECK_FALSE(c.scene->junctions.empty());  // the split and the sum branch
  // The devices' junctions are copied from their own scenes, only the links' nets searched: the same dots as a full
  // search of the composed scene.
  {
    CircuitScene full = *c.scene;
    const float include[4] = {c.x0, c.y0, c.x1, c.y1};
    indexScene(full, include);
    auto sorted = [](std::vector<graph::Point2> v) {
      std::sort(v.begin(), v.end(), [](const graph::Point2& a, const graph::Point2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
      return v;
    };
    const std::vector<graph::Point2> a = sorted(c.scene->junctions), b = sorted(full.junctions);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK((a[i].x == b[i].x && a[i].y == b[i].y));
  }

  // Rack mode (the project patch view draws it in the rack's block): the same boxes under "in" / "out" terminals.
  const ChainCircuit rc = buildRackCircuit(*u.ctx.doc(), rack, u.options());
  REQUIRE(rc.roots.size() == 1);
  CHECK(rc.boxes.size() == c.boxes.size() - 4);
  CHECK(rc.scene->specs[rc.scene->nodes[rc.inputNode].spec].title == "in");
  CHECK(rc.scene->specs[rc.scene->nodes[rc.outputNode].spec].title == "out");
}

TEST_CASE("one circuit: Open in Circuit View opens the project patch view on the band, framed on the device; no second window") {
  Ui u;
  // "Open in Circuit View" on the EQ Eight (the Device view's menu and title double-click call this).
  REQUIRE(u.ctx.openProjectCircuit({}, u.chain[2]));
  u.d.tree().layoutNow();
  u.d.step();
  project_patch::ProjectPatchView* v = findProjectPatch(*u.shell);
  REQUIRE(v != nullptr);
  CHECK(v->shown());
  CHECK(v->openedBand() == u.keys);
  CHECK(v->focusedDevice() == u.chain[2]);
  for (const doc::DeviceId d : u.chain) CHECK(v->deviceExpanded(d));  // the band opened: every device to its atoms
  CHECK(u.shell->overlay().childCount() == 0);  // no second circuit window: the project's own view
  // Framed and centred on the EQ Eight's frame.
  project_patch::LaneRect r;
  REQUIRE(v->deviceWorldRect(u.chain[2], r));
  const Vec2 a = v->canvas().toDevice({r.x0, r.y0}), b = v->canvas().toDevice({r.x1, r.y1});
  const IRect cr = v->canvas().rect();
  CHECK(std::fabs((a.x + b.x) * 0.5f - (static_cast<float>(cr.x) + static_cast<float>(cr.w) * 0.5f)) <= 1.0f);
  CHECK(std::fabs((a.y + b.y) * 0.5f - (static_cast<float>(cr.y) + static_cast<float>(cr.h) * 0.5f)) <= 1.0f);
  CHECK(a.x >= static_cast<float>(cr.x));
  CHECK(b.x <= static_cast<float>(cr.right()));
  // The project is still editable from here: a device dragged off… is not tested here; a param edit does not rebuild.
  const uint64_t rebuilds = v->stats().rebuilds;
  (void)run(u.ctx.store(), doc::ops::setDeviceParam(*u.ctx.doc(), u.chain[3], "drive", 0.3));
  CHECK(v->stats().rebuilds == rebuilds);
  // A double-click inside an opened device (below its title strip) edits its circuit: the Device level.
  REQUIRE(v->deviceWorldRect(u.chain[1], r));
  v->canvas().setView([&] {
    circuit::CircuitCanvas::View vw = v->canvas().view();
    vw.zoom = 1.0;
    vw.x = r.x0 - 20.0;
    vw.y = r.y0 - 20.0;
    return vw;
  }());
  u.d.doubleClick(v->canvas().toDevice({r.x0 + 6.0f, r.y0 + 40.0f}));
  u.d.step();
  u.d.step();
  CircuitEditor* ed = u.editor();
  REQUIRE(ed != nullptr);
  CHECK(ed->shownPatch() == "auto_filter");
  CHECK(ed->host() != nullptr);  // the project-device editor: edits are document commands
  // Its breadcrumb's Track goes back to the project view, the Auto Filter framed and focused.
  REQUIRE(ed->hasTrackAction());
  auto* track = dynamic_cast<Button*>(ed->find("bar/track"));
  REQUIRE(track != nullptr);
  u.d.tree().layoutNow();
  u.d.click({static_cast<float>(track->rect().x + 4), static_cast<float>(track->rect().y + 4)});
  u.d.step();
  u.d.step();
  CHECK(u.editor() == nullptr);
  CHECK(v->shown());
  CHECK(v->openedBand() == u.keys);
  CHECK(v->focusedDevice() == u.chain[1]);
  // A double-click on an opened device's title strip closes it; F closes the band and the focus.
  REQUIRE(v->deviceWorldRect(u.chain[3], r));
  v->canvas().setView([&] {
    circuit::CircuitCanvas::View vw = v->canvas().view();
    vw.zoom = 1.0;
    vw.x = r.x0 - 20.0;
    vw.y = r.y0 - 20.0;
    return vw;
  }());
  u.d.doubleClick(v->canvas().toDevice({r.x0 + 30.0f, r.y0 + 6.0f}));
  u.d.advance(0.5);
  CHECK_FALSE(v->deviceExpanded(u.chain[3]));
  v->fitAll(false);
  CHECK_FALSE(v->openedBand().valid());
  CHECK_FALSE(v->focusedDevice().valid());
  // View > Track Circuit (Ctrl+Alt+C): the selected track's band opened, no device focused.
  CHECK(u.d.press("Ctrl+Alt+C") == KeyResult::Action);
  u.d.advance(0.5);
  CHECK(v->openedBand() == u.keys);
  CHECK_FALSE(v->focusedDevice().valid());
  CHECK(u.shell->overlay().childCount() == 0);
}

TEST_CASE("project patch view: opening a band opens its devices in place (racks to nested frames); F closes them") {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  ctx.setDeviceLibrary(&library());
  REQUIRE(project_patch::buildSketchProject(ctx.store()).ok());
  doc::DocumentStore& st = ctx.store();
  const doc::Track* keys = nullptr;
  doc::forEachTrack(*st.current(), [&](const doc::Track& t, const doc::Track*) {
    if (t.name == "Keys") keys = &t;
  });
  REQUIRE(keys != nullptr);
  const doc::TrackId kid = keys->id;
  // A rack at the end of Keys' chain: [Utility | Saturator].
  const doc::DeviceId rack =
      run(st, doc::ops::addDevice(*st.current(), st.ids(), kid, std::string(doc::kAudioEffectRack), doc::DeviceKind::AudioEffect)).devices.at(0);
  (void)run(st, doc::ops::addChainWithDevice(*st.current(), st.ids(), rack, "utility", doc::DeviceKind::AudioEffect));
  (void)run(st, doc::ops::addChainWithDevice(*st.current(), st.ids(), rack, "saturator", doc::DeviceKind::AudioEffect));
  auto s = Shell::create(d.tree(), ctx);
  REQUIRE(s.ok());
  project_patch::ProjectPatchView* v = findProjectPatch(**s);
  REQUIRE(v != nullptr);
  d.tree().layoutNow();
  v->setShown(true, false);
  d.tree().layoutNow();
  v->fitAll(false);
  const size_t nodes = v->scene().scene->nodes.size();
  // Double-click the band's empty space: the band opens.
  const project_patch::ProjectPatch& pp = v->patch();
  const project_patch::Band& band = pp.bands[pp.bandOf(kid)];
  d.doubleClick(v->canvas().toDevice({band.x1 - 6.0f, band.y + band.h - 4.0f}));
  d.advance(0.5);
  CHECK(v->openedBand() == kid);
  const doc::Track* k2 = doc::findTrackPtr(*ctx.doc(), kid);
  for (const doc::DevicePtr& dv : k2->devices) CHECK(v->deviceExpanded(dv->id));
  CHECK(v->scene().scene->nodes.size() > nodes + 700);  // Operator's atoms and the rest
  // The rack opened to its chains: nested frames, its devices' atoms inside its block, wired to its block's pins.
  const circuit::ChainCircuit* rc = v->rackCircuit(rack);
  REQUIRE(rc != nullptr);
  CHECK(rc->boxes.size() == 5);  // the rack, two chains, two devices
  const project_patch::Node& rn = v->patch().nodes[v->patch().nodeOf(rack)];
  CHECK(rn.expanded);
  CHECK(rn.w >= rc->x1 - rc->x0);
  CHECK(v->scene().sceneNode[v->patch().nodeOf(rack)] == project_patch::kNone);
  CHECK(checkAgainstGraph(v->patch(), *ctx.doc(), *v->lowered()) == "");  // still the same graph
  // The band fills the view (zoomed onto it).
  const project_patch::Band& opened = v->patch().bands[v->patch().bandOf(kid)];
  const Vec2 a = v->canvas().toDevice({opened.x0, opened.y}), b = v->canvas().toDevice({opened.x1, opened.y + opened.h});
  const IRect cr = v->canvas().rect();
  CHECK(std::max(b.x - a.x, b.y - a.y) >= std::min(static_cast<float>(cr.w), static_cast<float>(cr.h)) * 0.5f);
  // F: the whole project again, the band's devices closed.
  v->fitAll(false);
  CHECK_FALSE(v->openedBand().valid());
  for (const doc::DevicePtr& dv : doc::findTrackPtr(*ctx.doc(), kid)->devices) CHECK_FALSE(v->deviceExpanded(dv->id));
  CHECK(v->scene().scene->nodes.size() == nodes);
  CHECK(v->rackCircuit(rack) == nullptr);
  d.tree().setRoot(std::make_unique<Widget>());
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
  ImageDiff diff;
  CHECK(pq::ui::test::matchesGolden(name, *img, *ref, true, &diff));
  MESSAGE(std::string(name) << ": " << pq::ui::test::describe(diff));
}
}  // namespace

TEST_CASE("render: project-patch-zoom matches the golden image") { golden("project-patch-zoom"); }
