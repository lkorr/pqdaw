// TX.cables (SPEC §15 2026-11-02 "cables and the node menu"): hanging, simulated, glowing cables between the project
// patch view's blocks. The rest shape (the catenary table), the rope simulation (it swings, settles and sleeps: no frames
// while idle), which connections are cables (buses stay traces), a block drag swinging its cables and the frames ending
// within 2 s of the drop, hit tests on the curve, the glow (a playing source lights its cable; silence: no repaints),
// the far level. Goldens: project-cables (near), project-cables-far, project-cables-glow over the user's meta patch.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "lone_instrument.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/cables.h"
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
using circuit::CableSim;
using graph::Point2;

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

float dist(Point2 a, Point2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

float ropeLength(std::span<const Point2> p) {
  float l = 0.0f;
  for (size_t i = 0; i + 1 < p.size(); ++i) l += dist(p[i], p[i + 1]);
  return l;
}

// IG.1-2: the user's meta patch (schema 16) loads with its meta voice run wrapped in an instrument group, the VCA wired to
// it with it. These tests are about free blocks on a band: the group is ungrouped again (Ctrl+Shift+G) — the run back on
// the path, the VCA free.
void ungroupAll(ViewContext& ctx) {
  std::vector<doc::TrackId> tracks;
  doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) { tracks.push_back(t.id); });
  for (const doc::TrackId t : tracks) view_fixture::loneInstrument(ctx.store(), library(), t);
}

// The shell over a project, the patch view shown without the morph.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;

  explicit Ui(const std::string& projectDir = {}) {
    ctx.setDeviceLibrary(&library());
    if (projectDir.empty()) REQUIRE(buildSketchProject(ctx.store()).ok());
    else {
      REQUIRE(ctx.openProject(projectDir).ok());
      ungroupAll(ctx);
    }
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setShown(true, false);
    // GU.4: a fixture's Phase accumulator and VCA load as their modules (groups, open by default): closed, each is one
    // block (drawn as its panel), as the old meta blocks were.
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      for (const doc::DevicePtr& dv : t.devices)
        if (doc::isGroupDevice(*dv) && doc::findModule(dv->name) != nullptr) view().setGroupOpen(dv->id, false);
    });
    d.tree().layoutNow();
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
  Vec2 dev(Point2 w) const { return canvas().toDevice(w); }
  // The scene wire of model edge k (kNone: none).
  uint32_t wireOf(uint32_t k) const {
    const ProjectScene& ps = view().scene();
    for (uint32_t w = 0; w < ps.modelEdge.size(); ++w)
      if (ps.modelEdge[w] == k) return w;
    return kNone;
  }
  // The longest cable of the scene (its sag is the largest).
  uint32_t longestCable() const {
    uint32_t best = kNone;
    float len = 0.0f;
    const circuit::CircuitScene& sc = scene();
    for (uint32_t w = 0; w < sc.wires.size(); ++w) {
      if (!sc.wires[w].cable) continue;
      const Point2 a = sc.points[sc.wires[w].firstPoint], b = sc.points[sc.wires[w].firstPoint + sc.wires[w].pointCount - 1];
      if (dist(a, b) > len) len = dist(a, b), best = w;
    }
    return best;
  }
  // Runs frames until the canvas asks for none (≤ `limit` s); returns the time it took.
  double settle(double limit = 5.0) {
    const double t0 = d.now();
    while (d.now() - t0 < limit && (canvas().cablesMoving() || d.tree().needsFrame())) d.advance(1.0 / 144.0);
    return d.now() - t0;
  }
};

std::string fixture() { return views::test::fixtureDir("meta-cables"); }

}  // namespace

// ---- the rest shape --------------------------------------------------------------------------------------------------

