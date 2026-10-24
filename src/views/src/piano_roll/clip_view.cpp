// The clip view widget (T3.11): builds its parts, lays them out, follows the document and the context, and installs
// the pianoroll actions.

#include "pq/views/piano_roll/clip_view.h"

#include <algorithm>
#include <cmath>

#include "pq/ui/theme.h"
#include "roll_widgets.h"

namespace pq::views {

using namespace pq::ui;
using roll::RollState;

ClipView::ClipView(ViewContext& ctx) : Widget("clip-view"), ctx_(ctx), state_(std::make_unique<RollState>(ctx)) {
  setLayout(LayoutKind::Custom);
  setClipChildren(true);
  setKeyScope(ctx_.scope("pianoroll"));
  RollState& s = *state_;
  s.boxPanel = &add<roll::ClipBoxPanel>(s);
  s.cornerPanel = &add<roll::CornerPanel>(s);
  s.rulerView = &add<roll::RulerWidget>(s);
  s.keysView = &add<roll::KeysWidget>(s);
  s.noteArea = &add<roll::NoteAreaWidget>(s);
  s.laneHeaderView = &add<roll::LaneHeaderWidget>(s);
  s.laneView = &add<roll::VelocityLaneWidget>(s);

  auto bind = [this](std::string_view id, auto fn) {
    ctx_.setViewAction(id, [this, fn] { fn(*this); }, [this] { return hasClip(); });
  };
  bind("pianoroll.selectAll", [](ClipView& v) { v.selectAll(); });
  bind("pianoroll.copy", [](ClipView& v) { v.copySelection(); });
  bind("pianoroll.cut", [](ClipView& v) { v.cutSelection(); });
  bind("pianoroll.paste", [](ClipView& v) { v.paste(); });
  bind("pianoroll.duplicate", [](ClipView& v) { v.duplicateSelection(); });
  bind("pianoroll.delete", [](ClipView& v) { v.deleteSelection(); });
  bind("pianoroll.quantize", [](ClipView& v) { v.quantizeSelection(); });
  bind("pianoroll.stepUp", [](ClipView& v) { v.stepSelection(1, doc::ops::StepUnit::Grid); });
  bind("pianoroll.stepDown", [](ClipView& v) { v.stepSelection(-1, doc::ops::StepUnit::Grid); });
  bind("pianoroll.equaveUp", [](ClipView& v) { v.stepSelection(1, doc::ops::StepUnit::Equave); });
  bind("pianoroll.equaveDown", [](ClipView& v) { v.stepSelection(-1, doc::ops::StepUnit::Equave); });
  bind("pianoroll.toggleActive", [](ClipView& v) { v.toggleSelectionActive(); });
  bind("pianoroll.fold", [](ClipView& v) { v.setFold(!v.fold()); });
  bind("pianoroll.toggleScale", [](ClipView& v) { v.setScaleHighlight(!v.scaleHighlight()); });
  bind("pianoroll.nudgeLeft", [](ClipView& v) { v.nudgeSelection(-1); });  // T3.x-fix-look: arrow keys (Live)
  bind("pianoroll.nudgeRight", [](ClipView& v) { v.nudgeSelection(1); });

  docSub_ = ctx_.subscribe([this](const doc::StoreEvent&) { onDocument(); });
  viewSub_ = ctx_.observe([this](uint32_t bits) { onView(bits); });
  (void)s.refresh();
  setVisible(s.hasClip());
  s.markAllDirty();
}

ClipView::~ClipView() {
  if (state_->morphingGenerator() && ctx_.tuningPreview()) ctx_.clearTuningPreview();  // a morph cut short: no trace
  for (std::string_view id : ViewContext::kPianoRollActions) ctx_.clearViewAction(id);
}

void ClipView::onDocument() {
  RollState& s = *state_;
  if (s.refresh()) {
    if (s.pitchOnlyChange()) {  // a live morph frame (T3.12): rows and notes move, nothing else
      if (s.keysView) s.keysView->markDirty();
      if (s.noteArea) s.noteArea->markDirty();
      if (s.boxPanel) s.boxPanel->refresh();
      return;
    }
    setVisible(s.hasClip());
    s.markAllDirty();
  }
}

void ClipView::onView(uint32_t bits) {
  RollState& s = *state_;
  if (bits & view_change::kDetailClip) {
    (void)s.refresh();
    setVisible(s.hasClip());
    invalidateArrange();  // fits the new clip once laid out
    s.markAllDirty();
  }
  if (bits & view_change::kGrid) {
    if (s.rulerView) s.rulerView->markDirty();
    s.markNotesDirty();
  }
  if ((bits & view_change::kTool) && s.cornerPanel) s.cornerPanel->markDirty();
  if (bits & view_change::kTuningPreview) onDocument();  // the MOS wheel's live tuning (T3.14)
}

void ClipView::paint(PaintContext& ctx) {
  if (const Theme* th = ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), th->surface.background);
}

