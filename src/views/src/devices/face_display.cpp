// A meta module's face as a device-panel control (TMM.10, docs/plans/MM.md "Chain and devices"; SPEC §15 2026-11-01:
// a meta module placed directly in a track's chain is a device whose panel IS its face). The same vector face the
// circuit view hosts (pq/views/meta/meta_face.h), fitted into the control's rect (aspect kept, centred, whole device
// px at its origin so its tiles match), fully interactive: pointer events go to the face in face-local world units,
// drags are measured in device px as in the circuit view (Q169 1), keys go to it while it holds the focus (a
// keyframe's Delete), its info text is the face's own.
//
// Painting: the static layers come from coverage tiles of the renderer's tile atlas (built in onFrame at the exact
// scale, keyed by layer and the face's per-instance key; drawn directly until they exist and while the face drags a
// layer that depends on its value), the live layer is geometry each frame. Voices: polled at ≤ 60 Hz only while the
// face's reader is live or the face animates (idle: no timer, no frames); wake() restarts the poll.
//
// UI thread only.

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "panel_internal.h"
#include "pq/meta/registry.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/theme.h"
#include "pq/views/meta/meta_face.h"

namespace pq::views::devices {

namespace {

constexpr double kFaceFrameSeconds = 1.0 / 60.0;

class FaceDisplay final : public PanelDisplay {
public:
  FaceDisplay(DevicePanel::Impl& impl, const ControlDesc& desc) : PanelDisplay(desc.id, impl, desc) {
    setFocusable(true);
    DeviceTarget* t = impl.target();
    type_ = t != nullptr ? t->metaType(desc.meta) : nullptr;
    // A chain meta device's own face: its patch is an expansion of the type (meta_<type>__…).
    if (type_ == nullptr && desc.meta.empty()) type_ = meta::typeOfCall(impl.panel.signature().patch);
    if (type_ == nullptr) return;
    face_ = makeFace(*type_);
    if (!face_) return;
    face_->setPopupOverFace(true);  // TX.orbit-ports: a panel clips to its body: a face's floating panel opens over it
    std::unique_ptr<MetaTarget> target = t != nullptr ? t->metaTarget(desc.meta, *type_) : nullptr;
    if (!target) target = std::make_unique<DefaultTarget>(*type_);
    face_->bind(std::move(target), t != nullptr ? t->metaVoices(desc.meta) : nullptr);
    // TX.spectrum: a chain device's own face that shows its signal's analysis reads the device's feed.
    if (desc.meta.empty() && face_->wantsAnalysis())
      face_->setAnalysis([this]() -> SpectrumFeed* { return impl_.target() != nullptr ? impl_.target()->outputSpectrum() : nullptr; },
                         [this] { wake(); });
    // A chain meta device's own face shows where its signal goes: the output socket on the right edge, and the voice
    // input on the left when it continues a run (its patch has an audio `in`). Nested faces show no sockets here.
    if (desc.meta.empty()) {
      const ui::Vec2 ws = face_->worldSize();
      if (!impl.panel.signature().audioIn.empty())
        jacks_.push_back(FaceJack{{0.0f, face_->inputY(0, 1)}, true, true, true, impl.panel.signature().audioIn});
      jacks_.push_back(FaceJack{{ws.x, face_->outputY(0, 1)}, false, true, true, "out"});
    }
  }

  MetaFace* face() const noexcept { return face_.get(); }

  bool dependsOn(std::string_view) const override { return true; }  // any value: a repaint (the face reads its target)

  // ---- geometry: the face fitted into the rect ----
  ui::FaceTransform transform() const noexcept {
    const ui::IRect r = rect();
    if (!face_) return {};
    const ui::Vec2 ws = face_->worldSize();
    const float s = std::max(1e-3f, std::min(static_cast<float>(r.w) / ws.x, static_cast<float>(r.h) / ws.y));
    return {{std::round(static_cast<float>(r.x) + (static_cast<float>(r.w) - ws.x * s) * 0.5f),
             std::round(static_cast<float>(r.y) + (static_cast<float>(r.h) - ws.y * s) * 0.5f)},
            s};
  }

  std::optional<ui::IRect> paramRect(std::string_view param) const override {
    if (!face_ || !visible()) return std::nullopt;
    const int32_t i = type_->paramIndex(param);
    if (i < 0) return std::nullopt;
    const std::optional<ui::Rect> lr = face_->paramRect(static_cast<size_t>(i));
    if (!lr) return std::nullopt;
    const ui::FaceTransform t = transform();
    const ui::Vec2 a = t.map({lr->x0, lr->y0}), b = t.map({lr->x1, lr->y1});
    const int32_t x0 = static_cast<int32_t>(std::floor(a.x)), y0 = static_cast<int32_t>(std::floor(a.y));
    return ui::IRect{x0, y0, std::max(1, static_cast<int32_t>(std::ceil(b.x)) - x0),
                     std::max(1, static_cast<int32_t>(std::ceil(b.y)) - y0)};
  }