TEST_CASE("cables: the rest shape is the hanging rope — exact ends, its length, its sag, mirrored, any angle") {
  std::vector<Point2> p(32);
  // Level: a catenary of 1.06 × the chord sags ≈ 0.15 of it, lowest in the middle, symmetric.
  circuit::cableRest({100, 50}, {500, 50}, 32, p.data());
  CHECK(p.front() == Point2{100, 50});
  CHECK(p.back() == Point2{500, 50});
  CHECK(ropeLength(p) == doctest::Approx(400.0f * circuit::kCableSlack).epsilon(0.01));
  float low = 0.0f;
  for (const Point2 q : p) low = std::max(low, q.y - 50.0f);
  CHECK(low == doctest::Approx(0.154f * 400.0f).epsilon(0.05));
  for (size_t i = 0; i < p.size(); ++i) {
    CHECK(p[i].y >= 50.0f - 1e-3f);                                              // hangs down only
    CHECK(p[i].y == doctest::Approx(p[p.size() - 1 - i].y).epsilon(1e-3));        // symmetric
    CHECK(p[i].x + p[p.size() - 1 - i].x == doctest::Approx(600.0f).epsilon(1e-3));
  }
  // Equal arc length between points (the table's), within a few percent.
  for (size_t i = 0; i + 1 < p.size(); ++i) CHECK(dist(p[i], p[i + 1]) == doctest::Approx(400.0f * circuit::kCableSlack / 31.0f).epsilon(0.04));
  // Mirrored: b left of a is the same rope traced the other way.
  std::vector<Point2> m(32);
  circuit::cableRest({500, 50}, {100, 50}, 32, m.data());
  for (size_t i = 0; i < m.size(); ++i) {
    CHECK(m[i].x == doctest::Approx(p[p.size() - 1 - i].x).epsilon(1e-3));
    CHECK(m[i].y == doctest::Approx(p[p.size() - 1 - i].y).epsilon(1e-3));
  }
  // Any angle, steep and straight up / down included: finite, exact ends, the rope's length, never above the higher end.
  for (int deg = -90; deg <= 90; deg += 7) {
    const float r = 300.0f, a = static_cast<float>(deg) * 3.14159265f / 180.0f;
    const Point2 A{0, 0}, B{r * std::cos(a), r * std::sin(a)};
    for (const uint32_t n : {12u, 20u, 32u}) {
      std::vector<Point2> q(n);
      circuit::cableRest(A, B, n, q.data());
      CHECK(q.front() == A);
      CHECK(q.back() == B);
      for (const Point2 x : q) {
        CHECK(std::isfinite(x.x));
        CHECK(std::isfinite(x.y));
        CHECK(x.y >= std::min(A.y, B.y) - 0.5f);
      }
      // (Hanging straight down the rope folds under the lower end; points at equal arc length cut that fold short.)
      if (std::abs(deg) < 80) CHECK(ropeLength(q) == doctest::Approx(r * circuit::kCableSlack).epsilon(0.03));
      else CHECK(ropeLength(q) >= r - 0.01f);
    }
  }
  // Another slack is solved directly: tauter, less sag.
  std::vector<Point2> t(32);
  circuit::cableRest({100, 50}, {500, 50}, 32, t.data(), 1.01f);
  float lowT = 0.0f;
  for (const Point2 q : t) lowT = std::max(lowT, q.y - 50.0f);
  CHECK(lowT < 0.5f * low);
  CHECK(circuit::cablePointCount(0.0f) == circuit::kCableMinPoints);
  CHECK(circuit::cablePointCount(10000.0f) == circuit::kCableMaxPoints);
}

// ---- the simulation --------------------------------------------------------------------------------------------------

TEST_CASE("cables: the rope swings when an end moves, settles on its rest shape and sleeps within 2 s") {
  CableSim sim;
  const CableSim::Spec specs[2] = {{1, {0, 0}, {300, 0}}, {2, {0, 100}, {200, 160}}};
  sim.sync(specs);
  REQUIRE(sim.size() == 2);
  CHECK_FALSE(sim.awake());  // new cables start asleep on their rest shapes
  std::vector<Point2> rest(sim.points(0).size());
  circuit::cableRest({0, 0}, {300, 0}, static_cast<uint32_t>(rest.size()), rest.data());
  for (size_t i = 0; i < rest.size(); ++i) CHECK(dist(sim.points(0)[i], rest[i]) < 1e-4f);
  // Idle: advancing costs nothing.
  const uint64_t steps0 = sim.steps();
  CHECK_FALSE(sim.advance(1.0));
  CHECK(sim.steps() == steps0);
  // One end jumps 120 units right: the rope is carried most of the way and swings over the rest.
  sim.setEnds(0, {120, 0}, {300, 0});
  CHECK(sim.awake());
  CHECK(sim.awake(0));
  CHECK_FALSE(sim.awake(1));
  CHECK(sim.points(0).front() == Point2{120, 0});
  std::vector<Point2> rest2(sim.points(0).size());
  circuit::cableRest({120, 0}, {300, 0}, static_cast<uint32_t>(rest2.size()), rest2.data());
  float off0 = 0.0f;
  for (size_t i = 0; i < rest2.size(); ++i) off0 = std::max(off0, dist(sim.points(0)[i], rest2[i]));
  CHECK(off0 > 2.0f);  // displaced from its new rest: it will move
  double t = 0.0;
  bool moved = false;
  float worst = 0.0f;
  while (sim.awake() && t < 3.0) {
    moved = sim.advance(1.0 / 144.0) || moved;
    t += 1.0 / 144.0;
    for (const Point2 q : sim.points(0)) {
      REQUIRE(std::isfinite(q.x));
      REQUIRE(std::isfinite(q.y));
      worst = std::max(worst, std::fabs(q.y));
    }
  }
  CHECK(moved);
  CHECK(t <= 2.0);  // asleep within 2 s of the move
  CHECK(worst < 200.0f);  // a swing, not an explosion
  for (size_t i = 0; i < rest2.size(); ++i) CHECK(dist(sim.points(0)[i], rest2[i]) < 1e-4f);  // exactly on its rest shape
  CHECK(sim.points(0).front() == Point2{120, 0});
  CHECK(sim.points(0).back() == Point2{300, 0});
  // Animation off: a move lands at once, nothing wakes.
  sim.setAnimated(false);
  sim.setEnds(1, {0, 300}, {400, 300});
  CHECK_FALSE(sim.awake());
  std::vector<Point2> rest3(sim.points(1).size());
  circuit::cableRest({0, 300}, {400, 300}, static_cast<uint32_t>(rest3.size()), rest3.data());
  for (size_t i = 0; i < rest3.size(); ++i) CHECK(dist(sim.points(1)[i], rest3[i]) < 1e-4f);
}

