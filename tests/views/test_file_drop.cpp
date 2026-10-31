// T5.7: dropping files into the arrangement — every rule of docs/plans/M5.md "Decisions" through simulated drag
// sessions (UiTree::dragEnter / dragMove / dragDrop, the path Explorer drops and browser drags share): onto an audio
// track (overwrite), several files (one new track each), Ctrl (one track, one after another), onto MIDI and group tracks
// (new tracks after them), below the tracks, refused rows, MIDI / preset files (status), folders (browser places),
// browser items; each drop one undo step; ghosts at probed lengths while hovering; the preloader; 100 files ≤ 300 ms.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pq/document/audio_clip.h"
#include "pq/document/edit_ops.h"
#include "pq/media/wav_file.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/browser/library.h"
#include "pq/views/fake_transport.h"
#include "pq/views/file_probes.h"
#include "pq/views/sample_drop.h"
#include "pq/views/shell/shell.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
namespace fs = std::filesystem;

namespace {

constexpr Ticks kBar = kTicksPerWhole;
constexpr const char* kArr = "main/body/top/track-area/arrangement";

std::string u8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// A stereo float WAV of `seconds` at `rate`.
std::string makeWav(const fs::path& dir, const std::string& name, double seconds, uint32_t rate = 48000) {
  const auto n = static_cast<size_t>(std::llround(seconds * rate));
  std::vector<std::vector<float>> ch(2, std::vector<float>(n, 0.0f));
  for (size_t i = 0; i < n; ++i) ch[0][i] = ch[1][i] = 0.25f * static_cast<float>(std::sin(0.01 * static_cast<double>(i)));
  const fs::path p = dir / name;
  REQUIRE(media::writeWav(p, ch, rate).ok());
  return u8(p);
}

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  fs::path dir;
  doc::TrackId audio, midi, group, member;
  std::vector<std::string> preloaded;

  explicit Fixture(std::string_view name) {
    dir = fs::path(views::test::tempDir(std::string("drop_") + std::string(name)));
    ctx.setSamplePreloader([this](const std::string& abs) { preloaded.push_back(abs); });
    doc::DocumentStore& st = ctx.store();
    auto run = [&](const doc::ops::EditResult& e) {
      REQUIRE(e.ok());
      REQUIRE(st.dispatch(e->command).ok());
      return *e;
    };
    audio = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "Drums")).tracks[0];
    midi = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, audio, "Keys")).tracks[0];
    member = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, midi, "Pad")).tracks[0];
    group = run(doc::ops::groupTracks(*st.current(), st.ids(), std::vector<doc::TrackId>{member}, "Group")).tracks[0];
    run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Return, {}, "A Return"));
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    arr->setTimeView(0.0, 200.0 / static_cast<double>(kBar));
    d.tree().layoutNow();
    d.step();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }

  size_t history() const { return ctx.store().history().cursor(); }
  int32_t rowOf(doc::TrackId id) const {
    const auto rows = arr->rows();
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].id == id) return static_cast<int32_t>(i);
    return -1;
  }
  // A point in the lane of `id` at tick t.
  Vec2 at(doc::TrackId id, Ticks t) {
    d.tree().layoutNow();  // a drop that selected its clip may have opened the detail view
    const IRect r = arr->rowLaneRect(rowOf(id));
    return {arr->xOfTick(static_cast<double>(t)) + 1.0f, static_cast<float>(r.y + r.h / 2)};
  }
  Vec2 belowTracks(Ticks t) {
    d.tree().layoutNow();
    arr->scrollToY(1 << 20);  // the drop area under the last track
    d.tree().layoutNow();
    const auto rows = arr->rows();
    const IRect r = arr->rowLaneRect(static_cast<int32_t>(rows.size()) - 1);
    return {arr->xOfTick(static_cast<double>(t)) + 1.0f, static_cast<float>(r.bottom() + 30)};
  }
  DropEffect dropFiles(std::vector<std::string> paths, Vec2 pos, Mods mods = 0,
                       DragKind kind = DragKind::Files) {
    DragPayload p;
    p.kind = kind;
    p.paths = std::move(paths);
    p.label = "files";
    d.tree().dragEnter(p, {pos.x, pos.y - 3.0f}, mods);
    (void)d.tree().dragMove(pos, mods);
    return d.tree().dragDrop(pos, mods);
  }
  const doc::Track& track(doc::TrackId id) const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), id);
    REQUIRE(t != nullptr);
    return *t;
  }
  // Top-level and nested tracks in tree order (name, kind).
  std::vector<std::string> names() const {
    std::vector<std::string> out;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (t.kind != doc::TrackKind::Master && t.kind != doc::TrackKind::Return) out.push_back(t.name);
    });
    return out;
  }
  const doc::AudioClip& clip(const doc::Track& t, size_t i) const {
    REQUIRE(i < t.clips.size());
    REQUIRE(t.clips[i].audio != nullptr);
    return *t.clips[i].audio;
  }
  const doc::Track& byName(std::string_view n) const {
    const doc::Track* f = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!f && t.name == n) f = &t;
    });
    REQUIRE(f != nullptr);
    return *f;
  }
};

}  // namespace

