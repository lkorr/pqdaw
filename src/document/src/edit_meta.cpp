// Meta-module edit builders (TMM.3): pq/document/edit_ops.h, "Meta modules".
#include <algorithm>

#include "edit_common.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

bool under(std::string_view path, std::string_view root) {
  return path == root || (path.size() > root.size() && path.starts_with(root) && path[root.size()] == '.');
}

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

// The device text edit (editDevicePatch) as the first commands of `e`.
Status textEdit(const Project& p, Edit& e, DeviceId device, std::string text, std::shared_ptr<const graph::PatchFile> ids) {
  EditResult t = editDevicePatch(p, device, std::move(text), std::move(ids));
  if (!t) return std::move(t).error();
  for (Command& c : batchOf(*t).commands) batchOf(e).commands.push_back(std::move(c));
  e.devices = t->devices;
  e.tracks = t->tracks;
  return ok();
}

bool holdsState(const Device& d, std::string_view root) {
  if (!d.meta) return false;
  return std::any_of(d.meta->begin(), d.meta->end(), [&](const MetaEntry& m) { return under(m.path, root); });
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

EditResult addMetaCall(const Project& p, DeviceId device, std::string text, std::string path, std::string_view type,
                       std::shared_ptr<const graph::PatchFile> ids) {
  const meta::Type* t = meta::findType(type);
  if (t == nullptr) return invalid("unknown meta module type '" + std::string(type) + "'");
  if (path.empty() || !meta::validPath(path)) return invalid("a meta call needs a name");
  Edit e = makeEdit("Add Meta Module");
  PQ_DOC_TRY(textEdit(p, e, device, std::move(text), std::move(ids)));
  const DeviceLoc l = findDevice(p, device);
  if (holdsState(*l.device, path))  // a stale state under that name (a deleted call's): the new call starts fresh
    batchOf(e).commands.push_back(RemoveMetaInstance{device, path});
  batchOf(e).commands.push_back(AddMetaInstance{device, std::move(path), defaultMetaInstance(*t)});
  return e;
}

EditResult renameMetaCall(const Project& p, DeviceId device, std::string text, std::string from, std::string to,
                          std::shared_ptr<const graph::PatchFile> ids) {
  Edit e = makeEdit("Rename Meta Module");
  PQ_DOC_TRY(textEdit(p, e, device, std::move(text), std::move(ids)));
  const DeviceLoc l = findDevice(p, device);
  if (from != to && holdsState(*l.device, from)) batchOf(e).commands.push_back(RenameMetaInstance{device, from, to});
  return e;
}

EditResult removeMetaCall(const Project& p, DeviceId device, std::string text, std::string path,
                          std::shared_ptr<const graph::PatchFile> ids) {
  Edit e = makeEdit("Delete Meta Module");
  PQ_DOC_TRY(textEdit(p, e, device, std::move(text), std::move(ids)));
  const DeviceLoc l = findDevice(p, device);
  if (l.device->meta)
    for (const MetaEntry& m : *l.device->meta)
      if (under(m.path, path)) batchOf(e).commands.push_back(RemoveMetaInstance{device, m.path});
  return e;
}

EditResult editDevicePatchKeepingMeta(const Project& p, DeviceId device, std::string text,
                                      std::shared_ptr<const graph::PatchFile> ids, const graph::PatchFile& previous) {
  const DeviceLoc l = findDevice(p, device);
  if (!l.track) return notFound("device not found");
  std::vector<MetaRename> renames;
  if (ids) renames = metaRenames(previous, *ids, l.device->patch);
  Edit e = makeEdit("Edit Device Patch");
  PQ_DOC_TRY(textEdit(p, e, device, std::move(text), std::move(ids)));
  // Applied in order (outer renames first, metaRenames): track where each state is after the earlier ones.
  std::vector<std::string> paths;
  if (l.device->meta)
    for (const MetaEntry& m : *l.device->meta) paths.push_back(m.path);
  for (const MetaRename& r : renames) {
    if (!std::any_of(paths.begin(), paths.end(), [&](const std::string& s) { return under(s, r.from); })) continue;
    if (std::any_of(paths.begin(), paths.end(), [&](const std::string& s) { return under(s, r.to); })) continue;
    batchOf(e).commands.push_back(RenameMetaInstance{device, r.from, r.to});
    for (std::string& s : paths)
      if (under(s, r.from)) s = r.to + s.substr(r.from.size());
  }
  return e;
}

}  // namespace pq::doc::ops
