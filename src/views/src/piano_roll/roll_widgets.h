#pragma once

// The clip view's parts (private to pq_views, T3.11). Each paints only inside its own rect from the shared RollState
// and reports changes with markDirty(), so a drag repaints the note area and the velocity lane, a zoom the ruler too,
// and nothing else in the window.
//
//   ┌ clip-box ┐┌ corner ┬ ruler: loop brace, start / end markers, bar numbers ──────────────────┐
//   │ name   ? ││ Fold   │                                                                      │
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
inline constexpr float kKeysWidth = 76.0f;      // the keys column (the zoom strip at its left included)
inline constexpr float kZoomStripWidth = 9.0f;  // bug-roll-vzoom: the row zoom strip left of the keys
inline constexpr float kRulerHeight = 34.0f;
inline constexpr float kBraceHeight = 13.0f;  // top strip of the ruler: loop brace and markers
// bug-paused-playhead (user 2026-11-15, Live's scrub area): the strip under the brace, above the bar numbers: a press
// plays from there (playing: jumps), a drag scrubs. The numbers row under it zooms (↕) and scrolls (↔).
inline constexpr float kScrubHeight = 7.0f;
inline constexpr float kLaneHeight = 72.0f;
inline constexpr float kEdgeGrab = 4.0f;      // note edge resize zone
inline constexpr float kStemGrab = 5.0f;      // velocity stem hit width (each side)
inline constexpr float kHelpButton = 16.0f;   // KB.help: the ? button at the right end of the clip box's title bar

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
  ui::Cursor cursorAt(ui::Vec2 p) const override;  // T4.16b: what a press there would drag

private:
  // What a press at `p` grabs (the press and the cursor share it): StartMarker, EndMarker, LoopBrace (with its
  // edge and snapping origin) or RulerZoom.
  struct Hit {
    DragKind kind = DragKind::RulerZoom;
    BraceEdge brace = BraceEdge::Both;
    Ticks origin = 0;
  };
  Hit hitAt(ui::Vec2 p) const;
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
  ui::Cursor cursorAt(ui::Vec2 p) const override;  // T4.16b: Pointer (click auditions), ↔ while zooming the rows

private:
  void paintScaleKeys(ui::PaintContext& ctx, const ui::IRect& r, const ui::IRect& vis);  // T3.12
  RollState& s_;
  std::vector<ui::Rgba> classColours_;  // scratch: one colour per step class
};

// bug-roll-vzoom (Live's zoom strips): a narrow bar left of the keys. Dragging it down makes the rows taller (zooms
// in), up flatter, ×2 per 60 logical px like the ruler's numbers, with the pitch under the press held at the press;
// a double-click fits the rows to the clip's notes (RollState::fitRows). It never auditions or selects.
class RowZoomStrip : public ui::Control {
public:
  explicit RowZoomStrip(RollState& s);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override;
  ui::Cursor cursorAt(ui::Vec2 p) const override;  // ↕

private:
  RollState& s_;
  bool dragging_ = false;
  float pressY_ = 0.0f;         // device px
  int32_t pressRowPx_ = 0;      // logical px
  double pressCents_ = 0.0;     // the pitch under the press, held there
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
  // T4.16b: the part a press would grab (RollState::noteAt, as pressNote): body = Move, start / end = ResizeEW, empty
  // space = Arrow (selection rectangle); Draw Mode = Crosshair everywhere; a drag keeps its own.
  ui::Cursor cursorAt(ui::Vec2 p) const override;
  // The right-click menu at device `at` (T3.19).
  ui::Menu* openMenu(ui::Vec2 at);

private:
  void pressNote(const ui::PointerEvent& e, const doc::Note& n, int zone);
  void pressEmpty(const ui::PointerEvent& e);
  bool pressPen(const ui::PointerEvent& e);                    // T7.6: Draw Mode on a note
  void dragTo(ui::Vec2 p, ui::Mods mods);
  DrawnNote cellNote(TickRange c, ui::Vec2 p, DrawPitch mode, ui::Mods mods) const;
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
  ui::Cursor cursorAt(ui::Vec2 p) const override;  // T4.16b: ↕ over a stem a press would grab
  void onHoverChanged(bool hovered) override;
  // T4.17: the stem a press would grab is highlighted (its head ringed) while the pointer rests over it.
  uint32_t hoveredStem() const noexcept { return hoverStem_; }

  // Value ↔ lane y (device px) for the lane's current rect.
  float yOfValue(float value) const noexcept;
  float valueRange() const noexcept;

private:
  // The stem a press at `p` grabs: the nearest within reach; among stems at the same x the one whose head is closest.
  const doc::Note* stemAt(ui::Vec2 p) const;
  void setHoverStem(uint32_t id);
  RollState& s_;
  uint32_t hoverStem_ = 0;  // note id (0: none)
};

// Shared paint helpers.
void paintRowsBackground(ui::PaintContext& ctx, const RollState& s, const ui::IRect& r);
void paintFundamental(ui::PaintContext& ctx, const RollState& s, const ui::IRect& r, const ui::IRect& vis);
void paintHarmonicLabels(ui::PaintContext& ctx, const RollState& s, const ui::IRect& r);
void paintOverlay(ui::PaintContext& ctx, const RollState& s, const ui::IRect& vis, float x0, float x1);
// The notes drawn along a pitch path (RollState::curveNotes, filled by the note area's paint; T7.12: roll_paint.cpp,
// the same line as every note).
void paintCurveNotes(ui::PaintContext& ctx, RollState& s, const ui::IRect& vis);
// T7.6 (roll_pen.cpp): the breakpoint handles of the curves being edited, the neighbours an Alt + edge drag bends to.
void paintCurveHandles(ui::PaintContext& ctx, RollState& s, const ui::IRect& vis);
// T7.14 (roll_vibrato.cpp): the ~ grips and the envelope corners of the hovered and the selected notes.
void paintVibratoGrips(ui::PaintContext& ctx, RollState& s, const ui::IRect& vis);
// T7.14 (roll_groups.cpp): the groups' frames and headers (under the notes); warp markers, grips and labels, the
// proportion tool's box (over them).
void paintGroupFrames(ui::PaintContext& ctx, RollState& s, const ui::IRect& vis);
void paintGroupOverlays(ui::PaintContext& ctx, RollState& s, const ui::IRect& vis);
// Key colours of a scale's step classes (T3.12): L in roll.keyLarge, s blended toward roll.keySmall by hardness
// (mos-explorer's softness: s = L at L/s = 1, fully keySmall from L/s = 3), sizes in between interpolated between
// their neighbours and pulled toward roll.keyExtra the more they stand apart (drawMOSCircle's colour map, with the
// theme's keyExtra in place of the complement). `out` gets one colour per class.
void stepClassColours(const ui::Theme& th, std::span<const StepClass> classes, double hardness, std::vector<ui::Rgba>& out);

}  // namespace pq::views::roll
