// T2.7: the main tunings model — named project tunings, track / clip assignment and override (Inherit through
// groups, Main, Private), per-tuning reference pitch, note resolution through the assignment (doc::resolveTrackPitches),
// re-resolution on reassignment and on tuning edits, and the JSON round trip of all of it.

#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "doc_fixture.h"
#include "pq/document/commands.h"
#include "pq/document/pitch_resolve.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/tuning/pitch_id.h"

using namespace doctest_fixture;
using pq::tuning::PitchId;

namespace {

Note pitched(IdSource& ids, pq::Ticks start, PitchId pitch) {
  Note n;
  n.id = ids.note();
  n.start = start;
  n.length = pq::kPpq / 2;
  n.pitch = pitch;
  return n;
}

// tunings: 12-EDO (default, 261.626 Hz), A = 5L2s MOS (mode 1, stretch 1.5, 440 Hz), B = 22-EDO (300 Hz),
//          C = a Scala scale (static pitches only)
// tracks:  G (group, Inherit) > T1 (MIDI, Inherit): clip X (Inherit; every pitch kind, anchors, a broken anchor,
//          an anchor cycle), clip Y (override: Main B)
//          T2 (MIDI, Main A): clip Z (degrees)
//          T3 (MIDI, Private 17-EDO at 200 Hz): clip W (degrees)
//          T4 (MIDI, Main C): clip V (degrees)
struct Project4 {
  DocumentStore store;
  TuningId t12, tA, tB, tC;
  TrackId g, t1, t2, t3, t4;
  ClipId x, y, z, w, v;
  NoteId n0, nAnchored, nBroken, nCycle1, nCycle2;

