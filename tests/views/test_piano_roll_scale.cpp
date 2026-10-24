// T3.12 piano roll scale / MOS layouts: RowModel's Proportional and Equidistant layouts against mos-roll.js run under
// Node (tests/views/fixtures/roll_layout.json, tools/fixtures/gen_roll_layout_fixtures.mjs: 50 (generator, size,
// height) cases × both layouts, ±0.5 px), step classes and z, retune == rebuild, the layout switch keeping the centred
// row, the background EDO overlay (display, Cents snapping, Alt-draw), the live generator morph (in place inside a
// MosLinearTable interval, rebuilt at crossings, Degree notes on their rows every frame, one undo step, Esc-free
// restore), and the goldens pianoroll-5L2s-prop / -equi / -near-collapse / -over-31edo / pianoroll-scala.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <ostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pq/document/commands.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/piano_roll/row_model.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::roll;
using Json = nlohmann::json;

namespace {

tuning::Tuning mosTuning(const tuning::MosParams& p) {
  auto m = tuning::Mos::make(p);
  REQUIRE(m.ok());
  auto t = tuning::Tuning::make(*m);
  REQUIRE(t.ok());
  return *t;
}

tuning::MosParams fiveTwo(double generator) {
  tuning::MosParams p;
  p.generator = generator;
  p.n = 7;
  p.period = 1200.0;
  p.rotation = 4;
  p.region = tuning::RegionHint{7, 1200.0 * 4 / 7, 720.0};
  return p;
}

// The shell over a fixture project with its first clip in the clip view (as test_piano_roll.cpp's Roll).
struct Roll {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ClipView* view = nullptr;
  doc::ClipRef ref{};

  explicit Roll(std::string_view fixture) {
    REQUIRE(ctx.openProject(views::test::fixtureDir(fixture)).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(520.0f);
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!ref.clip.valid() && t.kind == doc::TrackKind::Midi && !t.clips.empty()) ref = {t.id, t.clips.front().id};
    });
    REQUIRE(ref.clip.valid());
    ctx.setDetailClip(ref);
    d.tree().layoutNow();
    view = dynamic_cast<ClipView*>(d.find("main/body/bottom/detail/clip/clip-view"));
    REQUIRE(view != nullptr);
    REQUIRE(view->hasClip());
  }
  ~Roll() { d.tree().setRoot(std::make_unique<Widget>()); }

  const doc::NoteClip& clip() const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), ref.track);
    REQUIRE(t != nullptr);
    const int32_t i = doc::findClipIndex(*t, ref.clip);
    REQUIRE(i >= 0);
    return *t->clips[static_cast<size_t>(i)].clip;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  IRect area() const { return d.find("main/body/bottom/detail/clip/clip-view/notes")->rect(); }
  // Every visible Degree note's rect lies on its row's block (the row of its degree in the current rows).
  int checkDegreeNotesOnRows() const {
    const RowModel& rows = view->rows();
    const int32_t n = rows.degreesPerEquave();
    int checked = 0;
    for (const doc::Note& note : clip().notes) {
      if (note.pitch.kind() != tuning::PitchKind::Degree) continue;
      const int64_t deg = static_cast<int64_t>(note.pitch.equave()) * n + note.pitch.degree();
      const PitchRow* row = rows.rowOfDegree(deg);
      REQUIRE(row != nullptr);
      const IRect r = view->noteRect(note.id);
      if (r.empty()) continue;
      const int32_t centreY = static_cast<int32_t>(view->yOfDegree(deg));
      const int32_t top = centreY - (row->centre - row->top), bottom = centreY + (row->bottom - row->centre);
      const IRect a = area();
      CHECK(r.y == std::max(top, a.y));
      CHECK(r.bottom() == std::min(bottom, a.bottom()));
      ++checked;
    }
    return checked;
  }
};

}  // namespace

// ---- row model vs mos-roll.js ---------------------------------------------------------------------------

