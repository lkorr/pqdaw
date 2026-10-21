// ArrangementView (T3.8): layout, rows, axis (zoom / scroll), Z / X, follow, playhead, actions, install.

#include <algorithm>
#include <cassert>
#include <cmath>

#include "internal.h"
#include "pq/document/edit_ops.h"
#include "pq/views/shell/shell.h"

namespace pq::views {

using namespace pq::ui;
namespace am = arrangement_metrics;
using arrangement::Model;

namespace {

constexpr float kScrollbarFallback = 10.0f;
constexpr size_t kMaxZoomStack = 32;

constexpr std::string_view kActionIds[] = {"arrangement.zoomToSelection", "arrangement.zoomBack", "arrangement.zoomIn",
                                           "arrangement.zoomOut", "view.toggleFollow"};

float scrollbarWidth(const UiTree* tree) {
  const PaintResources* res = tree ? tree->resources() : nullptr;
  return res && res->theme ? res->theme->size.scrollbar : kScrollbarFallback;
}

// The tuning a clip's pitches resolve in: its own assignment, else the row's (Inherit resolved up the groups).
const doc::TuningDef* assignmentDef(const doc::Project& p, const doc::TuningAssignment& a, const doc::TuningDef* inherited) {
  switch (a.mode) {
    case doc::TuningMode::Inherit: return inherited;
    case doc::TuningMode::Main: {
      const doc::TuningSlot* s = doc::findTuning(p, a.main);
      return s ? &s->def : inherited;
    }
    case doc::TuningMode::Private: return a.privateDef ? a.privateDef.get() : inherited;
  }
  return inherited;
}

}  // namespace

// --- construction ---------------------------------------------------------------------------------------

ArrangementView::ArrangementView(ViewContext& ctx) : Widget("arrangement"), ctx_(ctx), model_(std::make_unique<Model>()) {
  setLayout(LayoutKind::Custom);
  setClipChildren(true);
  setFocusable(true);
  setKeyScope(ctx_.scope("arrangement"));

  overview_ = &add<arrangement::Overview>(*this);
  overviewCorner_ = &add<arrangement::Corner>();
  ruler_ = &add<arrangement::Ruler>(*this);
  rulerCorner_ = &add<arrangement::Corner>();
  scroll_ = &add<ScrollView>("tracks", false, true);
  stack_ = &scroll_->add<arrangement::TrackStack>(*this);
  lanes_ = &stack_->add<arrangement::LaneCanvas>(*this);
  timeRuler_ = &add<arrangement::TimeRuler>(*this);
  timeCorner_ = &add<arrangement::Corner>();
  add<Scrollbar>("vscroll", *scroll_, Scrollbar::Axis::Vertical);
  overview_->attachScroll(*scroll_);

  docSub_ = ctx_.subscribe([this](const doc::StoreEvent& e) { onDocument(e); });
  viewSub_ = ctx_.observe([this](uint32_t bits) { onViewChange(bits); });
  rebuildRows();
  registerActions();
  refreshTransport();
}

ArrangementView::~ArrangementView() {
  if (playheadTimer_) cancelTimer(playheadTimer_);
  // Park the actions: the registry outlives the view (a rebuilt shell installs a new arrangement over them).
  for (std::string_view id : kActionIds)
    (void)ctx_.actions().set(std::string(id), [] {}, [] { return false; });
}

const doc::Snapshot& ArrangementView::shown() const noexcept { return model_->shown; }
std::span<const ArrangementRow> ArrangementView::rows() const noexcept { return model_->rows; }
const ArrangementView::Stats& ArrangementView::stats() const noexcept { return model_->stats; }
size_t ArrangementView::previewCacheSize() const noexcept { return model_->previewCount(); }

size_t ArrangementView::previewCacheNotes() const noexcept { return model_->cachedNotes(); }
void ArrangementView::setPreviewCacheLimit(size_t notes) { model_->setCacheLimit(notes); }

void ArrangementView::dropPreviews() {
  model_->dropPreviews();
  lanes_->markDirty();
}

void ArrangementView::registerActions() {
  ActionRegistry& a = ctx_.actions();
  auto set = [&](std::string_view id, ActionRegistry::Handler h) {
    [[maybe_unused]] auto r = a.set(std::string(id), std::move(h));
    assert(r.ok());
  };
  set(kActionIds[0], [this] { zoomToSelection(); });
  set(kActionIds[1], [this] { (void)zoomBack(); });
  set(kActionIds[2], [this] { zoomIn(); });
  set(kActionIds[3], [this] { zoomOut(); });
  set(kActionIds[4], [this] { setFollow(!follow_); });
}

// --- document and view-state changes --------------------------------------------------------------------

void ArrangementView::onDocument(const doc::StoreEvent& e) {
  if (e.kind == doc::StoreEventKind::Reset) {
    model_->clear();
    zoomStack_.clear();
    timeSelection_.reset();
  }
  rebuildRows();
}

void ArrangementView::onViewChange(uint32_t bits) {
  if (bits & view_change::kTransport) refreshTransport();
  if (bits & view_change::kGrid) {
    lanes_->markDirty();
    ruler_->markDirty();
  }
}

// Flattens the track tree into rows (members of folded groups are skipped), with device-px positions from the
// view state's heights (logical px; 0 = the default). Headers are rebound by index.
void ArrangementView::rebuildRows() {
  Model& m = *model_;
  const bool docChanged = m.shown != ctx_.doc();
  m.shown = ctx_.doc();
  ++m.stats.rowRebuilds;
  const doc::Project& p = *m.shown;
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  rowsScale_ = s.factor;
  const doc::ViewState& vs = ctx_.store().view();
  const doc::TuningDef* projectDef = (p.tunings && !p.tunings->empty()) ? &p.tunings->front()->def : nullptr;

  m.rows.clear();
  double cum = 0.0;  // logical px
  size_t clipCount = 0;
  struct Walk {
    ArrangementView& v;
    const doc::Project& p;
    const doc::ViewState& vs;
    const UiScale& s;
    std::vector<ArrangementRow>& rows;
    double& cum;
    size_t& clipCount;
    void operator()(const std::vector<doc::TrackPtr>& list, int32_t parent, uint16_t depth, const doc::TuningDef* inherited) {
      for (const doc::TrackPtr& t : list) {
        clipCount += t->clips.size();
        const auto it = vs.tracks.find(t->id);
        const doc::TrackView tv = it != vs.tracks.end() ? it->second : doc::TrackView{};
        ArrangementRow r;
        r.id = t->id;
        r.track = t.get();
        r.parent = parent;
        r.depth = depth;
        r.group = t->kind == doc::TrackKind::Group;
        r.folded = r.group && tv.folded;
        r.tuning = assignmentDef(p, t->tuning, inherited);
        const double h = tv.height > 0 ? std::max<double>(tv.height, am::kMinTrackHeight) : am::kTrackHeight;
        r.y = s.px(static_cast<float>(cum));
        cum += h;
        r.h = s.px(static_cast<float>(cum)) - r.y;
        rows.push_back(r);
        const auto index = static_cast<int32_t>(rows.size() - 1);
        if (r.group && !r.folded) (*this)(t->children, index, static_cast<uint16_t>(depth + 1), r.tuning);
        else if (r.group) countHidden(t->children);
      }
    }
    void countHidden(const std::vector<doc::TrackPtr>& list) {
      for (const doc::TrackPtr& t : list) {
        clipCount += t->clips.size();
        countHidden(t->children);
      }
    }
  } walk{*this, p, vs, s, m.rows, cum, clipCount};
  walk(p.tracks, -1, 0, projectDef);
  m.contentHeight = s.px(static_cast<float>(cum));
  if (docChanged || m.songEnd == 0) {
    Ticks end = 8 * kTicksPerWhole;
    doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
      if (!t.clips.empty()) end = std::max(end, model_->clips(t).maxEnd.back());
    });
    end = std::max(end, p.loop.end);
    if (p.locators && !p.locators->empty()) end = std::max(end, p.locators->back().tick);
    m.songEnd = end;
  }
  if (docChanged) m.afterDocumentChange(clipCount);

  syncHeaders();
  overview_->invalidateCache();
  stack_->invalidateLayout();
  stack_->invalidateArrange();
  markDirty();
}

