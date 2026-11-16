#pragma once

// The reusable spectrum + response display (TX.spectrum; SPEC §11.1, §15 2026-11-01 "analysis modules"): what every
// signal display that shows a frequency spectrum draws — EQ Eight's and Auto Filter's panels (Device view and circuit
// panels), the plain "spectrum" panel display, the Spectrum meta module's face. PQ Meter's spectrum is the style guide
// (pq meter/src/modules/spectrum.cpp, ported in look and behaviour, our own code):
//
//   axis        the style's frequency axis (VZ.2: pq/dsp/freq_axis.h, Log / Mel / Linear, fMin … fMax; default log
//               10 Hz … 22 kHz; SpectrumAxis), dB window floor … ceiling. The snapshot carries the axis its points lie
//               on; the plot reads them through it, so a display on another axis (the EQ's) resamples
//   backdrop    three faint tinted columns (low / mid / high: edges at 250 Hz and 4 kHz, the palette at each third,
//               α 0.055) that fade out toward the bottom rows
//   grid        vertical rules from freqGridLines (the decade ladder on Log / Mel, even steps on Linear), horizontal
//               rules every 12 dB with their dB labels at the right edge (where the panel is wide enough)
//   input       optional: the device's INPUT spectrum as a faint grey curve behind (no glow) — what the device got
//   colour      PQ Meter's modes (VZ.2 S8): Colormap by level (default: Ice), Colormap by frequency (level as
//               brightness), Blend, Spectral, Heat, Solid, through a 64 × 128 (frequency × level) palette built when
//               the colour options change; the ten colormaps plus Custom (ui::levelGradient: stops in dB). Level is
//               normalised over the COLOUR window: its own Quiet … 0 dBFS (Loud pinned), or floor … ceiling while
//               the colours follow the scale
//   spectrum    the output (draw: Curve / Bars / Both). Curve: a gradient fill fading to the bottom, one optional glow
//               pass (level³; off by default), the antialiased core (ui::spectrumCurve); a vertex fades out over the
//               lowest 4 % of the range, so silence draws nothing. Bars (S11): every pixel column through the palette
//               by its own level, straight (no gamma), fading in over the 12 dB above the colour window's bottom; Both
//               stops them at the curve
//   peaks       optional: the peak hold through the knot spline (8 px knots), faded where it sits on the curve and
//               where it decays to the floor, rising instantly, falling eased (curve styles only)
//   average     optional (S6): the long-term average, amber, 1.8 px
//   guides      optional: the slope guide (dashed: a straight line on Log, the curve a dB/oct tilt is on Mel /
//               Linear, pivoted at 1 kHz on the middle of the range); the tilt tag top left (S7)
//   response    optional: the MEASURED response (|H| from the cross spectra, pq/dsp/transfer.h) on the response axis
//               (`respTopDb … respBottomDb`), violet, each vertex's alpha its confidence: no curve on silence, on
//               bands the input does not excite, or where the output does not follow the input
//   labels      frequency labels on the bottom edge on dark backing pads, placed by priority (decades first) with an
//               occupancy test so they never overlap at any width; 0 / 10 keep their rules but no label
//   hover       the pointer's frequency (Hz / kHz), its nearest note (C3 = 261.626 Hz naming) with cents, the level
//               under it and, while shown, the measured response there; a faint vertical line
//   ease        45 ms per displayed frame toward the analyser's snapshot (frame-rate independent; peaks rise at once)
//
// The plot paints into a draw list at a device-px rect, so a panel (pq/ui widgets) and a vector face (a FacePainter's
// list) draw the same picture. It holds its feed while shown (SpectrumFeed::acquire with what the style draws) and
// lets go while hidden; a watched feed's waker restarts its frames when sound arrives (pq/views/devices/analysis.h).
//
// UI thread only. Painting allocates nothing once its scratch has grown.

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "pq/ui/draw_list.h"
#include "pq/ui/theme.h"
#include "pq/views/devices/analysis.h"

