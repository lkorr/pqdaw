// The arrangement's model caches (T3.8): per-track clip indices and per-clip note previews.

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "internal.h"
#include "pq/document/audio_clip.h"
#include "pq/document/automation_curve.h"
#include "pq/document/note_bend.h"
#include "pq/document/pitch_resolve.h"

namespace pq::views::arrangement {

// --- ClipIndex ------------------------------------------------------------------------------------------

std::pair<size_t, size_t> ClipIndex::visible(Ticks from, Ticks to) const noexcept {
  // First clip whose (running max) end is past `from`, then every clip starting before `to`.
  const auto first = std::upper_bound(maxEnd.begin(), maxEnd.end(), from);
  const auto last = std::lower_bound(start.begin(), start.end(), to);
  const size_t a = static_cast<size_t>(first - maxEnd.begin());
  const size_t b = static_cast<size_t>(last - start.begin());
  return {a, std::max(a, b)};
}

namespace {

void buildIndex(ClipIndex& ix, const void* node, const std::vector<doc::ClipSlot>& clips) {
  ix.node = node;
  const size_t n = clips.size();
  ix.start.resize(n);
  ix.end.resize(n);
  ix.maxEnd.resize(n);
  Ticks m = INT64_MIN;
  for (size_t i = 0; i < n; ++i) {
    const doc::ClipSlot& c = clips[i];  // T5.3: audio clips too
    ix.start[i] = c.start();
    ix.end[i] = c.end();
    m = std::max(m, ix.end[i]);
    ix.maxEnd[i] = m;
  }
}

}  // namespace

const ClipIndex& Model::clips(const doc::Track& t) {
  ClipIndex& ix = clipIndex_[t.id.value];
  if (ix.node != &t) buildIndex(ix, &t, t.clips);
  return ix;
}

const ClipIndex& Model::clips(const doc::TakeLane& l) {
  ClipIndex& ix = takeIndex_[l.id.value];
  if (ix.node != &l) buildIndex(ix, &l, l.clips);
  return ix;
}

// --- previews -------------------------------------------------------------------------------------------

const tuning::Tuning* Model::tuningFor(const doc::TuningDef* def) {
  if (!def) return nullptr;
  auto it = tunings_.find(def);
  if (it == tunings_.end()) {
    auto t = doc::buildTuning(*def);
    it = tunings_.emplace(def, t ? std::optional<tuning::Tuning>(std::move(*t)) : std::nullopt).first;
  }
  return it->second ? &*it->second : nullptr;
}

namespace {

// Pitch height in cents (any fixed origin): the identity resolved in the clip's tuning. Identities that need a
// context the preview does not have (a Ratio anchored to another note, a Harmonic voice) fall back to their ratio
// over the reference, or the reference itself.
double pitchCents(const tuning::PitchId& p, const tuning::Tuning* t, double refHz) {
  if (t) {
    if (auto hz = tuning::resolveHzDouble(p, *t, {}); hz && *hz > 0.0) return 1200.0 * std::log2(*hz);
  }
  switch (p.kind()) {
    case tuning::PitchKind::Ratio:
      return 1200.0 * std::log2(refHz * static_cast<double>(p.rawNum()) / static_cast<double>(p.rawDen()));
    case tuning::PitchKind::Cents: return 1200.0 * std::log2(refHz) + p.cents();
    default: return 1200.0 * std::log2(refHz);
  }
}

}  // namespace

namespace {

const void* timelineOf(const doc::Project* p) noexcept { return p ? static_cast<const void*>(p->timeline.get()) : nullptr; }

// Whether a clip's notes need the clip resolved (pitch_resolve) to be placed and drawn: a Ratio anchored to another
// note, a note whose pitch moves (a curve record), a JI group (its warps).
// What a preview counts as against the cache bound (in notes): its steady notes, and a moving note as one note plus
// a note per four points of its curves.
size_t previewWeight(const Preview& pv) noexcept {
  size_t w = pv.notes.size();
  for (const PreviewPath& path : pv.paths) w += 1 + (path.pts.size() + path.rest.size()) / 4;
  return w;
}

bool needsResolve(const doc::NoteClip& c) noexcept {
  if (c.curves || c.groups) return true;
  for (const doc::Note& n : c.notes)
    if (doc::noteAnchorOf(n.pitch).valid()) return true;
  return false;
}

}  // namespace

Preview* Model::cachedPreview(const doc::ClipPtr& clip, const doc::TuningDef* tuningDef, const doc::Project* p) noexcept {
  auto found = previews_.find(clip.get());
  if (found == previews_.end() || found->second.clip != clip || found->second.tuning != tuningDef ||
      found->second.timeline != timelineOf(p))
    return nullptr;
  found->second.lastUsed = paintSerial_;
  return &found->second;
}

Preview* Model::preview(const doc::ClipPtr& clip, const doc::TuningDef* tuningDef, const doc::Project* p, doc::TrackId track) {
  auto found = previews_.find(clip.get());
  if (found != previews_.end() && found->second.clip == clip && found->second.tuning == tuningDef && found->second.timeline == timelineOf(p)) {
    found->second.lastUsed = paintSerial_;
    return &found->second;  // same immutable clip, same tuning: still valid
  }
  // Not cached: build within this paint's budget (always at least one clip per paint, so previews keep coming).
  const auto cost = static_cast<uint32_t>(std::min<size_t>(clip->notes.size(), UINT32_MAX));
  if (!unlimited_ && builtThisPaint_ && cost > budgetLeft_) {
    buildsDeferred = true;
    return nullptr;
  }
  builtThisPaint_ = true;
  budgetLeft_ -= std::min(budgetLeft_, cost);
  Preview& pv = found != previews_.end() ? found->second : previews_[clip.get()];
  pv.lastUsed = paintSerial_;
  ++stats.previewBuilds;
  cachedNotes_ -= previewWeight(pv);  // (the new entries are added once built)
  pv.clip = clip;
  pv.tuning = tuningDef;
  pv.timeline = timelineOf(p);
  pv.tile = PreviewTile{};  // a reused entry's tile showed another clip
  pv.notes.clear();
  pv.paths.clear();
  pv.maxLength = 0;
  const doc::NoteSeq& seq = clip->notes;
  if (seq.empty()) return &pv;
  const tuning::Tuning* t = tuningFor(tuningDef);
  const double refHz = tuningDef ? tuningDef->referenceHz : tuning::kDefaultReferenceHz;

  // The clip resolved when its notes need it: exact pitches (a JI member is anchor × ratio, where its anchor is when
  // it starts) and the effective curves (as the roll draws and the playback plays them).
  doc::NotePitchIndex index;
  bool resolved = false;
  if (p && needsResolve(*clip))
    if (const std::optional<doc::TrackPath> path = doc::findTrack(*p, track)) {
      index = doc::NotePitchIndex(doc::resolveClipPitches(*p, *path, *clip));
      resolved = true;
    }
  const bool curves = resolved && (clip->curves || index.anyWarp());

  // Pass 1: cents per note (kept in the y field's place via a scratch vector).
  std::vector<double>& cents = centsScratch_;
  cents.clear();
  cents.reserve(seq.size());
  double lo = INFINITY, hi = -INFINITY;
  for (const doc::Note& n : seq) {
    double c = pitchCents(n.pitch, t, refHz);
    if (resolved)
      if (const std::optional<double> hz = index.hz(n.id); hz && *hz > 0.0) c = 1200.0 * std::log2(*hz);
    cents.push_back(c);
    lo = std::min(lo, c);
    hi = std::max(hi, c);
  }
  // Pass 2 (bug-curve-render): each moving note's curve, a path drawn as an anti-aliased stroke (paintPreviewPaths),
  // flattened once to ~0.25 px of the tallest body (kMaxTrackHeight at 2×, ~960 px) over the notes' range (at least
  // 100 ¢: a clip of one pitch is as tall as its curves), 2 ¢ at most; a vibrato's note also keeps its envelope and the
  // curve without it, for the band a zoom too far out for its cycles draws.
  std::vector<uint8_t>& moving = movingScratch_;
  moving.assign(seq.size(), 0);
  if (curves) {
    const double tol = std::clamp(std::max(hi - lo, 100.0) / 4000.0, 1.0 / 64.0, 2.0);
    size_t i = 0;
    for (const doc::Note& n : seq) {
      std::vector<doc::Breakpoint> pts = doc::effectiveNoteCurve(*clip, n, index, tol);
      if (!pts.empty()) {
        const double c = cents[i];
        for (const doc::Breakpoint& b : pts) {
          lo = std::min(lo, c + static_cast<double>(b.value));
          hi = std::max(hi, c + static_cast<double>(b.value));
        }
        PreviewPath& path = pv.paths.emplace_back();
        path.start = n.start;
        path.end = n.start + n.length;
        path.cents = c;
        path.pts = std::move(pts);
        path.curved = std::any_of(path.pts.begin(), path.pts.end(), [](const doc::Breakpoint& b) { return b.curve != 0.0f; });
        path.flags = n.enabled() ? 0 : kPreviewDisabled;
        const doc::NoteCurves* rec = clip->curves ? doc::findNoteCurves(*clip, n.id) : nullptr;
        const double period = rec && rec->vibrato.set() ? index.vibratoPeriod(n.id) : 0.0;
        if (period > 0.0) {
          const Ticks L = std::max<Ticks>(n.length, 1);
          path.vib = doc::VibratoEnvelope(rec->vibrato, L);
          if (path.vib.any()) {
            path.vibPeriod = std::max(period, doc::kMinVibratoPeriodTicks);
            doc::NoteCurves still = *rec;
            still.vibrato = {};
            const doc::NoteWarp warp = index.anyWarp() ? index.warp(*clip, n.id) : doc::NoteWarp{};
            path.rest = doc::effectiveNoteCurve(&still, L, doc::noteBendTargets(still, index), warp, tol);
            path.restCurved = std::any_of(path.rest.begin(), path.rest.end(), [](const doc::Breakpoint& b) { return b.curve != 0.0f; });
          }
        }
        moving[i] = 1;
      }
      ++i;
    }
  }
  // Row height: one row per tuning step over the range (Live: one per semitone of the clip's note range), but
  // never thicker than a quarter of the body.
  double step = 100.0;
  if (t && t->edo()) step = t->edo()->stepCents();
  const double range = hi - lo;
  const double rowsD = range / std::max(step, 1.0) + 1.0;
  pv.rowFraction = static_cast<float>(1.0 / std::max(4.0, std::min(rowsD, 512.0)));
  pv.hiCents = hi;
  pv.invRange = range > 1e-9 ? 1.0 / range : 0.0;

  auto yOf = [&](double c) {
    const double yNorm = range > 1e-9 ? (hi - c) / range : 0.5;
    return static_cast<uint16_t>(std::lround(std::clamp(yNorm, 0.0, 1.0) * 65535.0));
  };
  // Pass 3: the steady notes.
  pv.notes.reserve(seq.size() - std::min(seq.size(), pv.paths.size()));
  size_t i = 0;
  for (const doc::Note& n : seq) {
    pv.maxLength = std::max(pv.maxLength, n.length);
    if (!moving[i]) pv.notes.push_back({n.start, n.start + n.length, yOf(cents[i]), static_cast<uint16_t>(n.enabled() ? 0 : kPreviewDisabled)});
    ++i;
  }
  pv.maxPathLength = 0;
  for (const PreviewPath& path : pv.paths) pv.maxPathLength = std::max(pv.maxPathLength, path.end - path.start);
  cachedNotes_ += previewWeight(pv);
  return &pv;
}

// --- audio layouts (T5.6) ---------------------------------------------------------------------------------

AudioLayout makeAudioLayout(const doc::Project& p, doc::TrackId track, const doc::AudioClipPtr& clip) {
  AudioLayout L;
  L.clip = clip;
  L.tempo = &p.timeline->tempoMap;
  if (const doc::SampleRef* s = doc::findSample(p, clip->sample)) {
    L.rate = s->rate;
    L.frames = std::max<int64_t>(1, s->frames);
    L.channels = std::max<uint32_t>(1, s->channels);
  }
  if (clip->warp == doc::AudioWarp::Off) {
    if (const std::optional<doc::TrackPath> path = doc::findTrack(p, track)) L.ratio = doc::audioRateRatio(p, *path, *clip);
    L.startSec = p.timeline->tempoMap.ticksToSeconds(clip->start);
    L.constantTempo = p.timeline->tempo.size() == 1;
  } else {
    L.fpt = doc::framesPerContentTick(L.rate, clip->sourceBpm);
  }
  return L;
}

AudioLayout* Model::cachedAudioLayout(const doc::AudioClip* clip) noexcept {
  const auto it = audio_.find(clip);
  if (it == audio_.end() || it->second.tempo != &shown->timeline->tempoMap) return nullptr;
  it->second.lastUsed = paintSerial_;
  return &it->second;
}

AudioLayout& Model::audioLayout(const ArrangementRow& row, const doc::AudioClipPtr& clip) {
  AudioLayout& L = audio_[clip.get()];
  if (L.clip != clip || L.tempo != &shown->timeline->tempoMap) {
    L = makeAudioLayout(*shown, row.id, clip);  // a reused entry's tile showed another clip: reset with it
  }
  L.lastUsed = paintSerial_;
  return L;
}

void Model::beginPaint(uint32_t buildBudgetNotes) noexcept {
  ++paintSerial_;
  unlimited_ = buildBudgetNotes == 0;
  budgetLeft_ = buildBudgetNotes;
  builtThisPaint_ = false;
  buildsDeferred = false;
}

void Model::endPaint() {
  // Bound the cache by what the view shows: once it holds more notes than the limit (or 3 × the most notes one paint
  // drew), previews not drawn in the last kPreviewMaxAgePaints paints go. The sweep is O(cache), at most every 64
  // paints.
  const size_t drawn = size_t{stats.noteRectsEmitted} + stats.tileNotes;  // notes shown, directly or through tiles
  peakNotes_ = std::max<size_t>(peakNotes_, drawn);
  if (paintSerial_ < nextTrim_) return;
  nextTrim_ = paintSerial_ + 64;
  // T5.6: audio layouts (a few hundred bytes each) beyond 4096 entries: those not drawn lately go.
  if (audio_.size() > 4096) {
    const uint64_t keep = paintSerial_ > arrangement_metrics::kPreviewMaxAgePaints ? paintSerial_ - arrangement_metrics::kPreviewMaxAgePaints : 0;
    std::erase_if(audio_, [&](const auto& kv) { return kv.second.lastUsed < keep; });
  }
  const size_t cap = std::max(cacheLimitNotes_, 3 * peakNotes_);
  peakNotes_ = drawn;
  if (cachedNotes_ <= cap) return;
  const uint64_t keepFrom =
      paintSerial_ > arrangement_metrics::kPreviewMaxAgePaints ? paintSerial_ - arrangement_metrics::kPreviewMaxAgePaints : 0;
  stats.previewsEvicted += std::erase_if(previews_, [&](const auto& kv) { return kv.second.lastUsed < keepFrom; });
  recountNotes();
}

void Model::recountNotes() noexcept {
  cachedNotes_ = 0;
  for (const auto& kv : previews_) cachedNotes_ += previewWeight(kv.second);
}

void Model::afterDocumentChange(size_t clipsInProject) {
  // Built tunings are dropped (their TuningDef may belong to snapshots that are gone; rebuilding one is cheap and
  // only happens on a preview miss). Previews keep their TuningDef pointer as a key: tuning nodes are immutable and
  // shared between snapshots, and the history keeps every snapshot alive until a Reset (which clears the caches),
  // so an unchanged pointer means an unchanged tuning.
  tunings_.clear();
  // The caches hold entries for clips/tracks that may no longer exist; drop them once they clearly outgrow the
  // project (keeps memory bounded without a sweep per edit).
  if (previews_.size() > 2 * clipsInProject + 256 || clipIndex_.size() > 2 * rows.size() + 256 ||
      audio_.size() > 2 * clipsInProject + 256 || takeIndex_.size() > 2 * takeRows.size() + 256) {
    std::unordered_set<const doc::NoteClip*> live;
    std::unordered_set<const doc::AudioClip*> liveAudio;
    std::unordered_set<uint32_t> liveTracks, liveTakes;
    live.reserve(clipsInProject);
    doc::forEachTrack(*shown, [&](const doc::Track& tr, const doc::Track*) {
      liveTracks.insert(tr.id.value);
      auto see = [&](const std::vector<doc::ClipSlot>& v) {
        for (const doc::ClipSlot& s : v) {
          if (s.clip) live.insert(s.clip.get());
          if (s.audio) liveAudio.insert(s.audio.get());
        }
      };
      see(tr.clips);
      for (const doc::TakeLanePtr& l : tr.takes) {  // T6.10
        liveTakes.insert(l->id.value);
        see(l->clips);
      }
    });
    std::erase_if(audio_, [&](const auto& kv) { return !liveAudio.contains(kv.first); });
    std::erase_if(previews_, [&](const auto& kv) { return !live.contains(kv.first); });
    recountNotes();
    std::erase_if(clipIndex_, [&](const auto& kv) { return !liveTracks.contains(kv.first); });
    std::erase_if(takeIndex_, [&](const auto& kv) { return !liveTakes.contains(kv.first); });
  }
}

void Model::clear() {
  clipIndex_.clear();
  takeIndex_.clear();
  previews_.clear();
  audio_.clear();
  cachedNotes_ = 0;
  tunings_.clear();
}

ui::Rgba Model::clipTextColour(const ui::Theme& th, ui::Rgba fill) {
  for (const TextColour& t : textColours_)
    if (t.theme == &th && t.fill == fill) return t.text;
  TextColour& slot = textColours_[textNext_++ % std::size(textColours_)];
  slot = {fill, ui::clipTextFor(th, fill), &th};
  return slot.text;
}

}  // namespace pq::views::arrangement
