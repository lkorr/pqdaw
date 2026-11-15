// KB.help: the piano roll's help panel (roll_help.h). Layout and painting of the table in roll_help_content.cpp.

#include "roll_help.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "../common/paint.h"
#include "pq/ui/controls.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/views/view_context.h"

namespace pq::views::roll {

using namespace pq::ui;

// --- gestures -------------------------------------------------------------------------------------------

std::string helpActionKeys(std::string_view action, const Keymap& keymap) {
  const ScopeId roll = keymap.findScope("pianoroll");
  const Keymap::Binding* found = nullptr;
  for (const Keymap::Binding& b : keymap.bindings()) {
    if (b.action != action) continue;
    if (!found || (b.scope == roll && found->scope != roll)) found = &b;
    if (found->scope == roll) break;
  }
  return found ? formatChord(found->chord) : std::string();
}

namespace {

void pushWords(std::vector<HelpToken>& out, std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') ++i;
    size_t j = i;
    while (j < text.size() && text[j] != ' ') ++j;
    if (j > i) out.push_back({HelpToken::Kind::Word, false, std::string(text.substr(i, j - i))});
    i = j;
  }
}

// "Ctrl+Shift+J" -> [Ctrl] + [Shift] + [J], one unbreakable run. A chord whose key is "+" ends in "++".
void pushChord(std::vector<HelpToken>& out, std::string_view chord) {
  bool first = true;
  size_t i = 0;
  while (i < chord.size()) {
    size_t j = chord.find('+', i);
    if (j == i) j = i + 1;  // the key "+" itself
    if (j == std::string_view::npos) j = chord.size();
    if (!first) out.push_back({HelpToken::Kind::Word, true, "+"});
    out.push_back({HelpToken::Kind::Key, !first, std::string(chord.substr(i, j - i))});
    first = false;
    i = j < chord.size() ? j + 1 : j;
  }
}

}  // namespace

std::vector<HelpToken> parseHelpGesture(std::string_view markup, const Keymap& keymap) {
  std::vector<HelpToken> out;
  size_t i = 0;
  while (i < markup.size()) {
    const char c = markup[i];
    if (c == '\n') {
      out.push_back({HelpToken::Kind::Break, false, {}});
      ++i;
    } else if (c == '{' || c == '[') {
      const size_t end = markup.find(c == '{' ? '}' : ']', i + 1);
      if (end == std::string_view::npos) {  // unbalanced: shown as written
        pushWords(out, markup.substr(i));
        break;
      }
      const std::string_view inner = markup.substr(i + 1, end - i - 1);
      if (c == '[') {
        out.push_back({HelpToken::Kind::Key, false, std::string(inner)});
      } else {
        const std::string keys = helpActionKeys(inner, keymap);
        if (keys.empty()) out.push_back({HelpToken::Kind::Unbound, false, "unbound"});
        else pushChord(out, keys);
      }
      i = end + 1;
    } else {
      const size_t end = markup.find_first_of("{[\n", i);
      pushWords(out, markup.substr(i, end == std::string_view::npos ? end : end - i));
      i = end == std::string_view::npos ? markup.size() : end;
    }
  }
  return out;
}

std::string helpGestureText(std::span<const HelpToken> tokens) {
  std::string s;
  bool lineStart = true;
  for (const HelpToken& t : tokens) {
    if (t.kind == HelpToken::Kind::Break) {
      s += '\n';
      lineStart = true;
      continue;
    }
    if (!lineStart && !t.glue) s += ' ';
    s += t.text;
    lineStart = false;
  }
  return s;
}

// --- the laid-out text ----------------------------------------------------------------------------------

