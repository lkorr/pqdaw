// Operator's panel displays (T4.12): the multi-stage envelope with draggable points, the algorithm picker and the
// user waveform's partial editor. Like every panel display they read and write the device through DevicePanel::Impl
// (params: one gesture per drag; consts: a structural edit) and repaint only on a value change of what they bind.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>

#include "../common/paint.h"
#include "panel_internal.h"
#include "pq/ui/lines.h"
#include "pq/ui/theme.h"

namespace pq::views::devices {

namespace {

// The resolved name bound to a display role ("#name" for a const), or "".
std::string roleName(const DevicePanel::Impl& impl, const ControlDesc& d, const char* r) {
  const auto it = d.bind.find(r);
  if (it == d.bind.end() || it->second.empty()) return {};
  const std::string& t = it->second;
  if (t[0] == '#') return "#" + impl.resolve(std::string_view(t).substr(1));
  return impl.resolve(t);
}
std::string bareName(const std::string& n) { return !n.empty() && n[0] == '#' ? n.substr(1) : n; }
ui::IRect toIRect(const ui::Rect& r) {
  const int32_t x0 = static_cast<int32_t>(std::lround(r.x0)), y0 = static_cast<int32_t>(std::lround(r.y0));
  return {x0, y0, static_cast<int32_t>(std::lround(r.x1)) - x0, static_cast<int32_t>(std::lround(r.y1)) - y0};
}

// ---- the envelope ----------------------------------------------------------------------------------------------------
// msenv's shape (stdlib/envelopes/msenv.pqp): Initial → (attack, linear) Peak → (decay, −80 dB after d) Sustain, held
// for a while, → (release) End. Four points: Initial (vertical), Peak (attack time, level), Sustain (decay time,
// level), End (release time, level). Times drag multiplicatively (a doubling per 50 px: 1 ms … 60 s in reach);
// levels in dB for amplitude envelopes (`@db`: −70 dB = −inf at the bottom), else linear over the param's range.
class EnvelopeDisplay final : public PanelDisplay {
public:
  EnvelopeDisplay(DevicePanel::Impl& impl, const ControlDesc& d) : PanelDisplay(d.id, impl, d) {
    if (const auto it = d.bind.find("@db"); it != d.bind.end()) db_ = it->second == "1";
    rebind();
  }
  void rebind() override {
    deps_.clear();
    for (size_t i = 0; i < kRoles; ++i) {
      names_[i] = roleName(impl_, desc_, kRoleNames[i]);
      if (!names_[i].empty()) deps_.insert(names_[i]);
    }
    markDirty();
  }
  bool dependsOn(std::string_view name) const override { return deps_.count(std::string(name)) != 0; }
  ui::InfoText info() const override {
    return {"Envelope", "Initial, Peak, Sustain and End levels joined by the attack (linear), decay and release (exponential, "
                        "−80 dB after their times). Drag a point: sideways changes its time, up or down its level (Shift: fine)."};
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, ctx.scale.exact(2.0f), th->device.display, th->device.border);
    const float pad = ctx.scale.exact(6.0f);
    plot_ = {static_cast<float>(r.x) + pad, static_cast<float>(r.y) + pad, static_cast<float>(r.right()) - pad,
             static_cast<float>(r.bottom()) - pad};
    ctx.dl.pushClip(r.toRect());
    // Level grid: −6 dB steps (amplitude) or quarters (bipolar), the zero line stronger.
    if (db_) {
      for (float d = -12.0f; d > -70.0f; d -= 12.0f) {
        const float y = std::round(yOfDb(d));
        ctx.dl.addRectFilled({plot_.x0, y, plot_.x1, y + 1.0f}, th->device.displayGrid);
      }
    } else {
      for (float v : {-0.5f, 0.5f}) {
        const float y = std::round(yOfLevel(v, kInit));
        ctx.dl.addRectFilled({plot_.x0, y, plot_.x1, y + 1.0f}, th->device.displayGrid);
      }
      const float y0 = std::round(yOfLevel(0.0f, kInit));
      ctx.dl.addRectFilled({plot_.x0, y0, plot_.x1, y0 + 1.0f}, th->device.displayGridMajor);
    }
    if (drag_ < 0) scaleFromValues();
    // The curve.
    const uint32_t n = static_cast<uint32_t>(std::max(32.0f, plot_.width() / ctx.scale.exact(1.5f)));
    pts_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      const double t = total_ * static_cast<double>(i) / static_cast<double>(n - 1);
      pts_[i] = {xOf(t), std::clamp(yOfLevel(static_cast<float>(levelAt(t)), kPeak), plot_.y0, plot_.y1)};
    }
    // Fill under the curve (to the bottom for amplitude, to zero for bipolar): one pixel column each (no seams between
    // anti-aliased slices), then the line.
    const float base = db_ ? plot_.y1 : yOfLevel(0.0f, kInit);
    for (float x = std::floor(plot_.x0); x < plot_.x1; x += 1.0f) {
      const double t = total_ * static_cast<double>((x + 0.5f - plot_.x0) / std::max(plot_.width(), 1.0f));
      const float y = std::clamp(yOfLevel(static_cast<float>(levelAt(t)), kPeak), plot_.y0, plot_.y1);
      ctx.dl.addRectFilled({x, std::min(y, base), x + 1.0f, std::max(y, base)}, th->device.curveFill);
    }
    ui::smoothPolyline(ctx.dl, pts_.data(), n, th->device.curve, ctx.scale.exact(1.6f));
    // The sustain hold, dashed: the note is held for as long as it is held.
    const float xs0 = xOf(value(kA) + value(kD)), xs1 = xOf(value(kA) + value(kD) + hold_);
    for (float x = xs0; x < xs1; x += ctx.scale.exact(6.0f))
      ctx.dl.addRectFilled({x, plot_.y0, std::min(x + ctx.scale.exact(2.0f), xs1), plot_.y0 + ctx.scale.exact(1.0f)}, th->device.displayGrid);
    // Points.
    const float rad = ctx.scale.exact(kDotRadius);
    for (int p = 0; p < 4; ++p) {
      const ui::Vec2 c = pointPos(p);
      if (p == hover_ || p == drag_) ctx.dl.addCircleFilled(c, rad + ctx.scale.exact(1.5f), th->device.bandSelected);
      ctx.dl.addCircleFilled(c, rad, th->device.band);
    }
    // Loop mode.
    const std::string& m = names_[kMode];
    if (!m.empty() && ctx.res.uiFonts) {
      const int mode = static_cast<int>(std::lround(impl_.value(m)));
      static const char* kModes[] = {"", "Loop", "Beat", "Sync", "Trigger"};
      if (mode > 0 && mode < 5) {
        const int32_t lh = ctx.scale.px(12.0f);
        const ui::IRect box{r.right() - ctx.scale.px(54.0f), r.y + ctx.scale.px(2.0f), ctx.scale.px(50.0f), lh};
        paint::text(ctx, ctx.res.uiFonts->small, box, kModes[mode], th->device.displayText, ui::TextAlign::Right);
      }
    }
    ctx.dl.popClip();
  }

