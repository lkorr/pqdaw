// Pitch paths and declaring JI in the piano roll (T7.4 / T7.13 / T7.14; docs/plans/M7.md "Rework: notes-first JI").
//
// The note is the only primitive (T7.13).
//   - pitch paths: a note whose pitch moves — a drawn curve, a bend, a vibrato (its curve record) or its group's
//     warps — is drawn along its pitch path instead of on a row (roll_paint.cpp paintCurveNotes: the same line as
//     every other note, T7.12). The path is the document's ONE evaluation (doc::effectiveNoteCurve) on the pitch the
//     note resolves to where it starts, so what is drawn is what plays; it is flattened once per clip snapshot
//     (RollState::pathOf, M7 review M14) and a paint only maps the cached points to the screen. pathCents is the
//     pitch of any note at any tick (hit-testing: noteAt), pathBox / pathPoint give a path note's screen extent.
//   - "Is Harmonic…" on a note (declareHarmonic → ops::makeJiGroup, or ops::reanchorJiGroup for a member): the note
//     becomes the frozen anchor of a JI group; the active group's harmonic series is drawn as guide lines
//     (RollState::harmonics), the plain draw puts members on it (DrawPitch::Ratio), and members carry their harmonic
//     numbers (RollState::harmonicOf).
//   - parseProportion ("8:9:10:12:15") and the inline text field (openJiField) of the harmonic, proportion and ratio
//     entries.
// The group UI (frames, Tune to JI, warps, walk, proportions, ratio entry) is roll_groups.cpp.
// Every edit is one doc::ops builder dispatched once: one undo step. Nothing here runs per frame but cache lookups.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>

#include "pq/document/automation_curve.h"
#include "pq/document/commands.h"
#include "pq/document/note_bend.h"
#include "pq/ui/overlay.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

namespace {

float scaleOf(const RollState& s) noexcept {
  return (s.noteArea && s.noteArea->tree()) ? s.noteArea->tree()->scale() : 1.0f;
}

Error invalid(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message), {}}; }

std::string_view trimmed(std::string_view t) {
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
  return t;
}

}  // namespace

// --- pitch paths ------------------------------------------------------------------------------------------------

float RollState::pathHalfThickness() const noexcept {
  const float sc = scaleOf(*this);
  const float lane = static_cast<float>(std::max(1, rows_.laneHeight()));
  return 0.5f * std::clamp(0.55f * lane, 3.0f * sc, 9.0f * sc);
}

void RollState::fitPathTolerance(float maxPx) noexcept {
  // Device px per cent over an octave of the rows (a Proportional / Equidistant layout's rows differ; this is their
  // mean, the uniform layouts' exact slope).
  double pxPerCent = 0.0;
  if (!rows_.empty()) pxPerCent = std::fabs(rows_.yOfCents(1200.0) - rows_.yOfCents(0.0)) / 1200.0;
  // Fitted again only when the rows changed scale by more than 2× since the last fit: a tuning change (a MOS morph
  // frame) moves the rows a little and must not flatten every path again (T7.16a M16); a vertical zoom does.
  if (fittedPxPerCent_ > 0.0 && pxPerCent < 2.0 * fittedPxPerCent_ && pxPerCent > 0.5 * fittedPxPerCent_) return;
  fittedPxPerCent_ = pxPerCent;
  double tol = kPathToleranceCents;
  while (tol > kMinPathToleranceCents && tol * pxPerCent > static_cast<double>(maxPx)) tol *= 0.5;
  pathTol_ = tol;
}

float RollState::yOfCents(double cents) const noexcept {
  if (rows_.empty()) return static_cast<float>(area().y);
  return static_cast<float>(static_cast<double>(area().y - scrollY_) + rows_.yOfCents(cents));
}

