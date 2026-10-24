// T3.15 automation lanes: lanes shown / hidden under tracks, the parameter chooser (mixer and main-tuning
// parameters), automation mode (A) over the clips; breakpoint add (snapped, on the line), move (relative snap, value
// readout, clamped between neighbours), delete (double-click, Delete), multi-select (Shift / Ctrl, time selection),
// Alt-drag curvature, shape insertion over a time selection; each gesture exactly one undo step whose ChangeSet is not
// Structural (Automation-only), a 60-frame drag one entry; the first edit of an unautomated parameter creates its lane
// in the same step; automated / overridden state and "Re-enable Automation"; drag frames damage only the affected
// columns of the band and partial repaints equal full ones; the view's envelope evaluation equals
// pq::evalAutomation within 1e-6 at 10^4 points; HiDPI; the goldens `automation-lanes` and
// `automation-mode-over-clips`.

#include <doctest/doctest.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "pq/core/automation_curve.h"
#include "pq/document/automation_curve.h"
#include "pq/document/serialize.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/overlay.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/arrangement/automation.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
#include "automation_fixture.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr Ticks kBar = kTicksPerWhole;  // the fixture is 4/4 throughout
constexpr const char* kArr = "main/body/top/track-area/arrangement";

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d;
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  explicit Fixture(float scale = 1.0f, uint32_t w = 1280, uint32_t h = 800) : d(w, h, scale) {
    REQUIRE(ctx.openProject(views::test::fixtureDir("automation")).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    ctx.setDetailVisible(false);
    // A fixed quarter-note grid (deterministic snapping) and 120 logical px per bar.
    GridSpec g;
    g.mode = GridMode::Fixed;
    g.fixedExp = 2;
    ctx.setGrid(g);
    d.tree().layoutNow();
    arr->setTimeView(0.0, 120.0 / static_cast<double>(kBar));
    d.tree().layoutNow();
    d.step();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }

  ArrangementAutomation& au() const { return arr->automation(); }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::ChangeSet& lastChanges() const { return ctx.store().history().entries()[history() - 1].changes; }
  const doc::Track& track(std::string_view name) const {
    const doc::Track* found = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!found && t.name == name) found = &t;
    });
    REQUIRE_MESSAGE(found != nullptr, name);
    return *found;
  }
  doc::ParamRef vol(std::string_view name) const { return doc::ParamRef::track(doc::ParamKind::TrackVolume, track(name).id); }
  doc::ParamRef pan(std::string_view name) const { return doc::ParamRef::track(doc::ParamKind::TrackPan, track(name).id); }
  const doc::AutomationLane* lane(const doc::ParamRef& r) const { return findAutomationLane(*ctx.doc(), r).lane; }
  std::vector<doc::Breakpoint> points(const doc::ParamRef& r) const {
    const doc::AutomationLane* l = lane(r);
    return l ? l->points : std::vector<doc::Breakpoint>{};
  }
  AutomationBand band(std::string_view name, const doc::ParamRef& r, bool overClips = false) {
    d.tree().layoutNow();
    const auto b = au().band(track(name).id, r, overClips);
    REQUIRE(b.has_value());
    return *b;
  }
  float x(double bars) const { return arr->xOfTick(bars * static_cast<double>(kBar)); }
  Vec2 onLine(const AutomationBand& b, double bars) {
    const double t = bars * static_cast<double>(kBar);
    const auto pts = points(b.param);
    const double v = pts.empty() ? currentParamValue(*ctx.doc(), b.param) : pq::evalAutomation(std::span<const doc::Breakpoint>(pts), t);
    return {arr->xOfTick(t), au().yOfValue(b, v)};
  }
  Vec2 point(const AutomationBand& b, double bars, size_t nth = 0) {
    for (const doc::Breakpoint& p : points(b.param))
      if (p.tick == static_cast<Ticks>(bars * static_cast<double>(kBar)) && nth-- == 0) return *au().pointPos(b, p.id);
    FAIL("no breakpoint at that bar");
    return {};
  }
  int32_t row(std::string_view name) const {
    const auto rows = arr->rows();
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].track->name == name) return static_cast<int32_t>(i);
    return -1;
  }
};

std::string readText(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

double tol(double v) { return 1e-6 * std::max(1.0, std::abs(v)); }

}  // namespace

TEST_CASE("automation: the fixture project is what buildAutomationProject makes") {
  doc::DocumentStore store;
  views::test::buildAutomationProject(store);
  auto text = doc::serializeProject(*store.current(), store.view());
  REQUIRE(text.ok());
  const std::string dir = views::test::fixtureDir("automation");
  if (const char* u = std::getenv("PQ_UPDATE_FIXTURES"); u && *u == '1') REQUIRE(doc::saveProject(dir, store).ok());
  std::string file = readText(dir + "/project.pqd");
  std::erase(file, '\r');
  CHECK_MESSAGE(file == *text, "tests/views/fixtures/automation is stale: run pq_tests_views with PQ_UPDATE_FIXTURES=1");
}

