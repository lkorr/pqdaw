#include "pq/ui/test_driver.h"

#include <cassert>
#include <cmath>

#include "pq/ui/gpu.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scenes.h"

namespace pq::ui {

struct TestDriver::Gpu {
  std::unique_ptr<GpuContext> ctx;
  std::unique_ptr<Renderer> renderer;
  SceneTextures textures;
};

Scancode scancodeForKey(Key k) noexcept {
  if (k & kKeyScancodeBit) return static_cast<Scancode>(k & ~kKeyScancodeBit);
  if (k >= 'a' && k <= 'z') return static_cast<Scancode>(static_cast<uint32_t>(Scancode::A) + (k - 'a'));
  if (k >= '1' && k <= '9') return static_cast<Scancode>(static_cast<uint32_t>(Scancode::Num1) + (k - '1'));
  switch (k) {
    case '0': return Scancode::Num0;
    case key::kReturn: return Scancode::Return;
    case key::kEscape: return Scancode::Escape;
    case key::kBackspace: return Scancode::Backspace;
    case key::kTab: return Scancode::Tab;
    case key::kSpace: return Scancode::Space;
    case key::kDelete: return Scancode::Delete;
    case '-': return Scancode::Minus;
    case '=': return Scancode::Equals;
    case '`': return Scancode::Grave;
    default: return Scancode::Unknown;
  }
}

TestDriver::TestDriver(uint32_t width, uint32_t height, float scale) {
  tree_.setViewport(width, height, scale);
  tree_.setResources(&res_);  // widgets measure text outside paint (menus)
}

TestDriver::~TestDriver() = default;

void TestDriver::resize(uint32_t width, uint32_t height, float scale) {
  tree_.setViewport(width, height, scale);
  if (gpu_ && !(gpu_->textures.scale == tree_.uiScale())) {
    // Same order as the window loop: font roles re-resolve at the new scale before anything paints.
    [[maybe_unused]] const Status s = setSceneScale(gpu_->textures, tree_.uiScale());
    assert(s.ok() && "TestDriver::resize: font roles failed to resolve at the new scale");
  }
  tree_.layoutNow();
}

bool TestDriver::step() {
  tree_.update(now_);
  if (!tree_.needsPaint()) return false;
  if (res_.text) res_.text->beginFrame();  // frame boundary for the run cache and atlas LRU
  dl_.reset(tree_.width(), tree_.height());
  tree_.paint(dl_, res_, now_);
  ++frames_;
  return true;
}

void TestDriver::advance(double seconds) {
  // Integer tick count so 2 s at 144 Hz is exactly 288 ticks regardless of float accumulation.
  const int64_t ticks = std::llround(seconds / period_);
  const double start = now_;
  for (int64_t i = 1; i <= ticks; ++i) {
    now_ = start + static_cast<double>(i) * period_;
    step();
  }
}

IRect TestDriver::rect(std::string_view path) {
  tree_.layoutNow();
  const Widget* w = tree_.find(path);
  return w ? w->rect() : IRect{};
}

Vec2 TestDriver::center(std::string_view path) {
  const IRect r = rect(path);
  return {static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f, static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f};
}

Widget* TestDriver::widgetAt(Vec2 p) noexcept {
  tree_.layoutNow();
  return tree_.hitTest(p);
}

void TestDriver::dispatch(const PointerEvent& e, bool& handled) {
  tree_.layoutNow();
  handled = tree_.dispatchPointer(e);
  tree_.layoutNow();
}

bool TestDriver::move(Vec2 p, Mods m) {
  PointerEvent e;
  e.type = PointerType::Move;
  e.pos = p;
  e.mods = m;
  bool h = false;
  dispatch(e, h);
  return h;
}

bool TestDriver::down(Vec2 p, PointerButton b, Mods m, uint8_t clicks) {
  PointerEvent e;
  e.type = PointerType::Down;
  e.pos = p;
  e.button = b;
  e.mods = m;
  e.clickCount = clicks;
  bool h = false;
  dispatch(e, h);
  return h;
}

bool TestDriver::up(Vec2 p, PointerButton b, Mods m, uint8_t clicks) {
  PointerEvent e;
  e.type = PointerType::Up;
  e.pos = p;
  e.button = b;
  e.mods = m;
  e.clickCount = clicks;
  bool h = false;
  dispatch(e, h);
  return h;
}

void TestDriver::click(Vec2 p, Mods m, PointerButton b) {
  move(p, m);
  down(p, b, m, 1);
  up(p, b, m, 1);
}

void TestDriver::doubleClick(Vec2 p, Mods m) {
  click(p, m);
  down(p, PointerButton::Left, m, 2);
  up(p, PointerButton::Left, m, 2);
}

void TestDriver::drag(Vec2 from, Vec2 to, int steps, Mods m, PointerButton b) {
  move(from, m);
  down(from, b, m, 1);
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    move({from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t}, m);
  }
  up(to, b, m, 1);
}