void RollState::syncPaths() const {
  const void* tl = snap_ && snap_->timeline ? static_cast<const void*>(snap_->timeline.get()) : nullptr;
  const bool timeline = paths_.timeline != tl;
  const bool tuning = paths_.tuningGen != tuningGen_;
  if (paths_.clip == clip_.get() && !timeline && !tuning) return;
  previewPaths_.clear();  // (built against the old snapshot)
  if (timeline || !clip_ || paths_.notes.size() > (size_t{1} << 16)) {
    paths_.notes.clear();  // every tick moved (a vibrato's period in ticks)
  } else {
    // A new snapshot of the clip or a new tuning: an entry stays while what it was flattened from stands. Entries
    // that read the resolved pitches go (a neighbour may have moved); when the curve list was replaced, an entry stays
    // if its record is equal in the new list (the old list is still alive here: paths_.curves holds it). T7.16a (M7
    // review M16): a tuning change (a MOS morph frame) keeps the other entries' points — they are cents over the
    // note, the tuning does not enter them — and only their `base` is derived again (pathOf, baseGen).
    const bool listChanged = clip_->curves.get() != paths_.curves.get();
    for (auto it = paths_.notes.begin(); it != paths_.notes.end();) {
      NotePath& e = it->second;
      bool keep = e.ok && !e.usedIndex && e.rec != nullptr;
      if (keep && listChanged) {
        const doc::NoteCurves* now = clip_->curves ? doc::findNoteCurves(*clip_, doc::NoteId{it->first}) : nullptr;
        keep = now != nullptr && *e.rec == *now;
        if (keep) e.rec = now;
      }
      it = keep ? std::next(it) : paths_.notes.erase(it);
    }
  }
  paths_.curves = clip_ ? clip_->curves : nullptr;
  paths_.clip = clip_.get();
  paths_.timeline = tl;
  paths_.tuningGen = tuningGen_;
  paths_.resolved = false;
  paths_.anyWarp = false;
  if (clip_)
    for (const doc::JiGroup& g : doc::jiGroupsOf(*clip_)) paths_.anyWarp = paths_.anyWarp || !doc::effectiveJiWarps(g).empty();
}

void RollState::touchPreview() {
  ++previewGen_;
  previewPaths_.clear();
}

const doc::NotePitchIndex& RollState::pitchIndex() const {
  syncPaths();
  if (!paths_.resolved) {
    paths_.resolved = true;
    paths_.pitches = {};
    if (clip_ && snap_)
      if (const std::optional<doc::TrackPath> path = doc::findTrack(*snap_, ref_.track))
        paths_.pitches = doc::resolveClipPitches(*snap_, *path, *clip_);
    paths_.index = doc::NotePitchIndex(paths_.pitches);
  }
  return paths_.index;
}

bool RollState::previewTouches(const doc::Note& n) const {
  if (!curvePreview_.empty()) {
    const auto it = std::lower_bound(curvePreview_.begin(), curvePreview_.end(), n.id,
                                     [](const doc::NoteCurves& c, doc::NoteId id) { return c.note < id; });
    if (it != curvePreview_.end() && it->note == n.id) return true;
  }
  if (warpPreview_.valid() && warpPreview_.clip)
    if (const doc::JiGroup* g = doc::findJiGroup(*warpPreview_.clip, warpPreview_.group))
      return doc::noteAnchorOf(n.pitch) == g->anchor && doc::findNoteWritten(*clip_, n.id) != nullptr;
  return false;
}

bool RollState::onPath(const doc::Note& n, const tuning::PitchId&) const {
  if (!clip_) return false;
  if (previewTouches(n)) return true;
  if (clip_->curves && doc::findNoteCurves(*clip_, n.id) != nullptr) return true;
  syncPaths();
  if (anchorMoved(n)) return true;
  return paths_.anyWarp && (doc::noteAnchorOf(n.pitch).valid() || groupRole(n) == 2) && pitchIndex().warped(n.id) != nullptr;
}

bool RollState::anchorMoved(const doc::Note& n) const {
  // (Only a curve or a warp somewhere in the clip can move an anchor.)
  if (!clip_ || !(clip_->curves || paths_.anyWarp) || !doc::noteAnchorOf(n.pitch).valid()) return false;
  const std::optional<double> hz = pitchIndex().hz(n.id);
  const std::optional<double> still = centsOfImpl(n.pitch, false);  // its anchor chain standing still (no drag preview)
  if (!hz || !still || !(tuning_.referenceHz() > 0.0)) return false;
  return std::fabs(1200.0 * std::log2(*hz / tuning_.referenceHz()) - *still) > 1e-6;
}

