#pragma once

// The circuit editor (T4.4, SPEC §7.3): a full-window popover with the circuit canvas and the text pane side by side,
// both views of one PatchEditor (pq/views/circuit/patch_editor.h: the text is the source of truth; one undo history).
//
//   title bar   Back · file › patch path · Play · Tidy · Fit · Save · Close
//   canvas      the shown patch, editable (this file's patches; an imported patch opens read-only)
//   text pane   the whole file (ui::CodeEditor): syntax colours, the parser's / validator's / checker's errors inline
//   status      the last edit's note (an inserted z⁻¹, why an edit was refused), what plays
//
// Canvas editing: drag a node (the selection) to move it; drag from a pin to a pin to connect (output → input or
// input → output; dropping on a node's hidden optional input works: every pin of the node under the pointer shows
// while dragging; a wire that would close a delay-free loop gets an explicit z1, and the status says so); double-click
// empty space (or N) adds a node from a spec (`tanh`, `0.5`, `440Hz`, `*`, `svf`, `param cutoff = 1kHz`, `in x :
// audio`, `out y`, `export e`); double-click a node renames it (Ctrl+R too; a subcircuit opens instead); Delete /
// Backspace deletes the selected nodes and wires; Ctrl+G makes a subcircuit of the selection; P turns a selected
// constant into a param pin; Enter on a selected subcircuit opens it, Backspace with nothing selected goes back. Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y undo and redo in either pane; Ctrl+S saves; Ctrl+Enter plays /
// stops the shown patch. The text is checked ≈ 50 ms after the last edit (infer, optimise, schedule); while it plays a
// clean result hot-swaps the running program; an error leaves the last good one playing and shows inline.
//
// Legibility (T4.15, SPEC §15 2026-10-26): resting the pointer on a node shows its tooltip after the overlay's tooltip
// delay (pq/views/circuit/node_info.h: what it is, a description, its value's rate and poly, a constant's recognised
// form), built when it shows, never per frame. Branches that a structural constant switches off (graph::analyseStructure
// with the shown instance's arguments: a device's settings, carried down the call stack into opened subcircuits) are
// hidden; the title bar's Inactive toggle (remembered in the view prefs) draws them dimmed instead. Hiding is view-only:
// the text, its layout block and what is compiled stay as they are, and neither hovering nor the toggle compiles
// anything. While hidden, a patch with no stored positions for its visible nodes shows them laid out on their own;
// a circuit edit made then stores that layout (PatchEditor::setLayoutBase) and Tidy re-places only the visible nodes.
//
// Options > Developer > Open Patch File in Circuit View… (`view.openPatch`) opens a loose file in it; a device of the
// project opens through pq/views/circuit/device_circuit.h (T4.4-fix1: a Host, the document as its history). UI thread only.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/graph/structure.h"
#include "pq/ui/code_editor.h"
#include "pq/ui/overlay.h"
#include "pq/views/circuit/circuit_canvas.h"
#include "pq/views/circuit/patch_editor.h"
#include "pq/views/circuit/probes.h"

namespace pq::ui {
class Label;
class Button;
class Toggle;
}  // namespace pq::ui

namespace pq::views {
class Shell;
}

namespace pq::views::circuit {

// Syntax colours for .pqp text (state 1 = inside a block comment).
uint32_t highlightPatchLine(std::string_view line, uint32_t state, std::vector<ui::CodeSpan>& out);

// The canvas with editing gestures (see above). Reports them; the editor applies them to the document.
class EditCanvas : public CircuitCanvas {
public:
  explicit EditCanvas(std::string name = "circuit");
  void setEditable(bool e) noexcept { editable_ = e; }
  bool editable() const noexcept { return editable_; }
  void setStale(bool s);  // the text does not parse: the canvas shows the last good version under a veil
  // T4.5: the probe readout and cost labels drawn over the scene (owned by the caller; null: none).
  void setProbeOverlay(const ProbeOverlay* o) {
    overlay_ = o;
    markDirty();
  }
  ui::Vec2 pointer() const noexcept { return pointer_; }  // the last pointer position over the canvas (device px)
  void setPointer(ui::Vec2 p) {  // scenes and tests: where the readout is drawn
    pointer_ = p;
    markDirty();
  }

  std::function<void(std::vector<uint32_t> nodes, float dx, float dy)> onMove;  // world units
  // A connection from an output pin to an input pin (scene pin indices; hidden pins included).
  std::function<void(uint32_t fromPin, uint32_t toPin)> onConnect;
  std::function<void(graph::Point2 world)> onAddAt;
  std::function<void(uint32_t node)> onRename;

  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override;
  // T4.16c (what a press would do): a pin Crosshair (drag a wire), a node Move, panning Move; while a wire is dragged
  // Crosshair, while nodes are moved Move. Read-only or stale: the canvas's (Arrow, Move while panning).
  ui::Cursor cursorAt(ui::Vec2 pos) const override;

