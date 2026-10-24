// T3.8 arrangement view: rows from the track tree (nested and folded groups), Ctrl+wheel zoom keeps the tick under
// the pointer, Z / X round trip, ruler click locates through the TransportPort, ruler drag zooms, scrolling,
// folding, follow, the playhead read only while playing (and damaging only its columns), virtualisation and the
// note-preview cache, the fixture project and the `arrangement-basic` / `arrangement-zoomed-out` goldens.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <ostream>
#include <sstream>
#include <string>

#include "pq/document/serialize.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
#include "arrangement_fixture.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr Ticks kBar = kTicksPerWhole;
constexpr const char* kArr = "main/body/top/track-area/arrangement";
constexpr const char* kRuler = "main/body/top/track-area/arrangement/ruler";
constexpr const char* kTracks = "main/body/top/track-area/arrangement/tracks";  // the rows' scroll view

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d;
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  explicit Fixture(bool content = true, uint32_t w = 1280, uint32_t h = 800, float scale = 1.0f) : d(w, h, scale) {
    if (content) REQUIRE(ctx.openProject(views::test::fixtureDir("arrangement")).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    d.tree().layoutNow();
    port.calls.clear();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }  // widgets before the context they observe
  Vec2 lanePoint(float fx, float fy) {
    const IRect lv = arr->lanesViewport();
    return {static_cast<float>(lv.x) + fx * static_cast<float>(lv.w), static_cast<float>(lv.y) + fy * static_cast<float>(lv.h)};
  }
};

