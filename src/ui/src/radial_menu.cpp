// MX.1: the nested radial ("bubble") menu (pq/ui/radial_menu.h); SX.1: its controls (knob, slider, chips, toggle).

#include "pq/ui/radial_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

#include "controls_paint.h"
#include "pq/ui/lines.h"
#include "pq/ui/overlay.h"
#include "pq/ui/scene_textures.h"
#include "pq/ui/theme.h"

namespace pq::ui {

namespace {

// Logical px.
constexpr float kCentreR = 20.0f;   // the centre disc
constexpr float kMaxCentreR = 58.0f;  // with the opener's centre text
constexpr float kGap = 6.0f;        // between bubbles, and between rings
constexpr float kMinTopR = 26.0f;   // ring-0 bubbles at least
constexpr float kMinLeafR = 21.0f;  // deeper bubbles at least
constexpr float kMaxR = 44.0f;      // text wider than this is truncated
constexpr float kPad = 6.0f;        // text inside a bubble
constexpr float kEdge = 4.0f;       // kept from the window's edges
constexpr float kTagH = 18.0f;
constexpr float kKnobR = 2.0f * kMinLeafR;  // a knob's disc
constexpr float kSliderHH = 18.0f;          // a slider capsule's half height
constexpr float kSliderMinHW = 52.0f, kSliderMaxHW = 96.0f;
constexpr float kBandPillHH = 12.0f;  // SX.5 bands: a chip / toggle / leaf pill's half height
constexpr float kBandKnobR = 32.0f;   // a knob's disc in a band (at least)
constexpr double kOpenSeconds = 0.12;
constexpr double kWheelGestureSeconds = 0.4;
constexpr float kPi = std::numbers::pi_v<float>;

std::string lower(std::string_view s) {
  std::string r(s);
  for (char& c : r)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return r;
}

// 0 exact, 1 prefix, 2 word prefix, 3 substring, -1 none.
int32_t fieldScore(std::string_view field, std::string_view q) noexcept {
  if (field.empty() || q.empty()) return -1;
  if (field == q) return 0;
  if (field.starts_with(q)) return 1;
  for (size_t at = field.find(q); at != std::string_view::npos; at = field.find(q, at + 1)) {
    const char before = field[at - 1];  // at > 0: not a prefix
    if (!((before >= 'a' && before <= 'z') || (before >= '0' && before <= '9'))) return 2;
  }
  return field.find(q) != std::string_view::npos ? 3 : -1;
}

int32_t better(int32_t a, int32_t b) noexcept { return a < 0 ? b : b < 0 ? a : std::min(a, b); }

IRect unite(const IRect& a, const IRect& b) noexcept {
  if (a.empty()) return b;
  if (b.empty()) return a;
  const int32_t x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
  return {x0, y0, std::max(a.right(), b.right()) - x0, std::max(a.bottom(), b.bottom()) - y0};
}

IRect boxRect(Vec2 c, float hw, float hh) noexcept {
  const int32_t x0 = static_cast<int32_t>(std::floor(c.x - hw)), y0 = static_cast<int32_t>(std::floor(c.y - hh));
  const int32_t x1 = static_cast<int32_t>(std::ceil(c.x + hw)), y1 = static_cast<int32_t>(std::ceil(c.y + hh));
  return {x0, y0, x1 - x0, y1 - y0};
}
IRect discRect(Vec2 c, float r) noexcept { return boxRect(c, r, r); }

// Angle measured clockwise from the top, 0 .. 2π (ring-0 ordering).
float fromTop(float a) noexcept {
  float t = std::fmod(a + 0.5f * kPi, 2.0f * kPi);
  if (t < 0.0f) t += 2.0f * kPi;
  return t;
}

bool samePlace(const RadialPlace& a, const RadialPlace& b) noexcept {
  return a.side == b.side && (a.side != RadialPlace::Side::Angle || a.degrees == b.degrees);
}

double keyStepOf(const ValueSpec& s) noexcept {
  if (s.keyStep > 0.0) return s.keyStep;
  return std::max(s.step, (s.max - s.min) / 100.0);
}

double clampQuantise(const ValueSpec& s, double v) noexcept {
  const double lo = std::min(s.min, s.max), hi = std::max(s.min, s.max);
  if (!(v == v)) v = s.def;
  v = std::clamp(v, lo, hi);
  if (s.step > 0.0) v = std::clamp(lo + std::round((v - lo) / s.step) * s.step, lo, hi);
  return v;
}

}  // namespace

float RadialPlace::degreesFromTop() const noexcept {
  switch (side) {
    case Side::Right: return 90.0f;
    case Side::Bottom: return 180.0f;
    case Side::Left: return 270.0f;
    case Side::Angle: return degrees;
    default: return 0.0f;
  }
}

RadialItem RadialItem::leaf(std::string key, std::string label, std::string glyph, std::string keywords) {
  RadialItem it;
  it.key = std::move(key);
  it.label = std::move(label);
  it.glyph = std::move(glyph);
  it.keywords = std::move(keywords);
  return it;
}

RadialItem RadialItem::category(std::string label, std::vector<RadialItem> children) {
  RadialItem it;
  it.label = std::move(label);
  it.children = std::move(children);
  return it;
}

namespace {
RadialItem control(RadialKind kind, std::string key, std::string label, ValueSpec spec, RadialBinding binding, RadialPlace place) {
  RadialItem it;
  it.kind = kind;
  it.key = std::move(key);
  it.label = std::move(label);
  it.spec = std::move(spec);
  it.binding = std::make_shared<const RadialBinding>(std::move(binding));
  it.place = place;
  return it;
}
}  // namespace

RadialItem RadialItem::knob(std::string key, std::string label, ValueSpec spec, RadialBinding binding, RadialPlace place) {
  return control(RadialKind::Knob, std::move(key), std::move(label), std::move(spec), std::move(binding), place);
}

RadialItem RadialItem::slider(std::string key, std::string label, ValueSpec spec, RadialBinding binding, RadialPlace place) {
  return control(RadialKind::Slider, std::move(key), std::move(label), std::move(spec), std::move(binding), place);
}

RadialItem RadialItem::toggle(std::string key, std::string label, RadialBinding binding, RadialPlace place) {
  ValueSpec s;
  s.min = 0.0;
  s.max = 1.0;
  s.step = 1.0;
  return control(RadialKind::Toggle, std::move(key), std::move(label), s, std::move(binding), place);
}

RadialItem RadialItem::chips(std::string key, std::vector<RadialChoice> choices, RadialBinding binding, RadialPlace place) {
  return chips(std::move(key), std::move(choices), std::make_shared<const RadialBinding>(std::move(binding)), place);
}

RadialItem RadialItem::chips(std::string key, std::vector<RadialChoice> choices, std::shared_ptr<const RadialBinding> binding,
                             RadialPlace place) {
  RadialItem g;
  g.kind = RadialKind::Chip;
  g.key = key;
  g.place = place;
  g.binding = binding;
  g.chips_.reserve(choices.size());
  for (RadialChoice& c : choices) {
    RadialItem it;
    it.kind = RadialKind::Chip;
    it.key = key + "/" + c.label;
    it.label = std::move(c.label);
    it.chipValue = c.value;
    it.children = std::move(c.children);
    it.binding = binding;
    it.place = place;
    g.chips_.push_back(std::move(it));
  }
  return g;
}

// --- construction ---------------------------------------------------------------------------------------

RadialMenu::RadialMenu(std::vector<RadialItem> items, Vec2 anchor, RadialMenuOptions options, std::string name)
    : Widget(std::move(name)), anchor_(anchor), options_(std::move(options)) {
  setLayout(LayoutKind::Custom);
  setFocusable(true);
  setAcceptsTextInput(true);
  setCursor(Cursor::Arrow);
  flatten(items);
  // Everything the layout and the filter use is reserved here: nothing grows while the menu is open.
  bubbles_.reserve(nodes_.size() + 1);
  bubbleOf_.assign(nodes_.size(), -1);
  results_.reserve(nodes_.size());
  lit_.assign(nodes_.size(), 0);
  slots_.reserve(nodes_.size());
  order_.reserve(nodes_.size());
  filter_.reserve(64);
  filterL_.reserve(64);
  for (int32_t i = 0; i < static_cast<int32_t>(nodes_.size()); ++i) {
    const Node& n = nodes_[static_cast<size_t>(i)];
    if (n.count == 0 && n.kind == RadialKind::Item && !options_.current.empty() && n.key == options_.current) {
      current_ = i;
      break;
    }
  }
  if (current_ >= 0) {
    // Open the rings down to the current item and focus it.
    const int32_t depth = nodes_[static_cast<size_t>(current_)].depth;
    if (depth < kMaxDepth) {
      for (int32_t n = nodes_[static_cast<size_t>(current_)].parent; n >= 0; n = nodes_[static_cast<size_t>(n)].parent)
        path_[nodes_[static_cast<size_t>(n)].depth] = n;
      openDepth_ = depth;
      focusRing_ = depth;
      hi_ = current_;
    } else {
      current_ = -1;
    }
  }
  if (!options_.animate) progress_ = 1.0f;
  else progress_ = 0.0f;
  bands_ = options_.layout == RadialLayout::Bands;
  if (bands_) {
    rows_.reserve(nodes_.size());
    rowNodes_.reserve(nodes_.size());
  }
  readValues();  // values are read at open
}

void RadialMenu::flatten(std::vector<RadialItem>& items) {
  // A chip group's chips take its place among its siblings (one layout slot, by their group id).
  int32_t groups = 0;
  auto expand = [&groups](std::vector<RadialItem>& v) {
    bool any = false;
    for (const RadialItem& it : v) any = any || !it.chips_.empty();
    if (!any) return;
    std::vector<RadialItem> out;
    out.reserve(v.size() + 8);
    for (RadialItem& it : v) {
      if (it.chips_.empty()) {
        out.push_back(std::move(it));
        continue;
      }
      const int32_t g = groups++;
      for (RadialItem& c : it.chips_) {
        c.groupId_ = g;
        out.push_back(std::move(c));
      }
    }
    v = std::move(out);
  };
  expand(items);
  // Breadth first: every item's children are contiguous (first .. first + count).
  std::vector<RadialItem*> src;
  for (RadialItem& it : items) src.push_back(&it);
  rootCount_ = static_cast<int32_t>(items.size());
  nodes_.resize(src.size());
  for (size_t i = 0; i < src.size(); ++i) {
    RadialItem& it = *src[i];
    Node& n = nodes_[i];
    n.key = std::move(it.key);
    n.label = std::move(it.label);
    n.glyph = std::move(it.glyph);
    n.keyL = lower(n.key);
    n.labelL = lower(n.label);
    n.glyphL = lower(n.glyph);
    n.keywordsL = lower(it.keywords);
    n.kind = it.kind;
    n.place = it.place;
    n.spec = std::move(it.spec);
    n.resettable = it.resettable;
    n.chipValue = it.chipValue;
    n.group = it.groupId_;
    if (n.kind != RadialKind::Item && it.binding) {
      int32_t b = -1;
      for (size_t k = 0; k < bindings_.size() && b < 0; ++k)
        if (bindings_[k] == it.binding) b = static_cast<int32_t>(k);
      if (b < 0) {
        b = static_cast<int32_t>(bindings_.size());
        bindings_.push_back(std::move(it.binding));
      }
      n.binding = b;
    }
    if (!it.children.empty()) {
      expand(it.children);
      n.first = static_cast<int32_t>(src.size());
      n.count = static_cast<int32_t>(it.children.size());
      for (RadialItem& c : it.children) src.push_back(&c);
      const int32_t parent = static_cast<int32_t>(i), depth = n.depth + 1;
      nodes_.resize(src.size());  // n is invalid from here
      for (int32_t k = 0; k < nodes_[i].count; ++k) {
        Node& c = nodes_[static_cast<size_t>(nodes_[i].first + k)];
        c.parent = parent;
        c.depth = depth;
      }
    }
  }
}

RadialMenu* openRadialMenu(Widget& from, const IRect& anchor, std::vector<RadialItem> items, RadialMenuOptions options) {
  OverlayLayer* layer = OverlayLayer::of(from);
  if (!layer) return nullptr;
  const IRect a = OverlayLayer::anchorRect(from, anchor);
  const Vec2 c{static_cast<float>(a.x) + 0.5f * static_cast<float>(a.w), static_cast<float>(a.y) + 0.5f * static_cast<float>(a.h)};
  OverlayOptions o;
  o.anchor = layer->rect();
  o.placement = Placement::Free;  // the menu covers the layer: it takes every press itself
  o.dismissOnOutsideClick = true;
  o.takeFocus = true;
  o.onClosed = std::move(options.onClosed);
  options.onClosed = nullptr;
  if (options.layout == RadialLayout::Bands) {  // SX.5: the bands hug the anchor, centred on bandX (mapped like it)
    options.around = a;
    if (!std::isnan(options.bandX)) {
      const IRect x = OverlayLayer::anchorRect(from, {static_cast<int32_t>(std::floor(options.bandX)), anchor.y, 1, 1});
      options.bandX = static_cast<float>(x.x) + 0.5f * static_cast<float>(x.w);
    }
  }
  auto m = std::make_unique<RadialMenu>(std::move(items), c, std::move(options), layer->uniqueName("radial-menu"));
  auto& menu = static_cast<RadialMenu&>(layer->open(std::move(m), std::move(o)));
  if (menu.openProgress() < 1.0f) menu.requestFrame();
  return &menu;
}

// --- queries --------------------------------------------------------------------------------------------

int32_t RadialMenu::find(std::string_view keyOrLabel) const noexcept {
  for (size_t i = 0; i < nodes_.size(); ++i)
    if ((nodes_[i].count == 0 || nodes_[i].kind != RadialKind::Item) && nodes_[i].key == keyOrLabel) return static_cast<int32_t>(i);
  for (size_t i = 0; i < nodes_.size(); ++i)
    if (nodes_[i].label == keyOrLabel) return static_cast<int32_t>(i);
  return -1;
}

bool RadialMenu::shown(int32_t item) const noexcept {
  return item >= 0 && item < itemCount() && bubbleOf_[static_cast<size_t>(item)] >= 0;
}

Vec2 RadialMenu::centreOf(int32_t item) const noexcept {
  return shown(item) ? bubbles_[static_cast<size_t>(bubbleOf_[static_cast<size_t>(item)])].c : Vec2{-1.0f, -1.0f};
}

float RadialMenu::radiusOf(int32_t item) const noexcept {
  return shown(item) ? bubbles_[static_cast<size_t>(bubbleOf_[static_cast<size_t>(item)])].r : 0.0f;
}

float RadialMenu::halfWidthOf(int32_t item) const noexcept {
  return shown(item) ? bubbles_[static_cast<size_t>(bubbleOf_[static_cast<size_t>(item)])].hw : 0.0f;
}

int32_t RadialMenu::highlighted() const noexcept {
  if (!filter_.empty()) return shownResults_ > 0 ? results_[static_cast<size_t>(resHi_)].node : -1;
  return hi_;
}

bool RadialMenu::litOf(int32_t item) const noexcept {
  const Node& n = nodes_[static_cast<size_t>(item)];
  if (n.kind == RadialKind::Chip) return std::lround(n.value) == n.chipValue;
  if (n.kind == RadialKind::Toggle) return n.value != 0.0;
  return false;
}

int32_t RadialMenu::ringFirst(int32_t ring) const noexcept {
  return ring == 0 ? 0 : nodes_[static_cast<size_t>(path_[ring - 1])].first;
}

int32_t RadialMenu::ringCount(int32_t ring) const noexcept {
  return ring == 0 ? rootCount_ : nodes_[static_cast<size_t>(path_[ring - 1])].count;
}

// --- measuring and layout -------------------------------------------------------------------------------

void RadialMenu::measure() {
  UiTree* t = tree();
  const UiScale s = t->uiScale();
  const PaintResources* res = t->resources();
  TextSystem* ts = res ? res->text : nullptr;
  const FontId symbol = res && res->fonts ? res->fonts->symbol : kNoFont;
  const FontId title = res && res->uiFonts ? res->uiFonts->title : kNoFont;
  const FontId body = res && res->uiFonts ? res->uiFonts->body : kNoFont;
  const FontId small = res && res->uiFonts ? res->uiFonts->small : kNoFont;
  const FontId numeric = res && res->uiFonts ? res->uiFonts->numeric : kNoFont;
  const float pad = s.exact(kPad), maxR = s.exact(kMaxR);
  auto width = [&](FontId f, std::string_view text) -> float {
    if (ts && f != kNoFont) return static_cast<float>(ts->measure(f, text));
    return static_cast<float>(text.size()) * s.exact(7.0f);  // no text system (draw-list-only tests): an estimate
  };
  for (int32_t d = 0; d < kMaxDepth; ++d) ringR_[d] = s.exact(d == 0 ? kMinTopR : kMinLeafR);
  resultR_ = s.exact(kMinLeafR);
  for (Node& n : nodes_) {
    n.capsule = false;
    if (n.kind == RadialKind::Knob || n.kind == RadialKind::Slider) {
      n.labelFont = small;
      n.labelW = static_cast<int32_t>(width(small, n.label));
      if (n.kind == RadialKind::Knob) {
        n.hw = n.hh = s.exact(kKnobR);
      } else {
        // Wide enough for the label and the widest of the texts at min / max / default, side by side.
        const RadialBinding* b = bindingOf(static_cast<int32_t>(&n - nodes_.data()));
        float vw = 0.0f;
        for (const double v : {n.spec.min, n.spec.max, n.spec.def})
          vw = std::max(vw, width(numeric, b && b->format ? b->format(v) : formatValue(v, n.spec)));
        n.hh = s.exact(kSliderHH);
        n.hw = std::clamp(std::ceil(0.5f * (static_cast<float>(n.labelW) + vw) + pad + 0.6f * n.hh), s.exact(kSliderMinHW),
                          s.exact(kSliderMaxHW));
        n.capsule = true;
      }
      continue;
    }
    n.labelFont = n.count > 0 && n.kind == RadialKind::Item ? title : body;
    n.labelW = static_cast<int32_t>(width(n.labelFont, n.label));
    float need;
    if (!n.glyph.empty()) {
      n.glyphFont = symbol;
      float w = symbol != kNoFont ? width(symbol, n.glyph) : 1e9f;
      if (w > 2.0f * (maxR - pad)) {
        n.glyphFont = title;
        w = width(title, n.glyph);
      }
      need = 0.5f * w + pad;
    } else {
      need = 0.5f * static_cast<float>(n.labelW) + pad;
    }
    need = std::min(need, maxR);
    if (n.depth < kMaxDepth) ringR_[n.depth] = std::max(ringR_[n.depth], std::ceil(need));
    if (n.count == 0 && n.kind == RadialKind::Item) resultR_ = std::max(resultR_, std::ceil(need));
  }
  for (Node& n : nodes_) {
    const float ring = ringR_[std::min(n.depth, kMaxDepth - 1)];
    if (n.kind == RadialKind::Knob) n.hw = n.hh = std::max(n.hw, std::ceil(1.25f * ring));  // always the larger disc
    else if (n.kind != RadialKind::Slider) n.hw = n.hh = ring;
  }
  if (bands_) {
    // SX.5: pills sized to their label (or glyph), knobs a smaller disc that still fits the label; sliders as on a ring.
    const float hh = s.exact(kBandPillHH);
    for (Node& n : nodes_) {
      if (n.kind == RadialKind::Slider) continue;
      if (n.kind == RadialKind::Knob) {
        // The label is drawn 1.6 r wide with the text's padding.
        n.hw = n.hh = std::clamp(std::ceil((static_cast<float>(n.labelW) + s.exact(kPad) + s.exact(2.0f)) / 1.6f),
                                 s.exact(kBandKnobR), s.exact(kKnobR));
        continue;
      }
      n.labelFont = body;
      n.labelW = static_cast<int32_t>(width(body, n.label));
      const float w = n.glyph.empty() ? static_cast<float>(n.labelW) : width(n.glyphFont, n.glyph);
      n.hh = hh;
      n.hw = std::max(hh, std::ceil(0.5f * w + pad + 0.3f * hh));
    }
  }
  centreR_ = s.exact(kCentreR);
  gap_ = s.exact(kGap);
  scaleAt_ = s.factor;
  measuredWithText_ = ts != nullptr;
  measureCentre();
  laidOutFor_ = {};
}

void RadialMenu::measureCentre() {
  UiTree* t = tree();
  if (!t || centreText_.empty()) return;
  const UiScale s = t->uiScale();
  const PaintResources* res = t->resources();
  TextSystem* ts = res ? res->text : nullptr;
  const ThemeFonts* f = res ? res->uiFonts : nullptr;
  const std::string_view all = centreText_;
  const size_t nl = all.find('\n');
  const std::string_view l1 = all.substr(0, nl), l2 = nl == std::string_view::npos ? std::string_view{} : all.substr(nl + 1);
  auto width = [&](FontId font, std::string_view str) {
    return ts && f && font != kNoFont ? static_cast<float>(ts->measure(font, str)) : static_cast<float>(str.size()) * s.exact(7.0f);
  };
  const float w = std::max(width(f ? f->bodyStrong : kNoFont, l1), width(f ? f->small : kNoFont, l2));
  // It only grows while open: a changing state line does not make the rings jump back and forth.
  const float need = std::clamp(std::ceil(0.5f * w + s.exact(kPad)), s.exact(kCentreR), s.exact(kMaxCentreR));
  if (need > centreR_) {
    centreR_ = need;
    laidOutFor_ = {};
  }
}

void RadialMenu::ensureLayout() {
  UiTree* t = tree();
  if (!t || rect().empty()) return;
  const PaintResources* res = t->resources();
  if (scaleAt_ != t->scale() || (!measuredWithText_ && res && res->text)) measure();
  if (laidOutFor_ != rect()) relayout();
}

void RadialMenu::arrange(LayoutPass&) { ensureLayout(); }

float RadialMenu::tangentExt(int32_t node, float a) const noexcept {
  const Node& n = nodes_[static_cast<size_t>(node)];
  if (!n.capsule) return n.hw;
  return std::abs(std::sin(a)) * n.hw + std::abs(std::cos(a)) * n.hh;
}

float RadialMenu::radialExt(int32_t node, float a) const noexcept {
  const Node& n = nodes_[static_cast<size_t>(node)];
  if (!n.capsule) return n.hw;
  return std::abs(std::cos(a)) * n.hw + std::abs(std::sin(a)) * n.hh;
}

void RadialMenu::placeArc(int32_t ring, int32_t node0, int32_t n, float baseAngle, float R, float r) {
  const IRect vp = rect();
  auto nodeAt = [&](int32_t i) { return ring >= 0 ? node0 + i : results_[static_cast<size_t>(i)].node; };
  auto halfW = [&](int32_t i) { return ring >= 0 ? nodes_[static_cast<size_t>(nodeAt(i))].hw : r; };
  auto halfH = [&](int32_t i) { return ring >= 0 ? nodes_[static_cast<size_t>(nodeAt(i))].hh : r; };
  auto widthOf = [&](int32_t i) { return (2.0f * (ring >= 0 ? tangentExt(nodeAt(i), baseAngle) : r) + gap_) / R; };
  float total = 0.0f;
  for (int32_t i = 0; i < n; ++i) total += widthOf(i);
  const float k = total > 2.0f * kPi ? 2.0f * kPi / total : 1.0f;  // a full ring at most
  auto offset = [&](int32_t i) {
    float o = -0.5f * total * k;
    for (int32_t j = 0; j < i; ++j) o += widthOf(j) * k;
    return o + 0.5f * widthOf(i) * k;
  };
  auto at = [&](int32_t i, float delta) {
    const float a = baseAngle + delta + offset(i);
    return Vec2{centre_.x + R * std::cos(a), centre_.y + R * std::sin(a)};
  };
  auto inside = [&](int32_t i, Vec2 p) {
    const float mx = halfW(i) + gap_ * 0.5f, my = halfH(i) + gap_ * 0.5f;
    return p.x >= static_cast<float>(vp.x) + mx && p.x <= static_cast<float>(vp.right()) - mx &&
           p.y >= static_cast<float>(vp.y) + my && p.y <= static_cast<float>(vp.bottom()) - my;
  };
  // The smallest rotation (7.5° steps, alternating sides) that keeps every bubble inside the window.
  float delta = 0.0f;
  bool fits = false;
  for (int32_t kk = 0; kk <= 48 && !fits; ++kk) {
    const float d = static_cast<float>((kk + 1) / 2) * (kPi / 24.0f) * (kk % 2 == 1 ? 1.0f : -1.0f);
    fits = true;
    for (int32_t i = 0; i < n && fits; ++i) fits = inside(i, at(i, d));
    if (fits) delta = d;
  }
  for (int32_t i = 0; i < n; ++i) {
    Vec2 p = at(i, delta);
    if (!fits) {  // nothing fits (a tiny window): clamp each bubble inside it
      const float mx = halfW(i) + gap_ * 0.5f, my = halfH(i) + gap_ * 0.5f;
      const float lo[2] = {static_cast<float>(vp.x) + mx, static_cast<float>(vp.y) + my};
      const float hi[2] = {static_cast<float>(vp.right()) - mx, static_cast<float>(vp.bottom()) - my};
      p.x = lo[0] <= hi[0] ? std::clamp(p.x, lo[0], hi[0]) : 0.5f * (lo[0] + hi[0]);
      p.y = lo[1] <= hi[1] ? std::clamp(p.y, lo[1], hi[1]) : 0.5f * (lo[1] + hi[1]);
    }
    const int32_t node = nodeAt(i);
    bubbleOf_[static_cast<size_t>(node)] = static_cast<int32_t>(bubbles_.size());
    bubbles_.push_back({p, halfH(i), node, ring, halfW(i)});
  }
}

void RadialMenu::layoutRing0() {
  // Slots: one item, or a run of neighbours that are chips of one group or pinned to the same place.
  slots_.clear();
  for (int32_t i = 0; i < rootCount_;) {
    const Node& a = nodes_[static_cast<size_t>(i)];
    int32_t j = i + 1;
    while (j < rootCount_) {
      const Node& b = nodes_[static_cast<size_t>(j)];
      const bool sameGroup = a.group >= 0 && b.group == a.group;
      const bool samePin = a.place.pinned() && samePlace(a.place, b.place);
      if (!sameGroup && !samePin) break;
      ++j;
    }
    Slot s{i, j - i, 0.0f, 0.0f, 0.0f, a.place.pinned()};
    if (s.pinned) s.angle = -0.5f * kPi + a.place.degreesFromTop() * (kPi / 180.0f);
    slots_.push_back(s);
    i = j;
  }
  const int32_t ns = static_cast<int32_t>(slots_.size());
  bool anyPinned = false;
  for (const Slot& s : slots_) anyPinned = anyPinned || s.pinned;
  auto measureSlot = [&](Slot& s) {
    s.tw = 0.0f;
    s.rx = 0.0f;
    for (int32_t k = 0; k < s.count; ++k) {
      s.tw += 2.0f * tangentExt(s.first + k, s.angle) + gap_;
      s.rx = std::max(s.rx, radialExt(s.first + k, s.angle));
    }
  };
  for (int32_t i = 0; i < ns; ++i) {
    Slot& s = slots_[static_cast<size_t>(i)];
    if (!s.pinned) s.angle = -0.5f * kPi + (anyPinned ? 0.0f : 2.0f * kPi * static_cast<float>(i) / static_cast<float>(ns));
    measureSlot(s);
  }
  auto minR = [&] {
    float rx = 0.0f, total = 0.0f;
    for (const Slot& s : slots_) {
      rx = std::max(rx, s.rx);
      total += s.tw;
    }
    return std::max(centreR_ + gap_ + rx, total / (2.0f * kPi));
  };
  float R = minR();
  if (!anyPinned) {
    // Spread from the top, clockwise; the spare angle shared evenly (equal bubbles: equal steps).
    float used = 0.0f;
    for (const Slot& s : slots_) used += s.tw / R;
    const float spare = std::max(0.0f, 2.0f * kPi - used) / static_cast<float>(ns);
    for (int32_t i = 1; i < ns; ++i) {
      const Slot& p = slots_[static_cast<size_t>(i - 1)];
      Slot& s = slots_[static_cast<size_t>(i)];
      s.angle = p.angle + 0.5f * (p.tw + s.tw) / R + spare;
    }
  } else {
    // Each unpinned slot (in order) takes the middle of the largest gap the others leave; then neighbours that
    // overlap push each other apart, the pinned ones 4× stiffer (a pin is where an item wants to be, so a crowded
    // side spreads instead of the ring growing). R grows only when that cannot settle.
    float total = 0.0f;
    for (const Slot& s : slots_) total += s.tw;
    R = std::max(R, total / (2.0f * kPi * 0.85f));  // some air: pins move less
    const float maxR = 4.0f * static_cast<float>(std::max(rect().w, rect().h)) + 1.0f;
    auto byAngle = [&](int32_t a, int32_t b) {
      return fromTop(slots_[static_cast<size_t>(a)].angle) < fromTop(slots_[static_cast<size_t>(b)].angle);
    };
    order_.clear();
    for (int32_t i = 0; i < ns; ++i)
      if (slots_[static_cast<size_t>(i)].pinned) order_.push_back(i);
    std::sort(order_.begin(), order_.end(), byAngle);
    for (int32_t i = 0; i < ns; ++i) {
      Slot& s = slots_[static_cast<size_t>(i)];
      if (s.pinned) continue;
      float bestGap = -1e9f, bestAt = 0.0f;
      const size_t m = order_.size();
      for (size_t k = 0; k < m; ++k) {
        const Slot& a = slots_[static_cast<size_t>(order_[k])];
        const Slot& b = slots_[static_cast<size_t>(order_[(k + 1) % m])];
        const float ta = fromTop(a.angle);
        float tb = fromTop(b.angle);
        if (tb <= ta) tb += 2.0f * kPi;  // wraps (or the only one: the whole ring)
        const float from = ta + 0.5f * a.tw / R, to = tb - 0.5f * b.tw / R;
        if (to - from - s.tw / R > bestGap) {
          bestGap = to - from - s.tw / R;
          bestAt = 0.5f * (from + to);
        }
      }
      s.angle = -0.5f * kPi + bestAt;
      measureSlot(s);
      order_.push_back(i);
      std::sort(order_.begin(), order_.end(), byAngle);
    }
    const size_t m = order_.size();
    auto gapAfter = [&](size_t k) {  // the angle from order_[k] to the next, minus what both need
      const Slot& a = slots_[static_cast<size_t>(order_[k])];
      const Slot& b = slots_[static_cast<size_t>(order_[(k + 1) % m])];
      float d = fromTop(b.angle) - fromTop(a.angle);
      if (d <= 0.0f) d += 2.0f * kPi;
      return d - 0.5f * (a.tw + b.tw) / R;
    };
    for (int32_t round = 0; round < 8 && m >= 2; ++round) {
      for (int32_t iter = 0; iter < 64; ++iter) {
        bool moved = false;
        for (size_t k = 0; k < m; ++k) {
          const float over = -gapAfter(k);
          if (over <= 1e-4f) continue;
          Slot& a = slots_[static_cast<size_t>(order_[k])];
          Slot& b = slots_[static_cast<size_t>(order_[(k + 1) % m])];
          const float wa = a.pinned ? 0.25f : 1.0f, wb = b.pinned ? 0.25f : 1.0f;
          a.angle -= (over + 1e-4f) * wa / (wa + wb);
          b.angle += (over + 1e-4f) * wb / (wa + wb);
          moved = true;
        }
        if (!moved) break;
      }
      for (Slot& s : slots_) measureSlot(s);  // capsules' widths depend on where they ended
      bool clear = true;
      for (size_t k = 0; k < m && clear; ++k) clear = gapAfter(k) > -1e-3f;
      if (clear || R >= maxR) break;
      R = std::min(R * 1.08f, maxR);
    }
  }
  ringRadius_[0] = R;
  float rx = 0.0f;
  for (const Slot& s : slots_) rx = std::max(rx, s.rx);
  ringExt_[0] = rx;
}

// --- bands (SX.5) ---------------------------------------------------------------------------------------

bool RadialMenu::expanded(int32_t node) const noexcept {
  const Node& n = nodes_[static_cast<size_t>(node)];
  if (n.count == 0) return false;
  if (n.kind == RadialKind::Chip) return litOf(node);  // the chosen mode's settings
  if (n.kind != RadialKind::Item) return false;
  return n.depth < openDepth_ && n.depth < kMaxDepth && path_[n.depth] == node;  // an open category
}

void RadialMenu::layoutBands() {
  const IRect vp = rect();
  const UiScale s = tree()->uiScale();
  const float edge = s.exact(kEdge), pad = gap_, gapIn = 0.5f * gap_, gapOut = 1.5f * gap_;
  rows_.clear();
  rowNodes_.clear();
  auto measureRow = [&](Row& r) {
    r.w = 0.0f;
    r.h = 0.0f;
    for (int32_t k = 0; k < r.count; ++k) {
      const Node& n = nodes_[static_cast<size_t>(rowNodes_[static_cast<size_t>(r.first + k)])];
      r.w += 2.0f * n.hw;
      r.h = std::max(r.h, 2.0f * n.hh);
      if (k > 0) {
        const Node& p = nodes_[static_cast<size_t>(rowNodes_[static_cast<size_t>(r.first + k - 1)])];
        r.w += p.group >= 0 && p.group == n.group ? gapIn : gapOut;
      }
    }
  };
  // A row, then (directly outward) the rows of its expanded items' children, depth first.
  auto emitChildren = [&](auto& self, size_t row) -> void {
    const Row r = rows_[row];
    for (int32_t k = 0; k < r.count; ++k) {
      const int32_t node = rowNodes_[static_cast<size_t>(r.first + k)];
      if (!expanded(node)) continue;
      const Node& n = nodes_[static_cast<size_t>(node)];
      Row c{r.band, static_cast<int32_t>(rowNodes_.size()), n.count, 0.0f, 0.0f};
      for (int32_t i = 0; i < n.count; ++i) rowNodes_.push_back(n.first + i);
      measureRow(c);
      rows_.push_back(c);
      self(self, rows_.size() - 1);
    }
  };
  auto bandOf = [&](int32_t i) { return nodes_[static_cast<size_t>(i)].place.side == RadialPlace::Side::Top ? 0 : 1; };
  auto rowOf = [&](int32_t i) { return std::max<int32_t>(0, nodes_[static_cast<size_t>(i)].place.row); };
  for (int32_t band = 0; band < 2; ++band) {
    int32_t maxRow = -1;
    for (int32_t i = 0; i < rootCount_; ++i)
      if (bandOf(i) == band) maxRow = std::max(maxRow, rowOf(i));
    for (int32_t row = 0; row <= maxRow; ++row) {
      Row r{band, static_cast<int32_t>(rowNodes_.size()), 0, 0.0f, 0.0f};
      for (int32_t i = 0; i < rootCount_; ++i)
        if (bandOf(i) == band && rowOf(i) == row) rowNodes_.push_back(i);
      r.count = static_cast<int32_t>(rowNodes_.size()) - r.first;
      if (r.count == 0) continue;
      measureRow(r);
      rows_.push_back(r);
      emitChildren(emitChildren, rows_.size() - 1);
    }
  }
  float bw[2] = {0.0f, 0.0f}, bh[2] = {0.0f, 0.0f};
  int32_t nr[2] = {0, 0};
  for (const Row& r : rows_) {
    bw[r.band] = std::max(bw[r.band], r.w);
    bh[r.band] += r.h + (nr[r.band] > 0 ? gap_ : 0.0f);
    ++nr[r.band];
  }
  for (int32_t b = 0; b < 2; ++b)
    if (nr[b] > 0) {
      bw[b] += 2.0f * pad;
      bh[b] += 2.0f * pad;
    }

  // The rectangle hugged (on screen), the x the bands centre on.
  IRect A = intersect(options_.around, vp);
  if (A.empty()) A = {static_cast<int32_t>(std::lround(anchor_.x)), static_cast<int32_t>(std::lround(anchor_.y)), 0, 0};
  const float X = std::isnan(options_.bandX) ? static_cast<float>(A.x) + 0.5f * static_cast<float>(A.w) : options_.bandX;
  centre_ = {static_cast<float>(A.x) + 0.5f * static_cast<float>(A.w), static_cast<float>(A.y) + 0.5f * static_cast<float>(A.h)};
  haloR_ = 0.0f;

  // Sides: a band that does not fit its own side goes outward beyond the other side's band when both fit there.
  const float spaceAbove = static_cast<float>(A.y - vp.y) - edge - gap_;
  const float spaceBelow = static_cast<float>(vp.bottom() - A.bottom()) - edge - gap_;
  bool above[2] = {true, false};
  bandFlipped_[0] = bandFlipped_[1] = false;
  if (nr[0] > 0 && bh[0] > spaceAbove && bh[1] + (nr[1] > 0 ? gap_ : 0.0f) + bh[0] <= spaceBelow) above[0] = false;
  else if (nr[1] > 0 && bh[1] > spaceBelow && bh[0] + (nr[0] > 0 ? gap_ : 0.0f) + bh[1] <= spaceAbove) above[1] = true;
  bandFlipped_[0] = !above[0];
  bandFlipped_[1] = above[1];
  float upEdge = static_cast<float>(A.y) - gap_, downEdge = static_cast<float>(A.bottom()) + gap_;
  float y0[2] = {0.0f, 0.0f}, x0[2] = {0.0f, 0.0f};
  auto clampAxis = [](float v, float lo, float hi) { return lo <= hi ? std::clamp(v, lo, hi) : lo; };
  // Nearest the rectangle first: on top the "above" band, below the "below" band; a flipped one beyond.
  const int32_t order[2][2] = {{0, 1}, {1, 0}};
  for (int32_t side = 0; side < 2; ++side)
    for (const int32_t b : order[side]) {
      if (nr[b] == 0 || above[b] != (side == 0)) continue;
      if (side == 0) {
        y0[b] = upEdge - bh[b];
        upEdge = y0[b] - gap_;
      } else {
        y0[b] = downEdge;
        downEdge += bh[b] + gap_;
      }
    }
  for (int32_t b = 0; b < 2; ++b) {
    bandRect_[b] = {};
    if (nr[b] == 0) continue;
    y0[b] = clampAxis(y0[b], static_cast<float>(vp.y) + edge, static_cast<float>(vp.bottom()) - edge - bh[b]);
    x0[b] = clampAxis(X - 0.5f * bw[b], static_cast<float>(vp.x) + edge, static_cast<float>(vp.right()) - edge - bw[b]);
    y0[b] = std::round(y0[b]);
    x0[b] = std::round(x0[b]);
    bandRect_[b] = {static_cast<int32_t>(x0[b]), static_cast<int32_t>(y0[b]), static_cast<int32_t>(std::ceil(bw[b])),
                    static_cast<int32_t>(std::ceil(bh[b]))};
  }

  // Rows outward from the rectangle's side (row 0 nearest), each centred in its band.
  float cursor[2] = {above[0] ? y0[0] + bh[0] - pad : y0[0] + pad, above[1] ? y0[1] + bh[1] - pad : y0[1] + pad};
  for (const Row& r : rows_) {
    const int32_t b = r.band;
    float cy;
    if (above[b]) {
      cy = cursor[b] - 0.5f * r.h;
      cursor[b] -= r.h + gap_;
    } else {
      cy = cursor[b] + 0.5f * r.h;
      cursor[b] += r.h + gap_;
    }
    float x = x0[b] + 0.5f * (bw[b] - r.w);
    for (int32_t k = 0; k < r.count; ++k) {
      const int32_t node = rowNodes_[static_cast<size_t>(r.first + k)];
      const Node& n = nodes_[static_cast<size_t>(node)];
      if (k > 0) {
        const Node& p = nodes_[static_cast<size_t>(rowNodes_[static_cast<size_t>(r.first + k - 1)])];
        x += p.group >= 0 && p.group == n.group ? gapIn : gapOut;
      }
      bubbleOf_[static_cast<size_t>(node)] = static_cast<int32_t>(bubbles_.size());
      bubbles_.push_back({{x + n.hw, cy}, n.hh, node, n.depth, n.hw});
      x += 2.0f * n.hw;
    }
  }
}

void RadialMenu::relayout() {
  const IRect old = bbox_;
  const IRect vp = rect();
  laidOutFor_ = vp;
  bubbles_.clear();
  std::fill(bubbleOf_.begin(), bubbleOf_.end(), -1);
  shownResults_ = 0;
  tagNode_ = -1;
  if (vp.empty() || nodes_.empty()) {
    bbox_ = {};
    damage(old);
    return;
  }
  const UiScale s = tree()->uiScale();
  auto clampAxis = [](float v, float lo, float hi) { return lo <= hi ? std::clamp(v, lo, hi) : 0.5f * (lo + hi); };
  if (bands_) {
    layoutBands();  // SX.5
  } else {
    // Ring 0: a full ring around the centre, the first item at the top, clockwise (pinned items where they are pinned).
    layoutRing0();
    const float R0 = ringRadius_[0];
    const float m = R0 + ringExt_[0] + s.exact(kEdge);
    centre_ = {clampAxis(anchor_.x, static_cast<float>(vp.x) + m, static_cast<float>(vp.right()) - m),
               clampAxis(anchor_.y, static_cast<float>(vp.y) + m, static_cast<float>(vp.bottom()) - m)};
    for (const Slot& sl : slots_) {
      float a = sl.angle - 0.5f * sl.tw / R0;  // a run's items side by side, clockwise
      for (int32_t k = 0; k < sl.count; ++k) {
        const int32_t i = sl.first + k;
        const float w = (2.0f * tangentExt(i, sl.angle) + gap_) / R0;
        const float ai = sl.count == 1 ? sl.angle : a + 0.5f * w;
        a += w;
        const Node& n = nodes_[static_cast<size_t>(i)];
        bubbleOf_[static_cast<size_t>(i)] = static_cast<int32_t>(bubbles_.size());
        bubbles_.push_back({{centre_.x + R0 * std::cos(ai), centre_.y + R0 * std::sin(ai)}, n.hh, i, 0, n.hw});
      }
    }

    auto angleOf = [&](int32_t node) {
      const Vec2 p = bubbles_[static_cast<size_t>(bubbleOf_[static_cast<size_t>(node)])].c;
      return std::atan2(p.y - centre_.y, p.x - centre_.x);
    };
    if (filter_.empty()) {
      // Rings 1..openDepth_: each an arc outward from the item open on the ring inside it, clear of that ring's
      // bubbles in its direction (a knob beside the parent is larger than the parent).
      for (int32_t k = 1; k <= openDepth_ && k < kMaxDepth; ++k) {
        const int32_t parent = path_[k - 1];
        const Node& p = nodes_[static_cast<size_t>(parent)];
        const float base = angleOf(parent);
        float rx = 0.0f, tw = 0.0f;
        for (int32_t c = 0; c < p.count; ++c) {
          rx = std::max(rx, radialExt(p.first + c, base));
          tw += 2.0f * tangentExt(p.first + c, base) + gap_;
        }
        const float Rin = ringRadius_[k - 1];
        float inner = radialExt(parent, base);
        const float half = 0.5f * tw / std::max(Rin + inner + gap_ + rx, 1.0f);
        for (const Bubble& b : bubbles_) {
          if (b.ring != k - 1 || b.node == parent) continue;
          float d = std::abs(std::atan2(b.c.y - centre_.y, b.c.x - centre_.x) - base);
          if (d > kPi) d = 2.0f * kPi - d;
          const float bw = tangentExt(b.node, base) / std::max(Rin, 1.0f);
          if (d < half + bw) inner = std::max(inner, radialExt(b.node, base));
        }
        float R = Rin + inner + gap_ + rx;
        R = std::max(R, tw / (2.0f * kPi));  // a full ring at most
        ringRadius_[k] = R;
        ringExt_[k] = rx;
        placeArc(k, p.first, p.count, base, R, 0.0f);
      }
    } else {
      // The results: one ring outside ring 0, best first, centred at the top.
      const float r = resultR_;
      const float R = R0 + ringExt_[0] + gap_ + r;
      ringRadius_[kMaxDepth] = R;
      const int32_t capacity = std::max(1, static_cast<int32_t>(std::floor(2.0f * kPi * R / (2.0f * r + gap_))));
      shownResults_ = std::min(static_cast<int32_t>(results_.size()), capacity);
      resHi_ = std::clamp(resHi_, 0, std::max(0, shownResults_ - 1));
      if (shownResults_ > 0) placeArc(-1, 0, shownResults_, -0.5f * kPi, R, r);
    }
    // The halo behind ring 0 (it groups the categories and calms what is under them).
    haloR_ = R0 + ringExt_[0] + gap_;
  }

  // The highlighted leaf's name, outside its bubble when the bubble shows a glyph (or cuts the label).
  const int32_t h = highlighted();
  if (h >= 0 && shown(h)) {
    const Node& n = nodes_[static_cast<size_t>(h)];
    const Bubble& b = bubbles_[static_cast<size_t>(bubbleOf_[static_cast<size_t>(h)])];
    const bool cut = static_cast<float>(n.labelW) > 2.0f * (b.r - s.exact(kPad));
    const bool bubbleKind = n.kind == RadialKind::Item || n.kind == RadialKind::Chip || n.kind == RadialKind::Toggle;
    const bool leafLike = n.kind == RadialKind::Item ? n.count == 0 : bubbleKind;
    if (leafLike && !n.label.empty() && ((!n.glyph.empty() && n.label != n.glyph) || (n.glyph.empty() && cut))) {
      const float w = static_cast<float>(n.labelW) + 2.0f * s.exact(kPad), th = s.exact(kTagH);
      float dx = b.c.x - centre_.x, dy = b.c.y - centre_.y;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len < 1e-3f) dx = 0.0f, dy = -1.0f;
      else dx /= len, dy /= len;
      const float off = b.r + 0.5f * gap_ + std::abs(dx) * 0.5f * w + std::abs(dy) * 0.5f * th;
      float cx = b.c.x + dx * off, cy = b.c.y + dy * off;
      cx = clampAxis(cx, static_cast<float>(vp.x) + 0.5f * w + 1.0f, static_cast<float>(vp.right()) - 0.5f * w - 1.0f);
      cy = clampAxis(cy, static_cast<float>(vp.y) + 0.5f * th + 1.0f, static_cast<float>(vp.bottom()) - 0.5f * th - 1.0f);
      tag_ = {static_cast<int32_t>(std::lround(cx - 0.5f * w)), static_cast<int32_t>(std::lround(cy - 0.5f * th)),
              static_cast<int32_t>(std::lround(w)), static_cast<int32_t>(std::lround(th))};
      tagNode_ = h;
    }
  }

