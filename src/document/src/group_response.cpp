// EQ.2: the analytic response of a group (pq/document/group_response.h).

#include "pq/document/group_response.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <unordered_map>

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
  GroupResponse* self = this;
  return prepareMany(project, lowered, devices, std::span<const DeviceId>(&group, 1), std::span<GroupResponse* const>(&self, 1))
      .front();
}

std::vector<Status> GroupResponse::prepareMany(const Project& project, const LoweredProject& lowered,
                                               const DeviceLibrary& devices, std::span<const DeviceId> groups,
                                               std::span<GroupResponse* const> out) {
  std::vector<Status> st(groups.size(), Status{Ok{}});
  // Each group's track patch and call node (kNone: none).
  struct Where {
    uint32_t track = graph::kNone;
    graph::NodeIndex call = graph::kNone;
  };
  std::vector<Where> at(groups.size());
  for (size_t k = 0; k < groups.size(); ++k) {
    GroupResponse& r = *out[k];
    r.response_ = graph::LinearResponse{};
    r.params_.clear();
    r.curves_.clear();
    r.driven_.clear();
    r.drivenBindings_.clear();
    ++r.stamp_;
    const DeviceId group = groups[k];
    const DeviceLoc loc = findDevice(project, group);
    if (loc.device == nullptr || !isGroupDevice(*loc.device)) {
      st[k] = Error{ErrorCode::InvalidArgument, "group response: device " + std::to_string(group.value) + " is not a group", {}};
      continue;
    }
    // The track patch the group's call is in (its own track's, or its host's for a hosted context).
    const graph::NodeId call{nodeIdFor(NodeRole::Device, group.value, 0)};
    for (uint32_t t = 0; t < lowered.patches.size() && at[k].track == graph::kNone; ++t) {
      if (lowered.patches[t] == nullptr) continue;
      const graph::Patch& p = lowered.patches[t]->patch;
      if (const auto n = p.indexOf(call); n && p.node(*n).kind == graph::NodeKind::Subcircuit) at[k] = {t, *n};
    }
    if (at[k].track == graph::kNone)
      st[k] = Error{ErrorCode::NotFound, "group response: group " + std::to_string(group.value) + " is not lowered", {}};
  }
  // One inference per track, with probes on every group's In and Out there.
  std::vector<uint8_t> done(groups.size(), 0);
  for (size_t k0 = 0; k0 < groups.size(); ++k0) {
    if (done[k0] || at[k0].track == graph::kNone) continue;
    const uint32_t track = at[k0].track;
    const graph::Patch& patch = lowered.patches[track]->patch;
    std::vector<size_t> members;
    std::vector<graph::ProbeTarget> probes;
    for (size_t k = k0; k < groups.size(); ++k) {
      if (done[k] || at[k].track != track) continue;
      done[k] = 1;
      const graph::NodeId call{nodeIdFor(NodeRole::Device, groups[k].value, 0)};
      const auto inPort = patch.findOutput(at[k].call, kGroupInPort);
      const auto outPort = patch.findOutput(at[k].call, "y");
      if (!inPort || !outPort) {
        st[k] = Error{ErrorCode::Unknown, "group response: the group call has no In / Out", {}};
        continue;
      }
      members.push_back(k);
      probes.push_back({{}, call, static_cast<uint16_t>(*inPort)});
      probes.push_back({{}, call, static_cast<uint16_t>(*outPort)});
    }
    if (members.empty()) continue;
    graph::InferOptions io;
    io.library = lowered.graph.library;
    io.voiceContext = track < lowered.graph.tracks.size() && lowered.graph.tracks[track].role == graph::TrackRole::Instrument;
    io.probes = probes;
    graph::InferResult inf = graph::inferDetailed(patch, io);
    for (size_t m = 0; m < members.size(); ++m) {
      const size_t k = members[m];
      if (!inf.ok()) {
        st[k] = Error{ErrorCode::Type, "group response: " + inf.diagnostics.front().message, {}};
        continue;
      }
      st[k] = out[k]->finish(project, lowered, devices, groups[k], inf, track, inf.probeSources[2 * m], inf.probeSources[2 * m + 1]);
    }
  }
  return st;
}

