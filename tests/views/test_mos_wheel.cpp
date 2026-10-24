// T3.14 MOS wheel: the circle view and the popover.
//   Circle: rotate by generator / by step through the buttons matches tuning::rotateByGenerator (and
//   tuning::applyModeAction carries MODMOS) for every region of sizes 3–12; hardness r gives the exact step ratio
//   L/s = r within 1e-6 (2.0 in 5L 2s is exactly 7\12; the slider snaps to its 2:1 notch) on direct, mirror and
//   multi-period regions; the parent / child arrows name tuning::findParentMos / tuning::childMoses (the child whose
//   region holds the generator) and navigate there; the MODMOS dot drag snaps to whole chromas, Esc cancels.
//   Popover (through the real shell): the clip box's tuning chip opens it for the clip; a chart drag previews the tuning
//   in the piano roll without touching the document; Apply / Enter / a press outside commits one command (one undo
//   step) that changes only that clip; Esc cancels with no history entry; EDO entry, Scala file (scripted dialog),
//   temperament presets, track and main-tuning targets; Ctrl+Shift+W.
//   Goldens: mos-circle-5L2s, mos-circle-modmos, mos-wheel-popover.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/tuning/mos.h"
#include "pq/tuning/mos_data.h"
#include "pq/tuning/mos_regions.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mos_wheel/mos_chart.h"
#include "pq/views/mos_wheel/mos_circle.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
namespace tu = pq::tuning;

namespace {

struct CircleEvent {
  std::optional<MosSelection> sel;
  std::vector<double> modmos;
  MosGesture gesture;
};

MosChartConfig smallConfig() {
  MosChartConfig cfg;
  cfg.maxEdo = 31;
  return cfg;
}

// The selection the chart makes for `key` at `gen` (the circle's input in the popover).
MosSelection selectionFor(tu::MosKey perPeriodKey, double gen, int32_t rotation = 0, MosChartConfig cfg = smallConfig()) {
  MosChart chart("chart", cfg);
  REQUIRE(chart.select(perPeriodKey, gen, rotation));
  REQUIRE(chart.selection().has_value());
  return *chart.selection();
}

struct Circle {
  TestDriver d;
  MosCircle* c = nullptr;
  std::vector<CircleEvent> events;
  explicit Circle(MosChartConfig cfg = smallConfig(), uint32_t w = 440, uint32_t h = 560) : d(w, h) {
    c = &d.setRoot<MosCircle>("mos-circle", cfg);
    c->setOnChange([this](const std::optional<MosSelection>& s, std::span<const double> mm, MosGesture g) {
      events.push_back({s, {mm.begin(), mm.end()}, g});
    });
    d.tree().layoutNow();
  }
  void set(const MosSelection& s, std::vector<double> modmos = {}) {
    c->setSelection(s);
    c->setModmos(std::move(modmos));
    events.clear();
  }
  static Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + r.w / 2.0f, static_cast<float>(r.y) + r.h / 2.0f}; }
  Vec2 atCents(double cents) const {
    const MosCircle::Geometry& g = c->geometry();
    const double dp = c->selection()->period * c->selection()->periodsPerEquave;
    const double a = cents / dp * 2.0 * std::numbers::pi - std::numbers::pi / 2.0;
    return {g.centre.x + static_cast<float>(std::cos(a)) * g.radius * 0.8f, g.centre.y + static_cast<float>(std::sin(a)) * g.radius * 0.8f};
  }
};

}  // namespace

// ---- circle ---------------------------------------------------------------------------------------------------------

