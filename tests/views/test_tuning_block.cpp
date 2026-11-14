// BR.tuning-block: every MIDI band's tuning block in the project patch view — one per MIDI band in the full patch (none
// in a scoped model), BR.tuning-outside: hitched outside its band's left edge (the chain where it is without one), the
// lane's notes drawn into it out of the lane's left end and one cable of the same wire on to the instrument
// (checkAgainstGraph still passes), opened in place by a double-click (real dispatch; saved with the project) growing
// leftward, its Wheel button growing it further left to the full wheel with the circle where it was (real dispatch;
// saved; the chart's click as one undo step), the circle's hardness drag as one undo step on the track's tuning, Z on
// its out cable inserting on the lane's link, the track's own notes lighting it (not another track's on the same
// tuning). Goldens: project-patch-tuning, project-patch-tuning-wheel.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/serialize.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mos_wheel/mos_chart.h"
#include "pq/views/mos_wheel/mos_circle.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../../src/views/src/project_patch/internal.h"
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

const doc::Track* named(const doc::Project& p, std::string_view name) {
  const doc::Track* found = nullptr;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (!found && t.name == name) found = &t;
  });
  REQUIRE_MESSAGE(found != nullptr, "no track " << std::string(name));
  return found;
}

// The shell over the sketch project (Keys, Pad, Mod: MIDI; Space: a return), the patch view shown without the morph.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;

  Ui() {
    ctx.setDeviceLibrary(&library());
    const pq::Status built = buildSketchProject(ctx.store());
    REQUIRE_MESSAGE(built.ok(), (built.ok() ? std::string() : built.error().message));
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    REQUIRE(d.enableGpu().ok());  // the canvas paints (and lays out an opened block's face) with the theme and fonts
    d.tree().layoutNow();
    view().setShown(true, false);
    d.tree().layoutNow();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const ProjectPatch& pp() const { return view().patch(); }
  doc::TrackId track(std::string_view name) const { return named(*ctx.doc(), name)->id; }
  const Band& band(std::string_view name) const {
    const uint32_t b = pp().bandOf(track(name));
    REQUIRE(b != kNone);
    return pp().bands[b];
  }
  size_t history() const { return ctx.store().history().cursor(); }
  Vec2 dev(float x, float y) const { return view().canvas().toDevice({x, y}); }
  // The band's left end framed at `zoom` (its tuning block outside it, its first blocks).
  void frame(std::string_view name, double zoom = 1.0) {
    const Band& b = band(name);
    circuit::CircuitCanvas::View cv;
    cv.zoom = zoom;
    cv.x = static_cast<double>(b.tuning != kNone ? pp().nodes[b.tuning].x : b.x0) - 30.0;
    cv.y = static_cast<double>(b.y) - 30.0;
    view().canvas().setView(cv);
    d.tree().layoutNow();
    d.step();
  }
  Face* tuningFace(std::string_view name) const {
    const uint32_t tn = band(name).tuning;
    REQUIRE(tn != kNone);
    const uint32_t sn = tn < view().scene().sceneNode.size() ? view().scene().sceneNode[tn] : kNone;
    REQUIRE(sn != kNone);
    return view().canvas().panelOn(sn);
  }
  // The edges of `name`'s notes link: the lane's (drawn into the tuning block) and the cable on from the block.
  struct Link {
    uint32_t lane = kNone, out = kNone;
  };
  Link notesLink(std::string_view name) const {
    const Band& b = band(name);
    Link l;
    for (uint32_t i = 0; i < pp().edges.size(); ++i) {
      const Edge& e = pp().edges[i];
      if (e.kind != EdgeKind::Notes) continue;
      if (e.from == b.source && e.fromPort == 0) l.lane = i;
      if (e.from == b.tuning) l.out = i;
    }
    return l;
  }
  // A point on a cable's rest shape that the canvas hits as that edge.
  Vec2 onCable(uint32_t edge) const {
    const circuit::CircuitScene& sc = *view().canvas().scene();
    for (uint32_t w = 0; w < sc.wires.size(); ++w) {
      if (w >= view().scene().modelEdge.size() || view().scene().modelEdge[w] != edge) continue;
      const circuit::SceneWire& sw = sc.wires[w];
      for (uint32_t i = sw.pointCount / 3; i < sw.pointCount; ++i) {
        const graph::Point2 q = sc.points[sw.firstPoint + i];
        const Vec2 p = dev(q.x, q.y);
        if (view().canvas().edgeAt(p) == edge && view().canvas().rect().contains(p)) return p;
      }
    }
    FAIL("no point on the cable");
    return {};
  }
};

}  // namespace

