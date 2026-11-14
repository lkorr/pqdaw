// T7.3 (docs/plans/M7.md "T7.3", amendment A1; Q275, Q278): JI objects and note curves play. ProjectLink::lowerTrack
// turns a clip's objects into one note per voice with pitch / gain curves, gives inner notes their voice's curve and
// every note its own curve (src/app/src/project_link_ji.h); the timeline plays the curves (T7.1) and chases notes on
// a seek. Two instruments:
//   the probe    an instrument group whose Out is voice.hz (tests/support/blocks.h): the rendered track is
//                K · Σ gain_v · hz_v per sample, so pitch AND gain curves are checked on every sample against the
//                document's own evaluation (doc::jiVoiceHz, the plan's gains);
//   Basic Synth  the default (sine) instrument group, for what is about audio: unison lanes, the merge level,
//                interpreter == JIT, edits while playing.
// 120 BPM: a quarter note is 0.5 s = 24000 samples. A probe voice fades in over its first milliseconds (the voice
// engine's de-click), so comparisons start kAttack samples after a note-on.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/automation_curve.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/ji_object.h"
#include "pq/document/pitch_resolve.h"
#include "pq/tuning/mos_tuning.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;
using pq::tuning::PitchId;
using pq::tuning::Rational;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr double kTicksPerSample = static_cast<double>(kPpq) / 24000.0;
constexpr Ticks kQ = kPpq;
constexpr double kRef = pq::tuning::kDefaultReferenceHz;
constexpr size_t kAttack = 2400;
// The lowering flattens shapes to 1 c (project_link_ji.h kJiPitchTolerance) and thins breakpoints to 1 ms.
constexpr double kGlideCents = 1.2;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, bool jit = false) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

std::vector<float> render(ProjectLink& link, double seconds) {
  engine::BufferSink sink;
  auto st = link.renderOffline(0, static_cast<uint64_t>(std::llround(seconds * kRate)), sink);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  REQUIRE(sink.channels.size() == 2);
  return sink.channels[0];
}

size_t at(double seconds) { return static_cast<size_t>(std::llround(seconds * kRate)); }

// Drives a link's engine block by block like a device (RtScope: the trap fails the test on an allocation).
struct Driver {
  ProjectLink& link;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  std::vector<float> out;
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      out.insert(out.end(), l.begin(), l.end());
    }
  }
};

// One MIDI track "JI" in the default project (12-EDO, reference 261.626 Hz) with one clip from tick 0.
struct Rig {
  DocumentStore store{playback_fixture::unwiredDefaultProject()};
  TrackId track;
  ClipRef ref;
  DeviceId group;

  explicit Rig(bool probe, std::vector<Note> notes = {}, Ticks clipLength = 16 * kQ) {
    IdSource& ids = store.ids();
    track = ids.track();
    Batch b{"ji fixture", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "JI"), {}, kAppend});
    ClipPtr c = playback_fixture::clip(ids, 0, clipLength, std::move(notes), "JI");
    ref = ClipRef{track, c->id};
    b.commands.push_back(AddClip{track, c});
    if (!probe) group = playback_fixture::instrument(b, ids, track, ids.device());
    const Status st = store.dispatch(b);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    playback_fixture::wireSeries(store);
    if (probe) group = test_blocks::addProbeInstrument(store, track);
  }
  const Project& doc() const { return *store.current(); }
  const NoteClip& clip() const { return *findTrackPtr(doc(), track)->clips.front().clip; }
  const JiObject& object(JiObjectId id) const {
    const JiObject* o = findJiObject(clip(), id);
    REQUIRE(o != nullptr);
    return *o;
  }
  JiObjectId edit(const ops::EditResult& e) {
    test_blocks::dispatchEdit(store, e);
    return e->jiObjects.empty() ? JiObjectId{} : e->jiObjects.front();
  }
  // A sounding object with one proportion stop.
  JiObjectId create(Ticks start, Ticks length, std::vector<int64_t> terms, PitchId fundamental = PitchId::fromDegree(0),
                    bool undertone = false) {
    return edit(ops::createJiObject(doc(), store.ids(), ref, start, length, fundamental, terms, undertone));
  }
  JiObjectId stop(JiObjectId o, Ticks atTick, std::vector<int64_t> terms, Ticks glide) {
    return edit(ops::addJiStop(doc(), store.ids(), ref, o, atTick, terms, glide));
  }
  void set(JiObject o) { (void)edit(ops::setJiObject(doc(), store.ids(), ref, std::move(o))); }
  tuning::Tuning tuning() const {
    auto t = buildTuning(*effectiveTuningRef(doc(), *findTrack(doc(), track), &clip()).def);
    REQUIRE(t.ok());
    return std::move(t.value());
  }
};

// The lowering's gain ramp at 120 BPM: 20 ms.
constexpr Ticks kGainRamp = kPpq / 25;

double gainAt(const std::vector<JiGainPoint>& g, double tick) {
  auto it = std::upper_bound(g.begin(), g.end(), tick, [](double v, const JiGainPoint& p) { return v < static_cast<double>(p.tick); });
  if (it == g.begin()) return g.front().gain;
  const JiGainPoint& a = *(it - 1);
  if (it == g.end() || static_cast<double>(a.tick) == tick) return a.gain;
  const double x = (tick - static_cast<double>(a.tick)) / static_cast<double>(it->tick - a.tick);
  return a.gain + (it->gain - a.gain) * x;
}

// What the probe plays for a sounding object: Σ gain_v · hz_v at content tick `tick` (0 outside the object).
struct ObjectSum {
  JiObject o;
  tuning::Tuning t;
  JiPlan plan;
  ObjectSum(const JiObject& object, tuning::Tuning tun) : o(object), t(std::move(tun)) {
    JiPlanOptions po;
    po.gainRamp = kGainRamp;
    auto p = jiVoicePlan(o, t, po);
    REQUIRE(p.has_value());
    plan = std::move(*p);
  }
  double operator()(double tick) const {
    const double rel = tick - static_cast<double>(o.start);
    if (rel < 0.0 || rel >= static_cast<double>(o.length)) return 0.0;
    double s = 0.0;
    for (uint32_t v = 0; v < o.voices(); ++v) s += gainAt(plan.voices[v].gain, rel) * *jiVoiceHz(o, t, v, rel);
    return s;
  }
};

