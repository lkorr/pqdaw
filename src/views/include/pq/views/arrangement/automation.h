#pragma once

// Automation lanes in the arrangement (T3.15, SPEC §9.6; Live 12's breakpoint envelopes).
//
//   ┌──────────────────────────────────────────────────────┬────────────────────────────┐
//   │ track row: clips; in automation mode (A) the chosen  │ header: … [Volume ▾] (A)  ▸ │  ArrangementView rows
//   │ parameter's envelope drawn over them                 │                            │
//   ├──────────────────────────────────────────────────────┼────────────────────────────┤
//   │ lane: one parameter's envelope (breakpoints, curved  │ lane header: ● Volume ▾ + − │  lane[i] (AutomationLaneRow)
//   │ segments, jumps); a dashed line when not automated   │                            │
//   └──────────────────────────────────────────────────────┴────────────────────────────┘
//
// Which lanes a track shows, the parameter shown over its clips and automation mode itself are view state (not in
// the document, not undoable, Q15; a lane's height is the document's LaneView when the lane exists). The chooser
// offers the track's mixer parameters (volume, pan, activator, sends) and every main tuning's continuous parameters
// (reference, EDO equave, MOS generator / period / stretch); a tuning parameter has at most one lane in the project
// (the document's rule), so a tuning automated on another track is shown, and edited, where it lives.
//
// Editing (each gesture is one pq::doc::ops edit = one undo step; on an existing lane the ChangeSet is
// Automation-only, never Structural):
//   click on the line           adds a breakpoint there (time snapped to the grid, value on the line); dragging on
//                               moves it
//   press on a breakpoint       selects it (Shift / Ctrl toggles it in a multi-selection); a drag moves the selected
//                               points (relative snap in time, value in the lane's display scale; points cannot pass
//                               their unselected neighbours); the value is read out next to the point while dragging
//   double-click a breakpoint   deletes it; Delete deletes the selected breakpoints
//   drag elsewhere in a lane    a time selection in that lane; it selects the breakpoints inside it
//   Alt+drag on a segment       bends it (the curve law of pq/core/automation_curve.h, Q39: up bulges it upwards)
//   right-click                 the envelope menu: Insert Shape ▸ (Ramp Up / Ramp Down / Sine / Triangle / Square /
//                               Saw over the time selection), Delete Breakpoints, Re-enable Automation, the lane's
//                               parameter, Show / Hide lanes, Automation Mode
// The first edit of a parameter that has no lane yet creates the lane in the same undo step (AddAutomationLane is
// Structural by the document's rule: the parameter becomes an audio-rate signal, T3.16).
//
// Automated state (Live): automated parameters show a red marker on their controls (mixer strips, lane headers); a
// hand-made change of an automated parameter overrides its automation (ViewContext::automationOverridden): the
// envelope turns grey and "Re-enable Automation" (the button above the track headers, the envelope menu) clears it.
// State only until T3.17 lowers it.
//
// Rendering: envelopes are feathered ribbons (pq/ui/lines.h smoothPolyline). The polyline has a vertex per
// breakpoint (straight segments need no more), curved segments are subdivided to ≤ kMaxChordPx device px, and runs of
// vertices inside one pixel column collapse to first / min / max / last, so a lane costs O(visible width) however
// many breakpoints it has. Every band paints only inside its rect and the paint clip; a drag frame damages the
// columns between the moved points' unmoved neighbours (old and new) in every band showing that parameter.
//
// UI thread only. Nothing here is RT-safe except the pure evaluation helpers (EnvelopeCursor).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "pq/core/automation_curve.h"
#include "pq/core/grid.h"
#include "pq/core/time.h"
#include "pq/document/edit_ops.h"
#include "pq/document/model.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"

