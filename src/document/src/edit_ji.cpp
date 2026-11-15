// JI group, warp and note-curve edit operations (T7.13; edit_ops.h "JI groups, warps and note curves").

#include <algorithm>
#include <cmath>
#include <numeric>

#include "edit_common.h"
#include "pq/document/note_bend.h"
#include "pq/document/pitch_resolve.h"

namespace pq::doc::ops {

using namespace detail;
using tuning::PitchId;
using tuning::PitchKind;
using tuning::Rational;

namespace {

struct GroupFound {
  ClipFound clip;
  const JiGroup* group = nullptr;
  const Note* anchor = nullptr;
};

Result<GroupFound> findGroup(const Project& p, const ClipRef& clip, JiGroupId group) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  const JiGroup* g = findJiGroup(*f->clip, group);
  if (!g) return notFound("JI group not found");
  const Note* anchor = f->clip->notes.findById(g->anchor);
  if (!anchor) return notFound("the JI group's anchor is not in the clip");
  return GroupFound{std::move(*f), g, anchor};
}

Edit groupEdit(const char* label, const ClipRef& clip) {
  Edit e = makeEdit(label);
  e.noteClip = clip;
  return e;
}

// The clip's group list with `g` put in (replacing the group with its id) or, with `remove`, taken out.
JiGroupList groupsWith(const NoteClip& c, const JiGroup& g, bool remove = false) {
  JiGroupList list;
  list.reserve(jiGroupsOf(c).size() + 1);
  for (const JiGroup& o : jiGroupsOf(c))
    if (o.id != g.id) list.push_back(o);
  if (!remove) list.push_back(g);
  return list;
}

void pushGroups(Edit& e, const ClipRef& clip, const NoteClip& c, JiGroupList list) {
  std::sort(list.begin(), list.end(), [](const JiGroup& a, const JiGroup& b) { return a.id < b.id; });
  if (!std::ranges::equal(list, jiGroupsOf(c))) batchOf(e).commands.push_back(SetJiGroups{clip, std::move(list)});
}

void pushWritten(Edit& e, const ClipRef& clip, const NoteClip& c, NoteWrittenList list) {
  std::sort(list.begin(), list.end(), [](const NoteWritten& a, const NoteWritten& b) { return a.note < b.note; });
  if (!std::ranges::equal(list, notesWrittenOf(c))) batchOf(e).commands.push_back(SetNotesWritten{clip, std::move(list)});
}

// A clip resolved for an operation that turns pitches into identities.
struct Resolved {
  tuning::Tuning tuning;
  ClipPitches pitches;
  NotePitchIndex hz;
  // A member's JI pitch (a warped member's start pitch may be its written one).
  std::optional<double> jiHz(NoteId id) const noexcept {
    if (const NoteWarpState* w = hz.warped(id)) return w->jiHz;
    return hz.hz(id);
  }
  std::optional<PitchId> centsAt(std::optional<double> v) const noexcept {
    if (!v || !(*v > 0.0)) return std::nullopt;
    const double c = 1200.0 * std::log2(*v / tuning.referenceHz());
    return std::isfinite(c) ? std::optional<PitchId>(PitchId::fromCents(c)) : std::nullopt;
  }
};

Result<Resolved> resolveFor(const Project& p, const ClipFound& f) {
  Result<tuning::Tuning> t = clipTuning(p, f);
  if (!t) return std::move(t).error();
  ClipPitches cp = resolveClipPitches(p, f.path, *f.clip);
  NotePitchIndex hz(cp);
  return Resolved{std::move(t).value(), std::move(cp), std::move(hz)};
}

// What a note that leaves its anchor becomes so that it keeps its JI pitch: the exact reference-anchored Ratio when
// the anchor's pitch is one (`anchorExact`), else Cents. nullopt: it does not resolve.
std::optional<PitchId> detached(const Note& n, const std::optional<Rational>& anchorExact, const Resolved& r) {
  if (anchorExact)
    if (const std::optional<Rational> x = tuning::mul(*anchorExact, n.pitch.ratio()); x && x->isPositive())
      return PitchId::fromRatio(*x);
  return r.centsAt(r.jiHz(n.id));
}

// removeJiGroup: the notes keep sounding where they are — in JI (a tuned group), or as written (T7.16a: a group Back
// to Tuning; its anchor too when it has a written identity).
EditResult dissolve(const Project& p, ClipRef clip, JiGroupId group, const char* label) {
  Result<GroupFound> f = findGroup(p, clip, group);
  if (!f) return std::move(f).error();
  const NoteClip& c = *f->clip.clip;
  const bool restoreWritten = !f->group->tuned;
  Edit e = groupEdit(label, clip);
  UpdateNotes u{clip, {}};
  std::optional<Resolved> r;  // resolved for the first member that needs a pitch
  std::optional<Rational> anchorExact;
  std::vector<NoteId> members;
  for (const Note& n : c.notes) {
    if (noteAnchorOf(n.pitch) != f->group->anchor) {
      if (n.id == f->group->anchor) {
        e.notes.push_back(keyOf(n));
        members.push_back(n.id);  // its written identity goes with the group
        if (const NoteWritten* w = restoreWritten ? findNoteWritten(c, n.id) : nullptr; w && !(w->written == n.pitch)) {
          Note m = n;
          m.pitch = w->written;
          u.updates.push_back({keyOf(n), m});
        }
      }
      continue;
    }
    members.push_back(n.id);
    e.notes.push_back(keyOf(n));
    std::optional<PitchId> pitch;
    if (const NoteWritten* w = restoreWritten ? findNoteWritten(c, n.id) : nullptr) {
      pitch = w->written;
    } else {
      if (!r) {
        Result<Resolved> res = resolveFor(p, f->clip);
        if (!res) return std::move(res).error();
        r = std::move(res).value();
        anchorExact = exactReferenceRatio(c, r->hz, f->anchor);
      }
      pitch = detached(n, anchorExact, *r);
    }
    if (!pitch) continue;  // it did not sound: left anchored to the note
    Note m = n;
    m.pitch = *pitch;
    u.updates.push_back({keyOf(n), m});
  }
  if (!u.updates.empty()) batchOf(e).commands.push_back(std::move(u));
  pushGroups(e, clip, c, groupsWith(c, *f->group, true));
  std::sort(members.begin(), members.end());
  NoteWrittenList written;
  for (const NoteWritten& w : notesWrittenOf(c))
    if (!std::binary_search(members.begin(), members.end(), w.note)) written.push_back(w);
  pushWritten(e, clip, c, std::move(written));
  return e;
}

// ---- Tune to JI -------------------------------------------------------------------------------------------------

// The selected notes as a chord: its distinct pitches as written, ascending; pitches[0] holds the anchor.
struct Chord {
  ClipFound clip;
  Resolved r;
  struct Pitch {
    double cents = 0.0;             // above the tuning's reference, as written
    std::optional<Rational> exact;  // its ratio to the anchor when it already is exact (kept in every reading)
    struct Member {
      const Note* note;
      bool exact;  // this note is that exact ratio
    };
    std::vector<Member> notes;
  };
  std::vector<Pitch> pitches;
  const Note* anchor = nullptr;
};

// A member that remembers what it was written as.
const NoteWritten* writtenOf(const NoteClip& c, const Note& n) noexcept {
  return noteAnchorOf(n.pitch).valid() ? findNoteWritten(c, n.id) : nullptr;
}

Result<Chord> chordOf(const Project& p, const ClipRef& clip, std::span<const NoteKey> keys) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  if (keys.empty()) return invalid("no notes");
  Result<Resolved> res = resolveFor(p, *f);
  if (!res) return std::move(res).error();
  Chord ch{std::move(*f), std::move(res).value(), {}, nullptr};
  const NoteClip& c = *ch.clip.clip;
  struct Entry {
    const Note* note;
    double cents;
  };
  std::vector<Entry> entries;
  std::vector<NoteId> seen;
  for (const NoteKey& k : keys) {
    const Note* n = c.notes.locate(k);
    if (!n) return notFound("note not found");
    if (std::find(seen.begin(), seen.end(), n->id) != seen.end()) continue;
    seen.push_back(n->id);
    // As written: a tuned member is read as what it was before it was tuned.
    std::optional<double> hz;
    if (const NoteWritten* w = writtenOf(c, *n)) hz = resolvePitch(w->written, ch.r.tuning, TuningId{}).hz;
    else hz = ch.r.hz.hz(n->id);
    if (!hz || !(*hz > 0.0)) return invalid("a note's pitch cannot be resolved");
    entries.push_back({n, 1200.0 * std::log2(*hz / ch.r.tuning.referenceHz())});
  }
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.cents != b.cents) return a.cents < b.cents;
    if (a.note->start != b.note->start) return a.note->start < b.note->start;
    return a.note->id < b.note->id;
  });
  ch.anchor = entries.front().note;
  // The anchor's pitch as an exact ratio of the reference (not for one that goes back to a written identity).
  const std::optional<Rational> anchorExact =
      writtenOf(c, *ch.anchor) ? std::nullopt : exactReferenceRatio(c, ch.r.hz, ch.anchor);
  // M9 (T7.16a): two notes on one chain of note anchors are an exact ratio apart, whatever the root is.
  const std::optional<RootedRatio> anchorRooted =
      writtenOf(c, *ch.anchor) ? std::nullopt : rootedRatio(c, ch.r.hz, ch.anchor);
  auto exactTo = [&](const Note& n) -> std::optional<Rational> {
    if (&n == ch.anchor || writtenOf(c, n)) return std::nullopt;
    if (noteAnchorOf(n.pitch) == ch.anchor->id) return n.pitch.ratio();
    if (anchorRooted)
      if (const std::optional<RootedRatio> own = rootedRatio(c, ch.r.hz, &n); own && own->root == anchorRooted->root)
        if (const std::optional<Rational> q = tuning::div(own->ratio, anchorRooted->ratio); q && q->isPositive()) return q;
    if (!anchorExact) return std::nullopt;
    const std::optional<Rational> own = exactReferenceRatio(c, ch.r.hz, &n);
    const std::optional<Rational> q = own ? tuning::div(*own, *anchorExact) : std::nullopt;
    return q && q->isPositive() ? q : std::nullopt;
  };
  for (const Entry& en : entries) {
    const std::optional<Rational> exact = exactTo(*en.note);
    if (ch.pitches.empty() || en.cents - ch.pitches.back().cents > 1e-6)
      ch.pitches.push_back({en.cents, ch.pitches.empty() ? std::nullopt : exact, {}});
    Chord::Pitch& pitch = ch.pitches.back();
    pitch.notes.push_back({en.note, exact && pitch.exact && *exact == *pitch.exact});
  }
  if (ch.pitches.size() > kMaxProportionTerms) return invalid("a proportion has at most 64 terms");
  return ch;
}