namespace pq::views::devices {

// The displays' frequency axis across [x0, x1] (device px): `freq` (default log 10 Hz … 22 kHz). EQ.4: with
// `offsetHz` > 0 the response displays' axis instead: x ∝ log(1 + f / offsetHz) over 0 … topHz (0 Hz at x0).
struct SpectrumAxis {
  static constexpr float kLoHz = 10.0f, kHiHz = 22000.0f;
  float x0 = 0, x1 = 1;
  float offsetHz = 0.0f, topHz = kHiHz;
  dsp::FreqAxis freq{dsp::FreqScale::Log, kLoHz, kHiHz};
  float x(float hz) const noexcept;
  float hz(float px) const noexcept;
};

// VZ.2 S8: how a point picks its colour (PQ Meter's menu order). APPEND-ONLY (instance data stores the index).
enum class SpectrumColour : uint8_t { Colormap, ColormapFreq, Blend, Spectral, Heat, Solid };
inline constexpr int kSpectrumColourCount = 6;
enum class SpectrumDraw : uint8_t { Curve, Bars, Both };

struct SpectrumPlotStyle {
  float floorDb = -78.0f, ceilDb = 0.0f;  // SPEC §11.1
  float glow = 0.0f, fill = 0.5f;          // PQ Meter's glow amount (0: off, its default) and fill alpha
  dsp::FreqAxis axis;                      // paintAll's frequency axis
  SpectrumDraw draw = SpectrumDraw::Curve;
  SpectrumColour colour = SpectrumColour::Colormap;
  int colormap = 4;                        // ui::Colormap (Ice), or ui::Gradient::kCustomIndex
  int customBase = 4;                      // Custom: the colormap its stop colours come from
  float stopsDb[4] = {-60.0f, -40.0f, -20.0f, -8.0f};  // Custom: the middle stops (dB)
  bool colourFollow = false;               // the colours span floor … ceiling (else colourLoDb … 0 dBFS)
  float colourLoDb = -80.0f;
  bool average = false;                    // the average line (when the snapshot has one)
  bool slopeGuide = false;
  bool tiltTag = true;
  float tiltDbPerOct = 0.0f;               // the analysis' tilt (the tag and the guide)
  float lineWidth = 1.6f;                  // logical px
  bool bands = true;                       // the low / mid / high backdrop
  bool grid = true;                        // frequency + 12 dB rules
  bool freqLabels = true, dbLabels = true;
  bool peaks = false;                      // peak hold
  bool input = false;                      // the input spectrum behind the output
  bool response = false;                   // the measured response
  float respTopDb = 24.0f, respBottomDb = -48.0f;  // the response's dB axis (an EQ display: its ± range)
  float respInset = 2.0f;                  // device px kept free above the top and below the bottom of that axis
  float alpha = 1.0f;                      // EQ.4: the spectrum's opacity (a display that draws curves over it)
};

// How the plot writes text (a panel: the tree's fonts; a face: its painter). width: device px of a string (null: an
// estimate); draw: at a top-left corner (device px).
struct PlotText {
  std::function<float(std::string_view)> width;
  std::function<void(ui::Vec2 topLeft, std::string_view, ui::Rgba)> draw;
  float lineHeight = 12.0f;  // device px
};

class SpectrumPlot {
public:
  SpectrumPlot() = default;
  ~SpectrumPlot();
  SpectrumPlot(const SpectrumPlot&) = delete;
  SpectrumPlot& operator=(const SpectrumPlot&) = delete;

  // The feed to show (null: the grid only) and how to ask the owner for a frame (also the feed's waker).
  void setFeed(SpectrumFeed* feed, std::function<void()> requestFrame);
  SpectrumFeed* feed() const noexcept { return feed_; }
  bool acquired() const noexcept { return acquired_; }
  // What the style draws decides what the feed computes; a change re-acquires.
  void setStyle(const SpectrumPlotStyle& s);
  const SpectrumPlotStyle& style() const noexcept { return style_; }

  // From the owner's paint, before it draws (only reached while shown): hold the feed, and when nothing is shown yet
  // show what it holds now (a display that just appeared draws its first frame complete).
  void wake();
  // From the owner's frame (`shown`: the owner is in a visible part of its tree). Returns true when the drawing changed;
  // asks for the next frame while something moves, else rests (a watched feed keeps its reader; another lets go).
  bool frame(double now, bool shown);
  // Lets go of the feed (the owner was hidden).
  void release();

  // ---- painting (device px; `scale` = UI scale: line widths and pads) ----
  void paintBackdrop(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax) const;
  void paintGrid(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, const ui::Theme& th, float scale,
                 bool dbRules = true) const;
  // The input (if styled), the output spectrum and the peaks (if styled).
  void paintSpectrum(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, float scale);
  // The measured response (if styled and known) on the response axis of r.
  void paintResponse(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, float scale);
  void paintLabels(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, const ui::Theme& th, float scale,
                   const PlotText& text) const;
  // The pointer readout at device x `px` (inside r).
  void paintHover(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, float px, float scale,
                  const PlotText& text) const;
  // Everything in order: backdrop, grid, spectrum, response, labels (hoverX ≥ 0: the readout).
  void paintAll(ui::DrawList& dl, const ui::Rect& r, const ui::Theme& th, float scale, const PlotText& text,
                float hoverX = -1.0f);

  // ---- what is shown ----
  bool hasLevels() const noexcept { return !shown_.out.empty(); }
  // The shown (eased) output level at hz (dB; the floor when nothing is shown); the measured response there (dB) and
  // its confidence (0: none).
  float levelAt(float hz) const noexcept;
  float responseAt(float hz, float* confidence = nullptr) const noexcept;
  // The response's y for dB value `db` in r.
  float responseY(const ui::Rect& r, float db) const noexcept;
  float levelY(const ui::Rect& r, float db) const noexcept;

  // Tests: feeds the eased state directly (no feed).
  void setShownForTest(SpectrumSnapshot s, float pointCount);
  // The colour of a point at axis fraction ft and level db (the palette; alpha 255).
  ui::Rgba colourAt(float ft, float db) const noexcept;

private:
  uint32_t wants() const noexcept;
  float sampleAt(const std::vector<float>& v, float hz) const noexcept;
  void acquire();
  void rebuildPalette() const;
  float colourT(float db) const noexcept;

  SpectrumFeed* feed_ = nullptr;
  std::function<void()> requestFrame_;
  SpectrumPlotStyle style_;
  bool acquired_ = false;
  uint32_t acquiredWants_ = 0;
  uint64_t seq_ = 0;
  double last_ = 0.0;
  SpectrumSnapshot target_, shown_;
  // The palette (64 frequencies × 128 levels) and what it was built for.
  static constexpr int kPalF = 64, kPalL = 128;
  mutable std::vector<ui::Rgba> palette_;
  mutable SpectrumPlotStyle palFor_;
  mutable bool palValid_ = false;
  // Scratch.
  std::vector<ui::Vec2> poly_;
  std::vector<ui::Rgba> cols_;
  std::vector<float> lvl_, vis_, pk_, pkSm_, db_;
  std::vector<ui::Rgba> tmp_;
};

}  // namespace pq::views::devices
