// Device panel displays (T4.6; the Saturator and EQ Eight panels of T4.9 / T4.10): the transfer curve, EQ Eight's
// response with draggable band dots over the output spectrum, and a plain spectrum. The spectrum is PQ Meter's look
// (SPEC §11.1: Ice colormap by level, fill fading down, level-weighted glow, body, bright core; 45 ms UI ease) over
// snapshots of a SpectrumFeed.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "../common/paint.h"
#include "device_math.h"
#include "panel_internal.h"
#include "pq/ui/colormap.h"
#include "pq/ui/lines.h"
#include "pq/ui/theme.h"

namespace pq::views::devices {

namespace {

constexpr float kSpecFloorDb = -78.0f, kSpecCeilDb = 0.0f;  // SPEC §11.1
constexpr double kEaseSeconds = 0.045;

// ---- the spectrum layer: snapshots → eased levels → PQ Meter's curve ---------------------------------------------------
class SpectrumLayer {
public:
  explicit SpectrumLayer(SpectrumFeed* feed) : feed_(feed) {}
  // The feed the panel's target offers now (it may arrive or change after the display was made).
  void setFeed(SpectrumFeed* f) {
    if (f == feed_) return;
    release();
    feed_ = f;
    seq_ = 0;
    target_.clear();
    shown_.clear();
  }
  ~SpectrumLayer() { release(); }
  SpectrumLayer(const SpectrumLayer&) = delete;
  SpectrumLayer& operator=(const SpectrumLayer&) = delete;

  SpectrumFeed* feed() const noexcept { return feed_; }
  bool acquired() const noexcept { return acquired_; }

