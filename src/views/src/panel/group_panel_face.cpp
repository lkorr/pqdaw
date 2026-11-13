// GU.3: a group's panel as a face, its element tile cache, the group inspector's panel and the PanelHost widget
// (pq/views/panel/group_panel_face.h).

#include "pq/views/panel/group_panel_face.h"

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>

#include "pq/document/edit_ops.h"
#include "pq/document/model.h"
#include "pq/ui/theme.h"
#include "pq/views/view_context.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

// ---- the element tile cache ---------------------------------------------------------------------------------------
// One coverage tile per (an element's staticKey: kind + style + size, layer, scale, atlas): shared by every element
// alike on every panel. UI thread only. Entries not drawn for a while go once the map has grown.
struct TileEntry {
  ui::LayerTile tile;
  uint32_t used = 0;
};
struct ElementTiles {
  std::unordered_map<uint64_t, TileEntry> map;
  ui::DrawList scratch;
  uint32_t epoch = 1;
  static uint64_t key(uint64_t staticKey, uint32_t layer, float scale, const ui::TileAtlas* atlas) noexcept {
    uint64_t h = staticKey * 0x9E3779B97F4A7C15ull;
    h ^= (static_cast<uint64_t>(layer) + 1) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<uint64_t>(std::bit_cast<uint32_t>(scale)) * 0x165667B19E3779F9ull;
    h ^= atlas->serial() * 0xD6E8FEB86659FD93ull;  // (never the address: a later atlas may reuse it)
    return h;
  }
  void prune() {
    if (map.size() <= 512) return;
    std::erase_if(map, [&](const auto& kv) { return kv.second.used + 64 < epoch; });
  }
};
ElementTiles& elementTiles() {
  static ElementTiles t;
  return t;
}

}  // namespace

// ---- GroupPanelFace -------------------------------------------------------------------------------------------------

GroupPanelFace::GroupPanelFace(Source source, std::unique_ptr<PanelTarget> target)
    : source_(std::move(source)), target_(std::move(target)) {
  (void)sync();
}

GroupPanelFace::~GroupPanelFace() = default;

bool GroupPanelFace::sync() {
  doc::PanelPtr p = source_ ? source_() : nullptr;
  if (p == panel_) return false;
  rebuild(p);
  return true;
}

bool GroupPanelFace::changedRects(std::vector<Rect>& out) {
  std::vector<int8_t>& memo = freshMemo();
  const bool known = liveKeys_.size() == elements_.size();
  liveKeys_.resize(elements_.size(), 0);
  for (size_t i = 0; i < elements_.size(); ++i) {
    const uint64_t key = shownNow(i, memo) ? elements_[i]->liveKey(*target_) | 1u : 0u;
    if (known && (key == Element::kAnyChange || key != liveKeys_[i])) {
      const Rect r = elements_[i]->rect();
      constexpr float kOut = 3.0f;  // a driven ring reaches a unit past a knob's rect
      out.push_back({r.x0 - kOut, r.y0 - kOut, r.x1 + kOut, r.y1 + kOut});
    }
    liveKeys_[i] = key;
  }
  return known;
}

void GroupPanelFace::rebuild(const doc::PanelPtr& p) {
  liveKeys_.clear();
  // Unbound tabs keep their selection (by element id) across the rebuild.
  std::vector<std::pair<uint32_t, double>> keep;
  for (const std::unique_ptr<Element>& e : elements_)
    if (e->binding() == nullptr)
      if (const std::optional<double> s = e->selection(*target_)) keep.emplace_back(e->id(), *s);
  if (capture_ >= 0 && static_cast<size_t>(capture_) < elements_.size()) elements_[static_cast<size_t>(capture_)]->cancelDrag(*target_);
  capture_ = focus_ = -1;
  cable_ = cableTarget_ = -1;
  hovered_ = kNone;
  panel_ = p;
  elements_.clear();
  whenSource_.clear();
  target_->bindingsChanged();
  if (!p) return;
  elements_.reserve(p->elements.size());
  for (const doc::PanelElement& d : p->elements) {
    elements_.push_back(makeElement(d));
    elements_.back()->bind(*target_, host_);  // GU.6: an embedded panel reads its child through the target
    for (const auto& [id, v] : keep)
      if (id == d.id) elements_.back()->setLocalSelection(v);
  }
  for (const doc::PanelElement& d : p->elements) whenSource_.push_back(d.when ? indexOf(d.when->element) : -1);
  round_ = false;
  corner_ = kCorner;
  if (!elements_.empty())
    if (const std::optional<FaceShape> o = elements_.front()->outline(); o && !o->round) {
      corner_ = o->radius;  // GU.7: a plate shape: the panel keeps its rectangle with that corner
    } else if (o) {
      round_ = true;
      roundShape_ = *o;
      const Rect r = elements_.front()->rect();
      roundShape_.centre = {roundShape_.centre.x + r.x0, roundShape_.centre.y + r.y0};
    }
}

FaceShape GroupPanelFace::shape() const noexcept {
  if (round_) return roundShape_;
  return {false, {0.0f, 0.0f}, corner_};
}

std::optional<FaceShape> panelShape(const doc::Panel& panel, const std::vector<doc::DevicePtr>* list) {
  if (panel.elements.empty()) return std::nullopt;
  const doc::PanelElement& bottom = panel.elements.front();
  if (const doc::DeviceId child = doc::embeddedGroup(bottom); child.valid()) {  // GU.6: the embedded child's outline
    if (list == nullptr) return std::nullopt;
    for (const doc::DevicePtr& d : *list)
      if (d->id == child && d->hasPanel()) {
        std::optional<FaceShape> o = panelShape(*d->panel, list);  // (groups nest as a tree: this ends)
        if (!o) return o;
        const doc::EmbedFit f = doc::embedFit(bottom.w, bottom.h, std::max(1.0f, d->panel->width), std::max(1.0f, d->panel->height));
        o->centre = {bottom.x + f.x + o->centre.x * f.k, bottom.y + f.y + o->centre.y * f.k};
        o->radius *= f.k;
        return o;
      }
    return std::nullopt;
  }
  const std::unique_ptr<Element> e = makeElement(panel.elements.front());
  std::optional<FaceShape> o = e->outline();
  if (o) o->centre = {o->centre.x + e->desc().x, o->centre.y + e->desc().y};
  return o;
}

