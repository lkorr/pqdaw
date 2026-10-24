// T3.7 shell: layout and slots, global hotkeys through the real key routing (Space → port, Ctrl+Z/Ctrl+Shift+Z/Ctrl+Y
// → History, Shift+Tab → detail view, Ctrl+S → save, Ctrl+O → open), control bar ↔ document binding (one undo step
// per gesture), info view, position display while playing, idle = no frames; the `shell` golden.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <ostream>
#include <string>

#include "pq/ui/controls.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using pq::views::test::ScriptedDialogs;

namespace {

constexpr Ticks kSixteenth = kTicksPerWhole / 16;

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d;
  std::unique_ptr<Shell> shell;
  explicit Fixture(uint32_t w = 1280, uint32_t h = 800, float scale = 1.0f) : d(w, h, scale) {
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    port.calls.clear();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }  // widgets before the context they observe
  template <class W = Widget>
  W& get(std::string_view path) {
    Widget* w = d.find(path);
    REQUIRE_MESSAGE(w != nullptr, path);
    W* t = dynamic_cast<W*>(w);
    REQUIRE_MESSAGE(t != nullptr, path);
    return *t;
  }
  std::string text(std::string_view dragBox) { return get<DragBox>(dragBox).valueText(); }
};

constexpr const char* kTempo = "main/control-bar/tempo";
constexpr const char* kSigNum = "main/control-bar/sig-numerator";
constexpr const char* kSigDen = "main/control-bar/sig-denominator";
constexpr const char* kLoop = "main/control-bar/loop";
constexpr const char* kLoopStart = "main/control-bar/loop-start";
constexpr const char* kLoopLength = "main/control-bar/loop-length";
constexpr const char* kPosition = "main/control-bar/position";

}  // namespace

TEST_CASE("shell: Ableton layout (control bar, track area, info + detail, status line) and slots") {
  Fixture f;
  // T3.x-fix-look: the detail view starts collapsed (shown once a clip is opened), and the panels sit on the window
  // colour with 6-px gutters (theme size.gutter) around and between them (were 3-px gaps, flush with the window edge).
  CHECK_FALSE(f.d.find("main/body/bottom")->visible());
  CHECK(f.d.rect("main/body/top/track-area").bottom() == 772);  // the whole height without the detail view
  f.ctx.setDetailVisible(true);
  f.d.tree().layoutNow();
  const IRect bar = f.d.rect("main/control-bar");
  CHECK(bar == IRect{0, 0, 1280, 32});
  const IRect status = f.d.rect("main/status-bar");
  CHECK(status == IRect{0, 778, 1280, 22});
  const IRect track = f.d.rect("main/body/top/track-area");
  const IRect info = f.d.rect("main/body/bottom/info");
  const IRect detail = f.d.rect("main/body/bottom/detail");
  CHECK(track.y == 38);  // a 6-px gutter below the control bar
  CHECK(track.x == 6);   // the browser (M5) is hidden; gutter at the window edge
  CHECK(track.w == 1268);
  CHECK(info.h == 240);  // the detail view's default height (splitter anchored at the bottom)
  CHECK(info.bottom() == 772);  // a 6-px gutter above the status line
  CHECK(track.bottom() + 6 == info.y);
  CHECK(info.x == 6);
  CHECK(info.w == 200);
  CHECK(detail.x == 212);
  CHECK(detail.right() == 1274);
  CHECK(detail.y == info.y);
  // Slots.
  CHECK(&f.shell->slot(ShellSlot::TrackArea) == f.d.find("main/body/top/track-area"));
  CHECK(&f.shell->slot(ShellSlot::Mixer) == f.d.find("main/body/top/track-area/mixer"));
  CHECK(&f.shell->slot(ShellSlot::ClipDetail) == f.d.find("main/body/bottom/detail/clip"));
  CHECK(&f.shell->slot(ShellSlot::DeviceDetail) == f.d.find("main/body/bottom/detail/device"));
  CHECK(&f.shell->slot(ShellSlot::Browser) == f.d.find("main/body/top/browser"));
  CHECK_FALSE(f.shell->slot(ShellSlot::Browser).visible());
  CHECK_FALSE(f.shell->slot(ShellSlot::Mixer).visible());
  CHECK(f.shell->slot(ShellSlot::ClipDetail).visible());
  CHECK_FALSE(f.shell->slot(ShellSlot::DeviceDetail).visible());
  CHECK(f.d.find("overlay") == &f.shell->overlay());
  // The splitters resize: drag the detail view's top edge up by 100 px.
  auto& body = f.get<Splitter>("main/body");
  const IRect h = body.handleRect();
  f.d.drag({640, static_cast<float>(h.y + 1)}, {640, static_cast<float>(h.y + 1 - 100)});
  CHECK(f.d.rect("main/body/bottom/info").h == 340);
  auto& bottom = f.get<Splitter>("main/body/bottom");
  const IRect hb = bottom.handleRect();
  f.d.drag({static_cast<float>(hb.x + 1), 600}, {static_cast<float>(hb.x + 1 + 50), 600});
  CHECK(f.d.rect("main/body/bottom/info").w == 250);
}

