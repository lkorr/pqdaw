// T3.14 MOS wheel: the circle view and the popover.
//   Circle: rotate by generator / by step through the buttons matches tuning::rotateByGenerator (and
//   tuning::applyModeAction carries MODMOS) for every region of sizes 3–12; hardness r gives the exact step ratio
//   L/s = r within 1e-6 (2.0 in 5L 2s is exactly 7\12; the slider snaps to its 2:1 notch) on direct, mirror and
//   multi-period regions; the parent / child arrows name tuning::findParentMos / tuning::childMoses (the child whose
//   region holds the generator) and navigate there; the MODMOS dot drag snaps to whole chromas, Esc cancels.
//   Popover (through the real shell; TU.1): the clip box's tuning chip opens it for the clip; every change is in the
//   document at once (a chart drag step by step inside one store gesture: one undo step per drag, Esc restores where
//   it started), changing only that clip; Revert; Esc / Enter / a press outside close; EDO entry, Scala file
//   (scripted dialog), the temperament search; a main tuning's generator drag is Param-only SetTuningParam steps;
//   track targets; Ctrl+Shift+W; the popover stays inside the panels (L17).
//   TU.2: the explorer's step colours ease to a new generator's and the frames stop once settled; a note-on lights its
//   degree in its frequency's colour and a note-off fades it; the popover lights the notes of playback, the computer
//   keyboard and auditions in the target's tuning.
//   Goldens: mos-circle-5L2s, mos-circle-modmos, mos-circle-lit, mos-wheel-popover.

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
        // unnudged chain degenerates: Q83), its generator agrees with ours to its 0.01-cent precision.
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

TEST_CASE("mos circle: a MODMOS step pattern letters the altered step as the wheel does (no cents); M6 review L18") {
  Circle t;
  t.set(selectionFor({5, 2}, 700.0, 2), {0, 0, 0, 0, 0, 0, 1});  // the mos-circle-modmos scene: a raised 7th
  // Before: "L s L L s 300 s" (cents for the altered step), and the wheel's slices renamed the plain L steps.
  CHECK(t.c->stepPatternText() == "L s L L s M s");
  const auto labels = t.c->stepLabels();
  std::string joined;
  for (size_t i = 0; i < labels.size() && i < 7; ++i) joined += (i ? " " : "") + labels[i];
  CHECK(joined == "L s L L s M s");
}

TEST_CASE("mos circle (TU.2): the step colours ease toward a new generator's, then the frames stop") {
  // 18\31 (L/s = 3:2) and 714¢ (L/s ≈ 7.6) in the same 5L 2s region.
  const MosSelection soft = selectionFor({5, 2}, 18.0 * 1200.0 / 31.0), hard = selectionFor({5, 2}, 714.0);
  Circle fresh;
  fresh.set(hard);
  const auto labels = fresh.c->stepLabels();
  const int32_t sIdx = static_cast<int32_t>(std::find(labels.begin(), labels.end(), "s") - labels.begin());
  REQUIRE(labels[0] == "L");
  REQUIRE(static_cast<size_t>(sIdx) < labels.size());
  const Rgba hardL = fresh.c->sliceColour(0), hardS = fresh.c->sliceColour(sIdx);

  Circle t;
  t.set(soft);
  t.d.advance(0.5);
  CHECK_FALSE(t.c->animating());
  const Rgba softL = t.c->sliceColour(0), softS = t.c->sliceColour(sIdx);
  // The explorer's palette: s is L pushed toward its complement as the scale hardens, so they differ more when hard.
  auto dist = [](Rgba a, Rgba b) {
    int d = 0;
    for (int sh : {0, 8, 16}) d += std::abs(static_cast<int>((a >> sh) & 0xFF) - static_cast<int>((b >> sh) & 0xFF));
    return d;
  };
  CHECK(dist(hardL, hardS) > dist(softL, softS));
  REQUIRE(dist(softL, hardL) + dist(softS, hardS) > 60);

  // A new generator: the colours start where they were and ease over frames (1 − e^(−8·dt)) to a fresh circle's.
  t.c->setSelection(hard);
  CHECK(t.c->sliceColour(0) == softL);
  CHECK(t.c->animating());
  t.d.step();
  t.d.advance(0.1);
  const Rgba midL = t.c->sliceColour(0);
  CHECK(midL != softL);
  CHECK(midL != hardL);
  CHECK(dist(midL, hardL) < dist(softL, hardL));
  t.d.advance(2.0);
  CHECK(t.c->sliceColour(0) == hardL);
  CHECK(t.c->sliceColour(sIdx) == hardS);
  CHECK_FALSE(t.c->animating());
  // Settled: no frame request, nothing damaged, nothing drawn.
  const uint64_t frames = t.d.framesDrawn(), paints = t.c->stats().paints;
  t.d.advance(1.0);
  CHECK(t.d.framesDrawn() == frames);
  CHECK(t.c->stats().paints == paints);
  CHECK_FALSE(t.d.tree().needsFrame());
}