std::vector<JiCandidate> candidatesOf(const Chord& ch) {
  std::vector<JiCandidate> out;
  const size_t n = ch.pitches.size();
  // Every exact ratio needs a whole term: the lowest term is a multiple of their denominators' lcm.
  int64_t step = 1;
  for (size_t i = 1; i < n; ++i)
    if (ch.pitches[i].exact) {
      step = lcmCapped(step, ch.pitches[i].exact->den(), kMaxHarmonicIndex);
      if (step < 0) return out;
    }
  const int64_t last = std::max<int64_t>(kMaxJiCandidateBase, step);
  std::vector<int64_t> terms(n);
  for (int64_t a0 = step; a0 <= last; a0 += step) {
    terms[0] = a0;
    JiCandidate cand;
    bool fits = true;
    for (size_t i = 1; i < n && fits; ++i) {
      const Chord::Pitch& pi = ch.pitches[i];
      const double want = pi.cents - ch.pitches[0].cents;
      if (pi.exact) {
        terms[i] = pi.exact->num() * (a0 / pi.exact->den());
      } else {
        const double t = std::round(static_cast<double>(a0) * std::exp2(want / 1200.0));
        fits = t >= 1.0 && t <= static_cast<double>(kMaxFundamentalDenominator);
        if (!fits) break;
        terms[i] = static_cast<int64_t>(t);
        const double err = std::fabs(1200.0 * std::log2(t / static_cast<double>(a0)) - want);
        fits = err <= kTuneToJiToleranceCents;
        cand.error += err;
        cand.worst = std::max(cand.worst, err);
      }
      fits = fits && terms[i] > terms[i - 1];  // two written pitches are two terms
    }
    if (!fits) continue;
    int64_t g = 0;
    for (const int64_t t : terms) g = std::gcd(g, t);
    cand.terms = terms;
    for (int64_t& t : cand.terms) t /= g;
    if (std::ranges::any_of(out, [&](const JiCandidate& o) { return o.terms == cand.terms; })) continue;
    // Simplicity: the mean Tenney height log2(n · d) of the intervals between every two terms (4:5:6 → 5/4, 3/2, 6/5).
    double height = 0.0;
    for (size_t i = 0; i < n; ++i)
      for (size_t k = i + 1; k < n; ++k) {
        const int64_t d = std::gcd(cand.terms[i], cand.terms[k]);
        height += std::log2(static_cast<double>(cand.terms[i] / d) * static_cast<double>(cand.terms[k] / d));
      }
    const double pairs = static_cast<double>(n * (n - 1) / 2);
    cand.score = (n > 1 ? cand.error / static_cast<double>(n - 1) + kTuneToJiSimplicity * height / pairs : 0.0);
    out.push_back(std::move(cand));
  }
  std::stable_sort(out.begin(), out.end(), [](const JiCandidate& a, const JiCandidate& b) { return a.score < b.score; });
  if (out.size() > kMaxJiCandidates) out.resize(kMaxJiCandidates);
  return out;
}