namespace pq::views {

class ArrangementView;
class ViewContext;

namespace automation_metrics {
inline constexpr float kLaneHeight = 40.0f;      // default lane height (logical px)
inline constexpr float kMinLaneHeight = 20.0f;
inline constexpr float kMaxLaneHeight = 320.0f;
inline constexpr float kLanePad = 4.0f;          // logical px between a lane's edges and the envelope's range
inline constexpr float kPointRadius = 3.0f;      // breakpoint handle (logical px)
inline constexpr float kPointGrab = 6.0f;        // a press this close to a breakpoint grabs it
inline constexpr float kLineGrab = 4.0f;         // a press this close to the line adds a point there
inline constexpr float kLineWidth = 1.5f;        // envelope ribbon core (logical px)
inline constexpr float kMaxChordPx = 2.0f;       // curved segments: subdivision chord (device px)
inline constexpr float kCurveDragPx = 120.0f;    // Alt-drag: logical px of vertical travel for curve 0 → ±1
inline constexpr float kChooserHeight = 14.0f;   // header parameter chooser
inline constexpr float kHandleMinSpacing = 12.0f; // logical px per breakpoint (band average) below which only selected handles show
}  // namespace automation_metrics

// ---- parameters ------------------------------------------------------------------------------------------------

// How a parameter's values map onto a lane's height: norm 0 = bottom, 1 = top.
struct ParamDisplay {
  enum class Scale : uint8_t {
    Linear,    // lo … hi
    Fader,     // volume / sends: the mixer's fader position (volumeToFader), as Live draws volume envelopes
    Log,       // Hz: log2 between lo and hi
    Inverted,  // the activator: TrackMute 0 (on) at the top, 1 (off) at the bottom
  };
  Scale scale = Scale::Linear;
  double lo = 0.0, hi = 1.0;
};
ParamDisplay paramDisplay(const doc::Project& p, const doc::ParamRef& r) noexcept;
// Display position of a value (clamped to [0, 1]) and back (to a value inside the parameter's range).
double displayNorm(const ParamDisplay& d, double value) noexcept;
double displayValue(const ParamDisplay& d, double norm) noexcept;

// "Volume", "Pan", "Track Activator", "Send A", "31-EDO: Equave", "5L 2s: Generator" (tuning slot name first).
std::string paramName(const doc::Project& p, const doc::ParamRef& r);
// Short form for the track header's chooser: "Volume", "Pan", "Activator", "Send A", "5L 2s Generator".
std::string paramShortName(const doc::Project& p, const doc::ParamRef& r);
// The value as Live shows it: "-3.0 dB" / "-inf dB", "25L" / "C", "On" / "Off", "702.0 ¢", "440.0 Hz".
std::string formatParamValue(const doc::ParamRef& r, double v);
// The parameter's value without automation (the mixer setting, the tuning definition's number).
double currentParamValue(const doc::Project& p, const doc::ParamRef& r) noexcept;
// Whether `r` names something that exists in `p` and can be automated (a removed return or tuning: false).
bool paramValid(const doc::Project& p, const doc::ParamRef& r) noexcept;

// One entry of a track's parameter chooser.
struct ParamChoice {
  doc::ParamRef param;
  std::string group;         // "Mixer" or the tuning slot's name
  std::string label;         // "Volume", "Generator", …
  bool automated = false;    // a lane exists (anywhere in the project)
  bool elsewhere = false;    // a tuning parameter automated on another track (shown and edited there)
};
// Mixer parameters of `t` (volume, pan, activator unless the master, a send per return for tracks with sends),
// then each main tuning's continuous parameters.
std::vector<ParamChoice> paramChoices(const doc::Project& p, const doc::Track& t);

// The lane automating `r` anywhere in the project and the track holding it (nullptr: not automated).
struct LaneLookup {
  doc::TrackId owner;
  const doc::AutomationLane* lane = nullptr;
};
LaneLookup findAutomationLane(const doc::Project& p, const doc::ParamRef& r) noexcept;

// ---- envelope evaluation (shared curve law, pq/core/automation_curve.h) -------------------------------------------

// Evaluates an envelope at non-decreasing ticks, walking its points once: amortised O(1) per call instead of
// pq::evalAutomation's binary search. at(t) equals evalAutomation(points, t, fallback). Allocation-free; RT-safe.
class EnvelopeCursor {
public:
  EnvelopeCursor(std::span<const doc::Breakpoint> pts, double fallback) noexcept : pts_(pts), fallback_(fallback) {}
  // `t` must not be less than the previous call's (else call reset()).
  double at(double t) noexcept {
    const size_t n = pts_.size();
    if (n == 0) return fallback_;
    while (i_ < n && static_cast<double>(pts_[i_].tick) <= t) ++i_;  // i_ = first point with tick > t
    if (i_ == 0) return pts_.front().value;
    const doc::Breakpoint& a = pts_[i_ - 1];
    if (static_cast<double>(a.tick) == t || i_ == n) return a.value;
    return pq::automationSegmentValue(a, pts_[i_], t);
  }
  void reset() noexcept { i_ = 0; }

private:
  std::span<const doc::Breakpoint> pts_;
  double fallback_ = 0.0;
  size_t i_ = 0;
};

// One vertex of an envelope's polyline, in ticks and parameter units (double: what painting maps to pixels).
struct EnvelopeVertex {
  double tick;
  double value;
};
// The envelope over [t0, t1] as a polyline: the value at t0, every breakpoint inside (both points of a jump: the
// value arriving, then the value from there), curved segments subdivided so no chord spans more than
// `maxChordTicks`, the value at t1. Every vertex lies exactly on the envelope (evalAutomation at its tick; the first
// vertex of a jump: the left limit). `out` is overwritten (its capacity reused).
void envelopeVertices(std::span<const doc::Breakpoint> pts, double fallback, double t0, double t1, double maxChordTicks,
                      std::vector<EnvelopeVertex>& out);

// ---- the arrangement's automation (owned by ArrangementView) -----------------------------------------------------

// An automation lane shown under a track (ArrangementView::laneRows()).
struct AutomationLaneRow {
  doc::TrackId track;          // the track it is shown under
  int32_t row = -1;            // that track's row index (ArrangementView::rows())
  uint32_t index = 0;          // its position among the track's shown lanes
  doc::ParamRef param;
  doc::TrackId owner;          // the track holding the lane (invalid: not automated yet)
  const doc::AutomationLane* lane = nullptr;
  int32_t y = 0;               // device px from the top of the track content (as ArrangementRow::y)
  int32_t h = 0;               // device px (the bottom pixel row is the separator)
};

// Where an envelope is drawn and edited: a lane, or a track row in automation mode (its chosen parameter).
struct AutomationBand {
  doc::TrackId track;          // the row's track (a lane's: the track it is shown under)
  doc::ParamRef param;
  doc::TrackId owner;          // the track holding the lane (invalid: not automated yet)
  const doc::AutomationLane* lane = nullptr;
  ui::IRect rect;              // device px, window coordinates (without the separator row)
  float top = 0.0f;            // device y of display value 1 (the envelope's range: top … bottom)
  float bottom = 0.0f;         // device y of display value 0
  bool overClips = false;      // a track row in automation mode (else a lane)
  bool pinned = false;         // a return / master row (pinned at the bottom)
  int32_t laneIndex = -1;      // laneRows() index (lanes)
};

class ArrangementAutomation {
public:
  explicit ArrangementAutomation(ArrangementView& view);
  ~ArrangementAutomation();
  ArrangementAutomation(const ArrangementAutomation&) = delete;
  ArrangementAutomation& operator=(const ArrangementAutomation&) = delete;