TEST_CASE("row model: Proportional / Equidistant layouts match mos-roll.js layoutRows (50 cases x 2 layouts, ±0.5 px)") {
  std::ifstream in(std::string(PQ_TEST_DATA_DIR) + "/views/fixtures/roll_layout.json", std::ios::binary);
  REQUIRE(in.good());
  const Json fx = Json::parse(in);
  const Json& cases = fx.at("cases");
  REQUIRE(cases.size() == 100u);
  size_t compared = 0, proportional = 0, gaps = 0, slivers = 0;
  for (size_t c = 0; c < cases.size(); ++c) {
    const Json& k = cases[c];
    CAPTURE(c);
    tuning::MosParams p;
    p.generator = k.at("generator").get<double>();
    p.n = k.at("n").get<int32_t>();
    p.period = k.at("period").get<double>();
    p.periodsPerEquave = k.at("periodsPerEquave").get<int32_t>();
    p.rotation = k.at("rotation").get<int32_t>();
    p.modmos = k.at("modmos").get<std::vector<double>>();
    const Json& rg = k.at("region");
    p.region = tuning::RegionHint{rg.at("perPeriodSize").get<int32_t>(), rg.at("lowCents").get<double>(),
                                  rg.at("highCents").get<double>()};
    const tuning::Tuning t = mosTuning(p);
    const bool equi = k.at("equidistant").get<bool>();
    const double equave = k.at("equave").get<double>();
    RowOptions o;
    o.layout = equi ? RowLayout::Equidistant : RowLayout::Proportional;
    o.pxPerCent = k.at("pxPerCent").get<double>();
    o.lowCents = -1.5 * equave;
    o.highCents = 3.5 * equave;
    const RowModel m = RowModel::build(t, o);
    REQUIRE(m.layout() == o.layout);
    CHECK(m.degreesPerEquave() == k.at("size").get<int32_t>());
    CHECK(std::fabs(m.pxPerCent() * equave / m.degreesPerEquave() - k.at("laneH").get<double>()) < 1e-5);
    if (!equi) ++proportional;
    // Geometry relative to degree 0 (both sides place the content origin differently).
    const Json* zero = nullptr;
    for (const Json& r : k.at("rows"))
      if (r[0].get<int64_t>() == 0) zero = &r;
    REQUIRE(zero != nullptr);
    const int64_t i0 = m.indexOfDegree(0);
    REQUIRE(i0 >= 0);
    const double jsY0 = (*zero)[2].get<double>(), cY0 = m.exactCentreY(static_cast<size_t>(i0));
    for (const Json& r : k.at("rows")) {
      const int64_t deg = r[0].get<int64_t>();
      const int64_t i = m.indexOfDegree(deg);
      REQUIRE(i >= 0);
      const PitchRow& row = m.row(static_cast<size_t>(i));
      CAPTURE(deg);
      CHECK(std::fabs(row.cents - r[1].get<double>()) < 1e-5);
      const double y = r[2].get<double>() - jsY0 + cY0, top = r[3].get<double>() - jsY0 + cY0,
                   bottom = r[4].get<double>() - jsY0 + cY0;
      CHECK(std::fabs(m.exactCentreY(static_cast<size_t>(i)) - y) < 1e-6);
      CHECK(std::fabs(row.top - top) <= 0.5 + 1e-6);
      CHECK(std::fabs(row.bottom - bottom) <= 0.5 + 1e-6);
      CHECK(std::fabs(row.centre - y) <= 0.5 + 1e-6);
      if (!equi && i > 0 && m.row(static_cast<size_t>(i - 1)).bottom < row.top) ++gaps;
      if (!equi && r[4].get<double>() - r[3].get<double>() < 4.0) ++slivers;
      ++compared;
    }
  }
  MESSAGE(compared << " rows compared; proportional: " << gaps << " gaps between rows, " << slivers << " slivers (< 4 px)");
  CHECK(proportional == 50u);
  CHECK(compared > 2000u);
  CHECK(gaps > 0u);     // the interval shows as space
  CHECK(slivers > 0u);  // near-collapse cases close their small steps
}

