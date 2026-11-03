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
#include "pq/views/circuit/chain_circuit.h"
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
  view->setPanelsShown(false);  // TX.panel-faces: the faces' A/B alone (the panels have their own rows above)
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

}  // namespace

int main(int argc, char** argv) {
  const double nsPerCycle = tscNsPerCycle();
  auto lib = doc::loadDeviceLibrary(std::string(PQ_SOURCE_DIR) + "/stdlib");
  if (!lib) {
    std::printf("device library: %s\n", lib.error().message.c_str());
    return 1;
  }
  // TMM.13-fix2: `pq_bench_project_patch faces` runs only the TMM.4 / TMM.7 face rows (H5's A/B while iterating).
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
  // TX.panel-faces: the first show with panels (the panel faces made: every closed stock device's DevicePanel).
  {
    view->setPanelsShown(false);
    settle();
    const auto t0 = std::chrono::steady_clock::now();
    view->setPanelsShown(true);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("panels: %zu panel faces made in %.2f ms (rebuild %.2f ms of it)\n", view->canvas().panelCount(), ms, view->stats().lastRebuildMs);
    settle();
  }
  // The whole project, fitted (far level), then one band (near level). TX.panel-faces: with the stock devices drawn as
  // their panels (the default) and as plain blocks (setPanelsShown(false): the A/B of the panels' cost).
  for (int level = 0; level < 2; ++level) {
    for (int panels = 1; panels >= 0; --panels) {
      view->setPanelsShown(panels != 0);
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
  view->setPanelsShown(true);
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

  // ---- T5.x-one-circuit: lane windows, 64 tracks ----
  {
    view->setShown(true, false);
    settle();
    view->fitAll(false);
    settle();
    std::vector<double> rl;
    for (int i = 0; i < 40; ++i) {
      view->zoomLanes(i % 2 == 0 ? -4.0 : 4.0);  // 4 bars <-> ~10 bars: the windows' width changes every time
      rl.push_back(view->stats().lastRelayoutMs * 1e6);
    }
    pq::bench::reportStats("one_circuit.lane time zoom re-layout (model + scene + canvas, no lowering) 64 tracks", pq::bench::summarize(rl), "ns/relayout");
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
    const char* fx[] = {"auto_filter", "eq_eight", "saturator"};
    auto inst = doc::ops::addDevice(*st.current(), st.ids(), track, "operator", doc::DeviceKind::Instrument);
    if (!inst || !st.dispatch(inst->command)) return 1;
    for (const char* f : fx) {
      auto e = doc::ops::addDevice(*st.current(), st.ids(), track, f, doc::DeviceKind::AudioEffect);
      if (!e || !st.dispatch(e->command)) return 1;
    }
    std::vector<double> cold, warm;
    circuit::ChainSceneCache cache;
    circuit::ChainOptions co;
    co.devices = lib->get();
    size_t nodes = 0;
    for (int i = 0; i < 10; ++i) {
      circuit::ChainOptions c0 = co;  // no cache: every device's scene built
      c0.cache = nullptr;
      const auto a = std::chrono::steady_clock::now();
      const circuit::ChainCircuit c = circuit::buildTrackCircuit(*ctx.doc(), track, c0);
      const auto b = std::chrono::steady_clock::now();
      co.cache = &cache;
      const circuit::ChainCircuit d = circuit::buildTrackCircuit(*ctx.doc(), track, co);
      const auto e = std::chrono::steady_clock::now();
      nodes = d.scene->nodes.size();
      cold.push_back(std::chrono::duration<double, std::nano>(b - a).count());
      if (i > 0) warm.push_back(std::chrono::duration<double, std::nano>(e - b).count());
    }
    std::printf("chain: %zu scene nodes\n", nodes);
    {
      std::vector<double> ix;
      const circuit::ChainCircuit d = circuit::buildTrackCircuit(*ctx.doc(), track, co);
      for (int i = 0; i < 10; ++i) {
        circuit::CircuitScene copy = *d.scene;
        const float inc[4] = {d.x0, d.y0, d.x1, d.y1};
        const auto a = std::chrono::steady_clock::now();
        circuit::indexScene(copy, inc);
        ix.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - a).count());
      }
      pq::bench::reportStats("chain_circuit.indexScene (junctions, bounds, grid) of the composed scene", pq::bench::summarize(ix), "ns/index");
    }
    pq::bench::reportStats("chain_circuit.build cold (device scenes + layout + scene) Operator>AutoFilter>EQ8>Saturator", pq::bench::summarize(cold), "ns/build");
    pq::bench::reportStats("chain_circuit.build from cached device scenes (a structure change) Operator>AutoFilter>EQ8>Saturator",
                           pq::bench::summarize(warm), "ns/build");
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
  return a | b;
}