// The worst |out[n] / K − want(tick of n)| / want over samples [from, to), every `step`-th sample.
template <class F>
double worstRel(const std::vector<float>& out, size_t from, size_t to, double K, F&& want, double tick0 = 0.0, size_t step = 1) {
  double worst = 0.0;
  for (size_t n = from; n < std::min(to, out.size()); n += step) {
    const double w = want(tick0 + static_cast<double>(n) * kTicksPerSample);
    if (!(w > 0.0)) continue;
    worst = std::max(worst, std::fabs(static_cast<double>(out[n]) / K - w) / w);
  }
  return worst;
}
double cents(double rel) { return 1200.0 * std::log2(1.0 + rel); }

const engine::TimelineStats& tstats(ProjectLink& l) { return l.engine().timeline().stats(); }

double rms(const std::vector<float>& x, size_t from, size_t n) {
  double s = 0.0;
  for (size_t i = from; i < from + n; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(n));
}

}  // namespace

TEST_CASE("ji playback: 4:5:6 -> 2:3 -> 1 — every voice's pitch and gain follow the plan on every sample (probe)") {
  Rig r(true);
  const JiObjectId id = r.create(0, 8 * kQ, {4, 5, 6});
  (void)r.stop(id, 3 * kQ, {2, 3}, kQ);   // glide over [2 q, 3 q]: voices 0 and 1 merge on 2, voice 2 -> 3
  (void)r.stop(id, 6 * kQ, {1}, kQ);      // glide over [5 q, 6 q]: everything merges on 1
  const ObjectSum want(r.object(id), r.tuning());
  auto link = makeLink(r.store.current());
  const std::vector<float> out = render(*link, 4.2);
  CHECK(link->lastError().empty());
  // The first hold: 4 f + 5 f + 6 f. K (the track's constant gains) from one sample of it.
  REQUIRE(want(100.0) == doctest::Approx(15.0 * kRef).epsilon(1e-12));
  const double K = static_cast<double>(out[at(0.5)]) / (15.0 * kRef);
  REQUIRE(K > 0.0);
  // Holds are exact (float32): 15 f, then 2 f + 3 f (voice 1 parked on voice 0's pitch), then f.
  CHECK(worstRel(out, kAttack, at(1.0), K, [](double) { return 15.0 * kRef; }) < 2e-6);
  CHECK(worstRel(out, at(1.5) + 8, at(2.5), K, [](double) { return 5.0 * kRef; }) < 2e-6);
  CHECK(worstRel(out, at(3.0) + 8, at(4.0), K, [](double) { return 1.0 * kRef; }) < 2e-6);
  // Everything, glides and gain ramps included, against the document's evaluation. A gain ramp's ends land on whole
  // samples (≤ 1/2 sample of a 960-sample ramp); the pitch is flattened to 1 c.
  const double worst = worstRel(out, kAttack, at(4.0), K, want);
  CHECK(worst < 1.5e-3);
  // The glides alone, where no gain moves (the first 0.4 s of each): the pitch within the flattening tolerance.
  const double glide = std::max(worstRel(out, at(1.0), at(1.4), K, want), worstRel(out, at(2.5), at(2.9), K, want));
  CHECK(cents(glide) < kGlideCents);
  MESSAGE("worst relative error " << worst << "; in the glides " << cents(glide) << " c");
  // Three voices, their ids the object's voice notes; no event was dropped.
  const engine::TimelineStats& ts = tstats(*link);
  CHECK(ts.noteOns == 3);
  CHECK(ts.droppedCurveEvents == 0);
  CHECK(ts.curveEvents > 20);
  const engine::SequenceImage* img = link->engine().timeline().sequence();
  REQUIRE(img != nullptr);
  REQUIRE(img->notes().size() == 3);
  for (uint32_t v = 0; v < 3; ++v) {
    const engine::SequenceImage::Note& n = img->notes()[v];
    CHECK(n.id == r.object(id).voiceNotes[v].value);
    CHECK(n.hz == static_cast<float>(kRef * (4.0 + v)));
    CHECK(n.ratio == static_cast<float>(4.0 + v));
    CHECK(n.harmonic == static_cast<float>(4.0 + v));
    CHECK(n.unisonCount == 1);
    CHECK(n.unisonIndex == 0);
    // T7.7: voice.noteDegree — where the voice starts among the tuning's degrees (12-EDO, the fundamental on degree 0).
    CHECK(n.keyDegree == doctest::Approx(12.0 * std::log2(4.0 + v)).epsilon(1e-6));
  }
  // T7.7: at 3 q voice 1 is Synced to voice 0 (the point the jump leaves carries the Sync) and both step to 1/2,
  // keyed by voice 0's note.
  const uint32_t keeper = r.object(id).voiceNotes[0].value;
  const auto g0 = img->gainCurve(img->notes()[0]), g1 = img->gainCurve(img->notes()[1]);
  auto has = [](std::span<const engine::CurvePoint> g, const engine::CurvePoint& p) { return std::find(g.begin(), g.end(), p) != g.end(); };
  CHECK(has(g0, engine::CurvePoint{3 * kQ, 1.0f, 0}));
  CHECK(has(g0, engine::CurvePoint{3 * kQ, 0.5f, keeper}));
  CHECK(has(g1, engine::CurvePoint{3 * kQ, 0.0f, keeper}));
  CHECK(has(g1, engine::CurvePoint{3 * kQ, 0.5f, keeper}));
  CHECK(ts.syncEvents == 2);  // voice 1 at 3 q; voice 2 at 6 q (voices 0 and 1 are identical lanes by then)
}

