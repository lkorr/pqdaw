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
// View > Open Patch in Circuit View… (`view.openPatch`) opens a file in it. UI thread only.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/ui/code_editor.h"
#include "pq/ui/overlay.h"
#include "pq/views/circuit/circuit_canvas.h"
#include "pq/views/circuit/patch_editor.h"

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

  std::function<void(std::vector<uint32_t> nodes, float dx, float dy)> onMove;  // world units
  // A connection from an output pin to an input pin (scene pin indices; hidden pins included).
  std::function<void(uint32_t fromPin, uint32_t toPin)> onConnect;
  std::function<void(graph::Point2 world)> onAddAt;
  std::function<void(uint32_t node)> onRename;

  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override;

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
  bool readOnly() const noexcept { return shownIndex() == graph::kNone; }

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
};

// Opens an editor over the whole window of `from`'s tree (its overlay layer). nullptr without an overlay layer or when
// the patch is not found.
CircuitEditor* openCircuitEditor(ui::Widget& from, std::unique_ptr<PatchEditor> document, std::string_view patch = {},
                                 PatchPlayer* player = nullptr);

// The player the editors opened from this shell's View menu use (the app's audio preview). Null: no playback.
void setPatchPlayer(Shell& shell, PatchPlayer* player);

}  // namespace pq::views::circuit
