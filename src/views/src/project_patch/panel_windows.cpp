// GU.5: the project patch view's panel windows — opening them from a group's capsule, their view state, the control
// selected in the circuit, sending controls to a panel (P, a window's +, a drag onto a window) and the link lines from a
// window's elements to their controls in the circuit. See pq/views/project_patch/project_patch_view.h ("GU.5") and
// pq/views/panel/panel_window.h.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "internal.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/graph/patch_file.h"
#include "pq/ui/face_painter.h"
#include "pq/views/devices/device_model.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/panel/panel_window.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::project_patch {

using ui::Vec2;

// ---- windows --------------------------------------------------------------------------------------------------------

panel::PanelWindow* ProjectPatchView::panelWindow(doc::DeviceId group) const noexcept {
  for (panel::PanelWindow* w : windows_)
    if (w->group() == group && w->state().open) return w;
  return nullptr;
}

bool ProjectPatchView::panelWindowOpen(doc::DeviceId group) const noexcept { return panelWindow(group) != nullptr; }

panel::PanelWindow* ProjectPatchView::panelWindowAt(Vec2 device) const noexcept {
  for (size_t i = windows_.size(); i-- > 0;) {  // (later windows are drawn over earlier ones)
    panel::PanelWindow* w = windows_[i];
    if (w->visible() && w->state().open && w->rect().contains(device)) return w;
  }
  return nullptr;
}

panel::PanelWindow& ProjectPatchView::makeWindow(doc::DeviceId group) {
  panel::PanelWindow::Hooks hooks;
  hooks.changed = [this](panel::PanelWindow& w) { windowChanged(w); };
  hooks.closed = [this](panel::PanelWindow& w) {
    windowChanged(w);
    canvas_->markDirty();
  };
  hooks.send = [this](panel::PanelWindow& w) { (void)sendSelectionToPanel(w.group()); };
  hooks.linksChanged = [this](panel::PanelWindow&) { canvas_->markDirty(); };
  auto& w = add<panel::PanelWindow>(ctx_, group, std::move(hooks));
  windows_.push_back(&w);
  return w;
}

void ProjectPatchView::windowChanged(panel::PanelWindow& w) {
  auto& all = ctx_.store().view().panelWindows;
  doc::PanelWindowView s = w.state();
  if (s.empty()) all.erase(w.group().value);
  else all[w.group().value] = s;
  ctx_.viewStateChanged();
  invalidateArrange();
  markDirty();
}

bool ProjectPatchView::setPanelWindowOpen(doc::DeviceId group, bool open) {
  if (embedded()) return false;
  const doc::Snapshot& snap = ctx_.doc();
  const doc::Device* g = snap ? doc::findDevice(*snap, group).device : nullptr;
  if (g == nullptr || !doc::isGroupDevice(*g)) return false;
  auto& all = ctx_.store().view().panelWindows;
  doc::PanelWindowView s;
  if (const auto it = all.find(group.value); it != all.end()) s = it->second;
  if (s.open == open) return false;
  s.open = open;
  if (open) {
    s.minimised = false;
    if (s.w <= 0.0f || s.h <= 0.0f) {
      // Placed the first time: the panel at zoom 1 (an empty group's in edit mode, with the inspector), at the canvas's
      // top right, a step down from any other open window.
      const float us = tree() != nullptr ? tree()->scale() : 1.0f;
      const float vw = static_cast<float>(rect().w) / us, vh = static_cast<float>(rect().h) / us;
      const float pw = g->hasPanel() ? g->panel->width : panel::kNewPanelW, ph = g->hasPanel() ? g->panel->height : panel::kNewPanelH;
      s.edit = !g->hasPanel();
      s.w = std::round(pw + 2.0f * panel::PanelWindow::kPad + 2.0f + (s.edit ? panel::PanelWindow::kInspectorW : 0.0f));
      s.h = std::round(ph + panel::PanelWindow::kTitleH + 2.0f * panel::PanelWindow::kPad + panel::PanelWindow::kGripPx);
      s.w = std::clamp(s.w, panel::PanelWindow::kMinW, std::max(panel::PanelWindow::kMinW, vw * 0.8f));
      s.h = std::clamp(s.h, panel::PanelWindow::kMinH, std::max(panel::PanelWindow::kMinH, vh * 0.8f));
      uint32_t others = 0;
      for (const panel::PanelWindow* w : windows_) others += w->state().open ? 1u : 0u;
      s.x = std::max(8.0f, std::round(vw - s.w - 24.0f));
      s.y = std::round(24.0f + 28.0f * static_cast<float>(others % 8));
    }
  }
  all[group.value] = s;
  ctx_.viewStateChanged();
  syncWindows();
  if (panel::PanelWindow* w = panelWindow(group); w != nullptr && open) {
    // The newest window on top (the last child is hit and drawn first).
    if (windows_.back() != w) {
      std::unique_ptr<Widget> mine = removeChild(*w);
      addChild(std::move(mine));
      std::erase(windows_, w);
      windows_.push_back(w);
    }
  }
  canvas_->markDirty();
  return true;
}

