// TX.all-free (SPEC §15 2026-11-03 "everything is free; the chain is just cables") through the real shell and real
// event dispatch: the Device view lists a track's MAIN PATH (doc::mainPath: the main links from the source to the
// mixer), in signal order; its list gestures rewire — a title bar dragged elsewhere is dissolve-at-the-old-place +
// splice-at-the-new, Delete dissolves (the neighbours joined), a block dropped from the free rack is spliced in — each
// one undo step, the path staying connected; a broken path lists what is still on it; a block nothing connects is in
// the free rack only. In the project patch view a block is placed anywhere (a move is view state: the wiring decides
// the path), and a cable drawn onto a band's mixer ends the path there.
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "all_free_helpers.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
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

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + static_cast<float>(r.w) / 2.0f, static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f}; }

// A project with one MIDI track Keys: source → BasicSynth → Utility → Saturator → mixer, and an LFO off the path (it
// modulates the Utility's gain).
struct Doc {
  doc::TrackId keys;
  doc::DeviceId synth, util, sat, lfo;
  doc::WireId lfoWire;
  void build(doc::DocumentStore& st) {
    auto run = [&](const doc::ops::EditResult& e) {
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
      REQUIRE(st.dispatch(e->command).ok());
      return *e;
    };
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks.at(0);
    synth = doc::findTrackPtr(*st.current(), keys)->devices.at(0)->id;
    util = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "utility", doc::DeviceKind::AudioEffect)).devices.at(0);
    sat = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "saturator", doc::DeviceKind::AudioEffect)).devices.at(0);
    lfo = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "lfo_device", doc::DeviceKind::AudioEffect, doc::ops::kOffPath))
              .devices.at(0);
    lfoWire = run(doc::ops::connectWire(*st.current(), st.ids(), lfo, "lfo", util, "gain")).wires.at(0);
  }
};

// The shell in the Device view on Keys, the free rack open.
struct DeviceUi : Doc {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{2200, 900};
  std::unique_ptr<Shell> shell;

