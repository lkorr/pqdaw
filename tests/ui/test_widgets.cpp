// T3.1 scripted TestDriver tests: pointer (click, double-click, capture, hover, wheel), focus / Tab order, key
// routing precedence (widgets → modal scopes → focus scopes → global; text fields swallow printable keys),
// damage, idle, timers, virtualisation, ids, and allocation-free dispatch.

#include <doctest/doctest.h>

#include <limits>
#include <ostream>  // doctest prints std::string_view through operator<<
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/ui/actions.h"
#include "pq/ui/image.h"
#include "pq/ui/input.h"
#include "pq/ui/scenes.h"
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

// Records what it receives. Handles pointer Down (so it captures) unless told not to.
class Probe : public Widget {
public:
  explicit Probe(std::string name, bool handlesDown = true) : Widget(std::move(name)), handlesDown_(handlesDown) {}
  int downs = 0, ups = 0, moves = 0, wheels = 0, enters = 0, leaves = 0, focusIn = 0, focusOut = 0, captureLost = 0;
  int keys = 0, timers = 0, frames = 0;
  uint8_t lastClicks = 0;
  Vec2 lastMove{0, 0};
  Key handlesKey = key::kUnknown;  // onKey consumes this key
  std::string text;
  int animateFrames = 0;  // keep requesting frames while > 0

  bool onPointer(const PointerEvent& e) override {
    switch (e.type) {
      case PointerType::Down:
        ++downs;
        lastClicks = e.clickCount;
        return handlesDown_;
      case PointerType::Up: ++ups; return handlesDown_;
      case PointerType::Move:
        ++moves;
        lastMove = e.pos;
        return false;
      case PointerType::Wheel: ++wheels; return false;
      default: return false;
    }
  }
  bool onKey(const KeyEvent& e) override {
    if (e.down && e.key == handlesKey) {
      ++keys;
      return true;
    }
    return false;
  }
  bool onText(const TextEvent& e) override {
    text += e.text();
    return true;
  }
  void onHoverChanged(bool h) override { ++(h ? enters : leaves); }
  void onFocusChanged(bool f) override { ++(f ? focusIn : focusOut); }
  void onCaptureLost() override { ++captureLost; }
  void onTimer(uint32_t) override { ++timers; }
  void onFrame(double) override {
    ++frames;
    markDirty();
    if (--animateFrames > 0) requestFrame();
  }

private:
  bool handlesDown_;
};

Probe& probe(Widget& parent, std::string name, Length w = Length::flex(), Length h = Length::flex()) {
  Probe& p = parent.add<Probe>(std::move(name));
  p.setSize(w, h);
  return p;
}

}  // namespace

TEST_CASE("widgets: click and double-click reach the widget under the pointer") {
  TestDriver d(300, 100);
  Widget& root = d.setRoot<Widget>();
  root.setLayout(LayoutKind::Row);
  Probe& a = probe(root, "a", Length::fixed(100));
  Probe& b = probe(root, "b", Length::fixed(100));
  d.click(d.center("a"));
  CHECK(a.downs == 1);
  CHECK(a.ups == 1);
  CHECK(a.lastClicks == 1);
  CHECK(b.downs == 0);
  CHECK(d.tree().captured() == nullptr);
  d.doubleClick(d.center("b"));
  CHECK(b.downs == 2);
  CHECK(b.ups == 2);
  CHECK(b.lastClicks == 2);
  CHECK(a.downs == 1);
  // Empty space (root, which handles nothing): no capture.
  d.click({250, 50});
  CHECK(d.tree().captured() == nullptr);
}

