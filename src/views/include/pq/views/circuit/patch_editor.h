#pragma once

// The circuit editor's document (T4.4, SPEC §7.3): one .pqp file edited through two equal views — the circuit
// canvas and the text pane — with ONE source of truth, the text, and one undo history.
//
// Every edit ends as new text, parsed with the previous parse (ParseOptions::previous, T1.x-text-ids), so NodeIds and
// hot-swap state keys survive edits from either pane:
//   text pane      replaceText(offset, length, text): the text changes; the parse follows.
//   circuit        an IR edit (pq/graph/edit.h) on the drawn patch (stored layout completed for drawing), then:
//                  layout-only edits (move, tidy) → graph::spliceLayout: only the layout block changes, hand
//                  formatting stays; structural edits → that patch's text [`patch` … `}`] is reprinted (print),
//                  other patches keep their text. The re-parse takes its NodeIds from the edited IR, so a new node
//                  keeps the id the edit gave it (the view selects it).
// Parsing is lenient where an editor needs it: stale layout keys are dropped (lenientLayout, positions carried by
// NodeId: graph::carryLayout) and missing call arguments are validation errors (deferMissingInputs), so a circuit
// with an unwired required input still has text. A text that does not lower (syntax, names) keeps the last good parse
// on the canvas (the view dims it) and its errors in diagnostics(); circuit edits are refused until it lowers again.
//
// Checking (check(), the view runs it ≈ 50 ms after the last edit, SPEC §7.5): infer → optimise → schedule of the
// shown patch, at the player's sample rate; errors become diagnostics at their node's text. A clean schedule of the
// playing patch goes to the PatchPlayer (hot swap). Nothing that fails ever reaches the player: the last good program
// keeps playing.
//
// Undo: one history over both panes. A step is a text replacement (offset, removed, inserted); typing coalesces into
// one step per run (no newline, contiguous, < 1.5 s apart), a circuit edit or a paste is one step. Undo / redo replay
// the inverse / the step through the same parse path.
//
// Imported files are read-only here (stock subcircuits are shared files; editing a stock device copies it, T4.1).
// UI thread only; nothing here runs on the audio thread.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/edit.h"
#include "pq/graph/infer.h"
#include "pq/graph/parse.h"
#include "pq/views/circuit/circuit_viewer.h"
#include "pq/views/circuit/patch_player.h"

namespace pq::views::circuit {

struct EditorDiagnostic {
  enum class Phase : uint8_t { Syntax, Validation, Check, Warning };
  Phase phase = Phase::Syntax;
  uint32_t offset = 0, length = 0;  // byte span in text()
  uint32_t line = 0, col = 0;       // 1-based
  std::string message;
};

enum class EditSource : uint8_t { TextPane, Circuit, Undo, Redo, Load };

struct EditorChange {
  EditSource source = EditSource::Load;
  bool text = false;         // text() changed: [offset, offset + removed) was replaced by `inserted` bytes
  uint32_t offset = 0, removed = 0, inserted = 0;
  bool graph = false;        // parsed() changed (the canvas rebuilds)
  bool diagnostics = false;  // diagnostics() changed
  uint32_t caret = 0;        // where a text pane's caret goes after an undo / redo / circuit edit
};

class PatchEditor {
public:
  static Result<std::unique_ptr<PatchEditor>> open(const std::string& path);
  // `path` names the file (imports resolve relative to it; save() writes it).
  static Result<std::unique_ptr<PatchEditor>> fromText(std::string path, std::string text);
  ~PatchEditor();
  PatchEditor(const PatchEditor&) = delete;
  PatchEditor& operator=(const PatchEditor&) = delete;

