// bug-wire-cut: X over a cable cuts it — its two halves hang on the pointer (the source half wants an input, the target
// half an output), either order, a click on empty canvas lets go of what still hangs, Esc puts the cable back, a
// refused click keeps the half hanging; one undo step for the cut and its reconnections. Z over an input port with
// cables: one VCA takes them all (VCA → the port), one undo step; Z over a cable as before; Z over an empty port: nothing.
// Everything through the real window → canvas dispatch (keys and clicks), the scene rebuilt during the gesture.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "all_free_helpers.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/modules.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::circuit;
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

// "Keys": the default instrument group → a gain block (`util`: x · g) → a tanh block (`sat`); a free LFO module whose
// output `y` drives the gain's `g` (the cable cut here); two free tanh blocks (`a`, `b`) to re-plug onto / from.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  doc::TrackId keys;
  doc::DeviceId synth, util, sat, lfo, a, b;
  doc::WireId lfoWire;

  Ui() {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    auto run = [&](const doc::ops::EditResult& e) {
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
      REQUIRE(st.dispatch(e->command).ok());
      return *e;
    };
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks.at(0);
    synth = doc::mainPath(*st.current(), *doc::findTrackPtr(*st.current(), keys)).blocks.at(0)->id;
    util = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "atom:gain", doc::DeviceKind::AudioEffect)).devices.at(0);
    sat = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "atom:tanh", doc::DeviceKind::AudioEffect)).devices.at(0);
    lfo = test_blocks::addModule(st, keys, "lfo", doc::ops::kOffPath);
    a = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "atom:tanh", doc::DeviceKind::AudioEffect, doc::ops::kOffPath)).devices.at(0);
    b = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "atom:tanh", doc::DeviceKind::AudioEffect, doc::ops::kOffPath)).devices.at(0);
    lfoWire = run(doc::ops::connectWire(*st.current(), st.ids(), lfo, "y", util, "g")).wires.at(0);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    view().focusBand(keys, false);
    d.tree().layoutNow();
    fit();
    d.tree().setFocus(&view().canvas());
  }
  void fit() {
    view().sync();
    const Band& bd = pp().bands[pp().bandOf(keys)];
    const ui::IRect r = view().canvas().rect();
    const double span = static_cast<double>(bd.x1 + 420.0f - (bd.x0 - 40.0f));
    CircuitCanvas::View v;
    v.zoom = std::min({1.0, static_cast<double>(r.w) / span, static_cast<double>(r.h) / static_cast<double>(bd.h + 80.0f)});
    v.x = bd.x0 - 40.0f;
    v.y = bd.y - 40.0f;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  ProjectCanvas& canvas() const { return view().canvas(); }
  const ProjectPatch& pp() const { return view().patch(); }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Device* device(doc::DeviceId id) const { return doc::findDevice(*ctx.doc(), id).device; }
  uint32_t node(doc::DeviceId id) const {
    const uint32_t n = pp().nodeOf(id);
    REQUIRE(n != kNone);
    return n;
  }
  Vec2 dev(graph::Point2 w) const { return canvas().toDevice(w); }
  graph::Point2 world(Vec2 p) const { return canvas().toWorld(p); }
  Vec2 outPin(doc::DeviceId id, std::string_view name) const {
    view().sync();
    const int32_t k = pp().nodes[node(id)].output(name);
    REQUIRE(k >= 0);
    return dev(canvas().portWorld(node(id), static_cast<uint32_t>(k), false));
  }
  Vec2 inPin(doc::DeviceId id, std::string_view name) const {
    view().sync();
    const int32_t k = pp().nodes[node(id)].input(name);
    REQUIRE_MESSAGE(k >= 0, std::string(name));
    return dev(canvas().portWorld(node(id), static_cast<uint32_t>(k), true));
  }
  Vec2 empty() const {
    const Band& bd = pp().bands[pp().bandOf(keys)];
    return dev({bd.x1 + 160.0f, bd.y + 60.0f});
  }
  bool clear(Vec2 m) const {
    const uint32_t n = canvas().nodeAt(m);
    return (n == kNone || (pp().nodes[n].group && pp().nodes[n].groupOpen)) && !canvas().pinAt(m).valid();
  }
  uint32_t cableOf(doc::WireId w) const {
    view().sync();
    for (uint32_t i = 0; i < pp().edges.size(); ++i)
      if (pp().edges[i].wire == w) return i;
    FAIL("no cable for the wire");
    return kNone;
  }
  // A device point on the cable of model edge `edge` (the canvas's own wire hit test), clear of blocks and pins.
  Vec2 onCable(uint32_t edge) const {
    const Edge& e = pp().edges[edge];
    REQUIRE(e.points.size() >= 2);
    float x0 = e.points.front().x, x1 = x0, y0 = e.points.front().y, y1 = y0;
    for (const graph::Point2& p : e.points) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    for (float y = y0 - 40.0f; y <= y1 + 40.0f; y += 2.0f)
      for (float x = x0; x <= x1; x += 2.0f) {
        const Vec2 m = dev({x, y});
        if (canvas().edgeAt(m) == edge && clear(m)) return m;
      }
    FAIL("no point on the cable");
    return {};
  }
  std::vector<doc::Wire> wires() const { return doc::wiresOf(*ctx.doc()); }
  // TX.all-free: a wire names a main port "" (doc::kMainPort); compared by the port it stands for.
  bool wired(doc::DeviceId from, std::string_view out, doc::DeviceId to, std::string_view param) const {
    for (const doc::Wire& w : wires()) {
      if (w.from != from || w.to != to) continue;
      const std::string outName = w.port.empty() ? std::string(view().mainPorts(from).out) : w.port;
      const std::string into = w.param.empty() ? std::string(view().mainPorts(to).in) : w.param;
      if (outName == out && into == param) return true;
    }
    return false;
  }
  size_t into(doc::DeviceId to, std::string_view param) const {
    size_t n = 0;
    for (const doc::Wire& w : wires()) {
      const std::string p = w.param.empty() && w.to.valid() ? std::string(view().mainPorts(w.to).in) : w.param;
      if (w.to == to && p == param) ++n;
    }
    return n;
  }
  void key(Key k) {
    d.keyDown(k);
    d.keyUp(k);
    d.step();
  }
  // X over the LFO's cable: cut, both halves hanging from their ports, the wire gone (the gesture open: no history yet).
  void cut() {
    const size_t h = history();
    const Vec2 at = onCable(cableOf(lfoWire));
    d.move(at);
    key('x');
    REQUIRE(canvas().cutting());
    CHECK(canvas().cutSourceHanging());
    CHECK(canvas().cutTargetHanging());
    CHECK(doc::findWire(*ctx.doc(), lfoWire) == nullptr);
    CHECK(history() == h);
    CHECK(canvas().looseCable(ProjectCanvas::kCutSourceSlot));
    CHECK(canvas().looseCable(ProjectCanvas::kCutTargetSlot));
  }
  // The pointer to `p` (a move, then a click there), a frame after.
  void clickAt(Vec2 p) {
    d.move(p);
    d.click(p);
    d.step();
    view().sync();
  }
  // Whether block `id` overlaps no other block (devices, mixers; an opened capsule's frame is no block).
  bool overlapsNothing(doc::DeviceId id) const {
    const uint32_t k = node(id);
    const Node& v = pp().nodes[k];
    for (uint32_t i = 0; i < pp().nodes.size(); ++i) {
      const Node& n = pp().nodes[i];
      if (i == k || (n.kind != NodeKind::Device && n.kind != NodeKind::Mixer) || (n.group && n.groupOpen) || n.w <= 0.0f) continue;
      if (v.x < n.x + n.w && n.x < v.x + v.w && v.y < n.y + n.h && n.y < v.y + v.h) {
        MESSAGE("overlaps " << n.title);
        return false;
      }
    }
    return true;
  }
  std::vector<doc::DeviceId> path() const {
    std::vector<doc::DeviceId> v;
    for (const doc::Device* x : doc::mainPath(*ctx.doc(), *doc::findTrackPtr(*ctx.doc(), keys)).blocks) v.push_back(x->id);
    return v;
  }
  doc::DeviceId newestVca() const {
    doc::DeviceId best;
    for (const doc::DevicePtr& dv : doc::findTrackPtr(*ctx.doc(), keys)->devices)
      if (doc::isGroupDevice(*dv) && dv->name == "VCA" && dv->id.value > best.value) best = dv->id;
    return best;
  }
};

