// Note edit operations and identity-aware pitch steps (T3.6).

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "edit_common.h"
#include "pq/document/pitch_resolve.h"
#include "pq/tuning/ji.h"

namespace pq::doc::ops {

using namespace detail;
using tuning::PitchId;
using tuning::PitchKind;
using tuning::Rational;

namespace {

constexpr double kOnLineCents = 1e-6;

int64_t floorDiv(int64_t a, int64_t b) {
  const int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

bool fitsI32(int64_t v) {
  return v >= std::numeric_limits<int32_t>::min() && v <= std::numeric_limits<int32_t>::max();
}

// The degree lattice of a tuning (or of an EDO display grid): degree d of equave e sits at pitch(d) + e·equave
// cents (stretch included for MOS).
struct DegreeLattice {
  const tuning::Edo* edo = nullptr;
  const tuning::Mos* mos = nullptr;
  const tuning::Scale* scale = nullptr;
  int32_t size = 0;
  double equave = 0.0;

  static DegreeLattice of(const tuning::Tuning& t) {
    DegreeLattice l;
    if (const tuning::Edo* e = t.edo()) return ofEdo(*e);
    if ((l.mos = t.mos())) {
      l.size = l.mos->size();
      l.equave = l.mos->centsOf(0, 1) - l.mos->centsOf(0, 0);
    } else if ((l.scale = t.scale())) {
      l.size = l.scale->size();
      l.equave = l.scale->equaveCents();
    }
    return l;
  }
  static DegreeLattice ofEdo(const tuning::Edo& e) {
    DegreeLattice l;
    l.edo = &e;
    l.size = static_cast<int32_t>(e.n());
    l.equave = e.equaveCents();
    return l;
  }
  // Cents of degree d (0 ≤ d < size) in equave 0.
  double pitch(int32_t d) const {
    if (edo) return edo->centsOfStep(d);
    if (mos) return mos->centsOf(d, 0);
    return scale->centsOf(d, 0);
  }
  double centsOf(int64_t absDegree) const {
    const int64_t e = floorDiv(absDegree, size);
    return pitch(static_cast<int32_t>(absDegree - e * size)) + static_cast<double>(e) * equave;
  }
  // The next lattice line strictly above (up) / below c (lines within kOnLineCents of c count as c).
  double next(double c, bool up) const {
    double best = up ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
    for (int32_t d = 0; d < size; ++d) {
      const double p = pitch(d);
      if (up) {
        const double k = std::floor((c + kOnLineCents - p) / equave) + 1.0;
        best = std::min(best, p + k * equave);
      } else {
        const double k = std::ceil((c - kOnLineCents - p) / equave) - 1.0;
        best = std::max(best, p + k * equave);
      }
    }
    return best;
  }
  // The absolute degree nearest to c.
  int64_t nearest(double c) const {
    const int64_t e = static_cast<int64_t>(std::floor(c / equave));
    int64_t bestDeg = 0;
    double bestDist = std::numeric_limits<double>::infinity();
    for (int64_t ee = e - 1; ee <= e + 1; ++ee)
      for (int32_t d = 0; d < size; ++d) {
        const double dist = std::abs(pitch(d) + static_cast<double>(ee) * equave - c);
        if (dist < bestDist) {
          bestDist = dist;
          bestDeg = ee * size + d;
        }
      }
    return bestDeg;
  }
  bool usable() const { return size > 0 && equave > 0.0 && std::isfinite(equave); }
};

// The tuning's equave as an exact ratio, if it has one (see stepPitch).
std::optional<Rational> equaveRatio(const tuning::Tuning& t) {
  double cents = 0.0;
  if (const tuning::Edo* e = t.edo()) {
    if (auto r = e->equaveRatio()) return r;
    cents = e->equaveCents();
  } else if (const tuning::Scale* s = t.scale()) {
    const tuning::ScalaPitch& last = s->source().pitches.back();
    if (last.kind == tuning::ScalaPitch::Kind::Ratio) return last.ratio;
    cents = s->equaveCents();
  } else if (const tuning::Mos* m = t.mos()) {
    cents = m->centsOf(0, 1) - m->centsOf(0, 0);
  }
  for (int64_t q = 1; q <= 64; ++q)
    for (int64_t pn = q + 1; pn <= 64; ++pn) {
      if (std::gcd(pn, q) != 1) continue;
      if (std::abs(1200.0 * std::log2(static_cast<double>(pn) / static_cast<double>(q)) - cents) < 1e-3)
        return Rational(pn, q);
    }
  return std::nullopt;
}

// Distinct keys (a key listed twice would make UpdateNotes fail).
std::vector<NoteKey> distinct(std::span<const NoteKey> keys) {
  std::vector<NoteKey> v(keys.begin(), keys.end());
  std::sort(v.begin(), v.end(), [](const NoteKey& a, const NoteKey& b) { return a.id < b.id; });
  v.erase(std::unique(v.begin(), v.end(), [](const NoteKey& a, const NoteKey& b) { return a.id == b.id; }), v.end());
  return v;
}

// Applies f(Note&) to each addressed note (f returns false to fail with `why`), emitting one UpdateNotes.
template <class F>
EditResult updateNotes(const Project& p, ClipRef clip, std::span<const NoteKey> keys, std::string label, F&& f) {
  Result<ClipFound> found = findClip(p, clip);
  if (!found) return std::move(found).error();
  const NoteSeq& seq = found->clip->notes;
  Edit e = makeEdit(std::move(label));
  e.noteClip = clip;
  UpdateNotes u{clip, {}};
  for (const NoteKey& k : distinct(keys)) {
    const Note* n = seq.locate(k);
    if (!n) return notFound("note not found");
    Note m = *n;
    PQ_DOC_TRY(f(m));
    PQ_DOC_TRY(checkTick(m.start, "note start"));
    if (!(m == *n)) u.updates.push_back({keyOf(*n), m});
    e.notes.push_back(keyOf(m));
  }
  if (!u.updates.empty()) batchOf(e).commands.push_back(std::move(u));
  return e;
}

float clampProperty(NoteProperty prop, float v) {
  switch (prop) {
    case NoteProperty::Velocity: return std::clamp(v, 1.0f, 127.0f);
    case NoteProperty::ReleaseVelocity: return std::clamp(v, 0.0f, 127.0f);
    case NoteProperty::Chance: return std::clamp(v, 0.0f, 1.0f);
    case NoteProperty::VelocityRange: return std::clamp(v, -127.0f, 127.0f);
    case NoteProperty::Enabled: return v != 0.0f ? 1.0f : 0.0f;
  }
  return v;
}

float propertyOf(const Note& n, NoteProperty prop) {
  switch (prop) {
    case NoteProperty::Velocity: return n.velocity;
    case NoteProperty::ReleaseVelocity: return n.releaseVelocity;
    case NoteProperty::Chance: return n.chance;
    case NoteProperty::VelocityRange: return n.velocityRange;
    case NoteProperty::Enabled: return n.enabled() ? 1.0f : 0.0f;
  }
  return 0.0f;
}

void setProperty(Note& n, NoteProperty prop, float v) {
  switch (prop) {
    case NoteProperty::Velocity: n.velocity = v; break;
    case NoteProperty::ReleaseVelocity: n.releaseVelocity = v; break;
    case NoteProperty::Chance: n.chance = v; break;
    case NoteProperty::VelocityRange: n.velocityRange = v; break;
    case NoteProperty::Enabled:
      n.flags = v != 0.0f ? (n.flags & ~static_cast<uint32_t>(kNoteDisabled)) : (n.flags | kNoteDisabled);
      break;
  }
}

std::string_view propertyLabel(NoteProperty prop) {
  switch (prop) {
    case NoteProperty::Velocity: return "Note Velocity";
    case NoteProperty::ReleaseVelocity: return "Note Release Velocity";
    case NoteProperty::Chance: return "Note Chance";
    case NoteProperty::VelocityRange: return "Note Velocity Range";
    case NoteProperty::Enabled: return "Activate Notes";
  }
  return "Note Property";
}

}  // namespace

std::optional<PitchId> stepPitch(const PitchId& pitch, int32_t steps, StepUnit unit, const PitchStepContext& ctx) {
  if (steps == 0) return pitch;
  switch (pitch.kind()) {
    case PitchKind::Degree: {
      if (unit == StepUnit::Equave) {
        const int64_t e = static_cast<int64_t>(pitch.equave()) + steps;
        if (!fitsI32(e)) return std::nullopt;
        return PitchId::fromDegree(pitch.degree(), static_cast<int32_t>(e));
      }
      if (!ctx.tuning) return std::nullopt;
      const DegreeLattice l = DegreeLattice::of(*ctx.tuning);
      if (l.size <= 0) return std::nullopt;
      const int64_t abs = static_cast<int64_t>(pitch.equave()) * l.size + pitch.degree() + steps;
      const int64_t e = floorDiv(abs, l.size);
      if (!fitsI32(e)) return std::nullopt;
      return PitchId::fromDegree(static_cast<int32_t>(abs - e * l.size), static_cast<int32_t>(e));
    }
    case PitchKind::Cents: {
      if (!ctx.tuning && !(unit == StepUnit::Grid && ctx.displayGrid)) return std::nullopt;
      if (unit == StepUnit::Equave) {
        const DegreeLattice l = DegreeLattice::of(*ctx.tuning);
        if (!l.usable()) return std::nullopt;
        return PitchId::fromCents(pitch.cents() + static_cast<double>(steps) * l.equave);
      }
      const DegreeLattice l = ctx.displayGrid ? DegreeLattice::ofEdo(*ctx.displayGrid) : DegreeLattice::of(*ctx.tuning);
      if (!l.usable()) return std::nullopt;
      double c = pitch.cents();
      for (int32_t i = 0; i < std::abs(steps); ++i) c = l.next(c, steps > 0);
      if (!std::isfinite(c)) return std::nullopt;
      return PitchId::fromCents(c);
    }
    case PitchKind::Ratio: {
      const Rational r = pitch.ratio();
      std::optional<Rational> out;
      if (unit == StepUnit::Equave) {
        if (!ctx.tuning) return std::nullopt;
        const auto eq = equaveRatio(*ctx.tuning);
        if (!eq) return std::nullopt;
        const auto f = tuning::pow(*eq, steps);
        if (!f) return std::nullopt;
        out = tuning::mul(r, *f);
      } else {
        // The fundamental both the note and the clip's fundamental are harmonics of: gcd(n/d, F) (own: 1/d).
        Rational f = Rational(1, r.den());
        std::optional<Rational> on;  // the fundamental the note steps on with its own
        if (pitch.anchorKind() == tuning::AnchorKind::Reference) {
          on = ctx.fundamental;
        } else if (!ctx.groups.empty()) {  // T7.13: a member of a JI group: the group's harmonics
          const auto it = std::lower_bound(ctx.groups.begin(), ctx.groups.end(), NoteId{pitch.anchorId()},
                                           [](const GroupStepFundamental& g, NoteId id) { return g.anchor < id; });
          if (it != ctx.groups.end() && it->anchor.value == pitch.anchorId()) on = it->fundamental;
        }
        if (on) {
          const Rational both[2] = {r, *on};
          const std::optional<Rational> g = tuning::virtualFundamental(both);
          if (!g) return std::nullopt;
          f = *g;
        }
        const std::optional<uint64_t> m = tuning::harmonicNumber(r, f);
        if (!m || *m > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
        int64_t m2 = 0;
        if (tuning::detail::addOverflows(static_cast<int64_t>(*m), steps, m2) || m2 < 1) return std::nullopt;
        out = tuning::mul(Rational(m2), f);
      }
      if (!out || !out->isPositive()) return std::nullopt;
      return PitchId::fromRatio(*out, pitch.anchorKind(), pitch.anchorId());
    }
  }
  return std::nullopt;
}

std::vector<GroupStepFundamental> groupStepFundamentals(const NoteClip& clip) {
  std::vector<GroupStepFundamental> out;
  for (const JiGroup& g : jiGroupsOf(clip))
    if (const std::optional<GroupFundamental> f = groupFundamental(clip, g)) out.push_back({g.anchor, f->fundamental});
  std::sort(out.begin(), out.end(), [](const GroupStepFundamental& a, const GroupStepFundamental& b) { return a.anchor < b.anchor; });
  return out;
}

EditResult addNotes(const Project& p, IdSource& ids, ClipRef clip, std::span<const Note> notes,
                    std::span<const NoteCurves> curves, const JiSource& ji) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  Edit e = makeEdit(notes.size() == 1 ? "Add Note" : "Add Notes");
  e.noteClip = clip;
  AddNotes a{clip, {}};
  a.notes.reserve(notes.size());
  std::vector<NoteId> sources;  // the ids the given notes carry (what `curves` and `ji` are matched by)
  sources.reserve(notes.size());
  for (const Note& n : notes) {
    Note m = n;
    m.id = ids.note();
    PQ_DOC_TRY(validateNote(m));
    sources.push_back(n.id);
    a.notes.push_back(m);
    e.notes.push_back(keyOf(m));
  }
  // One command for all the carried curves (M13); bends between pasted notes glide between the copies (T7.11a).
  Result<std::optional<SetNoteCurves>> carried = copiedCurves(clip, *f->clip, curves, sources, a.notes);
  if (!carried) return std::move(carried).error();
  // T7.13: pasted members follow the pasted anchor, as one new group.
  Result<CopiedJi> copied = !ji.groups.empty() || !ji.written.empty() ? copyJi(clip, *f->clip, ji, sources, a.notes, ids)
                                                                       : Result<CopiedJi>(CopiedJi{});
  if (!copied) return std::move(copied).error();
  e.jiGroups = copied->made;
  if (!a.notes.empty()) batchOf(e).commands.push_back(std::move(a));
  if (*carried) batchOf(e).commands.push_back(std::move(**carried));
  if (copied->groups) batchOf(e).commands.push_back(std::move(*copied->groups));
  if (copied->written) batchOf(e).commands.push_back(std::move(*copied->written));
  return e;
}

EditResult deleteNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  Edit e = makeEdit(notes.size() == 1 ? "Delete Note" : "Delete Notes");
  e.noteClip = clip;
  RemoveNotes r{clip, distinct(notes)};
  std::vector<NoteId> gone;
  gone.reserve(r.notes.size());
  for (const NoteKey& k : r.notes) {
    const Note* n = f->clip->notes.locate(k);
    if (!n) return notFound("note not found");
    gone.push_back(n->id);
  }
  std::sort(gone.begin(), gone.end());
  // T7.11a (H3): the notes anchored to a deleted note keep their pitch, in the same undo step.
  appendReanchored(batchOf(e), clip, reanchorWithout(p, f->path, *f->clip, gone));
  if (!r.notes.empty()) batchOf(e).commands.push_back(std::move(r));
  return e;
}

namespace {

bool referenceRatio(const PitchId& p) noexcept {
  return p.kind() == PitchKind::Ratio && p.anchorKind() == tuning::AnchorKind::Reference;
}

// The harmonic-step memory after stepping notes (T3.6-fix1): every recorded step whose note still has the pitch it
// produced and was not stepped again, plus one per stepped reference Ratio note whose effective denominator
// lcm(D, its new denominator) is finer than that denominator (D = the clip's fundamental 1/D before the step).
// Unchanged memory: nullopt (no command).
std::optional<ClipFundamental> steppedMemory(const NoteClip& clip, const std::optional<Rational>& fundamental,
                                            std::vector<std::pair<NoteId, Rational>> stepped) {
  std::sort(stepped.begin(), stepped.end(), [](const auto& a, const auto& b) { return a.first.value < b.first.value; });
  if (stepped.empty() && clip.fundamental.steps.empty()) return std::nullopt;
  const int64_t D = fundamental && fundamental->num() == 1 ? fundamental->den() : 0;
  std::vector<HarmonicStep> next;
  auto isStepped = [&](NoteId id) {
    const auto it = std::lower_bound(stepped.begin(), stepped.end(), id,
                                     [](const auto& s, NoteId x) { return s.first.value < x.value; });
    return it != stepped.end() && it->first == id;
  };
  // Prune: steps of notes that are gone, changed or stepped now (one pass over the clip when there are any).
  if (!clip.fundamental.steps.empty()) {
    const std::vector<HarmonicStep>& old = clip.fundamental.steps;
    for (const Note& n : clip.notes) {
      if (!referenceRatio(n.pitch)) continue;
      const auto it = std::lower_bound(old.begin(), old.end(), n.id,
                                       [](const HarmonicStep& h, NoteId id) { return h.note.value < id.value; });
      if (it != old.end() && it->note == n.id && it->ratio == n.pitch.ratio() && !isStepped(n.id)) next.push_back(*it);
    }
  }
  if (D > 0)
    for (const auto& [id, r] : stepped) {
      const int64_t used = lcmCapped(D, r.den(), kMaxFundamentalDenominator);  // −1: finer than any fundamental
      if (used > 0 && used != r.den()) next.push_back(HarmonicStep{id, used, r});
    }
  std::sort(next.begin(), next.end(), [](const HarmonicStep& a, const HarmonicStep& b) { return a.note.value < b.note.value; });
  if (next == clip.fundamental.steps) return std::nullopt;
  ClipFundamental f = clip.fundamental;
  f.steps = std::move(next);
  return f;
}

// moveNotes / transposeNotes: step the notes on the clip's fundamental and update the step memory in the same Batch.
template <class Move>
EditResult stepNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, std::string label, int32_t steps,
                     StepUnit unit, const tuning::Edo* displayGrid, Move&& move) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  std::optional<tuning::Tuning> tun;
  if (steps != 0) {
    Result<tuning::Tuning> t = clipTuning(p, *f);
    if (!t) return std::move(t).error();
    tun = std::move(t).value();
  }
  const std::optional<Rational> fundamental = steps != 0 ? virtualFundamental(*f->clip) : std::nullopt;
  const std::vector<GroupStepFundamental> groups =
      steps != 0 && f->clip->groups ? groupStepFundamentals(*f->clip) : std::vector<GroupStepFundamental>{};
  const PitchStepContext ctx{tun ? &*tun : nullptr, displayGrid, fundamental, groups};
  const NoteClip& c = *f->clip;
  // T7.16a (M7 review H2, M4): JI groups move as chords. A member whose anchor is stepped too keeps its ratio (it
  // follows its anchor) and its written identity steps with it, so Back to Tuning restores the transposed chord. A
  // member stepped without its anchor: in a tuned group it goes to its next harmonic and is re-written as the
  // tuning's nearest step there; in a group Back to Tuning it is the tempered note it sounds as — its written
  // identity stepped, out of the group. An anchor's own written identity steps with it.
  std::vector<NoteId> moving;
  if (steps != 0 && (c.groups || c.written)) {
    moving.reserve(notes.size());
    for (const NoteKey& k : notes)
      if (const Note* n = c.notes.locate(k)) moving.push_back(n->id);
    std::sort(moving.begin(), moving.end());
  }
  auto isMoving = [&](NoteId id) { return std::binary_search(moving.begin(), moving.end(), id); };
  std::vector<std::pair<NoteId, const JiGroup*>> byAnchor;  // O(log groups) per note
  for (const JiGroup& g : jiGroupsOf(c)) byAnchor.emplace_back(g.anchor, &g);
  std::sort(byAnchor.begin(), byAnchor.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  auto groupAnchoredBy = [&](NoteId id) -> const JiGroup* {
    const auto it = std::lower_bound(byAnchor.begin(), byAnchor.end(), id, [](const auto& x, NoteId v) { return x.first < v; });
    return it != byAnchor.end() && it->first == id ? it->second : nullptr;
  };
  std::vector<std::pair<NoteId, std::optional<PitchId>>> rewritten;  // (note, its written identity now; none: dropped)
  std::vector<std::pair<NoteId, Rational>> harmonics;                // tuned members on a new harmonic (re-written below)
  std::vector<std::pair<NoteId, Rational>> stepped;
  std::vector<NoteId> movedAnchors;  // group anchors stepped: their members' written identities step too
  EditResult e = updateNotes(p, clip, notes, std::move(label), [&](Note& n) -> Status {
    move(n);
    if (steps == 0) return ok();
    const NoteId anchor = noteAnchorOf(n.pitch);
    const JiGroup* g = anchor.valid() ? groupAnchoredBy(anchor) : nullptr;
    const NoteWritten* w = c.written ? findNoteWritten(c, n.id) : nullptr;
    auto stepWritten = [&] {
      if (w == nullptr) return;
      if (std::optional<PitchId> sw = stepPitch(w->written, steps, unit, ctx)) rewritten.emplace_back(n.id, *sw);
    };
    if (g != nullptr && isMoving(anchor)) {  // follows its anchor
      stepWritten();
      return ok();
    }
    if (g != nullptr && !g->tuned && w != nullptr) {  // a tempered note leaving its group
      const std::optional<PitchId> sw = stepPitch(w->written, steps, unit, ctx);
      n.pitch = sw ? *sw : w->written;
      rewritten.emplace_back(n.id, std::nullopt);
      return ok();
    }
    if (auto np = stepPitch(n.pitch, steps, unit, ctx)) {
      n.pitch = *np;
      if (referenceRatio(n.pitch)) stepped.emplace_back(n.id, n.pitch.ratio());
      if (g != nullptr && w != nullptr) harmonics.emplace_back(n.id, n.pitch.ratio());
      else if (g == nullptr && groupAnchoredBy(n.id) != nullptr) {  // an anchor: what hangs on it moves with it
        stepWritten();
        movedAnchors.push_back(n.id);
      }
      else if (w != nullptr && g == nullptr) rewritten.emplace_back(n.id, std::nullopt);  // no member: means nothing
    }
    return ok();
  });
  if (!e || steps == 0) return e;
  if (std::optional<ClipFundamental> mem = steppedMemory(c, fundamental, stepped))
    batchOf(*e).commands.push_back(SetClipFundamental{clip, std::move(*mem)});
  if (!movedAnchors.empty() && c.written) {
    // The members of a stepped anchor that were not stepped themselves follow it in JI: what they are written as
    // follows too (so Back to Tuning restores the chord where it now is).
    std::sort(movedAnchors.begin(), movedAnchors.end());
    for (const Note& n : c.notes) {
      const NoteId a = noteAnchorOf(n.pitch);
      if (!a.valid() || isMoving(n.id) || !std::binary_search(movedAnchors.begin(), movedAnchors.end(), a)) continue;
      if (const NoteWritten* w = findNoteWritten(c, n.id))
        if (std::optional<PitchId> sw = stepPitch(w->written, steps, unit, ctx)) rewritten.emplace_back(n.id, *sw);
    }
  }
  if (!harmonics.empty() && tun) {
    // The nearest step to the member's new JI pitch (its anchor's pitch × its ratio): Back to Tuning puts it on a row.
    const ClipPitches cp = resolveClipPitches(p, f->path, c);
    const NotePitchIndex hz(cp);
    for (const auto& [id, ratio] : harmonics) {
      const Note* n = c.notes.findById(id);
      const std::optional<double> a = n ? hz.hz(noteAnchorOf(n->pitch)) : std::nullopt;
      std::optional<PitchId> near;
      if (a && *a > 0.0) near = nearestDegreePitch(*tun, 1200.0 * std::log2(*a / tun->referenceHz()) + tuning::cents(ratio));
      rewritten.emplace_back(id, near);
    }
  }
  if (!rewritten.empty()) {
    std::sort(rewritten.begin(), rewritten.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    NoteWrittenList list;
    for (const NoteWritten& w : notesWrittenOf(c)) {
      const auto it = std::lower_bound(rewritten.begin(), rewritten.end(), w.note, [](const auto& x, NoteId id) { return x.first < id; });
      if (it == rewritten.end() || it->first != w.note) list.push_back(w);
      else if (it->second) list.push_back(NoteWritten{w.note, *it->second});
    }
    if (!std::ranges::equal(list, notesWrittenOf(c)))
      batchOf(*e).commands.push_back(SetNotesWritten{clip, std::move(list)});
  }
  return e;
}

// bug-ji-input (the user: "dragging the anchor must move only that note"): a JI group's anchor moved without every
// member of its group moves ALONE, as a member would — the notes that stay keep their pitch, their number and their
// group, and the fundamental stays. In pitch the group is first re-anchored (reanchorJiGroup) onto its lowest staying
// member with that member's own number, so the moved note IS a member when `build` steps it: on the group's
// harmonics in a tuned group, as the tempered note it sounds as (leaving the group) in one Back to Tuning. In time
// alone nothing is re-anchored; the group's warps are shifted so they stay where they are in the clip. A group all of
// whose notes move is a chord moving (T7.16a H2): untouched. `build(project)` makes the move on the given project.
// Cost: one pass over the notes when no anchor moves alone; else per such group one re-anchoring (a resolve of the
// clip) and its commands applied to a scratch snapshot.
template <class Build>
EditResult moveAnchorsAlone(const Project& p, ClipRef clip, std::span<const NoteKey> notes, Ticks dt, int32_t steps, Build&& build) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f || !f->clip->groups || (dt == 0 && steps == 0)) return build(p);
  const NoteClip& c = *f->clip;
  std::vector<NoteId> moving;
  moving.reserve(notes.size());
  for (const NoteKey& k : notes)
    if (const Note* n = c.notes.locate(k)) moving.push_back(n->id);
  std::sort(moving.begin(), moving.end());
  auto isMoving = [&](NoteId id) { return std::binary_search(moving.begin(), moving.end(), id); };
  std::vector<NoteId> alone;  // moving anchors with a member that stays
  for (const Note& n : c.notes)
    if (const NoteId a = noteAnchorOf(n.pitch); a.valid() && isMoving(a) && !isMoving(n.id)) alone.push_back(a);
  std::sort(alone.begin(), alone.end());
  alone.erase(std::unique(alone.begin(), alone.end()), alone.end());
  std::vector<JiGroupId> groups;
  for (const JiGroup& g : jiGroupsOf(c))
    if (std::binary_search(alone.begin(), alone.end(), g.anchor)) groups.push_back(g.id);
  if (groups.empty()) return build(p);
  if (steps == 0) {
    EditResult e = build(p);
    if (!e) return e;
    JiGroupList list(jiGroupsOf(c).begin(), jiGroupsOf(c).end());
    bool shifted = false;
    for (JiGroup& g : list)
      if (std::binary_search(alone.begin(), alone.end(), g.anchor) && !g.warps.empty()) {
        g.warps = shiftedWarps(g.warps, dt);  // the same points in the clip, counted from the anchor's new start
        shifted = true;
      }
    if (shifted) batchOf(*e).commands.push_back(SetJiGroups{clip, std::move(list)});
    e->jiGroups.insert(e->jiGroups.end(), groups.begin(), groups.end());
    return e;
  }
  Snapshot scratch(Snapshot{}, &p);  // non-owning: p outlives this call
  std::vector<Command> first;
  for (const JiGroupId id : groups) {
    Result<ClipFound> now = findClip(*scratch, clip);
    if (!now) return std::move(now).error();
    const NoteClip& cc = *now->clip;
    const JiGroup* g = nullptr;
    for (const JiGroup& x : jiGroupsOf(cc))
      if (x.id == id) g = &x;
    const std::optional<GroupHarmonics> gh = g ? groupHarmonics(cc, *g) : std::nullopt;
    if (!gh) return invalid("the JI group's anchor is not in the clip");
    const GroupMember* carrier = nullptr;  // the lowest staying member with a number
    for (const GroupMember& m : gh->members)
      if (!isMoving(m.note) && m.harmonic >= 1 && m.harmonic <= static_cast<int64_t>(kMaxHarmonicIndex) &&
          (carrier == nullptr || m.harmonic < carrier->harmonic))
        carrier = &m;
    const Note* cn = carrier ? cc.notes.findById(carrier->note) : nullptr;
    if (cn == nullptr) return invalid("the notes that stay in the JI group have no harmonic number to carry it; move the whole chord");
    EditResult r = reanchorJiGroup(*scratch, clip, id, keyOf(*cn), static_cast<uint32_t>(carrier->harmonic));
    if (!r) return r;
    for (const Command& cmd : r->batch().commands) {
      ChangeSet cs;
      Result<Snapshot> next = apply(scratch, cmd, cs);
      if (!next) return std::move(next).error();
      scratch = std::move(*next);
      first.push_back(cmd);
    }
  }
  EditResult e = build(*scratch);
  if (!e) return e;
  Batch& b = batchOf(*e);
  b.commands.insert(b.commands.begin(), first.begin(), first.end());
  e->jiGroups.insert(e->jiGroups.end(), groups.begin(), groups.end());
  return e;
}

}  // namespace