TEST_CASE("mos circle (TU.2): a note-on lights its degree in its frequency's colour; a note-off fades it, then frames stop") {
  Circle t;
  t.set(selectionFor({5, 2}, 696.0));
  t.d.advance(0.5);
  REQUIRE(t.c->degreeCount() == 7);
  CHECK(t.c->glowOf(2) == 0.0f);

  t.c->noteOn(7, 2 + 7, 1400.0);  // degree 2 an equave up: wrapped onto degree 2
  t.c->noteOn(8, -3, 20.0);       // degree 4 an equave down
  t.d.advance(0.2);
  CHECK(t.c->glowOf(2) == doctest::Approx(1.0f));
  CHECK(t.c->glowOf(4) == doctest::Approx(1.0f));
  CHECK(t.c->glowOf(1) == 0.0f);
  CHECK(t.c->glowColourOf(2) == hex(0xFCDC40));  // ≥ 1050 Hz: yellow (the Phase Accumulator's colours)
  CHECK(t.c->glowColourOf(4) == hex(0xE83434));  // ≤ 30 Hz: red
  CHECK(t.c->noteHeld(7));
  // Held: it stays lit (sustain 1) and keeps animating.
  t.d.advance(1.0);
  CHECK(t.c->glowOf(2) == doctest::Approx(1.0f));
  CHECK(t.c->animating());

  // Release: the explorer's 1.5 s fade (the audio's perceptual decay, tapered), only on that degree.
  t.c->noteOff(7);
  CHECK_FALSE(t.c->noteHeld(7));
  t.d.advance(0.75);
  const float mid = t.c->glowOf(2);
  CHECK(mid > 0.1f);
  CHECK(mid < 0.5f);
  CHECK(t.c->glowOf(4) == doctest::Approx(1.0f));
  t.c->notesOff();
  t.d.advance(1.0);
  CHECK(t.c->glowOf(2) == 0.0f);
  CHECK(t.c->glowOf(4) < 0.5f);
  t.d.advance(1.0);
  CHECK(t.c->glowOf(4) == 0.0f);
  CHECK_FALSE(t.c->animating());
  const uint64_t frames = t.d.framesDrawn();
  t.d.advance(1.0);
  CHECK(t.d.framesDrawn() == frames);
  CHECK_FALSE(t.d.tree().needsFrame());
}

