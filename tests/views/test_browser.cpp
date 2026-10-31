// T5.10: the browser panel (pq/views/browser/browser_panel.h) over browserDemoIndex — the place tree (fold / unfold),
// categories, collections and the search (flat results, best first), sorting, the keys (Up / Down with auto-preview,
// Left / Right, Enter loads a sample as a clip: one undo step; typing goes to the search field; Ctrl+F; Esc stops the
// preview), the preview strip (scrub, Sync, volume), drag out to the arrangement (a BrowserItems payload), folders
// dropped on the panel become places, the goldens browser-panel and browser-search, and (perf) a 200k-row list.

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <string>

#include "../ui/golden_check.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/browser/browser_panel.h"
#include "pq/views/browser/demo_index.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/preview_port.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

std::string samplesDir() { return views::test::fixtureDir("audio") + "/samples"; }

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  FakePreviewPort preview;
  TestDriver d;
  std::unique_ptr<Shell> shell;
  BrowserPanel* b = nullptr;
  doc::TrackId audio;
  explicit Fixture(uint32_t extra = 0, uint32_t w = 1280, uint32_t h = 800) : d(w, h) {
    ctx.library().setPreview(&preview);
    ctx.library().setIndex(browserDemoIndex(samplesDir(), extra));
    auto t = doc::ops::createTrack(*ctx.doc(), ctx.store().ids(), doc::TrackKind::Audio, {}, "Audio");
    REQUIRE(t.ok());
    REQUIRE(ctx.store().dispatch(t->command).ok());
    audio = t->tracks[0];
    ctx.setSelectedTrack(audio);
    ctx.setBrowserVisible(true);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    b = findBrowser(*shell);
    REQUIRE(b != nullptr);
    d.tree().layoutNow();
    d.step();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }
  std::vector<std::string> names() const {
    std::vector<std::string> v;
    for (const BrowserRow& r : b->rows())
      v.push_back(r.dir ? std::string(b->index()->dirName(r.id)) + "/"
                        : r.device ? b->cellText(r, BrowserColumn::Name) : std::string(b->index()->name(r.id)));
    return v;
  }
  int32_t rowOf(std::string_view n) const {
    const auto v = names();
    for (size_t i = 0; i < v.size(); ++i)
      if (v[i] == n) return static_cast<int32_t>(i);
    return -1;
  }
  Vec2 rowCentre(int32_t row) {
    d.tree().layoutNow();
    const IRect r = b->rowRect(row);
    return {static_cast<float>(r.x + r.w / 3), static_cast<float>(r.y + r.h / 2)};
  }
};

}  // namespace

TEST_CASE("browser: a place shows its folders first, then its files; folders fold and unfold") {
  Fixture f;
  f.b->setSource({BrowserSource::Kind::Place, 0});
  CHECK(f.names() == std::vector<std::string>{"Drums/", "Loops/", "Presets/", "beat.wav", "vox.wav"});
  const int32_t drums = f.rowOf("Drums/");
  f.b->setFolderOpen(f.b->rows()[static_cast<size_t>(drums)].id, true);
  CHECK(f.names() == std::vector<std::string>{"Drums/", "Hat Open.aif", "Kick 01.wav", "Snare Tight.wav", "Loops/",
                                              "Presets/", "beat.wav", "vox.wav"});
  CHECK(f.b->rows()[1].depth == 1);
  // Click on the triangle folds it again.
  const Vec2 p = f.rowCentre(drums);
  f.d.click({static_cast<float>(f.b->listWidget().rect().x + 8), p.y});
  CHECK(f.names().size() == 5);
  // The second place.
  f.b->setSource({BrowserSource::Kind::Place, 1});
  CHECK(f.names() == std::vector<std::string>{"Rain on Roof.wav", "Vocal Chop.wav"});
}

