// The Phase accumulator face (TMM.4, docs/plans/MM.md "Phase accumulator face"; inspired by SeptaBee's phase
// generator, our own design): a label-free circle. Four arcs are the controls — the left arc FREQUENCY (the Exp0 law,
// 0 Hz at the bottom), the top arc AMPLITUDE (0–4, ticks at the whole numbers), the right arc PITCH (TMM.15, SPEC §15
// 2026-11-01 "Phase accumulator pitch slider": ±4800 cents, 0 at 3 o'clock, filled from there, a tick per octave), the
// bottom arc PHASE offset (wraps) — and the centre is the output: a hub ringed by an orbit on which each sounding voice
// is a glowing dot at its newest sampled phase (0 at the top, clockwise), with the phases sampled during the last frame
// as a faint additive trail and a spoke from the hub. The output leaves through the gap between amplitude and pitch to
// the jack on the right edge. While transposed (or while the pitch is dragged, hovered or typed), the pitch shows as
// semitones + cents ("+7 st", "−12 st +14 ¢") in the bottom-right corner.
//
// Pitch gestures: drag up / down in whole semitones (keeping any cents offset; Ctrl: single cents, Shift: slower);
// after a press on it, Up / Down step ±1 semitone, Shift ±12 (an octave), Ctrl ±1 cent, and typing a number enters a
// value (cents; an `s` / `st` suffix: semitones; Enter commits, Esc cancels); double-click resets to 0.
//
// World units (120 × 120): centre (60, 64), body radius 48, control tracks at radius 39 (7 wide), orbit radius 23.
// Static layers: body, tracks (+ orbit), frame (+ ticks). Live: value arcs, handles, hover, hub, lead, readout, voices.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <string>

#include "faces.h"
#include "pq/meta/phase_acc.h"

namespace pq::views {

namespace {

using ui::Rgba;
using ui::Vec2;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float deg(float d) noexcept { return d * kPi / 180.0f; }

constexpr float kW = 120.0f, kH = 120.0f;
constexpr Vec2 kC{60.0f, 64.0f};
constexpr float kBodyR = 48.0f;
constexpr float kTrackR = 39.0f, kTrackW = 7.0f;
constexpr float kOrbitR = 23.0f;
constexpr float kHubR = 10.0f;
constexpr float kLeadY = 24.0f;      // the output jack's height (the host's main pin row)
constexpr float kLeadDeg = 307.0f;   // the output's lead leaves the hub through the gap between amplitude and pitch
constexpr Vec2 kReadout{95.0f, 115.5f};
constexpr float kReadoutPx = 7.0f;

// The four controls: arc start (travel 0) and sweep (signed degrees per full travel; angles clockwise from 3 o'clock).
enum Part : int32_t { kFreq = 0, kPhase = 1, kAmp = 2, kPitch = 3, kParts = 4 };
struct ArcSpec {
  float a0, sweep;  // degrees
};
constexpr ArcSpec kArcs[kParts] = {
    {150.0f, 90.0f},   // frequency: up the left side
    {130.0f, -80.0f},  // phase: along the bottom, left to right
    {252.0f, 40.0f},   // amplitude: over the top, left to right
    {36.0f, -72.0f},   // pitch: up the right side, 0 cents at 3 o'clock
};
// Hit sectors (degrees, [from, to)): pitch wraps through 0.
constexpr float kPhaseFrom = 43.0f, kFreqFrom = 140.0f, kAmpFrom = 246.0f, kGapFrom = 299.0f, kPitchFrom = 315.0f;

float arcAngle(int32_t part, float t) noexcept { return deg(kArcs[part].a0 + kArcs[part].sweep * t); }
Vec2 polar(float r, float a) noexcept { return {kC.x + r * std::cos(a), kC.y + r * std::sin(a)}; }

// The pitch as semitones + cents: the nearest semitone, the rest in cents ("+7 st", "−12 st +14 ¢", "+3.5 ¢", "0 st").
std::string minusSign(std::string s) {
  for (size_t at = s.find('-'); at != std::string::npos; at = s.find('-', at + 3)) s.replace(at, 1, "\xE2\x88\x92");
  return s;
}
std::string formatPitch(double cents) {
  const double st = std::round(cents / 100.0);
  double rest = cents - 100.0 * st;
  rest = std::round(rest * 100.0) / 100.0;  // hundredths of a cent
  char buf[64];
  char restText[32] = "";
  if (rest != 0.0) {
    if (rest == std::round(rest)) std::snprintf(restText, sizeof restText, "%+.0f \xC2\xA2", rest);
    else std::snprintf(restText, sizeof restText, "%+.2f \xC2\xA2", rest);
    // Trailing zeros of the hundredths ("+3.50" → "+3.5").
    std::string t = restText;
    const size_t dot = t.find('.');
    if (dot != std::string::npos) {
      size_t end = t.find(' ');
      while (end > dot + 2 && t[end - 1] == '0') t.erase(--end, 1);
      std::snprintf(restText, sizeof restText, "%s", t.c_str());
    }
  }
  if (st == 0.0) std::snprintf(buf, sizeof buf, "%s", rest == 0.0 ? "0 st" : restText);
  else if (rest == 0.0) std::snprintf(buf, sizeof buf, "%+.0f st", st);
  else std::snprintf(buf, sizeof buf, "%+.0f st %s", st, restText);
  return minusSign(buf);
}

// A typed pitch: a number of cents, or of semitones with an `s` / `st` suffix; a leading '-'. nullopt: not a pitch.
std::optional<double> parsePitch(std::string t) {
  double unit = 1.0;
  if (t.size() >= 2 && t.compare(t.size() - 2, 2, "st") == 0) {
    unit = 100.0;
    t.resize(t.size() - 2);
  } else if (!t.empty() && t.back() == 's') {
    unit = 100.0;
    t.pop_back();
  } else if (!t.empty() && t.back() == 'c') {
    t.pop_back();
  }
  if (t.empty() || t == "-" || t == "." || t == "-.") return std::nullopt;
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end != t.c_str() + t.size() || !std::isfinite(v)) return std::nullopt;
  return v * unit;
}

class PhaseAccFace final : public MetaFace {
public:
  explicit PhaseAccFace(const meta::Type& t) : MetaFace(t) {
    param_[kFreq] = t.paramIndex("freq");
    param_[kPhase] = t.paramIndex("phase");
    param_[kAmp] = t.paramIndex("amp");
    param_[kPitch] = t.paramIndex("pitch");
  }

