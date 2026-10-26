// T4.13 benchmarks: the project patch view over a 64-track project (groups of eight, two returns, sends, an effect or
// two per track, a clip each) at 2560×1440 device px, CPU only (text through the CPU atlas, tiles through a CPU
// TileAtlas whose per-frame staging copy is counted, as in bench_circuit). Frames are the UI thread's own CPU time
// (QueryThreadCycleTime) for input → update → paint.
//   - the model: lowering, the project patch (model + layout + routes), the scene; the view's whole rebuild
//   - the whole project zoomed out (fitted: the far level, cached chunks): settled full paint, pan frames
//   - one band (zoomed onto it: the near level, symbol tiles and text): full paint, pan frames
//   - the morph from the arrangement and back: every frame at 144 Hz
// Acceptance (T4.13): zoomed-out full-project paint < 2 ms; the morph at 144 Hz with 64 tracks (a frame < 6.9 ms).
// Run from the ship build (bench target only); results → bench/RESULTS.md.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <intrin.h>

#include "pq/document/devices.h"
#include "pq/document/lower.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/tile_atlas.h"
#include "pq/ui/widget.h"
#include "pq/views/fake_transport.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq_bench.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::project_patch;

namespace {

uint64_t threadCycles() {
  ULONG64 c = 0;
  QueryThreadCycleTime(GetCurrentThread(), &c);
  return c;
}

double tscNsPerCycle() {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const uint64_t c0 = __rdtsc();
  while (clock::now() - t0 < std::chrono::milliseconds(200)) {
  }
  const uint64_t c1 = __rdtsc();
  return std::chrono::duration<double, std::nano>(clock::now() - t0).count() / static_cast<double>(c1 - c0);
}

}  // namespace

