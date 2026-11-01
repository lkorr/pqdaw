// The Wavetable face (TMM.7, docs/plans/MM.md "Wavetable face", "Per-voice dots"; the user's sketch rotated upright,
// the display after Live's Wavetable). Label-free, in the Phase accumulator's language (the same theme tokens: a dark
// body, quiet tracks and frame, values in the accent, voices as neon-blue glowing dots).
//
//   top        the Phase / Shape mode switch by the input jack; the Basic Shapes chooser (replaces the selected
//              keyframe's cycle); the harmonic rows of the selected keyframe — amplitude, and phase about a centre line
//              (read-only here: TMM.9 edits them and adds the draw tools in the gap left of the chooser)
//   display    the table's dense frames as offset lines receding up-right (a static layer keyed by the instance's
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
// Every drag and click is one undo step and never a compile: params are runtime values, fold / warp / sync are baked by
// the link off the audio thread, data edits are the MetaData class. The warp mode is a setting (one compile, as every
// setting; TMM.8's live path reads it). Shape mode is refused until TMM.8 (Q171): its switch says so.
//
// World units (204 × 248): see the layout constants below.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

#include "faces.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/meta/wavetable.h"
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
// top row: the mode switch by the input jack, the Basic Shapes chooser at the right
constexpr float kRowY0 = 17.0f, kRowY1 = 31.0f;
constexpr float kModeX0 = 14.0f, kCell = 14.0f, kCellGap = 1.5f;
constexpr float kShapeX1 = 188.0f;
constexpr uint32_t kShapeCount = 6;
// harmonic rows
constexpr float kHarmX0 = 16.0f, kHarmX1 = 188.0f;
constexpr float kAmpY0 = 37.0f, kAmpY1 = 57.0f;
constexpr float kPhY0 = 60.0f, kPhY1 = 72.0f;
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
  kHarmPart = 40,
  kKeyBase = 1000,  // + keyframe
  kGapBase = 3000,  // + gap
};