  DeviceUi() {
    ctx.setDeviceLibrary(&library());
    build(ctx.store());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(300.0f);
    ctx.setSelectedTrack(keys);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
    d.tree().layoutNow();
    d.step();
    chain().setFreeRackOpen(true);
    d.tree().layoutNow();
    d.step();
  }
  ~DeviceUi() { d.tree().setRoot(std::make_unique<Widget>()); }
  DeviceChainView& chain() const {
    DeviceChainView* v = findDeviceChain(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  ProjectPatchView& rack() const {
    ProjectPatchView* r = chain().freeRackView();
    REQUIRE(r != nullptr);
    return *r;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Track& track() const { return *doc::findTrackPtr(*ctx.doc(), keys); }
  doc::MainPath path() const { return doc::mainPath(*ctx.doc(), track()); }
  std::vector<doc::DeviceId> onPath() const {
    std::vector<doc::DeviceId> v;
    for (const doc::Device* x : path().blocks) v.push_back(x->id);
    return v;
  }
  // The Device view's frames, left to right (what the list shows).
  std::vector<doc::DeviceId> listed() {
    d.tree().layoutNow();
    std::vector<std::pair<int32_t, doc::DeviceId>> f;
    for (const doc::DevicePtr& x : track().devices)
      if (Widget* w = chain().frameOf(x->id)) f.emplace_back(w->rect().x, x->id);
    std::sort(f.begin(), f.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<doc::DeviceId> v;
    for (const auto& [x, id] : f) v.push_back(id);
    return v;
  }
  bool inRack(doc::DeviceId id) {
    d.tree().layoutNow();
    d.step();
    return rack().patch().nodeOf(id) != kNone;
  }
  IRect frame(doc::DeviceId id) {
    d.tree().layoutNow();
    Widget* f = chain().frameOf(id);
    REQUIRE(f != nullptr);
    return f->rect();
  }
  Vec2 title(doc::DeviceId id) {
    const IRect r = frame(id);
    return {static_cast<float>(r.x + r.w / 2), static_cast<float>(r.y + 18)};
  }
  // Whether a main link joins a → b (a source / mixer end: an invalid id).
  bool linked(doc::DeviceId a, doc::DeviceId b) const {
    for (const doc::Wire& w : doc::wiresOf(*ctx.doc()))
      if (w.mainLink() && !w.fromNoteSource() && w.from == a && w.to == b && !w.toMixer()) return true;
    return false;
  }
  bool intoMixer(doc::DeviceId a) const {
    for (const doc::Wire& w : doc::wiresOf(*ctx.doc()))
      if (w.mainLink() && w.from == a && w.toMixer() && w.mixer == keys) return true;
    return false;
  }
};

using Ids = std::vector<doc::DeviceId>;

}  // namespace

TEST_CASE("all-free (Device view): the list is the main path in signal order; a block off it is in the free rack only") {
  DeviceUi u;
  CHECK(u.onPath() == Ids{u.synth, u.util, u.sat});
  CHECK(u.listed() == Ids{u.synth, u.util, u.sat});
  CHECK(u.chain().frameOf(u.lfo) == nullptr);
  CHECK(u.inRack(u.lfo));
  CHECK_FALSE(u.inRack(u.util));
  // The list follows the cables, not the blocks' order in the track: the path rewired Saturator before Utility (by
  // the builder the list's reorder uses) lists them so, while the track's block set keeps its order.
  REQUIRE(u.ctx.store().dispatch(doc::ops::moveOnPath(*u.ctx.doc(), u.ctx.store().ids(), &library(), u.sat, u.keys, 1)->command).ok());
  CHECK(u.listed() == Ids{u.synth, u.sat, u.util});
  CHECK(u.track().deviceIndex(u.util) < u.track().deviceIndex(u.sat));
  // An unconnected block (the node menu's spawn, a cable deleted) is in the rack and not in the list.
  auto add = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.keys, "eq_eight", doc::DeviceKind::AudioEffect,
                                 doc::ops::kOffPath);
  REQUIRE(add.ok());
  REQUIRE(u.ctx.store().dispatch(add->command).ok());
  const doc::DeviceId eq = add->devices.at(0);
  CHECK(u.chain().frameOf(eq) == nullptr);
  CHECK(u.inRack(eq));
  CHECK(all_free::offPath(*u.ctx.doc(), eq));
}

TEST_CASE("all-free (Device view): a title bar dragged elsewhere rewires — dissolve at the old place, splice at the new") {
  DeviceUi u;
  const size_t h0 = u.history();
  // The Saturator's title bar onto the Utility's left half: it goes between the synth and the Utility.
  const IRect fu = u.frame(u.util);
  u.d.drag(u.title(u.sat), {static_cast<float>(fu.x + 10), static_cast<float>(fu.y + 18)}, 8);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(u.onPath() == Ids{u.synth, u.sat, u.util});
  CHECK(u.listed() == Ids{u.synth, u.sat, u.util});
  CHECK(u.path().complete);
  CHECK(u.linked(u.synth, u.sat));
  CHECK(u.linked(u.sat, u.util));
  CHECK(u.intoMixer(u.util));
  CHECK_FALSE(u.linked(u.util, u.sat));
  CHECK_FALSE(u.intoMixer(u.sat));
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) != nullptr);  // the LFO's modulation is left alone
  // Dragged to the far left (before the instrument): it is first on the path; the instrument after it.
  const IRect fi = u.frame(u.synth);
  u.d.drag(u.title(u.util), {static_cast<float>(fi.x + 2), static_cast<float>(fi.y + 18)}, 8);
  u.d.step();
  CHECK(u.history() == h0 + 2);
  CHECK(u.onPath() == Ids{u.util, u.synth, u.sat});
  CHECK(u.path().complete);
  // One undo step each, back to the start.
  REQUIRE(u.ctx.undo());
  CHECK(u.onPath() == Ids{u.synth, u.sat, u.util});
  REQUIRE(u.ctx.undo());
  CHECK(u.onPath() == Ids{u.synth, u.util, u.sat});
  CHECK(u.listed() == Ids{u.synth, u.util, u.sat});
}

