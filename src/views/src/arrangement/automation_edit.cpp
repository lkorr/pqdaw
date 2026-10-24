// Automation (T3.15): the arrangement's lane state, geometry, breakpoint editing (add / move / delete / select,
// Alt-drag curves, time selection, shapes) and the envelope menus.
//
// Like the clip drags (T3.9), a breakpoint drag never touches the document while it runs: the bands draw the lane's
// points as they will be (Drag::preview) and every frame damages only the columns between the moved points' unmoved
// neighbours, in every band showing that parameter. The edit is one pq::doc::ops builder dispatched on release: one
// undo step, an Automation-only ChangeSet (a lane created by the first edit of a parameter makes it Structural).

#include <algorithm>
#include <cmath>
#include <limits>

#include "internal.h"
#include "pq/document/commands.h"
#include "pq/views/arrangement/automation.h"

namespace pq::views {

using namespace pq::ui;
namespace am = arrangement_metrics;
namespace au = automation_metrics;
using arrangement::ClipIndex;
using arrangement::Model;

namespace {

constexpr float kClickSlop = 3.0f;  // logical px before a press becomes a drag
// The id a point created by a click on the line has until the release dispatches it (the largest id: on a shared
// tick it sorts last, as the fresh id the document gives it will).
constexpr doc::BreakpointId kPendingPoint{std::numeric_limits<uint32_t>::max()};

bool pointLess(const doc::Breakpoint& a, const doc::Breakpoint& b) noexcept {
  return a.tick != b.tick ? a.tick < b.tick : a.id.value < b.id.value;
}

bool containsId(const std::vector<uint32_t>& sorted, uint32_t id) noexcept {
  return std::binary_search(sorted.begin(), sorted.end(), id);
}

}  // namespace

ArrangementAutomation::ArrangementAutomation(ArrangementView& view) : view_(view), ctx_(view.context()) {}
ArrangementAutomation::~ArrangementAutomation() = default;

Grid ArrangementAutomation::grid() const noexcept {
  return Grid(ctx_.grid(), view_.shown()->timeline->sigMap, view_.pxPerTick());
}

// ---- mode and lanes --------------------------------------------------------------------------------------------

void ArrangementAutomation::setMode(bool on) {
  if (on == mode_) return;
  mode_ = on;
  if (drag_.kind != DragKind::None && drag_.band.overClips) cancelDrag();
  if (!on && selOverClips_) clearSelection();
  ctx_.setStatus(on ? "Automation Mode on" : "Automation Mode off");
  view_.rowsChanged();  // headers show / hide their choosers; the rows repaint
}

ArrangementAutomation::TrackState& ArrangementAutomation::state(doc::TrackId t) { return tracks_[t.value]; }

const ArrangementAutomation::TrackState* ArrangementAutomation::findState(doc::TrackId t) const noexcept {
  const auto it = tracks_.find(t.value);
  return it == tracks_.end() ? nullptr : &it->second;
}

bool ArrangementAutomation::lanesShown(doc::TrackId t) const noexcept {
  const TrackState* s = findState(t);
  return s && s->shown;
}

// The first time a track's lanes are shown: its automated parameters whose lanes are visible (LaneView), else the
// chosen one.
void ArrangementAutomation::initLanes(TrackState& s, doc::TrackId t) {
  if (s.initialised) return;
  s.initialised = true;
  const doc::Project& p = *ctx_.doc();
  const doc::Track* tr = doc::findTrackPtr(p, t);
  if (!tr) return;
  const doc::ViewState& vs = ctx_.store().view();
  for (const doc::LanePtr& l : tr->lanes) {
    const auto it = vs.lanes.find(l->id);
    if ((it == vs.lanes.end() || it->second.visible) && paramValid(p, l->param)) s.lanes.push_back(l->param);
  }
}

void ArrangementAutomation::showLanes(doc::TrackId t, bool on) {
  const doc::Track* tr = doc::findTrackPtr(*ctx_.doc(), t);
  if (!tr || tr->kind == doc::TrackKind::Return || tr->kind == doc::TrackKind::Master) return;  // pinned rows: A mode only
  TrackState& s = state(t);
  if (s.shown == on) return;
  s.shown = on;
  if (on) {
    initLanes(s, t);
    std::erase_if(s.lanes, [&](const doc::ParamRef& r) { return !paramValid(*ctx_.doc(), r); });
    if (s.lanes.empty()) s.lanes.push_back(chosen(t));
  }
  view_.rowsChanged();
}

std::vector<doc::ParamRef> ArrangementAutomation::shownLanes(doc::TrackId t) const {
  std::vector<doc::ParamRef> out;
  if (const doc::Track* tr = doc::findTrackPtr(*ctx_.doc(), t)) lanesFor(*ctx_.doc(), *tr, out);
  return out;
}

void ArrangementAutomation::lanesFor(const doc::Project& p, const doc::Track& t, std::vector<doc::ParamRef>& out) const {
  out.clear();
  const TrackState* s = findState(t.id);
  if (!s || !s->shown) return;
  for (const doc::ParamRef& r : s->lanes)
    if (paramValid(p, r) && std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
}

void ArrangementAutomation::setLaneParam(doc::TrackId t, size_t index, const doc::ParamRef& p) {
  TrackState& s = state(t);
  if (index >= s.lanes.size() || s.lanes[index] == p) return;
  const auto it = std::find(s.lanes.begin(), s.lanes.end(), p);
  if (it != s.lanes.end()) std::swap(*it, s.lanes[index]);
  else s.lanes[index] = p;
  if (const LaneLookup l = view_.model().lane(p); l.lane) ctx_.store().view().lanes[l.lane->id].visible = true;
  view_.rowsChanged();
}

bool ArrangementAutomation::addLane(doc::TrackId t) {
  const doc::Project& p = *ctx_.doc();
  const doc::Track* tr = doc::findTrackPtr(p, t);
  if (!tr) return false;
  TrackState& s = state(t);
  initLanes(s, t);
  const std::vector<ParamChoice> choices = paramChoices(p, *tr);
  const ParamChoice* pick = nullptr;
  for (int pass = 0; pass < 2 && !pick; ++pass)  // automated parameters first (Live adds the next automated one)
    for (const ParamChoice& c : choices)
      if ((pass == 1 || c.automated) && std::find(s.lanes.begin(), s.lanes.end(), c.param) == s.lanes.end()) {
        pick = &c;
        break;
      }
  if (!pick) {
    ctx_.setStatus("Every parameter of this track already has a lane");
    return false;
  }
  s.lanes.push_back(pick->param);
  s.shown = true;
  if (const LaneLookup l = view_.model().lane(pick->param); l.lane) ctx_.store().view().lanes[l.lane->id].visible = true;
  view_.rowsChanged();
  return true;
}

void ArrangementAutomation::hideLane(doc::TrackId t, size_t index) {
  TrackState& s = state(t);
  // `index` counts the shown (valid) lanes: map it onto the stored list.
  const std::vector<doc::ParamRef> shown = shownLanes(t);
  if (index >= shown.size()) return;
  const doc::ParamRef r = shown[index];
  if (const LaneLookup l = view_.model().lane(r); l.lane) ctx_.store().view().lanes[l.lane->id].visible = false;
  std::erase(s.lanes, r);
  if (drag_.kind != DragKind::None && !drag_.band.overClips && drag_.band.param == r) cancelDrag();
  if (s.lanes.empty()) s.shown = false;
  view_.rowsChanged();
}

void ArrangementAutomation::hideLaneLater(doc::TrackId t, size_t index) {
  pendingHide_ = {t, index};
  view_.requestFrame();
}

void ArrangementAutomation::runPending() {
  if (!pendingHide_) return;
  const auto [t, i] = *pendingHide_;
  pendingHide_.reset();
  hideLane(t, i);
}

doc::ParamRef ArrangementAutomation::chosen(doc::TrackId t) const {
  const doc::Project& p = *ctx_.doc();
  if (const TrackState* s = findState(t); s && s->chosen && paramValid(p, *s->chosen)) return *s->chosen;
  if (const doc::Track* tr = doc::findTrackPtr(p, t))
    for (const doc::LanePtr& l : tr->lanes)
      if (paramValid(p, l->param)) return l->param;
  return doc::ParamRef::track(doc::ParamKind::TrackVolume, t);
}

void ArrangementAutomation::choose(doc::TrackId t, const doc::ParamRef& p) {
  TrackState& s = state(t);
  if (s.chosen && *s.chosen == p) return;
  if (drag_.kind != DragKind::None && drag_.band.overClips && drag_.band.track == t) cancelDrag();
  s.chosen = p;
  view_.rowsChanged();
}

float ArrangementAutomation::laneHeight(doc::TrackId t, const doc::ParamRef& p) const {
  if (const LaneLookup l = view_.model().lane(p); l.lane) {
    const auto& lanes = ctx_.store().view().lanes;
    if (const auto it = lanes.find(l.lane->id); it != lanes.end() && it->second.height > 0)
      return std::clamp(static_cast<float>(it->second.height), au::kMinLaneHeight, au::kMaxLaneHeight);
  }
  if (const TrackState* s = findState(t))
    for (const auto& [r, h] : s->heights)
      if (r == p) return h;
  return au::kLaneHeight;
}

void ArrangementAutomation::setLaneHeight(doc::TrackId t, const doc::ParamRef& p, float logicalPx) {
  const float h = std::round(std::clamp(logicalPx, au::kMinLaneHeight, au::kMaxLaneHeight));
  if (h == laneHeight(t, p)) return;
  if (const LaneLookup l = view_.model().lane(p); l.lane) {
    ctx_.store().view().lanes[l.lane->id].height = static_cast<int32_t>(h);
  } else {
    TrackState& s = state(t);
    auto it = std::find_if(s.heights.begin(), s.heights.end(), [&](const auto& e) { return e.first == p; });
    if (it == s.heights.end()) s.heights.emplace_back(p, h);
    else it->second = h;
  }
  view_.rowsChanged();
}

// ---- geometry ------------------------------------------------------------------------------------------------

AutomationBand ArrangementAutomation::laneBand(size_t i) const {
  const AutomationLaneRow& lr = view_.laneRows()[i];
  AutomationBand b;
  b.track = lr.track;
  b.param = lr.param;
  b.owner = lr.owner;
  b.lane = lr.lane;
  b.laneIndex = static_cast<int32_t>(i);
  IRect r = view_.laneRect(i);
  r.h = std::max(0, r.h - 1);  // the separator row
  b.rect = r;
  const UiScale s = view_.tree() ? view_.tree()->uiScale() : UiScale{};
  int32_t pad = s.px(au::kLanePad);
  if (r.h - 2 * pad < 4) pad = std::max(0, (r.h - 4) / 2);
  b.top = static_cast<float>(r.y + pad);
  b.bottom = static_cast<float>(r.bottom() - pad - 1);
  return b;
}

std::optional<AutomationBand> ArrangementAutomation::rowBand(int32_t row, bool pinned) const {
  const auto rows = pinned ? view_.bottomRows() : view_.rows();
  if (row < 0 || static_cast<size_t>(row) >= rows.size()) return std::nullopt;
  const ArrangementRow& ar = rows[static_cast<size_t>(row)];
  AutomationBand b;
  b.track = ar.id;
  b.param = chosen(ar.id);
  const LaneLookup l = view_.model().lane(b.param);
  b.owner = l.owner;
  b.lane = l.lane;
  b.overClips = true;
  b.pinned = pinned;
  IRect r = pinned ? view_.pinnedRowRect(static_cast<size_t>(row)) : view_.rowLaneRect(row);
  r.h = std::max(0, r.h - 1);
  b.rect = r;
  const UiScale s = view_.tree() ? view_.tree()->uiScale() : UiScale{};
  // Under the clips' title strips (Live draws the envelope over the clip bodies), when the row is tall enough.
  int32_t top = r.y + s.px(am::kClipTitleHeight) + s.px(2.0f);
  int32_t bottom = r.bottom() - s.px(3.0f);
  if (bottom - top < s.px(8.0f)) {
    top = r.y + std::min(2, r.h / 4);
    bottom = r.bottom() - std::min(2, r.h / 4) - 1;
  }
  b.top = static_cast<float>(top);
  b.bottom = static_cast<float>(std::max(top + 1, bottom));
  return b;
}

std::optional<AutomationBand> ArrangementAutomation::band(doc::TrackId t, const doc::ParamRef& p, bool overClips) const {
  if (overClips) {
    if (!mode_ || chosen(t) != p) return std::nullopt;
    const auto rows = view_.rows();
    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].id == t) return rowBand(static_cast<int32_t>(i), false);
    const auto pinned = view_.bottomRows();
    for (size_t i = 0; i < pinned.size(); ++i)
      if (pinned[i].id == t) return rowBand(static_cast<int32_t>(i), true);
    return std::nullopt;
  }
  const auto lanes = view_.laneRows();
  for (size_t i = 0; i < lanes.size(); ++i)
    if (lanes[i].track == t && lanes[i].param == p) return laneBand(i);
  return std::nullopt;
}

