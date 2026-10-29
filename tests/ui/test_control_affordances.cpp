// T4.16a: interaction affordances of the base controls — the mouse cursor each part asks for (hovered through the
// TestDriver, resolved by UiTree::cursor), the mouse wheel on value controls (one gesture per run of notches),
// double-click to the default on knobs and faders, and typing a number into a focused drag box.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "pq/ui/code_editor.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/widget.h"

using namespace pq::ui;

namespace {

struct Fixture {
  TestDriver d;
  Widget* root = nullptr;
  Widget* content = nullptr;
  explicit Fixture(uint32_t w = 400, uint32_t h = 400) : d(w, h) {
    root = &d.setRoot<Widget>();
    content = &root->add<Widget>("content");
    content->setLayout(LayoutKind::Column).setPadding(Insets::all(10)).setGap(10);
    root->add<OverlayLayer>("overlay");
    d.tree().layoutNow();
  }
  template <class W, class... A>
  W& add(float w, float h, A&&... args) {
    W& x = content->add<W>(std::forward<A>(args)...);
    x.setSize(Length::fixed(w), Length::fixed(h));
    d.tree().layoutNow();
    return x;
  }
  Cursor at(Vec2 p) {
    d.move(p);
    return d.tree().cursor();
  }
};

Vec2 centre(const IRect& r) { return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)}; }

struct GestureLog {
  int begins = 0, ends = 0, cancels = 0, changes = 0;
  void attach(ValueControl& c) {
    c.onGestureBegin = [this] { ++begins; };
    c.onGestureEnd = [this] { ++ends; };
    c.onGestureCancel = [this] { ++cancels; };
    c.onChange = [this](double) { ++changes; };
  }
};

ValueSpec spec0to100(double def = 50) {
  ValueSpec s;
  s.min = 0;
  s.max = 100;
  s.def = def;
  s.keyStep = 1;
  s.fineFactor = 0.1;
  s.decimals = 1;
  return s;
}

class Tall : public Widget {
public:
  Tall() : Widget("content") { setSize(Length::fixed(50), Length::fixed(400)); }
};

}  // namespace

TEST_CASE("affordances: each control shows the cursor for what a press does") {
  Fixture f(400, 520);
  auto& button = f.add<Button>(80, 20, "button", "OK");
  auto& toggle = f.add<Toggle>(20, 20, "toggle", "S", ToggleStyle::Solo);
  auto& dropdown = f.add<Dropdown>(100, 18, "dropdown", std::vector<std::string>{"A", "B"}, 0);
  auto& box = f.add<DragBox>(60, 18, "box", spec0to100());
  auto& knob = f.add<Knob>(40, 40, "knob", spec0to100());
  auto& vfader = f.add<Fader>(20, 60, "vfader", spec0to100(), Fader::Orientation::Vertical);
  auto& hfader = f.add<Fader>(100, 14, "hfader", spec0to100(), Fader::Orientation::Horizontal);
  auto& text = f.add<TextEdit>(100, 18, "text", "name");
  auto& label = f.add<Label>(100, 16, "label", "plain");
  auto& meter = f.add<Meter>(10, 60, "meter");

  CHECK(f.at(centre(button.rect())) == Cursor::Pointer);
  CHECK(f.at(centre(toggle.rect())) == Cursor::Pointer);
  CHECK(f.at(centre(dropdown.rect())) == Cursor::Pointer);
  CHECK(f.at(centre(box.rect())) == Cursor::ResizeNS);  // a vertical value drag
  CHECK(f.at(centre(knob.rect())) == Cursor::ResizeNS);
  CHECK(f.at(centre(vfader.rect())) == Cursor::ResizeNS);
  CHECK(f.at(centre(hfader.rect())) == Cursor::ResizeEW);
  CHECK(f.at(centre(text.rect())) == Cursor::Text);
  CHECK(f.at(centre(label.rect())) == Cursor::Arrow);  // not interactive
  // A meter's clip lamps are clickable only when something resets them.
  const Vec2 lamp{centre(meter.rect()).x, static_cast<float>(meter.rect().y + 1)};
  CHECK(f.at(lamp) == Cursor::Arrow);
  meter.onResetClip = [] {};
  CHECK(f.at(lamp) == Cursor::Pointer);
  CHECK(f.at(centre(meter.rect())) == Cursor::Arrow);
  // Disabled: no affordance (the parent's cursor).
  button.setEnabled(false);
  CHECK(f.at(centre(button.rect())) == Cursor::Arrow);
  // The number box's type-in is a text field.
  f.d.doubleClick(centre(box.rect()));
  REQUIRE(box.editing());
  CHECK(f.at(centre(box.rect())) == Cursor::Text);
  f.d.press("Escape");
  CHECK(f.at(centre(box.rect())) == Cursor::ResizeNS);
  // A knob drag keeps its cursor over other controls (the captured widget decides).
  f.d.down(centre(knob.rect()));
  CHECK(f.at(centre(text.rect())) == Cursor::ResizeNS);
  f.d.up(centre(text.rect()));
  CHECK(f.d.tree().cursor() == Cursor::Text);
}