TEST_CASE("widgets: drag captures the pointer outside the widget; hover leaves on release") {
  TestDriver d(300, 100);
  Widget& root = d.setRoot<Widget>();
  root.setLayout(LayoutKind::Row);
  Probe& a = probe(root, "a", Length::fixed(100));
  Probe& b = probe(root, "b", Length::fixed(100));
  const Vec2 pa = d.center("a"), pb = d.center("b");
  d.move(pa);
  CHECK(a.hovered());
  CHECK(a.enters == 1);
  d.down(pa);
  CHECK(d.tree().captured() == &a);
  CHECK(a.captured());
  d.move({pa.x + 60, pa.y});
  d.move(pb);
  d.move({280, 90});  // over the root, outside both
  CHECK(a.moves == 4);  // the initial hover move + three while captured
  CHECK(a.lastMove.x == 280.0f);
  CHECK(b.moves == 0);
  CHECK(b.enters == 0);  // hover is frozen on the captured widget
  CHECK(a.hovered());
  CHECK(a.leaves == 0);
  d.up(pb);
  CHECK(a.ups == 1);
  CHECK(b.ups == 0);
  CHECK(d.tree().captured() == nullptr);
  CHECK_FALSE(a.captured());
  // Release outside: a gets its leave, b (now under the pointer) its enter.
  CHECK(a.leaves == 1);
  CHECK_FALSE(a.hovered());
  CHECK(b.enters == 1);
  CHECK(d.tree().hovered() == &b);
  // Pointer leaves the window: hover ends.
  d.leave();
  CHECK(b.leaves == 1);
  CHECK(d.tree().hovered() == nullptr);
}

TEST_CASE("widgets: an unhandled Down bubbles to the ancestor that handles it, which captures") {
  TestDriver d(200, 100);
  Probe& parent = d.setRoot<Probe>("root");
  Probe& leaf = parent.add<Probe>("leaf", false);
  leaf.setSize(Length::fixed(50), Length::fixed(50));
  d.down({10, 10});
  CHECK(leaf.downs == 1);
  CHECK(parent.downs == 1);
  CHECK(d.tree().captured() == &parent);
  CHECK(d.tree().hovered() == &leaf);
  d.up({10, 10});
  CHECK(d.tree().captured() == nullptr);
}

TEST_CASE("widgets: removing a captured, hovered, focused widget clears the tree's pointers") {
  TestDriver d(200, 100);
  Widget& root = d.setRoot<Widget>();
  Probe& p = probe(root, "p");
  p.setFocusable(true);
  d.down({20, 20});
  CHECK(d.tree().captured() == &p);
  CHECK(d.tree().focus() == &p);
  std::unique_ptr<Widget> gone = root.removeChild(p);
  CHECK(static_cast<Probe&>(*gone).captureLost == 1);
  CHECK(static_cast<Probe&>(*gone).focusOut == 1);
  CHECK(d.tree().captured() == nullptr);
  CHECK(d.tree().hovered() == nullptr);
  CHECK(d.tree().focus() == nullptr);
  d.move({30, 30});
  d.up({30, 30});
  CHECK(d.tree().hovered() == &root);
}

TEST_CASE("widgets: wheel scrolls a scroll view, clamped, Shift for horizontal") {
  TestDriver d(200, 100);
  Widget& root = d.setRoot<Widget>();
  auto& sv = root.add<ScrollView>("sv");
  Widget& content = sv.add<Widget>("content");
  content.setSize(Length::fixed(500), Length::fixed(1000));
  d.tree().layoutNow();
  CHECK(sv.contentSize() == ISize{500, 1000});
  const Vec2 p{100, 50};
  CHECK(d.wheel(p, -1.0f));  // one notch towards the user: down by the 48 px step
  CHECK(sv.scrollY() == 48);
  CHECK(content.rect() == IRect{0, -48, 500, 1000});
  CHECK(d.wheel(p, 1.0f));
  CHECK(sv.scrollY() == 0);
  CHECK_FALSE(d.wheel(p, 1.0f));  // already at the top: unhandled (an outer view may scroll)
  d.wheel(p, -100.0f);
  CHECK(sv.scrollY() == 900);  // 1000 - 100
  d.wheel(p, -1.0f, 0.0f, mod::kShift);
  CHECK(sv.scrollX() == 48);
  d.wheel(p, 0.0f, 100.0f);
  CHECK(sv.scrollX() == 300);  // 500 - 200
  CHECK(d.widgetAt({100, 50}) == &content);
}

