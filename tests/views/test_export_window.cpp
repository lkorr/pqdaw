// EX.2: the Export Audio window over a scripted ExportPort (FakeExportPort), through real window dispatch: Ctrl+Shift+R
// opens and closes it (disabled without a port); choosers and toggles change the remembered choices (ViewPrefs, the
// listener the app saves with); Export hands the port the matching job; progress shows while it runs, Cancel cancels,
// the done line says how it ended, and the window polls only while a job runs; Selected Tracks / Time Selection use the
// arrangement's selection; a window closed during an export leaves the host polling until the status line can say how
// it ended; the golden export-audio.
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/export/export_window.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr const char* kArr = "main/body/top/track-area/arrangement";

// Answers the folder picker at once.
struct PickFolder final : FileDialogs {
  std::string next;
  void chooseProjectToOpen(PathCallback done) override { done(std::string()); }
  void chooseProjectToSave(std::string_view, PathCallback done) override { done(std::string()); }
  SaveChoice askSaveChanges(std::string_view) override { return SaveChoice::Cancel; }
  void chooseFolder(PathCallback done) override { done(next); }
};

struct Fx {
  FakeTransport transport;
  ViewContext ctx{transport};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  FakeExportPort port;
  PickFolder dialogs;
  std::vector<ViewPrefs> saved;  // what the app's listener would have written

  explicit Fx(bool withPort = true) {
    REQUIRE(ctx.openProject(views::test::fixtureDir("arrangement")).ok());
    port.rate = 44100;
    if (withPort) ctx.setExportPort(&port);
    ctx.setFileDialogs(&dialogs);
    ctx.setViewPrefsListener([this](const ViewPrefs& p) { saved.push_back(p); });
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
  }
  ~Fx() {
    d.tree().setRoot(std::make_unique<Widget>());
    ctx.setFileDialogs(nullptr);
  }

  ExportWindow* window() const { return findExportWindow(shell->overlay()); }
  ExportWindow& open() {
    d.press("Ctrl+Shift+R");
    d.step();
    ExportWindow* w = window();
    REQUIRE(w != nullptr);
    d.tree().layoutNow();
    return *w;
  }
  void click(const Widget* w) {
    REQUIRE(w != nullptr);
    REQUIRE(w->visible());
    const IRect r = w->rect();
    d.click({static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)});
    d.step();
  }
  // A real click on the chooser, then on its menu item.
  void pick(Dropdown* dd, std::string_view item) {
    click(dd);
    views::test::clickMenuItem(d, shell->overlay(), item);
    d.tree().layoutNow();
  }
  const ExportPrefs& prefs() const { return ctx.viewPrefs().exportAudio; }
  ArrangementView& arrangement() const {
    auto* a = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(a != nullptr);
    return *a;
  }
};

}  // namespace

TEST_CASE("export: Ctrl+Shift+R opens and closes the window; without a port the action is disabled") {
  {
    Fx f;
    CHECK(f.window() == nullptr);
    ExportWindow& w = f.open();
    CHECK_FALSE(w.polling());  // idle: no timer
    f.d.press("Ctrl+Shift+R");  // again: closes it
    f.d.step();
    CHECK(f.window() == nullptr);
    f.open();
    f.d.press("Escape");
    f.d.step();
    CHECK(f.window() == nullptr);
    // A click outside does not dismiss it (a dialog): only Close / Esc.
    ExportWindow& again = f.open();
    f.d.click({4.0f, 790.0f});
    f.d.step();
    CHECK(f.window() == &again);
    f.click(again.find("header/close"));
    CHECK(f.window() == nullptr);
  }
  {
    Fx f(false);
    CHECK_FALSE(f.ctx.actions().enabled(f.ctx.actions().find("file.exportAudio")));
    f.d.press("Ctrl+Shift+R");
    f.d.step();
    CHECK(f.window() == nullptr);
  }
}