TEST_CASE("tuning block (BR.tuning-block): one per MIDI band in the full patch, the notes drawn through it, the graph matches") {
  Ui u;
  const ProjectPatch& pp = u.pp();
  CHECK(checkAgainstGraph(pp, *u.ctx.doc(), *u.view().lowered()) == "");
  uint32_t blocks = 0;
  for (const Node& n : pp.nodes) blocks += n.kind == NodeKind::Tuning ? 1u : 0u;
  uint32_t midi = 0;
  for (const Band& b : pp.bands) {
    if (b.kind != doc::TrackKind::Midi) {
      CHECK(b.tuning == kNone);
      continue;
    }
    ++midi;
    REQUIRE(b.tuning != kNone);
    const Node& t = pp.nodes[b.tuning];
    CHECK(t.kind == NodeKind::Tuning);
    CHECK(t.band == static_cast<uint32_t>(&b - pp.bands.data()));
    // Closed by default: the narrow panel hitched OUTSIDE the band — its right edge kTuningGap left of the bands' left
    // edge (every depth: one column), its output on the spine, above the band's lane.
    CHECK_FALSE(t.expanded);
    CHECK(t.w == metrics::kTuningW);
    CHECK(t.h == metrics::kTuningH);
    CHECK(t.x + t.w == doctest::Approx(metrics::kLeft - metrics::kTuningGap));
    CHECK(t.x + t.w < b.x0);
    CHECK(t.y >= b.y + metrics::kTitle - 0.01f);
    CHECK(t.y + t.h < b.laneY);
    // The band's chain starts at its own left edge, as without the block.
    for (const uint32_t d : b.devices)
      if (!pp.nodes[d].offPath) CHECK(pp.nodes[d].x >= b.x0);
  }
  CHECK(midi >= 3);
  CHECK(blocks == midi);
  // 12-EDO by default (a new project's tuning; the sketch's tracks inherit it).
  const doc::TuningDef* def = targetTuning(*u.ctx.doc(), TuningTarget::forTrack(u.track("Keys")));
  REQUIRE(def != nullptr);
  REQUIRE(def->edo() != nullptr);
  CHECK(def->edo()->n() == 12u);

  // Keys: the lane's notes link drawn into the block (a cable out of the lane's left end, across the band's edge, up
  // into its bottom) and one cable of the same wire on from its output, back across the edge, into where the link goes
  // (the instrument's notes).
  const Band& b = u.band("Keys");
  const Ui::Link l = u.notesLink("Keys");
  REQUIRE(l.lane != kNone);
  REQUIRE(l.out != kNone);
  const Edge& lane = pp.edges[l.lane];
  const Edge& out = pp.edges[l.out];
  const Node& t = pp.nodes[b.tuning];
  CHECK(lane.drawnTo == b.tuning);
  CHECK(lane.cable);
  REQUIRE(lane.points.size() == 2);
  const Node& ln = pp.nodes[b.source];
  CHECK(lane.points.front().x == doctest::Approx(ln.x));
  CHECK(lane.points.front().y > ln.y);
  CHECK(lane.points.front().y < ln.y + ln.h);
  CHECK(lane.points.back().x == doctest::Approx(inputEntry(t, 0).x));
  CHECK(lane.points.back().y == doctest::Approx(t.y + t.h));
  CHECK(lane.points.back().x < b.x0);  // outside the band
  CHECK(out.kind == EdgeKind::Notes);
  CHECK(out.cable);
  CHECK(out.wire == lane.wire);
  CHECK(out.wire.valid());
  CHECK(out.status == lane.status);
  CHECK(out.to == lane.to);
  CHECK(out.toPort == lane.toPort);
  CHECK(out.drawnTo == kNone);
  REQUIRE(out.points.size() == 2);
  CHECK(out.points.front().x == doctest::Approx(t.x + t.w));
  CHECK(out.points.front().y == doctest::Approx(t.y + metrics::kPinTop));
  CHECK(out.points.back().x > b.x0);  // into the band
  CHECK(pp.nodes[out.to].kind == NodeKind::Device);
  // The other lane outputs (Hz, gate, vel) are not drawn through it.
  for (const Edge& e : pp.edges)
    if (e.from == b.source && e.fromPort != 0) CHECK(e.drawnTo == kNone);
  // The scene: the lane's cable plugs into the block's input socket, the block's output into the instrument.
  const circuit::CircuitScene& sc = *u.view().canvas().scene();
  const uint32_t sn = u.view().scene().sceneNode[b.tuning];
  REQUIRE(sn != kNone);
  bool laneSeen = false;
  for (uint32_t w = 0; w < sc.wires.size(); ++w)
    if (w < u.view().scene().modelEdge.size() && u.view().scene().modelEdge[w] == l.lane) {
      laneSeen = true;
      CHECK(sc.wires[w].toNode == sn);
      CHECK(sc.wires[w].toPin == sc.nodes[sn].firstPin);
      CHECK(sc.pins[sc.nodes[sn].firstPin].connected);
    }
  CHECK(laneSeen);
  // It is drawn as its face (closed).
  Face* f = u.tuningFace("Keys");
  REQUIRE(f != nullptr);
  CHECK(f->worldSize().x == metrics::kTuningW);
  CHECK(tuningFaceCircle(*f) == nullptr);

  // Not in a scoped model (a group's panel, the free rack): no lanes, no tuning blocks.
  LayoutOptions o;
  o.freeTrack = u.track("Mod");
  const ProjectPatch free = buildProjectPatch(*u.ctx.doc(), *u.view().lowered(), &library(), o);
  CHECK(free.scoped);
  for (const Node& n : free.nodes) CHECK(n.kind != NodeKind::Tuning);
  for (const Band& fb : free.bands) CHECK(fb.tuning == kNone);
}

