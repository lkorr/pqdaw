// GU.3: the element base, the registry and the generic element kinds (pq/views/panel/element.h): knob, slider (h / v),
// number, toggle, button, menu, XY, label, shape, tabs, mod source. The curve editor is curve_element.cpp.
//
// Every kind draws in the meta faces' family (a dark body, quiet tracks, the value in the accent; theme tokens for
// every colour, so a style picks a token, never an RGB) and keeps its static part (tracks, grooves, frames: depends
// only on kind + style + size) apart from the live one (the value), so the face's tile cache shares the static part.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "elements_internal.h"
#include "pq/ui/overlay.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

// ---- base ------------------------------------------------------------------------------------------------------

namespace {

uint64_t mix(uint64_t h, uint64_t v) noexcept {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h * 0xBF58476D1CE4E5B9ull;
}
uint64_t hashText(uint64_t h, std::string_view s) noexcept {
  uint64_t x = 0xCBF29CE484222325ull;
  for (const char c : s) x = (x ^ static_cast<uint8_t>(c)) * 0x100000001B3ull;
  return mix(h, x ^ s.size());
}

}  // namespace

Element::Element(doc::PanelElement desc) : desc_(std::move(desc)) {
  uint64_t h = hashText(0x50414E454Cull, desc_.kind);
  for (const doc::StyleField& f : desc_.style) {
    h = hashText(h, f.name);
    if (f.isText) h = hashText(h, f.text);
    else h = mix(h, std::bit_cast<uint64_t>(f.number));
  }
  h = mix(h, static_cast<uint64_t>(std::lround(desc_.w * 64.0f)));
  h = mix(h, static_cast<uint64_t>(std::lround(desc_.h * 64.0f)));
  staticKey_ = h | 1u;
}

std::optional<ControlAnchor> Element::anchorFor(const doc::Binding& want, bool /*shownOnly*/) const {
  const doc::Binding* b = binding();
  if (b == nullptr || !(*b == want)) return std::nullopt;
  return anchor().value_or(ControlAnchor{{desc_.w * 0.5f, desc_.h * 0.5f}, 0.0f, desc_.w * 0.5f});
}

bool Element::hit(Vec2 l) const noexcept { return l.x >= 0.0f && l.y >= 0.0f && l.x <= desc_.w && l.y <= desc_.h; }

const std::string& Element::labelText(const PanelTarget& t, size_t slot) const {
  static const std::string kNone;
  const std::string_view f = txt("label", "");
  if (f == "-") return kNone;
  const doc::Binding* b = binding(slot);
  const uint64_t gen = b != nullptr ? t.info(*b).generation : 0;
  if (gen != labelGen_) {
    labelGen_ = gen;
    label_ = !f.empty() ? std::string(f) : b != nullptr ? t.info(*b).label : std::string();
  }
  return label_;
}

const std::string& Element::readoutText(const PanelTarget& t, double position, size_t slot) const {
  static const std::string kNone;
  const doc::Binding* b = binding(slot);
  if (b == nullptr) return kNone;
  const uint64_t gen = t.info(*b).generation;
  if (gen != readoutGen_ || position != readoutPos_) {
    readoutGen_ = gen;
    readoutPos_ = position;
    readout_ = t.format(*b, position);
  }
  return readout_;
}

void Element::describe(const PanelTarget& t, std::string& title, std::string& body) const {
  title = labelText(t);
  if (title.empty()) title = std::string(desc_.kind);
  const doc::Binding* b = binding();
  if (b == nullptr) {
    body.clear();
    return;
  }
  const BoundInfo& i = t.info(*b);
  if (!i.valid) {
    body = "Bound to something the group no longer has.";
    return;
  }
  body = t.format(*b, t.position(*b)) + (i.setting ? " (a setting: a change recompiles the group's track)." : ".");
}

Rgba themeToken(const ui::Theme& th, std::string_view k, Rgba fallback) noexcept {
  const ui::Theme::Face& f = th.face;
  if (k == "value" || k == "accent") return f.value;
  if (k == "signal") return f.signal;
  if (k == "track") return f.track;
  if (k == "handle") return f.handle;
  if (k == "ink") return f.ink;
  if (k == "frame") return f.frame;
  if (k == "tick") return f.tick;
  if (k == "hover") return f.hover;
  if (k == "glow") return f.glow;
  if (k == "body") return f.body;
  if (k == "hub") return f.hub;
  if (k == "driven") return f.valueDriven;
  if (k == "selected") return th.circuit.selected;
  if (k == "instrument") return th.circuit.wireAudio;
  return fallback;
}

void outlineRect(ui::FacePainter& p, const Rect& r, float rad, float t, Rgba col) {
  constexpr float kPi = std::numbers::pi_v<float>;
  if (ui::alphaOf(col) == 0) return;
  rad = std::min({rad, (r.x1 - r.x0) * 0.5f, (r.y1 - r.y0) * 0.5f});
  p.line({r.x0 + rad, r.y0}, {r.x1 - rad, r.y0}, t, col);
  p.line({r.x0 + rad, r.y1}, {r.x1 - rad, r.y1}, t, col);
  p.line({r.x0, r.y0 + rad}, {r.x0, r.y1 - rad}, t, col);
  p.line({r.x1, r.y0 + rad}, {r.x1, r.y1 - rad}, t, col);
  if (rad <= 0.0f) return;
  p.arc({r.x0 + rad, r.y0 + rad}, rad, kPi, 1.5f * kPi, t, col);
  p.arc({r.x1 - rad, r.y0 + rad}, rad, -0.5f * kPi, 0.0f, t, col);
  p.arc({r.x1 - rad, r.y1 - rad}, rad, 0.0f, 0.5f * kPi, t, col);
  p.arc({r.x0 + rad, r.y1 - rad}, rad, 0.5f * kPi, kPi, t, col);
}

// ---- shared behaviour ---------------------------------------------------------------------------------------------

std::vector<std::string> splitItems(std::string_view s) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  size_t a = 0;
  while (true) {
    const size_t b = s.find('|', a);
    out.emplace_back(s.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a));
    if (b == std::string_view::npos) break;
    a = b + 1;
  }
  return out;
}

