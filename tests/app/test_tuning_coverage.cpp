// T2.6-fix2 / T2.6-fix3 / T2.6-fix4 (M3 review H3, H4, H5, M6–M9): tuning-signal coverage in project playback,
// rendered through the real ProjectLink with a probe instrument whose output is voice.hz (tests/app/fixtures/
// probe_stdlib), so the tuning node's pitch can be checked on every sample. The projects under
// tests/app/fixtures/m3_review are the review's probes (docs/reviews/M3/tuning/make_variants.py).
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../engine/tuning_signal_ref.h"
#include "../golden/golden_audio.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/core/time.h"
#include "pq/document/commands.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/engine/automation.h"
#include "pq/engine/offline_render.h"
#include "pq/tuning/mos_regions.h"
#include "pq/tuning/scala.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

namespace fs = std::filesystem;

constexpr double kRate = 48000.0;

fs::path fixture(const std::string& name) { return fs::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / name; }

Snapshot load(const std::string& name) {
  auto l = loadProject(fixture(name).string());
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return l->project;
}

// A link whose instrument plays voice.hz (the probe), or the default instrument.
std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, bool probe = true) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  if (probe) c.stdlib = fixture("probe_stdlib").string();
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

const TuningSlot& tuningOf(const Project& p, uint32_t id) {
  const TuningSlot* t = findTuning(p, TuningId{id});
  REQUIRE(t != nullptr);
  return *t;
}

// The generator an automation lane gives at song sample n (120 BPM, the lane of track 0).
std::vector<engine::AutomationPoint> lanePoints(const Project& p) {
  std::vector<engine::AutomationPoint> pts;
  for (const Breakpoint& b : p.tracks.front()->lanes.front()->points) pts.push_back({b.tick, b.value, b.curve});
  return pts;
}
float laneValueAt(const std::vector<engine::AutomationPoint>& pts, int64_t n) {
  const TempoMap tm;  // 120 BPM, as every fixture here
  return static_cast<float>(engine::evalAutomation(std::span<const engine::AutomationPoint>(pts), tm.samplesToTicks(n, kRate)));
}

}  // namespace

// ---- T2.6-fix2 ------------------------------------------------------------------------------------------------

TEST_CASE("region boundary (H3): a generator automated to 720 cents and held plays the static 720-cent pitch bit for bit") {
  // proj_boundary_auto: 5L2s (hint: the 5L2s region, whose collapsed end is 3\5 = 720 cents) defined at 700 cents,
  // its generator automated 700 → 720 over the first second, then held. proj_boundary_static: the same tuning
  // defined at 720 cents, no automation. A Degree note (degree 1) from 0 to 2.5 s in both. The review measured
  // 261.626 vs 300.529 Hz here (the table took the interval above 720; buildMOSScale nudges toward the hint).
  const Snapshot staticSnap = load("m3_review/proj_boundary_static");
  auto a = makeLink(load("m3_review/proj_boundary_auto"));
  auto s = makeLink(staticSnap);
  const std::vector<float> outA = render(*a, 2.5), outS = render(*s, 2.5);
  size_t equal = 0, total = 0;
  for (size_t i = at(1.05); i < at(2.45); ++i) {
    equal += outA[i] == outS[i];
    ++total;
  }
  MESSAGE(equal << " of " << total << " held samples bit-equal (" << outA[at(2.0)] << " vs " << outS[at(2.0)] << ")");
  CHECK(equal == total);
  // And that pitch is buildMOSScale's at 720 with the 5L2s hint (degree 1 = 240 cents above the reference): the
  // static note's own Hz, passed through by its unmodulated node.
  const TuningSlot& t = tuningOf(*staticSnap, 4);
  const auto hz = tuning::resolveHzDouble(tuning::PitchId::fromDegree(1, 0), buildTuning(t.def).value(), {});
  REQUIRE(hz);
  CHECK(*hz == doctest::Approx(261.626 * std::exp2(240.0 / 1200.0)).epsilon(1e-9));  // (the 1e-7-cent nudge)
  CHECK(s->engine().timeline().sequence()->notes()[0].hz == static_cast<float>(*hz));
}

