// GU.5b (docs/plans/GU.md "Phases" 5a): the rich elements the modules' faces are rebuilt from — registered element
// kinds like the generic ones (elements.cpp), nothing per module:
//
//   dial    a round 360° knob filling a circle (the VCA's face, TMM.14 / TX.vca-knob): min at 6 o'clock clockwise round
//           to max, a pie wedge from the value's zero (or the end nearest it) to the pointer, the value inside on a dark
//           pill (double-click: type one), and the MARKERS: every interior point of the bound control's curve is a tick
//           outside the track (the VCA preset's ±1 / ±10 / ±100, dsp::CurvePreset::Vca; the ±1 pair longer, the fine
//           zone between them a brighter band). A tick dragged round the track moves that point of the curve — with
//           the point of the opposite value mirrored (Ctrl: alone) —, one undo step, a curve edit inside its class (no
//           recompile); double-click a tick to type its place in % of the travel. The chevron under the hub opens a
//           FLOATING POPUP (Element::popupRect) under the panel: the range's ends (drag along the default VCA scale,
//           double-click to type), the curve on a symmetric-log scale with the same markers as corner points (drag
//           left / right; a drag of the box moves the ±1 pair apart or together) and six preset chips. Its bottom element
//           outline makes the panel round (the module's ports orbit the circle).
//   arc     a control on an arc of a circle (the Phase Accumulator's four controls): its track from `start` sweeping
//           `sweep` degrees (signed: clockwise positive) round (cx, cy), the value filled from `origin` (a share of the
//           travel) with a handle across the track, ticks at the values listed in `ticks` / `minor` (through the bound
//           control's curve), dragged up / down, left / right or both (`drag`; `dragPx` pointer px per whole travel,
//           Shift: fine; a Wrap curve wraps), double-click: default. Hit only in its band (`hitR0` … `hitR1`) between
//           the angles `hitFrom` … `hitTo` (degrees; may wrap through 0), so arcs share one rect. An optional readout
//           (`readout` corner, at readoutX / readoutY; `readoutWhen` moved: only while off its default or hot); a Cents
//           curve reads in semitones and cents ("+7 st", "−12 st +14 ¢").
//   orbit   the Phase Accumulator's centre: an orbit ring, the hub (lit by the control in slot 1, the amplitude), the
//           output's lead out through `lead` degrees to (leadX, leadY), and — when a voice reader exists for the signal
//           in slot 0 (PanelTarget::voices) — each sounding voice as a glowing dot at its phase on an orbit whose radius
//           and colour follow its frequency (TMM.17), with its trail and spoke.
//
// UI thread only.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "elements_internal.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float deg(float d) noexcept { return d * kPi / 180.0f; }
bool inside(const Rect& r, Vec2 p, float slack = 0.0f) noexcept {
  return p.x >= r.x0 - slack && p.x <= r.x1 + slack && p.y >= r.y0 - slack && p.y <= r.y1 + slack;
}
Vec2 centreOf(const Rect& r) noexcept { return {(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}; }

// A leading '-' as the typographic minus (U+2212); every '-' in `s` when `all`.
std::string minusSign(std::string s, bool all = false) {
  for (size_t at = s.find('-'); at != std::string::npos; at = all ? s.find('-', at + 3) : std::string::npos)
    s.replace(at, 1, "\xE2\x88\x92");
  return s;
}
// 3 significant digits, k for thousands: "1.00", "−0.50", "137", "1.68k", "−20.0k".
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
// A range end, compact: "−20k", "20k", "0", "1", "0.5", "150", "1.5k".
std::string formatEnd(double v) {
  char buf[32];
  if (std::abs(v) >= 999.5) std::snprintf(buf, sizeof buf, "%.3gk", v / 1000.0);
  else std::snprintf(buf, sizeof buf, "%.3g", v);
  std::string s = buf;
  if (s == "-0") s = "0";
  return minusSign(s);
}
double roundSig(double v) noexcept {
  if (v == 0.0 || !std::isfinite(v)) return 0.0;
  const double e = std::pow(10.0, std::floor(std::log10(std::abs(v))) - 2.0);
  return std::round(v / e) * e;
}
// A typed number: digits, one '.', a leading '-', an optional k (× 1000).
std::optional<double> parseTyped(std::string t) {
  if (t.empty()) return std::nullopt;
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
// The pitch as semitones + cents ("+7 st", "−12 st +14 ¢", "+3.5 ¢", "0 st").
std::string formatPitch(double cents) {
  const double st = std::round(cents / 100.0);
  double rest = std::round((cents - 100.0 * st) * 100.0) / 100.0;
  char restText[32] = "";
  if (rest != 0.0) {
    std::snprintf(restText, sizeof restText, rest == std::round(rest) ? "%+.0f \xC2\xA2" : "%+.2f \xC2\xA2", rest);
    std::string t = restText;
    if (const size_t dot = t.find('.'); dot != std::string::npos) {
      size_t end = t.find(' ');
      while (end > dot + 2 && t[end - 1] == '0') t.erase(--end, 1);
      std::snprintf(restText, sizeof restText, "%s", t.c_str());
    }
  }
  char buf[64];
  if (st == 0.0) std::snprintf(buf, sizeof buf, "%s", rest == 0.0 ? "0 st" : restText);
  else if (rest == 0.0) std::snprintf(buf, sizeof buf, "%+.0f st", st);
  else std::snprintf(buf, sizeof buf, "%+.0f st %s", st, restText);
  return minusSign(buf, true);
}
double pointValue(const dsp::Curve& c, uint32_t i) noexcept { return c.min + c.points[i].y * (c.max - c.min); }
// Values listed in a style field ("100|1000|10000").
std::vector<double> valuesOf(std::string_view s) {
  std::vector<double> v;
  for (const std::string& item : splitItems(s))
    if (const std::optional<double> x = parseTyped(item)) v.push_back(*x);
  return v;
}

// ==== dial ===========================================================================================================

// Layout in units of a 120 × 120 dial (the element scales it to its rect).
constexpr float kD = 120.0f;
constexpr Vec2 kO{60.0f, 60.0f};  // the body's centre
constexpr float kBodyR = 58.0f;
constexpr Vec2 kC{60.0f, 61.0f};  // the knob's centre
constexpr float kRingR = 46.5f, kRingW = 4.5f, kDialR = 41.5f, kWedgeR = 38.5f, kHubR = 5.0f;
constexpr float kA0 = 90.0f;  // degrees: position 0 at 6 o'clock, clockwise all the way round
constexpr float kReadoutY = 37.0f, kReadoutPx = 10.5f;
constexpr Rect kReadout{37.0f, 29.5f, 83.0f, 44.5f};
constexpr Vec2 kChev{60.0f, 85.0f};
constexpr Rect kChevRect{50.0f, 78.5f, 70.0f, 91.5f};
constexpr float kTickR0 = kRingR + kRingW * 0.5f + 1.0f, kTickLong = 4.5f, kTickShort = 2.6f;
constexpr float kTickHitR0 = kRingR + kRingW * 0.5f + 0.25f, kTickHitR1 = kTickR0 + kTickLong + 2.0f;
constexpr float kMarkHit = 3.5f;
constexpr float kPopH = 80.0f, kPopGap = 6.0f, kPopOverTop = 38.0f;
constexpr float kRangeDy = 17.0f, kRangePx = 8.0f;
constexpr float kBoxDy0 = 28.0f, kBoxDy1 = 58.0f, kBoxPad = 3.0f;
constexpr float kChipDy0 = 63.0f, kChipDy1 = 73.0f, kChipW = 16.0f, kChipX0 = 7.0f, kChipStep = 18.0f;
constexpr int kCurveN = 64, kWedgeN = 16, kGlyphN = 17, kPresets = 6;
constexpr double kMarkGap = 0.004;  // the closest two markers come (share of the travel)

struct DialPreset {
  const char* name;
  const char* body;
};
constexpr DialPreset kPresetInfo[kPresets] = {
    {"Default", "Bipolar, -20k ... +20k: -1 ... +1 fine around 12 o'clock, logarithmic to the ends (the VCA's law)."},
    {"0 -> 1", "Unipolar linear, 0 ... 1: a level or a gain (0 at the start, 1 at the end)."},
    {"-1 -> +1", "Bipolar linear, -1 ... +1: an attenuverter (0 at 12 o'clock, inverted to the left)."},
    {"x0 ... x2", "Linear gain, 0 ... 2: unity (x1) at 12 o'clock, up to +6 dB."},
    {"Fader", "0 ... 1 on an audio taper (exponential: fine near silence; about -18 dB at 12 o'clock)."},
    {"20 Hz -> 20 kHz", "A frequency, 20 Hz ... 20 kHz on an octave curve: equal ratios per turn."}};
dsp::Curve presetCurve(int i) {
  using P = dsp::CurvePreset;
  switch (i) {
    case 0: return dsp::curvePreset(P::Vca, -20000.0, 20000.0);
    case 1: return dsp::curvePreset(P::Linear, 0.0, 1.0);
    case 2: return dsp::curvePreset(P::Linear, -1.0, 1.0);
    case 3: return dsp::curvePreset(P::Linear, 0.0, 2.0);
    case 4: return dsp::curvePreset(P::Exponential, 0.0, 1.0, 4.0);
    default: return dsp::curvePreset(P::Octave, 20.0, 20000.0);
  }
}
const dsp::Curve& endScale() {
  static const dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Vca, -20000.0, 20000.0);
  return c;
}
double symLog(double a) noexcept { return a <= 1.0 ? a / std::numbers::ln10 : (1.0 + std::log(a)) / std::numbers::ln10; }

constexpr std::array<std::string_view, 1> kValueSlot = {"value"};
constexpr std::array<std::string_view, 2> kOrbitSlots = {"signal", "level"};

class Dial final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "value"},
  };

  // Parts (hit / hover / drag).
  enum Part : int32_t {
    kNoPart = -1, kKnob = 0, kValueText, kMinText, kMaxText, kCurveBox, kChevron, kPanel, kPreset0,
    kTick0 = kPreset0 + kPresets,   // marker i's tick: kTick0 + 2i, its corner point in the box: kTick0 + 2i + 1
  };

  bool interactive() const noexcept override { return true; }
  std::optional<FaceShape> outline() const noexcept override { return FaceShape{true, at(kO), kBodyR * k()}; }

  uint32_t staticLayerCount() const noexcept override { return 3; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.body : (layer == 1 ? th.face.track : th.face.frame);
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    const float s = k();
    if (layer == 0) {
      p.disc(at(kO), kBodyR * s, w);
      return;
    }
    if (layer == 1) {
      p.ring(at(kC), kRingR * s, kRingW * s, w);
      p.disc(at(kC), kDialR * s, ui::mulAlpha(w, 0.45f));
      return;
    }
    p.ring(at(kO), (kBodyR - 0.75f) * s, 1.25f * s, w);
    const float a = angleOf(0.0);
    p.line(polar(kRingR - kRingW * 0.5f - 1.0f, a), polar(kRingR + kRingW * 0.5f + 2.5f, a), 1.2f * s, ui::mulAlpha(w, 0.8f));
  }

  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    refresh(t);
    const float s = k();
    hot_ = env.hovered ? hot_ : kNoPart;
    const bool driven = binding() != nullptr && t.driven(*binding());
    const Rgba accent = driven ? th.face.valueDriven : colour(th, "colour", "value", th.face.value);
    p.ring(at(kC), kRingR * s, (kRingW + 1.5f) * s, hot_ == kKnob || dragPart_ == kKnob ? th.face.hover : 0);
    // The fine zone: a brighter band of the track between the ±1 markers.
    const auto [f0, f1] = fineSpan();
    p.arc(at(kC), kRingR * s, angleOf(f0), angleOf(f1), kRingW * s, f1 > f0 + 1e-6 ? ui::mulAlpha(th.face.handle, 0.22f) : 0);
    for (size_t i = 0; i < marks_.size(); ++i) {
      const Mark& m = marks_[i];
      const float a = angleOf(m.t);
      const bool hot = markHot(i);
      const float len = m.unit ? kTickLong : kTickShort;
      const Rgba col = hot ? th.face.value : ui::mulAlpha(th.face.handle, m.unit ? 0.85f : 0.5f);
      p.line(polar(kTickR0, a), polar(kTickR0 + len, a), (hot ? 1.6f : (m.unit ? 1.2f : 0.9f)) * s, col);
      p.disc(polar(kTickR0 + len + 0.9f, a), 1.3f * s, hot ? th.face.value : 0);
    }
    // The value's span on the track and the pie wedge, from the anchor (value 0, or the end nearest it).
    const dsp::Curve& c = curve_;
    const double anchor = c.min >= 0.0 ? 0.0 : (c.max <= 0.0 ? 1.0 : dsp::curvePosition(c, 0.0, 0.5));
    const float a0 = angleOf(anchor), a1 = angleOf(pos_);
    p.arc(at(kC), kRingR * s, std::min(a0, a1), std::max(a0, a1), (kRingW - 1.0f) * s, accent);
    wedge(p, a0, a1, ui::lerp(th.face.body, accent, 0.85f));
    const float pa = angleOf(pos_);
    const Rgba hand = driven ? th.face.valueDriven : th.face.handle;
    p.line(polar(kHubR + 1.0f, pa), polar(kRingR - 1.0f, pa), (dragPart_ == kKnob ? 2.4f : 1.9f) * s, hand);
    p.disc(polar(kRingR, pa), 2.9f * s, hand);
    p.disc(at(kC), kHubR * s, th.face.hub);
    p.ring(at(kC), (kHubR - 0.5f) * s, 1.0f * s, ui::mulAlpha(th.face.handle, 0.8f));
    // GU.5: a cable drives it — the modulation ring just outside the body's rim.
    if (driven) p.ring(at(kO), (kBodyR - 3.0f) * s, 1.5f * s, ui::mulAlpha(th.face.valueDriven, 0.8f));
    // The readout (a marker's place being typed shows here while the popup is closed).
    const bool markTyping = isMark(edit_) && !open_, typing = edit_ == kValueText || markTyping;
    const bool readoutHot = hot_ == kValueText || edit_ == kValueText;
    p.roundedRect(rectAt(kReadout), 4.0f * s, ui::mulAlpha(th.face.body, 0.82f));
    p.roundedRect(rectAt(kReadout), 4.0f * s, typing ? th.face.track : (readoutHot ? th.face.hover : 0));
    p.text(at({kC.x, kReadoutY}), edit_ == kValueText ? editShown() : markTyping ? markEditShown() : readout(t),
           (markTyping ? 7.5f : kReadoutPx) * s, typing ? th.face.handle : accent);
    // The chevron: down while the popup is closed, up while it is open.
    const bool chevHot = hot_ == kChevron;
    p.roundedRect(rectAt(kChevRect), 4.0f * s, ui::mulAlpha(th.face.body, 0.82f));
    p.roundedRect(rectAt(kChevRect), 4.0f * s, chevHot ? th.face.hover : 0);
    const float cy = open_ ? -1.5f : 1.5f;
    const Vec2 chev[3] = {at({kChev.x - 4.5f, kChev.y - cy}), at({kChev.x, kChev.y + cy}), at({kChev.x + 4.5f, kChev.y - cy})};
    p.polyline(chev, 1.4f * s, open_ ? th.face.value : ui::mulAlpha(th.face.handle, chevHot ? 0.95f : 0.65f));
  }

  // ---- the floating popup ----
  std::optional<Rect> popupRect() const noexcept override {
    if (!open_) return std::nullopt;
    return rectAt({0.0f, popTop(), kD, popTop() + kPopH});
  }
  void setPopupOverFace(bool on) override { over_ = on; }
  void paintPopup(ui::FacePainter& p, const ElementEnv& env) override {
    if (!open_) return;
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    refresh(t);
    const float s = k(), top = popTop();
    const Rect body = rectAt({0.5f, top + 0.5f, kD - 0.5f, top + kPopH - 0.5f});
    p.roundedRect({body.x0 + 1.5f * s, body.y0 + 2.5f * s, body.x1 + 1.5f * s, body.y1 + 2.5f * s}, 10.0f * s, ui::rgba(0, 0, 0, 90));
    p.roundedRect(body, 10.0f * s, th.face.frame);
    p.roundedRect({body.x0 + 1.25f * s, body.y0 + 1.25f * s, body.x1 - 1.25f * s, body.y1 - 1.25f * s}, 8.75f * s, th.face.body);
    const Rect caret = caretRect();
    p.roundedRect(caret, 3.0f * s, hot_ == kChevron ? th.face.hover : 0);
    const Vec2 cc = centreOf(caret);
    const Vec2 up[3] = {{cc.x - 4.5f * s, cc.y + 1.5f * s}, {cc.x, cc.y - 1.5f * s}, {cc.x + 4.5f * s, cc.y + 1.5f * s}};
    p.polyline(up, 1.4f * s, th.face.value);
    const bool driven = binding() != nullptr && t.driven(*binding());
    const Rgba accent = driven ? th.face.valueDriven : colour(th, "colour", "value", th.face.value);
    const Rgba hand = driven ? th.face.valueDriven : th.face.handle;
    // Range.
    for (const int32_t part : {static_cast<int32_t>(kMinText), static_cast<int32_t>(kMaxText)}) {
      const bool hot = hot_ == part || dragPart_ == part || edit_ == part;
      p.roundedRect(textRect(part), 3.0f * s, edit_ == part ? th.face.track : (hot ? th.face.hover : 0));
    }
    const float ry = at({0.0f, top + kRangeDy}).y;
    p.line({minRect().x1 + 2.0f * s, ry}, {maxRect().x0 - 2.0f * s, ry}, 1.0f * s, th.face.ink);
    p.text(centreOf(minRect()), edit_ == kMinText ? editShown() : formatEnd(curve_.min), kRangePx * s,
           ui::mulAlpha(th.face.handle, edit_ == kMinText ? 1.0f : 0.85f));
    p.text(centreOf(maxRect()), edit_ == kMaxText ? editShown() : formatEnd(curve_.max), kRangePx * s,
           ui::mulAlpha(th.face.handle, edit_ == kMaxText ? 1.0f : 0.85f));
    // The curve box.
    const Rect bx = box();
    p.roundedRect(bx, 5.0f * s, ui::mulAlpha(th.face.track, 0.6f));
    p.roundedRect(bx, 5.0f * s, hot_ == kCurveBox || dragPart_ == kCurveBox ? th.face.hover : 0);
    const bool zeroIn = curve_.min < 0.0 && curve_.max > 0.0;
    const float zy = boxY(curveY(0.0));
    const Vec2 zl[2] = {{bx.x0 + 2.0f * s, zy}, {bx.x1 - 2.0f * s, zy}};
    p.dashed(zl, 0.6f * s, 1.5f * s, 2.0f * s, zeroIn ? ui::mulAlpha(th.face.handle, 0.3f) : 0);
    const auto [f0, f1] = fineSpan();
    p.roundedRect({boxX(f0), bx.y0 + 2.0f * s, std::max(boxX(f1), boxX(f0) + 0.5f), bx.y1 - 2.0f * s}, 1.5f * s,
                  f1 > f0 + 1e-6 ? ui::mulAlpha(th.face.handle, 0.07f) : 0);
    p.fillUnder(drawn_, zeroIn ? zy : bx.y1 - kBoxPad * s, ui::mulAlpha(accent, 0.26f), ui::mulAlpha(accent, 0.04f));
    p.polyline(drawn_, (hot_ == kCurveBox || dragPart_ == kCurveBox ? 1.9f : 1.4f) * s, accent);
    const Vec2 here = boxPoint(pos_);
    const Vec2 drop[2] = {{here.x, bx.y0 + 2.0f * s}, {here.x, bx.y1 - 2.0f * s}};
    p.dashed(drop, 0.7f * s, 1.2f * s, 1.8f * s, ui::mulAlpha(th.face.handle, 0.45f));
    p.disc(here, 2.2f * s, hand);
    for (size_t i = 0; i < marks_.size(); ++i) {
      const Vec2 cpt = boxPoint(marks_[i].t);
      const bool unit = marks_[i].unit, hot = markHot(i);
      const float r = (hot ? 2.6f : (unit ? 1.9f : 1.4f)) * s;
      p.disc(cpt, r, th.face.body);
      p.ring(cpt, r, (hot ? 1.2f : 0.9f) * s, hot ? th.face.value : ui::mulAlpha(th.face.handle, unit ? 0.9f : 0.6f));
    }
    if (isMark(edit_)) {
      const Rect er{at({32.0f, 0.0f}).x, bx.y0 + 1.5f * s, at({88.0f, 0.0f}).x, bx.y0 + 10.5f * s};
      p.roundedRect(er, 3.0f * s, th.face.track);
      p.text(centreOf(er), markEditShown(), 7.0f * s, th.face.handle);
    }
    // Presets: each chip its curve (value against the knob, linear), a faint zero line, a dot at ×1.
    for (int i = 0; i < kPresets; ++i) {
      const Rect r = chipRect(i);
      const auto q = static_cast<size_t>(i);
      const bool on = curve_ == presets_[q], hot = hot_ == kPreset0 + i;
      const Rgba ink = on ? th.face.value : ui::mulAlpha(th.face.handle, hot ? 0.95f : 0.6f);
      p.roundedRect(r, 3.0f * s, ui::mulAlpha(th.face.track, 0.5f));
      p.roundedRect(r, 3.0f * s, on ? ui::mulAlpha(th.face.value, 0.3f) : (hot ? th.face.hover : 0));
      p.line({r.x0 + 2.0f * s, glyphZero_[q]}, {r.x1 - 2.0f * s, glyphZero_[q]}, 0.5f * s,
             glyphZero_[q] > 0.0f ? ui::mulAlpha(th.face.handle, 0.35f) : 0);
      p.polyline(glyph_[q], 1.0f * s, ink);
      p.disc(glyphUnit_[q], 1.1f * s, glyphUnit_[q].x > 0.0f ? ink : 0);
    }
  }

  void liveRects(std::vector<Rect>& out) const override {
    out.push_back({0.0f, 0.0f, desc_.w, desc_.h});
    if (const std::optional<Rect> pr = popupRect()) out.push_back(*pr);
  }

  bool hit(Vec2 l) const noexcept override {
    if (const std::optional<Rect> pr = popupRect(); pr && inside(*pr, l)) return true;
    // The knob and its markers' ticks; the rest of the rim is the block's (select, move), as the old face's frame.
    const float dx = l.x - at(kC).x, dy = l.y - at(kC).y, r = std::sqrt(dx * dx + dy * dy) / k();
    if (r <= kRingR + kRingW + 2.0f) return true;
    if (r < kTickHitR0 || r > kTickHitR1) return false;
    const float a = std::atan2(dy, dx);
    for (const Mark& m : marks_)
      if (std::abs(std::remainder(a - angleOf(m.t), 2.0f * kPi)) * r <= kMarkHit + 1.0f) return true;
    return false;
  }

  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    refresh(t);
    switch (e.kind) {
      case K::Down: return down(e, t);
      case K::Move:
        if (dragPart_ == kNoPart) {
          const int32_t h = partAt(e.local, e.pxPerUnit);
          const bool changed = h != hot_;
          hot_ = h;
          return changed;
        }
        move(e, t);
        return true;
      case K::Up:
        if (dragPart_ == kNoPart) return false;
        if (begun_) t.end();
        dragPart_ = kNoPart;
        begun_ = false;
        return true;
      case K::Wheel:
      case K::Leave: return false;
    }
    return false;
  }
  bool dragging() const noexcept override { return dragPart_ != kNoPart; }
  void cancelDrag(PanelTarget& t) override {
    if (dragPart_ != kNoPart && begun_) t.cancel();
    dragPart_ = kNoPart;
    begun_ = false;
  }
  void focusLost(PanelTarget& t) override {
    if (edit_ != kNoPart) commitEdit(t);
    open_ = false;
  }
  bool wantsText() const noexcept override { return false; }
  bool onKey(const ui::KeyEvent& e, PanelTarget& t) override {
    if (edit_ == kNoPart) {
      if (e.down && open_ && e.key == ui::key::kEscape && (e.mods & ui::mod::kChordMask) == 0) {
        open_ = false;
        return true;
      }
      return false;
    }
    if (!e.down) return true;
    if ((e.mods & (ui::mod::kCtrl | ui::mod::kAlt | ui::mod::kSuper)) != 0) return false;
    const ui::Key key = e.key;
    if (key == ui::key::kEscape) {
      edit_ = kNoPart;
      return true;
    }
    if (key == ui::key::kReturn) {
      commitEdit(t);
      return true;
    }
    if (key == ui::key::kBackspace || key == ui::key::kDelete) {
      if (!editText_.empty()) editText_.pop_back();
      return true;
    }
    const bool mark = isMark(edit_);
    if (editText_.size() < 12 && ((key >= '0' && key <= '9') || key == '.' || (!mark && (key == 'k' || key == 'K')) ||
                                  (!mark && key == '-' && editText_.empty()) || (mark && key == '%' && !editText_.empty()))) {
      editText_ += static_cast<char>(key == 'K' ? 'k' : key);
      return true;
    }
    return ui::isPrintableKey(e);
  }
  ui::Cursor cursor() const noexcept override {
    const int32_t part = dragPart_ != kNoPart ? dragPart_ : hot_;
    if ((part >= kPreset0 && part < kPreset0 + kPresets) || part == kChevron) return ui::Cursor::Pointer;
    if (part == kPanel) return ui::Cursor::Arrow;
    if (isMark(part)) return isCorner(part) ? ui::Cursor::ResizeEW : ui::Cursor::Move;
    if (part == kValueText && edit_ == kValueText) return ui::Cursor::Text;
    return ui::Cursor::ResizeNS;
  }
  std::optional<ControlAnchor> anchor() const override { return ControlAnchor{at(kC), (kRingR + 3.0f) * k(), kBodyR * k()}; }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    const int32_t part = dragPart_ != kNoPart ? dragPart_ : hot_;
    if (isMark(part) && static_cast<size_t>(markOf(part)) < marks_.size()) {
      const Mark& m = marks_[markOf(part)];
      title = "Marker " + minusSign(formatValue(m.value));
      char buf[200];
      std::snprintf(buf, sizeof buf, "At %.1f %% of the knob. Drag it %s (with its opposite, mirrored; Ctrl: alone); "
                                     "double-click to type its place in %%. A point of the knob's curve.",
                    100.0 * m.t, isTick(part) ? "along the track" : "left / right");
      body = buf;
      return;
    }
    if (part == kChevron) {
      title = open_ ? "Close Range and Curve" : "Range and Curve";
      body = open_ ? "Click (or Esc, or a click elsewhere) to close the panel." : "Click to open the knob's range and curve.";
      return;
    }
    if (part >= kPreset0 && part < kPreset0 + kPresets) {
      title = std::string("Preset: ") + kPresetInfo[part - kPreset0].name;
      body = std::string(kPresetInfo[part - kPreset0].body) + " Click: the range and the curve in one step.";
      return;
    }
    if (part == kMinText || part == kMaxText) {
      title = part == kMinText ? "Min" : "Max";
      body = "The knob's " + std::string(part == kMinText ? "left" : "right") + " end. Drag up / down; double-click to type.";
      return;
    }
    if (part == kCurveBox) {
      title = "Curve";
      body = "The value against the knob. Drag a marker's dot left / right; drag the box up / down to widen or narrow the "
             "fine zone between -1 and +1.";
      return;
    }
    ValueElement::describe(t, title, body);
  }

