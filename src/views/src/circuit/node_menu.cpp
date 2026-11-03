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

ui::ISize NodeMenu::contentExtent(ui::ISize viewport) const {
  // TX.node-menu-fix1: never larger than the window (less a margin); the preview pane gives way first.
  const ui::UiScale s = tree() ? tree()->uiScale() : ui::UiScale{};
  const int32_t margin = 2 * s.px(12);
  return {std::min(s.px(kWidth), std::max(s.px(440), viewport.w - margin)), std::min(s.px(kHeight), std::max(s.px(300), viewport.h - margin))};
}

NodeMenu::Metrics NodeMenu::metrics() const noexcept {
  const ui::UiScale s = tree() ? tree()->uiScale() : ui::UiScale{};
  Metrics m;
  m.pad = s.px(8);
  m.header = s.px(46);
  m.tabsW = s.px(150);
  m.tabH = s.px(28);
  m.rowH = s.px(24);
  m.headH = s.px(26);
  m.listW = s.px(262);
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

void NodeMenu::onFrame(double now) {
  // TX.node-menu-fix1: the open animation (kOpenSeconds), then no frames.
  if (openAt_ < 0.0) openAt_ = now;
  markDirty();
  if (now - openAt_ < kOpenSeconds) requestFrame();
  else openAt_ = -2.0;  // done
}

void NodeMenu::finishOpening() noexcept { openAt_ = -2.0; }

float NodeMenu::openProgress() const noexcept {
  if (openAt_ <= -2.0) return 1.0f;
  if (openAt_ < 0.0 || tree() == nullptr) return 0.0f;
  return static_cast<float>(std::clamp((tree()->now() - openAt_) / kOpenSeconds, 0.0, 1.0));
}

void NodeMenu::paint(ui::PaintContext& ctx) {
  // The open animation: faded in and grown from 96 % about the corner at the pointer, eased out.
  const float t = openProgress();
  if (t < 1.0f && openAt_ == -1.0) requestFrame();  // first paint: start the clock (onFrame)
  if (ctx.res.theme == nullptr) return;
  const uint32_t first = ctx.dl.vtxCurrentIdx();
  paintMenu(ctx);
  if (t < 1.0f) {
    const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    const IRect r = rect();
    ctx.dl.fadeSince(first, e, {static_cast<float>(r.x), static_cast<float>(r.y)}, 0.96f + 0.04f * e);
  }
}

namespace {

// A line glyph per tab / category, about `sz` device px across, centred at c (the face's line language: round, thin).
void glyph(ui::DrawList& dl, NodeTab t, Vec2 c, float sz, Rgba col, float lw) {
  const float h = 0.5f * sz;
  switch (t) {
    case NodeTab::All:
      for (int k = 0; k < 4; ++k)
        dl.addCircleFilled({c.x + ((k & 1) ? 0.42f : -0.42f) * h, c.y + ((k & 2) ? 0.42f : -0.42f) * h}, 0.2f * h, col);
      break;
    case NodeTab::Atoms:
      dl.addCircleStroke(c, 0.82f * h, col, lw);
      dl.addCircleFilled(c, 0.28f * h, col);
      break;
    case NodeTab::MetaModules:  // the phase accumulator's dial: an arc, a hub, a hand
      dl.addArcStroke(c, 0.82f * h, 2.4f, 2.4f + 4.6f, col, lw, ui::LineCap::Round);
      dl.addCircleFilled(c, 0.24f * h, col);
      segment(dl, c, {c.x + 0.62f * h, c.y - 0.5f * h}, col, lw);
      break;
    case NodeTab::Instruments:  // three keys
      for (int k = -1; k <= 1; ++k) {
        const float x = c.x + static_cast<float>(k) * 0.62f * h;
        dl.addRoundedRectFilled({x - 0.2f * h, c.y - 0.85f * h, x + 0.2f * h, c.y + 0.85f * h}, 0.15f * h, col);
      }
      break;
    case NodeTab::AudioEffects: {  // a sine
      Vec2 prev{c.x - h, c.y};
      for (int k = 1; k <= 8; ++k) {
        const float u = static_cast<float>(k) / 8.0f;
        const Vec2 q{c.x - h + 2.0f * h * u, c.y - 0.7f * h * std::sin(u * 6.2831853f)};
        segment(dl, prev, q, col, lw);
        prev = q;
      }
      break;
    }
    case NodeTab::MidiEffects:  // a DIN socket
      dl.addCircleStroke(c, 0.82f * h, col, lw);
      for (int k = 0; k < 5; ++k) {
        const float a = 3.14159265f * (1.0f + 0.25f * static_cast<float>(k));
        dl.addCircleFilled({c.x + 0.45f * h * std::cos(a), c.y + 0.45f * h * std::sin(a) + 0.1f * h}, 0.13f * h, col);
      }
      break;
    case NodeTab::Visualisers: {  // a spectrum
      const float hs[4] = {0.5f, 1.0f, 0.75f, 0.35f};
      for (int k = 0; k < 4; ++k) {
        const float x = c.x - 0.78f * h + static_cast<float>(k) * 0.52f * h;
        dl.addRoundedRectFilled({x - 0.15f * h, c.y + 0.85f * h - 1.7f * h * hs[k], x + 0.15f * h, c.y + 0.85f * h}, 0.1f * h, col);
      }
      break;
    }
    case NodeTab::UserLibrary: {  // a bookmark
      const Vec2 p[5] = {{c.x - 0.55f * h, c.y - 0.85f * h}, {c.x + 0.55f * h, c.y - 0.85f * h}, {c.x + 0.55f * h, c.y + 0.85f * h},
                         {c.x, c.y + 0.4f * h}, {c.x - 0.55f * h, c.y + 0.85f * h}};
      for (int k = 0; k < 5; ++k) segment(dl, p[k], p[(k + 1) % 5], col, lw);
      break;
    }
    case NodeTab::Recent:  // a clock
      dl.addCircleStroke(c, 0.82f * h, col, lw);
      segment(dl, c, {c.x, c.y - 0.5f * h}, col, lw);
      segment(dl, c, {c.x + 0.38f * h, c.y + 0.12f * h}, col, lw);
      break;
    case NodeTab::Count: break;
  }
}

// A jack as the cables' plugs sit in it: a dark socket, a metal ring, the cable's colour at its core (device px).
void jack(ui::DrawList& dl, Vec2 c, float r, Rgba col) {
  dl.addCircleFilled(c, r, ui::rgba(22, 24, 28));
  dl.addCircleStroke(c, r, ui::rgba(150, 156, 168, 210), std::max(1.0f, 0.22f * r));
  dl.addCircleFilled(c, 0.45f * r, col);
}

// A soft light behind something (additive blobs): the face's glow.
void softGlow(ui::DrawList& dl, Vec2 c, float r, Rgba col) {
  dl.setBlend(ui::BlendMode::Additive);
  dl.addBlob(c, r, col);
  dl.setBlend(ui::BlendMode::Alpha);
}

}  // namespace

void NodeMenu::paintMenu(ui::PaintContext& ctx) {
  const ui::Theme* th = ctx.res.theme;
  const ui::ThemeFonts* f = ctx.res.uiFonts;
  const auto& F = th->face;
  const IRect r = rect();
  const Metrics m = metrics();
  const float us = ctx.scale.factor;
  const float rad = ctx.scale.exact(10.0f);
  const float hair = std::max(1.0f, std::round(us));
  const ui::Rect rr0 = r.toRect();
  // Shadow, a faint halo in the face's glow, the near-black body, a hairline.
  {
    const float d = ctx.scale.exact(8.0f);
    ctx.dl.addRoundedRectFilled({rr0.x0 - d, rr0.y0 - d * 0.25f, rr0.x1 + d, rr0.y1 + d * 1.5f}, rad + d, ui::rgba(0, 0, 0, 40));
    ctx.dl.addRoundedRectFilled({rr0.x0 - d * 0.5f, rr0.y0, rr0.x1 + d * 0.5f, rr0.y1 + d}, rad + d * 0.5f, ui::rgba(0, 0, 0, 70));
    for (int k = 3; k >= 1; --k) {
      const float g = ctx.scale.exact(1.5f * static_cast<float>(k));
      ctx.dl.addRoundedRectFilled({rr0.x0 - g, rr0.y0 - g, rr0.x1 + g, rr0.y1 + g}, rad + g, ui::setAlpha(F.glow, 0.035f * static_cast<float>(4 - k)));
    }
  }
  ctx.dl.addRoundedRectGradientV(rr0, rad, ui::lerp(F.body, F.hub, 0.35f), F.body);
  ctx.dl.addRoundedRectStroke(rr0, rad, ui::mulAlpha(F.frame, 0.75f), hair);
  // The header's hairline and the rail's: the face's track colour (quiet).
  const float hy = static_cast<float>(r.y + m.header);
  ctx.dl.addHLine(rr0.x0 + rad, rr0.x1 - rad, hy, F.track, hair);
  ctx.dl.addVLine(static_cast<float>(r.x + m.tabsW), hy + static_cast<float>(m.pad), rr0.y1 - static_cast<float>(m.pad), F.track, hair);
  const IRect pv = previewRect();
  ctx.dl.addVLine(static_cast<float>(pv.x - 1), hy + static_cast<float>(m.pad), rr0.y1 - static_cast<float>(m.pad), F.track, hair);
  if (f == nullptr || ctx.res.text == nullptr) return;
  ui::TextSystem& ts = *ctx.res.text;

  // Header: the title (small caps, quiet) over the rail; the search borderless, a thin underline lit under the text.
  {
    std::string title = opt_.title.empty() ? std::string("Add node") : opt_.title;
    for (char& ch : title) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    paint::text(ctx, f->small, IRect{r.x, r.y, m.tabsW, m.header}, title, F.ink, ui::TextAlign::Left, m.pad + ctx.scale.px(8), m.pad / 2);
    const int32_t fx = r.x + m.tabsW + m.pad + ctx.scale.px(6);
    const IRect field{fx, r.y + ctx.scale.px(6), r.right() - m.pad - ctx.scale.px(6) - fx, m.header - ctx.scale.px(12)};
    // The magnifier in the accent.
    const float cx = static_cast<float>(field.x) + ctx.scale.exact(6.0f), cy = static_cast<float>(field.y + field.h / 2) - ctx.scale.exact(1.0f);
    ctx.dl.addCircleStroke({cx, cy}, ctx.scale.exact(4.5f), F.signal, ctx.scale.exact(1.4f));
    segment(ctx.dl, {cx + ctx.scale.exact(3.3f), cy + ctx.scale.exact(3.3f)}, {cx + ctx.scale.exact(7.0f), cy + ctx.scale.exact(7.0f)}, F.signal,
            ctx.scale.exact(1.6f));
    const int32_t tx = field.x + ctx.scale.px(20);
    const IRect textBox{tx, field.y, field.right() - tx, field.h};
    if (buf_.text().empty())
      paint::text(ctx, f->body, textBox, "Search, or type a node: tanh, 0.5, 440Hz, * 2, param cutoff = 1kHz", ui::mulAlpha(F.ink, 0.8f),
                  ui::TextAlign::Left);
    const ui::TextRun run = ts.shape(f->body, buf_.text());
    const int32_t base = paint::capCentredBaseline(field.y, field.h, ts.metrics(f->body).pixelSize);
    const float y0 = static_cast<float>(field.y + ctx.scale.px(6)), y1 = static_cast<float>(field.bottom() - ctx.scale.px(6));
    if (buf_.hasSelection()) {
      const float a = static_cast<float>(tx + run.caretX(buf_.selBegin())), b = static_cast<float>(tx + run.caretX(buf_.selEnd()));
      ctx.dl.addRectFilled({a, y0, b, y1}, ui::setAlpha(F.glow, 0.35f));
    }
    ui::TextSystem::drawRunClipped(ctx.dl, run, {static_cast<float>(tx), static_cast<float>(base)}, th->text.primary, paint::textClip(ctx, textBox));
    const float x = static_cast<float>(tx + run.caretX(buf_.caret()));
    ctx.dl.addRectFilled({x, y0, x + std::max(1.0f, std::round(us)), y1}, F.signal);
    // The underline: a hairline across, lit (and glowing) under what is typed.
    const float uy = static_cast<float>(field.bottom()) - hair;
    ctx.dl.addHLine(static_cast<float>(field.x), static_cast<float>(field.right()), uy, ui::mulAlpha(F.frame, 0.6f), hair);
    const float lit = std::max(ctx.scale.exact(24.0f), static_cast<float>(tx - field.x + run.advance) + ctx.scale.exact(6.0f));
    ctx.dl.addHLine(static_cast<float>(field.x), static_cast<float>(field.x) + lit, uy, F.signal, hair);
    ctx.dl.addRectFilled({static_cast<float>(field.x), uy - ctx.scale.exact(3.0f), static_cast<float>(field.x) + lit, uy},
                         ui::setAlpha(F.glow, 0.08f));
  }

  // The rail: a glyph and a name per tab; the current one lit (a bar, a soft glow, the accent).
  const float glyphSz = ctx.scale.exact(12.0f), lw = std::max(1.0f, ctx.scale.exact(1.3f));
  for (uint8_t k = 0; k < static_cast<uint8_t>(NodeTab::Count); ++k) {
    const NodeTab nt = static_cast<NodeTab>(k);
    const IRect tr = tabRect(nt);
    const bool on = nt == tab_;
    const IRect inner{tr.x + ctx.scale.px(6), tr.y + ctx.scale.px(2), tr.w - ctx.scale.px(12), tr.h - ctx.scale.px(4)};
    if (on) {
      ctx.dl.addRoundedRectFilled(inner.toRect(), ctx.scale.exact(6.0f), ui::setAlpha(F.glow, 0.12f));
      ctx.dl.addRoundedRectFilled({static_cast<float>(inner.x), static_cast<float>(inner.y) + ctx.scale.exact(5.0f),
                                   static_cast<float>(inner.x) + ctx.scale.exact(2.0f), static_cast<float>(inner.bottom()) - ctx.scale.exact(5.0f)},
                                  ctx.scale.exact(1.0f), F.signal);
    }
    const Vec2 gc{static_cast<float>(inner.x) + ctx.scale.exact(15.0f), static_cast<float>(inner.y + inner.h / 2)};
    if (on) softGlow(ctx.dl, gc, ctx.scale.exact(14.0f), ui::setAlpha(F.glow, 0.35f));
    glyph(ctx.dl, nt, gc, glyphSz, on ? F.signal : F.ink, lw);
    paint::text(ctx, f->body, inner, tabName(nt), on ? th->text.primary : ui::lerp(F.ink, th->text.secondary, 0.5f), ui::TextAlign::Left,
                ctx.scale.px(28), ctx.scale.px(4));
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
      if (!row.compatible) head += "  \xC2\xB7  NOT CONNECTABLE";
      const Vec2 gc{static_cast<float>(rr.x + m.pad) + ctx.scale.exact(8.0f), static_cast<float>(rr.y + rr.h / 2) + ctx.scale.exact(2.0f)};
      glyph(ctx.dl, tabOf(row.category), gc, ctx.scale.exact(9.0f), ui::mulAlpha(F.ink, 0.85f), std::max(1.0f, ctx.scale.exact(1.1f)));
      paint::text(ctx, f->small, IRect{rr.x, rr.y + ctx.scale.px(4), rr.w, rr.h - ctx.scale.px(4)}, head, F.ink, ui::TextAlign::Left,
                  m.pad + ctx.scale.px(18), m.pad);
      continue;
    }
    const bool hi = i == hi_;
    const IRect cell{rr.x + ctx.scale.px(6), rr.y + ctx.scale.px(1), rr.w - ctx.scale.px(14), rr.h - ctx.scale.px(2)};
    if (hi) {
      ctx.dl.addRoundedRectFilled(cell.toRect(), ctx.scale.exact(6.0f), ui::setAlpha(F.glow, 0.16f));
      ctx.dl.addRoundedRectStroke(cell.toRect(), ctx.scale.exact(6.0f), ui::setAlpha(F.signal, 0.28f), hair);
      const float bx = static_cast<float>(cell.x) + ctx.scale.exact(1.0f);
      softGlow(ctx.dl, {bx, static_cast<float>(cell.y + cell.h / 2)}, ctx.scale.exact(12.0f), ui::setAlpha(F.glow, 0.45f));
      ctx.dl.addRoundedRectFilled({bx, static_cast<float>(cell.y) + ctx.scale.exact(5.0f), bx + ctx.scale.exact(2.0f),
                                   static_cast<float>(cell.bottom()) - ctx.scale.exact(5.0f)},
                                  ctx.scale.exact(1.0f), F.signal);
    }
    const Rgba ink = hi ? ui::lerp(th->text.primary, F.signal, 0.25f) : row.compatible ? th->text.primary : th->text.disabled;
    if (row.kind == Row::Kind::Spec) {
      paint::text(ctx, f->body, rr, "Place  " + buf_.text(), ink, ui::TextAlign::Left, m.pad + ctx.scale.px(10), m.pad);
      paint::text(ctx, f->small, rr, "TYPED", hi ? F.signal : F.ink, ui::TextAlign::Right, m.pad, m.pad + ctx.scale.px(10));
      continue;
    }
    const NodeEntry& e = c.entries[row.entry];
    (void)paint::text(ctx, f->body, rr, e.name, ink, ui::TextAlign::Left, m.pad + ctx.scale.px(10), m.pad + ctx.scale.px(48));
    // The ports at a glance: in → out, jacks where it has them.
    const float y = static_cast<float>(rr.y) + static_cast<float>(rr.h) * 0.5f;
    const float xr = static_cast<float>(rr.right() - m.pad - ctx.scale.px(12));
    const float dot = ctx.scale.exact(2.6f);
    const Rgba port = hi ? F.signal : row.compatible ? ui::mulAlpha(F.ink, 0.9f) : ui::mulAlpha(F.ink, 0.45f);
    auto pin = [&](float x, bool has) {
      if (has) ctx.dl.addCircleFilled({x, y}, dot, port);
      else ctx.dl.addCircleStroke({x, y}, dot, ui::mulAlpha(port, 0.5f), ctx.scale.exact(1.0f));
    };
    pin(xr - ctx.scale.exact(18.0f), e.hasIn);
    segment(ctx.dl, {xr - ctx.scale.exact(13.5f), y}, {xr - ctx.scale.exact(4.5f), y}, ui::mulAlpha(port, 0.6f), ctx.scale.exact(1.0f));
    pin(xr, e.hasOut);
  }
  if (rows_.empty())
    paint::text(ctx, f->body, IRect{l.x, l.y + m.pad, l.w, m.rowH},
                tab_ == NodeTab::MidiEffects ? "No MIDI effects yet" : tab_ == NodeTab::Recent ? "Nothing placed yet" : "No match", F.ink,
                ui::TextAlign::Left, m.pad + ctx.scale.px(10), m.pad);
  ctx.dl.popClip();
  // A thin scroll thumb when the list is longer than its pane.
  if (const int32_t ch = contentHeight(); ch > l.h && l.h > 0) {
    const float t0 = static_cast<float>(scroll_) / static_cast<float>(ch), t1 = static_cast<float>(scroll_ + l.h) / static_cast<float>(ch);
    const float x = static_cast<float>(l.right()) - ctx.scale.exact(4.0f);
    ctx.dl.addRoundedRectFilled({x, static_cast<float>(l.y) + t0 * static_cast<float>(l.h) + 2.0f, x + ctx.scale.exact(2.0f),
                                 static_cast<float>(l.y) + t1 * static_cast<float>(l.h) - 2.0f},
                                ctx.scale.exact(1.0f), ui::mulAlpha(F.frame, 0.9f));
  }
  paintPreview(ctx, pv);
}

