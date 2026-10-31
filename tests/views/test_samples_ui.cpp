// T5.8: project samples through the ViewContext — missing samples on open (status line), relink from a folder (not an
// undo step), Collect All and Save with the copy on a worker (the UI thread stays free; project.pqd written last).

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/samples.h"
#include "pq/document/serialize.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::views;
namespace fs = std::filesystem;

namespace {

void writeBytes(const fs::path& p, size_t n, uint32_t seed) {
  fs::create_directories(p.parent_path());
  std::string s(n, '\0');
  uint32_t x = seed * 2654435761u + 1u;
  for (char& c : s) {
    x = x * 1664525u + 1013904223u;
    c = static_cast<char>(x >> 24);
  }
  std::ofstream(p, std::ios::binary).write(s.data(), static_cast<std::streamsize>(s.size()));
}

std::string u8(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// An audio track playing `file` (a referenced sample; the bytes are not audio: the document never decodes them).
doc::SampleId addSample(ViewContext& ctx, const fs::path& file) {
  doc::DocumentStore& st = ctx.store();
  auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio);
  REQUIRE(t.ok());
  REQUIRE(st.dispatch(t->command).ok());
  auto id = doc::fileIdentity(u8(file));
  REQUIRE(id.ok());
  doc::SampleRef r;
  r.id = st.ids().sample();
  r.path = doc::storedSamplePath(u8(file), ctx.projectDir());
  r.size = id->size;
  r.contentHash = id->hash;
  r.frames = 48000;
  r.rate = 48000.0;
  r.channels = 2;
  r.displayName = "sample";
  const doc::ops::NewAudioClip nc{t->tracks.at(0), 0, r.id, {}};
  auto c = doc::ops::addAudioClips(*st.current(), st.ids(), std::span<const doc::SampleRef>(&r, 1),
                                   std::span<const doc::ops::NewAudioClip>(&nc, 1));
  REQUIRE(c.ok());
  REQUIRE(st.dispatch(c->command).ok());
  return r.id;
}

struct FolderDialogs final : FileDialogs {
  std::string folder;
  void chooseProjectToOpen(PathCallback done) override { done({}); }
  void chooseProjectToSave(std::string_view, PathCallback done) override { done({}); }
  SaveChoice askSaveChanges(std::string_view) override { return SaveChoice::Discard; }
  void chooseFolder(PathCallback done) override { done(folder); }
};

}  // namespace

TEST_CASE("samples ui: missing samples on open, relink (not an undo step), the status line") {
  FakeTransport port;
  ViewContext ctx(port);
  const fs::path root = test::tempDir("samples_relink");
  const fs::path file = root / "library" / "hit.wav";
  writeBytes(file, 5000, 1);
  REQUIRE(ctx.saveProjectAs(u8(root / "Song")).ok());
  const doc::SampleId id = addSample(ctx, file);
  CHECK(ctx.missingSamples().empty());
  CHECK(ctx.sampleAvailable(id));
  CHECK(ctx.samplePath(id) == u8(file));
  REQUIRE(ctx.saveProject().ok());
  // The file moves away: on open the sample is missing and the status line says so.
  fs::create_directories(root / "elsewhere" / "deep");
  fs::rename(file, root / "elsewhere" / "deep" / "hit.wav");
  REQUIRE(ctx.openProject(u8(root / "Song")).ok());
  CHECK(ctx.missingSamples() == std::vector<doc::SampleId>{id});
  CHECK_FALSE(ctx.sampleAvailable(id));
  CHECK(ctx.status().find("1 sample missing") == 0);
  FolderDialogs dlg;
  ctx.setFileDialogs(&dlg);
  const ui::ActionIndex relink = ctx.actions().find("file.relinkSamples");
  CHECK(ctx.actions().enabled(relink));
  dlg.folder = u8(root / "elsewhere");
  const size_t cursor = ctx.store().history().cursor();
  REQUIRE(ctx.actions().invoke(relink));
  CHECK(ctx.missingSamples().empty());
  CHECK(ctx.store().history().cursor() == cursor);
  CHECK(ctx.samplePath(id) == u8(root / "elsewhere" / "deep" / "hit.wav"));
  CHECK(ctx.status().find("Relinked 1 of 1") == 0);
  CHECK_FALSE(ctx.actions().enabled(relink));
  CHECK(ctx.dirty());  // the new path is saved with the next save
  ctx.setFileDialogs(nullptr);
}

