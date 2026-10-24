// Pointer and key input of the clip view's parts (T3.11). Live's MIDI editor gestures:
//   note area   click selects (Shift / Ctrl toggles), drag moves (time snapped relative to the grid, pitch by rows),
//               Alt-drag copies, the note's ends resize, a drag on empty space draws a selection rectangle,
//               double-click adds / deletes a note; in draw mode (B) a click adds a grid-cell note (or deletes the
//               note under it) and a drag adds one per cell crossed. Wheel scrolls (Shift: horizontally), Ctrl+wheel
//               zooms time, Ctrl+Alt+wheel zooms rows. Esc cancels a drag.
//   keys        click auditions the row and selects its notes (Shift adds); horizontal drag zooms the rows.
//   ruler       drag the loop brace (its ends or the whole brace), the start marker and the end marker; elsewhere a
//               vertical drag zooms time around the pointer and a horizontal drag scrolls.
//   velocity    drag a stem (or chance bar) up / down: every selected note changes by the same amount.
// Every gesture is previewed and dispatched once on release: one undo step.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

#include "pq/ui/theme.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

namespace {

constexpr float kClickSlop = 3.0f;  // device px the pointer may wander before a press becomes a drag

bool moreThanSlop(Vec2 a, Vec2 b, float scale) noexcept {
  return std::fabs(a.x - b.x) > kClickSlop * scale || std::fabs(a.y - b.y) > kClickSlop * scale;
}

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
  s_.beginDragTracking();
  s_.audition(n.pitch, n.velocity, n.start, n.length);
}

// The note a draw gesture puts in cell `c` at device y: a Degree note on the row there, or with `onOverlay` (Alt, while
// a background EDO is shown) a Cents note on the overlay line nearest the pointer's pitch (T3.12).
DrawnNote NoteAreaWidget::cellNote(TickRange c, float y, bool onOverlay) const {
  DrawnNote dn{c.start, c.end - c.start, 0};
  const RowModel& rows = s_.rows();
  if (rows.empty()) return dn;
  const int32_t cy = s_.contentY(y);
  if (onOverlay && s_.overlay()) {
    dn.onOverlay = true;
    dn.cents = s_.nearestOverlayCents(rows.centsAtY(static_cast<double>(cy)));
    dn.degree = rows.placeCents(dn.cents).nearestDegree;
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
    d.drawn.push_back(cellNote(c, e.pos.y, (e.mods & mod::kAlt) != 0));
    d.trackDamage = true;  // no notes tracked: the drawn cells are
    s_.audition(s_.drawnPitch(d.drawn.back()), 100.0f, c.start, c.end - c.start);
    s_.damageDrag();
    return;
  }
  s_.setInsertTick(g.snapping() ? g.snap(t) : t);
  d.kind = DragKind::Rubber;
  d.additive = (e.mods & (mod::kShift | mod::kCtrl)) != 0;
  if (!d.additive) s_.clearSelection();
  d.base = s_.selection();
  d.rubber = {static_cast<int32_t>(e.pos.x), static_cast<int32_t>(e.pos.y), 0, 0};
}

void NoteAreaWidget::dragTo(Vec2 p) {
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
      if (!s_.rows().empty()) {
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
      s_.forNotesIn(t0, t1 + 1, [&](const doc::Note& n) {
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
        d.drawn.push_back(cellNote(c, p.y, !d.drawn.empty() && d.drawn.front().onOverlay));
        s_.audition(s_.drawnPitch(d.drawn.back()), 100.0f, c.start, c.end - c.start);
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
      if (e.button != PointerButton::Left) return false;
      if (tree()) tree()->setFocus(this);
      int zone = 0;
      const doc::Note* n = s_.noteAt(e.pos, &zone);
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
          const DrawnNote dn = cellNote(c, e.pos.y, (e.mods & mod::kAlt) != 0);
          s_.addNotes({dn});
          s_.audition(s_.drawnPitch(dn), 100.0f, c.start, c.end - c.start);
        }
      } else {
        pressEmpty(e);
      }
      return true;
    }
    case PointerType::Move:
      if (captured() && s_.drag.kind != DragKind::None) {
        dragTo(e.pos);
        return true;
      }
      return false;
    case PointerType::Up: {
      if (e.button != PointerButton::Left) return false;
      DragState& d = s_.drag;
      if (d.kind == DragKind::None) return true;
      if (!d.moved && (d.kind == DragKind::Move || d.kind == DragKind::Copy) && d.pressedWasSelected &&
          !(e.mods & (mod::kShift | mod::kCtrl))) {
        s_.drag = {};
        s_.selectOnly(d.pressed);  // a click on a note of a multiple selection selects just it (Live)
        return true;
      }
      if (!d.moved && d.kind != DragKind::Draw) {
        s_.drag = {};
        s_.markNotesDirty();
        return true;
      }
      s_.commitDrag();
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
    s_.cancelDrag();
    if (tree() && tree()->captured() == this) tree()->releaseCapture();
    return true;
  }
  return false;
}

