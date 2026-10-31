// Arrangement editing (T3.9): selection, clip move / copy / resize drags, time selection (rubber band), the edit
// commands behind the arrangement's actions, the in-process clipboard and the right-click edit + grid menu.
//
// Every edit is built by pq::doc::ops from the current snapshot and dispatched once: one undo step. Edits that need
// several builders (cut a range out of clips, then consolidate what is left inside it; clear a destination, then add
// pasted clips) chain them on a scratch snapshot (doc::apply) and dispatch the commands as one Batch.
//
// Drags never touch the document while they run: the lanes draw the dragged clips at their target (Editor::preview)
// and skip the originals; each frame damages only the previews' old and new rects. The edit is dispatched on
// release.

#include <algorithm>
#include <cmath>
#include <limits>

#include "../common/drag.h"
#include "internal.h"
#include "pq/document/audio_clip.h"
#include "pq/document/commands.h"
#include "pq/views/arrangement/arrangement.h"

namespace pq::views::arrangement {

using namespace pq::ui;
namespace am = arrangement_metrics;
using Selection = ArrangementView::Selection;

namespace {

constexpr float kEdgeGrab = 4.0f;       // logical px at each clip end that resize instead of move
constexpr size_t kMaxPreviewRects = 24;  // more dragged clips than this: their bounding box is damaged

Ticks floorMod(Ticks a, Ticks m) noexcept {
  const Ticks r = a % m;
  return r < 0 ? r + m : r;
}

// The start marker a clip has after its start edge moves to `start` (doc::ops::resizeClips' rule, edit_common.cpp
// withBounds): notes keep their arrangement time.
Ticks contentStartAfter(const doc::NoteClip& c, Ticks start) noexcept {
  if (start >= c.start) return doc::clipContentAt(c, start - c.start);
  const Ticks d = c.start - start;
  if (doc::clipLooped(c) && c.contentStart >= c.loop.start && c.contentStart < c.loop.end)
    return c.loop.start + floorMod(c.contentStart - d - c.loop.start, c.loop.end - c.loop.start);
  return c.contentStart - d;
}

// Commands applied to a scratch snapshot one after another and collected into one Batch (one undo step).
struct Chain {
  doc::Snapshot snap;
  doc::Batch batch;
  std::string error;
  doc::ChangeSet scratch;

  explicit Chain(doc::Snapshot s, std::string label) : snap(std::move(s)) { batch.label = std::move(label); }
  bool run(const doc::Command& c) {
    if (!error.empty()) return false;
    Result<doc::Snapshot> r = doc::apply(snap, c, scratch);
    if (!r) {
      error = r.error().message;
      return false;
    }
    snap = std::move(*r);
    batch.commands.push_back(c);
    return true;
  }
  bool run(const doc::ops::EditResult& e) {
    if (!error.empty()) return false;
    if (!e) {
      error = e.error().message;
      return false;
    }
    return e->empty() || run(e->command);
  }
};

bool overlaps(const doc::ClipSlot& c, Ticks s, Ticks e) noexcept { return c.start() < e && c.end() > s; }

// Removes whatever lies in [s, e) on a MIDI or audio track (T5.6: either kind): the clips crossing the edges are split
// there (each piece plays what the clip played), then the pieces inside are deleted. Nothing when no clip meets the range.
bool carve(Chain& ch, doc::IdSource& ids, doc::TrackId track, Ticks s, Ticks e) {
  const doc::Track* t = doc::findTrackPtr(*ch.snap, track);
  if (!t || (t->kind != doc::TrackKind::Midi && t->kind != doc::TrackKind::Audio) || e <= s) return true;
  std::vector<doc::ClipRef> crossing;
  for (const doc::ClipSlot& c : t->clips)
    if (overlaps(c, s, e) && (c.start() < s || c.end() > e)) crossing.push_back({track, c.id});
  if (!crossing.empty()) {
    const Ticks cuts[] = {s, e};
    if (!ch.run(doc::ops::splitClips(*ch.snap, ids, crossing, cuts))) return false;
    t = doc::findTrackPtr(*ch.snap, track);
  }
  std::vector<doc::ClipRef> inside;
  for (const doc::ClipSlot& c : t->clips)
    if (c.start() >= s && c.end() <= e) inside.push_back({track, c.id});
  if (inside.empty()) return true;
  return ch.run(doc::ops::deleteClips(*ch.snap, inside));
}

// A copy of a clip with fresh clip and note ids at `start` (as doc::ops copies clips). T5.6: an audio clip keeps its
// audio (warp Off: its length re-derived at the new start).
doc::ClipSlot cloneFresh(const doc::ClipSlot& slot, doc::IdSource& ids, Ticks start, const doc::Timeline& timeline) {
  doc::ClipSlot out;
  if (slot.audio) {
    doc::AudioClip a = doc::audioMovedTo(timeline, *slot.audio, start);
    a.id = ids.clip();
    a.crossfadeIn = 0;  // a pasted clip has no neighbour yet
    out.id = a.id;
    out.audio = std::make_shared<const doc::AudioClip>(std::move(a));
    return out;
  }
  const doc::NoteClip& c = *slot.clip;
  std::vector<doc::Note> notes;
  notes.reserve(c.notes.size());
  for (const doc::Note& n : c.notes) {
    doc::Note m = n;
    m.id = ids.note();
    notes.push_back(m);
  }
  Result<doc::NoteSeq> seq = doc::NoteSeq::fromNotes(std::move(notes));
  doc::NoteClip nc = doc::withNotes(c, seq ? std::move(*seq) : doc::NoteSeq{});
  nc.id = ids.clip();
  nc.start = start;
  out.id = nc.id;
  out.clip = std::make_shared<const doc::NoteClip>(std::move(nc));
  return out;
}

const doc::ClipSlot* findClip(const doc::Project& p, const doc::ClipRef& r) noexcept {
  const doc::Track* t = doc::findTrackPtr(p, r.track);
  if (!t) return nullptr;
  const int32_t i = doc::findClipIndex(*t, r.clip);
  return i < 0 ? nullptr : &t->clips[static_cast<size_t>(i)];
}

void collectMidi(const doc::Track& t, std::vector<doc::TrackId>& out, bool audioToo = false) {
  if (t.kind == doc::TrackKind::Midi || (audioToo && t.kind == doc::TrackKind::Audio)) {
    if (std::find(out.begin(), out.end(), t.id) == out.end()) out.push_back(t.id);
    return;
  }
  for (const doc::TrackPtr& c : t.children) collectMidi(*c, out, audioToo);
}

}  // namespace

// --- construction, geometry ---------------------------------------------------------------------------------

Editor::Editor(ArrangementView& view) : view_(view), ctx_(view.context()) {}

Grid Editor::grid() const noexcept { return Grid(ctx_.grid(), view_.shown()->timeline->sigMap, view_.pxPerTick()); }

Ticks Editor::snapTick(Ticks t) const noexcept { return grid().snap(t); }

Ticks Editor::tickAt(float x) const noexcept { return static_cast<Ticks>(std::llround(view_.tickAtX(x))); }

int32_t Editor::rowOf(doc::TrackId id) const noexcept {
  const auto rows = view_.rows();
  for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i].id == id) return static_cast<int32_t>(i);
  return -1;
}

// The row under y, clamped to the first / last row when y is above / below them.
int32_t Editor::clampRow(float y) const noexcept {
  const auto rows = view_.rows();
  if (rows.empty()) return -1;
  const int32_t r = view_.rowAt(y);
  if (r >= 0) return r;
  // In a track's automation lanes (T3.15): that track's row.
  const float local = y - static_cast<float>(view_.rowLaneRect(0).y - rows.front().y);
  const auto it = std::upper_bound(rows.begin(), rows.end(), local,
                                   [](float v, const ArrangementRow& row) { return v < static_cast<float>(row.y + row.h + row.lanesH); });
  if (it != rows.end() && local >= static_cast<float>(it->y)) return static_cast<int32_t>(it - rows.begin());
  return y < static_cast<float>(view_.rowLaneRect(0).y) ? 0 : static_cast<int32_t>(rows.size() - 1);
}

IRect Editor::clipBox(int32_t row, Ticks start, Ticks end) const noexcept {
  const IRect lane = view_.rowLaneRect(row);
  const IRect lv = view_.lanesViewport();
  const float lo = static_cast<float>(lv.x - 8), hi = static_cast<float>(lv.right() + 8);
  const float x0 = std::clamp(std::round(view_.xOfTick(static_cast<double>(start))), lo, hi);
  const float x1 = std::clamp(std::max(x0 + 1.0f, std::round(view_.xOfTick(static_cast<double>(end)))), lo, hi);
  return {static_cast<int32_t>(x0), lane.y, std::max(1, static_cast<int32_t>(x1 - x0)), lane.h};
}