void ArrangementView::syncHeaders() {
  const auto& rows = model_->rows;
  while (headers_.size() > rows.size()) {
    TrackHeader* h = headers_.back();
    headers_.pop_back();
    (void)stack_->removeChild(*h);
  }
  while (headers_.size() < rows.size())
    headers_.push_back(&stack_->add<TrackHeader>("track[" + std::to_string(headers_.size()) + "]", *this));
  for (size_t i = 0; i < rows.size(); ++i) {
    headers_[i]->bind(rows[i], rows);
    if (decorator_) decorator_(*headers_[i], rows[i]);
  }
}

TrackHeader* ArrangementView::header(doc::TrackId id) const noexcept {
  for (TrackHeader* h : headers_)
    if (h->trackId() == id) return h;
  return nullptr;
}

void ArrangementView::toggleFold(doc::TrackId id) {
  (void)doc::ops::toggleFolded(ctx_.store().view(), id);
  rebuildRows();
}

void ArrangementView::setHeaderWidth(float logicalPx) {
  logicalPx = std::max(60.0f, logicalPx);
  if (logicalPx == headerWidth_) return;
  headerWidth_ = logicalPx;
  invalidateArrange();
  stack_->invalidateArrange();
  markDirty();
}

void ArrangementView::setHeaderMixerWidth(float logicalPx) {
  logicalPx = std::max(0.0f, logicalPx);
  if (logicalPx == mixerWidth_) return;
  mixerWidth_ = logicalPx;
  for (TrackHeader* h : headers_) {
    h->invalidateArrange();
    h->markDirty();
  }
}

