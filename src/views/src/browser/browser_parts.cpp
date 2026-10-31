// T5.10: the browser panel's parts (browser_parts.h): chips, sidebar, column header, list, preview strip.

#include "browser_parts.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

#include "../common/paint.h"
#include "../common/waveform_data.h"
#include "pq/ui/overlay.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/waveform.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/preview_port.h"

namespace pq::views::browser {

using namespace pq::ui;
namespace bm = metrics;

Rgba collectionColour(uint32_t c) noexcept {
  static constexpr Rgba kColours[library::kCollectionCount] = {
      rgba(229, 83, 75), rgba(237, 146, 56), rgba(229, 207, 77), rgba(120, 196, 92),
      rgba(88, 155, 229), rgba(168, 112, 214), rgba(150, 150, 150)};
  return kColours[std::min<uint32_t>(c, library::kCollectionCount - 1)];
}

// --- Chips ---------------------------------------------------------------------------------------------------------

Chips::Chips(BrowserPanel& panel) : Control("chips"), panel_(panel) {
  setInfo("Collections", "Shows only the files in the colour collections that are lit (several: any of them). Assign "
                         "collections from a file's right-click menu.");
  setCursor(Cursor::Pointer);
}

IRect Chips::chipRect(uint32_t c) const noexcept {
  const IRect r = rect();
  const float s = tree() ? tree()->scale() : 1.0f;
  const int32_t sz = static_cast<int32_t>(std::lround(bm::kChipSize * s));
  const int32_t gap = static_cast<int32_t>(std::lround(5.0f * s));
  return {r.x + static_cast<int32_t>(c) * (sz + gap), r.y + (r.h - sz) / 2, sz, sz};
}

void Chips::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const uint8_t on = panel_.collectionFilter();
  for (uint32_t c = 0; c < library::kCollectionCount; ++c) {
    const IRect r = chipRect(c);
    const Rgba col = collectionColour(c);
    if (on & (1u << c)) {
      paint::box(ctx.dl, r, 2.0f * ctx.scale.factor, col, th->text.primary);
    } else {
      paint::box(ctx.dl, r, 2.0f * ctx.scale.factor, withAlpha(col, 70), withAlpha(col, 200));
    }
  }
}

bool Chips::onPointer(const PointerEvent& e) {
  if (e.type != PointerType::Down || e.button != PointerButton::Left) return false;
  for (uint32_t c = 0; c < library::kCollectionCount; ++c)
    if (chipRect(c).contains(e.pos)) {
      panel_.setCollectionFilter(static_cast<uint8_t>(panel_.collectionFilter() ^ (1u << c)));
      return true;
    }
  return false;
}

// --- FilterChips (T5.11) ----------------------------------------------------------------------------------------------

FilterChips::FilterChips(BrowserPanel& panel) : Control("filters"), panel_(panel) {
  setInfo("Filters and Tags", "Duration, BPM, sample rate and channels: a click steps to the next range, a right-click "
                              "back to Any. Tags: a click shows only files with that tag (several: all of them). Give "
                              "a file tags from its right-click menu.");
  setCursor(Cursor::Pointer);
}

std::string FilterChips::chipLabel(size_t i) const {
  const BrowserFilters& f = panel_.filters();
  if (i < 4) {
    const uint8_t st[4] = {f.duration, f.bpm, f.rate, f.channels};
    return filterChipLabel(static_cast<uint32_t>(i), st[i]);
  }
  const auto& tags = panel_.allTags();
  return i - 4 < tags.size() ? "#" + tags[i - 4] : std::string();
}

void FilterChips::invalidateChips() {
  rects_.clear();
  invalidateArrange();
  markDirty();
}

void FilterChips::arrange(LayoutPass&) { layoutChips(); }

IRect FilterChips::chipRect(size_t i) const noexcept {
  if (rects_.size() != 4 + panel_.allTags().size()) layoutChips();
  return i < rects_.size() ? rects_[i] : IRect{};
}

