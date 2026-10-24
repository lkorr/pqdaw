#pragma once

// Base controls (T3.2, SPEC §9 / §11): label, button, toggle (activator / solo / arm style), Ableton numeric
// drag box, fader, knob (with a modulation-range ring hook), single-line text edit, dropdown, scrollbar and a
// PQ Meter-style peak/RMS meter. Menus, popovers and tooltips are in pq/ui/overlay.h.
//
// Conventions shared by every control:
//   - Sizes come from the caller's layout (Length, logical px). Controls draw in device px from their rect;
//     strokes that should look 1 px (borders, focus rings) are 1 device px at any UI scale (Q31), everything
//     else (radii, arc widths, insets) is logical px × the tree's UiScale.
//   - Colours and font roles come from PaintContext::res.theme / res.uiFonts (pq/ui/theme.h). A control painted
//     without a theme draws nothing. The meter alone uses PQ Meter's signal palette (signal_theme).
//   - States: normal / hover / pressed / focused / disabled (VisualState). Value controls and text fields show
//     their focus ring whenever they have focus (they then take keys, like Live's selected parameter);
//     buttons, toggles and dropdowns only after keyboard focus (UiTree::focusVisible()).
//   - Callbacks are std::function on the UI thread. A value control reports a user edit as a gesture:
//     onGestureBegin (lazily, at the first actual change), onChange per change, onGestureEnd once; a drag,
//     a type-in, a Delete reset or one arrow key are each one gesture = one undo step (T3.4 History).
//   - Info view / tooltip text: Control::setInfo / setTooltip (Widget::info(), Widget::tooltip()).
//
// UI thread only. Nothing here is RT-safe. Painting does not allocate once text runs are cached; value
// changes format their display text once per change, never per frame.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pq/ui/text.h"
#include "pq/ui/widget.h"

namespace pq::ui {

struct Theme;
struct ThemeFonts;

// --- state ----------------------------------------------------------------------------------------------
struct VisualState {
  bool hover = false;
  bool pressed = false;
  bool focus = false;     // focus ring visible
  bool disabled = false;
};
// Bits for Control::setStateOverride.
namespace vis {
inline constexpr uint8_t kNormal = 0;
inline constexpr uint8_t kHover = 1u << 0;
inline constexpr uint8_t kPressed = 1u << 1;
inline constexpr uint8_t kFocus = 1u << 2;
inline constexpr uint8_t kDisabled = 1u << 3;
inline constexpr uint8_t kLive = 0xFF;  // no override
}  // namespace vis

enum class FontRoleId : uint8_t { Small, Body, BodyStrong, Title, Numeric };
enum class TextAlign : uint8_t { Left, Center, Right };
enum class TextTone : uint8_t { Primary, Secondary };

FontId fontOf(const ThemeFonts& f, FontRoleId role) noexcept;

// --- control base ---------------------------------------------------------------------------------------
class Control : public Widget {
public:
  explicit Control(std::string name);

  Control& setTooltip(std::string text);
  // Info-view title and body (Live's help text for the hovered control).
  Control& setInfo(std::string title, std::string body);
  InfoText info() const override { return {infoTitle_, infoBody_}; }
  std::string_view tooltip() const override { return tooltip_; }

