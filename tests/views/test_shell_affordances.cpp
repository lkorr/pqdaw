// T4.16a: interaction affordances of the shell and the views built from the base controls — the mouse cursor of every
// interactive part (control bar, menu bar, splitters, status line, track headers and their mixer section, the mixer
// view, the tunings panel and its colour palette, the preferences window, the MOS wheel popover, chart and circle),
// hovered through the TestDriver and resolved by UiTree::cursor; the wheel in the mixer (adjusts) and over the
// headers' boxes (left to the track scroll); typed numbers into the control bar's tempo.

#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/mos_wheel/mos_chart.h"
#include "pq/views/mos_wheel/mos_circle.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/views/prefs/audio_prefs.h"
#include "pq/views/shell/shell.h"
#include "pq/views/tunings/tunings_panel.h"
#include "../../src/views/src/shell/shell_widgets.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr const char* kArr = "main/body/top/track-area/arrangement";

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)}; }

template <class W>
W* findWidget(Widget& root, const std::function<bool(W&)>& pred) {
  if (auto* w = dynamic_cast<W*>(&root); w && pred(*w)) return w;
  for (size_t i = 0; i < root.childCount(); ++i)
    if (W* f = findWidget<W>(root.child(i), pred)) return f;
  return nullptr;
}

// The shell over the T3.10 mixer project (groups, returns, sends, tunings), with a scripted audio port.
struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  FakeAudioPrefsPort audio = FakeAudioPrefsPort::studio();
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  MixerView* mixer = nullptr;
  Fixture() {
    ctx.setAudioPrefs(&audio);
    REQUIRE(ctx.openProject(views::test::fixtureDir("mixer")).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    mixer = findMixer(*shell);
    REQUIRE(arr != nullptr);
    REQUIRE(mixer != nullptr);
    d.tree().layoutNow();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }

  Cursor at(Vec2 p) {
    d.move(p);
    return d.tree().cursor();
  }
  Cursor atPath(std::string_view path) {
    Widget* w = d.find(path);
    REQUIRE_MESSAGE(w != nullptr, path);
    REQUIRE_MESSAGE(w->visible(), path);
    return at(mid(w->rect()));
  }
  Cursor over(const Widget* w) {
    REQUIRE(w != nullptr);
    REQUIRE(w->visible());
    return at(mid(w->rect()));
  }
  const doc::Track& track(std::string_view name) const {
    const doc::Track* found = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!found && t.name == name) found = &t;
    });
    REQUIRE_MESSAGE(found != nullptr, name);
    return *found;
  }
  TrackHeader& header(std::string_view name) {
    TrackHeader* h = arr->header(track(name).id);
    REQUIRE(h != nullptr);
    return *h;
  }
  MixerStrip& strip(std::string_view name) {
    Widget& sec = header(name).mixerSection();
    REQUIRE(sec.childCount() == 1);
    return static_cast<MixerStrip&>(sec.child(0));
  }
  size_t history() const { return ctx.store().history().cursor(); }
};

}  // namespace

