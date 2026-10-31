// The bug report panel (T5.x-bugreport): pq/views/bug_report/bug_panel.h describes it.
#include "pq/views/bug_report/bug_panel.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "../common/paint.h"
#include "pq/ui/code_editor.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/tile_atlas.h"

namespace pq::views::bugreport {

using namespace pq::ui;
namespace fs = std::filesystem;

namespace {

constexpr float kThumbW = 160.0f, kThumbH = 90.0f;      // logical px: the shot strip
constexpr float kListThumbW = 88.0f, kListThumbH = 50.0f;  // the ticket list
constexpr float kRowH = 58.0f;

// ---- images through the tile atlas ------------------------------------------------------------------------------

// An RGB image drawn from ONE coverage tile holding its three channel planes side by side (two zero texels between
// them): a black rect, then each plane added with BlendMode::Additive in its primary colour gives back the colour
// (dst = coverage × channel + dst). The atlas has no colour textures; this needs none and costs four quads.
struct TileImage {
  TileAtlas::Tile tile;
  const uint8_t* data = nullptr;  // the image this tile was written from (pixels' address + size)
  uint32_t w = 0, h = 0;
  bool failed = false;
};

void drawImage(PaintContext& ctx, TileImage& cache, const Image& img, const IRect& dst) {
  if (img.width == 0 || dst.empty()) return;
  ctx.dl.addRectFilled(dst.toRect(), rgba(0, 0, 0));
  TileAtlas* tiles = ctx.res.tiles;
  const uint32_t w = img.width, h = img.height, planeW = w + 2;
  const bool same = cache.data == img.rgba.data() && cache.w == w && cache.h == h;
  if (tiles && !(same && cache.failed) && !(same && tiles->valid(cache.tile)) && 3 * planeW <= tiles->maxTileSize() &&
      h <= tiles->maxTileSize()) {
    cache = {};
    cache.data = img.rgba.data();
    cache.w = w;
    cache.h = h;
    if (!tiles->allocate(3 * planeW, h, cache.tile)) {
      cache.failed = true;
    } else if (uint8_t* t = tiles->write(cache.tile)) {
      const uint32_t pitch = TileAtlas::rowPitch(cache.tile);
      for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t y = 0; y < h; ++y) {
          uint8_t* row = t + size_t(y + 1) * pitch + 1 + c * planeW;
          const uint8_t* s = img.pixel(0, y) + c;
          for (uint32_t x = 0; x < w; ++x, s += 4) row[x] = *s;
        }
    }
  }
  if (tiles && same && !cache.failed && tiles->valid(cache.tile)) {
    tiles->touch(cache.tile.page);
    const float inv = tiles->uvScale();
    const BlendMode before = ctx.dl.blend();
    ctx.dl.setBlend(BlendMode::Additive);
    const Rgba planes[3] = {rgba(255, 0, 0), rgba(0, 255, 0), rgba(0, 0, 255)};
    for (uint32_t c = 0; c < 3; ++c) {
      const float u0 = static_cast<float>(cache.tile.x + c * planeW) * inv;
      const Vec2 uv0{u0, static_cast<float>(cache.tile.y) * inv};
      const Vec2 uv1{u0 + static_cast<float>(w) * inv, static_cast<float>(cache.tile.y + h) * inv};
      ctx.dl.addImage(TileAtlas::texture(cache.tile.page), dst.toRect(), uv0, uv1, planes[c]);
    }
    ctx.dl.setBlend(before);
    return;
  }
  // No atlas (CPU-only tests) or no room: a mosaic of about 8 device px cells.
  const int32_t cell = std::max(4, ctx.scale.px(8.0f));
  for (int32_t y = 0; y < dst.h; y += cell)
    for (int32_t x = 0; x < dst.w; x += cell) {
      const uint32_t sx = std::min(w - 1, static_cast<uint32_t>((x + cell / 2) * static_cast<int64_t>(w) / dst.w));
      const uint32_t sy = std::min(h - 1, static_cast<uint32_t>((y + cell / 2) * static_cast<int64_t>(h) / dst.h));
      const uint8_t* p = img.pixel(sx, sy);
      ctx.dl.addRectFilled({static_cast<float>(dst.x + x), static_cast<float>(dst.y + y), static_cast<float>(std::min(dst.x + x + cell, dst.right())),
                            static_cast<float>(std::min(dst.y + y + cell, dst.bottom()))},
                           rgba(p[0], p[1], p[2]));
    }
}

// The fitted rect of a w×h picture inside `box` (centred, aspect kept).
IRect fit(const IRect& box, uint32_t w, uint32_t h) {
  if (!w || !h || box.empty()) return box;
  const double k = std::min(static_cast<double>(box.w) / w, static_cast<double>(box.h) / h);
  const auto fw = static_cast<int32_t>(std::lround(w * k)), fh = static_cast<int32_t>(std::lround(h * k));
  return {box.x + (box.w - fw) / 2, box.y + (box.h - fh) / 2, fw, fh};
}

std::string sizeText(size_t bytes) {
  char b[32];
  if (bytes < 1024) std::snprintf(b, sizeof b, "%zu B", bytes);
  else if (bytes < (1u << 20)) std::snprintf(b, sizeof b, "%.0f KB", static_cast<double>(bytes) / 1024.0);
  else std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return b;
}

bool hasTag(const std::vector<std::string>& v, std::string_view t) { return std::find(v.begin(), v.end(), t) != v.end(); }

// A description box: the code editor without line numbers, and a placeholder while empty.
class DescriptionBox final : public CodeEditor {
public:
  DescriptionBox() : CodeEditor("description") { setLineNumbers(false); }
  void paintOver(PaintContext& ctx) override {
    if (!text().empty() || !ctx.res.uiFonts || !ctx.res.theme) return;
    const IRect r = rect();
    const int32_t pad = ctx.scale.px(6.0f);
    paint::text(ctx, ctx.res.uiFonts->body, {r.x + pad, r.y + pad / 2, r.w - 2 * pad, ctx.scale.px(18.0f)},
                "What is wrong, or what you want instead. Ctrl+Enter submits, Esc closes (the draft stays).",
                ctx.res.theme->text.disabled, TextAlign::Left);
  }
};

}  // namespace

