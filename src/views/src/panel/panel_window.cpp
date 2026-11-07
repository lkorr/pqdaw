// GU.5: a group's panel window — the window, its editor (edit mode) and its generated inspector, and how a control sent
// to a panel becomes an element (pq/views/panel/panel_window.h).

#include "pq/views/panel/panel_window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

#include "../common/paint.h"
#include "elements_internal.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/graph/patch_file.h"
#include "pq/ui/controls.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/overlay.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/views/devices/device_model.h"
#include "pq/views/panel/style_schemas.h"
#include "pq/views/view_context.h"

namespace pq::views::panel {

using ui::IRect;
using ui::Rect;
using ui::Rgba;
using ui::Vec2;

// ---- sending ------------------------------------------------------------------------------------------------------

ElementChoice defaultElement(const doc::Binding& b, const BoundInfo& info) {
  auto sized = [](std::string kind) {
    ElementChoice c;
    if (const ElementSchema* s = elementSchema(kind)) {
      c.w = s->defaultW;
      c.h = s->defaultH;
    }
    c.kind = std::move(kind);
    return c;
  };
  if (b.kind == doc::BindingKind::Signal) return sized("modsource");
  if (b.kind == doc::BindingKind::Panel) return sized(std::string(doc::kEmbedKind));  // GU.6 (sized by its child's panel)
  if (info.isSwitch) return sized("toggle");
  if (!info.choices.empty()) return sized("menu");
  if (info.setting) return sized("number");
  return sized("knob");
}

namespace {

bool overlapsAny(const doc::Panel* p, float x, float y, float w, float h, float gap) {
  if (p == nullptr) return false;
  for (const doc::PanelElement& e : p->elements)
    if (x < e.x + e.w + gap && e.x < x + w + gap && y < e.y + e.h + gap && e.y < y + h + gap) return true;
  return false;
}

float snapUnit(float v) noexcept { return std::round(v); }

}  // namespace

Vec2 freePlace(const doc::Panel* p, float w, float h) {
  const float pw = p != nullptr && p->width > 0.0f ? p->width : kNewPanelW;
  float bottom = kPlaceGap;
  if (p != nullptr)
    for (const doc::PanelElement& e : p->elements) bottom = std::max(bottom, e.y + e.h + kPlaceGap);
  constexpr float kStep = 8.0f;
  for (float y = kPlaceGap; y <= bottom; y += kStep)
    for (float x = kPlaceGap; x + w + kPlaceGap <= pw + 0.5f; x += kStep)
      if (!overlapsAny(p, x, y, w, h, kPlaceGap - 0.5f)) return {x, y};
  return {kPlaceGap, bottom};
}

doc::Panel panelWithElements(const doc::Panel* base, std::vector<doc::PanelElement> add, std::optional<Vec2> at, std::vector<uint32_t>* ids) {
  doc::Panel p;
  if (base != nullptr) p = *base;
  if (p.width <= 0.0f || p.height <= 0.0f) {
    p.width = std::max(p.width, kNewPanelW);
    p.height = std::max(p.height, kNewPanelH);
  }
  bool first = true;
  for (doc::PanelElement& e : add) {
    e.id = p.nextId();
    if (first && at) {
      e.x = std::max(0.0f, snapUnit(at->x - e.w * 0.5f));
      e.y = std::max(0.0f, snapUnit(at->y - e.h * 0.5f));
    } else {
      const Vec2 f = freePlace(&p, e.w, e.h);
      e.x = f.x;
      e.y = f.y;
    }
    first = false;
    p.width = std::min(doc::kMaxPanelSize, std::max(p.width, e.x + e.w + kPlaceGap));
    p.height = std::min(doc::kMaxPanelSize, std::max(p.height, e.y + e.h + kPlaceGap));
    if (ids != nullptr) ids->push_back(e.id);
    p.elements.push_back(std::move(e));
  }
  return p;
}

// ---- the editor ---------------------------------------------------------------------------------------------------

class PanelEditor final : public ui::Widget {
public:
  explicit PanelEditor(PanelWindow& w) : Widget("editor"), win_(w) { setFocusable(true); }

  const std::vector<float>& guidesX() const noexcept { return gx_; }
  const std::vector<float>& guidesY() const noexcept { return gy_; }
  int32_t hovered() const noexcept { return hover_; }

  void paint(ui::PaintContext& ctx) override {
    if (ctx.res.theme == nullptr) return;
    const ui::Theme& th = *ctx.res.theme;
    ui::DrawList& dl = ctx.dl;
    const ui::FaceTransform tr = win_.panelTransform();
    const float us = ctx.scale.factor;
    GroupPanelFace& f = win_.face();
    (void)f.sync();
    const doc::PanelPtr p = win_.shownPanel();
    if (!p) return;
    auto dev = [&](const Rect& r) { return Rect{tr.map({r.x0, r.y0}).x, tr.map({r.x0, r.y0}).y, tr.map({r.x1, r.y1}).x, tr.map({r.x1, r.y1}).y}; };
    // Every element's frame, faint (an edit mode reads as one), the hovered one stronger.
    for (size_t i = 0; i < f.elementCount(); ++i) {
      if (!f.shown(i)) continue;
      const Rect r = dev(f.element(i).rect());
      const bool hot = static_cast<int32_t>(i) == hover_;
      dl.addRectStroke(r, ui::mulAlpha(th.circuit.selected, hot ? 0.7f : 0.22f), 1.0f);
    }
    // The selection: outlines, and a single selection's handles.
    Rect bb{};
    size_t n = 0;
    for (const uint32_t id : win_.selection_) {
      const int32_t i = f.indexOf(id);
      if (i < 0) continue;
      const Rect r = dev(f.element(static_cast<size_t>(i)).rect());
      dl.addRectStroke({r.x0 - 1.0f, r.y0 - 1.0f, r.x1 + 1.0f, r.y1 + 1.0f}, th.circuit.selected, std::max(1.0f, std::round(1.5f * us)));
      bb = n++ == 0 ? r : Rect{std::min(bb.x0, r.x0), std::min(bb.y0, r.y0), std::max(bb.x1, r.x1), std::max(bb.y1, r.y1)};
    }
    if (n == 1)
      for (const Vec2 h : handles(bb)) {
        const float hs = std::round(3.5f * us);
        dl.addRectFilled({h.x - hs, h.y - hs, h.x + hs, h.y + hs}, th.circuit.selected);
        dl.addRectStroke({h.x - hs, h.y - hs, h.x + hs, h.y + hs}, th.device.background, 1.0f);
      }
    // Snap guides across the panel.
    const Vec2 o = tr.map({0.0f, 0.0f}), e = tr.map({p->width, p->height});
    for (const float x : gx_) dl.addVLine(std::round(tr.map({x, 0.0f}).x), o.y, e.y, th.face.signal, 1.0f);
    for (const float y : gy_) dl.addHLine(o.x, e.x, std::round(tr.map({0.0f, y}).y), th.face.signal, 1.0f);
    // The panel's size handle (its bottom-right corner).
    const float hs = std::round(5.0f * us);
    dl.addTriangleFilled({e.x, e.y - 2.0f * hs}, {e.x, e.y}, {e.x - 2.0f * hs, e.y}, ui::mulAlpha(th.circuit.selected, drag_ == Drag::Panel ? 1.0f : 0.6f));
    if (drag_ == Drag::Frame) {
      const Rect r{std::min(press_.x, last_.x), std::min(press_.y, last_.y), std::max(press_.x, last_.x), std::max(press_.y, last_.y)};
      dl.addRectFilled(r, ui::mulAlpha(th.circuit.selected, 0.1f));
      dl.addRectStroke(r, ui::mulAlpha(th.circuit.selected, 0.8f), 1.0f);
    }
    if (f.elementCount() == 0 && ctx.res.uiFonts != nullptr) {
      const IRect box{static_cast<int32_t>(o.x), static_cast<int32_t>(o.y), static_cast<int32_t>(e.x - o.x), static_cast<int32_t>(e.y - o.y)};
      paint::text(ctx, ctx.res.uiFonts->small, box, "Select a control in the circuit and press P, or drag it here", th.text.secondary,
                  ui::TextAlign::Center);
    }
  }

