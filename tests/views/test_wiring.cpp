// T4.7 cross-device wiring in the views, through the real shell: a drag from a jack onto a knob is one undo step and
// draws a cable between them; a drag to another track's knob (hover its row: the Device view switches) works the same;
// cables follow scrolling in the same frame and damage only themselves; the badge's amount drag is one undo step of
// Param changes; Delete on a selected badge removes the wire; a delay-free loop shows on its badge; show / fade / hide;
// the knobs' rings show the static range and the polled sweeps; the views' knob travel equals the lowering's.
// Golden: device-wiring.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/ui/controls.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/devices/device_model.h"
#include "pq/views/devices/device_panel.h"
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

// Mod: an LFO; Keys: BasicSynth → LFO → Utility (shown in the Device view).
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  doc::TrackId mod, keys;
  doc::DeviceId lfoA, inst, lfoB, util;

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
    lfoA = run(doc::ops::addDevice(*st.current(), st.ids(), mod, "lfo_device", doc::DeviceKind::AudioEffect)).devices[0];
    lfoB = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "lfo_device", doc::DeviceKind::AudioEffect)).devices[0];
    util = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "utility", doc::DeviceKind::AudioEffect)).devices[0];
    inst = doc::findTrackPtr(*st.current(), keys)->devices.at(0)->id;
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(300.0f);
    ctx.setSelectedTrack(keys);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
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
  Vec2 jack(doc::DeviceId dev) {
    d.tree().layoutNow();
    devices::DevicePanel* p = view().panelOf(dev);
    REQUIRE(p != nullptr);
    REQUIRE_FALSE(p->jacks().empty());
    return mid(p->jacks()[0].rect);
  }
  Vec2 knob(doc::DeviceId dev, const char* param) {
    d.tree().layoutNow();
    devices::DevicePanel* p = view().panelOf(dev);
    REQUIRE(p != nullptr);
    Widget* w = p->controlFor(param);
    REQUIRE(w != nullptr);
    return mid(w->rect());
  }
  const CableLayer::Cable* cable(doc::WireId w) const {
    for (const CableLayer::Cable& c : layer().cables())
      if (c.wire == w) return &c;
    return nullptr;
  }
};

float dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

}  // namespace