// ---- the loader thread ------------------------------------------------------------------------------------------

struct BugReportPanel::Loader {
  explicit Loader(PanelHooks& h, std::shared_ptr<BugReportPanel*> alive, uint32_t w, uint32_t hgt)
      : hooks(h), alive(std::move(alive)), maxW(w), maxH(hgt) {}
  ~Loader() {
    {
      std::lock_guard lk(m);
      stop = true;
    }
    cv.notify_all();
    if (thread.joinable()) thread.join();
  }
  struct Entry {
    Image img;
    bool done = false, failed = false;
  };
  PanelHooks& hooks;
  std::shared_ptr<BugReportPanel*> alive;
  uint32_t maxW, maxH;
  std::mutex m;
  std::condition_variable cv;
  std::map<std::string, Entry> cache;  // by path (UTF-8)
  std::deque<fs::path> queue;
  std::thread thread;
  bool stop = false;

  Result<Image> load(const fs::path& p) {
    if (hooks.thumbnail) return hooks.thumbnail(p, maxW, maxH);
    return loadThumbnail(p, maxW, maxH);
  }
  // UI thread. Null while pending.
  const Image* get(const fs::path& p) {
    const std::u8string u = p.u8string();
    const std::string key(u.begin(), u.end());
    std::unique_lock lk(m);
    if (auto it = cache.find(key); it != cache.end()) return it->second.done && !it->second.failed ? &it->second.img : nullptr;
    cache[key];
    if (!hooks.post) {  // no poster: synchronously (tests, scenes)
      lk.unlock();
      auto img = load(p);
      lk.lock();
      Entry& e = cache[key];
      e.done = true;
      e.failed = !img;
      if (img) e.img = std::move(*img);
      return e.failed ? nullptr : &e.img;
    }
    queue.push_back(p);
    if (!thread.joinable()) thread = std::thread([this] { run(); });
    cv.notify_one();
    return nullptr;
  }
  void run() {
    for (;;) {
      fs::path p;
      {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return stop || !queue.empty(); });
        if (stop) return;
        p = std::move(queue.front());
        queue.pop_front();
      }
      auto img = load(p);
      const std::u8string u = p.u8string();
      const std::string key(u.begin(), u.end());
      {
        std::lock_guard lk(m);
        Entry& e = cache[key];
        e.done = true;
        e.failed = !img;
        if (img) e.img = std::move(*img);
      }
      // One repaint per decoded picture (each decode is ~10-20 ms, so these are few).
      std::weak_ptr<BugReportPanel*> w = alive;
      hooks.post([w] {
        if (auto a = w.lock(); a && *a) (*a)->thumbsArrived();
      });
    }
  }
};

// ---- ticket list ------------------------------------------------------------------------------------------------

class TicketList final : public Widget {
public:
  explicit TicketList(BugReportPanel& p) : Widget("tickets"), panel_(p) { setCursor(Cursor::Pointer); }
  int32_t scroll = 0;
  int hovered = -2;
  std::vector<TileImage> tiles;