  bool onPointer(const ui::PointerEvent& e) override {
    using T = ui::PointerType;
    const ui::FaceTransform tr = win_.panelTransform();
    const Vec2 p = tr.unmap(e.pos);
    GroupPanelFace& f = win_.face();
    (void)f.sync();
    if (e.type == T::Move && drag_ == Drag::None) {
      const int32_t h = elementAt(p);
      if (h != hover_) {
        hover_ = h;
        markDirty();
        win_.linksChanged();
      }
      cursor_ = handleAt(e.pos) >= 0 ? ui::Cursor::ResizeNWSE : panelHandle(e.pos) ? ui::Cursor::ResizeNWSE : h >= 0 ? ui::Cursor::Move : ui::Cursor::Arrow;
      return true;
    }
    if (e.type == T::Leave) {
      if (hover_ >= 0) {
        hover_ = -1;
        markDirty();
        win_.linksChanged();
      }
      return false;
    }
    if (e.type == T::Down) {
      if (tree() != nullptr) tree()->setFocus(this);
      if (e.button == ui::PointerButton::Right) {
        const int32_t i = elementAt(p);
        if (i >= 0 && !selected(f.element(static_cast<size_t>(i)).id())) win_.setSelection({f.element(static_cast<size_t>(i)).id()});
        openMenu(e.pos);
        return true;
      }
      if (e.button != ui::PointerButton::Left) return false;
      press_ = last_ = e.pos;
      pressP_ = p;
      moved_ = false;
      const doc::PanelPtr base = win_.shownPanel();
      base_ = base ? base : win_.empty_;
      gx_.clear();
      gy_.clear();
      if (panelHandle(e.pos)) {
        drag_ = Drag::Panel;
        return true;
      }
      if (const int32_t h = handleAt(e.pos); h >= 0) {
        drag_ = Drag::Resize;
        handle_ = h;
        return true;
      }
      const int32_t i = elementAt(p);
      const bool add = (e.mods & (ui::mod::kShift | ui::mod::kCtrl)) != 0;
      if (i >= 0) {
        const uint32_t id = f.element(static_cast<size_t>(i)).id();
        if (add) {
          std::vector<uint32_t> s = win_.selection_;
          if (const auto it = std::find(s.begin(), s.end(), id); it != s.end()) s.erase(it);
          else s.push_back(id);
          win_.setSelection(std::move(s));
          drag_ = Drag::None;
          return true;
        }
        clickOnly_ = selected(id) && win_.selection_.size() > 1 ? id : 0;
        if (!selected(id)) win_.setSelection({id});
        drag_ = Drag::Move;
        return true;
      }
      if (!add) win_.setSelection({});
      frameKeep_ = add ? win_.selection_ : std::vector<uint32_t>{};
      drag_ = Drag::Frame;
      return true;
    }
    if (e.type == T::Move && drag_ != Drag::None) {
      last_ = e.pos;
      const float slop = 3.0f * (tree() != nullptr ? tree()->scale() : 1.0f);
      if (!moved_ && std::max(std::fabs(e.pos.x - press_.x), std::fabs(e.pos.y - press_.y)) < slop) return true;
      moved_ = true;
      const bool snap = (e.mods & ui::mod::kAlt) == 0;
      switch (drag_) {
        case Drag::Move: win_.preview(moved(p, snap)); break;
        case Drag::Resize: win_.preview(resized(p, snap)); break;
        case Drag::Panel: win_.preview(sizedPanel(p, snap)); break;
        case Drag::Frame: frameSelect(); break;
        case Drag::None: break;
      }
      markDirty();
      return true;
    }
    if (e.type == T::Up && drag_ != Drag::None) {
      const Drag d = drag_;
      drag_ = Drag::None;
      gx_.clear();
      gy_.clear();
      markDirty();
      if (!moved_) {
        if (d == Drag::Move && clickOnly_ != 0) win_.setSelection({clickOnly_});
        win_.cancelPreview();
        return true;
      }
      if (d == Drag::Move) (void)win_.commitPreview("Move Panel Elements");
      else if (d == Drag::Resize) (void)win_.commitPreview("Resize Panel Element");
      else if (d == Drag::Panel) (void)win_.commitPreview("Resize Panel");
      return true;
    }
    return false;
  }

  void onCaptureLost() override {
    if (drag_ == Drag::Move || drag_ == Drag::Resize || drag_ == Drag::Panel) win_.cancelPreview();
    drag_ = Drag::None;
    gx_.clear();
    gy_.clear();
    markDirty();
  }

  bool onKey(const ui::KeyEvent& e) override {
    if (!e.down) return false;  // (GU.6: a key's release acted again — every nudge was two)
    const ui::Mods m = e.mods & (ui::mod::kCtrl | ui::mod::kShift | ui::mod::kAlt);
    if (e.key == ui::key::kEscape) {
      if (drag_ != Drag::None) {
        if (drag_ != Drag::Frame) win_.cancelPreview();
        drag_ = Drag::None;
        gx_.clear();
        gy_.clear();
        if (tree() != nullptr) tree()->releaseCapture();
        markDirty();
        return true;
      }
      if (!win_.selection_.empty()) {
        win_.setSelection({});
        return true;
      }
      return false;
    }
    if ((e.key == ui::key::kDelete || e.key == ui::key::kBackspace) && m == ui::mod::kNone) return win_.removeSelected() || !win_.selection_.empty();
    if (e.key == 'a' && m == ui::mod::kCtrl) {
      std::vector<uint32_t> all;
      if (const doc::PanelPtr p = win_.docPanel())
        for (const doc::PanelElement& x : p->elements) all.push_back(x.id);
      win_.setSelection(std::move(all));
      return true;
    }
    if (e.key == 'd' && m == ui::mod::kCtrl) return win_.duplicateSelected() || true;
    if (e.key == 'g' && m == (ui::mod::kCtrl | ui::mod::kShift)) return win_.breakApartSelected() || true;  // GU.6
    const float step = (m & ui::mod::kShift) ? 10.0f : 1.0f;
    if ((m & ~ui::mod::kShift) == ui::mod::kNone && !win_.selection_.empty()) {
      if (e.key == ui::key::kLeft) return win_.nudgeSelected(-step, 0.0f) || true;
      if (e.key == ui::key::kRight) return win_.nudgeSelected(step, 0.0f) || true;
      if (e.key == ui::key::kUp) return win_.nudgeSelected(0.0f, -step) || true;
      if (e.key == ui::key::kDown) return win_.nudgeSelected(0.0f, step) || true;
    }
    return false;
  }

  ui::Cursor cursorAt(Vec2) const override { return drag_ == Drag::Move ? ui::Cursor::Move : cursor_; }

private:
  enum class Drag : uint8_t { None, Move, Resize, Panel, Frame };

  bool selected(uint32_t id) const { return std::find(win_.selection_.begin(), win_.selection_.end(), id) != win_.selection_.end(); }

