// SM.8 (docs/plans/SM.md "Decisions", Drop; pq/views/project_patch/project_patch_view.h "audio files dropped on the
// circuit"): audio files dropped on a band's circuit place Sampler groups there (or replace a Sampler's sample), one
// undo step per drop; the analysis's root and slices follow into that step. CPU: a drag move is a band scan and a node
// hit test (the lane drop's cost); the ghost is a few rects in the overlay; nothing runs once the drag is gone.

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "internal.h"
#include "pq/document/main_path.h"
#include "pq/document/modules.h"
#include "pq/document/samples.h"
#include "pq/meta/sampler.h"
#include "pq/ui/theme.h"
#include "pq/views/file_probes.h"
#include "pq/views/sample_analysis.h"
#include "pq/views/sample_drop.h"

namespace pq::views::project_patch {

using ui::Vec2;

namespace {

// The closed Sampler face's size (docs/plans/SM.md "Face": ≈ 150 × 84 units): the ghost of a sampler to come.
constexpr float kGhostW = 150.0f, kGhostH = 84.0f;
// Between two samplers of one drop (world units at zoom 1).
constexpr float kDropGap = 16.0f;

bool isSamplerEngine(const doc::Device& d) { return doc::isMetaDevice(d) && doc::metaDeviceType(d) == "sampler"; }

// The audio files among a payload's paths (by extension: no file access while hovering).
std::vector<std::string> audioPaths(const ui::DragPayload& p) {
  std::vector<std::string> out;
  if (p.kind != ui::DragKind::Files && p.kind != ui::DragKind::BrowserItems) return out;
  for (const std::string& s : p.paths)
    if (fileKindByName(s) == ProbedFile::Kind::Audio) out.push_back(s);
  return out;
}

// The analysis's slice table: a slice at the start and one at each transient (source frames).
std::vector<float> slicesOf(const SampleAnalysisResult& a) {
  std::vector<float> s{0.0f};
  if (a.transients)
    for (const double f : a.transients->onsets)
      if (f > 0.0 && std::isfinite(f)) s.push_back(static_cast<float>(f));
  return s;
}

// SM.8: the analysis arrived for `sample`, dropped into Sampler `sampler` (whose engine then held `data`): its root
// (the detected pitch; unpitched 261.63 Hz) and slices, into the drop's undo step (`key`) while nothing happened in
// between. A sampler edited since (its data no longer the drop's) is left alone.
void applyAnalysis(ViewContext& ctx, doc::DeviceId sampler, doc::DeviceId engine, const meta::DataValue* data,
                   doc::SampleId sample, uint64_t key, const SampleAnalysisResult* a) {
  if (a == nullptr) return;  // unreadable: the drop's 261.63 Hz stays
  const doc::Device* e = doc::findDevice(*ctx.doc(), engine).device;
  const doc::MetaInstance* m = e != nullptr ? e->metaAt("") : nullptr;
  if (m == nullptr || m->data.get() != data || meta::samplerSample(data) != sample.value) return;
  const double root = a->pitch.pitched ? static_cast<double>(a->pitch.hz) : meta::kSamplerDefaultRoot;
  auto r = doc::ops::setSamplerSample(*ctx.doc(), sampler, sample, root, slicesOf(*a));
  if (!r) {
    ctx.setStatus("Sampler: " + r.error().message);
    return;
  }
  if (Status s = ctx.store().dispatchCoalescing(r->command, key); !s) ctx.setStatus("Sampler: " + s.error().message);
}

}  // namespace

doc::DeviceId ProjectPatchView::samplerOf(doc::DeviceId device) const {
  const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), device);
  if (l.device == nullptr) return {};
  if (isSamplerEngine(*l.device)) return l.device->group.valid() ? l.device->group : device;
  if (!doc::isGroupDevice(*l.device)) return {};
  bool found = false;
  doc::forEachDeviceIn(l.track->devices, [&](const doc::Device& d) { found = found || (d.group == device && isSamplerEngine(d)); });
  return found ? device : doc::DeviceId{};
}

// ---- the drag ---------------------------------------------------------------------------------------------------------