  int rowAt(Vec2 pos) const {
    const int32_t rh = tree() ? tree()->uiScale().px(kRowH) : static_cast<int32_t>(kRowH);
    const int32_t y = static_cast<int32_t>(pos.y) - rect().y + scroll;
    if (y < 0 || !rect().contains(pos)) return -2;
    const int row = y / rh;
    return row <= static_cast<int>(panel_.shownTickets_.size()) ? row - 1 : -2;
  }
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th || !ctx.res.uiFonts) return;
    const IRect r = rect();
    paint::box(ctx.dl, r, 0.0f, th->surface.sunken, th->surface.divider);
    ctx.dl.pushClip(r.toRect());
    const int32_t rh = ctx.scale.px(kRowH), pad = ctx.scale.px(5.0f);
    const auto& list = panel_.shownTickets_;
    tiles.resize(list.size());
    for (int i = -1; i < static_cast<int>(list.size()); ++i) {
      const IRect row{r.x, r.y + (i + 1) * rh - scroll, r.w, rh};
      if (row.bottom() < ctx.clip.y || row.y > ctx.clip.bottom()) continue;
      const bool sel = panel_.selected_ == i;
      if (sel) ctx.dl.addRectFilled(row.toRect(), th->selection.fill);
      else if (hovered == i) ctx.dl.addRectFilled(row.toRect(), paint::hoverOf(th->surface.sunken));
      ctx.dl.addRectFilled({static_cast<float>(row.x), static_cast<float>(row.bottom() - 1), static_cast<float>(row.right()),
                            static_cast<float>(row.bottom())},
                           th->surface.divider);
      const IRect thumb{row.x + pad, row.y + (rh - ctx.scale.px(kListThumbH)) / 2, ctx.scale.px(kListThumbW), ctx.scale.px(kListThumbH)};
      const int32_t tx = thumb.right() + pad * 2;
      const IRect line1{tx, row.y + pad, row.right() - tx - pad, ctx.scale.px(20.0f)};
      const IRect line2{tx, line1.bottom(), line1.w, ctx.scale.px(18.0f)};
      const Rgba primary = sel ? th->selection.text : th->text.primary, secondary = sel ? th->selection.text : th->text.secondary;
      if (i < 0) {
        paint::box(ctx.dl, thumb, ctx.scale.exact(3.0f), th->control.fill, th->control.border);
        paint::text(ctx, ctx.res.uiFonts->title, thumb, "+", primary, TextAlign::Center);
        paint::text(ctx, ctx.res.uiFonts->bodyStrong, line1, panel_.suggestions() ? "New suggestion" : "New bug report", primary, TextAlign::Left);
        paint::text(ctx, ctx.res.uiFonts->small, line2, "the shots taken now, in a new folder", secondary, TextAlign::Left);
        continue;
      }
      const Ticket& t = list[static_cast<size_t>(i)];
      const Image* img = t.shots.empty() ? nullptr : panel_.ticketThumb(t.shots.front());
      if (img) drawImage(ctx, tiles[static_cast<size_t>(i)], *img, fit(thumb, img->width, img->height));
      else paint::box(ctx.dl, thumb, 0.0f, th->surface.panel, th->surface.divider);
      paint::text(ctx, ctx.res.uiFonts->body, line1, t.firstLine.empty() ? std::string("(no description)") : t.firstLine, primary,
                  TextAlign::Left);
      std::string meta = t.id + " \xC2\xB7 " + std::to_string(t.shots.size()) + (t.shots.size() == 1 ? " shot" : " shots");
      for (const std::string& tag : t.tags) meta += "  #" + tag;
      paint::text(ctx, ctx.res.uiFonts->small, line2, meta, secondary, TextAlign::Left);
    }
    ctx.dl.popClip();
  }
  bool onPointer(const PointerEvent& e) override {
    if (e.type == PointerType::Move) {
      const int h = rowAt(e.pos);
      if (h != hovered) {
        hovered = h;
        markDirty();
      }
      return false;
    }
    if (e.type == PointerType::Leave) {
      hovered = -2;
      markDirty();
      return false;
    }
    if (e.type == PointerType::Wheel) {
      const int32_t rh = tree()->uiScale().px(kRowH);
      const int32_t content = rh * static_cast<int32_t>(panel_.shownTickets_.size() + 1);
      scroll = std::clamp(scroll - static_cast<int32_t>(e.wheel.y * static_cast<float>(rh)), 0, std::max(0, content - rect().h));
      markDirty();
      return true;
    }
    if (e.type == PointerType::Down && e.button == PointerButton::Left) {
      const int row = rowAt(e.pos);
      if (row >= -1) panel_.selectTicket(row);
      return true;
    }
    return e.type == PointerType::Up;
  }

private:
  BugReportPanel& panel_;
};

// ---- the shot strip ---------------------------------------------------------------------------------------------

class ShotStrip final : public Widget {
public:
  explicit ShotStrip(BugReportPanel& p) : Widget("shots"), panel_(p) {}
  int32_t scroll = 0;
  std::vector<TileImage> tiles, ticketTiles;

