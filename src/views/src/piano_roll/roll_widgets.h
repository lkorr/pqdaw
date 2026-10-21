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
#include "pq/ui/widget.h"
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

private:
  RollState& s_;
};

class NoteAreaWidget : public ui::Control {
public:
  explicit NoteAreaWidget(RollState& s);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  void onCaptureLost() override;
  void onFocusChanged(bool focused) override;

private:
  void pressNote(const ui::PointerEvent& e, const doc::Note& n, int zone);
  void pressEmpty(const ui::PointerEvent& e);
  void dragTo(ui::Vec2 p);
  RollState& s_;
};

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

}  // namespace pq::views::roll