  Project4() {
    IdSource& ids = store.ids();
    t12 = store.current()->tunings->front()->id;
    tA = ids.tuning();
    tB = ids.tuning();
    tC = ids.tuning();
    g = ids.track();
    t1 = ids.track();
    t2 = ids.track();
    t3 = ids.track();
    t4 = ids.track();
    Batch b{"setup", {}};
    TuningSlot a{tA, "A: 5L2s", Colour{0xAA4422}, {}};
    pq::tuning::MosParams mp;
    mp.generator = 696.578428;
    mp.rotation = 1;
    mp.octaveStretch = 1.5;
    a.def.system = mp;
    a.def.referenceHz = 440.0;
    b.commands.push_back(AddTuning{a});
    TuningSlot sb{tB, "B: 22-EDO", Colour{0x22AA44}, {}};
    sb.def.system = pq::tuning::Edo::make(22).value();
    sb.def.referenceHz = 300.0;
    b.commands.push_back(AddTuning{sb});
    b.commands.push_back(AddTuning{TuningSlot{tC, "C: Scala", Colour{0x4422AA}, fixtureScaleDef()}});
    b.commands.push_back(AddTrack{makeTrackPtr(g, TrackKind::Group, "G"), {}, kAppend});
    b.commands.push_back(AddTrack{makeTrackPtr(t1, TrackKind::Midi, "T1"), g, kAppend});
    b.commands.push_back(AddTrack{makeTrackPtr(t2, TrackKind::Midi, "T2"), {}, kAppend});
    b.commands.push_back(AddTrack{makeTrackPtr(t3, TrackKind::Midi, "T3"), {}, kAppend});
    b.commands.push_back(AddTrack{makeTrackPtr(t4, TrackKind::Midi, "T4"), {}, kAppend});
    b.commands.push_back(SetTrackTuning{t2, TuningAssignment::mainTuning(tA)});
    TuningDef priv;
    priv.system = pq::tuning::Edo::make(17).value();
    priv.referenceHz = 200.0;
    b.commands.push_back(SetTrackTuning{t3, TuningAssignment::privateTuning(priv)});
    b.commands.push_back(SetTrackTuning{t4, TuningAssignment::mainTuning(tC)});

    std::vector<Note> xn;
    const Note first = pitched(ids, 0, PitchId::fromDegree(0));
    n0 = first.id;
    xn.push_back(first);
    for (int d = 1; d < 8; ++d) xn.push_back(pitched(ids, d * pq::kPpq, PitchId::fromDegree(d, d % 3 - 1)));
    xn.push_back(pitched(ids, 9 * pq::kPpq, PitchId::fromRatio(pq::tuning::Rational{3, 2})));
    const Note anchored = pitched(ids, 10 * pq::kPpq, PitchId::fromRatio(pq::tuning::Rational{5, 4},
                                                                         pq::tuning::AnchorKind::Note, first.id.value));
    nAnchored = anchored.id;
    xn.push_back(anchored);
    xn.push_back(pitched(ids, 11 * pq::kPpq,  // anchored to the anchored note: a chain
                         PitchId::fromRatio(pq::tuning::Rational{6, 5}, pq::tuning::AnchorKind::Note, anchored.id.value)));
    xn.push_back(pitched(ids, 12 * pq::kPpq, PitchId::fromCents(386.31371386)));
    const Note broken = pitched(ids, 13 * pq::kPpq,
                                PitchId::fromRatio(pq::tuning::Rational{7, 4}, pq::tuning::AnchorKind::Note, 999999));
    nBroken = broken.id;
    xn.push_back(broken);
    nCycle1 = ids.note();
    nCycle2 = ids.note();
    Note c1, c2;
    c1.id = nCycle1;
    c1.start = 14 * pq::kPpq;
    c1.pitch = PitchId::fromRatio(pq::tuning::Rational{3, 2}, pq::tuning::AnchorKind::Note, nCycle2.value);
    c2.id = nCycle2;
    c2.start = 15 * pq::kPpq;
    c2.pitch = PitchId::fromRatio(pq::tuning::Rational{2, 3}, pq::tuning::AnchorKind::Note, nCycle1.value);
    xn.push_back(c1);
    xn.push_back(c2);
    ClipPtr cx = makeClip(ids, 0, 4 * pq::kTicksPerWhole, xn, "X");
    x = cx->id;
    b.commands.push_back(AddClip{t1, cx});
    ClipPtr cy = makeClip(ids, 8 * pq::kTicksPerWhole, pq::kTicksPerWhole,
                          {makeNote(ids, 0, 0), makeNote(ids, pq::kPpq, 5), makeNote(ids, 2 * pq::kPpq, 21)}, "Y");
    y = cy->id;
    b.commands.push_back(AddClip{t1, cy});
    b.commands.push_back(SetClipTuning{{t1, y}, TuningAssignment::mainTuning(tB)});
    auto degrees = [&](const char* name) {
      std::vector<Note> ns;
      for (int d = -3; d < 10; ++d) ns.push_back(makeNote(ids, (d + 3) * pq::kPpq, d));
      return makeClip(ids, 0, 4 * pq::kTicksPerWhole, ns, name);
    };
    ClipPtr cz = degrees("Z"), cw = degrees("W"), cv = degrees("V");
    z = cz->id;
    w = cw->id;
    v = cv->id;
    b.commands.push_back(AddClip{t2, cz});
    b.commands.push_back(AddClip{t3, cw});
    b.commands.push_back(AddClip{t4, cv});
    const pq::Status st = store.dispatch(b);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  const Project& doc() const { return *store.current(); }
};

// The pitch a note should have in `def` (independent of resolveTrackPitches: resolveHzDouble with the anchor's Hz
// computed the same way), or nullopt.
std::optional<double> expectedHz(const NoteClip& clip, const Note& n, const TuningDef& def, int depth = 0) {
  if (depth > 8) return std::nullopt;
  auto t = buildTuning(def);
  REQUIRE(t.ok());
  pq::tuning::ResolveContext rc;
  if (n.pitch.kind() == pq::tuning::PitchKind::Ratio && n.pitch.anchorKind() == pq::tuning::AnchorKind::Note) {
    const Note* a = nullptr;
    for (const Note& m : clip.notes)
      if (m.id.value == n.pitch.anchorId()) a = &m;
    if (!a) return std::nullopt;
    const auto ah = expectedHz(clip, *a, def, depth + 1);
    if (!ah) return std::nullopt;
    rc.anchorHz = *ah;
  }
  return pq::tuning::resolveHzDouble(n.pitch, t.value(), rc);
}

// How a note follows its clip's tuning signal (T2.6-fix3; independent of resolveTrackPitches): Degree notes and Ratio
// notes anchored through notes to one follow its degree; Ratio-on-reference and Cents notes (and notes anchored to
// them) follow the reference; the rest are static.
struct Follow {
  PitchFollow follow = PitchFollow::Static;
  int32_t degree = 0, equave = 0;
};
Follow expectedFollow(const NoteClip& clip, const Note& n, int depth = 0) {
  using pq::tuning::AnchorKind;
  using pq::tuning::PitchKind;
  if (depth > 8) return {};
  switch (n.pitch.kind()) {
    case PitchKind::Degree: return {PitchFollow::Degree, n.pitch.degree(), n.pitch.equave()};
    case PitchKind::Cents: return {PitchFollow::Reference};
    case PitchKind::Ratio:
      if (n.pitch.anchorKind() == AnchorKind::Reference) return {PitchFollow::Reference};
      if (n.pitch.anchorKind() == AnchorKind::Note)
        for (const Note& m : clip.notes)
          if (m.id.value == n.pitch.anchorId()) return expectedFollow(clip, m, depth + 1);
      return {};
    case PitchKind::Harmonic: return {};
  }
  return {};
}

const NoteClip& clipOf(const Project& p, TrackId t, ClipId c) {
  const Track& tr = *findTrackPtr(p, t);
  return *tr.clips[static_cast<size_t>(findClipIndex(tr, c))].clip;
}

// Checks every note of `clip` on `track` against `def` (main tuning `main`, or invalid for a private one).
void checkClip(const Project& p, const TrackPitches& tp, TrackId track, ClipId clip, const TuningDef& def,
               TuningId main, NoteId broken = {}, NoteId cycle1 = {}, NoteId cycle2 = {}) {
  const ClipPitches* cp = nullptr;
  for (const ClipPitches& c : tp.clips)
    if (c.clip == clip) cp = &c;
  REQUIRE(cp != nullptr);
  CHECK(cp->tuning.def == &def);
  CHECK(cp->tuning.main == main);
  const NoteClip& nc = clipOf(p, track, clip);
  REQUIRE(cp->notes.size() == nc.notes.size());
  size_t i = 0;
  for (const Note& n : nc.notes) {
    const NotePitch& np = cp->notes[i++];
    CAPTURE(n.id.value);
    CHECK(np.note == n.id);
    if (n.id == broken || n.id == cycle1 || n.id == cycle2) {
      CHECK_FALSE(np.hz.has_value());
      continue;
    }
    const auto want = expectedHz(nc, n, def);
    REQUIRE(want.has_value());
    REQUIRE(np.hz.has_value());
    CHECK(*np.hz == *want);  // the same double computation
    // T4.17 (M4 review M7): the degree an Instrument Rack's Degree zones see, in the clip's own tuning: a Degree
    // identity's absolute degree; any other pitch between the two degrees around it, linear in log frequency.
    REQUIRE(np.keyDegree.has_value());
    {
      auto t = buildTuning(def);
      REQUIRE(t.ok());
      const auto hzOf = [&](double d) {
        return *pq::tuning::resolveHzDouble(PitchId::fromDegree(static_cast<int32_t>(d), 0), t.value(), {});
      };
      const double kd = *np.keyDegree;
      if (n.pitch.kind() == pq::tuning::PitchKind::Degree) {
        CHECK(kd == std::round(kd));
        CHECK(hzOf(kd) == doctest::Approx(*np.hz).epsilon(1e-12));
      } else {
        const double lo = std::floor(kd);
        CHECK(hzOf(lo) <= *np.hz);
        CHECK(hzOf(lo + 1) > *np.hz);
        CHECK(hzOf(lo) * std::pow(hzOf(lo + 1) / hzOf(lo), kd - lo) == doctest::Approx(*np.hz).epsilon(1e-9));
      }
    }
    // T2.6-fix3: Ratio-on-note, Ratio-on-reference and Cents notes follow the signal too (they used to be static).
    const Follow f = main.valid() && hasTuningSignal(def) ? expectedFollow(nc, n) : Follow{};
    CHECK(np.follow == f.follow);
    CHECK(np.signal == (f.follow != PitchFollow::Static ? main : TuningId{}));
    if (f.follow == PitchFollow::Degree) {
      CHECK(np.degree == f.degree);
      CHECK(np.equave == f.equave);
    }
  }
}

const TuningDef& defOf(const Project& p, TuningId id) { return findTuning(p, id)->def; }

TrackPitches resolve(const Project& p, TrackId t) {
  auto r = resolveTrackPitches(p, t);
  REQUIRE(r.ok());
  return std::move(r.value());
}

}  // namespace

TEST_CASE("main tunings: every note resolves in its clip's effective tuning (Inherit, Main, Private, override)") {
  Project4 f;
  const Project& p = f.doc();
  // T1 inherits from G, which inherits the project's first main tuning; clip Y overrides with B.
  checkClip(p, resolve(p, f.t1), f.t1, f.x, defOf(p, f.t12), f.t12, f.nBroken, f.nCycle1, f.nCycle2);
  checkClip(p, resolve(p, f.t1), f.t1, f.y, defOf(p, f.tB), f.tB);
  checkClip(p, resolve(p, f.t2), f.t2, f.z, defOf(p, f.tA), f.tA);  // a MOS main tuning: Degree notes follow it
  const TuningDef& priv = *findTrackPtr(p, f.t3)->tuning.privateDef;
  checkClip(p, resolve(p, f.t3), f.t3, f.w, priv, TuningId{});  // private: static pitches, at its own reference
  checkClip(p, resolve(p, f.t4), f.t4, f.v, defOf(p, f.tC), f.tC);  // a Scala main tuning: no signal
  // Per-tuning reference: degree 0 sounds at each tuning's own reference.
  CHECK(*resolve(p, f.t2).clips[0].notes[3].hz == doctest::Approx(440.0).epsilon(1e-12));
  CHECK(*resolve(p, f.t3).clips[0].notes[3].hz == 200.0);
  CHECK(*resolve(p, f.t1).clips[1].notes[0].hz == 300.0);
  CHECK(effectiveTuningRef(p, *findTrack(p, f.g)).main == f.t12);
  CHECK_FALSE(resolveTrackPitches(p, TrackId{123456}).ok());
}

TEST_CASE("main tunings: reassigning a track re-resolves all its notes; tuning edits re-resolve their followers") {
  Project4 f;
  const TrackPitches before = resolve(f.doc(), f.t1);

  // T1 → Main A: clip X (Inherit) follows A; the override on Y stays B.
  REQUIRE(f.store.dispatch(SetTrackTuning{f.t1, TuningAssignment::mainTuning(f.tA)}).ok());
  {
    const Project& p = f.doc();
    const TrackPitches after = resolve(p, f.t1);
    checkClip(p, after, f.t1, f.x, defOf(p, f.tA), f.tA, f.nBroken, f.nCycle1, f.nCycle2);
    checkClip(p, after, f.t1, f.y, defOf(p, f.tB), f.tB);
    // Every resolvable note of X changed pitch (all identities depend on the tuning or its reference), Y's did not.
    size_t changed = 0, resolvable = 0;
    for (size_t i = 0; i < after.clips[0].notes.size(); ++i) {
      if (!after.clips[0].notes[i].hz) continue;
      ++resolvable;
      changed += *after.clips[0].notes[i].hz != *before.clips[0].notes[i].hz;
    }
    CHECK(resolvable == after.clips[0].notes.size() - 3);
    CHECK(changed == resolvable);
    for (size_t i = 0; i < after.clips[1].notes.size(); ++i)
      CHECK(*after.clips[1].notes[i].hz == *before.clips[1].notes[i].hz);
  }

  // Back to Inherit, then the group G → Main B: T1's inheriting clip follows the group.
  REQUIRE(f.store.dispatch(SetTrackTuning{f.t1, TuningAssignment::inherit()}).ok());
  REQUIRE(f.store.dispatch(SetTrackTuning{f.g, TuningAssignment::mainTuning(f.tB)}).ok());
  checkClip(f.doc(), resolve(f.doc(), f.t1), f.t1, f.x, defOf(f.doc(), f.tB), f.tB, f.nBroken, f.nCycle1, f.nCycle2);

  // A private clip override: static pitches in its own tuning, no signal.
  TuningDef priv;
  priv.system = pq::tuning::Edo::make(5).value();
  priv.referenceHz = 100.0;
  REQUIRE(f.store.dispatch(SetClipTuning{{f.t1, f.x}, TuningAssignment::privateTuning(priv)}).ok());
  {
    const Project& p = f.doc();
    const NoteClip& cx = clipOf(p, f.t1, f.x);
    checkClip(p, resolve(p, f.t1), f.t1, f.x, *cx.tuning.privateDef, TuningId{}, f.nBroken, f.nCycle1, f.nCycle2);
  }

  // Editing main tuning A's reference re-resolves T2 (which follows A): every degree scales by 450 / 440.
  const TrackPitches z0 = resolve(f.doc(), f.t2);
  REQUIRE(f.store.dispatch(SetTuningParam{f.tA, ParamKind::TuningReference, 450.0}).ok());
  const TrackPitches z1 = resolve(f.doc(), f.t2);
  checkClip(f.doc(), z1, f.t2, f.z, defOf(f.doc(), f.tA), f.tA);
  for (size_t i = 0; i < z1.clips[0].notes.size(); ++i)
    CHECK(*z1.clips[0].notes[i].hz / *z0.clips[0].notes[i].hz == doctest::Approx(450.0 / 440.0).epsilon(1e-14));

  // Undo restores the old assignments and so the old pitches.
  for (int i = 0; i < 5; ++i) REQUIRE(f.store.undo());
  const TrackPitches restored = resolve(f.doc(), f.t1);
  REQUIRE(restored.clips.size() == before.clips.size());
  for (size_t c = 0; c < restored.clips.size(); ++c)
    for (size_t i = 0; i < restored.clips[c].notes.size(); ++i) {
      CHECK(restored.clips[c].notes[i].hz == before.clips[c].notes[i].hz);
      CHECK(restored.clips[c].notes[i].signal == before.clips[c].notes[i].signal);
    }
}

TEST_CASE("main tunings: the JSON round trip keeps tunings, references, assignments and every resolved pitch") {
  Project4 f;
  REQUIRE(f.store.dispatch(SetTuningParam{f.tA, ParamKind::MosGenerator, 701.955}).ok());
  const Project& p = f.doc();
  const auto text = serializeProject(p);
  REQUIRE(text.ok());
  const auto loaded = parseProject(text.value());
  REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
  const Project& q = *loaded.value().project;
  CHECK(q == p);
  // Named tunings with their references, in order.
  REQUIRE(q.tunings->size() == 4);
  CHECK((*q.tunings)[1]->name == "A: 5L2s");
  CHECK((*q.tunings)[1]->def.referenceHz == 440.0);
  CHECK((*q.tunings)[1]->def.mos()->generator == 701.955);
  CHECK((*q.tunings)[2]->def.referenceHz == 300.0);
  // Assignments: Inherit (G, T1, clip X), Main (T2, T4, clip Y), Private (T3).
  CHECK(findTrackPtr(q, f.g)->tuning.mode == TuningMode::Inherit);
  CHECK(findTrackPtr(q, f.t1)->tuning.mode == TuningMode::Inherit);
  CHECK(findTrackPtr(q, f.t2)->tuning == TuningAssignment::mainTuning(f.tA));
  CHECK(clipOf(q, f.t1, f.y).tuning == TuningAssignment::mainTuning(f.tB));
  CHECK(findTrackPtr(q, f.t3)->tuning.mode == TuningMode::Private);
  CHECK(findTrackPtr(q, f.t3)->tuning.privateDef->referenceHz == 200.0);
  // Every track resolves identically after the round trip (exact doubles), and the text is stable.
  for (TrackId t : {f.t1, f.t2, f.t3, f.t4}) {
    const TrackPitches a = resolve(p, t), b = resolve(q, t);
    REQUIRE(a.clips.size() == b.clips.size());
    for (size_t c = 0; c < a.clips.size(); ++c) {
      CHECK(a.clips[c].tuning.main == b.clips[c].tuning.main);
      REQUIRE(a.clips[c].notes.size() == b.clips[c].notes.size());
      for (size_t i = 0; i < a.clips[c].notes.size(); ++i) {
        CHECK(a.clips[c].notes[i].hz == b.clips[c].notes[i].hz);
        CHECK(a.clips[c].notes[i].signal == b.clips[c].notes[i].signal);
      }
    }
  }
  const auto again = serializeProject(q);
  REQUIRE(again.ok());
  CHECK(again.value() == text.value());
}

TEST_CASE("main tunings: a definition outside the tuning params' ranges has no signal (static pitches; M3 review L9)") {
  // The tuning node's params are declared with paramRange and the program clamps them to it, so a definition outside
  // it would sound clamped through a node but exact as a static pitch: hasTuningSignal says no.
  TuningDef mos;
  pq::tuning::MosParams m;
  m.generator = 700.0;
  mos.system = m;
  CHECK(hasTuningSignal(mos));
  TuningDef far = mos;
  std::get<pq::tuning::MosParams>(far.system).generator = 12500.0;  // valid for Mos::make, beyond [-12000, 12000]
  CHECK(buildTuning(far).ok());
  CHECK_FALSE(hasTuningSignal(far));
  TuningDef highRef = mos;
  highRef.referenceHz = 25000.0;
  CHECK(buildTuning(highRef).ok());
  CHECK_FALSE(hasTuningSignal(highRef));
  TuningDef wideEdo;
  wideEdo.system = pq::tuning::Edo::makeCents(12, 15000.0).value();
  CHECK_FALSE(hasTuningSignal(wideEdo));
  TuningDef edo;
  CHECK(hasTuningSignal(edo));
}
