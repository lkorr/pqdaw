// T3.17-fix3: lowering semantics, the M3 review's document probes (docs/reviews/M3/document/probe_mix.py,
// probe_lanes.py, probe_slots.py, probe_edges.py) as tests, each with the result Live gives (M1–M5, M16, L1–L5).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/cli.h"
#include "pq/app/commands.h"
#include "pq/app/project_link.h"
#include "pq/document/audibility.h"
#include "pq/document/lower.h"
#include "pq/document/serialize.h"
#include "pq/tuning/scala.h"
#include "pq/engine/offline_render.h"
#include "pq/views/view_context.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr double kRate = 48000.0;
constexpr double kRef = 261.626;  // the default 12-EDO's reference

using Channels = std::vector<std::vector<float>>;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, uint64_t seed = 0) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.chanceSeed = seed;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

Channels render(ProjectLink& link, double seconds) {
  engine::BufferSink sink;
  auto st = link.renderOffline(0, static_cast<uint64_t>(std::llround(seconds * kRate)), sink);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  return sink.channels;
}

Channels render(const Snapshot& s, double seconds, uint64_t seed = 0) {
  auto link = makeLink(s, seed);
  return render(*link, seconds);
}

double hz(int degree) { return kRef * std::pow(2.0, degree / 12.0); }

// The amplitude of a degree's fundamental per channel (Hann-windowed DFT at that exact frequency, 1.5 … 2.5 s), as
// probe_mix.py's levels().
std::vector<double> level(const Channels& x, int degree) {
  const size_t a = static_cast<size_t>(1.5 * kRate), b = static_cast<size_t>(2.5 * kRate);
  std::vector<double> out;
  for (const auto& ch : x) {
    std::complex<double> acc = 0.0;
    double wsum = 0.0;
    for (size_t i = a; i < b && i < ch.size(); ++i) {
      const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i - a) / static_cast<double>(b - a));
      const double t = static_cast<double>(i - a) / kRate;
      acc += std::polar(w * ch[i], -2.0 * std::numbers::pi * hz(degree) * t);
      wsum += w;
    }
    out.push_back(std::abs(acc) * 2.0 / wsum);
  }
  return out;
}

double dbGain(double db) { return db <= kMinVolumeDb ? 0.0 : std::pow(10.0, db / 20.0); }
std::pair<double, double> cp(double pan, double db = 0.0) {  // constant power (MIDI tracks)
  const double th = (pan + 1.0) * std::numbers::pi / 4.0;
  return {std::cos(th) * dbGain(db), std::sin(th) * dbGain(db)};
}
std::pair<double, double> bal(double pan, double db = 0.0) {  // balance (buses)
  return {std::min(1.0, 1.0 - pan) * dbGain(db), std::min(1.0, 1.0 + pan) * dbGain(db)};
}

// A project built through commands, with probe_mix.py's tone tracks: a MIDI track holding one note of its degree.
struct Proj {
  DocumentStore store;
  Batch batch{"probe", {}};
  TrackId master;
  Proj() { master = store.current()->master->id; }
  TrackId add(TrackKind kind, const char* name, TrackId parent = {}) {
    const TrackId id = store.ids().track();
    batch.commands.push_back(AddTrack{playback_fixture::track(id, kind, name), parent, kAppend});
    return id;
  }
  TrackId tone(int degree, TrackId parent = {}, double seconds = 3.0) {
    const TrackId id = add(TrackKind::Midi, "tone", parent);
    const Ticks len = static_cast<Ticks>(seconds * 2.0) * kPpq;  // 120 BPM: a quarter is 0.5 s
    std::vector<Note> n{playback_fixture::note(store.ids(), 0, len, degree)};
    batch.commands.push_back(AddClip{id, playback_fixture::clip(store.ids(), 0, len, n, "tone")});
    return id;
  }
  void cmd(Command c) { batch.commands.push_back(std::move(c)); }
  Snapshot done() {
    const Status st = store.dispatch(batch);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    batch.commands.clear();
    return store.current();
  }
};

double referenceAmplitude() {  // one track, 0 dB, centre: the instrument's amplitude before the pan law
  static const double a0 = [] {
    Proj p;
    p.tone(0);
    return level(render(p.done(), 3.0), 0)[0] / std::cos(std::numbers::pi / 4.0);
  }();
  return a0;
}

