// ArrangementView (T3.8): layout, rows, axis (zoom / scroll), Z / X, follow, playhead, actions, install.

#include <algorithm>
#include <cassert>
#include <cmath>

#include "../common/drag.h"
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

constexpr std::string_view kActionIds[] = {
    "arrangement.zoomToSelection", "arrangement.zoomBack", "arrangement.zoomIn", "arrangement.zoomOut",
    "view.toggleFollow",
    // T3.9
    "arrangement.selectAll", "arrangement.copy", "arrangement.cut", "arrangement.paste", "arrangement.duplicate",
    "arrangement.delete", "arrangement.split", "arrangement.consolidate", "arrangement.loopSelection",
    "arrangement.insertMidiClip", "arrangement.insertTime", "arrangement.duplicateTime", "arrangement.deleteTime",
    "arrangement.toggleActive",
    // T3.x-fix-look: arrow keys
    "arrangement.markerLeft", "arrangement.markerRight", "arrangement.trackUp", "arrangement.trackDown",
    // T4.16b: Alt / Shift arrows, Esc
    "arrangement.nudgeFineLeft", "arrangement.nudgeFineRight", "arrangement.extendLeft", "arrangement.extendRight",
    "arrangement.deselect",
    // T3.10
    "track.newMidi", "track.newReturn", "track.group", "track.ungroup", "arrangement.rename",
    // T5.3
    "track.newAudio",
    // T3.15
    "arrangement.toggleAutomationMode"};

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