Editor::Hit Editor::hitTest(Vec2 p) const {
  Hit h;
  h.row = view_.rowAt(p.y);
  if (h.row < 0) return h;
  const ArrangementRow& row = view_.rows()[static_cast<size_t>(h.row)];
  if (row.group || row.track->clips.empty()) return h;
  const IRect lane = view_.rowLaneRect(h.row);
  const float top = static_cast<float>(lane.y + 1), bottom = static_cast<float>(lane.bottom() - 1);
  if (p.y < top || p.y >= bottom) return h;
  const UiScale s = view_.tree() ? view_.tree()->uiScale() : UiScale{};
  const doc::Track& t = *row.track;
  const ClipIndex& ix = view_.model().clips(t);
  const auto from = static_cast<Ticks>(std::floor(view_.tickAtX(p.x - 1.0f)));
  const auto to = static_cast<Ticks>(std::ceil(view_.tickAtX(p.x + 1.0f))) + 1;
  const auto [a, b] = ix.visible(from, to);
  for (size_t j = b; j-- > a;) {  // the last drawn is on top
    const float x0 = std::round(view_.xOfTick(static_cast<double>(ix.start[j])));
    const float x1 = std::max(x0 + 1.0f, std::round(view_.xOfTick(static_cast<double>(ix.end[j]))));
    if (p.x < x0 || p.x >= x1) continue;
    h.clip = &t.clips[j];
    h.ref = {t.id, t.clips[j].id};
    const float edge = std::min(static_cast<float>(s.px(kEdgeGrab)), std::floor((x1 - x0) / 4.0f));
    const float titleB = top + static_cast<float>(s.px(am::kClipTitleHeight));
    // T5.6: an audio clip's handles (fades, gain) at the top of its body take the press before the edges.
    if (const doc::AudioClipPtr& ac = t.clips[j].audio; ac && p.y >= titleB && ctx_.sampleAvailable(ac->sample)) {
      const AudioHandleRects hr = audioHandles(*ac, x0, x1, titleB, bottom - 1.0f, view_.devicePxPerTick(), s.factor);
      const float slop = std::max(1.0f, std::round(2.0f * s.factor));
      auto in = [&](const Rect& r) { return p.x >= r.x0 - slop && p.x < r.x1 + slop && p.y >= r.y0 - slop && p.y < r.y1 + slop; };
      if (hr.show) {
        if (in(hr.gain)) h.zone = kZoneGain;
        else if (in(hr.fadeIn)) h.zone = kZoneFadeIn;
        else if (in(hr.fadeOut)) h.zone = kZoneFadeOut;
        if (h.zone >= kZoneFadeIn) return h;
      }
    }
    if (edge >= 1.0f && p.x < x0 + edge) h.zone = -1;
    else if (edge >= 1.0f && p.x >= x1 - edge) h.zone = 1;
    else h.zone = (p.y < titleB || bottom - titleB < 4.0f) ? 0 : 2;  // a body under 4 px: the clip is all title
    return h;
  }
  return h;
}

// --- selection ----------------------------------------------------------------------------------------------

Selection Editor::selectionOfClips(std::vector<doc::ClipRef> clips) const {
  Selection s;
  const doc::Project& p = *ctx_.doc();
  Ticks lo = std::numeric_limits<Ticks>::max(), hi = std::numeric_limits<Ticks>::min();
  for (const doc::ClipRef& r : clips) {
    const doc::ClipSlot* c = findClip(p, r);
    if (!c) continue;
    if (std::any_of(s.clips.begin(), s.clips.end(), [&](const doc::ClipRef& x) { return x.clip == r.clip; })) continue;
    s.clips.push_back(r);
    lo = std::min(lo, c->start());
    hi = std::max(hi, c->end());
    if (std::find(s.tracks.begin(), s.tracks.end(), r.track) == s.tracks.end()) s.tracks.push_back(r.track);
  }
  std::stable_sort(s.tracks.begin(), s.tracks.end(), [&](doc::TrackId a, doc::TrackId b) {
    return static_cast<uint32_t>(rowOf(a)) < static_cast<uint32_t>(rowOf(b));  // rows in order, hidden tracks last
  });
  if (!s.clips.empty()) s.time = TickRange{lo, hi};
  return s;
}

void Editor::setSelection(Selection s, bool showInClipView) {
  Selection old = std::move(sel_);
  sel_ = std::move(s);
  ids_.clear();
  for (const doc::ClipRef& r : sel_.clips) ids_.push_back(r.clip.value);
  std::sort(ids_.begin(), ids_.end());
  trackIds_.clear();
  for (doc::TrackId t : sel_.tracks) trackIds_.push_back(t.value);
  std::sort(trackIds_.begin(), trackIds_.end());
  damageSelection(old, sel_);
  if (old.tracks != sel_.tracks) view_.syncHeaderSelection();  // T3.10: selected headers are tinted
  view_.setTimeSelection(sel_.time && sel_.time->end > sel_.time->start ? sel_.time : std::nullopt);
  if (showInClipView && !sel_.clips.empty()) ctx_.setDetailClip(sel_.clips.front());
  ctx_.setSelectedTrack(sel_.tracks.empty() ? doc::TrackId{} : sel_.tracks.front());
}

// [t0, t1] covering the time selection (or insert marker) and every selected clip. False when there is neither.
bool Editor::spanOf(const Selection& s, double& t0, double& t1) const {
  bool any = false;
  t0 = std::numeric_limits<double>::max();
  t1 = std::numeric_limits<double>::lowest();
  if (s.time) {
    t0 = static_cast<double>(s.time->start);
    t1 = static_cast<double>(s.time->end);
    any = true;
  }
  const doc::Project& p = *view_.shown();
  for (const doc::ClipRef& r : s.clips)
    if (const doc::ClipSlot* c = findClip(p, r)) {
      t0 = std::min(t0, static_cast<double>(c->start()));
      t1 = std::max(t1, static_cast<double>(c->end()));
      any = true;
    }
  return any;
}

// The selected tracks' rows: whole rows (their tint changes) or only the columns of [t0, t1].
void Editor::damageRows(const Selection& s, double t0, double t1, bool fullRows) {
  UiTree* tr = view_.tree();
  if (!tr) return;
  const IRect lv = view_.lanesViewport();
  int32_t x0 = lv.x, x1 = lv.right();
  if (!fullRows) {
    const float lo = static_cast<float>(lv.x - 4), hi = static_cast<float>(lv.right() + 4);
    x0 = std::max(lv.x, static_cast<int32_t>(std::clamp(std::floor(view_.xOfTick(t0)), lo, hi)) - 3);
    x1 = std::min(lv.right(), static_cast<int32_t>(std::clamp(std::ceil(view_.xOfTick(t1)), lo, hi)) + 3);
    if (x1 <= x0) return;
  }
  for (doc::TrackId t : s.tracks) {
    const int32_t row = rowOf(t);
    if (row < 0) continue;
    const IRect lane = view_.rowLaneRect(row);
    const IRect r = intersect(IRect{x0, lane.y, x1 - x0, lane.h}, lv);
    if (!r.empty()) tr->damage().add(r.toRect());
  }
}

void Editor::damageSelection(const Selection& a, const Selection& b) {
  if (a.tracks != b.tracks) {
    damageRows(a, 0, 0, true);
    damageRows(b, 0, 0, true);
    return;
  }
  double a0 = 0, a1 = 0, b0 = 0, b1 = 0;
  const bool ha = spanOf(a, a0, a1), hb = spanOf(b, b0, b1);
  if (!ha && !hb) return;
  damageRows(b, std::min(ha ? a0 : b0, hb ? b0 : a0), std::max(ha ? a1 : b1, hb ? b1 : a1), false);
}

void Editor::afterDocumentChange(const doc::StoreEvent& e) {
  if (e.kind == doc::StoreEventKind::Reset) {
    drag_ = {};
    hover_ = {};
    sel_ = {};
    ids_.clear();
    trackIds_.clear();
    view_.setTimeSelection(std::nullopt);
    return;
  }
  if (drag_.kind == DragKind::Move || drag_.kind == DragKind::Resize) drag_ = {};  // the clips changed under it
  // T5.6: an audio handle drag edits the document itself (its own gesture); anything else ending the gesture (undo)
  // ends the drag.
  if ((drag_.kind == DragKind::Fade || drag_.kind == DragKind::Gain) && !ctx_.store().inGesture()) drag_ = {};
  // Drop what no longer exists (undo of a creation, a deleted track). The lanes repaint fully after a document
  // change, so nothing needs damaging here.
  const doc::Project& p = *ctx_.doc();
  const size_t nc = sel_.clips.size(), nt = sel_.tracks.size();
  std::erase_if(sel_.clips, [&](const doc::ClipRef& r) { return findClip(p, r) == nullptr; });
  std::erase_if(sel_.tracks, [&](doc::TrackId t) { return doc::findTrackPtr(p, t) == nullptr; });
  if (sel_.clips.size() != nc || sel_.tracks.size() != nt) {
    Selection s = std::move(sel_);
    sel_ = {};
    if (s.clips.empty() && s.tracks.empty()) s.time.reset();
    setSelection(std::move(s), false);
  }
}

void Editor::selectTrack(doc::TrackId id, Mods mods) {
  if (UiTree* t = view_.tree()) t->setFocus(&view_);
  Selection s;
  if (mods & (mod::kShift | mod::kCtrl)) {
    s.tracks = sel_.tracks;
    const auto it = std::find(s.tracks.begin(), s.tracks.end(), id);
    if (it == s.tracks.end()) s.tracks.push_back(id);
    else if (mods & mod::kCtrl) s.tracks.erase(it);
    std::stable_sort(s.tracks.begin(), s.tracks.end(), [&](doc::TrackId a, doc::TrackId b) {
      return static_cast<uint32_t>(rowOf(a)) < static_cast<uint32_t>(rowOf(b));
    });
  } else {
    s.tracks = {id};
  }
  setSelection(std::move(s), false);
}

