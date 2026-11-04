// TX.orbit-ports (SPEC §15 2026-11-03 "orbiting ports, a round VCA, Delete on a cable"): a block's main input and main
// output orbit its outline toward their cables (pq/views/circuit/orbit.h). The outline geometry and the placement rule;
// every block's main ports orbit in the model and the scene, the other pins keep their places; through real event
// dispatch (window → canvas → face) over the user's meta patch (tests/views/fixtures/meta-cables): ports follow a moved
// target during the lifted drag and after the drop (animated, then idle), a port dragged along its block's edge is pinned
// (view state, saved), a double-click unpins it, an output dragged away from its block draws a cable; ports keep their
// spacing; the round VCA's floating panel opens, edits (one undo step each) and closes with nothing moving; Delete over a
// hovered cable deletes that cable (the hover wins over the selection). Goldens project-orbit-ports, project-vca-panel.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/edit_ops.h"
#include "pq/document/serialize.h"
#include "pq/meta/vca.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/orbit.h"
#include "pq/views/fake_transport.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::project_patch;
using graph::Point2;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

float dist(Point2 a, Point2 b) { return std::hypot(a.x - b.x, a.y - b.y); }
std::string fixture() { return views::test::fixtureDir("meta-cables"); }

// The shell over the meta patch, the patch view shown, its band framed.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;

  explicit Ui(bool animated = false) {
    ctx.setDeviceLibrary(&library());
    REQUIRE(ctx.openProject(fixture()).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setShown(true, false);
    d.tree().layoutNow();
    canvas().setCablesAnimated(animated);
    const doc::Track* midi = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (midi == nullptr && t.kind == doc::TrackKind::Midi) midi = &t;
    });
    REQUIRE(midi != nullptr);
    view().focusBand(midi->id, false);
    d.tree().layoutNow();
    settle();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  ProjectCanvas& canvas() const { return view().canvas(); }
  const ProjectPatch& pp() const { return view().patch(); }
  const circuit::CircuitScene& scene() const { return *view().scene().scene; }
  size_t history() const { return ctx.store().history().cursor(); }
  Vec2 dev(Point2 w) const { return canvas().toDevice(w); }
  // The model node of the face of meta type `id` (the fixture has one of each).
  uint32_t faceNode(std::string_view id) const {
    for (uint32_t i = 0; i < pp().nodes.size(); ++i)
      if (pp().nodes[i].face != nullptr && pp().nodes[i].face->id == id) return i;
    FAIL("no " << id << " block");
    return kNone;
  }
  const Node& node(uint32_t n) const { return pp().nodes[n]; }
  doc::DeviceId device(std::string_view id) const { return node(faceNode(id)).device; }
  // A block's main input / output: its scene pin, where it is drawn now (world), its angle.
  uint32_t mainPin(uint32_t n, bool input) const {
    const Node& b = node(n);
    REQUIRE((input ? b.orbitIn.on : b.orbitOut.on));
    return canvas().scenePin(n, input ? static_cast<uint32_t>(b.mainIn) : 0u, input);
  }
  Point2 portAt(uint32_t n, bool input) const { return canvas().pinPoint(mainPin(n, input)); }
  // Frames until the canvas asks for none (≤ `limit` s); the time it took.
  double settle(double limit = 5.0) {
    const double t0 = d.now();
    while (d.now() - t0 < limit && (canvas().cablesMoving() || canvas().portsMoving() || d.tree().needsFrame())) d.advance(1.0 / 144.0);
    return d.now() - t0;
  }
  // The VCA's frame (not a control): left of its knob, inside its circle.
  // A press on the VCA's frame (its rim, outside the knob: TX.vca-knob's knob fills the circle to r ≈ 53 of 58, so the
  // old grip at (12, 63) is on the knob now), at 9 o'clock where no marker's tick is.
  Vec2 vcaGrip() const {
    const Node& n = node(faceNode("vca"));
    return dev({n.x + 4.0f * n.scale, n.y + 61.0f * n.scale});
  }
  // A point of the canvas over nothing (no block, cable, lane window): a press there is the canvas's.
  Vec2 emptyPoint() const {
    const IRect r = canvas().rect();
    for (int y = r.y + 20; y < r.bottom() - 20; y += 23)
      for (int x = r.x + 20; x < r.right() - 20; x += 37) {
        const Vec2 p{static_cast<float>(x), static_cast<float>(y)};
        if (canvas().hitTest(p).kind == circuit::CanvasHit::Kind::None && canvas().nodeAt(p) == kNone && view().laneAt(p) == kNone &&
            canvas().bandAt(p) == kNone)
          return p;
      }
    FAIL("no empty point on the canvas");
    return {};
  }
  MetaFace& vcaFace() const {
    MetaFace* f = canvas().faceOn(view().scene().sceneNode[faceNode("vca")]);
    REQUIRE(f != nullptr);
    return *f;
  }
};