namespace {

// Virtualised content: 20 px rows, paints only the rows intersecting the clip.
class RowList : public Widget {
public:
  explicit RowList(std::string name) : Widget(std::move(name)) {}
  static constexpr int kRows = 10000;
  int first = -1, last = -1, painted = 0;
  ISize contentExtent(ISize viewport) const override { return {viewport.w, kRows * 20}; }
  void paint(PaintContext& ctx) override {
    const IRect vis = intersect(rect(), ctx.clip);
    first = (vis.y - rect().y) / 20;
    last = (vis.bottom() - 1 - rect().y) / 20;
    painted = 0;
    for (int r = first; r <= last; ++r, ++painted)
      ctx.dl.addRectFilled({static_cast<float>(vis.x), static_cast<float>(rect().y + r * 20), static_cast<float>(vis.right()),
                            static_cast<float>(rect().y + r * 20 + 19)}, rgba(80, 80, 90));
  }
};

}  // namespace

TEST_CASE("widgets: scroll view content paints only the visible range") {
  TestDriver d(200, 100);
  Widget& root = d.setRoot<Widget>();
  root.setLayout(LayoutKind::Column);
  probe(root, "header", Length::flex(), Length::fixed(20));
  auto& sv = root.add<ScrollView>("sv", false, true);
  RowList& list = sv.add<RowList>("rows");
  d.step();
  CHECK(sv.contentSize() == ISize{200, 200000});
  CHECK(list.first == 0);
  CHECK(list.last == 3);  // 80 px viewport
  CHECK(list.painted == 4);
  sv.scrollTo(0, 5010);
  d.step();
  CHECK(list.first == 250);
  CHECK(list.last == 254);  // a partly visible row at each end
  CHECK(list.painted == 5);
  CHECK(d.drawList().vertices().size() < 200);  // header + a handful of rows, not 10,000
}

TEST_CASE("widgets: Tab order follows the tree, skips hidden/disabled/unfocusable, wraps") {
  TestDriver d(400, 400);
  Widget& root = d.setRoot<Widget>();
  root.setLayout(LayoutKind::Column);
  Probe& f1 = probe(root, "f1");
  probe(root, "plain");
  Widget& hidden = root.add<Widget>("hidden");
  Probe& fh = probe(hidden, "fh");
  hidden.setVisible(false);
  Probe& f2 = probe(root, "f2");
  Probe& f3 = probe(root, "f3");
  Widget& group = root.add<Widget>("group");
  Probe& f4 = probe(group, "f4");
  for (Probe* p : {&f1, &fh, &f2, &f3, &f4}) p->setFocusable(true);
  f2.setEnabled(false);

  CHECK(d.press("Tab") == KeyResult::Focus);
  CHECK(d.tree().focus() == &f1);
  d.press("Tab");
  CHECK(d.tree().focus() == &f3);
  d.press("Tab");
  CHECK(d.tree().focus() == &f4);
  d.press("Tab");
  CHECK(d.tree().focus() == &f1);  // wrapped
  d.press("Shift+Tab");
  CHECK(d.tree().focus() == &f4);
  d.press("Shift+Tab");
  CHECK(d.tree().focus() == &f3);
  CHECK(f3.focused());
  CHECK(f1.focusIn == 2);
  CHECK(f1.focusOut == 2);
  // Clicking a focusable widget focuses it; clicking a non-focusable area focuses its focusable ancestor
  // or nothing.
  d.click(d.center("f1"));
  CHECK(d.tree().focus() == &f1);
  d.click(d.center("plain"));
  CHECK(d.tree().focus() == nullptr);
}

namespace {

struct Routing {
  ActionRegistry actions;
  Keymap keymap;
  int draw = 0, globalB = 0, dup = 0, pianoB = 0, macroOne = 0, nudge = 0;
  bool drawEnabled = true;
  ScopeId arr = 0, piano = 0, macro = 0;