void Editor::nudgeMarker(int32_t cells) {
  const Ticks from = sel_.time ? sel_.time->start : ctx_.startMarker();
  const Ticks t = std::max<Ticks>(0, drag::stepByCells(grid(), from, cells));
  Selection s;
  s.tracks = sel_.tracks;
  s.time = TickRange{t, t};
  setSelection(std::move(s), false);
  view_.revealTick(t);
}

// T4.16b: the clips move by whole grid cells measured from the earliest one's start (as the piano roll nudges notes);
// moveClipsBy overwrites what lies at the destination, as a drag does.
bool Editor::nudgeClips(int32_t cells) {
  if (sel_.clips.empty() || cells == 0) return false;
  const doc::Project& p = *ctx_.doc();
  Ticks first = std::numeric_limits<Ticks>::max();
  for (const doc::ClipRef& r : sel_.clips)
    if (const doc::ClipSlot* c = findClip(p, r)) first = std::min(first, c->start());
  if (first == std::numeric_limits<Ticks>::max()) return false;
  const Ticks dt = std::max<Ticks>(0, drag::stepByCells(grid(), first, cells)) - first;
  if (dt == 0) return false;
  return apply(doc::ops::moveClipsBy(p, ctx_.store().ids(), sel_.clips, dt), "Move", true);
}

bool Editor::nudgeClipsFine(int32_t dir) {
  if (sel_.clips.empty() || dir == 0) return false;
  const doc::Project& p = *ctx_.doc();
  Ticks first = std::numeric_limits<Ticks>::max();
  for (const doc::ClipRef& r : sel_.clips)
    if (const doc::ClipSlot* c = findClip(p, r)) first = std::min(first, c->start());
  if (first == std::numeric_limits<Ticks>::max()) return false;
  const Ticks dt = std::max<Ticks>(dir * drag::kFineNudge, -first);
  if (dt == 0) return false;
  return apply(doc::ops::moveClipsBy(p, ctx_.store().ids(), sel_.clips, dt), "Move", true);
}

// Shift+Left / Right (Live): the time selection's end one grid cell later / earlier (never before its start); from
// the insert marker it becomes a range. The selected tracks stay; the clips the range touches are selected.
void Editor::extendTime(int32_t cells) {
  const Ticks start = sel_.time ? sel_.time->start : std::max<Ticks>(0, ctx_.startMarker());
  const Ticks end = sel_.time ? sel_.time->end : start;
  const Ticks e = std::max(start, drag::stepByCells(grid(), end, cells));
  Selection s;
  s.tracks = sel_.tracks;
  s.time = TickRange{start, e};
  s.range = e > start;
  if (s.range) s.clips = clipsMeeting(0, static_cast<int32_t>(view_.rows().size()) - 1, true, start, e, {});
  setSelection(std::move(s), false);
  view_.revealTick(e);
}

bool Editor::deselect() {
  if (sel_.clips.empty() && !sel_.time) return false;
  Selection s;
  s.tracks = sel_.tracks;
  setSelection(std::move(s), false);
  return true;
}

std::vector<doc::ClipRef> Editor::clipsMeeting(int32_t r0, int32_t r1, bool selectedTracksOnly, Ticks s, Ticks e,
                                               std::vector<doc::ClipRef> clips) const {
  const auto rows = view_.rows();
  if (e > s)
    for (int32_t i = std::max(0, r0); i <= r1 && static_cast<size_t>(i) < rows.size(); ++i) {
      const ArrangementRow& row = rows[static_cast<size_t>(i)];
      if (row.group || row.track->clips.empty() || (selectedTracksOnly && !trackSelected(row.id))) continue;
      const ClipIndex& ix = view_.model().clips(*row.track);
      const auto [a, b] = ix.visible(s, e);
      for (size_t j = a; j < b; ++j)
        if (ix.start[j] < e && ix.end[j] > s)
          clips.push_back({row.id, row.track->clips[j].id});
    }
  // De-duplicated by id (a base may repeat clips the range covers).
  std::sort(clips.begin(), clips.end(), [](const doc::ClipRef& a, const doc::ClipRef& b) { return a.clip.value < b.clip.value; });
  clips.erase(std::unique(clips.begin(), clips.end(), [](const doc::ClipRef& a, const doc::ClipRef& b) { return a.clip == b.clip; }),
              clips.end());
  return clips;
}

// --- affordances (T4.16b) -------------------------------------------------------------------------------------

Cursor Editor::cursorAt(Vec2 p) const {
  if (drag_.kind != DragKind::None) {
    switch (drag_.kind) {
      case DragKind::Move: return Cursor::Move;
      case DragKind::Resize: return Cursor::ResizeEW;
      case DragKind::Fade: return drag_.curve ? Cursor::ResizeNS : Cursor::ResizeEW;
      case DragKind::Gain: return Cursor::ResizeNS;
      default: return Cursor::Arrow;  // a time selection
    }
  }
  const Hit h = hitTest(p);
  if (!h.clip) return Cursor::Arrow;  // empty lane space: a drag selects time
  if (h.zone == 0) return Cursor::Move;
  if (h.zone == kZoneGain) return Cursor::ResizeNS;
  if (h.zone == kZoneFadeIn || h.zone == kZoneFadeOut) return Cursor::ResizeEW;
  return h.zone == 2 ? Cursor::Arrow : Cursor::ResizeEW;  // the body selects time (Live); the ends trim
}

void Editor::setHover(HoverPart h) {
  if (h == hover_) return;
  if (UiTree* tr = view_.tree()) {
    const IRect lv = view_.lanesViewport();
    const doc::Project& p = *view_.shown();
    for (const HoverPart* x : {&hover_, &h}) {
      if (!x->ref.clip.valid()) continue;
      const int32_t row = rowOf(x->ref.track);
      const doc::ClipSlot* c = row >= 0 ? findClip(p, x->ref) : nullptr;
      if (!c) continue;
      IRect b = clipBox(row, c->start(), c->end());
      if (c->audio && c->audio->crossfadeIn > 0) {  // the X reaches into the previous clip
        const auto xf = static_cast<int32_t>(std::ceil(static_cast<double>(c->audio->crossfadeIn) * view_.devicePxPerTick()));
        b = {b.x - xf, b.y, b.w + xf, b.h};
      }
      if (const IRect v = intersect(IRect{b.x - 1, b.y, b.w + 2, b.h}, lv); !v.empty()) tr->damage().add(v.toRect());
    }
  }
  hover_ = h;
}

void Editor::hoverAt(Vec2 p) {
  if (drag_.kind != DragKind::None) return;
  const Hit h = hitTest(p);
  HoverPart hp;
  if (h.clip && h.zone != 2) {
    hp.ref = h.ref;
    hp.zone = static_cast<int8_t>(h.zone);
  }
  setHover(hp);
}

void Editor::clearHover() { setHover({}); }

void Editor::stepTrack(int32_t rows) {
  const auto rs = view_.rows();
  if (rs.empty() || rows == 0) return;
  int32_t cur = sel_.tracks.empty() ? (rows > 0 ? -1 : static_cast<int32_t>(rs.size())) : rowOf(rows > 0 ? sel_.tracks.back() : sel_.tracks.front());
  const int32_t next = std::clamp(cur + rows, 0, static_cast<int32_t>(rs.size()) - 1);
  Selection s;
  s.tracks = {rs[static_cast<size_t>(next)].id};
  s.time = sel_.time;
  setSelection(std::move(s), false);
  view_.revealRow(next);
}

// --- pointer ------------------------------------------------------------------------------------------------

bool Editor::pointer(const PointerEvent& e) {
  switch (e.type) {
    case PointerType::Down:
      if (e.button != PointerButton::Left && e.button != PointerButton::Right) return false;
      press(e);
      return true;
    case PointerType::Move:
      if (drag_.kind == DragKind::None) return false;
      dragTo(e);
      return true;
    case PointerType::Up:
      if (e.button != PointerButton::Left) return true;
      release(e);
      return true;
    default: return false;
  }
}