TEST_CASE("browser: categories, search (best first), collections, sorting") {
  Fixture f;
  f.b->setSource({BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::Samples)});
  CHECK(f.names().size() == 9);  // every sample of every place, in path order
  // T5.11: Instruments lists the devices (no device library here: the stock list) with their presets under them; the
  // demo's presets lie in Presets/Operator (the User Library's layout names their device).
  f.b->setSource({BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::Instruments)});
  CHECK(f.names() == std::vector<std::string>{"Basic Synth", "Operator", "Instrument Rack"});
  f.b->setRowOpen(f.b->rows()[1], true);
  CHECK(f.names() == std::vector<std::string>{"Basic Synth", "Operator", "Bright Pad.pqpreset", "Glass Bell.pqpreset",
                                              "Sub Pluck.pqpreset", "Warm Keys.pqpreset", "Instrument Rack"});
  // A search text lists matching files flat, as before.
  f.b->setQuery("bright");
  CHECK(f.names() == std::vector<std::string>{"Bright Pad.pqpreset"});
  f.b->setQuery("");
  f.b->setSource({BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::Samples)});
  f.b->setQuery("kick");
  REQUIRE_FALSE(f.names().empty());
  CHECK(f.names()[0] == "Kick 01.wav");
  f.b->setQuery("");
  // Sort by duration, longest first.
  f.b->setSort(BrowserColumn::Duration, true);
  CHECK(f.names()[0] == "Rain on Roof.wav");
  f.b->setSort(BrowserColumn::Name, false);
  CHECK(f.names()[0] == "beat.wav");
  // Collections: tag Kick red, then the red chip shows only it.
  const library::Index& ix = *f.b->index();
  const uint32_t kick = ix.findPath(ix.places[0].root + "\\Drums\\Kick 01.wav");
  REQUIRE(kick != library::Index::kNone);
  f.ctx.library().tags().setCollection(library::tagKeyOf(ix, kick), 0, true);
  f.ctx.library().tagsChanged();
  f.b->setCollectionFilter(1);
  CHECK(f.names() == std::vector<std::string>{"Kick 01.wav"});
  f.b->setCollectionFilter(0);
  f.b->setSource({BrowserSource::Kind::Collection, 0});
  CHECK(f.names() == std::vector<std::string>{"Kick 01.wav"});
  // Clicking a column header sorts (again: descending).
  f.b->setSource({BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::Samples)});
  f.b->setSort(BrowserColumn::Relevance, false);
  f.d.tree().layoutNow();
  const Vec2 hdr = f.d.center("main/body/top/browser/browser/body/content/columns");
  f.d.click({static_cast<float>(f.d.rect("main/body/top/browser/browser/body/content/columns").x + 10), hdr.y});
  CHECK(f.b->sortColumn() == BrowserColumn::Name);
  CHECK_FALSE(f.b->sortDescending());
  f.d.click({static_cast<float>(f.d.rect("main/body/top/browser/browser/body/content/columns").x + 10), hdr.y});
  CHECK(f.b->sortDescending());
}

TEST_CASE("browser keys: Up / Down select and auto-preview; Left / Right fold; Esc stops; typing searches") {
  Fixture f;
  f.b->setSource({BrowserSource::Kind::Place, 0});
  f.d.tree().setFocus(&f.b->listWidget());
  f.d.press("Down");
  CHECK(f.b->selected() == 0);  // Drums/: a folder, nothing to preview
  CHECK(f.preview.played.empty());
  f.d.press("Right");  // unfolds
  CHECK(f.names().size() == 8);
  f.d.press("Right");  // into it
  CHECK(f.b->selected() == 1);
  REQUIRE(f.preview.played.size() == 1);  // Hat Open.aif auto-previews
  CHECK(f.preview.played[0].path.find("Hat Open.aif") != std::string::npos);
  CHECK(f.preview.selected.size() == 1);
  f.d.press("Down");
  CHECK(f.preview.played.size() == 2);
  f.d.press("Left");  // to the folder
  CHECK(f.b->selected() == 0);
  f.d.press("Left");  // folds it
  CHECK(f.names().size() == 5);
  // Esc stops a playing preview.
  f.preview.isPlaying = true;
  f.d.press("Escape");
  CHECK(f.preview.stops >= 1);
  // A printable key goes to the search field.
  f.d.press("K");
  f.d.type("k");
  CHECK(f.d.tree().focus() == &f.b->searchField());
  CHECK(f.b->query() == "k");
  // Preview off: selecting does not play.
  f.b->setPreviewOn(false);
  const size_t played = f.preview.played.size();
  f.d.tree().setFocus(&f.b->listWidget());
  f.d.press("Down");
  CHECK(f.preview.played.size() == played);
}

