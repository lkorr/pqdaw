#pragma once

// The circuit view at the TRACK level (T5.x-circuit-chain): a full-window popover showing a track's whole device chain
// as one circuit (pq/views/circuit/chain_circuit.h) — every device's own circuit inside a labelled boundary, left →
// right in chain order, the audio wires from each device's `out` into the next one's `in`, from the track's input
// through the chain into its mixer; racks as nested boundaries with their chains stacked; T4.7's modulation wires
// dashed in the gutter above.
//
//   open        a device's "Open in Circuit View" (its menu, a double-click on its title, the project patch view's
//               block menu) opens its track's circuit FOCUSED on it (its boundary fitted and centred); View > Track
//               Circuit (`view.trackCircuit`, Ctrl+Alt+C) and the track header's menu open the selected track's.
//   levels      the title bar is a breadcrumb: Track (this view) · Device (the device's circuit editor, editable).
//               Double-click inside a device's boundary, Enter on a selected node, or Edit Device opens that device's
//               editor (circuit::openDeviceInCircuitView: edits are document commands); the editor's Track button comes
//               back here, focused on that device.
//   read-only   this view does not edit: pan / zoom / select only (the canvas's keys; F fits the track). Each device
//               is edited at the Device level, where its text pane and checks are.
//   follows     any structural or name change of the document rebuilds the circuit (device scenes come from a
//               ChainSceneCache: an edited device rebuilds only its own scene), keeping the view; the track removed
//               closes the view.
//
// CPU: the layout and scene are built once per structure change (never per frame). Painting is the circuit canvas's
// (spatial-grid culling, symbol tiles, cached Far chunks: zoomed out, the whole chain is ≈ 2 quads per 256-px chunk)
// plus one pass over the boundaries (a few rects and a title each; boxes off screen skipped). Idle: no frames, no
// timers. UI thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "pq/document/ids.h"
#include "pq/ui/overlay.h"
#include "pq/views/circuit/chain_circuit.h"
#include "pq/views/circuit/circuit_canvas.h"
#include "pq/views/view_context.h"

namespace pq::ui {
class Label;
class Button;
}  // namespace pq::ui

namespace pq::views::circuit {

// Paints `c`'s boundaries on `canvas`, translated by (dx, dy) world units (the project patch view draws a rack's
// circuit inside its block): in track mode the track's band (its colour, its name), then every box, parents first
// (fill, outline and title strip in the kind's accent; the title at every zoom where the box is wide enough to read
// it). Boxes outside the clip are skipped. Called from a canvas's paintUnderlay.
void paintChainBoxes(ui::PaintContext& ctx, const CircuitCanvas& canvas, const ChainCircuit& c, float dx = 0.0f, float dy = 0.0f);
// Over the scene: deactivated devices / chains veiled, "off" in their title strips.
void paintChainVeils(ui::PaintContext& ctx, const CircuitCanvas& canvas, const ChainCircuit& c, float dx = 0.0f, float dy = 0.0f);

// The canvas of the view: the circuit canvas with the boundaries under the scene, pin stubs from the Near level only.
class ChainCanvas : public CircuitCanvas {
public:
  ChainCanvas();
  // The circuit to show (its scene goes to the canvas). keepTiles: symbol body tiles stay (a rebuild after an edit).
  void setCircuit(std::shared_ptr<const ChainCircuit> c, bool fit, bool keepTiles);
  const ChainCircuit* circuit() const noexcept { return circuit_.get(); }
  // Frames box `box` at the next layout (or now when laid out): fitted, centred, at most zoom 1.5.
  void frameBox(uint32_t box);
  // The device box under a device point (kNoBox: none).
  uint32_t deviceBoxAt(ui::Vec2 device) const noexcept;

  std::function<void(uint32_t box)> onOpenBox;  // a double-click inside a device's / rack's boundary

  bool onPointer(const ui::PointerEvent& e) override;
  void arrange(ui::LayoutPass& pass) override;
  ui::InfoText info() const override;

protected:
  void paintUnderlay(ui::PaintContext& ctx) override;
  void paintOverlay(ui::PaintContext& ctx) override;

private:
  std::shared_ptr<const ChainCircuit> circuit_;
  uint32_t pendingFrame_ = kNoBox;
};

class TrackCircuitView : public ui::Popover {
public:
  TrackCircuitView(ViewContext& ctx, doc::TrackId track);
  ~TrackCircuitView() override;

  ChainCanvas& canvas() noexcept { return *canvas_; }
  const ChainCircuit& circuit() const noexcept { return *circuit_; }
  doc::TrackId track() const noexcept { return track_; }
  const std::string& title() const noexcept { return titleText_; }
  const std::string& status() const noexcept { return statusText_; }

  // Focuses a device of the track (any depth): its boundary framed, the title bar names it. False: not in this track.
  bool focusDevice(doc::DeviceId device);
  doc::DeviceId focused() const noexcept { return focus_; }
  // The device the Device level would open: the one holding the selection, else the focused one.
  doc::DeviceId currentDevice() const;
  // The Device level: `device`'s circuit editor (pq/views/circuit/device_circuit.h); this view closes. False: a rack
  // (no patch of its own: the status says so) or the editor could not open.
  bool editDevice(doc::DeviceId device);
  // Rebuilds the circuit now (the document listener does it after a structural or name change).
  void rebuild(bool fit);
  uint64_t rebuilds() const noexcept { return rebuilds_; }
  double lastBuildMs() const noexcept { return lastBuildMs_; }
  const ChainSceneCache& sceneCache() const noexcept { return cache_; }
  void closeSoon();

  bool onKey(const ui::KeyEvent& e) override;
  void onTimer(uint32_t id) override;
  ui::InfoText info() const override;

private:
  void onDocument(const doc::StoreEvent& e);
  void updateTitle();
  void setStatus(std::string s);

  ViewContext& ctx_;
  doc::TrackId track_;
  doc::DeviceId focus_;
  ViewContext::Subscription sub_;
  ChainSceneCache cache_;
  std::shared_ptr<const ChainCircuit> circuit_;
  ChainCanvas* canvas_ = nullptr;
  ui::Label* title_ = nullptr;
  ui::Label* status_ = nullptr;
  ui::Button* edit_ = nullptr;
  std::string titleText_, statusText_;
  uint64_t rebuilds_ = 0;
  double lastBuildMs_ = 0.0;
  uint32_t closeTimer_ = 0;
};

// Opens the view over the whole window of `from`'s tree (its overlay layer) on `track`, focused on `focus` when given.
// nullptr (and a status line) without an overlay layer or when the track does not exist.
TrackCircuitView* openTrackCircuit(ui::Widget& from, ViewContext& ctx, doc::TrackId track, doc::DeviceId focus = {});
// "Open in Circuit View" on a device: its track's circuit focused on it.
TrackCircuitView* openDeviceChainCircuit(ui::Widget& from, ViewContext& ctx, doc::DeviceId device);

}  // namespace pq::views::circuit