  // ---- automation mode (A): every track row shows its chosen parameter's envelope over the clips ----------------
  bool mode() const noexcept { return mode_; }
  void setMode(bool on);

  // ---- lanes under a track (view state) ------------------------------------------------------------------------
  bool lanesShown(doc::TrackId t) const noexcept;
  // Shows / hides the track's lanes. The first time: its automated parameters (LaneView::visible ones), else the
  // chosen parameter.
  void showLanes(doc::TrackId t, bool on);
  // The parameters of the track's lanes, in order (empty while hidden). Invalid ones (a removed return) are dropped.
  std::vector<doc::ParamRef> shownLanes(doc::TrackId t) const;
  // The lane chooser: shows `p` in lane `index` (a parameter shown in another lane of the track swaps places).
  void setLaneParam(doc::TrackId t, size_t index, const doc::ParamRef& p);
  // "+": a new lane with the next parameter not shown yet (automated ones first). False when every one is shown.
  bool addLane(doc::TrackId t);
  // "−": hides lane `index` (LaneView::visible = false when it is automated); the last one hides the lanes.
  void hideLane(doc::TrackId t, size_t index);
  // The same at the view's next frame: for the lane header's own "−" (hiding removes that header widget, which must
  // not happen inside its pointer handler).
  void hideLaneLater(doc::TrackId t, size_t index);
  void runPending();
  // The parameter drawn over the track's clips in automation mode (the header's chooser). Default: the track's
  // first automated parameter, else its volume.
  doc::ParamRef chosen(doc::TrackId t) const;
  void choose(doc::TrackId t, const doc::ParamRef& p);
  // Lane height (logical px) and its change (LaneView::height when the lane exists, else view state).
  float laneHeight(doc::TrackId t, const doc::ParamRef& p) const;
  void setLaneHeight(doc::TrackId t, const doc::ParamRef& p, float logicalPx);