TEST_CASE("ji playback: a unison from the start is bit-identical lanes — three voices at 1/3 are one voice (sine)") {
  // Stop 0 is the single term 4 (the three voices share it), the split to 4:5:6 comes at 3 q.
  Rig three(false);
  const JiObjectId id = three.create(0, 8 * kQ, {4, 5, 6});
  {
    JiObject o = three.object(id);
    JiStop s0 = o.stops[0], s1 = o.stops[0];
    s0.terms = {4};
    s1.at = 3 * kQ;
    s1.timing.glide = kQ / 2;
    o.stops = {s0, s1};
    three.set(o);
  }
  Rig one(false);
  (void)one.create(0, 8 * kQ, {4});
  auto la = makeLink(three.store.current()), lb = makeLink(one.store.current());
  const std::vector<float> a = render(*la, 1.2), b = render(*lb, 1.2);  // before the split begins (1.25 s)
  double worst = 0.0, peak = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    worst = std::max(worst, static_cast<double>(std::fabs(a[i] - b[i])));
    peak = std::max(peak, static_cast<double>(std::fabs(b[i])));
  }
  CHECK(peak > 0.05);
  CHECK(worst <= 1e-6);
  MESSAGE("three voices at 1/3 vs one voice: worst difference " << worst << " (peak " << peak << ")");
  const engine::SequenceImage* img = la->engine().timeline().sequence();
  REQUIRE(img->notes().size() == 3);
  for (uint32_t v = 0; v < 3; ++v) {
    CHECK(img->notes()[v].unisonCount == 3);
    CHECK(img->notes()[v].unisonIndex == v);
    CHECK(img->notes()[v].hz == img->notes()[0].hz);
    REQUIRE(img->gainCurve(img->notes()[v]).size() >= 2);
    CHECK(img->gainCurve(img->notes()[v])[0].value == 1.0f / 3.0f);
  }
}

TEST_CASE("ji playback: a merge — the voice fades as it closes in, is Synced, and the unison is the keeper sample for sample (sine; T7.7, Q281)") {
  // 4:5 -> 4: voice 1 glides onto voice 0 (which never moves) over [1 q, 2 q]; its gain is the squared distance to the
  // keeper (ji_object.h "Gains"). At 2 q (1 s) it is Synced to the keeper and both step to 1/2. The reference is the
  // keeper alone.
  Rig two(false);
  const JiObjectId id = two.create(0, 8 * kQ, {4, 5});
  (void)two.stop(id, 2 * kQ, {4}, kQ);
  Rig keeper(false);
  (void)keeper.create(0, 8 * kQ, {4});
  auto la = makeLink(two.store.current()), lb = makeLink(keeper.store.current());
  const std::vector<float> a = render(*la, 3.0), b = render(*lb, 3.0);
  auto worstDb = [&](size_t win, size_t hop, double from, double to) {
    double worst = 0.0;
    for (size_t i = at(from); i + win <= std::min(a.size(), at(to)); i += hop)
      worst = std::min(worst, 20.0 * std::log10(rms(a, i, win) / rms(b, i, win)));
    return worst;
  };
  CHECK(tstats(*la).syncEvents == 1);
  // The level over 100 ms windows (every 10 ms, from the end of the attack): never 1 dB below the keeper.
  const double level = worstDb(4800, 480, 0.05, 3.0);
  CHECK(level > -1.0);
  // From the end of the merge on: two identical lanes at 1/2 — the keeper alone, sample for sample, in every 10 ms
  // window (the plan's bound is -0.5 dB).
  CHECK(worstDb(480, 240, 1.0, 3.0) > -0.01);
  double diff = 0.0;
  for (size_t i = at(1.0); i < a.size(); ++i) diff = std::max(diff, static_cast<double>(std::fabs(a[i] - b[i])));
  CHECK(diff <= 1e-6);
  // While the two free-running voices converge they beat; the fade keeps the slow beats of the last part of the glide
  // (where a 10 ms window sees a whole trough) shallow. Before T7.7 (full gain until a 20 ms fade): -5.4 dB here, and
  // down to -28 dB at the worst relative phase (Q281).
  const double early = worstDb(480, 240, 0.5, 0.75), late = worstDb(480, 240, 0.75, 1.0);
  CHECK(late > -1.5);
  CHECK(early > -6.0);
  MESSAGE("merge 4:5 -> 4 against the keeper alone: 100 ms windows " << level << " dB; 10 ms windows " << early
          << " dB in the first half of the glide, " << late << " dB in the second; merged: difference " << diff);
}