  // The input / output pin nearest a device point on the node under it or within 8 px (hidden pins included);
  // UINT32_MAX = none.
  uint32_t pinNear(ui::Vec2 device, bool wantInput) const;

protected:
  void paintOverlay(ui::PaintContext& ctx) override;

private:
  enum class Drag : uint8_t { None, MaybeMove, Move, Wire };
  Drag drag_ = Drag::None;
  ui::Vec2 press_{0, 0}, last_{0, 0};
  uint32_t fromPin_ = UINT32_MAX, targetPin_ = UINT32_MAX;
  bool editable_ = true, stale_ = false;
  const ProbeOverlay* overlay_ = nullptr;
  ui::Vec2 pointer_{0, 0};
};

class CircuitEditor : public ui::Popover {
public:
  explicit CircuitEditor(std::unique_ptr<PatchEditor> document);
  ~CircuitEditor() override;

  PatchEditor& document() noexcept { return *doc_; }
  EditCanvas& canvas() noexcept { return *canvas_; }
  ui::CodeEditor& textPane() noexcept { return *text_; }
  const std::string& title() const noexcept { return titleText_; }
  const std::string& status() const noexcept { return statusText_; }

  // Shows a patch of the file ("" = the first), or of an import (read-only). Clears the back stack.
  bool show(std::string_view patch = {});
  bool openNode(uint32_t node);  // a subcircuit's callee (pushes the shown patch)
  bool back();
  // The shown patch's name, and its index in the file (kNone: an imported, read-only patch).
  const std::string& shownPatch() const noexcept { return shown_; }
  size_t shownIndex() const noexcept;
  bool readOnly() const noexcept { return !shown_.empty() && shownIndex() == graph::kNone; }

  // The editor's commands (the keys and gestures above call these; tests script them).
  bool addAt(graph::Point2 world, std::string_view spec);
  bool deleteSelection();
  bool renameNode(uint32_t node, std::string name);
  bool makeSubcircuit(std::string name = {});
  bool makeParam();
  bool undo();
  bool redo();
  bool save();
  bool togglePlay();
  bool tidy();
  // Runs the pending check now (the view runs it ≈ 50 ms after an edit).
  void checkNow();
  // Opens the add / rename type-in at a device point.
  void beginAdd(graph::Point2 world, ui::Vec2 device);
  void beginRename(uint32_t node);

  // ---- T4.5: live probes and cost meters (pq/views/circuit/probes.h) ----
  // Probes the source of scene wire `wire` as a hover dwell does (UINT32_MAX: drops the hover probe at once).
  void probeWire(uint32_t wire);
  // The Cost toggle.
  void setCostShown(bool on);
  bool costShown() const noexcept;
  // Runs the 30 Hz probe poll and the 4 Hz cost poll now (tests; the timers run them otherwise).
  void pollProbes();
  void pollCost();
  const ProbeOverlay& probeOverlay() const noexcept { return overlay_; }
  // The call path of the shown patch inside the playing one (NodeIds of the call nodes), or false when the shown patch
  // is not part of what plays.
  bool shownPath(std::vector<graph::NodeId>& path) const;
  // The probe targets and ids the document compiles in now, and the bool wires lit from them.
  const std::vector<graph::ProbeTarget>& probeTargets() const noexcept { return compiledTargets_; }
  const std::vector<uint32_t>& probeIds() const noexcept { return compiledIds_; }

  // ---- T4.4-fix1: an editor over a device of the project (pq/views/circuit/device_circuit.h) ----
  // The host owns undo / redo (the document's: one history with the Ableton view) and the title; there is no file to
  // save (the project holds the text: Save is hidden, closing never asks) and nothing to start (the project plays the
  // device: Play is hidden; probes and cost follow the player the editor was opened with). It lives as long as the
  // editor and goes first.
  class Host {
  public:
    virtual ~Host() = default;
    virtual bool undo() = 0;
    virtual bool redo() = 0;
    virtual std::string title() const = 0;  // in place of the file name
  };
  void setHost(std::unique_ptr<Host> host);
  Host* host() const noexcept { return host_.get(); }
  // Closes the editor at its next timer tick (safe from inside a callback the editor owns).
  void closeSoon();
  void refreshTitle() { updateTitle(); }
  void showStatus(std::string s) { setStatus(std::move(s)); }  // the host's notes (e.g. the fork)

  // ---- T4.15: tooltips and inactive branches ----
  // The Inactive toggle: draw the branches a structural constant switches off, dimmed (off, the default: hidden).
  void setShowInactive(bool on);
  bool showInactive() const noexcept { return showInactive_; }
  std::function<void(bool on)> onShowInactiveChanged;  // the user flipped the toggle (the view prefs remember it)
  // The instance the editor shows (a device of the project): its structural settings and param values by name, for
  // patch `patch` (the device's patch). Settings decide what is inactive there and in what it calls; param values show
  // in tooltips. A change of the settings re-hides (no compile); of params, nothing until the next tooltip.
  void setInstance(std::string patch, std::vector<graph::ConstArg> settings, std::vector<graph::ConstArg> params);
  // The shown patch's structural constants and inactive nodes / wires (with the arguments it is shown with).
  const graph::StructuralInfo& structure() const noexcept { return structure_; }
  const std::vector<graph::ConstArg>& shownArguments() const noexcept { return shownArgs_; }
  // The tooltip of scene node `node` (what a hover shows after the delay).
  std::string nodeTooltipText(uint32_t node);
  // Shows it now at device position `at`, as a rest of the pointer would (tests, scenes). False without an overlay.
  bool showNodeTooltip(uint32_t node, ui::Vec2 at);

