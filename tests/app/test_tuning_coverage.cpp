// T2.6-fix2 / T2.6-fix3 / T2.6-fix4 (M3 review H3, H4, H5, M6–M9): tuning-signal coverage in project playback,
// rendered through the real ProjectLink with a probe instrument whose output is voice.hz (tests/app/fixtures/
// probe_stdlib), so the tuning node's pitch can be checked on every sample. The projects under
// tests/app/fixtures/m3_review are the review's probes (docs/reviews/M3/tuning/make_variants.py).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../engine/tuning_signal_ref.h"
#include "../golden/golden_audio.h"
#include "pq/app/project_link.h"
#include "pq/core/time.h"
#include "pq/document/commands.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/engine/automation.h"
#include "pq/engine/offline_render.h"
#include "pq/tuning/mos_regions.h"

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