ArrangementView::ArrangementView(ViewContext& ctx)
    : Widget("arrangement"),
      ctx_(ctx),
      model_(std::make_unique<Model>()),
      editor_(std::make_unique<arrangement::Editor>(*this)),
      automation_(std::make_unique<ArrangementAutomation>(*this)) {
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
  bottom_ = &add<arrangement::BottomTracks>(*this);  // T3.10: returns + master, pinned
  reenable_ = &add<arrangement::ReenableButton>(*this);  // T3.15
  add<Scrollbar>("vscroll", *scroll_, Scrollbar::Axis::Vertical);  // last (arrange() relies on it)
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
std::span<const ArrangementRow> ArrangementView::bottomRows() const noexcept { return model_->bottomRows; }
std::span<const AutomationLaneRow> ArrangementView::laneRows() const noexcept { return model_->laneRows; }
Widget* ArrangementView::reenableButton() const noexcept { return reenable_; }
IRect ArrangementView::bottomRect() const noexcept { return bottom_->visible() ? bottom_->rect() : IRect{}; }
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
  // T3.9 editing. A disabled action's chord falls through (Q27), e.g. Delete with nothing selected.
  arrangement::Editor* const ed = editor_.get();
  auto edit = [&](std::string_view id, std::function<void()> h, std::function<bool()> en) {
    [[maybe_unused]] auto r = a.set(std::string(id), std::move(h), std::move(en));
    assert(r.ok());
  };
  auto hasTime = [ed] { return ed->selection().time && ed->selection().time->end > ed->selection().time->start; };
  auto hasRange = [ed, hasTime] { return ed->selection().range && hasTime(); };
  auto hasClips = [ed] { return !ed->selection().clips.empty(); };
  auto clipsOrRange = [hasClips, hasRange] { return hasClips() || hasRange(); };
  edit("arrangement.selectAll", [ed] { ed->selectAll(); }, {});
  edit("arrangement.copy", [ed] { (void)ed->copySelection(); }, clipsOrRange);
  edit("arrangement.cut", [ed] { (void)ed->cutSelection(); }, clipsOrRange);
  edit("arrangement.paste", [ed] { (void)ed->paste(); }, [ed] { return ed->hasClipboard(); });
  edit("arrangement.duplicate", [ed] { (void)ed->duplicateSelection(); }, clipsOrRange);
  // Delete: clips or a range; with only tracks selected (a header click: no time selection) the tracks (T3.10);
  // selected breakpoints first (T3.15: one selection at a time).
  ArrangementAutomation* const au = automation_.get();
  edit("arrangement.delete",
       [ed, au] {
         if (!au->selectedPoints().empty()) (void)au->deleteSelected();
         else (void)ed->deleteSelection();
       },
       [ed, au, clipsOrRange] {
         return !au->selectedPoints().empty() || clipsOrRange() || (!ed->selection().tracks.empty() && !ed->selection().time);
       });
  edit("arrangement.split", [ed] { (void)ed->splitSelection(); }, [ed] { return ed->selection().time.has_value(); });
  edit("arrangement.consolidate", [ed] { (void)ed->consolidateSelection(); }, clipsOrRange);
  edit("arrangement.loopSelection", [ed] { (void)ed->loopSelection(); }, hasTime);
  edit("arrangement.insertMidiClip", [ed] { (void)ed->insertMidiClip(); },
       [ed] { return ed->selection().time.has_value() && !ed->selection().tracks.empty(); });
  edit("arrangement.insertTime", [ed] { (void)ed->insertTime(); }, hasTime);
  edit("arrangement.duplicateTime", [ed] { (void)ed->duplicateTime(); }, hasTime);
  edit("arrangement.deleteTime", [ed] { (void)ed->deleteTime(); }, hasTime);
  edit("arrangement.toggleActive", [ed] { (void)ed->toggleActive(); },
       [ed] { return !ed->selection().clips.empty() || !ed->selection().tracks.empty(); });
  // T3.10 tracks. Ctrl+Shift+T / Ctrl+Alt+T are global chords (Live inserts tracks from anywhere).
  auto hasTracks = [ed] { return !ed->selection().tracks.empty(); };
  edit("track.newMidi", [ed] { (void)ed->newTrack(doc::TrackKind::Midi); }, {});
  edit("track.newAudio", [ed] { (void)ed->newTrack(doc::TrackKind::Audio); }, {});  // T5.3: Ctrl+T
  edit("track.newReturn", [ed] { (void)ed->newTrack(doc::TrackKind::Return); }, {});
  edit("track.group", [ed] { (void)ed->groupSelection(); }, hasTracks);
  edit("track.ungroup", [ed] { (void)ed->ungroupSelection(); }, [this, ed] {
    for (doc::TrackId id : ed->selection().tracks)
      if (const doc::Track* t = doc::findTrackPtr(*ctx_.doc(), id); t && t->kind == doc::TrackKind::Group) return true;
    return false;
  });
  edit("arrangement.rename", [ed] { (void)ed->renameSelection(); }, hasTracks);
  // T3.x-fix-look: arrow keys (Live): Left / Right move the insert marker by a grid cell, Up / Down select the track
  // above / below (Shift extends the selection). T4.16b: with clips selected (not a range) Left / Right move them by
  // a grid cell instead, Alt+Left / Right off the grid; Shift+Left / Right move the time selection's end; Esc drops
  // the clip / time / breakpoint selection.
  edit("arrangement.markerLeft", [ed] { ed->clipsNudgeable() ? (void)ed->nudgeClips(-1) : ed->nudgeMarker(-1); }, {});
  edit("arrangement.markerRight", [ed] { ed->clipsNudgeable() ? (void)ed->nudgeClips(1) : ed->nudgeMarker(1); }, {});
  edit("arrangement.nudgeFineLeft", [ed] { (void)ed->nudgeClipsFine(-1); }, [ed] { return ed->clipsNudgeable(); });
  edit("arrangement.nudgeFineRight", [ed] { (void)ed->nudgeClipsFine(1); }, [ed] { return ed->clipsNudgeable(); });
  edit("arrangement.extendLeft", [ed] { ed->extendTime(-1); }, {});
  edit("arrangement.extendRight", [ed] { ed->extendTime(1); }, {});
  edit("arrangement.deselect",
       [ed, au] {
         au->clearSelection();
         (void)ed->deselect();
       },
       [ed, au] { return au->hasSelection() || !ed->selection().clips.empty() || ed->selection().time.has_value(); });
  edit("arrangement.trackUp", [ed] { ed->stepTrack(-1); }, {});
  edit("arrangement.trackDown", [ed] { ed->stepTrack(1); }, {});
  // T3.15: A toggles automation mode (Live).
  edit("arrangement.toggleAutomationMode", [au] { au->setMode(!au->mode()); }, {});
}

// --- editing (T3.9: arrangement::Editor, editing.cpp) ----------------------------------------------------