// The direction (radians) from a to b.
float toward(Point2 a, Point2 b) { return circuit::wrapAngle(std::atan2(b.y - a.y, b.x - a.x)); }

}  // namespace

// ---- the outline geometry and the placement rule --------------------------------------------------------------------

TEST_CASE("orbit ports: outline points, the port's place, the mean direction, spacing and pins") {
  using namespace circuit;
  Outline c;
  c.round = true;
  c.cx = 100.0f, c.cy = 50.0f, c.r = 40.0f;
  Point2 n;
  const Point2 p0 = outlinePoint(c, 0.0f, &n);
  CHECK(p0.x == doctest::Approx(140.0f));
  CHECK(p0.y == doctest::Approx(50.0f));
  CHECK(n.x == doctest::Approx(1.0f));
  const Point2 down = orbitPoint(c, 0.5f * kPi, 2.0f);
  CHECK(down.x == doctest::Approx(100.0f).epsilon(1e-4));
  CHECK(down.y == doctest::Approx(50.0f + 40.0f + 2.0f * (kOrbitPortR + kOrbitGap)));
  // A rounded rect: a straight edge, then a corner (the point on the corner's arc, its normal out of the arc).
  Outline r;
  r.x0 = 0.0f, r.y0 = 0.0f, r.x1 = 200.0f, r.y1 = 100.0f, r.corner = 12.0f;
  const Point2 e = outlinePoint(r, 0.0f, &n);
  CHECK(e.x == doctest::Approx(200.0f));
  CHECK(e.y == doctest::Approx(50.0f));
  const Point2 k = outlinePoint(r, std::atan2(-50.0f, -100.0f), &n);  // toward the top-left corner
  CHECK(dist(k, {12.0f, 12.0f}) == doctest::Approx(12.0f).epsilon(1e-3));
  CHECK(n.x < 0.0f);
  CHECK(n.y < 0.0f);
  for (float a = 0.0f; a < 2.0f * kPi; a += 0.1f) {
    CHECK(std::fabs(outlineDistance(r, outlinePoint(r, a))) < 1e-3f);
    CHECK(outlineDistance(r, orbitPoint(r, a, 1.5f)) == doctest::Approx((kOrbitPortR + kOrbitGap) * 1.5f).epsilon(1e-3));
    CHECK(outlineDistance(c, orbitPoint(c, a, 1.0f)) == doctest::Approx(kOrbitPortR + kOrbitGap).epsilon(1e-3));
  }
  // The shortest path: toward one target; several: their mean direction; none or cancelling: the default.
  const Point2 one[1] = {{100.0f, 300.0f}};
  CHECK(orbitAngle(c, one, 0.0f) == doctest::Approx(0.5f * kPi));
  const Point2 two[2] = {{300.0f, 50.0f}, {100.0f, 300.0f}};
  CHECK(orbitAngle(c, two, 0.0f) == doctest::Approx(0.25f * kPi).epsilon(1e-4));
  const Point2 opposite[2] = {{300.0f, 50.0f}, {-100.0f, 50.0f}};
  CHECK(orbitAngle(c, opposite, 1.0f) == doctest::Approx(1.0f));
  CHECK(orbitAngle(c, {}, kOrbitDefaultIn) == doctest::Approx(kOrbitDefaultIn));
  // Both ports toward the same target: the output gives way (≥ two radii and the spacing apart), the input keeps it.
  const bool on[2] = {true, true}, free2[2] = {false, false};
  const std::span<const Point2> same[2] = {one, one};
  float a2[2] = {0.0f, 0.0f};
  solveOrbits(c, 1.0f, on, free2, same, {}, a2);
  CHECK(a2[0] == doctest::Approx(0.5f * kPi));
  CHECK(dist(orbitPoint(c, a2[0], 1.0f), orbitPoint(c, a2[1], 1.0f)) >= 2.0f * kOrbitPortR + kOrbitSpacing - 1e-3f);
  // A pinned port keeps its angle; the other gives way to it.
  const bool pinOut[2] = {false, true};
  float a3[2] = {0.0f, 0.5f * kPi};
  solveOrbits(c, 1.0f, on, pinOut, same, {}, a3);
  CHECK(a3[1] == doctest::Approx(0.5f * kPi));
  CHECK(dist(orbitPoint(c, a3[0], 1.0f), orbitPoint(c, a3[1], 1.0f)) >= 2.0f * kOrbitPortR + kOrbitSpacing - 1e-3f);
  // A fixed pin of the block where the port would go: the port steps along the outline clear of it.
  const Point2 fixedPin[1] = {orbitPoint(c, 0.5f * kPi, 1.0f)};
  const bool inOnly[2] = {true, false};
  float a4[2] = {0.0f, 0.0f};
  const std::span<const Point2> t4[2] = {one, {}};
  solveOrbits(c, 1.0f, inOnly, free2, t4, fixedPin, a4);
  CHECK(dist(orbitPoint(c, a4[0], 1.0f), fixedPin[0]) >= kOrbitPortR + kOrbitPinR + kOrbitSpacing - 1e-3f);
}