TEST_CASE("audio-rate tuning automation (M6): the pitch follows the lane per sample and crosses on the exact sample") {
  // tests/app/fixtures/tuning_automation: a 5L2s generator lane 700 → 740 cents over 2 s (crossing 3\5 = 720 cents
  // at 1 s), one Degree note. The lane's buffer drives the node per sample (AutomationLaneDesc::tap), so the probe's
  // output is the model at the lane's value of every sample, and the jump at 720 cents lands on the first sample the
  // explorer puts above it (the review measured a lag of half a 64-frame block: a param read at frame 0).
  const Snapshot snap = load("tuning_automation");
  const Project& p = *snap;
  auto link = makeLink(snap);
  const std::vector<float> out = render(*link, 2.5);
  const tuning::MosParams def = *tuningOf(p, 4).def.mos();
  int32_t degree = 0;
  for (const Note& n : p.tracks.front()->clips.front().clip->notes) degree = n.pitch.degree();
  const auto pts = lanePoints(p);
  auto modelHz = [&](int64_t n) {
    const auto m = tuning_ref::mosAt(def, static_cast<double>(laneValueAt(pts, n)));
    REQUIRE(m);
    return 261.626 * std::exp2(m->centsOf(degree, 0) / 1200.0);
  };
  // The output is voice.hz times the track's settled gains: one constant.
  const double C = out[at(0.5)] / modelHz(static_cast<int64_t>(at(0.5)));
  double worst = 0.0;
  for (size_t i = at(0.1); i < at(2.4); ++i)
    worst = std::max(worst, std::fabs(out[i] / C / modelHz(static_cast<int64_t>(i)) - 1.0));
  MESSAGE("probe output vs the model at the lane's per-sample value: worst relative error " << worst);
  CHECK(worst < 2e-6);  // 0.0035 cents (float32 gains and Hz); a block-rate read is off by up to ~5e-5 here
  // The crossing sample.
  const tuning::MosLinearTable lt(def.n);
  int64_t expected = -1;
  for (int64_t n = static_cast<int64_t>(at(0.9)); n < static_cast<int64_t>(at(1.1)) && expected < 0; ++n)
    if (lt.lookup(laneValueAt(pts, n), def.period).interval != lt.lookup(laneValueAt(pts, n - 1), def.period).interval)
      expected = n;
  int64_t jump = -1;
  for (size_t i = at(0.9); i < at(1.1) && jump < 0; ++i)
    if (std::fabs(1200.0 * std::log2(out[i] / out[i - 1])) > 50.0) jump = static_cast<int64_t>(i);
  MESSAGE("crossing: expected at sample " << expected << ", the output jumps at " << jump);
  CHECK(expected > 0);
  CHECK(jump == expected);
}