void NodeMenu::paintPreview(ui::PaintContext& ctx, const IRect& pv) {
  const ui::Theme& th = *ctx.res.theme;
  const auto& F = th.face;
  const ui::ThemeFonts& f = *ctx.res.uiFonts;
  ui::TextSystem& ts = *ctx.res.text;
  const int32_t pad = ctx.scale.px(16);
  const IRect in{pv.x + pad, pv.y + pad, std::max(0, pv.w - 2 * pad), std::max(0, pv.h - 2 * pad)};
  ctx.dl.pushClip(pv.toRect());
  int32_t y = in.y;
  const int32_t line = ctx.scale.px(17);
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
  // Name, then the kind chip (an outlined pill in the category's tint, its glyph) and "N params, M in".
  paint::text(ctx, f.title, IRect{in.x, y, in.w, ctx.scale.px(22)}, e->name, th.text.primary, ui::TextAlign::Left);
  y += ctx.scale.px(28);
  {
    const Rgba tint = e->category == NodeCategory::Atoms         ? th.circuit.wireAudio
                      : e->category == NodeCategory::MetaModules ? F.signal
                      : e->category == NodeCategory::Visualisers ? th.circuit.wireMod
                      : e->category == NodeCategory::Instruments ? th.circuit.wireNotes
                                                                 : F.value;
    const int32_t cw = ts.measure(f.small, e->chip) + ctx.scale.px(30);
    const IRect chip{in.x, y, cw, ctx.scale.px(18)};
    ctx.dl.addRoundedRectFilled(chip.toRect(), ctx.scale.exact(9.0f), ui::mulAlpha(tint, 0.12f));
    ctx.dl.addRoundedRectStroke(chip.toRect(), ctx.scale.exact(9.0f), ui::mulAlpha(tint, 0.7f), std::max(1.0f, std::round(ctx.scale.factor)));
    glyph(ctx.dl, tabOf(e->category), {static_cast<float>(chip.x) + ctx.scale.exact(11.0f), static_cast<float>(chip.y + chip.h / 2)},
          ctx.scale.exact(9.0f), tint, std::max(1.0f, ctx.scale.exact(1.1f)));
    paint::text(ctx, f.small, chip, e->chip, tint, ui::TextAlign::Left, ctx.scale.px(20), ctx.scale.px(6));
    const std::string counts = std::to_string(e->params.size()) + (e->params.size() == 1 ? " param  \xC2\xB7  " : " params  \xC2\xB7  ") +
                               std::to_string(e->inputs.size()) + " in  \xC2\xB7  " + std::to_string(e->outputs.size()) + " out";
    paint::text(ctx, f.small, IRect{chip.right() + ctx.scale.px(10), y, in.right() - chip.right() - ctx.scale.px(10), chip.h}, counts, F.ink,
                ui::TextAlign::Left);
    y += chip.h + ctx.scale.px(12);
  }
  // Description.
  for (const std::string& ln : wrap(ts, f.body, e->description, in.w)) {
    if (y + line > in.bottom()) break;
    paint::text(ctx, f.body, IRect{in.x, y, in.w, line}, ln, th.text.secondary, ui::TextAlign::Left);
    y += line;
  }
  y += ctx.scale.px(12);
  // The ports as a mini block: its inputs' jacks on the left, its outputs' on the right, each with a short cable in its
  // signal's colour (as the cables plug into the blocks), the names inside.
  {
    const size_t rowsN = std::max<size_t>(1, std::max(e->inputs.size(), e->outputs.size()));
    const int32_t pitch = ctx.scale.px(16);
    const int32_t bw = std::min(in.w - ctx.scale.px(60), ctx.scale.px(220));
    const int32_t bh = static_cast<int32_t>(rowsN) * pitch + ctx.scale.px(12);
    if (y + bh <= in.bottom() && bw > ctx.scale.px(80)) {
      const IRect blk{in.x + (in.w - bw) / 2, y, bw, bh};
      ctx.dl.addRoundedRectFilled(blk.toRect(), ctx.scale.exact(6.0f), F.hub);
      ctx.dl.addRoundedRectStroke(blk.toRect(), ctx.scale.exact(6.0f), ui::mulAlpha(F.frame, 0.8f), std::max(1.0f, std::round(ctx.scale.factor)));
      const float jr = ctx.scale.exact(4.0f), stub = ctx.scale.exact(22.0f), cwid = ctx.scale.exact(3.0f);
      auto colourOf = [&](const std::string& name, bool main) {
        if (name == "notes") return th.circuit.wireNotes;
        return main ? th.circuit.wireAudio : th.circuit.wireMod;
      };
      auto side = [&](const std::vector<std::string>& ports, bool left) {
        for (size_t k = 0; k < ports.size(); ++k) {
          const float py = static_cast<float>(blk.y + ctx.scale.px(6) + static_cast<int32_t>(k) * pitch + pitch / 2);
          const float px = left ? static_cast<float>(blk.x) : static_cast<float>(blk.right());
          const Rgba col = colourOf(ports[k], k == 0);
          const float sx = left ? px - stub : px + stub;
          // The cable: a short tube (a darker rim, the colour, a highlight) fading out away from the jack.
          ctx.dl.addRectFilledMultiColor({std::min(px, sx), py - cwid * 0.5f, std::max(px, sx), py + cwid * 0.5f},
                                         left ? ui::withAlpha(col, 0) : col, left ? col : ui::withAlpha(col, 0),
                                         left ? col : ui::withAlpha(col, 0), left ? ui::withAlpha(col, 0) : col);
          jack(ctx.dl, {px, py}, jr, col);
          const IRect lab{left ? blk.x + ctx.scale.px(10) : blk.x + blk.w / 2, static_cast<int32_t>(py) - pitch / 2, blk.w / 2 - ctx.scale.px(12), pitch};
          paint::text(ctx, f.small, lab, ports[k], th.text.secondary, left ? ui::TextAlign::Left : ui::TextAlign::Right);
        }
      };
      side(e->inputs, true);
      side(e->outputs, false);
      if (e->inputs.empty() && e->outputs.empty())
        paint::text(ctx, f.small, blk, "no signal ports", F.ink, ui::TextAlign::Center);
      y += bh + ctx.scale.px(16);
    }
  }
  if (!e->params.empty() && y + ctx.scale.px(18) < in.bottom()) {
    paint::text(ctx, f.small, IRect{in.x, y, in.w, ctx.scale.px(16)}, "PARAMETERS", F.ink, ui::TextAlign::Left);
    y += ctx.scale.px(20);
    const int32_t colW = (in.w - ctx.scale.px(16)) / 2;
    const int32_t rowH = ctx.scale.px(17);
    const size_t half = (e->params.size() + 1) / 2;
    for (size_t k = 0; k < e->params.size(); ++k) {
      const int32_t col = k < half ? 0 : 1;
      const int32_t ry = y + static_cast<int32_t>(col == 0 ? k : k - half) * rowH;
      if (ry + rowH > in.bottom()) continue;
      const IRect cell{in.x + col * (colW + ctx.scale.px(16)), ry, colW, rowH};
      const int32_t vw = std::min(colW / 2, ts.measure(f.numeric, e->params[k].value) + ctx.scale.px(2));
      paint::text(ctx, f.small, cell, e->params[k].name, th.text.secondary, ui::TextAlign::Left, 0, vw + ctx.scale.px(6));
      paint::text(ctx, f.numeric, cell, e->params[k].value, F.value, ui::TextAlign::Right);
    }
  }
  ctx.dl.popClip();
}

NodeMenu* openNodeMenu(ui::Widget& from, Vec2 at, NodeMenuOptions options) {
  ui::OverlayLayer* layer = ui::OverlayLayer::of(from);
  if (layer == nullptr || !options.catalog) return nullptr;
  auto onClosed = std::move(options.onClosed);
  ui::OverlayOptions o;
  // TX.node-menu-fix1: just right of and below the pointer (left of it when there is no room on the right), so the
  // point where the node will be placed (marked by the opener) and a cable dropped there stay in sight.
  const int32_t g = from.tree() != nullptr ? from.tree()->uiScale().px(NodeMenu::kPointerGap) : static_cast<int32_t>(NodeMenu::kPointerGap);
  o.anchor = ui::OverlayLayer::anchorRect(from, {static_cast<int32_t>(std::lround(at.x)) - g, static_cast<int32_t>(std::lround(at.y)) + g, 2 * g, 0});
  o.placement = ui::Placement::Right;
  o.takeFocus = true;
  o.onClosed = std::move(onClosed);
  auto menu = std::make_unique<NodeMenu>(std::move(options));
  return &static_cast<NodeMenu&>(layer->open(std::move(menu), std::move(o)));
}

}  // namespace pq::views::circuit