bool ProjectPatchView::togglePanelWindow(doc::DeviceId group) { return setPanelWindowOpen(group, !panelWindowOpen(group)); }

void ProjectPatchView::syncWindows() {
  if (embedded()) return;
  const doc::Snapshot& snap = ctx_.doc();
  auto& all = ctx_.store().view().panelWindows;
  // Windows whose group went: hidden now, removed at the next frame (an event of theirs may be running).
  for (panel::PanelWindow* w : windows_) {
    const doc::Device* g = snap ? doc::findDevice(*snap, w->group()).device : nullptr;
    const auto it = all.find(w->group().value);
    if (g == nullptr || !doc::isGroupDevice(*g) || it == all.end() || !it->second.open) {
      if (w->visible()) w->setVisible(false);
      if (g == nullptr) windowsStale_ = true;
      if (w->state().open) {
        doc::PanelWindowView s = w->state();
        s.open = false;
        w->setState(s);
      }
      continue;
    }
    if (!(w->state() == it->second)) w->setState(it->second);
  }
  // Windows to open.
  for (const auto& [id, s] : all) {
    if (!s.open) continue;
    const doc::DeviceId g{id};
    const doc::Device* d = snap ? doc::findDevice(*snap, g).device : nullptr;
    if (d == nullptr || !doc::isGroupDevice(*d)) continue;
    bool have = false;
    for (panel::PanelWindow* w : windows_) have = have || w->group() == g;
    if (!have) makeWindow(g).setState(s);
  }
  if (windowsStale_) requestFrame();
  invalidateArrange();
}

// ---- the selected control -----------------------------------------------------------------------------------------

void ProjectPatchView::setSelectedControl(doc::Binding b) {
  if (b == selectedControl_) return;
  selectedControl_ = std::move(b);
  canvas_->markDirty();
}

doc::Binding ProjectPatchView::controlAt(Vec2 pos) const {
  if (!canvas_->scene()) return {};
  // A face's or panel's control (a closed group's panel: the control inside the group it shows).
  const circuit::CircuitCanvas::FaceParamHit fh = canvas_->faceParamAt(pos);
  if (!fh.param.empty() && fh.node < scene_.modelNode.size()) {
    const uint32_t mn = scene_.modelNode[fh.node];
    if (mn < patch_.nodes.size() && patch_.nodes[mn].kind == NodeKind::Device) {
      const Node& n = patch_.nodes[mn];
      if (n.groupPanel) return panel::GroupPanelFace::controlOfKey(n.device, fh.param);
      return doc::controlBinding(n.device, fh.param);
    }
  }
  // A param pin.
  if (const ProjectCanvas::PinHit pin = canvas_->pinAt(pos); pin.valid() && pin.input && pin.node < patch_.nodes.size()) {
    const Node& t = patch_.nodes[pin.node];
    if (t.kind == NodeKind::Device && pin.port < t.inputs.size() && t.inputs[pin.port].param) {
      std::string param = t.inputs[pin.port].name;
      doc::DeviceId to = t.device;
      if (const size_t at = param.rfind('@'); at != std::string::npos && (t.rack || t.group)) {
        to = doc::DeviceId{static_cast<uint32_t>(std::stoul(param.substr(at + 1)))};
        param.resize(at);
      }
      return doc::controlBinding(to, std::move(param));
    }
  }
  // A block of one control (a value block): that control.
  const uint32_t mn = canvas_->nodeAt(pos);
  if (mn < patch_.nodes.size() && patch_.nodes[mn].kind == NodeKind::Device && !patch_.nodes[mn].group) {
    const std::vector<doc::Binding> cs = controlsOf(patch_.nodes[mn].device);
    if (cs.size() == 1) return cs.front();
  }
  return {};
}

