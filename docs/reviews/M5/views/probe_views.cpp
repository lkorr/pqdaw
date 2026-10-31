// M5 review (views): dynamic probes. Usage: probe_views <repo root>
//  A. Waveforms::window at a clip's file start (frame0 = -2, as audio_paint.cpp asks) / past its end: does a read ever
//     satisfy it, or does every paint re-request (and every arrival repaint)?
//  B. Browser keys: a printable key typed while the list has focus also fires global chords (B, Space)?
//     Enter in the search field loads the selection?
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/ui/test_driver.h"
#include "pq/views/browser/browser_panel.h"
#include "pq/views/browser/demo_index.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/preview_port.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "pq/views/waveforms.h"

using namespace pq;
using namespace pq::views;
using namespace pq::ui;
namespace fs = std::filesystem;

struct QueuePoster {
  std::mutex m;
  std::vector<std::function<void()>> q;
  ViewContext::UiPoster poster() {
    return [this](std::function<void()> f) { std::lock_guard<std::mutex> l(m); q.push_back(std::move(f)); };
  }
  int pump() {
    std::vector<std::function<void()>> run;
    { std::lock_guard<std::mutex> l(m); run.swap(q); }
    for (auto& f : run) f();
    return static_cast<int>(run.size());
  }
};

static void probeWindows(const fs::path& root) {
  const fs::path dir = fs::temp_directory_path() / "pq_review_m5" / "wf";
  fs::remove_all(dir);
  fs::create_directories(dir);
  fs::copy(root / "tests/views/fixtures/audio", dir, fs::copy_options::recursive);
  FakeTransport port;
  ViewContext ctx(port);
  QueuePoster qp;
  ctx.setUiPoster(qp.poster());
  if (!ctx.openProject(dir.string()).ok()) { std::printf("A: open failed\n"); return; }
  int changes = 0;
  auto sub = ctx.observe([&](uint32_t bits) { if (bits & view_change::kWaveforms) ++changes; });
  Waveforms& w = ctx.waveforms();
  struct Case { const char* what; int64_t a, b; };
  const Case cases[] = {{"start of file  [-2, 500)", -2, 500}, {"end of file [87700, 88202)", 87700, 88202}, {"inside [1000, 1500)", 1000, 1500}};
  for (const Case& c : cases) {
    changes = 0;
    int hits = 0, nulls = 0;
    for (int paint = 0; paint < 20; ++paint) {  // each "paint" asks once, then the worker result arrives (repaint)
      if (w.window(doc::SampleId{3}, c.a, c.b)) ++hits; else ++nulls;
      w.waitIdle();
      qp.pump();
    }
    std::printf("A: window %-28s: 20 paints -> %d hits, %d nulls, %d kWaveforms notifications (each one a repaint)\n", c.what, hits, nulls, changes);
  }
  ctx.newProject();
  w.waitIdle();
  qp.pump();
}

static void probeBrowser(const fs::path& root) {
  FakeTransport port;
  ViewContext ctx(port);
  FakePreviewPort preview;
  TestDriver d(1280, 800);
  ctx.library().setPreview(&preview);
  ctx.library().setIndex(browserDemoIndex((root / "tests/views/fixtures/audio/samples").string(), 0));
  auto t = doc::ops::createTrack(*ctx.doc(), ctx.store().ids(), doc::TrackKind::Audio, {}, "Audio");
  (void)ctx.store().dispatch(t->command);
  ctx.setSelectedTrack(t->tracks[0]);
  ctx.setBrowserVisible(true);
  auto s = Shell::create(d.tree(), ctx);
  if (!s.ok()) { std::printf("B: shell failed\n"); return; }
  auto shell = std::move(*s);
  BrowserPanel* b = findBrowser(*shell);
  d.tree().layoutNow();
  d.step();
  b->setSource({BrowserSource::Kind::Place, 0});
  // B1: "b" typed with the list focused.
  d.tree().setFocus(&b->listWidget());
  const bool draw0 = ctx.drawMode();
  const KeyResult r1 = d.press("B");
  d.type("b");
  std::printf("B1: list focused, press B: result %d, drawMode %d -> %d, query \"%s\"\n", static_cast<int>(r1), draw0, ctx.drawMode(), b->query().c_str());
  // B2: Space with the list focused.
  b->setQuery("");
  d.tree().setFocus(&b->listWidget());
  const bool play0 = port.playhead().playing;
  const KeyResult r2 = d.press("Space");
  d.type(" ");
  std::printf("B2: list focused, press Space: result %d, transport playing %d -> %d, calls %zu, query \"%s\"\n", static_cast<int>(r2), play0,
              port.playhead().playing, port.calls.size(), b->query().c_str());
  // B3: Enter in the search field with a sample row selected.
  b->setQuery("beat");
  b->waitForSearch();
  d.tree().layoutNow();
  int32_t row = -1;
  for (size_t i = 0; i < b->rows().size(); ++i)
    if (!b->rows()[i].dir && std::string(b->index()->name(b->rows()[i].id)) == "beat.wav") row = static_cast<int32_t>(i);
  b->select(row, false);
  d.tree().setFocus(&b->searchField());
  const size_t h0 = ctx.store().history().cursor();
  const KeyResult r3 = d.press("Enter");
  std::printf("B3: search focused, row %d selected, press Enter: result %d, history %zu -> %zu (loaded: %s)\n", row, static_cast<int>(r3), h0,
              ctx.store().history().cursor(), ctx.store().history().cursor() != h0 ? "yes" : "NO");
  d.tree().setRoot(std::make_unique<Widget>());
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const fs::path root = argv[1];
  probeWindows(root);
  probeBrowser(root);
  return 0;
}
