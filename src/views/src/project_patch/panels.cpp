// TX.panel-faces: closed stock devices drawn as their Device-view panels in the project patch view — what a device's
// panel is (the Device view's template), its size for the layout, the face the canvas hosts (devices::PanelFace) bound
// to the document, and the shown panels kept up to date. See pq/views/project_patch/project_patch_view.h.

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

#include "../common/panel_template.h"
#include "pq/core/automation_curve.h"
#include "pq/document/edit_ops.h"
#include "pq/views/browser/library.h"
#include "pq/views/devices/panel_face.h"
#include "pq/views/meta/face_targets.h"
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
    // T4.8: a knob a macro drives turns its macro (the inverse of the mapping), as in the Device view.
    if (const doc::MappingLoc m = doc::mappingOf(*ctx_.doc(), id_, param); m.mapping != nullptr) {
      const devices::ParamInfo* info = panel != nullptr ? panel->signature().findParam(param) : nullptr;
      if (info == nullptr) return;
      const doc::MacroMapping& mm = *m.mapping;
      const double span = mm.max - mm.min;
      const double s = std::abs(span) < 1e-9 ? 0.0 : std::clamp((devices::toPosition(*info, value) - mm.min) / span, 0.0, 1.0);
      const double k = kAutomationCurveK * mm.curve;
      const double x = std::abs(k) < kAutomationLinearK ? s : std::log1p(s * std::expm1(k)) / k;  // shape⁻¹
      live(doc::ops::setMacroValue(*ctx_.doc(), m.rack->id, mm.macro, std::clamp(x, 0.0, 1.0) * doc::kMacroMax));
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
  if (lib == nullptr || d.edited() || d.rack || doc::isMetaDevice(d)) return nullptr;
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
  const doc::DeviceLoc loc = doc::findDevice(*ctx_.doc(), doc::DeviceId{f.owner});
  if (loc.device == nullptr) return nullptr;
  const doc::Device& d = *loc.device;
  const PanelTemplate* t = panelTemplate(d);
  const uint32_t mn = patch_.nodeOf(d.id);
  if (t == nullptr || mn == kNone || !patch_.nodes[mn].panel) return nullptr;
  const Node& n = patch_.nodes[mn];
  std::map<std::string, double, std::less<>> values;
  for (const doc::DeviceValue& v : d.params) values[v.name] = v.value;
  for (const doc::DeviceValue& v : d.settings) values[v.name] = v.value;
  auto target = std::make_unique<PatchPanelTarget>(*this, d.id);
  PatchPanelTarget* tp = target.get();
  auto face = std::make_unique<devices::PanelFace>(t->sig, t->desc, std::move(target), values, ui::Vec2{n.w, n.h});
  devices::DevicePanel& p = face->panel();
  tp->panel = &p;
  p.setActive(d.active);
  p.setFree(d.free);
  p.setTitle(panelTitle(d, p));
  p.setPresetName(ctx_.library().presetName(d.id));
  panelSynced_[d.id.value] = loc.list()[loc.index];
  return face;
}

std::optional<graph::Point2> ProjectPatchView::controlAnchorOf(const doc::Device& d, std::string_view param) {
  auto point = [](const std::optional<ControlAnchor>& a) -> std::optional<graph::Point2> {
    if (!a) return std::nullopt;
    return graph::Point2{a->at.x, a->at.y};
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
  // Not shown yet: a prototype of its kind.
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
  if (mn == kNone || mn >= scene_.sceneNode.size() || scene_.sceneNode[mn] == kNone) return nullptr;
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
    auto& face = static_cast<devices::PanelFace&>(canvas_->panelAt(i));
    const uint32_t sn = canvas_->panelNode(i);
    const uint32_t mn = sn < scene_.modelNode.size() ? scene_.modelNode[sn] : kNone;
    if (mn >= patch_.nodes.size()) continue;
    const Node& n = patch_.nodes[mn];
    face.setWorldSize({n.w, n.h});
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
    p.setFree(d.free);
    p.setPresetName(ctx_.library().presetName(d.id));
    for (const devices::ParamInfo& pi : p.signature().params) {
      const doc::DeviceValue* v = d.param(pi.name);
      p.setParamValue(pi.name, v != nullptr ? v->value : pi.def);
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
