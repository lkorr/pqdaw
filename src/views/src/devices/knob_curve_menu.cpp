// The "Knob Curve" submenu (TX.knob-curves). See pq/views/devices/knob_curve_menu.h.

#include "pq/views/devices/knob_curve_menu.h"

#include <memory>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/meta/registry.h"
#include "pq/views/devices/device_model.h"
#include "pq/views/view_context.h"

namespace pq::views {

std::optional<ui::MenuItem> knobCurveMenu(ViewContext& ctx, doc::DeviceId device, const std::string& param) {
  const doc::DeviceLoc loc = doc::findDevice(*ctx.doc(), device);
  const doc::DeviceLibrary* lib = ctx.deviceLibrary();
  if (loc.device == nullptr || loc.device->rack || lib == nullptr || param.empty()) return std::nullopt;
  if (doc::isMetaDevice(*loc.device))  // the VCA's value: its curve is its face's own curve box
    if (const meta::Type* t = meta::findType(doc::metaDeviceType(*loc.device)))
      if (const int32_t i = t->paramIndex(param); i >= 0 && (t->params[static_cast<size_t>(i)].unitsInBody || t->params[static_cast<size_t>(i)].noKnobCurve))
        return std::nullopt;
  std::shared_ptr<const doc::ResolvedDevice> hold;
  const graph::Patch* patch = lib->patchOf(*loc.device, hold);
  const graph::ParamDecl* decl = patch != nullptr ? doc::findParamDecl(*patch, param) : nullptr;
  if (decl == nullptr) return std::nullopt;
  const doc::ParamTravel declared = doc::paramTravel(*decl);
  const doc::KnobCurve def = doc::declaredCurve(declared);
  const doc::KnobCurve stored = loc.device->curve(param);
  const doc::KnobCurve on = stored == doc::KnobCurve::Default || !doc::curveAvailable(declared, stored) ? def : stored;
  std::vector<ui::MenuItem> items;
  for (const doc::KnobCurve c : {doc::KnobCurve::Octave, doc::KnobCurve::Exponential, doc::KnobCurve::Logarithmic, doc::KnobCurve::Linear}) {
    if (!doc::curveAvailable(declared, c)) continue;
    std::string label = devices::knobCurveLabel(c);
    if (c == def) label += " (default)";
    const doc::KnobCurve store = c == def ? doc::KnobCurve::Default : c;
    ViewContext* cx = &ctx;
    ui::MenuItem it = ui::MenuItem::item(std::move(label), [cx, device, param, store] {
      if (auto e = doc::ops::setParamCurve(*cx->doc(), device, param, store); !e) cx->setStatus(e.error().message);
      else if (!e->empty()) (void)cx->store().dispatch(e->command);
    });
    it.checked = c == on;
    items.push_back(std::move(it));
  }
  if (items.size() < 2) return std::nullopt;
  return ui::MenuItem::sub("Knob Curve", std::move(items));
}

}  // namespace pq::views
