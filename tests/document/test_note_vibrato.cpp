// T7.14: vibrato as a property of the note (model.h NoteVibrato, note_bend.h vibrato(t); plan M7 V1–V3) and
// "Is Harmonic N" on a member (ops::reanchorJiGroup; QUESTIONS Q297 b).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <variant>
#include <vector>

#include "note_fix.h"

using namespace edit_test;
using note_fix::BendFix;
using note_fix::centsOf;
using pq::kPpq;
using pq::tuning::AnchorKind;
using pq::tuning::PitchId;
using pq::tuning::PitchKind;
using pq::tuning::Rational;

namespace {

double polyline(const std::vector<Breakpoint>& pts, double x) {
  REQUIRE(!pts.empty());
  auto it = std::upper_bound(pts.begin(), pts.end(), x, [](double v, const Breakpoint& p) { return v < static_cast<double>(p.tick); });
  if (it == pts.begin()) return pts.front().value;
  const Breakpoint& a = *(it - 1);
  if (it == pts.end() || static_cast<double>(a.tick) == x) return a.value;
  return a.value + (it->value - a.value) * (x - static_cast<double>(a.tick)) / static_cast<double>(it->tick - a.tick);
}

// vibrato(t) as note_bend.h defines it.
double expected(const NoteVibrato& v, Ticks length, double period, double t) {
  const double on = static_cast<double>(v.onset), stop = static_cast<double>(v.end > 0 ? std::min(v.end, length) : length);
  if (t <= on || t >= stop) return 0.0;
  double a = 1.0;
  if (v.fadeIn > 0) a = std::min(a, (t - on) / static_cast<double>(v.fadeIn));
  if (v.fadeOut > 0) a = std::min(a, (stop - t) / static_cast<double>(v.fadeOut));
  return static_cast<double>(v.depthCents) * a * std::sin(2.0 * std::numbers::pi * (t - on) / period);
}

ops::NoteVibratoEdit edit(const Note& n, NoteVibrato v) { return {keyOf(n), v}; }

}  // namespace