Status checkTerms(std::span<const int64_t> terms) {
  if (terms.empty() || terms.size() > kMaxProportionTerms) return invalid("a proportion has 1 to 64 terms");
  for (size_t i = 0; i < terms.size(); ++i) {
    if (terms[i] < 1 || terms[i] > kMaxFundamentalDenominator) return invalid("proportion term out of range (1 to 2^20)");
    if (i > 0 && terms[i] <= terms[i - 1]) return invalid("the terms of a proportion ascend");
  }
  if (terms[0] > static_cast<int64_t>(kMaxHarmonicIndex)) return invalid("the lowest term of a proportion goes from 1 to 4096");
  return ok();
}

}  // namespace

EditResult makeJiGroup(const Project& p, IdSource& ids, ClipRef clip, NoteKey anchorNote, uint32_t index) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  const Note* n = f->clip->notes.locate(anchorNote);
  if (!n) return notFound("note not found");
  if (index < 1 || index > kMaxHarmonicIndex) return invalid("the harmonic number goes from 1 to 4096");
  Edit e = groupEdit("Is Harmonic", clip);
  e.notes.push_back(keyOf(*n));
  JiGroup g;
  if (const JiGroup* have = findJiGroupByAnchor(*f->clip, n->id)) {
    g = *have;
    if (g.index == index) {
      e.jiGroups.push_back(g.id);
      return e;
    }
  } else {
    g.id = ids.jiGroup();
    g.anchor = n->id;
  }
  g.index = index;
  e.jiGroups.push_back(g.id);
  pushGroups(e, clip, *f->clip, groupsWith(*f->clip, g));
  return e;
}