  bool onPointer(const ui::PointerEvent& e) override {
    switch (e.type) {
      case ui::PointerType::Move: {
        if (drag_ >= 0) {
          dragTo(e.pos, e.mods);
          return true;
        }
        const int32_t h = pointAt(e.pos);
        if (h != hover_) {
          hover_ = h;
          markDirty();
        }
        return false;
      }
      case ui::PointerType::Down: {
        if (e.button != ui::PointerButton::Left) return false;
        const int32_t p = pointAt(e.pos);
        if (p < 0) return false;
        drag_ = p;
        start_ = e.pos;
        for (int k : paramsOf(p))
          if (!names_[static_cast<size_t>(k)].empty()) {
            startValue_[static_cast<size_t>(k)] = value(k);
            impl_.userParamBegin(names_[static_cast<size_t>(k)]);
          }
        markDirty();
        return true;
      }
      case ui::PointerType::Up:
        if (drag_ < 0 || e.button != ui::PointerButton::Left) return false;
        endDrag();
        return true;
      default: return false;
    }
  }
  void onCaptureLost() override {
    if (drag_ >= 0) endDrag();
  }
  // T4.16c: a breakpoint moves.
  ui::Cursor cursorAt(ui::Vec2) const override { return drag_ >= 0 || hover_ >= 0 ? ui::Cursor::Move : ui::Cursor::Inherit; }
  void onHoverChanged(bool h) override {
    if (!h && hover_ >= 0) {
      hover_ = -1;
      markDirty();
    }
  }