TEST_CASE("shell: Space / Shift+Space call the transport port") {
  Fixture f;
  CHECK(f.d.press("Space") == KeyResult::Action);
  REQUIRE(f.port.calls.size() == 1);
  CHECK(f.port.calls[0].call == FakeTransport::Call::Play);
  CHECK(f.port.calls[0].tick == 0);
  CHECK(f.shell->positionText() == "1. 1. 1");
  CHECK(f.d.press("Space") == KeyResult::Action);
  CHECK(f.port.calls.back().call == FakeTransport::Call::Stop);
  CHECK(f.d.press("Shift+Space") == KeyResult::Action);
  CHECK(f.port.calls.back().call == FakeTransport::Call::Continue);
  // With a number box focused (clicked), Space still reaches the transport, not the box.
  f.d.click(f.d.center(kTempo));
  REQUIRE(f.d.tree().focus() == f.d.find(kTempo));
  f.d.press("Space");
  CHECK(f.port.calls.back().call == FakeTransport::Call::Stop);
  // The transport buttons do not take focus, so Space after a click on Play stops instead of pressing Play again.
  f.d.click(f.d.center("main/control-bar/play"));
  CHECK(f.port.calls.back().call == FakeTransport::Call::Play);
  CHECK(f.d.tree().focus() != f.d.find("main/control-bar/play"));
  f.d.press("Space");
  CHECK(f.port.calls.back().call == FakeTransport::Call::Stop);
  f.d.click(f.d.center("main/control-bar/stop"));  // stopped: back to the beginning
  CHECK(f.port.calls.back().call == FakeTransport::Call::Locate);
}

TEST_CASE("shell: undo / redo hotkeys go through the History") {
  Fixture f;
  doc::DocumentStore& store = f.ctx.store();
  const doc::Snapshot s0 = store.current();
  REQUIRE(store.dispatch(doc::SetLoop{{0, kTicksPerWhole, true}}).ok());
  const doc::Snapshot s1 = store.current();
  REQUIRE(store.dispatch(doc::SetTempoMap{{{0, 99.0, TempoCurve::Step}}}).ok());
  const doc::Snapshot s2 = store.current();
  CHECK(f.d.press("Ctrl+Z") == KeyResult::Action);
  CHECK(store.current() == s1);
  CHECK(store.history().cursor() == 1);
  CHECK(f.d.press("Ctrl+Z") == KeyResult::Action);
  CHECK(store.current() == s0);
  CHECK(f.d.press("Ctrl+Z") == KeyResult::Unhandled);  // nothing to undo: the disabled action falls through
  CHECK(f.d.press("Ctrl+Shift+Z") == KeyResult::Action);
  CHECK(store.current() == s1);
  CHECK(f.d.press("Ctrl+Y") == KeyResult::Action);
  CHECK(store.current() == s2);
  // The control bar follows: undo the tempo and the box shows the old value.
  CHECK(f.text(kTempo) == "99.00");
  f.d.press("Ctrl+Z");
  CHECK(f.text(kTempo) == "120.00");
}