namespace {

// Logical px.
constexpr float kPadX = 14.0f, kPadTop = 8.0f, kPadBottom = 14.0f;
constexpr float kGestureWidth = 200.0f, kColumnGap = 18.0f;
constexpr float kSectionGap = 22.0f, kEntryGap = 11.0f, kCapPadX = 6.0f, kCapGap = 4.0f, kLineGap = 3.0f;
constexpr float kRailWidth = 238.0f, kHeaderHeight = 24.0f, kRailRow = 24.0f;

enum class Tone : uint8_t { Primary, Secondary, Disabled, Accent };

Rgba toneOf(const Theme& th, Tone t) noexcept {
  switch (t) {
    case Tone::Primary: return th.text.primary;
    case Tone::Secondary: return th.text.secondary;
    case Tone::Disabled: return th.text.disabled;
    case Tone::Accent: return th.accent.primary;
  }
  return th.text.primary;
}

// Greedy word wrap of `text` at `width` px: `emit(line)` per line.
template <typename Measure, typename Emit>
void wrapText(std::string_view text, int32_t width, Measure&& measure, Emit&& emit) {
  size_t start = 0;
  while (start < text.size()) {
    size_t end = start, fit = start;
    while (end < text.size()) {
      size_t next = end;
      while (next < text.size() && text[next] == ' ') ++next;
      while (next < text.size() && text[next] != ' ') ++next;
      if (measure(text.substr(start, next - start)) > width && fit > start) break;
      fit = end = next;
    }
    if (fit == start) fit = text.size();  // one word wider than the column: truncated when drawn
    emit(text.substr(start, fit - start));
    start = fit;
    while (start < text.size() && text[start] == ' ') ++start;
  }
}

}  // namespace

// The scrolled content: headings, intros and the two columns, positioned once per width (device px, relative to the
// widget's top-left) and painted from those lists.
class HelpContent : public Widget {
public:
  struct Entry {
    std::vector<HelpToken> gesture;
    std::string_view title, text;
  };
  struct Section {
    std::string_view title, intro;
    std::vector<Entry> entries;
  };

  explicit HelpContent(const Keymap& keymap) : Widget("content") {
    for (const HelpSection& s : rollHelpSections()) {
      Section sec{s.title, s.intro, {}};
      sec.entries.reserve(s.entries.size());
      for (const HelpEntry& e : s.entries) sec.entries.push_back({parseHelpGesture(e.gesture, keymap), e.title, e.text});
      sections_.push_back(std::move(sec));
    }
  }

  const std::vector<Section>& sections() const noexcept { return sections_; }
  // The y of a section's heading inside the content (device px; -1 before the first layout).
  int32_t sectionY(size_t i) const noexcept { return i < sectionY_.size() ? sectionY_[i] : -1; }
  int32_t heightFor(int32_t width) const {
    layout(width);
    return height_;
  }

  ISize contentExtent(ISize viewport) const override { return {viewport.w, heightFor(viewport.w)}; }

  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    const ThemeFonts* f = ctx.res.uiFonts;
    if (!th || !f) return;
    const IRect r = rect();
    layout(r.w);
    const int32_t top = ctx.clip.y - r.y, bottom = ctx.clip.bottom() - r.y;
    // Blocks are in y order: the first one that reaches the clip, then on until one starts below it.
    auto it = std::lower_bound(blocks_.begin(), blocks_.end(), top, [](const Block& b, int32_t y) { return b.y1 <= y; });
    const float rad = ctx.scale.exact(3.0f);
    for (; it != blocks_.end() && it->y0 < bottom; ++it) {
      for (uint32_t i = it->rule0; i < it->rule1; ++i) {
        const Rule& q = rules_[i];
        ctx.dl.addHLine(static_cast<float>(r.x + q.x0), static_cast<float>(r.x + q.x1), static_cast<float>(r.y + q.y), th->surface.divider);
      }
      for (uint32_t i = it->cap0; i < it->cap1; ++i) {
        const IRect& c = caps_[i];
        paint::box(ctx.dl, {r.x + c.x, r.y + c.y, c.w, c.h}, rad, th->control.fill, th->control.border);
      }
      for (uint32_t i = it->text0; i < it->text1; ++i) {
        const Text& t = texts_[i];
        paint::text(ctx, fontOf(*f, t.font), {r.x + t.box.x, r.y + t.box.y, t.box.w, t.box.h}, t.s, toneOf(*th, t.tone),
                    t.centre ? TextAlign::Center : TextAlign::Left);
      }
    }
  }

