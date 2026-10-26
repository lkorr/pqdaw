#pragma once

// A multi-line code editor (T4.4: the circuit editor's text pane; generic, so other text views can use it): UTF-8 text,
// caret and selection at code point boundaries, mouse and keyboard editing, syntax colouring through a highlighter,
// inline diagnostics (a wavy underline under the span, a marker in the gutter and the message at the end of the line),
// line numbers, its own scrolling.
//
// Ownership of edits: a user edit changes the editor's text at once and is reported through onEdit(offset, removed,
// inserted, typing); undo is the owner's (Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z are not handled here and bubble up), which
// applies the inverse through replace(). Keys: arrows (Ctrl: by word), Home / End (Ctrl: of the text), Page Up / Down,
// Shift extends the selection; Backspace / Delete (Ctrl: by word), Enter keeps the line's indentation, Tab inserts
// two spaces (Shift+Tab removes up to two before the caret's line), Ctrl+A / C / X / V. Mouse: click places the caret,
// drag selects, double-click selects a word, Shift+click extends; the wheel scrolls (Shift: horizontally).
//
// Cost: the text is one string plus a line-start table (rebuilt in O(n) per edit, ≈ µs for the stdlib's largest file)
// and the highlighter's per-line start state (one O(n) pass per edit); painting shapes only the visible lines, one run
// per coloured token (cached by the text system). The caret does not blink, so a focused editor is idle (no timers,
// no frames) between key presses.
//
// UI thread only.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "pq/ui/widget.h"

namespace pq::ui {

enum class CodeStyle : uint8_t { Text, Keyword, Number, String, Comment, Name, Punctuation };
inline constexpr uint32_t kCodeStyleCount = 7;

// A coloured part of one line (byte offsets relative to the line start).
struct CodeSpan {
  uint32_t begin = 0, end = 0;
  CodeStyle style = CodeStyle::Text;
};

// Colours one line. `state` is the highlighter's state at the line's start (0 at the top of the text); returns the
// state at its end (e.g. "inside a block comment"). Spans in order, non-overlapping; gaps are Text.
using CodeHighlighter = std::function<uint32_t(std::string_view line, uint32_t state, std::vector<CodeSpan>& out)>;

struct CodeMark {
  uint32_t begin = 0, end = 0;  // byte span in the text (end == begin: a point; drawn one character wide)
  bool warning = false;         // else an error
  std::string message;          // shown at the end of the span's first line
};

class CodeEditor : public Widget {
public:
  explicit CodeEditor(std::string name = "code");
  ~CodeEditor() override;

  const std::string& text() const noexcept { return text_; }
  // Programmatic: the whole text (caret to `caret`, selection cleared, scroll kept where possible). No onEdit.
  void setText(std::string text, uint32_t caret = 0);
  // Programmatic replacement of [offset, offset + removed) (undo, the other pane): caret and selection follow the
  // edit (or go to `caret` when given), the scroll keeps the caret in view. No onEdit.
  void replace(uint32_t offset, uint32_t removed, std::string_view inserted, uint32_t caret = UINT32_MAX);
  void setHighlighter(CodeHighlighter h);
  void setMarks(std::vector<CodeMark> marks);
  const std::vector<CodeMark>& marks() const noexcept { return marks_; }
  void setReadOnly(bool r) noexcept { readOnly_ = r; }
  bool readOnly() const noexcept { return readOnly_; }

  uint32_t caret() const noexcept { return caret_; }
  uint32_t anchor() const noexcept { return anchor_; }
  bool hasSelection() const noexcept { return caret_ != anchor_; }
  uint32_t selBegin() const noexcept { return caret_ < anchor_ ? caret_ : anchor_; }
  uint32_t selEnd() const noexcept { return caret_ < anchor_ ? anchor_ : caret_; }
  void setCaret(uint32_t byte, bool extend = false);
  void select(uint32_t begin, uint32_t end);  // anchor = begin, caret = end
  // Scrolls so that `byte` is visible (centred vertically when it was off screen).
  void reveal(uint32_t byte);

  uint32_t lineCount() const noexcept { return static_cast<uint32_t>(lineStart_.size()); }
  uint32_t lineOf(uint32_t byte) const noexcept;       // 0-based
  uint32_t lineStart(uint32_t line) const noexcept;
  uint32_t lineEnd(uint32_t line) const noexcept;      // before its '\n' (and '\r')
  // The byte under a device point (nearest caret stop on the nearest line).
  uint32_t byteAt(Vec2 device);
  // Device position of the caret stop at `byte` (top-left of its line box).
  Vec2 positionOf(uint32_t byte);
  int32_t lineHeight() const noexcept;                  // device px (after a paint or layout)

  // Every user edit: [offset, offset + removed) became `inserted`. typing: a single typed character or a backspace /
  // delete (an owner may coalesce those into one undo step).
  std::function<void(uint32_t offset, uint32_t removed, std::string_view inserted, bool typing)> onEdit;
  std::function<void()> onCaretMoved;

  struct Stats {
    uint64_t paints = 0, linesPainted = 0, runsShaped = 0, edits = 0;
  };
  const Stats& stats() const noexcept { return stats_; }

  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;
  bool onKey(const KeyEvent& e) override;
  bool onText(const TextEvent& e) override;
  void onFocusChanged(bool focused) override;
  InfoText info() const override;

private:
  void rebuildLines();
  void rebuildStates(uint32_t fromLine);
  void userEdit(uint32_t offset, uint32_t removed, std::string_view inserted, bool typing);
  void keepCaretVisible();
  uint32_t prevBoundary(uint32_t i) const noexcept;
  uint32_t nextBoundary(uint32_t i) const noexcept;
  uint32_t wordLeft(uint32_t i) const noexcept;
  uint32_t wordRight(uint32_t i) const noexcept;
  bool metrics();  // fonts / sizes from the tree's resources; false without them
  int32_t xInLine(uint32_t line, uint32_t byte);
  uint32_t byteInLine(uint32_t line, int32_t x);
  void moveVertical(int32_t lines, bool extend);

  std::string text_;
  std::vector<uint32_t> lineStart_{0};
  std::vector<uint32_t> lineState_{0};  // highlighter state at each line's start
  CodeHighlighter highlighter_;
  std::vector<CodeMark> marks_;
  std::vector<CodeSpan> spans_;  // scratch
  uint32_t caret_ = 0, anchor_ = 0;
  int32_t goalX_ = -1;           // remembered x for vertical moves
  int32_t scrollX_ = 0, scrollY_ = 0;
  bool readOnly_ = false;
  bool selecting_ = false;
  bool pendingReveal_ = false;
  // Device metrics from the last paint / measurement.
  uint32_t font_ = UINT32_MAX;  // a FontId (pq/ui/text.h)
  bool haveFont_ = false;
  int32_t lineH_ = 14, ascent_ = 10, gutterW_ = 32, pad_ = 4, charW_ = 7;
  Stats stats_;
};

}  // namespace pq::ui
