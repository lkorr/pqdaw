#pragma once

// Private to the device view (T4.1; racks T4.8): its sizes (logical px), the view's shared state (Impl) and the frames
// it shows — a device's panel (DeviceFrame, device_chain.cpp) or a rack (RackFrame, rack_frame.cpp: its macro panel, its
// chain list with the chain mixer and the zone editor, and the frames of the chain it shows, nested to any depth).

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../common/drag.h"
#include "../common/panel_template.h"
#include "pq/document/edit_ops.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/devices/panel_desc.h"

namespace pq::views {

namespace project_patch {
class ProjectPatchView;
}

namespace device_chain {

inline constexpr float kTitleH = 20.0f;    // a device panel's title bar (T4.6's frame)
inline constexpr float kFrameGap = 4.0f;   // between frames
inline constexpr float kChainPad = 4.0f;   // around the chain
inline constexpr float kAddW = 132.0f;     // the add slot at the end of the chain

class FrameBase;
class FreeRackPanel;

// Where a dragged device would land (T4.8: a rack's chain list or the chain a rack shows, besides the track's chain).
struct DropTarget {
  // TX.free-rack: Free = onto the free-device rack (out of the chain).
  enum class Kind : uint8_t { None, Track, Chain, NewChain, Free };
  Kind kind = Kind::None;
  uint32_t index = 0;   // Track / Chain: the position among the list's devices (the dragged one counted, if it is there)
  doc::ChainId chain;   // Chain
  doc::DeviceId rack;   // NewChain: the rack that gets a new chain
  ui::IRect marker;     // device px: what paintOver draws (a line, or a row outline)
  bool row = false;     // the marker outlines a row (a chain) instead of being an insertion line
  // T4.16c: the drop would be refused (a rack into itself or a rack inside it, an instrument into an Audio Effect
  // Rack): no marker, the NotAllowed cursor, nothing happens on release (the status line says why).
  const char* refused = nullptr;
};

}  // namespace device_chain

struct DeviceChainView::Impl {
  DeviceChainView& view;
  ui::ScrollView* scroll = nullptr;
  ui::Widget* content = nullptr;
  std::vector<device_chain::FrameBase*> frames;       // the track's chain (top level)
  // TMM.13-fix2 (MM review H1): frames a sync replaced, kept alive until the next frame. A sync runs from a store
  // listener, i.e. inside whatever dispatched the edit, and that can be a key handler of the frame's own embedded
  // ProjectPatchView (Delete / Ctrl+Shift+G in a group's panel): destroying the frame there returned into freed memory.
  std::vector<std::unique_ptr<ui::Widget>> retiredFrames;
  std::vector<doc::DevicePtr> shown;                  // what each top-level frame shows (pointer-equal while unchanged)
  doc::Snapshot shownFrom;                            // keeps `shown` alive
  std::map<uint32_t, device_chain::FrameBase*> all;   // T4.8: every frame shown (rack contents too), by device id
  // Reordering by dragging a title bar (T4.8: into and out of rack chains too).
  drag::Tracker drag;
  doc::DeviceId dragging;
  device_chain::DropTarget drop;
  // T4.8: per rack (by device id): the chain its frame shows, its zone editor's tab (kZonesHidden: hidden).
  static constexpr uint8_t kZonesHidden = 0xFF;
  std::map<uint32_t, doc::ChainId> shownChain;
  std::map<uint32_t, uint8_t> zoneTab;
  // M4 review M9 (DeviceChainView::kMaxNestedRackFrames): the nesting of the rack frame being built (its ctor reads it).
  uint32_t nestingBuilt = 0;
  // T4.8: macro map mode — the rack whose Map is on, and the macro picked (−1: none yet).
  doc::DeviceId mapRack;
  int32_t mapMacro = -1;
  ui::Widget* mapOverlay = nullptr;

