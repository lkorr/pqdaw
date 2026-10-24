// T3.7 ViewContext: project I/O (save → reopen = identical document), unsaved-changes tracking, the dialog-driven
// file actions, undo/redo through History, transport routing through the port, loop sync, grid label, layout state,
// keymap binding.

#include <doctest/doctest.h>

#include <ostream>
#include <string>

#include "pq/document/serialize.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/view_context.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::views;
using pq::views::test::ScriptedDialogs;

namespace {

void checkSameView(const doc::ViewState& a, const doc::ViewState& b) {
  REQUIRE(a.tracks.size() == b.tracks.size());
  for (const auto& [id, v] : a.tracks) {
    auto it = b.tracks.find(id);
    REQUIRE(it != b.tracks.end());
    CHECK(it->second.folded == v.folded);
    CHECK(it->second.height == v.height);
  }
  CHECK(a.lanes.size() == b.lanes.size());
}

}  // namespace

TEST_CASE("view context: picked paths map to project directories") {
  CHECK(projectDirFromPickedPath("C:\\Music\\Song\\project.pqd") == "C:\\Music\\Song");
  CHECK(projectDirFromPickedPath("C:/Music/Song/PROJECT.PQD") == "C:/Music/Song");
  CHECK(projectDirFromPickedPath("C:\\Music\\New Song.pqd") == "C:\\Music\\New Song");
  CHECK(projectDirFromPickedPath("C:\\Music\\Folder\\") == "C:\\Music\\Folder");
  CHECK(projectDirFromPickedPath("relative/dir") == "relative/dir");
  CHECK(projectDirFromPickedPath(".pqd") == ".pqd");
  CHECK(projectDirFromPickedPath("") == "");
  CHECK(projectNameOf("C:\\Music\\Song") == "Song");
  CHECK(projectNameOf("C:/Music/Song/") == "Song");
  CHECK(projectNameOf("") == "Untitled");
}

TEST_CASE("view context: a new context holds the default project, untitled and clean") {
  FakeTransport port;
  ViewContext ctx(port);
  doc::IdSource ids;
  CHECK(*ctx.doc() == *doc::makeDefaultProject(ids));
  CHECK(ctx.projectDir().empty());
  CHECK(ctx.projectName() == "Untitled");
  CHECK_FALSE(ctx.dirty());
  // The loop was handed to the transport once at construction.
  REQUIRE(port.count(FakeTransport::Call::SetLoop) == 1);
  CHECK(port.loop == TransportLoop{ctx.doc()->loop.start, ctx.doc()->loop.end, false});
}

TEST_CASE("view context: save then reopen gives an identical document") {
  FakeTransport port;
  ViewContext ctx(port);
  test::buildRichProject(ctx.store());
  CHECK(ctx.dirty());
  const doc::Snapshot before = ctx.doc();
  const doc::ViewState viewBefore = ctx.store().view();
  const std::string dir = test::tempDir("save_reopen");
  REQUIRE(ctx.saveProjectAs(dir).ok());
  CHECK_FALSE(ctx.dirty());
  CHECK(ctx.projectDir() == dir);
  CHECK(ctx.doc() == before);  // saving does not touch the document

  // A second context (as after a restart) and the same context (Ctrl+O on the open project).
  FakeTransport port2;
  ViewContext other(port2);
  REQUIRE(other.openProject(dir).ok());
  CHECK(*other.doc() == *before);
  checkSameView(other.store().view(), viewBefore);
  CHECK_FALSE(other.dirty());
  CHECK(other.store().history().entries().empty());
  CHECK(other.projectName() == pq::views::projectNameOf(dir));

  REQUIRE(ctx.openProject(dir).ok());
  CHECK(*ctx.doc() == *before);
  checkSameView(ctx.store().view(), viewBefore);
  // New ids continue after the loaded document's largest id.
  CHECK(ctx.store().ids().peek() > doc::maxEntityId(*ctx.doc()));
}