std::optional<AutomationBand> ArrangementAutomation::bandAt(Vec2 p) const {
  const IRect lv = view_.lanesViewport();
  if (lv.contains(p)) {
    const auto lanes = view_.laneRows();
    if (!lanes.empty()) {
      const int32_t top0 = view_.laneRect(0).y - lanes.front().y;  // the track content's top (window y)
      const auto local = static_cast<int32_t>(std::floor(p.y)) - top0;
      const auto it = std::upper_bound(lanes.begin(), lanes.end(), local,
                                       [](int32_t y, const AutomationLaneRow& r) { return y < r.y + r.h; });
      if (it != lanes.end() && local >= it->y && local < it->y + it->h - 1)
        return laneBand(static_cast<size_t>(it - lanes.begin()));
    }
    if (mode_) {
      const int32_t row = view_.rowAt(p.y);
      if (row >= 0) {
        std::optional<AutomationBand> b = rowBand(row, false);
        if (b && b->rect.contains(p)) return b;
      }
    }
    return std::nullopt;
  }
  if (!mode_) return std::nullopt;
  const auto pinned = view_.bottomRows();
  for (size_t i = 0; i < pinned.size(); ++i) {
    std::optional<AutomationBand> b = rowBand(static_cast<int32_t>(i), true);
    if (b && b->rect.contains(p) && intersect(b->rect, view_.bottomRect()).contains(p)) return b;
  }
  return std::nullopt;
}

