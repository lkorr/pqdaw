#pragma once

// The Device view (T4.1, SPEC §9; QUESTIONS Q129): the detail view's Device tab shows the selected track's device
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

#include <functional>
#include <map>
#include <memory>
#include <optional>
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
  // T4.4-fix1 (SPEC §15 2026-10-26): the device's context menu "Open in Circuit View" (also a double-click on its
  // title: pq/views/circuit/device_circuit.h) and "Revert to Stock" (the selected user devices back to their stock
  // patch: one undo step). An edited device's title shows an "edited" marker.
  bool openInCircuitView(doc::DeviceId device);
  bool revertSelectedToStock();

  // The device menu (the + slot, a right-click on empty space): the catalog's instruments and audio effects.
  std::vector<ui::MenuItem> deviceMenu();
  // A device's frame, its panel and the panel's target (tests).
  ui::Widget* frameOf(doc::DeviceId device) const noexcept;
  devices::DevicePanel* panelOf(doc::DeviceId device) const noexcept;
  devices::DeviceTarget* targetOf(doc::DeviceId device) const noexcept;
  ui::Widget* addSlot() const noexcept { return addSlot_; }

  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onTimer(uint32_t id) override;

  // ---- T4.7: cross-device wiring (pq/views/device_chain/wiring.h) --------------------------------------------------
  // A jack press (Down) and the pointer events of the drag that follows (Move, Up); installWiring sets it.
  std::function<void(doc::DeviceId device, std::string_view port, const ui::PointerEvent& e)> onJack;
  struct ParamControl {
    doc::DeviceId device;
    std::string param;
    ui::IRect rect;  // device px
  };
  // The controls a cable can be dropped on now: the knobs and boxes of real (not bool) params, visible in the chain.
  std::vector<ParamControl> paramControls() const;
  // Device px: a device's jack centre / the top centre of the control bound to one of its params. nullopt: the device
  // is not in the shown chain. `docked` false: it is, but the point is scrolled away or its control is hidden (a
  // folded panel, another tab): the nearest point of its frame inside the chain's viewport.
  std::optional<ui::Vec2> jackPoint(doc::DeviceId device, std::string_view port, bool* docked = nullptr) const;
  std::optional<ui::Vec2> paramPoint(doc::DeviceId device, std::string_view param, bool* docked = nullptr) const;
  ui::IRect chainViewport() const noexcept;  // the chain's visible area
  void scrollChainBy(int32_t dx);
  // The knobs' modulation rings: static ranges from the document's wires, live sweeps from the context's
  // ModulationSource (polled at 30 Hz only while it is live and a modulated knob is shown).
  void refreshRings();
  bool ringsPolling() const noexcept;
  // Reads the sweeps once and updates the rings (the 30 Hz timer's work; tests and scenes call it directly).
  void pollRings();

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