void ClipView::arrange(LayoutPass& pass) {
  const IRect r = rect();
  const int32_t gap = 1;
  const int32_t boxW = std::min(pass.px(roll::kClipBoxWidth), r.w / 3);
  const int32_t keysW = pass.px(roll::kKeysWidth);
  const int32_t rulerH = pass.px(roll::kRulerHeight);
  // The velocity lane gives way first when the detail view is short.
  int32_t laneH = pass.px(roll::kLaneHeight);
  if (r.h - rulerH - laneH - 2 * gap < pass.px(60)) laneH = std::max(0, r.h - rulerH - 2 * gap - pass.px(60));
  const int32_t ex = r.x + boxW + gap;
  const int32_t ew = std::max(0, r.right() - ex);
  const int32_t gx = ex + keysW + gap;
  const int32_t gw = std::max(0, r.right() - gx);
  const int32_t notesY = r.y + rulerH + gap;
  const int32_t notesH = std::max(0, r.bottom() - notesY - (laneH > 0 ? laneH + gap : 0));
  const int32_t laneY = notesY + notesH + gap;
  RollState& s = *state_;
  pass.place(*s.boxPanel, {r.x, r.y, boxW, r.h});
  pass.place(*s.cornerPanel, {ex, r.y, std::min(keysW, ew), rulerH});
  pass.place(*s.rulerView, {gx, r.y, gw, rulerH});
  pass.place(*s.keysView, {ex, notesY, std::min(keysW, ew), notesH});
  pass.place(*s.noteArea, {gx, notesY, gw, notesH});
  s.laneHeaderView->setVisible(laneH > 0);
  s.laneView->setVisible(laneH > 0);
  if (laneH > 0) {
    pass.place(*s.laneHeaderView, {ex, laneY, std::min(keysW, ew), laneH});
    pass.place(*s.laneView, {gx, laneY, gw, laneH});
  }
  s.afterLayout();
}

// --- state ------------------------------------------------------------------------------------------------

bool ClipView::hasClip() const noexcept { return state_->hasClip(); }
const doc::ClipRef& ClipView::clip() const noexcept { return state_->clipRef(); }
const roll::RowModel& ClipView::rows() const noexcept { return state_->rows(); }
std::span<const uint32_t> ClipView::selection() const noexcept { return state_->selection(); }

void ClipView::select(std::span<const doc::NoteId> ids) {
  std::vector<uint32_t> v;
  v.reserve(ids.size());
  for (doc::NoteId id : ids) v.push_back(id.value);
  state_->select(std::move(v));
}

Grid ClipView::grid() const noexcept { return state_->grid(); }
std::optional<tuning::Rational> ClipView::virtualFundamental() const { return state_->virtualFundamental(); }
double ClipView::pxPerTick() const noexcept { return state_->pxPerTick(); }
double ClipView::scrollTick() const noexcept { return state_->scrollTick(); }

void ClipView::setZoom(double devicePxPerTick, double anchorTick) {
  state_->setZoom(devicePxPerTick, anchorTick, state_->xOfTick(anchorTick));
}