private:
  struct Mark {
    uint32_t point = 0;  // the curve's point
    double t = 0.0, value = 0.0;
    bool unit = false;   // ±1 (the fine zone's ends)
    int32_t partner = -1;  // the mark of the opposite value
  };
  static constexpr bool isMark(int32_t part) noexcept { return part >= kTick0; }
  static constexpr bool isTick(int32_t part) noexcept { return part >= kTick0 && (part - kTick0) % 2 == 0; }
  static constexpr bool isCorner(int32_t part) noexcept { return part >= kTick0 && (part - kTick0) % 2 == 1; }
  static constexpr int32_t markOf(int32_t part) noexcept { return (part - kTick0) / 2; }

  float k() const noexcept { return std::max(0.05f, std::min(desc_.w, desc_.h) / kD); }
  Vec2 at(Vec2 v) const noexcept {
    const float s = k();
    return {(desc_.w - kD * s) * 0.5f + v.x * s, (desc_.h - kD * s) * 0.5f + v.y * s};
  }
  Rect rectAt(const Rect& r) const noexcept {
    const Vec2 a = at({r.x0, r.y0}), b = at({r.x1, r.y1});
    return {a.x, a.y, b.x, b.y};
  }
  static float angleOf(double p) noexcept { return deg(kA0 + 360.0f * static_cast<float>(std::clamp(p, 0.0, 1.0))); }
  Vec2 polar(float r, float a) const noexcept {
    const Vec2 c = at(kC);
    return {c.x + r * k() * std::cos(a), c.y + r * k() * std::sin(a)};
  }
  float popTop() const noexcept { return over_ ? kPopOverTop : kD + kPopGap; }
  Rect minRect() const noexcept { return rectAt({8.0f, popTop() + kRangeDy - 6.0f, 40.0f, popTop() + kRangeDy + 6.0f}); }
  Rect maxRect() const noexcept { return rectAt({80.0f, popTop() + kRangeDy - 6.0f, 112.0f, popTop() + kRangeDy + 6.0f}); }
  Rect box() const noexcept { return rectAt({14.0f, popTop() + kBoxDy0, 106.0f, popTop() + kBoxDy1}); }
  Rect chipRect(int i) const noexcept {
    const float x = kChipX0 + kChipStep * static_cast<float>(i);
    return rectAt({x, popTop() + kChipDy0, x + kChipW, popTop() + kChipDy1});
  }
  Rect caretRect() const noexcept { return rectAt({50.0f, popTop() + 1.5f, 70.0f, popTop() + 9.5f}); }
  Rect textRect(int32_t part) const noexcept { return part == kMinText ? minRect() : part == kMaxText ? maxRect() : rectAt(kReadout); }

  // Re-derives the curve, the markers and what the popup draws when the binding's curve or position changed.
  void refresh(const PanelTarget& t) const {
    const doc::Binding* b = binding();
    if (b == nullptr) return;
    const BoundInfo& info = t.info(*b);
    const double pos = std::clamp(t.position(*b), 0.0, 1.0);
    const uint64_t key = dsp::curveHash(info.curve) ^ (over_ ? 0x9E37ull : 0ull);
    pos_ = pos;
    if (key == curveKey_ && !drawn_.empty()) return;
    curveKey_ = key;
    curve_ = info.curve;
    marks_.clear();
    for (uint32_t i = 1; i < curve_.segments; ++i) {
      Mark m;
      m.point = i;
      m.t = curve_.points[i].t;
      m.value = pointValue(curve_, i);
      m.unit = std::abs(std::abs(m.value) - 1.0) < 1e-9;
      marks_.push_back(m);
    }
    for (Mark& m : marks_)
      for (size_t j = 0; j < marks_.size(); ++j)
        if (m.value != 0.0 && std::abs(marks_[j].value + m.value) <= 1e-9 * std::max(1.0, std::abs(m.value))) m.partner = static_cast<int32_t>(j);
    symlog_ = !dsp::curveIsLinear(curve_);
    const double m = std::max({std::abs(curve_.min), std::abs(curve_.max), 1.0});
    logScale_ = symLog(m);
    yLo_ = shown(curve_.min);
    yHi_ = shown(curve_.max);
    drawn_.clear();
    for (int i = 0; i <= kCurveN; ++i) drawn_.push_back(boxPoint(static_cast<double>(i) / kCurveN));
    for (int i = 0; i < kPresets; ++i) {
      const auto q = static_cast<size_t>(i);
      presets_[q] = presetCurve(i);
      presets_[q].unit = curve_.unit;
      presets_[q].digits = curve_.digits;
      const dsp::Curve& l = presets_[q];
      const Rect r = chipRect(i);
      const float s = k();
      const float x0 = r.x0 + 2.5f * s, w = (kChipW - 5.0f) * s, y0 = r.y1 - 2.0f * s, h = (kChipDy1 - kChipDy0 - 4.0f) * s;
      auto y = [&](double v) { return y0 - static_cast<float>((v - l.min) / std::max(l.max - l.min, 1e-30)) * h; };
      glyph_[q].clear();
      for (int g = 0; g < kGlyphN; ++g) {
        const double u = static_cast<double>(g) / (kGlyphN - 1);
        glyph_[q].push_back({x0 + static_cast<float>(u) * w, y(dsp::evalCurve(l, u))});
      }
      glyphZero_[q] = l.min < 0.0 && l.max > 0.0 ? y(0.0) : -1.0f;
      glyphUnit_[q] = l.min < 1.0 && l.max > 1.0 ? Vec2{x0 + static_cast<float>(dsp::curvePosition(l, 1.0, 0.5)) * w, y(1.0)} : Vec2{-1.0f, -1.0f};
    }
  }
  double shown(double v) const {
    if (!symlog_) return v;
    return (v < 0.0 ? -1.0 : 1.0) * symLog(std::abs(v)) / logScale_;
  }
  double curveY(double v) const {
    const double span = yHi_ - yLo_;
    return span > 0.0 ? std::clamp((shown(v) - yLo_) / span, 0.0, 1.0) : 0.5;
  }
  float boxY(double u) const {
    const Rect b = box();
    const float pad = kBoxPad * k();
    return b.y1 - pad - static_cast<float>(u) * (b.y1 - b.y0 - 2.0f * pad);
  }
  float boxX(double p) const {
    const Rect b = box();
    const float pad = kBoxPad * k();
    return b.x0 + pad + static_cast<float>(p) * (b.x1 - b.x0 - 2.0f * pad);
  }
  double boxP(float x) const {
    const Rect b = box();
    const float pad = kBoxPad * k();
    return (x - b.x0 - pad) / std::max(b.x1 - b.x0 - 2.0f * pad, 1e-3f);
  }
  Vec2 boxPoint(double p) const { return {boxX(p), boxY(curveY(dsp::evalCurveUnits(curve_, p)))}; }
  std::pair<double, double> fineSpan() const {
    double a = 0.0, b = 0.0;
    bool any = false;
    for (const Mark& m : marks_)
      if (m.unit) {
        if (m.value < 0.0) a = m.t;
        else b = m.t;
        any = true;
      }
    if (!any) return {0.0, 0.0};
    for (const Mark& m : marks_)
      if (m.unit && m.partner < 0) {  // one of the pair is past an end of the range
        if (m.value < 0.0) b = 1.0;
        else a = 0.0;
      }
    return {a, b};
  }
  bool markHot(size_t i) const noexcept {
    const auto tick = kTick0 + 2 * static_cast<int32_t>(i);
    for (const int32_t part : {tick, tick + 1})
      if (hot_ == part || dragPart_ == part || edit_ == part) return true;
    for (const int32_t part : {dragPart_, edit_})
      if (isMark(part) && pair_ && static_cast<size_t>(markOf(part)) < marks_.size() && marks_[static_cast<size_t>(markOf(part))].partner == static_cast<int32_t>(i))
        return true;
    return false;
  }
  std::string readout(const PanelTarget& t) const {
    if (curve_.unit == dsp::CurveUnit::None && curve_.output == dsp::CurveOutput::None) return formatValue(dsp::evalCurve(curve_, pos_));
    return binding() != nullptr ? readoutText(t, pos_) : std::string();
  }
  std::string editShown() const { return editText_ + "|"; }
  std::string markEditShown() const {
    const size_t i = static_cast<size_t>(markOf(edit_));
    if (i >= marks_.size()) return editShown();
    std::string name = formatValue(marks_[i].value);
    if (pair_ && marks_[i].partner >= 0) name = "\xC2\xB1" + formatValue(std::abs(marks_[i].value));
    return name + ": " + editShown();
  }

  // Which part a point is (element-local).
  int32_t partAt(Vec2 l, float pxPerUnit) const {
    const float s = k();
    const float slack = std::max(0.0f, 5.0f / std::max(pxPerUnit * s, 1e-6f) - 2.0f) * s;
    if (const std::optional<Rect> pr = popupRect(); pr && inside(*pr, l)) {
      if (inside(caretRect(), l, std::min(slack, 2.0f * s))) return kChevron;
      for (int i = 0; i < kPresets; ++i)
        if (inside(chipRect(i), l, std::min(slack, 2.5f * s))) return kPreset0 + i;
      int32_t best = kNoPart;
      float bestD = (kMarkHit * s) + std::min(slack, 2.0f * s);
      for (size_t i = 0; i < marks_.size(); ++i) {
        const Vec2 c = boxPoint(marks_[i].t);
        const float d = std::hypot(l.x - c.x, l.y - c.y);
        if (d <= bestD) {
          bestD = d;
          best = kTick0 + 2 * static_cast<int32_t>(i) + 1;
        }
      }
      if (best != kNoPart) return best;
      if (inside(box(), l, std::min(slack, 2.0f * s))) return kCurveBox;
      if (inside(minRect(), l, std::min(slack, 2.0f * s))) return kMinText;
      if (inside(maxRect(), l, std::min(slack, 2.0f * s))) return kMaxText;
      return kPanel;
    }
    const Vec2 c = at(kC);
    const float dx = l.x - c.x, dy = l.y - c.y;
    if (inside(rectAt(kChevRect), l, std::min(slack, 2.0f * s))) return kChevron;
    if (inside(rectAt(kReadout), l, std::min(slack, 2.0f * s))) return kValueText;
    const float r = std::sqrt(dx * dx + dy * dy) / s;
    if (r >= kTickHitR0 && r <= kTickHitR1 + slack * 0.5f / s) {
      int32_t best = kNoPart;
      float bestD = kMarkHit + slack * 0.5f / s;
      const float a = std::atan2(dy, dx);
      for (size_t i = 0; i < marks_.size(); ++i) {
        const float d = std::abs(std::remainder(a - angleOf(marks_[i].t), 2.0f * kPi)) * r;
        if (d <= bestD) {
          bestD = d;
          best = kTick0 + 2 * static_cast<int32_t>(i);
        }
      }
      if (best != kNoPart) return best;
    }
    return kKnob;
  }
  // The knob position under the pointer round the track (0 at 6 o'clock, clockwise), unwrapped to the nearest of `near`.
  double trackP(Vec2 l, double near) const {
    const Vec2 c = at(kC);
    const double a = std::atan2(static_cast<double>(l.y - c.y), static_cast<double>(l.x - c.x));
    double p = (a - static_cast<double>(deg(kA0))) / (2.0 * std::numbers::pi);
    p -= std::floor(p);
    if (p - near > 0.5) p -= 1.0;
    if (near - p > 0.5) p += 1.0;
    return p;
  }

  // The curve with mark i at position p (its partner mirrored when `pair`), each kept a gap inside its neighbours.
  dsp::Curve movedMark(const dsp::Curve& from, size_t i, double p, bool pair) const {
    dsp::Curve c = from;
    const Mark& m = startMarks_[i];
    auto place = [&](uint32_t point, double t) {
      const double lo = c.points[point - 1].t + kMarkGap, hi = c.points[point + 1].t - kMarkGap;
      if (lo < hi) c.points[point].t = std::clamp(t, lo, hi);
    };
    place(m.point, p);
    if (pair && m.partner >= 0) {
      const Mark& o = startMarks_[static_cast<size_t>(m.partner)];
      place(o.point, o.t - (c.points[m.point].t - m.t));
    }
    if (!(c == from)) c.preset = dsp::CurvePreset::Custom;
    return c;
  }

  bool down(const FacePointer& e, PanelTarget& t) {
    if (e.button != ui::PointerButton::Left) return false;
    const int32_t part = partAt(e.local, e.pxPerUnit);
    hot_ = part;
    if (edit_ != kNoPart && part != edit_) commitEdit(t);
    const doc::Binding* b = binding();
    if (b == nullptr) return false;
    const bool ok = editable(t);
    if (part == kChevron) {
      open_ = !open_;
      return true;
    }
    if (part == kPanel) return true;
    if (part >= kPreset0 && part < kPreset0 + kPresets) {
      if (ok && !(curve_ == presets_[static_cast<size_t>(part - kPreset0)])) {
        t.begin(*b, "Curve Preset");
        t.setCurve(*b, presets_[static_cast<size_t>(part - kPreset0)]);
        t.end();
      }
      return true;
    }
    if (part == edit_) return true;
    if (e.clicks >= 2) {
      if (!ok) return true;
      if (part == kValueText || part == kMinText || part == kMaxText || isMark(part)) {
        edit_ = part;
        pair_ = (e.mods & ui::mod::kCtrl) == 0;
        editText_.clear();
        return true;
      }
      if (part == kKnob) t.setOnce(*b, dsp::curvePosition(curve_, std::clamp(1.0, std::min(curve_.min, curve_.max), std::max(curve_.min, curve_.max)), pos_), "Value x1");
      return true;
    }
    if (!ok) return true;
    dragPart_ = part;
    begun_ = false;
    last_ = e.device;
    startCurve_ = curve_;
    startMarks_ = marks_;
    pair_ = (e.mods & ui::mod::kCtrl) == 0;
    if (part == kKnob) dragT_ = pos_;
    else if (part == kMinText) dragT_ = dsp::curvePosition(endScale(), curve_.min, 0.5);
    else if (part == kMaxText) dragT_ = dsp::curvePosition(endScale(), curve_.max, 0.5);
    else if (part == kCurveBox) dragT_ = 0.0;
    else if (isMark(part)) dragT_ = static_cast<size_t>(markOf(part)) < marks_.size() ? marks_[static_cast<size_t>(markOf(part))].t : 0.0;
    else dragPart_ = kNoPart;  // the readout: a double-click types
    return true;
  }

  void begin(PanelTarget& t, const char* label) {
    if (begun_) return;
    t.begin(*binding(), label);
    begun_ = true;
  }

  void move(const FacePointer& e, PanelTarget& t) {
    const doc::Binding& b = *binding();
    if (isMark(dragPart_)) {
      const size_t i = static_cast<size_t>(markOf(dragPart_));
      if (i >= startMarks_.size()) return;
      const double p = isTick(dragPart_) ? trackP(e.local, dragT_) : boxP(e.local.x);
      dragT_ = std::clamp(p, 0.0, 1.0);
      const dsp::Curve c = movedMark(startCurve_, i, dragT_, pair_);
      if (c == curve_) return;
      begin(t, pair_ && startMarks_[i].partner >= 0 ? "Move Markers" : "Move Marker");
      t.setCurve(b, c);
      return;
    }
    const float dy = -(e.device.y - last_.y);
    last_ = e.device;
    if (dy == 0.0f) return;
    const double fine = (e.mods & ui::mod::kShift) ? 0.1 : 1.0;
    const float s = k();
    const float extent = dragPart_ == kKnob ? 2.0f * kRingR : (dragPart_ == kCurveBox ? 60.0f : 40.0f);
    const double span = std::max(extent * s * e.pxPerUnit, 150.0f * e.uiScale);
    if (dragPart_ == kCurveBox) {  // the ±1 pair apart (up) or together (down), mirrored
      dragT_ += dy / span * fine * 0.5;
      int32_t plus = -1;
      for (size_t i = 0; i < startMarks_.size(); ++i)
        if (startMarks_[i].unit && startMarks_[i].value > 0.0) plus = static_cast<int32_t>(i);
      if (plus < 0) return;
      const dsp::Curve c = movedMark(startCurve_, static_cast<size_t>(plus), startMarks_[static_cast<size_t>(plus)].t + dragT_, true);
      if (c == curve_) return;
      begin(t, "Shape Curve");
      t.setCurve(b, c);
      return;
    }
    dragT_ = std::clamp(dragT_ + dy / span * fine, 0.0, 1.0);
    if (dragPart_ == kKnob) {
      begin(t, "Set Value");
      t.setPosition(b, dragT_);
      return;
    }
    const double v = roundSig(dsp::evalCurve(endScale(), dragT_));
    const dsp::Curve c = withRange(dragPart_ == kMinText ? std::min(v, curve_.max) : curve_.min, dragPart_ == kMaxText ? std::max(v, curve_.min) : curve_.max);
    if (c == curve_) return;
    begin(t, dragPart_ == kMinText ? "Set Min" : "Set Max");
    t.setCurve(b, c);
  }

  // The curve over [lo, hi]: a preset rebuilt over the new range (its markers placed again), a custom curve's points
  // kept (their values scale with it).
  dsp::Curve withRange(double lo, double hi) const {
    dsp::Curve c = curve_;
    if (c.preset != dsp::CurvePreset::Custom && c.preset != dsp::CurvePreset::Linear) {
      dsp::Curve n = dsp::curvePreset(c.preset, lo, hi, c.presetArg);
      n.unit = c.unit, n.digits = c.digits, n.step = c.step, n.mode = c.mode, n.output = c.output;
      return n;
    }
    c.min = lo;
    c.max = hi;
    return c;
  }

  void commitEdit(PanelTarget& t) {
    const int32_t part = edit_;
    edit_ = kNoPart;
    const doc::Binding* b = binding();
    if (part == kNoPart || b == nullptr || !editable(t)) return;
    std::string text = editText_;
    if (isMark(part)) {
      const size_t i = static_cast<size_t>(markOf(part));
      if (i >= marks_.size() || text.empty()) return;
      if (text.back() == '%') text.pop_back();
      const std::optional<double> typed = parseTyped(text);
      if (!typed) {
        t.notice("Not a number: " + editText_);
        return;
      }
      startCurve_ = curve_;
      startMarks_ = marks_;
      const dsp::Curve c = movedMark(curve_, i, std::clamp(*typed / 100.0, 0.0, 1.0), pair_);
      if (c == curve_) return;
      t.begin(*b, "Type Marker");
      t.setCurve(*b, c);
      t.end();
      return;
    }
    if (text.empty()) return;
    const std::optional<double> typed = parseTyped(text);
    if (!typed) {
      t.notice("Not a number: " + editText_);
      return;
    }
    const double v = std::clamp(*typed, -20000.0, 20000.0);
    if (part == kValueText) {
      t.setOnce(*b, dsp::curvePosition(curve_, std::clamp(v, std::min(curve_.min, curve_.max), std::max(curve_.min, curve_.max)), pos_), "Type Value");
      return;
    }
    const dsp::Curve c = withRange(part == kMinText ? std::min(v, curve_.max) : curve_.min, part == kMaxText ? std::max(v, curve_.min) : curve_.max);
    if (c == curve_) return;
    t.begin(*b, part == kMinText ? "Type Min" : "Type Max");
    t.setCurve(*b, c);
    t.end();
  }

  // The pie wedge from angle a0 to a1: three convex pieces (each ≤ 120°), the same primitive count for every span.
  void wedge(ui::FacePainter& p, float a0, float a1, Rgba col) const {
    const float lo = std::min(a0, a1), hi = std::max(a0, a1), step = (hi - lo) / 3.0f;
    const bool empty = hi - lo < 1e-4f;
    const Vec2 c = at(kC);
    for (int h = 0; h < 3; ++h) {
      std::array<Vec2, kWedgeN + 2> pts{};
      pts[0] = c;
      const float s = std::max(lo, lo + step * static_cast<float>(h) - 0.02f), e = lo + step * static_cast<float>(h + 1);
      for (int q = 0; q <= kWedgeN; ++q) pts[static_cast<size_t>(q) + 1] = polar(kWedgeR, s + (e - s) * static_cast<float>(q) / kWedgeN);
      p.convex(pts, empty ? 0 : col);
    }
  }
  std::string hint() const override { return " Drag up / down (Shift: fine); double-click: x1; the chevron: range and curve."; }

  int32_t dragPart_ = kNoPart, hot_ = kNoPart, edit_ = kNoPart;
  bool begun_ = false, pair_ = true, open_ = false, over_ = false;
  Vec2 last_{0.0f, 0.0f};
  double dragT_ = 0.0;
  std::string editText_;
  dsp::Curve startCurve_;
  std::vector<Mark> startMarks_;
  // Derived from the bound curve (refresh).
  mutable uint64_t curveKey_ = 0;
  mutable dsp::Curve curve_;
  mutable double pos_ = 0.0, logScale_ = 1.0, yLo_ = 0.0, yHi_ = 1.0;
  mutable bool symlog_ = false;
  mutable std::vector<Mark> marks_;
  mutable std::vector<Vec2> drawn_;
  mutable std::array<dsp::Curve, kPresets> presets_;
  mutable std::array<std::vector<Vec2>, kPresets> glyph_;
  mutable std::array<float, kPresets> glyphZero_{};
  mutable std::array<Vec2, kPresets> glyphUnit_{};
};