TEST_CASE("mos circle: the mode buttons rotate by generator (tuning::rotateByGenerator) and by step") {
  Circle t;
  const tu::MosRegionSet& rs = t.c->regionSet();
  int checked = 0;
  for (const tu::MosRegion& r : rs.regions) {
    if (r.perPeriodSize < 3 || r.perPeriodSize > 12) continue;
    const double g = (r.lowCents + r.highCents) / 2.0;
    const int32_t n = r.perPeriodSize;
    const tu::RegionHint hint = r.hint();
    for (int32_t r0 = 0; r0 < n; ++r0) {
      const MosSelection s = selectionFor(r.perPeriodKey, g, r0);
      t.set(s);
      t.d.click(Circle::mid(t.c->geometry().genUp));
      REQUIRE(t.c->selection().has_value());
      CHECK(t.c->selection()->modeRotation == tu::rotateByGenerator(g, n, r0, 1, 1200.0, &hint));
      REQUIRE(t.events.size() == 1);
      CHECK(t.events.back().gesture == MosGesture::Commit);
      t.set(s);
      t.d.click(Circle::mid(t.c->geometry().genDown));
      CHECK(t.c->selection()->modeRotation == tu::rotateByGenerator(g, n, r0, -1, 1200.0, &hint));
      t.set(s);
      t.d.click(Circle::mid(t.c->geometry().stepUp));
      CHECK(t.c->selection()->modeRotation == (r0 + 1) % n);
      t.set(s);
      t.d.click(Circle::mid(t.c->geometry().stepDown));
      CHECK(t.c->selection()->modeRotation == (r0 + n - 1) % n);
      ++checked;
    }
  }
  CHECK(checked > 300);

  SUBCASE("MODMOS alterations follow their notes (tuning::applyModeAction)") {
    const MosSelection s = selectionFor({5, 2}, 696.0, 0);
    const std::vector<double> mm{0, 0, 0, -1, 0, 0, 0};
    t.set(s, mm);
    t.d.click(Circle::mid(t.c->geometry().genUp));
    const tu::RegionHint hint = s.hint();
    const tu::ModeChange want = tu::applyModeAction(tu::ModeAction::GenUp, 696.0, 7, 0, mm, 1200.0, &hint);
    CHECK(t.c->selection()->modeRotation == want.rotation);
    CHECK(std::vector<double>(t.c->modmos().begin(), t.c->modmos().end()) == want.modmos);
  }
}

