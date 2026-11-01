// The Phase accumulator face (TMM.4, docs/plans/MM.md "Phase accumulator face"; inspired by SeptaBee's phase
// generator, our own design): a label-free circle. Three arcs are the controls — the left arc FREQUENCY (the Exp0 law,
// 0 Hz at the bottom), the right arc AMPLITUDE (0–4, ticks at the whole numbers), the bottom arc PHASE offset (wraps) —
// and the centre is the output: a hub ringed by an orbit on which each sounding voice is a glowing dot at its newest
// sampled phase (0 at the top, clockwise), with the phases sampled during the last frame as a faint additive trail
// and a spoke from the hub. The output leaves through the top gap to the jack on the right edge.
//
// World units (120 × 120): centre (60, 64), body radius 48, control tracks at radius 39 (7 wide), orbit radius 23.
// Static layers: body, tracks (+ orbit), frame (+ ticks). Live: value arcs, handles, hover, hub, lead, voices.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
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
constexpr float kLeadY = 24.0f;  // the output jack's height (the host's main pin row)

// The three controls: arc start (travel 0) and sweep (signed degrees per full travel).
enum Part : int32_t { kFreq = 0, kPhase = 1, kAmp = 2, kParts = 3 };
struct ArcSpec {
  float a0, sweep;  // degrees
};
constexpr ArcSpec kArcs[kParts] = {
    {150.0f, 100.0f},  // frequency: up the left side
    {130.0f, -80.0f},  // phase: along the bottom, left to right
    {30.0f, -100.0f},  // amplitude: up the right side
};
float arcAngle(int32_t part, float t) noexcept { return deg(kArcs[part].a0 + kArcs[part].sweep * t); }
Vec2 polar(float r, float a) noexcept { return {kC.x + r * std::cos(a), kC.y + r * std::sin(a)}; }

class PhaseAccFace final : public MetaFace {
public:
  explicit PhaseAccFace(const meta::Type& t) : MetaFace(t) {
    param_[kFreq] = t.paramIndex("freq");
    param_[kPhase] = t.paramIndex("phase");
    param_[kAmp] = t.paramIndex("amp");
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
    // phase: quarters), and the end stops of every arc.
    p.ring(kC, kBodyR - 0.75f, 1.25f, w);
    const float r0 = kTrackR + kTrackW * 0.5f + 1.25f, r1 = r0 + 2.5f;
    auto tick = [&](int32_t part, float t, float len) {
      const float a = arcAngle(part, t);
      p.line(polar(r0, a), polar(r0 + len, a), 1.0f, w);
    };
    for (const double hz : {100.0, 1000.0, 10000.0}) tick(kFreq, static_cast<float>(meta::phaseAccTravelFromFreq(hz)), r1 - r0);
    for (int i = 1; i <= 3; ++i) tick(kAmp, 0.25f * static_cast<float>(i), r1 - r0);
    for (int i = 1; i <= 3; ++i) tick(kPhase, 0.25f * static_cast<float>(i), i == 2 ? r1 - r0 : (r1 - r0) * 0.6f);
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    const ui::Theme& th = *env.theme;
    const MetaTarget& tg = target();
    // Hover: the hovered control's track lights up.
    if (hovered_ >= 0 && hovered_ < kParts) p.arc(kC, kTrackR, arcAngle(hovered_, 0.0f), arcAngle(hovered_, 1.0f), kTrackW, th.face.hover);
    else p.arc(kC, kTrackR, 0.0f, 0.0f, kTrackW, th.face.hover);  // the same primitive count either way
    // Values: an arc from the start of each track to the value, and the handle across the track.
    for (int32_t k = 0; k < kParts; ++k) {
      const float t = travel(k);
      const bool driven = drivenBy(env, k);
      const Rgba col = driven ? th.face.valueDriven : th.face.value;
      p.arc(kC, kTrackR, arcAngle(k, 0.0f), arcAngle(k, t), kTrackW - 2.0f, col);
      const float a = arcAngle(k, t);
      const float hr0 = kTrackR - kTrackW * 0.5f - 1.0f, hr1 = kTrackR + kTrackW * 0.5f + 1.0f;
      p.line(polar(hr0, a), polar(hr1, a), dragPart_ == k ? 2.5f : 2.0f, driven ? th.face.valueDriven : th.face.handle);
    }
    (void)tg;
    // The inputs: a faint lead from each input jack to the body (a wired one in the signal colour).
    for (const FaceJack& j : env.jacks) {
      if (!j.input) continue;
      const float dy = j.at.y - kC.y;
      const float edge = kC.x - std::sqrt(std::max(0.0f, kBodyR * kBodyR - dy * dy));
      const bool reach = j.visible && edge > 9.0f;
      p.line({8.0f, j.at.y}, {reach ? edge : 8.0f, j.at.y}, 1.0f,
             reach ? (j.connected ? ui::mulAlpha(th.face.signal, 0.45f) : ui::mulAlpha(th.face.jackRing, 0.3f)) : 0);
    }
    // The output: the lead from the hub up through the top gap to the jack, the hub, its core lit by the amplitude.
    const float amp = static_cast<float>(param_[kAmp] >= 0 ? target().value(static_cast<size_t>(param_[kAmp])) : 1.0);
    const float lit = std::clamp(0.35f + 0.65f * std::min(amp, 1.0f), 0.0f, 1.0f);
    const Vec2 lead[] = {{kC.x, kC.y - kHubR}, {kC.x, kLeadY + 6.0f}, {kC.x + 6.0f, kLeadY}, {kW - 8.0f, kLeadY}};
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
    if (a >= 40.0f && a < 140.0f) return kPhase;
    if (a >= 140.0f && a < 260.0f) return kFreq;
    if (a >= 280.0f || a < 40.0f) return kAmp;
    return kFrame;  // the top gap (the output's lead)
  }

