// T3.1 widget toolkit benchmarks: layout of a 10,000-widget tree (full and incremental), hit testing, pointer
// and key dispatch, and the paint traversal. Run from the release build; results → bench/RESULTS.md.
// Criteria: full layout of 10k widgets < 1 ms; hit test < 2 µs; dispatch allocation-free (tested in
// tests/ui/test_widgets.cpp).

#include <cstdio>
#include <string>

#include "pq/ui/actions.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/widget.h"
#include "pq_bench.h"

using namespace pq::ui;

namespace {

// A leaf that paints one rect (typical cost of a simple control).
class Cell : public Widget {
public:
  explicit Cell(std::string name) : Widget(std::move(name)) {}
  void paint(PaintContext& ctx) override { ctx.dl.addRectFilled(rect().toRect(), 0xFF303040u); }
  bool onPointer(const PointerEvent& e) override { return e.type == PointerType::Down; }
};

// 100 rows × 100 cells (+ 100 rows + root = 10,101 widgets). Rows mix fixed and flex cells with min/max so
// the flex resolution loop runs; every 10th row has padding and a gap.
void build10k(UiTree& tree) {
  auto root = std::make_unique<Widget>();
  root->setLayout(LayoutKind::Column);
  for (int r = 0; r < 100; ++r) {
    Widget& row = root->add<Widget>("row[" + std::to_string(r) + "]");
    row.setLayout(LayoutKind::Row);
    if (r % 10 == 0) row.setPadding(Insets::all(1)).setGap(1);
    for (int c = 0; c < 100; ++c) {
      Cell& cell = row.add<Cell>("c" + std::to_string(c));
      if (c % 4 == 0) cell.setWidth(Length::fixed(12));
      else if (c % 4 == 1) cell.setWidth(Length::flex(2, 8, 40));
      else cell.setWidth(Length::flex(1));
    }
  }
  tree.setRoot(std::move(root));
}

}  // namespace

int main() {
  UiTree tree;
  tree.setViewport(1920, 1080);
  build10k(tree);
  tree.layoutNow();
  uint64_t widgets = 1;
  for (size_t r = 0; r < tree.root()->childCount(); ++r) widgets += 1 + tree.root()->child(r).childCount();
  std::printf("tree: %llu widgets\n", static_cast<unsigned long long>(widgets));

  // Full layout: every rect changes (alternating window width), the worst case (a window resize).
  uint32_t w = 1920;
  auto s = pq::bench::measure(
      [&] {
        w = w == 1920 ? 1900 : 1920;
        tree.setViewport(w, 1080);
        tree.layoutNow();
        pq::bench::doNotOptimize(tree.root()->child(50).child(50).rect());
      },
      1);
  pq::bench::report("ui/layout 10k widgets full (resize)", s, "ns/layout");
  std::printf("  (%.1f ns/widget)\n", s.medianNs / static_cast<double>(widgets));
  s = pq::bench::measure(
      [&] {
        tree.invalidateAll();
        tree.layoutNow();
        pq::bench::doNotOptimize(tree.root()->child(50).child(50).rect());
      },
      1);
  pq::bench::report("ui/layout 10k widgets full (same size)", s, "ns/layout");

  // Incremental: one cell's width changes → its row is re-arranged (101 widgets).
  float fw = 12;
  Widget& cell = tree.root()->child(37).child(8);
  s = pq::bench::measure(
      [&] {
        fw = fw == 12.0f ? 14.0f : 12.0f;
        cell.setWidth(Length::fixed(fw));
        tree.layoutNow();
        pq::bench::doNotOptimize(cell.rect());
      },
      1);
  pq::bench::report("ui/layout incremental (one cell width)", s, "ns/layout");
  tree.damage().clear();

  // Hit test: deepest widget at pseudo-random points over the whole window.
  uint32_t rng = 12345;
  auto nextPoint = [&] {
    rng = rng * 1664525u + 1013904223u;
    const float x = static_cast<float>((rng >> 8) % 1920u);
    rng = rng * 1664525u + 1013904223u;
    const float y = static_cast<float>((rng >> 8) % 1080u);
    return Vec2{x, y};
  };
  s = pq::bench::measure([&] { pq::bench::doNotOptimize(tree.hitTest(nextPoint())); }, 1);
  pq::bench::report("ui/hitTest 10k-widget tree", s, "ns/hit");

  // Pointer move dispatch (hit test + hover change + bubbling), alternating between two cells.
  PointerEvent mv;
  mv.type = PointerType::Move;
  bool flip = false;
  s = pq::bench::measure(
      [&] {
        flip = !flip;
        mv.pos = flip ? Vec2{500.5f, 300.5f} : Vec2{900.5f, 700.5f};
        pq::bench::doNotOptimize(tree.dispatchPointer(mv));
      },
      1);
  pq::bench::report("ui/dispatch pointer move (hover change)", s, "ns/event");

  // Click: down (focus walk, capture) + up (release, hover update).
  PointerEvent dn, up;
  dn.type = PointerType::Down;
  dn.button = PointerButton::Left;
  dn.clickCount = 1;
  dn.pos = {700.5f, 400.5f};
  up = dn;
  up.type = PointerType::Up;
  s = pq::bench::measure(
      [&] {
        tree.dispatchPointer(dn);
        pq::bench::doNotOptimize(tree.dispatchPointer(up));
      },
      1);
  pq::bench::report("ui/dispatch click (down + up)", s, "ns/click");

  // Key routing through focus chain → scope → global with 200 bindings.
  ActionRegistry actions;
  Keymap keymap;
  int fired = 0;
  for (int i = 0; i < 100; ++i) {
    (void)actions.add("a" + std::to_string(i), [&fired] { ++fired; });
    (void)keymap.bind(kGlobalScope, Chord{static_cast<Key>('a' + i % 26), static_cast<Mods>(i / 26)}, "a" + std::to_string(i));
    (void)keymap.bind(keymap.scope("arrangement"), Chord{static_cast<Key>('a' + i % 26), static_cast<Mods>(4 + i / 26)},
                      "a" + std::to_string(i));
  }
  tree.root()->child(10).setKeyScope(keymap.findScope("arrangement"));
  tree.root()->child(10).child(10).setFocusable(true);
  tree.setFocus(&tree.root()->child(10).child(10));
  tree.setKeymap(&actions, &keymap);
  KeyEvent kd;
  kd.key = 'q';
  kd.mods = mod::kNone;  // misses the arrangement scope, hits global
  s = pq::bench::measure([&] { pq::bench::doNotOptimize(tree.dispatchKey(kd)); }, 1);
  pq::bench::report("ui/dispatch key → global action", s, "ns/key");
  std::printf("  (fired %d)\n", fired > 0 ? 1 : 0);

  // Paint traversal of the whole tree into a draw list (10k rects).
  DrawList dl;
  PaintResources res;
  s = pq::bench::measure(
      [&] {
        dl.reset(1920, 1080);
        tree.damage().addAll();
        tree.paint(dl, res, 0.0);
        pq::bench::doNotOptimize(dl.indices().size());
      },
      1);
  pq::bench::report("ui/paint 10k-widget tree (draw list build)", s, "ns/frame");
  return 0;
}