// T7.7's acceptance (docs/plans/M7.md): 4:5:6:7 -> 1 -> 4:5:6:7. The reference is one voice on the keeper's path
// (4 -> 1 -> 4).
TEST_CASE("ji playback: 4:5:6:7 -> 1 -> 4:5:6:7 — after the merge the unison is one voice, the split's level rises monotonically; interpreter == JIT; a chase lands in the unison (sine; T7.7)") {
  auto make = [](std::vector<int64_t> chord, Rig& r) {
    const JiObjectId id = r.create(0, 8 * kQ, std::move(chord));
    (void)r.stop(id, 2 * kQ, {1}, kQ);
    return r.stop(id, 4 * kQ, r.object(id).stops[0].terms, kQ);
  };
  Rig four(false), one(false);
  (void)make({4, 5, 6, 7}, four);
  (void)make({4}, one);
  auto la = makeLink(four.store.current()), lb = makeLink(one.store.current());
  const std::vector<float> a = render(*la, 3.0), b = render(*lb, 3.0);
  CHECK(tstats(*la).syncEvents == 3);
  CHECK(tstats(*la).droppedCurveEvents == 0);
  // Merged ([1 s, 1.5 s): from the Sync to the tick the split begins): four identical lanes at 1/4 are the single
  // voice, sample for sample; no 10 ms window is below it (the plan: none below -0.5 dB).
  double diff = 0.0, peak = 0.0, worst = 0.0;
  for (size_t i = at(1.0); i < at(1.5); ++i) {
    diff = std::max(diff, static_cast<double>(std::fabs(a[i] - b[i])));
    peak = std::max(peak, static_cast<double>(std::fabs(b[i])));
  }
  for (size_t i = at(1.0); i + 480 <= at(1.5); i += 120)
    worst = std::min(worst, 20.0 * std::log10(rms(a, i, 480) / rms(b, i, 480)));
  CHECK(peak > 0.05);
  CHECK(diff <= 1e-6);
  CHECK(worst > -0.01);
  // No jump into the unison or out of it: around the Sync (1 s) and the first tick of the split (1.5 s) consecutive
  // samples differ no more than the single voice's do.
  for (const double t : {1.0, 1.5}) {
    double stepA = 0.0, stepB = 0.0;
    for (size_t i = at(t) - 200; i < at(t) + 200; ++i) {
      stepA = std::max(stepA, static_cast<double>(std::fabs(a[i + 1] - a[i])));
      stepB = std::max(stepB, static_cast<double>(std::fabs(b[i + 1] - b[i])));
    }
    CHECK(stepA <= stepB * 1.5 + 1e-4);
  }
  // The split ([1.5 s, 2 s]): the level over 50 ms windows rises from one voice's to the chord's and never falls
  // (0.5 dB of slack for the beats inside a window); it never falls below the single voice either.
  double prev = -100.0, fall = 0.0, below = 0.0, first = 0.0, last = 0.0;
  for (size_t i = at(1.5) - 2400; i + 2400 <= at(2.1); i += 480) {
    const double db = 20.0 * std::log10(rms(a, i, 2400) / rms(b, at(1.2), 2400));
    if (prev > -100.0) fall = std::max(fall, prev - db);
    else first = db;
    below = std::min(below, db);
    prev = last = db;
  }
  CHECK(std::fabs(first) < 0.1);
  CHECK(last > 4.0);  // four voices at different pitches: about +6 dB
  CHECK(fall < 0.5);
  CHECK(below > -0.25);
  MESSAGE("4:5:6:7 -> 1 -> 4:5:6:7: merged difference " << diff << " (peak " << peak << "); split level " << first << " -> "
          << last << " dB, largest fall between 50 ms windows " << fall << " dB");
  // interpreter == JIT.
  auto jit = makeLink(four.store.current(), true);
  REQUIRE(jit->waitForJit());
  const std::vector<float> c = render(*jit, 3.0);
  REQUIRE(a.size() == c.size());
  CHECK(std::memcmp(a.data(), c.data(), a.size() * sizeof(float)) == 0);
  // A chase into the unison (2.5 q), driven like a device (RtScope): the four voices start at one frame at 1/4 and
  // are Synced; the sound is the single voice chased to the same place, without a click.
  auto ca = makeLink(four.store.current()), cb = makeLink(one.store.current());
  Driver da{*ca}, db{*cb};
  ca->play(5 * kQ / 2);
  cb->play(5 * kQ / 2);
  da.run(150);
  db.run(150);
  CHECK(tstats(*ca).chasedNotes == 4);
  CHECK(tstats(*ca).syncEvents == 3);
  double chase = 0.0, chasePeak = 0.0;
  for (size_t i = 0; i < da.out.size(); ++i) {
    chase = std::max(chase, static_cast<double>(std::fabs(da.out[i] - db.out[i])));
    chasePeak = std::max(chasePeak, static_cast<double>(std::fabs(db.out[i])));
  }
  CHECK(chasePeak > 0.05);
  CHECK(chase <= 1e-5);
}

