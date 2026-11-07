// GU.3: a group's panel as a face, its element tile cache, the group inspector's panel and the PanelHost widget
// (pq/views/panel/group_panel_face.h).

#include "pq/views/panel/group_panel_face.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>

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
    h ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(atlas)) * 0xD6E8FEB86659FD93ull;
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

void GroupPanelFace::rebuild(const doc::PanelPtr& p) {
  // Unbound tabs keep their selection (by element id) across the rebuild.
  std::vector<std::pair<uint32_t, double>> keep;
  for (const std::unique_ptr<Element>& e : elements_)
    if (e->binding() == nullptr)
      if (const std::optional<double> s = e->selection(*target_)) keep.emplace_back(e->id(), *s);
  if (capture_ >= 0 && static_cast<size_t>(capture_) < elements_.size()) elements_[static_cast<size_t>(capture_)]->cancelDrag(*target_);
  capture_ = focus_ = -1;
  hovered_ = kNone;
  panel_ = p;
  elements_.clear();
  whenSource_.clear();
  if (!p) return;
  elements_.reserve(p->elements.size());
  for (const doc::PanelElement& d : p->elements) {
    elements_.push_back(makeElement(d));
    for (const auto& [id, v] : keep)
      if (id == d.id) elements_.back()->setLocalSelection(v);
  }
  for (const doc::PanelElement& d : p->elements) whenSource_.push_back(d.when ? indexOf(d.when->element) : -1);
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
  if (on) {
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

void GroupPanelFace::paintLive(ui::FacePainter& p, const FaceEnv& env) {
  (void)sync();
  const ui::Theme& th = *env.theme;
  const Vec2 ws = worldSize();
  stats_ = {};
  // The plate: the meta faces' body and frame.
  p.roundedRect({0.0f, 0.0f, ws.x, ws.y}, kCorner, th.face.body);
  outlineRect(p, {0.6f, 0.6f, ws.x - 0.6f, ws.y - 0.6f}, kCorner - 0.6f, 1.2f, th.face.frame);
  ui::TileAtlas* at = atlas();
  ElementTiles& cache = elementTiles();
  const float sc = p.transform().scale;
  const Vec2 o0 = p.transform().origin;
  tilesWanted_ = 0.0f;
  std::vector<int8_t>& memo = freshMemo();
  ElementEnv ee;
  ee.theme = &th;
  ee.target = target_.get();
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
    ui::FacePainter q = p.at({o, sc});
    ee.hovered = hovered_ == static_cast<int32_t>(i);
    ee.focused = focus_ == static_cast<int32_t>(i);
    e.paintLive(q, ee);
    ++stats_.drawn;
  }
  // The block's selection / hover rim.
  outlineRect(p, {0.6f, 0.6f, ws.x - 0.6f, ws.y - 0.6f}, kCorner - 0.6f, env.selected ? 1.75f : 1.25f,
              env.selected ? th.circuit.selected : (env.hovered && hovered_ == kFrame ? th.circuit.hover : 0u));
}

void GroupPanelFace::liveRects(std::vector<Rect>& out) const {
  const Vec2 ws = worldSize();
  out.push_back({0.0f, 0.0f, ws.x, ws.y});
}

bool GroupPanelFace::needsPrepare(float pxPerUnit) const noexcept { return tilesWanted_ > 0.0f && tilesWanted_ == pxPerUnit && atlas() != nullptr; }

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
  cache.prune();
  return built;
}

