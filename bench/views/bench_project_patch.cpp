// T4.13 benchmarks: the project patch view over a 64-track project (groups of eight, two returns, sends, an effect or
// two per track, a clip each) at 2560×1440 device px, CPU only (text through the CPU atlas, tiles through a CPU
// TileAtlas whose per-frame staging copy is counted, as in bench_circuit). Frames are the UI thread's own CPU time
// (QueryThreadCycleTime) for input → update → paint.
//   - the model: lowering, the project patch (model + layout + routes), the scene; the view's whole rebuild
//   - the whole project zoomed out (fitted: the far level, cached chunks): settled full paint, pan frames
//   - one band (zoomed onto it: the near level, symbol tiles and text): full paint, pan frames
//   - the morph from the arrangement and back: every frame at 144 Hz
//   - T5.x-circuit-chain: the track circuit view over Operator -> Auto Filter -> EQ Eight -> Saturator (the chain's
//     layout + scene, cold and from cached device scenes; full paints and pan frames fitted (far) and at zoom 1 (near))
//   - TMM.4: the project plus 32 Phase accumulator faces at 2x: pan frames with faces vs plain blocks (far, near); 16
//     faces x 8 animated voices (animation frames, a full paint); idle after the voices stop
//   - TX.block-drag: a block dragged live by real pointer events (the block drawn at the pointer each frame, its cables
//     on its ports): one band framed (near: a panel with its cables) and the whole project (far), each move a frame;
//     the drop (nudge + re-layout + the push-apart's check)
//   - TX.cables (`pq_bench_project_patch cables`, also at the end of a full run): 200 hanging cables between 40 blocks
//     in view at zoom 1 (2560x1440): the simulation alone (200 awake ropes, a 60 Hz frame's steps), a settled full paint,
//     frames while all 200 swing (a drag offset each frame: sim + damage + paint), frames while all 200 glow (poll +
//     paint), and idle after they settle (frames and timers: none); TX.orbit-ports: the same cables with orbiting main
//     ports — one block dragged (its ports and its cables' other ends solved per move) and every block dragged

// Acceptance (T4.13): zoomed-out full-project paint < 2 ms; the morph at 144 Hz with 64 tracks (a frame < 6.9 ms).
// Run from the ship build (bench target only); results → bench/RESULTS.md.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>
#include <intrin.h>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/tile_atlas.h"
#include "pq/ui/widget.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/circuit/cables.h"
#include "pq/views/circuit/circuit_canvas.h"
#include "pq/views/fake_transport.h"
#include "pq/views/meta/face_targets.h"
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

