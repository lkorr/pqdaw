// MX.1: the radial ("bubble") menu, through real event dispatch (TestDriver → UiTree → OverlayLayer → RadialMenu):
// hover-open, click-pick, type-pick, keyboard in / out, Esc, a press outside, right-click, the menu closing itself and
// the opener rebuilt (and another menu opened) inside the pick handler with poisoned frees, edges, the open animation,
// no allocation per frame while open; goldens closed / level 1 / level 2 / filtered at 1× and 2×.

#include <doctest/doctest.h>

#include <cmath>
#include <ostream>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/radial_menu.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"

#include "golden_check.h"

using namespace pq::ui;

namespace {

std::vector<RadialItem> mathItems() {
  using R = RadialItem;
  return {
      R::category("Compare", {R::leaf("lt", "less than", "<"), R::leaf("gt", "greater than", ">"),
                              R::leaf("le", "less or equal", "\xE2\x89\xA4", "<="), R::leaf("ge", "greater or equal", "\xE2\x89\xA5", ">="),
                              R::leaf("eq", "equal", "=="), R::leaf("ne", "not equal", "\xE2\x89\xA0", "!=")}),
      R::category("Logic", {R::leaf("and", "and", "and"), R::leaf("or", "or", "or"), R::leaf("xor", "xor", "xor"),
                            R::leaf("nand", "nand", "nand"), R::leaf("nor", "nor", "nor"), R::leaf("xnor", "xnor", "xnor"),
                            R::leaf("not", "not A", "not")}),
      R::category("Arithmetic", {R::leaf("add", "add", "+"), R::leaf("sub", "subtract", "\xE2\x88\x92", "-"),
                                 R::leaf("mul", "multiply", "\xC3\x97", "*"), R::leaf("div", "divide", "\xC3\xB7", "/"),
                                 R::leaf("mod", "modulo", "mod", "%"), R::leaf("pow", "power", "pow", "^"),
                                 R::leaf("avg", "average", "avg", "mean"), R::leaf("absdiff", "difference", "|A\xE2\x88\x92" "B|")}),
      R::category("Range", {R::leaf("min", "minimum", "min"), R::leaf("max", "maximum", "max"), R::leaf("clip", "clip", "clip"),
                            R::leaf("wrap", "wrap", "wrap"), R::leaf("fold", "fold", "fold")}),
      R::category("Geometry", {R::leaf("atan2", "arctangent", "atan2"), R::leaf("hypot", "hypotenuse", "hypot")}),
  };
}

// The face stand-in: a round symbol that opens the menu on a press (as the Math face will), from its own onPointer.
class Opener : public Widget {
public:
  explicit Opener(std::string name) : Widget(std::move(name)) {
    setSize(Length::fixed(44), Length::fixed(44));
    setFocusable(true);
  }
  RadialMenuOptions options;
  std::string current;
  RadialMenu* menu = nullptr;
  int presses = 0;
  bool onPointer(const PointerEvent& e) override {
    if (e.type != PointerType::Down || e.button != PointerButton::Left) return false;
    ++presses;
    RadialMenuOptions o = options;
    o.current = current;
    menu = openRadialMenu(*this, rect(), mathItems(), std::move(o));
    return true;
  }
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th) return;
    const IRect r = rect();
    const Vec2 c{static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)};
    ctx.dl.addCircleFilled(c, 0.5f * static_cast<float>(r.w), th->surface.raised);
    ctx.dl.addCircleStroke(c, 0.5f * static_cast<float>(r.w), th->menu.border, ctx.scale.exact(1.0f));
    if (ctx.res.text && ctx.res.uiFonts)
      ctx.res.text->draw(ctx.dl, ctx.res.uiFonts->title, {c.x - ctx.scale.exact(4.0f), c.y + ctx.scale.exact(4.0f)}, ">",
                         th->text.primary);
  }
};

