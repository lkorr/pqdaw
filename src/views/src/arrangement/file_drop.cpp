// T5.7: files and browser items dropped onto the arrangement's lanes (internal.h FileDrop; the rules in
// pq/views/sample_drop.h).

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

int32_t FileDrop::insertY() const {
  const auto rows = view_.rows();
  if (row_ >= 0 && static_cast<size_t>(row_) < rows.size()) {
    const ArrangementRow& r = rows[static_cast<size_t>(row_)];
    return r.y + r.h + r.lanesH;
  }
  if (rows.empty()) return 0;
  return rows.back().y + rows.back().h + rows.back().lanesH;
}

void FileDrop::retarget(Vec2 pos, Mods mods) {
  const IRect lr = view_.lanes().rect();
  const float local = pos.y - static_cast<float>(lr.y);
  const auto rows = view_.rows();
  row_ = -1;
  for (size_t i = 0; i < rows.size(); ++i)
    if (local >= static_cast<float>(rows[i].y) && local < static_cast<float>(rows[i].y + rows[i].h + rows[i].lanesH)) {
      row_ = static_cast<int32_t>(i);
      break;
    }
  const doc::Project& p = *view_.shown();
  target_ = sampleDropTargetFor(p, row_ >= 0 ? rows[static_cast<size_t>(row_)].id : doc::TrackId{});
  const Ticks t = std::max<Ticks>(0, static_cast<Ticks>(std::llround(view_.tickAtX(pos.x))));
  const Grid g(view_.context().grid(), p.timeline->sigMap, view_.pxPerTick());
  at_ = std::max<Ticks>(0, g.snap(t));
  oneTrack_ = (mods & mod::kCtrl) != 0;
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
  if (!hovering_ || plan_.items.empty()) return {};
  const IRect lr = view_.lanes().rect();
  const auto rows = view_.rows();
  const double k = view_.devicePxPerTick();
  const UiScale s = view_.tree() ? view_.tree()->uiScale() : UiScale{};
  const int32_t newH = s.px(am::kTrackHeight);
  Ticks t0 = INT64_MAX, t1 = INT64_MIN;
  for (const SampleDropPlan::Item& it : plan_.items) {
    t0 = std::min(t0, it.start);
    t1 = std::max(t1, it.start + it.length);
  }
  int32_t y0 = INT32_MAX, y1 = INT32_MIN;
  if (plan_.target.kind == SampleDropTarget::Kind::OnTrack && row_ >= 0) {
    const ArrangementRow& r = rows[static_cast<size_t>(row_)];
    y0 = r.y;
    y1 = r.y + r.h;
  }
  if (plan_.newTracks > 0) {
    const int32_t iy = insertY();
    y0 = std::min(y0, iy - s.px(2.0f));
    y1 = std::max(y1, iy + static_cast<int32_t>(plan_.newTracks) * newH);
  }
  const float x0 = view_.xOfTick(static_cast<double>(t0)) - 2.0f;
  const float x1 = static_cast<float>(view_.xOfTick(static_cast<double>(t1))) + 2.0f;
  (void)k;
  IRect b{static_cast<int32_t>(std::floor(x0)), lr.y + y0, static_cast<int32_t>(std::ceil(x1 - std::floor(x0))), y1 - y0};
  return intersect(b, lr);
}

void FileDrop::damageGhosts() {
  UiTree* tree = view_.lanes().tree();
  if (!tree) return;
  const IRect now = ghostBounds();
  if (now == painted_) return;
  // The lanes clip to the track scroll view: damage only what is visible there.
  const IRect vis = view_.trackScroll().rect();
  if (const IRect o = intersect(painted_, vis); !o.empty()) tree->damage().add(o.toRect());
  if (const IRect n = intersect(now, vis); !n.empty()) tree->damage().add(n.toRect());
  painted_ = now;
}

DropFeedback FileDrop::over(const DragPayload& p, Vec2 pos, Mods mods) {
  if (!takesFiles(p) || p.paths.empty() || pendingCommit_) return {};
  if (!hovering_ || p.paths != paths_) classify(p);
  hovering_ = true;
  retarget(pos, mods);
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

void FileDrop::leave() {
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

DropEffect FileDrop::drop(const DragPayload& p, Vec2 pos, Mods mods) {
  const DropFeedback fb = over(p, pos, mods);
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

void FileDrop::paint(LanePaint& lp) {
  if (!hovering_ || plan_.items.empty()) return;
  const auto rows = view_.rows();
  const Theme& th = lp.th;
  DrawList& dl = lp.ctx.dl;
  const int32_t newH = lp.ctx.scale.px(am::kTrackHeight);
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
  if (plan_.newTracks > 0) {  // the insertion line, then the new tracks' ghosts under it
    const float iy = static_cast<float>(lp.r.y + insertY());
    const float lw = static_cast<float>(std::max(2, lp.ctx.scale.px(2.0f)));
    dl.addRectFilled({static_cast<float>(lp.c.x), iy - lw * 0.5f, static_cast<float>(lp.c.right()), iy + lw * 0.5f}, th.accent.primary);
  }
  for (const SampleDropPlan::Item& it : plan_.items) {
    float top = 0, bottom = 0;
    if (it.newTrack < 0) {
      if (row_ < 0) continue;
      const ArrangementRow& r = rows[static_cast<size_t>(row_)];
      top = static_cast<float>(lp.r.y + r.y + 1);
      bottom = static_cast<float>(lp.r.y + r.y + r.h - 1);
    } else {
      top = static_cast<float>(lp.r.y + insertY() + it.newTrack * newH + 1);
      bottom = top + static_cast<float>(newH - 2);
    }
    if (bottom <= static_cast<float>(lp.c.y) || top >= static_cast<float>(lp.c.bottom())) continue;
    ghost(it, top, bottom);
  }
}

}  // namespace pq::views::arrangement

namespace pq::views {
const SampleDropPlan& ArrangementView::dropPlan() const noexcept { return fileDrop_->plan(); }
bool ArrangementView::dropHovering() const noexcept { return fileDrop_->hovering(); }
}  // namespace pq::views

namespace pq::views::arrangement {

// --- LaneCanvas hooks ------------------------------------------------------------------------------------------

DropFeedback LaneCanvas::dragOver(const DragPayload& p, Vec2 pos, Mods mods) { return view_.fileDrop().over(p, pos, mods); }
void LaneCanvas::dragLeave() { view_.fileDrop().leave(); }
DropEffect LaneCanvas::drop(const DragPayload& p, Vec2 pos, Mods mods) { return view_.fileDrop().drop(p, pos, mods); }
bool LaneCanvas::autoScrollDrag(Vec2 pos, double dt) { return view_.fileDrop().autoScroll(pos, dt); }

}  // namespace pq::views::arrangement