  // ---- geometry ----------------------------------------------------------------------------------------------
  // The band showing `p` for track `t` (a lane, or with overClips the track row in automation mode), when shown.
  std::optional<AutomationBand> band(doc::TrackId t, const doc::ParamRef& p, bool overClips) const;
  // The band under a device point (lanes, then automation-mode rows).
  std::optional<AutomationBand> bandAt(ui::Vec2 p) const;
  float yOfValue(const AutomationBand& b, double value) const;
  double valueAtY(const AutomationBand& b, float y) const;
  // Device position of a breakpoint in a band (as drawn now: a drag's preview included).
  std::optional<ui::Vec2> pointPos(const AutomationBand& b, doc::BreakpointId id) const;

  // ---- selection (breakpoints of one lane, and a time selection in one band) ----------------------------------
  const std::vector<doc::BreakpointId>& selectedPoints() const noexcept { return selPoints_; }
  const doc::ParamRef& selectedParam() const noexcept { return selParam_; }
  doc::TrackId selectedTrack() const noexcept { return selTrack_; }
  bool hasSelection() const noexcept { return !selPoints_.empty() || selRange_.has_value(); }
  void select(doc::TrackId t, const doc::ParamRef& p, std::vector<doc::BreakpointId> points, std::optional<TickRange> range = {});
  void clearSelection();
  const std::optional<TickRange>& timeSelection() const noexcept { return selRange_; }

  // ---- edits (each = one undo step; false when there was nothing to do or it failed: the reason in the status line)
  bool deleteSelected();
  // Replaces the envelope over the time selection with a shape spanning the lane's display range (periodic shapes:
  // one cycle per 4/4 bar of the selection, at least one).
  bool insertShape(doc::ops::ShapeKind kind);
  // "Re-enable Automation" (ViewContext::reenableAutomation).
  void reenable();
  // Menus: the envelope menu for a band (right-click), the parameter chooser (a lane's header: laneIndex ≥ 0; the
  // track header: −1).
  ui::Menu* openMenu(ui::Vec2 at, const AutomationBand& b);
  ui::Menu* openChooser(ui::Widget& from, doc::TrackId t, int32_t laneIndex);

  // ---- drag state (tests, the view) ----------------------------------------------------------------------------
  bool dragging() const noexcept;
  void cancelDrag();
  // The value readout shown next to a dragged breakpoint (empty when none).
  const std::string& readout() const noexcept { return readoutText_; }

  // ---- internal (the arrangement's widgets) -------------------------------------------------------------------
  // Pointer input from the lanes (and the pinned rows): true when the automation took it.
  bool pointer(const ui::PointerEvent& e);
  // Paints the bands meeting ctx.clip: lanes (main rows) or the pinned rows' automation-mode bands.
  void paintLanes(ui::PaintContext& ctx, const ui::IRect& canvas);
  void paintPinned(ui::PaintContext& ctx, const ui::IRect& canvas);
  // The parameters of the lanes shown under `t` (ArrangementView::rebuildRows lays them out).
  void lanesFor(const doc::Project& p, const doc::Track& t, std::vector<doc::ParamRef>& out) const;
  // Paints the lanes' background before the grid, then (paintLanes) their separators and envelopes.
  void paintLaneBackgrounds(ui::PaintContext& ctx, const ui::IRect& canvas);
  // The rows changed (document, fold, heights): drops a drag whose lane went away.
  void afterDocumentChange(const doc::StoreEvent& e);
  // Damages every band showing the lanes in `lanes` (an Automation-only document change: only they repaint).
  void damageLanes(std::span<const doc::LaneId> lanes);
  // Repaints every band (override state changed, mode toggled).
  void damageAll();

