// GU.3: group panels — the edit builders (pq/document/edit_ops.h "group panels"; pq/document/panel.h).
#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "edit_common.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

Result<const Device*> findGroup(const Project& p, DeviceId group) {
  const DeviceLoc l = findDevice(p, group);
  if (l.device == nullptr) return notFound("Edit Panel: group not found");
  if (!isGroupDevice(*l.device)) return invalid("Edit Panel: only a group has a panel");
  return l.device;
}

// One SetGroupPanel step, checked as the command checks it (so a refused edit says why before it is dispatched).
EditResult panelEdit(const Project& p, DeviceId group, Panel panel) {
  const DeviceLoc l = findDevice(p, group);
  Edit e = makeEdit("Edit Panel");
  PanelPtr next;
  if (!panel.empty()) {
    PQ_DOC_TRY(checkPanel(panel));
    if (std::string why = bindingsProblem(l.track->devices, group, panel); !why.empty()) return invalid("Edit Panel: " + why);
    next = std::make_shared<const Panel>(std::move(panel));
  }
  const PanelPtr& now = l.device->panel;
  if ((next == nullptr && now == nullptr) || (next && now && *next == *now)) return e;
  batchOf(e).commands.push_back(SetGroupPanel{group, std::move(next)});
  e.devices.push_back(group);
  return e;
}

}  // namespace

EditResult setGroupPanel(const Project& p, DeviceId group, PanelPtr panel) {
  Result<const Device*> g = findGroup(p, group);
  if (!g) return std::move(g).error();
  return panelEdit(p, group, panel ? *panel : Panel{});
}

EditResult addPanelElements(const Project& p, DeviceId group, std::vector<PanelElement> elements, std::vector<uint32_t>* added) {
  Result<const Device*> g = findGroup(p, group);
  if (!g) return std::move(g).error();
  Panel panel = (*g)->panel ? *(*g)->panel : Panel{};
  uint32_t next = panel.nextId();
  std::unordered_set<uint32_t> taken;
  for (const PanelElement& e : panel.elements) taken.insert(e.id);
  for (const PanelElement& e : elements)
    if (e.id != 0) next = std::max(next, e.id + 1);
  float w = panel.empty() ? 0.0f : panel.width, h = panel.empty() ? 0.0f : panel.height;
  for (PanelElement& e : elements) {
    if (e.id == 0) e.id = next++;
    if (!taken.insert(e.id).second) return invalid("Edit Panel: element id " + std::to_string(e.id) + " is taken");
    if (added != nullptr) added->push_back(e.id);
    if (std::isfinite(e.x + e.w) && std::isfinite(e.y + e.h)) {
      w = std::max(w, std::ceil(e.x + e.w + kPanelMargin));
      h = std::max(h, std::ceil(e.y + e.h + kPanelMargin));
    }
    panel.elements.push_back(std::move(e));
  }
  panel.width = std::min(w, kMaxPanelSize);
  panel.height = std::min(h, kMaxPanelSize);
  return panelEdit(p, group, std::move(panel));
}

EditResult setPanelElement(const Project& p, DeviceId group, PanelElement element) {
  Result<const Device*> g = findGroup(p, group);
  if (!g) return std::move(g).error();
  if (!(*g)->panel) return notFound("Edit Panel: the group has no panel");
  Panel panel = *(*g)->panel;
  const int32_t i = panel.indexOf(element.id);
  if (i < 0) return notFound("Edit Panel: no element " + std::to_string(element.id));
  panel.elements[static_cast<size_t>(i)] = std::move(element);
  return panelEdit(p, group, std::move(panel));
}

EditResult removePanelElements(const Project& p, DeviceId group, std::span<const uint32_t> elements) {
  Result<const Device*> g = findGroup(p, group);
  if (!g) return std::move(g).error();
  if (!(*g)->panel) return makeEdit("Edit Panel");
  Panel panel = *(*g)->panel;
  const std::unordered_set<uint32_t> gone(elements.begin(), elements.end());
  std::erase_if(panel.elements, [&](const PanelElement& e) { return gone.count(e.id) != 0; });
  for (PanelElement& e : panel.elements)
    if (e.when && gone.count(e.when->element) != 0) e.when.reset();
  return panelEdit(p, group, std::move(panel));
}

EditResult setPanelSize(const Project& p, DeviceId group, float width, float height) {
  Result<const Device*> g = findGroup(p, group);
  if (!g) return std::move(g).error();
  if (!(width > 0.0f) || !(height > 0.0f)) return invalid("Edit Panel: a panel's size is > 0");
  if (!(*g)->panel) return invalid("Edit Panel: an empty panel has no size (add an element)");
  Panel panel = *(*g)->panel;
  panel.width = width;
  panel.height = height;
  return panelEdit(p, group, std::move(panel));
}

}  // namespace pq::doc::ops
