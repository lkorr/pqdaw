// TX.panel-faces: closed stock devices drawn as their Device-view panels in the project patch view — what a device's
// panel is (the Device view's template), its size for the layout, the face the canvas hosts (devices::PanelFace) bound
// to the document, and the shown panels kept up to date. See pq/views/project_patch/project_patch_view.h.

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

#include "../common/panel_template.h"
#include "internal.h"
#include "pq/core/automation_curve.h"
#include "pq/document/edit_ops.h"
#include "pq/views/browser/library.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/devices/panel_face.h"
#include "pq/views/meta/face_targets.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::project_patch {

namespace {

// A block's panel edits the document's device instance, exactly as the Device view's panel does: a param drag is one
// store gesture (one undo step; each change applied at once, a param write), a const or the activator one command (a
// recompile of the track's unit), a knob a macro drives turns its macro.
class PatchPanelTarget final : public devices::DeviceTarget {
public:
  PatchPanelTarget(ProjectPatchView& view, doc::DeviceId id) : view_(view), ctx_(view.context()), id_(id) {}
  devices::DevicePanel* panel = nullptr;  // set once the face made it (the signature of mapped params)

  void paramGestureBegin(std::string_view) override {
    if (!ctx_.store().inGesture()) {
      (void)ctx_.store().beginGesture("Device Parameter");
      ownGesture_ = true;
    }
  }
  void setParam(std::string_view param, double value) override {
    // GU.1: the document stores the knob position that gives the value (nearest the knob's current one).
    const doc::Device* d = doc::findDevice(*ctx_.doc(), id_).device;
    const devices::ParamInfo* info = panel != nullptr ? panel->signature().findParam(param) : nullptr;
    if (d != nullptr && info != nullptr) {
      live(doc::ops::setDeviceParam(*ctx_.doc(), id_, std::string(param),
                                    devices::toPosition(*info, value, devices::storedPosition(*info, *d))));
      return;
    }
    live(doc::ops::setDeviceParam(*ctx_.doc(), id_, std::string(param), value));
  }
  void paramGestureEnd(std::string_view) override {
    if (ownGesture_ && ctx_.store().inGesture()) (void)ctx_.store().commitGesture();
    ownGesture_ = false;
  }
  void setConst(std::string_view name, double value) override { run(doc::ops::setDeviceSetting(*ctx_.doc(), id_, std::string(name), value)); }
  void setActive(bool on) override { run(doc::ops::setDevicesActive(*ctx_.doc(), std::vector<doc::DeviceId>{id_}, on)); }
  const meta::Type* metaType(std::string_view path) const override { return metaTypeAt(ctx_, id_, path); }
  std::unique_ptr<MetaTarget> metaTarget(std::string_view path, const meta::Type& type) override {
    return std::make_unique<DocTarget>(ctx_, id_, std::string(path), type);
  }
  VoiceTapReader* metaVoices(std::string_view path) override { return view_.faceVoices ? view_.faceVoices(id_, path) : nullptr; }
  // TX.spectrum: the device's analysis (its panel's spectrum and measured response) and the engine's rate.
  devices::SpectrumFeed* outputSpectrum() override { return ctx_.analysisFeed(id_); }
  double sampleRate() const override { return ctx_.analysisHub() ? ctx_.analysisHub()->sampleRate() : 48000.0; }

private:
  // Inside a gesture each change applies at once (live param writes); outside it is one undo step of its own.
  void live(const doc::ops::EditResult& e) {
    if (e && !e->empty()) (void)ctx_.store().dispatch(e->command);
  }
  void run(const doc::ops::EditResult& e) {
    if (!e) {
      ctx_.setStatus(e.error().message);
      return;
    }
    if (e->empty()) return;
    if (Status s = ctx_.store().dispatch(e->command); !s) ctx_.setStatus(s.error().message);
  }
  ProjectPatchView& view_;
  ViewContext& ctx_;
  doc::DeviceId id_;
  bool ownGesture_ = false;
};

std::string panelTitle(const doc::Device& d, const devices::DevicePanel& p) { return d.name.empty() ? p.desc().title : d.name; }

}  // namespace

const PanelTemplate* ProjectPatchView::panelTemplate(const doc::Device& d) {
  const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
  if (lib != panelTemplatesFrom_) {
    panelTemplates_.clear();
    panelTemplatesFrom_ = lib;
  }
  if (lib == nullptr || d.edited() || doc::isMetaDevice(d)) return nullptr;
  if (const auto it = panelTemplates_.find(d.patch); it != panelTemplates_.end()) return it->second.get();
  return (panelTemplates_[d.patch] = buildPanelTemplate(lib, d)).get();
}