double ValueElement::pos(const PanelTarget& t, size_t slot) const {
  const doc::Binding* b = binding(slot);
  return b != nullptr ? std::clamp(t.position(*b), 0.0, 1.0) : 0.0;
}
const BoundInfo& ValueElement::bound(const PanelTarget& t, size_t slot) const {
  static const BoundInfo kNone;
  const doc::Binding* b = binding(slot);
  return b != nullptr ? t.info(*b) : kNone;
}
bool ValueElement::editable(const PanelTarget& t, size_t slot) const {
  const doc::Binding* b = binding(slot);
  if (b == nullptr) return false;
  const BoundInfo& i = t.info(*b);
  return i.valid && i.editable;
}
void ValueElement::startDrag(PanelTarget& t, const FacePointer& e, const char* label) {
  drag_ = true;
  startDev_ = e.device;
  for (size_t s = 0; s < kSlots; ++s) start_[s] = pos(t, s);
  if (const doc::Binding* b = binding()) t.begin(*b, label);
}
void ValueElement::endDrag(PanelTarget& t) {
  if (!drag_) return;
  drag_ = false;
  t.end();
}
void ValueElement::cancelDrag(PanelTarget& t) {
  if (!drag_) return;
  drag_ = false;
  t.cancel();
}
double ValueElement::quantise(const PanelTarget& t, double p, size_t slot) const {
  const BoundInfo& i = bound(t, slot);
  if (i.steps >= 2) return std::round(p * (i.steps - 1)) / (i.steps - 1);
  return p;
}

// A relative drag: `axisPx` device px of pointer travel per whole range (Shift: a tenth).
double ValueElement::dragged(const FacePointer& e, size_t slot, float dx, float dy, float axisPx) const {
  const double fine = (e.mods & ui::mod::kShift) != 0 ? 0.1 : 1.0;
  const double d = (static_cast<double>(e.device.x) - startDev_.x) * dx - (static_cast<double>(e.device.y) - startDev_.y) * dy;
  return std::clamp(start_[slot] + d * fine / std::max(4.0, static_cast<double>(axisPx)), 0.0, 1.0);
}

void ValueElement::describe(const PanelTarget& t, std::string& title, std::string& body) const {
  Element::describe(t, title, body);
  if (!body.empty() && interactive()) body += hint();
}

// ---- knob ---------------------------------------------------------------------------------------------------------

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kLabelPx = 7.5f, kValuePx = 8.0f;
Vec2 polar(Vec2 c, float r, float a) noexcept { return {c.x + r * std::cos(a), c.y + r * std::sin(a)}; }

constexpr std::array<std::string_view, 4> kPointers = {"arc", "pie", "dot", "line"};
constexpr std::array<std::string_view, 4> kReadouts = {"below", "above", "inside", "none"};
constexpr std::array<std::string_view, 3> kOrient = {"auto", "horizontal", "vertical"};
constexpr std::array<std::string_view, 2> kHandles = {"bar", "dot"};
constexpr std::array<std::string_view, 2> kToggleLooks = {"switch", "button"};
constexpr std::array<std::string_view, 4> kShapes = {"rect", "round", "circle", "line"};
constexpr std::array<std::string_view, 3> kAligns = {"centre", "left", "right"};
constexpr std::array<std::string_view, 1> kValueSlot = {"value"};
constexpr std::array<std::string_view, 2> kXySlots = {"x", "y"};
constexpr std::array<std::string_view, 1> kSignalSlot = {"signal"};

class Knob final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"pointer", StyleSpec::Type::Choice, 0, "arc", kPointers},
      {"track", StyleSpec::Type::Number, 3.0, {}, {}, 0.5, 12.0},
      {"sweep", StyleSpec::Type::Number, 270.0, {}, {}, 30.0, 360.0},
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"readout", StyleSpec::Type::Choice, 0, "below", kReadouts},
      {"label", StyleSpec::Type::Text, 0, ""},
  };

  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 2; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.track : th.face.tick;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    const G g = geom();
    if (layer == 0) {
      if (pointer() == "pie") p.disc(g.c, g.r, ui::mulAlpha(w, 0.55f));
      else p.arc(g.c, g.r, g.a0, g.a0 + g.sweep, track(), w, ui::LineCap::Round);
      return;
    }
    // The ends of the travel: two short ticks outside the track.
    if (g.sweep < 2.0f * kPi - 0.01f)
      for (const float a : {g.a0, g.a0 + g.sweep}) p.line(polar(g.c, g.r + track() * 0.5f + 1.0f, a), polar(g.c, g.r + track() * 0.5f + 3.5f, a), 1.0f, w);
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const G g = geom();
    const double v = pos(t);
    const bool ok = editable(t);
    const Rgba accent = ok ? colour(th, "colour", "value", th.face.value) : ui::lerp(th.face.track, th.face.value, 0.35f);
    const float a = g.a0 + g.sweep * static_cast<float>(v);
    const bool hot = env.hovered || drag_;
    const std::string_view ptr = pointer();
    if (hot) p.ring(g.c, g.r, track() + 2.0f, th.face.hover);
    // GU.5: a cable drives the control — its modulation ring, just outside the track (and its end ticks).
    if (binding() != nullptr && t.driven(*binding())) p.ring(g.c, g.r + track() * 0.5f + 5.0f, 1.5f, th.face.valueDriven);
    if (ptr == "pie") {
      // A filled wedge from the start to the value: an arc as thick as the radius (one primitive, no seams).
      if (a > g.a0 + 1e-4f) p.arc(g.c, g.r * 0.5f, g.a0, a, g.r, ui::mulAlpha(accent, 0.9f));
      p.line(g.c, polar(g.c, g.r, a), 1.5f, th.face.handle);
    } else if (ptr == "arc") {
      if (a > g.a0 + 1e-4f) p.arc(g.c, g.r, g.a0, a, track(), accent, ui::LineCap::Round);
      p.disc(g.c, std::max(1.0f, g.r - track() - 1.5f), ui::lerp(th.face.body, th.face.track, 0.5f));
      p.line(polar(g.c, std::max(0.0f, g.r * 0.25f), a), polar(g.c, g.r - track() * 0.5f - 1.0f, a), hot ? 2.0f : 1.6f, th.face.handle);
    } else if (ptr == "dot") {
      p.disc(polar(g.c, g.r, a), track() * 0.5f + 1.6f, accent);
    } else {  // line
      p.disc(g.c, std::max(1.0f, g.r - track() * 0.5f - 1.0f), ui::lerp(th.face.body, th.face.track, 0.45f));
      p.line(g.c, polar(g.c, g.r, a), std::max(1.2f, track() * 0.6f), accent);
    }
    const std::string& label = labelText(t);
    if (!label.empty()) p.text({desc_.w * 0.5f, g.labelY}, label, kLabelPx, th.face.ink);
    if (g.readoutY > 0.0f && binding() != nullptr)
      p.text({desc_.w * 0.5f, g.readoutY}, readoutText(t, v), g.inside ? kLabelPx : kValuePx, ok ? accent : th.face.ink);
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    if (e.kind == K::Down) {
      if (e.button != ui::PointerButton::Left || !editable(t)) return false;
      if (e.clicks >= 2) {
        t.reset(*binding());
        return true;
      }
      startDrag(t, e, "Panel Knob");
      return true;
    }
    if (e.kind == K::Move && drag_) {
      t.setPosition(*binding(), quantise(t, dragged(e, 0, 0.0f, 1.0f, 200.0f * e.uiScale)));
      return true;
    }
    if (e.kind == K::Up && drag_) {
      endDrag(t);
      return true;
    }
    return false;
  }
  ui::Cursor cursor() const noexcept override { return ui::Cursor::ResizeNS; }
  // GU.5b: the dial and a little around it (its label and readout are the panel's: a value block's knob is moved by them).
  bool hit(Vec2 l) const noexcept override {
    const G g = geom();
    const float r = g.r + track() * 0.5f + 4.0f;
    return (l.x - g.c.x) * (l.x - g.c.x) + (l.y - g.c.y) * (l.y - g.c.y) <= r * r;
  }
  std::optional<ControlAnchor> anchor() const override {
    const G g = geom();
    return ControlAnchor{g.c, g.r + track() * 0.5f, desc_.w * 0.5f};
  }

