// The Wavetable face (TMM.7, docs/plans/MM.md "Wavetable face", "Per-voice dots"; the user's sketch rotated upright,
// the display after Live's Wavetable). Label-free, in the Phase accumulator's language (the same theme tokens: a dark
// body, quiet tracks and frame, values in the accent, voices as neon-blue glowing dots).
//
//   top        the Phase / Shape mode switch by the input jack; the draw tools (pencil, line, smooth curve: TMM.9);
//              the Basic Shapes chooser (replaces the edited keyframe's cycle)
//   harmonics  TMM.9, Serum-like: the edited keyframe's harmonics as two rows of bars — amplitude (linear, 1 = full
//              height; Alt shows dB, −60 … 0) and phase about a centre line (relative to a sine) —, 32 visible at first;
//              the wheel scrolls, Ctrl+wheel zooms (8 … all 1024: DC and harmonics 1 … 1023), a thin scrollbar below
//              shows where the view is. Dragging paints across the bars (Shift: a straight line from the press),
//              right-click / right-drag or a double-click resets bars (amplitude 0, phase 0). Bars too thin for the
//              screen are drawn as columns of the loudest bar they cover.
//   display    (a draw tool chosen: the edited keyframe's cycle, large, on a grid, with the snap grid / normalise /
//              remove-DC options in its corner; drag to draw — see wavetable_edit.h; click the lit tool or Esc to leave)
//              the table's dense frames as offset lines receding up-right (a static layer keyed by the instance's
//              data and baked values), the frame at the position highlighted in the accent with a fill; with no voice,
//              a dashed line at the phase slider's read phase; per sounding voice a glowing dot on its own frame at
//              the newest sampled read phase, a dotted vertical bar, and the faint glow density of the phases sampled
//              during the last displayed frame (the dot is a sample; the glow shows where the reads landed). Voices
//              fade in and out over 80 ms.
//   right      the position slider with the keyframe markers (click selects, drag moves — Ctrl snaps to dense frames —,
//              double-click on the track adds a keyframe from the frame there, Delete removes the selected one but
//              never the last) and a glyph per gap toggling spectral ↔ crossfade
//   below      the phase slider (the phase input's base value; per-voice ticks when the input is wired)
//   bottom     fold / warp / sync knobs, the warp mode as a 3-glyph switch under the warp knob
//
// The EDITED KEYFRAME (harmonics, drawing, basic shapes) is the selected one (a marker click selects). Once the
// position slider has been dragged, it is the keyframe at the position instead — and with the position between
// keyframes, a hollow marker shows where the first edit creates one from the blended frame (the plan: "the first
// stroke creates one there"). Drawing and the harmonic rows are two views of the same 2048 samples: viewing writes
// nothing, a harmonic edit changes only the harmonics edited (wavetable_edit.h).
//
// Every drag and click is one undo step and never a compile: params are runtime values, fold / warp / sync are baked by
// the link off the audio thread, data edits are the MetaData class. The warp mode is a setting (one compile, as every
// setting; TMM.8's live path reads it). Shape mode is refused until TMM.8 (Q171): its switch says so.
//
// World units (204 × 248): see the layout constants below.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

#include "faces.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/meta/wavetable.h"
#include "wavetable_edit.h"
#include "wavetable_model.h"

