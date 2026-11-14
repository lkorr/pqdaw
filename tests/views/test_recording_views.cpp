// T6.7: the views' side of recording, through real dispatch — an audio strip's input chooser (none, mono inputs, stereo
// pairs; channels the device lacks marked; one structural undo step) and its arm button; the arrangement's live
// recording overlay (read with the playhead while recording, painted over the rows, dropped when the session ends).
// M6 review L13: a crashed run's takes offered at start (Open as Project / Show in Explorer / Discard / Later).
#include <doctest/doctest.h>

#include <filesystem>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/samples.h"
#include "pq/document/serialize.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/shell/recover_recordings.h"
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

// ---- M6 review L13: recordings of a crashed run, offered at start ----------------------------------------------------

namespace {

namespace fs = std::filesystem;

std::string u8s(const fs::path& p) {
  const std::u8string u = p.u8string();
  return {reinterpret_cast<const char*>(u.data()), u.size()};
}

// An untitled context whose Unsaved root (a temp folder) holds an ended session "111-1000" with two takes; the hooks
// behave like ProjectLink's (the folder is listed while it holds .wav files; discardStale deletes it and counts).
struct Stale {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  fs::path root, session;
  int fresh = 0, discards = 0;
  std::optional<ViewContext::StaleAutosave> autosave;  // AS.2: listed while <session>/autosave/project.pqd exists
  Stale() {
    root = fs::temp_directory_path() / ("pq_l13_views_" + std::to_string(std::random_device{}())) / "Unsaved";
    session = root / "111-1000";
    fs::create_directories(session / "1");
    for (const char* f : {"beat.wav", "vox.wav"})
      fs::copy_file(fs::path(views::test::fixtureDir("audio")) / "samples" / f, session / "1" / f);
    ViewContext::UnsavedRecordings u;
    u.root = u8s(root);
    u.current = u8s(root / "222-2000" / "1");
    u.fresh = [this] { return u8s(root / "222-2000" / std::to_string(2 + fresh++)); };
    u.discard = [](const std::string&) {};
    u.findStale = [this] {
      std::vector<ViewContext::StaleRecordings> v;
      std::error_code ec;
      if (!fs::exists(session, ec)) return v;
      ViewContext::StaleRecordings s;
      s.dir = u8s(session);
      for (const auto& e : fs::recursive_directory_iterator(session, ec))
        if (e.path().extension() == ".wav") s.files.push_back(u8s(e.path()));
      s.started = 1000;
      if (autosave && fs::exists(session / "autosave" / "project.pqd", ec)) s.autosave = autosave;
      if (!s.files.empty() || s.autosave) v.push_back(std::move(s));
      return v;
    };
    u.discardStale = [this](const std::string& dir) {
      ++discards;
      std::error_code ec;
      return fs::remove_all(fs::path(std::u8string(dir.begin(), dir.end())), ec) > 0;
    };
    ctx.setUnsavedRecordings(std::move(u));
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
  }
  ~Stale() {
    d.tree().setRoot(std::make_unique<Widget>());
    std::error_code ec;
    fs::remove_all(root.parent_path(), ec);
  }
  // AS.2: the crashed run's autosave of `p` (as the Autosaver writes it) and its meta.
  void writeAutosave(const doc::Project& p, const std::string& originDir, const std::string& name) {
    REQUIRE(doc::saveProject(u8s(session / "autosave"), p).ok());
    ViewContext::StaleAutosave a;
    a.dir = u8s(session / "autosave");
    a.originDir = originDir;
    if (originDir.empty()) a.recordingsDir = u8s(session / "1");
    a.name = name;
    a.savedAt = static_cast<int64_t>(std::time(nullptr));
    autosave = std::move(a);
  }
  RecoverRecordingsPopover* popover() const { return dynamic_cast<RecoverRecordingsPopover*>(shell->overlay().top()); }
  void click(const char* button) {
    d.click(d.center(std::string("overlay/recover-recordings/buttons/") + button));
    d.step();
  }
};

}  // namespace