const RollState::NotePath* RollState::pathOf(const doc::Note& n) const {
  if (!clip_) return nullptr;
  const bool preview = previewTouches(n);
  const doc::NoteCurves* rec = curveRecord(n.id);
  // One evaluation for the document's notes and a gesture's preview alike.
  // The clip's resolved pitches are needed only by a note that bends to a neighbour, that a warp moves, or that is
  // anchored to another note (it starts where that note's curve is, Q293): a drawn curve or a vibrato on a plain note
  // costs no resolve of the clip.
  auto build = [&](const doc::NoteClip& clip, const doc::NotePitchIndex* index, NotePath& out) {
    out.pts.clear();
    out.ok = false;
    out.rec = rec;
    out.start = n.start;
    out.length = n.length;
    out.pitch = n.pitch;
    out.usedIndex = index != nullptr;
    out.baseGen = tuningGen_;
    std::optional<double> base;
    if (const std::optional<double> hz = index ? index->hz(n.id) : std::nullopt; hz && tuning_.referenceHz() > 0.0) base = 1200.0 * std::log2(*hz / tuning_.referenceHz());
    else base = centsOf(n.pitch);
    if (!base || !std::isfinite(*base)) return;
    out.base = *base;
    out.ok = true;
    doc::NoteBendTargets targets = rec && index ? doc::noteBendTargets(*rec, *index) : doc::NoteBendTargets{};
    if (rec && rec->vibrato.set()) targets.vibratoPeriod = doc::vibratoPeriodTicks(rec->vibrato, ticksPerSecond(n.start));
    const doc::NoteWarp warp = index && index->anyWarp() ? index->warp(clip, n.id) : doc::NoteWarp{};
    // bug-bend-shape: a pen stroke lengthening the note previews the curve over its new length (its bend out there).
    const bool grows = preview && drag.kind == DragKind::PenDraw && drag.curve.note == n.id && drag.penLength > n.length;
    const Ticks L = std::max<Ticks>(grows ? drag.penLength : n.length, 1);
    out.tol = pathTol_;
    if (rec || warp.any()) out.pts = doc::effectiveNoteCurve(rec, L, targets, warp, pathTol_);
    out.curved = std::any_of(out.pts.begin(), out.pts.end(), [](const doc::Breakpoint& b) { return b.curve != 0.0f; });
    // bug-curve-render: the curve without its vibrato, for the band a zoomed-out paint draws instead of the wave.
    out.rest.clear();
    out.restCurved = false;
    out.vib = {};
    out.vibPeriod = 0.0;
    if (rec && targets.vibratoPeriod > 0.0) {
      out.vib = doc::VibratoEnvelope(rec->vibrato, L);
      if (out.vib.any()) {
        out.vibPeriod = std::max(targets.vibratoPeriod, doc::kMinVibratoPeriodTicks);
        doc::NoteCurves still = *rec;
        still.vibrato = {};
        doc::NoteBendTargets t = targets;
        t.vibratoPeriod = 0.0;
        out.rest = doc::effectiveNoteCurve(&still, L, t, warp, pathTol_);
        out.restCurved = std::any_of(out.rest.begin(), out.rest.end(), [](const doc::Breakpoint& b) { return b.curve != 0.0f; });
      }
    }
    out.lo = out.hi = 0.0f;
    for (const doc::Breakpoint& b : out.pts) {
      out.lo = std::min(out.lo, b.value);
      out.hi = std::max(out.hi, b.value);
    }
    ++pathBuilds_;
  };
  syncPaths();
  // T7.16a (M7 review M16): per note — a warp in the clip makes only the notes it moves read the index.
  // (Only a member or an anchor can be warped: a plain note costs no resolve of the clip.)
  const bool warpedNote = paths_.anyWarp && (doc::noteAnchorOf(n.pitch).valid() || groupRole(n) == 2) && pitchIndex().warped(n.id) != nullptr;
  const bool needIndex = warpedNote || (rec && (rec->bendIn.set() || rec->bendOut.set())) || doc::noteAnchorOf(n.pitch).valid();
  if (preview) {
    // One flattening per note and preview (M15): the entry stands until the preview changes (previewGen_).
    NotePath& e = previewPaths_[n.id.value];
    if (e.previewGen != previewGen_ || e.rec != rec || e.start != n.start || e.length != n.length || !(e.pitch == n.pitch) || e.baseGen != tuningGen_ ||
        e.tol > 2.0 * pathTol_) {
      const bool warped = warpPreview_.valid() && warpPreview_.clip != nullptr;
      const bool index = needIndex || (warped && warpPreview_.index.warped(n.id) != nullptr);
      build(warped ? *warpPreview_.clip : *clip_, warped ? &warpPreview_.index : index ? &pitchIndex() : nullptr, e);
      e.previewGen = previewGen_;
    }
    return e.ok ? &e : nullptr;
  }
  // A note anchored to a note whose curve moved it (Q293: it starts where that note's curve is) is drawn where it
  // sounds, on a path with no points of its own.
  if (!rec && !warpedNote && !anchorMoved(n)) return nullptr;
  auto it = paths_.notes.find(n.id.value);
  if (it == paths_.notes.end()) {
    it = paths_.notes.emplace(n.id.value, NotePath{}).first;
    build(*clip_, needIndex ? &pitchIndex() : nullptr, it->second);
  } else if (const NotePath& e = it->second;
             e.rec != rec || e.start != n.start || e.length != n.length || !(e.pitch == n.pitch) || e.usedIndex != needIndex || e.tol > 2.0 * pathTol_) {
    build(*clip_, needIndex ? &pitchIndex() : nullptr, it->second);  // the note changed under a kept entry (or the rows grew past its tolerance)
  } else if (e.baseGen != tuningGen_) {
    // Only the tuning moved (M16): the points stand, the pitch it starts on is derived again.
    NotePath& k = it->second;
    const std::optional<double> base = centsOf(n.pitch);
    k.ok = base && std::isfinite(*base);
    if (k.ok) k.base = *base;
    k.baseGen = tuningGen_;
  }
  return it->second.ok ? &it->second : nullptr;
}

