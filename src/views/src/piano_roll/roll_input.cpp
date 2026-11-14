// Pointer and key input of the clip view's parts (T3.11). Live's MIDI editor gestures:
//   note area   click selects (Shift / Ctrl toggles), drag moves (time snapped relative to the grid, pitch by rows),
//               Alt-drag copies, the note's ends resize, a drag on empty space draws a selection rectangle,
//               double-click adds / deletes a note; in draw mode (B) a click adds a grid-cell note (or deletes the
//               note under it) and a drag adds one per cell crossed. Alt draws Cents notes on the background EDO,
//               Shift draws Ratio notes on the virtual fundamental's harmonics (T3.19). Right-click: the note menu
//               (convert identity, set pitch, lock the fundamental, row labels). Wheel scrolls (Shift: horizontally),
//               Ctrl+wheel zooms time, Ctrl+Alt+wheel zooms rows. Esc cancels a drag. Hover: the pitch readout.
//               T7.4 JI objects: J toggles the JI tool (drag a box, then type a proportion); a JI object's header or
//               border selects it, a drag from there moves it (time and rows; Alt: free cents), its left / right
//               border resizes it.
//               T7.5: a double-click on an object enters it (Esc / a double-click outside leaves): drawing and
//               double-clicks put notes on its voices, a digit key opens the field for a new stop at the pointer.
//               A stop's label under the box selects it (drag: move it in time, double-click: retype it, Delete:
//               remove it), the grip on the bottom border sets its glide, a harmonic number selects that term
//               (Alt+Up / Down walk it), a bezier transition's handles drag. Right-click on an object: its menu
//               (stops, the transition of the voice under the pointer, undertone, back to the tuning); on notes:
//               Tune to JI, Is Harmonic… (the virtual fundamental).
//   keys        click auditions the row and selects its notes (Shift adds); horizontal drag zooms the rows;
//               right-click: row labels; hover: the row's pitch readout.
//   ruler       drag the loop brace (its ends or the whole brace), the start marker and the end marker; elsewhere a
//               vertical drag zooms time around the pointer and a horizontal drag scrolls.
//   velocity    drag a stem (or chance bar) up / down: every selected note changes by the same amount.
// Every gesture is previewed and dispatched once on release: one undo step.
// T4.16b: every part's cursorAt asks the same hit test its press uses (noteAt, RulerWidget::hitAt, stemAt), so the
// cursor always says what a press would do; the note area highlights the note part a press would grab.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "../common/drag.h"
#include "pq/tuning/ji.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

namespace {

// The click slop every editor shares (M17: views/src/common/drag.h).
bool moreThanSlop(Vec2 a, Vec2 b, float scale) noexcept { return drag::beyondSlop(a, b, scale); }

Ticks roundTicks(double t) noexcept { return static_cast<Ticks>(std::llround(t)); }

// The cell a draw-mode note fills: the grid cell under t, or a quarter note from t when the grid is off.
TickRange drawCell(const Grid& g, Ticks t) noexcept {
  if (g.enabled()) return g.cellAt(t);
  return {t, t + kPpq};
}

}  // namespace

// --- note area ------------------------------------------------------------------------------------------

NoteAreaWidget::NoteAreaWidget(RollState& s) : Control("notes"), s_(s) {
  setFocusable(true);
  setInfo("MIDI Note Editor",
          "Click a note to select it, drag to move (Alt+drag copies), drag a note's end to resize. Double-click adds or "
          "deletes a note; B toggles Draw Mode. Up/Down move by one step of the tuning, Shift+Up/Down by one equave.");
}

void NoteAreaWidget::onFocusChanged(bool focused) {
  if (focused) s_.reportResolvedGrid();
}

void NoteAreaWidget::pressNote(const PointerEvent& e, const doc::Note& n, int zone) {
  const bool toggle = (e.mods & (mod::kShift | mod::kCtrl)) != 0;
  DragState& d = s_.drag;
  d = {};
  d.pressedWasSelected = s_.selected(n.id);
  if (toggle) {
    s_.toggleSelected(n.id);
    if (!s_.selected(n.id)) return;  // toggled off: no drag
  } else if (!d.pressedWasSelected) {
    s_.selectOnly(n.id);
  }
  d.kind = zone > 0 ? DragKind::ResizeEnd : zone < 0 ? DragKind::ResizeStart
                                                     : ((e.mods & mod::kAlt) ? DragKind::Copy : DragKind::Move);
  d.pressed = n.id;
  d.press = d.last = e.pos;
  d.originTick = zone > 0 ? n.start + n.length : n.start;
  d.pressTick = roundTicks(s_.tickAtX(e.pos.x));
  if (!s_.rows().empty()) d.pressDegree = s_.rows().row(s_.rows().indexAtY(s_.contentY(e.pos.y))).degree;
  // T7.5: inside an entered object a vertical drag steps by its voices.
  if (s_.jiEntered().valid())
    d.pressVoice = n.pitch.kind() == tuning::PitchKind::Harmonic ? static_cast<int32_t>(n.pitch.voiceIndex()) : s_.jiVoiceAt(s_.jiEntered(), e.pos);
  s_.beginDragTracking();
  s_.audition(n.pitch, n.velocity, n.start, n.length);
  s_.auditionHeld = true;  // AU.voices: released on pointer up
}

// The note a draw gesture puts in cell `c` at device y: a Degree note on the row there; with Overlay (Alt, while a
// background EDO is shown) a Cents note on the overlay line nearest the pointer's pitch (T3.12); with Ratio (Shift,
// T3.19) a Ratio note on the virtual fundamental's harmonic nearest the pointer (RollState::ratioFor).
DrawnNote NoteAreaWidget::cellNote(TickRange c, Vec2 p, DrawPitch mode) const {
  DrawnNote dn{c.start, c.end - c.start, 0};
  const RowModel& rows = s_.rows();
  if (rows.empty()) return dn;
  const int32_t cy = s_.contentY(p.y);
  if (mode == DrawPitch::Voice) {
    // T7.5: inside the entered object: a Harmonic note on the voice under the pointer, clipped to the object's span
    // (a cell outside it gets no note: length 0).
    dn.mode = DrawPitch::Voice;
    dn.degree = rows.row(rows.indexAtY(cy)).degree;
    const doc::JiObject* o = s_.jiObject(s_.jiEntered());
    const int32_t voice = s_.jiVoiceAt(s_.jiEntered(), p);
    const Ticks a = o ? std::max(c.start, o->start) : 0, b = o ? std::min(c.end, o->end()) : 0;
    if (!o || voice < 0 || b <= a) {
      dn.length = 0;
      return dn;
    }
    dn.start = a;
    dn.length = b - a;
    dn.voice = static_cast<uint32_t>(voice);
    return dn;
  }
  if (mode == DrawPitch::Overlay && s_.overlay()) {
    dn.mode = DrawPitch::Overlay;
    dn.cents = s_.nearestOverlayCents(rows.centsAtY(static_cast<double>(cy)));
    dn.degree = rows.placeCents(dn.cents).nearestDegree;
  } else if (mode == DrawPitch::Ratio) {
    dn.mode = DrawPitch::Ratio;
    const int64_t row = rows.row(rows.indexAtY(cy)).degree;
    dn.ratio = s_.ratioFor(row, rows.centsAtY(static_cast<double>(cy)));
    const std::optional<RollState::Harmonics> h = s_.harmonics();  // T7.5b: the ratio is to the fundamental's base
    dn.degree = rows.placeCents(tuning::cents(dn.ratio) + (h ? h->baseCents : 0.0)).nearestDegree;
  } else {
    dn.degree = rows.row(rows.indexAtY(cy)).degree;
  }
  return dn;
}