TEST_CASE("shell affordances: control bar, menu bar, splitters and status line") {
  Fixture f;
  CHECK(f.atPath("main/control-bar/tempo") == Cursor::ResizeNS);  // number boxes: a vertical drag
  CHECK(f.atPath("main/control-bar/sig-numerator") == Cursor::ResizeNS);
  CHECK(f.atPath("main/control-bar/loop-start") == Cursor::ResizeNS);
  CHECK(f.atPath("main/control-bar/play") == Cursor::Pointer);
  CHECK(f.atPath("main/control-bar/stop") == Cursor::Pointer);
  CHECK(f.atPath("main/control-bar/loop") == Cursor::Pointer);
  CHECK(f.atPath("main/control-bar/record") == Cursor::Arrow);  // disabled until M6
  CHECK(f.atPath("main/control-bar/position") == Cursor::Arrow);  // a readout
  CHECK(f.atPath("main/control-bar/tuning") == Cursor::Pointer);  // opens the MOS wheel
  CHECK(f.atPath("main/control-bar/cpu") == Cursor::Arrow);
  auto* bar = dynamic_cast<shell::MenuBar*>(f.d.find("main/menu-bar"));
  REQUIRE(bar != nullptr);
  CHECK(f.at(mid(bar->titleRect(0))) == Cursor::Pointer);
  CHECK(f.at(mid(bar->titleRect(5))) == Cursor::Pointer);
  CHECK(f.at({static_cast<float>(bar->rect().right() - 20), mid(bar->rect()).y}) == Cursor::Arrow);  // past the titles
  // The panel edges: the detail view's top edge and the info / detail split.
  f.ctx.setDetailVisible(true);
  f.d.tree().layoutNow();
  auto& body = dynamic_cast<Splitter&>(*f.d.find("main/body"));
  REQUIRE_FALSE(body.handleRect().empty());
  CHECK(f.at(mid(body.handleRect())) == Cursor::ResizeNS);
  auto& bottom = dynamic_cast<Splitter&>(*f.d.find("main/body/bottom"));
  CHECK(f.at(mid(bottom.handleRect())) == Cursor::ResizeEW);
  CHECK(f.atPath("main/status-bar/view-clip") == Cursor::Pointer);
  CHECK(f.atPath("main/status-bar/view-device") == Cursor::Pointer);
  CHECK(f.atPath("main/status-bar/message") == Cursor::Arrow);
}

TEST_CASE("shell affordances: typing a number into the focused tempo sets it (one undo step)") {
  Fixture f;
  auto* tempo = dynamic_cast<DragBox*>(f.d.find("main/control-bar/tempo"));
  REQUIRE(tempo != nullptr);
  f.d.click(mid(tempo->rect()));
  REQUIRE(f.d.tree().focus() == tempo);
  const size_t h0 = f.history();
  f.d.press("9");
  REQUIRE(tempo->editing());
  f.d.type("0");
  f.d.press("Enter");
  CHECK(f.ctx.doc()->timeline->tempo.front().bpm == doctest::Approx(90.0));
  CHECK(f.history() == h0 + 1);
  // The wheel over the tempo: one run of notches = one undo step.
  f.d.move(mid(tempo->rect()));
  f.d.wheel(mid(tempo->rect()), 2.0f);
  f.d.wheel(mid(tempo->rect()), 1.0f);
  f.d.advance(1.0);
  CHECK(f.ctx.doc()->timeline->tempo.front().bpm == doctest::Approx(93.0));
  CHECK(f.history() == h0 + 2);
}

TEST_CASE("shell affordances: track headers (title, bottom edge, fold, mixer section)") {
  Fixture f;
  TrackHeader& drums = f.header("Drums");
  const IRect t = drums.titleRect();
  CHECK(f.at(mid(t)) == Cursor::Arrow);  // a click selects the track
  // The bottom edge resizes the row: ResizeNS there, and through the drag wherever the pointer goes.
  const IRect r = drums.rect();
  const Vec2 edge{static_cast<float>(t.x + 10), static_cast<float>(r.bottom() - 1)};
  REQUIRE(drums.onResizeEdge(edge));
  CHECK(f.at(edge) == Cursor::ResizeNS);
  CHECK_FALSE(drums.onResizeEdge({edge.x, static_cast<float>(r.bottom() - 8)}));
  const float h0 = f.arr->trackHeight(drums.trackId());
  f.d.down(edge);
  CHECK(f.at({edge.x, edge.y + 30}) == Cursor::ResizeNS);
  f.d.up({edge.x, edge.y + 30});
  CHECK(f.arr->trackHeight(drums.trackId()) == doctest::Approx(h0 + 30.0f));
  // A group's fold triangle.
  TrackHeader& keys = f.header("Keys");
  CHECK(f.over(&keys.foldButton()) == Cursor::Pointer);
  // The mixer section: buttons Pointer, number boxes ResizeNS, the output chooser Pointer, the tuning chip Pointer.
  MixerStrip& s = f.strip("Drums");
  CHECK(f.over(s.activator()) == Cursor::Pointer);
  CHECK(f.over(s.solo()) == Cursor::Pointer);
  CHECK(f.over(s.arm()) == Cursor::Pointer);
  CHECK(f.over(s.volume()) == Cursor::ResizeNS);
  CHECK(f.over(s.pan()) == Cursor::ResizeNS);
  CHECK(f.over(s.output()) == Cursor::Pointer);
  Widget& chip = f.header("Drums").chipSection();
  REQUIRE(chip.childCount() == 1);
  CHECK(f.over(&chip.child(0)) == Cursor::Pointer);
  // Automation mode: the parameter chooser and the lanes toggle open / flip on a click.
  f.d.tree().setFocus(f.arr);
  f.d.press("A");
  f.d.tree().layoutNow();
  TrackHeader& bass = f.header("Bass");
  if (bass.paramChooser().visible()) CHECK(f.over(&bass.paramChooser()) == Cursor::Pointer);
  CHECK(f.over(&bass.lanesToggle()) == Cursor::Pointer);
}