  // Tests: point p's centre (device px), after a paint.
  ui::Vec2 pointPos(int p) const {
    switch (p) {
      case 0: return {xOf(0.0), yOfLevel(static_cast<float>(value(kInit)), kInit)};
      case 1: return {xOf(value(kA)), yOfLevel(static_cast<float>(value(kPeak)), kPeak)};
      case 2: return {xOf(value(kA) + value(kD)), yOfLevel(static_cast<float>(value(kSus)), kSus)};
      default: return {xOf(value(kA) + value(kD) + hold_ + value(kR)), yOfLevel(static_cast<float>(value(kEnd)), kEnd)};
    }
  }

private:
  enum Role : int { kA, kD, kR, kInit, kPeak, kSus, kEnd, kMode, kRoles };
  static constexpr const char* kRoleNames[kRoles] = {"a", "d", "r", "init", "peak", "sus", "end", "mode"};
  static constexpr float kDotRadius = 4.5f;

  double value(int role) const {
    const std::string& n = names_[static_cast<size_t>(role)];
    if (n.empty()) return role == kPeak || role == kSus ? 1.0 : 0.0;
    return impl_.value(n);
  }
  // Per point: [time role, level role].
  static std::array<int, 2> paramsOf(int p) {
    switch (p) {
      case 0: return {-1, kInit};
      case 1: return {kA, kPeak};
      case 2: return {kD, kSus};
      default: return {kR, kEnd};
    }
  }
  void scaleFromValues() {
    const double a = value(kA), d = value(kD), r = value(kR);
    hold_ = 0.25 * std::max(a + d + r, 0.02);
    total_ = (a + d + hold_ + r) * 1.04;
  }
  float xOf(double t) const noexcept { return plot_.x0 + static_cast<float>(t / std::max(total_, 1e-9)) * plot_.width(); }
  float yOfDb(float dbv) const noexcept { return plot_.y0 + (-dbv / 70.0f) * plot_.height(); }
  // Level → y: dB for amplitude envelopes, else linear over the role's param range (bipolar −1 … 1).
  float yOfLevel(float v, int role) const {
    if (db_) return yOfDb(std::clamp(v > 0.0f ? 20.0f * std::log10(v) : -70.0f, -70.0f, 0.0f));
    const ParamInfo* p = impl_.paramInfo(names_[static_cast<size_t>(role)]);
    const float lo = p ? static_cast<float>(p->min) : -1.0f, hi = p ? static_cast<float>(p->max) : 1.0f;
    return plot_.y1 - (std::clamp(v, lo, hi) - lo) / std::max(hi - lo, 1e-6f) * plot_.height();
  }
  double levelAt(double t) const {
    const double a = value(kA), d = value(kD), r = value(kR);
    const double i = value(kInit), pk = value(kPeak), s = value(kSus), e = value(kEnd);
    auto decayAt = [&](double u) { return s + (pk - s) * std::exp(-9.210340371976184 * u / std::max(d, 1e-6)); };
    if (t < a) return i + (pk - i) * t / std::max(a, 1e-9);
    const double tr = a + d + hold_;
    if (t < tr) return decayAt(t - a);
    const double l0 = decayAt(tr - a);
    return e + (l0 - e) * std::exp(-9.210340371976184 * (t - tr) / std::max(r, 1e-6));
  }
  int32_t pointAt(ui::Vec2 p) const {
    const float sc = tree() ? tree()->scale() : 1.0f;
    const float r2 = (kDotRadius + 4.0f) * sc * (kDotRadius + 4.0f) * sc;
    int32_t best = -1;
    float bestD = r2;
    for (int i = 0; i < 4; ++i) {
      const ui::Vec2 c = pointPos(i);
      const float dd = (c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y);
      if (dd <= bestD) {
        bestD = dd;
        best = i;
      }
    }
    return best;
  }
  void dragTo(ui::Vec2 p, ui::Mods mods) {
    const float sc = tree() ? tree()->scale() : 1.0f;
    const float k = (mods & ui::mod::kShift) ? 0.1f : 1.0f;
    const float dx = (p.x - start_.x) * k / sc, dy = (p.y - start_.y) * k / sc;
    const auto [tr, lr] = paramsOf(drag_);
    if (tr >= 0 && !names_[static_cast<size_t>(tr)].empty())
      if (const ParamInfo* pi = impl_.paramInfo(names_[static_cast<size_t>(tr)])) {
        double t = std::max(startValue_[static_cast<size_t>(tr)], 0.001) * std::exp2(dx / 50.0);
        if (t < 0.0005 && pi->min <= 0.0) t = 0.0;
        impl_.userParam(names_[static_cast<size_t>(tr)], std::clamp(t, pi->min, pi->max), this);
      }
    if (!names_[static_cast<size_t>(lr)].empty())
      if (const ParamInfo* pi = impl_.paramInfo(names_[static_cast<size_t>(lr)])) {
        const double v0 = startValue_[static_cast<size_t>(lr)];
        double v;
        if (db_) {
          const double d0 = v0 > 0.0 ? 20.0 * std::log10(v0) : -70.0;
          const double dbv = std::clamp(d0 - dy * 70.0 / std::max(1.0f, plot_.height() / sc), -70.0, 0.0);
          v = std::pow(10.0, dbv / 20.0);
        } else {
          v = v0 - dy * (pi->max - pi->min) / std::max(1.0f, plot_.height() / sc);
        }
        impl_.userParam(names_[static_cast<size_t>(lr)], std::clamp(v, pi->min, pi->max), this);
      }
    markDirty();
  }
  void endDrag() {
    const int32_t p = drag_;
    drag_ = -1;
    for (int k : paramsOf(p))
      if (k >= 0 && !names_[static_cast<size_t>(k)].empty()) impl_.userParamEnd(names_[static_cast<size_t>(k)]);
    markDirty();
  }