TEST_CASE("mode transfer (M7): a live generator edit across 720 cents carries the mode, as a reload of it plays") {
  // A 5L2s mode (rotation 2) defined at 700 cents, a chord of every degree. SetTuningParam moves the generator to
  // 731 cents while it plays: the document carries the mode like the explorer (transferModeRotation), the node does
  // the same from its table (no compile), and a fresh link on the edited document (static pitches) sounds the same.
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId tr = ids.track();
  const TuningId t = ids.tuning();
  Batch b{"mode transfer fixture", {}};
  TuningSlot s{t, "5L2s", Colour{0xAA4422}, {}};
  tuning::MosParams mp;
  mp.n = 7;
  mp.generator = 700.0;
  mp.rotation = 2;
  s.def.system = mp;
  b.commands.push_back(AddTuning{s});
  b.commands.push_back(AddTrack{std::make_shared<const Track>(makeTrack(tr, TrackKind::Midi, "Chord")), {}, kAppend});
  b.commands.push_back(SetTrackTuning{tr, TuningAssignment::mainTuning(t)});
  NoteClip c;
  c.id = ids.clip();
  c.start = 0;
  c.length = 4 * kPpq;
  std::vector<Note> notes;
  for (int32_t d = 0; d < 7; ++d) {
    Note n;
    n.id = ids.note();
    n.start = 0;
    n.length = 4 * kPpq;
    n.pitch = tuning::PitchId::fromDegree(d, 0);
    notes.push_back(n);
  }
  c.notes = NoteSeq::fromNotes(std::move(notes)).value();
  b.commands.push_back(AddClip{tr, std::make_shared<const NoteClip>(std::move(c))});
  REQUIRE(store.dispatch(b).ok());

  auto live = makeLink(store.current());
  [[maybe_unused]] const ListenerId sub = store.subscribe([&](const StoreEvent& e) { live->onDocumentChange(e); });
  const app::LinkStats s0 = live->stats();
  REQUIRE(store.dispatch(SetTuningParam{t, ParamKind::MosGenerator, 731.0}).ok());
  live->flush();
  CHECK(live->stats().compiles == s0.compiles);  // the table (shape, root chain position, hint) is unchanged
  const tuning::MosParams& edited = *findTuning(*store.current(), t)->def.mos();
  CHECK(edited.rotation == tuning::transferModeRotation(700.0, 7, 2, 731.0, 7, 1200.0));
  CHECK(edited.rotation != 2);  // 720 cents changes this mode's rotation
  auto reloaded = makeLink(store.current());
  const std::vector<float> a = render(*live, 1.5), r = render(*reloaded, 1.5);
  double worst = 0.0;
  for (size_t i = at(0.2); i < at(1.4); ++i) worst = std::max(worst, std::fabs(a[i] / r[i] - 1.0));
  MESSAGE("live edit vs reload: worst relative difference " << worst);
  CHECK(worst < 1e-6);
}

// ---- T2.6-fix3 -------------------------------------------------------------------------------------------------

namespace {

// The probe output of a one-note project against a model pitch per sample (both up to one constant: the track's
// settled gains), over [from, to) seconds. Returns the worst relative error.
template <class Model>
double trackError(const std::vector<float>& out, Model&& model, double from, double to, double calibrate) {
  const double C = out[at(calibrate)] / model(static_cast<int64_t>(at(calibrate)));
  double worst = 0.0;
  for (size_t i = at(from); i < at(to); ++i)
    worst = std::max(worst, std::fabs(out[i] / C / model(static_cast<int64_t>(i)) - 1.0));
  return worst;
}

// 5L2s (tuning 4 of the review's projects) at the lane's generator for sample n: degree `degree`'s Hz.
double mosHz(const Project& p, const std::vector<engine::AutomationPoint>& pts, int64_t n, int32_t degree) {
  const tuning::MosParams& def = *tuningOf(p, 4).def.mos();
  const auto m = tuning_ref::mosAt(def, static_cast<double>(laneValueAt(pts, n)));
  REQUIRE(m);
  return tuningOf(p, 4).def.referenceHz * std::exp2(m->centsOf(degree, 0) / 1200.0);
}

}  // namespace