Status GroupResponse::finish(const Project& project, const LoweredProject& lowered, const DeviceLibrary& devices,
                             DeviceId group, const graph::InferResult& inf, uint32_t track, uint32_t inSource,
                             uint32_t outSource) {
  const graph::Patch& flat = inf.typed.patch;
  const graph::ResponseEnd src = endOf(flat, inSource);
  const graph::ResponseEnd sink = endOf(flat, outSource);
  if (src.node == graph::kNone)
    return Error{ErrorCode::NotFound, "group response: nothing reaches the In terminal of group " + std::to_string(group.value), {}};
  if (sink.node == graph::kNone)
    return Error{ErrorCode::NotFound, "group response: nothing reaches the Out terminal of group " + std::to_string(group.value), {}};
  // TX.mod-bands: every driven control's final value is a set port (its flat node: the one made from the CurveSite's
  // value node, in whichever instance the control's nodes were lowered in).
  std::unordered_map<uint32_t, size_t> siteOf;  // CurveSite::value → index into lowered.curves
  for (size_t k = 0; k < lowered.curves.size(); ++k)
    if (lowered.curves[k].value.valid()) siteOf.emplace(lowered.curves[k].value.value, k);
  std::vector<uint32_t> settable;
  std::vector<size_t> settableSite;
  if (!siteOf.empty())
    for (graph::NodeIndex n = 0; n < flat.nodeCount() && n < inf.typed.origin.size(); ++n) {
      const graph::NodeOrigin& o = inf.typed.origin[n];
      if (o.what != graph::NodeOrigin::What::Node || o.instance >= inf.typed.instances.size()) continue;
      const graph::Patch* ip = inf.typed.instances[o.instance].patch;
      if (ip == nullptr || o.node >= ip->nodeCount()) continue;
      const auto it = siteOf.find(ip->node(o.node).id.value);
      if (it == siteOf.end()) continue;
      const CurveSite& site = lowered.curves[it->second];
      if (site.valuePort >= flat.node(n).numOutputs) continue;
      settable.push_back(flat.flatOutput(n, site.valuePort));
      settableSite.push_back(it->second);
    }
  response_.prepare(inf.typed, src, sink, settable);

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
  for (uint32_t i = 0; i < response_.setPorts().size(); ++i) {
    const auto at = std::find(settable.begin(), settable.end(), response_.setPorts()[i]);
    if (at == settable.end()) continue;
    const CurveSite& site = lowered.curves[settableSite[static_cast<size_t>(at - settable.begin())]];
    DrivenControl c;
    c.index = i;
    c.device = site.device;
    c.param = site.param;
    if (!calleeDecl(c.device, c.param, c.decl)) continue;
    drivenBindings_.push_back(controlBinding(c.device, c.param));
    driven_.push_back(std::move(c));
  }
  update(project);
  return Ok{};
}

void GroupResponse::hold(DrivenControl& c, bool& changed) noexcept {
  const double pos = c.live >= 0.0 ? c.live : c.knob;
  const float v = static_cast<float>(dsp::evalCurve(c.curve, pos));
  if (c.held && response_.value(c.index) == v) return;
  c.held = true;
  response_.setValue(c.index, v);
  changed = true;
}

bool GroupResponse::setLive(uint32_t i, double position) noexcept {
  if (i >= driven_.size()) return false;
  DrivenControl& c = driven_[i];
  const double live = std::isfinite(position) && position >= 0.0 ? position : -1.0;
  if (live == c.live) return false;
  c.live = live;
  bool changed = false;
  hold(c, changed);
  if (changed) ++stamp_;
  return changed;
}

void GroupResponse::update(const Project& project) noexcept {
  bool changed = false;
  for (const Control& c : params_) {
    const DeviceLoc l = findDevice(project, c.device);
    if (l.device == nullptr) continue;
    const double v = c.position ? paramPosition(*l.device, c.param, c.decl) : paramValue(*l.device, c.param, c.decl);
    const float f = static_cast<float>(v);
    if (response_.param(c.index) == f) continue;
    response_.setParam(c.index, f);
    changed = true;
  }
  for (Control& c : curves_) {
    const DeviceLoc l = findDevice(project, c.device);
    if (l.device == nullptr) continue;
    const dsp::Curve k = controlCurve(*l.device, c.param, c.decl);
    if (c.curveSet && k == c.curve) continue;
    c.curve = k;
    c.curveSet = true;
    response_.setCurve(c.index, k);
    changed = true;
  }
  for (DrivenControl& c : driven_) {
    const DeviceLoc l = findDevice(project, c.device);
    if (l.device == nullptr) continue;
    c.curve = controlCurve(*l.device, c.param, c.decl);
    c.knob = paramPosition(*l.device, c.param, c.decl);
    hold(c, changed);
  }
  if (changed) ++stamp_;
}

}  // namespace pq::doc
