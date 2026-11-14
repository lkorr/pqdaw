// AS.1: the autosave (pq/app/autosave.h): the policy on a test clock (10 s after the last change, at the latest 60 s
// after the first unsaved one, never mid-gesture), the files (project.pqd round-trips through loadProject, autosave.json),
// the delete after Save As / New / an undo to the saved state / the preference off, close() at a clean quit, an
// untitled document's recordings by absolute path, and the worker thread. AS.2: findStaleUnsaved's autosave sessions.

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "pq/app/autosave.h"
#include "pq/app/recording.h"
#include "pq/app/sim_transport.h"
#include "pq/document/commands.h"
#include "pq/document/samples.h"
#include "pq/document/serialize.h"
#include "pq/views/view_context.h"

using namespace pq;
using namespace pq::app;
namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
  const std::u8string u = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

fs::path freshDir(const char* tag) {
  std::random_device rd;
  fs::path d = fs::temp_directory_path() / (std::string(tag) + std::to_string(rd()));
  fs::create_directories(d);
  return d;
}

struct Fixture {
  fs::path session = freshDir("pq_as1_session_");  // stands for <Unsaved>/<pid>-<time>
  fs::path projects = freshDir("pq_as1_projects_");
  SimulatedTransport transport{[] { return 0.0; }};
  views::ViewContext ctx{transport};
  Autosaver saver{session / "autosave", session, false};

  ~Fixture() {
    std::error_code ec;
    fs::remove_all(session, ec);
    fs::remove_all(projects, ec);
  }
  doc::SampleId addSample(const std::string& path) {
    doc::SampleRef s;
    s.id = ctx.store().ids().sample();
    s.path = path;
    s.size = 1000;
    s.frames = 100;
    s.channels = 1;
    s.displayName = "take";
    REQUIRE(ctx.store().dispatch(doc::AddSample{s}).ok());
    return s.id;
  }
  fs::path dir() const { return session / "autosave"; }
  bool written() const { return fs::exists(dir() / "project.pqd") && fs::exists(dir() / "autosave.json"); }
};

}  // namespace

TEST_CASE("AS.1: an edit is autosaved 10 s after the last change; the files round-trip through loadProject") {
  Fixture f;
  CHECK(f.saver.tick(f.ctx, 0.0) < 0);  // clean: nothing to do, no timer
  CHECK_FALSE(f.saver.pump());
  const doc::SampleId a = f.addSample("C:/samples/a.wav");
  CHECK(f.saver.tick(f.ctx, 0.0) == doctest::Approx(10.0));  // the shell arms its wake timer for this
  CHECK(f.saver.tick(f.ctx, 5.0) == doctest::Approx(5.0));
  CHECK_FALSE(f.saver.pump());
  const doc::SampleId b = f.addSample("C:/samples/b.wav");
  CHECK(f.saver.tick(f.ctx, 8.0) == doctest::Approx(10.0));  // the quiet period restarts at the change
  CHECK(f.saver.tick(f.ctx, 17.9) > 0);
  CHECK_FALSE(f.saver.pump());
  CHECK(f.saver.tick(f.ctx, 18.0) == doctest::Approx(Autosaver::kOutcomePollSeconds));  // submitted: back for its outcome (L1)
  REQUIRE(f.saver.pump());
  CHECK(f.saver.writes() == 1);
  CHECK(f.saver.lastError().empty());
  REQUIRE(f.written());

  auto loaded = doc::loadProject(utf8(f.dir()));
  REQUIRE(loaded.ok());
  const doc::SampleRef* ra = doc::findSample(*loaded->project, a);
  const doc::SampleRef* rb = doc::findSample(*loaded->project, b);
  REQUIRE(ra != nullptr);
  REQUIRE(rb != nullptr);
  CHECK(ra->path == "C:/samples/a.wav");
  CHECK(rb->path == "C:/samples/b.wav");
  CHECK(*doc::findSample(*f.ctx.doc(), b) == *rb);

  auto meta = readAutosaveMeta(f.dir());
  REQUIRE(meta.ok());
  CHECK(meta->originDir.empty());
  CHECK(meta->name == "Untitled");
  CHECK(meta->savedAt > 1700000000);
  CHECK(meta->originMtime == 0);

  CHECK(f.saver.tick(f.ctx, 30.0) < 0);  // written: nothing more until the next change
  CHECK_FALSE(f.saver.pump());
  CHECK(f.ctx.dirty());  // the autosave never makes the document clean
}