  // The topmost shown element whose rect holds panel point p (every kind: a label or a shape is selectable).
  int32_t elementAt(Vec2 p) const {
    GroupPanelFace& f = win_.face();
    for (size_t i = f.elementCount(); i-- > 0;) {
      if (!f.shown(i)) continue;
      const Rect r = f.element(i).rect();
      if (p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1) return static_cast<int32_t>(i);
    }
    return -1;
  }
  // A single selection's eight handles (device px): corners then edges, clockwise from the top-left.
  static std::array<Vec2, 8> handles(const Rect& r) {
    const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f;
    return {Vec2{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}, {cx, r.y0}, {r.x1, cy}, {cx, r.y1}, {r.x0, cy}};
  }
  const doc::PanelElement* single() const {
    if (win_.selection_.size() != 1) return nullptr;
    const doc::PanelPtr p = win_.shownPanel();
    return p ? p->find(win_.selection_.front()) : nullptr;
  }
  int32_t handleAt(Vec2 pos) const {
    const doc::PanelElement* e = single();
    if (e == nullptr) return -1;
    const ui::FaceTransform tr = win_.panelTransform();
    const Vec2 a = tr.map({e->x, e->y}), b = tr.map({e->x + e->w, e->y + e->h});
    const float reach = 5.0f * (tree() != nullptr ? tree()->scale() : 1.0f);
    const std::array<Vec2, 8> hs = handles({a.x, a.y, b.x, b.y});
    for (size_t k = 0; k < hs.size(); ++k)
      if (std::fabs(pos.x - hs[k].x) <= reach && std::fabs(pos.y - hs[k].y) <= reach) return static_cast<int32_t>(k);
    return -1;
  }
  bool panelHandle(Vec2 pos) const {
    const doc::PanelPtr p = win_.shownPanel();
    const doc::Panel* q = p ? p.get() : win_.empty_.get();
    const Vec2 c = win_.panelTransform().map({q->width, q->height});
    const float reach = 10.0f * (tree() != nullptr ? tree()->scale() : 1.0f);
    return pos.x <= c.x + 2.0f && pos.y <= c.y + 2.0f && pos.x >= c.x - reach && pos.y >= c.y - reach && handleAt(pos) < 0;
  }
  // The snap threshold in panel units: 4 px on screen at the window's zoom (4 units at zoom 1).
  float threshold() const {
    const float us = tree() != nullptr ? tree()->scale() : 1.0f;
    return 4.0f * us / std::max(1e-3f, win_.panelTransform().scale);
  }
  // The edges and centres a moving edge may snap to: the other (unselected) elements' and the panel's.
  void targets(std::vector<float>& xs, std::vector<float>& ys) const {
    xs.clear();
    ys.clear();
    const doc::Panel& p = *base_;
    xs.insert(xs.end(), {0.0f, p.width * 0.5f, p.width});
    ys.insert(ys.end(), {0.0f, p.height * 0.5f, p.height});
    for (const doc::PanelElement& e : p.elements) {
      if (selected(e.id)) continue;
      xs.insert(xs.end(), {e.x, e.x + e.w * 0.5f, e.x + e.w});
      ys.insert(ys.end(), {e.y, e.y + e.h * 0.5f, e.y + e.h});
    }
  }
  // The smallest correction that lands one of `mine` on a target within the threshold (0: none), its guide.
  static float snapOf(std::span<const float> mine, const std::vector<float>& to, float thr, std::vector<float>& guides) {
    float best = thr + 1.0f, corr = 0.0f;
    for (const float m : mine)
      for (const float t : to)
        if (std::fabs(t - m) < best) {
          best = std::fabs(t - m);
          corr = t - m;
        }
    if (best > thr) return 0.0f;
    for (const float m : mine)
      for (const float t : to)
        if (std::fabs(t - (m + corr)) < 1e-3f && std::find(guides.begin(), guides.end(), t) == guides.end()) guides.push_back(t);
    return corr;
  }
  doc::PanelPtr moved(Vec2 p, bool snap) {
    auto out = std::make_shared<doc::Panel>(*base_);
    float dx = p.x - pressP_.x, dy = p.y - pressP_.y;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const doc::PanelElement& e : base_->elements)
      if (selected(e.id)) x0 = std::min(x0, e.x), y0 = std::min(y0, e.y), x1 = std::max(x1, e.x + e.w), y1 = std::max(y1, e.y + e.h);
    gx_.clear();
    gy_.clear();
    if (x0 > x1) return out;
    // Whole units first, then the snap (Alt: neither).
    if (snap) {
      dx = std::round(x0 + dx) - x0;
      dy = std::round(y0 + dy) - y0;
      std::vector<float> xs, ys;
      targets(xs, ys);
      const float thr = threshold();
      const float mx[3] = {x0 + dx, (x0 + x1) * 0.5f + dx, x1 + dx};
      const float my[3] = {y0 + dy, (y0 + y1) * 0.5f + dy, y1 + dy};
      dx += snapOf(mx, xs, thr, gx_);
      dy += snapOf(my, ys, thr, gy_);
    }
    for (doc::PanelElement& e : out->elements)
      if (selected(e.id)) {
        e.x = std::clamp(e.x + dx, 0.0f, std::max(0.0f, out->width - 1.0f));
        e.y = std::clamp(e.y + dy, 0.0f, std::max(0.0f, out->height - 1.0f));
      }
    return out;
  }
  doc::PanelPtr resized(Vec2 p, bool snap) {
    auto out = std::make_shared<doc::Panel>(*base_);
    gx_.clear();
    gy_.clear();
    const doc::PanelElement* e0 = base_->find(win_.selection_.empty() ? 0 : win_.selection_.front());
    const int32_t at = e0 != nullptr ? out->indexOf(e0->id) : -1;
    if (at < 0) return out;
    doc::PanelElement& e = out->elements[static_cast<size_t>(at)];
    float x0 = e0->x, y0 = e0->y, x1 = e0->x + e0->w, y1 = e0->y + e0->h;
    const float dx = p.x - pressP_.x, dy = p.y - pressP_.y;
    // Which edges this handle moves: corners 0 … 3 (tl, tr, br, bl), edges 4 … 7 (t, r, b, l).
    const bool left = handle_ == 0 || handle_ == 3 || handle_ == 7, right = handle_ == 1 || handle_ == 2 || handle_ == 5;
    const bool top = handle_ == 0 || handle_ == 1 || handle_ == 4, bottom = handle_ == 2 || handle_ == 3 || handle_ == 6;
    std::vector<float> xs, ys;
    if (snap) targets(xs, ys);
    const float thr = threshold();
    auto edge = [&](float v, const std::vector<float>& to, std::vector<float>& g) {
      if (!snap) return v;
      v = std::round(v);
      const float m[1] = {v};
      return v + snapOf(m, to, thr, g);
    };
    constexpr float kMin = 8.0f;
    if (left) x0 = std::min(edge(x0 + dx, xs, gx_), x1 - kMin);
    if (right) x1 = std::max(edge(x1 + dx, xs, gx_), x0 + kMin);
    if (top) y0 = std::min(edge(y0 + dy, ys, gy_), y1 - kMin);
    if (bottom) y1 = std::max(edge(y1 + dy, ys, gy_), y0 + kMin);
    e.x = std::max(0.0f, x0);
    e.y = std::max(0.0f, y0);
    e.w = std::max(kMin, x1 - e.x);
    e.h = std::max(kMin, y1 - e.y);
    out->width = std::min(doc::kMaxPanelSize, std::max(out->width, e.x + e.w));
    out->height = std::min(doc::kMaxPanelSize, std::max(out->height, e.y + e.h));
    return out;
  }
  doc::PanelPtr sizedPanel(Vec2 p, bool snap) {
    auto out = std::make_shared<doc::Panel>(*base_);
    float w = base_->width + (p.x - pressP_.x), h = base_->height + (p.y - pressP_.y);
    if (snap) w = std::round(w / 4.0f) * 4.0f, h = std::round(h / 4.0f) * 4.0f;
    out->width = std::clamp(w, 24.0f, doc::kMaxPanelSize);
    out->height = std::clamp(h, 24.0f, doc::kMaxPanelSize);
    return out;
  }
  void frameSelect() {
    const ui::FaceTransform tr = win_.panelTransform();
    const Vec2 a = tr.unmap(press_), b = tr.unmap(last_);
    const Rect r{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
    std::vector<uint32_t> s = frameKeep_;
    GroupPanelFace& f = win_.face();
    for (size_t i = 0; i < f.elementCount(); ++i) {
      if (!f.shown(i)) continue;
      const Rect q = f.element(i).rect();
      if (q.x0 < r.x1 && r.x0 < q.x1 && q.y0 < r.y1 && r.y0 < q.y1 && std::find(s.begin(), s.end(), f.element(i).id()) == s.end())
        s.push_back(f.element(i).id());
    }
    win_.setSelection(std::move(s));
  }
  void openMenu(Vec2 at) {
    std::vector<ui::MenuItem> items;
    std::weak_ptr<int> alive = win_.alive_;
    PanelWindow* w = &win_;
    auto act = [alive, w](PanelWindow::AlignTo a) {
      return [alive, w, a] {
        if (!alive.expired()) (void)w->alignSelected(a);
      };
    };
    const bool any = !win_.selection_.empty(), many = win_.selection_.size() > 1;
    std::vector<ui::MenuItem> align = {
        ui::MenuItem::item("Left", act(PanelWindow::AlignTo::Left)),     ui::MenuItem::item("Centre", act(PanelWindow::AlignTo::Centre)),
        ui::MenuItem::item("Right", act(PanelWindow::AlignTo::Right)),   ui::MenuItem::sep(),
        ui::MenuItem::item("Top", act(PanelWindow::AlignTo::Top)),       ui::MenuItem::item("Middle", act(PanelWindow::AlignTo::Middle)),
        ui::MenuItem::item("Bottom", act(PanelWindow::AlignTo::Bottom)), ui::MenuItem::sep(),
        ui::MenuItem::item("Distribute Horizontally", act(PanelWindow::AlignTo::SpaceH)),
        ui::MenuItem::item("Distribute Vertically", act(PanelWindow::AlignTo::SpaceV))};
    for (ui::MenuItem& m : align) m.enabled = many;
    items.push_back(ui::MenuItem::sub("Align", std::move(align)));
    ui::MenuItem front = ui::MenuItem::item("Bring to Front", act(PanelWindow::AlignTo::Front));
    ui::MenuItem back = ui::MenuItem::item("Send to Back", act(PanelWindow::AlignTo::Back));
    front.enabled = back.enabled = any;
    items.push_back(std::move(front));
    items.push_back(std::move(back));
    items.push_back(ui::MenuItem::sep());
    ui::MenuItem dup = ui::MenuItem::item("Duplicate", [alive, w] {
      if (!alive.expired()) (void)w->duplicateSelected();
    }, "Ctrl+D");
    ui::MenuItem del = ui::MenuItem::item("Delete", [alive, w] {
      if (!alive.expired()) (void)w->removeSelected();
    }, "Delete");
    dup.enabled = del.enabled = any;
    ui::MenuItem brk = ui::MenuItem::item("Break Apart", [alive, w] {  // GU.6: an embedded child panel → its elements
      if (!alive.expired()) (void)w->breakApartSelected();
    }, "Ctrl+Shift+G");
    brk.enabled = false;
    if (const doc::PanelPtr p = win_.docPanel())
      for (const doc::PanelElement& x : p->elements)
        if (selected(x.id) && doc::embeddedGroup(x).valid()) brk.enabled = true;
    items.push_back(std::move(brk));
    items.push_back(std::move(dup));
    items.push_back(std::move(del));
    (void)ui::openContextMenu(*this, at, std::move(items));
  }

  PanelWindow& win_;
  Drag drag_ = Drag::None;
  int32_t handle_ = -1, hover_ = -1;
  uint32_t clickOnly_ = 0;
  bool moved_ = false;
  Vec2 press_{0.0f, 0.0f}, last_{0.0f, 0.0f}, pressP_{0.0f, 0.0f};
  doc::PanelPtr base_;
  std::vector<uint32_t> frameKeep_;
  std::vector<float> gx_, gy_;
  ui::Cursor cursor_ = ui::Cursor::Arrow;
};

// ---- the inspector ------------------------------------------------------------------------------------------------

namespace {

constexpr std::string_view kTokens[] = {"value", "signal", "track", "handle", "ink", "frame", "tick", "hover",
                                        "glow", "body", "hub", "driven", "selected", "instrument"};

// A row: its label on the left, its control filling the rest.
ui::Widget& row(ui::Widget& parent, std::string name, std::string label) {
  ui::Widget& r = parent.add<ui::Widget>(std::move(name));
  r.setLayout(ui::LayoutKind::Row);
  r.setHeight(ui::Length::fixed(20.0f));
  r.setGap(4);
  r.add<ui::Label>("label", std::move(label)).setWidth(ui::Length::fixed(62.0f));
  return r;
}

std::string blockName(const ViewContext& ctx, const doc::Device& d) {
  if (!d.name.empty()) return d.name;
  if (const doc::DeviceLibrary* lib = ctx.deviceLibrary())
    if (const doc::DeviceInfo* i = lib->infoOf(d)) return i->name;
  return d.patch;
}

}  // namespace

class PanelInspector final : public ui::Widget {
public:
  explicit PanelInspector(PanelWindow& w) : Widget("inspector"), win_(w) {
    setLayout(ui::LayoutKind::Column);
    setPadding(ui::Insets::all(6));
    setGap(4);
    setHeight(ui::Length::fixed(40.0f));
  }