std::vector<doc::Binding> ProjectPatchView::controlsOf(doc::DeviceId device) const {
  std::vector<doc::Binding> out;
  const doc::Snapshot& snap = ctx_.doc();
  const doc::Device* d = snap ? doc::findDevice(*snap, device).device : nullptr;
  const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
  if (d == nullptr || lib == nullptr || doc::isGroupDevice(*d) || d->rack) return out;  // (a group's own panel: GU.6)
  std::shared_ptr<const doc::ResolvedDevice> hold;
  const graph::Patch* patch = lib->patchOf(*d, hold);
  if (patch == nullptr) return out;
  for (const devices::ParamInfo& pi : devices::signatureOf(*patch).params) out.push_back(doc::controlBinding(device, pi.name));
  return out;
}

doc::DeviceId ProjectPatchView::panelGroupFor(doc::DeviceId device) const {
  const doc::Snapshot& snap = ctx_.doc();
  const doc::DeviceLoc l = snap ? doc::findDevice(*snap, device) : doc::DeviceLoc{};
  if (l.device == nullptr || l.track == nullptr) return {};
  // Up the groups around it: the innermost with an open window; else the outermost.
  doc::DeviceId outer;
  uint32_t guard = 0;
  for (doc::DeviceId g = l.device->group; g.valid() && guard < doc::kMaxGroupDepth + 1; ++guard) {
    if (panelWindowOpen(g)) return g;
    outer = g;
    const doc::Device* gd = nullptr;
    for (const doc::DevicePtr& x : l.track->devices)
      if (x->id == g) gd = x.get();
    g = gd != nullptr ? gd->group : doc::DeviceId{};
  }
  return outer;
}

// ---- sending ------------------------------------------------------------------------------------------------------

bool ProjectPatchView::sendToPanel(doc::DeviceId group, std::span<const doc::Binding> bindings, std::optional<Vec2> panelAt) {
  const doc::Snapshot& snap = ctx_.doc();
  const doc::DeviceLoc gl = snap ? doc::findDevice(*snap, group) : doc::DeviceLoc{};
  if (gl.device == nullptr || gl.track == nullptr || !doc::isGroupDevice(*gl.device)) {
    ctx_.setStatus("Send to Panel: select a control of a block inside a group");
    return false;
  }
  if (bindings.empty()) {
    ctx_.setStatus("Send to Panel: select a control first (press a knob, a slider or a value block in the group)");
    return false;
  }
  // What each binding becomes (its kind and size from what it is bound to).
  const std::unique_ptr<panel::PanelTarget> target = panel::makeDocPanelTarget(ctx_, group, {});
  std::vector<doc::PanelElement> add;
  for (const doc::Binding& b : bindings) {
    if (!doc::bindingAllowed(gl.track->devices, group, b)) {
      const std::string name = gl.device->name.empty() ? std::string("the group") : "'" + gl.device->name + "'";
      ctx_.setStatus("Send to Panel: that control is not inside " + name + " (a panel binds only what is inside its group)");
      return false;
    }
    const panel::BoundInfo& info = target->info(b);
    if (!info.valid) continue;
    const panel::ElementChoice c = panel::defaultElement(b, info);
    doc::PanelElement e;
    e.kind = c.kind;
    e.w = c.w;
    e.h = c.h;
    e.bindings.push_back(b);
    add.push_back(std::move(e));
  }
  if (add.empty()) {
    ctx_.setStatus("Send to Panel: nothing there to put on a panel");
    return false;
  }
  std::vector<uint32_t> ids;
  auto p = std::make_shared<doc::Panel>(panel::panelWithElements(gl.device->hasPanel() ? gl.device->panel.get() : nullptr, std::move(add), panelAt, &ids));
  if (!apply(doc::ops::setGroupPanel(*snap, group, p), "Send to Panel")) return false;
  // The window shows them, selected (in edit mode they can be placed at once).
  if (!panelWindowOpen(group)) (void)setPanelWindowOpen(group, true);
  if (panel::PanelWindow* w = panelWindow(group)) {
    w->setEditMode(true);  // the new elements selected, ready to be placed
    w->documentChanged();
    w->setSelection(std::move(ids));
  }
  return true;
}

