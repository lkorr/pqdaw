// MX.2: the Math module's op swap (pq/document/edit_ops.h "MX.2"; pq/document/modules.h "the Math module").
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "edit_common.h"
#include "edit_paths.h"
#include "pq/document/modules.h"
#include "pq/dsp/curve.h"

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
  // MX.4 (Q?-BR.B-b): B's typed value follows the op's default while it is still the old op's (a value the user set is
  // kept). Compared in units, to a hair (a position round trip through the curve is not exact).
  if (const Device* b = info->roles.b.valid() ? findDevice(p, info->roles.b).device : nullptr;
      b != nullptr && info->op != nullptr && info->op->bDefault != op->bDefault && b->curve(kValueParam) != nullptr) {
    const double now = valueBlockUnits(*b), was = info->op->bDefault;
    if (std::fabs(now - was) <= 1e-6 * std::max(1.0, std::fabs(was))) {
      const dsp::Curve& c = *b->curve(kValueParam);
      const DeviceValue* pos = b->param(kValueParam);
      const double to = dsp::curvePosition(c, dsp::curveOutput(c, op->bDefault), pos != nullptr ? pos->value : 0.0);
      PQ_DOC_TRY(k.add(SetDeviceParam{b->id, std::string(kValueParam), std::clamp(to, 0.0, 1.0)}));
    }
  }
  batchOf(out).commands = std::move(k.commands);
  return out;
}

}  // namespace pq::doc::ops
