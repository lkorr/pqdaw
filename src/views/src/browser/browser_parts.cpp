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
    if (row.dir) {  // fold triangle + folder name
      const bool open = panel_.folderOpen(row.id);
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
      if (br.dir) {
        // The triangle (or a double-click) folds; a click elsewhere selects.
        const int32_t ind = static_cast<int32_t>(std::lround((6.0f + bm::kIndent * br.depth) * (tree() ? tree()->scale() : 1.0f)));
        const bool onTriangle = e.pos.x < static_cast<float>(rect().x + ind) + 14.0f * (tree() ? tree()->scale() : 1.0f);
        if (onTriangle || e.clickCount >= 2) {
          panel_.select(row, false);
          panel_.setFolderOpen(br.id, !panel_.folderOpen(br.id));
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

void List::beginRename(int32_t row) {
  if (rename_ || row < 0) return;
  renameRow_ = row;
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