EditResult reanchorJiGroup(const Project& p, ClipRef clip, JiGroupId group, NoteKey member, uint32_t index) {
  Result<GroupFound> f = findGroup(p, clip, group);
  if (!f) return std::move(f).error();
  const NoteClip& c = *f->clip.clip;
  const Note* m = c.notes.locate(member);
  if (!m) return notFound("note not found");
  if (index < 1 || index > kMaxHarmonicIndex) return invalid("the harmonic number goes from 1 to 4096");
  if (noteAnchorOf(m->pitch) != f->group->anchor) return invalid("the note is not a member of the JI group");
  const Rational mr = m->pitch.ratio();
  Result<Resolved> res = resolveFor(p, f->clip);
  if (!res) return std::move(res).error();
  const std::optional<PitchId> frozen = detached(*m, exactReferenceRatio(c, res->hz, f->anchor), *res);
  if (!frozen) return invalid("the note's pitch cannot be resolved");
  Edit e = groupEdit("Is Harmonic", clip);
  e.jiGroups.push_back(group);
  UpdateNotes u{clip, {}};
  NoteWrittenList written(notesWrittenOf(c).begin(), notesWrittenOf(c).end());
  // T7.16a (M7 review M4): the new anchor keeps what it was written as (Back to Tuning plays it as written).
  std::erase_if(written, [&](const NoteWritten& w) { return w.note == f->anchor->id; });
  for (const Note& n : c.notes) {
    Note w = n;
    if (n.id == m->id) {
      w.pitch = *frozen;
    } else if (n.id == f->anchor->id) {
      const std::optional<Rational> inv = Rational::tryMake(mr.den(), mr.num());
      if (!inv || !inv->isPositive()) return invalid("ratio out of range");
      w.pitch = PitchId::fromRatio(*inv, tuning::AnchorKind::Note, m->id.value);
      if (const NoteWritten* was = findNoteWritten(c, n.id)) written.push_back(*was);  // it was a member made the anchor
      else if (plainPitch(n.pitch)) written.push_back(NoteWritten{n.id, n.pitch});
    } else if (noteAnchorOf(n.pitch) == f->anchor->id) {
      const std::optional<Rational> q = tuning::div(n.pitch.ratio(), mr);
      if (!q || !q->isPositive()) return invalid("ratio out of range");
      w.pitch = PitchId::fromRatio(*q, tuning::AnchorKind::Note, m->id.value);
    } else {
      continue;
    }
    e.notes.push_back(keyOf(n));
    if (!(w.pitch == n.pitch)) u.updates.push_back({keyOf(n), w});
  }
  JiGroup g = *f->group;
  g.anchor = m->id;
  g.index = index;
  g.warps = shiftedWarps(g.warps, m->start - f->anchor->start);  // they count from the anchor's start (M7)
  if (!u.updates.empty()) batchOf(e).commands.push_back(std::move(u));
  pushGroups(e, clip, c, groupsWith(c, g));
  pushWritten(e, clip, c, std::move(written));
  return e;
}

EditResult removeJiGroup(const Project& p, ClipRef clip, JiGroupId group) { return dissolve(p, clip, group, "Remove JI Group"); }

EditResult setJiGroupsTuned(const Project& p, ClipRef clip, std::span<const JiGroupId> groups, bool tuned) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  const NoteClip& c = *f->clip;
  Edit e = groupEdit(tuned ? "Tune to JI" : "Back to Tuning", clip);
  JiGroupList list(jiGroupsOf(c).begin(), jiGroupsOf(c).end());
  NoteWrittenList written(notesWrittenOf(c).begin(), notesWrittenOf(c).end());
  std::optional<Resolved> r;  // resolved for the first member that has no written identity
  std::vector<NoteId> anchors;
  for (const JiGroupId id : groups) {
    const auto it = std::find_if(list.begin(), list.end(), [&](const JiGroup& g) { return g.id == id; });
    if (it == list.end()) return notFound("JI group not found");
    if (c.notes.findById(it->anchor) == nullptr) return notFound("the JI group's anchor is not in the clip");
    it->tuned = tuned;
    e.jiGroups.push_back(id);
    anchors.push_back(it->anchor);
  }
  std::sort(anchors.begin(), anchors.end());
  for (const Note& n : c.notes) {
    const bool anchor = std::binary_search(anchors.begin(), anchors.end(), n.id);
    const NoteId of = noteAnchorOf(n.pitch);
    if (!anchor && !(of.valid() && std::binary_search(anchors.begin(), anchors.end(), of))) continue;
    e.notes.push_back(keyOf(n));
    if (tuned || anchor || findNoteWritten(c, n.id) != nullptr) continue;
    // M3: a member that was never written tempered (drawn on the guides, typed as a proportion, added at a ratio)
    // goes to the tuning's nearest step to its JI pitch, which it remembers as what it is written as.
    if (!r) {
      Result<Resolved> res = resolveFor(p, *f);
      if (!res) return std::move(res).error();
      r = std::move(res).value();
    }
    const std::optional<double> hz = r->jiHz(n.id);
    if (!hz || !(*hz > 0.0)) continue;  // it does not sound: nothing to go back to
    const double cents = 1200.0 * std::log2(*hz / r->tuning.referenceHz());
    std::optional<PitchId> step = nearestDegreePitch(r->tuning, cents);
    if (!step) step = r->centsAt(hz);  // a tuning without steps: it stays where it is
    if (step) written.push_back(NoteWritten{n.id, *step});
  }
  pushGroups(e, clip, c, std::move(list));
  pushWritten(e, clip, c, std::move(written));
  return e;
}