float ArrangementAutomation::yOfValue(const AutomationBand& b, double v) const {
  const double n = displayNorm(paramDisplay(*view_.shown(), b.param), v);
  return static_cast<float>(static_cast<double>(b.bottom) - n * static_cast<double>(b.bottom - b.top));
}

double ArrangementAutomation::valueAtY(const AutomationBand& b, float y) const {
  const double span = std::max(1.0, static_cast<double>(b.bottom - b.top));
  return displayValue(paramDisplay(*view_.shown(), b.param), (static_cast<double>(b.bottom) - static_cast<double>(y)) / span);
}

std::span<const doc::Breakpoint> ArrangementAutomation::pointsOf(const AutomationBand& b) const noexcept {
  if (drag_.kind != DragKind::None && drag_.previewValid && drag_.band.param == b.param) return drag_.preview;
  return b.lane ? std::span<const doc::Breakpoint>(b.lane->points) : std::span<const doc::Breakpoint>{};
}

std::optional<Vec2> ArrangementAutomation::pointPos(const AutomationBand& b, doc::BreakpointId id) const {
  for (const doc::Breakpoint& pt : pointsOf(b))
    if (pt.id == id) return Vec2{view_.xOfTick(static_cast<double>(pt.tick)), yOfValue(b, pt.value)};
  return std::nullopt;
}

// ---- selection -------------------------------------------------------------------------------------------------

void ArrangementAutomation::select(doc::TrackId t, const doc::ParamRef& p, std::vector<doc::BreakpointId> points,
                                   std::optional<TickRange> range) {
  damageSelection();
  // One selection at a time in the arrangement: the clips / time selection of T3.9 go.
  if (!view_.selection().empty()) view_.clearSelection();
  selTrack_ = t;
  selParam_ = p;
  selPoints_ = std::move(points);
  selIds_.clear();
  for (doc::BreakpointId id : selPoints_) selIds_.push_back(id.value);
  std::sort(selIds_.begin(), selIds_.end());
  selRange_ = range;
  selOverClips_ = drag_.kind != DragKind::None ? drag_.band.overClips : selOverClips_;
  view_.setTimeSelection(range && range->end > range->start ? range : std::nullopt);  // Z zooms to it
  damageSelection();
}

void ArrangementAutomation::clearSelection() {
  if (!hasSelection()) return;
  damageSelection();
  selPoints_.clear();
  selIds_.clear();
  selRange_.reset();
}

// ---- damage ----------------------------------------------------------------------------------------------------

