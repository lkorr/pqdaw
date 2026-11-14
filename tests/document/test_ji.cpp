// T7.2: JI objects and note curves in the document (model, commands, evaluation, the voice plan, resolution, edit
// operations, the file). Plan: docs/plans/M7.md "Data model", "T7.2" and amendment A1 (tuning stops).

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "edit_helpers.h"
#include "pq/core/alloc_trap.h"
#include "pq/document/ji_object.h"
#include "pq/document/pitch_resolve.h"
#include "pq/document/serialize.h"
#include "pq/tuning/edo.h"

using namespace edit_test;
using doctest_fixture::Fixture;
using doctest_fixture::makeClip;
using pq::kPpq;
using pq::tuning::PitchId;
using pq::tuning::PitchKind;
using pq::tuning::Rational;

namespace {

pq::tuning::Tuning edo(uint32_t n) { return pq::tuning::Tuning::make(pq::tuning::Edo::make(n).value()).value(); }

JiStop prop(Ticks at, std::vector<int64_t> terms, Ticks glide = 0, JiShape::Kind kind = JiShape::SCurve) {
  JiStop s;
  s.at = at;
  s.terms = std::move(terms);
  s.timing.glide = glide;
  s.timing.shape.kind = kind;
  return s;
}

JiStop written(Ticks at, std::vector<PitchId> pitches, Ticks glide = 0, JiShape::Kind kind = JiShape::SCurve) {
  JiStop s;
  s.at = at;
  s.pitches = std::move(pitches);
  s.timing.glide = glide;
  s.timing.shape.kind = kind;
  return s;
}

// A free-standing object (not in a clip): ids 1 / 101…, fundamental = the reference.
JiObject object(Ticks length, std::vector<JiStop> stops) {
  JiObject o;
  o.id = JiObjectId{1};
  o.length = length;
  o.fundamental = PitchId::fromRatio(Rational{1});
  o.stops = std::move(stops);
  size_t V = 0;
  for (const JiStop& s : o.stops) V = std::max<size_t>(V, s.size());
  for (size_t i = 0; i < V; ++i) o.voiceNotes.push_back(NoteId{static_cast<uint32_t>(101 + i)});
  return o;
}

double polyline(const std::vector<JiPitchPoint>& pts, Ticks x) {
  auto it = std::upper_bound(pts.begin(), pts.end(), x, [](Ticks v, const JiPitchPoint& p) { return v < p.tick; });
  const JiPitchPoint& a = *(it - 1);
  if (it == pts.end() || a.tick == x) return a.cents;
  return a.cents + (it->cents - a.cents) * static_cast<double>(x - a.tick) / static_cast<double>(it->tick - a.tick);
}

// The fixture plus a 12-EDO clip "J" on Bass (the project's default tuning) holding one note per given degree, all at
// tick 0 and `length` long.
struct JiFix : Fixture {
  ClipRef ref;
  explicit JiFix(std::vector<int32_t> degrees = {}, Ticks length = 4 * kPpq) {
    std::vector<Note> notes;
    for (const int32_t d : degrees) notes.push_back(doctest_fixture::makeNote(store.ids(), 0, d, length));
    ClipPtr c = makeClip(store.ids(), 0, 4 * kBar, std::move(notes), "J");
    ref = {bass, c->id};
    REQUIRE(store.dispatch(AddClip{bass, c}).ok());
  }
  const NoteClip& clip() const { return Fixture::clip(ref.track, ref.clip); }
  const JiObject& ji(JiObjectId id) const {
    const JiObject* o = findJiObject(clip(), id);
    REQUIRE(o != nullptr);
    return *o;
  }
  Note note(Ticks start, PitchId pitch, Ticks length = kPpq) {
    Note n;
    n.id = store.ids().note();
    n.start = start;
    n.length = length;
    n.pitch = pitch;
    return n;
  }
  // The key of the clip's note at `start` (the tests keep starts distinct where they use this).
  NoteKey at(Ticks start) const {
    for (const Note& n : clip().notes)
      if (n.start == start) return keyOf(n);
    FAIL("no note at this tick");
    return {};
  }
  double hzOf(NoteId id) const {
    auto r = resolveClipPitches(doc(), ref.track, ref.clip);
    REQUIRE(r.ok());
    for (const NotePitch& np : r->notes)
      if (np.note == id) {
        REQUIRE(np.hz.has_value());
        return *np.hz;
      }
    FAIL("note not resolved");
    return 0.0;
  }
  void roundTrips() const {
    pq::Result<std::string> text = serializeProject(doc(), store.view());
    REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));
    pq::Result<LoadedProject> back = parseProject(*text);
    REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
    CHECK(*back->project == doc());
    pq::Result<std::string> again = serializeProject(*back->project, back->view);
    REQUIRE(again.ok());
    CHECK(*again == *text);
  }
};

}  // namespace

// ---- Evaluation ------------------------------------------------------------------------------------------------------

TEST_CASE("ji: the spread rule, voice maps, undertone objects, exact ratios") {
  const pq::tuning::Tuning t = edo(12);
  JiObject o = object(1000, {prop(0, {1, 2, 3, 4, 5, 6, 7}), prop(100, {10, 11, 12, 13, 14})});
  const uint32_t expect[7] = {0, 0, 1, 2, 2, 3, 4};  // 7 voices on 5 terms: ⌊v·5/7⌋
  for (uint32_t v = 0; v < 7; ++v) {
    CHECK(jiVoiceEntry(o, o.stops[1], v) == expect[v]);
    CHECK(jiVoiceEntry(o, o.stops[0], v) == v);
    CHECK(jiVoiceRatio(o, v, 1) == Rational{10 + expect[v]});
  }
  o.stops[1].voiceTerm = {4, 4, 3, 3, 2, 1, 0};
  CHECK(jiVoiceRatio(o, 0, 1) == Rational{14});
  CHECK(jiVoiceRatio(o, 6, 1) == Rational{10});
  CHECK_FALSE(jiVoiceRatio(o, 7, 0).has_value());
  CHECK_FALSE(jiVoiceRatio(o, 0, 2).has_value());

  // Undertone 4:5:6 = 1/4 : 1/5 : 1/6 × F, on a fundamental 3/2 above the reference, with a stop scale.
  JiObject u = object(1000, {prop(0, {4, 5, 6})});
  u.undertone = true;
  u.fundamental = PitchId::fromRatio(Rational{3, 2});
  CHECK(jiVoiceRatio(u, 0, 0) == Rational{1, 4});
  CHECK(jiVoiceRatio(u, 1, 0) == Rational{1, 5});
  CHECK(jiVoiceRatio(u, 2, 0) == Rational{1, 6});
  CHECK(*jiVoiceHz(u, t, 0, 0.0) == t.referenceHz() * 1.5 * 0.25);  // exact at a hold: no log / exp round trip
  CHECK(*jiVoiceCents(u, t, 2, 500.0) == doctest::Approx(1200.0 * std::log2(1.5 / 6.0)).epsilon(1e-12));
  u.stops[0].scale = Rational{8, 3};
  CHECK(jiVoiceRatio(u, 0, 0) == Rational{2, 3});
  CHECK(*jiFundamentalHz(u, t, 0.0) == t.referenceHz() * 1.5 * (8.0 / 3.0));
  CHECK(*jiFundamentalCents(u, t, 0.0) == doctest::Approx(1200.0 * std::log2(4.0)).epsilon(1e-12));
  CHECK_FALSE(jiVoiceCents(u, t, 3, 0.0).has_value());

  // The fundamental must be one the tuning can resolve on its own.
  JiObject bad = object(1000, {prop(0, {4, 5, 6})});
  bad.fundamental = PitchId::fromHarmonic(1, 0);
  CHECK_FALSE(jiVoiceCents(bad, t, 0, 0.0).has_value());
  CHECK_FALSE(jiVoicePlan(bad, t).has_value());
}