TEST_CASE("automation: the view's envelope evaluation equals evalAutomation within 1e-6 at 10^4 points") {
  std::mt19937 rng(1234);
  std::uniform_real_distribution<double> val(-70.0, 6.0), curve(-1.0, 1.0), unit(0.0, 1.0);
  std::vector<doc::Breakpoint> pts;
  Ticks t = 0;
  for (uint32_t i = 0; i < 1500; ++i) {
    if (unit(rng) > 0.08) t += 1 + static_cast<Ticks>(unit(rng) * static_cast<double>(kBar) / 4.0);  // else a jump (same tick)
    const float c = unit(rng) < 0.3 ? 0.0f : static_cast<float>(curve(rng));
    pts.push_back({t, static_cast<float>(val(rng)), c, doc::BreakpointId{i + 1}});
  }
  const std::span<const doc::Breakpoint> sp(pts);
  // 10^4 increasing query ticks (fractional, exact breakpoint ticks, before the first and after the last point).
  std::vector<double> qs;
  for (int i = 0; i < 10000; ++i) qs.push_back(-500.0 + (static_cast<double>(t) + 1000.0) * unit(rng));
  for (size_t i = 0; i < pts.size(); i += 7) qs[i] = static_cast<double>(pts[i].tick);
  std::sort(qs.begin(), qs.end());
  EnvelopeCursor cur(sp, -3.0);
  double worst = 0.0;
  for (double q : qs) {
    const double a = cur.at(q), b = pq::evalAutomation(sp, q, -3.0);
    worst = std::max(worst, std::abs(a - b) / std::max(1.0, std::abs(b)));
    REQUIRE(std::abs(a - b) <= tol(b));
  }
  MESSAGE("cursor: worst relative error " << worst);

  // The drawn polyline: every vertex on the envelope (a jump's first vertex is the value arriving at it).
  std::vector<EnvelopeVertex> v;
  size_t checked = 0;
  for (const auto& [t0, t1, chord] : {std::tuple{0.0, static_cast<double>(t), 50.0}, std::tuple{1234.5, 777777.25, 3.0},
                                      std::tuple{-100.0, 20.0, 1.0}, std::tuple{static_cast<double>(t) - 5000.0, static_cast<double>(t) + 9000.0, 10.0}}) {
    envelopeVertices(sp, -3.0, t0, t1, chord, v);
    REQUIRE(v.size() >= 2);
    CHECK(v.front().tick == t0);
    CHECK(v.back().tick == t1);
    for (size_t i = 0; i < v.size(); ++i) {
      CHECK(v[i].tick >= t0);
      CHECK(v[i].tick <= t1);
      if (i > 0) CHECK(v[i].tick >= v[i - 1].tick);
      const bool sameAsNext = i + 1 < v.size() && v[i + 1].tick == v[i].tick;
      const bool sameAsPrev = i > 0 && v[i - 1].tick == v[i].tick;
      if (sameAsNext && sameAsPrev) continue;  // the middle of a three-point jump
      const auto ti = static_cast<Ticks>(v[i].tick);
      const double want = (sameAsNext && static_cast<double>(ti) == v[i].tick) ? doc::automationValueBefore(sp, ti, -3.0)
                                                                                : pq::evalAutomation(sp, v[i].tick, -3.0);
      REQUIRE(std::abs(v[i].value - want) <= tol(want));
      ++checked;
    }
  }
  CHECK(checked > 2000);
}

TEST_CASE("automation: lanes show and hide under a track; the chooser offers mixer and main-tuning parameters") {
  Fixture f;
  ArrangementAutomation& au = f.au();
  const doc::Track& lead = f.track("Lead");
  auto contentH = [&] {
    const ArrangementRow& last = f.arr->rows().back();
    return last.y + last.h + last.lanesH;
  };
  const int32_t before = contentH();
  CHECK(f.arr->laneRows().empty());
  au.showLanes(lead.id, true);
  f.d.tree().layoutNow();
  // Its automated parameters, in the document's order: volume, send A, activator.
  const auto shown = au.shownLanes(lead.id);
  REQUIRE(shown.size() == 3);
  CHECK(shown[0].kind == doc::ParamKind::TrackVolume);
  CHECK(shown[1].kind == doc::ParamKind::TrackSend);
  CHECK(shown[2].kind == doc::ParamKind::TrackMute);
  REQUIRE(f.arr->laneRows().size() == 3);
  CHECK(contentH() > before);
  const auto& lr = f.arr->laneRows()[0];
  const ArrangementRow& row = f.arr->rows()[static_cast<size_t>(f.row("Lead"))];
  CHECK(lr.y == row.y + row.h);  // right under the track
  CHECK(row.laneCount == 3);
  CHECK(row.lanesH == f.arr->laneRows()[2].y + f.arr->laneRows()[2].h - lr.y);
  CHECK(f.d.find(std::string(kArr) + "/tracks/lane[2]") != nullptr);
  // Hide the send lane (LaneView::visible off), add one back with +, hide all.
  const size_t h0 = f.history();
  au.hideLane(lead.id, 1);
  CHECK(au.shownLanes(lead.id).size() == 2);
  CHECK_FALSE(f.ctx.store().view().lanes.at(f.lane(doc::ParamRef::track(doc::ParamKind::TrackSend, lead.id, f.track("A Reverb").id))->id).visible);
  CHECK(au.addLane(lead.id));
  CHECK(au.shownLanes(lead.id).size() == 3);
  // The header's minus button hides its lane (at the next frame: the click's own widget goes away).
  {
    f.d.tree().layoutNow();
    const IRect hr = f.d.rect(std::string(kArr) + "/tracks/lane[2]");
    f.d.click({static_cast<float>(hr.right() - 14), static_cast<float>(hr.y + 10)});
    f.d.step();
    CHECK(au.shownLanes(lead.id).size() == 2);
    CHECK(au.addLane(lead.id));
  }
  au.showLanes(lead.id, false);
  CHECK(f.arr->laneRows().empty());
  CHECK(contentH() == before);
  CHECK(f.history() == h0);  // view state: no undo entries

  // The chooser: mixer parameters, then the main tunings' (5L 2s: reference, generator, period, stretch; 12-EDO:
  // reference, equave). The generator is automated on Piano: "elsewhere" for Drums.
  const auto choices = paramChoices(*f.ctx.doc(), f.track("Drums"));
  auto find = [&](doc::ParamKind k, std::string_view group) -> const ParamChoice* {
    for (const ParamChoice& c : choices)
      if (c.param.kind == k && c.group == group) return &c;
    return nullptr;
  };
  REQUIRE(find(doc::ParamKind::TrackVolume, "Mixer") != nullptr);
  CHECK(find(doc::ParamKind::TrackVolume, "Mixer")->automated);
  REQUIRE(find(doc::ParamKind::TrackSend, "Mixer") != nullptr);
  REQUIRE(find(doc::ParamKind::MosGenerator, "5L 2s") != nullptr);
  CHECK(find(doc::ParamKind::MosGenerator, "5L 2s")->elsewhere);
  CHECK(find(doc::ParamKind::TuningEquave, "12-EDO") != nullptr);
  CHECK(find(doc::ParamKind::MosStretch, "5L 2s") != nullptr);
  CHECK(paramName(*f.ctx.doc(), find(doc::ParamKind::MosGenerator, "5L 2s")->param) == "5L 2s: Generator");

  // A lane shows another parameter through the chooser; a tuning parameter automated on Piano shows its envelope
  // under Drums too (one lane per parameter in the project).
  const doc::Track& drums = f.track("Drums");
  au.showLanes(drums.id, true);
  au.setLaneParam(drums.id, 0, find(doc::ParamKind::MosGenerator, "5L 2s")->param);
  f.d.tree().layoutNow();
  REQUIRE(f.arr->laneRows().size() == 1);
  CHECK(f.arr->laneRows()[0].lane != nullptr);
  CHECK(f.arr->laneRows()[0].owner == f.track("Piano").id);
  // The lane's header opens the chooser menu.
  auto* header = f.d.find(std::string(kArr) + "/tracks/lane[0]");
  REQUIRE(header != nullptr);
  const IRect hr = f.d.rect(std::string(kArr) + "/tracks/lane[0]");
  f.d.click({static_cast<float>(hr.x + 40), static_cast<float>(hr.y + 10)});  // the chooser (left of + / −)
  CHECK(OverlayLayer::of(*f.arr)->openCount() > 0);
  f.d.press("Escape");
}