TEST_CASE("row model: 5L 2s step classes, hardness, z (s under L); a collapsing MOS; retune == rebuild; Scala order") {
  RowOptions o;
  o.layout = RowLayout::Proportional;
  o.rowPx = 34;
  o.lowCents = -1200.0;
  o.highCents = 2400.0;
  const RowModel m = RowModel::build(mosTuning(fiveTwo(1200.0 * 18 / 31)), o);
  REQUIRE(m.stepClasses().size() == 2u);
  CHECK(m.stepClasses()[0].cents == doctest::Approx(193.55).epsilon(1e-4));  // L
  CHECK(m.stepClasses()[1].cents == doctest::Approx(116.13).epsilon(1e-4));  // s
  CHECK(m.hardness() == doctest::Approx(193.55 / 116.13).epsilon(1e-3));
  CHECK(m.laneHeight() == 34);
  // Ionian: steps L L s L L L s above degrees 0 … 6.
  const int expectSmall[7] = {0, 0, 1, 0, 0, 0, 1};
  for (int d = 0; d < 7; ++d) {
    const PitchRow* r = m.rowOfDegree(d);
    REQUIRE(r != nullptr);
    CHECK(r->stepClass == expectSmall[d]);
    CHECK(r->z == (expectSmall[d] ? 0 : 1));
  }
  // Near collapse (s → 0 at 3\5): the two rows across each s step close up to half blocks facing each other, the
  // L rows keep the full nominal block.
  const RowModel c = RowModel::build(mosTuning(fiveTwo(719.9)), o);
  CHECK(c.hardness() > 100.0);  // s = 0.4 ¢: fully "hard" (softness 0)
  const PitchRow* below = c.rowOfDegree(2);  // the s step 2 → 3
  const PitchRow* above = c.rowOfDegree(3);
  const PitchRow* mid = c.rowOfDegree(1);
  REQUIRE((below && above && mid));
  CHECK(below->centre - below->top <= 1);
  CHECK(above->bottom - above->centre <= 1);
  CHECK(mid->height() >= 20);  // nominal min(22, 0.62 · 34) = 21.08
  // retune() of the 18\31 model to 700 ¢ equals a fresh build at 700 ¢.
  RowModel r = RowModel::build(mosTuning(fiveTwo(1200.0 * 18 / 31)), o);
  r.retune(mosTuning(fiveTwo(700.0)));
  const RowModel b = RowModel::build(mosTuning(fiveTwo(700.0)), o);
  REQUIRE(r.size() == b.size());
  for (size_t i = 0; i < r.size(); ++i) {
    CHECK(r.row(i).cents == doctest::Approx(b.row(i).cents));
    CHECK(r.row(i).top == b.row(i).top);
    CHECK(r.row(i).bottom == b.row(i).bottom);
    CHECK(r.row(i).z == b.row(i).z);
  }
  // A Scala scale out of pitch order cannot be laid out proportionally: Equidistant instead.
  auto scl = tuning::parseScl("! odd.scl\nodd\n 3\n!\n 700.0\n 300.0\n 1200.0\n");
  REQUIRE(scl.ok());
  auto sc = tuning::Scale::make(*scl);
  REQUIRE(sc.ok());
  auto st = tuning::Tuning::make(*sc);
  REQUIRE(st.ok());
  CHECK(RowModel::build(*st, o).layout() == RowLayout::Equidistant);
  // EDOs stay Uniform whatever is asked.
  auto e = tuning::Edo::make(31);
  REQUIRE(e.ok());
  auto et = tuning::Tuning::make(*e);
  REQUIRE(et.ok());
  CHECK(RowModel::build(*et, o).layout() == RowLayout::Uniform);
}

// ---- clip view ------------------------------------------------------------------------------------------

