// Meta-module edit builders (TMM.3): pq/document/edit_ops.h, "Meta modules".
#include "edit_common.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"
#include "pq/meta/sampler.h"

#include <algorithm>
#include <cmath>

namespace pq::doc::ops {

using namespace detail;

namespace {

// The device and, for a nested path without state, the AddMetaInstance of its default state (into `e`).
Result<const Device*> prepare(const Project& p, Edit& e, DeviceId device, const std::string& path, std::string_view type) {
  const DeviceLoc l = findDevice(p, device);
  if (!l.track) return notFound("device not found");
  e.devices.push_back(device);
  e.tracks.push_back(l.track->id);
  if (!path.empty() && l.device->metaAt(path) == nullptr) {
    const meta::Type* t = meta::findType(type);
    if (t == nullptr) return invalid("unknown meta module type '" + std::string(type) + "'");
    batchOf(e).commands.push_back(AddMetaInstance{device, path, defaultMetaInstance(*t)});
  }
  return l.device;
}

}  // namespace

EditResult setMetaParam(const Project& p, DeviceId device, std::string path, std::string_view type, std::string param,
                        std::optional<double> value) {
  Edit e = makeEdit("Meta Module Parameter");
  Result<const Device*> d = prepare(p, e, device, path, type);
  if (!d) return std::move(d).error();
  batchOf(e).commands.push_back(SetMetaParam{device, std::move(path), std::move(param), value});
  return e;
}

EditResult setMetaParamValue(const Project& p, DeviceId device, std::string path, std::string_view type, std::string param,
                             double value) {  // GU.1
  const DeviceLoc l = findDevice(p, device);
  if (!l.track) return notFound("device not found");
  const meta::Type* t = meta::findType(path.empty() ? metaDeviceType(*l.device) : type);
  if (t == nullptr) return notFound("unknown meta module type");
  const int32_t i = t->paramIndex(param);
  if (i < 0) return invalid("'" + param + "' is not a param of " + std::string(t->id));
  const auto k = static_cast<size_t>(i);
  const double now = metaParamPosition(*l.device, path, *t, k);
  const double pos = dsp::curvePosition(metaParamCurve(*l.device, path, *t, k), value, now);
  return setMetaParam(p, device, std::move(path), type, std::move(param), pos);
}

EditResult setMetaSetting(const Project& p, DeviceId device, std::string path, std::string_view type, std::string setting,
                          std::optional<double> value) {
  Edit e = makeEdit("Meta Module Setting");
  Result<const Device*> d = prepare(p, e, device, path, type);
  if (!d) return std::move(d).error();
  batchOf(e).commands.push_back(SetMetaSetting{device, std::move(path), std::move(setting), value});
  return e;
}

EditResult setMetaData(const Project& p, DeviceId device, std::string path, std::string_view type, meta::DataPtr data) {
  Edit e = makeEdit("Edit Meta Module Data");
  Result<const Device*> d = prepare(p, e, device, path, type);
  if (!d) return std::move(d).error();
  if (path.empty() && (*d)->metaAt("") == nullptr) {  // a chain device's "" entry (always made with it; restored here)
    const meta::Type* t = meta::findType(metaDeviceType(**d));
    if (t == nullptr) return invalid("not a meta device");
    batchOf(e).commands.push_back(AddMetaInstance{device, "", defaultMetaInstance(*t)});
  }
  batchOf(e).commands.push_back(SetMetaData{device, std::move(path), std::move(data)});
  return e;
}

// SM.3: a Sampler's sample (pq/document/edit_ops.h).
EditResult setSamplerSample(const Project& p, DeviceId sampler, SampleId sample, std::optional<double> rootHz,
                            std::vector<float> slices) {
  const DeviceLoc l = findDevice(p, sampler);
  if (!l.track) return notFound("device not found");
  auto isEngine = [](const Device& d) { return isMetaDevice(d) && metaDeviceType(d) == "sampler"; };
  DeviceId engine;
  if (isEngine(*l.device)) {
    engine = sampler;
  } else if (isGroupDevice(*l.device)) {
    forEachDeviceIn(l.track->devices, [&](const Device& d) {
      if (!engine.valid() && d.group == sampler && isEngine(d)) engine = d.id;
    });
  }
  if (!engine.valid()) return invalid("not a Sampler");
  if (sample.valid() && findSample(p, sample) == nullptr) return notFound("sample not found");
  for (const float f : slices)
    if (!(f >= 0.0f) || !std::isfinite(f)) return invalid("a slice must be a frame ≥ 0");
  std::sort(slices.begin(), slices.end());
  slices.erase(std::unique(slices.begin(), slices.end()), slices.end());
  const double hz = rootHz && *rootHz > 0.0 && std::isfinite(*rootHz) ? *rootHz : meta::kSamplerDefaultRoot;
  Result<Edit> data = setMetaData(p, engine, "", "sampler", meta::samplerData(sample.valid() ? sample.value : 0, std::move(slices)));
  if (!data) return std::move(data).error();
  Result<Edit> root = setMetaParamValue(p, engine, "", "sampler", "root", hz);
  if (!root) return std::move(root).error();
  Edit e = makeEdit("Set Sampler Sample");
  e.devices.push_back(sampler);
  e.tracks.push_back(l.track->id);
  for (Edit* x : {&*data, &*root})
    for (Command& c : batchOf(*x).commands) batchOf(e).commands.push_back(std::move(c));
  return e;
}

}  // namespace pq::doc::ops