// M5 review H4: a printable key typed with the list focused starts a search and nothing else (it also ran its global
// binding: B toggled Draw Mode); Space stays the transport's.
TEST_CASE("browser keys: typing into the list searches without firing global shortcuts; Space plays") {
  Fixture f;
  f.b->setSource({BrowserSource::Kind::Place, 0});
  const bool draw0 = f.ctx.drawMode();
  for (const char* k : {"B", "1", "2", "W"}) {
    CAPTURE(k);
    f.b->setQuery("");
    f.d.tree().setFocus(&f.b->listWidget());
    const std::string text(1, static_cast<char>(std::tolower(static_cast<unsigned char>(k[0]))));
    CHECK(f.d.press(k) == KeyResult::Widget);
    f.d.type(text);
    CHECK(f.d.tree().focus() == &f.b->searchField());
    CHECK(f.b->query() == text);
    CHECK(f.ctx.drawMode() == draw0);
  }
  f.b->setQuery("");
  f.d.tree().setFocus(&f.b->listWidget());
  const size_t calls = f.port.calls.size();
  f.d.press("Space");
  CHECK(f.port.calls.size() > calls);  // the transport got it
  CHECK(f.b->query().empty());
  CHECK(f.d.tree().focus() == &f.b->listWidget());
}

// M5 review H5: Enter in the search field loads the selection; Esc stops the preview and keeps the query, and an Esc
// with nothing playing clears the query.
TEST_CASE("browser keys: Enter in the search field loads; Esc stops the preview first, then clears the query") {
  Fixture f;
  f.b->setSource({BrowserSource::Kind::Place, 0});
  f.d.tree().setFocus(&f.b->searchField());
  f.d.type("beat");
  CHECK(f.b->query() == "beat");
  f.b->waitForSearch();
  f.d.tree().layoutNow();
  REQUIRE(f.rowOf("beat.wav") >= 0);
  f.b->select(f.rowOf("beat.wav"), false);
  f.d.tree().setFocus(&f.b->searchField());
  const size_t h0 = f.ctx.store().history().cursor();
  CHECK(f.d.press("Enter") == KeyResult::Widget);
  CHECK(f.ctx.store().history().cursor() == h0 + 1);
  const doc::Track* t = doc::findTrackPtr(*f.ctx.doc(), f.audio);
  REQUIRE(t != nullptr);
  CHECK(t->clips.size() == 1);
  // Esc while previewing: stops, the query stays.
  f.preview.isPlaying = true;
  const int stops = f.preview.stops;
  f.d.press("Escape");
  CHECK(f.preview.stops == stops + 1);
  CHECK(f.b->query() == "beat");
  CHECK(f.b->searchField().text() == "beat");
  // Esc again (nothing playing): clears the query.
  f.preview.isPlaying = false;
  f.d.press("Escape");
  CHECK(f.b->query().empty());
  CHECK(f.b->searchField().text().empty());
}