std::optional<double> RollState::startCents(const doc::Note& n) const {
  if (const NotePath* p = pathOf(n)) return p->base;
  return clip_ ? centsOf(n.pitch) : std::nullopt;
}

double RollState::anchorPreviewShift(const doc::Note& n) const {
  if (!drag.movesAnchors || drag.kind != DragKind::Move || !doc::noteAnchorOf(n.pitch).valid()) return 0.0;
  const std::optional<double> shown = centsOfImpl(n.pitch, true), doc = centsOfImpl(n.pitch, false);
  return shown && doc ? *shown - *doc : 0.0;
}

std::optional<double> RollState::pathCents(const doc::Note& n, Ticks, const tuning::PitchId& pitch, double offset) const {
  if (!clip_) return std::nullopt;
  const NotePath* p = pathOf(n);
  if (!p) return centsOf(pitch);
  // A drag's preview pitch: its identity's pitch, with the note's curve.
  std::optional<double> base = pitch == n.pitch ? std::optional<double>(p->base) : centsOf(pitch);
  if (!base) return std::nullopt;
  if (pitch == n.pitch) *base += anchorPreviewShift(n);  // M14 (T7.16a): its anchor is being dragged
  if (!p->pts.empty()) *base += pq::evalAutomation(std::span<const doc::Breakpoint>(p->pts), offset);
  return base;
}

double RollState::harmonicsShift(const Harmonics& h, double t) const {
  const doc::Note* a = h.anchor.valid() && clip_ ? findNote(h.anchor) : nullptr;
  if (!a || !onPath(*a, a->pitch)) return 0.0;  // a steady anchor: the guides are straight
  const CurveTarget ct{a->id};
  const std::span<const doc::Breakpoint> free = curvePoints(ct);
  auto steady = [&](double off) -> std::optional<double> {
    std::optional<double> c = curveBase(ct, off);  // its bends and warp there
    if (c && !free.empty()) *c += pq::evalAutomation(free, off);
    return c;
  };
  const double len = static_cast<double>(std::max<Ticks>(a->length, 1));
  const std::optional<double> at = steady(std::clamp(t - static_cast<double>(a->start), 0.0, len)), at0 = steady(0.0);
  return at && at0 ? *at - *at0 : 0.0;
}

std::optional<ui::Vec2> RollState::pathPoint(const doc::Note& n, double frac) const {
  if (!onPath(n, n.pitch)) return std::nullopt;
  const double off = std::clamp(frac, 0.0, 1.0) * static_cast<double>(n.length);
  const std::optional<double> c = pathCents(n, n.start, n.pitch, off);
  if (!c) return std::nullopt;
  return ui::Vec2{xOfTick(static_cast<double>(n.start) + off), yOfCents(*c)};
}

ui::IRect RollState::pathBox(const doc::Note& n, Ticks start, Ticks length, const tuning::PitchId& pitch) const {
  const float x0 = std::round(xOfTick(static_cast<double>(start)));
  const float x1 = std::max(x0 + 3.0f, std::round(xOfTick(static_cast<double>(start + length))));
  const NotePath* p = pathOf(n);
  const std::optional<double> base = p && pitch == n.pitch ? std::optional<double>(p->base) : centsOf(pitch);
  if (!base) return {};
  // The extent of the cached points (segments are straight between them; a stored free curve's bow stays inside
  // its end values).
  const double lo = p ? static_cast<double>(p->lo) : 0.0, hi = p ? static_cast<double>(p->hi) : 0.0;
  const float ya = yOfCents(*base + hi), yb = yOfCents(*base + lo);
  const float h = pathHalfThickness() + 2.0f;
  return {static_cast<int32_t>(x0) - 1, static_cast<int32_t>(std::floor(ya - h)), static_cast<int32_t>(x1 - x0) + 2,
          static_cast<int32_t>(std::ceil(yb + h)) - static_cast<int32_t>(std::floor(ya - h))};
}

