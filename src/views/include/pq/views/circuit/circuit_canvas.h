#pragma once

// The circuit canvas (T4.2, SPEC §7.3): a zoomable, pannable schematic of one CircuitScene — symbols, orthogonal wires
// coloured and thickened by kind and rate (bool thin green-ish, param thin grey, audio thicker in the signal colour,
// poly doubled, modulation into a parameter dashed), junction dots where a net branches (crossings have none), labels,
// selection and hit testing. It is the base of the circuit editor (T4.4: editing, text pane), live probing (T4.5) and
// the project patch view (T4.13); those build on the hooks below rather than on its internals.
//
// View: `View::x/y` is the world point at the canvas's top-left corner and `zoom` the logical px per world unit (device
// px = zoom × UI scale). Ctrl+wheel zooms at the pointer, the wheel pans (Shift: horizontally), the middle button (or
// Alt + left) drags the view, F fits the scene, = / − zoom, Esc clears the selection. Left click selects a node or wire
// (Shift adds, Ctrl toggles), a left drag on empty space selects with a rubber band, a double-click on a subcircuit or
// filter calls onOpen.
//
// Level of detail by zoom: Far (< 0.3): nodes as outlined boxes, wires 1 px in one colour, no dots or text — the whole
// picture cached in world-anchored chunks of 256 device px (two coverage tiles each: wires, nodes), so a pan or a settled
// frame draws ≈ 2 quads per chunk whatever the patch's size; while zooming the previous level's chunks are stretched,
// missing chunks are drawn directly and built in onFrame (≤ 4 per frame); the selection and the hovered item are drawn
// over them; Mid (< 0.75): symbols and
// styled wires with junction dots; Near: also titles, captions and (from zoom 1) pin labels in the UI font. Symbol bodies
// are cached as coverage tiles (pq/ui/tile_atlas.h, two per distinct body: fill and stroke) rasterised once per zoom
// level (pq/ui/raster.h) after a zoom settles, so a settled frame draws each symbol as two quads; while zooming, the
// previous level's tiles are drawn stretched, and without a tile atlas (no GPU) the bodies are drawn directly.
// Culling goes through the scene's spatial grid. Pan and zoom repaint the canvas; a selection or hover change repaints
// only the canvas too. Idle: no frames, no timers.
//
// UI thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pq/graph/patch.h"
#include "pq/ui/widget.h"
#include "pq/views/circuit/circuit_scene.h"

namespace pq::views::circuit {

struct CanvasHit {
  enum class Kind : uint8_t { None, Node, Pin, Wire };
  Kind kind = Kind::None;
  uint32_t index = UINT32_MAX;  // node, pin or wire index in the scene
  bool operator==(const CanvasHit&) const = default;
};

class CircuitCanvas : public ui::Widget {
public:
  explicit CircuitCanvas(std::string name = "circuit");
  ~CircuitCanvas() override;

  // The scene to show (shared: a canvas never changes it). fit = frame the whole scene at the next layout. keepTiles
  // (T4.4: an edited patch's next scene): symbol body tiles stay (a body's pixels depend only on its bodyKey and the
  // zoom), so an edit rasterises only the bodies that are new; the far-level chunks are rebuilt either way.
  void setScene(std::shared_ptr<const CircuitScene> scene, bool fit = true, bool keepTiles = false);
  const std::shared_ptr<const CircuitScene>& scene() const noexcept;

