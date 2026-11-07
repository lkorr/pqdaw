// Device panel displays (T4.6; the Saturator panel of T4.9): the transfer curve and a plain spectrum (GU.10: EQ Eight's
// and Auto Filter's displays are retired: retired/src/views/src/devices/device_displays_eq_af.cpp). The spectrum is PQ Meter's look
// (SPEC §11.1: Ice colormap by level, fill fading down, level-weighted glow, body, bright core; 45 ms UI ease) over
// snapshots of a SpectrumFeed.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "../common/paint.h"
#include "device_math.h"
#include "panel_internal.h"
#include "pq/ui/text.h"
#include "pq/views/devices/spectrum_plot.h"
#include "pq/ui/colormap.h"
#include "pq/ui/lines.h"
#include "pq/ui/theme.h"

namespace pq::views::devices {

namespace {

using FreqAxis = SpectrumAxis;  // 10 Hz … 22 kHz, log (the analyser's axis)

ui::IRect insetRect(const ui::IRect& r, int32_t d) { return paint::inset(r, d); }

// The plot's text through the tree's small font (PQ Meter's grid labels).
PlotText panelText(ui::PaintContext& ctx) {
  PlotText t;
  t.lineHeight = static_cast<float>(ctx.scale.px(12.0f));
  if (!ctx.res.uiFonts || !ctx.res.text) return t;
  const ui::FontId font = ctx.res.uiFonts->small;
  ui::TextSystem* ts = ctx.res.text;
  t.width = [ts, font](std::string_view s) { return static_cast<float>(ts->shape(font, s).advance); };
  t.draw = [&ctx, ts, font, lh = ctx.scale.px(12.0f)](ui::Vec2 at, std::string_view s, ui::Rgba col) {
    const int32_t w = ts->shape(font, s).advance + 2;
    const ui::IRect box{static_cast<int32_t>(std::lround(at.x)), static_cast<int32_t>(std::lround(at.y)), w, lh};
    paint::text(ctx, font, box, s, col, ui::TextAlign::Left);
  };
  return t;
}

// ---- a display over the analysis of the device's signal (TX.spectrum) ------------------------------------------------
// The reusable plot (pq/views/devices/spectrum_plot.h) bound to the panel target's feed, the pointer readout, and —
// where the feed carries the device's input (a project device's analysis tap) and the display has an exact curve of
// its own — the "Measured" chip that overlays the response measured from the input and output (QUESTIONS
// Q?-TX.spectrum-a: the exact curve is drawn, the measured one on demand).
class AnalysedDisplay : public PanelDisplay {
public:
  AnalysedDisplay(std::string name, DevicePanel::Impl& impl, const ControlDesc& d) : PanelDisplay(std::move(name), impl, d) {}