  bool onKey(const ui::KeyEvent& e) override;
  void onTimer(uint32_t id) override;
  ui::InfoText info() const override;

private:
  void documentChanged(const EditorChange& c);
  void rebuildScene(bool fit);
  void refreshMarks();
  void setStatus(std::string s);
  void updateTitle();
  void scheduleCheck();
  void present(const std::string& patch, bool fit);
  const graph::Patch* shownGraph();

  std::unique_ptr<PatchEditor> doc_;
  std::unique_ptr<Host> host_;  // T4.4-fix1 (declared after doc_: destroyed first)
  ui::Button* save_ = nullptr;
  uint32_t closeTimer_ = 0;
  std::string shown_;
  std::vector<std::string> backStack_;
  std::vector<graph::NodeId> keepSelection_;
  std::vector<graph::NodeId> lastIds_;  // NodeId of every node of the scene shown (selection across rebuilds)
  EditCanvas* canvas_ = nullptr;
  ui::CodeEditor* text_ = nullptr;
  ui::Label* title_ = nullptr;
  ui::Label* status_ = nullptr;
  ui::Button* back_ = nullptr;
  ui::Toggle* play_ = nullptr;
  std::string titleText_, statusText_;
  uint32_t checkTimer_ = 0;
  bool closeArmed_ = false;
  graph::Patch importedShown_;  // an imported patch being shown (laid out)
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  // T4.5.
  void hoverChanged(const CanvasHit& hit);
  void refreshProbes(bool recheck);  // the probe set from the LED wires and the hover probe; a recheck if it changed
  void clearProbeDisplay();
  void ensurePoll();
  uint32_t probeIdFor(graph::NodeId node, uint16_t port);
  bool wireSource(uint32_t wire, graph::NodeId& node, uint16_t& port) const;
  std::vector<graph::NodeId> callStack_;  // the call nodes opened from the first shown patch (parallel to backStack_)
  ui::Toggle* cost_ = nullptr;
  ProbeOverlay overlay_;
  uint32_t hoverWire_ = UINT32_MAX;       // the scene wire under the pointer
  uint32_t probeWire_ = UINT32_MAX;       // the wire whose source the hover probe reads
  uint32_t hoverProbe_ = UINT32_MAX;      // that probe's id
  std::vector<std::pair<uint64_t, uint32_t>> probeIdOf_;  // (NodeId << 16 | port) → id, for the shown patch
  uint32_t nextProbeId_ = 1;
  std::vector<std::pair<uint32_t, uint32_t>> ledWires_;   // (scene wire, probe id) of the bool wires
  std::vector<uint8_t> lit_;                              // per scene wire: drawn lit
  std::vector<graph::ProbeTarget> compiledTargets_;
  std::vector<uint32_t> compiledIds_, compiledSignals_;
  uint32_t dwellTimer_ = 0, graceTimer_ = 0, pollTimer_ = 0, costTimer_ = 0;
  std::vector<std::pair<uint64_t, double>> costSmooth_;  // NodeId (wires: | 1 << 63 sums, | 1 << 62 copies) → ns
  double costTotal_ = 0.0;

  // T4.15.
  std::vector<graph::ConstArg> argsForShown() const;
  const graph::Patch* patchNamed(std::string_view name) const;
  bool storedPlacesVisible() const;
  void hideNodeTooltip();
  std::string describeCallee(std::string_view callee) const;
  ui::Toggle* inactive_ = nullptr;
  bool showInactive_ = false;
  graph::StructuralInfo structure_;
  std::vector<graph::ConstArg> shownArgs_;
  std::string instancePatch_;
  std::vector<graph::ConstArg> instanceSettings_, instanceParams_;
  graph::Patch viewPatch_;     // the shown patch as laid out while hidden branches leave the visible ones on their own
  bool viewOverride_ = false;  // viewPatch_ is the base of circuit edits (PatchEditor::setLayoutBase)
  uint32_t tipTimer_ = 0, tipNode_ = UINT32_MAX;
  bool tipShown_ = false;
};

// Opens an editor over the whole window of `from`'s tree (its overlay layer). nullptr without an overlay layer or when
// the patch is not found.
CircuitEditor* openCircuitEditor(ui::Widget& from, std::unique_ptr<PatchEditor> document, std::string_view patch = {},
                                 PatchPlayer* player = nullptr);

// The player the editors opened from this shell's View menu use (the app's audio preview). Null: no playback.
void setPatchPlayer(Shell& shell, PatchPlayer* player);

}  // namespace pq::views::circuit