private:
  struct G {
    Vec2 c;
    float r, a0, sweep, labelY, readoutY;
    bool inside;
  };
  std::string_view pointer() const noexcept { return txt("pointer", "arc"); }
  float track() const noexcept { return static_cast<float>(std::clamp(num("track", 3.0), 0.5, 12.0)); }
  G geom() const noexcept {
    const std::string_view ro = txt("readout", "below");
    const bool label = txt("label", "") != "-";
    const float rowH = 10.0f;
    float top = label && ro != "above" ? rowH : 0.0f, bottom = desc_.h - (ro == "below" || (ro == "above" && label) ? rowH : 0.0f);
    if (ro == "above") top = rowH;
    G g{};
    const float avail = std::max(4.0f, std::min(desc_.w, bottom - top));
    g.c = {desc_.w * 0.5f, top + (bottom - top) * 0.5f};
    g.r = std::max(2.0f, avail * 0.5f - track() * 0.5f - 4.0f);
    g.sweep = static_cast<float>(std::clamp(num("sweep", 270.0), 30.0, 360.0)) * kPi / 180.0f;
    g.a0 = 0.5f * kPi + (2.0f * kPi - g.sweep) * 0.5f;
    g.inside = ro == "inside";
    if (ro == "above") {
      g.readoutY = 5.0f;
      g.labelY = desc_.h - 5.0f;
    } else {
      g.labelY = 5.0f;
      g.readoutY = ro == "below" ? desc_.h - 5.0f : ro == "inside" ? g.c.y : -1.0f;
    }
    if (!label) g.labelY = -100.0f;
    return g;
  }
  std::string hint() const override { return " Drag up / down (Shift: fine), double-click: default."; }
};

// ---- slider -------------------------------------------------------------------------------------------------------

class Slider final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"orientation", StyleSpec::Type::Choice, 0, "auto", kOrient},
      {"track", StyleSpec::Type::Number, 4.0, {}, {}, 1.0, 16.0},
      {"handle", StyleSpec::Type::Choice, 0, "bar", kHandles},
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"readout", StyleSpec::Type::Choice, 0, "below", kReadouts},
      {"label", StyleSpec::Type::Text, 0, ""},
  };
  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return th.face.track; }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    const G g = geom();
    p.roundedRect(g.groove, std::min(g.groove.x1 - g.groove.x0, g.groove.y1 - g.groove.y0) * 0.5f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const G g = geom();
    const double v = pos(t);
    const bool ok = editable(t);
    const Rgba accent = ok ? colour(th, "colour", "value", th.face.value) : ui::lerp(th.face.track, th.face.value, 0.35f);
    const Vec2 h = at(g, v);
    Rect fill = g.groove;
    if (g.vertical) fill.y0 = h.y;
    else fill.x1 = h.x;
    const float rr = std::min(g.groove.x1 - g.groove.x0, g.groove.y1 - g.groove.y0) * 0.5f;
    // GU.5: a cable drives the control — its modulation ring: an outline round the groove.
    if (binding() != nullptr && t.driven(*binding()))
      outlineRect(p, {g.groove.x0 - 2.5f, g.groove.y0 - 2.5f, g.groove.x1 + 2.5f, g.groove.y1 + 2.5f}, rr + 2.5f, 1.2f, th.face.valueDriven);
    if (fill.x1 - fill.x0 > 0.2f && fill.y1 - fill.y0 > 0.2f) p.roundedRect(fill, rr, accent);
    const bool hot = env.hovered || drag_;
    const Rgba cap = hot ? ui::lerp(th.face.handle, th.face.value, 0.35f) : th.face.handle;
    if (txt("handle", "bar") == "dot") {
      p.disc(h, rr + 3.0f, ui::mulAlpha(th.face.body, 0.9f));
      p.disc(h, rr + 2.0f, cap);
    } else if (g.vertical) {
      p.roundedRect({h.x - g.half - 3.0f, h.y - 3.0f, h.x + g.half + 3.0f, h.y + 3.0f}, 2.0f, ui::mulAlpha(th.face.body, 0.9f));
      p.roundedRect({h.x - g.half - 2.0f, h.y - 2.0f, h.x + g.half + 2.0f, h.y + 2.0f}, 1.5f, cap);
    } else {
      p.roundedRect({h.x - 3.0f, h.y - g.half - 3.0f, h.x + 3.0f, h.y + g.half + 3.0f}, 2.0f, ui::mulAlpha(th.face.body, 0.9f));
      p.roundedRect({h.x - 2.0f, h.y - g.half - 2.0f, h.x + 2.0f, h.y + g.half + 2.0f}, 1.5f, cap);
    }
    const std::string& label = labelText(t);
    if (!label.empty()) p.textAligned({g.vertical ? desc_.w * 0.5f : 2.0f, 5.0f}, label, kLabelPx, th.face.ink, g.vertical ? 0.5f : 0.0f);
    if (txt("readout", "below") != "none" && binding() != nullptr)
      p.textAligned({g.vertical ? desc_.w * 0.5f : desc_.w - 2.0f, g.vertical ? desc_.h - 5.0f : 5.0f}, readoutText(t, v), kLabelPx,
                    ok ? accent : th.face.ink, g.vertical ? 0.5f : 1.0f);
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    if (e.kind == K::Down) {
      if (e.button != ui::PointerButton::Left || !editable(t)) return false;
      if (e.clicks >= 2) {
        t.reset(*binding());
        return true;
      }
      startDrag(t, e, "Panel Slider");
      return true;
    }
    if (e.kind == K::Move && drag_) {
      const G g = geom();
      const float len = (g.vertical ? g.groove.y1 - g.groove.y0 : g.groove.x1 - g.groove.x0) * e.pxPerUnit;
      t.setPosition(*binding(), quantise(t, g.vertical ? dragged(e, 0, 0.0f, 1.0f, len) : dragged(e, 0, 1.0f, 0.0f, len)));
      return true;
    }
    if (e.kind == K::Up && drag_) {
      endDrag(t);
      return true;
    }
    return false;
  }
  ui::Cursor cursor() const noexcept override { return geom().vertical ? ui::Cursor::ResizeNS : ui::Cursor::ResizeEW; }
  std::optional<ControlAnchor> anchor() const override {
    const G g = geom();
    return ControlAnchor{{(g.groove.x0 + g.groove.x1) * 0.5f, (g.groove.y0 + g.groove.y1) * 0.5f}, 0.0f, desc_.w * 0.5f};
  }