void FilterChips::layoutChips() const {
  rects_.clear();
  const IRect r = rect();
  const float s = tree() ? tree()->scale() : 1.0f;
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  const int32_t pad = static_cast<int32_t>(std::lround(5.0f * s)), gap = static_cast<int32_t>(std::lround(4.0f * s));
  int32_t x = r.x;
  const size_t n = 4 + panel_.allTags().size();
  for (size_t i = 0; i < n; ++i) {
    const std::string label = chipLabel(i);
    int32_t w = static_cast<int32_t>(std::lround(6.0f * s * static_cast<float>(label.size())));
    if (res && res->text && res->uiFonts) w = res->text->measure(res->uiFonts->small, label);
    w += 2 * pad;
    if (x + w > r.right()) {
      rects_.push_back({});
      continue;
    }
    rects_.push_back({x, r.y, w, r.h});
    x += w + gap;
  }
}

void FilterChips::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  const ThemeFonts* f = ctx.res.uiFonts;
  if (!th || !f) return;
  if (rects_.size() != 4 + panel_.allTags().size()) layoutChips();
  const BrowserFilters& fl = panel_.filters();
  const uint8_t st[4] = {fl.duration, fl.bpm, fl.rate, fl.channels};
  for (size_t i = 0; i < rects_.size(); ++i) {
    const IRect cr = rects_[i];
    if (cr.empty()) continue;
    bool on = i < 4 ? st[i] != 0 : false;
    if (i >= 4) {
      const std::string& t = panel_.allTags()[i - 4];
      on = std::find(fl.tags.begin(), fl.tags.end(), t) != fl.tags.end();
    }
    paint::box(ctx.dl, cr, static_cast<float>(cr.h) * 0.5f, on ? th->accent.primary : th->surface.sunken,
               on ? th->accent.primary : th->surface.divider);
    paint::text(ctx, f->small, cr, chipLabel(i), on ? th->text.onAccent : th->text.secondary, TextAlign::Center, 2, 2);
  }
}

bool FilterChips::onPointer(const PointerEvent& e) {
  if (e.type != PointerType::Down || (e.button != PointerButton::Left && e.button != PointerButton::Right)) return false;
  if (rects_.size() != 4 + panel_.allTags().size()) layoutChips();
  for (size_t i = 0; i < rects_.size(); ++i) {
    if (rects_[i].empty() || !rects_[i].contains(e.pos)) continue;
    BrowserFilters f = panel_.filters();
    if (i < 4) {
      uint8_t* st[4] = {&f.duration, &f.bpm, &f.rate, &f.channels};
      *st[i] = e.button == PointerButton::Right ? 0 : static_cast<uint8_t>((*st[i] + 1) % kFilterSteps[i]);
    } else {
      const std::string& t = panel_.allTags()[i - 4];
      const auto it = std::find(f.tags.begin(), f.tags.end(), t);
      if (it != f.tags.end()) f.tags.erase(it);
      else if (e.button == PointerButton::Left) f.tags.push_back(t);
    }
    panel_.setFilters(std::move(f));
    invalidateChips();
    return true;
  }
  return true;
}

// --- HotSwapBar (T5.11) ------------------------------------------------------------------------------------------------

HotSwapBar::HotSwapBar(BrowserPanel& panel) : Control("hotswap"), panel_(panel) {
  setInfo("Hot-Swap", "Selecting an item loads it in place of the device's preset (or the tuning). Enter loads it and "
                      "leaves hot-swap; Esc or the cross leaves without a change.");
}

IRect HotSwapBar::closeRect() const noexcept {
  const IRect r = rect();
  const float s = tree() ? tree()->scale() : 1.0f;
  const int32_t sz = static_cast<int32_t>(std::lround(14.0f * s));
  return {r.right() - sz - static_cast<int32_t>(std::lround(6.0f * s)), r.y + (r.h - sz) / 2, sz, sz};
}

void HotSwapBar::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  const ThemeFonts* f = ctx.res.uiFonts;
  if (!th) return;
  const IRect r = rect();
  ctx.dl.addRectFilled(r.toRect(), th->accent.primary);
  if (!f) return;
  const HotSwapTarget& h = panel_.hotSwap();
  const std::string what = h.kind == HotSwapTarget::Kind::Tuning ? "tunings for " : "presets of ";
  paint::text(ctx, f->small, {r.x, r.y, closeRect().x - r.x, r.h}, "Hot-Swap: " + what + h.label, th->text.onAccent,
              TextAlign::Left, ctx.scale.px(8.0f), ctx.scale.px(4.0f));
  paint::text(ctx, f->small, closeRect(), "\xC3\x97", th->text.onAccent, TextAlign::Center, 0, 0);
}