EditResult moveNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, Ticks dt, int32_t steps,
                     const tuning::Edo* displayGrid) {
  return moveAnchorsAlone(p, clip, notes, dt, steps, [&](const Project& q) {
    return stepNotes(q, clip, notes, notes.size() == 1 ? "Move Note" : "Move Notes", steps, StepUnit::Grid, displayGrid,
                     [&](Note& n) { n.start += dt; });
  });
}

EditResult transposeNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, int32_t steps, StepUnit unit,
                          const tuning::Edo* displayGrid) {
  return moveAnchorsAlone(p, clip, notes, 0, steps, [&](const Project& q) {
    return stepNotes(q, clip, notes, unit == StepUnit::Equave ? "Transpose Equave" : "Transpose", steps, unit,
                     displayGrid, [](Note&) {});
  });
}

EditResult resizeNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, NoteEdge edge, Ticks delta) {
  return updateNotes(p, clip, notes, notes.size() == 1 ? "Resize Note" : "Resize Notes", [&](Note& n) -> Status {
    if (edge == NoteEdge::Start) {
      const Ticks end = n.start + n.length;
      n.start = std::min(n.start + delta, end - 1);
      n.length = end - n.start;
    } else {
      n.length = std::max<Ticks>(n.length + delta, 1);
    }
    return checkLength(n.length, "note length");
  });
}