TEST_CASE("automation: a click on the line adds a snapped breakpoint on the envelope: one undo step, not Structural") {
  Fixture f;
  const doc::Track& drums = f.track("Drums");
  f.au().showLanes(drums.id, true);
  const AutomationBand b = f.band("Drums", f.vol("Drums"));
  REQUIRE(b.lane != nullptr);
  const auto before = f.points(b.param);
  const double expected = pq::evalAutomation(std::span<const doc::Breakpoint>(before), static_cast<double>(kBar));
  const size_t h0 = f.history();
  f.d.click(f.onLine(b, 1.03));  // snaps to the quarter grid: bar 1 (1.03 bars = 4.12 beats)
  REQUIRE(f.history() == h0 + 1);
  CHECK(f.lastChanges().automationOnly());
  CHECK_FALSE(f.lastChanges().structural());
  const auto after = f.points(b.param);
  REQUIRE(after.size() == before.size() + 1);
  const auto it = std::find_if(after.begin(), after.end(), [](const doc::Breakpoint& p) { return p.tick == kBar; });
  REQUIRE(it != after.end());
  CHECK(it->value == doctest::Approx(expected).epsilon(1e-6));
  CHECK(f.au().selectedPoints().size() == 1);
  CHECK(f.au().selectedPoints()[0] == it->id);
  // The envelope did not change shape (the point sits on it).
  for (double bars : {0.5, 1.5, 3.0})
    CHECK(pq::evalAutomation(std::span<const doc::Breakpoint>(after), bars * kBar) ==
          doctest::Approx(pq::evalAutomation(std::span<const doc::Breakpoint>(before), bars * kBar)).epsilon(1e-5));
  REQUIRE(f.ctx.undo());
  CHECK(f.points(b.param).size() == before.size());
}