TEST_CASE("all-free (Device view): Delete dissolves — the block and its wires go, its neighbours are joined") {
  DeviceUi u;
  u.d.click(u.title(u.util));
  REQUIRE(u.chain().selection().size() == 1);
  REQUIRE(u.chain().selection()[0] == u.util);
  const size_t h0 = u.history();
  CHECK(u.d.press("Delete") == KeyResult::Action);
  CHECK(u.history() == h0 + 1);
  CHECK(doc::findDevice(*u.ctx.doc(), u.util).device == nullptr);
  CHECK(u.onPath() == Ids{u.synth, u.sat});
  CHECK(u.listed() == Ids{u.synth, u.sat});
  CHECK(u.linked(u.synth, u.sat));
  CHECK(u.path().complete);
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) == nullptr);  // its modulation went with it
  CHECK(u.inRack(u.lfo));                                     // the LFO stays, off the path
  REQUIRE(u.ctx.undo());
  CHECK(u.onPath() == Ids{u.synth, u.util, u.sat});
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) != nullptr);
}

TEST_CASE("all-free (Device view): a block dragged from the free rack onto the list is spliced in; back onto the rack, "
          "taken off") {
  DeviceUi u;
  // The LFO block's top strip in the rack onto the Saturator's left half: between the Utility and the Saturator.
  const ProjectPatch& pp = u.rack().patch();
  const uint32_t n = pp.nodeOf(u.lfo);
  REQUIRE(n != kNone);
  const Node& x = pp.nodes[n];
  const Vec2 from = u.rack().canvas().toDevice({x.x + x.w * 0.5f, x.y + 6.0f});
  const IRect fs = u.frame(u.sat);
  const size_t h0 = u.history();
  u.d.drag(from, {static_cast<float>(fs.x + 10), static_cast<float>(fs.y + fs.h / 2)}, 10);
  u.d.tree().layoutNow();
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(u.onPath() == Ids{u.synth, u.util, u.lfo, u.sat});
  CHECK(u.listed() == Ids{u.synth, u.util, u.lfo, u.sat});
  CHECK(u.linked(u.util, u.lfo));
  CHECK(u.linked(u.lfo, u.sat));
  CHECK_FALSE(u.inRack(u.lfo));
  // Its title bar onto the rack: off the path again, its neighbours joined.
  u.d.drag(u.title(u.lfo), mid(u.chain().freeRackRect()), 8);
  u.d.step();
  CHECK(u.history() == h0 + 2);
  CHECK(u.onPath() == Ids{u.synth, u.util, u.sat});
  CHECK(u.linked(u.util, u.sat));
  CHECK(u.inRack(u.lfo));
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) != nullptr);  // a block taken off keeps its other wires
}

TEST_CASE("all-free (Device view): a broken path lists what is still on it; the rest is in the free rack") {
  DeviceUi u;
  auto drop = [&](doc::WireId w) {
    REQUIRE(u.ctx.store().dispatch(doc::ops::deleteWires(*u.ctx.doc(), std::vector<doc::WireId>{w})->command).ok());
  };
  // The Saturator's cable into the mixer gone: the path still runs to the Saturator, but reaches no mixer.
  drop(u.path().links.back());
  CHECK_FALSE(u.path().complete);
  CHECK(u.listed() == Ids{u.synth, u.util, u.sat});
  // The Utility → Saturator cable gone too: the list ends at the Utility; the Saturator is off the path, in the rack.
  drop(u.path().links.back());
  CHECK(u.listed() == Ids{u.synth, u.util});
  CHECK(u.chain().frameOf(u.sat) == nullptr);
  CHECK(u.inRack(u.sat));
  // Undo gives the cables back: the whole path again.
  REQUIRE(u.ctx.undo());
  REQUIRE(u.ctx.undo());
  CHECK(u.listed() == Ids{u.synth, u.util, u.sat});
  CHECK(u.path().complete);
}

namespace {

// The shell in the project patch view, Keys focused.
struct PatchUi : Doc {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;