TEST_CASE("mos circle: hardness r gives the exact step ratio L/s = r within 1e-6 (generatorForHardness in closed form)") {
  for (const MosChartConfig cfg : {smallConfig(), [] {
         MosChartConfig c = smallConfig();
         c.period = 600.0;
         c.periodsPerEquave = 2;
         return c;
       }()}) {
    Circle t(cfg);
    int checked = 0, cliAgreed = 0;
    for (const tu::MosRegion& r : t.c->regionSet().regions) {
      if (r.perPeriodSize < 3 || r.perPeriodSize > 11) continue;
      t.set(selectionFor(r.perPeriodKey, (r.lowCents + r.highCents) / 2.0, 0, cfg));
      for (const double ratio : {1.25, 1.5, 2.0, 2.5, 3.0, 5.0}) {
        REQUIRE(t.c->setHardness(ratio));
        const MosSelection& s = *t.c->selection();
        CHECK(s.generatorCents >= s.regionLow);
        CHECK(s.generatorCents <= s.regionHigh);
        CHECK(std::fabs(exactHardness(s) - ratio) < 1e-6);
        // Where the CLI solves it (its first region of this key; it fails on regions ending at 0 or P/2, where its
        // unnudged chain degenerates: Q?-T3.14-a), its generator agrees with ours to its 0.01-cent precision.
        if (!r.isMirror && cfg.periodsPerEquave == 1)
          if (const auto sol = tu::mos_calc::generatorForHardness(r.perPeriodKey.nL, r.perPeriodKey.nS, ratio, cfg.period)) {
            const auto b = tu::mos_calc::mosRegionBounds(r.perPeriodKey.nL, r.perPeriodKey.nS, cfg.period);
            if (b && std::fabs(b->lowCents - r.lowCents) < 1e-6 && std::fabs(b->highCents - r.highCents) < 1e-6 &&
                std::fabs(sol->ratio - ratio) < 0.01 && ++cliAgreed)
              CHECK_MESSAGE(std::fabs(sol->generator - s.generatorCents) < 0.02, r.perPeriodKey.nL << "L" << r.perPeriodKey.nS << "s [" << r.lowCents << "," << r.highCents << "] r=" << ratio << " cli=" << sol->generator << " (L/s " << sol->ratio << ") ours=" << s.generatorCents);
          }
      }
      ++checked;
    }
    CHECK(checked > 20);
    if (cfg.periodsPerEquave == 1) CHECK(cliAgreed > 100);
  }

  SUBCASE("5L 2s at hardness 2 is exactly 7\\12; the slider snaps to its 2:1 notch") {
    Circle t;
    t.set(selectionFor({5, 2}, 690.0));
    REQUIRE(t.c->setHardness(2.0));
    CHECK(t.c->selection()->generatorCents == 700.0);
    REQUIRE(t.c->selection()->edo.has_value());
    CHECK(t.c->selection()->edo->edo == 12);
    t.set(selectionFor({5, 2}, 690.0));
    const IRect sl = t.c->geometry().slider;
    const float y = static_cast<float>(sl.y) + sl.h / 2.0f;
    t.d.drag({static_cast<float>(sl.x) + 3.0f, y}, {t.c->sliderXOfHardness(2.0) + 2.0f, y}, 6);
    CHECK(std::fabs(t.c->hardness() - 2.0) < 1e-6);
    REQUIRE(!t.events.empty());
    CHECK(t.events.back().gesture == MosGesture::Commit);
    CHECK(std::count_if(t.events.begin(), t.events.end(), [](const CircleEvent& e) { return e.gesture == MosGesture::Commit; }) == 1);
    for (size_t i = 0; i + 1 < t.events.size(); ++i) CHECK(t.events[i].gesture == MosGesture::Live);
    // Alt bypasses the notch.
    t.set(selectionFor({5, 2}, 690.0));
    t.d.drag({static_cast<float>(sl.x) + 3.0f, y}, {t.c->sliderXOfHardness(2.0) + 2.0f, y}, 6, mod::kAlt);
    CHECK(std::fabs(t.c->hardness() - 2.0) > 1e-3);
    // Esc during a slider drag: back to the press, Cancel.
    t.set(selectionFor({5, 2}, 690.0));
    t.d.down({static_cast<float>(sl.x) + 3.0f, y});
    t.d.move({static_cast<float>(sl.x) + sl.w * 0.7f, y});
    t.d.press("Escape");
    t.d.up({static_cast<float>(sl.x) + sl.w * 0.7f, y});
    CHECK(t.c->selection()->generatorCents == 690.0);
    CHECK(t.events.back().gesture == MosGesture::Cancel);
  }
}

TEST_CASE("mos circle: parent / child arrows match findParentMOS / getChildMOSes and navigate there") {
  Circle t;
  int checked = 0;
  for (const tu::MosRegion& r : t.c->regionSet().regions) {
    if (r.perPeriodSize < 2 || r.perPeriodSize > 12) continue;
    const double g = (r.lowCents + r.highCents) / 2.0;
    t.set(selectionFor(r.perPeriodKey, g));
    CHECK(t.c->parentKey() == tu::findParentMos(r.key.nL, r.key.nS));
    const std::array<tu::MosKey, 2> kids = tu::childMoses(r.key);
    const std::optional<tu::MosKey> child = t.c->childKey();
    REQUIRE(child.has_value());
    CHECK((*child == kids[0] || *child == kids[1]));
    // The arrow names the child whose region holds the generator (when it is within the chart's sizes).
    for (const tu::MosRegion& cr : t.c->regionSet().regions)
      if ((cr.key == kids[0] || cr.key == kids[1]) && g >= cr.lowCents && g <= cr.highCents && !(cr.key == *child)) {
        // Only acceptable when the named child also holds it (g on a shared boundary).
        bool alsoHolds = false;
        for (const tu::MosRegion& q : t.c->regionSet().regions) alsoHolds = alsoHolds || (q.key == *child && g >= q.lowCents && g <= q.highCents);
        CHECK(alsoHolds);
      }
    ++checked;
  }
  CHECK(checked > 50);

  // Clicking: 5L 2s → parent 2L 3s, back down to a child containing the generator, the mode carried.
  t.set(selectionFor({5, 2}, 696.0, 3));
  REQUIRE(t.c->parentKey() == tu::MosKey{2, 3});
  t.d.click(t.c->geometry().parentArrow);
  REQUIRE(t.c->selection().has_value());
  CHECK(t.c->selection()->key == tu::MosKey{2, 3});
  CHECK(t.c->selection()->generatorCents == 696.0);
  CHECK(t.c->selection()->modeRotation == tu::transferModeRotation(696.0, 7, 3, 696.0, 5, 1200.0, nullptr));
  CHECK(t.events.back().gesture == MosGesture::Commit);
  const std::optional<tu::MosKey> down = t.c->childKey();
  REQUIRE(down.has_value());
  CHECK(*down == tu::MosKey{5, 2});
  t.d.click(t.c->geometry().childArrow);
  CHECK(t.c->selection()->key == tu::MosKey{5, 2});
  t.d.click(t.c->geometry().childArrow);
  CHECK(t.c->selection()->key == tu::MosKey{7, 5});  // 696 ¢ is in 7L 5s (meantone's chromatic MOS)
}