TEST_CASE("ji: timing — a voice arrives at at + delay after a glide; windows are clamped to the previous arrival") {
  const pq::tuning::Tuning t = edo(12);
  JiObject o = object(10000, {prop(0, {4, 5}), prop(1000, {5, 6}, 400, JiShape::LogLinear), prop(1200, {6, 7}, 900, JiShape::LogLinear)});
  o.stops[1].voiceTiming = {std::nullopt, JiTiming{300, 200, JiShape{JiShape::LogLinear}}};
  // Voice 0: stop 1 glides 600 → 1000; stop 2 wants 300 → 1200 but starts when stop 1 arrived: 1000 → 1200.
  CHECK(jiTransition(o, 0, 1).begin == 600);
  CHECK(jiTransition(o, 0, 1).arrive == 1000);
  CHECK(jiTransition(o, 0, 2).begin == 1000);
  CHECK(jiTransition(o, 0, 2).arrive == 1200);
  // Voice 1: its own delay 300 and glide 200 into stop 1 (1100 → 1300); stop 2 cannot arrive before that: a jump there.
  CHECK(jiTransition(o, 1, 1).begin == 1100);
  CHECK(jiTransition(o, 1, 1).arrive == 1300);
  CHECK(jiTransition(o, 1, 2).begin == 1300);
  CHECK(jiTransition(o, 1, 2).arrive == 1300);
  const double c4 = 2400.0, c5 = 1200.0 * std::log2(5.0), c6 = 1200.0 * std::log2(6.0), c7 = 1200.0 * std::log2(7.0);
  CHECK(*jiVoiceCents(o, t, 0, 0.0) == doctest::Approx(c4));
  CHECK(*jiVoiceCents(o, t, 0, 600.0) == doctest::Approx(c4));
  CHECK(*jiVoiceCents(o, t, 0, 800.0) == doctest::Approx(0.5 * (c4 + c5)));  // linear in cents
  CHECK(*jiVoiceCents(o, t, 0, 1000.0) == doctest::Approx(c5));
  CHECK(*jiVoiceCents(o, t, 0, 1100.0) == doctest::Approx(0.5 * (c5 + c6)));
  CHECK(*jiVoiceCents(o, t, 0, 1200.0) == doctest::Approx(c6));
  CHECK(*jiVoiceCents(o, t, 1, 1299.0) < c6);   // still gliding into stop 1
  CHECK(*jiVoiceCents(o, t, 1, 1300.0) == doctest::Approx(c7));  // the jump: from that tick on
  CHECK(*jiVoiceCents(o, t, 1, 99999.0) == doctest::Approx(c7));  // clamped to the span
  CHECK(jiPosition(o, 0, 800.0).next == 1);
  CHECK(jiPosition(o, 0, 800.0).u == doctest::Approx(0.5));
  CHECK(jiPosition(o, 0, 5000.0).holding());
  CHECK(jiPosition(o, 0, 5000.0).stop == 2);
  // Shapes: 0 → 1, the S-curve symmetric, the bezier's overshoot allowed.
  for (JiShape::Kind k : {JiShape::LogLinear, JiShape::SCurve, JiShape::Bezier}) {
    CHECK(jiShapeValue(JiShape{k}, 0.0) == doctest::Approx(0.0));
    CHECK(jiShapeValue(JiShape{k}, 1.0) == doctest::Approx(1.0));
    CHECK(jiShapeValue(JiShape{k}, 0.5) == doctest::Approx(0.5));
  }
  CHECK(jiShapeValue(JiShape{JiShape::SCurve}, 0.25) == doctest::Approx(0.15625));
  CHECK(jiShapeValue(JiShape{JiShape::Bezier, 0.0f, 0.0f, 1.0f, 1.0f}, 0.3) == doctest::Approx(0.3).epsilon(1e-6));
  CHECK(jiShapeValue(JiShape{JiShape::Bezier, 0.3f, 0.0f, 0.6f, 2.0f}, 0.8) > 1.0);
}

TEST_CASE("ji: the voice plan stays within 0.05 cents of the dense evaluation for every shape") {
  const pq::tuning::Tuning t = edo(12);
  // With a ride (the general flattening) and without one (T7.7: the walk over the stops, no evaluation at the knots).
  for (const bool ride : {true, false})
  for (JiShape::Kind kind : {JiShape::LogLinear, JiShape::SCurve, JiShape::Bezier}) {
    CAPTURE(ride);
    CAPTURE(static_cast<int>(kind));
    const JiShape shape{kind, 0.3f, -0.6f, 0.6f, 1.8f};  // (the handles matter for Bezier only: an overshoot)
    JiObject o = object(40000, {prop(0, {4, 5, 6}), prop(10000, {5, 6, 7}, 6000, kind),
                                written(20000, {PitchId::fromDegree(0, 2), PitchId::fromDegree(4, 2), PitchId::fromDegree(7, 2)}, 5000, kind),
                                prop(30000, {6, 7, 9})});  // the last one a jump
    o.stops[1].timing.shape = shape;
    o.stops[2].timing.shape = shape;
    o.stops[2].voiceTiming = {std::nullopt, JiTiming{700, 3000, shape}, JiTiming{2500, 9000, shape}};
    if (ride) o.ride = {{2000, 0.0f, 0.8f, {}}, {9000, 35.0f, -0.6f, {}}, {9000, -20.0f, 0.0f, {}}, {26000, 10.0f, 0.3f, {}}, {39000, 0.0f, 0.0f, {}}};
    REQUIRE(validateJiObject(o).ok());
    const std::optional<JiPlan> plan = jiVoicePlan(o, t, JiPlanOptions{0.05, 500});
    REQUIRE(plan.has_value());
    REQUIRE(plan->voices.size() == 3);
    for (uint32_t v = 0; v < 3; ++v) {
      const std::vector<JiPitchPoint>& pts = plan->voices[v].pitch;
      REQUIRE(pts.size() >= 2);
      CHECK(pts.front().tick == 0);
      CHECK(pts.back().tick == o.length);
      CHECK(std::is_sorted(pts.begin(), pts.end(), [](const JiPitchPoint& a, const JiPitchPoint& b) { return a.tick < b.tick; }));
      CHECK(pts.size() < 4000);  // flattened, not sampled (40000 ticks)
      double worst = 0.0;
      for (Ticks x = 0; x < o.length; ++x) worst = std::max(worst, std::abs(polyline(pts, x) - *jiVoiceCents(o, t, v, static_cast<double>(x))));
      CHECK(worst <= 0.05);
      MESSAGE("shape " << static_cast<int>(kind) << " voice " << v << ": " << pts.size() << " points, worst " << worst << " cents");
    }
  }
  // Holds and straight glides cost exactly their knots.
  const JiObject lin = object(1000, {prop(0, {4, 5}), prop(500, {5, 6}, 200, JiShape::LogLinear)});
  const std::optional<JiPlan> p = jiVoicePlan(lin, t);
  REQUIRE(p.has_value());
  REQUIRE(p->voices[0].pitch.size() == 4);
  CHECK(p->voices[0].pitch[1].tick == 300);
  CHECK(p->voices[0].pitch[2].tick == 500);
  CHECK(p->voices[0].pitch[0].cents == p->voices[0].pitch[1].cents);
  CHECK(p->voices[0].pitch[2].cents == p->voices[0].pitch[3].cents);
  // Cost of a large object (64 voices, 256 S-curve transitions): a number for the log, no threshold.
  std::vector<JiStop> stops;
  for (int k = 0; k < 256; ++k) {
    std::vector<int64_t> terms;
    for (int i = 0; i < 64; ++i) terms.push_back(64 + k + i);
    stops.push_back(prop(k * 1000, std::move(terms), k == 0 ? 0 : 400));
  }
  const JiObject big = object(256 * 1000, std::move(stops));
  REQUIRE(validateJiObject(big).ok());
  const auto t0 = std::chrono::steady_clock::now();
  const std::optional<JiPlan> bp = jiVoicePlan(big, t);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(bp.has_value());
  size_t points = 0;
  for (const JiVoicePlan& vp : bp->voices) points += vp.pitch.size();
  MESSAGE("jiVoicePlan, 64 voices x 256 stops: " << ms << " ms, " << points << " pitch points");
}

TEST_CASE("ji: unison classes and gains for 1 -> 2:3 -> 4:5:6 -> 2:3 -> 1") {
  const pq::tuning::Tuning t = edo(12);
  const JiObject o = object(5000, {prop(0, {1}), prop(1000, {2, 3}, 200), prop(2000, {4, 5, 6}, 200), prop(3000, {2, 3}, 200), prop(4000, {1}, 200)});
  REQUIRE(validateJiObject(o).ok());
  const std::optional<JiPlan> plan = jiVoicePlan(o, t, JiPlanOptions{0.05, 50});
  REQUIRE(plan.has_value());
  REQUIRE(plan->voices.size() == 3);
  using U = JiUnison;
  // {from, leader, size, lockedSize, locked}: all three start as one voice; 0 and 1 stay bit-identical through 2:3;
  // after 4:5:6 they have different histories: 1 merging back into 0 is Synced to it at 3000 (T7.7), then 2 at 4000.
  CHECK(plan->voices[0].unison == std::vector<U>{{0, 0, 3, 3, true}, {1000, 0, 2, 2, true}, {2000, 0, 1, 1, true}, {3000, 0, 2, 2, true}, {4000, 0, 3, 3, true}});
  CHECK(plan->voices[1].unison == std::vector<U>{{0, 0, 3, 3, true}, {1000, 0, 2, 2, true}, {2000, 1, 1, 1, true}, {3000, 0, 2, 2, true}, {4000, 0, 3, 3, true}});
  CHECK(plan->voices[2].unison == std::vector<U>{{0, 0, 3, 3, true}, {1000, 2, 1, 1, true}, {4000, 0, 3, 3, true}});
  using G = JiGainPoint;
  const float third = 1.0f / 3.0f;
  // T7.7 (ji_object.h "Gains"). The keeper (voice 0): at each split the voices that stay step to 1 / (their number)
  // at the tick the split begins; at each merge every voice of the unison steps to 1 / k when it is complete, keyed
  // by the keeper (sync 0).
  CHECK(plan->voices[0].gain == std::vector<G>{{0, third}, {800, third}, {800, 0.5f}, {1800, 0.5f}, {1800, 1.0f}, {3000, 1.0f},
                                               {3000, 0.5f, 0}, {4000, 0.5f}, {4000, third, 0}, {5000, third}});
  // Voice 1 stays with the keeper at 800, leaves it at 1800 (0, rising with its squared distance to the keeper over
  // the glide), merges back over [2800, 3000] (falling the same way), is Synced at 3000 (the point the jump leaves
  // carries the Sync) and steps to 1/2 with the keeper; at 4000 both step to 1/3 (no Sync: they are identical).
  const std::vector<G>& g1 = plan->voices[1].gain;
  auto at = [](const std::vector<G>& g, Ticks tick) {  // the value from `tick` on
    float v = g.front().gain;
    for (size_t i = 0; i < g.size() && g[i].tick <= tick; ++i) {
      v = g[i].gain;
      if (i + 1 < g.size() && g[i + 1].tick > tick && g[i].tick < tick)
        v += (g[i + 1].gain - g[i].gain) * static_cast<float>(tick - g[i].tick) / static_cast<float>(g[i + 1].tick - g[i].tick);
    }
    return v;
  };
  auto find = [](const std::vector<G>& g, const G& p) { return std::find(g.begin(), g.end(), p) != g.end(); };
  CHECK(find(g1, G{800, 0.5f}));
  CHECK(find(g1, G{1800, 0.0f}));
  CHECK(at(g1, 1800) == 0.0f);
  CHECK(at(g1, 1900) == doctest::Approx(0.25).epsilon(0.02));  // the S-curve's middle: half the distance, squared
  CHECK(at(g1, 2000) == 1.0f);
  CHECK(at(g1, 2800) == 1.0f);
  CHECK(at(g1, 2900) == doctest::Approx(0.25).epsilon(0.02));
  CHECK(find(g1, G{3000, 0.0f, 0}));
  CHECK(find(g1, G{3000, 0.5f, 0}));
  CHECK(find(g1, G{4000, third, 0}));
  CHECK(g1.back() == G{5000, third});
  for (size_t i = 1; i < g1.size(); ++i) CHECK(g1[i].tick >= g1[i - 1].tick);
  // Voice 2 leaves at 800 (0 → 1 over the glide) and merges over [3800, 4000]: Synced to voice 0, then 1/3.
  const std::vector<G>& g2 = plan->voices[2].gain;
  CHECK(find(g2, G{800, 0.0f}));
  CHECK(at(g2, 1000) == 1.0f);
  CHECK(at(g2, 3800) == 1.0f);
  CHECK(at(g2, 3900) == doctest::Approx(0.25).epsilon(0.02));
  CHECK(find(g2, G{4000, 0.0f, 0}));
  CHECK(find(g2, G{4000, third, 0}));
  // Every unison sums to one voice: identical lanes at 1 / k.
  for (const Ticks x : {Ticks{0}, Ticks{500}, Ticks{1500}, Ticks{3500}, Ticks{4500}}) {
    CHECK(at(plan->voices[0].gain, x) * static_cast<float>(x < 800 || x >= 4000 ? 3 : x >= 1800 && x < 3000 ? 1 : 2) ==
          doctest::Approx(1.0));
  }
  // Locked voices have the identical pitch curve (bit for bit) while they are locked.
  for (Ticks x = 0; x <= 1800; x += 7) CHECK(*jiVoiceCents(o, t, 0, static_cast<double>(x)) == *jiVoiceCents(o, t, 1, static_cast<double>(x)));
  CHECK(plan->voices[0].pitch.front() == plan->voices[1].pitch.front());
  CHECK(plan->voices[0].pitch != plan->voices[1].pitch);
  // A unison from start to end: the same points.
  const JiObject two = object(1000, {prop(0, {3}), prop(500, {4, 5}, 100)});
  JiObject same = two;
  same.stops[1].voiceTerm = {1, 1};
  const std::optional<JiPlan> sp = jiVoicePlan(same, t);
  REQUIRE(sp.has_value());
  CHECK(sp->voices[0].pitch == sp->voices[1].pitch);
  CHECK(sp->voices[0].gain == std::vector<G>{{0, 0.5f}, {1000, 0.5f}});
}