TEST_CASE("cables: a kept connection keeps its motion across a rebuild; a new one starts at rest; settle()") {
  CableSim sim;
  const CableSim::Spec a[2] = {{10, {0, 0}, {300, 0}}, {11, {0, 50}, {300, 50}}};
  sim.sync(a);
  sim.setEnds(0, {0, -80}, {300, 0});
  for (int i = 0; i < 6; ++i) (void)sim.advance(1.0 / 144.0);
  REQUIRE(sim.awake(0));
  const std::vector<Point2> before(sim.points(0).begin(), sim.points(0).end());
  // The scene is rebuilt: 10 is kept (the same ends), 11 went, 12 is new; order changed.
  const CableSim::Spec b[2] = {{12, {0, 200}, {100, 260}}, {10, {0, -80}, {300, 0}}};
  sim.sync(b);
  REQUIRE(sim.size() == 2);
  CHECK(sim.awake(1));
  CHECK_FALSE(sim.awake(0));
  for (size_t i = 0; i < before.size(); ++i) CHECK(dist(sim.points(1)[i], before[i]) < 1e-5f);
  // A long move changes its point count (resampled), still finite and pinned.
  sim.setEnds(1, {0, -80}, {2000, 300});
  CHECK(sim.points(1).size() == circuit::kCableMaxPoints);
  CHECK(sim.points(1).back() == Point2{2000, 300});
  sim.settle();
  CHECK_FALSE(sim.awake());
  std::vector<Point2> rest(sim.points(1).size());
  circuit::cableRest({0, -80}, {2000, 300}, static_cast<uint32_t>(rest.size()), rest.data());
  for (size_t i = 0; i < rest.size(); ++i) CHECK(dist(sim.points(1)[i], rest[i]) < 1e-4f);
}

// ---- the project patch view -----------------------------------------------------------------------------------------