int32_t GroupPanelFace::popupAt(Vec2 l) const {
  for (size_t k = elements_.size(); k-- > 0;)
    if (const std::optional<Rect> pr = elements_[k]->popupRect()) {
      const Rect r = elements_[k]->rect();
      if (l.x >= r.x0 + pr->x0 && l.x <= r.x0 + pr->x1 && l.y >= r.y0 + pr->y0 && l.y <= r.y0 + pr->y1) return static_cast<int32_t>(k);
    }
  return -1;
}

std::optional<Rect> GroupPanelFace::popupRect() const noexcept {
  for (const std::unique_ptr<Element>& e : elements_)
    if (const std::optional<Rect> pr = e->popupRect()) {
      const Rect r = e->rect();
      return Rect{r.x0 + pr->x0, r.y0 + pr->y0, r.x0 + pr->x1, r.y0 + pr->y1};
    }
  return std::nullopt;
}

void GroupPanelFace::paintPopup(ui::FacePainter& p, const FaceEnv& env) {
  if (env.theme == nullptr) return;
  ElementEnv ee;
  ee.theme = env.theme;
  ee.target = target_.get();
  ee.uiScale = env.uiScale;
  const float sc = p.transform().scale;
  const Vec2 o0 = p.transform().origin;
  for (size_t i = 0; i < elements_.size(); ++i) {
    Element& e = *elements_[i];
    if (!e.popupRect()) continue;
    const Rect r = e.rect();
    ui::FacePainter q = p.at({{o0.x + r.x0 * sc, o0.y + r.y0 * sc}, sc});
    ee.hovered = hovered_ == static_cast<int32_t>(i);
    ee.focused = focus_ == static_cast<int32_t>(i);
    e.paintPopup(q, ee);
  }
}

void GroupPanelFace::setPopupOverFace(bool on) {
  for (const std::unique_ptr<Element>& e : elements_) e->setPopupOverFace(on);
}

// GU.7: the host polls the panel while ANY element's reader is live (several displays and voice faces on one panel).
uint32_t GroupPanelFace::AnyReader::read(std::span<FaceVoice>) { return 0; }
uint64_t GroupPanelFace::AnyReader::serial() const noexcept { return 0; }
bool GroupPanelFace::AnyReader::live() const noexcept {
  for (const std::unique_ptr<Element>& e : face->elements_)
    if (VoiceTapReader* r = e->voiceReader(*face->target_); r != nullptr && r->live()) return true;
  return false;
}

VoiceTapReader* GroupPanelFace::voices() const noexcept {
  VoiceTapReader* one = nullptr;
  size_t n = 0;
  for (const std::unique_ptr<Element>& e : elements_)
    if (VoiceTapReader* r = e->voiceReader(*target_)) {
      one = r;
      ++n;
    }
  if (n <= 1) return one;
  any_.face = this;
  return &any_;
}

bool GroupPanelFace::pollVoices() {
  bool changed = false;
  for (const std::unique_ptr<Element>& e : elements_) changed = e->pollVoices(*target_) || changed;
  return changed;
}

bool GroupPanelFace::frameTick() {
  bool changed = false;
  for (const std::unique_ptr<Element>& e : elements_) changed = e->frameTick(*target_) || changed;
  return changed;
}

bool GroupPanelFace::ticking() const noexcept {
  for (const std::unique_ptr<Element>& e : elements_)
    if (e->ticking()) return true;
  return false;
}

bool GroupPanelFace::animating() const noexcept {
  for (const std::unique_ptr<Element>& e : elements_)
    if (e->animating()) return true;
  return false;
}

Vec2 GroupPanelFace::worldSize() const noexcept {
  if (!panel_) return {120.0f, 60.0f};
  return {std::max(1.0f, panel_->width), std::max(1.0f, panel_->height)};
}

int32_t GroupPanelFace::indexOf(uint32_t id) const noexcept {
  for (size_t i = 0; i < elements_.size(); ++i)
    if (elements_[i]->id() == id) return static_cast<int32_t>(i);
  return -1;
}

// memo: per element 0 unknown, 1 shown, 2 hidden (a condition's source is evaluated once per pass; chains of conditions
// follow their sources; a cycle reads as hidden).
bool GroupPanelFace::shownNow(size_t i, std::vector<int8_t>& memo) const {
  if (memo[i] != 0) return memo[i] == 1;
  const int32_t src = whenSource_[i];
  if (src < 0) return (memo[i] = 1) == 1;
  memo[i] = 2;  // (while it is evaluated: a cycle is hidden)
  bool on = shownNow(static_cast<size_t>(src), memo);
  if (!on) {  // EQ.3 (panel.h kWhenHidden): shown while its source is hidden
    for (const double want : elements_[i]->desc().when->values) on = on || want == doc::kWhenHidden;
  } else {
    const std::optional<double> v = elements_[static_cast<size_t>(src)]->selection(*target_);
    on = false;
    if (v)
      for (const double want : elements_[i]->desc().when->values) on = on || std::fabs(*v - want) < 1e-6;
  }
  memo[i] = on ? 1 : 2;
  return on;
}

std::vector<int8_t>& GroupPanelFace::freshMemo() const {
  memo_.assign(elements_.size(), 0);
  return memo_;
}

bool GroupPanelFace::shown(size_t i) const {
  std::vector<int8_t>& memo = freshMemo();
  return i < elements_.size() && shownNow(i, memo);
}

int32_t GroupPanelFace::elementAt(Vec2 l) const {
  if (const int32_t pop = popupAt(l); pop >= 0) return pop;  // GU.5b: an open popup is on top of everything
  std::vector<int8_t>& memo = freshMemo();
  for (size_t k = elements_.size(); k-- > 0;) {
    const Element& e = *elements_[k];
    if (!e.interactive() || !shownNow(k, memo)) continue;
    const Rect r = e.rect();
    if (e.hit({l.x - r.x0, l.y - r.y0})) return static_cast<int32_t>(k);
  }
  return -1;
}

ui::TileAtlas* GroupPanelFace::atlas() const noexcept {
  ui::Widget* w = host_ != nullptr ? host_->hostWidget() : nullptr;
  ui::UiTree* t = w != nullptr ? w->tree() : nullptr;
  return t != nullptr && t->resources() != nullptr ? t->resources()->tiles : nullptr;
}

