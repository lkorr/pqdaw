// GU.3: the curve editor element (docs/plans/GU.md "Curves": Vital / Serum style). It shows the bound control's curve
// (pq/dsp/curve.h: knob position left → right, the value's share of its range bottom → top) with the knob's position on
// it, and edits it through the target's setCurve (ops::setParamCurve: within the curve's class a Param change — no
// recompile —, a new class one Structural step):
//   drag a point            moves it (the first and last stay at t = 0 / 1; t stays between its neighbours)
//   drag a segment's handle bends that segment (its middle follows the pointer up / down); a straight segment becomes a
//                           Power segment at the first bend (a new class: the one recompile of that drag)
//   double-click the curve  adds a point there (≤ 16 segments); double-click a point: removes it
//   right-click             the segment's shape (Linear, Exponential, Logarithmic, Power, S-curve) and the presets
// Each drag or menu choice is one undo step.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "elements_internal.h"
#include "pq/ui/overlay.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr StyleSpec kStyle[] = {
    {"colour", StyleSpec::Type::Token, 0, "value"},
    {"grid", StyleSpec::Type::Number, 4.0, {}, {}, 0.0, 16.0},
};
constexpr size_t kSamples = 64;
constexpr float kGrabPx = 7.0f;  // device px around a point / handle that grabs it

constexpr std::array<const char*, dsp::kCurveShapeCount> kShapeNames = {"Linear", "Exponential", "Logarithmic", "Power", "S-curve", "Joined exponential"};

double shareAt(const dsp::Curve& c, double t) noexcept {
  const double span = c.max - c.min;
  if (!(std::fabs(span) > 0.0)) return 0.0;
  return std::clamp((dsp::evalCurveUnits(c, t) - c.min) / span, -0.25, 1.25);
}