void ArrangementAutomation::damageColumns(const doc::ParamRef& p, int32_t x0, int32_t x1) {
  UiTree* tr = view_.tree();
  if (!tr || x1 <= x0) return;
  const IRect lv = view_.lanesViewport();
  auto add = [&](const AutomationBand& b, const IRect& viewport) {
    const IRect r = intersect(intersect(IRect{x0, b.rect.y, x1 - x0, b.rect.h}, b.rect), viewport);
    if (!r.empty()) tr->damage().add(r.toRect());
  };
  const auto lanes = view_.laneRows();
  for (size_t i = 0; i < lanes.size(); ++i)
    if (lanes[i].param == p) add(laneBand(i), lv);
  if (!mode_) return;
  const auto rows = view_.rows();
  for (size_t i = 0; i < rows.size(); ++i)
    if (chosen(rows[i].id) == p)
      if (const auto b = rowBand(static_cast<int32_t>(i), false)) add(*b, lv);
  const auto pinned = view_.bottomRows();
  for (size_t i = 0; i < pinned.size(); ++i)
    if (chosen(pinned[i].id) == p)
      if (const auto b = rowBand(static_cast<int32_t>(i), true)) add(*b, view_.bottomRect());
}

void ArrangementAutomation::damageSelection() {
  if (!hasSelection()) return;
  const IRect lv = view_.lanesViewport();
  damageColumns(selParam_, lv.x, lv.right());
}

void ArrangementAutomation::damageLanes(std::span<const doc::LaneId> lanes) {
  const IRect lv = view_.lanesViewport();
  for (doc::LaneId id : lanes)
    for (const Model::LaneEntry& e : view_.model().laneIndex)
      if (e.lane && e.lane->id == id) damageColumns(e.param, lv.x, lv.right());
}

void ArrangementAutomation::damageAll() {
  view_.lanes().markDirty();
  if (const IRect b = view_.bottomRect(); !b.empty())
    if (UiTree* tr = view_.tree()) tr->damage().add(b.toRect());
}

// The columns a drag frame changes: from the unmoved neighbour left of the leftmost moved point to the one right of
// the rightmost (the lane's edges when there is none: the envelope holds the end values out to them), plus the
// handles and the value readout.
void ArrangementAutomation::damageDrag() {
  if (drag_.kind == DragKind::None) return;
  const IRect lv = view_.lanesViewport();
  const UiScale s = view_.tree() ? view_.tree()->uiScale() : UiScale{};
  int32_t x0 = lv.x, x1 = lv.right();
  const std::vector<doc::Breakpoint>& o = drag_.orig;
  if (drag_.kind == DragKind::Points && !o.empty()) {
    size_t lo = o.size(), hi = 0;
    for (size_t i = 0; i < o.size(); ++i)
      if (containsId(drag_.moving, o[i].id.value)) {
        lo = std::min(lo, i);
        hi = std::max(hi, i);
      }
    if (lo < o.size()) {
      if (lo > 0) x0 = static_cast<int32_t>(std::floor(view_.xOfTick(static_cast<double>(o[lo - 1].tick))));
      if (hi + 1 < o.size()) x1 = static_cast<int32_t>(std::ceil(view_.xOfTick(static_cast<double>(o[hi + 1].tick))));
    }
    x0 -= s.px(au::kPointGrab) + 2;
    x1 += s.px(au::kPointGrab) + s.px(96.0f);  // the readout sits right of the point
  } else if (drag_.kind == DragKind::Curve) {
    for (size_t i = 0; i + 1 < o.size(); ++i)
      if (o[i].id == drag_.segment) {
        x0 = static_cast<int32_t>(std::floor(view_.xOfTick(static_cast<double>(o[i].tick)))) - 2;
        x1 = static_cast<int32_t>(std::ceil(view_.xOfTick(static_cast<double>(o[i + 1].tick)))) + 2;
      }
  }
  x0 = std::clamp(x0, lv.x, lv.right());
  x1 = std::clamp(x1, lv.x, lv.right());
  const int32_t d0 = drag_.damageX1 > drag_.damageX0 ? std::min(x0, drag_.damageX0) : x0;
  const int32_t d1 = drag_.damageX1 > drag_.damageX0 ? std::max(x1, drag_.damageX1) : x1;
  damageColumns(drag_.band.param, d0, d1);
  drag_.damageX0 = x0;
  drag_.damageX1 = x1;
}

// ---- input -----------------------------------------------------------------------------------------------------

bool ArrangementAutomation::dragging() const noexcept { return drag_.kind != DragKind::None; }

void ArrangementAutomation::cancelDrag() {
  if (drag_.kind == DragKind::None) return;
  const bool created = drag_.created;
  drag_.previewValid = false;
  damageDrag();
  drag_ = {};
  readoutText_.clear();
  if (created) clearSelection();
}

bool ArrangementAutomation::pointer(const PointerEvent& e) {
  switch (e.type) {
    case PointerType::Down: {
      if (e.button != PointerButton::Left && e.button != PointerButton::Right) return false;
      const std::optional<AutomationBand> b = bandAt(e.pos);
      if (!b) return false;
      if (b->overClips && !b->pinned) {
        // Over the clips the title strips stay the clips' (move, Alt-copy, select): T3.9's editor takes them.
        const int32_t row = view_.rowAt(e.pos.y);
        const ArrangementRow& ar = view_.rows()[static_cast<size_t>(row)];
        if (!ar.group && !ar.track->clips.empty()) {
          const UiScale s = view_.tree() ? view_.tree()->uiScale() : UiScale{};
          const IRect lane = view_.rowLaneRect(row);
          if (e.pos.y < static_cast<float>(lane.y + 1 + s.px(am::kClipTitleHeight))) {
            const ClipIndex& ix = view_.model().clips(*ar.track);
            const auto t = static_cast<Ticks>(std::floor(view_.tickAtX(e.pos.x)));
            const auto [a, z] = ix.visible(t, t + 1);
            if (a < z) return false;
          }
        }
      }
      press(e, *b);
      return true;
    }
    case PointerType::Move:
      if (drag_.kind == DragKind::None) return false;
      dragTo(e);
      return true;
    case PointerType::Up:
      if (drag_.kind == DragKind::None) return false;
      if (e.button == PointerButton::Left) release();
      return true;
    default: return false;
  }
}