TEST_CASE("file drop: one file onto an audio track lands at the snapped tick and overwrites what lies under it") {
  Fixture f("one");
  const std::string kick = makeWav(f.dir, "Kick.wav", 0.5);
  const std::string loop = makeWav(f.dir, "Loop.wav", 4.0);
  const size_t h0 = f.history();
  CHECK(f.dropFiles({loop}, f.at(f.audio, kBar)) == DropEffect::Copy);
  CHECK(f.history() == h0 + 1);
  const doc::Track& t = f.track(f.audio);
  REQUIRE(t.clips.size() == 1);
  CHECK(f.clip(t, 0).start == kBar);
  CHECK(f.clip(t, 0).name == "Loop");
  CHECK(std::abs(f.clip(t, 0).seconds - 4.0) < 1e-9);
  CHECK(doc::samplesOf(*f.ctx.doc()).size() == 1);
  // The new clip is selected and shown in the clip view.
  CHECK(f.arr->selection().clips.size() == 1);
  // A short file dropped into its middle overwrites that part (Live): the loop is split around it.
  CHECK(f.dropFiles({kick}, f.at(f.audio, 2 * kBar)) == DropEffect::Copy);
  CHECK(f.history() == h0 + 2);
  const doc::Track& t2 = f.track(f.audio);
  REQUIRE(t2.clips.size() == 3);
  CHECK(f.clip(t2, 1).name == "Kick");
  CHECK(f.clip(t2, 1).start == 2 * kBar);
  CHECK(f.clip(t2, 0).start + f.clip(t2, 0).length == 2 * kBar);
  CHECK(f.clip(t2, 2).start == f.clip(t2, 1).start + f.clip(t2, 1).length);
  // The same file again reuses its table entry.
  CHECK(f.dropFiles({kick}, f.at(f.audio, 3 * kBar + kBar / 2)) == DropEffect::Copy);
  CHECK(doc::samplesOf(*f.ctx.doc()).size() == 2);
  // The preloader saw each file once, in the form the document resolves it.
  CHECK(f.preloaded.size() >= 2);
  CHECK(std::find(f.preloaded.begin(), f.preloaded.end(), doc::absoluteSamplePath(kick, {})) != f.preloaded.end());
  // Undo takes each drop back as one step.
  REQUIRE(f.ctx.undo());
  REQUIRE(f.ctx.undo());
  REQUIRE(f.ctx.undo());
  CHECK(f.track(f.audio).clips.empty());
  CHECK(doc::samplesOf(*f.ctx.doc()).empty());
}

