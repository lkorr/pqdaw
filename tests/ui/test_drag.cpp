// T5.7: drag sessions (pq/ui/drag.h): internal drags from a captured source, external (OS) sessions, target search
// up the ancestors, dragLeave / drop / cancel (Esc, lost capture, a removed source), the ghost's damage, the cursor,
// auto-scroll near a ScrollView's edges.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "pq/ui/drag.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/widget.h"

using namespace pq::ui;

namespace {

// Starts a drag after the pointer moved 6 px with the button held.
struct Source : Widget {
  explicit Source(std::string n) : Widget(std::move(n)) { setSize(Length::fixed(100), Length::fixed(40)); }
  Vec2 pressAt{0, 0};
  bool pressed = false, started = false;
  int captureLost = 0;
  bool onPointer(const PointerEvent& e) override {
    if (e.type == PointerType::Down) {
      pressed = true;
      pressAt = e.pos;
      return true;
    }
    if (e.type == PointerType::Move && pressed && !started && std::abs(e.pos.x - pressAt.x) > 6.0f) {
      DragPayload p;
      p.kind = DragKind::BrowserItems;
      p.paths = {"C:/x/Kick.wav"};
      p.label = "Kick.wav";
      started = tree()->beginDrag(std::move(p));
    }
    if (e.type == PointerType::Up) pressed = false;
    return true;
  }
  void onCaptureLost() override {
    ++captureLost;
    pressed = false;
  }
};

struct Target : Widget {
  explicit Target(std::string n, DragKind accepts) : Widget(std::move(n)), accepts_(accepts) {
    setSize(Length::fixed(100), Length::fixed(40));
  }
  int overs = 0, leaves = 0, drops = 0;
  Mods lastMods = 0;
  std::vector<std::string> dropped;
  DropFeedback dragOver(const DragPayload& p, Vec2, Mods m) override {
    ++overs;
    lastMods = m;
    if (p.kind != accepts_) return {};
    return {(m & mod::kCtrl) ? DropEffect::Move : DropEffect::Copy, Cursor::Crosshair};
  }
  void dragLeave() override { ++leaves; }
  DropEffect drop(const DragPayload& p, Vec2, Mods) override {
    ++drops;
    dropped = p.paths;
    return DropEffect::Copy;
  }

private:
  DragKind accepts_;
};

struct Rig {
  TestDriver d{400, 300};
  Source* src = nullptr;
  Target* files = nullptr;
  Target* items = nullptr;
  Widget* inner = nullptr;
  Rig() {
    auto& root = d.setRoot<Widget>();
    root.setLayout(LayoutKind::Column);
    src = &root.add<Source>("src");
    files = &root.add<Target>("files", DragKind::Files);
    items = &root.add<Target>("items", DragKind::BrowserItems);
    inner = &items->add<Widget>("inner");  // refuses everything: its parent is the target
    d.tree().layoutNow();
  }
};

}  // namespace

TEST_CASE("drag: an internal drag targets the widget that accepts it and drops on release") {
  Rig r;
  const Vec2 s = r.d.center("src");
  r.d.down(s);
  r.d.move({s.x + 10, s.y});
  REQUIRE(r.src->started);
  REQUIRE(r.d.tree().dragging());
  CHECK(r.d.tree().dragPayload()->kind == DragKind::BrowserItems);
  // Over the Files target: refused; the cursor says so.
  r.d.move(r.d.center("files"));
  CHECK(r.d.tree().dropTarget() == nullptr);
  CHECK(r.d.tree().cursor() == Cursor::NotAllowed);
  CHECK(r.files->overs > 0);
  // Over the child of the items target: the child refuses, its parent accepts.
  r.d.move(r.d.center("items/inner"));
  CHECK(r.d.tree().dropTarget() == r.items);
  CHECK(r.d.tree().dropFeedback().effect == DropEffect::Copy);
  CHECK(r.d.tree().cursor() == Cursor::Crosshair);
  CHECK_FALSE(r.d.tree().dragGhostRect().empty());
  r.d.up(r.d.center("items/inner"));
  CHECK(r.items->drops == 1);
  CHECK(r.items->leaves == 0);  // no dragLeave after a drop
  REQUIRE(r.items->dropped.size() == 1);
  CHECK(r.items->dropped[0] == "C:/x/Kick.wav");
  CHECK_FALSE(r.d.tree().dragging());
  CHECK(r.d.tree().captured() == nullptr);
  CHECK(r.src->captureLost == 1);  // the source learns that its gesture ended
}

TEST_CASE("drag: moving off a target leaves it; Esc cancels without a drop") {
  Rig r;
  const Vec2 s = r.d.center("src");
  r.d.down(s);
  r.d.move({s.x + 10, s.y});
  r.d.move(r.d.center("items"));
  CHECK(r.d.tree().dropTarget() == r.items);
  r.d.move(r.d.center("src"));
  CHECK(r.items->leaves == 1);
  CHECK(r.d.tree().dropTarget() == nullptr);
  r.d.move(r.d.center("items"));
  CHECK(r.d.press("Escape") == KeyResult::Widget);
  CHECK(r.items->leaves == 2);
  CHECK(r.items->drops == 0);
  CHECK_FALSE(r.d.tree().dragging());
  r.d.up(r.d.center("items"));
  CHECK(r.items->drops == 0);
}