void ArrangementView::setHeaderDecorator(HeaderDecorator d) {
  decorator_ = std::move(d);
  if (decorator_)
    for (size_t i = 0; i < headers_.size(); ++i) decorator_(*headers_[i], model_->rows[i]);
}

// --- layout ---------------------------------------------------------------------------------------------

void ArrangementView::arrange(LayoutPass& pass) {
  if (tree() && tree()->scale() != rowsScale_) rebuildRows();
  const IRect r = rect();
  const UiScale s(pass.scale());
  const int32_t sb = s.px(scrollbarWidth(tree()));
  const int32_t hdr = s.px(headerWidth_);
  const int32_t div = std::max(1, s.px(1.0f));
  const int32_t ovH = std::min(r.h, s.px(am::kOverviewHeight));
  const int32_t ruH = std::min(r.h - ovH, s.px(am::kRulerHeight));
  const int32_t lanesW = std::max(0, r.w - sb - hdr - div);
  pass.place(*overview_, {r.x, r.y, lanesW, ovH});
  pass.place(*overviewCorner_, {r.x + lanesW, r.y, r.w - lanesW, ovH});
  pass.place(*ruler_, {r.x, r.y + ovH, lanesW, ruH});
  pass.place(*rulerCorner_, {r.x + lanesW, r.y + ovH, r.w - lanesW, ruH});
  const int32_t bodyY = r.y + ovH + ruH;
  const int32_t trH = std::min(std::max(0, r.h - ovH - ruH), s.px(am::kTimeRulerHeight));
  const int32_t bodyH = std::max(0, r.h - ovH - ruH - trH);
  pass.place(*scroll_, {r.x, bodyY, std::max(0, r.w - sb), bodyH});
  pass.place(*timeRuler_, {r.x, bodyY + bodyH, lanesW, trH});
  pass.place(*timeCorner_, {r.x + lanesW, bodyY + bodyH, r.w - lanesW, trH});
  Widget& vscroll = child(childCount() - 1);
  pass.place(vscroll, {r.right() - sb, bodyY, sb, bodyH});
}