TEST_CASE("M6 review L13: a crashed run's takes are offered at start; Open as Project places them in a new project") {
  Stale f;
  RecoverRecordingsPopover* p = offerRecordingRecovery(*f.shell);
  REQUIRE(p != nullptr);
  f.d.step();
  CHECK(p->rect().y >= f.d.find("main/body")->rect().y);  // over the workspace, not the control bar
  CHECK(p->current().files.size() == 2);
  f.click("open");
  CHECK(f.popover() == nullptr);
  const doc::Track* rec = nullptr;
  doc::forEachTrack(*f.ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (t.name == "Recovered") rec = &t;
  });
  REQUIRE(rec != nullptr);
  CHECK(rec->kind == doc::TrackKind::Audio);
  CHECK(rec->clips.size() == 2);
  CHECK(rec->clips[0].audio->start + rec->clips[0].audio->length <= rec->clips[1].audio->start);  // one after another
  CHECK(f.ctx.store().history().cursor() == 1);  // one undo step
  // Moved into the new untitled project's own folder (its first save moves them on); the emptied folder went.
  const fs::path into(std::u8string(f.ctx.unsavedRecordingDir().begin(), f.ctx.unsavedRecordingDir().end()));
  CHECK(fs::exists(into / "beat.wav"));
  CHECK(fs::exists(into / "vox.wav"));
  CHECK_FALSE(fs::exists(f.session));
  CHECK(f.discards == 1);
  CHECK(offerRecordingRecovery(*f.shell) == nullptr);  // nothing left to offer
}

TEST_CASE("M6 review L13: Discard deletes the folder; Later and Esc keep it (offered again)") {
  {
    Stale f;
    const doc::Snapshot before = f.ctx.doc();
    REQUIRE(offerRecordingRecovery(*f.shell) != nullptr);
    f.d.step();
    f.click("later");
    CHECK(f.popover() == nullptr);
    CHECK(f.discards == 0);
    CHECK(fs::exists(f.session / "1" / "beat.wav"));
    REQUIRE(offerRecordingRecovery(*f.shell) != nullptr);  // the next start
    f.d.step();
    f.d.press("Escape");
    f.d.step();
    CHECK(f.popover() == nullptr);
    CHECK(fs::exists(f.session / "1" / "beat.wav"));
    // Show in Explorer changes nothing (the scene's dialogs are none here: the popover just stays).
    REQUIRE(offerRecordingRecovery(*f.shell) != nullptr);
    f.d.step();
    f.click("reveal");
    CHECK(f.popover() != nullptr);
    f.click("discard");
    CHECK(f.popover() == nullptr);
    CHECK(f.discards == 1);
    CHECK_FALSE(fs::exists(f.session));
    CHECK(f.ctx.doc() == before);  // the document untouched
  }
}

namespace {

// A sample entry for `file` stored as `stored` (absolute, or relative to the project).
doc::SampleRef sampleFor(doc::DocumentStore& st, const fs::path& file, std::string stored) {
  doc::SampleRef r;
  r.id = st.ids().sample();
  r.path = std::move(stored);
  r.size = fs::file_size(file);
  r.frames = 1000;
  r.channels = 1;
  r.displayName = u8s(file.stem());
  return r;
}

const doc::Track* trackNamed(const doc::Project& p, std::string_view name) {
  const doc::Track* found = nullptr;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (t.name == name) found = &t;
  });
  return found;
}

}  // namespace