  // From the owner's paint (only reached while shown): start the feed if there is something to show.
  void wake(ui::Widget& owner) {
    if (!feed_ || acquired_) return;
    acquire();
    owner.requestFrame();
  }
  // From the owner's onFrame. Returns true when the drawing changed; keeps the owner's frames going while there is
  // something to animate, else releases the feed (the worker blocks, the tap loses its reader).
  bool frame(ui::Widget& owner, double now) {
    if (!feed_) return false;
    if (!shownInTree(owner)) {
      release();
      return false;
    }
    if (!acquired_) acquire();
    bool settled = false;
    const uint64_t s = feed_->snapshot(seq_, target_, settled);
    const bool fresh = s != seq_;
    seq_ = s;
    if (shown_.size() != target_.size()) shown_ = target_;
    const double dt = std::clamp(last_ > 0.0 ? now - last_ : 0.0, 0.0, 0.1);
    last_ = now;
    const float a = static_cast<float>(1.0 - std::exp(-dt / kEaseSeconds));
    float moved = 0.0f;
    for (size_t i = 0; i < shown_.size(); ++i) {
      const float d = (target_[i] - shown_[i]) * a;
      shown_[i] += d;
      moved = std::max(moved, std::fabs(d));
    }
    const bool changed = fresh || moved > 0.01f;
    const bool animate = changed || !settled || feed_->live();
    if (animate) owner.requestFrame();
    else release();
    return changed;
  }
  // Draws the curve into r: x through hzToX, y from the dB range.
  template <class HzToX>
  void paint(ui::PaintContext& ctx, const ui::Rect& r, HzToX hzToX, float glow, float fillAlpha) {
    if (!feed_ || shown_.empty()) return;
    const uint32_t P = static_cast<uint32_t>(shown_.size());
    const uint32_t steps = std::clamp<uint32_t>(static_cast<uint32_t>(r.width()), 32, 1024);
    poly_.resize(steps);
    cols_.resize(steps);
    lvl_.resize(steps);
    const ui::ColorLut& lut = ui::colormapLut(ui::Colormap::Ice);
    const float range = kSpecCeilDb - kSpecFloorDb;
    const float h = r.height() - ctx.scale.exact(4.0f);
    bool any = false;
    for (uint32_t s = 0; s < steps; ++s) {
      const float x = r.x0 + r.width() * static_cast<float>(s) / static_cast<float>(steps - 1);
      // The analyser point under this column (its axis may differ from the display's).
      const float hz = hzAt(x, hzToX);
      const float t = std::clamp(feed_->axisT(hz), 0.0f, 1.0f) * static_cast<float>(P - 1);
      const uint32_t i0 = std::min(P - 2, static_cast<uint32_t>(t));
      const float f = t - static_cast<float>(i0);
      const float db = shown_[i0] + (shown_[i0 + 1] - shown_[i0]) * f;
      const float lt = std::clamp((db - kSpecFloorDb) / range, 0.0f, 1.0f);
      const float vis = std::clamp((db - kSpecFloorDb) / std::max(1.0f, 0.04f * range), 0.0f, 1.0f);
      any = any || vis > 0.0f;
      poly_[s] = {x, r.y1 - lt * h};
      lvl_[s] = lt;
      cols_[s] = ui::mulAlpha(ui::lutAt(lut, 0.25f + 0.75f * lt), vis);
    }
    if (!any) return;
    ui::SpectrumCurveStyle st;
    st.glowAmount = glow;
    st.fillAlpha = fillAlpha;
    ui::spectrumCurve(ctx.dl, poly_.data(), cols_.data(), lvl_.data(), steps, r.y1, st);
  }

private:
  template <class HzToX>
  static float hzAt(float x, HzToX hzToX) {
    // hzToX is monotonic and logarithmic: invert from its two ends.
    const float lo = 10.0f, hi = 22000.0f;
    const float x0 = hzToX(lo), x1 = hzToX(hi);
    const float u = x1 > x0 ? (x - x0) / (x1 - x0) : 0.0f;
    return lo * std::pow(hi / lo, u);
  }
  void acquire() {
    if (acquired_ || !feed_) return;
    feed_->acquire();
    acquired_ = true;
    last_ = 0.0;
  }
  void release() {
    if (!acquired_) return;
    feed_->release();
    acquired_ = false;
  }
  SpectrumFeed* feed_;
  bool acquired_ = false;
  uint64_t seq_ = 0;
  double last_ = 0.0;
  std::vector<float> target_, shown_;
  std::vector<ui::Vec2> poly_;
  std::vector<ui::Rgba> cols_;
  std::vector<float> lvl_;
};

// Log frequency axis of a display, 10 Hz … 22 kHz (the analyser's axis in EQ Eight's panel).
struct FreqAxis {
  float x0 = 0, x1 = 1;
  float x(float hz) const noexcept { return x0 + (x1 - x0) * std::log(std::max(hz, 1.0f) / 10.0f) / std::log(2200.0f); }
  float hz(float px) const noexcept { return 10.0f * std::pow(2200.0f, std::clamp((px - x0) / (x1 - x0), 0.0f, 1.0f)); }
};

void drawFreqGrid(ui::PaintContext& ctx, const ui::Theme& th, const ui::Rect& r, const FreqAxis& ax, bool labels) {
  for (float f : {20.0f, 30.0f, 50.0f, 100.0f, 200.0f, 300.0f, 500.0f, 1000.0f, 2000.0f, 3000.0f, 5000.0f, 10000.0f, 20000.0f}) {
    const float x = std::round(ax.x(f));
    const bool major = f == 100.0f || f == 1000.0f || f == 10000.0f;
    ctx.dl.addRectFilled({x, r.y0, x + 1.0f, r.y1}, major ? th.device.displayGridMajor : th.device.displayGrid);
  }
  if (!labels || !ctx.res.uiFonts) return;
  const int32_t lh = ctx.scale.px(12.0f);
  for (float f : {100.0f, 1000.0f, 10000.0f}) {
    const float x = std::round(ax.x(f));
    const char* t = f == 100.0f ? "100" : f == 1000.0f ? "1k" : "10k";
    const ui::IRect box{static_cast<int32_t>(x) + ctx.scale.px(3.0f), static_cast<int32_t>(r.y1) - lh - ctx.scale.px(1.0f), ctx.scale.px(28.0f), lh};
    paint::text(ctx, ctx.res.uiFonts->small, box, t, th.device.displayText, ui::TextAlign::Left);
  }
}

ui::IRect insetRect(const ui::IRect& r, int32_t d) { return paint::inset(r, d); }

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
class SpectrumDisplay final : public PanelDisplay {
public:
  SpectrumDisplay(DevicePanel::Impl& impl, const ControlDesc& d) : PanelDisplay(d.id, impl, d), layer_(impl.spectrum()) {}
  bool dependsOn(std::string_view) const override { return false; }
  void onFrame(double now) override {
    layer_.setFeed(impl_.spectrum());
    if (layer_.frame(*this, now)) markDirty();
  }
  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    layer_.setFeed(impl_.spectrum());
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, ctx.scale.exact(2.0f), th->device.display, th->device.border);
    const ui::Rect f = insetRect(r, 1).toRect();
    FreqAxis ax{f.x0, f.x1};
    ctx.dl.pushClip(f);
    for (float db = -12.0f; db > kSpecFloorDb; db -= 12.0f) {
      const float y = std::round(f.y1 - (db - kSpecFloorDb) / (kSpecCeilDb - kSpecFloorDb) * f.height());
      ctx.dl.addRectFilled({f.x0, y, f.x1, y + 1.0f}, th->device.displayGrid);
    }
    drawFreqGrid(ctx, *th, f, ax, true);
    layer_.paint(ctx, f, [&](float hz) { return ax.x(hz); }, 1.0f, 0.5f);
    ctx.dl.popClip();
    layer_.wake(*this);
  }
  ui::InfoText info() const override { return {"Spectrum", "The device's output spectrum (PQ Meter's analyser)."}; }

private:
  SpectrumLayer layer_;
};

