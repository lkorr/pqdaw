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
#include "internal.h"
#include "pq/document/racks.h"
#include "pq/views/arrangement/arrangement.h"

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
                         static_cast<double>(view_.stats().rebuilds), static_cast<double>(subs.size())});
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
void ProjectPatchView::subLanesChanged() {
  if (embedded()) return;
  thread_local std::vector<SubLaneSpec> now;
  collectSubLanes(now);
  if (now == subSpecs_) return;
  relayout();
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

bool ProjectPatchView::showParamAutomation(doc::TrackId track, const doc::ParamRef& p) {
  if (embedded() || arrangement() == nullptr || !paramValid(*ctx_.doc(), p)) return false;
  (void)laneEditReady();
  ctx_.showAutomation(track, p);  // the arrangement shows it (ArrangementAutomation::showParam): so does the band
  subLanesChanged();
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
  return showParamAutomation(l.track->id, r);
}

std::optional<std::pair<Vec2, float>> ProjectPatchView::controlDevice(const doc::Binding& b) const {
  float r = 0.0f;
  const std::optional<graph::Point2> c = controlWorld(b, &r);
  if (!c) return std::nullopt;
  return std::make_pair(canvas_->toDevice(*c), r * canvas_->deviceScale());
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

ui::Widget& addLaneRuler(ProjectCanvas& canvas, ProjectPatchView& view) {
  auto& r = canvas.add<arrangement::Ruler>(std::make_unique<LaneRulerAxis>(view));
  r.setInfo("Time Ruler",
            "The lanes' time (the arrangement's: one time window). Click to set the start marker and jump there. Drag up "
            "or down to zoom, left or right to scroll; Ctrl+wheel zooms, Shift+wheel scrolls. Drag the loop brace or a "
            "locator to move it.");
  return r;
}

}  // namespace pq::views::project_patch