TEST_CASE("mos circle: MODMOS dot drag snaps to whole chromas; Esc and a right click cancel") {
  Circle t;
  // 5L 2s at 7\12, rotation 0 (Lydian): 0 200 400 600 700 900 1100; chroma 100.
  t.set(selectionFor({5, 2}, 700.0, 0));
  REQUIRE(t.c->degreeCount() == 7);
  CHECK(t.c->displayPitches()[3] == doctest::Approx(600.0));
  t.d.down(t.c->dotPos(3));
  t.d.move(t.atCents(560.0));
  t.d.move(t.atCents(503.0));  // within the snap of −1 chroma
  t.d.up(t.atCents(503.0));
  REQUIRE(t.c->modmos().size() >= 4);
  CHECK(t.c->modmos()[3] == -1.0);
  CHECK(t.c->displayPitches()[3] == doctest::Approx(500.0));
  CHECK(t.events.back().gesture == MosGesture::Commit);
  CHECK(t.c->titleText().find("MODMOS") != std::string::npos);
  REQUIRE(t.c->params().has_value());
  CHECK(t.c->params()->modmos.size() == 7);

  // Esc: back to the press.
  t.events.clear();
  t.d.down(t.c->dotPos(5));
  t.d.move(t.atCents(1000.0));
  CHECK(t.events.back().gesture == MosGesture::Live);
  t.d.press("Escape");
  t.d.up(t.atCents(1000.0));
  CHECK(t.c->modmos()[5] == 0.0);
  CHECK(t.c->modmos()[3] == -1.0);
  CHECK(t.events.back().gesture == MosGesture::Cancel);

  // Reset clears every alteration.
  t.d.click(Circle::mid(t.c->geometry().reset));
  CHECK_FALSE(tu::hasModmos(t.c->modmos()));
  CHECK(t.events.back().gesture == MosGesture::Commit);

  // The tonic only auditions.
  double heard = -1.0;
  t.c->onAudition = [&](double c) { heard = c; };
  t.d.click(t.c->dotPos(0));
  CHECK(heard == 0.0);
}

TEST_CASE("mos circle: step labels, title and the all-intervals toggle") {
  Circle t;
  t.set(selectionFor({5, 2}, 700.0, 1));  // Mixolydian side: L L s L L s L
  const auto labels = t.c->stepLabels();
  REQUIRE(labels.size() == 7);
  std::string pat;
  for (const std::string& l : labels) pat += l;
  const tu::RegionHint hint = t.c->selection()->hint();
  CHECK(pat == tu::computeStepPattern(700.0, 7, 1, 1200.0, &hint));
  const std::string title = t.c->titleText();
  CHECK(title.find("5L 2s") == 0);
  CHECK(title.find("2.00 (2:1)") != std::string::npos);
  CHECK_FALSE(t.c->showAllIntervals());
  t.d.click(Circle::mid(t.c->geometry().allIntervals));
  CHECK(t.c->showAllIntervals());
}