TEST_CASE("ji: a tuning stop morphs into a proportion — the resolved degrees at stop 0, the exact ratios at stop 1 (A1)") {
  const pq::tuning::Tuning t = edo(12);
  JiObject o = object(4000, {written(0, {PitchId::fromDegree(2), PitchId::fromDegree(6), PitchId::fromDegree(9)}), prop(1000, {4, 5, 6}, 400)});
  o.fundamental = PitchId::fromDegree(2);  // the lowest voice, pinned: scale · 4 = 1
  o.stops[0].scale = o.stops[1].scale = Rational{1, 4};
  REQUIRE(validateJiObject(o).ok());
  const double d2 = *pq::tuning::resolveHzDouble(PitchId::fromDegree(2), t, {});
  const double written12[3] = {200.0, 600.0, 900.0};
  const PitchId ids[3] = {PitchId::fromDegree(2), PitchId::fromDegree(6), PitchId::fromDegree(9)};
  const Rational ratios[3] = {Rational{1}, Rational{5, 4}, Rational{3, 2}};
  for (uint32_t v = 0; v < 3; ++v) {
    // Stop 0 (and the hold up to the glide's start): the degrees as the tuning resolves them, exactly.
    for (double x : {0.0, 300.0, 600.0}) {
      CHECK(*jiVoiceCents(o, t, v, x) == doctest::Approx(written12[v]).epsilon(1e-12));
      CHECK(*jiVoiceHz(o, t, v, x) == *pq::tuning::resolveHzDouble(ids[v], t, {}));
    }
    CHECK_FALSE(jiVoiceRatio(o, v, 0).has_value());
    // Stop 1: exact ratios over the pinned voice.
    CHECK(jiVoiceRatio(o, v, 1) == ratios[v]);
    CHECK(*jiVoiceHz(o, t, v, 1000.0) == d2 * ratios[v].toDouble());
    CHECK(*jiVoiceHz(o, t, v, 3999.0) == d2 * ratios[v].toDouble());
    // In between: on the way (monotonic for an S-curve).
    const double mid = *jiVoiceCents(o, t, v, 800.0);
    const double target = 200.0 + 1200.0 * std::log2(ratios[v].toDouble());
    CHECK(mid == doctest::Approx(0.5 * (written12[v] + target)));
  }
  // The fundamental stays where it is through the morph: two octaves under the pinned voice.
  for (double x : {0.0, 800.0, 2000.0}) CHECK(*jiFundamentalCents(o, t, x) == doctest::Approx(200.0 - 2400.0));
  // And back: proportion → tuning stop is the same kind of transition.
  o.stops.push_back(written(3000, o.stops[0].pitches, 500, JiShape::LogLinear));
  o.stops[2].scale = Rational{1, 4};
  CHECK(*jiVoiceCents(o, t, 1, 2750.0) == doctest::Approx(0.5 * (600.0 + 200.0 + 1200.0 * std::log2(1.25))));
  CHECK(*jiVoiceHz(o, t, 1, 3000.0) == *pq::tuning::resolveHzDouble(ids[1], t, {}));
}

TEST_CASE("ji: the single queries do not allocate") {
  REQUIRE(pq::alloc_trap::installed());
  const pq::tuning::Tuning t = edo(31);
  JiObject o = object(40000, {prop(0, {4, 5, 6}), prop(10000, {5, 6, 7}, 6000, JiShape::Bezier),
                              written(20000, {PitchId::fromDegree(0, 2), PitchId::fromCents(2786.0), PitchId::fromRatio(Rational{6})}, 5000)});
  o.fundamental = PitchId::fromDegree(3, -1);
  o.stops[2].voiceTiming = {std::nullopt, JiTiming{700, 3000, JiShape{}}, std::nullopt};
  o.ride = {{0, 0.0f, 0.5f, {}}, {30000, 40.0f, 0.0f, {}}};
  volatile double sink = 0.0;
  CHECK(pq::alloc_trap::countRtViolations([&] {
          for (double x = 0.0; x <= 40000.0; x += 1250.0)
            for (uint32_t v = 0; v < 3; ++v) {
              sink = sink + *jiVoiceCents(o, t, v, x) + *jiVoiceHz(o, t, v, x) + *jiFundamentalCents(o, t, x) + *jiFundamentalHz(o, t, x);
              sink = sink + jiVoiceRatio(o, v, 1)->toDouble() + static_cast<double>(jiTransition(o, v, 2).arrive) + jiPosition(o, static_cast<int32_t>(v), x).u;
            }
        }) == 0);
}

// ---- Commands ----------------------------------------------------------------------------------------------------------