void Editor::press(const PointerEvent& e) {
  if (UiTree* t = view_.tree()) t->setFocus(&view_);
  if (drag_.kind != DragKind::None) cancelDrag();
  clearHover();  // the gesture shows its own preview
  pressSel_ = sel_;
  drag_ = {};
  drag_.press = e.pos;
  const Hit h = hitTest(e.pos);
  const bool toggle = (e.mods & mod::kCtrl) != 0, add = (e.mods & mod::kShift) != 0;

  if (e.button == PointerButton::Right) {
    if (h.clip && !selected(h.ref.clip)) setSelection(selectionOfClips({h.ref}), true);
    (void)openContextMenu(e.pos);
    return;
  }

  // T5.6: an audio clip's handles: a drag edits the fade / gain (double-click on the gain handle: 0 dB).
  if (h.clip && h.clip->audio && h.zone >= kZoneFadeIn && e.button == PointerButton::Left) {
    if (!selected(h.ref.clip)) setSelection(selectionOfClips({h.ref}), true);
    if (h.zone == kZoneGain && e.clickCount >= 2) {
      (void)apply(doc::ops::setAudioClipGain(*ctx_.doc(), std::span<const doc::ClipRef>(&h.ref, 1), 0.0f), "Clip Gain", false);
      return;
    }
    beginAudioDrag(h, e.mods);
    return;
  }

  if (h.clip) {
    if (e.clickCount >= 2 && !toggle && !add) {
      // Double-click: select it and open it in the detail view (Live).
      setSelection(selectionOfClips({h.ref}), true);
      if (view_.onOpenClip) view_.onOpenClip();
      return;
    }
    const bool was = selected(h.ref.clip);
    if (toggle) {
      std::vector<doc::ClipRef> clips = sel_.clips;
      if (was) std::erase_if(clips, [&](const doc::ClipRef& r) { return r.clip == h.ref.clip; });
      else clips.insert(clips.begin(), h.ref);
      setSelection(selectionOfClips(std::move(clips)), !was);
      if (was) return;  // toggled off: no drag
    } else if (add) {
      if (!was) {
        std::vector<doc::ClipRef> clips = sel_.clips;
        clips.insert(clips.begin(), h.ref);
        setSelection(selectionOfClips(std::move(clips)), true);
      }
    } else if (h.zone == 2) {
      // The body: select the clip and put the insert marker there; a drag makes a time selection (Live).
      Selection s = selectionOfClips({h.ref});
      const Ticks m = std::max<Ticks>(0, snapTick(tickAt(e.pos.x)));
      s.time = TickRange{m, m};
      setSelection(std::move(s), true);
      drag_.kind = DragKind::Range;
      drag_.origin = m;
      drag_.anchorRow = h.row;
      return;
    } else if (!was) {
      setSelection(selectionOfClips({h.ref}), true);
    } else {
      ctx_.setDetailClip(h.ref);
      drag_.collapseOnClick = sel_.clips.size() > 1;
      drag_.pressed = h.ref;
    }
    if (h.zone == 2) return;  // Shift / Ctrl on a body: selection only
    beginClipDrag(h.zone == 0 ? DragKind::Move : DragKind::Resize, h);
    return;
  }

  // Empty lane space (or a group lane).
  if (h.row < 0) {
    setSelection({}, false);
    return;
  }
  const Ticks t = std::max<Ticks>(0, snapTick(tickAt(e.pos.x)));
  drag_.kind = DragKind::Range;
  if (add && sel_.time) {
    // Shift+click extends the time selection to the click (and over the rows between).
    drag_.origin = t < sel_.time->start ? sel_.time->end : sel_.time->start;
    const int32_t first = sel_.tracks.empty() ? -1 : rowOf(sel_.tracks.front());
    drag_.anchorRow = first >= 0 ? first : h.row;
    drag_.moved = true;
    updateRange(e.pos);
    return;
  }
  drag_.origin = t;
  drag_.anchorRow = h.row;
  drag_.additive = toggle;
  if (toggle) drag_.base = sel_.clips;
  Selection s;
  s.tracks = {view_.rows()[static_cast<size_t>(h.row)].id};
  s.time = TickRange{t, t};
  setSelection(std::move(s), false);
}

void Editor::beginClipDrag(DragKind kind, const Hit& h) {
  drag_.kind = kind;
  drag_.pressRow = h.row;
  drag_.edge = h.zone < 0 ? doc::ops::ClipEdge::Start : doc::ops::ClipEdge::End;
  drag_.origin = (kind == DragKind::Move || h.zone < 0) ? h.clip->start() : h.clip->end();
  const doc::Project& p = *ctx_.doc();
  Ticks minStart = std::numeric_limits<Ticks>::max(), maxEnd = 0, minLen = std::numeric_limits<Ticks>::max();
  Ticks trimLeft = std::numeric_limits<Ticks>::min(), trimRight = std::numeric_limits<Ticks>::max();
  drag_.clips.clear();
  drag_.clips.reserve(sel_.clips.size());
  for (const doc::ClipRef& r : sel_.clips) {
    const doc::Track* t = doc::findTrackPtr(p, r.track);
    const int32_t i = t ? doc::findClipIndex(*t, r.clip) : -1;
    if (i < 0) continue;
    const doc::ClipSlot& c = t->clips[static_cast<size_t>(i)];
    drag_.clips.push_back({r, c, rowOf(r.track)});
    minStart = std::min(minStart, c.start());
    maxEnd = std::max(maxEnd, c.end());
    minLen = std::min(minLen, c.length());
    if (c.audio && kind == DragKind::Resize) {  // T5.6: trimming stops at the file's ends
      Ticks lo = 0, hi = 0;
      audioTrimLimits(*c.audio, lo, hi);
      trimLeft = std::max(trimLeft, lo);
      trimRight = std::min(trimRight, hi);
    }
  }
  if (drag_.clips.empty()) {
    drag_.kind = DragKind::None;
    return;
  }
  const Ticks far = doc::kMaxTicks / 2;
  if (kind == DragKind::Move) {
    drag_.minDt = -minStart;
    drag_.maxDt = std::max<Ticks>(0, far - maxEnd);
  } else if (drag_.edge == doc::ops::ClipEdge::Start) {
    drag_.minDt = std::min<Ticks>(0, std::max(-minStart, trimLeft));
    drag_.maxDt = minLen - 1;
  } else {
    drag_.minDt = 1 - minLen;
    drag_.maxDt = std::max<Ticks>(0, std::min(far - maxEnd, trimRight));
  }
}

// Off: the window of source frames may grow until it meets the file's start or end (a reversed clip's left edge shows
// the window's end); RePitch without a loop: content ticks [0, file length); a looped RePitch clip extends through its
// loop (no limit).
void Editor::audioTrimLimits(const doc::AudioClip& c, Ticks& minStartDt, Ticks& maxEndDt) const {
  minStartDt = std::numeric_limits<Ticks>::min();
  maxEndDt = std::numeric_limits<Ticks>::max();
  const doc::Project& p = *ctx_.doc();
  const doc::SampleRef* sref = doc::findSample(p, c.sample);
  if (!sref) return;
  const auto frames = static_cast<double>(sref->frames);
  if (c.warp == doc::AudioWarp::Off) {
    const std::optional<doc::TrackPath> path = [&]() -> std::optional<doc::TrackPath> {
      for (const doc::ClipRef& r : sel_.clips)
        if (r.clip == c.id) return doc::findTrack(p, r.track);
      return std::nullopt;
    }();
    const double speed = sref->rate * (path ? doc::audioRateRatio(p, *path, c) : 1.0);
    if (!(speed > 0.0)) return;
    const double head = std::max(0.0, c.contentStartFrame) / speed;                       // seconds before the window
    const double tail = std::max(0.0, frames - c.contentStartFrame - c.seconds * speed) / speed;  // after it
    const double left = c.reverse ? tail : head, right = c.reverse ? head : tail;
    const TempoMap& tm = p.timeline->tempoMap;
    const double t0 = tm.ticksToSeconds(c.start), t1 = tm.ticksToSeconds(c.start + c.length);
    minStartDt = static_cast<Ticks>(std::ceil(tm.secondsToTicks(t0 - left) - 1e-6)) - c.start;
    maxEndDt = static_cast<Ticks>(std::floor(tm.secondsToTicks(t1 + right) + 1e-6)) - (c.start + c.length);
    return;
  }
  if (contentLooped(doc::contentLoop(c))) return;
  const double fpt = doc::framesPerContentTick(sref->rate, c.sourceBpm);
  if (!(fpt > 0.0)) return;
  const auto contentLen = static_cast<Ticks>(std::floor(frames / fpt));
  const Ticks before = std::max<Ticks>(0, c.contentStart), after = std::max<Ticks>(0, contentLen - (c.contentStart + c.length));
  minStartDt = -(c.reverse ? after : before);
  maxEndDt = c.reverse ? before : after;
}

// --- audio handles (T5.6) -------------------------------------------------------------------------------------

void Editor::beginAudioDrag(const Hit& h, Mods mods) {
  drag_.kind = h.zone == kZoneGain ? DragKind::Gain : DragKind::Fade;
  drag_.audio = h.clip->audio;
  drag_.zone = h.zone;
  drag_.curve = (mods & mod::kAlt) != 0 && h.zone != kZoneGain;
  drag_.pressed = h.ref;
  drag_.pressRow = h.row;
  (void)ctx_.store().beginGesture(h.zone == kZoneGain ? "Clip Gain" : "Clip Fade");
}