private:
  struct G {
    Rect groove;
    bool vertical;
    float half;
  };
  G geom() const noexcept {
    const std::string_view o = txt("orientation", "auto");
    G g{};
    g.vertical = o == "vertical" || (o == "auto" && desc_.h > desc_.w);
    const float tw = static_cast<float>(std::clamp(num("track", 4.0), 1.0, 16.0));
    g.half = tw * 0.5f;
    if (g.vertical) {
      const float cx = desc_.w * 0.5f;
      g.groove = {cx - g.half, 13.0f, cx + g.half, std::max(14.0f, desc_.h - 13.0f)};
    } else {
      const float cy = std::max(desc_.h * 0.5f + 4.0f, 14.0f);
      g.groove = {6.0f, cy - g.half, std::max(7.0f, desc_.w - 6.0f), cy + g.half};
    }
    return g;
  }
  static Vec2 at(const G& g, double v) noexcept {
    const float f = static_cast<float>(v);
    return g.vertical ? Vec2{(g.groove.x0 + g.groove.x1) * 0.5f, g.groove.y1 - (g.groove.y1 - g.groove.y0) * f}
                      : Vec2{g.groove.x0 + (g.groove.x1 - g.groove.x0) * f, (g.groove.y0 + g.groove.y1) * 0.5f};
  }
  std::string hint() const override { return " Drag along it (Shift: fine), double-click: default."; }
};

// ---- number box ---------------------------------------------------------------------------------------------------

class Number final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"label", StyleSpec::Type::Text, 0, ""},
      {"size", StyleSpec::Type::Number, 8.5, {}, {}, 4.0, 32.0},
  };
  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 2; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? ui::lerp(th.face.body, th.face.track, 0.5f) : th.face.frame;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rect r = box();
    if (layer == 0) p.roundedRect(r, 3.0f, ui::rgba(255, 255, 255));
    else outlineRect(p, r, 3.0f, 1.0f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const Rect r = box();
    const bool ok = editable(t);
    const Rgba accent = ok ? colour(th, "colour", "value", th.face.value) : th.face.ink;
    if (env.hovered || drag_ || typing_) outlineRect(p, r, 3.0f, 1.25f, typing_ ? th.circuit.selected : th.face.hover);
    const float px = static_cast<float>(std::clamp(num("size", 8.5), 4.0, 32.0));
    const Vec2 c{(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f};
    if (typing_) p.text(c, text_ + "|", px, th.face.ink);
    else if (binding() != nullptr) p.text(c, readoutText(t, pos(t)), px, accent);
    const std::string& label = labelText(t);
    if (!label.empty() && r.y0 > 6.0f) p.textAligned({r.x0, 4.5f}, label, kLabelPx, th.face.ink, 0.0f);
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    if (e.kind == K::Down) {
      if (e.button != ui::PointerButton::Left || !editable(t)) return false;
      if (typing_) return true;
      if (e.clicks >= 2) {  // type a value
        typing_ = true;
        text_.clear();
        return true;
      }
      startDrag(t, e, "Panel Number");
      return true;
    }
    if (e.kind == K::Move && drag_) {
      const BoundInfo& i = bound(t);
      // A step per 6 px for a stepped value, else 300 px per range.
      const float per = i.steps >= 2 ? 6.0f * static_cast<float>(i.steps - 1) : 300.0f;
      t.setPosition(*binding(), quantise(t, dragged(e, 0, 0.0f, 1.0f, per * e.uiScale)));
      return true;
    }
    if (e.kind == K::Up && drag_) {
      endDrag(t);
      return true;
    }
    return false;
  }
  bool wantsText() const noexcept override { return typing_; }
  bool onText(const ui::TextEvent& e, PanelTarget&) override {
    if (!typing_) return false;
    text_ += e.text();
    return true;
  }
  bool onKey(const ui::KeyEvent& e, PanelTarget& t) override {
    if (!typing_ || !e.down) return false;
    if (e.key == ui::key::kEscape) {
      typing_ = false;
      return true;
    }
    if (e.key == ui::key::kBackspace) {
      if (!text_.empty()) text_.pop_back();
      return true;
    }
    if (e.key == ui::key::kReturn) {
      commit(t);
      return true;
    }
    return true;  // the box holds the keys while typing
  }
  void focusLost(PanelTarget& t) override {
    if (typing_) commit(t);
  }
  ui::Cursor cursor() const noexcept override { return typing_ ? ui::Cursor::Text : ui::Cursor::ResizeNS; }
  bool typing() const noexcept { return typing_; }

private:
  Rect box() const noexcept {
    const float top = labelRow() ? std::min(10.0f, desc_.h * 0.4f) : 0.0f;
    return {0.5f, top + 0.5f, desc_.w - 0.5f, desc_.h - 0.5f};
  }
  bool labelRow() const noexcept { return txt("label", "") != "-" && desc_.h >= 26.0f; }
  void commit(PanelTarget& t) {
    typing_ = false;
    const doc::Binding* b = binding();
    if (b == nullptr || text_.empty()) return;
    if (const std::optional<double> p = t.parse(*b, text_)) t.setOnce(*b, *p, "Panel Value");
    else t.notice("'" + text_ + "' is not a value");
  }
  std::string hint() const override { return " Drag up / down (Shift: fine), double-click: type a value."; }
  bool typing_ = false;
  std::string text_;
};

// ---- toggle / button ----------------------------------------------------------------------------------------------