  // What the strip shows: a picked ticket's shots (old, dimmed), then the draft's.
  struct Item {
    const Image* img;
    bool old;
    size_t index;  // into the draft's shots (new) or the ticket's (old)
  };
  std::vector<Item> items() {
    std::vector<Item> out;
    if (panel_.selected_ >= 0 && static_cast<size_t>(panel_.selected_) < panel_.shownTickets_.size()) {
      const Ticket& t = panel_.shownTickets_[static_cast<size_t>(panel_.selected_)];
      for (size_t i = 0; i < t.shots.size() && i < 8; ++i) out.push_back({panel_.ticketThumb(t.shots[i]), true, i});
    }
    for (size_t i = 0; i < panel_.draft_.shots.size(); ++i) out.push_back({&panel_.draft_.shots[i].thumb, false, i});
    return out;
  }
  IRect slot(size_t i) const {
    const UiScale s = tree()->uiScale();
    const int32_t w = s.px(kThumbW), gap = s.px(8.0f);
    return {rect().x + static_cast<int32_t>(i) * (w + gap) - scroll, rect().y, w, s.px(kThumbH)};
  }
  IRect removeBox(const IRect& slot) const {
    const int32_t b = tree()->uiScale().px(18.0f), m = tree()->uiScale().px(3.0f);
    return {slot.right() - b - m, slot.y + m, b, b};
  }
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th || !ctx.res.uiFonts) return;
    const IRect r = rect();
    ctx.dl.pushClip(r.toRect());
    const auto list = items();
    tiles.resize(panel_.draft_.shots.size());
    ticketTiles.resize(8);
    if (list.empty()) {
      paint::box(ctx.dl, {r.x, r.y, ctx.scale.px(kThumbW), ctx.scale.px(kThumbH)}, ctx.scale.exact(3.0f), th->surface.sunken, th->surface.divider);
      paint::text(ctx, ctx.res.uiFonts->small, {r.x + ctx.scale.px(kThumbW + 12.0f), r.y, r.w, ctx.scale.px(kThumbH)},
                  panel_.suggestions() ? "No picture: a suggestion may be words only (Add another, then F12, to show something)."
                                       : "No picture yet: Add another, then press F12 in the window.",
                  th->text.secondary, TextAlign::Left);
    }
    int newNo = 0;
    for (size_t i = 0; i < list.size(); ++i) {
      const IRect s = slot(i);
      const IRect pic{s.x, s.y, s.w, s.h};
      paint::box(ctx.dl, pic, 0.0f, th->surface.sunken, 0);
      if (list[i].img && list[i].img->width) {
        TileImage& tc = list[i].old ? ticketTiles[list[i].index] : tiles[list[i].index];
        drawImage(ctx, tc, *list[i].img, fit(pic, list[i].img->width, list[i].img->height));
      }
      if (list[i].old) ctx.dl.addRectFilled(pic.toRect(), rgba(0, 0, 0, 110));
      ctx.dl.addRectStroke(pic.toRect(), list[i].old ? th->surface.divider : th->accent.focus);
      const IRect label{s.x, s.bottom() + ctx.scale.px(2.0f), s.w, ctx.scale.px(16.0f)};
      const std::string text = list[i].old ? "on the ticket: shot " + std::to_string(list[i].index + 1)
                                            : "new shot " + std::to_string(++newNo);
      paint::text(ctx, ctx.res.uiFonts->small, label, text, list[i].old ? th->text.disabled : th->text.secondary, TextAlign::Left);
      if (!list[i].old) {
        const IRect x = removeBox(s);
        paint::box(ctx.dl, x, ctx.scale.exact(3.0f), hoverX_ == static_cast<int>(list[i].index) ? th->accent.arm : rgba(0, 0, 0, 170), 0);
        paint::text(ctx, ctx.res.uiFonts->bodyStrong, x, "\xC3\x97", rgba(255, 255, 255), TextAlign::Center);
      }
    }
    ctx.dl.popClip();
  }
  bool onPointer(const PointerEvent& e) override {
    const auto list = items();
    if (e.type == PointerType::Move || e.type == PointerType::Down) {
      int over = -1;
      for (size_t i = 0; i < list.size(); ++i)
        if (!list[i].old && removeBox(slot(i)).contains(e.pos)) over = static_cast<int>(list[i].index);
      if (over != hoverX_) {
        hoverX_ = over;
        setCursor(over >= 0 ? Cursor::Pointer : Cursor::Inherit);
        markDirty();
      }
      if (e.type == PointerType::Down && e.button == PointerButton::Left && over >= 0) {
        hoverX_ = -1;
        panel_.removeShot(static_cast<size_t>(over));
        return true;
      }
      return e.type == PointerType::Down;
    }
    if (e.type == PointerType::Wheel) {
      const UiScale s = tree()->uiScale();
      const int32_t content = static_cast<int32_t>(list.size()) * (s.px(kThumbW) + s.px(8.0f));
      scroll = std::clamp(scroll - static_cast<int32_t>((e.wheel.y + e.wheel.x) * static_cast<float>(s.px(kThumbW) / 2)), 0,
                          std::max(0, content - rect().w));
      markDirty();
      return true;
    }
    return e.type == PointerType::Up;
  }