TEST_CASE("shell affordances: the wheel over a header's box scrolls; in the mixer view it adjusts (one undo step)") {
  Fixture f;
  MixerStrip& s = f.strip("Drums");
  const double v0 = s.volume()->value();
  const size_t h0 = f.history();
  f.d.wheel(mid(s.volume()->rect()), -2.0f);
  f.d.advance(1.0);
  CHECK(s.volume()->value() == v0);  // left to the track scroll view
  CHECK(f.history() == h0);
  f.mixer->setShown(true);
  f.d.tree().layoutNow();
  MixerStrip* ms = f.mixer->strip(f.track("Drums").id);
  REQUIRE(ms != nullptr);
  REQUIRE(ms->fader() != nullptr);
  CHECK(f.over(ms->fader()) == Cursor::ResizeNS);
  CHECK(f.over(ms->volume()) == Cursor::ResizeNS);
  CHECK(f.over(ms->activator()) == Cursor::Pointer);
  CHECK(f.over(ms->tuningChip()) == Cursor::Pointer);
  const float db0 = f.track("Drums").mixer.volumeDb;
  f.d.wheel(mid(ms->fader()->rect()), 1.0f);
  f.d.wheel(mid(ms->fader()->rect()), 1.0f);
  f.d.advance(1.0);
  CHECK(f.track("Drums").mixer.volumeDb > db0);
  CHECK(f.history() == h0 + 1);
  CHECK(f.ctx.store().history().entries()[f.history() - 1].changes.paramOnly());
}

TEST_CASE("shell affordances: tunings panel, colour palette and preferences") {
  Fixture f;
  f.ctx.setTuningsVisible(true);
  f.d.tree().layoutNow();
  auto* panel = dynamic_cast<TuningsPanel*>(f.shell->slot(ShellSlot::Tunings).find("tunings-panel"));
  REQUIRE(panel != nullptr);
  Widget* card = panel->find("list/cards/tuning[0]");
  REQUIRE(card != nullptr);
  CHECK(f.over(card->find("head/colour")) == Cursor::Pointer);
  CHECK(f.over(card->find("head/name")) == Cursor::Text);
  CHECK(f.over(card->find("head/remove")) == Cursor::Pointer);
  CHECK(f.over(card->find("kind/wheel")) == Cursor::Pointer);
  CHECK(f.over(panel->find("header/new")) == Cursor::Pointer);
  // The swatch opens the palette: a colour under the pointer is a click target (the palette comes from the theme).
  REQUIRE(f.d.enableGpu().ok());
  f.d.click(mid(card->find("head/colour")->rect()));
  f.d.step();
  Widget* grid = findWidget<Widget>(f.shell->overlay(), [](Widget& w) { return w.name() == "colours"; });
  REQUIRE(grid != nullptr);
  const IRect g = grid->rect();
  CHECK(f.at({static_cast<float>(g.x + 4), static_cast<float>(g.y + 4)}) == Cursor::Pointer);
  f.d.press("Escape");
  f.d.step();
  // Preferences: choosers and toggles Pointer, number boxes ResizeNS.
  PreferencesWindow* w = openPreferences(f.shell->overlay(), f.audio);
  REQUIRE(w != nullptr);
  f.d.tree().layoutNow();
  REQUIRE(w->driverChooser()->enabled());
  CHECK(f.over(w->driverChooser()) == Cursor::Pointer);
  CHECK(f.over(w->toneToggle()) == Cursor::Pointer);
  CHECK(f.over(w->toneVolume()) == Cursor::ResizeNS);
  Widget* close = findWidget<Button>(*w, [](Button& b) { return b.name() == "close"; });
  CHECK(f.over(close) == Cursor::Pointer);
}