bool TestDriver::wheel(Vec2 p, float notchesY, float notchesX, Mods m) {
  PointerEvent e;
  e.type = PointerType::Wheel;
  e.pos = p;
  e.mods = m;
  e.wheel = {notchesX, notchesY};
  bool h = false;
  dispatch(e, h);
  return h;
}

void TestDriver::leave() {
  PointerEvent e;
  e.type = PointerType::Leave;
  bool h = false;
  dispatch(e, h);
}

KeyResult TestDriver::keyDown(Key k, Mods m, bool repeat) {
  KeyEvent e;
  e.down = true;
  e.repeat = repeat;
  e.mods = m;
  e.key = k;
  e.scancode = scancodeForKey(k);
  tree_.layoutNow();
  const KeyResult r = tree_.dispatchKey(e);
  tree_.layoutNow();
  return r;
}

void TestDriver::keyUp(Key k, Mods m) {
  KeyEvent e;
  e.down = false;
  e.mods = m;
  e.key = k;
  e.scancode = scancodeForKey(k);
  (void)tree_.dispatchKey(e);
  tree_.layoutNow();
}

KeyResult TestDriver::press(Chord c, bool repeat) {
  const KeyResult r = keyDown(c.key, c.mods, repeat);
  keyUp(c.key, c.mods);
  return r;
}

KeyResult TestDriver::press(std::string_view chord) {
  auto c = parseChord(chord);
  assert(c.ok() && "TestDriver::press: unparsable chord");
  if (!c) return KeyResult::Unhandled;
  return press(*c);
}

bool TestDriver::type(std::string_view utf8) {
  const bool h = tree_.dispatchText(makeTextEvent(utf8));
  tree_.layoutNow();
  return h;
}

Status TestDriver::enableGpu() {
  if (gpu_) return Ok{};
  auto g = std::make_unique<Gpu>();
  GpuOptions o;
  o.validation = true;  // used when the Khronos validation layer is installed
  auto ctx = GpuContext::create(o);
  if (!ctx) return std::move(ctx).error();
  g->ctx = std::move(*ctx);
  auto r = Renderer::create(*g->ctx, OffscreenTarget::kFormat, 1);
  if (!r) return std::move(r).error();
  g->renderer = std::move(*r);
  auto t = createSceneTextures(*g->renderer, tree_.uiScale());
  if (!t) return std::move(t).error();
  g->textures = std::move(*t);
  gpu_ = std::move(g);
  res_ = paintResources(gpu_->textures);
  tree_.damage().addAll();  // anything painted before had no text
  return Ok{};
}

Result<Image> TestDriver::render() {
  if (Status s = enableGpu(); !s) return std::move(s).error();
  tree_.update(now_);
  if (res_.text) res_.text->beginFrame();
  dl_.reset(tree_.width(), tree_.height());
  tree_.paint(dl_, res_, now_);
  auto target = OffscreenTarget::create(*gpu_->ctx, tree_.width(), tree_.height());
  if (!target) return std::move(target).error();
  if (Status s = (*target)->draw(*gpu_->renderer, dl_, kSceneClearColor); !s) return std::move(s).error();
  auto img = (*target)->read();
  if (img && gpu_->ctx->validationErrorCount() != 0)
    return Error{ErrorCode::Device, std::to_string(gpu_->ctx->validationErrorCount()) + " Vulkan validation error(s)", {}};
  return img;
}

}  // namespace pq::ui