// ---- EQ Eight: response, band dots, spectrum ------------------------------------------------------------------------------
class EqDisplay final : public PanelDisplay {
public:
  EqDisplay(DevicePanel::Impl& impl, const ControlDesc& d) : PanelDisplay(d.id, impl, d), layer_(impl.spectrum()) {
    if (const auto it = d.bind.find("@bands"); it != d.bind.end()) bandSel_ = it->second;
    if (const auto it = d.bind.find("@range"); it != d.bind.end()) rangeDb_ = std::max(1.0f, std::stof(it->second));
    rebind();
  }

  void rebind() override {
    const SelectorDesc* s = impl_.panel.desc().findSelector(bandSel_);
    bands_.clear();
    deps_.clear();
    if (!s) return;
    for (const std::string& v : s->values) {
      NameMap m = impl_.selValues;
      m[bandSel_] = v;
      Band b;
      b.label = v;
      b.freq = role("freq", m);
      b.gain = role("gain", m);
      b.q = role("q", m);
      b.type = role("type", m);
      b.on = role("on", m);
      for (const std::string* n : {&b.freq, &b.gain, &b.q, &b.type, &b.on})
        if (!n->empty()) deps_.insert(*n);
      bands_.push_back(std::move(b));
    }
    for (const char* r : {"scale", "adaptive", "output", "hq", "mode"}) {
      std::string n = role(r, impl_.selValues);
      if (!n.empty()) deps_.insert(n);
      globals_[r] = std::move(n);
    }
    dirty_ = true;
  }
  bool dependsOn(std::string_view name) const override { return deps_.count(std::string(name)) != 0; }
  void valuesChanged() override {
    dirty_ = true;
    markDirty();
  }
  ui::InfoText info() const override {
    return {"EQ Display", "Drag a band's dot to change its frequency and gain (Shift: fine; Alt or the wheel: Q). Click selects a "
                          "band; a double-click switches it on or off. The output spectrum is drawn behind the curve."};
  }

