// SX.5: the radial menu's Bands layout (RadialMenuOptions::layout = Bands) through real event dispatch (TestDriver →
// UiTree → OverlayLayer → RadialMenu): rows hugging the opener's rectangle above / below (never covering it), centred
// on bandX; the lit chip's children as the row outward from it (no hover sub-rings), swapped when another chip is
// chosen; bands flipped / clamped at the window's edges; the same control gestures (one gesture per edit); a press on a
// panel between items consumed, outside closes; keys; no allocation per frame.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/ui/overlay.h"
#include "pq/ui/radial_menu.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/widget.h"

using namespace pq::ui;

namespace {

struct Model {
  double transpose = 0.0, grain = 50.0;
  int mode = 2, preserve = 0;  // Beats
  bool reverse = false;
  int begins = 0, ends = 0;
  bool open = false, misuse = false;
  RadialBinding bind(double Model::*field) {
    RadialBinding b;
    Model* m = this;
    b.get = [m, field] { return m->*field; };
    b.set = [m, field](double v) {
      m->misuse = m->misuse || !m->open;
      m->*field = v;
    };
    b.begin = [m] {
      m->misuse = m->misuse || m->open;
      m->open = true;
      ++m->begins;
    };
    b.end = [m] {
      m->misuse = m->misuse || !m->open;
      m->open = false;
      ++m->ends;
    };
    return b;
  }
  RadialBinding bindInt(int Model::*field) {
    RadialBinding b = bind(nullptr);
    Model* m = this;
    b.get = [m, field] { return static_cast<double>(m->*field); };
    b.set = [m, field](double v) {
      m->misuse = m->misuse || !m->open;
      m->*field = static_cast<int>(v);
    };
    return b;
  }
  bool balanced() const { return !open && begins == ends && !misuse; }
};

std::vector<RadialItem> bandItems(Model& md) {
  using R = RadialItem;
  ValueSpec tr;
  tr.min = -48;
  tr.max = 48;
  tr.step = 1;
  tr.decimals = 0;
  tr.unit = "st";
  ValueSpec grain;
  grain.min = 0;
  grain.max = 100;
  grain.def = 50;
  grain.decimals = 0;
  RadialBinding trB = md.bind(&Model::transpose);
  RadialBinding grainB = md.bind(&Model::grain);
  RadialBinding presB = md.bindInt(&Model::preserve);
  RadialBinding revB = md.bind(nullptr);
  Model* m = &md;
  revB.get = [m] { return m->reverse ? 1.0 : 0.0; };
  revB.set = [m](double v) { m->reverse = v != 0.0; };
  auto mode = std::make_shared<const RadialBinding>(md.bindInt(&Model::mode));

  std::vector<RadialItem> beats;
  beats.push_back(R::chips("preserve", {{"Trans", 0, {}}, {"1/8", 1, {}}, {"1/16", 2, {}}}, presB));
  beats.push_back(R::slider("grain", "Grain", grain, grainB));
  std::vector<RadialItem> tones;
  tones.push_back(R::slider("tones-grain", "Grain", grain, md.bind(&Model::grain)));
  std::vector<RadialChoice> stretch;
  stretch.push_back({"Beats", 2, std::move(beats)});
  stretch.push_back({"Tones", 3, std::move(tones)});
  stretch.push_back({"Complex", 4, {}});

  std::vector<RadialItem> items;
  items.push_back(R::chips("mode", {{"Off", 0, {}}, {"Re-Pitch", 1, {}}}, mode, RadialPlace::above()));
  items.push_back(R::chips("mode", std::move(stretch), mode, RadialPlace::above()));
  items.push_back(R::knob("transpose", "Transpose", tr, trB, RadialPlace::below()));
  items.push_back(R::toggle("reverse", "Reverse", revB, RadialPlace::below()));
  return items;
}

// The rectangle the bands hug (a clip): a widget at a chosen place; a press opens the menu at the press x.
class Opener : public Widget {
public:
  Opener(std::string name, Model& m) : Widget(std::move(name)), model(m) {}
  Model& model;
  RadialMenu* menu = nullptr;
  float atX = std::numeric_limits<float>::quiet_NaN();
  bool onPointer(const PointerEvent& e) override {
    if (e.type != PointerType::Down || e.button != PointerButton::Left) return false;
    RadialMenuOptions o;
    o.animate = false;
    o.layout = RadialLayout::Bands;
    o.bandX = std::isnan(atX) ? e.pos.x : atX;
    o.onClosed = [this] { menu = nullptr; };
    menu = openRadialMenu(*this, rect(), bandItems(model), std::move(o));
    return true;
  }
  void documentChanged() {
    if (menu) menu->refresh();
  }
};

class Area : public Widget {
public:
  using Widget::Widget;
  IRect at{};
  int presses = 0;
  void arrange(LayoutPass& pass) override {
    for (size_t i = 0; i < childCount(); ++i)
      pass.place(child(i), {rect().x + pass.px(static_cast<float>(at.x)), rect().y + pass.px(static_cast<float>(at.y)),
                            pass.px(static_cast<float>(at.w)), pass.px(static_cast<float>(at.h))});
  }
  bool onPointer(const PointerEvent& e) override {
    if (e.type == PointerType::Down) ++presses;
    return e.type == PointerType::Down;
  }
};

struct Ui {
  TestDriver d;
  Model model;
  Area* area = nullptr;
  Opener* opener = nullptr;
  OverlayLayer* overlay = nullptr;
  Ui(uint32_t w, uint32_t h, IRect clip, bool gpu = false) : d(w, h) {
    if (gpu) REQUIRE(d.enableGpu());
    Widget* root = &d.setRoot<Widget>();
    area = &root->add<Area>("area");
    area->setLayout(LayoutKind::Custom);
    area->at = clip;
    opener = &area->add<Opener>("clip", model);
    overlay = &root->add<OverlayLayer>("overlay");
    d.tree().layoutNow();
  }
  RadialMenu& open(Vec2 p) {
    d.click(p);
    RadialMenu* m = dynamic_cast<RadialMenu*>(overlay->top());
    REQUIRE(m != nullptr);
    d.step();
    return *m;
  }
  RadialMenu& open() { return open(d.center("area/clip")); }
  RadialMenu* menu() const { return dynamic_cast<RadialMenu*>(overlay->top()); }
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
  bool shown(std::string_view k) { return menu()->shown(item(k)); }
  IRect clip() const { return opener->rect(); }
};

bool overlaps(const IRect& a, const IRect& b) {
  return !a.empty() && !b.empty() && a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

bool inside(const IRect& r, int32_t w, int32_t h) { return r.x >= 0 && r.y >= 0 && r.right() <= w && r.bottom() <= h; }

// Every shown item's box (device px).
IRect itemBox(RadialMenu& m, int32_t i) {
  const Vec2 c = m.centreOf(i);
  const float hw = m.halfWidthOf(i), hh = m.radiusOf(i);
  return {static_cast<int32_t>(std::floor(c.x - hw)), static_cast<int32_t>(std::floor(c.y - hh)),
          static_cast<int32_t>(std::ceil(2.0f * hw)), static_cast<int32_t>(std::ceil(2.0f * hh))};
}

}  // namespace

TEST_SUITE("radial bands") {

TEST_CASE("radial bands: rows above and below the rectangle, centred on the press x; the lit chip's settings above it") {
  Ui u(900, 600, {200, 260, 400, 70});
  const IRect clip = u.clip();
  RadialMenu& m = u.open({450.0f, 290.0f});
  CHECK(m.bands());
  const IRect a = m.bandRect(0), b = m.bandRect(1);
  REQUIRE_FALSE(a.empty());
  REQUIRE_FALSE(b.empty());
  CHECK(a.bottom() <= clip.y);  // above, below: never over the clip
  CHECK(b.y >= clip.bottom());
  CHECK_FALSE(m.bandFlipped(0));
  CHECK_FALSE(m.bandFlipped(1));
  CHECK(std::abs(0.5f * static_cast<float>(a.x + a.right()) - 450.0f) <= 1.0f);
  CHECK(std::abs(0.5f * static_cast<float>(b.x + b.right()) - 450.0f) <= 1.0f);
  // Compact: hugging the clip, not a ring.
  CHECK(clip.y - a.bottom() <= 12);
  CHECK(b.y - clip.bottom() <= 12);
  CHECK(a.h < 120);
  CHECK(b.h < 100);
  // Above: the mode chips in one row (in order), Beats lit with its settings directly above them; Tones' not shown.
  const Vec2 off = u.at("mode/Off"), rp = u.at("mode/Re-Pitch"), be = u.at("mode/Beats"), cx = u.at("mode/Complex");
  CHECK(off.y == be.y);
  CHECK(off.x < rp.x);
  CHECK(rp.x < be.x);
  CHECK(be.x < cx.x);
  CHECK(rp.x + m.halfWidthOf(u.item("mode/Re-Pitch")) < be.x - m.halfWidthOf(u.item("mode/Beats")));
  CHECK(m.litOf(u.item("mode/Beats")));
  CHECK(u.at("preserve/1/8").y < be.y);
  CHECK(u.at("grain").y < be.y);
  CHECK_FALSE(u.shown("tones-grain"));
  // Pills are wider than tall and fit their label; the knob a disc of at least 32 px.
  CHECK(m.halfWidthOf(u.item("mode/Re-Pitch")) > m.radiusOf(u.item("mode/Re-Pitch")));
  CHECK(m.radiusOf(u.item("mode/Off")) < 16.0f);
  CHECK(m.radiusOf(u.item("transpose")) >= 32.0f);
  CHECK(m.radiusOf(u.item("transpose")) <= 42.0f);  // fits its label (estimated without a text system), never larger than on a ring
  // Below: Transpose then Reverse, one row.
  CHECK(u.at("transpose").y == doctest::Approx(u.at("reverse").y));
  CHECK(u.at("transpose").x < u.at("reverse").x);
  // No item overlaps another or the clip; all inside their band.
  std::vector<int32_t> shownItems;
  for (int32_t i = 0; i < m.itemCount(); ++i)
    if (m.shown(i)) shownItems.push_back(i);
  for (size_t i = 0; i < shownItems.size(); ++i) {
    const IRect bi = itemBox(m, shownItems[i]);
    CHECK_FALSE(overlaps(bi, clip));
    CHECK((overlaps(bi, a) || overlaps(bi, b)));
    for (size_t j = i + 1; j < shownItems.size(); ++j)
      CHECK_MESSAGE(!overlaps(itemBox(m, shownItems[j]), {bi.x + 1, bi.y + 1, bi.w - 2, bi.h - 2}),
                    m.labelOf(shownItems[i]) << " / " << m.labelOf(shownItems[j]));
  }
}

TEST_CASE("radial bands: hovering a chip opens nothing; choosing one swaps the settings row (one gesture)") {
  Ui u(900, 600, {200, 260, 400, 70});
  RadialMenu& m = u.open();
  u.d.move(u.at("mode/Tones"));
  CHECK_FALSE(u.shown("tones-grain"));
  CHECK(u.shown("grain"));
  u.d.click(u.at("mode/Tones"));
  CHECK(u.model.mode == 3);
  CHECK(u.model.begins == 1);
  CHECK(u.model.balanced());
  CHECK(u.shown("tones-grain"));
  CHECK_FALSE(u.shown("grain"));
  CHECK_FALSE(u.shown("preserve/1/8"));
  // Complex: no settings row.
  u.d.click(u.at("mode/Complex"));
  CHECK_FALSE(u.shown("tones-grain"));
  const IRect a = m.bandRect(0);
  CHECK(a.h < 50);
  // The document changing under it (another view) swaps the row too.
  u.model.mode = 2;
  u.opener->documentChanged();
  CHECK(u.shown("preserve/1/8"));
  // The settings row's controls edit: a chip, a slider drag (one gesture each), the menu stays open.
  u.d.click(u.at("preserve/1/16"));
  CHECK(u.model.preserve == 2);
  const Vec2 s0 = m.sliderStart(u.item("grain")), s1 = m.sliderEnd(u.item("grain"));
  const int b0 = u.model.begins;
  u.d.drag({s0.x + 1.0f, s0.y}, {s1.x, s1.y}, 5);
  CHECK(u.model.grain == doctest::Approx(100.0));
  CHECK(u.model.begins == b0 + 1);
  CHECK(u.model.balanced());
  CHECK(u.menu() == &m);
}

TEST_CASE("radial bands: a knob drag and a toggle (one gesture each); a press between items is consumed, outside closes") {
  Ui u(900, 600, {200, 260, 400, 70});
  RadialMenu& m = u.open();
  const Vec2 p = u.at("transpose");
  u.d.drag(p, {p.x, p.y - 50.0f}, 5);
  CHECK(u.model.transpose > 10.0);
  CHECK(u.model.begins == 1);
  CHECK(u.model.balanced());
  u.d.click(u.at("reverse"));
  CHECK(u.model.reverse);
  CHECK(m.litOf(u.item("reverse")));
  CHECK(u.model.begins == 2);
  // The panel's padding beside the Off pill: nothing happens, the menu stays.
  const IRect a = m.bandRect(0);
  const int presses = u.area->presses;
  u.d.click({static_cast<float>(a.x) + 2.0f, static_cast<float>(a.y) + 2.0f});
  CHECK(u.menu() == &m);
  // A press outside (even on the clip) closes it and is consumed.
  u.d.click(u.d.center("area/clip"));
  u.d.step();
  CHECK(u.menu() == nullptr);
  CHECK(u.area->presses == presses);
  CHECK(u.model.balanced());
}

TEST_CASE("radial bands: keys walk the shown items, Enter acts, Esc closes; typing does not filter") {
  Ui u(900, 600, {200, 260, 400, 70});
  RadialMenu& m = u.open();
  u.d.type("x");
  CHECK(m.filter().empty());
  u.d.press("Right");
  CHECK(m.highlighted() >= 0);
  // Walk to Re-Pitch (the shown order starts with the rows nearest the rectangle) and choose it.
  for (int i = 0; i < 20 && m.highlighted() != u.item("mode/Re-Pitch"); ++i) u.d.press("Right");
  REQUIRE(m.highlighted() == u.item("mode/Re-Pitch"));
  u.d.press("Enter");
  CHECK(u.model.mode == 1);
  CHECK_FALSE(u.shown("grain"));  // Re-Pitch has no settings
  CHECK(u.model.balanced());
  u.d.press("Left");
  CHECK(m.highlighted() == u.item("mode/Off"));
  u.d.press("Escape");
  u.d.step();
  CHECK(u.menu() == nullptr);
}

TEST_CASE("radial bands: at the window's top / bottom a band goes to the other side; a tiny window clamps; never off screen") {
  SUBCASE("top: the upper band goes below the lower one") {
    Ui u(900, 600, {200, 10, 400, 70});
    RadialMenu& m = u.open();
    const IRect clip = u.clip();
    CHECK(m.bandFlipped(0));
    CHECK_FALSE(m.bandFlipped(1));
    CHECK(m.bandRect(1).y >= clip.bottom());
    CHECK(m.bandRect(0).y >= m.bandRect(1).bottom());
    CHECK_FALSE(overlaps(m.bandRect(0), clip));
    CHECK(inside(m.bandRect(0), 900, 600));
    // Its rows still run outward: the settings row beyond the modes.
    CHECK(u.at("grain").y > u.at("mode/Beats").y);
  }
  SUBCASE("bottom: the lower band goes above the upper one") {
    Ui u(900, 600, {200, 520, 400, 70});
    RadialMenu& m = u.open();
    const IRect clip = u.clip();
    CHECK(m.bandFlipped(1));
    CHECK(m.bandRect(0).bottom() <= clip.y);
    CHECK(m.bandRect(1).bottom() <= m.bandRect(0).y);
    CHECK(inside(m.bandRect(1), 900, 600));
  }
  SUBCASE("left / right edges: clamped, the clip still clear") {
    Ui u(900, 600, {0, 260, 60, 70});
    RadialMenu& m = u.open({5.0f, 290.0f});
    CHECK(m.bandRect(0).x >= 0);
    CHECK(m.bandRect(1).x >= 0);
    CHECK_FALSE(overlaps(m.bandRect(0), u.clip()));
    CHECK_FALSE(overlaps(m.bandRect(1), u.clip()));
    u.d.click({1.0f, 590.0f});
    Ui v(900, 600, {840, 260, 60, 70});
    RadialMenu& n = v.open({895.0f, 290.0f});
    CHECK(n.bandRect(0).right() <= 900);
    CHECK(n.bandRect(1).right() <= 900);
  }
  SUBCASE("a clip as tall as the window: clamped inside it") {
    Ui u(900, 300, {200, 20, 400, 260});
    RadialMenu& m = u.open();
    CHECK(inside(m.bandRect(0), 900, 300));
    CHECK(inside(m.bandRect(1), 900, 300));
    CHECK(inside(m.bounds(), 900, 300));
  }
}

TEST_CASE("radial bands: no allocation per frame while open (hover over the items, painted)") {
  Ui u(900, 600, {200, 260, 400, 70}, true);
  u.open();
  auto script = [&] {
    for (const char* k : {"mode/Off", "mode/Beats", "preserve/1/8", "grain", "transpose", "reverse", "mode/Tones"}) {
      RadialMenu* m = u.menu();
      const int32_t i = m->find(k);
      if (i < 0 || !m->shown(i)) continue;
      u.d.move(m->centreOf(i));
      u.d.step();
    }
  };
  script();
  script();
  const uint64_t n = pq::alloc_trap::countRtViolations(script);
  CHECK(n == 0);
}

}  // TEST_SUITE