// ---- the model and the scene ----------------------------------------------------------------------------------------

TEST_CASE("orbit ports: every block's main ports orbit its outline toward their cables; other pins keep their places") {
  Ui u;
  const ProjectPatch& pp = u.pp();
  const circuit::CircuitScene& sc = u.scene();
  uint32_t blocks = 0, single = 0;
  for (uint32_t i = 0; i < pp.nodes.size(); ++i) {
    const Node& n = pp.nodes[i];
    if (n.kind != NodeKind::Device || n.expanded) {
      CHECK_FALSE(n.orbitIn.on);
      CHECK_FALSE(n.orbitOut.on);
      continue;
    }
    ++blocks;
    // From doc::mainPortsOf: the main input (not an anchored control) and the first output.
    CHECK(n.orbitOut.on == !n.outputs.empty());
    CHECK(n.orbitIn.on == (n.mainIn >= 0 && !n.inputs[static_cast<size_t>(n.mainIn)].anchored));
    REQUIRE(n.outline.valid());
    if (n.face != nullptr) CHECK(n.outline.round == faceShape(*n.face).round);
    const uint32_t sn = u.view().scene().sceneNode[i];
    REQUIRE(sn != kNone);
    for (int side = 0; side < 2; ++side) {
      const bool input = side == 0;
      const Node::Orbit& o = input ? n.orbitIn : n.orbitOut;
      if (!o.on) continue;
      // Just outside the outline; the scene's pin there, marked; the cables start there.
      CHECK(circuit::outlineDistance(n.outline, o.at) == doctest::Approx((circuit::kOrbitPortR + circuit::kOrbitGap) * n.scale).epsilon(1e-3));
      const uint32_t p = u.canvas().scenePin(i, input ? static_cast<uint32_t>(n.mainIn) : 0u, input);
      REQUIRE(p != kNone);
      CHECK(sc.pins[p].orbit);
      CHECK(sc.pins[p].x == doctest::Approx(o.at.x));
      CHECK(sc.pins[p].y == doctest::Approx(o.at.y));
      CHECK(sc.nodes[sn].outline.valid());
      // The shortest path: a port with one cable points at its other end (that end's block centre when it orbits too,
      // a control's centre, a pin), unless the spacing moved it.
      std::vector<Point2> ends;
      for (const Edge& e : pp.edges) {
        if (!e.cable) continue;
        if (!input && e.from == i && e.fromPort == 0) {
          const Node& t = pp.nodes[e.to];
          ends.push_back(portOrbits(t, e.toPort, true) ? t.outline.centre() : e.points.back());
        }
        if (input && e.to == i && static_cast<int32_t>(e.toPort) == n.mainIn) {
          const Node& f = pp.nodes[e.from];
          ends.push_back(portOrbits(f, e.fromPort, false) ? f.outline.centre() : e.points.front());
        }
      }
      if (ends.empty()) {
        CHECK(o.angle == doctest::Approx(input ? circuit::kOrbitDefaultIn : circuit::kOrbitDefaultOut).epsilon(0.2));
      } else {
        const float want = circuit::orbitAngle(n.outline, ends, 0.0f);
        const bool spaced = std::fabs(circuit::angleDelta(want, o.angle)) > 1e-3f;
        if (!spaced && ends.size() == 1) ++single;
        if (spaced) MESSAGE(n.title << (input ? " in" : " out") << ": moved by the spacing rule");
      }
    }
    // The other pins keep their places (left / right edge rows); none within the spacing of a port.
    for (uint32_t k = 0; k < sc.nodes[sn].pinCount; ++k) {
      const circuit::ScenePin& pin = sc.pins[sc.nodes[sn].firstPin + k];
      if (pin.orbit || !pin.visible) continue;
      CHECK((pin.x == doctest::Approx(n.x) || pin.x == doctest::Approx(n.x + n.w)));
      for (const Node::Orbit* o : {&n.orbitIn, &n.orbitOut})
        if (o->on) CHECK(dist(o->at, {pin.x, pin.y}) >= (circuit::kOrbitPortR + circuit::kOrbitPinR + circuit::kOrbitSpacing) * n.scale - 1e-3f);
    }
    if (n.orbitIn.on && n.orbitOut.on)
      CHECK(dist(n.orbitIn.at, n.orbitOut.at) >= (2.0f * circuit::kOrbitPortR + circuit::kOrbitSpacing) * n.scale - 1e-3f);
  }
  CHECK(blocks >= 5);  // accumulator, wavetable, envelope, oscilloscope, VCA
  CHECK(single >= 3);
  // The VCA is round: its ports go round its circle.
  const Node& vca = pp.nodes[u.faceNode("vca")];
  CHECK(vca.outline.round);
  CHECK(dist(vca.orbitIn.at, vca.outline.centre()) == doctest::Approx(vca.outline.r + circuit::kOrbitPortR + circuit::kOrbitGap).epsilon(1e-4));
}