  std::array<std::string, kRoles> names_;
  std::array<double, kRoles> startValue_{};
  std::set<std::string, std::less<>> deps_;
  std::vector<ui::Vec2> pts_;
  ui::Rect plot_{0, 0, 1, 1};
  double hold_ = 0.1, total_ = 1.0;
  bool db_ = false;
  int32_t hover_ = -1, drag_ = -1;
  ui::Vec2 start_{0, 0};
};

// ---- the algorithm picker --------------------------------------------------------------------------------------------
// The 11 algorithms of stdlib/devices/operator.pqp as diagrams (D is on top; an arrow is "modulates"; carriers feed the
// output bar); click one to choose it (a structural edit). Operators that are off are drawn dimmed.
struct AlgoLayout {
  std::array<std::array<float, 2>, 4> pos;  // A, B, C, D: (column, row) in a 4 × 4 grid, row 0 at the top
  std::array<uint8_t, 6> edges;             // modulator << 2 | target, 0xFF = none
  uint8_t carriers;                         // bit per operator (A = 1)
};
constexpr uint8_t E(int m, int t) { return static_cast<uint8_t>(m << 2 | t); }
constexpr uint8_t kNoEdge = 0xFF;
constexpr int A = 0, B = 1, C = 2, D = 3;
constexpr AlgoLayout kAlgos[11] = {
    {{{{1.5f, 3}, {1.5f, 2}, {1.5f, 1}, {1.5f, 0}}}, {E(D, C), E(C, B), E(B, A), kNoEdge, kNoEdge, kNoEdge}, 0b0001},
    {{{{1.5f, 2}, {1.5f, 1}, {1, 0}, {2, 0}}}, {E(C, B), E(D, B), E(B, A), kNoEdge, kNoEdge, kNoEdge}, 0b0001},
    {{{{1.5f, 2}, {2, 1}, {1, 1}, {1, 0}}}, {E(D, C), E(C, A), E(B, A), kNoEdge, kNoEdge, kNoEdge}, 0b0001},
    {{{{1.5f, 2}, {1, 1}, {1, 0}, {2, 1}}}, {E(C, B), E(B, A), E(D, A), kNoEdge, kNoEdge, kNoEdge}, 0b0001},
    {{{{1.5f, 2}, {0.5f, 1}, {1.5f, 1}, {2.5f, 1}}}, {E(B, A), E(C, A), E(D, A), kNoEdge, kNoEdge, kNoEdge}, 0b0001},
    {{{{1, 2}, {2, 2}, {2, 1}, {2, 0}}}, {E(D, C), E(C, B), kNoEdge, kNoEdge, kNoEdge, kNoEdge}, 0b0011},
    {{{{2, 2}, {2, 1}, {1, 2}, {1, 1}}}, {E(D, C), E(B, A), kNoEdge, kNoEdge, kNoEdge, kNoEdge}, 0b0101},
    {{{{2.2f, 2}, {1.2f, 2}, {0.7f, 1}, {1.7f, 1}}}, {E(C, B), E(D, B), kNoEdge, kNoEdge, kNoEdge, kNoEdge}, 0b0011},
    {{{{2.5f, 2}, {1.5f, 2}, {0.5f, 2}, {0.5f, 1}}}, {E(D, C), kNoEdge, kNoEdge, kNoEdge, kNoEdge, kNoEdge}, 0b0111},
    {{{{2.5f, 2}, {1.5f, 2}, {0.5f, 2}, {1.5f, 1}}}, {E(D, C), E(D, B), E(D, A), kNoEdge, kNoEdge, kNoEdge}, 0b0111},
    {{{{0, 2}, {1, 2}, {2, 2}, {3, 2}}}, {kNoEdge, kNoEdge, kNoEdge, kNoEdge, kNoEdge, kNoEdge}, 0b1111},
};