TEST_CASE("ji playback: a ride curve, an undertone object, a note's own curve and a note on the fundamental (probe)") {
  SUBCASE("ride: one voice on 3 under a curved ride") {
    Rig r(true);
    const JiObjectId id = r.create(0, 6 * kQ, {3});
    JiObject o = r.object(id);
    o.ride = {{0, 0.0f, 0.6f, {}}, {kQ, 50.0f, 0.0f, {}}, {2 * kQ, -30.0f, -0.8f, {}}, {3 * kQ, 0.0f, 0.0f, {}},
              {4 * kQ, 0.0f, 0.0f, {}}, {4 * kQ, 100.0f, 0.0f, {}}};  // … and a jump of 100 c at 4 q
    r.set(o);
    const ObjectSum want(r.object(id), r.tuning());
    auto link = makeLink(r.store.current());
    const std::vector<float> out = render(*link, 3.1);
    const double K = static_cast<double>(out[at(1.75)]) / want(3.5 * kQ);  // the flat stretch [3 q, 4 q]
    CHECK(want(3.5 * kQ) == doctest::Approx(3.0 * kRef).epsilon(1e-12));
    const double worst = worstRel(out, kAttack, at(3.0), K, want);
    CHECK(cents(worst) < kGlideCents);
    CHECK(worstRel(out, at(2.0) + 2, at(3.0), K, [](double) { return 3.0 * kRef * std::exp2(100.0 / 1200.0); }) < 2e-6);
    MESSAGE("ride: worst " << cents(worst) << " c");
  }
  SUBCASE("undertone: 1/4 : 1/5 : 1/6 -> 1/2 : 1/3") {
    Rig r(true);
    const JiObjectId id = r.create(0, 6 * kQ, {4, 5, 6}, PitchId::fromCents(2400.0), true);
    (void)r.stop(id, 3 * kQ, {2, 3}, kQ);
    const ObjectSum want(r.object(id), r.tuning());
    auto link = makeLink(r.store.current());
    const std::vector<float> out = render(*link, 3.0);
    const double f = 4.0 * kRef;
    REQUIRE(want(10.0) == doctest::Approx(f * (1.0 / 4 + 1.0 / 5 + 1.0 / 6)).epsilon(1e-12));
    const double K = static_cast<double>(out[at(0.5)]) / want(kQ);
    CHECK(worstRel(out, kAttack, at(1.0), K, [&](double) { return f * (1.0 / 4 + 1.0 / 5 + 1.0 / 6); }) < 2e-6);
    CHECK(worstRel(out, kAttack, at(2.99), K, want) < 1.5e-3);
    // After the morph: the two voices that merged on 1/2 (one parked) and the one on 1/3.
    const JiPlan& plan = want.plan;
    double held = 0.0;
    for (uint32_t v = 0; v < 3; ++v) held += plan.voices[v].gain.back().gain * *jiVoiceHz(want.o, want.t, v, 4.0 * kQ);
    CHECK(held == doctest::Approx(f * (1.0 / 2 + 1.0 / 3)).epsilon(1e-9));
    CHECK(worstRel(out, at(1.5) + 8, at(2.9), K, [&](double) { return held; }) < 2e-6);
  }
  SUBCASE("a plain note's own pitch curve (a bend with a curved segment and a jump)") {
    Rig r(true, {});
    Note n = playback_fixture::note(r.store.ids(), kQ, 4 * kQ, 7);
    auto added = ops::addNotes(r.doc(), r.store.ids(), r.ref, std::vector<Note>{n});
    test_blocks::dispatchEdit(r.store, added);
    const std::vector<Breakpoint> curve = {{0, -200.0f, 0.7f, {}}, {kQ, 0.0f, 0.0f, {}}, {2 * kQ, 0.0f, 0.0f, {}},
                                           {2 * kQ, 35.0f, 0.0f, {}}, {3 * kQ, -35.0f, 0.0f, {}}};
    test_blocks::dispatchEdit(r.store, ops::setNoteCurve(r.doc(), r.ref, added->notes.front(), curve));
    auto link = makeLink(r.store.current());
    const std::vector<float> out = render(*link, 2.6);
    const double hz = kRef * std::exp2(7.0 / 12.0);
    auto want = [&](double tick) {
      const double rel = tick - static_cast<double>(kQ);
      if (rel < 0.0 || rel >= 4.0 * kQ) return 0.0;
      return hz * std::exp2(evalAutomation(std::span<const Breakpoint>(curve), rel) / 1200.0);
    };
    const double K = static_cast<double>(out[at(1.25)]) / hz;  // the flat stretch [1 q, 2 q] of the curve
    const double worst = worstRel(out, at(0.5) + kAttack, at(2.5), K, want);
    CHECK(cents(worst) < kGlideCents);
    CHECK(worstRel(out, at(1.0) + 2, at(1.5), K, [&](double) { return hz; }) < 2e-6);
    CHECK(out[at(0.5) - 2] == 0.0f);  // the note starts at 1 q
    MESSAGE("note curve: worst " << cents(worst) << " c");
  }
  SUBCASE("a scaffold: an inner note follows its voice (plus its own curve), a 3/2 on the fundamental follows it") {
    Rig r(true);
    const JiObjectId id = r.create(0, 8 * kQ, {4, 5, 6});
    {
      JiObject o = r.object(id);
      o.sounds = false;
      JiStop s1 = o.stops[0];
      s1.at = 3 * kQ;
      s1.terms = {4, 5, 7};
      s1.scale = *Rational::tryMake(9, 8);  // the fundamental walks up a 9/8
      s1.timing.glide = kQ;
      o.stops.push_back(s1);
      r.set(o);
    }
    Note inner = playback_fixture::note(r.store.ids(), kQ, 5 * kQ, 0);
    inner.pitch = PitchId::fromHarmonic(id.value, 2);  // 6 -> 7 · 9/8
    Note onFund = playback_fixture::note(r.store.ids(), kQ / 2, 5 * kQ, 0);
    onFund.pitch = PitchId::fromRatio(*Rational::tryMake(3, 2), pq::tuning::AnchorKind::JiFundamental, id.value);
    auto added = ops::addNotes(r.doc(), r.store.ids(), r.ref, std::vector<Note>{inner, onFund});
    test_blocks::dispatchEdit(r.store, added);
    REQUIRE(added->notes.size() == 2);
    const NoteKey innerKey = added->notes[0].start == kQ ? added->notes[0] : added->notes[1];
    const std::vector<Breakpoint> own = {{0, 0.0f, 0.0f, {}}, {3 * kQ, 0.0f, 0.0f, {}}, {4 * kQ, 20.0f, 0.0f, {}}};
    test_blocks::dispatchEdit(r.store, ops::setNoteCurve(r.doc(), r.ref, innerKey, own));
    const JiObject o = r.object(id);
    const tuning::Tuning t = r.tuning();
    auto want = [&](double tick) {
      double s = 0.0;
      if (tick >= kQ && tick < 6.0 * kQ)
        s += *jiVoiceHz(o, t, 2, tick) * std::exp2(evalAutomation(std::span<const Breakpoint>(own), tick - kQ) / 1200.0);
      if (tick >= kQ / 2.0 && tick < 5.5 * kQ) s += 1.5 * *jiFundamentalHz(o, t, tick);
      return s;
    };
    auto link = makeLink(r.store.current());
    const std::vector<float> out = render(*link, 3.2);
    CHECK(tstats(*link).noteOns == 2);  // the scaffold itself does not sound
    {  // T7.7: an inner Harmonic note carries voice.harmonic and its exact ratio to the fundamental at its start
      const engine::SequenceImage* img = link->engine().timeline().sequence();
      REQUIRE(img != nullptr);
      REQUIRE(img->notes().size() == 2);
      const engine::SequenceImage::Note& in = img->notes()[img->notes()[0].start == kQ ? 0 : 1];
      CHECK(in.harmonic == 6.0f);
      CHECK(in.ratio == 6.0f);
    }
    const double K = static_cast<double>(out[at(0.75)]) / want(1.5 * kQ);
    // Before the morph: 6 f + 3/2 f; after it (3 q … 4 q, before the own curve moves): 7 · 9/8 f + 3/2 · 9/8 f.
    CHECK(worstRel(out, at(0.5) + kAttack, at(1.0), K, [](double) { return 7.5 * kRef; }) < 2e-6);
    CHECK(worstRel(out, at(1.5) + 2, at(2.0), K, [](double) { return (7.0 + 1.5) * 9.0 / 8.0 * kRef; }) < 3e-6);
    const double worst = worstRel(out, at(0.5) + kAttack, at(2.7), K, want);
    CHECK(cents(worst) < kGlideCents);
    MESSAGE("inner note + fundamental note: worst " << cents(worst) << " c");
  }
}

