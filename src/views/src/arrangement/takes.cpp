// Take lanes and comping (T6.10): the take lanes' gestures (swipe = comp a range, click = swap a region in, Up / Down =
// the comp selection from the next take), their paint (the track's clips, veils over what the main lane does not take,
// the comp selection) and their headers (name, audition, delete). The document side: pq/document/edit_ops.h "Take lanes".

#include <algorithm>
#include <cmath>

#include "../common/paint.h"
#include "lane_paint.h"
#include "pq/document/edit_ops.h"

namespace pq::views {

using namespace pq::ui;
namespace am = arrangement_metrics;

namespace arrangement {

namespace {

const doc::Track* trackOf(const ArrangementView& v, doc::TrackId id) { return doc::findTrackPtr(*v.shown(), id); }

// Whether the track sounds (its activator and its groups'; as the lanes draw the main lane).
bool rowSounds(std::span<const ArrangementRow> rows, const ArrangementRow& row) noexcept {
  bool active = sounds(*row.track);
  for (int32_t p = row.parent; p >= 0 && active; p = rows[static_cast<size_t>(p)].parent) active = sounds(*rows[static_cast<size_t>(p)].track);
  return active;
}

bool meets(const doc::TakeLane& l, Ticks t0, Ticks t1) noexcept {
  for (const doc::ClipSlot& c : l.clips)
    if (c.start() < t1 && c.end() > t0) return true;
  return false;
}

}  // namespace

Comper::Comper(ArrangementView& view) : view_(view) {}

Ticks Comper::snap(Ticks t) const noexcept {
  const ViewContext& ctx = view_.context();
  if (ctx.grid().off) return std::max<Ticks>(0, t);
  const Grid g(ctx.grid(), view_.shown()->timeline->sigMap, view_.pxPerTick());
  return std::max<Ticks>(0, g.snap(t));
}

void Comper::damageTake(int32_t take, Ticks a, Ticks b) const {
  UiTree* tr = view_.tree();
  if (!tr || take < 0) return;
  const IRect r = view_.takeRect(static_cast<size_t>(take));
  const IRect lv = view_.lanesViewport();
  const auto x0 = static_cast<int32_t>(std::floor(view_.xOfTick(static_cast<double>(std::min(a, b))))) - 2;
  const auto x1 = static_cast<int32_t>(std::ceil(view_.xOfTick(static_cast<double>(std::max(a, b))))) + 2;
  if (const IRect v = intersect(IRect{x0, r.y, std::max(1, x1 - x0), r.h}, lv); !v.empty()) tr->damage().add(v.toRect());
}

void Comper::damageSelection() const {
  if (!sel_) return;
  const auto takes = view_.takeRows();
  for (size_t i = 0; i < takes.size(); ++i)
    if (takes[i].lane == sel_->lane) damageTake(static_cast<int32_t>(i), sel_->range.start, sel_->range.end);
}

void Comper::clearSelection() {
  if (!sel_) return;
  damageSelection();
  sel_.reset();
}

void Comper::abortDrag() {
  if (!press_) return;
  damageTake(press_->take, press_->a, press_->b);
  press_.reset();
}

Cursor Comper::cursorAt(Vec2 p) const {
  if (press_) return Cursor::Arrow;
  return view_.takeAt(p.y) >= 0 ? Cursor::Arrow : Cursor::Inherit;
}

bool Comper::pointer(const PointerEvent& e) {
  if (press_) {
    Press& p = *press_;
    if (e.type == PointerType::Move) {
      const float slop = 3.0f * (view_.tree() ? view_.tree()->scale() : 1.0f);
      if (!p.moved && std::abs(e.pos.x - p.at.x) < slop) return true;
      p.moved = true;
      const auto t = static_cast<Ticks>(std::llround(view_.tickAtX(e.pos.x)));
      const Ticks a = snap(std::min(p.tick, t)), b = snap(std::max(p.tick, t));
      if (a != p.a || b != p.b) {
        damageTake(p.take, p.a, p.b);
        p.a = a;
        p.b = b;
        damageTake(p.take, a, b);
      }
      return true;
    }
    if (e.type == PointerType::Up && e.button == PointerButton::Left) {
      const Press done = p;
      press_.reset();
      damageTake(done.take, done.a, done.b);
      if (!done.moved) (void)swapAt(done);
      else if (done.b > done.a) (void)comp(done.track, done.lane, done.a, done.b);
      return true;
    }
    return true;  // other buttons during the swipe
  }
  if (e.type != PointerType::Down || e.button != PointerButton::Left) return false;
  const int32_t ti = view_.takeAt(e.pos.y);
  if (ti < 0) return false;
  const TakeLaneRow& tr = view_.takeRows()[static_cast<size_t>(ti)];
  Press p;
  p.take = ti;
  p.track = tr.track;
  p.lane = tr.lane;
  p.at = e.pos;
  p.tick = std::max<Ticks>(0, static_cast<Ticks>(std::llround(view_.tickAtX(e.pos.x))));
  press_ = p;
  view_.automation().clearSelection();
  return true;
}

// A click on a take: the main-lane clip under it is replaced by this take over its whole span; in a gap of the main lane,
// the take clip under the click within the gap. A region the take already supplies just becomes the comp selection.
bool Comper::swapAt(const Press& p) {
  const doc::Track* t = trackOf(view_, p.track);
  const doc::TakeLane* lane = t ? t->takeLane(p.lane) : nullptr;
  if (!lane) return false;
  const doc::ClipSlot* under = nullptr;
  Ticks gapStart = 0, gapEnd = INT64_MAX;
  for (const doc::ClipSlot& c : t->clips) {
    if (c.start() <= p.tick && p.tick < c.end()) under = &c;
    if (c.end() <= p.tick) gapStart = std::max(gapStart, c.end());
    if (c.start() > p.tick) gapEnd = std::min(gapEnd, c.start());
  }
  TickRange range{};
  if (under) {
    range = {under->start(), under->end()};
    if (under->take == p.lane) {  // already this take's: select it
      clearSelection();
      sel_ = ArrangementView::CompSelection{p.track, p.lane, range};
      damageSelection();
      return false;
    }
  } else {
    const doc::ClipSlot* source = nullptr;
    for (const doc::ClipSlot& c : lane->clips)
      if (c.start() <= p.tick && p.tick < c.end()) source = &c;
    if (!source) {
      clearSelection();
      return false;
    }
    range = {std::max(source->start(), gapStart), std::min(source->end(), gapEnd)};
  }
  if (range.end <= range.start) return false;
  return comp(p.track, p.lane, range.start, range.end);
}

bool Comper::comp(doc::TrackId track, doc::LaneId lane, Ticks t0, Ticks t1) {
  ViewContext& ctx = view_.context();
  const doc::ops::EditResult e = doc::ops::compRegion(*ctx.doc(), ctx.store().ids(), track, lane, t0, t1);
  if (!e) {
    ctx.setStatus("Comp: " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status s = ctx.store().dispatch(e->command); !s) {
    ctx.setStatus("Comp: " + s.error().message);
    return false;
  }
  clearSelection();
  sel_ = ArrangementView::CompSelection{track, lane, {t0, t1}};
  damageSelection();
  return true;
}

bool Comper::step(int32_t dir) {
  if (!sel_ || dir == 0) return false;
  const doc::Track* t = trackOf(view_, sel_->track);
  if (!t) return false;
  const int32_t at = t->takeIndex(sel_->lane);
  if (at < 0) return false;
  for (int32_t i = at + dir; i >= 0 && i < static_cast<int32_t>(t->takes.size()); i += dir) {
    const doc::TakeLane& l = *t->takes[static_cast<size_t>(i)];
    if (!meets(l, sel_->range.start, sel_->range.end)) continue;  // nothing there to comp
    const ArrangementView::CompSelection s = *sel_;
    return comp(s.track, l.id, s.range.start, s.range.end);
  }
  view_.context().setStatus(dir < 0 ? "Comp: no take above has anything there" : "Comp: no take below has anything there");
  return false;
}

bool Comper::audition(doc::TrackId track, doc::LaneId lane) {
  ViewContext& ctx = view_.context();
  const doc::Track* t = trackOf(view_, track);
  if (!t) return false;
  const doc::LaneId want = t->auditionTake == lane ? doc::LaneId{} : lane;
  const doc::ops::EditResult e = doc::ops::setAuditionTake(*ctx.doc(), track, want);
  if (!e) {
    ctx.setStatus("Audition: " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status s = ctx.store().dispatch(e->command); !s) {
    ctx.setStatus("Audition: " + s.error().message);
    return false;
  }
  return true;
}

bool Comper::remove(doc::TrackId track, doc::LaneId lane) {
  ViewContext& ctx = view_.context();
  const doc::ops::EditResult e = doc::ops::removeTakeLane(*ctx.doc(), track, lane);
  if (!e) {
    ctx.setStatus("Delete Take: " + e.error().message);
    return false;
  }
  if (Status s = ctx.store().dispatch(e->command); !s) {
    ctx.setStatus("Delete Take: " + s.error().message);
    return false;
  }
  return true;  // afterDocumentChange drops a selection / swipe on it
}

void Comper::removeLater(doc::TrackId track, doc::LaneId lane) {
  pendingRemove_ = {track, lane};
  view_.requestFrame();
}

void Comper::runPending() {
  if (!pendingRemove_) return;
  const auto [t, l] = *pendingRemove_;
  pendingRemove_.reset();
  (void)remove(t, l);
}

void Comper::afterDocumentChange() {
  auto gone = [&](doc::TrackId track, doc::LaneId lane) {
    const doc::Track* t = trackOf(view_, track);
    return !t || t->takeLane(lane) == nullptr;
  };
  if (sel_ && gone(sel_->track, sel_->lane)) sel_.reset();
  if (press_) {
    // The rows were rebuilt: find the pressed lane again (gone: the swipe ends).
    press_->take = -1;
    const auto takes = view_.takeRows();
    for (size_t i = 0; i < takes.size(); ++i)
      if (takes[i].lane == press_->lane) press_->take = static_cast<int32_t>(i);
    if (press_->take < 0) press_.reset();
  }
}

// ---- paint -----------------------------------------------------------------------------------------------------

void Comper::paint(LanePaint& lp, int32_t localTop, int32_t localBottom) {
  const auto takes = view_.takeRows();
  if (takes.empty()) return;
  const auto rows = view_.rows();
  Model& m = lp.m;
  DrawList& dl = lp.ctx.dl;
  const Theme& th = lp.th;
  const float cx0 = static_cast<float>(lp.c.x), cx1 = static_cast<float>(lp.c.right());
  auto first = std::upper_bound(takes.begin(), takes.end(), localTop, [](int32_t y, const TakeLaneRow& r) { return y < r.y + r.h; });
  auto last = first;
  while (last != takes.end() && last->y < localBottom) ++last;
  if (first == last) return;
  // Clips (as the main lane draws them: the same caches and tiles), then the tile quads, then the veils over them.
  for (auto it = first; it != last; ++it) {
    ++m.stats.takeRowsPainted;
    const ArrangementRow& row = rows[static_cast<size_t>(it->row)];
    const float y0 = static_cast<float>(lp.r.y + it->y), y1 = static_cast<float>(lp.r.y + it->y + it->h);
    dl.addRectFilled({cx0, y1 - 1.0f, cx1, y1}, th.arrangement.separator);
    const doc::TakeLane& lane = *it->take;
    if (lane.clips.empty()) continue;
    const ClipIndex& ix = m.clips(lane);
    const auto [a, b] = ix.visible(lp.from, lp.to);
    const bool active = rowSounds(rows, row);
    for (size_t j = a; j < b; ++j) {
      const doc::ClipSlot& slot = lane.clips[j];
      if (j > 0 && ix.start[j] < ix.maxEnd[j - 1]) flushTiles(lp);
      ++m.stats.takeClipsPainted;
      if (slot.audio) {
        paintAudioClip(lp, row, slot.audio, {slot.audio->start, slot.audio->length, 0}, y0 + 1.0f, y1 - 1.0f, active, false, kNoHover);
        continue;
      }
      paintClip(lp, row, slot.clip, y0 + 1.0f, y1 - 1.0f, active, {slot.clip->start, slot.clip->length, slot.clip->contentStart}, false);
    }
    for (size_t j = b; j < lane.clips.size() && j < b + 2; ++j)
      if (const doc::AudioClipPtr& next = lane.clips[j].audio) paintAudioCrossfade(lp, *next, y0 + 1.0f, y1 - 1.0f, false);
  }
  flushTiles(lp);
  // The veil: every part of a take clip the main lane does not take from this lane (the bright parts are the comp; an
  // auditioned take is bright all over: it is what plays).
  // M6 review L20: half as opaque again as automation's veil (it read subtle at a glance).
  const Rgba veil = withAlpha(th.automation.veil, std::min(255u, alphaOf(th.automation.veil) * 3u / 2u));
  auto colX = [&](Ticks t) { return std::clamp(std::round(lp.X(static_cast<double>(t))), cx0, cx1); };
  std::vector<TickRange>& used = usedScratch_;
  for (auto it = first; it != last; ++it) {
    const doc::TakeLane& lane = *it->take;
    if (lane.clips.empty()) continue;
    const doc::Track& t = *rows[static_cast<size_t>(it->row)].track;
    if (t.auditionTake == it->lane) continue;  // auditioned: the whole take plays (no veil)
    used.clear();
    if (!t.clips.empty()) {
      const ClipIndex& mix = m.clips(t);
      const auto [ma, mb] = mix.visible(lp.from, lp.to);
      for (size_t k = ma; k < mb; ++k)
        if (t.clips[k].take == it->lane) used.push_back({t.clips[k].start(), t.clips[k].end()});
    }
    const float top = static_cast<float>(lp.r.y + it->y) + 1.0f, bottom = static_cast<float>(lp.r.y + it->y + it->h) - 1.0f;
    const ClipIndex& ix = m.clips(lane);
    const auto [a, b] = ix.visible(lp.from, lp.to);
    size_t u = 0;
    for (size_t j = a; j < b; ++j) {
      Ticks s = std::max(ix.start[j], lp.from);
      const Ticks e = std::min(ix.end[j], lp.to);
      while (u < used.size() && used[u].end <= s) ++u;
      for (size_t v = u; v < used.size() && s < e; ++v) {
        if (used[v].start >= e) break;
        if (used[v].start > s) {
          const float x0 = colX(s), x1 = colX(used[v].start);
          if (x1 > x0) dl.addRectFilled({x0, top, x1, bottom}, veil);
        }
        s = std::max(s, used[v].end);
      }
      if (s < e) {
        const float x0 = colX(s), x1 = colX(e);
        if (x1 > x0) dl.addRectFilled({x0, top, x1, bottom}, veil);
      }
    }
  }
  // The comp selection (outlined) and a running swipe (a band, as the time selection).
  const float w = static_cast<float>(std::max(1, lp.ctx.scale.px(1.0f)));
  for (auto it = first; it != last; ++it) {
    const float top = static_cast<float>(lp.r.y + it->y), bottom = static_cast<float>(lp.r.y + it->y + it->h) - 1.0f;
    if (sel_ && sel_->lane == it->lane) {
      const float x0 = std::round(lp.X(static_cast<double>(sel_->range.start))), x1 = std::round(lp.X(static_cast<double>(sel_->range.end)));
      const Rgba col = th.clip.selectedBorder;
      const float a = std::max(x0, cx0), b = std::min(x1, cx1);
      if (b > a) {
        dl.addRectFilled({a, top, b, top + w}, col);
        dl.addRectFilled({a, bottom - w, b, bottom}, col);
      }
      if (x0 >= cx0 && x0 < cx1) dl.addRectFilled({x0, top, x0 + w, bottom}, col);
      if (x1 - w >= cx0 && x1 - w < cx1) dl.addRectFilled({x1 - w, top, x1, bottom}, col);
    }
    if (press_ && press_->moved && press_->lane == it->lane && press_->b > press_->a) {
      const float a = std::max(cx0, std::floor(lp.X(static_cast<double>(press_->a))));
      const float b = std::min(cx1, std::floor(lp.X(static_cast<double>(press_->b))));
      if (b > a) dl.addRectFilled({a, top, b, bottom}, withAlpha(th.clip.selectedBorder, 46));
    }
  }
}

void Comper::paintBackgrounds(PaintContext& ctx, const IRect& canvas) const {
  const auto takes = view_.takeRows();
  if (takes.empty() || !ctx.res.theme) return;
  const IRect c = intersect(canvas, ctx.clip);
  const int32_t localTop = c.y - canvas.y, localBottom = c.bottom() - canvas.y;
  auto it = std::upper_bound(takes.begin(), takes.end(), localTop, [](int32_t y, const TakeLaneRow& r) { return y < r.y + r.h; });
  for (; it != takes.end() && it->y < localBottom; ++it) {
    const IRect r = intersect(IRect{c.x, canvas.y + it->y, c.w, it->h}, c);
    if (!r.empty()) ctx.dl.addRectFilled(r.toRect(), ctx.res.theme->automation.lane);
  }
}

// ---- take header -------------------------------------------------------------------------------------------------

TakeHeader::TakeHeader(std::string name, ArrangementView& view) : Control(std::move(name)), view_(view) {
  setInfo("Take Lane",
          "One recorded take. Drag across it to comp that range into the track (the main lane); click it to swap it in "
          "for the region under the click; Up / Down then take the same range from the take above / below. The speaker "
          "auditions this take instead of the main lane; × deletes the take lane.");
}

void TakeHeader::bind(const TakeLaneRow& take, uint16_t depth, bool auditioned) {
  const bool changed = take.lane != take_.lane || take.take == nullptr || label_ != take.take->name || depth != depth_ ||
                       auditioned != auditioned_;
  take_ = take;
  depth_ = depth;
  auditioned_ = auditioned;
  if (take.take) label_ = take.take->name;
  if (changed) markDirty();
}

namespace {
// The header's buttons at its right end (as the automation lane header's + / −).
IRect buttonRect(const IRect& r, const UiScale& s, float fromRight) {
  return {r.right() - s.px(fromRight), r.y + s.px(3.0f), s.px(14.0f), std::min(s.px(14.0f), std::max(0, r.h - s.px(4.0f)))};
}
}  // namespace

IRect TakeHeader::auditionRect() const noexcept { return buttonRect(rect(), tree() ? tree()->uiScale() : UiScale{}, 38.0f); }
IRect TakeHeader::deleteRect() const noexcept { return buttonRect(rect(), tree() ? tree()->uiScale() : UiScale{}, 21.0f); }

void TakeHeader::paint(PaintContext& ctx) {
  const Theme* thp = ctx.res.theme;
  if (!thp) return;
  const Theme& th = *thp;
  const IRect r = rect();
  if (r.empty()) return;
  DrawList& dl = ctx.dl;
  const float x0 = static_cast<float>(r.x), y0 = static_cast<float>(r.y), x1 = static_cast<float>(r.right()), y1 = static_cast<float>(r.bottom());
  dl.addRectFilled({x0, y0, x1, y1 - 1.0f}, th.automation.laneHeader);
  dl.addRectFilled({x0, y1 - 1.0f, x1, y1}, th.arrangement.separator);
  const IRect ar = auditionRect(), dr = deleteRect();
  // Name: indented under the track's title (one group level deeper), up to the buttons; in the primary text colour
  // (M6 review L20: the lane-header grey read dim on the take header).
  if (const ThemeFonts* f = ctx.res.uiFonts) {
    const int32_t indent = ctx.scale.px(am::kGroupIndent * static_cast<float>(depth_ + 1));
    const IRect line{r.x + indent, r.y + ctx.scale.px(3.0f), std::max(0, ar.x - r.x - indent - ctx.scale.px(4.0f)), ctx.scale.px(14.0f)};
    paint::text(ctx, f->small, line, label_, th.text.primary, TextAlign::Left, ctx.scale.px(4.0f), 0);
  }
  // Audition: a speaker, lit (the solo colour) while this take plays instead of the main lane.
  if (!ar.empty()) {
    paint::box(dl, ar, 0.0f, auditioned_ ? th.mixer.soloOn : th.mixer.buttonOff, th.mixer.boxBorder);
    const Rgba ink = auditioned_ ? th.mixer.activatorText : th.mixer.buttonText;
    const float cx = static_cast<float>(ar.x) + static_cast<float>(ar.w) * 0.5f, cy = static_cast<float>(ar.y) + static_cast<float>(ar.h) * 0.5f;
    const float u = static_cast<float>(ar.w) / 14.0f;
    dl.addRectFilled({std::round(cx - 4.0f * u), std::round(cy - 1.5f * u), std::round(cx - 1.5f * u), std::round(cy + 1.5f * u)}, ink);
    const Vec2 cone[] = {{cx - 1.5f * u, cy - 1.5f * u}, {cx + 2.5f * u, cy - 4.0f * u}, {cx + 2.5f * u, cy + 4.0f * u}, {cx - 1.5f * u, cy + 1.5f * u}};
    dl.addConvexPolyFilled(cone, ink);
  }
  // Delete: ×.
  if (!dr.empty()) {
    paint::box(dl, dr, 0.0f, th.mixer.buttonOff, th.mixer.boxBorder);
    const float cx = static_cast<float>(dr.x) + static_cast<float>(dr.w) * 0.5f, cy = static_cast<float>(dr.y) + static_cast<float>(dr.h) * 0.5f;
    const float a = std::round(static_cast<float>(dr.w) * 0.22f), t = 0.5f * static_cast<float>(std::max(1, ctx.scale.px(1.0f)));
    const Vec2 d1[] = {{cx - a - t, cy - a + t}, {cx - a + t, cy - a - t}, {cx + a + t, cy + a - t}, {cx + a - t, cy + a + t}};
    const Vec2 d2[] = {{cx + a - t, cy - a - t}, {cx + a + t, cy - a + t}, {cx - a + t, cy + a + t}, {cx - a - t, cy + a - t}};
    dl.addConvexPolyFilled(d1, th.mixer.buttonText);
    dl.addConvexPolyFilled(d2, th.mixer.buttonText);
  }
}

Cursor TakeHeader::cursorAt(Vec2 p) const {
  if (auditionRect().contains(p) || deleteRect().contains(p)) return Cursor::Pointer;
  return Cursor::Inherit;  // a click selects the track
}

bool TakeHeader::onPointer(const PointerEvent& e) {
  if (e.type != PointerType::Down) return e.type == PointerType::Up;
  if (e.button != PointerButton::Left) return true;
  if (auditionRect().contains(e.pos)) {
    (void)view_.auditionTake(take_.track, take_.lane);  // the take headers stay (same count): safe from here
    return true;
  }
  if (deleteRect().contains(e.pos)) {
    view_.comper().removeLater(take_.track, take_.lane);  // removes a take header: not from inside its own handler
    return true;
  }
  view_.selectTrack(take_.track, e.mods);
  return true;
}

}  // namespace arrangement

}  // namespace pq::views
