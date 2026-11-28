// FD.2: the lane modules' N (pq/document/edit_ops.h "FD.2"; pq/document/modules.h "lane cables").
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "edit_common.h"
#include "edit_paths.h"
#include "pq/document/modules.h"

namespace pq::doc::ops {

using namespace detail;

EditResult setLaneCount(const Project& p, IdSource& ids, DeviceId module, uint32_t lanes) {
  const std::optional<LaneModuleInfo> info = laneModuleInfo(p, module);
  if (!info) return invalid("not a lane module (Lanes, Merge, Unpack, Pack)");
  if (!laneCountValid(lanes)) return invalid("a lane cable has 2, 4, 8 or 16 lanes");
  const Track* track = findDevice(p, module).track;
  Edit out = makeEdit("Lane Count");
  Steps k(p);
  // The lane blocks that stay keep their cables (Unpack's outputs, Pack's inputs); everything else inside is rebuilt.
  const bool jacks = info->kind == LaneModule::Unpack || info->kind == LaneModule::Pack;
  std::vector<DeviceId> keep;
  if (jacks) keep.assign(info->roles.lane.begin(), info->roles.lane.begin() + std::min<size_t>(info->roles.lane.size(), lanes));
  for (const DeviceId d : info->roles.lane)
    if (std::find(keep.begin(), keep.end(), d) == keep.end()) PQ_DOC_TRY(k.add(RemoveDevice{d}));
  for (const DeviceId d : info->roles.inner) PQ_DOC_TRY(k.add(RemoveDevice{d}));
  LaneModuleRoles roles;
  const BlockSet set = laneModuleInside(ids, info->kind, module, lanes, keep, &roles);
  for (const DevicePtr& d : set.devices) {
    PQ_DOC_TRY(k.add(AddDevice{track->id, d, kAppend}));
    out.devices.push_back(d->id);
  }
  for (const Wire& w : set.wires) PQ_DOC_TRY(k.add(AddWire{w}));
  PQ_DOC_TRY(k.add(SetDeviceSetting{module, std::string(kLaneCountSetting), static_cast<double>(lanes)}));
  PQ_DOC_TRY(k.add(SetGroupPanel{module, std::make_shared<const Panel>(laneModulePanel(info->kind, module, lanes, roles.lane))}));
  batchOf(out).commands = std::move(k.commands);
  return out;
}

}  // namespace pq::doc::ops