const ArrangementView::Selection& ArrangementView::selection() const noexcept { return editor_->selection(); }
void ArrangementView::setSelection(Selection s, bool showInClipView) { editor_->setSelection(std::move(s), showInClipView); }
void ArrangementView::selectClips(std::span<const doc::ClipRef> clips, bool showInClipView) {
  editor_->setSelection(editor_->selectionOfClips({clips.begin(), clips.end()}), showInClipView);
}
void ArrangementView::clearSelection() { editor_->setSelection({}, false); }
bool ArrangementView::clipSelected(doc::ClipId id) const noexcept { return editor_->selected(id); }
bool ArrangementView::dragging() const noexcept { return editor_->dragging(); }
bool ArrangementView::deleteSelection() { return editor_->deleteSelection(); }
bool ArrangementView::copySelection() { return editor_->copySelection(); }
bool ArrangementView::cutSelection() { return editor_->cutSelection(); }
bool ArrangementView::paste() { return editor_->paste(); }
bool ArrangementView::duplicateSelection() { return editor_->duplicateSelection(); }
bool ArrangementView::splitSelection() { return editor_->splitSelection(); }
bool ArrangementView::consolidateSelection() { return editor_->consolidateSelection(); }
bool ArrangementView::insertMidiClip() { return editor_->insertMidiClip(); }
bool ArrangementView::insertTime() { return editor_->insertTime(); }
bool ArrangementView::duplicateTime() { return editor_->duplicateTime(); }
bool ArrangementView::deleteTime() { return editor_->deleteTime(); }
bool ArrangementView::loopSelection() { return editor_->loopSelection(); }
bool ArrangementView::toggleActive() { return editor_->toggleActive(); }
void ArrangementView::selectTrack(doc::TrackId id, Mods mods) { editor_->selectTrack(id, mods); }
bool ArrangementView::newTrack(doc::TrackKind kind) { return editor_->newTrack(kind); }
bool ArrangementView::groupSelection() { return editor_->groupSelection(); }
bool ArrangementView::ungroupSelection() { return editor_->ungroupSelection(); }
bool ArrangementView::deleteTracks() { return editor_->deleteTracks(); }
bool ArrangementView::renameSelection() { return editor_->renameSelection(); }
bool ArrangementView::setTracksColour(doc::Colour c) { return editor_->setTracksColour(c); }
Menu* ArrangementView::openTrackMenu(doc::TrackId id, Vec2 at) { return editor_->openTrackMenu(id, at); }
Widget* ArrangementView::openColourPicker(Vec2 at) { return editor_->openColourPicker(at); }
void ArrangementView::selectAll() { editor_->selectAll(); }
bool ArrangementView::hasClipboard() const noexcept { return editor_->hasClipboard(); }
Menu* ArrangementView::openContextMenu(Vec2 at) { return editor_->openContextMenu(at); }

IRect ArrangementView::clipRect(const doc::ClipRef& ref) const noexcept {
  const int32_t row = [&] {
    for (size_t i = 0; i < model_->rows.size(); ++i)
      if (model_->rows[i].id == ref.track) return static_cast<int32_t>(i);
    return -1;
  }();
  if (row < 0) return {};
  const doc::Track& t = *model_->rows[static_cast<size_t>(row)].track;
  const int32_t i = doc::findClipIndex(t, ref.clip);
  if (i < 0) return {};
  const doc::ClipSlot& c = t.clips[static_cast<size_t>(i)];
  return editor_->clipBox(row, c.start(), c.end());
}

ArrangementView::ClipHover ArrangementView::clipHover() const noexcept {
  const arrangement::Editor::HoverPart& h = editor_->hoverPart();
  return {h.ref, h.zone};
}

bool ArrangementView::onKey(const KeyEvent& e) {
  // T4.16b: a modifier pressed or released with the pointer still changes what a press would do (Alt over a segment
  // curves it): the lanes' hover and cursor follow. Not consumed (the keymap ignores lone modifiers anyway).
  if (isModifierKey(e.key)) lanes_->modsChanged(drag::modsAfterKey(e));
  if (e.down && e.key == key::kEscape && ruler_->cancelDrag()) {
    if (tree() && tree()->captured()) tree()->releaseCapture();
    return true;
  }
  if (e.down && e.key == key::kEscape && automation_->dragging()) {
    automation_->abortDrag();
    if (tree() && tree()->captured()) tree()->releaseCapture();
    return true;
  }
  if (e.down && e.key == key::kEscape && editor_->dragging()) {
    editor_->abortDrag();
    if (tree() && tree()->captured()) tree()->releaseCapture();
    return true;
  }
  return false;
}