TEST_CASE("affordances: a scrollbar's thumb shows its axis' resize cursor; the code editor's text the I-beam") {
  Fixture f;
  auto& row = f.add<Widget>(70, 100, "row");
  row.setLayout(LayoutKind::Row);
  auto& view = row.add<ScrollView>("view", false, true);
  view.add<Tall>();
  auto& sb = row.add<Scrollbar>("bar", view);
  sb.setWidth(Length::fixed(10));
  f.d.tree().layoutNow();
  const IRect t = sb.thumbRect();
  REQUIRE_FALSE(t.empty());
  CHECK(f.at(centre(t)) == Cursor::ResizeNS);
  CHECK(f.at({centre(t).x, static_cast<float>(sb.rect().bottom() - 2)}) == Cursor::Arrow);  // the track pages
  // While the thumb is dragged the cursor stays, wherever the pointer goes.
  f.d.down(centre(t));
  CHECK(f.at({300, 300}) == Cursor::ResizeNS);
  f.d.up({300, 300});

  auto& code = f.add<CodeEditor>(200, 80, "code");
  code.setText("line one\nline two");
  const IRect r = code.rect();
  CHECK(f.at({static_cast<float>(r.right() - 20), centre(r).y}) == Cursor::Text);
  CHECK(f.at({static_cast<float>(r.x + 2), centre(r).y}) == Cursor::Arrow);  // the line-number gutter
}

TEST_CASE("affordances: the wheel steps a value control; a run of notches is one gesture") {
  Fixture f;
  auto& knob = f.add<Knob>(40, 40, "knob", spec0to100());
  GestureLog g;
  g.attach(knob);
  const Vec2 c = centre(knob.rect());
  f.d.move(c);
  CHECK(f.d.wheel(c, 1.0f));
  CHECK(f.d.wheel(c, 2.0f));
  CHECK(knob.value() == doctest::Approx(53.0));  // one key step per notch
  CHECK(g.begins == 1);
  CHECK(g.ends == 0);  // the run is still open
  f.d.advance(ValueControl::kWheelGestureSeconds * 0.5);
  CHECK(f.d.wheel(c, -1.0f, 0.0f, mod::kShift));  // Shift: fine (0.1 step), same run
  CHECK(knob.value() == doctest::Approx(52.9));
  f.d.advance(ValueControl::kWheelGestureSeconds + 0.1);
  CHECK(g.begins == 1);
  CHECK(g.ends == 1);  // one undo step for the whole run
  // Ctrl / Alt pass the wheel on (zoom in the views); a control told so leaves it to its parent.
  CHECK_FALSE(f.d.wheel(c, 1.0f, 0.0f, mod::kCtrl));
  knob.setWheelAdjusts(false);
  CHECK_FALSE(f.d.wheel(c, 1.0f));
  CHECK(knob.value() == doctest::Approx(52.9));
  knob.setWheelAdjusts(true);
  // At a limit the wheel is still the control's (the view behind does not scroll), but nothing changes.
  knob.setValue(100.0);
  CHECK(f.d.wheel(c, 1.0f));
  CHECK(g.begins == 1);
  // Leaving the control ends the run at once.
  CHECK(f.d.wheel(c, -1.0f));
  CHECK(g.begins == 2);
  f.d.move({390, 390});
  CHECK(g.ends == 2);
  // Esc during a run cancels it: the value returns.
  f.d.move(c);
  f.d.tree().setFocus(&knob);
  const double before = knob.value();
  CHECK(f.d.wheel(c, -3.0f));
  CHECK(knob.value() == doctest::Approx(before - 3.0));
  f.d.press("Escape");
  CHECK(knob.value() == doctest::Approx(before));
  CHECK(g.cancels == 1);
  f.d.advance(1.0);
  CHECK(g.ends == 2);  // the cancelled run never ends as a commit
  // A drag after a run starts its own gesture (the run ends first).
  CHECK(f.d.wheel(c, -1.0f));
  f.d.drag(c, {c.x, c.y + 20}, 2);
  CHECK(g.begins == 5);
  CHECK(g.ends == 4);
}

