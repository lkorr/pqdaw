// T4.2 / T4.3 benchmarks: the circuit canvas over a 10k-atom patch at 2560×1440 device px (1× and 2× UI scale), CPU
// only (text through the CPU atlas; symbol tiles through a CPU TileAtlas whose per-frame staging copy is counted, as
// in bench_arrangement). Frames are the UI thread's own CPU time (QueryThreadCycleTime) for input → update → paint.
//   - scene build (auto-layout + routing + scene) of the 10k-atom patch; auto-layout of 500-node patches (T4.3: < 50 ms)
//   - per zoom level (fitted = Far, 0.5 = Mid with tiles, 1.0 = Near with text): pan frames, zoom-step frames (stale
//     tiles stretched), settled full paints, and the first frames after a zoom settles (tile builds)
// Acceptance (T4.2): a pan / zoom frame < 2 ms at 2560×1440 with 10k atoms. Run from the ship build (bench target only);
// results → bench/RESULTS.md.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <tuple>
#include <vector>

#include <windows.h>
#include <intrin.h>

#include "pq/graph/atoms.h"
#include "pq/graph/layout.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/tile_atlas.h"
#include "pq/ui/widget.h"
#include "pq/views/circuit/circuit_canvas.h"
#include "pq/views/circuit/circuit_scene.h"
#include "pq_bench.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views::circuit;

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

// A large patch as a big device would be: `strands` signal paths (bands, voices, channels: an input, ~`perStrand` atoms
// of arithmetic, shaping, comparators and selects, one-sample feedback loops, an output), each with its own 3 params.
// (Params shared by every strand make one trunk per param across the whole drawing: QUESTIONS Q?-T4.3-a.)
graph::Patch bigPatch(uint32_t strands, uint32_t perStrand, uint32_t seed) {
  std::mt19937 rng(seed);
  graph::Patch p("Big");
  const graph::AtomId add = graph::atomId("add"), mul = graph::atomId("mul"), sub = graph::atomId("sub"),
                      tanh = graph::atomId("tanh"), abs = graph::atomId("abs"), gt = graph::atomId("gt"),
                      sel = graph::atomId("select"), z1 = graph::atomId("z1");
  for (uint32_t s = 0; s < strands; ++s) {
    std::vector<graph::NodeIndex> params;
    for (uint32_t i = 0; i < 3; ++i)
      params.push_back(p.addParam("p" + std::to_string(s) + "_" + std::to_string(i), graph::ParamDecl{}));
    std::vector<graph::NodeIndex> vals{p.addInput("in" + std::to_string(s), graph::portType(graph::Kind::Real, graph::Rate::Audio))};
    const uint32_t start = p.nodeCount();
    auto recent = [&] { return vals[vals.size() - 1 - rng() % std::min<size_t>(vals.size(), 4)]; };
    while (p.nodeCount() - start < perStrand) {
      const uint32_t r = rng() % 20;
      if (r < 11) {
        const graph::NodeIndex a = p.addAtom(r < 5 ? add : (r < 9 ? mul : sub));
        p.connect(recent(), a, 0);
        p.connect(rng() % 4 == 0 ? params[rng() % params.size()] : recent(), a, 1);
        vals.push_back(a);
      } else if (r < 14) {
        const graph::NodeIndex a = p.addAtom(r < 13 ? tanh : abs);
        p.connect(recent(), a, 0);
        vals.push_back(a);
      } else if (r < 17) {  // c = x > p ? a : b
        const graph::NodeIndex c = p.addAtom(gt), q = p.addAtom(sel);
        p.connect(recent(), c, 0);
        p.connect(params[rng() % params.size()], c, 1);
        p.connect(c, q, 0);
        p.connect(recent(), q, 1);
        p.connect(recent(), q, 2);
        vals.push_back(q);
      } else {  // y = z1(x + y · k)
        const graph::NodeIndex a = p.addAtom(add), d = p.addAtom(z1), m = p.addAtom(mul);
        p.connect(recent(), a, 0);
        p.connect(a, d, 0);
        p.connect(d, m, 0);
        p.connect(params[rng() % params.size()], m, 1);
        p.connect(m, a, 1);
        vals.push_back(d);
      }
    }
    const graph::NodeIndex o = p.addOutput("out" + std::to_string(s), graph::portType(graph::Kind::Real, graph::Rate::Audio));
    p.connect(vals.back(), o, 0);
  }
  return p;
}

