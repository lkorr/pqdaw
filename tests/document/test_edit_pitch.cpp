// Identity-aware Up/Down (T3.6, SPEC §6.1): Degree ±1 grid step, Cents to the next display-grid line, Ratio /
// Harmonic to the next harmonic, Shift = one equave. On 31-EDO, 5L2s, a Scala scale, and Ratio 5/4 → 6/4.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "edit_helpers.h"

using namespace pq::doc;
using namespace edit_test;
using doctest_fixture::Fixture;
using pq::tuning::AnchorKind;
using pq::tuning::PitchId;
using pq::tuning::Rational;
using ops::StepUnit;

namespace {

pq::tuning::Tuning tuningOf(const TuningDef& d) {
  auto t = buildTuning(d);
  REQUIRE(t.ok());
  return std::move(t).value();
}

TuningDef edoDef(uint32_t n) {
  TuningDef d;
  d.system = pq::tuning::Edo::make(n).value();
  return d;
}

TuningDef mosDef(double generator = 696.0, double period = 1200.0, double stretch = 0.0) {
  TuningDef d;
  pq::tuning::MosParams m;
  m.generator = generator;
  m.period = period;
  m.octaveStretch = stretch;
  d.system = m;
  return d;
}

// Brute-force display-grid oracle: every degree of every equave in [−eqRange, eqRange].
double oracleNext(const pq::tuning::Tuning& t, double c, bool up, int eqRange) {
  double best = up ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
  auto consider = [&](double v) {
    if (up && v > c + 1e-6) best = std::min(best, v);
    if (!up && v < c - 1e-6) best = std::max(best, v);
  };
  for (int e = -eqRange; e <= eqRange; ++e) {
    if (const auto* edo = t.edo())
      for (uint32_t d = 0; d < edo->n(); ++d) consider(edo->centsOfStep(static_cast<int64_t>(e) * edo->n() + d));
    if (const auto* mos = t.mos())
      for (int32_t d = 0; d < mos->size(); ++d) consider(mos->centsOf(d, e));
    if (const auto* sc = t.scale())
      for (int32_t d = 0; d < sc->size(); ++d) consider(sc->centsOf(d, e));
  }
  return best;
}

std::optional<PitchId> step(const PitchId& p, int32_t n, StepUnit u, const pq::tuning::Tuning& t,
                            const pq::tuning::Edo* grid = nullptr, int64_t hd = 0) {
  return ops::stepPitch(p, n, u, ops::PitchStepContext{&t, grid, hd});
}

}  // namespace

TEST_CASE("pitch steps: Degree in 31-EDO, 5L2s and a Scala scale carry into the equave") {
  const auto e31 = tuningOf(edoDef(31));
  CHECK(step(PitchId::fromDegree(30, 0), 1, StepUnit::Grid, e31) == PitchId::fromDegree(0, 1));
  CHECK(step(PitchId::fromDegree(0, 0), -1, StepUnit::Grid, e31) == PitchId::fromDegree(30, -1));
  CHECK(step(PitchId::fromDegree(4, 0), 31, StepUnit::Grid, e31) == PitchId::fromDegree(4, 1));
  CHECK(step(PitchId::fromDegree(5, 2), -1, StepUnit::Equave, e31) == PitchId::fromDegree(5, 1));

  const auto mos = tuningOf(mosDef());
  REQUIRE(mos.mos()->size() == 7);
  CHECK(step(PitchId::fromDegree(6, 0), 1, StepUnit::Grid, mos) == PitchId::fromDegree(0, 1));
  CHECK(step(PitchId::fromDegree(0, 1), -1, StepUnit::Grid, mos) == PitchId::fromDegree(6, 0));
  CHECK(step(PitchId::fromDegree(3, 0), 1, StepUnit::Equave, mos) == PitchId::fromDegree(3, 1));

  const auto scala = tuningOf(doctest_fixture::fixtureScaleDef());
  REQUIRE(scala.scale()->size() == 4);
  CHECK(step(PitchId::fromDegree(2, 0), 1, StepUnit::Grid, scala) == PitchId::fromDegree(3, 0));
  CHECK(step(PitchId::fromDegree(3, 0), 1, StepUnit::Grid, scala) == PitchId::fromDegree(0, 1));
  CHECK(step(PitchId::fromDegree(0, 0), -2, StepUnit::Grid, scala) == PitchId::fromDegree(2, -1));
}

