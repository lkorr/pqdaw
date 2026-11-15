// T7.3 / T7.13 (docs/plans/M7.md "Rework: notes-first JI"; Q278): note curves, bends and JI groups play.
// ProjectLink::lowerTrack gives every note its effective curve — its free curve, its bends and its group's warps
// (src/app/src/project_link_ji.h) — and the notes of a JI group their harmonic numbers; the timeline plays the curves
// (T7.1) and chases notes on a seek. Two instruments:
//   the probe    an instrument group whose Out is voice.hz (tests/support/blocks.h): the rendered track is
//                K · Σ hz_v per sample, so pitch curves are checked on every sample against the document's own
//                evaluation (doc::noteCurveCents on what pitch_resolve resolves);
//   Basic Synth  the default (sine) instrument group, for what is about audio: interpreter == JIT, edits while
//                playing.
// 120 BPM: a quarter note is 0.5 s = 24000 samples. A probe voice fades in over its first milliseconds (the voice
// engine's de-click), so comparisons start kAttack samples after a note-on.
//
// Deleted with the JI object (T7.13): the per-voice plan / gain tests (merges, unison lanes, Syncs: the engine's
// Gain / Sync events stay tested in tests/engine), ride curves, undertone objects, scaffolds with inner Harmonic
// notes, the object under a modulated MOS generator. Rewritten for groups: Tune to JI (now a warp), interpreter ==
// JIT, an edit while playing, the chase, the event-list bound, the lowering's CPU.
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
#include "pq/document/note_bend.h"
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
  JiGroupId edit(const ops::EditResult& e) {
    test_blocks::dispatchEdit(store, e);
    return e->jiGroups.empty() ? JiGroupId{} : e->jiGroups.front();
  }
  // The 12-EDO chord `degrees` over [start, start + length), tuned to JI (ops::tuneToJi; `proportion` empty: the best
  // candidate). Returns the group.
  JiGroupId tuned(std::vector<int32_t> degrees, Ticks start, Ticks length, std::vector<int64_t> proportion = {}) {
    std::vector<Note> notes;
    for (const int32_t d : degrees) notes.push_back(playback_fixture::note(store.ids(), start, length, d));
    auto added = ops::addNotes(doc(), store.ids(), ref, notes);
    test_blocks::dispatchEdit(store, added);
    ops::TuneToJiOptions o;
    o.proportion = std::move(proportion);
    return edit(ops::tuneToJi(doc(), store.ids(), ref, added->notes, o));
  }
  void warps(JiGroupId g, std::vector<JiWarp> w) { (void)edit(ops::setJiWarps(doc(), ref, g, std::move(w))); }
};