// ---- TMM.4: meta-module faces ----------------------------------------------------------------------------------
// The 64-track project with 32 Phase accumulator faces (instead of the first 32 instruments), at 2x UI scale, 2560x1440
// device px: pan frames with the faces drawn vs the same project with the face nodes drawn as plain blocks
// (CircuitCanvas::setFacesShown(false), an exact A/B of the faces' cost), fitted (far) and one band (near); then 16 faces
// in view at the mid level with 8 animated voices each (synthetic taps, advanced 1/60 s per frame): the frames the
// face timer drives (only the live rects repaint) and a full paint; idle: frames and timers after the voices stop.
// TMM.7: the same with 32 Wavetable faces (the demo table: 64 dense frames; `wavetable`), voices spread over positions.
int facesBench(const doc::DeviceLibrary& lib, double nsPerCycle, bool wavetable) {
  const char* tag = wavetable ? "wavetable_faces" : "meta_faces";
  FakeTransport port;
  ViewContext ctx(port);
  ctx.setDeviceLibrary(&lib);
  if (Status s = buildWideProject(ctx.store(), 64); !s) return 1;
  if (Status s = wavetable ? addWavetableFaces(ctx.store(), 32, demoWavetableData()) : addPhaseAccFaces(ctx.store(), 32); !s) {
    std::printf("faces: %s\n", s.error().message.c_str());
    return 1;
  }
  auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(2.0f));
  if (!tex) return 1;
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
  tree.setViewport(w, h, UiScale(2.0f));
  tree.setResources(&res);
  auto shell = Shell::create(tree, ctx);
  if (!shell) return 1;
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
  auto fullPaint = [&] {
    tex->text->beginFrame();
    dl.reset(w, h);
    tree.paint(dl, res, now);
    stageTiles();
  };
  tree.layoutNow();
  view->setShown(true, false);
  settle();
  circuit::CircuitCanvas& c = view->canvas();
  std::printf("faces: %zu faces hosted\n", c.faceCount());
  char name[200];
  const doc::Snapshot snap = ctx.doc();
  for (int level = 0; level < 2; ++level) {
    if (level == 0) view->fitAll(false);
    else view->focusBand(snap->tracks[0]->children.at(3)->id, false);
    settle();
    const char* lod = c.lod() == circuit::CircuitCanvas::Lod::Far ? "far" : (c.lod() == circuit::CircuitCanvas::Lod::Mid ? "mid" : "near");
    for (int shown = 1; shown >= 0; --shown) {
      c.setFacesShown(shown != 0);
      settle();
      fullPaint();
      std::printf("faces %s: %s zoom %.3f: %u faces painted (%u layers from tiles), %u nodes painted, %u vertices\n",
                  shown ? "on" : "off", lod, c.view().zoom, c.stats().facesPainted, c.stats().faceLayersFromTiles, c.stats().nodesPainted,
                  static_cast<unsigned>(dl.vertices().size()));
      std::vector<double> pan;
      for (int i = 0; i < 300; ++i)
        pan.push_back(timed([&] { c.panBy((i / 50) % 2 ? -37.0f : 37.0f, (i / 75) % 2 ? -11.0f : 11.0f); }));
      std::snprintf(name, sizeof name, "%s.pan frame (%s, %s, %s) 64 tracks + 32 faces 2x %ux%u", tag, level == 0 ? "whole project" : "one band",
                    lod, shown ? "faces" : "plain blocks", w, h);
      pq::bench::reportStats(name, pq::bench::summarize(pan), "ns CPU/frame");
    }
    c.setFacesShown(true);
  }
  // 16 faces in view (far level), 8 animated voices each.
  std::vector<std::unique_ptr<SyntheticVoices>> voices;
  view->faceVoices = [&](doc::DeviceId, std::string_view path) -> VoiceTapReader* {
    if (!path.empty()) return nullptr;
    auto v = std::make_unique<SyntheticVoices>();
    std::vector<SyntheticVoices::Voice> vs;
    for (uint32_t k = 0; k < 8; ++k)
      vs.push_back({k, 1.5 + 0.7 * k + 0.1 * static_cast<double>(voices.size()), 0.125 * k, wavetable ? 0.1 * k : 0.0});
    v->setVoices(std::move(vs));
    voices.push_back(std::move(v));
    return voices.back().get();
  };
  view->rebindFaces();
  {
    const Band& b0 = view->patch().bands.at(1);
    circuit::CircuitCanvas::View v;
    v.zoom = 0.21;  // 16+ faces in view (the far level: faces are still drawn in full)
    v.x = b0.x0 - 8.0;
    v.y = b0.y - 8.0;
    c.setView(v);
  }
  settle();
  for (auto& v : voices) v->advance(1.0 / 60.0);
  c.wakeFaces();
  fullPaint();
  std::printf("animated: zoom %.3f, %u faces painted, %u face primitives\n", c.view().zoom, c.stats().facesPainted, c.stats().facePrimitives);
  const uint32_t painted = c.stats().facesPainted;
  std::snprintf(name, sizeof name, "%s.paint(full, %u faces x 8 animated voices, zoom %.2f) 2x 2560x1440", tag, c.stats().facesPainted, c.view().zoom);
  pq::bench::reportStats(name, pq::bench::measure(fullPaint, 1), "ns/frame");
  std::vector<double> anim;
  for (int i = 0; i < 300; ++i)
    anim.push_back(timed([&] {
      for (auto& v : voices) v->advance(1.0 / 60.0);
      c.wakeFaces();  // what the face timer does at 60 Hz: poll, damage the live rects
    }));
  std::snprintf(name, sizeof name, "%s.animation frame (%u faces x 8 voices: poll + live rects repainted) 2x %ux%u", tag, painted, w, h);
  pq::bench::reportStats(name, pq::bench::summarize(anim), "ns CPU/frame");
  // Silent again: no frames, no timers.
  for (auto& v : voices) v->setVoices({});
  c.wakeFaces();
  settle();
  const bool idle = !tree.needsFrame() && std::isinf(tree.nextWakeTime());
  std::printf("%s idle after the voices stop: %s (animating %d)\n", tag, idle ? "yes (no frames, no timers)" : "NO", c.animatingFaces() ? 1 : 0);
  view->faceVoices = nullptr;
  view->rebindFaces();
  return idle ? 0 : 1;
}