  void onFrame(double now) override {
    layer_.setFeed(impl_.spectrum());
    if (layer_.frame(*this, now)) markDirty();
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    layer_.setFeed(impl_.spectrum());
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, ctx.scale.exact(2.0f), th->device.display, th->device.border);
    const ui::Rect f = insetRect(r, 1).toRect();
    axis_ = {f.x0, f.x1};
    plot_ = f;
    ctx.dl.pushClip(f);
    // Grid: frequencies, then gain lines (0 dB stronger).
    drawFreqGrid(ctx, *th, f, axis_, true);
    for (float db : {-12.0f, -6.0f, 6.0f, 12.0f}) {
      if (std::fabs(db) > rangeDb_) continue;
      const float y = std::round(yOf(db));
      ctx.dl.addRectFilled({f.x0, y, f.x1, y + 1.0f}, th->device.displayGrid);
    }
    ctx.dl.addRectFilled({f.x0, std::round(yOf(0.0f)), f.x1, std::round(yOf(0.0f)) + 1.0f}, th->device.displayGridMajor);
    // Spectrum behind everything else (subdued: the curve is the point of this display).
    layer_.paint(ctx, f, [&](float hz) { return axis_.x(hz); }, 0.6f, 0.35f);
    // Response.
    const uint32_t n = static_cast<uint32_t>(std::max(16.0f, f.width() / ctx.scale.exact(1.5f)));
    if (dirty_ || resp_.size() != n) compute(n);
    pts_.resize(n);
    const float y0 = yOf(0.0f);
    for (uint32_t i = 0; i < n; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(n - 1);
      pts_[i] = {f.x0 + u * f.width(), std::clamp(yOf(resp_[i]), f.y0 - 2.0f, f.y1 + 2.0f)};
    }
    // The selected band's own response, filled faintly toward 0 dB (Live shows the band being edited).
    const int32_t sel = impl_.panel.selected(bandSel_);
    if (sel >= 0 && sel < static_cast<int32_t>(bands_.size()) && selResp_.size() == n) {
      for (uint32_t i = 0; i + 1 < n; ++i) {
        const float xa = pts_[i].x, xb = pts_[i + 1].x;
        const float ya = std::clamp(yOf(selResp_[i]), f.y0, f.y1), yb = std::clamp(yOf(selResp_[i + 1]), f.y0, f.y1);
        const ui::Vec2 q[4] = {{xa, y0}, {xa, ya}, {xb, yb}, {xb, y0}};
        if (std::fabs(ya - y0) + std::fabs(yb - y0) > 0.25f) ctx.dl.addConvexPolyFilled(q, th->device.curveFill);
      }
    }
    ui::smoothPolyline(ctx.dl, pts_.data(), n, th->device.curve, ctx.scale.exact(1.6f));
    // Band dots.
    const float rad = ctx.scale.exact(kDotRadius);
    for (size_t b = 0; b < bands_.size(); ++b) {
      const ui::Vec2 c = dotPos(b);
      const bool on = bands_[b].on.empty() || impl_.value(bands_[b].on) >= 0.5;
      const bool selected = static_cast<int32_t>(b) == sel;
      const bool hot = static_cast<int32_t>(b) == hover_ || static_cast<int32_t>(b) == drag_;
      if (selected || hot) ctx.dl.addCircleFilled(c, rad + ctx.scale.exact(1.5f), th->device.bandSelected);
      ctx.dl.addCircleFilled(c, rad, on ? th->device.band : th->device.bandOff);
      if (ctx.res.uiFonts) {
        const int32_t s = static_cast<int32_t>(std::lround(2.0f * rad));
        const ui::IRect box{static_cast<int32_t>(std::lround(c.x - rad)), static_cast<int32_t>(std::lround(c.y - rad)), s, s};
        paint::text(ctx, ctx.res.uiFonts->small, box, bands_[b].label, th->device.bandText, ui::TextAlign::Center);
      }
    }
    ctx.dl.popClip();
    layer_.wake(*this);
  }

  bool onPointer(const ui::PointerEvent& e) override {
    switch (e.type) {
      case ui::PointerType::Move: {
        if (drag_ >= 0) {
          dragTo(e.pos, e.mods);
          return true;
        }
        const int32_t h = dotAt(e.pos);
        if (h != hover_) {
          hover_ = h;
          markDirty();
        }
        return false;
      }
      case ui::PointerType::Down: {
        if (e.button != ui::PointerButton::Left) return false;
        const int32_t b = dotAt(e.pos);
        if (b < 0) return false;
        impl_.userSelect(bandSel_, b);
        if (e.clickCount == 2 && !bands_[static_cast<size_t>(b)].on.empty()) {
          const std::string& on = bands_[static_cast<size_t>(b)].on;
          impl_.userConst(bare(on), impl_.value(on) >= 0.5 ? 0.0 : 1.0, this);
          return true;
        }
        drag_ = b;
        last_ = e.pos;
        const Band& bd = bands_[static_cast<size_t>(b)];
        if (!bd.freq.empty()) impl_.userParamBegin(bd.freq);
        if (!bd.gain.empty()) impl_.userParamBegin(bd.gain);
        if (!bd.q.empty()) impl_.userParamBegin(bd.q);
        markDirty();
        return true;
      }
      case ui::PointerType::Up: {
        if (drag_ < 0 || e.button != ui::PointerButton::Left) return false;
        endDrag();
        return true;
      }
      case ui::PointerType::Wheel: {
        const int32_t b = dotAt(e.pos) >= 0 ? dotAt(e.pos) : impl_.panel.selected(bandSel_);
        if (b < 0 || b >= static_cast<int32_t>(bands_.size())) return false;
        const Band& bd = bands_[static_cast<size_t>(b)];
        if (const ParamInfo* p = impl_.paramInfo(bd.q)) {
          const double pos = std::clamp(toPosition(*p, impl_.value(bd.q)) + 0.02 * e.wheel.y, 0.0, 1.0);
          impl_.userParamBegin(bd.q);
          impl_.userParam(bd.q, fromPosition(*p, pos), this);
          impl_.userParamEnd(bd.q);
          return true;
        }
        return false;
      }
      default: return false;
    }
  }
  void onCaptureLost() override {
    if (drag_ >= 0) endDrag();
  }
  void onHoverChanged(bool h) override {
    if (!h && hover_ >= 0) {
      hover_ = -1;
      markDirty();
    }
  }

  // Tests: where band b's dot is drawn (device px), after a paint.
  ui::Vec2 dotPos(size_t b) const {
    const Band& bd = bands_[b];
    const float x = axis_.x(static_cast<float>(impl_.value(bd.freq)));
    float db = 0.0f;
    const int type = static_cast<int>(std::lround(impl_.value(bd.type)));
    if (type == math::kLowShelf || type == math::kBell || type == math::kHighShelf) {
      const double g = impl_.value(bd.gain);
      db = static_cast<float>(20.0 * std::log10(std::max(g, 1e-9)) * globalValue("scale", 1.0));
    }
    return {x, std::clamp(yOf(db), plot_.y0 + 2.0f, plot_.y1 - 2.0f)};
  }