  const std::string& path() const noexcept;
  const std::string& text() const noexcept;
  bool dirty() const noexcept;          // text() differs from the file
  bool textLowered() const noexcept;    // the current text is what parsed() holds
  // The last text that lowered: its parse (NodeIds carried across edits) and, per patch, the patch with the layout the
  // canvas draws (stored layout + carried positions + completed for the nodes without one).
  const graph::ParseResult& parsed() const noexcept;
  const graph::Patch& drawn(size_t patch);
  std::span<const EditorDiagnostic> diagnostics() const noexcept;
  // Imported files (read-only), for callees.
  const std::vector<std::shared_ptr<const PatchDocument>>& imports() const noexcept;
  // The patch named `name` of this file (kNone when absent); the patch's callee lookup through imports.
  size_t patchIndex(std::string_view name) const noexcept;
  const graph::Patch* findCallee(std::string_view name) const noexcept;
  // Signatures of every patch callable from this file (its own, then its imports').
  std::vector<graph::SubcircuitRef> callees() const;

  // ---- edits (each one history step; refused edits change nothing and say why) ------------------------------------
  // Text pane. `typing`: may coalesce with the previous typing step.
  void replaceText(uint32_t offset, uint32_t length, std::string_view with, bool typing = true);
  // Circuit edits on patch `patch` (node / wire indices of drawn(patch)).
  Result<graph::NodeId> addNode(size_t patch, std::string_view spec, graph::Point2 at);
  Status connect(size_t patch, graph::PortRef from, graph::PortRef to);
  Status erase(size_t patch, std::span<const graph::NodeIndex> nodes, std::span<const graph::WireIndex> wires);
  Status rename(size_t patch, graph::NodeIndex node, std::string name);
  Result<graph::NodeId> makeParam(size_t patch, graph::NodeIndex constNode);
  // Returns the new patch's index.
  Result<size_t> makeSubcircuit(size_t patch, std::span<const graph::NodeIndex> nodes, std::string name = {});
  Status moveNodes(size_t patch, std::span<const graph::NodeIndex> nodes, float dx, float dy);
  Status tidy(size_t patch);
  bool canUndo() const noexcept;
  bool canRedo() const noexcept;
  bool undo();
  bool redo();
  size_t historySize() const noexcept;  // steps that undo() can take
  // A one-line note on the last edit (e.g. "inserted a z1: the wire closed a loop with no delay"), or "".
  const std::string& note() const noexcept;

  // Writes the file: the text with the drawn layout spliced in (graph::spliceLayout keeps hand formatting; the
  // splice is a history step when it changes the text), atomically (temp + rename).
  Status save();

  // ---- check + playback ----------------------------------------------------------------------------------------------
  // Infer → optimise → schedule patch `patch` (kNone: the playing patch, else the first) of the current parse; its
  // errors join diagnostics(). With a player playing this patch, a clean schedule hot-swaps it in. Returns the check's
  // error, if any.
  Status check(size_t patch = graph::kNone);
  bool checkPending() const noexcept;   // the text changed since the last check
  void setPlayer(PatchPlayer* player) noexcept;
  PatchPlayer* player() const noexcept;
  Status play(size_t patch);            // checks the patch and starts it
  void stop();
  bool playing() const noexcept;
  const std::string& playingPatch() const noexcept;  // "" when stopped
  // The last check's schedule statistics.
  struct CheckStats {
    uint64_t checks = 0, swaps = 0, refusedSwaps = 0;  // swaps: schedules handed to the player
    double lastCheckMs = 0.0, lastParseMs = 0.0;
    graph::IdMatchStats lastIdMatch;
  };
  const CheckStats& stats() const noexcept;

  // Every change (both panes and the canvas listen).
  std::function<void(const EditorChange&)> onChange;
  // After every edit whose text lowered: the new text and its parse. The seam for T4.1 / T4.13 — a device patch
  // edited in place hands its text to the document here (one command per edit); the stand-alone editor has no use for
  // it (the file is written by save()).
  std::function<void(const std::string& text, const graph::ParseResult& parsed)> onCommitted;

  struct Impl;

private:
  explicit PatchEditor(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace pq::views::circuit
