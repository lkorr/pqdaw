// A device's panel as a block's face (TX.panel-faces). See pq/views/devices/panel_face.h.

#include "pq/views/devices/panel_face.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "pq/ui/atlas.h"
#include "pq/ui/text.h"
#include "pq/ui/tile_atlas.h"

namespace pq::views::devices {

PanelFace::PanelFace(DeviceSignature sig, PanelDesc desc, std::unique_ptr<DeviceTarget> target,
                     const std::map<std::string, double, std::less<>>& values, ui::Vec2 worldSize)
    : target_(std::move(target)), size_(worldSize) {
  static uint64_t panels = 0;  // UI thread
  serial_ = ++panels;
  sig.exports.clear();  // the host draws the exports as pins on the block's right edge
  auto p = std::make_unique<DevicePanel>("panel", std::move(sig), std::move(desc), target_.get(), values);
  panel_ = p.get();
  panel_->setFoldable(false);  // the block keeps its size
  tree_.setRoot(std::move(p));
  tree_.setHost(this);
}

PanelFace::~PanelFace() { tree_.setHost(nullptr); }

ui::Vec2 PanelFace::sizeFor(const PanelDesc& desc) noexcept {
  return DevicePanel::logicalSizeOf(DeviceSignature{}, desc);  // no exports: no jack strip
}

void PanelFace::setWorldSize(ui::Vec2 s) {
  if (s.x == size_.x && s.y == size_.y) return;
  size_ = s;
  if (layScale_ > 0.0f) {
    ui::UiScale us;
    us.factor = layScale_;
    quiet_ = true;
    tree_.setViewport(static_cast<uint32_t>(std::ceil(size_.x * layScale_)), static_cast<uint32_t>(std::ceil(size_.y * layScale_)), us);
    quiet_ = false;
  }
  recValid_ = false;
}

float PanelFace::clampScale(float s) noexcept { return std::clamp(s, kMinScale, kMaxScale); }

const ui::PaintResources* PanelFace::hostResources() const noexcept {
  const ui::Widget* w = hostWidget();
  return w != nullptr && w->tree() != nullptr ? w->tree()->resources() : nullptr;
}

double PanelFace::hostNow() const noexcept {
  const ui::Widget* w = hostWidget();
  return w != nullptr && w->tree() != nullptr ? w->tree()->now() : 0.0;
}

bool PanelFace::layoutAt(float s) {
  s = clampScale(s);
  const ui::PaintResources* hr = hostResources();
  const bool sameRes = hr == nullptr || (from_.text == hr->text && from_.theme == hr->theme && from_.tiles == hr->tiles);
  if (s == layScale_ && sameRes) return false;
  if (hr != nullptr) from_ = *hr;
  res_ = hr != nullptr ? *hr : ui::PaintResources{};
  res_.uiFonts = nullptr;
  ui::UiScale us;
  us.factor = s;  // not UiScale's window range: a canvas zooms past it (fonts stay within the text system's 4 … 256 px)
  if (res_.text != nullptr && res_.theme != nullptr) {
    // The theme's roles at this scale. Faces 0 and 1 are the UI's regular and semibold faces (pq/ui/scene_textures.h
    // loads them first, in that order; FacePainter's face 0 is the same regular face).
    if (auto f = ui::resolveThemeFonts(*res_.text, 0, 1, *res_.theme, us)) {
      fonts_ = *f;
      res_.uiFonts = &fonts_;
    } else {
      res_.text = nullptr;  // no glyphs rather than a font that does not exist
    }
  }
  tree_.setResources(&res_);
  if (ui::Widget* w = hostWidget(); w != nullptr && w->tree() != nullptr) tree_.setClipboard(&w->tree()->clipboard());
  quiet_ = true;
  tree_.setViewport(static_cast<uint32_t>(std::ceil(size_.x * s)), static_cast<uint32_t>(std::ceil(size_.y * s)), us);
  tree_.layoutNow();
  quiet_ = false;
  layScale_ = s;
  recValid_ = false;
  return true;
}

namespace {

// The far-out colour class of a triangle's colour (PanelFace::kLayers): 3 accents (saturated and not dark: value arcs,
// curves, lit buttons), 2 light ink (text, strokes), 1 dark fills (fields, displays, title bars: darker than the panel),
// 0 the rest (the panel's own fills and borders).
uint32_t colourClass(ui::Rgba c, int32_t baseValue) noexcept {
  const int32_t r = static_cast<int32_t>(c & 0xFF), g = static_cast<int32_t>((c >> 8) & 0xFF), b = static_cast<int32_t>((c >> 16) & 0xFF);
  const int32_t mx = std::max({r, g, b}), mn = std::min({r, g, b});
  if (mx > 90 && (mx - mn) * 100 > 35 * mx) return 3;
  if (mx > 150) return 2;
  if (mx + 6 < baseValue) return 1;
  return 0;
}

}  // namespace

void PanelFace::record() {
  rec_.reset(tree_.width(), tree_.height());
  quiet_ = true;
  tree_.paint(rec_, res_, hostNow());
  quiet_ = false;
  recValid_ = true;
  ++recordings_;
  recTiles_ = false;
  for (const ui::DrawCmd& c : rec_.commands()) recTiles_ = recTiles_ || c.texture >= ui::kFirstTileTexture;
  recEvictions_ = res_.text != nullptr && res_.text->atlas() ? res_.text->atlas()->evictions() : 0;
  // The triangles' colour classes and each class's mean colour (weighted by alpha) for the far-out layers.
  int32_t base = 40;
  if (res_.theme != nullptr) {
    const ui::Rgba bg = res_.theme->device.background;
    base = std::max({static_cast<int32_t>(bg & 0xFF), static_cast<int32_t>((bg >> 8) & 0xFF), static_cast<int32_t>((bg >> 16) & 0xFF)});
  }
  const auto idx = rec_.indices();
  const auto vtx = rec_.vertices();
  triClass_.resize(idx.size() / 3);
  double sum[kLayers][4] = {};
  for (size_t t = 0; t < triClass_.size(); ++t) {
    const ui::Vertex &v0 = vtx[idx[3 * t]], &v1 = vtx[idx[3 * t + 1]], &v2 = vtx[idx[3 * t + 2]];
    const ui::Rgba c = v0.col;
    const uint32_t k = colourClass(c, base);
    triClass_[t] = static_cast<uint8_t>(k);
    // Weighted by covered area (alpha × the triangle's area): a fill outweighs the thin strokes around it.
    const double area = 0.5 * std::fabs(static_cast<double>((v1.pos.x - v0.pos.x) * (v2.pos.y - v0.pos.y) - (v2.pos.x - v0.pos.x) * (v1.pos.y - v0.pos.y)));
    const double a = static_cast<double>(ui::alphaOf(c)) * area;
    sum[k][0] += a * static_cast<double>(c & 0xFF);
    sum[k][1] += a * static_cast<double>((c >> 8) & 0xFF);
    sum[k][2] += a * static_cast<double>((c >> 16) & 0xFF);
    sum[k][3] += a;
  }
  for (uint32_t k = 0; k < kLayers; ++k)
    classColour_[k] = sum[k][3] > 0.0 ? ui::rgba(static_cast<uint8_t>(sum[k][0] / sum[k][3] + 0.5), static_cast<uint8_t>(sum[k][1] / sum[k][3] + 0.5),
                                                 static_cast<uint8_t>(sum[k][2] / sum[k][3] + 0.5))
                                      : 0;
}

ui::Rgba PanelFace::staticLayerColour(const ui::Theme& /*theme*/, uint32_t layer) const noexcept {
  return layer < kLayers ? classColour_[layer] : 0;
}

uint64_t PanelFace::staticKey(uint32_t /*layer*/) const {
  // This panel's current recording (a new one: new tiles; the canvas prunes the old). TMM.13-fix1 L15: by the panel's
  // serial, not its address (a new panel at a freed one's address showed the old panel's far tiles until pruning).
  return (serial_ * 0x9E3779B97F4A7C15ull) ^ (recordings_ * 0xC2B2AE3D27D4EB4Full) ^ 1ull;
}

void PanelFace::paintStatic(ui::FacePainter& p, uint32_t layer) const {
  if (!recValid_ || layScale_ == 0.0f || layer >= kLayers || classColour_[layer] == 0) return;
  ui::DrawList& dl = p.drawList();
  const float k = p.transform().scale / layScale_;
  const ui::Vec2 o = p.transform().origin;
  const ui::Vec2 white = dl.whiteUv();
  const auto idx = rec_.indices();
  const auto vtx = rec_.vertices();
  uint32_t n = 0;
  for (const uint8_t c : triClass_) n += c == layer ? 1u : 0u;
  if (n == 0) return;
  // Each triangle of the class in white (its alpha kept: coverage), glyph quads as solid boxes (text far out is a bar).
  const ui::DrawList::PrimSpan sp = dl.primReserveSpan(3 * n, 3 * n);
  uint32_t w = 0;
  for (size_t t = 0; t < triClass_.size(); ++t) {
    if (triClass_[t] != layer) continue;
    for (uint32_t j = 0; j < 3; ++j) {
      const ui::Vertex& v = vtx[idx[3 * t + j]];
      sp.v[w] = {{o.x + v.pos.x * k, o.y + v.pos.y * k}, white, p.colour(ui::withAlpha(ui::rgba(255, 255, 255), ui::alphaOf(v.col)))};
      sp.i[w] = sp.base + w;
      ++w;
    }
  }
}

void PanelFace::paintLive(ui::FacePainter& p, const FaceEnv& env) {
  const float sc = p.transform().scale;
  if (layScale_ == 0.0f) (void)layoutAt(sc);  // the first paint lays out at once (later scales: prepare, settled)
  if (layScale_ == 0.0f) return;
  lastOrigin_ = p.transform().origin;
  lastScale_ = sc;
  const ui::Atlas* glyphs = res_.text != nullptr && res_.text->atlas() ? res_.text->atlas().get() : nullptr;
  // Re-record only when the panel changed (or what the recording samples may have moved: an evicted glyph page, tiles).
  if (!recValid_ || tree_.needsPaint() || tree_.framesRequested() || recTiles_ || (glyphs != nullptr && glyphs->evictions() != recEvictions_)) {
    quiet_ = true;
    tree_.layoutNow();
    quiet_ = false;
    record();
  }
  ui::DrawList& dl = p.drawList();
  if (!staticAt(sc)) {  // far out the host drew the static layers (tiles) instead
    dl.appendTransformed(rec_, lastOrigin_, sc / layScale_);
    // The pages the recording samples stay resident this frame.
    for (const ui::DrawCmd& c : rec_.commands()) {
      if (ui::isAtlasPageTexture(c.texture) && res_.text != nullptr && res_.text->atlas()) res_.text->atlas()->touch(c.texture - 1);
      else if (c.texture >= ui::kFirstTileTexture && res_.tiles != nullptr) res_.tiles->touch(c.texture - ui::kFirstTileTexture);
    }
  }
  if (env.theme == nullptr) return;
  const ui::Theme& th = *env.theme;
  // The host's pins: sockets centred on the block's edges (a lead is drawn by the wire up to the edge).
  for (const FaceJack& j : env.jacks) {
    if (!j.visible) continue;
    const ui::Vec2 c{j.input ? 0.0f : size_.x, j.at.y};
    p.disc(c, 3.6f, th.face.jack);
    p.ring(c, 3.0f, 1.1f, th.face.jackRing);
    p.disc(c, 1.3f, j.connected ? th.face.signal : ui::mulAlpha(th.face.jackRing, 0.35f));
  }
  if (env.selected || env.hovered) {
    const float t = std::max(1.0f, std::round((env.selected ? 2.0f : 1.0f) * env.uiScale));
    const ui::Rect r{std::round(lastOrigin_.x) - t, std::round(lastOrigin_.y) - t, std::round(lastOrigin_.x + size_.x * sc) + t,
                     std::round(lastOrigin_.y + size_.y * sc) + t};
    dl.addRectStroke(r, env.selected ? th.circuit.selected : th.circuit.hover, t);
  }
}

int32_t PanelFace::hitTest(ui::Vec2 local, float /*pxPerUnit*/) const noexcept {
  if (local.x < 0.0f || local.y < 0.0f || local.x > size_.x || local.y > size_.y) return kNone;
  if (layScale_ == 0.0f) return kFrame;
  return panel_->interactiveAt(toPanel(local)) ? 0 : kFrame;
}

bool PanelFace::onPointer(const FacePointer& e) {
  if (layScale_ == 0.0f) return false;
  ui::PointerEvent pe;
  pe.mods = e.mods;
  pe.clickCount = e.clicks;
  // A drag reads the pointer's movement in device px (at most 1:1 with the panel's px): a knob laid out at kMinScale and
  // drawn far smaller turns as it does at its own scale; zoomed in past kMaxScale it follows the pointer.
  auto dragPos = [&](ui::Vec2 device) {
    const float f = downK_ >= 1.0f ? 1.0f / downK_ : 1.0f;
    return ui::Vec2{downPanel_.x + (device.x - downDevice_.x) * f, downPanel_.y + (device.y - downDevice_.y) * f};
  };
  switch (e.kind) {
    case FacePointer::Kind::Down: {
      pe.type = ui::PointerType::Down;
      pe.button = e.button;
      pe.pos = toPanel(e.local);
      quiet_ = true;
      tree_.update(hostNow());  // due timers first, the layout current for the hit test
      quiet_ = false;
      downPanel_ = pe.pos;
      downDevice_ = e.device;
      downK_ = e.pxPerUnit / layScale_;
      if (!tree_.dispatchPointer(pe)) {
        // Nothing took it (a right press on a knob): the release goes to the tree at once (its held buttons stay
        // balanced), and the press is the host's (the block's menu).
        ui::PointerEvent up = pe;
        up.type = ui::PointerType::Up;
        (void)tree_.dispatchPointer(up);
        pressed_ = false;
        return false;
      }
      pressed_ = tree_.captured() != nullptr;
      return true;
    }
    case FacePointer::Kind::Move:
      pe.type = ui::PointerType::Move;
      pe.pos = pressed_ ? dragPos(e.device) : toPanel(e.local);
      (void)tree_.dispatchPointer(pe);
      return tree_.needsPaint();
    case FacePointer::Kind::Up:
      pe.type = ui::PointerType::Up;
      pe.button = e.button;
      pe.pos = pressed_ ? dragPos(e.device) : toPanel(e.local);
      (void)tree_.dispatchPointer(pe);
      pressed_ = false;
      return true;
    case FacePointer::Kind::Leave:
      pe.type = ui::PointerType::Leave;
      (void)tree_.dispatchPointer(pe);
      return tree_.needsPaint();
    case FacePointer::Kind::Wheel: return false;  // the canvas's (it pans)
  }
  return false;
}

bool PanelFace::dragging() const noexcept { return pressed_ && tree_.captured() != nullptr; }

bool PanelFace::onKey(const ui::KeyEvent& e) {
  if (tree_.focus() == nullptr || e.key == ui::key::kTab) return false;
  const ui::KeyResult r = tree_.dispatchKey(e);
  return r == ui::KeyResult::Widget || r == ui::KeyResult::TextSwallowed;
}

bool PanelFace::onText(const ui::TextEvent& e) { return tree_.dispatchText(e); }

bool PanelFace::wantsText() const noexcept { return tree_.focus() != nullptr && tree_.focus()->acceptsTextInput(); }

void PanelFace::focusLost() {
  if (tree_.focus() != nullptr) tree_.setFocus(nullptr);
}

void PanelFace::cancelDrag() {
  if (pressed_) tree_.cancelInteraction();
  pressed_ = false;
}

ui::Cursor PanelFace::cursor(int32_t part) const noexcept {
  if (part < 0 && !dragging()) return ui::Cursor::Inherit;
  const ui::Cursor c = tree_.cursor();
  return c == ui::Cursor::Arrow ? ui::Cursor::Inherit : c;
}

void PanelFace::describe(int32_t /*part*/, std::string& title, std::string& body) const {
  const ui::InfoText t = ui::infoFor(tree_.hovered());
  title.assign(t.title);
  body.assign(t.body);
}

std::string PanelFace::paramAt(ui::Vec2 local) const {
  if (layScale_ == 0.0f) return {};
  return panel_->paramAt(toPanel(local));
}

std::optional<ControlAnchor> PanelFace::controlAnchor(std::string_view param) const {
  if (layScale_ == 0.0f) return std::nullopt;
  const std::optional<DevicePanel::Anchor> a = panel_->controlAnchor(param);
  if (!a) return std::nullopt;
  const float k = 1.0f / layScale_;
  return ControlAnchor{{a->at.x * k, a->at.y * k}, a->radius * k, a->clear * k};
}

std::optional<ControlAnchor> PanelFace::controlHome(std::string_view param) const {
  const std::optional<DevicePanel::Anchor> a = panel_->controlHome(param);
  if (!a) return std::nullopt;
  return ControlAnchor{a->at, a->radius, a->clear};
}

bool PanelFace::setHoveredPart(int32_t part) noexcept {
  bool changed = Face::setHoveredPart(part);
  if (part == kNone && !pressed_) {
    ui::PointerEvent pe;
    pe.type = ui::PointerType::Leave;
    (void)tree_.dispatchPointer(pe);
    changed = changed || tree_.needsPaint();
  }
  return changed;
}

void PanelFace::attach(FaceHost* host) { host_ = host; }

bool PanelFace::update(double now) {
  quiet_ = true;
  tree_.update(now);
  quiet_ = false;
  return tree_.needsPaint();
}

double PanelFace::nextWake() const noexcept {
  if (tree_.framesRequested()) return -std::numeric_limits<double>::infinity();
  return tree_.nextWakeTime();
}

bool PanelFace::needsPrepare(float pxPerUnit) const noexcept {
  if (clampScale(pxPerUnit) != layScale_) return true;
  const ui::PaintResources* hr = hostResources();  // a theme reload, another text system: fonts again
  return hr != nullptr && (from_.text != hr->text || from_.theme != hr->theme || from_.tiles != hr->tiles);
}

bool PanelFace::prepare(float pxPerUnit) { return layoutAt(pxPerUnit); }

ui::Widget* PanelFace::hostWidget() const noexcept { return host_ != nullptr ? host_->hostWidget() : nullptr; }

ui::IRect PanelFace::toHost(const ui::IRect& r) const noexcept {
  const float k = layScale_ > 0.0f ? lastScale_ / layScale_ : 1.0f;
  const float x0 = std::round(lastOrigin_.x + static_cast<float>(r.x) * k), y0 = std::round(lastOrigin_.y + static_cast<float>(r.y) * k);
  const float x1 = std::round(lastOrigin_.x + static_cast<float>(r.right()) * k), y1 = std::round(lastOrigin_.y + static_cast<float>(r.bottom()) * k);
  return {static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<int32_t>(x1 - x0), static_cast<int32_t>(y1 - y0)};
}

void PanelFace::treeNeedsUpdate() noexcept {
  if (!quiet_ && host_ != nullptr) host_->faceNeedsUpdate(*this);
}

}  // namespace pq::views::devices
