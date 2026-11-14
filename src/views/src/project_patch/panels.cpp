// The faces the project patch view's canvas hosts for blocks drawn as panels (GU.3 / GU.5b / GU.11): a closed group as
// its panel (GroupPanelFace bound to the document), a value block as its knob, a track's mixer as its face; and where a
// wire into a control ends (controlAnchorOf). See pq/views/project_patch/project_patch_view.h.

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

#include "internal.h"
#include "pq/core/automation_curve.h"
#include "pq/document/edit_ops.h"
#include "pq/views/browser/library.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/meta/face_targets.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::project_patch {

std::unique_ptr<Face> ProjectPatchView::makePanel(const circuit::SceneFace& f) {
  // TX.mixer-look: a track's mixer is drawn as its face.
  if (std::string_view(f.path).starts_with(kMixerFacePrefix)) {
    const uint32_t mn = f.node < scene_.modelNode.size() ? scene_.modelNode[f.node] : kNone;
    return mn < patch_.nodes.size() ? makeMixerFace(*this, patch_, patch_.nodes[mn]) : nullptr;
  }
  // BR.tuning-block: a MIDI band's tuning block (closed or opened to its wheel).
  if (std::string_view(f.path).starts_with(kTuningFacePrefix)) {
    const uint32_t mn = f.node < scene_.modelNode.size() ? scene_.modelNode[f.node] : kNone;
    return mn < patch_.nodes.size() ? makeTuningFace(*this, patch_, patch_.nodes[mn]) : nullptr;
  }
  // GU.5b: a value block drawn as its knob (a one-knob panel bound to its `value`; a wire into it ends on the knob).
  if (std::string_view(f.path).starts_with(kValueKnobFacePrefix)) {
    const doc::DeviceId v{f.owner};
    const uint32_t mn = patch_.nodeOf(v);
    const ui::Vec2 size = mn != kNone ? ui::Vec2{patch_.nodes[mn].w / patch_.nodes[mn].scale, patch_.nodes[mn].h / patch_.nodes[mn].scale}
                                      : ui::Vec2{metrics::kValueKnobW, metrics::kValueKnobH};
    auto p = std::make_shared<const doc::Panel>(panel::valueKnobPanel(v, size.x, size.y));
    auto face = std::make_unique<panel::GroupPanelFace>([p] { return p; },
                                                        panel::makeDocPanelTarget(ctx_, v, [c = canvas_]() -> ui::Widget* { return c; }));
    face->setOwner(v);
    return face;
  }
  // GU.3: a closed group drawn as its panel, bound to the document (menus open from the canvas).
  if (std::string_view(f.path).starts_with(kGroupPanelFacePrefix)) {
    const doc::DeviceId g{f.owner};
    auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx_, g),
                                                        panel::makeDocPanelTarget(ctx_, g, [c = canvas_]() -> ui::Widget* { return c; }));
    face->setOwner(g);  // GU.5: its elements answer for the controls inside the group ("<name>@<id>": wires end on them)
    return face;
  }
  return nullptr;  // GU.11: no other block is drawn as a panel (a native engine: its face; atoms: symbols)
}
std::optional<LayoutOptions::Anchor> ProjectPatchView::controlAnchorOf(const doc::Device& d, std::string_view param) {
  // TMM.13-fix2 (H6): the wire runs down a gap beside the control (its labels' half width, or its rim, + 3 units).
  auto point = [](const std::optional<ControlAnchor>& a) -> std::optional<LayoutOptions::Anchor> {
    if (!a) return std::nullopt;
    return LayoutOptions::Anchor{a->at.x, a->at.y, std::max(a->clear, a->radius) + 3.0f};
  };
  // The face shown now (the previous build's block of this device): its values (a meta face's handles), the control its
  // panel shows now (Operator's LFO amount on the oscillator tab or in the LFO section), else the control's place.
  const uint32_t mn = patch_.nodeOf(d.id);
  if (mn != kNone && mn < scene_.sceneNode.size() && scene_.sceneNode[mn] != kNone) {
    const uint32_t sn = scene_.sceneNode[mn];
    if (const Face* f = canvas_->faceOn(sn)) return point(f->controlHome(param));
    if (const Face* f = canvas_->panelOn(sn)) {
      if (const std::optional<ControlAnchor> now = f->controlAnchor(param)) return point(now);
      return point(f->controlHome(param));
    }
  }
  // Not shown yet: a prototype of its kind. GU.5: a closed group's panel — the element bound to the control inside it.
  if (doc::isGroupDevice(d)) {
    if (!d.hasPanel()) return std::nullopt;
    const doc::PanelPtr p = d.panel;
    panel::GroupPanelFace f([p] { return p; }, panel::makeDocPanelTarget(ctx_, d.id, {}));
    f.setOwner(d.id);
    return point(f.controlHome(param));
  }
  if (doc::isMetaDevice(d)) {
    const meta::Type* t = meta::findType(doc::metaDeviceType(d));
    if (t == nullptr || !hasFace(*t)) return std::nullopt;
    std::unique_ptr<MetaFace>& f = anchorFaces_[t];
    if (!f) f = makeFace(*t);
    if (!f) return std::nullopt;
    f->bind(std::make_unique<DocTarget>(ctx_, d.id, std::string(), *t), nullptr);  // its values (a handle's place)
    return point(f->controlHome(param));
  }
  return std::nullopt;
}