  // The selection or the document's panel changed: rebuilt at the next frame (never inside the edit that changed it: a
  // control's callback may be running).
  void refresh() {
    stale_ = true;
    requestFrame();
  }
  void onFrame(double) override {
    if (stale_) rebuild();
  }
  bool stale() const noexcept { return stale_; }
  // Rebuilds now when stale (tests read the rows right after an edit).
  void sync() {
    if (stale_) rebuild();
  }

  void paint(ui::PaintContext& ctx) override {
    if (ctx.res.theme == nullptr) return;
    ctx.dl.addRectFilled(ui::intersect(rect(), ctx.clip).toRect(), ctx.res.theme->surface.panel);
  }

private:
  void rebuild() {
    stale_ = false;
    clearChildren();
    curveHost_ = nullptr;
    const doc::PanelPtr p = win_.docPanel();
    seen_ = p;
    std::vector<const doc::PanelElement*> sel;
    if (p)
      for (const uint32_t id : win_.selection_)
        if (const doc::PanelElement* e = p->find(id)) sel.push_back(e);
    float h = 12.0f;
    auto title = [&](std::string t) {
      add<ui::Label>("title", std::move(t), ui::FontRoleId::BodyStrong, ui::TextTone::Primary).setHeight(ui::Length::fixed(18.0f));
      h += 22.0f;
    };
    if (sel.empty()) {
      title("Panel");
      geometryRow("panel-w", "Width", p ? p->width : kNewPanelW, [](doc::Panel& q, double v) { q.width = static_cast<float>(v); });
      geometryRow("panel-h", "Height", p ? p->height : kNewPanelH, [](doc::Panel& q, double v) { q.height = static_cast<float>(v); });
      h += 48.0f;
      add<ui::Label>("hint", "Click an element to inspect it; Shift+click or drag a frame for several.").setHeight(ui::Length::fixed(30.0f));
      h += 34.0f;
      setHeight(ui::Length::fixed(h));
      invalidateLayout();
      return;
    }
    if (sel.size() > 1) {
      title(std::to_string(sel.size()) + " elements");
      static constexpr std::pair<const char*, PanelWindow::AlignTo> kAlign[] = {
          {"Left", PanelWindow::AlignTo::Left}, {"Centre", PanelWindow::AlignTo::Centre}, {"Right", PanelWindow::AlignTo::Right},
          {"Top", PanelWindow::AlignTo::Top},   {"Middle", PanelWindow::AlignTo::Middle}, {"Bottom", PanelWindow::AlignTo::Bottom},
          {"Space H", PanelWindow::AlignTo::SpaceH}, {"Space V", PanelWindow::AlignTo::SpaceV}};
      for (size_t k = 0; k < std::size(kAlign); k += 3) {
        ui::Widget& r = add<ui::Widget>("align" + std::to_string(k / 3));
        r.setLayout(ui::LayoutKind::Row);
        r.setHeight(ui::Length::fixed(20.0f));
        r.setGap(4);
        for (size_t j = k; j < std::min(k + 3, std::size(kAlign)); ++j) {
          auto& b = r.add<ui::Button>(std::string("align:") + kAlign[j].first, kAlign[j].first);
          const PanelWindow::AlignTo a = kAlign[j].second;
          PanelWindow* w = &win_;
          b.onClick = [w, a] { (void)w->alignSelected(a); };
        }
        h += 24.0f;
      }
      bool same = true;
      for (const doc::PanelElement* e : sel) same = same && e->kind == sel.front()->kind;
      if (same) h += styleRows(*sel.front()) + schemaRows(*sel.front());
      setHeight(ui::Length::fixed(h));
      invalidateLayout();
      return;
    }
    const doc::PanelElement& e = *sel.front();
    const ElementSchema* schema = elementSchema(e.kind);
    title(schema != nullptr ? std::string(schema->label) : e.kind);
    // Kind.
    {
      ui::Widget& r = row(*this, "row:kind", "Kind");
      std::vector<std::string> names;
      int32_t at = 0;
      const std::vector<std::string_view> kinds = elementKinds();
      for (size_t k = 0; k < kinds.size(); ++k) {
        const ElementSchema* s = elementSchema(kinds[k]);
        names.emplace_back(s != nullptr ? s->label : kinds[k]);
        if (kinds[k] == e.kind) at = static_cast<int32_t>(k);
      }
      auto& d = r.add<ui::Dropdown>("kind", std::move(names), at);
      PanelWindow* w = &win_;
      d.onChange = [w, kinds](int32_t i) {
        if (i < 0 || static_cast<size_t>(i) >= kinds.size()) return;
        const std::string k(kinds[static_cast<size_t>(i)]);
        (void)w->editSelected([&](doc::PanelElement& x) { x.kind = k; }, "Element Kind");
      };
      h += 24.0f;
    }
    // Bindings (one row per slot of the kind).
    {
      const size_t slots = schema != nullptr ? std::max<size_t>(1, schema->slots.size()) : 1;
      for (size_t s = 0; s < slots; ++s) {
        const std::string label = slots > 1 && schema != nullptr ? "Bound " + std::string(schema->slots[s]) : "Bound to";
        ui::Widget& r = row(*this, "row:bind" + std::to_string(s), label);
        auto& b = r.add<ui::Button>(s == 0 ? std::string("bind") : "bind" + std::to_string(s), bindingText(e, s));
        PanelWindow* w = &win_;
        PanelInspector* self = this;
        std::weak_ptr<int> alive = win_.alive_;
        ui::Button* bp = &b;
        b.onClick = [self, w, alive, s, bp] {
          if (alive.expired()) return;
          (void)ui::openContextMenu(*bp, {static_cast<float>(bp->rect().x), static_cast<float>(bp->rect().bottom())}, self->bindingItems(*w, s));
        };
        h += 24.0f;
      }
    }
    // Visibility.
    {
      ui::Widget& r = row(*this, "row:when", "Shown");
      std::vector<std::string> names{"Always"};
      whenChoices_.clear();
      whenChoices_.push_back({0, 0.0});
      int32_t at = 0;
      for (const doc::PanelElement& src : p->elements) {
        if (src.id == e.id) continue;
        const std::vector<std::string> values = sourceValues(src);
        for (size_t v = 0; v < values.size(); ++v) {
          names.push_back(sourceName(src) + " = " + values[v]);
          whenChoices_.push_back({src.id, static_cast<double>(v)});
          if (e.when && e.when->element == src.id && e.when->values.size() == 1 && std::fabs(e.when->values.front() - static_cast<double>(v)) < 1e-6)
            at = static_cast<int32_t>(names.size()) - 1;
        }
      }
      if (e.when && at == 0) {
        names.push_back("(a condition of several values)");
        at = static_cast<int32_t>(names.size()) - 1;
      }
      auto& d = r.add<ui::Dropdown>("when", std::move(names), at);
      PanelWindow* w = &win_;
      const std::vector<std::pair<uint32_t, double>> choices = whenChoices_;
      d.onChange = [w, choices](int32_t i) {
        if (i < 0 || static_cast<size_t>(i) >= choices.size()) return;
        const auto [src, v] = choices[static_cast<size_t>(i)];
        (void)w->editSelected(
            [&](doc::PanelElement& x) {
              if (src == 0) x.when.reset();
              else x.when = doc::Visibility{src, {v}};
            },
            "Element Visibility");
      };
      h += 24.0f;
    }
    // Place and size.
    geometryRow("x", "X", e.x, [id = e.id](doc::Panel& q, double v) { if (doc::PanelElement* x = find(q, id)) x->x = static_cast<float>(v); });
    geometryRow("y", "Y", e.y, [id = e.id](doc::Panel& q, double v) { if (doc::PanelElement* x = find(q, id)) x->y = static_cast<float>(v); });
    geometryRow("w", "Width", e.w, [id = e.id](doc::Panel& q, double v) { if (doc::PanelElement* x = find(q, id)) x->w = std::max(1.0f, static_cast<float>(v)); });
    geometryRow("h", "Height", e.h, [id = e.id](doc::Panel& q, double v) { if (doc::PanelElement* x = find(q, id)) x->h = std::max(1.0f, static_cast<float>(v)); });
    h += 4.0f * 24.0f;
    h += styleRows(e);
    h += schemaRows(e);
    // The curve of a bound control (the curve editor element on the same binding: its edits are the document's).
    if (const doc::Binding* b = e.binding(); b != nullptr && b->kind == doc::BindingKind::Control && e.kind != "curve") {
      add<ui::Label>("curve-label", "Curve").setHeight(ui::Length::fixed(14.0f));
      auto panel = std::make_shared<doc::Panel>();
      panel->width = kInspectorCurveW;
      panel->height = kInspectorCurveH;
      doc::PanelElement c;
      c.id = 1;
      c.kind = "curve";
      c.w = kInspectorCurveW;
      c.h = kInspectorCurveH;
      c.bindings.push_back(*b);
      panel->elements.push_back(std::move(c));
      doc::PanelPtr fixed = panel;
      PanelHost* hostp = nullptr;
      auto face = std::make_unique<GroupPanelFace>([fixed] { return fixed; },
                                                   makeDocPanelTarget(win_.ctx_, win_.group_, [this]() -> ui::Widget* { return curveHost_; }));
      PanelHost& host = add<PanelHost>("curve", std::move(face));
      hostp = &host;
      host.setHeight(ui::Length::fixed(kInspectorCurveH));
      curveHost_ = hostp;
      h += 18.0f + kInspectorCurveH + 4.0f;
    }
    setHeight(ui::Length::fixed(h));
    invalidateLayout();
  }