graph::Point2 ProjectPatchView::panelSize(const doc::Device& d) {
  const PanelTemplate* t = panelTemplate(d);
  if (t == nullptr) return {0.0f, 0.0f};
  const ui::Vec2 s = devices::PanelFace::sizeFor(t->desc);
  return {s.x, s.y};
}

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
  const doc::DeviceLoc loc = doc::findDevice(*ctx_.doc(), doc::DeviceId{f.owner});
  if (loc.device == nullptr) return nullptr;
  const doc::Device& d = *loc.device;
  const PanelTemplate* t = panelTemplate(d);
  const uint32_t mn = patch_.nodeOf(d.id);
  if (t == nullptr || mn == kNone || !patch_.nodes[mn].panel) return nullptr;
  const Node& n = patch_.nodes[mn];
  std::map<std::string, double, std::less<>> values;
  for (const doc::DeviceValue& v : d.params)  // GU.1: the panel shows values: the curve at the stored position
    if (const devices::ParamInfo* pi = t->sig.findParam(v.name)) values[v.name] = devices::storedValue(*pi, d);
  for (const doc::DeviceValue& v : d.settings) values[v.name] = v.value;
  auto target = std::make_unique<PatchPanelTarget>(*this, d.id);
  PatchPanelTarget* tp = target.get();
  // TX.resize: the panel in its own units (a scaled block draws it at its scale).
  auto face = std::make_unique<devices::PanelFace>(t->sig, t->desc, std::move(target), values, ui::Vec2{n.w / n.scale, n.h / n.scale});
  devices::DevicePanel& p = face->panel();
  tp->panel = &p;
  for (const doc::ControlCurve& c : d.curves) p.setParamCurve(c.param, &c.curve);  // GU.1
  p.setActive(d.active);
  p.setTitle(panelTitle(d, p));
  p.setPresetName(ctx_.library().presetName(d.id));
  panelSynced_[d.id.value] = loc.list()[loc.index];
  return face;
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
  // A panel's controls have their places in its description (no layout needed).
  const PanelTemplate* t = panelTemplate(d);
  if (t == nullptr) return std::nullopt;
  std::unique_ptr<Face>& f = anchorPanels_[d.patch];
  if (!f)
    f = std::make_unique<devices::PanelFace>(t->sig, t->desc, std::make_unique<PatchPanelTarget>(*this, d.id),
                                             std::map<std::string, double, std::less<>>{}, devices::PanelFace::sizeFor(t->desc));
  return point(f->controlHome(param));
}

devices::PanelFace* ProjectPatchView::panelOf(doc::DeviceId device) const noexcept {
  const uint32_t mn = patch_.nodeOf(device);
  if (mn == kNone || mn >= scene_.sceneNode.size() || scene_.sceneNode[mn] == kNone || patch_.nodes[mn].groupPanel) return nullptr;
  return static_cast<devices::PanelFace*>(canvas_->panelOn(scene_.sceneNode[mn]));  // the canvas's panels are ours
}

void ProjectPatchView::syncPanels() {
  if (canvas_->panelCount() == 0) return;
  const doc::Snapshot& snap = ctx_.doc();
  // The top-level chain devices by id, one walk of the project (panels are drawn for those only).
  std::unordered_map<uint32_t, const doc::DevicePtr*> devices;
  devices.reserve(canvas_->panelCount() * 2);
  doc::forEachTrack(*snap, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::DevicePtr& d : t.devices) devices.emplace(d->id.value, &d);
  });
  for (size_t i = 0; i < canvas_->panelCount(); ++i) {
    const uint32_t sn = canvas_->panelNode(i);
    const uint32_t mn = sn < scene_.modelNode.size() ? scene_.modelNode[sn] : kNone;
    if (mn >= patch_.nodes.size() || patch_.nodes[mn].kind != NodeKind::Device) continue;  // TX.mixer-look: a mixer's face
    if (patch_.nodes[mn].groupPanel) continue;  // GU.3: a group's panel follows the document itself
    auto& face = static_cast<devices::PanelFace&>(canvas_->panelAt(i));
    const Node& n = patch_.nodes[mn];
    face.setWorldSize({n.w / n.scale, n.h / n.scale});  // TX.resize: its own units
    const auto found = devices.find(n.device.value);
    if (found == devices.end()) continue;
    const doc::DevicePtr& now = *found->second;
    doc::DevicePtr& shown = panelSynced_[n.device.value];
    if (shown == now) continue;  // unchanged (a persistent document: an edited device is a new object)
    shown = now;
    const doc::Device& d = *now;
    devices::DevicePanel& p = face.panel();
    p.setTitle(panelTitle(d, p));
    p.setActive(d.active);
    p.setPresetName(ctx_.library().presetName(d.id));
    for (const devices::ParamInfo& pi : p.signature().params) {
      p.setParamCurve(pi.name, d.curve(pi.name));  // GU.1
      p.setParamValue(pi.name, devices::storedValue(pi, d));
    }
    for (const devices::ConstInfo& c : p.signature().consts) {
      const doc::DeviceValue* v = d.setting(c.name);
      p.setConstValue(c.name, v != nullptr ? v->value : c.def);
    }
  }
  if (panelSynced_.size() > 2 * canvas_->panelCount() + 64) {  // devices whose panels went: forget them
    std::unordered_map<uint32_t, doc::DevicePtr> keep;
    for (size_t i = 0; i < canvas_->panelCount(); ++i) {
      const uint32_t sn = canvas_->panelNode(i);
      const uint32_t mn = sn < scene_.modelNode.size() ? scene_.modelNode[sn] : kNone;
      if (mn >= patch_.nodes.size()) continue;
      const auto it = panelSynced_.find(patch_.nodes[mn].device.value);
      if (it != panelSynced_.end()) keep.insert(*it);
    }
    panelSynced_.swap(keep);
  }
}

void ProjectPatchView::setPanelsShown(bool on) {
  if (panelsShown_ == on) return;
  panelsShown_ = on;
  stale_ = true;
  if (shown()) rebuild();
}

}  // namespace pq::views::project_patch