TEST_CASE("mos wheel: the summary's mode number is the circle header's (genchain order), not the rotation; L18") {
  doc::TuningDef def;
  tu::MosParams m;
  m.generator = 700.0;
  m.n = 7;
  m.rotation = 1;
  def.system = m;
  const std::string sum = tuningSummary(def);
  const auto gmn = tu::computeGmn(700.0, 7, 1, 1200.0);
  REQUIRE(gmn.has_value());
  CHECK(sum.find(tu::gmnLabel(*gmn)) != std::string::npos);
  CHECK(sum.find("mode 1") == std::string::npos);
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
  doc::TuningId tA1;  // TU.3: A1's own named tuning ("Tuning B", 12-EDO); A2 and B1 inherit the default 12-EDO

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
    tA1 = views::test::addTuning(st, doc::TuningDef{}, "Tuning B");
    REQUIRE(st.dispatch(doc::SetClipTuning{a1, doc::TuningAssignment::mainTuning(tA1)}).ok());
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

TEST_CASE("mos wheel: the clip box chip opens the wheel; a chart drag retunes the clip live, one undo step per drag") {
  Wheel w;
  CHECK(w.view().rows().degreesPerEquave() == 12);
  auto* chip = dynamic_cast<TuningChip*>(w.d.find("main/body/bottom/detail/clip/clip-view/clip-box/tuning"));
  REQUIRE(chip != nullptr);
  CHECK(chip->text() == "Tuning B \xC2\xB7 12");  // TU.3: the chip names the clip's named tuning (and what it is)
  MosWheelPopover& p = w.openFromChip();
  CHECK(p.target().kind == TuningTarget::Kind::Clip);
  CHECK(p.target().clip.clip == w.a1.clip);
  const doc::Snapshot before = w.ctx.doc();
  const size_t h0 = w.history();
  const doc::NoteClip* a2Before = w.clip(w.a2);
  const doc::NoteClip* b1Before = w.clip(w.b1);

  // Choosing 5L 2s applies at once (one undo step) to A1's named tuning: only A1 is on it.
  REQUIRE(p.chooseMos({5, 2}, 696.0));
  w.d.step();
  CHECK(w.history() == h0 + 1);
  REQUIRE(w.tuningOf(w.a1)->mos() != nullptr);
  CHECK(doc::findTuning(*w.ctx.doc(), w.tA1)->def.mos() != nullptr);  // TU.3: the shared tuning was edited
  CHECK(w.clip(w.a1)->tuning == doc::TuningAssignment::mainTuning(w.tA1));
  CHECK(w.clip(w.a2) == a2Before);  // pointer-equal: untouched
  CHECK(w.clip(w.b1) == b1Before);
  CHECK(w.tuningOf(w.b1)->edo() != nullptr);
  CHECK(w.view().rows().degreesPerEquave() == 7);
  CHECK(chip->text() == "Tuning B \xC2\xB7 5L 2s");

  // A chart drag (real pointer events): every step is in the document while the button is down — what the playback
  // link retunes sounding notes from — inside one store gesture; the release records one undo step.
  MosChart& chart = p.chart();
  const IRect ax = chart.axisRect();
  const float y = static_cast<float>(ax.y) + ax.h / 2.0f;
  w.d.down({chart.xOfCents(696.0), y});
  for (const double c : {700.5, 704.0, 708.0}) {
    w.d.move({chart.xOfCents(c), y}, mod::kAlt);
    w.d.step();
    const tu::MosParams* m = w.tuningOf(w.a1)->mos();
    REQUIRE(m != nullptr);
    CHECK(std::fabs(m->generator - c) < 0.5);
    CHECK(w.ctx.store().inGesture());
    CHECK(w.history() == h0 + 1);
    CHECK(w.view().rows().degreesPerEquave() == 7);
  }
  w.d.up({chart.xOfCents(708.0), y}, PointerButton::Left, mod::kAlt);
  CHECK_FALSE(w.ctx.store().inGesture());
  CHECK(w.history() == h0 + 2);
  CHECK(*w.tuningOf(w.a1) == p.draft());
  CHECK(w.clip(w.a2) == a2Before);

  // Done closes and keeps it; undo steps back one drag, then the choice.
  w.d.click(w.d.center("overlay/mos-wheel/footer/done"));
  w.d.step();
  CHECK(w.popover() == nullptr);
  CHECK(w.history() == h0 + 2);
  REQUIRE(w.ctx.undo());
  CHECK(w.tuningOf(w.a1)->mos()->generator == 696.0);
  REQUIRE(w.ctx.undo());
  CHECK(w.ctx.doc() == before);
  CHECK(w.view().rows().degreesPerEquave() == 12);
}

TEST_CASE("mos wheel (TU.3): a chip's menu picks a named tuning or a New Tuning; the wheel edits the shared tuning, so "
          "every linked track and clip changes with it") {
  Wheel w;
  auto* chip = dynamic_cast<TuningChip*>(w.d.find("main/body/bottom/detail/clip/clip-view/clip-box/tuning"));
  REQUIRE(chip != nullptr);
  const size_t h0 = w.history();
  auto menuAt = [&](TuningChip& c) {
    w.d.click(w.d.center("main/body/bottom/detail/clip/clip-view/clip-box/tuning"), mod::kNone, PointerButton::Right);
    w.d.step();
    auto* m = dynamic_cast<Menu*>(w.shell->overlay().top());
    REQUIRE(m != nullptr);
    (void)c;
    return m;
  };
  // The menu lists Inherit (naming what it resolves to), every project tuning with its colour and summary, New Tuning.
  {
    Menu* m = menuAt(*chip);
    std::vector<std::string> labels;
    for (const MenuItem& it : m->items()) labels.push_back(it.label);
    REQUIRE(labels.size() >= 4u);
    CHECK(labels[0] == "Inherit (12-EDO)");
    CHECK(labels[1] == "12-EDO");
    CHECK(labels[2] == "Tuning B");
    CHECK(labels[3] == "New Tuning");
    CHECK(m->items()[2].checked);  // A1's assignment
    CHECK_FALSE(m->items()[0].checked);
    CHECK((m->items()[1].swatch >> 24) != 0u);
    CHECK(m->items()[2].shortcut == "12-EDO");  // the summary (its name differs)
    w.d.press("Escape");
  }
  // Picking 12-EDO (a real click) links A1 to the default, as B1 is: one undo step.
  (void)menuAt(*chip);
  views::test::clickMenuItem(w.d, w.shell->overlay(), "12-EDO");
  CHECK(w.history() == h0 + 1);
  CHECK(w.clip(w.a1)->tuning == doc::TuningAssignment::mainTuning(w.ctx.doc()->defaultTuning));
  CHECK(chip->text() == "12-EDO");
  // The wheel now edits 12-EDO: B1 (another track, inheriting the default) and A2 change with A1.
  {
    MosWheelPopover& p = w.openFromChip();
    REQUIRE(p.chooseEdo(19));
    w.d.press("Return");
    w.d.step();
  }
  CHECK(w.history() == h0 + 2);
  CHECK(w.tuningOf(w.a1)->edo()->n() == 19u);
  CHECK(w.tuningOf(w.a2)->edo()->n() == 19u);
  CHECK(w.tuningOf(w.b1)->edo()->n() == 19u);
  CHECK(doc::findTuning(*w.ctx.doc(), w.tA1)->def.edo()->n() == 12u);  // Tuning B is no longer A1's
  // New Tuning: a copy of the current one under the next letter, assigned to A1, one undo step; editing it changes A1
  // alone.
  const size_t tunings = w.ctx.doc()->tunings->size();
  (void)menuAt(*chip);
  views::test::clickMenuItem(w.d, w.shell->overlay(), "New Tuning");
  CHECK(w.history() == h0 + 3);
  REQUIRE(w.ctx.doc()->tunings->size() == tunings + 1);
  const doc::TuningSlot& added = *w.ctx.doc()->tunings->back();
  CHECK(added.name == "Tuning C");
  CHECK(added.def.edo()->n() == 19u);
  CHECK(w.clip(w.a1)->tuning == doc::TuningAssignment::mainTuning(added.id));
  CHECK(chip->text() == "Tuning C \xC2\xB7 19");
  {
    MosWheelPopover& p = w.openFromChip();
    REQUIRE(p.chooseEdo(31));
    w.d.press("Return");
    w.d.step();
  }
  CHECK(w.tuningOf(w.a1)->edo()->n() == 31u);
  CHECK(w.tuningOf(w.b1)->edo()->n() == 19u);
  // Inherit: back to the track's tuning.
  (void)menuAt(*chip);
  views::test::clickMenuItem(w.d, w.shell->overlay(), "Inherit");
  CHECK(w.clip(w.a1)->tuning.mode == doc::TuningMode::Inherit);
  CHECK(w.tuningOf(w.a1)->edo()->n() == 19u);
}

TEST_CASE("mos wheel: Esc during a drag restores where it started; Revert; Esc, Enter and a press outside close") {
  Wheel w;
  const doc::Snapshot before = w.ctx.doc();
  const size_t h0 = w.history();
  {
    // Esc during a circle (hardness) drag cancels the drag's gesture: the document is as before the press.
    MosWheelPopover& p = w.openFromChip();
    REQUIRE(p.chooseMos({5, 2}, 696.0));
    const doc::Snapshot chosen = w.ctx.doc();
    const IRect sl = p.circle().geometry().slider;
    const float y = static_cast<float>(sl.y) + sl.h / 2.0f;
    w.d.down({static_cast<float>(sl.x) + 2.0f, y});
    w.d.move({static_cast<float>(sl.x) + sl.w * 0.8f, y});
    w.d.step();
    CHECK(w.tuningOf(w.a1)->mos()->generator != 696.0);  // live
    w.d.press("Escape");
    w.d.up({static_cast<float>(sl.x) + sl.w * 0.8f, y});
    w.d.step();
    REQUIRE(w.popover() == &p);
    CHECK(w.ctx.doc() == chosen);
    CHECK(p.draft().mos()->generator == 696.0);
    CHECK(w.history() == h0 + 1);
    // Revert: the tuning it had when the wheel opened, one more step.
    CHECK(p.changed());
    w.d.click(w.d.center("overlay/mos-wheel/footer/revert"));
    w.d.step();
    CHECK(w.tuningOf(w.a1)->edo() != nullptr);
    CHECK_FALSE(p.changed());
    CHECK(w.history() == h0 + 2);
    w.d.press("Escape");  // Esc closes; nothing is discarded (there is no draft)
    w.d.step();
    CHECK(w.popover() == nullptr);
    CHECK(w.history() == h0 + 2);
  }
  {
    MosWheelPopover& p = w.openFromChip();
    REQUIRE(p.chooseEdo(31));
    w.d.press("Return");
    w.d.step();
    CHECK(w.popover() == nullptr);
    CHECK(w.history() == h0 + 3);
    CHECK(w.tuningOf(w.a1)->edo()->n() == 31u);
    // Opening and closing without a change records nothing; a press outside closes.
    (void)w.openFromChip();
    w.d.click({40.0f, 880.0f});
    w.d.step();
    CHECK(w.popover() == nullptr);
    CHECK(w.history() == h0 + 3);
  }
  while (w.history() > h0) REQUIRE(w.ctx.undo());
  CHECK(w.ctx.doc() == before);
}

TEST_CASE("mos wheel: EDO entry, Scala file, the temperament search (no dropdown)") {
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
    CHECK(w.history() == h0 + 1);  // applied at once
    CHECK(w.tuningOf(w.a1)->edo()->n() == 22u);
    CHECK(w.view().rows().degreesPerEquave() == 22);
  }
  SUBCASE("Scala file through the file dialog") {
    w.dialogs.scalaPath = std::string(PQ_TEST_DATA_DIR) + "/tuning/fixtures/scl/bp_just.scl";
    MosWheelPopover& p = w.openFromChip();
    w.d.click(w.d.center("overlay/mos-wheel/header/scala"));
    CHECK(w.dialogs.scalas == 1);
    REQUIRE(p.draft().scale() != nullptr);
    CHECK(p.draft().scale()->scale.pitches.size() == 13u);
    CHECK(p.draft().referenceHz == p.original().referenceHz);
    CHECK(w.history() == h0 + 1);
    CHECK(w.tuningOf(w.a1)->scale() != nullptr);
    // A missing file changes nothing and says so.
    const doc::TuningDef d0 = p.draft();
    CHECK_FALSE(p.chooseScalaFile("no/such/file.scl").ok());
    CHECK(p.draft() == d0);
    CHECK(w.history() == h0 + 1);
    w.d.press("Escape");
  }
  SUBCASE("temperaments: typed and found; several matches open a short menu; every preset still reachable") {
    CHECK(w.d.find("overlay/mos-wheel/header/presets") == nullptr);  // the 80-item dropdown is gone
    MosWheelPopover& p = w.openFromChip();
    const auto temps = tu::temperaments();
    int32_t meantone = -1;
    for (size_t i = 0; i < temps.size(); ++i)
      if (temps[i].name == "Meantone") meantone = static_cast<int32_t>(i);
    REQUIRE(meantone >= 0);
    // Typed (real key events): an exact name is chosen at once.
    w.d.click(w.d.center("overlay/mos-wheel/header/temperament"));
    w.d.type("meantone");
    w.d.press("Return");
    w.d.step();
    const tu::MosParams* m = p.draft().mos();
    REQUIRE(m != nullptr);
    CHECK(m->n == 7);
    CHECK(m->generator == doctest::Approx((temps[static_cast<size_t>(meantone)].genLow + temps[static_cast<size_t>(meantone)].genHigh) / 2.0));
    REQUIRE(p.chart().selection().has_value());
    CHECK(p.chart().selection()->key == tu::MosKey{5, 2});
    CHECK(w.history() == h0 + 1);
    CHECK(*w.tuningOf(w.a1) == p.draft());
    // Matching: case-insensitive, exact first, then prefixes, then the rest; a few letters give a short list.
    const std::vector<int32_t> mean = MosWheelPopover::matchTemperaments("MEAN");
    REQUIRE_FALSE(mean.empty());
    CHECK(std::find(mean.begin(), mean.end(), meantone) != mean.end());
    CHECK(MosWheelPopover::matchTemperaments("e").size() <= 12u);
    CHECK(MosWheelPopover::matchTemperaments("zzzz").empty());
    // Several matches: a menu of them over the wheel; choosing one applies it.
    ui::OverlayLayer& layer = w.shell->overlay();
    const std::vector<int32_t> several = MosWheelPopover::matchTemperaments("a");
    REQUIRE(several.size() > 1);
    REQUIRE(p.findTemperament("a"));
    w.d.step();
    auto* menu = dynamic_cast<ui::Menu*>(layer.top());
    REQUIRE(menu != nullptr);
    CHECK(menu->items().size() == several.size());
    menu->choose(0);
    w.d.step();
    CHECK(w.popover() == &p);
    CHECK(p.chart().selection().has_value());
    // Every enabled preset is still reachable by name.
    int found = 0;
    for (size_t i = 0; i < temps.size(); ++i)
      if (!temps[i].disabled && p.chooseTemperament(static_cast<int32_t>(i))) ++found;
    CHECK(found > 10);
    w.d.press("Escape");
  }
}

