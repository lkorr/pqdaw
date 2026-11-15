// Pointer and key input of the clip view's parts (T3.11). Live's MIDI editor gestures:
//   note area   click selects (Shift / Ctrl toggles), drag moves (time snapped relative to the grid, pitch by rows),
//               Alt-drag copies, the note's ends resize, a drag on empty space draws a selection rectangle,
//               double-click adds / deletes a note; in draw mode (B) a click adds a grid-cell note (or deletes the
//               note under it) and a drag adds one per cell crossed. Alt draws Cents notes on the background EDO,
//               Shift draws Ratio notes on the virtual fundamental's harmonics (T3.19). Right-click: the note menu
//               (convert identity, set pitch, Is Harmonic…, row labels). Wheel scrolls (Shift: horizontally),
//               Ctrl+wheel zooms time, Ctrl+Alt+wheel zooms rows. Esc cancels a drag. Hover: the pitch readout.
//               T7.13: "Is Harmonic…" declares the note the anchor of a JI group; with an active group the plain
//               draw puts members on its harmonics (Shift: the rows).
//               T7.6 (roll_pen.cpp): Alt + a note's left / right edge bends it in from the previous note / out to
//               the next one; Shift + a note's body detunes freely (cents, after a dead zone); in Draw Mode a drag on
//               a note draws its pitch curve, Alt + drag paints vibrato, Ctrl + click adds a breakpoint (a click —
//               or a drag of a few px — still deletes the note); the breakpoints of the selected notes drag (Shift:
//               free), a double-click deletes one, the diamond between two bends the segment.
//               T7.14, what a press finds first: the proportion tool's box (J armed), a vibrato grip (the ~ above a
//               note's end, the envelope's corners under it; roll_vibrato.cpp), a warp marker / glide grip / frame
//               corner grip (roll_groups.cpp), a breakpoint, a note, a group's header (only where no note is), then
//               empty space. A frame itself takes nothing: inside it notes and empty space are what they are.
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
#include "roll_help.h"
#include "pq/tuning/ji.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "roll_playhead.h"
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
          "deletes a note; B toggles Draw Mode. Up/Down move by one step of the tuning, Shift+Up/Down by one equave. "
          "Alt+drag a note's edge to glide it in from the previous note or out to the next; drag the ~ above a note's "
          "end for vibrato. Ctrl+Shift+J tunes the selected chord to just intonation and back. The ? button lists "
          "everything.");
}

void NoteAreaWidget::onFocusChanged(bool focused) {
  if (focused) s_.reportResolvedGrid();
}

void NoteAreaWidget::pressNote(const PointerEvent& e, const doc::Note& n, int zone) {
  const bool toggle = (e.mods & (mod::kShift | mod::kCtrl)) != 0;
  s_.selectWarp({});  // T7.14: the notes have the keys again
  DragState& d = s_.drag;
  d = {};
  d.pressedWasSelected = s_.selected(n.id);
  d.pressMods = e.mods;
  if (toggle && d.pressedWasSelected && zone == 0 && !(e.mods & mod::kCtrl)) {
    d.toggleOnClick = true;  // T7.6: Shift on a selected note: a click deselects it, a drag detunes freely
  } else if (toggle) {
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
  s_.beginDragTracking();
  // T7.16a (M7 review M14): a group's anchor dragged with its whole group: its members follow it in the preview.
  // bug-ji-input: an anchor dragged without all its members moves alone (as a member: the harmonics), they stay.
  if (d.kind == DragKind::Move && s_.clip()->groups) {
    s_.cacheSelectedNotes();
    for (const uint32_t id : s_.selection())  // sorted: so are the two lists
      if (const doc::Note* m = s_.findNote(doc::NoteId{id}); m && s_.groupRole(*m) == 2)
        (s_.groupSelectedWhole(m->id) ? d.followAnchors : d.aloneAnchors).push_back(id);
    d.movesAnchors = !d.followAnchors.empty();
    if (d.movesAnchors) d.trackDamage = false;  // the members repaint too
  }
  s_.audition(n.pitch, n.velocity, n.start, n.length);
  s_.auditionHeld = true;  // AU.voices: released on pointer up
}

// The note a draw gesture puts in cell `c` at device y: a Degree note on the row there; with Overlay (Alt, while a
// background EDO is shown) a Cents note on the overlay line nearest the pointer's pitch (T3.12); with Ratio (Shift,
// T3.19) a Ratio note on the virtual fundamental's harmonic nearest the pointer (RollState::ratioFor).
DrawnNote NoteAreaWidget::cellNote(TickRange c, Vec2 p, DrawPitch mode, Mods mods) const {
  DrawnNote dn{c.start, c.end - c.start, 0};
  const RowModel& rows = s_.rows();
  if (rows.empty()) return dn;
  const int32_t cy = s_.contentY(p.y);
  if (mode == DrawPitch::Overlay && s_.overlay()) {
    dn.mode = DrawPitch::Overlay;
    dn.cents = s_.nearestOverlayCents(rows.centsAtY(static_cast<double>(cy)));
    dn.degree = rows.placeCents(dn.cents).nearestDegree;
  } else if (mode == DrawPitch::Ratio) {
    dn.mode = DrawPitch::Ratio;
    const int64_t row = rows.row(rows.indexAtY(cy)).degree;
    // T7.16a: the group the note joins here (its harmonics; with Alt the finer series, M5), else the reference's.
    const doc::JiGroup* g = s_.captureGroupAt(c.start, p.y);
    const bool shift = (mods & mod::kShift) != 0;
    std::optional<RollState::Harmonics> h =
        g && !shift ? s_.groupHarmonicsOf(*g, (mods & mod::kAlt) != 0) : s_.referenceHarmonics();  // the ratio is to its base
    if (h) h->baseCents += s_.harmonicsShift(*h, static_cast<double>(c.start));  // the guides there (a moving anchor)
    dn.ratio = s_.ratioFor(row, rows.centsAtY(static_cast<double>(cy)), h);
    if (h && h->anchor.valid()) dn.anchor = h->anchor;
    dn.degree = rows.placeCents(tuning::cents(dn.ratio) + (h ? h->baseCents : 0.0)).nearestDegree;
  } else {
    dn.degree = rows.row(rows.indexAtY(cy)).degree;
  }
  return dn;
}

void NoteAreaWidget::pressEmpty(const PointerEvent& e) {
  s_.selectWarp({});
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
    d.pressMods = e.mods;
    d.drawn.push_back(cellNote(c, e.pos, s_.drawPitchFor(e.mods, c.start, e.pos.y), e.mods));
    d.trackDamage = true;  // no notes tracked: the drawn cells are
    s_.audition(s_.drawnPitch(d.drawn.back()), 100.0f, d.drawn.back().start, c.end - c.start);
    s_.auditionHeld = true;  // AU.voices: released on pointer up
    s_.damageDrag();
    return;
  }
  s_.setInsertTick(g.snapping() ? g.snap(t) : t);
  d.kind = DragKind::Rubber;
  d.pressSelection = s_.selection();
  d.additive = (e.mods & (mod::kShift | mod::kCtrl)) != 0;
  if (!d.additive) s_.clearSelection();
  d.base = s_.selection();
  d.rubber = {static_cast<int32_t>(e.pos.x), static_cast<int32_t>(e.pos.y), 0, 0};
}