  // The painted bounds: the halo (rings) or the bands' panels, every bubble.
  IRect box = bands_ ? unite(bandRect_[0], bandRect_[1]) : discRect(centre_, haloR_);
  if (bands_) box = {box.x, box.y, box.w, box.h + s.px(2) + 1};  // the panels' drop shadow
  const float shadow = s.exact(2.0f) + 1.0f, mark = s.exact(4.0f);  // drop shadow below, the current item's ring
  for (const Bubble& b : bubbles_) box = unite(box, boxRect({b.c.x, b.c.y + shadow}, b.hw + mark, b.r + mark));
  if (tagNode_ >= 0) box = unite(box, {tag_.x - 1, tag_.y - 1, tag_.w + 2, tag_.h + s.px(2) + 3});
  box = {box.x - 2, box.y - 2, box.w + 4, box.h + 4};
  bbox_ = intersect(box, vp);
  damage(unite(old, bbox_));
}

void RadialMenu::damage(const IRect& r) noexcept {
  UiTree* t = tree();
  if (!t || r.empty()) return;
  t->damage().add(r.toRect());
  if (t->host()) t->host()->treeNeedsUpdate();
}

// --- state ----------------------------------------------------------------------------------------------

void RadialMenu::openTo(int32_t ring, int32_t node) {
  if (ring >= kMaxDepth - 1) return;
  path_[ring] = node;
  openDepth_ = ring + 1;
}