TEST_CASE("cables: every connection between blocks is a cable from port to port; buses stay traces") {
  Ui u;
  // (GU.11: the modules' panels lay out at their first paint: the LFO's wires end on their knobs from there)
  REQUIRE(u.d.enableGpu().ok());
  u.d.advance(1.0);
  const ProjectPatch& pp = u.pp();
  const circuit::CircuitScene& sc = u.scene();
  uint32_t cables = 0, traces = 0;
  for (uint32_t k = 0; k < pp.edges.size(); ++k) {
    const Edge& e = pp.edges[k];
    const uint32_t w = u.wireOf(k);
    REQUIRE(w != kNone);
    const circuit::SceneWire& sw = sc.wires[w];
    CHECK(sw.cable == e.cable);
    // TX.cable-look: a wire between two blocks of one opened group is a cable too; only the bus structure is a trace.
    CHECK(e.cable == isCable(e.kind, pp.nodes[e.from].kind));
    if (!sw.cable) {
      ++traces;
      CHECK((e.kind == EdgeKind::Route || e.kind == EdgeKind::Send || pp.nodes[e.from].kind == NodeKind::Mixer));
      continue;
    }
    ++cables;
    // From the source's pin to the target's pin (or control), its rest shape between; a key of its own.
    CHECK(sc.points[sw.firstPoint] == e.points.front());
    CHECK(sc.points[sw.firstPoint + sw.pointCount - 1] == e.points.back());
    CHECK(sw.pointCount >= circuit::kCableMinPoints);
    CHECK(sw.cableKey != 0);
    for (uint32_t w2 = 0; w2 < w; ++w2) CHECK((!sc.wires[w2].cable || sc.wires[w2].cableKey != sw.cableKey));
    // The canvas's rope at rest is the scene's rest shape.
    const std::span<const Point2> rope = u.canvas().cablePoints(w);
    CAPTURE(k);
    CAPTURE(pp.nodes[e.from].title);
    CAPTURE(pp.nodes[e.to].title);
    if (sw.control != UINT32_MAX) {
      // Onto a control (GU.11: a module's knob on its closed group's panel; TX.cable-look: the notes onto the Phase
      // Accumulator's Frequency arc): the rope ends on the control's rim, toward its source (CircuitCanvas cableEnds), the
      // scene's rest shape at its centre — a shorter chord, so its point count may be one less.
      REQUIRE_FALSE(rope.empty());
      CHECK(rope.size() <= sw.pointCount);
      CHECK(dist(rope.front(), sc.points[sw.firstPoint]) < 0.05f);
      CHECK(dist(rope.back(), sc.points[sw.firstPoint + sw.pointCount - 1]) < 30.0f);
      continue;
    }
    REQUIRE(rope.size() == sw.pointCount);
    for (uint32_t i = 0; i < sw.pointCount; ++i) CHECK(dist(rope[i], sc.points[sw.firstPoint + i]) < 0.05f);
  }
  CHECK(cables >= 8);
  CHECK(traces >= 4);
  // No junction dots on cables; idle.
  for (const uint32_t w : sc.junctionWire) CHECK_FALSE(sc.wires[w].cable);
  CHECK_FALSE(u.canvas().cablesMoving());
  // A rebuild that moves nothing (a rename) keeps every cable still.
  const doc::Track* keys = nullptr;
  doc::forEachTrack(*u.ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (keys == nullptr && t.name == "Keys") keys = &t;
  });
  REQUIRE(keys != nullptr);
  auto e = doc::ops::renameTrack(*u.ctx.doc(), keys->id, "Keys 2");
  REQUIRE(e.ok());
  REQUIRE(u.ctx.store().dispatch(e->command).ok());
  u.d.step();
  CHECK_FALSE(u.canvas().cablesMoving());
}

TEST_CASE("cables: a block drag swings its cables; the canvas stops requesting frames within 2 s of the drop") {
  Ui u;
  // A device block in the middle of a chain (cables on both sides), its band framed (near: cables animate there; far
  // out they do not): Ctrl+drag nudges it.
  uint32_t node = kNone;
  for (const Band& b : u.pp().bands)
    if (b.devices.size() >= 2 && node == kNone) {
      node = b.devices[1];
      u.view().focusBand(b.track, false);
    }
  REQUIRE(node != kNone);
  u.d.advance(0.5);
  REQUIRE(u.canvas().lod() != circuit::CircuitCanvas::Lod::Far);
  const ProjectPatch& pp0 = u.pp();
  const doc::DeviceId device = pp0.nodes[node].device;
  const float y0 = pp0.nodes[node].y;
  const Vec2 grip = u.dev({pp0.nodes[node].x + pp0.nodes[node].w * 0.5f, y0 + 10.0f});
  // Its cables (both ends' scene nodes).
  const ProjectScene& ps = u.view().scene();
  const uint32_t sn = ps.sceneNode[node];
  std::vector<uint32_t> mine;
  const circuit::CircuitScene& sc = u.scene();
  for (uint32_t w = 0; w < sc.wires.size(); ++w)
    if (sc.wires[w].cable && (sc.pins[sc.wires[w].fromPin].node == sn || sc.pins[sc.wires[w].toPin].node == sn)) mine.push_back(w);
  REQUIRE(mine.size() >= 2);
  const Point2 end0 = u.canvas().cablePoints(mine[0]).front(), last0 = u.canvas().cablePoints(mine[0]).back();
  u.d.advance(0.5);
  CHECK_FALSE(u.d.tree().needsFrame());
  // Drag: the cables' ends follow the outline and the ropes swing (frames run) while the scene is unchanged.
  const float dy = 90.0f;
  u.d.move(grip, mod::kCtrl);
  u.d.down(grip, PointerButton::Left, mod::kCtrl);
  for (int i = 1; i <= 6; ++i) {
    u.d.move({grip.x + 6.0f * static_cast<float>(i), grip.y + dy * static_cast<float>(i) / 6.0f}, mod::kCtrl);
    u.d.advance(1.0 / 144.0);
  }
  CHECK(u.canvas().cablesMoving());
  const float s = u.canvas().deviceScale();
  const std::span<const Point2> during = u.canvas().cablePoints(mine[0]);
  const bool fromDragged = sc.pins[sc.wires[mine[0]].fromPin].node == sn;
  const Point2 moved = fromDragged ? during.front() : during.back();
  const Point2 fixed = fromDragged ? during.back() : during.front();
  const Point2 movedRef = fromDragged ? end0 : last0, fixedRef = fromDragged ? last0 : end0;
  // TX.orbit-ports: each end is on its port where that is drawn now — the dragged block's at the drag offset (it went
  // down with the block), the other one's turned toward it along its own outline.
  const uint32_t movedPin = fromDragged ? sc.wires[mine[0]].fromPin : sc.wires[mine[0]].toPin;
  const uint32_t fixedPin = fromDragged ? sc.wires[mine[0]].toPin : sc.wires[mine[0]].fromPin;
  const Point2 off = u.canvas().nodeDragOffset();
  const Point2 mp = u.canvas().pinPoint(movedPin), fp = u.canvas().pinPoint(fixedPin);
  CHECK(off.y == doctest::Approx(dy / s).epsilon(0.02));
  CHECK(dist(moved, {mp.x + off.x, mp.y + off.y}) < 0.01f);
  CHECK(dist(fixed, fp) < 0.01f);
  CHECK(moved.y - movedRef.y > 0.5f * dy / s);
  (void)fixedRef;
  const uint64_t framesBefore = u.d.framesDrawn();
  u.d.advance(0.1);
  CHECK(u.d.framesDrawn() > framesBefore);  // animating
  // Drop: the nudge places the block; its cables swing on from where they were, then sleep — within 2 s.
  const Vec2 drop{grip.x + 36.0f, grip.y + dy};
  u.d.up(drop, PointerButton::Left, mod::kCtrl);
  const double took = u.settle(5.0);
  CHECK(took <= 2.0);
  CHECK_FALSE(u.canvas().cablesMoving());
  CHECK_FALSE(u.d.tree().needsFrame());
  // The block moved for real (a nudge), its cables at rest on the new ports.
  const uint32_t node2 = u.pp().nodeOf(device);
  REQUIRE(node2 != kNone);
  CHECK(u.pp().nodes[node2].y == doctest::Approx(y0 + dy / s).epsilon(0.02));
  // Idle: no frames, no timers.
  const uint64_t f0 = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.d.framesDrawn() == f0);
  CHECK(std::isinf(u.d.tree().nextWakeTime()));
}