TEST_CASE("ji commands: validation — objects, the notes that follow them, curves") {
  JiFix f;
  IdSource& ids = f.store.ids();
  JiObject o = object(kPpq, {prop(0, {4, 5, 6})});
  o.id = ids.jiObject();
  for (NoteId& v : o.voiceNotes) v = ids.note();
  auto refused = [&](const Command& c) {
    const Snapshot before = f.store.current();
    CHECK_FALSE(f.store.dispatch(c).ok());
    CHECK(f.store.current() == before);
  };
  // A note cannot follow an object the clip does not have.
  refused(AddNotes{f.ref, {f.note(0, PitchId::fromHarmonic(o.id.value, 0))}});
  refused(AddNotes{f.ref, {f.note(0, PitchId::fromRatio(Rational{3, 2}, pq::tuning::AnchorKind::JiFundamental, o.id.value))}});
  // Invalid objects.
  auto bad = [&](auto&& change) {
    JiObject b = o;
    change(b);
    refused(AddJiObject{f.ref, b});
  };
  bad([](JiObject& b) { b.fundamental = PitchId::fromHarmonic(1, 0); });  // a fundamental is never Harmonic
  bad([](JiObject& b) { b.fundamental = PitchId::fromRatio(Rational{3, 2}, pq::tuning::AnchorKind::Note, 5); });
  bad([](JiObject& b) { b.stops[0].terms = {4, 4, 6}; });
  bad([](JiObject& b) { b.stops[0].terms = {4, 5, (int64_t{1} << 20) + 1}; });
  bad([](JiObject& b) { b.stops[0].at = 1; });
  bad([](JiObject& b) { b.stops.clear(); });
  bad([](JiObject& b) { b.voiceNotes.pop_back(); });                      // V = the largest stop
  bad([](JiObject& b) { b.voiceNotes.push_back(b.voiceNotes[0]); });
  bad([](JiObject& b) { b.stops[0].pitches = {PitchId::fromDegree(0)}; });  // terms or pitches, not both
  bad([](JiObject& b) { b.stops.push_back(written(5, {PitchId::fromHarmonic(1, 0)})); });
  bad([](JiObject& b) { b.stops[0].scale = Rational{0}; });
  bad([](JiObject& b) { b.stops[0].voiceTerm = {0, 1}; });
  bad([](JiObject& b) { b.stops[0].voiceTerm = {0, 1, 3}; });
  bad([](JiObject& b) { b.stops[0].timing.glide = -1; });
  bad([](JiObject& b) { b.stops[0].timing.shape.x1 = 1.5f; });
  bad([](JiObject& b) { b.ride = {{10, 0.0f, 0.0f, {}}, {5, 0.0f, 0.0f, {}}}; });
  bad([](JiObject& b) { b.velocity = 0.0f; });
  bad([](JiObject& b) { b.length = 0; });
  REQUIRE(f.store.dispatch(AddJiObject{f.ref, o}).ok());
  CHECK(f.store.lastChanges().sequenceOnly());  // never a recompile
  CHECK(f.store.lastChanges().jiObjects == std::vector<JiObjectId>{o.id});
  refused(AddJiObject{f.ref, o});  // the id is taken
  // Notes that follow it.
  const Note inner = f.note(0, PitchId::fromHarmonic(o.id.value, 2));
  REQUIRE(f.store.dispatch(AddNotes{f.ref, {inner}}).ok());
  refused(AddNotes{f.ref, {f.note(0, PitchId::fromHarmonic(o.id.value, 3))}});  // voice 3 of 3
  refused(MoveNote{f.ref, keyOf(inner), 0, PitchId::fromHarmonic(o.id.value, 7)});
  refused(RemoveJiObject{f.ref, o.id});  // a note still follows it
  JiObject two = o;
  two.stops[0].terms = {4, 5};
  two.voiceNotes.pop_back();
  refused(SetJiObject{f.ref, two});  // the note's voice would be lost
  REQUIRE(f.store.dispatch(MoveNote{f.ref, keyOf(inner), 0, PitchId::fromHarmonic(o.id.value, 1)}).ok());
  REQUIRE(f.store.dispatch(SetJiObject{f.ref, two}).ok());
  CHECK(f.store.lastChanges().sequenceOnly());
  CHECK(f.ji(o.id).voices() == 2);
  // Curves: by note key; RemoveNotes drops the note's curve; undo brings both back.
  const std::vector<Breakpoint> curve = {{0, 0.0f, 0.0f, {}}, {100, 50.0f, 0.5f, {}}, {100, -50.0f, 0.0f, {}}};
  refused(SetNoteCurve{f.ref, keyOf(inner), {{0, 0.0f, 0.0f, BreakpointId{9}}}});
  refused(SetNoteCurve{f.ref, keyOf(inner), {{-1, 0.0f, 0.0f, {}}}});
  refused(SetNoteCurve{f.ref, keyOf(inner), {{0, 1e9f, 0.0f, {}}}});
  refused(SetNoteCurve{f.ref, NoteKey{0, NoteId{999999}}, curve});
  REQUIRE(f.store.dispatch(SetNoteCurve{f.ref, keyOf(inner), curve}).ok());
  CHECK(f.store.lastChanges().sequenceOnly());
  CHECK(f.store.lastChanges().notes == std::vector<NoteId>{inner.id});
  REQUIRE(findNoteCurves(f.clip(), inner.id) != nullptr);
  CHECK(findNoteCurves(f.clip(), inner.id)->pitch == curve);
  const Snapshot withCurve = f.store.current();
  REQUIRE(f.store.dispatch(SetNoteCurve{f.ref, keyOf(inner), curve}).ok());
  CHECK(f.store.current() == withCurve);  // a no-op
  f.roundTrips();
  REQUIRE(f.store.dispatch(RemoveNotes{f.ref, {keyOf(inner)}}).ok());
  CHECK(f.clip().curves == nullptr);
  REQUIRE(f.store.undo());
  CHECK(f.store.current() == withCurve);
  REQUIRE(f.store.dispatch(SetNoteCurve{f.ref, keyOf(inner), {}}).ok());
  CHECK(f.clip().curves == nullptr);
  // The loader refuses what the commands refuse.
  pq::Result<std::string> text = serializeProject(f.doc(), f.store.view());
  REQUIRE(text.ok());
  const std::string needle = "\"jiObject\": " + std::to_string(o.id.value) + ", \"voice\": 1";
  const size_t at = text->find(needle);
  REQUIRE(at != std::string::npos);
  std::string broken = *text;
  broken.replace(at, needle.size(), "\"jiObject\": " + std::to_string(o.id.value) + ", \"voice\": 2");
  pq::Result<LoadedProject> r = parseProject(broken);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().message.find("the note's JI object has no such voice") != std::string::npos);
  MESSAGE(r.error().message);
}

// ---- Resolution ------------------------------------------------------------------------------------------------------

TEST_CASE("ji resolution: Harmonic and JiFundamental notes resolve at their start and follow the fundamental") {
  JiFix f;
  const pq::tuning::Tuning t = edo(12);
  // 4:5:6 over a Degree fundamental (degree 2), then 5:6:7 from kPpq (a jump).
  auto made = ops::createJiObject(f.doc(), f.store.ids(), f.ref, kPpq, 4 * kPpq, PitchId::fromDegree(2), std::vector<int64_t>{4, 5, 6});
  commit(f.store, made);
  const JiObjectId id = made->jiObjects.at(0);
  commit(f.store, ops::addJiStop(f.doc(), f.store.ids(), f.ref, id, kPpq, std::vector<int64_t>{5, 6, 7}));
  const Note a = f.note(kPpq, PitchId::fromHarmonic(id.value, 1));              // at the object's start: 5/4
  const Note b = f.note(3 * kPpq, PitchId::fromHarmonic(id.value, 1));          // after the jump: 6
  const Note c = f.note(2 * kPpq, PitchId::fromRatio(Rational{3, 2}, pq::tuning::AnchorKind::JiFundamental, id.value));
  const Note d = f.note(0, PitchId::fromHarmonic(id.value, 2));                 // before the object: clamped to its start
  REQUIRE(f.store.dispatch(AddNotes{f.ref, {a, b, c, d}}).ok());
  const double fund = *pq::tuning::resolveHzDouble(PitchId::fromDegree(2), t, {});
  CHECK(f.hzOf(a.id) == fund * 5.0);
  CHECK(f.hzOf(b.id) == fund * 6.0);
  CHECK(f.hzOf(c.id) == fund * 1.5);
  CHECK(f.hzOf(d.id) == fund * 6.0);
  // They follow the tuning signal as the fundamental does: its degree, the note keeping its ratio.
  auto r = resolveClipPitches(f.doc(), f.ref.track, f.ref.clip);
  REQUIRE(r.ok());
  for (const NotePitch& np : r->notes) {
    CHECK(np.follow == PitchFollow::Degree);
    CHECK(np.degree == 2);
    CHECK(np.equave == 0);
    CHECK(np.signal == f.t12);
    CHECK(np.keyDegree.has_value());
  }
  // Up / Down on a Harmonic note steps through the object's voices and stops at the last one.
  ops::PitchStepContext ctx;
  ctx.clip = &f.clip();
  CHECK(ops::stepPitch(a.pitch, 1, ops::StepUnit::Grid, ctx) == PitchId::fromHarmonic(id.value, 2));
  CHECK_FALSE(ops::stepPitch(a.pitch, 2, ops::StepUnit::Grid, ctx).has_value());  // V − 1 is the last
  CHECK_FALSE(ops::stepPitch(a.pitch, -2, ops::StepUnit::Grid, ctx).has_value());
  const NoteKey ka = keyOf(a);
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, std::vector<NoteKey>{ka}, 1, ops::StepUnit::Grid));
  CHECK(f.clip().notes.locate(ka)->pitch == PitchId::fromHarmonic(id.value, 2));
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, std::vector<NoteKey>{ka}, 1, ops::StepUnit::Grid));
  CHECK(f.clip().notes.locate(ka)->pitch == PitchId::fromHarmonic(id.value, 2));  // cannot step: unchanged
  // Converting leaves the object with the pitch the note has (here an exact hold: 6 × the fundamental).
  commit(f.store, ops::convertNotes(f.doc(), f.ref, std::vector<NoteKey>{ka}, PitchKind::Cents));
  const Note* frozen = f.clip().notes.locate(ka);
  REQUIRE(frozen->pitch.kind() == PitchKind::Cents);
  CHECK(frozen->pitch.cents() == doctest::Approx(200.0 + 1200.0 * std::log2(6.0)).epsilon(1e-12));
  // setNotesPitch accepts a Harmonic pitch of an object of the clip only.
  CHECK(ops::setNotesPitch(f.doc(), f.ref, std::vector<NoteKey>{ka}, PitchId::fromHarmonic(id.value, 0)).ok());
  CHECK_FALSE(ops::setNotesPitch(f.doc(), f.ref, std::vector<NoteKey>{ka}, PitchId::fromHarmonic(id.value, 3)).ok());
  CHECK_FALSE(ops::setNotesPitch(f.doc(), f.ref, std::vector<NoteKey>{ka}, PitchId::fromHarmonic(id.value + 1000, 0)).ok());
}

// ---- Edit operations -------------------------------------------------------------------------------------------------