// T7.6: Draw Mode on a note: a drag draws its pitch curve, Alt + drag paints vibrato, Ctrl + click adds a breakpoint
// and drags it. False: nothing there for the pen (the press draws notes as before).
bool NoteAreaWidget::pressPen(const PointerEvent& e) {
  const doc::Note* n = s_.noteAt(e.pos);
  if (!n) return false;
  const CurveTarget t{n->id};
  if (e.mods & mod::kCtrl) return s_.beginCurvePoint(t, e.pos);
  s_.beginPen(t, e.pos, e.mods, (e.mods & mod::kAlt) != 0);
  if (s_.drag.kind == DragKind::None) return false;
  s_.drag.deleteOnClick = true;
  return true;
}

void NoteAreaWidget::dragTo(Vec2 p, Mods mods) {
  DragState& d = s_.drag;
  const float scale = tree() ? tree()->scale() : 1.0f;
  if (!d.moved && !moreThanSlop(p, d.press, scale)) return;
  if (!d.moved) s_.markNotesDirty();  // T7.14: the vibrato grips of the selected notes go while another gesture runs
  d.moved = true;
  d.last = p;
  d.travel = std::max(d.travel, std::max(std::fabs(p.x - d.press.x), std::fabs(p.y - d.press.y)));
  const Grid g = s_.grid();
  const Ticks raw = roundTicks(s_.tickAtX(p.x) - s_.tickAtX(d.press.x));
  switch (d.kind) {
    case DragKind::Move:
    case DragKind::Copy: {
      d.dt = g.snapping() ? g.snapDelta(d.originTick, raw) : raw;
      d.cents = 0.0;
      if (d.kind == DragKind::Move && (mods & mod::kShift) && !s_.rows().empty()) {
        // T7.6: Shift = free detune: cents instead of rows (the notes become Cents notes). T7.14 (M7 review H5): only
        // past a vertical dead zone, so a Shift + click that slips, or a Shift drag along the time, detunes nothing.
        d.steps = 0;
        const float dead = RollState::kDetuneDeadZone * scale;
        const float dy = p.y - d.press.y;
        if (std::fabs(dy) > dead) d.cents = std::round(s_.centsAtScreenY(p.y) - s_.centsAtScreenY(d.press.y + std::copysign(dead, dy)));
        d.trackDamage = false;
        char buf[64];
        if (d.cents != 0.0) std::snprintf(buf, sizeof buf, "Detune %+.0f \xC2\xA2", d.cents);
        else std::snprintf(buf, sizeof buf, "Detune: drag up or down");
        s_.ctx.setStatus(buf);
      } else if (!s_.rows().empty()) {
        const int64_t deg = s_.rows().row(s_.rows().indexAtY(s_.contentY(p.y))).degree;
        d.steps = static_cast<int32_t>(std::clamp<int64_t>(deg - d.pressDegree, INT32_MIN / 2, INT32_MAX / 2));
        // T7.14: a member of a JI group snaps to its group's harmonics: the one nearest the pointer.
        if (const doc::Note* pressed = s_.findNote(d.pressed))
          if (const std::optional<int32_t> h = s_.harmonicDragSteps(*pressed, p.y + d.grabY)) d.steps = *h;
      }
      s_.updatePreviewPitches();
      break;
    }
    case DragKind::ResizeEnd:
    case DragKind::ResizeStart:
      d.dt = g.snapping() ? g.snapDelta(d.originTick, raw) : raw;
      // T7.6 (A3): with Alt the edge stays and the note glides in from the previous note / out to the next one over
      // the dragged length (previewed from the edit the release dispatches).
      d.bend = (mods & mod::kAlt) != 0;
      if (d.bend) {
        d.trackDamage = false;
        d.bendCents = s_.freeBendCents(p, mods);  // a note with no neighbour there bends from / to the pointer's pitch
        s_.updateBendPreview();
      } else {
        s_.clearCurvePreview();
      }
      break;
    case DragKind::PenDraw:
    case DragKind::PenVibrato: s_.penTo(p, mods); break;
    case DragKind::CurvePoint:
    case DragKind::CurveBow: s_.curveHandleTo(p, mods); break;
    case DragKind::Vibrato:
    case DragKind::VibratoCorner: s_.vibratoDragTo(p, mods); break;
    case DragKind::WarpNew:
    case DragKind::WarpMove:
    case DragKind::WarpGlide: s_.warpDragTo(p, mods); break;
    case DragKind::ProportionBox: {
      const int32_t x0 = static_cast<int32_t>(std::min(p.x, d.press.x)), x1 = static_cast<int32_t>(std::max(p.x, d.press.x));
      const int32_t y0 = static_cast<int32_t>(std::min(p.y, d.press.y)), y1 = static_cast<int32_t>(std::max(p.y, d.press.y));
      d.rubber = {x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
      s_.markNotesDirty();
      break;
    }
    case DragKind::Rubber: {
      const int32_t x0 = static_cast<int32_t>(std::min(p.x, d.press.x)), x1 = static_cast<int32_t>(std::max(p.x, d.press.x));
      const int32_t y0 = static_cast<int32_t>(std::min(p.y, d.press.y)), y1 = static_cast<int32_t>(std::max(p.y, d.press.y));
      d.rubber = {x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
      const int32_t cy0 = s_.contentY(static_cast<float>(y0)), cy1 = s_.contentY(static_cast<float>(y1));
      std::vector<uint32_t> ids = d.base;
      const Ticks t0 = roundTicks(s_.tickAtX(static_cast<float>(x0))), t1 = roundTicks(s_.tickAtX(static_cast<float>(x1)));
      s_.forNotesIn(t0, t1 + 1, [&](const doc::Note& n) {
        if (s_.onPath(n, n.pitch)) {  // where the note is drawn
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
    case DragKind::Draw: {
      if (s_.rows().empty()) break;
      const TickRange c = drawCell(g, roundTicks(s_.tickAtX(p.x)));
      const bool have = std::any_of(d.drawn.begin(), d.drawn.end(), [&](const DrawnNote& n) { return n.start == c.start; });
      if (!have) {
        // Each cell as its own press would (T7.16a: inside a group's span it joins it, outside it is on the rows).
        d.drawn.push_back(cellNote(c, p, s_.drawPitchFor(d.pressMods, c.start, p.y), d.pressMods));
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
  if (e.type != PointerType::Wheel) s_.setPointerMods(e.mods);  // T7.16a: Alt shows the finer series, widens the bend edges
  switch (e.type) {
    case PointerType::Down: {
      if (e.button == PointerButton::Right) {
        if (tree()) tree()->setFocus(this);
        (void)openMenu(e.pos);
        return true;
      }
      if (e.button != PointerButton::Left) return false;
      if (tree()) tree()->setFocus(this);
      // T7.14: the proportion tool (J): this press starts its box, whatever is under it.
      if (s_.proportionArmed()) {
        s_.setHoverPart({});
        DragState& d = s_.drag;
        d = {};
        d.kind = DragKind::ProportionBox;
        d.press = d.last = e.pos;
        d.rubber = {static_cast<int32_t>(e.pos.x), static_cast<int32_t>(e.pos.y), 1, 1};
        return true;
      }
      // T7.14: a warp marker, its glide grip, a frame's corner grip (on the frame's bottom border). T7.16a (M7 review
      // H4): before the vibrato grips, so a warp is always reachable (the frame also keeps its bottom clear of them).
      const GroupHit gh = s_.groupPartAt(e.pos);
      if (gh.part == GroupPart::Marker || gh.part == GroupPart::GlideGrip || gh.part == GroupPart::LeftGrip || gh.part == GroupPart::RightGrip) {
        s_.setHoverPart({});
        s_.beginWarpDrag(gh, e.pos);
        return true;
      }
      // T7.14: a vibrato grip (they lie outside the note's band: above its end, under its envelope).
      if (const RollState::VibratoHit vh = s_.vibratoPartAt(e.pos); vh.valid()) {
        if (e.clickCount >= 2 && vh.part == RollState::VibratoPart::Grip) {
          s_.drag = {};
          (void)s_.removeVibrato(vh.note);
        } else {
          s_.beginVibratoDrag(vh, e.pos);
        }
        return true;
      }
      s_.setHoverPart({});  // a gesture shows its own preview
      // T7.6: a breakpoint of a curve being shown (the selected notes): drag it, a
      // double-click deletes it; the diamond between two points bends the segment. A note's edge comes first (a
      // bend's first / last point lies on it; its handle reaches further into the note).
      if (const RollState::CurveHit ch = s_.curveHandleAt(e.pos); ch.valid()) {
        int edgeZone = 0;
        if (!s_.ctx.drawMode()) (void)s_.noteAt(e.pos, &edgeZone);
        if (edgeZone == 0) {
          if (e.clickCount >= 2 && !ch.mid) {
            s_.drag = {};
            (void)s_.deleteCurvePoint(ch);
          } else {
            s_.beginCurveHandle(ch, e.pos);
          }
          return true;
        }
      }
      int zone = 0;
      const doc::Note* n = s_.noteAt(e.pos, &zone, (e.mods & mod::kAlt) != 0);  // M13: Alt widens the bend edges
      if (s_.ctx.drawMode() && pressPen(e)) return true;  // T7.6: the pen on a note
      if (n && (s_.ctx.drawMode() || (e.clickCount >= 2 && !(e.mods & (mod::kShift | mod::kCtrl))))) {
        // Draw mode click / double-click on a note: delete it.
        s_.drag = {};
        s_.selectOnly(n->id);
        s_.deleteSelection();
        return true;
      }
      if (n) {
        pressNote(e, *n, zone);
        // Where the press holds the note's line (a member dragged vertically snaps to the harmonic under its line).
        if (const std::optional<double> c = s_.pathCents(*n, n->start, n->pitch, std::clamp(s_.tickAtX(e.pos.x) - static_cast<double>(n->start), 0.0, static_cast<double>(n->length))))
          s_.drag.grabY = s_.yOfCents(*c) - e.pos.y;
      } else if (gh.part == GroupPart::Header || gh.part == GroupPart::Proportion) {
        // T7.14: a group's header (where no note is): its notes are selected. T7.16a (M7 review H2): a double-click
        // on a tuned group's proportion opens the chooser — a click to select the chord for a transpose leaves the
        // arrow keys to the notes (an open chooser would take them).
        s_.drag = {};
        s_.selectGroup(gh.group);
        if (gh.part == GroupPart::Proportion && e.clickCount >= 2) (void)s_.openJiChooser(gh.group);
        else if (gh.part == GroupPart::Proportion) s_.ctx.setStatus("The chord is selected: Up / Down or a drag transposes it; double-click its proportion for its other readings");
      } else if (e.clickCount >= 2 && !s_.ctx.drawMode()) {
        // Double-click on empty space: one note in the grid cell.
        s_.drag = {};
        const Grid g = s_.grid();
        const TickRange c = drawCell(g, roundTicks(s_.tickAtX(e.pos.x)));
        if (!s_.rows().empty()) {
          const DrawnNote dn = cellNote(c, e.pos, s_.drawPitchFor(e.mods, c.start, e.pos.y), e.mods);
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
      // T7.14: grips and markers.
      if (d.kind == DragKind::ProportionBox) {
        const DragState was = d;
        s_.drag = {};
        s_.armProportionTool(false);
        if (!s_.rows().empty()) {
          // The box in ticks (whole grid cells; a click: one bar from its cell) and its bottom row.
          const Grid g = s_.grid();
          const float xa = was.moved ? static_cast<float>(was.rubber.x) : was.press.x;
          const float xb = was.moved ? static_cast<float>(was.rubber.right()) : was.press.x;
          const TickRange first = drawCell(g, std::max<Ticks>(roundTicks(s_.tickAtX(xa)), 0));
          const TickRange last = drawCell(g, std::max<Ticks>(roundTicks(s_.tickAtX(xb)) - (was.moved ? 1 : 0), first.start));
          Ticks end = std::max(last.end, first.end);
          if (!was.moved) end = first.start + kTicksPerWhole * s_.clip()->signature.numerator / std::max(1, static_cast<int>(s_.clip()->signature.denominator));
          const float yb = was.moved ? static_cast<float>(was.rubber.bottom()) - 1.0f : was.press.y;
          const int64_t bottom = s_.rows().row(s_.rows().indexAtY(s_.contentY(yb))).degree;
          const IRect anchor = was.moved ? was.rubber : IRect{static_cast<int32_t>(was.press.x), static_cast<int32_t>(was.press.y), 1, 1};
          (void)s_.openProportionEntry(first.start, end - first.start, bottom, anchor);
        }
        s_.markNotesDirty();
        return true;
      }
      if (!d.moved && (d.kind == DragKind::Vibrato || d.kind == DragKind::VibratoCorner || d.kind == DragKind::WarpNew || d.kind == DragKind::WarpGlide ||
                       d.kind == DragKind::WarpMove)) {
        const DragState was = d;
        s_.cancelDrag();
        if (was.kind == DragKind::WarpMove) s_.selectWarp({was.group, was.warpIndex});  // a click selects the marker (Delete removes it)
        else if (was.kind == DragKind::WarpNew) s_.ctx.setStatus(was.fromLeft ? "Drag this corner inward: the chord plays as written up to there, then glides into JI"
                                                                               : "Drag this corner inward: from there the chord glides back to the tuning");
        else if (was.kind == DragKind::Vibrato) s_.ctx.setStatus("Vibrato: drag this grip up for depth, sideways for speed; double-click removes it");
        s_.hoverPartAt(e.pos);
        return true;
      }
      // T7.14 (M7 review M7): a pen drag of a few px is a click: it erases the note, as Draw Mode's click does.
      const float penMin = RollState::kMinPenStroke * (tree() ? tree()->scale() : 1.0f);
      if (d.moved && (d.kind == DragKind::PenDraw || d.kind == DragKind::PenVibrato) && d.travel < penMin) d.moved = false;
      if (!d.moved && d.toggleOnClick) {  // T7.6: Shift + click on a selected note deselects it (on release)
        const doc::NoteId id = d.pressed;
        s_.drag = {};
        s_.toggleSelected(id);
        s_.hoverPartAt(e.pos);
        return true;
      }
      if (!d.moved && (d.kind == DragKind::PenDraw || d.kind == DragKind::PenVibrato || (d.kind == DragKind::CurvePoint && !d.curveAdded) ||
                       d.kind == DragKind::CurveBow)) {
        // T7.6: a click with the pen. On a note it deletes the note (Draw Mode's click); on a breakpoint it does
        // nothing.
        const DragState was = d;
        s_.cancelDrag();
        if (was.kind == DragKind::PenDraw || was.kind == DragKind::PenVibrato) {
          if (was.deleteOnClick) {
            s_.selectOnly(was.curve.note);
            s_.deleteSelection();
          }
        }
        s_.hoverPartAt(e.pos);
        return true;
      }
      if (!d.moved && (d.kind == DragKind::Move || d.kind == DragKind::Copy) && d.pressedWasSelected &&
          !(e.mods & (mod::kShift | mod::kCtrl))) {
        const doc::NoteId only = d.pressed;  // read before the reset (`d` is s_.drag; the click used to select note 0)
        s_.drag = {};
        s_.selectOnly(only);  // a click on a note of a multiple selection selects just it (Live)
        s_.hoverPartAt(e.pos);
        return true;
      }
      if (!d.moved && d.kind != DragKind::Draw && !(d.kind == DragKind::CurvePoint && d.curveAdded)) {
        s_.drag = {};
        s_.clearCurvePreview();
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
  // T7.16a (M5 / M13): Alt pressed or let go over the roll: the finer series of the group under the pointer.
  if ((e.key == key::kLAlt || e.key == key::kRAlt) && !e.repeat) {
    s_.setPointerMods(e.down ? (e.mods | mod::kAlt) : (e.mods & ~mod::kAlt));
    return false;
  }
  if (e.down && e.key == key::kEscape && (s_.drag.kind != DragKind::None || s_.proportionArmed())) {
    s_.armProportionTool(false);  // T7.14: Esc also puts the proportion tool away
    s_.abortDrag();
    if (tree() && tree()->captured() == this) tree()->releaseCapture();
    return true;
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
      case DragKind::Draw:
      case DragKind::PenDraw:
      case DragKind::PenVibrato: return Cursor::Crosshair;
      case DragKind::CurvePoint: return Cursor::Move;
      case DragKind::CurveBow: return Cursor::ResizeNS;
      case DragKind::Rubber: return Cursor::Arrow;
      case DragKind::Vibrato: return Cursor::Move;
      case DragKind::VibratoCorner:
      case DragKind::WarpNew:
      case DragKind::WarpMove:
      case DragKind::WarpGlide: return Cursor::ResizeEW;
      case DragKind::ProportionBox: return Cursor::Crosshair;
      default: break;
    }
  // T7.14: as the press: the proportion tool, a vibrato grip, a warp marker / grip.
  if (s_.proportionArmed()) return Cursor::Crosshair;
  const GroupHit gh = s_.groupPartAt(p);
  if (gh.part == GroupPart::Marker || gh.part == GroupPart::GlideGrip || gh.part == GroupPart::LeftGrip || gh.part == GroupPart::RightGrip) return Cursor::ResizeEW;
  if (const RollState::VibratoHit vh = s_.vibratoPartAt(p); vh.valid()) return vh.part == RollState::VibratoPart::Grip ? Cursor::Move : Cursor::ResizeEW;
  // T7.6: a breakpoint (drag it), the diamond between two (bend the segment); a note's edge comes first, as the press.
  if (const RollState::CurveHit ch = s_.curveHandleAt(p); ch.valid()) {
    int edgeZone = 0;
    if (!s_.ctx.drawMode()) (void)s_.noteAt(p, &edgeZone);
    if (edgeZone == 0) return ch.mid ? Cursor::ResizeNS : Cursor::Move;
  }
  int zone = 0;
  const doc::Note* under = s_.noteAt(p, &zone, (s_.pointerMods() & mod::kAlt) != 0);
  if (!under && (gh.part == GroupPart::Header || gh.part == GroupPart::Proportion)) return Cursor::Pointer;  // selects the group / opens the chooser
  if (s_.ctx.drawMode()) return Cursor::Crosshair;  // a click adds a cell's note (or deletes the one under it)
  if (!under) return Cursor::Arrow;  // empty space: a drag draws the selection rectangle
  return zone != 0 ? Cursor::ResizeEW : Cursor::Move;
}

InfoText NoteAreaWidget::info() const {
  if (!s_.hovering()) return Control::info();
  return {s_.hoverTitle(), s_.hoverBody()};
}

// The right-click menu (T3.19, M3 review H7): identity conversions, a typed pitch and "Is Harmonic…" for the
// selected notes (a note under the pointer that is not selected becomes the selection, as Live), else paste / select
// all; the row labels for any press.
Menu* NoteAreaWidget::openMenu(Vec2 at) {
  std::shared_ptr<RollState*> alive = s_.self();
  auto guarded = [alive](std::function<void(RollState&)> f) {
    return [alive, f = std::move(f)] {
      if (*alive) f(**alive);
    };
  };
  // T7.16a (M7 review L3): the keys the LOADED keymap gives the actions, as the help panel shows them.
  auto keyOf = [this](std::string_view action) { return helpActionKeys(action, s_.ctx.keymap()); };
  // T7.14: a warp marker (or its glide grip): its shape, or away with it.
  if (const GroupHit gh = s_.groupPartAt(at); gh.part == GroupPart::Marker || gh.part == GroupPart::GlideGrip) {
    const GroupInfo* g = s_.groupInfo(gh.group);
    if (g && gh.warp >= 0 && static_cast<size_t>(gh.warp) < g->group->warps.size()) {
      s_.selectWarp({gh.group, gh.warp});
      const doc::GlideShape::Kind kind = g->group->warps[static_cast<size_t>(gh.warp)].shape.kind;
      std::vector<MenuItem> items;
      MenuItem straight = MenuItem::item("Straight", guarded([gh](RollState& st) { (void)st.setWarpShape(gh.group, gh.warp, doc::GlideShape::LogLinear); }));
      straight.checked = kind == doc::GlideShape::LogLinear;
      MenuItem scurve = MenuItem::item("S-Curve", guarded([gh](RollState& st) { (void)st.setWarpShape(gh.group, gh.warp, doc::GlideShape::SCurve); }));
      scurve.checked = kind != doc::GlideShape::LogLinear;
      items.push_back(std::move(straight));
      items.push_back(std::move(scurve));
      items.push_back(MenuItem::sep());
      items.push_back(MenuItem::item("Remove Warp", guarded([gh](RollState& st) { (void)st.removeWarp(gh.group, gh.warp); }), keyOf("pianoroll.delete")));
      return openContextMenu(*this, at, std::move(items));
    }
  }
  const doc::Note* under = s_.noteAt(at);
  if (under && !s_.selected(under->id)) s_.selectOnly(under->id);
  std::vector<MenuItem> items;
  // Ids of notes that are gone (an undo) may linger in the selection: the menu is for notes that are there.
  if (!s_.selection().empty() && s_.selectionKeys().empty()) s_.select({});
  const std::vector<uint32_t>& sel = s_.selection();
  const Ticks atTick = roundTicks(s_.tickAtX(at.x));
  // T7.14: the warp entry for the group whose frame the pointer is in (a tuned group): the next transition moves
  // here, or a new one is made.
  // T7.14: the proportion tool's mouse path: a typed chord from the cell under the pointer, one bar long, its lowest
  // note on this row.
  auto proportionItem = [&] {
    if (s_.rows().empty()) return;
    const TickRange cell = drawCell(s_.grid(), std::max<Ticks>(atTick, 0));
    const Ticks bar = kTicksPerWhole * s_.clip()->signature.numerator / std::max(1, static_cast<int>(s_.clip()->signature.denominator));
    const int64_t row = s_.rows().row(s_.rows().indexAtY(s_.contentY(at.y))).degree;
    const IRect anchor{static_cast<int32_t>(at.x), static_cast<int32_t>(at.y), 1, 1};
    items.push_back(MenuItem::item("Insert Proportion\xE2\x80\xA6", guarded([cell, bar, row, anchor](RollState& st) { (void)st.openProportionEntry(cell.start, bar, row, anchor); }),
                                   keyOf("pianoroll.proportionTool")));
  };
  auto warpHere = [&](const GroupInfo* g) {
    if (!g || !g->tuned || !g->inJi || atTick < g->start || atTick > g->end) return;
    const double state = s_.warpStateAt(*g, atTick);
    if (state != 0.0 && state != 1.0) return;
    const Grid gr = s_.grid();
    const Ticks tick = gr.snapping() ? gr.snap(atTick) : atTick;
    const doc::JiGroupId id = g->group->id;
    items.push_back(MenuItem::item(state == 0.0 ? "Back to Tuning Here" : "Into JI Here", guarded([id, tick](RollState& st) { (void)st.insertWarpAt(id, tick); })));
  };
  if (!sel.empty()) {
    // Which identities the selection holds.
    bool kinds[3] = {false, false, false};
    const doc::Note* single = nullptr;
    for (const doc::Note& n : s_.clip()->notes)
      if (s_.selected(n.id)) {
        kinds[static_cast<size_t>(n.pitch.kind())] = true;
        single = &n;
      }
    if (sel.size() != 1) single = nullptr;
    const int nKinds = kinds[0] + kinds[1] + kinds[2];
    auto conv = [&](const char* label, tuning::PitchKind k) {
      MenuItem m = MenuItem::item(label, guarded([k](RollState& st) { (void)st.convertSelection(k); }));
      m.checked = nKinds == 1 && kinds[static_cast<size_t>(k)];
      m.enabled = !(nKinds == 1 && kinds[static_cast<size_t>(k)]);
      return m;
    };
    items.push_back(conv("Degree (moves with the tuning)", tuning::PitchKind::Degree));
    items.push_back(conv("Ratio (stays at its just pitch)", tuning::PitchKind::Ratio));
    items.push_back(conv("Cents (never moves)", tuning::PitchKind::Cents));
    items.push_back(MenuItem::item("Set Pitch\xE2\x80\xA6", guarded([](RollState& st) { (void)st.openPitchEntry(); })));
    items.push_back(MenuItem::sep());
    // T7.14: just intonation. Tune to JI is a switch: on notes that already are one tuned group it reads Back to
    // Tuning. "Is Harmonic…" declares a note (a member: it becomes its group's frozen note).
    const doc::JiGroup* active = s_.activeJiGroup();
    const GroupInfo* info = active ? s_.groupInfo(active->id) : nullptr;
    bool inOne = info != nullptr;  // every selected note is of the active group
    if (inOne)
      for (const doc::Note& n : s_.clip()->notes)
        if (s_.selected(n.id) && doc::jiGroupOfNote(*s_.clip(), n) != active) inOne = false;
    const bool back = inOne && info->inJi;  // T7.16a: the group's state decides, not its written identities (M3)
    const bool oneTuned = inOne && info->tuned;
    MenuItem tune = MenuItem::item(back ? "Back to Tuning" : "Tune to JI", guarded([](RollState& st) { (void)st.toggleTuneToJi(); }), keyOf("pianoroll.tuneToJi"));
    tune.enabled = inOne || sel.size() >= 2;
    items.push_back(std::move(tune));
    if (oneTuned) {
      const doc::JiGroupId id = active->id;
      items.push_back(MenuItem::item("Other Readings\xE2\x80\xA6", guarded([id](RollState& st) { (void)st.openJiChooser(id); })));
    }
    if (const doc::Note* target = under ? under : single) {
      const doc::NoteId id = target->id;
      items.push_back(MenuItem::item("Is Harmonic\xE2\x80\xA6", guarded([id](RollState& st) { (void)st.openHarmonicEntry(id); })));
      MenuItem ratio = MenuItem::item("Add Note at Ratio\xE2\x80\xA6", guarded([id](RollState& st) { (void)st.openRatioEntry(id); }));
      ratio.enabled = doc::jiGroupOfNote(*s_.clip(), *target) != nullptr;
      items.push_back(std::move(ratio));
    }
    MenuItem up = MenuItem::item("Walk Up", guarded([](RollState& st) { (void)st.walkActiveGroup(1); }), keyOf("pianoroll.walkUp"));
    MenuItem down = MenuItem::item("Walk Down", guarded([](RollState& st) { (void)st.walkActiveGroup(-1); }), keyOf("pianoroll.walkDown"));
    up.enabled = down.enabled = active != nullptr;
    items.push_back(std::move(up));
    items.push_back(std::move(down));
    warpHere(info);
    MenuItem unlock = MenuItem::item("Remove JI Group", guarded([](RollState& st) { (void)st.removeActiveJiGroup(); }));
    unlock.enabled = active != nullptr;
    items.push_back(std::move(unlock));
    items.push_back(MenuItem::sep());
    // T7.6 / T7.14: bends. The bend of the note under the pointer (Alt + its edge made it) can be straight or an
    // S-curve; the selection glides, loses its bends and curves. (Vibrato has no menu: the ~ grip on the note.)
    bool anyCurve = false;
    for (const uint32_t id : sel) anyCurve = anyCurve || s_.curveRecord(doc::NoteId{id}) != nullptr;
    if (const doc::Note* target = under ? under : single) {
      const doc::NoteId id = target->id;
      for (const int edge : {-1, 1}) {
        if (s_.bendLength(id, edge) <= 0) continue;
        const bool straight = s_.bendIsStraight(id, edge);
        std::vector<MenuItem> sub;
        MenuItem a = MenuItem::item("Straight", guarded([edge](RollState& st) { (void)st.setBendShape(edge, doc::GlideShape::LogLinear); }));
        a.checked = straight;
        MenuItem b = MenuItem::item("S-Curve", guarded([edge](RollState& st) { (void)st.setBendShape(edge, doc::GlideShape::SCurve); }));
        b.checked = !straight;
        sub.push_back(std::move(a));
        sub.push_back(std::move(b));
        items.push_back(MenuItem::sub(edge < 0 ? "Bend In" : "Bend Out", std::move(sub)));
      }
    }
    MenuItem glide = MenuItem::item("Glide into Next", guarded([](RollState& st) { (void)st.glideSelection(); }), keyOf("pianoroll.glide"));
    glide.enabled = sel.size() >= 2;
    items.push_back(std::move(glide));
    MenuItem clear = MenuItem::item("Clear Curves", guarded([](RollState& st) { (void)st.clearBends(); }));  // L2: bends, drawn curves, vibrato
    clear.enabled = anyCurve;
    items.push_back(std::move(clear));
    items.push_back(MenuItem::sep());
    MenuItem dup = MenuItem::item("Duplicate", guarded([](RollState& st) { st.duplicateSelection(); }), keyOf("pianoroll.duplicate"));
    items.push_back(std::move(dup));
    items.push_back(MenuItem::item("Quantize", guarded([](RollState& st) { st.quantizeSelection(); }), keyOf("pianoroll.quantize")));
    items.push_back(MenuItem::item("Delete", guarded([](RollState& st) { st.deleteSelection(); }), keyOf("pianoroll.delete")));
    // T7.16a (M7 review L1): on empty space the proportion tool is offered with a selection too.
    if (!under) {
      items.push_back(MenuItem::sep());
      proportionItem();
    }
  } else {
    items.push_back(MenuItem::item("Paste", guarded([](RollState& st) { st.paste(); }), keyOf("pianoroll.paste")));
    items.push_back(MenuItem::item("Select All", guarded([](RollState& st) { st.selectAll(); }), keyOf("pianoroll.selectAll")));
    items.push_back(MenuItem::sep());
    proportionItem();
    const doc::JiGroupId framed = s_.groupFrameAt(at);
    if (const GroupInfo* g = framed.valid() ? s_.groupInfo(framed) : nullptr) {
      const doc::JiGroupId id = g->group->id;
      items.push_back(MenuItem::item("Select Group", guarded([id](RollState& st) { st.selectGroup(id); })));
      warpHere(g);
    }
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
  setInfo("Clip Ruler", "The thin strip above the bar numbers: click to play from there (while playing: jump there), "
                        "snapped to the grid, Alt: exactly where you click; drag to scrub. The numbers: drag up or "
                        "down to zoom, sideways to scroll. Drag the loop brace or its ends to move or resize the "
                        "loop, the flags to move the clip's start and end.");
}

RulerWidget::Hit RulerWidget::hitAt(Vec2 p) const {
  Hit h;
  const float scale = tree() ? tree()->scale() : 1.0f;
  const float grab = 5.0f * scale;
  const float braceBottom = static_cast<float>(rect().y) + kBraceHeight * scale;
  if (p.y >= braceBottom + kScrubHeight * scale) return h;  // the numbers row: zoom / scroll
  if (p.y >= braceBottom) {  // bug-paused-playhead: the scrub strip
    h.kind = DragKind::RulerScrub;
    return h;
  }
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

// The flags and the brace's ends drag sideways (↔), the whole brace moves; the scrub strip plays from the press and
// scrubs; the numbers row zooms (↕) and scrolls sideways.
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
    case DragKind::RulerScrub: return Cursor::Pointer;
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
      d.pressMods = e.mods;
      if (d.kind == DragKind::RulerZoom) {
        d.zoomPxPerTick = s_.pxPerTick();
        d.zoomTick = s_.tickAtX(e.pos.x);
        d.pressScrollTick = s_.scrollTick();
      }
      // bug-paused-playhead: the scrub strip plays from the press (Live), then follows the drag.
      if (d.kind == DragKind::RulerScrub) d.scrubSong = roll::scrubAt(s_, s_.tickAtX(e.pos.x), e.mods);
      return true;
    }
    case PointerType::Move: {
      if (!captured() || d.kind == DragKind::None) return false;
      if (!d.moved) {
        if (!moreThanSlop(e.pos, d.press, scale)) return true;
        d.moved = true;
      }
      if (d.kind == DragKind::RulerScrub) {
        d.scrubSong = roll::scrubAt(s_, s_.tickAtX(e.pos.x), e.mods, d.scrubSong);
        return true;
      }
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
      if (d.kind == DragKind::RulerZoom || d.kind == DragKind::RulerScrub || !d.moved) {
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
    s_.focusNotes();  // T7.16a (M10): the roll keeps its keys
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
  labels_->onChange = [this](int32_t i) {
    s_.setRowLabels(static_cast<RowLabels>(std::clamp(i, 0, 4)));
    s_.focusNotes();
  };
  jiRow_ = &row("ji-row", "Harmonics");
  ji_ = &jiRow_->add<Toggle>("ji", "JI", ToggleStyle::Accent);
  ji_->setWidth(Length::fixed(34));
  ji_->setFocusable(false);
  ji_->setInfo("Draw on Harmonics", "On: inside a JI group's time span the notes you draw snap to its harmonics (drawn as "
                                    "numbered guide lines there) and join the group. Off: every note goes on the "
                                    "tuning's rows, and no guides show.");
  ji_->onToggle = [this](bool on) {
    s_.setShowHarmonics(on);
    s_.focusNotes();
  };
  auto& spacer = add<Widget>();
  spacer.setHeight(Length::flex(1));
  loop_ = &add<Toggle>("loop", "Loop", ToggleStyle::Accent);
  loop_->setSize(Length::fixed(56), Length::fixed(18));
  loop_->setFocusable(false);
  loop_->setInfo("Loop Switch", "Plays the clip's loop brace in a loop (the ruler's brace sets its start and end).");
  loop_->onToggle = [this](bool on) {
    s_.focusNotes();
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
  // (Shown whenever there is a series to snap to, switched on or off: T7.16a, the switch is what decides snapping.)
  const bool ji = s_.hasClip() && (s_.clip()->groups != nullptr || s_.referenceHarmonics().has_value());
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
  fold_->onToggle = [this](bool on) {
    s_.setFold(on);
    s_.focusNotes();  // T7.16a (M10): the roll keeps its keys
  };
  scale_ = &add<Toggle>("scale", "Scale", ToggleStyle::Accent);
  scale_->setSize(Length::fixed(36), Length::fixed(16));
  scale_->setFocusable(false);
  scale_->setInfo("Scale", "Highlights the rows of the scale (for an EDO: its diatonic scale from the patent fifth).");
  scale_->onToggle = [this](bool on) {
    s_.setScaleHighlight(on);
    s_.focusNotes();
  };
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
  chance_->onToggle = [this](bool on) {
    s_.setShowChance(on);
    s_.focusNotes();
  };
}

void LaneHeaderWidget::refresh() {
  title_->setText(s_.showChance() ? "Chance" : "Velocity");
  chance_->setOn(s_.showChance());
  markDirty();
}

}  // namespace pq::views::roll