// ---- popover ---------------------------------------------------------------------------------------------------------

namespace {

// The shell over a project with track A (clips A1, A2) and track B (clip B1), A1 in the clip view.
struct Wheel {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  views::test::ScriptedDialogs dialogs;
  doc::TrackId trackA, trackB;
  doc::ClipRef a1, a2, b1;

  Wheel() {
    doc::DocumentStore& st = ctx.store();
    auto run = [&](const doc::ops::EditResult& e) {
      REQUIRE(e.ok());
      REQUIRE(st.dispatch(e->command).ok());
      return *e;
    };
    trackA = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "A")).tracks[0];
    trackB = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, trackA, "B")).tracks[0];
    a1 = run(doc::ops::insertEmptyClip(*st.current(), st.ids(), trackA, 0, kTicksPerWhole, "A1")).clips[0];
    a2 = run(doc::ops::insertEmptyClip(*st.current(), st.ids(), trackA, 2 * kTicksPerWhole, 3 * kTicksPerWhole, "A2")).clips[0];
    b1 = run(doc::ops::insertEmptyClip(*st.current(), st.ids(), trackB, 0, kTicksPerWhole, "B1")).clips[0];
    std::vector<doc::Note> notes(3);
    for (int i = 0; i < 3; ++i) {
      notes[static_cast<size_t>(i)].start = i * kPpq;
      notes[static_cast<size_t>(i)].length = kPpq;
      notes[static_cast<size_t>(i)].pitch = tu::PitchId::fromDegree(i * 2, 0);
    }
    run(doc::ops::addNotes(*st.current(), st.ids(), a1, notes));
    ctx.setFileDialogs(&dialogs);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(520.0f);
    ctx.setDetailClip(a1);
    d.tree().layoutNow();
  }
  ~Wheel() { d.tree().setRoot(std::make_unique<Widget>()); }

  ClipView& view() const {
    auto* v = dynamic_cast<ClipView*>(d.find("main/body/bottom/detail/clip/clip-view"));
    REQUIRE(v != nullptr);
    return *v;
  }
  MosWheelPopover* popover() const { return openMosWheelPopover(shell->overlay()); }
  MosWheelPopover& openFromChip() {
    const Vec2 chip = d.center("main/body/bottom/detail/clip/clip-view/clip-box/tuning");
    d.click(chip);
    d.step();
    MosWheelPopover* p = popover();
    REQUIRE(p != nullptr);
    return *p;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::NoteClip* clip(const doc::ClipRef& r) const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), r.track);
    REQUIRE(t != nullptr);
    const int32_t i = doc::findClipIndex(*t, r.clip);
    REQUIRE(i >= 0);
    return t->clips[static_cast<size_t>(i)].clip.get();
  }
  const doc::TuningDef* tuningOf(const doc::ClipRef& r) const { return targetTuning(*ctx.doc(), TuningTarget::forClip(r)); }
};

}  // namespace