TEST_CASE("AS.2: Recover (through save-changes) restores an untitled autosave and its takes into a new folder") {
  Stale f;
  // The crashed run's untitled document used beat.wav (absolute, inside its session); vox.wav came after the autosave.
  doc::DocumentStore crashed;
  const fs::path beat = f.session / "1" / "beat.wav";
  const doc::SampleRef ref = sampleFor(crashed, beat, doc::storedSamplePath(u8s(beat), {}));
  REQUIRE(crashed.dispatch(doc::AddSample{ref}).ok());
  f.writeAutosave(*crashed.current(), "", "Untitled");
  views::test::ScriptedDialogs dlg;
  f.ctx.setFileDialogs(&dlg);
  REQUIRE(f.ctx.store().dispatch(doc::AddSample{sampleFor(f.ctx.store(), beat, "C:/elsewhere/x.wav")}).ok());
  REQUIRE(f.ctx.dirty());
  const doc::Snapshot before = f.ctx.doc();

  RecoverRecordingsPopover* p = offerRecordingRecovery(*f.shell);
  REQUIRE(p != nullptr);
  f.d.step();
  CHECK(p->current().autosave.has_value());
  CHECK_FALSE(f.d.find("overlay/recover-recordings/buttons/open")->visible());
  CHECK(f.d.find("overlay/recover-recordings/buttons/recover")->visible());
  // "Save changes?" -> Cancel: nothing happens, still offered.
  dlg.choice = SaveChoice::Cancel;
  f.click("recover");
  CHECK(dlg.asks == 1);
  CHECK(f.popover() == p);
  CHECK(f.ctx.doc() == before);
  // -> Don't save: the autosaved document, dirty, its takes moved into the new untitled document's folder.
  dlg.choice = SaveChoice::Discard;
  f.click("recover");
  CHECK(dlg.asks == 2);
  CHECK(f.popover() == nullptr);
  CHECK(f.ctx.projectDir().empty());
  CHECK(f.ctx.dirty());
  const fs::path into(std::u8string(f.ctx.unsavedRecordingDir().begin(), f.ctx.unsavedRecordingDir().end()));
  CHECK(into.parent_path().filename() == "222-2000");  // this run's session
  CHECK(fs::exists(into / "beat.wav"));
  CHECK(fs::exists(into / "vox.wav"));
  const doc::SampleRef* got = doc::findSample(*f.ctx.doc(), ref.id);
  REQUIRE(got != nullptr);
  CHECK(got->path == doc::storedSamplePath(u8s(into / "beat.wav"), {}));
  CHECK(doc::samplesOf(*f.ctx.doc()).size() == 2);  // beat + the take placed below (not the replaced document's x)
  const doc::Track* rec = trackNamed(*f.ctx.doc(), "Recovered");  // vox: not in the autosave, placed as in L13
  REQUIRE(rec != nullptr);
  CHECK(rec->clips.size() == 1);
  CHECK(f.ctx.store().history().cursor() == 1);  // only that placement (the old history is not kept)
  CHECK(f.ctx.missingSamples().empty());
  // AU.app H1: the emptied session keeps its autosave (the only copy on disk of the recovered document) until this
  // session holds the document; it is not offered again meanwhile.
  CHECK(fs::exists(f.session / "autosave" / "project.pqd"));
  CHECK(f.ctx.heldRecovery() != 0);
  CHECK(offerRecordingRecovery(*f.shell) == nullptr);
  f.ctx.releaseRecovery(f.ctx.heldRecovery());  // (the Autosaver's first write after it, or a save)
  CHECK_FALSE(fs::exists(f.session));  // the emptied session (and its autosave) went
  CHECK(offerRecordingRecovery(*f.shell) == nullptr);
  f.ctx.setFileDialogs(nullptr);
}

TEST_CASE("AS.2: Recover opens a saved project's autosave titled with its folder; the session's takes stay offered") {
  Stale f;
  const fs::path song = f.root.parent_path() / "Song";
  fs::create_directories(song / "samples");
  fs::copy_file(f.session / "1" / "beat.wav", song / "samples" / "x.wav");
  doc::DocumentStore crashed;
  REQUIRE(doc::saveProject(u8s(song), *crashed.current()).ok());  // what was saved before the crash
  const doc::SampleRef ref = sampleFor(crashed, song / "samples" / "x.wav", "samples/x.wav");
  REQUIRE(crashed.dispatch(doc::AddSample{ref}).ok());
  f.writeAutosave(*crashed.current(), u8s(song), "Song");

  REQUIRE(offerRecordingRecovery(*f.shell) != nullptr);
  f.d.step();
  f.click("recover");
  CHECK(f.popover() == nullptr);
  CHECK(f.ctx.projectDir() == u8s(song));
  CHECK(f.ctx.projectName() == "Song");
  CHECK(f.ctx.dirty());
  CHECK(f.ctx.store().history().cursor() == 0);
  const doc::SampleRef* got = doc::findSample(*f.ctx.doc(), ref.id);
  REQUIRE(got != nullptr);
  CHECK(got->path == "samples/x.wav");  // relative: resolves in the project's folder
  REQUIRE(f.ctx.sampleResolutions().size() == 1);
  const std::string& played = f.ctx.sampleResolutions()[0].path;
  REQUIRE_FALSE(played.empty());
  CHECK(fs::equivalent(fs::path(std::u8string(played.begin(), played.end())), song / "samples" / "x.wav"));
  // AU.app H1: the autosave stays until this session holds the document (a save here); the session's takes are not
  // this project's: offered again as recordings.
  CHECK(fs::exists(f.session / "autosave" / "project.pqd"));
  REQUIRE(f.ctx.saveProject().ok());
  CHECK_FALSE(fs::exists(f.session / "autosave"));
  CHECK(fs::exists(f.session / "1" / "beat.wav"));
  RecoverRecordingsPopover* p = offerRecordingRecovery(*f.shell);
  REQUIRE(p != nullptr);
  f.d.step();
  CHECK_FALSE(p->current().autosave.has_value());
  CHECK(f.d.find("overlay/recover-recordings/buttons/open")->visible());
  f.click("later");
}