bool HotSwapBar::onPointer(const PointerEvent& e) {
  if (e.type == PointerType::Down && e.button == PointerButton::Left && closeRect().contains(e.pos)) {
    panel_.setHotSwap({});
    return true;
  }
  return e.type == PointerType::Down;
}

// --- WidthHandle (T5.11) -----------------------------------------------------------------------------------------------

WidthHandle::WidthHandle(ViewContext& ctx, Widget& slot) : Control("resize"), ctx_(ctx), slot_(slot) {
  setInfo("Browser Width", "Drag to make the browser wider or narrower; double-click for the default width.");
}

void WidthHandle::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  ctx.dl.addRectFilled(rect().toRect(), dragging_ || visualState().hover ? th->accent.primary : th->surface.divider);
}

bool WidthHandle::onPointer(const PointerEvent& e) {
  const float s = tree() ? tree()->scale() : 1.0f;
  auto apply = [&](float w) {
    w = clampBrowserWidth(w);
    slot_.setWidth(Length::fixed(w));
    if (Widget* p = slot_.parent()) p->invalidateLayout();
    ViewPrefs prefs = ctx_.viewPrefs();
    prefs.browserWidth = w;
    ctx_.setViewPrefs(prefs);
  };
  switch (e.type) {
    case PointerType::Down:
      if (e.button != PointerButton::Left) return false;
      if (e.clickCount >= 2) {
        apply(bm::kPanelWidth);
        return true;
      }
      dragging_ = true;
      pressX_ = e.pos.x;
      startW_ = static_cast<float>(slot_.rect().w) / s;
      markDirty();
      return true;
    case PointerType::Move:
      if (dragging_) apply(startW_ + (e.pos.x - pressX_) / s);
      return dragging_;
    case PointerType::Up:
      dragging_ = false;
      markDirty();
      return true;
    default: return false;
  }
}

// --- Sidebar ---------------------------------------------------------------------------------------------------------

Sidebar::Sidebar(BrowserPanel& panel) : Control("sidebar"), panel_(panel) {
  setInfo("Browser Sidebar", "Collections (your colour labels), Categories (every place's files by kind) and Places "
                             "(the User Library, this project's folder, folders you added: drop a folder here). A click "
                             "shows it in the list.");
  setClipChildren(true);
}

void Sidebar::rebuild() {
  std::vector<Item> v;
  v.push_back({"COLLECTIONS", true, {}, 0});
  Library& lib = panel_.context().library();
  const auto& names = lib.tags().collectionNames();
  for (uint32_t c = 0; c < library::kCollectionCount; ++c)
    v.push_back({names[c], false, {BrowserSource::Kind::Collection, c}, collectionColour(c)});
  v.push_back({"CATEGORIES", true, {}, 0});
  for (uint32_t c = 1; c < static_cast<uint32_t>(library::Category::Count); ++c)
    v.push_back({std::string(library::categoryName(static_cast<library::Category>(c))), false,
                 {BrowserSource::Kind::Category, c}, 0});
  v.push_back({"PLACES", true, {}, 0});
  const library::Index* ix = panel_.index();
  const auto& places = ix ? ix->places : lib.places();
  for (uint32_t p = 0; p < places.size(); ++p) v.push_back({places[p].name, false, {BrowserSource::Kind::Place, p}, 0});
  items_ = std::move(v);
  markDirty();
}

IRect Sidebar::itemRect(size_t i) const noexcept {
  const IRect r = rect();
  const float s = tree() ? tree()->scale() : 1.0f;
  const int32_t h = static_cast<int32_t>(std::lround(bm::kRowHeight * s));
  const int32_t top = static_cast<int32_t>(std::lround(4.0f * s));
  return {r.x, r.y + top + static_cast<int32_t>(i) * h, r.w, h};
}