  Vec2 worldSize() const noexcept override { return {kW, kH}; }

  uint32_t staticLayerCount() const noexcept override { return 3; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.body : (layer == 1 ? th.face.track : th.face.frame);
  }

  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) {
      p.disc(kC, kBodyR, w);
      return;
    }
    if (layer == 1) {
      for (int32_t k = 0; k < kParts; ++k) p.arc(kC, kTrackR, arcAngle(k, 0.0f), arcAngle(k, 1.0f), kTrackW, w);
      p.ring(kC, kOrbitR, 0.75f, w);
      return;
    }
    // Frame and ticks: the outer ring, ticks outside the tracks (amp: 1, 2, 3; frequency: 100 Hz, 1 kHz, 10 kHz;
    // phase: quarters; pitch: every octave, 0 longest).
    p.ring(kC, kBodyR - 0.75f, 1.25f, w);
    const float r0 = kTrackR + kTrackW * 0.5f + 1.25f, r1 = r0 + 2.5f;
    auto tick = [&](int32_t part, float t, float len) {
      const float a = arcAngle(part, t);
      p.line(polar(r0, a), polar(r0 + len, a), 1.0f, w);
    };
    for (const double hz : {100.0, 1000.0, 10000.0}) tick(kFreq, static_cast<float>(meta::phaseAccTravelFromFreq(hz)), r1 - r0);
    for (int i = 1; i <= 3; ++i) tick(kAmp, 0.25f * static_cast<float>(i), r1 - r0);
    for (int i = 1; i <= 3; ++i) tick(kPhase, 0.25f * static_cast<float>(i), i == 2 ? r1 - r0 : (r1 - r0) * 0.6f);
    const float octaves = static_cast<float>(meta::kPhaseAccPitchRange / 1200.0);
    for (int i = -3; i <= 3; ++i)
      tick(kPitch, 0.5f + 0.5f * static_cast<float>(i) / octaves, i == 0 ? (r1 - r0) * 1.4f : (r1 - r0) * 0.6f);
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    const ui::Theme& th = *env.theme;
    // Hover: the hovered control's track lights up.
    if (hovered_ >= 0 && hovered_ < kParts) p.arc(kC, kTrackR, arcAngle(hovered_, 0.0f), arcAngle(hovered_, 1.0f), kTrackW, th.face.hover);
    else p.arc(kC, kTrackR, 0.0f, 0.0f, kTrackW, th.face.hover);  // the same primitive count either way
    // Values: an arc from each track's origin (its start; the pitch's centre, 0 cents) to the value, and the handle
    // across the track.
    for (int32_t k = 0; k < kParts; ++k) {
      const float t = travel(k);
      const bool driven = drivenBy(env, k);
      const Rgba col = driven ? th.face.valueDriven : th.face.value;
      p.arc(kC, kTrackR, arcAngle(k, k == kPitch ? 0.5f : 0.0f), arcAngle(k, t), kTrackW - 2.0f, col);
      const float a = arcAngle(k, t);
      const float hr0 = kTrackR - kTrackW * 0.5f - 1.0f, hr1 = kTrackR + kTrackW * 0.5f + 1.0f;
      p.line(polar(hr0, a), polar(hr1, a), dragPart_ == k ? 2.5f : 2.0f, driven ? th.face.valueDriven : th.face.handle);
    }
    // The inputs: a faint lead from each input jack to the body (a wired one in the signal colour).
    for (const FaceJack& j : env.jacks) {
      if (!j.input) continue;
      const float dy = j.at.y - kC.y;
      const float edge = kC.x - std::sqrt(std::max(0.0f, kBodyR * kBodyR - dy * dy));
      const bool reach = j.visible && edge > 9.0f;
      p.line({8.0f, j.at.y}, {reach ? edge : 8.0f, j.at.y}, 1.0f,
             reach ? (j.connected ? ui::mulAlpha(th.face.signal, 0.45f) : ui::mulAlpha(th.face.jackRing, 0.3f)) : 0);
    }
    // The output: the lead from the hub out through the gap between amplitude and pitch to the jack's row, then to the
    // jack; the hub, its core lit by the amplitude.
    const float amp = static_cast<float>(value(kAmp, 1.0));
    const float lit = std::clamp(0.35f + 0.65f * std::min(amp, 1.0f), 0.0f, 1.0f);
    const float la = deg(kLeadDeg);
    const float leadR = (kLeadY - kC.y) / std::sin(la);  // where the lead's ray reaches the jack's row
    const Vec2 lead[] = {polar(kHubR, la), polar(leadR, la), {kW - 8.0f, kLeadY}};
    p.polyline(lead, 1.5f, ui::mulAlpha(th.face.signal, 0.25f + 0.45f * lit));
    p.beginGlow();  // the output's light: brighter with the amplitude
    p.glow(kC, kHubR * 2.2f, ui::withAlpha(th.face.glow, static_cast<uint32_t>(20.0f + 50.0f * lit)));
    p.endGlow();
    p.disc(kC, kHubR, th.face.hub);
    p.ring(kC, kHubR - 0.75f, 1.5f, ui::mulAlpha(th.face.signal, 0.45f + 0.55f * lit));
    p.disc(kC, 3.5f, ui::mulAlpha(th.face.signal, lit));
    if (env.selected) p.ring(kC, kBodyR - 0.75f, 1.75f, th.circuit.selected);
    else if (env.hovered && hovered_ == kFrame) p.ring(kC, kBodyR - 0.75f, 1.25f, th.circuit.hover);
    else p.ring(kC, kBodyR - 0.75f, 0.0f, th.circuit.hover);
    // The pitch readout: shown while transposed, or while the pitch is hovered, dragged or typed into.
    // One text primitive either way (hidden: a transparent one), so the count does not depend on the value.
    const double cents = value(kPitch, 0.0);
    if (typing_) p.text(kReadout, minusSign(editText_) + "|", kReadoutPx, th.face.handle);
    else if (cents != 0.0 || hovered_ == kPitch || dragPart_ == kPitch)
      p.text(kReadout, formatPitch(cents), kReadoutPx, drivenBy(env, kPitch) ? th.face.valueDriven : th.face.value);
    else p.text(kReadout, "", kReadoutPx, 0);
    paintVoices(p, th);
  }

  void liveRects(std::vector<ui::Rect>& out) const override {
    const float r = kOrbitR + 9.0f;
    out.push_back({kC.x - r, kC.y - r, kC.x + r, kC.y + r});
  }

  int32_t hitTest(Vec2 local, float pxPerUnit) const noexcept override {
    if (local.x < 0.0f || local.y < 0.0f || local.x > kW || local.y > kH) return kNone;
    const float dx = local.x - kC.x, dy = local.y - kC.y, r = std::sqrt(dx * dx + dy * dy);
    // A control's band reaches from the orbit's edge to past the body, widened at small zooms so a few device px
    // always hit it.
    const float slack = std::max(0.0f, 6.0f / std::max(pxPerUnit, 1e-6f) - 3.0f);
    if (r < kOrbitR + 6.0f || r > kBodyR + 2.0f + slack) return kFrame;
    float a = std::atan2(dy, dx) * 180.0f / kPi;  // −180 … 180, clockwise from +x
    if (a < 0.0f) a += 360.0f;
    if (a >= kPhaseFrom && a < kFreqFrom) return kPhase;
    if (a >= kFreqFrom && a < kAmpFrom) return kFreq;
    if (a >= kAmpFrom && a < kGapFrom) return kAmp;
    if (a >= kPitchFrom || a < kPhaseFrom) return kPitch;
    return kFrame;  // the gap the output's lead leaves through
  }

  bool onPointer(const FacePointer& e) override {
    using K = FacePointer::Kind;
    switch (e.kind) {
      case K::Down: {
        if (e.button != ui::PointerButton::Left) return false;  // no right-click use: the host's menu
        const int32_t part = hitTest(e.local, e.pxPerUnit);
        if (typing_) commitEdit();  // a press anywhere commits what was typed
        keyPart_ = part;
        if (part < 0 || param_[part] < 0) return false;
        if (e.clicks >= 2) {
          if (target().editable()) target().reset(static_cast<size_t>(param_[part]));
          dragPart_ = -1;
          return true;
        }
        dragPart_ = part;
        dragT_ = travel(part);
        startValue_ = value(part, 0.0);
        last_ = e.device;
        begun_ = false;
        return true;
      }
      case K::Move: {
        if (dragPart_ < 0) return false;
        const float dx = e.device.x - last_.x, dy = -(e.device.y - last_.y);
        const float d = dragPart_ == kPhase ? dx : (dragPart_ == kAmp ? dx + dy : dy);  // the top arc: right or up
        last_ = e.device;
        if (d == 0.0f || !target().editable()) return true;
        // Travel per device px: the arc's extent on screen, never less than 150 logical px for the whole range (the
        // pitch: 300, about 3 px per semitone).
        const float extent = dragPart_ == kPhase ? 50.0f : (dragPart_ == kAmp ? 40.0f : (dragPart_ == kPitch ? 46.0f : 56.0f));
        const float floorPx = dragPart_ == kPitch ? 300.0f : 150.0f;
        const float span = std::max(extent * e.pxPerUnit, floorPx * e.uiScale);
        dragT_ += d / span * ((e.mods & ui::mod::kShift) ? 0.1f : 1.0f);
        const meta::ParamSpec& spec = type().params[static_cast<size_t>(param_[dragPart_])];
        if (dragPart_ == kPhase) dragT_ -= std::floor(dragT_);
        else dragT_ = std::clamp(dragT_, 0.0f, 1.0f);
        double v = meta::travelToValue(spec, dragT_);
        if (dragPart_ == kAmp && (e.mods & ui::mod::kCtrl)) v = std::round(v);  // Ctrl: whole multiples
        if (dragPart_ == kPitch) {
          // Whole semitones from where the drag began (a cents offset stays); Ctrl: single cents.
          v = (e.mods & ui::mod::kCtrl) ? std::round(v) : startValue_ + 100.0 * std::round((v - startValue_) / 100.0);
          v = std::clamp(v, spec.min, spec.max);
        }
        if (!begun_) {
          static constexpr const char* kLabels[kParts] = {"Set Frequency", "Set Phase", "Set Amplitude", "Set Pitch"};
          target().begin(kLabels[dragPart_]);
          begun_ = true;
        }
        target().set(static_cast<size_t>(param_[dragPart_]), v);
        return true;
      }
      case K::Up: {
        if (dragPart_ < 0) return false;
        if (begun_) target().end();
        dragPart_ = -1;
        begun_ = false;
        return true;
      }
      case K::Wheel:
      case K::Leave: return false;
    }
    return false;
  }

  // After a press on the pitch arc: Up / Down step it (±1 semitone; Shift: ±12, an octave; Ctrl: ±1 cent), and typing
  // a number enters one (cents; `s` / `st`: semitones; Enter, Esc, Backspace).
  bool onKey(const ui::KeyEvent& e) override {
    const ui::Key k = e.key;
    if (typing_) {
      if (!e.down) return true;
      if ((e.mods & (ui::mod::kCtrl | ui::mod::kAlt | ui::mod::kSuper)) != 0) return false;
      if (k == ui::key::kEscape) {
        typing_ = false;
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
      if (typedChar(k, editText_)) return true;
      return ui::isPrintableKey(e);  // other printable keys are swallowed while typing (no shortcuts fire)
    }
    if (keyPart_ != kPitch || param_[kPitch] < 0 || dragPart_ >= 0) return false;
    if (!e.down) return false;
    if (k == ui::key::kUp || k == ui::key::kDown) {
      if ((e.mods & (ui::mod::kAlt | ui::mod::kSuper)) != 0) return false;
      if (!target().editable()) return true;
      const double step = (e.mods & ui::mod::kCtrl) ? 1.0 : ((e.mods & ui::mod::kShift) ? 1200.0 : 100.0);
      const meta::ParamSpec& spec = type().params[static_cast<size_t>(param_[kPitch])];
      const double v = std::clamp(value(kPitch, 0.0) + (k == ui::key::kUp ? step : -step), spec.min, spec.max);
      target().begin(k == ui::key::kUp ? "Pitch Up" : "Pitch Down");
      target().set(static_cast<size_t>(param_[kPitch]), v);
      target().end();
      return true;
    }
    if ((e.mods & (ui::mod::kCtrl | ui::mod::kAlt | ui::mod::kSuper)) != 0) return false;
    std::string first;
    if (target().editable() && ((k >= '0' && k <= '9') || k == '-' || k == '.') && typedChar(k, first)) {
      typing_ = true;
      editText_ = first;
      return true;
    }
    return false;
  }

  bool dragging() const noexcept override { return dragPart_ >= 0; }
  void cancelDrag() override {
    if (dragPart_ >= 0 && begun_) target().cancel();
    dragPart_ = -1;
    begun_ = false;
  }

  ui::Cursor cursor(int32_t part) const noexcept override {
    if (part == kPhase || part == kAmp) return ui::Cursor::ResizeEW;
    return part >= 0 ? ui::Cursor::ResizeNS : ui::Cursor::Inherit;
  }

  // TMM.10: each arc's band (the bounding box of the arc's sweep between the orbit and the body's edge).
  std::optional<ui::Rect> paramRect(size_t param) const noexcept override {
    for (int32_t part = 0; part < kParts; ++part) {
      if (param_[part] != static_cast<int32_t>(param)) continue;
      ui::Rect r{1e9f, 1e9f, -1e9f, -1e9f};
      for (int i = 0; i <= 16; ++i) {
        const float a = arcAngle(part, static_cast<float>(i) / 16.0f);
        for (const float rad : {kOrbitR + 6.0f, kBodyR + 2.0f}) {
          const Vec2 v = polar(rad, a);
          r = {std::min(r.x0, v.x), std::min(r.y0, v.y), std::max(r.x1, v.x), std::max(r.y1, v.y)};
        }
      }
      return r;
    }
    return std::nullopt;
  }

  void describe(int32_t part, std::string& title, std::string& body) const override {
    char buf[240];
    if (part < 0 || param_[part] < 0) {
      MetaFace::describe(part, title, body);
      body = "The phase of each voice: the note's frequency plus an offset, transposed, scaled. Drag the arcs; "
             "double-click the frame to open it to its atoms.";
      return;
    }
    const double v = target().value(static_cast<size_t>(param_[part]));
    if (part == kFreq) {
      title = "Frequency";
      std::snprintf(buf, sizeof buf, "+%.4g Hz added to the note. Drag up / down (Shift: fine); double-click resets to 0 Hz.", v);
    } else if (part == kPhase) {
      title = "Phase";
      std::snprintf(buf, sizeof buf, "Start phase %.3f of a cycle. Drag left / right (Shift: fine); double-click resets.", v);
    } else if (part == kAmp) {
      title = "Amplitude";
      std::snprintf(buf, sizeof buf, "x%.3f. Drag right / up (Shift: fine, Ctrl: whole numbers); double-click resets to 1.", v);
    } else {
      title = "Pitch";
      std::snprintf(buf, sizeof buf,
                    "%s (%+.2f cents, x%.6g in frequency). Drag up / down in semitones (Ctrl: cents, Shift: fine); Up / "
                    "Down: 1 semitone, Shift: an octave, Ctrl: 1 cent; type a value (cents, or st); double-click resets.",
                    formatPitch(v).c_str(), v, meta::phaseAccPitchRatio(v));
    }
    body = buf;
  }

private:
  double value(int32_t part, double fallback) const {
    return param_[part] >= 0 ? target().value(static_cast<size_t>(param_[part])) : fallback;
  }

  float travel(int32_t part) const {
    if (param_[part] < 0) return 0.0f;
    const meta::ParamSpec& spec = type().params[static_cast<size_t>(param_[part])];
    const double v = target().value(static_cast<size_t>(param_[part]));
    if (part == kPhase) return static_cast<float>(v - std::floor(v));
    return static_cast<float>(meta::valueToTravel(spec, v));
  }

  bool drivenBy(const FaceEnv& env, int32_t part) const {
    if (param_[part] < 0) return false;
    const std::string_view name = type().params[static_cast<size_t>(param_[part])].name;
    for (const FaceJack& j : env.jacks)
      if (j.input && j.connected && j.name == name) return true;
    return false;
  }

  // Appends a typed key to a pitch entry: digits, one '.', a leading '-', then an `s` / `st` / `c` suffix.
  static bool typedChar(ui::Key k, std::string& text) {
    if (text.size() >= 12) return false;
    const bool suffixed = !text.empty() && (text.back() == 's' || text.back() == 't' || text.back() == 'c');
    if (k >= '0' && k <= '9' && !suffixed) {
      text += static_cast<char>(k);
      return true;
    }
    if (k == '-' && text.empty()) {
      text += '-';
      return true;
    }
    if (k == '.' && !suffixed && text.find('.') == std::string::npos) {
      text += '.';
      return true;
    }
    if ((k == 's' || k == 'c') && !suffixed && !text.empty()) {
      text += static_cast<char>(k);
      return true;
    }
    if (k == 't' && !text.empty() && text.back() == 's') {
      text += 't';
      return true;
    }
    return false;
  }

  void commitEdit() {
    typing_ = false;
    MetaTarget& tg = target();
    if (param_[kPitch] < 0 || !tg.editable()) return;
    const size_t pi = static_cast<size_t>(param_[kPitch]);
    if (editText_.empty()) return;
    const std::optional<double> typed = parsePitch(editText_);
    if (!typed) {
      tg.notice("Not a pitch: " + editText_);
      return;
    }
    const meta::ParamSpec& spec = type().params[pi];
    tg.begin("Type Pitch");
    tg.set(pi, std::clamp(*typed, spec.min, spec.max));
    tg.end();
  }

  void paintVoices(ui::FacePainter& p, const ui::Theme& th) {
    if (voiceCount_ == 0) return;  // silent: nothing moves (pollVoices fills the voices, at ≤ 60 Hz)
    const Vec2 hub = kC;
    p.beginGlow();
    // The trail: every phase sampled during the last frame, faint (where the reads landed).
    for (uint32_t v = 0; v < voiceCount_; ++v) {
      const FaceVoice& fv = voiceBuf_[v];
      for (uint32_t s = 0; s < fv.sampleCount; ++s) p.glow(onOrbit(fv.samples[s]), 3.5f, ui::withAlpha(th.face.glow, 34));
    }
    for (uint32_t v = 0; v < voiceCount_; ++v) {
      const Vec2 d = onOrbit(voiceBuf_[v].phase);
      const float dx = d.x - hub.x, dy = d.y - hub.y, l = std::sqrt(dx * dx + dy * dy);
      const Vec2 from{hub.x + dx / l * (kHubR + 1.0f), hub.y + dy / l * (kHubR + 1.0f)};
      p.line(from, d, 1.0f, ui::withAlpha(th.face.glow, 70));
      p.glow(d, 9.0f, ui::withAlpha(th.face.glow, 150));
    }
    p.endGlow();
    for (uint32_t v = 0; v < voiceCount_; ++v) {
      const Vec2 d = onOrbit(voiceBuf_[v].phase);
      p.disc(d, 2.4f, th.face.signal);
      p.disc(d, 1.1f, ui::rgba(255, 255, 255));
    }
  }

  static Vec2 onOrbit(float phase) noexcept { return polar(kOrbitR, -kPi * 0.5f + 2.0f * kPi * phase); }

  int32_t param_[kParts] = {-1, -1, -1, -1};
  int32_t dragPart_ = -1;
  int32_t keyPart_ = kNone;  // the part the last press went to (keys act on the pitch only after a press on it)
  float dragT_ = 0.0f;
  double startValue_ = 0.0;
  Vec2 last_{0.0f, 0.0f};
  bool begun_ = false;
  bool typing_ = false;
  std::string editText_;
};

}  // namespace

std::unique_ptr<MetaFace> makePhaseAccFace(const meta::Type& t) { return std::make_unique<PhaseAccFace>(t); }

}  // namespace pq::views