int main() {
  const double nsPerCycle = tscNsPerCycle();
  auto lib = doc::loadDeviceLibrary(std::string(PQ_SOURCE_DIR) + "/stdlib");
  if (!lib) {
    std::printf("device library: %s\n", lib.error().message.c_str());
    return 1;
  }
  FakeTransport port;
  ViewContext ctx(port);
  ctx.setDeviceLibrary(lib->get());
  if (Status s = buildWideProject(ctx.store(), 64); !s) {
    std::printf("project: %s\n", s.error().message.c_str());
    return 1;
  }
  const doc::Snapshot snap = ctx.doc();

  // ---- the model ----
  {
    doc::LowerOptions lo;
    lo.library = &(*lib)->library();
    std::vector<double> tl, tm, ts;
    std::shared_ptr<const doc::LoweredProject> low;
    ProjectPatch pp;
    for (int i = 0; i < 20; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      auto r = doc::lower(*snap, lo);
      const auto t1 = std::chrono::steady_clock::now();
      if (!r) return 1;
      low = std::make_shared<doc::LoweredProject>(std::move(*r));
      pp = buildProjectPatch(*snap, *low, &(*lib)->catalog());
      const auto t2 = std::chrono::steady_clock::now();
      const ProjectScene sc = buildProjectScene(pp);
      const auto t3 = std::chrono::steady_clock::now();
      tl.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
      tm.push_back(std::chrono::duration<double, std::nano>(t2 - t1).count());
      ts.push_back(std::chrono::duration<double, std::nano>(t3 - t2).count());
    }
    std::printf("64 tracks: %zu bands, %zu nodes, %zu edges, %zu graph wires\n", pp.bands.size(), pp.nodes.size(), pp.edges.size(),
                low->graph.wires.size());
    pq::bench::reportStats("project_patch.lower 64 tracks", pq::bench::summarize(tl), "ns/build");
    pq::bench::reportStats("project_patch.model (bands, nodes, edges, layout, routes) 64 tracks", pq::bench::summarize(tm), "ns/build");
    pq::bench::reportStats("project_patch.scene 64 tracks", pq::bench::summarize(ts), "ns/build");
  }

  // ---- the view in the shell ----
  auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(1.0f));
  if (!tex) {
    std::printf("scene text: %s\n", tex.error().message.c_str());
    return 1;
  }
  auto tiles = std::make_shared<TileAtlas>();
  PaintResources res = paintResources(*tex);
  res.tiles = tiles.get();
  std::vector<uint8_t> staging;
  auto stageTiles = [&] {
    const auto b = tiles->uploadBytes();
    if (staging.size() < b.size()) staging.resize(b.size());
    if (!b.empty()) std::memcpy(staging.data(), b.data(), b.size());
    tiles->clearUploads();
    tiles->beginFrame();
  };
  const uint32_t w = 2560, h = 1440;
  UiTree tree;
  tree.setViewport(w, h, UiScale(1.0f));
  tree.setResources(&res);
  auto shell = Shell::create(tree, ctx);
  if (!shell) {
    std::printf("shell: %s\n", shell.error().message.c_str());
    return 1;
  }
  ProjectPatchView* view = findProjectPatch(**shell);
  if (view == nullptr) return 1;
  DrawList dl;
  double now = 1.0;
  auto frame = [&] {
    now += 1.0 / 144.0;
    tree.update(now);
    tex->text->beginFrame();
    dl.reset(w, h);
    if (tree.needsPaint()) tree.paint(dl, res, now);
    stageTiles();
  };
  auto timed = [&](auto&& input) {
    const uint64_t c0 = threadCycles();
    input();
    frame();
    return static_cast<double>(threadCycles() - c0) * nsPerCycle;
  };
  auto settle = [&] {
    for (int i = 0; i < 2000 && (tree.needsFrame() || tree.nextWakeTime() <= now + 1.0); ++i) {
      now = std::max(now, std::min(tree.nextWakeTime(), now + 1.0)) - 1.0 / 144.0;
      frame();
    }
  };
  settle();
  tree.layoutNow();
  {
    std::vector<double> t;
    view->setShown(true, false);
    for (int i = 0; i < 10; ++i) {
      view->setMixerExpanded(snap->tracks[0]->id, i % 2 == 0);  // a rebuild each
      t.push_back(view->stats().lastRebuildMs * 1e6);
    }
    view->setMixerExpanded(snap->tracks[0]->id, false);
    pq::bench::reportStats("project_patch.view rebuild (lower + model + scene + canvas) 64 tracks", pq::bench::summarize(t), "ns/rebuild");
  }
  char name[160];
  auto fullPaint = [&] {
    tex->text->beginFrame();
    dl.reset(w, h);
    tree.paint(dl, res, now);
    stageTiles();
  };
  // The whole project, fitted (far level), then one band (near level).
  for (int level = 0; level < 2; ++level) {
    if (level == 0) view->fitAll(false);
    else view->focusBand(snap->tracks[0]->children.at(3)->id, false);
    settle();
    fullPaint();
    const circuit::CircuitCanvas& c = view->canvas();
    const char* lod = c.lod() == circuit::CircuitCanvas::Lod::Far ? "far" : (c.lod() == circuit::CircuitCanvas::Lod::Mid ? "mid" : "near");
    std::printf("%s: zoom %.3f (%s): chunks %u from tiles / %u direct, %u nodes painted (%u from tiles), %u vertices, %u commands\n",
                level == 0 ? "whole project" : "one band", c.view().zoom, lod, c.stats().chunksFromTiles, c.stats().chunksDirect,
                c.stats().nodesPainted, c.stats().nodesFromTiles, static_cast<unsigned>(dl.vertices().size()),
                static_cast<unsigned>(dl.commands().size()));
    std::snprintf(name, sizeof name, "project_patch.paint(full, settled, %s, zoom %.2f %s) 64 tracks %ux%u", level == 0 ? "whole project" : "one band",
                  c.view().zoom, lod, w, h);
    pq::bench::reportStats(name, pq::bench::measure(fullPaint, 1), "ns/frame");
    std::vector<double> pan;
    for (int i = 0; i < 300; ++i)
      pan.push_back(timed([&] { view->canvas().panBy((i / 50) % 2 ? -37.0f : 37.0f, (i / 75) % 2 ? -11.0f : 11.0f); }));
    std::snprintf(name, sizeof name, "project_patch.pan frame (%s, %s) 64 tracks %ux%u", level == 0 ? "whole project" : "one band", lod, w, h);
    pq::bench::reportStats(name, pq::bench::summarize(pan), "ns CPU/frame");
  }
  // The morph: back to the arrangement and in again, every frame timed at 144 Hz.
  view->setShown(false, false);
  settle();
  std::vector<double> morph;
  for (int round = 0; round < 6; ++round) {
    view->toggle();
    while (view->morphing() || tree.needsFrame()) {
      morph.push_back(timed([] {}));
      if (morph.size() > 100000) break;
    }
  }
  std::printf("morph: %zu frames over 6 morphs\n", morph.size());
  pq::bench::reportStats("project_patch.morph frame (arrangement <-> patch) 64 tracks 2560x1440", pq::bench::summarize(morph), "ns CPU/frame");
  return 0;
}
