// Multi-line code editor (T4.4): see pq/ui/code_editor.h.

#include "pq/ui/code_editor.h"

#include <algorithm>
#include <cmath>

#include "controls_paint.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"

namespace pq::ui {

namespace {

bool isWordByte(unsigned char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}
bool isSpace(unsigned char c) noexcept { return c == ' ' || c == '\t'; }

Rgba styleColour(const Theme::Code& c, CodeStyle s) noexcept {
  switch (s) {
    case CodeStyle::Keyword: return c.keyword;
    case CodeStyle::Number: return c.number;
    case CodeStyle::String: return c.string;
    case CodeStyle::Comment: return c.comment;
    case CodeStyle::Name: return c.name;
    case CodeStyle::Punctuation: return c.punctuation;
    case CodeStyle::Text: break;
  }
  return c.text;
}

}  // namespace

CodeEditor::CodeEditor(std::string name) : Widget(std::move(name)) {
  setFocusable(true);
  setAcceptsTextInput(true);
  setClipChildren(true);
}

CodeEditor::~CodeEditor() = default;

// ---- text model ------------------------------------------------------------------------------------------------------

void CodeEditor::rebuildLines() {
  lineStart_.clear();
  lineStart_.push_back(0);
  const char* s = text_.data();
  const size_t n = text_.size();
  for (size_t i = 0; i < n; ++i)
    if (s[i] == '\n') lineStart_.push_back(static_cast<uint32_t>(i + 1));
}

void CodeEditor::rebuildStates(uint32_t fromLine) {
  const uint32_t lines = lineCount();
  if (lineState_.size() != lines) lineState_.resize(lines, 0);
  if (!highlighter_) {
    std::fill(lineState_.begin(), lineState_.end(), 0u);
    return;
  }
  fromLine = std::min(fromLine, lines - 1);
  if (fromLine == 0) lineState_[0] = 0;
  uint32_t state = lineState_[fromLine];
  for (uint32_t l = fromLine; l < lines; ++l) {
    lineState_[l] = state;
    spans_.clear();
    const std::string_view line = std::string_view(text_).substr(lineStart_[l], lineEnd(l) - lineStart_[l]);
    state = highlighter_(line, state, spans_);
  }
}

uint32_t CodeEditor::lineOf(uint32_t byte) const noexcept {
  const auto it = std::upper_bound(lineStart_.begin(), lineStart_.end(), byte);
  return static_cast<uint32_t>(it - lineStart_.begin()) - 1;
}

uint32_t CodeEditor::lineStart(uint32_t line) const noexcept {
  return line < lineStart_.size() ? lineStart_[line] : static_cast<uint32_t>(text_.size());
}

uint32_t CodeEditor::lineEnd(uint32_t line) const noexcept {
  uint32_t e = line + 1 < lineStart_.size() ? lineStart_[line + 1] - 1 : static_cast<uint32_t>(text_.size());
  if (e > lineStart(line) && text_[e - 1] == '\r') --e;
  return e;
}

uint32_t CodeEditor::prevBoundary(uint32_t i) const noexcept {
  if (i == 0) return 0;
  --i;
  while (i > 0 && (static_cast<unsigned char>(text_[i]) & 0xC0) == 0x80) --i;
  if (i > 0 && text_[i] == '\n' && text_[i - 1] == '\r') --i;
  return i;
}

uint32_t CodeEditor::nextBoundary(uint32_t i) const noexcept {
  const uint32_t n = static_cast<uint32_t>(text_.size());
  if (i >= n) return n;
  if (text_[i] == '\r' && i + 1 < n && text_[i + 1] == '\n') return i + 2;
  ++i;
  while (i < n && (static_cast<unsigned char>(text_[i]) & 0xC0) == 0x80) ++i;
  return i;
}

uint32_t CodeEditor::wordLeft(uint32_t i) const noexcept {
  while (i > 0 && isSpace(static_cast<unsigned char>(text_[i - 1]))) --i;
  if (i == 0) return 0;
  const bool word = isWordByte(static_cast<unsigned char>(text_[i - 1]));
  if (text_[i - 1] == '\n') return prevBoundary(i);
  while (i > 0 && text_[i - 1] != '\n' && !isSpace(static_cast<unsigned char>(text_[i - 1])) &&
         isWordByte(static_cast<unsigned char>(text_[i - 1])) == word)
    --i;
  return i;
}

uint32_t CodeEditor::wordRight(uint32_t i) const noexcept {
  const uint32_t n = static_cast<uint32_t>(text_.size());
  if (i < n && (text_[i] == '\n' || text_[i] == '\r')) return nextBoundary(i);
  if (i < n && !isSpace(static_cast<unsigned char>(text_[i]))) {
    const bool word = isWordByte(static_cast<unsigned char>(text_[i]));
    while (i < n && text_[i] != '\n' && text_[i] != '\r' && !isSpace(static_cast<unsigned char>(text_[i])) &&
           isWordByte(static_cast<unsigned char>(text_[i])) == word)
      ++i;
  }
  while (i < n && isSpace(static_cast<unsigned char>(text_[i]))) ++i;
  return i;
}

void CodeEditor::setText(std::string text, uint32_t caret) {
  text_ = std::move(text);
  rebuildLines();
  lineState_.assign(lineCount(), 0);
  rebuildStates(0);
  caret_ = anchor_ = std::min<uint32_t>(caret, static_cast<uint32_t>(text_.size()));
  goalX_ = -1;
  markDirty();
}

void CodeEditor::replace(uint32_t offset, uint32_t removed, std::string_view inserted, uint32_t caret) {
  offset = std::min<uint32_t>(offset, static_cast<uint32_t>(text_.size()));
  removed = std::min<uint32_t>(removed, static_cast<uint32_t>(text_.size()) - offset);
  text_.replace(offset, removed, inserted);
  const uint32_t line = lineOf(offset);
  rebuildLines();
  rebuildStates(line);
  auto map = [&](uint32_t b) -> uint32_t {
    if (b <= offset) return b;
    if (b >= offset + removed) return b - removed + static_cast<uint32_t>(inserted.size());
    return offset + static_cast<uint32_t>(inserted.size());
  };
  if (caret != UINT32_MAX) {
    caret_ = anchor_ = std::min<uint32_t>(caret, static_cast<uint32_t>(text_.size()));
    keepCaretVisible();
  } else {
    caret_ = map(caret_);
    anchor_ = map(anchor_);
  }
  goalX_ = -1;
  markDirty();
}

void CodeEditor::setHighlighter(CodeHighlighter h) {
  highlighter_ = std::move(h);
  lineState_.assign(lineCount(), 0);
  rebuildStates(0);
  markDirty();
}

void CodeEditor::setMarks(std::vector<CodeMark> marks) {
  marks_ = std::move(marks);
  markDirty();
}

void CodeEditor::setCaret(uint32_t byte, bool extend) {
  byte = std::min<uint32_t>(byte, static_cast<uint32_t>(text_.size()));
  while (byte > 0 && byte < text_.size() && (static_cast<unsigned char>(text_[byte]) & 0xC0) == 0x80) --byte;
  caret_ = byte;
  if (!extend) anchor_ = byte;
  keepCaretVisible();
  markDirty();
  if (onCaretMoved) onCaretMoved();
}

void CodeEditor::select(uint32_t begin, uint32_t end) {
  anchor_ = std::min<uint32_t>(begin, static_cast<uint32_t>(text_.size()));
  caret_ = std::min<uint32_t>(end, static_cast<uint32_t>(text_.size()));
  keepCaretVisible();
  markDirty();
  if (onCaretMoved) onCaretMoved();
}

void CodeEditor::userEdit(uint32_t offset, uint32_t removed, std::string_view inserted, bool typing) {
  if (readOnly_) return;
  if (removed == 0 && inserted.empty()) return;
  const std::string ins(inserted);  // `inserted` may point into text_ (paste of a selection)
  replace(offset, removed, ins, offset + static_cast<uint32_t>(ins.size()));
  ++stats_.edits;
  if (onEdit) onEdit(offset, removed, ins, typing);
  if (onCaretMoved) onCaretMoved();
}

// ---- metrics and positions -------------------------------------------------------------------------------------------

bool CodeEditor::metrics() {
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  if (!res || !res->text || !res->uiFonts || !res->theme) return false;
  const UiScale sc = tree()->uiScale();
  font_ = res->uiFonts->body;
  haveFont_ = true;
  const FontMetrics& fm = res->text->metrics(font_);
  ascent_ = fm.ascent;
  lineH_ = fm.ascent + fm.descent + sc.px(3);
  pad_ = sc.px(6);
  charW_ = std::max(1, res->text->measure(font_, "0"));
  const uint32_t digits = lineCount() >= 10000 ? 5 : lineCount() >= 1000 ? 4 : 3;
  gutterW_ = static_cast<int32_t>(digits) * charW_ + sc.px(16);
  return true;
}

int32_t CodeEditor::lineHeight() const noexcept { return lineH_; }

int32_t CodeEditor::xInLine(uint32_t line, uint32_t byte) {
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  if (!res || !res->text || !haveFont_) return 0;
  const uint32_t b = lineStart(line), e = lineEnd(line);
  const TextRun run = res->text->shape(font_, std::string_view(text_).substr(b, e - b));
  return run.caretX(std::clamp(byte, b, e) - b);
}

uint32_t CodeEditor::byteInLine(uint32_t line, int32_t x) {
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  const uint32_t b = lineStart(line), e = lineEnd(line);
  if (!res || !res->text || !haveFont_) return e;
  const TextRun run = res->text->shape(font_, std::string_view(text_).substr(b, e - b));
  return b + std::min(run.hitTest(x), e - b);
}

uint32_t CodeEditor::byteAt(Vec2 d) {
  if (!metrics()) return caret_;
  const IRect r = rect();
  const int32_t y = static_cast<int32_t>(std::floor(d.y)) - r.y + scrollY_;
  const uint32_t line = static_cast<uint32_t>(std::clamp<int32_t>(y / std::max(1, lineH_), 0, static_cast<int32_t>(lineCount()) - 1));
  const int32_t x = static_cast<int32_t>(std::lround(d.x)) - (r.x + gutterW_ + pad_) + scrollX_;
  return byteInLine(line, x);
}

Vec2 CodeEditor::positionOf(uint32_t byte) {
  if (!metrics()) return {static_cast<float>(rect().x), static_cast<float>(rect().y)};
  const uint32_t line = lineOf(byte);
  const IRect r = rect();
  return {static_cast<float>(r.x + gutterW_ + pad_ - scrollX_ + xInLine(line, byte)),
          static_cast<float>(r.y + static_cast<int32_t>(line) * lineH_ - scrollY_)};
}

void CodeEditor::keepCaretVisible() {
  if (!metrics() || rect().empty()) {
    pendingReveal_ = true;
    return;
  }
  const IRect r = rect();
  const uint32_t line = lineOf(caret_);
  const int32_t top = static_cast<int32_t>(line) * lineH_;
  if (top < scrollY_) scrollY_ = top;
  if (top + lineH_ > scrollY_ + r.h) scrollY_ = top + lineH_ - r.h;
  const int32_t viewW = std::max(1, r.w - gutterW_ - 2 * pad_);
  const int32_t x = xInLine(line, caret_);
  if (x < scrollX_) scrollX_ = std::max(0, x - viewW / 4);
  if (x > scrollX_ + viewW - 2) scrollX_ = x - viewW + viewW / 4;
  scrollY_ = std::max(0, scrollY_);
  pendingReveal_ = false;
}

void CodeEditor::reveal(uint32_t byte) {
  if (!metrics() || rect().empty()) return;
  const uint32_t line = lineOf(std::min<uint32_t>(byte, static_cast<uint32_t>(text_.size())));
  const int32_t top = static_cast<int32_t>(line) * lineH_;
  if (top < scrollY_ || top + lineH_ > scrollY_ + rect().h) scrollY_ = std::max(0, top - rect().h / 2);
  markDirty();
}

void CodeEditor::moveVertical(int32_t lines, bool extend) {
  const uint32_t line = lineOf(caret_);
  if (!metrics()) {  // no fonts (headless): keep the byte column
    const int32_t t = std::clamp<int32_t>(static_cast<int32_t>(line) + lines, 0, static_cast<int32_t>(lineCount()) - 1);
    const uint32_t col = caret_ - lineStart(line);
    setCaret(std::min(lineStart(static_cast<uint32_t>(t)) + col, lineEnd(static_cast<uint32_t>(t))), extend);
    return;
  }
  if (goalX_ < 0) goalX_ = xInLine(line, caret_);
  const int32_t target = std::clamp<int32_t>(static_cast<int32_t>(line) + lines, 0, static_cast<int32_t>(lineCount()) - 1);
  const int32_t gx = goalX_;
  uint32_t b;
  if (target == static_cast<int32_t>(line) && lines < 0) b = 0;
  else if (target == static_cast<int32_t>(line) && lines > 0) b = static_cast<uint32_t>(text_.size());
  else b = byteInLine(static_cast<uint32_t>(target), gx);
  setCaret(b, extend);
  goalX_ = gx;
}

// ---- paint -------------------------------------------------------------------------------------------------------------

void CodeEditor::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const Theme::Code& c = th->code;
  const IRect r = rect();
  ++stats_.paints;
  ctx.dl.pushClip(r.toRect());
  ctx.dl.addRectFilled(r.toRect(), c.background);
  if (!ctx.res.text || !ctx.res.uiFonts || !metrics()) {
    ctx.dl.popClip();
    return;
  }
  if (pendingReveal_) keepCaretVisible();
  TextSystem& ts = *ctx.res.text;
  const IRect gutter{r.x, r.y, gutterW_, r.h};
  ctx.dl.addRectFilled(gutter.toRect(), c.gutter);
  const int32_t codeX = r.x + gutterW_;
  const int32_t x0 = codeX + pad_ - scrollX_;
  const uint32_t lines = lineCount();
  const int32_t clipTop = std::max(r.y, ctx.clip.y), clipBottom = std::min(r.bottom(), ctx.clip.bottom());
  const uint32_t first = static_cast<uint32_t>(std::max(0, (clipTop - r.y + scrollY_) / std::max(1, lineH_)));
  const uint32_t last = std::min<uint32_t>(lines, static_cast<uint32_t>(std::max(0, (clipBottom - r.y + scrollY_) / std::max(1, lineH_) + 1)));
  const uint32_t caretLine = lineOf(caret_);
  const uint32_t sb = selBegin(), se = selEnd();
  const float px1 = std::max(1.0f, std::round(ctx.scale.exact(1.0f)));
  const Rect codeClip{static_cast<float>(codeX), static_cast<float>(r.y), static_cast<float>(r.right()), static_cast<float>(r.bottom())};
  // Lines with a mark, for the gutter.
  for (uint32_t line = first; line < last; ++line) {
    const int32_t y = r.y + static_cast<int32_t>(line) * lineH_ - scrollY_;
    const float fy = static_cast<float>(y), fy1 = static_cast<float>(y + lineH_);
    const uint32_t b = lineStart_[line], e = lineEnd(line);
    const std::string_view lineText = std::string_view(text_).substr(b, e - b);
    ++stats_.linesPainted;
    if (line == caretLine && focused())
      ctx.dl.addRectFilled({static_cast<float>(codeX), fy, static_cast<float>(r.right()), fy1}, c.currentLine);
    const TextRun run = ts.shape(font_, lineText);
    ++stats_.runsShaped;
    const int32_t base = y + (lineH_ - (ascent_ + ts.metrics(font_).descent)) / 2 + ascent_;
    ctx.dl.pushClip(codeClip);
    // Selection.
    const uint32_t lineNext = line + 1 < lines ? lineStart_[line + 1] : static_cast<uint32_t>(text_.size());
    if (sb != se && sb < lineNext && se >= b && !(se == b && sb < b)) {
      const uint32_t a = std::max(sb, b), z = std::min(se, e);
      float xa = static_cast<float>(x0 + run.caretX(a - b)), xz = static_cast<float>(x0 + run.caretX(z - b));
      if (se > e && line + 1 < lines) xz += static_cast<float>(charW_ / 2);  // the newline is selected
      if (xz > xa) ctx.dl.addRectFilled({xa, fy, xz, fy1}, c.selection);
    }
    // Tokens.
    spans_.clear();
    if (highlighter_) (void)highlighter_(lineText, line < lineState_.size() ? lineState_[line] : 0, spans_);
    uint32_t at = 0;
    auto draw = [&](uint32_t from, uint32_t to, CodeStyle s) {
      if (to <= from) return;
      const std::string_view part = lineText.substr(from, to - from);
      if (part.find_first_not_of(" \t") == std::string_view::npos) return;
      const TextRun pr = ts.shape(font_, part);
      ++stats_.runsShaped;
      TextSystem::drawRun(ctx.dl, pr, {static_cast<float>(x0 + run.caretX(from)), static_cast<float>(base)}, styleColour(c, s));
    };
    for (const CodeSpan& sp : spans_) {
      const uint32_t sb2 = std::min<uint32_t>(sp.begin, static_cast<uint32_t>(lineText.size()));
      const uint32_t se2 = std::min<uint32_t>(sp.end, static_cast<uint32_t>(lineText.size()));
      draw(at, sb2, CodeStyle::Text);
      draw(sb2, se2, sp.style);
      at = std::max(at, se2);
    }
    draw(at, static_cast<uint32_t>(lineText.size()), CodeStyle::Text);
    // Marks: a wavy underline under the span's part on this line, the message after the line's text.
    bool lineMarked = false, lineError = false;
    for (const CodeMark& m : marks_) {
      const uint32_t mb = m.begin, me = std::max(m.end, m.begin + 1);
      const uint32_t nextLine = line + 1 < lines ? lineStart_[line + 1] : static_cast<uint32_t>(text_.size()) + 1;
      if (mb >= nextLine || me <= b) continue;
      lineMarked = true;
      lineError |= !m.warning;
      const Rgba col = m.warning ? c.warning : c.error;
      const uint32_t ua = std::clamp(mb, b, e) - b, uz = std::clamp(me, b, e) - b;
      float xa = static_cast<float>(x0 + run.caretX(ua)), xz = static_cast<float>(x0 + run.caretX(uz));
      if (xz < xa + static_cast<float>(charW_)) xz = xa + static_cast<float>(charW_);
      const float uy = fy1 - 2.0f * px1;
      const float step = 2.0f * px1;
      int k = 0;
      for (float x = xa; x < xz; x += step, ++k)
        ctx.dl.addRectFilled({x, uy + ((k & 1) ? px1 : 0.0f), std::min(x + step, xz), uy + ((k & 1) ? 2.0f * px1 : px1)}, col);
      if (mb >= b && mb <= e && !m.message.empty()) {
        // After the line's text; when that is out of view, right-aligned over the line on the gutter colour.
        const FontId small = ctx.res.uiFonts->small;
        const int32_t msgW = ts.measure(small, m.message);
        float mx = static_cast<float>(x0 + run.advance + 3 * charW_);
        int32_t maxW = r.right() - static_cast<int32_t>(mx) - pad_;
        if (maxW < std::min(msgW, 24 * charW_)) {
          maxW = std::min(msgW, (r.right() - codeX) * 2 / 3);
          mx = static_cast<float>(r.right() - pad_ - maxW);
          ctx.dl.addRectFilled({mx - static_cast<float>(pad_), fy, static_cast<float>(r.right()), fy1}, c.gutter);
        }
        if (maxW > 4 * charW_)
          (void)ts.drawTruncated(ctx.dl, small, {mx, static_cast<float>(base)}, m.message, maxW,
                                 m.warning ? c.warning : c.errorText);
      }
    }
    ctx.dl.popClip();
    // Gutter: line number, a dot for a marked line.
    char num[12];
    const int len = std::snprintf(num, sizeof num, "%u", line + 1);
    const int32_t nw = ts.measure(font_, std::string_view(num, static_cast<size_t>(len)));
    (void)ts.draw(ctx.dl, font_, {static_cast<float>(r.x + gutterW_ - nw - ctx.scale.px(8)), static_cast<float>(base)},
                  std::string_view(num, static_cast<size_t>(len)), line == caretLine ? c.lineNumberCurrent : c.lineNumber);
    if (lineMarked) {
      const float d = static_cast<float>(ctx.scale.px(4));
      const float cx = static_cast<float>(r.x + ctx.scale.px(3)), cy = fy + static_cast<float>(lineH_) * 0.5f - d * 0.5f;
      ctx.dl.addRectFilled({cx, cy, cx + d, cy + d}, lineError ? c.error : c.warning);
    }
  }
  // Caret (no blink: a focused editor is idle between keys).
  if (focused() && caretLine >= first && caretLine < last) {
    const int32_t y = r.y + static_cast<int32_t>(caretLine) * lineH_ - scrollY_;
    const float x = static_cast<float>(x0 + xInLine(caretLine, caret_));
    if (x >= static_cast<float>(codeX))
      ctx.dl.addRectFilled({x, static_cast<float>(y + 1), x + px1, static_cast<float>(y + lineH_ - 1)}, c.caret);
  }
  // A thin scroll thumb when the text is taller than the view.
  const int32_t contentH = static_cast<int32_t>(lines) * lineH_;
  if (contentH > r.h && r.h > 0) {
    const float th2 = std::max(static_cast<float>(ctx.scale.px(16)), static_cast<float>(r.h) * static_cast<float>(r.h) / static_cast<float>(contentH));
    const float ty = static_cast<float>(r.y) + (static_cast<float>(r.h) - th2) * static_cast<float>(scrollY_) / static_cast<float>(contentH - r.h);
    const float w = static_cast<float>(ctx.scale.px(3));
    ctx.dl.addRectFilled({static_cast<float>(r.right()) - w - 1.0f, ty, static_cast<float>(r.right()) - 1.0f, ty + th2}, c.lineNumber);
  }
  ctx.dl.popClip();
}