class Toggle final : public ValueElement {
public:
  Toggle(doc::PanelElement d, bool momentary) : ValueElement(std::move(d)), momentary_(momentary) {}
  static constexpr StyleSpec kStyle[] = {
      {"look", StyleSpec::Type::Choice, 0, "switch", kToggleLooks},
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"text", StyleSpec::Type::Text, 0, ""},
  };
  static constexpr StyleSpec kButtonStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"text", StyleSpec::Type::Text, 0, ""},
  };
  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return th.face.track; }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    if (switchLook()) {
      const Rect s = pill();
      p.roundedRect(s, (s.y1 - s.y0) * 0.5f, ui::rgba(255, 255, 255));
    } else {
      p.roundedRect({0.5f, 0.5f, desc_.w - 0.5f, desc_.h - 0.5f}, 3.5f, ui::rgba(255, 255, 255));
    }
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const bool on = pos(t) >= 0.5;
    const bool ok = editable(t);
    const Rgba accent = ok ? colour(th, "colour", "value", th.face.value) : ui::lerp(th.face.track, th.face.value, 0.35f);
    std::string label(txt("text", ""));
    if (label.empty()) label = labelText(t);
    if (switchLook()) {
      const Rect s = pill();
      const float rr = (s.y1 - s.y0) * 0.5f;
      if (on) p.roundedRect(s, rr, accent);
      if (env.hovered) outlineRect(p, s, rr, 1.0f, th.face.hover);
      const float cx = on ? s.x1 - rr : s.x0 + rr;
      p.disc({cx, (s.y0 + s.y1) * 0.5f}, rr - 1.5f, th.face.handle);
      if (!label.empty()) p.textAligned({s.x1 + 4.0f, (s.y0 + s.y1) * 0.5f}, label, kLabelPx, th.face.ink, 0.0f);
    } else {
      const Rect r{0.5f, 0.5f, desc_.w - 0.5f, desc_.h - 0.5f};
      if (on) {
        p.roundedRect(r, 3.5f, accent);
        p.beginGlow();
        p.glow({desc_.w * 0.5f, desc_.h * 0.5f}, std::max(desc_.w, desc_.h) * 0.45f, ui::mulAlpha(accent, 0.22f));
        p.endGlow();
      }
      if (env.hovered) outlineRect(p, r, 3.5f, 1.0f, th.face.hover);
      p.text({desc_.w * 0.5f, desc_.h * 0.5f}, label, kLabelPx, on ? th.face.body : th.face.ink);
    }
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    const doc::Binding* b = binding();
    if (e.kind == K::Down) {
      if (e.button != ui::PointerButton::Left || !editable(t)) return false;
      if (momentary_) {  // pressed: on until released, one undo step
        startDrag(t, e, "Panel Button");
        t.setPosition(*b, 1.0);
        return true;
      }
      t.setOnce(*b, pos(t) >= 0.5 ? 0.0 : 1.0, "Panel Toggle");
      return true;
    }
    if (e.kind == K::Up && drag_) {
      t.setPosition(*b, 0.0);
      endDrag(t);
      return true;
    }
    return e.kind == K::Move && drag_;
  }
  std::optional<double> selection(const PanelTarget& t) const override {
    if (binding() == nullptr) return std::nullopt;
    return pos(t) >= 0.5 ? 1.0 : 0.0;
  }

private:
  bool switchLook() const noexcept { return !momentary_ && txt("look", "switch") == "switch"; }
  Rect pill() const noexcept {
    const float h = std::min(desc_.h - 1.0f, 12.0f), w = std::min(desc_.w - 1.0f, h * 2.0f);
    const float y0 = (desc_.h - h) * 0.5f;
    return {0.5f, y0, 0.5f + w, y0 + h};
  }
  std::string hint() const override { return momentary_ ? " Press: on while held." : " Click to switch."; }
  bool momentary_ = false;
};

// ---- menu ---------------------------------------------------------------------------------------------------------

std::vector<std::string> choicesOf(const Element& e, const BoundInfo& i) {
  std::vector<std::string> items = splitItems(e.desc().text("items", ""));
  if (!items.empty()) return items;
  if (!i.choices.empty()) return i.choices;
  return {};
}
double indexPosition(size_t index, size_t count) noexcept {
  return count <= 1 ? 0.0 : static_cast<double>(index) / static_cast<double>(count - 1);
}
size_t positionIndex(double p, size_t count) noexcept {
  if (count <= 1) return 0;
  return static_cast<size_t>(std::clamp(std::lround(p * static_cast<double>(count - 1)), 0l, static_cast<long>(count - 1)));
}

class MenuElement final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"items", StyleSpec::Type::Text, 0, ""},
      {"label", StyleSpec::Type::Text, 0, ""},
  };
  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 2; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? ui::lerp(th.face.body, th.face.track, 0.5f) : th.face.frame;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rect r = box();
    if (layer == 0) {
      p.roundedRect(r, 3.0f, ui::rgba(255, 255, 255));
      return;
    }
    outlineRect(p, r, 3.0f, 1.0f, ui::rgba(255, 255, 255));
    const float cx = r.x1 - 6.0f, cy = (r.y0 + r.y1) * 0.5f;  // the chevron
    p.line({cx - 2.5f, cy - 1.2f}, {cx, cy + 1.3f}, 1.1f, ui::rgba(255, 255, 255));
    p.line({cx, cy + 1.3f}, {cx + 2.5f, cy - 1.2f}, 1.1f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const Rect r = box();
    if (env.hovered) outlineRect(p, r, 3.0f, 1.25f, th.face.hover);
    const BoundInfo& i = bound(t);
    const std::vector<std::string> items = choicesOf(*this, i);
    std::string shown;
    if (binding() != nullptr) shown = items.empty() ? readoutText(t, pos(t)) : items[positionIndex(pos(t), items.size())];
    p.textAligned({r.x0 + 5.0f, (r.y0 + r.y1) * 0.5f}, shown, kLabelPx, editable(t) ? colour(th, "colour", "value", th.face.value) : th.face.ink, 0.0f);
    const std::string& label = labelText(t);
    if (!label.empty() && r.y0 > 6.0f) p.textAligned({r.x0, 4.5f}, label, kLabelPx, th.face.ink, 0.0f);
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    if (e.kind != FacePointer::Kind::Down || e.button != ui::PointerButton::Left || !editable(t)) return false;
    ui::Widget* host = t.menuHost();
    const doc::Binding* b = binding();
    const std::vector<std::string> items = choicesOf(*this, bound(t));
    if (host == nullptr || b == nullptr || items.empty()) return true;
    const size_t now = positionIndex(pos(t), items.size());
    std::vector<ui::MenuItem> menu;
    PanelTarget* target = &t;
    const doc::Binding bound = *b;
    for (size_t k = 0; k < items.size(); ++k) {
      ui::MenuItem m = ui::MenuItem::item(items[k], [target, bound, k, n = items.size()] { target->setOnce(bound, indexPosition(k, n), "Panel Menu"); });
      m.checked = k == now;
      menu.push_back(std::move(m));
    }
    (void)ui::openContextMenu(*host, e.device, std::move(menu));
    return true;
  }
  std::optional<double> selection(const PanelTarget& t) const override {
    if (binding() == nullptr) return std::nullopt;
    const std::vector<std::string> items = choicesOf(*this, bound(t));
    return static_cast<double>(positionIndex(pos(t), std::max<size_t>(items.size(), 2)));
  }