bool ProjectPatchView::sendSelectionToPanel(doc::DeviceId group) {
  std::vector<doc::Binding> bs;
  if (selectedControl_.bound()) {
    bs.push_back(selectedControl_);
  } else {
    for (const doc::DeviceId d : selectedDevices())
      for (doc::Binding& b : controlsOf(d)) bs.push_back(std::move(b));
  }
  if (bs.empty()) {
    ctx_.setStatus("Send to Panel: select a control first (press a knob, a slider or a value block inside a group)");
    return false;
  }
  if (!group.valid()) group = panelGroupFor(bs.front().device);
  if (!group.valid()) {
    ctx_.setStatus("Send to Panel: the control is in no group (group its block first: Ctrl+G)");
    return false;
  }
  return sendToPanel(group, bs);
}

bool ProjectPatchView::dropOnPanelWindow(panel::PanelWindow& w, std::span<const doc::Binding> bindings, Vec2 at) {
  Vec2 p{0.0f, 0.0f};
  std::optional<Vec2> where;
  if (w.panelPoint(at, p)) where = p;
  return sendToPanel(w.group(), bindings, where);
}

void ProjectPatchView::setPanelDropHover(Vec2 pos, bool on) {
  panel::PanelWindow* over = on ? panelWindowAt(pos) : nullptr;
  for (panel::PanelWindow* w : windows_) w->setDropTarget(w == over);
}

// ---- link lines ---------------------------------------------------------------------------------------------------

LaneRect ProjectPatchView::panelButtonRect(const Node& n) noexcept {
  const float rad = std::min(28.0f, n.h * 0.5f);
  const float x1 = n.x + n.w - std::max(10.0f, rad * 0.6f);
  return {x1 - 16.0f, n.y + 2.0f, x1, n.y + 2.0f + 16.0f};
}

