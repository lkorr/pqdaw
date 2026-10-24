// The clip view's state: clip resolution and caches, view transform, selection, edits, drag commit (T3.11).

#include "roll_state.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "pq/document/commands.h"
#include "pq/ui/theme.h"
#include "roll_widgets.h"

namespace pq::views::roll {

namespace {

int64_t floorDiv(int64_t a, int64_t b) noexcept {
  const int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Largest shared denominator the Up/Down memory keeps (Q40); beyond it each note steps on its own denominator.
constexpr int64_t kMaxHarmonicDenominator = doc::kMaxFundamentalDenominator;

constexpr double kMinPxPerTick = 1e-5;  // ≈ 1 px per bar
constexpr double kMaxPxPerTick = 0.5;   // ≈ 870 px per 1/64 note
constexpr int32_t kMinRowPx = 3;
constexpr int32_t kMaxRowPx = 48;
// Drags of more notes than this repaint the whole note area and lane instead of tracking their rects.
constexpr size_t kMaxTrackedDragNotes = 256;

ui::Rgba mix(ui::Rgba a, ui::Rgba b, float t) noexcept { return ui::lerp(a, b, t); }

}  // namespace

RollState::RollState(ViewContext& c) : ctx(c) {}

RollState::~RollState() = default;

// --- clip and caches ------------------------------------------------------------------------------------

bool RollState::refresh() {
  const doc::Snapshot& cur = ctx.doc();
  const doc::ClipRef want = ctx.detailClip();
  bool changed = false;
  if (want.track != ref_.track || want.clip != ref_.clip) {
    ref_ = want;
    clip_.reset();
    sel_.clear();
    insertTick_.reset();
    fitted_ = false;
    drag = {};
    changed = true;
  }
  doc::ClipPtr c;
  const doc::TuningDef* def = nullptr;
  if (ref_.clip.valid()) {
    if (const std::optional<doc::TrackPath> path = doc::findTrack(*cur, ref_.track)) {
      const doc::Track& t = doc::trackAt(*cur, *path);
      const int32_t i = doc::findClipIndex(t, ref_.clip);
      if (i >= 0) {
        c = t.clips[static_cast<size_t>(i)].clip;
        def = ctx.previewedTuning(*cur, *path, c.get(), ref_.clip);  // the MOS wheel preview (T3.14)
      }
    }
  }
  const bool snapChanged = cur != snap_;
  snap_ = cur;
  bool rowsDirty = false;
  bool clipCaches = false;
  bool morphed = false;
  if (def && (!tuningValid_ || !(tuningDef_ == *def))) {
    if (tuningValid_ && morphInPlace(*def)) {
      // A generator move inside one MosLinearTable interval (T3.12 live morph): the tuning and the rows were
      // retuned in place; the row set, labels, chunk caches and highlight scale stay.
      morphed = true;
    } else if (Result<tuning::Tuning> t = doc::buildTuning(*def)) {
      const int32_t oldN = tuningValid_ ? degreesPerEquave(tuning_) : -1;
      tuning_ = std::move(*t);
      tuningDef_ = *def;
      tuningValid_ = true;
      scaleMask_ = defaultHighlightScale(tuning_);
      rowsDirty = true;
      changed = true;
      ++morphStats_.rebuilt;
      if (degreesPerEquave(tuning_) != oldN) {
        chunkCache_.clear();  // chunk degree spans are absolute degrees: equave · size + degree
        clipCaches = true;
      } else if (fold_ && clip_) {
        rebuildUsed();  // Ratio / Cents notes' nearest rows follow the pitches
      }
    }
  }
  if (def && (snapChanged || changed || morphed)) {
    std::string name = "Private Tuning";
    for (const doc::TuningSlotPtr& slot : *cur->tunings)
      if (slot->def == *def) {
        name = slot->name;
        break;
      }
    if (name != tuningName_) {
      tuningName_ = std::move(name);
      changed = true;
    }
  }
  if (c != clip_ || clipCaches) {
    clip_ = std::move(c);
    changed = true;
    anchorIndexValid_ = false;
    anchorIndex_.clear();
    rebuildChunks();
    if (clip_) {
      if (!(clip_->signature == sigsFor_)) {
        const TimeSigChange first{0, clip_->signature};
        if (auto m = TimeSigMap::create(std::span<const TimeSigChange>(&first, 1))) sigs_ = std::move(*m);
        else sigs_ = TimeSigMap{};
        sigsFor_ = clip_->signature;
      }
      rebuildColours();
      const std::vector<int64_t> before = used_;
      rebuildUsed();
      if (used_ != before) rowsDirty = true;
    }
  }
  if (rowsDirty || rows_.empty()) rebuildRows();
  pitchOnlyChange_ = morphed && !changed;
  return changed || morphed;
}

void RollState::rebuildChunks() {
  chunks_.clear();
  if (!clip_) {
    chunkCache_.clear();
    return;
  }
  std::unordered_map<const doc::Note*, ChunkInfo> next;
  next.reserve(clip_->notes.chunks().size());
  for (const doc::NoteSeq::Chunk& ch : clip_->notes.chunks()) {
    const doc::Note* p = ch.notes.get();
    ChunkInfo info;
    if (auto it = chunkCache_.find(p); it != chunkCache_.end() && it->second.count == ch.count) {
      info = it->second;  // chunks are immutable: same pointer and count = same notes
    } else {
      info.notes = p;
      info.count = ch.count;
      info.firstStart = ch.count ? p[0].start : 0;
      // One pass: the time extent, the Degree span (unfolded row range) and the lcm of the reference-anchored
      // ratios' denominators (the virtual fundamental), so an edit rescans only the chunks it rebuilt.
      const int64_t n = std::max(1, degreesPerEquave(tuning_));
      Ticks maxEnd = INT64_MIN;
      int64_t lo = INT64_MAX, hi = INT64_MIN, den = 0;
      for (uint32_t i = 0; i < ch.count; ++i) {
        const doc::Note& note = p[i];
        maxEnd = std::max(maxEnd, note.start + note.length);
        if (note.pitch.kind() != tuning::PitchKind::Degree) ++info.nonDegree;
        if (note.pitch.kind() == tuning::PitchKind::Degree) {
          const int64_t d = static_cast<int64_t>(note.pitch.equave()) * n + note.pitch.degree();
          lo = std::min(lo, d);
          hi = std::max(hi, d);
        } else if (note.pitch.kind() == tuning::PitchKind::Ratio && note.pitch.anchorKind() == tuning::AnchorKind::Reference &&
                   den >= 0) {
          const int64_t d = note.pitch.ratio().den();
          den = den ? std::lcm(den, d) : d;
          if (den > kMaxHarmonicDenominator) den = -1;
        }
      }
      info.maxEnd = maxEnd;
      info.minDegree = lo;
      info.maxDegree = hi;
      info.ratioDen = den;
    }
    chunks_.push_back(info);
    next.emplace(p, info);
  }
  chunkCache_ = std::move(next);
}

void RollState::rebuildColours() {
  const uint32_t rgb = clip_ ? clip_->colour.rgb : 0x808080;
  if (colours_.forRgb == rgb) return;
  colours_.forRgb = rgb;
  const ui::Rgba base = ui::hex(rgb);
  // Live colours notes by velocity: soft notes fade towards the editor's grey, loud ones take the full clip colour.
  const ui::Rgba soft = ui::hex(0x5A5A5A);
  const ui::Rgba dark = ui::scaleRgb(base, 0.32f);
  for (int v = 0; v < 128; ++v) {
    const float t = 0.12f + 0.88f * std::pow(static_cast<float>(v) / 127.0f, 1.1f);
    const ui::Rgba f = mix(soft, base, t);
    colours_.fill[v] = f;
    colours_.border[v] = mix(dark, ui::hex(0x101010), 0.35f);
    // Selected: lifted towards white, as Live draws the selection lighter than the clip colour.
    colours_.selFill[v] = mix(f, ui::hex(0xFFFFFF), 0.55f);
  }
  colours_.selBorder = ui::hex(0x161616);
  colours_.disabledFill = ui::hex(0x5C5C5C);
  colours_.disabledBorder = ui::hex(0x2A2A2A);
}

void RollState::rebuildUsed() {
  used_.clear();
  if (!clip_) return;
  if (!fold_) {
    // Unfolded rows only need the Degree span (the range always covers it): from the chunk summaries.
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    for (const ChunkInfo& c : chunks_) {
      lo = std::min(lo, c.minDegree);
      hi = std::max(hi, c.maxDegree);
    }
    if (lo <= hi) used_ = lo == hi ? std::vector<int64_t>{lo} : std::vector<int64_t>{lo, hi};
    return;
  }
  const int32_t n = std::max(1, degreesPerEquave(tuning_));
  for (const doc::Note& note : clip_->notes) {
    if (note.pitch.kind() == tuning::PitchKind::Degree) {
      used_.push_back(static_cast<int64_t>(note.pitch.equave()) * n + note.pitch.degree());
    } else if (const std::optional<double> c = centsOf(note.pitch)) {
      used_.push_back(nearestDegree(*c));
    }
  }
  std::sort(used_.begin(), used_.end());
  used_.erase(std::unique(used_.begin(), used_.end()), used_.end());
}

void RollState::rebuildRows() {
  const float scale = (noteArea && noteArea->tree()) ? noteArea->tree()->scale() : 1.0f;
  const ui::UiScale us(scale);
  // Keep the pitch at the middle of the area where it is.
  const ui::IRect a = area();
  std::optional<double> centreCents;
  if (!rows_.empty() && a.h > 0) {
    // (The middle of the content when it is shorter than the area, e.g. a fold of a few rows.)
    const int32_t cy = scrollY_ + std::min(a.h, rows_.contentHeight()) / 2;
    // Scale layouts keep the exact pitch there (a morph crossing a region does not jump), except when the layout
    // itself changes: then the centred row stays centred (mos-roll.js toggleLayout).
    centreCents = rows_.scaleLayout() && !keepCentredRow_ ? rows_.centsAtY(cy) : rows_.row(rows_.indexAtY(cy)).cents;
  }
  RowOptions o;
  o.layout = scaleLayout_;
  o.pxScale = static_cast<double>(scale);
  o.rowPx = std::max(1, us.px(static_cast<float>(rowPxLogical_)));
  o.fold = fold_ ? (scale_ ? Fold::Scale : Fold::UsedNotes) : Fold::None;
  o.used = used_;
  if (scale_ || o.fold == Fold::Scale) o.scale = scaleMask_;
  rows_ = RowModel::build(tuning_, o);
  builtScale_ = scale;
  if (centreCents && !rows_.empty() && a.h > 0) {
    const CentsPlacement p = rows_.placeCents(*centreCents);
    scrollY_ = p.centre - a.h / 2;
  }
  scrollByPx(0, 0);  // clamp
}

// --- live morph (T3.12) -----------------------------------------------------------------------------------------

// SPEC §3.3: inside one interval of the generator axis where the chain's sorted order is fixed (a MosLinearTable
// interval: no chain pitch crosses another or the period), every degree is linear in the generator, so a generator
// change is n multiply-adds into the existing tuning and an in-place relayout of the existing rows. Anything else (a
// crossing into another interval or MOS size, a mode / MODMOS / period change, a folded roll whose used rows may
// change) returns false and refresh() rebuilds (buildMOSScale via doc::buildTuning; the mode across a region change
// was carried by whoever changed the generator: the chart, or morphGenerator below, with transferModeRotation).
bool RollState::morphInPlace(const doc::TuningDef& def) {
  const tuning::MosParams* a = tuningDef_.mos();
  const tuning::MosParams* b = def.mos();
  tuning::Mos* mos = tuning_.mos();
  if (!a || !b || !mos || fold_ || rows_.empty() || !rows_.scaleLayout()) return false;
  if (a->n != b->n || a->period != b->period || a->periodsPerEquave != b->periodsPerEquave ||
      a->rotation != b->rotation || a->octaveStretch != b->octaveStretch || def.referenceHz != tuningDef_.referenceHz ||
      tuning::hasModmos(a->modmos) || tuning::hasModmos(b->modmos) || !a->genSequence.empty() || !b->genSequence.empty() ||
      a->modmos.size() != b->modmos.size())
    return false;
  const double P = b->period;
  if (!linear_ || linear_->n() != b->n) {
    linear_ = std::make_unique<tuning::MosLinearTable>(b->n);  // ~0.3·n² intervals, once per chain length
    linearHigh_ = -1.0;
  }
  // Stay clear of the interval ends, where buildMOSScale nudges a degenerate chain (chainSafeGen, 1e-6 ¢).
  constexpr double kEdge = 1e-4;
  auto inside = [&](double g, double lo, double hi) { return g > lo + kEdge && g < hi - kEdge; };
  const tuning::MosLinearSpan span = linear_->lookup(b->generator, P);
  if (!inside(b->generator, span.lowGenerator, span.highGenerator) ||
      !inside(a->generator, span.lowGenerator, span.highGenerator))
    return false;  // the generator crossed an interval boundary (or sits on one): rebuild
  if (span.lowGenerator != linearLow_ || span.highGenerator != linearHigh_ || b->rotation != linearRotation_) {
    linearK_.resize(static_cast<size_t>(b->n));
    linearM_.resize(static_cast<size_t>(b->n));
    tuning::rotateLinearForm(span, b->rotation, linearK_.data(), linearM_.data());
    linearLow_ = span.lowGenerator;
    linearHigh_ = span.highGenerator;
    linearRotation_ = b->rotation;
  }
  mos->retuneLinear(b->generator, linearK_, linearM_, b->region);
  tuningDef_ = def;
  rows_.retune(tuning_);
  ++morphStats_.linear;
  scrollByPx(0.0f, 0.0f);  // the content height may change; keep the scroll in range
  return true;
}

bool RollState::canMorphGenerator() const noexcept {
  return clip_ != nullptr && tuningValid_ && tuningDef_.mos() != nullptr && tuningDef_.mos()->genSequence.empty() &&
         tuningDef_.mos()->n >= 2 && (morphBase_.has_value() || !ctx.tuningPreview().has_value());
}

void RollState::beginGeneratorMorph() {
  if (morphBase_ || !canMorphGenerator()) return;
  // The document's tuning (not a preview): what end(false) restores and end(true) compares with.
  morphBase_ = tuningDef_;
  morphDef_ = tuningDef_;
  const tuning::MosParams& m = *morphDef_.mos();
  const int32_t maxSize = std::max(18, m.n);  // the chart's default size range, or more for a larger MOS
  if (morphRegionsPeriod_ != m.period || morphRegionsReps_ != m.periodsPerEquave || morphRegionsMax_ != maxSize) {
    morphRegions_ = tuning::buildMosRegions(maxSize, maxSize, m.period, m.periodsPerEquave);
    morphRegionsPeriod_ = m.period;
    morphRegionsReps_ = m.periodsPerEquave;
    morphRegionsMax_ = maxSize;
  }
}

void RollState::morphGenerator(double g) {
  if (!morphBase_) beginGeneratorMorph();
  if (!morphBase_ || !std::isfinite(g)) return;
  tuning::MosParams& m = std::get<tuning::MosParams>(morphDef_.system);
  const double P = m.period;
  const std::span<const tuning::MosRegion> regions = morphRegions_.regions;
  // The current region: the one the hint names, else the one of this size containing the generator.
  auto contains = [&](const tuning::MosRegion& r, double x) { return x >= r.lowCents && x <= r.highCents; };
  const tuning::MosRegion* sel = nullptr;
  for (const tuning::MosRegion& r : regions) {
    if (r.perPeriodSize != m.n) continue;
    if (m.region && r.lowCents == m.region->lowCents && r.highCents == m.region->highCents) {
      sel = &r;
      break;
    }
    if (!sel && contains(r, m.generator)) sel = &r;
  }
  double gen = g;
  if (sel && !contains(*sel, g)) {
    // mos_chart.cpp dragGenerator (setGenFromScreenX): into the adjacent region, carrying the mode.
    const double clamped = std::clamp(g, 0.0, P);
    const int32_t adj = tuning::findAdjacentRegion(regions, *sel, clamped, 2, morphRegionsMax_);
    if (adj >= 0) {
      const tuning::MosRegion& r = regions[static_cast<size_t>(adj)];
      gen = std::clamp(clamped, r.lowCents, r.highCents);
      if (!(r.key == sel->key)) {
        const tuning::RegionHint hint = r.hint();
        if (r.perPeriodSize == sel->perPeriodSize && m.n == sel->perPeriodSize) {
          // Same chain length: the rule the tuning node's table follows when automation moves the generator
          // (MosLinearTable::carryRotation = transferModeRotation for an unchanged size; T2.6-fix2), so the roll's
          // morph and the audio cannot disagree on the mode.
          if (!linear_ || linear_->n() != m.n) {
            linear_ = std::make_unique<tuning::MosLinearTable>(m.n);
            linearHigh_ = -1.0;
          }
          m.rotation = linear_->carryRotation(m.generator, m.rotation, gen, P, &hint);
        } else {
          m.rotation = tuning::transferModeRotation(m.generator, sel->perPeriodSize, m.rotation, gen, r.perPeriodSize, P, &hint);
        }
      }
      if (r.perPeriodSize != m.n) m.modmos.clear();  // alterations are per degree of the old size
      m.n = r.perPeriodSize;
      m.region = r.hint();
    } else {
      gen = std::clamp(g, sel->lowCents, sel->highCents);
    }
  } else if (sel) {
    m.region = sel->hint();
  }
  if (gen == m.generator && morphDef_ == tuningDef_) return;
  m.generator = gen;
  ctx.setTuningPreview(TuningTarget::forClip(ref_), morphDef_);  // → kTuningPreview → refresh(): the rows follow
}

void RollState::endGeneratorMorph(bool commit) {
  if (!morphBase_) return;
  const doc::TuningDef base = std::move(*morphBase_);
  morphBase_.reset();
  if (ctx.tuningPreview()) ctx.clearTuningPreview();
  if (commit && !(morphDef_ == base) && clip_) {
    if (Status s = ctx.store().dispatch(tuningCommand(TuningTarget::forClip(ref_), morphDef_)); !s)
      ctx.setStatus("Generator: " + s.error().message);
  }
  (void)refresh();
  markAllDirty();
}

void RollState::setScaleLayout(RowLayout layout) {
  if (layout == RowLayout::Uniform) layout = RowLayout::Equidistant;
  if (layout == scaleLayout_) return;
  scaleLayout_ = layout;
  keepCentredRow_ = true;
  rebuildRows();
  keepCentredRow_ = false;
  markAllDirty();
}

void RollState::setOverlayEdo(uint32_t n) {
  if (n == overlayEdo()) return;
  overlay_.reset();
  if (n > 0)
    if (Result<tuning::Edo> e = tuning::Edo::make(n)) overlay_ = *e;
  markAllDirty();
}

double RollState::nearestOverlayCents(double cents) const noexcept {
  if (!overlay_) return cents;
  const double step = overlay_->equaveCents() / overlay_->n();
  return overlay_->centsOfStep(static_cast<int64_t>(std::llround(cents / step)));
}

tuning::PitchId RollState::drawnPitch(const DrawnNote& d) const noexcept {
  return d.onOverlay ? tuning::PitchId::fromCents(d.cents) : degreePitch(d.degree);
}

void RollState::buildAnchorIndex() const {
  anchorIndex_.clear();
  if (clip_)
    for (const doc::Note& n : clip_->notes) anchorIndex_.emplace(n.id.value, &n);
  anchorIndexValid_ = true;
}

doc::ClipLoop RollState::shownLoop() const noexcept {
  if (!clip_) return {};
  doc::ClipLoop l = clip_->loop;
  if (drag.kind == DragKind::LoopBrace) {
    if (drag.brace != BraceEdge::End) l.start += drag.dt;
    if (drag.brace != BraceEdge::Start) l.end += drag.dt;
    if (l.end <= l.start) l.end = l.start + 1;
  }
  return l;
}

Ticks RollState::shownContentStart() const noexcept {
  if (!clip_) return 0;
  return clip_->contentStart + (drag.kind == DragKind::StartMarker ? drag.dt : 0);
}

Ticks RollState::shownLength() const noexcept {
  if (!clip_) return kTicksPerWhole;
  return std::max<Ticks>(1, clip_->length + (drag.kind == DragKind::EndMarker ? drag.dt : 0));
}

Ticks RollState::regionStart() const noexcept {
  if (!clip_) return 0;
  const doc::ClipLoop l = shownLoop();
  return l.enabled ? l.start : shownContentStart();
}

Ticks RollState::regionEnd() const noexcept {
  if (!clip_) return kTicksPerWhole;
  const doc::ClipLoop l = shownLoop();
  return l.enabled ? l.end : shownContentStart() + shownLength();
}

// --- transform ------------------------------------------------------------------------------------------

ui::IRect RollState::area() const noexcept { return noteArea ? noteArea->rect() : ui::IRect{}; }

float RollState::xOfTick(double t) const noexcept {
  return static_cast<float>(area().x + (t - scrollTick_) * pxPerTick_);
}

double RollState::tickAtX(float x) const noexcept {
  return pxPerTick_ > 0.0 ? scrollTick_ + (static_cast<double>(x) - area().x) / pxPerTick_ : scrollTick_;
}

int32_t RollState::screenY(int32_t cy) const noexcept { return area().y + cy - scrollY_; }

int32_t RollState::contentY(float sy) const noexcept {
  return static_cast<int32_t>(std::floor(sy)) - area().y + scrollY_;
}

void RollState::setZoom(double ppt, double anchorTick, float anchorX) {
  ppt = std::clamp(ppt, kMinPxPerTick, kMaxPxPerTick);
  pxPerTick_ = ppt;
  scrollTick_ = anchorTick - (static_cast<double>(anchorX) - area().x) / ppt;
  scrollToTick(scrollTick_);
  reportResolvedGrid();
}

void RollState::scrollToTick(double t) {
  const double minTick = static_cast<double>(std::min<Ticks>(0, regionStart()) - kTicksPerWhole);
  scrollTick_ = std::max(t, minTick);
  if (rulerView) rulerView->markDirty();
  markNotesDirty();
}

void RollState::scrollByPx(float dx, float dy) {
  if (dx != 0.0f && pxPerTick_ > 0.0) scrollToTick(scrollTick_ + dx / pxPerTick_);
  const int32_t maxY = std::max(0, rows_.contentHeight() - area().h);
  const int32_t y = std::clamp(scrollY_ + static_cast<int32_t>(std::lround(dy)), 0, maxY);
  if (y != scrollY_) {
    scrollY_ = y;
    if (keysView) keysView->markDirty();
    if (noteArea) noteArea->markDirty();
  }
}

void RollState::setRowHeight(int32_t logicalPx) {
  logicalPx = std::clamp(logicalPx, kMinRowPx, kMaxRowPx);
  if (logicalPx == rowPxLogical_) return;
  rowPxLogical_ = logicalPx;
  rebuildRows();
  if (keysView) keysView->markDirty();
  markNotesDirty();
}

void RollState::centreOnDegree(int64_t degree) {
  if (rows_.empty()) return;
  int32_t cy;
  if (const PitchRow* r = rows_.rowOfDegree(degree)) cy = r->centre;
  else cy = rows_.placeCents(degreeCents(tuning_, degree)).centre;
  scrollY_ = 0;
  scrollByPx(0, static_cast<float>(cy - area().h / 2));
  if (keysView) keysView->markDirty();
  markNotesDirty();
}

void RollState::fitToClip() {
  const ui::IRect a = area();
  if (!clip_ || a.w <= 0 || a.h <= 0) return;
  fitted_ = true;
  // Rows: an equave of about 240 logical px (12edo: 14 px rows, Live's default key height; 31edo: 8 px).
  const int32_t n = std::max(1, degreesPerEquave(tuning_));
  // (EDO rows stop at Live's 14 px; a scale's fewer degrees get the whole equave height: 5L 2s → 34 px lanes.)
  const int32_t maxRow = tuning_.edo() ? 14 : kMaxRowPx;
  const int32_t rowPx = std::clamp(static_cast<int32_t>(std::lround(240.0 / n)), kMinRowPx + 2, maxRow);
  if (rowPx != rowPxLogical_) {
    rowPxLogical_ = rowPx;
    rebuildRows();
  }
  const Ticks start = regionStart();
  const Ticks span = std::max<Ticks>(regionEnd() - start, kPpq);
  pxPerTick_ = std::clamp(static_cast<double>(a.w) / static_cast<double>(span), kMinPxPerTick, kMaxPxPerTick);
  scrollTick_ = static_cast<double>(start);
  centreOnDegree(densestDegree(a.h));
  reportResolvedGrid();
}

// The degree to centre a fresh view on (T3.x-fix-edit, M3 review M14): the middle of the band one view tall that holds
// the most notes. The middle of the whole pitch extent (Q77.5 before) showed empty equaves, and no note at all, as soon
// as one note sat far away (12 equaves up, −9000 ¢). Notes count once each; ties go to the band nearest the median.
int64_t RollState::densestDegree(int32_t viewHeightPx) const {
  if (!clip_ || clip_->notes.empty()) return 0;
  const int32_t n = std::max(1, degreesPerEquave(tuning_));
  std::vector<int64_t> pos;
  pos.reserve(clip_->notes.size());
  for (const doc::Note& note : clip_->notes) {
    if (note.pitch.kind() == tuning::PitchKind::Degree) pos.push_back(static_cast<int64_t>(note.pitch.equave()) * n + note.pitch.degree());
    else if (const std::optional<double> c = centsOf(note.pitch)) pos.push_back(nearestDegree(*c));
  }
  if (pos.empty()) return 0;
  std::sort(pos.begin(), pos.end());
  // Degrees one view spans: an equave is about 240 logical px in the default fit (rows and lanes alike).
  const float scale = (noteArea && noteArea->tree()) ? noteArea->tree()->scale() : 1.0f;
  const double equavePx = 240.0 * static_cast<double>(scale);
  const auto span = std::max<int64_t>(1, static_cast<int64_t>(static_cast<double>(n) * static_cast<double>(viewHeightPx) / equavePx) - 1);
  // The middle of the Degree notes' extent (Q77.5) while it shows at least a quarter of the notes: the usual clip,
  // unchanged.
  const int64_t whole = used_.empty() ? (pos.front() + pos.back()) / 2 : (used_.front() + used_.back()) / 2;
  const auto lo = std::lower_bound(pos.begin(), pos.end(), whole - span / 2);
  const auto hi = std::upper_bound(pos.begin(), pos.end(), whole + span / 2);
  if (static_cast<size_t>(hi - lo) * 4 >= pos.size()) return whole;
  const int64_t median = pos[pos.size() / 2];
  size_t best = 0, bestEnd = 0, j = 0;
  int64_t bestDist = INT64_MAX;
  for (size_t i = 0; i < pos.size(); ++i) {
    if (i > 0 && pos[i] == pos[i - 1]) continue;  // windows start at a note
    while (j < pos.size() && pos[j] - pos[i] <= span) ++j;
    const size_t count = j - i;
    const int64_t mid = (pos[i] + pos[j - 1]) / 2;
    const int64_t dist = mid > median ? mid - median : median - mid;
    if (count > bestEnd - best || (count == bestEnd - best && dist < bestDist)) {
      best = i;
      bestEnd = j;
      bestDist = dist;
    }
  }
  return (pos[best] + pos[bestEnd - 1]) / 2;
}

void RollState::afterLayout() {
  const float scale = (noteArea && noteArea->tree()) ? noteArea->tree()->scale() : 1.0f;
  if (scale != builtScale_) rebuildRows();
  else scrollByPx(0.0f, 0.0f);
  if (!fitted_ && clip_) fitToClip();
}

Grid RollState::grid() const noexcept {
  const float scale = (noteArea && noteArea->tree()) ? noteArea->tree()->scale() : 1.0f;
  return Grid(ctx.grid(), sigs_, pxPerTick_ / static_cast<double>(scale));
}

void RollState::reportResolvedGrid() {
  if (!noteArea || !noteArea->focused()) return;
  ctx.setResolvedGrid(grid().divisionAt(static_cast<Ticks>(std::llround(scrollTick_))));
}

// --- display options ------------------------------------------------------------------------------------

void RollState::setFold(bool on) {
  if (on == fold_) return;
  fold_ = on;
  rebuildUsed();
  rebuildRows();
  markAllDirty();
}

void RollState::setScaleHighlight(bool on) {
  if (on == scale_) return;
  scale_ = on;
  rebuildRows();
  markAllDirty();
}

void RollState::setShowHarmonics(bool on) {
  if (on == harmonics_) return;
  harmonics_ = on;
  markAllDirty();
}

void RollState::setShowChance(bool on) {
  if (on == chance_) return;
  chance_ = on;
  if (laneView) laneView->markDirty();
  if (laneHeaderView) laneHeaderView->refresh();
}

// --- notes ----------------------------------------------------------------------------------------------

std::optional<double> RollState::centsOf(const tuning::PitchId& p) const {
  switch (p.kind()) {
    case tuning::PitchKind::Degree: return degreeCents(tuning_, absoluteDegree(tuning_, p.degree(), p.equave()));
    case tuning::PitchKind::Cents: return p.cents();
    case tuning::PitchKind::Ratio: {
      const double rc = tuning::cents(p.ratio());
      if (p.anchorKind() == tuning::AnchorKind::Reference) return rc;
      if (p.anchorKind() != tuning::AnchorKind::Note) return std::nullopt;  // JI objects do not exist yet
      if (!anchorIndexValid_) buildAnchorIndex();
      // Follow the anchor chain (bounded: a cycle or a missing anchor leaves the note unplaced).
      double total = rc;
      uint32_t id = p.anchorId();
      for (int depth = 0; depth < 16; ++depth) {
        auto it = anchorIndex_.find(id);
        if (it == anchorIndex_.end()) return std::nullopt;
        const tuning::PitchId& a = it->second->pitch;
        if (a.kind() != tuning::PitchKind::Ratio) {
          const std::optional<double> base = centsOf(a);
          if (!base) return std::nullopt;
          return total + *base;
        }
        total += tuning::cents(a.ratio());
        if (a.anchorKind() == tuning::AnchorKind::Reference) return total;
        if (a.anchorKind() != tuning::AnchorKind::Note) return std::nullopt;
        id = a.anchorId();
      }
      return std::nullopt;
    }
    case tuning::PitchKind::Harmonic: return std::nullopt;  // needs the JI object (SPEC §5)
  }
  return std::nullopt;
}

int64_t RollState::nearestDegree(double cents) const noexcept {
  const int32_t n = std::max(1, degreesPerEquave(tuning_));
  const double eq = degreeCents(tuning_, n) - degreeCents(tuning_, 0);
  const double step = eq > 0.0 ? eq / n : 100.0;
  int64_t d = static_cast<int64_t>(std::llround(cents / step));
  // Degrees ascend in pitch: walk to the closest one (a step or two for uneven scales).
  for (int i = 0; i < 4 * n + 4; ++i) {
    const double here = std::fabs(degreeCents(tuning_, d) - cents);
    if (std::fabs(degreeCents(tuning_, d + 1) - cents) < here) ++d;
    else if (std::fabs(degreeCents(tuning_, d - 1) - cents) < here) --d;
    else break;
  }
  return d;
}

std::optional<int64_t> RollState::rowDegreeOf(const tuning::PitchId& p) const {
  if (p.kind() == tuning::PitchKind::Degree) return absoluteDegree(tuning_, p.degree(), p.equave());
  const std::optional<double> c = centsOf(p);
  if (!c) return std::nullopt;
  return nearestDegree(*c);
}

NotePlace RollState::place(const tuning::PitchId& p) const {
  NotePlace r;
  if (rows_.empty()) return r;
  double cents;
  if (p.kind() == tuning::PitchKind::Degree) {
    const int64_t d = absoluteDegree(tuning_, p.degree(), p.equave());
    if (const PitchRow* row = rows_.rowOfDegree(d)) {
      r.top = row->top;
      r.bottom = row->bottom;
      r.z = row->z;
      r.ok = true;
      return r;
    }
    cents = degreeCents(tuning_, d);  // a folded-away row: between its visible neighbours
  } else {
    const std::optional<double> c = centsOf(p);
    if (!c) return r;
    cents = *c;
  }
  const CentsPlacement cp = rows_.placeCents(cents);
  r.top = cp.top;
  r.bottom = cp.bottom;
  r.z = cp.z;
  r.ok = true;
  if (p.kind() != tuning::PitchKind::Degree && std::fabs(cp.offsetCents) > kOnGridToleranceCents)
    r.offGrid = cp.offsetCents > 0 ? 1 : -1;
  return r;
}

const doc::Note* RollState::findNote(doc::NoteId id) const noexcept { return clip_ ? clip_->notes.findById(id) : nullptr; }

const doc::Note* RollState::noteAt(ui::Vec2 p, int* zone) const {
  if (!clip_ || pxPerTick_ <= 0.0) return nullptr;
  const float scale = (noteArea && noteArea->tree()) ? noteArea->tree()->scale() : 1.0f;
  const float grab = kEdgeGrab * scale;
  const double t0 = tickAtX(p.x - 3.0f * grab), t1 = tickAtX(p.x + 3.0f * grab);
  const int32_t cy = contentY(p.y);
  const doc::Note* hit = nullptr;
  int hitZone = 0;
  int hitKey = -1;
  const bool zOrder = rows_.layout() == RowLayout::Proportional;
  forNotesIn(static_cast<Ticks>(std::floor(t0)), static_cast<Ticks>(std::ceil(t1)) + 1, [&](const doc::Note& n) {
    const NotePlace pl = place(n.pitch);
    if (!pl.ok || cy < pl.top || cy >= pl.bottom) return;
    const float x0 = std::round(xOfTick(static_cast<double>(n.start)));
    const float x1 = std::max(x0 + 3.0f, std::round(xOfTick(static_cast<double>(n.start + n.length))));
    if (p.x < x0 || p.x >= x1) return;
    // The note drawn on top: later notes, and in the Proportional layout higher z (selected notes highest).
    const int key = zOrder ? pl.z + (selected(n.id) ? 256 : 0) : 0;
    if (key < hitKey) return;
    hitKey = key;
    hit = &n;
    const float w = x1 - x0;
    hitZone = 0;
    if (p.x >= x1 - grab && w >= 2.0f * grab) hitZone = 1;
    else if (p.x < x0 + grab && w >= 3.0f * grab) hitZone = -1;
  });
  if (zone) *zone = hitZone;
  return hit;
}

// --- selection ------------------------------------------------------------------------------------------

bool RollState::selected(doc::NoteId id) const noexcept {
  if (sel_.empty()) return false;
  if (!selBits_.empty()) {  // O(1): the paint loop asks once per visible note
    const uint64_t off = static_cast<uint64_t>(id.value) - selMin_;
    return off < static_cast<uint64_t>(selBits_.size()) * 64 && ((selBits_[off >> 6] >> (off & 63)) & 1u) != 0;
  }
  return std::binary_search(sel_.begin(), sel_.end(), id.value);
}

void RollState::select(std::vector<uint32_t> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (ids == sel_) return;
  sel_ = std::move(ids);
  // A bitmap over [min id, max id] when it is compact (ids of one clip mostly are): ≤ 64 bits per selected note
  // plus 64 KiB; otherwise selected() binary-searches.
  selBits_.clear();
  if (!sel_.empty()) {
    selMin_ = sel_.front();
    const uint64_t span = static_cast<uint64_t>(sel_.back()) - selMin_ + 1;
    if (span <= 64 * static_cast<uint64_t>(sel_.size()) + (uint64_t{1} << 19)) {
      selBits_.assign(static_cast<size_t>((span + 63) / 64), 0);
      for (uint32_t id : sel_) {
        const uint64_t off = id - selMin_;
        selBits_[off >> 6] |= uint64_t{1} << (off & 63);
      }
    }
  }
  markNotesDirty();
  if (boxPanel) boxPanel->markDirty();  // its "Selected" count
}

void RollState::selectOnly(doc::NoteId id) { select({id.value}); }

void RollState::toggleSelected(doc::NoteId id) {
  std::vector<uint32_t> s = sel_;
  auto it = std::lower_bound(s.begin(), s.end(), id.value);
  if (it != s.end() && *it == id.value) s.erase(it);
  else s.insert(it, id.value);
  select(std::move(s));
}

void RollState::clearSelection() { select({}); }

void RollState::selectAll() {
  if (!clip_) return;
  std::vector<uint32_t> all;
  all.reserve(clip_->notes.size());
  for (const doc::Note& n : clip_->notes) all.push_back(n.id.value);
  select(std::move(all));
}

void RollState::selectRow(int64_t degree, bool add) {
  if (!clip_) return;
  std::vector<uint32_t> s = add ? sel_ : std::vector<uint32_t>{};
  for (const doc::Note& n : clip_->notes)
    if (const std::optional<int64_t> d = rowDegreeOf(n.pitch); d && *d == degree) s.push_back(n.id.value);
  select(std::move(s));
}

std::vector<doc::NoteKey> RollState::selectionKeys() const {
  std::vector<doc::NoteKey> keys;
  if (!clip_ || sel_.empty()) return keys;
  keys.reserve(sel_.size());
  for (const doc::Note& n : clip_->notes)
    if (selected(n.id)) keys.push_back(doc::keyOf(n));
  return keys;
}

// --- edits ----------------------------------------------------------------------------------------------

bool RollState::apply(const doc::ops::EditResult& e, std::string_view what) {
  if (!e) {
    ctx.setStatus(std::string(what) + ": " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status s = ctx.store().dispatch(e->command); !s) {
    ctx.setStatus(std::string(what) + ": " + s.error().message);
    return false;
  }
  (void)refresh();  // the ClipView's store listener did this already when it is subscribed; cheap when nothing moved
  std::vector<uint32_t> ids;
  ids.reserve(e->notes.size());
  for (const doc::NoteKey& k : e->notes) ids.push_back(k.id.value);
  select(std::move(ids));
  return true;
}

void RollState::deleteSelection() {
  if (!clip_) return;
  const std::vector<doc::NoteKey> keys = selectionKeys();
  if (keys.empty()) return;
  if (apply(doc::ops::deleteNotes(*ctx.doc(), ref_, keys), "Delete Notes")) clearSelection();
}

void RollState::duplicateSelection() {
  if (!clip_) return;
  const std::vector<doc::NoteKey> keys = selectionKeys();
  if (keys.empty()) return;
  (void)apply(doc::ops::duplicateNotes(*ctx.doc(), ctx.store().ids(), ref_, keys), "Duplicate Notes");
}

void RollState::quantizeSelection() {
  if (!clip_) return;
  const std::vector<doc::NoteKey> keys = selectionKeys();  // empty: the whole clip (Live quantizes all)
  const Grid g = grid();
  if (!g.enabled()) {
    ctx.setStatus("Quantize: the grid is off");
    return;
  }
  const std::vector<uint32_t> keep = sel_;
  if (apply(doc::ops::quantizeNotes(*ctx.doc(), ref_, keys, g), "Quantize") && keys.empty()) select(keep);
}

void RollState::toggleSelectionActive() {
  if (!clip_) return;
  const std::vector<doc::NoteKey> keys = selectionKeys();
  if (keys.empty()) return;
  bool anyEnabled = false;
  for (const doc::NoteKey& k : keys)
    if (const doc::Note* n = clip_->notes.find(k); n && n->enabled()) anyEnabled = true;
  (void)apply(doc::ops::setNotesProperty(*ctx.doc(), ref_, keys, doc::NoteProperty::Enabled, anyEnabled ? 0.0f : 1.0f),
              "Activate Notes");
}

// Q40 / T3.6-fix1: the rule lives in the document (doc::virtualFundamental: 1/lcm of the reference-anchored Ratio
// notes' effective denominators, the steps it remembers kept in the clip, or the clip's lock). The view only feeds
// it the lcm of the notes' own denominators from its per-chunk summaries, so an edit rescans just the rebuilt
// chunks (plus one pass over the notes while the clip has remembered steps).
std::optional<tuning::Rational> RollState::virtualFundamental() const {
  if (!clip_) return std::nullopt;
  if (vfClip_ == clip_.get()) return vf_;
  int64_t D = 0;
  for (const ChunkInfo& c : chunks_) {
    if (D < 0 || c.ratioDen == 0) continue;
    D = c.ratioDen < 0 ? -1 : (D ? std::lcm(D, c.ratioDen) : c.ratioDen);
    if (D > kMaxHarmonicDenominator) D = -1;
  }
  vfClip_ = clip_.get();
  vf_ = doc::virtualFundamental(*clip_, D);
  return vf_;
}

void RollState::stepSelection(int32_t steps, doc::ops::StepUnit unit) {
  if (!clip_) return;
  const std::vector<doc::NoteKey> keys = selectionKeys();
  if (keys.empty()) return;
  // Ratio notes step on the clip's virtual fundamental; the document remembers it with the step (T3.6-fix1).
  const doc::ops::EditResult e = doc::ops::transposeNotes(*ctx.doc(), ref_, keys, steps, unit, overlay());
  (void)apply(e, unit == doc::ops::StepUnit::Grid ? "Transpose" : "Transpose by Equave");
}

void RollState::nudgeSelection(int32_t cells) {
  if (!clip_ || cells == 0) return;
  const std::vector<doc::NoteKey> keys = selectionKeys();
  if (keys.empty()) return;
  Ticks first = INT64_MAX;
  for (const doc::Note& n : clip_->notes)
    if (selected(n.id)) first = std::min(first, n.start);
  // One grid cell from the earliest note's start (on the grid: the next line; off it: to the line), or a sixteenth
  // with the grid off.
  const Grid g = grid();
  Ticks to = first;
  for (int32_t i = 0; i < std::abs(cells); ++i) {
    if (!g.enabled()) to += (cells > 0 ? 1 : -1) * (kTicksPerWhole / 16);
    else to = cells > 0 ? g.lineAfter(to) : g.lineAtOrBefore(to - 1);
  }
  const Ticks dt = std::max<Ticks>(to, 0) - first;
  if (dt == 0) return;
  (void)apply(doc::ops::moveNotes(*ctx.doc(), ref_, keys, dt, 0, overlay()), "Move Notes");
}

void RollState::copySelection() {
  clipboard_.clear();
  if (!clip_) return;
  Ticks first = INT64_MAX;
  for (const doc::Note& n : clip_->notes)
    if (selected(n.id)) {
      clipboard_.push_back(n);
      first = std::min(first, n.start);
    }
  for (doc::Note& n : clipboard_) n.start -= first;
  if (!clipboard_.empty()) {
    Ticks end = 0;
    for (const doc::Note& n : clipboard_) end = std::max(end, n.start + n.length);
    insertTick_ = first + end;  // paste right after the copied notes unless the user clicks elsewhere
    ctx.setStatus(std::to_string(clipboard_.size()) + (clipboard_.size() == 1 ? " note copied" : " notes copied"));
  }
}

void RollState::cutSelection() {
  copySelection();
  deleteSelection();
}

void RollState::paste() {
  if (!clip_ || clipboard_.empty()) return;
  const Ticks at = insertTick_.value_or(0);
  std::vector<doc::Note> notes = clipboard_;
  Ticks span = 0;
  for (doc::Note& n : notes) {
    span = std::max(span, n.start + n.length);
    n.start += at;
  }
  if (apply(doc::ops::addNotes(*ctx.doc(), ctx.store().ids(), ref_, notes), "Paste Notes")) insertTick_ = at + span;
}

tuning::PitchId RollState::degreePitch(int64_t degree) const noexcept {
  const int32_t n = std::max(1, degreesPerEquave(tuning_));
  const int64_t e = floorDiv(degree, n);
  return tuning::PitchId::fromDegree(static_cast<int32_t>(degree - e * n), static_cast<int32_t>(e));
}

void RollState::addNotes(const std::vector<DrawnNote>& drawn) {
  if (!clip_ || drawn.empty()) return;
  std::vector<doc::Note> notes(drawn.size());
  for (size_t i = 0; i < drawn.size(); ++i) {
    notes[i].start = drawn[i].start;
    notes[i].length = drawn[i].length;
    notes[i].pitch = drawnPitch(drawn[i]);
  }
  (void)apply(doc::ops::addNotes(*ctx.doc(), ctx.store().ids(), ref_, notes), "Add Notes");
}

// --- drag ------------------------------------------------------------------------------------------------

void RollState::updatePreviewPitches() {
  // Recomputed only when the step count changes, and only over chunks that hold non-Degree notes (Degree notes
  // preview by row arithmetic in previewOf).
  if (drag.steps == drag.previewSteps) return;
  drag.previewSteps = drag.steps;
  drag.previewPitch.clear();
  if (!clip_ || drag.steps == 0) return;
  doc::ops::PitchStepContext pc;
  pc.tuning = &tuning_;
  pc.displayGrid = overlay();  // Cents notes move on the background EDO (T3.12)
  pc.fundamental = virtualFundamental();
  for (const ChunkInfo& c : chunks_) {
    if (c.nonDegree == 0) continue;
    for (uint32_t i = 0; i < c.count; ++i) {
      const doc::Note& n = c.notes[i];
      if (n.pitch.kind() == tuning::PitchKind::Degree || !selected(n.id)) continue;
      const std::optional<tuning::PitchId> p = doc::ops::stepPitch(n.pitch, drag.steps, doc::ops::StepUnit::Grid, pc);
      drag.previewPitch.emplace_back(n.id.value, p ? *p : n.pitch);
    }
  }
  std::sort(drag.previewPitch.begin(), drag.previewPitch.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });  // previewOf binary-searches by id
}

std::optional<RollState::Preview> RollState::previewOf(const DragState& d, const doc::Note& n) const {
  switch (d.kind) {
    case DragKind::Move:
    case DragKind::Copy:
    case DragKind::ResizeEnd:
    case DragKind::ResizeStart: break;
    default: return std::nullopt;
  }
  if (!selected(n.id)) return std::nullopt;
  Preview p{n.start, n.length, n.pitch};
  switch (d.kind) {
    case DragKind::Move:
    case DragKind::Copy:
      p.start = n.start + d.dt;
      if (d.steps != 0) {
        if (n.pitch.kind() == tuning::PitchKind::Degree) {
          p.pitch = degreePitch(absoluteDegree(tuning_, n.pitch.degree(), n.pitch.equave()) + d.steps);
        } else {
          auto it = std::lower_bound(d.previewPitch.begin(), d.previewPitch.end(), n.id.value,
                                     [](const auto& e, uint32_t id) { return e.first < id; });
          if (it != d.previewPitch.end() && it->first == n.id.value) p.pitch = it->second;
        }
      }
      break;
    case DragKind::ResizeEnd: p.length = std::max<Ticks>(1, n.length + d.dt); break;
    case DragKind::ResizeStart: {
      const Ticks end = n.start + n.length;
      p.start = std::min(n.start + d.dt, end - 1);
      p.length = end - p.start;
      break;
    }
    default: break;
  }
  return p;
}

float RollState::previewValue(const doc::Note& n) const noexcept {
  const float v = chance_ ? n.chance : n.velocity;
  if (drag.kind != DragKind::Velocity || !selected(n.id)) return v;
  return chance_ ? std::clamp(v + drag.dValue, 0.0f, 1.0f) : std::clamp(v + drag.dValue, 1.0f, 127.0f);
}

void RollState::beginDragTracking() {
  drag.tracked.clear();
  drag.damage = {};
  drag.trackDamage = clip_ && sel_.size() <= kMaxTrackedDragNotes;
  if (!drag.trackDamage) return;
  for (const doc::Note& n : clip_->notes)
    if (selected(n.id)) drag.tracked.push_back(n);
}

ui::IRect RollState::screenRectOf(Ticks start, Ticks length, const tuning::PitchId& pitch) const {
  const NotePlace pl = place(pitch);
  if (!pl.ok) return {};
  const float x0 = std::round(xOfTick(static_cast<double>(start)));
  const float x1 = std::max(x0 + 3.0f, std::round(xOfTick(static_cast<double>(start + length))));
  return {static_cast<int32_t>(x0) - 1, screenY(pl.top) - 1, static_cast<int32_t>(x1 - x0) + 2, pl.bottom - pl.top + 2};
}

void RollState::damageDrag() {
  ui::UiTree* tree = noteArea ? noteArea->tree() : nullptr;
  if (!tree || !drag.trackDamage) {
    markNotesDirty();
    return;
  }
  // The dragged notes where they are and where the preview draws them (a Copy shows both), plus last frame's box:
  // that covers everything the preview changed. Their stems' columns in the velocity lane likewise.
  auto unite = [](ui::IRect a, const ui::IRect& b) {
    if (b.empty()) return a;
    if (a.empty()) return b;
    const int32_t x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int32_t x1 = std::max(a.right(), b.right()), y1 = std::max(a.bottom(), b.bottom());
    return ui::IRect{x0, y0, x1 - x0, y1 - y0};
  };
  ui::IRect box{};
  for (const doc::Note& n : drag.tracked) {
    box = unite(box, screenRectOf(n.start, n.length, n.pitch));
    if (const std::optional<Preview> p = preview(n)) box = unite(box, screenRectOf(p->start, p->length, p->pitch));
  }
  for (const DrawnNote& d : drag.drawn) box = unite(box, screenRectOf(d.start, d.length, drawnPitch(d)));
  const ui::IRect all = unite(box, drag.damage);
  drag.damage = box;
  if (all.empty()) return;
  const float head = 6.0f * tree->scale();
  if (const ui::IRect a = intersect(all, area()); !a.empty()) tree->damage().add(a.toRect());
  if (laneView && laneView->visible()) {
    const ui::IRect l = laneView->rect();
    const ui::IRect col{all.x - static_cast<int32_t>(head), l.y, all.w + 2 * static_cast<int32_t>(head), l.h};
    if (const ui::IRect c = intersect(col, l); !c.empty()) tree->damage().add(c.toRect());
  }
}

void RollState::commitDrag() {
  DragState d = std::move(drag);
  drag = {};
  if (!clip_) return;
  const doc::Project& p = *ctx.doc();
  switch (d.kind) {
    case DragKind::Move:
      if (d.dt != 0 || d.steps != 0) {
        // Ratio notes moved by rows step by harmonics of their fundamental, exactly as Up/Down does.
        const std::vector<doc::NoteKey> keys = selectionKeys();
        (void)apply(doc::ops::moveNotes(p, ref_, keys, d.dt, d.steps, overlay()), "Move Notes");
      }
      break;
    case DragKind::Copy: {
      // A copy dropped back where it started (past the click slop, snapped to offset 0) would stack exact duplicates
      // on the originals: invisible and double-triggered. Nothing to do, as in the arrangement (M3 review M10).
      if (d.dt == 0 && d.steps == 0) break;
      std::vector<doc::Note> copies;
      for (const doc::Note& n : clip_->notes) {
        if (!selected(n.id)) continue;
        const std::optional<Preview> pv = previewOf(d, n);
        doc::Note c = n;
        if (pv) {
          c.start = pv->start;
          c.pitch = pv->pitch;
        }
        copies.push_back(c);
      }
      (void)apply(doc::ops::addNotes(p, ctx.store().ids(), ref_, copies), "Copy Notes");
      break;
    }
    case DragKind::ResizeEnd:
    case DragKind::ResizeStart:
      if (d.dt != 0)
        (void)apply(doc::ops::resizeNotes(p, ref_, selectionKeys(),
                                          d.kind == DragKind::ResizeEnd ? doc::ops::NoteEdge::End : doc::ops::NoteEdge::Start,
                                          d.dt),
                    "Resize Notes");
      break;
    case DragKind::Velocity:
      if (d.dValue != 0.0f) {
        const std::vector<uint32_t> keep = sel_;
        (void)apply(doc::ops::offsetNotesProperty(p, ref_, selectionKeys(),
                                                  chance_ ? doc::NoteProperty::Chance : doc::NoteProperty::Velocity, d.dValue),
                    chance_ ? "Change Chance" : "Change Velocity");
        if (sel_.empty()) select(keep);
      }
      break;
    case DragKind::Draw: addNotes(d.drawn); break;
    case DragKind::LoopBrace:
      if (d.dt != 0) {
        doc::ClipLoop l = clip_->loop;
        if (d.brace != BraceEdge::End) l.start += d.dt;
        if (d.brace != BraceEdge::Start) l.end += d.dt;
        if (l.end <= l.start) l.end = l.start + 1;
        if (Status s = ctx.store().dispatch(doc::SetClipLoop{ref_, l}); !s) ctx.setStatus("Loop: " + s.error().message);
      }
      break;
    case DragKind::StartMarker:
      if (d.dt != 0)
        if (Status s = ctx.store().dispatch(doc::SetClipBounds{ref_, clip_->start, clip_->length, clip_->contentStart + d.dt}); !s)
          ctx.setStatus("Start: " + s.error().message);
      break;
    case DragKind::EndMarker:
      if (d.dt != 0) {
        const std::vector<uint32_t> keep = sel_;
        const doc::ClipRef refs[] = {ref_};
        (void)apply(doc::ops::resizeClips(p, ctx.store().ids(), refs, doc::ops::ClipEdge::End, d.dt), "Resize Clip");
        select(keep);
      }
      break;
    default: break;
  }
  markAllDirty();
}

void RollState::abortDrag() {
  if (drag.kind == DragKind::Rubber) {
    std::vector<uint32_t> keep = std::move(drag.pressSelection);
    cancelDrag();
    select(std::move(keep));
    return;
  }
  cancelDrag();
}

void RollState::cancelDrag() {
  if (drag.kind == DragKind::None) return;
  drag = {};
  markAllDirty();
}

// --- audition --------------------------------------------------------------------------------------------

void RollState::audition(const tuning::PitchId& p, float velocity, Ticks contentStart, Ticks length) {
  if (!clip_) return;
  tuning::ResolveContext rc;
  if (p.kind() == tuning::PitchKind::Ratio && p.anchorKind() == tuning::AnchorKind::Note) {
    const std::optional<double> c = centsOf(p);
    if (!c) return;
    const double hz = tuning_.referenceHz() * std::exp2(*c / 1200.0);
    ctx.transport().auditionHz(static_cast<float>(hz), velocity, 300.0f);
    return;
  }
  const std::optional<double> hz = tuning::resolveHzDouble(p, tuning_, rc);
  if (!hz) return;
  // The note's own duration at the project tempo (where the clip plays it), 50 ms … 2 s.
  double ms = 300.0;
  if (snap_ && snap_->timeline) {
    const Ticks at = clip_->start - clip_->contentStart + contentStart;
    const TempoMap& tm = snap_->timeline->tempoMap;
    ms = 1000.0 * (tm.ticksToSeconds(at + length) - tm.ticksToSeconds(at));
  }
  ctx.transport().auditionHz(static_cast<float>(*hz), velocity, static_cast<float>(std::clamp(ms, 50.0, 2000.0)));
}

// --- repaint ---------------------------------------------------------------------------------------------

void RollState::markNotesDirty() {
  if (noteArea) noteArea->markDirty();
  if (laneView) laneView->markDirty();
}

void RollState::markAllDirty() {
  markNotesDirty();
  if (rulerView) rulerView->markDirty();
  if (keysView) keysView->markDirty();
  if (boxPanel) boxPanel->refresh();
  if (cornerPanel) cornerPanel->refresh();
  if (laneHeaderView) laneHeaderView->refresh();
}

}  // namespace pq::views::roll