TEST_CASE("shell affordances: the MOS wheel popover's title band moves it; chart and circle parts") {
  Fixture f;
  const doc::TuningId main = f.ctx.doc()->tunings->front()->id;
  MosWheelPopover* p = openMosWheel(f.shell->overlay(), f.ctx, TuningTarget::forMain(main));
  REQUIRE(p != nullptr);
  REQUIRE(p->chooseMos({5, 2}, 696.0));
  f.d.tree().layoutNow();
  const IRect r = p->rect();
  CHECK(f.at({static_cast<float>(r.x + 3), static_cast<float>(r.y + 3)}) == Cursor::Move);
  CHECK(f.at({static_cast<float>(r.x + 3), static_cast<float>(r.bottom() - 3)}) == Cursor::Arrow);
  // Moving keeps the cursor and moves the popover.
  f.d.down({static_cast<float>(r.x + 3), static_cast<float>(r.y + 3)});
  CHECK(f.at({static_cast<float>(r.x + 43), static_cast<float>(r.y + 23)}) == Cursor::Move);
  f.d.up({static_cast<float>(r.x + 43), static_cast<float>(r.y + 23)});
  f.d.tree().layoutNow();
  CHECK(p->rect().x == r.x + 40);
  MosWheelPopover::forgetPosition();

  // Chart: the selected generator's axis and marker drag (ResizeEW); another band is a click target.
  MosChart& chart = p->chart();
  REQUIRE(chart.selection().has_value());
  const double gen = chart.selection()->generatorCents;
  const IRect axis = chart.axisRect();
  CHECK(f.at({chart.xOfCents(gen), static_cast<float>(axis.y + axis.h / 2)}) == Cursor::ResizeEW);
  const IRect row = chart.rowRect(3);
  REQUIRE_FALSE(row.empty());
  bool sawBand = false;
  for (int x = row.x + 2; x < row.right() - 2 && !sawBand; x += 4) {
    const Vec2 pt{static_cast<float>(x), static_cast<float>(row.y + row.h / 2)};
    if (chart.regionAt(pt) < 0 || std::abs(pt.x - chart.xOfCents(gen)) < 20.0f) continue;
    CHECK(f.at(pt) == Cursor::Pointer);
    sawBand = true;
  }
  CHECK(sawBand);
  const IRect sb = chart.scrollbarRect();
  if (!sb.empty()) {
    // The scrollbar thumb: the thumb is somewhere in the bar; probe the bar for it.
    bool sawThumb = false;
    for (int y = sb.y + 1; y < sb.bottom() && !sawThumb; y += 3) sawThumb = f.at({static_cast<float>(sb.x + sb.w / 2), static_cast<float>(y)}) == Cursor::ResizeNS;
    CHECK(sawThumb);
  }

  // Circle: buttons and arrows Pointer, the hardness slider ResizeEW, a degree's dot Move, the tonic Pointer.
  MosCircle& circle = p->circle();
  REQUIRE(circle.selection().has_value());
  const MosCircle::Geometry& cg = circle.geometry();
  CHECK(f.at(mid(cg.genUp)) == Cursor::Pointer);
  CHECK(f.at(mid(cg.stepDown)) == Cursor::Pointer);
  CHECK(f.at(mid(cg.allIntervals)) == Cursor::Pointer);
  CHECK(f.at(mid(cg.slider)) == Cursor::ResizeEW);
  CHECK(f.at(circle.dotPos(0)) == Cursor::Pointer);  // the tonic auditions
  CHECK(f.at(circle.dotPos(2)) == Cursor::Move);     // drags its MODMOS alteration
  CHECK(f.at(cg.centre) == Cursor::Arrow);
  // During a slider drag the cursor stays.
  f.d.down(mid(cg.slider));
  CHECK(f.at({mid(cg.slider).x, mid(cg.slider).y + 80}) == Cursor::ResizeEW);
  f.d.up({mid(cg.slider).x, mid(cg.slider).y + 80});
  p->cancel();
}
