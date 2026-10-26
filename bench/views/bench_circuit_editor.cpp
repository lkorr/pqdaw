// T4.4 benchmarks: the circuit editor on the largest stdlib patch (by node count), CPU only (text through the CPU atlas,
// tiles through a CPU TileAtlas, as bench_circuit).
//   - one keystroke in the document: PatchEditor::replaceText = re-parse with the previous parse (id diff) + carried and
//     completed layout of the shown patch
//   - the same + the canvas's scene rebuild (what the view does per keystroke)
//   - the check that follows 50 ms later: infer → optimise → schedule
//   - typing latency in the window: one character typed into the text pane of a 1600×1000 editor = text event →
//     document → scene → the next frame's paint (the UI thread's own CPU time, QueryThreadCycleTime)
// Results → bench/RESULTS.md (ship).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#include <windows.h>
#include <intrin.h>

#include "pq/ui/atlas.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/overlay.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/text.h"
#include "pq/ui/tile_atlas.h"
#include "pq/ui/widget.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/circuit/circuit_scene.h"
#include "pq_bench.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views::circuit;
namespace fs = std::filesystem;

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

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

double nowNs() {
  return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int main() {
  const double nsPerCycle = tscNsPerCycle();
  // The largest stdlib patch by node count.
  fs::path bestFile;
  std::string bestPatch;
  uint32_t bestNodes = 0;
  const fs::path stdlib = fs::path(PQ_SOURCE_DIR) / "stdlib";
  for (const auto& e : fs::recursive_directory_iterator(stdlib)) {
    if (!e.is_regular_file() || e.path().extension() != ".pqp") continue;
    auto ed = PatchEditor::fromText(e.path().string(), readFile(e.path()));
    if (!ed) continue;
    for (const graph::Patch& p : (*ed)->parsed().file.patches)
      if (p.nodeCount() > bestNodes) {
        bestNodes = p.nodeCount();
        bestFile = e.path();
        bestPatch = p.name();
      }
  }
  const std::string source = readFile(bestFile);
  std::printf("largest stdlib patch: %s in %s (%u nodes, file %zu bytes)\n", bestPatch.c_str(), bestFile.filename().string().c_str(),
              bestNodes, source.size());
  auto made = PatchEditor::fromText(bestFile.string(), source);
  if (!made) {
    std::printf("cannot open: %s\n", made.error().message.c_str());
    return 1;
  }
  PatchEditor& ed = **made;
  const size_t idx = ed.patchIndex(bestPatch);
  (void)ed.drawn(idx);
  // A keystroke that always parses: a digit typed after the first number of the patch's body, then deleted.
  const size_t begin = ed.parsed().patchSpans[idx].begin;
  size_t at = begin;
  while (at < ed.text().size() && !(ed.text()[at] >= '0' && ed.text()[at] <= '9')) ++at;
  ++at;
  bool typed = false;
  auto key = [&] {
    if (!typed) ed.replaceText(static_cast<uint32_t>(at), 0, "1");
    else ed.replaceText(static_cast<uint32_t>(at), 1, "");
    typed = !typed;
  };
  {
    std::vector<double> t;
    for (int i = 0; i < 400; ++i) {
      const double t0 = nowNs();
      key();
      t.push_back(nowNs() - t0);
    }
    std::printf("  id match: %u of %u nodes kept their ids\n", ed.stats().lastIdMatch.matched, ed.stats().lastIdMatch.nodes);
    pq::bench::reportStats("circuit.editor keystroke: re-parse + id diff + layout (largest stdlib patch)", pq::bench::summarize(t),
                           "ns/key");
  }
  {
    std::vector<double> t;
    for (int i = 0; i < 400; ++i) {
      const double t0 = nowNs();
      key();
      auto sc = buildScene(ed.drawn(idx));
      pq::bench::doNotOptimize(sc);
      t.push_back(nowNs() - t0);
    }
    pq::bench::reportStats("circuit.editor keystroke + scene rebuild (largest stdlib patch)", pq::bench::summarize(t), "ns/key");
  }
  {
    std::vector<double> t;
    for (int i = 0; i < 200; ++i) {
      key();
      const double t0 = nowNs();
      const Status s = ed.check(idx);
      t.push_back(nowNs() - t0);
      if (!s) {
        std::printf("check failed: %s\n", s.error().message.c_str());
        return 1;
      }
    }
    pq::bench::reportStats("circuit.editor check: infer + optimise + schedule (largest stdlib patch)", pq::bench::summarize(t),
                           "ns/check");
  }

  // Typing latency in the window.
  auto tex = createSceneText(std::make_shared<Atlas>(), UiScale(1.0f));
  if (!tex) {
    std::printf("scene text: %s\n", tex.error().message.c_str());
    return 1;
  }
  auto tiles = std::make_shared<TileAtlas>();
  PaintResources res = paintResources(*tex);
  res.tiles = tiles.get();
  const uint32_t w = 1600, h = 1000;
  UiTree tree;
  tree.setViewport(w, h, UiScale(1.0f));
  tree.setResources(&res);
  auto& root = tree.emplaceRoot<Widget>("root");
  root.setLayout(LayoutKind::Stack);
  (void)root.add<OverlayLayer>("overlay");
  tree.layoutNow();
  auto doc = PatchEditor::fromText(bestFile.string(), source);
  CircuitEditor* editor = openCircuitEditor(root, std::move(*doc), bestPatch);
  if (!editor) return 1;
  DrawList dl;
  double now = 1.0;
  auto frame = [&] {
    now += 1.0 / 144.0;
    tree.update(now);
    tex->text->beginFrame();
    dl.reset(w, h);
    if (tree.needsPaint()) tree.paint(dl, res, now);
    (void)tiles->uploadBytes();
    tiles->clearUploads();
    tiles->beginFrame();
  };
  auto settle = [&] {
    for (int i = 0; i < 2000 && (tree.needsFrame() || tree.nextWakeTime() <= now + 1.0); ++i) {
      now = std::max(now, std::min(tree.nextWakeTime(), now + 1.0)) - 1.0 / 144.0;
      frame();
    }
  };
  settle();
  CodeEditor& pane = editor->textPane();
  tree.setFocus(&pane);
  pane.setCaret(static_cast<uint32_t>(at));
  settle();
  std::vector<double> lat, checkFrames;
  bool on = false;
  for (int i = 0; i < 200; ++i) {
    const uint64_t c0 = threadCycles();
    if (!on) (void)tree.dispatchText(makeTextEvent("1"));
    else (void)tree.dispatchKey(KeyEvent{true, false, mod::kNone, Scancode::Backspace, key::kBackspace});
    on = !on;
    frame();
    lat.push_back(static_cast<double>(threadCycles() - c0) * nsPerCycle);
    // The debounced check (≈ 50 ms later) and its repaint, measured apart.
    const uint64_t c1 = threadCycles();
    settle();
    checkFrames.push_back(static_cast<double>(threadCycles() - c1) * nsPerCycle);
  }
  pq::bench::reportStats("circuit.editor typing latency: key -> document -> scene -> frame, 1600x1000 (largest stdlib patch)",
                         pq::bench::summarize(lat), "ns CPU/key");
  pq::bench::reportStats("circuit.editor after a key: debounced check + repaint, 1600x1000 (largest stdlib patch)",
                         pq::bench::summarize(checkFrames), "ns CPU/key");
  std::printf("  idle after the check: needsFrame %d, next wake %s\n", tree.needsFrame() ? 1 : 0,
              tree.nextWakeTime() > now + 1e6 ? "never" : "pending");
  return 0;
}