private:
  BugReportPanel& panel_;
  int hoverX_ = -1;
};

// ---- tag chips --------------------------------------------------------------------------------------------------

class TagChips final : public Widget {
public:
  explicit TagChips(BugReportPanel& p) : Widget("tags"), panel_(p) { setCursor(Cursor::Pointer); }
  struct Chip {
    IRect r;
    std::string tag;
  };
  std::vector<Chip> chips;  // from the last paint
  int hovered = -1;

  void layoutChips(const PaintResources& res, UiScale s) {
    chips.clear();
    if (!res.text || !res.uiFonts) return;
    const IRect r = rect();
    const int32_t h = s.px(20.0f), gap = s.px(5.0f), padX = s.px(9.0f);
    int32_t x = r.x, y = r.y;
    for (const std::string& t : panel_.knownTags_) {
      const int32_t w = res.text->measure(res.uiFonts->small, t) + 2 * padX;
      if (x > r.x && x + w > r.right()) {
        x = r.x;
        y += h + gap;
      }
      chips.push_back({{x, y, w, h}, t});
      x += w + gap;
    }
  }
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th || !ctx.res.uiFonts) return;
    layoutChips(ctx.res, ctx.scale);
    ctx.dl.pushClip(rect().toRect());
    for (size_t i = 0; i < chips.size(); ++i) {
      const bool on = hasTag(panel_.draft_.tags, chips[i].tag);
      const float rad = static_cast<float>(chips[i].r.h) * 0.5f;
      Rgba fill = on ? th->accent.primary : th->control.fill;
      if (static_cast<int>(i) == hovered) fill = paint::hoverOf(fill);
      paint::box(ctx.dl, chips[i].r, rad, fill, on ? 0 : th->control.border);
      paint::text(ctx, ctx.res.uiFonts->small, chips[i].r, chips[i].tag, on ? th->text.onAccent : th->text.primary, TextAlign::Center);
    }
    ctx.dl.popClip();
  }
  int chipAt(Vec2 p) const {
    for (size_t i = 0; i < chips.size(); ++i)
      if (chips[i].r.contains(p)) return static_cast<int>(i);
    return -1;
  }
  bool onPointer(const PointerEvent& e) override {
    if (e.type == PointerType::Move || e.type == PointerType::Leave) {
      const int h = e.type == PointerType::Leave ? -1 : chipAt(e.pos);
      if (h != hovered) {
        hovered = h;
        markDirty();
      }
      return false;
    }
    if (e.type == PointerType::Down && e.button == PointerButton::Left) {
      if (const int c = chipAt(e.pos); c >= 0) panel_.toggleTag(chips[static_cast<size_t>(c)].tag);
      return true;
    }
    return e.type == PointerType::Up;
  }

private:
  BugReportPanel& panel_;
};

// ---- the panel --------------------------------------------------------------------------------------------------