private:
  struct Text {
    IRect box;
    std::string_view s;
    FontRoleId font;
    Tone tone;
    bool centre;
  };
  struct Rule {
    int32_t x0, x1, y;
  };
  struct Block {  // a heading, an intro or one entry: its vertical span and its items
    int32_t y0, y1;
    uint32_t text0, text1, cap0, cap1, rule0, rule1;
  };

  // Positions everything for `width` device px. Cached: the same width, scale and fonts lay out nothing.
  void layout(int32_t width) const {
    const UiTree* t = tree();
    const PaintResources* res = t ? t->resources() : nullptr;
    const ThemeFonts* f = res ? res->uiFonts : nullptr;
    TextSystem* ts = res ? res->text : nullptr;
    const float sc = t ? t->scale() : 1.0f;
    const FontId small = f ? f->small : kNoFont;
    if (width == width_ && sc == scale_ && small == font_ && ts == text_) return;
    width_ = width;
    scale_ = sc;
    font_ = small;
    text_ = ts;
    texts_.clear();
    caps_.clear();
    rules_.clear();
    blocks_.clear();
    sectionY_.assign(sections_.size(), 0);
    const UiScale s(sc);
    // Without a text system (draw-list-only trees) widths are estimated; nothing is drawn there anyway.
    auto measure = [&](FontRoleId role, std::string_view text) -> int32_t {
      if (ts && f) return ts->measure(fontOf(*f, role), text);
      return static_cast<int32_t>(std::lround(6.5f * sc * static_cast<float>(text.size())));
    };
    auto lineHeight = [&](FontRoleId role) -> int32_t {
      if (ts && f) return ts->metrics(fontOf(*f, role)).lineHeight;
      return s.px(16);
    };
    const int32_t padX = s.px(kPadX), gestureW = s.px(kGestureWidth);
    const int32_t descX = padX + gestureW + s.px(kColumnGap), descW = std::max(s.px(80), width - descX - padX);
    const int32_t lhSmall = lineHeight(FontRoleId::Small), lhBody = lineHeight(FontRoleId::Body), lhTitle = lineHeight(FontRoleId::Title);
    const int32_t capH = lhSmall + s.px(4), capPad = s.px(kCapPadX), capGap = s.px(kCapGap), rowH = capH + s.px(kLineGap);
    const int32_t space = std::max(1, measure(FontRoleId::Small, "n n") - measure(FontRoleId::Small, "nn"));

    auto begin = [&](int32_t y) {
      blocks_.push_back({y, y, static_cast<uint32_t>(texts_.size()), 0, static_cast<uint32_t>(caps_.size()), 0, static_cast<uint32_t>(rules_.size()), 0});
    };
    auto end = [&](int32_t y) {
      Block& b = blocks_.back();
      b.y1 = y;
      b.text1 = static_cast<uint32_t>(texts_.size());
      b.cap1 = static_cast<uint32_t>(caps_.size());
      b.rule1 = static_cast<uint32_t>(rules_.size());
    };

    int32_t y = s.px(kPadTop);
    for (size_t si = 0; si < sections_.size(); ++si) {
      const Section& sec = sections_[si];
      if (si > 0) y += s.px(kSectionGap);
      sectionY_[si] = y;
      begin(y);
      texts_.push_back({{padX, y, width - 2 * padX, lhTitle}, sec.title, FontRoleId::Title, Tone::Primary, false});
      y += lhTitle + s.px(5);
      rules_.push_back({padX, width - padX, y});
      y += s.px(9);
      end(y);
      if (!sec.intro.empty()) {
        begin(y);
        wrapText(sec.intro, width - 2 * padX, [&](std::string_view v) { return measure(FontRoleId::Body, v); }, [&](std::string_view line) {
          texts_.push_back({{padX, y, width - 2 * padX, lhBody}, line, FontRoleId::Body, Tone::Secondary, false});
          y += lhBody;
        });
        y += s.px(kEntryGap);
        end(y);
      }
      for (const Entry& e : sec.entries) {
        begin(y);
        // The gesture column: key caps and words flow left to right; a run of glued tokens (a chord) never splits.
        int32_t gx = 0, gy = y;
        for (size_t i = 0; i < e.gesture.size();) {
          if (e.gesture[i].kind == HelpToken::Kind::Break) {
            gx = 0;
            gy += rowH;
            ++i;
            continue;
          }
          size_t j = i + 1;
          while (j < e.gesture.size() && e.gesture[j].glue) ++j;
          auto widthOf = [&](const HelpToken& tok) {
            return measure(FontRoleId::Small, tok.text) + (tok.kind == HelpToken::Kind::Key ? 2 * capPad : 0);
          };
          int32_t runW = 0;
          for (size_t k = i; k < j; ++k) runW += widthOf(e.gesture[k]) + (k > i ? capGap : 0);
          if (gx > 0 && gx + space + runW > gestureW) {
            gx = 0;
            gy += rowH;
          } else if (gx > 0) {
            gx += space;
          }
          for (size_t k = i; k < j; ++k) {
            const HelpToken& tok = e.gesture[k];
            const int32_t w = widthOf(tok);
            if (k > i) gx += capGap;
            const IRect box{padX + gx, gy, w, capH};
            if (tok.kind == HelpToken::Kind::Key) {
              caps_.push_back(box);
              texts_.push_back({box, tok.text, FontRoleId::Small, Tone::Primary, true});
            } else {
              texts_.push_back({box, tok.text, FontRoleId::Small, tok.kind == HelpToken::Kind::Unbound ? Tone::Disabled : Tone::Secondary, false});
            }
            gx += w;
          }
          i = j;
        }
        const int32_t gestureBottom = e.gesture.empty() ? y : gy + capH;
        // The description column: the title on the first gesture line, the text wrapped under it.
        int32_t dy = y;
        texts_.push_back({{descX, dy, descW, capH}, e.title, FontRoleId::BodyStrong, Tone::Primary, false});
        dy += capH + s.px(1);
        wrapText(e.text, descW, [&](std::string_view v) { return measure(FontRoleId::Body, v); }, [&](std::string_view line) {
          texts_.push_back({{descX, dy, descW, lhBody}, line, FontRoleId::Body, Tone::Secondary, false});
          dy += lhBody;
        });
        y = std::max(gestureBottom, dy) + s.px(kEntryGap);
        end(y);
      }
    }
    height_ = y + s.px(kPadBottom);
  }

  std::vector<Section> sections_;
  mutable std::vector<Text> texts_;
  mutable std::vector<IRect> caps_;
  mutable std::vector<Rule> rules_;
  mutable std::vector<Block> blocks_;
  mutable std::vector<int32_t> sectionY_;
  mutable int32_t width_ = -1, height_ = 0;
  mutable float scale_ = 0.0f;
  mutable FontId font_ = kNoFont;
  mutable TextSystem* text_ = nullptr;
};

