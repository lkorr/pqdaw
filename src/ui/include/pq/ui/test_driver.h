#pragma once

// ui::TestDriver (T3.1): runs a widget tree headless for scripted tests. It owns a UiTree at a given size,
// injects synthetic input (pointer, keys, text), drives a fake clock through the same update → paint cycle as
// the window loop, and renders through OffscreenTarget for golden images.
//
//   TestDriver d(400, 300);
//   auto& root = d.setRoot<Widget>();
//   ...build...
//   d.click(d.center("toolbar/play"));
//   d.press("Ctrl+D");
//   d.advance(2.0);                      // 2 s of fake time at 144 Hz; frames are drawn only when damaged
//   CHECK(d.framesDrawn() == 1);
//
// Event helpers lay the tree out first (so queries and hit tests see current rects) but do not paint; paint
// happens in step()/advance(), like a real frame loop. Text needs the GPU (glyphs live in the renderer's
// atlas): call enableGpu() before painting text, or let render() do it.

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include "pq/core/result.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/image.h"
#include "pq/ui/widget.h"

namespace pq::ui {

class TestDriver {
public:
  TestDriver(uint32_t width, uint32_t height, float scale = 1.0f);
  ~TestDriver();
  TestDriver(const TestDriver&) = delete;
  TestDriver& operator=(const TestDriver&) = delete;

  UiTree& tree() noexcept { return tree_; }
  Widget* root() const noexcept { return tree_.root(); }
  template <class W, class... Args>
  W& setRoot(Args&&... args) {
    W& w = tree_.emplaceRoot<W>(std::forward<Args>(args)...);
    tree_.layoutNow();
    return w;
  }
  void resize(uint32_t width, uint32_t height, float scale = 1.0f);

  // --- fake clock and frames ---
  double now() const noexcept { return now_; }
  void setFrameRate(double hz) noexcept { period_ = 1.0 / hz; }
  // One loop iteration at the current time: timers, frame requests, layout; paints if anything is damaged.
  // Returns whether a frame was drawn.
  bool step();
  // Advances the clock by `seconds` in frame periods, calling step() at every tick.
  void advance(double seconds);
  uint64_t framesDrawn() const noexcept { return frames_; }
  const DrawList& drawList() const noexcept { return dl_; }

  // --- queries (run pending layout first) ---
  Widget* find(std::string_view path) const noexcept { return tree_.find(path); }
  IRect rect(std::string_view path);  // {} when absent
  Vec2 center(std::string_view path);
  Widget* widgetAt(Vec2 p) noexcept;

  // --- pointer (device px) ---
  bool move(Vec2 p, Mods m = mod::kNone);
  bool down(Vec2 p, PointerButton b = PointerButton::Left, Mods m = mod::kNone, uint8_t clicks = 1);
  bool up(Vec2 p, PointerButton b = PointerButton::Left, Mods m = mod::kNone, uint8_t clicks = 1);
  void click(Vec2 p, Mods m = mod::kNone, PointerButton b = PointerButton::Left);
  void doubleClick(Vec2 p, Mods m = mod::kNone);  // click (count 1), then down/up with count 2
  // move to `from`, press, `steps` moves to `to`, release at `to`.
  void drag(Vec2 from, Vec2 to, int steps = 4, Mods m = mod::kNone, PointerButton b = PointerButton::Left);
  bool wheel(Vec2 p, float notchesY, float notchesX = 0.0f, Mods m = mod::kNone);
  void leave();

  // --- keyboard ---
  KeyResult keyDown(Key k, Mods m = mod::kNone, bool repeat = false);
  void keyUp(Key k, Mods m = mod::kNone);
  // Down + up; returns the down result.
  KeyResult press(Chord c, bool repeat = false);
  KeyResult press(std::string_view chord);  // parseChord; an unparsable chord is a test bug (asserts)
  bool type(std::string_view utf8);         // one TextEvent

  // --- rendering ---
  // GPU context + renderer + the scene fonts/textures (pq/ui/scenes.h). Idempotent.
  Status enableGpu();
  const PaintResources& resources() const noexcept { return res_; }
  // Paints the tree and reads it back through OffscreenTarget (creates the GPU on first use).
  Result<Image> render();

private:
  void dispatch(const PointerEvent& e, bool& handled);
  struct Gpu;
  std::unique_ptr<Gpu> gpu_;
  UiTree tree_;
  DrawList dl_;
  PaintResources res_;
  double now_ = 0.0;
  double period_ = 1.0 / 144.0;
  uint64_t frames_ = 0;
};

// The scancode of a key on a US layout, for synthetic events (Unknown when there is no obvious one).
Scancode scancodeForKey(Key k) noexcept;

}  // namespace pq::ui