void ArrangementAutomation::press(const PointerEvent& e, const AutomationBand& b) {
  if (UiTree* t = view_.tree()) t->setFocus(&view_);
  if (drag_.kind != DragKind::None) cancelDrag();
  drag_ = {};
  const UiScale s = view_.tree() ? view_.tree()->uiScale() : UiScale{};
  const std::span<const doc::Breakpoint> pts = b.lane ? std::span<const doc::Breakpoint>(b.lane->points) : std::span<const doc::Breakpoint>{};
  const bool ctrl = (e.mods & mod::kCtrl) != 0, shift = (e.mods & mod::kShift) != 0, alt = (e.mods & mod::kAlt) != 0;
  const bool sameLane = selParam_ == b.param && selTrack_ == b.track;

  // The breakpoint under the pointer (the nearest within the grab radius; later points win ties: drawn on top).
  int32_t hit = -1;
  {
    const float grab = static_cast<float>(s.px(au::kPointGrab));
    float best = grab * grab;
    const auto t0 = static_cast<Ticks>(std::floor(view_.tickAtX(e.pos.x - grab)));
    const auto t1 = static_cast<Ticks>(std::ceil(view_.tickAtX(e.pos.x + grab)));
    auto it = std::lower_bound(pts.begin(), pts.end(), t0, [](const doc::Breakpoint& bp, Ticks t) { return bp.tick < t; });
    for (; it != pts.end() && it->tick <= t1; ++it) {
      const float dx = view_.xOfTick(static_cast<double>(it->tick)) - e.pos.x;
      const float dy = yOfValue(b, it->value) - e.pos.y;
      if (dx * dx + dy * dy <= best) {
        best = dx * dx + dy * dy;
        hit = static_cast<int32_t>(it - pts.begin());
      }
    }
  }

  if (e.button == PointerButton::Right) {
    if (hit >= 0 && !(sameLane && containsId(selIds_, pts[static_cast<size_t>(hit)].id.value)))
      select(b.track, b.param, {pts[static_cast<size_t>(hit)].id});
    (void)openMenu(e.pos, b);
    return;
  }

  drag_.band = b;
  drag_.press = e.pos;
  const double tick = std::max(0.0, view_.tickAtX(e.pos.x));
  const double fallback = currentParamValue(*view_.shown(), b.param);

  // Alt+drag on a segment: its curvature.
  if (alt && pts.size() >= 2) {
    auto it = std::upper_bound(pts.begin(), pts.end(), tick, [](double t, const doc::Breakpoint& bp) { return t < static_cast<double>(bp.tick); });
    if (it != pts.begin() && it != pts.end()) {
      const doc::Breakpoint& a = *(it - 1);
      const doc::Breakpoint& z = *it;
      if (z.tick > a.tick) {
        drag_.kind = DragKind::Curve;
        drag_.orig.assign(pts.begin(), pts.end());
        drag_.segment = a.id;
        drag_.curve0 = drag_.curve = a.curve;
        const ParamDisplay d = paramDisplay(*view_.shown(), b.param);
        // Up bulges the segment upwards: a rising segment then starts fast (curve < 0), a falling one slow (> 0).
        drag_.curveSign = displayNorm(d, z.value) > displayNorm(d, a.value) ? -1.0 : 1.0;
        return;
      }
    }
    drag_ = {};
    return;
  }

  // Points: [start, end) of the moved set's clamps (they cannot pass their unmoved neighbours, nor go below 0).
  auto beginPoints = [&](const doc::Breakpoint& pressed) {
    drag_.kind = DragKind::Points;
    drag_.pressed = pressed;
    drag_.moving = selIds_;
    const std::vector<doc::Breakpoint>& o = drag_.orig;
    Ticks minDt = std::numeric_limits<Ticks>::min() / 4, maxDt = std::numeric_limits<Ticks>::max() / 4;
    for (size_t i = 0; i < o.size(); ++i) {
      if (!containsId(drag_.moving, o[i].id.value)) continue;
      minDt = std::max(minDt, -o[i].tick);
      size_t j = i;
      while (j > 0 && containsId(drag_.moving, o[j - 1].id.value)) --j;
      if (j > 0) minDt = std::max(minDt, o[j - 1].tick - o[i].tick);
      size_t k = i;
      while (k + 1 < o.size() && containsId(drag_.moving, o[k + 1].id.value)) ++k;
      if (k + 1 < o.size()) maxDt = std::min(maxDt, o[k + 1].tick - o[i].tick);
    }
    drag_.minDt = std::min<Ticks>(0, minDt);
    drag_.maxDt = std::max<Ticks>(0, maxDt);
  };

  if (hit >= 0) {
    const doc::Breakpoint& pt = pts[static_cast<size_t>(hit)];
    if (e.clickCount >= 2 && !ctrl && !shift) {  // double-click deletes it
      drag_ = {};
      const doc::BreakpointId id = pt.id;
      const AutomationBand band = b;
      if (editLane(band, "Delete Breakpoint", [&](const doc::Project& p, doc::TrackId owner, doc::LaneId lane) {
            return doc::ops::deleteBreakpoints(p, owner, lane, std::span<const doc::BreakpointId>(&id, 1));
          }))
        clearSelection();
      return;
    }
    const bool was = sameLane && containsId(selIds_, pt.id.value);
    if (ctrl || shift) {
      std::vector<doc::BreakpointId> ids = sameLane ? selPoints_ : std::vector<doc::BreakpointId>{};
      if (was) std::erase(ids, pt.id);
      else ids.push_back(pt.id);
      select(b.track, b.param, std::move(ids), sameLane ? selRange_ : std::nullopt);
      if (was) {
        drag_ = {};
        return;  // toggled off: no drag
      }
    } else if (!was) {
      select(b.track, b.param, {pt.id});
    }
    drag_.orig.assign(pts.begin(), pts.end());
    beginPoints(pt);
    return;
  }

  // On the line: a new breakpoint there (snapped in time, on the envelope's value), dragged on from here.
  const double lineValue = pts.empty() ? fallback : pq::evalAutomation(pts, tick, fallback);
  if (!ctrl && !shift && std::fabs(yOfValue(b, lineValue) - e.pos.y) <= static_cast<float>(s.px(au::kLineGrab))) {
    const Ticks at = std::max<Ticks>(0, grid().snap(static_cast<Ticks>(std::llround(tick))));
    const double v = pts.empty() ? fallback : pq::evalAutomation(pts, static_cast<double>(at), fallback);
    const doc::Breakpoint np{at, static_cast<float>(v), 0.0f, kPendingPoint};
    drag_.orig.assign(pts.begin(), pts.end());
    drag_.orig.insert(std::upper_bound(drag_.orig.begin(), drag_.orig.end(), np, pointLess), np);
    drag_.created = true;
    select(b.track, b.param, {kPendingPoint});
    beginPoints(np);
    updatePreview();
    damageDrag();
    return;
  }

  // Elsewhere: a time selection in this band (it selects the breakpoints inside it).
  const Ticks at = std::max<Ticks>(0, grid().snap(static_cast<Ticks>(std::llround(tick))));
  drag_.kind = DragKind::Range;
  drag_.anchor = at;
  select(b.track, b.param, {}, TickRange{at, at});
  selOverClips_ = b.overClips;
}