// The section list on the left: a click scrolls to that section; the section at the top of the view is marked.
class HelpRail : public Control, private ScrollView::Dependent {
public:
  HelpRail(RollHelpPopover& owner, ScrollView& view, const HelpContent& content)
      : Control("sections"), owner_(owner), view_(&view), content_(content) {
    setFocusable(false);
    view.linkDependent(this);
  }
  ~HelpRail() override {
    if (view_) view_->unlinkDependent(this);
  }

  // The section whose heading is at (or above) the top of the view; the last one once scrolled to the end.
  size_t current() const noexcept {
    if (!view_) return 0;
    const size_t n = content_.sections().size();
    if (n == 0) return 0;
    const int32_t y = view_->scrollY();
    if (y > 0 && y >= view_->contentSize().h - view_->rect().h) return n - 1;
    size_t cur = 0;
    const int32_t slack = static_cast<int32_t>(std::lround(12.0f * (tree() ? tree()->scale() : 1.0f)));
    for (size_t i = 0; i < n; ++i)
      if (content_.sectionY(i) <= y + slack) cur = i;
    return cur;
  }
  IRect rowRect(size_t i) const noexcept {
    const IRect r = rect();
    const float sc = tree() ? tree()->scale() : 1.0f;
    const int32_t h = static_cast<int32_t>(std::lround(kRailRow * sc));
    return {r.x, r.y + static_cast<int32_t>(i) * h, r.w, h};
  }

  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    const ThemeFonts* f = ctx.res.uiFonts;
    if (!th || !f) return;
    const size_t cur = current();
    const int32_t pad = ctx.scale.px(8);
    for (size_t i = 0; i < content_.sections().size(); ++i) {
      const IRect row = rowRect(i);
      const bool on = i == cur;
      if (on) paint::box(ctx.dl, row, ctx.scale.exact(3.0f), th->selection.fill, 0);
      else if (static_cast<int32_t>(i) == hover_) paint::box(ctx.dl, row, ctx.scale.exact(3.0f), th->control.fillHover, 0);
      paint::text(ctx, on ? f->bodyStrong : f->body, row, content_.sections()[i].title, on ? th->selection.text : th->text.secondary,
                  TextAlign::Left, pad, pad);
    }
  }

  bool onPointer(const PointerEvent& e) override {
    if (e.type == PointerType::Move) {
      const int32_t h = rowAt(e.pos);
      if (h != hover_) {
        hover_ = h;
        markDirty();
      }
      return false;
    }
    if (e.type == PointerType::Down && e.button == PointerButton::Left) {
      const int32_t i = rowAt(e.pos);
      if (i >= 0) owner_.showSection(static_cast<size_t>(i));
      return true;
    }
    return false;
  }
  void onHoverChanged(bool hovered) override {
    if (!hovered && hover_ >= 0) {
      hover_ = -1;
      markDirty();
    }
  }
  Cursor cursorAt(Vec2 p) const override { return rowAt(p) >= 0 ? Cursor::Pointer : Cursor::Arrow; }

