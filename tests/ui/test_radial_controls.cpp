// SX.1: the radial menu's controls (knob, slider, chips, toggle) through real event dispatch (TestDriver → UiTree →
// OverlayLayer → RadialMenu): placement by side, knob / slider drags that keep the menu (and a sub-ring) open, chips,
// toggles, the wheel, reset, typed entry, Esc and a press outside mid-gesture (one undo step each), the opener's
// document changing or vanishing under the menu (poisoned frees), no allocation per frame; goldens at 1× and 2×.

#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/radial_menu.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"

#include "golden_check.h"

using namespace pq::ui;

namespace {

// The opener's document: replaced or deleted under the menu by some tests.
struct Doc {
  double transpose = 0.0, gain = 0.0, detune = 0.0, fade = 0.0, grain = 50.0;
  int mode = 0, preserve = 0;
  bool reverse = false;
};

// Gestures as an undo stack sees them: every set inside begin / end, one step per end.
struct Model {
  std::unique_ptr<Doc> doc = std::make_unique<Doc>();
  int begins = 0, sets = 0, ends = 0;
  bool open = false;
  bool misuse = false;  // a set outside a gesture, a nested begin, an end without a begin
  void begin() {
    misuse = misuse || open;
    open = true;
    ++begins;
  }
  void end() {
    misuse = misuse || !open;
    open = false;
    ++ends;
  }
  template <class F> RadialBinding bind(F field) {
    RadialBinding b;
    Model* m = this;  // the model outlives every menu; the document is looked up on each call
    b.get = [m, field] { return m->doc ? field(*m->doc) : 0.0; };
    b.begin = [m] { m->begin(); };
    b.end = [m] { m->end(); };
    return b;
  }
};

const char* modeName(int m) {
  static const char* names[] = {"Off", "Re-Pitch", "Beats", "Tones", "Texture"};
  return m >= 0 && m < 5 ? names[m] : "?";
}

std::vector<RadialItem> clipItems(Model& md) {
  using R = RadialItem;
  Model* m = &md;
  auto set = [m](auto apply) {
    return [m, apply](double v) {
      m->misuse = m->misuse || !m->open;
      ++m->sets;
      if (m->doc) apply(*m->doc, v);
    };
  };
  ValueSpec tr;
  tr.min = -48;
  tr.max = 48;
  tr.step = 1;
  tr.decimals = 0;
  tr.unit = "st";
  tr.keyStep = 1;
  RadialBinding trB = md.bind([](const Doc& d) { return d.transpose; });
  trB.set = set([](Doc& d, double v) { d.transpose = v; });

  ValueSpec gain;
  gain.min = -24;
  gain.max = 24;
  gain.decimals = 1;
  gain.unit = "dB";
  gain.keyStep = 0.5;
  RadialBinding gainB = md.bind([](const Doc& d) { return d.gain; });
  gainB.set = set([](Doc& d, double v) { d.gain = v; });

  ValueSpec det;
  det.min = -50;
  det.max = 50;
  det.decimals = 0;
  det.unit = "ct";
  RadialBinding detB = md.bind([](const Doc& d) { return d.detune; });
  detB.set = set([](Doc& d, double v) { d.detune = v; });
  detB.enabled = [m] { return m->doc && !m->doc->reverse; };  // a control the state disables

  ValueSpec fade;
  fade.min = 0;
  fade.max = 1000;
  fade.decimals = 0;
  fade.unit = "ms";
  RadialBinding fadeB = md.bind([](const Doc& d) { return d.fade; });
  fadeB.set = set([](Doc& d, double v) { d.fade = v; });

  ValueSpec grain;
  grain.min = 0;
  grain.max = 100;
  grain.def = 50;
  grain.decimals = 0;
  grain.unit = "%";
  RadialBinding grainB = md.bind([](const Doc& d) { return d.grain; });
  grainB.set = set([](Doc& d, double v) { d.grain = v; });

  RadialBinding presB = md.bind([](const Doc& d) { return static_cast<double>(d.preserve); });
  presB.set = set([](Doc& d, double v) { d.preserve = static_cast<int>(v); });

  RadialBinding revB = md.bind([](const Doc& d) { return d.reverse ? 1.0 : 0.0; });
  revB.set = set([](Doc& d, double v) { d.reverse = v != 0.0; });

  RadialBinding modeB = md.bind([](const Doc& d) { return static_cast<double>(d.mode); });
  modeB.set = set([](Doc& d, double v) { d.mode = static_cast<int>(v); });
  auto mode = std::make_shared<const RadialBinding>(std::move(modeB));  // one binding, two groups (two sides)

  std::vector<RadialItem> beats;
  beats.push_back(R::slider("grain", "Grain", grain, grainB));
  beats.push_back(R::chips("preserve", {{"Trans", 0, {}}, {"1/8", 1, {}}, {"1/16", 2, {}}}, presB));
  std::vector<RadialChoice> right;
  right.push_back({"Beats", 2, std::move(beats)});
  right.push_back({"Tones", 3, {}});
  right.push_back({"Texture", 4, {}});

  std::vector<RadialItem> items;
  items.push_back(R::knob("gain", "Gain", gain, gainB, RadialPlace::top()));
  items.push_back(R::knob("detune", "Detune", det, detB, RadialPlace::top()));
  items.push_back(R::chips("mode", std::move(right), mode, RadialPlace::right()));
  items.push_back(R::knob("transpose", "Transpose", tr, trB, RadialPlace::bottom()));
  items.push_back(R::chips("mode", {{"Off", 0, {}}, {"Re-Pitch", 1, {}}}, mode, RadialPlace::left()));
  items.push_back(R::toggle("reverse", "Reverse", revB));
  items.push_back(R::slider("fade", "Fade In", fade, fadeB));
  items.push_back(R::category("More", {R::leaf("consolidate", "Consolidate"), R::leaf("crop", "Crop")}));
  return items;
}

class Opener : public Widget {
public:
  Opener(std::string name, Model& m) : Widget(std::move(name)), model(m) {
    setSize(Length::fixed(44), Length::fixed(44));
    setFocusable(true);
  }
  Model& model;
  RadialMenu* menu = nullptr;  // nulled by onClosed: the opener never holds a dangling menu
  std::vector<std::string> picks;
  int closes = 0;
  bool onPointer(const PointerEvent& e) override {
    if (e.type != PointerType::Down || e.button != PointerButton::Left) return false;
    RadialMenuOptions o;
    o.animate = false;
    Model* m = &model;
    o.centreText = [m] { return m->doc ? std::string("Clip\n") + modeName(m->doc->mode) : std::string(); };
    o.valid = [m] { return m->doc != nullptr; };
    o.onPick = [this](std::string_view k) { picks.emplace_back(k); };
    o.onClosed = [this] {
      ++closes;
      menu = nullptr;
    };
    menu = openRadialMenu(*this, rect(), clipItems(model), std::move(o));
    return true;
  }
  // The opener's document changed (an undo, another view): the open menu re-reads.
  void documentChanged() {
    if (menu) menu->refresh();
  }
  void paint(PaintContext& ctx) override {
    if (!ctx.res.theme) return;
    const IRect r = rect();
    ctx.dl.addRoundedRectFilled(r.toRect(), ctx.scale.exact(3.0f), ctx.res.theme->surface.raised);
  }
};

class Probe : public Widget {
public:
  using Widget::Widget;
  Vec2 at{0, 0};
  int presses = 0;
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
  Model model;
  Probe* content = nullptr;
  Opener* opener = nullptr;
  OverlayLayer* overlay = nullptr;