TEST_CASE("ji ops: Tune to JI — a 12-EDO major triad becomes 4:5:6 with the lowest voice's Hz unchanged; a tuning stop returns to the written degrees") {
  JiFix f({2, 6, 9});  // D F# A, four beats long
  const pq::tuning::Tuning t = edo(12);
  std::vector<NoteKey> keys;
  std::vector<double> before;
  for (const Note& n : f.clip().notes) {
    keys.push_back(keyOf(n));
    before.push_back(f.hzOf(n.id));
  }
  auto e = ops::jiFromNotes(f.doc(), f.store.ids(), f.ref, keys);
  commit(f.store, e);  // one undo step
  REQUIRE(e->jiObjects.size() == 1);
  const JiObjectId id = e->jiObjects[0];
  const JiObject& o = f.ji(id);
  CHECK(o.start == 0);
  CHECK(o.length == 4 * kPpq);
  CHECK_FALSE(o.sounds);  // the notes stay and play
  CHECK(o.fundamental == PitchId::fromDegree(2));
  REQUIRE(o.stops.size() == 2);
  CHECK(o.stops[0].pitches == std::vector<PitchId>{PitchId::fromDegree(2), PitchId::fromDegree(6), PitchId::fromDegree(9)});
  CHECK(o.stops[1].terms == std::vector<int64_t>{4, 5, 6});
  CHECK(o.stops[1].scale == Rational{1, 4});
  CHECK(o.stops[1].at == kPpq / 4);
  CHECK(o.stops[1].timing.glide == kPpq / 4);
  // The notes are the object's inner notes now and sound exactly as written at their start (stop 0).
  size_t i = 0;
  for (const Note& n : f.clip().notes) {
    CHECK(n.pitch == PitchId::fromHarmonic(id.value, static_cast<uint32_t>(i)));
    CHECK(f.hzOf(n.id) == before[i]);
    ++i;
  }
  // At the proportion the lowest voice has not moved at all; the others are exact ratios over it.
  CHECK(jiVoiceRatio(o, 0, 1) == Rational{1});
  CHECK(*jiVoiceHz(o, t, 0, static_cast<double>(kPpq)) == before[0]);
  CHECK(static_cast<float>(*jiVoiceHz(o, t, 0, static_cast<double>(kPpq / 8))) == static_cast<float>(before[0]));  // mid-glide too
  CHECK(*jiVoiceHz(o, t, 1, static_cast<double>(kPpq)) == before[0] * 1.25);
  CHECK(*jiVoiceHz(o, t, 2, static_cast<double>(kPpq)) == before[0] * 1.5);
  // Back to the tuning: the stop-0 pitches again.
  commit(f.store, ops::addJiTuningStop(f.doc(), f.ref, id, 3 * kPpq, kPpq / 2));
  const JiObject& o2 = f.ji(id);
  REQUIRE(o2.stops.size() == 3);
  CHECK(o2.stops[2].at == 3 * kPpq);
  CHECK(o2.stops[2].pitches == o2.stops[0].pitches);
  CHECK(o2.stops[2].timing.glide == kPpq / 2);
  for (uint32_t v = 0; v < 3; ++v) CHECK(*jiVoiceHz(o2, t, v, static_cast<double>(3 * kPpq)) == before[v]);
  CHECK(*jiVoiceHz(o2, t, 1, static_cast<double>(2 * kPpq)) == before[0] * 1.25);
  f.roundTrips();  // save → load: the tuning stops, the proportion, the inner notes
  // Undo twice: the plain notes again.
  REQUIRE(f.store.undo());
  REQUIRE(f.store.undo());
  CHECK(f.clip().ji == nullptr);
  CHECK(f.clip().notes.begin()->pitch == PitchId::fromDegree(2));

  // replaceNotes: the notes go, the object sounds; without a tuning stop: one proportion stop.
  ops::JiFromNotesOptions opt;
  opt.tuningStop = false;
  opt.replaceNotes = true;
  auto r = ops::jiFromNotes(f.doc(), f.store.ids(), f.ref, keys, opt);
  commit(f.store, r);
  CHECK(f.clip().notes.empty());
  const JiObject& o3 = f.ji(r->jiObjects.at(0));
  CHECK(o3.sounds);
  REQUIRE(o3.stops.size() == 1);
  CHECK(o3.stops[0].terms == std::vector<int64_t>{4, 5, 6});
  CHECK(*jiVoiceHz(o3, t, 0, 0.0) == before[0]);
  // An object made of a proportion only goes back to the nearest degrees of the clip's tuning.
  commit(f.store, ops::addJiTuningStop(f.doc(), f.ref, o3.id, kPpq));
  CHECK(f.ji(o3.id).stops[1].pitches == std::vector<PitchId>{PitchId::fromDegree(2), PitchId::fromDegree(6), PitchId::fromDegree(9)});
  // Errors.
  CHECK_FALSE(ops::jiFromNotes(f.doc(), f.store.ids(), f.ref, {}).ok());
  CHECK_FALSE(ops::jiFromNotes(f.doc(), f.store.ids(), f.ref, std::vector<NoteKey>{NoteKey{0, NoteId{424242}}}).ok());
}

TEST_CASE("ji ops: exact ratios between Ratio notes; a minor triad; unisons share a voice") {
  JiFix f;
  // 9/8, 45/32 and 27/16 above the reference, the 45/32 doubled: three voices 4:5:6, exactly.
  std::vector<Note> notes = {f.note(0, PitchId::fromRatio(Rational{9, 8})), f.note(10, PitchId::fromRatio(Rational{45, 32})),
                             f.note(20, PitchId::fromRatio(Rational{27, 16})), f.note(30, PitchId::fromRatio(Rational{45, 32}))};
  REQUIRE(f.store.dispatch(AddNotes{f.ref, notes}).ok());
  std::vector<NoteKey> keys;
  for (const Note& n : notes) keys.push_back(keyOf(n));
  auto e = ops::jiFromNotes(f.doc(), f.store.ids(), f.ref, keys);
  commit(f.store, e);
  const JiObject& o = f.ji(e->jiObjects.at(0));
  CHECK(o.voices() == 3);
  CHECK(o.stops[1].terms == std::vector<int64_t>{4, 5, 6});
  CHECK(o.fundamental == PitchId::fromRatio(Rational{9, 8}));
  CHECK(f.clip().notes.locate(keys[1])->pitch == PitchId::fromHarmonic(o.id.value, 1));
  CHECK(f.clip().notes.locate(keys[3])->pitch == PitchId::fromHarmonic(o.id.value, 1));
  CHECK(o.start == 0);
  CHECK(o.length == 30 + kPpq);
  // 12-EDO minor triad (0, 3, 7): 6/5 and 3/2 → 10:12:15.
  JiFix g({0, 3, 7});
  std::vector<NoteKey> gk;
  for (const Note& n : g.clip().notes) gk.push_back(keyOf(n));
  auto m = ops::jiFromNotes(g.doc(), g.store.ids(), g.ref, gk);
  commit(g.store, m);
  CHECK(g.ji(m->jiObjects.at(0)).stops[1].terms == std::vector<int64_t>{10, 12, 15});
  CHECK(g.ji(m->jiObjects.at(0)).stops[1].scale == Rational{1, 10});
}

TEST_CASE("ji ops: a walk keeps the pinned voice's pitch exactly (22:26:30:33:39:50:59 -> 21:…)") {
  JiFix f;
  auto made = ops::createJiObject(f.doc(), f.store.ids(), f.ref, 0, kPpq, PitchId::fromRatio(Rational{1}), std::vector<int64_t>{22, 26, 30, 33, 39, 50, 59});
  commit(f.store, made);
  const JiObjectId id = made->jiObjects.at(0);
  const pq::tuning::Tuning t = edo(12);
  const double hz0 = *jiVoiceHz(f.ji(id), t, 0, 0.0);
  commit(f.store, ops::walkJiTerm(f.doc(), f.ref, id, 0, 0, -1, 0));  // term 0 down, voice 0 pinned
  const JiObject& o = f.ji(id);
  CHECK(o.stops[0].terms == std::vector<int64_t>{21, 26, 30, 33, 39, 50, 59});
  CHECK(o.stops[0].scale == Rational{22, 21});
  CHECK(jiVoiceRatio(o, 0, 0) == Rational{22});  // rational equality: the pinned voice did not move
  CHECK(*jiVoiceHz(o, t, 0, 0.0) == hz0);
  CHECK(jiVoiceRatio(o, 1, 0) == Rational{26 * 22, 21});  // everything else is now over 21
  // Pinned elsewhere: only the walked term moves.
  commit(f.store, ops::walkJiTerm(f.doc(), f.ref, id, 0, 6, 1, 3));
  CHECK(f.ji(id).stops[0].terms.back() == 60);
  CHECK(f.ji(id).stops[0].scale == Rational{22, 21});
  CHECK(jiVoiceRatio(f.ji(id), 3, 0) == Rational{33 * 22, 21});
  // Refused: terms must keep ascending; a tuning stop has none.
  CHECK_FALSE(ops::walkJiTerm(f.doc(), f.ref, id, 0, 1, 4, 0).ok());   // 26 → 30 collides
  CHECK_FALSE(ops::walkJiTerm(f.doc(), f.ref, id, 0, 0, -21, 0).ok());  // below 1
  CHECK_FALSE(ops::walkJiTerm(f.doc(), f.ref, id, 1, 0, 1, 0).ok());
  // Undertone: the pinned voice keeps scale / term.
  auto under = ops::createJiObject(f.doc(), f.store.ids(), f.ref, 0, kPpq, PitchId::fromRatio(Rational{1}), std::vector<int64_t>{4, 5, 6}, true);
  commit(f.store, under);
  commit(f.store, ops::walkJiTerm(f.doc(), f.ref, under->jiObjects.at(0), 0, 2, 1, 2));  // 6 → 7, voice 2 pinned
  CHECK(f.ji(under->jiObjects.at(0)).stops[0].terms == std::vector<int64_t>{4, 5, 7});
  CHECK(jiVoiceRatio(f.ji(under->jiObjects.at(0)), 2, 0) == Rational{1, 6});
  CHECK(jiVoiceRatio(f.ji(under->jiObjects.at(0)), 0, 0) == Rational{7, 24});
}