TEST_CASE("AS.1: a long editing run is autosaved 60 s after its first change; never inside a gesture") {
  Fixture f;
  for (int t = 0; t <= 55; t += 5) {
    f.addSample("C:/samples/" + std::to_string(t) + ".wav");
    CHECK(f.saver.tick(f.ctx, t) > 0);
    CHECK_FALSE(f.saver.pump());
  }
  CHECK(f.saver.tick(f.ctx, 59.0) == doctest::Approx(1.0));
  CHECK(f.saver.tick(f.ctx, 60.0) == doctest::Approx(Autosaver::kOutcomePollSeconds));
  REQUIRE(f.saver.pump());
  CHECK(f.saver.writes() == 1);

  REQUIRE(f.ctx.store().beginGesture("drag").ok());
  f.addSample("C:/samples/gesture.wav");
  CHECK(f.saver.tick(f.ctx, 61.0) < 0);
  CHECK(f.saver.tick(f.ctx, 200.0) < 0);  // overdue, but the gesture is open
  CHECK_FALSE(f.saver.pump());
  REQUIRE(f.ctx.store().commitGesture().ok());
  CHECK(f.saver.tick(f.ctx, 200.0) == doctest::Approx(Autosaver::kOutcomePollSeconds));
  REQUIRE(f.saver.pump());
  CHECK(f.saver.writes() == 2);
}

TEST_CASE("AS.1: Save As, New, an undo back to the saved state and the preference off delete the autosave") {
  Fixture f;
  auto autosaveAt = [&](double t) {
    CHECK(f.saver.tick(f.ctx, t) > 0);
    CHECK(f.saver.tick(f.ctx, t + 10.0) == doctest::Approx(Autosaver::kOutcomePollSeconds));
    REQUIRE(f.saver.pump());
    REQUIRE(f.written());
  };
  f.addSample("C:/samples/a.wav");
  autosaveAt(0.0);
  const std::string song = utf8(f.projects / "Song");
  REQUIRE(f.ctx.saveProjectAs(song).ok());
  CHECK(f.saver.tick(f.ctx, 11.0) < 0);
  REQUIRE(f.saver.pump());  // the discard
  CHECK_FALSE(fs::exists(f.dir()));
  CHECK(fs::exists(f.session));  // only close() removes the session folder
  CHECK(fs::exists(f.projects / "Song" / "project.pqd"));

  // A titled project's autosave names its origin (AS.2 offers it only when newer than the origin's file).
  f.addSample("C:/samples/b.wav");
  autosaveAt(20.0);
  auto meta = readAutosaveMeta(f.dir());
  REQUIRE(meta.ok());
  CHECK(meta->originDir == song);
  CHECK(meta->name == "Song");
  CHECK(meta->recordingsDir.empty());
  CHECK(meta->originMtime == fileUnixTime(f.projects / "Song" / "project.pqd"));
  CHECK(meta->originMtime > 0);
  CHECK(fs::exists(f.projects / "Song" / "project.pqd"));
  CHECK_FALSE(fs::exists(f.projects / "Song" / "autosave"));  // D6: never inside the user's project folder

  REQUIRE(f.ctx.undo());  // back to the saved snapshot: the autosave is stale
  CHECK(f.saver.tick(f.ctx, 40.0) < 0);
  REQUIRE(f.saver.pump());
  CHECK_FALSE(fs::exists(f.dir()));

  f.addSample("C:/samples/c.wav");
  autosaveAt(50.0);
  f.ctx.newProject();
  CHECK(f.saver.tick(f.ctx, 61.0) < 0);
  REQUIRE(f.saver.pump());
  CHECK_FALSE(fs::exists(f.dir()));

  f.addSample("C:/samples/d.wav");
  autosaveAt(70.0);
  views::ViewPrefs p = f.ctx.viewPrefs();
  p.autosave = false;
  f.ctx.setViewPrefs(p);
  CHECK(f.saver.tick(f.ctx, 81.0) < 0);
  REQUIRE(f.saver.pump());
  CHECK_FALSE(fs::exists(f.dir()));
  f.addSample("C:/samples/e.wav");
  CHECK(f.saver.tick(f.ctx, 82.0) < 0);
  CHECK(f.saver.tick(f.ctx, 200.0) < 0);  // off: nothing is written
  CHECK_FALSE(f.saver.pump());
  p.autosave = true;
  f.ctx.setViewPrefs(p);
  CHECK(f.saver.tick(f.ctx, 201.0) == doctest::Approx(Autosaver::kOutcomePollSeconds));  // on again: the change is older than the quiet period, written at once
  REQUIRE(f.saver.pump());
  CHECK(f.written());

  f.saver.close();  // a clean quit: the autosave and the (now empty) session folder go
  CHECK_FALSE(fs::exists(f.session));
}