TEST_CASE("clip view: a 5L 2s clip opens Proportional; switching layouts keeps the centred row; one row per degree") {
  Roll r("pianoroll-5L2s");
  CHECK(r.view->scaleLayout() == RowLayout::Proportional);
  CHECK(r.view->rows().layout() == RowLayout::Proportional);
  CHECK(r.view->rows().degreesPerEquave() == 7);
  const IRect a = r.area();
  auto centredDegree = [&] {
    const RowModel& rows = r.view->rows();
    int64_t best = 0;
    float bestD = 1e30f;
    for (const PitchRow& row : rows.rows()) {
      const float y = r.view->yOfDegree(row.degree);
      if (std::fabs(y - (a.y + a.h * 0.5f)) < bestD) {
        bestD = std::fabs(y - (a.y + a.h * 0.5f));
        best = row.degree;
      }
    }
    return best;
  };
  const int64_t before = centredDegree();
  r.view->setScaleLayout(RowLayout::Equidistant);
  CHECK(r.view->rows().layout() == RowLayout::Equidistant);
  CHECK(centredDegree() == before);
  CHECK(std::fabs(r.view->yOfDegree(before) - (a.y + a.h * 0.5f)) <= r.view->rows().laneHeight());
  r.view->setScaleLayout(RowLayout::Proportional);
  CHECK(centredDegree() == before);
  CHECK(r.checkDegreeNotesOnRows() > 5);
  // The clip box shows the scale controls (and the EDO clip views do not: test_piano_roll's goldens are unchanged).
  CHECK(r.d.find("main/body/bottom/detail/clip/clip-view/clip-box/layout-row")->visible());
  CHECK(r.d.find("main/body/bottom/detail/clip/clip-view/clip-box/generator-row")->visible());
}

TEST_CASE("clip view: background EDO — Up on a Cents note steps on its lines; Alt-draw puts Cents notes on them") {
  Roll r("pianoroll-5L2s");
  r.view->setOverlayEdo(31);
  CHECK(r.view->overlayEdo() == 31);
  // The Cents note at 13\31 (503.2 ¢): Up → 14\31.
  const doc::Note* cents = nullptr;
  for (const doc::Note& n : r.clip().notes)
    if (n.pitch.kind() == tuning::PitchKind::Cents) cents = &n;
  REQUIRE(cents != nullptr);
  const doc::NoteId id = cents->id;
  r.view->select(std::span<const doc::NoteId>(&id, 1));
  const size_t h0 = r.history();
  r.view->stepSelection(1, doc::ops::StepUnit::Grid);
  const doc::Note* after = r.clip().notes.findById(id);
  REQUIRE(after != nullptr);
  CHECK(after->pitch.cents() == doctest::Approx(1200.0 * 14 / 31).epsilon(1e-12));
  CHECK(r.history() == h0 + 1);
  // Without the overlay the next line is the next scale degree (degree 4 = 696.8 ¢).
  r.view->setOverlayEdo(0);
  r.view->stepSelection(1, doc::ops::StepUnit::Grid);
  CHECK(r.clip().notes.findById(id)->pitch.cents() == doctest::Approx(1200.0 * 18 / 31).epsilon(1e-12));
  // Alt + draw mode: a Cents note on the 31edo line nearest the pointer, not a Degree note.
  r.view->setOverlayEdo(31);
  r.ctx.setDrawMode(true);
  const IRect a = r.area();
  const double t = 1.55 * static_cast<double>(kPpq);
  const float y = (r.view->yOfDegree(1) + r.view->yOfDegree(2)) * 0.5f;  // between degrees 1 and 2: a gap
  const size_t before = r.clip().notes.size();
  r.d.click({r.view->xOfTick(t), y}, mod::kAlt);
  REQUIRE(r.clip().notes.size() == before + 1);
  bool found = false;
  for (const doc::Note& n : r.clip().notes)
    if (n.pitch.kind() == tuning::PitchKind::Cents && n.start <= static_cast<Ticks>(t) &&
        n.start + n.length > static_cast<Ticks>(t)) {
      const double steps = n.pitch.cents() / (1200.0 / 31);
      CHECK(std::fabs(steps - std::round(steps)) < 1e-9);
      CHECK(n.pitch.cents() > 1200.0 * 18 / 31 * 0 + 200.0);  // between degree 1 (193.5) and 2 (387.1)
      CHECK(n.pitch.cents() < 390.0);
      found = true;
    }
  CHECK(found);
  (void)a;
}