// PF.2: element `e`'s live layer at `scale`, averaged over its rect: each triangle's area times its mean vertex colour
// (premultiplied), summed over the element's device area (coverage clamped to 1). Remade only when the element, its
// live key (a value moved; an element that cannot tell keeps its swatch until the scale changes) or the scale by more
// than a quarter changes: at this size its look is a pixel or two.
ui::Rgba GroupPanelFace::swatchOf(Element& e, const ElementEnv& env, float scale, Swatch& sw) {
  const uint64_t key = e.liveKey(*target_);
  const bool scaleOk = sw.scale > 0.0f && std::fabs(scale / sw.scale - 1.0f) <= 0.25f;
  if (sw.of == &e && scaleOk && (key == sw.key || key == Element::kAnyChange)) return sw.colour;
  if (!swatchList_) swatchList_ = std::make_unique<ui::DrawList>();
  ui::DrawList& dl = *swatchList_;
  dl.reset(4096, 4096);
  ui::FacePainter q(dl, {{0.0f, 0.0f}, scale});
  e.paintLive(q, env);
  const std::span<const ui::Vertex> v = dl.vertices();
  const std::span<const uint32_t> ix = dl.indices();
  double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
  for (size_t k = 0; k + 2 < ix.size(); k += 3) {
    const ui::Vertex& p0 = v[ix[k]];
    const ui::Vertex& p1 = v[ix[k + 1]];
    const ui::Vertex& p2 = v[ix[k + 2]];
    const double area = 0.5 * std::fabs(static_cast<double>(p1.pos.x - p0.pos.x) * (p2.pos.y - p0.pos.y) -
                                        static_cast<double>(p2.pos.x - p0.pos.x) * (p1.pos.y - p0.pos.y));
    for (const ui::Vertex* q3 : {&p0, &p1, &p2}) {
      const double w = area / 3.0 * ui::alphaOf(q3->col) / 255.0;
      r += w * ui::redOf(q3->col), g += w * ui::greenOf(q3->col), b += w * ui::blueOf(q3->col), a += w;
    }
  }
  const Rect rr = e.rect();
  const double cell = std::max(1e-6, static_cast<double>(rr.x1 - rr.x0) * (rr.y1 - rr.y0) * scale * scale);
  ui::Rgba col = 0;
  if (a > 0.0) {
    const auto ch = [&](double c) { return static_cast<uint32_t>(std::clamp(std::lround(c / a), 0l, 255l)); };
    col = ui::rgba(ch(r), ch(g), ch(b), static_cast<uint32_t>(std::lround(std::min(1.0, a / cell) * 255.0)));
  }
  sw = {&e, scale, key, col};
  return col;
}

void GroupPanelFace::paintLive(ui::FacePainter& p, const FaceEnv& env) {
  (void)sync();
  const ui::Theme& th = *env.theme;
  const Vec2 ws = worldSize();
  stats_ = {};
  // The plate: the meta faces' body and frame (GU.5b: a round panel's bottom element is its plate).
  if (!round_) {
    p.roundedRect({0.0f, 0.0f, ws.x, ws.y}, corner_, th.face.body);
    outlineRect(p, {0.6f, 0.6f, ws.x - 0.6f, ws.y - 0.6f}, std::max(0.0f, corner_ - 0.6f), 1.2f, th.face.frame);
  }
  ui::TileAtlas* at = atlas();
  ElementTiles& cache = elementTiles();
  const float sc = p.transform().scale;
  const Vec2 o0 = p.transform().origin;
  // PF.2: a panel only a few device px across (the project patch zoomed out to a whole project: hundreds of panels
  // ~4 px wide) is its plate and its elements' static layers (a tile quad each); the live layers (values, text, dots:
  // ~2k vertices a panel) are below a pixel there.
  const bool standIn = std::max(ws.x, ws.y) * sc < kStandInPx;
  tilesWanted_ = 0.0f;
  std::vector<int8_t>& memo = freshMemo();
  ElementEnv ee;
  ee.theme = &th;
  ee.target = target_.get();
  ee.uiScale = env.uiScale;
  for (size_t i = 0; i < elements_.size(); ++i) {
    if (!shownNow(i, memo)) continue;
    Element& e = *elements_[i];
    const Rect r = e.rect();
    const Vec2 o{std::round(o0.x + r.x0 * sc), std::round(o0.y + r.y0 * sc)};
    for (uint32_t layer = 0; layer < e.staticLayerCount(); ++layer) {
      const Rgba col = e.staticLayerColour(th, layer);
      if (ui::alphaOf(col) == 0) continue;
      bool drawn = false;
      if (at != nullptr) {
        const auto it = cache.map.find(ElementTiles::key(e.staticKey(), layer, sc, at));
        if (it != cache.map.end() && it->second.tile.scale == sc && ui::layerReady(*at, it->second.tile)) {
          ui::drawLayer(p.drawList(), *at, it->second.tile, o, sc, col);
          it->second.used = cache.epoch;
          drawn = true;
          ++stats_.fromTiles;
        } else if (!(it != cache.map.end() && it->second.tile.failed)) {
          tilesWanted_ = sc;
        }
      }
      if (!drawn) {
        ui::FacePainter q = p.at({o, sc});
        q.setTint(col);
        e.paintStatic(q, layer);
        ++stats_.direct;
      }
    }
    ++stats_.drawn;
    if (standIn) {  // PF.2: its live layer as one swatch of its mean colour
      if (swatches_.size() != elements_.size()) swatches_.assign(elements_.size(), {});
      ee.hovered = ee.focused = false;
      const ui::Rgba col = swatchOf(e, ee, sc, swatches_[i]);
      if (ui::alphaOf(col) != 0)
        p.drawList().addRoundedRectFilled({o.x, o.y, o.x + (r.x1 - r.x0) * sc, o.y + (r.y1 - r.y0) * sc}, 0.0f, col);
      continue;
    }
    ui::FacePainter q = p.at({o, sc});
    ee.hovered = hovered_ == static_cast<int32_t>(i);
    ee.focused = focus_ == static_cast<int32_t>(i);
    e.paintLive(q, ee);
  }
  // GU.5: a mod source's cable on the pointer: a hanging curve from its handle, the control a release wires outlined.
  if (cable_ >= 0 && static_cast<size_t>(cable_) < elements_.size()) {
    const Element& src = *elements_[static_cast<size_t>(cable_)];
    const Vec2 h = src.cableHandle().value_or(Vec2{src.desc().w * 0.5f, src.desc().h * 0.5f});
    const Vec2 a{src.desc().x + h.x, src.desc().y + h.y}, b = cableTo_;
    const float sag = std::min(40.0f, 0.2f * std::hypot(b.x - a.x, b.y - a.y));
    std::array<Vec2, 17> pts;
    for (size_t k = 0; k < pts.size(); ++k) {
      const float u = static_cast<float>(k) / static_cast<float>(pts.size() - 1);
      pts[k] = {a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u + sag * 4.0f * u * (1.0f - u)};
    }
    if (cableTarget_ >= 0 && static_cast<size_t>(cableTarget_) < elements_.size()) {
      const Rect r = elements_[static_cast<size_t>(cableTarget_)]->rect();
      outlineRect(p, {r.x0 - 2.0f, r.y0 - 2.0f, r.x1 + 2.0f, r.y1 + 2.0f}, 3.0f, 1.5f, th.circuit.selected);
    }
    p.polyline(pts, 2.4f, ui::mulAlpha(th.face.signal, 0.95f));
    p.disc(b, 2.5f, th.face.signal);
  }
  // The block's selection / hover rim.
  const Rgba rim = env.selected ? th.circuit.selected : (env.hovered && hovered_ == kFrame ? th.circuit.hover : 0u);
  if (round_) p.ring(roundShape_.centre, roundShape_.radius - 0.75f, env.selected ? 1.75f : 1.25f, rim);
  else outlineRect(p, {0.6f, 0.6f, ws.x - 0.6f, ws.y - 0.6f}, kCorner - 0.6f, env.selected ? 1.75f : 1.25f, rim);
}

