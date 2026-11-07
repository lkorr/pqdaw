// GU.9 (docs/plans/GU.md "One hierarchy"; doc model.h "Racks"): a rack in the Device view. A rack is a group whose chains
// are child groups (doc::rackChains: fed from its In); the circuit shows it as the group it is, the Device view as Live's
// rack: its CHAIN LIST on the left (a row per chain, the shown one lit; "+ Chain" adds one: doc::ops::addChain), its PANEL
// (the macros: value blocks of the rack bound to knobs, GU.3's PanelHost, edited in place: a drag is a param write) and
// the SHOWN CHAIN's contents (the project patch canvas scoped to that chain group, as a group frame shows a group).
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../common/paint.h"
#include "internal.h"
#include "pq/document/instrument_groups.h"
#include "pq/ui/theme.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::device_chain {

namespace {

constexpr float kListW = 132.0f;   // the chain list
constexpr float kRowH = 20.0f;     // a chain row (and the "+ Chain" row)
constexpr float kChainW = 360.0f;  // the shown chain's canvas
constexpr float kMinPanelW = 120.0f;

const doc::Track* trackOf(const doc::Project& p, doc::DeviceId d) { return doc::findDevice(p, d).track; }

std::vector<doc::DeviceId> chainsOf(const doc::Project& p, doc::DeviceId rack) {
  std::vector<doc::DeviceId> out;
  if (const doc::Track* t = trackOf(p, rack))
    for (const doc::Device* c : doc::rackChains(p, *t, rack)) out.push_back(c->id);
  return out;
}

class RackFrame final : public FrameBase {
public:
  RackFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d)
      : FrameBase("rack[" + std::to_string(d.id.value) + "]", impl, ctx, d.id) {
    setLayout(ui::LayoutKind::Custom);
    hasPanel_ = d.hasPanel();
    if (hasPanel_) {
      auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, d.id),
                                                          panel::makeDocPanelTarget(ctx, d.id, [this]() -> ui::Widget* { return panel_; }));
      panel_ = &add<panel::PanelHost>("macros[" + std::to_string(d.id.value) + "]", std::move(face));
      sub_ = ctx.subscribe([this](const doc::StoreEvent&) {
        if (panel_ != nullptr) panel_->notifyChanged();
      });
    }
    update(d);
  }

  // The chain shown (the stored choice while it is still a chain of the rack, else the first).
  doc::DeviceId shown() const {
    const auto it = impl_.shownChain.find(id_.value);
    if (it != impl_.shownChain.end() && std::find(chains_.begin(), chains_.end(), it->second) != chains_.end()) return it->second;
    return chains_.empty() ? doc::DeviceId{} : chains_.front();
  }

  // Re-reads the chains; the canvas follows the chain shown.
  void refresh() {
    const doc::Project& p = *ctx_.doc();
    chains_ = chainsOf(p, id_);
    names_.clear();
    for (const doc::DeviceId c : chains_) {
      const doc::Device* d = doc::findDevice(p, c).device;
      names_.push_back(d != nullptr && !d->name.empty() ? d->name : std::string("Chain"));
    }
    const doc::DeviceId want = shown();
    if (want != viewChain_) {
      if (view_ != nullptr) impl_.retiredFrames.push_back(removeChild(*view_));  // (not destroyed inside its own handler)
      view_ = nullptr;
      viewChain_ = want;
      if (want.valid()) {
        view_ = &add<project_patch::ProjectPatchView>(ctx_, want);
        view_->faceVoices = impl_.view.faceVoices;
        view_->sync();
      }
      invalidateLayout();
      impl_.view.requestFrame();
    }
    markDirty();
  }

  float logicalWidth() const override { return logicalWidthFor(0.0f); }
  float logicalWidthFor(float available) const override {
    float w = kListW + kChainW + 2.0f;
    if (panel_ != nullptr) {
      const ui::Vec2 ws = panel_->face().worldSize();
      const float h = available > 0.0f ? available - kTitleH - 1.0f : ws.y;
      const float z = std::min(1.0f, h / std::max(1.0f, ws.y));
      w += std::max(kMinPanelW, std::ceil(ws.x * z));
    }
    return w;
  }
  float logicalHeight(float available) const override { return available; }
  void update(const doc::Device& d) override {
    name_ = d.name;
    active_ = d.active;
    instrument_ = doc::isInstrumentGroup(*ctx_.doc(), d.id);
    refresh();
  }
  bool keeps(const doc::Device& d) const override { return showsAsRack(ctx_, d) && d.hasPanel() == hasPanel_; }
  void paramsChanged(const doc::Device&, std::span<const doc::ParamRef>) override {
    if (panel_ != nullptr) panel_->notifyChanged();
  }

  ui::InfoText info() const override {
    return {"Rack",
            "A rack: a group of parallel chains (each a group fed from the rack's input; their outputs are summed). Click a "
            "chain to show it, + Chain adds one. The knobs are the rack's macros (value blocks on its panel): wire one into "
            "a control to map it. In the circuit the rack is the group it is."};
  }

  void arrange(ui::LayoutPass& pass) override {
    const ui::IRect r = rect();
    const ui::UiScale us = tree() ? tree()->uiScale() : ui::UiScale{};
    const int32_t th = us.px(kTitleH), lw = us.px(kListW);
    const ui::IRect body{r.x + 1, r.y + th, std::max(0, r.w - 2), std::max(0, r.h - th - 1)};
    int32_t x = body.x + std::min(lw, body.w);
    if (panel_ != nullptr) {
      const ui::Vec2 ws = panel_->face().worldSize();
      const float z = std::min(us.factor, static_cast<float>(body.h) / std::max(1.0f, ws.y));  // device px per world unit
      const int32_t pw = std::max(us.px(kMinPanelW), static_cast<int32_t>(std::ceil(ws.x * z)));
      pass.place(*panel_, {x, body.y, std::min(pw, std::max(0, body.right() - x)), body.h});
      x += pw;
    }
    if (view_ != nullptr) pass.place(*view_, {x, body.y, std::max(0, body.right() - x), body.h});
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* t = ctx.res.theme;
    if (t == nullptr) return;
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, 0.0f, t->surface.raised, t->surface.divider);
    const int32_t th = ctx.scale.px(kTitleH);
    const ui::IRect title{r.x, r.y, r.w, th};
    const ui::Rgba accent = instrument_ ? t->accent.primary : t->circuit.wireAudio;
    ctx.dl.addRectFilled(title.toRect(), active_ ? ui::lerp(t->surface.raised, accent, 0.25f) : t->surface.sunken);
    // The chain list.
    const ui::IRect list{r.x + 1, r.y + th, ctx.scale.px(kListW), std::max(0, r.h - th - 1)};
    ctx.dl.addRectFilled(list.toRect(), t->surface.sunken);
    if (ctx.res.uiFonts == nullptr) return;
    const std::string label = (name_.empty() ? std::string(instrument_ ? "Instrument Rack" : "Audio Effect Rack") : name_) +
                              "  ·  " + std::to_string(chains_.size()) + (chains_.size() == 1 ? " chain" : " chains");
    paint::text(ctx, ctx.res.uiFonts->small, paint::inset(title, ctx.scale.px(6.0f)), label,
                active_ ? t->text.primary : t->text.secondary, ui::TextAlign::Left);
    const doc::DeviceId cur = shown();
    for (size_t i = 0; i <= chains_.size(); ++i) {
      const ui::IRect row = rowRect(i);
      if (row.y >= list.bottom()) break;
      const bool add = i == chains_.size();
      if (!add && chains_[i] == cur) ctx.dl.addRectFilled(row.toRect(), ui::lerp(t->surface.sunken, accent, 0.35f));
      paint::text(ctx, ctx.res.uiFonts->small, paint::inset(row, ctx.scale.px(6.0f)), add ? std::string("+ Chain") : names_[i],
                  add ? t->text.secondary : t->text.primary, ui::TextAlign::Left);
    }
  }

  bool onPointer(const ui::PointerEvent& e) override {
    if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left && !inTitle(e.pos))
      for (size_t i = 0; i <= chains_.size(); ++i) {
        if (!rowRect(i).contains(e.pos)) continue;
        if (i == chains_.size()) (void)impl_.view.addChain(id_);
        else impl_.view.showChain(id_, chains_[i]);
        return true;
      }
    return FrameBase::onPointer(e);
  }

  // A row of the chain list (index chains_.size(): the "+ Chain" row), device px.
  ui::IRect rowRect(size_t i) const {
    const ui::UiScale us = tree() ? tree()->uiScale() : ui::UiScale{};
    const ui::IRect r = rect();
    const int32_t rh = us.px(kRowH);
    return {r.x + 1, r.y + us.px(kTitleH) + static_cast<int32_t>(i) * rh, us.px(kListW), rh};
  }
  project_patch::ProjectPatchView* chainView() const noexcept { return view_; }
  panel::PanelHost* macroPanel() const noexcept { return panel_; }