TEST_CASE("clip view: generator morph across region boundaries keeps Degree notes on their rows; one undo step") {
  Roll r("pianoroll-5L2s");
  const doc::NoteClip* before = &r.clip();
  const size_t h0 = r.history();
  CHECK(r.checkDegreeNotesOnRows() > 5);
  const uint64_t lin0 = r.view->morphLinearFrames(), reb0 = r.view->morphRebuilds();
  // Up through the collapsed end (3\5 = 720 ¢: 5L 2s → the adjacent region, a new size), frame by frame.
  r.view->beginGeneratorMorph();
  int frames = 0;
  int32_t sizeAfter = 7;
  for (double g = 1200.0 * 18 / 31; g <= 724.0; g += 0.37) {
    r.view->morphGenerator(g);
    r.d.step();
    CHECK(r.history() == h0);              // nothing dispatched while dragging
    CHECK(&r.clip() == before);            // the clip itself is untouched (the preview is view state)
    CHECK(r.checkDegreeNotesOnRows() > 0);  // every visible Degree note sits on its row, every frame
    sizeAfter = r.view->rows().degreesPerEquave();
    ++frames;
  }
  CHECK(frames > 60);
  CHECK(sizeAfter != 7);                                   // crossed into another MOS
  CHECK(r.view->morphLinearFrames() - lin0 > 50u);         // in place inside the MosLinearTable intervals
  CHECK(r.view->morphRebuilds() - reb0 >= 1u);             // rebuilt at the crossing
  CHECK(r.view->morphRebuilds() - reb0 < 10u);
  r.view->endGeneratorMorph(true);
  CHECK(r.history() == h0 + 1);                            // one undo step
  CHECK_FALSE(r.ctx.tuningPreview().has_value());
  const doc::NoteClip& committed = r.clip();
  REQUIRE(committed.tuning.mode == doc::TuningMode::Private);
  const tuning::MosParams* mp = committed.tuning.privateDef->mos();
  REQUIRE(mp != nullptr);
  CHECK(mp->n == sizeAfter);
  CHECK(r.view->rows().degreesPerEquave() == sizeAfter);
  CHECK(r.checkDegreeNotesOnRows() > 0);
  // The rows are the committed tuning's pitches.
  auto built = doc::buildTuning(*committed.tuning.privateDef);
  REQUIRE(built.ok());
  for (const PitchRow& row : r.view->rows().rows()) CHECK(row.cents == doctest::Approx(degreeCents(*built, row.degree)));
  r.d.press("Ctrl+Z");
  CHECK(r.history() == h0);
  CHECK(r.view->rows().degreesPerEquave() == 7);
  CHECK(r.checkDegreeNotesOnRows() > 5);

  // Down through the equalized end (4\7 = 685.7 ¢: 5L 2s → 2L 5s, same size, the mode carried); end(false) restores.
  r.view->beginGeneratorMorph();
  for (double g = 1200.0 * 18 / 31; g >= 676.0; g -= 0.41) {
    r.view->morphGenerator(g);
    r.d.step();
    CHECK(r.checkDegreeNotesOnRows() > 0);
  }
  REQUIRE(r.ctx.tuningPreview().has_value());
  const tuning::MosParams* pm = r.ctx.tuningPreview()->def.mos();
  REQUIRE(pm != nullptr);
  CHECK(pm->n == 7);
  CHECK(pm->generator < 685.0);
  CHECK(pm->region->highCents == doctest::Approx(1200.0 * 4 / 7));
  r.view->endGeneratorMorph(false);
  CHECK(r.history() == h0);
  CHECK_FALSE(r.ctx.tuningPreview().has_value());
  CHECK(r.view->rows().rowOfDegree(4)->cents == doctest::Approx(1200.0 * 18 / 31));
}