TEST_CASE("wiring: a drag from a jack onto a knob is one undo step and draws a cable between them") {
  Ui u;
  const size_t h0 = u.history();
  const Vec2 from = u.jack(u.lfoB), to = u.knob(u.util, "width");
  u.d.drag(from, to, 10);
  CHECK(u.history() == h0 + 1);
  const doc::WireList& wires = doc::wiresOf(*u.ctx.doc());
  REQUIRE(wires.size() == 1);
  CHECK(wires[0].from == u.lfoB);
  CHECK(wires[0].port == "lfo");
  CHECK(wires[0].to == u.util);
  CHECK(wires[0].param == "width");
  CHECK(wires[0].amount == doc::kDefaultWireAmount);
  CHECK_FALSE(u.layer().dragging());
  u.d.step();
  const CableLayer::Cable* c = u.cable(wires[0].id);
  REQUIRE(c != nullptr);
  CHECK(c->aDocked);
  CHECK(c->bDocked);
  CHECK(dist(c->a, from) < 2.0f);
  CHECK(dist(c->b, to) < 40.0f);  // the top part of the knob
  CHECK(c->rate == 1);            // an LFO: audio rate
  CHECK(u.layer().selected() == wires[0].id);  // the new wire is selected
  // A drop on nothing (empty space) makes no wire; Esc cancels.
  u.d.drag(from, {from.x, from.y - 120.0f}, 6);
  CHECK(u.history() == h0 + 1);
  u.d.down(from);
  u.d.move({from.x + 30, from.y + 30});
  CHECK(u.layer().dragging());
  u.d.press("Escape");
  CHECK_FALSE(u.layer().dragging());
  u.d.up(to);
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.ctx.undo());
  CHECK(doc::wiresOf(*u.ctx.doc()).empty());
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
  const Vec2 from = u.jack(u.lfoB);
  u.d.down(from);
  u.d.move({from.x + 5, from.y - 5});
  u.d.move(mid(lane));
  u.d.advance(0.5);  // hover: the Device view switches to Mod
  CHECK(u.ctx.selectedTrack() == u.mod);
  CHECK(u.view().track() == u.mod);
  CHECK(u.layer().dragging());  // the drag survives the chain being rebuilt
  const Vec2 to = u.knob(u.lfoA, "depth");
  u.d.move(to);
  u.d.up(to);
  CHECK(u.history() == h0 + 1);
  const doc::WireList& wires = doc::wiresOf(*u.ctx.doc());
  REQUIRE(wires.size() == 1);
  CHECK(wires[0].from == u.lfoB);
  CHECK(wires[0].to == u.lfoA);
  CHECK(wires[0].param == "depth");
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
  REQUIRE(u.ctx.store().dispatch(doc::ops::connectWire(*u.ctx.doc(), u.ctx.store().ids(), u.lfoB, "lfo", u.util, "width")->command).ok());
  REQUIRE(u.ctx.store().dispatch(doc::ops::connectWire(*u.ctx.doc(), u.ctx.store().ids(), u.lfoA, "lfo", u.inst, "bright")->command).ok());
  u.d.step();
  u.d.step();
  const doc::WireId local = doc::wiresOf(*u.ctx.doc())[0].id;
  // Scroll the chain sideways (two wide devices more: the chain is wider than the view).
  for (const char* dev : {"eq_eight", "saturator"})
    REQUIRE(u.ctx.store().dispatch(doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.keys, dev,
                                                       doc::DeviceKind::AudioEffect)->command).ok());
  u.d.step();
  u.d.step();
  const CableLayer::Cable mid0 = *u.cable(local);
  u.view().scrollChainBy(60);
  u.d.tree().update(u.d.now());  // one frame: layout → the layer's onLayout → damage
  const CableLayer::Cable* after = u.cable(local);
  REQUIRE(after != nullptr);
  const Vec2 jackNow = u.jack(u.lfoB);
  CHECK(after->a.x == jackNow.x);  // the cable's end is on the jack in the same frame
  CHECK(after->a.x != mid0.a.x);
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
  // Nothing moves: the layer adds no damage (cables cost nothing while still).
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
  REQUIRE(u.ctx.store().dispatch(doc::ops::connectWire(*u.ctx.doc(), u.ctx.store().ids(), u.lfoB, "lfo", u.util, "width")->command).ok());
  REQUIRE(u.ctx.store().dispatch(doc::ops::connectWire(*u.ctx.doc(), u.ctx.store().ids(), u.lfoB, "lfo", u.lfoB, "rate", 0.2)->command).ok());
  u.d.step();
  const doc::WireId w = doc::wiresOf(*u.ctx.doc())[0].id, loop = doc::wiresOf(*u.ctx.doc())[1].id;
  REQUIRE(u.cable(w) != nullptr);
  REQUIRE(u.cable(loop) != nullptr);
  CHECK(u.cable(w)->status == doc::WireStatus::Lowered);
  CHECK(u.cable(loop)->status == doc::WireStatus::Delayed);  // the LFO's own rate: a delay-free loop, its badge shows z⁻¹
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
  REQUIRE(u.ctx.store().dispatch(doc::ops::connectWire(*u.ctx.doc(), u.ctx.store().ids(), u.lfoB, "lfo", u.util, "width", 0.25)->command).ok());
  u.d.step();
  CableLayer& l = u.layer();
  CHECK(l.mode() == CableMode::Show);
  CHECK(l.cables().size() == 1);
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
  CHECK(l.cables().size() == 1);
  // The ring: knob ± Σ|amount| (faint), and the polled sweep (bright).
  devices::DevicePanel* panel = u.view().panelOf(u.util);
  REQUIRE(panel != nullptr);
  const devices::ParamInfo* info = panel->signature().findParam("width");
  REQUIRE(info != nullptr);
  const auto pk = static_cast<float>(devices::toPosition(*info, panel->paramValue("width")));
  CHECK(pk > 0.0f);
  FixedModulationSource src;
  src.set(u.util, "width", ModSweep{true, 0.1f, 0.2f});
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

TEST_CASE("wiring: the views' knob travel is the lowering's (every stock device param)") {
  for (const doc::DeviceInfo& di : library().catalog().devices()) {
    const graph::Patch* p = library().library().find(di.patch);
    REQUIRE(p != nullptr);
    const devices::DeviceSignature sig = devices::signatureOf(*p);
    for (const devices::ParamInfo& pi : sig.params) {
      if (pi.isBool) continue;
      const auto n = p->findByName(pi.name);
      REQUIRE(n.has_value());
      const doc::ParamTravel t = doc::paramTravel(p->paramDecl(*n));
      CAPTURE(di.patch);
      CAPTURE(pi.name);
      for (double x : {0.0, 0.13, 0.5, 0.77, 1.0}) {
        const double v = devices::fromPosition(pi, x);
        CHECK(doc::travelPosition(t, v) == doctest::Approx(devices::toPosition(pi, v)).epsilon(1e-9));
        if (pi.steps <= 1) CHECK(doc::travelValue(t, x) == doctest::Approx(v).epsilon(1e-9));
      }
    }
  }
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
