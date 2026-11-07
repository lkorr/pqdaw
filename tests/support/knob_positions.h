#pragma once

// GU.1 test support (docs/plans/GU.md "Curves": a param stores its KNOB POSITION). The tests written before GU.1 set
// params by their values in units (SetDeviceParam{device, param, value} in a Batch). knobPositions(p, b, lib) turns
// every such command of `b` into the position that gives the value through the param's curve — a SetParamCurve earlier
// in the batch, else the device's own (in `p`), else its declaration in `lib` — by the nearest-position rule, exactly as
// ops::setDeviceParamValue does for one value. The device may be in `p` or added by the batch (AddTrack / AddDevice, a
// rack's chains included). A rack's params (macros, the selector) and unknown devices or params are left as they are.

#include <memory>
#include <string_view>
#include <variant>

#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/lower.h"
#include "pq/document/model.h"

namespace knob_positions {

inline const pq::doc::Device* findIn(const pq::doc::Device& d, pq::doc::DeviceId id) {
  return d.id == id ? &d : nullptr;
}

inline const pq::doc::Device* findIn(const pq::doc::Track& t, pq::doc::DeviceId id) {
  for (const pq::doc::DevicePtr& d : t.devices)
    if (const pq::doc::Device* f = findIn(*d, id)) return f;
  for (const pq::doc::TrackPtr& c : t.children)
    if (const pq::doc::Device* f = findIn(*c, id)) return f;
  return nullptr;
}

inline pq::doc::Batch knobPositions(const pq::doc::Project& p, pq::doc::Batch b, const pq::doc::DeviceLibrary& lib) {
  using namespace pq::doc;
  auto deviceOf = [&](DeviceId id) -> const Device* {
    if (const Device* d = findDevice(p, id).device) return d;
    for (const Command& c : b.commands) {
      if (const auto* a = std::get_if<AddDevice>(&c.v); a != nullptr && a->device)
        if (const Device* f = findIn(*a->device, id)) return f;
      if (const auto* t = std::get_if<AddTrack>(&c.v); t != nullptr && t->track)
        if (const Device* f = findIn(*t->track, id)) return f;
    }
    return nullptr;
  };
  for (size_t i = 0; i < b.commands.size(); ++i) {
    auto* s = std::get_if<SetDeviceParam>(&b.commands[i].v);
    if (s == nullptr || !s->value) continue;
    const Device* d = deviceOf(s->device);
    if (d == nullptr) continue;
    const pq::graph::Patch* callee = lib.patchOf(*d);
    const pq::graph::ParamDecl* decl = callee != nullptr ? findAnyParamDecl(*callee, s->param) : nullptr;
    if (decl == nullptr) continue;
    pq::dsp::Curve curve = controlCurve(*d, s->param, *decl);
    for (size_t j = 0; j < i; ++j)  // a curve set earlier in the batch
      if (const auto* c = std::get_if<SetParamCurve>(&b.commands[j].v); c != nullptr && c->device == s->device && c->param == s->param)
        curve = c->curve ? *c->curve : declaredCurve(*decl);
    s->value = pq::dsp::curvePosition(curve, *s->value, paramPosition(*d, s->param, *decl));
  }
  return b;
}

// The knob position of a device param `param` of the device `device` in `p` that gives `value` (in units) through its
// curve (the device's own, else the declared one). A rack's params and unknown ones: `value` as it is.
inline double positionOf(const pq::doc::Project& p, pq::doc::DeviceId device, std::string_view param, double value,
                         const pq::doc::DeviceLibrary& lib) {
  using namespace pq::doc;
  const Device* d = findDevice(p, device).device;
  if (d == nullptr) return value;
  const pq::graph::Patch* callee = lib.patchOf(*d);
  const pq::graph::ParamDecl* decl = callee != nullptr ? findAnyParamDecl(*callee, param) : nullptr;
  if (decl == nullptr) return value;
  return pq::dsp::curvePosition(controlCurve(*d, param, *decl), value, paramPosition(*d, param, *decl));
}

// A device param's automation lane written in units (GU.1: lanes hold knob positions): its points through the param's
// curve. Other lanes (a rack's macros, mixers, wires) as they are.
inline void lanePositions(const pq::doc::Project& p, const pq::doc::ParamRef& r, std::string_view param,
                          std::vector<pq::doc::Breakpoint>& points, const pq::doc::DeviceLibrary& lib) {
  using namespace pq::doc;
  if (r.kind != ParamKind::DeviceParam) return;
  for (Breakpoint& b : points) b.value = static_cast<float>(positionOf(p, DeviceId{r.owner}, param, b.value, lib));
}

// The same for one command: a Batch, or a lone SetDeviceParam; anything else as it is.
inline pq::doc::Command inUnits(const pq::doc::Project& p, pq::doc::Command c, const pq::doc::DeviceLibrary& lib) {
  using namespace pq::doc;
  if (auto* b = std::get_if<Batch>(&c.v)) return Command{knobPositions(p, std::move(*b), lib)};
  if (std::holds_alternative<SetDeviceParam>(c.v)) {
    Batch b{"", {}};
    b.commands.push_back(std::move(c));
    b = knobPositions(p, std::move(b), lib);
    return std::move(b.commands.front());
  }
  return c;
}

}  // namespace knob_positions