TEST_CASE("ji playback: Tune to JI — a 12-EDO triad morphs into 4:5:6 and lands on the exact ratios (A1, Q275)") {
  for (const bool replace : {false, true}) {
    CAPTURE(replace);
    Rig r(true);
    std::vector<Note> notes = {playback_fixture::note(r.store.ids(), 0, 6 * kQ, 0), playback_fixture::note(r.store.ids(), 0, 6 * kQ, 4),
                               playback_fixture::note(r.store.ids(), 0, 6 * kQ, 7)};
    auto added = ops::addNotes(r.doc(), r.store.ids(), r.ref, notes);
    test_blocks::dispatchEdit(r.store, added);
    ops::JiFromNotesOptions jo;
    jo.glide = kQ;
    jo.replaceNotes = replace;
    const JiObjectId id = r.edit(ops::jiFromNotes(r.doc(), r.store.ids(), r.ref, added->notes, jo));
    const JiObject o = r.object(id);
    REQUIRE(o.stops.size() == 2);
    REQUIRE(o.stops[0].isTuning());
    CHECK(o.sounds == replace);  // Q275: by default the notes stay, as the inner notes of a scaffold
    const tuning::Tuning t = r.tuning();
    auto want = [&](double tick) {
      double s = 0.0;
      for (uint32_t v = 0; v < 3; ++v) s += *jiVoiceHz(o, t, v, tick);
      return tick < 6.0 * kQ ? s : 0.0;
    };
    auto link = makeLink(r.store.current());
    const std::vector<float> out = render(*link, 2.9);
    CHECK(tstats(*link).noteOns == 3);
    // Tick 0 is the chord as written (12-EDO); from 1 q on it is 4:5:6 on the unmoved lowest note.
    const double edo = kRef * (1.0 + std::exp2(4.0 / 12.0) + std::exp2(7.0 / 12.0));
    const double ji = kRef * (1.0 + 5.0 / 4.0 + 3.0 / 2.0);
    const double K = static_cast<double>(out[at(1.0)]) / ji;
    CHECK(want(0.0) == doctest::Approx(edo).epsilon(1e-12));
    CHECK(want(static_cast<double>(kQ)) == doctest::Approx(ji).epsilon(1e-12));
    const double landed = worstRel(out, at(0.5) + 2, at(2.9), K, [&](double) { return ji; });
    CHECK(landed < 2e-6);  // exact ratios: float32 Hz, nothing accumulated by the morph
    const double worst = worstRel(out, kAttack, at(2.9), K, want);
    CHECK(cents(worst) < kGlideCents);
    // … and it really was the 12-EDO chord at the start (the major third 13.7 c above 5/4).
    CHECK(static_cast<double>(out[kAttack]) / K > ji * 1.001);
    MESSAGE(std::string(replace ? "sounding object" : "inner notes of a scaffold") << ": landed within " << landed << " (relative), morph within "
                                                                      << cents(worst) << " c");
  }
}

