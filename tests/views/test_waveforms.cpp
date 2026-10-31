// T5.5: the views' waveform overviews (pq/views/waveforms.h) — synchronous without a UI poster, on the background
// workers with one (progress back through the poster, kWaveforms), the project / shared caches only when enabled,
// missing samples, sample windows, re-resolution.

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "pq/media/wav_file.h"
#include "pq/views/fake_transport.h"
#include "pq/views/view_context.h"
#include "pq/views/waveforms.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::views;
namespace fs = std::filesystem;

namespace {

// A copy of the `audio` fixture in a temp directory (tests may write caches there).
std::string audioProjectCopy(const char* name) {
  const std::string dir = test::tempDir(name);
  fs::copy(test::fixtureDir("audio"), dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
  return dir;
}

// A UI poster that queues the posted functions; pump() runs them (the window loop's user events).
struct QueuePoster {
  std::mutex m;
  std::vector<std::function<void()>> q;
  ViewContext::UiPoster poster() {
    return [this](std::function<void()> f) {
      std::lock_guard<std::mutex> l(m);
      q.push_back(std::move(f));
    };
  }
  int pump() {
    std::vector<std::function<void()>> run;
    {
      std::lock_guard<std::mutex> l(m);
      run.swap(q);
    }
    for (auto& f : run) f();
    return static_cast<int>(run.size());
  }
};

}  // namespace

TEST_CASE("waveforms: without a poster an overview builds on request; no cache is written unless enabled") {
  const std::string dir = audioProjectCopy("waveforms_sync");
  FakeTransport port;
  ViewContext ctx(port);
  REQUIRE(ctx.openProject(dir).ok());
  Waveforms& w = ctx.waveforms();
  const media::WaveformOverview* beat = w.overview(doc::SampleId{3});
  REQUIRE(beat != nullptr);
  CHECK(beat->complete());
  CHECK(beat->channels() == 2);
  CHECK(beat->frames() == 88200);
  CHECK(beat->sampleRate() == 44100);
  CHECK(w.overview(doc::SampleId{3}) == beat);  // cached
  CHECK(w.requests() == 1);
  CHECK(w.overview(doc::SampleId{5}) == nullptr);  // missing: no request
  CHECK(w.requests() == 1);
  CHECK_FALSE(fs::exists(fs::path(dir) / "cache"));

  // The project cache on: the next context reads it instead of decoding.
  w.clear();
  w.setProjectCache(true);
  REQUIRE(w.overview(doc::SampleId{4}) != nullptr);
  const fs::path cache = fs::path(dir) / "cache" / "waveforms";
  REQUIRE(fs::exists(cache));
  int files = 0;
  for (const auto& e : fs::directory_iterator(cache)) files += e.path().extension() == ".pqwf" ? 1 : 0;
  CHECK(files == 1);

  // Deep zoom: a window of samples.
  const media::SampleWindow* win = w.window(doc::SampleId{4}, 1000, 2000);
  REQUIRE(win != nullptr);
  CHECK(win->frame0 <= 1000);
  CHECK(win->frame0 + win->frames >= 2000);
  CHECK(w.window(doc::SampleId{4}, 1100, 1900) == win);  // inside the window read: no new read
  fs::remove_all(dir);
}

TEST_CASE("waveforms: with a poster, overviews build on the worker and arrive through the poster (kWaveforms)") {
  const std::string dir = audioProjectCopy("waveforms_async");
  FakeTransport port;
  ViewContext ctx(port);
  QueuePoster qp;
  ctx.setUiPoster(qp.poster());
  REQUIRE(ctx.openProject(dir).ok());
  uint32_t seen = 0;
  auto sub = ctx.observe([&](uint32_t bits) { seen |= bits; });
  Waveforms& w = ctx.waveforms();
  const uint64_t rev0 = w.revision();
  CHECK(w.overview(doc::SampleId{3}) == nullptr);  // requested, not there yet
  CHECK(w.overview(doc::SampleId{4}) == nullptr);
  CHECK(w.requests() == 2);
  for (int i = 0; i < 500 && (w.overview(doc::SampleId{3}) == nullptr || w.overview(doc::SampleId{4}) == nullptr); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    qp.pump();
  }
  REQUIRE(w.overview(doc::SampleId{3}) != nullptr);
  REQUIRE(w.overview(doc::SampleId{4}) != nullptr);
  CHECK(w.requests() == 2);  // each requested once
  CHECK(w.revision() > rev0);
  CHECK((seen & view_change::kWaveforms) != 0);
  // A window read: null first, then the poster brings it.
  CHECK(w.window(doc::SampleId{3}, 500, 900) == nullptr);
  const media::SampleWindow* win = nullptr;
  for (int i = 0; i < 500 && !win; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    qp.pump();
    win = w.window(doc::SampleId{3}, 500, 900);
  }
  CHECK(win != nullptr);
  // A new project drops everything (results of older requests are ignored).
  ctx.newProject();
  CHECK(w.overview(doc::SampleId{3}) == nullptr);
  w.waitIdle();
  qp.pump();
  fs::remove_all(dir);
}

TEST_CASE("waveforms: the context's destruction with builds in flight is clean (posted work after it is dropped)") {
  const std::string dir = audioProjectCopy("waveforms_teardown");
  QueuePoster qp;
  {
    FakeTransport port;
    ViewContext ctx(port);
    ctx.setUiPoster(qp.poster());
    REQUIRE(ctx.openProject(dir).ok());
    (void)ctx.waveforms().overview(doc::SampleId{3});
    (void)ctx.waveforms().overview(doc::SampleId{4});
  }
  qp.pump();  // drains posted after the context is gone: no-ops
  fs::remove_all(dir);
}