  static doc::PanelElement* find(doc::Panel& p, uint32_t id) {
    const int32_t i = p.indexOf(id);
    return i >= 0 ? &p.elements[static_cast<size_t>(i)] : nullptr;
  }

  // A number row whose drag previews and commits once (the panel's or an element's geometry).
  void geometryRow(std::string name, std::string label, double value, std::function<void(doc::Panel&, double)> set) {
    ui::Widget& r = row(*this, "row:" + name, std::move(label));
    ui::ValueSpec spec;
    spec.min = name.starts_with("panel") || name == "w" || name == "h" ? 1.0 : 0.0;
    spec.max = doc::kMaxPanelSize;
    spec.def = value;
    spec.step = 1.0;
    spec.dragPixels = 800.0;
    spec.decimals = 0;
    auto& box = r.add<ui::DragBox>(name, spec);
    box.setValue(value);
    hookBox(box, [set](doc::Panel& q, double v) { set(q, v); }, "Panel Element Place");
  }

  // A DragBox's gesture: a preview per change, one step at its end (Esc: nothing).
  void hookBox(ui::DragBox& box, std::function<void(doc::Panel&, double)> set, const char* label) {
    PanelWindow* w = &win_;
    auto base = std::make_shared<doc::PanelPtr>();
    box.onGestureBegin = [w, base] { *base = w->docPanel(); };
    box.onChange = [w, base, set](double v) {
      if (!*base) *base = w->docPanel();
      auto q = std::make_shared<doc::Panel>(*base ? **base : doc::Panel{kNewPanelW, kNewPanelH, {}});
      set(*q, v);
      w->preview(q);
    };
    box.onGestureEnd = [w, base, label] {
      *base = nullptr;
      (void)w->commitPreview(label);
    };
    box.onGestureCancel = [w, base] {
      *base = nullptr;
      w->cancelPreview();
    };
  }

  // The kind's style fields (registry schema): a number box, a choice, a theme token or a text each. Height added.
  float styleRows(const doc::PanelElement& e) {
    const ElementSchema* schema = elementSchema(e.kind);
    if (schema == nullptr || schema->style.empty()) return 0.0f;
    add<ui::Label>("style-label", "Style").setHeight(ui::Length::fixed(14.0f));
    float h = 18.0f;
    PanelWindow* w = &win_;
    for (const StyleSpec& s : schema->style) {
      const std::string field(s.name);
      ui::Widget& r = row(*this, "row:style:" + field, field);
      const std::string name = "style:" + field;
      switch (s.type) {
        case StyleSpec::Type::Number: {
          ui::ValueSpec spec;
          spec.min = s.min < s.max ? s.min : -10000.0;
          spec.max = s.min < s.max ? s.max : 10000.0;
          spec.def = s.number;
          spec.step = 0.0;
          spec.decimals = 1;
          spec.dragPixels = 300.0;
          auto& box = r.add<ui::DragBox>(name, spec);
          box.setValue(e.number(s.name, s.number));
          const std::vector<uint32_t> ids = win_.selection_;
          hookBox(box, [ids, field](doc::Panel& q, double v) {
            for (const uint32_t id : ids)
              if (doc::PanelElement* x = find(q, id)) x->setField(doc::styleNumber(field, v));
          }, "Element Style");
          break;
        }
        case StyleSpec::Type::Choice:
        case StyleSpec::Type::Token: {
          std::vector<std::string> items;
          if (s.type == StyleSpec::Type::Choice)
            for (const std::string_view c : s.choices) items.emplace_back(c);
          else
            for (const std::string_view c : kTokens) items.emplace_back(c);
          const std::string_view now = e.text(s.name, s.text);
          int32_t at = 0;
          for (size_t k = 0; k < items.size(); ++k)
            if (items[k] == now) at = static_cast<int32_t>(k);
          auto& d = r.add<ui::Dropdown>(name, items, at);
          d.onChange = [w, items, field](int32_t i) {
            if (i < 0 || static_cast<size_t>(i) >= items.size()) return;
            const std::string v = items[static_cast<size_t>(i)];
            (void)w->editSelected([&](doc::PanelElement& x) { x.setField(doc::styleText(field, v)); }, "Element Style");
          };
          break;
        }
        case StyleSpec::Type::Text: {
          auto& t = r.add<ui::TextEdit>(name, std::string(e.text(s.name, s.text)));
          t.onCommit = [w, field](const std::string& v) {
            (void)w->editSelected([&](doc::PanelElement& x) { x.setField(doc::styleText(field, v)); }, "Element Style");
          };
          break;
        }
      }
      h += 24.0f;
    }
    return h;
  }

  // GU.8: the kind's style schemas — apply one to the selection (one step), save the element's style as one. Height.
  float schemaRows(const doc::PanelElement& e) {
    if (schemaFields(e).empty()) return 0.0f;
    std::vector<std::string> names{"Apply…"};
    for (const StyleSchema* s : StyleSchemas::shared().forKind(e.kind)) names.push_back(s->name);
    PanelWindow* w = &win_;
    {
      ui::Widget& r = row(*this, "row:schema", "Schema");
      auto& d = r.add<ui::Dropdown>("schema", names, 0);
      d.setTooltip("Apply a named style set to the selected elements of this kind");
      d.onChange = [w, kind = e.kind, names](int32_t i) {
        if (i <= 0 || static_cast<size_t>(i) >= names.size()) return;
        const StyleSchema* s = StyleSchemas::shared().find(kind, names[static_cast<size_t>(i)]);
        if (s == nullptr) return;
        const StyleSchema pick = *s;
        (void)w->editSelected([&](doc::PanelElement& x) { (void)applyStyleSchema(pick, x); }, "Apply Style Schema");
      };
    }
    {
      ui::Widget& r = row(*this, "row:schema-save", "Save as");
      auto& t = r.add<ui::TextEdit>("schema-name", std::string());
      t.setTooltip("Type a name and press Enter: this element's style becomes a schema of its kind");
      PanelInspector* self = this;
      std::weak_ptr<int> alive = win_.alive_;
      t.onCommit = [w, self, alive, id = e.id](const std::string& v) {
        if (alive.expired()) return;
        const doc::PanelPtr p = w->docPanel();
        const doc::PanelElement* x = p ? p->find(id) : nullptr;
        if (x == nullptr) return;
        if (auto saved = StyleSchemas::shared().save(*x, v); saved) self->refresh();
      };
    }
    return 48.0f;
  }