void ArrangementAutomation::dragTo(const PointerEvent& e) {
  const float scale = view_.tree() ? view_.tree()->scale() : 1.0f;
  if (!drag_.moved && std::fabs(e.pos.x - drag_.press.x) <= kClickSlop * scale && std::fabs(e.pos.y - drag_.press.y) <= kClickSlop * scale)
    return;
  const bool first = !drag_.moved;
  drag_.moved = true;
  const AutomationBand& b = drag_.band;
  switch (drag_.kind) {
    case DragKind::Points: {
      const auto raw = static_cast<Ticks>(std::llround(view_.tickAtX(e.pos.x) - view_.tickAtX(drag_.press.x)));
      const Grid g = grid();
      const Ticks dt = std::clamp(g.snapping() ? g.snapDelta(drag_.pressed.tick, raw) : raw, drag_.minDt, drag_.maxDt);
      // The value moves in the lane's display scale (Live drags volume in fader positions).
      const ParamDisplay d = paramDisplay(*view_.shown(), b.param);
      const double span = std::max(1.0, static_cast<double>(b.bottom - b.top));
      const double n = displayNorm(d, drag_.pressed.value) + static_cast<double>(drag_.press.y - e.pos.y) / span;
      const double dv = displayValue(d, n) - static_cast<double>(drag_.pressed.value);
      if (!first && dt == drag_.dt && dv == drag_.dv) return;
      drag_.dt = dt;
      drag_.dv = dv;
      break;
    }
    case DragKind::Curve: {
      const double c = static_cast<double>(drag_.curve0) +
                       drag_.curveSign * static_cast<double>(drag_.press.y - e.pos.y) / static_cast<double>(au::kCurveDragPx * scale);
      const auto curve = static_cast<float>(std::clamp(c, -1.0, 1.0));
      if (!first && curve == drag_.curve) return;
      drag_.curve = curve;
      break;
    }
    case DragKind::Range: {
      const Ticks t = std::max<Ticks>(0, grid().snap(static_cast<Ticks>(std::llround(std::max(0.0, view_.tickAtX(e.pos.x))))));
      const TickRange r{std::min(drag_.anchor, t), std::max(drag_.anchor, t)};
      std::vector<doc::BreakpointId> ids;
      if (b.lane)
        for (const doc::Breakpoint& pt : b.lane->points)
          if (pt.tick >= r.start && pt.tick <= r.end && r.end > r.start) ids.push_back(pt.id);
      select(b.track, b.param, std::move(ids), r);
      return;
    }
    case DragKind::None: return;
  }
  updatePreview();
  damageDrag();
}

void ArrangementAutomation::updatePreview() {
  drag_.preview = drag_.orig;
  const doc::ParamRange range = doc::paramRange(drag_.band.param.kind);
  readoutText_.clear();
  if (drag_.kind == DragKind::Points) {
    for (doc::Breakpoint& p : drag_.preview) {
      if (!containsId(drag_.moving, p.id.value)) continue;
      p.tick += drag_.dt;
      p.value = static_cast<float>(std::clamp(static_cast<double>(p.value) + drag_.dv, range.min, range.max));
      if (p.id == drag_.pressed.id) readoutText_ = formatParamValue(drag_.band.param, p.value);
    }
    // The moves are clamped between unmoved neighbours and shift together, so the order holds except for ties on a
    // neighbour's tick (ordered by id): one insertion pass, O(n) on sorted input (no n log n sort per frame).
    auto& pv = drag_.preview;
    for (size_t i = 1; i < pv.size(); ++i)
      for (size_t j = i; j > 0 && pointLess(pv[j], pv[j - 1]); --j) std::swap(pv[j], pv[j - 1]);
  } else if (drag_.kind == DragKind::Curve) {
    for (doc::Breakpoint& p : drag_.preview)
      if (p.id == drag_.segment) p.curve = drag_.curve;
  }
  drag_.previewValid = true;
}