TEST_CASE("ji ops: move, resize, stops, delete — each one undo step") {
  JiFix f;
  auto made = ops::createJiObject(f.doc(), f.store.ids(), f.ref, kPpq, 4 * kPpq, PitchId::fromRatio(Rational{3, 2}), std::vector<int64_t>{4, 5, 6});
  commit(f.store, made);
  const JiObjectId id = made->jiObjects.at(0);
  CHECK(f.ji(id).sounds);
  CHECK(f.ji(id).voices() == 3);
  const Note h = f.note(2 * kPpq, PitchId::fromHarmonic(id.value, 1));
  const Note r = f.note(kPpq, PitchId::fromRatio(Rational{2}, pq::tuning::AnchorKind::JiFundamental, id.value));
  const Note plain = f.note(0, PitchId::fromDegree(0));
  REQUIRE(f.store.dispatch(AddNotes{f.ref, {h, r, plain}}).ok());

  // Move: the inner notes move with it; the plain note stays.
  auto mv = ops::moveJiObject(f.doc(), f.ref, id, kPpq);
  commit(f.store, mv);
  CHECK(f.ji(id).start == 2 * kPpq);
  CHECK(mv->notes.size() == 2);
  CHECK(f.clip().notes.findById(h.id)->start == 3 * kPpq);
  CHECK(f.clip().notes.findById(r.id)->start == 2 * kPpq);
  CHECK(f.clip().notes.findById(plain.id)->start == 0);
  CHECK(ops::moveJiObject(f.doc(), f.ref, id, 0)->empty());

  // Stops: a proportion with more terms grows the voices (fresh note ids); the glide is clamped to the gap.
  commit(f.store, ops::addJiStop(f.doc(), f.store.ids(), f.ref, id, kPpq, std::vector<int64_t>{4, 5, 6, 7}, 10 * kPpq));
  {
    const JiObject& o = f.ji(id);
    REQUIRE(o.stops.size() == 2);
    CHECK(o.voices() == 4);
    CHECK(o.voiceNotes[3].valid());
    CHECK(o.stops[1].timing.glide == kPpq);
    CHECK(validateJiObject(o).ok());
  }
  commit(f.store, ops::addJiStop(f.doc(), f.store.ids(), f.ref, id, kPpq, std::vector<int64_t>{8, 10, 12, 15}));  // replaces the stop there
  CHECK(f.ji(id).stops.size() == 2);
  CHECK(f.ji(id).stops[1].terms == std::vector<int64_t>{8, 10, 12, 15});
  // Fewer voices is refused while a note is on a voice that would go.
  const Note high = f.note(5 * kPpq, PitchId::fromHarmonic(id.value, 3));
  REQUIRE(f.store.dispatch(AddNotes{f.ref, {high}}).ok());
  CHECK_FALSE(ops::setJiStops(f.doc(), f.store.ids(), f.ref, id, {prop(0, {4, 5, 6})}).ok());
  REQUIRE(f.store.dispatch(RemoveNotes{f.ref, {keyOf(high)}}).ok());
  JiStop mapped = prop(0, {4, 5, 6, 7});
  mapped.voiceTerm = {3, 2, 1, 0};
  commit(f.store, ops::setJiStops(f.doc(), f.store.ids(), f.ref, id, {mapped, prop(kPpq, {2, 3}, 100)}));
  CHECK(f.ji(id).voices() == 4);
  commit(f.store, ops::setJiStops(f.doc(), f.store.ids(), f.ref, id, {mapped, prop(kPpq, {5, 6, 7}, 100)}));
  CHECK(f.ji(id).stops[0].voiceTerm.size() == 4);
  // setJiObject: any field; the voice list follows the stops.
  JiObject edited = f.ji(id);
  edited.velocity = 80.0f;
  edited.sounds = false;
  edited.ride = {{0, 0.0f, 0.0f, {}}, {kPpq, 30.0f, 0.5f, {}}, {2 * kPpq, 30.0f, 0.0f, {}}};
  commit(f.store, ops::setJiObject(f.doc(), f.store.ids(), f.ref, edited));
  CHECK(f.ji(id) == edited);

  // Resize the end: only the length; stops past it are kept.
  commit(f.store, ops::resizeJiObject(f.doc(), f.store.ids(), f.ref, id, ops::NoteEdge::End, -(3 * kPpq + kPpq / 2)));
  CHECK(f.ji(id).length == kPpq / 2);
  CHECK(f.ji(id).stops.size() == 2);
  commit(f.store, ops::resizeJiObject(f.doc(), f.store.ids(), f.ref, id, ops::NoteEdge::End, 3 * kPpq + kPpq / 2));
  // Resize the start later, past stop 1: it becomes stop 0, the ride is cut there, the content keeps its time.
  const pq::tuning::Tuning t = edo(12);
  const double wasAt = *jiVoiceCents(f.ji(id), t, 0, static_cast<double>(kPpq + kPpq / 2));
  commit(f.store, ops::resizeJiObject(f.doc(), f.store.ids(), f.ref, id, ops::NoteEdge::Start, kPpq + kPpq / 4));
  {
    const JiObject& o = f.ji(id);
    CHECK(o.start == 3 * kPpq + kPpq / 4);
    CHECK(o.length == 4 * kPpq - kPpq - kPpq / 4);
    REQUIRE(o.stops.size() == 1);
    CHECK(o.stops[0].terms == std::vector<int64_t>{5, 6, 7});
    CHECK(o.voices() == 3);
    REQUIRE(o.ride.size() == 2);
    CHECK(o.ride[0].tick == 0);
    CHECK(o.ride[0].value == 30.0f);
    CHECK(o.ride[1].tick == kPpq - kPpq / 4);
    CHECK(*jiVoiceCents(o, t, 0, static_cast<double>(kPpq / 4)) == doctest::Approx(wasAt));
  }
  // … and earlier: stop 0 extends.
  commit(f.store, ops::resizeJiObject(f.doc(), f.store.ids(), f.ref, id, ops::NoteEdge::Start, -kPpq));
  CHECK(f.ji(id).start == 2 * kPpq + kPpq / 4);
  CHECK(f.ji(id).ride[1].tick == 2 * kPpq - kPpq / 4);
  f.roundTrips();

  // Delete: the inner notes keep their pitch as plain identities, the object goes, one step; undo restores it all.
  commit(f.store, ops::setJiObject(f.doc(), f.store.ids(), f.ref, [&] {
           JiObject o = f.ji(id);
           o.ride.clear();
           return o;
         }()));
  const Snapshot before = commit(f.store, ops::deleteJiObject(f.doc(), f.ref, id));
  CHECK(f.clip().ji == nullptr);
  // h (voice 1 of 5:6:7 on 3/2) → 3/2 · 6 = 9; r (2/1 over the fundamental) → 3.
  CHECK(f.clip().notes.findById(h.id)->pitch == PitchId::fromRatio(Rational{9}));
  CHECK(f.clip().notes.findById(r.id)->pitch == PitchId::fromRatio(Rational{3}));
  CHECK(jiObjectsOf(*findTrackPtr(*before, f.bass)->clips[0].clip).size() == 1);
  f.roundTrips();
}

TEST_CASE("ji ops: deleting an object on a Degree fundamental or in a glide freezes its notes to cents; on a tuning stop they get its pitch") {
  JiFix f({0, 4, 7});
  const pq::tuning::Tuning t = edo(12);
  std::vector<NoteKey> keys;
  for (const Note& n : f.clip().notes) keys.push_back(keyOf(n));
  auto e = ops::jiFromNotes(f.doc(), f.store.ids(), f.ref, keys);
  commit(f.store, e);
  const JiObjectId id = e->jiObjects.at(0);
  // Two more inner notes: one mid-glide, one on the proportion.
  const Note mid = f.note(kPpq / 8, PitchId::fromHarmonic(id.value, 1));
  const Note just = f.note(kPpq, PitchId::fromHarmonic(id.value, 2));
  REQUIRE(f.store.dispatch(AddNotes{f.ref, {mid, just}}).ok());
  const double midCents = *jiVoiceCents(f.ji(id), t, 1, static_cast<double>(kPpq / 8));
  commit(f.store, ops::deleteJiObject(f.doc(), f.ref, id));
  CHECK(f.clip().ji == nullptr);
  CHECK(f.clip().notes.locate(keys[1])->pitch == PitchId::fromDegree(4));  // at stop 0: as written
  REQUIRE(f.clip().notes.findById(mid.id)->pitch.kind() == PitchKind::Cents);
  CHECK(f.clip().notes.findById(mid.id)->pitch.cents() == midCents);
  CHECK(midCents > 386.0);
  CHECK(midCents < 400.0);
  REQUIRE(f.clip().notes.findById(just.id)->pitch.kind() == PitchKind::Cents);  // a Degree fundamental has no exact ratio
  CHECK(f.clip().notes.findById(just.id)->pitch.cents() == doctest::Approx(1200.0 * std::log2(1.5)).epsilon(1e-12));
}

// ---- Curves and objects through the note-id operations ----------------------------------------------------------------

