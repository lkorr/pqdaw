// The arrangement's model caches (T3.8): per-track clip indices and per-clip note previews.

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "internal.h"

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

const ClipIndex& Model::clips(const doc::Track& t) {
  ClipIndex& ix = clipIndex_[t.id.value];
  if (ix.track == &t) return ix;
  ix.track = &t;
  const size_t n = t.clips.size();
  ix.start.resize(n);
  ix.end.resize(n);
  ix.maxEnd.resize(n);
  Ticks m = INT64_MIN;
  for (size_t i = 0; i < n; ++i) {
    const doc::NoteClip& c = *t.clips[i].clip;
    ix.start[i] = c.start;
    ix.end[i] = c.start + c.length;
    m = std::max(m, ix.end[i]);
    ix.maxEnd[i] = m;
  }
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

const Preview* Model::preview(const doc::ClipPtr& clip, const doc::TuningDef* tuningDef) {
  auto found = previews_.find(clip.get());
  if (found != previews_.end() && found->second.clip == clip && found->second.tuning == tuningDef) {
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
  cachedNotes_ += clip->notes.size();
  cachedNotes_ -= pv.notes.size();
  pv.clip = clip;
  pv.tuning = tuningDef;
  pv.notes.clear();
  pv.maxLength = 0;
  const doc::NoteSeq& seq = clip->notes;
  if (seq.empty()) return &pv;
  const tuning::Tuning* t = tuningFor(tuningDef);
  const double refHz = tuningDef ? tuningDef->referenceHz : tuning::kDefaultReferenceHz;

  // Pass 1: cents per note (kept in the y field's place via a scratch vector).
  std::vector<double>& cents = centsScratch_;
  cents.clear();
  cents.reserve(seq.size());
  double lo = INFINITY, hi = -INFINITY;
  for (const doc::Note& n : seq) {
    const double c = pitchCents(n.pitch, t, refHz);
    cents.push_back(c);
    lo = std::min(lo, c);
    hi = std::max(hi, c);
  }
  // Row height: one row per tuning step over the range (Live: one per semitone of the clip's note range), but
  // never thicker than a quarter of the body.
  double step = 100.0;
  if (t && t->edo()) step = t->edo()->stepCents();
  const double range = hi - lo;
  const double rowsD = range / std::max(step, 1.0) + 1.0;
  pv.rowFraction = static_cast<float>(1.0 / std::max(4.0, std::min(rowsD, 512.0)));

  pv.notes.resize(seq.size());
  size_t i = 0;
  for (const doc::Note& n : seq) {
    PreviewNote& o = pv.notes[i];
    o.start = n.start;
    o.end = n.start + n.length;
    const double yNorm = range > 1e-9 ? (hi - cents[i]) / range : 0.5;
    o.y = static_cast<uint16_t>(std::lround(std::clamp(yNorm, 0.0, 1.0) * 65535.0));
    o.flags = n.enabled() ? 0 : kPreviewDisabled;
    pv.maxLength = std::max(pv.maxLength, n.length);
    ++i;
  }
  return &pv;
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
  peakNotes_ = std::max<size_t>(peakNotes_, stats.noteRectsEmitted);
  if (paintSerial_ < nextTrim_) return;
  nextTrim_ = paintSerial_ + 64;
  const size_t cap = std::max(cacheLimitNotes_, 3 * peakNotes_);
  peakNotes_ = stats.noteRectsEmitted;
  if (cachedNotes_ <= cap) return;
  const uint64_t keepFrom =
      paintSerial_ > arrangement_metrics::kPreviewMaxAgePaints ? paintSerial_ - arrangement_metrics::kPreviewMaxAgePaints : 0;
  stats.previewsEvicted += std::erase_if(previews_, [&](const auto& kv) { return kv.second.lastUsed < keepFrom; });
  recountNotes();
}

void Model::recountNotes() noexcept {
  cachedNotes_ = 0;
  for (const auto& kv : previews_) cachedNotes_ += kv.second.notes.size();
}

void Model::afterDocumentChange(size_t clipsInProject) {
  // Built tunings are dropped (their TuningDef may belong to snapshots that are gone; rebuilding one is cheap and
  // only happens on a preview miss). Previews keep their TuningDef pointer as a key: tuning nodes are immutable and
  // shared between snapshots, and the history keeps every snapshot alive until a Reset (which clears the caches),
  // so an unchanged pointer means an unchanged tuning.
  tunings_.clear();
  // The caches hold entries for clips/tracks that may no longer exist; drop them once they clearly outgrow the
  // project (keeps memory bounded without a sweep per edit).
  if (previews_.size() > 2 * clipsInProject + 256 || clipIndex_.size() > 2 * rows.size() + 256) {
    std::unordered_set<const doc::NoteClip*> live;
    std::unordered_set<uint32_t> liveTracks;
    live.reserve(clipsInProject);
    doc::forEachTrack(*shown, [&](const doc::Track& tr, const doc::Track*) {
      liveTracks.insert(tr.id.value);
      for (const doc::ClipSlot& s : tr.clips) live.insert(s.clip.get());
    });
    std::erase_if(previews_, [&](const auto& kv) { return !live.contains(kv.first); });
    recountNotes();
    std::erase_if(clipIndex_, [&](const auto& kv) { return !liveTracks.contains(kv.first); });
  }
}

void Model::clear() {
  clipIndex_.clear();
  previews_.clear();
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