TEST_CASE("automation: dragging a breakpoint moves it (relative snap, value readout) as one undo step; neighbours clamp it") {
  Fixture f;
  f.au().showLanes(f.track("Drums").id, true);
  const AutomationBand b = f.band("Drums", f.vol("Drums"));
  const auto before = f.points(b.param);
  const Vec2 p = f.point(b, 4.0);  // -3 dB, between bar 2 and the jump at bar 6
  const size_t h0 = f.history();
  // A 60-frame drag: one bar later, 10 px up.
  f.d.move(p);
  f.d.down(p);
  const Vec2 to{p.x + 120.0f, p.y - 10.0f};
  for (int i = 1; i <= 60; ++i) {
    const float k = static_cast<float>(i) / 60.0f;
    f.d.move({p.x + (to.x - p.x) * k, p.y + (to.y - p.y) * k});
    if (i == 30) {
      CHECK(f.au().dragging());
      CHECK_FALSE(f.au().readout().empty());
      CHECK(f.history() == h0);  // nothing dispatched while dragging
    }
  }
  CHECK(f.au().readout().find("dB") != std::string::npos);
  f.d.up(to);
  REQUIRE(f.history() == h0 + 1);
  CHECK(f.lastChanges().automationOnly());
  const auto after = f.points(b.param);
  REQUIRE(after.size() == before.size());
  const doc::Breakpoint& moved = *std::find_if(after.begin(), after.end(), [&](const doc::Breakpoint& q) { return q.id == before[2].id; });
  CHECK(moved.tick == 5 * kBar);
  const ParamDisplay disp = paramDisplay(*f.ctx.doc(), b.param);
  const double want = displayValue(disp, displayNorm(disp, -3.0) + 10.0 / static_cast<double>(b.bottom - b.top));
  CHECK(moved.value == doctest::Approx(want).epsilon(1e-4));
  CHECK(moved.curve == before[2].curve);
  // Past its right neighbour (the jump at bar 6): clamped onto it.
  const Vec2 q = f.point(f.band("Drums", f.vol("Drums")), 5.0);
  f.d.drag(q, {q.x + 400.0f, q.y}, 8);
  const auto clamped = f.points(b.param);
  CHECK(std::find_if(clamped.begin(), clamped.end(), [&](const doc::Breakpoint& x) { return x.id == before[2].id; })->tick == 6 * kBar);
  CHECK(f.history() == h0 + 2);
  // Esc during a drag cancels it: no entry.
  const Vec2 r = f.point(f.band("Drums", f.vol("Drums")), 2.0);
  f.d.move(r);
  f.d.down(r);
  f.d.move({r.x + 50.0f, r.y + 5.0f});
  f.d.move({r.x + 60.0f, r.y + 6.0f});
  f.d.press("Escape");
  CHECK_FALSE(f.au().dragging());
  f.d.up({r.x + 60.0f, r.y + 6.0f});
  CHECK(f.history() == h0 + 2);
}

TEST_CASE("automation: double-click deletes, Shift-click and a time selection multi-select, Delete removes the selection") {
  Fixture f;
  f.au().showLanes(f.track("Drums").id, true);
  AutomationBand b = f.band("Drums", f.vol("Drums"));
  const size_t n0 = f.points(b.param).size();
  const size_t h0 = f.history();
  f.d.doubleClick(f.point(b, 8.0));
  CHECK(f.points(b.param).size() == n0 - 1);
  CHECK(f.history() == h0 + 1);
  CHECK(f.lastChanges().automationOnly());

  // Shift-click: two points.
  b = f.band("Drums", f.vol("Drums"));
  f.d.click(f.point(b, 2.0));
  f.d.click(f.point(b, 4.0), mod::kShift);
  CHECK(f.au().selectedPoints().size() == 2);
  f.d.click(f.point(b, 4.0), mod::kShift);  // toggles it off
  CHECK(f.au().selectedPoints().size() == 1);

  // A drag off the line makes a time selection that selects the points inside it (bars 1.5 … 4.5: 2 and 4).
  const float yFar = std::abs(f.onLine(b, 1.5).y - b.bottom) > 10.0f ? b.bottom - 1.0f : b.top + 1.0f;
  f.d.drag({f.x(1.5), yFar}, {f.x(4.5), yFar}, 6);
  REQUIRE(f.au().timeSelection().has_value());
  CHECK(f.au().timeSelection()->start == kBar * 3 / 2);
  CHECK(f.au().timeSelection()->end == kBar * 9 / 2);
  CHECK(f.au().selectedPoints().size() == 2);
  CHECK(f.history() == h0 + 1);  // selecting is not an edit
  f.d.press("Delete");
  CHECK(f.history() == h0 + 2);
  CHECK(f.lastChanges().automationOnly());
  CHECK(f.points(b.param).size() == n0 - 3);
  CHECK(f.au().selectedPoints().empty());
}

TEST_CASE("automation: Alt-drag bends a segment with the shared curve law: one undo step") {
  Fixture f;
  f.au().showLanes(f.track("Drums").id, true);
  const AutomationBand b = f.band("Drums", f.vol("Drums"));
  const auto before = f.points(b.param);
  REQUIRE(before[0].curve == 0.0f);  // 0 dB at bar 0 → −12 dB at bar 2: falling
  const size_t h0 = f.history();
  const Vec2 at{f.x(1.0), (b.top + b.bottom) * 0.5f};
  f.d.drag(at, {at.x, at.y - 60.0f}, 10, mod::kAlt);  // up: bulges upwards = a falling segment starts slowly (> 0)
  REQUIRE(f.history() == h0 + 1);
  CHECK(f.lastChanges().automationOnly());
  auto after = f.points(b.param);
  CHECK(after[0].curve == doctest::Approx(60.0 / static_cast<double>(automation_metrics::kCurveDragPx)).epsilon(1e-4));
  // The drawn envelope uses the law: the value at bar 1 is the curved segment's.
  const double mid = pq::evalAutomation(std::span<const doc::Breakpoint>(after), static_cast<double>(kBar));
  CHECK(mid == doctest::Approx(0.0 + (-12.0 - 0.0) * pq::automationShape(0.5, after[0].curve)));
  CHECK(mid > -6.0);  // above the straight line's −6 dB
  // A rising segment (bar 2 → 4, curve 0.6): up makes it start fast (the curve decreases).
  const Vec2 at2{f.x(3.0), (b.top + b.bottom) * 0.5f};
  f.d.drag(at2, {at2.x, at2.y - 60.0f}, 10, mod::kAlt);
  after = f.points(b.param);
  CHECK(after[1].curve == doctest::Approx(0.6 - 0.5).epsilon(1e-4));
  CHECK(f.history() == h0 + 2);
}

