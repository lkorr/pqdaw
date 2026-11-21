// TX.groups (SPEC §15 2026-11-01 "resize, groups, and the free-device rack"; doc model.h "Groups"): a group in the Device
// view is one device of the chain whose panel is a mini circuit of its contents — the project patch canvas itself,
// embedded and scoped to the group (project_patch::ProjectPatchView(ctx, group)): zoom, pan, wire, edit faces and
// panels in place, S-resize blocks, Ctrl+G / Delete inside, exactly as in the circuit. The frame's title bar moves
// and selects the group as any device's; its right edge drags the panel wider or narrower (view state of the session).
// GU.3 (docs/plans/GU.md "Decided"): a group whose PANEL has elements shows its panel instead (pq/views/panel/
// group_panel_face.h: the same face the circuit draws for the closed group, at 1:1 (BR.I), edited in
// place); an empty panel keeps the mini circuit. The INSPECTOR on the left: an instrument group's polyphony (voices,
// mono, legato, glide, steal) and (TX.group-oversample) the group's Oversample menu as panel elements bound to the
// group's settings (an edit: one undo step, one recompile) — what IG.4's group panel was to be. BR.A: only an
// instrument group or a group without a panel (the mini circuit) shows it; a group that shows its own panel (a module:
// EQ, Saturator, Utility …) is its panel alone — its oversampling is its panel's menu where the panel places one, and
// every group's title-bar context menu (Oversample ▸). The title counts the group's DEVICES (members that are not atom
// or value blocks: a module's internals are not devices) and shows no count for a module.
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "../common/paint.h"
#include "internal.h"
#include "pq/document/edit_ops.h"
#include "pq/document/panel.h"
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
constexpr float kPlainInspectorW = 96.0f;  // TX.group-oversample: any other group's (its Oversample menu)