// --- document and view-state changes --------------------------------------------------------------------

void ArrangementView::onDocument(const doc::StoreEvent& e) {
  if (e.kind == doc::StoreEventKind::Reset) {
    model_->clear();
    zoomStack_.clear();
    timeSelection_.reset();
  }
  // T3.x-fix-paint (M3 review M12a): a mixer value change (volume, pan, sends, solo: a Param event per frame of a drag)
  // changes no row, lane or tuning: only the edited tracks' nodes (and their groups') are new. Repoint the rows and
  // rebind just those headers, instead of rebuilding every row and rebinding every header and strip (O(tracks) per
  // drag frame: 93 µs at 256 tracks before).
  if (e.kind != doc::StoreEventKind::Reset && e.changes.paramOnly() && !e.changes.params.empty() &&
      std::all_of(e.changes.params.begin(), e.changes.params.end(), [](const doc::ParamRef& r) {
        return r.kind == doc::ParamKind::TrackVolume || r.kind == doc::ParamKind::TrackPan || r.kind == doc::ParamKind::TrackSolo ||
               r.kind == doc::ParamKind::TrackSend;
      })) {
    automation_->afterDocumentChange(e);
    rebindTrackNodes();
    return;
  }
  automation_->afterDocumentChange(e);
  // T3.10: mixer edits (volume, pan, sends, solo: Param) and header-only Meta edits (track names, colours, arm) do
  // not change what the lanes, ruler or overview show: rebind the rows and headers (whose widgets repaint what
  // changed) without repainting the view. A mute changes the clips' look (muted tracks draw inactive): full repaint.
  const doc::ChangeSet& c = e.changes;
  bool lanesUnchanged = false;
  bool automationOnly = false;
  if (e.kind != doc::StoreEventKind::Reset && !c.empty()) {
    if (c.automationOnly()) {
      // T3.15: breakpoints of existing lanes: only the bands showing those lanes repaint.
      lanesUnchanged = automationOnly = true;
    } else if (c.paramOnly()) {
      lanesUnchanged = std::none_of(c.params.begin(), c.params.end(),
                                    [](const doc::ParamRef& r) { return r.kind == doc::ParamKind::TrackMute; });
    } else if (c.metaOnly()) {
      lanesUnchanged = c.clips.empty() && c.locators.empty() && c.tunings.empty() && c.lanes.empty();
    }
  }
  rebuildRows(lanesUnchanged);
  if (automationOnly) automation_->damageLanes(c.lanes);
  editor_->afterDocumentChange(e);
}

void ArrangementView::onViewChange(uint32_t bits) {
  if (bits & view_change::kTransport) refreshTransport();
  if (bits & view_change::kGrid) {
    lanes_->markDirty();
    ruler_->markDirty();
  }
  if (bits & view_change::kAutomation) {
    // T3.15: overridden envelopes turn grey, the LEDs too; "Re-enable Automation" shows while any is overridden.
    automation_->damageAll();
    syncHeaders();
    if (reenable_->visible() != ctx_.anyAutomationOverridden()) {
      reenable_->setVisible(ctx_.anyAutomationOverridden());
      invalidateArrange();
      rulerCorner_->markDirty();
    }
  }
}

