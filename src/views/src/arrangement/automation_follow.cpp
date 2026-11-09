// T4.18 / TL.6: automated controls follow the playhead — the shared mechanism (pq/views/arrangement/automation_follow.h).

#include "pq/views/arrangement/automation_follow.h"

#include <algorithm>
#include <span>

#include "pq/core/automation_curve.h"
#include "pq/document/racks.h"
#include "pq/ui/widget.h"
#include "pq/views/view_context.h"

namespace pq::views::device_chain {
// device_chain.cpp: the macro mapping onto device.param (its rack, the mapping); nullopt: none.
std::optional<std::pair<doc::RackInfo, doc::RackMapping>> macroMappingOf(const doc::Project& p, doc::DeviceId device, std::string_view param);
}  // namespace pq::views::device_chain

namespace pq::views {

void AutomationFollow::collect(const ViewContext& ctx, const doc::Track& t, bool trackParams) {
  const doc::Project& p = *ctx.doc();
  for (const doc::LanePtr& l : t.lanes) {
    if (l->points.empty()) continue;
    const doc::ParamKind k = l->param.kind;
    const bool device = k == doc::ParamKind::DeviceParam;
    const bool mixer = k == doc::ParamKind::TrackVolume || k == doc::ParamKind::TrackPan || k == doc::ParamKind::TrackSend;
    if (!device && !(trackParams && mixer)) continue;
    Target& x = targets_.emplace_back();
    x.param = l->param;
    if (device) x.name = l->paramName;
    x.lane = l;
    x.overridden = ctx.automationOverridden(l->param);
    // A macro-mapped param plays its macro, not its lane (lower.h).
    x.mapped = device && device_chain::macroMappingOf(p, doc::DeviceId{l->param.owner}, l->paramName).has_value();
  }
}

int32_t AutomationFollow::find(const doc::ParamRef& p) const noexcept {
  for (size_t i = 0; i < targets_.size(); ++i)
    if (targets_[i].param == p) return static_cast<int32_t>(i);
  return -1;
}

double AutomationFollow::valueAt(const Target& t, double tick, double own) noexcept {
  const double v = evalAutomation(std::span<const doc::Breakpoint>(t.lane->points), tick, own);
  return t.param.kind == doc::ParamKind::DeviceParam ? std::clamp(v, 0.0, 1.0) : v;  // GU.1: a knob position
}

void AutomationFollow::poll(ui::Widget& w, bool want) {
  if (want && timer_ == 0) timer_ = w.requestTimer(kPollSeconds);
  if (!want && timer_ != 0) {
    w.cancelTimer(timer_);
    timer_ = 0;
  }
}

}  // namespace pq::views
