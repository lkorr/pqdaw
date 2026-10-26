#pragma once

// The Device view (T4.1, SPEC §9; QUESTIONS Q?-T4.1-a): the detail view's Device tab shows the selected track's device
// chain left → right, one frame per device (title bar with the activator, then the device's panel), and an add slot at
// the end. Every edit is a document command (pq/document/edit_ops.h), one undo step each:
//   add            the + slot or a right-click: a menu of the device catalog (instruments, audio effects; until M5's
//                  browser). An instrument replaces the track's instrument (Live).
//   reorder        drag a device's title bar left / right (a marker shows where it lands); within the chain only
//   remove         Delete (device.delete) or the context menu
//   deactivate     the activator in the title bar, `0` (device.toggleActive) or the context menu
//   copy / paste   Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+D (device.copy, .cut, .paste, .duplicate; an in-process clipboard)
//   param values   the panel's controls: a drag / type-in / reset is one store gesture = one undo step; the playback link
//                  turns it into param writes (no compile)
//   settings       structural constants (`real const` inputs, Q121): applied when the gesture ends (one recompile of
//                  the track's unit, never one per drag step)
//
// ---- The panel hook (for T4.6's device panels) -----------------------------------------------------------------------
// A device's panel is a DevicePanel built by the factory registered for its patch in the view's DevicePanelRegistry
// (`panels().add("utility", factory)`); devices without one get the generic panel (makeGenericPanel: one control per
// param, settings first). A panel edits through its DeviceHost (one per device frame): param gestures and settings as
// document commands, and reads the catalog entry and the device's current values. The frame calls refresh() when the
// device changed in the document and paramsChanged() when only some of its params did (a knob drag, an undo), so a
// panel repaints only the controls whose value moved. T4.6 may replace the frame's look too: the frame only lays out
// the title bar and gives the panel the rest.
//
// The view follows ViewContext::selectedTrack() (else the clip in the clip view's track) and the context's device
// library (ViewContext::setDeviceLibrary). Idle: no frames or timers; a param change repaints only its control.
// UI thread only.

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/model.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"
#include "pq/views/view_context.h"

namespace pq::views {

class Shell;

// What a device's panel edits through and reads from (implemented by the device view, one per device frame).
class DeviceHost {
public:
  virtual ~DeviceHost() = default;
  virtual ViewContext& context() noexcept = 0;
  virtual doc::DeviceId device() const noexcept = 0;
  // The catalog entry of the device's patch (null: the patch is not in the library; the device is bypassed).
  virtual const doc::DeviceInfo* info() const noexcept = 0;
  // The device as in the current snapshot (null once it is gone).
  virtual const doc::Device* current() const noexcept = 0;
  // A param edit as one undo step: beginParam, then setParam per change (each is applied at once: live param writes),
  // then endParam (commit) or cancelParam (Esc: back to the value before). setParam outside a gesture is one step on
  // its own. `value` is in the param's normalised unit (DeviceParamInfo); nullopt = back to the patch's default.
  virtual void beginParam(std::string_view param) = 0;
  virtual void setParam(std::string_view param, std::optional<double> value) = 0;
  virtual void endParam() = 0;
  virtual void cancelParam() = 0;
  // A structural setting (one undo step; the track's unit recompiles). nullopt = the patch's default.
  virtual void setSetting(std::string_view setting, std::optional<double> value) = 0;
};

// The body of a device frame. Lives as long as its frame (a chain edit that keeps the device keeps its panel).
class DevicePanel : public ui::Widget {
public:
  explicit DevicePanel(std::string name) : Widget(std::move(name)) {}
  // The device changed in the document (values, settings, activation): show `device`.
  virtual void refresh(const doc::Device& device) = 0;
  // Only these params of the device changed (a Param ChangeSet): repaint just their controls. Default: refresh.
  virtual void paramsChanged(const doc::Device& device, std::span<const doc::ParamRef> params) {
    (void)params;
    refresh(device);
  }
  // The width (logical px) the panel wants at `height` logical px (the frame adds nothing horizontally).
  virtual float preferredWidth(float height) const = 0;
};

using DevicePanelFactory = std::function<std::unique_ptr<DevicePanel>(DeviceHost& host)>;

// Patch name → panel factory (T4.6 registers its stock panels here).
class DevicePanelRegistry {
public:
  void add(std::string patch, DevicePanelFactory factory) { factories_[std::move(patch)] = std::move(factory); }
  const DevicePanelFactory* find(std::string_view patch) const noexcept {
    const auto it = factories_.find(std::string(patch));
    return it == factories_.end() ? nullptr : &it->second;
  }

private:
  std::unordered_map<std::string, DevicePanelFactory> factories_;
};

// The generic panel: one control per param (a number box per real param in its display unit — dB, Hz, %, ms —
// logarithmic travel for @curve(exp), stepped for @steps(n); a toggle per bool), the structural settings first (a
// stepped box each, applied when its gesture ends). Arranged in columns that fill the chain's height.
std::unique_ptr<DevicePanel> makeGenericPanel(DeviceHost& host);

class DeviceChainView : public ui::Widget {
public:
  explicit DeviceChainView(ViewContext& ctx);
  ~DeviceChainView() override;

  DevicePanelRegistry& panels() noexcept { return panels_; }
  // Rebuilds every frame (after registering panels).
  void rebuild();

  // The track whose chain is shown (invalid: none) and the selected devices.
  doc::TrackId track() const noexcept { return track_; }
  std::span<const doc::DeviceId> selection() const noexcept { return selection_; }
  void select(std::span<const doc::DeviceId> devices);

  // The edits the keys, menus and drags run (each one undo step; false when there was nothing to do).
  bool addDevice(const doc::DeviceInfo& info);  // after the selected device, else at the end
  bool deleteSelected();
  bool copySelected();
  bool cutSelected();
  bool paste();  // after the selected device, else at the end
  bool duplicateSelected();
  bool toggleSelectedActive();
  bool moveDevice(doc::DeviceId device, uint32_t index);  // index in the chain after removal

  // The device menu (the + slot, a right-click on empty space): the catalog's instruments and audio effects.
  std::vector<ui::MenuItem> deviceMenu();
  // The frame of a device (tests): its rect, its panel.
  ui::Widget* frameOf(doc::DeviceId device) const noexcept;
  DevicePanel* panelOf(doc::DeviceId device) const noexcept;
  ui::Widget* addSlot() const noexcept { return addSlot_; }

  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;

  struct Impl;

private:
  friend struct Impl;
  void onStore(const doc::StoreEvent& e);
  void follow();  // the track to show changed?
  doc::TrackId wantedTrack() const;
  void sync(bool force);  // frames ↔ the shown track's chain

  ViewContext& ctx_;
  DevicePanelRegistry panels_;
  std::unique_ptr<Impl> impl_;
  ViewContext::Subscription sub_, observer_;
  doc::TrackId track_;
  std::vector<doc::DeviceId> selection_;
  std::vector<doc::DevicePtr> clipboard_;
  ui::Widget* addSlot_ = nullptr;
};

// Installs the Device view into ShellSlot::DeviceDetail and its view actions (device.*).
DeviceChainView& installDeviceChain(Shell& shell);
DeviceChainView* findDeviceChain(Shell& shell) noexcept;

}  // namespace pq::views