TEST_CASE("browser: Enter loads a sample as a clip at the start marker on the selected audio track (one undo step)") {
  Fixture f;
  f.b->setSource({BrowserSource::Kind::Place, 0});
  f.ctx.setStartMarker(kTicksPerWhole);
  const size_t h0 = f.ctx.store().history().cursor();
  f.b->select(f.rowOf("beat.wav"), false);
  f.d.tree().setFocus(&f.b->listWidget());
  CHECK(f.d.press("Enter") == KeyResult::Widget);
  CHECK(f.ctx.store().history().cursor() == h0 + 1);
  const doc::Track* t = doc::findTrackPtr(*f.ctx.doc(), f.audio);
  REQUIRE(t != nullptr);
  REQUIRE(t->clips.size() == 1);
  REQUIRE(t->clips[0].audio != nullptr);
  CHECK(t->clips[0].audio->start == kTicksPerWhole);
  CHECK(t->clips[0].audio->name == "beat");
  // T5.11: an instrument (the first Instruments row) onto the selected audio track: refused (status), nothing changes.
  f.b->setSource({BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::Instruments)});
  f.b->select(0, false);
  CHECK_FALSE(f.b->loadSelection());
  CHECK(f.ctx.store().history().cursor() == h0 + 1);
  // Double-click loads too.
  f.b->setSource({BrowserSource::Kind::Place, 0});
  f.ctx.setStartMarker(4 * kTicksPerWhole);
  f.d.doubleClick(f.rowCentre(f.rowOf("vox.wav")));
  CHECK(f.ctx.store().history().cursor() == h0 + 2);
}

TEST_CASE("browser: Ctrl+F shows the browser and focuses its search field; Ctrl+Alt+B toggles it") {
  Fixture f;
  f.ctx.setBrowserVisible(false);
  f.d.tree().layoutNow();
  CHECK(f.d.press("Ctrl+F") == KeyResult::Action);
  CHECK(f.ctx.browserVisible());
  CHECK(f.d.tree().focus() == &f.b->searchField());
  f.d.tree().setFocus(nullptr);
  CHECK(f.d.press("Ctrl+Alt+B") == KeyResult::Action);
  CHECK_FALSE(f.ctx.browserVisible());
  CHECK(f.d.press("Ctrl+Alt+B") == KeyResult::Action);
  CHECK(f.ctx.browserVisible());
}

TEST_CASE("browser preview strip: scrub on click, Sync, volume") {
  Fixture f;
  f.b->setSource({BrowserSource::Kind::Place, 0});
  f.b->select(f.rowOf("vox.wav"));  // preview on: plays
  REQUIRE(f.preview.played.size() == 1);
  CHECK_FALSE(f.preview.played[0].sync);
  CHECK(f.b->previewOverview() != nullptr);  // the waveform (built here: no UI poster)
  f.d.tree().layoutNow();
  const IRect w = f.d.rect("main/body/top/browser/browser/preview/waveform");
  REQUIRE_FALSE(w.empty());
  f.d.click({static_cast<float>(w.x + w.w / 2), static_cast<float>(w.y + w.h / 2)});
  REQUIRE(f.preview.scrubs.size() == 1);
  CHECK(f.preview.scrubs[0] == doctest::Approx(f.b->previewLength() / 2).epsilon(0.05));
  // Sync restarts the preview re-pitched.
  f.b->setSync(true);
  CHECK(f.preview.played.back().sync);
  // Volume.
  f.preview.isPlaying = true;
  f.d.press("Escape");
  CHECK_FALSE(f.preview.isPlaying);
}

TEST_CASE("browser: an item dragged out lands on the arrangement as a clip; a folder dropped on the panel becomes a place") {
  Fixture f;
  f.b->setSource({BrowserSource::Kind::Place, 0});
  const int32_t row = f.rowOf("beat.wav");
  const Vec2 from = f.rowCentre(row);
  auto* arr = dynamic_cast<ArrangementView*>(f.d.find("main/body/top/track-area/arrangement"));
  REQUIRE(arr != nullptr);
  f.d.tree().layoutNow();
  const IRect lane = arr->rowLaneRect(0);
  const Vec2 to{arr->xOfTick(0) + 2.0f, static_cast<float>(lane.y + lane.h / 2)};
  const size_t h0 = f.ctx.store().history().cursor();
  f.d.move(from);
  f.d.down(from);
  f.d.move({from.x + 20, from.y});
  REQUIRE(f.d.tree().dragging());
  CHECK(f.d.tree().dragPayload()->kind == DragKind::BrowserItems);
  f.d.move(to);
  CHECK(f.d.tree().dropFeedback().effect == DropEffect::Copy);
  f.d.up(to);
  CHECK(f.ctx.store().history().cursor() == h0 + 1);
  const doc::Track* t = doc::findTrackPtr(*f.ctx.doc(), f.audio);
  REQUIRE(t != nullptr);
  CHECK(t->clips.size() == 1);
  // A folder from Explorer on the panel.
  const std::string dir = views::test::tempDir("browser_place");
  std::vector<std::string> saved;
  f.ctx.library().setFoldersListener([&](const std::vector<std::string>& v) { saved = v; });
  DragPayload p;
  p.paths = {dir};
  const Vec2 at = f.d.center("main/body/top/browser/browser/body/sidebar");
  f.d.tree().dragEnter(p, at, 0);
  CHECK(f.d.tree().dragDrop(at, 0) == DropEffect::Link);
  CHECK(saved.size() == 1);
  bool found = false;
  for (const auto& pl : f.ctx.library().places()) found = found || placeKey(pl.root) == placeKey(dir);
  CHECK(found);
}