private:
  std::vector<doc::DeviceId> chains_;
  std::vector<std::string> names_;
  doc::DeviceId viewChain_;
  project_patch::ProjectPatchView* view_ = nullptr;
  panel::PanelHost* panel_ = nullptr;
  ViewContext::Subscription sub_;
  std::string name_;
  bool hasPanel_ = false, active_ = true, instrument_ = false;
};

}  // namespace

bool showsAsRack(ViewContext& ctx, const doc::Device& d) {
  if (!doc::isGroupDevice(d)) return false;
  const doc::Track* t = trackOf(*ctx.doc(), d.id);
  return t != nullptr && !doc::rackChains(*ctx.doc(), *t, d.id).empty();
}

std::unique_ptr<FrameBase> makeRackFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d) {
  return std::make_unique<RackFrame>(impl, ctx, d);
}

}  // namespace pq::views::device_chain

namespace pq::views {

doc::DeviceId DeviceChainView::shownChain(doc::DeviceId rack) const {
  if (auto* f = dynamic_cast<device_chain::RackFrame*>(impl_->frameOf(rack))) return f->shown();
  const std::vector<doc::DeviceId> chains = device_chain::chainsOf(*ctx_.doc(), rack);
  return chains.empty() ? doc::DeviceId{} : chains.front();
}

void DeviceChainView::showChain(doc::DeviceId rack, doc::DeviceId chain) {
  impl_->shownChain[rack.value] = chain;
  if (auto* f = dynamic_cast<device_chain::RackFrame*>(impl_->frameOf(rack))) f->refresh();
}

bool DeviceChainView::addChain(doc::DeviceId rack) {
  const doc::ops::EditResult e = doc::ops::addChain(*ctx_.doc(), ctx_.store().ids(), rack);
  if (!e) {
    ctx_.setStatus("Add Chain: " + e.error().message);
    return false;
  }
  const doc::DeviceId chain = e->devices.front();
  if (!impl_->run(e, false)) return false;
  showChain(rack, chain);
  return true;
}

}  // namespace pq::views