// What the probe plays for a clip: Σ over the notes sounding at content tick `tick` of the note's resolved Hz times
// its effective curve there — the document's own evaluation (pitch_resolve + note_bend.h), not the lowering's.
struct ClipSum {
  ClipPtr clip;
  ClipPitches pitches;
  NotePitchIndex index;
  ClipSum(const Rig& r)
      : clip(findTrackPtr(r.doc(), r.track)->clips.front().clip),
        pitches(resolveClipPitches(r.doc(), *findTrack(r.doc(), r.track), *clip)),
        index(pitches) {}
  double operator()(double tick) const {
    double s = 0.0;
    for (const Note& n : clip->notes) {
      const double rel = tick - static_cast<double>(n.start);
      if (rel < 0.0 || rel >= static_cast<double>(n.length)) continue;
      const std::optional<double> hz = index.hz(n.id);
      if (!hz) continue;
      const NoteCurves* nc = findNoteCurves(*clip, n.id);
      const NoteBendTargets targets = nc ? noteBendTargets(*nc, index) : NoteBendTargets{};
      s += *hz * std::exp2(noteCurveCents(nc, n.length, targets, index.warp(*clip, n.id), rel) / 1200.0);
    }
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

TEST_CASE("ji playback: a note's own curve and a derived bend (probe)") {
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
  SUBCASE("a derived bend plays the glide from the neighbour's pitch, and follows the neighbour when it moves (T7.11a)") {
    Rig r(true, {});
    const Note a = playback_fixture::note(r.store.ids(), 0, kQ, 0);
    const Note b = playback_fixture::note(r.store.ids(), 2 * kQ, 4 * kQ, 7);
    auto added = ops::addNotes(r.doc(), r.store.ids(), r.ref, std::vector<Note>{a, b});
    test_blocks::dispatchEdit(r.store, added);
    const NoteKey ka = added->notes[0], kb = added->notes[1];
    test_blocks::dispatchEdit(r.store, ops::setNoteBend(r.doc(), r.ref, kb, ops::NoteEdge::Start, kQ, GlideShape{}, ka.id));
    const NoteCurves record = *findNoteCurves(r.clip(), kb.id);
    const double hz = kRef * std::exp2(7.0 / 12.0);
    for (const double from : {-700.0, -500.0}) {  // the neighbour a fifth below, then moved up two steps
      auto link = makeLink(r.store.current());
      const std::vector<float> out = render(*link, 2.6);
      NoteBendTargets targets;
      targets.in = from;
      auto want = [&](double tick) {
        const double rel = tick - static_cast<double>(2 * kQ);
        if (rel < 0.0 || rel >= 4.0 * kQ) return 0.0;
        return hz * std::exp2(noteCurveCents(record, 4 * kQ, targets, rel) / 1200.0);
      };
      const double K = static_cast<double>(out[at(2.0)]) / hz;  // after the glide
      const double worst = worstRel(out, at(1.0) + kAttack, at(2.5), K, want);
      CHECK(cents(worst) < kGlideCents);
      CHECK(worstRel(out, at(1.5) + 2, at(2.5), K, [&](double) { return hz; }) < 2e-6);  // arrived: its own pitch, exactly
      // It does start a long way below (the glide is there at all): a tenth into it, still more than 300 c under.
      CHECK(static_cast<double>(out[at(1.05)]) / K < hz * std::exp2(-300.0 / 1200.0));
      MESSAGE("bend from " << from << " c: worst " << cents(worst) << " c");
      if (from == -700.0) {
        const NoteKey keys[] = {ka};
        test_blocks::dispatchEdit(r.store, ops::transposeNotes(r.doc(), r.ref, keys, 2, ops::StepUnit::Grid));
        CHECK(*findNoteCurves(r.clip(), kb.id) == record);  // nothing stored changed
      }
    }
  }
}

TEST_CASE("ji playback: a triad written in 12-EDO with a toJi warp is tempered before it, glides, holds exact 4:5:6 after it, and goes back with a second warp (probe)") {
  Rig r(true);
  const JiGroupId g = r.tuned({0, 4, 7}, 0, 8 * kQ);
  r.warps(g, {JiWarp{2 * kQ, kQ, GlideShape{}, true}, JiWarp{5 * kQ, kQ, GlideShape{GlideShape::LogLinear}, false}});
  // A member drawn after the first warp starts on its JI pitch (no curve until the second warp), the anchor never moves.
  (void)r.edit(ops::addGroupNote(r.doc(), r.store.ids(), r.ref, g, ops::GroupNotePitch{{}, 7}, 12 * kQ, kQ));
  const ClipSum want(r);
  const double edo = kRef * (1.0 + std::exp2(4.0 / 12.0) + std::exp2(7.0 / 12.0));
  const double ji = kRef * (1.0 + 5.0 / 4.0 + 3.0 / 2.0);
  CHECK(want(0.0) == doctest::Approx(edo).epsilon(1e-15));
  CHECK(want(4.0 * kQ) == doctest::Approx(ji).epsilon(1e-12));
  CHECK(want(7.0 * kQ) == doctest::Approx(edo).epsilon(1e-15));
  auto link = makeLink(r.store.current());
  const std::vector<float> out = render(*link, 6.6);
  CHECK(tstats(*link).noteOns == 4);
  const double K = static_cast<double>(out[at(2.0)]) / ji;  // the JI hold [3 q, 5 q]
  // Before the warp: the chord as written (the major third 13.7 c above 5/4), exactly.
  CHECK(worstRel(out, kAttack, at(1.0), K, [&](double) { return edo; }) < 2e-6);
  CHECK(edo > ji * 1.001);
  // After the glide: exact 4:5:6 on the unmoved lowest note.
  const double landed = worstRel(out, at(1.5) + 2, at(2.5), K, [&](double) { return ji; });
  CHECK(landed < 2e-6);
  // After the second warp: the written chord again.
  CHECK(worstRel(out, at(3.0) + 2, at(4.0), K, [&](double) { return edo; }) < 2e-6);
  // Every sample, through both glides.
  const double worst = worstRel(out, kAttack, at(4.0), K, want);
  CHECK(cents(worst) < kGlideCents);
  // The late member: 7/4 over the root from its first sample on, with no curve.
  CHECK(worstRel(out, at(6.0) + kAttack, at(6.5), K, [&](double) { return kRef * 1.75; }) < 2e-6);
  {
    const engine::SequenceImage* img = link->engine().timeline().sequence();
    REQUIRE(img != nullptr);
    REQUIRE(img->notes().size() == 4);
    // The notes of a group carry voice.harmonic (their number) and voice.ratio (to the anchor).
    std::vector<std::pair<float, float>> hr;
    for (const engine::SequenceImage::Note& n : img->notes()) hr.emplace_back(n.harmonic, n.ratio);
    std::sort(hr.begin(), hr.end());
    CHECK(hr == std::vector<std::pair<float, float>>{{4.0f, 1.0f}, {5.0f, 1.25f}, {6.0f, 1.5f}, {7.0f, 1.75f}});
  }
  MESSAGE("warp: landed within " << landed << " (relative), glides within " << cents(worst) << " c");
}

TEST_CASE("ji playback: interpreter == JIT on a warped group (sine)") {
  Rig r(false);
  const JiGroupId g = r.tuned({0, 4, 7}, 0, 6 * kQ);
  r.warps(g, {JiWarp{kQ, kQ, GlideShape{}, true}, JiWarp{4 * kQ, kQ / 2, GlideShape{}, false}});
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

TEST_CASE("ji playback: editing a group while it plays keeps its voices (curves re-synchronised, members retuned, never cut)") {
  Rig r(false);
  const JiGroupId g = r.tuned({0, 4, 7}, 0, 8 * kQ);
  r.warps(g, {JiWarp{4 * kQ, kQ, GlideShape{}, true}});
  auto link = makeLink(r.store.current());
  [[maybe_unused]] const ListenerId sub = r.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(300);  // 0.4 s: the three voices sound, as written
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
  // The warp moves earlier and gets shorter: only the curves change.
  r.warps(g, {JiWarp{2 * kQ, kQ / 2, GlideShape{}, true}});
  link->flush();
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
  // The warps go: the two members are JI from their start — retuned where they sound.
  r.warps(g, {});
  link->flush();
  mark = d.out.size();
  d.run(40);
  CHECK(tl.stats().stoppedOnSwap == before.stoppedOnSwap);
  CHECK(tl.stats().keptOnSwap == before.keptOnSwap + 6);
  CHECK(tl.stats().retunedOnSwap == before.retunedOnSwap + 2);
  CHECK(tl.stats().noteOns == 3);
  CHECK(tl.stats().noteOffs == before.noteOffs);
  CHECK(tl.soundingNotes(0) == 3);
  CHECK(maxStep(mark, d.out.size()) <= usual * 1.25f);  // a retune is a pitch step with continuous phase
  // The voices then play the edited group to its end.
  d.run(static_cast<uint32_t>((at(4.2) - d.out.size()) / kBlock));
  CHECK(tl.stats().noteOffs == before.noteOffs + 3);
  CHECK(tl.stats().droppedCurveEvents == 0);
}

TEST_CASE("ji playback: a locate into the middle of a warped group chases its notes where the warp has them (Q278, probe)") {
  Rig r(true);
  const JiGroupId g = r.tuned({0, 4, 7}, kQ, 8 * kQ);
  r.warps(g, {JiWarp{2 * kQ, kQ, GlideShape{}, true}});  // group ticks: the glide is [3 q, 4 q] of the clip
  const ClipSum want(r);
  const double ji = kRef * (1.0 + 5.0 / 4.0 + 3.0 / 2.0), edo = kRef * (1.0 + std::exp2(4.0 / 12.0) + std::exp2(7.0 / 12.0));
  double K = 0.0;
  {
    auto link = makeLink(r.store.current());
    const std::vector<float> out = render(*link, 1.0);
    K = static_cast<double>(out[at(0.75)]) / edo;
  }
  REQUIRE(K > 0.0);
  // Into the glide, into the JI hold after it, and (playing) back into the written chord.
  for (const Ticks from : {3 * kQ + kQ / 2, 6 * kQ}) {
    CAPTURE(from);
    auto link = makeLink(r.store.current());
    Driver d{*link};
    link->play(from);
    d.run(200);
    CHECK(tstats(*link).chasedNotes == 3);
    CHECK(tstats(*link).noteOns == 3);
    const double worst = worstRel(d.out, kAttack, d.out.size(), K, want, static_cast<double>(from));
    CHECK(worst < 1.5e-3);
    if (from == 6 * kQ) CHECK(worstRel(d.out, kAttack, d.out.size(), K, [&](double) { return ji; }) < 2e-6);
    CHECK(d.out[4] != 0.0f);  // from the first samples
    link->locate(2 * kQ);     // while playing: released and chased again, before the warp
    const size_t mark = d.out.size();
    d.run(100);
    CHECK(tstats(*link).chasedNotes == 6);
    const std::vector<float> tail(d.out.begin() + static_cast<std::ptrdiff_t>(mark), d.out.end());
    CHECK(worstRel(tail, kAttack, tail.size(), K, [&](double) { return edo; }) < 2e-6);
  }
}

TEST_CASE("ji playback: a 64-note group warped back and forth every eighth stays inside the track's event list" *
          doctest::test_suite("slow")) {
  Rig r(true);
  test_blocks::dispatchEdit(r.store, [&] {
    GroupPolyphony gp;
    gp.voices = 64;
    return ops::setGroupPolyphony(r.doc(), r.group, gp);
  }());
  std::vector<int64_t> terms;
  for (int64_t k = 0; k < 64; ++k) terms.push_back(16 + k);
  auto made = ops::proportionNotes(r.doc(), r.store.ids(), r.ref, 0, 8 * kQ, PitchId::fromCents(-4800.0), terms);  // from 16.35 Hz
  const JiGroupId g = r.edit(made);
  {  // every member written 30 cents away from its harmonic, alternately above and below
    std::vector<NoteWritten> written;
    for (size_t i = 1; i < made->notes.size(); ++i)
      written.push_back(NoteWritten{made->notes[i].id, PitchId::fromCents(-4800.0 + 1200.0 * std::log2(static_cast<double>(terms[i]) / 16.0) + (i % 2 ? 30.0 : -30.0))});
    REQUIRE(r.store.dispatch(SetNotesWritten{r.ref, written}).ok());
    std::vector<JiWarp> w;
    for (int s = 0; s < 16; ++s) w.push_back(JiWarp{static_cast<Ticks>(s) * kQ / 2, kQ / 16, GlideShape{}, s % 2 == 0});  // 31 ms glides
    r.warps(g, w);
  }
  const ClipSum want(r);
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
  CHECK(worstBlock >= 126);  // the limit is really approached: 64 note-ons, 63 of them with a ramp, in one block
  // … and what is played is still the group: Σ hz on every 5th sample.
  const double K = static_cast<double>(d.out[at(2.4)]) / want(2.4 * kRate * kTicksPerSample);
  const double worst = worstRel(d.out, kAttack, at(3.99), K, want, 0.0, 5);
  CHECK(worst < 3e-3);
  MESSAGE("64 notes: " << last << " curve events in 4 s, at most " << worstBlock << " events in one 64-frame block (limit 256); "
                       << "worst relative error " << worst);
}

// ---- T7.14: vibrato as a note property (plan M7 V1–V3) ---------------------------------------------------------------

namespace {

NoteVibrato vibrato(float depth, float rate, Ticks onset = 0, Ticks fadeIn = 0, Ticks end = 0, Ticks fadeOut = 0) {
  NoteVibrato v;
  v.depthCents = depth;
  v.rate = rate;
  v.onset = onset;
  v.fadeIn = fadeIn;
  v.end = end;
  v.fadeOut = fadeOut;
  return v;
}

}  // namespace

TEST_CASE("ji playback: a vibrato with fades and a warped chord SOUND as they are drawn — every sample within 2 c of the note's effective curve (probe)") {
  Rig r(true);
  // A long note: 35 c at 5.5 Hz from a beat in, fading in over a beat and a half, fading out over a beat before 5 q.
  const Note n = playback_fixture::note(r.store.ids(), 0, 6 * kQ, 12);
  auto added = ops::addNotes(r.doc(), r.store.ids(), r.ref, std::vector<Note>{n});
  test_blocks::dispatchEdit(r.store, added);
  const NoteVibrato v = vibrato(35.0f, 5.5f, kQ, 3 * kQ / 2, 5 * kQ, kQ);
  const ops::NoteVibratoEdit edits[] = {{added->notes.front(), v}};
  test_blocks::dispatchEdit(r.store, ops::setNoteVibrato(r.doc(), r.ref, edits));
  CHECK(findNoteCurves(r.clip(), added->notes.front().id)->pitch.empty());  // nothing baked: the lowering reads the property
  // Then a seventh chord written in 12-EDO that warps into JI and back.
  const JiGroupId g = r.tuned({0, 4, 7, 10}, 8 * kQ, 8 * kQ);
  r.warps(g, {JiWarp{2 * kQ, kQ, GlideShape{}, true}, JiWarp{5 * kQ, kQ, GlideShape{}, false}});
  const ClipSum want(r);  // the document's evaluation: what the roll draws (RollState::pathOf uses the same function)
  auto link = makeLink(r.store.current());
  const std::vector<float> out = render(*link, 8.1);
  const double hz = kRef * 2.0;
  const double K = static_cast<double>(out[at(0.25)]) / hz;  // before the onset: the note's own pitch
  CHECK(worstRel(out, kAttack, at(0.5) - 2, K, [&](double) { return hz; }) < 2e-6);  // steady until the onset
  const double vib = worstRel(out, kAttack, at(3.0) - 2, K, want);
  CHECK(cents(vib) < 2.0);
  // It really swings: ± 35 c between the fades, about half of that half way through the fade-in, nothing after its end.
  auto swing = [&](size_t from, size_t to) {
    double lo = 1e9, hi = -1e9;
    for (size_t i = from; i < to; ++i) {
      const double c = 1200.0 * std::log2(static_cast<double>(out[i]) / K / hz);
      lo = std::min(lo, c);
      hi = std::max(hi, c);
    }
    return std::pair<double, double>{lo, hi};
  };
  const auto full = swing(at(1.3), at(1.95));
  CHECK(full.first == doctest::Approx(-35.0).epsilon(0.03));
  CHECK(full.second == doctest::Approx(35.0).epsilon(0.03));
  const auto early = swing(at(0.5), at(0.875));  // the first half of the fade-in
  CHECK(early.second < 19.0);
  CHECK(early.second > 8.0);
  CHECK(worstRel(out, at(2.5) + 2, at(3.0) - 2, K, [&](double) { return hz; }) < 2e-6);  // after the end: steady again
  // The chord: tempered, gliding into 4:5:6:7, back.
  const double chord = worstRel(out, at(4.0) + kAttack, at(8.0) - 2, K, want);
  CHECK(cents(chord) < 2.0);
  const double ji = kRef * (1.0 + 5.0 / 4.0 + 3.0 / 2.0 + 7.0 / 4.0);
  CHECK(worstRel(out, at(5.5) + 2, at(6.5) - 2, K, [&](double) { return ji; }) < 2e-6);
  CHECK(tstats(*link).droppedCurveEvents == 0);
  MESSAGE("vibrato within " << cents(vib) << " c, warped chord within " << cents(chord) << " c of the effective curve");
}

TEST_CASE("ji playback: interpreter == JIT on a vibrato with fades over a warped chord (sine)") {
  Rig r(false);
  const JiGroupId g = r.tuned({0, 4, 7}, 0, 6 * kQ);
  r.warps(g, {JiWarp{kQ, kQ, GlideShape{}, true}, JiWarp{4 * kQ, kQ / 2, GlideShape{}, false}});
  const Note n = playback_fixture::note(r.store.ids(), kQ / 2, 5 * kQ, 12);
  auto added = ops::addNotes(r.doc(), r.store.ids(), r.ref, std::vector<Note>{n});
  test_blocks::dispatchEdit(r.store, added);
  const ops::NoteVibratoEdit edits[] = {{added->notes.front(), vibrato(40.0f, 6.0f, kQ / 2, kQ, 4 * kQ, kQ)}};
  test_blocks::dispatchEdit(r.store, ops::setNoteVibrato(r.doc(), r.ref, edits));
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

TEST_CASE("ji playback: 64 notes vibrating at once stay inside the track's event list — a usual vibrato exactly, the fastest and deepest allowed without a dropped event" *
          doctest::test_suite("slow")) {
  for (const bool extreme : {false, true}) {
    CAPTURE(extreme);
    Rig r(true);
    test_blocks::dispatchEdit(r.store, [&] {
      GroupPolyphony gp;
      gp.voices = 64;
      return ops::setGroupPolyphony(r.doc(), r.group, gp);
    }());
    std::vector<int64_t> terms;
    for (int64_t k = 0; k < 64; ++k) terms.push_back(16 + k);
    auto made = ops::proportionNotes(r.doc(), r.store.ids(), r.ref, 0, 8 * kQ, PitchId::fromCents(-4800.0), terms);
    (void)r.edit(made);
    std::vector<ops::NoteVibratoEdit> edits;
    for (size_t i = 0; i < made->notes.size(); ++i)
      edits.push_back({made->notes[i], extreme ? vibrato(kMaxVibratoCents, kMaxVibratoHz)
                                               : vibrato(35.0f + static_cast<float>(i % 7), 5.0f + 0.05f * static_cast<float>(i), 0, kQ, 0, kQ)});
    test_blocks::dispatchEdit(r.store, ops::setNoteVibrato(r.doc(), r.ref, edits));
    const ClipSum want(r);
    auto link = makeLink(r.store.current());
    Driver d{*link};
    link->play(0);
    const engine::Timeline& tl = link->engine().timeline();
    uint64_t worstBlock = 0, sumEvents = 0, blocks = 0;
    for (uint32_t blk = 0; blk < at(4.1) / kBlock; ++blk) {
      d.run(1);
      worstBlock = std::max<uint64_t>(worstBlock, tl.events(0).count);
      sumEvents += tl.events(0).count;
      ++blocks;
    }
    const engine::TimelineStats& ts = tl.stats();
    CHECK(ts.noteOns == 64);
    CHECK(ts.noteOffs == 64);
    CHECK(ts.droppedNoteOns == 0);
    CHECK(ts.droppedCurveEvents == 0);
    CHECK(ts.curveCollapses == 0);
    CHECK(worstBlock <= 256);
    if (!extreme) {
      // … and what plays is the 64 waves: Σ hz on every 5th sample.
      const double K = static_cast<double>(d.out[at(2.4)]) / want(2.4 * kRate * kTicksPerSample);
      const double worst = worstRel(d.out, kAttack, at(3.99), K, want, 0.0, 5);
      CHECK(worst < 3e-3);
      MESSAGE("64 vibratos (35-41 c, 5-8.2 Hz): at most " << worstBlock << " events in one 64-frame block (limit 256), "
                                                            << static_cast<double>(sumEvents) / static_cast<double>(blocks) << " on average; worst relative error " << worst);
    } else {
      MESSAGE("64 vibratos (1200 c, 30 Hz): at most " << worstBlock << " events in one 64-frame block (limit 256), "
                                                       << static_cast<double>(sumEvents) / static_cast<double>(blocks) << " on average");
    }
  }
}

TEST_CASE("ji playback: lowering 100 groups x 8 notes x 32 warps (CPU)") {
  Rig r(true, {}, 100 * 16 * kQ);
  IdSource& ids = r.store.ids();
  const int32_t written[8] = {0, 2, 4, 5, 7, 8, 10, 11};  // the nearest 12-EDO degrees of 8:9:…:15
  AddNotes add{r.ref, {}};
  SetJiGroups groups{r.ref, {}};
  SetNotesWritten was{r.ref, {}};
  for (int i = 0; i < 100; ++i) {
    const NoteId anchor = ids.note();
    for (int k = 0; k < 8; ++k) {
      Note n;
      n.id = k == 0 ? anchor : ids.note();
      n.start = static_cast<Ticks>(i) * 16 * kQ;
      n.length = 16 * kQ;
      n.pitch = k == 0 ? PitchId::fromDegree(i % 12) : PitchId::fromRatio(Rational(8 + k, 8), pq::tuning::AnchorKind::Note, anchor.value);
      if (k > 0) was.written.push_back({n.id, PitchId::fromDegree(i % 12 + written[k])});
      add.notes.push_back(n);
    }
    JiGroup g;
    g.id = ids.jiGroup();
    g.anchor = anchor;
    g.index = 8;
    for (int s = 0; s < 32; ++s) g.warps.push_back(JiWarp{static_cast<Ticks>(s) * kQ / 2, kQ / 8, GlideShape{}, s % 2 == 0});
    groups.groups.push_back(std::move(g));
  }
  Batch all{"groups", {}};
  all.commands.push_back(std::move(add));
  all.commands.push_back(std::move(groups));
  all.commands.push_back(std::move(was));
  const Status st = r.store.dispatch(all);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  REQUIRE(jiGroupsOf(r.clip()).size() == 100);
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
  MESSAGE("lowering 100 groups x 8 notes x 32 warps: " << best << " ms (best of 5), " << points << " curve points");
#if PQ_OPTIMIZED
  CHECK(best < 25.0);  // the bound survives a loaded machine
#endif
}
