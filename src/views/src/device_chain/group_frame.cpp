// TX.groups (SPEC §15 2026-11-01 "resize, groups, and the free-device rack"; doc model.h "Groups"): a group in the Device
// view is one device of the chain whose panel is a mini circuit of its contents — the project patch canvas itself,
// embedded and scoped to the group (project_patch::ProjectPatchView(ctx, group)): zoom, pan, wire, edit faces and
// panels in place, S-resize blocks, Ctrl+G / Delete inside, exactly as in the circuit. The frame's title bar moves
// and selects the group as any device's; its right edge drags the panel wider or narrower (view state of the session).
// GU.3 (docs/plans/GU.md "Decided"): a group whose PANEL has elements shows its panel instead (pq/views/panel/
// group_panel_face.h: the same face the circuit draws for the closed group, fitted to the frame's height, edited in
// place); an empty panel keeps the mini circuit. An INSTRUMENT GROUP shows its INSPECTOR on the left: its polyphony
// (voices, mono, legato, glide, steal) as panel elements bound to the group's settings (an edit: one undo step, one
// recompile) — what IG.4's group panel was to be.
#include <algorithm>
#include <cmath>
#include <string>

#include "../common/paint.h"
#include "internal.h"
#include "pq/document/racks.h"
#include "pq/document/instrument_groups.h"
#include "pq/ui/theme.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::device_chain {

namespace {

constexpr float kDefaultW = 420.0f, kMinW = 220.0f, kMaxW = 1600.0f;
constexpr float kEdgeGrab = 5.0f;  // logical px at the right edge that resize the panel
constexpr float kInspectorW = 150.0f;  // GU.3: an instrument group's inspector column

class GroupFrame final : public FrameBase {
public:
  GroupFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d)
      : FrameBase("group[" + std::to_string(d.id.value) + "]", impl, ctx, d.id), name_(d.name) {
    setLayout(ui::LayoutKind::Custom);
    hasPanel_ = d.hasPanel();
    instrument_ = doc::isInstrumentGroup(*ctx.doc(), d.id);
    if (instrument_) {  // GU.3: the inspector (a generated panel of the group's own settings)
      auto face = std::make_unique<panel::GroupPanelFace>(
          [p = std::make_shared<const doc::Panel>(panel::inspectorPanel(d.id, kInspectorW))]() -> doc::PanelPtr { return p; },
          panel::makeDocPanelTarget(ctx, d.id, [this]() -> ui::Widget* { return inspector_; }));
      inspector_ = &add<panel::PanelHost>("inspector[" + std::to_string(d.id.value) + "]", std::move(face));
    }
    if (hasPanel_) {
      auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, d.id),
                                                          panel::makeDocPanelTarget(ctx, d.id, [this]() -> ui::Widget* { return panel_; }));
      panel_ = &add<panel::PanelHost>("panel[" + std::to_string(d.id.value) + "]", std::move(face));
    } else {
      view_ = &add<project_patch::ProjectPatchView>(ctx, d.id);
      view_->faceVoices = impl.view.faceVoices;  // the same voice taps as the main circuit and the panels
    }
    active_ = d.active;
    members_ = countMembers();
    if (view_ != nullptr) view_->sync();  // TMM.13-fix2: its contents' extent sizes the panel (logicalWidth)
    // The panels follow the document (a value from elsewhere, undo): repainted on every change.
    if (panel_ != nullptr || inspector_ != nullptr)
      sub_ = ctx.subscribe([this](const doc::StoreEvent&) {
        if (panel_ != nullptr) panel_->notifyChanged();
        if (inspector_ != nullptr) inspector_->notifyChanged();
      });
  }

  // TMM.13-fix2 (MM review "UI and aesthetic" 9): by default the panel is as wide as its contents at the zoom that fills its
  // height (at most 1: a face at its own size), so the mini circuit opens legible instead of letterboxed at ~0.35. A width
  // dragged this session wins.
  float logicalWidth() const override { return logicalWidthFor(0.0f); }
  float logicalWidthFor(float available) const override {
    const auto it = impl_.groupWidths.find(id_.value);
    if (it != impl_.groupWidths.end()) return it->second;
    const float side = inspector_ != nullptr ? kInspectorW : 0.0f;
    if (panel_ != nullptr) {  // GU.3: the panel at the scale that fills the height (at most 1), the inspector beside it
      const ui::Vec2 ws = panel_->face().worldSize();
      const float h = available > 0.0f ? available - kTitleH - 1.0f : ws.y;
      const float z = std::min(1.0f, h / std::max(1.0f, ws.y));
      return std::clamp(std::ceil(ws.x * z) + side + 2.0f, kMinW, kMaxW);
    }
    if (view_ == nullptr) return side + kMinW;
    const project_patch::ProjectPatch& pp = view_->patch();
    if (pp.nodes.empty() || !(available > 0.0f)) return kDefaultW + side;
    float x0 = pp.nodes[0].x, y0 = pp.nodes[0].y, x1 = x0 + pp.nodes[0].w, y1 = y0 + pp.nodes[0].h;
    for (const project_patch::Node& n : pp.nodes)
      x0 = std::min(x0, n.x), y0 = std::min(y0, n.y - 12.0f), x1 = std::max(x1, n.x + n.w), y1 = std::max(y1, n.y + n.h);
    constexpr float kMargin = 24.0f;  // CircuitCanvas's fit margin (world units)
    const float h = available - kTitleH - 1.0f;
    const float z = std::min(1.0f, h / std::max(1.0f, (y1 - y0) + 2.0f * kMargin));
    return std::clamp(std::ceil(((x1 - x0) + 2.0f * kMargin) * z) + 2.0f + side, kMinW, kMaxW);
  }
  float logicalHeight(float available) const override { return available; }
  void update(const doc::Device& d) override {
    name_ = d.name;
    active_ = d.active;
    members_ = countMembers();
    markDirty();
  }
  // GU.3: a panel placed or emptied, or the group becoming or ceasing to be an instrument: a new frame.
  bool keeps(const doc::Device& d) const override {
    return d.hasPanel() == hasPanel_ && doc::isInstrumentGroup(*ctx_.doc(), d.id) == instrument_ && !rackChain(d);  // GU.9: a group wired in parallel with others is a rack's chain
  }
  void paramsChanged(const doc::Device&, std::span<const doc::ParamRef>) override {}  // the canvas follows the document
  bool rackChain(const doc::Device& d) const {
    const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), d.id);
    return l.track != nullptr && doc::rackOfChain(*ctx_.doc(), *l.track, d.id).has_value();
  }

  project_patch::ProjectPatchView* patchView() const noexcept { return view_; }  // null: the group shows its panel
  panel::PanelHost* panelHost() const noexcept { return panel_; }
  panel::PanelHost* inspectorHost() const noexcept { return inspector_; }

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
    const int32_t side = inspector_ != nullptr ? (tree() ? tree()->uiScale().px(kInspectorW) : static_cast<int32_t>(kInspectorW)) : 0;
    const ui::IRect body{r.x + 1, r.y + th, std::max(0, r.w - 2), std::max(0, r.h - th - 1)};
    if (inspector_ != nullptr) pass.place(*inspector_, {body.x, body.y, std::min(side, body.w), body.h});
    const ui::IRect rest{body.x + std::min(side, body.w), body.y, std::max(0, body.w - side), body.h};
    if (panel_ != nullptr) pass.place(*panel_, rest);
    if (view_ != nullptr) pass.place(*view_, rest);
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
      w0_ = static_cast<float>(rect().w) / us;  // the width shown (fitted to the contents by default)
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
  panel::PanelHost* panel_ = nullptr;      // GU.3: the group's panel (when it has elements)
  panel::PanelHost* inspector_ = nullptr;  // GU.3: an instrument group's inspector
  bool hasPanel_ = false, instrument_ = false;
  ViewContext::Subscription sub_;
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
  return g != nullptr ? g->patchView() : nullptr;
}