  explicit Impl(DeviceChainView& v) : view(v) {}
  ViewContext& ctx() const noexcept { return view.ctx_; }
  const doc::Project& doc() const noexcept { return *view.ctx_.doc(); }
  const doc::DeviceCatalog* catalog() const noexcept {
    const doc::DeviceLibrary* lib = view.ctx_.deviceLibrary();
    return lib != nullptr ? &lib->catalog() : nullptr;
  }
  const doc::Track* track() const noexcept {
    return view.track_.valid() ? doc::findTrackPtr(*view.ctx_.doc(), view.track_) : nullptr;
  }
  // Runs an edit builder's result as one undo step; false when there was nothing to do.
  bool run(const doc::ops::EditResult& e, bool selectResult = true);
  // Inside a store gesture (a drag): applies the edit at once (Live: values follow the pointer), one undo step at the end.
  void live(const doc::ops::EditResult& e);
  int32_t frameIndex(doc::DeviceId id) const noexcept;  // among the top-level frames
  device_chain::FrameBase* frameOf(doc::DeviceId id) const noexcept;  // any level
  // One panel template per device patch (signature + description), built on first use. T4.4-fix1: keyed by what the
  // device plays — the stock patch's name, or a user device's own root (its name carries the device id and its graph's
  // hash: a new graph, a new template); "" for a device with nothing to play.
  std::map<std::string, std::unique_ptr<PanelTemplate>, std::less<>> templates;
  static std::string templateKey(const doc::DeviceLibrary* lib, const doc::Device& d);
  const PanelTemplate* templateFor(const doc::Device& d);
  void startDrag(doc::DeviceId id, const ui::PointerEvent& e);
  void moveDrag(const ui::PointerEvent& e);
  void endDrag(bool commit);
  // T4.8: rebuilds `all` from the frames (after any frame was added or removed).
  void reindex();
  // T4.8: the device menu for a list: the catalog's instruments (where they go) and audio effects, and the racks; a
  // pick adds into `track`'s chain (chain invalid) or into `chain` at `index`.
  std::vector<ui::MenuItem> deviceMenu(doc::TrackId track, doc::ChainId chain, uint32_t index, bool instruments);
  // T4.8: map mode.
  void setMapMode(doc::DeviceId rack);  // invalid: off
  // T4.7: the modulated params of the shown chain, the knob travel their wires sum to, their last sweeps.
  std::vector<ModTarget> ringTargets;
  std::vector<float> ringSpan;
  std::vector<uint8_t> ringUnits;  // TX.knob-curves: what the sweep is in (device_chain.cpp ringLaw: 0 travel, 1 Add, 2 Units)
  std::vector<ModSweep> ringSweeps;
  uint32_t ringTimer = 0;
  bool ringsLit = false;
  // T4.18: the automated params of the shown devices (a lane with points, on the shown track) and the 30 Hz timer
  // that moves their knobs to the envelope at the playhead while the transport plays.
  struct AutoTarget {
    doc::DeviceId device;
    std::string param;
    doc::LanePtr lane;
    bool overridden = false;
  };
  std::vector<AutoTarget> autoTargets;
  bool chainAuto = false;  // a rack chain's volume / pan lane on the shown track
  uint32_t autoTimer = 0;
  // TX.groups: a group panel's width (logical px) after its right edge was dragged, by group id (this session).
  std::map<uint32_t, float> groupWidths;
  // TX.free-rack: the free-device rack over the chain (free_rack.cpp), and a block of it dragged over the chain (the
  // drop marker shows where it lands; `drop` holds it).
  device_chain::FreeRackPanel* freeRack = nullptr;
  doc::DeviceId rackDragging;
  // TX.free-rack: per shown device (by id), its non-const inputs (not params) and outputs (outs, then exports), from
  // the patch it plays: where cables enter and leave its frame. Filled in sync().
  struct Ports {
    std::vector<std::string> inputs, outputs;
  };
  std::map<uint32_t, Ports> ports;
  const Ports* portsOf(doc::DeviceId id) const noexcept {
    const auto it = ports.find(id.value);
    return it == ports.end() ? nullptr : &it->second;
  }
  void refreshPorts();
  // The drop slot (and its marker) of the track's chain for a device dragged at `pos` (`dragged`: its frame, if it is
  // one of the chain's).
  device_chain::DropTarget trackSlot(ui::Vec2 pos, doc::DeviceId dragged) const;
};

namespace device_chain {

// What the chain shows for one device: its panel (DeviceFrame) or a rack (RackFrame).
class FrameBase : public ui::Widget {
public:
  FrameBase(std::string name, DeviceChainView::Impl& impl, ViewContext& ctx, doc::DeviceId id)
      : Widget(std::move(name)), impl_(impl), ctx_(ctx), id_(id) {}
  doc::DeviceId id() const noexcept { return id_; }
  virtual float logicalWidth() const = 0;
  virtual float logicalHeight(float available) const = 0;
  // TMM.10: the width in a chain `available` logical px tall (a face panel scales with the height).
  virtual float logicalWidthFor(float /*available*/) const { return logicalWidth(); }
  // The device (or something inside it) changed in the document: show it (values, settings, activation, structure).
  virtual void update(const doc::Device& d) = 0;
  // Whether update(d) can show `d` (else the frame is rebuilt). T4.4-fix1: a device whose patch text now plays another
  // graph (an edit in the circuit view, Revert to Stock) needs a new panel; a comment or layout edit keeps it.
  virtual bool keeps(const doc::Device& /*d*/) const { return true; }
  // Only these params changed (a Param ChangeSet): repaint what shows them.
  virtual void paramsChanged(const doc::Device& d, std::span<const doc::ParamRef> params) = 0;
  // The panel showing this device's params (a rack: its macro panel), and its binding.
  virtual devices::DevicePanel* panel() const noexcept { return nullptr; }
  virtual devices::DeviceTarget* target() noexcept { return nullptr; }
  virtual bool isRack() const noexcept { return false; }
  // Adds itself (and what it shows inside: a rack's frames) to `all`.
  virtual void collect(std::map<uint32_t, FrameBase*>& all) { all[id_.value] = this; }
  // T4.16c: itself, then (a rack) the frames of the chain it shows, in display order (left → right): the order Left /
  // Right walk. A folded rack shows none of its devices.
  virtual void appendShown(std::vector<FrameBase*>& out) { out.push_back(this); }
  // T4.16c: items the right-click menu at `p` adds first (a rack: Rename Macro over a macro knob).
  virtual void contextItems(ui::Vec2 /*p*/, std::vector<ui::MenuItem>& /*items*/) {}
  // T4.18: the automation lanes of the shown track or the playhead changed: a rack re-shows its chains' volume / pan
  // (an automated one: the envelope at the playhead and an LED).
  virtual void automationChanged() {}
  // T4.18: the param of panel() whose control is under `p` ("" none: no panel, or not over a param's control).
  std::string paramAt(ui::Vec2 p) const;
  // T4.18: the params with an automation lane (second: overridden): their controls get Live's LED (paintOver).
  void setAutomated(std::vector<std::pair<std::string, bool>> params);
  const std::vector<std::pair<std::string, bool>>& automated() const noexcept { return automated_; }
  // T4.8: a drop for a dragged device at `p` inside this frame (racks: their chain list and shown chain).
  virtual bool dropAt(ui::Vec2 /*p*/, DropTarget& /*out*/) { return false; }
  // The title-bar band a reorder drag starts from.
  virtual bool inTitle(ui::Vec2 p) const;
  void setSelected(bool s);
  bool selected() const noexcept { return selected_; }