int32_t GroupPanelFace::hitTest(Vec2 l, float) const noexcept {
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
    return elements_[static_cast<size_t>(c)]->onPointer(local(c), *target_) || e.kind == K::Up || e.kind == K::Move;
  }
  switch (e.kind) {
    case K::Down: {
      const int32_t i = elementAt(e.local);
      if (focus_ >= 0 && focus_ != i && static_cast<size_t>(focus_) < elements_.size()) elements_[static_cast<size_t>(focus_)]->focusLost(*target_);
      focus_ = i;
      if (i < 0) return false;
      // (an edit made by the element may rebuild the host's scene; this face and its elements stay)
      const bool used = elements_[static_cast<size_t>(i)]->onPointer(local(i), *target_);
      if (used && static_cast<size_t>(i) < elements_.size() && elements_[static_cast<size_t>(i)]->dragging()) capture_ = i;
      return used;
    }
    case K::Move: {
      const int32_t i = elementAt(e.local);
      return i >= 0 && elements_[static_cast<size_t>(i)]->onPointer(local(i), *target_);
    }
    case K::Up:
    case K::Wheel:
    case K::Leave: return false;
  }
  return false;
}

bool GroupPanelFace::dragging() const noexcept {
  return capture_ >= 0 && static_cast<size_t>(capture_) < elements_.size() && elements_[static_cast<size_t>(capture_)]->dragging();
}

void GroupPanelFace::cancelDrag() {
  if (capture_ >= 0 && static_cast<size_t>(capture_) < elements_.size()) elements_[static_cast<size_t>(capture_)]->cancelDrag(*target_);
  capture_ = -1;
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
         "the group; double-click it to open the group's circuit.";
}

bool GroupPanelFace::setHoveredPart(int32_t part) noexcept {
  if (part == hovered_) return false;
  hovered_ = part;
  return true;
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

doc::Panel inspectorPanel(doc::DeviceId group, float width) {
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
  doc::PanelElement& title = add("label", kPad, w - 2.0f * kPad, 12.0f, {});
  title.setField(doc::styleText("text", "POLYPHONY"));
  title.setField(doc::styleText("align", "left"));
  title.setField(doc::styleNumber("size", 7.5));
  y += 16.0f;
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
  p.width = w;
  p.height = y + kPad - (kRow - 18.0f);
  return p;
}

// ---- PanelHost ------------------------------------------------------------------------------------------------------

PanelHost::PanelHost(std::string name, std::unique_ptr<Face> face) : Widget(std::move(name)), face_(std::move(face)) {
  setFocusable(true);
  face_->attach(this);
}

PanelHost::~PanelHost() { face_->attach(nullptr); }

ui::FaceTransform PanelHost::transform() const noexcept {
  const ui::IRect r = rect();
  const Vec2 ws = face_->worldSize();
  const float s = std::max(1e-3f, std::min(static_cast<float>(r.w) / ws.x, static_cast<float>(r.h) / ws.y));
  return {{std::round(static_cast<float>(r.x) + (static_cast<float>(r.w) - ws.x * s) * 0.5f), std::round(static_cast<float>(r.y))}, s};
}

void PanelHost::notifyChanged() {
  if (auto* g = dynamic_cast<GroupPanelFace*>(face_.get())) (void)g->sync();
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
  if (face_->needsPrepare(tr.scale)) requestFrame();
}

void PanelHost::onFrame(double /*now*/) {
  const float s = transform().scale;
  if (face_->needsPrepare(s) && face_->prepare(s)) markDirty();
}

void PanelHost::faceNeedsUpdate(Face& /*f*/) noexcept { requestFrame(); }

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
    hover_ = part;
    if (face_->onPointer(f) || hov) markDirty();
    return part != Face::kNone;
  }
  if (e.type == ui::PointerType::Leave) {
    if (face_->setHoveredPart(Face::kNone)) markDirty();
    hover_ = Face::kNone;
    return false;
  }
  if (e.type == ui::PointerType::Down && tree() != nullptr) tree()->setFocus(this);
  const bool used = face_->onPointer(f);
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

ui::Cursor PanelHost::cursorAt(Vec2 pos) const {
  const ui::FaceTransform tr = transform();
  return face_->cursor(captured_ ? face_->hoveredPart() : face_->hitTest(tr.unmap(pos), tr.scale));
}

ui::InfoText PanelHost::info() const {
  face_->describe(hover_, title_, body_);
  return {title_, body_};
}

}  // namespace pq::views::panel