EditResult setNotesProperty(const Project& p, ClipRef clip, std::span<const NoteKey> notes, NoteProperty prop,
                            float value) {
  if (!std::isfinite(value)) return invalid("value must be finite");
  const float v = clampProperty(prop, value);
  return updateNotes(p, clip, notes, std::string(propertyLabel(prop)), [&](Note& n) -> Status {
    setProperty(n, prop, v);
    return ok();
  });
}

EditResult offsetNotesProperty(const Project& p, ClipRef clip, std::span<const NoteKey> notes, NoteProperty prop,
                               float delta) {
  if (!std::isfinite(delta)) return invalid("value must be finite");
  return updateNotes(p, clip, notes, std::string(propertyLabel(prop)), [&](Note& n) -> Status {
    setProperty(n, prop, clampProperty(prop, propertyOf(n, prop) + delta));
    return ok();
  });
}

EditResult quantizeNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, const Grid& grid,
                         Ticks gridOrigin, int amountPercent, QuantizeMode mode) {
  auto q = [&](Note& n) -> Status {
    const TickRange r = quantize(grid, n.start + gridOrigin, n.start + n.length + gridOrigin, amountPercent, mode);
    n.start = r.start - gridOrigin;
    n.length = std::max<Ticks>(r.end - r.start, 1);
    return ok();
  };
  if (!notes.empty()) return updateNotes(p, clip, notes, "Quantize", q);
  // Every note of the clip: no key lookups.
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  Edit e = makeEdit("Quantize");
  e.noteClip = clip;
  UpdateNotes u{clip, {}};
  e.notes.reserve(f->clip->notes.size());
  for (const Note& n : f->clip->notes) {
    Note m = n;
    PQ_DOC_TRY(q(m));
    PQ_DOC_TRY(checkTick(m.start, "note start"));
    if (!(m == n)) u.updates.push_back({keyOf(n), m});
    e.notes.push_back(keyOf(m));
  }
  if (!u.updates.empty()) batchOf(e).commands.push_back(std::move(u));
  return e;
}