private:
  static constexpr float kDotRadius = 6.5f;
  struct Band {
    std::string label, freq, gain, q, type, on;  // resolved names (consts with their '#')
  };
  // The resolved name bound to a role ("#name" for a const, as Impl::value and the change keys take it), or "".
  std::string role(const char* r, const NameMap& m) const {
    const auto it = desc_.bind.find(r);
    if (it == desc_.bind.end() || it->second.empty()) return {};
    const std::string& t = it->second;
    if (t[0] == '#') return "#" + resolveTemplate(std::string_view(t).substr(1), m);
    return resolveTemplate(t, m);
  }
  double globalValue(const char* r, double def) const {
    const auto it = globals_.find(r);
    if (it == globals_.end() || it->second.empty()) return def;
    return impl_.value(it->second);
  }
  static std::string bare(const std::string& n) { return !n.empty() && n[0] == '#' ? n.substr(1) : n; }
  float yOf(float db) const noexcept {
    const float mid = 0.5f * (plot_.y0 + plot_.y1);
    return mid - db / rangeDb_ * (0.5f * plot_.height() - 6.0f);
  }
  int32_t dotAt(ui::Vec2 p) const {
    const float s = tree() ? tree()->scale() : 1.0f;
    const float r2 = (kDotRadius + 3.0f) * s * (kDotRadius + 3.0f) * s;
    int32_t best = -1;
    float bestD = r2;
    for (size_t b = 0; b < bands_.size(); ++b) {
      const ui::Vec2 c = dotPos(b);
      const float d = (c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y);
      if (d <= bestD) {
        bestD = d;
        best = static_cast<int32_t>(b);
      }
    }
    return best;
  }
  void dragTo(ui::Vec2 p, ui::Mods mods) {
    const Band& bd = bands_[static_cast<size_t>(drag_)];
    const float k = (mods & ui::mod::kShift) ? 0.1f : 1.0f;
    const float dx = (p.x - last_.x) * k, dy = (p.y - last_.y) * k;
    last_ = p;
    if (mods & ui::mod::kAlt) {  // Q
      if (const ParamInfo* q = impl_.paramInfo(bd.q)) {
        const double pos = std::clamp(toPosition(*q, impl_.value(bd.q)) - dy / 150.0, 0.0, 1.0);
        impl_.userParam(bd.q, fromPosition(*q, pos), this);
      }
      return;
    }
    if (const ParamInfo* fp = impl_.paramInfo(bd.freq)) {
      const double cur = toPosition(*fp, impl_.value(bd.freq));
      // The axis spans 10 Hz … 22 kHz; the param's own range may differ (position is log in both).
      const double perPx = std::log(2200.0) / std::log(fp->max / fp->min) / std::max(1.0f, plot_.width());
      impl_.userParam(bd.freq, fromPosition(*fp, std::clamp(cur + dx * perPx, 0.0, 1.0)), this);
    }
    const int type = static_cast<int>(std::lround(impl_.value(bd.type)));
    if (type == math::kLowShelf || type == math::kBell || type == math::kHighShelf)
      if (const ParamInfo* gp = impl_.paramInfo(bd.gain)) {
        const double scale = globalValue("scale", 1.0);
        const double dbPerPx = rangeDb_ / std::max(1.0f, 0.5f * plot_.height() - 6.0f) / std::max(0.05, std::fabs(scale));
        const double db = toDisplay(*gp, impl_.value(bd.gain)) - dy * dbPerPx * (scale < 0 ? -1.0 : 1.0);
        impl_.userParam(bd.gain, std::clamp(fromDisplay(*gp, db), gp->min, gp->max), this);
      }
  }
  void endDrag() {
    const Band& bd = bands_[static_cast<size_t>(drag_)];
    drag_ = -1;
    if (!bd.freq.empty()) impl_.userParamEnd(bd.freq);
    if (!bd.gain.empty()) impl_.userParamEnd(bd.gain);
    if (!bd.q.empty()) impl_.userParamEnd(bd.q);
    markDirty();
  }
  void compute(uint32_t n) {
    resp_.assign(n, 0.0f);
    selResp_.assign(n, 0.0f);
    const double sr = impl_.sampleRate();
    const double os = globalValue("hq", 1.0) >= 1.5 ? 2.0 : 1.0;
    const double scale = globalValue("scale", 1.0);
    const bool adaptive = globalValue("adaptive", 0.0) >= 0.5;
    const double outDb = 20.0 * std::log10(std::max(globalValue("output", 1.0), 1e-9));
    const int32_t sel = impl_.panel.selected(bandSel_);
    std::vector<math::EqBand> bs(bands_.size());
    std::vector<math::EqCoef> cs(bands_.size());
    for (size_t b = 0; b < bands_.size(); ++b) {
      const Band& bd = bands_[b];
      bs[b].on = bd.on.empty() || impl_.value(bd.on) >= 0.5;
      bs[b].type = static_cast<int>(std::lround(impl_.value(bd.type)));
      bs[b].freq = impl_.value(bd.freq);
      bs[b].gain = impl_.value(bd.gain);
      bs[b].q = impl_.value(bd.q);
      cs[b] = math::eqCoef(bs[b], adaptive, scale, sr, os);
    }
    for (uint32_t i = 0; i < n; ++i) {
      const double hz = axis_.hz(plot_.x0 + plot_.width() * static_cast<float>(i) / static_cast<float>(n - 1));
      std::complex<double> h = 1.0;
      const double wf = math::eqWarp(hz, sr, os);
      for (size_t b = 0; b < bs.size(); ++b) {
        const std::complex<double> hb = math::eqBandResponseWarped(bs[b], cs[b], wf);
        h *= hb;
        if (static_cast<int32_t>(b) == sel) selResp_[i] = static_cast<float>(20.0 * std::log10(std::max(std::abs(hb), 1e-9)));
      }
      resp_[i] = static_cast<float>(20.0 * std::log10(std::max(std::abs(h), 1e-9)) + outDb);
    }
    dirty_ = false;
  }

  SpectrumLayer layer_;
  std::string bandSel_ = "band";
  float rangeDb_ = 15.0f;
  std::vector<Band> bands_;
  std::map<std::string, std::string, std::less<>> globals_;
  std::set<std::string, std::less<>> deps_;
  std::vector<float> resp_, selResp_;
  std::vector<ui::Vec2> pts_;
  FreqAxis axis_;
  ui::Rect plot_{0, 0, 1, 1};
  bool dirty_ = true;
  int32_t hover_ = -1, drag_ = -1;
  ui::Vec2 last_{0, 0};
};

}  // namespace

std::unique_ptr<PanelDisplay> makeDisplay(DevicePanel::Impl& impl, const ControlDesc& desc) {
  if (desc.visual == "transfer") return std::make_unique<TransferDisplay>(impl, desc);
  if (desc.visual == "eq") return std::make_unique<EqDisplay>(impl, desc);
  return std::make_unique<SpectrumDisplay>(impl, desc);
}

}  // namespace pq::views::devices
