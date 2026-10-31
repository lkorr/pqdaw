// TextBuffer (single-line UTF-8 editing state) and the TextEdit control.

#include <algorithm>
#include <cmath>

#include "controls_paint.h"
#include "pq/ui/controls.h"
#include "pq/ui/theme.h"

namespace pq::ui {

namespace {

constexpr double kBlinkSeconds = 0.53;

bool isCont(char c) noexcept { return (static_cast<uint8_t>(c) & 0xC0) == 0x80; }

enum class CharClass : uint8_t { Space, Word, Punct };
CharClass classOf(char c) noexcept {
  const auto u = static_cast<uint8_t>(c);
  if (c == ' ' || c == '\t') return CharClass::Space;
  if (u >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') return CharClass::Word;
  return CharClass::Punct;
}

// Keeps whole code points of `s` that fit in `room` bytes, dropping control characters (single-line field).
std::string sanitize(std::string_view s, size_t room) {
  std::string out;
  out.reserve(std::min(s.size(), room));
  for (size_t i = 0; i < s.size();) {
    size_t n = 1;
    while (i + n < s.size() && isCont(s[i + n])) ++n;
    const auto c = static_cast<uint8_t>(s[i]);
    if (c == '\n' || c == '\r') break;  // first line only
    if (c >= 0x20 && c != 0x7F) {
      if (out.size() + n > room) break;
      out.append(s.substr(i, n));
    }
    i += n;
  }
  return out;
}

}  // namespace

// --- TextBuffer -----------------------------------------------------------------------------------------

void TextBuffer::setText(std::string text) {
  text_ = sanitize(text, maxBytes);
  caret_ = anchor_ = static_cast<uint32_t>(text_.size());
}

uint32_t TextBuffer::prevBoundary(uint32_t i) const noexcept {
  if (i == 0) return 0;
  --i;
  while (i > 0 && isCont(text_[i])) --i;
  return i;
}

uint32_t TextBuffer::nextBoundary(uint32_t i) const noexcept {
  const auto n = static_cast<uint32_t>(text_.size());
  if (i >= n) return n;
  ++i;
  while (i < n && isCont(text_[i])) ++i;
  return i;
}

uint32_t TextBuffer::wordLeft(uint32_t i) const noexcept {
  while (i > 0 && classOf(text_[prevBoundary(i)]) == CharClass::Space) i = prevBoundary(i);
  if (i == 0) return 0;
  const CharClass c = classOf(text_[prevBoundary(i)]);
  while (i > 0 && classOf(text_[prevBoundary(i)]) == c) i = prevBoundary(i);
  return i;
}

uint32_t TextBuffer::wordRight(uint32_t i) const noexcept {
  const auto n = static_cast<uint32_t>(text_.size());
  if (i < n) {
    const CharClass c = classOf(text_[i]);
    if (c != CharClass::Space)
      while (i < n && classOf(text_[i]) == c) i = nextBoundary(i);
  }
  while (i < n && classOf(text_[i]) == CharClass::Space) i = nextBoundary(i);  // to the start of the next word
  return i;
}

void TextBuffer::setCaret(uint32_t byte, bool extend) {
  byte = std::min(byte, static_cast<uint32_t>(text_.size()));
  while (byte > 0 && byte < text_.size() && isCont(text_[byte])) --byte;
  caret_ = byte;
  if (!extend) anchor_ = byte;
}

void TextBuffer::selectAll() {
  anchor_ = 0;
  caret_ = static_cast<uint32_t>(text_.size());
}

void TextBuffer::selectWordAt(uint32_t byte) {
  const auto n = static_cast<uint32_t>(text_.size());
  if (n == 0) return;
  byte = std::min(byte, n);
  if (byte == n) byte = prevBoundary(n);
  while (byte > 0 && isCont(text_[byte])) --byte;
  const CharClass c = classOf(text_[byte]);
  uint32_t a = byte, b = nextBoundary(byte);
  while (a > 0 && classOf(text_[prevBoundary(a)]) == c) a = prevBoundary(a);
  while (b < n && classOf(text_[b]) == c) b = nextBoundary(b);
  anchor_ = a;
  caret_ = b;
}

void TextBuffer::eraseSelection() {
  if (!hasSelection()) return;
  const uint32_t a = selBegin(), b = selEnd();
  text_.erase(a, b - a);
  caret_ = anchor_ = a;
}

void TextBuffer::insert(std::string_view utf8) {
  eraseSelection();
  const size_t room = maxBytes > text_.size() ? maxBytes - text_.size() : 0;
  const std::string s = sanitize(utf8, room);
  text_.insert(caret_, s);
  caret_ += static_cast<uint32_t>(s.size());
  anchor_ = caret_;
}

void TextBuffer::moveLeft(bool word, bool extend) {
  if (hasSelection() && !extend && !word) {
    caret_ = anchor_ = selBegin();
    return;
  }
  setCaret(word ? wordLeft(caret_) : prevBoundary(caret_), extend);
}

void TextBuffer::moveRight(bool word, bool extend) {
  if (hasSelection() && !extend && !word) {
    caret_ = anchor_ = selEnd();
    return;
  }
  setCaret(word ? wordRight(caret_) : nextBoundary(caret_), extend);
}

void TextBuffer::home(bool extend) { setCaret(0, extend); }
void TextBuffer::end(bool extend) { setCaret(static_cast<uint32_t>(text_.size()), extend); }

void TextBuffer::backspace(bool word) {
  if (hasSelection()) {
    eraseSelection();
    return;
  }
  const uint32_t a = word ? wordLeft(caret_) : prevBoundary(caret_);
  text_.erase(a, caret_ - a);
  caret_ = anchor_ = a;
}

void TextBuffer::del(bool word) {
  if (hasSelection()) {
    eraseSelection();
    return;
  }
  const uint32_t b = word ? wordRight(caret_) : nextBoundary(caret_);
  text_.erase(caret_, b - caret_);
  anchor_ = caret_;
}

bool TextBuffer::handleKey(const KeyEvent& e, Clipboard& clipboard, bool& changed) {
  changed = false;
  if (!e.down) return false;
  const Mods m = e.mods & mod::kChordMask;
  const bool ctrl = (m & mod::kCtrl) != 0, shift = (m & mod::kShift) != 0;
  if (m & (mod::kAlt | mod::kSuper)) return false;
  const std::string before = (e.key == key::kBackspace || e.key == key::kDelete || (ctrl && (e.key == 'x' || e.key == 'v'))) ? text_ : std::string();
  switch (e.key) {
    case key::kLeft: moveLeft(ctrl, shift); return true;
    case key::kRight: moveRight(ctrl, shift); return true;
    case key::kHome: home(shift); return true;
    case key::kEnd: end(shift); return true;
    case key::kBackspace:
      if (shift) return false;
      backspace(ctrl);
      changed = text_ != before;
      return true;
    case key::kDelete:
      if (shift) return false;
      del(ctrl);
      changed = text_ != before;
      return true;
    default: break;
  }
  if (!ctrl || shift) return false;
  switch (e.key) {
    case 'a': selectAll(); return true;
    case 'c':
      if (hasSelection()) clipboard.setText(selected());
      return true;
    case 'x':
      if (hasSelection()) {
        clipboard.setText(selected());
        eraseSelection();
      }
      changed = text_ != before;
      return true;
    case 'v':
      insert(clipboard.text());
      changed = text_ != before;
      return true;
    default: return false;
  }
}

// --- TextEdit -------------------------------------------------------------------------------------------

TextEdit::TextEdit(std::string name, std::string text) : Control(std::move(name)) {
  setFocusable(true);
  setAcceptsTextInput(true);
  focusRingOnPointerFocus_ = true;
  setCursor(Cursor::Text);
  buf_.setText(std::move(text));
  committed_ = buf_.text();
}

void TextEdit::setText(std::string text) {
  buf_.setText(std::move(text));
  committed_ = buf_.text();
  scrollX_ = 0;
  markDirty();
}

void TextEdit::restartBlink() {
  caretOn_ = true;
  if (blinkTimer_) cancelTimer(blinkTimer_);
  blinkTimer_ = focused() ? requestTimer(kBlinkSeconds) : 0;
}

void TextEdit::onTimer(uint32_t id) {
  if (id != blinkTimer_) return;
  caretOn_ = !caretOn_;
  markDirty();
  blinkTimer_ = focused() ? requestTimer(kBlinkSeconds) : 0;
}

void TextEdit::edited(bool changed) {
  restartBlink();
  markDirty();
  if (changed && onChange) onChange(buf_.text());
}

uint32_t TextEdit::byteAtX(int32_t deviceX) {
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  if (!res || !res->text || !res->uiFonts || !res->theme) return static_cast<uint32_t>(buf_.text().size());
  const TextRun run = res->text->shape(res->uiFonts->body, buf_.text());
  const int32_t pad = tree()->uiScale().px(res->theme->spacing.sm);
  return run.hitTest(deviceX - (rect().x + pad) + scrollX_);
}

bool TextEdit::onPointer(const PointerEvent& e) {
  switch (e.type) {
    case PointerType::Down: {
      if (e.button != PointerButton::Left) return false;
      const uint32_t b = byteAtX(static_cast<int32_t>(std::lround(e.pos.x)));
      if (e.clickCount >= 2) buf_.selectWordAt(b);
      else buf_.setCaret(b, (e.mods & mod::kShift) != 0);
      selecting_ = true;
      edited(false);
      return true;
    }
    case PointerType::Move:
      if (!selecting_) return false;
      buf_.setCaret(byteAtX(static_cast<int32_t>(std::lround(e.pos.x))), true);
      edited(false);
      return true;
    case PointerType::Up:
      if (!selecting_) return false;
      selecting_ = false;
      return true;
    default: return false;
  }
}

bool TextEdit::onKey(const KeyEvent& e) {
  if (!e.down) return false;
  const Mods m = e.mods & mod::kChordMask;
  if (e.key == key::kReturn && m == mod::kNone) {
    committed_ = buf_.text();
    if (onCommit) onCommit(committed_);
    if (onEnter) onEnter();
    return true;
  }
  if (e.key == key::kEscape && m == mod::kNone && !revertOnEscape) {
    if (onCancel) onCancel();
    return true;
  }
  if (e.key == key::kEscape && m == mod::kNone) {
    const bool changed = buf_.text() != committed_;
    buf_.setText(committed_);
    buf_.selectAll();
    edited(changed);
    if (onCancel) onCancel();
    return true;
  }
  bool changed = false;
  if (!tree() || !buf_.handleKey(e, tree()->clipboard(), changed)) return false;
  edited(changed);
  return true;
}

bool TextEdit::onText(const TextEvent& e) {
  const std::string before = buf_.text();
  buf_.insert(e.text());
  edited(buf_.text() != before);
  return true;
}

void TextEdit::onFocusChanged(bool focused) {
  if (focused) {
    if (tree() && tree()->focusVisible()) buf_.selectAll();  // tabbing into a field selects it
    restartBlink();
  } else {
    if (blinkTimer_) cancelTimer(blinkTimer_);
    blinkTimer_ = 0;
    selecting_ = false;
    buf_.setCaret(buf_.caret(), false);
    if (buf_.text() != committed_) {  // leaving the field commits (Live's rename)
      committed_ = buf_.text();
      if (onCommit) onCommit(committed_);
    }
  }
  markDirty();
}

void TextEdit::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState v = visualState();
  const IRect r = rect();
  const Rgba fill = v.disabled ? th->control.fillDisabled : v.hover && !focused() ? th->field.fillHover : th->field.fill;
  detail::box(ctx.dl, r, ctx.scale.exact(th->radius.control), fill, v.focus ? th->field.borderFocus : th->field.border);
  const ThemeFonts* f = ctx.res.uiFonts;
  if (!f || !ctx.res.text) return;
  TextSystem& ts = *ctx.res.text;
  const int32_t pad = ctx.scale.px(th->spacing.sm);
  const int32_t viewW = std::max(1, r.w - 2 * pad);
  const bool editing = v.focus || focused();
  if (buf_.text().empty() && !editing) {
    if (!placeholder_.empty()) detail::drawText(ctx, f->body, r, placeholder_, th->text.disabled, TextAlign::Left, pad, pad);
    if (!v.focus) return;
  }
  const TextRun run = ts.shape(f->body, buf_.text());
  // Keep the caret in view (the scroll follows the caret, never the other way round).
  const int32_t cx = run.caretX(buf_.caret());
  if (cx - scrollX_ > viewW - 1) scrollX_ = cx - viewW + 1;
  if (cx - scrollX_ < 0) scrollX_ = cx;
  scrollX_ = std::clamp(scrollX_, 0, std::max(0, run.advance - viewW + 1));
  const int32_t x0 = r.x + pad - scrollX_;
  ctx.dl.pushClip(detail::inset(r, 1).toRect());
  const float y0 = static_cast<float>(r.y + ctx.scale.px(3)), y1 = static_cast<float>(r.bottom() - ctx.scale.px(3));
  if (editing && buf_.hasSelection()) {
    const float a = static_cast<float>(x0 + run.caretX(buf_.selBegin())), b = static_cast<float>(x0 + run.caretX(buf_.selEnd()));
    ctx.dl.addRectFilled({a, y0, b, y1}, th->text.selection);
  }
  const int32_t base = detail::capCentredBaseline(r.y, r.h, ts.metrics(f->body).pixelSize);
  TextSystem::drawRun(ctx.dl, run, {static_cast<float>(x0), static_cast<float>(base)}, v.disabled ? th->text.disabled : th->text.primary);
  if (editing && caretOn_ && !v.disabled) {
    const float x = static_cast<float>(x0 + cx);
    ctx.dl.addRectFilled({x, y0, x + std::max(1.0f, std::round(ctx.scale.exact(1.0f))), y1}, th->text.caret);
  }
  ctx.dl.popClip();
}

}  // namespace pq::ui