TEST_CASE("clip view: the clip box Generator box morphs on a drag and commits on release (one undo step)") {
  Roll r("pianoroll-5L2s");
  const size_t h0 = r.history();
  const char* path = "main/body/bottom/detail/clip/clip-view/clip-box/generator-row/generator";
  REQUIRE(r.d.find(path) != nullptr);
  const Vec2 c = r.d.center(path);
  r.d.move(c);
  r.d.down(c);
  for (int i = 1; i <= 10; ++i) {
    r.d.move({c.x, c.y - 4.0f * static_cast<float>(i)});
    CHECK(r.history() == h0);
  }
  REQUIRE(r.ctx.tuningPreview().has_value());
  const double g = r.ctx.tuningPreview()->def.mos()->generator;
  CHECK(g == doctest::Approx(1200.0 * 18 / 31 + 20.0).epsilon(1e-6));  // 40 px × 0.5 ¢
  CHECK(r.view->rows().rowOfDegree(4)->cents == doctest::Approx(g));
  r.d.up({c.x, c.y - 40.0f});
  CHECK(r.history() == h0 + 1);
  CHECK_FALSE(r.ctx.tuningPreview().has_value());
  CHECK(r.clip().tuning.privateDef->mos()->generator == doctest::Approx(g));
}

TEST_CASE("clip view: Generator box: Esc cancels the drag, Delete restores the committed value, the keys return to the notes (M12b)") {
  Roll r("pianoroll-5L2s");
  const char* path = "main/body/bottom/detail/clip/clip-view/clip-box/generator-row/generator";
  auto* gen = dynamic_cast<DragBox*>(r.d.find(path));
  REQUIRE(gen != nullptr);
  const doc::Snapshot before = r.ctx.doc();
  const size_t h0 = r.history();
  const double g0 = gen->value();
  // 1. Esc mid-drag: the preview goes, the document and history are unchanged, the box shows the old value.
  const Vec2 c = r.d.center(path);
  r.d.move(c);
  r.d.down(c);
  for (int i = 1; i <= 6; ++i) r.d.move({c.x, c.y - 5.0f * static_cast<float>(i)});
  REQUIRE(r.ctx.tuningPreview().has_value());
  CHECK(r.d.press("Escape") == KeyResult::Widget);
  CHECK_FALSE(r.ctx.tuningPreview().has_value());
  r.d.move({c.x, c.y - 60.0f});  // the rest of the press does nothing
  r.d.up({c.x, c.y - 60.0f});
  CHECK(r.ctx.doc() == before);
  CHECK(r.history() == h0);
  CHECK(gen->value() == doctest::Approx(g0));
  // 2. The default is the clip's committed generator (it was a fixed 700 ¢): Delete on the focused box is a no-op.
  CHECK(gen->spec().def == doctest::Approx(g0));
  r.d.tree().setFocus(gen);
  r.d.press("Delete");
  CHECK(r.history() == h0);
  // 3. After a committed drag the keys go back to the note area: Up transposes notes, not the generator.
  r.d.drag(c, {c.x, c.y - 20.0f}, 5);
  CHECK(r.history() == h0 + 1);
  const double g1 = r.clip().tuning.privateDef->mos()->generator;
  const Widget* notes = r.d.find("main/body/bottom/detail/clip/clip-view/notes");
  CHECK(r.d.tree().focus() == notes);
  r.d.press("Ctrl+A");
  r.d.press("Up");
  CHECK(r.clip().tuning.privateDef->mos()->generator == doctest::Approx(g1));  // unchanged: the notes moved
  CHECK(r.history() == h0 + 2);
}

// ---- goldens --------------------------------------------------------------------------------------------

TEST_CASE("render: scale / MOS piano roll goldens (5L 2s prop, equi, near collapse, over 31edo; Scala)") {
  const std::pair<const char*, const char*> scenes[] = {
      {"pianoroll-5L2s-prop", "pianoroll-5L2s"},
      {"pianoroll-5L2s-equi", "pianoroll-5L2s"},
      {"pianoroll-5L2s-near-collapse", "pianoroll-5L2s-near-collapse"},
      {"pianoroll-5L2s-over-31edo", "pianoroll-5L2s"},
      {"pianoroll-scala", "pianoroll-scala"},
  };
  for (const auto& [scene, fixture] : scenes) {
    CAPTURE(scene);
    uint32_t w = 0, h = 0;
    viewSceneDefaultSize(scene, w, h);
    GpuOptions o;
    o.validation = true;
    auto img = renderViewScene(scene, views::test::fixtureDir(fixture), w, h, {}, o);
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png");
    if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
    REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
    ImageDiff diff;
    CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &diff));
    MESSAGE(scene << ": " << pq::ui::test::describe(diff));
  }
}