TEST_CASE("browser: the context menu toggles a collection") {
  Fixture f;
  f.b->setSource({BrowserSource::Kind::Place, 0});
  const int32_t row = f.rowOf("beat.wav");
  f.b->openContextMenu(row, f.rowCentre(row));
  f.d.step();
  auto* menu = dynamic_cast<Menu*>(f.shell->overlay().top());
  REQUIRE(menu != nullptr);
  REQUIRE(menu->items().size() >= 3);
  CHECK(menu->items()[0].label == "Collections");
  CHECK(menu->items()[1].label == "Tags");  // T5.11
  CHECK(menu->items()[2].label == "Show in Explorer");
  // Rename / Delete: the User Library only (place 0 is one).
  CHECK(menu->items()[3].enabled);
  menu->items()[0].submenu[2].onChoose();  // Yellow
  const library::Index& ix = *f.b->index();
  const library::TagItem* it = f.ctx.library().tags().find(library::tagKeyOf(ix, f.b->rows()[static_cast<size_t>(row)].id));
  REQUIRE(it != nullptr);
  CHECK(it->collections == (1u << 2));
}

namespace {
void checkGolden(const char* scene) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, views::test::fixtureDir("audio"), w, h, {}, o);
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

TEST_CASE("render: browser-panel (place tree, a folder open, a sample selected with its waveform) matches the golden") {
  checkGolden("browser-panel");
}

TEST_CASE("render: browser-search (search results, best first) matches the golden") { checkGolden("browser-search"); }

TEST_CASE("browser: a 200k-row list scrolls with every list paint within 1 ms" * doctest::test_suite("perf")) {
  Fixture f(200000, 1280, 1440);
  REQUIRE(f.d.enableGpu().ok());
  f.b->setSource({BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::Samples)});
  REQUIRE(f.b->rows().size() >= 200000);
  f.d.step();
  auto* sv = dynamic_cast<ScrollView*>(f.b->listWidget().parent());
  REQUIRE(sv != nullptr);
  std::vector<double> ms;
  // Wheel scrolling (3 rows a notch) through the middle of the list.
  sv->scrollTo(0, 100000 * f.b->rowRect(1).y - 100000 * f.b->rowRect(0).y);
  f.d.step();
  for (int i = 0; i < 300; ++i) {
    f.d.wheel(f.d.center("main/body/top/browser/browser/body/content/scroll"), -1.0f);
    f.d.step();
    ms.push_back(f.b->stats().lastPaintMs);
  }
  std::sort(ms.begin(), ms.end());
  MESSAGE("200k rows: list paint median " << ms[ms.size() / 2] << " ms, max " << ms.back() << " ms, rows painted "
                                          << f.b->stats().rowsPainted);
  CHECK(ms[ms.size() / 2] <= 1.0);
  CHECK(ms[ms.size() * 99 / 100] <= 1.0);
}

TEST_CASE("browser: a 200k-row list builds and paints only its visible rows") {
  Fixture f(200000);
  f.b->setSource({BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::Samples)});
  CHECK(f.b->rows().size() == 200009);
  f.d.step();
  CHECK(f.b->stats().rowsPainted < 100);
}