TEST_CASE("pitch steps: Cents go to the next display-grid line (oracle over every degree and equave)") {
  const pq::tuning::Tuning tunings[] = {tuningOf(edoDef(31)), tuningOf(mosDef()), tuningOf(mosDef(705.0, 1200.0, 7.5)),
                                        tuningOf(doctest_fixture::fixtureScaleDef())};
  const double starts[] = {0.0, 1e-9, 100.0, 386.3137138648348, 500.5, -250.0, 1199.9999, 2400.0, -3601.7};
  for (const auto& t : tunings) {
    for (double c : starts) {
      for (bool up : {true, false}) {
        CAPTURE(c);
        CAPTURE(up);
        const auto r = step(PitchId::fromCents(c), up ? 1 : -1, StepUnit::Grid, t);
        REQUIRE(r);
        CHECK(r->cents() == doctest::Approx(oracleNext(t, c, up, 16)).epsilon(1e-12));
      }
    }
  }
  // The non-monotonic Scala scale: degree 3 (~12357 ¢) folds to ~357 ¢ ten equaves down, between 9/8 and 386 ¢.
  const auto scala = tuningOf(doctest_fixture::fixtureScaleDef());
  const auto up1 = step(PitchId::fromCents(204.0), 1, StepUnit::Grid, scala);
  REQUIRE(up1);
  CHECK(up1->cents() == doctest::Approx(scala.scale()->centsOf(3, -10)));
  // Several steps = repeated single steps; a display-grid EDO overrides the tuning's lattice.
  const auto e31 = tuningOf(edoDef(31));
  CHECK(step(PitchId::fromCents(0.0), 3, StepUnit::Grid, e31)->cents() == doctest::Approx(3 * 1200.0 / 31));
  const auto mos = tuningOf(mosDef());
  const pq::tuning::Edo grid31 = pq::tuning::Edo::make(31).value();
  CHECK(step(PitchId::fromCents(0.0), 1, StepUnit::Grid, mos, &grid31)->cents() == doctest::Approx(1200.0 / 31));
  // Shift: ± one equave in cents (stretched for MOS).
  CHECK(step(PitchId::fromCents(10.0), 1, StepUnit::Equave, e31)->cents() == doctest::Approx(1210.0));
  const auto stretched = tuningOf(mosDef(696.0, 1200.0, 6.0));
  CHECK(step(PitchId::fromCents(10.0), -1, StepUnit::Equave, stretched)->cents() == doctest::Approx(10.0 - 1206.0));
}

TEST_CASE("pitch steps: Ratio moves to the next harmonic of its fundamental, Shift by the equave ratio") {
  const auto e31 = tuningOf(edoDef(31));
  const PitchId r54 = PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, 7);
  const auto up = step(r54, 1, StepUnit::Grid, e31);
  REQUIRE(up);
  CHECK(up->ratio() == Rational(6, 4));  // = 3/2
  CHECK(up->anchorKind() == AnchorKind::Note);
  CHECK(up->anchorId() == 7u);
  CHECK(step(r54, -1, StepUnit::Grid, e31)->ratio() == Rational(1));
  CHECK(step(r54, 3, StepUnit::Grid, e31)->ratio() == Rational(2));
  // A reduced 3/2 is the 3rd harmonic of 1/2; with a shared denominator 4 it is the 6th of 1/4.
  const PitchId r32 = PitchId::fromRatio(Rational(3, 2));
  CHECK(step(r32, 1, StepUnit::Grid, e31)->ratio() == Rational(2));
  CHECK(step(r32, 1, StepUnit::Grid, e31, nullptr, 4)->ratio() == Rational(7, 4));
  CHECK_FALSE(step(PitchId::fromRatio(Rational(1, 4)), -1, StepUnit::Grid, e31));
  // Equave: exact ratio of the EDO / Scala equave; a MOS's cents equave recognised as 2/1 or 3/1.
  CHECK(step(r54, 1, StepUnit::Equave, e31)->ratio() == Rational(5, 2));
  CHECK(step(r54, -2, StepUnit::Equave, e31)->ratio() == Rational(5, 16));
  const auto scala = tuningOf(doctest_fixture::fixtureScaleDef());
  CHECK(step(r54, 1, StepUnit::Equave, scala)->ratio() == Rational(5, 2));
  const auto mos = tuningOf(mosDef());
  CHECK(step(r54, 1, StepUnit::Equave, mos)->ratio() == Rational(5, 2));
  const auto tritave = tuningOf(mosDef(440.0, 1200.0 * std::log2(3.0)));
  CHECK(step(r54, 1, StepUnit::Equave, tritave)->ratio() == Rational(15, 4));
  const auto odd = tuningOf(mosDef(696.0, 1234.5678));
  CHECK_FALSE(step(r54, 1, StepUnit::Equave, odd));
}

