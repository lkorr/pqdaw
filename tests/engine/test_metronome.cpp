// T6.6: the metronome and the count-in in the engine (pq/engine/metronome.h, Timeline "Count-in"): clicks on the exact
// beat samples across tempo ramps and signature changes (accents on the bars), a click PDC-late with the program's
// output (aligned with a compensated track's impulse), the count-in (1 bar at 120 BPM starts the transport 96 000
// samples after Record at 48 kHz; clicks with the metronome off; Stop cancels it), the idle gate waits for it, the
// click pair, no clicks offline, block-size independence and RtScope cleanliness.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <span>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/time.h"
#include "pq/engine/engine.h"
#include "pq/engine/latency_plan.h"
#include "pq/engine/metronome.h"
#include "pq/engine/offline_render.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/engine/sequence.h"
#include "pq/graph/parse.h"

namespace graph = pq::graph;
namespace e = pq::engine;
using pq::kPpq;
using pq::Ticks;

namespace {

constexpr double kSr = 48000.0;

const char* kPatches = R"(
patch Impulse {
  out out : audio global mono
  out = pulse()
}

patch ImpulseLate {
  out out : audio global mono
  out = latency(pulse(), 480)
}

patch Sum {
  in  in : audio global mono
  out out : audio global mono
  out = in
}

patch Inst {
  out out : audio global mono
  out = gate_to_real(voice.gate)
}
)";

const graph::PatchFile& patches() {
  static const graph::PatchFile f = [] {
    auto r = graph::parse(kPatches, "metronome.pqp");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return std::move(r.value());
  }();
  return f;
}

const graph::Patch* patch(const std::string& name) {
  for (const graph::Patch& p : patches().patches)
    if (p.name() == name) return &p;
  FAIL("no patch " << name);
  return nullptr;
}

struct G {
  graph::ProjectGraph g;
  uint32_t add(const std::string& name, graph::TrackRole role = graph::TrackRole::Audio, uint32_t slot = graph::kNone) {
    g.tracks.push_back({static_cast<uint32_t>(g.tracks.size() + 1), name + std::to_string(g.tracks.size()), patch(name), role, slot});
    return static_cast<uint32_t>(g.tracks.size() - 1);
  }
  void wire(uint32_t from, uint32_t to) { g.wires.push_back({{from, "out"}, {to, "in"}, graph::WireKind::Audio}); }
};

// The engine with a compiled project; run() drives it like a device with a click pair (channels 2, 3) unless
// clickPair is false (then the click is in the main pair). main / click collect channel 0 / channel 2.
struct Rig {
  std::unique_ptr<e::Engine> en;
  std::unique_ptr<e::ProjectCompiler> compiler;
  std::vector<float> main, click;
  int64_t time = 0;
  uint64_t rtViolations = 0;
  bool clickPair = true;

  void run(uint64_t frames, uint32_t block = 64) {
    std::vector<float> ch[4];
    for (auto& c : ch) c.assign(block, 0.0f);
    float* outs[4] = {ch[0].data(), ch[1].data(), ch[2].data(), ch[3].data()};
    while (frames > 0) {
      const auto n = static_cast<uint32_t>(std::min<uint64_t>(block, frames));
      e::AudioBlock b{nullptr, 0, outs, clickPair ? 4u : 2u, n, time};
      rtViolations += pq::alloc_trap::countRtViolations([&] {
        pq::DenormalGuard dg;
        en->setClickPair(clickPair);
        en->process(b);
      });
      main.insert(main.end(), ch[0].begin(), ch[0].begin() + n);
      click.insert(click.end(), (clickPair ? ch[2] : ch[0]).begin(), (clickPair ? ch[2] : ch[0]).begin() + n);
      time += n;
      frames -= n;
    }
  }
  void post(e::TransportCommandKind k, Ticks tick = 0, uint8_t countIn = 0) {
    e::TransportCommand c;
    c.kind = k;
    c.tick = tick;
    c.countInBars = countIn;
    REQUIRE(en->post(c));
  }
};