void checkGain(const Channels& x, int degree, std::pair<double, double> expect) {
  const std::vector<double> got = level(x, degree);
  const double a0 = referenceAmplitude();
  const double want[2] = {expect.first, expect.second};
  for (int c = 0; c < 2; ++c) {
    CAPTURE(degree);
    CAPTURE(c);
    const double g = got[static_cast<size_t>(c)] / a0;
    if (want[c] < 1e-9) {
      CHECK(g < 1e-4);
    } else {
      CHECK(std::fabs(20.0 * std::log10(g / want[c])) < 0.05);
    }
  }
}

bool heard(const Channels& x, int degree) {
  const std::vector<double> l = level(x, degree);
  return std::max(l[0], l[1]) / referenceAmplitude() > 1e-4;
}

}  // namespace

// ---- probe_mix.py: the laws ---------------------------------------------------------------------------------------

TEST_CASE("probe_mix: volume / pan law, group balance and master, post and pre sends (Live-expected gains)") {
  {
    Proj p;
    const TrackId a = p.tone(0), b = p.tone(4), c = p.tone(7), d = p.tone(9);
    p.cmd(SetTrackVolume{a, -6.0f});
    p.cmd(SetTrackPan{a, -0.4f});
    p.cmd(SetTrackVolume{b, 6.0f});
    p.cmd(SetTrackPan{b, 1.0f});
    p.cmd(SetTrackVolume{c, -69.9f});
    p.cmd(SetTrackVolume{d, -70.0f});
    const Channels x = render(p.done(), 3.0);
    checkGain(x, 0, cp(-0.4, -6));
    checkGain(x, 4, cp(1.0, 6));
    checkGain(x, 7, cp(0, -69.9));
    checkGain(x, 9, {0, 0});  // the fader bottom is −∞
  }
  {
    Proj p;
    const TrackId g = p.add(TrackKind::Group, "g");
    p.tone(0, g);
    p.cmd(SetTrackVolume{g, -4.0f});
    p.cmd(SetTrackPan{g, 0.5f});
    p.cmd(SetTrackVolume{p.master, -2.0f});
    p.cmd(SetTrackPan{p.master, -0.25f});
    const Channels x = render(p.done(), 3.0);
    const auto t = cp(0), gb = bal(0.5, -4), m = bal(-0.25, -2);
    checkGain(x, 0, {t.first * gb.first * m.first, t.second * gb.second * m.second});
  }
  {
    Proj p;
    const TrackId r1 = p.add(TrackKind::Return, "r1"), r2 = p.add(TrackKind::Return, "r2");
    const TrackId t = p.tone(0);
    p.cmd(SetTrackVolume{r1, -3.0f});
    p.cmd(SetTrackPan{r1, 0.3f});
    p.cmd(SetTrackVolume{t, -10.0f});
    p.cmd(SetSendLevel{t, r1, -6.0f});
    p.cmd(SetSendLevel{t, r2, -12.0f});
    p.cmd(SetSendPreFader{t, r2, true});
    p.cmd(SetTrackOutput{t, Output{OutputKind::None, {}}});
    const Channels x = render(p.done(), 3.0);
    const auto post = cp(0, -16), rb = bal(0.3, -3), pre = cp(0, -12);
    checkGain(x, 0, {post.first * rb.first + pre.first, post.second * rb.second + pre.second});
  }
}

// ---- probe_mix.py: mute / solo / routing scenarios (Live's exclusive solo, solo in place) -------------------------