TEST_CASE("samples ui: Collect All and Save copies on a worker; the UI thread is not blocked by the copy") {
  FakeTransport port;
  ViewContext ctx(port);
  const fs::path root = test::tempDir("samples_collect");
  const fs::path file = root / "ext" / "big.wav";
  writeBytes(file, size_t{64} << 20, 2);  // 64 MB
  REQUIRE(ctx.saveProjectAs(u8(root / "Song")).ok());
  const doc::SampleId id = addSample(ctx, file);
  // The poster: work for the UI thread lands in a queue this test drains (the app: an SDL user event).
  std::mutex m;
  std::vector<std::function<void()>> queue;
  ctx.setUiPoster([&](std::function<void()> fn) {
    std::lock_guard lock(m);
    queue.push_back(std::move(fn));
  });
  const size_t cursor = ctx.store().history().cursor();
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(ctx.collectAllAndSave().ok());
  const double callMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ctx.collecting());
  MESSAGE("collectAllAndSave returned after " << callMs << " ms (the copy runs on a worker)");
  CHECK(ctx.status().find("Collecting samples") == 0);
  // The UI thread keeps running "frames" (draining the queue) until the copy is done.
  double worstFrameMs = 0.0;
  for (int frame = 0; ctx.collecting() && frame < 100000; ++frame) {
    const auto f0 = std::chrono::steady_clock::now();
    std::vector<std::function<void()>> work;
    {
      std::lock_guard lock(m);
      work.swap(queue);
    }
    for (auto& fn : work) fn();
    worstFrameMs = std::max(worstFrameMs,
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE_FALSE(ctx.collecting());
  MESSAGE("worst UI frame while 64 MB were copied: " << worstFrameMs << " ms (includes the final save)");
  CHECK(ctx.store().history().cursor() == cursor);  // not an undo step
  CHECK(doc::findSample(*ctx.doc(), id)->path == "samples/imported/big.wav");
  CHECK(fs::file_size(root / "Song" / "samples" / "imported" / "big.wav") == fs::file_size(file));
  CHECK(ctx.status().find("Collected 1 sample and saved") == 0);
  CHECK_FALSE(ctx.dirty());
  auto back = doc::loadProject(u8(root / "Song"));
  REQUIRE(back.ok());
  CHECK(*back->project == *ctx.doc());
  ctx.setUiPoster(nullptr);
  std::error_code ec;
  fs::remove_all(root, ec);
}

// M5 review M12: T5.8's criterion is "while 1 GB is copied, the UI's max frame stays <= 16 ms"; the test above copies
// 64 MB and times queue draining only. Here a 1 GiB file is collected while the shell runs real frames: every frame
// drains the UI poster and repaints the whole window (invalidateAll + TestDriver::step: layout and paint), and the
// frame that starts the collect counts too. Wall clock on a shared machine: `perf`.
TEST_CASE("samples ui (perf): while 1 GiB is collected, every UI frame (full repaint) stays <= 16 ms" * doctest::test_suite("perf")) {
  FakeTransport port;
  ViewContext ctx(port);
  ui::TestDriver d(1280, 800);
  const fs::path root = test::tempDir("samples_collect_1g");
  const fs::path file = root / "ext" / "huge.wav";
  {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    std::vector<char> chunk(size_t{16} << 20);
    uint32_t x = 12345u;
    for (char& c : chunk) {
      x = x * 1664525u + 1013904223u;
      c = static_cast<char>(x >> 24);
    }
    for (int i = 0; i < 64; ++i) {  // 64 × 16 MiB
      chunk[0] = static_cast<char>(i);
      out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
  }
  REQUIRE(fs::file_size(file) == uint64_t{1} << 30);
  REQUIRE(ctx.saveProjectAs(u8(root / "Song")).ok());
  (void)addSample(ctx, file);
  REQUIRE(d.enableGpu().ok());  // fonts and textures: frames paint text as the app's do
  auto s = Shell::create(d.tree(), ctx);
  REQUIRE(s.ok());
  auto shell = std::move(*s);
  d.tree().layoutNow();
  d.step();
  const uint64_t drawn0 = d.framesDrawn();
  std::mutex m;
  std::vector<std::function<void()>> queue;
  ctx.setUiPoster([&](std::function<void()> fn) {
    std::lock_guard lock(m);
    queue.push_back(std::move(fn));
  });
  std::vector<double> frames;
  auto frame = [&](auto&& extra) {
    const auto f0 = std::chrono::steady_clock::now();
    extra();
    std::vector<std::function<void()>> work;
    {
      std::lock_guard lock(m);
      work.swap(queue);
    }
    for (auto& fn : work) fn();
    d.tree().invalidateAll();
    (void)d.step();
    frames.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count());
  };
  const auto t0 = std::chrono::steady_clock::now();
  frame([&] { REQUIRE(ctx.collectAllAndSave().ok()); });
  for (int i = 0; ctx.collecting() && i < 200000; ++i) {
    frame([] {});
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  frame([] {});  // the frame that applies the end (the save)
  const double totalS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE_FALSE(ctx.collecting());
  CHECK(fs::file_size(root / "Song" / "samples" / "imported" / "huge.wav") == uint64_t{1} << 30);
  std::vector<double> sorted = frames;
  std::sort(sorted.begin(), sorted.end());
  MESSAGE("1 GiB collected in " << totalS << " s over " << frames.size() << " frames: max frame " << sorted.back()
                                << " ms, median " << sorted[sorted.size() / 2] << " ms, first (starts the copy) " << frames.front() << " ms");
  CHECK(d.framesDrawn() - drawn0 == frames.size());  // every frame painted
  CHECK(sorted.back() <= 16.0);
  d.tree().setRoot(std::make_unique<ui::Widget>());
  ctx.setUiPoster(nullptr);
  std::error_code ec;
  fs::remove_all(root, ec);
}