  Routing() {
    REQUIRE(actions.add("view.draw_mode", [this] { ++draw; }, [this] { return drawEnabled; }).ok());
    REQUIRE(actions.add("global.b", [this] { ++globalB; }).ok());
    REQUIRE(actions.add("edit.duplicate", [this] { ++dup; }).ok());
    REQUIRE(actions.add("pianoroll.p", [this] { ++pianoB; }).ok());
    REQUIRE(actions.add("macro.mul3_2", [this] { ++macroOne; }).ok());
    REQUIRE(actions.add("edit.nudge_up", [this] { ++nudge; }, {}, {true}).ok());
    arr = keymap.scope("arrangement");
    piano = keymap.scope("pianoroll");
    macro = keymap.scope("macro");
    keymap.setRules(macro, {mod::kCtrl | mod::kAlt, true});
    REQUIRE(keymap.bind("arrangement", "B", "view.draw_mode").ok());
    REQUIRE(keymap.bind("global", "B", "global.b").ok());
    REQUIRE(keymap.bind("arrangement", "Ctrl+D", "edit.duplicate").ok());
    REQUIRE(keymap.bind("arrangement", "Up", "edit.nudge_up").ok());
    REQUIRE(keymap.bind("pianoroll", "P", "pianoroll.p").ok());
    REQUIRE(keymap.bind("macro", "2", "macro.mul3_2").ok());
    REQUIRE(keymap.bind("global", "F9", "deferred:M5").ok());  // unresolved id: harmless
  }
};

}  // namespace

TEST_CASE("widgets: key routing precedence") {
  Routing R;
  TestDriver d(400, 300);
  Widget& root = d.setRoot<Widget>();
  root.setLayout(LayoutKind::Row);
  Probe& arrangement = probe(root, "arrangement");
  arrangement.setKeyScope(R.arr);
  arrangement.setFocusable(true);
  arrangement.handlesKey = 'k';
  arrangement.setLayout(LayoutKind::Column);
  Probe& clip = probe(arrangement, "clip");
  clip.setFocusable(true);
  clip.handlesKey = 'x';
  Probe& name = probe(arrangement, "name");
  name.setFocusable(true);
  name.setAcceptsTextInput(true);
  Probe& pianoroll = probe(root, "pianoroll");
  pianoroll.setKeyScope(R.piano);
  pianoroll.setFocusable(true);
  d.tree().setKeymap(&R.actions, &R.keymap);

  SUBCASE("no focus: only global bindings") {
    CHECK(d.press("B") == KeyResult::Action);
    CHECK(R.globalB == 1);
    CHECK(R.draw == 0);
    CHECK(d.press("Ctrl+D") == KeyResult::Unhandled);
    CHECK(d.press("F9") == KeyResult::Unhandled);
  }
  SUBCASE("focused widget, then its ancestors, then scopes") {
    d.tree().setFocus(&clip);
    CHECK(d.press("X") == KeyResult::Widget);
    CHECK(clip.keys == 1);
    CHECK(d.press("K") == KeyResult::Widget);  // the ancestor's own handler
    CHECK(arrangement.keys == 1);
    CHECK(d.press("B") == KeyResult::Action);  // arrangement scope beats global
    CHECK(R.draw == 1);
    CHECK(R.globalB == 0);
    CHECK(d.press("Ctrl+D") == KeyResult::Action);
    CHECK(R.dup == 1);
  }
  SUBCASE("a text field swallows printable keys but not command chords") {
    d.tree().setFocus(&name);
    CHECK(d.press("B") == KeyResult::TextSwallowed);
    CHECK(R.draw == 0);
    CHECK(R.globalB == 0);
    CHECK(d.press("Shift+B") == KeyResult::TextSwallowed);
    CHECK(d.type("b") == true);
    CHECK(name.text == "b");
    CHECK(d.press("Ctrl+D") == KeyResult::Action);
    CHECK(R.dup == 1);
    // Text goes only to a focused text-entry widget.
    d.tree().setFocus(&clip);
    CHECK(d.type("q") == false);
    CHECK(clip.text.empty());
  }
  SUBCASE("a chord in an inactive scope does nothing") {
    d.tree().setFocus(&clip);
    CHECK(d.press("P") == KeyResult::Unhandled);
    CHECK(R.pianoB == 0);
    d.tree().setFocus(&pianoroll);
    CHECK(d.press("P") == KeyResult::Action);
    CHECK(R.pianoB == 1);
    CHECK(d.press("Ctrl+D") == KeyResult::Unhandled);  // arrangement scope inactive now
    CHECK(R.dup == 0);
  }
  SUBCASE("modal macro scope: takes plain keys, passes Ctrl/Alt chords through") {
    d.tree().setFocus(&clip);
    d.tree().pushModalScope(R.macro);
    CHECK(d.press("2") == KeyResult::Action);
    CHECK(R.macroOne == 1);
    CHECK(d.press("B") == KeyResult::ScopeSwallowed);
    CHECK(R.draw == 0);
    CHECK(R.globalB == 0);
    CHECK(d.press("Ctrl+D") == KeyResult::Action);
    CHECK(R.dup == 1);
    CHECK(d.press("X") == KeyResult::Widget);  // the focused widget still comes first
    d.tree().removeModalScope(R.macro);
    CHECK(d.press("B") == KeyResult::Action);
    CHECK(R.draw == 1);
  }
  SUBCASE("a disabled action lets the chord fall through") {
    d.tree().setFocus(&clip);
    R.drawEnabled = false;
    CHECK(d.press("B") == KeyResult::Action);
    CHECK(R.draw == 0);
    CHECK(R.globalB == 1);
  }
  SUBCASE("auto-repeat fires only repeatable actions; lone modifiers are never chords") {
    d.tree().setFocus(&clip);
    CHECK(d.keyDown('b', mod::kNone, true) == KeyResult::Action);
    CHECK(R.draw == 0);  // held B does not toggle draw mode repeatedly
    CHECK(d.keyDown(key::kUp, mod::kNone, false) == KeyResult::Action);
    CHECK(d.keyDown(key::kUp, mod::kNone, true) == KeyResult::Action);
    CHECK(R.nudge == 2);
    CHECK(d.keyDown(key::kLCtrl, mod::kCtrl) == KeyResult::Unhandled);
  }
}

