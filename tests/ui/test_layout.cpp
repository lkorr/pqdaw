// T3.1 layout tables: exact device-pixel rects for row / column / stack / splitter cases, incl. min/max
// resolution, integer snapping of flex shares, padding, gaps, alignment and non-1 UI scales.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "pq/ui/test_driver.h"
#include "pq/ui/widget.h"

using namespace pq::ui;

namespace doctest {
template <>
struct StringMaker<IRect> {
  static String convert(const IRect& r) {
    return ("{" + std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," + std::to_string(r.h) + "}").c_str();
  }
};
}  // namespace doctest

namespace {

struct Kid {
  Length w = Length::flex();
  Length h = Length::flex();
  bool visible = true;
};

struct LinearCase {
  const char* name;
  LayoutKind kind;
  uint32_t width, height;
  float scale;
  Insets padding;
  int16_t gap;
  Align main, cross;
  std::vector<Kid> kids;
  std::vector<IRect> expect;  // one per kid (invisible kids: not checked, use {})
};

Length F(float px) { return Length::fixed(px); }
Length X(float w = 1, int32_t mn = 0, int32_t mx = kUnbounded) { return Length::flex(w, mn, mx); }

const std::vector<LinearCase>& linearCases() {
  static const std::vector<LinearCase> cases = {
      {"row: fixed widths, flex height stretches", LayoutKind::Row, 300, 40, 1, {}, 0, Align::Start, Align::Start,
       {{F(50)}, {F(60)}, {F(70)}}, {{0, 0, 50, 40}, {50, 0, 60, 40}, {110, 0, 70, 40}}},
      {"row: equal flex", LayoutKind::Row, 300, 40, 1, {}, 0, Align::Start, Align::Start,
       {{X()}, {X()}, {X()}}, {{0, 0, 100, 40}, {100, 0, 100, 40}, {200, 0, 100, 40}}},
      {"row: flex weights 1:2", LayoutKind::Row, 300, 40, 1, {}, 0, Align::Start, Align::Start,
       {{X(1)}, {X(2)}}, {{0, 0, 100, 40}, {100, 0, 200, 40}}},
      {"row: thirds of 100 snap by running sum (33/34/33)", LayoutKind::Row, 100, 10, 1, {}, 0, Align::Start, Align::Start,
       {{X()}, {X()}, {X()}}, {{0, 0, 33, 10}, {33, 0, 34, 10}, {67, 0, 33, 10}}},
      {"row: fixed + flex + fixed", LayoutKind::Row, 300, 40, 1, {}, 0, Align::Start, Align::Start,
       {{F(80)}, {X()}, {F(40)}}, {{0, 0, 80, 40}, {80, 0, 180, 40}, {260, 0, 40, 40}}},
      {"row: padding and gap", LayoutKind::Row, 320, 50, 1, Insets::all(5), 10, Align::Start, Align::Start,
       {{X()}, {X()}, {X()}}, {{5, 5, 97, 40}, {112, 5, 96, 40}, {218, 5, 97, 40}}},
      {"row: flex min wins", LayoutKind::Row, 300, 20, 1, {}, 0, Align::Start, Align::Start,
       {{X(1, 200)}, {X(1)}}, {{0, 0, 200, 20}, {200, 0, 100, 20}}},
      {"row: flex max caps, rest re-shared", LayoutKind::Row, 300, 20, 1, {}, 0, Align::Start, Align::Start,
       {{X(1, 0, 50)}, {X(1)}}, {{0, 0, 50, 20}, {50, 0, 250, 20}}},
      {"row: max and min violations resolved in turn", LayoutKind::Row, 300, 20, 1, {}, 0, Align::Start, Align::Start,
       {{X(1, 0, 40)}, {X(1)}, {X(1, 150)}}, {{0, 0, 40, 20}, {40, 0, 110, 20}, {150, 0, 150, 20}}},
      {"column: header / body / footer", LayoutKind::Column, 200, 300, 1, {}, 0, Align::Start, Align::Start,
       {{X(), F(30)}, {X(), X()}, {X(), F(20)}}, {{0, 0, 200, 30}, {0, 30, 200, 250}, {0, 280, 200, 20}}},
      {"column: fixed width centred on the cross axis", LayoutKind::Column, 300, 100, 1, {}, 0, Align::Start, Align::Center,
       {{F(100), F(40)}}, {{100, 0, 100, 40}}},
      {"row: fixed height at cross end", LayoutKind::Row, 100, 40, 1, {}, 0, Align::Start, Align::End,
       {{F(20), F(10)}}, {{0, 30, 20, 10}}},
      {"row: fixed children centred on the main axis", LayoutKind::Row, 300, 20, 1, {}, 0, Align::Center, Align::Start,
       {{F(50)}, {F(50)}}, {{100, 0, 50, 20}, {150, 0, 50, 20}}},
      {"row: fixed children at the main end", LayoutKind::Row, 300, 20, 1, {}, 0, Align::End, Align::Start,
       {{F(50)}, {F(50)}}, {{200, 0, 50, 20}, {250, 0, 50, 20}}},
      {"row: overflow leaves flex at 0", LayoutKind::Row, 300, 20, 1, {}, 0, Align::Start, Align::Start,
       {{F(200)}, {X()}, {F(200)}}, {{0, 0, 200, 20}, {200, 0, 0, 20}, {200, 0, 200, 20}}},
      {"row: invisible child takes no space or gap", LayoutKind::Row, 300, 20, 1, {}, 5, Align::Start, Align::Start,
       {{F(50)}, {F(60), X(), false}, {F(70)}}, {{0, 0, 50, 20}, {}, {55, 0, 70, 20}}},
      {"row: flex cross with max is aligned", LayoutKind::Row, 100, 40, 1, {}, 0, Align::Start, Align::Center,
       {{F(10), X(1, 0, 20)}}, {{0, 10, 10, 20}}},
      {"column: thirds with gap 1", LayoutKind::Column, 10, 100, 1, {}, 1, Align::Start, Align::Start,
       {{X(), X()}, {X(), X()}, {X(), X()}}, {{0, 0, 10, 33}, {0, 34, 10, 32}, {0, 67, 10, 33}}},
      {"stack: flex fills padded content, fixed centred", LayoutKind::Stack, 200, 100, 1, Insets::all(10), 0, Align::Center, Align::Center,
       {{X(), X()}, {F(40), F(20)}}, {{10, 10, 180, 80}, {80, 40, 40, 20}}},
      {"stack: fixed at end/end", LayoutKind::Stack, 200, 100, 1, {}, 0, Align::End, Align::End,
       {{F(40), F(20)}}, {{160, 80, 40, 20}}},
      {"stack: flex width capped by max, centred", LayoutKind::Stack, 200, 100, 1, {}, 0, Align::Center, Align::Start,
       {{X(1, 0, 50), F(10)}}, {{75, 0, 50, 10}}},
      {"scale 1.5: logical lengths, padding and gap scale and snap", LayoutKind::Row, 300, 60, 1.5f, Insets::all(4), 2, Align::Start, Align::Start,
       {{F(50)}, {X()}}, {{6, 6, 75, 48}, {84, 6, 210, 48}}},
      {"scale 1.25: 13 px rounds to 16, min scales", LayoutKind::Row, 200, 20, 1.25f, {}, 0, Align::Start, Align::Start,
       {{F(13)}, {X(1, 100)}, {X(1)}}, {{0, 0, 16, 20}, {16, 0, 125, 20}, {141, 0, 59, 20}}},
      {"scale 2: column of fixed rows", LayoutKind::Column, 100, 200, 2.0f, {}, 1, Align::Start, Align::Start,
       {{X(), F(22)}, {X(), F(22)}}, {{0, 0, 100, 44}, {0, 46, 100, 44}}},
  };
  return cases;
}

}  // namespace