  PatchUi() {
    ctx.setDeviceLibrary(&library());
    build(ctx.store());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPanelsShown(false);
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    view().focusBand(keys, false);
    d.tree().layoutNow();
    {
      const Band& b = pp().bands[pp().bandOf(keys)];
      const IRect r = view().canvas().rect();
      const double span = static_cast<double>(b.x1 + 420.0f - (b.x0 - 40.0f));
      circuit::CircuitCanvas::View v;
      v.zoom = std::min(1.0, static_cast<double>(r.w) / span);
      v.x = b.x0 - 40.0f;
      v.y = b.y - 40.0f;
      view().canvas().setView(v);
    }
    d.tree().layoutNow();
    d.step();
    d.tree().setFocus(&view().canvas());
  }
  ~PatchUi() { d.tree().setRoot(std::make_unique<Widget>()); }
  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const ProjectPatch& pp() const { return view().patch(); }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Track& track() const { return *doc::findTrackPtr(*ctx.doc(), keys); }
  std::vector<doc::DeviceId> onPath() const {
    std::vector<doc::DeviceId> v;
    for (const doc::Device* x : doc::mainPath(*ctx.doc(), track()).blocks) v.push_back(x->id);
    return v;
  }
  uint32_t node(doc::DeviceId id) const {
    const uint32_t n = pp().nodeOf(id);
    REQUIRE(n != kNone);
    return n;
  }
  Vec2 dev(graph::Point2 w) const { return view().canvas().toDevice(w); }
  Vec2 top(doc::DeviceId id) const {
    const Node& n = pp().nodes[node(id)];
    return dev({n.x + n.w * 0.5f, n.y + 6.0f});
  }
  Vec2 outPin(doc::DeviceId id, std::string_view name) const {
    const Node& n = pp().nodes[node(id)];
    const int32_t k = n.output(name);
    REQUIRE(k >= 0);
    return dev({n.x + n.w, n.y + pinOffset(n, static_cast<size_t>(k))});
  }
  Vec2 inPin(doc::DeviceId id, std::string_view name) const {
    const Node& n = pp().nodes[node(id)];
    const int32_t k = n.input(name);
    REQUIRE(k >= 0);
    return dev(inputEntry(n, static_cast<size_t>(k)));
  }
  Vec2 mixer() const {
    const Node& n = pp().nodes[pp().bands[pp().bandOf(keys)].mixer];
    return dev({n.x + n.w * 0.5f, n.y + n.h * 0.5f});
  }
};

}  // namespace

TEST_CASE("all-free (project patch): a block is placed anywhere — the move is view state, the cables keep the path") {
  PatchUi u;
  const Ids path0{u.synth, u.util, u.sat};
  REQUIRE(u.onPath() == path0);
  const size_t h0 = u.history();
  // A block of the path dragged well below and left of its place: it stays on the path (no edit; a nudge).
  const Vec2 a = u.top(u.util);
  u.d.drag(a, {a.x - 60.0f, a.y + 90.0f}, 8);
  u.d.step();
  CHECK(u.history() == h0);
  CHECK(u.onPath() == path0);
  CHECK(u.ctx.store().view().patchNudges.count(u.util.value) == 1);
  // The block off the path dragged between two blocks of the path: still off it (only a cable puts it on).
  const Vec2 b = u.top(u.lfo);
  const Vec2 between = u.top(u.sat);
  u.d.drag(b, {between.x - 40.0f, between.y}, 8);
  u.d.step();
  CHECK(u.history() == h0);
  CHECK(u.onPath() == path0);
  CHECK(all_free::offPath(*u.ctx.doc(), u.lfo));
}