EditResult duplicateNotes(const Project& p, IdSource& ids, ClipRef clip, std::span<const NoteKey> notes, Ticks offset) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  std::vector<Note> src;
  Ticks lo = kMaxTicks, hi = -kMaxTicks;
  for (const NoteKey& k : distinct(notes)) {
    const Note* n = f->clip->notes.locate(k);
    if (!n) return notFound("note not found");
    src.push_back(*n);
    lo = std::min(lo, n->start);
    hi = std::max(hi, n->start + n->length);
  }
  Edit e = makeEdit(src.size() == 1 ? "Duplicate Note" : "Duplicate Notes");
  e.noteClip = clip;
  if (src.empty()) return e;
  if (offset == 0) offset = hi - lo;
  AddNotes a{clip, {}};
  for (const Note& n : src) {
    Note m = n;
    m.id = ids.note();
    m.start += offset;
    PQ_DOC_TRY(validateNote(m));
    a.notes.push_back(m);
    e.notes.push_back(keyOf(m));
  }
  if (f->clip->curves || f->clip->groups || f->clip->written) {
    // A copy has its source's curves, in one command (M13); T7.13: copied members follow the copied anchor as a new
    // group and keep their written identities.
    std::vector<NoteId> sources;
    sources.reserve(src.size());
    for (const Note& n : src) sources.push_back(n.id);
    Result<std::optional<SetNoteCurves>> carried = copiedCurves(clip, *f->clip, noteCurvesOf(*f->clip), sources, a.notes);
    if (!carried) return std::move(carried).error();
    Result<CopiedJi> copied = copyJi(clip, *f->clip, JiSource{jiGroupsOf(*f->clip), notesWrittenOf(*f->clip)}, sources, a.notes, ids);
    if (!copied) return std::move(copied).error();
    e.jiGroups = copied->made;
    batchOf(e).commands.push_back(std::move(a));
    if (*carried) batchOf(e).commands.push_back(std::move(**carried));
    if (copied->groups) batchOf(e).commands.push_back(std::move(*copied->groups));
    if (copied->written) batchOf(e).commands.push_back(std::move(*copied->written));
  } else {
    batchOf(e).commands.push_back(std::move(a));
  }
  return e;
}

