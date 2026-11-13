// T5.7: files and browser items dropped onto clip lanes (internal.h FileDrop; the rules in pq/views/sample_drop.h).
// TX.lane-drop: one drop path for every EditSurface — the arrangement's lanes and the project patch view's band lanes
// hand their drag hooks here with themselves; only the pointer → track / tick mapping and where new tracks show are the
// surface's (EditSurface::dropSpotAt, axis, dropInsert).

#include <algorithm>
#include <cmath>
#include <string>

#include "internal.h"
#include "lane_paint.h"
#include "pq/views/browser/library.h"
#include "pq/views/file_probes.h"

namespace pq::views::arrangement {

using namespace pq::ui;
namespace am = arrangement_metrics;

namespace {

bool takesFiles(const DragPayload& p) { return p.kind == ui::DragKind::Files || p.kind == ui::DragKind::BrowserItems; }

std::string plural(uint32_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

FileDrop::FileDrop(ArrangementView& view) : view_(view) {}

FileDrop::~FileDrop() {
  if (hovering_ || pendingCommit_) view_.context().fileProbes().setListener(nullptr);
}

void FileDrop::classify(const DragPayload& p) {
  paths_ = p.paths;
  audio_.clear();
  folders_.clear();
  midi_ = presets_ = others_ = 0;
  FileProbes& probes = view_.context().fileProbes();
  probes.clear();  // a new session: files may have changed since the last one
  const std::vector<ProbedFile> all = probes.request(paths_);  // starts probing + preloading the audio files
  for (const ProbedFile& f : all) {
    switch (f.kind) {
      case ProbedFile::Kind::Audio: audio_.push_back(f.path); break;
      case ProbedFile::Kind::Folder: folders_.push_back(f.path); break;
      case ProbedFile::Kind::Midi: ++midi_; break;
      case ProbedFile::Kind::Preset: ++presets_; break;
      default: ++others_; break;
    }
  }
  probes.setListener([this] { probesArrived(); });
  // What cannot be dropped here says so in the status line (once per session).
  std::string why;
  if (midi_) why = plural(midi_, "MIDI file", "MIDI files") + " skipped: MIDI file import is not available yet";
  else if (presets_) why = plural(presets_, "preset or device file", "preset or device files") + " skipped: drop presets onto a device chain";
  else if (others_ && audio_.empty() && folders_.empty()) why = "Only audio files and folders can be dropped here";
  if (!why.empty()) view_.context().setStatus(why);
}

EditSurface* FileDrop::live() const noexcept {
  if (surf_ == nullptr) return nullptr;
  for (EditSurface* e : view_.surfaces().all())
    if (e == surf_) return surf_;
  return nullptr;
}

const EditSurface* FileDrop::surface() const noexcept { return live(); }

bool FileDrop::retarget(EditSurface& s, Vec2 pos, Mods mods) {
  const std::optional<EditSurface::DropSpot> spot = s.dropSpotAt(pos);
  if (!spot) return false;
  spot_ = *spot;
  const doc::Project& p = *view_.shown();
  target_ = sampleDropTargetFor(p, spot_.track);
  const Ticks t = std::max<Ticks>(0, static_cast<Ticks>(std::llround(s.axis(std::max(0, spot_.row)).tick(pos.x))));
  const Grid g(view_.context().grid(), p.timeline->sigMap, s.gridPxPerTick());
  at_ = std::max<Ticks>(0, g.snap(t));
  oneTrack_ = (mods & mod::kCtrl) != 0;
  return true;
}

void FileDrop::replan() {
  probed_ = view_.context().fileProbes().snapshot(audio_);
  // Files whose probe failed are left out (the status line names them at the drop).
  std::vector<ProbedFile> usable;
  usable.reserve(probed_.size());
  for (const ProbedFile& f : probed_)
    if (f.state != ProbedFile::State::Failed) usable.push_back(f);
  probed_ = std::move(usable);
  plan_ = planSampleDrop(*view_.shown(), target_, probed_, at_, oneTrack_);
}

IRect FileDrop::ghostBounds() const {
  const EditSurface* s = live();
  if (s == nullptr || !hovering_ || plan_.items.empty()) return {};
  const UiScale us = view_.tree() ? view_.tree()->uiScale() : UiScale{};
  Ticks t0 = INT64_MAX, t1 = INT64_MIN;
  for (const SampleDropPlan::Item& it : plan_.items) {
    t0 = std::min(t0, it.start);
    t1 = std::max(t1, it.start + it.length);
  }
  int32_t y0 = INT32_MAX, y1 = INT32_MIN;
  if (plan_.target.kind == SampleDropTarget::Kind::OnTrack && spot_.row >= 0) {
    const IRect rr = s->rowLaneRect(spot_.row);
    y0 = rr.y;
    y1 = rr.bottom();
  }
  if (plan_.newTracks > 0) {
    const EditSurface::DropInsert ins = s->dropInsert(spot_);
    y0 = std::min(y0, ins.y - us.px(2.0f));
    y1 = std::max(y1, ins.y + static_cast<int32_t>(plan_.newTracks) * ins.h);
  }
  if (y1 <= y0) return {};
  const TimeAxis ax = s->axis(std::max(0, spot_.row));
  const float x0 = ax.x(static_cast<double>(t0)) - 2.0f;
  const float x1 = ax.x(static_cast<double>(t1)) + 2.0f;
  const IRect b{static_cast<int32_t>(std::floor(x0)), y0, static_cast<int32_t>(std::ceil(x1 - std::floor(x0))), y1 - y0};
  return intersect(b, s->viewport());
}

void FileDrop::damageGhosts() {
  const EditSurface* s = live();
  UiTree* tree = s != nullptr ? s->widget().tree() : nullptr;
  if (!tree) return;
  const IRect now = ghostBounds();
  if (now == painted_) return;
  // Only what the surface shows (ghostBounds is clipped to its viewport).
  if (!painted_.empty()) tree->damage().add(painted_.toRect());
  if (!now.empty()) tree->damage().add(now.toRect());
  painted_ = now;
}

DropFeedback FileDrop::over(EditSurface& s, const DragPayload& p, Vec2 pos, Mods mods) {
  if (!takesFiles(p) || p.paths.empty() || pendingCommit_) return {};
  if (surf_ != &s) {  // the drag moved to another surface: the old one's ghosts go (the session's files stay classified)
    const bool h = hovering_;
    hovering_ = false;
    damageGhosts();
    hovering_ = h;
    surf_ = &s;
    painted_ = {};
  }
  if (!retarget(s, pos, mods)) {  // no drop here on this surface (the patch canvas outside its lanes)
    if (hovering_) leave(s);
    return {};
  }
  if (!hovering_ || p.paths != paths_) classify(p);
  hovering_ = true;
  if (audio_.empty()) {
    damageGhosts();
    // Folders become browser places; nothing else drops here.
    return folders_.empty() ? DropFeedback{} : DropFeedback{DropEffect::Link, Cursor::Arrow};
  }
  if (target_.kind == SampleDropTarget::Kind::Refused) {
    plan_ = {};
    damageGhosts();
    return {DropEffect::None, Cursor::NotAllowed};
  }
  replan();
  damageGhosts();
  return {DropEffect::Copy, Cursor::Arrow};
}

void FileDrop::leave(const EditSurface& s) {
  if (&s != surf_) return;  // the session is another surface's now
  hovering_ = false;
  damageGhosts();
  plan_ = {};
  if (!pendingCommit_) view_.context().fileProbes().setListener(nullptr);
}

void FileDrop::probesArrived() {
  if (pendingCommit_) {
    if (view_.context().fileProbes().ready(audio_)) commit();
    return;
  }
  if (!hovering_) return;
  replan();
  damageGhosts();
}

DropEffect FileDrop::drop(EditSurface& s, const DragPayload& p, Vec2 pos, Mods mods) {
  const DropFeedback fb = over(s, p, pos, mods);
  hovering_ = false;
  damageGhosts();
  ViewContext& ctx = view_.context();
  if (fb.effect == DropEffect::None) {
    ctx.fileProbes().setListener(nullptr);
    return DropEffect::None;
  }
  if (!folders_.empty()) {  // folders offered to the browser as places
    uint32_t added = 0;
    for (const std::string& f : folders_) added += ctx.library().addPlace(f) ? 1u : 0u;
    ctx.setBrowserVisible(true);
    if (audio_.empty()) {
      ctx.setStatus(added ? plural(added, "folder", "folders") + " added to the browser's Places"
                          : "Already in the browser's Places");
      ctx.fileProbes().setListener(nullptr);
      return DropEffect::Link;
    }
  }
  FileProbes& probes = ctx.fileProbes();
  if (!probes.ready(audio_)) {
    if (ctx.uiPoster()) {  // commit when the last probe arrives (probesArrived)
      pendingCommit_ = true;
      ctx.setStatus("Importing " + plural(static_cast<uint32_t>(audio_.size()), "file", "files") + "\xE2\x80\xA6");
      return DropEffect::Copy;
    }
    (void)probes.wait(audio_, std::chrono::seconds(30));  // no UI poster (tests, scenes): wait here
  }
  commit();
  return DropEffect::Copy;
}

void FileDrop::commit() {
  pendingCommit_ = false;
  ViewContext& ctx = view_.context();
  ctx.fileProbes().setListener(nullptr);
  const std::vector<ProbedFile> all = ctx.fileProbes().snapshot(audio_);
  std::vector<ProbedFile> ok;
  std::string failed;
  uint32_t nFailed = 0;
  for (const ProbedFile& f : all) {
    if (f.state == ProbedFile::State::Ready) {
      ok.push_back(f);
    } else {
      if (nFailed++ == 0) failed = f.displayName + " (" + (f.error.empty() ? "unreadable" : f.error) + ")";
    }
  }
  if (target_.kind == SampleDropTarget::Kind::Refused || ok.empty()) {
    if (nFailed) ctx.setStatus("Could not read " + failed + (nFailed > 1 ? " and " + plural(nFailed - 1, "more file", "more files") : ""));
    return;
  }
  // The target may have gone while the probes ran (an undo in between): fall back to new tracks at the end.
  SampleDropTarget t = target_;
  if (t.kind != SampleDropTarget::Kind::AtEnd && !doc::findTrackPtr(*ctx.doc(), t.track)) t = {SampleDropTarget::Kind::AtEnd, {}};
  auto r = dropSamples(ctx, t, ok, at_, oneTrack_);
  if (!r) {
    ctx.setStatus("Drop failed: " + r.error().message);
    return;
  }
  view_.selectClips(*r, true);
  std::string msg = plural(static_cast<uint32_t>(ok.size()), "sample", "samples") + " added";
  if (nFailed) msg += "; could not read " + failed + (nFailed > 1 ? " and " + plural(nFailed - 1, "more", "more") : "");
  if (midi_) msg += "; " + plural(midi_, "MIDI file", "MIDI files") + " skipped (MIDI import is not available yet)";
  ctx.setStatus(msg);
}

bool FileDrop::autoScroll(Vec2 pos, double dt) {
  const IRect lv = view_.lanesViewport();
  if (lv.empty() || pos.y < static_cast<float>(lv.y) || pos.y >= static_cast<float>(lv.bottom())) return false;
  const float s = view_.tree() ? view_.tree()->scale() : 1.0f;
  const float zone = ScrollView::kDragScrollZone * s;
  float v = 0;
  if (pos.x < static_cast<float>(lv.x) + zone && view_.originTick() > 0.0)
    v = -ScrollView::kDragScrollSpeed * s * std::clamp(1.0f - (pos.x - static_cast<float>(lv.x)) / zone, 0.0f, 1.0f);
  else if (pos.x > static_cast<float>(lv.right()) - zone)
    v = ScrollView::kDragScrollSpeed * s * std::clamp(1.0f - (static_cast<float>(lv.right()) - pos.x) / zone, 0.0f, 1.0f);
  if (v == 0) return false;
  if (dt <= 0.0) return true;
  autoScrollAcc_ += static_cast<float>(v * dt);
  const float px = std::trunc(autoScrollAcc_);
  autoScrollAcc_ -= px;
  if (px != 0) view_.scrollByPx(static_cast<double>(px));
  return true;
}

void FileDrop::paint(LanePaint& lp, const EditSurface& s) {
  if (&s != live() || !hovering_ || plan_.items.empty()) return;
  const Theme& th = lp.th;
  DrawList& dl = lp.ctx.dl;
  const Rgba fill = withAlpha(th.accent.primary, 150);
  const Rgba pendingFill = withAlpha(th.accent.primary, 70);
  auto ghost = [&](const SampleDropPlan::Item& it, float top, float bottom) {
    ClipCols cc;
    if (!clipColumns(lp, it.start, it.length, cc)) return;
    const float titleB = paintClipChrome(lp, cc, top, bottom, it.pending ? pendingFill : fill,
                                         probed_[it.file].displayName);
    (void)titleB;
    dl.addRectStroke({cc.cx0, top, cc.cx1, bottom}, th.clip.selectedBorder);
  };
  const EditSurface::DropInsert ins = plan_.newTracks > 0 ? s.dropInsert(spot_) : EditSurface::DropInsert{};
  if (plan_.newTracks > 0) {  // the insertion line, then the new tracks' ghosts under it
    const auto iy = static_cast<float>(ins.y);
    const float lw = static_cast<float>(std::max(2, lp.ctx.scale.px(2.0f)));
    dl.addRectFilled({static_cast<float>(lp.c.x), iy - lw * 0.5f, static_cast<float>(lp.c.right()), iy + lw * 0.5f}, th.accent.primary);
  }
  for (const SampleDropPlan::Item& it : plan_.items) {
    float top = 0, bottom = 0;
    if (it.newTrack < 0) {
      if (spot_.row < 0) continue;
      const IRect rr = s.rowLaneRect(spot_.row);
      top = static_cast<float>(rr.y + 1);
      bottom = static_cast<float>(rr.bottom() - 1);
    } else {
      top = static_cast<float>(ins.y + it.newTrack * ins.h + 1);
      bottom = top + static_cast<float>(ins.h - 2);
    }
    if (bottom <= static_cast<float>(lp.c.y) || top >= static_cast<float>(lp.c.bottom())) continue;
    ghost(it, top, bottom);
  }
}

// --- the arrangement's lanes as a drop surface -----------------------------------------------------------------

std::optional<EditSurface::DropSpot> ArrangementSurface::dropSpotAt(Vec2 pos) const {
  const float local = pos.y - static_cast<float>(view_.lanes().rect().y);
  const auto rows = view_.rows();
  for (size_t i = 0; i < rows.size(); ++i)
    if (local >= static_cast<float>(rows[i].y) && local < static_cast<float>(rows[i].y + rows[i].h + rows[i].lanesH))
      return DropSpot{rows[i].id, static_cast<int32_t>(i), static_cast<int32_t>(i)};
  return DropSpot{};  // below the last track: new tracks at the end
}

// Under the spot's row and its automation lanes (below the last track: after it).
EditSurface::DropInsert ArrangementSurface::dropInsert(const DropSpot& s) const {
  const auto rows = view_.rows();
  const UiScale us = view_.tree() ? view_.tree()->uiScale() : UiScale{};
  int32_t y = 0;
  if (s.spot >= 0 && static_cast<size_t>(s.spot) < rows.size()) {
    const ArrangementRow& r = rows[static_cast<size_t>(s.spot)];
    y = r.y + r.h + r.lanesH;
  } else if (!rows.empty()) {
    y = rows.back().y + rows.back().h + rows.back().lanesH;
  }
  return {view_.lanes().rect().y + y, us.px(am::kTrackHeight)};
}

}  // namespace pq::views::arrangement

namespace pq::views {
const SampleDropPlan& ArrangementView::dropPlan() const noexcept { return fileDrop_->plan(); }
bool ArrangementView::dropHovering() const noexcept { return fileDrop_->hovering(); }
}  // namespace pq::views

namespace pq::views::arrangement {

// --- LaneCanvas hooks ------------------------------------------------------------------------------------------

DropFeedback LaneCanvas::dragOver(const DragPayload& p, Vec2 pos, Mods mods) {
  return view_.fileDrop().over(view_.surfaces().own(), p, pos, mods);
}
void LaneCanvas::dragLeave() { view_.fileDrop().leave(view_.surfaces().own()); }
DropEffect LaneCanvas::drop(const DragPayload& p, Vec2 pos, Mods mods) {
  return view_.fileDrop().drop(view_.surfaces().own(), p, pos, mods);
}
bool LaneCanvas::autoScrollDrag(Vec2 pos, double dt) { return view_.fileDrop().autoScroll(pos, dt); }

}  // namespace pq::views::arrangement