// ---- TL.6: automated controls follow the playhead ------------------------------------------------------------------

Face* ProjectPatchView::autoFace(const AutoControl& c) const noexcept {
  Face* f = c.kind == AutoControl::Kind::Meta ? static_cast<Face*>(canvas_->faceOn(c.node)) : canvas_->panelOn(c.node);
  return f == c.face ? f : nullptr;
}

namespace {
// Shows `v` (nullopt: its own value) on control `c` of face `f`; true when what the face shows changed. A mixer
// control damages its own rect here (`area` face-local).
bool showOn(Face& f, const doc::ParamRef& p, const ProjectPatchView::AutoControl& c, std::optional<double> v, ProjectCanvas& canvas) {
  using Kind = ProjectPatchView::AutoControl::Kind;
  switch (c.kind) {
    case Kind::Panel:
      if (auto* g = dynamic_cast<panel::GroupPanelFace*>(&f)) return g->target().showPosition(c.binding, v);
      return false;
    case Kind::Meta: return static_cast<MetaFace&>(f).target().showPosition(c.param, v);
    case Kind::Mixer: {
      ui::Rect area{};
      if (mixerShow(f, p, v, area)) canvas.damageFace(c.node, &area);
      return false;  // repainted
    }
  }
  return false;
}
}  // namespace