TEST_CASE("export: the rows change the remembered choices; Export starts the matching job") {
  Fx f;
  ExportWindow& w = f.open();
  // Defaults: the master, the song, the engine's rate, 24-bit TPDF; the project's folder and name.
  CHECK(w.rateChooser()->items()[static_cast<size_t>(w.rateChooser()->index())] == "44100");
  CHECK(w.nameField()->text() == "arrangement");
  CHECK(w.folderText() == f.ctx.projectDir());
  CHECK_FALSE(w.returnsToggle()->enabled());  // the master has no returns of its own
  CHECK(w.ditherChooser()->enabled());
  CHECK(w.rangeText().rfind("Start 1. 1. 1", 0) == 0);
  CHECK(w.rangeText().find("+ tail") != std::string::npos);
  CHECK(w.exportButton()->enabled());

  f.pick(w.tracksChooser(), "All Individual Tracks");
  CHECK(f.prefs().tracks == ExportTracks::AllTracks);
  CHECK(w.returnsToggle()->enabled());
  f.click(w.returnsToggle());
  CHECK(f.prefs().includeReturnsMaster);
  f.pick(w.rangeChooser(), "Loop");
  CHECK(f.prefs().range == ExportRange::Loop);
  CHECK(w.rangeText().find("+ tail") == std::string::npos);
  f.click(w.loopToggle());
  CHECK(f.prefs().renderAsLoop);
  f.pick(w.rateChooser(), "96000");
  CHECK(f.prefs().sampleRate == 96000);
  f.pick(w.bitsChooser(), "16");
  CHECK(f.prefs().bits == ExportBitDepth::Pcm16);
  f.pick(w.ditherChooser(), "Shaped");
  CHECK(f.prefs().dither == ExportDither::Shaped);
  f.click(w.normalizeToggle());
  CHECK(f.prefs().normalize);
  f.dialogs.next = "E:\\Bounces";
  f.click(w.browseButton());
  CHECK(f.prefs().folder == "E:\\Bounces");
  CHECK(w.folderText() == "E:\\Bounces");
  // The name: typed into the field.
  f.click(w.nameField());
  f.d.press("Ctrl+A");
  f.d.type("Mix");
  f.d.press("Return");
  CHECK(w.nameField()->text() == "Mix");
  // Each choice reached the app's listener (it saves the settings file).
  REQUIRE_FALSE(f.saved.empty());
  CHECK(f.saved.back().exportAudio == f.prefs());

  f.click(w.exportButton());
  REQUIRE(f.port.started.size() == 1);
  const ExportJob& j = f.port.started[0];
  const doc::Project& p = *f.ctx.doc();
  CHECK(j.snapshot == f.ctx.doc());
  CHECK(j.from == p.loop.start);
  CHECK(j.to == p.loop.end);
  CHECK_FALSE(j.song);
  CHECK(j.tracks == ExportTracks::AllTracks);
  CHECK(j.includeReturnsMaster);
  CHECK(j.renderAsLoop);
  CHECK(j.normalize);
  CHECK(j.sampleRate == 96000);
  CHECK(j.bits == ExportBitDepth::Pcm16);
  CHECK(j.dither == ExportDither::Shaped);
  CHECK(j.format == ExportFileFormat::Wav);
  CHECK(j.folder == "E:\\Bounces");
  CHECK(j.baseName == "Mix");
  CHECK(j.projectDir == f.ctx.projectDir());
}

TEST_CASE("export: 32-bit float disables Dither (the job has none); the song range carries the tail flag") {
  Fx f;
  ExportWindow& w = f.open();
  f.pick(w.bitsChooser(), "32 (float)");
  CHECK_FALSE(w.ditherChooser()->enabled());
  f.click(w.exportButton());
  REQUIRE(f.port.started.size() == 1);
  const ExportJob& j = f.port.started[0];
  CHECK(j.bits == ExportBitDepth::Float32);
  CHECK(j.dither == ExportDither::None);
  CHECK(j.song);
  CHECK(j.from == 0);
  CHECK(j.to == exportSongEnd(*f.ctx.doc()));
  CHECK(j.to > 0);
  CHECK(j.tracks == ExportTracks::Master);
  CHECK(j.sampleRate == 44100);  // the engine's
  CHECK(j.baseName == "arrangement");
}

TEST_CASE("export: progress shows while the job runs; Cancel cancels; the done line; polling only while running") {
  Fx f;
  ExportWindow& w = f.open();
  f.click(w.exportButton());
  REQUIRE(f.port.started.size() == 1);
  CHECK(w.polling());
  CHECK(w.cancelButton()->visible());
  CHECK_FALSE(w.exportButton()->visible());
  CHECK_FALSE(w.tracksChooser()->enabled());  // the rows are locked while it runs
  f.port.setProgress(0.4f);
  f.d.advance(0.1);
  CHECK(w.progressShown() == doctest::Approx(0.4f));
  CHECK(w.find("footer/progress")->visible());
  f.click(w.cancelButton());
  CHECK(f.port.cancels == 1);
  f.port.endCancelled();
  f.d.advance(0.1);
  CHECK_FALSE(w.polling());
  CHECK(w.exportButton()->visible());
  CHECK_FALSE(w.cancelButton()->visible());
  CHECK(w.tracksChooser()->enabled());
  CHECK(w.doneText() == "Export cancelled: no files written.");
  // A second export that finishes.
  f.click(w.exportButton());
  REQUIRE(f.port.started.size() == 2);
  f.port.finish({"C:\\Out\\arrangement.wav"});
  f.d.advance(0.1);
  CHECK_FALSE(w.polling());
  CHECK(w.doneText() == "Exported arrangement.wav to C:\\Out");
  // A failure says why.
  f.click(w.exportButton());
  f.port.fail("export: cannot create X:\\");
  f.d.advance(0.1);
  CHECK(w.doneText() == "Export failed: export: cannot create X:\\");
}