EditResult untuneJiGroup(const Project& p, ClipRef clip, JiGroupId group) {
  const JiGroupId one[] = {group};
  return setJiGroupsTuned(p, clip, one, false);
}

Result<std::vector<JiCandidate>> tuneToJiCandidates(const Project& p, ClipRef clip, std::span<const NoteKey> notes) {
  Result<Chord> ch = chordOf(p, clip, notes);
  if (!ch) return std::move(ch).error();
  return candidatesOf(*ch);
}

namespace {

// One chord of tuneToJi / tuneChordsToJi, applied to the working lists (`groups`, `written`) and `u`.
Status tuneChord(const Chord& ch, const TuneToJiOptions& options, IdSource& ids, Edit& e, UpdateNotes& u,
                 JiGroupList& groups, NoteWrittenList& written) {
  const NoteClip& c = *ch.clip.clip;
  const size_t n = ch.pitches.size();
  std::vector<int64_t> terms = options.proportion;
  if (!terms.empty()) {
    if (terms.size() != n) return invalid("the proportion has one term per distinct pitch of the notes");
  } else if (std::vector<JiCandidate> cands = candidatesOf(ch); !cands.empty()) {
    terms = std::move(cands.front().terms);
  } else {
    // No reading of the whole chord: each pitch its simplest ratio to the lowest.
    std::vector<Rational> ratios(n, Rational(1));
    int64_t L = 1;
    for (size_t i = 1; i < n; ++i) {
      const std::optional<Rational> q =
          ch.pitches[i].exact ? ch.pitches[i].exact
                              : simplestRatioNear(ch.pitches[i].cents - ch.pitches[0].cents,
                                                  ratioSnapTolerance(ch.r.tuning, ch.pitches[i].cents));
      if (!q || !q->isPositive()) return invalid("the chord has no proportion");
      ratios[i] = *q;
      L = lcmCapped(L, q->den(), kMaxHarmonicIndex);
      if (L < 0) return invalid("the chord has no proportion within the harmonic limit");
    }
    terms.resize(n);
    for (size_t i = 0; i < n; ++i) {
      const int64_t mult = L / ratios[i].den();
      if (ratios[i].num() > kMaxFundamentalDenominator / mult) return invalid("the chord has no proportion within the harmonic limit");
      terms[i] = ratios[i].num() * mult;
    }
  }
  PQ_DOC_TRY(checkTerms(terms));

  auto unwrite = [&](NoteId id) { std::erase_if(written, [&](const NoteWritten& w) { return w.note == id; }); };
  // The anchor: frozen where it is written. One that hung on another note is detached (what it was written as, else
  // its exact pitch), so no member can be its own anchor's anchor.
  const Note& a = *ch.anchor;
  if (noteAnchorOf(a.pitch).valid()) {
    std::optional<PitchId> pitch;
    if (const NoteWritten* w = findNoteWritten(c, a.id)) {
      pitch = w->written;
    } else {
      const Note* its = c.notes.findById(noteAnchorOf(a.pitch));
      pitch = detached(a, exactReferenceRatio(c, ch.r.hz, its), ch.r);
    }
    if (!pitch) return invalid("the lowest note's pitch cannot be resolved");
    Note m = a;
    m.pitch = *pitch;
    u.updates.push_back({keyOf(a), m});
    unwrite(a.id);
  }
  // (An anchor's own written identity — a tuned member made the anchor, reanchorJiGroup — is kept: Back to Tuning.)
  e.notes.push_back(keyOf(a));
  for (size_t i = 0; i < n; ++i) {
    const std::optional<Rational> ratio = Rational::tryMake(terms[i], terms[0]);
    if (!ratio || !ratio->isPositive()) return invalid("proportion term out of range");
    for (const Chord::Pitch::Member& member : ch.pitches[i].notes) {
      const Note& note = *member.note;
      if (&note == &a) continue;
      e.notes.push_back(keyOf(note));
      const PitchId pitch = PitchId::fromRatio(*ratio, tuning::AnchorKind::Note, a.id.value);
      if (pitch == note.pitch) continue;  // already this exact ratio to the anchor
      if (writtenOf(c, note) == nullptr) {
        // What it was written as (T7.16a: an exact ratio of the reference too, so Back to Tuning, a state of the
        // group, gives it back exactly).
        const NoteWritten* own = findNoteWritten(c, note.id);  // an anchor's (reanchorJiGroup): what it was written as
        const std::optional<PitchId> was = own ? std::optional<PitchId>(own->written)
                                               : plainPitch(note.pitch) ? std::optional<PitchId>(note.pitch)
                                                                        : ch.r.centsAt(ch.r.hz.hz(note.id));
        unwrite(note.id);
        if (!was) return invalid("a note's pitch cannot be resolved");
        written.push_back(NoteWritten{note.id, *was});
      }
      Note m = note;
      m.pitch = pitch;
      u.updates.push_back({keyOf(note), m});
    }
  }
  const auto have = std::find_if(groups.begin(), groups.end(), [&](const JiGroup& g) { return g.anchor == a.id; });
  if (have == groups.end()) {
    JiGroup g;
    g.id = ids.jiGroup();
    g.anchor = a.id;
    g.index = static_cast<uint32_t>(terms[0]);
    e.jiGroups.push_back(g.id);
    groups.push_back(std::move(g));
  } else {
    have->index = static_cast<uint32_t>(terms[0]);
    have->tuned = true;  // T7.16a: tuning is the JI state
    e.jiGroups.push_back(have->id);
  }
  return ok();
}

// The group every one of the chord's notes belongs to (its anchor or a member); nullptr: none, or several.
const JiGroup* oneGroupOf(const NoteClip& c, std::span<const NoteKey> keys) {
  const JiGroup* one = nullptr;
  for (const NoteKey& k : keys) {
    const Note* n = c.notes.locate(k);
    const JiGroup* g = n ? jiGroupOfNote(c, *n) : nullptr;
    if (!g || (one && g != one)) return nullptr;
    one = g;
  }
  return one;
}

}  // namespace