// ---- TX.cables ---------------------------------------------------------------------------------------------------
// 200 cables between 40 plain blocks (8 × 5 over 2400 × 1300 world units), every cable in view at zoom 1, UI scale 1,
// 2560x1440: what the UI thread pays per frame for the rope simulation and the tubes (shadow, tube, plugs; glow and
// flow when lit). The scene is built by hand (the canvas's own path: SceneWire::cable, rest shapes, indexScene).
int cablesBench(double nsPerCycle) {
  auto sc = std::make_shared<circuit::CircuitScene>();
  constexpr uint32_t kCols = 8, kRows = 5, kCables = 200;
  for (uint32_t r = 0; r < kRows; ++r)
    for (uint32_t c = 0; c < kCols; ++c) {
      circuit::SymbolSpec spec;
      spec.symbol = circuit::Symbol::Box;
      spec.w = 120.0f;
      spec.h = 80.0f;
      spec.bodyKey = 77;
      for (int k = 0; k < 5; ++k) {
        circuit::PinSpec ps;
        ps.y = 12.0f + 14.0f * static_cast<float>(k);
        spec.inputs.push_back(ps);
        spec.outputs.push_back(ps);
      }
      circuit::SceneNode n;
      n.x = 60.0f + 300.0f * static_cast<float>(c);
      n.y = 60.0f + 260.0f * static_cast<float>(r);
      n.w = spec.w;
      n.h = spec.h;
      n.spec = static_cast<uint32_t>(sc->specs.size());
      n.node = static_cast<graph::NodeIndex>(sc->nodes.size());
      n.firstPin = static_cast<uint32_t>(sc->pins.size());
      for (int side = 0; side < 2; ++side)
        for (int k = 0; k < 5; ++k) {
          circuit::ScenePin pin;
          pin.input = side == 0;
          pin.x = side == 0 ? n.x : n.x + n.w;
          pin.y = n.y + spec.inputs[static_cast<size_t>(k)].y;
          pin.node = n.node;
          pin.port = static_cast<uint16_t>(k);
          pin.connected = true;
          sc->pins.push_back(pin);
        }
      n.pinCount = 10;
      sc->specs.push_back(spec);
      sc->nodes.push_back(n);
    }
  uint32_t seed = 12345;
  auto rnd = [&](uint32_t m) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % m;
  };
  std::vector<graph::Point2> rest;
  for (uint32_t i = 0; i < kCables; ++i) {
    const uint32_t a = rnd(kCols * kRows);
    uint32_t b = rnd(kCols * kRows);
    if (b == a) b = (b + 1) % (kCols * kRows);
    const circuit::SceneNode& na = sc->nodes[a];
    const circuit::SceneNode& nb = sc->nodes[b];
    const uint32_t fromPin = na.firstPin + 5 + rnd(5), toPin = nb.firstPin + rnd(5);
    const graph::Point2 p0{sc->pins[fromPin].x, sc->pins[fromPin].y}, p1{sc->pins[toPin].x, sc->pins[toPin].y};
    const uint32_t n = circuit::cablePointCount(std::hypot(p1.x - p0.x, p1.y - p0.y));
    rest.resize(n);
    circuit::cableRest(p0, p1, n, rest.data());
    circuit::SceneWire sw;
    sw.firstPoint = static_cast<uint32_t>(sc->points.size());
    sw.pointCount = n;
    sw.style.rate = circuit::WireRate::Audio;
    sw.style.modulation = i % 3 == 1;
    sw.style.notes = i % 7 == 3;
    sw.net = i;
    sw.wire = i;
    sw.fromPin = fromPin;
    sw.toPin = toPin;
    sw.cable = true;
    sw.cableKey = i + 1;
    sw.x0 = sw.x1 = p0.x, sw.y0 = sw.y1 = p0.y;
    for (const graph::Point2& q : rest) {
      sw.x0 = std::min(sw.x0, q.x), sw.x1 = std::max(sw.x1, q.x);
      sw.y0 = std::min(sw.y0, q.y), sw.y1 = std::max(sw.y1, q.y);
    }
    for (uint32_t k = 0; k + 1 < n; ++k) sc->segments.push_back({i, sw.firstPoint + k});
    sc->points.insert(sc->points.end(), rest.begin(), rest.end());
    sc->wires.push_back(sw);
  }
  circuit::indexScene(*sc);
  std::printf("cables: %u cables, %zu points, %zu nodes\n", kCables, sc->points.size(), sc->nodes.size());

  // The simulation alone: 200 awake ropes (every end moved), the steps of a 60 Hz frame (2 × 1/120 s).
  {
    circuit::CableSim sim;
    std::vector<circuit::CableSim::Spec> specs;
    for (const circuit::SceneWire& sw : sc->wires)
      specs.push_back({sw.cableKey, sc->points[sw.firstPoint], sc->points[sw.firstPoint + sw.pointCount - 1]});
    sim.sync(specs);
    std::vector<double> t;
    for (int round = 0; round < 60; ++round) {
      const float d = round % 2 ? -30.0f : 30.0f;
      for (uint32_t i = 0; i < sim.size(); ++i) sim.setEnds(i, {sim.endA(i).x + d, sim.endA(i).y}, sim.endB(i));
      for (int f = 0; f < 4; ++f) {
        const uint64_t c0 = threadCycles();
        (void)sim.advance(1.0 / 60.0);
        t.push_back(static_cast<double>(threadCycles() - c0) * nsPerCycle);
      }
    }
    std::printf("cables.sim: %u awake after the rounds\n", sim.awakeCount());
    pq::bench::reportStats("cables.sim step (200 awake ropes, one 60 Hz frame: 2 steps of 1/120 s)", pq::bench::summarize(t), "ns CPU/frame");
  }

  // The canvas in a tree (CPU paint).
  auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(1.0f));
  if (!tex) return 1;
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
  circuit::CircuitCanvas& c = tree.emplaceRoot<circuit::CircuitCanvas>("cables");
  bool live = false;
  c.cableSignalLive = [&] { return live; };
  c.cableLevels = [&](std::span<const uint32_t>, std::span<float> levels) {
    for (float& l : levels) l = live ? 0.8f : 0.0f;
  };
  tree.layoutNow();
  c.setScene(sc, false);
  c.setView({0.0, 0.0, 1.0});
  DrawList dl;
  double now = 1.0;
  uint64_t frames = 0;
  auto frame = [&] {
    now += 1.0 / 144.0;
    tree.update(now);
    tex->text->beginFrame();
    dl.reset(w, h);
    if (tree.needsPaint()) {
      tree.paint(dl, res, now);
      ++frames;
    }
    stageTiles();
  };
  auto timed = [&](auto&& input) {
    const uint64_t c0 = threadCycles();
    input();
    frame();
    return static_cast<double>(threadCycles() - c0) * nsPerCycle;
  };
  auto settle = [&] {
    for (int i = 0; i < 4000 && (tree.needsFrame() || tree.nextWakeTime() <= now + 1.0); ++i) {
      now = std::max(now, std::min(tree.nextWakeTime(), now + 1.0)) - 1.0 / 144.0;
      frame();
    }
  };
  auto fullPaint = [&] {
    tex->text->beginFrame();
    dl.reset(w, h);
    tree.paint(dl, res, now);
    stageTiles();
  };
  settle();
  fullPaint();
  std::printf("cables: zoom %.2f, %u cables painted, %u vertices, %zu indices\n", c.view().zoom, c.stats().cablesPainted,
              static_cast<unsigned>(dl.vertices().size()), dl.indices().size());
  pq::bench::reportStats("cables.paint full, 200 cables at rest (zoom 1, 2560x1440)", pq::bench::measure(fullPaint, 1), "ns/frame");
  // All 200 swing: every block dragged along a small loop, a step per frame (each frame: the ends move, the ropes step,
  // their old and new rects are damaged and repainted).
  std::vector<uint32_t> all;
  for (uint32_t v = 0; v < sc->nodes.size(); ++v) all.push_back(v);
  std::vector<double> swing;
  for (int i = 0; i < 120; ++i) {
    const float t = static_cast<float>(i) / 120.0f;
    swing.push_back(timed([&] { c.setNodeDrag(all, {40.0f * std::sin(6.2831853f * 2.0f * t), 25.0f * std::cos(6.2831853f * 3.0f * t)}); }));
  }
  std::printf("cables: moving %d during the drag frames, %u cables painted per frame\n", c.cablesMoving() ? 1 : 0, c.stats().cablesPainted);
  pq::bench::reportStats("cables.swing frame (200 cables awake: drag + sim + damage + paint) 2560x1440", pq::bench::summarize(swing), "ns CPU/frame");
  c.clearNodeDrag();
  const double t0 = now;
  while (c.cablesMoving() && now - t0 < 10.0) frame();
  std::printf("cables: every cable asleep %.2f s after the drag ended (frames stop then)\n", now - t0);
  settle();
  // TX.orbit-ports: the same 200 cables with every block's first input and first output ORBITING its outline (about
  // 80 cables end on an orbiting port): one block dragged (its ports and its cables' other ends solved each move, the
  // slides eased, the ropes swing), and every block dragged (every port solved) — against cables.swing above.
  {
    auto so = std::make_shared<circuit::CircuitScene>(*sc);
    for (uint32_t v = 0; v < so->nodes.size(); ++v) {
      circuit::SceneNode& n = so->nodes[v];
      n.outline.x0 = n.x + 8.0f, n.outline.y0 = n.y, n.outline.x1 = n.x + n.w - 8.0f, n.outline.y1 = n.y + n.h, n.outline.corner = 2.0f;
      for (const uint32_t k : {0u, 5u}) {
        circuit::ScenePin& pin = so->pins[n.firstPin + k];
        pin.orbit = true;
        pin.angle = k == 0 ? circuit::kOrbitDefaultIn : circuit::kOrbitDefaultOut;
        pin.orbitKey = (static_cast<uint64_t>(v) << 1 | (k == 0 ? 0u : 1u)) + 1;
        const graph::Point2 at = circuit::orbitPoint(n.outline, pin.angle, 1.0f);
        pin.x = at.x, pin.y = at.y;
      }
    }
    c.setScene(so, false);
    settle();
    const uint32_t one[1] = {static_cast<uint32_t>(so->nodes.size() / 2)};
    std::vector<double> drag1, dragAll;
    uint64_t s0 = c.portSolves();
    for (int i = 0; i < 120; ++i) {
      const float t = static_cast<float>(i) / 120.0f;
      drag1.push_back(timed([&] { c.setNodeDrag(one, {220.0f * std::sin(6.2831853f * t), 140.0f * std::cos(6.2831853f * t)}); }));
    }
    const double solves1 = static_cast<double>(c.portSolves() - s0) / 120.0;
    c.clearNodeDrag();
    settle();
    s0 = c.portSolves();
    for (int i = 0; i < 120; ++i) {
      const float t = static_cast<float>(i) / 120.0f;
      dragAll.push_back(timed([&] { c.setNodeDrag(all, {40.0f * std::sin(6.2831853f * 2.0f * t), 25.0f * std::cos(6.2831853f * 3.0f * t)}); }));
    }
    const double solvesAll = static_cast<double>(c.portSolves() - s0) / 120.0;
    std::printf("orbit: %.1f block solves per move (one block lifted), %.1f (all %zu lifted)\n", solves1, solvesAll, so->nodes.size());
    pq::bench::reportStats("orbit.drag frame (one block lifted: its orbiting ports + its cables' other ends solved, slides, sim, paint) 200 cables 2560x1440",
                           pq::bench::summarize(drag1), "ns CPU/frame");
    pq::bench::reportStats("orbit.swing frame (all 40 blocks lifted: every port solved, slides, sim, paint) 200 cables 2560x1440",
                           pq::bench::summarize(dragAll), "ns CPU/frame");
    c.clearNodeDrag();
    const double t1 = now;
    while ((c.cablesMoving() || c.portsMoving()) && now - t1 < 10.0) frame();
    std::printf("orbit: every port and cable at rest %.2f s after the drag ended\n", now - t1);
    settle();
  }
  // All 200 glow (the sources live): the 30 Hz poll + the repaint of the lit cables.
  live = true;
  c.wakeCables();
  std::vector<double> glow;
  for (int i = 0; i < 120; ++i) {
    const double next = now + 1.0 / 30.0;
    glow.push_back(timed([&] { now = next - 1.0 / 144.0; }));
  }
  std::printf("cables: %u glowing in the last paint\n", c.stats().cablesGlowing);
  pq::bench::reportStats("cables.glow frame (200 cables lit: poll + flow repaint, 30 Hz) 2560x1440", pq::bench::summarize(glow), "ns CPU/frame");
  live = false;
  settle();
  // Idle: no frames, no timers.
  const uint64_t f0 = frames;
  for (int i = 0; i < 288; ++i) frame();
  const bool idle = frames == f0 && !tree.needsFrame() && std::isinf(tree.nextWakeTime());
  std::printf("cables idle: %s (%llu frames in 2 s, glow polling %d, moving %d)\n", idle ? "yes" : "NO",
              static_cast<unsigned long long>(frames - f0), c.cableGlowPolling() ? 1 : 0, c.cablesMoving() ? 1 : 0);
  return idle ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const double nsPerCycle = tscNsPerCycle();
  auto lib = doc::loadDeviceLibrary(std::string(PQ_SOURCE_DIR) + "/stdlib");
  if (!lib) {
    std::printf("device library: %s\n", lib.error().message.c_str());
    return 1;
  }
  // TMM.13-fix2: `pq_bench_project_patch faces` runs only the TMM.4 / TMM.7 face rows (H5's A/B while iterating).
  if (argc > 1 && std::string_view(argv[1]) == "cables") return cablesBench(nsPerCycle);  // TX.cables
  if (argc > 1 && std::string_view(argv[1]) == "faces") {
    const int a = facesBench(**lib, nsPerCycle, false);
    const int b = facesBench(**lib, nsPerCycle, true);
    return a != 0 ? a : b;
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
      pp = buildProjectPatch(*snap, *low, lib->get());
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
  // The whole project, fitted (far level), then one band (near level). (GU.11: the stock devices' panels and their A/B
  // are gone: the modules are groups drawn as their own panels.)
  for (int level = 0; level < 2; ++level) {
    for (int panels = 1; panels >= 1; --panels) {
      if (level == 0) view->fitAll(false);
      else view->focusBand(snap->tracks[0]->children.at(3)->id, false);
      settle();
      fullPaint();
      const circuit::CircuitCanvas& c = view->canvas();
      const char* lod = c.lod() == circuit::CircuitCanvas::Lod::Far ? "far" : (c.lod() == circuit::CircuitCanvas::Lod::Mid ? "mid" : "near");
      const char* what = panels != 0 ? "panels" : "plain blocks";
      std::printf("%s (%s): zoom %.3f (%s): chunks %u from tiles / %u direct, %u nodes painted (%u from tiles), %u panels, %u vertices, %u commands\n",
                  level == 0 ? "whole project" : "one band", what, c.view().zoom, lod, c.stats().chunksFromTiles, c.stats().chunksDirect,
                  c.stats().nodesPainted, c.stats().nodesFromTiles, c.stats().panelsPainted, static_cast<unsigned>(dl.vertices().size()),
                  static_cast<unsigned>(dl.commands().size()));
      std::snprintf(name, sizeof name, "project_patch.paint(full, settled, %s, zoom %.2f %s, %s) 64 tracks %ux%u",
                    level == 0 ? "whole project" : "one band", c.view().zoom, lod, what, w, h);
      pq::bench::reportStats(name, pq::bench::measure(fullPaint, 1), "ns/frame");
      std::vector<double> pan;
      for (int i = 0; i < 300; ++i)
        pan.push_back(timed([&] { view->canvas().panBy((i / 50) % 2 ? -37.0f : 37.0f, (i / 75) % 2 ? -11.0f : 11.0f); }));
      std::snprintf(name, sizeof name, "project_patch.pan frame (%s, %s, %s) 64 tracks %ux%u", level == 0 ? "whole project" : "one band", lod,
                    what, w, h);
      pq::bench::reportStats(name, pq::bench::summarize(pan), "ns CPU/frame");
      if (panels != 0) {
        // A param edit from elsewhere (automation, the Device view; each step of a panel's own knob drag does the same):
        // the document change, the shown panel updated and re-recorded, the frame.
        const doc::Track& t1 = *snap->tracks[0]->children.at(3);  // Track 4: EQ Eight, Utility
        const doc::DeviceId dev = t1.devices.back()->id;
        std::vector<double> edit;
        for (int i = 0; i < 100; ++i)
          edit.push_back(timed([&] {
            auto e = doc::ops::setDeviceParam(*ctx.doc(), dev, "gain", i % 2 == 0 ? 0.5 : 0.75);
            if (e && !e->empty()) (void)ctx.store().dispatch(e->command);
          }));
        std::snprintf(name, sizeof name, "project_patch.param edit frame (a panel shows it; %s, %s) 64 tracks %ux%u",
                      level == 0 ? "whole project" : "one band", lod, w, h);
        pq::bench::reportStats(name, pq::bench::summarize(edit), "ns CPU/frame");
      }
    }
  }
  // TX.node-menu: the node menu over one band — opening it with the full catalog (Tab: the catalog shared, the search,
  // the popup laid out and painted with the canvas under it), a keystroke (search again + repaint), and the push-apart
  // after a spawn onto a block (a re-layout per frame until settled; then idle).
  {
    view->focusBand(snap->tracks[0]->children.at(3)->id, false);
    settle();
    const ui::IRect r = view->canvas().rect();
    const Vec2 at{static_cast<float>(r.x + r.w / 3), static_cast<float>(r.y + r.h / 3)};
    std::vector<double> open, key;
    for (int i = 0; i < 60; ++i) {
      circuit::NodeMenu* m = nullptr;
      open.push_back(timed([&] { m = view->openSpawnMenu(at); }));
      if (m == nullptr) return 1;
      key.push_back(timed([&] { m->setQuery(i % 2 == 0 ? "sat" : "sa"); }));
      tree.setFocus(&view->canvas());
      ui::OverlayLayer::of(view->canvas())->closeAll();
      settle();
    }
    pq::bench::reportStats("node_menu.open frame (Tab: full catalog, search, layout, paint) one band 2560x1440", pq::bench::summarize(open), "ns CPU/frame");
    pq::bench::reportStats("node_menu.keystroke frame (search + repaint)", pq::bench::summarize(key), "ns CPU/frame");
    // Push-apart: two one-atom devices spawned at the same spot of the band: the second lands on the first and the two
    // push each other apart.
    const doc::TrackId t = snap->tracks[0]->children.at(3)->id;
    const ProjectPatch& pp = view->patch();
    const Band& b = pp.bands[pp.bandOf(t)];
    const graph::Point2 spot{b.x0 + 200.0f, b.y + b.h - 30.0f};
    std::vector<double> push;
    uint64_t frames = 0;
    for (int round = 0; round < 5; ++round) {
      const doc::DeviceId a = view->spawnNode(ProjectPatchView::specChoice("tanh"), spot);
      settle();
      const doc::DeviceId c = view->spawnNode(ProjectPatchView::specChoice("tanh"), spot);
      if (!a.valid() || !c.valid()) return 1;
      while (view->pushing()) {
        push.push_back(timed([] {}));
        if (push.size() > 100000) break;
      }
      frames += view->pushFrames();
      settle();
    }
    std::printf("push-apart: %llu frames over 5 spawns (each a re-layout)\n", static_cast<unsigned long long>(frames));
    pq::bench::reportStats("node_menu.push-apart frame (step + re-layout + paint) 64 tracks 2560x1440", pq::bench::summarize(push), "ns CPU/frame");
  }
  // TX.block-drag: a device block dragged by pointer events through the tree (press, moves, release), every move a frame:
  // the block (a panel) drawn at the pointer with its cables on its ports, nothing re-laid out until the drop.
  for (int level = 0; level < 2; ++level) {
    const doc::TrackId t = snap->tracks[0]->children.at(3)->id;
    // Far out the blocks are plain (a panel's controls would take a press there: TMM.4 keeps them interactive).
    if (level == 0) view->focusBand(t, false);
    else view->fitAll(false);
    settle();
    const ProjectPatch& pp = view->patch();
    uint32_t node = kNone;
    for (const uint32_t d : pp.bands[pp.bandOf(t)].devices)
      if (node == kNone && pp.nodes[d].kind == NodeKind::Device) node = d;
    if (node == kNone) return 1;
    const circuit::CircuitCanvas& c = view->canvas();
    const char* lod = c.lod() == circuit::CircuitCanvas::Lod::Far ? "far" : (c.lod() == circuit::CircuitCanvas::Lod::Mid ? "mid" : "near");
    auto pointer = [&](PointerType type, Vec2 at) {
      PointerEvent e;
      e.type = type;
      e.pos = at;
      if (type != PointerType::Move) e.button = PointerButton::Left;
      e.clickCount = 1;
      (void)tree.dispatchPointer(e);
    };
    std::vector<double> move, drop;
    uint32_t lifted = 0;
    const uint64_t solves0 = view->canvas().portSolves();  // TX.orbit-ports: the blocks whose ports a move solves
    const doc::DeviceId dragged = pp.nodes[node].device;
    for (int round = 0; round < 4; ++round) {
      const Node& n = view->patch().nodes[view->patch().nodeOf(dragged)];
      const Vec2 grip = view->canvas().toDevice({n.x + n.w * 0.5f, level == 0 ? n.y + 6.0f : n.y + n.h * 0.5f});  // a panel's title bar
      pointer(PointerType::Move, grip);
      pointer(PointerType::Down, grip);
      frame();
      for (int i = 1; i <= 60; ++i) {
        const float k = static_cast<float>(i) / 60.0f;
        const Vec2 at{grip.x + 160.0f * std::sin(3.1415927f * k), grip.y + (round % 2 == 0 ? 90.0f : -90.0f) * k};
        move.push_back(timed([&] { pointer(PointerType::Move, at); }));
        lifted += view->canvas().draggedNodes().empty() ? 0u : 1u;
      }
      const Vec2 end{grip.x, grip.y + (round % 2 == 0 ? 90.0f : -90.0f)};
      drop.push_back(timed([&] { pointer(PointerType::Up, end); }));
      settle();
    }
    std::printf("block drag (%s, %s): the block lifted in %u of %zu move frames; %.1f orbiting-port solves per move\n",
                level == 0 ? "one band" : "whole project", lod, lifted, move.size(),
                static_cast<double>(view->canvas().portSolves() - solves0) / static_cast<double>(std::max<size_t>(1, move.size())));

    std::snprintf(name, sizeof name, "block_drag.move frame (a %s at the pointer + its cables; %s, %s) 64 tracks %ux%u",
                  level == 0 ? "panel" : "plain block", level == 0 ? "one band" : "whole project", lod, w, h);
    pq::bench::reportStats(name, pq::bench::summarize(move), "ns CPU/frame");
    std::snprintf(name, sizeof name, "block_drag.drop frame (nudge + re-layout + overlap check; %s, %s) 64 tracks %ux%u",
                  level == 0 ? "one band" : "whole project", lod, w, h);
    pq::bench::reportStats(name, pq::bench::summarize(drop), "ns CPU/frame");
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

  // ---- T5.x-one-circuit / TL.1: the band lanes, 64 tracks ----
  {
    view->setShown(true, false);
    settle();
    view->fitAll(false);
    settle();
    // TL.1: a time zoom is the shared window: the lanes and the ruler repaint (the lanes keep their width, no re-layout).
    std::vector<double> rl;
    for (int i = 0; i < 300; ++i) rl.push_back(timed([&] { view->zoomLanes(i % 2 == 0 ? -4.0 : 4.0); }));
    pq::bench::reportStats("one_circuit.lane time zoom frame (lanes + ruler repainted, fitted far) 64 tracks 2560x1440", pq::bench::summarize(rl), "ns CPU/frame");
    std::vector<double> sc;
    for (int i = 0; i < 300; ++i) sc.push_back(timed([&] { view->scrollLanes(i % 2 == 0 ? 1.0 : -1.0); }));
    pq::bench::reportStats("one_circuit.lane scroll frame (windows repainted, fitted far) 64 tracks 2560x1440", pq::bench::summarize(sc), "ns CPU/frame");
    view->focusBand(snap->tracks[0]->children.at(3)->id, false);  // a band framed: its window and its neighbours' at near zoom
    settle();
    fullPaint();
    std::snprintf(name, sizeof name, "one_circuit.paint(full, settled, one band, lane windows with notes) 64 tracks %ux%u", w, h);
    pq::bench::reportStats(name, pq::bench::measure(fullPaint, 1), "ns/frame");
    std::vector<double> ls;
    for (int i = 0; i < 300; ++i) ls.push_back(timed([&] { view->scrollLanes(i % 2 == 0 ? 1.0 : -1.0); }));
    pq::bench::reportStats("one_circuit.lane scroll frame (one band framed, near) 64 tracks 2560x1440", pq::bench::summarize(ls), "ns CPU/frame");
    // TX.lane-edit: the windows drawn by the arrangement's painters with the arrangement's editing state over them — a
    // time selection on every MIDI track (their clips outlined) and automation mode (each window's volume envelope).
    if (auto* arr = dynamic_cast<pq::views::ArrangementView*>((**shell).slot(pq::views::ShellSlot::TrackArea).find("arrangement"))) {
      pq::views::ArrangementView::Selection sel;
      const pq::Ticks bar = pq::ticksPerBar(ctx.doc()->timeline->sigMap.signatureAt(0));
      doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
        if (t.kind != doc::TrackKind::Midi) return;
        sel.tracks.push_back(t.id);
        for (const doc::ClipSlot& c : t.clips)
          if (c.start() < 3 * bar && c.end() > bar) sel.clips.push_back({t.id, c.id});
      });
      sel.time = pq::TickRange{bar, 3 * bar};
      sel.range = true;
      arr->setSelection(sel, false);
      arr->automation().setMode(true);
      settle();
      fullPaint();
      std::snprintf(name, sizeof name, "lane_edit.paint(full, one band, windows with a range + automation mode) 64 tracks %ux%u", w, h);
      pq::bench::reportStats(name, pq::bench::measure(fullPaint, 1), "ns/frame");
      std::vector<double> sf;  // a selection change: the selected tracks' windows damaged, repainted
      for (int i = 0; i < 300; ++i) {
        sel.time = pq::TickRange{bar, (i % 2 == 0 ? 2 : 3) * bar};
        sf.push_back(timed([&] { arr->setSelection(sel, false); }));
      }
      pq::bench::reportStats("lane_edit.selection change frame (64 tracks selected, one band framed) 2560x1440", pq::bench::summarize(sf), "ns CPU/frame");
      arr->automation().setMode(false);
      arr->clearSelection();
    }
    view->setShown(false, false);
    settle();
  }

  // ---- T5.x-circuit-chain: a track's chain as one circuit ----
  {
    doc::DocumentStore& st = ctx.store();
    auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Chain");
    if (!t || !st.dispatch(t->command)) return 1;
    const doc::TrackId track = t->tracks.at(0);
    const char* fx[] = {"module:saturator", "module:vca", "module:utility"};  // GU.11: modules
    auto inst = doc::ops::addDevice(*st.current(), st.ids(), track, "module:operator", doc::DeviceKind::Instrument);
    if (!inst || !st.dispatch(inst->command)) return 1;
    for (const char* f : fx) {
      auto e = doc::ops::addDevice(*st.current(), st.ids(), track, f, doc::DeviceKind::AudioEffect);
      if (!e || !st.dispatch(e->command)) return 1;
    }
    // (GU.9: the T5.x chain circuit went with the stored racks; the project patch view below is the circuit now.)
    // The other 64 tracks go (the shell under the full-window view still paints the arrangement: keep it to one lane).
    {
      std::vector<doc::TrackId> others;
      for (const doc::TrackPtr& o : ctx.doc()->tracks)
        if (o->id != track) others.push_back(o->id);
      for (const doc::TrackPtr& o : ctx.doc()->returns) others.push_back(o->id);
      auto del = doc::ops::deleteTracks(*ctx.doc(), others);
      if (!del || !st.dispatch(del->command)) return 1;
    }
    view->setShown(false, false);
    settle();
    fullPaint();
    std::printf("chain: the shell under the view alone: %u vertices, %u commands\n", static_cast<unsigned>(dl.vertices().size()),
                static_cast<unsigned>(dl.commands().size()));
    pq::bench::reportStats("chain_circuit.baseline: the shell without the view (one track) 2560x1440", pq::bench::measure(fullPaint, 1), "ns/frame");
    // T5.x-one-circuit: Open in Circuit View on the Auto Filter: the project patch view, the band opened, framed on it.
    const doc::DeviceId af = doc::findTrackPtr(*ctx.doc(), track)->devices.at(1)->id;
    if (!ctx.openProjectCircuit(track, af)) return 1;
    tree.layoutNow();
    settle();
    for (int level = 0; level < 2; ++level) {
      if (level == 1) {  // zoomed out around it (Ctrl+wheel steps): the band in context
        const IRect r = view->canvas().rect();
        for (int i = 0; i < 6; ++i) view->canvas().zoomAt({r.x + r.w * 0.5f, r.y + r.h * 0.5f}, 1.0 / 1.15);
      }
      settle();
      fullPaint();
      const circuit::CircuitCanvas& c = view->canvas();
      const char* lod = c.lod() == circuit::CircuitCanvas::Lod::Far ? "far" : (c.lod() == circuit::CircuitCanvas::Lod::Mid ? "mid" : "near");
      std::printf("one circuit %s: zoom %.3f (%s): chunks %u from tiles / %u direct, %u nodes painted (%u from tiles), %u vertices, %u commands\n",
                  level == 0 ? "framed" : "out", c.view().zoom, lod, c.stats().chunksFromTiles, c.stats().chunksDirect, c.stats().nodesPainted,
                  c.stats().nodesFromTiles, static_cast<unsigned>(dl.vertices().size()), static_cast<unsigned>(dl.commands().size()));
      std::snprintf(name, sizeof name, "one_circuit.paint(full, settled, %s, zoom %.2f %s) band Operator>AutoFilter>EQ8>Saturator opened %ux%u",
                    level == 0 ? "framed on Auto Filter" : "zoomed out", c.view().zoom, lod, w, h);
      pq::bench::reportStats(name, pq::bench::measure(fullPaint, 1), "ns/frame");
      std::vector<double> pan;
      for (int i = 0; i < 300; ++i)
        pan.push_back(timed([&] { view->canvas().panBy((i / 50) % 2 ? -37.0f : 37.0f, (i / 75) % 2 ? -11.0f : 11.0f); }));
      std::snprintf(name, sizeof name, "one_circuit.pan frame (%s, %s) band opened %ux%u", level == 0 ? "framed" : "zoomed out", lod, w, h);
      pq::bench::reportStats(name, pq::bench::summarize(pan), "ns CPU/frame");
    }
  }
  const int a = facesBench(**lib, nsPerCycle, false);  // TMM.4
  const int b = facesBench(**lib, nsPerCycle, true);   // TMM.7
  const int c = cablesBench(nsPerCycle);                // TX.cables
  return a | b | c;
}