TEST_CASE("all-free (project patch): cables drawn between main ports and onto the mixer make the path") {
  PatchUi u;
  // A second Utility off the path (unconnected).
  const doc::DeviceId u2 = [&] {
    auto e = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.keys, "utility", doc::DeviceKind::AudioEffect, doc::ops::kOffPath);
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    return e->devices.at(0);
  }();
  u.d.tree().layoutNow();
  u.d.step();
  const size_t h0 = u.history();
  // The Saturator's main output dragged onto the new block's main input: a main link (both ends "").
  u.d.drag(u.outPin(u.sat, "out"), u.inPin(u2, "x"), 8);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  bool satToEq = false;
  for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc())) satToEq = satToEq || (w.from == u.sat && w.to == u2 && w.mainLink());
  CHECK(satToEq);
  CHECK(u.onPath() == Ids{u.synth, u.util, u.sat});  // a branch: the path keeps its link into the mixer
  // The new block's output onto the band's mixer.
  u.d.tree().layoutNow();
  u.d.step();
  u.d.drag(u.outPin(u2, "out"), u.mixer(), 8);
  u.d.step();
  CHECK(u.history() == h0 + 2);
  bool eqToMixer = false;
  for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc())) eqToMixer = eqToMixer || (w.from == u2 && w.toMixer() && w.mixer == u.keys && w.mainLink());
  CHECK(eqToMixer);
  // The Saturator's own link into the mixer deleted: the path runs through the new block now.
  doc::WireId satOut;
  for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc()))
    if (w.from == u.sat && w.toMixer()) satOut = w.id;
  REQUIRE(satOut.valid());
  REQUIRE(u.ctx.store().dispatch(doc::ops::deleteWires(*u.ctx.doc(), std::vector<doc::WireId>{satOut})->command).ok());
  CHECK(u.onPath() == Ids{u.synth, u.util, u.sat, u2});
  CHECK(doc::mainPath(*u.ctx.doc(), u.track()).complete);
  u.d.tree().layoutNow();
  u.d.step();
  CHECK_FALSE(u.pp().nodes[u.node(u2)].offPath);
}

TEST_CASE("all-free (project patch): a cable from a block on the path onto its mixer is a branch the mixer sums") {
  PatchUi u;
  const size_t h0 = u.history();
  // The Utility (on the path: synth → Utility → Saturator → mixer) dragged from its output onto the band's mixer.
  u.d.drag(u.outPin(u.util, "out"), u.mixer(), 8);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  doc::WireId branch;
  for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc()))
    if (w.from == u.util && w.toMixer() && w.mixer == u.keys) branch = w.id;
  REQUIRE(branch.valid());
  CHECK(doc::findWire(*u.ctx.doc(), branch)->mainLink());
  // The path keeps its oldest link that reaches the mixer (through the Saturator); the new cable is summed beside it.
  CHECK(u.onPath() == Ids{u.synth, u.util, u.sat});
  CHECK(doc::mainPath(*u.ctx.doc(), u.track()).complete);
  CHECK_FALSE(doc::mainPath(*u.ctx.doc(), u.track()).isLink(branch));
  u.view().sync();
  REQUIRE(u.view().lowered() != nullptr);
  bool lowered = false;
  for (const doc::WireState& st : u.view().lowered()->wires) lowered = lowered || (st.id == branch && st.status == doc::WireStatus::Lowered);
  CHECK(lowered);
  REQUIRE(u.ctx.undo());
  CHECK(doc::findWire(*u.ctx.doc(), branch) == nullptr);
}

namespace {
ui::Menu* blockMenu(PatchUi& u, doc::DeviceId d) {
  u.d.tree().layoutNow();
  u.d.step();
  u.d.click(u.top(d), mod::kNone, PointerButton::Right);
  for (size_t i = u.shell->overlay().childCount(); i-- > 0;)
    if (auto* m = dynamic_cast<ui::Menu*>(&u.shell->overlay().child(i))) return m;
  return nullptr;
}
bool choose(PatchUi& u, ui::Menu& m, std::string_view label) {
  for (int32_t i = 0; i < static_cast<int32_t>(m.items().size()); ++i)
    if (m.items()[static_cast<size_t>(i)].label == label) {
      m.choose(i);
      u.d.step();
      return true;
    }
  return false;
}
}  // namespace