  void wake() override {
    if (face_ && face_->voices() != nullptr && timer_ == 0 && tree() != nullptr) timer_ = requestTimer(kFaceFrameSeconds);
  }

  // ---- painting ----
  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    if (!face_) {
      ctx.dl.addRectFilled(rect().toRect(), th->surface.sunken);
      return;
    }
    const ui::FaceTransform tr = transform();
    ui::TileAtlas* atlas = ctx.res.tiles;
    bool missing = false;
    if (!face_->ownsPlate() && desc_.meta.empty()) {
      // TMM.13-fix2: a chain device's face without its own plate (the Phase accumulator) gets the family's rounded body.
      const ui::Vec2 ws = face_->worldSize();
      const ui::Vec2 a = tr.map({1.0f, 1.0f}), b = tr.map({ws.x - 1.0f, ws.y - 1.0f});
      ctx.dl.addRoundedRectFilled({a.x, a.y, b.x, b.y}, 10.0f * tr.scale, th->face.body);
      ctx.dl.addRoundedRectStroke({a.x, a.y, b.x, b.y}, 10.0f * tr.scale, th->face.frame, std::max(1.0f, 1.25f * tr.scale));
    }
    for (uint32_t layer = 0; layer < face_->staticLayerCount(); ++layer) {
      const ui::Rgba col = face_->staticLayerColour(*th, layer);
      // A layer that depends on the instance's values is drawn directly while a drag changes it every step.
      const bool volatileLayer = face_->dragging() && face_->staticKey(layer) != 0;
      bool drawn = false;
      if (atlas != nullptr && !volatileLayer) {
        const auto it = tiles_.find(keyOf(layer));
        if (it != tiles_.end() && ui::layerReady(*atlas, it->second) && it->second.scale == tr.scale) {
          ui::drawLayer(ctx.dl, *atlas, it->second, tr.origin, tr.scale, col);
          drawn = true;
        } else if (!(it != tiles_.end() && it->second.failed && it->second.scale == tr.scale)) {
          missing = true;
        }
      }
      if (!drawn) {
        ui::FacePainter p(ctx.dl, tr);
        p.setTint(col);
        face_->paintStatic(p, layer);
      }
    }
    FaceEnv env;
    env.theme = th;
    env.uiScale = ctx.scale.factor;
    env.hovered = hovered();
    env.jacks = jacks_;
    ui::FacePainter p(ctx.dl, tr);
    p.setText(ctx.res.text);
    p.setImages(ctx.res.images);  // TX.spectrogram: a face with its own texture
    face_->paintJacks(p, env);
    face_->paintLive(p, env);
    face_->paintPopup(p, env);  // TX.orbit-ports: its floating panel (over the face here), when open
    if (missing) {
      tilesWanted_ = true;
      requestFrame();
    }
    if (timer_ == 0 && face_->voices() != nullptr && (face_->voices()->live() || face_->animating()))
      timer_ = requestTimer(kFaceFrameSeconds);
  }

  void onFrame(double) override {
    if (!tilesWanted_ || !face_ || tree() == nullptr || tree()->resources() == nullptr ||
        tree()->resources()->tiles == nullptr)
      return;
    tilesWanted_ = false;
    ui::TileAtlas& atlas = *tree()->resources()->tiles;
    const ui::FaceTransform tr = transform();
    const ui::Vec2 ws = face_->worldSize();
    bool built = false;
    for (uint32_t layer = 0; layer < face_->staticLayerCount(); ++layer) {
      if (face_->dragging() && face_->staticKey(layer) != 0) continue;
      ui::LayerTile& t = tiles_[keyOf(layer)];
      if (t.scale == tr.scale && (ui::layerReady(atlas, t) || t.failed)) continue;
      ui::LayerTile fresh;
      (void)ui::recordLayer(atlas, scratch_, ws.x, ws.y, tr.scale, [&](ui::FacePainter& p) { face_->paintStatic(p, layer); },
                            fresh);
      t = fresh;
      built = true;
    }
    // Instance keys change with the data: drop tiles nothing uses any more once the map has grown.
    if (tiles_.size() > 16) {
      std::map<uint64_t, ui::LayerTile> keep;
      for (uint32_t layer = 0; layer < face_->staticLayerCount(); ++layer)
        if (const auto it = tiles_.find(keyOf(layer)); it != tiles_.end()) keep.emplace(it->first, it->second);
      tiles_.swap(keep);
    }
    if (built) markDirty();
  }

  void onTimer(uint32_t id) override {
    if (id != timer_) return;
    timer_ = 0;
    if (!face_ || face_->voices() == nullptr || !shownInTree(*this)) return;
    if (face_->pollVoices()) markDirty();
    if (face_->voices()->live() || face_->animating()) timer_ = requestTimer(kFaceFrameSeconds);
  }