EditResult tuneToJi(const Project& p, IdSource& ids, ClipRef clip, std::span<const NoteKey> notes,
                    const TuneToJiOptions& options) {
  Result<Chord> chord = chordOf(p, clip, notes);
  if (!chord) return std::move(chord).error();
  const NoteClip& c = *chord->clip.clip;
  Edit e = groupEdit("Tune to JI", clip);
  UpdateNotes u{clip, {}};
  JiGroupList groups(jiGroupsOf(c).begin(), jiGroupsOf(c).end());
  NoteWrittenList written(notesWrittenOf(c).begin(), notesWrittenOf(c).end());
  PQ_DOC_TRY(tuneChord(*chord, options, ids, e, u, groups, written));
  if (!u.updates.empty()) batchOf(e).commands.push_back(std::move(u));
  pushGroups(e, clip, c, std::move(groups));
  pushWritten(e, clip, c, std::move(written));
  return e;
}

EditResult tuneChordsToJi(const Project& p, IdSource& ids, ClipRef clip, std::span<const std::vector<NoteKey>> chords) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  const NoteClip& c = *f->clip;
  Edit e = groupEdit("Tune to JI", clip);
  UpdateNotes u{clip, {}};
  JiGroupList groups(jiGroupsOf(c).begin(), jiGroupsOf(c).end());
  NoteWrittenList written(notesWrittenOf(c).begin(), notesWrittenOf(c).end());
  std::vector<JiGroupId> back;  // groups Back to Tuning whose notes are a chord: their own reading comes back
  for (const std::vector<NoteKey>& keys : chords) {
    if (keys.empty()) continue;
    if (const JiGroup* g = oneGroupOf(c, keys)) {
      if (!g->tuned && std::find(back.begin(), back.end(), g->id) == back.end()) back.push_back(g->id);
      continue;  // a tuned group's notes are tuned already
    }
    if (keys.size() < 2) continue;
    Result<Chord> chord = chordOf(p, clip, keys);
    if (!chord) return std::move(chord).error();
    PQ_DOC_TRY(tuneChord(*chord, {}, ids, e, u, groups, written));
  }
  for (const JiGroupId id : back)
    for (JiGroup& g : groups)
      if (g.id == id) {
        g.tuned = true;
        e.jiGroups.push_back(id);
      }
  for (const std::vector<NoteKey>& keys : chords)
    for (const NoteKey& k : keys)
      if (const Note* n = c.notes.locate(k)) e.notes.push_back(keyOf(*n));
  if (!u.updates.empty()) batchOf(e).commands.push_back(std::move(u));
  pushGroups(e, clip, c, std::move(groups));
  pushWritten(e, clip, c, std::move(written));
  return e;
}

EditResult addGroupNote(const Project& p, IdSource& ids, ClipRef clip, JiGroupId group, const GroupNotePitch& pitch,
                        Ticks start, Ticks length, float velocity) {
  Result<GroupFound> f = findGroup(p, clip, group);
  if (!f) return std::move(f).error();
  Rational ratio = pitch.ratio;
  if (pitch.harmonic != 0) {
    const std::optional<GroupFundamental> gf = groupFundamental(*f->clip.clip, *f->group);
    const std::optional<Rational> q =
        gf && pitch.harmonic > 0 && pitch.harmonic <= kMaxFundamentalDenominator ? Rational::tryMake(pitch.harmonic, gf->anchor)
                                                                                 : std::nullopt;
    if (!q) return invalid("harmonic number out of range");
    ratio = *q;
  }
  if (!ratio.isPositive()) return invalid("a ratio pitch must be positive");
  Note n;
  n.id = NoteId{1};  // validated before an id is drawn, so a refused note costs none
  n.start = start;
  n.length = length;
  n.velocity = velocity;
  n.pitch = PitchId::fromRatio(ratio, tuning::AnchorKind::Note, f->group->anchor.value);
  PQ_DOC_TRY(validateNote(n));
  n.id = ids.note();
  Edit e = groupEdit("Add Note", clip);
  e.notes.push_back(keyOf(n));
  e.jiGroups.push_back(group);
  batchOf(e).commands.push_back(AddNotes{clip, {n}});
  return e;
}