  // Paint as if in the given state (vis:: bits) regardless of input: for screenshot galleries and goldens.
  // vis::kLive (default) follows input.
  void setStateOverride(uint8_t bits);
  // The state paint() uses.
  VisualState visualState() const noexcept;

protected:
  void onHoverChanged(bool) override { markDirty(); }
  void onFocusChanged(bool) override { markDirty(); }
  // The pressed look (set by subclasses while a press or drag is in progress).
  void setPressed(bool p) noexcept;
  bool pressedNow() const noexcept { return pressed_; }
  // Value controls and text fields: the focus ring shows for pointer focus too.
  bool focusRingOnPointerFocus_ = false;

private:
  std::string tooltip_, infoTitle_, infoBody_;
  uint8_t override_ = vis::kLive;
  bool pressed_ = false;
};

// --- label ----------------------------------------------------------------------------------------------
class Label : public Control {
public:
  Label(std::string name, std::string text, FontRoleId font = FontRoleId::Small, TextTone tone = TextTone::Secondary,
        TextAlign align = TextAlign::Left);
  void setText(std::string text);
  const std::string& text() const noexcept { return text_; }
  void paint(PaintContext& ctx) override;

private:
  std::string text_;
  FontRoleId font_;
  TextTone tone_;
  TextAlign align_;
};

// --- button ---------------------------------------------------------------------------------------------
// Momentary push button: fires onClick on release inside (or Space / Enter when focused).
class Button : public Control {
public:
  Button(std::string name, std::string text);
  std::function<void()> onClick;
  void setText(std::string text);
  const std::string& text() const noexcept { return text_; }

  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;
  bool onKey(const KeyEvent& e) override;
  void onCaptureLost() override;

private:
  std::string text_;
  bool inside_ = false;
};

// --- toggle ---------------------------------------------------------------------------------------------
// Latching square button in Live's track-header style. It flips on press (as Live's mute/solo/arm do).
enum class ToggleStyle : uint8_t {
  Accent,     // generic on/off (orange when on)
  Activator,  // track activator: shows the track number, yellow when on (= not muted)
  Solo,       // "S", blue when on
  Arm,        // record arm: a dot (or text), red when on
};
class Toggle : public Control {
public:
  Toggle(std::string name, std::string text, ToggleStyle style = ToggleStyle::Accent, bool on = false);
  std::function<void(bool on)> onToggle;  // user changes only
  bool on() const noexcept { return on_; }
  void setOn(bool on);  // programmatic: no callback

  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;
  bool onKey(const KeyEvent& e) override;
  void onCaptureLost() override;

private:
  void flip();
  std::string text_;
  ToggleStyle style_;
  bool on_;
};

// --- text buffer ----------------------------------------------------------------------------------------
// Editing state of a single-line UTF-8 buffer: caret and selection at code point boundaries, word moves,
// clipboard. Shared by TextEdit and DragBox's type-in. Pure logic (no drawing, no tree). Painting maps byte
// offsets to x with TextRun::caretX and clicks to bytes with TextRun::hitTest (cluster stops).
class TextBuffer {
public:
  const std::string& text() const noexcept { return text_; }
  void setText(std::string text);  // caret to the end, no selection
  uint32_t caret() const noexcept { return caret_; }
  uint32_t anchor() const noexcept { return anchor_; }
  bool hasSelection() const noexcept { return caret_ != anchor_; }
  uint32_t selBegin() const noexcept { return caret_ < anchor_ ? caret_ : anchor_; }
  uint32_t selEnd() const noexcept { return caret_ < anchor_ ? anchor_ : caret_; }
  std::string_view selected() const noexcept { return std::string_view(text_).substr(selBegin(), selEnd() - selBegin()); }

  void setCaret(uint32_t byte, bool extend);  // snapped to a code point boundary
  void selectAll();
  void selectWordAt(uint32_t byte);
  void insert(std::string_view utf8);  // replaces the selection; stops at maxBytes (whole code points)
  void moveLeft(bool word, bool extend);
  void moveRight(bool word, bool extend);
  void home(bool extend);
  void end(bool extend);
  void backspace(bool word);
  void del(bool word);
  // Editing keys: arrows, Home/End, Backspace/Delete (Ctrl: by word), Ctrl+A/C/X/V. Returns whether the key
  // was an editing key (Enter/Esc/Tab are not). `changed` reports a text change.
  bool handleKey(const KeyEvent& e, Clipboard& clipboard, bool& changed);

  uint32_t maxBytes = 1024;

private:
  uint32_t prevBoundary(uint32_t i) const noexcept;
  uint32_t nextBoundary(uint32_t i) const noexcept;
  uint32_t wordLeft(uint32_t i) const noexcept;
  uint32_t wordRight(uint32_t i) const noexcept;
  void eraseSelection();