// ==== arc ============================================================================================================

constexpr std::array<std::string_view, 3> kDragAxes = {"vertical", "horizontal", "diagonal"};
constexpr std::array<std::string_view, 2> kArcReadouts = {"none", "corner"};
constexpr std::array<std::string_view, 2> kReadoutWhen = {"always", "moved"};

class Arc final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"cx", StyleSpec::Type::Number, 60.0},
      {"cy", StyleSpec::Type::Number, 64.0},
      {"radius", StyleSpec::Type::Number, 39.0, {}, {}, 2.0, 1024.0},
      {"track", StyleSpec::Type::Number, 7.0, {}, {}, 0.5, 32.0},
      {"start", StyleSpec::Type::Number, 150.0, {}, {}, -720.0, 720.0},
      {"sweep", StyleSpec::Type::Number, 90.0, {}, {}, -360.0, 360.0},
      {"origin", StyleSpec::Type::Number, 0.0, {}, {}, 0.0, 1.0},
      {"ticks", StyleSpec::Type::Text, 0, ""},
      {"minor", StyleSpec::Type::Text, 0, ""},
      {"drag", StyleSpec::Type::Choice, 0, "vertical", kDragAxes},
      {"dragPx", StyleSpec::Type::Number, 150.0, {}, {}, 20.0, 2000.0},
      {"hitR0", StyleSpec::Type::Number, 29.0, {}, {}, 0.0, 1024.0},
      {"hitR1", StyleSpec::Type::Number, 50.0, {}, {}, 0.0, 1024.0},
      {"hitFrom", StyleSpec::Type::Number, 0.0, {}, {}, -720.0, 720.0},
      {"hitTo", StyleSpec::Type::Number, 0.0, {}, {}, -720.0, 720.0},
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"readout", StyleSpec::Type::Choice, 0, "none", kArcReadouts},
      {"readoutX", StyleSpec::Type::Number, 95.0},
      {"readoutY", StyleSpec::Type::Number, 115.5},
      {"readoutWhen", StyleSpec::Type::Choice, 0, "moved", kReadoutWhen},
  };

  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return th.face.track; }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    const G g = geom();
    p.arc(g.c, g.r, angle(0.0f), angle(1.0f), g.track, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const G g = geom();
    const bool hot = env.hovered || drag_;
    p.arc(g.c, g.r, angle(0.0f), angle(1.0f), g.track, hot ? th.face.hover : 0);
    // Ticks (through the bound curve: where those values are on the travel), outside the track in the frame colour.
    const doc::Binding* b = binding();
    const BoundInfo* info = b != nullptr ? &t.info(*b) : nullptr;
    if (info != nullptr && info->valid) {
      if (info->generation != tickGen_ || ticks_.empty()) buildTicks(*info);
      const float r0 = g.r + g.track * 0.5f + 1.25f;
      for (const Tick& k : ticks_) {
        const float a = angle(static_cast<float>(k.t));
        p.line(polar(g, r0, a), polar(g, r0 + k.len, a), k.width, th.face.frame);
      }
    }
    const double v = pos(t);
    const bool driven = b != nullptr && t.driven(*b);
    const Rgba col = driven ? th.face.valueDriven : colour(th, "colour", "value", th.face.value);
    const float o = static_cast<float>(std::clamp(num("origin", 0.0), 0.0, 1.0));
    const float a0 = angle(o), a1 = angle(static_cast<float>(v));
    p.arc(g.c, g.r, std::min(a0, a1), std::max(a0, a1), std::max(0.5f, g.track - 2.0f), col);
    const float hr0 = g.r - g.track * 0.5f - 1.0f, hr1 = g.r + g.track * 0.5f + 1.0f;
    p.line(polar(g, hr0, a1), polar(g, hr1, a1), drag_ ? 2.5f : 2.0f, driven ? th.face.valueDriven : th.face.handle);
    if (txt("readout", "none") == "corner" && b != nullptr && info != nullptr) {
      const bool moved = std::abs(v - defaultPos(*info)) > 1e-9 || hot;
      const bool show = txt("readoutWhen", "moved") == "always" || moved;
      const Vec2 at{static_cast<float>(num("readoutX", 95.0)), static_cast<float>(num("readoutY", 115.5))};
      p.text(at, show ? text(t, *info, v) : std::string(), 7.0f, show ? col : 0);
    }
  }
  bool hit(Vec2 l) const noexcept override {
    const G g = geom();
    const float dx = l.x - g.c.x, dy = l.y - g.c.y, r = std::sqrt(dx * dx + dy * dy);
    if (r < static_cast<float>(num("hitR0", 29.0)) || r > static_cast<float>(num("hitR1", 50.0))) return false;
    float a = std::atan2(dy, dx) * 180.0f / kPi;
    if (a < 0.0f) a += 360.0f;
    double from = num("hitFrom", 0.0), to = num("hitTo", 0.0);
    if (from == to) {  // the arc's own span, a few degrees wider
      const double s = num("start", 150.0), w = num("sweep", 90.0);
      from = std::min(s, s + w) - 4.0, to = std::max(s, s + w) + 4.0;
    }
    from -= 360.0 * std::floor(from / 360.0);
    to -= 360.0 * std::floor(to / 360.0);
    return from <= to ? a >= from && a < to : a >= from || a < to;
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    const doc::Binding* b = binding();
    if (e.kind == K::Down) {
      if (e.button != ui::PointerButton::Left || !editable(t)) return false;
      if (e.clicks >= 2) {
        t.reset(*b);
        return true;
      }
      startDrag(t, e, "Panel Arc");
      return true;
    }
    if (e.kind == K::Move && drag_) {
      const std::string_view ax = txt("drag", "vertical");
      const float dx = ax == "vertical" ? 0.0f : 1.0f, dy = ax == "horizontal" ? 0.0f : 1.0f;
      const float px = static_cast<float>(num("dragPx", 150.0)) * e.uiScale;
      const double fine = (e.mods & ui::mod::kShift) != 0 ? 0.1 : 1.0;
      const double d = (static_cast<double>(e.device.x) - startDev_.x) * dx - (static_cast<double>(e.device.y) - startDev_.y) * dy;
      double p = start_[0] + d * fine / std::max(4.0, static_cast<double>(px));
      if (t.info(*b).curve.mode == dsp::CurveMode::Wrap) p -= std::floor(p);
      t.setPosition(*b, quantise(t, std::clamp(p, 0.0, 1.0)));
      return true;
    }
    if (e.kind == K::Up && drag_) {
      endDrag(t);
      return true;
    }
    return false;
  }
  ui::Cursor cursor() const noexcept override {
    const std::string_view ax = txt("drag", "vertical");
    return ax == "horizontal" ? ui::Cursor::ResizeEW : ui::Cursor::ResizeNS;
  }
  std::optional<ControlAnchor> anchor() const override {
    const G g = geom();
    return ControlAnchor{polar(g, g.r, angle(static_cast<float>(lastPos_))), g.track * 0.5f, 0.0f};
  }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    lastPos_ = binding() != nullptr ? std::clamp(t.position(*binding()), 0.0, 1.0) : 0.0;
    ValueElement::describe(t, title, body);
  }