void Editor::audioDragTo(const PointerEvent& e) {
  const doc::AudioClip& c0 = *drag_.audio;
  const doc::Snapshot snap = ctx_.doc();  // held: the dispatches below replace the store's current snapshot
  const doc::Project& p = *snap;
  const doc::ClipRef ref = drag_.pressed;
  const doc::ClipSlot* now = findClip(p, ref);
  if (!now || !now->audio) return;
  const doc::AudioClip& c = *now->audio;
  const float scale = view_.tree() ? view_.tree()->scale() : 1.0f;
  if (drag_.kind == DragKind::Gain) {
    const double perPx = ((e.mods & mod::kShift) ? 0.02 : 0.2) / static_cast<double>(scale);
    const double db = std::clamp(static_cast<double>(c0.gainDb) - static_cast<double>(e.pos.y - drag_.press.y) * perPx,
                                 static_cast<double>(doc::kMinClipGainDb), static_cast<double>(doc::kMaxClipGainDb));
    if (static_cast<float>(db) != c.gainDb) (void)apply(doc::ops::setAudioClipGain(p, std::span<const doc::ClipRef>(&ref, 1), static_cast<float>(db)), "Clip Gain", false);
    return;
  }
  const bool in = drag_.zone == kZoneFadeIn;
  if (drag_.curve) {  // Alt: the curve (up = slower start for a fade-in; as the automation curve law)
    const float dc = static_cast<float>(drag_.press.y - e.pos.y) * 0.01f / scale;
    doc::AudioFade fi = c.fadeIn, fo = c.fadeOut;
    if (in) fi.curve = std::clamp(c0.fadeIn.curve + dc, -1.0f, 1.0f);
    else fo.curve = std::clamp(c0.fadeOut.curve - dc, -1.0f, 1.0f);
    if (!(fi == c.fadeIn) || !(fo == c.fadeOut)) (void)apply(doc::ops::setAudioClipFades(p, ref, fi, fo), "Clip Fade", false);
    return;
  }
  const Ticks t = tickAt(e.pos.x);
  const doc::Track* track = doc::findTrackPtr(p, ref.track);
  if (!track) return;
  if (in) {
    // The clip ending exactly at this one's start (the crossfade's partner).
    const bool leftNeighbour = std::any_of(track->clips.begin(), track->clips.end(),
                                           [&](const doc::ClipSlot& o) { return o.id != c.id && o.audio && o.end() == c.start; });
    if (c0.crossfadeIn > 0 || (t < c.start && leftNeighbour)) {
      const Ticks x = std::clamp<Ticks>(2 * std::abs(t - c.start), 0, 2 * c.length);
      if (x != c.crossfadeIn) {
        doc::ops::EditResult r = doc::ops::setAudioClipCrossfade(p, ref, leftNeighbour ? x : 0);
        if (r) (void)apply(r, "Clip Fade", false);
      }
      return;
    }
    doc::AudioFade fi = c.fadeIn;
    fi.length = std::clamp<Ticks>(t - c.start, 0, c.length);
    if (!(fi == c.fadeIn)) (void)apply(doc::ops::setAudioClipFades(p, ref, fi, c.fadeOut), "Clip Fade", false);
    return;
  }
  // Fade-out; past the end onto the clip starting there: that clip's crossfade.
  const doc::ClipSlot* right = nullptr;
  for (const doc::ClipSlot& o : track->clips)
    if (o.id != c.id && o.audio && o.start() == doc::clipEnd(c)) right = &o;
  const Ticks cEnd = doc::clipEnd(c);
  if (right && t > cEnd) {
    const doc::ClipRef rr{ref.track, right->id};
    const Ticks x = std::clamp<Ticks>(2 * (t - cEnd), 0, 2 * c.length);
    if (x != right->audio->crossfadeIn) (void)apply(doc::ops::setAudioClipCrossfade(p, rr, x), "Clip Fade", false);
    if (c.fadeOut.length != 0) {
      doc::AudioFade fo = c.fadeOut;
      fo.length = 0;
      (void)apply(doc::ops::setAudioClipFades(*ctx_.doc(), ref, c.fadeIn, fo), "Clip Fade", false);
    }
    return;
  }
  if (right && right->audio->crossfadeIn > 0 && c0.fadeOut.length == 0 && t <= cEnd)
    (void)apply(doc::ops::setAudioClipCrossfade(p, {ref.track, right->id}, 0), "Clip Fade", false);
  const doc::ClipSlot* cur = findClip(*ctx_.doc(), ref);
  if (!cur || !cur->audio) return;
  doc::AudioFade fo = cur->audio->fadeOut;
  fo.length = std::clamp<Ticks>(cur->audio->start + cur->audio->length - t, 0, cur->audio->length);
  if (!(fo == cur->audio->fadeOut)) (void)apply(doc::ops::setAudioClipFades(*ctx_.doc(), ref, cur->audio->fadeIn, fo), "Clip Fade", false);
}

void Editor::dragTo(const PointerEvent& e) {
  const float scale = view_.tree() ? view_.tree()->scale() : 1.0f;
  if (!drag_.moved && !drag::beyondSlop(e.pos, drag_.press, scale)) return;  // shared click slop (M17)
  const bool first = !drag_.moved;
  drag_.moved = true;
  if (drag_.kind == DragKind::Fade || drag_.kind == DragKind::Gain) {
    audioDragTo(e);
    return;
  }
  if (drag_.kind == DragKind::Range) {
    updateRange(e.pos);
    return;
  }
  const Ticks raw = static_cast<Ticks>(std::llround(view_.tickAtX(e.pos.x) - view_.tickAtX(drag_.press.x)));
  const Grid g = grid();
  const Ticks dt = std::clamp(g.snapping() ? g.snapDelta(drag_.origin, raw) : raw, drag_.minDt, drag_.maxDt);
  int32_t drow = drag_.drow;
  bool copy = false;
  if (drag_.kind == DragKind::Move) {
    copy = (e.mods & mod::kAlt) != 0;
    const int32_t r = view_.rowAt(e.pos.y);
    if (r >= 0) {
      const auto rows = view_.rows();
      const int32_t cand = r - drag_.pressRow;
      const bool ok = std::all_of(drag_.clips.begin(), drag_.clips.end(), [&](const DragClip& c) {
        if (c.row < 0) return cand == 0;  // a clip of a hidden track only moves in time
        const int32_t to = c.row + cand;
        return to >= 0 && static_cast<size_t>(to) < rows.size() && !rows[static_cast<size_t>(to)].group;
      });
      if (ok) drow = cand;
    }
  }
  // The originals vanish (Move / Resize) or reappear (Copy toggled) and the time overlay hides: their columns.
  if (first || copy != drag_.copy) {
    double t0 = 0, t1 = 0;
    if (spanOf(sel_, t0, t1)) damageRows(sel_, t0, t1, false);
  }
  if (!first && dt == drag_.dt && drow == drag_.drow && copy == drag_.copy) return;
  drag_.dt = dt;
  drag_.drow = drow;
  drag_.copy = copy;
  damageDragPreview();
}

bool Editor::preview(const DragClip& c, int32_t& row, ClipGeom& g) const noexcept {
  if (c.row < 0 || !drag_.moved) return false;
  const doc::ClipSlot& slot = c.clip;
  row = c.row;
  g = {slot.start(), slot.length(), slot.clip ? slot.clip->contentStart : 0};
  if (drag_.kind == DragKind::Move) {
    row += drag_.drow;
    g.start += drag_.dt;
  } else if (drag_.kind == DragKind::Resize) {
    if (drag_.edge == doc::ops::ClipEdge::Start) {
      const Ticks end = slot.end();
      g.start = std::min(slot.start() + drag_.dt, end - 1);
      if (slot.clip) g.contentStart = contentStartAfter(*slot.clip, g.start);
      g.length = end - g.start;
    } else {
      g.length = std::max<Ticks>(1, slot.length() + drag_.dt);
    }
  } else {
    return false;
  }
  return row >= 0 && static_cast<size_t>(row) < view_.rows().size();
}

void Editor::damageDragPreview() {
  UiTree* tr = view_.tree();
  if (!tr) return;
  const IRect lv = view_.lanesViewport();
  std::vector<IRect> now;
  now.reserve(std::min(drag_.clips.size(), kMaxPreviewRects));
  IRect box{};
  for (const DragClip& c : drag_.clips) {
    int32_t row = 0;
    ClipGeom g;
    if (!preview(c, row, g)) continue;
    IRect r = clipBox(row, g.start, g.start + g.length);
    r = {r.x - 1, r.y, r.w + 2, r.h};
    if (drag_.clips.size() > kMaxPreviewRects) {
      box = box.empty() ? r : IRect{std::min(box.x, r.x), std::min(box.y, r.y),
                                    std::max(box.right(), r.right()) - std::min(box.x, r.x),
                                    std::max(box.bottom(), r.bottom()) - std::min(box.y, r.y)};
    } else {
      now.push_back(r);
    }
  }
  if (!box.empty()) now.push_back(box);
  for (const IRect& r : drag_.damage)
    if (const IRect v = intersect(r, lv); !v.empty()) tr->damage().add(v.toRect());
  for (const IRect& r : now)
    if (const IRect v = intersect(r, lv); !v.empty()) tr->damage().add(v.toRect());
  drag_.damage = std::move(now);
}