// ---- through real dispatch --------------------------------------------------------------------------------------------

TEST_CASE("orbit ports: a lifted drag turns the ports toward where the blocks are drawn; they slide, then idle") {
  Ui u(true);
  const uint32_t vca = u.faceNode("vca"), env = u.faceNode("envelope");
  const uint32_t vin = u.mainPin(vca, true), eout = u.mainPin(env, false);
  const float vin0 = u.canvas().portAngle(vin), eout0 = u.canvas().portAngle(eout);
  const Point2 envC = u.node(env).outline.centre();
  // The VCA's input points at the envelope (its one cable, from an orbiting output: toward that block's centre).
  CHECK(std::fabs(circuit::angleDelta(vin0, toward(u.node(vca).outline.centre(), envC))) < 1e-3f);
  CHECK_FALSE(u.canvas().portsMoving());
  u.d.advance(0.5);
  CHECK_FALSE(u.d.tree().needsFrame());
  // Drag the VCA well right of the envelope and down: lifted (no re-layout), its ports and the envelope's turn.
  const Vec2 grip = u.vcaGrip();
  const uint64_t solves0 = u.canvas().portSolves(), relayouts0 = u.view().stats().relayouts;
  u.d.move(grip);
  u.d.down(grip);
  const Vec2 to{grip.x + 900.0f, grip.y + 60.0f};
  for (int k = 1; k <= 8; ++k) {
    u.d.move({grip.x + (to.x - grip.x) * static_cast<float>(k) / 8.0f, grip.y + (to.y - grip.y) * static_cast<float>(k) / 8.0f});
    u.d.advance(1.0 / 144.0);
  }
  REQUIRE(u.canvas().drag() == ProjectCanvas::Drag::MoveDevice);
  CHECK(u.view().stats().relayouts == relayouts0);
  const Point2 off = u.canvas().nodeDragOffset();
  const Point2 vcaC{u.node(vca).outline.centre().x + off.x, u.node(vca).outline.centre().y + off.y};
  // The targets: toward where the other block is drawn now.
  CHECK(std::fabs(circuit::angleDelta(u.canvas().portTarget(vin), toward(vcaC, envC))) < 1e-3f);
  CHECK(std::fabs(circuit::angleDelta(u.canvas().portTarget(vin), vin0)) > 0.3f);
  CHECK(std::fabs(circuit::angleDelta(u.canvas().portTarget(eout), eout0)) > 0.01f);
  // Only the ports on the lifted block and on its cables' other ends were solved (a few per move, not every block).
  const uint64_t perMove = (u.canvas().portSolves() - solves0) / 8;
  CHECK(perMove >= 2);
  CHECK(perMove <= 4);
  // Animated: the slide runs frames, the cable ends sit on the ports where they are drawn.
  CHECK(u.canvas().portsMoving());
  const uint64_t f0 = u.d.framesDrawn();
  u.d.advance(0.05);
  CHECK(u.d.framesDrawn() > f0);
  for (int k = 0; k < 60; ++k) u.d.advance(1.0 / 144.0);
  CHECK(std::fabs(circuit::angleDelta(u.canvas().portAngle(vin), u.canvas().portTarget(vin))) < 0.01f);
  for (uint32_t w = 0; w < u.scene().wires.size(); ++w) {
    const circuit::SceneWire& sw = u.scene().wires[w];
    if (!sw.cable || sw.toPin != vin) continue;
    const Point2 p = u.canvas().pinPoint(vin);
    CHECK(dist(u.canvas().cablePoints(w).back(), {p.x + off.x, p.y + off.y}) < 0.01f);
  }
  // The drop: one nudge (view state), the new scene settles the ports where the drag had them (no jump), then idle.
  const size_t h0 = u.history();
  u.d.up(to);
  CHECK(u.history() == h0);
  const uint32_t vin2 = u.mainPin(u.faceNode("vca"), true);
  CHECK(std::fabs(circuit::angleDelta(u.canvas().portAngle(vin2), u.canvas().portTarget(vin2))) < 0.05f);
  CHECK(u.settle(5.0) <= 2.5);
  CHECK_FALSE(u.canvas().portsMoving());
  CHECK(u.canvas().portAngle(vin2) == u.scene().pins[vin2].angle);
  const uint64_t idle = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.d.framesDrawn() == idle);
}