private:
  struct G {
    Vec2 c;
    float r, track;
  };
  struct Tick {
    double t;
    float len, width;
  };
  G geom() const noexcept {
    return {{static_cast<float>(num("cx", desc_.w * 0.5)), static_cast<float>(num("cy", desc_.h * 0.5))},
            static_cast<float>(std::clamp(num("radius", 39.0), 2.0, 1024.0)), static_cast<float>(std::clamp(num("track", 7.0), 0.5, 32.0))};
  }
  float angle(float t) const noexcept { return deg(static_cast<float>(num("start", 150.0)) + static_cast<float>(num("sweep", 90.0)) * t); }
  static Vec2 polar(const G& g, float r, float a) noexcept { return {g.c.x + r * std::cos(a), g.c.y + r * std::sin(a)}; }
  void buildTicks(const BoundInfo& info) const {
    ticks_.clear();
    tickGen_ = info.generation;
    const float len = 2.5f;
    for (const double v : valuesOf(txt("ticks", "")))
      ticks_.push_back({dsp::curvePosition(info.curve, v, 0.5), v == 0.0 && num("origin", 0.0) > 0.0 ? len * 1.4f : len, 1.0f});
    for (const double v : valuesOf(txt("minor", ""))) ticks_.push_back({dsp::curvePosition(info.curve, v, 0.5), len * 0.45f, 0.6f});
  }
  static double defaultPos(const BoundInfo& info) { return dsp::curvePosition(info.curve, 0.0, 0.5); }
  std::string text(const PanelTarget& t, const BoundInfo& info, double v) const {
    if (info.curve.unit == dsp::CurveUnit::Cents) return formatPitch(dsp::evalCurve(info.curve, v));
    return readoutText(t, v);
  }
  std::string hint() const override { return " Drag (Shift: fine), double-click: default."; }
  mutable std::vector<Tick> ticks_;
  mutable uint64_t tickGen_ = ~0ull;
  mutable double lastPos_ = 0.0;
};