void NoteAreaWidget::pressEmpty(const PointerEvent& e) {
  DragState& d = s_.drag;
  d = {};
  const Ticks t = roundTicks(s_.tickAtX(e.pos.x));
  const Grid g = s_.grid();
  d.press = d.last = e.pos;
  d.pressTick = t;
  if (!s_.rows().empty()) d.pressDegree = s_.rows().row(s_.rows().indexAtY(s_.contentY(e.pos.y))).degree;
  if (s_.ctx.drawMode()) {
    const TickRange c = drawCell(g, t);
    d.kind = DragKind::Draw;
    d.drawn.push_back(cellNote(c, e.pos, s_.drawPitchFor(e.mods)));
    d.trackDamage = d.drawn.back().mode != DrawPitch::Voice;  // no notes tracked: the drawn cells are (a voice note: the area)
    s_.audition(s_.drawnPitch(d.drawn.back()), 100.0f, d.drawn.back().start, c.end - c.start);
    s_.auditionHeld = true;  // AU.voices: released on pointer up
    s_.damageDrag();
    return;
  }
  s_.setInsertTick(g.snapping() ? g.snap(t) : t);
  d.kind = DragKind::Rubber;
  d.pressSelection = s_.selection();
  d.additive = (e.mods & (mod::kShift | mod::kCtrl)) != 0;
  if (!d.additive) {
    s_.clearSelection();
    if (s_.jiEntered().valid()) s_.selectJiStop(s_.jiEntered(), -1);  // T7.5: the stop / harmonic selection goes too
    else s_.selectJiObject({});  // T7.4
  }
  d.base = s_.selection();
  d.rubber = {static_cast<int32_t>(e.pos.x), static_cast<int32_t>(e.pos.y), 0, 0};
}

// T7.4: a press on a JI object's header or border selects it; a drag from the header / top / bottom border moves it
// (time snapped as a note's, pitch by rows), from the left / right border resizes it.
void NoteAreaWidget::pressJi(const PointerEvent& e, const JiHit& hit) {
  if (hit.zone != JiZone::Body && hit.zone != JiZone::Start && hit.zone != JiZone::End) {
    pressJiPart(e, hit);
    return;
  }
  s_.selectJiObject(hit.id);
  if (s_.jiEntered().valid()) s_.selectJiStop(hit.id, -1);
  DragState& d = s_.drag;
  d = {};
  const JiView* v = nullptr;
  for (const JiView& j : s_.jiViews())
    if (j.id == hit.id) v = &j;
  if (!v) return;
  d.kind = hit.zone == JiZone::Start ? DragKind::JiResizeStart : hit.zone == JiZone::End ? DragKind::JiResizeEnd : DragKind::JiMove;
  d.ji = hit.id;
  d.press = d.last = e.pos;
  d.originTick = hit.zone == JiZone::End ? v->start + v->length : v->start;
  d.pressTick = roundTicks(s_.tickAtX(e.pos.x));
  if (!s_.rows().empty()) d.pressDegree = s_.rows().row(s_.rows().indexAtY(s_.contentY(e.pos.y))).degree;
}

// T7.5: a stop's label (select; drag = move it in time), its glide grip, a harmonic number (select the term), a
// bezier handle of a voice's transition.
void NoteAreaWidget::pressJiPart(const PointerEvent& e, const JiHit& hit) {
  DragState& d = s_.drag;
  d = {};
  const JiView* v = s_.jiViewOf(hit.id);
  const doc::JiObject* o = s_.jiObject(hit.id);
  if (!v || !o) return;
  d.press = d.last = e.pos;
  d.pressTick = roundTicks(s_.tickAtX(e.pos.x));
  d.ji = hit.id;
  switch (hit.zone) {
    case JiZone::StopLabel:
      s_.selectJiStop(hit.id, hit.index);
      if (hit.index >= 1 && hit.index < static_cast<int32_t>(o->stops.size())) {  // stop 0 is the object's start
        d.kind = DragKind::JiStop;
        d.jiStop = hit.index;
        d.originTick = o->start + o->stops[static_cast<size_t>(hit.index)].at;
      }
      break;
    case JiZone::Glide:
      if (hit.index >= 1 && hit.index < static_cast<int32_t>(o->stops.size())) {
        s_.selectJiStop(hit.id, hit.index);
        const doc::JiStop& st = o->stops[static_cast<size_t>(hit.index)];
        d.kind = DragKind::JiGlide;
        d.jiStop = hit.index;
        d.originTick = o->start + st.at - std::min(st.timing.glide, st.at - o->stops[static_cast<size_t>(hit.index) - 1].at);
      }
      break;
    case JiZone::VoiceLabel: s_.selectJiTerm(hit.id, hit.index); break;
    case JiZone::Bezier:
      if (hit.index >= 0 && hit.index < static_cast<int32_t>(v->beziers.size())) {
        const JiView::Bezier& bz = v->beziers[static_cast<size_t>(hit.index)];
        d.kind = DragKind::JiBezier;
        d.jiStop = static_cast<int32_t>(bz.stop);
        d.jiVoice = bz.voice;
        d.jiHandle = hit.handle;
        d.bx = hit.handle == 0 ? bz.x1 : bz.x2;
        d.by = hit.handle == 0 ? bz.y1 : bz.y2;
      }
      break;
    default: break;
  }
}

// T7.4 (JI tool): the box of a new object, from the grid cell and row under the press.
void NoteAreaWidget::pressJiBox(const PointerEvent& e) {
  DragState& d = s_.drag;
  d = {};
  s_.selectJiObject({});
  s_.clearSelection();
  if (s_.rows().empty()) return;
  d.kind = DragKind::JiBox;
  d.press = d.last = e.pos;
  d.pressTick = roundTicks(s_.tickAtX(e.pos.x));
  d.pressDegree = s_.rows().row(s_.rows().indexAtY(s_.contentY(e.pos.y))).degree;
}