TEST_CASE("view context: unsaved changes follow the history (undo back to the saved state is clean)") {
  FakeTransport port;
  ViewContext ctx(port);
  int projectNotes = 0;
  auto sub = ctx.observe([&](uint32_t w) { projectNotes += (w & view_change::kProject) ? 1 : 0; });
  const std::string dir = test::tempDir("dirty");
  REQUIRE(ctx.saveProjectAs(dir).ok());
  projectNotes = 0;
  REQUIRE(ctx.store().dispatch(doc::SetLoop{{0, kTicksPerWhole, true}}).ok());
  CHECK(ctx.dirty());
  CHECK(projectNotes == 1);
  REQUIRE(ctx.undo());
  CHECK_FALSE(ctx.dirty());
  CHECK(projectNotes == 2);
  REQUIRE(ctx.redo());
  CHECK(ctx.dirty());
  REQUIRE(ctx.saveProject().ok());
  CHECK_FALSE(ctx.dirty());
  CHECK(projectNotes == 4);
}

TEST_CASE("view context: undo and redo go through the History") {
  FakeTransport port;
  ViewContext ctx(port);
  const doc::Snapshot s0 = ctx.doc();
  REQUIRE(ctx.store().dispatch(doc::SetLoop{{0, kTicksPerWhole, true}}).ok());
  const doc::Snapshot s1 = ctx.doc();
  REQUIRE(ctx.store().dispatch(doc::SetTempoMap{{{0, 99.0, TempoCurve::Step}}}).ok());
  const doc::Snapshot s2 = ctx.doc();
  CHECK(ctx.store().history().cursor() == 2);
  REQUIRE(ctx.undo());
  CHECK(ctx.doc() == s1);  // the stored snapshot pointer, not a re-application
  CHECK(ctx.status() == "Undo Tempo");
  REQUIRE(ctx.undo());
  CHECK(ctx.doc() == s0);
  CHECK_FALSE(ctx.undo());
  REQUIRE(ctx.redo());
  REQUIRE(ctx.redo());
  CHECK(ctx.doc() == s2);
  CHECK_FALSE(ctx.redo());
  // During a gesture undo is refused (and the action is disabled, so its chord falls through).
  REQUIRE(ctx.store().beginGesture("drag").ok());
  CHECK_FALSE(ctx.undo());
  CHECK_FALSE(ctx.actions().enabled(ctx.actions().find("edit.undo")));
  REQUIRE(ctx.store().commitGesture().ok());
  CHECK(ctx.actions().enabled(ctx.actions().find("edit.undo")));
}

TEST_CASE("view context: transport requests go to the port") {
  FakeTransport port;
  ViewContext ctx(port);
  port.calls.clear();
  ctx.setStartMarker(2 * kTicksPerWhole);
  ctx.togglePlay();  // Space while stopped: play from the start marker
  REQUIRE(port.calls.size() == 1);
  CHECK(port.calls[0].call == FakeTransport::Call::Play);
  CHECK(port.calls[0].tick == 2 * kTicksPerWhole);
  CHECK(ctx.playing());
  port.setPlayheadTick(3 * kTicksPerWhole);
  ctx.togglePlay();  // Space while playing: stop
  CHECK(port.calls.back().call == FakeTransport::Call::Stop);
  CHECK_FALSE(ctx.playing());
  ctx.continuePlayback();  // Shift+Space
  CHECK(port.calls.back().call == FakeTransport::Call::Continue);
  CHECK(port.playhead().tick == 3 * kTicksPerWhole);
  ctx.continuePlayback();  // already playing: nothing
  CHECK(port.count(FakeTransport::Call::Continue) == 1);
  ctx.stopButton();  // stop
  CHECK(port.calls.back().call == FakeTransport::Call::Stop);
  CHECK(ctx.startMarker() == 2 * kTicksPerWhole);
  ctx.stopButton();  // stop again while stopped: back to the beginning
  CHECK(port.calls.back().call == FakeTransport::Call::Locate);
  CHECK(port.calls.back().tick == 0);
  CHECK(ctx.startMarker() == 0);
}