  struct Stats {
    uint32_t bandsPainted = 0;
    uint32_t vertices = 0;       // polyline vertices of the last paint
    uint32_t handles = 0;        // breakpoint handles drawn in the last paint
  };
  const Stats& stats() const noexcept { return stats_; }

private:
  struct TrackState {
    bool shown = false;
    bool initialised = false;
    std::vector<doc::ParamRef> lanes;
    std::optional<doc::ParamRef> chosen;
    std::vector<std::pair<doc::ParamRef, float>> heights;  // logical px, lanes without a document lane
  };
  enum class DragKind : uint8_t { None, Points, Curve, Range };
  struct Drag {
    DragKind kind = DragKind::None;
    AutomationBand band;
    ui::Vec2 press{};
    bool moved = false;
    std::vector<doc::Breakpoint> orig;     // the lane's points at the press (+ a created point)
    std::vector<uint32_t> moving;          // raw ids being moved (sorted)
    bool created = false;                  // the pressed point is new (a click on the line)
    doc::Breakpoint pressed{};             // the grabbed point as it was at the press
    Ticks dt = 0;
    double dv = 0.0;                       // parameter units, applied to every moved point (clamped per point)
    Ticks minDt = 0, maxDt = 0;
    float curve0 = 0.0f, curve = 0.0f;     // Curve: the segment's curvature at the press / now
    double curveSign = 1.0;
    doc::BreakpointId segment{};           // Curve: the point starting the segment
    Ticks anchor = 0;                      // Range
    std::vector<doc::Breakpoint> preview;  // the lane's points as drawn now (sorted)
    bool previewValid = false;
    int32_t damageX0 = 0, damageX1 = 0;    // columns damaged last frame
  };

  TrackState& state(doc::TrackId t);
  const TrackState* findState(doc::TrackId t) const noexcept;
  void initLanes(TrackState& s, doc::TrackId t);
  void relayout();
  void press(const ui::PointerEvent& e, const AutomationBand& b);
  void dragTo(const ui::PointerEvent& e);
  void release();
  void updatePreview();
  void damageDrag();
  void damageColumns(const doc::ParamRef& p, int32_t x0, int32_t x1);
  void damageSelection();
  bool commit(const doc::ops::EditResult& e, const AutomationBand& b, std::string_view what);
  // An edit on a band's lane: on an existing lane `make(project, laneId)`; a band without a lane gets one created
  // first (one Batch, one undo step).
  template <class Make>
  bool editLane(const AutomationBand& b, std::string_view what, Make&& make);
  std::span<const doc::Breakpoint> pointsOf(const AutomationBand& b) const noexcept;
  void paintBand(ui::PaintContext& ctx, const AutomationBand& b, const ui::IRect& clip);
  AutomationBand laneBand(size_t laneIndex) const;
  std::optional<AutomationBand> rowBand(int32_t row, bool pinned) const;
  Grid grid() const noexcept;

  ArrangementView& view_;
  ViewContext& ctx_;
  bool mode_ = false;
  std::unordered_map<uint32_t, TrackState> tracks_;
  // Selection.
  doc::TrackId selTrack_;
  doc::ParamRef selParam_;
  std::vector<doc::BreakpointId> selPoints_;
  std::vector<uint32_t> selIds_;  // raw, sorted
  std::optional<TickRange> selRange_;
  bool selOverClips_ = false;
  Drag drag_;
  std::string readoutText_;
  std::optional<std::pair<doc::TrackId, size_t>> pendingHide_;
  // Paint scratch (kept: painting does not allocate once warm).
  std::vector<EnvelopeVertex> verts_;
  std::vector<ui::Vec2> poly_;
  Stats stats_;
};

}  // namespace pq::views