TEST_CASE("tuning block: a double-click opens it in place (saved with the project), another on its header closes it") {
  Ui u;
  const doc::TrackId keys = u.track("Keys");
  std::vector<float> chainX;  // the band's spine blocks while it is closed
  for (const uint32_t d : u.band("Keys").devices) chainX.push_back(u.pp().nodes[d].x);
  const float right = metrics::kLeft - metrics::kTuningGap;
  u.frame("Keys");
  {
    const Node& t = u.pp().nodes[u.band("Keys").tuning];
    u.d.doubleClick(u.dev(t.x + t.w * 0.5f, t.y + t.h * 0.5f));
  }
  u.d.step();
  CHECK(u.view().tuningOpen(keys));
  {
    const auto& nudges = u.ctx.store().view().patchNudges;
    const auto it = nudges.find(keys.value);
    REQUIRE(it != nudges.end());
    CHECK(it->second.tuningOpen);
  }
  const Band& b = u.band("Keys");
  const Node& t = u.pp().nodes[b.tuning];
  CHECK(t.expanded);
  CHECK(t.w == metrics::kTuningOpenW);
  CHECK(t.h == metrics::kTuningOpenH);
  // It grew leftward (its right edge where it was, outside the band); the chain did not move; the band holds it above
  // its lane.
  CHECK(t.x + t.w == doctest::Approx(right));
  REQUIRE(b.devices.size() == chainX.size());
  for (size_t i = 0; i < chainX.size(); ++i) CHECK(u.pp().nodes[b.devices[i]].x == doctest::Approx(chainX[i]));
  CHECK(t.y + t.h < b.laneY);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // The opened face hosts the circle: 12 equal degrees (12-EDO shown as 7L 5s at its equalized end).
  u.d.step();
  Face* f = u.tuningFace("Keys");
  REQUIRE(f != nullptr);
  MosCircle* c = tuningFaceCircle(*f);
  REQUIRE(c != nullptr);
  CHECK(c->degreeCount() == 12);
  CHECK(c->hardness() == doctest::Approx(1.0));
  // Saved with the project's view state, read back.
  {
    auto text = doc::serializeProject(*u.ctx.doc(), u.ctx.store().view());
    REQUIRE(text.ok());
    CHECK(text->find("\"tuningOpen\"") != std::string::npos);
    auto back = doc::parseProject(*text);
    REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
    const auto it = back->view.patchNudges.find(keys.value);
    REQUIRE(it != back->view.patchNudges.end());
    CHECK(it->second.tuningOpen);
  }
  // A double-click on its header strip (right of the TUNING label, left of its controls: the name) closes it.
  u.frame("Keys");
  {
    const Node& o = u.pp().nodes[u.band("Keys").tuning];
    u.d.doubleClick(u.dev(o.x + 120.0f, o.y + metrics::kTuningHeaderH * 0.5f));
  }
  u.d.step();
  CHECK_FALSE(u.view().tuningOpen(keys));
  CHECK(u.pp().nodes[u.band("Keys").tuning].w == metrics::kTuningW);
  CHECK(u.ctx.store().view().patchNudges.count(keys.value) == 0);  // nothing else stored: the entry goes
}