TEST_CASE("view context: the transport loop follows the document (edits, undo, open)") {
  FakeTransport port;
  ViewContext ctx(port);
  port.calls.clear();
  REQUIRE(ctx.store().dispatch(doc::SetLoop{{kTicksPerWhole, 2 * kTicksPerWhole, true}}).ok());
  REQUIRE(port.count(FakeTransport::Call::SetLoop) == 1);
  CHECK(port.loop == TransportLoop{kTicksPerWhole, 2 * kTicksPerWhole, true});
  REQUIRE(ctx.store().dispatch(doc::SetTempoMap{{{0, 99.0, TempoCurve::Step}}}).ok());
  CHECK(port.count(FakeTransport::Call::SetLoop) == 1);  // unrelated edits do not resend it
  REQUIRE(ctx.undo());
  REQUIRE(ctx.undo());
  CHECK(port.loop == TransportLoop{0, 4 * kTicksPerWhole, false});
  REQUIRE(ctx.openProject(test::fixtureDir("small")).ok());
  CHECK(port.loop == TransportLoop{7 * kTicksPerWhole / 8, 4 * 7 * kTicksPerWhole / 8, true});  // bar 2 → bar 5 of 7/8
}

TEST_CASE("view context: opening stops playback, resets the start marker and reports errors") {
  FakeTransport port;
  ViewContext ctx(port);
  ctx.setStartMarker(kTicksPerWhole);
  ctx.togglePlay();
  REQUIRE(ctx.playing());
  REQUIRE(ctx.openProject(test::fixtureDir("empty")).ok());
  CHECK_FALSE(ctx.playing());
  CHECK(ctx.startMarker() == 0);
  CHECK(ctx.status() == "Opened empty");
  const doc::Snapshot loaded = ctx.doc();
  auto bad = ctx.openProject(test::fixtureDir("does-not-exist"));
  CHECK_FALSE(bad.ok());
  CHECK(ctx.doc() == loaded);  // unchanged
  CHECK(ctx.status().rfind("Could not open", 0) == 0);
}

