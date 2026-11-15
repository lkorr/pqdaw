// bug-roll-playhead: the transport's playhead in the clip view (clip time) and the ruler as its scrub area; Follow.
// The polling is the arrangement's (ArrangementView::schedulePlayhead): read the port only while playing and shown,
// wake when the line will have moved a device pixel, damage only its old and new columns.

#include "roll_playhead.h"

#include <algorithm>
#include <cmath>

#include "pq/document/clip_time.h"
#include "pq/ui/theme.h"
#include "pq/views/piano_roll/clip_view.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

std::optional<Ticks> contentAtSong(const doc::NoteClip& c, Ticks song) noexcept {
  if (song < c.start || song >= c.start + c.length) return std::nullopt;
  return doc::clipContentAt(c, song - c.start);
}

Ticks songAtContent(const doc::NoteClip& c, Ticks content, std::optional<Ticks> playing) noexcept {
  const Ticks length = std::max<Ticks>(1, c.length);
  if (!doc::clipLooped(c)) return c.start + std::clamp<Ticks>(content - c.contentStart, 0, length - 1);
  const Ticks c0 = doc::clipContentAt(c, 0);  // the start marker, folded into the loop
  const Ticks ls = c.loop.start, le = c.loop.end, loopLen = le - ls;
  const Ticks first = le - c0;  // the first pass: content [c0, loop end)
  content = std::clamp<Ticks>(content, std::min(c0, ls), le - 1);
  const bool inFirst = content >= c0, inLoop = content >= ls;
  if (playing && *playing >= c.start && *playing < c.start + length) {
    const Ticks o = *playing - c.start;
    if (o < first) {
      if (inFirst) return c.start + (content - c0);
    } else if (inLoop) {
      return c.start + first + (o - first) / loopLen * loopLen + (content - ls);
    }
  }
  if (inFirst) return c.start + (content - c0);
  return c.start + first + (content - ls);  // before the start marker, inside the loop: the first repeat
}

void paintPlayhead(PaintContext& ctx, const RollState& s, const IRect& r) {
  if (!s.playhead || !ctx.res.theme) return;
  const float x = std::floor(s.xOfTick(static_cast<double>(*s.playhead)));
  const float w = static_cast<float>(std::max(1, ctx.scale.px(1.0f)));
  if (x + w <= static_cast<float>(r.x) || x >= static_cast<float>(r.right())) return;
  ctx.dl.addRectFilled({x, static_cast<float>(r.y), x + w, static_cast<float>(r.bottom())}, ctx.res.theme->arrangement.playhead);
}

Ticks scrubAt(RollState& s, double contentTick, Mods mods, Ticks last) {
  const doc::NoteClip* c = s.clip();
  if (!c) return last;
  Ticks t = static_cast<Ticks>(std::floor(contentTick));
  const Grid g = s.grid();
  if (!(mods & mod::kAlt) && g.snapping()) t = g.snap(t);
  const PlayheadSnapshot ph = s.ctx.transport().playhead();
  const Ticks song = std::max<Ticks>(0, songAtContent(*c, t, ph.playing ? std::optional<Ticks>(ph.tick) : std::nullopt));
  if (song == last) return last;
  s.ctx.playFrom(song);
  return song;
}

}  // namespace pq::views::roll

