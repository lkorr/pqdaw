#pragma once

// The clip view's parts (private to pq_views, T3.11). Each paints only inside its own rect from the shared RollState
// and reports changes with markDirty(), so a drag repaints the note area and the velocity lane, a zoom the ruler too,
// and nothing else in the window.
//
//   ┌ clip-box ┐┌ corner ┬ ruler: loop brace, start / end markers, bar numbers ──────────────────┐
//   │ name     ││ Fold   │                                                                      │
//   │ tuning   │├ keys ──┼ notes: rows, grid, notes, rubber band, draw preview ─────────────────┤
//   │ loop …   ││ C3     │                                                                      │
//   │          │├ lane-  ┼ velocity: stems (or chance) ──────────────────────────────────────────┤
//   │          ││ header │                                                                      │
//   └──────────┘└────────┴──────────────────────────────────────────────────────────────────────┘

#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "roll_state.h"

namespace pq::views::roll {

// Layout constants (logical px).
inline constexpr float kClipBoxWidth = 148.0f;
inline constexpr float kKeysWidth = 76.0f;
inline constexpr float kRulerHeight = 34.0f;
inline constexpr float kBraceHeight = 13.0f;  // top strip of the ruler: loop brace and markers
inline constexpr float kLaneHeight = 72.0f;
inline constexpr float kEdgeGrab = 4.0f;      // note edge resize zone
inline constexpr float kStemGrab = 5.0f;      // velocity stem hit width (each side)

class ClipBoxPanel : public ui::Widget {
public:
  explicit ClipBoxPanel(RollState& s);
  void refresh();
  void paint(ui::PaintContext& ctx) override;

private:
  RollState& s_;
  ui::Toggle* loop_ = nullptr;
  TuningChip* tuning_ = nullptr;  // opens the MOS wheel for this clip (T3.14)
  // T3.12 (scale / MOS tunings only): layout switch, background EDO, generator morph (MOS).
  ui::Widget* layoutRow_ = nullptr;
  ui::Button* layout_ = nullptr;
  ui::Widget* gridRow_ = nullptr;
  ui::DragBox* grid_ = nullptr;
  ui::Widget* genRow_ = nullptr;
  ui::DragBox* gen_ = nullptr;
  // T3.19: the keys' row label mode.
  ui::Widget* labelsRow_ = nullptr;
  ui::Dropdown* labels_ = nullptr;
  // T3.x-fix-look: the virtual fundamental's guide on / off (shown when the clip has reference-anchored ratios).
  ui::Widget* jiRow_ = nullptr;
  ui::Toggle* ji_ = nullptr;
};

class CornerPanel : public ui::Widget {
public:
  explicit CornerPanel(RollState& s);
  void refresh();
  void paint(ui::PaintContext& ctx) override;

private:
  RollState& s_;
  ui::Toggle* fold_ = nullptr;
  ui::Toggle* scale_ = nullptr;
};

class RulerWidget : public ui::Control {
public:
  explicit RulerWidget(RollState& s);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override;

private:
  RollState& s_;
};

class KeysWidget : public ui::Control {
public:
  explicit KeysWidget(RollState& s);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override;
  void onHoverChanged(bool hovered) override;
  ui::InfoText info() const override;  // the hovered row's pitch readout (T3.19)

private:
  void paintScaleKeys(ui::PaintContext& ctx, const ui::IRect& r, const ui::IRect& vis);  // T3.12
  RollState& s_;
  std::vector<ui::Rgba> classColours_;  // scratch: one colour per step class
};

class NoteAreaWidget : public ui::Control {
public:
  explicit NoteAreaWidget(RollState& s);
  void paint(ui::PaintContext& ctx) override;
  void paintOver(ui::PaintContext& ctx) override;  // the harmonic labels strip and F chip, over the notes
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  void onCaptureLost() override;
  void onFocusChanged(bool focused) override;
  void onHoverChanged(bool hovered) override;
  ui::InfoText info() const override;  // the pitch readout under the pointer (T3.19)
  // The right-click menu at device `at` (T3.19).
  ui::Menu* openMenu(ui::Vec2 at);

private:
  void pressNote(const ui::PointerEvent& e, const doc::Note& n, int zone);
  void pressEmpty(const ui::PointerEvent& e);
  void dragTo(ui::Vec2 p);
  DrawnNote cellNote(TickRange c, float y, DrawPitch mode) const;
  void paintNoteLabels(ui::PaintContext& ctx);
  RollState& s_;
};

// The "Row Labels ▸" submenu (keys and note area menus).
ui::MenuItem rowLabelsMenu(RollState& s);

class LaneHeaderWidget : public ui::Widget {
public:
  explicit LaneHeaderWidget(RollState& s);
  void refresh();
  void paint(ui::PaintContext& ctx) override;

private:
  RollState& s_;
  ui::Label* title_ = nullptr;
  ui::Toggle* chance_ = nullptr;
};

class VelocityLaneWidget : public ui::Control {
public:
  explicit VelocityLaneWidget(RollState& s);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  void onCaptureLost() override;

  // Value ↔ lane y (device px) for the lane's current rect.
  float yOfValue(float value) const noexcept;
  float valueRange() const noexcept;

private:
  RollState& s_;
};

// Shared paint helpers.
void paintRowsBackground(ui::PaintContext& ctx, const RollState& s, const ui::IRect& r);
void paintFundamental(ui::PaintContext& ctx, const RollState& s, const ui::IRect& r, const ui::IRect& vis);
void paintHarmonicLabels(ui::PaintContext& ctx, const RollState& s, const ui::IRect& r);
void paintOverlay(ui::PaintContext& ctx, const RollState& s, const ui::IRect& vis, float x0, float x1);
// Key colours of a scale's step classes (T3.12): L in roll.keyLarge, s blended toward roll.keySmall by hardness
// (mos-explorer's softness: s = L at L/s = 1, fully keySmall from L/s = 3), sizes in between interpolated between
// their neighbours and pulled toward roll.keyExtra the more they stand apart (drawMOSCircle's colour map, with the
// theme's keyExtra in place of the complement). `out` gets one colour per class.
void stepClassColours(const ui::Theme& th, std::span<const StepClass> classes, double hardness, std::vector<ui::Rgba>& out);

}  // namespace pq::views::roll