// TX.node-menu's push-apart glides blocks (a re-layout per frame): the cables of a gliding block swing with it, then all
// of it settles and the canvas goes idle.
TEST_CASE("cables: a block pushed apart by a spawn glides, its cables swing; then idle") {
  Ui u(fixture());
  doc::TrackId midi;
  doc::forEachTrack(*u.ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (!midi.valid() && t.kind == doc::TrackKind::Midi) midi = t.id;
  });
  REQUIRE(midi.valid());
  u.view().focusBand(midi, false);
  u.d.advance(0.5);
  REQUIRE(u.canvas().lod() != circuit::CircuitCanvas::Lod::Far);
  // The free block (the VCA, wired both ways) and its cables.
  uint32_t vca = kNone;
  for (uint32_t i = 0; i < u.pp().nodes.size(); ++i)
    if (u.pp().nodes[i].kind == NodeKind::Device && u.pp().nodes[i].offPath) vca = i;
  REQUIRE(vca != kNone);
  const doc::DeviceId vcaId = u.pp().nodes[vca].device;
  const graph::Point2 on{u.pp().nodes[vca].x + u.pp().nodes[vca].w * 0.5f, u.pp().nodes[vca].y + u.pp().nodes[vca].h * 0.5f};
  const float x0 = u.pp().nodes[vca].x, y0 = u.pp().nodes[vca].y;
  u.view().setPushAnimated(true);
  const doc::DeviceId spawned = u.view().spawnNode(ProjectPatchView::specChoice("tanh"), on);
  REQUIRE(spawned.valid());
  REQUIRE(u.view().pushing());
  bool swung = false;
  for (int i = 0; i < 30 && !swung; ++i) {
    u.d.advance(1.0 / 144.0);
    swung = u.canvas().cablesMoving();
  }
  CHECK(swung);
  const double took = u.settle(5.0);
  CHECK(took <= 2.5);
  CHECK_FALSE(u.view().pushing());
  CHECK_FALSE(u.canvas().cablesMoving());
  const uint32_t moved = u.pp().nodeOf(vcaId);
  REQUIRE(moved != kNone);
  CHECK((u.pp().nodes[moved].x != x0 || u.pp().nodes[moved].y != y0));
  // Its cables hang from its pins where it is now, at rest.
  const circuit::CircuitScene& sc = u.scene();
  const uint32_t sn = u.view().scene().sceneNode[moved];
  uint32_t mine = 0;
  for (uint32_t w = 0; w < sc.wires.size(); ++w) {
    if (!sc.wires[w].cable) continue;
    const bool from = sc.pins[sc.wires[w].fromPin].node == sn, to = sc.pins[sc.wires[w].toPin].node == sn;
    if (!from && !to) continue;
    ++mine;
    const std::span<const Point2> rope = u.canvas().cablePoints(w);
    const Point2 end = from ? rope.front() : rope.back();
    const Point2 pin = from ? sc.points[sc.wires[w].firstPoint] : sc.points[sc.wires[w].firstPoint + sc.wires[w].pointCount - 1];
    CHECK(dist(end, pin) < 0.01f);
  }
  CHECK(mine >= 2);
  const uint64_t f0 = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.d.framesDrawn() == f0);
}