// ---- input -------------------------------------------------------------------------------------------------------------

bool CodeEditor::onPointer(const PointerEvent& e) {
  switch (e.type) {
    case PointerType::Wheel: {
      if (!metrics()) return false;
      const int32_t contentH = static_cast<int32_t>(lineCount()) * lineH_;
      if (e.mods & mod::kShift) scrollX_ = std::max(0, scrollX_ - static_cast<int32_t>(e.wheel.y * 6.0f * static_cast<float>(charW_)));
      else {
        scrollY_ -= static_cast<int32_t>(e.wheel.y * 3.0f * static_cast<float>(lineH_));
        scrollX_ = std::max(0, scrollX_ + static_cast<int32_t>(e.wheel.x * 6.0f * static_cast<float>(charW_)));
      }
      scrollY_ = std::clamp(scrollY_, 0, std::max(0, contentH - lineH_));
      markDirty();
      return true;
    }
    case PointerType::Down: {
      if (e.button != PointerButton::Left) return false;
      if (tree()) tree()->setFocus(this);
      const uint32_t b = byteAt(e.pos);
      goalX_ = -1;
      if (e.clickCount >= 2) {
        uint32_t a = b, z = b;
        while (a > 0 && isWordByte(static_cast<unsigned char>(text_[a - 1]))) --a;
        while (z < text_.size() && isWordByte(static_cast<unsigned char>(text_[z]))) ++z;
        select(a, z);
      } else {
        setCaret(b, (e.mods & mod::kShift) != 0);
      }
      selecting_ = true;
      return true;
    }
    case PointerType::Move:
      if (!selecting_) return false;
      setCaret(byteAt(e.pos), true);
      return true;
    case PointerType::Up:
      if (!selecting_) return false;
      selecting_ = false;
      return true;
    default: return false;
  }
}