class AlgorithmDisplay final : public PanelDisplay {
public:
  AlgorithmDisplay(DevicePanel::Impl& impl, const ControlDesc& d) : PanelDisplay(d.id, impl, d) { rebind(); }
  void rebind() override {
    algo_ = roleName(impl_, desc_, "algorithm");
    deps_.clear();
    if (!algo_.empty()) deps_.insert(algo_);
    static const char* kOn[4] = {"onA", "onB", "onC", "onD"};
    for (int i = 0; i < 4; ++i) {
      on_[static_cast<size_t>(i)] = roleName(impl_, desc_, kOn[i]);
      if (!on_[static_cast<size_t>(i)].empty()) deps_.insert(on_[static_cast<size_t>(i)]);
    }
    markDirty();
  }
  bool dependsOn(std::string_view name) const override { return deps_.count(std::string(name)) != 0; }
  ui::InfoText info() const override {
    return {"Algorithm", "How the four operators connect: an arrow means the upper operator modulates the phase of the lower "
                         "one; the operators on the bottom line are heard. D, always on top, can modulate itself (Feedback). "
                         "Click an algorithm to choose it (structural: the device recompiles, crossfaded while it plays)."};
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, ctx.scale.exact(2.0f), th->device.display, th->device.border);
    const int sel = selected();
    for (int k = 0; k < 11; ++k) {
      const ui::Rect c = cell(k);
      if (k == sel) paint::box(ctx.dl, toIRect(c), ctx.scale.exact(2.0f), th->device.curveFill, th->device.curve);
      else if (k == hover_) paint::box(ctx.dl, toIRect(c), ctx.scale.exact(2.0f), 0, th->device.displayGridMajor);
      drawAlgo(ctx, *th, k, c, k == sel);
    }
  }

  bool onPointer(const ui::PointerEvent& e) override {
    if (e.type == ui::PointerType::Move) {
      const int h = cellAt(e.pos);
      if (h != hover_) {
        hover_ = h;
        markDirty();
      }
      return false;
    }
    if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left) {
      const int k = cellAt(e.pos);
      if (k < 0 || algo_.empty()) return false;
      if (algo_[0] == '#') impl_.userConst(bareName(algo_), static_cast<double>(k + 1), this);
      else {
        impl_.userParamBegin(algo_);
        impl_.userParam(algo_, static_cast<double>(k + 1), this);
        impl_.userParamEnd(algo_);
      }
      markDirty();
      return true;
    }
    return false;
  }
  void onHoverChanged(bool h) override {
    if (!h && hover_ >= 0) {
      hover_ = -1;
      markDirty();
    }
  }
  ui::Cursor cursorAt(ui::Vec2) const override { return hover_ >= 0 ? ui::Cursor::Pointer : ui::Cursor::Inherit; }  // T4.16c: picks it
  // Tests: the centre of algorithm k's cell (device px).
  ui::Vec2 cellCentre(int k) const {
    const ui::Rect c = cell(k);
    return {0.5f * (c.x0 + c.x1), 0.5f * (c.y0 + c.y1)};
  }