TEST_CASE("vibrato: a property of the note — sine from the onset, the four-corner envelope, Hz and beats; one undo step for many notes") {
  BendFix f;
  const Ticks L = 8 * kPpq;
  const Note a = f.add(0, PitchId::fromDegree(0), L);
  const Note b = f.add(0, PitchId::fromDegree(7), L);
  NoteVibrato v;
  v.depthCents = 35.0f;
  v.rate = 5.5f;
  v.unit = NoteVibrato::RateUnit::Hz;
  v.onset = kPpq;
  v.fadeIn = 2 * kPpq;
  v.end = 7 * kPpq;
  v.fadeOut = kPpq;
  NoteVibrato beats = v;
  beats.unit = NoteVibrato::RateUnit::Beats;
  beats.rate = 2.0f;  // two cycles per quarter
  const size_t h = f.store.history().cursor();
  const ops::NoteVibratoEdit both[] = {edit(a, v), edit(b, beats)};
  commit(f.store, ops::setNoteVibrato(f.doc(), f.ref, both));
  CHECK(f.store.history().cursor() == h + 1);
  const NoteCurves* na = findNoteCurves(f.clip(), a.id);
  REQUIRE(na != nullptr);
  CHECK(na->vibrato == v);
  CHECK(na->pitch.empty());  // nothing is baked
  CHECK(findNoteCurves(f.clip(), b.id)->vibrato == beats);

  // The period: the fixture plays at its tempo; beats need none.
  const ClipPitches cp = f.pitches(f.clip());
  const NotePitchIndex index(cp);
  const double tps = clipTicksPerSecond(f.doc(), f.clip(), 0);
  CHECK(index.vibratoPeriod(a.id) == doctest::Approx(tps / 5.5));
  CHECK(index.vibratoPeriod(b.id) == doctest::Approx(static_cast<double>(kPpq) / 2.0));
  CHECK(index.vibratoPeriod(NoteId{999999}) == 0.0);

  for (const Note* n : {&a, &b}) {
    const NoteVibrato& nv = n == &a ? v : beats;
    const double period = index.vibratoPeriod(n->id);
    const std::vector<Breakpoint> pts = f.effective(f.clip(), *n, 0.05);
    REQUIRE(pts.size() > 8);
    CHECK(pts.front().tick == 0);
    CHECK(pts.back().tick == L);
    double worst = 0.0;
    for (Ticks t = 0; t <= L; t += 7) worst = std::max(worst, std::fabs(polyline(pts, static_cast<double>(t)) - expected(nv, L, period, static_cast<double>(t))));
    CHECK(worst <= 0.06);
    // Silent before the onset and after the end; phase 0 at the onset, rising first; the full depth between the fades.
    CHECK(polyline(pts, static_cast<double>(kPpq) - 1.0) == 0.0);
    CHECK(polyline(pts, static_cast<double>(7 * kPpq) + 1.0) == 0.0);
    CHECK(polyline(pts, static_cast<double>(kPpq) + 0.1 * period) > 0.0);
    float peak = 0.0f, early = 0.0f;
    for (const Breakpoint& p : pts) {
      peak = std::max(peak, std::fabs(p.value));
      if (p.tick < 2 * kPpq) early = std::max(early, std::fabs(p.value));
    }
    CHECK(peak == doctest::Approx(35.0f).epsilon(0.01));
    CHECK(early < 0.5f * 35.0f + 0.5f);  // half way through the fade-in
    // The single-tick query agrees with the flattened curve.
    const NoteCurves& nc = *findNoteCurves(f.clip(), n->id);
    const NoteBendTargets targets = noteBendTargets(nc, index);
    CHECK(targets.vibratoPeriod == period);
    CHECK(targets.any());
    CHECK(noteCurveCents(nc, L, targets, 3.3 * kPpq) == doctest::Approx(expected(nv, L, period, 3.3 * kPpq)).epsilon(1e-9));
  }
  // What the lowering asks for (1 ¢): a few points per cycle, not hundreds.
  const std::vector<Breakpoint> coarse = f.effective(f.clip(), a, 1.0);
  const double cycles = static_cast<double>(6 * kPpq) / index.vibratoPeriod(a.id);
  CHECK(static_cast<double>(coarse.size()) <= 20.0 * cycles + 8.0);
  CHECK(static_cast<double>(coarse.size()) >= 4.0 * cycles);
  f.roundTrips();

  // Undo removes both; redo brings them back.
  REQUIRE(f.store.undo());
  CHECK(f.clip().curves == nullptr);
  REQUIRE(f.store.redo());
  CHECK(findNoteCurves(f.clip(), a.id)->vibrato == v);
  // Depth 0 removes it (and the record with it).
  const ops::NoteVibratoEdit off[] = {edit(a, NoteVibrato{})};
  commit(f.store, ops::setNoteVibrato(f.doc(), f.ref, off));
  CHECK(findNoteCurves(f.clip(), a.id) == nullptr);
  CHECK(f.effective(f.clip(), a).empty());
  // Setting the same vibrato again is no command.
  const ops::NoteVibratoEdit same[] = {edit(b, beats)};
  auto none = ops::setNoteVibrato(f.doc(), f.ref, same);
  REQUIRE(none.ok());
  CHECK(none->empty());
}