TEST_CASE("mos wheel: the clip box chip opens the wheel; a chart drag previews in the roll; Apply commits only that clip") {
  Wheel w;
  CHECK(w.view().rows().degreesPerEquave() == 12);
  auto* chip = dynamic_cast<TuningChip*>(w.d.find("main/body/bottom/detail/clip/clip-view/clip-box/tuning"));
  REQUIRE(chip != nullptr);
  CHECK(chip->text() == "12-EDO");
  MosWheelPopover& p = w.openFromChip();
  CHECK(p.target().kind == TuningTarget::Kind::Clip);
  CHECK(p.target().clip.clip == w.a1.clip);
  const doc::Snapshot before = w.ctx.doc();
  const size_t h0 = w.history();
  const doc::NoteClip* a2Before = w.clip(w.a2);
  const doc::NoteClip* b1Before = w.clip(w.b1);

  // Choose 5L 2s, then drag the generator in the chart: every Live frame previews, nothing is committed.
  REQUIRE(p.chooseMos({5, 2}, 696.0));
  w.d.step();
  CHECK(w.ctx.tuningPreview().has_value());
  CHECK(w.view().rows().degreesPerEquave() == 7);
  CHECK(chip->text().find("5L 2s") == 0);
  MosChart& chart = p.chart();
  const IRect ax = chart.axisRect();
  const float y = static_cast<float>(ax.y) + ax.h / 2.0f;
  w.d.down({chart.xOfCents(696.0), y});
  for (const double c : {700.5, 704.0, 708.0}) {
    w.d.move({chart.xOfCents(c), y}, mod::kAlt);
    w.d.step();
    REQUIRE(w.ctx.tuningPreview().has_value());
    const tu::MosParams* m = w.ctx.tuningPreview()->def.mos();
    REQUIRE(m != nullptr);
    CHECK(std::fabs(m->generator - c) < 0.5);
    CHECK(w.ctx.doc() == before);  // the document is untouched while dragging
    CHECK(w.history() == h0);
  }
  w.d.up({chart.xOfCents(708.0), y}, PointerButton::Left, mod::kAlt);
  CHECK(w.ctx.doc() == before);
  CHECK(w.history() == h0);
  const doc::TuningDef draft = p.draft();
  REQUIRE(draft.mos() != nullptr);

  // Apply: one command, one undo step, only A1 changes.
  w.d.click(w.d.center("overlay/mos-wheel/footer/apply"));
  w.d.step();
  CHECK(w.popover() == nullptr);
  CHECK_FALSE(w.ctx.tuningPreview().has_value());
  CHECK(w.history() == h0 + 1);
  REQUIRE(w.tuningOf(w.a1) != nullptr);
  CHECK(*w.tuningOf(w.a1) == draft);
  CHECK(w.clip(w.a1)->tuning.mode == doc::TuningMode::Private);
  CHECK(w.clip(w.a2) == a2Before);  // pointer-equal: untouched
  CHECK(w.clip(w.b1) == b1Before);
  CHECK(w.tuningOf(w.a2)->edo() != nullptr);
  CHECK(w.view().rows().degreesPerEquave() == 7);
  REQUIRE(w.ctx.undo());
  CHECK(w.ctx.doc() == before);
  CHECK(w.view().rows().degreesPerEquave() == 12);
}

TEST_CASE("mos wheel: Esc cancels with no history entry; Enter and a press outside commit") {
  Wheel w;
  const doc::Snapshot before = w.ctx.doc();
  const size_t h0 = w.history();
  {
    MosWheelPopover& p = w.openFromChip();
    REQUIRE(p.chooseMos({5, 2}, 696.0));
    REQUIRE(p.circle().setHardness(2.5));
    CHECK(p.changed());
    w.d.press("Escape");
    w.d.step();
    CHECK(w.popover() == nullptr);
    CHECK(w.ctx.doc() == before);
    CHECK(w.history() == h0);
    CHECK_FALSE(w.ctx.tuningPreview().has_value());
    CHECK(w.view().rows().degreesPerEquave() == 12);
  }
  {
    // Esc during a circle drag cancels only the drag; the popover stays.
    MosWheelPopover& p = w.openFromChip();
    REQUIRE(p.chooseMos({5, 2}, 696.0));
    const IRect sl = p.circle().geometry().slider;
    const float y = static_cast<float>(sl.y) + sl.h / 2.0f;
    w.d.down({static_cast<float>(sl.x) + 2.0f, y});
    w.d.move({static_cast<float>(sl.x) + sl.w * 0.8f, y});
    w.d.press("Escape");
    w.d.up({static_cast<float>(sl.x) + sl.w * 0.8f, y});
    w.d.step();
    REQUIRE(w.popover() == &p);
    REQUIRE(p.draft().mos() != nullptr);
    CHECK(p.draft().mos()->generator == 696.0);
    w.d.press("Return");  // Enter commits
    w.d.step();
    CHECK(w.popover() == nullptr);
    CHECK(w.history() == h0 + 1);
    CHECK(w.tuningOf(w.a1)->mos() != nullptr);
  }
  {
    // A press outside keeps what was set (one more undo step); opening without changing anything commits nothing.
    MosWheelPopover& p = w.openFromChip();
    REQUIRE(p.chooseEdo(31));
    w.d.click({40.0f, 880.0f});
    w.d.step();
    CHECK(w.popover() == nullptr);
    CHECK(w.history() == h0 + 2);
    REQUIRE(w.tuningOf(w.a1)->edo() != nullptr);
    CHECK(w.tuningOf(w.a1)->edo()->n() == 31u);
    (void)w.openFromChip();
    w.d.click({40.0f, 880.0f});
    w.d.step();
    CHECK(w.history() == h0 + 2);
  }
}