TEST_CASE("layout: row / column / stack table") {
  int checked = 0;
  for (const LinearCase& c : linearCases()) {
    CAPTURE(c.name);
    TestDriver d(c.width, c.height, c.scale);
    Widget& root = d.setRoot<Widget>();
    root.setLayout(c.kind).setPadding(c.padding).setGap(c.gap).setAlign(c.main, c.cross);
    std::vector<Widget*> kids;
    for (size_t i = 0; i < c.kids.size(); ++i) {
      Widget& k = root.add<Widget>("k" + std::to_string(i));
      k.setSize(c.kids[i].w, c.kids[i].h);
      k.setVisible(c.kids[i].visible);
      kids.push_back(&k);
    }
    d.tree().layoutNow();
    REQUIRE(c.expect.size() == kids.size());
    for (size_t i = 0; i < kids.size(); ++i) {
      CAPTURE(i);
      if (!c.kids[i].visible) continue;
      CHECK(kids[i]->rect() == c.expect[i]);
    }
    ++checked;
  }
  CHECK(checked >= 20);
}

namespace {

struct SplitCase {
  const char* name;
  Splitter::Axis axis;
  Splitter::Anchor anchor;
  uint32_t width, height;
  float scale;
  float position;
  int32_t minA, maxA, minOther;
  int32_t thickness;
  bool showA, showB;
  IRect a, handle, b;
};

}  // namespace