TEST_CASE("vibrato: a swell (the inner corners meet), end 0 follows a resize, values are clamped, validation") {
  BendFix f;
  const Ticks L = 4 * kPpq;
  const Note a = f.add(0, PitchId::fromDegree(0), L);
  NoteVibrato v;
  v.depthCents = 50.0f;
  v.rate = 4.0f;
  v.unit = NoteVibrato::RateUnit::Beats;
  v.fadeIn = 2 * kPpq;
  v.fadeOut = 2 * kPpq;  // the corners meet in the middle
  const ops::NoteVibratoEdit one[] = {edit(a, v)};
  commit(f.store, ops::setNoteVibrato(f.doc(), f.ref, one));
  CHECK(findNoteCurves(f.clip(), a.id)->vibrato == v);
  std::vector<Breakpoint> pts = f.effective(f.clip(), f.clip().notes.findById(a.id)[0]);
  float peak = 0.0f;
  Ticks at = 0;
  for (const Breakpoint& p : pts)
    if (std::fabs(p.value) > peak) {
      peak = std::fabs(p.value);
      at = p.tick;
    }
  CHECK(peak > 45.0f);
  CHECK(std::abs(at - 2 * kPpq) <= kPpq / 4);
  CHECK(std::fabs(polyline(pts, 0.25 * kPpq)) < 8.0);
  CHECK(std::fabs(polyline(pts, 3.75 * kPpq)) < 8.0);

  // end 0 = the note's end: after a resize the fade-out still ends on the note's end.
  const NoteKey key = keyOf(a);
  commit(f.store, ops::resizeNotes(f.doc(), f.ref, std::span<const NoteKey>(&key, 1), ops::NoteEdge::End, 4 * kPpq));
  const Note& longer = *f.clip().notes.findById(a.id);
  REQUIRE(longer.length == 8 * kPpq);
  pts = f.effective(f.clip(), longer);
  CHECK(pts.back().tick == 8 * kPpq);
  CHECK(std::fabs(polyline(pts, 5.0 * kPpq + kPpq / 16.0)) > 20.0);  // the full depth holds between the fades now
  CHECK(std::fabs(polyline(pts, 7.9 * kPpq)) < 4.0);

  // Clamps: depth, rate, an onset beyond the note, an end beyond the note (stored as 0), fades longer than the span.
  NoteVibrato wild;
  wild.depthCents = 5000.0f;
  wild.rate = 1000.0f;
  wild.onset = 100 * kPpq;
  wild.end = 200 * kPpq;
  wild.fadeIn = 100 * kPpq;
  wild.fadeOut = 100 * kPpq;
  const ops::NoteVibratoEdit w[] = {{keyOf(longer), wild}};
  commit(f.store, ops::setNoteVibrato(f.doc(), f.ref, w));
  const NoteVibrato got = findNoteCurves(f.clip(), a.id)->vibrato;
  CHECK(got.depthCents == kMaxVibratoCents);
  CHECK(got.rate == kMaxVibratoHz);
  CHECK(got.onset == 8 * kPpq - 1);
  CHECK(got.end == 0);
  CHECK(got.fadeIn == 1);
  CHECK(got.fadeOut == 0);

  // Validation (the command and so the loader).
  auto refused = [&](NoteVibrato bad) {
    CHECK_FALSE(f.store.dispatch(SetNoteCurves{f.ref, {NoteCurves{a.id, {}, {}, {}, bad}}}).ok());
  };
  NoteVibrato good;
  good.depthCents = 10.0f;
  good.rate = 6.0f;
  REQUIRE(f.store.dispatch(SetNoteCurves{f.ref, {NoteCurves{a.id, {}, {}, {}, good}}}).ok());
  NoteVibrato bad = good;
  bad.depthCents = 1201.0f;
  refused(bad);
  bad.depthCents = -1201.0f;
  refused(bad);
  // A negative depth is the inverted vibrato (it falls first): valid, clamped like a positive one, the mirror wave.
  NoteVibrato inverted = good;
  inverted.depthCents = -10.0f;
  REQUIRE(f.store.dispatch(SetNoteCurves{f.ref, {NoteCurves{a.id, {}, {}, {}, inverted}}}).ok());
  CHECK(findNoteCurves(f.clip(), a.id)->vibrato.set());
  {
    const std::vector<Breakpoint> down = f.effective(f.clip(), *f.clip().notes.findById(a.id));
    REQUIRE(f.store.dispatch(SetNoteCurves{f.ref, {NoteCurves{a.id, {}, {}, {}, good}}}).ok());
    const std::vector<Breakpoint> up = f.effective(f.clip(), *f.clip().notes.findById(a.id));
    REQUIRE(down.size() == up.size());
    for (size_t i = 0; i < up.size(); ++i) {
      CHECK(down[i].tick == up[i].tick);
      CHECK(down[i].value == doctest::Approx(-up[i].value).epsilon(1e-6));
    }
    CHECK(polyline(down, kPpq / 8.0) < 0.0);  // the first swing goes down
  }
  inverted.depthCents = -5000.0f;
  const ops::NoteVibratoEdit inv[] = {{keyOf(longer), inverted}};
  commit(f.store, ops::setNoteVibrato(f.doc(), f.ref, inv));
  CHECK(findNoteCurves(f.clip(), a.id)->vibrato.depthCents == -kMaxVibratoCents);
  f.roundTrips();  // (the negative depth survives save and load)
  REQUIRE(f.store.dispatch(SetNoteCurves{f.ref, {NoteCurves{a.id, {}, {}, {}, good}}}).ok());
  bad = good;
  bad.rate = 31.0f;
  refused(bad);
  bad = good;
  bad.rate = 0.0f;
  refused(bad);
  bad = good;
  bad.onset = -1;
  refused(bad);
  bad = good;
  bad.onset = 10;
  bad.end = 10;
  refused(bad);
  bad = good;
  bad.fadeOut = -1;
  refused(bad);
  bad = NoteVibrato{};
  bad.rate = 6.0f;  // a rate without a depth
  refused(bad);
  NoteVibrato nan = good;
  nan.depthCents = std::nanf("");
  const ops::NoteVibratoEdit n[] = {{keyOf(longer), nan}};
  CHECK_FALSE(ops::setNoteVibrato(f.doc(), f.ref, n).ok());
  const ops::NoteVibratoEdit twice[] = {{keyOf(longer), good}, {keyOf(longer), good}};
  CHECK_FALSE(ops::setNoteVibrato(f.doc(), f.ref, twice).ok());
  f.roundTrips();
}

