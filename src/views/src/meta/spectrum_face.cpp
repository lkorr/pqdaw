// The Spectrum face (TX.spectrum; SPEC §15 2026-11-01 "analysis modules"): the module IS its display. A dark body with
// the reusable spectrum plot (pq/views/devices/spectrum_plot.h, PQ Meter's look: Ice by level, fill, glow, peak hold,
// grid and labels) filling it; the signal enters on the left and leaves on the right unchanged. Three small chips in
// the top-right corner: SET (VZ.1: the settings sheet, option_sheet.h — ANALYSIS: window, low-end window, smoothing,
// attack / release, tilt, peak hold time and fall; DISPLAY: range, fill, glow, line, bands, grid, labels, peak line),
// PEAK (the peak hold line) and FREEZE (the picture holds while the analysis keeps running, as PQ Meter's freeze). The
// options are the instance's data. The pointer shows its frequency, nearest note and level. Labels appear from a
// readable zoom up; far out the face is a faithful miniature of the curve. World units 240 × 150.
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
#include "option_sheet.h"
#include "pq/views/devices/spectrum_plot.h"

namespace pq::views {

namespace {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

constexpr float kW = 240.0f, kH = 150.0f;
constexpr float kRadius = 10.0f;
constexpr Rect kPlot{8.0f, 8.0f, kW - 8.0f, kH - 8.0f};
constexpr Rect kSetChip{kW - 109.0f, 12.0f, kW - 83.0f, 22.0f};
constexpr Rect kPeakChip{kW - 80.0f, 12.0f, kW - 48.0f, 22.0f};
constexpr Rect kFreezeChip{kW - 45.0f, 12.0f, kW - 12.0f, 22.0f};
constexpr float kLabelPx = 8.0f;          // world units
constexpr float kLabelMinDevicePx = 7.0f;  // below this the labels are left out (the curve stays)
enum Part : int32_t { kPlotPart = 0, kPeak = 1, kFreeze = 2, kSetPart = 3, kSheetPart = 4, kRowBase = sheet::kRowBase };

bool inside(const Rect& r, Vec2 p) noexcept { return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1; }

// ---- options (the instance's data; VZ.1) -----------------------------------------------------------------------------

using sheet::kOnOff;
using sheet::Opt;
enum O : uint32_t {
  oFft, oSharpLows, oSmooth, oAttack, oRelease, oTilt, oHold, oFall,
  oPeaks, oFloor, oCeil, oFill, oGlow, oLine, oBands, oGrid, oLabels, oCount
};
constexpr const char* kSmoothNames[] = {"Off", "1/24 oct", "1/12 oct", "1/6 oct", "1/3 oct"};
constexpr float kSmoothOct[] = {0.0f, 1.0f / 24.0f, 1.0f / 12.0f, 1.0f / 6.0f, 1.0f / 3.0f};
// The analysis defaults are AnalysisHub::config's (dsp::SpectrumConfig: SPEC §11.1), the look SpectrumPlotStyle's.
const Opt kOpts[oCount] = {
    {"fft", "Window", 11, 10, 14, Opt::Pow2},
    {"lows", "Long low window", 1, 0, 1, Opt::Toggle, kOnOff},
    {"smooth", "Smoothing", 0, 0, 4, Opt::Choice, kSmoothNames},
    {"att", "Attack", 25, 1, 500, Opt::LogNumber, nullptr, "%.0f ms"},
    {"rel", "Release", 350, 10, 3000, Opt::LogNumber, nullptr, "%.0f ms"},
    {"tilt", "Tilt", 4.5, -6.0, 9.0, Opt::Number, nullptr, "%.1f dB/oct", 0.1},
    {"hold", "Peak hold", 1.5, 0.1, 10.0, Opt::LogNumber, nullptr, "%.1f s"},
    {"fall", "Peak fall", 12, 1, 60, Opt::LogNumber, nullptr, "%.0f dB/s"},
    {"peaks", "Peak line", 1, 0, 1, Opt::Toggle, kOnOff},
    {"floor", "Floor", -78, -140, -30, Opt::Number, nullptr, "%.0f dB", 1.0},
    {"ceil", "Ceiling", 0, -40, 12, Opt::Number, nullptr, "%.0f dB", 1.0},
    {"fill", "Fill", 0.5, 0.0, 1.0, Opt::Number, nullptr, "%.2f", 0.01},
    {"glow", "Glow", 1.0, 0.0, 2.0, Opt::Number, nullptr, "%.2f", 0.01},
    {"lw", "Line width", 1.6, 0.5, 4.0, Opt::Number, nullptr, "%.1f", 0.1},
    {"bands", "Band backdrop", 1, 0, 1, Opt::Toggle, kOnOff},
    {"grid", "Grid", 1, 0, 1, Opt::Toggle, kOnOff},
    {"labels", "Labels", 1, 0, 1, Opt::Toggle, kOnOff},
};

struct Options : sheet::Values {
  Options() : Values(kOpts) {}
  devices::SpectrumTuning tuning() const noexcept {
    devices::SpectrumTuning t;
    t.fftSize = 1u << std::clamp(choice(oFft), 10, 14);
    t.sharpLows = on(oSharpLows);
    t.smoothingOct = kSmoothOct[std::clamp(choice(oSmooth), 0, 4)];
    t.attackMs = static_cast<float>((*this)[oAttack]);
    t.releaseMs = static_cast<float>((*this)[oRelease]);
    t.tiltDbPerOct = static_cast<float>((*this)[oTilt]);
    t.holdSeconds = static_cast<float>((*this)[oHold]);
    t.fallDbPerSecond = static_cast<float>((*this)[oFall]);
    return t;
  }
  // The look (the ceiling at least 10 dB over the floor).
  void style(devices::SpectrumPlotStyle& s) const noexcept {
    s.peaks = on(oPeaks);
    s.floorDb = static_cast<float>((*this)[oFloor]);
    s.ceilDb = std::max(static_cast<float>((*this)[oCeil]), s.floorDb + 10.0f);
    s.fill = static_cast<float>((*this)[oFill]);
    s.glow = static_cast<float>((*this)[oGlow]);
    s.lineWidth = static_cast<float>((*this)[oLine]);
    s.bands = on(oBands);
    s.grid = on(oGrid);
  }
};

void visibleRows(int tab, std::vector<uint32_t>& out) {
  out.clear();
  if (tab == sheet::kAnalysis)
    for (uint32_t i = oFft; i <= oFall; ++i) out.push_back(i);
  else
    for (uint32_t i = oPeaks; i < oCount; ++i) out.push_back(i);
}

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
    visibleRows(panel_.tab, rows_);
    applyOptions();
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
    syncOptions();
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
    syncOptions();
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
    s.freqLabels = s.dbLabels = labels && opts_.on(oLabels) && !sheet_;
    plot_.setStyle(s);
    plot_.paintAll(dl, r, th, scale, text, hoverX_ >= 0.0f ? t.map({hoverX_, 0.0f}).x : -1.0f);
    dl.popClip();
    if (labels) {
      chip(p, kSetChip, "SET", sheet_, th);
      chip(p, kPeakChip, "PEAK", opts_.on(oPeaks), th);
      chip(p, kFreezeChip, "FREEZE", frozen_, th);
      if (sheet_) {
        Options saved;
        const bool isSaved = sheet::savedDefault(type().id, saved) && saved == opts_;
        panel_.paint(p, th, opts_, rows_, hovered_, isSaved);
      }
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
    if (inside(kSetChip, l)) return kSetPart;
    if (inside(kPeakChip, l)) return kPeak;
    if (inside(kFreezeChip, l)) return kFreeze;
    if (sheet_) {
      const int32_t part = panel_.hit(l, rows_.size());
      if (part >= 0) return part == sheet::kBody ? kSheetPart : part;
    }
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
          setOption(oPeaks, opts_.on(oPeaks) ? 0.0 : 1.0);
          return true;
        }
        if (part == kFreeze) {
          frozen_ = !frozen_;
          if (!frozen_) plot_.wake();
          return true;
        }
        if (part == kSetPart) {
          sheet_ = !sheet_;
          return true;
        }
        if (part >= kRowBase) return rowDown(static_cast<uint32_t>(part - kRowBase), e);
        if (part == sheet::kTabBase || part == sheet::kTabBase + 1) {
          panel_.tab = part - sheet::kTabBase;
          visibleRows(panel_.tab, rows_);
          return true;
        }
        if (part == sheet::kSave) {
          meta::saveUserDefaultData(type().id, opts_.data());
          return true;
        }
        if (part == sheet::kReset) {
          opts_.reset();
          writeOptions();
          return true;
        }
        if (part == kSheetPart) return true;
        return false;  // the plot: the host's (select, move)
      }
      case K::Move: {
        if (drag_) {
          const Opt& op = kOpts[dragOpt_];
          const double k = ((e.mods & ui::mod::kShift) ? 0.1 : 1.0) / 160.0;
          setOption(dragOpt_, sheet::fromTravel(op, std::clamp(pressTravel_ + (pressY_ - e.device.y) * k, 0.0, 1.0)));
          return true;
        }
        const float x = !sheet_ && hitTest(e.local, e.pxPerUnit) == kPlotPart ? e.local.x : -1.0f;
        if (x == hoverX_) return false;
        hoverX_ = x;
        return true;
      }
      case K::Leave:
        if (hoverX_ < 0.0f) return false;
        hoverX_ = -1.0f;
        return true;
      case K::Up: {
        const bool was = drag_;
        drag_ = false;
        if (was && editing_) {
          target().end();
          editing_ = false;
        }
        return was;
      }
      case K::Wheel: return false;
    }
    return false;
  }
  bool dragging() const noexcept override { return drag_; }
  void cancelDrag() override {
    if (drag_ && editing_) {
      target().cancel();
      editing_ = false;
      syncOptions(true);
    }
    drag_ = false;
  }
  bool onKey(const ui::KeyEvent& e) override {
    if (!e.down || e.key != ui::key::kEscape || !sheet_) return false;
    sheet_ = false;
    return true;
  }
  ui::Cursor cursor(int32_t part) const noexcept override {
    if (part >= kRowBase) {
      const auto i = static_cast<size_t>(part - kRowBase);
      return i < rows_.size() && sheet::numeric(kOpts[rows_[i]]) ? ui::Cursor::ResizeNS : ui::Cursor::Pointer;
    }
    if (part == kPeak || part == kFreeze || part == kSetPart || (part >= sheet::kTabBase && part <= sheet::kReset))
      return ui::Cursor::Pointer;
    return ui::Cursor::Inherit;
  }

  void describe(int32_t part, std::string& title, std::string& body) const override {
    switch (part) {
      case kPeak:
        title = "Peak Hold";
        body = opts_.on(oPeaks) ? "On: the line marks each frequency's recent maximum (held, then falling: SET → ANALYSIS). "
                                  "Click: off."
                                : "Off. Click to draw the peak hold line (each frequency's recent maximum).";
        return;
      case kSetPart:
        title = "Settings";
        body = sheet_ ? "Click to close the settings."
                      : "Click for every option: window, smoothing, attack / release, tilt, peak hold; range, fill, glow, "
                        "line, bands, grid, labels.";
        return;
      case sheet::kTabBase:
        title = "Analysis";
        body = "The options of what is analysed: window, long low window, smoothing, attack / release, tilt, peak hold.";
        return;
      case sheet::kTabBase + 1:
        title = "Display";
        body = "Only how it looks: peak line, level range, fill, glow, line width, band backdrop, grid, labels.";
        return;
      case sheet::kSave:
        title = "Save as default";
        body = "New Spectrums start with these options (every tab). Saved with pqdaw's settings; existing ones keep theirs.";
        return;
      case sheet::kReset:
        title = "Reset";
        body = "Every option back to the factory default.";
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
      default:
        if (part >= kRowBase && static_cast<size_t>(part - kRowBase) < rows_.size()) {
          const Opt& op = kOpts[rows_[static_cast<size_t>(part - kRowBase)]];
          title = op.label;
          body = sheet::numeric(op) ? "Drag up / down to set (Shift: fine); double-click resets."
                                    : "Click to change (Shift: back); double-click resets.";
          return;
        }
        MetaFace::describe(part, title, body);
    }
  }

  // Tests: the plot (its shown state, its style).
  devices::SpectrumPlot& plot() noexcept { return plot_; }
  const devices::SpectrumPlotStyle& style() const noexcept { return plot_.style(); }

