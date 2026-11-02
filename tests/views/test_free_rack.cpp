// TX.free-rack (SPEC §15 2026-11-01 "resize, groups, and the free-device rack"): the Device view's free-device rack and
// the cables of the signal path. Free devices are not frames of the chain; the rack (an embedded project patch canvas
// pruned to the shown track's free devices: the circuit's free row) collapses and expands (the detail area grows by its
// height), resizes from its bottom edge (both remembered in the view prefs); a chain device dragged onto it leaves the
// chain, a block dragged out of it onto the chain goes in, a wire dragged out of it lands on a chain knob or input (one
// undo step each). The cable layer draws the chain's own path (notes → instrument → effects), wires into inputs, wires
// onto knobs (from the rack too), note-source wires; each reads on hover; free devices have no chain cable. Goldens
// device-free-rack (expanded) and device-free-rack-collapsed. Perf: 8 free LFOs wired to chain knobs.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/device_chain/wiring.h"
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

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + static_cast<float>(r.w) / 2.0f, static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f}; }

struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{2200, 900};  // the whole chain in view (the Auto Filter is wide)
  std::unique_ptr<Shell> shell;
  FreeRackIds ids;

  explicit Ui(uint32_t lfos = 3) {
    ctx.setDeviceLibrary(&library());
    REQUIRE(buildFreeRackProject(ctx.store(), lfos, &ids).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    body().setPosition(300.0f);
    ctx.setSelectedTrack(ids.lead);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
    d.tree().layoutNow();
    d.step();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }
  Splitter& body() const { return dynamic_cast<Splitter&>(*d.find("main/body")); }
  DeviceChainView& chain() const {
    DeviceChainView* v = findDeviceChain(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  CableLayer& layer() const {
    CableLayer* l = findCableLayer(*shell);
    REQUIRE(l != nullptr);
    return *l;
  }
  ProjectPatchView& rack() const {
    ProjectPatchView* r = chain().freeRackView();
    REQUIRE(r != nullptr);
    return *r;
  }
  void open() {
    chain().setFreeRackOpen(true);
    d.tree().layoutNow();
    d.step();
    REQUIRE(rack().shown());
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Track& lead() const { return *doc::findTrackPtr(*ctx.doc(), ids.lead); }
  bool isFree(doc::DeviceId id) const { return doc::findDevice(*ctx.doc(), id).device->free; }
  // A block of the rack: the middle of its top strip (a panel's title bar: the block's, not a control's).
  Vec2 rackBlockTop(doc::DeviceId id) const {
    const ProjectPatch& pp = rack().patch();
    const uint32_t n = pp.nodeOf(id);
    REQUIRE(n != kNone);
    const Node& x = pp.nodes[n];
    return rack().canvas().toDevice({x.x + x.w * 0.5f, x.y + 6.0f});
  }
  Vec2 tab() const { const IRect r = chain().freeRackRect(); return {static_cast<float>(r.x) + 40.0f, static_cast<float>(r.y) + 10.0f}; }
  Vec2 knob(doc::DeviceId dev, const char* param) {
    d.tree().layoutNow();
    devices::DevicePanel* p = chain().panelOf(dev);
    REQUIRE(p != nullptr);
    Widget* w = p->controlFor(param);
    REQUIRE(w != nullptr);
    return mid(w->rect());
  }
  std::vector<const CableLayer::Cable*> cablesOf(CableLayer::Kind k) const {
    std::vector<const CableLayer::Cable*> out;
    for (const CableLayer::Cable& c : layer().cables())
      if (c.kind == k) out.push_back(&c);
    return out;
  }
  const CableLayer::Cable* cable(doc::WireId w) const {
    for (const CableLayer::Cable& c : layer().cables())
      if (c.wire == w) return &c;
    return nullptr;
  }
  doc::WireId wireBetween(doc::DeviceId from, doc::DeviceId to, std::string_view param) const {
    for (const doc::Wire& w : doc::wiresOf(*ctx.doc()))
      if (w.from == from && w.to == to && w.param == param) return w.id;
    return {};
  }
};

}  // namespace

TEST_CASE("free rack: free devices are not frames of the chain; the rack holds exactly the track's free devices") {
  Ui u;
  DeviceChainView& c = u.chain();
  CHECK(c.frameOf(u.ids.sat) != nullptr);
  CHECK(c.frameOf(u.ids.filter) != nullptr);
  CHECK(c.frameOf(u.ids.util) != nullptr);
  for (const doc::DeviceId l : u.ids.lfos) CHECK(c.frameOf(l) == nullptr);
  CHECK(c.frameOf(u.ids.acc) == nullptr);
  CHECK(c.shownOrder().size() == 4);  // the synth and the three effects
  // Collapsed: the rack's canvas is hidden and has built nothing.
  CHECK_FALSE(c.freeRackOpen());
  CHECK(c.freeRackView() != nullptr);
  CHECK_FALSE(u.rack().shown());
  CHECK(u.rack().stats().rebuilds == 0);
  u.open();
  ProjectPatchView& r = u.rack();
  CHECK(r.freeRack());
  CHECK(r.rackTrack() == u.ids.lead);
  const ProjectPatch& pp = r.patch();
  CHECK(pp.scoped);
  REQUIRE(pp.bands.size() == 1);
  std::vector<doc::DeviceId> inRack;
  for (const Node& n : pp.nodes) {
    CHECK(n.kind == NodeKind::Device);
    CHECK(n.free);
    inRack.push_back(n.device);
  }
  std::vector<doc::DeviceId> want = u.ids.lfos;
  want.push_back(u.ids.acc);
  std::sort(inRack.begin(), inRack.end());
  std::sort(want.begin(), want.end());
  CHECK(inRack == want);
  // The same set as the circuit's free row.
  ProjectPatchView* main = findProjectPatch(*u.shell);
  REQUIRE(main != nullptr);
  main->setShown(true, false);
  u.d.tree().layoutNow();
  std::vector<doc::DeviceId> row;
  const uint32_t b = main->patch().bandOf(u.ids.lead);
  for (const Node& n : main->patch().nodes)
    if (n.band == b && n.kind == NodeKind::Device && n.free && n.capsule == kNone) row.push_back(n.device);
  std::sort(row.begin(), row.end());
  CHECK(row == want);
  main->setShown(false, false);
  // Only the wires among them are drawn inside: the first LFO into the accumulator's frequency.
  size_t wires = 0;
  for (const Edge& e : pp.edges) wires += e.kind == EdgeKind::Modulation ? 1 : 0;
  CHECK(wires == 1);
  // Another track: the rack follows it (no free devices there).
  const auto add = doc::ops::createTrack(*u.ctx.doc(), u.ctx.store().ids(), doc::TrackKind::Midi, u.ids.lead, "Other");
  REQUIRE(add.ok());
  REQUIRE(u.ctx.store().dispatch(add->command).ok());
  const doc::TrackId other = add->tracks.at(0);
  u.ctx.setSelectedTrack(other);
  u.d.tree().layoutNow();
  u.d.step();
  CHECK(u.rack().rackTrack() == other);
  CHECK(u.rack().patch().nodes.empty());
}

TEST_CASE("free rack: collapse / expand grows and shrinks the detail area by the rack; its edge resizes it; both remembered") {
  Ui u;
  const float p0 = u.body().position();
  const float h = u.chain().freeRackHeight();
  const IRect chain0 = u.chain().chainViewport();
  // A click on the tab expands it: the arrangement moves up by the body, the chain keeps its height.
  u.d.click(u.tab());
  u.d.tree().layoutNow();
  u.d.step();
  CHECK(u.chain().freeRackOpen());
  CHECK(u.ctx.viewPrefs().freeRackOpen);
  CHECK(u.body().position() == doctest::Approx(p0 + h + DeviceChainView::kRackHandleH));
  CHECK(u.chain().chainViewport().h == chain0.h);
  REQUIRE(u.rack().shown());
  CHECK(u.rack().canvas().rect().h >= static_cast<int32_t>(h) - 2);
  // The bottom edge drags its height (and the detail area with it); remembered on release.
  const IRect rr = u.chain().freeRackRect();
  const Vec2 grip{static_cast<float>(rr.x) + 200.0f, static_cast<float>(rr.bottom()) - 2.0f};
  u.d.drag(grip, {grip.x, grip.y + 40.0f}, 4);
  u.d.tree().layoutNow();
  CHECK(u.chain().freeRackHeight() == doctest::Approx(h + 40.0f));
  CHECK(u.ctx.viewPrefs().freeRackHeight == doctest::Approx(h + 40.0f));
  CHECK(u.body().position() == doctest::Approx(p0 + h + 40.0f + DeviceChainView::kRackHandleH));
  CHECK(u.chain().chainViewport().h == chain0.h);
  // Collapsed again: back to where it was; the canvas hides and follows no document change.
  u.d.click(u.tab());
  u.d.tree().layoutNow();
  u.d.step();
  CHECK_FALSE(u.chain().freeRackOpen());
  CHECK_FALSE(u.ctx.viewPrefs().freeRackOpen);
  CHECK(u.body().position() == doctest::Approx(p0));
  CHECK_FALSE(u.rack().shown());
  const uint64_t builds = u.rack().stats().rebuilds;
  const doc::DeviceId one[1] = {u.ids.lfos[0]};
  REQUIRE(u.ctx.store().dispatch(doc::ops::deleteDevices(*u.ctx.doc(), one)->command).ok());
  u.d.step();
  CHECK(u.rack().stats().rebuilds == builds);  // collapsed: nothing built
  // Expanded again it shows the document as it is now.
  u.chain().setFreeRackOpen(true);
  u.d.tree().layoutNow();
  u.d.step();
  CHECK(u.rack().patch().nodes.size() == u.ids.lfos.size());  // two LFOs and the accumulator
  CHECK(u.rack().patch().nodeOf(u.ids.lfos[0]) == kNone);
}

TEST_CASE("free rack: a chain device dragged onto the rack leaves the chain; a rack block dragged onto the chain goes in") {
  Ui u;
  u.open();
  // Out of the chain: the Saturator's title bar onto the rack.
  REQUIRE(u.chain().panelOf(u.ids.sat) != nullptr);
  const IRect fr = u.chain().frameOf(u.ids.sat)->rect();
  const Vec2 title{static_cast<float>(fr.x) + 60.0f, static_cast<float>(fr.y) + 10.0f};
  const IRect rr = u.chain().freeRackRect();
  const size_t h0 = u.history();
  u.d.drag(title, mid(rr), 8);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(u.isFree(u.ids.sat));
  CHECK(u.chain().frameOf(u.ids.sat) == nullptr);
  CHECK(u.rack().patch().nodeOf(u.ids.sat) != kNone);
  REQUIRE(u.ctx.undo());
  u.d.tree().layoutNow();
  u.d.step();
  CHECK_FALSE(u.isFree(u.ids.sat));
  CHECK(u.chain().frameOf(u.ids.sat) != nullptr);
  // Into the chain: an LFO block dropped on the Utility's left half goes in before the Utility.
  const doc::DeviceId lfo = u.ids.lfos[1];
  const Vec2 from = u.rackBlockTop(lfo);
  const IRect ur = u.chain().frameOf(u.ids.util)->rect();
  const Vec2 to{static_cast<float>(ur.x) + 10.0f, static_cast<float>(ur.y) + static_cast<float>(ur.h) * 0.5f};
  const size_t h1 = u.history();
  u.d.drag(from, to, 10);
  u.d.tree().layoutNow();
  u.d.step();
  CHECK(u.history() == h1 + 1);
  CHECK_FALSE(u.isFree(lfo));
  CHECK(u.lead().deviceIndex(lfo) == u.lead().deviceIndex(u.ids.util) - 1);
  CHECK(u.chain().frameOf(lfo) != nullptr);
  CHECK(u.rack().patch().nodeOf(lfo) == kNone);
  REQUIRE(u.ctx.undo());
  u.d.tree().layoutNow();
  u.d.step();
  CHECK(u.isFree(lfo));
  // Moved inside the rack, a block only moves (a nudge: view state, no undo step).
  const Vec2 a = u.rackBlockTop(lfo);
  const size_t h2 = u.history();
  u.d.drag(a, {a.x + 30.0f, a.y + 10.0f}, 6);
  u.d.step();
  CHECK(u.history() == h2);
  CHECK(u.isFree(lfo));
  CHECK(u.ctx.store().view().patchNudges.count(lfo.value) == 1);
}

TEST_CASE("free rack: a wire dragged out of the rack lands on a chain knob or an input socket; one undo step") {
  Ui u;
  u.open();
  ProjectPatchView& r = u.rack();
  const doc::DeviceId lfo = u.ids.lfos[2];
  const std::optional<Vec2> pin = r.outputPoint(lfo, "lfo");
  REQUIRE(pin.has_value());
  const size_t h0 = u.history();
  const Vec2 gain = u.knob(u.ids.util, "gain");
  u.d.drag({pin->x - 2.0f, pin->y}, gain, 12);
  u.d.step();
  CHECK_FALSE(u.layer().dragging());
  CHECK(u.history() == h0 + 1);
  const doc::WireId w = u.wireBetween(lfo, u.ids.util, "gain");
  REQUIRE(w.valid());
  const CableLayer::Cable* c = u.cable(w);
  REQUIRE(c != nullptr);
  CHECK(c->bDocked);  // on the knob
  CHECK(c->aDocked);  // at the LFO's pin in the rack
  CHECK_FALSE(c->input);
  // The accumulator's output onto the Utility's audio input socket: a wire into an input (a parallel route).
  std::vector<DeviceChainView::InputSocket> sockets = u.chain().inputSockets();
  const auto s = std::find_if(sockets.begin(), sockets.end(), [&](const DeviceChainView::InputSocket& x) { return x.device == u.ids.util; });
  REQUIRE(s != sockets.end());
  CHECK(s->input == "x");
  const std::optional<Vec2> accOut = r.outputPoint(u.ids.acc, "out");
  REQUIRE(accOut.has_value());
  u.d.drag({accOut->x + 2.0f, accOut->y}, mid(s->rect), 12);
  u.d.step();
  CHECK(u.history() == h0 + 2);
  const doc::WireId wi = u.wireBetween(u.ids.acc, u.ids.util, "x");
  REQUIRE(wi.valid());
  REQUIRE(u.cable(wi) != nullptr);
  CHECK(u.cable(wi)->input);
  CHECK(u.cable(wi)->bDocked);
  REQUIRE(u.ctx.undo());
  REQUIRE(u.ctx.undo());
  CHECK_FALSE(u.wireBetween(lfo, u.ids.util, "gain").valid());
}

TEST_CASE("free rack: the cables — the chain's path, wires into inputs, onto knobs, from the notes; each reads on hover") {
  Ui u;
  // A note-source wire too: the track's velocity onto the Utility's gain.
  REQUIRE(u.ctx.store().dispatch(doc::ops::connectNoteWire(*u.ctx.doc(), u.ctx.store().ids(), u.ids.lead, "velocity", u.ids.util, "gain")->command).ok());
  u.open();
  u.layer().refresh();
  // The chain: notes → synth (per voice), synth → Saturator → Auto Filter → Utility (audio). No free device takes part.
  const auto links = u.cablesOf(CableLayer::Kind::Chain);
  REQUIRE(links.size() == 4);
  CHECK(links[0]->rate == 2);
  for (size_t i = 1; i < links.size(); ++i) CHECK(links[i]->rate == 1);
  for (const CableLayer::Cable* k : links) {
    CHECK(k->input);
    CHECK(k->bDocked);
    CHECK_FALSE(k->wire.valid());
    CHECK(k->label.find("LFO") == std::string::npos);
  }
  // The wires: three LFOs onto knobs (docked at their pins in the rack and on the knobs), the accumulator into the Auto
  // Filter's sidechain input (an input socket), the note-source wire from the chain's left end. The LFO → accumulator
  // wire is inside the rack (its canvas draws it).
  for (const doc::DeviceId l : u.ids.lfos) {
    const doc::WireList& ws = doc::wiresOf(*u.ctx.doc());
    const auto it = std::find_if(ws.begin(), ws.end(), [&](const doc::Wire& w) { return w.from == l && w.to != u.ids.acc; });
    REQUIRE(it != ws.end());
    const CableLayer::Cable* c = u.cable(it->id);
    REQUIRE(c != nullptr);
    CHECK(c->aDocked);
    CHECK(c->bDocked);
    CHECK_FALSE(c->input);
  }
  const doc::WireId sc = u.wireBetween(u.ids.acc, u.ids.filter, "sc");
  REQUIRE(sc.valid());
  REQUIRE(u.cable(sc) != nullptr);
  CHECK(u.cable(sc)->input);
  CHECK(u.cable(sc)->bDocked);
  CHECK(u.cable(u.wireBetween(u.ids.lfos[0], u.ids.acc, "freq")) == nullptr);
  doc::WireId notes;
  for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc()))
    if (w.fromNoteSource()) notes = w.id;
  REQUIRE(notes.valid());
  REQUIRE(u.cable(notes) != nullptr);
  CHECK(u.cable(notes)->rate == 2);
  CHECK(u.cable(notes)->aDocked);
  // Hover-readable: every cable has a badge whose tooltip names both ends; hovering lights the cable.
  for (const CableLayer::Cable& c : u.layer().cables()) {
    const std::string name = c.wire.valid() ? "wire[" + std::to_string(c.wire.value) + "]" : "link[" + std::to_string(c.key) + "]";
    Widget* b = u.layer().find(name);
    REQUIRE_MESSAGE(b != nullptr, name);
    const std::string tip(b->tooltip());
    CHECK_MESSAGE(tip.find("\xE2\x86\x92") != std::string::npos, tip);
  }
  const Widget* sb = u.layer().find("wire[" + std::to_string(sc.value) + "]");
  REQUIRE(sb != nullptr);
  CHECK(std::string(sb->tooltip()).find("input") != std::string::npos);
  u.d.move(mid(sb->rect()));
  CHECK(u.layer().hoveredWire() == sc);
  const Widget* lb = u.layer().find("link[" + std::to_string(links[2]->key) + "]");
  REQUIRE(lb != nullptr);
  CHECK(std::string(lb->tooltip()).find("Saturator") != std::string::npos);
  u.d.move(mid(lb->rect()));
  CHECK(u.layer().hoveredKey() == links[2]->key);
  // Selectable and deletable (wires; a chain link only reads).
  u.d.click(mid(sb->rect()));
  CHECK(u.layer().selected() == sc);
  const size_t h0 = u.history();
  CHECK(u.d.press("Delete") == KeyResult::Widget);
  CHECK(u.history() == h0 + 1);
  CHECK(doc::findWire(*u.ctx.doc(), sc) == nullptr);
  REQUIRE(u.ctx.undo());
  // A switched-off device is passed by: the Auto Filter off, the Saturator feeds the Utility.
  const doc::DeviceId af[1] = {u.ids.filter};
  REQUIRE(u.ctx.store().dispatch(doc::ops::setDevicesActive(*u.ctx.doc(), af, false)->command).ok());
  u.d.tree().layoutNow();
  u.d.step();
  const auto links2 = u.cablesOf(CableLayer::Kind::Chain);
  REQUIRE(links2.size() == 3);
  CHECK(links2[2]->label.find("Saturator") == 0);
  CHECK(links2[2]->label.find("Utility") != std::string::npos);
}

