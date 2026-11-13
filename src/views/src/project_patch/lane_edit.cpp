// TX.lane-edit (bug report 2026-11-01_13-50-52): the project patch view's lanes (TL.1: the band lanes) are an editing
// surface of their tracks. No second implementation: the arrangement's Editor (selection, time selection, clip move / copy / trim, audio
// handles, the edit commands and their menu) and ArrangementAutomation (breakpoints, curves, the envelope menu) run on
// the windows through arrangement::EditSurface — one selection, one clipboard, one undo history for both views. This
// file is the surface (rows = the lanes with a clip row, one time axis: the shared window over the lanes' x range) and the
// routing of the canvas's pointer and keys.
//
// CPU: nothing runs unless the pointer or a key is in a lane; the rows are rebuilt once per document version or model
// rebuild (a vector of the lanes), the geometry is computed per query from the model's bands (a handful of multiplies).

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#include "../arrangement/internal.h"
#include "../arrangement/lane_paint.h"
#include "internal.h"
#include "pq/document/racks.h"
#include "pq/ui/lines.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/circuit/cables.h"

namespace pq::views::device_chain {
// The macro mapping onto device.param (src/device_chain/rack_frame.cpp; its header pulls in the Device view's internals).
std::optional<std::pair<doc::RackInfo, doc::RackMapping>> macroMappingOf(const doc::Project& p, doc::DeviceId device, std::string_view param);
}  // namespace pq::views::device_chain

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
    if (!hasClipRow(pp.bands[b]) || pp.bands[b].laneH <= 0.0f) continue;
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
  return laneBodyDevice(view_.canvas(), pp, pp.bands[b]);
}

// Lanes sit in band order but bands may be nudged: a linear scan (a handful of rows, a few multiplies each).
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

// Every lane spans the same x range (TL.1): one axis for the clip rows and (TL.2) the sub-lanes of bands without one.
arrangement::TimeAxis LaneEditSurface::axis(int32_t) const {
  const ProjectPatch& pp = view_.patch();
  if (!(pp.laneX1 > pp.laneX0)) return {};
  const Rect b = snapRect(view_.canvas().deviceRect(pp.laneX0, 0.0f, pp.laneX1 - pp.laneX0, 0.0f));
  const double span = std::max(1.0, view_.laneSpan());
  const double k = std::max(1e-9, static_cast<double>(b.x1 - b.x0) / span);
  return {static_cast<double>(b.x0), view_.laneStart(), k};
}

// TL.2: the sub-lanes as the surface's lanes. Cached: a pointer move asks for them several times (bandAt, hover, damage);
// the key is what their device rects depend on (a handful of numbers per band).
void LaneEditSurface::syncLanes() const {
  const ProjectCanvas& c = view_.canvas();
  const ProjectPatch& pp = view_.patch();
  const auto subs = view_.subLanes();
  thread_local std::vector<double> key;
  key.clear();
  const circuit::CircuitCanvas::View cv = c.view();
  const ui::IRect cr = c.rect();
  key.insert(key.end(), {cv.x, cv.y, cv.zoom, static_cast<double>(cr.x), static_cast<double>(cr.y), static_cast<double>(c.deviceScale()),
                         static_cast<double>(view_.stats().rebuilds), static_cast<double>(subs.size()),
                         static_cast<double>(view_.subLaneGeneration())});
  if (!subs.empty())
    for (uint32_t b = 0; b < pp.bands.size(); ++b)
      key.push_back(c.bandLifted(b) ? 1e300 : static_cast<double>(c.bandNow(b).y1 - (pp.bands[b].y + pp.bands[b].h)));
  // The lanes' document entries (an Automation-only edit makes a new lane without a re-layout).
  const doc::Snapshot& snap = view_.context().doc();
  if (key == laneKey_ && snap == laneDoc_) return;
  laneKey_ = key;
  laneDoc_ = snap;
  lanes_.clear();
  laneX_ = laneW_ = 0;
  if (subs.empty() || !(pp.laneX1 > pp.laneX0)) return;
  const ui::IRect xr = toIRect(snapRect(c.deviceRect(pp.laneX0, 0.0f, pp.laneX1 - pp.laneX0, 0.0f)));
  laneX_ = xr.x;
  laneW_ = xr.w;
  sync();
  for (const ProjectPatchView::SubLane& sl : subs) {
    if (sl.band >= pp.bands.size() || c.bandLifted(sl.band)) continue;
    const Band& band = pp.bands[sl.band];
    const float dy = c.bandNow(sl.band).y1 - (band.y + band.h);
    const Rect r = snapRect(c.deviceRect(pp.laneX0, sl.y + dy, pp.laneX1 - pp.laneX0, sl.h));
    AutomationLaneRow lr;
    lr.track = band.track;
    lr.row = sl.band < rowOfBand_.size() ? rowOfBand_[sl.band] : -1;
    lr.index = sl.index;
    lr.param = sl.param;
    const LaneLookup l = findAutomationLane(*view_.context().doc(), sl.param);
    lr.owner = l.owner;
    lr.lane = l.lane;
    lr.y = static_cast<int32_t>(r.y0);
    lr.h = std::max(1, static_cast<int32_t>(r.y1 - r.y0));
    lanes_.push_back(lr);
  }
  std::stable_sort(lanes_.begin(), lanes_.end(), [](const AutomationLaneRow& a, const AutomationLaneRow& b) { return a.y < b.y; });
}