TEST_CASE("view context: file actions use the dialogs; unsaved changes are asked about") {
  FakeTransport port;
  ViewContext ctx(port);
  ScriptedDialogs dlg;
  // Without dialogs, open / save-as are disabled and save of an untitled project reports it.
  CHECK_FALSE(ctx.actions().enabled(ctx.actions().find("file.open")));
  CHECK_FALSE(ctx.actions().enabled(ctx.actions().find("file.saveAs")));
  ctx.setFileDialogs(&dlg);
  CHECK(ctx.actions().enabled(ctx.actions().find("file.open")));

  // Ctrl+S on an untitled project = Save As.
  dlg.savePath = test::tempDir("dialog_save") + "/project.pqd";
  REQUIRE(ctx.store().dispatch(doc::SetLoop{{0, kTicksPerWhole, true}}).ok());
  REQUIRE(ctx.actions().invoke(ctx.actions().find("file.save")));
  CHECK(dlg.saves == 1);
  CHECK(ctx.projectDir() == projectDirFromPickedPath(dlg.savePath));
  CHECK_FALSE(ctx.dirty());
  // Ctrl+S on a titled project saves without asking.
  REQUIRE(ctx.store().dispatch(doc::SetLoop{{0, 2 * kTicksPerWhole, true}}).ok());
  ctx.actions().invoke(ctx.actions().find("file.save"));
  CHECK(dlg.saves == 1);
  CHECK_FALSE(ctx.dirty());

  // Ctrl+N with unsaved changes: Cancel keeps everything.
  REQUIRE(ctx.store().dispatch(doc::SetLoop{{0, 3 * kTicksPerWhole, true}}).ok());
  const doc::Snapshot edited = ctx.doc();
  dlg.choice = SaveChoice::Cancel;
  ctx.actions().invoke(ctx.actions().find("file.new"));
  CHECK(dlg.asks == 1);
  CHECK(ctx.doc() == edited);
  // Save: saves to the project, then starts the new one.
  dlg.choice = SaveChoice::Save;
  ctx.actions().invoke(ctx.actions().find("file.new"));
  CHECK(dlg.asks == 2);
  CHECK(ctx.projectDir().empty());
  CHECK_FALSE(ctx.dirty());
  FakeTransport p2;
  ViewContext check(p2);
  REQUIRE(check.openProject(projectDirFromPickedPath(dlg.savePath)).ok());
  CHECK(*check.doc() == *edited);

  // Ctrl+O: picks a project.pqd; Discard drops the unsaved edit.
  REQUIRE(ctx.store().dispatch(doc::SetLoop{{0, 5 * kTicksPerWhole, true}}).ok());
  dlg.choice = SaveChoice::Discard;
  dlg.openPath = test::fixtureDir("small") + "/project.pqd";
  ctx.actions().invoke(ctx.actions().find("file.open"));
  CHECK(dlg.opens == 1);
  CHECK(ctx.projectName() == "small");
  CHECK(ctx.doc()->timeline->tempo.front().bpm == 96.5);
  // A cancelled dialog (empty path) does nothing.
  dlg.openPath.clear();
  const doc::Snapshot s = ctx.doc();
  ctx.actions().invoke(ctx.actions().find("file.open"));
  CHECK(ctx.doc() == s);

  // Quit: nothing unsaved → go ahead; unsaved + Cancel → stay; unsaved + Save on a titled project → saved, go ahead.
  CHECK(ctx.confirmQuit({}));
  REQUIRE(ctx.store().dispatch(doc::SetLoop{{0, 6 * kTicksPerWhole, true}}).ok());
  dlg.choice = SaveChoice::Cancel;
  CHECK_FALSE(ctx.confirmQuit({}));
  ctx.newProject();
  REQUIRE(ctx.store().dispatch(doc::SetLoop{{0, 6 * kTicksPerWhole, true}}).ok());
  dlg.choice = SaveChoice::Save;  // untitled: Save As runs, then the continuation (the app pushes its quit)
  dlg.savePath = test::tempDir("dialog_quit");
  int quits = 0;
  CHECK_FALSE(ctx.confirmQuit([&] { ++quits; }));
  CHECK(quits == 1);
  CHECK_FALSE(ctx.dirty());
}

TEST_CASE("view context: grid label") {
  FakeTransport port;
  ViewContext ctx(port);
  CHECK(ctx.gridLabel() == "Adaptive (Medium)");
  GridSpec g;
  g.mode = GridMode::Fixed;
  g.fixedExp = 3;
  ctx.setGrid(g);
  CHECK(ctx.gridLabel() == "1/8");
  g.tuplet = 3;
  ctx.setGrid(g);
  CHECK(ctx.gridLabel() == "1/8T");
  g.snap = false;
  ctx.setGrid(g);
  CHECK(ctx.gridLabel() == "1/8T \xC2\xB7 Snap off");
  g.fixedExp = -2;
  g.tuplet = 1;
  g.snap = true;
  ctx.setGrid(g);
  CHECK(ctx.gridLabel() == "4 Bars");
  g.off = true;
  ctx.setGrid(g);
  CHECK(ctx.gridLabel() == "Off");
  ctx.setGrid(GridSpec{});
  ctx.setResolvedGrid(GridDivision{0, 16, 1});
  CHECK(ctx.gridLabel() == "1/16");
}