private:
  // ---- options ----
  void syncOptions(bool force = false) {
    const meta::DataPtr d = hasTarget() ? target().data() : nullptr;
    if (!force && d == data_ && optionsLoaded_) return;
    data_ = d;
    optionsLoaded_ = true;
    if (!local_) opts_.load(d.get());
    applyOptions();
  }
  void applyOptions() {
    visibleRows(panel_.tab, rows_);
    devices::SpectrumPlotStyle s = plot_.style();
    opts_.style(s);
    plot_.setStyle(s);
    sendTuning();
  }
  void sendTuning() {
    devices::SpectrumFeed* f = plot_.feed();
    if (f != nullptr) f->setTuning(opts_.tuning());  // (the feed ignores an unchanged tuning)
  }
  void setOption(uint32_t o, double v) {
    opts_.set(o, v);
    writeOptions();
  }
  void writeOptions() {
    if (hasTarget() && target().editable()) {
      data_ = opts_.data();
      target().setData(data_);
      data_ = target().data();
    } else {
      local_ = true;  // a read-only target: the face keeps its options itself
    }
    applyOptions();
    if (wakeHost_) wakeHost_();
  }
  bool rowDown(uint32_t row, const FacePointer& e) {
    if (row >= rows_.size()) return false;
    const uint32_t o = rows_[row];
    const Opt& op = kOpts[o];
    if (e.clicks >= 2) {
      setOption(o, op.def);
      return true;
    }
    if (op.kind == Opt::Choice || op.kind == Opt::Toggle) {
      setOption(o, sheet::cycled(opts_, o, (e.mods & ui::mod::kShift) ? -1 : 1));
      return true;
    }
    drag_ = true;
    dragOpt_ = o;
    pressY_ = e.device.y;
    pressTravel_ = sheet::toTravel(op, opts_[o]);
    if (hasTarget() && target().editable()) {
      target().begin(op.label);
      editing_ = true;
    }
    return true;
  }

  // The feed the source gives now (it appears once the instance's unit is compiled); true when there is one.
  bool syncFeed() {
    devices::SpectrumFeed* f = feedOf_ ? feedOf_() : nullptr;
    if (f != plot_.feed()) {
      plot_.setFeed(f, [this] {
        frames_.live_ = true;
        if (!polling_ && !painting_ && wakeHost_) wakeHost_();  // the host's own poll / paint keeps going by itself
      });
      sendTuning();
    }
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
  bool frozen_ = false, sheet_ = false;
  Options opts_;
  meta::DataPtr data_;
  bool optionsLoaded_ = false, local_ = false;
  std::vector<uint32_t> rows_;
  sheet::Sheet panel_{{kPlot.x0 + 1.0f, 25.0f, kPlot.x1 - 1.0f, kPlot.y1 - 1.0f}, 6.0f};
  bool drag_ = false, editing_ = false;
  uint32_t dragOpt_ = 0;
  float pressY_ = 0.0f;
  double pressTravel_ = 0.0;
  float hoverX_ = -1.0f;  // world x of the pointer over the plot (−1: none)
  double lastPaint_ = 0.0, probe_ = 0.0;
  bool polling_ = false, painting_ = false;
};

}  // namespace

std::unique_ptr<MetaFace> makeSpectrumFace(const meta::Type& t) { return std::make_unique<SpectrumFace>(t); }

bool spectrumStyle(const MetaFace& f, devices::SpectrumPlotStyle& out) {
  const auto* s = dynamic_cast<const SpectrumFace*>(&f);
  if (s == nullptr) return false;
  out = s->style();
  return true;
}

}  // namespace pq::views