int32_t GroupPanelFace::cableTargetAt(Vec2 l) const {
  const int32_t i = elementAt(l);
  if (i < 0 || i == cable_) return -1;
  const doc::Binding* b = elements_[static_cast<size_t>(i)]->binding();
  return b != nullptr && b->kind == doc::BindingKind::Control ? i : -1;
}

std::string GroupPanelFace::portKey(doc::DeviceId group, const doc::Binding& b) {
  if (b.device == group) return b.name;
  return b.name + "@" + std::to_string(b.device.value);
}

doc::Binding GroupPanelFace::controlOfKey(doc::DeviceId group, std::string_view key) {
  const size_t at = key.rfind('@');
  if (at == std::string_view::npos) return key.empty() ? doc::Binding{} : doc::controlBinding(group, std::string(key));
  uint32_t id = 0;
  for (const char c : key.substr(at + 1)) {
    if (c < '0' || c > '9' || id > 100000000u) return {};
    id = id * 10u + static_cast<uint32_t>(c - '0');
  }
  if (at == 0 || id == 0) return {};
  return doc::controlBinding(doc::DeviceId{id}, std::string(key.substr(0, at)));
}

std::string GroupPanelFace::paramAt(Vec2 local) const {
  const int32_t i = elementAt(local);
  if (i < 0) return {};
  const Element& el = *elements_[static_cast<size_t>(i)];
  const doc::Binding* b = el.controlAt({local.x - el.rect().x0, local.y - el.rect().y0});  // (GU.6: inside an embed)
  if (b == nullptr || b->kind != doc::BindingKind::Control || !b->meta.empty()) return {};
  return portKey(owner_, *b);
}

std::optional<ControlAnchor> GroupPanelFace::elementAnchor(size_t i) const {
  if (i >= elements_.size()) return std::nullopt;
  const Element& e = *elements_[i];
  const Rect r = e.rect();
  ControlAnchor a = e.anchor().value_or(ControlAnchor{{e.desc().w * 0.5f, e.desc().h * 0.5f}, 0.0f, e.desc().w * 0.5f});
  a.at = {a.at.x + r.x0, a.at.y + r.y0};
  return a;
}

std::optional<ControlAnchor> GroupPanelFace::bindingAnchor(const doc::Binding& want, bool shownOnly) const {
  if (!want.bound()) return std::nullopt;
  std::vector<int8_t>& memo = freshMemo();
  for (size_t i = 0; i < elements_.size(); ++i) {
    if (shownOnly && !shownNow(i, memo)) continue;
    if (std::optional<ControlAnchor> a = elements_[i]->anchorFor(want, shownOnly)) {  // (GU.6: an embed's inner control)
      const Rect r = elements_[i]->rect();
      a->at = {a->at.x + r.x0, a->at.y + r.y0};
      return a;
    }
  }
  return std::nullopt;
}

std::optional<ControlAnchor> GroupPanelFace::controlAnchor(std::string_view param) const {
  return bindingAnchor(controlOfKey(owner_, param), true);
}

std::optional<ControlAnchor> GroupPanelFace::controlHome(std::string_view param) const {
  if (std::optional<ControlAnchor> now = controlAnchor(param)) return now;
  return bindingAnchor(controlOfKey(owner_, param), false);  // on another tab: where it is placed
}

void GroupPanelFace::liveRects(std::vector<Rect>& out) const {
  const Vec2 ws = worldSize();
  out.push_back({0.0f, 0.0f, ws.x, ws.y});
  if (const std::optional<Rect> pr = popupRect()) out.push_back(*pr);
}

bool GroupPanelFace::needsPrepare(float pxPerUnit) const noexcept {
  if (tilesWanted_ > 0.0f && tilesWanted_ == pxPerUnit && atlas() != nullptr) return true;
  for (const std::unique_ptr<Element>& e : elements_)
    if (e->needsPrepare(pxPerUnit)) return true;  // GU.6: an embedded panel's own tiles
  return false;
}

void GroupPanelFace::attach(FaceHost* host) {
  host_ = host;
  for (const std::unique_ptr<Element>& e : elements_) e->bind(*target_, host_);
}

