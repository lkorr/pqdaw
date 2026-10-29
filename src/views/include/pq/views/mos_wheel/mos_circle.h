#pragma once

// MOS wheel: circle view (T3.14, SPEC §4.2). A port of mos-explorer's drawMOSCircle (index.html ~3693–4650) and its
// handlers (handleModeButton, navigateToParent / navigateToChild, the hardness slider, the MODMOS dot drag, the
// "all intervals" toggle and the MODMOS reset button):
//
//            5L 2s  2.00 (2:1)  Mode 4 · 5|1  Ionian            title (drops UDP / mode name / tags to fit)
//      [◀ gen ▶]           LLsLLLs           [◀ step ▶]         mode rotation by generator / by step
//      ( ════════════●──────────────────────── )                hardness slider (equalized ↔ collapsed)
//        equalized        ·    ·   ·          collapsed          ticks at L/s = 4:3, 3:2, 2, 5:2, 3, 4
//      ▲ 4L 1s       ╭──── 0 ────╮         7L 5s ▼               parent / child arrows (findParentMos / childMoses)
//                  ╱  L    │    L ╲                              step slices (L / s / M…), degree spokes and dots
//                 │ s ─── ● ─── L │  degree cents outside        with cents, the tonic in red, JI markers inside
//                  ╲  L    │    L ╱                              (n/d, n, d < 20), MODMOS alterations (# / b)
//                    ╰────────╯              [all intervals]
//
// Input: the MOS from the chart (setSelection: key, region, generator, mode rotation) and the MODMOS alterations
// (setModmos). Output: plain values through onChange(selection, modmos, gesture) — Live while the hardness slider or
// a MODMOS dot is dragged, Commit for a release / button / arrow, Cancel when Esc (or a right click) ends a drag.
// The circle has no document dependency (the popover, pq/views/mos_wheel/mos_wheel.h, turns values into commands).
//
//   - Hardness slider: the thumb sits at u = 1 − s/L (0 at the equalized end, 1 at the collapsed end; L/s = 2 is
//     the middle), oriented like the explorer (its equalized end on the region's equalized side). Dragging sets the
//     generator for that hardness (generatorForHardnessIn: generatorForHardness's semantics, solved exactly). It snaps
//     to the tick ratios within kHardnessSnapPx; Alt bypasses.
//   - Mode buttons: tuning::applyModeAction (rotation by step / by generator, MODMOS carried to the same notes).
//   - Arrows: the parent / child region containing the generator (the explorer's navigateToParent /
//     navigateToChild), the mode carried by tuning::transferModeRotation.
//   - Dots: press a non-tonic dot and drag around the circle to alter it (snaps to whole chromas, the EDO grid when
//     the generator is an EDO generator, and zero); the tonic auditions (onAudition).
//
// Rendering: one widget; the scale model (pitches, steps, colours, labels, title parts) is rebuilt only when the
// selection, MODMOS, config or rect changes, the JI marker table only when the displayed period or stretch changes;
// paint emits geometry from the model. Every change damages the whole widget (the whole circle moves). Colours:
// theme group `mosCircle.*` (surfaces from the Live 12 look, data colours from the explorer) and the explorer's
// region palette for the step colours. No colour smoothing over time (the explorer's 8/s lerp): the colours are a
// pure function of the generator, so frames are reproducible.
//
// UI thread only. Nothing here is RT-safe.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pq/tuning/mos.h"
#include "pq/tuning/mos_regions.h"
#include "pq/ui/widget.h"
#include "pq/views/mos_wheel/mos_chart.h"