TEST_CASE("AS.2: Later keeps an autosave (offered again); Discard deletes it with the session") {
  Stale f;
  doc::DocumentStore crashed;
  f.writeAutosave(*crashed.current(), "", "Untitled");
  const doc::Snapshot before = f.ctx.doc();
  REQUIRE(offerRecordingRecovery(*f.shell) != nullptr);
  f.d.step();
  f.click("later");
  CHECK(f.popover() == nullptr);
  CHECK(fs::exists(f.session / "autosave" / "project.pqd"));
  RecoverRecordingsPopover* p = offerRecordingRecovery(*f.shell);
  REQUIRE(p != nullptr);
  f.d.step();
  CHECK(p->current().autosave.has_value());
  f.click("discard");
  CHECK(f.popover() == nullptr);
  CHECK(f.discards == 1);
  CHECK_FALSE(fs::exists(f.session));
  CHECK(f.ctx.doc() == before);
  CHECK_FALSE(f.ctx.dirty());
}

// AU.app H1 (docs/reviews/RB-DY.md): crash twice. A Recover, then a crash before this session wrote its own autosave:
// the next run finds the first session's autosave again, with its edits. M5: a cache folder left in it does not keep
// the session once released.
TEST_CASE("AU.app H1: after a Recover the dead session's autosave survives until released; a second crash recovers "
          "the same edits") {
  Stale f;
  doc::DocumentStore crashed;
  const fs::path beat = f.session / "1" / "beat.wav";
  const doc::SampleRef ref = sampleFor(crashed, beat, doc::storedSamplePath(u8s(beat), {}));
  REQUIRE(crashed.dispatch(doc::AddSample{ref}).ok());
  f.writeAutosave(*crashed.current(), "", "Untitled");
  fs::create_directories(f.session / "cache" / "freeze");
  std::ofstream(f.session / "cache" / "freeze" / "3-48000-00.wav") << "render";
  REQUIRE(offerRecordingRecovery(*f.shell) != nullptr);
  f.d.step();
  f.click("recover");
  REQUIRE(f.popover() == nullptr);
  REQUIRE(doc::findSample(*f.ctx.doc(), ref.id) != nullptr);
  // "Crash" now: what the next run finds in the Unsaved folder still holds the first crash's edits.
  REQUIRE(fs::exists(f.session / "autosave" / "project.pqd"));
  {
    auto again = doc::loadProject(u8s(f.session / "autosave"));
    REQUIRE(again.ok());
    CHECK(doc::findSample(*again->project, ref.id) != nullptr);
  }
  CHECK(f.ctx.staleRecordings().empty());  // this run does not offer the held session again
  // Released: the session goes (its cache does not keep it: M5).
  f.ctx.releaseRecovery(f.ctx.heldRecovery() + 1);  // not the held one: nothing happens
  CHECK(fs::exists(f.session));
  f.ctx.releaseRecovery(f.ctx.heldRecovery());
  CHECK(f.ctx.heldRecovery() == 0);
  CHECK_FALSE(fs::exists(f.session));
}