  std::string bindingText(const doc::PanelElement& e, size_t slot) const {
    const doc::Binding* b = e.binding(slot);
    if (b == nullptr) return "None";
    const BoundInfo& i = win_.face().target().info(*b);
    std::string block;
    if (const doc::Snapshot& s = win_.ctx_.doc())
      if (const doc::Device* d = doc::findDevice(*s, b->device).device) block = blockName(win_.ctx_, *d);
    const std::string what = i.valid && !i.label.empty() ? i.label : b->name;
    return block.empty() || block == what ? what : block + " \xC2\xB7 " + what;
  }

  // The group's controls, settings and outputs as a menu (a submenu per block inside the group, any depth): a pick binds
  // the selected element's slot `slot` to it (one step).
  std::vector<ui::MenuItem> bindingItems(PanelWindow& w, size_t slot) {
    std::vector<ui::MenuItem> items;
    const doc::Snapshot& snap = w.ctx_.doc();
    const doc::DeviceLoc gl = snap ? doc::findDevice(*snap, w.group_) : doc::DeviceLoc{};
    if (gl.track == nullptr) return items;
    std::weak_ptr<int> alive = w.alive_;
    PanelWindow* wp = &w;
    auto bind = [alive, wp, slot](doc::Binding b) {
      return [alive, wp, slot, b] {
        if (alive.expired()) return;
        (void)wp->editSelected([&](doc::PanelElement& x) {
          if (x.bindings.size() <= slot) x.bindings.resize(slot + 1);
          x.bindings[slot] = b;
        }, "Element Binding");
      };
    };
    items.push_back(ui::MenuItem::item("None", bind(doc::Binding{})));
    items.push_back(ui::MenuItem::sep());
    const doc::DeviceLibrary* lib = w.ctx_.deviceLibrary();
    for (const doc::Device* d : doc::groupMembersDeep(gl.track->devices, w.group_)) {
      if (doc::isGroupDevice(*d) || lib == nullptr) continue;
      const graph::Patch* patch = lib->patchOf(*d);
      if (patch == nullptr) continue;
      const devices::DeviceSignature sig = devices::signatureOf(*patch);
      std::vector<ui::MenuItem> sub;
      for (const devices::ParamInfo& pi : sig.params)
        sub.push_back(ui::MenuItem::item(pi.label.empty() ? pi.name : pi.label, bind(doc::controlBinding(d->id, pi.name))));
      for (const devices::ConstInfo& ci : sig.consts) sub.push_back(ui::MenuItem::item(ci.name + " (setting)", bind(doc::settingBinding(d->id, ci.name))));
      const graph::SubcircuitRef ref = graph::subcircuitSignature(*patch);
      if (!ref.outputs.empty() && !sub.empty()) sub.push_back(ui::MenuItem::sep());
      for (const graph::SubcircuitPort& o : ref.outputs) sub.push_back(ui::MenuItem::item("Output " + o.name, bind(doc::signalBinding(d->id, o.name))));
      if (!sub.empty()) items.push_back(ui::MenuItem::sub(blockName(w.ctx_, *d), std::move(sub)));
    }
    if (doc::isInstrumentGroup(*snap, w.group_)) {
      std::vector<ui::MenuItem> sub;
      for (const doc::SettingSpec& s : doc::groupSettings())
        sub.push_back(ui::MenuItem::item(std::string(s.label), bind(doc::settingBinding(w.group_, std::string(s.name)))));
      items.push_back(ui::MenuItem::sub("Polyphony", std::move(sub)));
    }
    return items;
  }

  // What a visibility condition can name: a tabs element's items, a menu's choices, a toggle's Off / On.
  std::vector<std::string> sourceValues(const doc::PanelElement& src) const {
    if (src.kind == "tabs") return splitItems(src.text("items", "A|B"));
    if (src.kind == "toggle" || src.kind == "button") return {"Off", "On"};
    if (src.kind == "menu") {
      std::vector<std::string> items = splitItems(src.text("items", ""));
      if (items.empty())
        if (const doc::Binding* b = src.binding()) items = win_.face().target().info(*b).choices;
      return items;
    }
    return {};
  }
  std::string sourceName(const doc::PanelElement& src) const {
    const std::string_view label = src.text("label", "");
    if (!label.empty() && label != "-") return std::string(label);
    if (const doc::Binding* b = src.binding()) {
      const BoundInfo& i = win_.face().target().info(*b);
      if (i.valid && !i.label.empty()) return i.label;
    }
    const ElementSchema* s = elementSchema(src.kind);
    return (s != nullptr ? std::string(s->label) : src.kind) + " " + std::to_string(src.id);
  }

