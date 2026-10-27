#pragma once

// Opening any .pqp in the circuit canvas (T4.2/T4.3; read-only, editing is T4.4): a full-window viewer in the shell's
// overlay — a title bar (file › patch, Back, Tidy, Fit, Close) over a CircuitCanvas. Double-clicking a subcircuit or
// stock filter opens its callee (this file's or an imported file's patch), Back returns. Tidy re-lays out the shown
// patch with the auto-layout and router (graph/layout.h); laidOutText() is the file's text with that layout spliced in
// (graph::spliceLayout: only the layout block changes), which is what a "save layout" writes (T4.4 saves it).
//
// View > Open Patch in Circuit View… (`view.openPatch`) asks for a file (FileDialogs::choosePatchFile) and opens it.
// `pqdaw --screenshot circuit out.png --project <file.pqp>` renders the first patch of a file (view scenes).

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/parse.h"
#include "pq/ui/overlay.h"
#include "pq/views/circuit/circuit_canvas.h"

namespace pq::views {
class Shell;
}

namespace pq::views::circuit {

// A .pqp file read for viewing: its text, its parse (imports resolved relative to it) and the files it imports.
struct PatchDocument {
  std::string path;
  std::string source;
  graph::ParseResult parsed;
  std::vector<std::shared_ptr<const PatchDocument>> imports;

  // A patch of this file, else of its imports (depth first). nullptr when absent.
  const graph::Patch* find(std::string_view name) const noexcept;
  // The document that holds `patch` (this one or an import).
  const PatchDocument* owner(const graph::Patch& patch) const noexcept;
};

// Reads and parses `path` and, recursively, what it imports. Parse errors are returned formatted (file:line:col).
Result<std::shared_ptr<const PatchDocument>> loadPatchDocument(const std::string& path);
// The same from text in memory (imports relative to `path`'s directory).
Result<std::shared_ptr<const PatchDocument>> loadPatchDocument(std::string path, std::string source);

class CircuitViewer : public ui::Popover {
public:
  explicit CircuitViewer(std::shared_ptr<const PatchDocument> doc);

  CircuitCanvas& canvas() noexcept { return *canvas_; }
  // Shows a patch of the document (or of its imports); "" = the first patch. Clears the back stack.
  bool show(std::string_view patchName = {});
  // Opens the callee of a subcircuit node of the shown patch (pushes the shown patch). False when it is not found.
  bool openNode(uint32_t node);
  bool back();
  // T4.8: shows `doc` instead (a rack's subcircuit rebuilt after an edit): the patch of the same name stays shown
  // without refitting the view, else the document's first patch; the back stack is cleared. False when it is empty.
  bool replaceDocument(std::shared_ptr<const PatchDocument> doc);
  const PatchDocument& document() const noexcept { return *doc_; }
  const graph::Patch* shownPatch() const noexcept { return shown_; }
  const graph::Patch& laidOut() const noexcept { return laidOut_; }  // the shown patch with its drawn layout
  // Re-places and re-routes the shown patch (graph::autoLayout with the symbol geometry), in memory.
  graph::AutoLayoutStats tidy();
  // The shown patch's file text with laidOut()'s layout spliced in.
  Result<std::string> laidOutText() const;
  const std::string& title() const noexcept { return titleText_; }

  bool onKey(const ui::KeyEvent& e) override;
  ui::InfoText info() const override;

private:
  void present(const graph::Patch& p, bool fit);
  std::shared_ptr<const PatchDocument> doc_;
  const graph::Patch* shown_ = nullptr;
  graph::Patch laidOut_;
  std::vector<const graph::Patch*> backStack_;
  CircuitCanvas* canvas_ = nullptr;
  ui::Label* title_ = nullptr;
  ui::Button* back_ = nullptr;
  std::string titleText_;
};

// The symbol sheet (the `circuit-symbols` golden): every symbol and glyph, captioned, and a row of wire styles.
std::shared_ptr<const PatchDocument> symbolSheet();

// Opens a viewer over the whole window of `from`'s tree (its overlay layer). nullptr without an overlay layer or when
// the patch is not found.
CircuitViewer* openCircuitViewer(ui::Widget& from, std::shared_ptr<const PatchDocument> doc, std::string_view patch = {});

// The `view.openPatch` action (View > Open Patch in Circuit View…): file dialog → viewer; errors go to the status line.
void installCircuitViewer(Shell& shell);

}  // namespace pq::views::circuit