BugReportPanel::BugReportPanel(Draft& draft, fs::path root, PanelHooks hooks)
    : Control("bug-report"), draft_(draft), root_(std::move(root)), hooks_(std::move(hooks)),
      alive_(std::make_shared<BugReportPanel*>(this)) {
  setLayout(LayoutKind::Custom);
  setFocusable(true);
  title_ = &add<Label>("title", "Bug report", FontRoleId::Title, TextTone::Primary);
  bugs_ = &add<Toggle>("bugs", "Bugs", ToggleStyle::Accent, !draft_.suggestion);
  bugs_->setInfo("Bugs", "File a bug: bug_reports/<time>/. The list shows the open bug tickets.");
  bugs_->onToggle = [this](bool) { setSuggestions(false); };
  suggest_ = &add<Toggle>("suggestions", "Suggestions", ToggleStyle::Accent, draft_.suggestion);
  suggest_->setInfo("Suggestions", "File an idea or a wanted change: bug_reports/suggestions/<time>/ (a picture is optional).");
  suggest_->onToggle = [this](bool) { setSuggestions(true); };
  close_ = &add<Button>("close", "Close");
  close_->setInfo("Close", "Hides the panel and keeps the draft (Esc). F12 or Shift+F12 brings it back.");
  close_->onClick = [this] {
    if (hooks_.close) hooks_.close();
  };
  filterChooser_ = &add<Dropdown>("filter", std::vector<std::string>{"All tags"}, 0);
  filterChooser_->setInfo("Tag filter", "Shows only the tickets with this tag.");
  filterChooser_->onChange = [this](int32_t i) {
    const auto& items = filterChooser_->items();
    setTagFilter(i <= 0 || static_cast<size_t>(i) >= items.size() ? std::string() : items[static_cast<size_t>(i)]);
  };
  list_ = &add<TicketList>(*this);
  target_ = &add<Label>("target", "", FontRoleId::BodyStrong, TextTone::Primary);
  strip_ = &add<ShotStrip>(*this);
  auto& desc = add<DescriptionBox>();
  description_ = &desc;
  desc.setText(draft_.text, static_cast<uint32_t>(draft_.text.size()));
  desc.onEdit = [this](uint32_t, uint32_t, std::string_view, bool) { draft_.text = description_->text(); };
  chips_ = &add<TagChips>(*this);
  tagEntry_ = &add<TextEdit>("new-tag");
  tagEntry_->setPlaceholder("new tag, Enter");
  tagEntry_->onEnter = [this] {
    if (addTag(tagEntry_->text())) tagEntry_->setText({});
  };
  tagEntry_->onCancel = [this] {
    if (hooks_.close) hooks_.close();
  };
  attach_ = &add<Toggle>("attach", "Attach project", ToggleStyle::Accent, draft_.attachProject);
  attach_->setInfo("Attach project", "Writes the document as it was at the first shot (unsaved edits included) as project.pqd "
                                     "in the report folder: no samples, caches or waveforms.");
  attach_->onToggle = [this](bool on) { draft_.attachProject = on; };
  messageLabel_ = &add<Label>("message", "", FontRoleId::Small, TextTone::Secondary);
  discard_ = &add<Button>("discard", "Discard");
  discard_->setInfo("Discard", "Drops this draft (its shots and text). Nothing on disk is touched.");
  discard_->onClick = [this] {
    if (hooks_.discard) hooks_.discard();
  };
  another_ = &add<Button>("add-another", "Add another");
  another_->setInfo("Add another", "Hides the panel; the next F12 adds a shot to this report (Shift+F12: back without one).");
  another_->onClick = [this] {
    if (hooks_.addAnother) hooks_.addAnother();
  };
  submit_ = &add<Button>("submit", "Submit");
  submit_->setInfo("Submit", "Writes the report (Ctrl+Enter).");
  submit_->onClick = [this] {
    if (hooks_.submit) hooks_.submit();
  };
  refresh();
  shotsChanged();
}

BugReportPanel::~BugReportPanel() {
  *alive_ = nullptr;
  loader_.reset();  // joins the loader thread (it finishes the decode in hand)
}

void BugReportPanel::refresh() {
  allTickets_ = hooks_.tickets ? hooks_.tickets(draft_.suggestion) : listTickets(root_, draft_.suggestion);
  knownTags_ = knownTags(root_, allTickets_);
  for (const std::string& t : draft_.tags)
    if (!hasTag(knownTags_, t)) knownTags_.push_back(t);
  std::vector<std::string> filterItems{"All tags"};
  int32_t fi = 0;
  for (const Ticket& t : allTickets_)
    for (const std::string& tag : t.tags)
      if (!hasTag(filterItems, tag)) filterItems.push_back(tag);
  for (size_t i = 1; i < filterItems.size(); ++i)
    if (filterItems[i] == filter_) fi = static_cast<int32_t>(i);
  if (fi == 0) filter_.clear();
  filterChooser_->setItems(filterItems, fi);
  // Keep the picked ticket picked (by folder) if it is still listed.
  const fs::path keep = draft_.target;
  shownTickets_.clear();
  for (const Ticket& t : allTickets_)
    if (filter_.empty() || hasTag(t.tags, filter_)) shownTickets_.push_back(t);
  selected_ = -1;
  for (size_t i = 0; i < shownTickets_.size(); ++i)
    if (!keep.empty() && shownTickets_[i].dir == keep) selected_ = static_cast<int>(i);
  if (!keep.empty() && selected_ < 0) {  // the ticket went away (moved to fixed/): back to a new report
    draft_.target.clear();
    draft_.text = newText_;
    draft_.tags = newTags_;
    description_->setText(draft_.text, static_cast<uint32_t>(draft_.text.size()));
  }
  if (list_) list_->scroll = 0;
  updateLabels();
  invalidateArrange();
  markDirty();
}

void BugReportPanel::selectTicket(int index) {
  if (index < -1 || index >= static_cast<int>(shownTickets_.size())) return;
  if (index == selected_) return;
  if (selected_ < 0) {  // leaving the new report: keep its text and tags for coming back
    newText_ = draft_.text;
    newTags_ = draft_.tags;
  }
  selected_ = index;
  if (index < 0) {
    draft_.target.clear();
    draft_.text = newText_;
    draft_.tags = newTags_;
  } else {
    const Ticket& t = shownTickets_[static_cast<size_t>(index)];
    draft_.target = t.dir;
    if (auto m = readTicket(t.dir)) {
      draft_.text = m->text == "(no description)" ? std::string() : m->text;
      draft_.tags = m->tags;
    } else {
      setMessage("Cannot read " + t.dir.string() + ": " + m.error().message);
    }
  }
  for (const std::string& tg : draft_.tags)
    if (!hasTag(knownTags_, tg)) knownTags_.push_back(tg);
  description_->setText(draft_.text, static_cast<uint32_t>(draft_.text.size()));
  updateLabels();
  markDirty();
}