float distance(graph::Point2 p, graph::Point2 q) { return std::hypot(p.x - q.x, p.y - q.y); }

}  // namespace

TEST_CASE("wire cut: X, then the source half onto an input, then the target half from an output — one undo step") {
  Ui u;
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  u.cut();
  // The halves hang from where the cable was plugged in: the LFO's jack and the gain's g.
  const graph::Point2 jack = u.world(u.outPin(u.lfo, "y")), g = u.world(u.inPin(u.util, "g"));
  REQUIRE_FALSE(u.canvas().looseCablePoints(ProjectCanvas::kCutSourceSlot).empty());
  CHECK(distance(u.canvas().looseCablePoints(ProjectCanvas::kCutSourceSlot).front(), jack) < 1.0f);
  CHECK(distance(u.canvas().looseCablePoints(ProjectCanvas::kCutTargetSlot).front(), g) < 1.0f);
  // Moving draws both to the pointer; over the Saturator's input the source half snaps there (the target half does not).
  u.d.move(u.empty());
  u.d.step();
  const Vec2 satIn = u.inPin(u.sat, "x");
  u.d.move(satIn);
  u.d.step();
  CHECK(u.canvas().looseCableSnapped(ProjectCanvas::kCutSourceSlot));
  CHECK_FALSE(u.canvas().looseCableSnapped(ProjectCanvas::kCutTargetSlot));
  // Click: LFO → Saturator's x (the scene is rebuilt: the target half still hangs from g).
  u.clickAt(satIn);
  REQUIRE(u.canvas().cutting());
  CHECK_FALSE(u.canvas().cutSourceHanging());
  CHECK(u.canvas().cutTargetHanging());
  CHECK(u.wired(u.lfo, "y", u.sat, "x"));
  CHECK(u.history() == h0);  // (still one gesture)
  CHECK(distance(u.canvas().looseCablePoints(ProjectCanvas::kCutTargetSlot).front(), u.world(u.inPin(u.util, "g"))) < 1.0f);
  // Over an output the target half snaps; click: a's output → g. Both connected: the cut ends.
  const Vec2 aOut = u.outPin(u.a, "out");
  u.d.move(aOut);
  u.d.step();
  CHECK(u.canvas().looseCableSnapped(ProjectCanvas::kCutTargetSlot));
  u.clickAt(aOut);
  CHECK_FALSE(u.canvas().cutting());
  CHECK(u.wired(u.a, "out", u.util, "g"));
  CHECK(u.wired(u.lfo, "y", u.sat, "x"));
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) == nullptr);
  CHECK(u.history() == h0 + 1);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // The next frames: no loose cable left (the made cables took their motion over, or they went).
  for (int i = 0; i < 3; ++i) u.d.step();
  CHECK_FALSE(u.canvas().looseCable(ProjectCanvas::kCutSourceSlot));
  CHECK_FALSE(u.canvas().looseCable(ProjectCanvas::kCutTargetSlot));
  // One undo step puts everything back.
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
  CHECK(u.history() == h0);
}

