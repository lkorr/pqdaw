#pragma once

// GU.5 (docs/plans/GU.md "Panels and the panel window", "Phases" 5): a group's PANEL WINDOW — the first place the user
// builds a GUI. It floats over the project patch canvas (a child of the view, over its canvas; never in the overlay
// layer, so menus open over it and it stays put):
//
//   title bar   the group's name; Edit (the mode toggle), + (send the selected control: the host's hook), – (minimise:
//               the title bar only; again, or a double-click on the title, restores it), × (close; the capsule's header
//               button reopens it). Dragging the title moves the window, the corner grip resizes it; the panel inside
//               is fitted to the window (aspect kept), so resizing zooms it. Place, size, mode and minimised are view state
//               (doc::ViewState::panelWindows: saved, not undoable; the hook `changed` stores them).
//   play mode   the panel itself (a PanelHost with the group's GroupPanelFace bound to the document): its elements edit
//               what they are bound to, as on the closed group in the circuit; a mod source dragged onto a control makes
//               the cable. The window shows no wires (link lines are drawn by the circuit, under the window).
//   edit mode   the PANEL EDITOR over the panel: click an element to select it (Shift / Ctrl: add or remove), drag an empty
//               place for a selection frame; drag a selection to move it, a handle of a single selection to resize it;
//               moves and resizes snap their edges and centres to the other elements' and the panel's within 4 px at zoom
//               1 (guides drawn; Alt: no snap); Delete removes, arrows nudge (Shift: 10), Ctrl+D duplicates, Ctrl+A selects
//               all, Ctrl+Shift+G breaks an embedded child panel apart (GU.6), the right-click menu aligns /
//               distributes / reorders; the panel's corner handle sizes the panel. Every
//               gesture is ONE undo step (a drag previews in the window and commits one SetGroupPanel at the release).
//               The INSPECTOR column on the right is generated: the element's kind, its binding (a menu of the group's
//               controls, settings and outputs), its visibility condition (shown while a tabs / menu / toggle element has a
//               value), its place and size, its style fields (from the kind's registry schema: numbers, choices, theme
//               tokens, text) and, for a control, its curve (a curve editor element bound to the same control). Each edit
//               is one undo step (a number dragged: one step at its release).
//
// CPU: nothing runs while idle (no timers; the panel repaints when the document changes or the pointer acts on it). A
// drag in the editor rebuilds the window's elements from the preview panel per move (a few dozen small objects) and
// never touches the document until the release. UI thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pq/document/panel.h"
#include "pq/document/store.h"
#include "pq/ui/widget.h"
#include "pq/views/panel/element.h"
#include "pq/views/panel/group_panel_face.h"

namespace pq::views {
class ViewContext;
}

namespace pq::views::panel {

// ---- sending controls to a panel ------------------------------------------------------------------------------------
// The element a binding becomes when it is sent to a panel (its kind and size, from the kind's registry default):
// a mod source for an output, a toggle for a switch, a menu for choices, a number box for a setting without choices,
// a knob for any other control.
struct ElementChoice {
  std::string kind;
  float w = 48.0f, h = 60.0f;
};
ElementChoice defaultElement(const doc::Binding& b, const BoundInfo& info);
// A panel that has none yet starts this size (world units); elements keep this far from its edges and from each other.
inline constexpr float kNewPanelW = 240.0f, kNewPanelH = 120.0f, kPlaceGap = 8.0f;
// The first free place for a w × h element on `p` (null: a new panel) in reading order (8-unit steps from its top-left,
// kPlaceGap from every element, inside its width); below everything when nothing fits (the panel then grows).
ui::Vec2 freePlace(const doc::Panel* p, float w, float h);
// `base` (null: a new kNewPanelW × kNewPanelH panel) with `add` appended: each gets a fresh id (into `ids`) and, the
// first centred at `at` when given (panel units, on whole units, inside the panel), the rest at their free places; the
// panel grows to hold them (+ kPlaceGap).
doc::Panel panelWithElements(const doc::Panel* base, std::vector<doc::PanelElement> add, std::optional<ui::Vec2> at,
                             std::vector<uint32_t>* ids = nullptr);

// ---- the window -----------------------------------------------------------------------------------------------------
class PanelEditor;
class PanelInspector;
class PanelWindowGrip;

class PanelWindow final : public ui::Widget, private ui::TreeObserver {
public:
  // Logical px.
  static constexpr float kTitleH = 24.0f, kInspectorW = 220.0f, kGripPx = 14.0f, kPad = 6.0f;
  static constexpr float kMinW = 180.0f, kMinH = 96.0f;
  struct Hooks {
    std::function<void(PanelWindow&)> changed;    // place, size, mode or minimised changed by the user (save view state)
    std::function<void(PanelWindow&)> closed;     // × pressed (the window hides itself first)
    std::function<void(PanelWindow&)> send;       // + pressed: send the selected control here
    std::function<void(PanelWindow&)> linksChanged;  // hovered element, selection or activity changed (link lines)
  };
  PanelWindow(ViewContext& ctx, doc::DeviceId group, Hooks hooks);
  ~PanelWindow() override;