void Editor::updateRange(Vec2 p) {
  const auto rows = view_.rows();
  if (rows.empty()) return;
  const Ticks t = std::max<Ticks>(0, snapTick(tickAt(p.x)));
  const Ticks s = std::min(drag_.origin, t), e = std::max(drag_.origin, t);
  const int32_t r = clampRow(p.y);
  const int32_t anchor = std::clamp(drag_.anchorRow, 0, static_cast<int32_t>(rows.size() - 1));
  const int32_t r0 = std::min(anchor, r), r1 = std::max(anchor, r);
  Selection sel;
  sel.time = TickRange{s, e};
  sel.range = e > s;
  for (int32_t i = r0; i <= r1; ++i) sel.tracks.push_back(rows[static_cast<size_t>(i)].id);
  sel.clips = clipsMeeting(r0, r1, false, s, e, drag_.additive ? drag_.base : std::vector<doc::ClipRef>{});
  if (drag_.additive)
    for (const doc::ClipRef& c : drag_.base)
      if (std::find(sel.tracks.begin(), sel.tracks.end(), c.track) == sel.tracks.end()) sel.tracks.push_back(c.track);
  const bool sameClips = std::equal(sel.clips.begin(), sel.clips.end(), sel_.clips.begin(), sel_.clips.end(),
                                    [](const doc::ClipRef& a, const doc::ClipRef& b) { return a.clip == b.clip && a.track == b.track; });
  if (sameClips && sel.tracks == sel_.tracks && sel.time == sel_.time && sel.range == sel_.range) return;
  setSelection(std::move(sel), false);
}

void Editor::release(const PointerEvent& e) {
  if (drag_.kind == DragKind::None) return;
  if (drag_.kind == DragKind::Fade || drag_.kind == DragKind::Gain) {
    drag_ = {};
    if (ctx_.store().inGesture()) (void)ctx_.store().commitGesture();
    return;
  }
  if (!drag_.moved) {
    const Drag d = std::move(drag_);
    drag_ = {};
    if (d.collapseOnClick && !(e.mods & (mod::kShift | mod::kCtrl))) setSelection(selectionOfClips({d.pressed}), true);
    return;
  }
  if (drag_.kind == DragKind::Range) {
    drag_ = {};
    return;
  }
  commitDrag();
}

void Editor::cancelDrag() {
  if (drag_.kind == DragKind::None) return;
  if (drag_.kind == DragKind::Fade || drag_.kind == DragKind::Gain) {  // Esc: back to where the gesture began
    drag_ = {};
    if (ctx_.store().inGesture()) (void)ctx_.store().cancelGesture();
    return;
  }
  const bool clipDrag = drag_.moved && drag_.kind != DragKind::Range;
  std::vector<IRect> rects = std::move(drag_.damage);
  drag_ = {};
  if (!clipDrag) return;
  if (UiTree* tr = view_.tree()) {
    const IRect lv = view_.lanesViewport();
    for (const IRect& r : rects)
      if (const IRect v = intersect(r, lv); !v.empty()) tr->damage().add(v.toRect());
  }
  double t0 = 0, t1 = 0;
  if (spanOf(sel_, t0, t1)) damageRows(sel_, t0, t1, false);  // the originals and the time overlay come back
}

void Editor::abortDrag() {
  const bool range = drag_.kind == DragKind::Range;
  cancelDrag();
  if (range) setSelection(pressSel_, false);
}

void Editor::commitDrag() {
  const Drag d = drag_;
  cancelDrag();  // the preview goes; the document change (if any) repaints the lanes
  const doc::Project& p = *ctx_.doc();
  doc::IdSource& ids = ctx_.store().ids();
  const auto rows = view_.rows();
  if (d.kind == DragKind::Move) {
    if (d.dt == 0 && d.drow == 0) return;  // also for a copy: a copy onto itself would overwrite the original
    std::vector<doc::ops::ClipMove> moves;
    moves.reserve(d.clips.size());
    for (const DragClip& c : d.clips) {
      doc::ops::ClipMove m;
      m.clip = c.ref;
      m.start = c.clip.start() + d.dt;
      if (d.drow != 0 && c.row >= 0) m.toTrack = rows[static_cast<size_t>(c.row + d.drow)].id;
      moves.push_back(m);
    }
    doc::ops::MoveClipsOptions o;
    o.copy = d.copy;
    (void)apply(doc::ops::moveClips(p, ids, moves, o), d.copy ? "Copy" : "Move", true);
    return;
  }
  if (d.kind == DragKind::Resize && d.dt != 0) {
    std::vector<doc::ClipRef> refs;
    refs.reserve(d.clips.size());
    for (const DragClip& c : d.clips) refs.push_back(c.ref);
    (void)apply(doc::ops::resizeClips(p, ids, refs, d.edge, d.dt), "Resize", true);
  }
}

// --- edits --------------------------------------------------------------------------------------------------

bool Editor::dispatch(doc::Batch batch, std::string_view what) {
  if (batch.commands.empty()) return false;
  if (Status s = ctx_.store().dispatch(doc::Command(std::move(batch))); !s) {
    ctx_.setStatus(std::string(what) + ": " + s.error().message);
    return false;
  }
  return true;
}