TEST_CASE("AS.1: an untitled document's autosave references its recordings by absolute path") {
  Fixture f;
  const fs::path takes = f.session / "1";  // the untitled document's own recordings folder
  fs::create_directories(takes);
  views::ViewContext::UnsavedRecordings u;
  u.root = utf8(f.session.parent_path());
  u.current = utf8(takes);
  u.fresh = [&] { return utf8(f.session / "2"); };
  u.discard = [](const std::string&) {};
  f.ctx.setUnsavedRecordings(u);
  REQUIRE(f.ctx.unsavedRecordingDir() == utf8(takes));
  const std::string take = utf8(takes / "Audio 1 take 1.wav");
  const doc::SampleId id = f.addSample(take);
  CHECK(f.saver.tick(f.ctx, 0.0) > 0);
  CHECK(f.saver.tick(f.ctx, 10.0) == doctest::Approx(Autosaver::kOutcomePollSeconds));
  REQUIRE(f.saver.pump());
  auto loaded = doc::loadProject(utf8(f.dir()));
  REQUIRE(loaded.ok());
  const doc::SampleRef* r = doc::findSample(*loaded->project, id);
  REQUIRE(r != nullptr);
  CHECK(doc::isAbsoluteSamplePath(r->path));
  CHECK(r->path == take);
  auto meta = readAutosaveMeta(f.dir());
  REQUIRE(meta.ok());
  CHECK(meta->originDir.empty());
  CHECK(meta->recordingsDir == utf8(takes));
}

TEST_CASE("AS.1: the worker thread writes off the caller's thread; latest wins; discard and close") {
  const fs::path session = freshDir("pq_as1_thread_");
  {
    SimulatedTransport transport{[] { return 0.0; }};
    views::ViewContext ctx{transport};
    Autosaver saver(session / "autosave", session);
    for (int i = 0; i < 20; ++i) {  // back-to-back submits: at most one pending, the last one wins
      doc::SampleRef s;
      s.id = ctx.store().ids().sample();
      s.path = "C:/samples/" + std::to_string(i) + ".wav";
      REQUIRE(ctx.store().dispatch(doc::AddSample{s}).ok());
      saver.submit(ctx.doc(), ctx.store().view(), AutosaveMeta{"", "", "Untitled", 0, 0});
    }
    saver.flush();
    CHECK(saver.writes() >= 1);
    CHECK(saver.writes() <= 20);
    auto loaded = doc::loadProject(utf8(session / "autosave"));
    REQUIRE(loaded.ok());
    CHECK(loaded->project->samples->size() == 20);  // the last submit's document
    saver.discard();
    saver.flush();
    CHECK_FALSE(fs::exists(session / "autosave"));
    saver.submit(ctx.doc(), ctx.store().view(), AutosaveMeta{"", "", "Untitled", 0, 0});
  }  // the destructor runs the pending write and keeps it (an exit that is not a clean quit)
  CHECK(fs::exists(session / "autosave" / "project.pqd"));
  std::error_code ec;
  fs::remove_all(session, ec);
}