// --- declaring JI -----------------------------------------------------------------------------------------------

DrawPitch RollState::drawPitchFor(ui::Mods m, Ticks t, float y) const {
  // T7.16a (M7 review H1 / H3 / M5): a note drawn where it joins a group (inside the group's time span, the Harmonics
  // switch on) snaps to the group's harmonics — with Alt to the finer series between them — and Shift then draws on
  // the rows. Elsewhere the plain draw is on the rows, Shift draws a reference ratio, Alt on the background EDO.
  const bool group = captureGroupAt(t, y) != nullptr;
  if (m & mod::kAlt) return group ? DrawPitch::Ratio : DrawPitch::Overlay;
  return (((m & mod::kShift) != 0) != group) ? DrawPitch::Ratio : DrawPitch::Row;
}

Result<std::vector<int64_t>> RollState::parseProportion(std::string_view text) {
  std::vector<int64_t> out;
  size_t pos = 0;
  if (trimmed(text).empty()) return invalid("type a proportion such as 4:5:6");
  while (pos <= text.size()) {
    const size_t colon = text.find(':', pos);
    const std::string_view part = trimmed(text.substr(pos, colon == std::string_view::npos ? std::string_view::npos : colon - pos));
    pos = colon == std::string_view::npos ? text.size() + 1 : colon + 1;
    int64_t term = 0;
    const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), term);
    if (part.empty() || ec != std::errc{} || end != part.data() + part.size())
      return invalid("\"" + std::string(part) + "\" is not a whole number: type a proportion such as 4:5:6");
    if (term < 1 || term > doc::kMaxFundamentalDenominator)
      return invalid("terms go from 1 to " + std::to_string(doc::kMaxFundamentalDenominator));
    out.push_back(term);
  }
  if (out.size() > doc::ops::kMaxProportionTerms) return invalid("at most " + std::to_string(doc::ops::kMaxProportionTerms) + " terms");
  for (size_t i = 1; i < out.size(); ++i)
    if (out[i] <= out[i - 1]) return invalid("terms must ascend, such as 4:5:6");
  return out;
}

bool RollState::declareHarmonic(doc::NoteId note, uint32_t n, std::string* error) {
  auto fail = [&](std::string m) {
    ctx.setStatus("Is Harmonic: " + m);
    if (error) *error = std::move(m);
    return false;
  };
  if (!clip_) return fail("no clip");
  const doc::Note* nt = findNote(note);
  if (!nt) return fail("no such note");
  if (n < 1 || n > doc::kMaxHarmonicIndex) return fail("type a harmonic number from 1 to 4096");
  if (!pathCents(*nt, nt->start, nt->pitch, 0.0)) return fail("the note's pitch cannot be resolved");
  std::vector<uint32_t> keep = sel_;
  if (keep.empty()) keep.push_back(note.value);  // the declared note's group is the active one
  // A member of a group: it becomes that group's anchor (Q297 b) — no second, chained group from the menu.
  const doc::JiGroup* own = doc::findJiGroupByAnchor(*clip_, note);
  const doc::JiGroup* of = own ? nullptr : doc::jiGroupOfNote(*clip_, *nt);
  const doc::ops::EditResult e = of ? doc::ops::reanchorJiGroup(*ctx.doc(), ref_, of->id, doc::keyOf(*nt), n)
                                    : doc::ops::makeJiGroup(*ctx.doc(), ctx.store().ids(), ref_, doc::keyOf(*nt), n);
  if (!e) return fail(e.error().message);
  if (!e->empty() && !apply(e, "Is Harmonic")) return fail("the document refused the harmonic");
  select(std::move(keep));
  if (const doc::JiGroup* g = activeJiGroup(); !g || g->anchor != note) selectOnly(note);  // its group is the active one
  setShowHarmonics(true);
  char buf[160];
  std::snprintf(buf, sizeof buf, "The note is harmonic %u: it stays where it is; draw on the guide lines over it to add notes of its series (Alt: between them; Shift: the rows)", n);
  ctx.setStatus(buf);
  return true;
}