void RadialMenu::setHighlight(int32_t ring, int32_t node) {
  focusRing_ = ring;
  hi_ = node;
  if (nodes_[static_cast<size_t>(node)].count > 0) openTo(ring, node);
  else openDepth_ = ring;
  relayout();
}

int32_t RadialMenu::matchScore(const Node& n) const noexcept {
  const std::string_view q = filterL_;
  int32_t s = better(fieldScore(n.glyphL, q), fieldScore(n.labelL, q));
  s = better(s, fieldScore(n.keyL, q));
  const std::string_view kw = n.keywordsL;
  for (size_t b = 0; b < kw.size();) {
    size_t e = kw.find(' ', b);
    if (e == std::string_view::npos) e = kw.size();
    if (e > b) {
      const std::string_view tok = kw.substr(b, e - b);
      const int32_t t = tok == q ? 0 : tok.starts_with(q) ? 1 : tok.find(q) != std::string_view::npos ? 3 : -1;
      s = better(s, t);
    }
    b = e + 1;
  }
  return s;
}

bool RadialMenu::hasMatch(int32_t node) const noexcept { return lit_[static_cast<size_t>(node)] != 0; }

void RadialMenu::updateFilter() {
  filterL_.assign(filter_);
  for (char& c : filterL_)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  results_.clear();
  std::fill(lit_.begin(), lit_.end(), uint8_t{0});
  resHi_ = 0;
  if (!filter_.empty()) {
    for (int32_t i = 0; i < itemCount(); ++i) {
      const Node& n = nodes_[static_cast<size_t>(i)];
      if (n.count > 0 || n.kind != RadialKind::Item) continue;  // pick leaves only
      const int32_t sc = matchScore(n);
      if (sc < 0) continue;
      results_.push_back({sc, i});
      for (int32_t a = i; a >= 0; a = nodes_[static_cast<size_t>(a)].parent) lit_[static_cast<size_t>(a)] = 1;
    }
    std::sort(results_.begin(), results_.end(),
              [](const Result& a, const Result& b) { return a.score != b.score ? a.score < b.score : a.node < b.node; });
  }
  relayout();
}

