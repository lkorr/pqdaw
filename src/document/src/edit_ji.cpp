// JI object and note-curve edit operations (T7.2; edit_ops.h "JI objects and note curves").

#include <algorithm>
#include <cmath>
#include <numeric>

#include "edit_common.h"
#include "pq/document/ji_object.h"
#include "pq/document/pitch_resolve.h"

namespace pq::doc::ops {

using namespace detail;
using tuning::PitchId;
using tuning::PitchKind;
using tuning::Rational;

namespace {

struct JiFound {
  ClipFound clip;
  const JiObject* object = nullptr;
};

Result<JiFound> findJi(const Project& p, const ClipRef& clip, JiObjectId object) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  const JiObject* o = findJiObject(*f->clip, object);
  if (!o) return notFound("JI object not found");
  return JiFound{std::move(*f), o};
}

// What a fundamental / a tuning stop's pitch may be.
bool plainPitch(const PitchId& p) noexcept {
  return p.kind() == PitchKind::Degree || p.kind() == PitchKind::Cents ||
         (p.kind() == PitchKind::Ratio && p.anchorKind() == tuning::AnchorKind::Reference);
}

// The voice count follows the stops: new voices get fresh note ids, voices beyond it are dropped; voice maps and
// per-voice timings of another size are dropped (the spread rule applies again).
Status fitVoices(JiObject& o, IdSource* ids) {
  size_t V = 0;
  for (const JiStop& s : o.stops) V = std::max<size_t>(V, s.size());
  if (V < 1 || V > kMaxJiVoices) return invalid("a JI object has 1 to 64 voices");
  if (o.voiceNotes.size() > V) o.voiceNotes.resize(V);
  while (o.voiceNotes.size() < V) {
    if (!ids) return invalid("the JI object would need more voices");
    o.voiceNotes.push_back(ids->note());
  }
  for (JiStop& s : o.stops) {
    if (!s.voiceTerm.empty() && s.voiceTerm.size() != V) s.voiceTerm.clear();
    if (!s.voiceTiming.empty() && s.voiceTiming.size() != V) s.voiceTiming.clear();
  }
  return ok();
}

// The Edit replacing `f.object` by `o` (voices fitted, validated; a Harmonic note on a lost voice refuses).
EditResult replaceObject(const ClipRef& clip, const JiFound& f, JiObject o, IdSource* ids, std::string label) {
  o.id = f.object->id;
  PQ_DOC_TRY(fitVoices(o, ids));
  PQ_DOC_TRY(checkJiObject(o));
  if (o.voices() < f.object->voices())
    for (const Note& n : f.clip.clip->notes)
      if (n.pitch.kind() == PitchKind::Harmonic && n.pitch.jiObject() == o.id.value && n.pitch.voiceIndex() >= o.voices())
        return invalid("a note follows a voice the JI object would lose");
  Edit e = makeEdit(std::move(label));
  e.noteClip = clip;
  e.jiObjects.push_back(o.id);
  if (!(o == *f.object)) batchOf(e).commands.push_back(SetJiObject{clip, std::move(o)});
  return e;
}

Status checkTerms(std::span<const int64_t> terms) {
  if (terms.empty() || terms.size() > kMaxJiVoices) return invalid("a proportion has 1 to 64 terms");
  for (size_t i = 0; i < terms.size(); ++i) {
    if (terms[i] < 1 || terms[i] > kMaxJiTerm) return invalid("JI term out of range (1 to 2^20)");
    if (i > 0 && terms[i] <= terms[i - 1]) return invalid("JI terms must ascend");
  }
  return ok();
}

// Inserts `stop` (its `at` set) into o.stops, replacing a stop at the same tick; the glide is clamped to the gap after
// the stop before it.
void placeStop(JiObject& o, JiStop stop) {
  auto it = std::lower_bound(o.stops.begin(), o.stops.end(), stop.at, [](const JiStop& s, Ticks at) { return s.at < at; });
  const Ticks gap = it == o.stops.begin() ? 0 : stop.at - (it - 1)->at;
  stop.timing.glide = std::clamp<Ticks>(stop.timing.glide, 0, gap);
  if (it != o.stops.end() && it->at == stop.at) *it = std::move(stop);
  else o.stops.insert(it, std::move(stop));
}

Status checkStopTick(Ticks at) {
  if (at < 0 || at > kMaxTicks) return invalid("JI stop out of range");
  return ok();
}

}  // namespace