TEST_CASE("note-anchored Ratio (H4b): a 3/2 on a Degree note keeps 702 cents to its anchor while the generator moves") {
  // proj_anchor_ratio: the 5L2s generator automated 700 → 740 cents over 2 s; a 3/2 anchored to a (disabled) Degree
  // note of degree 1. The review measured the 3/2 frozen at 440.498 Hz while its anchor glided: 897 cents apart at
  // 1.05 s. It now follows the anchor's signal: anchor pitch × 3/2 on every sample, the crossing included.
  const Snapshot snap = load("m3_review/proj_anchor_ratio");
  auto link = makeLink(snap);
  const std::vector<float> out = render(*link, 2.5);
  const auto pts = lanePoints(*snap);
  auto model = [&](int64_t n) { return mosHz(*snap, pts, n, 1) * 1.5; };
  const double worst = trackError(out, model, 0.1, 2.4, 0.5);
  // The interval to the anchor, measured: the output over the anchor's model (calibrated at 0.5 s, where both are
  // known), in cents.
  const double C = out[at(0.5)] / model(static_cast<int64_t>(at(0.5)));
  double lo = 1e9, hi = -1e9;
  for (size_t i = at(0.1); i < at(2.4); ++i) {
    const double cents = 1200.0 * std::log2(out[i] / C / mosHz(*snap, pts, static_cast<int64_t>(i), 1));
    lo = std::min(lo, cents);
    hi = std::max(hi, cents);
  }
  MESSAGE("3/2 over its anchor: " << lo << " … " << hi << " cents; worst relative error vs anchor × 3/2 " << worst);
  CHECK(worst < 2e-6);
  CHECK(lo > 701.955 - 0.01);
  CHECK(hi < 701.955 + 0.01);
}

TEST_CASE("clip override (H4a): a clip in another main tuning follows that tuning's automation") {
  // proj_clip_override: the track follows 12-EDO, its clip overrides to 5L2s, whose generator is automated 700 →
  // 740 cents. The review measured the clip's note frozen at 293.665 Hz. The track patch now has a tuning node per
  // signal its clips follow (12-EDO: node 1, 5L2s: node 2) and the note selects node 2 (voice.tuning).
  const Snapshot snap = load("m3_review/proj_clip_override");
  auto link = makeLink(snap);
  const std::vector<float> out = render(*link, 2.5);
  const auto pts = lanePoints(*snap);
  const double worst = trackError(out, [&](int64_t n) { return mosHz(*snap, pts, n, 1); }, 0.1, 2.4, 0.5);
  MESSAGE("clip override vs 5L2s at the lane's generator: worst relative error " << worst);
  CHECK(worst < 2e-6);
  const auto notes = link->engine().timeline().sequence()->notes();
  REQUIRE(notes.size() == 1);
  CHECK(notes[0].pitch.node == 2);
  CHECK(notes[0].pitch.tuning == 4);
}

TEST_CASE("reference automation (H4c): a Cents note follows the reference like the Degree notes") {
  // proj_ref_auto_cents: the reference of 5L2s automated 261.626 → 300 Hz over 2 s, one Cents note (+200 cents).
  // The review measured it frozen at 293.665 Hz (while a static reference edit did move it: Q1 vs Q100.1). It now
  // scales with reference(t) / reference₀; proj_ref_auto_degree (a Degree note) agrees with the same rule.
  for (const char* name : {"m3_review/proj_ref_auto_cents", "m3_review/proj_ref_auto_degree"}) {
    const std::string project = name;
    CAPTURE(project);
    const Snapshot snap = load(name);
    auto link = makeLink(snap);
    const std::vector<float> out = render(*link, 2.5);
    const auto pts = lanePoints(*snap);
    const tuning::Mos m = tuning::Mos::make(*tuningOf(*snap, 4).def.mos()).value();
    const bool cents = std::string(name).ends_with("cents");
    auto model = [&](int64_t n) {
      return static_cast<double>(laneValueAt(pts, n)) * std::exp2((cents ? 200.0 : m.centsOf(1, 0)) / 1200.0);
    };
    const double worst = trackError(out, model, 0.1, 2.4, 0.5);
    MESSAGE("vs reference(t) · the note's ratio to it: worst relative error " << worst);
    CHECK(worst < 2e-6);
    CHECK(out[at(1.9)] / out[at(0.1)] > 1.1);  // it moved
  }
}