  explicit Ui(uint32_t w = 640, uint32_t h = 560, float scale = 1.0f, Vec2 at = {298, 258}, bool gpu = false) : d(w, h, scale) {
    if (gpu) REQUIRE(d.enableGpu());
    Widget* root = &d.setRoot<Widget>();
    content = &root->add<Probe>("content");
    content->setLayout(LayoutKind::Custom);
    content->at = at;
    opener = &content->add<Opener>("opener", model);
    overlay = &root->add<OverlayLayer>("overlay");
    d.tree().layoutNow();
  }
  RadialMenu* menu() const { return dynamic_cast<RadialMenu*>(overlay->top()); }
  RadialMenu& open() {
    d.click(d.center("content/opener"));
    RadialMenu* m = menu();
    REQUIRE(m != nullptr);
    d.step();
    return *m;
  }
  int32_t item(std::string_view k) {
    const int32_t i = menu()->find(k);
    REQUIRE_MESSAGE(i >= 0, k);
    return i;
  }
  Vec2 at(std::string_view k) {
    const int32_t i = item(k);
    REQUIRE_MESSAGE(menu()->shown(i), k);
    return menu()->centreOf(i);
  }
  void frame() {
    d.step();
    (void)d.tree().cursor();
  }
  bool balanced() const { return !model.open && model.begins == model.ends && !model.misuse; }
};

}  // namespace