TEST_CASE("tuning block: its Wheel button grows it leftward to the full wheel, the circle where it was (saved), and back") {
  Ui u;
  const doc::TrackId keys = u.track("Keys");
  u.view().setTuningOpen(keys, true);
  u.frame("Keys");
  u.d.step();
  // The circle's world rect, and the Wheel button's centre on the canvas (the face's own geometry).
  auto circleWorld = [&](Face& f) {
    const Node& t = u.pp().nodes[u.band("Keys").tuning];
    MosCircle* c = tuningFaceCircle(f);
    REQUIRE(c != nullptr);
    const float k = u.view().canvas().deviceScale();
    const IRect r = c->rect();
    return Rect{t.x + static_cast<float>(r.x) / k, t.y + static_cast<float>(r.y) / k, t.x + static_cast<float>(r.right()) / k,
                t.y + static_cast<float>(r.bottom()) / k};
  };
  auto wheelButton = [&](Face& f) {
    const Node& t = u.pp().nodes[u.band("Keys").tuning];
    const std::optional<Rect> r = tuningFaceWheelButton(f);
    REQUIRE(r.has_value());
    return u.dev(t.x + (r->x0 + r->x1) * 0.5f, t.y + (r->y0 + r->y1) * 0.5f);
  };
  Face* f = u.tuningFace("Keys");
  REQUIRE(f != nullptr);
  CHECK(tuningFaceChart(*f) == nullptr);
  const Rect before = circleWorld(*f);
  const float right = metrics::kLeft - metrics::kTuningGap;
  std::vector<float> chainX;
  for (const uint32_t d : u.band("Keys").devices) chainX.push_back(u.pp().nodes[d].x);

  // A click on Wheel (real dispatch, through the canvas): the block grows leftward by the chart's column.
  u.d.click(wheelButton(*f));
  u.d.step();
  CHECK(u.view().tuningWheel(keys));
  CHECK(u.view().tuningOpen(keys));
  {
    const Node& t = u.pp().nodes[u.band("Keys").tuning];
    CHECK(t.expanded);
    CHECK(t.w == metrics::kTuningOpenW + metrics::kTuningWheelW);
    CHECK(t.h == metrics::kTuningOpenH);
    CHECK(t.x + t.w == doctest::Approx(right));
    const Band& b = u.band("Keys");
    REQUIRE(b.devices.size() == chainX.size());
    for (size_t i = 0; i < chainX.size(); ++i) CHECK(u.pp().nodes[b.devices[i]].x == doctest::Approx(chainX[i]));
  }
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  u.frame("Keys");
  u.d.step();
  f = u.tuningFace("Keys");
  REQUIRE(f != nullptr);
  MosChart* chart = tuningFaceChart(*f);
  REQUIRE(chart != nullptr);
  // The circle kept its size and place; the chart shows what it does (12-EDO: the 12-note MOS at its equalized end).
  const Rect after = circleWorld(*f);
  CHECK(after.x0 == doctest::Approx(before.x0).epsilon(0.002));
  CHECK(after.y0 == doctest::Approx(before.y0).epsilon(0.002));
  CHECK(after.x1 == doctest::Approx(before.x1).epsilon(0.002));
  CHECK(after.y1 == doctest::Approx(before.y1).epsilon(0.002));
  MosCircle* c = tuningFaceCircle(*f);
  REQUIRE(c != nullptr);
  CHECK(c->degreeCount() == 12);
  REQUIRE(chart->selection().has_value());
  REQUIRE(c->selection().has_value());
  CHECK(chart->selection()->perPeriodKey == c->selection()->perPeriodKey);
  CHECK(chart->rect().right() <= c->rect().x);  // the chart is left of the circle

  // The selected size's row is scrolled into view; a click in it at 690¢ (real dispatch; the 7L 5s region, no EDO marker
  // there): the track's tuning is that MOS, one undo step, the circle follows.
  {
    const Node& t = u.pp().nodes[u.band("Keys").tuning];
    const Vec2 o = u.dev(t.x, t.y);
    const IRect row = chart->rowRect(12);
    REQUIRE(row.w > 0);
    const size_t h0 = u.history();
    u.d.click({std::round(o.x) + chart->xOfCents(690.0), std::round(o.y) + static_cast<float>(row.y) + static_cast<float>(row.h) * 0.5f});
    u.d.step();
    CHECK(u.history() == h0 + 1);
    const doc::TuningDef* def = targetTuning(*u.ctx.doc(), TuningTarget::forTrack(keys));
    REQUIRE(def != nullptr);
    REQUIRE(def->mos() != nullptr);
    CHECK(def->mos()->n == 12);
    CHECK(def->mos()->generator == doctest::Approx(690.0).epsilon(0.003));
    REQUIRE(c->selection().has_value());
    CHECK(c->selection()->generatorCents == doctest::Approx(690.0).epsilon(0.003));
    // The wheel over the chart zooms the chart, not the canvas.
    const double span = chart->viewMax() - chart->viewMin();
    const circuit::CircuitCanvas::View cv = u.view().canvas().view();
    u.d.wheel({std::round(o.x) + chart->xOfCents(690.0), std::round(o.y) + static_cast<float>(row.y) + static_cast<float>(row.h) * 0.5f}, 1.0f);
    CHECK(chart->viewMax() - chart->viewMin() < span);
    CHECK(u.view().canvas().view().zoom == doctest::Approx(cv.zoom));
  }

  // Saved with the project's view state, read back.
  {
    auto text = doc::serializeProject(*u.ctx.doc(), u.ctx.store().view());
    REQUIRE(text.ok());
    CHECK(text->find("\"tuningWheel\"") != std::string::npos);
    auto back = doc::parseProject(*text);
    REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
    const auto it = back->view.patchNudges.find(keys.value);
    REQUIRE(it != back->view.patchNudges.end());
    CHECK(it->second.tuningOpen);
    CHECK(it->second.tuningWheel);
  }

  // Its button again (now "Circle"): back to the circle alone, where it was.
  u.d.click(wheelButton(*f));
  u.d.step();
  CHECK_FALSE(u.view().tuningWheel(keys));
  CHECK(u.view().tuningOpen(keys));
  CHECK(u.pp().nodes[u.band("Keys").tuning].w == metrics::kTuningOpenW);
  CHECK(u.pp().nodes[u.band("Keys").tuning].x + metrics::kTuningOpenW == doctest::Approx(right));

  // Grown again, then closed by its menu's state call: closing folds the wheel too (nothing stored).
  u.view().setTuningWheel(keys, true);
  CHECK(u.view().tuningWheel(keys));
  u.view().setTuningOpen(keys, false);
  CHECK_FALSE(u.view().tuningWheel(keys));
  CHECK(u.pp().nodes[u.band("Keys").tuning].w == metrics::kTuningW);
  CHECK(u.ctx.store().view().patchNudges.count(keys.value) == 0);
}