TEST_CASE("cables: hit tests follow the hanging curve (within 4 px), not the chord; over a face's control only the plug") {
  Ui u;
  // A cable whose lowest point hangs in the open (no block under it), the longest such: zoomed 1:1 onto that point.
  const circuit::CircuitScene& sc0 = u.scene();
  uint32_t w = kNone;
  float best = 0.0f;
  Point2 bottom{};
  for (uint32_t cw = 0; cw < sc0.wires.size(); ++cw) {
    if (!sc0.wires[cw].cable) continue;
    const std::span<const Point2> r = u.canvas().cablePoints(cw);
    size_t lo = 0;
    for (size_t i = 0; i < r.size(); ++i)
      if (r[i].y > r[lo].y) lo = i;
    bool free = lo > 0 && lo + 1 < r.size();
    for (const circuit::SceneNode& n : sc0.nodes)
      free = free && !(r[lo].x >= n.x - 8 && r[lo].x <= n.x + n.w + 8 && r[lo].y >= n.y - 8 && r[lo].y <= n.y + n.h + 8);
    if (free && dist(r.front(), r.back()) > best) best = dist(r.front(), r.back()), w = cw, bottom = r[lo];
  }
  REQUIRE(w != kNone);
  {
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = bottom.x - 0.5 * u.canvas().rect().w;
    v.y = bottom.y - 0.5 * u.canvas().rect().h;
    u.canvas().setView(v);
    u.d.advance(0.3);
  }
  const std::span<const Point2> rope = u.canvas().cablePoints(w);
  // The rope's lowest point, and the chord's middle well above it.
  size_t lowest = 0;
  for (size_t i = 0; i < rope.size(); ++i)
    if (rope[i].y > rope[lowest].y) lowest = i;
  const Point2 chordMid{0.5f * (rope.front().x + rope.back().x), 0.5f * (rope.front().y + rope.back().y)};
  const Vec2 onCurve = u.dev(rope[lowest]), onChord = u.dev(chordMid);
  REQUIRE(std::fabs(onCurve.y - onChord.y) > 12.0f);  // sagging enough to tell
  // Zoomed so that the curve's lowest point is on the canvas.
  REQUIRE(u.canvas().rect().contains(onCurve));
  const circuit::CanvasHit hit = u.canvas().hitTest(onCurve);
  // Something drawn over the cable there would win (a pin); the sketch's longest cable hangs in the open.
  CHECK(hit.kind == circuit::CanvasHit::Kind::Wire);
  CHECK(hit.index == w);
  CHECK(u.canvas().hitTest({onCurve.x, onCurve.y + 3.0f}).index == w);
  const circuit::CanvasHit off = u.canvas().hitTest({onCurve.x, onCurve.y + 12.0f});
  CHECK_FALSE((off.kind == circuit::CanvasHit::Kind::Wire && off.index == w));
  const circuit::CanvasHit chord = u.canvas().hitTest(onChord);
  CHECK_FALSE((chord.kind == circuit::CanvasHit::Kind::Wire && chord.index == w));
  // Hover and a click select it; Delete acts on it (whatever its kind does: one undo step or nothing).
  u.d.move(onCurve);
  CHECK(u.canvas().hovered().kind == circuit::CanvasHit::Kind::Wire);
  CHECK(u.canvas().hovered().index == w);
  u.d.click(onCurve);
  CHECK(u.canvas().wireSelected(w));
  // While it swings (a drag offset on its source's block), hits follow the rope as it hangs now.
  const circuit::CircuitScene& sc = u.scene();
  const uint32_t src = sc.pins[sc.wires[w].fromPin].node;
  const uint32_t nodes[1] = {src};
  u.canvas().setNodeDrag(nodes, {0.0f, -60.0f});
  for (int i = 0; i < 20; ++i) u.d.advance(1.0 / 144.0);
  const std::span<const Point2> now = u.canvas().cablePoints(w);
  size_t low2 = 0;
  for (size_t i = 0; i < now.size(); ++i)
    if (now[i].y > now[low2].y) low2 = i;
  const Vec2 p2 = u.dev(now[low2 > 0 && low2 + 1 < now.size() ? low2 : now.size() / 2]);
  if (u.canvas().rect().contains(p2)) CHECK(u.canvas().hitTest(p2).index == w);
  u.canvas().clearNodeDrag();
  u.settle();
  // Over a face's control a cable yields to the control (a press there turns it) but for its plug.
  Ui m(fixture());
  {  // GU.4: the fixture's cable onto a face control (the VCA into the Phase accumulator's frequency) ends on the
     // accumulator module's Frequency value block now; one onto the Wavetable's position control stands in for it.
    const doc::Device* env = nullptr;
    const doc::Device* wt = nullptr;
    doc::forEachTrack(*m.ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      for (const doc::DevicePtr& d : t.devices) {
        if (d->patch == "meta:envelope") env = d.get();
        if (d->patch == "meta:wavetable") wt = d.get();
      }
    });
    REQUIRE(env != nullptr);
    REQUIRE(wt != nullptr);
    auto e = doc::ops::connectWire(*m.ctx.doc(), m.ctx.store().ids(), env->id, "out", wt->id, "position");
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(m.ctx.store().dispatch(e->command).ok());
  }
  m.view().rebindFaces();
  doc::TrackId midi;
  doc::forEachTrack(*m.ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (!midi.valid() && t.kind == doc::TrackKind::Midi) midi = t.id;
  });
  REQUIRE(midi.valid());
  m.view().focusBand(midi, false);
  m.d.advance(0.3);
  const circuit::CircuitScene& ms = m.scene();
  uint32_t checkedControl = 0, checkedPlug = 0;
  for (uint32_t cw = 0; cw < ms.wires.size(); ++cw) {
    if (!ms.wires[cw].cable) continue;
    const std::span<const Point2> r = m.canvas().cablePoints(cw);
    // Its plug at the target: the cable's own.
    const Vec2 plug = m.dev(r.back());
    if (m.canvas().rect().contains(plug) && m.canvas().hitTest(plug).kind != circuit::CanvasHit::Kind::Pin) {
      CHECK(m.canvas().hitTest(plug).index == cw);
      ++checkedPlug;
    }
    for (size_t i = 2; i + 2 < r.size(); ++i) {
      const Vec2 dp = m.dev(r[i]);
      for (uint32_t f = 0; f < m.canvas().faceCount(); ++f) {
        const uint32_t fn = m.canvas().faceNode(f);
        const circuit::SceneNode& n = ms.nodes[fn];
        if (r[i].x < n.x || r[i].x > n.x + n.w || r[i].y < n.y || r[i].y > n.y + n.h) continue;
        const MetaFace& face = m.canvas().faceAt(f);
        const ui::Vec2 local{(r[i].x - n.x) / n.scale, (r[i].y - n.y) / n.scale};
        if (face.hitTest(local, m.canvas().deviceScale() * n.scale) < 0) continue;
        const circuit::CanvasHit h = m.canvas().hitTest(dp);
        CHECK_FALSE((h.kind == circuit::CanvasHit::Kind::Wire && h.index == cw));
        ++checkedControl;
      }
    }
  }
  CHECK(checkedPlug >= 1);  // a plug on a pin is the pin's (a new wire); on a control, the cable's
  CHECK(checkedControl >= 1);
}

