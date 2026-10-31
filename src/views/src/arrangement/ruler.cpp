// The arrangement's ruler (T3.8): loop brace, bar numbers and ticks, locators, tempo / time-signature markers, the
// start marker and the playhead. A click locates; a drag zooms (vertical) and scrolls (horizontal) around the
// pressed tick, like Live's beat-time ruler. T4.16b: the loop brace's ends, the brace itself and the locators drag
// (relative to the grid; previewed in the ruler, one SetLoop / UpdateLocator on release; a click on a locator jumps
// there); the cursor shows which.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

#include "internal.h"
#include "pq/document/commands.h"

namespace pq::views::arrangement {

using namespace pq::ui;
namespace am = arrangement_metrics;

namespace {

constexpr float kClickSlop = 3.0f;  // logical px a press may move and still count as a click
constexpr double kDragZoomPerPx = 0.01;  // zoom ×e per 100 logical px of vertical drag (down = in, as in Live)

// "133.25" (tempo marker text): up to 2 decimals, trailing zeros dropped.
std::string_view formatBpm(double bpm, std::span<char> buf) {
  const int n = std::snprintf(buf.data(), buf.size(), "%.2f", bpm);
  std::string_view s(buf.data(), n > 0 ? static_cast<size_t>(n) : 0);
  while (!s.empty() && s.back() == '0') s.remove_suffix(1);
  if (!s.empty() && s.back() == '.') s.remove_suffix(1);
  return s;
}

}  // namespace

Ruler::Ruler(ArrangementView& view) : Control("ruler"), view_(view) {
  setInfo("Beat Time Ruler",
          "Click to set the start marker and jump there (snapped to the grid). Drag up or down to zoom around the "
          "pressed position, left or right to scroll. Drag the loop brace or its ends to move or resize the loop, a "
          "locator to move it. Ctrl+wheel zooms, the wheel scrolls. Z zooms to the selection, X zooms back.");
}

Ruler::Hit Ruler::hitAt(Vec2 p) const {
  Hit h;
  const doc::Project* proj = view_.shown().get();
  if (!proj) return h;
  const doc::Project& pr = *proj;
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  const float loopY = static_cast<float>(rect().y + s.px(am::kRulerNumberRow));
  const float markY = loopY + static_cast<float>(s.px(am::kRulerLoopRow));
  const float grab = static_cast<float>(s.px(4.0f));
  if (p.y >= loopY && p.y < markY) {
    h.grab = Grab::LoopNew;  // T4.17: off the brace, a drag draws a new loop
    if (pr.loop.end <= pr.loop.start) return h;
    const float lx0 = std::round(view_.xOfTick(static_cast<double>(pr.loop.start)));
    const float lx1 = std::round(view_.xOfTick(static_cast<double>(pr.loop.end)));
    // The nearer end wins when the brace is narrow; a brace too narrow for a body is all ends.
    if (std::fabs(p.x - lx0) <= grab && std::fabs(p.x - lx0) <= std::fabs(p.x - lx1)) h.grab = Grab::LoopStart;
    else if (std::fabs(p.x - lx1) <= grab) h.grab = Grab::LoopEnd;
    else if (p.x > lx0 && p.x < lx1) h.grab = Grab::Loop;
    return h;
  }
  if (p.y >= markY && pr.locators) {
    // The flag drawn last (the latest at or before the pointer) wins: its triangle spans [x, x + 0.6 · row height].
    const float w = std::max(grab, std::round((static_cast<float>(rect().bottom() - 1) - markY - 4.0f) * 0.6f));
    for (size_t i = 0; i < pr.locators->size(); ++i) {
      const float x = std::floor(view_.xOfTick(static_cast<double>((*pr.locators)[i].tick)));
      if (p.x >= x - 2.0f && p.x <= x + w) {
        h.grab = Grab::Locator;
        h.locator = static_cast<uint32_t>(i);
      }
    }
  }
  return h;
}

doc::LoopRegion Ruler::shownLoop() const noexcept {
  doc::LoopRegion l = view_.shown()->loop;
  if (!pressed_ || !dragging_) return l;
  switch (grab_.grab) {
    case Grab::LoopStart: l.start += dt_; break;
    case Grab::LoopEnd: l.end += dt_; break;
    case Grab::Loop:
      l.start += dt_;
      l.end += dt_;
      break;
    case Grab::LoopNew:
      if (newHi_ > newLo_) {
        l.start = newLo_;
        l.end = newHi_;
        l.enabled = true;
      }
      break;
    default: break;
  }
  return l;
}

Ticks Ruler::shownLocator(uint32_t index) const noexcept {
  const doc::Project& p = *view_.shown();
  const Ticks t = (*p.locators)[index].tick;
  return pressed_ && dragging_ && grab_.grab == Grab::Locator && grab_.locator == index ? t + dt_ : t;
}

Cursor Ruler::cursorAt(Vec2 p) const {
  const Grab g = pressed_ ? grab_.grab : hitAt(p).grab;
  switch (g) {
    case Grab::LoopStart:
    case Grab::LoopEnd:
    case Grab::LoopNew:
    case Grab::Locator: return Cursor::ResizeEW;
    case Grab::Loop: return Cursor::Move;
    case Grab::Zoom: break;
  }
  return Cursor::ResizeNS;  // a drag zooms (sideways: scrolls); a click locates
}

bool Ruler::cancelDrag() {
  const bool was = pressed_ && dragging_ && grab_.grab != Grab::Zoom;
  pressed_ = dragging_ = false;
  grab_ = {};
  dt_ = 0;
  newLo_ = newHi_ = 0;
  if (was) markDirty();
  return was;
}

void Ruler::paint(PaintContext& ctx) {
  const Theme* thp = ctx.res.theme;
  if (!thp) return;
  const Theme& th = *thp;
  const IRect r = rect();
  const IRect c = intersect(r, ctx.clip);
  if (c.empty()) return;
  DrawList& dl = ctx.dl;
  const UiScale s = ctx.scale;
  const Model& m = view_.model();
  const doc::Project& p = *m.shown;
  const TimeSigMap& sigs = p.timeline->sigMap;
  const double k = view_.devicePxPerTick();
  const double origin = view_.originTick();
  auto X = [&](double t) { return static_cast<float>(static_cast<double>(r.x) + (t - origin) * k); };
  const Ticks from = std::max<Ticks>(0, static_cast<Ticks>(std::floor(origin)) - 1);
  const Ticks to = static_cast<Ticks>(std::ceil(origin + static_cast<double>(r.w) / k)) + 1;

  // Rows (Live 12): bar numbers on top, the loop brace under them, then the marker row (locators, tempo and
  // signature changes, the start marker).
  const int32_t numH = s.px(am::kRulerNumberRow);
  const int32_t loopH = s.px(am::kRulerLoopRow);
  const float y0 = static_cast<float>(r.y);
  const float numY = y0;
  const float loopY = numY + static_cast<float>(numH);
  const float markY = loopY + static_cast<float>(loopH);
  const float bottom = static_cast<float>(r.bottom() - 1);
  const float rx0 = static_cast<float>(c.x), rx1 = static_cast<float>(c.right());
  const auto& at = th.arrangement;

  dl.addRectFilled({rx0, y0, rx1, bottom}, at.ruler);
  dl.addRectFilled({rx0, bottom, rx1, bottom + 1.0f}, at.separator);
  dl.pushClip(r.toRect());

  // Ticks: a short tick per line of a fine adaptive grid, a longer one with a label on a coarser grid (≥ 44 px).
  const Rgba minor = withAlpha(at.rulerText, 110);
  const Grid lines = Grid::adaptive(6.0, sigs, view_.pxPerTick());
  const float tickB = loopY;
  for (const GridLine& g : lines.lines(from, to)) {
    const float x = std::floor(X(static_cast<double>(g.tick)));
    if (x < rx0 - 1.0f || x >= rx1) continue;
    const float h = g.strength == GridStrength::Bar ? static_cast<float>(numH) * 0.35f
                    : g.strength == GridStrength::Beat ? static_cast<float>(numH) * 0.22f
                                                       : static_cast<float>(numH) * 0.12f;
    dl.addRectFilled({x, tickB - std::max(1.0f, std::round(h)), x + 1.0f, tickB}, minor);
  }
  TextSystem* ts = ctx.res.text;
  const FontId font = ctx.res.uiFonts ? ctx.res.uiFonts->small : kNoFont;
  const int32_t cap = ts && font != kNoFont ? static_cast<int32_t>(std::lround(0.727 * ts->metrics(font).pixelSize)) : 0;
  const Grid labels = Grid::adaptive(am::kRulerLabelMinPx, sigs, view_.pxPerTick());
  char buf[kRulerLabelMaxChars];
  for (const GridLine& g : labels.lines(from, to)) {
    const float x = std::floor(X(static_cast<double>(g.tick)));
    if (x >= rx1) break;
    dl.addRectFilled({x, numY + std::round(static_cast<float>(numH) * 0.4f), x + 1.0f, tickB}, at.rulerText);
    if (ts && font != kNoFont) {
      const size_t n = formatRulerPosition(rulerPosition(g), buf);
      ts->draw(dl, font, {x + static_cast<float>(s.px(3.0f)), numY + static_cast<float>((numH + cap) / 2)}, {buf, n},
               g.strength == GridStrength::Bar ? at.rulerText : withAlpha(at.rulerText, 170));
    }
  }

  // Loop brace (Live: a bar between two light edges under the numbers, with notched ends; dim when off).
  {
    const doc::LoopRegion loop = shownLoop();  // T4.16b: a brace drag's preview
    const float lx0 = std::round(X(static_cast<double>(loop.start)));
    const float lx1 = std::round(X(static_cast<double>(loop.end)));
    if (lx1 > rx0 - 2.0f && lx0 < rx1 + 2.0f && lx1 > lx0) {
      const Rgba fill = loop.enabled ? at.loopFill : at.loopOffFill;
      const Rgba edge = loop.enabled ? at.loopEdge : at.loopOffEdge;
      const float a = std::max(lx0, rx0 - 2.0f), b = std::min(lx1, rx1 + 2.0f);
      const float top = loopY + 1.0f, bot = markY - 1.0f;
      dl.addRectFilled({a, top, b, bot}, fill);
      dl.addRectFilled({a, top, b, top + 1.0f}, edge);
      dl.addRectFilled({a, bot - 1.0f, b, bot}, edge);
      const float hh = (bot - top) * 0.5f;
      // Ends: inward-pointing notches in the ruler colour (Live's brace corners).
      if (lx0 >= a && lx1 - lx0 > 4.0f * hh)
        dl.addTriangleFilled({lx0, top + 1.0f}, {lx0 + hh, top + hh}, {lx0, bot - 1.0f}, at.ruler);
      if (lx1 <= b && lx1 - lx0 > 4.0f * hh)
        dl.addTriangleFilled({lx1, top + 1.0f}, {lx1, bot - 1.0f}, {lx1 - hh, top + hh}, at.ruler);
    }
  }

  // Marker row: locators, tempo changes and time-signature changes (the latter two only when there are changes),
  // merged per tick so markers on one tick share one label ("Verse  128  3/4"); each label is cut at the next
  // marker.
  const float markH = bottom - markY;
  const float markBase = markY + std::round((markH + static_cast<float>(cap)) * 0.5f);
  const float pad = static_cast<float>(s.px(3.0f));
  auto& mk = markers_;
  mk.clear();
  if (p.locators)
    for (size_t i = 0; i < p.locators->size(); ++i) mk.push_back({shownLocator(static_cast<uint32_t>(i)), 0, static_cast<uint32_t>(i)});
  if (sigs.changeCount() > 1)
    for (size_t i = 0; i < sigs.changeCount(); ++i) mk.push_back({sigs.changeStartTick(i), 1, static_cast<uint32_t>(i)});
  const auto& tempo = p.timeline->tempo;
  if (tempo.size() > 1)
    for (size_t i = 0; i < tempo.size(); ++i) mk.push_back({tempo[i].tick, 2, static_cast<uint32_t>(i)});
  std::sort(mk.begin(), mk.end(), [](const Marker& x, const Marker& y) { return x.tick != y.tick ? x.tick < y.tick : x.kind < y.kind; });
  const Rgba dim = withAlpha(at.rulerText, 190);
  for (size_t i = 0; i < mk.size();) {
    size_t j = i;
    while (j < mk.size() && mk[j].tick == mk[i].tick) ++j;
    const float x = std::floor(X(static_cast<double>(mk[i].tick)));
    const float nextX = j < mk.size() ? std::floor(X(static_cast<double>(mk[j].tick))) - pad : rx1 + 1000.0f;
    if (x < rx1 && nextX > rx0) {
      float tx = x + pad;
      if (mk[i].kind == 0) {  // a locator: Live's right-pointing flag
        const float h = markH - 4.0f;
        const float w = std::round(h * 0.6f);
        dl.addTriangleFilled({x, markY + 2.0f}, {x + w, markY + 2.0f + h * 0.5f}, {x, markY + 2.0f + h}, at.rulerText);
        tx = x + w + pad;
      } else {
        dl.addRectFilled({x, markY, x + 1.0f, bottom}, dim);
      }
      if (ts && font != kNoFont) {
        for (size_t q = i; q < j && tx < nextX; ++q) {
          char tb[48];
          std::string_view text;
          Rgba col = dim;
          if (mk[q].kind == 0) {
            text = (*p.locators)[mk[q].index].name;
            col = at.rulerText;
          } else if (mk[q].kind == 1) {
            const TimeSignature sig = sigs.changeSignature(mk[q].index);
            const int n = std::snprintf(tb, sizeof tb, "%d/%d", sig.numerator, sig.denominator);
            text = {tb, static_cast<size_t>(std::max(0, n))};
          } else {
            text = formatBpm(tempo[mk[q].index].bpm, tb);
          }
          if (text.empty()) continue;
          tx += static_cast<float>(drawTextUpTo(dl, *ts, font, text, tx, markBase, nextX, col)) + 2.0f * pad;
        }
      }
    }
    i = j;
  }
  {
    // Start marker: a small downward triangle at the top of the marker row (where Space plays from).
    const float x = std::floor(X(static_cast<double>(view_.context().startMarker())));
    const float h = std::round(markH * 0.55f);
    if (x + h >= rx0 && x - h < rx1)
      dl.addTriangleFilled({x - h * 0.6f, markY}, {x + h * 0.6f + 1.0f, markY}, {x + 0.5f, markY + h}, at.startMarker);
  }
  if (const auto& ph = view_.playhead()) {
    const float x = std::floor(X(static_cast<double>(*ph)));
    const float w = static_cast<float>(std::max(1, s.px(1.0f)));
    if (x + w > rx0 && x < rx1) dl.addRectFilled({x, y0, x + w, bottom + 1.0f}, at.playhead);
  }
  dl.popClip();
}

bool Ruler::onPointer(const PointerEvent& e) {
  const float scale = tree() ? tree()->scale() : 1.0f;
  switch (e.type) {
    case PointerType::Down:
      if (e.button != PointerButton::Left) return false;
      pressed_ = true;
      dragging_ = false;
      pressPos_ = e.pos;
      pressTick_ = view_.tickAtX(e.pos.x);
      pressZoom_ = view_.pxPerTick();
      grab_ = hitAt(e.pos);
      dt_ = 0;
      if (UiTree* t = tree()) t->setFocus(&view_);  // the arrangement's keys (Esc cancels a brace drag)
      return true;
    case PointerType::Move: {
      if (!pressed_) return false;
      const float dx = e.pos.x - pressPos_.x, dy = e.pos.y - pressPos_.y;
      if (!dragging_ && std::max(std::abs(dx), std::abs(dy)) < kClickSlop * scale) return true;
      dragging_ = true;
      if (grab_.grab != Grab::Zoom) {
        // A brace / locator drag: relative snap from the dragged edge (Live), kept in range.
        const doc::Project& p = *view_.shown();
        const Grid g(view_.context().grid(), p.timeline->sigMap, view_.pxPerTick());
        if (grab_.grab == Grab::LoopNew) {  // T4.17: both ends on the grid (the cell under each end when snapping)
          const double a = std::max(0.0, std::min(pressTick_, view_.tickAtX(e.pos.x)));
          const double b = std::max(0.0, std::max(pressTick_, view_.tickAtX(e.pos.x)));
          Ticks lo = static_cast<Ticks>(std::floor(a)), hi = static_cast<Ticks>(std::ceil(b));
          if (g.snapping()) {
            lo = g.snap(lo);
            hi = std::max(g.snap(hi), lo + 1);
          }
          if (lo != newLo_ || hi != newHi_ || dt_ == 0) {
            newLo_ = lo;
            newHi_ = hi;
            dt_ = 1;  // something to commit
            markDirty();
          }
          return true;
        }
        const auto raw = static_cast<Ticks>(std::llround(view_.tickAtX(e.pos.x) - pressTick_));
        Ticks origin = 0, lo = 0, hi = doc::kMaxTicks / 2;
        switch (grab_.grab) {
          case Grab::LoopStart:
            origin = p.loop.start;
            lo = -p.loop.start;
            hi = p.loop.end - p.loop.start - 1;
            break;
          case Grab::LoopEnd:
            origin = p.loop.end;
            lo = p.loop.start + 1 - p.loop.end;
            break;
          case Grab::Loop:
            origin = p.loop.start;
            lo = -p.loop.start;
            break;
          case Grab::Locator:
            origin = (*p.locators)[grab_.locator].tick;
            lo = -origin;
            break;
          case Grab::Zoom:
          case Grab::LoopNew: break;
        }
        const Ticks dt = std::clamp(g.snapping() ? g.snapDelta(origin, raw) : raw, lo, hi);
        if (dt != dt_) {
          dt_ = dt;
          markDirty();
        }
        return true;
      }
      // The pressed tick stays under the pointer: vertical distance zooms, horizontal movement scrolls.
      const double zoom = std::clamp(pressZoom_ * std::exp(static_cast<double>(dy / scale) * kDragZoomPerPx), am::kMinPxPerTick,
                                     am::kMaxPxPerTick);
      const double dk = zoom * scale;
      view_.setTimeView(pressTick_ - (static_cast<double>(e.pos.x) - static_cast<double>(view_.lanesViewport().x)) / dk, zoom);
      return true;
    }
    case PointerType::Up: {
      if (!pressed_ || e.button != PointerButton::Left) return false;
      const doc::Project& p = *view_.shown();
      if (dragging_ && grab_.grab != Grab::Zoom) {
        // One command, one undo step; the preview stays drawn until the document change repaints the ruler.
        const doc::LoopRegion loop = shownLoop();
        const Ticks locTick = grab_.grab == Grab::Locator ? shownLocator(grab_.locator) : 0;
        const Hit g = grab_;
        const Ticks dt = dt_;
        cancelDrag();
        if (dt != 0) {
          ViewContext& ctx = view_.context();
          doc::Locator l;
          if (g.grab == Grab::Locator) {
            l = (*p.locators)[g.locator];
            l.tick = locTick;
          }
          const Status st = g.grab == Grab::Locator ? ctx.store().dispatch(doc::UpdateLocator{l}) : ctx.store().dispatch(doc::SetLoop{loop});
          if (!st) ctx.setStatus("Ruler: " + st.error().message);
        }
        return true;
      }
      const bool click = !dragging_;
      const Hit g = grab_;
      pressed_ = dragging_ = false;
      grab_ = {};
      // A click on a locator jumps there (Live); elsewhere it locates at the pointer.
      if (click && g.grab == Grab::Locator) view_.locateAt((*p.locators)[g.locator].tick, false);
      else if (click) view_.locateAt(static_cast<Ticks>(std::floor(pressTick_)));
      return true;
    }
    case PointerType::Wheel:
      if (e.mods & mod::kCtrl) {
        view_.zoomAt(e.pos.x, std::pow(1.25, static_cast<double>(e.wheel.y)));
      } else {
        const float notches = e.wheel.x != 0.0f ? e.wheel.x : -e.wheel.y;
        view_.scrollByPx(static_cast<double>(notches * 48.0f * scale));
      }
      return true;
    default: return false;
  }
}

// --- TimeRuler ------------------------------------------------------------------------------------------

TimeRuler::TimeRuler(ArrangementView& view) : Control("time-ruler"), view_(view) {
  setInfo("Time Ruler", "Minutes and seconds of the arrangement (from the tempo map).");
}

void TimeRuler::paint(PaintContext& ctx) {
  const Theme* thp = ctx.res.theme;
  if (!thp) return;
  const auto& at = thp->arrangement;
  const IRect r = rect();
  const IRect c = intersect(r, ctx.clip);
  if (c.empty()) return;
  DrawList& dl = ctx.dl;
  const float rx0 = static_cast<float>(c.x), rx1 = static_cast<float>(c.right());
  dl.addRectFilled({rx0, static_cast<float>(r.y), rx1, static_cast<float>(r.bottom())}, at.ruler);
  dl.addRectFilled({rx0, static_cast<float>(r.y), rx1, static_cast<float>(r.y + 1)}, at.separator);
  const TempoMap& tm = view_.model().shown->timeline->tempoMap;
  const TickRange vis = view_.visibleTicks();
  const double s0 = tm.ticksToSeconds(std::max<Ticks>(0, vis.start));
  const double s1 = tm.ticksToSeconds(std::max<Ticks>(1, vis.end));
  if (!(s1 > s0) || r.w <= 0) return;
  // Label step: the smallest "nice" number of seconds whose labels are ≥ 50 logical px apart.
  const double pxPerSecond = static_cast<double>(r.w) / ctx.scale.factor / (s1 - s0);
  constexpr double kSteps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 1800, 3600};
  double step = kSteps[std::size(kSteps) - 1];
  for (double st : kSteps)
    if (st * pxPerSecond >= 50.0) {
      step = st;
      break;
    }
  TextSystem* ts = ctx.res.text;
  const FontId font = ctx.res.uiFonts ? ctx.res.uiFonts->small : kNoFont;
  const int32_t cap = ts && font != kNoFont ? static_cast<int32_t>(std::lround(0.727 * ts->metrics(font).pixelSize)) : 0;
  const float base = static_cast<float>(r.y) + std::round((static_cast<float>(r.h) + static_cast<float>(cap)) * 0.5f);
  const double k = view_.devicePxPerTick();
  dl.pushClip(r.toRect());
  for (double sec = std::ceil(s0 / step) * step; sec <= s1 + 1e-9; sec += step) {
    const double tick = tm.secondsToTicks(sec);
    const float x = std::floor(static_cast<float>(static_cast<double>(r.x) + (tick - view_.originTick()) * k));
    if (x >= rx1) break;
    dl.addRectFilled({x, static_cast<float>(r.y + 1), x + 1.0f, static_cast<float>(r.y + r.h / 2)}, withAlpha(at.rulerText, 130));
    if (ts && font != kNoFont) {
      char buf[24];
      const auto total = static_cast<long long>(std::llround(sec));
      const int n = std::snprintf(buf, sizeof buf, "%lld:%02lld", total / 60, total % 60);
      ts->draw(dl, font, {x + static_cast<float>(ctx.scale.px(3.0f)), base}, {buf, static_cast<size_t>(std::max(0, n))},
               withAlpha(at.rulerText, 200));
    }
  }
  dl.popClip();
}

}  // namespace pq::views::arrangement