  void onFrame(double now) override {
    sync();
    if (spec_.frame(now, shownInTree(*this))) markDirty();
  }
  void onHoverChanged(bool h) override {
    if (!h && hoverX_ >= 0.0f) {
      hoverX_ = -1.0f;
      markDirty();
    }
  }
  // Tests and scenes: the measured overlay (the chip's state).
  bool measured() const noexcept override { return measured_; }
  void setMeasured(bool on) override {
    if (on == measured_ || !withChip_) return;  // only a display with an exact curve of its own has the overlay
    measured_ = on;
    SpectrumPlotStyle s = spec_.style();
    s.response = on;
    spec_.setStyle(s);
    markDirty();
  }
  SpectrumPlot& spectrumPlot() noexcept { return spec_; }
  // Whether the chip is offered (the feed has the device's input).
  bool measurable() const noexcept { return withChip_ && spec_.feed() != nullptr && spec_.feed()->hasInput(); }
  ui::Rect chipRect() const noexcept { return chip_; }

protected:
  void sync() {
    spec_.setFeed(impl_.spectrum(), [this] { requestFrame(); });
  }
  // The chip (when offered) at the plot's top left; returns its rect.
  void paintChip(ui::PaintContext& ctx, const ui::Theme& th, const ui::Rect& plot) {
    chip_ = {};
    if (!measurable() || !ctx.res.uiFonts || !ctx.res.text) return;
    const float s = ctx.scale.factor;
    const char* label = "Measured";
    const float w = static_cast<float>(ctx.res.text->shape(ctx.res.uiFonts->small, label).advance) + std::round(10.0f * s);
    const float h = static_cast<float>(ctx.scale.px(12.0f)) + std::round(4.0f * s);
    chip_ = {std::round(plot.x0 + 4.0f * s), std::round(plot.y0 + 4.0f * s), 0, 0};
    chip_.x1 = chip_.x0 + w;
    chip_.y1 = chip_.y0 + h;
    const ui::Rgba violet = ui::hex(0xAC8EFF);
    ctx.dl.addRoundedRectFilled(chip_, 3.0f * s, measured_ ? ui::setAlpha(violet, 0.22f) : ui::rgba(8, 8, 12, 150));
    ctx.dl.addRoundedRectStroke(chip_, 3.0f * s, measured_ ? violet : th.device.displayGrid, 1.0f);
    const ui::IRect box{static_cast<int32_t>(chip_.x0), static_cast<int32_t>(chip_.y0), static_cast<int32_t>(w),
                        static_cast<int32_t>(h)};
    paint::text(ctx, ctx.res.uiFonts->small, box, label, measured_ ? violet : th.device.displayText, ui::TextAlign::Center);
  }
  bool chipAt(ui::Vec2 p) const noexcept { return chip_.x1 > chip_.x0 && p.x >= chip_.x0 && p.x < chip_.x1 && p.y >= chip_.y0 && p.y < chip_.y1; }
  // Pointer: the chip toggles the overlay; a move tracks the readout. True when consumed.
  bool chipPointer(const ui::PointerEvent& e) {
    if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left && chipAt(e.pos)) {
      setMeasured(!measured_);
      return true;
    }
    return false;
  }
  void trackHover(ui::Vec2 p, bool on) {
    const float x = on && !chipAt(p) ? p.x : -1.0f;
    if (x != hoverX_) {
      hoverX_ = x;
      markDirty();
    }
  }

  SpectrumPlot spec_;
  bool withChip_ = false;
  bool measured_ = false;
  float hoverX_ = -1.0f;
  ui::Rect chip_{};
};

// ---- Saturator: the transfer curve ------------------------------------------------------------------------------------
class TransferDisplay final : public PanelDisplay {
public:
  TransferDisplay(DevicePanel::Impl& impl, const ControlDesc& d) : PanelDisplay(d.id, impl, d) { rebind(); }
  void rebind() override {
    deps_.clear();
    for (const auto& [role, name] : desc_.bind) deps_.insert(name.empty() || name[0] != '#' ? impl_.resolve(name) : "#" + impl_.resolve(name.substr(1)));
    dirty_ = true;
  }
  bool dependsOn(std::string_view name) const override { return deps_.count(std::string(name)) != 0; }
  void valuesChanged() override {
    dirty_ = true;
    markDirty();
  }
  ui::InfoText info() const override {
    return {"Transfer Curve", "The Saturator's static curve: output level (vertical) against input level (horizontal), with drive, "
                              "output, soft clip and dry/wet applied. The colour EQ is frequency dependent and not shown."};
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, ctx.scale.exact(2.0f), th->device.display, th->device.border);
    const ui::IRect in = insetRect(r, ctx.scale.px(3.0f));
    const ui::Rect f = in.toRect();
    ctx.dl.pushClip(r.toRect());
    const float cx = std::round(0.5f * (f.x0 + f.x1)), cy = std::round(0.5f * (f.y0 + f.y1));
    const float sx = 0.5f * f.width() / kRange, sy = 0.5f * f.height() / kRange;
    for (float v : {-1.0f, -0.5f, 0.5f, 1.0f}) {
      const float x = std::round(cx + v * sx), y = std::round(cy - v * sy);
      ctx.dl.addRectFilled({x, f.y0, x + 1.0f, f.y1}, th->device.displayGrid);
      ctx.dl.addRectFilled({f.x0, y, f.x1, y + 1.0f}, th->device.displayGrid);
    }
    ctx.dl.addRectFilled({cx, f.y0, cx + 1.0f, f.y1}, th->device.displayGridMajor);
    ctx.dl.addRectFilled({f.x0, cy, f.x1, cy + 1.0f}, th->device.displayGridMajor);
    // The identity (no saturation), faint.
    const ui::Vec2 diag[2] = {{cx - kRange * sx, cy + kRange * sy}, {cx + kRange * sx, cy - kRange * sy}};
    ui::smoothPolyline(ctx.dl, diag, 2, th->device.displayGrid, ctx.scale.exact(1.0f));
    // The curve, cached per value change and width.
    const uint32_t n = static_cast<uint32_t>(std::max(16, in.w));
    if (dirty_ || ys_.size() != n) compute(n);
    pts_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(n - 1);
      pts_[i] = {f.x0 + u * f.width(), std::clamp(cy - ys_[i] * sy, f.y0 - 4.0f, f.y1 + 4.0f)};
    }
    ui::smoothPolyline(ctx.dl, pts_.data(), n, th->device.curve, ctx.scale.exact(1.6f));
    ctx.dl.popClip();
  }