TEST_CASE("cables: a playing source lights its cable (≤ 30 Hz while live); silence: no polls, no repaints") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());
  FixedMeterSource meters;
  u.view().setMeterSource(&meters);
  // A chain cable of the Keys track.
  const ProjectPatch& pp = u.pp();
  uint32_t edge = kNone;
  doc::TrackId track;
  for (uint32_t k = 0; k < pp.edges.size() && edge == kNone; ++k)
    if (pp.edges[k].kind == EdgeKind::Chain && pp.bands[pp.nodes[pp.edges[k].from].band].name == "Keys") {
      edge = k;
      track = pp.bands[pp.nodes[pp.edges[k].from].band].track;
    }
  REQUIRE(edge != kNone);
  const uint32_t w = u.wireOf(edge);
  REQUIRE(w != kNone);
  u.view().focusBand(track, false);
  u.d.advance(0.5);
  // Silent and stopped: nothing polls, nothing glows, no frames.
  u.canvas().wakeCables();
  CHECK_FALSE(u.canvas().cableGlowPolling());
  CHECK(u.canvas().cableGlow(w) == 0.0f);
  uint64_t f0 = u.d.framesDrawn();
  u.d.advance(0.5);
  CHECK(u.d.framesDrawn() == f0);
  // Playing: the track's level lights its cables; the glow repaints at ≤ 30 Hz (its light flows).
  ui::MeterLevels l;
  l.peakDb[0] = l.peakDb[1] = -6.0f;
  l.rmsDb[0] = l.rmsDb[1] = -12.0f;
  meters.set(track, l);
  meters.setLive(true);
  u.canvas().wakeCables();
  CHECK(u.canvas().cableGlowPolling());
  const uint64_t polls0 = u.canvas().stats().cableGlowPolls;
  f0 = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.canvas().cableGlow(w) > 0.5f);
  const uint64_t polls = u.canvas().stats().cableGlowPolls - polls0;
  CHECK(polls >= 20);
  CHECK(polls <= 31);
  const uint64_t frames = u.d.framesDrawn() - f0;
  CHECK(frames >= 20);
  CHECK(frames <= 31);
  CHECK(u.canvas().stats().cablesGlowing >= 1);
  // Live but silent (a quiet passage): the glow fades out, then it is polled, not repainted.
  l.peakDb[0] = l.peakDb[1] = -200.0f;
  meters.set(track, l);
  u.d.advance(1.0);
  CHECK(u.canvas().cableGlow(w) == 0.0f);
  f0 = u.d.framesDrawn();
  u.d.advance(0.5);
  CHECK(u.d.framesDrawn() == f0);
  CHECK(u.canvas().cableGlowPolling());
  // Stopped: one more poll, then nothing at all.
  meters.setLive(false);
  u.d.advance(0.5);
  CHECK_FALSE(u.canvas().cableGlowPolling());
  const uint64_t p1 = u.canvas().stats().cableGlowPolls;
  f0 = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.canvas().stats().cableGlowPolls == p1);
  CHECK(u.d.framesDrawn() == f0);
  CHECK(std::isinf(u.d.tree().nextWakeTime()));
  u.view().setMeterSource(nullptr);
  // A wire into a param glows with its sweep (the meta patch's VCA onto the accumulator's frequency).
  Ui m(fixture());
  FixedModulationSource sweeps;
  m.ctx.setModulationSource(&sweeps);
  uint32_t mw = kNone;
  for (uint32_t k = 0; k < m.pp().edges.size(); ++k) {
    const Edge& e = m.pp().edges[k];
    if (e.kind != EdgeKind::Modulation || !m.pp().nodes[e.to].inputs[e.toPort].param) continue;
    const doc::Wire* dw = doc::findWire(*m.ctx.doc(), e.wire);
    REQUIRE(dw != nullptr);
    sweeps.set(dw->to, dw->param, ModSweep{true, 0.3f, 0.1f});
    mw = m.wireOf(k);
  }
  REQUIRE(mw != kNone);
  m.view().fitAll(false);
  m.d.advance(0.3);
  m.canvas().wakeCables();
  CHECK_FALSE(m.canvas().cableGlowPolling());  // not live: nothing
  sweeps.setLive(true);
  m.canvas().wakeCables();
  m.d.advance(0.2);
  CHECK(m.canvas().cableGlow(mw) > 0.5f);
  sweeps.setLive(false);
  m.d.advance(1.0);
  CHECK(m.canvas().cableGlow(mw) == 0.0f);
  CHECK_FALSE(m.canvas().cableGlowPolling());
  m.ctx.setModulationSource(nullptr);
}