  bool onPointer(const FacePointer& e) override {
    using K = FacePointer::Kind;
    switch (e.kind) {
      case K::Down: {
        if (e.button != ui::PointerButton::Left) return false;  // no right-click use: the host's menu
        const int32_t part = hitTest(e.local, e.pxPerUnit);
        if (part < 0 || param_[part] < 0) return false;
        if (e.clicks >= 2) {
          if (target().editable()) target().reset(static_cast<size_t>(param_[part]));
          dragPart_ = -1;
          return true;
        }
        dragPart_ = part;
        dragT_ = travel(part);
        last_ = e.device;
        begun_ = false;
        return true;
      }
      case K::Move: {
        if (dragPart_ < 0) return false;
        const float d = dragPart_ == kPhase ? e.device.x - last_.x : -(e.device.y - last_.y);
        last_ = e.device;
        if (d == 0.0f || !target().editable()) return true;
        // Travel per device px: the arc's extent on screen, never less than 150 logical px for the whole range.
        const float extent = dragPart_ == kPhase ? 50.0f : 56.0f;
        const float span = std::max(extent * e.pxPerUnit, 150.0f * e.uiScale);
        dragT_ += d / span * ((e.mods & ui::mod::kShift) ? 0.1f : 1.0f);
        const meta::ParamSpec& spec = type().params[static_cast<size_t>(param_[dragPart_])];
        if (dragPart_ == kPhase) dragT_ -= std::floor(dragT_);
        else dragT_ = std::clamp(dragT_, 0.0f, 1.0f);
        double v = meta::travelToValue(spec, dragT_);
        if (dragPart_ == kAmp && (e.mods & ui::mod::kCtrl)) v = std::round(v);  // Ctrl: whole multiples
        if (!begun_) {
          target().begin(dragPart_ == kFreq ? "Set Frequency" : (dragPart_ == kPhase ? "Set Phase" : "Set Amplitude"));
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

  bool dragging() const noexcept override { return dragPart_ >= 0; }
  void cancelDrag() override {
    if (dragPart_ >= 0 && begun_) target().cancel();
    dragPart_ = -1;
    begun_ = false;
  }

  ui::Cursor cursor(int32_t part) const noexcept override {
    if (part == kPhase) return ui::Cursor::ResizeEW;
    return part >= 0 ? ui::Cursor::ResizeNS : ui::Cursor::Inherit;
  }

  void describe(int32_t part, std::string& title, std::string& body) const override {
    char buf[160];
    if (part < 0 || param_[part] < 0) {
      MetaFace::describe(part, title, body);
      body = "The phase of each voice: the note's frequency plus an offset, scaled. Drag the arcs; double-click the "
             "frame to open it to its atoms.";
      return;
    }
    const double v = target().value(static_cast<size_t>(param_[part]));
    if (part == kFreq) {
      title = "Frequency";
      std::snprintf(buf, sizeof buf, "+%.4g Hz added to the note. Drag up / down (Shift: fine); double-click resets to 0 Hz.", v);
    } else if (part == kPhase) {
      title = "Phase";
      std::snprintf(buf, sizeof buf, "Start phase %.3f of a cycle. Drag left / right (Shift: fine); double-click resets.", v);
    } else {
      title = "Amplitude";
      std::snprintf(buf, sizeof buf, "x%.3f. Drag up / down (Shift: fine, Ctrl: whole numbers); double-click resets to 1.", v);
    }
    body = buf;
  }

private:
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

  int32_t param_[kParts] = {-1, -1, -1};
  int32_t dragPart_ = -1;
  float dragT_ = 0.0f;
  Vec2 last_{0.0f, 0.0f};
  bool begun_ = false;
};

}  // namespace

std::unique_ptr<MetaFace> makePhaseAccFace(const meta::Type& t) { return std::make_unique<PhaseAccFace>(t); }

}  // namespace pq::views