TEST_CASE("widgets: chords parse and print") {
  auto c = parseChord("Ctrl+Shift+T");
  REQUIRE(c.ok());
  CHECK(c->key == 't');
  CHECK(c->mods == (mod::kCtrl | mod::kShift));
  CHECK(formatChord(*c) == "Ctrl+Shift+T");
  CHECK(formatChord(*parseChord("shift+ctrl+t")) == "Ctrl+Shift+T");
  CHECK(parseChord("space")->key == key::kSpace);
  CHECK(parseChord("F9")->key == key::kF1 + 8);
  CHECK(formatChord(*parseChord("F12")) == "F12");
  CHECK(parseChord("Ctrl++")->key == '+');
  CHECK(parseChord("`")->key == '`');
  CHECK(formatChord(*parseChord("Alt+Up")) == "Alt+Up");
  CHECK(formatChord(*parseChord("Ctrl+CapsLock")) == "Ctrl+CapsLock");
  for (const char* bad : {"", "Ctrl+", "Hyper+X", "Ctrl+Ctrl+X", "F13", "Ctrl+Foo", "+X"}) {
    CAPTURE(bad);
    CHECK_FALSE(parseChord(bad).ok());
  }
  Keymap km;
  CHECK(km.bind("global", "Ctrl+D", "a").ok());
  CHECK_FALSE(km.bind("global", "ctrl+d", "b").ok());  // duplicate chord in a scope
  CHECK(km.bind("arrangement", "Ctrl+D", "b").ok());   // same chord, other scope: fine
  CHECK_FALSE(km.bind(kGlobalScope, Chord{key::kLCtrl, mod::kCtrl}, "c").ok());
  CHECK(km.lookup(km.findScope("arrangement"), *parseChord("Ctrl+D")) == "b");
  ActionRegistry reg;
  CHECK(reg.add("x", [] {}).ok());
  CHECK_FALSE(reg.add("x", [] {}).ok());
  CHECK_FALSE(reg.add("y", {}).ok());
  CHECK(reg.find("x") == 0);
  CHECK(reg.find("nope") == kNoAction);
}

TEST_CASE("widgets: paths and lookup") {
  TestDriver d(400, 300);
  Widget& root = d.setRoot<Widget>();
  Widget& arrangement = root.add<Widget>("arrangement");
  Widget& wrapper = arrangement.add<Widget>();  // unnamed: transparent in paths
  Widget& track = wrapper.add<Widget>("track[3]");
  Widget& mute = track.add<Widget>("header").add<Widget>("mute");
  CHECK(mute.path() == "arrangement/track[3]/header/mute");
  CHECK(d.find("arrangement/track[3]/header/mute") == &mute);
  CHECK(arrangement.find("track[3]/header") == mute.parent());
  CHECK(d.find("") == &root);
  CHECK(d.find("arrangement/track[4]") == nullptr);
  CHECK(d.find("arrangement/header") == nullptr);
  CHECK(root.path().empty());
}