EditResult walkJiGroup(const Project& p, ClipRef clip, JiGroupId group, int32_t steps) {
  Result<GroupFound> f = findGroup(p, clip, group);
  if (!f) return std::move(f).error();
  const NoteClip& c = *f->clip.clip;
  const std::optional<GroupHarmonics> gh = groupHarmonics(c, *f->group);
  if (!gh) return notFound("the JI group's anchor is not in the clip");
  Edit e = groupEdit("Walk JI Group", clip);
  e.jiGroups.push_back(group);
  e.notes.push_back(keyOf(*f->anchor));
  const int64_t anchor = gh->anchor + steps;
  if (anchor < 1) return invalid("the group is on its first harmonic");
  if (anchor > static_cast<int64_t>(kMaxHarmonicIndex)) return invalid("the harmonic number goes from 1 to 4096");
  UpdateNotes u{clip, {}};
  size_t mi = 0;
  for (const Note& n : c.notes) {  // gh->members is in the clip's note order
    if (mi >= gh->members.size() || gh->members[mi].note != n.id) continue;
    const GroupMember& m = gh->members[mi++];
    if (m.harmonic < 1) return invalid("a note of the group has no harmonic number");
    const int64_t h = m.harmonic + steps;
    if (h < 1) return invalid("the group is on its first harmonic");
    const std::optional<Rational> ratio = Rational::tryMake(h, anchor);
    if (!ratio || !ratio->isPositive()) return invalid("harmonic number out of range");
    Note w = n;
    w.pitch = PitchId::fromRatio(*ratio, tuning::AnchorKind::Note, f->group->anchor.value);
    if (!(w.pitch == n.pitch)) u.updates.push_back({keyOf(n), w});
    e.notes.push_back(keyOf(n));
  }
  if (steps == 0) return e;
  JiGroup g = *f->group;
  g.index = static_cast<uint32_t>(anchor);
  if (!u.updates.empty()) batchOf(e).commands.push_back(std::move(u));
  pushGroups(e, clip, c, groupsWith(c, g));
  return e;
}

EditResult proportionNotes(const Project& p, IdSource& ids, ClipRef clip, Ticks start, Ticks length,
                           const PitchId& bottomPitch, std::span<const int64_t> terms, float velocity) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  PQ_DOC_TRY(checkTerms(terms));
  if (const NoteId on = noteAnchorOf(bottomPitch); on.valid() && f->clip->notes.findById(on) == nullptr)
    return notFound("the note the bottom pitch is anchored to is not in the clip");
  AddNotes add{clip, {}};
  add.notes.resize(terms.size());
  for (size_t i = 0; i < terms.size(); ++i) {  // validated before ids are drawn, so a refused proportion costs none
    Note& n = add.notes[i];
    n.id = NoteId{static_cast<uint32_t>(i + 1)};
    n.start = start;
    n.length = length;
    n.velocity = velocity;
    const std::optional<Rational> ratio = Rational::tryMake(terms[i], terms[0]);
    if (!ratio) return invalid("proportion term out of range");
    n.pitch = i == 0 ? bottomPitch : PitchId::fromRatio(*ratio, tuning::AnchorKind::Note, 1);
    PQ_DOC_TRY(validateNote(n));
  }
  Edit e = groupEdit("Add Proportion", clip);
  for (Note& n : add.notes) n.id = ids.note();
  const NoteId anchor = add.notes[0].id;
  for (size_t i = 1; i < add.notes.size(); ++i)
    add.notes[i].pitch = PitchId::fromRatio(add.notes[i].pitch.ratio(), tuning::AnchorKind::Note, anchor.value);
  for (const Note& n : add.notes) e.notes.push_back(keyOf(n));
  JiGroup g;
  g.id = ids.jiGroup();
  g.anchor = anchor;
  g.index = static_cast<uint32_t>(terms[0]);
  e.jiGroups.push_back(g.id);
  JiGroupList list = groupsWith(*f->clip, g);
  batchOf(e).commands.push_back(std::move(add));
  batchOf(e).commands.push_back(SetJiGroups{clip, std::move(list)});
  return e;
}

