#pragma once

// MOS wheel: region chart (T3.13, SPEC §4.2). A port of mos-explorer's main canvas (index.html drawAxis / drawMOS /
// drawEDOs / drawTemperaments / drawGeneratorMarker / drawSizeScrollbar, hitTest / handleClick / setGenFromScreenX):
//
//   ┌──────────────────────────────────────────────────────────────────────────────┬──┐
//   │ 600¢        650¢        [700.0¢]       750¢          cents labels, marker pill │  │
//   │ ═══╪════|════╪════|═══════▼═╪════|════╪═══  axis bar (press/drag: generator)   │  │
//   │                 18\31  7\12 :           EDO markers m\n ≤ max EDO, cents, L:s   │▓▓│ size
//   │              [  Meantone   ] :           temperament ranges (packed rows)       │▓▓│ scroll-
//   │ 7  ░░░░░░░░[   5L 2s (diatonic)   ]░░░░  one row per MOS size, Stern–Brocot     │  │ bar
//   │ 8  ░░░░[ 7L 1s ]░░░░░░░░[  5L 3s  ]░░░  bands (buildMosRegions), dashed marker  │  │
//   └──────────────────────────────────────────────────────────────────────────────┴──┘
//
// x = generator cents (per period; repeated across the equave when the period divides it), y = MOS size per
// period (rows from firstSize() to config().maxSize). Nothing selected: every band at the explorer's resting
// alpha, hover highlights. A band selected: the others grey out, the EDO markers and temperament ranges of that
// band appear above the rows, the selected band's colour follows the generator (collapsed ↔ equalized), and the
// generator marker shows.
//
// Interaction (the explorer's, plus EDO snapping and Esc):
//   - click a band: select its MOS (generator = the clicked cents, clamped to the band); click inside the selected
//     band's range: set the generator; click an EDO marker: generator = exactly that m\n; click elsewhere: clear.
//   - press the axis bar inside the selected range, or the marker line anywhere: drag the generator. It follows the
//     pointer, crosses into the adjacent region past the band's ends (tuning::findAdjacentRegion, the mode rotation
//     carried by tuning::transferModeRotation), and snaps to a visible EDO marker within kSnapPx (exactly its
//     m·period/n cents); Alt bypasses snapping; Esc cancels (back to the selection at the press).
//   - drag the background: pan (horizontal) and scroll the sizes (vertical, 1 size per 20 px, as the explorer).
//   - wheel: zoom around the pointer (×1.15 per notch); Ctrl+wheel: row height; Shift+wheel: scroll the sizes.
//   - size scrollbar (right edge): drag the thumb or press the track.
//   - hover: an info card (MOS name, range, parent, hardness L:s, UDP, temperaments at the cursor, nearest EDO).
//
// Output: plain values. The chart has no document dependency; it emits a MosSelection through onChange
// (MosGesture::Live while dragging, Commit on a click or a drag's release, Cancel on Esc), and T3.14's popover
// turns a Commit into a tuning command (one undo step). select() / clearSelection() set it from outside without
// emitting.
//
// Rendering: one widget, paints only what meets PaintContext::clip (bands, markers and labels are culled per item),
// and reports damage precisely: a generator change damages the marker's old and new columns, the pill label and the
// selected band (its colour follows the generator); a hover change damages the two bands and the info card; a view,
// size or selection change damages the whole chart. Layout (band rects, EDO markers, temperament rows, label
// strings) is rebuilt only when the view, the rect, the config or the selected region changes, never per paint.
// Geometry is logical px through UiScale (crisp primitives on device px), text through the theme's font roles.
// Colours: theme group `mosChart.*` (surfaces from the Live 12 look, Q47), the explorer's region palette
// (tuning::kMosPalette) and temperament colours (tuning::temperamentColor) for the data.
//
// UI thread only. Nothing here is RT-safe.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pq/tuning/mos.h"
#include "pq/tuning/mos_regions.h"
#include "pq/ui/widget.h"

