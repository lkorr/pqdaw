// AS.1: the autosave (pq/app/autosave.h): the policy on a test clock (10 s after the last change, at the latest 60 s
// after the first unsaved one, never mid-gesture), the files (project.pqd round-trips through loadProject, autosave.json),
// the delete after Save As / New / an undo to the saved state / the preference off, close() at a clean quit, an
// untitled document's recordings by absolute path, and the worker thread.

#include <doctest/doctest.h>

#include <filesystem>
#include <random>
#include <string>

#include "pq/app/autosave.h"
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
  CHECK(f.saver.tick(f.ctx, 18.0) < 0);
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
  CHECK(f.saver.tick(f.ctx, 60.0) < 0);
  REQUIRE(f.saver.pump());
  CHECK(f.saver.writes() == 1);

  REQUIRE(f.ctx.store().beginGesture("drag").ok());
  f.addSample("C:/samples/gesture.wav");
  CHECK(f.saver.tick(f.ctx, 61.0) < 0);
  CHECK(f.saver.tick(f.ctx, 200.0) < 0);  // overdue, but the gesture is open
  CHECK_FALSE(f.saver.pump());
  REQUIRE(f.ctx.store().commitGesture().ok());
  CHECK(f.saver.tick(f.ctx, 200.0) < 0);
  REQUIRE(f.saver.pump());
  CHECK(f.saver.writes() == 2);
}

TEST_CASE("AS.1: Save As, New, an undo back to the saved state and the preference off delete the autosave") {
  Fixture f;
  auto autosaveAt = [&](double t) {
    CHECK(f.saver.tick(f.ctx, t) > 0);
    CHECK(f.saver.tick(f.ctx, t + 10.0) < 0);
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
  CHECK(f.saver.tick(f.ctx, 201.0) < 0);  // on again: the change is older than the quiet period, written at once
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
  CHECK(f.saver.tick(f.ctx, 10.0) < 0);
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