EditResult createJiObject(const Project& p, IdSource& ids, ClipRef clip, Ticks start, Ticks length,
                          const PitchId& fundamental, std::span<const int64_t> terms, bool undertone) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  PQ_DOC_TRY(checkTerms(terms));
  JiObject o;
  o.start = start;
  o.length = length;
  o.fundamental = fundamental;
  o.undertone = undertone;
  JiStop s;
  s.terms.assign(terms.begin(), terms.end());
  o.stops.push_back(std::move(s));
  o.id = JiObjectId{1};  // validated before ids are drawn, so a refused object costs none
  o.voiceNotes.assign(terms.size(), NoteId{});
  for (size_t i = 0; i < o.voiceNotes.size(); ++i) o.voiceNotes[i] = NoteId{static_cast<uint32_t>(i + 1)};
  PQ_DOC_TRY(checkJiObject(o));
  o.id = ids.jiObject();
  for (NoteId& v : o.voiceNotes) v = ids.note();
  Edit e = makeEdit("Create JI Object");
  e.noteClip = clip;
  e.jiObjects.push_back(o.id);
  batchOf(e).commands.push_back(AddJiObject{clip, std::move(o)});
  return e;
}

EditResult jiFromNotes(const Project& p, IdSource& ids, ClipRef clip, std::span<const NoteKey> keys,
                       const JiFromNotesOptions& options) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  if (keys.empty()) return invalid("no notes");
  Result<tuning::Tuning> t = clipTuning(p, *f);
  if (!t) return std::move(t).error();
  Result<ClipPitches> resolved = resolveClipPitches(p, clip.track, clip.clip);
  if (!resolved) return std::move(resolved).error();
  std::vector<std::pair<NoteId, double>> hzOf;  // sorted by note id
  hzOf.reserve(resolved->notes.size());
  for (const NotePitch& np : resolved->notes)
    if (np.hz) hzOf.emplace_back(np.note, *np.hz);
  std::sort(hzOf.begin(), hzOf.end());

  struct Voice {
    double cents = 0.0;
    PitchId pitch;                  // as written when it may be a stop's pitch, else Cents
    std::optional<Rational> ratio;  // a reference-anchored Ratio note's
    std::vector<const Note*> notes;
  };
  std::vector<Voice> voices;
  std::vector<NoteId> seen;
  Ticks lo = kMaxTicks, hi = -kMaxTicks;
  float velocity = 0.0f;
  for (const NoteKey& k : keys) {
    const Note* n = f->clip->notes.locate(k);
    if (!n) return notFound("note not found");
    if (std::find(seen.begin(), seen.end(), n->id) != seen.end()) continue;
    seen.push_back(n->id);
    const auto it = std::lower_bound(hzOf.begin(), hzOf.end(), n->id, [](const auto& e, NoteId id) { return e.first < id; });
    if (it == hzOf.end() || it->first != n->id) return invalid("a note's pitch cannot be resolved");
    Voice v;
    v.cents = 1200.0 * std::log2(it->second / t->referenceHz());
    v.pitch = plainPitch(n->pitch) ? n->pitch : PitchId::fromCents(v.cents);
    if (n->pitch.kind() == PitchKind::Ratio && n->pitch.anchorKind() == tuning::AnchorKind::Reference) v.ratio = n->pitch.ratio();
    v.notes.push_back(n);
    voices.push_back(std::move(v));
    lo = std::min(lo, n->start);
    hi = std::max(hi, n->start + n->length);
    velocity = std::max(velocity, n->velocity);
  }
  std::stable_sort(voices.begin(), voices.end(), [](const Voice& a, const Voice& b) { return a.cents < b.cents; });
  std::vector<Voice> distinct;  // one voice per pitch
  for (Voice& v : voices) {
    if (!distinct.empty() && std::abs(distinct.back().cents - v.cents) < 1e-6) distinct.back().notes.push_back(v.notes.front());
    else distinct.push_back(std::move(v));
  }
  const size_t V = distinct.size();
  if (V > kMaxJiVoices) return invalid("a JI object has at most 64 voices");

  // Each voice's ratio to the lowest, then the proportion over the lcm of the denominators.
  std::vector<Rational> ratios(V, Rational(1));
  int64_t L = 1;
  for (size_t i = 1; i < V; ++i) {
    std::optional<Rational> r;
    if (distinct[i].ratio && distinct[0].ratio) r = tuning::div(*distinct[i].ratio, *distinct[0].ratio);
    else r = simplestRatioNear(distinct[i].cents - distinct[0].cents, ratioSnapTolerance(*t, distinct[i].cents));
    if (!r || !r->isPositive()) return invalid("the chord has no proportion");
    ratios[i] = *r;
    L = lcmCapped(L, r->den(), kMaxJiTerm);
    if (L < 0) return invalid("the chord has no proportion within the term limit");
  }
  std::vector<int64_t> terms(V);
  for (size_t i = 0; i < V; ++i) {
    const int64_t mult = L / ratios[i].den();
    if (ratios[i].num() > kMaxJiTerm / mult) return invalid("the chord has no proportion within the term limit");
    terms[i] = ratios[i].num() * mult;
    if (i > 0 && terms[i] <= terms[i - 1]) return invalid("two notes are too close to be different terms");
  }

  JiObject o;
  o.id = ids.jiObject();
  o.start = lo;
  o.length = hi - lo;
  o.fundamental = distinct[0].pitch;  // the pinned (lowest) voice: scale · its term = 1 exactly
  o.velocity = velocity;
  o.sounds = options.replaceNotes;
  const Rational scale(1, terms[0]);
  JiStop proportion;
  proportion.terms = terms;
  proportion.scale = scale;
  if (options.tuningStop) {
    JiStop written;
    written.scale = scale;  // the fundamental stays put through the morph
    for (const Voice& v : distinct) written.pitches.push_back(v.pitch);
    proportion.at = std::clamp<Ticks>(options.glide, 1, std::max<Ticks>(1, o.length - 1));
    proportion.timing.glide = proportion.at;
    o.stops.push_back(std::move(written));
  }
  o.stops.push_back(std::move(proportion));
  for (size_t i = 0; i < V; ++i) o.voiceNotes.push_back(ids.note());
  PQ_DOC_TRY(checkJiObject(o));

  Edit e = makeEdit("Tune to JI");
  e.noteClip = clip;
  e.jiObjects.push_back(o.id);
  const JiObjectId id = o.id;
  batchOf(e).commands.push_back(AddJiObject{clip, std::move(o)});
  if (options.replaceNotes) {
    RemoveNotes r{clip, {}};
    for (const Voice& v : distinct)
      for (const Note* n : v.notes) r.notes.push_back(keyOf(*n));
    batchOf(e).commands.push_back(std::move(r));
  } else {
    UpdateNotes u{clip, {}};
    for (size_t i = 0; i < V; ++i)
      for (const Note* n : distinct[i].notes) {
        Note m = *n;
        m.pitch = PitchId::fromHarmonic(id.value, static_cast<uint32_t>(i));
        u.updates.push_back({keyOf(*n), m});
        e.notes.push_back(keyOf(m));
      }
    batchOf(e).commands.push_back(std::move(u));
  }
  return e;
}