class CurveElement final : public Element {
public:
  using Element::Element;
  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 2; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? ui::lerp(th.face.body, th.face.track, 0.35f) : th.face.track;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rect r = plot();
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) {
      p.roundedRect({0.5f, 0.5f, desc_.w - 0.5f, desc_.h - 0.5f}, 4.0f, w);
      return;
    }
    const int n = static_cast<int>(std::clamp(num("grid", 4.0), 0.0, 16.0));
    for (int k = 1; k < n; ++k) {
      const float fx = r.x0 + (r.x1 - r.x0) * static_cast<float>(k) / static_cast<float>(n);
      const float fy = r.y0 + (r.y1 - r.y0) * static_cast<float>(k) / static_cast<float>(n);
      p.line({fx, r.y0}, {fx, r.y1}, 0.6f, ui::mulAlpha(w, 0.55f));
      p.line({r.x0, fy}, {r.x1, fy}, 0.6f, ui::mulAlpha(w, 0.55f));
    }
    outlineRect(p, {0.5f, 0.5f, desc_.w - 0.5f, desc_.h - 0.5f}, 4.0f, 1.0f, w);
  }

  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const doc::Binding* b = binding();
    if (b == nullptr) return;
    const BoundInfo& info = t.info(*b);
    if (!info.valid) return;
    const dsp::Curve& c = info.curve;
    const Rect r = plot();
    const Rgba accent = colour(th, "colour", "value", th.face.value);
    std::array<Vec2, kSamples + 1> pts;
    for (size_t i = 0; i <= kSamples; ++i) {
      const double x = static_cast<double>(i) / kSamples;
      pts[i] = toLocal(x, shareAt(c, x));
    }
    p.fillUnder(pts, r.y1, ui::mulAlpha(accent, 0.28f), ui::mulAlpha(accent, 0.02f));
    p.polyline(pts, 1.6f, accent);
    // The knob's position on it.
    const double kp = std::clamp(t.position(*b), 0.0, 1.0);
    const Vec2 at = toLocal(kp, shareAt(c, kp));
    const std::array<Vec2, 2> drop = {Vec2{at.x, r.y1}, at};
    p.dashed(drop, 0.8f, 2.0f, 2.0f, ui::mulAlpha(th.face.handle, 0.6f));
    p.disc(at, 2.4f, th.face.handle);
    // Points and bend handles.
    for (uint32_t i = 0; i < c.segments; ++i) {
      const Vec2 h = handleAt(c, i);
      const bool hot = (drag_ == Drag::Bend && index_ == i) || (hoverKind_ == Drag::Bend && hoverIndex_ == i);
      p.ring(h, hot ? 3.2f : 2.6f, 1.0f, hot ? th.face.handle : ui::mulAlpha(th.face.handle, 0.7f));
    }
    for (uint32_t i = 0; i <= c.segments; ++i) {
      const Vec2 q = toLocal(c.points[i].t, c.points[i].y);
      const bool hot = (drag_ == Drag::Point && index_ == i) || (hoverKind_ == Drag::Point && hoverIndex_ == i);
      p.disc(q, hot ? 3.8f : 3.0f, ui::mulAlpha(th.face.body, 0.9f));
      p.disc(q, hot ? 2.9f : 2.2f, hot ? th.face.handle : accent);
    }
  }

  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    const doc::Binding* b = binding();
    if (b == nullptr) return false;
    const BoundInfo& info = t.info(*b);
    if (!info.valid || !info.editable) return false;
    const dsp::Curve& c = info.curve;
    if (e.kind == K::Move && drag_ == Drag::None) {
      Drag k = Drag::None;
      uint32_t i = 0;
      grab(c, e, k, i);
      const bool changed = k != hoverKind_ || i != hoverIndex_;
      hoverKind_ = k, hoverIndex_ = i;
      return changed;
    }
    if (e.kind == K::Down) {
      Drag k = Drag::None;
      uint32_t i = 0;
      grab(c, e, k, i);
      if (e.button == ui::PointerButton::Right) {
        openMenu(t, *b, c, e, k == Drag::Bend ? static_cast<int32_t>(i) : segmentAt(c, fromLocal(e.local).x));
        return true;
      }
      if (e.button != ui::PointerButton::Left) return false;
      if (e.clicks >= 2) {
        if (k == Drag::Point) removePoint(t, *b, c, i);
        else addPoint(t, *b, c, fromLocal(e.local).x);
        return true;
      }
      if (k == Drag::None) return true;  // a press on the plot: nothing to drag
      drag_ = k;
      index_ = i;
      startDev_ = e.device;
      startBend_ = c.shapes[std::min<uint32_t>(i, dsp::kMaxCurveSegments - 1)].bend;
      t.begin(*b, "Edit Curve");
      return true;
    }
    if (e.kind == K::Move && drag_ != Drag::None) {
      dsp::Curve next = c;
      next.preset = dsp::CurvePreset::Custom;
      if (drag_ == Drag::Point) {
        const Vec2 f = fromLocal(e.local);
        const uint32_t i = index_;
        dsp::CurvePoint& q = next.points[i];
        if (i > 0 && i < next.segments) {
          constexpr double kGap = 1e-3;
          q.t = std::clamp(static_cast<double>(f.x), next.points[i - 1].t + kGap, next.points[i + 1].t - kGap);
        }
        q.y = std::clamp(static_cast<double>(f.y), 0.0, 1.0);
      } else {
        const uint32_t i = index_;
        dsp::CurveSegment& s = next.shapes[i];
        if (s.shape == dsp::CurveShape::Linear) s.shape = dsp::CurveShape::Power;  // a straight segment bends as a power
        const double up = -(static_cast<double>(e.device.y) - startDev_.y) / (30.0 * e.uiScale);
        const double gain = s.shape == dsp::CurveShape::Power ? 0.5 : 1.5;
        s.bend = std::clamp(startBend_ + bendSign(next, i) * up * gain, -24.0, 24.0);
      }
      if (dsp::validCurve(next)) t.setCurve(*b, next);
      return true;
    }
    if (e.kind == K::Up && drag_ != Drag::None) {
      drag_ = Drag::None;
      t.end();
      return true;
    }
    return false;
  }
  bool dragging() const noexcept override { return drag_ != Drag::None; }
  void cancelDrag(PanelTarget& t) override {
    if (drag_ == Drag::None) return;
    drag_ = Drag::None;
    t.cancel();
  }
  ui::Cursor cursor() const noexcept override {
    const Drag k = drag_ != Drag::None ? drag_ : hoverKind_;
    return k == Drag::Point ? ui::Cursor::Move : k == Drag::Bend ? ui::Cursor::ResizeNS : ui::Cursor::Crosshair;
  }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    title = "Curve · " + labelText(t);
    body = "Drag a point to move it, a segment's handle to bend it; double-click to add or remove a point; right-click "
           "for the segment's shape and the presets.";
  }

  // Tests: where point i / segment i's bend handle is (element-local).
  Vec2 pointAt(const dsp::Curve& c, uint32_t i) const noexcept { return toLocal(c.points[i].t, c.points[i].y); }
  Vec2 handleAt(const dsp::Curve& c, uint32_t i) const noexcept {
    const double tm = 0.5 * (c.points[i].t + c.points[i + 1].t);
    return toLocal(tm, shareAt(c, tm));
  }