void ClipView::scrollToTick(double t) { state_->scrollToTick(t); }
int32_t ClipView::rowHeight() const noexcept { return state_->rowPx(); }
void ClipView::setRowHeight(int32_t px) { state_->setRowHeight(px); }
void ClipView::centreOnDegree(int64_t d) { state_->centreOnDegree(d); }
void ClipView::fitToClip() { state_->fitToClip(); }
bool ClipView::fold() const noexcept { return state_->fold(); }
void ClipView::setFold(bool on) { state_->setFold(on); }
bool ClipView::scaleHighlight() const noexcept { return state_->scaleHighlight(); }
void ClipView::setScaleHighlight(bool on) { state_->setScaleHighlight(on); }
bool ClipView::showChance() const noexcept { return state_->showChance(); }
void ClipView::setShowChance(bool on) { state_->setShowChance(on); }
roll::RowLayout ClipView::scaleLayout() const noexcept { return state_->scaleLayout(); }
void ClipView::setScaleLayout(roll::RowLayout layout) { state_->setScaleLayout(layout); }
uint32_t ClipView::overlayEdo() const noexcept { return state_->overlayEdo(); }
void ClipView::setOverlayEdo(uint32_t n) { state_->setOverlayEdo(n); }
void ClipView::beginGeneratorMorph() { state_->beginGeneratorMorph(); }
void ClipView::morphGenerator(double g) { state_->morphGenerator(g); }
void ClipView::endGeneratorMorph(bool commit) { state_->endGeneratorMorph(commit); }
uint64_t ClipView::morphLinearFrames() const noexcept { return state_->morphStats().linear; }
uint64_t ClipView::morphRebuilds() const noexcept { return state_->morphStats().rebuilt; }

float ClipView::xOfTick(double t) const noexcept { return state_->xOfTick(t); }

float ClipView::yOfDegree(int64_t d) const noexcept {
  const roll::PitchRow* row = state_->rows().rowOfDegree(d);
  return row ? static_cast<float>(state_->screenY(row->centre)) : -1.0f;
}

IRect ClipView::noteRect(doc::NoteId id) const {
  const RollState& s = *state_;
  const doc::Note* n = s.findNote(id);
  if (!n) return {};
  const roll::NotePlace pl = s.place(n->pitch);
  if (!pl.ok) return {};
  const float x0 = std::round(s.xOfTick(static_cast<double>(n->start)));
  const float x1 = std::max(x0 + 3.0f, std::round(s.xOfTick(static_cast<double>(n->start + n->length))));
  const IRect r{static_cast<int32_t>(x0), s.screenY(pl.top), static_cast<int32_t>(x1 - x0), pl.bottom - pl.top};
  return intersect(r, s.area());
}

Vec2 ClipView::stemHead(doc::NoteId id) const {
  const RollState& s = *state_;
  const doc::Note* n = s.findNote(id);
  if (!n || !s.laneView) return {-1.0f, -1.0f};
  return {std::round(s.xOfTick(static_cast<double>(n->start))), s.laneView->yOfValue(s.previewValue(*n))};
}

void ClipView::stepSelection(int32_t steps, doc::ops::StepUnit unit) { state_->stepSelection(steps, unit); }
void ClipView::nudgeSelection(int32_t cells) { state_->nudgeSelection(cells); }
bool ClipView::showHarmonics() const noexcept { return state_->showHarmonics(); }
void ClipView::setShowHarmonics(bool on) { state_->setShowHarmonics(on); }
void ClipView::deleteSelection() { state_->deleteSelection(); }
void ClipView::duplicateSelection() { state_->duplicateSelection(); }
void ClipView::quantizeSelection() { state_->quantizeSelection(); }
void ClipView::selectAll() { state_->selectAll(); }
void ClipView::toggleSelectionActive() { state_->toggleSelectionActive(); }
void ClipView::copySelection() { state_->copySelection(); }
void ClipView::cutSelection() { state_->cutSelection(); }
void ClipView::paste() { state_->paste(); }

ClipView& installClipView(Shell& shell) {
  return shell.slot(ShellSlot::ClipDetail).add<ClipView>(shell.context());
}

}  // namespace pq::views