TEST_CASE("wire cut: the target half first (from an output), then the source half (onto an input)") {
  Ui u;
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  u.cut();
  u.clickAt(u.outPin(u.b, "out"));
  REQUIRE(u.canvas().cutting());
  CHECK(u.canvas().cutSourceHanging());
  CHECK_FALSE(u.canvas().cutTargetHanging());
  CHECK(u.wired(u.b, "out", u.util, "g"));
  // An output again: the source half wants an input (nothing happens, it keeps hanging).
  u.clickAt(u.outPin(u.a, "out"));
  CHECK(u.canvas().cutSourceHanging());
  CHECK(u.ctx.status().find("Cut") != std::string::npos);
  // The source half onto a's input.
  u.clickAt(u.inPin(u.a, "x"));
  CHECK_FALSE(u.canvas().cutting());
  CHECK(u.wired(u.lfo, "y", u.a, "x"));
  CHECK(u.wired(u.b, "out", u.util, "g"));
  CHECK(u.history() == h0 + 1);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
}

TEST_CASE("wire cut: a click on empty canvas lets both halves go — the cable stays deleted, one undo step") {
  Ui u;
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  const size_t wires0 = u.wires().size();
  u.cut();
  u.clickAt(u.empty());
  CHECK_FALSE(u.canvas().cutting());
  CHECK_FALSE(u.canvas().looseCable(ProjectCanvas::kCutSourceSlot));
  CHECK_FALSE(u.canvas().looseCable(ProjectCanvas::kCutTargetSlot));
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) == nullptr);
  CHECK(u.wires().size() == wires0 - 1);
  CHECK(u.into(u.util, "g") == 0);
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
  // One half connected, then empty canvas: that one stays, the other goes.
  u.cut();
  u.clickAt(u.inPin(u.b, "x"));
  u.clickAt(u.empty());
  CHECK_FALSE(u.canvas().cutting());
  CHECK(u.wired(u.lfo, "y", u.b, "x"));
  CHECK(u.into(u.util, "g") == 0);
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
}