  std::string text_;
  uint32_t caret_ = 0, anchor_ = 0;
};

// --- value controls -------------------------------------------------------------------------------------
struct ValueSpec {
  double min = 0.0, max = 1.0, def = 0.0;
  double step = 0.0;          // value quantum (0 = continuous); drags accumulate unquantised and snap
  double dragPixels = 200.0;  // logical px of vertical drag that sweeps min..max (coarse)
  double fineFactor = 0.1;    // Shift held: drag and key sensitivity × fineFactor
  double keyStep = 0.0;       // Up/Down / wheel step; 0 = max(step, (max − min) / 100)
  int decimals = 2;           // default formatter
  std::string unit;           // default formatter: appended after a space ("dB", "Hz", "%")
};

// Shared value model, gestures and keys of DragBox, Fader and Knob. Keys when focused: Up/Down (+Shift: fine)
// step, Delete / Backspace = default (Live's "Delete returns a parameter to its default").
class ValueControl : public Control {
public:
  ValueControl(std::string name, ValueSpec spec);

  double value() const noexcept { return value_; }
  // Programmatic: clamps and quantises, no callbacks.
  void setValue(double v);
  double normalized() const noexcept;  // (value − min) / (max − min)
  const ValueSpec& spec() const noexcept { return spec_; }
  void setSpec(ValueSpec spec);
  bool inGesture() const noexcept { return gesture_; }

  std::function<void()> onGestureBegin;
  std::function<void(double value)> onChange;
  std::function<void()> onGestureEnd;
  // Esc during a drag or gesture (T3.x-fix-edit, M3 review M12b / Q92.6): the value returns to where the gesture
  // started and this runs instead of onGestureEnd (unset: onChange(start value), then onGestureEnd).
  std::function<void()> onGestureCancel;
  // Ends the current drag / gesture without keeping it (see onGestureCancel). False when none was running.
  bool cancelGesture();
  // Display text for a value (default: fixed `decimals` + unit). Called once per change.
  std::function<std::string(double value)> formatter;
  // Type-in parser (default: a decimal number, optionally followed by the unit; "-inf" when min is −inf).
  std::function<std::optional<double>(std::string_view text)> parser;

  const std::string& valueText() const noexcept { return text_; }
  std::optional<double> parse(std::string_view text) const;

  bool onKey(const KeyEvent& e) override;
  void onCaptureLost() override;
  void onInteractionCancelled() override { (void)cancelGesture(); }

protected:
  double clampQuantise(double v) const noexcept;
  // A user edit: starts the gesture if needed, sets, notifies.
  void userSet(double v);
  void endGesture();
  // Relative drag: `unitsPerPx` is the coarse change per device px along `axis` (pointer up/right = more).
  void dragStart(Vec2 pos);
  void dragMove(Vec2 pos, Mods m, double unitsPerPx, bool vertical);
  void dragEnd();
  bool dragging() const noexcept { return dragging_; }
  double keyStep() const noexcept;
  void refreshText();