void Sidebar::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  const ThemeFonts* f = ctx.res.uiFonts;
  if (!th) return;
  const IRect r = rect();
  ctx.dl.addRectFilled(r.toRect(), th->surface.raised);
  if (!f) return;
  const int32_t pad = ctx.scale.px(8.0f);
  for (size_t i = 0; i < items_.size(); ++i) {
    const IRect ir = itemRect(i);
    if (ir.y >= r.bottom()) break;
    if (intersect(ir, ctx.clip).empty()) continue;
    const Item& it = items_[i];
    if (it.header) {
      paint::text(ctx, f->small, ir, it.label, th->text.disabled, TextAlign::Left, pad / 2, pad / 2);
      continue;
    }
    const bool sel = panel_.source() == it.source;
    if (sel) ctx.dl.addRectFilled(ir.toRect(), th->selection.fill);
    int32_t x = pad;
    if (it.swatch) {
      const int32_t sw = ctx.scale.px(8.0f);
      paint::box(ctx.dl, {ir.x + pad, ir.y + (ir.h - sw) / 2, sw, sw}, 1.5f * ctx.scale.factor, it.swatch, 0);
      x += sw + ctx.scale.px(5.0f);
    }
    paint::text(ctx, f->small, ir, it.label, sel ? th->selection.text : th->text.primary, TextAlign::Left, x, pad / 2);
  }
}

bool Sidebar::onPointer(const PointerEvent& e) {
  if (e.type != PointerType::Down) return false;
  for (size_t i = 0; i < items_.size(); ++i)
    if (!items_[i].header && itemRect(i).contains(e.pos)) {
      panel_.setSource(items_[i].source);
      if (e.button == PointerButton::Right && items_[i].source.kind == BrowserSource::Kind::Place) {
        Library& lib = panel_.context().library();
        const uint32_t p = items_[i].source.index;
        const library::Index* ix = panel_.index();
        const auto& places = ix ? ix->places : lib.places();
        if (p < places.size() && places[p].kind == library::PlaceKind::Folder) {
          const std::string root = places[p].root;
          (void)ui::openContextMenu(*this, e.pos, {MenuItem::item("Remove from Places", [&lib, root] { (void)lib.removePlace(root); })});
        }
      }
      return true;
    }
  return true;
}

// --- ColumnHeader ------------------------------------------------------------------------------------------------------

ColumnHeader::ColumnHeader(BrowserPanel& panel) : Control("columns"), panel_(panel) {
  setInfo("Columns", "Click a column to sort the list by it; click again to reverse the order.");
  setCursor(Cursor::Pointer);
}

void ColumnHeader::columns(const IRect& r, float s, int32_t (&x0)[5], int32_t (&x1)[5]) noexcept {
  const float widths[4] = {bm::kTypeWidth, bm::kDurWidth, bm::kBpmWidth, bm::kRateWidth};
  int32_t right = r.right() - static_cast<int32_t>(std::lround(4.0f * s));
  for (int i = 4; i >= 1; --i) {
    x1[i] = right;
    x0[i] = right - static_cast<int32_t>(std::lround(widths[i - 1] * s));
    right = x0[i];
  }
  x0[0] = r.x;
  x1[0] = std::max(r.x, right);
}

IRect ColumnHeader::columnRect(BrowserColumn c) const noexcept {
  int32_t x0[5], x1[5];
  const IRect r = rect();
  columns(r, tree() ? tree()->scale() : 1.0f, x0, x1);
  const auto i = static_cast<size_t>(c);
  if (i > 4) return {};
  return {x0[i], r.y, x1[i] - x0[i], r.h};
}

void ColumnHeader::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  const ThemeFonts* f = ctx.res.uiFonts;
  if (!th) return;
  const IRect r = rect();
  ctx.dl.addRectFilled(r.toRect(), th->surface.raised);
  if (!f) return;
  int32_t x0[5], x1[5];
  columns(r, ctx.scale.factor, x0, x1);
  static constexpr const char* kNames[5] = {"Name", "Type", "Dur", "BPM", "Rate"};
  const int32_t pad = ctx.scale.px(6.0f);
  for (int i = 0; i < 5; ++i) {
    const IRect cr{x0[i], r.y, x1[i] - x0[i], r.h};
    std::string label = kNames[i];
    if (panel_.sortColumn() == static_cast<BrowserColumn>(i)) label += panel_.sortDescending() ? " \xE2\x96\xBE" : " \xE2\x96\xB4";
    paint::text(ctx, f->small, cr, label, th->text.secondary, i == 0 ? TextAlign::Left : TextAlign::Right, pad, i == 0 ? pad : 2);
  }
  ctx.dl.addRectFilled({static_cast<float>(r.x), static_cast<float>(r.bottom() - 1), static_cast<float>(r.right()),
                        static_cast<float>(r.bottom())},
                       th->surface.divider);
}