TEST_CASE("ji playback: a Degree fundamental under a modulated MOS generator — the object follows the tuning signal") {
  // 5L2s, the generator automated 700 c -> 715 c over bar 1 (2 s); the object's fundamental is degree 2 of it, one
  // voice on 3 morphing to 4 over [2 q, 3 q].
  Rig r(true);
  IdSource& ids = r.store.ids();
  const TuningId tid = ids.tuning();
  pq::tuning::MosParams mos;
  mos.n = 7;
  mos.period = 1200.0;
  mos.generator = 700.0;
  {
    Batch b{"mos", {}};
    TuningSlot s{tid, "5L2s", Colour{0xAA4422}, {}};
    s.def.system = mos;
    b.commands.push_back(AddTuning{s});
    b.commands.push_back(SetTrackTuning{r.track, TuningAssignment::mainTuning(tid)});
    AutomationLane lane;
    lane.id = ids.lane();
    lane.param = ParamRef::tuning(ParamKind::MosGenerator, tid);
    lane.points = {{0, 700.0f, 0.0f, ids.breakpoint()}, {kTicksPerWhole, 715.0f, 0.0f, ids.breakpoint()}};
    b.commands.push_back(AddAutomationLane{r.track, std::make_shared<const AutomationLane>(lane)});
    const Status st = r.store.dispatch(b);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  const int32_t degree = 2;
  const JiObjectId id = r.create(0, 6 * kQ, {3}, PitchId::fromDegree(degree));
  (void)r.stop(id, 3 * kQ, {4}, kQ);
  const JiObject o = r.object(id);
  const tuning::Tuning t = r.tuning();  // as defined (generator 700)
  const double fund0 = *jiFundamentalHz(o, t, 0.0);
  auto degreeHz = [&](double seconds) {
    pq::tuning::MosParams m = mos;
    m.generator = 700.0 + 15.0 * std::clamp(seconds / 2.0, 0.0, 1.0);
    auto built = pq::tuning::Mos::make(m);
    REQUIRE(built.ok());
    return kRef * std::exp2(built.value().centsOf(degree, 0) / 1200.0);
  };
  REQUIRE(degreeHz(0.0) == doctest::Approx(fund0).epsilon(1e-9));
  REQUIRE(std::fabs(cents(degreeHz(2.0) / fund0 - 1.0)) > 5.0);  // the degree really moves with the generator
  auto link = makeLink(r.store.current());
  const std::vector<float> out = render(*link, 2.9);
  // The voice keeps its ratio to the fundamental (3, then the morph to 4) while the fundamental follows the node.
  auto want = [&](double tick) {
    return *jiVoiceHz(o, t, 0, tick) / fund0 * degreeHz(tick / kTicksPerSample / kRate);
  };
  const double K = static_cast<double>(out[at(2.5)]) / (4.0 * degreeHz(2.0));
  const double worst = worstRel(out, kAttack, at(2.9), K, want, 0.0, 7);
  CHECK(cents(worst) < kGlideCents);  // the automation is rendered per block (as the tuning-automation golden's tolerance)
  CHECK(worstRel(out, at(2.0) + 64, at(2.9), K, [&](double) { return 4.0 * degreeHz(2.0); }) < 1e-5);
  MESSAGE("object on a modulated 5L2s: worst " << cents(worst) << " c");
}

TEST_CASE("ji playback: interpreter == JIT; an object is the same sound at any device buffer (sine)") {
  Rig r(false);
  const JiObjectId id = r.create(0, 6 * kQ, {4, 5, 6});
  (void)r.stop(id, 2 * kQ, {2, 3}, kQ);
  (void)r.stop(id, 4 * kQ, {4, 5, 6}, kQ);
  auto interp = makeLink(r.store.current());
  const std::vector<float> a = render(*interp, 3.2);
  float peak = 0.0f;
  for (const float x : a) peak = std::max(peak, std::fabs(x));
  CHECK(peak > 0.1f);
  auto jit = makeLink(r.store.current(), true);
  REQUIRE(jit->waitForJit());
  CHECK(jit->jitStats().published >= 1);
  const std::vector<float> b = render(*jit, 3.2);
  REQUIRE(a.size() == b.size());
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

TEST_CASE("ji playback: editing a stop while the object plays keeps its voices (retuned and re-synchronised, never cut)") {
  Rig r(false);
  const JiObjectId id = r.create(0, 8 * kQ, {4, 5, 6});
  (void)r.stop(id, 4 * kQ, {2, 3}, kQ);
  auto link = makeLink(r.store.current());
  [[maybe_unused]] const ListenerId sub = r.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(300);  // 0.4 s: the three voices sound
  const engine::Timeline& tl = link->engine().timeline();
  REQUIRE(tl.stats().noteOns == 3);
  const engine::TimelineStats before = tl.stats();
  const app::LinkStats ls = link->stats();
  auto maxStep = [&](size_t from, size_t to) {
    float m = 0.0f;
    for (size_t i = std::max<size_t>(from, 1); i < to; ++i) m = std::max(m, std::fabs(d.out[i] - d.out[i - 1]));
    return m;
  };
  const float usual = maxStep(at(0.2), d.out.size());
  {  // a later stop: only the curves change
    JiObject o = r.object(id);
    o.stops[1].terms = {4, 5, 7};
    o.stops[1].at = 2 * kQ;
    o.stops[1].timing.glide = kQ / 2;
    r.set(o);
    link->flush();
  }
  size_t mark = d.out.size();
  d.run(40);
  CHECK(link->stats().compiles == ls.compiles);  // a Sequence change: no compile
  CHECK(link->stats().sequencePublishes == ls.sequencePublishes + 1);
  CHECK(tl.stats().stoppedOnSwap == before.stoppedOnSwap);
  CHECK(tl.stats().keptOnSwap == before.keptOnSwap + 3);
  CHECK(tl.stats().noteOns == 3);
  CHECK(tl.stats().noteOffs == before.noteOffs);
  CHECK(tl.stats().curveResyncs > before.curveResyncs);
  CHECK(maxStep(mark, d.out.size()) <= usual * 1.05f);  // no click: the sine goes on
  {  // the stop the voices are holding: voice 2 is retuned (6 -> 7) where it sounds
    JiObject o = r.object(id);
    o.stops[0].terms = {4, 5, 7};
    r.set(o);
    link->flush();
  }
  mark = d.out.size();
  d.run(40);
  CHECK(tl.stats().stoppedOnSwap == before.stoppedOnSwap);
  CHECK(tl.stats().keptOnSwap == before.keptOnSwap + 6);
  CHECK(tl.stats().retunedOnSwap == before.retunedOnSwap + 1);
  CHECK(tl.stats().noteOns == 3);
  CHECK(tl.stats().noteOffs == before.noteOffs);
  CHECK(tl.soundingNotes(0) == 3);
  // A retune is a pitch step with continuous phase: the waveform does not jump (7/6 the slope at most).
  CHECK(maxStep(mark, d.out.size()) <= usual * 1.25f);
  // The voices then play the edited object to its end.
  d.run(static_cast<uint32_t>((at(4.2) - d.out.size()) / kBlock));
  CHECK(tl.stats().noteOffs == before.noteOffs + 3);
  CHECK(tl.stats().droppedCurveEvents == 0);
}

TEST_CASE("ji playback: a locate into the middle of an object chases its voices at the plan's pitch and gain (Q278, probe)") {
  Rig r(true);
  const JiObjectId id = r.create(kQ, 8 * kQ, {4, 5, 6});
  (void)r.stop(id, 3 * kQ, {2, 3}, kQ);
  const ObjectSum want(r.object(id), r.tuning());
  double K = 0.0;
  {
    auto link = makeLink(r.store.current());
    const std::vector<float> out = render(*link, 1.0);
    K = static_cast<double>(out[at(0.75)]) / (15.0 * kRef);
  }
  REQUIRE(K > 0.0);
  // Into the glide (object tick 2.5 q), into the hold after it, and (playing) back into the first hold.
  for (const Ticks from : {kQ + 5 * kQ / 2, 6 * kQ}) {
    CAPTURE(from);
    auto link = makeLink(r.store.current());
    Driver d{*link};
    link->play(from);
    d.run(200);
    CHECK(tstats(*link).chasedNotes == 3);
    CHECK(tstats(*link).noteOns == 3);
    const double worst = worstRel(d.out, kAttack, d.out.size(), K, want, static_cast<double>(from));
    CHECK(worst < 1.5e-3);
    CHECK(d.out[4] != 0.0f);  // from the first samples
    link->locate(2 * kQ);     // while playing: released and chased again, in the first hold
    const size_t mark = d.out.size();
    d.run(100);
    CHECK(tstats(*link).chasedNotes == 6);
    const std::vector<float> tail(d.out.begin() + static_cast<std::ptrdiff_t>(mark), d.out.end());
    CHECK(worstRel(tail, kAttack, tail.size(), K, [](double) { return 15.0 * kRef; }) < 2e-6);
  }
}

TEST_CASE("ji playback: a 64-voice object with a vibrato ride and fast morphs stays inside the track's event list" *
          doctest::test_suite("slow")) {
  Rig r(true);
  test_blocks::dispatchEdit(r.store, [&] {
    GroupPolyphony gp;
    gp.voices = 64;
    return ops::setGroupPolyphony(r.doc(), r.group, gp);
  }());
  std::vector<int64_t> a, b;
  for (int64_t k = 0; k < 64; ++k) {
    a.push_back(16 + k);
    b.push_back(17 + k);
  }
  const JiObjectId id = r.create(0, 8 * kQ, a, PitchId::fromCents(-4800.0));  // the fundamental: 16.35 Hz
  (void)r.stop(id, 2 * kQ, b, kQ / 16);           // every voice a step up in 31 ms
  (void)r.stop(id, 3 * kQ, a, kQ / 2);
  (void)r.stop(id, 5 * kQ, {16}, kQ / 8);         // all 64 merge on one pitch: 63 are parked at one tick
  (void)r.stop(id, 6 * kQ, a, kQ / 8);            // … and split again
  {
    JiObject o = r.object(id);
    for (Ticks t = 0; t <= 8 * kQ; t += kQ / 8)  // a 4 Hz vibrato, ±25 c, curved segments
      o.ride.push_back({t, (t / (kQ / 8)) % 2 == 0 ? -25.0f : 25.0f, (t / (kQ / 8)) % 2 == 0 ? 0.7f : -0.7f, {}});
    r.set(o);
  }
  const ObjectSum want(r.object(id), r.tuning());
  auto link = makeLink(r.store.current());
  Driver d{*link};
  link->play(0);
  const engine::Timeline& tl = link->engine().timeline();
  uint64_t last = 0, worstBlock = 0;
  for (uint32_t blk = 0; blk < at(4.1) / kBlock; ++blk) {
    d.run(1);
    worstBlock = std::max<uint64_t>(worstBlock, tl.events(0).count);
    last = tl.stats().curveEvents;
  }
  const engine::TimelineStats& ts = tl.stats();
  CHECK(ts.noteOns == 64);
  CHECK(ts.noteOffs == 64);
  CHECK(ts.droppedNoteOns == 0);
  CHECK(ts.droppedCurveEvents == 0);
  CHECK(ts.curveCollapses == 0);
  CHECK(worstBlock <= 256);
  CHECK(worstBlock >= 128);  // the limit is really approached: 64 note-ons with their ramps in one block
  // … and what is played is still the object: Σ gain · hz on every 5th sample.
  const double K = static_cast<double>(d.out[at(2.4)]) / want(2.4 * kRate * kTicksPerSample);
  const double worst = worstRel(d.out, kAttack, at(3.99), K, want, 0.0, 5);
  CHECK(worst < 3e-3);
  MESSAGE("64 voices: " << last << " curve events in 4 s, at most " << worstBlock << " events in one 64-frame block (limit 256); "
                        << "worst relative error " << worst);
}

TEST_CASE("ji playback: lowering 100 objects x 8 voices x 32 stops (CPU)") {
  Rig r(true, {}, 100 * 16 * kQ);
  std::vector<int64_t> a, b;
  for (int64_t k = 0; k < 8; ++k) {
    a.push_back(8 + k);
    b.push_back(9 + k);
  }
  Batch all{"objects", {}};
  for (int i = 0; i < 100; ++i) {
    auto e = ops::createJiObject(r.doc(), r.store.ids(), r.ref, static_cast<Ticks>(i) * 16 * kQ, 16 * kQ, PitchId::fromDegree(i % 12), a);
    REQUIRE(e.ok());
    JiObject o = std::get<AddJiObject>(e->batch().commands.front().v).object;
    for (int s = 1; s < 32; ++s) {
      JiStop st = o.stops[0];
      st.at = static_cast<Ticks>(s) * kQ / 2;
      st.terms = s % 2 == 1 ? b : a;
      st.timing.glide = kQ / 8;
      o.stops.push_back(st);
    }
    all.commands.push_back(AddJiObject{r.ref, std::move(o)});
  }
  const Status st = r.store.dispatch(all);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  REQUIRE(jiObjectsOf(r.clip()).size() == 100);
  double best = 1e9;
  size_t points = 0;
  for (int round = 0; round < 5; ++round) {
    auto link = makeLink(r.store.current());
    best = std::min(best, link->stats().lowerMs);
    (void)render(*link, 0.01);
    const engine::SequenceImage* img = link->engine().timeline().sequence();
    REQUIRE(img->notes().size() == 800);
    points = img->curves().size();
  }
  MESSAGE("lowering 100 objects x 8 voices x 32 stops: " << best << " ms (best of 5), " << points << " curve points");
#if PQ_OPTIMIZED
  CHECK(best < 25.0);  // the target is < 5 ms in release on an idle machine; the bound here survives a loaded one
#endif
}