int32_t RadialMenu::bubbleAt(Vec2 p) const noexcept {
  for (size_t i = bubbles_.size(); i-- > 0;) {
    const Bubble& b = bubbles_[i];
    // A circle, or a capsule: the distance from its centre segment.
    const float seg = std::max(0.0f, b.hw - b.r);
    const float dx = std::max(0.0f, std::abs(p.x - b.c.x) - seg), dy = p.y - b.c.y;
    if (dx * dx + dy * dy <= b.r * b.r) return static_cast<int32_t>(i);
  }
  return -1;
}

void RadialMenu::close() {
  if (closed_) return;
  finishAll(true);  // a running gesture ends (one undo step); a typed entry commits
  closed_ = true;
  if (auto* layer = dynamic_cast<OverlayLayer*>(parent())) layer->close(*this);
}

void RadialMenu::pick(int32_t item) {
  if (item < 0 || item >= itemCount() || nodes_[static_cast<size_t>(item)].count > 0 || isControl(item)) return;
  // Copy what runs: closing retires the menu (alive until the next frame), and the handler may rebuild its opener,
  // open another menu or close every popup. Nothing of the menu is touched after it.
  const std::string key = nodes_[static_cast<size_t>(item)].key;
  const std::function<void(std::string_view)> cb = std::move(options_.onPick);
  options_.onPick = nullptr;
  close();
  if (cb) cb(key);
}