std::optional<Rational> simplestRatioBetween(double lo, double hi) noexcept {
  if (!(lo > 0.0) || !(hi >= lo) || !std::isfinite(hi)) return std::nullopt;
  // Continued-fraction terms of the simplest number in [lo, hi] (the Stern–Brocot node closest to the root): the
  // integer part while both ends share it, then recurse on the reciprocals of the fractional parts.
  int64_t terms[64];
  int n = 0;
  double a = lo, b = hi;
  for (;;) {
    if (n == 64) return std::nullopt;
    const double fa = std::floor(a);
    if (fa > 1e15) return std::nullopt;
    if (fa == a) {
      terms[n++] = static_cast<int64_t>(fa);
      break;
    }
    if (fa + 1.0 <= b) {
      terms[n++] = static_cast<int64_t>(fa) + 1;
      break;
    }
    terms[n++] = static_cast<int64_t>(fa);
    const double na = 1.0 / (b - fa), nb = 1.0 / (a - fa);
    a = na;
    b = nb;
  }
  // t0 + 1/(t1 + 1/(… + 1/t_{n−1})), folded from the back with overflow checks.
  int64_t num = terms[n - 1], den = 1;
  constexpr int64_t kLimit = int64_t{1} << 40;
  for (int i = n - 2; i >= 0; --i) {
    if (num > kLimit || terms[i] > kLimit) return std::nullopt;
    const int64_t next = terms[i] * num + den;
    den = num;
    num = next;
  }
  if (num <= 0 || den <= 0 || num > kLimit || den > kLimit) return std::nullopt;
  return Rational::tryMake(num, den);
}

