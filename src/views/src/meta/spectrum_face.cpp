// The Spectrum face (TX.spectrum; SPEC §15 2026-11-01 "analysis modules"): the module IS its display. A dark body with
// the reusable spectrum plot (pq/views/devices/spectrum_plot.h, PQ Meter's look: Ice by level, fill, glow, peak hold,
// grid and labels) filling it; the signal enters on the left and leaves on the right unchanged. Two small toggles in
// the top-right corner: PEAK (the peak hold line) and FREEZE (the picture holds while the analysis keeps running, as
// PQ Meter's freeze). The pointer shows its frequency, nearest note and level. Labels appear from a readable zoom up;
// far out the face is a faithful miniature of the curve. World units 240 × 150.
//
// The analysis: the host binds the instance's feed (Face::setAnalysis: a chain Spectrum device's own tap; a nested
// call in a circuit has none and shows its grid). The face's "voices" are the feed's frames: the hosts poll it at
// ≤ 60 Hz while the levels move and stop once they rest; the feed's waker (sound arrived) asks the host to poll again.
// A face that is polled but no longer painted (its host hidden) lets go of the feed after one unanswered repaint.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "faces.h"
#include "pq/views/devices/spectrum_plot.h"

namespace pq::views {

namespace {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

constexpr float kW = 240.0f, kH = 150.0f;
constexpr float kRadius = 10.0f;
constexpr Rect kPlot{8.0f, 8.0f, kW - 8.0f, kH - 8.0f};
constexpr Rect kPeakChip{kW - 80.0f, 12.0f, kW - 48.0f, 22.0f};
constexpr Rect kFreezeChip{kW - 45.0f, 12.0f, kW - 12.0f, 22.0f};
constexpr float kLabelPx = 8.0f;          // world units
constexpr float kLabelMinDevicePx = 7.0f;  // below this the labels are left out (the curve stays)
enum Part : int32_t { kPlotPart = 0, kPeak = 1, kFreeze = 2 };

bool inside(const Rect& r, Vec2 p) noexcept { return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1; }

// The hosts poll a face's voice reader; this one stands for the feed's frames.
class FeedFrames final : public VoiceTapReader {
public:
  uint32_t read(std::span<FaceVoice>) override { return 0; }
  uint64_t serial() const noexcept override { return serial_; }
  bool live() const noexcept override { return live_; }
  uint64_t serial_ = 0;
  bool live_ = false;
};

class SpectrumFace final : public MetaFace {
public:
  explicit SpectrumFace(const meta::Type& t) : MetaFace(t) {
    devices::SpectrumPlotStyle s;
    s.peaks = peaks_;
    plot_.setStyle(s);
  }

  Vec2 worldSize() const noexcept override { return {kW, kH}; }
  float inputY(size_t, size_t) const noexcept override { return 0.5f * kH; }
  float jackInset() const noexcept override { return 0.0f; }  // TMM.13-fix2 (L23): on the frame's edge, off the plot
  float outputY(size_t, size_t) const noexcept override { return 0.5f * kH; }

  void setAnalysis(std::function<devices::SpectrumFeed*()> feedOf, std::function<void()> wakeHost) override {
    feedOf_ = std::move(feedOf);
    wakeHost_ = std::move(wakeHost);
    syncFeed();
  }
  bool wantsAnalysis() const noexcept override { return true; }
  // Bound to a source: the hosts poll the face (once per wake while it has no feed yet: a repaint that asks again).
  VoiceTapReader* voices() const noexcept override { return feedOf_ ? &frames_ : nullptr; }
  bool animating() const noexcept override { return frames_.live_; }
  bool pollVoices() override {
    if (!syncFeed()) return true;  // no tap yet: one repaint (the host asks again at its next wake)
    const double now = clockNow();
    // Painted lately: shown. Not painted since the last repaint it asked for: its host is hidden — let go.
    if (now - lastPaint_ > 0.5) {
      if (probe_ == 0.0) {
        probe_ = now;
        return true;  // ask for one repaint
      }
      if (lastPaint_ < probe_ && now - probe_ > 0.05) {
        plot_.release();
        frames_.live_ = false;
        probe_ = 0.0;
        return false;
      }
    }
    if (frozen_) {
      frames_.live_ = false;
      return false;
    }
    frames_.live_ = false;  // set again by the plot's frame request while something moves
    polling_ = true;
    const bool changed = plot_.frame(now, true);
    polling_ = false;
    if (changed) ++frames_.serial_;
    return changed;
  }