TEST_CASE("tuning block: the circle's hardness drag edits the track's tuning live, one undo step; Z on its out cable inserts on the lane's link") {
  Ui u;
  const doc::TrackId keys = u.track("Keys");
  u.view().setTuningOpen(keys, true);
  u.frame("Keys");
  u.d.step();
  Face* f = u.tuningFace("Keys");
  REQUIRE(f != nullptr);
  MosCircle* c = tuningFaceCircle(*f);
  REQUIRE(c != nullptr);
  // The circle's geometry is in the face's panel px: on the canvas at the block's top-left, 1:1 at a settled zoom.
  const Node& t = u.pp().nodes[u.band("Keys").tuning];
  const Vec2 o = u.dev(t.x, t.y);
  auto onCanvas = [&](float px, float py) { return Vec2{std::round(o.x) + px, std::round(o.y) + py}; };
  const MosCircle::Geometry& g = c->geometry();
  const float sy = static_cast<float>(g.slider.y) + static_cast<float>(g.slider.h) * 0.5f;
  const Vec2 from = onCanvas(c->sliderXOfHardness(1.0), sy), to = onCanvas(c->sliderXOfHardness(2.0), sy);
  const size_t h0 = u.history();
  const doc::Snapshot start = u.ctx.doc();
  u.d.down(from);
  u.d.move({(from.x + to.x) * 0.5f, from.y});
  u.d.move(to);
  // Live: the track's tuning follows the drag before the release (sounding notes retune during it).
  const doc::TuningDef* live = targetTuning(*u.ctx.doc(), TuningTarget::forTrack(keys));
  REQUIRE(live != nullptr);
  CHECK(live->mos() != nullptr);
  u.d.up(to);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  const doc::TuningDef* def = targetTuning(*u.ctx.doc(), TuningTarget::forTrack(keys));
  REQUIRE(def != nullptr);
  REQUIRE(def->mos() != nullptr);
  CHECK(def->mos()->n == 12);
  CHECK(c->hardness() > 1.5);
  REQUIRE(u.ctx.undo());
  CHECK(u.ctx.doc() == start);
  u.d.step();
  CHECK(targetTuning(*u.ctx.doc(), TuningTarget::forTrack(keys))->edo() != nullptr);
  CHECK(c->hardness() == doctest::Approx(1.0));  // the undo shows at once

  // Z over the cable out of the block: a VCA at the head of the track's path (what the notes reach first), one step —
  // the cable is the lane's link.
  const Ui::Link l = u.notesLink("Keys");
  REQUIRE(l.out != kNone);
  const doc::DeviceId inst = u.pp().nodes[u.pp().edges[l.out].to].device;
  const size_t h1 = u.history();
  u.d.move(u.onCable(l.out));
  u.d.keyDown('z');
  u.d.keyUp('z');
  u.view().sync();
  CHECK(u.history() == h1 + 1);
  const doc::Track& kt = *named(*u.ctx.doc(), "Keys");
  const doc::MainPath path = doc::mainPath(*u.ctx.doc(), kt);
  REQUIRE(path.blocks.size() >= 2);
  CHECK(path.blocks[0]->name == "VCA");
  CHECK(path.blocks[1]->id == inst);
  CHECK(checkAgainstGraph(u.pp(), *u.ctx.doc(), *u.view().lowered()) == "");
  // The notes still pass through the block, now into the VCA.
  const Ui::Link l2 = u.notesLink("Keys");
  REQUIRE(l2.out != kNone);
  CHECK(u.pp().nodes[u.pp().edges[l2.out].to].device == path.blocks[0]->id);
}

