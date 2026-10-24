// T3.10 track headers, groups and the mixer: the header mixer section (volume, pan, activator = mute, solo, arm,
// sends, Pre/Post, output, tuning chip), returns and master pinned at the bottom, Ctrl+Shift+T / Ctrl+Alt+T / Ctrl+G /
// Ctrl+Shift+G / Ctrl+R / Delete on tracks (MIDI track: Ctrl+Shift+T since T3.x-fix-look), the colour palette, height resize, the mixer view (Ctrl+Alt+M), meters
// from a MeterSource, and the goldens `tracks-mixer` / `mixer-view`. Every edit is one undo step; a volume drag is one
// step whose ChangeSet is paramOnly.

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "pq/document/audibility.h"
#include "pq/document/serialize.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
#include "mixer_fixture.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr const char* kArr = "main/body/top/track-area/arrangement";

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d;
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  MixerView* mixer = nullptr;
  explicit Fixture(const char* project = "mixer", uint32_t w = 1280, uint32_t h = 800) : d(w, h) {
    REQUIRE(ctx.openProject(views::test::fixtureDir(project)).ok());
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

  size_t history() const { return ctx.store().history().cursor(); }
  const doc::ChangeSet& lastEntryChanges() const { return ctx.store().history().entries()[history() - 1].changes; }
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
  Vec2 centre(const Widget& w) const {
    const IRect r = w.rect();
    return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)};
  }
  Vec2 title(std::string_view name) {
    const IRect t = header(name).titleRect();
    return {static_cast<float>(t.x + t.w / 2), static_cast<float>(t.y + t.h / 2)};
  }
};