std::span<const AutomationLaneRow> LaneEditSurface::laneRows() const {
  syncLanes();
  return lanes_;
}

ui::IRect LaneEditSurface::laneRect(size_t i) const {
  syncLanes();
  if (i >= lanes_.size()) return {};
  return {laneX_, lanes_[i].y, laneW_, lanes_[i].h};
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

// The lanes page so the tick shows (the insert marker moved by the arrow keys, Home / End): as the arrangement, the tick a
// quarter into the window (the shared window: the arrangement follows).
void LaneEditSurface::revealTick(Ticks t) {
  const double start = view_.laneStart(), span = view_.laneSpan(), at = static_cast<double>(t);
  if (!(span > 0.0) || (at >= start && at < start + span)) return;
  view_.setLaneTime(at - 0.25 * span, span);
}

// The arrangement's rows changed (ArrangementView::rowsChanged: lanes shown / hidden / resized, a parameter chosen,
// automation mode) or its editing state wants a full repaint. TL.2: the sub-lanes follow the arrangement's shown lanes.
void LaneEditSurface::damageAll() {
  view_.subLanesChanged();
  view_.damageLanes();
}

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

// ---- drops (TX.lane-drop) -----------------------------------------------------------------------------------------
// The arrangement's FileDrop runs them (one drop path: rules, probing, ghosts, the commit); this surface only maps the
// pointer to a band's track and says where new tracks show. CPU: a scan of the bands per drag move (a few multiplies each).

std::optional<arrangement::EditSurface::DropSpot> LaneEditSurface::dropSpotAt(Vec2 pos) const {
  const ProjectPatch& pp = view_.patch();
  const ProjectCanvas& c = view_.canvas();
  if (pp.scoped || view_.embedded() || !(view_.laneSpan() > 0.0) || !c.rect().contains(pos)) return std::nullopt;
  for (uint32_t b = 0; b < pp.bands.size(); ++b) {
    const Band& band = pp.bands[b];
    if (band.laneH <= 0.0f || c.bandLifted(b) || view_.bandTrack(b) == nullptr) continue;
    const Rect a = laneAreaDevice(c, pp, band);
    if (pos.x >= a.x0 && pos.x < a.x1 && pos.y >= a.y0 && pos.y < a.y1) return DropSpot{band.track, rowOfBand(b), static_cast<int32_t>(b)};
  }
  return std::nullopt;
}

arrangement::EditSurface::DropInsert LaneEditSurface::dropInsert(const DropSpot& s) const {
  const ProjectPatch& pp = view_.patch();
  if (s.spot < 0 || static_cast<size_t>(s.spot) >= pp.bands.size()) return {};
  const ProjectCanvas& c = view_.canvas();
  const Band& band = pp.bands[static_cast<size_t>(s.spot)];
  const Rect body = laneBodyDevice(c, pp, band);
  const Rect area = laneAreaDevice(c, pp, band);
  const float bottom = std::max(area.y1, c.deviceRect(0.0f, band.y + band.h, 0.0f, 0.0f).y0);
  return {static_cast<int32_t>(std::lround(bottom)), std::max(1, static_cast<int32_t>(std::lround(body.y1 - body.y0)))};
}

ui::DropFeedback ProjectCanvas::dragOver(const ui::DragPayload& p, Vec2 pos, ui::Mods mods) {
  LaneEditSurface* s = view_.laneEditReady();
  if (s == nullptr) return {};
  return view_.laneArr_->fileDrop().over(*s, p, pos, mods);
}

void ProjectCanvas::dragLeave() {
  if (ArrangementView* arr = view_.laneArrangement()) arr->fileDrop().leave(*view_.laneEdit_);
}

ui::DropEffect ProjectCanvas::drop(const ui::DragPayload& p, Vec2 pos, ui::Mods mods) {
  LaneEditSurface* s = view_.laneEditReady();
  if (s == nullptr) return ui::DropEffect::None;
  return view_.laneArr_->fileDrop().drop(*s, p, pos, mods);
}

// The ghosts over everything on the canvas (new tracks show under a band, over its neighbours' blocks), on the lanes'
// one time axis, as the arrangement draws them (FileDrop::paint).
void ProjectPatchView::paintLaneDrop(ui::PaintContext& ctx) {
  ArrangementView* arr = laneArrangement();
  if (arr == nullptr || ctx.res.theme == nullptr) return;
  arrangement::FileDrop& fd = arr->fileDrop();
  if (!fd.hovering() || fd.plan().items.empty() || fd.surface() != laneEdit_.get()) return;
  const arrangement::TimeAxis ax = laneEdit_->axis(0);
  if (!(ax.k > 0.0)) return;
  const ui::IRect cr = canvas_->rect();
  const Rect lx = snapRect(canvas_->deviceRect(patch_.laneX0, 0.0f, patch_.laneX1 - patch_.laneX0, 0.0f));
  const ui::IRect r = ui::intersect({static_cast<int32_t>(ax.x0), cr.y, static_cast<int32_t>(lx.x1 - lx.x0), cr.h}, cr);
  const ui::IRect c = ui::intersect(r, ctx.clip);
  if (c.empty()) return;
  const arrangement::EditSurface::DropSpot& spot = fd.spot();
  const float us = ctx.scale.factor;
  float h = 0.0f;
  if (spot.row >= 0) {
    const Rect b = laneEdit_->body(spot.row);
    h = b.y1 - b.y0;
  } else {
    h = static_cast<float>(laneEdit_->dropInsert(spot).h);
  }
  const float titleH = laneClipTitleH(h, us);
  arrangement::LanePaint lp{ctx, *ctx.res.theme, arr->model(), *arr, *ctx_.doc(), r, c, ax.k, ax.t0,
                            static_cast<Ticks>(std::floor(ax.t0 + (static_cast<double>(c.x - r.x) - 1.0) / ax.k)),
                            static_cast<Ticks>(std::ceil(ax.t0 + (static_cast<double>(c.right() - r.x) + 1.0) / ax.k)),
                            static_cast<int32_t>(titleH)};
  lp.arrangementLanes = false;
  if (ctx.res.uiFonts != nullptr && ctx.res.text != nullptr) {
    const ui::FontId font = ctx.res.uiFonts->small;
    const auto px = static_cast<float>(ctx.res.text->metrics(font).pixelSize);
    if (titleH >= px + 1.0f) {
      lp.font = font;
      const auto cap = static_cast<int32_t>(std::lround(0.727 * static_cast<double>(px)));
      lp.baselineOff = (lp.titleH + cap + 1) / 2;
    }
  }
  fd.paint(lp, *laneEdit_);
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
  if (!(laneSpan() > 0.0) || embedded()) return kNone;
  for (uint32_t i = 0; i < patch_.bands.size(); ++i) {
    const Band& b = patch_.bands[i];
    if (!hasClipRow(b) || b.laneH <= 0.0f) continue;
    const Rect r = laneBodyDevice(*canvas_, patch_, b);
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
// The arrangement's edit chords that act on the lanes (its keymap scope "arrangement", so a rebinding follows). TL.1: its
// zoom chords too (Z, X, + / −: the shared time window). Not here: follow, track commands (global chords reach them).
constexpr std::array<std::string_view, 30> kLaneActions = {
    "arrangement.zoomToSelection", "arrangement.zoomBack", "arrangement.zoomIn", "arrangement.zoomOut",
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


// ---- automation sub-lanes (TL.2) ----------------------------------------------------------------------------------------

void ProjectPatchView::collectSubLanes(std::vector<SubLaneSpec>& out) const {
  out.clear();
  if (embedded()) return;
  ArrangementView* arr = arrangement();
  const doc::Snapshot& snap = ctx_.doc();
  if (arr == nullptr || !snap) return;
  const ArrangementAutomation& au = arr->automation();
  thread_local std::vector<doc::ParamRef> params;  // (UI thread; kept: no allocation once warm)
  doc::forEachTrack(*snap, [&](const doc::Track& t, const doc::Track*) {
    au.lanesFor(*snap, t, params);
    for (const doc::ParamRef& r : params) out.push_back({t.id, r, std::round(au.laneHeight(t.id, r))});
  });
}

// The arrangement's shown lanes or their heights changed (a lane shown, hidden, resized, a parameter chosen for one): the
// bands re-lay out (model + scene; the lowering kept) when what the model holds differs. O(tracks + shown lanes).
// TL.5: when only parameters changed (the same tracks and heights in the same order: a follow lane re-pointed, a lane's
// chooser), the sub-lanes are re-pointed in place: no re-layout, the lanes and the wire repaint.
void ProjectPatchView::subLanesChanged() {
  if (embedded()) return;
  thread_local std::vector<SubLaneSpec> now;
  collectSubLanes(now);
  if (now == subSpecs_) return;
  bool sameShape = now.size() == subSpecs_.size();
  for (size_t i = 0; sameShape && i < now.size(); ++i) sameShape = now[i].track == subSpecs_[i].track && now[i].h == subSpecs_[i].h;
  if (!sameShape) {
    relayout();
    return;
  }
  subSpecs_ = now;
  for (SubLane& sl : subLanes_) sl.param = subSpecs_[sl.spec].param;
  ++subLaneGen_;
  damageLanes();
  canvas_->markDirty();  // the labels and the automation wire
}

std::optional<ui::Rect> ProjectPatchView::subLanePinDevice(size_t i) const {
  if (i >= subLanes_.size() || embedded() || canvas_->lod() != circuit::CircuitCanvas::Lod::Near) return std::nullopt;
  const SubLane& sl = subLanes_[i];
  if (sl.band >= patch_.bands.size() || canvas_->bandLifted(sl.band)) return std::nullopt;
  const Band& band = patch_.bands[sl.band];
  const float us = tree() != nullptr ? tree()->scale() : 1.0f;
  const float dy = canvas_->bandNow(sl.band).y1 - (band.y + band.h);
  const Rect r = snapRect(canvas_->deviceRect(patch_.laneX0, sl.y + dy, patch_.laneX1 - patch_.laneX0, sl.h));
  const float sz = std::round(metrics::kSubLanePin * us);
  if (r.y1 - r.y0 < sz + 4.0f * us || r.x1 - r.x0 < sz + 8.0f * us) return std::nullopt;
  const float x0 = std::round(r.x0 + 2.0f * us), y0 = std::round(r.y0 + 2.0f * us);
  return Rect{x0, y0, x0 + sz, y0 + sz};
}

bool ProjectPatchView::toggleSubLanePin(size_t i) {
  ArrangementView* arr = embedded() ? nullptr : arrangement();
  if (arr == nullptr || i >= subLanes_.size() || subLanes_[i].band >= patch_.bands.size()) return false;
  const doc::TrackId t = patch_.bands[subLanes_[i].band].track;
  const doc::ParamRef p = subLanes_[i].param;
  (void)laneEditReady();
  ArrangementAutomation& au = arr->automation();
  au.setPinned(t, p, !au.pinned(t, p));
  subLanesChanged();
  return true;
}

ui::Rect ProjectPatchView::subLaneDevice(size_t i) const {
  if (i >= subLanes_.size()) return {};
  const SubLane& sl = subLanes_[i];
  return snapRect(canvas_->deviceRect(patch_.laneX0, sl.y, patch_.laneX1 - patch_.laneX0, sl.h));
}

uint32_t ProjectPatchView::subLaneAt(Vec2 device) const {
  if (embedded()) return kNone;
  for (size_t i = 0; i < subLanes_.size(); ++i) {
    const Rect r = subLaneDevice(i);
    if (device.x >= r.x0 && device.x < r.x1 && device.y >= r.y0 && device.y < r.y1) return static_cast<uint32_t>(i);
  }
  return kNone;
}

std::optional<ui::Rect> ProjectPatchView::subLaneRect(doc::TrackId track, const doc::ParamRef& param) const {
  const uint32_t b = patch_.bandOf(track);
  if (b == kNone) return std::nullopt;
  for (size_t i = 0; i < subLanes_.size(); ++i)
    if (subLanes_[i].band == b && subLanes_[i].param == param) return subLaneDevice(i);
  return std::nullopt;
}

bool ProjectPatchView::subLanesShown(doc::TrackId track) const {
  ArrangementView* arr = embedded() ? nullptr : arrangement();
  return arr != nullptr && arr->automation().lanesShown(track);
}

void ProjectPatchView::showSubLanes(doc::TrackId track, bool on) {
  ArrangementView* arr = embedded() ? nullptr : arrangement();
  if (arr == nullptr) return;
  (void)laneEditReady();  // registered: the arrangement's rowsChanged reaches the surface (damageAll → subLanesChanged)
  arr->automation().showLanes(track, on);
  subLanesChanged();  // (also when the surface is not active yet)
}

void ProjectPatchView::setSubLaneHeight(doc::TrackId track, const doc::ParamRef& p, float h) {
  ArrangementView* arr = embedded() ? nullptr : arrangement();
  if (arr == nullptr) return;
  arr->automation().setLaneHeight(track, p, h);
  subLanesChanged();
}

bool ProjectPatchView::showParamAutomation(doc::TrackId track, const doc::ParamRef& p, const doc::Binding* control) {
  if (embedded() || arrangement() == nullptr || !paramValid(*ctx_.doc(), p)) return false;
  (void)laneEditReady();
  ctx_.showAutomation(track, p);  // the arrangement shows it (ArrangementAutomation::showParam): so does the band
  subLanesChanged();
  autoWire_ = AutoWireSource{track, p, control != nullptr ? *control : doc::Binding{}};  // TL.3: the wire from it
  canvas_->markDirty();
  return true;
}

bool ProjectPatchView::showControlAutomation(const doc::Binding& b) {
  // A device's param (a value block's value, a face's or panel's control). A native engine's inner instance (`meta`) is
  // no document param; a macro-mapped param plays its macro: no lane of its own unless it has one (the Device view's
  // "Show Automation" rule).
  if (b.kind != doc::BindingKind::Control || !b.meta.empty() || !b.device.valid() || b.name.empty()) return false;
  const doc::Snapshot& snap = ctx_.doc();
  const doc::DeviceLoc l = snap ? doc::findDevice(*snap, b.device) : doc::DeviceLoc{};
  if (l.track == nullptr || l.device == nullptr) return false;
  const doc::ParamRef r = doc::ParamRef::device(b.device, b.name);
  if (device_chain::macroMappingOf(*snap, b.device, b.name) && findAutomationLane(*snap, r).lane == nullptr) return false;
  return showParamAutomation(l.track->id, r, &b);
}

std::optional<std::pair<Vec2, float>> ProjectPatchView::controlDevice(const doc::Binding& b) const {
  float r = 0.0f;
  const std::optional<graph::Point2> c = controlWorld(b, &r);
  if (!c) return std::nullopt;
  return std::make_pair(canvas_->toDevice(*c), r * canvas_->deviceScale());
}

// ---- the automation wire (TL.3) ---------------------------------------------------------------------------------------

void ProjectPatchView::clearAutomationWire() {
  if (!autoWire_) return;
  autoWire_.reset();
  canvas_->markDirty();
}

void ProjectPatchView::automationWirePress(uint32_t node, const doc::Binding& ctl) {
  if (!autoWire_) return;
  const AutoWireSource& src = *autoWire_;
  if (src.control.bound() && ctl == src.control) return;
  uint32_t own = kNone;
  if (src.control.bound()) {
    own = patch_.nodeOf(src.control.device);
  } else if (const uint32_t b = patch_.bandOf(src.track); b != kNone) {
    own = patch_.bands[b].mixer;
  }
  if (node != kNone && node == own && !ctl.bound()) return;
  clearAutomationWire();
}

std::optional<ProjectPatchView::AutomationWire> ProjectPatchView::automationWire() const {
  if (!autoWire_ || embedded() || !canvas_->scene()) return std::nullopt;
  const AutoWireSource& src = *autoWire_;
  const doc::Snapshot& snap = ctx_.doc();
  if (!snap) return std::nullopt;
  // The control's track (a device's: where it is now) and the model node it is drawn on.
  doc::TrackId track = src.track;
  uint32_t mn = kNone;
  if (src.control.bound()) {
    const doc::DeviceLoc l = doc::findDevice(*snap, src.control.device);
    if (l.track == nullptr || l.device == nullptr) return std::nullopt;  // deleted
    track = l.track->id;
    mn = patch_.nodeOf(src.control.device);  // (inside a closed group: the group's block)
  } else if (const uint32_t b = patch_.bandOf(track); b != kNone) {
    mn = patch_.bands[b].mixer;
  }
  if (mn == kNone || mn >= patch_.nodes.size()) return std::nullopt;
  // Its sub-lane, as drawn now (shifted with its band's strip, lifted with a band dragged by its title).
  const uint32_t band = patch_.bandOf(track);
  if (band == kNone) return std::nullopt;
  size_t si = subLanes_.size();
  for (size_t i = 0; i < subLanes_.size() && si == subLanes_.size(); ++i)
    if (subLanes_[i].band == band && subLanes_[i].param == src.param) si = i;
  if (si == subLanes_.size()) return std::nullopt;  // hidden
  const ProjectCanvas& c = *canvas_;
  const float s = c.deviceScale();
  const Band& B = patch_.bands[band];
  Vec2 shift{0.0f, (c.bandNow(band).y1 - (B.y + B.h)) * s};
  if (c.bandLifted(band)) {
    const graph::Point2 o = c.nodeDragOffset();
    shift = {shift.x + o.x * s, shift.y + o.y * s};
  }
  Rect R = subLaneDevice(si);
  R = {R.x0 + shift.x, R.y0 + shift.y, R.x1 + shift.x, R.y1 + shift.y};
  const ui::IRect cr = c.rect();
  const float vx0 = std::max(R.x0, static_cast<float>(cr.x)), vx1 = std::min(R.x1, static_cast<float>(cr.right()));
  if (!(vx1 > vx0) || !(R.y1 > R.y0)) return std::nullopt;
  // The control as drawn now: its rim (a face's or panel's control, the mixer's knob / cap / handle), else its block's
  // edge (a closed group without a panel element for it); a folded band's: its title strip.
  const Node& n = patch_.nodes[mn];
  const uint32_t sn = mn < scene_.sceneNode.size() ? scene_.sceneNode[mn] : kNone;
  const graph::Point2 no = sn != kNone ? c.nodeOffset(sn) : graph::Point2{0.0f, 0.0f};
  AutomationWire w;
  bool box = false;
  if (n.folded && n.band < patch_.bands.size()) {
    const ProjectCanvas::WorldRect bn = c.bandNow(n.band);
    w.centre = c.toDevice({std::clamp(n.x + 0.5f * n.w, bn.x0, bn.x1), bn.y0 + metrics::kTitle});
  } else {
    std::optional<graph::Point2> at;
    float r = 0.0f;
    if (src.control.bound()) {
      at = controlWorld(src.control, &r);
    } else if (const Face* f = sn != kNone ? c.panelOn(sn) : nullptr) {
      if (const std::optional<ControlAnchor> a = mixerControlAnchor(*f, src.param)) {
        at = graph::Point2{n.x + a->at.x * n.scale, n.y + a->at.y * n.scale};
        r = a->radius * n.scale;
      }
    }
    if (!at) return std::nullopt;
    box = !(r > 0.0f) && std::fabs(at->x - (n.x + 0.5f * n.w)) < 1e-3f && std::fabs(at->y - (n.y + 0.5f * n.h)) < 1e-3f;
    w.centre = c.toDevice({at->x + no.x, at->y + no.y});
    w.radius = r * s;
  }
  // TL.4 (user 2026-11-09: "connect to the automation line itself"): it lands ON the envelope drawn in the sub-lane (the
  // flat current value where the parameter has no points): the point of that line in the lane's visible x range nearest
  // to an aim point beside the control — offset along x by ~0.4 of the drop, toward the side with room — so the lead
  // always runs at a slant and hangs (never a straight drop).
  w.lane = R;
  const float drop = std::fabs(std::clamp(w.centre.y, R.y0, R.y1) - w.centre.y);
  const float us = tree() != nullptr ? tree()->scale() : 1.0f;
  const float off = std::clamp(0.4f * drop, 14.0f * us, 160.0f * us);
  const float ax = w.centre.x + off <= vx1 ? w.centre.x + off : w.centre.x - off;
  const Vec2 aim{std::clamp(ax, vx0, vx1), w.centre.y};
  constexpr size_t kSamples = 48;
  Vec2 env[kSamples];
  size_t li = SIZE_MAX;
  if (const LaneEditSurface* surf = laneEditReady(); surf != nullptr && !c.bandLifted(band)) {
    const auto rows = surf->laneRows();
    for (size_t i = 0; i < rows.size() && li == SIZE_MAX; ++i)
      if (rows[i].track == track && rows[i].param == src.param) li = i;
    // Coarse over the visible range, then fine round the best sample: ~2 × 48 envelope reads per paint, no allocation.
    if (li != SIZE_MAX && laneArrangement()->automation().envelopePoints(*surf, li, vx0, vx1, env)) {
      auto nearest = [&](float& bestD) {
        size_t k = 0;
        bestD = 1e30f;
        for (size_t i = 0; i < kSamples; ++i) {
          const float ex = env[i].x - aim.x, ey = env[i].y - aim.y, d2 = ex * ex + ey * ey;
          if (d2 < bestD) bestD = d2, k = i;
        }
        return k;
      };
      float bd = 0.0f;
      size_t k = nearest(bd);
      const float step = (vx1 - vx0) / static_cast<float>(kSamples - 1);
      const float f0 = std::max(vx0, env[k].x - step), f1 = std::min(vx1, env[k].x + step);
      Vec2 best = env[k];
      if (f1 > f0 && laneArrangement()->automation().envelopePoints(*surf, li, f0, f1, env)) {
        k = nearest(bd);
        best = env[k];
        // Exact on the sampled line: the aim projected onto the segments either side of the best sample.
        for (size_t j = k > 0 ? k - 1 : 0; j < k + 1 && j + 1 < kSamples; ++j) {
          const Vec2 a = env[j], d{env[j + 1].x - a.x, env[j + 1].y - a.y};
          const float l2 = d.x * d.x + d.y * d.y;
          if (!(l2 > 0.0f)) continue;
          const float t = std::clamp(((aim.x - a.x) * d.x + (aim.y - a.y) * d.y) / l2, 0.0f, 1.0f);
          const Vec2 q{a.x + d.x * t, a.y + d.y * t};
          const float d2 = (q.x - aim.x) * (q.x - aim.x) + (q.y - aim.y) * (q.y - aim.y);
          if (d2 < bd) bd = d2, best = q;
        }
      }
      w.to = {best.x, std::clamp(best.y, R.y0, R.y1)};
    } else {
      li = SIZE_MAX;
    }
  }
  if (li == SIZE_MAX) w.to = {aim.x, (R.y0 + R.y1) * 0.5f};  // no envelope to read (a band dragged by its title): its middle
  w.normal = w.centre.y <= w.to.y ? Vec2{0.0f, 1.0f} : Vec2{0.0f, -1.0f};
  const float dx = w.to.x - w.centre.x, dy = w.to.y - w.centre.y, d = std::sqrt(dx * dx + dy * dy);
  w.from = w.centre;
  if (d > 1e-4f && w.radius > 0.0f) {
    const float k = std::min(1.0f, w.radius / d);
    w.from = {w.centre.x + dx * k, w.centre.y + dy * k};
  } else if (d > 1e-4f && box) {  // where the ray to the lane leaves the block
    const Rect nr = c.deviceRect(n.x + no.x, n.y + no.y, n.w, n.h);
    const float tx = dx > 0.0f ? (nr.x1 - w.centre.x) / dx : dx < 0.0f ? (nr.x0 - w.centre.x) / dx : 1.0f;
    const float ty = dy > 0.0f ? (nr.y1 - w.centre.y) / dy : dy < 0.0f ? (nr.y0 - w.centre.y) / dy : 1.0f;
    const float t = std::clamp(std::min(tx, ty), 0.0f, 1.0f);
    w.from = {w.centre.x + dx * t, w.centre.y + dy * t};
  }
  return w;
}

void ProjectPatchView::paintAutomationWire(ui::PaintContext& ctx) {
  ui::UiTree* tr = tree();
  const std::optional<AutomationWire> aw = ctx.res.theme != nullptr ? automationWire() : std::nullopt;
  if (!aw) {
    if (tr != nullptr && autoWireBox_.x1 > autoWireBox_.x0) tr->damage().add(autoWireBox_);
    autoWireBox_ = {};
    return;
  }
  const ui::Theme& th = *ctx.res.theme;
  ui::DrawList& dl = ctx.dl;
  const float us = ctx.scale.factor;
  const AutomationWire& w = *aw;
  // TL.4 (user 2026-11-09: "slightly bendy … a thin cable that is less obtrusive"): a quadratic curve bowed off its chord
  // toward the lower side (a lead hanging from the control: it leaves steeper and swings onto the line), ~0.14 of the
  // chord, sampled at ~6 px; fixed-size buffer, nothing allocated per frame.
  const float dx = w.to.x - w.from.x, dy = w.to.y - w.from.y, chord = std::sqrt(dx * dx + dy * dy);
  constexpr uint32_t kMaxPts = 64;
  Vec2 pts[kMaxPts];
  uint32_t m = 0;
  if (chord > 1.0f) {
    Vec2 nrm{-dy / chord, dx / chord};
    if (nrm.y < 0.0f || (nrm.y == 0.0f && nrm.x < 0.0f)) nrm = {-nrm.x, -nrm.y};  // the hanging side
    const float bow = 0.14f * chord;
    const Vec2 ctl{(w.from.x + w.to.x) * 0.5f + nrm.x * bow, (w.from.y + w.to.y) * 0.5f + nrm.y * bow};
    m = std::clamp(static_cast<uint32_t>(std::ceil(chord / (6.0f * us))) + 1u, 3u, kMaxPts);
    for (uint32_t i = 0; i < m; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(m - 1), u1 = 1.0f - t;
      pts[i] = {u1 * u1 * w.from.x + 2.0f * u1 * t * ctl.x + t * t * w.to.x, u1 * u1 * w.from.y + 2.0f * u1 * t * ctl.y + t * t * w.to.y};
    }
  }
  // The automation line's red, half transparent: a hairline with a faint dark edge (it reads on a light face), a small
  // dot where it meets the envelope. No glow, no ring.
  const ui::Rgba red = th.automation.line;
  const float lw = std::max(1.0f, 1.1f * us);
  if (m >= 2) {
    ui::smoothPolyline(dl, pts, m, ui::setAlpha(ui::rgba(0, 0, 0), 0.18f), lw + 1.0f * us);
    ui::smoothPolyline(dl, pts, m, ui::setAlpha(red, 0.58f), lw);
  }
  dl.addCircleFilled(w.to, 2.2f * us, ui::setAlpha(red, 0.75f));
  const float tick = 3.0f * us, ringR = 0.0f;
  // Its damage: where it moves without a repaint of the canvas (a mixer fader's cap dragged repaints the face alone).
  float x0 = std::min(w.to.x - tick, w.centre.x - ringR), y0 = std::min(w.to.y - tick, w.centre.y - ringR);
  float x1 = std::max(w.to.x + tick, w.centre.x + ringR), y1 = std::max(w.to.y + tick, w.centre.y + ringR);
  for (uint32_t i = 0; i < m; ++i) {
    x0 = std::min(x0, pts[i].x), y0 = std::min(y0, pts[i].y);
    x1 = std::max(x1, pts[i].x), y1 = std::max(y1, pts[i].y);
  }
  const float pad = 4.0f * lw + 4.0f * us;
  const Rect bx{std::floor(x0 - pad), std::floor(y0 - pad), std::ceil(x1 + pad), std::ceil(y1 + pad)};
  if (bx.x0 != autoWireBox_.x0 || bx.y0 != autoWireBox_.y0 || bx.x1 != autoWireBox_.x1 || bx.y1 != autoWireBox_.y1) {
    if (tr != nullptr) {
      if (autoWireBox_.x1 > autoWireBox_.x0) tr->damage().add(autoWireBox_);
      tr->damage().add(bx);
    }
    autoWireBox_ = bx;
  }
}

// ---- the time ruler over the lanes (TL.1) ------------------------------------------------------------------------------

namespace {

// The band lanes as the arrangement Ruler's axis: the lanes' x range on the canvas over the shared time window.
class LaneRulerAxis final : public arrangement::RulerAxis {
public:
  explicit LaneRulerAxis(ProjectPatchView& v) : v_(v) {}
  ViewContext& context() const override { return v_.context(); }
  const doc::Project& project() const override { return *v_.context().doc(); }
  float x0() const override { return lanes().x0; }
  double origin() const override { return v_.laneStart(); }
  double devicePxPerTick() const override {
    const Rect r = lanes();
    const double span = v_.laneSpan();
    return span > 0.0 && r.x1 > r.x0 ? static_cast<double>(r.x1 - r.x0) / span : 0.0;
  }
  double pxPerTick() const override { return devicePxPerTick() / static_cast<double>(uiScale()); }
  ui::IRect range() const override {
    const ProjectPatch& pp = v_.patch();
    if (pp.scoped || v_.embedded() || !(pp.laneX1 > pp.laneX0)) return {};
    const Rect r = lanes();
    const ui::IRect cr = v_.canvas().rect();
    const auto x0 = static_cast<int32_t>(std::floor(r.x0)), x1 = static_cast<int32_t>(std::ceil(r.x1));
    return ui::intersect(cr, {x0, cr.y, x1 - x0, cr.h});
  }
  void setTimeView(double origin, double pxPerTick) override {
    const Rect r = lanes();
    if (!(pxPerTick > 0.0) || !(r.x1 > r.x0)) return;
    v_.setLaneTime(origin, static_cast<double>(r.x1 - r.x0) / (pxPerTick * static_cast<double>(uiScale())));
  }
  void zoomAt(float x, double factor) override {
    if (!(factor > 0.0)) return;
    v_.zoomLanes(std::log(factor) / std::log(ProjectPatchView::kLaneZoomStep), v_.laneAnchor({x, 0.0f}));
  }
  void scrollByPx(double px) override {
    const double k = devicePxPerTick();
    if (k > 0.0) v_.setLaneTime(v_.laneStart() + px / k, v_.laneSpan());
  }
  void locateAt(Ticks tick, bool snap) override {
    ViewContext& ctx = v_.context();
    tick = std::max<Ticks>(0, tick);
    if (snap) tick = std::max<Ticks>(0, Grid(ctx.grid(), ctx.doc()->timeline->sigMap, pxPerTick()).snap(tick));
    ctx.setStartMarker(tick);
    ctx.transport().locate(tick);
  }
  std::optional<Ticks> playhead() const override { return v_.lanePlayhead(); }
  ui::Widget* focusTarget() override { return &v_.canvas(); }

private:
  float uiScale() const { return v_.tree() != nullptr ? v_.tree()->scale() : 1.0f; }
  Rect lanes() const {
    const ProjectPatch& pp = v_.patch();
    return v_.canvas().deviceRect(pp.laneX0, 0.0f, pp.laneX1 - pp.laneX0, 0.0f);
  }
  ProjectPatchView& v_;
};

}  // namespace

ui::Widget& addCompactLaneRuler(ProjectCanvas& canvas, ProjectPatchView& view) {
  auto& r = canvas.add<arrangement::Ruler>(std::make_unique<LaneRulerAxis>(view), "lane-ruler");
  r.setCompact(true);
  r.setVisible(false);  // never laid out or hit by the tree: the canvas paints it into each lane and hands it the presses
  return r;
}

void paintLaneRuler(ui::Widget& ruler, ui::PaintContext& ctx, const ui::IRect& strip) {
  static_cast<arrangement::Ruler&>(ruler).paintStrip(ctx, strip);
}

bool laneRulerPointer(ui::Widget& ruler, const ui::PointerEvent& e, const ui::IRect& strip) {
  return static_cast<arrangement::Ruler&>(ruler).pointerStrip(e, strip);
}

void cancelLaneRuler(ui::Widget& ruler) {
  auto& r = static_cast<arrangement::Ruler&>(ruler);
  r.onCaptureLost();
}

ui::Widget& addLaneRuler(ProjectCanvas& canvas, ProjectPatchView& view) {
  auto& r = canvas.add<arrangement::Ruler>(std::make_unique<LaneRulerAxis>(view));
  r.setInfo("Time Ruler",
            "The lanes' time (the arrangement's: one time window). Click to set the start marker and jump there. Drag up "
            "or down to zoom, left or right to scroll; Ctrl+wheel zooms, Shift+wheel scrolls. Drag the loop brace or a "
            "locator to move it.");
  return r;
}

}  // namespace pq::views::project_patch