ui::DropFeedback ProjectPatchView::sampleDragOver(const ui::DragPayload& p, Vec2 pos) {
  const std::vector<std::string> audio = audioPaths(p);
  SampleDropHover h;
  if (!audio.empty() && !patch_.scoped && !embedded() && !pendingSampleDrop_ && canvas_->rect().contains(pos)) {
    const uint32_t b = canvas_->bandAt(pos);
    const graph::Point2 w = canvas_->toWorld(pos);
    if (b != kNone && patch_.bands[b].track.valid() && !patch_.bands[b].folded &&
        !(patch_.bands[b].laneH > 0.0f && w.y >= patch_.bands[b].laneY)) {  // (its lane and sub-lanes: clips)
      h.on = true;
      h.at = w;
      h.band = b;
      h.files = static_cast<uint32_t>(audio.size());
      if (const uint32_t n = canvas_->nodeAt(pos); n != kNone && patch_.nodes[n].device.valid())
        h.replace = samplerOf(patch_.nodes[n].device);
    }
  }
  const bool changed = h.on != sampleHover_.on || h.replace != sampleHover_.replace || h.band != sampleHover_.band ||
                       h.at.x != sampleHover_.at.x || h.at.y != sampleHover_.at.y;
  sampleHover_ = h;
  if (changed) canvas_->markDirty();
  if (!h.on) return {};
  return {ui::DropEffect::Copy, ui::Cursor::Arrow};
}

void ProjectPatchView::sampleDragLeave() {
  if (!sampleHover_.on) return;
  sampleHover_ = {};
  canvas_->markDirty();
}

ui::DropEffect ProjectPatchView::sampleDropAt(const ui::DragPayload& p, Vec2 pos) {
  if (sampleDragOver(p, pos).effect == ui::DropEffect::None) return ui::DropEffect::None;
  const SampleDropHover h = sampleHover_;
  sampleDragLeave();
  PendingSampleDrop d;
  d.audio = audioPaths(p);
  d.at = h.at;
  d.track = patch_.bands[h.band].track;
  d.replace = h.replace;
  FileProbes& probes = ctx_.fileProbes();
  (void)probes.request(d.audio);
  pendingSampleDrop_ = std::move(d);
  if (!probes.ready(pendingSampleDrop_->audio)) {
    if (ctx_.uiPoster()) {  // commit when the last probe arrives
      probes.setListener([this] {
        if (pendingSampleDrop_ && ctx_.fileProbes().ready(pendingSampleDrop_->audio)) commitSampleDrop();
      });
      ctx_.setStatus("Importing\xE2\x80\xA6");
      return ui::DropEffect::Copy;
    }
    (void)probes.wait(pendingSampleDrop_->audio, std::chrono::seconds(30));  // no UI poster (tests, scenes): wait here
  }
  commitSampleDrop();
  return ui::DropEffect::Copy;
}

void ProjectPatchView::commitSampleDrop() {
  if (!pendingSampleDrop_) return;
  PendingSampleDrop d = std::move(*pendingSampleDrop_);
  pendingSampleDrop_.reset();
  ctx_.fileProbes().setListener(nullptr);
  std::vector<ProbedFile> ok;
  std::string failed;
  for (const ProbedFile& f : ctx_.fileProbes().snapshot(d.audio)) {
    if (f.state == ProbedFile::State::Ready) ok.push_back(f);
    else if (failed.empty()) failed = f.displayName + " (" + (f.error.empty() ? "unreadable" : f.error) + ")";
  }
  if (ok.empty()) {
    ctx_.setStatus("Could not read " + failed);
    return;
  }
  // The band or the sampler may have gone while the probes ran (an undo in between).
  if (d.replace.valid() && !samplerOf(d.replace).valid()) d.replace = {};
  if (!doc::findTrackPtr(*ctx_.doc(), d.track)) {
    ctx_.setStatus("Drop: the track is gone");
    return;
  }
  if (!dropSamplers(ok, d.at, d.track, d.replace).empty() && !failed.empty()) ctx_.setStatus("Could not read " + failed);
}

// ---- the edit -----------------------------------------------------------------------------------------------------------