private:
  static constexpr int kCols = 6;
  int selected() const { return algo_.empty() ? 0 : static_cast<int>(std::lround(impl_.value(algo_))) - 1; }
  ui::Rect cell(int k) const {
    const ui::IRect r = rect();
    const float sc = tree() ? tree()->scale() : 1.0f;
    const float pad = 3.0f * sc;
    const float w = (static_cast<float>(r.w) - 2 * pad) / kCols, h = (static_cast<float>(r.h) - 2 * pad) / 2.0f;
    const int col = k % kCols, row = k / kCols;
    const float x0 = static_cast<float>(r.x) + pad + w * static_cast<float>(col), y0 = static_cast<float>(r.y) + pad + h * static_cast<float>(row);
    return {std::round(x0) + 1.0f, std::round(y0) + 1.0f, std::round(x0 + w) - 1.0f, std::round(y0 + h) - 1.0f};
  }
  int cellAt(ui::Vec2 p) const {
    for (int k = 0; k < 11; ++k) {
      const ui::Rect c = cell(k);
      if (p.x >= c.x0 && p.x < c.x1 && p.y >= c.y0 && p.y < c.y1) return k;
    }
    return -1;
  }
  bool isOn(int op) const { return on_[static_cast<size_t>(op)].empty() || impl_.value(on_[static_cast<size_t>(op)]) >= 0.5; }
  void drawAlgo(ui::PaintContext& ctx, const ui::Theme& th, int k, const ui::Rect& c, bool sel) const {
    const AlgoLayout& L = kAlgos[k];
    const float sc = ctx.scale.exact(1.0f);
    // The diagram's grid unit: as large as its own extent (columns, rows + the output bar) allows in the cell, below
    // the cell's number.
    float minC = 9, maxC = -9, maxR = 0;
    for (const auto& p : L.pos) {
      minC = std::min(minC, p[0]);
      maxC = std::max(maxC, p[0]);
      maxR = std::max(maxR, p[1]);
    }
    const float numH = 9.0f * sc;
    const float gw = (c.x1 - c.x0 - 4 * sc) / (maxC - minC + 1.0f), gh = (c.y1 - c.y0 - numH - 2 * sc) / (maxR + 1.5f);
    const float g = std::min(std::min(gw, gh), 14.0f * sc);
    const float bs = std::max(4.0f * sc, 0.68f * g);  // box size
    const float cx = 0.5f * (c.x0 + c.x1) - 0.5f * (minC + maxC) * g;
    const float top = c.y0 + numH + 0.5f * ((c.y1 - c.y0 - numH) - (maxR + 1.5f) * g);
    auto centre = [&](int op) { return ui::Vec2{cx + L.pos[static_cast<size_t>(op)][0] * g, top + L.pos[static_cast<size_t>(op)][1] * g + 0.5f * g}; };
    const ui::Rgba line = sel ? th.device.curve : th.device.label;
    for (uint8_t e : L.edges) {
      if (e == kNoEdge) continue;
      const ui::Vec2 a = centre(e >> 2), b = centre(e & 3);
      const ui::Vec2 seg[2] = {{a.x, a.y + 0.5f * bs}, {b.x, b.y - 0.5f * bs}};
      ui::smoothPolyline(ctx.dl, seg, 2, line, 1.2f * sc);
    }
    // Output bar under the carriers.
    float ox0 = 1e9f, ox1 = -1e9f, oy = 0;
    for (int op = 0; op < 4; ++op)
      if (L.carriers & (1u << op)) {
        const ui::Vec2 p = centre(op);
        ox0 = std::min(ox0, p.x);
        ox1 = std::max(ox1, p.x);
        oy = std::max(oy, p.y + 0.5f * bs + 0.45f * g);
        ctx.dl.addRectFilled({std::round(p.x), p.y + 0.5f * bs, std::round(p.x) + std::max(1.0f, sc), p.y + 0.5f * bs + 0.45f * g}, line);
      }
    ctx.dl.addRectFilled({std::round(ox0), std::round(oy), std::round(ox1) + std::max(1.0f, sc), std::round(oy) + std::max(1.0f, sc)}, line);
    // Feedback on D: a small loop on its right.
    const ui::Vec2 d = centre(D);
    ctx.dl.addArcStroke({d.x + 0.5f * bs, d.y}, 0.28f * bs, -1.5707963f, 1.5707963f, line, 1.0f * sc);
    // Operator boxes.
    for (int op = 0; op < 4; ++op) {
      const ui::Vec2 p = centre(op);
      const ui::Rect b{std::round(p.x - 0.5f * bs), std::round(p.y - 0.5f * bs), std::round(p.x + 0.5f * bs), std::round(p.y + 0.5f * bs)};
      ctx.dl.addRectFilled(b, isOn(op) ? th.device.band : th.device.bandOff);
      if (ctx.res.uiFonts && bs >= 9.0f * sc) {
        static const char* kL[4] = {"A", "B", "C", "D"};
        paint::text(ctx, ctx.res.uiFonts->small, toIRect(b), kL[op], th.device.bandText, ui::TextAlign::Center);
      }
    }
    if (ctx.res.uiFonts) {
      char num[4];
      std::snprintf(num, sizeof num, "%d", k + 1);
      const ui::IRect nb{static_cast<int32_t>(c.x0) + ctx.scale.px(2.0f), static_cast<int32_t>(c.y0), ctx.scale.px(14.0f), ctx.scale.px(11.0f)};
      paint::text(ctx, ctx.res.uiFonts->small, nb, num, sel ? th.device.curve : th.device.displayText, ui::TextAlign::Left);
    }
  }

  std::string algo_;
  std::array<std::string, 4> on_;
  std::set<std::string, std::less<>> deps_;
  int hover_ = -1;
};