// --- controls -------------------------------------------------------------------------------------------

const RadialBinding* RadialMenu::bindingOf(int32_t node) const noexcept {
  const int32_t b = nodes_[static_cast<size_t>(node)].binding;
  return b >= 0 ? bindings_[static_cast<size_t>(b)].get() : nullptr;
}

void RadialMenu::readValues() {
  if (closed_) return;
  bool rowsChanged = false;  // bands: a chip with children lit / unlit (its row shows / hides)
  for (size_t i = 0; i < nodes_.size(); ++i) {
    Node& n = nodes_[i];
    if (n.kind == RadialKind::Item || n.binding < 0) continue;
    const RadialBinding& b = *bindings_[static_cast<size_t>(n.binding)];
    n.enabled = !b.enabled || b.enabled();
    const double v = b.get ? b.get() : 0.0;
    if (bands_ && n.kind == RadialKind::Chip && n.count > 0)
      rowsChanged = rowsChanged || (std::lround(v) == n.chipValue) != (std::lround(n.value) == n.chipValue);
    if (n.kind == RadialKind::Knob || n.kind == RadialKind::Slider) {
      if (v != n.value || n.valueText.empty()) {
        n.value = v;
        n.valueText = b.format ? b.format(v) : formatValue(v, n.spec);  // on a change only
      }
    } else {
      n.value = v;
    }
  }
  if (options_.centreText) {
    std::string t = options_.centreText();
    if (t != centreText_) {
      centreText_ = std::move(t);
      if (scaleAt_ > 0.0f) measureCentre();
    }
  }
  if (rowsChanged && tree() && !laidOutFor_.empty()) relayout();
  damage(bbox_);
}

void RadialMenu::refresh() {
  if (closed_) return;
  if (options_.valid && !options_.valid()) {
    close();
    return;
  }
  readValues();
}

void RadialMenu::setControl(int32_t node, double v) {
  if (closed_ || node < 0) return;
  Node& n = nodes_[static_cast<size_t>(node)];
  if (n.kind == RadialKind::Knob || n.kind == RadialKind::Slider) v = clampQuantise(n.spec, v);
  if (v == n.value || n.binding < 0) return;
  const std::shared_ptr<const RadialBinding> keep = bindings_[static_cast<size_t>(n.binding)];  // held across the calls
  if (gesture_ != node || gestureAlt_) {
    endGesture();
    if (closed_) return;
    gesture_ = node;
    if (keep->begin) keep->begin();
    if (closed_) return;
  }
  if (keep->set) keep->set(v);
  if (closed_) return;
  readValues();  // other controls may follow (a mode enables its parameters)
}

void RadialMenu::setAlt(int32_t node, double v) {
  if (closed_ || node < 0) return;
  const Node& n = nodes_[static_cast<size_t>(node)];
  if (n.binding < 0) return;
  const std::shared_ptr<const RadialBinding> keep = bindings_[static_cast<size_t>(n.binding)];  // held across the calls
  if (!keep->alt) return;
  const RadialBinding& a = *keep->alt;
  v = clampQuantise(keep->altSpec, v);
  if (a.get && a.get() == v) return;
  if (gesture_ != node || !gestureAlt_) {
    endGesture();
    if (closed_) return;
    gesture_ = node;
    gestureAlt_ = true;
    if (a.begin) a.begin();
    if (closed_) return;
  }
  if (a.set) a.set(v);
  if (closed_) return;
  readValues();
}

void RadialMenu::endGesture() {
  if (gesture_ < 0) return;
  const int32_t node = gesture_;
  const bool alt = gestureAlt_;
  gesture_ = -1;
  gestureAlt_ = false;
  const int32_t bi = nodes_[static_cast<size_t>(node)].binding;
  if (bi >= 0) {
    const std::shared_ptr<const RadialBinding> keep = bindings_[static_cast<size_t>(bi)];
    const RadialBinding* b = alt ? keep->alt.get() : keep.get();
    if (b && b->end) b->end();
  }
  if (!closed_) readValues();
}

void RadialMenu::finishWheel() {
  if (wheelTimer_) cancelTimer(wheelTimer_);
  wheelTimer_ = 0;
  if (wheel_ < 0) return;
  wheel_ = -1;
  endGesture();
}

void RadialMenu::finishAll(bool commitTyping) {
  if (typing_ >= 0) endTypeIn(commitTyping);
  drag_ = -1;
  dragAlt_ = false;
  finishWheel();
  endGesture();
}

void RadialMenu::resetControl(int32_t node) {
  const Node& n = nodes_[static_cast<size_t>(node)];
  if (!n.resettable) return;
  setControl(node, n.spec.def);
  endGesture();
}