TEST_CASE("shell: Shift+Tab switches clip / device; Ctrl+Alt+L hides the detail view") {
  Fixture f;
  Widget& clip = f.shell->slot(ShellSlot::ClipDetail);
  Widget& device = f.shell->slot(ShellSlot::DeviceDetail);
  // T3.x-fix-look: the detail view starts collapsed; the first Shift+Tab shows it (clip), the next switches.
  CHECK_FALSE(f.d.find("main/body/bottom")->visible());
  CHECK(f.d.press("Shift+Tab") == KeyResult::Action);
  CHECK(f.d.find("main/body/bottom")->visible());
  CHECK(f.ctx.detailMode() == DetailMode::Clip);
  CHECK(f.d.press("Shift+Tab") == KeyResult::Action);
  CHECK(f.ctx.detailMode() == DetailMode::Device);
  CHECK_FALSE(clip.visible());
  CHECK(device.visible());
  CHECK(f.d.rect("main/body/bottom/detail/device").w > 0);
  CHECK(f.get<Toggle>("main/status-bar/view-device").on());
  CHECK_FALSE(f.get<Toggle>("main/status-bar/view-clip").on());
  CHECK(f.d.press("Shift+Tab") == KeyResult::Action);
  CHECK(clip.visible());
  CHECK_FALSE(device.visible());
  const int32_t trackH = f.d.rect("main/body/top/track-area").h;
  CHECK(f.d.press("Ctrl+Alt+L") == KeyResult::Action);
  CHECK_FALSE(f.d.find("main/body/bottom")->visible());
  CHECK(f.d.rect("main/body/top/track-area").h > trackH);
  CHECK_FALSE(f.get<Toggle>("main/status-bar/view-clip").on());
  f.d.press("Shift+Tab");  // shows it again
  CHECK(f.d.find("main/body/bottom")->visible());
  // The status line's selectors do the same with the pointer.
  f.d.click(f.d.center("main/status-bar/view-device"));
  CHECK(f.ctx.detailMode() == DetailMode::Device);
  f.d.click(f.d.center("main/status-bar/view-device"));  // the active selector hides the detail view
  CHECK_FALSE(f.ctx.detailVisible());
}

TEST_CASE("shell: Ctrl+S then reopen (Ctrl+O) gives an identical document") {
  Fixture f;
  ScriptedDialogs dlg;
  f.ctx.setFileDialogs(&dlg);
  views::test::buildRichProject(f.ctx.store());
  const doc::Snapshot before = f.ctx.doc();
  const std::string dir = views::test::tempDir("shell_ctrl_s");
  dlg.savePath = dir;
  CHECK(f.d.press("Ctrl+S") == KeyResult::Action);  // untitled: save-as dialog
  CHECK(dlg.saves == 1);
  CHECK_FALSE(f.ctx.dirty());
  CHECK(f.get<Label>("main/status-bar/project").text() == projectNameOf(dir));
  // Change something, then reopen the saved file over it (Discard).
  REQUIRE(f.ctx.store().dispatch(doc::SetLoop{{0, kTicksPerWhole, false}}).ok());
  CHECK(f.get<Label>("main/status-bar/project").text() == projectNameOf(dir) + " *");
  dlg.choice = SaveChoice::Discard;
  dlg.openPath = dir + "/project.pqd";
  CHECK(f.d.press("Ctrl+O") == KeyResult::Action);
  CHECK(dlg.asks == 1);
  CHECK(*f.ctx.doc() == *before);
  CHECK(f.ctx.doc() != before);  // a fresh load, not the old pointer
  CHECK(f.get<Label>("main/status-bar/project").text() == projectNameOf(dir));
  // Ctrl+Shift+S saves elsewhere; Ctrl+N starts over.
  const std::string dir2 = views::test::tempDir("shell_ctrl_shift_s");
  dlg.savePath = dir2;
  CHECK(f.d.press("Ctrl+Shift+S") == KeyResult::Action);
  CHECK(f.ctx.projectDir() == dir2);
  FakeTransport p2;
  ViewContext other(p2);
  REQUIRE(other.openProject(dir2).ok());
  CHECK(*other.doc() == *before);
  CHECK(f.d.press("Ctrl+N") == KeyResult::Action);
  CHECK(f.ctx.doc()->tracks.empty());
  CHECK(f.ctx.projectName() == "Untitled");
}