// The content under the menu: places the opener at `at` (logical px) and counts the presses that reach it.
class Probe : public Widget {
public:
  using Widget::Widget;
  int presses = 0;
  Vec2 at{0, 0};
  void arrange(LayoutPass& pass) override {
    for (size_t i = 0; i < childCount(); ++i)
      pass.place(child(i), {rect().x + pass.px(at.x), rect().y + pass.px(at.y), pass.px(44), pass.px(44)});
  }
  bool onPointer(const PointerEvent& e) override {
    if (e.type == PointerType::Down) ++presses;
    return e.type == PointerType::Down;
  }
  void paint(PaintContext& ctx) override {
    if (ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), ctx.res.theme->surface.panel);
  }
};

struct Ui {
  TestDriver d;
  Widget* root = nullptr;
  Probe* content = nullptr;
  Opener* opener = nullptr;
  OverlayLayer* overlay = nullptr;
  std::vector<std::string> picks;
  int closes = 0;

  // `at`: where the opener sits (logical px).
  explicit Ui(uint32_t w = 520, uint32_t h = 420, float scale = 1.0f, Vec2 at = {238, 188}, bool gpu = false) : d(w, h, scale) {
    if (gpu) REQUIRE(d.enableGpu());
    root = &d.setRoot<Widget>();
    content = &root->add<Probe>("content");
    content->setLayout(LayoutKind::Custom);
    overlay = &root->add<OverlayLayer>("overlay");
    rebuildOpener(at);
  }
  void rebuildOpener(Vec2 at) {
    content->clearChildren();
    opener = &content->add<Opener>("opener");
    opener->options.animate = false;
    opener->options.onPick = [this](std::string_view k) { picks.emplace_back(k); };
    opener->options.onClosed = [this] { ++closes; };
    content->at = at;
    content->invalidateArrange();
  }
  RadialMenu* menu() const { return dynamic_cast<RadialMenu*>(overlay->top()); }
  Vec2 openerCentre() { return d.center("content/opener"); }
  RadialMenu& open(std::string current = {}) {
    opener->current = std::move(current);
    d.click(openerCentre());
    RadialMenu* m = menu();
    REQUIRE(m != nullptr);
    d.step();
    return *m;
  }
  Vec2 bubble(std::string_view keyOrLabel) {
    RadialMenu* m = menu();
    REQUIRE(m != nullptr);
    const int32_t i = m->find(keyOrLabel);
    REQUIRE_MESSAGE(i >= 0, keyOrLabel);
    REQUIRE_MESSAGE(m->shown(i), keyOrLabel);
    return m->centreOf(i);
  }
  void frame() {
    d.step();
    (void)d.tree().cursor();  // the app's loop: update, then applyCursor(ui->cursor())
  }
};

}  // namespace