bool ColumnHeader::onPointer(const PointerEvent& e) {
  if (e.type != PointerType::Down || e.button != PointerButton::Left) return false;
  for (int i = 0; i < 5; ++i) {
    const auto c = static_cast<BrowserColumn>(i);
    if (columnRect(c).contains(e.pos)) {
      panel_.setSort(c, panel_.sortColumn() == c ? !panel_.sortDescending() : false);
      markDirty();
      return true;
    }
  }
  return true;
}

// --- List ------------------------------------------------------------------------------------------------------------

List::List(BrowserPanel& panel) : Widget("list"), panel_(panel) {
  setLayout(LayoutKind::Custom);
  setFocusable(true);
}

int32_t List::rowHeight() const noexcept {
  return static_cast<int32_t>(std::lround(bm::kRowHeight * (tree() ? tree()->scale() : 1.0f)));
}

ISize List::contentExtent(ISize viewport) const {
  const int32_t h = rowHeight() * static_cast<int32_t>(panel_.rows().size());
  return {viewport.w, std::max(viewport.h, h)};
}

void List::arrange(LayoutPass& pass) {
  if (rename_ && renameRow_ >= 0) pass.place(*rename_, rowRect(renameRow_));
}

int32_t List::rowAt(Vec2 p) const noexcept {
  const IRect r = rect();
  if (!r.contains(p)) return -1;
  const auto row = static_cast<int32_t>((p.y - static_cast<float>(r.y)) / static_cast<float>(rowHeight()));
  return row < static_cast<int32_t>(panel_.rows().size()) ? row : -1;
}

IRect List::rowRect(int32_t row) const noexcept {
  const IRect r = rect();
  return {r.x, r.y + row * rowHeight(), r.w, rowHeight()};
}

void List::reveal(int32_t row) {
  auto* sv = dynamic_cast<ScrollView*>(parent());
  if (!sv) return;
  const int32_t h = rowHeight();
  const int32_t top = row * h, view = sv->rect().h;
  if (top < sv->scrollY()) sv->scrollTo(0, top);
  else if (top + h > sv->scrollY() + view) sv->scrollTo(0, top + h - view);
}