bool GroupPanelFace::prepare(float pxPerUnit) {
  ui::TileAtlas* at = atlas();
  if (at == nullptr) return false;
  tilesWanted_ = 0.0f;
  ElementTiles& cache = elementTiles();
  ++cache.epoch;
  bool built = false;
  std::vector<int8_t>& memo = freshMemo();
  for (size_t i = 0; i < elements_.size(); ++i) {
    if (!shownNow(i, memo)) continue;
    const Element& e = *elements_[i];
    for (uint32_t layer = 0; layer < e.staticLayerCount(); ++layer) {
      TileEntry& t = cache.map[ElementTiles::key(e.staticKey(), layer, pxPerUnit, at)];
      t.used = cache.epoch;
      if (t.tile.scale == pxPerUnit && (ui::layerReady(*at, t.tile) || t.tile.failed)) continue;
      ui::LayerTile fresh;
      (void)ui::recordLayer(*at, cache.scratch, e.desc().w, e.desc().h, pxPerUnit, [&](ui::FacePainter& p) { e.paintStatic(p, layer); }, fresh);
      t.tile = fresh;
      built = true;
      ++stats_.tilesBuilt;
    }
  }
  for (size_t i = 0; i < elements_.size(); ++i)
    if (shownNow(i, memo)) built = elements_[i]->prepare(pxPerUnit) || built;  // GU.6: an embedded panel's
  cache.prune();
  return built;
}

int32_t GroupPanelFace::hitTest(Vec2 l, float) const noexcept {
  if (const int32_t pop = popupAt(l); pop >= 0) return pop;  // GU.5b: outside the panel too
  const Vec2 ws = worldSize();
  if (l.x < 0.0f || l.y < 0.0f || l.x > ws.x || l.y > ws.y) return kNone;
  const int32_t i = elementAt(l);
  return i >= 0 ? i : kFrame;
}

bool GroupPanelFace::onPointer(const FacePointer& e) {
  using K = FacePointer::Kind;
  (void)sync();
  auto local = [&](int32_t i) {
    FacePointer x = e;
    const Rect r = elements_[static_cast<size_t>(i)]->rect();
    x.local = {e.local.x - r.x0, e.local.y - r.y0};
    return x;
  };
  if (capture_ >= 0 && static_cast<size_t>(capture_) < elements_.size()) {
    const int32_t c = capture_;
    if (e.kind == K::Up) capture_ = -1;
    if (e.kind == K::Leave) return false;
    const bool used = elements_[static_cast<size_t>(c)]->onPointer(local(c), *target_) || e.kind == K::Up || e.kind == K::Move;
    // GU.5: a mod source's cable follows the pointer; released on a control, the cable is made (one undo step).
    if (c == cable_) {
      cableTo_ = e.local;
      cableTarget_ = cableTargetAt(e.local);
      if (e.kind == K::Up) {
        const int32_t to = cableTarget_;
        cable_ = cableTarget_ = -1;
        if (to >= 0) {
          const doc::Binding from = *elements_[static_cast<size_t>(c)]->binding();
          const doc::Binding into = *elements_[static_cast<size_t>(to)]->binding();
          (void)target_->connect(from, into);  // (the host's scene may rebuild; this face stays)
        }
      }
    }
    return used;
  }
  switch (e.kind) {
    case K::Down: {
      const int32_t i = elementAt(e.local);
      endWheel();  // GU.13: a press ends a wheel run (its undo step) before anything else
      if (focus_ >= 0 && focus_ != i && static_cast<size_t>(focus_) < elements_.size()) elements_[static_cast<size_t>(focus_)]->focusLost(*target_);
      focus_ = i;
      if (i < 0) return false;
      // (an edit made by the element may rebuild the host's scene; this face and its elements stay)
      const bool used = elements_[static_cast<size_t>(i)]->onPointer(local(i), *target_);
      if (used && static_cast<size_t>(i) < elements_.size() && elements_[static_cast<size_t>(i)]->dragging()) {
        capture_ = i;
        if (elements_[static_cast<size_t>(i)]->cableHandle()) {  // GU.5: a mod source's cable comes out
          cable_ = i;
          cableTo_ = e.local;
          cableTarget_ = -1;
        }
      }
      return used;
    }
    case K::Move: {
      const int32_t i = elementAt(e.local);
      return i >= 0 && elements_[static_cast<size_t>(i)]->onPointer(local(i), *target_);
    }
    case K::Wheel: {  // GU.7: an element that uses the wheel (the harmonic editor's scroll / zoom, a scope's zoom)
      const int32_t i = elementAt(e.local);
      return i >= 0 && elements_[static_cast<size_t>(i)]->onPointer(local(i), *target_);
    }
    case K::Up:
    case K::Leave: return false;
  }
  return false;
}

void GroupPanelFace::endWheel() {
  for (const std::unique_ptr<Element>& e : elements_) e->endWheel(*target_);
}

void GroupPanelFace::visitControls(const Element::ControlVisitor& fn) const {
  std::vector<int8_t>& memo = freshMemo();
  for (size_t i = 0; i < elements_.size(); ++i) {
    if (!shownNow(i, memo)) continue;
    const Rect er = elements_[i]->rect();
    elements_[i]->visitControls([&](const doc::Binding& b, const Rect& r) {
      fn(b, {er.x0 + r.x0, er.y0 + r.y0, er.x0 + r.x1, er.y0 + r.y1});
    });
  }
}

const doc::Binding* GroupPanelFace::controlAt(Vec2 local) const {
  const int32_t i = elementAt(local);
  if (i < 0) return nullptr;
  const Element& e = *elements_[static_cast<size_t>(i)];
  const doc::Binding* b = e.controlAt({local.x - e.rect().x0, local.y - e.rect().y0});
  return b != nullptr && b->kind == doc::BindingKind::Control ? b : nullptr;
}

bool GroupPanelFace::dragging() const noexcept {
  return capture_ >= 0 && static_cast<size_t>(capture_) < elements_.size() && elements_[static_cast<size_t>(capture_)]->dragging();
}

void GroupPanelFace::cancelDrag() {
  if (capture_ >= 0 && static_cast<size_t>(capture_) < elements_.size()) elements_[static_cast<size_t>(capture_)]->cancelDrag(*target_);
  capture_ = -1;
  cable_ = cableTarget_ = -1;
}

