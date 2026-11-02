#pragma once

// A device's Device-view panel as a block's face (TX.panel-faces, SPEC §15 2026-10-31 "the whole project is one
// circuit"; bug report 2026-11-01_12-15-15: "the utility in the circuit should just look like the utility ui in the
// device list below"). The project patch view draws a closed stock device as this face: the same DevicePanel the Device
// view shows (pq/views/devices/device_panel.h: the same widgets, layout, displays and bindings), hosted by the circuit
// canvas through the one face path (pq/views/meta/meta_face.h Face) — not a second widget system.
//
//   tree        the panel is the root of its own ui::UiTree, laid out at the canvas's device scale (zoom × UI scale,
//               clamped to kMinScale … kMaxScale) with fonts resolved at that scale, so its text and 1-px lines are as
//               crisp as in the Device view at every settled zoom. The tree is linked to the canvas (ui::TreeHost):
//               popups (a chooser's menu) open in the canvas's overlay where the panel shows the control, the
//               clipboard is the canvas tree's, and damage, frame requests and timers reach the canvas (Face::update /
//               nextWake), so the panel's own animations and gesture timers run with no polling while idle.
//   paint       the tree paints into a recorded draw list once per change and that list is replayed into the canvas's
//               list each frame mapped to the block (DrawList::appendTransformed: scale 1 and a whole-pixel offset at a
//               settled zoom, so a pan or a repaint of something else never re-runs the widgets). While a zoom settles,
//               and below kMinScale (far out: the recording at kMinScale), the recording is drawn scaled; the canvas
//               asks for the layout at the new scale once the zoom settles (needsPrepare / prepare).
//   input       the canvas routes presses on a control (DevicePanel::interactiveAt: knobs, boxes, toggles, choosers,
//               tabs, displays, the activator) to the tree in panel px; the rest of the block (body, labels, the title)
//               is the block's (select, move, double-click opens it to its atoms). A drag reads pointer movement in
//               device px, so a knob at far zoom turns as it does at the scale it was laid out at. Keys and text go to
//               the tree while one of its controls has the focus (the last press went there: a number box's type-in).
//               The wheel stays the canvas's (it pans), as for the Phase accumulator's face.
//   ports       the host's pins are sockets centred on the block's left / right edge (the exports too: the panel's
//               own jack strip is left out); a wire dropped on a control drives its param (paramAt).
//
// UI thread only.

#include <array>
#include <map>
#include <vector>
#include <memory>
#include <string>
#include <string_view>

#include "pq/ui/draw_list.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/meta/meta_face.h"

namespace pq::views::devices {

class PanelFace final : public Face, private ui::TreeHost {
public:
  static constexpr float kMinScale = 0.5f;  // device px per world unit: below it the recording is drawn shrunk
  static constexpr float kMaxScale = 8.0f;  // above it, stretched

  // `sig` / `desc`: the panel (sig's exports are dropped: the host draws them as pins); `target`: where edits go (owned);
  // `values`: params / consts by name; `worldSize`: the block (≥ the panel's logical size: its background continues).
  PanelFace(DeviceSignature sig, PanelDesc desc, std::unique_ptr<DeviceTarget> target,
            const std::map<std::string, double, std::less<>>& values, ui::Vec2 worldSize);
  ~PanelFace() override;

  DevicePanel& panel() const noexcept { return *panel_; }
  DeviceTarget* target() const noexcept { return target_.get(); }
  // The block's size changed (a wired param added a pin row): the panel is laid out again at the next paint.
  void setWorldSize(ui::Vec2 s);
  // The size a panel face for `desc` needs (world units: the panel without the exports' strip).
  static ui::Vec2 sizeFor(const PanelDesc& desc) noexcept;

  // Tests and benchmarks: the scale the tree is laid out at (0: not yet), the tree, and how often it was recorded.
  float layoutScale() const noexcept { return layScale_; }
  ui::UiTree& panelTree() noexcept { return tree_; }
  uint64_t recordings() const noexcept { return recordings_; }