Rect modeCell(uint32_t i) noexcept {
  const float x0 = kModeX0 + static_cast<float>(i) * (kCell + kCellGap);
  return {x0, kRowY0, x0 + kCell, kRowY1};
}
Rect shapeCell(uint32_t i) noexcept {
  const float x0 = kShapeX1 - static_cast<float>(kShapeCount - i) * kCell - static_cast<float>(kShapeCount - 1 - i) * kCellGap;
  return {x0, kRowY0, x0 + kCell, kRowY1};
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
enum class Glyph : uint8_t { Sine, Triangle, Saw, Square, Pulse, Noise, PhaseMode, ShapeMode, Bend, Mirror, PulseWidth, Fold, Sync };

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
    }
    p.polyline(pts_, th, col);
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

  // ---- static layers: body, tracks, frame + glyphs (shared by the type), the stacked frames (this instance's) ----
  uint32_t staticLayerCount() const noexcept override { return 4; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    switch (layer) {
      case 0: return th.face.body;
      case 1: return th.face.track;
      case 2: return th.face.frame;
      default: return th.face.tick;
    }
  }
  uint64_t staticKey(uint32_t layer) const override {
    if (layer != 3) return 0;
    refresh();
    return model_.key();
  }

  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    switch (layer) {
      case 0: p.roundedRect({1.0f, 1.0f, kW - 1.0f, kH - 1.0f}, kRadius, w); return;
      case 1: paintTracks(p, w); return;
      case 2: paintFrame(p, w); return;
      default: paintStack(p); return;
    }
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    refresh();
    const ui::Theme& th = *env.theme;
    const double now = clockNow();
    paintCells(p, th);
    paintHarmonics(p, th);
    paintSliders(p, env);
    paintKnobs(p, env);
    paintDisplay(p, env, now);
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
    if (l.x >= kHarmX0 && l.x <= kHarmX1 && l.y >= kAmpY0 - 1.0f && l.y <= kPhY1 + 1.0f) return kHarmPart;
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
      case K::Wheel:
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
    if (e.key != ui::key::kDelete && e.key != ui::key::kBackspace) return false;
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
    if (part >= kWarpMode0 && part != kHarmPart) return ui::Cursor::Pointer;  // switches, shapes, gap glyphs
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
    const auto n = static_cast<int32_t>(model_.data().keyframes.size());
    selected_ = std::clamp(selected_, 0, std::max(n - 1, 0));
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
    // The harmonic rows: one slot per bar, the phase row's centre line.
    const float pitch = (kHarmX1 - kHarmX0) / wt::kBars;
    for (uint32_t h = 0; h < wt::kBars; ++h) {
      const float x0 = kHarmX0 + pitch * static_cast<float>(h) + 0.6f;
      p.roundedRect({x0, kAmpY0, x0 + pitch - 1.2f, kAmpY1}, 1.0f, ui::mulAlpha(w, 0.45f));
      p.roundedRect({x0, kPhY0, x0 + pitch - 1.2f, kPhY1}, 1.0f, ui::mulAlpha(w, 0.45f));
    }
    // The display's well, the sliders' tracks, the knobs' arcs.
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
    for (uint32_t i = 0; i < kShapeCount; ++i) glyphs_.draw(p, kShapes[i], centreOf(shapeCell(i)), 4.2f, 3.5f, 1.0f, w);
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
      place(model_.frame(d), u, f > 1 ? 2u : 1u);
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
    const int32_t sh = model_.shapeOf(static_cast<uint32_t>(selected_));
    for (uint32_t i = 0; i < kShapeCount; ++i)
      cell(shapeCell(i), sh == static_cast<int32_t>(i), kShape0 + static_cast<int32_t>(i), kShapes[i], 4.2f, 3.5f);
    static constexpr Glyph kWarps[3] = {Glyph::Bend, Glyph::Mirror, Glyph::PulseWidth};
    const auto wm = static_cast<int32_t>(std::round(target().setting(meta::kWtWarpMode)));
    for (uint32_t i = 0; i < 3; ++i)
      cell(warpCell(i), wm == static_cast<int32_t>(i), kWarpMode0 + static_cast<int32_t>(i), kWarps[i], 3.8f, 3.2f);
  }

  void paintHarmonics(ui::FacePainter& p, const ui::Theme& th) {
    model_.harmonics(static_cast<uint32_t>(selected_), amp_, phase_);
    const float pitch = (kHarmX1 - kHarmX0) / wt::kBars;
    const float mid = (kPhY0 + kPhY1) * 0.5f, half = (kPhY1 - kPhY0) * 0.5f - 0.5f;
    const Rgba ampCol = ui::mulAlpha(th.face.value, hovered_ == kHarmPart ? 0.9f : 0.7f);
    const Rgba phCol = ui::mulAlpha(th.face.handle, hovered_ == kHarmPart ? 0.95f : 0.75f);
    for (uint32_t h = 0; h < wt::kBars; ++h) {
      const float x0 = kHarmX0 + pitch * static_cast<float>(h) + 1.0f, x1 = x0 + pitch - 2.0f;
      const float a = std::clamp(amp_[h], 0.0f, 1.0f);
      // A bar never vanishes below a hairline while the harmonic is present (−60 dB still reads as "there").
      const float hgt = a > 1e-3f ? std::max(1.0f, a * (kAmpY1 - kAmpY0 - 1.0f)) : 0.0f;
      p.roundedRect({x0, kAmpY1 - 0.5f - hgt, x1, kAmpY1 - 0.5f}, 0.8f, hgt > 0.0f ? ampCol : 0);
      // Phase: a faint bar from the centre line and a bright tick at the value.
      const float ph = a > 1e-3f ? phase_[h] / kPi : 0.0f;  // −1 … 1
      const float yv = mid - ph * half;
      const float y0 = std::min(mid, yv), y1 = std::max(mid, yv);
      p.roundedRect({x0, y0, x1, y1}, 0.5f, a > 1e-3f ? ui::mulAlpha(phCol, 0.35f) : 0);
      p.roundedRect({x0, yv - 0.7f, x1, yv + 0.7f}, 0.5f, a > 1e-3f ? phCol : 0);
    }
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
    for (size_t k = 0; k < d.keyframes.size(); ++k) {
      const float y = posY(d.keyframes[k].position);
      const bool sel = static_cast<int32_t>(k) == selected_;
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
    place(blend_, u, 1);
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
      place(blend_, vu, 2);
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
  void commitData(const char* label, const meta::WavetableData& d) {
    if (!target().editable()) return;
    target().begin(label);
    target().setData(meta::wavetableData(d));
    target().end();
  }

  // State read from the target (mutable: refreshed from const paths such as the static layers and the tile key).
  mutable wt::Model model_;
  mutable int32_t selected_ = 0;
  mutable std::vector<Vec2> curve_;
  mutable Glyphs glyphs_;
  std::vector<float> blend_;
  std::array<float, wt::kBars> amp_{}, phase_{};
  std::vector<Shown> shown_;
  // drags
  int32_t drag_ = kNone;
  bool begun_ = false;
  bool keyFocus_ = false;  // the last press was on the position slider or a marker: Delete removes the selected keyframe
  float dragT_ = 0.0f;
  Vec2 last_{0.0f, 0.0f};
  meta::WavetableData dragData_;  // a marker drag edits a copy of the data it began with
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
  keyFocus_ = part == kPositionPart || (part >= kKeyBase && part < kGapBase);
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
        size_t at = 0;
        while (at < n.keyframes.size() && n.keyframes[at].position <= u) ++at;
        const dsp::GapMode mode = at > 0 && at - 1 < n.gaps.size() ? n.gaps[at - 1] : dsp::GapMode::Spectral;
        n.keyframes.insert(n.keyframes.begin() + static_cast<std::ptrdiff_t>(at), {u, model_.sourceCycle(u)});
        if (!n.gaps.empty() || mode != dsp::GapMode::Spectral) {
          n.gaps.resize(d.keyframes.size() - 1, dsp::GapMode::Spectral);
          n.gaps.insert(n.gaps.begin() + static_cast<std::ptrdiff_t>(std::min(at, n.gaps.size())), mode);
          n.gaps.resize(n.keyframes.size() - 1, mode);
        }
        selected_ = static_cast<int32_t>(at);
        commitData("Add Keyframe", n);
        drag_ = kNone;
        return true;
      }
      if (tg.editable()) tg.reset(static_cast<size_t>(part));
      drag_ = kNone;
      return true;
    }
    drag_ = part;
    dragT_ = travel(part);
    last_ = e.device;
    begun_ = false;
    return true;
  }
  if (part >= kKeyBase && part < kGapBase) {
    selected_ = part - kKeyBase;
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
    if (!model_.ok()) return true;
    meta::WavetableData n = d;
    const AlignedBuffer<float> c = dsp::basicCycle(static_cast<dsp::BasicCycle>(part - kShape0));
    n.keyframes[static_cast<size_t>(selected_)].cycle.assign(c.span().begin(), c.span().end());
    commitData("Basic Shape", n);
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
  return false;  // the harmonic rows (read-only here) and the frame: the host's (select, move, open)
}

bool WavetableFace::move(const FacePointer& e) {
  if (drag_ == kNone) return false;
  MetaTarget& tg = target();
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
  char buf[200];
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
  } else if (part == kHarmPart) {
    title = "Harmonics";
    std::snprintf(buf, sizeof buf, "Keyframe %d's first %u harmonics: amplitude, and phase relative to a sine.", selected_ + 1, wt::kBars);
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