TEST_CASE("probe_mix: solo, mute and routing scenarios give Live's result (M1, M2, L1, L2, L3)") {
  SUBCASE("s1 solo a child in a group") {
    Proj p;
    p.tone(0);
    const TrackId g = p.add(TrackKind::Group, "g");
    p.tone(2, g);
    const TrackId c = p.tone(4, g);
    p.cmd(SetTrackSolo{c, true});
    const Channels x = render(p.done(), 3.0);
    CHECK(!heard(x, 0));
    CHECK(!heard(x, 2));
    CHECK(heard(x, 4));
  }
  SUBCASE("s2 solo a group") {
    Proj p;
    p.tone(0);
    const TrackId g = p.add(TrackKind::Group, "g");
    p.tone(2, g);
    p.tone(4, g);
    p.cmd(SetTrackSolo{g, true});
    const Channels x = render(p.done(), 3.0);
    CHECK(!heard(x, 0));
    CHECK(heard(x, 2));
    CHECK(heard(x, 4));
  }
  SUBCASE("s3 a soloed child of a muted outer group") {
    Proj p;
    p.tone(0);
    const TrackId g = p.add(TrackKind::Group, "outer");
    const TrackId h = p.add(TrackKind::Group, "inner", g);
    const TrackId a = p.tone(2, h);
    p.tone(4, h);
    p.cmd(SetTrackSolo{a, true});
    p.cmd(SetTrackMute{g, true});
    const Channels x = render(p.done(), 3.0);
    CHECK(!heard(x, 0));
    CHECK(!heard(x, 2));
    CHECK(!heard(x, 4));
  }
  SUBCASE("s4 a soloed return: its senders are heard through it only") {
    Proj p;
    const TrackId r = p.add(TrackKind::Return, "r");
    const TrackId a = p.tone(0);
    p.tone(4);
    p.cmd(SetSendLevel{a, r, 0.0f});
    p.cmd(SetTrackSolo{r, true});
    const Channels x = render(p.done(), 3.0);
    CHECK(heard(x, 0));
    CHECK(!heard(x, 4));
  }
  SUBCASE("s5 a send to a muted return") {
    Proj p;
    const TrackId r = p.add(TrackKind::Return, "r");
    const TrackId a = p.tone(0);
    p.cmd(SetTrackOutput{a, Output{OutputKind::None, {}}});
    p.cmd(SetSendLevel{a, r, 0.0f});
    p.cmd(SetTrackMute{r, true});
    CHECK(!heard(render(p.done(), 3.0), 0));
  }
  SUBCASE("s6 a soloed track routed to another group: the destination stays open (M1)") {
    Proj p;
    const TrackId g1 = p.add(TrackKind::Group, "g1");
    const TrackId a = p.tone(0, g1);
    p.tone(2, g1);
    const TrackId g2 = p.add(TrackKind::Group, "g2");
    p.tone(7, g2);
    p.cmd(SetTrackSolo{a, true});
    p.cmd(SetTrackOutput{a, Output{OutputKind::Track, g2}});
    const Snapshot s = p.done();
    const AudibilityMap aud = computeAudibility(*s);
    CHECK(aud.find(g2)->output);  // the destination is solo-related now
    const Channels x = render(s, 3.0);
    CHECK(heard(x, 0));  // was silent: g2's gate closed
    CHECK(!heard(x, 2));
    CHECK(!heard(x, 7));
  }
  SUBCASE("s7 a track routed to a deactivated group is not heard (L1)") {
    Proj p;
    const TrackId a = p.tone(0);
    const TrackId g2 = p.add(TrackKind::Group, "g2");
    p.tone(7, g2);
    p.cmd(SetTrackOutput{a, Output{OutputKind::Track, g2}});
    p.cmd(SetTrackActive{g2, false});
    const Channels x = render(p.done(), 3.0);
    CHECK(!heard(x, 0));  // was heard: it fell back to the master
    CHECK(!heard(x, 7));
  }
  SUBCASE("s8 a return routed to a muted group is not heard (L2)") {
    Proj p;
    const TrackId r = p.add(TrackKind::Return, "r");
    const TrackId g = p.add(TrackKind::Group, "g");
    p.tone(2, g);
    const TrackId a = p.tone(0);
    p.cmd(SetTrackOutput{r, Output{OutputKind::Track, g}});
    p.cmd(SetTrackMute{g, true});
    p.cmd(SetTrackOutput{a, Output{OutputKind::None, {}}});
    p.cmd(SetSendLevel{a, r, 0.0f});
    const Channels x = render(p.done(), 3.0);
    CHECK(!heard(x, 0));  // was heard: the return bypassed the group to the master
    CHECK(!heard(x, 2));
  }
  SUBCASE("s8b a return routed to a group is heard through it") {
    Proj p;
    const TrackId r = p.add(TrackKind::Return, "r");
    const TrackId g = p.add(TrackKind::Group, "g");
    const TrackId a = p.tone(0);
    p.cmd(SetTrackOutput{r, Output{OutputKind::Track, g}});
    p.cmd(SetTrackVolume{g, -6.0f});
    p.cmd(SetTrackOutput{a, Output{OutputKind::None, {}}});
    p.cmd(SetSendLevel{a, r, 0.0f});
    const auto t = cp(0), gb = bal(0, -6);
    checkGain(render(p.done(), 3.0), 0, {t.first * gb.first, t.second * gb.second});
  }
  SUBCASE("s9 a soloed deactivated track does not silence the others (L3)") {
    Proj p;
    const TrackId a = p.tone(0);
    p.tone(4);
    p.cmd(SetTrackSolo{a, true});
    p.cmd(SetTrackActive{a, false});
    const Channels x = render(p.done(), 3.0);
    CHECK(!heard(x, 0));
    CHECK(heard(x, 4));  // was silent
  }
  SUBCASE("s10 a return → return send is heard (M2)") {
    Proj p;
    const TrackId r1 = p.add(TrackKind::Return, "r1"), r2 = p.add(TrackKind::Return, "r2");
    const TrackId a = p.tone(0);
    p.cmd(SetSendLevel{r1, r2, 0.0f});
    p.cmd(SetTrackOutput{r1, Output{OutputKind::None, {}}});
    p.cmd(SetTrackOutput{a, Output{OutputKind::None, {}}});
    p.cmd(SetSendLevel{a, r1, 0.0f});
    const auto t = cp(0);
    checkGain(render(p.done(), 3.0), 0, t);  // through r1 → r2 → master, all at 0 dB (was silent)
  }
}