TEST_CASE("layout: splitter table") {
  using A = Splitter::Axis;
  using N = Splitter::Anchor;
  const SplitCase cases[] = {
      {"horizontal at 100", A::Horizontal, N::First, 400, 100, 1, 100, 0, kUnbounded, 0, 4, true, true,
       {0, 0, 100, 100}, {100, 0, 4, 100}, {104, 0, 296, 100}},
      {"position clamped to max", A::Horizontal, N::First, 400, 100, 1, 350, 50, 300, 0, 4, true, true,
       {0, 0, 300, 100}, {300, 0, 4, 100}, {304, 0, 96, 100}},
      {"position clamped to min", A::Horizontal, N::First, 400, 100, 1, 10, 50, 300, 0, 4, true, true,
       {0, 0, 50, 100}, {50, 0, 4, 100}, {54, 0, 346, 100}},
      {"other pane keeps its minimum", A::Horizontal, N::First, 400, 100, 1, 300, 0, 1000, 150, 4, true, true,
       {0, 0, 246, 100}, {246, 0, 4, 100}, {250, 0, 150, 100}},
      {"anchored minimum beats the other's minimum", A::Horizontal, N::First, 400, 100, 1, 100, 300, 1000, 200, 4, true, true,
       {0, 0, 300, 100}, {300, 0, 4, 100}, {304, 0, 96, 100}},
      {"vertical anchored at the end (detail view)", A::Vertical, N::Second, 200, 400, 1, 100, 0, kUnbounded, 0, 4, true, true,
       {0, 0, 200, 296}, {0, 296, 200, 4}, {0, 300, 200, 100}},
      {"anchored end yields to the other's minimum", A::Vertical, N::Second, 200, 400, 1, 350, 0, 1000, 120, 4, true, true,
       {0, 0, 200, 120}, {0, 120, 200, 4}, {0, 124, 200, 276}},
      {"scale 1.5", A::Horizontal, N::First, 600, 90, 1.5f, 100, 0, kUnbounded, 0, 4, true, true,
       {0, 0, 150, 90}, {150, 0, 6, 90}, {156, 0, 444, 90}},
      {"second pane hidden: first fills", A::Horizontal, N::First, 400, 100, 1, 100, 0, kUnbounded, 0, 4, true, false,
       {0, 0, 400, 100}, {}, {}},
      {"first pane hidden: second fills", A::Vertical, N::Second, 200, 300, 1, 100, 0, kUnbounded, 0, 4, false, true,
       {}, {}, {0, 0, 200, 300}},
  };
  for (const SplitCase& c : cases) {
    CAPTURE(c.name);
    TestDriver d(c.width, c.height, c.scale);
    auto& s = d.setRoot<Splitter>("split", c.axis, c.position, c.anchor);
    s.setLimits(c.minA, c.maxA, c.minOther);
    s.setPosition(c.position);
    s.setHandleThickness(c.thickness);
    Widget& a = s.add<Widget>("a");
    Widget& b = s.add<Widget>("b");
    a.setVisible(c.showA);
    b.setVisible(c.showB);
    d.tree().layoutNow();
    if (c.showA) CHECK(a.rect() == c.a);
    if (c.showB) CHECK(b.rect() == c.b);
    CHECK(s.handleRect() == c.handle);
  }
}

TEST_CASE("layout: splitter drag moves the handle within its limits") {
  TestDriver d(400, 100);
  auto& s = d.setRoot<Splitter>("split", Splitter::Axis::Horizontal, 100.0f);
  s.setLimits(50, 300, 60);
  Widget& a = s.add<Widget>("a");
  Widget& b = s.add<Widget>("b");
  d.tree().layoutNow();
  const Vec2 h{102, 50};
  d.drag(h, {h.x + 50, h.y});
  CHECK(s.position() == 150.0f);
  CHECK(a.rect() == IRect{0, 0, 150, 100});
  CHECK(b.rect() == IRect{154, 0, 246, 100});
  d.drag({152, 50}, {800, 50});  // far past the end: max 300, but b keeps 60 → 400-4-60 = 336 > 300, so 300
  CHECK(s.position() == 300.0f);
  CHECK(a.rect().w == 300);
  d.drag({302, 50}, {-500, 50});
  CHECK(s.position() == 50.0f);
  // Pressing beside the handle does not start a drag.
  d.drag({200, 50}, {250, 50});
  CHECK(s.position() == 50.0f);
}

TEST_CASE("layout: relayout only visits what changed") {
  TestDriver d(400, 400);
  Widget& root = d.setRoot<Widget>();
  root.setLayout(LayoutKind::Column);
  std::vector<Widget*> rows;
  for (int r = 0; r < 10; ++r) {
    Widget& row = root.add<Widget>("row[" + std::to_string(r) + "]");
    row.setLayout(LayoutKind::Row);
    for (int c = 0; c < 10; ++c) row.add<Widget>("c" + std::to_string(c));
    rows.push_back(&row);
  }
  d.tree().layoutNow();
  const uint64_t before = d.tree().stats().widgetsArranged;
  // A width change inside row 3 re-arranges row 3 and its 10 children only.
  rows[3]->child(0).setWidth(Length::fixed(10));
  d.tree().layoutNow();
  CHECK(d.tree().stats().widgetsArranged - before == 11);
  CHECK(rows[3]->child(0).rect() == IRect{0, 120, 10, 40});
  CHECK(rows[3]->child(1).rect() == IRect{10, 120, 43, 40});  // 390 / 9 = 43.33: running-sum snap
  // Nothing dirty: no work.
  const uint64_t after = d.tree().stats().widgetsArranged;
  d.tree().layoutNow();
  CHECK(d.tree().stats().widgetsArranged == after);
}
