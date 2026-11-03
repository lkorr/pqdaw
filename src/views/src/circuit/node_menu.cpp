// The node menu (TX.node-menu): pq/views/circuit/node_menu.h.

#include "pq/views/circuit/node_menu.h"

#include <algorithm>
#include <cmath>

#include "../common/paint.h"
#include "pq/library/index.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"

namespace pq::views::circuit {

using ui::IRect;
using ui::Rgba;
using ui::Vec2;

NodeMenu::NodeMenu(NodeMenuOptions options) : ui::Control("node-menu"), opt_(std::move(options)) {
  setFocusable(true);
  setAcceptsTextInput(true);
  setVisualOverflow(ui::Insets::all(8));
  tab_ = opt_.tab;
  refresh();
}

ui::ISize NodeMenu::contentExtent(ui::ISize) const {
  const ui::UiScale s = tree() ? tree()->uiScale() : ui::UiScale{};
  return {s.px(kWidth), s.px(kHeight)};
}

NodeMenu::Metrics NodeMenu::metrics() const noexcept {
  const ui::UiScale s = tree() ? tree()->uiScale() : ui::UiScale{};
  Metrics m;
  m.pad = s.px(8);
  m.header = s.px(36);
  m.tabsW = s.px(124);
  m.tabH = s.px(26);
  m.rowH = s.px(22);
  m.headH = s.px(22);
  m.listW = s.px(272);
  return m;
}

IRect NodeMenu::listRect() const noexcept {
  const Metrics m = metrics();
  const IRect r = rect();
  return {r.x + m.tabsW + 1, r.y + m.header, m.listW, std::max(0, r.h - m.header - 1)};
}

IRect NodeMenu::tabRect(NodeTab t) const noexcept {
  const Metrics m = metrics();
  const IRect r = rect();
  return {r.x + 1, r.y + m.header + m.pad / 2 + static_cast<int32_t>(t) * m.tabH, m.tabsW - 1, m.tabH};
}

IRect NodeMenu::previewRect() const noexcept {
  const Metrics m = metrics();
  const IRect r = rect();
  const int32_t x = r.x + m.tabsW + 1 + m.listW + 1;
  return {x, r.y + m.header, std::max(0, r.right() - 1 - x), std::max(0, r.h - m.header - 1)};
}

int32_t NodeMenu::rowTop(int32_t row) const noexcept {
  const Metrics m = metrics();
  int32_t y = m.pad / 2;
  for (int32_t i = 0; i < row && i < static_cast<int32_t>(rows_.size()); ++i)
    y += rows_[static_cast<size_t>(i)].kind == Row::Kind::Header ? m.headH : m.rowH;
  return y;
}

int32_t NodeMenu::contentHeight() const noexcept { return rowTop(static_cast<int32_t>(rows_.size())) + metrics().pad / 2; }

IRect NodeMenu::rowRect(int32_t row) const noexcept {
  if (row < 0 || row >= static_cast<int32_t>(rows_.size())) return {};
  const Metrics m = metrics();
  const IRect l = listRect();
  const int32_t h = rows_[static_cast<size_t>(row)].kind == Row::Kind::Header ? m.headH : m.rowH;
  return {l.x, l.y + rowTop(row) - scroll_, l.w, h};
}

void NodeMenu::setQuery(std::string q) {
  buf_.setText(std::move(q));
  refresh();
}

void NodeMenu::setTab(NodeTab t) {
  if (t == tab_) return;
  tab_ = t;
  refresh();
}

void NodeMenu::refresh() {
  const NodeCatalog& c = *opt_.catalog;
  const std::string& q = buf_.text();
  const std::vector<std::string> recent = tab_ == NodeTab::Recent ? recentNodes() : std::vector<std::string>{};
  const std::vector<NodeHit> hits = searchNodes(c, q, tab_, opt_.filter, recent);
  rows_.clear();
  // A typed spec that names no entry exactly: offered first (Enter places it).
  specValid_ = false;
  bool blank = true;
  for (const char ch : q) blank = blank && ch == ' ';
  if (!blank && tab_ != NodeTab::Recent) {
    std::string folded;
    for (const char ch : q)
      if (ch != ' ' || !folded.empty()) folded.push_back(library::foldChar(ch));
    while (!folded.empty() && folded.back() == ' ') folded.pop_back();
    bool exact = false;
    for (const NodeHit& h : hits) exact = exact || c.entries[h.entry].foldedName == folded;
    if (!exact && nodeSpecValid(q, c.scope, opt_.callees)) {
      specValid_ = true;
      rows_.push_back({Row::Kind::Spec, 0, NodeCategory::Atoms, true});
    }
  }
  // Grouped: a header whenever the category (or compatibility) changes.
  bool first = true;
  NodeCategory cat = NodeCategory::Atoms;
  bool compat = true;
  for (const NodeHit& h : hits) {
    const NodeEntry& e = c.entries[h.entry];
    if (tab_ != NodeTab::Recent && (first || e.category != cat || h.compatible != compat)) {
      rows_.push_back({Row::Kind::Header, 0, e.category, h.compatible});
      cat = e.category;
      compat = h.compatible;
    }
    first = false;
    rows_.push_back({Row::Kind::Entry, h.entry, e.category, h.compatible});
  }
  scroll_ = 0;
  hi_ = step(-1, 1);
  markDirty();
}

int32_t NodeMenu::step(int32_t from, int32_t dir) const noexcept {
  const int32_t n = static_cast<int32_t>(rows_.size());
  for (int32_t i = from + dir; i >= 0 && i < n; i += dir)
    if (rows_[static_cast<size_t>(i)].kind != Row::Kind::Header) return i;
  return from >= 0 && from < n ? from : -1;
}

void NodeMenu::setHighlighted(int32_t row) {
  if (row >= 0 && row < static_cast<int32_t>(rows_.size()) && rows_[static_cast<size_t>(row)].kind == Row::Kind::Header) return;
  if (row < -1 || row >= static_cast<int32_t>(rows_.size())) return;
  if (row == hi_) return;
  hi_ = row;
  ensureVisible(row);
  markDirty();
}

const NodeEntry* NodeMenu::highlightedEntry() const noexcept {
  if (hi_ < 0 || hi_ >= static_cast<int32_t>(rows_.size())) return nullptr;
  const Row& r = rows_[static_cast<size_t>(hi_)];
  return r.kind == Row::Kind::Entry ? &opt_.catalog->entries[r.entry] : nullptr;
}

void NodeMenu::ensureVisible(int32_t row) {
  if (row < 0) return;
  const IRect l = listRect();
  int32_t top = rowTop(row);
  // Keep the group's header in view with its first row.
  if (row > 0 && rows_[static_cast<size_t>(row - 1)].kind == Row::Kind::Header) top = rowTop(row - 1);
  const int32_t bottom = rowTop(row + 1);
  if (top < scroll_) scroll_ = top;
  else if (bottom > scroll_ + l.h) scroll_ = bottom - l.h;
  scroll_ = std::clamp(scroll_, 0, std::max(0, contentHeight() - l.h));
}

bool NodeMenu::choose() {
  if (hi_ < 0 || hi_ >= static_cast<int32_t>(rows_.size()) || closing_) return false;
  const Row r = rows_[static_cast<size_t>(hi_)];
  // Copies: the callbacks may close (and later destroy) this menu.
  const std::shared_ptr<const NodeCatalog> keep = opt_.catalog;
  auto onPick = opt_.onPick;
  auto onSpec = opt_.onSpec;
  const std::string spec = buf_.text();
  close();
  if (r.kind == Row::Kind::Spec) {
    if (onSpec) onSpec(spec);
    return true;
  }
  const NodeEntry& e = keep->entries[r.entry];
  noteRecentNode(e.key);
  if (onPick) onPick(e);
  return true;
}

void NodeMenu::close() {
  if (closing_) return;
  closing_ = true;
  if (ui::OverlayLayer* l = ui::OverlayLayer::of(*this)) l->close(*this);
}

int32_t NodeMenu::rowAt(Vec2 p) const noexcept {
  const IRect l = listRect();
  if (!l.contains(p)) return -1;
  for (int32_t i = 0; i < static_cast<int32_t>(rows_.size()); ++i) {
    const IRect rr = rowRect(i);
    if (p.y >= static_cast<float>(rr.y) && p.y < static_cast<float>(rr.bottom())) return i;
  }
  return -1;
}

bool NodeMenu::onPointer(const ui::PointerEvent& e) {
  switch (e.type) {
    case ui::PointerType::Move: {
      const int32_t r = rowAt(e.pos);
      if (r >= 0 && rows_[static_cast<size_t>(r)].kind != Row::Kind::Header) setHighlighted(r);
      return true;
    }
    case ui::PointerType::Down: {
      if (e.button != ui::PointerButton::Left) return true;
      for (uint8_t t = 0; t < static_cast<uint8_t>(NodeTab::Count); ++t)
        if (tabRect(static_cast<NodeTab>(t)).contains(e.pos)) {
          setTab(static_cast<NodeTab>(t));
          return true;
        }
      const int32_t r = rowAt(e.pos);
      if (r >= 0 && rows_[static_cast<size_t>(r)].kind != Row::Kind::Header) {
        setHighlighted(r);
        (void)choose();
      }
      return true;
    }
    case ui::PointerType::Wheel: {
      const IRect l = listRect();
      const int32_t before = scroll_;
      scroll_ = std::clamp(scroll_ - static_cast<int32_t>(std::lround(e.wheel.y * 3.0f * static_cast<float>(metrics().rowH))), 0,
                           std::max(0, contentHeight() - l.h));
      if (scroll_ != before) markDirty();
      return true;
    }
    default: return true;  // presses inside never reach the layer (no outside-dismiss)
  }
}

bool NodeMenu::onKey(const ui::KeyEvent& e) {
  if (!e.down) return true;  // releases stay here (the canvas behind must not see a lone Tab release as its own)
  const ui::Mods m = e.mods & ui::mod::kChordMask;
  const Metrics mt = metrics();
  const int32_t page = std::max(1, listRect().h / std::max(1, mt.rowH) - 1);
  switch (e.key) {
    case ui::key::kEscape: close(); return true;
    case ui::key::kReturn: (void)choose(); return true;
    case ui::key::kUp: setHighlighted(step(hi_, -1)); return true;
    case ui::key::kDown: setHighlighted(step(hi_, 1)); return true;
    case ui::key::kPageUp: {
      int32_t r = hi_;
      for (int32_t k = 0; k < page; ++k) r = step(r, -1);
      setHighlighted(r);
      return true;
    }
    case ui::key::kPageDown: {
      int32_t r = hi_;
      for (int32_t k = 0; k < page; ++k) r = step(r, 1);
      setHighlighted(r);
      return true;
    }
    case ui::key::kTab: {
      const int32_t n = static_cast<int32_t>(NodeTab::Count);
      const int32_t d = (m & ui::mod::kShift) ? -1 : 1;
      setTab(static_cast<NodeTab>((static_cast<int32_t>(tab_) + d + n) % n));
      return true;
    }
    default: break;
  }
  if ((e.key == ui::key::kHome || e.key == ui::key::kEnd) && (m & ui::mod::kCtrl)) {
    setHighlighted(e.key == ui::key::kHome ? step(-1, 1) : step(static_cast<int32_t>(rows_.size()), -1));
    return true;
  }
  bool changed = false;
  if (tree() && buf_.handleKey(e, tree()->clipboard(), changed)) {
    if (changed) refresh();
    else markDirty();
    return true;
  }
  return !ui::isPrintableKey(e) || true;  // printable keys come as text; nothing reaches the canvas behind
}

bool NodeMenu::onText(const ui::TextEvent& e) {
  const std::string before = buf_.text();
  buf_.insert(e.text());
  if (buf_.text() != before) refresh();
  return true;
}

ui::InfoText NodeMenu::info() const {
  return {"Node menu",
          "Type to search (names, aliases, descriptions) or type a node (tanh, 0.5, 440Hz, * 2, param cutoff = 1kHz). "
          "Up / Down choose, Enter places it at the pointer, Tab / Shift+Tab switch the category, Esc closes."};
}

// ---- painting ---------------------------------------------------------------------------------------------------

namespace {

// A straight segment of width w (device px) as a quad.
void segment(ui::DrawList& dl, Vec2 a, Vec2 b, Rgba col, float w) {
  const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
  if (len <= 0.0f) return;
  const Vec2 n{-dy / len * w * 0.5f, dx / len * w * 0.5f};
  const Vec2 q[4] = {{a.x + n.x, a.y + n.y}, {b.x + n.x, b.y + n.y}, {b.x - n.x, b.y - n.y}, {a.x - n.x, a.y - n.y}};
  dl.addConvexPolyFilled(q, col);
}

// Greedy word wrap of `s` into lines of at most `w` device px (font `f`).
std::vector<std::string> wrap(ui::TextSystem& ts, ui::FontId f, std::string_view s, int32_t w) {
  std::vector<std::string> lines;
  std::string cur;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(' ', i);
    if (j == std::string_view::npos) j = s.size();
    const std::string_view word = s.substr(i, j - i);
    std::string next = cur.empty() ? std::string(word) : cur + " " + std::string(word);
    if (!cur.empty() && ts.measure(f, next) > w) {
      lines.push_back(std::move(cur));
      cur = std::string(word);
    } else {
      cur = std::move(next);
    }
    i = j + 1;
  }
  if (!cur.empty()) lines.push_back(std::move(cur));
  return lines;
}

std::string joinPorts(const std::vector<std::string>& ports) {
  std::string s;
  for (size_t i = 0; i < ports.size(); ++i) s += (i ? ", " : "") + ports[i];
  return s.empty() ? std::string("none") : s;
}

}  // namespace