TEST_CASE("note curves: duplicate, paste, delete and convert carry or drop them") {
  JiFix f;
  const Note a = f.note(0, PitchId::fromDegree(0));
  const Note b = f.note(kPpq, PitchId::fromDegree(4));
  REQUIRE(f.store.dispatch(AddNotes{f.ref, {a, b}}).ok());
  const std::vector<Breakpoint> bend = {{0, -200.0f, 0.4f, {}}, {kPpq / 4, 0.0f, 0.0f, {}}};
  auto set = ops::setNoteCurve(f.doc(), f.ref, keyOf(a), bend);
  commit(f.store, set);
  CHECK(ops::setNoteCurve(f.doc(), f.ref, keyOf(a), bend)->empty());
  CHECK_FALSE(ops::setNoteCurve(f.doc(), f.ref, NoteKey{5, NoteId{777777}}, bend).ok());
  CHECK_FALSE(ops::setNoteCurve(f.doc(), f.ref, keyOf(a), {{5, 0.0f, 0.0f, {}}, {1, 0.0f, 0.0f, {}}}).ok());
  // Duplicate: the copy has the curve (one undo step); the plain note's copy has none.
  auto dup = ops::duplicateNotes(f.doc(), f.store.ids(), f.ref, std::vector<NoteKey>{keyOf(a), keyOf(b)}, 8 * kPpq);
  commit(f.store, dup);
  REQUIRE(dup->notes.size() == 2);
  const NoteCurves* copied = findNoteCurves(f.clip(), dup->notes[0].id);
  REQUIRE(copied != nullptr);
  CHECK(copied->pitch == bend);
  CHECK(findNoteCurves(f.clip(), dup->notes[1].id) == nullptr);
  CHECK(noteCurvesOf(f.clip()).size() == 2);
  // Paste: the clipboard's notes and their curves (matched by the ids the notes still carry).
  const std::vector<Note> clipboard = {a, b};
  const std::vector<NoteCurves> clipCurves = {NoteCurves{a.id, bend}};
  std::vector<Note> pasted = clipboard;
  for (Note& n : pasted) n.start += 16 * kPpq;
  auto paste = ops::addNotes(f.doc(), f.store.ids(), f.ref, pasted, clipCurves);
  commit(f.store, paste);
  REQUIRE(findNoteCurves(f.clip(), paste->notes[0].id) != nullptr);
  CHECK(findNoteCurves(f.clip(), paste->notes[0].id)->pitch == bend);
  CHECK(noteCurvesOf(f.clip()).size() == 3);
  // Converting keeps the id, so the curve stays.
  commit(f.store, ops::convertNotes(f.doc(), f.ref, std::vector<NoteKey>{keyOf(a)}, PitchKind::Cents));
  CHECK(findNoteCurves(f.clip(), a.id) != nullptr);
  // Moving / resizing too (the key changes, the id does not).
  commit(f.store, ops::moveNotes(f.doc(), f.ref, std::vector<NoteKey>{keyOf(a)}, 5));
  CHECK(findNoteCurves(f.clip(), a.id)->pitch == bend);
  // Delete: the curve goes with its note.
  commit(f.store, ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{NoteKey{5, a.id}, dup->notes[0]}));
  CHECK(findNoteCurves(f.clip(), a.id) == nullptr);
  CHECK(noteCurvesOf(f.clip()).size() == 1);
  f.roundTrips();
  commit(f.store, ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{paste->notes[0]}));
  CHECK(f.clip().curves == nullptr);
}

TEST_CASE("clip copies, splits and consolidation carry JI objects and curves with fresh ids") {
  JiFix f;
  IdSource& ids = f.store.ids();
  // An object across the first bar line with an inner note on each side, and a curved plain note in bar 2.
  auto made = ops::createJiObject(f.doc(), ids, f.ref, kBar - kPpq, 2 * kPpq, PitchId::fromDegree(0), std::vector<int64_t>{4, 5, 6});
  commit(f.store, made);
  const JiObjectId id = made->jiObjects.at(0);
  const Note left = f.note(kBar - kPpq, PitchId::fromHarmonic(id.value, 0));
  const Note right = f.note(kBar, PitchId::fromHarmonic(id.value, 2));
  const Note curved = f.note(kBar + 2 * kPpq, PitchId::fromDegree(7));
  REQUIRE(f.store.dispatch(AddNotes{f.ref, {left, right, curved}}).ok());
  const std::vector<Breakpoint> bend = {{0, 100.0f, 0.0f, {}}, {1000, 0.0f, 0.0f, {}}};
  REQUIRE(f.store.dispatch(SetNoteCurve{f.ref, keyOf(curved), bend}).ok());
  REQUIRE(f.store.dispatch(SetNoteCurve{f.ref, keyOf(left), bend}).ok());
  const Snapshot original = f.store.current();
  auto clipsOf = [&]() -> const std::vector<ClipSlot>& { return f.track(f.bass).clips; };
  auto idsUnique = [&](const NoteClip& c) { return validateClipJi(c).ok(); };

  SUBCASE("duplicate: fresh object, voice and note ids; the copy's notes follow the copy; curves re-keyed") {
    const ClipRef sel[] = {f.ref};
    auto e = ops::duplicateClips(f.doc(), ids, sel);
    commit(f.store, e);
    REQUIRE(clipsOf().size() == 2);
    const NoteClip& src = *clipsOf()[0].clip;
    const NoteClip& cp = *clipsOf()[1].clip;
    CHECK(src.ji == original->tracks.back()->clips[0].clip->ji);  // the source is untouched (shared)
    REQUIRE(jiObjectsOf(cp).size() == 1);
    const JiObject& o = jiObjectsOf(cp)[0];
    CHECK(o.id != id);
    CHECK(o.voiceNotes[0] != jiObjectsOf(src)[0].voiceNotes[0]);
    CHECK(o.stops == jiObjectsOf(src)[0].stops);
    CHECK(idsUnique(cp));
    size_t harmonic = 0;
    for (const Note& n : cp.notes)
      if (n.pitch.kind() == PitchKind::Harmonic) {
        CHECK(n.pitch.jiObject() == o.id.value);
        ++harmonic;
      }
    CHECK(harmonic == 2);
    REQUIRE(noteCurvesOf(cp).size() == 2);
    for (const NoteCurves& nc : noteCurvesOf(cp)) {
      CHECK(nc.pitch == bend);
      CHECK(cp.notes.findById(nc.note) != nullptr);
      CHECK(src.notes.findById(nc.note) == nullptr);
    }
    f.roundTrips();  // ids are unique across the project
  }
  SUBCASE("split at the bar line: each half keeps what it needs; the right half's copy of the object has fresh ids") {
    const ClipRef sel[] = {f.ref};
    const Ticks cuts[] = {kBar};
    auto e = ops::splitClips(f.doc(), ids, sel, cuts);
    commit(f.store, e);
    REQUIRE(clipsOf().size() == 2);
    const NoteClip& l = *clipsOf()[0].clip;
    const NoteClip& r = *clipsOf()[1].clip;
    REQUIRE(jiObjectsOf(l).size() == 1);
    REQUIRE(jiObjectsOf(r).size() == 1);
    CHECK(jiObjectsOf(l)[0].id == id);
    CHECK(jiObjectsOf(r)[0].id != id);
    CHECK(jiObjectsOf(r)[0].start == kBar - kPpq);  // content ticks: unchanged
    CHECK(l.notes.size() == 1);
    CHECK(r.notes.size() == 2);
    CHECK(r.notes.findById(right.id)->pitch == PitchId::fromHarmonic(jiObjectsOf(r)[0].id.value, 2));
    REQUIRE(noteCurvesOf(l).size() == 1);
    CHECK(noteCurvesOf(l)[0].note == left.id);
    REQUIRE(noteCurvesOf(r).size() == 1);
    CHECK(noteCurvesOf(r)[0].note == curved.id);
    CHECK(idsUnique(l));
    CHECK(idsUnique(r));
    f.roundTrips();
    // Consolidating the halves again: both copies of the object, every curve, the notes following their own copy.
    const ClipRef both[] = {{f.bass, l.id}, {f.bass, r.id}};
    auto c = ops::consolidateClips(f.doc(), ids, both);
    commit(f.store, c);
    REQUIRE(clipsOf().size() == 1);
    const NoteClip& m = *clipsOf()[0].clip;
    CHECK(m.notes.size() == 3);
    CHECK(jiObjectsOf(m).size() == 2);
    CHECK(noteCurvesOf(m).size() == 2);
    CHECK(idsUnique(m));
    for (const JiObject& o : jiObjectsOf(m)) CHECK(o.start == kBar - kPpq);
    f.roundTrips();
  }
  SUBCASE("consolidating a looped clip: each pass gets its own copy of the object, cut at the pass's end") {
    REQUIRE(f.store.dispatch(SetClipLoop{f.ref, ClipLoop{true, 0, 2 * kBar}}).ok());
    const ClipRef sel[] = {f.ref};
    auto c = ops::consolidateClips(f.doc(), ids, sel);  // 4 bars = two passes of the 2-bar loop
    commit(f.store, c);
    const NoteClip& m = *clipsOf()[0].clip;
    CHECK(m.notes.size() == 6);
    REQUIRE(jiObjectsOf(m).size() == 2);
    CHECK(noteCurvesOf(m).size() == 4);
    CHECK(idsUnique(m));
    std::vector<Ticks> starts;
    for (const JiObject& o : jiObjectsOf(m)) {
      starts.push_back(o.start);
      size_t followers = 0;
      for (const Note& n : m.notes)
        if (jiObjectOf(n.pitch) == o.id) {
          ++followers;
          CHECK(n.start >= o.start);
          CHECK(n.start < o.start + o.length);
        }
      CHECK(followers == 2);
    }
    std::sort(starts.begin(), starts.end());
    CHECK(starts == std::vector<Ticks>{kBar - kPpq, 3 * kBar - kPpq});
    f.roundTrips();
  }
}