TEST_CASE("orbit ports: a port dragged along its block's edge is pinned (view state, saved); a double-click unpins") {
  Ui u;
  const uint32_t vca = u.faceNode("vca");
  const doc::DeviceId vcaId = u.device("vca");
  const uint32_t vin = u.mainPin(vca, true);
  const float a0 = u.canvas().portAngle(vin);
  const Point2 c = u.node(vca).outline.centre();
  const float rr = u.node(vca).outline.r + circuit::kOrbitPortR + circuit::kOrbitGap;
  const size_t h0 = u.history();
  // Along the circle, 100° clockwise, through real dispatch: the port follows the pointer.
  const float a1 = circuit::wrapAngle(a0 + 100.0f * kPi / 180.0f);
  u.d.move(u.dev(u.canvas().pinPoint(vin)));
  u.d.down(u.dev(u.canvas().pinPoint(vin)));
  for (int k = 1; k <= 10; ++k) {
    const float a = a0 + (a1 - a0 + (a1 < a0 ? 2.0f * kPi : 0.0f)) * static_cast<float>(k) / 10.0f;
    u.d.move(u.dev({c.x + rr * std::cos(a), c.y + rr * std::sin(a)}));
  }
  CHECK(u.canvas().drag() == ProjectCanvas::Drag::Port);
  CHECK(std::fabs(circuit::angleDelta(u.canvas().portAngle(vin), a1)) < 0.02f);
  u.d.up(u.dev({c.x + rr * std::cos(a1), c.y + rr * std::sin(a1)}));
  CHECK(u.history() == h0);  // view state, not an undo step
  REQUIRE(u.view().portPinned(vcaId, true));
  const doc::PatchNudge& pin = u.ctx.store().view().patchNudges.at(vcaId.value);
  CHECK(pin.inPinned);
  CHECK(pin.inAngle == doc::patchPinQuantise(pin.inAngle));
  CHECK(std::fabs(circuit::angleDelta(pin.inAngle, a1)) < 0.02f);
  CHECK(u.node(u.faceNode("vca")).orbitIn.pinned);
  CHECK(u.node(u.faceNode("vca")).orbitIn.angle == doctest::Approx(pin.inAngle));
  // Saved with the project and read back exactly.
  auto text = doc::serializeProject(*u.ctx.doc(), u.ctx.store().view());
  REQUIRE(text.ok());
  CHECK(text->find("\"pinIn\"") != std::string::npos);
  auto back = doc::parseProject(*text);
  REQUIRE(back.ok());
  REQUIRE(back->view.patchNudges.count(vcaId.value) == 1);
  CHECK(back->view.patchNudges.at(vcaId.value) == pin);
  // Pinned: dragging the block keeps it at its angle (the output still follows).
  const Vec2 grip = u.vcaGrip();
  u.d.drag(grip, {grip.x + 400.0f, grip.y - 30.0f}, 8);
  u.settle();
  const uint32_t vca2 = u.faceNode("vca");
  CHECK(u.node(vca2).orbitIn.angle == doctest::Approx(pin.inAngle));
  CHECK(u.canvas().portAngle(u.mainPin(vca2, true)) == doctest::Approx(pin.inAngle));
  // A double-click on it: back to following its cable.
  u.d.doubleClick(u.dev(u.portAt(vca2, true)));
  u.settle();
  CHECK_FALSE(u.view().portPinned(vcaId, true));
  const uint32_t vca3 = u.faceNode("vca");
  CHECK_FALSE(u.node(vca3).orbitIn.pinned);
  const Point2 envC = u.node(u.faceNode("envelope")).outline.centre();
  CHECK(std::fabs(circuit::angleDelta(u.node(vca3).orbitIn.angle, toward(u.node(vca3).outline.centre(), envC))) < 1e-3f);
  CHECK(u.history() == h0);
}