std::optional<Rational> simplestRatioNear(double cents, double toleranceCents) noexcept {
  if (!std::isfinite(cents) || !(toleranceCents >= 0.0)) return std::nullopt;
  return simplestRatioBetween(std::exp2((cents - toleranceCents) / 1200.0), std::exp2((cents + toleranceCents) / 1200.0));
}

EditResult convertNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, PitchKind to) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  Result<tuning::Tuning> t = clipTuning(p, *f);
  if (!t) return std::move(t).error();
  const DegreeLattice l = DegreeLattice::of(*t);
  if (!l.usable()) return invalid("the clip's tuning has no usable equave");
  // Cents of a note relative to the tuning's reference, if it has a fixed one.
  auto centsOf = [&](const Note& n) -> std::optional<double> {
    const PitchId& id = n.pitch;
    switch (id.kind()) {
      case PitchKind::Degree:
        return l.centsOf(static_cast<int64_t>(id.equave()) * l.size + id.degree());
      case PitchKind::Ratio:
        if (id.anchorKind() != tuning::AnchorKind::Reference) return std::nullopt;
        return 1200.0 * std::log2(id.ratio().toDouble());
      case PitchKind::Cents: return id.cents();
    }
    return std::nullopt;
  };
  const char* label = to == PitchKind::Cents ? "Freeze to Cents" : to == PitchKind::Degree ? "Snap to Degree" : "Snap to Ratio";
  return updateNotes(p, clip, notes, label, [&](Note& n) -> Status {
    if (n.pitch.kind() == to) return ok();
    const auto c = centsOf(n);
    if (!c) return ok();
    if (to == PitchKind::Cents) {
      n.pitch = PitchId::fromCents(*c);
    } else if (to == PitchKind::Degree) {
      const int64_t abs = l.nearest(*c);
      const int64_t e = floorDiv(abs, l.size);
      if (!fitsI32(e)) return ok();
      n.pitch = PitchId::fromDegree(static_cast<int32_t>(abs - e * l.size), static_cast<int32_t>(e));
    } else {
      const std::optional<Rational> r = simplestRatioNear(*c, ratioSnapTolerance(*t, *c));
      if (!r || !r->isPositive()) return ok();
      n.pitch = PitchId::fromRatio(*r);
    }
    return ok();
  });
}

