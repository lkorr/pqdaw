// The VCA face (TMM.14; SPEC §15 2026-11-01 "a VCA meta module"; TX.orbit-ports, SPEC §15 2026-11-03 "orbiting ports, a
// round VCA, Delete on a cable": the user's sketch and SeptaBee's Pomegranate, in the meta faces' family: a dark body,
// quiet tracks, the value in the accent, voices as neon-blue glowing dots). A CIRCLE, and little else:
//
//   readout    the value (3 significant digits, k for thousands) at the top, where the sketch's ✕ was. Double-click to
//              type one.
//   knob       a circular dial filling most of the circle (TX.vca-knob: the track's outer edge ≈ 0.85 of the body's
//              radius, its ticks to 0.9) on a full 360° track: min at 6 o'clock, clockwise all the way round to max at 6
//              o'clock from the other side (value 0 at 12 o'clock on the default bipolar range); a drag clamps at the
//              ends, it never wraps across 6 o'clock. A pie wedge in the accent fills from the value's zero (or the
//              range's end nearest it) to the pointer, like Pomegranate's; the track carries the same span. Ticks: the
//              end stop at 6 o'clock and, on the Log curve, the MARKERS (pq/meta/vca.h: −100, −10, −1, +1, +10, +100,
//              each where it plays; the ±1 pair longer and brighter, the fine zone between them a brighter band of the
//              track; a marker outside min … max is not drawn). Drag up / down (Shift: fine); double-click: ×1 (the
//              position of value 1, or the nearest end).
//   markers    a tick dragged ALONG THE TRACK moves that marker (it follows the pointer round the circle, clamped a gap
//              short of its neighbours; one undo step); the curve box's corner points are the same markers, dragged
//              left / right. TX.vca-knob2: PAIRED — a plain drag moves the marker of the opposite value with it, mirrored
//              (+10 out by some travel: −10 out by the same; meta::vcaMoveMark), Ctrl+drag moves it alone. Double-click
//              a marker to TYPE its place in % of the knob's travel (Ctrl+double-click: that marker alone; Enter
//              commits, Esc cancels, an empty entry restores the default place); shown on the readout, or in the
//              curve box while the panel is open.
//   chevron    inside the dial under the hub (mirroring the readout above it; the track has no gap now): opens the
//              FLOATING PANEL (Face::popupRect) — the min / max range and
//              the curve box, as the face's lower half was — under the face over the canvas (nothing moves; it follows
//              the face at any zoom). The chevron, the panel's own caret, Esc or a press anywhere else closes it. A host
//              without room around the face (a Device-view panel) opens it over the face (setPopupOverFace).
//   panel      min — max: the range's ends, joined by a line (the sketch's "−20 —— 20"). Drag up / down (on the
//              default bipolar-log scale, rounded to 3 significant digits); double-click to type (Enter commits, Esc
//              cancels, an empty entry restores the default). min ≤ max is kept.
//              curve box: the value against the knob position (the Log curve on a symmetric-log scale — linear inside
//              ±1, one equal step per decade beyond (TX.vca-knob2: ±1, ±10, ±100 evenly spaced) — the Bend curve
//              linear), the knob's position on it (a dotted drop and a dot), each voice's dot. Drag up / down to shape
//              it: the Log curve's fine zone (up: wider: the ±1 markers move apart symmetrically and the others with
//              the outers, each keeping its offset), the Bend curve's bend (up: bulges up, logarithmic). The markers'
//              corner points on the curve (dots, the ±1 pair larger) drag on their own. Double-click the box: the
//              shape's default (the zone, every marker at offset 0).
//              presets (TX.vca-knob2): six chips under the box, each a glyph of its own curve (value against the knob,
//              linear; a faint zero line, a dot at ×1) with a tooltip: Default (bipolar log ±20k), 0 → 1, −1 → +1,
//              ×0 … ×2, Fader (0 … 1 on an audio taper), 20 Hz → 20 kHz (octaves). Each sets the range, the curve and
//              the markers in one undo step; the one the state matches is lit.
//
// The signal's ports are the host's (orbiting the circle: pq/views/circuit/orbit.h); the face draws no jacks or leads for
// them. Voices: the knob position each voice plays (the tap: automation, modulation wires and macros move the knob) is a
// glowing dot on the track (and on the open panel's curve), with the positions sampled during the last displayed frame
// as a faint trail. Every control is a runtime param (a drag never compiles) except the curve kind (a preset that
// switches between Log and Bend recompiles its unit). World units 120 × 120 (the panel 120 × 80 under it).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include "faces.h"
#include "pq/meta/vca.h"