std::unique_ptr<Rig> makeRig(const graph::ProjectGraph& g, float sleepMs = 0.0f, uint32_t blockFrames = 64) {
  auto r = std::make_unique<Rig>();
  e::EngineConfig ec;
  ec.sampleRate = kSr;
  ec.blockFrames = blockFrames;
  ec.numOut = 2;
  ec.maxSlots = 4;
  ec.sleepAfterMs = sleepMs;
  auto en = e::Engine::create(ec);
  REQUIRE(en.ok());
  r->en = std::move(en.value());
  e::ProjectCompileOptions o;
  o.sampleRate = kSr;
  o.maxBlockFrames = blockFrames;
  o.voice.maxVoices = 4;
  o.voice.silenceDb = -300.0f;
  r->compiler = std::make_unique<e::ProjectCompiler>(o);
  auto b = r->compiler->build(g);
  REQUIRE_MESSAGE(b.ok(), (b.ok() ? "" : b.error().message));
  auto prog = e::ProjectProgram::create(r->en->projectConfig(), b.value().layout);
  REQUIRE(prog.ok());
  for (uint32_t u = 0; u < b.value().images.size(); ++u) {
    auto pub = prog.value()->publish(u, std::move(b.value().images[u]));
    REQUIRE_MESSAGE(pub.ok(), (pub.ok() ? "" : pub.error().message));
  }
  const e::CompensationPlan plan = e::planCompensation(b.value().layout, r->compiler->latencies());
  if (!plan.none()) REQUIRE(prog.value()->setCompensation(plan).ok());
  r->en->setProgram(std::move(prog.value()));
  r->en->metronome().setLevelDb(0.0f);
  return r;
}

graph::ProjectGraph silentProject() {  // an instrument without notes into the master
  G g;
  const uint32_t inst = g.add("Inst", graph::TrackRole::Instrument, 0), m = g.add("Sum", graph::TrackRole::Master);
  g.wire(inst, m);
  g.g.output = {m, "out"};
  return g.g;
}

struct ClickAt {
  int64_t frame;
  bool accent;
};

// The clicks in x: a click starts with sin(0) = 0, so its first frame is the zero before the first nonzero sample;
// its second sample tells the accent (2 kHz, full level) from a beat (1 kHz, 0.7).
std::vector<ClickAt> clicks(const std::vector<float>& x) {
  std::vector<ClickAt> v;
  const auto len = static_cast<int64_t>(std::lround(e::Metronome::kClickSeconds * kSr));
  for (int64_t i = 1; i < static_cast<int64_t>(x.size());) {
    if (x[static_cast<size_t>(i)] != 0.0f && x[static_cast<size_t>(i - 1)] == 0.0f) {
      v.push_back({i - 1, std::fabs(x[static_cast<size_t>(i)]) > 0.2f});
      i += len;
      continue;
    }
    ++i;
  }
  return v;
}

std::unique_ptr<e::TempoImage> tempo(std::vector<pq::TempoPoint> pts, std::vector<pq::TimeSigChange> sigs = {}) {
  auto r = e::TempoImage::create(pts, sigs);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
  return std::move(r.value());
}

}  // namespace

TEST_CASE("metronome: off costs nothing and clicks nothing; on clicks on every beat with an accent on the bar") {
  auto r = makeRig(silentProject());
  r->post(e::TransportCommandKind::Play);
  r->run(48000);
  CHECK(clicks(r->click).empty());
  CHECK(r->en->metronome().stats().clicks == 0);
  r->en->metronome().setEnabled(true);
  r->post(e::TransportCommandKind::Play);  // from 0 again, at engine time 48000
  r->click.clear();
  r->run(96000 * 2);
  const auto c = clicks(r->click);
  REQUIRE(c.size() == 8);  // 4 s at 120 BPM
  for (size_t k = 0; k < c.size(); ++k) {
    CHECK(c[k].frame == static_cast<int64_t>(k) * 24000);
    CHECK(c[k].accent == (k % 4 == 0));
  }
  CHECK(r->rtViolations == 0);
}

TEST_CASE("metronome: clicks on the exact beat samples across a tempo ramp and signature changes") {
  auto r = makeRig(silentProject());
  // 100 BPM ramping linearly to 173 over 6 bars, then a step to 97; 4/4, from bar 2 3/4, from bar 4 7/8, from bar 6
  // 5/16.
  const std::vector<pq::TempoPoint> pts = {{0, 100.0, pq::TempoCurve::Linear}, {6 * 4 * kPpq, 173.0, pq::TempoCurve::Step},
                                           {7 * 4 * kPpq, 97.0, pq::TempoCurve::Step}};
  const std::vector<pq::TimeSigChange> sigs = {{0, {4, 4}}, {2, {3, 4}}, {4, {7, 8}}, {6, {5, 16}}};
  auto img = tempo(pts, sigs);
  const pq::TempoMap tm = img->tempo();
  const pq::TimeSigMap sm = img->signatures();
  REQUIRE(r->en->timeline().publishTempo(std::move(img)).ok());
  r->en->metronome().setEnabled(true);
  r->post(e::TransportCommandKind::Play);
  const pq::Ticks end = 9 * 4 * kPpq;
  const int64_t frames = tm.ticksToSamples(end, kSr);
  r->run(static_cast<uint64_t>(frames), 37);  // an odd device block: beats land anywhere in a block
  // Expected: every beat of the signature map before `end`.
  std::vector<ClickAt> want;
  for (int64_t bar = 0;; ++bar) {
    const pq::Ticks b0 = sm.barToTicks(bar);
    if (b0 >= end) break;
    const pq::TimeSignature sig = sm.signatureAtBar(bar);
    for (int32_t k = 0; k < sig.numerator; ++k) {
      const pq::Ticks t = b0 + k * pq::ticksPerBeat(sig);
      if (tm.ticksToSamples(t, kSr) + static_cast<int64_t>(std::lround(e::Metronome::kClickSeconds * kSr)) < frames)
        want.push_back({tm.ticksToSamples(t, kSr), k == 0});
    }
  }
  const auto got = clicks(r->click);
  REQUIRE(want.size() > 40);
  // Sixteenths at 173 BPM are 52 ms apart: clicks never overlap here, so every one is found.
  REQUIRE(got.size() >= want.size());
  for (size_t i = 0; i < want.size(); ++i) {
    CHECK(got[i].frame == want[i].frame);
    CHECK(got[i].accent == want[i].accent);
  }
  CHECK(r->rtViolations == 0);
}