// Flattens the track tree into rows (members of folded groups are skipped), with device-px positions from the
// view state's heights (logical px; 0 = the default). Headers are rebound by index.
void ArrangementView::rebuildRows(bool lanesUnchanged) {
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
  // T3.15: every lane of the project by parameter, then the lanes shown under each row.
  m.laneIndex.clear();
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::LanePtr& l : t.lanes) m.laneIndex.push_back({l->param, t.id, l.get()});
  });
  std::sort(m.laneIndex.begin(), m.laneIndex.end(), [](const Model::LaneEntry& a, const Model::LaneEntry& b) { return a.param < b.param; });
  m.laneRows.clear();
  std::vector<doc::ParamRef> laneParams;
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
    Model& m;
    std::vector<doc::ParamRef>& laneParams;
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
        // T3.15: the automation lanes shown under the track.
        v.automation_->lanesFor(p, *t, laneParams);
        r.firstLane = static_cast<uint32_t>(m.laneRows.size());
        for (size_t k = 0; k < laneParams.size(); ++k) {
          AutomationLaneRow lr;
          lr.track = t->id;
          lr.row = static_cast<int32_t>(rows.size());
          lr.index = static_cast<uint32_t>(k);
          lr.param = laneParams[k];
          const LaneLookup l = m.lane(lr.param);
          lr.owner = l.owner;
          lr.lane = l.lane;
          lr.y = s.px(static_cast<float>(cum));
          cum += v.automation_->laneHeight(t->id, lr.param);
          lr.h = s.px(static_cast<float>(cum)) - lr.y;
          m.laneRows.push_back(lr);
        }
        r.laneCount = static_cast<uint32_t>(laneParams.size());
        r.lanesH = s.px(static_cast<float>(cum)) - (r.y + r.h);
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
  } walk{*this, p, vs, s, m.rows, cum, clipCount, m, laneParams};
  walk(p.tracks, -1, 0, projectDef);
  m.contentHeight = s.px(static_cast<float>(cum));

  // T3.10: the returns and the master, pinned at the bottom (their own y origin).
  m.bottomRows.clear();
  double bcum = 0.0;
  auto addBottom = [&](const doc::Track& t, float defaultH) {
    const auto it = vs.tracks.find(t.id);
    const int32_t vh = it != vs.tracks.end() ? it->second.height : 0;
    const double h = vh > 0 ? std::clamp<double>(vh, am::kMinTrackHeight, am::kMaxTrackHeight) : defaultH;
    ArrangementRow r;
    r.id = t.id;
    r.track = &t;
    r.bottom = true;
    r.y = s.px(static_cast<float>(bcum));
    bcum += h;
    r.h = s.px(static_cast<float>(bcum)) - r.y;
    m.bottomRows.push_back(r);
  };
  for (const doc::TrackPtr& t : p.returns) addBottom(*t, am::kReturnTrackHeight);
  if (p.master) addBottom(*p.master, am::kMasterTrackHeight);
  const int32_t bottomH = s.px(static_cast<float>(bcum));
  const bool bottomResized = bottomH != m.bottomHeight;
  m.bottomHeight = bottomH;
  if (lanesUnchanged && !bottomResized) {
    // Same rows, new track pointers (an edited track is a new node): rebind the headers only.
    syncHeaders();
    return;
  }
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
  bottom_->invalidateArrange();
  invalidateArrange();  // the pinned area's height follows its rows
  markDirty();
}

void ArrangementView::rebindTrackNodes() {
  Model& m = *model_;
  m.shown = ctx_.doc();
  const doc::Project& p = *m.shown;
  // The rows are the flattened tree (folded groups skip their members) and a Param change keeps its shape: walk it in
  // the same order and repoint every row whose track node is new.
  size_t i = 0;
  bool shapeOk = true;
  auto walk = [&](auto&& self, const std::vector<doc::TrackPtr>& list) -> void {
    for (const doc::TrackPtr& t : list) {
      if (!shapeOk) return;
      if (i >= m.rows.size() || m.rows[i].id != t->id) {
        shapeOk = false;
        return;
      }
      ArrangementRow& r = m.rows[i];
      const bool changed = r.track != t.get();
      r.track = t.get();
      if (changed && i < headers_.size()) {
        headers_[i]->bind(r, m.rows);
        if (decorator_) decorator_(*headers_[i], r);
      }
      ++i;
      if (r.group && !r.folded) self(self, t->children);
    }
  };
  walk(walk, p.tracks);
  size_t b = 0;
  auto bottom = [&](const doc::Track& t) {
    if (b >= m.bottomRows.size() || m.bottomRows[b].id != t.id) {
      shapeOk = false;
      return;
    }
    ArrangementRow& r = m.bottomRows[b];
    if (r.track != &t) {
      r.track = &t;
      if (b < bottomHeaders_.size()) {
        bottomHeaders_[b]->bind(r, m.bottomRows);
        if (decorator_) decorator_(*bottomHeaders_[b], r);
      }
    }
    ++b;
  };
  for (const doc::TrackPtr& t : p.returns) bottom(*t);
  if (p.master) bottom(*p.master);
  if (!shapeOk || i != m.rows.size() || b != m.bottomRows.size()) rebuildRows(true);  // not expected: be safe
}

