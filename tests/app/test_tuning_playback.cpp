// T2.6-fix1: the tuning signal in project playback — doc::lower emits a tuning node (tuning_hz + tune_voices) per
// MIDI track that follows a main tuning with a linear form, its params and automation lanes reach the node through
// project param slots, and ProjectLink owns the tables (Resources::tunings) and resolves the notes' static Hz with
// doc::resolveTrackPitches. Criteria: an automated 5L2s generator crossing a region boundary (golden + measured
// pitch), a live generator / reference change heard at the next block with no compile, and unmodulated projects
// rendering bit-identically to static pitches (the T3.17 playback golden).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/lower.h"
#include "pq/document/pitch_resolve.h"
#include "pq/document/serialize.h"
#include "pq/engine/wav_file.h"
#include "pq/tuning/mos_tuning.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

golden::Channels render(ProjectLink& link, double seconds) {
  engine::BufferSink sink;
  auto st = link.renderOffline(0, static_cast<uint64_t>(std::llround(seconds * kRate)), sink);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  return sink.channels;
}

bool updateGoldens() {
  const char* v = std::getenv("PQ_UPDATE_GOLDENS");
  return v != nullptr && *v != '\0' && *v != '0';
}

fs::path fixtureDir(const char* name) { return fs::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / name; }

// Drives a link's engine block by block like a device (RtScope: the trap fails the test on an allocation).
struct Driver {
  ProjectLink& link;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  golden::Channels out = golden::Channels(2);
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      out[0].insert(out[0].end(), l.begin(), l.end());
      out[1].insert(out[1].end(), r.begin(), r.end());
    }
  }
};

double maxDiff(const golden::Channels& a, const golden::Channels& b, size_t from = 0, size_t to = SIZE_MAX) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = from; i < std::min(to, a[c].size()); ++i)
      m = std::max(m, static_cast<double>(std::fabs(a[c][i] - b[c][i])));
  }
  return m;
}

const LoweredPatch* patchOf(const LoweredProject& lp, TrackId id) {
  for (size_t i = 0; i < lp.trackIds.size(); ++i)
    if (lp.trackIds[i] == id) return lp.patches[i].get();
  return nullptr;
}

const LoweredParam* paramOf(const LoweredProject& lp, const std::string& key) {
  for (const LoweredParam& p : lp.params)
    if (p.key == key) return &p;
  return nullptr;
}

std::string tuningKey(TuningId t, const char* name) { return "tuning:" + std::to_string(t.value) + ":" + name; }

// ---- The automation fixture -------------------------------------------------------------------------------------
//   tuning   5L2s (MOS, n = 7, period 1200 c, generator 700 c)
//   Glide    MIDI in 5L2s: one Degree note from 0 to 2.5 s; a lane on the 5L2s generator 700 c → 740 c over bar 1
//            (0 → 2 s, linear), then held: it crosses the constant-order interval boundary 3\5 = 720 c at 1 s
// 120 BPM 4/4 (a bar is 2 s). The degree is the first one whose chain step changes at 720 c (its slope dc/dg; it
// also jumps there, as buildMOSScale says).

constexpr double kGenFrom = 700.0, kGenTo = 740.0, kCrossing = 720.0;

pq::tuning::MosParams glideMos(double g) {
  pq::tuning::MosParams m;
  m.n = 7;
  m.period = 1200.0;
  m.generator = g;
  return m;
}

double degreeCents(double g, int32_t degree) {
  auto m = pq::tuning::Mos::make(glideMos(g));
  REQUIRE(m.ok());
  return m.value().centsOf(degree, 0);
}

int32_t crossingDegree() {
  for (int32_t d = 1; d < 7; ++d) {
    const double below = (degreeCents(718.0, d) - degreeCents(702.0, d)) / 16.0;
    const double above = (degreeCents(738.0, d) - degreeCents(722.0, d)) / 16.0;
    if (std::fabs(below - above) > 0.5) return d;
  }
  FAIL("no degree changes its chain step at 720 c");
  return 0;
}