bool CodeEditor::onKey(const KeyEvent& e) {
  if (!e.down) return false;
  const Mods m = e.mods & mod::kChordMask;
  const bool ctrl = (m & mod::kCtrl) != 0, shift = (m & mod::kShift) != 0;
  if (m & (mod::kAlt | mod::kSuper)) return false;
  const uint32_t n = static_cast<uint32_t>(text_.size());
  auto eraseOr = [&](uint32_t from, uint32_t to) {
    if (hasSelection()) userEdit(selBegin(), selEnd() - selBegin(), {}, false);
    else if (to > from) userEdit(from, to - from, {}, true);
  };
  switch (e.key) {
    case key::kLeft:
      goalX_ = -1;
      if (hasSelection() && !shift && !ctrl) setCaret(selBegin());
      else setCaret(ctrl ? wordLeft(caret_) : prevBoundary(caret_), shift);
      return true;
    case key::kRight:
      goalX_ = -1;
      if (hasSelection() && !shift && !ctrl) setCaret(selEnd());
      else setCaret(ctrl ? wordRight(caret_) : nextBoundary(caret_), shift);
      return true;
    case key::kUp: moveVertical(-1, shift); return true;
    case key::kDown: moveVertical(1, shift); return true;
    case key::kPageUp:
    case key::kPageDown: {
      const int32_t page = std::max(1, rect().h / std::max(1, lineH_) - 1);
      moveVertical(e.key == key::kPageUp ? -page : page, shift);
      return true;
    }
    case key::kHome: {
      goalX_ = -1;
      if (ctrl) {
        setCaret(0, shift);
        return true;
      }
      const uint32_t line = lineOf(caret_), b = lineStart(line), en = lineEnd(line);
      uint32_t first = b;
      while (first < en && isSpace(static_cast<unsigned char>(text_[first]))) ++first;
      setCaret(caret_ == first ? b : first, shift);
      return true;
    }
    case key::kEnd:
      goalX_ = -1;
      setCaret(ctrl ? n : lineEnd(lineOf(caret_)), shift);
      return true;
    case key::kBackspace:
      if (readOnly_) return true;
      eraseOr(ctrl ? wordLeft(caret_) : prevBoundary(caret_), caret_);
      return true;
    case key::kDelete:
      if (readOnly_) return true;
      eraseOr(caret_, ctrl ? wordRight(caret_) : nextBoundary(caret_));
      return true;
    case key::kReturn: {
      if (ctrl) return false;
      if (readOnly_) return true;
      const uint32_t line = lineOf(selBegin()), b = lineStart(line);
      uint32_t indentEnd = b;
      while (indentEnd < selBegin() && isSpace(static_cast<unsigned char>(text_[indentEnd]))) ++indentEnd;
      std::string ins = text_.find("\r\n") != std::string::npos ? "\r\n" : "\n";
      ins += text_.substr(b, indentEnd - b);
      uint32_t before = selBegin();
      while (before > b && isSpace(static_cast<unsigned char>(text_[before - 1]))) --before;
      if (before > b && text_[before - 1] == '{') ins += "  ";
      userEdit(selBegin(), selEnd() - selBegin(), ins, false);
      return true;
    }
    case key::kTab: {
      if (ctrl || readOnly_) return false;
      if (shift) {
        const uint32_t b = lineStart(lineOf(caret_));
        uint32_t k = 0;
        while (k < 2 && b + k < n && text_[b + k] == ' ') ++k;
        if (k) userEdit(b, k, {}, false);
        return true;
      }
      userEdit(selBegin(), selEnd() - selBegin(), "  ", false);
      return true;
    }
    default: break;
  }
  if (!ctrl || shift || !tree()) return false;
  switch (e.key) {
    case 'a':
      select(0, n);
      return true;
    case 'c':
      if (hasSelection()) tree()->clipboard().setText(std::string_view(text_).substr(selBegin(), selEnd() - selBegin()));
      return true;
    case 'x':
      if (hasSelection() && !readOnly_) {
        tree()->clipboard().setText(std::string_view(text_).substr(selBegin(), selEnd() - selBegin()));
        userEdit(selBegin(), selEnd() - selBegin(), {}, false);
      }
      return true;
    case 'v':
      if (!readOnly_) {
        const std::string clip(tree()->clipboard().text());
        userEdit(selBegin(), selEnd() - selBegin(), clip, false);
      }
      return true;
    default: return false;
  }
}

bool CodeEditor::onText(const TextEvent& e) {
  if (readOnly_) return true;
  goalX_ = -1;
  userEdit(selBegin(), selEnd() - selBegin(), e.text(), !hasSelection() && e.text().size() <= 4);
  return true;
}

void CodeEditor::onFocusChanged(bool) {
  selecting_ = false;
  markDirty();
}

InfoText CodeEditor::info() const {
  return {"Text", "The patch as text. Edits here and in the circuit are the same edit; Ctrl+Z undoes either."};
}

}  // namespace pq::ui