  struct View {
    double x = 0.0, y = 0.0;  // world point at the canvas's top-left corner
    double zoom = 1.0;        // logical px per world unit
    bool operator==(const View&) const = default;
  };
  static constexpr double kMinZoom = 0.02;
  static constexpr double kMaxZoom = 8.0;
  static constexpr double kFarZoom = 0.3;    // below: Far detail
  static constexpr double kNearZoom = 0.75;  // from here: Near (text)
  static constexpr double kPinLabelZoom = 1.0;
  static constexpr double kZoomSettleSeconds = 0.12;  // tiles are rebuilt this long after the last zoom step
  const View& view() const noexcept;
  void setView(const View& v);
  void fitAll();                                // the whole scene, centred, at most zoom 2
  // M4 review M15: the view a patch opens at: fitAll when that is readable (≥ kNearZoom), else kNearZoom on the
  // outputs (setScene(…, fit = true) does this).
  void fitToRead();
  void zoomAt(ui::Vec2 device, double factor);  // keeps the world point under `device` where it is
  void panBy(float dxDevice, float dyDevice);
  float deviceScale() const noexcept;           // device px per world unit (zoom × UI scale)
  ui::Vec2 toDevice(graph::Point2 world) const noexcept;
  graph::Point2 toWorld(ui::Vec2 device) const noexcept;

  enum class Lod : uint8_t { Far, Mid, Near };
  Lod lod() const noexcept;

  // Topmost pin, else node, else wire under a device point (pins within 5 px, wires within 4 px).
  CanvasHit hitTest(ui::Vec2 device) const noexcept;
  // T4.16c: what the pointer rests on (the last move's hit test, as drawn in the hover colour; not updated during a
  // drag), and whether a middle / Alt drag is panning the view now (subclasses' cursorAt).
  const CanvasHit& hovered() const noexcept;
  bool panning() const noexcept;
  // Alt is held: a left press pans (T4.16-fix1: the cursor shows it before the press).
  bool altHeld() const noexcept;

  struct Selection {
    std::vector<uint32_t> nodes, wires;  // sorted
  };
  const Selection& selection() const noexcept;
  void select(std::span<const uint32_t> nodes, std::span<const uint32_t> wires, bool add = false);
  void clearSelection();
  bool nodeSelected(uint32_t node) const noexcept;
  bool wireSelected(uint32_t wire) const noexcept;

  // Hooks for the views built on the canvas.
  std::function<void()> onSelectionChanged;
  std::function<void(const CanvasHit&)> onHoverChanged;
  std::function<void(uint32_t node)> onOpen;  // double-click on a subcircuit or filter
  // T4.5 (bool wires lit when high): drawn in circuit.wireBoolLit. Repaints only when the flag changes.
  void setWireLit(uint32_t wire, bool lit);

  struct Stats {
    uint32_t nodesPainted = 0, nodesFromTiles = 0, nodesStretched = 0, nodesDirect = 0;
    uint32_t wiresPainted = 0, segmentsPainted = 0, junctionsPainted = 0, labelsPainted = 0;
    // Far level: chunks drawn from their tiles, from the previous zoom level's tiles (stretched), or directly.
    uint32_t chunksFromTiles = 0, chunksStretched = 0, chunksDirect = 0;
    uint64_t tilesBuilt = 0, tileFailures = 0, paints = 0;
  };
  const Stats& stats() const noexcept;
  bool zooming() const noexcept;
  bool tilesPending() const noexcept;  // a visible body has no tile at this zoom yet (built in onFrame)
  void dropTiles();                    // benchmarks: forget every tile

  void paint(ui::PaintContext& ctx) override;
  void arrange(ui::LayoutPass& pass) override;
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  void onFrame(double now) override;
  void onTimer(uint32_t id) override;
  void onCaptureLost() override;
  void onInteractionCancelled() override;
  ui::InfoText info() const override;
  // T4.16c: Move while panning; otherwise inherited (the read-only viewer: Arrow). The hovered pin is ringed, the
  // hovered node and wire drawn in the hover colour.
  ui::Cursor cursorAt(ui::Vec2 pos) const override;

protected:
  // Drawn over the scene, inside the canvas (editing previews, probes).
  virtual void paintOverlay(ui::PaintContext& /*ctx*/) {}
  // T4.13: drawn under the scene (after the background, before wires and symbols; at every level of detail, so not
  // cached in the Far chunks): the project patch view's bands and group containers.
  virtual void paintUnderlay(ui::PaintContext& /*ctx*/) {}

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pq::views::circuit