void List::paint(PaintContext& ctx) {
  const auto t0 = std::chrono::steady_clock::now();
  const Theme* th = ctx.res.theme;
  const ThemeFonts* f = ctx.res.uiFonts;
  if (!th) return;
  const IRect r = rect();
  const IRect c = intersect(r, ctx.clip);
  if (c.empty()) return;
  ctx.dl.addRectFilled(c.toRect(), th->surface.panel);
  const auto rows = panel_.rows();
  const int32_t h = rowHeight();
  const int32_t first = std::max(0, (c.y - r.y) / h);
  const int32_t last = std::min(static_cast<int32_t>(rows.size()), (c.bottom() - r.y + h - 1) / h);
  int32_t x0[5], x1[5];
  ColumnHeader::columns(r, ctx.scale.factor, x0, x1);
  const int32_t pad = ctx.scale.px(6.0f), indent = ctx.scale.px(bm::kIndent);
  const int32_t sel = panel_.selected();
  const bool focus = tree() && (tree()->focus() == this || (tree()->focus() && panel_.isAncestorOf(*tree()->focus())));
  uint32_t painted = 0;
  for (int32_t i = first; i < last; ++i) {
    const BrowserRow& row = rows[static_cast<size_t>(i)];
    const IRect rr{r.x, r.y + i * h, r.w, h};
    const bool isSel = i == sel;
    if (isSel) ctx.dl.addRectFilled(rr.toRect(), focus ? th->selection.fill : withAlpha(th->selection.fill, 110));
    ++painted;
    if (!f) continue;
    const Rgba tc = isSel ? th->selection.text : th->text.primary;
    const int32_t ind = pad + row.depth * indent;
    if (row.dir || row.device) {  // fold triangle + folder (T5.11: or device) name
      const bool open = panel_.rowOpen(row);
      const float cx = static_cast<float>(rr.x + ind + ctx.scale.px(4.0f)), cy = static_cast<float>(rr.y + h / 2);
      const float a = 3.5f * ctx.scale.factor;
      if (open) ctx.dl.addTriangleFilled({cx - a, cy - a * 0.6f}, {cx + a, cy - a * 0.6f}, {cx, cy + a * 0.8f}, th->text.secondary);
      else ctx.dl.addTriangleFilled({cx - a * 0.6f, cy - a}, {cx - a * 0.6f, cy + a}, {cx + a * 0.8f, cy}, th->text.secondary);
      paint::text(ctx, f->small, {x0[0], rr.y, x1[4] - x0[0], h}, panel_.cellText(row, BrowserColumn::Name), tc,
                  TextAlign::Left, ind + ctx.scale.px(12.0f), pad);
      continue;
    }
    paint::text(ctx, f->small, {x0[0], rr.y, x1[0] - x0[0], h}, panel_.cellText(row, BrowserColumn::Name), tc,
                TextAlign::Left, ind + ctx.scale.px(12.0f), pad);
    const Rgba sc = isSel ? th->selection.text : th->text.secondary;
    for (int k = 1; k < 5; ++k)
      paint::text(ctx, f->small, {x0[k], rr.y, x1[k] - x0[k], h}, panel_.cellText(row, static_cast<BrowserColumn>(k)), sc,
                  TextAlign::Right, 2, 2);
  }
  auto& st = panel_.statsMut();
  st.rowsPainted = painted;
  st.lastPaintMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool List::onPointer(const PointerEvent& e) {
  switch (e.type) {
    case PointerType::Down: {
      const int32_t row = rowAt(e.pos);
      if (tree()) tree()->setFocus(this);
      if (e.button == PointerButton::Right) {
        if (row >= 0) panel_.openContextMenu(row, e.pos);
        return true;
      }
      if (e.button != PointerButton::Left) return false;
      if (row < 0) return true;
      const BrowserRow br = panel_.rows()[static_cast<size_t>(row)];
      if (br.dir || br.device) {
        // The triangle (or a double-click on a folder) folds; a click elsewhere selects. T5.11: a device row folds its
        // presets with the triangle; a double-click on it loads the device.
        const int32_t ind = static_cast<int32_t>(std::lround((6.0f + bm::kIndent * br.depth) * (tree() ? tree()->scale() : 1.0f)));
        const bool onTriangle = e.pos.x < static_cast<float>(rect().x + ind) + 14.0f * (tree() ? tree()->scale() : 1.0f);
        if (onTriangle || (br.dir && e.clickCount >= 2)) {
          panel_.select(row, false);
          panel_.setRowOpen(br, !panel_.rowOpen(br));
          return true;
        }
      }
      if (e.clickCount >= 2) {
        panel_.select(row, false);
        (void)panel_.loadSelection();
        return true;
      }
      panel_.select(row);
      pressed_ = row;
      pressAt_ = e.pos;
      dragStarted_ = false;
      return true;
    }
    case PointerType::Move:
      if (pressed_ >= 0 && !dragStarted_ && tree()) {
        const float d = std::fabs(e.pos.x - pressAt_.x) + std::fabs(e.pos.y - pressAt_.y);
        if (d > 5.0f * tree()->scale()) {
          dragStarted_ = true;
          (void)tree()->beginDrag(panel_.selectionPayload());  // the browser item travels as a T5.7 payload
        }
      }
      return true;
    case PointerType::Up:
      pressed_ = -1;
      return true;
    default: return false;
  }
}

void List::beginTagEdit(int32_t row) {
  if (rename_ || row < 0) return;
  renameRow_ = row;
  tagEdit_ = true;
  rename_ = &add<TextEdit>("tag", "");
  rename_->setPlaceholder("New tag");
  rename_->onCommit = [this](const std::string&) { endRename(true); };
  rename_->onCancel = [this] { endRename(false); };
  invalidateArrange();
  if (tree()) {
    tree()->layoutNow();
    tree()->setFocus(rename_, true);
  }
}

void List::beginRename(int32_t row) {
  if (rename_ || row < 0) return;
  renameRow_ = row;
  tagEdit_ = false;
  const BrowserRow br = panel_.rows()[static_cast<size_t>(row)];
  std::string name = panel_.cellText(br, BrowserColumn::Name);
  rename_ = &add<TextEdit>("rename", name);
  rename_->onCommit = [this](const std::string&) { endRename(true); };
  rename_->onCancel = [this] { endRename(false); };
  invalidateArrange();
  if (tree()) {
    tree()->layoutNow();
    tree()->setFocus(rename_, true);
  }
  // The stem selected, as Explorer does.
  const size_t dot = name.find_last_of('.');
  rename_->buffer().setCaret(0, false);
  rename_->buffer().setCaret(static_cast<uint32_t>(dot == std::string::npos ? name.size() : dot), true);
}

void List::endRename(bool commit) {
  if (!rename_) return;
  const std::string text = rename_->text();
  const int32_t row = renameRow_;
  TextEdit* field = rename_;
  rename_ = nullptr;
  renameRow_ = -1;
  if (tagEdit_ && commit && row >= 0 && static_cast<size_t>(row) < panel_.rows().size() && panel_.index()) {
    // T5.11: a free tag for the row's file (trimmed; empty: nothing).
    const BrowserRow br = panel_.rows()[static_cast<size_t>(row)];
    std::string tag = text;
    while (!tag.empty() && (tag.back() == ' ' || tag.back() == '#')) tag.pop_back();
    while (!tag.empty() && (tag.front() == ' ' || tag.front() == '#')) tag.erase(tag.begin());
    if (!tag.empty() && !br.dir && !br.device) {
      Library& lib = panel_.context().library();
      lib.tags().addTag(library::tagKeyOf(*panel_.index(), br.id), tag);
      lib.tagsChanged();
      panel_.context().setStatus("Tagged \"" + tag + "\"");
    }
    commit = false;
  }
  tagEdit_ = false;
  if (commit && row >= 0 && static_cast<size_t>(row) < panel_.rows().size() && panel_.index()) {
    const BrowserRow br = panel_.rows()[static_cast<size_t>(row)];
    const std::string from = panel_.index()->path(br.id);
    namespace fs = std::filesystem;
    const fs::path src(std::u8string(from.begin(), from.end()));
    const fs::path dst = src.parent_path() / fs::path(std::u8string(text.begin(), text.end()));
    std::error_code ec;
    if (!text.empty() && dst != src) {
      if (fs::exists(dst, ec)) panel_.context().setStatus("Rename: a file named \"" + text + "\" exists already");
      else {
        fs::rename(src, dst, ec);
        panel_.context().setStatus(ec ? "Rename failed: " + ec.message() : "Renamed to \"" + text + "\"");
      }
    }
  }
  // The field is inside its own callback now: it is removed on the next timer, not here.
  field->setVisible(false);
  doomed_ = field;
  (void)requestTimer(0.0);
  if (tree()) tree()->setFocus(this);
  markDirty();
}

void List::onTimer(uint32_t) {
  if (!doomed_) return;
  Widget* w = doomed_;
  doomed_ = nullptr;
  (void)removeChild(*w);
}

// --- Waveform + PreviewStrip -------------------------------------------------------------------------------------------

Waveform::Waveform(BrowserPanel& panel) : Control("waveform"), panel_(panel) {
  setInfo("Preview", "The selected sample. Click to play it from there (scrub); the line follows the preview.");
  setCursor(Cursor::Pointer);
}

void Waveform::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  paint::box(ctx.dl, r, 2.0f * ctx.scale.factor, th->surface.sunken, 0);
  const media::WaveformOverview* ov = panel_.previewOverview();
  if (ov && ov->frames() > 0) {
    WaveformLevel lv[kMaxOverviewLevels];
    const WaveformData data = waveformData(*ov, nullptr, lv);
    WaveformDraw d;
    d.frame0 = 0.0;
    d.frame1 = static_cast<double>(ov->frames());
    const float in = 2.0f * ctx.scale.factor;
    d.rect = {static_cast<float>(r.x) + in, static_cast<float>(r.y) + in, static_cast<float>(r.right()) - in,
              static_cast<float>(r.bottom()) - in};
    d.colour = withAlpha(th->text.secondary, 200);
    ctx.dl.pushClip(r.toRect());
    (void)drawWaveform(ctx.dl, data, d);
    ctx.dl.popClip();
  } else if (const ThemeFonts* f = ctx.res.uiFonts) {
    paint::text(ctx, f->small, r, panel_.selected() >= 0 ? "" : "Select a sample to preview it", th->text.disabled,
                TextAlign::Center, 4, 4);
  }
  const double len = panel_.previewLength();
  const double pos = panel_.previewPosition();
  if (len > 0 && pos >= 0) {
    const float x = static_cast<float>(r.x) + static_cast<float>(pos / len) * static_cast<float>(r.w);
    const float w = static_cast<float>(std::max(1, ctx.scale.px(1.0f)));
    ctx.dl.addRectFilled({x, static_cast<float>(r.y), x + w, static_cast<float>(r.bottom())}, th->accent.primary);
  }
}