TEST_CASE("file drop: several files onto an audio track: the first there, one new audio track each after it") {
  Fixture f("stems");
  const std::vector<std::string> files = {makeWav(f.dir, "Bass.wav", 1.0), makeWav(f.dir, "Gtr.wav", 2.0),
                                          makeWav(f.dir, "Vox.wav", 1.5)};
  const size_t h0 = f.history();
  CHECK(f.dropFiles(files, f.at(f.audio, 0)) == DropEffect::Copy);
  CHECK(f.history() == h0 + 1);
  CHECK(f.names() == std::vector<std::string>{"Drums", "Gtr", "Vox", "Keys", "Group", "Pad"});
  CHECK(f.clip(f.track(f.audio), 0).name == "Bass");
  const doc::Track& gtr = f.byName("Gtr");
  CHECK(gtr.kind == doc::TrackKind::Audio);
  CHECK(f.clip(gtr, 0).start == 0);
  CHECK(f.clip(f.byName("Vox"), 0).name == "Vox");
  REQUIRE(f.ctx.undo());
  CHECK(f.names() == std::vector<std::string>{"Drums", "Keys", "Group", "Pad"});
}

TEST_CASE("file drop: Ctrl puts every file on one track, one after another") {
  Fixture f("ctrl");
  const std::vector<std::string> files = {makeWav(f.dir, "a.wav", 1.0), makeWav(f.dir, "b.wav", 0.75),
                                          makeWav(f.dir, "c.wav", 0.5)};
  const size_t h0 = f.history();
  CHECK(f.dropFiles(files, f.at(f.audio, kBar), mod::kCtrl) == DropEffect::Copy);
  CHECK(f.history() == h0 + 1);
  const doc::Track& t = f.track(f.audio);
  REQUIRE(t.clips.size() == 3);
  CHECK(f.clip(t, 0).start == kBar);
  CHECK(f.clip(t, 1).start == f.clip(t, 0).start + f.clip(t, 0).length);
  CHECK(f.clip(t, 2).start == f.clip(t, 1).start + f.clip(t, 1).length);
  CHECK(f.names().size() == 4);  // no new tracks
  // Ctrl onto a MIDI track: ONE new audio track with all of them.
  CHECK(f.dropFiles(files, f.at(f.midi, 0), mod::kCtrl) == DropEffect::Copy);
  CHECK(f.names() == std::vector<std::string>{"Drums", "Keys", "a", "Group", "Pad"});
  CHECK(f.byName("a").clips.size() == 3);
}

TEST_CASE("file drop: onto a MIDI track or a group: new audio tracks after it; below the tracks: at the end") {
  Fixture f("midi");
  const std::string a = makeWav(f.dir, "One.wav", 1.0), b = makeWav(f.dir, "Two.wav", 1.0);
  CHECK(f.dropFiles({a}, f.at(f.midi, 0)) == DropEffect::Copy);
  CHECK(f.names() == std::vector<std::string>{"Drums", "Keys", "One", "Group", "Pad"});
  CHECK(f.track(f.midi).clips.empty());
  CHECK(f.dropFiles({a, b}, f.at(f.group, kBar)) == DropEffect::Copy);
  CHECK(f.names() == std::vector<std::string>{"Drums", "Keys", "One", "Group", "Pad", "One", "Two"});
  CHECK(f.dropFiles({b}, f.belowTracks(0)) == DropEffect::Copy);
  CHECK(f.names().back() == "Two");
  CHECK(f.names().size() == 8);
}