// ---- the partial editor ----------------------------------------------------------------------------------------------
// The User waveform's partials (roles "u1" … "u16"): one bar per harmonic, 0 … 1; click or drag across the bars to
// draw them (each touched bar is one gesture).
class PartialsDisplay final : public PanelDisplay {
public:
  PartialsDisplay(DevicePanel::Impl& impl, const ControlDesc& d) : PanelDisplay(d.id, impl, d) { rebind(); }
  void rebind() override {
    names_.clear();
    deps_.clear();
    for (int h = 1; h <= 64; ++h) {
      const std::string n = roleName(impl_, desc_, ("u" + std::to_string(h)).c_str());
      if (n.empty()) break;
      names_.push_back(n);
      deps_.insert(n);
    }
    markDirty();
  }
  bool dependsOn(std::string_view name) const override { return deps_.count(std::string(name)) != 0; }
  ui::InfoText info() const override {
    return {"Partials", "The User waveform: the level of each harmonic (1 = the fundamental, at the left). Click or drag across "
                        "the bars to draw them. Partials above 0.45 × the sample rate are left out as the pitch rises."};
  }
  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, ctx.scale.exact(2.0f), th->device.display, th->device.border);
    if (names_.empty()) return;
    const ui::Rect p = plot();
    const float bw = p.width() / static_cast<float>(names_.size());
    for (size_t i = 0; i < names_.size(); ++i) {
      const float v = static_cast<float>(std::clamp(impl_.value(names_[i]), 0.0, 1.0));
      const float x0 = std::round(p.x0 + bw * static_cast<float>(i)) + 1.0f, x1 = std::round(p.x0 + bw * static_cast<float>(i + 1)) - 1.0f;
      ctx.dl.addRectFilled({x0, p.y0, x1, p.y1}, th->device.displayGrid);
      if (v > 0.0f) ctx.dl.addRectFilled({x0, p.y1 - v * p.height(), x1, p.y1}, static_cast<int>(i) == hover_ ? th->device.bandSelected : th->device.curve);
    }
  }
  bool onPointer(const ui::PointerEvent& e) override {
    switch (e.type) {
      case ui::PointerType::Down:
        if (e.button != ui::PointerButton::Left || names_.empty()) return false;
        drawing_ = true;
        drawAt(e.pos);
        return true;
      case ui::PointerType::Move: {
        if (drawing_) {
          drawAt(e.pos);
          return true;
        }
        const int h = barAt(e.pos.x);
        if (h != hover_) {
          hover_ = h;
          markDirty();
        }
        return false;
      }
      case ui::PointerType::Up:
        if (!drawing_) return false;
        drawing_ = false;
        return true;
      default: return false;
    }
  }
  void onCaptureLost() override { drawing_ = false; }
  // T4.16c: click or drag across draws the levels.
  ui::Cursor cursorAt(ui::Vec2) const override { return names_.empty() ? ui::Cursor::Inherit : ui::Cursor::Crosshair; }
  void onHoverChanged(bool h) override {
    if (!h && hover_ >= 0) {
      hover_ = -1;
      markDirty();
    }
  }