TEST_CASE("tuning block: the track's own notes light its degrees, not another track's on the same tuning") {
  Ui u;
  doc::DocumentStore& st = u.ctx.store();
  const doc::TrackId keys = u.track("Keys"), pad = u.track("Pad");
  auto clipOn = [&](doc::TrackId t) {
    auto e = doc::ops::insertEmptyClip(*st.current(), st.ids(), t, 0, 4 * pq::kPpq, "Lit");
    REQUIRE(e.ok());
    REQUIRE(st.dispatch(e->command).ok());
    REQUIRE_FALSE(e->clips.empty());
    return e->clips.front();
  };
  const doc::ClipRef kc = clipOn(keys), pc = clipOn(pad);
  // Both play in the project's default tuning (one named tuning).
  CHECK(targetTuningId(*u.ctx.doc(), TuningTarget::forTrack(keys)) == targetTuningId(*u.ctx.doc(), TuningTarget::forTrack(pad)));
  u.view().setTuningOpen(keys, true);
  u.frame("Keys");
  u.d.step();
  Face* f = u.tuningFace("Keys");
  REQUIRE(f != nullptr);
  MosCircle* c = tuningFaceCircle(*f);
  REQUIRE(c != nullptr);
  // Pad's audition (degree 4): nothing lights on Keys's block.
  u.ctx.audition(329.63f, 100.0f, 400.0f, pc, 4);
  u.d.advance(0.1);
  CHECK(c->glowOf(4) == doctest::Approx(0.0f));
  // Keys's own (degree 7): lit.
  u.ctx.audition(392.0f, 100.0f, 400.0f, kc, 7);
  u.d.advance(0.1);
  CHECK(c->glowOf(7) > 0.5f);
  CHECK(c->glowOf(4) == doctest::Approx(0.0f));
  // Released and settled: no frames while nothing sounds.
  u.ctx.releaseAuditions();
  u.d.advance(3.0);
  CHECK(c->glowOf(7) == doctest::Approx(0.0f));
  CHECK_FALSE(c->animating());
}

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

// Keys's block opened (12-EDO, C E G lit), the band under it closed.
TEST_CASE("render: project-patch-tuning matches the golden image") { golden("project-patch-tuning"); }
// Keys's block grown to the full wheel (the region chart left of the circle).
TEST_CASE("render: project-patch-tuning-wheel matches the golden image") { golden("project-patch-tuning-wheel"); }