// ==== orbit ==========================================================================================================

// TMM.17: a voice's colour by its frequency (a log-frequency spectrum: red below 100 Hz … yellow from 1 kHz up) and its
// orbit's scale (1.25× at 20 Hz down to 0.25× at 10 kHz).
struct HueStop {
  float log2Hz;
  Rgba colour;
};
constexpr HueStop kHues[] = {
    {4.9069f, ui::hex(0xE83434)}, {6.4094f, ui::hex(0xF0405A)}, {7.4919f, ui::hex(0xB054EC)}, {8.7814f, ui::hex(0x5078FF)},
    {9.2761f, ui::hex(0x28C4EC)}, {9.6795f, ui::hex(0x58DE78)}, {10.0362f, ui::hex(0xFCDC40)},
};
Rgba voiceColour(double hz) noexcept {
  constexpr size_t n = std::size(kHues);
  if (!(hz > 0.0)) return kHues[0].colour;
  const float l = static_cast<float>(std::log2(hz));
  if (l <= kHues[0].log2Hz) return kHues[0].colour;
  for (size_t i = 1; i < n; ++i)
    if (l < kHues[i].log2Hz) return ui::lerp(kHues[i - 1].colour, kHues[i].colour, (l - kHues[i - 1].log2Hz) / (kHues[i].log2Hz - kHues[i - 1].log2Hz));
  return kHues[n - 1].colour;
}
constexpr float kOrbitMax = 1.25f, kOrbitMin = 0.25f;
constexpr double kLowHz = 20.0, kHighHz = 10000.0;
float orbitScale(double hz) noexcept {
  if (!(hz > kLowHz)) return kOrbitMax;
  const double t = std::log2(hz / kLowHz) / std::log2(kHighHz / kLowHz);
  return std::max(kOrbitMin, kOrbitMax - (kOrbitMax - kOrbitMin) * static_cast<float>(t));
}