namespace detail {
std::optional<PitchId> nearestDegreePitch(const tuning::Tuning& t, double cents) {
  const DegreeLattice l = DegreeLattice::of(t);
  if (!l.usable() || !std::isfinite(cents)) return std::nullopt;
  const int64_t abs = l.nearest(cents);
  const int64_t e = floorDiv(abs, l.size);
  if (!fitsI32(e)) return std::nullopt;
  return PitchId::fromDegree(static_cast<int32_t>(abs - e * l.size), static_cast<int32_t>(e));
}
}  // namespace detail

double ratioSnapTolerance(const tuning::Tuning& t, double cents) noexcept {
  const DegreeLattice l = DegreeLattice::of(t);
  if (!l.usable()) return kRatioSnapMaxCents;
  const int64_t d = l.nearest(cents);
  const double step = std::min(l.centsOf(d + 1) - l.centsOf(d), l.centsOf(d) - l.centsOf(d - 1));
  return std::clamp(0.45 * step, kRatioSnapMinCents, kRatioSnapMaxCents);
}

EditResult setNotesPitch(const Project& p, ClipRef clip, std::span<const NoteKey> notes, const PitchId& pitch) {
  if (pitch.kind() == PitchKind::Ratio && !pitch.ratio().isPositive()) return invalid("a ratio pitch must be positive");
  EditResult e = updateNotes(p, clip, notes, "Set Note Pitch", [&](Note& n) -> Status {
    n.pitch = pitch;
    return ok();
  });
  if (e)
    if (Result<ClipFound> f = findClip(p, clip); f && f->clip->written) dropRepitchedWritten(batchOf(*e), clip, *f->clip);
  return e;
}

}  // namespace pq::doc::ops