namespace pq::views {

using namespace pq::ui;
using roll::RollState;

std::optional<Ticks> ClipView::playheadTick() const noexcept { return state_->playhead; }

bool ClipView::shownNow() const noexcept {
  if (!tree()) return false;
  for (const Widget* w = this; w != nullptr; w = w->parent())
    if (!w->visible()) return false;
  return true;
}

// On transport notifications (play / stop, a jump, the start marker, Follow), a new clip, the detail view shown or
// hidden, a document change while one is drawn: the only reads while stopped; none while hidden.
void ClipView::refreshPlayhead() {
  RollState& s = *state_;
  PlayheadSnapshot ph{};
  const bool on = shownNow() && s.hasClip();
  if (on) {
    ++playheadReads_;
    ph = ctx_.transport().playhead();
  }
  if (!on || !ph.playing) {
    showPlayhead(std::nullopt);
    if (playheadTimer_) {
      cancelTimer(playheadTimer_);
      playheadTimer_ = 0;
    }
    return;
  }
  showPlayhead(roll::contentAtSong(*s.clip(), ph.tick));
  if (!playheadTimer_) schedulePlayhead(ph.tick);
}

void ClipView::onTimer(uint32_t id) {
  if (id != playheadTimer_) return;
  playheadTimer_ = 0;
  readPlayhead();
}

void ClipView::readPlayhead() {
  RollState& s = *state_;
  if (!shownNow() || !s.hasClip()) {
    refreshPlayhead();
    return;
  }
  ++playheadReads_;
  const PlayheadSnapshot ph = ctx_.transport().playhead();
  if (!ph.playing) {
    refreshPlayhead();
    return;
  }
  const std::optional<Ticks> c = roll::contentAtSong(*s.clip(), ph.tick);
  const IRect a = s.area();
  auto inView = [&](Ticks t) {
    const float x = s.xOfTick(static_cast<double>(t));
    return x >= static_cast<float>(a.x) && x < static_cast<float>(a.right() - 1);
  };
  // Follow (the arrangement's switch): the roll pages, the playhead to its left edge, when the playhead leaves the
  // view or enters the clip outside it — not while a gesture runs, nor when the user scrolled away from it (the last
  // position drawn is out of view then too).
  const bool wasIn = !s.playhead || inView(*s.playhead);
  if (c && a.w > 0 && ctx_.follow() && wasIn && s.drag.kind == roll::DragKind::None && !inView(*c)) {
    s.playhead = c;  // the scroll repaints everything: no column damage
    s.scrollToTick(static_cast<double>(*c));
  } else {
    showPlayhead(c);
  }
  schedulePlayhead(ph.tick);
}

// While playing, wake when the line will have moved about one device px (from the tempo there), bounded to one frame at
// 144 Hz … 100 ms; before the clip, when it reaches the clip's start.
void ClipView::schedulePlayhead(Ticks song) {
  const RollState& s = *state_;
  const double bpm = ctx_.doc()->timeline->tempoMap.bpmAt(song);
  const double ticksPerSecond = std::max(1e-9, bpm / 60.0 * static_cast<double>(kPpq));
  double wait = 1.0 / std::max(1e-12, s.pxPerTick() * ticksPerSecond);
  if (const doc::NoteClip* c = s.clip(); c && song < c->start) wait = static_cast<double>(c->start - song) / ticksPerSecond;
  else if (c && song >= c->start + c->length) wait = 0.1;
  playheadTimer_ = requestTimer(std::clamp(wait, 1.0 / 144.0, 0.1));
}

void ClipView::showPlayhead(std::optional<Ticks> content) {
  RollState& s = *state_;
  if (content == s.playhead) return;
  const std::optional<Ticks> old = s.playhead;
  s.playhead = content;
  if (old && content && std::floor(s.xOfTick(static_cast<double>(*old))) == std::floor(s.xOfTick(static_cast<double>(*content)))) return;
  if (old) damagePlayhead(*old);
  if (content) damagePlayhead(*content);
}

// The line's pixels (plus one either side) from the ruler's top down through the velocity lane, over the note area's
// columns.
void ClipView::damagePlayhead(Ticks content) {
  UiTree* tr = tree();
  const RollState& s = *state_;
  if (!tr || !s.rulerView) return;
  const IRect a = s.area();
  const float x = std::floor(s.xOfTick(static_cast<double>(content)));
  const int32_t w = std::max(1, tr->uiScale().px(1.0f));
  if (a.w <= 0 || x + static_cast<float>(w) < static_cast<float>(a.x) || x >= static_cast<float>(a.right())) return;
  const int32_t top = s.rulerView->rect().y;
  const int32_t bottom = s.laneView && s.laneView->visible() ? s.laneView->rect().bottom() : a.bottom();
  const IRect col = intersect({static_cast<int32_t>(x) - 1, top, w + 2, bottom - top}, {a.x, top, a.w, bottom - top});
  if (!col.empty()) tr->damage().add(col.toRect());
}

}  // namespace pq::views