namespace pq::views {

namespace {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float deg(float d) noexcept { return d * kPi / 180.0f; }

// ---- layout (world units) ----
constexpr float kW = 120.0f, kH = 120.0f;
constexpr Vec2 kO{60.0f, 60.0f};                   // the body's centre
constexpr float kBodyR = 58.0f;
constexpr Vec2 kC{60.0f, 61.0f};                   // the knob's centre
// The knob is most of the circle (the user 2026-11-03: "knob should be the vast majority of the circular space"): the
// track's outer edge with its end ticks reaches ≈ 0.9 of the body's radius.
constexpr float kRingR = 46.5f, kRingW = 4.5f;     // the travel track
constexpr float kDialR = 41.5f;                    // the dial
constexpr float kWedgeR = 38.5f;                   // the pie wedge
constexpr float kHubR = 5.0f;
constexpr float kA0 = 90.0f, kSweep = 360.0f;      // degrees: p = 0 at 6 o'clock, clockwise all the way round
constexpr float kReadoutY = 37.0f, kReadoutPx = 10.5f;  // inside the dial, above the hub, on a dark pill
constexpr Rect kReadout{37.0f, 29.5f, 83.0f, 44.5f};
constexpr Vec2 kChev{60.0f, 85.0f};                // the chevron's centre: inside the dial below the hub
constexpr Rect kChevRect{50.0f, 78.5f, 70.0f, 91.5f};  // (the readout's mirror image about the knob's centre)
// The markers' ticks, outside the track: from its outer edge out (±1 longer); a press in that band near one grabs it.
constexpr float kTickR0 = kRingR + kRingW * 0.5f + 1.0f, kTickLong = 4.5f, kTickShort = 2.6f;
constexpr float kTickHitR0 = kRingR + kRingW * 0.5f + 0.25f, kTickHitR1 = kTickR0 + kTickLong + 2.0f;
constexpr float kMarkHit = 3.5f;  // world units along the track / around a corner point
// The floating panel: kPopH tall from its top (under the face, or over it), its rows from there.
constexpr float kPopH = 80.0f, kPopGap = 6.0f, kPopOverTop = 38.0f;
constexpr float kRangeDy = 17.0f, kRangePx = 8.0f;
constexpr float kBoxDy0 = 28.0f, kBoxDy1 = 58.0f, kBoxPad = 3.0f;
constexpr float kChipDy0 = 63.0f, kChipDy1 = 73.0f, kChipW = 16.0f, kChipX0 = 7.0f, kChipStep = 18.0f;
constexpr int kCurveN = 64;   // points of the drawn curve (the same at every zoom)
constexpr int kWedgeN = 16;   // points per third of the wedge
constexpr int kGlyphN = 17;   // points per preset glyph

// ---- parts ----
enum Part : int32_t {
  kKnob = 0,
  kValueText = 1,
  kMinText = 2,
  kMaxText = 3,
  kCurveBox = 4,
  kPreset0 = 5,
  kPresets = 6,
  kChevron = kPreset0 + kPresets,  // the face's chevron and the panel's caret: open / close
  kPanel = kChevron + 1,           // the panel's background (a press there keeps it open)
  kTick0 = kPanel + 1,             // TX.vca-knob: marker i's tick on the track (kTick0 + i)
  kCorner0 = kTick0 + static_cast<int32_t>(meta::kVcaMarks)  // … and its corner point in the curve box
};
constexpr bool isTick(int32_t part) noexcept { return part >= kTick0 && part < kTick0 + static_cast<int32_t>(meta::kVcaMarks); }
constexpr bool isCorner(int32_t part) noexcept {
  return part >= kCorner0 && part < kCorner0 + static_cast<int32_t>(meta::kVcaMarks);
}
constexpr bool isMark(int32_t part) noexcept { return isTick(part) || isCorner(part); }
constexpr uint32_t markOf(int32_t part) noexcept { return static_cast<uint32_t>(isTick(part) ? part - kTick0 : part - kCorner0); }
// The ±1 pair (the fine zone's ends) is drawn stronger than the outer markers.
bool unitMark(uint32_t i) noexcept { return std::abs(meta::kVcaMarkValue[i]) == meta::kVcaFine; }
// "+10", "-100" (ASCII: the info view's titles).
std::string markName(uint32_t i) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%+g", meta::kVcaMarkValue[i]);
  return buf;
}

// TX.vca-knob2: the presets (the user: "we at least need one of them to scale 0→1, one of them −1 to +1, one of them to
// be default"). Each sets the curve, the range, its shape and the markers (at their places) in one undo step.
struct Preset {
  const char* name;  // the info view's title (ASCII)
  const char* body;  // the tooltip
  meta::VcaCurve curve;
  double min, max;
  double shape;      // zone (Log) or bend (Bend)
};
constexpr Preset kPresetList[kPresets] = {
    {"Default", "Bipolar log, -20k ... +20k: -1 ... +1 fine around 12 o'clock, logarithmic to the ends.",
     meta::VcaCurve::Log, -meta::kVcaRange, meta::kVcaRange, 0.4},
    {"0 -> 1", "Unipolar linear, 0 ... 1: a level or a gain (0 at the start, 1 at the end).", meta::VcaCurve::Bend, 0.0, 1.0,
     0.0},
    {"-1 -> +1", "Bipolar linear, -1 ... +1: an attenuverter (0 at 12 o'clock, inverted to the left).",
     meta::VcaCurve::Bend, -1.0, 1.0, 0.0},
    {"x0 ... x2", "Linear gain, 0 ... 2: unity (x1) at 12 o'clock, up to +6 dB.", meta::VcaCurve::Bend, 0.0, 2.0, 0.0},
    {"Fader", "0 ... 1 on an audio taper (exponential: fine near silence; -18.5 dB at 12 o'clock).", meta::VcaCurve::Bend,
     0.0, 1.0, 0.5},
    {"20 Hz -> 20 kHz", "A frequency, 20 Hz ... 20 kHz on an octave curve: equal ratios per turn, about an octave per tenth of the knob.",
     meta::VcaCurve::Log, 20.0, 20000.0, 0.4}};
meta::VcaLaw presetLaw(const Preset& pr) {
  return meta::vcaLaw(pr.curve, pr.min, pr.max, pr.curve == meta::VcaCurve::Log ? pr.shape : 0.4,
                      pr.curve == meta::VcaCurve::Bend ? pr.shape : 0.0);
}
// The curve box's value scale for the Log curve (TX.vca-knob2: "a bit more logarithmic"): linear inside ±1, one step
// per decade beyond (C1 at ±1), so ±1, ±10, ±100 sit at equal steps.
double symLog(double a) noexcept { return a <= 1.0 ? a / std::numbers::ln10 : (1.0 + std::log(a)) / std::numbers::ln10; }
// A marker's place as typed and shown: % of the knob's travel.
std::string formatPlace(double p) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f", 100.0 * p);
  return buf;
}

float angleOf(double p) noexcept { return deg(kA0 + kSweep * static_cast<float>(std::clamp(p, 0.0, 1.0))); }
Vec2 polar(float r, float a) noexcept { return {kC.x + r * std::cos(a), kC.y + r * std::sin(a)}; }
bool inside(const Rect& r, Vec2 p, float slack = 0.0f) noexcept {
  return p.x >= r.x0 - slack && p.x <= r.x1 + slack && p.y >= r.y0 - slack && p.y <= r.y1 + slack;
}
Vec2 centreOf(const Rect& r) noexcept { return {(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}; }

// A leading '-' as the typographic minus (U+2212, in the UI font).
std::string minusSign(std::string s) {
  if (!s.empty() && s[0] == '-') s.replace(0, 1, "\xE2\x88\x92");
  return s;
}

// 3 significant digits, k for thousands: "1.00", "-0.50", "137", "1.68k", "-20.0k".
std::string formatValue(double v) {
  char buf[32];
  const double a = std::abs(v);
  if (a >= 9999.5) std::snprintf(buf, sizeof buf, "%.1fk", v / 1000.0);
  else if (a >= 999.5) std::snprintf(buf, sizeof buf, "%.2fk", v / 1000.0);
  else if (a >= 99.95) std::snprintf(buf, sizeof buf, "%.0f", v);
  else if (a >= 9.995) std::snprintf(buf, sizeof buf, "%.1f", v);
  else std::snprintf(buf, sizeof buf, "%.2f", v);
  std::string s = buf;
  if (s == "-0.00") s = "0.00";
  return minusSign(s);
}
// A range end, compact: "-20k", "20k", "0", "1", "0.5", "150", "1.5k".
std::string formatEnd(double v) {
  char buf[32];
  const double a = std::abs(v);
  if (a >= 999.5) std::snprintf(buf, sizeof buf, "%.3gk", v / 1000.0);
  else std::snprintf(buf, sizeof buf, "%.3g", v);
  std::string s = buf;
  if (s == "-0") s = "0";
  return minusSign(s);
}
// Rounded to 3 significant digits (a dragged range end lands on readable numbers).
double roundSig(double v) noexcept {
  if (v == 0.0 || !std::isfinite(v)) return 0.0;
  const double e = std::pow(10.0, std::floor(std::log10(std::abs(v))) - 2.0);
  return std::round(v / e) * e;
}
// A typed number: digits, one '.', a leading '-', an optional k (× 1000). nullopt: not a number.
std::optional<double> parseTyped(const std::string& s) {
  if (s.empty()) return std::nullopt;
  std::string t = s;
  double mul = 1.0;
  if (t.back() == 'k' || t.back() == 'K') {
    mul = 1000.0;
    t.pop_back();
  }
  if (t.empty() || t == "-" || t == "." || t == "-.") return std::nullopt;
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end != t.c_str() + t.size() || !std::isfinite(v)) return std::nullopt;
  return v * mul;
}

// The default bipolar-log scale: the range ends drag along it.
const meta::VcaLaw& endScale() {
  static const meta::VcaLaw l = meta::vcaLaw(meta::VcaCurve::Log, -meta::kVcaRange, meta::kVcaRange, 0.4, 0.0);
  return l;
}

class VcaFace final : public MetaFace {
public:
  explicit VcaFace(const meta::Type& t) : MetaFace(t) {}

  Vec2 worldSize() const noexcept override { return {kW, kH}; }
  FaceShape shape() const noexcept override { return {true, kO, kBodyR}; }  // TX.orbit-ports: the ports go round it
  Vec2 captionAt() const noexcept override { return {6.0f, 4.0f}; }  // the empty top-left corner