TEST_CASE("vibrato: carried like a bend — duplicate and clip copies keep it, Clear Bends and a new free curve leave it, it adds to a bend") {
  BendFix f;
  const Note a = f.add(0, PitchId::fromDegree(0), 2 * kPpq);
  const Note b = f.add(2 * kPpq, PitchId::fromDegree(7), 2 * kPpq);
  NoteVibrato v;
  v.depthCents = 20.0f;
  v.rate = 8.0f;
  v.unit = NoteVibrato::RateUnit::Beats;
  const ops::NoteVibratoEdit one[] = {edit(b, v)};
  commit(f.store, ops::setNoteVibrato(f.doc(), f.ref, one));
  commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(b), ops::NoteEdge::Start, kPpq, GlideShape{GlideShape::LogLinear}, a.id));
  const NoteCurves rec = *findNoteCurves(f.clip(), b.id);
  CHECK(rec.vibrato == v);
  CHECK(rec.bendIn.other == a.id);
  // The sum: at a zero of the vibrato inside the glide the curve is the glide alone (−700 → 0 over a quarter).
  const std::vector<Breakpoint> pts = f.effective(f.clip(), b);
  const double period = static_cast<double>(kPpq) / 8.0;
  CHECK(polyline(pts, 4.0 * period) == doctest::Approx(-700.0 * (1.0 - 4.0 * period / static_cast<double>(kPpq))).epsilon(1e-4));
  CHECK(polyline(pts, 4.25 * period) ==
        doctest::Approx(-700.0 * (1.0 - 4.25 * period / static_cast<double>(kPpq)) + 20.0).epsilon(1e-3));
  // A note anchored to the vibrating note takes its pitch without the vibrato.
  const Note dep = f.add(2 * kPpq + static_cast<Ticks>(kPpq + period / 4), PitchId::fromRatio(Rational(3, 2), AnchorKind::Note, b.id.value));
  CHECK(f.hz(dep.id) == doctest::Approx(f.hz(b.id) * 1.5).epsilon(1e-12));

  // Clear Bends: the bend goes, the vibrato stays; a free curve set afterwards leaves it too.
  const NoteKey kb = keyOf(b);
  commit(f.store, ops::clearNoteBends(f.doc(), f.ref, std::span<const NoteKey>(&kb, 1)));
  CHECK(*findNoteCurves(f.clip(), b.id) == NoteCurves{b.id, {}, {}, {}, v});
  commit(f.store, ops::setNoteCurve(f.doc(), f.ref, kb, {{0, 0.0f, 0.0f, {}}, {kPpq, 30.0f, 0.0f, {}}}));
  CHECK(findNoteCurves(f.clip(), b.id)->vibrato == v);
  // Duplicate: the copy vibrates.
  auto dup = ops::duplicateNotes(f.doc(), f.store.ids(), f.ref, std::span<const NoteKey>(&kb, 1));
  commit(f.store, dup);
  const NoteCurves* copied = findNoteCurves(f.clip(), dup->notes[0].id);
  REQUIRE(copied != nullptr);
  CHECK(copied->vibrato == v);
  // A clip copy too.
  IdSource& ids = f.store.ids();
  const NoteClip cp = ops::cloneNoteClip(f.clip(), ids);
  size_t vibrating = 0;
  for (const NoteCurves& nc : noteCurvesOf(cp)) vibrating += nc.vibrato == v ? 1 : 0;
  CHECK(vibrating == 2);
  f.roundTrips();
}