private:
  int32_t rowAt(Vec2 p) const noexcept {
    for (size_t i = 0; i < content_.sections().size(); ++i)
      if (rowRect(i).contains(p)) return static_cast<int32_t>(i);
    return -1;
  }
  void onScrollChanged() noexcept override { markDirty(); }
  void onScrollViewGone() noexcept override { view_ = nullptr; }

  RollHelpPopover& owner_;
  ScrollView* view_;
  const HelpContent& content_;
  int32_t hover_ = -1;
};

// --- the popover ----------------------------------------------------------------------------------------

RollHelpPopover::RollHelpPopover(const Keymap& keymap, const Widget* within, int32_t left)
    : Popover("roll-help"), within_(within), left_(left) {
  setLayout(LayoutKind::Custom);
  title_ = &add<Label>("title", "Piano Roll Help: pitch, bends and just intonation", FontRoleId::Title, TextTone::Primary);
  hint_ = &add<Label>("hint", "Esc or a click outside closes", FontRoleId::Small, TextTone::Secondary, TextAlign::Right);
  title_->setHittable(false);
  hint_->setHittable(false);
  scroll_ = &add<ScrollView>("scroll", false, true);
  content_ = &scroll_->add<HelpContent>(keymap);
  rail_ = &add<HelpRail>(*this, *scroll_, *content_);
  bar_ = &add<Scrollbar>("vscroll", *scroll_, Scrollbar::Axis::Vertical);
}

size_t RollHelpPopover::sectionCount() const noexcept { return content_->sections().size(); }
size_t RollHelpPopover::entryCount(size_t section) const noexcept {
  return section < content_->sections().size() ? content_->sections()[section].entries.size() : 0;
}
std::string RollHelpPopover::gestureText(size_t section, size_t entry) const {
  if (entry >= entryCount(section)) return {};
  return helpGestureText(content_->sections()[section].entries[entry].gesture);
}

void RollHelpPopover::showSection(size_t section) {
  const int32_t y = content_->sectionY(section);
  if (y < 0) return;
  const float sc = tree() ? tree()->scale() : 1.0f;
  scrollToY(std::max(0, y - static_cast<int32_t>(std::lround(kPadTop * sc))));
}
int32_t RollHelpPopover::scrollY() const noexcept { return scroll_->scrollY(); }
int32_t RollHelpPopover::maxScrollY() const noexcept { return std::max(0, scroll_->contentSize().h - scroll_->rect().h); }
void RollHelpPopover::scrollToY(int32_t y) { scroll_->scrollTo(0, y); }

namespace {
constexpr float kFrame = 10.0f, kGap = 8.0f, kMargin = 8.0f, kScrollbarFallback = 10.0f;

float scrollbarWidth(const UiTree* tree) {
  const PaintResources* res = tree ? tree->resources() : nullptr;
  return res && res->theme ? res->theme->size.scrollbar : kScrollbarFallback;
}
}  // namespace

// As wide as kWidth and as tall as the text needs, both capped by the room over the panels (`within`).
ISize RollHelpPopover::contentExtent(ISize viewport) const {
  const UiScale s(tree() ? tree()->scale() : 1.0f);
  const int32_t right = within_ ? std::min(viewport.w, within_->rect().right()) : viewport.w;
  const int32_t roomW = right - left_ - s.px(kMargin);
  const int32_t roomH = (within_ ? std::min(viewport.h, within_->rect().h) : viewport.h) - 2 * s.px(kMargin);
  const int32_t w = std::max(s.px(360), std::min(s.px(kWidth), roomW));
  const int32_t contentW = w - 2 * s.px(kFrame) - s.px(kRailWidth) - s.px(kGap) - s.px(scrollbarWidth(tree()));
  const int32_t chrome = 2 * s.px(kFrame) + s.px(kHeaderHeight) + s.px(kGap);
  const int32_t wanted = chrome + content_->heightFor(contentW);
  return {w, std::max(s.px(160), std::min({wanted, s.px(kMaxHeight), roomH}))};
}