bool Waveform::onPointer(const PointerEvent& e) {
  if (e.type != PointerType::Down || e.button != PointerButton::Left) return false;
  const double len = panel_.previewLength();
  const IRect r = rect();
  if (len <= 0 || r.w <= 0) return true;
  const double at = std::clamp(static_cast<double>(e.pos.x - static_cast<float>(r.x)) / r.w, 0.0, 1.0) * len;
  panel_.scrubTo(at);
  return true;
}

PreviewStrip::PreviewStrip(BrowserPanel& panel) : Widget("preview"), panel_(panel) {
  setLayout(LayoutKind::Row);
  setPadding({6, 4, 6, 6});
  setGap(4);
  setAlign(Align::Start, Align::Center);
  auto& left = add<Widget>("buttons");
  left.setLayout(LayoutKind::Column);
  left.setWidth(Length::fixed(44));
  left.setGap(2);
  preview_ = &left.add<Toggle>("on", "Prev", ToggleStyle::Accent, panel_.previewOn());
  preview_->setHeight(Length::fixed(16));
  preview_->setFocusable(false);
  preview_->setInfo("Preview", "Plays the selected sample (Raw: at its own speed; Sync: re-pitched to the project tempo "
                               "when its BPM is known). The cue output is chosen in Preferences (Cue Out).");
  preview_->onToggle = [this](bool on) { panel_.setPreviewOn(on); };
  sync_ = &left.add<Toggle>("sync", "Sync", ToggleStyle::Accent, panel_.sync());
  sync_->setHeight(Length::fixed(16));
  sync_->setFocusable(false);
  sync_->setInfo("Raw / Sync", "Off (Raw): the file at its own speed. On (Sync): re-pitched to the project tempo when "
                               "the file's tempo is known (stretching without a pitch change comes with M6).");
  sync_->onToggle = [this](bool on) { panel_.setSync(on); };
  wave_ = &add<Waveform>(panel_);
  wave_->setWidth(Length::flex(1));
  wave_->setHeight(Length::flex(1));
  ValueSpec vs;
  vs.min = -70.0;
  vs.max = 6.0;
  vs.def = -6.0;
  vs.step = 0.1;
  vs.decimals = 1;
  vs.unit = "dB";
  volume_ = &add<DragBox>("volume", vs);
  volume_->setWidth(Length::fixed(56));
  volume_->setHeight(Length::fixed(18));
  volume_->setValue(-6.0);
  volume_->setInfo("Preview Volume", "The level of the browser's preview (drag, or double-click to type).");
  volume_->onChange = [this](double db) {
    if (PreviewPort* p = panel_.context().library().preview()) p->setVolumeDb(static_cast<float>(db));
  };
}