  static constexpr float kInspectorCurveW = 196.0f, kInspectorCurveH = 96.0f;
  PanelWindow& win_;
  bool stale_ = true;
  doc::PanelPtr seen_;
  PanelHost* curveHost_ = nullptr;
  std::vector<std::pair<uint32_t, double>> whenChoices_;
  friend class PanelWindow;
};

// ---- the grip -----------------------------------------------------------------------------------------------------

class PanelWindowGrip final : public ui::Widget {
public:
  explicit PanelWindowGrip(PanelWindow& w) : Widget("grip"), win_(w) { setCursor(ui::Cursor::ResizeNWSE); }
  bool onPointer(const ui::PointerEvent& e) override { return win_.gripPointer(e); }
  void paint(ui::PaintContext& ctx) override {
    if (ctx.res.theme == nullptr) return;
    const Rect r = rect().toRect();
    const Rgba c = ui::mulAlpha(ctx.res.theme->text.secondary, 0.7f);
    ui::FacePainter p(ctx.dl, ui::FaceTransform{});
    const float w = r.x1 - r.x0;
    for (int k = 1; k <= 3; ++k) {  // three diagonal strokes in the corner
      const float d = static_cast<float>(k) * w / 4.0f;
      p.line({r.x1 - d - 1.0f, r.y1 - 2.0f}, {r.x1 - 2.0f, r.y1 - d - 1.0f}, std::max(1.0f, ctx.scale.factor), c);
    }
  }

private:
  PanelWindow& win_;
};

// ---- the window ---------------------------------------------------------------------------------------------------

PanelWindow::PanelWindow(ViewContext& ctx, doc::DeviceId group, Hooks hooks) : Widget("panel-window[" + std::to_string(group.value) + "]"),
                                                                              ctx_(ctx), group_(group), hooks_(std::move(hooks)) {
  setLayout(ui::LayoutKind::Custom);
  setClipChildren(true);
  setVisualOverflow(ui::Insets::all(6));
  auto blank = std::make_shared<doc::Panel>();
  blank->width = kNewPanelW;
  blank->height = kNewPanelH;
  empty_ = blank;
  // The panel: the document's (or the editor's preview, or the empty panel), bound to the document.
  GroupPanelFace::Source docSource = docPanelSource(ctx, group);
  PanelWindow* self = this;
  auto source = [self, docSource]() -> doc::PanelPtr {
    if (self->preview_) return self->preview_;
    doc::PanelPtr p = docSource();
    return p ? p : self->empty_;
  };
  auto face = std::make_unique<GroupPanelFace>(std::move(source), makeDocPanelTarget(ctx, group, [self]() -> ui::Widget* { return self->host_; }));
  face->setOwner(group);
  host_ = &add<PanelHost>("panel", std::move(face));
  host_->onHoverPart = [self] { self->linksChanged(); };
  editor_ = &add<PanelEditor>(*this);
  auto& scroll = add<ui::ScrollView>("inspector-scroll", false, true);
  inspectorScroll_ = &scroll;
  inspector_ = &scroll.add<PanelInspector>(*this);
  auto& mode = add<ui::Toggle>("mode", "Edit", ui::ToggleStyle::Accent, false);
  mode.setTooltip("Edit the panel (move, resize, inspect its elements) / play it");
  mode.onToggle = [self](bool on) { self->setEditMode(on); };
  modeButton_ = &mode;
  auto& send = add<ui::Button>("send", "+");
  send.setTooltip("Send the control selected in the circuit to this panel (P)");
  send.onClick = [self] {
    if (self->hooks_.send) self->hooks_.send(*self);
  };
  auto& min = add<ui::Button>("min", "\xE2\x80\x93");
  min.setTooltip("Minimise (the title bar only)");
  min.onClick = [self] { self->setMinimised(!self->state_.minimised); };
  auto& close = add<ui::Button>("close", "\xC3\x97");
  close.setTooltip("Close (the group's header button opens it again)");
  close.onClick = [self] {
    self->state_.open = false;
    self->setVisible(false);
    if (self->hooks_.closed) self->hooks_.closed(*self);
  };
  grip_ = &add<PanelWindowGrip>(*this);
  layoutChildren();
}

PanelWindow::~PanelWindow() {
  if (observing_ != nullptr) observing_->removeObserver(this);
}

GroupPanelFace& PanelWindow::face() const noexcept { return static_cast<GroupPanelFace&>(host_->face()); }

doc::PanelPtr PanelWindow::docPanel() const {
  const doc::Snapshot& s = ctx_.doc();
  const doc::Device* d = s ? doc::findDevice(*s, group_).device : nullptr;
  return d != nullptr && d->hasPanel() ? d->panel : nullptr;
}

doc::PanelPtr PanelWindow::shownPanel() const {
  if (preview_) return preview_;
  return docPanel();
}

void PanelWindow::setState(const doc::PanelWindowView& s) {
  const bool layout = s.edit != state_.edit || s.minimised != state_.minimised;
  state_ = s;
  static_cast<ui::Toggle*>(modeButton_)->setOn(s.edit);
  if (layout) layoutChildren();
  setVisible(s.open);
}

void PanelWindow::layoutChildren() {
  const bool body = !state_.minimised;
  host_->setVisible(body);
  editor_->setVisible(body && state_.edit);
  inspectorScroll_->setVisible(body && state_.edit);
  grip_->setVisible(body);
  if (state_.edit) inspector_->refresh();
  invalidateArrange();
  markDirty();
}

void PanelWindow::setEditMode(bool on) {
  if (on == state_.edit) return;
  if (preview_) cancelPreview();
  // The window grows by the inspector's width going into edit mode (the panel keeps its zoom) and gives it back after.
  const float dw = on ? kInspectorW : -kInspectorW;
  if (state_.w > 0.0f) state_.w = std::max(kMinW, state_.w + dw);
  state_.edit = on;
  static_cast<ui::Toggle*>(modeButton_)->setOn(on);
  if (!on) selection_.clear();
  layoutChildren();
  if (parent() != nullptr) parent()->invalidateArrange();
  if (hooks_.changed) hooks_.changed(*this);
  linksChanged();
}

void PanelWindow::setMinimised(bool on) {
  if (on == state_.minimised) return;
  state_.minimised = on;
  layoutChildren();
  if (parent() != nullptr) parent()->invalidateArrange();
  if (hooks_.changed) hooks_.changed(*this);
  linksChanged();
}

void PanelWindow::setDropTarget(bool on) {
  if (on == drop_) return;
  drop_ = on;
  markDirty();
}

void PanelWindow::arrange(ui::LayoutPass& pass) {
  const IRect r = rect();
  auto px = [&](float v) { return pass.px(v); };
  const int32_t th = px(kTitleH), bh = px(18.0f), by = r.y + (th - bh) / 2;
  int32_t x = r.right() - px(4.0f);
  for (const char* n : {"close", "min", "send"}) {
    ui::Widget* b = find(n);
    x -= bh;
    pass.place(*b, {x, by, bh, bh});
    x -= px(3.0f);
  }
  x -= px(40.0f);
  pass.place(*modeButton_, {x, by, px(40.0f), bh});
  if (state_.minimised) return;
  const IRect body{r.x + 1, r.y + th, r.w - 2, r.h - th - 1};
  const int32_t iw = state_.edit && body.w > px(kInspectorW + 80.0f) ? px(kInspectorW) : 0;
  const int32_t pad = px(kPad);
  const IRect area{body.x + pad, body.y + pad, std::max(1, body.w - iw - 2 * pad), std::max(1, body.h - 2 * pad - px(kGripPx) / 2)};
  pass.place(*host_, area);
  pass.place(*editor_, area);
  pass.place(*inspectorScroll_, {body.right() - iw, body.y, iw, body.h});
  pass.place(*grip_, {r.right() - px(kGripPx), r.bottom() - px(kGripPx), px(kGripPx), px(kGripPx)});
}

const std::string& PanelWindow::title() const {
  const doc::Snapshot& s = ctx_.doc();
  if (s != titleOf_) {
    titleOf_ = s;
    const doc::Device* d = s ? doc::findDevice(*s, group_).device : nullptr;
    title_ = d != nullptr && !d->name.empty() ? d->name : "Group";
    title_ += " \xC2\xB7 Panel";
  }
  return title_;
}

void PanelWindow::paint(ui::PaintContext& ctx) {
  if (observing_ == nullptr && tree() != nullptr) {  // (the press observer: is the window's panel the selected one)
    observing_ = tree();
    observing_->addObserver(this);
  }
  if (ctx.res.theme == nullptr) return;
  const ui::Theme& th = *ctx.res.theme;
  ui::DrawList& dl = ctx.dl;
  const float us = ctx.scale.factor;
  const Rect r = rect().toRect();
  const float rad = std::round(5.0f * us);
  dl.addRoundedRectFilled({r.x0 + 2.0f * us, r.y0 + 4.0f * us, r.x1 + 3.0f * us, r.y1 + 5.0f * us}, rad, ui::mulAlpha(th.menu.shadow, 0.6f));
  dl.addRoundedRectFilled(r, rad, th.device.background);
  const float tbar = std::round(kTitleH * us);
  dl.addRoundedRectFilled({r.x0, r.y0, r.x1, r.y0 + tbar}, rad, active_ ? ui::lerp(th.device.titleBar, th.circuit.selected, 0.18f) : th.device.titleBar);
  if (!state_.minimised) dl.addRectFilled({r.x0, r.y0 + tbar - rad, r.x1, r.y0 + tbar}, active_ ? ui::lerp(th.device.titleBar, th.circuit.selected, 0.18f) : th.device.titleBar);
  if (ctx.res.uiFonts != nullptr) {
    const ui::IRect tb{static_cast<int32_t>(r.x0 + 8.0f * us), static_cast<int32_t>(r.y0), static_cast<int32_t>(r.x1 - r.x0 - 150.0f * us),
                       static_cast<int32_t>(tbar)};
    paint::text(ctx, ctx.res.uiFonts->small, tb, title(), th.device.titleText, ui::TextAlign::Left);
  }
  const Rgba border = drop_ ? th.accent.primary : active_ ? th.circuit.selected : th.device.border;
  dl.addRoundedRectStroke(r, rad, border, drop_ ? std::max(2.0f, std::round(2.0f * us)) : 1.0f);
}

void PanelWindow::paintOver(ui::PaintContext& ctx) {
  if (!drop_ || ctx.res.theme == nullptr || state_.minimised) return;
  const IRect a = host_->rect();
  ctx.dl.addRectFilled(a.toRect(), ui::mulAlpha(ctx.res.theme->accent.primary, 0.08f));
}

bool PanelWindow::onPointer(const ui::PointerEvent& e) {
  using T = ui::PointerType;
  const float us = tree() != nullptr ? tree()->scale() : 1.0f;
  if (e.type == T::Down && e.button == ui::PointerButton::Left) {
    if (e.pos.y < static_cast<float>(rect().y) + kTitleH * us) {
      if (e.clickCount >= 2) {
        setMinimised(!state_.minimised);
        return true;
      }
      drag_ = Drag::Move;
      press_ = e.pos;
      pressState_ = state_;
      return true;
    }
    return true;  // the window's own background: nothing under it gets the press
  }
  if (e.type == T::Move && drag_ == Drag::Move) {
    state_.x = std::round(pressState_.x + (e.pos.x - press_.x) / us);
    state_.y = std::round(pressState_.y + (e.pos.y - press_.y) / us);
    if (parent() != nullptr) parent()->invalidateArrange();
    linksChanged();
    return true;
  }
  if (e.type == T::Up && drag_ == Drag::Move) {
    drag_ = Drag::None;
    if (hooks_.changed && (state_.x != pressState_.x || state_.y != pressState_.y)) hooks_.changed(*this);
    return true;
  }
  return e.type == T::Down || e.type == T::Up || e.type == T::Wheel;
}

bool PanelWindow::gripPointer(const ui::PointerEvent& e) {
  using T = ui::PointerType;
  const float us = tree() != nullptr ? tree()->scale() : 1.0f;
  if (e.type == T::Down && e.button == ui::PointerButton::Left) {
    drag_ = Drag::Size;
    press_ = e.pos;
    pressState_ = state_;
    if (pressState_.w <= 0.0f) pressState_.w = static_cast<float>(rect().w) / us;
    if (pressState_.h <= 0.0f) pressState_.h = static_cast<float>(rect().h) / us;
    return true;
  }
  if (e.type == T::Move && drag_ == Drag::Size) {
    state_.w = std::max(kMinW, std::round(pressState_.w + (e.pos.x - press_.x) / us));
    state_.h = std::max(kMinH, std::round(pressState_.h + (e.pos.y - press_.y) / us));
    if (parent() != nullptr) parent()->invalidateArrange();
    linksChanged();
    return true;
  }
  if (e.type == T::Up && drag_ == Drag::Size) {
    drag_ = Drag::None;
    if (hooks_.changed) hooks_.changed(*this);
    return true;
  }
  return false;
}

void PanelWindow::onCaptureLost() {
  if (drag_ != Drag::None) {
    state_ = pressState_;
    drag_ = Drag::None;
    if (parent() != nullptr) parent()->invalidateArrange();
  }
}

ui::Cursor PanelWindow::cursorAt(Vec2 p) const {
  const float us = tree() != nullptr ? tree()->scale() : 1.0f;
  return p.y < static_cast<float>(rect().y) + kTitleH * us ? ui::Cursor::Move : ui::Cursor::Arrow;
}

ui::InfoText PanelWindow::info() const {
  return {title(), state_.edit ? "Edit mode: click / Shift+click / drag a frame to select, drag to move (snaps; Alt: free), handles resize, "
                                 "Delete removes; the inspector edits the selected element. Every gesture is one undo step."
                               : "Play mode: the panel's controls edit the group. Drag a mod source onto a control for a cable. "
                                 "Edit (title bar) arranges the panel; + sends the control selected in the circuit (P)."};
}

void PanelWindow::onPointerDown(Vec2 pos) {
  const bool in = visible() && rect().contains(pos);
  if (in == active_) return;
  active_ = in;
  markDirty();
  linksChanged();
}

void PanelWindow::linksChanged() {
  if (hooks_.linksChanged) hooks_.linksChanged(*this);
}

void PanelWindow::documentChanged() {
  // Elements gone (an undo, a deleted block): out of the selection.
  if (const doc::PanelPtr p = docPanel(); !selection_.empty()) {
    std::vector<uint32_t> keep;
    for (const uint32_t id : selection_)
      if (p && p->find(id) != nullptr) keep.push_back(id);
    if (keep.size() != selection_.size()) selection_ = std::move(keep);
  }
  host_->notifyChanged();
  if (state_.edit && inspector_->seen_ != docPanel()) inspector_->refresh();
  markDirty();
}

ui::FaceTransform PanelWindow::panelTransform() const noexcept { return host_->transform(); }

bool PanelWindow::panelPoint(Vec2 device, Vec2& out) const noexcept {
  if (state_.minimised || !host_->rect().contains(device)) return false;
  out = panelTransform().unmap(device);
  const Vec2 ws = face().worldSize();
  return out.x >= 0.0f && out.y >= 0.0f && out.x <= ws.x && out.y <= ws.y;
}

std::optional<Vec2> PanelWindow::elementPoint(size_t i) const {
  GroupPanelFace& f = face();
  if (i >= f.elementCount() || !f.shown(i) || state_.minimised) return std::nullopt;
  const std::optional<ControlAnchor> a = f.elementAnchor(i);
  if (!a) return std::nullopt;
  return panelTransform().map(a->at);
}

int32_t PanelWindow::hoveredElement() const noexcept {
  if (state_.minimised) return -1;
  if (state_.edit) return editor_->hovered();
  const int32_t h = face().hoveredPart();
  return h >= 0 ? h : -1;
}

const std::vector<float>& PanelWindow::guidesX() const noexcept { return editor_->guidesX(); }
const std::vector<float>& PanelWindow::guidesY() const noexcept { return editor_->guidesY(); }

void PanelWindow::setSelection(std::vector<uint32_t> ids) {
  if (ids == selection_) return;
  selection_ = std::move(ids);
  inspector_->refresh();
  editor_->markDirty();
  linksChanged();
}

void PanelWindow::preview(doc::PanelPtr p) {
  preview_ = std::move(p);
  host_->notifyChanged();
  editor_->markDirty();
}

void PanelWindow::cancelPreview() {
  if (!preview_) return;
  preview_ = nullptr;
  host_->notifyChanged();
  editor_->markDirty();
}

bool PanelWindow::commitPreview(const char* /*label*/) {
  doc::PanelPtr p = std::move(preview_);
  preview_ = nullptr;
  host_->notifyChanged();
  if (!p) return false;
  const doc::Snapshot& s = ctx_.doc();
  if (!s) return false;
  const doc::ops::EditResult e = doc::ops::setGroupPanel(*s, group_, p);
  if (!e) {
    ctx_.setStatus(e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status st = ctx_.store().dispatch(e->command); !st) {
    ctx_.setStatus(st.error().message);
    return false;
  }
  return true;
}

bool PanelWindow::editPanel(const std::function<void(doc::Panel&)>& edit, const char* label) {
  const doc::PanelPtr base = docPanel();
  auto p = std::make_shared<doc::Panel>(base ? *base : *empty_);
  edit(*p);
  preview_ = p;
  return commitPreview(label);
}

bool PanelWindow::editSelected(const std::function<void(doc::PanelElement&)>& edit, const char* label) {
  if (selection_.empty()) return false;
  return editPanel([&](doc::Panel& p) {
    for (doc::PanelElement& e : p.elements)
      if (std::find(selection_.begin(), selection_.end(), e.id) != selection_.end()) edit(e);
  }, label);
}

bool PanelWindow::removeSelected() {
  if (selection_.empty()) return false;
  const doc::Snapshot& s = ctx_.doc();
  const doc::ops::EditResult e = doc::ops::removePanelElements(*s, group_, selection_);
  if (!e || e->empty()) return false;
  if (!ctx_.store().dispatch(e->command)) return false;
  setSelection({});
  return true;
}

bool PanelWindow::duplicateSelected() {
  const doc::PanelPtr base = docPanel();
  if (!base || selection_.empty()) return false;
  std::vector<doc::PanelElement> copies;
  for (const doc::PanelElement& e : base->elements)
    if (std::find(selection_.begin(), selection_.end(), e.id) != selection_.end()) copies.push_back(e);
  doc::Panel p = *base;
  std::vector<uint32_t> ids;
  for (doc::PanelElement& c : copies) {
    c.id = p.nextId();
    c.x += 12.0f;
    c.y += 12.0f;
    c.when.reset();
    p.width = std::min(doc::kMaxPanelSize, std::max(p.width, c.x + c.w + kPlaceGap));
    p.height = std::min(doc::kMaxPanelSize, std::max(p.height, c.y + c.h + kPlaceGap));
    ids.push_back(c.id);
    p.elements.push_back(std::move(c));
  }
  preview_ = std::make_shared<doc::Panel>(std::move(p));
  if (!commitPreview("Duplicate Panel Elements")) return false;
  setSelection(std::move(ids));
  return true;
}

bool PanelWindow::breakApartSelected() {
  const doc::Snapshot& s = ctx_.doc();
  if (!s || selection_.empty()) return false;
  std::vector<uint32_t> ids;
  const doc::ops::EditResult e = doc::ops::breakApartPanelElements(*s, group_, selection_, &ids);
  if (!e) {
    ctx_.setStatus(e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status st = ctx_.store().dispatch(e->command); !st) {
    ctx_.setStatus(st.error().message);
    return false;
  }
  documentChanged();
  setSelection(std::move(ids));
  return true;
}

bool PanelWindow::nudgeSelected(float dx, float dy) {
  return editSelected([&](doc::PanelElement& e) {
    e.x = std::max(0.0f, e.x + dx);
    e.y = std::max(0.0f, e.y + dy);
  }, "Nudge Panel Elements");
}

bool PanelWindow::alignSelected(AlignTo a) {
  const doc::PanelPtr base = docPanel();
  if (!base || selection_.empty()) return false;
  auto in = [&](uint32_t id) { return std::find(selection_.begin(), selection_.end(), id) != selection_.end(); };
  if (a == AlignTo::Front || a == AlignTo::Back) {
    return editPanel([&](doc::Panel& p) {
      std::stable_partition(p.elements.begin(), p.elements.end(), [&](const doc::PanelElement& e) { return a == AlignTo::Back ? in(e.id) : !in(e.id); });
    }, a == AlignTo::Front ? "Bring to Front" : "Send to Back");
  }
  if (selection_.size() < 2) return false;
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (const doc::PanelElement& e : base->elements)
    if (in(e.id)) x0 = std::min(x0, e.x), y0 = std::min(y0, e.y), x1 = std::max(x1, e.x + e.w), y1 = std::max(y1, e.y + e.h);
  return editPanel([&](doc::Panel& p) {
    std::vector<doc::PanelElement*> sel;
    for (doc::PanelElement& e : p.elements)
      if (in(e.id)) sel.push_back(&e);
    if (a == AlignTo::SpaceH || a == AlignTo::SpaceV) {
      const bool h = a == AlignTo::SpaceH;
      std::sort(sel.begin(), sel.end(), [&](const doc::PanelElement* l, const doc::PanelElement* r) { return h ? l->x < r->x : l->y < r->y; });
      float total = 0.0f;
      for (const doc::PanelElement* e : sel) total += h ? e->w : e->h;
      const float gap = ((h ? x1 - x0 : y1 - y0) - total) / static_cast<float>(sel.size() - 1);
      float at = h ? x0 : y0;
      for (doc::PanelElement* e : sel) {
        (h ? e->x : e->y) = std::round(at);
        at += (h ? e->w : e->h) + gap;
      }
      return;
    }
    for (doc::PanelElement* e : sel) {
      switch (a) {
        case AlignTo::Left: e->x = x0; break;
        case AlignTo::Centre: e->x = std::round((x0 + x1 - e->w) * 0.5f); break;
        case AlignTo::Right: e->x = x1 - e->w; break;
        case AlignTo::Top: e->y = y0; break;
        case AlignTo::Middle: e->y = std::round((y0 + y1 - e->h) * 0.5f); break;
        case AlignTo::Bottom: e->y = y1 - e->h; break;
        default: break;
      }
    }
  }, "Align Panel Elements");
}

}  // namespace pq::views::panel