TEST_CASE("free rack: a chain jack's cable can be dropped on an input socket or a block in the rack") {
  Ui u;
  // An LFO put into the chain has a jack; its cable onto the Auto Filter's sidechain socket, then onto the rack's
  // accumulator (its frequency control, or its pin).
  const doc::DeviceId lfo = u.ids.lfos[1];
  REQUIRE(u.chain().putIntoChainAt(lfo, 3));
  u.open();
  devices::DevicePanel* p = u.chain().panelOf(lfo);
  REQUIRE(p != nullptr);
  REQUIRE_FALSE(p->jacks().empty());
  const Vec2 jack = mid(p->jacks()[0].rect);
  std::vector<DeviceChainView::InputSocket> sockets = u.chain().inputSockets();
  const auto s = std::find_if(sockets.begin(), sockets.end(),
                              [&](const DeviceChainView::InputSocket& x) { return x.device == u.ids.filter && x.input == "sc"; });
  REQUIRE(s != sockets.end());
  const size_t h0 = u.history();
  u.d.drag(jack, mid(s->rect), 10);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(u.wireBetween(lfo, u.ids.filter, "sc").valid());
}

// ---- perf --------------------------------------------------------------------------------------------------------

TEST_CASE("free rack: 8 free LFOs wired to chain knobs — idle costs nothing; a frame, a cable refresh, a rack rebuild" * doctest::test_suite("perf")) {
  Ui u(8);
  REQUIRE(u.d.enableGpu().ok());
  for (int i = 0; i < 3; ++i) REQUIRE(u.d.render().ok());
  u.open();
  for (int i = 0; i < 3; ++i) REQUIRE(u.d.render().ok());
  // Idle: no frame requested, no timer, nothing damaged.
  u.d.advance(1.0);  // the panels settle at the rack's zoom (a one-shot re-record), then nothing runs
  REQUIRE(u.d.render().ok());
  CHECK_FALSE(u.d.tree().framesRequested());
  CHECK_FALSE(u.d.tree().needsPaint());
  CHECK(std::isinf(u.d.tree().nextWakeTime()));
  const uint64_t f0 = u.d.framesDrawn();
  u.d.advance(2.0);
  CHECK(u.d.framesDrawn() == f0);  // two idle seconds: not one frame
  CHECK(u.cablesOf(CableLayer::Kind::Wire).size() == 9);  // 8 LFOs onto knobs, the accumulator into the sidechain
  // The cable layer's work after a layout pass (every cable's ends recomputed, nothing moved).
  std::vector<double> refresh;
  for (int i = 0; i < 50; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    (void)u.layer().refresh();
    refresh.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(refresh.begin(), refresh.end());
  // A full repaint of the Device view (rack, chain, cables), the whole window rendered and read back; A/B against the
  // cables hidden and the rack collapsed (interleaved rounds: the machine's load moves all of them alike).
  auto frame = [&] {
    u.chain().markDirty();
    u.layer().markDirty();
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(u.d.render().ok());
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  };
  std::vector<double> full, noCables, collapsed;
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 4; ++i) full.push_back(frame());
    u.layer().setMode(CableMode::Hide);
    for (int i = 0; i < 4; ++i) noCables.push_back(frame());
    u.layer().setMode(CableMode::Show);
    u.chain().setFreeRackOpen(false);
    u.d.tree().layoutNow();
    for (int i = 0; i < 4; ++i) collapsed.push_back(frame());
    u.chain().setFreeRackOpen(true);
    u.d.tree().layoutNow();
  }
  auto median = [](std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  const double ms = median(full);
  MESSAGE("cable refresh (9 wires + 4 chain links): median " << refresh[refresh.size() / 2] << " ms; Device view frame: rack open + cables "
                                                           << ms << " ms, cables hidden " << median(noCables) << " ms, rack collapsed + cables "
                                                           << median(collapsed) << " ms; rack rebuild " << u.rack().stats().lastRebuildMs << " ms");
  CHECK(refresh[refresh.size() / 2] < 1.0);
  CHECK(ms < 16.0);
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

// Lead's chain under its expanded rack: three LFOs wired down to the Saturator's drive, the Auto Filter's frequency and
// the Utility's width, the accumulator into the Auto Filter's sidechain; the chain's own path along the frames' feet.
TEST_CASE("render: device-free-rack matches the golden image") { golden("device-free-rack"); }
// The same collapsed: its tab only; the cables rise to it.
TEST_CASE("render: device-free-rack-collapsed matches the golden image") { golden("device-free-rack-collapsed"); }