void ArrangementAutomation::release() {
  // The preview goes: repaint its columns (a dispatched edit repaints its lanes too).
  drag_.previewValid = false;
  damageDrag();
  Drag d = std::move(drag_);
  drag_ = {};
  readoutText_.clear();
  const AutomationBand& b = d.band;
  switch (d.kind) {
    case DragKind::Points: {
      if (d.created) {
        const doc::ParamRange range = doc::paramRange(b.param.kind);
        const Ticks at = d.pressed.tick + d.dt;
        const double v = std::clamp(static_cast<double>(d.pressed.value) + d.dv, range.min, range.max);
        doc::ops::Edit made;
        const bool ok = editLane(b, "Add Breakpoint", [&](const doc::Project& p, doc::TrackId owner, doc::LaneId lane) {
          auto e = doc::ops::addBreakpoint(p, ctx_.store().ids(), owner, lane, at, v);
          if (e) made = *e;
          return e;
        });
        if (ok) select(b.track, b.param, made.points);
        else clearSelection();
        return;
      }
      if (!d.moved || (d.dt == 0 && d.dv == 0.0)) return;  // a click: selection only
      std::vector<doc::BreakpointId> ids;
      for (uint32_t raw : d.moving) ids.push_back(doc::BreakpointId{raw});
      (void)editLane(b, ids.size() == 1 ? "Move Breakpoint" : "Move Breakpoints",
                     [&](const doc::Project& p, doc::TrackId owner, doc::LaneId lane) {
                       return doc::ops::moveBreakpoints(p, owner, lane, ids, d.dt, d.dv);
                     });
      return;
    }
    case DragKind::Curve:
      if (d.curve != d.curve0)
        (void)editLane(b, "Curve Segment", [&](const doc::Project& p, doc::TrackId owner, doc::LaneId lane) {
          return doc::ops::setSegmentCurve(p, owner, lane, d.segment, d.curve);
        });
      return;
    case DragKind::Range:
      if (selRange_ && selRange_->end <= selRange_->start && selPoints_.empty()) clearSelection();  // a click
      return;
    case DragKind::None: return;
  }
}

// ---- edits -----------------------------------------------------------------------------------------------------