private:
  Rect box() const noexcept {
    const float top = txt("label", "") != "-" && desc_.h >= 26.0f ? std::min(10.0f, desc_.h * 0.4f) : 0.0f;
    return {0.5f, top + 0.5f, desc_.w - 0.5f, desc_.h - 0.5f};
  }
  std::string hint() const override { return " Click to choose."; }
};

// ---- XY pad -------------------------------------------------------------------------------------------------------

class XyPad final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"grid", StyleSpec::Type::Number, 4.0, {}, {}, 0.0, 16.0},
  };
  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 2; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? ui::lerp(th.face.body, th.face.track, 0.35f) : th.face.track;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rect r = pad();
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) {
      p.roundedRect(r, 4.0f, w);
      return;
    }
    const int n = static_cast<int>(std::clamp(num("grid", 4.0), 0.0, 16.0));
    for (int k = 1; k < n; ++k) {
      const float fx = r.x0 + (r.x1 - r.x0) * static_cast<float>(k) / static_cast<float>(n);
      const float fy = r.y0 + (r.y1 - r.y0) * static_cast<float>(k) / static_cast<float>(n);
      p.line({fx, r.y0 + 1.0f}, {fx, r.y1 - 1.0f}, 0.75f, ui::mulAlpha(w, 0.6f));
      p.line({r.x0 + 1.0f, fy}, {r.x1 - 1.0f, fy}, 0.75f, ui::mulAlpha(w, 0.6f));
    }
    outlineRect(p, r, 4.0f, 1.0f, w);
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const Rect r = pad();
    const Vec2 h = handle(t);
    const Rgba accent = colour(th, "colour", "value", th.face.value);
    p.line({h.x, r.y0 + 1.0f}, {h.x, r.y1 - 1.0f}, 1.0f, ui::mulAlpha(accent, 0.5f));
    p.line({r.x0 + 1.0f, h.y}, {r.x1 - 1.0f, h.y}, 1.0f, ui::mulAlpha(accent, 0.5f));
    p.beginGlow();
    p.glow(h, 10.0f, ui::mulAlpha(th.face.glow, env.hovered || drag_ ? 0.5f : 0.3f));
    p.endGlow();
    p.disc(h, 4.0f, accent);
    p.disc(h, 1.8f, th.face.handle);
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    using K = FacePointer::Kind;
    if (e.kind == K::Down) {
      if (e.button != ui::PointerButton::Left || !editable(t, 0)) return false;
      startDrag(t, e, "Panel XY");
      if (const doc::Binding* by = binding(1)) t.begin(*by, "Panel XY");
      jump(t, e.local);  // the handle goes where it was pressed
      return true;
    }
    if (e.kind == K::Move && drag_) {
      jump(t, e.local);
      return true;
    }
    if (e.kind == K::Up && drag_) {
      endDrag(t);
      return true;
    }
    return false;
  }
  ui::Cursor cursor() const noexcept override { return ui::Cursor::Crosshair; }

private:
  Rect pad() const noexcept { return {0.5f, 0.5f, desc_.w - 0.5f, desc_.h - 0.5f}; }
  Vec2 handle(const PanelTarget& t) const {
    const Rect r = pad();
    return {r.x0 + (r.x1 - r.x0) * static_cast<float>(pos(t, 0)), r.y1 - (r.y1 - r.y0) * static_cast<float>(pos(t, 1))};
  }
  void jump(PanelTarget& t, Vec2 l) {
    const Rect r = pad();
    const double x = std::clamp(static_cast<double>((l.x - r.x0) / std::max(1.0f, r.x1 - r.x0)), 0.0, 1.0);
    const double y = std::clamp(static_cast<double>((r.y1 - l.y) / std::max(1.0f, r.y1 - r.y0)), 0.0, 1.0);
    if (const doc::Binding* bx = binding(0)) t.setPosition(*bx, quantise(t, x, 0));
    if (const doc::Binding* by = binding(1)) t.setPosition(*by, quantise(t, y, 1));
  }
  std::string hint() const override { return " Drag the handle: x and y."; }
};

// ---- label / shape --------------------------------------------------------------------------------------------------

class Label final : public Element {
public:
  using Element::Element;
  static constexpr StyleSpec kStyle[] = {
      {"text", StyleSpec::Type::Text, 0, ""},
      {"size", StyleSpec::Type::Number, 9.0, {}, {}, 3.0, 64.0},
      {"align", StyleSpec::Type::Choice, 0, "centre", kAligns},
      {"colour", StyleSpec::Type::Token, 0, "ink"},
  };
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    std::string s(txt("text", ""));
    if (s.empty()) s = labelText(*env.target);
    const std::string_view a = txt("align", "centre");
    const float align = a == "left" ? 0.0f : a == "right" ? 1.0f : 0.5f;
    p.textAligned({desc_.w * align, desc_.h * 0.5f}, s, static_cast<float>(std::clamp(num("size", 9.0), 3.0, 64.0)),
                  colour(*env.theme, "colour", "ink", env.theme->face.ink), align);
  }
};