  ValueSpec spec_;
  double value_ = 0.0;
  double raw_ = 0.0;  // unquantised accumulator of the current drag
  Vec2 last_{0, 0};
  bool dragging_ = false;
  bool gesture_ = false;
  double gestureStart_ = 0.0;  // value_ when the current gesture began (cancelGesture restores it)
  std::string text_;
};

// Live's numeric box: vertical drag (Shift fine), double-click to type a value (Enter commits, Esc cancels,
// focus loss commits), Delete = default, Up/Down step.
class DragBox : public ValueControl {
public:
  DragBox(std::string name, ValueSpec spec, TextAlign align = TextAlign::Center);
  // Field (default): the sunken field colour, for boxes inside panels. OnWindow: Live's transport fields, panel-coloured
  // boxes on the window-coloured control bar (T3.x-fix-look, Q107.1 option B).
  enum class Style : uint8_t { Field, OnWindow };
  void setStyle(Style s) noexcept {
    style_ = s;
    markDirty();
  }
  bool editing() const noexcept { return editing_; }
  // Type-in mode (double-click does this). The current text is selected.
  void beginEdit();
  void commitEdit();
  void cancelEdit();
  const TextBuffer& editBuffer() const noexcept { return edit_; }

  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;
  bool onKey(const KeyEvent& e) override;
  bool onText(const TextEvent& e) override;
  void onFocusChanged(bool focused) override;

private:
  void endEdit();
  TextBuffer edit_;  // the type-in buffer
  TextAlign align_;
  Style style_ = Style::Field;
  bool editing_ = false;
};

// Fader: a track with a value fill and a thumb. Dragging the thumb is relative (the thumb follows the pointer;
// Shift: fine); pressing the track elsewhere jumps there first, then drags.
class Fader : public ValueControl {
public:
  enum class Orientation : uint8_t { Vertical, Horizontal };
  Fader(std::string name, ValueSpec spec, Orientation o = Orientation::Vertical);
  Orientation orientation() const noexcept { return orient_; }
  // Device-px thumb rect from the last layout (tests).
  IRect thumbRect() const noexcept;

  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;

private:
  int32_t trackLength() const noexcept;  // device px of travel
  Orientation orient_;
};

// Modulation-range ring (SPEC §11 "a modulated knob shows a live ring of its modulation range"): normalised
// 0..1 positions on the knob's travel. Drawn in the signal palette (it is a signal display). The modulation
// source (M4) updates it and marks the knob dirty.
struct ModulationRing {
  bool active = false;
  float lo = 0.0f, hi = 0.0f;  // range swept by the modulation
  float current = -1.0f;       // live modulated position (< 0: none)
  bool operator==(const ModulationRing&) const = default;
};

// Rotary knob, Live style: track arc, value arc (from the minimum, or from 0 for bipolar ranges), needle.
// Vertical drag over spec.dragPixels sweeps the range (Shift: fine).
class Knob : public ValueControl {
public:
  Knob(std::string name, ValueSpec spec);
  void setModulation(const ModulationRing& m);
  const ModulationRing& modulation() const noexcept { return mod_; }
  // Travel: 7 o'clock to 5 o'clock, clockwise (screen angles, radians).
  static constexpr float kStartAngle = 2.35619449f;  // 0.75 π
  static constexpr float kSweep = 4.71238898f;       // 1.5 π

  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;

private:
  ModulationRing mod_;
};

// --- text edit ------------------------------------------------------------------------------------------
// Single-line text field: click / drag to place the caret and select, double-click selects a word, Shift
// extends; keys as TextBuffer; Enter commits (onCommit), Esc reverts to the last committed text (onCancel);
// focus loss commits. Takes printable keys while focused (they never reach keymaps). The caret blinks with a
// 0.53 s timer while focused (no per-frame work).
class TextEdit : public Control {
public:
  TextEdit(std::string name, std::string text = {});
  std::function<void(const std::string&)> onChange;  // every edit
  std::function<void(const std::string&)> onCommit;
  std::function<void()> onCancel;