struct GlideFixture {
  DocumentStore store;
  TrackId master, glide;
  TuningId t;
  ClipId clipId;
  int32_t degree = 0;

  GlideFixture() {
    IdSource& ids = store.ids();
    master = store.current()->master->id;
    glide = ids.track();
    t = ids.tuning();
    degree = crossingDegree();
    Batch b{"tuning automation fixture", {}};
    TuningSlot s{t, "5L2s", Colour{0xAA4422}, {}};
    s.def.system = glideMos(kGenFrom);
    b.commands.push_back(AddTuning{s});
    b.commands.push_back(AddTrack{playback_fixture::track(glide, TrackKind::Midi, "Glide"), {}, kAppend});
    b.commands.push_back(SetTrackTuning{glide, TuningAssignment::mainTuning(t)});
    std::vector<Note> notes{playback_fixture::note(ids, 0, 5 * kPpq, degree, 0, 110.0f)};
    ClipPtr c = playback_fixture::clip(ids, 0, 2 * kTicksPerWhole, notes, "Glide");
    clipId = c->id;
    b.commands.push_back(AddClip{glide, c});
    AutomationLane lane;
    lane.id = ids.lane();
    lane.param = ParamRef::tuning(ParamKind::MosGenerator, t);
    lane.points = {{0, static_cast<float>(kGenFrom), 0.0f, ids.breakpoint()},
                   {kTicksPerWhole, static_cast<float>(kGenTo), 0.0f, ids.breakpoint()}};
    b.commands.push_back(AddAutomationLane{glide, std::make_shared<const AutomationLane>(lane)});
    const Status st = store.dispatch(b);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  const Snapshot& snap() const { return store.current(); }
};

// The generator the lane gives at time t (seconds).
double generatorAt(double t) { return kGenFrom + (kGenTo - kGenFrom) * std::clamp(t / 2.0, 0.0, 1.0); }

struct Cycle {
  double t;   // seconds (the cycle's middle)
  double hz;
};

// Upward zero crossings (linearly interpolated) → one frequency per cycle.
std::vector<Cycle> cycles(const std::vector<float>& x) {
  std::vector<double> at;
  for (size_t i = 1; i < x.size(); ++i)
    if (x[i - 1] < 0.0f && x[i] >= 0.0f)
      at.push_back(static_cast<double>(i - 1) + static_cast<double>(x[i - 1]) / static_cast<double>(x[i - 1] - x[i]));
  std::vector<Cycle> out;
  for (size_t i = 1; i < at.size(); ++i) out.push_back({(at[i] + at[i - 1]) / 2.0 / kRate, kRate / (at[i] - at[i - 1])});
  return out;
}

}  // namespace