TEST_CASE("shell: control bar edits the document, one undo step per gesture") {
  Fixture f;
  doc::DocumentStore& store = f.ctx.store();
  CHECK(f.text(kTempo) == "120.00");
  // Drag the tempo up 30 px (1 BPM per logical px): one gesture → one history entry.
  const Vec2 c = f.d.center(kTempo);
  f.d.drag(c, {c.x, c.y - 30.0f}, 6);
  CHECK(store.current()->timeline->tempo.front().bpm == doctest::Approx(150.0));
  CHECK(store.history().entries().size() == 1);
  CHECK(store.history().entries().back().label == "Change Tempo");
  CHECK(store.history().entries().back().changes.timelineOnly());
  // Delete on the focused box returns it to 120 (another single step).
  REQUIRE(f.d.tree().focus() == f.d.find(kTempo));
  f.d.press("Delete");
  CHECK(store.current()->timeline->tempo.front().bpm == 120.0);
  CHECK(store.history().entries().size() == 2);
  // Time signature: drag the numerator up 3 steps (6 px each), the denominator down one (16 px per step).
  const Vec2 n = f.d.center(kSigNum);
  f.d.drag(n, {n.x, n.y - 18.0f});
  CHECK(store.current()->timeline->signatures.front().sig == TimeSignature{7, 4});
  CHECK(f.text(kSigNum) == "7");
  const Vec2 dn = f.d.center(kSigDen);
  f.d.drag(dn, {dn.x, dn.y - 16.0f});
  CHECK(store.current()->timeline->signatures.front().sig == TimeSignature{7, 8});
  CHECK(f.text(kSigDen) == "8");
  CHECK(store.history().entries().size() == 4);
  // Loop switch: one entry, and undo turns the switch back off.
  f.d.click(f.d.center(kLoop));
  CHECK(store.current()->loop.enabled);
  CHECK(store.history().entries().size() == 5);
  f.d.press("Ctrl+Z");
  CHECK_FALSE(store.current()->loop.enabled);
  // Loop start: drag 16 px up = 16 sixteenths later; the length stays.
  const doc::LoopRegion l0 = store.current()->loop;
  const Vec2 ls = f.d.center(kLoopStart);
  f.d.drag(ls, {ls.x, ls.y - 16.0f});
  const doc::LoopRegion l1 = store.current()->loop;
  CHECK(l1.start == l0.start + 16 * kSixteenth);
  CHECK(l1.end - l1.start == l0.end - l0.start);
  CHECK(f.text(kLoopStart) == "2. 2. 1");  // 7/8: a bar is 14 sixteenths, so 16 = bar 2 + one eighth
}

TEST_CASE("shell: the control bar shows the opened project's values") {
  Fixture f;
  REQUIRE(f.ctx.openProject(views::test::fixtureDir("small")).ok());
  CHECK(f.text(kTempo) == "96.50");
  CHECK(f.text(kSigNum) == "7");
  CHECK(f.text(kSigDen) == "8");
  CHECK(f.ctx.doc()->loop.enabled);
  CHECK(f.text(kLoopStart) == "2. 1. 1");
  CHECK(f.text(kLoopLength) == "3. 0. 0");
  CHECK(f.get<Label>("main/status-bar/project").text() == "small");
  // Type a loop length: "2.3.1" in 7/8 = 2 bars + 3 eighths + 1 sixteenth.
  auto& len = f.get<DragBox>(kLoopLength);
  len.beginEdit();
  f.d.type("2.3.1");
  f.d.press("Enter");
  const doc::LoopRegion l = f.ctx.doc()->loop;
  CHECK(l.end - l.start == 2 * 7 * (kTicksPerWhole / 8) + 3 * (kTicksPerWhole / 8) + kSixteenth);
  CHECK(f.text(kLoopLength) == "2. 3. 1");
}

TEST_CASE("shell: the info view shows the hovered control's text") {
  Fixture f;
  CHECK(f.shell->infoTitle().empty());
  f.d.move(f.d.center(kTempo));
  CHECK(f.shell->infoTitle() == "Song Tempo");
  CHECK(f.shell->infoBody().find("beats per minute") != std::string::npos);
  f.d.move(f.d.center("main/control-bar/loop"));
  CHECK(f.shell->infoTitle() == "Loop Switch");
  f.d.move(f.d.center("main/body/top/track-area"));
  CHECK(f.shell->infoTitle().empty());
}