  // ---- static layers: body; the plot's well; frame ----
  uint32_t staticLayerCount() const noexcept override { return 3; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.body : (layer == 1 ? th.device.display : th.face.frame);
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) p.roundedRect({1.0f, 1.0f, kW - 1.0f, kH - 1.0f}, kRadius, w);
    else if (layer == 1) p.roundedRect(kPlot, 4.0f, w);
    else outline(p, w);
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    const ui::Theme& th = *env.theme;
    syncFeed();
    lastPaint_ = clockNow();
    probe_ = 0.0;
    const ui::FaceTransform& t = p.transform();
    const Vec2 a = t.map({kPlot.x0, kPlot.y0}), b = t.map({kPlot.x1, kPlot.y1});
    const Rect r{a.x, a.y, b.x, b.y};
    ui::DrawList& dl = p.drawList();
    const float scale = t.scale;  // device px per world unit: the plot's widths follow the zoom
    devices::PlotText text;
    text.lineHeight = kLabelPx * scale;
    const bool labels = text.lineHeight >= kLabelMinDevicePx * env.uiScale;
    if (labels)
      text.draw = [&p, &t, lh = text.lineHeight](Vec2 topLeft, std::string_view s, Rgba col) {
        const float w = 0.6f * lh * static_cast<float>(s.size());
        p.text(t.unmap({topLeft.x + 0.5f * w, topLeft.y + 0.5f * lh}), s, kLabelPx, col);
      };
    if (plot_.feed() != nullptr && !frozen_) {
      painting_ = true;
      plot_.wake();  // shown: hold the feed, and show what it has at once (the host polls while frames are live)
      painting_ = false;
    }
    dl.pushClip(r);
    devices::SpectrumPlotStyle s = plot_.style();
    s.freqLabels = s.dbLabels = labels;
    plot_.setStyle(s);
    plot_.paintAll(dl, r, th, scale, text, hoverX_ >= 0.0f ? t.map({hoverX_, 0.0f}).x : -1.0f);
    dl.popClip();
    if (labels) {
      chip(p, kPeakChip, "PEAK", peaks_, th);
      chip(p, kFreezeChip, "FREEZE", frozen_, th);
    }
    // Leads in and out: the signal passes through unchanged.
    for (const FaceJack& j : env.jacks) {
      if (!j.visible) continue;
      const float x0 = j.input ? 5.0f : kPlot.x1, x1 = j.input ? kPlot.x0 : kW - 5.0f;
      p.line({x0, j.at.y}, {x1, j.at.y}, 1.25f, j.connected ? th.face.signal : th.face.jackRing);
    }
    if (env.selected || (env.hovered && hovered_ == kFrame))
      outline(p, env.selected ? th.circuit.selected : th.circuit.hover);
  }

  void liveRects(std::vector<Rect>& out) const override { out.push_back({0.0f, 0.0f, kW, kH}); }

  int32_t hitTest(Vec2 l, float) const noexcept override {
    if (l.x < 0.0f || l.y < 0.0f || l.x > kW || l.y > kH) return kNone;
    if (inside(kPeakChip, l)) return kPeak;
    if (inside(kFreezeChip, l)) return kFreeze;
    if (inside(kPlot, l)) return kPlotPart;
    return kFrame;
  }