EditResult setJiWarps(const Project& p, ClipRef clip, JiGroupId group, std::vector<JiWarp> warps) {
  Result<GroupFound> f = findGroup(p, clip, group);
  if (!f) return std::move(f).error();
  JiGroup g = *f->group;
  g.warps = std::move(warps);
  PQ_DOC_TRY(checkJiGroup(g));
  Edit e = groupEdit("JI Warp", clip);
  e.jiGroups.push_back(group);
  pushGroups(e, clip, *f->clip.clip, groupsWith(*f->clip.clip, g));
  return e;
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

namespace {
Edit curveEdit(const char* label, ClipRef clip) {
  Edit e = makeEdit(label);
  e.noteClip = clip;
  return e;
}
}  // namespace

EditResult setNoteBend(const Project& p, ClipRef clip, NoteKey note, NoteEdge edge, Ticks length, GlideShape shape,
                       NoteId other) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  const Note* n = f->clip->notes.locate(note);
  if (!n) return notFound("note not found");
  NoteBend bend;
  if (other.valid() && length > 0) {
    if (other == n->id) return invalid("a note cannot bend to itself");
    if (!f->clip->notes.findById(other)) return notFound("the note to bend to is not in the clip");
    bend = NoteBend{std::clamp<Ticks>(length, 1, std::max<Ticks>(n->length, 1)), shape, other};
  }
  const NoteCurves* have = findNoteCurves(*f->clip, n->id);
  NoteCurves rec = have ? *have : NoteCurves{n->id, {}, {}, {}};
  (edge == NoteEdge::Start ? rec.bendIn : rec.bendOut) = bend;
  PQ_DOC_TRY(checkNoteCurveRecord(rec));
  Edit e = curveEdit(bend.set() ? "Bend Note" : "Clear Bend", clip);
  e.notes.push_back(keyOf(*n));
  if (have ? !(*have == rec) : !rec.empty()) batchOf(e).commands.push_back(SetNoteCurves{clip, {std::move(rec)}});
  return e;
}

EditResult clearNoteBends(const Project& p, ClipRef clip, std::span<const NoteKey> notes) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  Edit e = curveEdit("Clear Bends", clip);
  SetNoteCurves set{clip, {}};
  std::vector<NoteKey> keys(notes.begin(), notes.end());
  std::sort(keys.begin(), keys.end(), [](const NoteKey& x, const NoteKey& y) { return x.id < y.id; });
  keys.erase(std::unique(keys.begin(), keys.end(), [](const NoteKey& x, const NoteKey& y) { return x.id == y.id; }), keys.end());
  for (const NoteKey& k : keys) {
    const Note* n = f->clip->notes.locate(k);
    if (!n) return notFound("note not found");
    e.notes.push_back(keyOf(*n));
    const NoteCurves* have = findNoteCurves(*f->clip, n->id);
    if (!have || (!have->bendIn.set() && !have->bendOut.set())) continue;
    set.curves.push_back(NoteCurves{n->id, have->pitch, {}, {}, have->vibrato});
  }
  if (!set.curves.empty()) batchOf(e).commands.push_back(std::move(set));
  return e;
}

EditResult setNoteVibrato(const Project& p, ClipRef clip, std::span<const NoteVibratoEdit> edits) {
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  Edit e = curveEdit("Vibrato", clip);
  SetNoteCurves set{clip, {}};
  std::vector<NoteId> seen;
  for (const NoteVibratoEdit& ed : edits) {
    const Note* n = f->clip->notes.locate(ed.note);
    if (!n) return notFound("note not found");
    if (std::find(seen.begin(), seen.end(), n->id) != seen.end()) return invalid("two vibratos for one note");
    seen.push_back(n->id);
    e.notes.push_back(keyOf(*n));
    NoteVibrato v = ed.vibrato;
    if (!std::isfinite(v.depthCents) || !std::isfinite(v.rate)) return invalid("a vibrato's depth and rate are finite numbers");
    if (v.depthCents == 0.0f) {
      v = {};
    } else {
      const bool beats = v.unit == NoteVibrato::RateUnit::Beats;
      const Ticks L = std::max<Ticks>(n->length, 1);
      v.depthCents = std::clamp(v.depthCents, -kMaxVibratoCents, kMaxVibratoCents);
      v.rate = std::clamp(v.rate, beats ? kMinVibratoBeats : kMinVibratoHz, beats ? kMaxVibratoBeats : kMaxVibratoHz);
      v.onset = std::clamp<Ticks>(v.onset, 0, L - 1);
      if (v.end >= L || v.end <= v.onset) v.end = 0;
      const Ticks span = (v.end > 0 ? v.end : L) - v.onset;
      v.fadeIn = std::clamp<Ticks>(v.fadeIn, 0, span);
      v.fadeOut = std::clamp<Ticks>(v.fadeOut, 0, span - v.fadeIn);
    }
    const NoteCurves* have = findNoteCurves(*f->clip, n->id);
    NoteCurves rec = have ? *have : NoteCurves{n->id, {}, {}, {}, {}};
    rec.vibrato = v;
    PQ_DOC_TRY(checkNoteCurveRecord(rec));
    if (have ? !(*have == rec) : !rec.empty()) set.curves.push_back(std::move(rec));
  }
  if (!set.curves.empty()) batchOf(e).commands.push_back(std::move(set));
  return e;
}

NoteClip cloneNoteClip(const NoteClip& c, IdSource& ids) { return cloneClip(c, ids, false); }

}  // namespace pq::doc::ops
