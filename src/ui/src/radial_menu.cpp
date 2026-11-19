// MX.1: the nested radial ("bubble") menu (pq/ui/radial_menu.h).

#include "pq/ui/radial_menu.h"

#include <algorithm>
#include <cmath>
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
constexpr float kGap = 6.0f;        // between bubbles, and between rings
constexpr float kMinTopR = 26.0f;   // ring-0 bubbles at least
constexpr float kMinLeafR = 21.0f;  // deeper bubbles at least
constexpr float kMaxR = 44.0f;      // text wider than this is truncated
constexpr float kPad = 6.0f;        // text inside a bubble
constexpr float kEdge = 4.0f;       // kept from the window's edges
constexpr float kTagH = 18.0f;
constexpr double kOpenSeconds = 0.12;
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

IRect discRect(Vec2 c, float r) noexcept {
  const int32_t x0 = static_cast<int32_t>(std::floor(c.x - r)), y0 = static_cast<int32_t>(std::floor(c.y - r));
  const int32_t x1 = static_cast<int32_t>(std::ceil(c.x + r)), y1 = static_cast<int32_t>(std::ceil(c.y + r));
  return {x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

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
  filter_.reserve(64);
  filterL_.reserve(64);
  for (int32_t i = 0; i < static_cast<int32_t>(nodes_.size()); ++i)
    if (nodes_[static_cast<size_t>(i)].count == 0 && !options_.current.empty() && nodes_[static_cast<size_t>(i)].key == options_.current) {
      current_ = i;
      break;
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
}

void RadialMenu::flatten(std::vector<RadialItem>& items) {
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
    if (!it.children.empty()) {
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
  auto m = std::make_unique<RadialMenu>(std::move(items), c, std::move(options), layer->uniqueName("radial-menu"));
  auto& menu = static_cast<RadialMenu&>(layer->open(std::move(m), std::move(o)));
  if (menu.openProgress() < 1.0f) menu.requestFrame();
  return &menu;
}

// --- queries --------------------------------------------------------------------------------------------

int32_t RadialMenu::find(std::string_view keyOrLabel) const noexcept {
  for (size_t i = 0; i < nodes_.size(); ++i)
    if (nodes_[i].count == 0 && nodes_[i].key == keyOrLabel) return static_cast<int32_t>(i);
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

int32_t RadialMenu::highlighted() const noexcept {
  if (!filter_.empty()) return shownResults_ > 0 ? results_[static_cast<size_t>(resHi_)].node : -1;
  return hi_;
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
  const float pad = s.exact(kPad), maxR = s.exact(kMaxR);
  auto width = [&](FontId f, std::string_view text) -> float {
    if (ts && f != kNoFont) return static_cast<float>(ts->measure(f, text));
    return static_cast<float>(text.size()) * s.exact(7.0f);  // no text system (draw-list-only tests): an estimate
  };
  for (int32_t d = 0; d < kMaxDepth; ++d) ringR_[d] = s.exact(d == 0 ? kMinTopR : kMinLeafR);
  resultR_ = s.exact(kMinLeafR);
  for (Node& n : nodes_) {
    n.labelFont = n.count > 0 ? title : body;
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
    if (n.count == 0) resultR_ = std::max(resultR_, std::ceil(need));
  }
  centreR_ = s.exact(kCentreR);
  gap_ = s.exact(kGap);
  scaleAt_ = s.factor;
  measuredWithText_ = ts != nullptr;
  laidOutFor_ = {};
}

void RadialMenu::ensureLayout() {
  UiTree* t = tree();
  if (!t || rect().empty()) return;
  const PaintResources* res = t->resources();
  if (scaleAt_ != t->scale() || (!measuredWithText_ && res && res->text)) measure();
  if (laidOutFor_ != rect()) relayout();
}

void RadialMenu::arrange(LayoutPass&) { ensureLayout(); }

void RadialMenu::placeArc(int32_t ring, int32_t node0, int32_t n, float baseAngle, float R, float r) {
  const IRect vp = rect();
  const float margin = r + gap_ * 0.5f;
  const float lo[2] = {static_cast<float>(vp.x) + margin, static_cast<float>(vp.y) + margin};
  const float hi[2] = {static_cast<float>(vp.right()) - margin, static_cast<float>(vp.bottom()) - margin};
  const float step = std::min((2.0f * r + gap_) / R, 2.0f * kPi / static_cast<float>(n));
  const float half = 0.5f * static_cast<float>(n - 1);
  auto at = [&](int32_t i, float delta) {
    const float a = baseAngle + delta + (static_cast<float>(i) - half) * step;
    return Vec2{centre_.x + R * std::cos(a), centre_.y + R * std::sin(a)};
  };
  // The smallest rotation (7.5° steps, alternating sides) that keeps every bubble inside the window.
  float delta = 0.0f;
  bool fits = false;
  for (int32_t k = 0; k <= 48 && !fits; ++k) {
    const float d = static_cast<float>((k + 1) / 2) * (kPi / 24.0f) * (k % 2 == 1 ? 1.0f : -1.0f);
    fits = true;
    for (int32_t i = 0; i < n && fits; ++i) {
      const Vec2 p = at(i, d);
      fits = p.x >= lo[0] && p.x <= hi[0] && p.y >= lo[1] && p.y <= hi[1];
    }
    if (fits) delta = d;
  }
  for (int32_t i = 0; i < n; ++i) {
    Vec2 p = at(i, delta);
    if (!fits) {  // nothing fits (a tiny window): clamp each bubble inside it
      p.x = lo[0] <= hi[0] ? std::clamp(p.x, lo[0], hi[0]) : 0.5f * (lo[0] + hi[0]);
      p.y = lo[1] <= hi[1] ? std::clamp(p.y, lo[1], hi[1]) : 0.5f * (lo[1] + hi[1]);
    }
    const int32_t node = ring >= 0 ? node0 + i : results_[static_cast<size_t>(i)].node;
    bubbleOf_[static_cast<size_t>(node)] = static_cast<int32_t>(bubbles_.size());
    bubbles_.push_back({p, r, node, ring});
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

  // Ring 0: a full ring around the centre, the first item at the top, clockwise.
  const float r0 = ringR_[0];
  const int32_t n0 = rootCount_;
  ringRadius_[0] = std::max(centreR_ + gap_ + r0, static_cast<float>(n0) * (2.0f * r0 + gap_) / (2.0f * kPi));
  const float m = ringRadius_[0] + r0 + s.exact(kEdge);
  auto clampAxis = [](float v, float lo, float hi) { return lo <= hi ? std::clamp(v, lo, hi) : 0.5f * (lo + hi); };
  centre_ = {clampAxis(anchor_.x, static_cast<float>(vp.x) + m, static_cast<float>(vp.right()) - m),
             clampAxis(anchor_.y, static_cast<float>(vp.y) + m, static_cast<float>(vp.bottom()) - m)};
  for (int32_t i = 0; i < n0; ++i) {
    const float a = -0.5f * kPi + 2.0f * kPi * static_cast<float>(i) / static_cast<float>(n0);
    bubbleOf_[static_cast<size_t>(i)] = static_cast<int32_t>(bubbles_.size());
    bubbles_.push_back({{centre_.x + ringRadius_[0] * std::cos(a), centre_.y + ringRadius_[0] * std::sin(a)}, r0, i, 0});
  }

  auto angleOf = [&](int32_t node) {
    const Vec2 p = bubbles_[static_cast<size_t>(bubbleOf_[static_cast<size_t>(node)])].c;
    return std::atan2(p.y - centre_.y, p.x - centre_.x);
  };
  if (filter_.empty()) {
    // Rings 1..openDepth_: each an arc outward from the item open on the ring inside it.
    for (int32_t k = 1; k <= openDepth_ && k < kMaxDepth; ++k) {
      const int32_t parent = path_[k - 1];
      const Node& p = nodes_[static_cast<size_t>(parent)];
      const float r = ringR_[k];
      float R = ringRadius_[k - 1] + ringR_[k - 1] + gap_ + r;
      R = std::max(R, static_cast<float>(p.count) * (2.0f * r + gap_) / (2.0f * kPi));  // a full ring at most
      ringRadius_[k] = R;
      placeArc(k, p.first, p.count, angleOf(parent), R, r);
    }
  } else {
    // The results: one ring outside ring 0, best first, centred at the top.
    const float r = resultR_;
    const float R = ringRadius_[0] + r0 + gap_ + r;
    ringRadius_[kMaxDepth] = R;
    const int32_t capacity = std::max(1, static_cast<int32_t>(std::floor(2.0f * kPi * R / (2.0f * r + gap_))));
    shownResults_ = std::min(static_cast<int32_t>(results_.size()), capacity);
    resHi_ = std::clamp(resHi_, 0, std::max(0, shownResults_ - 1));
    if (shownResults_ > 0) placeArc(-1, 0, shownResults_, -0.5f * kPi, R, r);
  }

  // The highlighted leaf's name, outside its bubble when the bubble shows a glyph (or cuts the label).
  const int32_t h = highlighted();
  if (h >= 0 && shown(h)) {
    const Node& n = nodes_[static_cast<size_t>(h)];
    const Bubble& b = bubbles_[static_cast<size_t>(bubbleOf_[static_cast<size_t>(h)])];
    const bool cut = static_cast<float>(n.labelW) > 2.0f * (b.r - s.exact(kPad));
    if (n.count == 0 && !n.label.empty() && ((!n.glyph.empty() && n.label != n.glyph) || (n.glyph.empty() && cut))) {
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

  // The halo behind ring 0 (it groups the categories and calms what is under them) and the painted bounds.
  haloR_ = ringRadius_[0] + r0 + gap_;
  IRect box = discRect(centre_, haloR_);
  const float shadow = s.exact(2.0f) + 1.0f, mark = s.exact(4.0f);  // drop shadow below, the current item's ring
  for (const Bubble& b : bubbles_) box = unite(box, discRect({b.c.x, b.c.y + shadow}, b.r + mark));
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
      if (n.count > 0) continue;
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
    const float dx = p.x - b.c.x, dy = p.y - b.c.y;
    if (dx * dx + dy * dy <= b.r * b.r) return static_cast<int32_t>(i);
  }
  return -1;
}

void RadialMenu::close() {
  if (auto* layer = dynamic_cast<OverlayLayer*>(parent())) layer->close(*this);
}

void RadialMenu::pick(int32_t item) {
  if (item < 0 || item >= itemCount() || nodes_[static_cast<size_t>(item)].count > 0) return;
  // Copy what runs: closing retires the menu (alive until the next frame), and the handler may rebuild its opener,
  // open another menu or close every popup. Nothing of the menu is touched after it.
  const std::string key = nodes_[static_cast<size_t>(item)].key;
  const std::function<void(std::string_view)> cb = std::move(options_.onPick);
  options_.onPick = nullptr;
  close();
  if (cb) cb(key);
}

// --- input ----------------------------------------------------------------------------------------------

bool RadialMenu::onPointer(const PointerEvent& e) {
  ensureLayout();
  switch (e.type) {
    case PointerType::Move: {
      const int32_t b = bubbleAt(e.pos);
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
      if (e.button != PointerButton::Left) {
        close();
        return true;
      }
      const int32_t b = bubbleAt(e.pos);
      if (b < 0) {  // a press that misses every bubble: an outside click
        close();
        return true;
      }
      const Bubble& bb = bubbles_[static_cast<size_t>(b)];
      pressed_ = bb.node;
      if (nodes_[static_cast<size_t>(bb.node)].count > 0) {
        if (!filter_.empty()) {  // a category while filtering: back to browsing, in it
          filter_.clear();
          updateFilter();
        }
        setHighlight(bb.ring, bb.node);
      }
      return true;
    }
    case PointerType::Up: {
      if (e.button != PointerButton::Left) return true;
      const int32_t p = pressed_;
      pressed_ = -1;
      const int32_t b = bubbleAt(e.pos);
      if (p >= 0 && b >= 0 && bubbles_[static_cast<size_t>(b)].node == p) pick(p);  // leaves only
      return true;
    }
    case PointerType::Wheel: return true;
    default: return false;
  }
}

bool RadialMenu::onKey(const KeyEvent& e) {
  if (!e.down) return true;
  ensureLayout();
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
  const std::string_view t = e.text();
  for (const char c : t)
    if (static_cast<unsigned char>(c) < 0x20) return true;
  if (t.empty()) return true;
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

  dl.addCircleFilled(centre_, haloR_ * grow, mulAlpha(th->menu.shadow, a));

  auto text = [&](FontId font, Vec2 c, float r, std::string_view str, Rgba col) {
    if (font == kNoFont || !ctx.res.text || str.empty()) return;
    const int32_t h = static_cast<int32_t>(ctx.res.text->metrics(font).lineHeight);
    const int32_t pad = s.px(kPad) / 2;
    const IRect box{static_cast<int32_t>(std::lround(c.x - r)), static_cast<int32_t>(std::lround(c.y)) - h / 2,
                    static_cast<int32_t>(std::lround(2.0f * r)), h};
    detail::drawText(ctx, font, box, str, col, TextAlign::Center, pad, pad);
  };

  // The centre: the typed text, else the current item's glyph.
  dl.addCircleFilled(centre_, centreR_ * sizeK, mulAlpha(th->control.fill, a));
  dl.addCircleStroke(centre_, centreR_ * sizeK, mulAlpha(filtering ? th->accent.focus : th->menu.border, a), s.exact(1.0f));
  if (filtering) {
    if (f) text(f->title, centre_, centreR_, filter_, mulAlpha(th->accent.primary, a));
  } else if (current_ >= 0) {
    const Node& n = nodes_[static_cast<size_t>(current_)];
    const bool g = !n.glyph.empty();
    text(g ? n.glyphFont : n.labelFont, centre_, centreR_, g ? std::string_view(n.glyph) : std::string_view(n.label),
         mulAlpha(th->text.secondary, a));
  }

  // A thin spoke from each open item to the ring it opened (the path reads at a glance).
  if (!filtering)
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
    const bool onPath = !filtering && b.ring >= 0 && b.ring < openDepth_ && path_[b.ring] == b.node;
    const bool dim = filtering && b.ring >= 0 && !hasMatch(b.node);
    const bool lit = filtering && b.ring >= 0 && hasMatch(b.node);
    Rgba fill = isHi ? th->selection.fill : onPath ? th->surface.raised : n.count > 0 ? th->surface.panel : th->menu.fill;
    if (dim) fill = mulAlpha(fill, 0.55f);
    dl.addCircleFilled({c.x, c.y + s.exact(2.0f)}, r, mulAlpha(th->menu.shadow, a * (dim ? 0.5f : 1.0f)));
    dl.addCircleFilled(c, r, mulAlpha(fill, a));
    if (onPath || lit) dl.addCircleStroke(c, r - s.exact(0.75f), mulAlpha(th->accent.primary, a), s.exact(1.5f));
    else dl.addCircleStroke(c, r, mulAlpha(th->menu.border, a), s.exact(1.0f));
    if (b.node == current_) dl.addCircleStroke(c, r + s.exact(2.5f), mulAlpha(th->accent.primary, a), s.exact(1.5f));
    const Rgba ink = isHi ? th->selection.text : dim ? th->text.disabled : th->text.primary;
    const bool g = !n.glyph.empty();
    text(g ? n.glyphFont : n.labelFont, c, b.r, g ? std::string_view(n.glyph) : std::string_view(n.label), mulAlpha(ink, a));
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
