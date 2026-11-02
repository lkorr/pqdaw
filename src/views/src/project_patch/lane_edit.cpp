// TX.lane-edit (bug report 2026-11-01_13-50-52): the project patch view's lane windows are an editing surface of their
// tracks. No second implementation: the arrangement's Editor (selection, time selection, clip move / copy / trim, audio
// handles, the edit commands and their menu) and ArrangementAutomation (breakpoints, curves, the envelope menu) run on
// the windows through arrangement::EditSurface — one selection, one clipboard, one undo history for both views. This
// file is the surface (rows = the windows, a time axis per window) and the routing of the canvas's pointer and keys.
//
// CPU: nothing runs unless the pointer or a key is in a window; the rows are rebuilt once per document version or model
// rebuild (a vector of the windows), the geometry is computed per query from the model's nodes (a handful of multiplies).

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#include "../arrangement/internal.h"
#include "internal.h"
#include "pq/views/arrangement/arrangement.h"

namespace pq::views::project_patch {

using ui::Rect;
using ui::Vec2;

// ---- the surface ------------------------------------------------------------------------------------------------

ui::Widget& LaneEditSurface::widget() const { return view_.canvas(); }

bool LaneEditSurface::active() const { return view_.tree() != nullptr && view_.shown(); }

void LaneEditSurface::sync() const {
  const doc::Snapshot& now = view_.context().doc();
  if (doc_ == now && built_ == view_.stats().rebuilds) return;
  doc_ = now;
  built_ = view_.stats().rebuilds;
  const ProjectPatch& pp = view_.patch();
  rows_.clear();
  bands_.clear();
  rowOfBand_.assign(pp.bands.size(), -1);
  for (uint32_t b = 0; b < pp.bands.size(); ++b) {
    if (!hasLaneWindow(pp.bands[b]) || pp.bands[b].source == kNone) continue;
    const doc::Track* t = view_.bandTrack(b);
    if (t == nullptr) continue;
    ArrangementRow row;
    row.id = t->id;
    row.track = t;
    row.tuning = view_.bandTuning(b);
    rowOfBand_[b] = static_cast<int32_t>(rows_.size());
    rows_.push_back(row);
    bands_.push_back(b);
  }
}

std::span<const ArrangementRow> LaneEditSurface::rows() const {
  sync();
  return rows_;
}

uint32_t LaneEditSurface::bandOf(int32_t row) const {
  sync();
  return row >= 0 && static_cast<size_t>(row) < bands_.size() ? bands_[static_cast<size_t>(row)] : kNone;
}

int32_t LaneEditSurface::rowOfBand(uint32_t band) const {
  sync();
  return band < rowOfBand_.size() ? rowOfBand_[band] : -1;
}

Rect LaneEditSurface::body(int32_t row) const {
  const uint32_t b = bandOf(row);
  if (b == kNone) return {};
  const ProjectPatch& pp = view_.patch();
  return laneBodyDevice(view_.canvas(), pp.nodes[pp.bands[b].source]);
}

// Windows sit in one column but bands may be nudged: a linear scan (a handful of rows, a few multiplies each).
int32_t LaneEditSurface::rowAt(float y) const {
  const auto n = static_cast<int32_t>(rows().size());
  for (int32_t r = 0; r < n; ++r) {
    const Rect b = body(r);
    if (y >= b.y0 && y < b.y1) return r;
  }
  return -1;
}

int32_t LaneEditSurface::clampRow(float y) const {
  const auto n = static_cast<int32_t>(rows().size());
  int32_t best = -1;
  float bestD = 0.0f;
  for (int32_t r = 0; r < n; ++r) {
    const Rect b = body(r);
    const float d = y < b.y0 ? b.y0 - y : (y >= b.y1 ? y - b.y1 + 1.0f : 0.0f);
    if (best < 0 || d < bestD) {
      best = r;
      bestD = d;
    }
  }
  return best;
}

// The editor draws and hit-tests a row's clips over [y + 1, bottom − 1): the body plus a pixel above and below.
ui::IRect LaneEditSurface::rowLaneRect(int32_t row) const {
  const Rect b = body(row);
  if (!(b.x1 > b.x0)) return {};
  const ui::IRect r = toIRect(b);
  return {r.x, r.y - 1, r.w, r.h + 2};
}

ui::IRect LaneEditSurface::viewport() const { return view_.canvas().rect(); }

arrangement::TimeAxis LaneEditSurface::axis(int32_t row) const {
  const int32_t n = static_cast<int32_t>(rows().size());
  if (n == 0) return {};
  const Rect b = body(std::clamp(row, 0, n - 1));
  const Ticks span = std::max<Ticks>(1, view_.laneSpan());
  const double k = std::max(1e-9, static_cast<double>(b.x1 - b.x0) / static_cast<double>(span));
  return {static_cast<double>(b.x0), static_cast<double>(view_.laneStart()), k};
}

double LaneEditSurface::gridPxPerTick() const {
  const float us = view_.tree() != nullptr ? view_.tree()->scale() : 1.0f;
  return axis(0).k / static_cast<double>(us);
}

int32_t LaneEditSurface::clipTitleHeight(int32_t row) const {
  const Rect b = body(row);
  const float us = view_.tree() != nullptr ? view_.tree()->scale() : 1.0f;
  return static_cast<int32_t>(laneClipTitleH(b.y1 - b.y0, us));
}

// The windows page so the tick shows (the insert marker moved by the arrow keys, Home / End).
void LaneEditSurface::revealTick(Ticks t) {
  const Ticks start = view_.laneStart(), span = view_.laneSpan();
  if (span <= 0 || (t >= start && t < start + span)) return;
  const TimeSigMap& sig = view_.context().doc()->timeline->sigMap;
  const Ticks bar = sig.barToTicks(sig.ticksToBarBeat(std::max<Ticks>(0, t)).bar);
  view_.setLaneTime(t < start ? bar : std::max<Ticks>(0, bar + ticksPerBar(sig.signatureAt(bar)) - span), span);
}

void LaneEditSurface::damageAll() { view_.damageLanes(); }

// Appended to the arrangement's edit menu in a window: automation mode and the parameter drawn over this track's clips.
std::vector<ui::MenuItem> LaneEditSurface::menuItems() {
  std::vector<ui::MenuItem> items;
  ArrangementView* arr = view_.laneArrangement();
  if (arr == nullptr) return items;
  ArrangementAutomation& au = arr->automation();
  ui::MenuItem mode = ui::MenuItem::item("Automation Mode", {}, "A");
  mode.action = "arrangement.toggleAutomationMode";
  mode.checked = au.mode();
  items.push_back(std::move(mode));
  const auto rs = rows();
  if (menuRow >= 0 && static_cast<size_t>(menuRow) < rs.size())
    items.push_back(ui::MenuItem::sub("Draw Automation", au.parameterItems(rs[static_cast<size_t>(menuRow)].id)));
  return items;
}

// ---- the view's routing -------------------------------------------------------------------------------------------

LaneEditSurface* ProjectPatchView::laneEditReady() const {
  if (embedded()) return nullptr;
  ArrangementView* arr = arrangement();
  if (arr == nullptr) return nullptr;
  if (!laneEdit_) laneEdit_ = std::make_unique<LaneEditSurface>(const_cast<ProjectPatchView&>(*this));
  if (laneEdit_->set() != &arr->surfaces()) {
    if (laneEdit_->set() != nullptr) laneEdit_->set()->remove(*laneEdit_);
    arr->surfaces().add(*laneEdit_);
  }
  laneArr_ = arr;
  return laneEdit_.get();
}

ArrangementView* ProjectPatchView::laneArrangement() const {
  return laneEdit_ && laneEdit_->set() != nullptr ? laneArr_ : nullptr;
}

arrangement::EditSurface* ProjectPatchView::laneSurface() const noexcept { return laneEditReady(); }

uint32_t ProjectPatchView::laneBodyAt(Vec2 device) const {
  if (laneSpan_ <= 0 || embedded()) return kNone;
  for (uint32_t i = 0; i < patch_.bands.size(); ++i) {
    const Band& b = patch_.bands[i];
    if (!hasLaneWindow(b) || b.source == kNone) continue;
    const Rect r = laneBodyDevice(*canvas_, patch_.nodes[b.source]);
    if (device.x >= r.x0 && device.x < r.x1 && device.y >= r.y0 && device.y < r.y1) return i;
  }
  return kNone;
}

bool ProjectPatchView::lanePointer(const ui::PointerEvent& e) {
  LaneEditSurface* s = laneEditReady();
  if (s == nullptr) return false;
  ArrangementView& arr = *laneArr_;
  arrangement::Surfaces::Bind bind(arr.surfaces(), s);
  if (e.type == ui::PointerType::Down) s->menuRow = s->rowAt(e.pos.y);
  // As the arrangement's lanes: the automation (its envelopes over the clips in automation mode) takes a press first.
  bool handled = arr.automation().pointer(e);
  if (!handled) {
    if (e.type == ui::PointerType::Down) arr.automation().clearSelection();
    handled = arr.editor().pointer(e);
  }
  return handled;
}

bool ProjectPatchView::laneDragging() const noexcept {
  ArrangementView* arr = laneArrangement();
  if (arr == nullptr) return false;
  const arrangement::EditSurface* s = laneEdit_.get();
  return arr->editor().dragSurface() == s || arr->automation().dragSurface() == s;
}

void ProjectPatchView::laneHover(Vec2 pos, ui::Mods mods) {
  LaneEditSurface* s = laneEditReady();
  if (s == nullptr || laneDragging()) return;
  ArrangementView& arr = *laneArr_;
  arrangement::Surfaces::Bind bind(arr.surfaces(), s);
  if (arr.automation().hoverAt(pos, mods)) arr.editor().clearHover();
  else arr.editor().hoverAt(pos);
}

void ProjectPatchView::clearLaneHover() {
  ArrangementView* arr = laneArrangement();
  if (arr == nullptr || laneDragging()) return;
  arrangement::Surfaces::Bind bind(arr->surfaces(), laneEdit_.get());
  arr->editor().clearHover();
  arr->automation().clearHover();
}

ui::Cursor ProjectPatchView::laneCursor(Vec2 pos, ui::Mods mods) const {
  LaneEditSurface* s = laneEditReady();
  if (s == nullptr) return ui::Cursor::Arrow;
  ArrangementView& arr = *laneArr_;
  arrangement::Surfaces::Bind bind(arr.surfaces(), s);
  const arrangement::Editor& ed = arr.editor();
  const ArrangementAutomation& au = arr.automation();
  if (ed.dragging() && !au.dragging()) return ed.cursorAt(pos);
  if (const std::optional<ui::Cursor> c = au.cursorAt(pos, mods)) return *c;
  return ed.cursorAt(pos);
}

namespace {
// The arrangement's edit chords that act on the windows (its keymap scope "arrangement", so a rebinding follows). Not
// here: zoom / follow (the arrangement's own view), track commands (global chords reach them anyway).
constexpr std::array<std::string_view, 26> kLaneActions = {
    "arrangement.copy",         "arrangement.cut",           "arrangement.paste",        "arrangement.duplicate",
    "arrangement.delete",       "arrangement.split",         "arrangement.consolidate",  "arrangement.loopSelection",
    "arrangement.insertMidiClip", "arrangement.insertTime",  "arrangement.duplicateTime", "arrangement.deleteTime",
    "arrangement.toggleActive", "arrangement.selectAll",     "arrangement.markerLeft",   "arrangement.markerRight",
    "arrangement.nudgeFineLeft", "arrangement.nudgeFineRight", "arrangement.extendLeft", "arrangement.extendRight",
    "arrangement.deselect",     "arrangement.trackUp",       "arrangement.trackDown",    "arrangement.pointsUpFine",
    "arrangement.pointsDownFine", "arrangement.toggleAutomationMode"};
}  // namespace

bool ProjectPatchView::laneKey(const ui::KeyEvent& e) {
  if (!e.down) return false;
  LaneEditSurface* s = laneEditReady();
  if (s == nullptr) return false;
  const std::string_view id = ctx_.keymap().lookup(ctx_.scope("arrangement"), ui::chordOf(e));
  if (id.empty() || std::find(kLaneActions.begin(), kLaneActions.end(), id) == kLaneActions.end()) return false;
  const ui::ActionIndex a = ctx_.actions().find(id);
  if (a == ui::kNoAction) return false;
  if (e.repeat && !ctx_.actions().options(a).repeatable) return true;  // a held chord fires once
  arrangement::Surfaces::Bind bind(laneArr_->surfaces(), s);
  return ctx_.actions().invoke(a);  // disabled (nothing to act on): the canvas's own keys get it
}

void ProjectPatchView::laneAbort() {
  ArrangementView* arr = laneArrangement();
  if (arr == nullptr) return;
  arrangement::Surfaces::Bind bind(arr->surfaces(), laneEdit_.get());
  if (arr->editor().dragSurface() == laneEdit_.get()) arr->editor().abortDrag();
  if (arr->automation().dragSurface() == laneEdit_.get()) arr->automation().abortDrag();
}

}  // namespace pq::views::project_patch