private:
  static constexpr float kRange = 1.25f;  // the plot spans ±1.25 on both axes
  void compute(uint32_t n) {
    math::SaturatorState s;
    auto get = [&](const char* role, double def) {
      const auto it = desc_.bind.find(role);
      return it != desc_.bind.end() ? impl_.value(it->second) : def;
    };
    s.type = static_cast<int>(std::lround(get("type", 3)));
    s.softClip = get("softClip", 0) >= 0.5;
    s.drive = get("drive", 1);
    s.output = get("output", 1);
    s.dryWet = get("dryWet", 1);
    s.wsDrive = get("wsDrive", 0.5);
    s.wsLin = get("wsLin", 0.5);
    s.wsCurve = get("wsCurve", 0.5);
    s.wsDamp = get("wsDamp", 0);
    s.wsDepth = get("wsDepth", 0);
    s.wsPeriod = get("wsPeriod", 0);
    ys_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      const double x = (2.0 * i / (n - 1) - 1.0) * kRange;
      ys_[i] = static_cast<float>(math::saturatorTransfer(s, x));
    }
    dirty_ = false;
  }
  std::set<std::string, std::less<>> deps_;
  std::vector<float> ys_;
  std::vector<ui::Vec2> pts_;
  bool dirty_ = true;
};

// ---- a plain spectrum (the "spectrum" visual) -------------------------------------------------------------------------
class SpectrumDisplay final : public AnalysedDisplay {
public:
  SpectrumDisplay(DevicePanel::Impl& impl, const ControlDesc& d) : AnalysedDisplay(d.id, impl, d) {
    SpectrumPlotStyle s;
    s.peaks = true;
    spec_.setStyle(s);
  }
  bool dependsOn(std::string_view) const override { return false; }
  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    sync();
    spec_.wake();  // shown: hold the feed (and show what it has at once)
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, ctx.scale.exact(2.0f), th->device.display, th->device.border);
    const ui::Rect f = insetRect(r, 1).toRect();
    ctx.dl.pushClip(f);
    spec_.paintAll(ctx.dl, f, *th, ctx.scale.factor, panelText(ctx), hoverX_);
    ctx.dl.popClip();
  }
  bool onPointer(const ui::PointerEvent& e) override {
    if (e.type == ui::PointerType::Move) trackHover(e.pos, true);
    return false;
  }
  ui::InfoText info() const override {
    return {"Spectrum", "The spectrum of the device's output (PQ Meter's analyser: fill, glow, peak hold). Point at it for the "
                        "frequency, its nearest note and the level there."};
  }
};

}  // namespace

std::unique_ptr<PanelDisplay> makeDisplay(DevicePanel::Impl& impl, const ControlDesc& desc) {
  if (desc.visual == "transfer") return std::make_unique<TransferDisplay>(impl, desc);
  if (auto op = makeOperatorDisplay(impl, desc)) return op;
  return std::make_unique<SpectrumDisplay>(impl, desc);
}

}  // namespace pq::views::devices