TEST_CASE("return → return sends: a loop is closed by an automatic 1-sample delay on the send that closes it (M2)") {
  Proj p;
  const TrackId r1 = p.add(TrackKind::Return, "r1"), r2 = p.add(TrackKind::Return, "r2");
  const TrackId a = p.tone(0);
  p.cmd(SetSendLevel{a, r1, 0.0f});
  p.cmd(SetSendLevel{r1, r2, -12.0f});
  p.cmd(SetSendLevel{r2, r1, -12.0f});  // closes the loop: r1 → r2 → r1
  const Snapshot s = p.done();
  const std::vector<DelayedSend> d = delayedSends(*s);
  REQUIRE(d.size() == 1);
  CHECK(d[0].from == r2);  // the later of the two (sends are lowered in track order)
  CHECK(d[0].to == r1);
  auto link = makeLink(s);
  const Channels x = render(*link, 3.0);
  float peak = 0.0f;
  for (const auto& ch : x)
    for (float v : ch) {
      REQUIRE(std::isfinite(v));
      peak = std::max(peak, std::fabs(v));
    }
  CHECK(peak > 0.01f);
  CHECK(peak < 1.0f);  // −12 dB per pass: the loop decays
  CHECK(link->stats().delayedSends == 1);
  // A return → return send at −∞ (not stored) is not lowered at all: no loop, no delay, no merged units.
  Proj q;
  const TrackId q1 = q.add(TrackKind::Return, "r1");
  q.add(TrackKind::Return, "r2");
  q.tone(0);
  (void)q1;
  CHECK(delayedSends(*q.done()).empty());
  CHECK(conditionalSendsKey(*q.store.current()).empty());
}

TEST_CASE("return → return send turned up from −∞ while playing: compiled at once (the send now exists)") {
  Proj p;
  const TrackId r1 = p.add(TrackKind::Return, "r1"), r2 = p.add(TrackKind::Return, "r2");
  const TrackId a = p.tone(0);
  p.cmd(SetSendLevel{a, r1, 0.0f});
  p.cmd(SetTrackOutput{r1, Output{OutputKind::None, {}}});
  p.cmd(SetTrackOutput{a, Output{OutputKind::None, {}}});
  auto link = makeLink(p.done());
  [[maybe_unused]] const ListenerId sub = p.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  const app::LinkStats s0 = link->stats();
  REQUIRE(p.store.dispatch(SetSendLevel{r1, r2, -3.0f}).ok());  // a Param change in the document
  link->flush();
  CHECK(link->stats().compiles == s0.compiles + 1);
  CHECK(heard(render(*link, 3.0), 0));
}

// ---- probe_lanes.py ---------------------------------------------------------------------------------------------

TEST_CASE("probe_lanes: a mute (activator) lane plays (M3)") {
  Proj p;
  const TrackId t = p.tone(0, {}, 5.0);
  AutomationLane lane;
  lane.id = p.store.ids().lane();
  lane.param = ParamRef::track(ParamKind::TrackMute, t);
  lane.points = {{0, 0.0f, 0.0f, p.store.ids().breakpoint()},
                 {4 * kPpq, 0.0f, 0.0f, p.store.ids().breakpoint()},
                 {4 * kPpq + 1, 1.0f, 0.0f, p.store.ids().breakpoint()}};
  p.cmd(AddAutomationLane{t, std::make_shared<const AutomationLane>(lane)});
  const Channels x = render(p.done(), 4.8);
  auto rms = [&](double a, double b) {
    double s = 0.0;
    size_t n = 0;
    for (size_t i = static_cast<size_t>(a * kRate); i < static_cast<size_t>(b * kRate); ++i, ++n) s += x[0][i] * x[0][i];
    return std::sqrt(s / static_cast<double>(n));
  };
  CHECK(rms(1.0, 2.0) > 0.01);
  CHECK(rms(3.0, 4.0) < 1e-6);  // muted from 2 s (was: the lane was ignored)
}

