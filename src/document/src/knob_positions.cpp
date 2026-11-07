// GU.1: values → knob positions, the model part of schema 19 → 20 (knob_positions.h).

#include "knob_positions.h"

#include <algorithm>
#include <memory>

#include "pq/document/devices.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"
#include "pq/meta/retired.h"

namespace pq::doc {
namespace {

dsp::CurvePreset presetOfLegacy(uint8_t c) noexcept {
  switch (c) {
    case 2: return dsp::CurvePreset::Octave;
    case 3: return dsp::CurvePreset::Exponential;
    case 4: return dsp::CurvePreset::Logarithmic;
    default: return dsp::CurvePreset::Linear;
  }
}

void collect(const Device& d, std::vector<std::string>& out) {
  for (const DeviceValue& v : d.params) out.push_back(std::to_string(d.id.value) + ":" + v.name);
  if (d.meta)
    for (const MetaEntry& e : *d.meta)
      if (!e.path.empty())
        for (const DeviceValue& v : e.instance.params) out.push_back(std::to_string(d.id.value) + ":" + e.path + "." + v.name);
}

struct Converter {
  const DeviceLibrary& lib;
  const std::vector<LegacyCurve>& legacy;

  const LegacyCurve* legacyOf(DeviceId d, std::string_view param) const {
    for (const LegacyCurve& l : legacy)
      if (l.device == d && l.param == param) return &l;
    return nullptr;
  }
  // The curve `d`'s param plays after the migration (and whether it is its own: stored in Device::curves).
  bool curveOf(const Device& d, std::string_view param, dsp::Curve& out, bool& own) const {
    dsp::Curve declared;
    const meta::Type* retired = isMetaDevice(d) && meta::findType(metaDeviceType(d)) == nullptr
                                    ? meta::findRetiredType(metaDeviceType(d))
                                    : nullptr;
    if (retired != nullptr) {  // GU.4: a retired meta module (the library has no patch for it): its old declaration
      const int32_t i = retired->paramIndex(param);
      if (i < 0) return false;
      declared = meta::paramCurve(retired->params[static_cast<size_t>(i)]);
    } else {
      const graph::Patch* callee = lib.patchOf(d);
      const graph::ParamDecl* decl = callee != nullptr ? findAnyParamDecl(*callee, param) : nullptr;
      if (decl == nullptr) return false;
      declared = declaredCurve(*decl);
    }
    out = declared;
    own = false;
    if (const LegacyCurve* l = legacyOf(d.id, param); l != nullptr && l->curve != 0) {
      out = curveWithPreset(declared, presetOfLegacy(l->curve));
      own = !(out == declared);
    }
    return true;
  }

  DevicePtr device(const DevicePtr& dp) const {
    const Device& d = *dp;
    auto n = std::make_shared<Device>(d);
    for (DeviceValue& v : n->params) {
      dsp::Curve c;
      bool own = false;
      if (!curveOf(d, v.name, c, own)) continue;  // an unknown patch or param: kept as it is
      v.value = dsp::curvePosition(c, v.value, 0.0);
      if (own) {
        const auto it = std::lower_bound(n->curves.begin(), n->curves.end(), v.name,
                                         [](const ControlCurve& a, const std::string& b) { return a.param < b; });
        if (it == n->curves.end() || it->param != v.name) n->curves.insert(it, ControlCurve{v.name, c});
      }
    }
    // A switched knob left at its default (no value stored): its curve still applies.
    for (const LegacyCurve& l : legacy) {
      if (l.device != d.id || l.curve == 0 || n->curve(l.param) != nullptr) continue;
      dsp::Curve c;
      bool own = false;
      if (!curveOf(d, l.param, c, own) || !own) continue;
      const auto it = std::lower_bound(n->curves.begin(), n->curves.end(), l.param,
                                       [](const ControlCurve& a, const std::string& b) { return a.param < b; });
      n->curves.insert(it, ControlCurve{l.param, c});
    }
    if (d.meta) {
      auto m = std::make_shared<MetaMap>(*d.meta);
      for (MetaEntry& e : *m) {
        if (e.path.empty()) continue;
        const meta::Type* t = meta::findTypeOrRetired(e.instance.type);
        if (t == nullptr) continue;
        for (DeviceValue& v : e.instance.params)
          if (const int32_t i = t->paramIndex(v.name); i >= 0)
            v.value = dsp::curvePosition(meta::paramCurve(t->params[static_cast<size_t>(i)]), v.value, 0.0);
      }
      n->meta = std::move(m);
    }
    return n;
  }

  TrackPtr track(const TrackPtr& tp, const Project& before) const {
    auto t = std::make_shared<Track>(*tp);
    for (DevicePtr& d : t->devices) d = device(d);
    for (TrackPtr& c : t->children) c = track(c, before);
    for (LanePtr& lp : t->lanes) {
      if (lp->param.kind != ParamKind::DeviceParam || lp->paramName.empty()) continue;
      const Device* d = findDevice(before, DeviceId{lp->param.owner}).device;
      if (d == nullptr) continue;
      dsp::Curve c;
      bool own = false;
      if (!curveOf(*d, lp->paramName, c, own)) continue;
      auto l = std::make_shared<AutomationLane>(*lp);
      for (Breakpoint& b : l->points) b.value = static_cast<float>(dsp::curvePosition(c, b.value, 0.0));
      lp = std::move(l);
    }
    return t;
  }
};

}  // namespace

std::vector<std::string> knobPositionsToConvert(const Device& d) {
  std::vector<std::string> out;
  collect(d, out);
  return out;
}

Status migrateToKnobPositions(Project& p, const DeviceLibrary* lib, const std::vector<LegacyCurve>& legacy) {
  if (lib == nullptr) {
    std::vector<std::string> what;
    forEachTrack(p, [&](const Track& t, const Track*) {
      for (const DevicePtr& d : t.devices) collect(*d, what);
      for (const LanePtr& l : t.lanes)
        if (l->param.kind == ParamKind::DeviceParam && !l->points.empty()) what.push_back("a device automation lane");
    });
    if (what.empty() && legacy.empty()) return Ok{};
    return Error{ErrorCode::Unsupported,
                 "the project stores param values, not knob positions (schema 19 or older; e.g. " +
                     (what.empty() ? std::string("a knob curve") : what.front()) +
                     "): converting it needs the device library (LoadOptions::devices)",
                 {}};
  }
  const Converter c{*lib, legacy};
  const Project before = p;
  for (TrackPtr& t : p.tracks) t = c.track(t, before);
  for (TrackPtr& t : p.returns) t = c.track(t, before);
  if (p.master) p.master = c.track(p.master, before);
  return Ok{};
}

}  // namespace pq::doc