TEST_CASE("AS.2: findStaleUnsaved returns a dead session's autosave with its meta, unless its origin was saved after it") {
  const fs::path base = freshDir("pq_as2_stale_");
  const fs::path root = base / "Unsaved", song = base / "Song";
  doc::DocumentStore store;
  REQUIRE(doc::saveProject(utf8(song), *store.current()).ok());
  // An autosave as the Autosaver writes it (savedAt / originMtime filled in by the job).
  auto autosave = [&](const char* session, std::string origin) {
    const fs::path s = root / session;
    Autosaver a{s / "autosave", s, false};
    AutosaveMeta m;
    m.originDir = std::move(origin);
    m.name = m.originDir.empty() ? "Untitled" : "Song";
    if (m.originDir.empty()) m.recordingsDir = utf8(s / "1");
    a.submit(store.current(), {}, m);
    REQUIRE(a.pump());
    REQUIRE(a.lastError().empty());
    return readAutosaveMeta(s / "autosave").value();
  };
  auto setMtime = [](const fs::path& f, int64_t unix) {
    fs::last_write_time(f, std::chrono::clock_cast<std::chrono::file_clock>(
                               std::chrono::sys_seconds{std::chrono::seconds{unix}}));
  };
  const AutosaveMeta untitled = autosave("111-1000", "");
  const AutosaveMeta titled = autosave("222-1100", utf8(song));
  setMtime(song / "project.pqd", titled.originMtime - 60);  // as it was when 222 and 333 were written (below)
  (void)autosave("333-1200", utf8(song));               // ... but this one is older than the file (below)
  fs::create_directories(root / "444-1300" / "autosave");  // a project.pqd without its autosave.json: not offered
  std::ofstream(root / "444-1300" / "autosave" / "project.pqd") << "{}";
  const auto alive = [](uint32_t pid, int64_t) { return pid == 999; };

  // The stale check reads the origin's time now and compares it with the time it had when the autosave was written
  // (AU.app M10: the origin's own clock, never this machine's savedAt).
  {
    fs::path meta333 = root / "333-1200" / "autosave";
    const AutosaveMeta m = readAutosaveMeta(meta333).value();
    setMtime(song / "project.pqd", m.originMtime);
    CHECK(autosaveSupersedesOrigin(m));  // not written since
    setMtime(song / "project.pqd", m.originMtime + 30);  // the user saved after that autosave
    CHECK_FALSE(autosaveSupersedesOrigin(m));
    // A NAS whose clock runs an hour ahead: the origin's mtime is "in the future" for this machine, the autosave is
    // still the newer state (the old rule, origin mtime <= savedAt, hid it).
    AutosaveMeta skew = m;
    skew.originMtime = m.savedAt + 3600;
    setMtime(song / "project.pqd", skew.originMtime);
    CHECK(autosaveSupersedesOrigin(skew));
    setMtime(song / "project.pqd", titled.originMtime);
  }
  // Make 333's autosave older than the origin file: it was written while the origin had an earlier mtime.
  {
    const fs::path f = root / "333-1200" / "autosave" / std::string(kAutosaveMetaFileName);
    AutosaveMeta m = readAutosaveMeta(f.parent_path()).value();
    std::ofstream(f, std::ios::trunc) << "{\"originDir\":\"" << m.originDir << "\",\"recordingsDir\":\"\",\"name\":\"Song\","
                                      << "\"savedAt\":" << (titled.savedAt - 120) << ",\"originMtime\":"
                                      << (titled.originMtime - 120) << "}";
    REQUIRE(readAutosaveMeta(f.parent_path()).value().savedAt == titled.savedAt - 120);
  }
  const std::vector<StaleUnsavedSession> stale = findStaleUnsaved(root, "999-5000", alive);
  REQUIRE(stale.size() == 2);
  CHECK(stale[0].dir.filename() == "111-1000");
  CHECK(stale[0].wavs.empty());
  REQUIRE(stale[0].autosave.has_value());
  CHECK(*stale[0].autosave == untitled);
  CHECK(stale[0].autosave->recordingsDir == utf8(root / "111-1000" / "1"));
  CHECK(stale[1].dir.filename() == "222-1100");
  REQUIRE(stale[1].autosave.has_value());
  CHECK(stale[1].autosave->originDir == utf8(song));
  CHECK(stale[1].autosave->originMtime > 0);
  // An outdated autosave next to takes: the session is offered for its takes only.
  fs::create_directories(root / "333-1200" / "1");
  std::ofstream(root / "333-1200" / "1" / "take.wav", std::ios::binary) << "RIFF";
  const std::vector<StaleUnsavedSession> again = findStaleUnsaved(root, "999-5000", alive);
  REQUIRE(again.size() == 3);
  CHECK(again[2].dir.filename() == "333-1200");
  CHECK_FALSE(again[2].autosave.has_value());
  CHECK(again[2].wavs.size() == 1);
  std::error_code ec;
  fs::remove_all(base, ec);
}