  // ---- Face ----
  ui::Vec2 worldSize() const noexcept override { return size_; }
  // Far out (below kMinScale) the panel is drawn from kLayers coverage tiles at the canvas's scale (the host's static
  // layers: one quad each per frame): the recording's triangles split by colour — panel fills, dark fields, light ink
  // (text as bars, lines), accents (value arcs, curves, lit buttons) — each drawn in its class's mean colour; keyed by
  // the recording, so a value change re-rasterises that panel's tiles only.
  static constexpr uint32_t kLayers = 4;
  uint32_t staticLayerCount() const noexcept override { return kLayers; }
  bool staticAt(float pxPerUnit) const noexcept override { return pxPerUnit < kMinScale; }
  ui::Rgba staticLayerColour(const ui::Theme& theme, uint32_t layer) const noexcept override;
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override;
  uint64_t staticKey(uint32_t layer) const override;
  void paintLive(ui::FacePainter& p, const FaceEnv& env) override;
  void paintJacks(ui::FacePainter& /*p*/, const FaceEnv& /*env*/) const override {}  // drawn over the panel (paintLive)
  void liveRects(std::vector<ui::Rect>& out) const override { out.push_back({0.0f, 0.0f, size_.x, size_.y}); }
  int32_t hitTest(ui::Vec2 local, float pxPerUnit) const noexcept override;
  bool onPointer(const FacePointer& e) override;
  bool dragging() const noexcept override;
  bool onKey(const ui::KeyEvent& e) override;
  bool onText(const ui::TextEvent& e) override;
  bool wantsText() const noexcept override;
  void focusLost() override;
  void cancelDrag() override;
  ui::Cursor cursor(int32_t part) const noexcept override;
  void describe(int32_t part, std::string& title, std::string& body) const override;
  std::string paramAt(ui::Vec2 local) const override;
  bool setHoveredPart(int32_t part) noexcept override;
  void attach(FaceHost* host) override;
  bool update(double now) override;
  double nextWake() const noexcept override;
  bool needsPrepare(float pxPerUnit) const noexcept override;
  bool prepare(float pxPerUnit) override;

private:
  // ui::TreeHost
  ui::Widget* hostWidget() const noexcept override;
  ui::IRect toHost(const ui::IRect& r) const noexcept override;
  void treeNeedsUpdate() noexcept override;

  static float clampScale(float s) noexcept;
  const ui::PaintResources* hostResources() const noexcept;
  double hostNow() const noexcept;
  // Lays the tree out at scale `s` (fonts resolved at it). False when nothing changed.
  bool layoutAt(float s);
  void record();
  ui::Vec2 toPanel(ui::Vec2 local) const noexcept { return {local.x * layScale_, local.y * layScale_}; }

  std::unique_ptr<DeviceTarget> target_;
  ui::UiTree tree_;
  DevicePanel* panel_ = nullptr;
  ui::Vec2 size_{0.0f, 0.0f};
  FaceHost* host_ = nullptr;
  ui::PaintResources res_{};             // what the tree paints with (the host's, fonts at layScale_)
  ui::PaintResources from_{};            // the host's resources it was built from
  ui::ThemeFonts fonts_{};
  float layScale_ = 0.0f;                 // the tree's scale (0: never laid out)
  ui::Vec2 lastOrigin_{0.0f, 0.0f};       // where the last paint put the block (host device px)
  float lastScale_ = 1.0f;                // ... at this many host device px per world unit
  ui::DrawList rec_;                      // the tree's last paint, in panel px
  bool recValid_ = false;
  bool recTiles_ = false;                 // the recording samples tile-atlas pages (a meta face inside): re-recorded each paint
  uint64_t recEvictions_ = 0;             // the glyph atlas's eviction count when recorded
  uint64_t recordings_ = 0;
  std::vector<uint8_t> triClass_;            // per triangle of the recording (3 indices): its colour class
  std::array<ui::Rgba, kLayers> classColour_{};  // each class's mean colour (alpha 0: the class is empty)
  bool quiet_ = false;                    // inside our own update / paint: the tree's notifications are ours
  bool pressed_ = false;                  // a press went to the tree and it holds the pointer
  ui::Vec2 downPanel_{0.0f, 0.0f}, downDevice_{0.0f, 0.0f};
  float downK_ = 1.0f;
};

}  // namespace pq::views::devices
