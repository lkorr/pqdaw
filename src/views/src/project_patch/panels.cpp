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

}  // namespace pq::views::project_patch