TEST_CASE("wire cut: Esc puts the cable back exactly (after a half was connected too); nothing in the history") {
  Ui u;
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  u.cut();
  REQUIRE(u.d.press("Escape") == KeyResult::Widget);
  u.d.step();
  CHECK_FALSE(u.canvas().cutting());
  CHECK(u.ctx.doc() == start);
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) != nullptr);
  CHECK(u.history() == h0);
  CHECK_FALSE(u.canvas().looseCable(ProjectCanvas::kCutSourceSlot));
  CHECK_FALSE(u.canvas().looseCable(ProjectCanvas::kCutTargetSlot));
  // With one half re-plugged first.
  u.cut();
  u.clickAt(u.outPin(u.a, "out"));
  CHECK(u.wired(u.a, "out", u.util, "g"));
  REQUIRE(u.d.press("Escape") == KeyResult::Widget);
  u.d.step();
  CHECK_FALSE(u.canvas().cutting());
  CHECK(u.ctx.doc() == start);
  CHECK(u.history() == h0);
}

TEST_CASE("wire cut: a refused click keeps the half hanging and the status line says why") {
  Ui u;
  const doc::Snapshot start = u.ctx.doc();
  u.cut();
  // The Keys mixer's output is a route (no cable comes out of it): the target half is not driven from it.
  u.view().sync();
  const uint32_t mixer = u.pp().bands[u.pp().bandOf(u.keys)].mixer;
  REQUIRE(mixer != kNone);
  REQUIRE_FALSE(u.pp().nodes[mixer].outputs.empty());
  const Vec2 route = u.dev(u.canvas().portWorld(mixer, 0, false));
  const ProjectCanvas::PinHit hit = u.canvas().pinAt(route);
  REQUIRE(hit.valid());
  REQUIRE_FALSE(hit.input);
  u.d.move(route);
  u.d.step();
  CHECK_FALSE(u.canvas().looseCableSnapped(ProjectCanvas::kCutTargetSlot));
  u.clickAt(route);
  REQUIRE(u.canvas().cutting());
  CHECK(u.canvas().cutTargetHanging());
  CHECK(u.canvas().cutSourceHanging());
  CHECK(u.ctx.status().find("route") != std::string::npos);
  // The halves still work after the refusal.
  u.clickAt(u.outPin(u.a, "out"));
  CHECK_FALSE(u.canvas().cutTargetHanging());
  CHECK(u.wired(u.a, "out", u.util, "g"));
  REQUIRE(u.d.press("Escape") == KeyResult::Widget);
  CHECK(u.ctx.doc() == start);
}