namespace pq::views {

// The chart's output: one MOS tuning, as the explorer's selection + generator define it.
struct MosSelection {
  tuning::MosKey key;              // whole-scale counts (nL, nS), e.g. {5, 2}
  tuning::MosKey perPeriodKey;     // counts per period (= key when the period is the equave)
  int32_t periodsPerEquave = 1;
  double period = 1200.0;          // cents
  double generatorCents = 0.0;     // in [regionLow, regionHigh], 0 ≤ g ≤ period
  double regionLow = 0.0;          // the Stern–Brocot band (cents) the generator is in
  double regionHigh = 0.0;
  bool mirror = false;             // the band is the period − g side of its mediant
  int32_t modeRotation = 0;        // carried across a region crossing (transferModeRotation); 0 for a new MOS
  struct Edo {
    int32_t step = 0;
    int32_t edo = 0;               // per period
    friend bool operator==(const Edo&, const Edo&) = default;
  };
  std::optional<Edo> edo;          // the generator is exactly m·period/n for some n ≤ maxEdo

  int32_t notesPerPeriod() const noexcept { return perPeriodKey.size(); }
  tuning::RegionHint hint() const noexcept { return {perPeriodKey.size(), regionLow, regionHigh}; }
  // The tuning parameters (rotation, period, region hint) for tuning::buildFullScale / the document's MOS def.
  tuning::MosParams params() const;
  friend bool operator==(const MosSelection&, const MosSelection&) = default;
};

enum class MosGesture : uint8_t {
  Live,    // the generator is being dragged (preview; no undo step yet)
  Commit,  // a click, or the release that ends a drag: one undo step for the receiver
  Cancel,  // Esc during a drag: the selection is back to what it was at the press (the value passed)
};

struct MosChartConfig {
  double period = 1200.0;          // cents
  int32_t periodsPerEquave = 1;    // getPeriodRepetitions(): bands repeat this many times across the equave
  int32_t maxSize = 18;            // largest MOS size per period (rows; also the Stern–Brocot depth), 2..64
  int32_t maxEdo = 53;             // EDO markers m\n for n ≤ maxEdo (per period), 2..400
  double octaveStretch = 0.0;      // cents added to 1200 for the axis / marker labels only (stretchCents)
  friend bool operator==(const MosChartConfig&, const MosChartConfig&) = default;
};

// Layout metrics (logical px), the explorer's canvas constants.
namespace mos_chart_metrics {
inline constexpr float kMargin = 24.0f;             // MARGIN: cents → x inset on both sides of the plot
inline constexpr float kAxisY = 44.0f;              // axis bar centre (drawAxis Y)
inline constexpr float kAxisBarH = 22.0f;
inline constexpr float kAxisHitBottom = 55.0f;      // isOnAxisBar: my ≤ 55
inline constexpr float kEdoTop = 56.0f;             // drawEDOs baseY
inline constexpr float kEdoHitTop = 78.0f;          // marker hit box: baseY + 22 … + 62, |dx| < 24
inline constexpr float kEdoHitH = 40.0f;
inline constexpr float kEdoHitHalfW = 24.0f;
inline constexpr float kEdoMinPx = 48.0f;           // markers closer than 0.65 × this are dropped (lowest n wins)
inline constexpr float kTempTop = 124.0f;           // temperament rows (explorer: 120; our labels are 12–13 px)
inline constexpr float kTempBarH = 22.0f;
inline constexpr float kTempBarGap = 2.0f;
inline constexpr float kRowsTopIdle = 64.0f;        // first MOS row with nothing selected
inline constexpr float kRowUnit = 20.0f;            // row height = round(20 × rowScale), gap max(2, round(2 × rowScale))
inline constexpr float kDefaultRowScale = 1.32f;    // the explorer's S.rowScale
inline constexpr float kMinRowScale = 0.5f, kMaxRowScale = 4.0f;
inline constexpr float kSizeLabelRight = 16.0f;     // size labels right-aligned at x = 16
inline constexpr float kScrollbarW = 14.0f;         // SIZE_SB_WIDTH (the scrollbar sits right of the plot)
inline constexpr float kSnapPx = 6.0f;              // generator drag snaps to an EDO marker this close
inline constexpr float kMarkerGrabPx = 5.0f;        // a press this close to the marker line grabs it
inline constexpr float kClickSlop = 3.0f;           // a press that moves less is a click
inline constexpr double kZoomStep = 1.15;           // per wheel notch
inline constexpr double kMinViewCents = 0.3;
}  // namespace mos_chart_metrics

// An EDO marker as laid out (visible ones only, after the explorer's spacing rule).
struct MosChartEdoMarker {
  float x = 0.0f;          // device px
  double cents = 0.0;      // per-period generator cents (exactly step·period/edo)
  double shownCents = 0.0; // + k·period for the k-th repetition
  int32_t step = 0;
  int32_t edo = 0;
};

class MosChart : public ui::Widget {
public:
  explicit MosChart(std::string name = "mos-chart", const MosChartConfig& config = {});
  ~MosChart() override;