TEST_CASE("mos wheel: a main tuning's generator drag is param writes (no Structural change); track targets; Ctrl+Shift+W") {
  Wheel w;
  const size_t h0 = w.history();
  SUBCASE("track") {
    MosWheelPopover* p = openMosWheel(w.shell->overlay(), w.ctx, TuningTarget::forTrack(w.trackA));
    REQUIRE(p != nullptr);
    REQUIRE(p->chooseMos({5, 2}, 696.0));
    CHECK(w.history() == h0 + 1);
    // TU.3: track A inherits the default tuning, so the edit is the default's: track B (linked to it) changes too,
    // A1 (its own tuning) does not; nobody's assignment changes.
    CHECK(doc::findTrackPtr(*w.ctx.doc(), w.trackA)->tuning.mode == doc::TuningMode::Inherit);
    CHECK(doc::defaultTuningOf(*w.ctx.doc())->def.mos() != nullptr);
    CHECK(w.tuningOf(w.a2)->mos() != nullptr);
    CHECK(w.tuningOf(w.b1)->mos() != nullptr);
    CHECK(w.tuningOf(w.a1)->edo() != nullptr);
    p->apply();
    w.d.step();
    CHECK(w.history() == h0 + 1);
  }
  SUBCASE("main tuning") {
    const doc::TuningId main = w.ctx.doc()->tunings->front()->id;
    MosWheelPopover* p = openMosWheel(w.shell->overlay(), w.ctx, TuningTarget::forMain(main));
    REQUIRE(p != nullptr);
    CHECK(p->chooseEdo(19));
    CHECK(w.history() == h0 + 1);
    CHECK(w.tuningOf(w.b1)->edo()->n() == 19u);  // everything inheriting the main tuning
    REQUIRE(p->chooseMos({5, 2}, 696.0));
    CHECK(w.history() == h0 + 2);
    // Drag the generator: every step is ONE Param-only change of MosGenerator (SetTuningParam: the playback link
    // writes the tuning node's param, no recompile, tests/app/test_tuning_live.cpp); one undo step at the release.
    std::vector<doc::ChangeSet> changes;
    auto sub = w.ctx.subscribe([&](const doc::StoreEvent& e) {
      if (e.kind != doc::StoreEventKind::GestureCommit) changes.push_back(e.changes);
    });
    w.d.tree().layoutNow();
    MosChart& chart = p->chart();
    const IRect ax = chart.axisRect();
    const float y = static_cast<float>(ax.y) + ax.h / 2.0f;
    w.d.down({chart.xOfCents(696.0), y});
    int steps = 0;
    for (double c = 697.0; c <= 712.0; c += 1.5) {
      w.d.move({chart.xOfCents(c), y}, mod::kAlt);
      w.d.step();
      ++steps;
      CHECK(std::fabs(w.ctx.doc()->tunings->front()->def.mos()->generator - c) < 0.3);
    }
    w.d.up({chart.xOfCents(712.0), y}, PointerButton::Left, mod::kAlt);
    REQUIRE(changes.size() >= static_cast<size_t>(steps));
    for (const doc::ChangeSet& c : changes) {
      CHECK(c.paramOnly());
      CHECK_FALSE(c.structural());
      REQUIRE(c.params.size() == 1u);
      CHECK(c.params.front().kind == doc::ParamKind::MosGenerator);
    }
    CHECK(w.history() == h0 + 3);
    p->apply();
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
    // Without a clip in the clip view: the project's default tuning.
    w.ctx.setDetailClip({});
    CHECK(defaultTuningTarget(w.ctx).kind == TuningTarget::Kind::Project);
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

TEST_CASE("mos wheel (TU.2): the clip playing, the computer keyboard and a roll audition light the circle's degrees") {
  Wheel w;
  MosWheelPopover& p = w.openFromChip();
  REQUIRE(p.chooseMos({5, 2}, 696.0));
  w.d.step();
  MosCircle& c = p.circle();
  REQUIRE(c.degreeCount() == 7);

  // The computer keyboard into the auto-armed track A, whose live clip is A1 (the clip view's): 'd' = degree 2.
  w.ctx.setSelectedTrack(w.trackA);
  REQUIRE(w.ctx.autoArmTrack() == w.trackA);
  ComputerKeyboard& kb = w.ctx.computerKeyboard();
  kb.setActive(true);
  KeyEvent k;
  k.key = 'd';
  REQUIRE(kb.onTreeKey(k));
  w.d.advance(0.2);
  CHECK(c.glowOf(2) == doctest::Approx(1.0f));
  CHECK(c.glowOf(0) == 0.0f);
  k.down = false;
  REQUIRE(kb.onTreeKey(k));
  w.d.advance(2.0);
  CHECK(c.glowOf(2) == 0.0f);
  kb.setActive(false);

  // An audition of A1's degree 4 (the roll's ViewContext::audition) lights it for its length; B1's (12-EDO) does not.
  w.ctx.audition(440.0f, 100.0f, 300.0f, w.a1, 4);
  w.d.advance(0.1);
  CHECK(c.glowOf(4) == doctest::Approx(1.0f));
  w.ctx.releaseAuditions();
  w.d.advance(2.0);
  CHECK(c.glowOf(4) == 0.0f);
  w.ctx.audition(440.0f, 100.0f, 300.0f, w.b1, 4);
  w.d.advance(0.1);
  CHECK(c.glowOf(4) == 0.0f);

  // Playback: A1's notes are degrees 0, 2, 4 on beats 1, 2, 3; each lights while the playhead is in it.
  w.ctx.play();
  REQUIRE(w.port.playhead().playing);
  w.port.setPlayheadTick(kPpq + kPpq / 2);
  w.d.advance(0.2);
  CHECK(c.glowOf(2) == doctest::Approx(1.0f));
  CHECK(c.glowOf(0) == 0.0f);
  CHECK(c.glowOf(4) == 0.0f);
  w.port.setPlayheadTick(2 * kPpq + kPpq / 4);
  w.d.advance(0.3);
  CHECK(c.glowOf(4) == doctest::Approx(1.0f));
  CHECK(c.glowOf(2) > 0.0f);  // releasing
  CHECK(c.glowOf(2) < 1.0f);
  w.ctx.stopButton();
  w.d.advance(2.0);
  CHECK(c.glowOf(4) == 0.0f);
  CHECK(c.glowOf(2) == 0.0f);
  CHECK_FALSE(c.animating());
  w.d.press("Escape");
}

TEST_CASE("mos wheel: the popover never covers the control bar or the status bar (L17), also when dragged up") {
  Wheel w;
  MosWheelPopover::forgetPosition();
  const IRect bar = w.d.find("main/control-bar")->rect();
  const IRect body = w.d.find("main/body")->rect();
  // Anchored at the top of the window (the golden scene's anchor, y 36): placed below the control bar instead.
  MosWheelPopover* p = openMosWheel(w.shell->overlay(), w.ctx, TuningTarget::forClip(w.a1), {600, 36, 1, 1});
  REQUIRE(p != nullptr);
  w.d.step();
  CHECK(p->rect().y >= bar.bottom());
  CHECK(p->rect().bottom() <= body.bottom());
  // The title band dragged to the top of the window (real pointer events): it stops at the workspace's top edge.
  const Vec2 grab{static_cast<float>(p->rect().x + 200), static_cast<float>(p->rect().y + 10)};
  w.d.down(grab);
  w.d.move({grab.x, 2.0f});
  w.d.up({grab.x, 2.0f});
  w.d.step();
  CHECK(p->rect().y == body.y);
  // Reopened at the remembered (dragged) position: still clear of the control bar.
  w.d.press("Escape");
  w.d.step();
  MosWheelPopover& q = w.openFromChip();
  CHECK(q.rect().y >= bar.bottom());
  w.d.press("Escape");
  MosWheelPopover::forgetPosition();
}

// ---- goldens ----------------------------------------------------------------------------------------------------------

namespace {

// `scale` ≠ 1: the golden is <scene>-<scale>x.png (T3.x-fix-edit: HiDPI goldens, M3 review L20).
void checkGolden(const char* scene, float scale = 1.0f, const char* suffix = "") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, {}, w, h, UiScale(scale), o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string name = std::string(scene) + suffix;
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(name + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(name, *img, *ref, true, &d));
  MESSAGE(name << ": " << pq::ui::test::describe(d));
}

}  // namespace

TEST_CASE("render: mos-circle-5L2s (18\\31, Lydian root) matches the golden image") { checkGolden("mos-circle-5L2s"); }

TEST_CASE("render: mos-circle-modmos (7\\12 Aeolian, raised 7th) matches the golden image") { checkGolden("mos-circle-modmos"); }

TEST_CASE("render: mos-circle-lit (5L 2s, four notes sounding in their frequency colours) matches the golden image") {
  checkGolden("mos-circle-lit");
}

TEST_CASE("render: mos-wheel-popover (clip wheel over the roll preview) matches the golden image") { checkGolden("mos-wheel-popover"); }

TEST_CASE("render: mos-wheel-popover at 150 % UI scale matches the golden image") { checkGolden("mos-wheel-popover", 1.5f, "-1.5x"); }