namespace pq::views {

// Layout metrics (logical px).
namespace mos_circle_metrics {
inline constexpr float kTitleY = 14.0f;          // title line centre
inline constexpr float kModeRowY = 36.0f;        // mode buttons / step pattern centre
inline constexpr float kModeBtnW = 18.0f, kModeBtnH = 14.0f, kModeBtnGap = 3.0f, kModeLabelW = 30.0f;
inline constexpr float kSliderY = 52.0f;         // hardness slider top
inline constexpr float kSliderH = 14.0f;
inline constexpr float kSliderPad = 6.0f;        // hit slop (hitSlider)
inline constexpr float kSliderLabelY = 76.0f;    // "equalized" / "collapsed" centre
inline constexpr float kCircleTop = 112.0f;      // circle top (cy − r) at the smallest layout
inline constexpr float kBottom = 36.0f;          // below the circle (labels + the all-intervals button)
inline constexpr float kSideMargin = 45.0f;      // each side of the circle (degree labels)
inline constexpr float kMinRadius = 40.0f;
inline constexpr float kRefRadius = 170.0f;      // the explorer's default radius (rScale = radius / 170)
inline constexpr float kArrowR = 11.0f;
inline constexpr float kDotR = 5.0f;
inline constexpr float kDotHitR = 12.0f;         // hitCircleDot
inline constexpr float kHardnessSnapPx = 4.0f;
inline constexpr float kClickSlop = 3.0f;
}  // namespace mos_circle_metrics

// The generator in `sel`'s region whose exact step ratio L/s is `ratio` (≥ 1; +∞ = the collapsed end, 1 = the
// equalized end): the closed form of tuning::mos_calc::generatorForHardness. L and s are linear in the generator inside
// a region, so L − ratio·s = 0 is solved from two points on the unrounded steps; the CLI bisects on 0.01-cent-rounded
// steps (≤ ~1e-4 off in L/s, Q6), takes ~0.4 ms, and fails on regions ending at 0 or P/2 (tests check agreement
// wherever it solves). nullopt when the region is degenerate.
std::optional<double> generatorForHardnessIn(const MosSelection& sel, double ratio);
// Exact L/s of the unrotated MOS at `sel`'s generator (unrounded steps; +∞ when collapsed).
double exactHardness(const MosSelection& sel);

class MosCircle : public ui::Widget {
public:
  explicit MosCircle(std::string name = "mos-circle", const MosChartConfig& config = {});
  ~MosCircle() override;

  // Period, periods per equave, max size (the regions the arrows may navigate to), max EDO (the MODMOS EDO grid and
  // the title's EDO step ratio), octave stretch (labels). Rebuilds the regions; keeps the selection.
  void setConfig(const MosChartConfig& config);
  const MosChartConfig& config() const noexcept { return config_; }
  const tuning::MosRegionSet& regionSet() const noexcept { return regions_; }

  // --- value ---
  // The MOS shown (no onChange). nullopt: an empty circle with the explorer's hint text.
  void setSelection(const std::optional<MosSelection>& sel);
  const std::optional<MosSelection>& selection() const noexcept { return sel_; }
  // MODMOS alterations in chromas per degree of the rotated scale (tuning::applyModmos). No onChange.
  void setModmos(std::vector<double> modmos);
  std::span<const double> modmos() const noexcept { return modmos_; }
  // The tuning: selection().params() + the MODMOS alterations (trimmed to n, empty when none is > 0.01).
  std::optional<tuning::MosParams> params() const;

  using ChangeFn = std::function<void(const std::optional<MosSelection>&, std::span<const double> modmos, MosGesture)>;
  void setOnChange(ChangeFn fn) { onChange_ = std::move(fn); }
  std::function<void(double cents)> onAudition;  // a click on the tonic (unstretched cents from the tonic)

  // --- actions (the buttons and arrows; each emits onChange, Commit) ---
  bool modeAction(tuning::ModeAction a);
  bool navigateToParent();
  bool navigateToChild();
  // Generator for L/s = ratio in the selected region (generatorForHardnessIn). Emits `g`.
  bool setHardness(double ratio, MosGesture g = MosGesture::Commit);
  void resetModmos();  // emits Commit if there were alterations
  void setShowAllIntervals(bool on);
  bool showAllIntervals() const noexcept { return showAll_; }

  // --- what is shown (tests, the popover) ---
  std::optional<tuning::MosKey> parentKey() const;  // findParentMos of the selected (whole-scale) key
  std::optional<tuning::MosKey> childKey() const;   // the child the arrow names (the one containing the generator)
  double hardness() const;                          // exactHardness of the selection (+∞ collapsed)
  bool dragging() const noexcept { return drag_ != Drag::None; }