TEST_CASE("Is Harmonic N on a member: it becomes the group's anchor — every note keeps its Hz, the numbers re-derive, warps and the id stay") {
  BendFix f;
  IdSource& ids = f.store.ids();
  std::vector<Note> triad;
  for (const int32_t d : {0, 4, 7}) triad.push_back(f.add(0, PitchId::fromDegree(d), 4 * kPpq));
  std::vector<NoteKey> keys;
  for (const Note& n : triad) keys.push_back(keyOf(n));
  const double e12 = f.hz(triad[1].id), g12 = f.hz(triad[2].id);  // as written
  auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, keys);
  commit(f.store, tuned);
  const JiGroupId group = tuned->jiGroups.at(0);
  const double hz0 = f.hz(triad[0].id), hz1 = f.hz(triad[1].id), hz2 = f.hz(triad[2].id);
  CHECK(hz1 == doctest::Approx(hz0 * 1.25).epsilon(1e-12));
  commit(f.store, ops::setJiWarps(f.doc(), f.ref, group, {JiWarp{8 * kPpq, kPpq, GlideShape{}, false}}));

  // The third (5) is declared harmonic 10: 4:5:6 reads 8:10:12.
  const size_t h = f.store.history().cursor();
  auto re = ops::reanchorJiGroup(f.doc(), f.ref, group, keyOf(*f.clip().notes.findById(triad[1].id)), 10);
  commit(f.store, re);
  CHECK(f.store.history().cursor() == h + 1);
  REQUIRE(jiGroupsOf(f.clip()).size() == 1);
  const JiGroup& g = jiGroupsOf(f.clip())[0];
  CHECK(g.id == group);
  CHECK(g.anchor == triad[1].id);
  CHECK(g.index == 10);
  CHECK(g.warps.size() == 1);
  const std::optional<GroupHarmonics> gh = groupHarmonics(f.clip(), g);
  REQUIRE(gh.has_value());
  CHECK(gh->anchor == 10);
  CHECK(gh->proportion == std::vector<int64_t>{8, 10, 12});
  CHECK(f.hz(triad[0].id) == doctest::Approx(hz0).epsilon(1e-12));
  CHECK(f.hz(triad[1].id) == doctest::Approx(hz1).epsilon(1e-12));
  CHECK(f.hz(triad[2].id) == doctest::Approx(hz2).epsilon(1e-12));
  const Note& third = *f.clip().notes.findById(triad[1].id);
  CHECK(third.pitch.kind() == PitchKind::Cents);  // frozen at its JI pitch (the old anchor is a 12-EDO degree)
  // T7.16a (M7 review M4): it keeps what it was written as, which a group Back to Tuning plays.
  REQUIRE(findNoteWritten(f.clip(), triad[1].id) != nullptr);
  CHECK(findNoteWritten(f.clip(), triad[1].id)->written == PitchId::fromDegree(4));
  const Note& root = *f.clip().notes.findById(triad[0].id);
  CHECK(root.pitch == PitchId::fromRatio(Rational(4, 5), AnchorKind::Note, triad[1].id.value));
  REQUIRE(findNoteWritten(f.clip(), triad[0].id) != nullptr);
  CHECK(findNoteWritten(f.clip(), triad[0].id)->written == PitchId::fromDegree(0));
  CHECK(f.clip().notes.findById(triad[2].id)->pitch == PitchId::fromRatio(Rational(6, 5), AnchorKind::Note, triad[1].id.value));
  CHECK(findNoteWritten(f.clip(), triad[2].id)->written == PitchId::fromDegree(7));
  f.roundTrips();
  // Back to Tuning after it: every note as written, exactly — the new anchor too (M4).
  commit(f.store, ops::untuneJiGroup(f.doc(), f.ref, group));
  CHECK(f.hz(triad[0].id) == hz0);
  CHECK(f.hz(triad[1].id) == e12);
  CHECK(f.hz(triad[2].id) == g12);
  REQUIRE(f.store.undo());

  // Refused: not a member (the anchor itself, a stranger), a bad index.
  CHECK_FALSE(ops::reanchorJiGroup(f.doc(), f.ref, group, keyOf(third), 5).ok());
  const Note other = f.add(8 * kPpq, PitchId::fromDegree(2));
  CHECK_FALSE(ops::reanchorJiGroup(f.doc(), f.ref, group, keyOf(other), 5).ok());
  CHECK_FALSE(ops::reanchorJiGroup(f.doc(), f.ref, group, keyOf(root), 0).ok());
  // Undo: the group as it was.
  REQUIRE(f.store.undo());  // the stranger
  REQUIRE(f.store.undo());
  CHECK(jiGroupsOf(f.clip())[0].anchor == triad[0].id);
  CHECK(jiGroupsOf(f.clip())[0].index == 4);
  CHECK(f.clip().notes.findById(triad[0].id)->pitch == PitchId::fromDegree(0));

  // An exact anchor: the new anchor is the exact reference ratio.
  const int64_t t[] = {4, 5, 6};
  auto made = ops::proportionNotes(f.doc(), ids, f.ref, 16 * kPpq, kPpq, PitchId::fromRatio(Rational(1, 1)), t);
  commit(f.store, made);
  commit(f.store, ops::reanchorJiGroup(f.doc(), f.ref, made->jiGroups[0], made->notes[2], 3));
  CHECK(f.clip().notes.findById(made->notes[2].id)->pitch == PitchId::fromRatio(Rational(3, 2)));
  const std::optional<GroupHarmonics> g2 = groupHarmonics(f.clip(), *findJiGroup(f.clip(), made->jiGroups[0]));
  REQUIRE(g2.has_value());
  CHECK(g2->proportion == std::vector<int64_t>{4, 5, 6});  // 6 declared 3 → m = 2: still 4:5:6
  CHECK(g2->anchor == 6);
}

