#pragma once

// GU.3 (docs/plans/GU.md "Panels and the panel window"): a GROUP'S PANEL as a Face (pq/views/meta/meta_face.h), so every
// host that draws faces draws it — the project patch canvas (a collapsed group with a panel is drawn as its panel:
// project_patch.h Node::groupPanel), the Device view's group frame (PanelHost below), GU.5's floating panel window.
//
//   source     where the panel comes from: the group's document panel (re-read at each paint and event; the elements
//              are rebuilt only when the panel's pointer changes — a persistent document) or a fixed one (the group
//              inspector's generated panel).
//   elements   one pq/views/panel/element.h Element per document element, in paint order; hit-tested topmost first.
//              An element with a visibility condition is drawn and hit only while its source element (a tabs / menu /
//              toggle) has one of the values. Tabs that bind nothing keep their own selection across rebuilds.
//   drawing    the plate (body + frame) and each shown element: its static layers from the shared ELEMENT TILE CACHE —
//              one coverage tile per (kind + style + size, layer, scale) in the renderer's atlas, shared by every element
//              alike on every panel (200 identical knobs: one raster per layer) — drawn as one quad each (directly while
//              the tile is missing or no atlas exists: goldens); then its live layer. Tiles are built in prepare() (the
//              host's onFrame at a settled zoom), never in paint.
//   input      the canvas's face routing: a press goes to the element under it (it captures until Up), hover moves to
//              the element under the pointer, keys and text to the element the last press went to (a number box being
//              typed into). A press on no element is the block's (kFrame: select / move).
//   target     a PanelTarget (element.h): makeDocPanelTarget binds the document (gestures through the store: one undo
//              step per drag; a setting committed once).
//
// UI thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/panel.h"
#include "pq/ui/widget.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/panel/element.h"

namespace pq::views {
class ViewContext;
}

namespace pq::views::panel {

class GroupPanelFace final : public Face {
public:
  using Source = std::function<doc::PanelPtr()>;
  GroupPanelFace(Source source, std::unique_ptr<PanelTarget> target);
  ~GroupPanelFace() override;

  // ---- geometry ----
  ui::Vec2 worldSize() const noexcept override;  // the panel's size (as of the last sync)
  // GU.5b: a panel whose bottom element has an outline (Element::outline: a circle shape, the dial) is that shape (no
  // rectangular plate); any other a rounded rect.
  FaceShape shape() const noexcept override;
  bool ownsPlate() const noexcept override { return !round_; }
  static constexpr float kCorner = 8.0f;

  // ---- painting ----
  void paintLive(ui::FacePainter& p, const FaceEnv& env) override;
  void liveRects(std::vector<ui::Rect>& out) const override;
  bool needsPrepare(float pxPerUnit) const noexcept override;
  bool prepare(float pxPerUnit) override;
  void attach(FaceHost* host) override;
  // GU.5b: an element's floating popup (the dial's range and curve) and the elements' voices (the phase orbit's dots).
  std::optional<ui::Rect> popupRect() const noexcept override;
  void paintPopup(ui::FacePainter& p, const FaceEnv& env) override;
  void setPopupOverFace(bool on) override;
  VoiceTapReader* voices() const noexcept override;
  bool pollVoices() override;
  bool animating() const noexcept override;

