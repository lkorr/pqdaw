#pragma once

// T4.18 / TL.6: AUTOMATED CONTROLS FOLLOW THE PLAYHEAD (Live: an automated knob turns by itself while the song plays,
// and shows the envelope wherever the playhead is located). The one mechanism the views showing controls share — the
// Device view (pq/views/device_chain/device_chain.h) and the project patch (band) view
// (pq/views/project_patch/project_patch_view.h):
//
//   collect   which params are automated: the lanes with points on the tracks a view shows (device params; with
//             trackParams also the mixer's volume, pan and sends), each with its state — overridden by hand
//             (ViewContext::automationOverridden: the control shows and plays its own value until Re-enable) or
//             macro-mapped (a device param a rack's macro drives plays the macro, not its lane: left to the macro).
//             Redone when the document or the overrides change (view_change::kAutomation), never per frame.
//   valueAt   the envelope at the playhead (pq/core/automation_curve.h), in the lane's units: a device param's knob
//             position (GU.1), dB for volume / sends, −1 … 1 for pan.
//   poll      a 30 Hz timer on the view, armed only while a following control is shown and the transport plays (a
//             located playhead while stopped arrives as view_change::kTransport); idle otherwise.
//
// What a view does with the values is its own: it shows them on its controls (PanelTarget::showPosition,
// MetaTarget::showPosition, the mixer face) and damages only the controls that moved.
//
// UI thread only.

#include <cstdint>
#include <string>
#include <vector>

#include "pq/document/model.h"

namespace pq::ui {
class Widget;
}

namespace pq::views {

class ViewContext;

class AutomationFollow {
public:
  struct Target {
    doc::ParamRef param;
    std::string name;         // a device param's name (DeviceParam), else ""
    doc::LanePtr lane;        // held: the envelope stays valid while the snapshot changes under it
    bool overridden = false;  // grabbed by hand: its control shows its own value
    bool mapped = false;      // a macro-mapped device param: its control is left to the macro
    bool follows() const noexcept { return !overridden && !mapped; }
  };

  static constexpr double kPollSeconds = 1.0 / 30.0;

  // Forgets the targets (keeps the storage).
  void clear() noexcept { targets_.clear(); }
  // Adds the automated params whose lanes are on track `t`: device params (lanes live on the device's track: laneHome)
  // and, with `trackParams`, t's mixer volume, pan and sends.
  void collect(const ViewContext& ctx, const doc::Track& t, bool trackParams);
  const std::vector<Target>& targets() const noexcept { return targets_; }
  bool empty() const noexcept { return targets_.empty(); }
  // The index of the target driving `p` (DeviceParam: by owner and name hash), -1: none.
  int32_t find(const doc::ParamRef& p) const noexcept;

  // The envelope of `t` at `tick` (`own`: an empty lane's value), a device param's position clamped to [0, 1].
  static double valueAt(const Target& t, double tick, double own) noexcept;

  // Arms (want) or cancels the poll's timer on `w`.
  void poll(ui::Widget& w, bool want);
  bool polling() const noexcept { return timer_ != 0; }
  // Whether `id` is the poll's timer; it fired, so it is disarmed (the caller applies, which re-arms it if wanted).
  bool fired(uint32_t id) noexcept {
    if (id == 0 || id != timer_) return false;
    timer_ = 0;
    return true;
  }

private:
  std::vector<Target> targets_;
  uint32_t timer_ = 0;
};

}  // namespace pq::views
