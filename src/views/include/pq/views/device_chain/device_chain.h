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
  // T4.8: Ctrl+G — the selected devices (of one list) into a new rack (pq/document/edit_ops.h groupDevices).
  bool groupSelected();
  // ---- T5.11: the user library (pq/views/browser/library_load.h) ---------------------------------------------------
  // The device's context menu: "Save Preset" (the browser then renames it inline), "Save as Default Preset" (a new
  // device of its kind starts from it), "Save as New Device" (an edited device joins the device menu and the browser
  // for every project). "Hot-Swap Presets…" and a click on the title bar's preset slot put the browser in hot-swap mode
  // for the device. Presets, user devices and catalog devices dragged from the browser (or Explorer) onto the chain load
  // after the selected device (one undo step each).
  bool savePreset(doc::DeviceId device, bool asDefault);
  bool saveAsNewDevice(doc::DeviceId device);
  bool hotSwap(doc::DeviceId device);
  ui::DropFeedback dragOver(const ui::DragPayload& p, ui::Vec2 pos, ui::Mods mods) override;
  ui::DropEffect drop(const ui::DragPayload& p, ui::Vec2 pos, ui::Mods mods) override;
  // T4.17: Ctrl+Shift+G: the selected rack (one chain) becomes its devices in place; Ctrl+R: renames the selected device
  // in its title bar (Enter: one undo step). False when there is nothing to do (the reason in the status line).
  bool ungroupSelected();
  bool renameSelected();
  // M4 review UI item 6 (Create menu): an empty Instrument / Audio Effect Rack after the selected device, else at the end.
  bool addRack(bool instrument);
  // ---- T4.16c: keyboard (Live's Device view; docs/HOTKEYS.md) -------------------------------------------------------
  // Left / Right (device.selectPrev / Next): the neighbouring device in display order — a rack, then the devices of the
  // chain it shows, then what follows the rack — so the arrows walk into and out of rack chains; nothing selected: the
  // first / last. Shift (device.extendPrev / Next): adds the neighbour in the lead device's own list to the selection (or
  // steps back out of it), the devices Ctrl+G groups. Home / End: the first / last device of the selected device's list
  // (else the track's chain). Ctrl+Left / Right (device.moveLeft / Right): moves the one selected device a step along its
  // list (one undo step; the instrument stays first). Up / Down (device.chainUp / Down): the selected rack, or the rack
  // around the selected device, shows its previous / next chain. The selection is scrolled into view.
  bool selectNeighbour(int32_t dir, bool extend);
  bool selectEnd(bool last);
  bool moveSelected(int32_t dir);
  bool showNeighbourChain(int32_t dir);
  std::vector<doc::DeviceId> shownOrder() const;  // every device shown, in display order
  void revealDevice(doc::DeviceId device);        // scrolls the chain so the device's frame is in view
  // T4.8: opens a rack's lowered subcircuit in the circuit viewer (read-only; rebuilt while open after a structural
  // or meta edit of the rack, closed when the rack is removed; rack_frame.cpp, Q?-T4.8-a).
  bool openRackCircuit(doc::DeviceId rack);

  // ---- T4.8: racks (rack_frame.cpp) --------------------------------------------------------------------------------
  // The chain a rack's frame shows (invalid: none; its first chain by default) and showing another one.
  doc::ChainId shownChain(doc::DeviceId rack) const;
  void showChain(doc::DeviceId rack, doc::ChainId chain);
  // Macro map mode (Live's Map): on for `rack` (invalid: off). While on, a click on one of the rack's macros picks it;
  // then a click on a knob of a device inside the rack (or on the rack's chain selector) maps it to that macro (or
  // unmaps it when it already is), one undo step each.
  void setMapMode(doc::DeviceId rack);
  doc::DeviceId mapRack() const noexcept;
  int32_t mapMacro() const noexcept;
  void pickMacro(int32_t macro);
  // Map mode: maps `device`.`param` to the picked macro (as the overlay's click does).
  bool mapPicked(doc::DeviceId device, std::string_view param);
  // The zone editor of a rack: shown (with the Key / Velocity / Chain tab) or hidden.
  void showZones(doc::DeviceId rack, std::optional<doc::ZoneKind> tab);
  std::optional<doc::ZoneKind> zonesShown(doc::DeviceId rack) const;
  bool onKey(const ui::KeyEvent& e) override;

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

  // M4 review M9: rack frames show their chain's devices inline down to this many nested racks; a deeper rack shows its
  // strip, macros and chain list but not its chain's devices, so the widget tree stays shallow however deep racks nest.
  static constexpr uint32_t kMaxNestedRackFrames = 24;

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