EditResult moveJiObject(const Project& p, ClipRef clip, JiObjectId object, Ticks dt) {
  Result<JiFound> f = findJi(p, clip, object);
  if (!f) return std::move(f).error();
  Edit e = makeEdit("Move JI Object");
  e.noteClip = clip;
  e.jiObjects.push_back(object);
  JiObject o = *f->object;
  o.start += dt;
  PQ_DOC_TRY(checkTick(o.start, "JI object start"));
  UpdateNotes u{clip, {}};
  for (const Note& n : f->clip.clip->notes) {
    if (jiObjectOf(n.pitch) != object) continue;
    Note m = n;
    m.start += dt;
    PQ_DOC_TRY(checkTick(m.start, "note start"));
    if (dt != 0) u.updates.push_back({keyOf(n), m});
    e.notes.push_back(keyOf(m));
  }
  if (dt == 0) return e;
  batchOf(e).commands.push_back(SetJiObject{clip, std::move(o)});
  if (!u.updates.empty()) batchOf(e).commands.push_back(std::move(u));
  return e;
}

EditResult resizeJiObject(const Project& p, IdSource& ids, ClipRef clip, JiObjectId object, NoteEdge edge, Ticks delta) {
  Result<JiFound> f = findJi(p, clip, object);
  if (!f) return std::move(f).error();
  JiObject o = *f->object;
  if (edge == NoteEdge::End) {
    o.length = std::max<Ticks>(o.length + delta, 1);
  } else {
    const Ticks d = std::min(delta, o.length - 1);  // how far the start moves (later: > 0)
    o.start += d;
    o.length -= d;
    PQ_DOC_TRY(checkTick(o.start, "JI object start"));
    // The content keeps its absolute time: the stop the start is on (the last at or before it) becomes stop 0.
    size_t first = 0;
    for (size_t s = 1; s < o.stops.size() && o.stops[s].at <= d; ++s) first = s;
    o.stops.erase(o.stops.begin(), o.stops.begin() + static_cast<std::ptrdiff_t>(first));
    for (size_t s = 1; s < o.stops.size(); ++s) o.stops[s].at -= d;
    o.stops[0].at = 0;
    if (!o.ride.empty()) {
      std::vector<Breakpoint> ride;
      if (d > 0) {
        // Cut at d: the curve family is closed under cutting (the rest of a segment of curvature c from x0 on has
        // curvature c · (1 − x0)).
        const auto it = std::lower_bound(o.ride.begin(), o.ride.end(), d, [](const Breakpoint& b, Ticks v) { return b.tick < v; });
        if (it != o.ride.begin() && (it == o.ride.end() || it->tick > d)) {
          Breakpoint head;
          head.tick = 0;
          head.value = static_cast<float>(automationValueAt(o.ride, d, 0.0));
          if (it != o.ride.end()) {
            const Breakpoint& a = *(it - 1);
            head.curve = a.curve * static_cast<float>(static_cast<double>(it->tick - d) / static_cast<double>(it->tick - a.tick));
          }
          ride.push_back(head);
        }
        for (auto q = it; q != o.ride.end(); ++q) {
          ride.push_back(*q);
          ride.back().tick -= d;
        }
      } else {
        ride = o.ride;
        for (Breakpoint& b : ride) b.tick -= d;
      }
      o.ride = std::move(ride);
    }
  }
  PQ_DOC_TRY(checkLength(o.length, "JI object length"));
  return replaceObject(clip, *f, std::move(o), &ids, "Resize JI Object");
}

