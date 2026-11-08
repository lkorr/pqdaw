// EQ.2: the analytic response of a group (pq/document/group_response.h).

#include "pq/document/group_response.h"

#include <charconv>

#include "pq/graph/infer.h"

namespace pq::doc {

namespace {

// The flattened node and output index of flat output port `f` (kNone when f is none).
graph::ResponseEnd endOf(const graph::Patch& p, uint32_t f) noexcept {
  if (f == graph::kNone) return {};
  for (graph::NodeIndex n = 0; n < p.nodeCount(); ++n) {
    const graph::Node& node = p.node(n);
    if (node.numOutputs == 0) continue;
    const uint32_t first = p.flatOutput(n, 0);
    if (f >= first && f < first + node.numOutputs) return {n, static_cast<uint16_t>(f - first), false};
  }
  return {};
}

// "d<id>__<name>" (lower.h "Rack chain params") → the device id and the param name.
bool deviceParam(std::string_view n, uint32_t& id, std::string_view& name) noexcept {
  if (n.size() < 4 || n[0] != 'd') return false;
  const size_t sep = n.find("__");
  if (sep == std::string_view::npos || sep < 2) return false;
  const auto [ptr, ec] = std::from_chars(n.data() + 1, n.data() + sep, id);
  if (ec != std::errc{} || ptr != n.data() + sep) return false;
  name = n.substr(sep + 2);
  return !name.empty();
}

}  // namespace

Status GroupResponse::prepare(const Project& project, const LoweredProject& lowered, const DeviceLibrary& devices,
                              DeviceId group) {
  response_ = graph::LinearResponse{};
  params_.clear();
  curves_.clear();
  const DeviceLoc loc = findDevice(project, group);
  if (loc.device == nullptr || !isGroupDevice(*loc.device))
    return Error{ErrorCode::InvalidArgument, "group response: device " + std::to_string(group.value) + " is not a group", {}};
  // The track patch the group's call is in (its own track's, or its host's for a hosted context).
  const graph::NodeId call{nodeIdFor(NodeRole::Device, group.value, 0)};
  uint32_t track = graph::kNone;
  graph::NodeIndex callNode = graph::kNone;
  for (uint32_t t = 0; t < lowered.patches.size() && track == graph::kNone; ++t) {
    if (lowered.patches[t] == nullptr) continue;
    const graph::Patch& p = lowered.patches[t]->patch;
    if (const auto n = p.indexOf(call); n && p.node(*n).kind == graph::NodeKind::Subcircuit) {
      track = t;
      callNode = *n;
    }
  }
  if (track == graph::kNone)
    return Error{ErrorCode::NotFound, "group response: group " + std::to_string(group.value) + " is not lowered", {}};
  const graph::Patch& patch = lowered.patches[track]->patch;
  const auto inPort = patch.findOutput(callNode, kGroupInPort);
  const auto outPort = patch.findOutput(callNode, "y");
  if (!inPort || !outPort) return Error{ErrorCode::Unknown, "group response: the group call has no In / Out", {}};
  const graph::ProbeTarget probes[] = {{{}, call, static_cast<uint16_t>(*inPort)}, {{}, call, static_cast<uint16_t>(*outPort)}};
  graph::InferOptions io;
  io.library = lowered.graph.library;
  io.voiceContext = track < lowered.graph.tracks.size() && lowered.graph.tracks[track].role == graph::TrackRole::Instrument;
  io.probes = probes;
  graph::InferResult inf = graph::inferDetailed(patch, io);
  if (!inf.ok()) return Error{ErrorCode::Type, "group response: " + inf.diagnostics.front().message, {}};
  const graph::Patch& flat = inf.typed.patch;
  const graph::ResponseEnd src = endOf(flat, inf.probeSources[0]);
  const graph::ResponseEnd sink = endOf(flat, inf.probeSources[1]);
  if (src.node == graph::kNone)
    return Error{ErrorCode::NotFound, "group response: nothing reaches the In terminal of group " + std::to_string(group.value), {}};
  if (sink.node == graph::kNone)
    return Error{ErrorCode::NotFound, "group response: nothing reaches the Out terminal of group " + std::to_string(group.value), {}};
  response_.prepare(inf.typed, src, sink);

  // The params the response reads: device params are re-read by update(); others keep the lowering's value.
  auto calleeDecl = [&](DeviceId d, std::string_view name, graph::ParamDecl& out) {
    const DeviceLoc l = findDevice(project, d);
    if (l.device == nullptr) return false;
    const graph::Patch* callee = devices.patchOf(*l.device);
    const graph::ParamDecl* decl = callee != nullptr ? findAnyParamDecl(*callee, name) : nullptr;
    if (decl == nullptr) return false;
    out = *decl;
    return true;
  };
  for (uint32_t i = 0; i < response_.paramNodes().size(); ++i) {
    const std::string& name = flat.nodeName(response_.paramNodes()[i]);
    for (const LoweredParam& lp : lowered.params)
      if (lp.track == track && lp.name == name) response_.setParam(i, lp.value);
    uint32_t id = 0;
    std::string_view pname;
    if (!deviceParam(name, id, pname)) continue;
    Control c;
    c.index = i;
    c.device = DeviceId{id};
    c.param = std::string(pname);
    if (!calleeDecl(c.device, pname, c.decl)) continue;
    for (const LoweredParam& lp : lowered.params)
      if (lp.track == track && lp.name == name) c.position = lp.position;
    params_.push_back(std::move(c));
  }
  // Driven controls' curve atoms: their slot names the control (LoweredProject::curves).
  for (uint32_t i = 0; i < response_.curveNodes().size(); ++i) {
    const graph::NodeIndex n = response_.curveNodes()[i];
    const auto slotIn = flat.findInput(n, "slot");
    if (!slotIn) continue;
    uint32_t slot = 0;
    for (const graph::Wire& w : flat.wires())
      if (w.to.node == n && w.to.port == *slotIn && flat.node(w.from.node).kind == graph::NodeKind::Const)
        slot = static_cast<uint32_t>(flat.constant(w.from.node).value());
    if (slot == 0) continue;
    for (const CurveSite& site : lowered.curves) {
      if (site.slot != slot) continue;
      Control c;
      c.index = i;
      c.device = site.device;
      c.param = site.param;
      if (!calleeDecl(c.device, c.param, c.decl)) break;
      curves_.push_back(std::move(c));
      break;
    }
  }
  update(project);
  return Ok{};
}

void GroupResponse::update(const Project& project) noexcept {
  for (const Control& c : params_) {
    const DeviceLoc l = findDevice(project, c.device);
    if (l.device == nullptr) continue;
    const double v = c.position ? paramPosition(*l.device, c.param, c.decl) : paramValue(*l.device, c.param, c.decl);
    response_.setParam(c.index, static_cast<float>(v));
  }
  for (const Control& c : curves_) {
    const DeviceLoc l = findDevice(project, c.device);
    if (l.device != nullptr) response_.setCurve(c.index, controlCurve(*l.device, c.param, c.decl));
  }
}

}  // namespace pq::doc