TEST_CASE("orbit ports: an output dragged away from its block draws a cable; along its edge it slides; ports keep apart") {
  Ui u;
  const uint32_t vca = u.faceNode("vca");
  const doc::DeviceId vcaId = u.device("vca");
  const uint32_t vout = u.mainPin(vca, false);
  const Point2 c = u.node(vca).outline.centre();
  const float a0 = u.canvas().portAngle(vout);
  const Point2 p0 = u.canvas().pinPoint(vout);
  // Straight out (along its normal): a cable on the pointer.
  u.d.move(u.dev(p0));
  u.d.down(u.dev(p0));
  for (int k = 1; k <= 4; ++k)
    u.d.move(u.dev({p0.x + std::cos(a0) * 20.0f * static_cast<float>(k), p0.y + std::sin(a0) * 20.0f * static_cast<float>(k)}));
  CHECK(u.canvas().drag() == ProjectCanvas::Drag::Wire);
  CHECK(u.canvas().looseCable());
  (void)u.d.keyDown(key::kEscape);
  u.d.up(u.dev({p0.x + std::cos(a0) * 80.0f, p0.y + std::sin(a0) * 80.0f}));
  CHECK_FALSE(u.canvas().looseCable());
  // Along its edge: it slides (no cable), and pins there on release.
  const float rr = u.node(vca).outline.r + circuit::kOrbitPortR + circuit::kOrbitGap;
  u.d.move(u.dev(p0));
  u.d.down(u.dev(p0));
  for (int k = 1; k <= 12; ++k) {
    const float a = a0 - 0.12f * static_cast<float>(k);
    u.d.move(u.dev({c.x + rr * std::cos(a), c.y + rr * std::sin(a)}));
  }
  CHECK(u.canvas().drag() == ProjectCanvas::Drag::Port);
  CHECK_FALSE(u.canvas().looseCable());
  const float a1 = a0 - 0.12f * 12.0f;
  u.d.up(u.dev({c.x + rr * std::cos(a1), c.y + rr * std::sin(a1)}));
  REQUIRE(u.view().portPinned(vcaId, false));
  // Ports never overlap: the input pinned onto the output's place, the output (pinned too) keeps it and the input…
  // both pinned is the user's choice; unpin the output and it gives way to the pinned input.
  REQUIRE(u.view().pinPort(vcaId, true, a1));
  REQUIRE(u.view().unpinPort(vcaId, false));
  u.settle();
  const Node& n = u.node(u.faceNode("vca"));
  CHECK(n.orbitIn.pinned);
  CHECK_FALSE(n.orbitOut.pinned);
  CHECK(dist(n.orbitIn.at, n.orbitOut.at) >= (2.0f * circuit::kOrbitPortR + circuit::kOrbitSpacing) * n.scale - 1e-3f);
  const Point2 pi = u.portAt(u.faceNode("vca"), true), po = u.portAt(u.faceNode("vca"), false);
  CHECK(dist(pi, po) >= (2.0f * circuit::kOrbitPortR + circuit::kOrbitSpacing) * n.scale - 1e-3f);
}