void NoteAreaWidget::onCaptureLost() { s_.cancelDrag(); }

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
      if (e.button != PointerButton::Left) return false;
      if (s_.noteArea && tree()) tree()->setFocus(s_.noteArea);
      const PitchRow& row = s_.rows().row(s_.rows().indexAtY(s_.contentY(e.pos.y)));
      s_.audition(s_.degreePitch(row.degree), 100.0f, 0, kPpq);
      s_.selectRow(row.degree, (e.mods & mod::kShift) != 0);
      d = {};
      d.kind = DragKind::KeysZoom;
      d.press = d.last = e.pos;
      d.zoomRowPx = s_.rowPx();
      return true;
    }
    case PointerType::Move:
      if (captured() && d.kind == DragKind::KeysZoom) {
        const float scale = tree() ? tree()->scale() : 1.0f;
        const float dx = (e.pos.x - d.press.x) / scale;
        s_.setRowHeight(d.zoomRowPx + static_cast<int32_t>(std::lround(dx / 6.0f)));
        return true;
      }
      return false;
    case PointerType::Up:
      if (d.kind == DragKind::KeysZoom) d = {};
      return true;
    case PointerType::Wheel:
      if (e.mods & mod::kCtrl) s_.setRowHeight(s_.rowPx() + static_cast<int32_t>(std::lround(e.wheel.y)));
      else s_.scrollByPx(0.0f, -e.wheel.y * 3.0f * static_cast<float>(s_.rows().laneHeight()));
      return true;
    default: return false;
  }
}

void KeysWidget::onCaptureLost() {
  if (s_.drag.kind == DragKind::KeysZoom) s_.drag = {};
}

// --- ruler ----------------------------------------------------------------------------------------------

RulerWidget::RulerWidget(RollState& s) : Control("ruler"), s_(s) {
  setInfo("Clip Ruler", "Drag the loop brace or its ends to move or resize the loop, the flags to move the clip's "
                        "start and end. Drag up/down elsewhere to zoom, left/right to scroll.");
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
      const float grab = 5.0f * scale;
      const bool inBrace = e.pos.y < static_cast<float>(rect().y) + kBraceHeight * scale;
      const doc::ClipLoop loop = s_.shownLoop();
      const float sx = s_.xOfTick(static_cast<double>(s_.shownContentStart()));
      const float ex = s_.xOfTick(static_cast<double>(s_.shownContentStart() + s_.shownLength()));
      const float lx0 = s_.xOfTick(static_cast<double>(loop.start)), lx1 = s_.xOfTick(static_cast<double>(loop.end));
      if (inBrace && e.pos.x >= sx - 1.0f && e.pos.x <= sx + 2.0f * grab) {
        d.kind = DragKind::StartMarker;
        d.originTick = s_.shownContentStart();
      } else if (inBrace && !loop.enabled && std::fabs(e.pos.x - ex) <= 2.0f * grab) {
        d.kind = DragKind::EndMarker;
        d.originTick = s_.shownContentStart() + s_.shownLength();
      } else if (inBrace && std::fabs(e.pos.x - lx0) <= grab) {
        d.kind = DragKind::LoopBrace;
        d.brace = BraceEdge::Start;
        d.originTick = loop.start;
      } else if (inBrace && std::fabs(e.pos.x - lx1) <= grab) {
        d.kind = DragKind::LoopBrace;
        d.brace = BraceEdge::End;
        d.originTick = loop.end;
      } else if (inBrace && e.pos.x > lx0 && e.pos.x < lx1) {
        d.kind = DragKind::LoopBrace;
        d.brace = BraceEdge::Both;
        d.originTick = loop.start;
      } else {
        d.kind = DragKind::RulerZoom;
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
      // The stem nearest the pointer within reach; among stems at the same x the one whose head is closest.
      const float grab = kStemGrab * scale;
      const doc::Note* best = nullptr;
      float bestScore = 1e30f;
      const Ticks t0 = roundTicks(s_.tickAtX(e.pos.x - grab)) - 1, t1 = roundTicks(s_.tickAtX(e.pos.x + grab)) + 1;
      s_.forNotesIn(t0, t1, [&](const doc::Note& n) {
        if (n.start < t0 || n.start > t1) return;
        const float x = s_.xOfTick(static_cast<double>(n.start));
        if (std::fabs(x - e.pos.x) > grab) return;
        const float score = std::fabs(x - e.pos.x) * 4.0f + std::fabs(yOfValue(s_.previewValue(n)) - e.pos.y);
        if (score < bestScore) {
          bestScore = score;
          best = &n;
        }
      });
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
      if (!captured() || d.kind != DragKind::Velocity) return false;
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
                             "taken (as in the MOS wheel). Releasing commits it to this clip (one undo step).");
  gen_->onGestureBegin = [this] { s_.beginGeneratorMorph(); };
  gen_->onChange = [this](double v) { s_.morphGenerator(v); };
  gen_->onGestureEnd = [this] { s_.endGeneratorMorph(true); };
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
  if (mos && !gen_->inGesture()) {
    const tuning::MosParams& p = mos->params();
    if (gen_->spec().max != p.period) {
      ValueSpec sp = gen_->spec();
      sp.max = p.period;
      sp.dragPixels = 2.0 * p.period;
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