TEST_CASE("drag: Ctrl pressed during an internal drag retargets with the new modifiers") {
  Rig r;
  const Vec2 s = r.d.center("src");
  r.d.down(s);
  r.d.move({s.x + 10, s.y});
  r.d.move(r.d.center("items"));
  CHECK(r.d.tree().dropFeedback().effect == DropEffect::Copy);
  r.d.keyDown(key::kLCtrl, mod::kCtrl);
  CHECK(r.d.tree().dropFeedback().effect == DropEffect::Move);
  r.d.keyUp(key::kLCtrl);
  CHECK(r.d.tree().dropFeedback().effect == DropEffect::Copy);
}

TEST_CASE("drag: a lost capture (window focus) or a removed source cancels the session") {
  Rig r;
  const Vec2 s = r.d.center("src");
  r.d.down(s);
  r.d.move({s.x + 10, s.y});
  r.d.move(r.d.center("items"));
  r.d.tree().cancelInteraction();
  CHECK_FALSE(r.d.tree().dragging());
  CHECK(r.items->leaves == 1);
  r.src->started = false;
  r.d.down(s);
  r.d.move({s.x + 10, s.y});
  REQUIRE(r.d.tree().dragging());
  r.d.move(r.d.center("items"));
  (void)r.d.root()->removeChild(*r.src);
  CHECK_FALSE(r.d.tree().dragging());
  CHECK(r.items->leaves == 2);
}

TEST_CASE("drag: beginDrag needs a pointer gesture") {
  Rig r;
  DragPayload p;
  CHECK_FALSE(r.d.tree().beginDrag(p));
  CHECK_FALSE(r.d.tree().dragging());
}

TEST_CASE("drag: an external session (OS files) enters, moves, drops; an exit leaves the target") {
  Rig r;
  DragPayload p;
  p.kind = DragKind::Files;
  p.paths = {"C:/a.wav", "C:/b.wav"};
  p.label = "2 files";
  r.d.tree().dragEnter(p, r.d.center("src"), 0);
  CHECK(r.d.tree().dragging());
  CHECK(r.d.tree().dragPayload()->external);
  CHECK(r.d.tree().dropTarget() == nullptr);
  CHECK(r.d.tree().dragMove(r.d.center("files"), mod::kCtrl) == DropEffect::Move);
  CHECK(r.files->lastMods == mod::kCtrl);
  r.d.tree().dragExit();
  CHECK(r.files->leaves == 1);
  CHECK_FALSE(r.d.tree().dragging());
  r.d.tree().dragEnter(p, r.d.center("files"), 0);
  CHECK(r.d.tree().dragDrop(r.d.center("files"), 0) == DropEffect::Copy);
  CHECK(r.files->drops == 1);
  CHECK(r.files->dropped.size() == 2);
  CHECK(r.files->leaves == 1);
  CHECK_FALSE(r.d.tree().dragging());
  // Dropped where nothing accepts: nothing happens.
  r.d.tree().dragEnter(p, r.d.center("items"), 0);
  CHECK(r.d.tree().dragDrop(r.d.center("items"), 0) == DropEffect::None);
  CHECK(r.items->drops == 0);
}

TEST_CASE("drag: the ghost is damaged where it was and where it goes, and painted over the tree") {
  Rig r;
  DragPayload p;
  p.kind = DragKind::Files;
  p.label = "Kick.wav";
  r.d.step();
  r.d.tree().dragEnter(p, {50, 50}, 0);
  const IRect g0 = r.d.tree().dragGhostRect();
  CHECK_FALSE(g0.empty());
  CHECK(r.d.tree().needsPaint());
  r.d.step();
  CHECK_FALSE(r.d.tree().needsPaint());
  r.d.tree().dragMove({120, 90}, 0);
  const IRect g1 = r.d.tree().dragGhostRect();
  CHECK_FALSE(g1 == g0);
  CHECK(r.d.tree().needsPaint());
  r.d.step();
  r.d.tree().dragExit();
  CHECK(r.d.tree().needsPaint());  // the ghost's last place is repainted
}

TEST_CASE("drag: resting near a ScrollView's edge scrolls it until the pointer leaves the edge") {
  TestDriver d(300, 200);
  auto& root = d.setRoot<Widget>();
  auto& sv = root.add<ScrollView>("sv", false, true);
  auto& content = sv.add<Target>("content", DragKind::Files);
  content.setSize(Length::fixed(300), Length::fixed(2000));
  d.tree().layoutNow();
  DragPayload p;
  p.kind = DragKind::Files;
  d.tree().dragEnter(p, {150, 195}, 0);
  CHECK(d.tree().needsFrame());
  d.advance(0.25);
  const int32_t y1 = sv.scrollY();
  CHECK(y1 > 50);
  d.tree().dragMove({150, 100}, 0);  // the middle: no more scrolling
  d.advance(0.25);
  CHECK(sv.scrollY() == y1);
  CHECK_FALSE(d.tree().needsFrame());
  d.tree().dragMove({150, 3}, 0);    // the top edge scrolls back
  d.advance(0.25);
  CHECK(sv.scrollY() < y1);
  d.tree().dragExit();
}