void PreviewStrip::sync() {
  preview_->setOn(panel_.previewOn());
  sync_->setOn(panel_.sync());
  markDirty();
  wave_->markDirty();
}

void PreviewStrip::paint(PaintContext& ctx) {
  if (const Theme* th = ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), th->surface.raised);
}

}  // namespace pq::views::browser

namespace pq::views {

std::string filterChipLabel(uint32_t chip, uint8_t state) {
  static constexpr const char* kDur[5] = {"Dur: Any", "< 1 s", "1\xE2\x80\x93" "10 s", "10\xE2\x80\x93" "60 s", "> 60 s"};
  static constexpr const char* kBpm[4] = {"BPM: Any", "< 100", "100\xE2\x80\x93" "130", "> 130"};
  static constexpr const char* kRate[5] = {"Rate: Any", "44.1k", "48k", "88.2/96k", "> 96k"};
  static constexpr const char* kCh[3] = {"Ch: Any", "Mono", "Stereo"};
  switch (chip) {
    case 0: return kDur[state < 5 ? state : 0];
    case 1: return kBpm[state < 4 ? state : 0];
    case 2: return kRate[state < 5 ? state : 0];
    case 3: return kCh[state < 3 ? state : 0];
    default: return {};
  }
}

}  // namespace pq::views