TEST_CASE("view context: layout state and its notifications") {
  FakeTransport port;
  ViewContext ctx(port);
  uint32_t seen = 0;
  int calls = 0;
  auto sub = ctx.observe([&](uint32_t w) {
    seen |= w;
    ++calls;
  });
  // T3.x-fix-look: the detail view starts collapsed (Live: it opens when a clip is opened); the first toggle shows it.
  CHECK_FALSE(ctx.detailVisible());
  CHECK(ctx.detailMode() == DetailMode::Clip);
  ctx.toggleClipDevice();
  CHECK(ctx.detailVisible());
  CHECK(ctx.detailMode() == DetailMode::Clip);
  ctx.toggleClipDevice();
  CHECK(ctx.detailMode() == DetailMode::Device);
  ctx.toggleClipDevice();
  CHECK(ctx.detailMode() == DetailMode::Clip);
  ctx.setDetailVisible(false);
  ctx.toggleClipDevice();  // shows the detail view again, same mode
  CHECK(ctx.detailVisible());
  CHECK(ctx.detailMode() == DetailMode::Clip);
  CHECK(seen == view_change::kLayout);
  CHECK(calls == 5);
  sub.reset();
  ctx.toggleClipDevice();
  CHECK(calls == 5);  // unsubscribed
}

TEST_CASE("view context: the default keymap binds every implemented action; scope ids are stable") {
  FakeTransport port;
  ViewContext ctx(port);
  const ui::ScopeId arr = ctx.scope("arrangement");
  // Views register their actions before the keymap is applied (the shell installs them first): since T3.8 the
  // arrangement's (Z / X / zoom / follow) are implemented bindings, so a bare context cannot bind the default file.
  CHECK_FALSE(ctx.applyDefaultKeymap().ok());
  ArrangementView arrangement(ctx);
  REQUIRE(ctx.applyDefaultKeymap().ok());
  CHECK(ctx.keymap().lookup(arr, *ui::parseChord("Z")) == "arrangement.zoomToSelection");
  CHECK(ctx.scope("arrangement") == arr);
  for (std::string_view id : ViewContext::kGlobalActions) CHECK_MESSAGE(ctx.actions().find(id) != ui::kNoAction, id);
  auto chord = [](std::string_view c) { return *ui::parseChord(c); };
  CHECK(ctx.keymap().lookup(ui::kGlobalScope, chord("Space")) == "transport.playStop");
  CHECK(ctx.keymap().lookup(ui::kGlobalScope, chord("Shift+Space")) == "transport.continue");
  CHECK(ctx.keymap().lookup(ui::kGlobalScope, chord("Ctrl+Z")) == "edit.undo");
  CHECK(ctx.keymap().lookup(ui::kGlobalScope, chord("Ctrl+Shift+Z")) == "edit.redo");
  CHECK(ctx.keymap().lookup(ui::kGlobalScope, chord("Ctrl+Y")) == "edit.redo");
  CHECK(ctx.keymap().lookup(ui::kGlobalScope, chord("Ctrl+S")) == "file.save");
  CHECK(ctx.keymap().lookup(ui::kGlobalScope, chord("Shift+Tab")) == "view.toggleClipDevice");
  // Deferred bindings stay unbound: Ctrl+T (audio tracks, M6) since T3.x-fix-look follows Live 12; the MIDI track is
  // Ctrl+Shift+T.
  CHECK(ctx.keymap().lookup(ui::kGlobalScope, chord("Ctrl+T")).empty());
  CHECK(ctx.keymap().lookup(ui::kGlobalScope, chord("Ctrl+Shift+T")) == "track.newMidi");
  // Applying again (after more views registered actions) replaces the bindings instead of failing on duplicates.
  REQUIRE(ctx.applyDefaultKeymap().ok());
  CHECK(ctx.scope("arrangement") == arr);
  // An implemented binding without a registered action fails.
  ui::KeymapFile f;
  f.entries.push_back({"global", "Ctrl+Q", *ui::parseChord("Ctrl+Q"), "nobody.registered", "implemented", ""});
  CHECK_FALSE(ctx.applyKeymap(f).ok());
}