namespace {

// A project with one MIDI track in `def` (a main tuning) holding a chord of reference-anchored Ratio notes, a Degree
// note and a Ratio anchored to it, all from 0 for 4 beats.
struct HeldChord {
  DocumentStore store;
  TrackId track;
  TuningId tuning;
  explicit HeldChord(const TuningDef& def) {
    IdSource& ids = store.ids();
    track = ids.track();
    tuning = ids.tuning();
    Batch b{"held chord", {}};
    b.commands.push_back(AddTuning{TuningSlot{tuning, "T", Colour{0x4466AA}, def}});
    b.commands.push_back(AddTrack{std::make_shared<const Track>(makeTrack(track, TrackKind::Midi, "Chord")), {}, kAppend});
    b.commands.push_back(SetTrackTuning{track, TuningAssignment::mainTuning(tuning)});
    NoteClip c;
    c.id = ids.clip();
    c.start = 0;
    c.length = 8 * kPpq;
    std::vector<Note> notes;
    auto add = [&](tuning::PitchId pitch) {
      Note n;
      n.id = ids.note();
      n.start = 0;
      n.length = 4 * kPpq;
      n.pitch = pitch;
      notes.push_back(n);
      return n.id;
    };
    for (const auto& r : {tuning::Rational{1}, tuning::Rational{5, 4}, tuning::Rational{3, 2}, tuning::Rational{7, 4}})
      add(tuning::PitchId::fromRatio(r));
    const NoteId root = add(tuning::PitchId::fromDegree(2, 0));
    add(tuning::PitchId::fromRatio(tuning::Rational{9, 8}, tuning::AnchorKind::Note, root.value));
    c.notes = NoteSeq::fromNotes(std::move(notes)).value();
    b.commands.push_back(AddClip{track, std::make_shared<const NoteClip>(std::move(c))});
    REQUIRE(store.dispatch(b).ok());
  }
};

// Plays the link block by block, dragging the tuning's reference in a gesture from 0.3 s (one SetTuningParam per
// block, as a knob drag does), and returns the timeline's note-offs before the notes end and its retunes.
struct DragResult {
  uint64_t noteOffs = 0, stopped = 0, retuned = 0, lowered = 0, compiles = 0;
  std::vector<float> out;
};
DragResult dragReference(HeldChord& h, ProjectLink& link, double toHz) {
  [[maybe_unused]] const ListenerId sub = h.store.subscribe([&](const StoreEvent& e) { link.onDocumentChange(e); });
  link.play(0);
  constexpr uint32_t kBlock = 64;
  std::vector<float> l(kBlock), r(kBlock);
  DragResult res;
  const app::LinkStats s0 = link.stats();
  engine::TimelineStats t0{};
  const double from = findTuning(*h.store.current(), h.tuning)->def.referenceHz;
  for (int64_t time = 0, b = 0; time < static_cast<int64_t>(at(1.5)); time += kBlock, ++b) {
    if (time == static_cast<int64_t>(at(0.3)) / kBlock * kBlock) {
      REQUIRE(h.store.beginGesture("drag the reference").ok());
      t0 = link.engine().timeline().stats();
    }
    const int64_t d0 = static_cast<int64_t>(at(0.3)) / kBlock, d1 = static_cast<int64_t>(at(1.0)) / kBlock;
    if (b >= d0 && b <= d1) {
      const double u = static_cast<double>(b - d0) / static_cast<double>(d1 - d0);
      REQUIRE(h.store.dispatch(SetTuningParam{h.tuning, ParamKind::TuningReference, from + (toHz - from) * u}).ok());
      link.flush();
      if (b == d1) REQUIRE(h.store.commitGesture().ok());
    }
    float* ch[2] = {l.data(), r.data()};
    {
      RtScope rt;  // the audio thread: retunes, taps and the nodes allocate nothing
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
    }
    res.out.insert(res.out.end(), l.begin(), l.end());
  }
  const engine::TimelineStats t1 = link.engine().timeline().stats();
  res.noteOffs = t1.noteOffs - t0.noteOffs;
  res.stopped = t1.stoppedOnSwap - t0.stoppedOnSwap;
  res.retuned = t1.retunedOnSwap - t0.retunedOnSwap;
  res.lowered = link.stats().trackLowerings - s0.trackLowerings;
  res.compiles = link.stats().compiles - s0.compiles;
  return res;
}

}  // namespace