bool GroupPanelFace::onKey(const ui::KeyEvent& e) {
  return focus_ >= 0 && static_cast<size_t>(focus_) < elements_.size() && elements_[static_cast<size_t>(focus_)]->onKey(e, *target_);
}
bool GroupPanelFace::onText(const ui::TextEvent& e) {
  return focus_ >= 0 && static_cast<size_t>(focus_) < elements_.size() && elements_[static_cast<size_t>(focus_)]->onText(e, *target_);
}
bool GroupPanelFace::wantsText() const noexcept {
  return focus_ >= 0 && static_cast<size_t>(focus_) < elements_.size() && elements_[static_cast<size_t>(focus_)]->wantsText();
}
void GroupPanelFace::focusLost() {
  if (focus_ >= 0 && static_cast<size_t>(focus_) < elements_.size()) elements_[static_cast<size_t>(focus_)]->focusLost(*target_);
  focus_ = -1;
}

ui::Cursor GroupPanelFace::cursor(int32_t part) const noexcept {
  if (capture_ >= 0) part = capture_;
  if (part >= 0 && static_cast<size_t>(part) < elements_.size()) return elements_[static_cast<size_t>(part)]->cursor();
  return ui::Cursor::Inherit;
}

void GroupPanelFace::describe(int32_t part, std::string& title, std::string& body) const {
  if (part >= 0 && static_cast<size_t>(part) < elements_.size()) {
    elements_[static_cast<size_t>(part)]->describe(*target_, title, body);
    return;
  }
  title = "Group panel";
  body = "The group's panel: its elements edit the controls and settings inside the group. Drag the panel's frame to move "
         "the group; its menu's Open Group opens the group's circuit.";
}

bool GroupPanelFace::setHoveredPart(int32_t part) noexcept {
  if (part == hovered_) return false;
  hovered_ = part;
  return true;
}

// ---- DT.3: horizontal stretch ---------------------------------------------------------------------------------------

std::optional<Face::StretchRange> GroupPanelFace::stretchX() const {
  if (!panel_ || elements_.size() != panel_->elements.size()) return std::nullopt;
  float slack = -1.0f;  // how far the panel can narrow: the least any stretching element can give
  for (const std::unique_ptr<Element>& e : elements_)
    if (const float mw = e->stretchMinWidth(); mw > 0.0f) slack = slack < 0.0f ? std::max(0.0f, e->desc().w - mw) : std::min(slack, std::max(0.0f, e->desc().w - mw));
  if (slack < 0.0f) return std::nullopt;
  const float w = panel_->width;
  return StretchRange{std::max(1.0f, w - slack), std::max(w, kMaxStretchWidth)};
}

bool GroupPanelFace::stretchTo(float width) {
  (void)sync();
  const std::optional<StretchRange> r = stretchX();
  if (!r || !owner_.valid() || !std::isfinite(width)) return false;
  const float to = std::round(std::clamp(width, r->min, r->max));
  const float d = to - panel_->width;
  if (d == 0.0f) return false;
  // The rule: every stretching element widens by the change, and so does one spanning the whole panel (a plate behind
  // the rest); an element whose left edge is at or right of a stretching element's right edge moves right by it (a
  // button row beside a slider keeps its place at the slider's end); the panel holds them all.
  auto p = std::make_shared<doc::Panel>(*panel_);
  std::vector<float> ends;
  for (size_t i = 0; i < elements_.size(); ++i)
    if (elements_[i]->stretchMinWidth() > 0.0f) ends.push_back(p->elements[i].x + p->elements[i].w);
  for (size_t i = 0; i < elements_.size(); ++i) {
    doc::PanelElement& e = p->elements[i];
    if (elements_[i]->stretchMinWidth() > 0.0f) {
      e.w = std::max(elements_[i]->stretchMinWidth(), e.w + d);
      continue;
    }
    if (e.x <= 0.5f && e.x + e.w >= panel_->width - 0.5f) {
      e.w = std::max(1.0f, e.w + d);
      continue;
    }
    for (const float end : ends)
      if (e.x >= end - 0.5f) {
        e.x = std::max(0.0f, e.x + d);
        break;
      }
  }
  p->width = to;
  for (const doc::PanelElement& e : p->elements) p->width = std::max(p->width, e.x + e.w);
  const doc::DeviceId g = owner_;
  doc::PanelPtr next = std::move(p);
  return target_->run([g, next](const doc::Project& pr, doc::IdSource&) { return doc::ops::setGroupPanel(pr, g, next); });
}

// ---- sources --------------------------------------------------------------------------------------------------------

GroupPanelFace::Source docPanelSource(ViewContext& ctx, doc::DeviceId group) {
  // The group's device, found once per snapshot (a paint reads the source; findDevice walks the project).
  struct Memo {
    doc::Snapshot seen;
    doc::PanelPtr panel;
  };
  auto memo = std::make_shared<Memo>();
  return [&ctx, group, memo]() -> doc::PanelPtr {
    const doc::Snapshot& s = ctx.doc();
    if (s != memo->seen) {
      memo->seen = s;
      const doc::Device* d = s ? doc::findDevice(*s, group).device : nullptr;
      memo->panel = d != nullptr && d->hasPanel() ? d->panel : nullptr;
    }
    return memo->panel;
  };
}

doc::Panel valueKnobPanel(doc::DeviceId block, float w, float h) {
  doc::Panel p;
  p.width = w;
  p.height = h;
  doc::PanelElement k;
  k.id = 1;
  k.kind = "knob";
  k.x = 0.0f, k.y = 0.0f, k.w = w, k.h = h;
  k.bindings = {doc::controlBinding(block, std::string(doc::kValueParam))};
  p.elements.push_back(std::move(k));
  return p;
}