TEST_CASE("widgets: toggling one widget damages exactly its rect") {
  TestDriver d(400, 300);
  Widget& root = d.setRoot<Widget>();
  root.setLayout(LayoutKind::Column).setPadding(Insets::all(10)).setGap(5);
  probe(root, "a", Length::flex(), Length::fixed(30));
  Probe& b = probe(root, "b", Length::fixed(120), Length::fixed(30));
  probe(root, "c");
  CHECK(d.step());  // first frame: everything
  CHECK(d.framesDrawn() == 1);
  CHECK_FALSE(d.tree().needsFrame());
  b.markDirty();
  const Damage& dmg = d.tree().damage();
  REQUIRE(dmg.needsFrame());
  CHECK_FALSE(dmg.full());
  const Rect r = dmg.bounds();
  CHECK(r.x0 == 10.0f);
  CHECK(r.y0 == 45.0f);
  CHECK(r.x1 == 130.0f);
  CHECK(r.y1 == 75.0f);
  CHECK(b.rect() == IRect{10, 45, 120, 30});
  CHECK(d.step());
  CHECK_FALSE(d.tree().damage().needsFrame());
  // A widget scrolled out of its clipping parent damages nothing.
  auto& sv = root.add<ScrollView>("sv");
  Probe& inner = probe(sv, "inner", Length::fixed(100), Length::fixed(2000));
  d.step();
  sv.scrollTo(0, 1000);
  d.step();
  Probe& tail = probe(inner, "tail", Length::flex(), Length::fixed(10));  // inner is a Stack: tail at its top
  d.step();
  tail.markDirty();
  CHECK_FALSE(d.tree().damage().needsFrame());
}

TEST_CASE("widgets: idle draws nothing; animation and timers draw only while active") {
  TestDriver d(320, 200);
  Widget& root = d.setRoot<Widget>();
  Probe& p = probe(root, "p");
  d.step();
  const uint64_t base = d.framesDrawn();
  d.advance(2.0);  // 288 ticks at 144 Hz
  CHECK(d.framesDrawn() == base);
  CHECK_FALSE(d.tree().needsFrame());
  CHECK(d.tree().nextWakeTime() == std::numeric_limits<double>::infinity());

  // Ten animation frames, then idle again.
  p.animateFrames = 10;
  p.requestFrame();
  CHECK(d.tree().needsFrame());
  d.advance(1.0);
  CHECK(p.frames == 10);
  CHECK(d.framesDrawn() == base + 10);

  // A timer wakes the loop once at its due time.
  const double t0 = d.now();
  p.requestTimer(0.5);
  CHECK(d.tree().nextWakeTime() == doctest::Approx(t0 + 0.5));
  d.advance(0.49);
  CHECK(p.timers == 0);
  d.advance(0.02);
  CHECK(p.timers == 1);
  d.advance(2.0);
  CHECK(p.timers == 1);
  CHECK(d.framesDrawn() == base + 10);  // the timer changed nothing visible
  const uint32_t id = p.requestTimer(0.1);
  p.cancelTimer(id);
  d.advance(0.5);
  CHECK(p.timers == 1);
}

TEST_CASE("widgets: hit testing at UI scale 1.5 uses device pixels") {
  TestDriver d(450, 150, 1.5f);
  Widget& root = d.setRoot<Widget>();
  root.setLayout(LayoutKind::Row);
  Probe& a = probe(root, "a", Length::fixed(100));  // 150 device px
  Probe& b = probe(root, "b");
  d.tree().layoutNow();
  CHECK(a.rect() == IRect{0, 0, 150, 150});
  CHECK(b.rect() == IRect{150, 0, 300, 150});
  CHECK(d.widgetAt({149.5f, 10}) == &a);
  CHECK(d.widgetAt({150.0f, 10}) == &b);
  d.click({140, 10});
  CHECK(a.downs == 1);
}

