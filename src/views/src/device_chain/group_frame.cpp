// TX.groups (SPEC §15 2026-11-01 "resize, groups, and the free-device rack"; doc model.h "Groups"): a group in the Device
// view is one device of the chain whose panel is a mini circuit of its contents — the project patch canvas itself,
// embedded and scoped to the group (project_patch::ProjectPatchView(ctx, group)): zoom, pan, wire, edit faces and
// panels in place, S-resize blocks, Ctrl+G / Delete inside, exactly as in the circuit. The frame's title bar moves
// and selects the group as any device's; its right edge drags the panel wider or narrower (view state of the session).
#include <algorithm>
#include <cmath>
#include <string>

#include "../common/paint.h"
#include "internal.h"
#include "pq/ui/theme.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::device_chain {

namespace {

constexpr float kDefaultW = 420.0f, kMinW = 220.0f, kMaxW = 1600.0f;
constexpr float kEdgeGrab = 5.0f;  // logical px at the right edge that resize the panel

class GroupFrame final : public FrameBase {
public:
  GroupFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d)
      : FrameBase("group[" + std::to_string(d.id.value) + "]", impl, ctx, d.id), name_(d.name) {
    setLayout(ui::LayoutKind::Custom);
    view_ = &add<project_patch::ProjectPatchView>(ctx, d.id);
    view_->faceVoices = impl.view.faceVoices;  // the same voice taps as the main circuit and the panels
    active_ = d.active;
    members_ = countMembers();
  }

  float logicalWidth() const override {
    const auto it = impl_.groupWidths.find(id_.value);
    return it != impl_.groupWidths.end() ? it->second : kDefaultW;
  }
  float logicalHeight(float available) const override { return available; }
  void update(const doc::Device& d) override {
    name_ = d.name;
    active_ = d.active;
    members_ = countMembers();
    markDirty();
  }
  void paramsChanged(const doc::Device&, std::span<const doc::ParamRef>) override {}  // the canvas follows the document

  project_patch::ProjectPatchView& patchView() const noexcept { return *view_; }

  ui::InfoText info() const override {
    return {"Group",
            "A group: its devices placed freely between its In and Out, wired as drawn (no chain order inside). The panel is "
            "the circuit of its contents: Ctrl+wheel zooms, the wheel pans, drag outputs onto inputs or knobs to wire them, "
            "S-drag resizes a block. Drag the title bar to move the group, its right edge to resize the panel. "
            "Ctrl+Shift+G in the circuit ungroups it."};
  }

  void arrange(ui::LayoutPass& pass) override {
    const ui::IRect r = rect();
    const int32_t th = tree() ? tree()->uiScale().px(kTitleH) : 20;
    pass.place(*view_, {r.x + 1, r.y + th, std::max(0, r.w - 2), std::max(0, r.h - th - 1)});
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* t = ctx.res.theme;
    if (t == nullptr) return;
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, 0.0f, t->surface.raised, t->surface.divider);
    const int32_t th = ctx.scale.px(kTitleH);
    const ui::IRect title{r.x, r.y, r.w, th};
    ctx.dl.addRectFilled(title.toRect(), active_ ? ui::lerp(t->surface.raised, t->circuit.wireAudio, 0.25f) : t->surface.sunken);
    if (ctx.res.uiFonts == nullptr) return;
    const ui::IRect text = paint::inset(title, ctx.scale.px(6.0f));
    const std::string label = (name_.empty() ? std::string("Group") : name_) + "  ·  " + std::to_string(members_) +
                              (members_ == 1 ? " device" : " devices");
    paint::text(ctx, ctx.res.uiFonts->small, text, label, active_ ? t->text.primary : t->text.secondary, ui::TextAlign::Left);
  }

  bool onPointer(const ui::PointerEvent& e) override {
    const float us = tree() ? tree()->scale() : 1.0f;
    const bool atEdge = e.pos.x >= static_cast<float>(rect().right()) - kEdgeGrab * us && rect().contains(e.pos);
    if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left && atEdge) {
      resizing_ = true;
      pressX_ = e.pos.x;
      w0_ = logicalWidth();
      return true;
    }
    if (resizing_) {
      if (e.type == ui::PointerType::Move) {
        impl_.groupWidths[id_.value] = std::clamp(w0_ + (e.pos.x - pressX_) / us, kMinW, kMaxW);
        impl_.content->invalidateLayout();
        markDirty();
        return true;
      }
      if (e.type == ui::PointerType::Up) {
        resizing_ = false;
        return true;
      }
    }
    return FrameBase::onPointer(e);
  }
  void onCaptureLost() override {
    resizing_ = false;
    FrameBase::onCaptureLost();
  }
  ui::Cursor cursorAt(ui::Vec2 p) const override {
    const float us = tree() ? tree()->scale() : 1.0f;
    if (resizing_ || p.x >= static_cast<float>(rect().right()) - kEdgeGrab * us) return ui::Cursor::ResizeEW;
    return FrameBase::cursorAt(p);
  }

private:
  uint32_t countMembers() const {
    const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), id_);
    return l.track != nullptr ? static_cast<uint32_t>(doc::groupMembers(l.track->devices, id_).size()) : 0u;
  }

  project_patch::ProjectPatchView* view_ = nullptr;
  std::string name_;
  bool active_ = true;
  uint32_t members_ = 0;
  bool resizing_ = false;
  float pressX_ = 0.0f, w0_ = 0.0f;
};

}  // namespace

std::unique_ptr<FrameBase> makeGroupFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d) {
  return std::make_unique<GroupFrame>(impl, ctx, d);
}

project_patch::ProjectPatchView* groupPatchView(FrameBase& f) {
  auto* g = dynamic_cast<GroupFrame*>(&f);
  return g != nullptr ? &g->patchView() : nullptr;
}

}  // namespace pq::views::device_chain