bool ArrangementAutomation::commit(const doc::ops::EditResult& e, const AutomationBand&, std::string_view what) {
  if (!e) {
    ctx_.setStatus(std::string(what) + ": " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status st = ctx_.store().dispatch(e->command); !st) {
    ctx_.setStatus(std::string(what) + ": " + st.error().message);
    return false;
  }
  return true;
}

template <class Make>
bool ArrangementAutomation::editLane(const AutomationBand& b, std::string_view what, Make&& make) {
  const doc::Snapshot snap = ctx_.doc();
  if (b.lane) return commit(make(*snap, b.owner, b.lane->id), b, what);
  // No lane yet: create it on the band's track, then the edit on that scratch snapshot; one Batch = one undo step.
  auto lane = std::make_shared<doc::AutomationLane>();
  lane->id = ctx_.store().ids().lane();
  lane->param = b.param;
  const doc::Command add = doc::AddAutomationLane{b.track, lane};
  doc::ChangeSet scratch;
  Result<doc::Snapshot> s1 = doc::apply(snap, add, scratch);
  if (!s1) {
    ctx_.setStatus(std::string(what) + ": " + s1.error().message);
    return false;
  }
  const doc::ops::EditResult e = make(**s1, b.track, lane->id);
  if (!e) {
    ctx_.setStatus(std::string(what) + ": " + e.error().message);
    return false;
  }
  doc::Batch batch;
  batch.label = std::string(what);
  batch.commands.push_back(add);
  for (const doc::Command& c : e->batch().commands) batch.commands.push_back(c);
  if (Status st = ctx_.store().dispatch(batch); !st) {
    ctx_.setStatus(std::string(what) + ": " + st.error().message);
    return false;
  }
  return true;
}

bool ArrangementAutomation::deleteSelected() {
  if (selPoints_.empty()) return false;
  const LaneLookup l = view_.model().lane(selParam_);
  if (!l.lane) return false;
  const std::vector<doc::BreakpointId> ids = selPoints_;
  AutomationBand b;
  b.track = selTrack_;
  b.param = selParam_;
  b.owner = l.owner;
  b.lane = l.lane;
  if (!editLane(b, ids.size() == 1 ? "Delete Breakpoint" : "Delete Breakpoints",
                [&](const doc::Project& p, doc::TrackId owner, doc::LaneId lane) { return doc::ops::deleteBreakpoints(p, owner, lane, ids); }))
    return false;
  selPoints_.clear();
  selIds_.clear();
  damageSelection();
  return true;
}

bool ArrangementAutomation::insertShape(doc::ops::ShapeKind kind) {
  if (!selRange_ || selRange_->end <= selRange_->start) {
    ctx_.setStatus("Insert Shape: make a time selection in an automation lane first");
    return false;
  }
  const LaneLookup l = view_.model().lane(selParam_);
  AutomationBand b;
  b.track = selTrack_;
  b.param = selParam_;
  b.owner = l.owner;
  b.lane = l.lane;
  const ParamDisplay d = paramDisplay(*view_.shown(), selParam_);
  doc::ops::ShapeParams sp;
  sp.kind = kind;
  sp.low = displayValue(d, 0.0);
  sp.high = displayValue(d, 1.0);
  const Ticks len = selRange_->end - selRange_->start;
  sp.cycles = static_cast<int32_t>(std::clamp<Ticks>((len + kTicksPerWhole / 2) / kTicksPerWhole, 1, 256));
  const TickRange range = *selRange_;
  doc::ops::Edit made;
  if (!editLane(b, "Insert Shape", [&](const doc::Project& p, doc::TrackId owner, doc::LaneId lane) {
        auto e = doc::ops::insertShape(p, ctx_.store().ids(), owner, lane, range.start, range.end, sp);
        if (e) made = *e;
        return e;
      }))
    return false;
  select(b.track, b.param, made.points, range);
  return true;
}

void ArrangementAutomation::reenable() { ctx_.reenableAutomation(); }

void ArrangementAutomation::afterDocumentChange(const doc::StoreEvent& e) {
  if (e.kind == doc::StoreEventKind::Reset) {
    tracks_.clear();
    drag_ = {};
    readoutText_.clear();
    selPoints_.clear();
    selIds_.clear();
    selRange_.reset();
    return;
  }
  if (drag_.kind == DragKind::Points || drag_.kind == DragKind::Curve) {  // the lane changed under the drag
    drag_ = {};
    readoutText_.clear();
  }
  // Drop selected points that are gone (undo, delete).
  if (!selPoints_.empty()) {
    const LaneLookup l = view_.model().lane(selParam_);
    std::erase_if(selPoints_, [&](doc::BreakpointId id) {
      return !l.lane || std::none_of(l.lane->points.begin(), l.lane->points.end(), [&](const doc::Breakpoint& b) { return b.id == id; });
    });
    selIds_.clear();
    for (doc::BreakpointId id : selPoints_) selIds_.push_back(id.value);
    std::sort(selIds_.begin(), selIds_.end());
  }
}

// ---- menus -----------------------------------------------------------------------------------------------------

namespace {

std::vector<MenuItem> chooserItems(const doc::Project& p, const doc::Track& t, const doc::ParamRef& current,
                                   std::function<void(doc::ParamRef)> pick) {
  std::vector<MenuItem> items;
  std::vector<ParamChoice> choices = paramChoices(p, t);
  std::string group;
  std::vector<MenuItem>* into = &items;
  for (const ParamChoice& c : choices) {
    if (c.group != "Mixer" && c.group != group) {
      group = c.group;
      items.push_back(MenuItem::sub(group, {}));
      into = &items.back().submenu;
    }
    std::string label = c.label;
    if (c.automated) label += c.elsewhere ? " (automated on another track)" : " (automated)";
    MenuItem m = MenuItem::item(std::move(label), [pick, r = c.param] { pick(r); });
    m.checked = c.param == current;
    into->push_back(std::move(m));
  }
  return items;
}

}  // namespace

Menu* ArrangementAutomation::openChooser(Widget& from, doc::TrackId t, int32_t laneIndex) {
  const doc::Project& p = *ctx_.doc();
  const doc::Track* tr = doc::findTrackPtr(p, t);
  if (!tr) return nullptr;
  doc::ParamRef current = chosen(t);
  if (laneIndex >= 0) {
    const auto lanes = view_.laneRows();
    if (static_cast<size_t>(laneIndex) < lanes.size()) current = lanes[static_cast<size_t>(laneIndex)].param;
  }
  const uint32_t index = laneIndex >= 0 ? view_.laneRows()[static_cast<size_t>(laneIndex)].index : 0;
  std::vector<MenuItem> items = chooserItems(p, *tr, current, [this, t, laneIndex, index](doc::ParamRef r) {
    if (laneIndex >= 0) setLaneParam(t, index, r);
    else choose(t, r);
  });
  const IRect r = from.rect();
  return ui::openContextMenu(view_, {static_cast<float>(r.x), static_cast<float>(r.bottom())}, std::move(items));
}

Menu* ArrangementAutomation::openMenu(Vec2 at, const AutomationBand& b) {
  const doc::Project& p = *ctx_.doc();
  const bool range = selRange_ && selRange_->end > selRange_->start && selParam_ == b.param;
  std::vector<MenuItem> shapes;
  const std::pair<const char*, doc::ops::ShapeKind> kinds[] = {
      {"Ramp Up", doc::ops::ShapeKind::RampUp},     {"Ramp Down", doc::ops::ShapeKind::RampDown},
      {"Sine", doc::ops::ShapeKind::Sine},          {"Triangle", doc::ops::ShapeKind::Triangle},
      {"Square", doc::ops::ShapeKind::Square},      {"Saw", doc::ops::ShapeKind::Saw}};
  for (const auto& [label, kind] : kinds) {
    MenuItem m = MenuItem::item(label, [this, k = kind] { (void)insertShape(k); });
    m.enabled = range;
    shapes.push_back(std::move(m));
  }
  std::vector<MenuItem> items;
  MenuItem shape = MenuItem::sub("Insert Shape", std::move(shapes));
  shape.enabled = range;
  items.push_back(std::move(shape));
  MenuItem del = MenuItem::item("Delete Breakpoints", [this] { (void)deleteSelected(); }, "Del");
  del.enabled = !selPoints_.empty() && selParam_ == b.param;
  items.push_back(std::move(del));
  items.push_back(MenuItem::sep());
  MenuItem re = MenuItem::item("Re-enable Automation", [this] { reenable(); });
  re.enabled = ctx_.anyAutomationOverridden();
  items.push_back(std::move(re));
  items.push_back(MenuItem::sep());
  if (const doc::Track* tr = doc::findTrackPtr(p, b.track)) {
    const doc::TrackId t = b.track;
    const bool over = b.overClips;
    const uint32_t index = b.laneIndex >= 0 ? view_.laneRows()[static_cast<size_t>(b.laneIndex)].index : 0;
    items.push_back(MenuItem::sub("Parameter", chooserItems(p, *tr, b.param, [this, t, over, index](doc::ParamRef r) {
      if (over) choose(t, r);
      else setLaneParam(t, index, r);
    })));
    if (tr->kind != doc::TrackKind::Return && tr->kind != doc::TrackKind::Master) {
      const bool shown = lanesShown(t);
      items.push_back(MenuItem::item(shown ? "Hide Automation Lanes" : "Show Automation Lanes", [this, t, shown] { showLanes(t, !shown); }));
    }
  }
  MenuItem mode = MenuItem::item("Automation Mode", {}, "A");
  mode.action = "arrangement.toggleAutomationMode";
  mode.checked = mode_;
  items.push_back(std::move(mode));
  return ui::openContextMenu(view_, at, std::move(items));
}

}  // namespace pq::views