void ArrangementView::paint(PaintContext&) {}

IRect ArrangementView::lanesViewport() const noexcept {
  const IRect sv = scroll_->rect();
  const IRect lr = lanes_->rect();
  return {sv.x, sv.y, lr.w, sv.h};
}

// --- axis -----------------------------------------------------------------------------------------------

double ArrangementView::devicePxPerTick() const noexcept { return pxPerTick_ * (tree() ? tree()->scale() : 1.0f); }

float ArrangementView::xOfTick(double tick) const noexcept {
  return static_cast<float>(static_cast<double>(lanes_->rect().x) + (tick - originTick_) * devicePxPerTick());
}

double ArrangementView::tickAtX(float deviceX) const noexcept {
  return originTick_ + (static_cast<double>(deviceX) - static_cast<double>(lanes_->rect().x)) / devicePxPerTick();
}

TickRange ArrangementView::visibleTicks() const noexcept {
  const double w = static_cast<double>(lanes_->rect().w);
  return {static_cast<Ticks>(std::floor(originTick_)), static_cast<Ticks>(std::ceil(originTick_ + w / devicePxPerTick()))};
}

Ticks ArrangementView::songEnd() const noexcept { return model_->songEnd; }

void ArrangementView::setTimeView(double origin, double pxPerTick) {
  if (!std::isfinite(origin) || !std::isfinite(pxPerTick)) return;
  pxPerTick = std::clamp(pxPerTick, am::kMinPxPerTick, am::kMaxPxPerTick);
  origin = std::clamp(origin, 0.0, static_cast<double>(doc::kMaxTicks / 2));
  if (origin == originTick_ && pxPerTick == pxPerTick_) return;
  const bool zoomChanged = pxPerTick != pxPerTick_;
  originTick_ = origin;
  pxPerTick_ = pxPerTick;
  markAxisDirty();
  if (zoomChanged) {
    reportResolvedGrid();
    // A zoom gesture lasts until kZoomSettleSeconds after its last step (dense clips skip their previews meanwhile).
    if (tree()) {
      zooming_ = true;
      if (settleTimer_) cancelTimer(settleTimer_);
      settleTimer_ = requestTimer(am::kZoomSettleSeconds);
    }
  }
}

void ArrangementView::markAxisDirty() {
  overview_->markDirty();
  ruler_->markDirty();
  timeRuler_->markDirty();
  lanes_->markDirty();
}

void ArrangementView::zoomAt(float anchorX, double factor) {
  if (!(factor > 0.0)) return;
  const double anchor = tickAtX(anchorX);
  const double z = std::clamp(pxPerTick_ * factor, am::kMinPxPerTick, am::kMaxPxPerTick);
  const double dz = z * (tree() ? tree()->scale() : 1.0f);
  setTimeView(anchor - (static_cast<double>(anchorX) - static_cast<double>(lanes_->rect().x)) / dz, z);
}

void ArrangementView::scrollByPx(double devicePx) { setTimeView(originTick_ + devicePx / devicePxPerTick(), pxPerTick_); }

int32_t ArrangementView::scrollY() const noexcept { return scroll_->scrollY(); }
void ArrangementView::scrollToY(int32_t devicePx) { scroll_->scrollTo(0, devicePx); }

int32_t ArrangementView::rowAt(float deviceY) const noexcept {
  const IRect sv = scroll_->rect();
  if (deviceY < static_cast<float>(sv.y) || deviceY >= static_cast<float>(sv.bottom())) return -1;
  const float local = deviceY - static_cast<float>(stack_->rect().y);
  const auto& rows = model_->rows;
  const auto it = std::upper_bound(rows.begin(), rows.end(), local,
                                   [](float y, const ArrangementRow& r) { return y < static_cast<float>(r.y + r.h); });
  if (it == rows.end() || local < static_cast<float>(it->y)) return -1;
  return static_cast<int32_t>(it - rows.begin());
}