panel::PanelHost* groupPanelHost(FrameBase& f, bool inspector) {
  auto* g = dynamic_cast<GroupFrame*>(&f);
  return g == nullptr ? nullptr : inspector ? g->inspectorHost() : g->panelHost();
}

void forEachPanelControl(FrameBase& f, const std::function<bool(const PanelControl&)>& fn) {
  panel::PanelHost* host = groupPanelHost(f, false);
  if (host == nullptr || !host->visible() || host->rect().empty()) return;
  auto* face = dynamic_cast<panel::GroupPanelFace*>(&host->face());
  if (face == nullptr) return;
  const ui::FaceTransform t = host->transform();
  for (size_t i = 0; i < face->elementCount(); ++i) {
    const doc::Binding* b = face->element(i).binding();
    if (b == nullptr || b->kind != doc::BindingKind::Control || !b->meta.empty() || !face->shown(i)) continue;
    const ui::Rect er = face->element(i).rect();
    const ui::Vec2 a = t.map({er.x0, er.y0}), z = t.map({er.x1, er.y1});
    const ui::IRect cr{static_cast<int32_t>(std::floor(a.x)), static_cast<int32_t>(std::floor(a.y)),
                       static_cast<int32_t>(std::ceil(z.x) - std::floor(a.x)), static_cast<int32_t>(std::ceil(z.y) - std::floor(a.y))};
    const ui::IRect r = ui::intersect(cr, host->rect());
    if (r.empty()) continue;
    if (fn(PanelControl{b->device, b->name, r})) return;
  }
}

std::optional<ui::Vec2> panelControlPoint(FrameBase& f, doc::DeviceId device, std::string_view param) {
  panel::PanelHost* host = groupPanelHost(f, false);
  if (host == nullptr || !host->visible() || host->rect().empty()) return std::nullopt;
  auto* face = dynamic_cast<panel::GroupPanelFace*>(&host->face());
  if (face == nullptr) return std::nullopt;
  const std::optional<ControlAnchor> a = face->bindingAnchor(doc::controlBinding(device, std::string(param)), true);
  if (!a) return std::nullopt;
  return host->transform().map({a->at.x, a->at.y - 0.5f * a->radius});
}

}  // namespace pq::views::device_chain