EditResult deleteJiObject(const Project& p, ClipRef clip, JiObjectId object) {
  Result<JiFound> f = findJi(p, clip, object);
  if (!f) return std::move(f).error();
  const JiObject& o = *f->object;
  Edit e = makeEdit("Delete JI Object");
  e.noteClip = clip;
  UpdateNotes u{clip, {}};
  std::optional<tuning::Tuning> tun;  // built for the first inner note
  const std::optional<Rational> fundRatio =
      o.fundamental.kind() == PitchKind::Ratio ? std::optional<Rational>(o.fundamental.ratio()) : std::nullopt;
  for (const Note& n : f->clip.clip->notes) {
    if (jiObjectOf(n.pitch) != object) continue;
    if (!tun) {
      Result<tuning::Tuning> t = clipTuning(p, f->clip);
      if (!t) return std::move(t).error();
      tun = std::move(t).value();
    }
    const double tick = static_cast<double>(n.start - o.start);
    const bool harmonic = n.pitch.kind() == PitchKind::Harmonic;
    const JiPosition at = jiPosition(o, harmonic ? static_cast<int32_t>(n.pitch.voiceIndex()) : -1, tick);
    const JiStop& stop = o.stops[at.stop];
    const bool still = at.holding() && automationValueAt(o.ride, static_cast<Ticks>(std::clamp<double>(tick, 0.0, static_cast<double>(o.length))), 0.0) == 0.0;
    std::optional<PitchId> pitch;
    if (still && harmonic && stop.isTuning()) {
      pitch = stop.pitches[jiVoiceEntry(o, stop, n.pitch.voiceIndex())];
    } else if (still && fundRatio && (!harmonic || !stop.isTuning())) {  // exact: fundamental × the voice's / note's ratio
      const std::optional<Rational> over = harmonic ? jiVoiceRatio(o, n.pitch.voiceIndex(), at.stop) : tuning::mul(stop.scale, n.pitch.ratio());
      const std::optional<Rational> r = over ? tuning::mul(*fundRatio, *over) : std::nullopt;
      if (r && r->isPositive()) pitch = PitchId::fromRatio(*r);
    }
    if (!pitch) {
      std::optional<double> c = harmonic ? jiVoiceCents(o, *tun, n.pitch.voiceIndex(), tick) : jiFundamentalCents(o, *tun, tick);
      if (c && !harmonic) *c += tuning::cents(n.pitch.ratio());
      if (!c || !std::isfinite(*c)) return invalid("an inner note's pitch cannot be resolved");
      pitch = PitchId::fromCents(*c);
    }
    Note m = n;
    m.pitch = *pitch;
    u.updates.push_back({keyOf(n), m});
    e.notes.push_back(keyOf(m));
  }
  if (!u.updates.empty()) batchOf(e).commands.push_back(std::move(u));
  batchOf(e).commands.push_back(RemoveJiObject{clip, object});
  return e;
}