TEST_CASE("orbit ports: the VCA's floating panel opens, edits and closes through dispatch; nothing moves") {
  Ui u;
  const uint32_t vca = u.faceNode("vca");
  const Node n0 = u.node(vca);
  REQUIRE(n0.outline.round);
  std::vector<std::array<float, 4>> rects;
  for (const Node& n : u.pp().nodes) rects.push_back({n.x, n.y, n.w, n.h});
  const uint64_t rebuilds0 = u.view().stats().rebuilds, relayouts0 = u.view().stats().relayouts;
  const size_t h0 = u.history();
  auto at = [&](float lx, float ly) {
    const Node& n = u.node(u.faceNode("vca"));
    return u.dev({n.x + lx * n.scale, n.y + ly * n.scale});
  };
  for (const double zoom : {0.6, 2.0}) {
    CAPTURE(zoom);
    circuit::CircuitCanvas::View v;
    v.zoom = zoom;
    const IRect cr = u.canvas().rect();
    v.x = n0.x + 60.0 - cr.w / zoom * 0.5;
    v.y = n0.y + 120.0 - cr.h / zoom * 0.5;
    u.canvas().setView(v);
    u.d.step();
    // The chevron opens it (no edit, no rebuild); the panel is under the face, over the canvas.
    CHECK_FALSE(u.vcaFace().popupRect().has_value());
    u.d.click(at(60.0f, 85.0f));
    REQUIRE(u.vcaFace().popupRect().has_value());
    CHECK(u.vcaFace().popupRect()->y0 >= 120.0f);
    CHECK(u.history() == h0);
    // It follows the face at this zoom: its controls are hit where the face's units put them.
    u.d.move(at(24.0f, 143.0f));
    CHECK(std::string(u.canvas().info().title) == "Min");
    u.d.move(at(60.0f, 169.0f));
    CHECK(std::string(u.canvas().info().title).starts_with("Curve"));
    // Esc closes it; the chevron opens it again and closes it; a click on empty canvas closes it.
    (void)u.d.keyDown(key::kEscape);
    CHECK_FALSE(u.vcaFace().popupRect().has_value());
    u.d.click(at(60.0f, 85.0f));
    REQUIRE(u.vcaFace().popupRect().has_value());
    u.d.click(at(60.0f, 85.0f));
    CHECK_FALSE(u.vcaFace().popupRect().has_value());
    u.d.click(at(60.0f, 85.0f));
    REQUIRE(u.vcaFace().popupRect().has_value());
    u.d.click(u.emptyPoint());
    CHECK_FALSE(u.vcaFace().popupRect().has_value());
  }
  // An edit in the panel: one undo step (a drag on max, a preset click); the panel stays open over a press inside it.
  u.d.click(at(60.0f, 85.0f));
  REQUIRE(u.vcaFace().popupRect().has_value());
  const doc::DeviceId id = u.device("vca");
  auto value = [&](size_t p) { return doc::metaParamValue(*doc::findDevice(*u.ctx.doc(), id).device, "", meta::vcaType(), p); };
  const double max0 = value(meta::kVcaMax);
  const Vec2 mx = at(96.0f, 143.0f);
  u.d.drag(mx, {mx.x, mx.y + 30.0f}, 6);
  CHECK(u.history() == h0 + 1);
  CHECK(value(meta::kVcaMax) < max0);
  u.d.click(at(15.0f + 24.0f * 2.0f + 9.0f, 194.0f));  // the Exponential preset
  CHECK(u.history() == h0 + 2);
  CHECK(u.vcaFace().popupRect().has_value());
  REQUIRE(u.ctx.undo());
  REQUIRE(u.ctx.undo());
  CHECK(value(meta::kVcaMax) == doctest::Approx(max0));
  // Nothing moved: every block where it was, no re-layout for the panel (the preset's setting rebuilt the lowering only).
  u.d.step();
  REQUIRE(u.pp().nodes.size() == rects.size());
  for (size_t i = 0; i < rects.size(); ++i) {
    const Node& n = u.pp().nodes[i];
    CHECK(n.x == rects[i][0]);
    CHECK(n.y == rects[i][1]);
    CHECK(n.w == rects[i][2]);
    CHECK(n.h == rects[i][3]);
  }
  (void)rebuilds0;
  (void)relayouts0;
}