uint32_t atomCount(const graph::Patch& p) {
  uint32_t n = 0;
  for (const graph::Node& nd : p.nodes()) n += nd.kind == graph::NodeKind::Atom ? 1u : 0u;
  return n;
}

}  // namespace

int main() {
  const double nsPerCycle = tscNsPerCycle();
  char name[160];

  // ---- T4.3: auto-layout + routing of 500-node patches (5 strands × 100) and the 10k-atom scene build ----
  {
    std::vector<double> times;
    for (uint32_t seed = 1; seed <= 20; ++seed) {
      graph::Patch p = bigPatch(5, 100, seed);
      const auto t0 = std::chrono::steady_clock::now();
      const graph::AutoLayoutStats st = tidy(p);
      times.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count());
      if (seed == 1)
        std::printf("500-node patch: %u nodes, %u layers, %u dummies, %u feedback, crossings %llu -> %llu, %u tracks\n", p.nodeCount(),
                    st.layers, st.dummies, st.reversed, static_cast<unsigned long long>(st.crossingsInitial),
                    static_cast<unsigned long long>(st.crossings), st.tracks);
    }
    pq::bench::reportStats("circuit.tidy (auto-layout + route) 500 nodes", pq::bench::summarize(times), "ns/layout");
  }
  graph::Patch big = bigPatch(100, 100, 7);
  std::printf("10k patch: %u nodes (%u atoms), %u wires\n", big.nodeCount(), atomCount(big), big.wireCount());
  std::shared_ptr<CircuitScene> scene;
  {
    std::vector<double> times;
    for (int i = 0; i < 3; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      scene = buildScene(big);
      times.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count());
    }
    pq::bench::reportStats("circuit.buildScene (auto-layout + route + scene) 10k atoms", pq::bench::summarize(times), "ns/build");
    std::printf("  scene: %zu nodes, %zu wires, %zu segments, %zu junctions, bounds %.0f x %.0f world, grid %u x %u\n",
                scene->nodes.size(), scene->wires.size(), scene->segments.size(), scene->junctions.size(),
                static_cast<double>(scene->x1 - scene->x0), static_cast<double>(scene->y1 - scene->y0), scene->grid.cols,
                scene->grid.rows);
  }

  for (const auto [lw, lh, scale] : {std::tuple{2560u, 1440u, 1.0f}, std::tuple{1280u, 720u, 2.0f}}) {
    auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(scale));
    if (!tex) {
      std::printf("scene text: %s\n", tex.error().message.c_str());
      return 1;
    }
    auto tiles = std::make_shared<TileAtlas>();
    PaintResources res = paintResources(*tex);
    res.tiles = tiles.get();
    std::vector<uint8_t> staging;
    auto stageTiles = [&] {  // the renderer's per-frame part: copy the new tiles into the staging ring, frame boundary
      const auto b = tiles->uploadBytes();
      if (staging.size() < b.size()) staging.resize(b.size());
      if (!b.empty()) std::memcpy(staging.data(), b.data(), b.size());
      tiles->clearUploads();
      tiles->beginFrame();
    };
    const UiScale s(scale);
    const auto w = static_cast<uint32_t>(s.px(static_cast<float>(lw))), h = static_cast<uint32_t>(s.px(static_cast<float>(lh)));
    UiTree tree;
    tree.setViewport(w, h, s);
    tree.setResources(&res);
    auto& canvas = tree.emplaceRoot<CircuitCanvas>("circuit");
    canvas.setScene(scene);
    tree.layoutNow();
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
    const CircuitCanvas::View fitted = canvas.view();
    std::printf("\n%ux%u@%.0fx: fitted zoom %.3f\n", w, h, static_cast<double>(scale), fitted.zoom);
    const Vec2 centre{static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.5f};
    for (const double zoom : {fitted.zoom, 0.5, 1.0}) {
      // The same world point at the centre (the middle of the patch) at each zoom.
      const double hw = 0.5 * static_cast<double>(lw), hh = 0.5 * static_cast<double>(lh);
      CircuitCanvas::View v;
      v.zoom = zoom;
      v.x = fitted.x + hw / fitted.zoom - hw / zoom;
      v.y = fitted.y + hh / fitted.zoom - hh / zoom;
      canvas.setView(v);
      settle();
      // Settled full paint.
      auto fullPaint = [&] {
        tex->text->beginFrame();
        dl.reset(w, h);
        tree.paint(dl, res, now);
        stageTiles();
      };
      fullPaint();
      const CircuitCanvas::Stats st = canvas.stats();
      const char* lod = canvas.lod() == CircuitCanvas::Lod::Far ? "far" : (canvas.lod() == CircuitCanvas::Lod::Mid ? "mid" : "near");
      std::printf("  zoom %.3f (%s): %u nodes painted (%u from tiles, %u direct), %u wires / %u segments, %u junctions, %u labels; "
                  "far chunks %u from tiles / %u direct; %u vertices, %u commands; tile pages %u\n",
                  zoom, lod, st.nodesPainted, st.nodesFromTiles, st.nodesDirect, st.wiresPainted, st.segmentsPainted, st.junctionsPainted,
                  st.labelsPainted, st.chunksFromTiles, st.chunksDirect, static_cast<unsigned>(dl.vertices().size()),
                  static_cast<unsigned>(dl.commands().size()), tiles->pageCount());
      std::snprintf(name, sizeof name, "circuit.paint(full, settled, zoom %.2f %s) 10k atoms %ux%u@%.0fx", zoom, lod, w, h,
                    static_cast<double>(scale));
      pq::bench::reportStats(name, pq::bench::measure(fullPaint, 1), "ns/frame");
      // From no tiles at all (a cold start at this zoom): every frame until idle, the first one drawing directly.
      {
        canvas.dropTiles();
        std::vector<double> cold;
        for (int i = 0; i < 400 && (cold.empty() || tree.needsFrame() || tree.nextWakeTime() <= now + 1.0); ++i) {
          now = std::max(now, std::min(tree.nextWakeTime(), now + 1.0)) - 1.0 / 144.0;
          cold.push_back(timed([] {}));
        }
        std::snprintf(name, sizeof name, "circuit.cold-tiles frames (zoom %.2f %s) %ux%u@%.0fx", zoom, lod, w, h, static_cast<double>(scale));
        pq::bench::reportStats(name, pq::bench::summarize(cold), "ns CPU/frame");
        std::printf("  cold tiles: %zu frames to idle\n", cold.size());
      }
      // Pan frames: 37 device px per frame, back and forth.
      std::vector<double> pan;
      for (int i = 0; i < 300; ++i) pan.push_back(timed([&] { canvas.panBy((i / 50) % 2 ? -37.0f : 37.0f, (i / 75) % 2 ? -11.0f : 11.0f); }));
      std::snprintf(name, sizeof name, "circuit.pan frame (zoom %.2f %s) 10k atoms %ux%u@%.0fx", zoom, lod, w, h, static_cast<double>(scale));
      pq::bench::reportStats(name, pq::bench::summarize(pan), "ns CPU/frame");
      // Zoom-step frames: one wheel notch in and out at the centre (the previous level's tiles stretched).
      std::vector<double> zf;
      for (int i = 0; i < 300; ++i) zf.push_back(timed([&] { canvas.zoomAt(centre, i % 2 ? 1.0 / 1.15 : 1.15); }));
      std::snprintf(name, sizeof name, "circuit.zoom frame (zoom %.2f %s) 10k atoms %ux%u@%.0fx", zoom, lod, w, h, static_cast<double>(scale));
      pq::bench::reportStats(name, pq::bench::summarize(zf), "ns CPU/frame");
      // After the zoom: the frames until idle (the settle timer, the tile builds, the repaint).
      std::vector<double> after;
      for (int i = 0; i < 400 && (tree.needsFrame() || tree.nextWakeTime() <= now + 1.0); ++i) {
        now = std::max(now, std::min(tree.nextWakeTime(), now + 1.0)) - 1.0 / 144.0;
        after.push_back(timed([] {}));
      }
      if (!after.empty()) {
        std::snprintf(name, sizeof name, "circuit.after-zoom frames (tile builds; zoom %.2f %s) %ux%u@%.0fx", zoom, lod, w, h,
                      static_cast<double>(scale));
        pq::bench::reportStats(name, pq::bench::summarize(after), "ns CPU/frame");
        std::printf("  after zoom: %zu frames to idle, %llu tiles built in all\n", after.size(),
                    static_cast<unsigned long long>(canvas.stats().tilesBuilt));
      }
      std::printf("  idle: %s\n", (!tree.needsFrame() && std::isinf(tree.nextWakeTime())) ? "yes (no frames, no timers)" : "NO");
    }
    tree.setRoot(std::make_unique<Widget>());
  }
  return 0;
}