TEST_CASE("metronome: with PDC 480 the click is 480 later, together with a compensated instrument's note") {
  // A 480-late track and an instrument (its gate is its output) into the master: the instrument is delayed 480 at the
  // master, so its note on beat 0 is heard 480 after the beat; so is the click.
  G g;
  const uint32_t late = g.add("ImpulseLate"), inst = g.add("Inst", graph::TrackRole::Instrument, 0),
                 m = g.add("Sum", graph::TrackRole::Master);
  g.wire(late, m);
  g.wire(inst, m);
  g.g.output = {m, "out"};
  auto r = makeRig(g.g);
  r->run(64);  // the plan is taken in the program's first block (after its block hook): play once it is in
  REQUIRE(r->en->program()->outputLatency() == 480);
  e::NoteDesc n;
  n.length = kPpq;
  n.id = 1;
  e::ClipDesc clip;
  clip.notes = {n};
  const std::vector<e::TrackSequenceDesc> tracks = {e::TrackSequenceDesc{1, {clip}}};
    auto seq = e::SequenceImage::build(tracks, {});
  REQUIRE(seq.ok());
  REQUIRE(r->en->timeline().publishSequence(std::move(seq.value())).ok());
  r->en->metronome().setEnabled(true);
  r->post(e::TransportCommandKind::Play);  // beat 0 = engine time 64
  r->run(4096);
  CHECK(r->main[480] == 1.0f);  // the late track's pulse (engine time 0), heard at 480
  size_t onset = r->main.size();
  for (size_t i = 481; i < r->main.size() && onset == r->main.size(); ++i)
    if (r->main[i] != 0.0f) onset = i;
  CHECK(onset == 64 + 480);  // the note: beat 0 + the master's PDC total
  const auto c = clicks(r->click);
  REQUIRE(c.size() == 1);
  CHECK(c[0].frame == 64 + 480);
  CHECK(c[0].accent);
  // Without latency the click is on the beat itself.
  auto r3 = makeRig(silentProject());
  r3->run(64);
  r3->en->metronome().setEnabled(true);
  r3->post(e::TransportCommandKind::Play);
  r3->run(2048);
  const auto c3 = clicks(r3->click);
  REQUIRE(c3.size() == 1);
  CHECK(c3[0].frame == 64);
  CHECK(r->rtViolations == 0);
}

TEST_CASE("count-in: 1 bar at 120 BPM starts the transport exactly 96 000 samples after Record @48k") {
  for (const uint32_t block : {64u, 37u, 1000u}) {
    CAPTURE(block);
    // An instrument plays a note at tick 0 (its gate is the output): the note starts where the count-in ends.
    G g;
    const uint32_t inst = g.add("Inst", graph::TrackRole::Instrument, 0), m = g.add("Sum", graph::TrackRole::Master);
    g.wire(inst, m);
    g.g.output = {m, "out"};
    auto r = makeRig(g.g);
    e::NoteDesc n;
    n.start = 0;
    n.length = kPpq;
    n.id = 1;
    e::ClipDesc clip;
    clip.notes = {n};
    const std::vector<e::TrackSequenceDesc> tracks = {e::TrackSequenceDesc{1, {clip}}};
    auto seq = e::SequenceImage::build(tracks, {});
    REQUIRE(seq.ok());
    REQUIRE(r->en->timeline().publishSequence(std::move(seq.value())).ok());
    r->run(100);  // some engine time first: Record acts at the next block's frame 0
    const int64_t at = r->time;
    r->post(e::TransportCommandKind::Record, 0, 1);
    r->run(96000 + 48000, block);
    CHECK(r->en->timeline().stats().countIns == 1);
    // The gate (the note) starts at exactly at + 96 000.
    size_t onset = r->main.size();
    for (size_t i = 0; i < r->main.size() && onset == r->main.size(); ++i)
      if (r->main[i] != 0.0f) onset = i;
    CHECK(static_cast<int64_t>(onset) == at + 96000);
    // 4 count-in clicks, the metronome off: at, +24000, … (accent on the first); none after the start.
    const auto c = clicks(r->click);
    REQUIRE(c.size() == 4);
    for (size_t k = 0; k < 4; ++k) {
      CHECK(c[k].frame == at + static_cast<int64_t>(k) * 24000);
      CHECK(c[k].accent == (k == 0));
    }
    CHECK(r->en->timeline().playing());
    CHECK(r->en->timeline().songSample() == r->time - (at + 96000));
    CHECK(r->rtViolations == 0);
  }
}