TEST_CASE("orbit ports: Delete with the pointer on a cable deletes that cable — the hover wins over the selection") {
  Ui u;
  const uint32_t vca = u.faceNode("vca");
  const doc::DeviceId vcaId = u.device("vca");
  // Select the VCA (a click on its frame).
  u.d.click(u.vcaGrip());
  REQUIRE(u.view().selectedDevices() == std::vector<doc::DeviceId>{vcaId});
  // The envelope → VCA cable (into the selected block), hovered halfway along (over empty canvas).
  const uint32_t env = u.faceNode("envelope");
  uint32_t w = kNone;
  for (uint32_t k = 0; k < u.scene().wires.size(); ++k) {
    const circuit::SceneWire& sw = u.scene().wires[k];
    if (sw.cable && sw.fromPin == u.mainPin(env, false) && sw.toPin == u.mainPin(vca, true)) w = k;
  }
  REQUIRE(w != kNone);
  const uint32_t edge = u.view().scene().modelEdge[w];
  REQUIRE(edge != kNone);
  const doc::WireId wire = u.pp().edges[edge].wire;
  REQUIRE(wire.valid());
  // A point of the rope over empty canvas (not over a face's control, where a press goes to the control).
  Point2 mid = u.canvas().cableMidpoint(w);
  for (const Point2 q : u.canvas().cablePoints(w)) {
    const circuit::CanvasHit h = u.canvas().hitTest(u.dev(q));
    if (h.kind == circuit::CanvasHit::Kind::Wire && h.index == w && u.canvas().nodeAt(u.dev(q)) == kNone) {
      mid = q;
      break;
    }
  }
  u.d.move(u.dev(mid));
  REQUIRE(u.canvas().hovered().kind == circuit::CanvasHit::Kind::Wire);
  CHECK(u.canvas().hovered().index == w);
  const size_t h0 = u.history();
  CHECK(u.d.press("Delete") != KeyResult::Unhandled);
  CHECK(u.history() == h0 + 1);
  CHECK(doc::findWire(*u.ctx.doc(), wire) == nullptr);
  CHECK(doc::findDevice(*u.ctx.doc(), vcaId).device != nullptr);  // the selected block stays
  REQUIRE(u.ctx.undo());
  CHECK(doc::findWire(*u.ctx.doc(), wire) != nullptr);
  // Off every cable: Delete deletes the selection, as before.
  u.d.step();
  u.d.click(u.vcaGrip());
  u.d.move(u.emptyPoint());
  REQUIRE(u.canvas().hovered().kind != circuit::CanvasHit::Kind::Wire);
  CHECK(u.d.press("Delete") != KeyResult::Unhandled);
  CHECK(u.history() == h0 + 1);
  CHECK(doc::findDevice(*u.ctx.doc(), vcaId).device == nullptr);
}

// ---- goldens ----------------------------------------------------------------------------------------------------------

namespace {
void golden(const char* name) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(name, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(name, fixture(), w, h, {}, o, &library());
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

// The meta patch framed on the round VCA: every main port on its block's outline toward its cables.
TEST_CASE("render: project-orbit-ports matches the golden image") { golden("project-orbit-ports"); }
// … and the VCA's floating panel open under it (the range, the curve box, the presets).
TEST_CASE("render: project-vca-panel matches the golden image") { golden("project-vca-panel"); }
// TX.vca-knob: … with +1 and −10 moved off their defaults (an asymmetric fine zone: the ticks, the box's corner points).
TEST_CASE("render: project-vca-markers matches the golden image") { golden("project-vca-markers"); }