TEST_SUITE("radial menu") {

TEST_CASE("radial menu: opens on the clicked symbol with the categories in a ring; idle draws nothing") {
  Ui u;
  RadialMenu& m = u.open();
  CHECK(u.overlay->openCount() == 1);
  CHECK(u.d.tree().focus() == &m);
  CHECK(m.openDepth() == 0);
  CHECK(m.highlighted() == -1);
  const Vec2 c = u.openerCentre();
  CHECK(m.centre().x == doctest::Approx(c.x));
  CHECK(m.centre().y == doctest::Approx(c.y));
  // Five categories around the centre, the first at the top, all at one distance.
  float R = -1.0f;
  for (const char* cat : {"Compare", "Logic", "Arithmetic", "Range", "Geometry"}) {
    const Vec2 p = u.bubble(cat);
    const float d = std::hypot(p.x - c.x, p.y - c.y);
    if (R < 0.0f) R = d;
    CHECK(d == doctest::Approx(R).epsilon(0.001));
  }
  CHECK(u.bubble("Compare").y < c.y - R * 0.99f);
  CHECK_FALSE(m.shown(m.find("gt")));
  // Idle: nothing runs.
  u.d.advance(0.3);
  const uint64_t frames = u.d.framesDrawn();
  u.d.advance(1.0);
  CHECK(u.d.framesDrawn() == frames);
}

TEST_CASE("radial menu: hovering a category opens its ring outward from it; it stays open over the gap") {
  Ui u;
  RadialMenu& m = u.open();
  const int32_t compare = m.find("Compare"), range = m.find("Range");
  u.d.move(u.bubble("Compare"));
  CHECK(m.openDepth() == 1);
  CHECK(m.openItem(0) == compare);
  CHECK(m.highlighted() == compare);
  // Its children: an arc outside ring 0, centred on the category's direction.
  const Vec2 c = m.centre(), cat = u.bubble("Compare");
  const float rCat = std::hypot(cat.x - c.x, cat.y - c.y);
  for (const char* k : {"lt", "gt", "le", "ge", "eq", "ne"}) {
    const Vec2 p = u.bubble(k);
    CHECK(std::hypot(p.x - c.x, p.y - c.y) > rCat + m.radiusOf(compare));
  }
  CHECK(u.bubble("lt").x < cat.x);  // clockwise from the left of the top
  CHECK(u.bubble("ne").x > cat.x);
  // Over a leaf: highlighted, the category stays open and on the path.
  u.d.move(u.bubble("gt"));
  CHECK(m.highlighted() == m.find("gt"));
  CHECK(m.openDepth() == 1);
  CHECK(m.openItem(0) == compare);
  CHECK(m.focusRing() == 1);
  // Between bubbles nothing changes.
  u.d.move({c.x, c.y});
  CHECK(m.highlighted() == m.find("gt"));
  CHECK(m.openDepth() == 1);
  // Another category switches the ring.
  u.d.move(u.bubble("Range"));
  CHECK(m.openItem(0) == range);
  CHECK(m.shown(m.find("max")));
  CHECK_FALSE(m.shown(m.find("gt")));
  CHECK(u.picks.empty());
}

TEST_CASE("radial menu: a click on a leaf picks it and closes the menu; a click on a category opens it") {
  Ui u;
  RadialMenu& m = u.open();
  // Clicking (not hovering first) a category opens it: press on it directly.
  const Vec2 geo = u.bubble("Geometry");
  u.d.down(geo);
  u.d.up(geo);
  CHECK(m.openItem(0) == m.find("Geometry"));
  CHECK(u.overlay->openCount() == 1);
  u.d.click(u.bubble("hypot"));
  CHECK(u.picks == std::vector<std::string>{"hypot"});
  CHECK(u.closes == 1);
  CHECK(u.overlay->openCount() == 0);
  CHECK(u.d.tree().captured() == nullptr);
  CHECK(u.d.tree().focus() == u.opener);  // focus goes back to the opener
  u.frame();
  u.frame();
  // A press on one leaf released over another picks nothing.
  RadialMenu& m2 = u.open();
  u.d.move(u.bubble("Compare"));
  u.d.down(u.bubble("lt"));
  u.d.up(u.bubble("gt"));
  CHECK(u.picks.size() == 1);
  CHECK(u.overlay->isOpen(m2));
}

TEST_CASE("radial menu: typing filters across every category; Enter picks the best, Backspace edits") {
  Ui u;
  RadialMenu& m = u.open();
  u.d.type("hyp");
  CHECK(m.filter() == "hyp");
  REQUIRE(m.resultCount() == 1);
  CHECK(m.keyOf(m.result(0)) == "hypot");
  CHECK(m.highlighted() == m.find("hypot"));
  CHECK(m.shown(m.find("hypot")));
  // ">" ranks the exact glyph first, then ">=" (a keyword prefix).
  u.d.press("Backspace");
  u.d.press("Backspace");
  u.d.press("Backspace");
  CHECK(m.filter().empty());
  CHECK(m.resultCount() == 0);
  u.d.type(">");
  REQUIRE(m.resultCount() >= 2);
  CHECK(m.keyOf(m.result(0)) == "gt");
  CHECK(m.keyOf(m.result(1)) == "ge");
  // Matches from several categories: "m" → mul, mod, min, max, mean (avg), …
  u.d.press("Backspace");
  u.d.type("m");
  CHECK(m.resultCount() >= 4);
  bool range = false, arith = false;
  for (int32_t i = 0; i < m.resultCount(); ++i) {
    const std::string& k = m.keyOf(m.result(i));
    range = range || k == "min" || k == "max";
    arith = arith || k == "mod" || k == "mul";
  }
  CHECK(range);
  CHECK(arith);
  u.d.type("ax");
  REQUIRE(m.resultCount() >= 1);
  CHECK(m.keyOf(m.result(0)) == "max");
  CHECK(u.d.press("Return") == KeyResult::Widget);
  CHECK(u.picks == std::vector<std::string>{"max"});
  CHECK(u.overlay->openCount() == 0);
  u.frame();
  // Down moves through the results; Esc clears the text first, then closes.
  RadialMenu& m2 = u.open();
  u.d.type(">");
  u.d.press("Down");
  CHECK(m2.highlighted() == m2.result(1));
  u.d.press("Escape");
  CHECK(m2.filter().empty());
  CHECK(u.overlay->isOpen(m2));
  u.d.press("Escape");
  CHECK(u.overlay->openCount() == 0);
  CHECK(u.picks.size() == 1);
}

TEST_CASE("radial menu: keys move around a ring, Right / Enter go in, Left / Esc go out; the current item is marked") {
  Ui u;
  RadialMenu& m = u.open("ge");
  // Opened to the current item.
  CHECK(m.openDepth() == 1);
  CHECK(m.openItem(0) == m.find("Compare"));
  CHECK(m.focusRing() == 1);
  CHECK(m.highlighted() == m.find("ge"));
  u.d.press("Down");
  CHECK(m.highlighted() == m.find("eq"));
  u.d.press("Up");
  u.d.press("Up");
  CHECK(m.highlighted() == m.find("le"));
  // Esc goes back one level (the category, its ring still shown), then closes.
  u.d.press("Escape");
  CHECK(u.overlay->openCount() == 1);
  CHECK(m.focusRing() == 0);
  CHECK(m.highlighted() == m.find("Compare"));
  // Around ring 0: Down opens the next category's ring.
  u.d.press("Down");
  CHECK(m.highlighted() == m.find("Logic"));
  CHECK(m.openItem(0) == m.find("Logic"));
  u.d.press("Up");
  u.d.press("Up");
  CHECK(m.highlighted() == m.find("Geometry"));  // wraps
  u.d.press("Right");
  CHECK(m.focusRing() == 1);
  CHECK(m.highlighted() == m.find("atan2"));
  u.d.press("Left");
  CHECK(m.focusRing() == 0);
  u.d.press("Left");  // nothing on ring 0
  CHECK(u.overlay->openCount() == 1);
  // Right into Compare lands on the current item.
  u.d.press("Down");
  CHECK(m.highlighted() == m.find("Compare"));
  u.d.press("Right");
  CHECK(m.highlighted() == m.find("ge"));
  u.d.press("Down");
  u.d.press("Return");
  CHECK(u.picks == std::vector<std::string>{"eq"});
  CHECK(u.overlay->openCount() == 0);
  u.frame();
  RadialMenu& m2 = u.open();
  (void)m2;
  u.d.press("Escape");
  CHECK(u.overlay->openCount() == 0);
  CHECK(u.picks.size() == 1);
  CHECK(u.closes == 2);
}

TEST_CASE("radial menu: a press outside every bubble or a right-click closes it, and is consumed") {
  Ui u;
  u.open();
  const int before = u.content->presses;
  // Inside the menu's bounds but on no bubble: the centre disc (the symbol it was opened from).
  RadialMenu* m = u.menu();
  CHECK(m->bounds().contains(m->centre()));
  u.d.click(m->centre());
  CHECK(u.overlay->openCount() == 0);
  CHECK(u.closes == 1);
  CHECK(u.content->presses == before);
  CHECK(u.d.tree().captured() == nullptr);
  u.frame();
  // Far outside.
  u.open();
  u.d.click({5.0f, 5.0f});
  CHECK(u.overlay->openCount() == 0);
  CHECK(u.content->presses == before);
  u.frame();
  // A right-click, even on a bubble.
  u.open();
  u.d.click(u.bubble("Logic"), pq::ui::mod::kNone, PointerButton::Right);
  CHECK(u.overlay->openCount() == 0);
  CHECK(u.picks.empty());
  CHECK(u.closes == 3);
  CHECK(u.d.tree().captured() == nullptr);
}

// TX.node-menu-fix1 taught this: the menu closes inside its own handler and the handler rebuilds the scene. Freed
// memory is poisoned; every step is followed by the app's own sequence (update, then cursor()).
TEST_CASE("radial menu: the pick handler destroys the opener, rebuilds the scene and opens another menu (poisoned frees)") {
  Ui u;
  REQUIRE(pq::alloc_trap::installed());
  int rebuilt = 0;
  auto arm = [&] {
    u.opener->options.onPick = [&](std::string_view k) {
      u.picks.emplace_back(k);
      ++rebuilt;
      u.rebuildOpener({100, 100});  // the opener (whose options hold this very handler's copy) is destroyed here
      if (rebuilt == 1) u.opener->menu = openRadialMenu(*u.opener, u.opener->rect(), mathItems(), {});
    };
    u.opener->options.onClosed = [&] {
      ++u.closes;
      u.overlay->closeAll();  // re-entrant: closing everything from the close callback
    };
  };
  arm();
  u.open();
  u.d.move(u.bubble("Geometry"));
  u.frame();
  const Vec2 p = u.bubble("atan2");
  u.d.move(p);
  u.frame();
  {
    pq::alloc_trap::PoisonFrees poison;
    u.d.down(p);
    u.frame();
    u.d.up(p);  // picks: the menu closes, the opener is destroyed and rebuilt, a new menu opens
    CHECK(u.d.tree().captured() == nullptr);
    u.frame();  // the graveyard empties: the old menu is freed (poisoned)
    u.frame();
    u.d.move({p.x + 3.0f, p.y + 2.0f});
    u.frame();
    u.d.advance(0.3);
    (void)u.d.tree().cursor();
  }
  CHECK(u.picks == std::vector<std::string>{"atan2"});
  CHECK(rebuilt == 1);
  RadialMenu* second = u.menu();
  REQUIRE(second != nullptr);  // the menu opened by the handler
  // It works: a pick through it, its handler-less options (no onPick) are fine.
  u.d.move(u.bubble("Compare"));
  const Vec2 q = u.bubble("lt");
  {
    pq::alloc_trap::PoisonFrees poison;
    u.d.click(q);
    u.frame();
    u.frame();
    (void)u.d.tree().cursor();
  }
  CHECK(u.overlay->openCount() == 0);
  // A menu closed by a press outside inside its own Down handler is never captured.
  arm();
  u.open();
  {
    pq::alloc_trap::PoisonFrees poison;
    u.d.down({4.0f, 4.0f});
    CHECK(u.d.tree().captured() == nullptr);
    u.frame();
    u.frame();
    u.d.up({4.0f, 4.0f});
    u.frame();
  }
  CHECK(u.overlay->openCount() == 0);
}

TEST_CASE("radial menu: rings stay inside the window near its edges") {
  for (const Vec2 at : {Vec2{2, 2}, Vec2{474, 2}, Vec2{2, 374}, Vec2{474, 374}}) {
    Ui u(520, 420, 1.0f, at);
    RadialMenu& m = u.open();
    const IRect vp{0, 0, 520, 420};
    auto inside = [&](int32_t i) {
      const Vec2 c = m.centreOf(i);
      const float r = m.radiusOf(i);
      return c.x - r >= 0.0f && c.y - r >= 0.0f && c.x + r <= static_cast<float>(vp.w) && c.y + r <= static_cast<float>(vp.h);
    };
    for (const char* cat : {"Compare", "Logic", "Arithmetic", "Range", "Geometry"}) {
      CHECK_MESSAGE(inside(m.find(cat)), cat);
      u.d.move(u.bubble(cat));
      const int32_t ci = m.find(cat);
      for (int32_t k = 0; k < m.itemCount(); ++k)
        if (m.shown(k) && k != ci && m.labelOf(k) != cat) CHECK_MESSAGE(inside(k), m.labelOf(k));
    }
    u.d.type("a");
    for (int32_t i = 0; i < m.resultCount(); ++i) CHECK(inside(m.result(i)));
    const IRect b = m.bounds();
    CHECK(b.x >= 0);
    CHECK(b.y >= 0);
    CHECK(b.right() <= vp.w);
    CHECK(b.bottom() <= vp.h);
    u.d.press("Escape");
    u.d.press("Escape");
  }
}

TEST_CASE("radial menu: a 120 ms open animation, then idle; damage stays inside the menu's bounds") {
  Ui u;
  u.opener->options.animate = true;
  u.d.click(u.openerCentre());
  RadialMenu* m = u.menu();
  REQUIRE(m != nullptr);
  CHECK(m->openProgress() == 0.0f);
  u.d.step();
  u.d.advance(0.06);
  CHECK(m->openProgress() > 0.0f);
  CHECK(m->openProgress() < 1.0f);
  u.d.advance(0.1);
  CHECK(m->openProgress() == 1.0f);
  const uint64_t frames = u.d.framesDrawn();
  u.d.advance(0.5);
  CHECK(u.d.framesDrawn() == frames);
  // A hover change damages only the menu's bounds.
  u.d.move(u.bubble("Logic"));
  REQUIRE_FALSE(u.d.tree().damage().full());
  const pq::ui::Rect dmg = u.d.tree().damage().bounds();
  const IRect b = m->bounds();
  CHECK(dmg.x0 >= static_cast<float>(b.x));
  CHECK(dmg.y0 >= static_cast<float>(b.y));
  CHECK(dmg.x1 <= static_cast<float>(b.right()));
  CHECK(dmg.y1 <= static_cast<float>(b.bottom()));
  CHECK(b.w < 520);
}

TEST_CASE("radial menu: no allocation per frame while open (hover, rings, filter results painted)") {
  Ui u(520, 420, 1.0f, {238, 188}, true);
  u.open("ge");
  auto script = [&] {
    for (const char* cat : {"Compare", "Logic", "Arithmetic", "Range", "Geometry", "Compare"}) {
      u.d.move(u.bubble(cat));
      u.d.step();
    }
    u.d.move(u.bubble("gt"));
    u.d.step();
    u.d.move(u.bubble("ne"));
    u.d.step();
  };
  script();  // warm-up: text runs shaped and cached, draw-list and damage storage grown
  u.d.type("m");
  u.d.step();
  u.d.press("Down");
  u.d.step();
  u.d.press("Escape");
  u.d.step();
  script();
  REQUIRE(pq::alloc_trap::installed());
  const uint64_t n = pq::alloc_trap::countRtViolations(script);
  CHECK(n == 0);
}

}  // TEST_SUITE

