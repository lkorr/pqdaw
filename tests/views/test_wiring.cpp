// T4.7 cross-device wiring in the views, through the real shell: a drag from a jack onto a knob is one undo step and
// draws a cable between them; a drag to another track's knob (hover its row: the Device view switches) works the same;
// cables follow scrolling in the same frame and damage only themselves; the badge's amount drag is one undo step of
// Param changes; Delete on a selected badge removes the wire; a delay-free loop shows on its badge; show / fade / hide;
// the knobs' rings show the static range and the polled sweeps; the views' knob travel equals the lowering's; WI.3: a
// cable whose declared type clashes with the target (a real output onto a bool input) is refused at the drop.
// GU.11: the sources are free LFO modules (their pins in the free rack), the knobs a Utility module's panel elements.
// Golden: device-wiring.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/main_path.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "all_free_helpers.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/graph/parse.h"
#include "pq/graph/patch_file.h"
#include "pq/ui/controls.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/devices/device_model.h"
#include "pq/views/panel/block_frame.h"
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

// GU.11 (one hierarchy): Mod: its instrument group → a Utility, an LFO off the path; Keys: its instrument group → a
// Utility, an LFO off the path. The modules are groups: an LFO's output is its group's `y` (a block of the free rack:
// the cable starts at its pin there, the free rack open), a Utility's knobs are its panel's elements, each bound to a
// value block of the group (a wire into one targets that block's `value`).
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{2200, 800};  // (GU.11: the instrument group's frame is wide: the Utility in view)
  std::unique_ptr<Shell> shell;
  doc::TrackId mod, keys;
  doc::DeviceId lfoA, utilA, inst, lfoB, util;

  Ui() {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    auto run = [&](const doc::ops::EditResult& e) {
      REQUIRE(e.ok());
      REQUIRE(st.dispatch(e->command).ok());
      return *e;
    };
    mod = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Mod")).tracks[0];
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, mod, "Keys")).tracks[0];
    lfoA = test_blocks::addModule(st, mod, "lfo", doc::ops::kOffPath);
    utilA = test_blocks::addModule(st, mod, "utility");
    lfoB = test_blocks::addModule(st, keys, "lfo", doc::ops::kOffPath);
    util = test_blocks::addModule(st, keys, "utility");
    inst = doc::mainPath(*st.current(), *doc::findTrackPtr(*st.current(), keys)).blocks.at(0)->id;
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(300.0f);
    ctx.setSelectedTrack(keys);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
    d.tree().layoutNow();
    d.step();
    view().setFreeRackOpen(true);
    d.tree().layoutNow();
    d.step();
  }
  ~Ui() {
    ctx.setModulationSource(nullptr);
    d.tree().setRoot(std::make_unique<Widget>());
  }
  DeviceChainView& view() const {
    DeviceChainView* v = findDeviceChain(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  CableLayer& layer() const {
    CableLayer* l = findCableLayer(*shell);
    REQUIRE(l != nullptr);
    return *l;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  // The value block of module `group` named `control` ("Width").
  doc::DeviceId control(doc::DeviceId group, const char* name) const {
    const doc::DeviceId c = test_blocks::member(*ctx.doc(), group, name);
    REQUIRE_MESSAGE(c.valid(), std::string(name));
    return c;
  }
  // Where a cable from a free LFO starts: its output pin in the free rack (the shown track's).
  Vec2 jack(doc::DeviceId lfo) {
    d.tree().layoutNow();
    project_patch::ProjectPatchView* r = view().freeRackView();
    REQUIRE(r != nullptr);
    const std::optional<Vec2> p = r->outputPoint(lfo, "y");
    REQUIRE(p.has_value());
    return {p->x - 2.0f, p->y};
  }
  // The knob of a module's control on its group's panel (a drop target of the chain).
  Vec2 knob(doc::DeviceId group, const char* name) {
    d.tree().layoutNow();
    const doc::DeviceId c = control(group, name);
    for (const DeviceChainView::ParamControl& pc : view().paramControls())
      if (pc.device == c && pc.param == doc::kValueParam) return mid(pc.rect);
    FAIL("no shown control " << std::string(name));
    return {};
  }
  const CableLayer::Cable* cable(doc::WireId w) const {
    for (const CableLayer::Cable& c : layer().cables())
      if (c.wire == w) return &c;
    return nullptr;
  }
};

float dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

// GU.11: the patches whose params are knobs (GU.1): the value block's (every module control) and every stdlib patch's
// (parsed from the files: the device library holds the blocks only).
std::vector<const graph::Patch*> paramPatches() {
  static const std::vector<graph::PatchFile> files = [] {
    std::vector<graph::PatchFile> out;
    for (const auto& e : std::filesystem::recursive_directory_iterator(std::filesystem::path(PQ_TEST_DATA_DIR) / ".." / "stdlib")) {
      if (e.path().extension() != ".pqp") continue;
      std::ifstream in(e.path(), std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      auto f = graph::parse(text, e.path().filename().string());
      if (f.ok()) out.push_back(std::move(*f));  // (a file importing another is left out: its own patches are elsewhere)
    }
    return out;
  }();
  std::vector<const graph::Patch*> out{&doc::valuePatch()};
  for (const graph::PatchFile& f : files)
    for (const graph::Patch& p : f.patches) out.push_back(&p);
  CHECK(out.size() >= 30);
  return out;
}

// The drawn wires of the project that `before` did not hold (the modules come with wires of their own).
doc::WireList newWires(const Ui& u, const doc::WireList& before) {
  doc::WireList out;
  for (const doc::Wire& w : all_free::drawnWires(*u.ctx.doc()))
    if (std::none_of(before.begin(), before.end(), [&](const doc::Wire& b) { return b.id == w.id; })) out.push_back(w);
  return out;
}

}  // namespace

TEST_CASE("wiring: a drag from a jack onto a knob is one undo step and draws a cable between them") {
  Ui u;
  const size_t h0 = u.history();
  const doc::WireList before = all_free::drawnWires(*u.ctx.doc());  // (the modules' own inner wires)
  const Vec2 from = u.jack(u.lfoB), to = u.knob(u.util, "Width");
  u.d.drag(from, to, 10);
  CHECK(u.history() == h0 + 1);
  const doc::WireList wires = newWires(u, before);
  REQUIRE(wires.size() == 1);
  CHECK(wires[0].from == u.lfoB);
  CHECK(wires[0].port == "y");
  CHECK(wires[0].to == u.control(u.util, "Width"));
  CHECK(wires[0].param == doc::kValueParam);
  CHECK(wires[0].law == doc::WireLaw::Add);  // TX.patching: a new wire adds in the knob's units, no amount
  CHECK(wires[0].amount == 1.0);
  CHECK_FALSE(u.layer().dragging());
  u.d.step();
  const CableLayer::Cable* c = u.cable(wires[0].id);
  REQUIRE(c != nullptr);
  CHECK(c->aDocked);
  CHECK(c->bDocked);
  CHECK(dist(c->a, from) < 4.0f);  // at the LFO's pin in the free rack
  CHECK(dist(c->b, to) < 40.0f);  // the top part of the knob
  MESSAGE("LFO cable rate " << int(c->rate));
  CHECK(c->rate <= 1);            // an LFO module's output: audio or param rate (not per voice)
  CHECK(u.layer().selected() == wires[0].id);  // the new wire is selected
  // A drop on nothing (empty space: right of the chain) makes no wire; Esc cancels.
  // (TX.orbit-ports: the LFO's main output now faces its cable: its pin is asked again)
  const IRect vr = u.view().chainViewport();
  u.d.drag(u.jack(u.lfoB), {static_cast<float>(vr.right() - 20), static_cast<float>(vr.y + vr.h / 2)}, 6);
  CHECK(u.history() == h0 + 1);
  const Vec2 from2 = u.jack(u.lfoB);
  u.d.down(from2);
  {  // out of the rack (the free rack's canvas hands its wire over to the layer there)
    const Vec2 out{static_cast<float>(vr.right() - 40), static_cast<float>(vr.y + vr.h / 2)};
    for (int i = 1; i <= 10; ++i)
      u.d.move({from2.x + (out.x - from2.x) * static_cast<float>(i) / 10.0f, from2.y + (out.y - from2.y) * static_cast<float>(i) / 10.0f});
  }
  CHECK(u.layer().dragging());
  u.d.press("Escape");
  CHECK_FALSE(u.layer().dragging());
  u.d.up(to);
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.ctx.undo());
  CHECK(newWires(u, before).empty());
}

TEST_CASE("wiring (WI.3): a cable whose declared type clashes is refused at the target: no-drop look, the status says why") {
  Ui u;
  doc::DocumentStore& st = u.ctx.store();
  // A value block (its `y` declared real) off Keys's path, in the free rack; an `and` block (bool inputs a, b) on it.
  auto e = doc::ops::addDevice(*st.current(), st.ids(), u.keys, std::string(doc::kValueDevice), doc::DeviceKind::AudioEffect,
                               doc::ops::kOffPath);
  REQUIRE(e.ok());
  REQUIRE(st.dispatch(e->command).ok());
  const doc::DeviceId value = e->devices.at(0);
  const doc::DeviceId logic = test_blocks::appendAfter(st, u.keys, "atom:and");
  u.d.tree().layoutNow();
  u.d.step();
  auto socket = [&](const char* input) {
    u.d.tree().layoutNow();
    for (const DeviceChainView::InputSocket& is : u.view().inputSockets())
      if (is.device == logic && is.input == input) return mid(is.rect);
    FAIL("no socket " << input);
    return Vec2{};
  };
  const doc::WireList before = all_free::drawnWires(*u.ctx.doc());
  const size_t h0 = u.history();
  const Vec2 from = u.jack(value), sock = socket("b");
  u.d.down(from);
  for (int i = 1; i <= 10; ++i)  // out of the free rack: its canvas hands the cable to the layer
    u.d.move({from.x + (sock.x - from.x) * static_cast<float>(i) / 10.0f, from.y + (sock.y - from.y) * static_cast<float>(i) / 10.0f});
  REQUIRE(u.layer().dragging());
  CHECK(u.layer().targetAt(sock).device == logic);
  CHECK(u.layer().clashWith(u.layer().targetAt(sock)) == doc::WireClash::RealIntoBool);
  CHECK(u.d.tree().cursor() == Cursor::NotAllowed);
  CHECK(u.ctx.status() == doc::wireClashText(doc::WireClash::RealIntoBool));
  // A real knob takes it (Crosshair, the drag's hint back in the status line); back over the bool socket: refused again.
  const Vec2 width = u.knob(u.util, "Width");
  u.d.move(width);
  CHECK(u.d.tree().cursor() == Cursor::Crosshair);
  CHECK(u.ctx.status().find("Drop the cable") != std::string::npos);
  u.d.move(sock);
  CHECK(u.d.tree().cursor() == Cursor::NotAllowed);
  u.d.up(sock);
  CHECK_FALSE(u.layer().dragging());
  CHECK(u.history() == h0);  // nothing connected
  CHECK(newWires(u, before).empty());
  CHECK(u.ctx.status() == doc::wireClashText(doc::WireClash::RealIntoBool));
  // The same drop onto the knob connects (a real into a real param).
  u.d.drag(u.jack(value), width, 10);
  CHECK(u.history() == h0 + 1);
  CHECK(newWires(u, before).size() == 1);
}

TEST_CASE("wiring: a cable to another track: hovering its row shows its chain, the drop wires across tracks") {
  Ui u;
  ArrangementView* arr = dynamic_cast<ArrangementView*>(u.shell->slot(ShellSlot::TrackArea).find("arrangement"));
  REQUIRE(arr != nullptr);
  int32_t modRow = -1;
  for (size_t r = 0; r < arr->rows().size(); ++r)
    if (arr->rows()[r].id == u.mod) modRow = static_cast<int32_t>(r);
  REQUIRE(modRow >= 0);
  const IRect lane = arr->rowLaneRect(modRow);
  const size_t h0 = u.history();
  const doc::WireList before = all_free::drawnWires(*u.ctx.doc());
  const Vec2 from = u.jack(u.lfoB);
  u.d.down(from);
  u.d.move({from.x + 5, from.y - 5});
  u.d.move(mid(lane));
  u.d.advance(0.5);  // hover: the Device view switches to Mod
  CHECK(u.ctx.selectedTrack() == u.mod);
  CHECK(u.view().track() == u.mod);
  CHECK(u.layer().dragging());  // the drag survives the chain being rebuilt
  const Vec2 to = u.knob(u.utilA, "Gain");
  u.d.move(to);
  u.d.up(to);
  CHECK(u.history() == h0 + 1);
  const doc::WireList wires = newWires(u, before);
  REQUIRE(wires.size() == 1);
  CHECK(wires[0].from == u.lfoB);
  CHECK(wires[0].to == u.control(u.utilA, "Gain"));
  CHECK(wires[0].param == doc::kValueParam);
  // Shown from Mod's side: the target end docks on the knob, the source end rises to Keys' row in the arrangement.
  u.d.step();
  const CableLayer::Cable* c = u.cable(wires[0].id);
  REQUIRE(c != nullptr);
  CHECK(c->bDocked);
  CHECK_FALSE(c->aDocked);
  CHECK(c->a.x == static_cast<float>(arr->lanesViewport().right()));
}

TEST_CASE("wiring: cables follow scrolling in the same frame and damage only themselves") {
  Ui u;
  const doc::WireList before = all_free::drawnWires(*u.ctx.doc());
  const std::string vp(doc::kValueParam);
  REQUIRE(u.ctx.store().dispatch(doc::ops::connectTravelWire(*u.ctx.doc(), u.ctx.store().ids(), u.lfoB, "y", u.control(u.util, "Width"), vp)->command).ok());
  REQUIRE(u.ctx.store().dispatch(doc::ops::connectTravelWire(*u.ctx.doc(), u.ctx.store().ids(), u.lfoA, "y", u.control(u.util, "Gain"), vp)->command).ok());
  u.d.step();
  u.d.step();
  const doc::WireId local = newWires(u, before)[0].id;
  // Scroll the chain sideways (two wide devices more: the chain is wider than the view).
  for (int k = 0; k < 6; ++k) (void)test_blocks::addModule(u.ctx.store(), u.keys, "saturator");
  u.d.step();
  u.d.step();
  const CableLayer::Cable mid0 = *u.cable(local);
  u.view().scrollChainBy(60);
  u.d.tree().update(u.d.now());  // one frame: layout → the layer's onLayout → damage
  const CableLayer::Cable* after = u.cable(local);
  REQUIRE(after != nullptr);
  const std::optional<Vec2> knobNow = u.view().paramPoint(u.control(u.util, "Width"), vp);
  REQUIRE(knobNow.has_value());
  CHECK(after->b.x == knobNow->x);  // the cable's end is on the knob in the same frame
  CHECK(after->b.x != mid0.b.x);
  IRect rects[Damage::kMaxRects];
  int64_t area = 0;
  const uint32_t n = u.d.tree().damageRects(rects, area);
  REQUIRE(n >= 1);
  bool covers = false;
  for (uint32_t i = 0; i < n; ++i) covers = covers || ui::intersect(rects[i], after->bounds) == after->bounds;
  CHECK(covers);
  // A scroll frame (the chain's layout + the cables' ends + their damage), indicative (dev build, shared machine).
  {
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < 200; ++k) {
      u.view().scrollChainBy(k % 2 == 0 ? 4 : -4);
      u.d.tree().update(u.d.now());
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 200.0;
    MESSAGE("scroll frame with cables (layout + ends + damage): " << us << " us");
  }
  // Nothing moves: the layer adds no damage (cables cost nothing while still; the free rack's cables settled first).
  u.d.advance(3.0);
  u.d.step();
  u.d.step();
  u.d.tree().invalidateAll();
  u.d.step();
  const uint64_t f0 = u.d.framesDrawn();
  u.d.advance(0.5);
  CHECK(u.d.framesDrawn() == f0);
}

TEST_CASE("wiring: the badge sets the amount (one undo step, Param only), Delete removes the wire; z⁻¹ on a loop") {
  Ui u;
  const doc::WireList before = all_free::drawnWires(*u.ctx.doc());
  const std::string vp(doc::kValueParam);
  REQUIRE(u.ctx.store().dispatch(doc::ops::connectTravelWire(*u.ctx.doc(), u.ctx.store().ids(), u.lfoB, "y", u.control(u.util, "Width"), vp)->command).ok());
  // The Utility's output into the LFO's rate: LFO → Width → Utility → Rate → LFO, a delay-free loop.
  REQUIRE(u.ctx.store().dispatch(doc::ops::connectTravelWire(*u.ctx.doc(), u.ctx.store().ids(), u.util, "y", u.control(u.lfoB, "Rate"), vp, 0.2)->command).ok());
  u.d.step();
  const doc::WireId w = newWires(u, before)[0].id, loop = newWires(u, before)[1].id;
  REQUIRE(u.cable(w) != nullptr);
  REQUIRE(u.cable(loop) != nullptr);
  CHECK(u.cable(w)->status == doc::WireStatus::Lowered);
  CHECK(u.cable(loop)->status == doc::WireStatus::Delayed);  // a delay-free loop: its badge shows z⁻¹
  const size_t h0 = u.history();
  const Vec2 b = mid(u.cable(w)->badge);
  CHECK(u.layer().badgeAt(b) == w);
  u.d.drag(b, {b.x, b.y - 60.0f}, 6);  // up: more
  CHECK(u.history() == h0 + 1);
  const doc::ChangeSet& cs = u.ctx.store().history().entries()[h0].changes;
  CHECK(cs.paramOnly());
  REQUIRE(cs.params.size() == 1);
  CHECK(cs.params[0] == doc::ParamRef::wireAmount(w));
  CHECK(doc::findWire(*u.ctx.doc(), w)->amount > 0.8);
  CHECK(u.layer().selected() == w);
  u.d.step();
  CHECK(u.d.press("Delete") == KeyResult::Widget);
  CHECK(u.history() == h0 + 2);
  CHECK(doc::findWire(*u.ctx.doc(), w) == nullptr);
  u.d.step();
  CHECK(u.cable(w) == nullptr);
  REQUIRE(u.ctx.undo());
  CHECK(doc::findWire(*u.ctx.doc(), w) != nullptr);
}

TEST_CASE("wiring: show / fade / hide (hidden: no cables, no work); the rings show the range and the sweeps") {
  Ui u;
  // GU.11: the rings are a leaf's (a native engine's face knob in the chain): a Wavetable after the Utility.
  const doc::DeviceId wt = [&] {
    auto e = doc::ops::addMetaDevice(*u.ctx.doc(), u.ctx.store().ids(), u.keys, "wavetable");
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    return e->devices.at(0);
  }();
  const doc::WireList before = all_free::drawnWires(*u.ctx.doc());
  REQUIRE(u.ctx.store().dispatch(doc::ops::connectTravelWire(*u.ctx.doc(), u.ctx.store().ids(), u.lfoB, "y", wt, "position", 0.25)->command).ok());
  u.d.step();
  auto wireCount = [&] {
    size_t n = 0;
    for (const doc::Wire& x : newWires(u, before))
      for (const CableLayer::Cable& c : u.layer().cables()) n += c.wire == x.id ? 1u : 0u;
    return n;
  };
  CableLayer& l = u.layer();
  CHECK(l.mode() == CableMode::Show);
  CHECK(wireCount() == 1);
  REQUIRE(u.ctx.actions().invoke(u.ctx.actions().find("view.cycleCables")));
  CHECK(l.mode() == CableMode::Fade);
  REQUIRE(u.ctx.actions().invoke(u.ctx.actions().find("view.cycleCables")));
  CHECK(l.mode() == CableMode::Hide);
  CHECK_FALSE(l.visible());
  u.d.step();
  CHECK(l.cables().empty());
  CHECK_FALSE(l.refresh());  // hidden: nothing computed
  l.setMode(CableMode::Show);
  u.d.step();
  CHECK(wireCount() == 1);
  // The ring: knob ± Σ|amount| (faint), and the polled sweep (bright).
  panel::BlockFrame* panel = u.view().panelOf(wt);
  REQUIRE(panel != nullptr);
  const auto pk = static_cast<float>(panel->target().position(doc::controlBinding(wt, "position")));
  CHECK(pk >= 0.0f);
  FixedModulationSource src;
  src.set(wt, "position", ModSweep{true, 0.1f, 0.2f});
  src.setLive(true);
  u.ctx.setModulationSource(&src);
  CHECK(u.view().ringsPolling());
  u.d.advance(0.1);
  CHECK(src.reads() >= 2);  // ≈ 30 Hz while live
  src.setLive(false);
  u.d.advance(0.2);
  CHECK_FALSE(u.view().ringsPolling());  // stops once the source is not live (after one last read)
  const uint64_t reads = src.reads();
  u.d.advance(0.5);
  CHECK(src.reads() == reads);
}

TEST_CASE("wiring (GU.1): the views' knob travel is the lowering's curve (the value block, every stdlib param)") {
  for (const graph::Patch* p : paramPatches()) {
    const devices::DeviceSignature sig = devices::signatureOf(*p);
    for (const devices::ParamInfo& pi : sig.params) {
      if (pi.isBool) continue;
      const auto n = p->findByName(pi.name);
      REQUIRE(n.has_value());
      const dsp::Curve c = doc::declaredCurve(p->paramDecl(*n));
      CAPTURE(p->name());
      CAPTURE(pi.name);
      CHECK(pi.declared == c);
      for (double x : {0.0, 0.13, 0.5, 0.77, 1.0}) {
        const double v = devices::fromPosition(pi, x);
        CHECK(v == dsp::evalCurve(c, x));
        if (pi.steps <= 1) CHECK(devices::toPosition(pi, v) == doctest::Approx(x).epsilon(1e-9));
      }
    }
  }
}

TEST_CASE("wiring (GU.1): on every preset a knob's travel is the lowering's; the menu offers the document's presets") {
  for (const graph::Patch* p : paramPatches()) {
    const devices::DeviceSignature sig = devices::signatureOf(*p);
    for (devices::ParamInfo pi : sig.params) {
      if (pi.isBool) continue;
      const auto n = p->findByName(pi.name);
      REQUIRE(n.has_value());
      const dsp::Curve declared = doc::declaredCurve(p->paramDecl(*n));
      CAPTURE(p->name());
      CAPTURE(pi.name);
      const std::vector<dsp::CurvePreset> offered = devices::knobCurves(pi);
      const std::vector<dsp::CurvePreset> doc = doc::curvePresets(declared);
      CHECK(offered == (doc.size() >= 2 ? doc : std::vector<dsp::CurvePreset>{}));
      for (const dsp::CurvePreset c : offered) {
        CAPTURE(static_cast<int>(c));
        pi.curve = doc::curveWithPreset(declared, c);
        for (double x : {0.0, 0.13, 0.5, 0.77, 1.0}) {
          const double v = devices::fromPosition(pi, x);
          CHECK(v == dsp::evalCurve(pi.curve, x));
          CHECK(devices::toPosition(pi, v) == doctest::Approx(x).epsilon(1e-9));
        }
      }
    }
  }
}

TEST_CASE("wiring cursors (T4.16c): jacks Crosshair; a dragged cable Crosshair over a knob, NotAllowed over nothing; badges ↕ and lit") {
  Ui u;
  auto at = [&](Vec2 p) {
    u.d.move(p);
    return u.d.tree().cursor();
  };
  const doc::WireList before = all_free::drawnWires(*u.ctx.doc());
  const Vec2 from = u.jack(u.lfoB), to = u.knob(u.util, "Width");
  CHECK(at(from) == Cursor::Crosshair);
  u.d.down(from);
  CHECK(u.d.tree().cursor() == Cursor::Crosshair);
  const IRect vr = u.view().rect();
  const Vec2 nothing{static_cast<float>(vr.right() - 20), static_cast<float>(vr.y + vr.h / 2)};  // right of the chain
  u.d.move({from.x + 5.0f, from.y + 5.0f});
  u.d.move(nothing);
  CHECK(u.d.tree().cursor() == Cursor::NotAllowed);
  u.d.move(to);
  CHECK(u.d.tree().cursor() == Cursor::Crosshair);
  u.d.up(to);
  REQUIRE(newWires(u, before).size() == 1);  // the drop wired it, as the cursor said
  const doc::WireId w = newWires(u, before)[0].id;
  u.d.step();
  // The badge: hovering it lights its cable (only that cable repaints). TX.patching: the wire this drop made adds in
  // the knob's units and has no amount to drag (a Pointer, not ResizeNS; a migrated Travel wire's badge still drags).
  REQUIRE(u.cable(w) != nullptr);
  const Vec2 b = mid(u.cable(w)->badge);
  u.d.move({b.x + 60.0f, b.y + 60.0f});
  u.d.step();
  CHECK_FALSE(u.layer().hoveredWire().valid());
  CHECK(at(b) == Cursor::Pointer);
  CHECK(u.layer().hoveredWire() == w);
  IRect rects[Damage::kMaxRects];
  int64_t area = 0;
  const uint32_t n = u.d.tree().damageRects(rects, area);
  bool covers = false;
  for (uint32_t i = 0; i < n; ++i) covers = covers || ui::intersect(rects[i], u.cable(w)->bounds) == u.cable(w)->bounds;
  CHECK(covers);
  u.d.move({b.x + 60.0f, b.y + 60.0f});
  CHECK_FALSE(u.layer().hoveredWire().valid());
}

TEST_CASE("render: device-wiring matches the golden image") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize("device-wiring", w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene("device-wiring", {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/device-wiring.png";
  auto ref = readPng(path);
  if (!ref) (void)writePng("device-wiring.actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden("device-wiring", *img, *ref, true, &d));
  MESSAGE("device-wiring: " << pq::ui::test::describe(d));
}