void NodeMenu::paint(ui::PaintContext& ctx) {
  const ui::Theme* th = ctx.res.theme;
  const ui::ThemeFonts* f = ctx.res.uiFonts;
  if (th == nullptr) return;
  const IRect r = rect();
  const Metrics m = metrics();
  const float rad = ctx.scale.exact(th->radius.menu);
  // Shadow and frame (the overlay's popup look).
  {
    const float d = ctx.scale.exact(3.0f);
    ctx.dl.addRoundedRectFilled({static_cast<float>(r.x) - 2.0f, static_cast<float>(r.y) + d - 1.0f, static_cast<float>(r.right()) + 2.0f,
                                 static_cast<float>(r.bottom()) + d + 2.0f},
                                rad + 2.0f, ui::mulAlpha(th->menu.shadow, 0.5f));
    ctx.dl.addRoundedRectFilled({static_cast<float>(r.x), static_cast<float>(r.y) + d, static_cast<float>(r.right()), static_cast<float>(r.bottom()) + d},
                                rad, th->menu.shadow);
  }
  paint::box(ctx.dl, r, rad, th->surface.panel, th->menu.border);
  // The tab column and the preview pane a step darker than the list.
  const IRect tabs{r.x + 1, r.y + m.header, m.tabsW - 1, r.h - m.header - 1};
  ctx.dl.addRectFilled(tabs.toRect(), th->surface.sunken);
  const IRect pv = previewRect();
  ctx.dl.addRectFilled(pv.toRect(), th->surface.background);
  ctx.dl.addHLine(static_cast<float>(r.x + 1), static_cast<float>(r.right() - 1), static_cast<float>(r.y + m.header), th->menu.separator);
  ctx.dl.addVLine(static_cast<float>(r.x + m.tabsW), static_cast<float>(r.y + m.header), static_cast<float>(r.bottom() - 1), th->menu.separator);
  ctx.dl.addVLine(static_cast<float>(pv.x - 1), static_cast<float>(r.y + m.header), static_cast<float>(r.bottom() - 1), th->menu.separator);
  if (f == nullptr || ctx.res.text == nullptr) return;
  ui::TextSystem& ts = *ctx.res.text;

  // Search box: a field well across the top, the title left of it.
  {
    const std::string title = opt_.title.empty() ? std::string("Add node") : opt_.title;
    const int32_t tw = ts.measure(f->bodyStrong, title) + 2 * m.pad;
    const IRect titleBox{r.x + m.pad / 2, r.y + m.pad / 2, std::min(tw, m.tabsW), m.header - m.pad};
    paint::text(ctx, f->bodyStrong, titleBox, title, th->text.secondary, ui::TextAlign::Left, m.pad / 2, 0);
    const IRect field{r.x + std::max(tw, m.tabsW), r.y + m.pad / 2, r.right() - m.pad - (r.x + std::max(tw, m.tabsW)), m.header - m.pad};
    paint::box(ctx.dl, field, ctx.scale.exact(th->radius.control), th->field.fill, th->field.borderFocus);
    const int32_t padx = ctx.scale.px(8);
    // A magnifier: a ring and a handle.
    const float cx = static_cast<float>(field.x + padx) + ctx.scale.exact(5.0f), cy = static_cast<float>(field.y + field.h / 2) - ctx.scale.exact(1.0f);
    ctx.dl.addCircleStroke({cx, cy}, ctx.scale.exact(4.0f), th->text.secondary, ctx.scale.exact(1.3f));
    segment(ctx.dl, {cx + ctx.scale.exact(3.0f), cy + ctx.scale.exact(3.0f)}, {cx + ctx.scale.exact(6.5f), cy + ctx.scale.exact(6.5f)},
            th->text.secondary, ctx.scale.exact(1.5f));
    const int32_t tx = field.x + padx + ctx.scale.px(16);
    const IRect textBox{tx, field.y, field.right() - padx - tx, field.h};
    if (buf_.text().empty()) {
      paint::text(ctx, f->body, textBox, "Search, or type a node: tanh, 0.5, 440Hz, * 2, param cutoff = 1kHz", th->text.disabled,
                  ui::TextAlign::Left);
    }
    const ui::TextRun run = ts.shape(f->body, buf_.text());
    const int32_t base = paint::capCentredBaseline(field.y, field.h, ts.metrics(f->body).pixelSize);
    const float y0 = static_cast<float>(field.y + ctx.scale.px(5)), y1 = static_cast<float>(field.bottom() - ctx.scale.px(5));
    if (buf_.hasSelection()) {
      const float a = static_cast<float>(tx + run.caretX(buf_.selBegin())), b = static_cast<float>(tx + run.caretX(buf_.selEnd()));
      ctx.dl.addRectFilled({a, y0, b, y1}, th->text.selection);
    }
    ui::TextSystem::drawRunClipped(ctx.dl, run, {static_cast<float>(tx), static_cast<float>(base)}, th->text.primary, paint::textClip(ctx, textBox));
    const float x = static_cast<float>(tx + run.caretX(buf_.caret()));
    ctx.dl.addRectFilled({x, y0, x + std::max(1.0f, std::round(ctx.scale.exact(1.0f))), y1}, th->text.caret);
  }

  // Tabs.
  for (uint8_t t = 0; t < static_cast<uint8_t>(NodeTab::Count); ++t) {
    const NodeTab nt = static_cast<NodeTab>(t);
    const IRect tr = tabRect(nt);
    const bool on = nt == tab_;
    if (on) {
      ctx.dl.addRectFilled(IRect{tr.x, tr.y + 1, tr.w, tr.h - 2}.toRect(), th->surface.raised);
      ctx.dl.addRectFilled(IRect{tr.x, tr.y + 3, ctx.scale.px(3), tr.h - 6}.toRect(), th->accent.primary);
    }
    paint::text(ctx, on ? f->bodyStrong : f->body, tr, tabName(nt), on ? th->text.primary : th->text.secondary, ui::TextAlign::Left, m.pad + ctx.scale.px(4),
                m.pad / 2);
  }

  // The list.
  const IRect l = listRect();
  ctx.dl.pushClip(l.toRect());
  const NodeCatalog& c = *opt_.catalog;
  for (int32_t i = 0; i < static_cast<int32_t>(rows_.size()); ++i) {
    const IRect rr = rowRect(i);
    if (rr.bottom() < l.y) continue;
    if (rr.y > l.bottom()) break;
    const Row& row = rows_[static_cast<size_t>(i)];
    if (row.kind == Row::Kind::Header) {
      std::string head(categoryName(row.category));
      for (char& ch : head) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
      if (!row.compatible) head += "  (NOT CONNECTABLE)";
      paint::text(ctx, f->small, IRect{rr.x, rr.y + ctx.scale.px(4), rr.w, rr.h - ctx.scale.px(4)}, head, th->text.disabled, ui::TextAlign::Left,
                  m.pad + ctx.scale.px(2), m.pad);
      continue;
    }
    const bool hi = i == hi_;
    if (hi) ctx.dl.addRoundedRectFilled(IRect{rr.x + ctx.scale.px(4), rr.y, rr.w - ctx.scale.px(8), rr.h}.toRect(), ctx.scale.exact(3.0f), th->selection.fill);
    const Rgba ink = hi ? th->selection.text : row.compatible ? th->text.primary : th->text.disabled;
    if (row.kind == Row::Kind::Spec) {
      paint::text(ctx, f->body, rr, "Place  " + buf_.text(), ink, ui::TextAlign::Left, m.pad + ctx.scale.px(6), m.pad);
      paint::text(ctx, f->small, rr, "typed", hi ? th->selection.text : th->text.secondary, ui::TextAlign::Right, m.pad, m.pad + ctx.scale.px(4));
      continue;
    }
    const NodeEntry& e = c.entries[row.entry];
    const int32_t drawn = paint::text(ctx, f->body, rr, e.name, ink, ui::TextAlign::Left, m.pad + ctx.scale.px(6), m.pad + ctx.scale.px(44));
    (void)drawn;
    // The ports at a glance: "in → out" dots on the right (filled: has it).
    const float y = static_cast<float>(rr.y) + static_cast<float>(rr.h) * 0.5f;
    const float xr = static_cast<float>(rr.right() - m.pad - ctx.scale.px(6));
    const float dot = ctx.scale.exact(3.0f);
    const Rgba port = hi ? th->selection.text : th->circuit.wireAudio;
    auto pin = [&](float x, bool has) {
      if (has) ctx.dl.addCircleFilled({x, y}, dot, port);
      else ctx.dl.addCircleStroke({x, y}, dot, ui::mulAlpha(port, 0.45f), ctx.scale.exact(1.0f));
    };
    pin(xr - ctx.scale.exact(18.0f), e.hasIn);
    segment(ctx.dl, {xr - ctx.scale.exact(13.0f), y}, {xr - ctx.scale.exact(5.0f), y}, ui::mulAlpha(port, 0.6f), ctx.scale.exact(1.0f));
    pin(xr, e.hasOut);
  }
  if (rows_.empty())
    paint::text(ctx, f->body, IRect{l.x, l.y + m.pad, l.w, m.rowH},
                tab_ == NodeTab::MidiEffects ? "No MIDI effects yet" : tab_ == NodeTab::Recent ? "Nothing placed yet" : "No match",
                th->text.disabled, ui::TextAlign::Left, m.pad + ctx.scale.px(6), m.pad);
  ctx.dl.popClip();
  // A scroll thumb when the list is longer than its pane.
  if (const int32_t ch = contentHeight(); ch > l.h && l.h > 0) {
    const float t0 = static_cast<float>(scroll_) / static_cast<float>(ch), t1 = static_cast<float>(scroll_ + l.h) / static_cast<float>(ch);
    const float x = static_cast<float>(l.right()) - ctx.scale.exact(5.0f);
    ctx.dl.addRoundedRectFilled({x, static_cast<float>(l.y) + t0 * static_cast<float>(l.h) + 2.0f, x + ctx.scale.exact(3.0f),
                                 static_cast<float>(l.y) + t1 * static_cast<float>(l.h) - 2.0f},
                                ctx.scale.exact(1.5f), th->scrollbar.thumb);
  }
  paintPreview(ctx, pv);
}