TEST_CASE("ji file: the rich project's objects, tuning stop and curve survive save -> load; unknown shapes and broken stops are refused with a path") {
  JiFix f;
  auto made = ops::createJiObject(f.doc(), f.store.ids(), f.ref, 0, kPpq, PitchId::fromCents(-350.5), std::vector<int64_t>{4, 5, 6, 7});
  commit(f.store, made);
  JiObject o = f.ji(made->jiObjects.at(0));
  o.undertone = true;
  o.stops[0].scale = Rational{3, 7};
  JiStop tun = written(100, {PitchId::fromDegree(-3, 1), PitchId::fromCents(12.5), PitchId::fromRatio(Rational{7, 4})}, 50, JiShape::Bezier);
  tun.timing.delay = 7;
  tun.timing.shape = JiShape{JiShape::Bezier, 0.1f, -1.0f, 0.9f, 2.0f};
  tun.voiceTerm = {2, 1, 0, 0};
  tun.voiceTiming = {std::nullopt, JiTiming{1, 2, JiShape{JiShape::LogLinear}}, std::nullopt, JiTiming{}};
  o.stops.push_back(tun);
  o.ride = {{0, -0.0f, 1.0f, {}}, {7, 19200.0f, -1.0f, {}}};
  commit(f.store, ops::setJiObject(f.doc(), f.store.ids(), f.ref, o));
  CHECK(f.ji(o.id) == o);
  f.roundTrips();
  pq::Result<std::string> text = serializeProject(f.doc(), f.store.view());
  REQUIRE(text.ok());
  auto broken = [&](std::string_view from, std::string_view to, std::string_view message) {
    std::string s = *text;
    const size_t at = s.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, std::string(from));
    s.replace(at, from.size(), to);
    pq::Result<LoadedProject> r = parseProject(s);
    REQUIRE_FALSE(r.ok());
    CHECK_MESSAGE(r.error().message.find(message) != std::string::npos, r.error().message);
    CHECK(r.error().message.find("$.tracks[") != std::string::npos);
  };
  broken("\"shape\": \"bezier\"", "\"shape\": \"wobble\"", "shape must be");
  broken("\"terms\": [4, 5, 6, 7]", "\"terms\": [4, 5, 5, 7]", "JI terms must ascend");
  broken("\"terms\": [4, 5, 6, 7]", "\"terms\": [4, 5, 6, 7], \"pitches\": [{\"degree\": 0, \"equave\": 0}]", "either terms or pitches");
  broken("\"scale\": \"3/7\"", "\"scale\": \"0/7\"", "scale must be");
  broken("\"voiceTerm\": [2, 1, 0, 0]", "\"voiceTerm\": [2, 1, 0]", "one entry per voice");
  broken("\"undertone\": true", "\"undertone\": true, \"bogus\": 1", "unknown key");
}

// ---- T7.5 (Q279): pitch moves, paste / duplicate, delete with notes ----------------------------------------------------

TEST_CASE("ji ops: moveJiObject steps the fundamental and tuning stops by grid steps, or freely by cents; one undo step") {
  JiFix f({0, 4, 7});
  std::vector<NoteKey> keys;
  for (const Note& n : f.clip().notes) keys.push_back(keyOf(n));
  auto made = ops::jiFromNotes(f.doc(), f.store.ids(), f.ref, keys);
  commit(f.store, made);
  const JiObjectId id = made->jiObjects.at(0);
  const JiObject before = f.ji(id);
  REQUIRE(before.stops.size() == 2);
  REQUIRE(before.stops[0].isTuning());
  const Note first = *f.clip().notes.begin();

  // By rows: every Degree identity steps, the proportion rides on the fundamental; the inner notes move in time.
  auto mv = ops::moveJiObject(f.doc(), f.ref, id, kPpq, 2);
  commit(f.store, mv);
  {
    const JiObject& o = f.ji(id);
    CHECK(o.start == before.start + kPpq);
    CHECK(o.fundamental == PitchId::fromDegree(2));
    CHECK(o.stops[0].pitches == std::vector<PitchId>{PitchId::fromDegree(2), PitchId::fromDegree(6), PitchId::fromDegree(9)});
    CHECK(o.stops[1] == before.stops[1]);
    CHECK(f.clip().notes.findById(first.id)->start == first.start + kPpq);
    CHECK(validateJiObject(o).ok());
  }
  REQUIRE(f.store.undo());
  CHECK(f.ji(id) == before);
  CHECK(f.clip().notes.findById(first.id)->start == first.start);

  // Freely: the fundamental and the tuning stop's pitches become Cents that far away; no time move, notes untouched.
  auto bend = ops::moveJiObject(f.doc(), f.ref, id, 0, 0, nullptr, 37.5);
  commit(f.store, bend);
  {
    const JiObject& o = f.ji(id);
    CHECK(o.start == before.start);
    REQUIRE(o.fundamental.kind() == pq::tuning::PitchKind::Cents);
    CHECK(o.fundamental.cents() == doctest::Approx(37.5));
    REQUIRE(o.stops[0].pitches[1].kind() == pq::tuning::PitchKind::Cents);
    CHECK(o.stops[0].pitches[1].cents() == doctest::Approx(437.5));
    CHECK(o.stops[1] == before.stops[1]);
  }
  REQUIRE(f.store.undo());
  CHECK(ops::moveJiObject(f.doc(), f.ref, id, 0, 0, nullptr, 0.0)->empty());
  CHECK_FALSE(ops::moveJiObject(f.doc(), f.ref, JiObjectId{9999}, 0, 1).ok());
}

TEST_CASE("ji ops: duplicate / paste copy an object with its inner notes and curves under fresh ids; delete with notes") {
  JiFix f;
  auto made = ops::createJiObject(f.doc(), f.store.ids(), f.ref, kPpq, 2 * kPpq, PitchId::fromDegree(0), std::vector<int64_t>{4, 5, 6});
  commit(f.store, made);
  const JiObjectId id = made->jiObjects.at(0);
  const Note h = f.note(kPpq, PitchId::fromHarmonic(id.value, 2));
  const Note r = f.note(2 * kPpq, PitchId::fromRatio(Rational{2}, pq::tuning::AnchorKind::JiFundamental, id.value));
  const Note plain = f.note(0, PitchId::fromDegree(0));
  REQUIRE(f.store.dispatch(AddNotes{f.ref, {h, r, plain}}).ok());
  commit(f.store, ops::setNoteCurve(f.doc(), f.ref, keyOf(h), {{0, 0.0f, 0.0f, {}}, {kPpq, 50.0f, 0.0f, {}}}));
  const JiObject src = f.ji(id);

  auto dup = ops::duplicateJiObject(f.doc(), f.store.ids(), f.ref, id, src.length);
  commit(f.store, dup);
  REQUIRE(dup->jiObjects.size() == 1);
  const JiObjectId copy = dup->jiObjects[0];
  CHECK(copy != id);
  {
    const JiObject& c = f.ji(copy);
    CHECK(c.start == src.start + src.length);
    CHECK(c.stops == src.stops);
    CHECK(c.fundamental == src.fundamental);
    for (NoteId v : c.voiceNotes) CHECK(std::find(src.voiceNotes.begin(), src.voiceNotes.end(), v) == src.voiceNotes.end());
    CHECK(dup->notes.size() == 2);
    int harmonic = 0, anchored = 0;
    for (const Note& n : f.clip().notes) {
      if (n.pitch == PitchId::fromHarmonic(copy.value, 2) && n.start == 3 * kPpq) {
        ++harmonic;
        const NoteCurves* nc = findNoteCurves(f.clip(), n.id);
        REQUIRE(nc != nullptr);
        CHECK(nc->pitch.size() == 2);
      }
      if (n.pitch == PitchId::fromRatio(Rational{2}, pq::tuning::AnchorKind::JiFundamental, copy.value) && n.start == 4 * kPpq) ++anchored;
    }
    CHECK(harmonic == 1);
    CHECK(anchored == 1);
    CHECK(f.clip().notes.size() == 5);
    CHECK(validateClipJi(f.clip()).ok());
  }
  REQUIRE(f.store.undo());  // one step
  CHECK(jiObjectsOf(f.clip()).size() == 1);
  CHECK(f.clip().notes.size() == 3);

  // A paste of an object the clip no longer has (the roll's clipboard), at another tick.
  const std::vector<Note> inner = {h};
  const NoteCurves hc = *findNoteCurves(f.clip(), h.id);
  commit(f.store, ops::deleteJiObject(f.doc(), f.ref, id, true));  // with its notes
  CHECK(jiObjectsOf(f.clip()).empty());
  CHECK(f.clip().notes.size() == 1);  // the plain note stays
  CHECK(f.clip().notes.findById(plain.id) != nullptr);
  auto pasted = ops::pasteJiObject(f.doc(), f.store.ids(), f.ref, src, inner, std::span<const NoteCurves>(&hc, 1), 8 * kPpq);
  commit(f.store, pasted);
  REQUIRE(jiObjectsOf(f.clip()).size() == 1);
  const JiObject& p = jiObjectsOf(f.clip())[0];
  CHECK(p.id != id);
  CHECK(p.start == 8 * kPpq);
  REQUIRE(pasted->notes.size() == 1);
  const Note* pn = f.clip().notes.findById(pasted->notes[0].id);
  REQUIRE(pn != nullptr);
  CHECK(pn->start == 8 * kPpq);
  CHECK(pn->pitch == PitchId::fromHarmonic(p.id.value, 2));
  CHECK(findNoteCurves(f.clip(), pn->id) != nullptr);
  CHECK(validateClipJi(f.clip()).ok());

  // Refused: a note that follows another object, a voice the source does not have.
  const Note stray = f.note(0, PitchId::fromHarmonic(4242, 0));
  CHECK_FALSE(ops::pasteJiObject(f.doc(), f.store.ids(), f.ref, src, std::span<const Note>(&stray, 1), {}, 0).ok());
  const Note high = f.note(0, PitchId::fromHarmonic(src.id.value, 7));
  CHECK_FALSE(ops::pasteJiObject(f.doc(), f.store.ids(), f.ref, src, std::span<const Note>(&high, 1), {}, 0).ok());
}