  // --- configuration (rebuilds the regions and EDO generators; keeps the selection like the explorer's recalc) ---
  void setConfig(const MosChartConfig& config);
  const MosChartConfig& config() const noexcept { return config_; }
  const tuning::MosRegionSet& regionSet() const noexcept { return regions_; }
  std::span<const tuning::EdoGenerator> edoGenerators() const noexcept { return edos_; }

  // --- view (not part of the output) ---
  // Visible cents range, clamped to [0, period × periodsPerEquave], at least kMinViewCents wide.
  void setView(double minCents, double maxCents);
  double viewMin() const noexcept { return viewMin_; }
  double viewMax() const noexcept { return viewMax_; }
  // First row's MOS size (the explorer's minSize), 2..maxSize.
  void setFirstSize(int32_t size);
  int32_t firstSize() const noexcept { return firstSize_; }
  void setRowScale(float scale);
  float rowScale() const noexcept { return rowScale_; }

  // --- selection ---
  // Selects the region of `perPeriodKey` containing `generatorCents` (reduced into the period; the nearest such
  // region when none contains it), scrolls its row into view and keeps `modeRotation`. No onChange. False when
  // the key has no region at this config.
  bool select(tuning::MosKey perPeriodKey, double generatorCents, int32_t modeRotation = 0);
  void clearSelection();
  const std::optional<MosSelection>& selection() const noexcept { return selection_; }
  int32_t selectedRegion() const noexcept { return selected_; }  // index into regionSet().regions, −1 = none
  int32_t hoveredRegion() const noexcept { return hovered_; }
  bool dragging() const noexcept { return drag_ != Drag::None; }

  using ChangeFn = std::function<void(const std::optional<MosSelection>&, MosGesture)>;
  void setOnChange(ChangeFn fn) { onChange_ = std::move(fn); }

  // --- geometry (device px, from the last layout; for tests and T3.14) ---
  double centsAtX(float x) const noexcept;
  float xOfCents(double cents) const noexcept;
  // The MOS size (per period) of the row at y, or −1 (gap, above the rows, below the last row).
  int32_t sizeAtY(float y) const noexcept;
  // The row of `size` across the plot ({} when it is not visible).
  ui::IRect rowRect(int32_t size) const noexcept;
  ui::IRect axisRect() const noexcept;
  ui::IRect scrollbarRect() const noexcept;
  ui::IRect plotRect() const noexcept;    // the band area (rect minus the scrollbar column)
  ui::IRect infoCardRect() const noexcept { return card_.rect; }  // {} when hidden
  // Region under p (index into regionSet().regions), −1 if none (the explorer's hitTest).
  int32_t regionAt(ui::Vec2 p) const noexcept;
  std::span<const MosChartEdoMarker> edoMarkers() const noexcept;
  // Hover card / info text lines (empty when nothing is hovered).
  std::span<const std::string> infoLines() const noexcept { return card_.lines; }

  struct Stats {
    uint64_t rebuilds = 0;      // setConfig: regions + EDO generators
    uint64_t layouts = 0;       // band / marker / temperament layout passes
    uint64_t paints = 0;
    uint32_t bandsPainted = 0;  // last paint
    uint32_t labelsPainted = 0;
  };
  const Stats& stats() const noexcept { return stats_; }

  // --- widget ---
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  void onHoverChanged(bool hovered) override;
  void onCaptureLost() override;
  ui::InfoText info() const override;
  // T4.16a: what a press does under the pointer: the generator axis / marker of the selection ResizeEW (drags the
  // generator), the scrollbar thumb ResizeNS, a band Pointer (selects it); during a drag the drag's own cursor.
  ui::Cursor cursorAt(ui::Vec2 pos) const override;

private:
  enum class Drag : uint8_t { None, Pending, Pan, Generator, Scrollbar };
  struct Band {           // one drawn repetition of a region in a visible row (device px)
    int32_t region;
    float x1, x2;         // clamped to the plot
    int32_t y, h;
    double oLow, oHigh;   // cents of this repetition
  };
  struct TempBar {
    int32_t temperament;  // index into tuning::temperaments()
    float x1, x2;
    int32_t y, h;
    ui::Rgba fill, edge, text;
  };
  struct Card {
    std::vector<std::string> lines;
    ui::IRect rect{};
  };
  struct Labels {  // a region's band texts: "5L 2s", "5L2s", "5L 2s (Diatonic)" (or empty), "685.7¢–720.0¢"
    std::string label, compact, name, range;
    bool ready = false;
  };
  struct Geo {            // device px
    ui::IRect r, plot;
    double x0 = 0, w = 1;  // c2x(c) = x0 + (c − viewMin)/(viewMax − viewMin) · w
    int32_t rowsTop = 0, rowH = 1, rowPitch = 1, rowCount = 0;
  };