EditResult setJiObject(const Project& p, IdSource& ids, ClipRef clip, JiObject object) {
  Result<JiFound> f = findJi(p, clip, object.id);
  if (!f) return std::move(f).error();
  return replaceObject(clip, *f, std::move(object), &ids, "Edit JI Object");
}

EditResult setJiStops(const Project& p, IdSource& ids, ClipRef clip, JiObjectId object, std::vector<JiStop> stops) {
  Result<JiFound> f = findJi(p, clip, object);
  if (!f) return std::move(f).error();
  JiObject o = *f->object;
  o.stops = std::move(stops);
  return replaceObject(clip, *f, std::move(o), &ids, "Edit JI Stops");
}

EditResult addJiStop(const Project& p, IdSource& ids, ClipRef clip, JiObjectId object, Ticks at,
                     std::span<const int64_t> terms, Ticks glide) {
  Result<JiFound> f = findJi(p, clip, object);
  if (!f) return std::move(f).error();
  PQ_DOC_TRY(checkStopTick(at));
  PQ_DOC_TRY(checkTerms(terms));
  if (glide < 0) return invalid("JI stop glide out of range");
  JiObject o = *f->object;
  JiStop s;
  s.at = at;
  s.terms.assign(terms.begin(), terms.end());
  s.timing.glide = glide;
  for (const JiStop& before : o.stops)
    if (before.at <= at) s.scale = before.scale;
  placeStop(o, std::move(s));
  return replaceObject(clip, *f, std::move(o), &ids, "Add JI Stop");
}