  // ---- interaction ----
  int32_t hitTest(ui::Vec2 local, float pxPerUnit) const noexcept override;
  bool onPointer(const FacePointer& e) override;
  bool dragging() const noexcept override;
  bool onKey(const ui::KeyEvent& e) override;
  bool onText(const ui::TextEvent& e) override;
  bool wantsText() const noexcept override;
  void focusLost() override;
  void cancelDrag() override;
  void endWheel() override;  // GU.13: every element's wheel run ends (one undo step each)
  ui::Cursor cursor(int32_t part) const noexcept override;
  void describe(int32_t part, std::string& title, std::string& body) const override;
  bool setHoveredPart(int32_t part) noexcept override;
  // GU.5: a wire into a control inside the group ends on the element bound to it. `param` is the closed group's port for
  // that control (project_patch.h: "<name>@<device id>"; a bare name: a param of the group itself, owner()).
  std::string paramAt(ui::Vec2 local) const override;
  std::optional<ControlAnchor> controlAnchor(std::string_view param) const override;
  std::optional<ControlAnchor> controlHome(std::string_view param) const override;
  // The group whose panel this is (the port keys above name its own params bare); invalid: none known.
  void setOwner(doc::DeviceId group) noexcept { owner_ = group; }
  doc::DeviceId owner() const noexcept { return owner_; }
  // The port key of a Control binding on `group`'s panel ("<name>@<id>", or the name for the group's own) and back
  // (an unbound binding when `key` is malformed).
  static std::string portKey(doc::DeviceId group, const doc::Binding& b);
  static doc::Binding controlOfKey(doc::DeviceId group, std::string_view key);
  // GU.6: where Control binding `want` sits (panel units): an element bound to it, or one inside an embedded child
  // panel; `shownOnly`: only on elements shown now (not on another tab). nullopt: nowhere.
  std::optional<ControlAnchor> bindingAnchor(const doc::Binding& want, bool shownOnly) const;
  // GU.13: every control the shown elements show (Element::visitControls), its rect in panel units; and the control
  // under `local` (a Control binding: an element's own, an engine face's control, an embedded panel's), null: none.
  void visitControls(const Element::ControlVisitor& fn) const;
  const doc::Binding* controlAt(ui::Vec2 local) const;
  // Where element i's control sits (panel units): its anchor (a knob's centre) or its rect's centre; nullopt: none.
  std::optional<ControlAnchor> elementAnchor(size_t i) const;
  // GU.5: a mod source's cable on the pointer (a drag from its handle; the face draws it to the pointer and outlines the
  // control a release would wire), and that control's element (-1: none under the pointer).
  bool cableDragging() const noexcept { return cable_ >= 0; }
  int32_t cableTarget() const noexcept { return cableTarget_; }

  // ---- the panel ----
  // Re-reads the source: the elements rebuilt when the panel changed (true). Paint and the events call it.
  bool sync();
  // GU.13: the rects (panel units) of the shown elements whose live key (Element::liveKey) changed since the last call,
  // or that were shown or hidden; false: no keys yet (the first call, a rebuild): repaint the whole panel.
  bool changedRects(std::vector<ui::Rect>& out);
  PanelTarget& target() const noexcept { return *target_; }
  size_t elementCount() const noexcept { return elements_.size(); }
  Element& element(size_t i) const noexcept { return *elements_[i]; }
  int32_t indexOf(uint32_t id) const noexcept;  // −1: none
  bool shown(size_t i) const;                   // its visibility condition holds now
  // The element under `local` that takes presses (topmost, shown), −1: none.
  int32_t elementAt(ui::Vec2 local) const;
  // Stats of the last paint (the bench, tests): elements drawn, static layers from tiles / drawn directly.
  struct Stats {
    uint32_t drawn = 0, fromTiles = 0, direct = 0, tilesBuilt = 0;
  };
  const Stats& stats() const noexcept { return stats_; }

private:
  void rebuild(const doc::PanelPtr& p);
  bool shownNow(size_t i, std::vector<int8_t>& memo) const;
  std::vector<int8_t>& freshMemo() const;  // memo_ cleared for this many elements (no allocation once grown)
  ui::TileAtlas* atlas() const noexcept;