TEST_CASE("held notes (H5): dragging the reference under a held Ratio chord emits no note-off, and the chord follows") {
  // A 5L2s main tuning (a signal): every note follows the node (the reference-anchored ratios scale with the
  // reference, the Degree note and its 9/8 move with it too), so the drag changes no sequence: nothing is re-lowered
  // (the per-track description cache is keyed by the tables in use, not the tuning list), no note-off, no compile.
  TuningDef mos;
  tuning::MosParams mp;
  mp.generator = 700.0;
  mos.system = mp;
  HeldChord h(mos);
  auto link = makeLink(h.store.current());
  const DragResult r = dragReference(h, *link, 300.0);
  MESSAGE("signal tuning: " << r.noteOffs << " note-offs, " << r.stopped << " stopped, " << r.retuned << " retuned, "
                            << r.lowered << " track lowerings, " << r.compiles << " compiles during the drag");
  CHECK(r.noteOffs == 0);
  CHECK(r.stopped == 0);
  CHECK(r.lowered == 0);
  CHECK(r.compiles == 0);
  // The chord moved by the drag's ratio (the probe sums the six voices: every voice scales by 300 / 261.626).
  CHECK(r.out[at(1.2)] / r.out[at(0.2)] == doctest::Approx(300.0 / tuning::kDefaultReferenceHz).epsilon(1e-5));
}

TEST_CASE("held notes (H5): notes that keep static pitches are retuned by Pitch events when a drag re-resolves them") {
  // A Scala tuning has no signal: its notes resolve statically, so the reference drag re-lowers the track and swaps
  // the sequence with new Hz. The held notes are retuned at the swap (a Pitch event with their new identity), never
  // cut: no note-off before the notes end, and the chord ends at the dragged reference.
  auto scl = tuning::parseScl("! just.scl\njust\n3\n!\n5/4\n3/2\n2/1\n");
  REQUIRE(scl.ok());
  auto def = makeScaleDef(scl.value(), std::nullopt);
  REQUIRE(def.ok());
  HeldChord h(def.value());
  auto link = makeLink(h.store.current());
  const DragResult r = dragReference(h, *link, 300.0);
  MESSAGE("static tuning: " << r.noteOffs << " note-offs, " << r.stopped << " stopped, " << r.retuned << " retuned, "
                            << r.lowered << " track lowerings during the drag");
  CHECK(r.noteOffs == 0);
  CHECK(r.stopped == 0);
  CHECK(r.retuned > 6);  // six voices, several drag steps
  CHECK(r.lowered > 0);
  CHECK(r.out[at(1.2)] / r.out[at(0.2)] == doctest::Approx(300.0 / tuning::kDefaultReferenceHz).epsilon(1e-5));
}

// ---- T2.6-fix4 -------------------------------------------------------------------------------------------------

namespace {

// Into `store`: a main tuning with `def`, a track following it with one Degree note of each degree 0..6 for 2 beats.
// Returns the tuning's id (the same in every fresh store).
TuningId addOneTuning(DocumentStore& store, const TuningDef& def) {
  IdSource& ids = store.ids();
  const TuningId t = ids.tuning();
  const TrackId tr = ids.track();
  Batch b{"one tuning", {}};
  b.commands.push_back(AddTuning{TuningSlot{t, "T", Colour{0x4466AA}, def}});
  b.commands.push_back(AddTrack{std::make_shared<const Track>(makeTrack(tr, TrackKind::Midi, "M")), {}, kAppend});
  b.commands.push_back(SetTrackTuning{tr, TuningAssignment::mainTuning(t)});
  NoteClip c;
  c.id = ids.clip();
  c.start = 0;
  c.length = 4 * kPpq;
  std::vector<Note> notes;
  for (int32_t d = 0; d < 7; ++d) {
    Note n;
    n.id = ids.note();
    n.start = 0;
    n.length = 2 * kPpq;
    n.pitch = tuning::PitchId::fromDegree(d, 0);
    notes.push_back(n);
  }
  c.notes = NoteSeq::fromNotes(std::move(notes)).value();
  b.commands.push_back(AddClip{tr, std::make_shared<const NoteClip>(std::move(c))});
  REQUIRE(store.dispatch(b).ok());
  return t;
}

TuningDef mosDef(int32_t n, double generator, int32_t rotation = 0) {
  TuningDef d;
  tuning::MosParams m;
  m.n = n;
  m.generator = generator;
  m.rotation = rotation;
  d.system = m;
  return d;
}

void runBlocks(ProjectLink& link, int64_t& time, uint32_t blocks) {
  std::vector<float> l(64), r(64);
  float* ch[2] = {l.data(), r.data()};
  for (uint32_t b = 0; b < blocks; ++b, time += 64) link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, 64, time});
}

}  // namespace