// --- goldens --------------------------------------------------------------------------------------------

namespace {
void checkRadialGolden(const char* name, float scale, const char* typed, const char* current, bool open, const char* hover) {
  const uint32_t w = 480, h = 400;
  Ui u(static_cast<uint32_t>(std::lround(w * scale)), static_cast<uint32_t>(std::lround(h * scale)), scale, {218, 178}, true);
  if (open) {
    u.open(current ? current : "");
    if (hover) u.d.move(u.bubble(hover));
    if (typed) u.d.type(typed);
  }
  u.d.advance(0.2);
  auto img = u.d.render();
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string file = std::string(name) + (scale == 1.0f ? "" : "@2x");
  auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/ui/golden/" + file + ".png");
  if (!ref) (void)writePng(file + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(file, *img, *ref, true, &d));
  MESSAGE(file << ": " << pq::ui::test::describe(d));
}
}  // namespace

TEST_CASE("render: radial menu goldens (closed, level 1, level 2, filtered) at 1x and 2x") {
  for (const float s : {1.0f, 2.0f}) {
    checkRadialGolden("radial-closed", s, nullptr, nullptr, false, nullptr);
    checkRadialGolden("radial-level1", s, nullptr, nullptr, true, nullptr);
    checkRadialGolden("radial-level2", s, nullptr, "ge", true, nullptr);
    checkRadialGolden("radial-filtered", s, "m", "ge", true, nullptr);
  }
}