std::string readText(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("arrangement: the fixture project is what buildArrangementProject makes") {
  doc::DocumentStore store;
  views::test::buildArrangementProject(store);
  auto text = doc::serializeProject(*store.current(), store.view());
  REQUIRE(text.ok());
  const std::string dir = views::test::fixtureDir("arrangement");
  if (const char* u = std::getenv("PQ_UPDATE_FIXTURES"); u && *u == '1') REQUIRE(doc::saveProject(dir, store).ok());
  std::string file = readText(dir + "/project.pqd");
  std::erase(file, '\r');  // git may check text files out with CRLF
  CHECK_MESSAGE(file == *text, "tests/views/fixtures/arrangement is stale: run pq_tests_views with PQ_UPDATE_FIXTURES=1");
}

TEST_CASE("arrangement: installed in the track area, rows follow the track tree") {
  Fixture f;
  CHECK(f.d.tree().focus() == f.arr);  // the arrangement's key scope is active from the start
  const auto rows = f.arr->rows();
  // Drums, Bass, Keys [Piano, Pads [Strings]], FX (folded: Riser hidden).
  REQUIRE(rows.size() == 7);
  const char* names[] = {"Drums", "Bass", "Keys", "Piano", "Pads", "Strings", "FX"};
  const int depth[] = {0, 0, 0, 1, 1, 2, 0};
  for (size_t i = 0; i < rows.size(); ++i) {
    CHECK(rows[i].track->name == names[i]);
    CHECK(rows[i].depth == depth[i]);
  }
  CHECK(rows[2].group);
  CHECK_FALSE(rows[2].folded);
  CHECK(rows[4].parent == 2);
  CHECK(rows[5].parent == 4);
  CHECK(rows[6].group);
  CHECK(rows[6].folded);
  CHECK(rows[0].h == 60);  // ViewState height (logical px at scale 1)
  CHECK(rows[1].h == 48);
  CHECK(rows[1].y == 60);
  // Headers are widgets with stable paths; the lanes are one widget.
  auto* h = dynamic_cast<TrackHeader*>(f.d.find(std::string(kTracks) + "/track[2]"));
  REQUIRE(h != nullptr);
  CHECK(h->trackId() == rows[2].id);
  CHECK(f.d.find(std::string(kTracks) + "/track[2]/fold")->visible());
  CHECK_FALSE(f.d.find(std::string(kTracks) + "/track[0]/fold")->visible());
  CHECK(f.d.find(std::string(kTracks) + "/lanes") != nullptr);
  // Headers sit right of the lanes (Live), each at its row.
  const IRect lanes = f.d.rect(std::string(kTracks) + "/lanes");
  const IRect hr = f.d.rect(std::string(kTracks) + "/track[2]");
  CHECK(hr.x > lanes.right());
  CHECK(hr.y == f.arr->rowLaneRect(2).y);
  CHECK(f.arr->rowAt(static_cast<float>(hr.y + 3)) == 2);
}

TEST_CASE("arrangement: folding a group hides its members (view state, not undoable)") {
  Fixture f;
  const size_t undo0 = f.ctx.store().history().cursor();
  f.d.click(f.d.center(std::string(kTracks) + "/track[2]/fold"));  // Keys
  CHECK(f.arr->rows().size() == 4);  // Drums, Bass, Keys, FX
  CHECK(f.arr->rows()[2].folded);
  CHECK(f.ctx.store().view().tracks.at(f.arr->rows()[2].id).folded);
  CHECK(f.ctx.store().history().cursor() == undo0);
  CHECK(f.d.find(std::string(kTracks) + "/track[4]") == nullptr);  // spare headers removed
  f.d.click(f.d.center(std::string(kTracks) + "/track[3]/fold"));  // FX unfolds: Riser appears
  REQUIRE(f.arr->rows().size() == 5);
  CHECK(f.arr->rows()[4].track->name == "Riser");
  CHECK(f.arr->rows()[4].depth == 1);
}

TEST_CASE("arrangement: Ctrl+wheel zoom keeps the tick under the cursor fixed (±1 px)") {
  Fixture f;
  f.arr->setTimeView(6.0 * kBar, f.arr->pxPerTick());
  for (const float fx : {0.1f, 0.37f, 0.5f, 0.83f}) {
    for (const float notches : {1.0f, 3.0f, -2.0f, -1.0f, 0.5f}) {
      const Vec2 p = f.lanePoint(fx, 0.4f);
      const double t0 = f.arr->tickAtX(p.x);
      const double z0 = f.arr->pxPerTick();
      f.d.wheel(p, notches, 0.0f, mod::kCtrl);
      CHECK(f.arr->pxPerTick() != doctest::Approx(z0));
      CHECK(std::abs(f.arr->xOfTick(t0) - p.x) <= 1.0f);
    }
  }
  // Over the ruler too.
  const IRect ruler = f.d.rect(kRuler);
  const Vec2 p{static_cast<float>(ruler.x + 300), static_cast<float>(ruler.y + 10)};
  const double t0 = f.arr->tickAtX(p.x);
  f.d.wheel(p, 2.0f, 0.0f, mod::kCtrl);
  CHECK(std::abs(f.arr->xOfTick(t0) - p.x) <= 1.0f);
}

TEST_CASE("arrangement: Ctrl+wheel zoom keeps the tick fixed at a fractional UI scale") {
  Fixture f(true, 1280, 800, 1.5f);
  f.arr->setTimeView(3.0 * kBar, f.arr->pxPerTick());
  const Vec2 p = f.lanePoint(0.61f, 0.3f);
  const double t0 = f.arr->tickAtX(p.x);
  for (int i = 0; i < 6; ++i) {
    f.d.wheel(p, i % 2 ? -1.0f : 2.0f, 0.0f, mod::kCtrl);
    CHECK(std::abs(f.arr->xOfTick(t0) - p.x) <= 1.0f);
  }
}

TEST_CASE("arrangement: Z / X round trip (and = / - zoom around the centre)") {
  Fixture f;
  f.arr->setTimeView(2.5 * kBar, 150.0 / kBar);
  const double o0 = f.arr->originTick(), z0 = f.arr->pxPerTick();
  CHECK(f.d.press("Z") == KeyResult::Action);
  // No time selection yet (T3.9): Z fits the whole arrangement.
  CHECK(f.arr->originTick() == 0.0);
  const TickRange vis = f.arr->visibleTicks();
  CHECK(std::abs(static_cast<double>(vis.end - f.arr->songEnd())) <= 1.0 / f.arr->devicePxPerTick() + 1.0);
  CHECK(f.d.press("X") == KeyResult::Action);
  CHECK(f.arr->originTick() == o0);
  CHECK(f.arr->pxPerTick() == z0);
  // With a time selection: Z fits it exactly; nested Z, X, X unwinds in order.
  f.arr->setTimeSelection(TickRange{4 * kBar, 6 * kBar});
  f.d.press("Z");
  CHECK(f.arr->originTick() == static_cast<double>(4 * kBar));
  CHECK(std::abs(f.arr->xOfTick(static_cast<double>(6 * kBar)) - static_cast<float>(f.arr->lanesViewport().right())) <= 1.0f);
  const double o1 = f.arr->originTick(), z1 = f.arr->pxPerTick();
  f.arr->setTimeSelection(TickRange{4 * kBar + kBar / 4, 4 * kBar + kBar / 2});
  f.d.press("Z");
  CHECK(f.arr->pxPerTick() > z1);
  f.d.press("X");
  CHECK(f.arr->originTick() == o1);
  CHECK(f.arr->pxPerTick() == z1);
  f.d.press("X");
  CHECK(f.arr->originTick() == o0);
  CHECK(f.arr->pxPerTick() == z0);
  CHECK_FALSE(f.arr->zoomBack());  // nothing left
  // = / -: ×2 / ×½ around the centre.
  const IRect lv = f.arr->lanesViewport();
  const float cx = static_cast<float>(lv.x) + static_cast<float>(lv.w) * 0.5f;
  const double tc = f.arr->tickAtX(cx);
  f.d.press("=");
  CHECK(f.arr->pxPerTick() == doctest::Approx(2.0 * z0));
  CHECK(std::abs(f.arr->xOfTick(tc) - cx) <= 1.0f);
  f.d.press("-");
  CHECK(f.arr->pxPerTick() == doctest::Approx(z0));
}

TEST_CASE("arrangement: a ruler click sets the start marker and locates via the port (snapped)") {
  Fixture f;
  const IRect ruler = f.d.rect(kRuler);
  // Bar 3 plus a little: the adaptive grid (1/4 at the default zoom) snaps it to 3.1.1.
  const float x = f.arr->xOfTick(static_cast<double>(2 * kBar + kPpq / 5));
  f.d.click({x, static_cast<float>(ruler.y + ruler.h / 2)});
  REQUIRE(f.port.count(FakeTransport::Call::Locate) == 1);
  CHECK(f.port.calls.back().tick == 2 * kBar);
  CHECK(f.ctx.startMarker() == 2 * kBar);
  // With snapping off it lands on the clicked tick.
  GridSpec g = f.ctx.grid();
  g.snap = false;
  f.ctx.setGrid(g);
  const Ticks t = 5 * kBar + 1234;
  f.d.click({f.arr->xOfTick(static_cast<double>(t)) + 0.5f, static_cast<float>(ruler.y + 5)});
  REQUIRE(f.port.count(FakeTransport::Call::Locate) == 2);
  CHECK(std::abs(f.port.calls.back().tick - t) <= static_cast<Ticks>(1.0 / f.arr->devicePxPerTick()) + 1);
  // Space then plays from there.
  f.port.calls.clear();
  f.d.press("Space");
  REQUIRE(f.port.count(FakeTransport::Call::Play) == 1);
  CHECK(f.port.calls.back().tick == f.ctx.startMarker());
}

TEST_CASE("arrangement: a ruler drag zooms (vertical) around the pressed tick and does not locate") {
  Fixture f;
  f.arr->setTimeView(4.0 * kBar, f.arr->pxPerTick());
  const IRect ruler = f.d.rect(kRuler);
  const Vec2 from{static_cast<float>(ruler.x + 400), static_cast<float>(ruler.y + 10)};
  const double t0 = f.arr->tickAtX(from.x);
  const double z0 = f.arr->pxPerTick();
  f.d.drag(from, {from.x, from.y + 80.0f}, 8);  // down = zoom in
  CHECK(f.arr->pxPerTick() > z0 * 1.5);
  CHECK(std::abs(f.arr->xOfTick(t0) - from.x) <= 1.0f);
  CHECK(f.port.count(FakeTransport::Call::Locate) == 0);
  // Horizontal drag scrolls: the pressed tick follows the pointer.
  const double z1 = f.arr->pxPerTick();
  const Vec2 a{static_cast<float>(ruler.x + 500), static_cast<float>(ruler.y + 10)};
  const double ta = f.arr->tickAtX(a.x);
  f.d.drag(a, {a.x - 200.0f, a.y}, 6);
  CHECK(f.arr->pxPerTick() == doctest::Approx(z1));
  CHECK(std::abs(f.arr->xOfTick(ta) - (a.x - 200.0f)) <= 1.0f);
}

TEST_CASE("arrangement: wheel scrolls the tracks vertically, Shift+wheel scrolls time") {
  Fixture f(true, 1280, 600);  // short: the rows overflow
  const Vec2 p = f.lanePoint(0.5f, 0.5f);
  CHECK(f.arr->scrollY() == 0);
  f.d.wheel(p, -1.0f);
  CHECK(f.arr->scrollY() > 0);
  const double o0 = f.arr->originTick();
  f.d.wheel(p, -2.0f, 0.0f, mod::kShift);
  CHECK(f.arr->originTick() > o0);
  f.d.wheel(p, 10.0f, 0.0f, mod::kShift);
  CHECK(f.arr->originTick() == 0.0);  // clamped at the start
}

TEST_CASE("arrangement: the overview moves the view") {
  Fixture f;
  const IRect ov = f.d.rect(std::string(kArr) + "/overview");
  const double len = static_cast<double>(f.arr->songEnd() + 4 * kBar);
  // Click far right of the frame: the view centres there.
  const float x = static_cast<float>(ov.x) + static_cast<float>(ov.w) * 0.8f;
  f.d.click({x, static_cast<float>(ov.y + ov.h / 2)});
  const TickRange vis = f.arr->visibleTicks();
  CHECK(f.arr->originTick() > 0.0);
  const double centre = 0.5 * static_cast<double>(vis.start + vis.end);
  CHECK(std::abs(centre - 0.8 * len) < len * 0.05);
}

TEST_CASE("arrangement: the playhead is read only while playing and damages only its columns; follow pages") {
  Fixture f;
  f.d.advance(0.2);
  const uint64_t reads0 = f.arr->stats().playheadReads;
  const uint64_t frames0 = f.d.framesDrawn();
  f.d.advance(10.0);  // stopped: no reads, no frames, no timers
  CHECK(f.arr->stats().playheadReads == reads0);
  CHECK(f.d.framesDrawn() == frames0);
  CHECK(std::isinf(f.d.tree().nextWakeTime()));

  f.d.press("Space");
  REQUIRE(f.arr->playhead().has_value());
  f.d.advance(0.1);
  const uint64_t reads1 = f.arr->stats().playheadReads;
  CHECK(reads1 > reads0);
  // A move of a few pixels: the damage is the playhead's old and new columns (and the control bar's readout).
  const Ticks t1 = 2 * kBar;
  f.port.setPlayheadTick(t1);
  f.d.tree().update(f.d.now() + 0.15);
  const IRect lv = f.arr->lanesViewport();
  const IRect bar = f.d.rect("main/control-bar");
  for (const ClipRect& r : f.d.tree().damage().rects()) {
    const bool inBar = r.y1 <= bar.bottom();
    // T3.10: the column runs on through the pinned returns / master below the lanes.
    const int32_t columnBottom = f.arr->bottomRect().empty() ? lv.bottom() : f.arr->bottomRect().bottom();
    const bool narrowColumn = r.x1 - r.x0 <= 3 && r.y1 <= columnBottom;
    CHECK_MESSAGE((inBar || narrowColumn), r.x0 << "," << r.y0 << " to " << r.x1 << "," << r.y1);
  }
  CHECK(*f.arr->playhead() == t1);
  f.d.step();

  // Follow: the playhead leaving the view pages it (the playhead at the left edge).
  const Ticks far = f.arr->visibleTicks().end + 3 * kBar;
  f.port.setPlayheadTick(far);
  f.d.advance(0.3);
  CHECK(f.arr->originTick() == static_cast<double>(far));
  // Follow off (Ctrl+Shift+F): the view stays.
  f.d.press("Ctrl+Shift+F");
  CHECK_FALSE(f.arr->follow());
  const double o = f.arr->originTick();
  f.port.setPlayheadTick(far + 40 * kBar);
  f.d.advance(0.3);
  CHECK(f.arr->originTick() == o);

  f.d.press("Space");  // stop: no playhead, no more reads, idle again
  CHECK_FALSE(f.arr->playhead().has_value());
  f.d.advance(0.5);
  const uint64_t reads2 = f.arr->stats().playheadReads;
  const uint64_t frames2 = f.d.framesDrawn();
  f.d.advance(10.0);
  CHECK(f.arr->stats().playheadReads == reads2);
  CHECK(f.d.framesDrawn() == frames2);
}

TEST_CASE("arrangement: only visible clips emit geometry; previews rebuild only when the clip pointer changes") {
  Fixture f;
  REQUIRE(f.d.enableGpu().ok());
  f.d.tree().invalidateAll();
  f.d.step();
  const auto& st = f.arr->stats();
  const uint32_t painted = st.clipsPainted;
  CHECK(painted > 5);
  const uint64_t builds = st.previewBuilds;
  CHECK(builds > 0);
  // Repainting builds nothing new.
  f.d.tree().invalidateAll();
  f.d.step();
  CHECK(st.previewBuilds == builds);
  CHECK(st.clipsPainted == painted);
  // Count the clips that meet the visible window: exactly those were painted.
  const TickRange vis = f.arr->visibleTicks();
  uint32_t expected = 0;
  for (const ArrangementRow& r : f.arr->rows()) {
    if (r.group) continue;
    for (const doc::ClipSlot& s : r.track->clips)
      if (s.clip->start < vis.end + 1 && s.clip->start + s.clip->length > vis.start - 1) ++expected;
  }
  CHECK(painted == expected);
  // Scrolled far right (past the song): no clip is painted.
  f.arr->setTimeView(static_cast<double>(f.arr->songEnd() + 10 * kBar), f.arr->pxPerTick());
  f.d.step();
  CHECK(st.clipsPainted == 0);
  CHECK(st.previewBuilds == builds);
  f.arr->setTimeView(0.0, f.arr->pxPerTick());
  f.d.step();
  // Editing one clip's notes replaces that clip's pointer only: one preview rebuild.
  const ArrangementRow& bass = f.arr->rows()[1];
  const doc::ClipRef ref{bass.id, bass.track->clips[0].id};
  std::vector<doc::Note> one(1);
  one[0].start = 0;
  one[0].pitch = tuning::PitchId::fromDegree(24);
  auto e = doc::ops::addNotes(*f.ctx.doc(), f.ctx.store().ids(), ref, one);
  REQUIRE(e.ok());
  REQUIRE(f.ctx.store().dispatch(e->command).ok());
  f.d.step();
  CHECK(st.previewBuilds == builds + 1);
  // Undo restores the old clip pointer, whose preview is still cached (keyed by pointer): no rebuild.
  REQUIRE(f.ctx.undo());
  f.d.step();
  CHECK(st.previewBuilds == builds + 1);
  f.d.tree().invalidateAll();
  f.d.step();
  CHECK(st.previewBuilds == builds + 1);
}

TEST_CASE("arrangement: previews are built progressively, skipped while zooming, and the cache is bounded") {
  Fixture f(false, 2560, 1440);  // 16 rows × ~12 clips × 50 notes in view: more than one build budget
  constexpr int kTracksN = 16, kClipsN = 200, kNotesN = 50;
  views::test::buildLargeArrangement(f.ctx.store(), kTracksN, kClipsN, kNotesN);
  REQUIRE(f.d.enableGpu().ok());
  const auto& st = f.arr->stats();
  namespace am = arrangement_metrics;

  // Cold cache: the first paint builds at most one budget of notes (plus the clip that crosses it); the other clips
  // draw their bodies and are built over the next frames without painting; one repaint shows them all.
  f.d.tree().invalidateAll();
  f.d.step();
  CHECK(st.previewBuilds <= am::kPreviewBuildNotesPerPaint / kNotesN + 1);
  CHECK(st.previewsDeferred > 0);
  const uint64_t framesBefore = f.d.framesDrawn();
  int frames = 0;
  while (f.d.tree().needsFrame() && frames < 100) {
    f.d.advance(1.0 / 144.0);
    ++frames;
  }
  CHECK(frames > 1);
  CHECK(f.d.framesDrawn() - framesBefore == 1);  // build-only frames paint nothing; then one repaint
  CHECK(st.previewsDeferred == 0);
  const uint32_t visibleNotes = st.noteRectsEmitted;
  CHECK(visibleNotes > 0);

  // Zooming out: while the zoom changes, clips denser than kZoomPreviewMinPxPerNote draw no notes; after the
  // settle time the lanes repaint with every preview.
  const Vec2 mid = f.lanePoint(0.5f, 0.5f);
  for (int i = 0; i < 8; ++i) f.arr->zoomAt(mid.x, 0.8);
  f.d.step();
  CHECK(f.arr->zooming());
  CHECK(st.previewsDeferred > 0);
  CHECK(st.noteRectsEmitted == 0);  // 50 notes in a bar of ~16 px: all dense
  f.d.advance(0.5);
  CHECK_FALSE(f.arr->zooming());
  CHECK(st.previewsDeferred == 0);
  CHECK(st.noteRectsEmitted > visibleNotes);
  CHECK_FALSE(f.d.tree().needsFrame());  // idle again

  // The cache stays bounded while scrolling over the whole song (with a small limit to see the bound act).
  f.arr->setTimeView(0.0, am::kDefaultPxPerTick);
  f.d.advance(0.5);
  f.arr->setPreviewCacheLimit(2000);
  for (int i = 0; i < 2000; ++i) {
    f.arr->scrollByPx(40.0);
    f.d.advance(1.0 / 144.0);
  }
  CHECK(st.previewsEvicted > 0);
  CHECK(f.arr->previewCacheNotes() < static_cast<size_t>(kTracksN * kClipsN * kNotesN) / 4);
}

TEST_CASE("arrangement: rows only paint where the clip region is (partial redraw)") {
  Fixture f;
  REQUIRE(f.d.enableGpu().ok());
  f.d.tree().invalidateAll();
  f.d.step();
  // A partial paint of a strip inside one row: the lanes paint only that row (and only its clips there).
  const IRect lane = f.arr->rowLaneRect(1);
  const IRect region{lane.x + 10, lane.y + 2, 50, 4};
  DrawList dl;
  dl.reset(f.d.tree().width(), f.d.tree().height());
  f.d.tree().paint(dl, f.d.resources(), f.d.now(), region);
  CHECK(f.arr->stats().rowsPainted == 1);
  CHECK(f.arr->stats().clipsPainted <= 1);
}

TEST_CASE("arrangement: an empty project shows empty lanes and keeps the shell's hotkeys working") {
  Fixture f(false);
  CHECK(f.arr->rows().empty());
  f.d.press("Space");
  CHECK(f.port.count(FakeTransport::Call::Play) == 1);
  CHECK(f.d.press("Z") == KeyResult::Action);
  CHECK(f.d.press("X") == KeyResult::Action);
}

TEST_CASE("arrangement: rebuilding the shell over the same context takes the actions over") {
  FakeTransport port;
  ViewContext ctx(port);
  for (int i = 0; i < 3; ++i) {
    TestDriver d(800, 600);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE(s.ok());
    auto* arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    arr->setTimeView(0.0, 100.0 / kBar);
    d.press("=");
    CHECK(arr->pxPerTick() == doctest::Approx(200.0 / kBar));
    d.tree().setRoot(std::make_unique<Widget>());
  }
  // With no arrangement left, the actions are parked (disabled).
  const ActionIndex z = ctx.actions().find("arrangement.zoomToSelection");
  REQUIRE(z != kNoAction);
  CHECK_FALSE(ctx.actions().enabled(z));
}

namespace {

void checkGolden(const char* scene, const char* fixture) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, views::test::fixtureDir(fixture), w, h, {}, o);
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

TEST_CASE("render: arrangement-basic over tests/views/fixtures/arrangement matches the golden image") {
  checkGolden("arrangement-basic", "arrangement");
}

TEST_CASE("render: arrangement-zoomed-out over tests/views/fixtures/arrangement matches the golden image") {
  checkGolden("arrangement-zoomed-out", "arrangement");
}