TEST_CASE("vibrato (bug 2026-11-15): one that runs to the note's end ends on its swing there, with no step back to the note's pitch") {
  BendFix f;
  const Ticks L = 4 * kPpq;
  const Note a = f.add(0, PitchId::fromDegree(0), L);
  NoteVibrato v;
  v.depthCents = 40.0f;
  v.rate = 1.0625f;  // 4.25 cycles over the note: it ends on a crest
  v.unit = NoteVibrato::RateUnit::Beats;
  const ops::NoteVibratoEdit one[] = {edit(a, v)};
  commit(f.store, ops::setNoteVibrato(f.doc(), f.ref, one));
  const std::vector<Breakpoint> pts = f.effective(f.clip(), *f.clip().notes.findById(a.id), 0.05);
  REQUIRE(pts.size() > 8);
  CHECK(pts.back().tick == L);
  CHECK(pts[pts.size() - 2].tick < L);  // one point at the end, not a jump
  CHECK(pts.back().value == doctest::Approx(40.0).epsilon(0.01));
  CHECK(noteCurveCents(*findNoteCurves(f.clip(), a.id), L, NoteBendTargets{std::nullopt, std::nullopt, static_cast<double>(kPpq) / 1.0625}, static_cast<double>(L)) ==
        doctest::Approx(40.0).epsilon(0.01));
}