bool RadialMenu::activate(int32_t node) {
  const Node& n = nodes_[static_cast<size_t>(node)];
  if (!isControl(node) || !n.enabled) return false;
  if (n.kind == RadialKind::Knob || n.kind == RadialKind::Slider) {
    beginTypeIn(node);
  } else {
    setControl(node, n.kind == RadialKind::Toggle ? (n.value != 0.0 ? 0.0 : 1.0) : static_cast<double>(n.chipValue));
    endGesture();
  }
  return true;
}

void RadialMenu::bandStep(int32_t dir) {
  const int32_t n = static_cast<int32_t>(bubbles_.size());
  if (n == 0) return;
  const int32_t at = hi_ >= 0 ? bubbleOf_[static_cast<size_t>(hi_)] : -1;
  const int32_t i = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n;
  const Bubble& b = bubbles_[static_cast<size_t>(i)];
  focusRing_ = b.ring;
  hi_ = b.node;
  relayout();
}

void RadialMenu::beginTypeIn(int32_t node) {
  finishAll(true);
  const Node& n = nodes_[static_cast<size_t>(node)];
  if (!n.enabled) return;
  // The number alone, like the panels' type-in.
  char buf[64];
  double v = n.value;
  if (std::fabs(v) < 0.5 * std::pow(10.0, -n.spec.decimals)) v = 0.0;
  std::snprintf(buf, sizeof buf, "%.*f", std::clamp(n.spec.decimals, 0, 9), v);
  typeIn_.setText(buf);
  typeIn_.selectAll();
  typing_ = node;
  damage(bbox_);
}

void RadialMenu::endTypeIn(bool commit) {
  if (typing_ < 0) return;
  const int32_t node = typing_;
  typing_ = -1;
  damage(bbox_);
  if (!commit) return;
  const Node& n = nodes_[static_cast<size_t>(node)];
  const RadialBinding* b = bindingOf(node);
  const std::optional<double> v = b && b->parse ? b->parse(typeIn_.text()) : parseValue(typeIn_.text(), n.spec);
  if (!v) return;  // an unparsable entry reverts
  setControl(node, *v);
  endGesture();
}

void RadialMenu::sliderBar(int32_t node, float& x0, float& x1, float& y) const noexcept {
  const Bubble& b = bubbles_[static_cast<size_t>(bubbleOf_[static_cast<size_t>(node)])];
  const float inset = 0.75f * b.r;
  x0 = b.c.x - b.hw + inset;
  x1 = b.c.x + b.hw - inset;
  y = b.c.y + 0.42f * b.r;
}

Vec2 RadialMenu::sliderStart(int32_t item) const noexcept {
  if (!shown(item) || nodes_[static_cast<size_t>(item)].kind != RadialKind::Slider) return {-1.0f, -1.0f};
  float x0, x1, y;
  sliderBar(item, x0, x1, y);
  return {x0, y};
}

Vec2 RadialMenu::sliderEnd(int32_t item) const noexcept {
  if (!shown(item) || nodes_[static_cast<size_t>(item)].kind != RadialKind::Slider) return {-1.0f, -1.0f};
  float x0, x1, y;
  sliderBar(item, x0, x1, y);
  return {x1, y};
}

double RadialMenu::sliderValueAt(int32_t node, float x) const noexcept {
  const Node& n = nodes_[static_cast<size_t>(node)];
  float x0, x1, y;
  sliderBar(node, x0, x1, y);
  const double t = x1 > x0 ? std::clamp(static_cast<double>((x - x0) / (x1 - x0)), 0.0, 1.0) : 0.0;
  return n.spec.min + t * (n.spec.max - n.spec.min);
}

void RadialMenu::onTimer(uint32_t id) {
  if (id != wheelTimer_) return;
  wheelTimer_ = 0;
  wheel_ = -1;
  endGesture();
}

void RadialMenu::onFocusChanged(bool focused) {
  if (!focused) finishAll(true);  // the layer closed it (or focus was taken): nothing stays half done
}

void RadialMenu::onCaptureLost() {
  drag_ = -1;
  dragAlt_ = false;
  pressed_ = -1;
  endGesture();
}

// --- input ----------------------------------------------------------------------------------------------

bool RadialMenu::onPointer(const PointerEvent& e) {
  ensureLayout();
  if (closed_) return true;
  switch (e.type) {
    case PointerType::Move: {
      if (drag_ >= 0 && dragAlt_) {  // SX.2: an Alt-drag edits the alt value, vertically
        const RadialBinding* b = bindingOf(drag_);
        const ValueSpec& as = b->altSpec;
        const double lo = std::min(as.min, as.max), hi = std::max(as.min, as.max);
        const double perPx = (as.max - as.min) / std::max(1.0, static_cast<double>(tree()->uiScale().exact(static_cast<float>(as.dragPixels))));
        const double fine = (e.mods & mod::kShift) ? as.fineFactor : 1.0;
        dragRaw_ = std::clamp(dragRaw_ + static_cast<double>(dragLast_.y - e.pos.y) * perPx * fine, lo, hi);
        dragLast_ = e.pos;
        setAlt(drag_, dragRaw_);
        return true;
      }
      if (drag_ >= 0 && !shown(drag_)) {  // bands: its row went away under it (the document changed)
        drag_ = -1;
        endGesture();
        return true;
      }
      if (drag_ >= 0) {  // a knob / slider drag owns the pointer: nothing else reacts, its ring stays open
        Node& n = nodes_[static_cast<size_t>(drag_)];
        const double span = n.spec.max - n.spec.min, lo = std::min(n.spec.min, n.spec.max), hi = std::max(n.spec.min, n.spec.max);
        const double fine = (e.mods & mod::kShift) ? n.spec.fineFactor : 1.0;
        if (n.kind == RadialKind::Knob) {
          const double perPx = span / std::max(1.0, static_cast<double>(tree()->uiScale().exact(static_cast<float>(n.spec.dragPixels))));
          dragRaw_ = std::clamp(dragRaw_ + static_cast<double>(dragLast_.y - e.pos.y) * perPx * fine, lo, hi);
        } else if (e.mods & mod::kShift) {
          float x0, x1, y;
          sliderBar(drag_, x0, x1, y);
          dragRaw_ = std::clamp(dragRaw_ + static_cast<double>(e.pos.x - dragLast_.x) * span / std::max(1.0f, x1 - x0) * fine, lo, hi);
        } else {
          dragRaw_ = sliderValueAt(drag_, e.pos.x);
        }
        dragLast_ = e.pos;
        setControl(drag_, dragRaw_);
        return true;
      }
      const int32_t b = bubbleAt(e.pos);
      if (wheel_ >= 0 && (b < 0 || bubbles_[static_cast<size_t>(b)].node != wheel_)) finishWheel();  // left the control
      if (b < 0) return true;
      const Bubble& bb = bubbles_[static_cast<size_t>(b)];
      if (bb.ring < 0) {
        const int32_t i = b - rootCount_;  // ring 0 is laid out first, then the results
        if (i != resHi_) {
          resHi_ = i;
          relayout();
        }
      } else if (filter_.empty() && bb.node != hi_) {
        setHighlight(bb.ring, bb.node);
      }
      return true;
    }
    case PointerType::Down: {
      finishWheel();
      if (typing_ >= 0) endTypeIn(true);  // a press anywhere commits the typed entry
      if (closed_) return true;
      if (e.button != PointerButton::Left) {
        close();
        return true;
      }
      const int32_t b = bubbleAt(e.pos);
      if (b < 0 && bands_) {
        for (const IRect& r : bandRect_)
          if (r.contains(e.pos)) return true;  // a band's panel between its items: nothing
      }
      if (b < 0) {  // a press that misses every bubble: an outside click
        close();
        return true;
      }
      const Bubble& bb = bubbles_[static_cast<size_t>(b)];
      const int32_t node = bb.node, ring = bb.ring;
      const Node& n = nodes_[static_cast<size_t>(node)];
      if (isControl(node)) {
        if (!n.enabled) return true;
        if (ring >= 0 && hi_ != node && filter_.empty()) setHighlight(ring, node);
        if (n.kind == RadialKind::Knob || n.kind == RadialKind::Slider) {
          if ((e.mods & mod::kAlt) != 0) {
            if (const RadialBinding* ab = bindingOf(node); ab && ab->alt) {  // SX.2: Alt-drag edits the alt value
              drag_ = node;
              dragAlt_ = true;
              dragLast_ = e.pos;
              dragRaw_ = ab->alt->get ? ab->alt->get() : 0.0;
              return true;
            }
            resetControl(node);
            return true;
          }
          if (e.clickCount >= 2) {
            if (n.kind == RadialKind::Knob) beginTypeIn(node);
            else resetControl(node);
            return true;
          }
          drag_ = node;
          dragAlt_ = false;
          dragLast_ = e.pos;
          dragRaw_ = n.value;
          if (n.kind == RadialKind::Slider && (e.mods & mod::kShift) == 0) {
            setControl(node, sliderValueAt(node, e.pos.x));  // a press on the bar jumps there
            dragRaw_ = nodes_[static_cast<size_t>(node)].value;
          }
          return true;
        }
        pressed_ = node;  // chip / toggle: acts on the release over it
        return true;
      }
      pressed_ = node;
      if (n.count > 0) {
        if (!filter_.empty()) {  // a category while filtering: back to browsing, in it
          filter_.clear();
          updateFilter();
        }
        setHighlight(ring, node);
      }
      return true;
    }
    case PointerType::Up: {
      if (e.button != PointerButton::Left) return true;
      if (drag_ >= 0) {
        drag_ = -1;
        dragAlt_ = false;
        pressed_ = -1;
        endGesture();
        return true;
      }
      const int32_t p = pressed_;
      pressed_ = -1;
      const int32_t b = bubbleAt(e.pos);
      if (p < 0 || b < 0 || bubbles_[static_cast<size_t>(b)].node != p) return true;
      const Node& n = nodes_[static_cast<size_t>(p)];
      if (n.kind == RadialKind::Chip) {
        setControl(p, static_cast<double>(n.chipValue));
        endGesture();
      } else if (n.kind == RadialKind::Toggle) {
        setControl(p, n.value != 0.0 ? 0.0 : 1.0);
        endGesture();
      } else {
        pick(p);  // leaves only
      }
      return true;
    }
    case PointerType::Wheel: {
      if (drag_ >= 0) return true;
      const int32_t b = bubbleAt(e.pos);
      if (b < 0) return true;
      const int32_t node = bubbles_[static_cast<size_t>(b)].node;
      const Node& n = nodes_[static_cast<size_t>(node)];
      if ((n.kind != RadialKind::Knob && n.kind != RadialKind::Slider) || !n.enabled) return true;
      if ((e.mods & (mod::kCtrl | mod::kAlt | mod::kSuper)) != 0 || e.wheel.y == 0.0f) return true;
      if (typing_ >= 0) endTypeIn(true);
      if (wheel_ != node) {  // a new run
        finishWheel();
        wheel_ = node;
        wheelRaw_ = n.value;
      }
      double d = keyStepOf(n.spec) * ((e.mods & mod::kShift) ? n.spec.fineFactor : 1.0);
      if (n.spec.step > 0.0) d = std::max(d, n.spec.step);
      wheelRaw_ = std::clamp(wheelRaw_ + d * static_cast<double>(e.wheel.y), std::min(n.spec.min, n.spec.max),
                             std::max(n.spec.min, n.spec.max));
      setControl(node, wheelRaw_);
      if (gesture_ == node && !closed_) {  // (re)arm the run's end
        if (wheelTimer_) cancelTimer(wheelTimer_);
        wheelTimer_ = requestTimer(kWheelGestureSeconds);
      }
      return true;
    }
    default: return false;
  }
}