  Source source_;
  std::unique_ptr<PanelTarget> target_;
  doc::PanelPtr panel_;
  std::vector<std::unique_ptr<Element>> elements_;
  std::vector<int32_t> whenSource_;  // per element: the index of its condition's source element (−1: always shown)
  FaceHost* host_ = nullptr;
  int32_t capture_ = -1, focus_ = -1;
  mutable std::vector<int8_t> memo_;
  float tilesWanted_ = 0.0f;  // the scale the last paint missed tiles at (0: none missing)
  Stats stats_;
  std::vector<uint64_t> liveKeys_;  // GU.13: per element, its live key at the last changedRects (0: hidden)
  doc::DeviceId owner_;
  // GU.5: the mod source whose cable is on the pointer (-1: none), the pointer (panel units), the control under it.
  int32_t cable_ = -1, cableTarget_ = -1;
  ui::Vec2 cableTo_{0.0f, 0.0f};
  int32_t cableTargetAt(ui::Vec2 local) const;
  int32_t popupAt(ui::Vec2 local) const;  // the element whose open popup holds `local` (−1: none)
  bool round_ = false;                    // GU.5b: the bottom element's outline is the panel's
  float corner_ = kCorner;                // GU.7: the plate's corner (a bottom plate shape's radius)
  struct AnyReader final : VoiceTapReader {  // GU.7: live while any element's reader is
    const GroupPanelFace* face = nullptr;
    uint32_t read(std::span<FaceVoice> out) override;
    uint64_t serial() const noexcept override;
    bool live() const noexcept override;
  };
  mutable AnyReader any_;
  FaceShape roundShape_;
};

// GU.5b: the outline a panel takes from its bottom element (Element::outline, in panel units), nullopt: a rounded rect.
// GU.6: a bottom element embedding a child panel takes the child's outline (looked up in `list`, the group's track's
// blocks; null: an embed is never round).
std::optional<FaceShape> panelShape(const doc::Panel& panel, const std::vector<doc::DevicePtr>* list = nullptr);

// GU.5b: the face of a VALUE BLOCK in a circuit (an opened group's): one knob filling w × h, bound to the block's
// `value` (its name above, its value below).
doc::Panel valueKnobPanel(doc::DeviceId block, float w, float h);

// The group's document panel: Device::panel of `group` in the current snapshot (null: none).
GroupPanelFace::Source docPanelSource(ViewContext& ctx, doc::DeviceId group);
// A PanelTarget on the document, for the panel of group `group`: bindings resolved through the context's device library
// (a control: its param's label and curve — the device's own or the declared one —; a setting: the group's polyphony
// specs, a patch's `real const`, a meta module's setting), edits through doc::ops in store gestures. `menuHost`: the
// widget menus open from (null: none).
std::unique_ptr<PanelTarget> makeDocPanelTarget(ViewContext& ctx, doc::DeviceId group, std::function<ui::Widget*()> menuHost);

// The generated panel of a group's own settings (the GROUP INSPECTOR, GU.3: an instrument group's polyphony — voices,
// mono, legato, glide, steal — when `polyphony`; TX.group-oversample: every group's Oversample menu, 1× … 8× with each
// factor's latency) as number / toggle / menu elements bound to its settings, `width` wide.
doc::Panel inspectorPanel(doc::DeviceId group, float width, bool polyphony);

// A widget that hosts one Face fitted into its rect (aspect kept, centred, whole device px at its origin), routing pointer
// events, keys and text to it as the canvas does, preparing it at its scale in onFrame (its tiles). A panel repaints when
// its document changes (notifyChanged); its voices are polled at ≤ 60 Hz only while its reader is live or it animates
// (idle: no timer; wake() looks again). GU.13: its controls take the DEVICE-PANEL GESTURES (FacePointer::valueGestures:
// the wheel, a run of notches one undo step ended kWheelRunSeconds after the last; the keys after a press) and its
// popups are drawn over it. The Device view's frames (a group's panel, its inspector, a leaf's: block_frame.h) and the
// panel window use it.
class PanelHost final : public ui::Widget, public FaceHost {
public:
  PanelHost(std::string name, std::unique_ptr<Face> face);
  ~PanelHost() override;
  Face& face() const noexcept { return *face_; }
  GroupPanelFace* panelFace() const noexcept;  // the face as a group panel (null: another kind)
  ui::FaceTransform transform() const noexcept;
  // GU.13: the panel's controls in device px (GroupPanelFace::visitControls mapped through transform(), clipped to the
  // host's rect; nothing while it is hidden or another kind of face), the control under device point `p`, the rect of
  // a control and the top centre of its anchor (where a cable ends on it).
  using ControlFn = std::function<void(const doc::Binding& b, const ui::IRect& r)>;
  void visitControls(const ControlFn& fn) const;
  const doc::Binding* controlAt(ui::Vec2 p) const;
  std::optional<ui::IRect> controlRect(const doc::Binding& b) const;
  std::optional<ui::Vec2> controlPoint(const doc::Binding& b) const;
  // The document changed: repaint (and re-sync a group panel).
  void notifyChanged();
  // GU.13: the face's voices may have become live (a note while nothing animated): it polls again.
  void wake();
  static constexpr double kWheelRunSeconds = 0.4;  // ui::ValueControl::kWheelGestureSeconds
  // GU.5: the face part under the pointer changed (a panel window's link line follows the hovered element).
  std::function<void()> onHoverPart;
  int32_t hoveredPart() const noexcept { return hover_; }

  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  bool onText(const ui::TextEvent& e) override;
  void onFrame(double now) override;
  void onCaptureLost() override;
  void onFocusChanged(bool focused) override;
  void onHoverChanged(bool hovered) override;
  void onTimer(uint32_t id) override;
  ui::Cursor cursorAt(ui::Vec2 p) const override;
  ui::InfoText info() const override;
  // FaceHost
  ui::Widget* hostWidget() noexcept override { return this; }
  void faceNeedsUpdate(Face& f) noexcept override;

private:
  FacePointer pointerOf(const ui::PointerEvent& e) const;
  std::unique_ptr<Face> face_;
  int32_t hover_ = Face::kNone;
  bool captured_ = false;
  uint32_t wheelTimer_ = 0, voiceTimer_ = 0;  // GU.13
  void endWheel();
  mutable std::string title_, body_;
};

}  // namespace pq::views::panel