  // ---- interaction ----
  bool onPointer(const ui::PointerEvent& e) override {
    if (!face_) return false;
    using K = FacePointer::Kind;
    switch (e.type) {
      case ui::PointerType::Down: {
        // Left presses, and right presses a face uses (harmonic bars); anything else goes to the device frame.
        if (e.button != ui::PointerButton::Left && e.button != ui::PointerButton::Right) return false;
        const FacePointer fp = pointerOf(e, K::Down);
        if (face_->hitTest(fp.local, fp.pxPerUnit) < 0) {
          if (face_->popupRect()) {  // TX.orbit-ports: a press elsewhere closes the floating panel
            face_->focusLost();
            markDirty();
          }
          return false;
        }
        if (!face_->onPointer(fp)) return false;
        if (tree()) tree()->setFocus(this);
        markDirty();
        return true;  // captures the pointer until the buttons are up
      }
      case ui::PointerType::Move: {
        const FacePointer fp = pointerOf(e, K::Move);
        if (captured()) {
          (void)face_->onPointer(fp);
          markDirty();
          return true;
        }
        bool dirty = face_->setHoveredPart(face_->hitTest(fp.local, fp.pxPerUnit));
        dirty = face_->onPointer(fp) || dirty;  // the face's own hover (a bar, a draw cursor)
        if (dirty) markDirty();
        return false;
      }
      case ui::PointerType::Up: {
        if (!captured()) return false;
        (void)face_->onPointer(pointerOf(e, K::Up));
        markDirty();
        return true;
      }
      case ui::PointerType::Wheel: {
        if (!face_->onPointer(pointerOf(e, K::Wheel))) return false;
        markDirty();
        return true;
      }
      default: return false;
    }
  }

  void onFocusChanged(bool focused) override {
    if (!focused && face_ && face_->popupRect()) {  // TX.orbit-ports: the key focus went elsewhere: the panel closes
      face_->focusLost();
      markDirty();
    }
  }

  void onHoverChanged(bool hovered) override {
    if (!hovered && face_ && face_->setHoveredPart(MetaFace::kNone)) markDirty();
  }

  bool onKey(const ui::KeyEvent& e) override {
    if (!face_ || face_->dragging()) {
      if (face_ && e.down && e.key == ui::key::kEscape) {
        face_->cancelDrag();
        markDirty();
        return true;
      }
      return false;
    }
    if (!face_->onKey(e)) return false;
    markDirty();
    return true;
  }

  void onCaptureLost() override { cancel(); }
  void onInteractionCancelled() override { cancel(); }

  ui::Cursor cursorAt(ui::Vec2 pos) const override {
    if (!face_) return ui::Cursor::Inherit;
    if (captured()) return face_->cursor(face_->hoveredPart() >= 0 ? face_->hoveredPart() : 0);
    const ui::FaceTransform t = transform();
    return face_->cursor(face_->hitTest(t.unmap(pos), t.scale));
  }

  ui::InfoText info() const override {
    if (!face_) return {"Meta module", "This meta module's type has no face."};
    const int32_t part = face_->hoveredPart();
    face_->describe(part == MetaFace::kNone ? MetaFace::kFrame : part, title_, body_);
    return {title_, body_};
  }

private:
  uint64_t keyOf(uint32_t layer) const { return (face_->staticKey(layer) * 0x9E3779B97F4A7C15ull) ^ layer; }

  FacePointer pointerOf(const ui::PointerEvent& e, FacePointer::Kind kind) const {
    const ui::FaceTransform t = transform();
    FacePointer fp;
    fp.kind = kind;
    fp.local = t.unmap(e.pos);
    fp.device = e.pos;
    fp.pxPerUnit = t.scale;
    fp.uiScale = tree() ? tree()->scale() : 1.0f;
    fp.mods = e.mods;
    fp.clicks = e.clickCount;
    fp.wheel = e.wheel;
    if (e.button != ui::PointerButton::None) fp.button = e.button;
    return fp;
  }

  void cancel() {
    if (face_ && face_->dragging()) {
      face_->cancelDrag();
      markDirty();
    }
  }

  const meta::Type* type_ = nullptr;
  std::unique_ptr<MetaFace> face_;
  std::vector<FaceJack> jacks_;
  std::map<uint64_t, ui::LayerTile> tiles_;
  ui::DrawList scratch_;
  bool tilesWanted_ = false;
  uint32_t timer_ = 0;

  mutable std::string title_, body_;
};

}  // namespace

std::unique_ptr<PanelDisplay> makeFaceDisplay(DevicePanel::Impl& impl, const ControlDesc& desc) {
  return std::make_unique<FaceDisplay>(impl, desc);
}

}  // namespace pq::views::devices
