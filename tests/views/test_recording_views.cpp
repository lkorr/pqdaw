// T6.7: the views' side of recording, through real dispatch — an audio strip's input chooser (none, mono inputs, stereo
// pairs; channels the device lacks marked; one structural undo step) and its arm button; the arrangement's live
// recording overlay (read with the playhead while recording, painted over the rows, dropped when the session ends).
#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  doc::TrackId audio;
  Fixture() {
    REQUIRE(ctx.openProject(views::test::fixtureDir("mixer")).ok());
    auto t = doc::ops::createTrack(*ctx.doc(), ctx.store().ids(), doc::TrackKind::Audio, {}, "Vox");
    REQUIRE(t.ok());
    REQUIRE(ctx.store().dispatch(t->command).ok());
    audio = t->tracks.at(0);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }
  const doc::Track& track(doc::TrackId id) const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), id);
    REQUIRE(t != nullptr);
    return *t;
  }
  Vec2 centre(const Widget& w) const {
    const IRect r = w.rect();
    return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)};
  }
  Menu* openMenu(Control* c) {
    REQUIRE(c != nullptr);
    d.click(centre(*c));
    auto* menu = dynamic_cast<Menu*>(shell->overlay().top());
    REQUIRE(menu != nullptr);
    return menu;
  }
  int32_t itemOf(Menu* m, std::string_view label) {
    for (size_t i = 0; i < m->items().size(); ++i)
      if (m->items()[i].label == label) return static_cast<int32_t>(i);
    FAIL("no item " << label);
    return 0;
  }
};

}  // namespace

TEST_CASE("T6.7 mixer view: an audio strip's input chooser (mono, stereo pairs, none) and its arm button") {
  Fixture f;
  f.port.inputs = 4;
  MixerView* mixer = findMixer(*f.shell);
  REQUIRE(mixer != nullptr);
  mixer->setShown(true);
  f.d.tree().layoutNow();
  MixerStrip* s = mixer->strip(f.audio);
  REQUIRE(s != nullptr);
  REQUIRE(s->audioIn() != nullptr);
  REQUIRE(s->monitor() != nullptr);
  CHECK(s->midiIn() == nullptr);
  const size_t h0 = f.ctx.store().history().cursor();
  Menu* m = f.openMenu(s->audioIn());
  CHECK(m->items()[0].label == "No Input");
  CHECK(m->items()[0].checked);
  (void)f.itemOf(m, "In 4");
  CHECK_FALSE(m->items()[static_cast<size_t>(f.itemOf(m, "In 4"))].label.ends_with("(not available)"));
  m->choose(f.itemOf(m, "In 3/4"));
  f.d.step();
  CHECK(f.track(f.audio).mixer.audioIn == doc::AudioInput{2, 2});
  CHECK(f.ctx.store().history().cursor() == h0 + 1);
  CHECK(f.ctx.store().history().entries().back().changes.structural());  // the track's patch reads track.in now
  // Fewer device inputs: the chosen pair is still offered, marked.
  f.port.inputs = 2;
  m = f.openMenu(s->audioIn());
  CHECK(f.itemOf(m, "In 3/4 (not available)") >= 0);
  m->choose(f.itemOf(m, "In 1"));
  f.d.step();
  CHECK(f.track(f.audio).mixer.audioIn == doc::AudioInput{0, 1});
  // The arm button arms the audio track (Live: audio tracks record too).
  REQUIRE(s->arm() != nullptr);
  f.d.click(f.centre(*s->arm()));
  f.d.step();
  CHECK(f.track(f.audio).mixer.arm);
}

TEST_CASE("T6.7 arrangement: the live recording overlay is read while recording and painted, then dropped") {
  Fixture f;
  auto* view = dynamic_cast<ArrangementView*>(f.d.find("main/body/top/track-area/arrangement"));
  REQUIRE(view != nullptr);
  RecordingOverlay& ov = f.port.overlay;
  ov.revision = 1;
  ov.session = 1;
  ov.active = true;
  RecordingOverlay::Region g;
  g.trackId = f.audio.value;
  g.start = 0;
  g.end = 4 * kPpq;
  g.audio = true;
  g.audioStart = 0;
  g.peakList = 0;
  g.peak0 = 0;
  g.peakCount = 375;  // 2 s of 256-frame buckets
  ov.regions.push_back(g);
  ov.peaks.push_back(std::vector<float>(750, 0.0f));
  for (size_t i = 0; i < 375; ++i) {
    ov.peaks[0][2 * i] = -0.5f;
    ov.peaks[0][2 * i + 1] = 0.5f;
  }
  f.d.press("F9");  // the fake transport records (and plays)
  f.d.advance(0.3);  // the playhead timer reads the overlay
  CHECK(f.port.overlayReads > 0);
  CHECK(view->recording().active);
  REQUIRE(view->recording().regions.size() == 1);
  CHECK(view->recording().regions[0].trackId == f.audio.value);
  f.d.step();  // paints it (over the row)
  // The session grows: a new revision is taken.
  ov.regions[0].end = 8 * kPpq;
  ov.revision = 2;
  f.d.advance(0.3);
  CHECK(view->recording().regions[0].end == 8 * kPpq);
  // Stop: the session ends (inactive), nothing more is drawn.
  ov.active = false;
  ov.revision = 3;
  f.d.press("Space");
  f.d.advance(0.3);
  CHECK_FALSE(view->recording().active);
  f.d.step();
}