TEST_CASE("probe_lanes: tunings without a signal refuse new lanes; the lane chooser does not offer them (M3)") {
  Proj p;
  const TuningId scl = p.store.ids().tuning();
  TuningSlot slot{scl, "maj", Colour{0x4422AA}, {}};
  auto parsed = tuning::parseScl("! maj.scl\nmajor\n 7\n!\n9/8\n5/4\n4/3\n3/2\n5/3\n15/8\n2/1\n");
  REQUIRE(parsed.ok());
  slot.def.system = ScaleDef{parsed.value(), std::nullopt};
  p.cmd(AddTuning{slot});
  const TrackId t = p.tone(0);
  p.cmd(SetTrackTuning{t, TuningAssignment::mainTuning(scl)});
  p.done();
  AutomationLane lane;
  lane.id = p.store.ids().lane();
  lane.param = ParamRef::tuning(ParamKind::TuningReference, scl);
  lane.points = {{0, 261.626f, 0.0f, p.store.ids().breakpoint()}};
  const Status st = p.store.dispatch(AddAutomationLane{t, std::make_shared<const AutomationLane>(lane)});
  CHECK_FALSE(st.ok());
  // An EDO tuning's reference lane is accepted (it has a node).
  const TuningId edo = p.store.current()->tunings->front()->id;
  lane.param = ParamRef::tuning(ParamKind::TuningReference, edo);
  CHECK(p.store.dispatch(AddAutomationLane{t, std::make_shared<const AutomationLane>(lane)}).ok());
}

TEST_CASE("automation overrides (T3.15) are consumed: a hand-made value plays until Re-enable Automation (M4)") {
  playback_fixture::Fixture f;
  auto link = makeLink(f.snap());
  views::ViewContext ctx(*link);
  ctx.store().reset(f.snap());
  ctx.store().ids().reserveThrough(f.store.ids().peek());
  link->attach(ctx);
  link->flush();
  const uint32_t slot = link->paramSlot(std::to_string(f.grp.value) + ":volume");
  REQUIRE(slot != ProjectLink::kNoSlot);
  engine::Engine& eng = link->engine();
  int64_t time = 0;
  auto valueAfterBlock = [&](Ticks at) {
    link->locate(at);
    std::vector<float> l(64), r(64);
    float* ch[2] = {l.data(), r.data()};
    for (int i = 0; i < 4; ++i, time += 64) eng.process(engine::AudioBlock{nullptr, 0, ch, 2, 64, time});
    return eng.program()->projectParams()[slot];
  };
  link->play(0);
  CHECK(valueAfterBlock(0) == doctest::Approx(-18.0).epsilon(0.02));  // the lane (−18 dB at bar 1)
  REQUIRE(ctx.store().dispatch(SetTrackVolume{f.grp, -3.0f}).ok());  // by hand: overrides the lane (Live)
  CHECK(ctx.automationOverridden(ParamRef::track(ParamKind::TrackVolume, f.grp)));
  CHECK(valueAfterBlock(0) == -3.0f);  // was: the lane kept playing
  ctx.reenableAutomation();
  CHECK(valueAfterBlock(0) == doctest::Approx(-18.0).epsilon(0.02));
  link->detach();
}

// ---- probe_slots.py ---------------------------------------------------------------------------------------------

namespace {
// n MIDI tracks, a note only on the 129th (or the last one with a slot).
void slotsCase(uint32_t n) {
  {
    CAPTURE(n);
    Proj p;
    std::vector<TrackId> ids;
    for (uint32_t i = 0; i < n; ++i) ids.push_back(p.add(TrackKind::Midi, "m"));
    const TrackId last = n == 129 ? ids.back() : ids[ProjectLink::kEventSlots - 1];
    std::vector<Note> note{playback_fixture::note(p.store.ids(), 0, kPpq, 0)};
    p.cmd(AddClip{last, playback_fixture::clip(p.store.ids(), 0, 2 * kPpq, note, "c")});
    auto link = makeLink(p.done());
    const Channels x = render(*link, 1.0);
    float peak = 0.0f;
    for (float v : x[0]) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.01f);  // was 0 at 129 tracks
    CHECK(link->stats().tracksWithoutSlot == n - std::min(n, ProjectLink::kEventSlots));
    if (n > ProjectLink::kEventSlots) CHECK(link->lastError().find("at most") != std::string::npos);
  }
}
}  // namespace