std::optional<graph::Point2> ProjectPatchView::controlWorld(const doc::Binding& b, float* radius) const {
  if (radius != nullptr) *radius = 0.0f;
  if (!b.bound()) return std::nullopt;
  const uint32_t mn = patch_.nodeOf(b.device);
  if (mn == kNone || mn >= patch_.nodes.size()) return std::nullopt;
  const Node& n = patch_.nodes[mn];
  const uint32_t sn = mn < scene_.sceneNode.size() ? scene_.sceneNode[mn] : kNone;
  auto onFace = [&](const Face* f, std::string_view key) -> std::optional<graph::Point2> {
    if (f == nullptr) return std::nullopt;
    const std::optional<ControlAnchor> a = f->controlAnchor(key).has_value() ? f->controlAnchor(key) : f->controlHome(key);
    if (!a) return std::nullopt;
    if (radius != nullptr) *radius = a->radius * n.scale;
    return graph::Point2{n.x + a->at.x * n.scale, n.y + a->at.y * n.scale};
  };
  if (n.device == b.device) {
    if (sn != kNone && b.kind == doc::BindingKind::Control) {
      if (const Face* f = canvas_->faceOn(sn))
        if (auto p = onFace(f, b.name)) return p;
      if (const Face* f = canvas_->panelOn(sn); f != nullptr && !n.groupPanel)
        if (auto p = onFace(f, b.name)) return p;
    }
    if (b.kind == doc::BindingKind::Signal)
      if (const int32_t o = n.output(b.name); o >= 0) return outPortPoint(n, static_cast<size_t>(o));
    if (const int32_t i = n.input(b.name); i >= 0) return inPortPoint(n, static_cast<size_t>(i));
    return graph::Point2{n.x + n.w * 0.5f, n.y + n.h * 0.5f};
  }
  // Inside a closed group drawn as its panel: the element there bound to it (else the block).
  if (n.groupPanel && sn != kNone && b.kind == doc::BindingKind::Control)
    if (const Face* f = canvas_->panelOn(sn))
      if (auto p = onFace(f, panel::GroupPanelFace::portKey(n.device, b))) return p;
  return graph::Point2{n.x + n.w * 0.5f, n.y + n.h * 0.5f};
}

std::vector<ProjectPatchView::PanelLink> ProjectPatchView::panelLinks() const {
  std::vector<PanelLink> out;
  if (windows_.empty() || !canvas_->scene()) return out;
  // Groups selected in the circuit: an opened capsule's frame, a closed group's block.
  std::vector<doc::DeviceId> sel = selectedDevices();
  for (const panel::PanelWindow* w : windows_) {
    if (!w->visible() || !w->state().open || w->minimised()) continue;
    const bool all = w->active() || std::find(sel.begin(), sel.end(), w->group()) != sel.end();
    const int32_t hovered = w->hoveredElement();
    if (!all && hovered < 0) continue;
    panel::GroupPanelFace& f = w->face();
    (void)f.sync();
    for (size_t i = 0; i < f.elementCount(); ++i) {
      if (!all && static_cast<int32_t>(i) != hovered) continue;
      const doc::Binding* b = f.element(i).binding();
      if (b == nullptr || b->device == w->group()) continue;  // (the group's own settings: no control in the circuit)
      const std::optional<Vec2> from = w->elementPoint(i);
      const std::optional<graph::Point2> to = controlWorld(*b);
      if (!from || !to) continue;
      out.push_back({*from, canvas_->toDevice(*to), w->group(), f.element(i).id()});
    }
  }
  return out;
}

void ProjectPatchView::paintPanelLinks(ui::PaintContext& ctx) {
  if (ctx.res.theme == nullptr) return;
  const ui::Theme& th = *ctx.res.theme;
  const float us = ctx.scale.factor;
  ui::FacePainter p(ctx.dl, ui::FaceTransform{});
  // The selected control: a ring round it (what P sends).
  if (selectedControl_.bound()) {
    float r = 0.0f;
    if (const std::optional<graph::Point2> c = controlWorld(selectedControl_, &r)) {
      const Vec2 d = canvas_->toDevice(*c);
      const float rr = std::max(6.0f * us, r * canvas_->deviceScale() + 3.0f * us);
      p.ring(d, rr, std::max(1.0f, 1.5f * us), ui::mulAlpha(th.circuit.selected, 0.9f));
    }
  }
  if (windows_.empty()) return;
  const std::vector<PanelLink> links = panelLinks();
  if (links.empty()) return;
  const ui::Rgba col = ui::mulAlpha(th.circuit.selected, 0.85f);
  const float lw = std::max(1.0f, 1.25f * us);
  for (const PanelLink& l : links) {
    const Vec2 pts[2] = {l.from, l.to};
    p.dashed(pts, lw, 5.0f * us, 3.0f * us, col);
    p.disc(l.to, 2.5f * us, col);
    p.ring(l.to, 5.0f * us, lw, ui::mulAlpha(col, 0.6f));
  }
}

}  // namespace pq::views::project_patch