TEST_CASE("count-in: with the metronome on the song's clicks follow the count-in's without a gap; Stop cancels") {
  auto r = makeRig(silentProject());
  r->en->metronome().setEnabled(true);
  r->post(e::TransportCommandKind::Record, 0, 2);
  r->run(2 * 96000 + 48000);
  const auto c = clicks(r->click);
  REQUIRE(c.size() == 10);  // 8 count-in beats, then song beats 0 and 1
  for (size_t k = 0; k < c.size(); ++k) {
    CHECK(c[k].frame == static_cast<int64_t>(k) * 24000);
    CHECK(c[k].accent == (k % 4 == 0));
  }
  // Stop during a count-in: the transport stays where it would have started, nothing plays.
  r->post(e::TransportCommandKind::Record, 4 * kPpq, 1);
  r->run(64);
  CHECK(r->en->timeline().countingIn());
  r->post(e::TransportCommandKind::Stop);
  r->run(64);
  CHECK_FALSE(r->en->timeline().countingIn());
  CHECK_FALSE(r->en->timeline().playing());
  CHECK(r->en->timeline().position() == 4 * kPpq);
  r->click.clear();
  r->run(96000);
  CHECK(clicks(r->click).empty());
  // Record without a count-in plays at once (recording itself is T6.7).
  r->post(e::TransportCommandKind::Record, 0, 0);
  r->run(64);
  CHECK(r->en->timeline().playing());
}

TEST_CASE("count-in: the idle gate stays awake through a silent count-in; clicks sound on the main pair") {
  auto r = makeRig(silentProject(), 20.0f);
  r->clickPair = false;
  r->run(48000);
  REQUIRE(r->en->asleep());
  r->post(e::TransportCommandKind::Record, 0, 1);  // wakes it
  r->click.clear();
  r->run(96000 + 4800);
  CHECK(r->en->timeline().playing());
  CHECK(clicks(r->click).size() == 4);
}

TEST_CASE("metronome: never in an offline render") {
  auto r = makeRig(silentProject());
  r->en->metronome().setEnabled(true);
  r->post(e::TransportCommandKind::Play);
  e::BufferSink sink;
  auto st = r->en->renderOffline(96000, sink);
  REQUIRE(st.ok());
  float peak = 0.0f;
  for (const auto& ch : sink.channels)
    for (float x : ch) peak = std::max(peak, std::fabs(x));
  CHECK(peak == 0.0f);
  CHECK(r->en->metronome().stats().clicks == 0);
}

TEST_CASE("metronome: cost while a click sounds (render + schedule per 64-frame block)") {
  e::Metronome m(kSr);
  m.setEnabled(true);
  m.setLevelDb(-6.0f);
  auto img = tempo({{0, 120.0, pq::TempoCurve::Step}});
  std::vector<float> l(64), r(64);
  float* outs[2] = {l.data(), r.data()};
  const e::CountInState none;
  const auto t0 = std::chrono::steady_clock::now();
  constexpr int kBlocks = 200000;
  int64_t time = 0;
  for (int b = 0; b < kBlocks; ++b) {
    const e::TransportRun run{0, 64, time, e::TransportRun::Song};
    m.schedule(time, std::span<const e::TransportRun>(&run, 1), *img, none, kSr, 0, true);
    m.render(time, 64, outs, 2);
    time += 64;
  }
  const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kBlocks;
  MESSAGE("metronome: " << ns << " ns per 64-frame block on average (a click sounds in 3 % of the blocks)");
  CHECK(m.stats().clicks == static_cast<uint64_t>(kBlocks * 64 / 24000 + 1));
  CHECK(ns < 1000.0);
}
