// MX.2: the Math module's op swap (pq/document/edit_ops.h "MX.2"; pq/document/modules.h "the Math module").
#include <string>
#include <vector>

#include "edit_common.h"
#include "edit_paths.h"
#include "pq/document/modules.h"

namespace pq::doc::ops {

using namespace detail;

EditResult setMathOp(const Project& p, IdSource& ids, DeviceId math, std::string_view key) {
  const std::optional<MathInfo> info = mathInfo(p, math);
  if (!info) return invalid("not a Math module");
  const MathOpInfo* op = findMathOp(key);
  if (op == nullptr) return invalid("no Math op '" + std::string(key) + "'");
  const Track* track = findDevice(p, math).track;
  Edit out = makeEdit("Math Op");
  Steps k(p);
  // The old op's blocks go (their wires with them: into the group's Out, from A / B / Tolerance), the new op's come.
  for (const DeviceId d : info->roles.op) PQ_DOC_TRY(k.add(RemoveDevice{d}));
  const BlockSet set = mathOpBlocks(ids, *op, math, info->roles.inA, info->roles.inB, info->roles.tolerance);
  for (const DevicePtr& d : set.devices) {
    PQ_DOC_TRY(k.add(AddDevice{track->id, d, kAppend}));
    out.devices.push_back(d->id);
  }
  for (const Wire& w : set.wires) PQ_DOC_TRY(k.add(AddWire{w}));
  PQ_DOC_TRY(k.add(SetDeviceSetting{math, std::string(kMathOpSetting), static_cast<double>(op->code)}));
  batchOf(out).commands = std::move(k.commands);
  return out;
}

}  // namespace pq::doc::ops