TEST_CASE("cables: far out they are the rest shapes in the chunks: no motion, no glow") {
  Ui u;
  REQUIRE(u.d.enableGpu().ok());
  circuit::CircuitCanvas::View v = u.canvas().view();
  v.zoom = 0.1;
  u.canvas().setView(v);
  u.d.advance(0.5);
  REQUIRE(u.canvas().lod() == circuit::CircuitCanvas::Lod::Far);
  const uint32_t w = u.longestCable();
  REQUIRE(w != kNone);
  const circuit::CircuitScene& sc = u.scene();
  const uint32_t src = sc.pins[sc.wires[w].fromPin].node;
  const uint32_t nodes[1] = {src};
  u.canvas().setNodeDrag(nodes, {40.0f, 40.0f});
  CHECK_FALSE(u.canvas().cablesMoving());  // landed at once
  u.canvas().clearNodeDrag();
  CHECK_FALSE(u.canvas().cablesMoving());
  CHECK(u.canvas().stats().cablesPainted == 0);
  u.d.advance(0.5);
  CHECK_FALSE(u.d.tree().needsFrame());
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

// The user's meta patch (bug 2026-11-02_18-21-54) at rest: near (the band framed), far (zoom 0.2), and playing (lit).
TEST_CASE("render: project-cables matches the golden image") { golden("project-cables"); }
TEST_CASE("render: project-cables-far matches the golden image") { golden("project-cables-far"); }
TEST_CASE("render: project-cables-glow matches the golden image") { golden("project-cables-glow"); }