TEST_CASE("affordances: the wheel on faders and drag boxes; a stepped box moves by its step") {
  Fixture f;
  auto& fader = f.add<Fader>(20, 100, "fader", spec0to100(), Fader::Orientation::Vertical);
  CHECK(f.d.wheel(centre(fader.rect()), 2.0f));
  CHECK(fader.value() == doctest::Approx(52.0));
  ValueSpec s;
  s.min = 1;
  s.max = 99;
  s.def = 4;
  s.step = 1;
  s.decimals = 0;
  auto& box = f.add<DragBox>(40, 18, "box", s);
  CHECK(f.d.wheel(centre(box.rect()), -1.0f));
  CHECK(box.value() == 3.0);
  // A trackpad's fractions accumulate within the run: 0.4 of a notch moves nothing, 0.8 reaches the next step.
  CHECK(f.d.wheel(centre(box.rect()), 0.4f));
  CHECK(box.value() == 3.0);
  CHECK(f.d.wheel(centre(box.rect()), 0.4f));
  CHECK(box.value() == 4.0);
}

TEST_CASE("affordances: double-click returns a knob or fader to its default (one gesture)") {
  Fixture f;
  auto& knob = f.add<Knob>(40, 40, "knob", spec0to100(25));
  GestureLog g;
  g.attach(knob);
  knob.setValue(80);
  f.d.doubleClick(centre(knob.rect()));
  CHECK(knob.value() == 25.0);
  CHECK(g.begins == 1);
  CHECK(g.ends == 1);
  auto& fader = f.add<Fader>(20, 100, "fader", spec0to100(70), Fader::Orientation::Vertical);
  fader.setValue(10);
  const Vec2 t = centre(fader.thumbRect());
  f.d.doubleClick(t);
  CHECK(fader.value() == 70.0);
}

TEST_CASE("affordances: typing a number into a focused drag box starts the type-in with it") {
  Fixture f;
  ValueSpec s;
  s.min = 20;
  s.max = 999;
  s.def = 120;
  s.decimals = 2;
  auto& tempo = f.add<DragBox>(60, 18, "tempo", s);
  GestureLog g;
  g.attach(tempo);
  f.d.click(centre(tempo.rect()));
  REQUIRE(f.d.tree().focus() == &tempo);
  CHECK(f.d.press("1") == KeyResult::Widget);
  REQUIRE(tempo.editing());
  CHECK(tempo.editBuffer().text() == "1");
  CHECK(f.d.type("28"));  // the rest arrives as text (text input is on while typing in)
  f.d.press("Enter");
  CHECK_FALSE(tempo.editing());
  CHECK(tempo.value() == 128.0);
  CHECK(g.begins == 1);
  CHECK(g.ends == 1);
  // '-' and '.' start it too; chords and Shift do not (they stay keymap chords).
  CHECK(f.d.press("-") == KeyResult::Widget);
  CHECK(tempo.editBuffer().text() == "-");
  f.d.press("Escape");
  CHECK(tempo.value() == 128.0);
  CHECK(f.d.press("Ctrl+1") != KeyResult::Widget);
  CHECK_FALSE(tempo.editing());
}