doc::Panel inspectorPanel(doc::DeviceId group, float width, bool polyphony) {
  doc::Panel p;
  constexpr float kRow = 24.0f, kPad = 8.0f;
  const float w = std::max(80.0f, width);
  float y = kPad;
  uint32_t id = 1;
  auto add = [&](std::string kind, float x, float ew, float h, std::vector<doc::Binding> b) -> doc::PanelElement& {
    doc::PanelElement e;
    e.id = id++;
    e.kind = std::move(kind);
    e.x = x, e.y = y, e.w = ew, e.h = h;
    e.bindings = std::move(b);
    p.elements.push_back(std::move(e));
    return p.elements.back();
  };
  auto title = [&](const char* text) {
    doc::PanelElement& t = add("label", kPad, w - 2.0f * kPad, 12.0f, {});
    t.setField(doc::styleText("text", text));
    t.setField(doc::styleText("align", "left"));
    t.setField(doc::styleNumber("size", 7.5));
    y += 16.0f;
  };
  if (polyphony) {
    title("POLYPHONY");
    for (const doc::SettingSpec& s : doc::groupSettings()) {
      doc::PanelElement& l = add("label", kPad, w * 0.42f - kPad, 18.0f, {});
      l.setField(doc::styleText("text", std::string(s.label)));
      l.setField(doc::styleText("align", "left"));
      l.setField(doc::styleNumber("size", 8.0));
      const std::string kind = s.isSwitch() ? "toggle" : !s.choices.empty() ? "menu" : "number";
      doc::PanelElement& c = add(kind, w * 0.42f, w * 0.58f - kPad, 18.0f, {doc::settingBinding(group, std::string(s.name))});
      c.setField(doc::styleText("label", "-"));
      y += kRow;
    }
    y += 4.0f;
  }
  // TX.group-oversample: every group's oversampling, the menu's items with their latency (compensated by PDC).
  title("OVERSAMPLE");
  doc::PanelElement& os = add("menu", kPad, w - 2.0f * kPad, 18.0f,
                              {doc::settingBinding(group, std::string(doc::groupOversampleSetting().name))});
  os.setField(doc::styleText("label", "-"));
  y += kRow;
  p.width = w;
  p.height = y + kPad - (kRow - 18.0f);
  return p;
}

// ---- PanelHost ------------------------------------------------------------------------------------------------------

PanelHost::PanelHost(std::string name, std::unique_ptr<Face> face) : Widget(std::move(name)), face_(std::move(face)) {
  setFocusable(true);
  face_->attach(this);
}

PanelHost::~PanelHost() {
  endWheel();
  face_->attach(nullptr);
}

void PanelHost::endWheel() {
  if (wheelTimer_ == 0) return;
  cancelTimer(wheelTimer_);
  wheelTimer_ = 0;
  face_->endWheel();
  markDirty();
}

void PanelHost::wake() {
  markDirty();
  (void)face_->frameTick();  // DT.1 (the dirty mark above repaints it anyway)
  if (voiceTimer_ == 0 && (face_->voices() != nullptr || face_->ticking()) && tree() != nullptr) voiceTimer_ = requestTimer(1.0 / 60.0);
}

void PanelHost::transportChanged() {
  if (!visible()) return;
  if (face_->frameTick()) markDirty();
  if (voiceTimer_ == 0 && face_->ticking() && tree() != nullptr) voiceTimer_ = requestTimer(1.0 / 60.0);
}

void PanelHost::onTimer(uint32_t id) {
  if (id == wheelTimer_) {
    wheelTimer_ = 0;
    face_->endWheel();
    markDirty();
    return;
  }
  if (id != voiceTimer_) return;
  voiceTimer_ = 0;
  VoiceTapReader* v = face_->voices();
  if (!visible()) return;
  if (face_->frameTick()) markDirty();  // DT.1: a tempo slider while the transport plays
  if (v != nullptr && face_->pollVoices()) markDirty();
  if ((v != nullptr && (v->live() || face_->animating())) || face_->ticking()) voiceTimer_ = requestTimer(1.0 / 60.0);
}

ui::FaceTransform PanelHost::transform() const noexcept {
  const ui::IRect r = rect();
  const Vec2 ws = face_->worldSize();
  const float s = std::max(1e-3f, std::min(static_cast<float>(r.w) / ws.x, static_cast<float>(r.h) / ws.y));
  return {{std::round(static_cast<float>(r.x) + (static_cast<float>(r.w) - ws.x * s) * 0.5f), std::round(static_cast<float>(r.y))}, s};
}

GroupPanelFace* PanelHost::panelFace() const noexcept { return dynamic_cast<GroupPanelFace*>(face_.get()); }

namespace {
bool shownInTree(const ui::Widget& w) noexcept {
  if (w.tree() == nullptr) return false;
  for (const ui::Widget* p = &w; p != nullptr; p = p->parent())
    if (!p->visible()) return false;
  return true;
}
}  // namespace

void PanelHost::visitControls(const ControlFn& fn) const {
  const GroupPanelFace* g = panelFace();
  const ui::IRect hr = rect();
  if (g == nullptr || hr.empty() || !shownInTree(*this)) return;
  const ui::FaceTransform t = transform();
  g->visitControls([&](const doc::Binding& b, const Rect& r) {
    const Vec2 a = t.map({r.x0, r.y0}), z = t.map({r.x1, r.y1});
    const ui::IRect cr{static_cast<int32_t>(std::floor(a.x)), static_cast<int32_t>(std::floor(a.y)),
                       static_cast<int32_t>(std::ceil(z.x) - std::floor(a.x)), static_cast<int32_t>(std::ceil(z.y) - std::floor(a.y))};
    const ui::IRect clipped = ui::intersect(cr, hr);
    if (!clipped.empty()) fn(b, clipped);
  });
}

const doc::Binding* PanelHost::controlAt(Vec2 p) const {
  const GroupPanelFace* g = panelFace();
  if (g == nullptr || !rect().contains(p)) return nullptr;
  return g->controlAt(transform().unmap(p));
}

std::optional<ui::IRect> PanelHost::controlRect(const doc::Binding& b) const {
  std::optional<ui::IRect> out;
  visitControls([&](const doc::Binding& x, const ui::IRect& r) {
    if (!out && x == b) out = r;
  });
  return out;
}

std::optional<Vec2> PanelHost::controlPoint(const doc::Binding& b) const {
  const GroupPanelFace* g = panelFace();
  if (g == nullptr || rect().empty() || !shownInTree(*this)) return std::nullopt;
  const std::optional<ControlAnchor> a = g->bindingAnchor(b, true);
  if (!a) return std::nullopt;
  return transform().map({a->at.x, a->at.y - 0.5f * a->radius});
}