TEST_CASE("tuning tables (M8): File > New / Open of another project uses none of the previous project's tables") {
  // Two projects with the same tuning id and table key (a 7-note MOS) but different generators. Tables used to be
  // kept per (tuning id, shape) for the link's life, so the second project reused the first one's table (defined at
  // the first generator: its node then computed instead of passing the static pitches through). A Reset now drops
  // the document's tables: the second project plays exactly what a fresh link plays.
  DocumentStore sa, sb;
  const TuningId ta = addOneTuning(sa, mosDef(7, 700.0)), tb = addOneTuning(sb, mosDef(7, 690.0));
  REQUIRE(ta == tb);
  const Snapshot a = sa.current(), b = sb.current();
  auto link = makeLink(a);
  const TuningDef& defB = findTuning(*b, tb)->def;
  const dsp::TuningTableView* viewA = link->tuningView(ta, findTuning(*a, ta)->def);
  REQUIRE(viewA != nullptr);
  CHECK(viewA->generator == 700.0);
  const doc::ChangeSet none;
  link->onDocumentChange(StoreEvent{StoreEventKind::Reset, a, b, none});
  link->flush();
  const dsp::TuningTableView* viewB = link->tuningView(tb, defB);
  REQUIRE(viewB != nullptr);
  CHECK(viewB->generator == 690.0);
  CHECK(viewB != viewA);
  // A's tables are still held by the images that play them; once the new images are current (a few blocks, stopped:
  // an instant swap) and the program collects the retired ones, they are freed.
  CHECK(link->tuningTablesBuilt() == 3);  // A's MOS (its image), B's two; A's 12-EDO, which no image read, is gone
  int64_t time = 0;
  runBlocks(*link, time, 4);  // the new images become current; the next collect retires the old ones …
  link->engine().collect();
  runBlocks(*link, time, 4);  // … and once the audio thread has passed, frees them
  link->engine().collect();
  CHECK(link->tuningTablesBuilt() == 2);  // B's MOS and its default 12-EDO
  auto fresh = makeLink(b);
  const std::vector<float> outLink = render(*link, 1.0), outFresh = render(*fresh, 1.0);
  // (From 0.1 s: the gain smoothers of the reset link start from the state they reached in the blocks above.)
  size_t equal = 0, total = 0;
  for (size_t i = at(0.1); i < outLink.size(); ++i, ++total) equal += outLink[i] == outFresh[i];
  MESSAGE(equal << " of " << total << " samples equal to a fresh link's render");
  CHECK(equal == total);
  CHECK(link->engine().timeline().sequence()->notes()[1].hz == fresh->engine().timeline().sequence()->notes()[1].hz);
}

