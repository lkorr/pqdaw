#pragma once

// The Device view (T4.1, SPEC §9; QUESTIONS Q?-T4.1-a): the detail view's Device tab shows the selected track's device
// chain left → right — one T4.6 device panel per device (pq/views/devices/device_panel.h: Live-style frame, its panel
// description or the generated generic one) — and an add slot at the end. Every edit is a document command
// (pq/document/edit_ops.h), one undo step each:
//   add            the + slot or a right-click: a menu of the device catalog (instruments, audio effects; until M5's
//                  browser). An instrument replaces the track's instrument (Live).
//   reorder        drag a device's title bar left / right (a marker shows where it lands); within the chain only
//   remove         Delete (device.delete) or the context menu
//   deactivate     the panel's activator, `0` (device.toggleActive) or the context menu
//   copy / paste   Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+D (device.copy, .cut, .paste, .duplicate; an in-process clipboard)
//
// Each panel's DeviceTarget is the document's device instance: a param gesture (drag, type-in, reset) is one store
// gesture = one undo step of Param changes (the playback link writes params; no compile); a structural const is a
// SetDeviceSetting (one step; that track's unit recompiles); the activator is SetDeviceActive. Changes from elsewhere
// (undo, another view) come back through DevicePanel::setParamValue / setConstValue / setActive, which repaint only
// the controls bound to what changed.
//
// The view follows ViewContext::selectedTrack() (else the clip in the clip view's track) and the context's device
// library (ViewContext::setDeviceLibrary). T4.6's stand-alone preview (View > Open Device…, devices::DeviceView) shares
// the slot and covers the chain while a preview is open. UI thread only.

#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/model.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/view_context.h"

namespace pq::views {

class Shell;

class DeviceChainView : public ui::Widget {
public:
  explicit DeviceChainView(ViewContext& ctx);
  ~DeviceChainView() override;

  // Rebuilds every panel.
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
  // A device's frame, its panel and the panel's target (tests).
  ui::Widget* frameOf(doc::DeviceId device) const noexcept;
  devices::DevicePanel* panelOf(doc::DeviceId device) const noexcept;
  devices::DeviceTarget* targetOf(doc::DeviceId device) const noexcept;
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