void ArrangementView::syncHeaders() {
  auto sync = [this](std::vector<TrackHeader*>& headers, Widget& host, const std::vector<ArrangementRow>& rows, const char* base) {
    while (headers.size() > rows.size()) {
      TrackHeader* h = headers.back();
      headers.pop_back();
      (void)host.removeChild(*h);
    }
    while (headers.size() < rows.size())
      headers.push_back(&host.add<TrackHeader>(base + std::to_string(headers.size()) + "]", *this));
    for (size_t i = 0; i < rows.size(); ++i) {
      headers[i]->bind(rows[i], rows);
      if (decorator_) decorator_(*headers[i], rows[i]);
    }
  };
  sync(headers_, *stack_, model_->rows, "track[");
  sync(bottomHeaders_, *bottom_, model_->bottomRows, "bottom[");
  // T3.15: one header per automation lane.
  while (laneHeaders_.size() > model_->laneRows.size()) {
    arrangement::LaneHeader* h = laneHeaders_.back();
    laneHeaders_.pop_back();
    (void)stack_->removeChild(*h);
  }
  while (laneHeaders_.size() < model_->laneRows.size())
    laneHeaders_.push_back(&stack_->add<arrangement::LaneHeader>("lane[" + std::to_string(laneHeaders_.size()) + "]", *this));
  for (size_t i = 0; i < laneHeaders_.size(); ++i) {
    const AutomationLaneRow& lr = model_->laneRows[i];
    laneHeaders_[i]->bind(lr, lr.row >= 0 ? model_->rows[static_cast<size_t>(lr.row)].depth : uint16_t{0});
  }
}

TrackHeader* ArrangementView::header(doc::TrackId id) const noexcept {
  for (TrackHeader* h : headers_)
    if (h->trackId() == id) return h;
  for (TrackHeader* h : bottomHeaders_)
    if (h->trackId() == id) return h;
  return nullptr;
}

void ArrangementView::setTrackHeight(doc::TrackId id, float logicalPx) {
  const auto h = static_cast<int32_t>(std::lround(std::clamp(logicalPx, am::kMinTrackHeight, am::kMaxTrackHeight)));
  doc::TrackView& v = ctx_.store().view().tracks[id];
  if (v.height == h) return;
  v.height = h;
  ctx_.viewStateChanged();
  rebuildRows();
}

void ArrangementView::syncHeaderSelection() {
  for (TrackHeader* h : headers_) h->syncSelected();
  for (TrackHeader* h : bottomHeaders_) h->syncSelected();
}

float ArrangementView::trackHeight(doc::TrackId id) const noexcept {
  for (const auto* list : {&model_->rows, &model_->bottomRows})
    for (const ArrangementRow& r : *list)
      if (r.id == id) return static_cast<float>(r.h) / (tree() ? tree()->scale() : 1.0f);
  return 0.0f;
}

void ArrangementView::toggleFold(doc::TrackId id) {
  (void)doc::ops::toggleFolded(ctx_.store().view(), id);
  ctx_.viewStateChanged();
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
  for (auto* list : {&headers_, &bottomHeaders_})
    for (TrackHeader* h : *list) {
      h->invalidateArrange();
      h->markDirty();
    }
}