TEST_CASE("tuning tables (M8): 1000 size / mode changes keep the pool bounded; tables of one size share a shape") {
  // Browsing sizes and modes in the MOS wheel commits SetTuningDef (structural) each time; every new shape or mode
  // used a pool slot until the 256 ran out and pitches silently went static. Tables nobody holds are freed now: the
  // document's, the running compile's and the images' (freed by the program once retired).
  DocumentStore store;
  const TuningId t = addOneTuning(store, mosDef(7, 700.0));
  auto link = makeLink(store.current());
  [[maybe_unused]] const ListenerId sub = store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  link->play(0);
  int64_t time = 0;
  uint32_t most = 0;
  for (int i = 0; i < 1000; ++i) {
    const int32_t n = 5 + i % 9;           // sizes 5…13
    const int32_t rot = (i / 9) % n;       // and modes
    REQUIRE(store.dispatch(SetTuningDef{t, mosDef(n, 1200.0 * (2.0 * n / 3.0 + 0.3) / n, rot)}).ok());
    if (i % 25 == 0) {  // a compile every so often (the debounce), with audio running in between
      link->flush();
      runBlocks(*link, time, 8);
    }
    most = std::max(most, link->tuningTablesBuilt());
  }
  link->flush();
  // T3.17-fix2: the background JIT publishes its images (Instant swaps) whenever they are done; one landing in the
  // last blocks left a retired image (and its table) not yet collected, so the count at the end depended on the JIT
  // thread's timing. Let the JIT finish first (blocks give its publishes room), then swap, retire and free.
  for (int i = 0; i < 20000; ++i) {
    const app::JitStats js = link->jitStats();
    if (js.published + js.stale + js.errors >= js.wanted) break;
    runBlocks(*link, time, 1);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  runBlocks(*link, time, 8);
  link->engine().collect();
  // An image the collect above retired (the last swap's) is freed once the audio thread has passed one more block —
  // the reclaimer's rule; the app's worker and UI idle collects (T3.17-fix1) do exactly this.
  runBlocks(*link, time, 1);
  link->engine().collect();
  MESSAGE("1000 changes: at most " << most << " tables alive, " << link->tuningTablesBuilt() << " at the end, "
                                   << link->tuningShapesAlive() << " shapes; " << link->stats().tablesExhausted
                                   << " tunings left without a table");
  CHECK(link->stats().tablesExhausted == 0);
  CHECK(most <= 8);
  CHECK(link->tuningTablesBuilt() <= 4);  // the MOS (and one an image may still hold) and the default 12-EDO
  CHECK(link->tuningShapesAlive() <= 4);
}

TEST_CASE("tuning tables (M8): every slot in use leaves further tunings static, and says so; one shape per size") {
  // 300 main tunings with a signal, all 7-note MOS (one shape), each used by a track: 256 get a table, 44 play static
  // pitches, the link reports it.
  DocumentStore store;
  IdSource& ids = store.ids();
  Batch b{"300 tunings", {}};
  for (int i = 0; i < 300; ++i) {
    const TuningId t = ids.tuning();
    b.commands.push_back(AddTuning{TuningSlot{t, "T" + std::to_string(i), Colour{0x4466AA}, mosDef(7, 690.0 + 0.05 * i)}});
    const TrackId tr = ids.track();
    b.commands.push_back(AddTrack{std::make_shared<const Track>(makeTrack(tr, TrackKind::Midi, "M")), {}, kAppend});
    b.commands.push_back(SetTrackTuning{tr, TuningAssignment::mainTuning(t)});
  }
  REQUIRE(store.dispatch(b).ok());
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.stdlib = fixture("probe_stdlib").string();
  auto made = ProjectLink::create(c, store.current());
  REQUIRE(made.ok());
  ProjectLink& link = *made.value();
  MESSAGE(link.tuningTablesBuilt() << " tables, " << link.tuningShapesAlive() << " shapes, " << link.stats().tablesExhausted
                                   << " tunings without one: " << link.lastError());
  CHECK(link.tuningTablesBuilt() == ProjectLink::kTuningTables);
  CHECK(link.tuningShapesAlive() == 2);  // the default 12-EDO's and the 7-note MOS shape, shared by 255 tables
  CHECK(link.stats().tablesExhausted == 300 + 1 - ProjectLink::kTuningTables);  // the default 12-EDO is one more
  CHECK(link.lastError().find("static pitches") != std::string::npos);
}