TEST_CASE("all-free (project patch): the block menu's Take Off Path and Dissolve — one step each, the path joined") {
  PatchUi u;
  const size_t h0 = u.history();
  // Take Off Path on the Utility: it stays (with the LFO's wire into its gain), the synth joins the Saturator.
  ui::Menu* m = blockMenu(u, u.util);
  REQUIRE(m != nullptr);
  REQUIRE(choose(u, *m, "Take Off Path"));
  CHECK(u.history() == h0 + 1);
  CHECK(u.onPath() == Ids{u.synth, u.sat});
  CHECK(doc::mainPath(*u.ctx.doc(), u.track()).complete);
  CHECK(all_free::offPath(*u.ctx.doc(), u.util));
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) != nullptr);
  // An off-path block's menu has no Take Off Path.
  m = blockMenu(u, u.util);
  REQUIRE(m != nullptr);
  bool offered = false;
  for (const ui::MenuItem& it : m->items()) offered = offered || it.label == "Take Off Path";
  CHECK_FALSE(offered);
  u.d.press("Escape");
  REQUIRE(u.ctx.undo());
  CHECK(u.onPath() == Ids{u.synth, u.util, u.sat});
  // Dissolve on the Saturator: it goes, the Utility runs into the mixer.
  m = blockMenu(u, u.sat);
  REQUIRE(m != nullptr);
  REQUIRE(choose(u, *m, "Dissolve"));
  CHECK(u.history() == h0 + 1);
  CHECK(doc::findDevice(*u.ctx.doc(), u.sat).device == nullptr);
  CHECK(u.onPath() == Ids{u.synth, u.util});
  CHECK(doc::mainPath(*u.ctx.doc(), u.track()).complete);
  REQUIRE(u.ctx.undo());
  CHECK(u.onPath() == Ids{u.synth, u.util, u.sat});
}

// Golden device-path-broken (view_scenes.cpp): the free-rack project with the cable into Lead's last block deleted — the
// list shows the path up to the break, the free rack the blocks off it (the Utility among them).
TEST_CASE("render: device-path-broken matches the golden image") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize("device-path-broken", w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene("device-path-broken", {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/device-path-broken.png";
  auto ref = readPng(path);
  if (!ref) (void)writePng("device-path-broken.actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden("device-path-broken", *img, *ref, true, &d));
  MESSAGE("device-path-broken: " << pq::ui::test::describe(d));
}

// ---- TX.all-free-fix1: stock instruments and racks are ordinary blocks (QUESTIONS Q208) -------------------------------
// Through real event dispatch in the project patch view: the track's notes dragged onto an off-path instrument's notes
// input and its output onto the mixer make it play (lowered: a call of it in the track's patch, nothing missing, both
// cables Lowered); a block's output dragged onto ANOTHER track's mixer is a send-like route (one undo step, lowered).
namespace {
Vec2 sourceOut(const PatchUi& u, size_t port) {
  const Node& n = u.pp().nodes[u.pp().bands[u.pp().bandOf(u.keys)].source];
  return u.dev({n.x + n.w, n.y + pinOffset(n, port)});
}
const doc::WireState* loweredState(PatchUi& u, doc::WireId w) {
  u.view().sync();
  REQUIRE(u.view().lowered() != nullptr);
  for (const doc::WireState& st : u.view().lowered()->wires)
    if (st.id == w) return &st;
  return nullptr;
}
bool callsDevice(PatchUi& u, doc::DeviceId d) {
  u.view().sync();
  const doc::LoweredProject* low = u.view().lowered();
  REQUIRE(low != nullptr);
  const std::string name = "d" + std::to_string(d.value);
  for (const auto& p : low->patches)
    for (graph::NodeIndex n = 0; n < p->patch.nodeCount(); ++n)
      if (p->patch.nodeName(n) == name) return true;
  return false;
}
}  // namespace

TEST_CASE("all-free-fix1 (project patch): notes dragged onto an off-path instrument and its output onto the mixer play it") {
  PatchUi u;
  const doc::DeviceId synth2 = [&] {
    auto e = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.keys, "BasicSynth", doc::DeviceKind::Instrument,
                                 doc::ops::kOffPath);
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    return e->devices.at(0);
  }();
  u.d.tree().layoutNow();
  u.d.step();
  CHECK(all_free::offPath(*u.ctx.doc(), synth2));
  CHECK_FALSE(callsDevice(u, synth2));  // nothing feeds it notes: it plays nothing (and is not missing)
  {
    u.view().sync();
    const auto& missing = u.view().lowered()->missingDevices;
    CHECK(std::find(missing.begin(), missing.end(), synth2) == missing.end());
  }
  const size_t h0 = u.history();
  // The band's notes (the source's output 0) onto the new synth's notes input.
  u.d.drag(sourceOut(u, 0), u.inPin(synth2, "notes"), 8);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  doc::WireId notes;
  for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc()))
    if (w.fromNoteSource() && w.noteSource == u.keys && w.port.empty() && w.to == synth2) notes = w.id;
  REQUIRE(notes.valid());
  CHECK(all_free::offPath(*u.ctx.doc(), synth2));  // a branch from the source: the path keeps its own first link
  // Its output onto the band's mixer.
  u.d.tree().layoutNow();
  u.d.step();
  u.d.drag(u.outPin(synth2, "out"), u.mixer(), 8);
  u.d.step();
  CHECK(u.history() == h0 + 2);
  doc::WireId out;
  for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc()))
    if (w.from == synth2 && w.toMixer() && w.mixer == u.keys) out = w.id;
  REQUIRE(out.valid());
  const doc::WireState* ns = loweredState(u, notes);
  REQUIRE(ns != nullptr);
  CHECK(ns->status == doc::WireStatus::Lowered);
  const doc::WireState* os = loweredState(u, out);
  REQUIRE(os != nullptr);
  CHECK(os->status == doc::WireStatus::Lowered);
  CHECK(callsDevice(u, synth2));
  CHECK(u.view().lowered()->missingDevices.empty());
  // One undo step each: back to the unconnected synth.
  REQUIRE(u.ctx.undo());
  REQUIRE(u.ctx.undo());
  CHECK_FALSE(callsDevice(u, synth2));
}