void ArrangementView::setHeaderDecorator(HeaderDecorator d) {
  decorator_ = std::move(d);
  if (decorator_) {
    for (size_t i = 0; i < headers_.size(); ++i) decorator_(*headers_[i], model_->rows[i]);
    for (size_t i = 0; i < bottomHeaders_.size(); ++i) decorator_(*bottomHeaders_[i], model_->bottomRows[i]);
  }
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
  // T3.15: "Re-enable Automation" in the corner above the headers, while any automation is overridden.
  if (reenable_->visible() != ctx_.anyAutomationOverridden()) reenable_->setVisible(ctx_.anyAutomationOverridden());
  if (reenable_->visible()) {
    const int32_t m = s.px(4.0f);
    pass.place(*reenable_, {r.x + lanesW + div + m, r.y + ovH + m, std::min(s.px(150.0f), std::max(0, r.w - lanesW - div - 2 * m)),
                            std::min(s.px(16.0f), std::max(0, ruH - 2 * m))});
  }
  const int32_t bodyY = r.y + ovH + ruH;
  const int32_t trH = std::min(std::max(0, r.h - ovH - ruH), s.px(am::kTimeRulerHeight));
  const int32_t bodyH = std::max(0, r.h - ovH - ruH - trH);
  // T3.10: returns + master pinned under the scrolling rows (at most kBottomMaxFraction of the body).
  const int32_t pinnedRows = model_->bottomHeight;
  const int32_t pinH = pinnedRows > 0
                           ? std::min(pinnedRows + s.px(am::kBottomGap), static_cast<int32_t>(static_cast<float>(bodyH) * am::kBottomMaxFraction))
                           : 0;
  if (bottom_->visible() != (pinH > 0)) bottom_->setVisible(pinH > 0);
  const int32_t scrollH = bodyH - pinH;
  pass.place(*scroll_, {r.x, bodyY, std::max(0, r.w - sb), scrollH});
  if (pinH > 0) pass.place(*bottom_, {r.x, bodyY + scrollH, r.w, pinH});
  pass.place(*timeRuler_, {r.x, bodyY + bodyH, lanesW, trH});
  pass.place(*timeCorner_, {r.x + lanesW, bodyY + bodyH, r.w - lanesW, trH});
  Widget& vscroll = child(childCount() - 1);
  pass.place(vscroll, {r.right() - sb, bodyY, sb, scrollH});
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
  if (bottom_->childCount()) bottom_->child(0).markDirty();
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

void ArrangementView::revealTick(Ticks t) {
  const IRect lv = lanesViewport();
  const float x = xOfTick(static_cast<double>(t));
  if (x >= static_cast<float>(lv.x) && x < static_cast<float>(lv.right())) return;
  // Put it a quarter into the view (as Live pages when the marker leaves it).
  setTimeView(static_cast<double>(t) - 0.25 * static_cast<double>(lv.w) / devicePxPerTick(), pxPerTick_);
}

void ArrangementView::revealRow(int32_t row) {
  const auto rs = rows();
  if (row < 0 || static_cast<size_t>(row) >= rs.size()) return;
  const ArrangementRow& r = rs[static_cast<size_t>(row)];
  const int32_t viewH = scroll_->rect().h, y = scroll_->scrollY();
  if (r.y < y) scrollToY(r.y);
  else if (r.y + r.h > y + viewH) scrollToY(r.y + r.h - viewH);
}

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

IRect ArrangementView::laneRect(size_t i) const noexcept {
  if (i >= model_->laneRows.size()) return {};
  const AutomationLaneRow& l = model_->laneRows[i];
  const IRect lr = lanes_->rect();
  return {lr.x, stack_->rect().y + l.y, lr.w, l.h};
}

IRect ArrangementView::pinnedRowRect(size_t i) const noexcept {
  if (i >= model_->bottomRows.size() || bottom_->childCount() == 0) return {};
  const ArrangementRow& row = model_->bottomRows[i];
  const IRect lr = bottom_->child(0).rect();
  return {lr.x, lr.y + row.y, lr.w, row.h};
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
    if (model_->skippedForZoom || model_->tilesAfterZoom) {
      model_->skippedForZoom = false;
      model_->tilesAfterZoom = false;
      lanes_->requestFrame();  // builds the dense clips' previews and the tiles at the new zoom, then repaints once
    }
    return;
  }
  if (id != playheadTimer_) return;
  playheadTimer_ = 0;
  readPlayhead();
}

void ArrangementView::onFrame(double) { automation_->runPending(); }

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
  const IRect bt = bottomRect();
  const int32_t bottomY = bt.empty() ? lv.bottom() : bt.bottom();  // through the pinned returns / master too
  const IRect col{static_cast<int32_t>(x) - 1, ru.y, w + 2, bottomY - ru.y};
  const IRect vis = intersect(intersect(col, {lv.x, ru.y, lv.w, bottomY - ru.y}), rect());
  if (!vis.empty()) tr->damage().add(vis.toRect());
}

// --- install --------------------------------------------------------------------------------------------

ArrangementView& installArrangement(Shell& shell) {
  Widget& slot = shell.slot(ShellSlot::TrackArea);
  auto& view = slot.add<ArrangementView>(shell.context());
  shell.tree().setFocus(&view);
  // Double-click on a clip: the clip view comes up (detail view shown, Clip mode) and takes the keyboard (Live).
  view.onOpenClip = [&shell] {
    ViewContext& ctx = shell.context();
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Clip);
    if (Widget* notes = shell.slot(ShellSlot::ClipDetail).find("clip-view/notes")) {
      shell.tree().layoutNow();
      shell.tree().setFocus(notes);
    }
  };
  return view;
}

}  // namespace pq::views