void NoteAreaWidget::dragTo(Vec2 p, Mods mods) {
  DragState& d = s_.drag;
  const float scale = tree() ? tree()->scale() : 1.0f;
  if (!d.moved && !moreThanSlop(p, d.press, scale)) return;
  d.moved = true;
  d.last = p;
  const Grid g = s_.grid();
  const Ticks raw = roundTicks(s_.tickAtX(p.x) - s_.tickAtX(d.press.x));
  switch (d.kind) {
    case DragKind::Move:
    case DragKind::Copy: {
      d.dt = g.snapping() ? g.snapDelta(d.originTick, raw) : raw;
      if (s_.jiEntered().valid()) {  // T7.5: by the object's voices
        const int32_t voice = s_.jiVoiceAt(s_.jiEntered(), p);
        if (voice >= 0 && d.pressVoice >= 0) d.steps = voice - d.pressVoice;
      } else if (!s_.rows().empty()) {
        const int64_t deg = s_.rows().row(s_.rows().indexAtY(s_.contentY(p.y))).degree;
        d.steps = static_cast<int32_t>(std::clamp<int64_t>(deg - d.pressDegree, INT32_MIN / 2, INT32_MAX / 2));
      }
      s_.updatePreviewPitches();
      break;
    }
    case DragKind::ResizeEnd:
    case DragKind::ResizeStart: d.dt = g.snapping() ? g.snapDelta(d.originTick, raw) : raw; break;
    case DragKind::Rubber: {
      const int32_t x0 = static_cast<int32_t>(std::min(p.x, d.press.x)), x1 = static_cast<int32_t>(std::max(p.x, d.press.x));
      const int32_t y0 = static_cast<int32_t>(std::min(p.y, d.press.y)), y1 = static_cast<int32_t>(std::max(p.y, d.press.y));
      d.rubber = {x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
      const int32_t cy0 = s_.contentY(static_cast<float>(y0)), cy1 = s_.contentY(static_cast<float>(y1));
      std::vector<uint32_t> ids = d.base;
      const Ticks t0 = roundTicks(s_.tickAtX(static_cast<float>(x0))), t1 = roundTicks(s_.tickAtX(static_cast<float>(x1)));
      const doc::JiObjectId entered = s_.jiEntered();
      s_.forNotesIn(t0, t1 + 1, [&](const doc::Note& n) {
        if (entered.valid() && doc::jiObjectOf(n.pitch) != entered) return;  // T7.5: only the entered object's notes
        if (s_.onPath(n, n.pitch)) {  // T7.5: where the note is drawn
          const IRect b = s_.pathBox(n, n.start, n.length, n.pitch);
          if (!b.empty() && b.x < x1 && b.right() > x0 && b.y < y1 && b.bottom() > y0) ids.push_back(n.id.value);
          return;
        }
        const NotePlace pl = s_.place(n.pitch);
        if (pl.ok && pl.bottom > cy0 && pl.top <= cy1) ids.push_back(n.id.value);
      });
      s_.select(std::move(ids));
      break;
    }
    case DragKind::JiMove: {
      const Ticks dt = g.snapping() ? g.snapDelta(d.originTick, raw) : raw;
      int32_t steps = d.steps;
      double cents = 0.0;
      if (!s_.rows().empty()) {
        if (mods & mod::kAlt) {  // T7.5: Alt = the fundamental moves freely (cents), not by rows
          steps = 0;
          cents = s_.rows().centsAtY(static_cast<double>(s_.contentY(p.y))) - s_.rows().centsAtY(static_cast<double>(s_.contentY(d.press.y)));
        } else {
          const int64_t deg = s_.rows().row(s_.rows().indexAtY(s_.contentY(p.y))).degree;
          steps = static_cast<int32_t>(std::clamp<int64_t>(deg - d.pressDegree, INT32_MIN / 2, INT32_MAX / 2));
        }
      }
      if (dt != d.dt || steps != d.steps || cents != d.cents || !s_.jiPreview()) {
        d.dt = dt;
        d.steps = steps;
        d.cents = cents;
        s_.updateJiPreview();  // built from the edit the release will dispatch
      }
      break;
    }
    case DragKind::JiStop:
    case DragKind::JiGlide: {  // T7.5
      const Ticks dt = g.snapping() ? g.snapDelta(d.originTick, raw) : raw;
      if (dt != d.dt || !s_.jiPreview()) {
        d.dt = dt;
        s_.updateJiPreview();
      }
      break;
    }
    case DragKind::JiBezier: {  // T7.5: the handle follows the pointer inside its transition
      const JiView* v = s_.jiViewOf(d.ji);
      if (!v || s_.rows().empty()) break;
      for (const JiView::Bezier& bz : v->beziers) {
        if (static_cast<int32_t>(bz.stop) != d.jiStop || bz.voice != d.jiVoice) continue;
        const double span = static_cast<double>(bz.arrive - bz.begin);
        const double t = s_.tickAtX(p.x) - static_cast<double>(v->start + bz.begin);
        d.bx = static_cast<float>(std::clamp(span > 0.0 ? t / span : 0.0, 0.0, 1.0));
        const double c = s_.rows().centsAtY(static_cast<double>(s_.contentY(p.y)));
        const double dc = bz.to - bz.from;
        if (std::fabs(dc) > 1e-6) d.by = static_cast<float>(std::clamp((c - bz.from) / dc, -1.0, 2.0));
        s_.updateJiPreview();
        break;
      }
      break;
    }
    case DragKind::JiResizeStart:
    case DragKind::JiResizeEnd: {
      const Ticks dt = g.snapping() ? g.snapDelta(d.originTick, raw) : raw;
      if (dt != d.dt || !s_.jiPreview()) {
        d.dt = dt;
        s_.updateJiPreview();
      }
      break;
    }
    case DragKind::JiBox: {
      if (s_.rows().empty()) break;
      const TickRange a = drawCell(g, d.pressTick), b = drawCell(g, roundTicks(s_.tickAtX(p.x)));
      d.jiStart = std::min(a.start, b.start);
      d.jiLength = std::max(a.end, b.end) - d.jiStart;
      const int64_t deg = s_.rows().row(s_.rows().indexAtY(s_.contentY(p.y))).degree;
      d.jiLow = std::min(deg, d.pressDegree);
      d.jiHigh = std::max(deg, d.pressDegree);
      break;
    }
    case DragKind::Draw: {
      if (s_.rows().empty()) break;
      const TickRange c = drawCell(g, roundTicks(s_.tickAtX(p.x)));
      const bool have = std::any_of(d.drawn.begin(), d.drawn.end(), [&](const DrawnNote& n) { return n.start == c.start; });
      if (!have) {
        d.drawn.push_back(cellNote(c, p, d.drawn.empty() ? DrawPitch::Row : d.drawn.front().mode));
        s_.audition(s_.drawnPitch(d.drawn.back()), 100.0f, d.drawn.back().start, c.end - c.start);
        s_.auditionHeld = true;  // AU.voices: released on pointer up
      }
      break;
    }
    default: break;
  }
  if (d.kind == DragKind::Rubber) {
    // The band itself (selection changes repaint through select()).
    const IRect band{d.rubber.x - 1, d.rubber.y - 1, d.rubber.w + 2, d.rubber.h + 2};
    const IRect both{std::min(band.x, d.damage.x), std::min(band.y, d.damage.y),
                     std::max(band.right(), d.damage.right()) - std::min(band.x, d.damage.x),
                     std::max(band.bottom(), d.damage.bottom()) - std::min(band.y, d.damage.y)};
    d.damage = d.damage.empty() ? band : both;
    if (tree()) tree()->damage().add(intersect(d.damage, rect()).toRect());
    d.damage = band;
  } else {
    s_.damageDrag();
  }
}

bool NoteAreaWidget::onPointer(const PointerEvent& e) {
  if (!s_.hasClip()) return false;
  switch (e.type) {
    case PointerType::Down: {
      if (e.button == PointerButton::Right) {
        if (tree()) tree()->setFocus(this);
        (void)openMenu(e.pos);
        return true;
      }
      if (e.button != PointerButton::Left) return false;
      if (tree()) tree()->setFocus(this);
      s_.setHoverPart({});  // a gesture shows its own preview
      // T7.4: the JI tool grabs an object's header / border or draws a new box (also over notes); without it an
      // object's header / border is grabbed where no note lies under the pointer. (T7.5: a double-click on a hit
      // enters the object.)
      // T7.5: inside an entered object only it and its notes take presses (Draw Mode included: it draws on the
      // voices); a double-click outside leaves it; its handles (glide grips, harmonic numbers, bezier handles) come
      // before its notes. A double-click on an object enters it; on a stop's label retypes the stop.
      const doc::JiObjectId entered = s_.jiEntered();
      const JiHit jh = (s_.ctx.drawMode() && !entered.valid()) ? JiHit{} : s_.jiAt(e.pos);
      if (entered.valid() && jh.id != entered) {
        s_.drag = {};
        if (e.clickCount >= 2) {
          s_.leaveJiObject();
        } else {
          s_.clearSelection();
          s_.selectJiStop(entered, -1);
        }
        return true;
      }
      if (s_.jiTool()) {
        if (jh.grabs()) pressJi(e, jh);
        else pressJiBox(e);
        return true;
      }
      if (jh.zone == JiZone::Glide || jh.zone == JiZone::VoiceLabel || jh.zone == JiZone::Bezier) {
        pressJi(e, jh);
        return true;
      }
      int zone = 0;
      const doc::Note* n = s_.noteAt(e.pos, &zone);
      if (!n && jh.grabs()) {
        if (e.clickCount >= 2 && jh.zone == JiZone::StopLabel) {
          s_.drag = {};
          s_.selectJiStop(jh.id, jh.index);
          (void)s_.openJiStopRetype(jh.id, static_cast<uint32_t>(jh.index));
          return true;
        }
        if (e.clickCount >= 2 && !entered.valid()) {
          s_.drag = {};
          s_.enterJiObject(jh.id);
          return true;
        }
        pressJi(e, jh);
        return true;
      }
      if (!n && jh.zone == JiZone::Inside && e.clickCount >= 2 && !entered.valid() && !s_.ctx.drawMode()) {
        s_.drag = {};
        s_.enterJiObject(jh.id);
        return true;
      }
      if (n && (s_.ctx.drawMode() || (e.clickCount >= 2 && !(e.mods & (mod::kShift | mod::kCtrl))))) {
        // Draw mode click / double-click on a note: delete it.
        s_.drag = {};
        s_.selectOnly(n->id);
        s_.deleteSelection();
        return true;
      }
      if (n) {
        pressNote(e, *n, zone);
      } else if (e.clickCount >= 2 && !s_.ctx.drawMode()) {
        // Double-click on empty space: one note in the grid cell.
        s_.drag = {};
        const Grid g = s_.grid();
        const TickRange c = drawCell(g, roundTicks(s_.tickAtX(e.pos.x)));
        if (!s_.rows().empty()) {
          const DrawnNote dn = cellNote(c, e.pos, s_.drawPitchFor(e.mods));
          s_.addNotes({dn});
          s_.audition(s_.drawnPitch(dn), 100.0f, dn.start, c.end - c.start);
          s_.auditionHeld = true;  // AU.voices: released on pointer up
        }
      } else {
        pressEmpty(e);
      }
      return true;
    }
    case PointerType::Move:
      if (captured() && s_.drag.kind != DragKind::None) {
        dragTo(e.pos, e.mods);
        return true;
      }
      if (!captured()) {
        s_.hoverAt(e.pos, false);
        s_.hoverPartAt(e.pos);
      }
      return false;
    case PointerType::Up: {
      if (e.button == PointerButton::Right) return true;
      if (e.button != PointerButton::Left) return false;
      s_.releaseAudition();
      DragState& d = s_.drag;
      if (d.kind == DragKind::None) {
        s_.hoverPartAt(e.pos);
        return true;
      }
      if (!d.moved && (d.kind == DragKind::Move || d.kind == DragKind::Copy) && d.pressedWasSelected &&
          !(e.mods & (mod::kShift | mod::kCtrl))) {
        s_.drag = {};
        s_.selectOnly(d.pressed);  // a click on a note of a multiple selection selects just it (Live)
        s_.hoverPartAt(e.pos);
        return true;
      }
      if (!d.moved && d.kind != DragKind::Draw) {
        s_.drag = {};
        s_.markNotesDirty();
        s_.hoverPartAt(e.pos);
        return true;
      }
      s_.commitDrag();
      s_.hoverPartAt(e.pos);  // where the gesture left the notes (the edit repaints the area)
      return true;
    }
    case PointerType::Wheel: {
      const float scale = tree() ? tree()->scale() : 1.0f;
      const float dy = e.wheel.y;
      if ((e.mods & mod::kCtrl) && (e.mods & mod::kAlt)) {
        s_.setRowHeight(s_.rowPx() + static_cast<int32_t>(std::lround(dy)));
      } else if (e.mods & mod::kCtrl) {
        s_.setZoom(s_.pxPerTick() * std::pow(1.25, static_cast<double>(dy)), s_.tickAtX(e.pos.x), e.pos.x);
      } else if (e.mods & mod::kShift) {
        s_.scrollByPx(-dy * 60.0f * scale, 0.0f);
      } else {
        s_.scrollByPx(-e.wheel.x * 60.0f * scale, -dy * 3.0f * static_cast<float>(std::max(1, s_.rows().empty() ? 12 : s_.rows().laneHeight())));
      }
      return true;
    }
    default: return false;
  }
}

bool NoteAreaWidget::onKey(const KeyEvent& e) {
  if (e.down && e.key == key::kEscape && s_.drag.kind != DragKind::None) {
    s_.abortDrag();
    if (tree() && tree()->captured() == this) tree()->releaseCapture();
    return true;
  }
  // T7.5 (A2): inside an entered object, typing a proportion adds a stop at the pointer: the first digit opens the
  // field (the character itself arrives as text in the field).
  if (e.down && !e.repeat && (e.mods & mod::kChordMask) == 0 && e.key >= '1' && e.key <= '9' && s_.jiEntered().valid() &&
      s_.drag.kind == DragKind::None) {
    const std::optional<Ticks> at = s_.jiStopTickAt(s_.jiEntered(), s_.jiPointerX());
    if (!at) {
      s_.ctx.setStatus("New stop: point inside the object, then type its proportion");
      return true;
    }
    return s_.openJiStopEntry(s_.jiEntered(), *at) != nullptr;
  }
  return false;
}

void NoteAreaWidget::onCaptureLost() {
  s_.releaseAudition();
  s_.abortDrag();
}

void NoteAreaWidget::onHoverChanged(bool hovered) {
  Control::onHoverChanged(hovered);
  if (!hovered) {
    s_.clearHover();
    s_.clearHoverPart();
  }
}

Cursor NoteAreaWidget::cursorAt(Vec2 p) const {
  if (!s_.hasClip()) return Cursor::Inherit;
  if (captured()) switch (s_.drag.kind) {
      case DragKind::Move:
      case DragKind::Copy: return Cursor::Move;
      case DragKind::ResizeStart:
      case DragKind::ResizeEnd: return Cursor::ResizeEW;
      case DragKind::Draw: return Cursor::Crosshair;
      case DragKind::Rubber: return Cursor::Arrow;
      case DragKind::JiMove:
      case DragKind::JiBezier: return Cursor::Move;
      case DragKind::JiResizeStart:
      case DragKind::JiResizeEnd:
      case DragKind::JiStop:
      case DragKind::JiGlide: return Cursor::ResizeEW;
      case DragKind::JiBox: return Cursor::Crosshair;
      default: break;
    }
  const bool entered = s_.jiEntered().valid();
  if (s_.ctx.drawMode() && !entered) return Cursor::Crosshair;  // a click adds a cell's note (or deletes the one under it)
  // T7.4: a JI object's header / border (as the press: always with the JI tool, else where no note is). T7.5: its
  // stop labels (a later stop drags sideways), glide grips, harmonic numbers, bezier handles.
  const JiHit jh = s_.jiAt(p);
  Cursor jiCursor = Cursor::Arrow;
  switch (jh.zone) {
    case JiZone::Body:
    case JiZone::Bezier: jiCursor = Cursor::Move; break;
    case JiZone::Start:
    case JiZone::End:
    case JiZone::Glide: jiCursor = Cursor::ResizeEW; break;
    case JiZone::StopLabel: jiCursor = jh.index >= 1 ? Cursor::ResizeEW : Cursor::Pointer; break;
    case JiZone::VoiceLabel: jiCursor = Cursor::Pointer; break;
    default: break;
  }
  if (s_.jiTool()) return jh.grabs() ? jiCursor : Cursor::Crosshair;
  if (jh.zone == JiZone::Glide || jh.zone == JiZone::VoiceLabel || jh.zone == JiZone::Bezier) return jiCursor;
  int zone = 0;
  if (!s_.noteAt(p, &zone)) {  // empty space: a drag draws the selection rectangle (Draw Mode inside an object: a note)
    if (entered && s_.ctx.drawMode() && jh.zone == JiZone::Inside) return Cursor::Crosshair;
    return jiCursor;
  }
  return zone != 0 ? Cursor::ResizeEW : Cursor::Move;
}

InfoText NoteAreaWidget::info() const {
  if (!s_.hovering()) return Control::info();
  return {s_.hoverTitle(), s_.hoverBody()};
}

// The right-click menu (T3.19, M3 review H7): identity conversions, a typed pitch and the fundamental lock for the
// selected notes (a note under the pointer that is not selected becomes the selection, as Live), else paste / select
// all; the row labels for any press.
Menu* NoteAreaWidget::openMenu(Vec2 at) {
  const doc::Note* under = s_.noteAt(at);
  // T7.5: a JI object (its box, header, border, a stop's label) where no note lies under the pointer: its own menu.
  if (!under)
    if (const JiHit jh = s_.jiAt(at); jh.zone != JiZone::None) return openJiMenu(at, jh);
  if (under && !s_.selected(under->id)) s_.selectOnly(under->id);
  std::shared_ptr<RollState*> alive = s_.self();
  auto guarded = [alive](std::function<void(RollState&)> f) {
    return [alive, f = std::move(f)] {
      if (*alive) f(**alive);
    };
  };
  std::vector<MenuItem> items;
  const std::vector<uint32_t>& sel = s_.selection();
  if (!sel.empty()) {
    // Which identities the selection holds.
    bool kinds[4] = {false, false, false, false};
    const doc::Note* single = nullptr;
    for (const doc::Note& n : s_.clip()->notes)
      if (s_.selected(n.id)) {
        kinds[static_cast<size_t>(n.pitch.kind())] = true;
        single = &n;
      }
    if (sel.size() != 1) single = nullptr;
    const int nKinds = kinds[0] + kinds[1] + kinds[2] + kinds[3];
    auto conv = [&](const char* label, tuning::PitchKind k) {
      MenuItem m = MenuItem::item(label, guarded([k](RollState& st) { (void)st.convertSelection(k); }));
      m.checked = nKinds == 1 && kinds[static_cast<size_t>(k)];
      m.enabled = !(nKinds == 1 && kinds[static_cast<size_t>(k)]) && !(nKinds == 1 && kinds[3]);
      return m;
    };
    // T7.5 (A2): the selected chord goes from the tuning to the nearest just proportion (a JI object the notes follow).
    items.push_back(MenuItem::item("Tune to JI", guarded([](RollState& st) { (void)st.tuneSelectionToJi(); }), "Ctrl+Shift+J"));
    items.push_back(MenuItem::sep());
    items.push_back(conv("Degree (moves with the tuning)", tuning::PitchKind::Degree));
    items.push_back(conv("Ratio (stays at its just pitch)", tuning::PitchKind::Ratio));
    items.push_back(conv("Cents (never moves)", tuning::PitchKind::Cents));
    items.push_back(MenuItem::item("Set Pitch\xE2\x80\xA6", guarded([](RollState& st) { (void)st.openPitchEntry(); })));
    items.push_back(MenuItem::sep());
    // T7.5 (A4): the virtual fundamental with the mouse: "this note is harmonic N" (a number field): the fundamental
    // is locked at the note / N, its harmonics are drawn as guides and drawing snaps to them.
    const bool lock = s_.clip()->fundamental.anchor.has_value();
    if (const doc::Note* target = under ? under : single) {
      const doc::NoteId id = target->id;
      items.push_back(MenuItem::item("Is Harmonic\xE2\x80\xA6", guarded([id](RollState& st) { (void)st.openHarmonicEntry(id); })));
    }
    MenuItem unlock = MenuItem::item("Unlock Fundamental", guarded([](RollState& st) { (void)st.unlockFundamental(); }));
    unlock.enabled = lock;
    items.push_back(std::move(unlock));
    items.push_back(MenuItem::sep());
    MenuItem dup = MenuItem::item("Duplicate", guarded([](RollState& st) { st.duplicateSelection(); }), "Ctrl+D");
    items.push_back(std::move(dup));
    items.push_back(MenuItem::item("Quantize", guarded([](RollState& st) { st.quantizeSelection(); }), "Ctrl+U"));
    items.push_back(MenuItem::item("Delete", guarded([](RollState& st) { st.deleteSelection(); }), "Delete"));
  } else {
    items.push_back(MenuItem::item("Paste", guarded([](RollState& st) { st.paste(); }), "Ctrl+V"));
    items.push_back(MenuItem::item("Select All", guarded([](RollState& st) { st.selectAll(); }), "Ctrl+A"));
    MenuItem unlock = MenuItem::item("Unlock Fundamental", guarded([](RollState& st) { (void)st.unlockFundamental(); }));
    unlock.enabled = s_.clip()->fundamental.anchor.has_value();
    items.push_back(std::move(unlock));
  }
  items.push_back(MenuItem::sep());
  items.push_back(rowLabelsMenu(s_));
  return openContextMenu(*this, at, std::move(items));
}

// T7.5: the right-click menu of a JI object: the transition of the voice under the pointer (shape, its own glide and
// delay), stops at the pointer, undertone, enter / leave, copy / duplicate / remove.
Menu* NoteAreaWidget::openJiMenu(Vec2 at, const JiHit& hit) {
  const doc::JiObjectId id = hit.id;
  const doc::JiObject* o = s_.jiObject(id);
  const JiView* v = s_.jiViewOf(id);
  if (!o || !v) return nullptr;
  const bool entered = s_.jiEntered() == id;
  if (!entered) s_.selectJiObject(id);
  if (hit.zone == JiZone::StopLabel) s_.selectJiStop(id, hit.index);
  std::shared_ptr<RollState*> alive = s_.self();
  auto guarded = [alive](std::function<void(RollState&)> f) {
    return [alive, f = std::move(f)] {
      if (*alive) f(**alive);
    };
  };
  const float scale = tree() ? tree()->scale() : 1.0f;
  std::vector<MenuItem> items;
  // The voice under the pointer and the transition it is in (or moves to next).
  if (hit.zone == JiZone::Inside || hit.zone == JiZone::Body) {
    const int32_t voice = s_.jiVoiceAt(id, at);
    const int32_t stop = voice >= 0 ? s_.jiTransitionAt(id, static_cast<uint32_t>(voice), at) : -1;
    bool onVoice = false;
    if (voice >= 0 && stop >= 1) {
      const double t = std::clamp(s_.tickAtX(at.x) - static_cast<double>(o->start), 0.0, static_cast<double>(o->length));
      if (const std::optional<double> c = doc::jiVoiceCents(*o, s_.tuning(), static_cast<uint32_t>(voice), t))
        onVoice = std::fabs(s_.yOfCents(*c) - at.y) <= 10.0f * scale;
    }
    if (onVoice) {
      const uint32_t vi = static_cast<uint32_t>(voice), si = static_cast<uint32_t>(stop);
      const doc::JiTransition tr = doc::jiTransition(*o, vi, si);
      std::vector<MenuItem> sub;
      const std::pair<const char*, doc::JiShape::Kind> shapes[] = {{"Straight", doc::JiShape::LogLinear},
                                                                   {"S-Curve", doc::JiShape::SCurve},
                                                                   {"Bezier (drag its handles inside the object)", doc::JiShape::Bezier}};
      for (const auto& [label, kind] : shapes) {
        MenuItem m = MenuItem::item(label, guarded([id, si, vi, kind = kind](RollState& st) { (void)st.setJiVoiceShape(id, si, vi, kind); }));
        m.checked = tr.shape.kind == kind;
        sub.push_back(std::move(m));
      }
      sub.push_back(MenuItem::sep());
      sub.push_back(MenuItem::item("Glide\xE2\x80\xA6", guarded([id, si, vi](RollState& st) { (void)st.openJiTimingEntry(id, si, vi, false); })));
      sub.push_back(MenuItem::item("Delay\xE2\x80\xA6", guarded([id, si, vi](RollState& st) { (void)st.openJiTimingEntry(id, si, vi, true); })));
      sub.push_back(MenuItem::sep());
      sub.push_back(MenuItem::item("Same for All Voices", guarded([id, si, vi](RollState& st) { (void)st.setJiTimingForAll(id, si, vi); })));
      std::string name = vi < v->voiceLabels.size() && !v->voiceLabels[vi].empty() ? "Voice " + v->voiceLabels[vi] : "Voice " + std::to_string(vi + 1);
      for (const JiStopMark& m : v->stops)
        if (m.index == si) name += " into " + m.label;
      items.push_back(MenuItem::sub(std::move(name), std::move(sub)));
      items.push_back(MenuItem::sep());
    }
  }
  if (hit.zone == JiZone::StopLabel && hit.index >= 0) {
    const uint32_t si = static_cast<uint32_t>(hit.index);
    items.push_back(MenuItem::item("Retype Stop\xE2\x80\xA6", guarded([id, si](RollState& st) { (void)st.openJiStopRetype(id, si); }), "Double-click"));
    MenuItem del = MenuItem::item("Delete Stop", guarded([id, si](RollState& st) { (void)st.deleteJiStop(id, si); }), "Delete");
    del.enabled = o->stops.size() > 1;
    items.push_back(std::move(del));
    items.push_back(MenuItem::sep());
  }
  const std::optional<Ticks> tick = s_.jiStopTickAt(id, at.x);
  MenuItem add = MenuItem::item("Add Stop Here\xE2\x80\xA6", guarded([id, tick](RollState& st) {
                                  if (tick) (void)st.openJiStopEntry(id, *tick);
                                }),
                                entered ? "type it" : "");
  add.enabled = tick.has_value();
  items.push_back(std::move(add));
  MenuItem back = MenuItem::item("Back to Tuning Here", guarded([id, tick](RollState& st) {
                                   if (tick) (void)st.addJiTuningStopAt(id, *tick);
                                 }),
                                 "T");
  back.enabled = tick.has_value();
  items.push_back(std::move(back));
  items.push_back(MenuItem::sep());
  MenuItem under = MenuItem::item("Undertone (1/n)", guarded([id](RollState& st) { (void)st.toggleJiUndertone(id); }));
  under.checked = o->undertone;
  items.push_back(std::move(under));
  if (entered) items.push_back(MenuItem::item("Leave", guarded([](RollState& st) { st.leaveJiObject(); }), "Esc"));
  else items.push_back(MenuItem::item("Enter", guarded([id](RollState& st) { st.enterJiObject(id); }), "Double-click"));
  items.push_back(MenuItem::sep());
  items.push_back(MenuItem::item("Copy", guarded([id](RollState& st) {
                                   if (!st.jiEntered().valid()) st.selectJiObject(id);
                                   (void)st.copyJiSelection();
                                 }),
                                 "Ctrl+C"));
  if (!entered) {
    items.push_back(MenuItem::item("Duplicate", guarded([id](RollState& st) {
                                     st.selectJiObject(id);
                                     (void)st.duplicateJiSelection();
                                   }),
                                   "Ctrl+D"));
    // Removing the object gives the plain notes back (each at the pitch it has at its start).
    items.push_back(MenuItem::item(o->sounds ? "Delete JI Object" : "Remove JI Tuning", guarded([id](RollState& st) {
                                     st.selectJiObject(id);
                                     (void)st.deleteJiSelection();
                                   }),
                                   "Delete"));
  }
  items.push_back(MenuItem::sep());
  items.push_back(rowLabelsMenu(s_));
  return openContextMenu(*this, at, std::move(items));
}

MenuItem rowLabelsMenu(RollState& s) {
  std::shared_ptr<RollState*> alive = s.self();
  std::vector<MenuItem> sub;
  const RowLabels cur = s.rowLabels();
  const std::pair<const char*, RowLabels> modes[] = {{"Auto", RowLabels::Auto},       {"Note Names", RowLabels::Name},
                                                     {"Degrees", RowLabels::Degree},  {"Cents", RowLabels::Cents},
                                                     {"Ratios", RowLabels::Ratio}};
  for (const auto& [label, mode] : modes) {
    MenuItem m = MenuItem::item(label, [alive, mode = mode] {
      if (*alive) (*alive)->setRowLabels(mode);
    });
    m.checked = cur == mode;
    sub.push_back(std::move(m));
  }
  return MenuItem::sub("Row Labels", std::move(sub));
}

// --- keys -----------------------------------------------------------------------------------------------

KeysWidget::KeysWidget(RollState& s) : Control("keys"), s_(s) {
  setInfo("Keys", "Click a key to hear its pitch and select the notes on its row (Shift adds). Drag left/right to zoom "
                  "the rows; the mouse wheel scrolls.");
}

bool KeysWidget::onPointer(const PointerEvent& e) {
  if (!s_.hasClip() || s_.rows().empty()) return false;
  DragState& d = s_.drag;
  switch (e.type) {
    case PointerType::Down: {
      if (e.button == PointerButton::Right) {
        std::vector<MenuItem> items;
        items.push_back(rowLabelsMenu(s_));
        (void)openContextMenu(*this, e.pos, std::move(items));
        return true;
      }
      if (e.button != PointerButton::Left) return false;
      if (s_.noteArea && tree()) tree()->setFocus(s_.noteArea);
      const PitchRow& row = s_.rows().row(s_.rows().indexAtY(s_.contentY(e.pos.y)));
      s_.audition(s_.degreePitch(row.degree), 100.0f, 0, kPpq);
      s_.auditionHeld = true;  // AU.voices: released on pointer up
      s_.selectRow(row.degree, (e.mods & mod::kShift) != 0);
      d = {};
      d.kind = DragKind::KeysZoom;
      d.press = d.last = e.pos;
      d.zoomRowPx = s_.rowPx();
      return true;
    }
    case PointerType::Move:
      if (captured() && d.kind == DragKind::KeysZoom) {
        d.moved = d.moved || e.pos.x != d.press.x;
        const float scale = tree() ? tree()->scale() : 1.0f;
        const float dx = (e.pos.x - d.press.x) / scale;
        s_.setRowHeight(d.zoomRowPx + static_cast<int32_t>(std::lround(dx / 6.0f)));
        return true;
      }
      if (!captured()) s_.hoverAt(e.pos, true);
      return false;
    case PointerType::Up:
      if (d.kind == DragKind::KeysZoom) d = {};
      s_.releaseAudition();
      return true;
    case PointerType::Wheel:
      if (e.mods & mod::kCtrl) s_.setRowHeight(s_.rowPx() + static_cast<int32_t>(std::lround(e.wheel.y)));
      else s_.scrollByPx(0.0f, -e.wheel.y * 3.0f * static_cast<float>(s_.rows().laneHeight()));
      return true;
    default: return false;
  }
}

Cursor KeysWidget::cursorAt(Vec2) const {
  if (!s_.hasClip() || s_.rows().empty()) return Cursor::Inherit;
  return captured() && s_.drag.kind == DragKind::KeysZoom && s_.drag.moved ? Cursor::ResizeEW : Cursor::Pointer;
}

void KeysWidget::onCaptureLost() {
  s_.releaseAudition();
  if (s_.drag.kind == DragKind::KeysZoom) s_.drag = {};
}

void KeysWidget::onHoverChanged(bool hovered) {
  Control::onHoverChanged(hovered);
  if (!hovered) s_.clearHover();
}

InfoText KeysWidget::info() const {
  if (!s_.hovering()) return Control::info();
  return {s_.hoverTitle(), s_.hoverBody()};
}

// --- ruler ----------------------------------------------------------------------------------------------

RulerWidget::RulerWidget(RollState& s) : Control("ruler"), s_(s) {
  setInfo("Clip Ruler", "Drag the loop brace or its ends to move or resize the loop, the flags to move the clip's "
                        "start and end. Drag up/down elsewhere to zoom, left/right to scroll.");
}

RulerWidget::Hit RulerWidget::hitAt(Vec2 p) const {
  Hit h;
  const float scale = tree() ? tree()->scale() : 1.0f;
  const float grab = 5.0f * scale;
  if (p.y >= static_cast<float>(rect().y) + kBraceHeight * scale) return h;  // under the brace strip: zoom / scroll
  const doc::ClipLoop loop = s_.shownLoop();
  const float sx = s_.xOfTick(static_cast<double>(s_.shownContentStart()));
  const float ex = s_.xOfTick(static_cast<double>(s_.shownContentStart() + s_.shownLength()));
  const float lx0 = s_.xOfTick(static_cast<double>(loop.start)), lx1 = s_.xOfTick(static_cast<double>(loop.end));
  if (p.x >= sx - 1.0f && p.x <= sx + 2.0f * grab) {
    h.kind = DragKind::StartMarker;
    h.origin = s_.shownContentStart();
  } else if (!loop.enabled && std::fabs(p.x - ex) <= 2.0f * grab) {
    h.kind = DragKind::EndMarker;
    h.origin = s_.shownContentStart() + s_.shownLength();
  } else if (std::fabs(p.x - lx0) <= grab) {
    h.kind = DragKind::LoopBrace;
    h.brace = BraceEdge::Start;
    h.origin = loop.start;
  } else if (std::fabs(p.x - lx1) <= grab) {
    h.kind = DragKind::LoopBrace;
    h.brace = BraceEdge::End;
    h.origin = loop.end;
  } else if (p.x > lx0 && p.x < lx1) {
    h.kind = DragKind::LoopBrace;
    h.brace = BraceEdge::Both;
    h.origin = loop.start;
  }
  return h;
}

// The flags and the brace's ends drag sideways (↔), the whole brace moves; elsewhere a drag zooms (↕; sideways it
// scrolls).
Cursor RulerWidget::cursorAt(Vec2 p) const {
  if (!s_.hasClip()) return Cursor::Inherit;
  DragKind kind = DragKind::None;
  BraceEdge brace = BraceEdge::Both;
  if (captured() && s_.drag.kind != DragKind::None) {
    kind = s_.drag.kind;
    brace = s_.drag.brace;
  } else {
    const Hit h = hitAt(p);
    kind = h.kind;
    brace = h.brace;
  }
  switch (kind) {
    case DragKind::StartMarker:
    case DragKind::EndMarker: return Cursor::ResizeEW;
    case DragKind::LoopBrace: return brace == BraceEdge::Both ? Cursor::Move : Cursor::ResizeEW;
    default: return Cursor::ResizeNS;
  }
}

bool RulerWidget::onPointer(const PointerEvent& e) {
  if (!s_.hasClip()) return false;
  DragState& d = s_.drag;
  const float scale = tree() ? tree()->scale() : 1.0f;
  switch (e.type) {
    case PointerType::Down: {
      if (e.button != PointerButton::Left) return false;
      d = {};
      d.press = d.last = e.pos;
      d.pressTick = roundTicks(s_.tickAtX(e.pos.x));
      const Hit h = hitAt(e.pos);
      d.kind = h.kind;
      d.brace = h.brace;
      d.originTick = h.origin;
      if (d.kind == DragKind::RulerZoom) {
        d.zoomPxPerTick = s_.pxPerTick();
        d.zoomTick = s_.tickAtX(e.pos.x);
        d.pressScrollTick = s_.scrollTick();
      }
      return true;
    }
    case PointerType::Move: {
      if (!captured() || d.kind == DragKind::None) return false;
      d.moved = d.moved || moreThanSlop(e.pos, d.press, scale);
      if (!d.moved) return true;
      if (d.kind == DragKind::RulerZoom) {
        // Down zooms in (Live): ×2 per 60 logical px, around the tick under the press; sideways scrolls.
        const double factor = std::exp2(static_cast<double>(e.pos.y - d.press.y) / (60.0 * scale));
        const double ppt = d.zoomPxPerTick * factor;
        s_.setZoom(ppt, d.zoomTick, e.pos.x);
        return true;
      }
      const Grid g = s_.grid();
      const Ticks raw = roundTicks(s_.tickAtX(e.pos.x) - s_.tickAtX(d.press.x));
      d.dt = g.snapping() ? g.snapDelta(d.originTick, raw) : raw;
      if (d.kind == DragKind::EndMarker) d.dt = std::max(d.dt, 1 - s_.clip()->length);
      markDirty();
      s_.markNotesDirty();
      if (s_.boxPanel) s_.boxPanel->markDirty();
      return true;
    }
    case PointerType::Up: {
      if (e.button != PointerButton::Left) return false;
      if (d.kind == DragKind::RulerZoom || !d.moved) {
        d = {};
        return true;
      }
      s_.commitDrag();
      return true;
    }
    case PointerType::Wheel:
      s_.setZoom(s_.pxPerTick() * std::pow(1.25, static_cast<double>(e.wheel.y)), s_.tickAtX(e.pos.x), e.pos.x);
      return true;
    default: return false;
  }
}

void RulerWidget::onCaptureLost() { s_.cancelDrag(); }

// --- velocity lane --------------------------------------------------------------------------------------

VelocityLaneWidget::VelocityLaneWidget(RollState& s) : Control("velocity"), s_(s) {
  setInfo("Velocity Editor", "Drag a velocity marker up or down to change the velocity of the selected notes (with "
                             "Chance shown: their probability of playing).");
}

bool VelocityLaneWidget::onPointer(const PointerEvent& e) {
  if (!s_.hasClip()) return false;
  DragState& d = s_.drag;
  const float scale = tree() ? tree()->scale() : 1.0f;
  switch (e.type) {
    case PointerType::Down: {
      if (e.button != PointerButton::Left) return false;
      if (s_.noteArea && tree()) tree()->setFocus(s_.noteArea);
      const doc::Note* best = stemAt(e.pos);
      d = {};
      if (!best) {
        if (!(e.mods & (mod::kShift | mod::kCtrl))) s_.clearSelection();
        return true;
      }
      if (e.mods & (mod::kShift | mod::kCtrl)) {
        if (!s_.selected(best->id)) s_.toggleSelected(best->id);
      } else if (!s_.selected(best->id)) {
        s_.selectOnly(best->id);
      }
      d.kind = DragKind::Velocity;
      d.pressed = best->id;
      d.press = d.last = e.pos;
      s_.beginDragTracking();
      return true;
    }
    case PointerType::Move: {
      if (!captured()) {  // T4.17: hover highlight of the stem a press would grab
        const doc::Note* n = stemAt(e.pos);
        setHoverStem(n != nullptr ? n->id.value : 0u);
        return false;
      }
      if (d.kind != DragKind::Velocity) return false;
      const float usable = std::max(1.0f, static_cast<float>(rect().h) - 8.0f * scale);
      float dv = -(e.pos.y - d.press.y) / usable * valueRange();
      if (e.mods & mod::kShift) dv *= 0.1f;  // fine
      if (!s_.showChance()) dv = std::round(dv);
      d.dValue = dv;
      d.moved = true;
      s_.damageDrag();
      return true;
    }
    case PointerType::Up:
      if (e.button != PointerButton::Left) return false;
      if (d.kind == DragKind::Velocity) s_.commitDrag();
      return true;
    case PointerType::Wheel:
      s_.scrollByPx(-e.wheel.y * 60.0f * scale, 0.0f);
      return true;
    default: return false;
  }
}

const doc::Note* VelocityLaneWidget::stemAt(Vec2 p) const {
  const float grab = kStemGrab * (tree() ? tree()->scale() : 1.0f);
  const doc::Note* best = nullptr;
  float bestScore = 1e30f;
  const Ticks t0 = roundTicks(s_.tickAtX(p.x - grab)) - 1, t1 = roundTicks(s_.tickAtX(p.x + grab)) + 1;
  s_.forNotesIn(t0, t1, [&](const doc::Note& n) {
    if (n.start < t0 || n.start > t1) return;
    const float x = s_.xOfTick(static_cast<double>(n.start));
    if (std::fabs(x - p.x) > grab) return;
    const float score = std::fabs(x - p.x) * 4.0f + std::fabs(yOfValue(s_.previewValue(n)) - p.y);
    if (score < bestScore) {
      bestScore = score;
      best = &n;
    }
  });
  return best;
}

void VelocityLaneWidget::setHoverStem(uint32_t id) {
  if (id == hoverStem_) return;
  hoverStem_ = id;
  markDirty();  // the lane is short: one repaint of it per hover change (none while the pointer rests)
}

void VelocityLaneWidget::onHoverChanged(bool hovered) {
  if (!hovered) setHoverStem(0);
  Control::onHoverChanged(hovered);
}

Cursor VelocityLaneWidget::cursorAt(Vec2 p) const {
  if (!s_.hasClip()) return Cursor::Inherit;
  if (captured() && s_.drag.kind == DragKind::Velocity) return Cursor::ResizeNS;
  return stemAt(p) ? Cursor::ResizeNS : Cursor::Arrow;
}

bool VelocityLaneWidget::onKey(const KeyEvent& e) {
  if (e.down && e.key == key::kEscape && s_.drag.kind == DragKind::Velocity) {
    s_.cancelDrag();
    return true;
  }
  return false;
}

void VelocityLaneWidget::onCaptureLost() {
  if (s_.drag.kind == DragKind::Velocity) s_.cancelDrag();
}

// --- panels ---------------------------------------------------------------------------------------------

ClipBoxPanel::ClipBoxPanel(RollState& s) : Widget("clip-box"), s_(s) {
  setLayout(LayoutKind::Column);
  setAlign(Align::Start, Align::Start);
  setPadding({6, 24, 6, 6});  // the chip sits in the first row under the 20-px title bar
  tuning_ = &add<TuningChip>("tuning", s.ctx);
  tuning_->setHeight(Length::fixed(18));
  // T3.12: a "key  value" row per scale-view setting, shown only for MOS / Scala tunings (refresh()).
  auto row = [this](const char* name, const char* text) -> Widget& {
    Widget& w = add<Widget>(name);
    w.setLayout(LayoutKind::Row);
    w.setHeight(Length::fixed(18));
    w.setPadding({0, 2, 0, 0});
    w.add<Label>("label", text, FontRoleId::Small, TextTone::Secondary).setWidth(Length::flex(1));
    return w;
  };
  layoutRow_ = &row("layout-row", "Layout");
  layout_ = &layoutRow_->add<Button>("layout", "Proportional");
  layout_->setWidth(Length::fixed(84));
  layout_->setFocusable(false);
  layout_->setInfo("Pitch Layout", "Proportional spaces the scale's rows by their real intervals (the gaps are the steps); "
                                   "Equidistant gives every degree an equal row, like a chromatic piano roll.");
  layout_->onClick = [this] {
    s_.setScaleLayout(s_.scaleLayout() == RowLayout::Proportional ? RowLayout::Equidistant : RowLayout::Proportional);
  };
  gridRow_ = &row("grid-row", "EDO Grid");
  ValueSpec gs;
  gs.min = 0.0;
  gs.max = 311.0;
  gs.def = 0.0;
  gs.step = 1.0;
  gs.keyStep = 1.0;
  gs.dragPixels = 600.0;
  gs.decimals = 0;
  grid_ = &gridRow_->add<DragBox>("edo-grid", gs);
  grid_->setWidth(Length::fixed(64));
  grid_->formatter = [](double v) { return v < 0.5 ? std::string("Off") : std::to_string(std::lround(v)) + "-EDO"; };
  grid_->parser = [](std::string_view t) -> std::optional<double> {
    if (t == "Off" || t == "off") return 0.0;
    int v = 0;
    const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (ec != std::errc{} || p == t.data()) return std::nullopt;
    return static_cast<double>(v);
  };
  grid_->setSpec(gs);  // re-formats with the formatter
  grid_->setInfo("Background EDO Grid", "Shows an EDO of the octave behind the scale (0 = off). Cents notes step and "
                                        "move on its lines; Alt in Draw Mode draws Cents notes on them.");
  grid_->onChange = [this](double v) { s_.setOverlayEdo(static_cast<uint32_t>(std::lround(std::max(0.0, v)))); };
  genRow_ = &row("generator-row", "Generator");
  ValueSpec ps;
  ps.min = 0.0;
  ps.max = 1200.0;
  ps.def = 700.0;
  ps.dragPixels = 2400.0;  // 0.5 ¢ per px over a 1200-¢ period (Shift: 0.05)
  ps.keyStep = 0.1;
  ps.decimals = 2;
  ps.unit = "\xC2\xA2";
  gen_ = &genRow_->add<DragBox>("generator", ps);
  gen_->setWidth(Length::fixed(64));
  gen_->setInfo("Generator", "Drag to morph the MOS: rows and notes follow live, past a region's end the next MOS is "
                             "taken (as in the MOS wheel). It edits the clip's named tuning: every clip and track on "
                             "it follows. Releasing commits it (one undo step).");
  gen_->onGestureBegin = [this] { s_.beginGeneratorMorph(); };
  gen_->onChange = [this](double v) { s_.morphGenerator(v); };
  // T3.x-fix-edit (M3 review M12b, Q92.6): release commits and hands the keys back to the notes (Up / Down then
  // transpose notes instead of nudging the generator by 0.1 ¢ per key, each a tuning edit); Esc during
  // the drag restores the document's tuning and records nothing.
  gen_->onGestureEnd = [this] {
    s_.endGeneratorMorph(true);
    if (s_.noteArea && tree() && gen_->focused()) tree()->setFocus(s_.noteArea);
  };
  gen_->onGestureCancel = [this] {
    s_.endGeneratorMorph(false);
    if (s_.noteArea && tree() && gen_->focused()) tree()->setFocus(s_.noteArea);
  };
  labelsRow_ = &row("labels-row", "Row Labels");
  labels_ = &labelsRow_->add<Dropdown>("row-labels", std::vector<std::string>{"Auto", "Names", "Degrees", "Cents", "Ratios"}, 0);
  labels_->setWidth(Length::fixed(64));
  labels_->setFocusable(false);
  labels_->setInfo("Row Labels", "What the keys show on each row: Auto (ratios for a just Scala scale, else names), note "
                                 "names / k\\n steps, degrees, cents within the equave, or ratios (exact for a Scala "
                                 "ratio, else the simplest ratio nearby, marked ~).");
  labels_->onChange = [this](int32_t i) { s_.setRowLabels(static_cast<RowLabels>(std::clamp(i, 0, 4))); };
  jiRow_ = &row("ji-row", "Harmonics");
  ji_ = &jiRow_->add<Toggle>("ji", "JI", ToggleStyle::Accent);
  ji_->setWidth(Length::fixed(34));
  ji_->setFocusable(false);
  ji_->setInfo("JI Guide", "Shows the clip's virtual fundamental F (the line labelled 1/D) and its harmonics, numbered in "
                           "the strip at the note area's right edge; Up / Down step ratio notes along them.");
  ji_->onToggle = [this](bool on) { s_.setShowHarmonics(on); };
  auto& spacer = add<Widget>();
  spacer.setHeight(Length::flex(1));
  loop_ = &add<Toggle>("loop", "Loop", ToggleStyle::Accent);
  loop_->setSize(Length::fixed(56), Length::fixed(18));
  loop_->setFocusable(false);
  loop_->setInfo("Loop Switch", "Plays the clip's loop brace in a loop (the ruler's brace sets its start and end).");
  loop_->onToggle = [this](bool on) {
    const doc::NoteClip* c = s_.clip();
    if (!c) return;
    doc::ClipLoop l = c->loop;
    l.enabled = on;
    if (Status st = s_.ctx.store().dispatch(doc::SetClipLoop{s_.clipRef(), l}); !st) s_.ctx.setStatus("Loop: " + st.error().message);
  };
}

void ClipBoxPanel::refresh() {
  if (const doc::NoteClip* c = s_.clip()) loop_->setOn(c->loop.enabled);
  tuning_->setTarget(s_.hasClip() ? std::optional<TuningTarget>(TuningTarget::forClip(s_.clipRef())) : std::nullopt);
  const bool scale = s_.hasClip() && s_.rows().scaleLayout();
  const tuning::Mos* mos = scale ? s_.tuning().mos() : nullptr;
  if (layoutRow_->visible() != scale) layoutRow_->setVisible(scale);
  if (gridRow_->visible() != scale) gridRow_->setVisible(scale);
  if (genRow_->visible() != (mos != nullptr)) genRow_->setVisible(mos != nullptr);
  if (scale) {
    layout_->setText(s_.scaleLayout() == RowLayout::Proportional ? "Proportional" : "Equidistant");
    if (!grid_->inGesture()) grid_->setValue(static_cast<double>(s_.overlayEdo()));
  }
  labels_->setIndex(static_cast<int32_t>(s_.rowLabels()));
  const bool ji = s_.hasClip() && s_.harmonics().has_value();
  if (jiRow_->visible() != ji) jiRow_->setVisible(ji);
  ji_->setOn(s_.showHarmonics());
  if (mos && !gen_->inGesture()) {
    const tuning::MosParams& p = mos->params();
    // The default (Delete, double-click reset) is the clip's committed generator, not a fixed 700 ¢: in a period-600
    // MOS 700 ¢ is outside the period's MOS range altogether (M3 review M12b).
    if (gen_->spec().max != p.period || gen_->spec().def != p.generator) {
      ValueSpec sp = gen_->spec();
      sp.max = p.period;
      sp.dragPixels = 2.0 * p.period;
      sp.def = p.generator;
      gen_->setSpec(sp);
    }
    gen_->setValue(p.generator);
  }
  markDirty();
}

CornerPanel::CornerPanel(RollState& s) : Widget("corner"), s_(s) {
  setLayout(LayoutKind::Row);
  setPadding({4, 4, 4, 4});
  setGap(3);
  setAlign(Align::Start, Align::Start);
  fold_ = &add<Toggle>("fold", "Fold", ToggleStyle::Accent);
  fold_->setSize(Length::fixed(34), Length::fixed(16));
  fold_->setFocusable(false);
  fold_->setInfo("Fold", "Shows only the rows that have notes (with Scale on: only the rows of the scale).");
  fold_->onToggle = [this](bool on) { s_.setFold(on); };
  scale_ = &add<Toggle>("scale", "Scale", ToggleStyle::Accent);
  scale_->setSize(Length::fixed(36), Length::fixed(16));
  scale_->setFocusable(false);
  scale_->setInfo("Scale", "Highlights the rows of the scale (for an EDO: its diatonic scale from the patent fifth).");
  scale_->onToggle = [this](bool on) { s_.setScaleHighlight(on); };
}

void CornerPanel::refresh() {
  fold_->setOn(s_.fold());
  scale_->setOn(s_.scaleHighlight());
  markDirty();
}

LaneHeaderWidget::LaneHeaderWidget(RollState& s) : Widget("lane-header"), s_(s) {
  setLayout(LayoutKind::Column);
  setPadding({4, 4, 4, 4});
  setGap(3);
  title_ = &add<Label>("title", "Velocity", FontRoleId::Small, TextTone::Secondary);
  title_->setHeight(Length::fixed(14));
  chance_ = &add<Toggle>("chance", "Chance", ToggleStyle::Accent);
  chance_->setSize(Length::fixed(48), Length::fixed(16));
  chance_->setFocusable(false);
  chance_->setInfo("Chance", "Shows and edits each note's probability of playing instead of its velocity.");
  chance_->onToggle = [this](bool on) { s_.setShowChance(on); };
}

void LaneHeaderWidget::refresh() {
  title_->setText(s_.showChance() ? "Chance" : "Velocity");
  chance_->setOn(s_.showChance());
  markDirty();
}

}  // namespace pq::views::roll