TEST_CASE("wire cut: Z over an input with three cables — one VCA takes them all, the VCA into the port; one undo step") {
  Ui u;
  doc::DocumentStore& st = u.ctx.store();
  for (const doc::DeviceId from : {u.a, u.b}) {
    auto e = doc::ops::connectWire(*st.current(), st.ids(), from, "out", u.util, "g");
    REQUIRE(e.ok());
    REQUIRE(st.dispatch(e->command).ok());
  }
  u.view().sync();
  REQUIRE(u.into(u.util, "g") == 3);
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  const Vec2 g = u.inPin(u.util, "g");
  REQUIRE(u.view().cablesInto(u.canvas().pinAt(g).node, u.canvas().pinAt(g).port).size() == 3);
  u.d.move(g);
  u.key('z');
  u.view().sync();
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId vca = u.newestVca();
  REQUIRE(u.device(vca) != nullptr);
  CHECK(all_free::offPath(*u.ctx.doc(), vca));
  const MainPorts mp = u.view().mainPorts(vca);
  CHECK(u.into(u.util, "g") == 1);
  CHECK(u.wired(vca, mp.out, u.util, "g"));
  CHECK(u.wired(u.lfo, "y", vca, mp.in));
  CHECK(u.wired(u.a, "out", vca, mp.in));
  CHECK(u.wired(u.b, "out", vca, mp.in));
  CHECK(u.into(vca, mp.in) == 3);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Near the port, over no block (the gain sits right after the instrument: no room on its left, the overlap rule
  // moves the VCA aside).
  CHECK(u.overlapsNothing(vca));
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
}

TEST_CASE("wire cut: Z over a mixer's input with three cables (the path's end among them) — the VCA left of it, on the path") {
  Ui u;
  doc::DocumentStore& st = u.ctx.store();
  for (const doc::DeviceId from : {u.a, u.b}) {
    doc::Wire w;
    w.from = from;
    w.port = "out";
    w.mixer = u.keys;
    auto e = doc::ops::connect(*st.current(), st.ids(), doc::canonicalWire(*st.current(), w, &library()));
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
    REQUIRE(st.dispatch(e->command).ok());
  }
  u.view().sync();
  const uint32_t mixer = u.pp().bands[u.pp().bandOf(u.keys)].mixer;
  REQUIRE(mixer != kNone);
  REQUIRE(u.view().cablesInto(mixer, 0).size() == 3);
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  const Vec2 in = u.dev(u.canvas().portWorld(mixer, 0, true));
  REQUIRE(u.canvas().pinAt(in).valid());
  REQUIRE(u.canvas().pinAt(in).input);
  u.d.move(in);
  u.key('z');
  u.view().sync();
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId vca = u.newestVca();
  REQUIRE(u.device(vca) != nullptr);
  const MainPorts mp = u.view().mainPorts(vca);
  size_t intoMixer = 0, intoVca = 0;
  for (const doc::Wire& w : u.wires()) {
    if (w.mixer == u.keys) {
      ++intoMixer;
      CHECK(w.from == vca);
    }
    if (w.to == vca && (w.param.empty() || w.param == mp.in)) ++intoVca;
  }
  CHECK(intoMixer == 1);
  CHECK(intoVca == 3);
  CHECK(u.wired(u.a, "out", vca, mp.in));
  CHECK(u.wired(u.b, "out", vca, mp.in));
  const std::vector<doc::DeviceId> path = u.path();  // the path's end goes through the VCA
  REQUIRE(path.size() >= 2);
  CHECK(path[path.size() - 2] == u.sat);
  CHECK(path.back() == vca);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // Beside the input on its left (there is room there), over no block.
  const Node& vn = u.pp().nodes[u.node(vca)];
  const Node& mn = u.pp().nodes[u.pp().bands[u.pp().bandOf(u.keys)].mixer];
  CHECK(vn.x + vn.w <= mn.x);
  CHECK(mn.x - (vn.x + vn.w) < 120.0f);
  CHECK(u.overlapsNothing(vca));
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
}

TEST_CASE("wire cut: Z over a cable still inserts a VCA into that one cable; Z over a port without cables does nothing") {
  Ui u;
  const doc::Snapshot start = u.ctx.doc();
  const size_t h0 = u.history();
  u.d.move(u.onCable(u.cableOf(u.lfoWire)));
  u.key('z');
  u.view().sync();
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId vca = u.newestVca();
  REQUIRE(u.device(vca) != nullptr);
  const MainPorts mp = u.view().mainPorts(vca);
  CHECK(doc::findWire(*u.ctx.doc(), u.lfoWire) == nullptr);
  CHECK(u.wired(u.lfo, "y", vca, mp.in));
  CHECK(u.wired(vca, mp.out, u.util, "g"));
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
  // b's input has no cable: nothing (the status line says so).
  const Vec2 bIn = u.inPin(u.b, "x");
  REQUIRE(u.canvas().pinAt(bIn).valid());
  REQUIRE(u.canvas().pinAt(bIn).input);
  u.d.move(bIn);
  u.key('z');
  CHECK(u.history() == h0);
  CHECK(u.ctx.doc() == start);
  CHECK(u.ctx.status().find("no cable") != std::string::npos);
}
