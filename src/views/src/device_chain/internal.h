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
#include "pq/document/edit_ops.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/devices/panel_desc.h"

namespace pq::views {

// The panel of a device patch: its signature and its description (the .panel.json next to the device file, else the
// generated generic panel). Cached per patch by the view.
struct PanelTemplate {
  devices::DeviceSignature sig;
  devices::PanelDesc desc;
};

namespace device_chain {

inline constexpr float kTitleH = 20.0f;    // a device panel's title bar (T4.6's frame)
inline constexpr float kFrameGap = 4.0f;   // between frames
inline constexpr float kChainPad = 4.0f;   // around the chain
inline constexpr float kAddW = 132.0f;     // the add slot at the end of the chain

class FrameBase;

// Where a dragged device would land (T4.8: a rack's chain list or the chain a rack shows, besides the track's chain).
struct DropTarget {
  enum class Kind : uint8_t { None, Track, Chain, NewChain };
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
  std::vector<ModSweep> ringSweeps;
  uint32_t ringTimer = 0;
  bool ringsLit = false;
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
  // T4.8: a drop for a dragged device at `p` inside this frame (racks: their chain list and shown chain).
  virtual bool dropAt(ui::Vec2 /*p*/, DropTarget& /*out*/) { return false; }
  // The title-bar band a reorder drag starts from.
  virtual bool inTitle(ui::Vec2 p) const;
  void setSelected(bool s);
  bool selected() const noexcept { return selected_; }

  bool onPointer(const ui::PointerEvent& e) override;
  void paintOver(ui::PaintContext& ctx) override;
  // T4.16c: the title bar moves the device (a reorder drag); the rest inherits (the panel's parts decide).
  ui::Cursor cursorAt(ui::Vec2 p) const override;

protected:
  DeviceChainView::Impl& impl_;
  ViewContext& ctx_;
  doc::DeviceId id_;
  bool selected_ = false;
};

// A frame for `d` (a rack or a device).
std::unique_ptr<FrameBase> makeFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d);
std::unique_ptr<FrameBase> makeRackFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d);
// Devices of a rack's chain are framed like the track's: by makeFrame.
// T4.8: the map-mode overlay (rack_frame.cpp): over the chain while a rack's Map is on.
std::unique_ptr<ui::Widget> makeMapOverlay(DeviceChainView::Impl& impl);

}  // namespace device_chain

}  // namespace pq::views