namespace pq::views {

namespace {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float deg(float d) noexcept { return d * kPi / 180.0f; }

// ---- layout (world units) ----
constexpr float kW = 204.0f, kH = 248.0f;
constexpr float kRadius = 12.0f;
// top row: the mode switch by the input jack, the draw tools, the Basic Shapes chooser at the right
constexpr float kRowY0 = 17.0f, kRowY1 = 31.0f;
constexpr float kModeX0 = 14.0f, kCell = 14.0f, kCellGap = 1.5f;
constexpr float kSmallCell = 13.0f;  // tools and shapes
constexpr float kToolX0 = 51.0f;
constexpr uint32_t kToolCount = 3;
constexpr float kShapeX1 = 188.0f;
constexpr uint32_t kShapeCount = 6;
// harmonic rows and their scrollbar
constexpr float kHarmX0 = 16.0f, kHarmX1 = 188.0f;
constexpr float kAmpY0 = 36.0f, kAmpY1 = 58.0f;
constexpr float kPhY0 = 60.5f, kPhY1 = 71.5f;
constexpr float kScrollY = 73.7f, kScrollHalfH = 0.75f;
constexpr uint32_t kMinBars = 8, kDefaultBars = 32;
// draw mode: the cycle's place in the display, the option cells in its corner
constexpr float kDrawX0 = 20.0f, kDrawW = 138.0f, kDrawCy = 129.0f, kDrawA = 35.0f;
constexpr float kOptX0 = 19.0f, kOptW = 12.0f, kOptY0 = 80.5f, kOptY1 = 90.5f;
// display
constexpr Rect kDisp{16.0f, 78.0f, 162.0f, 172.0f};
// position slider (right of the display), its markers and gap glyphs
constexpr float kPosX = 178.0f, kPosHalfW = 2.0f;
constexpr float kMarkTipX = 174.5f, kMarkBaseX = 168.5f, kMarkHalfH = 3.2f;
constexpr float kGapX = 192.0f;
// phase slider (below the display)
constexpr float kPhaseY = 182.0f, kPhaseHalfH = 2.0f;
// knobs
constexpr float kKnobY = 212.0f, kKnobR = 12.0f, kKnobTrackW = 3.5f;
constexpr float kKnobX[3] = {38.0f, 89.0f, 140.0f};
constexpr float kKnobA0 = 135.0f, kKnobSweep = 270.0f;
constexpr float kGlyphY = 233.0f;
constexpr float kWarpCellW = 13.0f, kWarpCellY0 = 227.0f, kWarpCellY1 = 239.0f;

constexpr float kFade = 0.08f;  // voices fade in / out (seconds)

// ---- parts ----
enum Part : int32_t {
  kPhasePart = 0,
  kPositionPart = 1,
  kFoldPart = 2,
  kWarpPart = 3,
  kSyncPart = 4,
  kWarpMode0 = 10,  // + mode
  kModePhase = 20,
  kModeShape = 21,
  kShape0 = 30,  // + dsp::BasicCycle
  kHarmAmp = 40,     // the amplitude row
  kHarmPhase = 41,   // the phase row
  kHarmScroll = 42,  // the harmonic view's scrollbar
  kTool0 = 50,       // + tool − 1 (pencil, line, curve)
  kDrawPart = 60,    // the display in draw mode
  kGridPart = 61,
  kNormPart = 62,
  kDcPart = 63,
  kKeyBase = 1000,  // + keyframe
  kGapBase = 3000,  // + gap
};

Rect modeCell(uint32_t i) noexcept {
  const float x0 = kModeX0 + static_cast<float>(i) * (kCell + kCellGap);
  return {x0, kRowY0, x0 + kCell, kRowY1};
}
Rect shapeCell(uint32_t i) noexcept {
  const float x0 =
      kShapeX1 - static_cast<float>(kShapeCount - i) * kSmallCell - static_cast<float>(kShapeCount - 1 - i) * kCellGap;
  return {x0, kRowY0, x0 + kSmallCell, kRowY1};
}
Rect toolCell(uint32_t i) noexcept {
  const float x0 = kToolX0 + static_cast<float>(i) * (kSmallCell + kCellGap);
  return {x0, kRowY0, x0 + kSmallCell, kRowY1};
}
Rect optCell(uint32_t i) noexcept {
  const float x0 = kOptX0 + static_cast<float>(i) * (kOptW + kCellGap);
  return {x0, kOptY0, x0 + kOptW, kOptY1};
}
Rect warpCell(uint32_t i) noexcept {
  const float total = 3.0f * kWarpCellW + 2.0f * kCellGap;
  const float x0 = kKnobX[1] - total * 0.5f + static_cast<float>(i) * (kWarpCellW + kCellGap);
  return {x0, kWarpCellY0, x0 + kWarpCellW, kWarpCellY1};
}
bool inside(const Rect& r, Vec2 p, float slack = 0.0f) noexcept {
  return p.x >= r.x0 - slack && p.x <= r.x1 + slack && p.y >= r.y0 - slack && p.y <= r.y1 + slack;
}
Vec2 centreOf(const Rect& r) noexcept { return {(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}; }

float posY(float u) noexcept { return kDisp.y1 - u * (kDisp.y1 - kDisp.y0); }
float phaseX(float ph) noexcept { return kDisp.x0 + ph * (kDisp.x1 - kDisp.x0); }
float knobAngle(float t) noexcept { return deg(kKnobA0 + kKnobSweep * t); }
Vec2 polarAt(Vec2 c, float r, float a) noexcept { return {c.x + r * std::cos(a), c.y + r * std::sin(a)}; }

// A frame's place in the display: x0 + φ·w, centre line cy, half-height a (value 1).
struct FrameGeom {
  float x0, w, cy, a;
};

// ---- glyphs (drawn into a cell: centre c, half-width hw, half-height hh) ----
enum class Glyph : uint8_t {
  Sine, Triangle, Saw, Square, Pulse, Noise, PhaseMode, ShapeMode, Bend, Mirror, PulseWidth, Fold, Sync,
  Pencil, Line, Curve, Normalise, RemoveDc  // TMM.9 (the grid glyph: gridGlyph)
};

class Glyphs {
public:
  void draw(ui::FacePainter& p, Glyph g, Vec2 c, float hw, float hh, float th, Rgba col) {
    pts_.clear();
    auto at = [&](float x, float y) { pts_.push_back({c.x - hw + 2.0f * hw * x, c.y - hh * y}); };
    switch (g) {
      case Glyph::Sine:
        for (int i = 0; i <= 16; ++i) at(i / 16.0f, std::sin(2.0f * kPi * i / 16.0f));
        break;
      case Glyph::Triangle: at(0, 0), at(0.25f, 1), at(0.75f, -1), at(1, 0); break;
      case Glyph::Saw: at(0, -1), at(1, 1), at(1, -1); break;
      case Glyph::Square: at(0, 1), at(0.5f, 1), at(0.5f, -1), at(1, -1); break;
      case Glyph::Pulse: at(0, 1), at(0.25f, 1), at(0.25f, -1), at(1, -1); break;
      case Glyph::Noise: {
        static constexpr float kN[] = {0.1f, 0.8f, -0.5f, 0.4f, -0.9f, 0.6f, -0.2f, 1.0f, -0.7f, 0.2f, -0.4f};
        for (int i = 0; i < 11; ++i) at(i / 10.0f, kN[i]);
        break;
      }
      case Glyph::PhaseMode: at(0, -1), at(0.5f, 1), at(0.5f, -1), at(1, 1), at(1, -1); break;  // phase ramps, wrapping
      case Glyph::ShapeMode:  // a transfer curve
        for (int i = 0; i <= 12; ++i) {
          const float x = i / 12.0f;
          at(x, std::tanh(2.6f * (2.0f * x - 1.0f)) / std::tanh(2.6f));
        }
        break;
      case Glyph::Bend:
        for (int i = 0; i <= 12; ++i) {
          const float x = i / 12.0f;
          at(x, 2.0f * (x / (0.28f + 0.72f * x)) - 1.0f);
        }
        break;
      case Glyph::Mirror: at(0, -1), at(0.5f, 1), at(1, -1); break;
      case Glyph::PulseWidth: at(0, -1), at(0.12f, -1), at(0.12f, 1), at(0.36f, 1), at(0.36f, -1), at(1, -1); break;
      case Glyph::Fold:
        for (int i = 0; i <= 20; ++i) {
          const float x = i / 20.0f;
          at(x, std::sin(2.4f * std::sin(2.0f * kPi * x)) / std::sin(2.4f > kPi * 0.5f ? 1.0f : 2.4f));
        }
        break;
      case Glyph::Sync: at(0, -1), at(0.4f, 0.6f), at(0.4f, -1), at(0.8f, 0.6f), at(0.8f, -1), at(1, -0.2f); break;
      case Glyph::Pencil: {  // a freehand scribble ending in a dot
        static constexpr float kP[][2] = {{0, -0.2f}, {0.12f, 0.5f}, {0.22f, 0.7f}, {0.32f, 0.1f}, {0.42f, -0.7f},
                                         {0.55f, -0.5f}, {0.66f, 0.4f}, {0.78f, 0.75f}, {0.9f, 0.2f}};
        for (const auto& q : kP) at(q[0], q[1]);
        p.polyline(pts_, th, col);
        p.disc(pts_.back(), th * 1.1f, col);
        return;
      }
      case Glyph::Line:  // two points joined
        at(0.08f, -0.7f), at(0.92f, 0.7f);
        p.polyline(pts_, th, col);
        p.disc(pts_[0], th * 1.25f, col);
        p.disc(pts_[1], th * 1.25f, col);
        return;
      case Glyph::Curve: {  // a smooth S through three points
        for (int i = 0; i <= 16; ++i) {
          const float x = i / 16.0f;
          at(0.06f + 0.88f * x, -0.75f * std::cos(kPi * x));
        }
        p.polyline(pts_, th, col);
        p.disc(pts_[0], th * 1.1f, col);
        p.disc(pts_[8], th * 1.1f, col);
        p.disc(pts_[16], th * 1.1f, col);
        return;
      }
      case Glyph::Normalise:  // a wave touching both rails
        p.line({c.x - hw, c.y - hh}, {c.x + hw, c.y - hh}, th * 0.8f, col);
        p.line({c.x - hw, c.y + hh}, {c.x + hw, c.y + hh}, th * 0.8f, col);
        for (int i = 0; i <= 16; ++i) at(i / 16.0f, 0.9f * std::sin(2.0f * kPi * i / 16.0f));
        break;
      case Glyph::RemoveDc: {  // a wave centred on its (dashed) centre line
        const Vec2 mid[2] = {{c.x - hw, c.y}, {c.x + hw, c.y}};
        p.dashed(mid, th * 0.8f, 1.2f, 1.0f, col);
        for (int i = 0; i <= 16; ++i) at(i / 16.0f, 0.75f * std::sin(2.0f * kPi * i / 16.0f));
        break;
      }
    }
    p.polyline(pts_, th, col);
  }
  // The snap grid's glyph: a box with 1 / 2 / 3 / 4 inner lines each way (off: a plain box with one).
  static void gridGlyph(ui::FacePainter& p, Vec2 c, float hw, float hh, float th, uint32_t grid, Rgba col) {
    const int n = grid == 0 ? 1 : (grid <= 8 ? 2 : (grid <= 16 ? 3 : 4));
    const float x0 = c.x - hw, x1 = c.x + hw, y0 = c.y - hh, y1 = c.y + hh;
    for (int i = 0; i <= n + 1; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(n + 1);
      const float w = (i == 0 || i == n + 1) ? th : th * 0.7f;
      p.line({x0 + t * (x1 - x0), y0}, {x0 + t * (x1 - x0), y1}, w, col);
      p.line({x0, y0 + t * (y1 - y0)}, {x1, y0 + t * (y1 - y0)}, w, col);
    }
  }

private:
  std::vector<Vec2> pts_;
};

class WavetableFace final : public MetaFace {
public:
  explicit WavetableFace(const meta::Type& t) : MetaFace(t) {
    curve_.reserve(wt::kPoints + 1);
    blend_.resize(wt::kPoints + 1);
    shown_.reserve(2 * kMaxVoices);
  }

  Vec2 worldSize() const noexcept override { return {kW, kH}; }

  // Inputs on the left edge: x by the mode switch (the sketch), pos beside the display; a wired param's pin by its
  // control (phase by the phase slider, position level with the display, the knobs' by the knob row).
  float inputY(size_t i, size_t n) const noexcept override {
    if (n != type().inputs.size() + type().params.size()) return MetaFace::inputY(i, n);
    static constexpr float kY[] = {24.0f, 140.0f, kPhaseY, 112.0f, 205.0f, 219.0f, 233.0f};
    return i < std::size(kY) ? kY[i] : MetaFace::inputY(i, n);
  }
  // The output leaves the display's row at the right edge, past the position slider.
  float outputY(size_t, size_t) const noexcept override { return (kDisp.y0 + kDisp.y1) * 0.5f; }

  // ---- static layers: body, tracks, frame + glyphs (shared by the type); this instance's stacked frames (or, in
  // draw mode, the drawing grid), the edited keyframe's harmonic bars (amplitude, phase) and the harmonic view's slots
  // — data and view state, not values: drawn once into tiles, not per frame ----
  uint32_t staticLayerCount() const noexcept override { return 7; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    switch (layer) {
      case 0: return th.face.body;
      case 1: return th.face.track;
      case 2: return th.face.frame;
      case 3: return th.face.tick;
      case 4: return th.face.track;  // the harmonic slots (under the bars)
      case 5: return th.face.value;
      default: return th.face.handle;
    }
  }
  uint64_t staticKey(uint32_t layer) const override {
    if (layer < 3) return 0;
    refresh();
    const uint64_t view = (static_cast<uint64_t>(hFirst_) << 20) ^ (static_cast<uint64_t>(hCount_) << 8);
    switch (layer) {
      case 3: return tool_ != wt::Tool::None ? (0xC2B2AE3D27D4EB4Full ^ (static_cast<uint64_t>(grid_) + 1)) | 1u : model_.key();
      case 4: return (view * 0x2545F4914F6CDD1Dull) | 1u;
      case 5: return (targetKey_ ^ view ^ (alt_ ? 0x9E3779B97F4A7C15ull : 0u)) | 1u;
      default: return (targetKey_ ^ view ^ 0x5851F42D4C957F2Dull) | 1u;
    }
  }

  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    switch (layer) {
      case 0: p.roundedRect({1.0f, 1.0f, kW - 1.0f, kH - 1.0f}, kRadius, w); return;
      case 1: paintTracks(p, w); return;
      case 2: paintFrame(p, w); return;
      case 3:
        if (tool_ != wt::Tool::None) paintGrid(p);
        else paintStack(p);
        return;
      case 4: paintSlots(p); return;
      case 5:
      default: paintHarmonics(p, layer == 5); return;
    }
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    refresh();
    const ui::Theme& th = *env.theme;
    const double now = clockNow();
    paintCells(p, th);
    paintHarmonicLive(p, th);
    paintSliders(p, env);
    paintKnobs(p, env);
    if (tool_ != wt::Tool::None) paintDrawMode(p, th);
    else paintDisplay(p, env, now);
    paintOutputLead(p, env);
    // The selection / hover ring.
    const Rgba ring = env.selected ? th.circuit.selected : (env.hovered && hovered_ == kFrame ? th.circuit.hover : 0);
    outline(p, 0.75f, env.selected ? 1.75f : 1.25f, ring);
  }

  void liveRects(std::vector<Rect>& out) const override {
    out.push_back({kDisp.x0 - 10.0f, kDisp.y0 - 10.0f, kDisp.x1 + 10.0f, kDisp.y1 + 10.0f});
    out.push_back({kDisp.x0 - 4.0f, kPhaseY - 7.0f, kDisp.x1 + 4.0f, kPhaseY + 7.0f});
  }

  bool animating() const noexcept override { return voiceCount_ > 0 || !shown_.empty(); }

  // ---- interaction ----
  // Exact hits first; only where nothing is hit are the targets widened (at small zooms a few device px must still hit
  // a control, but a widened target must never shadow its exactly hit neighbour).
  int32_t hitTest(Vec2 l, float ppu) const noexcept override {
    if (l.x < 0.0f || l.y < 0.0f || l.x > kW || l.y > kH) return kNone;
    refresh();  // the keyframes may have changed since the last paint (undo, another view)
    const int32_t exact = hitAt(l, 0.0f);
    if (exact != kFrame) return exact;
    const float slack = std::min(12.0f, std::max(0.0f, 6.0f / std::max(ppu, 1e-6f) - 3.0f));
    return slack > 0.0f ? hitAt(l, slack) : kFrame;
  }

  int32_t hitAt(Vec2 l, float slack) const noexcept {
    const meta::WavetableData& d = model_.data();
    // Keyframe markers (the nearest within reach), then the position track, then the gap glyphs.
    if (l.x >= kMarkBaseX - 2.0f - slack && l.x <= kMarkTipX + 0.5f && l.y >= kDisp.y0 - 5.0f && l.y <= kDisp.y1 + 5.0f) {
      int32_t best = -1;
      float bestD = kMarkHalfH + 1.5f + slack;
      for (size_t k = 0; k < d.keyframes.size(); ++k) {
        const float dy = std::fabs(l.y - posY(d.keyframes[k].position));
        if (dy <= bestD) {
          bestD = dy;
          best = static_cast<int32_t>(k);
        }
      }
      if (best >= 0) return kKeyBase + best;
    }
    if (l.x >= kPosX - 5.0f - slack * 0.5f && l.x <= kPosX + 6.0f + slack * 0.5f && l.y >= kDisp.y0 - 3.0f && l.y <= kDisp.y1 + 3.0f)
      return kPositionPart;
    if (l.x > kPosX + 6.0f && l.x <= kW - 2.0f)
      for (size_t g = 0; g + 1 < d.keyframes.size(); ++g)
        if (gapShown(g) && std::fabs(l.y - gapY(g)) <= 4.5f + slack) return kGapBase + static_cast<int32_t>(g);
    if (l.y >= kPhaseY - 5.0f - slack && l.y <= kPhaseY + 5.0f + slack && l.x >= kDisp.x0 - 2.0f && l.x <= kDisp.x1 + 2.0f)
      return kPhasePart;
    for (int i = 0; i < 3; ++i) {
      const float dx = l.x - kKnobX[i], dy = l.y - kKnobY;
      if (dx * dx + dy * dy <= (kKnobR + 4.0f + slack) * (kKnobR + 4.0f + slack)) return kFoldPart + i;
    }
    for (uint32_t i = 0; i < 3; ++i)
      if (inside(warpCell(i), l, slack * 0.5f)) return kWarpMode0 + static_cast<int32_t>(i);
    for (uint32_t i = 0; i < 2; ++i)
      if (inside(modeCell(i), l, slack * 0.5f)) return kModePhase + static_cast<int32_t>(i);
    for (uint32_t i = 0; i < kShapeCount; ++i)
      if (inside(shapeCell(i), l, slack * 0.5f)) return kShape0 + static_cast<int32_t>(i);
    for (uint32_t i = 0; i < kToolCount; ++i)
      if (inside(toolCell(i), l, slack * 0.5f)) return kTool0 + static_cast<int32_t>(i);
    if (l.x >= kHarmX0 - 1.0f && l.x <= kHarmX1 + 1.0f) {
      if (l.y >= kAmpY0 - 1.5f && l.y <= 0.5f * (kAmpY1 + kPhY0)) return kHarmAmp;
      if (l.y > 0.5f * (kAmpY1 + kPhY0) && l.y <= kPhY1 + 0.9f) return kHarmPhase;
      if (l.y > kPhY1 + 0.9f && l.y <= kScrollY + 1.8f) return kHarmScroll;
    }
    if (tool_ != wt::Tool::None) {
      for (uint32_t i = 0; i < 3; ++i)
        if (inside(optCell(i), l, slack * 0.5f)) return kGridPart + static_cast<int32_t>(i);
      if (inside(kDisp, l, 2.0f)) return kDrawPart;
    }
    return kFrame;
  }

  bool onPointer(const FacePointer& e) override {
    using K = FacePointer::Kind;
    switch (e.kind) {
      case K::Down: return down(e);
      case K::Move: return move(e);
      case K::Up: {
        if (drag_ == kNone) return false;
        if (begun_) target().end();
        drag_ = kNone;
        begun_ = false;
        return true;
      }
      case K::Wheel: return wheel(e);
      case K::Leave: return false;
    }
    return false;
  }

  bool dragging() const noexcept override { return drag_ != kNone; }
  void cancelDrag() override {
    if (drag_ != kNone && begun_) target().cancel();
    drag_ = kNone;
    begun_ = false;
  }

  bool onKey(const ui::KeyEvent& e) override {
    if (!e.down || !keyFocus_) return false;
    if ((e.mods & ui::mod::kChordMask) != ui::mod::kNone) return false;
    if (e.key == ui::key::kEscape && tool_ != wt::Tool::None && drag_ == kNone) {  // leave draw mode
      tool_ = wt::Tool::None;
      return true;
    }
    if (!markerFocus_ || (e.key != ui::key::kDelete && e.key != ui::key::kBackspace)) return false;
    refresh();
    const meta::WavetableData& d = model_.data();
    if (!model_.ok() || !target().editable()) return false;
    if (d.keyframes.size() <= 1) {
      target().notice("A wavetable keeps at least one keyframe");
      return true;
    }
    meta::WavetableData n = d;
    const size_t k = std::min<size_t>(static_cast<size_t>(selected_), n.keyframes.size() - 1);
    n.keyframes.erase(n.keyframes.begin() + static_cast<std::ptrdiff_t>(k));
    if (!n.gaps.empty()) {
      // The gaps either side of it become one, which keeps the earlier one's mode (the ends: their only gap goes).
      n.gaps.resize(d.keyframes.size() - 1, dsp::GapMode::Spectral);
      const size_t g = k == 0 ? 0 : (k + 1 == d.keyframes.size() ? k - 1 : k);
      n.gaps.erase(n.gaps.begin() + static_cast<std::ptrdiff_t>(g));
    }
    selected_ = static_cast<int32_t>(std::min(k, n.keyframes.size() - 1));
    commitData("Delete Keyframe", n);
    return true;
  }

  ui::Cursor cursor(int32_t part) const noexcept override {
    if (part == kPhasePart) return ui::Cursor::ResizeEW;
    if (part == kPositionPart || (part >= kKeyBase && part < kGapBase) || (part >= kFoldPart && part <= kSyncPart))
      return ui::Cursor::ResizeNS;
    if (part == kHarmAmp || part == kHarmPhase || part == kDrawPart) return ui::Cursor::Crosshair;
    if (part == kHarmScroll) return ui::Cursor::ResizeEW;
    if (part >= kWarpMode0) return ui::Cursor::Pointer;  // switches, shapes, tools, options, gap glyphs
    return ui::Cursor::Inherit;
  }

  void describe(int32_t part, std::string& title, std::string& body) const override;

protected:
  bool voicesPolled(bool changed) override;

private:
  // ---- state from the target ----
  double param(uint32_t i) const { return target().value(i); }
  void refresh() const {
    wt::Bake b;
    b.fold = static_cast<float>(param(meta::kWtFold));
    b.warp = static_cast<float>(param(meta::kWtWarp));
    b.sync = static_cast<float>(param(meta::kWtSync));
    b.warpMode = static_cast<uint32_t>(std::clamp(std::round(target().setting(meta::kWtWarpMode)), 0.0, 2.0));
    (void)model_.update(target().data(), b);
    const meta::WavetableData& d = model_.data();
    const auto n = static_cast<int32_t>(d.keyframes.size());
    selected_ = std::clamp(selected_, 0, std::max(n - 1, 0));
    // The edited keyframe: the selected one, or (after a position drag) the one at the position — none there: the
    // blended frame at the position, which the first edit turns into a keyframe.
    virtual_ = false;
    if (atPosition_ && model_.ok() && n > 1) {
      const float u = std::clamp(static_cast<float>(param(meta::kWtPosition)), 0.0f, 1.0f);
      const float tol = 0.5f / static_cast<float>(std::max<uint32_t>(model_.frames(), 2) - 1);
      int32_t best = -1;
      float bestD = tol;
      for (int32_t k = 0; k < n; ++k) {
        const float dk = std::fabs(d.keyframes[static_cast<size_t>(k)].position - u);
        if (dk <= bestD) {
          bestD = dk;
          best = k;
        }
      }
      if (best >= 0) selected_ = best;
      else virtual_ = true;
    }
    // Its cycle and spectrum (recomputed only when it changed: a data edit, another keyframe, another position).
    uint64_t key = mix(0x84222325CBF29CE4ull, reinterpret_cast<uintptr_t>(model_.dataPtr().get()));
    key = mix(key, static_cast<uint64_t>(selected_) + (virtual_ ? 1ull << 40 : 0ull));
    if (virtual_) key = mix(key, std::bit_cast<uint32_t>(static_cast<float>(param(meta::kWtPosition))));
    key |= 1u;
    if (key != targetKey_) {
      targetKey_ = key;
      if (virtual_) targetCycle_ = model_.sourceCycle(std::clamp(static_cast<float>(param(meta::kWtPosition)), 0.0f, 1.0f));
      else targetCycle_ = d.keyframes[static_cast<size_t>(selected_)].cycle;
      targetCycle_.resize(wt::kCycleLen, 0.0f);
      wt::cycleSpectrum(targetCycle_, targetSpec_);
    }
  }
  static uint64_t mix(uint64_t h, uint64_t v) noexcept { return (h ^ v) * 0x100000001B3ull ^ (v >> 29); }

  // ---- the harmonic view ----
  float barPitch() const noexcept { return (kHarmX1 - kHarmX0) / static_cast<float>(hCount_); }
  float barX(uint32_t h) const noexcept { return kHarmX0 + barPitch() * static_cast<float>(h - hFirst_); }
  uint32_t barAt(float x) const noexcept {
    const float f = std::floor((x - kHarmX0) / barPitch());
    const auto i = static_cast<int64_t>(std::clamp(f, 0.0f, static_cast<float>(hCount_ - 1)));
    return std::min<uint32_t>(hFirst_ + static_cast<uint32_t>(i), wt::kHarmonicCount - 1);
  }
  void setView(int64_t first, int64_t count) const noexcept {
    hCount_ = static_cast<uint32_t>(std::clamp<int64_t>(count, kMinBars, wt::kHarmonicCount));
    hFirst_ = static_cast<uint32_t>(std::clamp<int64_t>(first, 0, wt::kHarmonicCount - hCount_));
  }
  // A row value at local y: amplitude 0 … 1 (bar height), or phase −π … π.
  static float ampAt(float y) noexcept { return std::clamp((kAmpY1 - 0.5f - y) / (kAmpY1 - kAmpY0 - 1.0f), 0.0f, 1.0f); }
  static float phaseAt(float y) noexcept {
    const float mid = (kPhY0 + kPhY1) * 0.5f, half = (kPhY1 - kPhY0) * 0.5f - 0.5f;
    return std::clamp((mid - y) / half, -1.0f, 1.0f) * kPi;
  }
  // The displayed bar height (0 … 1) of an amplitude: linear, or with Alt in dB (−60 … 0).
  float barHeight(float amp) const noexcept {
    if (!alt_) return std::min(amp, 1.0f);
    if (amp <= 1e-3f) return 0.0f;
    return std::clamp((20.0f * std::log10(amp) + 60.0f) / 60.0f, 0.0f, 1.0f);
  }

  // ---- draw mode ----
  static Vec2 drawPoint(float x, float v) noexcept { return {kDrawX0 + kDrawW * x, kDrawCy - kDrawA * v}; }
  static void drawCoords(Vec2 l, float& x, float& v) noexcept {
    x = (l.x - kDrawX0) / kDrawW;
    v = (kDrawCy - l.y) / kDrawA;
  }
  float travel(int32_t part) const {
    const meta::ParamSpec& s = type().params[static_cast<size_t>(part)];
    const double v = param(static_cast<uint32_t>(part));
    if (s.wraps) return static_cast<float>(v - std::floor(v));
    return static_cast<float>(meta::valueToTravel(s, v));
  }
  bool driven(const FaceEnv& env, std::string_view port) const {
    for (const FaceJack& j : env.jacks)
      if (j.input && j.connected && j.name == port) return true;
    return false;
  }
  // The position the highlighted frame shows: the param, or with the pos input wired the first voice's position.
  float shownPosition(const FaceEnv& env) const {
    if (voiceCount_ > 0 && driven(env, "pos")) return voiceBuf_[0].position;
    return static_cast<float>(param(meta::kWtPosition));
  }

  FrameGeom geom(float u) const noexcept {
    if (model_.frames() <= 1) {
      return {kDisp.x0 + 4.0f, kDisp.x1 - kDisp.x0 - 8.0f, (kDisp.y0 + kDisp.y1) * 0.5f, 40.0f};
    }
    constexpr float ox = 26.0f, oy = 30.0f, a = 29.0f;
    const float s = std::clamp(u, 0.0f, 1.0f);
    return {kDisp.x0 + 2.0f + ox * s, kDisp.x1 - kDisp.x0 - ox - 4.0f, kDisp.y1 - 3.0f - a - oy * s, a};
  }
  // Fills curve_ with frame values (kPoints + 1, every `step`-th) placed at depth u.
  // The point step for a frame drawn at `pxPerUnit` (`minStep` at full size): about one point per 1.5 device px, so a
  // far-zoomed face draws its curves with a few points (the same primitives, far fewer vertices).
  static uint32_t stepFor(float pxPerUnit, uint32_t minStep) noexcept {
    uint32_t step = minStep;
    const float width = (kDisp.x1 - kDisp.x0) * pxPerUnit;
    while (step < 64 && width * static_cast<float>(step) < 1.5f * wt::kPoints) step *= 2;
    return step;
  }
  void place(std::span<const float> v, float u, uint32_t step) const {
    curve_.clear();
    const FrameGeom g = geom(u);
    const float k = g.a * model_.gain();
    for (uint32_t i = 0; i <= wt::kPoints; i += step)
      curve_.push_back({g.x0 + g.w * static_cast<float>(i) / wt::kPoints, g.cy - k * v[i]});
  }
  Vec2 pointAt(float u, float phase) const {
    const FrameGeom g = geom(u);
    const float ph = phase - std::floor(phase);
    return {g.x0 + g.w * ph, g.cy - g.a * model_.gain() * model_.valueAt(u, ph)};
  }

  bool gapShown(size_t g) const noexcept {
    const auto& k = model_.data().keyframes;
    return g + 1 < k.size() && posY(k[g].position) - posY(k[g + 1].position) >= 9.0f;
  }
  float gapY(size_t g) const noexcept {
    const auto& k = model_.data().keyframes;
    return 0.5f * (posY(k[g].position) + posY(k[g + 1].position));
  }

  // ---- painting ----
  void outline(ui::FacePainter& p, float inset, float th, Rgba col) const {
    // A rounded rectangle's outline: four edges and four corner arcs (the same primitives whatever the colour).
    const float x0 = inset, y0 = inset, x1 = kW - inset, y1 = kH - inset, r = kRadius - inset;
    const Vec2 e[4][2] = {{{x0 + r, y0}, {x1 - r, y0}}, {{x1, y0 + r}, {x1, y1 - r}}, {{x1 - r, y1}, {x0 + r, y1}}, {{x0, y1 - r}, {x0, y0 + r}}};
    for (const auto& s : e) p.line(s[0], s[1], th, col);
    p.arc({x1 - r, y0 + r}, r, deg(-90.0f), deg(0.0f), th, col);
    p.arc({x1 - r, y1 - r}, r, deg(0.0f), deg(90.0f), th, col);
    p.arc({x0 + r, y1 - r}, r, deg(90.0f), deg(180.0f), th, col);
    p.arc({x0 + r, y0 + r}, r, deg(180.0f), deg(270.0f), th, col);
  }

  void paintTracks(ui::FacePainter& p, Rgba w) const {
    // Cells: the mode switch, the shapes, the warp modes.
    for (uint32_t i = 0; i < 2; ++i) p.roundedRect(modeCell(i), 3.0f, ui::mulAlpha(w, 0.8f));
    for (uint32_t i = 0; i < kShapeCount; ++i) p.roundedRect(shapeCell(i), 3.0f, ui::mulAlpha(w, 0.8f));
    for (uint32_t i = 0; i < 3; ++i) p.roundedRect(warpCell(i), 3.0f, ui::mulAlpha(w, 0.8f));
    for (uint32_t i = 0; i < kToolCount; ++i) p.roundedRect(toolCell(i), 3.0f, ui::mulAlpha(w, 0.8f));
    // (The harmonic rows' slots follow the harmonic view: paintSlots.) The display's well, the sliders' tracks, the knobs' arcs.
    p.roundedRect({kDisp.x0 - 2.0f, kDisp.y0 - 2.0f, kDisp.x1 + 2.0f, kDisp.y1 + 2.0f}, 5.0f, ui::mulAlpha(w, 0.28f));
    p.roundedRect({kPosX - kPosHalfW, kDisp.y0, kPosX + kPosHalfW, kDisp.y1}, kPosHalfW, w);
    p.roundedRect({kDisp.x0, kPhaseY - kPhaseHalfH, kDisp.x1, kPhaseY + kPhaseHalfH}, kPhaseHalfH, w);
    for (const float x : kKnobX) p.arc({x, kKnobY}, kKnobR, knobAngle(0.0f), knobAngle(1.0f), kKnobTrackW, w, ui::LineCap::Round);
  }

  void paintFrame(ui::FacePainter& p, Rgba w) const {
    outline(p, 0.75f, 1.25f, w);
    // Glyphs: the modes, the shapes, the warp modes, fold and sync.
    static constexpr Glyph kShapes[kShapeCount] = {Glyph::Sine, Glyph::Triangle, Glyph::Saw, Glyph::Square, Glyph::Pulse, Glyph::Noise};
    glyphs_.draw(p, Glyph::PhaseMode, centreOf(modeCell(0)), 4.0f, 3.5f, 1.0f, w);
    glyphs_.draw(p, Glyph::ShapeMode, centreOf(modeCell(1)), 4.0f, 3.5f, 1.0f, w);
    for (uint32_t i = 0; i < kShapeCount; ++i) glyphs_.draw(p, kShapes[i], centreOf(shapeCell(i)), 3.9f, 3.5f, 1.0f, w);
    static constexpr Glyph kTools[kToolCount] = {Glyph::Pencil, Glyph::Line, Glyph::Curve};
    for (uint32_t i = 0; i < kToolCount; ++i) glyphs_.draw(p, kTools[i], centreOf(toolCell(i)), 4.0f, 3.6f, 1.0f, w);
    static constexpr Glyph kWarps[3] = {Glyph::Bend, Glyph::Mirror, Glyph::PulseWidth};
    for (uint32_t i = 0; i < 3; ++i) glyphs_.draw(p, kWarps[i], centreOf(warpCell(i)), 3.8f, 3.2f, 1.0f, w);
    glyphs_.draw(p, Glyph::Fold, {kKnobX[0], kGlyphY}, 5.0f, 3.2f, 1.0f, w);
    glyphs_.draw(p, Glyph::Sync, {kKnobX[2], kGlyphY}, 5.0f, 3.2f, 1.0f, w);
    // Ticks: the phase slider's quarters, the position slider's quarters, the knobs' centre (warp: 0).
    for (int i = 0; i <= 4; ++i) {
      const float x = phaseX(0.25f * static_cast<float>(i));
      p.line({x, kPhaseY + kPhaseHalfH + 1.5f}, {x, kPhaseY + kPhaseHalfH + (i % 2 ? 3.0f : 4.0f)}, 0.9f, ui::mulAlpha(w, 0.8f));
      const float y = posY(0.25f * static_cast<float>(i));
      p.line({kPosX + kPosHalfW + 1.5f, y}, {kPosX + kPosHalfW + (i % 2 ? 3.0f : 4.0f), y}, 0.9f, ui::mulAlpha(w, 0.8f));
    }
    const Vec2 top = polarAt({kKnobX[1], kKnobY}, kKnobR + kKnobTrackW * 0.5f + 1.0f, deg(270.0f));
    p.line(top, {top.x, top.y - 2.0f}, 1.0f, w);
    // The phase row's centre line (phase 0).
    p.line({kHarmX0, (kPhY0 + kPhY1) * 0.5f}, {kHarmX1, (kPhY0 + kPhY1) * 0.5f}, 0.5f, ui::mulAlpha(w, 0.5f));
  }

  // The dense frames, back to front, every frame up to 64 lines (more: every k-th, the last always), fainter behind.
  void paintStack(ui::FacePainter& p) const {
    refresh();
    const uint32_t f = model_.frames();
    const uint32_t step = std::max(1u, (f + 63) / 64);
    std::array<uint32_t, 72> idx{};
    uint32_t n = 0;
    for (uint32_t d = 0; d < f && n < idx.size(); d += step) idx[n++] = d;
    if (idx[n - 1] != f - 1 && n < idx.size()) idx[n++] = f - 1;
    for (uint32_t i = n; i-- > 0;) {
      const uint32_t d = idx[i];
      const float u = f > 1 ? static_cast<float>(d) / static_cast<float>(f - 1) : 0.0f;
      place(model_.frame(d), u, stepFor(p.transform().scale, f > 1 ? 2u : 1u));
      const float alpha = f > 1 ? 0.95f - 0.6f * u : 0.55f;
      p.polyline(curve_, f > 1 ? 0.75f : 1.0f, ui::mulAlpha(ui::rgba(255, 255, 255), alpha));
    }
  }

  void paintCells(ui::FacePainter& p, const ui::Theme& th) {
    const Rgba lit = ui::mulAlpha(th.face.value, 0.22f);
    auto cell = [&](const Rect& r, bool on, int32_t part, Glyph g, float hw, float hh) {
      const bool hov = hovered_ == part;
      p.roundedRect(r, 3.0f, on ? lit : (hov ? th.face.hover : 0));
      glyphs_.draw(p, g, centreOf(r), hw, hh, 1.1f, on ? th.face.value : (hov ? th.face.handle : 0));
    };
    const bool shape = target().setting(meta::kWtMode) >= 0.5;
    cell(modeCell(0), !shape, kModePhase, Glyph::PhaseMode, 4.0f, 3.5f);
    cell(modeCell(1), shape, kModeShape, Glyph::ShapeMode, 4.0f, 3.5f);
    static constexpr Glyph kShapes[kShapeCount] = {Glyph::Sine, Glyph::Triangle, Glyph::Saw, Glyph::Square, Glyph::Pulse, Glyph::Noise};
    const int32_t sh = virtual_ ? -1 : model_.shapeOf(static_cast<uint32_t>(selected_));
    for (uint32_t i = 0; i < kShapeCount; ++i)
      cell(shapeCell(i), sh == static_cast<int32_t>(i), kShape0 + static_cast<int32_t>(i), kShapes[i], 3.9f, 3.5f);
    static constexpr Glyph kTools[kToolCount] = {Glyph::Pencil, Glyph::Line, Glyph::Curve};
    for (uint32_t i = 0; i < kToolCount; ++i)
      cell(toolCell(i), static_cast<uint32_t>(tool_) == i + 1, kTool0 + static_cast<int32_t>(i), kTools[i], 4.0f, 3.6f);
    static constexpr Glyph kWarps[3] = {Glyph::Bend, Glyph::Mirror, Glyph::PulseWidth};
    const auto wm = static_cast<int32_t>(std::round(target().setting(meta::kWtWarpMode)));
    for (uint32_t i = 0; i < 3; ++i)
      cell(warpCell(i), wm == static_cast<int32_t>(i), kWarpMode0 + static_cast<int32_t>(i), kWarps[i], 3.8f, 3.2f);
  }

  // The harmonic view's columns at this scale: `group` bars per drawn column (bars thinner than ≈ 1.6 device px are
  // merged into columns of the loudest bar they cover), `inset` the gap inside a slot.
  struct Columns {
    uint32_t group = 1;
    float inset = 0.0f;
  };
  Columns columns(float scale) const noexcept {
    Columns c;
    const float devPitch = barPitch() * scale;
    c.group = devPitch >= 1.6f ? 1u : static_cast<uint32_t>(std::ceil(1.6f / std::max(devPitch, 1e-4f)));
    c.inset = c.group > 1 ? 0.0f : std::clamp(barPitch() * 0.16f, 0.0f, 0.9f);
    return c;
  }

  // The edited keyframe's harmonic bars in white for a static layer: the amplitude row (`amplitude`: a bar from the
  // bottom; a bar over full scale gets a cap) or the phase row (a faint bar from the centre line and a bright tick at
  // the value). DC (harmonic 0) is drawn as an outline: it is an offset, not a partial.
  void paintHarmonics(ui::FacePainter& p, bool amplitude) const {
    refresh();
    const Rgba w = ui::rgba(255, 255, 255);
    const Columns col = columns(p.transform().scale);
    const float pitch = barPitch();
    const float mid = (kPhY0 + kPhY1) * 0.5f, half = (kPhY1 - kPhY0) * 0.5f - 0.5f;
    const float rowH = kAmpY1 - kAmpY0 - 1.0f;
    const uint32_t end = hFirst_ + hCount_;
    for (uint32_t h0 = hFirst_; h0 < end; h0 += col.group) {
      const uint32_t h1 = std::min(end, h0 + col.group);
      // The loudest bar of the column (its phase is the column's).
      uint32_t loud = h0;
      float a = 0.0f;
      for (uint32_t h = h0; h < h1; ++h) {
        const float ah = wt::harmonicAmp(targetSpec_, h);
        if (ah > a) {
          a = ah;
          loud = h;
        }
      }
      if (a <= 1e-4f) continue;  // absent (−80 dB): no bar
      const float x0 = barX(h0) + col.inset, x1 = barX(h1) - col.inset;
      const bool dc = loud == 0;
      if (amplitude) {
        // A bar never vanishes below a hairline while the harmonic is present (−60 dB still reads as "there").
        const float hgt = std::max(std::min(1.0f, pitch), barHeight(a) * rowH);
        const Rect r{x0, kAmpY1 - 0.5f - hgt, x1, kAmpY1 - 0.5f};
        if (dc) {
          outlineRect(p, r, 0.6f, ui::mulAlpha(w, 0.8f));
        } else {
          p.roundedRect(r, std::min(0.8f, (x1 - x0) * 0.3f), ui::mulAlpha(w, 0.95f));
          if (a > 1.0f) p.roundedRect({x0, r.y0 - 0.2f, x1, r.y0 + 1.0f}, 0.3f, w);  // over full scale
        }
      } else {
        // A quiet harmonic's phase matters less: its tick is fainter (full from amplitude 0.25 up). Dense columns
        // show ticks only.
        const float k = 0.3f + 0.7f * std::sqrt(std::min(1.0f, a / 0.25f));
        const float yv = mid - wt::harmonicPhase(targetSpec_, loud) / kPi * half;
        if (!dc && col.group == 1) p.roundedRect({x0, std::min(mid, yv), x1, std::max(mid, yv)}, 0.3f, ui::mulAlpha(w, 0.2f * k));
        p.roundedRect({x0, yv - 0.65f, x1, yv + 0.65f}, 0.3f, ui::mulAlpha(w, (dc ? 0.55f : 0.95f) * k));
      }
    }
  }
  static void outlineRect(ui::FacePainter& p, const Rect& r, float th, Rgba col) {
    p.line({r.x0, r.y0}, {r.x1, r.y0}, th, col);
    p.line({r.x1, r.y0}, {r.x1, r.y1}, th, col);
    p.line({r.x1, r.y1}, {r.x0, r.y1}, th, col);
    p.line({r.x0, r.y1}, {r.x0, r.y0}, th, col);
  }

  // The harmonic rows' slots (one per bar while bars are wide enough on screen, else one well per row; DC's slot is
  // narrower) and the scrollbar's track, in white for a static layer.
  void paintSlots(ui::FacePainter& p) const {
    const Rgba w = ui::rgba(255, 255, 255);
    const Columns col = columns(p.transform().scale);
    if (col.group == 1 && barPitch() * p.transform().scale >= 3.0f) {
      for (uint32_t h = hFirst_; h < hFirst_ + hCount_; ++h) {
        float x0 = barX(h) + col.inset * 0.6f, x1 = barX(h + 1) - col.inset * 0.6f;
        if (h == 0) x1 = x0 + (x1 - x0) * 0.6f;
        const float r = std::min(1.0f, (x1 - x0) * 0.3f);
        p.roundedRect({x0, kAmpY0, x1, kAmpY1}, r, ui::mulAlpha(w, 0.45f));
        p.roundedRect({x0, kPhY0, x1, kPhY1}, r, ui::mulAlpha(w, 0.45f));
      }
    } else {
      p.roundedRect({kHarmX0, kAmpY0, kHarmX1, kAmpY1}, 1.0f, ui::mulAlpha(w, 0.45f));
      p.roundedRect({kHarmX0, kPhY0, kHarmX1, kPhY1}, 1.0f, ui::mulAlpha(w, 0.45f));
    }
    p.roundedRect({kHarmX0, kScrollY - kScrollHalfH, kHarmX1, kScrollY + kScrollHalfH}, kScrollHalfH, ui::mulAlpha(w, 0.7f));
  }

  // Draw mode's grid in white for a static layer: the centre line, ±½ and ±1, the snap columns (every 4th brighter;
  // without a grid, quarters).
  void paintGrid(ui::FacePainter& p) const {
    const Rgba w = ui::rgba(255, 255, 255);
    const float x0 = kDrawX0, x1 = kDrawX0 + kDrawW;
    for (const float v : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
      const float a = v == 0.0f ? 0.55f : (std::fabs(v) == 1.0f ? 0.35f : 0.18f);
      p.line({x0, kDrawCy - kDrawA * v}, {x1, kDrawCy - kDrawA * v}, v == 0.0f ? 0.8f : 0.6f, ui::mulAlpha(w, a));
    }
    if (grid_ > 0) {  // the value steps, faint
      const float steps = static_cast<float>(grid_) * 0.5f;
      for (uint32_t k = 1; k < grid_; ++k) {
        const float v = -1.0f + static_cast<float>(k) / steps;
        if (std::fabs(v) < 1e-4f || std::fabs(std::fabs(v) - 0.5f) < 1e-4f) continue;
        p.line({x0, kDrawCy - kDrawA * v}, {x1, kDrawCy - kDrawA * v}, 0.4f, ui::mulAlpha(w, 0.1f));
      }
    }
    const uint32_t cols = grid_ > 0 ? grid_ : 4u;
    for (uint32_t c = 0; c <= cols; ++c) {
      const float x = x0 + kDrawW * static_cast<float>(c) / static_cast<float>(cols);
      const bool major = c % 4 == 0 || c == cols;
      p.line({x, kDrawCy - kDrawA - 3.0f}, {x, kDrawCy + kDrawA + 3.0f}, major ? 0.6f : 0.45f,
             ui::mulAlpha(w, grid_ > 0 ? (major ? 0.35f : 0.18f) : 0.15f));
    }
  }

  // The live part of the harmonic rows: the hovered column (both rows), the scrollbar's thumb.
  void paintHarmonicLive(ui::FacePainter& p, const ui::Theme& th) {
    const bool overRows = hovered_ == kHarmAmp || hovered_ == kHarmPhase || drag_ == kHarmAmp || drag_ == kHarmPhase;
    const uint32_t h = std::clamp(hoverBar_, hFirst_, hFirst_ + hCount_ - 1);
    const float x0 = barX(h), x1 = barX(h + 1);
    p.roundedRect({x0, kAmpY0, x1, kAmpY1}, std::min(1.0f, (x1 - x0) * 0.3f), overRows ? th.face.hover : 0);
    p.roundedRect({x0, kPhY0, x1, kPhY1}, std::min(1.0f, (x1 - x0) * 0.3f), overRows ? th.face.hover : 0);
    const float t0 = static_cast<float>(hFirst_) / wt::kHarmonicCount, t1 = static_cast<float>(hFirst_ + hCount_) / wt::kHarmonicCount;
    const float tx0 = kHarmX0 + (kHarmX1 - kHarmX0) * t0, tx1 = std::max(tx0 + 3.0f, kHarmX0 + (kHarmX1 - kHarmX0) * t1);
    const bool hot = hovered_ == kHarmScroll || drag_ == kHarmScroll;
    p.roundedRect({tx0, kScrollY - kScrollHalfH - (hot ? 0.4f : 0.0f), tx1, kScrollY + kScrollHalfH + (hot ? 0.4f : 0.0f)},
                  kScrollHalfH + (hot ? 0.4f : 0.0f), hot ? th.face.handle : ui::mulAlpha(th.face.tick, 0.9f));
  }

  // Draw mode's live layer: the edited cycle (dashed and faint while it is still the blended frame between
  // keyframes), the option cells over the display's corner, and where the pointer would draw.
  void paintDrawMode(ui::FacePainter& p, const ui::Theme& th) {
    curve_.clear();
    // About one point per device px (a drawn cycle has detail the frames' 256 points would alias), 64 … 1024.
    uint32_t n = 64;
    while (n < 1024 && static_cast<float>(n) < kDrawW * p.transform().scale) n *= 2;
    for (uint32_t i = 0; i <= n; ++i) {
      const float x = static_cast<float>(i) / static_cast<float>(n);
      const float v = std::clamp(targetCycle_[(static_cast<size_t>(i) * wt::kCycleLen / n) % wt::kCycleLen], -1.15f, 1.15f);
      curve_.push_back(drawPoint(x, v));
    }
    const float k = virtual_ ? 0.5f : 1.0f;
    p.fillUnder(curve_, kDrawCy, ui::mulAlpha(th.face.value, 0.24f * k), ui::mulAlpha(th.face.value, 0.03f * k));
    if (virtual_) {
      p.polyline(curve_, 1.5f, 0);
      p.dashed(curve_, 1.3f, 3.0f, 2.0f, ui::mulAlpha(th.face.value, 0.8f));
    } else {
      p.polyline(curve_, 1.5f, th.face.value);
      p.dashed(curve_, 1.3f, 3.0f, 2.0f, 0);
    }
    // The options: snap grid (off / 8 / 16 / 32), normalise, remove DC (table flags), on a body-coloured backing.
    const meta::WavetableData& d = model_.data();
    const bool on[3] = {grid_ > 0, d.normalise, d.removeDc};
    for (uint32_t i = 0; i < 3; ++i) {
      const Rect r = optCell(i);
      const int32_t part = kGridPart + static_cast<int32_t>(i);
      const bool hov = hovered_ == part;
      p.roundedRect({r.x0 - 0.5f, r.y0 - 0.5f, r.x1 + 0.5f, r.y1 + 0.5f}, 3.0f, th.face.body);
      p.roundedRect(r, 2.5f, on[i] ? ui::mulAlpha(th.face.value, 0.22f) : (hov ? th.face.hover : th.face.track));
      const Rgba gc = on[i] ? th.face.value : (hov ? th.face.handle : th.face.frame);
      const Vec2 c = centreOf(r);
      if (i == 0) Glyphs::gridGlyph(p, c, 3.4f, 3.0f, 0.8f, grid_, gc);
      else glyphs_.draw(p, i == 1 ? Glyph::Normalise : Glyph::RemoveDc, c, 3.6f, 3.0f, 0.9f, gc);
    }
    // Where the pointer draws (snapped): a ring and faint cross hairs.
    const bool show = hovered_ == kDrawPart && drag_ == kNone;
    float x = 0.0f, v = 0.0f;
    drawCoords(hoverLocal_, x, v);
    wt::Stroke::snap(tool_, grid_, x, v);
    const Vec2 at = drawPoint(x, v);
    const Rgba hair = show ? ui::mulAlpha(th.face.handle, 0.3f) : 0;
    p.line({at.x, kDrawCy - kDrawA - 3.0f}, {at.x, kDrawCy + kDrawA + 3.0f}, 0.6f, hair);
    p.line({kDrawX0, at.y}, {kDrawX0 + kDrawW, at.y}, 0.6f, hair);
    p.ring(at, 2.2f, 0.9f, show ? th.face.handle : 0);
  }
  void paintSliders(ui::FacePainter& p, const FaceEnv& env) {
    const ui::Theme& th = *env.theme;
    const meta::WavetableData& d = model_.data();
    // Position: hover, the value from the bottom, the handle; the keyframe markers; the gap glyphs.
    const bool posDriven = driven(env, "position");
    const float pos = travel(kPositionPart);
    p.roundedRect({kPosX - kPosHalfW, kDisp.y0, kPosX + kPosHalfW, kDisp.y1}, kPosHalfW, hovered_ == kPositionPart ? th.face.hover : 0);
    p.roundedRect({kPosX - kPosHalfW + 0.75f, posY(pos), kPosX + kPosHalfW - 0.75f, kDisp.y1}, 1.0f,
                  posDriven ? th.face.valueDriven : th.face.value);
    p.line({kPosX - 5.0f, posY(pos)}, {kPosX + 5.0f, posY(pos)}, drag_ == kPositionPart ? 2.5f : 2.0f,
           posDriven ? th.face.valueDriven : th.face.handle);
    // Between keyframes (the edited keyframe follows the position): a hollow marker where the first edit makes one.
    {
      const float y = posY(std::clamp(static_cast<float>(param(meta::kWtPosition)), 0.0f, 1.0f));
      const Rgba gc = virtual_ ? th.face.handle : 0;
      const Vec2 tip{kMarkTipX, y}, a{kMarkBaseX - 1.0f, y - kMarkHalfH - 0.6f}, b{kMarkBaseX - 1.0f, y + kMarkHalfH + 0.6f};
      p.line(tip, a, 0.8f, gc);
      p.line(a, b, 0.8f, gc);
      p.line(b, tip, 0.8f, gc);
    }
    for (size_t k = 0; k < d.keyframes.size(); ++k) {
      const float y = posY(d.keyframes[k].position);
      const bool sel = static_cast<int32_t>(k) == selected_ && !virtual_;
      const bool hov = hovered_ == kKeyBase + static_cast<int32_t>(k);
      const float hh = sel ? kMarkHalfH + 0.6f : kMarkHalfH;
      p.triangle({kMarkTipX, y}, {kMarkBaseX - (sel ? 1.0f : 0.0f), y - hh}, {kMarkBaseX - (sel ? 1.0f : 0.0f), y + hh},
                 sel ? th.face.handle : (hov ? th.face.value : th.face.tick));
    }
    for (size_t g = 0; g + 1 < d.keyframes.size(); ++g) {
      if (!gapShown(g)) continue;
      const float y = gapY(g);
      const bool hov = hovered_ == kGapBase + static_cast<int32_t>(g);
      const Rgba col = hov ? th.face.handle : ui::mulAlpha(th.face.tick, 0.9f);
      const bool spectral = g >= d.gaps.size() || d.gaps[g] == dsp::GapMode::Spectral;
      if (spectral) {
        glyphs_.draw(p, Glyph::Sine, {kGapX, y}, 3.6f, 2.0f, 0.9f, col);
      } else {
        p.line({kGapX - 2.6f, y - 2.6f}, {kGapX + 2.6f, y + 2.6f}, 0.9f, col);
        p.line({kGapX - 2.6f, y + 2.6f}, {kGapX + 2.6f, y - 2.6f}, 0.9f, col);
      }
    }
    // Phase: hover, the value from the left, the handle; per-voice ticks when the phase input is wired.
    const bool phDriven = driven(env, "phase");
    const float ph = travel(kPhasePart);
    p.roundedRect({kDisp.x0, kPhaseY - kPhaseHalfH, kDisp.x1, kPhaseY + kPhaseHalfH}, kPhaseHalfH,
                  hovered_ == kPhasePart ? th.face.hover : 0);
    p.roundedRect({kDisp.x0, kPhaseY - kPhaseHalfH + 0.75f, phaseX(ph), kPhaseY + kPhaseHalfH - 0.75f}, 1.0f,
                  phDriven ? th.face.valueDriven : th.face.value);
    p.line({phaseX(ph), kPhaseY - 5.0f}, {phaseX(ph), kPhaseY + 5.0f}, drag_ == kPhasePart ? 2.5f : 2.0f,
           phDriven ? th.face.valueDriven : th.face.handle);
  }

  void paintKnobs(ui::FacePainter& p, const FaceEnv& env) {
    const ui::Theme& th = *env.theme;
    static constexpr std::string_view kNames[3] = {"fold", "warp", "sync"};
    for (int i = 0; i < 3; ++i) {
      const int32_t part = kFoldPart + i;
      const Vec2 c{kKnobX[i], kKnobY};
      const float t = travel(part);
      const bool drv = driven(env, kNames[i]);
      const Rgba col = drv ? th.face.valueDriven : th.face.value;
      p.arc(c, kKnobR, knobAngle(0.0f), knobAngle(1.0f), kKnobTrackW, hovered_ == part ? th.face.hover : 0, ui::LineCap::Round);
      const float from = part == kWarpPart ? 0.5f : 0.0f;  // warp is bipolar: from its centre
      p.arc(c, kKnobR, knobAngle(std::min(from, t)), knobAngle(std::max(from, t)), kKnobTrackW - 1.0f, col);
      const float a = knobAngle(t);
      p.line(polarAt(c, kKnobR * 0.3f, a), polarAt(c, kKnobR + kKnobTrackW * 0.5f + 1.0f, a), drag_ == part ? 2.5f : 2.0f,
             drv ? th.face.valueDriven : th.face.handle);
    }
  }

  void paintOutputLead(ui::FacePainter& p, const FaceEnv& env) {
    // The output: a faint lead from the position slider to the output jack when the jack is level with the display.
    for (const FaceJack& j : env.jacks) {
      if (j.input || !j.visible) continue;
      const bool level = j.at.y >= kDisp.y0 && j.at.y <= kDisp.y1;
      p.line({kPosX + 7.0f, j.at.y}, {kW - 8.0f, j.at.y}, 1.25f, level ? ui::mulAlpha(env.theme->face.signal, 0.5f) : 0);
    }
  }

  void paintDisplay(ui::FacePainter& p, const FaceEnv& env, double now) {
    const ui::Theme& th = *env.theme;
    // The highlighted frame: a fill to its centre line and the curve, in the accent.
    const float u = std::clamp(shownPosition(env), 0.0f, 1.0f);
    model_.blended(u, blend_);
    place(blend_, u, stepFor(p.transform().scale, 1));
    const FrameGeom g = geom(u);
    p.fillUnder(curve_, g.cy, ui::mulAlpha(th.face.value, 0.22f), ui::mulAlpha(th.face.value, 0.02f));
    p.polyline(curve_, 1.5f, th.face.value);
    // No voice: a dashed line and a dot at the phase slider's read phase (the sketch).
    const float base = travel(kPhasePart);
    const bool quiet = shown_.empty();
    const Vec2 bp = pointAt(u, base);
    const Vec2 bar[2] = {{bp.x, g.cy - g.a - 2.0f}, {bp.x, g.cy + g.a + 2.0f}};
    p.dashed(bar, 0.9f, 2.5f, 2.0f, quiet ? ui::mulAlpha(th.face.handle, 0.45f) : 0);
    p.disc(bp, 1.8f, quiet ? th.face.handle : 0);
    paintVoices(p, env, now);
  }

  void paintVoices(ui::FacePainter& p, const FaceEnv& env, double now) {
    if (shown_.empty()) return;
    const ui::Theme& th = *env.theme;
    const float hiU = std::clamp(shownPosition(env), 0.0f, 1.0f);
    const float frameStep = model_.frames() > 1 ? 0.5f / static_cast<float>(model_.frames() - 1) : 2.0f;
    const bool wired = driven(env, "x");
    // A voice whose frame is not the highlighted one (the pos input modulated): its own frame, faint.
    for (const Shown& s : shown_) {
      const float a = alphaOf(s, now);
      const float vu = std::clamp(s.v.position, 0.0f, 1.0f);
      if (std::fabs(vu - hiU) <= frameStep) continue;
      model_.blended(vu, blend_);
      place(blend_, vu, stepFor(p.transform().scale, 2));
      p.polyline(curve_, 1.0f, ui::mulAlpha(th.face.signal, 0.28f * a));
    }
    p.beginGlow();
    // The glow density: every phase sampled during the last displayed frame, faint (where the reads landed). Each
    // sprite is fainter the more there are (∝ 1/n over all voices: n reads spread over a cycle overlap about n·7/150
    // sprites deep), so a chord of fast voices reads as a soft glow along the curve instead of a saturated stroke, and
    // a slow voice's few reads still show.
    uint32_t total = 0;
    for (const Shown& s : shown_) total += s.dying ? 0u : s.v.sampleCount;
    const float density = std::clamp(2400.0f / static_cast<float>(std::max(total, 1u)), 3.0f, 40.0f) / 48.0f;
    for (const Shown& s : shown_) {
      if (s.dying) continue;
      const float a = alphaOf(s, now);
      const float vu = std::clamp(s.v.position, 0.0f, 1.0f);
      const Rgba gc = ui::withAlpha(th.face.glow, static_cast<uint32_t>(48.0f * density * a));
      for (uint32_t k = 0; k < s.v.sampleCount; ++k) p.glow(pointAt(vu, s.v.samples[k]), 3.5f, gc);
    }
    for (const Shown& s : shown_) {
      const float a = alphaOf(s, now);
      const Vec2 d = pointAt(std::clamp(s.v.position, 0.0f, 1.0f), s.v.phase);
      p.glow(d, 9.0f, ui::withAlpha(th.face.glow, static_cast<uint32_t>(150.0f * a)));
    }
    p.endGlow();
    for (const Shown& s : shown_) {
      const float a = alphaOf(s, now);
      const float vu = std::clamp(s.v.position, 0.0f, 1.0f);
      const FrameGeom g = geom(vu);
      const Vec2 d = pointAt(vu, s.v.phase);
      const Vec2 bar[2] = {{d.x, g.cy - g.a - 2.0f}, {d.x, g.cy + g.a + 2.0f}};
      p.dashed(bar, 0.8f, 1.2f, 1.6f, ui::mulAlpha(th.face.signal, 0.45f * a));
      p.disc(d, 2.4f, ui::mulAlpha(th.face.signal, a));
      p.disc(d, 1.1f, ui::mulAlpha(ui::rgba(255, 255, 255), a));
      // The phase slider's tick for this voice (its read phase) when the phase input is wired.
      const float x = phaseX(s.v.phase - std::floor(s.v.phase));
      p.line({x, kPhaseY - 4.0f}, {x, kPhaseY + 4.0f}, 1.2f, wired ? ui::mulAlpha(th.face.signal, 0.9f * a) : 0);
    }
  }

  // ---- voices shown (with their fades) ----
  struct Shown {
    FaceVoice v;
    double born = 0.0;  // when it appeared (alpha rises over kFade)
    double died = 0.0;  // when it stopped (alpha falls over kFade), when dying
    bool dying = false;
    bool seen = false;  // in the last read
  };
  static float alphaOf(const Shown& s, double now) noexcept {
    const float in = std::clamp(static_cast<float>((now - s.born) / kFade), 0.0f, 1.0f);
    const float out = s.dying ? std::clamp(1.0f - static_cast<float>((now - s.died) / kFade), 0.0f, 1.0f) : 1.0f;
    return std::min(in, out);
  }

  // ---- gestures ----
  bool down(const FacePointer& e);
  bool move(const FacePointer& e);
  bool hover(const FacePointer& e);
  bool wheel(const FacePointer& e);
  bool harmonicDown(const FacePointer& e, int32_t part);
  void harmonicMove(Vec2 l);
  bool drawDown(const FacePointer& e);
  void commitData(const char* label, const meta::WavetableData& d) {
    if (!target().editable()) return;
    target().begin(label);
    target().setData(meta::wavetableData(d));
    target().end();
  }
  // A keyframe at u from `cycle` into n (the gap it splits keeps its mode on both sides); returns its index.
  static size_t insertKeyframe(meta::WavetableData& n, float u, std::vector<float> cycle) {
    const size_t before = n.keyframes.size();
    size_t at = 0;
    while (at < n.keyframes.size() && n.keyframes[at].position <= u) ++at;
    const dsp::GapMode mode = at > 0 && at - 1 < n.gaps.size() ? n.gaps[at - 1] : dsp::GapMode::Spectral;
    n.keyframes.insert(n.keyframes.begin() + static_cast<std::ptrdiff_t>(at), {u, std::move(cycle)});
    if (!n.gaps.empty() || mode != dsp::GapMode::Spectral) {
      n.gaps.resize(before - 1, dsp::GapMode::Spectral);
      n.gaps.insert(n.gaps.begin() + static_cast<std::ptrdiff_t>(std::min(at, n.gaps.size())), mode);
      n.gaps.resize(n.keyframes.size() - 1, mode);
    }
    return at;
  }
  // Starts an edit of the edited keyframe's cycle (one undo step until Up): dragData_ holds the data, editK_ the
  // keyframe (made from the blended frame first when the position lies between keyframes), editBase_ its cycle.
  bool beginEdit(const char* label) {
    refresh();
    MetaTarget& tg = target();
    if (!model_.ok() || !tg.editable()) return false;
    dragData_ = model_.data();
    if (virtual_) {
      if (dragData_.keyframes.size() >= dsp::kMaxKeyframes) {
        tg.notice("A wavetable holds at most 256 keyframes");
        return false;
      }
      const float u = std::clamp(static_cast<float>(param(meta::kWtPosition)), 0.0f, 1.0f);
      editK_ = insertKeyframe(dragData_, u, targetCycle_);
    } else {
      editK_ = static_cast<size_t>(selected_);
    }
    selected_ = static_cast<int32_t>(editK_);
    editBase_ = dragData_.keyframes[editK_].cycle;
    editBase_.resize(wt::kCycleLen, 0.0f);
    dragData_.keyframes[editK_].cycle = editBase_;
    tg.begin(label);
    begun_ = true;
    return true;
  }
  void publishEdit() { target().setData(meta::wavetableData(dragData_)); }

  // State read from the target (mutable: refreshed from const paths such as the static layers and the tile key).
  mutable wt::Model model_;
  mutable int32_t selected_ = 0;
  mutable bool atPosition_ = false;  // the edited keyframe follows the position (set by a position drag)
  mutable bool virtual_ = false;     // ... and none sits there: the blended frame, until the first edit
  mutable uint64_t targetKey_ = 0;
  mutable std::vector<float> targetCycle_;
  mutable wt::Spectrum1 targetSpec_;
  mutable std::vector<Vec2> curve_;
  mutable Glyphs glyphs_;
  std::vector<float> blend_;
  std::vector<Shown> shown_;
  // the harmonic view and draw mode (view state, not document state)
  mutable uint32_t hFirst_ = 1, hCount_ = kDefaultBars;
  bool alt_ = false;          // Alt held over the face (the amplitude row in dB)
  uint32_t hoverBar_ = 1;     // the bar under the pointer
  Vec2 hoverLocal_{0.0f, 0.0f};
  wt::Tool tool_ = wt::Tool::None;
  uint32_t grid_ = 0;         // snap columns: 0 / 8 / 16 / 32
  // drags
  int32_t drag_ = kNone;
  bool begun_ = false;
  bool keyFocus_ = false;     // the last press was on the face's position slider, markers or draw controls: it takes keys
  bool markerFocus_ = false;  // ... on the position slider or a marker: Delete removes the selected keyframe
  float dragT_ = 0.0f;
  Vec2 last_{0.0f, 0.0f};
  meta::WavetableData dragData_;  // a marker drag (and every cycle edit) edits a copy of the data it began with
  // a cycle edit: the keyframe, its cycle when the edit began, its spectrum, the harmonics wanted and which are edited
  size_t editK_ = 0;
  std::vector<float> editBase_;
  wt::Spectrum1 editSpec_;
  std::vector<std::complex<double>> want_;
  std::vector<uint8_t> edited_;
  bool resetting_ = false;   // a right-button drag: bars go back to 0
  bool lineMode_ = false;    // Shift at the press: a straight line from the press
  uint32_t anchorBar_ = 0, lastBar_ = 0;
  float anchorVal_ = 0.0f, lastVal_ = 0.0f;
  wt::HarmonicWriter writer_;
  wt::Stroke stroke_;
  float scrollGrab_ = 0.0f;  // the scrollbar drag: where on the thumb it was taken (in harmonics)
};

bool WavetableFace::voicesPolled(bool changed) {
  const double now = clockNow();
  if (changed) {
    for (Shown& s : shown_) s.seen = false;
    for (uint32_t i = 0; i < voiceCount_; ++i) {
      const FaceVoice& v = voiceBuf_[i];
      Shown* found = nullptr;
      for (Shown& s : shown_)
        if (s.v.slot == v.slot) found = &s;
      if (found == nullptr) {
        shown_.push_back({});
        found = &shown_.back();
        found->born = justBound_ ? now - kFade : now;  // sounding when the face appeared: shown at once
      } else if (found->dying) {
        found->born = now - static_cast<double>(alphaOf(*found, now)) * kFade;  // back before it faded out
      }
      found->v = v;
      found->dying = false;
      found->seen = true;
    }
    for (Shown& s : shown_)
      if (!s.seen && !s.dying) {
        s.dying = true;
        s.died = now;
        s.v.sampleCount = 0;  // its dot fades where it was; its reads are over
      }
  }
  justBound_ = false;
  // Drop the voices that have faded out.
  const size_t before = shown_.size();
  std::erase_if(shown_, [&](const Shown& s) { return s.dying && now - s.died >= kFade; });
  bool fading = shown_.size() != before;
  for (const Shown& s : shown_) fading = fading || s.dying || now - s.born < kFade;
  if (shown_.size() > 2 * kMaxVoices) shown_.erase(shown_.begin(), shown_.end() - 2 * kMaxVoices);
  return changed || fading;
}

bool WavetableFace::down(const FacePointer& e) {
  refresh();
  const int32_t part = hitTest(e.local, e.pxPerUnit);
  const bool right = e.button == ui::PointerButton::Right;
  if (right && part != kHarmAmp && part != kHarmPhase) return false;  // the host's menu
  markerFocus_ = part == kPositionPart || (part >= kKeyBase && part < kGapBase);
  keyFocus_ = markerFocus_ || part == kDrawPart || (part >= kTool0 && part < kTool0 + static_cast<int32_t>(kToolCount)) ||
              (part >= kGridPart && part <= kDcPart);
  if (part < 0) return false;
  MetaTarget& tg = target();
  const meta::WavetableData& d = model_.data();
  if (part <= kSyncPart) {
    if (e.clicks >= 2) {
      if (part == kPositionPart) {
        // Double-click on the track: a keyframe there, from the frame the table plays there (before the bake).
        if (!model_.ok() || !tg.editable()) return true;
        if (d.keyframes.size() >= dsp::kMaxKeyframes) {
          tg.notice("A wavetable holds at most 256 keyframes");
          return true;
        }
        const float u = std::clamp((kDisp.y1 - e.local.y) / (kDisp.y1 - kDisp.y0), 0.0f, 1.0f);
        meta::WavetableData n = d;
        selected_ = static_cast<int32_t>(insertKeyframe(n, u, model_.sourceCycle(u)));
        atPosition_ = false;
        commitData("Add Keyframe", n);
        drag_ = kNone;
        return true;
      }
      if (tg.editable()) tg.reset(static_cast<size_t>(part));
      drag_ = kNone;
      return true;
    }
    if (part == kPositionPart) atPosition_ = true;  // edits follow the position from now on
    drag_ = part;
    dragT_ = travel(part);
    last_ = e.device;
    begun_ = false;
    return true;
  }
  if (part >= kKeyBase && part < kGapBase) {
    selected_ = part - kKeyBase;
    atPosition_ = false;
    if (!model_.ok()) return true;
    drag_ = part;
    dragData_ = d;
    dragT_ = d.keyframes[static_cast<size_t>(selected_)].position;
    last_ = e.device;
    begun_ = false;
    return true;
  }
  if (part >= kGapBase) {
    if (!model_.ok()) return true;
    meta::WavetableData n = d;
    n.gaps.resize(n.keyframes.size() - 1, dsp::GapMode::Spectral);
    auto& gm = n.gaps[static_cast<size_t>(part - kGapBase)];
    gm = gm == dsp::GapMode::Spectral ? dsp::GapMode::Crossfade : dsp::GapMode::Spectral;
    commitData(gm == dsp::GapMode::Crossfade ? "Crossfade Gap" : "Spectral Gap", n);
    return true;
  }
  if (part >= kShape0 && part < kShape0 + static_cast<int32_t>(kShapeCount)) {
    if (!beginEdit("Basic Shape")) return true;
    const AlignedBuffer<float> c = dsp::basicCycle(static_cast<dsp::BasicCycle>(part - kShape0));
    dragData_.keyframes[editK_].cycle.assign(c.span().begin(), c.span().end());
    publishEdit();
    tg.end();
    begun_ = false;
    return true;
  }
  if (part >= kWarpMode0 && part < kWarpMode0 + 3) {
    const double m = part - kWarpMode0;
    if (tg.editable() && tg.setting(meta::kWtWarpMode) != m) {
      tg.begin("Set Warp Mode");
      tg.setSetting(meta::kWtWarpMode, m);
      tg.end();
    }
    return true;
  }
  if (part == kModePhase) return true;
  if (part == kModeShape) {
    tg.notice("Shape mode is not available yet: the Wavetable reads its table by phase");
    return true;
  }
  if (part >= kTool0 && part < kTool0 + static_cast<int32_t>(kToolCount)) {
    // A tool: draw mode with it (the lit one again: back to the stacked frames). View state: no undo step.
    const auto t = static_cast<wt::Tool>(part - kTool0 + 1);
    tool_ = tool_ == t ? wt::Tool::None : t;
    return true;
  }
  if (part == kGridPart) {
    grid_ = grid_ == 0 ? 8u : (grid_ >= 32 ? 0u : grid_ * 2);
    return true;
  }
  if (part == kNormPart || part == kDcPart) {
    if (!model_.ok()) return true;
    meta::WavetableData n = d;
    bool& flag = part == kNormPart ? n.normalise : n.removeDc;
    flag = !flag;
    commitData(part == kNormPart ? (flag ? "Normalise" : "Don't Normalise") : (flag ? "Remove DC" : "Keep DC"), n);
    return true;
  }
  if (part == kHarmAmp || part == kHarmPhase) return harmonicDown(e, part);
  if (part == kHarmScroll) {
    const float span = kHarmX1 - kHarmX0;
    const float at = (e.local.x - kHarmX0) / span * wt::kHarmonicCount;  // in harmonics
    if (at < static_cast<float>(hFirst_) || at > static_cast<float>(hFirst_ + hCount_))  // off the thumb: centre it there
      setView(static_cast<int64_t>(std::lround(at - 0.5f * static_cast<float>(hCount_))), hCount_);
    scrollGrab_ = at - static_cast<float>(hFirst_);
    drag_ = kHarmScroll;
    begun_ = false;
    return true;
  }
  if (part == kDrawPart) return drawDown(e);
  return false;  // the frame: the host's (select, move, open)
}

bool WavetableFace::harmonicDown(const FacePointer& e, int32_t part) {
  const bool right = e.button == ui::PointerButton::Right;
  const bool amp = part == kHarmAmp;
  const bool reset = right || e.clicks >= 2;
  const char* label = reset ? "Reset Harmonics" : (amp ? "Edit Harmonics" : "Edit Phases");
  if (!beginEdit(label)) return true;
  wt::cycleSpectrum(editBase_, editSpec_);
  want_ = editSpec_.c;
  edited_.assign(wt::kHarmonicCount, 0);
  resetting_ = reset;
  lineMode_ = !reset && (e.mods & ui::mod::kShift) != 0;
  anchorBar_ = lastBar_ = barAt(e.local.x);
  anchorVal_ = lastVal_ = amp ? ampAt(e.local.y) : phaseAt(e.local.y);
  drag_ = part;
  harmonicMove(e.local);
  if (e.clicks >= 2) {  // a double-click resets one bar: done
    target().end();
    begun_ = false;
    drag_ = kNone;
  }
  return true;
}

// The harmonic edit at local point l: the bars from the last point (or, Shift, the press) to here get their values,
// and the keyframe's cycle is written from the cycle the edit began with.
void WavetableFace::harmonicMove(Vec2 l) {
  const bool amp = drag_ == kHarmAmp;
  const uint32_t bar = barAt(l.x);
  const float val = amp ? ampAt(l.y) : phaseAt(l.y);
  hoverBar_ = bar;
  auto setBar = [&](uint32_t h, float v) {
    const std::complex<double> base = editSpec_.c[h];
    if (resetting_) want_[h] = amp ? wt::withAmp(base, h, 0.0) : wt::withPhase(base, h, 0.0);
    else want_[h] = amp ? wt::withAmp(base, h, v) : wt::withPhase(base, h, v);
    edited_[h] = 1;
  };
  auto span = [&](uint32_t ha, float va, uint32_t hb, float vb) {
    const uint32_t lo = std::min(ha, hb), hi = std::max(ha, hb);
    for (uint32_t h = lo; h <= hi; ++h) {
      const float t = ha == hb ? 1.0f : static_cast<float>(static_cast<int64_t>(h) - ha) / static_cast<float>(static_cast<int64_t>(hb) - ha);
      setBar(h, va + t * (vb - va));
    }
  };
  if (lineMode_) {
    want_ = editSpec_.c;
    std::fill(edited_.begin(), edited_.end(), uint8_t{0});
    span(anchorBar_, anchorVal_, bar, val);
  } else {
    span(lastBar_, lastVal_, bar, val);
  }
  lastBar_ = bar;
  lastVal_ = val;
  writer_.write(editBase_, editSpec_, want_, edited_, dragData_.keyframes[editK_].cycle);
  publishEdit();
}

bool WavetableFace::drawDown(const FacePointer& e) {
  static constexpr const char* kLabels[] = {"Draw", "Draw", "Draw Line", "Draw Curve"};
  if (!beginEdit(kLabels[static_cast<size_t>(tool_)])) return true;
  float x = 0.0f, v = 0.0f;
  drawCoords(e.local, x, v);
  stroke_.begin(editBase_, tool_, grid_, x, v);
  const auto c = stroke_.cycle();
  dragData_.keyframes[editK_].cycle.assign(c.begin(), c.end());
  publishEdit();
  drag_ = kDrawPart;
  return true;
}

bool WavetableFace::hover(const FacePointer& e) {
  // The pointer over the face without a drag: Alt (the dB view), the bar under it, where a stroke would land.
  const bool alt = (e.mods & ui::mod::kAlt) != 0;
  const uint32_t bar = barAt(e.local.x);
  const bool overRows = hovered_ == kHarmAmp || hovered_ == kHarmPhase;
  bool changed = alt != alt_ || (overRows && bar != hoverBar_);
  if (tool_ != wt::Tool::None && (hovered_ == kDrawPart || e.local.x != hoverLocal_.x || e.local.y != hoverLocal_.y))
    changed = changed || hovered_ == kDrawPart;
  alt_ = alt;
  if (overRows) hoverBar_ = bar;
  hoverLocal_ = e.local;
  return changed;
}

bool WavetableFace::wheel(const FacePointer& e) {
  const bool over = e.local.x >= kHarmX0 - 1.0f && e.local.x <= kHarmX1 + 1.0f && e.local.y >= kAmpY0 - 1.5f &&
                    e.local.y <= kScrollY + 1.8f;
  if (!over) return false;  // the canvas pans or zooms
  const float notches = std::fabs(e.wheel.y) >= std::fabs(e.wheel.x) ? e.wheel.y : -e.wheel.x;
  if (notches == 0.0f) return true;
  if (e.mods & ui::mod::kCtrl) {
    // Zoom about the bar under the pointer: wheel up = fewer, wider bars (√2 per notch).
    const float u = std::clamp((e.local.x - kHarmX0) / (kHarmX1 - kHarmX0), 0.0f, 1.0f);
    const float pivot = static_cast<float>(hFirst_) + u * static_cast<float>(hCount_);
    // On the √2 ladder (8, 11, 16, 23, 32, 45, 64, … 724, 1024), so the powers of two come back exactly.
    double steps = std::round(static_cast<double>(notches));
    if (steps == 0.0) steps = notches > 0.0f ? 1.0 : -1.0;  // a fine (trackpad) notch still moves one rung
    const double level = std::round(2.0 * std::log2(static_cast<double>(hCount_))) - steps;
    const double count = std::round(std::pow(2.0, 0.5 * level));
    const auto n = std::clamp<int64_t>(static_cast<int64_t>(count), kMinBars, wt::kHarmonicCount);
    setView(static_cast<int64_t>(std::lround(pivot - u * static_cast<float>(n))), n);
  } else {
    // Scroll: an eighth of the view per notch (at least one bar); wheel up = towards the low harmonics.
    const int64_t step = std::max<int64_t>(1, hCount_ / 8);
    setView(static_cast<int64_t>(hFirst_) - static_cast<int64_t>(std::lround(notches * static_cast<float>(step))), hCount_);
  }
  hoverBar_ = barAt(e.local.x);
  return true;
}

bool WavetableFace::move(const FacePointer& e) {
  if (drag_ == kNone) return hover(e);
  MetaTarget& tg = target();
  if (drag_ == kHarmAmp || drag_ == kHarmPhase) {
    if (begun_) harmonicMove(e.local);
    return true;
  }
  if (drag_ == kDrawPart) {
    if (!begun_) return true;
    float x = 0.0f, v = 0.0f;
    drawCoords(e.local, x, v);
    stroke_.to(x, v);
    const auto c = stroke_.cycle();
    dragData_.keyframes[editK_].cycle.assign(c.begin(), c.end());
    publishEdit();
    hoverLocal_ = e.local;
    return true;
  }
  if (drag_ == kHarmScroll) {
    const float at = (e.local.x - kHarmX0) / (kHarmX1 - kHarmX0) * wt::kHarmonicCount;
    setView(static_cast<int64_t>(std::lround(at - scrollGrab_)), hCount_);
    return true;
  }
  const bool horizontal = drag_ == kPhasePart;
  const float dpx = horizontal ? e.device.x - last_.x : -(e.device.y - last_.y);
  last_ = e.device;
  if (dpx == 0.0f || !tg.editable()) return true;
  const float fine = (e.mods & ui::mod::kShift) ? 0.1f : 1.0f;
  // Travel per device px: the control's extent on screen, never less than 150 logical px for the whole range.
  const float extent = horizontal ? kDisp.x1 - kDisp.x0 : (drag_ >= kFoldPart && drag_ <= kSyncPart ? 60.0f : kDisp.y1 - kDisp.y0);
  const float span = std::max(extent * e.pxPerUnit, 150.0f * e.uiScale);
  dragT_ += dpx / span * fine;
  if (drag_ >= kKeyBase) {
    // A keyframe marker: its position between its neighbours (Ctrl: on the dense frames).
    const size_t k = static_cast<size_t>(drag_ - kKeyBase);
    auto& keys = dragData_.keyframes;
    const float lo = k > 0 ? keys[k - 1].position : 0.0f, hi = k + 1 < keys.size() ? keys[k + 1].position : 1.0f;
    dragT_ = std::clamp(dragT_, 0.0f, 1.0f);
    float u = dragT_;
    if (e.mods & ui::mod::kCtrl) {
      const float f = static_cast<float>(std::max<uint32_t>(dragData_.frames, 2) - 1);
      u = std::round(u * f) / f;
    }
    u = std::clamp(u, lo, hi);
    if (u == keys[k].position) return true;
    keys[k].position = u;
    if (!begun_) {
      tg.begin("Move Keyframe");
      begun_ = true;
    }
    tg.setData(meta::wavetableData(dragData_));
    return true;
  }
  const meta::ParamSpec& spec = type().params[static_cast<size_t>(drag_)];
  if (spec.wraps) dragT_ -= std::floor(dragT_);
  else dragT_ = std::clamp(dragT_, 0.0f, 1.0f);
  if (!begun_) {
    static constexpr const char* kLabels[] = {"Set Phase", "Set Position", "Set Fold", "Set Warp", "Set Sync"};
    tg.begin(kLabels[drag_]);
    begun_ = true;
  }
  tg.set(static_cast<size_t>(drag_), meta::travelToValue(spec, dragT_));
  return true;
}

void WavetableFace::describe(int32_t part, std::string& title, std::string& body) const {
  char buf[360];
  auto value = [&](uint32_t i) { return param(i); };
  if (part == kPhasePart) {
    title = "Phase";
    std::snprintf(buf, sizeof buf, "Read phase %.3f, added to the phase input. Drag left / right (Shift: fine); double-click resets.",
                  value(meta::kWtPhase));
  } else if (part == kPositionPart) {
    title = "Position";
    std::snprintf(buf, sizeof buf,
                  "%.3f between the first and the last keyframe. Drag up / down (Shift: fine); double-click adds a keyframe here.",
                  value(meta::kWtPosition));
  } else if (part == kFoldPart) {
    title = "Fold";
    std::snprintf(buf, sizeof buf, "%.3f: a sine wavefolder, baked into the table. Drag up / down; double-click resets.", value(meta::kWtFold));
  } else if (part == kWarpPart) {
    title = "Warp";
    std::snprintf(buf, sizeof buf, "%+.3f, baked into the table (the switch below picks bend / mirror / pulse width). Double-click resets.",
                  value(meta::kWtWarp));
  } else if (part == kSyncPart) {
    title = "Sync";
    std::snprintf(buf, sizeof buf, "%.3f: the cycle read %.2fx per period (hard sync), baked into the table. Double-click resets.",
                  value(meta::kWtSync), 1.0 + 15.0 * value(meta::kWtSync));
  } else if (part >= kWarpMode0 && part < kWarpMode0 + 3) {
    static constexpr const char* kNames[] = {"Bend", "Mirror", "Pulse width"};
    title = std::string("Warp mode: ") + kNames[part - kWarpMode0];
    std::snprintf(buf, sizeof buf, "How warp reshapes the read. Click to choose.");
  } else if (part == kModePhase) {
    title = "Phase mode";
    std::snprintf(buf, sizeof buf, "The input is a phase: the table is read through one cycle per period.");
  } else if (part == kModeShape) {
    title = "Shape mode";
    std::snprintf(buf, sizeof buf, "The input as a signal through the table (a waveshaper). Not available yet.");
  } else if (part >= kShape0 && part < kShape0 + static_cast<int32_t>(kShapeCount)) {
    static constexpr const char* kNames[] = {"Sine", "Triangle", "Saw", "Square", "Pulse 25 %", "Noise"};
    title = std::string("Basic shape: ") + kNames[part - kShape0];
    std::snprintf(buf, sizeof buf, "Replaces keyframe %d's cycle.", selected_ + 1);
  } else if (part == kHarmAmp || part == kHarmPhase || part == kHarmScroll) {
    const uint32_t h = hoverBar_;
    const float a = wt::harmonicAmp(targetSpec_, h);
    const float ph = wt::harmonicPhase(targetSpec_, h) / kPi;
    title = part == kHarmScroll ? std::string("Harmonic view") : (h == 0 ? std::string("DC") : "Harmonic " + std::to_string(h));
    char amp[48];
    if (a > 1e-6f) std::snprintf(amp, sizeof amp, "%.3f (%.1f dB)", a, 20.0 * std::log10(a));
    else std::snprintf(amp, sizeof amp, "0 (absent)");
    std::snprintf(buf, sizeof buf,
                  "%s %s, phase %+.2fpi. Drag paints (Shift: a line); right-click or double-click resets. Wheel scrolls, "
                  "Ctrl+wheel zooms; showing %u-%u of 0-1023.",
                  virtual_ ? "Blended frame:" : (std::string("Keyframe ") + std::to_string(selected_ + 1) + ":").c_str(), amp,
                  ph, hFirst_, hFirst_ + hCount_ - 1);
  } else if (part >= kTool0 && part < kTool0 + static_cast<int32_t>(kToolCount)) {
    static constexpr const char* kNames[] = {"Pencil", "Line", "Smooth curve"};
    title = std::string("Draw: ") + kNames[part - kTool0];
    std::snprintf(buf, sizeof buf, "Draws the edited keyframe's cycle in the display. Click again (or Esc) for the frames.");
  } else if (part == kDrawPart) {
    title = virtual_ ? "Draw (a new keyframe)" : "Draw keyframe " + std::to_string(selected_ + 1);
    std::snprintf(buf, sizeof buf, "Drag to draw; each stroke is one undo step.%s",
                  virtual_ ? " The first stroke makes a keyframe here from the blended frame." : "");
  } else if (part == kGridPart) {
    title = "Snap grid";
    if (grid_ == 0) std::snprintf(buf, sizeof buf, "Off. Click: 8, 16, 32 columns (values in steps of 2/n), off.");
    else std::snprintf(buf, sizeof buf, "%u columns, values in steps of 2/%u. Click for the next.", grid_, grid_);
  } else if (part == kNormPart) {
    title = "Normalise";
    std::snprintf(buf, sizeof buf, "%s: every keyframe scaled to a peak of 1 in the table (the drawing is kept).",
                  model_.data().normalise ? "On" : "Off");
  } else if (part == kDcPart) {
    title = "Remove DC";
    std::snprintf(buf, sizeof buf, "%s: the table's frames centred on 0 (the drawing is kept).", model_.data().removeDc ? "On" : "Off");
  } else if (part >= kKeyBase && part < kGapBase) {
    const size_t k = static_cast<size_t>(part - kKeyBase);
    title = "Keyframe " + std::to_string(k + 1);
    const float p = k < model_.data().keyframes.size() ? model_.data().keyframes[k].position : 0.0f;
    std::snprintf(buf, sizeof buf, "At %.3f. Click selects, drag moves (Ctrl: snap to frames), Delete removes.", p);
  } else if (part >= kGapBase) {
    const size_t g = static_cast<size_t>(part - kGapBase);
    const auto& gaps = model_.data().gaps;
    const bool spectral = g >= gaps.size() || gaps[g] == dsp::GapMode::Spectral;
    title = spectral ? "Spectral morph" : "Crossfade";
    std::snprintf(buf, sizeof buf, "Between keyframes %zu and %zu. Click to switch to %s.", g + 1, g + 2,
                  spectral ? "a crossfade" : "a spectral morph");
  } else {
    title = "Wavetable";
    std::snprintf(buf, sizeof buf, "%zu keyframe%s, %u frames. Double-click the frame to open it to its atoms.",
                  model_.data().keyframes.size(), model_.data().keyframes.size() == 1 ? "" : "s", model_.frames());
  }
  body = buf;
}

}  // namespace

std::unique_ptr<MetaFace> makeWavetableFace(const meta::Type& t) { return std::make_unique<WavetableFace>(t); }

}  // namespace pq::views