// ---- AU.app (docs/reviews/RB-DY.md H1, L1, M5) ---------------------------------------------------------------------

TEST_CASE("AU.app L1: a failed autosave write is retried quietSeconds later without another edit") {
  Fixture f;
  std::ofstream(f.dir()) << "a file where the autosave folder goes";  // the write cannot create its folder
  f.addSample("C:/samples/a.wav");
  CHECK(f.saver.tick(f.ctx, 0.0) == doctest::Approx(10.0));
  CHECK(f.saver.tick(f.ctx, 10.0) == doctest::Approx(Autosaver::kOutcomePollSeconds));
  REQUIRE(f.saver.pump());
  CHECK_FALSE(f.saver.lastError().empty());
  CHECK(f.saver.writes() == 0);
  CHECK(f.saver.tick(f.ctx, 10.25) == doctest::Approx(10.0));  // the outcome: failed; retried, no edit needed
  CHECK_FALSE(f.saver.pump());
  fs::remove(f.dir());  // whatever blocked it went away
  CHECK(f.saver.tick(f.ctx, 20.25) == doctest::Approx(Autosaver::kOutcomePollSeconds));
  REQUIRE(f.saver.pump());
  CHECK(f.saver.writes() == 1);
  CHECK(f.written());
  CHECK(f.saver.tick(f.ctx, 20.5) < 0);  // written: idle again
}