TEST_CASE("export: Selected Tracks and Time Selection use the arrangement's selection") {
  Fx f;
  const doc::Project& p = *f.ctx.doc();
  REQUIRE(p.tracks.size() >= 3);
  const doc::TrackId a = p.tracks[0]->id, b = p.tracks[2]->id;
  ArrangementView::Selection sel;
  sel.tracks = {a, b, p.master->id};  // the master row is left out
  sel.time = TickRange{kTicksPerWhole, 3 * kTicksPerWhole};
  sel.range = true;
  f.arrangement().setSelection(sel);
  ExportWindow& w = f.open();
  f.pick(w.tracksChooser(), "Selected Tracks");
  f.pick(w.rangeChooser(), "Time Selection");
  CHECK(w.rangeText() == "Start 2. 1. 1  \xC2\xB7  Length 2. 0. 0  \xC2\xB7  2 tracks");
  f.click(w.exportButton());
  REQUIRE(f.port.started.size() == 1);
  const ExportJob& j = f.port.started[0];
  CHECK(j.tracks == ExportTracks::Selected);
  CHECK(j.trackIds == std::vector<doc::TrackId>{a, b});
  CHECK(j.from == kTicksPerWhole);
  CHECK(j.to == 3 * kTicksPerWhole);
  CHECK_FALSE(j.song);
}

TEST_CASE("export: without a selection Selected Tracks / Time Selection cannot export and say why") {
  Fx f;
  f.arrangement().clearSelection();
  f.ctx.setSelectedTrack({});
  ExportWindow& w = f.open();
  f.pick(w.rangeChooser(), "Time Selection");
  CHECK_FALSE(w.exportButton()->enabled());
  CHECK(w.doneText() == "No time selection: select a range in the arrangement first.");
  f.pick(w.rangeChooser(), "Song");
  f.pick(w.tracksChooser(), "Selected Tracks");
  CHECK_FALSE(w.exportButton()->enabled());
  CHECK(w.doneText() == "No tracks selected: select tracks in the arrangement first.");
  CHECK_FALSE(w.exportNow());
  CHECK(f.port.started.empty());
}

TEST_CASE("export: choices persist across windows; a window closed mid-export leaves the result in the status line") {
  Fx f;
  {
    ExportWindow& w = f.open();
    f.pick(w.tracksChooser(), "All Individual Tracks");
    f.pick(w.bitsChooser(), "32 (float)");
    f.d.press("Escape");
    f.d.step();
  }
  ExportWindow& w = f.open();
  CHECK(w.tracksChooser()->index() == static_cast<int32_t>(ExportTracks::AllTracks));
  CHECK(w.bitsChooser()->index() == static_cast<int32_t>(ExportBitDepth::Float32));
  f.click(w.exportButton());
  REQUIRE(f.port.started.size() == 1);
  f.d.press("Escape");  // closed while it runs: the export goes on
  f.d.step();
  CHECK(f.window() == nullptr);
  CHECK(f.port.cancels == 0);
  {  // reopened while it runs: the progress, polled again
    f.port.setProgress(0.7f);
    ExportWindow& during = f.open();
    CHECK(during.polling());
    CHECK(during.cancelButton()->visible());
    f.d.advance(0.1);
    CHECK(during.progressShown() == doctest::Approx(0.7f));
    f.d.press("Escape");
    f.d.step();
    CHECK(f.window() == nullptr);
  }
  f.port.finish({"C:\\Out\\arrangement Drums.wav", "C:\\Out\\arrangement Bass.wav"});
  f.d.advance(0.5);
  CHECK(f.ctx.status() == "Exported 2 files to C:\\Out");
  // Reopened, the window shows the finished job.
  ExportWindow& again = f.open();
  CHECK(again.doneText() == "Exported 2 files to C:\\Out");
}

TEST_CASE("render: export-audio (All Individual Tracks, Song, 24-bit TPDF, normalized) matches the golden image") {
  const char* scene = "export-audio";
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, views::test::fixtureDir("arrangement"), w, h, {}, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &d));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(d));
}