bool Editor::apply(const doc::ops::EditResult& e, std::string_view what, bool adoptClips) {
  if (!e) {
    ctx_.setStatus(std::string(what) + ": " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status s = ctx_.store().dispatch(e->command); !s) {
    ctx_.setStatus(std::string(what) + ": " + s.error().message);
    return false;
  }
  if (adoptClips) setSelection(selectionOfClips(e->clips), true);
  return true;
}

bool Editor::needRange(std::string_view what) const {
  if (sel_.time && sel_.time->end > sel_.time->start) return true;
  ctx_.setStatus(std::string(what) + ": select a time range first");
  return false;
}

std::vector<doc::TrackId> Editor::midiTracks() const {
  std::vector<doc::TrackId> out;
  const doc::Project& p = *ctx_.doc();
  for (doc::TrackId id : sel_.tracks)
    if (const doc::Track* t = doc::findTrackPtr(p, id)) collectMidi(*t, out);
  return out;
}

std::vector<doc::TrackId> Editor::clipTracks() const {
  std::vector<doc::TrackId> out;
  const doc::Project& p = *ctx_.doc();
  for (doc::TrackId id : sel_.tracks)
    if (const doc::Track* t = doc::findTrackPtr(p, id)) collectMidi(*t, out, true);
  return out;
}

std::vector<int32_t> Editor::midiRows() const {
  std::vector<int32_t> out;
  const auto rows = view_.rows();
  for (size_t i = 0; i < rows.size(); ++i)
    if (!rows[i].group) out.push_back(static_cast<int32_t>(i));
  return out;
}

bool Editor::deleteSelection() {
  doc::IdSource& ids = ctx_.store().ids();
  if (sel_.range && sel_.time && sel_.time->end > sel_.time->start) {
    Chain ch(ctx_.doc(), "Delete");
    for (doc::TrackId t : clipTracks()) carve(ch, ids, t, sel_.time->start, sel_.time->end);
    if (!ch.error.empty()) {
      ctx_.setStatus("Delete: " + ch.error);
      return false;
    }
    if (!dispatch(std::move(ch.batch), "Delete")) return false;
    Selection s = sel_;
    s.clips.clear();
    setSelection(std::move(s), false);
    return true;
  }
  // T3.10: tracks selected by their headers (no clips, no time selection or insert marker): delete the tracks.
  if (sel_.clips.empty() && !sel_.time && !sel_.tracks.empty()) return deleteTracks();
  if (sel_.clips.empty()) return false;
  if (!apply(doc::ops::deleteClips(*ctx_.doc(), sel_.clips), "Delete", false)) return false;
  Selection s;
  s.tracks = sel_.tracks;
  if (sel_.time) s.time = TickRange{sel_.time->start, sel_.time->start};
  setSelection(std::move(s), false);
  return true;
}

// Whole selected clips, or (a range) the parts of the selected tracks' clips inside it, cut on a scratch snapshot.
// Row offsets count MIDI rows from the first selected one; clips of tracks that are not rows are left out.
bool Editor::fillClipboard(Clipboard& out) const {
  out = {};
  const std::vector<int32_t> mrows = midiRows();
  const auto rows = view_.rows();
  auto midiIndex = [&](doc::TrackId id) -> int32_t {
    for (size_t i = 0; i < mrows.size(); ++i)
      if (rows[static_cast<size_t>(mrows[i])].id == id) return static_cast<int32_t>(i);
    return -1;
  };
  struct Src {
    int32_t index;
    doc::ClipSlot clip;
  };
  std::vector<Src> src;
  Ticks origin = 0;
  if (sel_.range && sel_.time && sel_.time->end > sel_.time->start) {
    const Ticks s = sel_.time->start, e = sel_.time->end;
    Chain ch(ctx_.doc(), "Copy");
    const std::vector<doc::TrackId> tracks = clipTracks();
    std::vector<doc::ClipRef> crossing;
    for (doc::TrackId id : tracks)
      if (const doc::Track* t = doc::findTrackPtr(*ch.snap, id))
        for (const doc::ClipSlot& c : t->clips)
          if ((c.start() < s && c.end() > s) || (c.start() < e && c.end() > e)) crossing.push_back({id, c.id});
    const Ticks cuts[] = {s, e};
    if (!crossing.empty() && !ch.run(doc::ops::splitClips(*ch.snap, ctx_.store().ids(), crossing, cuts))) return false;
    for (doc::TrackId id : tracks) {
      const int32_t mi = midiIndex(id);
      const doc::Track* t = doc::findTrackPtr(*ch.snap, id);
      if (mi < 0 || !t) continue;
      for (const doc::ClipSlot& c : t->clips)
        if (c.start() >= s && c.end() <= e) src.push_back({mi, c});
    }
    origin = s;
    out.length = e - s;
    // Offsets from the first selected MIDI row, even if it has no clip in the range.
    int32_t first = INT32_MAX;
    for (doc::TrackId id : tracks)
      if (const int32_t mi = midiIndex(id); mi >= 0) first = std::min(first, mi);
    for (Src& x : src) x.index -= first;
  } else {
    if (sel_.clips.empty()) return false;
    const doc::Project& p = *ctx_.doc();
    Ticks lo = std::numeric_limits<Ticks>::max(), hi = std::numeric_limits<Ticks>::min();
    int32_t first = INT32_MAX;
    for (const doc::ClipRef& r : sel_.clips) {
      const doc::Track* t = doc::findTrackPtr(p, r.track);
      const int32_t i = t ? doc::findClipIndex(*t, r.clip) : -1;
      const int32_t mi = midiIndex(r.track);
      if (i < 0 || mi < 0) continue;
      const doc::ClipSlot& c = t->clips[static_cast<size_t>(i)];
      src.push_back({mi, c});
      lo = std::min(lo, c.start());
      hi = std::max(hi, c.end());
      first = std::min(first, mi);
    }
    if (src.empty()) return false;
    origin = lo;
    out.length = hi - lo;
    for (Src& x : src) x.index -= first;
  }
  for (const Src& x : src) {
    doc::ClipSlot c = x.clip;
    if (c.audio) {  // the start relative to the origin; pasting re-derives a warp-Off clip's length at its new start
      auto a = std::make_shared<doc::AudioClip>(*c.audio);
      a->start -= origin;
      c.audio = std::move(a);
    } else {
      auto n = std::make_shared<doc::NoteClip>(doc::withNotes(*c.clip, c.clip->notes));
      n->start -= origin;
      c.clip = std::move(n);
    }
    out.clips.push_back({static_cast<uint32_t>(x.index), std::move(c)});
  }
  return !out.clips.empty() || out.length > 0;
}

bool Editor::copySelection() {
  Clipboard cb;
  if (!fillClipboard(cb) || cb.clips.empty()) return false;
  clipboard_ = std::move(cb);
  return true;
}

bool Editor::cutSelection() {
  if (!copySelection()) return false;
  return deleteSelection();
}

// Pastes `cb` at `at`, its first row on MIDI row `firstMidi` (rows beyond the last MIDI row are dropped); what lies
// under the pasted clips is overwritten. Selects the pasted clips over [at, at + length).
bool Editor::pasteAt(const Clipboard& cb, Ticks at, int32_t firstMidi, std::string_view what, bool selectRange) {
  const std::vector<int32_t> mrows = midiRows();
  const auto rows = view_.rows();
  doc::IdSource& ids = ctx_.store().ids();
  Chain ch(ctx_.doc(), std::string(what));
  std::vector<doc::ClipRef> made;
  size_t dropped = 0;
  for (const ClipboardEntry& e : cb.clips) {
    const size_t mi = static_cast<size_t>(firstMidi) + e.rowOffset;
    if (firstMidi < 0 || mi >= mrows.size()) {
      ++dropped;
      continue;
    }
    const ArrangementRow& row = rows[static_cast<size_t>(mrows[mi])];
    const doc::TrackId track = row.id;
    // T5.6: a clip goes only onto a track of its kind (note clips: MIDI, audio clips: audio).
    if ((e.clip.audio != nullptr) != (row.track->kind == doc::TrackKind::Audio)) {
      ++dropped;
      continue;
    }
    const Ticks start = at + e.clip.start();
    doc::ClipSlot c = cloneFresh(e.clip, ids, start, *ch.snap->timeline);
    if (!carve(ch, ids, track, start, start + c.length())) break;
    const doc::ClipRef ref{track, c.id};
    if (!ch.run(doc::AddClip{track, c.clip, c.audio})) break;
    made.push_back(ref);
  }
  if (!ch.error.empty()) {
    ctx_.setStatus(std::string(what) + ": " + ch.error);
    return false;
  }
  if (dropped) ctx_.setStatus(std::string(what) + ": " + std::to_string(dropped) + " clip(s) had no track of their kind to go to");
  if (!dispatch(std::move(ch.batch), what)) return false;
  Selection s = selectionOfClips(made);
  s.time = TickRange{at, at + cb.length};
  if (selectRange) {  // a duplicated time selection: the same tracks, the range after it
    s.range = cb.length > 0;
    s.tracks = sel_.tracks;
  }
  setSelection(std::move(s), true);
  return true;
}

bool Editor::paste() {
  if (clipboard_.clips.empty()) return false;
  if (!sel_.time || sel_.tracks.empty()) {
    ctx_.setStatus("Paste: click a track to choose where");
    return false;
  }
  // The first MIDI row at or below the first selected track.
  const std::vector<int32_t> mrows = midiRows();
  const int32_t row = rowOf(sel_.tracks.front());
  int32_t first = -1;
  for (size_t i = 0; i < mrows.size() && row >= 0; ++i)
    if (mrows[i] >= row) {
      first = static_cast<int32_t>(i);
      break;
    }
  if (first < 0) {
    ctx_.setStatus("Paste: no clip track there");
    return false;
  }
  // Pasting again right after a paste goes after it (the pasted clips are still selected), like the clip view.
  const Ticks at = (lastPaste_ && sel_.time == lastPaste_ && !sel_.range) ? sel_.time->end : sel_.time->start;
  if (!pasteAt(clipboard_, at, first, "Paste", false)) return false;
  lastPaste_ = sel_.time;
  return true;
}

bool Editor::duplicateSelection() {
  if (sel_.range && sel_.time && sel_.time->end > sel_.time->start) {
    // A time selection: its material on the selected tracks goes right after it, and the selection follows.
    Clipboard cb;
    if (!fillClipboard(cb)) return false;
    const std::vector<int32_t> mrows = midiRows();
    int32_t first = -1;
    for (doc::TrackId t : sel_.tracks) {
      const int32_t row = rowOf(t);
      for (size_t i = 0; i < mrows.size() && row >= 0; ++i)
        if (mrows[i] >= row) {
          if (first < 0 || static_cast<int32_t>(i) < first) first = static_cast<int32_t>(i);
          break;
        }
    }
    const Ticks end = sel_.time->end;
    if (cb.clips.empty()) {
      Selection s = sel_;
      s.time = TickRange{end, end + cb.length};
      s.clips.clear();
      setSelection(std::move(s), false);
      return false;
    }
    return pasteAt(cb, end, first, "Duplicate", true);
  }
  if (sel_.clips.empty()) return false;
  return apply(doc::ops::duplicateClips(*ctx_.doc(), ctx_.store().ids(), sel_.clips), "Duplicate", true);
}

bool Editor::splitSelection() {
  if (!sel_.time) return false;
  std::vector<Ticks> at;
  if (sel_.range && sel_.time->end > sel_.time->start) at = {sel_.time->start, sel_.time->end};
  else if (sel_.time->end == sel_.time->start) at = {sel_.time->start};
  else return false;  // whole clips selected: nothing to cut at (Live)
  const doc::Project& p = *ctx_.doc();
  std::vector<doc::ClipRef> clips;
  for (doc::TrackId id : clipTracks())
    if (const doc::Track* t = doc::findTrackPtr(p, id))
      for (const doc::ClipSlot& c : t->clips)
        if (std::any_of(at.begin(), at.end(), [&](Ticks x) { return c.start() < x && c.end() > x; }))
          clips.push_back({id, c.id});
  if (clips.empty()) return false;
  const doc::ops::EditResult e = doc::ops::splitClips(p, ctx_.store().ids(), clips, at);
  if (!apply(e, "Split", false)) return false;
  Selection s = sel_;
  s.clips.clear();
  if (s.range) {
    const doc::Project& q = *ctx_.doc();
    for (const doc::ClipRef& r : e->clips)
      if (const doc::ClipSlot* c = findClip(q, r); c && c->start() >= s.time->start && c->end() <= s.time->end)
        s.clips.push_back(r);
  }
  setSelection(std::move(s), false);
  return true;
}

bool Editor::consolidateSelection() {
  if (sel_.range && sel_.time && sel_.time->end > sel_.time->start) {
    const Ticks s = sel_.time->start, e = sel_.time->end;
    doc::IdSource& ids = ctx_.store().ids();
    Chain ch(ctx_.doc(), "Consolidate");
    const std::vector<doc::TrackId> tracks = midiTracks();
    std::vector<doc::ClipRef> crossing;
    for (doc::TrackId id : tracks)
      if (const doc::Track* t = doc::findTrackPtr(*ch.snap, id))
        for (const doc::ClipSlot& c : t->clips)
          if ((c.start() < s && c.end() > s) || (c.start() < e && c.end() > e)) crossing.push_back({id, c.id});
    const Ticks cuts[] = {s, e};
    if (!crossing.empty()) (void)ch.run(doc::ops::splitClips(*ch.snap, ids, crossing, cuts));
    std::vector<doc::ClipRef> inside;
    for (doc::TrackId id : tracks)
      if (const doc::Track* t = doc::findTrackPtr(*ch.snap, id))
        for (const doc::ClipSlot& c : t->clips)
          if (c.start() >= s && c.end() <= e) inside.push_back({id, c.id});
    if (inside.empty()) return false;
    const doc::ops::EditResult r = doc::ops::consolidateClips(*ch.snap, ids, inside);
    (void)ch.run(r);
    if (!ch.error.empty()) {
      ctx_.setStatus("Consolidate: " + ch.error);
      return false;
    }
    if (!dispatch(std::move(ch.batch), "Consolidate")) return false;
    setSelection(selectionOfClips(r->clips), true);
    return true;
  }
  if (sel_.clips.empty()) return false;
  return apply(doc::ops::consolidateClips(*ctx_.doc(), ctx_.store().ids(), sel_.clips), "Consolidate", true);
}

bool Editor::insertMidiClip() {
  const std::vector<doc::TrackId> tracks = midiTracks();
  if (tracks.empty() || !sel_.time) {
    ctx_.setStatus("Insert MIDI Clip: select a MIDI track first");
    return false;
  }
  const Ticks s = sel_.time->start;
  Ticks e = sel_.time->end;
  if (e <= s) e = s + ticksPerBar(ctx_.doc()->timeline->sigMap.signatureAt(s));  // at the insert marker: one bar
  doc::IdSource& ids = ctx_.store().ids();
  Chain ch(ctx_.doc(), "Insert MIDI Clip");
  std::vector<doc::ClipRef> made;
  for (doc::TrackId t : tracks) {
    const doc::ops::EditResult r = doc::ops::insertEmptyClip(*ch.snap, ids, t, s, e);
    if (!ch.run(r)) break;
    made.insert(made.end(), r->clips.begin(), r->clips.end());
  }
  if (!ch.error.empty()) {
    ctx_.setStatus("Insert MIDI Clip: " + ch.error);
    return false;
  }
  if (!dispatch(std::move(ch.batch), "Insert MIDI Clip")) return false;
  setSelection(selectionOfClips(made), true);
  return true;
}

bool Editor::insertTime() {
  if (!needRange("Insert Time")) return false;
  const TickRange r = *sel_.time;
  if (!apply(doc::ops::insertTime(*ctx_.doc(), ctx_.store().ids(), r.start, r.end - r.start), "Insert Time", false))
    return false;
  Selection s = sel_;
  s.clips.clear();
  s.range = true;
  setSelection(std::move(s), false);
  return true;
}

bool Editor::duplicateTime() {
  if (!needRange("Duplicate Time")) return false;
  const TickRange r = *sel_.time;
  if (!apply(doc::ops::duplicateTime(*ctx_.doc(), ctx_.store().ids(), r.start, r.end), "Duplicate Time", false)) return false;
  Selection s = sel_;
  s.clips.clear();
  s.range = true;
  s.time = TickRange{r.end, r.end + (r.end - r.start)};  // the copy is selected (Live)
  setSelection(std::move(s), false);
  return true;
}

bool Editor::deleteTime() {
  if (!needRange("Delete Time")) return false;
  const TickRange r = *sel_.time;
  if (!apply(doc::ops::deleteTime(*ctx_.doc(), ctx_.store().ids(), r.start, r.end), "Delete Time", false)) return false;
  Selection s = sel_;
  s.clips.clear();
  s.range = false;
  s.time = TickRange{r.start, r.start};
  setSelection(std::move(s), false);
  return true;
}

bool Editor::loopSelection() {
  if (!needRange("Loop Selection")) return false;
  return apply(doc::ops::loopArrangementToSelection(*ctx_.doc(), sel_.time->start, sel_.time->end), "Loop Selection", false);
}

// `0` (Q15): the selected clips are deactivated if any is active, else reactivated; with no clip selected, the
// selected tracks' activators toggle the same way. A track's activator is its mute (T3.10, Q15's answer: a Param,
// no recompile), like the activator button in its header.
bool Editor::toggleActive() {
  const doc::Project& p = *ctx_.doc();
  if (!sel_.clips.empty()) {
    bool anyActive = false;
    for (const doc::ClipRef& r : sel_.clips)
      if (const doc::ClipSlot* c = findClip(p, r)) anyActive = anyActive || c->active();
    doc::Batch b;
    b.label = anyActive ? "Deactivate Clips" : "Activate Clips";
    for (const doc::ClipRef& r : sel_.clips) b.commands.push_back(doc::SetClipActive{r, !anyActive});
    return dispatch(std::move(b), "Activate");
  }
  if (sel_.tracks.empty()) return false;
  return apply(doc::ops::toggleTracksMute(p, sel_.tracks), "Track Activator", false);
}

void Editor::selectAll() {
  std::vector<doc::ClipRef> all;
  doc::forEachTrack(*ctx_.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (t.kind != doc::TrackKind::Midi && t.kind != doc::TrackKind::Audio) return;
    for (const doc::ClipSlot& c : t.clips) all.push_back({t.id, c.id});
  });
  Selection s = selectionOfClips(std::move(all));
  s.tracks.clear();
  for (const ArrangementRow& r : view_.rows()) s.tracks.push_back(r.id);
  setSelection(std::move(s), false);
}

