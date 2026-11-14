#pragma once

// AC.look: the audio Clip View's box controls, drawn in the device panels' visual language (the Compressor's: dark
// face.body cards, Live-style arc knobs with the value lit in face.value and the value text under them, segmented chip
// rows whose chosen chip is filled). Widgets (pq/ui/widget.h) rather than panel elements, so they take the ordinary
// pointer / key dispatch, focus and Info text; colours are the theme's face tokens (no second palette).
//
//   Caption   a line of small text in face.ink (a caption over a chip row inside a card).
//   Section   a rounded face.body card with a small title ("Clip", "Warp", the warp mode's name); children laid out
//             under the title like any Column.
//   ClipKnob  a ui::Knob with its label above the dial and its value text under it; the arc runs from the minimum (or
//             from 0 when the range spans 0) to the value, so a percentage reads as how full the arc is. Gestures as
//             ui::Knob (drag, Shift fine, wheel, Up/Down, Delete / double-click = default, a digit types a value).
//   Chip      one cell of a chip row, or a lit toggle / push chip: a press fires onPress (a choice row selects it, a
//             toggle flips); `on` fills it in face.value. Not focusable (as the choosers it replaces).
//   FillBox   a ui::DragBox drawn as a dark rounded field with an optional label inside on the left and the value on the
//             right; `fill` paints the part of the field up to the value (from 0 when the range spans 0).
//
// UI thread only. Painting allocates nothing once text runs are cached.

#include <cstdint>
#include <functional>
#include <string>

#include "pq/ui/controls.h"
#include "pq/ui/widget.h"

namespace pq::views::audio_clip {

class Section : public ui::Widget {
public:
  Section(std::string name, std::string title);
  void setTitle(std::string title);
  const std::string& title() const noexcept { return title_; }
  void paint(ui::PaintContext& ctx) override;
  static constexpr float kTitleH = 17.0f;  // logical px (the padding's top)

private:
  std::string title_;
};

class Caption : public ui::Widget {
public:
  Caption(std::string name, std::string text);
  const std::string& text() const noexcept { return text_; }
  void paint(ui::PaintContext& ctx) override;

private:
  std::string text_;
};

class ClipKnob : public ui::Knob {
public:
  ClipKnob(std::string name, std::string label, ui::ValueSpec spec);
  const std::string& label() const noexcept { return label_; }
  // Text under the dial instead of valueText() (the transpose's "5/4 · 386.3 ¢"); empty: valueText().
  void setReadout(std::string text);
  const std::string& readout() const noexcept { return readout_.empty() ? valueText() : readout_; }
  // Device px: the dial's square (the label above, the readout below).
  ui::IRect dialRect() const noexcept;
  void paint(ui::PaintContext& ctx) override;
  void paintOver(ui::PaintContext& ctx) override;

  static constexpr float kLabelH = 15.0f, kReadoutH = 15.0f;  // logical px

private:
  std::string label_;
  std::string readout_;
};

class Chip : public ui::Control {
public:
  Chip(std::string name, std::string text);
  std::function<void()> onPress;
  bool on() const noexcept { return on_; }
  void setOn(bool on);  // programmatic, no callback
  const std::string& text() const noexcept { return text_; }
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override { setPressed(false); }

private:
  std::string text_;
  bool on_ = false;
};

class FillBox : public ui::DragBox {
public:
  FillBox(std::string name, std::string label, ui::ValueSpec spec, bool fill);
  void paint(ui::PaintContext& ctx) override;

private:
  std::string label_;
  bool fill_;
};

}  // namespace pq::views::audio_clip