class Orbit final : public Element {
public:
  using Element::Element;
  static constexpr StyleSpec kStyle[] = {
      {"cx", StyleSpec::Type::Number, 60.0},
      {"cy", StyleSpec::Type::Number, 64.0},
      {"orbit", StyleSpec::Type::Number, 23.0, {}, {}, 2.0, 1024.0},
      {"hub", StyleSpec::Type::Number, 10.0, {}, {}, 1.0, 256.0},
      {"lead", StyleSpec::Type::Number, 307.0, {}, {}, -720.0, 720.0},
      {"leadX", StyleSpec::Type::Number, 112.0},
      {"leadY", StyleSpec::Type::Number, 24.0},
  };
  static constexpr uint32_t kMaxVoices = 16;

  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return th.face.track; }
  void paintStatic(ui::FacePainter& p, uint32_t) const override { p.ring(centre(), orbitR(), 0.75f, ui::rgba(255, 255, 255)); }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const Vec2 c = centre();
    const float hubR = static_cast<float>(num("hub", 10.0));
    // The hub lit by the level (slot 1: the amplitude), up to × 1.
    double level = 1.0;
    if (const doc::Binding* b = binding(1)) level = dsp::evalCurve(t.info(*b).curve, t.position(*b));
    const float lit = std::clamp(0.35f + 0.65f * std::min(static_cast<float>(level), 1.0f), 0.0f, 1.0f);
    const float la = deg(static_cast<float>(num("lead", 307.0)));
    const float leadY = static_cast<float>(num("leadY", 24.0)), leadX = static_cast<float>(num("leadX", 112.0));
    const float s = std::sin(la);
    const float leadR = std::abs(s) > 1e-3f ? (leadY - c.y) / s : hubR;
    const Vec2 lead[] = {{c.x + hubR * std::cos(la), c.y + hubR * s}, {c.x + leadR * std::cos(la), c.y + leadR * s}, {leadX, leadY}};
    p.polyline(lead, 1.5f, ui::mulAlpha(th.face.signal, 0.25f + 0.45f * lit));
    p.beginGlow();
    p.glow(c, hubR * 2.2f, ui::withAlpha(th.face.glow, static_cast<uint32_t>(20.0f + 50.0f * lit)));
    p.endGlow();
    p.disc(c, hubR, th.face.hub);
    p.ring(c, hubR - 0.75f, 1.5f, ui::mulAlpha(th.face.signal, 0.45f + 0.55f * lit));
    p.disc(c, 3.5f, ui::mulAlpha(th.face.signal, lit));
    paintVoices(p, hubR);
  }
  bool hit(Vec2) const noexcept override { return false; }  // a display: presses go to the arcs and the block
  void liveRects(std::vector<Rect>& out) const override {
    const Vec2 c = centre();
    const float r = orbitR() * kOrbitMax + 9.5f;
    out.push_back({c.x - r, c.y - r, c.x + r, c.y + r});
  }
  VoiceTapReader* voiceReader(PanelTarget& t) const override {
    const doc::Binding* b = binding(0);
    return b != nullptr && b->kind == doc::BindingKind::Signal ? t.voices(*b) : nullptr;
  }
  bool pollVoices(PanelTarget& t) override {
    VoiceTapReader* r = voiceReader(t);
    if (r == nullptr) {
      const bool had = count_ > 0;
      count_ = 0;
      return had;
    }
    if (r->serial() == serial_ && count_ == 0) return false;
    serial_ = r->serial();
    const uint32_t before = count_;
    count_ = r->read(std::span<FaceVoice>(voices_.data(), voices_.size()));
    return count_ > 0 || before > 0;
  }
  bool animating() const noexcept override { return count_ > 0; }