void RollHelpPopover::arrange(LayoutPass& pass) {
  const IRect r = rect();
  const int32_t fr = pass.px(kFrame), gap = pass.px(kGap), head = pass.px(kHeaderHeight), rail = pass.px(kRailWidth);
  const int32_t sb = pass.px(scrollbarWidth(tree()));
  const int32_t innerW = std::max(0, r.w - 2 * fr);
  const int32_t hintW = std::min(innerW / 3, pass.px(220));
  pass.place(*title_, {r.x + fr, r.y + fr, innerW - hintW, head});
  pass.place(*hint_, {r.x + fr + innerW - hintW, r.y + fr, hintW, head});
  const int32_t by = r.y + fr + head + gap, bh = std::max(0, r.bottom() - fr - by);
  pass.place(*rail_, {r.x + fr, by, rail, bh});
  const int32_t sx = r.x + fr + rail + gap;
  const int32_t sw = std::max(0, r.right() - fr - sb - sx);
  pass.place(*scroll_, {sx, by, sw, bh});
  pass.place(*bar_, {sx + sw, by, sb, bh});
}

bool RollHelpPopover::onKey(const KeyEvent& e) {
  if (!e.down) return false;
  const UiScale s(tree() ? tree()->scale() : 1.0f);
  const int32_t page = std::max(s.px(40), scroll_->rect().h - s.px(40));
  int32_t y = scrollY();
  if (e.key == key::kDown) y += s.px(48);
  else if (e.key == key::kUp) y -= s.px(48);
  else if (e.key == key::kPageDown) y += page;
  else if (e.key == key::kPageUp) y -= page;
  else if (e.key == key::kHome) y = 0;
  else if (e.key == key::kEnd) y = maxScrollY();
  else return Popover::onKey(e);  // Esc closes
  scrollToY(y);
  return true;
}

// The panel takes every press and wheel notch over it (nothing under it scrolls or is clicked through).
bool RollHelpPopover::onPointer(const PointerEvent& e) { return e.type == PointerType::Wheel || Popover::onPointer(e); }

InfoText RollHelpPopover::info() const {
  return {"Piano Roll Help", "Every control and command for pitch work in the note editor. Click a section on the left to jump "
                             "to it; the mouse wheel, the arrow keys and Page Up / Down scroll. Keys are shown as they are "
                             "bound in the current keymap. Esc or a click outside closes."};
}

RollHelpPopover* openRollHelpPopover(const Widget& from) {
  OverlayLayer* layer = OverlayLayer::of(from);
  return layer ? dynamic_cast<RollHelpPopover*>(layer->top()) : nullptr;
}

RollHelpPopover* toggleRollHelp(Widget& from, ViewContext& ctx, const IRect& over, std::function<void()> onClosed) {
  OverlayLayer* layer = OverlayLayer::of(from);
  if (!layer) return nullptr;
  if (RollHelpPopover* open = openRollHelpPopover(from)) {
    layer->close(*open);
    return nullptr;
  }
  UiTree* tree = layer->tree();
  const Widget* body = tree ? tree->find("main/body") : nullptr;  // L17: kept off the control bar and the status bar
  OverlayOptions o;
  // Its bottom-left corner on the roll's (`over`), growing upwards over the roll and, when it is taller, the panels
  // above: AtPoint flips a popup that does not fit below the point to above it.
  o.anchor = OverlayLayer::anchorRect(from, {over.x, over.bottom(), 0, 0});
  auto pop = std::make_unique<RollHelpPopover>(ctx.keymap(), body, o.anchor.x);
  RollHelpPopover* raw = pop.get();
  o.placement = Placement::AtPoint;
  o.within = body;
  o.onClosed = std::move(onClosed);
  layer->open(std::move(pop), std::move(o));
  return raw;
}

}  // namespace pq::views::roll