TEST_CASE("mos wheel: EDO entry, Scala file, temperament presets") {
  Wheel w;
  const size_t h0 = w.history();
  SUBCASE("EDO typed into the field") {
    MosWheelPopover& p = w.openFromChip();
    w.d.click(w.d.center("overlay/mos-wheel/header/edo"));
    w.d.press("Ctrl+A");
    w.d.type("22");
    w.d.press("Return");
    REQUIRE(p.draft().edo() != nullptr);
    CHECK(p.draft().edo()->n() == 22u);
    CHECK(w.view().rows().degreesPerEquave() == 22);
    w.d.click(w.d.center("overlay/mos-wheel/footer/apply"));
    CHECK(w.history() == h0 + 1);
    CHECK(w.tuningOf(w.a1)->edo()->n() == 22u);
  }
  SUBCASE("Scala file through the file dialog") {
    w.dialogs.scalaPath = std::string(PQ_TEST_DATA_DIR) + "/tuning/fixtures/scl/bp_just.scl";
    MosWheelPopover& p = w.openFromChip();
    w.d.click(w.d.center("overlay/mos-wheel/header/scala"));
    CHECK(w.dialogs.scalas == 1);
    REQUIRE(p.draft().scale() != nullptr);
    CHECK(p.draft().scale()->scale.pitches.size() == 13u);
    CHECK(p.draft().referenceHz == p.original().referenceHz);
    w.d.click(w.d.center("overlay/mos-wheel/footer/apply"));
    CHECK(w.history() == h0 + 1);
    CHECK(w.tuningOf(w.a1)->scale() != nullptr);
    // A missing file changes nothing and says so.
    MosWheelPopover& q = w.openFromChip();
    const doc::TuningDef d0 = q.draft();
    CHECK_FALSE(q.chooseScalaFile("no/such/file.scl").ok());
    CHECK(q.draft() == d0);
    w.d.press("Escape");
  }
  SUBCASE("temperament presets select their MOS at the middle of the range") {
    MosWheelPopover& p = w.openFromChip();
    const auto temps = tu::temperaments();
    int32_t meantone = -1;
    for (size_t i = 0; i < temps.size(); ++i)
      if (temps[i].name == "Meantone") meantone = static_cast<int32_t>(i);
    REQUIRE(meantone >= 0);
    REQUIRE(p.chooseTemperament(meantone));
    const tu::MosParams* m = p.draft().mos();
    REQUIRE(m != nullptr);
    CHECK(m->n == 7);
    CHECK(m->generator == doctest::Approx((temps[static_cast<size_t>(meantone)].genLow + temps[static_cast<size_t>(meantone)].genHigh) / 2.0));
    REQUIRE(p.chart().selection().has_value());
    CHECK(p.chart().selection()->key == tu::MosKey{5, 2});
    // Every enabled preset with a 1200-cent period finds a MOS in the chart (or reports why) without throwing state off.
    int found = 0;
    for (size_t i = 0; i < temps.size(); ++i)
      if (!temps[i].disabled && p.chooseTemperament(static_cast<int32_t>(i))) ++found;
    CHECK(found > 10);
    w.d.press("Escape");
    CHECK(w.history() == h0);
  }
}

