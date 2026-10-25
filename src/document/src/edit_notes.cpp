// Note edit operations and identity-aware pitch steps (T3.6).

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "edit_common.h"
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
        if (ctx.fundamental && pitch.anchorKind() == tuning::AnchorKind::Reference) {
          const Rational both[2] = {r, *ctx.fundamental};
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
    case PitchKind::Harmonic: {
      if (unit == StepUnit::Equave) return std::nullopt;
      const int64_t v = static_cast<int64_t>(pitch.voiceIndex()) + steps;
      if (v < 0 || v > std::numeric_limits<uint32_t>::max()) return std::nullopt;
      return PitchId::fromHarmonic(pitch.jiObject(), static_cast<uint32_t>(v));
    }
  }
  return std::nullopt;
}

EditResult addNotes(const Project& p, IdSource& ids, ClipRef clip, std::span<const Note> notes) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  Edit e = makeEdit(notes.size() == 1 ? "Add Note" : "Add Notes");
  e.noteClip = clip;
  AddNotes a{clip, {}};
  a.notes.reserve(notes.size());
  for (const Note& n : notes) {
    Note m = n;
    m.id = ids.note();
    PQ_DOC_TRY(validateNote(m));
    a.notes.push_back(m);
    e.notes.push_back(keyOf(m));
  }
  if (!a.notes.empty()) batchOf(e).commands.push_back(std::move(a));
  return e;
}

EditResult deleteNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  Edit e = makeEdit(notes.size() == 1 ? "Delete Note" : "Delete Notes");
  e.noteClip = clip;
  RemoveNotes r{clip, distinct(notes)};
  for (const NoteKey& k : r.notes)
    if (!f->clip->notes.locate(k)) return notFound("note not found");
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
  if (clip.fundamental.lock) {  // a lock is the fundamental: steps under it are not remembered
    if (clip.fundamental.steps.empty()) return std::nullopt;
  } else if (stepped.empty() && clip.fundamental.steps.empty()) {
    return std::nullopt;
  }
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
  if (!clip.fundamental.lock && D > 0)
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
  const PitchStepContext ctx{tun ? &*tun : nullptr, displayGrid, fundamental};
  std::vector<std::pair<NoteId, Rational>> stepped;
  EditResult e = updateNotes(p, clip, notes, std::move(label), [&](Note& n) -> Status {
    move(n);
    if (steps == 0) return ok();
    if (auto np = stepPitch(n.pitch, steps, unit, ctx)) {
      n.pitch = *np;
      if (referenceRatio(n.pitch)) stepped.emplace_back(n.id, n.pitch.ratio());
    }
    return ok();
  });
  if (!e || steps == 0) return e;
  if (std::optional<ClipFundamental> mem = steppedMemory(*f->clip, fundamental, stepped))
    batchOf(*e).commands.push_back(SetClipFundamental{clip, std::move(*mem)});
  return e;
}

}  // namespace

EditResult moveNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, Ticks dt, int32_t steps,
                     const tuning::Edo* displayGrid) {
  return stepNotes(p, clip, notes, notes.size() == 1 ? "Move Note" : "Move Notes", steps, StepUnit::Grid, displayGrid,
                   [&](Note& n) { n.start += dt; });
}

EditResult transposeNotes(const Project& p, ClipRef clip, std::span<const NoteKey> notes, int32_t steps, StepUnit unit,
                          const tuning::Edo* displayGrid) {
  return stepNotes(p, clip, notes, unit == StepUnit::Equave ? "Transpose Equave" : "Transpose", steps, unit,
                   displayGrid, [](Note&) {});
}

EditResult lockFundamental(const Project& p, ClipRef clip, NoteKey key, uint32_t harmonicIndex) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  const Note* n = f->clip->notes.locate(key);
  if (!n) return notFound("note not found");
  if (harmonicIndex < 1) return invalid("harmonic index must be at least 1");
  if (!referenceRatio(n->pitch)) return invalid("the fundamental lock needs a Ratio note (relative to the reference)");
  const std::optional<Rational> lock = tuning::div(n->pitch.ratio(), Rational(static_cast<int64_t>(harmonicIndex)));
  if (!lock) return invalid("fundamental out of range");
  Edit e = makeEdit("Lock Fundamental");
  e.noteClip = clip;
  e.notes.push_back(key);
  if (f->clip->fundamental.lock != lock) {
    ClipFundamental cf = f->clip->fundamental;
    cf.lock = lock;
    batchOf(e).commands.push_back(SetClipFundamental{clip, std::move(cf)});
  }
  return e;
}

EditResult unlockFundamental(const Project& p, ClipRef clip) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  Edit e = makeEdit("Unlock Fundamental");
  e.noteClip = clip;
  if (f->clip->fundamental.lock) {
    ClipFundamental cf = f->clip->fundamental;
    cf.lock.reset();
    batchOf(e).commands.push_back(SetClipFundamental{clip, std::move(cf)});
  }
  return e;
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
  batchOf(e).commands.push_back(std::move(a));
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
  if (to == PitchKind::Harmonic) return invalid("notes convert to Degree, Ratio or Cents");
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  Result<tuning::Tuning> t = clipTuning(p, *f);
  if (!t) return std::move(t).error();
  const DegreeLattice l = DegreeLattice::of(*t);
  if (!l.usable()) return invalid("the clip's tuning has no usable equave");
  // Cents of a note relative to the tuning's reference, if it has a fixed one.
  auto centsOf = [&](const PitchId& id) -> std::optional<double> {
    switch (id.kind()) {
      case PitchKind::Degree:
        return l.centsOf(static_cast<int64_t>(id.equave()) * l.size + id.degree());
      case PitchKind::Ratio:
        if (id.anchorKind() != tuning::AnchorKind::Reference) return std::nullopt;
        return 1200.0 * std::log2(id.ratio().toDouble());
      case PitchKind::Cents: return id.cents();
      case PitchKind::Harmonic: return std::nullopt;
    }
    return std::nullopt;
  };
  const char* label = to == PitchKind::Cents ? "Freeze to Cents" : to == PitchKind::Degree ? "Snap to Degree" : "Snap to Ratio";
  return updateNotes(p, clip, notes, label, [&](Note& n) -> Status {
    if (n.pitch.kind() == to) return ok();
    const auto c = centsOf(n.pitch);
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

double ratioSnapTolerance(const tuning::Tuning& t, double cents) noexcept {
  const DegreeLattice l = DegreeLattice::of(t);
  if (!l.usable()) return kRatioSnapMaxCents;
  const int64_t d = l.nearest(cents);
  const double step = std::min(l.centsOf(d + 1) - l.centsOf(d), l.centsOf(d) - l.centsOf(d - 1));
  return std::clamp(0.45 * step, kRatioSnapMinCents, kRatioSnapMaxCents);
}

EditResult setNotesPitch(const Project& p, ClipRef clip, std::span<const NoteKey> notes, const PitchId& pitch) {
  if (pitch.kind() == PitchKind::Harmonic) return invalid("a Harmonic pitch needs a JI object");
  if (pitch.kind() == PitchKind::Ratio && (!pitch.ratio().isPositive() || pitch.anchorKind() == tuning::AnchorKind::JiFundamental))
    return invalid("a ratio pitch must be positive and anchored to the reference or a note");
  return updateNotes(p, clip, notes, "Set Note Pitch", [&](Note& n) -> Status {
    n.pitch = pitch;
    return ok();
  });
}

}  // namespace pq::doc::ops