void PanelHost::notifyChanged() {
  if (auto* g = dynamic_cast<GroupPanelFace*>(face_.get()); g != nullptr && tree() != nullptr) {
    // GU.13: only the elements whose shown values changed are repainted (the rest of the panel is not damaged).
    std::vector<Rect> rs;
    if (!g->sync() && g->changedRects(rs) && !g->popupRect()) {
      const ui::FaceTransform t = transform();
      for (const Rect& r : rs) {
        const Vec2 a = t.map({r.x0, r.y0}), z = t.map({r.x1, r.y1});
        const ui::IRect d = ui::intersect(ui::IRect{static_cast<int32_t>(std::floor(a.x)), static_cast<int32_t>(std::floor(a.y)),
                                                    static_cast<int32_t>(std::ceil(z.x) - std::floor(a.x)),
                                                    static_cast<int32_t>(std::ceil(z.y) - std::floor(a.y))},
                                          rect());
        if (!d.empty()) tree()->damage().add(d.toRect());
      }
      return;
    }
  }
  markDirty();
}

void PanelHost::paint(ui::PaintContext& ctx) {
  if (ctx.res.theme == nullptr) return;
  const ui::FaceTransform tr = transform();
  FaceEnv env;
  env.theme = ctx.res.theme;
  env.uiScale = ctx.scale.factor;
  env.hovered = hover_ != Face::kNone;
  ui::FacePainter p(ctx.dl, tr);
  p.setText(ctx.res.text);
  p.setImages(ctx.res.images);
  face_->paintLive(p, env);
  face_->paintPopup(p, env);  // GU.13: an element's floating popup (the dial's range and curve), over the panel
  if (face_->needsPrepare(tr.scale)) requestFrame();
  // GU.13: voices (an engine face's dots, a phase orbit): polled while live (as the old device panel's faces were).
  if (voiceTimer_ == 0)
    if (VoiceTapReader* v = face_->voices(); (v != nullptr && (v->live() || face_->animating())) || face_->ticking())
      voiceTimer_ = requestTimer(1.0 / 60.0);
}

void PanelHost::onFrame(double /*now*/) {
  const float s = transform().scale;
  if (face_->needsPrepare(s) && face_->prepare(s)) markDirty();
}

void PanelHost::faceNeedsUpdate(Face& /*f*/) noexcept {
  markDirty();  // (an analysis feed got sound: the display draws it)
  requestFrame();
}

FacePointer PanelHost::pointerOf(const ui::PointerEvent& e) const {
  const ui::FaceTransform tr = transform();
  FacePointer f;
  f.local = tr.unmap(e.pos);
  f.device = e.pos;
  f.pxPerUnit = tr.scale;
  f.uiScale = tree() != nullptr ? tree()->scale() : 1.0f;
  f.mods = e.mods;
  f.clicks = std::max<uint8_t>(1, e.clickCount);
  f.wheel = e.wheel;
  f.button = e.button;
  f.valueGestures = true;  // GU.13
  switch (e.type) {
    case ui::PointerType::Down: f.kind = FacePointer::Kind::Down; break;
    case ui::PointerType::Up: f.kind = FacePointer::Kind::Up; break;
    case ui::PointerType::Wheel: f.kind = FacePointer::Kind::Wheel; break;
    case ui::PointerType::Leave: f.kind = FacePointer::Kind::Leave; break;
    case ui::PointerType::Move: f.kind = FacePointer::Kind::Move; break;
  }
  return f;
}

bool PanelHost::onPointer(const ui::PointerEvent& e) {
  const FacePointer f = pointerOf(e);
  if (e.type == ui::PointerType::Move && !captured_) {
    const int32_t part = face_->hitTest(f.local, f.pxPerUnit);
    const bool hov = face_->setHoveredPart(part) || part != hover_;
    const bool moved = part != hover_;
    hover_ = part;
    if (moved && onHoverPart) onHoverPart();
    if (face_->onPointer(f) || hov) markDirty();
    return part != Face::kNone;
  }
  if (e.type == ui::PointerType::Leave) {
    endWheel();  // (ui::ValueControl: a run ends when the pointer leaves)
    if (face_->setHoveredPart(Face::kNone)) markDirty();
    const bool moved = hover_ != Face::kNone;
    hover_ = Face::kNone;
    if (moved && onHoverPart) onHoverPart();
    return false;
  }
  if (e.type == ui::PointerType::Down) {
    endWheel();
    if (tree() != nullptr) tree()->setFocus(this);
  }
  const bool used = face_->onPointer(f);
  if (e.type == ui::PointerType::Wheel && used) {  // GU.13: the run's end, re-armed by every notch
    if (wheelTimer_ != 0) cancelTimer(wheelTimer_);
    wheelTimer_ = requestTimer(kWheelRunSeconds);
  }
  if (e.type == ui::PointerType::Down) captured_ = used && face_->dragging();
  if (e.type == ui::PointerType::Up) captured_ = false;
  if (used) markDirty();
  setAcceptsTextInput(face_->wantsText());
  return used;
}

bool PanelHost::onKey(const ui::KeyEvent& e) {
  const bool used = face_->onKey(e);
  if (used) markDirty();
  setAcceptsTextInput(face_->wantsText());
  return used;
}

bool PanelHost::onText(const ui::TextEvent& e) {
  const bool used = face_->onText(e);
  if (used) markDirty();
  return used;
}

void PanelHost::onCaptureLost() {
  if (captured_) face_->cancelDrag();
  captured_ = false;
  markDirty();
}

void PanelHost::onFocusChanged(bool focused) {
  if (!focused) {
    face_->focusLost();
    setAcceptsTextInput(false);
    markDirty();
  }
}

void PanelHost::onHoverChanged(bool hovered) {
  if (hovered || hover_ == Face::kNone || captured_) return;
  (void)face_->setHoveredPart(Face::kNone);  // the pointer went to another widget: nothing of the panel is hovered
  hover_ = Face::kNone;
  markDirty();
  if (onHoverPart) onHoverPart();
}

ui::Cursor PanelHost::cursorAt(Vec2 pos) const {
  const ui::FaceTransform tr = transform();
  return face_->cursor(captured_ ? face_->hoveredPart() : face_->hitTest(tr.unmap(pos), tr.scale));
}

ui::InfoText PanelHost::info() const {
  face_->describe(hover_, title_, body_);
  return {title_, body_};
}

}  // namespace pq::views::panel