TEST_CASE("file drop: refused where no clip can go; MIDI and preset files explain themselves") {
  Fixture f("refuse");
  const std::string a = makeWav(f.dir, "One.wav", 1.0);
  const size_t h0 = f.history();
  // The return rows (pinned at the bottom) take no drop.
  const IRect bottom = f.arr->bottomRect();
  REQUIRE_FALSE(bottom.empty());
  const Vec2 ret{f.arr->xOfTick(0) + 10.0f, static_cast<float>(bottom.y + bottom.h / 2)};
  CHECK(f.dropFiles({a}, ret) == DropEffect::None);
  CHECK(f.history() == h0);
  // A MIDI file: refused, the status line says why.
  const fs::path mid = f.dir / "beat.mid";
  std::ofstream(mid, std::ios::binary) << "MThd";
  DragPayload p;
  p.paths = {u8(mid)};
  f.d.tree().dragEnter(p, f.at(f.audio, 0), 0);
  CHECK(f.d.tree().dropFeedback().effect == DropEffect::None);
  CHECK(f.ctx.status().find("MIDI") != std::string::npos);
  CHECK(f.d.tree().dragDrop(f.at(f.audio, 0), 0) == DropEffect::None);
  CHECK(f.history() == h0);
  // Audio + MIDI: the audio lands, the MIDI file is reported as skipped.
  CHECK(f.dropFiles({u8(mid), a}, f.at(f.audio, 0)) == DropEffect::Copy);
  CHECK(f.history() == h0 + 1);
  CHECK(f.ctx.status().find("skipped") != std::string::npos);
  // A file that is not audio although named so: reported, nothing added.
  const fs::path bad = f.dir / "broken.wav";
  std::ofstream(bad, std::ios::binary) << "not a wave file at all";
  f.dropFiles({u8(bad)}, f.at(f.audio, 4 * kBar));
  CHECK(f.history() == h0 + 1);
  CHECK(f.ctx.status().find("Could not read") != std::string::npos);
}

TEST_CASE("file drop: a folder becomes a browser place") {
  Fixture f("folder");
  const fs::path sub = f.dir / "Kits";
  fs::create_directories(sub);
  CHECK(f.dropFiles({u8(sub)}, f.at(f.audio, 0)) == DropEffect::Link);
  bool found = false;
  for (const library::PlaceSpec& p : f.ctx.library().places()) found = found || placeKey(p.root) == placeKey(u8(sub));
  CHECK(found);
  CHECK(f.ctx.browserVisible());
  // Again: already there, nothing added.
  const size_t n = f.ctx.library().places().size();
  f.dropFiles({u8(sub)}, f.at(f.audio, 0));
  CHECK(f.ctx.library().places().size() == n);
}

TEST_CASE("file drop: browser items drop like files") {
  Fixture f("browser");
  const std::string a = makeWav(f.dir, "Snare.wav", 0.5);
  CHECK(f.dropFiles({a}, f.at(f.audio, 0), 0, DragKind::BrowserItems) == DropEffect::Copy);
  CHECK(f.clip(f.track(f.audio), 0).name == "Snare");
}

TEST_CASE("file drop: ghosts show the probed lengths while hovering; leaving removes them without an edit") {
  Fixture f("ghost");
  const std::string a = makeWav(f.dir, "Long.wav", 3.0), b = makeWav(f.dir, "Short.wav", 0.5);
  const size_t h0 = f.history();
  DragPayload p;
  p.paths = {a, b};
  f.d.tree().dragEnter(p, f.at(f.audio, kBar), 0);
  REQUIRE(f.ctx.fileProbes().wait(p.paths, std::chrono::seconds(10)));
  f.d.tree().dragMove(f.at(f.audio, kBar), 0);
  const auto& plan = f.arr->dropPlan();
  REQUIRE(plan.items.size() == 2);
  CHECK_FALSE(plan.items[0].pending);
  const TempoMap& tm = f.ctx.doc()->timeline->tempoMap;
  CHECK(plan.items[0].length == doc::offClipLength(tm, kBar, 3.0));
  CHECK(plan.items[1].length == doc::offClipLength(tm, kBar, 0.5));
  CHECK(plan.newTracks == 1);
  CHECK(f.d.tree().needsPaint());
  f.d.step();
  // Ctrl: one track, the second ghost after the first.
  f.d.tree().dragMove(f.at(f.audio, kBar), mod::kCtrl);
  REQUIRE(f.arr->dropPlan().items.size() == 2);
  CHECK(f.arr->dropPlan().newTracks == 0);
  CHECK(f.arr->dropPlan().items[1].start == kBar + f.arr->dropPlan().items[0].length);
  CHECK(f.d.tree().needsPaint());
  f.d.step();
  f.d.tree().dragExit();
  CHECK_FALSE(f.arr->dropHovering());
  CHECK(f.d.tree().needsPaint());
  CHECK(f.history() == h0);
}