  bool onPointer(const FacePointer& e) override {
    using K = FacePointer::Kind;
    switch (e.kind) {
      case K::Down: {
        if (e.button != ui::PointerButton::Left) return false;
        const int32_t part = hitTest(e.local, e.pxPerUnit);
        if (part == kPeak) {
          peaks_ = !peaks_;
          devices::SpectrumPlotStyle s = plot_.style();
          s.peaks = peaks_;
          plot_.setStyle(s);
          return true;
        }
        if (part == kFreeze) {
          frozen_ = !frozen_;
          if (!frozen_) plot_.wake();
          return true;
        }
        return false;  // the plot: the host's (select, move)
      }
      case K::Move: {
        const float x = inside(kPlot, e.local) && !inside(kPeakChip, e.local) && !inside(kFreezeChip, e.local) ? e.local.x : -1.0f;
        if (x == hoverX_) return false;
        hoverX_ = x;
        return true;
      }
      case K::Leave:
        if (hoverX_ < 0.0f) return false;
        hoverX_ = -1.0f;
        return true;
      case K::Up:
      case K::Wheel: return false;
    }
    return false;
  }
  bool dragging() const noexcept override { return false; }
  void cancelDrag() override {}
  ui::Cursor cursor(int32_t part) const noexcept override {
    return part == kPeak || part == kFreeze ? ui::Cursor::Pointer : ui::Cursor::Inherit;
  }

  void describe(int32_t part, std::string& title, std::string& body) const override {
    switch (part) {
      case kPeak:
        title = "Peak Hold";
        body = peaks_ ? "On: the line marks each frequency's recent maximum (held 1.5 s, then falling 12 dB/s). Click: off."
                      : "Off. Click to draw the peak hold line (each frequency's recent maximum).";
        return;
      case kFreeze:
        title = "Freeze";
        body = frozen_ ? "Frozen: the picture holds (the signal still passes). Click to follow the signal again."
                       : "Click to hold the picture (the signal still passes).";
        return;
      case kPlotPart: {
        title = "Spectrum";
        body = plot_.feed() != nullptr
                   ? "The spectrum of the signal passing through (it is not changed). Point at it for the frequency, "
                     "its nearest note and the level there."
                   : "The spectrum of the signal passing through (it is not changed). Shown when the module plays in a "
                     "track's chain.";
        return;
      }
      default: MetaFace::describe(part, title, body);
    }
  }

  // Tests: the plot (its shown state, its style).
  devices::SpectrumPlot& plot() noexcept { return plot_; }

private:
  // The feed the source gives now (it appears once the instance's unit is compiled); true when there is one.
  bool syncFeed() {
    devices::SpectrumFeed* f = feedOf_ ? feedOf_() : nullptr;
    if (f != plot_.feed())
      plot_.setFeed(f, [this] {
        frames_.live_ = true;
        if (!polling_ && !painting_ && wakeHost_) wakeHost_();  // the host's own poll / paint keeps going by itself
      });
    return f != nullptr;
  }
  static void outline(ui::FacePainter& p, Rgba col) {
    const Vec2 pts[5] = {{1.0f, 1.0f}, {kW - 1.0f, 1.0f}, {kW - 1.0f, kH - 1.0f}, {1.0f, kH - 1.0f}, {1.0f, 1.0f}};
    p.polyline(pts, 1.25f, col);
  }
  void chip(ui::FacePainter& p, const Rect& r, const char* label, bool on, const ui::Theme& th) const {
    p.roundedRect(r, 3.0f, on ? ui::mulAlpha(th.face.value, 0.3f) : ui::rgba(8, 8, 12, 160));
    p.text({0.5f * (r.x0 + r.x1), 0.5f * (r.y0 + r.y1)}, label, 6.0f, on ? th.face.value : th.device.displayText);
  }

  mutable devices::SpectrumPlot plot_;
  mutable FeedFrames frames_;
  std::function<devices::SpectrumFeed*()> feedOf_;
  std::function<void()> wakeHost_;
  bool peaks_ = true, frozen_ = false;
  float hoverX_ = -1.0f;  // world x of the pointer over the plot (−1: none)
  double lastPaint_ = 0.0, probe_ = 0.0;
  bool polling_ = false, painting_ = false;
};

}  // namespace

std::unique_ptr<MetaFace> makeSpectrumFace(const meta::Type& t) { return std::make_unique<SpectrumFace>(t); }

}  // namespace pq::views