  // A wired param's pin (no anchor: the range and curve are in the panel) beside the knob's left.
  float inputY(size_t i, size_t n) const noexcept override {
    if (n != type().inputs.size() + type().params.size()) return i == 0 ? kC.y : MetaFace::inputY(i, n);
    static constexpr float kY[] = {kC.y, 76.0f, 88.0f, 96.0f, 104.0f, 112.0f};
    return i < std::size(kY) ? kY[i] : MetaFace::inputY(i, n);
  }
  float outputY(size_t j, size_t) const noexcept override { return kC.y + 16.0f * static_cast<float>(j); }

  // ---- static layers: body (the disc); tracks (the knob's track and dial); frame (the rim, the end stops) ----
  uint32_t staticLayerCount() const noexcept override { return 3; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.body : (layer == 1 ? th.face.track : th.face.frame);
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) {
      p.disc(kO, kBodyR, w);
      return;
    }
    if (layer == 1) {
      p.ring(kC, kRingR, kRingW, w);  // TX.vca-knob: the whole circle
      p.disc(kC, kDialR, ui::mulAlpha(w, 0.45f));
      return;
    }
    p.ring(kO, kBodyR - 0.75f, 1.25f, w);
    // The end stop at 6 o'clock, where min and max meet.
    const float a = angleOf(0.0);
    p.line(polar(kRingR - kRingW * 0.5f - 1.0f, a), polar(kRingR + kRingW * 0.5f + 2.5f, a), 1.2f, ui::mulAlpha(w, 0.8f));
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    const ui::Theme& th = *env.theme;
    refresh();
    const bool valueDriven = driven(env, "value");
    const Rgba accent = valueDriven ? th.face.valueDriven : th.face.value;
    // Hover: the hovered control lights.
    p.ring(kC, kRingR, kRingW + 1.5f, hovered_ == kKnob || drag_ == kKnob ? th.face.hover : 0);
    const bool readoutHot = hovered_ == kValueText || edit_ == kValueText;

    // ---- the knob ----
    // The fine zone (Log): a brighter band of the track between the ±1 markers; a tick per shown marker (the ±1 pair
    // longer and brighter; the hovered or dragged one lit, with a dot at its end).
    const auto [f0, f1] = fineSpan();
    const bool fine = law_.curve == meta::VcaCurve::Log && f1 > f0 + 1e-6;
    p.arc(kC, kRingR, angleOf(f0), angleOf(f1), kRingW, fine ? ui::mulAlpha(th.face.handle, 0.22f) : 0);
    for (uint32_t i = 0; i < meta::kVcaMarks; ++i) {
      const meta::VcaLaw::Mark& m = law_.marks[i];
      const float a = angleOf(m.at);
      const bool unit = unitMark(i), hot = markHot(i);
      const float len = unit ? kTickLong : kTickShort;
      const Rgba col = !m.shown ? 0 : hot ? th.face.value : ui::mulAlpha(th.face.handle, unit ? 0.85f : 0.5f);
      p.line(polar(kTickR0, a), polar(kTickR0 + len, a), hot ? 1.6f : (unit ? 1.2f : 0.9f), col);
      p.disc(polar(kTickR0 + len + 0.9f, a), 1.3f, m.shown && hot ? th.face.value : 0);
    }
    // The value's span on the track and the pie wedge, from the anchor (value 0, or the end nearest it) to the pointer.
    const double anchor = law_.lo >= 0.0 ? 0.0 : (law_.hi <= 0.0 ? 1.0 : meta::vcaPosition(law_, 0.0));
    const float a0 = angleOf(anchor), a1 = angleOf(pos_);
    p.arc(kC, kRingR, std::min(a0, a1), std::max(a0, a1), kRingW - 1.0f, accent);
    wedge(p, a0, a1, ui::lerp(th.face.body, accent, 0.85f));  // opaque: the halves overlap without a seam
    // The pointer: from the hub to the track, a disc on the track.
    const float pa = angleOf(pos_);
    const Rgba hand = valueDriven ? th.face.valueDriven : th.face.handle;
    p.line(polar(kHubR + 1.0f, pa), polar(kRingR - 1.0f, pa), drag_ == kKnob ? 2.4f : 1.9f, hand);
    p.disc(polar(kRingR, pa), 2.9f, hand);
    p.disc(kC, kHubR, th.face.hub);
    p.ring(kC, kHubR - 0.5f, 1.0f, ui::mulAlpha(th.face.handle, 0.8f));

    // ---- the readout (at the top, inside the dial: a dark pill over the wedge and the pointer) ----
    // TX.vca-knob2: a marker's place being typed shows here while the panel is closed ("±10: 82.5|", % of the knob).
    const bool markTyping = isMark(edit_) && !open_, typing = edit_ == kValueText || markTyping;
    p.roundedRect(kReadout, 4.0f, ui::mulAlpha(th.face.body, 0.82f));
    p.roundedRect(kReadout, 4.0f, typing ? th.face.track : (readoutHot ? th.face.hover : 0));
    p.text({kC.x, kReadoutY}, edit_ == kValueText ? editShown() : markTyping ? markEditShown() : formatValue(value_),
           markTyping ? 7.5f : kReadoutPx, typing ? th.face.handle : accent);

    // ---- the chevron: down while the panel is closed, up while it is open ----
    const bool chevHot = hovered_ == kChevron;
    p.roundedRect(kChevRect, 4.0f, ui::mulAlpha(th.face.body, 0.82f));  // a dark pill over the wedge, as the readout's
    p.roundedRect(kChevRect, 4.0f, chevHot ? th.face.hover : 0);
    const float cy = open_ ? -1.5f : 1.5f;  // closed: a "v" (it opens below); open: a "^"
    const Vec2 chev[3] = {{kChev.x - 4.5f, kChev.y - cy}, {kChev.x, kChev.y + cy}, {kChev.x + 4.5f, kChev.y - cy}};
    p.polyline(chev, 1.4f, open_ ? th.face.value : ui::mulAlpha(th.face.handle, chevHot ? 0.95f : 0.65f));