TEST_CASE("automation: shapes replace the envelope over the time selection only: one undo step") {
  Fixture f;
  f.au().showLanes(f.track("Bass").id, true);
  const AutomationBand b = f.band("Bass", f.pan("Bass"));
  const auto before = f.points(b.param);
  const std::span<const doc::Breakpoint> sb(before);
  // A time selection over bars 2 … 4, pressed away from the triangle (at bar 2 it is at −0.8: the bottom).
  f.d.drag({f.x(2.0), b.top + 2.0f}, {f.x(4.0), b.top + 2.0f}, 6);
  REQUIRE(f.au().timeSelection().has_value());
  CHECK(f.au().timeSelection()->start == 2 * kBar);
  CHECK(f.au().timeSelection()->end == 4 * kBar);
  // The envelope menu offers the shapes.
  Menu* m = f.au().openMenu({f.x(3.0), b.top + 2.0f}, b);
  REQUIRE(m != nullptr);
  REQUIRE(m->items()[0].submenu.size() == 6);
  CHECK(m->items()[0].enabled);
  OverlayLayer::of(*f.arr)->closeAll();
  const size_t h0 = f.history();
  REQUIRE(f.au().insertShape(doc::ops::ShapeKind::Sine));
  REQUIRE(f.history() == h0 + 1);
  CHECK(f.lastChanges().automationOnly());
  const auto after = f.points(b.param);
  const std::span<const doc::Breakpoint> sa(after);
  // Two cycles over two bars spanning the pan lane (−1 … 1): the peak a quarter cycle in.
  CHECK(pq::evalAutomation(sa, 2.25 * kBar) == doctest::Approx(1.0).epsilon(1e-6));
  CHECK(pq::evalAutomation(sa, 2.75 * kBar) == doctest::Approx(-1.0).epsilon(1e-6));
  // Outside the range the envelope is unchanged.
  for (double bars : {0.5, 1.0, 1.9, 4.1, 6.0, 7.5})
    CHECK(pq::evalAutomation(sa, bars * kBar) == doctest::Approx(pq::evalAutomation(sb, bars * kBar)).epsilon(1e-6));
  CHECK_FALSE(f.au().selectedPoints().empty());
  // Ramps.
  REQUIRE(f.au().insertShape(doc::ops::ShapeKind::RampUp));
  CHECK(f.history() == h0 + 2);
  const auto ramp = f.points(b.param);
  CHECK(pq::evalAutomation(std::span<const doc::Breakpoint>(ramp), 3.0 * kBar) == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("automation: the first edit of an unautomated parameter creates its lane in the same undo step") {
  Fixture f;
  const doc::Track& pad = f.track("Pad");
  REQUIRE(pad.lanes.empty());
  f.au().showLanes(pad.id, true);  // its volume, not automated: the dashed line at −4 dB
  const AutomationBand b = f.band("Pad", f.vol("Pad"));
  CHECK(b.lane == nullptr);
  const size_t h0 = f.history();
  f.d.click(f.onLine(b, 2.0));
  REQUIRE(f.history() == h0 + 1);
  CHECK(f.lastChanges().structural());  // AddAutomationLane: the parameter becomes an audio-rate signal (T3.16)
  const auto pts = f.points(f.vol("Pad"));
  REQUIRE(pts.size() == 1);
  CHECK(pts[0].tick == 2 * kBar);
  CHECK(pts[0].value == doctest::Approx(-4.0));
  // The next edit on it is Automation-only.
  f.d.click(f.onLine(f.band("Pad", f.vol("Pad")), 5.0));
  CHECK(f.history() == h0 + 2);
  CHECK(f.lastChanges().automationOnly());
  REQUIRE(f.ctx.undo());
  REQUIRE(f.ctx.undo());
  CHECK(f.track("Pad").lanes.empty());
}

TEST_CASE("automation: A toggles automation mode; the chosen envelope is drawn and edited over the clips") {
  Fixture f;
  ArrangementAutomation& au = f.au();
  f.d.tree().setFocus(f.arr);
  CHECK(f.d.press("A") == KeyResult::Action);
  REQUIRE(au.mode());
  f.d.tree().layoutNow();
  TrackHeader* drums = f.arr->header(f.track("Drums").id);
  REQUIRE(drums != nullptr);
  CHECK(drums->paramChooser().visible());
  CHECK(drums->lanesToggle().visible());
  CHECK(au.chosen(f.track("Drums").id) == f.vol("Drums"));
  CHECK(au.chosen(f.track("Piano").id).kind == doc::ParamKind::MosGenerator);  // its first lane
  CHECK(au.chosen(f.track("Pad").id) == f.vol("Pad"));                          // nothing automated: volume
  // A click on the line over the clips adds a point to Drums' volume; a click on a clip's title strip still selects it.
  const AutomationBand b = f.band("Drums", f.vol("Drums"), true);
  CHECK(b.overClips);
  const size_t n0 = f.points(b.param).size(), h0 = f.history();
  f.d.click(f.onLine(b, 3.0));
  CHECK(f.history() == h0 + 1);
  CHECK(f.points(b.param).size() == n0 + 1);
  const IRect lane = f.arr->rowLaneRect(f.row("Drums"));
  f.d.click({f.x(0.5), static_cast<float>(lane.y + 6)});
  CHECK(f.arr->selection().clips.size() == 1);
  CHECK(f.au().selectedPoints().empty());
  // The chooser switches the parameter shown over the clips.
  au.choose(f.track("Drums").id, f.pan("Drums"));
  CHECK(f.band("Drums", f.pan("Drums"), true).lane == nullptr);
  // The lanes toggle in the header shows the lanes.
  f.d.click(f.d.center(std::string(kArr) + "/tracks/track[0]/lanes"));
  CHECK(au.lanesShown(f.track("Drums").id));
  f.d.tree().setFocus(f.arr);
  CHECK(f.d.press("A") == KeyResult::Action);
  CHECK_FALSE(au.mode());
  f.d.tree().layoutNow();
  CHECK_FALSE(drums->paramChooser().visible());
  CHECK(drums->lanesToggle().visible());  // the lanes are still shown
}

TEST_CASE("automation: a hand-made change overrides an automated parameter; Re-enable Automation clears it") {
  Fixture f;
  MixerStrip& drums = static_cast<MixerStrip&>(f.arr->header(f.track("Drums").id)->mixerSection().child(0));
  MixerStrip& bass = static_cast<MixerStrip&>(f.arr->header(f.track("Bass").id)->mixerSection().child(0));
  CHECK(drums.automatedMask() == 1u);   // volume
  CHECK(bass.automatedMask() == 2u);    // pan
  CHECK(drums.overriddenMask() == 0u);
  CHECK_FALSE(f.ctx.anyAutomationOverridden());
  CHECK_FALSE(f.arr->reenableButton()->visible());
  // Bass's volume is not automated: changing it overrides nothing.
  const IRect bv = bass.volume()->rect();
  f.d.drag({static_cast<float>(bv.x + bv.w / 2), static_cast<float>(bv.y + bv.h / 2)},
           {static_cast<float>(bv.x + bv.w / 2), static_cast<float>(bv.y + bv.h / 2 - 20)}, 4);
  CHECK_FALSE(f.ctx.anyAutomationOverridden());
  // Drums' volume is: its automation is overridden (grey, the marker grey, the button shows).
  const IRect dv = drums.volume()->rect();
  f.d.drag({static_cast<float>(dv.x + dv.w / 2), static_cast<float>(dv.y + dv.h / 2)},
           {static_cast<float>(dv.x + dv.w / 2), static_cast<float>(dv.y + dv.h / 2 - 20)}, 4);
  CHECK(f.ctx.automationOverridden(f.vol("Drums")));
  CHECK(drums.overriddenMask() == 1u);
  f.d.tree().layoutNow();
  REQUIRE(f.arr->reenableButton()->visible());
  f.d.click(f.d.center(std::string(kArr) + "/reenable-automation"));
  CHECK_FALSE(f.ctx.anyAutomationOverridden());
  CHECK(drums.overriddenMask() == 0u);
  f.d.tree().layoutNow();
  CHECK_FALSE(f.arr->reenableButton()->visible());
  // Undo does not override; a new project clears.
  f.ctx.overrideAutomation(f.pan("Bass"));
  f.ctx.newProject();
  CHECK_FALSE(f.ctx.anyAutomationOverridden());
}

TEST_CASE("automation: a drag frame damages only the affected columns of the bands showing the parameter") {
  Fixture f;
  f.au().showLanes(f.track("Drums").id, true);
  f.d.step();
  const AutomationBand b = f.band("Drums", f.vol("Drums"));
  const Vec2 p = f.point(b, 4.0);
  f.d.move(p);
  f.d.down(p);
  f.d.move({p.x - 40.0f, p.y + 4.0f});
  f.d.step();
  const IRect lv = f.arr->lanesViewport();
  for (int i = 1; i <= 8; ++i) {
    // A beat (30 px) per frame, the value wobbling by a pixel: every frame changes the preview.
    f.d.move({p.x - 40.0f + 30.0f * static_cast<float>(i), p.y + 4.0f - static_cast<float>(i % 2)});
    const Damage& dm = f.d.tree().damage();
    REQUIRE_FALSE(dm.full());
    int32_t x0 = INT32_MAX, x1 = INT32_MIN;
    for (const ClipRect& r : dm.rects()) {
      CHECK(r.y0 >= b.rect.y);
      CHECK(r.y1 <= b.rect.bottom());
      x0 = std::min(x0, r.x0);
      x1 = std::max(x1, r.x1);
    }
    REQUIRE(x1 > x0);
    // From the left neighbour (bar 2) to the right one (bar 6) plus the handles and the readout: not the whole lane.
    CHECK(x0 >= static_cast<int32_t>(f.x(2.0)) - 12);
    CHECK(x1 <= static_cast<int32_t>(f.x(6.0)) + 110);
    CHECK(x1 - x0 < lv.w);
    f.d.step();
  }
  f.d.up({p.x + 200.0f, p.y + 4.0f});
}

TEST_CASE("automation: HiDPI (2x): bands scale and clicks land on the same ticks") {
  Fixture f(2.0f, 1280, 800);
  f.au().showLanes(f.track("Drums").id, true);
  const AutomationBand b = f.band("Drums", f.vol("Drums"));
  CHECK(b.rect.h == 2 * static_cast<int32_t>(automation_metrics::kLaneHeight) - 1);
  const size_t n0 = f.points(b.param).size();
  f.d.click(f.onLine(b, 1.03));
  const auto after = f.points(b.param);
  REQUIRE(after.size() == n0 + 1);
  CHECK(std::any_of(after.begin(), after.end(), [](const doc::Breakpoint& p) { return p.tick == kBar; }));
  // The point is drawn where the click was (device px).
  const auto pos = f.au().pointPos(f.band("Drums", f.vol("Drums")), f.au().selectedPoints().at(0));
  REQUIRE(pos.has_value());
  CHECK(std::abs(pos->x - f.x(1.0)) < 0.5f);
}

TEST_CASE("render: automation drags repaint only damage, and equal a full repaint at every step") {
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
  REQUIRE(ctx.openProject(views::test::fixtureDir("automation")).ok());
  UiTree tree;
  tree.setViewport(W, H);
  tree.setResources(&res);
  auto shell = Shell::create(tree, ctx);
  REQUIRE(shell.ok());
  ctx.setDetailVisible(false);
  tree.layoutNow();
  auto* arr = dynamic_cast<ArrangementView*>(tree.find(kArr));
  REQUIRE(arr != nullptr);
  arr->setTimeView(0.0, 110.0 / static_cast<double>(kBar));
  ArrangementAutomation& au = arr->automation();
  const doc::Project& p0 = *ctx.doc();
  const doc::Track* drums = nullptr;
  const doc::Track* lead = nullptr;
  doc::forEachTrack(p0, [&](const doc::Track& t, const doc::Track*) {
    if (t.name == "Drums") drums = &t;
    if (t.name == "Lead") lead = &t;
  });
  REQUIRE((drums && lead));
  const doc::TrackId drumsId = drums->id, leadId = lead->id;
  au.showLanes(drumsId, true);
  au.setMode(true);
  tree.layoutNow();

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
  auto pointer = [&](PointerType t, Vec2 pos, Mods m = mod::kNone) {
    PointerEvent e;
    e.type = t;
    e.pos = pos;
    e.mods = m;
    e.button = (t == PointerType::Down || t == PointerType::Up) ? PointerButton::Left : PointerButton::None;
    e.clickCount = 1;
    tree.dispatchPointer(e);
    frame();
  };
  // 1. Move a breakpoint of the Drums volume lane (its band in automation mode over the row repaints too).
  {
    const auto b = au.band(drumsId, doc::ParamRef::track(doc::ParamKind::TrackVolume, drumsId), false);
    REQUIRE(b.has_value());
    const Vec2 at = *au.pointPos(*b, b->lane->points[2].id);
    pointer(PointerType::Move, at);
    pointer(PointerType::Down, at);
    for (int i = 1; i <= 12; ++i) {
      pointer(PointerType::Move, {at.x + 7.0f * static_cast<float>(i), at.y - 2.0f * static_cast<float>(i)});
      compare("automation-move-" + std::to_string(i));
    }
    pointer(PointerType::Up, {at.x + 84.0f, at.y - 24.0f});
    frame();
    compare("automation-move-release");
  }
  // 2. Alt-drag Lead's volume ramp over its clips (automation mode).
  {
    const auto b = au.band(leadId, doc::ParamRef::track(doc::ParamKind::TrackVolume, leadId), true);
    REQUIRE(b.has_value());
    const Vec2 at{arr->xOfTick(3.0 * static_cast<double>(kBar)), (b->top + b->bottom) * 0.5f};
    pointer(PointerType::Move, at, mod::kAlt);
    pointer(PointerType::Down, at, mod::kAlt);
    for (int i = 1; i <= 8; ++i) {
      pointer(PointerType::Move, {at.x, at.y - 6.0f * static_cast<float>(i)}, mod::kAlt);
      compare("automation-curve-" + std::to_string(i));
    }
    pointer(PointerType::Up, {at.x, at.y - 48.0f}, mod::kAlt);
    frame();
    compare("automation-curve-release");
  }
  CHECK(partialFrames > 10);
  CHECK(au.stats().envTilesBuilt > 0);  // T3.x-fix-paint: the undragged bands came from envelope tiles
}

TEST_CASE("render: envelope tiles (T3.x-fix-paint) match the direct ribbon and keep partial == full through scroll and zoom") {
  GpuOptions go;
  go.validation = true;
  auto gpu = GpuContext::create(go);
  REQUIRE_MESSAGE(gpu.ok(), (gpu.ok() ? "" : gpu.error().message));
  auto renderer = Renderer::create(**gpu, OffscreenTarget::kFormat, 3);
  REQUIRE(renderer.ok());
  auto tex = createSceneTextures(**renderer);
  REQUIRE(tex.ok());
  const PaintResources res = paintResources(*tex);
  PaintResources resNoTiles = res;
  resNoTiles.tiles = nullptr;
  REQUIRE(res.tiles != nullptr);
  constexpr uint32_t W = 1280, H = 800;
  FakeTransport port;
  ViewContext ctx(port);
  REQUIRE(ctx.openProject(views::test::fixtureDir("automation")).ok());
  UiTree tree;
  tree.setViewport(W, H);
  tree.setResources(&res);
  auto shell = Shell::create(tree, ctx);
  REQUIRE(shell.ok());
  ctx.setDetailVisible(false);
  tree.layoutNow();
  auto* arr = dynamic_cast<ArrangementView*>(tree.find(kArr));
  REQUIRE(arr != nullptr);
  ArrangementAutomation& au = arr->automation();
  for (const doc::TrackPtr& t : ctx.doc()->tracks) au.showLanes(t->id, true);
  au.setMode(true);
  tree.layoutNow();
  auto partial = OffscreenTarget::create(**gpu, W, H, 1);
  auto full = OffscreenTarget::create(**gpu, W, H, 1);
  REQUIRE((partial.ok() && full.ok()));
  DrawList dl;
  const DrawLayer layer{&dl, kTransientLayer, 0};
  FrameDesc ff;
  ff.layers = {&layer, 1};
  ff.clear = kSceneClearColor;
  double now = 0.0;
  auto drawFull = [&](OffscreenTarget& target, const PaintResources& r) {
    tex->text->beginFrame();
    dl.reset(W, H);
    tree.paint(dl, r, now);
    REQUIRE(target.draw(**renderer, ff).ok());
    auto img = target.read();
    REQUIRE(img.ok());
    return std::move(*img);
  };
  auto frame = [&] {
    now += 1.0 / 144.0;
    tree.update(now);
    if (!tree.damage().needsFrame()) return;
    tex->text->beginFrame();
    dl.reset(W, H);
    if (tree.damage().full()) {
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
  };
  auto settle = [&] {
    for (int i = 0; i < 400 && (i < 2 || tree.needsFrame() || arr->zooming()); ++i) frame();
  };
  auto compare = [&](const std::string& what) {
    auto a = (*partial)->read();
    REQUIRE(a.ok());
    const Image b = drawFull(**full, res);
    const ImageDiff diff = compareImages(*a, b, 0);
    if (diff.pixelsOverTolerance != 0) {
      (void)writePng(what + ".partial.png", *a);
      (void)writePng(what + ".full.png", b);
    }
    CHECK_MESSAGE(diff.pixelsOverTolerance == 0, what << ": " << diff.pixelsOverTolerance << " px differ");
  };
  (void)drawFull(**partial, res);
  settle();
  // Settled: every automated band visible is drawn from tiles, none missing.
  (void)drawFull(**full, res);
  CHECK(au.stats().envTilesDrawn > 0);
  CHECK(au.stats().envTilesMissing == 0);
  CHECK(au.stats().envTilesBuilt > 0);
  compare("envtiles-settled");
  // Tiles against the direct ribbon: the same envelope within the golden tolerance (antialiasing may differ).
  {
    const Image a = drawFull(**full, res);
    const Image b = drawFull(**partial, resNoTiles);
    CHECK(au.stats().envTilesDrawn == 0);
    ImageDiff d;
    CHECK(pq::ui::test::matchesGolden("envtiles-vs-direct", a, b, true, &d));
    (void)drawFull(**partial, res);
  }
  // Scrolling keeps the tiles (whole-pixel chunks): nothing is rebuilt, partial frames equal a full repaint.
  const uint64_t built = au.stats().envTilesBuilt;
  for (int i = 0; i < 12; ++i) {
    arr->scrollByPx(23.0);
    frame();
  }
  settle();
  compare("envtiles-scrolled");
  CHECK(au.stats().envTilesBuilt > built);  // the newly exposed chunks
  const uint64_t afterScroll = au.stats().envTilesBuilt;
  arr->scrollByPx(-46.0);
  settle();
  CHECK(au.stats().envTilesBuilt == afterScroll);  // scrolling back: the chunks are still there
  compare("envtiles-scrolled-back");
  // Zoom: direct while zooming, tiles at the new zoom once it settles, repainted whole.
  const IRect lv = arr->lanesViewport();
  for (int i = 0; i < 4; ++i) {
    arr->zoomAt(static_cast<float>(lv.x + lv.w / 3), 0.85);
    frame();
  }
  compare("envtiles-zooming");
  settle();
  compare("envtiles-zoomed");
  (void)drawFull(**full, res);
  CHECK(au.stats().envTilesMissing == 0);
  CHECK(au.stats().envTilesDrawn > 0);
  CHECK((*gpu)->validationErrorCount() == 0);
}

namespace {

void checkGolden(const char* scene) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, views::test::fixtureDir("automation"), w, h, {}, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &d));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(d));
}

}  // namespace

