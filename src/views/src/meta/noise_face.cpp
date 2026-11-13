// The Noise engine's face (NZ.1, docs/plans/NZ.md "Panel"): its display, the panel's rich `noise` element. Label-free,
// the Envelope's visual family:
//
//   trace   a scrolling trace of the output (before the Range: −1 at the bottom, +1 at the top) of the first sounding
//           voice, newest at the right: each poll (≤ 60 Hz) appends the values the voice tap sampled since the last one
//           (up to four, evenly picked), so a slow random walk draws its shape and audio-rate noise a dense band
//   voices  every sounding voice a glowing dot at the right edge at its newest value (they fade in and out over 80 ms)
//   slope   a small line right of the trace tilted by Colour (−6 … +6 dB/oct over its width) and tinted by it: brown,
//           pink, white, blue, violet (noiseColourTint)
// The controls are the panel's generic knobs and chips (pq/document/modules.h enginePanel); the face has none. Costs:
// nothing while no voice sounds (the reader is not polled); a poll appends ≤ 4 floats per voice. World units 220 × 44.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "faces.h"
#include "pq/meta/noise.h"

namespace pq::views {

ui::Rgba noiseColourTint(double colour) noexcept {
  struct Stop {
    double c;
    float r, g, b;
  };
  static constexpr Stop kStops[] = {{-6.0, 156.0f, 98.0f, 56.0f},    // brown
                                    {-3.0, 238.0f, 140.0f, 172.0f},  // pink
                                    {0.0, 232.0f, 232.0f, 236.0f},   // white
                                    {3.0, 96.0f, 156.0f, 255.0f},    // blue
                                    {6.0, 176.0f, 96.0f, 255.0f}};   // violet
  const double c = std::isfinite(colour) ? std::clamp(colour, -6.0, 6.0) : 0.0;
  size_t i = 0;
  while (i + 2 < std::size(kStops) && c > kStops[i + 1].c) ++i;
  const Stop& a = kStops[i];
  const Stop& b = kStops[i + 1];
  const float t = static_cast<float>((c - a.c) / (b.c - a.c));
  auto mix = [&](float x, float y) { return static_cast<uint8_t>(std::lround(x + (y - x) * t)); };
  return ui::rgba(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
}

namespace {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

constexpr float kW = 220.0f, kH = 44.0f;
constexpr float kRadius = 6.0f;
constexpr Rect kWell{4.0f, 4.0f, 216.0f, 40.0f};
constexpr float kTraceX0 = 8.0f, kTraceX1 = 178.0f;  // the trace; the voices' dots at kTraceX1
constexpr float kYTop = 8.0f, kYBottom = 36.0f;      // +1 and −1
constexpr Rect kSlope{188.0f, 15.0f, 210.0f, 29.0f};  // the colour glyph, in its own column right of the trace
constexpr uint32_t kHistory = 171;                   // trace points (one world unit each)
constexpr float kFade = 0.08f;

float valueY(float v) noexcept {
  const float c = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
  return kYBottom + (kYTop - kYBottom) * (c + 1.0f) * 0.5f;
}

class NoiseFace final : public MetaFace {
public:
  explicit NoiseFace(const meta::Type& t) : MetaFace(t) {}

  Vec2 worldSize() const noexcept override { return {kW, kH}; }
  FaceShape shape() const noexcept override { return {false, {}, kRadius}; }
  float inputY(size_t i, size_t n) const noexcept override { return i == 0 ? kH * 0.5f : MetaFace::inputY(i, n); }
  float outputY(size_t, size_t) const noexcept override { return kH * 0.5f; }

  uint32_t staticLayerCount() const noexcept override { return 3; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.body : (layer == 1 ? th.face.track : th.face.frame);
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) {
      p.roundedRect({0.5f, 0.5f, kW - 0.5f, kH - 0.5f}, kRadius, w);
      return;
    }
    if (layer == 1) {
      p.roundedRect(kWell, 4.0f, ui::mulAlpha(w, 0.3f));
      return;
    }
    // The zero line, faint and dashed; the ±1 lines fainter.
    const Vec2 mid[2] = {{kTraceX0, valueY(0.0f)}, {kTraceX1, valueY(0.0f)}};
    p.dashed(mid, 0.5f, 1.5f, 2.5f, ui::mulAlpha(w, 0.35f));
    p.line({kTraceX0, kYTop}, {kTraceX1, kYTop}, 0.5f, ui::mulAlpha(w, 0.18f));
    p.line({kTraceX0, kYBottom}, {kTraceX1, kYBottom}, 0.5f, ui::mulAlpha(w, 0.18f));
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    const ui::Theme& th = *env.theme;
    const double now = clockNow();
    // The colour glyph: a line through the box's centre tilted by the slope, in the colour's tint.
    const double colour = hasTarget() ? target().value(meta::kNoiseColour) : 0.0;
    const float cx = (kSlope.x0 + kSlope.x1) * 0.5f, cy = (kSlope.y0 + kSlope.y1) * 0.5f;
    const float half = (kSlope.x1 - kSlope.x0) * 0.5f;
    const float dy = static_cast<float>(std::clamp(colour, -6.0, 6.0) / 6.0) * (kSlope.y1 - kSlope.y0) * 0.5f;
    p.line({cx - half, cy + dy}, {cx + half, cy - dy}, 1.4f, noiseColourTint(colour));
    // The trace.
    if (trace_.size() >= 2) {
      pts_.clear();
      const float step = (kTraceX1 - kTraceX0) / static_cast<float>(kHistory - 1);
      const float x0 = kTraceX1 - step * static_cast<float>(trace_.size() - 1);
      for (size_t i = 0; i < trace_.size(); ++i) pts_.push_back({x0 + step * static_cast<float>(i), valueY(trace_[i])});
      p.polyline(pts_, 1.1f, ui::mulAlpha(th.face.signal, 0.85f));
    }
    // The voices' dots at the newest end.
    if (!shown_.empty()) {
      p.beginGlow();
      for (const Shown& s : shown_)
        p.glow({kTraceX1, valueY(s.v.phase)}, 7.0f, ui::withAlpha(th.face.glow, static_cast<uint32_t>(150.0f * alphaOf(s, now))));
      p.endGlow();
      for (const Shown& s : shown_) {
        const float a = alphaOf(s, now);
        p.disc({kTraceX1, valueY(s.v.phase)}, 2.2f, ui::mulAlpha(th.face.signal, a));
        p.disc({kTraceX1, valueY(s.v.phase)}, 1.0f, ui::mulAlpha(ui::rgba(255, 255, 255), a));
      }
    }
    if (env.selected || env.hovered) {
      const Vec2 o[5] = {{0.75f, 0.75f}, {kW - 0.75f, 0.75f}, {kW - 0.75f, kH - 0.75f}, {0.75f, kH - 0.75f}, {0.75f, 0.75f}};
      p.polyline(o, env.selected ? 1.75f : 1.25f, env.selected ? th.circuit.selected : th.circuit.hover);
    }
  }

  void liveRects(std::vector<Rect>& out) const override { out.push_back(kWell); }
  bool animating() const noexcept override { return !shown_.empty(); }

  int32_t hitTest(Vec2 l, float) const noexcept override {
    return l.x < 0.0f || l.y < 0.0f || l.x > kW || l.y > kH ? kNone : kFrame;
  }
  bool onPointer(const FacePointer&) override { return false; }
  bool dragging() const noexcept override { return false; }
  void cancelDrag() override {}
  std::optional<Rect> paramRect(size_t param) const noexcept override {
    if (param == meta::kNoiseColour) return kSlope;
    return std::nullopt;
  }
  void describe(int32_t part, std::string& title, std::string& body) const override {
    if (part == kFrame || part == kNone) {
      title = "Noise";
      body = "The output's last moments (−1 … +1, before Min / Max) and each sounding voice's value; the small line is the "
             "colour's slope (brown, pink, white, blue, violet).";
      return;
    }
    MetaFace::describe(part, title, body);
  }

  // (tests) the trace as the face holds it
  const std::vector<float>& trace() const noexcept { return trace_; }

protected:
  bool voicesPolled(bool changed) override {
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
          found->born = justBound_ ? now - kFade : now;
        } else if (found->dying) {
          found->born = now - static_cast<double>(alphaOf(*found, now)) * kFade;
        }
        found->v = v;
        found->dying = false;
        found->seen = true;
      }
      for (Shown& s : shown_)
        if (!s.seen && !s.dying) {
          s.dying = true;
          s.died = now;
        }
      // The trace follows the lowest sounding voice slot: up to four of its samples since the last read, in order.
      const FaceVoice* tv = nullptr;
      for (uint32_t i = 0; i < voiceCount_; ++i)
        if (tv == nullptr || voiceBuf_[i].slot < tv->slot) tv = &voiceBuf_[i];
      if (tv != nullptr) {
        const uint32_t n = tv->sampleCount;
        if (n == 0) push(tv->phase);
        for (uint32_t k = 1; k <= std::min<uint32_t>(n, 4); ++k) push(tv->samples[(k * n) / std::min<uint32_t>(n, 4) - 1]);
      }
    }
    justBound_ = false;
    const size_t before = shown_.size();
    std::erase_if(shown_, [&](const Shown& s) { return s.dying && now - s.died >= kFade; });
    bool fading = shown_.size() != before;
    for (const Shown& s : shown_) fading = fading || s.dying || now - s.born < kFade;
    if (shown_.size() > 2 * kMaxVoices) shown_.erase(shown_.begin(), shown_.end() - 2 * kMaxVoices);
    return changed || fading;
  }

private:
  struct Shown {
    FaceVoice v;
    double born = 0.0, died = 0.0;
    bool dying = false, seen = false;
  };
  static float alphaOf(const Shown& s, double now) noexcept {
    const float in = std::clamp(static_cast<float>((now - s.born) / kFade), 0.0f, 1.0f);
    const float out = s.dying ? std::clamp(1.0f - static_cast<float>((now - s.died) / kFade), 0.0f, 1.0f) : 1.0f;
    return std::min(in, out);
  }
  void push(float v) {
    if (trace_.size() >= kHistory) trace_.erase(trace_.begin());
    trace_.push_back(std::isfinite(v) ? v : 0.0f);
  }

  std::vector<Shown> shown_;
  std::vector<float> trace_;
  std::vector<Vec2> pts_;
};

}  // namespace

std::unique_ptr<MetaFace> makeNoiseFace(const meta::Type& t) { return std::make_unique<NoiseFace>(t); }

}  // namespace pq::views