EditResult addJiTuningStop(const Project& p, ClipRef clip, JiObjectId object, Ticks at, Ticks glide) {
  Result<JiFound> f = findJi(p, clip, object);
  if (!f) return std::move(f).error();
  PQ_DOC_TRY(checkStopTick(at));
  if (glide < 0) return invalid("JI stop glide out of range");
  JiObject o = *f->object;
  JiStop s;
  s.at = at;
  s.timing.glide = glide;
  for (const JiStop& before : o.stops)
    if (before.at <= at) s.scale = before.scale;
  if (o.stops[0].isTuning()) {  // back to the chord as written
    s.pitches = o.stops[0].pitches;
    s.voiceTerm = o.stops[0].voiceTerm;
  } else {  // each voice to the nearest degree of the clip's tuning
    Result<tuning::Tuning> t = clipTuning(p, f->clip);
    if (!t) return std::move(t).error();
    const double ride = o.ride.empty() ? 0.0
                                       : pq::evalAutomation(std::span<const Breakpoint>(o.ride),
                                                            std::clamp(static_cast<double>(at), 0.0, static_cast<double>(o.length)), 0.0);
    for (uint32_t v = 0; v < o.voices(); ++v) {
      const std::optional<double> c = jiVoiceCents(o, *t, v, static_cast<double>(at));
      const std::optional<PitchId> d = c ? nearestDegreePitch(*t, *c - ride) : std::nullopt;
      if (!d) return invalid("the clip's tuning has no degree for a voice");
      s.pitches.push_back(*d);
    }
  }
  placeStop(o, std::move(s));
  return replaceObject(clip, *f, std::move(o), nullptr, "Add Tuning Stop");
}

EditResult walkJiTerm(const Project& p, ClipRef clip, JiObjectId object, uint32_t stop, uint32_t term, int32_t steps,
                      uint32_t pin) {
  Result<JiFound> f = findJi(p, clip, object);
  if (!f) return std::move(f).error();
  JiObject o = *f->object;
  if (stop >= o.stops.size()) return notFound("JI stop not found");
  JiStop& s = o.stops[stop];
  if (s.isTuning()) return invalid("a tuning stop has no terms to walk");
  if (term >= s.terms.size() || pin >= s.terms.size()) return notFound("JI term not found");
  const int64_t was = s.terms[term];
  const int64_t now = was + steps;
  if (now < 1 || now > kMaxJiTerm) return invalid("JI term out of range (1 to 2^20)");
  s.terms[term] = now;
  if (pin == term) {  // the pinned entry keeps scale · term (undertone: scale / term)
    const std::optional<Rational> scale = o.undertone ? tuning::mul(s.scale, Rational(now, was)) : tuning::mul(s.scale, Rational(was, now));
    if (!scale) return invalid("JI stop scale out of range");
    s.scale = *scale;
  }
  PQ_DOC_TRY(checkTerms(s.terms));
  return replaceObject(clip, *f, std::move(o), nullptr, "Walk JI Term");
}

EditResult setNoteCurve(const Project& p, ClipRef clip, NoteKey note, std::vector<Breakpoint> pitch) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  const Note* n = f->clip->notes.locate(note);
  if (!n) return notFound("note not found");
  PQ_DOC_TRY(checkCurve(pitch, "note curve"));
  Edit e = makeEdit("Note Curve");
  e.noteClip = clip;
  e.notes.push_back(keyOf(*n));
  const NoteCurves* have = findNoteCurves(*f->clip, n->id);
  if (have ? have->pitch != pitch : !pitch.empty()) batchOf(e).commands.push_back(SetNoteCurve{clip, keyOf(*n), std::move(pitch)});
  return e;
}

NoteClip cloneNoteClip(const NoteClip& c, IdSource& ids) { return cloneClip(c, ids, false); }

}  // namespace pq::doc::ops