TEST_CASE("tuning lowering: tracks following a main tuning signal get a tuning node and its params") {
  auto lib = app::loadInstrumentLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  playback_fixture::Fixture f;
  LowerOptions lo;
  lo.library = &(*lib)->library;
  lo.externals = (*lib)->signatures;
  uint32_t nextSlot = 0;
  lo.slotOf = [&](TrackId) { return nextSlot++; };
  std::vector<std::pair<TuningId, TuningDef>> asked;
  lo.tuningTableOf = [&](TuningId id, const TuningDef& def) {
    asked.emplace_back(id, def);
    return id == f.tMos ? (def.mos()->n == 7 ? 5u : 6u) : 3u;
  };
  auto low = lower(*f.snap(), lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  const LoweredProject& lp = low.value();
  const TuningId t12 = f.snap()->tunings->front()->id;  // the default 12-EDO: Muted inherits it
  // MIDI tracks: Lead (31-EDO), Mos (5L2s), Muted (12-EDO, the first main tuning). Groups / returns / master: none.
  CHECK(patchOf(lp, f.lead)->text.find("tuning_hz(voice.degree, voice.equave, voice.hz, voice.cents, 0, tper, 0, tref, table: 3)") != std::string::npos);
  CHECK(patchOf(lp, f.mos)->text.find("tuning_hz(voice.degree, voice.equave, voice.hz, voice.cents, tgen, tper, tst, tref, table: 5)") != std::string::npos);
  CHECK(patchOf(lp, f.muted)->text.find("tune_voices(tun.hz, tun.cents)") != std::string::npos);
  CHECK(patchOf(lp, f.grp)->text.find("tuning_hz") == std::string::npos);
  CHECK(patchOf(lp, f.echo)->text.find("tuning_hz") == std::string::npos);
  CHECK(asked.size() == 3);
  // Params: one key per tuning param, current values in the document's units; the refs automation lanes target.
  REQUIRE(paramOf(lp, tuningKey(f.t31, "reference")) != nullptr);
  CHECK(paramOf(lp, tuningKey(f.t31, "reference"))->value == static_cast<float>(pq::tuning::kDefaultReferenceHz));
  CHECK(paramOf(lp, tuningKey(f.t31, "equave"))->value == 1200.0f);
  CHECK(paramOf(lp, tuningKey(f.t31, "equave"))->name == "tper");
  CHECK(paramOf(lp, tuningKey(f.tMos, "generator"))->value == 705.0f);
  CHECK(paramOf(lp, tuningKey(f.tMos, "generator"))->ref == ParamRef::tuning(ParamKind::MosGenerator, f.tMos));
  CHECK(paramOf(lp, tuningKey(f.tMos, "period"))->value == 1200.0f);
  CHECK(paramOf(lp, tuningKey(f.tMos, "stretch"))->value == 0.0f);
  CHECK(paramOf(lp, tuningKey(t12, "equave")) != nullptr);
  CHECK(paramOf(lp, tuningKey(f.tMos, "equave")) == nullptr);  // not a MOS param
  CHECK(paramKey(ParamRef::tuning(ParamKind::MosGenerator, f.tMos)) == tuningKey(f.tMos, "generator"));
  CHECK(lowerParams(*f.snap(), true).size() == lp.params.size());
  CHECK(lowerParams(*f.snap()).size() < lp.params.size());  // without tuning nodes (lower() without tuningTableOf)

  // A continuous tuning change is a param: no patch text changes.
  REQUIRE(f.store.dispatch(SetTuningParam{f.tMos, ParamKind::MosGenerator, 690.0}).ok());
  REQUIRE(f.store.dispatch(SetTuningParam{f.t31, ParamKind::TuningReference, 300.0}).ok());
  auto again = lower(*f.snap(), lo, &lp);
  REQUIRE(again.ok());
  for (size_t i = 0; i < lp.patches.size(); ++i) CHECK(again.value().patches[i] == lp.patches[i]);
  CHECK(paramOf(again.value(), tuningKey(f.tMos, "generator"))->value == 690.0f);
  CHECK(paramOf(again.value(), tuningKey(f.t31, "reference"))->value == 300.0f);

  // A shape change (structural) asks for the table of the new shape; only the tracks of that tuning change text.
  TuningDef d = findTuning(*f.snap(), f.tMos)->def;
  pq::tuning::MosParams mp = *d.mos();
  mp.n = 5;
  d.system = mp;
  REQUIRE(f.store.dispatch(SetTuningDef{f.tMos, d}).ok());
  auto shaped = lower(*f.snap(), lo, &again.value());
  REQUIRE(shaped.ok());
  CHECK(patchOf(shaped.value(), f.mos)->text.find("table: 6") != std::string::npos);
  CHECK(patchOf(shaped.value(), f.mos) != patchOf(again.value(), f.mos));
  CHECK(patchOf(shaped.value(), f.lead) == patchOf(again.value(), f.lead));

  // A private tuning, a Scala tuning or a MODMOS has no node: static pitches.
  REQUIRE(f.store.dispatch(SetTrackTuning{f.lead, TuningAssignment::privateTuning(TuningDef{})}).ok());
  auto priv = lower(*f.snap(), lo, &shaped.value());
  REQUIRE(priv.ok());
  CHECK(patchOf(priv.value(), f.lead)->text.find("tuning_hz") == std::string::npos);
  mp.modmos = {0.0, 1.0};
  d.system = mp;
  REQUIRE(f.store.dispatch(SetTuningDef{f.tMos, d}).ok());
  auto modmos = lower(*f.snap(), lo, &priv.value());
  REQUIRE(modmos.ok());
  CHECK(patchOf(modmos.value(), f.mos)->text.find("tuning_hz") == std::string::npos);
  CHECK(paramOf(modmos.value(), tuningKey(f.tMos, "generator")) == nullptr);
}

TEST_CASE("tuning playback: unmodulated projects render bit-identically to static pitches (the T3.17 golden)") {
  // The playback fixture's three tunings (12-EDO, 31-EDO, 5L2s) are all signals now: every MIDI track has a tuning
  // node. Unmodulated, the node passes each Degree note's static Hz through, so the render equals the golden made
  // before T2.6-fix1 sample for sample (not only within the golden tolerance).
  auto loaded = loadProject(fixtureDir("playback").string());
  REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
  auto link = makeLink(loaded->project);
  CHECK(link->tuningTablesBuilt() == 3);
  const golden::Channels out = render(*link, 2.5);
  auto gold = engine::readWav(golden::goldenDir() / "audio" / "project_playback.wav");
  REQUIRE(gold.ok());
  REQUIRE(gold->channels.size() == out.size());
  CHECK(maxDiff(gold->channels, out) == 0.0);
  // The Degree notes really follow the nodes: their sequence notes carry the tuning signal.
  uint32_t following = 0;
  for (const auto& n : link->engine().timeline().sequence()->notes()) following += n.pitch.tuning != 0;
  CHECK(following == 6 + 5 + 4);  // Lead, Mos, Muted
}

TEST_CASE("tuning playback: a generator or reference change while playing is heard at the next block, no compile") {
  playback_fixture::Fixture fa, fb;
  auto a = makeLink(fa.snap()), b = makeLink(fb.snap());
  [[maybe_unused]] const ListenerId sub = fa.store.subscribe([&](const StoreEvent& e) { a->onDocumentChange(e); });
  Driver da{*a}, db{*b};
  a->play(0);
  b->play(0);
  da.run(225);  // 0.3 s: Mos's second note (degree 2, which moves with the generator) is sounding
  db.run(225);
  CHECK(maxDiff(da.out, db.out) == 0.0);
  const app::LinkStats s0 = a->stats();
  const engine::TimelineStats t0 = a->engine().timeline().stats();

  auto step = [&](const Command& c, const std::string& key, float value) {
    const size_t before = da.out[0].size();
    REQUIRE(fa.store.dispatch(c).ok());
    a->flush();
    CHECK(a->stats().compiles == s0.compiles);
    CHECK(a->stats().unitCompiles == s0.unitCompiles);
    CHECK(a->stats().sequencePublishes == s0.sequencePublishes);  // Degree notes follow the node: nothing to resolve
    da.run(1);
    db.run(1);
    const uint32_t slot = a->paramSlot(key);
    REQUIRE(slot != ProjectLink::kNoSlot);
    CHECK(a->engine().program()->projectParams()[slot] == value);
    CHECK(maxDiff(da.out, db.out, before, before + kBlock) > 1e-5);  // the very next block differs
  };
  step(SetTuningParam{fa.tMos, ParamKind::MosGenerator, 690.0}, tuningKey(fa.tMos, "generator"), 690.0f);
  CHECK(maxDiff(da.out, db.out, 0, 225 * kBlock) == 0.0);
  da.run(155);  // 0.507 s: Lead's second note (31-EDO degree 10) is sounding
  db.run(155);
  step(SetTuningParam{fa.t31, ParamKind::TuningReference, 300.0}, tuningKey(fa.t31, "reference"), 300.0f);
  // Held notes kept sounding (no sequence swap stopped them) and the edit stays audible.
  da.run(20);
  db.run(20);
  CHECK(a->engine().timeline().stats().stoppedOnSwap == t0.stoppedOnSwap);
  CHECK(maxDiff(da.out, db.out, da.out[0].size() - 20 * kBlock) > 1e-4);

  // Setting the generator back to the document's original value passes the static pitches through again: from the
  // next block on, the output equals the unedited link's... once the reference is back too.
  REQUIRE(fa.store.dispatch(SetTuningParam{fa.tMos, ParamKind::MosGenerator, 705.0}).ok());
  REQUIRE(fa.store.dispatch(SetTuningParam{fa.t31, ParamKind::TuningReference, pq::tuning::kDefaultReferenceHz}).ok());
  a->flush();
  CHECK(a->stats().compiles == s0.compiles);
  // Long enough for every voice started under the edit to end (Lead's 0.25 s notes, Mos's eighths, 250 ms release).
  da.run(1200);
  db.run(1200);
  const size_t n = da.out[0].size();
  CHECK(maxDiff(da.out, db.out, n - 100 * kBlock, n) <= 1e-6);
}

TEST_CASE("golden: a 5L2s generator automated across a region boundary (the pitch follows it, jumping only at the crossing)") {
  GlideFixture f;
  const fs::path dir = fixtureDir("tuning_automation");
  const fs::path wav = golden::goldenDir() / "audio" / "project_tuning_automation.wav";
  constexpr double kSeconds = 2.5;
  if (updateGoldens()) {
    fs::create_directories(dir);
    REQUIRE(saveProject(dir.string(), *f.snap()).ok());
  }
  auto loaded = loadProject(dir.string());
  REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
  CHECK(*loaded->project == *f.snap());  // the committed fixture is the project this test builds

  auto link = makeLink(loaded->project);
  const app::LinkStats s0 = link->stats();
  const golden::Channels out = render(*link, kSeconds);
  CHECK(link->stats().compiles == s0.compiles);  // automation is not a compile
  REQUIRE(out.size() == 2);
  if (updateGoldens()) {
    fs::create_directories(wav.parent_path());
    REQUIRE(engine::writeWav(wav, out, static_cast<uint32_t>(kRate)).ok());
    MESSAGE("updated " << wav.string());
  }
  auto gold = engine::readWav(wav);
  REQUIRE_MESSAGE(gold.ok(), (gold.ok() ? std::string() : gold.error().message + " (PQ_UPDATE_GOLDENS=1)"));
  const golden::Comparison c = golden::compare(*gold, out, static_cast<uint32_t>(kRate));
  CHECK_MESSAGE(c.ok, c.message);

  // The pitch, cycle by cycle, against buildMOSScale at the lane's generator (block-rate automation: ≤ 0.03 c of
  // generator per block, a fraction of the 0.5 c tolerance). The cycles that straddle the crossing are excluded
  // from the comparison (their period mixes both pitches) and checked as the jump instead.
  const std::vector<Cycle> cs = cycles(out[0]);
  REQUIRE(cs.size() > 500);
  constexpr double kCrossingT = 1.0;  // seconds: the lane reaches 720 c at 1 s
  double worst = 0.0, worstStep = 0.0, worstStepAt = 0.0, otherStep = 0.0;
  uint32_t checked = 0;
  std::optional<double> lastBelow, firstAbove;  // cents of the last cycle before / first after the crossing
  double tBelow = 0.0, tAbove = 0.0;
  std::vector<double> gBelow, cBelow, gAbove, cAbove;
  double prevCents = 0.0;
  bool havePrev = false;
  for (const Cycle& cy : cs) {
    if (cy.t < 0.02 || cy.t > kSeconds - 0.01) continue;  // the attack's first cycles, the render's end
    const double g = generatorAt(cy.t);
    const double got = 1200.0 * std::log2(cy.hz / pq::tuning::kDefaultReferenceHz);
    const bool near = std::fabs(cy.t - kCrossingT) < 0.006;  // the cycles around the crossing (~2.5 periods)
    if (!near) {
      worst = std::max(worst, std::fabs(got - degreeCents(g, f.degree)));
      ++checked;
      if (cy.t < kCrossingT) {
        lastBelow = got;
        tBelow = cy.t;
      } else if (!firstAbove) {
        firstAbove = got;
        tAbove = cy.t;
      }
    }
    if (havePrev) {
      const double st = std::fabs(got - prevCents);
      if (st > worstStep) {
        worstStep = st;
        worstStepAt = cy.t;
      }
      if (!near) otherStep = std::max(otherStep, st);
    }
    prevCents = got;
    havePrev = true;
    if (g > kGenFrom + 1.0 && g < kCrossing - 1.0) {
      gBelow.push_back(g);
      cBelow.push_back(got);
    } else if (g > kCrossing + 1.0 && g < kGenTo - 1.0) {
      gAbove.push_back(g);
      cAbove.push_back(got);
    }
  }
  auto slope = [](const std::vector<double>& x, const std::vector<double>& y) {
    double mx = 0, my = 0;
    for (size_t i = 0; i < x.size(); ++i) mx += x[i], my += y[i];
    mx /= static_cast<double>(x.size());
    my /= static_cast<double>(y.size());
    double sxy = 0, sxx = 0;
    for (size_t i = 0; i < x.size(); ++i) sxy += (x[i] - mx) * (y[i] - my), sxx += (x[i] - mx) * (x[i] - mx);
    return sxy / sxx;
  };
  const double kBelow = (degreeCents(718.0, f.degree) - degreeCents(702.0, f.degree)) / 16.0;
  const double kAbove = (degreeCents(738.0, f.degree) - degreeCents(722.0, f.degree)) / 16.0;
  const double mBelow = slope(gBelow, cBelow), mAbove = slope(gAbove, cAbove);
  // At 3\5 the chain's 7th note passes the period: the degree changes chain step (its slope dc/dg) and jumps;
  // everywhere else the pitch is continuous. The measured jump is between the cycles just outside the crossing's
  // neighbourhood, so it is compared with buildMOSScale at those cycles' generators (the slope adds ~1 c over them).
  const double expectedJump = degreeCents(kCrossing + 1e-6, f.degree) - degreeCents(kCrossing - 1e-6, f.degree);
  REQUIRE(lastBelow);
  REQUIRE(firstAbove);
  const double jump = *firstAbove - *lastBelow;
  const double jumpAtCycles = degreeCents(generatorAt(tAbove), f.degree) - degreeCents(generatorAt(tBelow), f.degree);
  MESSAGE("degree " << f.degree << ": " << checked << " cycles, max |measured - buildMOSScale| " << worst
                    << " c; dc/dg " << mBelow << " below 720 c (expected " << kBelow << "), " << mAbove
                    << " above (expected " << kAbove << "); jump at the crossing " << jump << " c (expected "
                    << jumpAtCycles << " between those cycles, " << expectedJump << " at 720 c), largest step " << worstStep << " c at t = " << worstStepAt
                    << " s, largest step elsewhere " << otherStep << " c");
  CHECK(worst < 0.5);
  CHECK(std::fabs(mBelow - kBelow) < 0.02);
  CHECK(std::fabs(mAbove - kAbove) < 0.02);
  CHECK(std::fabs(expectedJump) > 10.0);
  CHECK(std::fabs(jumpAtCycles - expectedJump) < 2.0);
  CHECK(std::fabs(jump - jumpAtCycles) < 0.5);
  CHECK(std::fabs(worstStepAt - kCrossingT) < 0.006);
  CHECK(otherStep < 0.5);
  // The note's static Hz (unmodulated) is the generator of the document, 700 c: at the first block the lane gives
  // 700 c too, so the node passes the static pitch through.
  const auto pitches = resolveTrackPitches(*loaded->project, f.glide);
  REQUIRE(pitches.ok());
  REQUIRE(pitches.value().clips[0].notes[0].hz);
  CHECK(pitches.value().clips[0].notes[0].signal == f.t);
}