bool RadialMenu::onKey(const KeyEvent& e) {
  if (!e.down) return true;
  ensureLayout();
  if (closed_) return true;
  if (typing_ >= 0) {
    switch (e.key) {
      case key::kReturn:
      case key::kTab: endTypeIn(true); return true;
      case key::kEscape: endTypeIn(false); return true;
      default: {
        bool changed = false;
        if (tree() && typeIn_.handleKey(e, tree()->clipboard(), changed)) damage(bbox_);
        return true;  // modal
      }
    }
  }
  if (e.key == key::kEscape && (drag_ >= 0 || gesture_ >= 0 || wheel_ >= 0)) {
    drag_ = -1;
    dragAlt_ = false;
    finishWheel();
    endGesture();  // the edit so far stays (one undo step); Esc then goes on as usual
  }
  if (bands_) {  // SX.5: the arrows walk the shown items in layout order; Enter acts; Esc closes
    switch (e.key) {
      case key::kDown:
      case key::kRight: bandStep(1); return true;
      case key::kUp:
      case key::kLeft: bandStep(-1); return true;
      case key::kReturn: {
        if (e.repeat) return true;
        if (hi_ < 0 || !shown(hi_)) {
          bandStep(1);
          return true;
        }
        const Node& n = nodes_[static_cast<size_t>(hi_)];
        if (isControl(hi_)) {
          if (n.enabled) activate(hi_);
        } else if (n.count == 0) {
          pick(hi_);
        } else {
          setHighlight(n.depth, hi_);  // a category: its row opens
        }
        return true;
      }
      case key::kEscape: close(); return true;
      default: return true;
    }
  }
  if (!filter_.empty()) {
    switch (e.key) {
      case key::kDown:
      case key::kUp:
        if (shownResults_ > 0) {
          resHi_ = (resHi_ + (e.key == key::kDown ? 1 : shownResults_ - 1)) % shownResults_;
          relayout();
        }
        return true;
      case key::kReturn:
        if (!e.repeat && shownResults_ > 0) pick(results_[static_cast<size_t>(resHi_)].node);
        return true;
      case key::kEscape:
      case key::kLeft:
        filter_.clear();
        updateFilter();
        return true;
      case key::kBackspace: {
        size_t n = filter_.size();
        while (n > 0 && (static_cast<unsigned char>(filter_[n - 1]) & 0xC0) == 0x80) --n;  // continuation bytes
        filter_.resize(n > 0 ? n - 1 : 0);
        updateFilter();
        return true;
      }
      default: return true;  // modal: nothing reaches the keymaps underneath
    }
  }
  switch (e.key) {
    case key::kDown:
    case key::kUp: {
      const int32_t first = ringFirst(focusRing_), n = ringCount(focusRing_);
      if (n <= 0) return true;
      const bool inRing = hi_ >= first && hi_ < first + n;
      int32_t i;
      if (!inRing) i = e.key == key::kDown ? 0 : n - 1;
      else i = (hi_ - first + (e.key == key::kDown ? 1 : n - 1)) % n;
      setHighlight(focusRing_, first + i);
      return true;
    }
    case key::kRight:
    case key::kReturn: {
      if (e.repeat && e.key == key::kReturn) return true;
      if (hi_ < 0) {
        if (ringCount(focusRing_) > 0) setHighlight(focusRing_, ringFirst(focusRing_));
        return true;
      }
      const Node& n = nodes_[static_cast<size_t>(hi_)];
      if (e.key == key::kReturn && isControl(hi_) && n.enabled) {
        const RadialKind kind = n.kind;
        activate(hi_);  // a chip: chosen; with a ring, Enter also goes in
        if (kind != RadialKind::Chip || closed_ || nodes_[static_cast<size_t>(hi_)].count == 0) return true;
      }
      if (n.count == 0) {
        if (e.key == key::kReturn) pick(hi_);
        return true;
      }
      if (focusRing_ + 1 >= kMaxDepth) return true;
      // In: the current item if it is on that ring, else the first child.
      int32_t child = n.first;
      for (int32_t c = current_; c >= 0; c = nodes_[static_cast<size_t>(c)].parent)
        if (nodes_[static_cast<size_t>(c)].parent == hi_) child = c;
      openTo(focusRing_, hi_);
      setHighlight(focusRing_ + 1, child);
      return true;
    }
    case key::kLeft:
    case key::kEscape:
      if (focusRing_ > 0) setHighlight(focusRing_ - 1, path_[focusRing_ - 1]);
      else if (e.key == key::kEscape) close();
      return true;
    default: return true;
  }
}

bool RadialMenu::onText(const TextEvent& e) {
  if (closed_) return true;
  const std::string_view t = e.text();
  for (const char c : t)
    if (static_cast<unsigned char>(c) < 0x20) return true;
  if (t.empty()) return true;
  if (typing_ >= 0) {
    typeIn_.insert(t);
    damage(bbox_);
    return true;
  }
  if (bands_) return true;  // bands hold controls: no filtering
  filter_.append(t);
  updateFilter();
  return true;
}

void RadialMenu::onFrame(double now) {
  if (progress_ >= 1.0f) return;
  if (openedAt_ < 0.0) openedAt_ = now;
  progress_ = static_cast<float>(std::clamp((now - openedAt_) / kOpenSeconds, 0.0, 1.0));
  damage(bbox_);
  if (progress_ < 1.0f) requestFrame();
}

// --- paint ----------------------------------------------------------------------------------------------