TEST_CASE("shell: the position display reads the playhead only while playing; idle draws nothing") {
  Fixture f;
  f.d.advance(0.1);  // first frame(s)
  const uint64_t frames0 = f.d.framesDrawn();
  f.d.advance(10.0);  // 10 s of fake time, nothing to do
  CHECK(f.d.framesDrawn() == frames0);
  CHECK(std::isinf(f.d.tree().nextWakeTime()));
  CHECK(f.shell->positionText() == "1. 1. 1");

  f.d.press("Space");
  f.port.setPlayheadTick(2 * kTicksPerWhole + kPpq);  // bar 3, beat 2
  f.d.advance(0.2);
  CHECK(f.shell->positionText() == "3. 2. 1");  // read from the playhead on the position timer
  CHECK(std::isfinite(f.d.tree().nextWakeTime()));
  CHECK(f.d.tree().nextWakeTime() - f.d.now() <= 0.1 + 1e-9);
  f.d.press("Space");  // stop: the timer ends, back to idle
  f.d.advance(0.5);
  const uint64_t frames1 = f.d.framesDrawn();
  f.d.advance(10.0);
  CHECK(f.d.framesDrawn() == frames1);
  CHECK(std::isinf(f.d.tree().nextWakeTime()));
}

TEST_CASE("shell: while playing, a position change damages only the position display and the playhead (partial redraw)") {
  Fixture f;
  f.d.advance(0.1);
  f.d.press("Space");
  f.d.advance(0.05);  // the play button's lit state is painted
  f.port.setPlayheadTick(5 * kTicksPerWhole);
  f.d.tree().update(f.d.now() + 0.2);  // the position timer fires (and the arrangement's playhead timer, T3.8)
  const IRect pos = f.d.rect(kPosition);
  // Since T3.8 the arrangement's playhead line also moves: its old and new columns (a pixel either side, from the
  // ruler down through the lanes) may be damaged too, nothing else.
  auto* arr = dynamic_cast<ArrangementView*>(f.d.find("main/body/top/track-area/arrangement"));
  REQUIRE(arr != nullptr);
  const IRect ruler = f.d.rect("main/body/top/track-area/arrangement/ruler");
  const IRect lanes = arr->lanesViewport();
  auto column = [&](Ticks t) {
    const auto x = static_cast<int32_t>(std::floor(arr->xOfTick(static_cast<double>(t))));
    // T3.10: through the pinned returns / master too.
    const int32_t bottom = arr->bottomRect().empty() ? lanes.bottom() : arr->bottomRect().bottom();
    return IRect{x - 1, ruler.y, 3, bottom - ruler.y};
  };
  const IRect allowed[] = {pos, column(0), column(5 * kTicksPerWhole)};
  const auto rects = f.d.tree().damage().rects();
  REQUIRE_FALSE(rects.empty());
  CHECK_FALSE(f.d.tree().damage().full());
  bool sawPosition = false;
  for (const ClipRect& r : rects) {
    bool inside = false;
    for (const IRect& a : allowed) inside |= r.x0 >= a.x && r.y0 >= a.y && r.x1 <= a.right() && r.y1 <= a.bottom();
    CHECK_MESSAGE(inside, "damage " << r.x0 << "," << r.y0 << " to " << r.x1 << "," << r.y1);
    sawPosition |= r.x0 >= pos.x && r.x1 <= pos.right() && r.y0 >= pos.y && r.y1 <= pos.bottom();
  }
  CHECK(sawPosition);
  CHECK(f.shell->positionText() == "6. 1. 1");
}

TEST_CASE("render: shell over tests/views/fixtures/empty matches the golden image") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize("shell", w, h);
  CHECK(w == 1280);
  CHECK(h == 800);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene("shell", views::test::fixtureDir("empty"), w, h, {}, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/shell.png");
  if (!ref) (void)writePng("shell.actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden("shell", *img, *ref, true, &d));
  MESSAGE("shell: " << pq::ui::test::describe(d));
}