TEST_CASE("pitch steps: Harmonic moves to the next voice; no equave") {
  const auto e31 = tuningOf(edoDef(31));
  CHECK(step(PitchId::fromHarmonic(3, 2), 1, StepUnit::Grid, e31) == PitchId::fromHarmonic(3, 3));
  CHECK(step(PitchId::fromHarmonic(3, 2), -2, StepUnit::Grid, e31) == PitchId::fromHarmonic(3, 0));
  CHECK_FALSE(step(PitchId::fromHarmonic(3, 0), -1, StepUnit::Grid, e31));
  CHECK_FALSE(step(PitchId::fromHarmonic(3, 0), 1, StepUnit::Equave, e31));
}

TEST_CASE("edit ops: Up/Down on documents in 31-EDO, 5L2s, a Scala scale and Ratio 5/4 → 6/4") {
  Fixture f;
  // 31-EDO (Lead): note 30 is degree 30.
  const ClipRef a{f.lead, f.clipA};
  const NoteKey k30[] = {f.keyA(30)};
  auto up = ops::transposeNotes(f.doc(), a, k30, 1, StepUnit::Grid);
  commit(f.store, up);
  CHECK(f.clip(f.lead, f.clipA).notes.find(up->notes[0])->pitch == PitchId::fromDegree(0, 1));
  auto sh = ops::transposeNotes(f.doc(), a, up->notes, -1, StepUnit::Equave);
  commit(f.store, sh);
  CHECK(f.clip(f.lead, f.clipA).notes.find(sh->notes[0])->pitch == PitchId::fromDegree(0, 0));

  // 5L2s (a clip on Bass in the MOS tuning).
  {
    REQUIRE(f.store.dispatch(SetTrackTuning{f.bass, TuningAssignment::mainTuning(f.tMos)}).ok());
    Note n;
    n.pitch = PitchId::fromDegree(6, 0);
    std::vector<Note> notes{n};
    notes[0].id = f.store.ids().note();
    ClipPtr c = doctest_fixture::makeClip(f.store.ids(), 0, kBar, notes);
    REQUIRE(f.store.dispatch(AddClip{f.bass, c}).ok());
    const NoteKey k[] = {keyOf(notes[0])};
    auto u = ops::transposeNotes(f.doc(), {f.bass, c->id}, k, 1, StepUnit::Grid);
    commit(f.store, u);
    CHECK(f.clip(f.bass, c->id).notes.find(u->notes[0])->pitch == PitchId::fromDegree(0, 1));
  }
  // A Scala scale as a clip override on the kick clip: degree 0 → 1 → 2 → 3 → 0 of the next equave.
  {
    const ClipRef kc{f.kick, f.clipK};
    REQUIRE(f.store.dispatch(SetClipTuning{kc, TuningAssignment::mainTuning(f.tScale)}).ok());
    std::vector<NoteKey> k{keyOf(*f.clip(f.kick, f.clipK).notes.begin())};
    for (int i = 0; i < 4; ++i) {
      auto u = ops::transposeNotes(f.doc(), kc, k, 1, StepUnit::Grid);
      commit(f.store, u);
      k = u->notes;
    }
    CHECK(f.clip(f.kick, f.clipK).notes.find(k[0])->pitch == PitchId::fromDegree(0, 1));
  }
  // Ratio 5/4 → 6/4 (31-EDO clip B); Shift+Up doubles.
  {
    const ClipRef b{f.lead, f.clipB};
    Note n;
    n.start = 3 * pq::kPpq;
    n.pitch = PitchId::fromRatio(Rational(5, 4));
    auto add = ops::addNotes(f.doc(), f.store.ids(), b, std::span<const Note>(&n, 1));
    commit(f.store, add);
    auto u = ops::transposeNotes(f.doc(), b, add->notes, 1, StepUnit::Grid);
    commit(f.store, u);
    CHECK(f.clip(f.lead, f.clipB).notes.find(u->notes[0])->pitch.ratio() == Rational(6, 4));
    auto s = ops::transposeNotes(f.doc(), b, u->notes, 1, StepUnit::Equave);
    commit(f.store, s);
    CHECK(f.clip(f.lead, f.clipB).notes.find(s->notes[0])->pitch.ratio() == Rational(3));
  }
  // Cents note in 31-EDO: Up to the next 31-EDO line.
  {
    const ClipRef b{f.lead, f.clipB};
    Note n;
    n.start = 3 * pq::kPpq + 1;
    n.pitch = PitchId::fromCents(50.0);
    auto add = ops::addNotes(f.doc(), f.store.ids(), b, std::span<const Note>(&n, 1));
    commit(f.store, add);
    auto u = ops::transposeNotes(f.doc(), b, add->notes, 1, StepUnit::Grid);
    commit(f.store, u);
    CHECK(f.clip(f.lead, f.clipB).notes.find(u->notes[0])->pitch.cents() == doctest::Approx(2 * 1200.0 / 31));
  }
}