void NodeMenu::paintPreview(ui::PaintContext& ctx, const IRect& pv) {
  const ui::Theme& th = *ctx.res.theme;
  const ui::ThemeFonts& f = *ctx.res.uiFonts;
  ui::TextSystem& ts = *ctx.res.text;
  const int32_t pad = ctx.scale.px(14);
  const IRect in{pv.x + pad, pv.y + pad, std::max(0, pv.w - 2 * pad), std::max(0, pv.h - 2 * pad)};
  ctx.dl.pushClip(pv.toRect());
  int32_t y = in.y;
  const int32_t line = ctx.scale.px(18);
  if (hi_ >= 0 && rows_[static_cast<size_t>(hi_)].kind == Row::Kind::Spec) {
    paint::text(ctx, f.title, IRect{in.x, y, in.w, ctx.scale.px(22)}, buf_.text(), th.text.primary, ui::TextAlign::Left);
    y += ctx.scale.px(30);
    const std::string desc = opt_.catalog->scope == NodeCatalog::Scope::Project
                                 ? "A typed node: placed as a free device made of this one node (its inputs and outputs as the device's ports)."
                                 : "A typed node, placed as the circuit editor's add field would place it.";
    for (const std::string& ln : wrap(ts, f.body, desc, in.w)) {
      paint::text(ctx, f.body, IRect{in.x, y, in.w, line}, ln, th.text.secondary, ui::TextAlign::Left);
      y += line;
    }
    ctx.dl.popClip();
    return;
  }
  const NodeEntry* e = highlightedEntry();
  if (e == nullptr) {
    ctx.dl.popClip();
    return;
  }
  // Name.
  paint::text(ctx, f.title, IRect{in.x, y, in.w, ctx.scale.px(22)}, e->name, th.text.primary, ui::TextAlign::Left);
  y += ctx.scale.px(28);
  // Kind chip, then "N params, M in".
  {
    const int32_t cw = ts.measure(f.small, e->chip) + ctx.scale.px(12);
    const IRect chip{in.x, y, cw, ctx.scale.px(18)};
    const Rgba tint = e->category == NodeCategory::Atoms         ? th.circuit.wireAudio
                      : e->category == NodeCategory::MetaModules ? th.face.signal
                      : e->category == NodeCategory::Visualisers ? th.circuit.wireMod
                      : e->category == NodeCategory::Instruments ? th.circuit.wireNotes
                                                                 : th.accent.primary;
    paint::box(ctx.dl, chip, ctx.scale.exact(9.0f), ui::mulAlpha(tint, 0.22f), ui::mulAlpha(tint, 0.8f));
    paint::text(ctx, f.small, chip, e->chip, tint, ui::TextAlign::Center);
    const std::string counts = std::to_string(e->params.size()) + (e->params.size() == 1 ? " param, " : " params, ") +
                               std::to_string(e->inputs.size()) + " in";
    paint::text(ctx, f.small, IRect{chip.right() + ctx.scale.px(10), y, in.right() - chip.right() - ctx.scale.px(10), chip.h}, counts,
                th.text.secondary, ui::TextAlign::Left);
    y += chip.h + ctx.scale.px(12);
  }
  // Description.
  for (const std::string& ln : wrap(ts, f.body, e->description, in.w)) {
    if (y + line > in.bottom()) break;
    paint::text(ctx, f.body, IRect{in.x, y, in.w, line}, ln, th.text.primary, ui::TextAlign::Left);
    y += line;
  }
  y += ctx.scale.px(10);
  // Ports.
  auto label = [&](std::string_view s) {
    paint::text(ctx, f.small, IRect{in.x, y, in.w, ctx.scale.px(16)}, s, th.text.disabled, ui::TextAlign::Left);
    y += ctx.scale.px(18);
  };
  label("PORTS");
  {
    const std::string ins = "in   " + joinPorts(e->inputs), outs = "out  " + joinPorts(e->outputs);
    paint::text(ctx, f.small, IRect{in.x, y, in.w, ctx.scale.px(16)}, ins, th.text.secondary, ui::TextAlign::Left);
    y += ctx.scale.px(16);
    paint::text(ctx, f.small, IRect{in.x, y, in.w, ctx.scale.px(16)}, outs, th.text.secondary, ui::TextAlign::Left);
    y += ctx.scale.px(24);
  }
  if (!e->params.empty() && y + ctx.scale.px(18) < in.bottom()) {
    label("PARAMETERS");
    const int32_t colW = (in.w - ctx.scale.px(12)) / 2;
    const int32_t rowH = ctx.scale.px(17);
    const size_t half = (e->params.size() + 1) / 2;
    for (size_t k = 0; k < e->params.size(); ++k) {
      const int32_t col = k < half ? 0 : 1;
      const int32_t ry = y + static_cast<int32_t>(col == 0 ? k : k - half) * rowH;
      if (ry + rowH > in.bottom()) continue;
      const IRect cell{in.x + col * (colW + ctx.scale.px(12)), ry, colW, rowH};
      const int32_t vw = std::min(colW / 2, ts.measure(f.numeric, e->params[k].value) + ctx.scale.px(2));
      paint::text(ctx, f.small, cell, e->params[k].name, th.text.secondary, ui::TextAlign::Left, 0, vw + ctx.scale.px(6));
      paint::text(ctx, f.numeric, cell, e->params[k].value, th.text.primary, ui::TextAlign::Right);
    }
  }
  ctx.dl.popClip();
}

NodeMenu* openNodeMenu(ui::Widget& from, Vec2 at, NodeMenuOptions options) {
  ui::OverlayLayer* layer = ui::OverlayLayer::of(from);
  if (layer == nullptr || !options.catalog) return nullptr;
  auto onClosed = std::move(options.onClosed);
  ui::OverlayOptions o;
  o.anchor = ui::OverlayLayer::anchorRect(from, {static_cast<int32_t>(std::lround(at.x)), static_cast<int32_t>(std::lround(at.y)), 0, 0});
  o.placement = ui::Placement::AtPoint;
  o.takeFocus = true;
  o.onClosed = std::move(onClosed);
  auto menu = std::make_unique<NodeMenu>(std::move(options));
  return &static_cast<NodeMenu&>(layer->open(std::move(menu), std::move(o)));
}

}  // namespace pq::views::circuit