    // Selection / hover of the block: the rim.
    p.ring(kO, kBodyR - 0.75f, env.selected ? 1.75f : 1.25f,
           env.selected ? th.circuit.selected : (env.hovered && hovered_ == kFrame ? th.circuit.hover : 0));
    paintVoices(p, th, false);
  }

  // ---- the floating panel ----
  std::optional<Rect> popupRect() const noexcept override {
    if (!open_) return std::nullopt;
    const float t = popTop();
    return Rect{0.0f, t, kW, t + kPopH};
  }
  void setPopupOverFace(bool on) override { over_ = on; }

  void paintPopup(ui::FacePainter& p, const FaceEnv& env) override {
    if (!open_ || env.theme == nullptr) return;
    const ui::Theme& th = *env.theme;
    refresh();
    const float t = popTop();
    const Rect body{0.5f, t + 0.5f, kW - 0.5f, t + kPopH - 0.5f};
    // A soft shadow, the face's body and rim, the caret that closes it at the top.
    p.roundedRect({body.x0 + 1.5f, body.y0 + 2.5f, body.x1 + 1.5f, body.y1 + 2.5f}, 10.0f, ui::rgba(0, 0, 0, 90));
    p.roundedRect(body, 10.0f, th.face.frame);
    p.roundedRect({body.x0 + 1.25f, body.y0 + 1.25f, body.x1 - 1.25f, body.y1 - 1.25f}, 8.75f, th.face.body);
    const Rect caret = caretRect();
    p.roundedRect(caret, 3.0f, hovered_ == kChevron ? th.face.hover : 0);
    const Vec2 cc = centreOf(caret);
    const Vec2 up[3] = {{cc.x - 4.5f, cc.y + 1.5f}, {cc.x, cc.y - 1.5f}, {cc.x + 4.5f, cc.y + 1.5f}};
    p.polyline(up, 1.4f, th.face.value);
    const Rgba accent = driven(env, "value") ? th.face.valueDriven : th.face.value;
    const Rgba hand = driven(env, "value") ? th.face.valueDriven : th.face.handle;

    // ---- range ----
    for (const int32_t part : {kMinText, kMaxText}) {
      const bool hot = hovered_ == part || drag_ == part || edit_ == part;
      p.roundedRect(textRect(part), 3.0f, edit_ == part ? th.face.track : (hot ? th.face.hover : 0));
    }
    const float ry = t + kRangeDy;
    p.line({minRect().x1 + 2.0f, ry}, {maxRect().x0 - 2.0f, ry}, 1.0f, th.face.ink);
    p.text(centreOf(minRect()), edit_ == kMinText ? editShown() : formatEnd(param(meta::kVcaMin)), kRangePx,
           ui::mulAlpha(th.face.handle, edit_ == kMinText ? 1.0f : 0.85f));
    p.text(centreOf(maxRect()), edit_ == kMaxText ? editShown() : formatEnd(param(meta::kVcaMax)), kRangePx,
           ui::mulAlpha(th.face.handle, edit_ == kMaxText ? 1.0f : 0.85f));

    // ---- the curve box ----
    const Rect bx = box();
    p.roundedRect(bx, 5.0f, ui::mulAlpha(th.face.track, 0.6f));
    p.roundedRect(bx, 5.0f, hovered_ == kCurveBox || drag_ == kCurveBox ? th.face.hover : 0);
    const float zy = boxY(curveY(0.0));
    const bool zeroIn = law_.lo < 0.0 && law_.hi > 0.0;
    const Vec2 zl[2] = {{bx.x0 + 2.0f, zy}, {bx.x1 - 2.0f, zy}};
    p.dashed(zl, 0.6f, 1.5f, 2.0f, zeroIn ? ui::mulAlpha(th.face.handle, 0.3f) : 0);
    // The fine zone's share of the travel (Log): a faint band behind the curve (what the box's drag widens).
    const auto [f0, f1] = fineSpan();
    const bool fine = law_.curve == meta::VcaCurve::Log && f1 > f0 + 1e-6;
    p.roundedRect({boxX(f0), bx.y0 + 2.0f, std::max(boxX(f1), boxX(f0) + 0.5f), bx.y1 - 2.0f}, 1.5f,
                  fine ? ui::mulAlpha(th.face.handle, 0.07f) : 0);
    const float base = zeroIn ? zy : bx.y1 - kBoxPad;
    p.fillUnder(curve_, base, ui::mulAlpha(accent, 0.26f), ui::mulAlpha(accent, 0.04f));
    p.polyline(curve_, hovered_ == kCurveBox || drag_ == kCurveBox ? 1.9f : 1.4f, accent);
    const Vec2 at = boxPoint(pos_);
    const Vec2 drop[2] = {{at.x, bx.y0 + 2.0f}, {at.x, bx.y1 - 2.0f}};
    p.dashed(drop, 0.7f, 1.2f, 1.8f, ui::mulAlpha(th.face.handle, 0.45f));
    p.disc(at, 2.2f, hand);
    // The markers' corner points (TX.vca-knob): each draggable left / right on its own.
    for (uint32_t i = 0; i < meta::kVcaMarks; ++i) {
      const meta::VcaLaw::Mark& m = law_.marks[i];
      const Vec2 c = boxPoint(m.at);
      const bool unit = unitMark(i), hot = markHot(i);
      p.disc(c, hot ? 2.6f : (unit ? 1.9f : 1.4f), m.shown ? th.face.body : 0);
      p.ring(c, hot ? 2.6f : (unit ? 1.9f : 1.4f), hot ? 1.2f : 0.9f,
             !m.shown ? 0 : hot ? th.face.value : ui::mulAlpha(th.face.handle, unit ? 0.9f : 0.6f));
    }
    // TX.vca-knob2: a marker's place being typed, over the box's top while the panel is open.
    if (isMark(edit_)) {
      const Rect er{32.0f, bx.y0 + 1.5f, 88.0f, bx.y0 + 10.5f};
      p.roundedRect(er, 3.0f, th.face.track);
      p.text(centreOf(er), markEditShown(), 7.0f, th.face.handle);
    }
    // ---- presets: each chip its curve (value against the knob, linear), a faint zero line, a dot at x1 ----
    for (int i = 0; i < kPresets; ++i) {
      const Rect r = chipRect(i);
      const auto k = static_cast<size_t>(i);
      const bool on = presetActive(i), hot = hovered_ == kPreset0 + i;
      const Rgba ink = on ? th.face.value : ui::mulAlpha(th.face.handle, hot ? 0.95f : 0.6f);
      p.roundedRect(r, 3.0f, ui::mulAlpha(th.face.track, 0.5f));
      p.roundedRect(r, 3.0f, on ? ui::mulAlpha(th.face.value, 0.3f) : (hot ? th.face.hover : 0));
      const float gz = glyphZero_[k];
      p.line({r.x0 + 2.0f, gz}, {r.x1 - 2.0f, gz}, 0.5f, gz > 0.0f ? ui::mulAlpha(th.face.handle, 0.35f) : 0);
      p.polyline(glyph_[k], 1.0f, ink);
      p.disc(glyphUnit_[k], 1.1f, glyphUnit_[k].x > 0.0f ? ink : 0);
    }
    paintVoices(p, th, true);
  }

  void liveRects(std::vector<Rect>& out) const override {
    out.push_back({kC.x - kRingR - 6.0f, kC.y - kRingR - 6.0f, kC.x + kRingR + 6.0f, kC.y + kRingR + 6.0f});
    if (open_) out.push_back(box());
  }

  int32_t hitTest(Vec2 l, float pxPerUnit) const noexcept override {
    refresh();  // the markers' places
    // Grown at small zooms so a few device px always hit a control.
    const float slack = std::max(0.0f, 5.0f / std::max(pxPerUnit, 1e-6f) - 2.0f);
    if (const std::optional<Rect> pr = popupRect(); pr && inside(*pr, l)) {
      if (inside(caretRect(), l, std::min(slack, 2.0f))) return kChevron;
      for (int i = 0; i < kPresets; ++i)
        if (inside(chipRect(i), l, std::min(slack, 2.5f))) return kPreset0 + i;
      // TX.vca-knob: a marker's corner point, before the box (the nearest one in reach).
      int32_t best = kNone;
      float bestD = kMarkHit + std::min(slack, 2.0f);
      for (uint32_t i = 0; i < meta::kVcaMarks; ++i) {
        if (!law_.marks[i].shown) continue;
        const Vec2 c = boxPoint(law_.marks[i].at);
        const float d = std::hypot(l.x - c.x, l.y - c.y);
        if (d <= bestD) {
          bestD = d;
          best = kCorner0 + static_cast<int32_t>(i);
        }
      }
      if (best >= 0) return best;
      if (inside(box(), l, std::min(slack, 2.0f))) return kCurveBox;
      if (inside(minRect(), l, std::min(slack, 2.0f))) return kMinText;
      if (inside(maxRect(), l, std::min(slack, 2.0f))) return kMaxText;
      return kPanel;
    }
    if (l.x < 0.0f || l.y < 0.0f || l.x > kW || l.y > kH) return kNone;
    const float dx = l.x - kC.x, dy = l.y - kC.y;
    if (inside(kChevRect, l, std::min(slack, 2.0f))) return kChevron;
    if (inside(kReadout, l, std::min(slack, 2.0f))) return kValueText;  // inside the dial: before the knob
    const float r = std::sqrt(dx * dx + dy * dy);
    // TX.vca-knob: a marker's tick (the band just outside the track), before the knob: the nearest along the track.
    if (r >= kTickHitR0 && r <= kTickHitR1 + slack * 0.5f) {
      int32_t best = kNone;
      float bestD = kMarkHit + slack * 0.5f;
      const float a = std::atan2(dy, dx);
      for (uint32_t i = 0; i < meta::kVcaMarks; ++i) {
        if (!law_.marks[i].shown) continue;
        const float da = std::remainder(a - angleOf(law_.marks[i].at), 2.0f * kPi);
        const float d = std::abs(da) * r;
        if (d <= bestD) {
          bestD = d;
          best = kTick0 + static_cast<int32_t>(i);
        }
      }
      if (best >= 0) return best;
    }
    if (r <= kRingR + kRingW + 2.0f + slack * 0.5f) return kKnob;
    return kFrame;
  }

  bool onPointer(const FacePointer& e) override {
    using K = FacePointer::Kind;
    switch (e.kind) {
      case K::Down: return down(e);
      case K::Move:
        if (drag_ < 0) return false;
        move(e);
        return true;
      case K::Up: {
        if (drag_ < 0) return false;
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

  bool dragging() const noexcept override { return drag_ >= 0; }
  void cancelDrag() override {
    if (drag_ >= 0 && begun_) target().cancel();
    drag_ = kNone;
    begun_ = false;
  }
  // A press anywhere else: a type-in commits, the panel closes.
  void focusLost() override {
    if (edit_ >= 0) commitEdit();
    open_ = false;
  }

  // Typing into the readout or a range end (after a double-click): digits, '.', '-', k; Backspace; Enter; Esc. Esc
  // without a type-in closes the panel.
  bool onKey(const ui::KeyEvent& e) override {
    if (edit_ < 0) {
      if (e.down && open_ && e.key == ui::key::kEscape && (e.mods & ui::mod::kChordMask) == 0) {
        open_ = false;
        return true;
      }
      return false;
    }
    if (!e.down) return true;
    if ((e.mods & (ui::mod::kCtrl | ui::mod::kAlt | ui::mod::kSuper)) != 0) return false;
    const ui::Key k = e.key;
    if (k == ui::key::kEscape) {
      edit_ = kNone;
      return true;
    }
    if (k == ui::key::kReturn) {
      commitEdit();
      return true;
    }
    if (k == ui::key::kBackspace || k == ui::key::kDelete) {
      if (!editText_.empty()) editText_.pop_back();
      return true;
    }
    const bool mark = isMark(edit_);  // a marker's place: digits, '.', a trailing %
    if (editText_.size() < 12 && ((k >= '0' && k <= '9') || k == '.' || (!mark && (k == 'k' || k == 'K')) ||
                                  (!mark && k == '-' && editText_.empty()) || (mark && k == '%' && !editText_.empty()))) {
      editText_ += static_cast<char>(k == 'K' ? 'k' : k);
      return true;
    }
    return ui::isPrintableKey(e);  // other printable keys are swallowed while typing (no shortcuts fire)
  }

  ui::Cursor cursor(int32_t part) const noexcept override {
    if ((part >= kPreset0 && part < kPreset0 + kPresets) || part == kChevron) return ui::Cursor::Pointer;
    if (part == kPanel) return ui::Cursor::Arrow;
    if (part == edit_ && part >= 0 && !isMark(part)) return ui::Cursor::Text;
    if (isTick(part)) return ui::Cursor::Move;       // round the track
    if (isCorner(part)) return ui::Cursor::ResizeEW;  // along the box
    return part >= 0 ? ui::Cursor::ResizeNS : ui::Cursor::Inherit;
  }

  // The knob is the face's one wire target (the range and curve are in the panel: not on the canvas).
  std::optional<Rect> paramRect(size_t param) const noexcept override {
    if (param == meta::kVcaValue) return Rect{kC.x - kRingR - 3.0f, kC.y - kRingR - 3.0f, kC.x + kRingR + 3.0f, kC.y + kRingR + 3.0f};
    return std::nullopt;
  }

  void describe(int32_t part, std::string& title, std::string& body) const override {
    refresh();
    char buf[400];
    switch (part) {
      case kKnob:
        title = "Value";
        std::snprintf(buf, sizeof buf, "x%s (knob %.3f). Drag up / down (Shift: fine); double-click: x1.",
                      formatValue(value_).c_str(), pos_);
        break;
      case kValueText:
        title = "Value";
        std::snprintf(buf, sizeof buf, "x%s. Double-click to type a value (k = thousands; Enter, Esc).",
                      formatValue(value_).c_str());
        break;
      case kChevron:
        title = open_ ? "Close Range and Curve" : "Range and Curve";
        std::snprintf(buf, sizeof buf, "%s", open_ ? "Click (or Esc, or a click elsewhere) to close the panel."
                                                   : "Click to open the knob's min / max range and its curve in a panel.");
        break;
      case kPanel:
        title = "Range and Curve";
        std::snprintf(buf, sizeof buf, "The knob's range and curve. Esc or a click elsewhere closes the panel.");
        break;
      case kMinText:
      case kMaxText:
        title = part == kMinText ? "Min" : "Max";
        std::snprintf(buf, sizeof buf, "%s at the knob's %s end. Drag up / down; double-click to type (empty: default).",
                      formatEnd(param(part == kMinText ? meta::kVcaMin : meta::kVcaMax)).c_str(),
                      part == kMinText ? "left" : "right");
        break;
      case kCurveBox:
        if (law_.curve == meta::VcaCurve::Log) {
          title = "Curve: bipolar log";
          std::snprintf(buf, sizeof buf, "-1 ... 1 spans %.0f %% of the knob, logarithmic beyond. Drag up / down to widen or "
                                         "narrow it, a marker's dot to move it (with its pair; Ctrl: alone); double-click resets.",
                        100.0 * (fineSpan().second - fineSpan().first));
        } else {
          title = "Curve: bend";
          std::snprintf(buf, sizeof buf, "Bend %+.2f (0: linear; up: logarithmic, down: exponential). Drag up / down; "
                                         "double-click resets.", param(meta::kVcaBend));
        }
        break;
      default:
        if (isMark(part)) {
          const uint32_t i = markOf(part);
          const uint32_t j = meta::vcaMarkPartner(i);
          const bool pair = j < meta::kVcaMarks && law_.marks[j].shown;
          title = "Marker " + markName(i);
          const std::string how = pair ? " to move it with " + markName(j) + ", mirrored (Ctrl: alone)" : " to move it";
          std::snprintf(buf, sizeof buf,
                        "Value %s at %s %% of the knob (default %s %%). Drag %s%s; double-click to type its place in %% "
                        "(empty: default)%s.",
                        markName(i).c_str(), formatPlace(law_.marks[i].at).c_str(), formatPlace(law_.marks[i].base).c_str(),
                        isTick(part) ? "along the track" : "left / right", how.c_str(), pair ? " (Ctrl: alone)" : "");
          break;
        }
        if (part >= kPreset0 && part < kPreset0 + kPresets) {
          const Preset& pr = kPresetList[part - kPreset0];
          title = std::string("Preset: ") + pr.name;
          std::snprintf(buf, sizeof buf, "%s Click: the range, the curve and the markers in one step.", pr.body);
          break;
        }
        MetaFace::describe(part, title, body);
        body = "out = in x value: one knob from min to max through a curve (default: fine -1 ... 1 at the top, "
               "logarithmic to +-20k; the chevron opens the range and curve). Double-click the frame to open it to its "
               "atoms.";
        return;
    }
    body = buf;
  }

private:
  double param(size_t i) const { return i < type().params.size() ? target().value(i) : 0.0; }
  double curveSetting() const { return type().settings.empty() ? 0.0 : target().setting(meta::kVcaCurveSetting); }

  // The panel's geometry (face-local): under the face, or over it (setPopupOverFace).
  float popTop() const noexcept { return over_ ? kPopOverTop : kH + kPopGap; }
  Rect minRect() const noexcept { return {8.0f, popTop() + kRangeDy - 6.0f, 40.0f, popTop() + kRangeDy + 6.0f}; }
  Rect maxRect() const noexcept { return {80.0f, popTop() + kRangeDy - 6.0f, 112.0f, popTop() + kRangeDy + 6.0f}; }
  Rect box() const noexcept { return {14.0f, popTop() + kBoxDy0, 106.0f, popTop() + kBoxDy1}; }
  Rect chipRect(int i) const noexcept {
    const float x = kChipX0 + kChipStep * static_cast<float>(i);
    return {x, popTop() + kChipDy0, x + kChipW, popTop() + kChipDy1};
  }
  Rect caretRect() const noexcept { return {50.0f, popTop() + 1.5f, 70.0f, popTop() + 9.5f}; }

  // Re-derives the law, the value and the drawn curve when the state changed (cheap: kCurveN points).
  void refresh() const {
    std::array<double, meta::kVcaParams + 2> s{};
    for (size_t i = 0; i < meta::kVcaParams; ++i) s[i] = param(i);
    s[meta::kVcaParams] = curveSetting();
    s[meta::kVcaParams + 1] = over_ ? 1.0 : 0.0;  // the panel's place (its curve's points)
    if (s == state_ && !curve_.empty()) return;
    state_ = s;
    law_ = meta::vcaLaw(std::span<const double>(s.data(), meta::kVcaParams), s[meta::kVcaParams]);
    pos_ = std::clamp(s[meta::kVcaValue], 0.0, 1.0);
    value_ = meta::vcaValue(law_, pos_);
    // The display scale: symmetric log for the Log curve (linear inside ±1, a step per decade beyond), linear for Bend.
    const double m = std::max({std::abs(law_.lo), std::abs(law_.hi), 1.0});
    logScale_ = symLog(m);
    yLo_ = shown(law_.lo);
    yHi_ = shown(law_.hi);
    curve_.clear();
    for (int i = 0; i <= kCurveN; ++i) curve_.push_back(boxPoint(static_cast<double>(i) / kCurveN));
    buildGlyphs();
  }
  double shown(double v) const {
    if (law_.curve == meta::VcaCurve::Bend) return v;
    return (v < 0.0 ? -1.0 : 1.0) * symLog(std::abs(v)) / logScale_;
  }
  // The value's height in the box, 0 (bottom) … 1 (top).
  double curveY(double v) const {
    const double span = yHi_ - yLo_;
    return span > 0.0 ? std::clamp((shown(v) - yLo_) / span, 0.0, 1.0) : 0.5;
  }
  float boxY(double t) const {
    const Rect b = box();
    return b.y1 - kBoxPad - static_cast<float>(t) * (b.y1 - b.y0 - 2.0f * kBoxPad);
  }
  float boxX(double p) const {
    const Rect b = box();
    return b.x0 + kBoxPad + static_cast<float>(p) * (b.x1 - b.x0 - 2.0f * kBoxPad);
  }
  double boxP(float x) const {
    const Rect b = box();
    return (x - b.x0 - kBoxPad) / std::max(b.x1 - b.x0 - 2.0f * kBoxPad, 1e-3f);
  }
  Vec2 boxPoint(double p) const { return {boxX(p), boxY(curveY(meta::vcaValue(law_, p)))}; }

  // TX.vca-knob: the fine zone's travel, between the ±1 markers where they are (an end of the travel where one is not
  // shown: the base law's pN / pP are 0 or 1 then, which the warp keeps).
  std::pair<double, double> fineSpan() const {
    double a = law_.pN, b = law_.pP;
    for (const meta::VcaLaw::Mark& m : law_.marks) {
      if (!m.shown) continue;
      if (m.value == -meta::kVcaFine) a = m.at;
      if (m.value == meta::kVcaFine) b = m.at;
    }
    return {a, b};
  }
  bool markHot(uint32_t i) const noexcept {
    for (const int32_t part : {kTick0 + static_cast<int32_t>(i), kCorner0 + static_cast<int32_t>(i)})
      if (hovered_ == part || drag_ == part || edit_ == part) return true;
    // TX.vca-knob2: the partner of a marker dragged or typed as a pair.
    for (const int32_t part : {drag_, edit_})
      if (isMark(part) && dragPair_ && meta::vcaMarkPartner(markOf(part)) == i) return true;
    return false;
  }
  std::string markEditShown() const {
    const uint32_t i = markOf(edit_);
    std::string name = markName(i);
    if (dragPair_ && meta::vcaMarkPartner(i) < meta::kVcaMarks && law_.marks[meta::vcaMarkPartner(i)].shown)
      name = "\xC2\xB1" + name.substr(1);  // ±10: the pair
    return minusSign(name) + ": " + editShown();
  }
  // The state as a VcaParam-ordered array (meta::vcaMoveMark's input).
  std::array<double, meta::kVcaParams> stateNow() const {
    std::array<double, meta::kVcaParams> a{};
    for (size_t i = 0; i < meta::kVcaParams; ++i) a[i] = param(i);
    return a;
  }
  // Writes the markers' offsets that differ from the state (inside a begun gesture).
  void writeMarks(const std::array<double, meta::kVcaMarks>& offs) {
    for (uint32_t k = 0; k < meta::kVcaMarks; ++k)
      if (offs[k] != param(meta::kVcaMark0 + k)) target().set(meta::kVcaMark0 + k, offs[k]);
  }
  bool marksDiffer(const std::array<double, meta::kVcaMarks>& offs) const {
    for (uint32_t k = 0; k < meta::kVcaMarks; ++k)
      if (offs[k] != param(meta::kVcaMark0 + k)) return true;
    return false;
  }
  bool marksAtDefault() const {
    for (uint32_t i = 0; i < meta::kVcaMarks; ++i)
      if (param(meta::kVcaMark0 + i) != 0.0) return false;
    return true;
  }
  // The knob position under the pointer on the track (0 at 6 o'clock, clockwise), unwrapped to the nearest of `near`.
  static double trackP(Vec2 l, double near) {
    const double a = std::atan2(static_cast<double>(l.y - kC.y), static_cast<double>(l.x - kC.x));
    double p = (a - static_cast<double>(deg(kA0))) / (2.0 * std::numbers::pi);
    p -= std::floor(p);
    if (p - near > 0.5) p -= 1.0;
    if (near - p > 0.5) p += 1.0;
    return p;
  }

  void buildGlyphs() const {
    for (int i = 0; i < kPresets; ++i) {
      const Preset& pr = kPresetList[i];
      const meta::VcaLaw l = presetLaw(pr);
      const Rect r = chipRect(i);
      const auto k = static_cast<size_t>(i);
      const float x0 = r.x0 + 2.5f, w = kChipW - 5.0f, y0 = r.y1 - 2.0f, h = kChipDy1 - kChipDy0 - 4.0f;
      auto y = [&](double v) { return y0 - static_cast<float>((v - l.lo) / std::max(l.hi - l.lo, 1e-30)) * h; };
      auto& g = glyph_[k];
      g.clear();
      for (int q = 0; q < kGlyphN; ++q) {
        const double t = static_cast<double>(q) / (kGlyphN - 1);
        g.push_back({x0 + static_cast<float>(t) * w, y(meta::vcaValue(l, t))});
      }
      glyphZero_[k] = l.lo < 0.0 && l.hi > 0.0 ? y(0.0) : -1.0f;
      glyphUnit_[k] = l.lo < 1.0 && l.hi > 1.0 ? Vec2{x0 + static_cast<float>(meta::vcaPosition(l, 1.0)) * w, y(1.0)}
                                               : Vec2{-1.0f, -1.0f};
    }
  }

  // The state matches preset i: its curve, range and shape, every marker at its place (Log).
  bool presetActive(int i) const {
    const Preset& pr = kPresetList[i];
    if (law_.curve != pr.curve) return false;
    auto same = [](double a, double b) { return std::abs(a - b) <= 1e-6 * std::max(1.0, std::abs(b)); };
    if (!same(param(meta::kVcaMin), pr.min) || !same(param(meta::kVcaMax), pr.max)) return false;
    const double v = param(pr.curve == meta::VcaCurve::Log ? meta::kVcaZone : meta::kVcaBend);
    return same(v, pr.shape) && (pr.curve != meta::VcaCurve::Log || marksAtDefault());
  }

  Rect textRect(int32_t part) const noexcept { return part == kMinText ? minRect() : part == kMaxText ? maxRect() : kReadout; }
  std::string editShown() const { return editText_ + "|"; }

  // The pie wedge from angle a0 to a1 (radians, either order): three convex pieces (each ≤ 120°), so a sweep of up to
  // 360° stays convex per piece; the same primitive count for every span.
  void wedge(ui::FacePainter& p, float a0, float a1, Rgba col) const {
    const float lo = std::min(a0, a1), hi = std::max(a0, a1), step = (hi - lo) / 3.0f;
    const bool empty = hi - lo < 1e-4f;
    for (int h = 0; h < 3; ++h) {
      std::array<Vec2, kWedgeN + 2> pts{};
      pts[0] = kC;
      // The pieces overlap a hair (no seam between them).
      const float s = std::max(lo, lo + step * static_cast<float>(h) - 0.02f), e = lo + step * static_cast<float>(h + 1);
      for (int k = 0; k <= kWedgeN; ++k) pts[static_cast<size_t>(k) + 1] = polar(kWedgeR, s + (e - s) * static_cast<float>(k) / kWedgeN);
      p.convex(pts, empty ? 0 : col);
    }
  }

  static bool driven(const FaceEnv& env, std::string_view name) {
    for (const FaceJack& j : env.jacks)
      if (j.input && j.connected && j.name == name) return true;
    return false;
  }

  // ---- voices: each one's knob position (the tap) on the track (and on the open panel's curve) ----
  void paintVoices(ui::FacePainter& p, const ui::Theme& th, bool onCurve) {
    if (voiceCount_ == 0) return;
    auto at = [&](double pos) { return onCurve ? boxPoint(pos) : polar(kRingR, angleOf(pos)); };
    p.beginGlow();
    for (uint32_t v = 0; v < voiceCount_; ++v) {
      const FaceVoice& fv = voiceBuf_[v];
      for (uint32_t s = 0; s < fv.sampleCount; ++s) p.glow(at(fv.samples[s]), onCurve ? 2.6f : 3.2f, ui::withAlpha(th.face.glow, onCurve ? 30 : 34));
    }
    for (uint32_t v = 0; v < voiceCount_; ++v) p.glow(at(voiceBuf_[v].phase), onCurve ? 6.0f : 8.5f, ui::withAlpha(th.face.glow, onCurve ? 120 : 150));
    p.endGlow();
    for (uint32_t v = 0; v < voiceCount_; ++v) {
      const Vec2 d = at(voiceBuf_[v].phase);
      p.disc(d, 2.2f, th.face.signal);
      p.disc(d, 1.0f, ui::rgba(255, 255, 255));
    }
  }

  // ---- gestures ----
  bool down(const FacePointer& e) {
    if (e.button != ui::PointerButton::Left) return false;  // the host's menu
    const int32_t part = hitTest(e.local, e.pxPerUnit);
    if (edit_ >= 0 && part != edit_) commitEdit();  // a press elsewhere commits what was typed
    if (part < 0) return false;
    MetaTarget& tg = target();
    refresh();
    if (part == kChevron) {  // TX.orbit-ports: the floating panel (view state of the face: no edit)
      open_ = !open_;
      drag_ = kNone;
      return true;
    }
    if (part == kPanel) {
      drag_ = kNone;
      return true;
    }
    if (part >= kPreset0 && part < kPreset0 + kPresets) {
      if (tg.editable()) applyPreset(part - kPreset0);
      drag_ = kNone;
      return true;
    }
    if (part == edit_) return true;  // a click into the field being typed into
    if (e.clicks >= 2) {
      drag_ = kNone;
      if (!tg.editable()) return true;
      if (part == kValueText || part == kMinText || part == kMaxText) {
        edit_ = part;
        editText_.clear();
        return true;
      }
      if (isMark(part)) {  // TX.vca-knob2: type its place (% of the knob); Ctrl: this marker alone, else with its pair
        edit_ = part;
        dragPair_ = (e.mods & ui::mod::kCtrl) == 0;
        editText_.clear();
        return true;
      }
      if (part == kKnob) {  // ×1 (or the end nearest it)
        tg.begin("Value x1");
        tg.set(meta::kVcaValue, meta::vcaPosition(law_, std::clamp(1.0, law_.lo, law_.hi)));
        tg.end();
      } else if (part == kCurveBox) {
        const bool isLog = law_.curve == meta::VcaCurve::Log;
        const size_t pi = isLog ? meta::kVcaZone : meta::kVcaBend;
        tg.begin("Reset Curve");
        tg.set(pi, type().params[pi].def);
        if (isLog)  // TX.vca-knob: the markers too (each where the curve puts its value)
          for (uint32_t i = 0; i < meta::kVcaMarks; ++i)
            if (param(meta::kVcaMark0 + i) != 0.0) tg.set(meta::kVcaMark0 + i, 0.0);
        tg.end();
      }
      return true;
    }
    drag_ = part;
    begun_ = false;
    last_ = e.device;
    switch (part) {
      case kKnob: dragT_ = pos_; break;
      case kMinText: dragT_ = meta::vcaPosition(endScale(), param(meta::kVcaMin)); break;
      case kMaxText: dragT_ = meta::vcaPosition(endScale(), param(meta::kVcaMax)); break;
      case kCurveBox: {
        const bool isLog = law_.curve == meta::VcaCurve::Log;
        const size_t pi = isLog ? meta::kVcaZone : meta::kVcaBend;
        const double t = meta::valueToTravel(type().params[pi], param(pi));
        dragT_ = isLog ? t : 1.0 - t;  // Bend: up is toward −1 (the curve bulges up)
        break;
      }
      default:
        if (isMark(part)) {  // TX.vca-knob: the marker, from where it is; TX.vca-knob2: with its pair unless Ctrl
          dragT_ = law_.marks[markOf(part)].at;
          dragPair_ = (e.mods & ui::mod::kCtrl) == 0;
          dragState_ = stateNow();
          break;
        }
        drag_ = kNone;  // the readout: a double-click types; a drag does nothing
        break;
    }
    return true;
  }

  void move(const FacePointer& e) {
    if (isMark(drag_)) {  // the marker follows the pointer (round the track / along the box), its pair mirrored
      last_ = e.device;
      MetaTarget& tg = target();
      if (!tg.editable()) return;
      refresh();
      const uint32_t i = markOf(drag_);
      const double p = isTick(drag_) ? trackP(e.local, dragT_) : boxP(e.local.x);
      dragT_ = std::clamp(p, 0.0, 1.0);
      // From the drag's start state: the pair's travel is limited as a whole, never accumulated.
      const std::array<double, meta::kVcaMarks> offs = meta::vcaMoveMark(dragState_, curveSetting(), i, dragT_, dragPair_);
      if (!begun_) {
        if (!marksDiffer(offs)) return;  // nothing moved yet
        tg.begin(dragPair_ ? "Move Markers" : "Move Marker");
        begun_ = true;
      }
      writeMarks(offs);
      return;
    }
    const float dy = -(e.device.y - last_.y);
    last_ = e.device;
    MetaTarget& tg = target();
    if (dy == 0.0f || !tg.editable()) return;
    const double fine = (e.mods & ui::mod::kShift) ? 0.1 : 1.0;
    // Travel per device px: the control's extent on screen, never less than 150 logical px for the whole range.
    const float extent = drag_ == kKnob ? 2.0f * kRingR : (drag_ == kCurveBox ? 60.0f : 40.0f);
    const double span = std::max(extent * e.pxPerUnit, 150.0f * e.uiScale);
    dragT_ = std::clamp(dragT_ + dy / span * fine, 0.0, 1.0);
    if (!begun_) {
      tg.begin(drag_ == kKnob ? "Set Value" : drag_ == kMinText ? "Set Min" : drag_ == kMaxText ? "Set Max" : "Shape Curve");
      begun_ = true;
    }
    switch (drag_) {
      case kKnob: tg.set(meta::kVcaValue, dragT_); break;
      case kMinText: tg.set(meta::kVcaMin, std::min(roundSig(meta::vcaValue(endScale(), dragT_)), param(meta::kVcaMax))); break;
      case kMaxText: tg.set(meta::kVcaMax, std::max(roundSig(meta::vcaValue(endScale(), dragT_)), param(meta::kVcaMin))); break;
      case kCurveBox: {
        // Up widens the fine zone (Log) / bends the curve up, toward logarithmic (Bend: negative bend).
        const bool isLog = law_.curve == meta::VcaCurve::Log;
        const size_t pi = isLog ? meta::kVcaZone : meta::kVcaBend;
        tg.set(pi, meta::travelToValue(type().params[pi], isLog ? dragT_ : 1.0 - dragT_));
        break;
      }
      default: break;
    }
  }

  // TX.vca-knob2: the curve, the range, its shape and the markers (at their places), one undo step.
  void applyPreset(int i) {
    const Preset& pr = kPresetList[i];
    MetaTarget& tg = target();
    tg.begin("Curve Preset");
    if (law_.curve != pr.curve) tg.setSetting(meta::kVcaCurveSetting, static_cast<double>(pr.curve));
    if (param(meta::kVcaMin) != pr.min) tg.set(meta::kVcaMin, pr.min);
    if (param(meta::kVcaMax) != pr.max) tg.set(meta::kVcaMax, pr.max);
    const size_t shape = pr.curve == meta::VcaCurve::Log ? meta::kVcaZone : meta::kVcaBend;
    if (param(shape) != pr.shape) tg.set(shape, pr.shape);
    for (uint32_t m = 0; m < meta::kVcaMarks; ++m)
      if (param(meta::kVcaMark0 + m) != 0.0) tg.set(meta::kVcaMark0 + m, 0.0);
    tg.end();
  }

  void commitEdit() {
    const int32_t part = edit_;
    edit_ = kNone;
    MetaTarget& tg = target();
    if (part < 0 || !tg.editable()) return;
    if (isMark(part)) {
      commitMark(markOf(part));
      return;
    }
    const size_t pi = part == kValueText ? meta::kVcaValue : part == kMinText ? meta::kVcaMin : meta::kVcaMax;
    if (editText_.empty()) {
      if (part != kValueText) tg.reset(pi);
      return;
    }
    const std::optional<double> typed = parseTyped(editText_);
    if (!typed) {
      tg.notice("Not a number: " + editText_);
      return;
    }
    const double v = std::clamp(*typed, -meta::kVcaRange, meta::kVcaRange);
    tg.begin(part == kValueText ? "Type Value" : part == kMinText ? "Type Min" : "Type Max");
    if (part == kValueText) tg.set(pi, meta::vcaPosition(law_, std::clamp(v, law_.lo, law_.hi)));
    else if (part == kMinText) tg.set(pi, std::min(v, param(meta::kVcaMax)));
    else tg.set(pi, std::max(v, param(meta::kVcaMin)));
    tg.end();
  }

  // TX.vca-knob2: a marker's typed place (% of the knob's travel), with its pair unless Ctrl; empty: the default place.
  void commitMark(uint32_t i) {
    MetaTarget& tg = target();
    refresh();
    std::array<double, meta::kVcaMarks> offs{};
    for (uint32_t k = 0; k < meta::kVcaMarks; ++k) offs[k] = param(meta::kVcaMark0 + k);
    if (editText_.empty()) {
      offs[i] = 0.0;
      const uint32_t j = meta::vcaMarkPartner(i);
      if (dragPair_ && j < meta::kVcaMarks && law_.marks[j].shown) offs[j] = 0.0;
    } else {
      std::string t = editText_;
      if (t.back() == '%') t.pop_back();
      const std::optional<double> typed = parseTyped(t);
      if (!typed) {
        tg.notice("Not a number: " + editText_);
        return;
      }
      offs = meta::vcaMoveMark(stateNow(), curveSetting(), i, std::clamp(*typed / 100.0, 0.0, 1.0), dragPair_);
    }
    if (!marksDiffer(offs)) return;
    tg.begin(editText_.empty() ? "Reset Marker" : "Type Marker");
    writeMarks(offs);
    tg.end();
  }

  int32_t drag_ = kNone;
  bool begun_ = false;
  bool dragPair_ = true;  // TX.vca-knob2: the marker drag / type-in moves its pair too (no Ctrl)
  std::array<double, meta::kVcaParams> dragState_{};  // the state at the marker drag's start
  Vec2 last_{0.0f, 0.0f};
  double dragT_ = 0.0;
  int32_t edit_ = kNone;   // the field being typed into
  std::string editText_;
  bool open_ = false;      // TX.orbit-ports: the floating panel is open (the face's own view state)
  bool over_ = false;      // … over the face (the host has no room around it)

  // The state, re-derived when it changes.
  mutable std::array<double, meta::kVcaParams + 2> state_{};
  mutable meta::VcaLaw law_;
  mutable double pos_ = meta::kVcaDefaultPos, value_ = 1.0;
  mutable double logScale_ = 1.0, yLo_ = 0.0, yHi_ = 1.0;
  mutable std::vector<Vec2> curve_;
  mutable std::array<std::vector<Vec2>, kPresets> glyph_;
  mutable std::array<float, kPresets> glyphZero_{};  // the zero line's y in each chip (< 0: none)
  mutable std::array<Vec2, kPresets> glyphUnit_{};   // the ×1 dot in each chip (x < 0: none)
};

}  // namespace

std::unique_ptr<MetaFace> makeVcaFace(const meta::Type& t) { return std::make_unique<VcaFace>(t); }

}  // namespace pq::views