std::string readText(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("mixer: the fixture project is what buildMixerProject makes") {
  doc::DocumentStore store;
  views::test::buildMixerProject(store);
  auto text = doc::serializeProject(*store.current(), store.view());
  REQUIRE(text.ok());
  const std::string dir = views::test::fixtureDir("mixer");
  if (const char* u = std::getenv("PQ_UPDATE_FIXTURES"); u && *u == '1') REQUIRE(doc::saveProject(dir, store).ok());
  std::string file = readText(dir + "/project.pqd");
  std::erase(file, '\r');
  CHECK_MESSAGE(file == *text, "tests/views/fixtures/mixer is stale: run pq_tests_views with PQ_UPDATE_FIXTURES=1");
}

TEST_CASE("mixer: value mappings (volume taper, pan text, type-in parsers)") {
  CHECK(volumeToFader(-70.0) == 0.0);
  CHECK(volumeToFader(6.0) == doctest::Approx(1.0));
  CHECK(volumeToFader(0.0) == doctest::Approx(0.70795).epsilon(1e-4));
  for (double db : {-60.0, -24.0, -6.0, 0.0, 3.0}) CHECK(faderToVolume(volumeToFader(db)) == doctest::Approx(db).epsilon(1e-9));
  CHECK(faderToVolume(0.0) == doc::kMinVolumeDb);
  CHECK(formatVolume(-70.0) == "-inf");
  CHECK(formatVolume(-3.54) == "-3.5");
  CHECK(formatVolume(-0.01) == "0.0");
  CHECK(formatPan(0.0) == "C");
  CHECK(formatPan(-0.4) == "20L");
  CHECK(formatPan(1.0) == "50R");
  CHECK(*parseVolume("-12 dB") == -12.0);
  CHECK(*parseVolume("-inf") == doc::kMinVolumeDb);
  CHECK(*parseVolume("+20") == 6.0);  // clamped
  CHECK_FALSE(parseVolume("loud").has_value());
  CHECK(*parsePan("25L") == doctest::Approx(-0.5));
  CHECK(*parsePan("c") == 0.0);
  CHECK(*parsePan("50r") == doctest::Approx(1.0));
  CHECK_FALSE(parsePan("x").has_value());
}

TEST_CASE("mixer: header strips show the document (labels, toggles, sends, output, tuning chip), returns + master pinned") {
  Fixture f;
  // Rows: main tree; bottom: the two returns and the master.
  REQUIRE(f.arr->bottomRows().size() == 3);
  CHECK(f.arr->bottomRows()[0].track->name == "A Reverb");
  CHECK(f.arr->bottomRows()[2].track->kind == doc::TrackKind::Master);
  const IRect bottom = f.arr->bottomRect();
  CHECK_FALSE(bottom.empty());
  CHECK(bottom.y >= f.arr->lanesViewport().bottom());

  MixerStrip& drums = f.strip("Drums");
  REQUIRE(drums.activator() != nullptr);
  CHECK(drums.activator()->on());  // on = not muted
  CHECK(drums.volume()->valueText() == "-3.5");
  CHECK(drums.sendCount() == 2);
  CHECK(drums.send(0)->valueText() == "-12.0");
  CHECK(drums.send(1)->valueText() == "-inf");
  CHECK(drums.arm() != nullptr);
  CHECK(f.strip("Bass").arm()->on());
  CHECK_FALSE(f.strip("Pad").activator()->on());  // muted
  CHECK(f.strip("Lead").solo()->on());
  CHECK(f.strip("Piano").pan()->valueText() == "20L");
  CHECK(f.strip("Keys").arm() == nullptr);  // groups cannot be armed
  // The return's Pre/Post switch, its letter on the activator; it sends to the other return only.
  MixerStrip& delay = f.strip("B Delay");
  REQUIRE(delay.post() != nullptr);
  CHECK_FALSE(delay.post()->on());  // Pre
  CHECK(f.strip("A Reverb").post()->on());  // Post
  CHECK(delay.sendCount() == 1);
  CHECK(delay.sendTarget(0) == f.track("A Reverb").id);
  // The master: volume, pan and meter only.
  MixerStrip& master = f.strip("Master");
  CHECK(master.activator() == nullptr);
  CHECK(master.solo() == nullptr);
  CHECK(master.volume()->valueText() == "-1.5");
  // Output chooser text: the parent group, a routed group, the master.
  CHECK(static_cast<const Control*>(f.strip("Piano").output()) != nullptr);
  // T3.14's tuning chip under the title, targeting the track.
  Widget& chipSec = f.header("Bass").chipSection();
  REQUIRE(chipSec.childCount() == 1);
  auto& chip = static_cast<TuningChip&>(chipSec.child(0));
  REQUIRE(chip.target().has_value());
  CHECK(chip.target()->track == f.track("Bass").id);
  CHECK(chip.text().find("31") != std::string::npos);
  CHECK(f.header("A Reverb").chipSection().childCount() == 0);  // returns play no notes
}

TEST_CASE("mixer: the activator is the mute (button and 0), a Param edit; several selected tracks toggle together") {
  Fixture f;
  MixerStrip& drums = f.strip("Drums");
  size_t h = f.history();
  f.d.click(f.centre(*drums.activator()));
  CHECK(f.history() == h + 1);
  CHECK(f.track("Drums").mixer.mute);
  CHECK(f.track("Drums").active);  // not a deactivation (no recompile)
  CHECK(f.lastEntryChanges().paramOnly());
  CHECK_FALSE(drums.activator()->on());
  // Select Drums and Bass by their titles; the activator of a selected track toggles both (like `0`).
  f.d.click(f.title("Drums"));
  f.d.click(f.title("Bass"), mod::kShift);
  REQUIRE(f.arr->selection().tracks.size() == 2);
  f.d.click(f.centre(*f.strip("Bass").activator()));  // Bass on → mute both
  CHECK(f.track("Drums").mixer.mute);
  CHECK(f.track("Bass").mixer.mute);
  h = f.history();
  CHECK(f.d.press("0") == KeyResult::Action);  // both muted → both on
  CHECK(f.history() == h + 1);
  CHECK_FALSE(f.track("Drums").mixer.mute);
  CHECK_FALSE(f.track("Bass").mixer.mute);
  CHECK(f.lastEntryChanges().paramOnly());
}

TEST_CASE("mixer: solo is exclusive, Ctrl-click adds; arm likewise; Pre/Post on a return") {
  Fixture f;
  REQUIRE(f.track("Lead").mixer.solo);
  f.d.click(f.centre(*f.strip("Drums").solo()));
  CHECK(f.track("Drums").mixer.solo);
  CHECK_FALSE(f.track("Lead").mixer.solo);  // exclusive
  CHECK(f.lastEntryChanges().paramOnly());
  CHECK_FALSE(f.strip("Lead").solo()->on());
  f.d.click(f.centre(*f.strip("Bass").solo()), mod::kCtrl);
  CHECK(f.track("Drums").mixer.solo);
  CHECK(f.track("Bass").mixer.solo);
  CHECK(doc::effectiveAudible(*f.ctx.doc(), f.track("Bass").id));
  CHECK_FALSE(doc::effectiveAudible(*f.ctx.doc(), f.track("Piano").id));
  f.d.click(f.centre(*f.strip("Drums").solo()));  // a soloed track: off
  CHECK_FALSE(f.track("Drums").mixer.solo);
  CHECK(f.track("Bass").mixer.solo);
  // Arm: Bass is armed; arming Drums disarms it; Ctrl adds.
  f.d.click(f.centre(*f.strip("Drums").arm()));
  CHECK(f.track("Drums").mixer.arm);
  CHECK_FALSE(f.track("Bass").mixer.arm);
  f.d.click(f.centre(*f.strip("Bass").arm()), mod::kCtrl);
  CHECK(f.track("Drums").mixer.arm);
  CHECK(f.track("Bass").mixer.arm);
  // Pre/Post: one structural step.
  const size_t h = f.history();
  f.d.click(f.centre(*f.strip("A Reverb").post()));
  CHECK(f.history() == h + 1);
  CHECK(doc::ops::returnSendsPreFader(*f.ctx.doc(), f.track("A Reverb").id));
  CHECK(f.lastEntryChanges().structural());
  CHECK_FALSE(f.strip("A Reverb").post()->on());
}

TEST_CASE("mixer: a volume drag is one undo step with a paramOnly ChangeSet; the lanes are not repainted") {
  Fixture f;
  MixerStrip& drums = f.strip("Drums");
  DragBox& vol = *drums.volume();
  const float before = f.track("Drums").mixer.volumeDb;
  const size_t h = f.history();
  int events = 0;
  bool lastParamOnly = false;
  auto sub = f.ctx.subscribe([&](const doc::StoreEvent& e) {
    ++events;
    lastParamOnly = e.changes.paramOnly();
  });
  const uint64_t rebuildsBefore = f.arr->stats().rowRebuilds;
  f.d.step();  // flush pending damage
  const Vec2 c = f.centre(vol);
  f.d.move(c);
  f.d.down(c);
  for (int i = 1; i <= 30; ++i) {
    f.d.move({c.x, c.y - static_cast<float>(i)});
    // The lanes are not damaged by a mixer edit (only the strip's own widgets).
    const IRect lanes = f.arr->lanesViewport();
    f.d.tree().layoutNow();
    for (const ClipRect& r : f.d.tree().damage().rects())
      CHECK_FALSE((r.x0 < lanes.right() && r.x1 > lanes.x && r.y0 < lanes.bottom() && r.y1 > lanes.y));
    f.d.step();
  }
  CHECK(f.history() == h);  // nothing recorded while dragging (live preview only)
  f.d.up({c.x, c.y - 30.0f});
  CHECK(f.history() == h + 1);
  CHECK(f.track("Drums").mixer.volumeDb > before);
  CHECK(f.lastEntryChanges().paramOnly());
  REQUIRE_FALSE(f.lastEntryChanges().params.empty());
  CHECK(f.lastEntryChanges().params[0].kind == doc::ParamKind::TrackVolume);
  CHECK(events >= 30);
  CHECK(lastParamOnly);
  CHECK(f.arr->stats().rowRebuilds > rebuildsBefore);
  REQUIRE(f.ctx.undo());
  CHECK(f.track("Drums").mixer.volumeDb == before);
  // A send drag and a pan key step are one step each.
  const size_t h2 = f.history();
  const Vec2 sc = f.centre(*drums.send(1));
  f.d.drag(sc, {sc.x, sc.y - 40.0f}, 8);
  CHECK(f.history() == h2 + 1);
  CHECK(f.lastEntryChanges().paramOnly());
  CHECK(f.track("Drums").mixer.sends.size() == 2);
  // Delete on a focused number box returns to the default (one step).
  f.d.click(f.centre(*drums.pan()));
  REQUIRE(f.ctx.store().dispatch(doc::SetTrackPan{f.track("Drums").id, 0.5f}).ok());
  f.d.press("Delete");
  CHECK(f.track("Drums").mixer.pan == 0.0f);
}

TEST_CASE("mixer: a volume-drag frame damages and paints the same whatever the track count; chips repaint only on tuning changes") {
  // T3.x-fix-paint (M3 review M12a): before, every header's tuning chip subscribed to the store and repainted on each
  // Param event of the drag, so a frame damaged (and painted) one chip per visible track.
  struct Frame {
    int64_t area = 0;
    uint32_t rects = 0;
    uint64_t widgets = 0;
  };
  auto measure = [](int tracks) {
    FakeTransport port;
    ViewContext ctx(port);
    views::test::buildLargeArrangement(ctx.store(), tracks, 4, 4);
    TestDriver d(1280, 800);
    auto shell = Shell::create(d.tree(), ctx);
    REQUIRE(shell.ok());
    d.tree().layoutNow();
    auto* arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    d.step();
    TrackHeader* h = arr->header(ctx.doc()->tracks.front()->id);
    REQUIRE(h != nullptr);
    auto& strip = static_cast<MixerStrip&>(h->mixerSection().child(0));
    const IRect vb = strip.volume()->rect();
    const Vec2 c{static_cast<float>(vb.x + vb.w / 2), static_cast<float>(vb.y + vb.h / 2)};
    d.move(c);
    d.down(c);
    d.move({c.x, c.y - 3.0f});
    d.step();  // the gesture's first frame
    // Every chip of the tracks visible: none may be damaged by a volume change.
    std::vector<IRect> chips;
    for (const doc::TrackPtr& t : ctx.doc()->tracks)
      if (TrackHeader* th = arr->header(t->id); th && th->visible() && th->chipSection().childCount() > 0)
        chips.push_back(th->chipSection().child(0).rect());
    REQUIRE(chips.size() >= 4);
    Frame fr;
    DrawList dl;
    for (int i = 0; i < 6; ++i) {
      d.move({c.x, c.y - 4.0f - static_cast<float>(i)});
      d.tree().update(d.now());
      IRect rs[Damage::kMaxRects];
      int64_t area = 0;
      const uint32_t n = d.tree().damageRects(rs, area);
      for (uint32_t k = 0; k < n; ++k)
        for (const IRect& chip : chips)
          CHECK_MESSAGE(intersect(rs[k], chip).empty(), "damage " << rs[k].x << "," << rs[k].y << " " << rs[k].w << "x" << rs[k].h << " chip "
                                                                   << chip.x << "," << chip.y << " " << chip.w << "x" << chip.h);
      dl.reset(1280, 800);
      d.tree().paint(dl, d.resources(), d.now(), std::span<const IRect>(rs, n));
      fr.area += area;
      fr.rects += n;
      fr.widgets += d.tree().stats().widgetsPainted;
    }
    d.up({c.x, c.y - 10.0f});
    // A tuning change does repaint the chips that show it (every track inherits the project's first tuning).
    d.step();
    const doc::TuningSlot& slot = *ctx.doc()->tunings->front();
    auto edo = tuning::Edo::make(19);
    REQUIRE(edo.ok());
    REQUIRE(ctx.store().dispatch(doc::SetTuningDef{slot.id, doc::TuningDef{*edo, tuning::kDefaultReferenceHz}}).ok());
    d.tree().update(d.now());
    IRect rs[Damage::kMaxRects];
    int64_t area = 0;
    const uint32_t n = d.tree().damageRects(rs, area);
    bool chipDamaged = false;
    for (uint32_t k = 0; k < n; ++k)
      for (const IRect& chip : chips) chipDamaged = chipDamaged || !intersect(rs[k], chip).empty();
    CHECK(chipDamaged);
    d.tree().setRoot(std::make_unique<Widget>());
    return fr;
  };
  const Frame a = measure(8), b = measure(64);
  CHECK(a.rects == b.rects);
  CHECK(a.area == b.area);
  CHECK(a.widgets == b.widgets);
  CHECK(a.area > 0);
}

TEST_CASE("mixer: Ctrl+Shift+T / Ctrl+Alt+T insert after the selection; Ctrl+G across nested groups; Ctrl+Shift+G") {
  Fixture f("arrangement");
  // Select Bass (second top-level track) by its title; Ctrl+Shift+T inserts right after it, selected, one step.
  f.d.click(f.title("Bass"));
  size_t h = f.history();
  CHECK(f.d.press("Ctrl+Shift+T") == KeyResult::Action);  // Live 12: a MIDI track
  CHECK(f.history() == h + 1);
  const doc::Project& p = *f.ctx.doc();
  REQUIRE(p.tracks.size() >= 4);
  CHECK(p.tracks[1]->name == "Bass");
  const doc::TrackId added = p.tracks[2]->id;
  CHECK(p.tracks[2]->kind == doc::TrackKind::Midi);
  REQUIRE(f.arr->selection().tracks.size() == 1);
  CHECK(f.arr->selection().tracks[0] == added);
  // Ctrl+Alt+T: a return (pinned at the bottom).
  h = f.history();
  CHECK(f.d.press("Ctrl+Alt+T") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  REQUIRE(f.ctx.doc()->returns.size() == 1);
  CHECK(f.arr->bottomRows().size() == 2);
  REQUIRE(f.ctx.undo());
  REQUIRE(f.ctx.undo());

  // Ctrl+G across nested groups: Piano (in Keys) and Strings (in Keys/Pads) into one new group where Piano was.
  f.d.click(f.title("Piano"));
  f.d.click(f.title("Strings"), mod::kCtrl);
  REQUIRE(f.arr->selection().tracks.size() == 2);
  h = f.history();
  CHECK(f.d.press("Ctrl+G") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  const doc::Track& keys = f.track("Keys");
  REQUIRE(keys.children.size() == 2);
  const doc::Track& g = *keys.children[0];
  CHECK(g.kind == doc::TrackKind::Group);
  REQUIRE(g.children.size() == 2);
  CHECK(g.children[0]->name == "Piano");
  CHECK(g.children[1]->name == "Strings");
  CHECK(keys.children[1]->name == "Pads");
  CHECK(f.track("Pads").children.empty());
  REQUIRE(f.arr->selection().tracks.size() == 1);
  CHECK(f.arr->selection().tracks[0] == g.id);
  // Ctrl+Shift+G: the new group's members go back in its place.
  h = f.history();
  CHECK(f.d.press("Ctrl+Shift+G") == KeyResult::Action);
  CHECK(f.history() == h + 1);
  CHECK(f.track("Keys").children.size() == 3);
  CHECK(f.track("Keys").children[0]->name == "Piano");
}

TEST_CASE("mixer: rename via Ctrl+R + Enter (one step), Esc cancels; Delete deletes header-selected tracks") {
  Fixture f;
  f.d.click(f.title("Bass"));
  CHECK(f.d.press("Ctrl+R") == KeyResult::Action);
  TrackHeader& hb = f.header("Bass");
  REQUIRE(hb.renaming());
  CHECK(f.d.tree().focus() == hb.renameField());
  const size_t h = f.history();
  CHECK(f.d.type("Sub Bass"));
  CHECK(f.d.press("Return") != KeyResult::Unhandled);
  CHECK(f.history() == h + 1);
  CHECK(f.track("Sub Bass").kind == doc::TrackKind::Midi);
  f.d.step();
  CHECK_FALSE(f.header("Sub Bass").renaming());
  // Esc: no change, no history.
  f.d.click(f.title("Drums"));
  f.d.press("Ctrl+R");
  REQUIRE(f.header("Drums").renaming());
  f.d.type("Nope");
  f.d.press("Escape");
  f.d.step();
  CHECK(f.history() == h + 1);
  CHECK_FALSE(f.header("Drums").renaming());
  CHECK(f.track("Drums").name == "Drums");
  // Delete after a header click: the track goes (one step).
  f.d.click(f.title("Lead"));
  const size_t n = f.ctx.doc()->tracks.size();
  CHECK(f.d.press("Delete") == KeyResult::Action);
  CHECK(f.ctx.doc()->tracks.size() == n - 1);
}

TEST_CASE("mixer: right-click track menu, colour palette, height resize (view state, no history), group fold") {
  Fixture f;
  REQUIRE(f.d.enableGpu().ok());  // the palette comes from the theme
  f.d.step();
  // Colour: the palette popover recolours the selected track (one step).
  f.d.click(f.title("Drums"));
  Widget* pop = f.arr->openColourPicker(f.title("Drums"));
  REQUIRE(pop != nullptr);
  f.d.tree().layoutNow();
  Widget* grid = pop->find("colours");
  REQUIRE(grid != nullptr);
  const IRect gr = grid->rect();
  const size_t h = f.history();
  f.d.click({static_cast<float>(gr.x + 3), static_cast<float>(gr.y + gr.h - 3)});  // the last row's first swatch
  CHECK(f.history() == h + 1);
  CHECK(f.track("Drums").colour.rgb != 0xEF7B7Bu);
  // The track menu opens on a right-click of the title.
  f.d.click(f.title("Bass"), mod::kNone, PointerButton::Right);
  REQUIRE(OverlayLayer::of(*f.arr) != nullptr);
  CHECK(OverlayLayer::of(*f.arr)->openCount() >= 1);
  CHECK(f.arr->selection().tracks.size() == 1);
  OverlayLayer::of(*f.arr)->closeAll();
  f.d.step();
  // Height: drag Bass's bottom edge down 30 px (not undoable).
  const IRect r = f.header("Bass").rect();
  const float h0 = f.arr->trackHeight(f.track("Bass").id);
  const size_t hist = f.history();
  f.d.drag({static_cast<float>(r.x + 20), static_cast<float>(r.bottom() - 2)}, {static_cast<float>(r.x + 20), static_cast<float>(r.bottom() + 28)}, 6);
  CHECK(f.arr->trackHeight(f.track("Bass").id) == doctest::Approx(h0 + 30.0f).epsilon(0.05));
  CHECK(f.history() == hist);
  // Fold the group: its members' rows go.
  const size_t rows = f.arr->rows().size();
  f.arr->toggleFold(f.track("Keys").id);
  CHECK(f.arr->rows().size() == rows - 2);
}

TEST_CASE("mixer: the mixer view (Ctrl+Alt+M): strips from the same widget, a fader drag is one paramOnly step") {
  Fixture f;
  CHECK_FALSE(f.mixer->shown());
  CHECK(f.d.press("Ctrl+Alt+M") == KeyResult::Action);
  CHECK(f.mixer->shown());
  CHECK_FALSE(f.arr->visible());
  f.d.tree().layoutNow();
  // Drums, Bass, Keys, Piano, Pad, Lead, the two returns, the master.
  REQUIRE(f.mixer->strips().size() == 9);
  CHECK(f.mixer->strips().back()->kind() == doc::TrackKind::Master);
  MixerStrip* lead = f.mixer->strip(f.track("Lead").id);
  REQUIRE(lead != nullptr);
  CHECK(lead->layoutKind() == StripLayout::Vertical);
  REQUIRE(lead->fader() != nullptr);
  CHECK(lead->tuningChip() != nullptr);
  CHECK(lead->solo()->on());
  const float before = f.track("Lead").mixer.volumeDb;
  const size_t h = f.history();
  const IRect fr = lead->fader()->thumbRect();
  const Vec2 c{static_cast<float>(fr.x + fr.w / 2), static_cast<float>(fr.y + fr.h / 2)};
  f.d.drag(c, {c.x, c.y + 40.0f}, 10);
  CHECK(f.history() == h + 1);
  CHECK(f.track("Lead").mixer.volumeDb < before);
  CHECK(f.lastEntryChanges().paramOnly());
  // The volume box of the same strip follows the fader; the header strip follows the document.
  CHECK(lead->volume()->valueText() == formatVolume(f.track("Lead").mixer.volumeDb));
  // A change elsewhere shows up in the view.
  REQUIRE(f.ctx.store().dispatch(doc::SetTrackMute{f.track("Drums").id, true}).ok());
  CHECK_FALSE(f.mixer->strip(f.track("Drums").id)->activator()->on());
  // Folding a group hides its members' strips.
  f.arr->toggleFold(f.track("Keys").id);
  REQUIRE(f.ctx.store().dispatch(doc::SetTrackMute{f.track("Drums").id, false}).ok());
  CHECK(f.mixer->strips().size() == 7);
  CHECK(f.d.press("Ctrl+Alt+M") == KeyResult::Action);
  CHECK_FALSE(f.mixer->shown());
  CHECK(f.arr->visible());
}

TEST_CASE("mixer: meters poll the MeterSource only while it is live, in one batched read; idle costs nothing") {
  Fixture f;
  FixedMeterSource src;
  MeterLevels loud;
  loud.rmsDb[0] = loud.rmsDb[1] = -10.0f;
  loud.peakDb[0] = loud.peakDb[1] = -3.0f;
  src.set(f.track("Drums").id, loud);
  f.mixer->setMeterSource(&src);
  CHECK(src.reads() == 1);  // one read when the source is set (not live: then stop)
  CHECK(f.strip("Drums").meter()->levels() == loud);
  CHECK(f.strip("Bass").meter()->levels() == MeterLevels{});
  f.d.advance(1.0);
  CHECK(src.reads() == 1);  // not live: no timer
  src.setLive(true);
  f.mixer->wakeMeters();
  f.d.advance(0.5);
  CHECK(src.reads() >= 10);  // ~30 Hz
  CHECK(src.reads() <= 20);
  const uint64_t frames = f.d.framesDrawn();
  f.d.advance(0.5);  // steady levels: no meter pixel changes, nothing drawn
  CHECK(f.d.framesDrawn() == frames);
  src.setLive(false);
  f.d.advance(0.2);
  const uint64_t r = src.reads();
  f.d.advance(1.0);
  CHECK(src.reads() == r);
  f.mixer->setMeterSource(nullptr);
}

TEST_CASE("mixer: the output chooser routes the track (one structural step)") {
  Fixture f;
  Control* out = f.strip("Drums").output();
  REQUIRE(out != nullptr);
  f.d.click(f.centre(*out));
  OverlayLayer* layer = OverlayLayer::of(*f.arr);
  REQUIRE(layer != nullptr);
  REQUIRE(layer->openCount() == 1);
  auto* menu = dynamic_cast<Menu*>(layer->top());
  REQUIRE(menu != nullptr);
  // Master, Keys, ─, Sends Only.
  REQUIRE(menu->items().size() == 4);
  CHECK(menu->items()[0].checked);
  CHECK(menu->items()[1].label == "Keys");
  const size_t h = f.history();
  menu->choose(1);
  CHECK(f.history() == h + 1);
  CHECK(f.track("Drums").mixer.output.kind == doc::OutputKind::Track);
  CHECK(f.lastEntryChanges().structural());
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

TEST_CASE("render: tracks-mixer over tests/views/fixtures/mixer matches the golden image") { checkGolden("tracks-mixer", "mixer"); }

TEST_CASE("render: mixer-view over tests/views/fixtures/mixer matches the golden image") { checkGolden("mixer-view", "mixer"); }