bool RollState::removeActiveJiGroup() {
  const doc::JiGroup* g = activeJiGroup();
  if (!g) return false;
  const std::vector<uint32_t> keep = sel_;
  if (!apply(doc::ops::removeJiGroup(*ctx.doc(), ref_, g->id), "Remove JI Group")) return false;
  select(keep);
  return true;
}

// --- inline fields ----------------------------------------------------------------------------------------------

ui::TextEdit* RollState::openJiField(std::string_view name, std::string_view title, std::string_view info, std::string_view initial,
                                     ui::IRect anchor, std::function<bool(RollState&, const std::string&, std::string&)> commit) {
  if (!clip_ || !noteArea) return nullptr;
  ui::OverlayLayer* layer = ui::OverlayLayer::of(*noteArea);
  if (!layer) return nullptr;
  anchor = intersect(anchor, area());
  if (anchor.empty()) anchor = {area().x + 8, area().y + 8, 1, 1};
  auto pop = std::make_unique<ui::Popover>(layer->uniqueName(name));
  ui::Popover* popPtr = pop.get();
  pop->setSize(ui::Length::fixed(280), ui::Length::fixed(78));
  pop->setInfo(std::string(title), std::string(info));
  pop->add<ui::Label>("title", std::string(title), ui::FontRoleId::Small, ui::TextTone::Secondary).setHeight(ui::Length::fixed(16));
  auto& edit = pop->add<ui::TextEdit>("value");
  edit.setHeight(ui::Length::fixed(20));
  if (!initial.empty()) edit.setText(std::string(initial));
  auto& err = pop->add<ui::Label>("error", "", ui::FontRoleId::Small, ui::TextTone::Primary);
  err.setHeight(ui::Length::fixed(16));
  std::shared_ptr<RollState*> alive = self_;
  ui::TextEdit* editPtr = &edit;
  ui::Label* errPtr = &err;
  edit.onEnter = [alive, layer, popPtr, editPtr, errPtr, commit = std::move(commit)] {
    RollState* s = *alive;
    if (!s) return;
    std::string why;
    const std::string text = editPtr->text();
    if (!commit(*s, text, why)) {
      errPtr->setText(why);  // the field stays open; nothing changed
      return;
    }
    if (layer->isOpen(*popPtr)) layer->close(*popPtr);
    // The keys go back to the notes.
    if (RollState* st = *alive; st && st->noteArea && st->noteArea->tree()) st->noteArea->tree()->setFocus(st->noteArea);
  };
  edit.onCancel = [alive, layer, popPtr] {
    if (layer->isOpen(*popPtr)) layer->close(*popPtr);
    if (RollState* st = *alive; st && st->noteArea && st->noteArea->tree()) st->noteArea->tree()->setFocus(st->noteArea);
  };
  ui::OverlayOptions o;
  o.anchor = anchor;
  o.placement = ui::Placement::Below;
  layer->open(std::move(pop), std::move(o));
  if (ui::UiTree* t = layer->tree()) {
    t->layoutNow();
    t->setFocus(&edit);
    if (!initial.empty()) edit.buffer().selectAll();
  }
  return &edit;
}

ui::TextEdit* RollState::openHarmonicEntry(doc::NoteId note) {
  const doc::Note* n = findNote(note);
  if (!n) return nullptr;
  ui::IRect anchor = onPath(*n, n->pitch) ? pathBox(*n, n->start, n->length, n->pitch) : screenRectOf(n->start, n->length, n->pitch);
  return openJiField("harmonic-entry", "This note is harmonic number:",
                     "Type N and press Enter: this note becomes harmonic N of a series and stays exactly where it is. The "
                     "series is drawn as guide lines over its time; notes you draw there snap to it as exact ratios to this "
                     "note (Shift draws on the rows; Alt shows the lines between the harmonics). A note placed between the "
                     "lines renumbers the series (5 and a 3/2 above it read 10 and 15). On a note that already belongs to a group, that note becomes the group's frozen note and the "
                     "others keep their pitches. Remove JI Group is in the same menu. Esc closes.",
                     "", anchor, [note](RollState& s, const std::string& text, std::string& why) {
                       const std::string_view t = trimmed(text);
                       uint32_t k = 0;
                       const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), k);
                       if (t.empty() || ec != std::errc{} || end != t.data() + t.size()) {
                         why = "type a whole number such as 5";
                         return false;
                       }
                       return s.declareHarmonic(note, k, &why);
                     });
}

}  // namespace pq::views::roll
