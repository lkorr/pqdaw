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

#include "pq/document/edit_ops.h"
#include "pq/document/samples.h"
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

// An audio track playing `file` (a referenced 48 kHz stereo sample).
doc::SampleId addSample(ViewContext& ctx, const fs::path& file) {
  doc::DocumentStore& st = ctx.store();
  auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio);
  REQUIRE(t.ok());
  REQUIRE(st.dispatch(t->command).ok());
  const std::u8string u = file.generic_u8string();
  const std::string path(reinterpret_cast<const char*>(u.data()), u.size());
  auto id = doc::fileIdentity(path);
  REQUIRE(id.ok());
  doc::SampleRef r;
  r.id = st.ids().sample();
  r.path = doc::storedSamplePath(path, ctx.projectDir());
  r.size = id->size;
  r.contentHash = id->hash;
  r.frames = int64_t{1} << 22;
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

// M5 review H3: at sample-level zoom the arrangement asks for [floor(f0) − 2, ceil(f1) + 2), so a clip's file start
// asks from −2 and its end past the file. Those requests never matched the (clamped) read, so every paint queued a
// read and every arrival repainted, forever. After one read (and its notification) a settled view reads nothing more.
TEST_CASE("waveforms: deep-zoom windows over a file's start and end settle (no read or repaint per paint)") {
  const std::string dir = audioProjectCopy("waveforms_edges");
  FakeTransport port;
  ViewContext ctx(port);
  QueuePoster qp;
  ctx.setUiPoster(qp.poster());
  REQUIRE(ctx.openProject(dir).ok());
  int changes = 0;
  auto sub = ctx.observe([&](uint32_t bits) { changes += (bits & view_change::kWaveforms) ? 1 : 0; });
  Waveforms& w = ctx.waveforms();
  struct Case {
    int64_t a, b;
  };
  // beat.wav (id 3) has 88,200 frames.
  for (const Case c : {Case{-2, 500}, Case{87700, 88202}, Case{88100, 90000}, Case{1000, 1500}}) {
    CAPTURE(c.a);
    changes = 0;
    const uint64_t reads0 = w.windowReads();
    int hits = 0;
    for (int paint = 0; paint < 20; ++paint) {  // each paint asks once; a result that arrives repaints
      if (w.window(doc::SampleId{3}, c.a, c.b)) ++hits;
      w.waitIdle();
      qp.pump();
    }
    CHECK(w.windowReads() - reads0 <= 1);
    CHECK(changes <= 1);
    CHECK(hits >= 19);
  }
  // Wholly past the end: nothing to read, nothing to draw.
  const uint64_t reads0 = w.windowReads();
  CHECK(w.window(doc::SampleId{3}, 90000, 90500) == nullptr);
  CHECK(w.windowReads() == reads0);
  ctx.newProject();
  w.waitIdle();
  qp.pump();
  fs::remove_all(dir);
}

// M5 review M9: deep-zoom windows are an LRU cache with a byte cap (they used to stay until the project closed).
TEST_CASE("waveforms: sample windows stay within the window cache's byte cap, least recently used first") {
  const std::string dir = test::tempDir("waveforms_window_lru");
  FakeTransport port;
  ViewContext ctx(port);
  // Six 2^22-frame stereo files (32 MiB windows each): 192 MiB > the 128 MiB cap.
  std::vector<doc::SampleId> ids;
  {
    std::vector<std::vector<float>> ch(2, std::vector<float>(size_t{1} << 22, 0.25f));
    for (int i = 0; i < 6; ++i) {
      const fs::path f = fs::path(dir) / ("long" + std::to_string(i) + ".wav");
      REQUIRE(media::writeWav(f, ch, 48000).ok());
      ids.push_back(addSample(ctx, f));
    }
  }
  Waveforms& w = ctx.waveforms();
  for (doc::SampleId id : ids) REQUIRE(w.window(id, 0, int64_t{1} << 22) != nullptr);
  CHECK(w.windowBytes() <= Waveforms::kWindowCacheBytes);
  CHECK(w.window(ids.back(), 0, 1000) != nullptr);  // the newest is kept
  const uint64_t reads = w.windowReads();
  CHECK(w.window(ids.front(), 0, 1000) != nullptr);  // the oldest was dropped: read again
  CHECK(w.windowReads() == reads + 1);
  CHECK(w.windowBytes() <= Waveforms::kWindowCacheBytes);
  fs::remove_all(dir);
}