  bool onPointer(const ui::PointerEvent& e) override;
  // M4 review L2: Esc, a lost capture or focus (alt-tab) cancel a reorder drag (nothing moves).
  bool onKey(const ui::KeyEvent& e) override;
  void onCaptureLost() override;
  void onInteractionCancelled() override;
  void paintOver(ui::PaintContext& ctx) override;
  // T4.16c: the title bar moves the device (a reorder drag); the rest inherits (the panel's parts decide).
  ui::Cursor cursorAt(ui::Vec2 p) const override;

protected:
  DeviceChainView::Impl& impl_;
  ViewContext& ctx_;
  doc::DeviceId id_;
  bool selected_ = false;
  std::vector<std::pair<std::string, bool>> automated_;  // T4.18
};

// A frame for `d` (a rack or a device).
std::unique_ptr<FrameBase> makeFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d);
std::unique_ptr<FrameBase> makeRackFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d);
// TX.groups (group_frame.cpp): a group's frame — its panel is the circuit of its contents (an embedded, scoped project
// patch view); groupPatchView: that view, or null for another frame.
std::unique_ptr<FrameBase> makeGroupFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d);
project_patch::ProjectPatchView* groupPatchView(FrameBase& f);
// TX.groups: the devices the Device view shows for a list: every device but a group's members (the group shows them).
// TX.free-rack: and no free device (the free-device rack shows them; a member is always free).
std::vector<doc::DevicePtr> shownDevices(const std::vector<doc::DevicePtr>& list);
// Devices of a rack's chain are framed like the track's: by makeFrame.
// T4.8: the map-mode overlay (rack_frame.cpp): over the chain while a rack's Map is on.
std::unique_ptr<ui::Widget> makeMapOverlay(DeviceChainView::Impl& impl);

// TX.free-rack (free_rack.cpp): the free-device rack over the chain — a tab strip (collapse / expand, the count, Add
// Free Device) and, expanded, the embedded circuit of the shown track's free devices with a resize handle under it.
class FreeRackPanel final : public ui::Widget {
public:
  FreeRackPanel(DeviceChainView::Impl& impl, ViewContext& ctx);
  project_patch::ProjectPatchView& view() const noexcept { return *view_; }
  bool open() const noexcept { return open_; }
  void setOpen(bool open);  // grows / shrinks the detail area (DeviceChainView::onRackHeight) and saves the prefs
  float bodyHeight() const noexcept { return height_; }
  void setBodyHeight(float logicalPx);
  float logicalHeight() const noexcept;  // the tab, and the body and handle while open
  void follow(doc::TrackId track);       // the shown track (its free devices; the count in the tab)
  void setDropHighlight(bool on);        // a chain device dragged over it
  ui::IRect plusRect() const noexcept;   // the tab's Add Free Device button (device px)
  ui::IRect handleRect() const noexcept; // the resize handle (empty while collapsed)

  void arrange(ui::LayoutPass& pass) override;
  void paint(ui::PaintContext& ctx) override;
  void paintOver(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override;
  ui::Cursor cursorAt(ui::Vec2 p) const override;
  ui::InfoText info() const override;

private:
  void savePrefs();
  void applyHeight();
  uint32_t freeCount() const;

  DeviceChainView::Impl& impl_;
  ViewContext& ctx_;
  project_patch::ProjectPatchView* view_ = nullptr;
  doc::TrackId track_;
  bool open_ = false;
  float height_ = 200.0f;
  bool resizing_ = false, highlight_ = false;
  float pressY_ = 0.0f, h0_ = 0.0f;
};

}  // namespace device_chain

}  // namespace pq::views