private:
  Vec2 centre() const noexcept { return {static_cast<float>(num("cx", 60.0)), static_cast<float>(num("cy", 64.0))}; }
  float orbitR() const noexcept { return static_cast<float>(std::clamp(num("orbit", 23.0), 2.0, 1024.0)); }
  Vec2 onOrbit(float phase, float r) const noexcept {
    const Vec2 c = centre();
    const float a = -kPi * 0.5f + 2.0f * kPi * phase;
    return {c.x + r * std::cos(a), c.y + r * std::sin(a)};
  }
  // TMM.17: each voice in the colour of the frequency it plays, on an orbit whose radius shrinks as it rises: its trail,
  // a spoke and its dot. Additive, so a chord's voices overlay.
  void paintVoices(ui::FacePainter& p, float hubR) {
    if (count_ == 0) return;
    const Vec2 hub = centre();
    std::array<Rgba, kMaxVoices> col{};
    std::array<float, kMaxVoices> rad{};
    for (uint32_t v = 0; v < count_; ++v) {
      const FaceVoice& fv = voices_[v];
      const double inc = static_cast<double>(fv.increment);
      const double hz = fv.rate > 0.0f ? std::fabs(inc - std::round(inc)) * static_cast<double>(fv.rate) : 0.0;
      col[v] = voiceColour(hz);
      rad[v] = orbitR() * orbitScale(hz);
    }
    p.beginGlow();
    for (uint32_t v = 0; v < count_; ++v)
      for (uint32_t s = 0; s < voices_[v].sampleCount; ++s) p.glow(onOrbit(voices_[v].samples[s], rad[v]), 3.5f, ui::withAlpha(col[v], 34));
    for (uint32_t v = 0; v < count_; ++v) {
      const Vec2 d = onOrbit(voices_[v].phase, rad[v]);
      const float dx = d.x - hub.x, dy = d.y - hub.y, l = std::sqrt(dx * dx + dy * dy);
      const float q = l > hubR + 1.0f ? (hubR + 1.0f) / l : 1.0f;
      p.line({hub.x + dx * q, hub.y + dy * q}, d, 1.0f, ui::withAlpha(col[v], 70));
      p.glow(d, 9.0f, ui::withAlpha(col[v], 150));
    }
    p.endGlow();
    for (uint32_t v = 0; v < count_; ++v) {
      const Vec2 d = onOrbit(voices_[v].phase, rad[v]);
      p.disc(d, 2.4f, col[v]);
      p.disc(d, 1.1f, ui::rgba(255, 255, 255));
    }
  }
  std::array<FaceVoice, kMaxVoices> voices_{};
  uint32_t count_ = 0;
  uint64_t serial_ = ~0ull;
};

template <class T>
std::unique_ptr<Element> makeRich(const doc::PanelElement& d) {
  return std::make_unique<T>(d);
}

}  // namespace

void registerRichElements(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                      float, float, ElementFactory)) {
  reg("dial", "Dial", Dial::kStyle, kValueSlot, 120, 120, &makeRich<Dial>);
  reg("arc", "Arc", Arc::kStyle, kValueSlot, 120, 120, &makeRich<Arc>);
  reg("orbit", "Phase Orbit", Orbit::kStyle, kOrbitSlots, 120, 120, &makeRich<Orbit>);
}

}  // namespace pq::views::panel