  // --- geometry (device px, from the last layout) ---
  struct Geometry {
    ui::Vec2 centre{};
    float radius = 0.0f;
    ui::IRect slider{};
    ui::IRect genDown{}, genUp{}, stepDown{}, stepUp{};
    ui::Vec2 parentArrow{}, childArrow{};
    ui::IRect allIntervals{}, reset{};  // reset: {} without MODMOS
  };
  const Geometry& geometry() const;
  // Device x of the slider thumb for L/s = ratio.
  float sliderXOfHardness(double ratio) const;
  // Position of displayed degree i's dot (i < degreeCount()).
  ui::Vec2 dotPos(int32_t degree) const;
  int32_t degreeCount() const;
  // Displayed pitches in cents (the rotated, altered scale repeated over the equave).
  std::span<const double> displayPitches() const;
  // Step letters per slice ("L", "s", "M", …) and the title parts as drawn (tests, info).
  std::span<const std::string> stepLabels() const;
  std::string titleText() const;

  struct Stats {
    uint64_t models = 0;  // scale model rebuilds
    uint64_t jiBuilds = 0;
    uint64_t paints = 0;
    uint32_t jiLabels = 0;  // last paint
  };
  const Stats& stats() const noexcept { return stats_; }

  // --- widget ---
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  void onCaptureLost() override;
  void onHoverChanged(bool hovered) override;
  ui::InfoText info() const override;
  // T4.16a: the mode / step buttons, the parent / child arrows, the toggles and the tonic (audition) Pointer; the
  // hardness slider ResizeEW; a degree's dot Move (drags its MODMOS alteration); during a drag the drag's cursor.
  ui::Cursor cursorAt(ui::Vec2 pos) const override;

  struct Model;  // the cached scale model (mos_circle.cpp)

private:
  enum class Drag : uint8_t { None, Slider, Modmos, Pending };
  struct ModmosDrag {
    int32_t origDeg = 0;
    double baseCents = 0, chroma = 0, startOffset = 0, edoStep = 0, prevCents = 0, nextCents = 0;
  };
  struct JiMarker {
    std::string ratio;
    double cents = 0;
    int32_t complexity = 0;
  };

  void rebuildRegions();
  int32_t findRegion(const MosSelection& s) const noexcept;
  void invalidate();
  void ensureModel() const;
  void ensureGeometry() const;
  void ensureJi() const;
  void emit(MosGesture g);
  void selectRegion(int32_t region, double gen, int32_t rotation);
  double hardnessAtSliderX(float x, bool snap) const;
  bool applyHardness(double ratio, MosGesture g, bool emitUnchanged);
  double sliderU(double ratio) const noexcept;  // 0 equalized … 1 collapsed → slider fraction (left → right)
  void dragModmos(ui::Vec2 p);
  void cancelDrag();
  int32_t dotAt(ui::Vec2 p) const;
  // What a left press at p hits, in onPointer's order (cursorAt).
  enum class Part : uint8_t { None, Button, Slider, Tonic, Dot };
  Part partAt(ui::Vec2 p) const;

  MosChartConfig config_;
  tuning::MosRegionSet regions_;
  std::vector<tuning::EdoGenerator> edos_;
  std::vector<ui::Rgba> palette_;

  std::optional<MosSelection> sel_;
  int32_t region_ = -1;
  std::vector<double> modmos_;
  bool showAll_ = false;
  ChangeFn onChange_;

  mutable std::unique_ptr<Model> model_;
  mutable bool modelDirty_ = true;
  mutable bool geoDirty_ = true;
  mutable ui::IRect geoRect_{};
  mutable float geoScale_ = 0.0f;
  mutable Geometry geo_;
  mutable std::vector<JiMarker> ji_;
  mutable double jiPeriod_ = -1.0, jiStretch_ = 0.0;

  Drag drag_ = Drag::None;
  ui::Vec2 pressPos_{};
  std::optional<MosSelection> pressSel_;
  std::vector<double> pressModmos_;
  ModmosDrag mdrag_;
  int32_t pressDot_ = -1;
  int32_t hoverDot_ = -1;

  mutable std::vector<ui::Vec2> scratchPts_;  // paint scratch, reused (no allocation per paint once grown)
  mutable std::vector<ui::Rect> scratchRects_;
  mutable Stats stats_;
};

}  // namespace pq::views