std::vector<doc::DeviceId> ProjectPatchView::dropSamplers(std::span<const ProbedFile> files, graph::Point2 at,
                                                          doc::TrackId track, doc::DeviceId replace) {
  sync();
  const doc::Snapshot start = ctx_.doc();
  if (files.empty() || doc::findTrackPtr(*start, track) == nullptr) return {};
  Composer k(start, ctx_.store().ids());
  struct Made {
    doc::DeviceId sampler;
    doc::SampleId sample;
    bool placed = false;  // a new block (placed at the drop point); else a replaced sample
  };
  std::vector<Made> made;
  std::vector<std::pair<std::string, doc::SampleId>> seen;  // this drop's files → their table entries
  for (size_t i = 0; i < files.size(); ++i) {
    const ProbedFile& f = files[i];
    if (f.state != ProbedFile::State::Ready) continue;
    // Its table entry: the one already resolving to this file, else a new one (as the lane drop's).
    doc::SampleId sid;
    for (const auto& [path, id] : seen)
      if (path == f.absPath) sid = id;
    for (const doc::SampleResolution& r : ctx_.sampleResolutions())
      if (!sid.valid() && !r.path.empty() && r.path == f.absPath) sid = r.id;
    if (!sid.valid()) {
      doc::SampleRef s;
      s.id = k.ids().sample();
      s.path = doc::storedSamplePath(f.absPath, ctx_.projectDir());
      s.contentHash = f.contentHash;
      s.size = f.size;
      s.frames = f.frames;
      s.rate = f.rate;
      s.channels = f.channels;
      s.displayName = f.displayName;
      if (!k.add(doc::Command{doc::AddSample{s}})) break;
      sid = s.id;
    }
    seen.emplace_back(f.absPath, sid);
    // What the analysis knows already (a sample in the table before): root and slices in the same edit.
    std::optional<double> root;
    std::vector<float> slices;
    if (const SampleAnalysisResult* a = ctx_.sampleAnalysis().get(sid)) {
      root = a->pitch.pitched ? static_cast<double>(a->pitch.hz) : meta::kSamplerDefaultRoot;
      slices = slicesOf(*a);
    }
    if (made.empty() && replace.valid()) {
      if (!k.add(doc::ops::setSamplerSample(k.doc(), replace, sid, root, std::move(slices)))) break;
      made.push_back({replace, sid, false});
      continue;
    }
    doc::IdSource local;
    doc::SamplerOptions o;
    o.name = f.displayName;
    o.sample = sid.value;
    o.slices = std::move(slices);
    o.rootHz = root.value_or(meta::kSamplerDefaultRoot);
    doc::ops::EditResult add = doc::ops::pasteBlocksAtNotes(k.doc(), k.ids(), track, doc::samplerBlocks(local, o));
    if (!add || add->devices.empty()) {
      k.why = add ? std::string("nothing was added") : add.error().message;
      break;
    }
    const doc::DeviceId id = add->devices.front();
    if (!k.add(add)) break;
    made.push_back({id, sid, true});
  }
  if (made.empty() || !k.why.empty()) {
    ctx_.setStatus("Drop Sampler: " + (k.why.empty() ? std::string("nothing to place") : k.why));
    return {};
  }
  const bool replaced = !made.front().placed;
  const char* label = replaced && made.size() == 1 ? "Replace Sample" : made.size() == 1 ? "Drop Sampler" : "Drop Samplers";
  const std::vector<Place> before = places();
  for (const Made& m : made)
    if (m.placed) setSpawnScale(m.sampler);  // its size on screen as at zoom 1 (as a spawn)
  const uint64_t key = newDropKey();
  settleHeld_ = true;
  if (Status s = ctx_.store().dispatchCoalescing(doc::Batch{label, std::move(k.steps)}, key); !s) {
    settleHeld_ = false;
    for (const Made& m : made) ctx_.store().view().patchNudges.erase(m.sampler.value);
    ctx_.setStatus(std::string(label) + ": " + s.error().message);
    return {};
  }
  // Placed: the first centred on the drop point (a replaced sampler stays: the next goes under it), each further one
  // under the previous; the blocks already there keep their places, an overlap goes to the nearest free spot.
  sync();
  (void)keepPlaces(before);
  graph::Point2 next = at;
  bool first = true;
  for (const Made& m : made) {
    sync();
    const uint32_t n = patch_.nodeOf(m.sampler);
    if (n == kNone) continue;
    if (m.placed) {
      if (!first) next.y += patch_.nodes[n].h * 0.5f;
      placeAt(m.sampler, next, "", "", true);
      sync();
    }
    const uint32_t q = patch_.nodeOf(m.sampler);
    if (q != kNone) next = {next.x, patch_.nodes[q].y + patch_.nodes[q].h + kDropGap};
    first = false;
  }
  settleHeld_ = false;
  (void)settleOverlaps();
  // The analysis (now, or when the worker has it): root and slices into this drop's step.
  std::vector<doc::DeviceId> out;
  ViewContext& ctx = ctx_;
  for (const Made& m : made) {
    out.push_back(m.sampler);
    doc::DeviceId engine;
    if (const doc::DeviceLoc l = doc::findDevice(*ctx.doc(), m.sampler); l.track != nullptr)
      doc::forEachDeviceIn(l.track->devices, [&](const doc::Device& d) {
        if (!engine.valid() && d.group == m.sampler && isSamplerEngine(d)) engine = d.id;
      });
    const doc::Device* e = engine.valid() ? doc::findDevice(*ctx.doc(), engine).device : nullptr;
    const doc::MetaInstance* mi = e != nullptr ? e->metaAt("") : nullptr;
    if (mi == nullptr) continue;
    const meta::DataPtr data = mi->data;  // kept alive by the callback: identity, compared on arrival
    const doc::DeviceId sampler = m.sampler;
    const doc::SampleId sample = m.sample;
    ctx.sampleAnalysis().whenAnalysed(sample, [&ctx, sampler, engine, data, sample, key](const SampleAnalysisResult* a) {
      applyAnalysis(ctx, sampler, engine, data.get(), sample, key, a);
    });
  }
  ctx.setStatus(made.size() == 1 ? (replaced ? "Sample replaced" : "Sampler placed")
                                 : std::to_string(made.size()) + " samplers placed");
  return out;
}