void RadialMenu::paint(PaintContext& ctx) {
  ensureLayout();
  const Theme* th = ctx.res.theme;
  if (!th || bubbles_.empty() || intersect(ctx.clip, bbox_).empty()) return;
  const UiScale& s = ctx.scale;
  // Open animation: ease-out; the rings grow from 70 % about the centre and fade in.
  const float p = 1.0f - (1.0f - progress_) * (1.0f - progress_) * (1.0f - progress_);
  const float grow = 0.7f + 0.3f * p, a = p;
  auto at = [&](Vec2 c) { return Vec2{centre_.x + (c.x - centre_.x) * grow, centre_.y + (c.y - centre_.y) * grow}; };
  const float sizeK = 0.85f + 0.15f * p;
  DrawList& dl = ctx.dl;
  const ThemeFonts* f = ctx.res.uiFonts;
  const int32_t hiNode = highlighted();
  const bool filtering = !filter_.empty();

  if (!bands_) {
    dl.addCircleFilled(centre_, haloR_ * grow, mulAlpha(th->menu.shadow, a));
  } else {
    // SX.5: each band on a popup panel (grown about the hugged rectangle's centre like the items).
    const float rad = s.exact(th->radius.menu);
    for (const IRect& br : bandRect_) {
      if (br.empty()) continue;
      const Vec2 p0 = at({static_cast<float>(br.x), static_cast<float>(br.y)});
      const Vec2 p1 = at({static_cast<float>(br.right()), static_cast<float>(br.bottom())});
      const Rect box{p0.x, p0.y, p1.x, p1.y};
      dl.addRoundedRectFilled({box.x0, box.y0 + s.exact(2.0f), box.x1, box.y1 + s.exact(2.0f)}, rad, mulAlpha(th->menu.shadow, a));
      dl.addRoundedRectFilled(box, rad, mulAlpha(th->menu.fill, a));
      dl.addRoundedRectStroke(box, rad, mulAlpha(th->menu.border, a), s.exact(1.0f));
    }
  }

  auto text = [&](FontId font, Vec2 c, float r, std::string_view str, Rgba col) {
    if (font == kNoFont || !ctx.res.text || str.empty()) return;
    const int32_t h = static_cast<int32_t>(ctx.res.text->metrics(font).lineHeight);
    const int32_t pad = s.px(kPad) / 2;
    const IRect box{static_cast<int32_t>(std::lround(c.x - r)), static_cast<int32_t>(std::lround(c.y)) - h / 2,
                    static_cast<int32_t>(std::lround(2.0f * r)), h};
    detail::drawText(ctx, font, box, str, col, TextAlign::Center, pad, pad);
  };
  // The typed entry's field (centred on c, half width hw).
  auto field = [&](Vec2 c, float hw) {
    if (!f || !ctx.res.text) return;
    TextSystem& ts = *ctx.res.text;
    const int32_t h = s.px(18.0f);
    const IRect r{static_cast<int32_t>(std::lround(c.x - hw)), static_cast<int32_t>(std::lround(c.y)) - h / 2,
                  static_cast<int32_t>(std::lround(2.0f * hw)), h};
    detail::box(dl, r, s.exact(th->radius.control), th->surface.sunken, th->field.borderFocus);
    const TextRun run = ts.shape(f->numeric, typeIn_.text());
    const int32_t pad = s.px(4);
    const int32_t x0 = std::max(r.x + (r.w - run.advance) / 2, r.x + pad - std::max(0, run.advance - (r.w - 2 * pad)));
    dl.pushClip(detail::inset(r, 1).toRect());
    const float y0 = static_cast<float>(r.y + s.px(3)), y1 = static_cast<float>(r.bottom() - s.px(3));
    if (typeIn_.hasSelection()) {
      const float sa = static_cast<float>(x0 + run.caretX(typeIn_.selBegin())), sb = static_cast<float>(x0 + run.caretX(typeIn_.selEnd()));
      dl.addRectFilled({sa, y0, sb, y1}, th->text.selection);
    }
    const int32_t base = detail::capCentredBaseline(r.y, r.h, ts.metrics(f->numeric).pixelSize);
    TextSystem::drawRun(dl, run, {static_cast<float>(x0), static_cast<float>(base)}, th->text.primary);
    const float cx = static_cast<float>(x0 + run.caretX(typeIn_.caret()));
    dl.addRectFilled({cx, y0, cx + 1.0f, y1}, th->text.caret);
    dl.popClip();
  };

  // The centre: the typed text, else the opener's centre text, else the current item's glyph (rings only).
  if (!bands_) {
    dl.addCircleFilled(centre_, centreR_ * sizeK, mulAlpha(th->control.fill, a));
    dl.addCircleStroke(centre_, centreR_ * sizeK, mulAlpha(filtering ? th->accent.focus : th->menu.border, a), s.exact(1.0f));
  }
  if (bands_) {
    // no centre: the bands hug what they edit
  } else if (filtering) {
    if (f) text(f->title, centre_, centreR_, filter_, mulAlpha(th->accent.primary, a));
  } else if (!centreText_.empty() && f) {
    const std::string_view all = centreText_;
    const size_t nl = all.find('\n');
    if (nl == std::string_view::npos) {
      text(f->bodyStrong, centre_, centreR_, all, mulAlpha(th->text.primary, a));
    } else {
      const float dy = s.exact(8.0f);
      text(f->bodyStrong, {centre_.x, centre_.y - dy}, centreR_, all.substr(0, nl), mulAlpha(th->text.primary, a));
      text(f->small, {centre_.x, centre_.y + dy}, centreR_ * 0.92f, all.substr(nl + 1), mulAlpha(th->text.secondary, a));
    }
  } else if (current_ >= 0) {
    const Node& n = nodes_[static_cast<size_t>(current_)];
    const bool g = !n.glyph.empty();
    text(g ? n.glyphFont : n.labelFont, centre_, centreR_, g ? std::string_view(n.glyph) : std::string_view(n.label),
         mulAlpha(th->text.secondary, a));
  }

  // A thin spoke from each open item to the ring it opened (the path reads at a glance).
  if (!filtering && !bands_)
    for (int32_t k = 0; k < openDepth_; ++k) {
      const int32_t from = k == 0 ? -1 : path_[k - 1];
      const Vec2 c0 = from < 0 ? centre_ : at(centreOf(from));
      const Vec2 c1 = at(centreOf(path_[k]));
      const Vec2 seg[2] = {c0, c1};
      smoothPolyline(dl, seg, 2, mulAlpha(th->accent.primary, 0.5f * a), s.exact(1.0f));
    }

  for (const Bubble& b : bubbles_) {
    const Node& n = nodes_[static_cast<size_t>(b.node)];
    const Vec2 c = at(b.c);
    const float r = b.r * sizeK;
    const bool isHi = b.node == hiNode;
    const bool onPath = !filtering && b.ring >= 0 && b.ring < openDepth_ && path_[b.ring] == b.node &&
                        (!bands_ || n.kind == RadialKind::Item);  // bands: an open category (a lit chip shows lit)
    const bool dim = (filtering && b.ring >= 0 && !hasMatch(b.node)) || (isControl(b.node) && !n.enabled);
    const bool lit = filtering && b.ring >= 0 && hasMatch(b.node);
    const float ca = a * (isControl(b.node) && !n.enabled ? 0.6f : 1.0f);

    if (n.kind == RadialKind::Knob || n.kind == RadialKind::Slider) {
      // A control: never the selection fill (its value colours must read); hover lifts it, the gesture rings it.
      const bool active = b.node == drag_ || b.node == gesture_ || b.node == typing_;
      const Rgba fill = isHi || active ? th->surface.raised : th->menu.fill;
      const Rgba ink = n.enabled ? th->text.primary : th->text.disabled;
      const float hw = b.hw * sizeK;
      if (n.kind == RadialKind::Knob) {
        dl.addCircleFilled({c.x, c.y + s.exact(2.0f)}, r, mulAlpha(th->menu.shadow, ca));
        dl.addCircleFilled(c, r, mulAlpha(fill, ca));
        if (active) dl.addCircleStroke(c, r - s.exact(0.75f), mulAlpha(th->accent.primary, ca), s.exact(1.5f));
        else dl.addCircleStroke(c, r, mulAlpha(isHi ? th->accent.focus : th->menu.border, ca), s.exact(1.0f));
        if (f) text(f->small, {c.x, c.y - 0.6f * r}, 0.8f * r, n.label, mulAlpha(th->text.secondary, ca));
        // The value arc (from the minimum, or from 0 for a bipolar range) and the needle, Knob's travel.
        const float thick = s.exact(2.5f), rad = 0.42f * r;
        const float a0 = Knob::kStartAngle, a1 = Knob::kStartAngle + Knob::kSweep;
        const double span = n.spec.max - n.spec.min;
        const float t = span != 0.0 ? static_cast<float>(std::clamp((n.value - n.spec.min) / span, 0.0, 1.0)) : 0.0f;
        const float av = a0 + Knob::kSweep * t;
        float anchor = a0;
        if (n.spec.min < 0.0 && n.spec.max > 0.0) anchor = a0 + Knob::kSweep * static_cast<float>((0.0 - n.spec.min) / span);
        const Rgba trackCol = n.enabled ? th->control.track : th->control.fillDisabled;
        const Rgba valueCol = n.enabled ? th->control.value : th->control.valueDisabled;
        dl.addArcStroke(c, rad, a0, a1, mulAlpha(trackCol, ca), thick);
        if (std::fabs(av - anchor) > 1e-4f) dl.addArcStroke(c, rad, std::min(anchor, av), std::max(anchor, av), mulAlpha(valueCol, ca), thick);
        const Vec2 d{std::cos(av), std::sin(av)}, nrm{-d.y, d.x};
        const float nw = 0.5f * s.exact(1.75f), i0 = rad * 0.25f, i1 = rad - 0.5f * thick;
        const Vec2 quad[4] = {{c.x + d.x * i0 + nrm.x * nw, c.y + d.y * i0 + nrm.y * nw},
                              {c.x + d.x * i1 + nrm.x * nw, c.y + d.y * i1 + nrm.y * nw},
                              {c.x + d.x * i1 - nrm.x * nw, c.y + d.y * i1 - nrm.y * nw},
                              {c.x + d.x * i0 - nrm.x * nw, c.y + d.y * i0 - nrm.y * nw}};
        dl.addConvexPolyFilled(quad, mulAlpha(n.enabled ? th->control.needle : th->text.disabled, ca));
        const Vec2 vc{c.x, c.y + 0.62f * r};
        if (b.node == typing_) field(vc, 0.72f * r);
        else if (f) text(f->numeric, vc, 0.75f * r, n.valueText, mulAlpha(ink, ca));
      } else {
        const Rect box{c.x - hw, c.y - r, c.x + hw, c.y + r};
        dl.addRoundedRectFilled({box.x0, box.y0 + s.exact(2.0f), box.x1, box.y1 + s.exact(2.0f)}, r, mulAlpha(th->menu.shadow, ca));
        dl.addRoundedRectFilled(box, r, mulAlpha(fill, ca));
        if (active) dl.addRoundedRectStroke(box, r, mulAlpha(th->accent.primary, ca), s.exact(1.5f));
        else dl.addRoundedRectStroke(box, r, mulAlpha(isHi ? th->accent.focus : th->menu.border, ca), s.exact(1.0f));
        // Label left, value right on the upper line; the bar below.
        const float inset = 0.75f * r;
        const float x0 = c.x - hw + inset, x1 = c.x + hw - inset, by = c.y + 0.42f * r;
        const int32_t lh = s.px(16.0f);
        const IRect line{static_cast<int32_t>(std::lround(x0)), static_cast<int32_t>(std::lround(c.y - 0.3f * r)) - lh / 2,
                         static_cast<int32_t>(std::lround(x1 - x0)), lh};
        if (f) detail::drawText(ctx, f->small, line, n.label, mulAlpha(th->text.secondary, ca), TextAlign::Left, 0, 0);
        if (b.node == typing_) field({c.x + 0.5f * (x1 - x0) * 0.5f, c.y - 0.3f * r}, 0.25f * (x1 - x0) + s.exact(4.0f));
        else if (f) detail::drawText(ctx, f->numeric, line, n.valueText, mulAlpha(ink, ca), TextAlign::Right, 0, 0);
        const double span = n.spec.max - n.spec.min;
        const float t = span != 0.0 ? static_cast<float>(std::clamp((n.value - n.spec.min) / span, 0.0, 1.0)) : 0.0f;
        float t0 = 0.0f;
        if (n.spec.min < 0.0 && n.spec.max > 0.0) t0 = static_cast<float>((0.0 - n.spec.min) / span);
        const float th2 = s.exact(1.5f);
        const float xv = x0 + (x1 - x0) * t, xa = x0 + (x1 - x0) * t0;
        dl.addRoundedRectFilled({x0, by - th2, x1, by + th2}, th2, mulAlpha(n.enabled ? th->control.track : th->control.fillDisabled, ca));
        if (std::fabs(xv - xa) > 0.5f)
          dl.addRoundedRectFilled({std::min(xa, xv), by - th2, std::max(xa, xv), by + th2}, th2,
                                  mulAlpha(n.enabled ? th->control.value : th->control.valueDisabled, ca));
        dl.addCircleFilled({xv, by}, s.exact(4.0f), mulAlpha(n.enabled ? th->control.needle : th->text.disabled, ca));
      }
      continue;
    }

    const bool on = (n.kind == RadialKind::Chip || n.kind == RadialKind::Toggle) && litOf(b.node);
    Rgba fill = isHi ? th->selection.fill : onPath ? th->surface.raised : n.count > 0 ? th->surface.panel : th->menu.fill;
    if (on) fill = !n.enabled ? lerp(th->accent.primary, th->control.fillDisabled, 0.65f) : isHi ? detail::hoverOf(th->accent.primary) : th->accent.primary;
    if (dim) fill = mulAlpha(fill, 0.55f);
    const Rgba edgeCol = onPath || lit ? (on ? th->text.onAccent : th->accent.primary) : isHi && on ? th->accent.focus : th->menu.border;
    const float edgeW = s.exact(onPath || lit ? 1.5f : 1.0f), edgeIn = onPath || lit ? s.exact(0.75f) : 0.0f;
    if (b.hw > b.r + 0.01f) {  // SX.5: a pill (bands)
      const float hw = b.hw * sizeK;
      const Rect box{c.x - hw, c.y - r, c.x + hw, c.y + r};
      dl.addRoundedRectFilled({box.x0, box.y0 + s.exact(2.0f), box.x1, box.y1 + s.exact(2.0f)}, r,
                              mulAlpha(th->menu.shadow, ca * (dim ? 0.5f : 1.0f)));
      dl.addRoundedRectFilled(box, r, mulAlpha(fill, ca));
      dl.addRoundedRectStroke({box.x0 + edgeIn, box.y0 + edgeIn, box.x1 - edgeIn, box.y1 - edgeIn}, r - edgeIn, mulAlpha(edgeCol, ca), edgeW);
      if (b.node == current_) {
        const float o = s.exact(2.5f);
        dl.addRoundedRectStroke({box.x0 - o, box.y0 - o, box.x1 + o, box.y1 + o}, r + o, mulAlpha(th->accent.primary, a), s.exact(1.5f));
      }
    } else {
      dl.addCircleFilled({c.x, c.y + s.exact(2.0f)}, r, mulAlpha(th->menu.shadow, ca * (dim ? 0.5f : 1.0f)));
      dl.addCircleFilled(c, r, mulAlpha(fill, ca));
      dl.addCircleStroke(c, r - edgeIn, mulAlpha(edgeCol, ca), edgeW);
      if (b.node == current_) dl.addCircleStroke(c, r + s.exact(2.5f), mulAlpha(th->accent.primary, a), s.exact(1.5f));
    }
    const Rgba ink = on ? (n.enabled ? th->text.onAccent : th->control.fillDisabled)
                        : isHi ? th->selection.text : dim ? th->text.disabled : th->text.primary;
    const bool g = !n.glyph.empty();
    text(g ? n.glyphFont : n.labelFont, c, b.hw, g ? std::string_view(n.glyph) : std::string_view(n.label), mulAlpha(ink, ca));
  }

  // The highlighted leaf's name.
  if (tagNode_ >= 0 && f && a > 0.0f) {
    const Node& n = nodes_[static_cast<size_t>(tagNode_)];
    const float rad = s.exact(th->radius.menu);
    const Rect tr = tag_.toRect();
    dl.addRoundedRectFilled({tr.x0, tr.y0 + s.exact(2.0f), tr.x1, tr.y1 + s.exact(2.0f)}, rad, mulAlpha(th->menu.shadow, a));
    detail::box(dl, tag_, rad, mulAlpha(th->tooltip.fill, a), mulAlpha(th->tooltip.border, a));
    detail::drawText(ctx, f->body, tag_, n.label, mulAlpha(th->tooltip.text, a), TextAlign::Center, s.px(kPad), s.px(kPad));
  }
}

}  // namespace pq::ui