  const std::string& text() const noexcept { return buf_.text(); }
  void setText(std::string text);  // programmatic: also the new committed text, no callbacks
  void setPlaceholder(std::string p) { placeholder_ = std::move(p); }
  TextBuffer& buffer() noexcept { return buf_; }
  const TextBuffer& buffer() const noexcept { return buf_; }
  int32_t scrollX() const noexcept { return scrollX_; }

  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;
  bool onKey(const KeyEvent& e) override;
  bool onText(const TextEvent& e) override;
  void onFocusChanged(bool focused) override;
  void onTimer(uint32_t id) override;

private:
  uint32_t byteAtX(int32_t deviceX);
  void edited(bool changed);
  void restartBlink();
  TextBuffer buf_;
  std::string committed_;
  std::string placeholder_;
  int32_t scrollX_ = 0;
  uint32_t blinkTimer_ = 0;
  bool caretOn_ = true;
  bool selecting_ = false;
};

// --- dropdown -------------------------------------------------------------------------------------------
// Live's chooser: shows the current item; click (or Enter / Space) opens a menu in the OverlayLayer; Up/Down
// change the item directly when focused.
class Dropdown : public Control {
public:
  Dropdown(std::string name, std::vector<std::string> items, int32_t index = 0);
  ~Dropdown() override;
  std::function<void(int32_t index)> onChange;  // user changes only
  int32_t index() const noexcept { return index_; }
  void setIndex(int32_t i);  // programmatic
  // Programmatic (T3.18: device lists change on hot-plug): new items and index, no callback. An open menu closes.
  void setItems(std::vector<std::string> items, int32_t index);
  const std::vector<std::string>& items() const noexcept { return items_; }
  bool menuOpen() const noexcept { return menu_ != nullptr; }
  void openMenu();

  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;
  bool onKey(const KeyEvent& e) override;

private:
  void choose(int32_t i);
  std::vector<std::string> items_;
  int32_t index_;
  Widget* menu_ = nullptr;  // open menu (owned by the overlay layer)
  std::shared_ptr<Dropdown*> alive_;  // menu callbacks reach the dropdown through this (null once destroyed)
};

// --- scrollbar ------------------------------------------------------------------------------------------
// Bound to a ScrollView: thumb size = viewport / content, drag the thumb, press the track to page. Repaints
// when the view scrolls (ScrollView::Dependent). Lay it out next to the view (e.g. a Row of the view and a
// Fixed(theme.size.scrollbar) scrollbar).
class Scrollbar : public Control, private ScrollView::Dependent {
public:
  enum class Axis : uint8_t { Vertical, Horizontal };
  Scrollbar(std::string name, ScrollView& view, Axis axis = Axis::Vertical);
  ~Scrollbar() override;
  ScrollView* view() const noexcept { return view_; }
  // Device-px thumb rect for the current scroll state (empty when everything fits).
  IRect thumbRect() const noexcept;

  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;
  void onCaptureLost() override;

private:
  void onScrollChanged() noexcept override { markDirty(); }
  void onScrollViewGone() noexcept override;
  int32_t viewLen() const noexcept;
  int32_t contentLen() const noexcept;
  int32_t scrollPos() const noexcept;
  void scrollToPos(int32_t p);
  ScrollView* view_;
  Axis axis_;
  bool dragging_ = false;
  float grab_ = 0.0f;  // pointer offset inside the thumb at press
};

// --- meter ----------------------------------------------------------------------------------------------
// Peak/RMS levels in dBFS for one frame, filled by whoever owns the analysis (M3: T3.10's MeterSource).
struct MeterLevels {
  uint8_t channels = 2;  // 1 or 2
  float rmsDb[2] = {-200.0f, -200.0f};
  float peakDb[2] = {-200.0f, -200.0f};
  float holdDb[2] = {-200.0f, -200.0f};
  bool clip[2] = {false, false};
  bool operator==(const MeterLevels&) const = default;
};

// PQ Meter's level meter look (src/modules/level_meter.cpp, portrait) at track-meter size: per channel a well,
// an RMS body (green L / salmon R, amber above −18 dBFS, red above −6), a bright peak cap, a hold tick and a
// clip lamp on top. Colours are signal_theme's (SPEC §11.1). setLevels() repaints only when a drawn pixel
// would change, so a meter fed at 60 Hz with a steady signal costs no frames.
class Meter : public Control {
public:
  explicit Meter(std::string name, float floorDb = -60.0f);
  void setLevels(const MeterLevels& l);
  const MeterLevels& levels() const noexcept { return levels_; }
  std::function<void()> onResetClip;  // click on the lamps

  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;

private:
  struct Px {
    int16_t rms[2], peak[2], hold[2];
    bool clip[2];
    uint8_t channels;
    bool operator==(const Px&) const = default;
  };
  Px pixels(const MeterLevels& l) const noexcept;
  int32_t lampHeight() const noexcept;
  MeterLevels levels_;
  Px drawn_{};
  float floorDb_;
};

}  // namespace pq::ui
