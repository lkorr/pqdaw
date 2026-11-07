// Meta-module edit builders (TMM.3): pq/document/edit_ops.h, "Meta modules".
#include "edit_common.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"

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

}  // namespace pq::doc::ops