TEST_CASE("mos wheel: track and main-tuning targets, the preview follows inheritance, Ctrl+Shift+W") {
  Wheel w;
  const size_t h0 = w.history();
  SUBCASE("track") {
    MosWheelPopover* p = openMosWheel(w.shell->overlay(), w.ctx, TuningTarget::forTrack(w.trackA));
    REQUIRE(p != nullptr);
    REQUIRE(p->chooseMos({5, 2}, 696.0));
    // A1 and A2 inherit from track A: both preview; B1 does not.
    const doc::Project& pr = *w.ctx.doc();
    auto previewed = [&](const doc::ClipRef& r) {
      const auto path = doc::findTrack(pr, r.track);
      return w.ctx.previewedTuning(pr, *path, w.clip(r), r.clip);
    };
    CHECK(previewed(w.a1)->mos() != nullptr);
    CHECK(previewed(w.a2)->mos() != nullptr);
    CHECK(previewed(w.b1)->edo() != nullptr);
    p->apply();
    w.d.step();
    CHECK(w.history() == h0 + 1);
    CHECK(doc::findTrackPtr(*w.ctx.doc(), w.trackA)->tuning.mode == doc::TuningMode::Private);
    CHECK(w.tuningOf(w.a2)->mos() != nullptr);
    CHECK(w.tuningOf(w.b1)->edo() != nullptr);
  }
  SUBCASE("main tuning") {
    const doc::TuningId main = w.ctx.doc()->tunings->front()->id;
    MosWheelPopover* p = openMosWheel(w.shell->overlay(), w.ctx, TuningTarget::forMain(main));
    REQUIRE(p != nullptr);
    CHECK(p->chooseEdo(19));
    p->apply();
    w.d.step();
    CHECK(w.history() == h0 + 1);
    CHECK(w.ctx.doc()->tunings->front()->def.edo()->n() == 19u);
    CHECK(w.tuningOf(w.b1)->edo()->n() == 19u);  // everything inheriting the main tuning
  }
  SUBCASE("Ctrl+Shift+W toggles the wheel for the clip in the clip view") {
    w.d.press("Ctrl+Shift+W");
    w.d.step();
    MosWheelPopover* p = w.popover();
    REQUIRE(p != nullptr);
    CHECK(p->target().kind == TuningTarget::Kind::Clip);
    CHECK(p->target().clip.clip == w.a1.clip);
    REQUIRE(p->chooseEdo(17));
    w.d.press("Ctrl+Shift+W");
    w.d.step();
    CHECK(w.popover() == nullptr);
    CHECK(w.history() == h0 + 1);
    CHECK(w.tuningOf(w.a1)->edo()->n() == 17u);
    // Without a clip in the clip view: the first main tuning.
    w.ctx.setDetailClip({});
    CHECK(defaultTuningTarget(w.ctx).kind == TuningTarget::Kind::Main);
  }
}

TEST_CASE("mos wheel: reopening a MOS tuning shows it in the chart and the circle (region, mode, MODMOS)") {
  Wheel w;
  MosWheelPopover& p = w.openFromChip();
  REQUIRE(p.chooseMos({5, 2}, 696.0, 4, {0, 0, 0, 0, 0, 0, 1}));
  p.apply();
  w.d.step();
  MosWheelPopover& q = w.openFromChip();
  REQUIRE(q.chart().selection().has_value());
  CHECK(q.chart().selection()->perPeriodKey == tu::MosKey{5, 2});
  CHECK(q.chart().selection()->modeRotation == 4);
  REQUIRE(q.circle().selection().has_value());
  CHECK(q.circle().selection()->generatorCents == 696.0);
  CHECK(q.circle().modmos().size() == 7u);
  CHECK_FALSE(q.changed());
  w.d.press("Escape");
}

// ---- goldens ----------------------------------------------------------------------------------------------------------

namespace {

void checkGolden(const char* scene) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, {}, w, h, {}, o);
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

TEST_CASE("render: mos-circle-5L2s (18\\31, Lydian root) matches the golden image") { checkGolden("mos-circle-5L2s"); }

TEST_CASE("render: mos-circle-modmos (7\\12 Aeolian, raised 7th) matches the golden image") { checkGolden("mos-circle-modmos"); }

TEST_CASE("render: mos-wheel-popover (clip wheel over the roll preview) matches the golden image") { checkGolden("mos-wheel-popover"); }