private:
  ui::Rect plot() const {
    const ui::IRect r = rect();
    const float pad = 4.0f * (tree() ? tree()->scale() : 1.0f);
    return {static_cast<float>(r.x) + pad, static_cast<float>(r.y) + pad, static_cast<float>(r.right()) - pad, static_cast<float>(r.bottom()) - pad};
  }
  int barAt(float x) const {
    if (names_.empty()) return -1;
    const ui::Rect p = plot();
    if (x < p.x0 || x >= p.x1) return -1;
    return std::clamp(static_cast<int>((x - p.x0) / p.width() * static_cast<float>(names_.size())), 0, static_cast<int>(names_.size()) - 1);
  }
  void drawAt(ui::Vec2 pos) {
    const int b = barAt(std::clamp(pos.x, plot().x0, plot().x1 - 0.5f));
    if (b < 0) return;
    const ui::Rect p = plot();
    const double v = std::clamp(static_cast<double>((p.y1 - pos.y) / std::max(1.0f, p.height())), 0.0, 1.0);
    const std::string& n = names_[static_cast<size_t>(b)];
    impl_.userParamBegin(n);
    impl_.userParam(n, v, this);
    impl_.userParamEnd(n);
    markDirty();
  }
  std::vector<std::string> names_;
  std::set<std::string, std::less<>> deps_;
  int hover_ = -1;
  bool drawing_ = false;
};

}  // namespace

std::unique_ptr<PanelDisplay> makeOperatorDisplay(DevicePanel::Impl& impl, const ControlDesc& desc) {
  if (desc.visual == "envelope") return std::make_unique<EnvelopeDisplay>(impl, desc);
  if (desc.visual == "algorithm") return std::make_unique<AlgorithmDisplay>(impl, desc);
  if (desc.visual == "partials") return std::make_unique<PartialsDisplay>(impl, desc);
  return nullptr;
}

}  // namespace pq::views::devices