IRect ArrangementView::rowLaneRect(int32_t row) const noexcept {
  if (row < 0 || static_cast<size_t>(row) >= model_->rows.size()) return {};
  const ArrangementRow& r = model_->rows[static_cast<size_t>(row)];
  const IRect lr = lanes_->rect();
  return {lr.x, stack_->rect().y + r.y, lr.w, r.h};
}

// --- Z / X ----------------------------------------------------------------------------------------------

void ArrangementView::setTimeSelection(std::optional<TickRange> r) {
  if (r && r->end <= r->start) r.reset();
  timeSelection_ = r;
}

void ArrangementView::pushZoom() {
  if (zoomStack_.size() == kMaxZoomStack) zoomStack_.erase(zoomStack_.begin());
  zoomStack_.push_back({originTick_, pxPerTick_});
}

void ArrangementView::zoomToSelection() {
  const IRect lv = lanesViewport();
  if (lv.w <= 0) return;
  const TickRange range = timeSelection_ ? *timeSelection_ : TickRange{0, songEnd()};
  const double len = static_cast<double>(std::max<Ticks>(1, range.end - range.start));
  const double logicalW = static_cast<double>(lv.w) / (tree() ? tree()->scale() : 1.0f);
  pushZoom();
  setTimeView(static_cast<double>(range.start), logicalW / len);
}

bool ArrangementView::zoomBack() {
  if (zoomStack_.empty()) return false;
  const SavedView v = zoomStack_.back();
  zoomStack_.pop_back();
  setTimeView(v.origin, v.pxPerTick);
  return true;
}

void ArrangementView::zoomIn() {
  const IRect lv = lanesViewport();
  zoomAt(static_cast<float>(lv.x) + static_cast<float>(lv.w) * 0.5f, 2.0);
}

void ArrangementView::zoomOut() {
  const IRect lv = lanesViewport();
  zoomAt(static_cast<float>(lv.x) + static_cast<float>(lv.w) * 0.5f, 0.5);
}

void ArrangementView::reportResolvedGrid() {
  const UiTree* t = tree();
  const bool focusInside = t && t->focus() && (t->focus() == this || isAncestorOf(*t->focus()));
  if (!focusInside) return;
  const GridSpec& spec = ctx_.grid();
  if (spec.off) {
    ctx_.setResolvedGrid(std::nullopt);
    return;
  }
  const Grid g(spec, model_->shown->timeline->sigMap, pxPerTick_);
  ctx_.setResolvedGrid(g.divisionAt(static_cast<Ticks>(originTick_)));
}

void ArrangementView::onFocusChanged(bool focused) {
  if (focused) reportResolvedGrid();
}

// --- follow and playhead ----------------------------------------------------------------------------------

void ArrangementView::setFollow(bool on) { follow_ = on; }

void ArrangementView::locateAt(Ticks tick, bool snapToGrid) {
  tick = std::max<Ticks>(0, tick);
  if (snapToGrid) {
    const Grid g(ctx_.grid(), model_->shown->timeline->sigMap, pxPerTick_);
    tick = std::max<Ticks>(0, g.snap(tick));
  }
  ctx_.setStartMarker(tick);
  ctx_.transport().locate(tick);
}

// Called on transport notifications (play/stop requested, start marker moved): the only reads while stopped.
void ArrangementView::refreshTransport() {
  ruler_->markDirty();  // start marker
  ++model_->stats.playheadReads;
  const PlayheadSnapshot ph = ctx_.transport().playhead();
  if (ph.playing) {
    if (!playhead_ || *playhead_ != ph.tick) {
      if (playhead_) damagePlayheadColumn(*playhead_);
      playhead_ = ph.tick;
      damagePlayheadColumn(ph.tick);
    }
    if (!playheadTimer_) schedulePlayhead();
    return;
  }
  if (playhead_) {
    damagePlayheadColumn(*playhead_);
    playhead_.reset();
  }
  if (playheadTimer_) {
    cancelTimer(playheadTimer_);
    playheadTimer_ = 0;
  }
}