void BugReportPanel::setSuggestions(bool on) {
  bugs_->setOn(!on);
  suggest_->setOn(on);
  if (on == draft_.suggestion) return;
  if (selected_ >= 0) selectTicket(-1);
  draft_.suggestion = on;
  filter_.clear();
  refresh();
}

void BugReportPanel::setTagFilter(std::string tag) {
  filter_ = std::move(tag);
  refresh();
}

void BugReportPanel::toggleTag(const std::string& tag) {
  auto it = std::find(draft_.tags.begin(), draft_.tags.end(), tag);
  if (it == draft_.tags.end()) draft_.tags.push_back(tag);
  else draft_.tags.erase(it);
  if (chips_) chips_->markDirty();
}

bool BugReportPanel::addTag(std::string_view typed) {
  const std::string t = normalizeTag(typed);
  if (t.empty()) {
    setMessage("A tag is lower-case letters, digits, '-' and '_'.");
    return false;
  }
  if (!hasTag(knownTags_, t)) knownTags_.push_back(t);
  if (!hasTag(draft_.tags, t)) draft_.tags.push_back(t);
  if (chips_) chips_->markDirty();
  return true;
}

void BugReportPanel::shotsChanged() {
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  const auto w = static_cast<uint32_t>(s.px(kThumbW)), h = static_cast<uint32_t>(s.px(kThumbH));
  for (Shot& shot : draft_.shots)
    if (shot.thumb.width == 0 || (shot.thumb.width != w && shot.thumb.height != h)) shot.thumb = downscale(shot.image, w, h);
  updateLabels();
  if (strip_) strip_->markDirty();
}

void BugReportPanel::removeShot(size_t index) {
  if (index >= draft_.shots.size()) return;
  draft_.shots.erase(draft_.shots.begin() + static_cast<std::ptrdiff_t>(index));
  if (strip_) strip_->tiles.clear();  // the cached tiles follow the images
  updateLabels();
  markDirty();
}

void BugReportPanel::setMessage(std::string text) {
  message_ = std::move(text);
  if (messageLabel_) messageLabel_->setText(message_);
}

void BugReportPanel::focusDescription() {
  if (tree()) tree()->setFocus(description_, true);
}

const Image* BugReportPanel::ticketThumb(const fs::path& png) {
  if (!loader_) {
    const UiScale s = tree() ? tree()->uiScale() : UiScale{};
    loader_ = std::make_unique<Loader>(hooks_, alive_, static_cast<uint32_t>(s.px(kThumbW)), static_cast<uint32_t>(s.px(kThumbH)));
  }
  return loader_->get(png);
}

void BugReportPanel::thumbsArrived() {
  if (list_) list_->markDirty();
  if (strip_) strip_->markDirty();
}

void BugReportPanel::updateLabels() {
  const size_t n = draft_.shots.size();
  std::string t;
  if (selected_ >= 0 && static_cast<size_t>(selected_) < shownTickets_.size()) {
    t = "Adding to " + shownTickets_[static_cast<size_t>(selected_)].id;
  } else {
    t = draft_.suggestion ? "New suggestion" : "New bug report";
  }
  t += " \xC2\xB7 " + std::to_string(n) + (n == 1 ? " new shot" : " new shots");
  target_->setText(t);
  title_->setText(draft_.suggestion ? "Suggestion" : "Bug report");
  // The project's size is painted next to the toggle (paint()).
  attach_->setEnabled(!draft_.project.empty());
  attach_->setOn(draft_.attachProject && !draft_.project.empty());
  markDirty();
}