TEST_CASE("file drop: planSampleDrop and sampleDropEdit (pure)") {
  Fixture f("plan");
  ProbedFile a;
  a.state = ProbedFile::State::Ready;
  a.path = a.absPath = "C:/x/a.wav";
  a.frames = 96000;
  a.rate = 48000;
  a.channels = 2;
  a.displayName = "a";
  ProbedFile b = a;
  b.path = b.absPath = "C:/x/b.wav";
  b.displayName = "b";
  b.state = ProbedFile::State::Pending;
  const std::vector<ProbedFile> files{a, b};
  const SampleDropTarget on = sampleDropTargetFor(*f.ctx.doc(), f.audio);
  CHECK(on.kind == SampleDropTarget::Kind::OnTrack);
  CHECK(sampleDropTargetFor(*f.ctx.doc(), f.midi).kind == SampleDropTarget::Kind::AfterTrack);
  CHECK(sampleDropTargetFor(*f.ctx.doc(), f.group).kind == SampleDropTarget::Kind::AfterTrack);
  CHECK(sampleDropTargetFor(*f.ctx.doc(), {}).kind == SampleDropTarget::Kind::AtEnd);
  CHECK(sampleDropTargetFor(*f.ctx.doc(), f.ctx.doc()->master->id).kind == SampleDropTarget::Kind::Refused);
  const SampleDropPlan plan = planSampleDrop(*f.ctx.doc(), on, files, 0, false);
  REQUIRE(plan.items.size() == 2);
  CHECK(plan.items[0].newTrack == -1);
  CHECK(plan.items[1].newTrack == 0);
  CHECK(plan.items[1].pending);
  CHECK(plan.items[1].length == kBar);  // a placeholder bar while pending
  // Pending files cannot be committed.
  CHECK_FALSE(sampleDropEdit(*f.ctx.doc(), f.ctx.store().ids(), plan, files, {}, {}).ok());
}

// The whole drop of 100 files: the first hover starts the probes, the drop waits for them (no UI poster here) and
// commits one Batch. Returns the milliseconds from the first hover to the committed state.
double dropHundred(Fixture& f) {
  std::vector<std::string> files;
  for (int i = 0; i < 100; ++i) files.push_back(makeWav(f.dir, "stem" + std::to_string(100 + i) + ".wav", 0.25));
  const size_t h0 = f.history();
  const Vec2 pos = f.at(f.audio, 0);
  DragPayload p;
  p.paths = files;
  p.label = "100 files";
  const auto t0 = std::chrono::steady_clock::now();
  f.d.tree().dragEnter(p, pos, 0);
  CHECK(f.d.tree().dragDrop(pos, 0) == DropEffect::Copy);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  MESSAGE("100 dropped files probed + committed in " << ms << " ms (" << f.ctx.fileProbes().workers() << " probe workers)");
  CHECK(f.history() == h0 + 1);
  CHECK(f.names().size() == 4 + 99);
  CHECK(doc::samplesOf(*f.ctx.doc()).size() == 100);
  return ms;
}

TEST_CASE("file drop: 100 files are probed in parallel and committed as one undo step") {
  Fixture f("hundred");
  (void)dropHundred(f);
  REQUIRE(f.ctx.undo());
  CHECK(f.names().size() == 4);
}

TEST_CASE("file drop: 100 files probed and committed within 300 ms" * doctest::test_suite("perf")) {
  Fixture f("hundred_perf");
  double best = 1e9;
  for (int k = 0; k < 3; ++k) {
    f.ctx.fileProbes().clear();
    best = std::min(best, dropHundred(f));
    REQUIRE(f.ctx.undo());
  }
  CHECK(best <= 300.0);
}