TEST_CASE("widgets: event dispatch does not allocate") {
  Routing R;
  TestDriver d(400, 300);
  Widget& root = d.setRoot<Widget>();
  root.setLayout(LayoutKind::Row);
  Probe& a = probe(root, "a", Length::fixed(100));
  a.setFocusable(true);
  a.setKeyScope(R.arr);
  Probe& t = probe(root, "t", Length::fixed(100));
  t.setFocusable(true);
  t.setAcceptsTextInput(true);
  auto& sv = root.add<ScrollView>("sv");
  probe(sv, "content", Length::fixed(100), Length::fixed(5000));
  d.tree().setKeymap(&R.actions, &R.keymap);
  const Chord kB{'b', mod::kNone}, kDup{'d', mod::kCtrl}, kTab{key::kTab, mod::kNone};

  auto script = [&] {
    for (int i = 0; i < 50; ++i) {
      d.move({50, 50});
      d.move({150, 50});                  // hover a → t
      d.drag({50, 50}, {350, 120}, 3);    // capture a, drag outside, release over the scroll view
      d.wheel({300, 100}, -1.0f);         // scroll (relayout of the content)
      d.wheel({300, 100}, 1.0f);
      d.click({150, 50});                 // focus t
      d.press(kB);                        // swallowed by the text field
      (void)d.type("b");
      d.press(kTab);                      // focus traversal
      d.press(kB);                        // action
      d.press(kDup);
      d.leave();
    }
  };
  script();  // warm-up: grows the text probe's string and the tree's work vectors
  t.text.reserve(4096);
  REQUIRE(pq::alloc_trap::installed());
  const uint64_t n = pq::alloc_trap::countRtViolations(script);
  CHECK(n == 0);
  CHECK(R.draw > 0);
  CHECK(R.dup > 0);
}

TEST_CASE("widgets: widgets-core demo: splitter drags, the bottom pane keeps its height on resize") {
  TestDriver d(800, 480);
  d.tree().setRoot(makeWidgetsCoreDemo());
  d.tree().layoutNow();
  CHECK(d.rect("header") == IRect{0, 0, 800, 28});
  CHECK(d.rect("status") == IRect{0, 458, 800, 22});
  CHECK(d.rect("body/browser") == IRect{0, 28, 180, 430});
  CHECK(d.rect("body/main/detail").h == 150);
  CHECK(d.rect("body/main/arrangement/toolbar/grid").w == 90);  // flex capped by its max
  const IRect handle = static_cast<Splitter*>(d.find("body"))->handleRect();
  CHECK(handle == IRect{180, 28, 4, 430});
  d.drag({182, 200}, {242, 200});
  CHECK(d.rect("body/browser").w == 240);
  d.drag({242, 200}, {700, 200});
  CHECK(d.rect("body/browser").w == 320);  // max
  d.resize(800, 600);
  CHECK(d.rect("body/main/detail").h == 150);  // anchored at the end
  CHECK(d.rect("body/main/arrangement").h == 600 - 28 - 22 - 150 - 4);
  // The lane grid scrolls under the wheel.
  auto* tracks = static_cast<ScrollView*>(d.find("body/main/arrangement/tracks"));
  d.wheel(d.center("body/main/arrangement/tracks"), -2.0f);
  CHECK(tracks->scrollY() == 96);
  d.step();
  CHECK(d.framesDrawn() == 1);
}

TEST_CASE("widgets: TestDriver renders through the offscreen target") {
  TestDriver d(320, 200);
  d.tree().setRoot(makeWidgetsCoreDemo());
  auto img = d.render();
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  CHECK(img->width == 320);
  CHECK(img->height == 200);
  // The header strip (signal_theme::kHeader = 24,24,32) at its left edge, below the text baseline.
  const uint8_t* px = img->pixel(2, 26);
  CHECK(px[0] == 24);
  CHECK(px[1] == 24);
  CHECK(px[2] == 32);
}

namespace {
// A widget whose right edge (last 4 px) resizes; the rest moves.
class EdgeProbe : public Probe {
public:
  using Probe::Probe;
  Cursor cursorAt(Vec2 p) const override { return p.x >= static_cast<float>(rect().x + rect().w - 4) ? Cursor::ResizeEW : Cursor::Move; }
};
}  // namespace