  void rebuild();
  void ensureLayout() const;
  void layoutNow() const;
  void invalidate();  // layout stale + whole-chart damage
  void computeGeo(Geo& g) const noexcept;
  int32_t periodReps() const noexcept;
  double viewLimit() const noexcept;
  double reduce(double cents) const noexcept;
  bool sameRegion(int32_t a, int32_t b) const noexcept;
  void setSelectedRegion(int32_t region, double gen, int32_t rotation);
  void setGenerator(double gen);
  void emit(MosGesture g);
  void refreshSelectionValue();
  double snapGenerator(double gen, float pointerX) const noexcept;
  const MosChartEdoMarker* edoMarkerAt(ui::Vec2 p) const noexcept;
  // A press at p would drag the selected region's generator (on the axis inside the region, or on a marker).
  bool onGeneratorHandle(ui::Vec2 p) const noexcept;
  void handleClick(ui::Vec2 p);
  void dragGenerator(float x, bool snap);
  void updateHover(ui::Vec2 p, bool force);
  void buildCard(int32_t region, ui::Vec2 p);
  void hideCard();
  void damageRect(const ui::IRect& r);
  void damageGeneratorParts();
  void damageRegionBands(int32_t region);
  void setFirstSizeInternal(int32_t size, bool emitDamage);
  ui::Rgba regionColor(int32_t region) const noexcept;
  ui::IRect pillRect(float markerX) const;
  const Labels& labelsOf(int32_t region);
  ui::IRect thumbRect() const noexcept;

  MosChartConfig config_;
  tuning::MosRegionSet regions_;
  std::vector<tuning::EdoGenerator> edos_;
  std::vector<std::vector<int32_t>> bySize_;  // per-period size → region indices sorted by lowCents
  std::vector<ui::Rgba> palette_;             // tuning::kMosPalette as colours
  std::vector<Labels> labels_;  // per region, formatted lazily (labelsOf)

  double viewMin_ = 0.0, viewMax_ = 1200.0;
  int32_t firstSize_ = 2;
  float rowScale_ = mos_chart_metrics::kDefaultRowScale;

  int32_t selected_ = -1, hovered_ = -1;
  int32_t equalizedEndLow_ = 0;  // selected region: 1 = equalized at the low end
  std::optional<MosSelection> selection_;
  ChangeFn onChange_;

  // Layout cache (mutable: rebuilt lazily from paint / hit tests when stale).
  mutable bool layoutDirty_ = true;
  mutable ui::IRect layoutRect_{};
  mutable float layoutScale_ = 0.0f;
  mutable Geo geo_;
  mutable std::vector<Band> bands_;          // row by row
  mutable std::vector<int32_t> rowFirstBand_;  // row → first index in bands_ (rowCount + 1 entries)
  mutable std::vector<MosChartEdoMarker> markers_;
  mutable std::vector<std::string> markerLabels_;  // per marker: "18\31", "696.8¢", hardness "3:2" (3 strings each)
  mutable std::vector<TempBar> tempBars_;
  mutable int32_t tempRows_ = 0;
  mutable std::vector<int32_t> tempMatches_;  // temperaments of the selected region (cached per selection)
  mutable std::vector<double> tempLow_, tempHigh_;
  mutable int32_t tempFor_ = -2;

  // Interaction.
  Drag drag_ = Drag::None;
  ui::Vec2 pressPos_{};
  double pressViewMin_ = 0.0, pressViewMax_ = 0.0;
  int32_t pressFirstSize_ = 2;
  int32_t pressScrollOffset_ = 0;
  std::optional<MosSelection> pressSelection_;
  int32_t pressSelected_ = -1;
  bool movedSinceLive_ = false;
  Card card_;
  std::string infoTitle_, infoBody_;

  mutable Stats stats_;  // mutable: layout runs lazily from const geometry queries
};

}  // namespace pq::views