private:
  enum class Drag : uint8_t { None, Point, Bend };
  Rect plot() const noexcept { return {6.0f, 6.0f, std::max(7.0f, desc_.w - 6.0f), std::max(7.0f, desc_.h - 6.0f)}; }
  Vec2 toLocal(double t, double share) const noexcept {
    const Rect r = plot();
    return {r.x0 + (r.x1 - r.x0) * static_cast<float>(t), r.y1 - (r.y1 - r.y0) * static_cast<float>(share)};
  }
  Vec2 fromLocal(Vec2 l) const noexcept {
    const Rect r = plot();
    return {std::clamp((l.x - r.x0) / std::max(1.0f, r.x1 - r.x0), 0.0f, 1.0f), std::clamp((r.y1 - l.y) / std::max(1.0f, r.y1 - r.y0), 0.0f, 1.0f)};
  }
  void grab(const dsp::Curve& c, const FacePointer& e, Drag& kind, uint32_t& index) const {
    const float lim = kGrabPx / std::max(1e-3f, e.pxPerUnit);
    float best = lim;
    kind = Drag::None;
    for (uint32_t i = 0; i <= c.segments; ++i) {
      const Vec2 q = pointAt(c, i);
      const float d = std::hypot(q.x - e.local.x, q.y - e.local.y);
      if (d <= best) best = d, kind = Drag::Point, index = i;
    }
    if (kind == Drag::Point) return;
    for (uint32_t i = 0; i < c.segments; ++i) {
      const Vec2 h = handleAt(c, i);
      const float d = std::hypot(h.x - e.local.x, h.y - e.local.y);
      if (d <= best) best = d, kind = Drag::Bend, index = i;
    }
  }
  static int32_t segmentAt(const dsp::Curve& c, double t) noexcept {
    for (uint32_t i = 0; i < c.segments; ++i)
      if (t <= c.points[i + 1].t) return static_cast<int32_t>(i);
    return static_cast<int32_t>(c.segments) - 1;
  }
  // +1 when a larger bend raises segment i's middle, −1 when it lowers it.
  static double bendSign(const dsp::Curve& c, uint32_t i) noexcept {
    dsp::Curve a = c, b = c;
    a.shapes[i].bend -= 0.05;
    b.shapes[i].bend += 0.05;
    const double tm = 0.5 * (c.points[i].t + c.points[i + 1].t);
    return shareAt(b, tm) >= shareAt(a, tm) ? 1.0 : -1.0;
  }
  static void once(PanelTarget& t, const doc::Binding& b, const dsp::Curve& c, const char* label) {
    if (!dsp::validCurve(c)) return;
    t.begin(b, label);
    t.setCurve(b, c);
    t.end();
  }
  static void addPoint(PanelTarget& t, const doc::Binding& b, const dsp::Curve& c, double at) {
    if (c.segments >= dsp::kMaxCurveSegments) {
      t.notice("A curve has at most 16 segments");
      return;
    }
    const int32_t s = segmentAt(c, at);
    if (s < 0) return;
    const uint32_t i = static_cast<uint32_t>(s);
    constexpr double kGap = 1e-3;
    if (at <= c.points[i].t + kGap || at >= c.points[i + 1].t - kGap) return;
    dsp::Curve n = c;
    n.preset = dsp::CurvePreset::Custom;
    for (uint32_t k = c.segments; k > i; --k) n.points[k + 1] = c.points[k];
    for (uint32_t k = c.segments; k > i; --k) n.shapes[k] = c.shapes[k - 1];
    n.points[i + 1] = {at, std::clamp(shareAt(c, at), 0.0, 1.0)};
    n.shapes[i + 1] = c.shapes[i];
    n.segments = static_cast<uint8_t>(c.segments + 1);
    once(t, b, n, "Add Curve Point");
  }
  static void removePoint(PanelTarget& t, const doc::Binding& b, const dsp::Curve& c, uint32_t i) {
    if (i == 0 || i >= c.segments) return;  // the ends stay
    dsp::Curve n = c;
    n.preset = dsp::CurvePreset::Custom;
    for (uint32_t k = i; k < c.segments; ++k) n.points[k] = c.points[k + 1];
    for (uint32_t k = i; k + 1 < c.segments; ++k) n.shapes[k] = c.shapes[k + 1];
    n.points[c.segments] = {};
    n.shapes[c.segments - 1] = {};
    n.segments = static_cast<uint8_t>(c.segments - 1);
    once(t, b, n, "Remove Curve Point");
  }
  void openMenu(PanelTarget& t, const doc::Binding& b, const dsp::Curve& c, const FacePointer& e, int32_t segment) {
    ui::Widget* host = t.menuHost();
    if (host == nullptr) return;
    std::vector<ui::MenuItem> items;
    PanelTarget* target = &t;
    if (segment >= 0) {
      const uint32_t i = static_cast<uint32_t>(segment);
      for (uint32_t k = 0; k < dsp::kCurveShapeCount; ++k) {
        const auto shape = static_cast<dsp::CurveShape>(k);
        ui::MenuItem m = ui::MenuItem::item(kShapeNames[k], [target, b, c, i, shape] {
          dsp::Curve n = c;
          n.preset = dsp::CurvePreset::Custom;
          n.shapes[i].shape = shape;
          if (shape != dsp::CurveShape::Linear && n.shapes[i].bend == 0.0) n.shapes[i].bend = shape == dsp::CurveShape::Power ? 1.0 : 3.0;
          once(*target, b, n, "Curve Shape");
        });
        m.checked = c.shapes[i].shape == shape;
        items.push_back(std::move(m));
      }
      items.push_back(ui::MenuItem::sep());
    }
    auto preset = [&](const char* label, dsp::CurvePreset p, double arg) {
      ui::MenuItem m = ui::MenuItem::item(label, [target, b, c, p, arg] {
        dsp::Curve n = dsp::curvePreset(p, c.min, c.max, arg);
        n.unit = c.unit, n.digits = c.digits, n.step = c.step, n.mode = c.mode, n.output = c.output;
        once(*target, b, n, "Curve Preset");
      });
      m.checked = c.preset == p;
      items.push_back(std::move(m));
    };
    preset("Linear", dsp::CurvePreset::Linear, 0.0);
    preset("Exponential", dsp::CurvePreset::Exponential, dsp::kCurveBendK);
    preset("Logarithmic", dsp::CurvePreset::Logarithmic, dsp::kCurveBendK);
    if (c.min > 0.0 && c.max > c.min) preset("Octave", dsp::CurvePreset::Octave, 0.0);
    // GU.4: the VCA's law (a fine ±1 zone, log ends joined to it), over the curve's range.
    preset("VCA (fine zone)", dsp::CurvePreset::Vca, c.preset == dsp::CurvePreset::Vca ? c.presetArg : 0.0);
    (void)ui::openContextMenu(*host, e.device, std::move(items));
  }

  Drag drag_ = Drag::None;
  uint32_t index_ = 0;
  Vec2 startDev_{0.0f, 0.0f};
  double startBend_ = 0.0;
  Drag hoverKind_ = Drag::None;
  uint32_t hoverIndex_ = 0;
};

}  // namespace

std::span<const StyleSpec> curveElementStyle() noexcept { return kStyle; }
std::unique_ptr<Element> makeCurveElement(const doc::PanelElement& desc) { return std::make_unique<CurveElement>(desc); }

}  // namespace pq::views::panel