void BugReportPanel::arrange(LayoutPass& pass) {
  const IRect r = rect();
  const int32_t margin = pass.px(24.0f);
  const int32_t cw = std::min(pass.px(kCardWidth), r.w - 2 * margin), ch = std::min(pass.px(kCardHeight), r.h - 2 * margin);
  card_ = {r.x + (r.w - cw) / 2, r.y + (r.h - ch) / 2, std::max(cw, 0), std::max(ch, 0)};
  const int32_t pad = pass.px(14.0f), gap = pass.px(8.0f), rowH = pass.px(24.0f);
  const IRect c{card_.x + pad, card_.y + pad, card_.w - 2 * pad, card_.h - 2 * pad};
  // Header.
  pass.place(*title_, {c.x, c.y, pass.px(240.0f), rowH});
  const int32_t closeW = pass.px(64.0f), toggleW = pass.px(96.0f);
  pass.place(*close_, {c.right() - closeW, c.y, closeW, rowH});
  pass.place(*suggest_, {c.right() - closeW - gap * 2 - toggleW, c.y, toggleW, rowH});
  pass.place(*bugs_, {c.right() - closeW - gap * 2 - 2 * toggleW - pass.px(2.0f), c.y, toggleW, rowH});
  const int32_t top = c.y + rowH + gap + pass.px(4.0f);
  // Left column: filter, tickets.
  const int32_t leftW = std::min(pass.px(330.0f), c.w / 3);
  pass.place(*filterChooser_, {c.x, top, leftW, pass.px(22.0f)});
  const int32_t listTop = top + pass.px(22.0f) + gap;
  pass.place(*list_, {c.x, listTop, leftW, std::max(0, c.bottom() - listTop)});
  // Right column.
  const int32_t x = c.x + leftW + pad, w = std::max(0, c.right() - x);
  int32_t y = top;
  pass.place(*target_, {x, y, w, pass.px(22.0f)});
  y += pass.px(22.0f) + gap / 2;
  const int32_t stripH = pass.px(kThumbH + 20.0f);
  pass.place(*strip_, {x, y, w, stripH});
  y += stripH + gap;
  // Bottom up: buttons, message, attach, tag entry + chips.
  const int32_t bw = pass.px(104.0f), bh = pass.px(26.0f);
  const int32_t by = c.bottom() - bh;
  pass.place(*submit_, {c.right() - bw, by, bw, bh});
  pass.place(*another_, {c.right() - 2 * bw - gap, by, bw, bh});
  pass.place(*discard_, {c.right() - 3 * bw - 2 * gap, by, bw, bh});
  pass.place(*attach_, {x, by, pass.px(130.0f), bh});
  const int32_t my = by - gap - pass.px(18.0f);
  pass.place(*messageLabel_, {x, my, w, pass.px(18.0f)});
  const int32_t entryH = pass.px(22.0f);
  const int32_t ey = my - gap / 2 - entryH;
  pass.place(*tagEntry_, {x, ey, pass.px(180.0f), entryH});
  const int32_t chipsH = pass.px(3 * 20.0f + 2 * 5.0f);
  const int32_t cy = ey - gap / 2 - chipsH;
  pass.place(*chips_, {x, cy, w, chipsH});
  pass.place(*description_, {x, y, w, std::max(pass.px(60.0f), cy - gap - y)});
}

void BugReportPanel::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  ctx.dl.addRectFilled(rect().toRect(), rgba(0, 0, 0, 120));  // the window behind, dimmed
  if (!th) return;
  paint::box(ctx.dl, card_, ctx.scale.exact(th->radius.panel), th->surface.panel, th->menu.border);
  if (ctx.res.uiFonts && !draft_.project.empty()) {  // the project's size next to its toggle
    const IRect a = attach_->rect();
    paint::text(ctx, ctx.res.uiFonts->small, {a.right() + ctx.scale.px(6.0f), a.y, ctx.scale.px(120.0f), a.h},
                sizeText(draft_.project.size()) + (draft_.project.size() > kAttachProjectDefaultLimit ? " (large)" : ""),
                th->text.secondary, TextAlign::Left);
  }
}

bool BugReportPanel::onPointer(const PointerEvent& e) {
  (void)e;
  return true;  // modal: nothing behind the panel takes the pointer
}

bool BugReportPanel::onKey(const KeyEvent& e) {
  if (!e.down) return true;
  const Mods m = e.mods & mod::kChordMask;
  if (e.key == key::kEscape && m == mod::kNone) {
    if (hooks_.close) hooks_.close();
    return true;
  }
  if (e.key == key::kReturn && m == mod::kCtrl) {
    if (hooks_.submit) hooks_.submit();
    return true;
  }
  if (e.key == key::kTab) return false;  // focus moves between the panel's controls
  return true;  // modal: the DAW's shortcuts do not fire behind the panel
}

BugReportPanel* findBugReportPanel(const Widget& w) {
  OverlayLayer* layer = OverlayLayer::of(w);
  if (!layer) return nullptr;
  for (size_t i = 0; i < layer->childCount(); ++i)
    if (auto* p = dynamic_cast<BugReportPanel*>(&layer->child(i)); p && layer->isOpen(*p)) return p;
  return nullptr;
}

BugReportPanel* openBugReportPanel(OverlayLayer& layer, Draft& draft, fs::path root, PanelHooks hooks) {
  if (BugReportPanel* open = findBugReportPanel(layer)) return open;
  OverlayOptions o;
  o.anchor = {0, 0, 0, 0};
  o.placement = Placement::AtPoint;
  o.dismissOnOutsideClick = false;
  o.takeFocus = true;
  auto& panel = static_cast<BugReportPanel&>(layer.open(std::make_unique<BugReportPanel>(draft, std::move(root), std::move(hooks)), o));
  panel.shotsChanged();  // thumbnails at the tree's scale
  panel.focusDescription();
  return &panel;
}

}  // namespace pq::views::bugreport