TEST_SUITE("radial controls") {

TEST_CASE("radial controls: pinned sides place the knob below, knobs above, the chip groups left and right") {
  Ui u;
  RadialMenu& m = u.open();
  const Vec2 c = m.centre();
  const Vec2 tr = u.at("transpose"), gain = u.at("gain"), det = u.at("detune");
  CHECK(tr.y > c.y + 20.0f);
  CHECK(std::abs(tr.x - c.x) < 0.25f * (tr.y - c.y));
  CHECK(gain.y < c.y - 20.0f);
  CHECK(det.y < c.y - 20.0f);
  CHECK(gain.x < c.x);  // side by side, in order, clockwise
  CHECK(det.x > c.x);
  for (const char* k : {"mode/Off", "mode/Re-Pitch"}) CHECK(u.at(k).x < c.x - 20.0f);
  for (const char* k : {"mode/Beats", "mode/Tones", "mode/Texture"}) CHECK(u.at(k).x > c.x + 20.0f);
  // Knobs are twice a bubble; a slider is a capsule wider than tall.
  CHECK(m.radiusOf(u.item("transpose")) >= 1.25f * m.radiusOf(u.item("mode/Off")));
  CHECK(m.halfWidthOf(u.item("fade")) > m.radiusOf(u.item("fade")) * 2.0f);
  CHECK(m.kindOf(u.item("transpose")) == RadialKind::Knob);
  CHECK(m.kindOf(u.item("fade")) == RadialKind::Slider);
  CHECK(m.kindOf(u.item("reverse")) == RadialKind::Toggle);
  CHECK(m.kindOf(u.item("mode/Beats")) == RadialKind::Chip);
  // No two ring-0 bubbles overlap (boxes around the capsules), and all are inside the window.
  std::vector<int32_t> ring0;
  for (const char* k : {"gain", "detune", "mode/Beats", "mode/Tones", "mode/Texture", "transpose", "mode/Off", "mode/Re-Pitch",
                        "reverse", "fade", "More"})
    ring0.push_back(u.item(k));
  for (size_t i = 0; i < ring0.size(); ++i) {
    const Vec2 a = m.centreOf(ring0[i]);
    const float aw = m.halfWidthOf(ring0[i]), ah = m.radiusOf(ring0[i]);
    CHECK(a.x - aw >= 0.0f);
    CHECK(a.y - ah >= 0.0f);
    CHECK(a.x + aw <= 640.0f);
    CHECK(a.y + ah <= 560.0f);
    for (size_t j = i + 1; j < ring0.size(); ++j) {
      const Vec2 b = m.centreOf(ring0[j]);
      const float bw = m.halfWidthOf(ring0[j]), bh = m.radiusOf(ring0[j]);
      const bool apart = std::abs(a.x - b.x) >= aw + bw - 0.5f || std::abs(a.y - b.y) >= ah + bh - 0.5f ||
                         std::hypot(a.x - b.x, a.y - b.y) >= std::max(aw, ah) + std::max(bw, bh) - 0.5f;
      CHECK_MESSAGE(apart, m.labelOf(ring0[i]) << " / " << m.labelOf(ring0[j]));
    }
  }
  // Values were read at open; the centre shows the opener's text.
  CHECK(m.valueTextOf(u.item("transpose")) == "0 st");
  CHECK(m.litOf(u.item("mode/Off")));
  CHECK_FALSE(m.litOf(u.item("mode/Beats")));
  CHECK(m.centreText() == "Clip\nOff");
  CHECK(u.model.begins == 0);
}

TEST_CASE("radial controls: dragging a knob edits in one gesture and keeps the menu open; the pointer can wander") {
  Ui u;
  RadialMenu& m = u.open();
  const int32_t tr = u.item("transpose");
  const Vec2 p = u.at("transpose");
  u.d.down(p);
  CHECK(m.dragging() == tr);
  CHECK(u.model.begins == 0);  // a press alone changes nothing
  for (int i = 1; i <= 5; ++i) u.d.move({p.x, p.y - 10.0f * static_cast<float>(i)});  // 50 px up of 200: a quarter of 96
  CHECK(u.model.doc->transpose == 24.0);
  CHECK(m.valueTextOf(tr) == "24 st");
  CHECK(u.model.begins == 1);
  CHECK(u.model.ends == 0);
  // Over other bubbles: still the knob's drag (relative: the value moves with the pointer's y, clamped), nothing
  // else is highlighted or opened.
  u.d.move(u.at("mode/Beats"));
  CHECK(m.dragging() == tr);
  CHECK(m.highlighted() == tr);
  CHECK(m.openDepth() == 0);
  u.d.move({p.x, p.y - 50.0f});
  u.d.up({p.x, p.y - 50.0f});
  const double v = u.model.doc->transpose;
  CHECK(u.model.ends == 1);
  CHECK(u.balanced());
  CHECK(u.overlay->isOpen(m));
  CHECK(u.d.tree().captured() == nullptr);
  // Shift: fine (×0.1): 50 px → 2.4 → 2 steps.
  u.d.down(p);
  u.d.move({p.x, p.y - 25.0f}, mod::kShift);
  u.d.move({p.x, p.y - 50.0f}, mod::kShift);
  u.d.up({p.x, p.y - 50.0f}, PointerButton::Left, mod::kShift);
  CHECK(u.model.doc->transpose == v + 2.0);
  CHECK(u.model.ends == 2);
  CHECK(u.balanced());
  // A disabled control ignores input.
  u.model.doc->reverse = true;
  u.opener->documentChanged();
  CHECK_FALSE(m.enabledOf(u.item("detune")));
  const Vec2 dp = u.at("detune");
  u.d.down(dp);
  u.d.move({dp.x, dp.y - 40.0f});
  u.d.up({dp.x, dp.y - 40.0f});
  CHECK(u.model.doc->detune == 0.0);
  CHECK(u.model.begins == 2);
}

TEST_CASE("radial controls: a slider in a chip's sub-ring jumps, drags, and keeps that ring open while dragged") {
  Ui u;
  RadialMenu& m = u.open();
  u.d.move(u.at("mode/Beats"));  // hovering the chip opens its ring (choosing it takes a click)
  CHECK(m.openDepth() == 1);
  CHECK(m.openItem(0) == u.item("mode/Beats"));
  CHECK(u.model.doc->mode == 0);
  const int32_t grain = u.item("grain");
  REQUIRE(m.shown(grain));
  const Vec2 s0 = m.sliderStart(grain), s1 = m.sliderEnd(grain);
  CHECK(s1.x > s0.x + 40.0f);
  u.d.move(s0);
  u.d.down(s0);
  CHECK(u.model.doc->grain == 0.0);  // the press jumps to the pointer
  CHECK(u.model.begins == 1);
  u.d.move({0.5f * (s0.x + s1.x), s0.y});
  CHECK(u.model.doc->grain == doctest::Approx(50.0).epsilon(0.02));
  u.d.move(s1);
  CHECK(u.model.doc->grain == 100.0);
  // Past the bar's end, off the ring: clamped. Over another ring-0 chip: the ring stays open, the slider keeps the
  // drag (its value follows the pointer's x along the bar).
  u.d.move({s1.x + 150.0f, s1.y + 120.0f});
  CHECK(u.model.doc->grain == 100.0);
  u.d.move(u.at("mode/Tones"));
  CHECK(m.dragging() == grain);
  CHECK(m.openDepth() == 1);
  CHECK(m.openItem(0) == u.item("mode/Beats"));
  CHECK(m.shown(grain));
  CHECK(m.highlighted() == grain);
  u.d.up(u.at("mode/Tones"));
  CHECK(u.model.ends == 1);
  CHECK(u.model.sets >= 3);
  CHECK(u.balanced());
  CHECK(u.overlay->isOpen(m));
  // Hovering Tones after the drag: back to normal browsing.
  const Vec2 tones = u.at("mode/Tones");
  u.d.move({tones.x + 1.0f, tones.y});
  CHECK(m.openDepth() == 0);
}

TEST_CASE("radial controls: chips choose one value (the chosen lit), a toggle flips; one undo step per click") {
  Ui u;
  RadialMenu& m = u.open();
  u.d.click(u.at("mode/Re-Pitch"));
  CHECK(u.model.doc->mode == 1);
  CHECK(m.litOf(u.item("mode/Re-Pitch")));
  CHECK_FALSE(m.litOf(u.item("mode/Off")));
  CHECK(u.model.begins == 1);
  CHECK(u.model.ends == 1);
  CHECK(u.overlay->isOpen(m));
  // Groups on two sides share one binding: a chip on the right un-lights the left.
  u.d.click(u.at("mode/Beats"));
  CHECK(u.model.doc->mode == 2);
  CHECK(m.litOf(u.item("mode/Beats")));
  CHECK_FALSE(m.litOf(u.item("mode/Re-Pitch")));
  CHECK(m.centreText() == "Clip\nBeats");  // re-read after the menu's own edit
  CHECK(m.openItem(0) == u.item("mode/Beats"));  // its parameters' ring
  CHECK(u.model.ends == 2);
  // The chosen chip again: nothing to do, no undo step.
  u.d.click(u.at("mode/Beats"));
  CHECK(u.model.begins == 2);
  // Chips in the sub-ring.
  u.d.click(u.at("preserve/1/16"));
  CHECK(u.model.doc->preserve == 2);
  CHECK(u.model.ends == 3);
  // A press on one chip released on another chooses nothing.
  u.d.down(u.at("preserve/Trans"));
  u.d.up(u.at("preserve/1/8"));
  CHECK(u.model.doc->preserve == 2);
  // The toggle.
  u.d.click(u.at("reverse"));
  CHECK(u.model.doc->reverse);
  CHECK(m.litOf(u.item("reverse")));
  CHECK_FALSE(m.enabledOf(u.item("detune")));  // the toggle's edit re-read the others
  u.d.click(u.at("reverse"));
  CHECK_FALSE(u.model.doc->reverse);
  CHECK(m.enabledOf(u.item("detune")));
  CHECK(u.model.ends == 5);
  CHECK(u.balanced());
  CHECK(u.opener->picks.empty());
}

TEST_CASE("radial controls: the wheel nudges a knob (Shift fine) in one gesture that ends after a pause or on leaving") {
  Ui u;
  RadialMenu& m = u.open();
  const Vec2 g = u.at("gain");
  u.d.move(g);
  u.d.wheel(g, 1.0f);
  u.d.wheel(g, 1.0f);
  u.d.wheel(g, 1.0f);
  CHECK(u.model.doc->gain == doctest::Approx(1.5));
  CHECK(m.valueTextOf(u.item("gain")) == "1.5 dB");
  CHECK(u.model.begins == 1);
  CHECK(u.model.ends == 0);
  CHECK(m.inGesture() == u.item("gain"));
  u.d.advance(0.6);  // the run ends 0.4 s after its last notch
  CHECK(u.model.ends == 1);
  CHECK(m.inGesture() == -1);
  u.d.wheel(g, -1.0f, 0.0f, mod::kShift);
  CHECK(u.model.doc->gain == doctest::Approx(1.45));
  CHECK(u.model.begins == 2);
  u.d.move(m.centre());  // leaving the knob ends the run at once
  CHECK(u.model.ends == 2);
  // A slider takes the wheel too; Ctrl + wheel is not a nudge.
  const Vec2 f = u.at("fade");
  u.d.move(f);
  u.d.wheel(f, 2.0f);
  CHECK(u.model.doc->fade == doctest::Approx(20.0));
  u.d.wheel(f, 2.0f, 0.0f, mod::kCtrl);
  CHECK(u.model.doc->fade == doctest::Approx(20.0));
  u.d.advance(0.6);
  CHECK(u.balanced());
  CHECK(u.overlay->isOpen(m));
}

TEST_CASE("radial controls: Alt-click resets to the default; a slider's double-click resets") {
  Ui u;
  RadialMenu& m = u.open();
  u.model.doc->transpose = 7;
  u.model.doc->fade = 300;
  u.opener->documentChanged();
  u.d.click(u.at("transpose"), mod::kAlt);
  CHECK(u.model.doc->transpose == 0.0);
  CHECK(u.model.ends == 1);
  CHECK(m.dragging() == -1);
  u.d.move(u.at("fade"));
  u.d.doubleClick(u.at("fade"));  // the first click jumps the bar to the centre (a step), the second resets
  CHECK(u.model.doc->fade == 0.0);
  CHECK(u.model.ends == 3);
  CHECK(m.typingIn() == -1);
  CHECK(u.balanced());
}

TEST_CASE("radial controls: typed entry on a knob: double-click or Enter, Enter commits, Esc cancels, junk reverts") {
  Ui u;
  RadialMenu& m = u.open();
  const int32_t tr = u.item("transpose");
  u.d.doubleClick(u.at("transpose"));
  REQUIRE(m.typingIn() == tr);
  CHECK(m.typeInBuffer().text() == "0");
  CHECK(m.typeInBuffer().hasSelection());
  u.d.type("-");
  u.d.type("7");
  CHECK(m.typeInBuffer().text() == "-7");
  CHECK(u.d.press("Return") == KeyResult::Widget);
  CHECK(m.typingIn() == -1);
  CHECK(u.model.doc->transpose == -7.0);
  CHECK(m.valueTextOf(tr) == "-7 st");
  CHECK(u.model.begins == 1);
  CHECK(u.model.ends == 1);
  CHECK(u.overlay->isOpen(m));
  // Out of range clamps; the unit may be typed.
  u.d.doubleClick(u.at("transpose"));
  u.d.type("60 st");
  u.d.press("Return");
  CHECK(u.model.doc->transpose == 48.0);
  // Junk reverts, Esc cancels (the menu stays open); typing never reaches the filter meanwhile.
  u.d.doubleClick(u.at("transpose"));
  u.d.type("abc");
  CHECK(m.filter().empty());
  u.d.press("Return");
  CHECK(u.model.doc->transpose == 48.0);
  u.d.doubleClick(u.at("transpose"));
  u.d.type("5");
  u.d.press("Escape");
  CHECK(m.typingIn() == -1);
  CHECK(u.model.doc->transpose == 48.0);
  CHECK(u.overlay->isOpen(m));
  // Keyboard: Enter on the highlighted knob opens it; Backspace edits.
  u.d.move(u.at("transpose"));
  u.d.press("Return");
  REQUIRE(m.typingIn() == tr);
  u.d.press("Backspace");
  u.d.type("12");
  u.d.press("Return");
  CHECK(u.model.doc->transpose == 12.0);
  CHECK(u.model.ends == 3);
  CHECK(u.balanced());
}

TEST_CASE("radial controls: Esc and a press outside mid-gesture end it (one undo step) and close") {
  SUBCASE("Esc during a drag") {
    Ui u;
    RadialMenu& m = u.open();
    const Vec2 p = u.at("transpose");
    u.d.down(p);
    u.d.move({p.x, p.y - 30.0f});
    CHECK(u.model.begins == 1);
    u.d.press("Escape");
    CHECK(u.model.ends == 1);
    CHECK(u.overlay->openCount() == 0);
    u.d.up({p.x, p.y - 30.0f});
    CHECK(u.d.tree().captured() == nullptr);
    CHECK(u.balanced());
    CHECK(u.model.doc->transpose == doctest::Approx(14.0));  // the edit so far stays
    (void)m;
  }
  SUBCASE("a press outside during a wheel run") {
    Ui u;
    u.open();
    const Vec2 g = u.at("gain");
    u.d.move(g);
    u.d.wheel(g, 2.0f);
    CHECK(u.model.begins == 1);
    CHECK(u.model.ends == 0);
    const int before = u.content->presses;
    u.d.click({6.0f, 6.0f});
    CHECK(u.model.ends == 1);
    CHECK(u.overlay->openCount() == 0);
    CHECK(u.content->presses == before);  // consumed
    u.d.advance(0.6);                     // the cancelled run's timer does not end anything again
    CHECK(u.model.ends == 1);
    CHECK(u.balanced());
  }
  SUBCASE("a press outside during a typed entry commits it") {
    Ui u;
    RadialMenu& m = u.open();
    u.d.doubleClick(u.at("transpose"));
    REQUIRE(m.typingIn() >= 0);
    u.d.type("9");
    u.d.click({6.0f, 6.0f});
    CHECK(u.model.doc->transpose == 9.0);
    CHECK(u.overlay->openCount() == 0);
    CHECK(u.model.ends == 1);
    CHECK(u.balanced());
  }
  SUBCASE("the layer closing the menu mid-drag") {
    Ui u;
    u.open();
    const Vec2 p = u.at("transpose");
    u.d.down(p);
    u.d.move({p.x, p.y - 20.0f});
    u.overlay->closeAll();
    CHECK(u.model.ends == 1);
    u.d.up({p.x, p.y - 20.0f});
    u.frame();
    CHECK(u.balanced());
  }
}

TEST_CASE("radial controls: a pick leaf still picks and closes; Esc on ring 0 closes") {
  Ui u;
  u.open();
  u.d.move(u.at("More"));
  u.d.click(u.at("crop"));
  CHECK(u.opener->picks == std::vector<std::string>{"crop"});
  CHECK(u.overlay->openCount() == 0);
  CHECK(u.opener->menu == nullptr);
  u.frame();
  u.open();
  u.d.press("Escape");
  CHECK(u.overlay->openCount() == 0);
  CHECK(u.opener->closes == 2);
  // Typing filters pick leaves only (no control is a result).
  RadialMenu& m = u.open();
  u.d.type("c");
  for (int32_t i = 0; i < m.resultCount(); ++i) CHECK(m.kindOf(m.result(i)) == RadialKind::Item);
  CHECK(m.resultCount() == 2);  // Consolidate, Crop
}

TEST_CASE("radial controls: the opener's document changing or vanishing under the menu (poisoned frees)") {
  Ui u;
  REQUIRE(pq::alloc_trap::installed());
  RadialMenu& m = u.open();
  const int32_t tr = u.item("transpose");
  // Changed underneath (an undo): refresh re-reads values, lit chips, enabled states and the centre text.
  u.model.doc->transpose = 12;
  u.model.doc->mode = 3;
  u.opener->documentChanged();
  CHECK(m.valueOf(tr) == 12.0);
  CHECK(m.valueTextOf(tr) == "12 st");
  CHECK(m.litOf(u.item("mode/Tones")));
  CHECK(m.centreText() == "Clip\nTones");
  {
    // Replaced: the old document is freed (poisoned); the bindings look the new one up.
    pq::alloc_trap::PoisonFrees poison;
    auto next = std::make_unique<Doc>();
    next->transpose = -3;
    u.model.doc = std::move(next);
    u.opener->documentChanged();
    u.frame();
  }
  CHECK(m.valueTextOf(tr) == "-3 st");
  CHECK(m.litOf(u.item("mode/Off")));
  // Deleted mid-drag: the menu closes, the gesture ends once, the opener is told (no dangling menu pointer).
  const Vec2 p = u.at("transpose");
  u.d.down(p);
  u.d.move({p.x, p.y - 20.0f});
  CHECK(u.model.begins == 1);
  {
    pq::alloc_trap::PoisonFrees poison;
    u.model.doc.reset();
    u.opener->documentChanged();
    CHECK(u.overlay->openCount() == 0);
    CHECK(u.opener->menu == nullptr);
    u.d.move({p.x, p.y - 40.0f});
    u.d.up({p.x, p.y - 40.0f});
    u.frame();
    u.frame();
    u.opener->documentChanged();  // a no-op: the opener knows the menu is gone
    (void)u.d.tree().cursor();
  }
  CHECK(u.model.ends == 1);
  CHECK(u.balanced());
}

TEST_CASE("radial controls: no allocation per frame while open (hover over controls, a sub-ring, painted)") {
  Ui u(640, 560, 1.0f, {298, 258}, true);
  u.open();
  auto script = [&] {
    for (const char* k : {"transpose", "gain", "detune", "mode/Beats", "grain", "preserve/1/8", "mode/Off", "reverse", "fade"}) {
      RadialMenu* m = u.menu();
      const int32_t i = m->find(k);
      if (i < 0 || !m->shown(i)) continue;
      u.d.move(m->centreOf(i));
      u.d.step();
    }
  };
  script();  // warm-up: text runs shaped and cached, draw-list and damage storage grown
  script();
  const uint64_t n = pq::alloc_trap::countRtViolations(script);
  CHECK(n == 0);
}

}  // TEST_SUITE

// --- goldens --------------------------------------------------------------------------------------------

namespace {
void checkControlsGolden(const char* name, float scale, bool typing) {
  const uint32_t w = 560, h = 480;
  Ui u(static_cast<uint32_t>(std::lround(w * scale)), static_cast<uint32_t>(std::lround(h * scale)), scale, {258, 218}, true);
  u.model.doc->mode = 2;
  u.model.doc->transpose = 5;
  u.model.doc->gain = -6;
  u.model.doc->fade = 250;
  u.model.doc->grain = 70;
  u.model.doc->preserve = 1;
  u.model.doc->reverse = true;  // Detune drawn disabled
  u.open();
  if (typing) {
    u.d.doubleClick(u.at("transpose"));
    u.d.type("12");
  } else {
    u.d.move(u.at("mode/Beats"));
    u.d.move(u.at("grain"));
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

TEST_CASE("render: radial menu controls goldens (a sub-ring open; a typed entry) at 1x and 2x") {
  for (const float s : {1.0f, 2.0f}) {
    checkControlsGolden("radial-controls", s, false);
    checkControlsGolden("radial-controls-typing", s, true);
  }
}