class GroupFrame final : public FrameBase {
public:
  GroupFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d)
      : FrameBase("group[" + std::to_string(d.id.value) + "]", impl, ctx, d.id), name_(d.name) {
    setLayout(ui::LayoutKind::Custom);
    hasPanel_ = d.hasPanel();
    instrument_ = doc::isInstrumentGroup(*ctx.doc(), d.id);
    if (instrument_ || !hasPanel_) {  // GU.3: the inspector (a generated panel of the group's own settings)
      auto face = std::make_unique<panel::GroupPanelFace>(
          [p = std::make_shared<const doc::Panel>(panel::inspectorPanel(d.id, inspectorW(), instrument_))]() -> doc::PanelPtr { return p; },
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

  // TMM.13-fix2 (MM review "UI and aesthetic" 9) / BR.I: a panel is as wide as it is at 1:1 (the chain is as tall as it
  // needs and scrolls); the mini circuit — a zoomable canvas, not a panel (Q?-BR.I-a) — as wide as its contents at the
  // zoom that fills the frame's height (at most 1). A width dragged this session wins.
  float logicalWidth() const override { return logicalWidthFor(0.0f); }
  float logicalWidthFor(float available) const override {
    const auto it = impl_.groupWidths.find(id_.value);
    if (it != impl_.groupWidths.end()) return it->second;
    const float side = inspector_ != nullptr ? inspectorW() : 0.0f;
    if (panel_ != nullptr)  // GU.3: the panel, the inspector beside it
      return std::clamp(std::ceil(panel_->face().worldSize().x) + side + 2.0f, kMinW, kMaxW);
    if (view_ == nullptr) return side + kMinW;
    const std::optional<ui::Rect> b = circuitBounds();
    if (!b || !(available > 0.0f)) return kDefaultW + side;
    const float z = std::min(1.0f, (available - kTitleH - 1.0f) / std::max(1.0f, b->y1 - b->y0));
    return std::clamp(std::ceil((b->x1 - b->x0) * z) + 2.0f + side, kMinW, kMaxW);
  }
  // BR.I: what its panels need at 1:1 (its panel, its inspector); a mini circuit needs nothing (it fits).
  float naturalHeight() const override {
    float h = panel_ != nullptr ? panel_->face().worldSize().y : 0.0f;
    if (inspector_ != nullptr) h = std::max(h, inspector_->face().worldSize().y);
    return h > 0.0f ? kTitleH + 1.0f + std::ceil(h) : 0.0f;
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

  // GU.13: its panel and its inspector are the frame's panel hosts (their controls: drop targets, LEDs, rings, map mode).
  void hosts(std::vector<panel::PanelHost*>& out) const override {
    if (panel_ != nullptr && panel_->visible()) out.push_back(panel_);
    if (inspector_ != nullptr && inspector_->visible()) out.push_back(inspector_);
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
    const int32_t side = inspector_ != nullptr ? (tree() ? tree()->uiScale().px(inspectorW()) : static_cast<int32_t>(inspectorW())) : 0;
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
    std::string label = name_.empty() ? std::string("Group") : name_;
    if (!hasPanel_ && members_ > 0) label += "  ·  " + std::to_string(members_) + (members_ == 1 ? " device" : " devices");
    paint::text(ctx, ctx.res.uiFonts->small, text, label, active_ ? t->text.primary : t->text.secondary, ui::TextAlign::Left);
  }

  // BR.A: every group's oversampling from its title bar's menu (the inspector shows it only where there is one).
  void contextItems(ui::Vec2 /*p*/, std::vector<ui::MenuItem>& items) override {
    const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), id_);
    if (l.device == nullptr) return;
    const doc::SettingSpec& spec = doc::groupOversampleSetting();
    const uint32_t factor = doc::groupOversampleFactor(*l.device);
    std::vector<ui::MenuItem> sub;
    DeviceChainView::Impl* im = &impl_;
    ViewContext* c = &ctx_;
    const doc::DeviceId id = id_;
    for (size_t i = 0; i < spec.choices.size(); ++i) {
      ui::MenuItem it = ui::MenuItem::item(std::string(spec.choices[i]), [im, c, id, i] {
        (void)im->run(doc::ops::setDeviceSetting(*c->doc(), id, std::string(doc::kGroupOversample),
                                                 i == 0 ? std::nullopt : std::optional<double>(static_cast<double>(i))),
                      false);
      });
      it.checked = (1u << i) == factor;
      sub.push_back(std::move(it));
    }
    items.push_back(ui::MenuItem::sub("Oversample", std::move(sub)));
    items.push_back(ui::MenuItem::sep());
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
  // BR.A: the members a user calls devices (atom and value blocks are a module's or a circuit's internals).
  uint32_t countMembers() const {
    const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), id_);
    if (l.track == nullptr) return 0u;
    uint32_t n = 0;
    for (const doc::Device* m : doc::groupMembers(l.track->devices, id_)) n += doc::isAtomDevice(*m) || doc::isValueDevice(*m) ? 0u : 1u;
    return n;
  }

  project_patch::ProjectPatchView* view_ = nullptr;
  panel::PanelHost* panel_ = nullptr;      // GU.3: the group's panel (when it has elements)
  panel::PanelHost* inspector_ = nullptr;  // GU.3: the group's inspector (its polyphony when an instrument group, its oversampling)
  float inspectorW() const noexcept { return instrument_ ? kInspectorW : kPlainInspectorW; }
  // The mini circuit's contents at zoom 1 with CircuitCanvas's fit margin (world units); nullopt: empty.
  std::optional<ui::Rect> circuitBounds() const {
    if (view_ == nullptr) return std::nullopt;
    const project_patch::ProjectPatch& pp = view_->patch();
    if (pp.nodes.empty()) return std::nullopt;
    float x0 = pp.nodes[0].x, y0 = pp.nodes[0].y, x1 = x0 + pp.nodes[0].w, y1 = y0 + pp.nodes[0].h;
    for (const project_patch::Node& n : pp.nodes)
      x0 = std::min(x0, n.x), y0 = std::min(y0, n.y - 12.0f), x1 = std::max(x1, n.x + n.w), y1 = std::max(y1, n.y + n.h);
    constexpr float kMargin = 24.0f;  // CircuitCanvas's fit margin (world units)
    return ui::Rect{x0 - kMargin, y0 - kMargin, x1 + kMargin, y1 + kMargin};
  }
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

}  // namespace pq::views::device_chain