// While playing, wake when the playhead will have moved by about one device pixel (from the tempo at the playhead),
// bounded to one frame at 144 Hz … 100 ms: never faster than the display can show it.
void ArrangementView::schedulePlayhead() {
  const Ticks t = playhead_.value_or(0);
  const double bpm = model_->shown->timeline->tempoMap.bpmAt(t);
  const double ticksPerSecond = bpm / 60.0 * static_cast<double>(kPpq);
  const double secondsPerPx = 1.0 / (devicePxPerTick() * ticksPerSecond);
  playheadTimer_ = requestTimer(std::clamp(secondsPerPx, 1.0 / 144.0, 0.1));
}

void ArrangementView::onTimer(uint32_t id) {
  if (id == settleTimer_) {
    settleTimer_ = 0;
    zooming_ = false;
    if (model_->skippedForZoom) {
      model_->skippedForZoom = false;
      lanes_->requestFrame();  // builds the dense clips' previews, then repaints the lanes once
    }
    return;
  }
  if (id != playheadTimer_) return;
  playheadTimer_ = 0;
  readPlayhead();
}

void ArrangementView::readPlayhead() {
  ++model_->stats.playheadReads;
  const PlayheadSnapshot ph = ctx_.transport().playhead();
  if (!ph.playing) {
    refreshTransport();
    return;
  }
  const IRect lv = lanesViewport();
  if (follow_ && lv.w > 0) {
    const float x = xOfTick(static_cast<double>(ph.tick));
    if (x < static_cast<float>(lv.x) || x >= static_cast<float>(lv.right() - 1)) {
      // Page: the playhead goes to the left edge (Live's Follow scrolls in pages).
      playhead_ = ph.tick;
      setTimeView(static_cast<double>(ph.tick), pxPerTick_);
      schedulePlayhead();
      return;
    }
  }
  if (!playhead_ || *playhead_ != ph.tick) {
    const bool moved = !playhead_ || std::floor(xOfTick(static_cast<double>(*playhead_))) !=
                                         std::floor(xOfTick(static_cast<double>(ph.tick)));
    if (moved && playhead_) damagePlayheadColumn(*playhead_);
    playhead_ = ph.tick;
    if (moved) damagePlayheadColumn(ph.tick);
  }
  schedulePlayhead();
}

// The playhead line's pixels in the ruler and the lanes (plus a pixel either side), clipped to what is visible.
void ArrangementView::damagePlayheadColumn(Ticks t) {
  UiTree* tr = tree();
  if (!tr) return;
  const IRect lv = lanesViewport();
  const IRect ru = ruler_->rect();
  const float x = std::floor(xOfTick(static_cast<double>(t)));
  const int32_t w = std::max(1, tr->uiScale().px(1.0f));
  if (x + static_cast<float>(w) < static_cast<float>(lv.x) || x >= static_cast<float>(lv.right())) return;
  const IRect col{static_cast<int32_t>(x) - 1, ru.y, w + 2, lv.bottom() - ru.y};
  const IRect vis = intersect(intersect(col, {lv.x, ru.y, lv.w, lv.bottom() - ru.y}), rect());
  if (!vis.empty()) tr->damage().add(vis.toRect());
}

// --- install --------------------------------------------------------------------------------------------

ArrangementView& installArrangement(Shell& shell) {
  Widget& slot = shell.slot(ShellSlot::TrackArea);
  auto& view = slot.add<ArrangementView>(shell.context());
  shell.tree().setFocus(&view);
  return view;
}

}  // namespace pq::views