TEST_CASE("probe_slots: more than 128 MIDI tracks play (M5)") { slotsCase(129); }

// A stress size (513 tracks, ~2 s in dev): the slow tier.
TEST_CASE("probe_slots: beyond the engine's 512 MIDI tracks the status says so (M5)" * doctest::test_suite("slow")) {
  slotsCase(ProjectLink::kEventSlots + 1u);
}

// ---- probe_edges.py ---------------------------------------------------------------------------------------------

TEST_CASE("probe_edges: a 1-tick note sounds (L4)") {
  Proj p;
  const TrackId t = p.add(TrackKind::Midi, "m");
  std::vector<Note> n{playback_fixture::note(p.store.ids(), 0, 1, 0), playback_fixture::note(p.store.ids(), 2 * kPpq, 1, 0)};
  p.cmd(AddClip{t, playback_fixture::clip(p.store.ids(), 0, 4 * kPpq, n, "c")});
  const Channels x = render(p.done(), 2.0);
  float a = 0.0f, b = 0.0f;
  for (size_t i = 0; i < 24000; ++i) a = std::max(a, std::fabs(x[0][i]));
  for (size_t i = 48000; i < 72000; ++i) b = std::max(b, std::fabs(x[0][i]));
  CHECK(a > 1e-4f);  // was exactly 0: on and off on one sample
  CHECK(b > 1e-4f);
}

TEST_CASE("probe_edges: chance decides anew per playback and per loop repeat, the same for a seed (M16)") {
  Proj p;
  const TrackId t = p.add(TrackKind::Midi, "m");
  std::vector<Note> ns;
  for (int i = 0; i < 40; ++i) {
    Note n = playback_fixture::note(p.store.ids(), i * (kPpq / 2), kPpq / 8, 0);
    n.chance = 0.5f;
    ns.push_back(n);
  }
  p.cmd(AddClip{t, playback_fixture::clip(p.store.ids(), 0, 20 * kPpq, ns, "c")});
  const Snapshot s = p.done();
  auto onsets = [](const Channels& x) {  // the 40 slots: played or not
    std::vector<bool> on;
    for (int i = 0; i < 40; ++i) {
      float pk = 0.0f;
      for (size_t k = 0; k < 6000; ++k) pk = std::max(pk, std::fabs(x[0][static_cast<size_t>(i) * 12000 + k]));
      on.push_back(pk > 1e-4f);
    }
    return on;
  };
  // Offline: seeded, so a render repeats exactly; another seed decides otherwise.
  const auto a = onsets(render(s, 10.5, 7)), b = onsets(render(s, 10.5, 7)), c = onsets(render(s, 10.5, 8));
  CHECK(a == b);
  CHECK(a != c);
  const auto played = std::count(a.begin(), a.end(), true);
  CHECK(played > 8);
  CHECK(played < 32);
  // Live: a second playback in the same link (another transport start) decides anew.
  auto link = makeLink(s, 7);
  const auto first = onsets(render(*link, 10.5));
  const auto second = onsets(render(*link, 10.5));
  CHECK(first != second);
}

TEST_CASE("probe_edges: --render refuses a default length of hours (L5)") {
  namespace fs = std::filesystem;
  Proj p;
  const TrackId t = p.add(TrackKind::Midi, "m");
  std::vector<Note> n{playback_fixture::note(p.store.ids(), 0, kPpq, 0)};
  p.cmd(AddClip{t, playback_fixture::clip(p.store.ids(), Ticks{1} << 40, kPpq, n, "far")});  // ~ 11 h at 120 BPM
  const fs::path dir = fs::temp_directory_path() / "pq_test_render_cap";
  fs::remove_all(dir);
  REQUIRE(saveProject(dir.string(), *p.done()).ok());
  app::CliOptions o;
  o.mode = app::Mode::Render;
  o.project = dir.string();
  o.outPath = (dir / "out.wav").string();
  CHECK(app::runRender(o) == 1);
  CHECK_FALSE(fs::exists(dir / "out.wav"));
  fs::remove_all(dir);
}