  doc::DeviceId group() const noexcept { return group_; }
  ViewContext& context() const noexcept { return ctx_; }
  // View state (the host places the window from x, y, w, h: logical px from its own top-left). setState applies it
  // without calling a hook.
  const doc::PanelWindowView& state() const noexcept { return state_; }
  void setState(const doc::PanelWindowView& s);
  bool editMode() const noexcept { return state_.edit; }
  void setEditMode(bool on);  // (the user's: calls `changed`)
  bool minimised() const noexcept { return state_.minimised; }
  void setMinimised(bool on);
  // The last press in the tree was inside the window (its panel is "selected": every link line shows).
  bool active() const noexcept { return active_; }
  // A drag from the circuit is over the window (a drop would send to it): drawn highlighted.
  void setDropTarget(bool on);
  bool dropTarget() const noexcept { return drop_; }

  // ---- the panel ----
  PanelHost& host() const noexcept { return *host_; }
  GroupPanelFace& face() const noexcept;
  // The panel shown: the editor's preview during a gesture, else the document's (null: none yet — an empty panel of
  // kNewPanelW × kNewPanelH is drawn).
  doc::PanelPtr shownPanel() const;
  doc::PanelPtr docPanel() const;
  // Panel units → device px (the host's fit) and back; false: outside the panel area.
  ui::FaceTransform panelTransform() const noexcept;
  bool panelPoint(ui::Vec2 device, ui::Vec2& out) const noexcept;
  // Where element i of the face is drawn (its anchor: a knob's centre), device px; nullopt: hidden or none.
  std::optional<ui::Vec2> elementPoint(size_t i) const;
  // The element under the pointer (index into face(); −1: none), in either mode.
  int32_t hoveredElement() const noexcept;

  // ---- editing (edit mode; every call one undo step) ----
  const std::vector<uint32_t>& selection() const noexcept { return selection_; }
  void setSelection(std::vector<uint32_t> ids);
  bool removeSelected();
  bool duplicateSelected();
  // GU.6: the selected embedded child panels replaced by copies of their elements (ops::breakApartPanelElements; one
  // undo step), the copies selected. False: no embed selected.
  bool breakApartSelected();
  enum class AlignTo : uint8_t { Left, Centre, Right, Top, Middle, Bottom, SpaceH, SpaceV, Front, Back };
  bool alignSelected(AlignTo a);
  bool nudgeSelected(float dx, float dy);
  // The element ops: `edit` applied to every selected element (or to the panel itself) of a copy, committed as one step.
  bool editSelected(const std::function<void(doc::PanelElement&)>& edit, const char* label);
  bool editPanel(const std::function<void(doc::Panel&)>& edit, const char* label);
  // A preview (a gesture in the editor or the inspector): shown at once, the document untouched until commit.
  void preview(doc::PanelPtr p);
  bool commitPreview(const char* label);
  void cancelPreview();
  bool previewing() const noexcept { return preview_ != nullptr; }
  // The snap guides of the running move / resize (panel units; tests and paint).
  const std::vector<float>& guidesX() const noexcept;
  const std::vector<float>& guidesY() const noexcept;
  PanelEditor& editor() const noexcept { return *editor_; }
  PanelInspector& inspector() const noexcept { return *inspector_; }

  // ---- widget ----
  void arrange(ui::LayoutPass& pass) override;
  void paint(ui::PaintContext& ctx) override;
  void paintOver(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override;
  ui::Cursor cursorAt(ui::Vec2 p) const override;
  ui::InfoText info() const override;
  // The document changed: the panel repaints, the inspector follows the selected elements.
  void documentChanged();
  // (the grip's drag)
  bool gripPointer(const ui::PointerEvent& e);
  void linksChanged();

private:
  void onPointerDown(ui::Vec2 pos) override;
  void layoutChildren();
  const std::string& title() const;
  ViewContext& ctx_;
  doc::DeviceId group_;
  Hooks hooks_;
  doc::PanelWindowView state_;
  PanelHost* host_ = nullptr;
  PanelEditor* editor_ = nullptr;
  PanelInspector* inspector_ = nullptr;
  ui::Widget* inspectorScroll_ = nullptr;
  PanelWindowGrip* grip_ = nullptr;
  ui::Widget* modeButton_ = nullptr;
  std::vector<uint32_t> selection_;
  doc::PanelPtr preview_;
  doc::PanelPtr empty_;  // the panel drawn while the group has none
  bool active_ = false, drop_ = false;
  ui::UiTree* observing_ = nullptr;
  // Title / grip drags (device px at the press, the state then).
  enum class Drag : uint8_t { None, Move, Size } drag_ = Drag::None;
  ui::Vec2 press_{0.0f, 0.0f};
  doc::PanelWindowView pressState_;
  mutable doc::Snapshot titleOf_;
  mutable std::string title_;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  friend class PanelEditor;
  friend class PanelInspector;
};

}  // namespace pq::views::panel