// ---- the ghost ----------------------------------------------------------------------------------------------------------

void ProjectPatchView::paintSampleDrop(ui::PaintContext& ctx) {
  if (!sampleHover_.on || ctx.res.theme == nullptr) return;
  const ui::Theme& th = *ctx.res.theme;
  ui::DrawList& dl = ctx.dl;
  const float us = ctx.scale.factor;
  const float lw = std::max(1.0f, std::round(2.0f * us));
  if (sampleHover_.replace.valid()) {  // the Sampler whose sample goes: its outline in the accent
    const uint32_t n = patch_.nodeOf(sampleHover_.replace);
    if (n == kNone) return;
    const Node& nd = patch_.nodes[n];
    const ui::Rect r = snapRect(canvas_->deviceRect(nd.x, nd.y, nd.w, nd.h));
    const float pad = 3.0f * us;
    dl.addRoundedRectStroke({r.x0 - pad, r.y0 - pad, r.x1 + pad, r.y1 + pad}, 6.0f * us, th.accent.primary, lw);
    return;
  }
  // A sampler to come per file (at most 8 drawn), the first centred on the drop point, each under the previous: a
  // rounded plate in the accent (as the lane drop's ghost clips).
  const float s = spawnScale();
  const float w = kGhostW * s, h = kGhostH * s;
  const uint32_t count = std::min<uint32_t>(sampleHover_.files, 8);
  for (uint32_t i = 0; i < count; ++i) {
    const float y = sampleHover_.at.y - h * 0.5f + static_cast<float>(i) * (h + kDropGap * s);
    const ui::Rect r = snapRect(canvas_->deviceRect(sampleHover_.at.x - w * 0.5f, y, w, h));
    const float rad = std::min(r.x1 - r.x0, r.y1 - r.y0) * 0.5f;
    dl.addRoundedRectFilled(r, rad, ui::withAlpha(th.accent.primary, 70));
    dl.addRoundedRectStroke(r, rad, th.accent.primary, lw);
  }
}

}  // namespace pq::views::project_patch