class Shape final : public Element {
public:
  using Element::Element;
  static constexpr StyleSpec kStyle[] = {
      {"shape", StyleSpec::Type::Choice, 0, "round", kShapes},
      {"fill", StyleSpec::Type::Token, 0, "track"},
      {"stroke", StyleSpec::Type::Token, 0, "frame"},
      {"radius", StyleSpec::Type::Number, 6.0, {}, {}, 0.0, 256.0},
      {"thickness", StyleSpec::Type::Number, 1.0, {}, {}, 0.0, 32.0},
      {"opacity", StyleSpec::Type::Number, 1.0, {}, {}, 0.0, 1.0},
      {"plate", StyleSpec::Type::Number, 0.0, {}, {}, 0.0, 1.0},  // GU.7: at the bottom, the panel's plate takes its corner
  };
  uint32_t staticLayerCount() const noexcept override { return 2; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    const float a = static_cast<float>(std::clamp(num("opacity", 1.0), 0.0, 1.0));
    if (layer == 0) return txt("fill", "track") == "none" ? 0u : ui::mulAlpha(colour(th, "fill", "track", th.face.track), a);
    return txt("stroke", "frame") == "none" ? 0u : ui::mulAlpha(colour(th, "stroke", "frame", th.face.frame), a);
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    const std::string_view s = txt("shape", "round");
    const float t = static_cast<float>(std::clamp(num("thickness", 1.0), 0.0, 32.0));
    const Rect r{t * 0.5f, t * 0.5f, desc_.w - t * 0.5f, desc_.h - t * 0.5f};
    const float rad = s == "rect" ? 0.0f : static_cast<float>(std::clamp(num("radius", 6.0), 0.0, 256.0));
    if (s == "line") {
      if (layer == 1) p.line({0.0f, desc_.h * 0.5f}, {desc_.w, desc_.h * 0.5f}, std::max(0.5f, t), w);
      return;
    }
    if (s == "circle") {
      const Vec2 c{desc_.w * 0.5f, desc_.h * 0.5f};
      const float rr = std::min(desc_.w, desc_.h) * 0.5f - t * 0.5f;
      if (layer == 0) p.disc(c, rr, w);
      else if (t > 0.0f) p.ring(c, rr, t, w);
      return;
    }
    if (layer == 0) p.roundedRect(r, rad, w);
    else if (t > 0.0f) outlineRect(p, r, rad, t, w);
  }
  void paintLive(ui::FacePainter&, const ElementEnv&) override {}
  bool hit(Vec2) const noexcept override { return false; }  // a decoration: presses go through it
  // GU.5b: a circle at the bottom of a panel makes the panel round.
  std::optional<FaceShape> outline() const noexcept override {
    if (num("plate", 0.0) >= 0.5 && txt("shape", "round") == "round")  // GU.7: the plate's corner (a rounded rect)
      return FaceShape{false, {desc_.w * 0.5f, desc_.h * 0.5f}, static_cast<float>(std::clamp(num("radius", 6.0), 0.0, 256.0))};
    if (txt("shape", "round") != "circle") return std::nullopt;
    const float t = static_cast<float>(std::clamp(num("thickness", 1.0), 0.0, 32.0));
    return FaceShape{true, {desc_.w * 0.5f, desc_.h * 0.5f}, std::min(desc_.w, desc_.h) * 0.5f - t * 0.5f};
  }
};

// ---- tabs ---------------------------------------------------------------------------------------------------------

class Tabs final : public ValueElement {
public:
  using ValueElement::ValueElement;
  static constexpr StyleSpec kStyle[] = {
      {"items", StyleSpec::Type::Text, 0, "A|B"},
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"default", StyleSpec::Type::Number, 0.0, {}, {}, 0.0, 64.0},
  };
  explicit Tabs(doc::PanelElement d) : ValueElement(std::move(d)) { local_ = std::max(0.0, std::round(num("default", 0.0))); }
  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return ui::lerp(th.face.body, th.face.track, 0.6f); }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    const size_t n = items().size();
    for (size_t k = 0; k < n; ++k) p.roundedRect(cell(k, n), 3.0f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    const std::vector<std::string> it = items();
    const size_t sel = static_cast<size_t>(selection(*env.target).value_or(0.0));
    const Rgba accent = colour(th, "colour", "value", th.face.value);
    for (size_t k = 0; k < it.size(); ++k) {
      const Rect c = cell(k, it.size());
      if (k == sel) p.roundedRect(c, 3.0f, accent);
      else if (env.hovered && static_cast<int32_t>(k) == hoverCell_) outlineRect(p, c, 3.0f, 1.0f, th.face.hover);
      p.text({(c.x0 + c.x1) * 0.5f, (c.y0 + c.y1) * 0.5f}, it[k], kLabelPx, k == sel ? th.face.body : th.face.ink);
    }
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    const std::vector<std::string> it = items();
    if (e.kind == FacePointer::Kind::Move && !drag_) {
      const int32_t h = cellAt(e.local, it.size());
      const bool changed = h != hoverCell_;
      hoverCell_ = h;
      return changed;
    }
    if (e.kind != FacePointer::Kind::Down || e.button != ui::PointerButton::Left) return false;
    const int32_t k = cellAt(e.local, it.size());
    if (k < 0) return false;
    if (const doc::Binding* b = binding()) {
      if (editable(t)) t.setOnce(*b, indexPosition(static_cast<size_t>(k), it.size()), "Panel Tab");
    } else {
      local_ = static_cast<double>(k);
    }
    return true;
  }
  std::optional<double> selection(const PanelTarget& t) const override {
    const size_t n = std::max<size_t>(items().size(), 1);
    if (binding() != nullptr) return static_cast<double>(positionIndex(pos(t), n));
    return std::min(local_, static_cast<double>(n - 1));
  }
  void setLocalSelection(double v) override { local_ = v; }

private:
  std::vector<std::string> items() const {
    std::vector<std::string> v = splitItems(txt("items", "A|B"));
    if (v.empty()) v.emplace_back("A");
    return v;
  }
  Rect cell(size_t k, size_t n) const noexcept {
    const float gap = 2.0f, w = (desc_.w - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
    const float x0 = static_cast<float>(k) * (w + gap);
    return {x0, 0.5f, x0 + w, desc_.h - 0.5f};
  }
  int32_t cellAt(Vec2 l, size_t n) const noexcept {
    for (size_t k = 0; k < n; ++k) {
      const Rect c = cell(k, n);
      if (l.x >= c.x0 && l.x <= c.x1 && l.y >= c.y0 && l.y <= c.y1) return static_cast<int32_t>(k);
    }
    return -1;
  }
  std::string hint() const override { return " Click a tab."; }
  double local_ = 0.0;
  int32_t hoverCell_ = -1;
};

// ---- mod source -----------------------------------------------------------------------------------------------------