TEST_CASE("widgets: the cursor comes from the hovered widget or its ancestors, and from the captured one while dragging") {
  TestDriver d(300, 100);
  Widget& root = d.setRoot<Widget>();
  root.setLayout(LayoutKind::Row);
  Widget& group = root.add<Widget>("group");
  group.setSize(Length::fixed(200), Length::flex());
  group.setLayout(LayoutKind::Row);
  group.setCursor(Cursor::Pointer);
  Probe& a = probe(group, "a", Length::fixed(100));  // Inherit: the group's
  EdgeProbe& b = group.add<EdgeProbe>("b");
  b.setSize(Length::fixed(100), Length::flex());
  CHECK(d.tree().cursor() == Cursor::Arrow);  // the pointer was never inside
  d.move(d.center("group/a"));
  CHECK(d.tree().cursor() == Cursor::Pointer);
  a.setCursor(Cursor::Text);
  CHECK(d.tree().cursor() == Cursor::Text);
  d.move(d.center("group/b"));
  CHECK(d.tree().cursor() == Cursor::Move);
  const IRect rb = b.rect();
  const Vec2 edge{static_cast<float>(rb.x + rb.w - 2), d.center("group/b").y};
  d.move(edge);
  CHECK(d.tree().cursor() == Cursor::ResizeEW);
  // A drag keeps the captured widget's cursor wherever the pointer goes.
  d.down(edge);
  d.move(d.center("group/a"));
  CHECK(d.tree().cursor() == Cursor::Move);  // over a (Text), but b holds the capture: b's rule at that point
  d.move({280, 50});
  CHECK(d.tree().cursor() == Cursor::ResizeEW);  // right of b's edge: b's rule still decides
  d.up({280, 50});
  CHECK(d.tree().cursor() == Cursor::Arrow);  // over the root
  // A disabled widget is skipped (its ancestors decide).
  a.setEnabled(false);
  d.move(d.center("group/a"));
  CHECK(d.tree().cursor() == Cursor::Pointer);
}

TEST_CASE("widgets: a splitter's handle shows the resize cursor of its axis") {
  TestDriver d(300, 200);
  Splitter& s = d.setRoot<Splitter>("split", Splitter::Axis::Vertical, 100.0f);
  probe(s, "top");
  probe(s, "bottom");
  d.tree().layoutNow();
  const IRect h = s.handleRect();
  d.move({static_cast<float>(h.x + h.w / 2), static_cast<float>(h.y + h.h / 2)});
  CHECK(d.tree().cursor() == Cursor::ResizeNS);
  d.move(d.center("top"));
  CHECK(d.tree().cursor() == Cursor::Arrow);
}

namespace {
// Move while Alt is held (a press would pan), else the arrow.
class AltProbe : public Probe {
public:
  using Probe::Probe;
  Cursor cursorAt(Vec2) const override { return tree() && (tree()->mods() & mod::kAlt) ? Cursor::Move : Cursor::Inherit; }
};
}  // namespace

TEST_CASE("widgets: UiTree::mods follows modifier keys with the pointer still, so the cursor can show them") {
  TestDriver d(200, 100);
  AltProbe& a = d.setRoot<AltProbe>("a");
  (void)a;
  d.move({50, 50});
  CHECK(d.tree().mods() == mod::kNone);
  CHECK(d.tree().cursor() == Cursor::Arrow);
  KeyEvent down;
  down.key = key::kLAlt;
  down.mods = mod::kNone;  // SDL reports the state before the key on its own key-down
  d.tree().dispatchKey(down);
  CHECK(d.tree().mods() == mod::kAlt);
  CHECK(d.tree().cursor() == Cursor::Move);
  KeyEvent up = down;
  up.down = false;
  up.mods = mod::kAlt;
  d.tree().dispatchKey(up);
  CHECK(d.tree().mods() == mod::kNone);
  CHECK(d.tree().cursor() == Cursor::Arrow);
  d.move({60, 50}, mod::kShift | mod::kAlt);
  CHECK(d.tree().mods() == (mod::kShift | mod::kAlt));
}
