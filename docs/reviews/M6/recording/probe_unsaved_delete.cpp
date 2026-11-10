// M6 review (recording): can the untitled-discard delete a WAV that a SAVED project references?
// A ViewContext with a fake transport, its unsaved recordings folder in a temp dir, and the discard hook wired to
// app::removeUnsavedFolder exactly as ProjectLink::discardUnsavedRecordings does (target = <root>/<session>).
// Recording commits are made with doc::ops::commitRecording (what ProjectLink::finishRecording dispatches), the WAV file
// placed in the unsaved folder (what RecordingService writes).
//   A: a take committed AFTER the first save (a recording running across Save As: its path was fixed at Record).
//   B: record, undo, Save As (only current samples move), redo, Save.
//   C: a project saved INTO the run's unsaved folder.
// Each: then New (an untitled project) and quit it → the saved project's WAV must still exist.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>

#include "pq/app/recording.h"
#include "pq/document/edit_ops.h"
#include "pq/document/samples.h"
#include "pq/views/view_context.h"

using namespace pq;
namespace fs = std::filesystem;

struct FakeTransport : views::TransportPort {
  void play(Ticks) override {}
  void stop() override {}
  void continuePlayback() override {}
  void locate(Ticks) override {}
  void setLoop(const views::TransportLoop&) override {}
  views::PlayheadSnapshot playhead() override { return {}; }
  void auditionHz(float, float, float) override {}
  void releaseAuditions() override {}
};

static std::string u8(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

struct Env {
  fs::path base, root, session, rec;
  FakeTransport tp;
  std::unique_ptr<views::ViewContext> ctx;
  doc::TrackId track;
  explicit Env(const char* tag) {
    std::random_device rd;
    base = fs::temp_directory_path() / ("pq_m6rev_unsaved_" + std::string(tag) + "_" + std::to_string(rd()));
    root = base / "pqdaw" / "Unsaved";
    session = root / "1234-5678";
    rec = session / "samples" / "recorded";
    fs::create_directories(rec);
    ctx = std::make_unique<views::ViewContext>(tp);
    ctx->setUnsavedRecordingDir(u8(rec));
    ctx->setUnsavedDiscardHook([this] {
      std::string why;
      const uint64_t n = app::removeUnsavedFolder(session, root, &why);
      std::printf("    [discard hook] removed %llu entries %s\n", static_cast<unsigned long long>(n), why.c_str());
    });
    auto t = doc::ops::createTrack(*ctx->doc(), ctx->store().ids(), doc::TrackKind::Audio);
    if (!t.ok() || !ctx->store().dispatch(t.value().command).ok()) std::abort();
    track = t.value().tracks.at(0);
  }
  ~Env() {
    ctx.reset();
    std::error_code ec;
    fs::remove_all(base, ec);
  }
  // What RecordingService + finishRecording do: a WAV in the unsaved folder, one commit referencing it (absolute).
  fs::path record(const char* name) {
    const fs::path wav = rec / name;
    { std::ofstream(wav, std::ios::binary) << "RIFF-user-audio"; }
    doc::SampleProbeFn probe = [](std::string_view) -> Result<doc::SampleProbe> { return doc::SampleProbe{48000, 48000.0, 1}; };
    auto ref = doc::makeSampleRef(ctx->store().ids(), u8(wav), ctx->projectDir(), probe);
    if (!ref.ok()) std::abort();
    doc::AudioClip a;
    a.name = "take";
    a.start = 0;
    a.sample = ref.value().id;
    a.seconds = 1.0;
    doc::ops::RecordingCommit c;
    doc::ops::RecordedTrack rt;
    rt.track = track;
    rt.passes.push_back(doc::ClipSlot{doc::ClipId{}, nullptr, std::make_shared<const doc::AudioClip>(std::move(a))});
    c.tracks.push_back(std::move(rt));
    c.samples.push_back(ref.value());
    auto e = doc::ops::commitRecording(*ctx->doc(), ctx->store().ids(), c);
    if (!e.ok() || !ctx->store().dispatch(e.value().command).ok()) std::abort();
    return wav;
  }
  // The path the saved project references for its (only) sample.
  std::string savedRef() const {
    const auto s = doc::samplesOf(*ctx->doc());
    return s.empty() ? std::string("<none>") : s.front().path;
  }
};

static int report(const char* name, const fs::path& wav, const std::string& ref) {
  const bool ok = fs::exists(wav);
  std::printf("  %s: saved project references \"%s\"; after New + quit the file %s -> %s\n", name, ref.c_str(),
              ok ? "EXISTS" : "IS DELETED", ok ? "ok" : "USER AUDIO LOST");
  return ok ? 0 : 1;
}

int main() {
  int bad = 0;
  {  // A: the take lands after the first save.
    Env e("A");
    std::printf("A: Save As, then a recording (path fixed while untitled) commits, Save, New, quit\n");
    if (!e.ctx->saveProjectAs(u8(e.base / "SongA")).ok()) std::abort();
    const fs::path wav = e.record("A take.wav");
    if (!e.ctx->saveProject().ok()) std::abort();
    const std::string ref = e.savedRef();
    e.ctx->newProject();
    (void)e.ctx->confirmQuit([] {});
    bad += report("A", wav, ref);
  }
  {  // B: undo before the first save, redo after it.
    Env e("B");
    std::printf("B: record, undo, Save As, redo, Save, New, quit\n");
    const fs::path wav = e.record("B take.wav");
    if (!e.ctx->undo()) std::abort();
    if (!e.ctx->saveProjectAs(u8(e.base / "SongB")).ok()) std::abort();
    if (!e.ctx->redo()) std::abort();
    if (!e.ctx->saveProject().ok()) std::abort();
    const std::string ref = e.savedRef();
    e.ctx->newProject();
    (void)e.ctx->confirmQuit([] {});
    bad += report("B", wav, ref);
  }
  {  // C: the project saved inside the run's unsaved folder.
    Env e("C");
    std::printf("C: record, Save As into <Unsaved>/<session>/SongC, New, quit\n");
    (void)e.record("C take.wav");
    const fs::path song = e.session / "SongC";
    if (!e.ctx->saveProjectAs(u8(song)).ok()) std::abort();
    const fs::path moved = song / "samples" / "recorded" / "C take.wav";
    const std::string ref = e.savedRef();
    e.ctx->newProject();
    (void)e.ctx->confirmQuit([] {});
    bad += report("C", moved, ref);
    std::printf("     project file %s\n", fs::exists(song) ? "exists" : "IS DELETED");
  }
  std::printf("%d of 3 scenarios lose user audio\n", bad);
  return bad;
}