// --- the right-click menu -----------------------------------------------------------------------------------

Menu* Editor::openContextMenu(Vec2 at) {
  auto action = [](std::string label, std::string id, std::string keys) {
    MenuItem m = MenuItem::item(std::move(label), {}, std::move(keys));
    m.action = std::move(id);
    return m;
  };
  const GridSpec g = ctx_.grid();
  ViewContext& ctx = ctx_;
  std::vector<MenuItem> items;
  items.push_back(action("Cut", "arrangement.cut", "Ctrl+X"));
  items.push_back(action("Copy", "arrangement.copy", "Ctrl+C"));
  items.push_back(action("Paste", "arrangement.paste", "Ctrl+V"));
  items.push_back(action("Duplicate", "arrangement.duplicate", "Ctrl+D"));
  items.push_back(action("Delete", "arrangement.delete", "Del"));
  items.push_back(MenuItem::sep());
  items.push_back(action("Split", "arrangement.split", "Ctrl+E"));
  items.push_back(action("Consolidate", "arrangement.consolidate", "Ctrl+J"));
  items.push_back(action("Insert MIDI Clip", "arrangement.insertMidiClip", "Ctrl+Shift+M"));
  items.push_back(action("Loop Selection", "arrangement.loopSelection", "Ctrl+L"));
  items.push_back(action("Deactivate / Activate", "arrangement.toggleActive", "0"));
  items.push_back(MenuItem::sep());
  items.push_back(action("Insert Time", "arrangement.insertTime", "Ctrl+I"));
  items.push_back(action("Duplicate Time", "arrangement.duplicateTime", "Ctrl+Shift+D"));
  items.push_back(action("Delete Time", "arrangement.deleteTime", "Ctrl+Shift+Del"));
  items.push_back(MenuItem::sep());
  std::vector<MenuItem> adaptive;
  for (int l = kGridLevelMin; l <= kGridLevelMax; ++l) {
    MenuItem m = MenuItem::item(adaptiveLevelName(l), [&ctx, l] {
      GridSpec s = ctx.grid();
      s.mode = GridMode::Adaptive;
      s.level = static_cast<int8_t>(l);
      s.off = false;
      ctx.setGrid(s);
    });
    m.checked = !g.off && g.mode == GridMode::Adaptive && g.level == l;
    adaptive.push_back(std::move(m));
  }
  MenuItem ad = MenuItem::sub("Adaptive Grid", std::move(adaptive));
  ad.checked = !g.off && g.mode == GridMode::Adaptive;
  items.push_back(std::move(ad));
  std::vector<MenuItem> fixed;
  for (int x = kGridFixedExpMin; x <= kGridFixedExpMax; ++x) {
    MenuItem m = MenuItem::item(fixedValueName(x), [&ctx, x] {
      GridSpec s = ctx.grid();
      s.mode = GridMode::Fixed;
      s.fixedExp = static_cast<int8_t>(x);
      s.off = false;
      ctx.setGrid(s);
    });
    m.checked = !g.off && g.mode == GridMode::Fixed && g.fixedExp == x;
    fixed.push_back(std::move(m));
  }
  MenuItem fx = MenuItem::sub("Fixed Grid", std::move(fixed));
  fx.checked = !g.off && g.mode == GridMode::Fixed;
  items.push_back(std::move(fx));
  items.push_back(MenuItem::sep());
  constexpr std::pair<uint8_t, const char*> kTuplets[] = {
      {1, "Straight"}, {3, "Triplet"}, {5, "5-Tuplet"}, {7, "7-Tuplet"}, {11, "11-Tuplet"}};
  for (const auto& [n, label] : kTuplets) {
    MenuItem m = MenuItem::item(label, [&ctx, n] {
      GridSpec s = ctx.grid();
      s.tuplet = n;
      s.off = false;
      ctx.setGrid(s);
    }, n == 3 ? "Ctrl+3" : "");
    m.checked = g.tuplet == n;
    items.push_back(std::move(m));
  }
  items.push_back(MenuItem::sep());
  MenuItem snap = MenuItem::item("Snap to Grid", [&ctx] { ctx.setGrid(ctx.grid().snapToggled()); }, "Ctrl+4");
  snap.checked = g.snap;
  items.push_back(std::move(snap));
  MenuItem off = MenuItem::item("Off", [&ctx] {
    GridSpec s = ctx.grid();
    s.off = !s.off;
    ctx.setGrid(s);
  });
  off.checked = g.off;
  items.push_back(std::move(off));
  return ui::openContextMenu(view_, at, std::move(items));
}

}  // namespace pq::views::arrangement