TEST_CASE("AU.app H1: a recovered session's autosave stays until this session's own autosave write succeeds") {
  Fixture f;
  const fs::path root = f.session.parent_path() / ("pq_au_h1_" + std::to_string(std::random_device{}()));
  const fs::path dead = root / "111-1000";
  // The crashed run's autosave of an untitled document with one sample.
  doc::DocumentStore crashed;
  doc::SampleRef ref;
  ref.id = crashed.ids().sample();
  ref.path = "C:/samples/crashed.wav";
  REQUIRE(crashed.dispatch(doc::AddSample{ref}).ok());
  {
    Autosaver a{dead / "autosave", dead, false};
    a.submit(crashed.current(), {}, AutosaveMeta{"", "", "Untitled", 0, 0});
    REQUIRE(a.pump());
  }
  views::ViewContext::UnsavedRecordings u;
  u.root = utf8(root);
  u.current = utf8(f.session / "1");
  u.fresh = [&] { return utf8(f.session / "2"); };
  u.discard = [](const std::string&) {};
  u.discardStale = [](const std::string& dir) {
    std::error_code ec;
    return fs::remove_all(fs::path(std::u8string(dir.begin(), dir.end())), ec) > 0;
  };
  f.ctx.setUnsavedRecordings(u);
  views::ViewContext::StaleRecordings s;
  s.dir = utf8(dead);
  views::ViewContext::StaleAutosave sa;
  sa.dir = utf8(dead / "autosave");
  sa.name = "Untitled";
  s.autosave = sa;
  REQUIRE(f.ctx.recoverAutosave(s));
  REQUIRE(doc::findSample(*f.ctx.doc(), ref.id) != nullptr);
  // Crash now (before this session wrote anything): the recovered edits are still on disk, offered again.
  CHECK(f.ctx.heldRecovery() != 0);
  CHECK(fs::exists(dead / "autosave" / "project.pqd"));
  {
    auto again = doc::loadProject(utf8(dead / "autosave"));
    REQUIRE(again.ok());
    CHECK(doc::findSample(*again->project, ref.id) != nullptr);
  }
  const std::vector<StaleUnsavedSession> offered = findStaleUnsaved(root, "999-5000", [](uint32_t, int64_t) { return false; });
  REQUIRE(offered.size() == 1);
  CHECK(offered[0].autosave.has_value());
  // This session's autosave: once its write succeeded, the dead session goes.
  CHECK(f.saver.tick(f.ctx, 0.0) > 0);
  CHECK(f.saver.tick(f.ctx, 10.0) == doctest::Approx(Autosaver::kOutcomePollSeconds));
  CHECK(fs::exists(dead));  // written only when the job ran
  REQUIRE(f.saver.pump());
  REQUIRE(f.written());
  (void)f.saver.tick(f.ctx, 10.25);
  CHECK(f.ctx.heldRecovery() == 0);
  CHECK_FALSE(fs::exists(dead));
  auto ours = doc::loadProject(utf8(f.dir()));
  REQUIRE(ours.ok());
  CHECK(doc::findSample(*ours->project, ref.id) != nullptr);  // the first crash's edits, now in this session's autosave
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("AU.app M5: sweepStaleUnsaved deletes dead sessions that hold only caches; takes, autosaves and live runs stay") {
  const fs::path base = freshDir("pq_au_m5_");
  const fs::path root = base / "Unsaved";
  auto file = [](const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "RIFF";
  };
  file(root / "111-1000" / "1" / "cache" / "freeze" / "3-48000-00.wav");  // only a cache: goes
  fs::create_directories(root / "112-1000" / "1");                       // empty: goes
  file(root / "222-1000" / "1" / "take.wav");                             // a take: stays
  file(root / "222-1000" / "1" / "cache" / "freeze" / "3-48000-00.wav");
  file(root / "333-1000" / "autosave" / "project.pqd");                   // an autosave: stays
  file(root / "999-1000" / "1" / "cache" / "freeze" / "3-48000-00.wav");  // its process runs: stays
  file(root / "notes" / "x.txt");                                         // not a session folder: stays
  const auto alive = [](uint32_t pid, int64_t) { return pid == 999; };
  CHECK(sweepStaleUnsaved(root, "555-1000", alive) == 2);
  CHECK_FALSE(fs::exists(root / "111-1000"));
  CHECK_FALSE(fs::exists(root / "112-1000"));
  CHECK(fs::exists(root / "222-1000" / "1" / "take.wav"));
  CHECK(fs::exists(root / "333-1000" / "autosave" / "project.pqd"));
  CHECK(fs::exists(root / "999-1000"));
  CHECK(fs::exists(root / "notes" / "x.txt"));
  std::error_code ec;
  fs::remove_all(base, ec);
}