TEST_CASE("all-free-fix1 (project patch): a block's output dragged onto another track's mixer is a send-like route") {
  PatchUi u;
  const doc::TrackId other = [&] {
    auto e = doc::ops::createTrack(*u.ctx.doc(), u.ctx.store().ids(), doc::TrackKind::Audio, u.keys, "Bus");
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    return e->tracks.at(0);
  }();
  u.d.tree().layoutNow();
  u.d.step();
  {  // both bands on screen
    const Band& a = u.pp().bands[u.pp().bandOf(u.keys)];
    const Band& b = u.pp().bands[u.pp().bandOf(other)];
    const IRect r = u.view().canvas().rect();
    const float x0 = std::min(a.x0, b.x0) - 40.0f, x1 = std::max(a.x1, b.x1) + 420.0f;
    const float y0 = std::min(a.y, b.y) - 40.0f, y1 = std::max(a.y + a.h, b.y + b.h) + 40.0f;
    circuit::CircuitCanvas::View v;
    v.zoom = std::min({1.0, static_cast<double>(r.w) / static_cast<double>(x1 - x0), static_cast<double>(r.h) / static_cast<double>(y1 - y0)});
    v.x = x0;
    v.y = y0;
    u.view().canvas().setView(v);
    u.d.tree().layoutNow();
    u.d.step();
  }
  const Node& m = u.pp().nodes[u.pp().bands[u.pp().bandOf(other)].mixer];
  const Vec2 otherMixer = u.dev({m.x + m.w * 0.5f, m.y + m.h * 0.5f});
  const size_t h0 = u.history();
  u.d.drag(u.outPin(u.sat, "out"), otherMixer, 8);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  doc::WireId route;
  for (const doc::Wire& w : doc::wiresOf(*u.ctx.doc()))
    if (w.from == u.sat && w.toMixer() && w.mixer == other) route = w.id;
  REQUIRE(route.valid());
  CHECK(doc::mainPath(*u.ctx.doc(), u.track()).complete);  // Keys' own path is untouched
  const doc::WireState* st = loweredState(u, route);
  REQUIRE(st != nullptr);
  CHECK(st->status == doc::WireStatus::Lowered);
  REQUIRE(u.ctx.undo());
  CHECK(doc::findWire(*u.ctx.doc(), route) == nullptr);
}