class ModSource final : public Element {
public:
  using Element::Element;
  static constexpr StyleSpec kStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "signal"},
      {"label", StyleSpec::Type::Text, 0, ""},
  };
  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return th.face.jackRing; }
  void paintStatic(ui::FacePainter& p, uint32_t) const override { p.ring(centre(), radius(), 1.5f, ui::rgba(255, 255, 255)); }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    const Rgba c = colour(th, "colour", "signal", th.face.signal);
    p.disc(centre(), radius() * 0.55f, c);
    if (env.hovered) {
      p.beginGlow();
      p.glow(centre(), radius() * 2.2f, ui::mulAlpha(c, 0.35f));
      p.endGlow();
    }
    std::string label(txt("label", ""));
    if (label.empty())
      if (const doc::Binding* b = binding()) label = b->name;
    if (label != "-" && !label.empty() && desc_.h >= 2.0f * radius() + 9.0f)
      p.text({desc_.w * 0.5f, desc_.h - 4.5f}, label, kLabelPx - 1.0f, th.face.ink);
  }
  void describe(const PanelTarget&, std::string& title, std::string& body) const override {
    const doc::Binding* b = binding();
    title = "Modulation source";
    body = b != nullptr ? "The output '" + b->name + "' of a block in the group. Drag it onto a control of the panel: the "
                                                     "cable from this output into that control (one undo step)."
                        : "Not bound to an output.";
  }
  // GU.5: bound to an output, a drag from the handle onto a control makes the cable (GroupPanelFace).
  bool interactive() const noexcept override { return signal(); }
  bool onPointer(const FacePointer& e, PanelTarget&) override {
    using K = FacePointer::Kind;
    if (e.kind == K::Down && e.button == ui::PointerButton::Left && signal()) return drag_ = true;
    if (e.kind == K::Move && drag_) return true;
    if (e.kind == K::Up && drag_) {
      drag_ = false;
      return true;
    }
    return false;
  }
  bool dragging() const noexcept override { return drag_; }
  void cancelDrag(PanelTarget&) override { drag_ = false; }
  ui::Cursor cursor() const noexcept override { return signal() ? ui::Cursor::Crosshair : ui::Cursor::Inherit; }
  std::optional<Vec2> cableHandle() const override { return signal() ? std::optional<Vec2>(centre()) : std::nullopt; }

private:
  bool signal() const noexcept {
    const doc::Binding* b = binding();
    return b != nullptr && b->kind == doc::BindingKind::Signal;
  }
  bool drag_ = false;
  Vec2 centre() const noexcept {
    const bool label = desc_.h >= 2.0f * std::min(desc_.w, desc_.h) * 0.3f + 9.0f;
    return {desc_.w * 0.5f, label ? (desc_.h - 9.0f) * 0.5f : desc_.h * 0.5f};
  }
  float radius() const noexcept { return std::max(2.0f, std::min(desc_.w, desc_.h - 9.0f) * 0.32f); }
};

// ---- placeholder (an unknown kind) ----------------------------------------------------------------------------------

class Unknown final : public Element {
public:
  using Element::Element;
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const std::array<Vec2, 5> box = {Vec2{0.5f, 0.5f}, {desc_.w - 0.5f, 0.5f}, {desc_.w - 0.5f, desc_.h - 0.5f}, {0.5f, desc_.h - 0.5f}, {0.5f, 0.5f}};
    p.dashed(box, 1.0f, 3.0f, 2.0f, env.theme->face.frame);
    p.text({desc_.w * 0.5f, desc_.h * 0.5f}, desc_.kind, kLabelPx, env.theme->face.ink);
  }
};

// ---- registry ------------------------------------------------------------------------------------------------------

struct Registered {
  ElementSchema schema;
  ElementFactory factory;
};
std::vector<Registered>& registry() {
  static std::vector<Registered> r;
  return r;
}

template <class T>
std::unique_ptr<Element> make(const doc::PanelElement& d) {
  return std::make_unique<T>(d);
}
std::unique_ptr<Element> makeToggle(const doc::PanelElement& d) { return std::make_unique<Toggle>(d, false); }
std::unique_ptr<Element> makeButton(const doc::PanelElement& d) { return std::make_unique<Toggle>(d, true); }

void registerGeneric() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto reg = [](std::string_view kind, std::string_view label, std::span<const StyleSpec> style, std::span<const std::string_view> slots,
                  float w, float h, ElementFactory f) {
      ElementSchema s;
      s.kind = kind;
      s.label = label;
      s.style = style;
      s.slots = slots;
      s.defaultW = w;
      s.defaultH = h;
      registry().push_back({s, f});
    };
    reg("knob", "Knob", Knob::kStyle, kValueSlot, 48, 60, &make<Knob>);
    reg("slider", "Slider", Slider::kStyle, kValueSlot, 120, 30, &make<Slider>);
    reg("number", "Number", Number::kStyle, kValueSlot, 56, 20, &make<Number>);
    reg("toggle", "Toggle", Toggle::kStyle, kValueSlot, 64, 16, &makeToggle);
    reg("button", "Button", Toggle::kButtonStyle, kValueSlot, 56, 20, &makeButton);
    reg("menu", "Menu", MenuElement::kStyle, kValueSlot, 80, 20, &make<MenuElement>);
    reg("xy", "XY Pad", XyPad::kStyle, kXySlots, 96, 96, &make<XyPad>);
    reg("label", "Label", Label::kStyle, kValueSlot, 80, 14, &make<Label>);
    reg("shape", "Shape", Shape::kStyle, {}, 80, 40, &make<Shape>);
    reg("tabs", "Tabs", Tabs::kStyle, kValueSlot, 120, 16, &make<Tabs>);
    reg("curve", "Curve Editor", curveElementStyle(), kValueSlot, 160, 96, &makeCurveElement);
    reg("modsource", "Mod Source", ModSource::kStyle, kSignalSlot, 24, 32, &make<ModSource>);
    registerRichElements(reg);  // GU.5b
    registerEmbedElement(reg);  // GU.6
    registerEngineElements(reg);  // GU.7
  });
}

}  // namespace

bool registerElement(const ElementSchema& schema, ElementFactory factory) {
  registerGeneric();
  for (const Registered& r : registry())
    if (r.schema.kind == schema.kind) return false;
  registry().push_back({schema, factory});
  return true;
}

std::unique_ptr<Element> makeElement(const doc::PanelElement& desc) {
  registerGeneric();
  for (const Registered& r : registry())
    if (r.schema.kind == desc.kind) return r.factory(desc);
  return std::make_unique<Unknown>(desc);
}

const ElementSchema* elementSchema(std::string_view kind) {
  registerGeneric();
  for (const Registered& r : registry())
    if (r.schema.kind == kind) return &r.schema;
  return nullptr;
}

std::vector<std::string_view> elementKinds() {
  registerGeneric();
  std::vector<std::string_view> v;
  for (const Registered& r : registry()) v.push_back(r.schema.kind);
  return v;
}

}  // namespace pq::views::panel