TEST_CASE("render: automation-lanes over tests/views/fixtures/automation matches the golden image") { checkGolden("automation-lanes"); }

TEST_CASE("render: automation-mode-over-clips over tests/views/fixtures/automation matches the golden image") {
  checkGolden("automation-mode-over-clips");
}

TEST_CASE("automation: a dense lane costs O(width): one span per pixel column, no handles") {
  FakeTransport port;
  ViewContext ctx(port);
  views::test::buildAutomationBench(ctx.store(), 1, 20000);
  TestDriver d(1280, 800);
  auto s = Shell::create(d.tree(), ctx);
  REQUIRE(s.ok());
  auto* arr = dynamic_cast<ArrangementView*>(d.find(kArr));
  REQUIRE(arr != nullptr);
  ctx.setDetailVisible(false);
  const doc::TrackId t = ctx.doc()->tracks.front()->id;
  arr->automation().showLanes(t, true);
  d.tree().layoutNow();
  arr->setTimeView(0.0, static_cast<double>(arr->lanesViewport().w) / (65.0 * static_cast<double>(kBar)));
  REQUIRE(d.enableGpu().ok());
  d.tree().invalidateAll();
  d.step();
  DrawList dl;
  dl.reset(d.tree().width(), d.tree().height());
  d.tree().paint(dl, d.resources(), d.now(), arr->lanesViewport());
  const auto& st = arr->automation().stats();
  CHECK(st.bandsPainted == 1);
  CHECK(st.handles == 0);
  CHECK(st.vertices <= static_cast<uint32_t>(arr->lanesViewport().w) + 2);  // spans, one per column
  CHECK(st.vertices > 100);
  d.tree().setRoot(std::make_unique<Widget>());
}