void ProjectPatchView::refreshAutomation() {
  // The controls followed so far show their own values again (their faces may be on other nodes now, or gone: found
  // by pointer among the canvas's faces; never through a stale pointer).
  if (!autoControls_.empty()) {
    auto release = [&](Face& f, uint32_t node) {
      bool changed = false;
      for (const AutoControl& c : autoControls_)
        if (c.face == &f && c.target < autoFollow_.targets().size())
          changed = showOn(f, autoFollow_.targets()[c.target].param, AutoControl{c.kind, &f, node, c.target, c.param, c.binding},
                           std::nullopt, *canvas_) || changed;
      if (changed) canvas_->damageFace(node);
    };
    for (size_t i = 0; i < canvas_->panelCount(); ++i) release(canvas_->panelAt(i), canvas_->panelNode(i));
    for (size_t i = 0; i < canvas_->faceCount(); ++i) release(canvas_->faceAt(i), canvas_->faceNode(i));
  }
  autoControls_.clear();
  autoFollow_.clear();
  if (shown()) {
    std::vector<uint32_t> seen;  // a track's lanes once
    for (uint32_t b = 0; b < patch_.bands.size(); ++b)
      if (const doc::Track* t = bandTrack(b); t != nullptr && std::find(seen.begin(), seen.end(), t->id.value) == seen.end()) {
        seen.push_back(t->id.value);
        autoFollow_.collect(ctx_, *t, true);
      }
  }
  const std::vector<AutomationFollow::Target>& ts = autoFollow_.targets();
  if (ts.empty()) return autoFollow_.poll(*this, false);
  auto add = [&](AutoControl c) {
    for (const AutoControl& x : autoControls_)  // two elements bound to one control: once
      if (x.face == c.face && x.target == c.target) return;
    autoControls_.push_back(std::move(c));
  };
  // Panels: a track's mixer face, a closed group's panel (its members' and embedded panels' controls, native engine
  // sections), a value block's knob. A macro-mapped param is left to its macro (the panel target shows it).
  for (size_t i = 0; i < canvas_->panelCount(); ++i) {
    Face& f = canvas_->panelAt(i);
    const uint32_t node = canvas_->panelNode(i);
    if (const doc::TrackId tr = mixerFaceTrack(f); tr.valid()) {
      for (uint32_t k = 0; k < ts.size(); ++k)
        if (!doc::isDeviceParamKind(ts[k].param.kind) && ts[k].param.owner == tr.value)
          add({AutoControl::Kind::Mixer, &f, node, k, 0, {}});
      continue;
    }
    auto* g = dynamic_cast<panel::GroupPanelFace*>(&f);
    if (g == nullptr) continue;
    (void)g->sync();
    g->visitControls([&](const doc::Binding& b, const ui::Rect&) {
      if (b.kind != doc::BindingKind::Control || !b.meta.empty()) return;
      for (uint32_t k = 0; k < ts.size(); ++k)
        if (ts[k].param.kind == doc::ParamKind::DeviceParam && !ts[k].mapped && ts[k].param.owner == b.device.value && ts[k].name == b.name)
          add({AutoControl::Kind::Panel, &f, node, k, 0, b});
    });
  }
  // Meta faces (a chain meta device's own params).
  for (size_t i = 0; i < canvas_->faceCount(); ++i) {
    MetaFace& f = canvas_->faceAt(i);
    const auto* dt = dynamic_cast<const DocTarget*>(&f.target());
    if (dt == nullptr || !dt->path().empty()) continue;
    for (uint32_t k = 0; k < ts.size(); ++k) {
      if (ts[k].param.kind != doc::ParamKind::DeviceParam || ts[k].mapped || ts[k].param.owner != dt->device().value) continue;
      const int32_t idx = f.type().paramIndex(ts[k].name);
      if (idx >= 0) add({AutoControl::Kind::Meta, &f, canvas_->faceNode(i), k, static_cast<uint32_t>(idx), {}});
    }
  }
  applyAutomation();
}

void ProjectPatchView::applyAutomation() {
  if (autoControls_.empty()) return autoFollow_.poll(*this, false);
  const PlayheadSnapshot ph = ctx_.transport().playhead();
  const double tick = static_cast<double>(ph.tick);
  const std::vector<AutomationFollow::Target>& ts = autoFollow_.targets();
  bool live = false;
  for (size_t i = 0; i < autoControls_.size();) {  // by face (the list is grouped so)
    const AutoControl& first = autoControls_[i];
    Face* f = autoFace(first);
    if (f == nullptr) return refreshAutomation();  // the canvas's faces changed under the list (fresh after: no loop)
    bool changed = false;
    size_t j = i;
    for (; j < autoControls_.size() && autoControls_[j].face == first.face; ++j) {
      const AutoControl& c = autoControls_[j];
      const AutomationFollow::Target& t = ts[c.target];
      std::optional<double> v;
      if (t.follows()) {
        v = AutomationFollow::valueAt(t, tick, 0.0);
        live = true;
      }
      changed = showOn(*f, t.param, c, v, *canvas_) || changed;
    }
    if (changed) {
      // A panel: only its controls that moved (GroupPanelFace::changedRects; the whole panel the first time). A meta
      // face: the face (its curves and plots read the params anywhere on it).
      auto* g = first.kind == AutoControl::Kind::Panel ? dynamic_cast<panel::GroupPanelFace*>(f) : nullptr;
      autoRects_.clear();
      if (g != nullptr && g->changedRects(autoRects_))
        for (const ui::Rect& r : autoRects_) canvas_->damageFace(first.node, &r);
      else
        canvas_->damageFace(first.node);
    }
    i = j;
  }
  autoFollow_.poll(*this, live && ph.playing && shown());
}

}  // namespace pq::views::project_patch
