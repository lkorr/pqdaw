// T6.2: plugin-delay compensation — reported latency (graph::pathLatency: the `latency` atom, oversample regions),
// the plan (engine::planCompensation), the reader-owned delay lines of ProjectProgram (impulse alignment through
// groups, sends / returns, nested groups, 64 tracks; fractional oversampling latency within half a sample), a plan
// change while playing (crossfaded), a project without latency (no plan, bit-identical), a merged sidechain unit
// aligned inside, notes and automation shifted by a unit's compensation, RtScope, and per-unit vs whole-project.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/time.h"
#include "pq/engine/automation.h"
#include "pq/engine/latency_plan.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/engine/timeline.h"
#include "pq/graph/infer.h"
#include "pq/graph/latency.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/project.h"

namespace graph = pq::graph;
namespace eng = pq::engine;
using W = graph::WireKind;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;

// The patches every test draws from (one file: they are tracks, not devices).
const char* kPatches = R"(
patch Impulse {
  out out : audio global mono
  out = pulse()
}

patch ImpulseLate {
  param frames = 480 [0..100000]
  out out : audio global mono
  out = latency(pulse(), 480)
}

patch Impulse100 {
  out out : audio global mono
  out = pulse() * 0.25
}

patch ImpulseLate100 {
  out out : audio global mono
  out = latency(pulse(), 100)
}

patch ImpulseSend {
  out out : audio global mono
  out send : audio global mono
  x = pulse()
  out = x
  send = x * 0.5
}

patch Sine {
  out out : audio global mono
  out = sine(phasor(100Hz))
}

patch SineNeg {
  out out : audio global mono
  out = -sine(phasor(100Hz))
}

patch SineOs4 {
  out out : audio global mono
  x = sine(phasor(100Hz))
  oversample(4) {
    out = x * 1.0
  }
}

patch SineLate {
  out out : audio global mono
  out = latency(sine(phasor(100Hz)), 64)
}

patch SineNegLate0 {
  out out : audio global mono
  out = latency(-sine(phasor(100Hz)), 0)
}

patch Sum {
  in  in : audio global mono
  out out : audio global mono
  out = in
}

patch Keyed {
  in  in : audio global mono
  in  key : audio global mono = 0
  out out : audio global mono
  out = in + key
}

patch Inst {
  in  x : audio global mono = 0
  out out : audio global mono
  out = gate_to_real(voice.gate) + x
}

patch Quiet {
  out out : audio global mono
  out = sine(phasor(100Hz)) * 0
}

patch ClipTrack {
  in  x : audio global mono = 0
  out out : audio global mono
  out = channel(clipPlayer(0), 0) + x
}

patch ClipKeyed {
  in  key : audio global mono = 0
  out out : audio global mono
  out = channel(clipPlayer(0), 0) + key
}

patch ImpulseLate4800 {
  out out : audio global mono
  out = latency(pulse(), 4800)
}

patch InstKeyed {
  in  key : audio global mono = 0
  out out : audio global mono
  out = gate_to_real(voice.gate) + key
}
)";

struct Lib {
  graph::PatchFile file;
  const graph::Patch* get(const std::string& name) const {
    for (const graph::Patch& p : file.patches)
      if (p.name() == name) return &p;
    FAIL("no patch " << name);
    return nullptr;
  }
};

const Lib& lib() {
  static const Lib l = [] {
    Lib x;
    auto r = graph::parse(kPatches, "pdc.pqp");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    x.file = std::move(r.value());
    return x;
  }();
  return l;
}

struct G {
  graph::ProjectGraph g;
  uint32_t add(const std::string& patch, graph::TrackRole role = graph::TrackRole::Audio, uint32_t slot = graph::kNone) {
    g.tracks.push_back({static_cast<uint32_t>(g.tracks.size() + 1), patch + std::to_string(g.tracks.size()), lib().get(patch), role, slot});
    return static_cast<uint32_t>(g.tracks.size() - 1);
  }
  void wire(uint32_t from, const char* fp, uint32_t to, const char* tp, W kind = W::Audio) {
    g.wires.push_back({{from, fp}, {to, tp}, kind});
  }
};

eng::ProjectCompileOptions options() {
  eng::ProjectCompileOptions o;
  o.sampleRate = kSr;
  o.maxBlockFrames = kBlock;
  o.voice.maxVoices = 4;
  o.voice.silenceDb = -300.0f;
  return o;
}

// T6.2b: clip slot 0 as the timeline's clip player hands it to the program (ProjectConfig::clipIn / clipActive): unit
// impulses on both channels at the song frames `at`, the slot active only in blocks that have one.
struct ClipFeed {
  std::vector<int64_t> at;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  const float* ptrs[2] = {l.data(), r.data()};
  uint8_t active[1] = {0};
  void fill(int64_t t, uint32_t n) {
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    active[0] = 0;
    for (const int64_t a : at)
      if (a >= t && a < t + n) {
        l[static_cast<size_t>(a - t)] = r[static_cast<size_t>(a - t)] = 1.0f;
        active[0] = 1;
      }
  }
};

struct Rig {
  std::unique_ptr<eng::ProjectCompiler> compiler;
  std::unique_ptr<eng::ProjectProgram> program;
  std::vector<std::unique_ptr<eng::NoteListSource>> notes;
  eng::CompensationPlan plan;
  ClipFeed* clips = nullptr;
  int64_t time = 0;
  uint64_t rtViolations = 0;
};

// Builds and starts the project; `compensate` false: no plan (what main did before T6.2).
std::unique_ptr<Rig> makeRig(const graph::ProjectGraph& g, bool compensate = true, bool whole = false,
                             std::vector<std::vector<eng::TimedNote>> notes = {}, std::vector<int32_t> offset = {},
                             ClipFeed* clips = nullptr, uint32_t eventCapacity = 0) {
  auto r = std::make_unique<Rig>();
  r->clips = clips;
  r->compiler = std::make_unique<eng::ProjectCompiler>(options());
  auto b = whole ? r->compiler->buildWhole(g) : r->compiler->build(g);
  REQUIRE_MESSAGE(b.ok(), (b.ok() ? "" : b.error().message));
  eng::ProjectConfig pc;
  pc.sampleRate = kSr;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 1;
  if (eventCapacity != 0) pc.eventCapacity = eventCapacity;
  if (clips != nullptr) {
    pc.clipIn = clips->ptrs;
    pc.clipActive = clips->active;
    pc.numClipSlots = 1;
  }
  for (auto& n : notes) {
    r->notes.push_back(std::make_unique<eng::NoteListSource>(n));
    pc.eventSources.push_back(r->notes.back()->source());
  }
  auto prog = eng::ProjectProgram::create(pc, b.value().layout);
  REQUIRE(prog.ok());
  r->program = std::move(prog.value());
  for (uint32_t u = 0; u < b.value().images.size(); ++u) REQUIRE(r->program->publish(u, std::move(b.value().images[u])).ok());
  r->plan = eng::planCompensation(b.value().layout, r->compiler->latencies(), offset);
  if (compensate && !r->plan.none()) REQUIRE(r->program->setCompensation(r->plan).ok());
  return r;
}

std::vector<float> render(Rig& r, uint32_t frames) {
  std::vector<float> out;
  std::vector<float> l(kBlock);
  float* outs[1] = {l.data()};
  while (frames > 0) {
    const uint32_t n = std::min(kBlock, frames);
    eng::AudioBlock ab;
    ab.out = outs;
    ab.numOut = 1;
    ab.frames = n;
    ab.frameTime = r.time;
    if (r.clips != nullptr) r.clips->fill(r.time, n);
    r.rtViolations += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      r.program->process(ab);
    });
    out.insert(out.end(), l.begin(), l.begin() + n);
    r.time += n;
    frames -= n;
  }
  return out;
}

// The nonzero samples of x: (index, value).
std::vector<std::pair<size_t, float>> nonzero(const std::vector<float>& x) {
  std::vector<std::pair<size_t, float>> v;
  for (size_t i = 0; i < x.size(); ++i)
    if (x[i] != 0.0f) v.emplace_back(i, x[i]);
  return v;
}

graph::TypedPatch typedOf(const char* text) {
  auto f = graph::parse(text, "t.pqp");
  REQUIRE_MESSAGE(f.ok(), (f.ok() ? "" : f.error().message));
  static std::vector<graph::PatchFile> keep;  // the typed patch points into its source patch
  keep.push_back(std::move(f.value()));
  graph::InferOptions io;
  io.voiceContext = false;
  auto t = graph::infer(keep.back().patches.front(), io);
  REQUIRE_MESSAGE(t.ok(), (t.ok() ? "" : t.error().message));
  graph::OptimiseOptions oo;
  oo.sampleRate = kSr;
  auto o = graph::optimise(std::move(t.value()), oo);
  REQUIRE(o.ok());
  return std::move(o.value());
}

double outLatency(const graph::TypedPatch& t, const char* name = "y") {
  const auto lat = graph::pathLatency(t);
  for (graph::NodeIndex n = 0; n < t.patch.nodeCount(); ++n)
    if (t.patch.node(n).kind == graph::NodeKind::Output && t.patch.nodeName(n) == name) return graph::arrivalLatency(t, lat, n);
  FAIL("no output");
  return -1.0;
}

}  // namespace

TEST_CASE("pdc: patches report the latency of their longest path (latency atom, oversample regions; z1 / delay 0)") {
  CHECK(outLatency(typedOf("patch P {\n in x : audio\n out y : audio\n y = latency(x, 480)\n}\n")) == 480.0);
  // Two parallel paths: the longest counts; constants fold first (2 · 100).
  CHECK(outLatency(typedOf("patch P {\n in x : audio\n out y : audio\n y = latency(x, 2 * 100) + latency(x, 7)\n}\n")) == 200.0);
  // In series they add up.
  CHECK(outLatency(typedOf("patch P {\n in x : audio\n out y : audio\n y = latency(latency(x, 30), 12)\n}\n")) == 42.0);
  // Effects are not latency.
  CHECK(outLatency(typedOf("patch P {\n in x : audio\n out y : audio\n y = delay(z1(x), 10ms, 1s)\n}\n")) == 0.0);
  // An oversample region: the up + down converters' group delay at DC.
  CHECK(outLatency(typedOf("patch P {\n in x : audio\n out y : audio\n oversample(4) {\n  y = tanh(x)\n }\n}\n")) ==
        doctest::Approx(4.74));
  CHECK(outLatency(typedOf("patch P {\n in x : audio\n out y : audio\n oversample(2) {\n  y = tanh(x)\n }\n}\n")) ==
        doctest::Approx(3.62));
  // A latency inside a region counts in the region's samples (/ factor).
  CHECK(outLatency(typedOf("patch P {\n in x : audio\n out y : audio\n oversample(4) {\n  y = latency(x, 40)\n }\n}\n")) ==
        doctest::Approx(4.74 + 10.0));
  // Below half a sample it is a wire (optimise), and reports nothing.
  const graph::TypedPatch wire = typedOf("patch P {\n in x : audio\n out y : audio\n y = latency(x, 0.25)\n}\n");
  CHECK(outLatency(wire) == 0.0);
  for (graph::NodeIndex n = 0; n < wire.patch.nodeCount(); ++n)
    CHECK(!(wire.patch.node(n).kind == graph::NodeKind::Atom && wire.patch.node(n).atom == graph::atomId("latency")));
}

TEST_CASE("pdc: the plan aligns every input of a sum (A_u = max, delays round(A_u - L), totals add up)") {
  eng::ProjectLayout l;
  l.units.resize(4);
  l.busWidth = {1, 1, 1, 1};
  l.busProducer = {0, 1, 2, 3};
  l.outputBus = 3;
  std::vector<eng::UnitLatency> u(4);
  u[0].out = {{0, 480.0}};
  u[1].out = {{1, 0.0}};
  u[2].out = {{2, 3.62}};  // the group oversamples
  u[2].compensated = {0, 1};
  u[3].compensated = {2};
  const eng::CompensationPlan p = eng::planCompensation(l, u);
  CHECK(p.delays[2] == std::vector<eng::InputDelay>{{1, 480}});
  CHECK(p.unitLatency[2] == 480);
  CHECK(p.busLatency[2] == doctest::Approx(483.62));
  CHECK(p.delays[3].empty());
  CHECK(p.unitLatency[3] == 484);
  CHECK(p.total == 484);
  CHECK(!p.none());
  // Modulation reads (not `compensated`) are never delayed; no latency anywhere: no plan.
  std::vector<eng::UnitLatency> z(4);
  z[2].compensated = {0, 1};
  CHECK(eng::planCompensation(l, z).none());
}

TEST_CASE("pdc: two tracks into a group, one with a 480-sample lookahead: aligned at the master (480 total)") {
  G g;
  const uint32_t a = g.add("ImpulseLate"), b = g.add("Impulse"), grp = g.add("Sum", graph::TrackRole::Group),
                 m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", grp, "in");
  g.wire(b, "out", grp, "in");
  g.wire(grp, "out", m, "in");
  g.g.output = {m, "out"};
  auto uncomp = makeRig(g.g, false);
  const auto before = nonzero(render(*uncomp, 2048));
  REQUIRE(before.size() == 2);  // main before T6.2: the two impulses 480 apart
  CHECK(before[0].first == 0);
  CHECK(before[1].first == 480);
  auto r = makeRig(g.g);
  CHECK(r->plan.total == 480);
  CHECK(r->program->outputLatency() == 480);
  const auto after = nonzero(render(*r, 2048));
  REQUIRE(after.size() == 1);
  CHECK(after[0].first == 480);
  CHECK(after[0].second == 2.0f);
  CHECK(r->rtViolations == 0);  // RtScope: runUnit with a delay line, the plan's switch
}

TEST_CASE("pdc: sends into a return, the return and the tracks into the master: aligned") {
  G g;
  const uint32_t a = g.add("ImpulseLate100"), s = g.add("ImpulseSend"), ret = g.add("Sum", graph::TrackRole::Return),
                 m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", m, "in");
  g.wire(s, "out", m, "in");
  g.wire(s, "send", ret, "in");
  g.wire(ret, "out", m, "in");
  g.g.output = {m, "out"};
  auto r = makeRig(g.g);
  CHECK(r->plan.total == 100);
  const auto out = nonzero(render(*r, 1024));
  REQUIRE(out.size() == 1);
  CHECK(out[0].first == 100);
  CHECK(out[0].second == 2.5f);  // a + s + the return's half
}

TEST_CASE("pdc: three nested groups, the late track at the deepest level") {
  G g;
  const uint32_t late = g.add("ImpulseLate100"), x = g.add("Impulse"), y = g.add("Impulse"), z = g.add("Impulse");
  const uint32_t g3 = g.add("Sum", graph::TrackRole::Group), g2 = g.add("Sum", graph::TrackRole::Group),
                 g1 = g.add("Sum", graph::TrackRole::Group), m = g.add("Sum", graph::TrackRole::Master);
  g.wire(late, "out", g3, "in");
  g.wire(x, "out", g3, "in");
  g.wire(g3, "out", g2, "in");
  g.wire(y, "out", g2, "in");
  g.wire(g2, "out", g1, "in");
  g.wire(z, "out", g1, "in");
  g.wire(g1, "out", m, "in");
  g.g.output = {m, "out"};
  auto r = makeRig(g.g);
  CHECK(r->plan.total == 100);
  const auto out = nonzero(render(*r, 1024));
  REQUIRE(out.size() == 1);
  CHECK(out[0].first == 100);
  CHECK(out[0].second == 4.0f);
  // per-unit == whole-project (one unit: the wires into each track aligned at compose time)
  auto whole = makeRig(g.g, true, true);
  const auto w = nonzero(render(*whole, 1024));
  REQUIRE(w.size() == 1);
  CHECK(w[0] == out[0]);
}

TEST_CASE("pdc: 64 tracks, one with latency(x, 480): every impulse lands at 480, master total 480") {
  G g;
  const uint32_t m = g.add("Sum", graph::TrackRole::Master);
  for (uint32_t k = 0; k < 64; ++k) g.wire(g.add(k == 37 ? "ImpulseLate" : "Impulse"), "out", m, "in");
  g.g.output = {m, "out"};
  auto r = makeRig(g.g);
  CHECK(r->plan.total == 480);
  uint32_t delayed = 0;
  for (const auto& d : r->plan.delays) delayed += static_cast<uint32_t>(d.size());
  CHECK(delayed == 63);  // the master's reads of the 63 other tracks
  const auto out = nonzero(render(*r, 2048));
  REQUIRE(out.size() == 1);
  CHECK(out[0].first == 480);
  CHECK(out[0].second == 64.0f);
  CHECK(r->rtViolations == 0);
}

TEST_CASE("pdc: an oversampled track (4x, 4.74 samples) aligned with a plain one within half a sample") {
  // The same 100 Hz sine, once through an oversample(4) region and once inverted: the sum is what is left of the
  // misalignment. Aligned to the nearest sample (5 for 4.74), the residual is ≤ 2π·f/sr · 0.5 of the amplitude.
  G g;
  const uint32_t a = g.add("SineOs4"), b = g.add("SineNeg"), m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", m, "in");
  g.wire(b, "out", m, "in");
  g.g.output = {m, "out"};
  auto r = makeRig(g.g);
  CHECK(r->plan.total == 5);
  auto uncomp = makeRig(g.g, false);
  const std::vector<float> y = render(*r, 9600), y0 = render(*uncomp, 9600);
  double res = 0.0, res0 = 0.0;
  for (size_t i = 4800; i < y.size(); ++i) {
    res = std::max(res, static_cast<double>(std::fabs(y[i])));
    res0 = std::max(res0, static_cast<double>(std::fabs(y0[i])));
  }
  const double bound = 2.0 * 3.14159265358979 * 100.0 / kSr * 0.5;
  CHECK(res <= bound);
  CHECK(res0 > 4.0 * bound);  // without compensation: 4.74 samples apart
}

TEST_CASE("pdc: a project without latency makes no plan and plays bit-identically") {
  G g;
  const uint32_t a = g.add("Sine"), b = g.add("SineNeg"), grp = g.add("Sum", graph::TrackRole::Group),
                 m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", grp, "in");
  g.wire(b, "out", grp, "in");
  g.wire(grp, "out", m, "in");
  g.g.output = {m, "out"};
  auto r = makeRig(g.g);
  CHECK(r->plan.none());
  CHECK(r->plan.total == 0);
  // An empty plan handed over anyway changes nothing either (no line is made).
  REQUIRE(r->program->setCompensation(r->plan).ok());
  auto ref = makeRig(g.g, false);
  CHECK(render(*r, 4096) == render(*ref, 4096));
  CHECK(!r->program->compensating(2, 0));
}

TEST_CASE("pdc: a latency change while playing crossfades the delay (10 ms) and then plays like a fresh build") {
  // Track a gains 64 samples of latency (an edit of its patch: a unit update, then the new plan): the group delays b
  // from then on. After the image's and the delay's 10-ms crossfades, the output equals a project built that way.
  G g;
  const uint32_t a = g.add("Sine"), b = g.add("SineNegLate0"), grp = g.add("Sum", graph::TrackRole::Group),
                 m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", grp, "in");
  g.wire(b, "out", grp, "in");
  g.wire(grp, "out", m, "in");
  g.g.output = {m, "out"};
  auto r = makeRig(g.g);
  CHECK(r->plan.none());
  (void)render(*r, 4800);
  G g2 = g;
  g2.g.tracks[a].patch = lib().get("SineLate");
  auto upd = r->compiler->update(g2.g, std::vector<uint32_t>{a});
  REQUIRE(upd.ok());
  for (auto& u : upd.value())
    if (!u.unchanged) REQUIRE(r->program->publish(u.unit, std::move(u.image)).ok());
  eng::ProjectLayout layout = r->program->layout();
  const eng::CompensationPlan p2 = eng::planCompensation(layout, r->compiler->latencies());
  CHECK(p2.total == 64);
  REQUIRE(r->program->setCompensation(p2).ok());
  const std::vector<float> y = render(*r, 9600);
  CHECK(r->rtViolations == 0);
  auto fresh = makeRig(g2.g);
  (void)render(*fresh, 4800);
  const std::vector<float> want = render(*fresh, 9600);
  // Outside the fades (10 ms = 480 frames, plus the 64-frame delay): ≤ −60 dBFS from the steady project.
  double err = 0.0;
  for (size_t i = 480 + 64 + kBlock; i < y.size(); ++i) err = std::max(err, static_cast<double>(std::fabs(y[i] - want[i])));
  CHECK(err <= 1e-3);
  CHECK(r->program->compensating(2, 1));
}

TEST_CASE("pdc: a merged sidechain unit aligns the audio into its track (the bus input waits for the lookahead)") {
  // a (480 late) feeds c's input and its sidechain key (merging a and c); b, another unit, feeds c's input too.
  G g;
  const uint32_t a = g.add("ImpulseLate"), b = g.add("Impulse"), c = g.add("Keyed"), m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", c, "in");
  g.wire(b, "out", c, "in");
  g.wire(a, "out", c, "key", W::Sidechain);
  g.wire(c, "out", m, "in");
  g.g.output = {m, "out"};
  auto r = makeRig(g.g);
  CHECK(r->plan.total == 480);
  const auto out = nonzero(render(*r, 2048));
  REQUIRE(out.size() == 1);
  CHECK(out[0].first == 480);
  CHECK(out[0].second == 3.0f);
  auto whole = makeRig(g.g, true, true);
  const auto w = nonzero(render(*whole, 2048));
  REQUIRE(w.size() == 1);
  CHECK(w[0] == out[0]);
}

TEST_CASE("pdc: cross-track modulation is not delayed") {
  // The late track drives the key of c through a Modulation wire (merged, never compensated): c's main input is not
  // delayed, the key arrives 480 late.
  G g;
  const uint32_t a = g.add("ImpulseLate"), b = g.add("Impulse"), c = g.add("Keyed"), m = g.add("Sum", graph::TrackRole::Master);
  g.wire(b, "out", c, "in");
  g.wire(a, "out", c, "key", W::Modulation);
  g.wire(c, "out", m, "in");
  g.g.output = {m, "out"};
  auto r = makeRig(g.g);
  CHECK(r->plan.total == 480);  // c's output path still reports the key's latency (its longest path)
  const auto out = nonzero(render(*r, 2048));
  REQUIRE(out.size() == 2);
  CHECK(out[0].first == 0);
  CHECK(out[1].first == 480);
}

TEST_CASE("pdc: a compensated instrument's notes come A_u late") {
  // The instrument reads a 100-late track's audio (mixer cable): its unit's A_u is 100, so its note at 0 sounds at 100,
  // aligned with what it reads.
  G g;
  const uint32_t a = g.add("ImpulseLate100"), inst = g.add("Inst", graph::TrackRole::Instrument, 0),
                 m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", inst, "x");
  g.wire(inst, "out", m, "in");
  g.g.output = {m, "out"};
  const std::vector<std::vector<eng::TimedNote>> notes = {{{0, 400, 220.0f, 0.8f, 0.5f}}};
  auto r = makeRig(g.g, true, false, notes);
  REQUIRE(r->plan.unitLatency.size() == 3);
  CHECK(r->plan.unitLatency[1] == 100);
  const std::vector<float> y = render(*r, 2048);
  size_t onset = y.size();
  for (size_t i = 0; i < y.size() && onset == y.size(); ++i)
    if (y[i] != 0.0f) onset = i;
  CHECK(onset == 100);
  CHECK(y[100] == 2.0f);  // the gate and a's impulse together
  CHECK(r->rtViolations == 0);
}

TEST_CASE("pdc: automation of a compensated unit is evaluated shiftFrames earlier on the song (block rate)") {
  // A step at tick X: the unshifted lane writes 1 from the first block whose frame 0 is at or after X; the shifted one
  // A_u samples later.
  eng::TimelineConfig tc;
  tc.sampleRate = kSr;
  tc.maxBlockFrames = kBlock;
  eng::Timeline tl(tc);
  const pq::Ticks X = pq::kPpq;  // one beat: 24000 samples at 120 BPM
  std::vector<eng::AutomationLaneDesc> lanes(2);
  for (uint32_t l = 0; l < 2; ++l) {
    lanes[l].target = l;
    lanes[l].perSample = false;
    lanes[l].points = {{0, 0.0f, 0.0f}, {X, 0.0f, 0.0f}, {X, 1.0f, 0.0f}};
  }
  lanes[1].shiftFrames = 256;
  auto img = eng::AutomationImage::build(lanes);
  REQUIRE(img.ok());
  REQUIRE(tl.publishAutomation(std::move(img.value())).ok());
  eng::TransportCommand play;
  play.kind = eng::TransportCommandKind::Play;
  play.tick = 0;
  tl.apply(play);
  std::vector<float> params(2, -1.0f);
  int64_t first[2] = {-1, -1};
  for (int64_t t = 0; t < 30000; t += kBlock) {
    tl.process(t, kBlock);
    tl.writeAutomatedParams(params);
    for (uint32_t l = 0; l < 2; ++l)
      if (first[l] < 0 && params[l] == 1.0f) first[l] = t;
  }
  CHECK(first[0] == 24000);
  CHECK(first[1] == 24000 + 256);
}

TEST_CASE("pdc: latency passes through feedback loops (an envelope's z1 before a lookahead and after it)") {
  // The path out of a cycle carries what entered it: latency(…) before a z1 loop and an oversample region after it.
  const char* text = "patch P {\n in x : audio\n out y : audio\n a = latency(x, 32)\n l = z1(l * 0.5 + a)\n"
                     " oversample(2) {\n  y = tanh(l)\n }\n}\n";
  CHECK(outLatency(typedOf(text)) == doctest::Approx(32.0 + 3.62));
}

TEST_CASE("pdc + T6.6: Track Delay as a unit offset: +10 ms plays the track 480 later, -10 ms 480 earlier (others late)") {
  // ProjectLink maps Track Delay d frames to unitOffset = -d (pq/app/project_link.h "Plugin-delay compensation").
  G g;
  const uint32_t a = g.add("Impulse"), b = g.add("Impulse100"), m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", m, "in");
  g.wire(b, "out", m, "in");
  g.g.output = {m, "out"};
  for (const int32_t d : {480, -480}) {
    CAPTURE(d);
    const auto part = graph::partitionProject(g.g);
    REQUIRE(part.ok());
    std::vector<int32_t> offset(part.value().units.size(), 0);
    offset[part.value().unitOf[a]] = -d;
    auto r = makeRig(g.g, true, false, {}, offset);
    const auto hits = nonzero(render(*r, 4096));
    REQUIRE(hits.size() == 2);
    // a's impulse (1) and b's (0.25) are told apart by their values.
    int64_t ta = -1, tb = -1;
    for (const auto& [i, v] : hits) (v == 1.0f ? ta : tb) = static_cast<int64_t>(i);
    CHECK(ta - tb == d);
    CHECK(r->program->outputLatency() == static_cast<uint32_t>(std::max(0, -d)));
    CHECK(r->rtViolations == 0);
  }
}

// ---- T6.2b: the sources of a compensated unit (clip players, notes) and of a merged member; tap automation ----------

TEST_CASE("pdc (T6.2b): the plan delays a unit's clip slots and notes by A_u plus their member's internal shift") {
  eng::ProjectLayout l;
  l.units.resize(3);
  l.busWidth = {1, 1, 1};
  l.busProducer = {0, 1, 2};
  l.outputBus = 2;
  std::vector<eng::UnitLatency> u(3);
  u[0].out = {{0, 100.0}};
  u[0].clips = {{4, 0.0}};      // its own clips: A_0 = 0, nothing to delay
  u[1].out = {{1, 480.0}};
  u[1].compensated = {0};
  u[1].clips = {{5, 0.0}, {6, 380.0}};  // a merged unit: slot 6's member waits 380 for a sidechain inside
  u[1].eventShift = 380.0;
  u[2].compensated = {1};
  const eng::CompensationPlan p = eng::planCompensation(l, u);
  CHECK(p.clipDelays[0].empty());
  CHECK(p.clipDelays[1] == std::vector<eng::SourceDelay>{{5, 100}, {6, 480}});
  CHECK(p.eventDelay[0] == 0);
  CHECK(p.eventDelay[1] == 480);
  CHECK(p.unitLatency[1] == 100);
  CHECK(p.arrival[1] == 100.0);
  CHECK(p.clipDelays[2].empty());
  CHECK(p.total == 580);
  // Only an internal shift (no input bus late): still a plan.
  std::vector<eng::UnitLatency> z(3);
  z[1].clips = {{6, 380.0}};
  const eng::CompensationPlan q = eng::planCompensation(l, z);
  CHECK(!q.none());
  CHECK(q.clipDelays[1] == std::vector<eng::SourceDelay>{{6, 380}});
  CHECK(!q.sameAs(eng::planCompensation(l, std::vector<eng::UnitLatency>(3))));
}

TEST_CASE("pdc (T6.2b): a compensated audio track's own clips play A_u late, aligned with its late cable input") {
  // c plays clip slot 0 and reads a (100 late) through a mixer cable: its A_u is 100, its clip comes 100 late with it.
  G g;
  const uint32_t a = g.add("ImpulseLate100"), c = g.add("ClipTrack"), m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", c, "x");
  g.wire(c, "out", m, "in");
  g.g.output = {m, "out"};
  ClipFeed feed;
  feed.at = {0, 1000};
  auto uncomp = makeRig(g.g, false, false, {}, {}, &feed);
  const auto before = nonzero(render(*uncomp, 2048));
  REQUIRE(before.size() == 3);  // the clip at 0 and 1000, the cable's impulse at 100
  CHECK(before[0].first == 0);
  CHECK(before[1].first == 100);
  feed.at = {0, 1000};
  auto r = makeRig(g.g, true, false, {}, {}, &feed);
  REQUIRE(r->plan.clipDelays.size() == 3);
  CHECK(r->plan.clipDelays[1] == std::vector<eng::SourceDelay>{{0, 100}});
  const auto after = nonzero(render(*r, 2048));
  REQUIRE(after.size() == 2);
  CHECK(after[0].first == 100);
  CHECK(after[0].second == 2.0f);  // the clip and the cable together
  CHECK(after[1].first == 1100);
  CHECK(after[1].second == 1.0f);
  CHECK(r->rtViolations == 0);  // RtScope: a clip delay line and the plan's switch
}

TEST_CASE("pdc (T6.2b): a merged member's own clip waits for a later sidechain inside its unit") {
  // a (480 late) keys c through a sidechain (one merged unit); c's own clip at 0 must meet a's impulse.
  G g;
  const uint32_t a = g.add("ImpulseLate"), c = g.add("ClipKeyed"), m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", c, "key", W::Sidechain);
  g.wire(c, "out", m, "in");
  g.g.output = {m, "out"};
  ClipFeed feed;
  feed.at = {0};
  auto uncomp = makeRig(g.g, false, false, {}, {}, &feed);
  const auto before = nonzero(render(*uncomp, 2048));
  REQUIRE(before.size() == 2);  // main before T6.2b: the clip on time, the key 480 late
  CHECK(before[0].first == 0);
  CHECK(before[1].first == 480);
  auto r = makeRig(g.g, true, false, {}, {}, &feed);
  CHECK(r->plan.total == 480);
  const auto after = nonzero(render(*r, 2048));
  REQUIRE(after.size() == 1);
  CHECK(after[0].first == 480);
  CHECK(after[0].second == 2.0f);
  CHECK(r->rtViolations == 0);
  auto whole = makeRig(g.g, true, true, {}, {}, &feed);
  const auto w = nonzero(render(*whole, 2048));
  REQUIRE(w.size() == 1);
  CHECK(w[0] == after[0]);
}

TEST_CASE("pdc (T6.2b): a merged instrument's notes wait for a later sidechain inside its unit") {
  G g;
  const uint32_t a = g.add("ImpulseLate"), inst = g.add("InstKeyed", graph::TrackRole::Instrument, 0),
                 m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", inst, "key", W::Sidechain);
  g.wire(inst, "out", m, "in");
  g.g.output = {m, "out"};
  const std::vector<std::vector<eng::TimedNote>> notes = {{{0, 1200, 220.0f, 0.8f, 0.5f}}};
  auto onsetOf = [](const std::vector<float>& y) {
    for (size_t i = 0; i < y.size(); ++i)
      if (y[i] != 0.0f) return i;
    return y.size();
  };
  auto uncomp = makeRig(g.g, false, false, notes);
  const std::vector<float> y0 = render(*uncomp, 2048);
  CHECK(onsetOf(y0) == 0);
  CHECK(y0[480] == 2.0f);
  auto r = makeRig(g.g, true, false, notes);
  const std::vector<float> y = render(*r, 2048);
  CHECK(onsetOf(y) == 480);
  CHECK(y[480] == 2.0f);  // the gate and the key's impulse together
  CHECK(y[481] == 1.0f);
  CHECK(r->rtViolations == 0);
}

TEST_CASE("pdc (T6.2b): an audio-rate (tap) lane of a compensated unit is rendered shiftFrames earlier, per sample") {
  // The tuning node's live inputs read a lane's buffer through its tap: the step must land shift samples later, on the
  // exact sample (not at a block boundary).
  eng::TimelineConfig tc;
  tc.sampleRate = kSr;
  tc.maxBlockFrames = kBlock;
  eng::Timeline tl(tc);
  const pq::Ticks X = pq::kPpq;  // 24000 samples at 120 BPM
  const float* taps[2] = {nullptr, nullptr};
  std::vector<eng::AutomationLaneDesc> lanes(2);
  for (uint32_t l = 0; l < 2; ++l) {
    lanes[l].target = l;
    lanes[l].tap = &taps[l];
    lanes[l].points = {{0, 0.0f, 0.0f}, {X, 0.0f, 0.0f}, {X, 1.0f, 0.0f}};
  }
  lanes[1].shiftFrames = 250;  // not a multiple of the block
  auto img = eng::AutomationImage::build(lanes);
  REQUIRE(img.ok());
  REQUIRE(tl.publishAutomation(std::move(img.value())).ok());
  eng::TransportCommand play;
  play.kind = eng::TransportCommandKind::Play;
  play.tick = 0;
  tl.apply(play);
  std::vector<float> params(2, -1.0f);
  int64_t first[2] = {-1, -1};
  uint64_t rt = 0;
  for (int64_t t = 0; t < 30000; t += kBlock) {
    rt += pq::alloc_trap::countRtViolations([&] {
      tl.process(t, kBlock);
      tl.writeAutomatedParams(params);
    });
    for (uint32_t l = 0; l < 2; ++l)
      for (uint32_t i = 0; i < kBlock && first[l] < 0; ++i) {
        const float v = taps[l] != nullptr ? taps[l][i] : params[l];
        if (v == 1.0f) first[l] = t + i;
      }
  }
  CHECK(first[0] == 24000);
  CHECK(first[1] == 24000 + 250);
  CHECK(rt == 0);
}

TEST_CASE("pdc (T6.2b): a delay that outgrows its ring moves to a larger one with its history, mid-fade exact") {
  // The group delays a's bus by 64, then by 100 (the 128-frame ring is replaced by a 192-frame one at the switch, its
  // history copied): the 10-ms crossfade reads the old tap from the copied history, sample for sample.
  G g;
  const uint32_t a = g.add("Sine"), q = g.add("Quiet"), grp = g.add("Sum", graph::TrackRole::Group),
                 m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", grp, "in");
  g.wire(q, "out", grp, "in");
  g.wire(grp, "out", m, "in");
  g.g.output = {m, "out"};
  auto ref = makeRig(g.g, false);
  const std::vector<float> s = render(*ref, 6400);  // a's sine as the master plays it undelayed
  auto r = makeRig(g.g, false);
  const graph::ProjectPartition& part = r->compiler->partition();
  const uint32_t uq = part.unitOf[q];
  auto planWith = [&](double late) {  // q's bus reported `late` frames late: the group delays a's by that much
    std::vector<eng::UnitLatency> lat = r->compiler->latencies();
    for (auto& [bus, frames] : lat[uq].out) frames = late;
    return eng::planCompensation(r->program->layout(), lat);
  };
  REQUIRE(r->program->setCompensation(planWith(64.0)).ok());
  const std::vector<float> y0 = render(*r, 4800);
  for (size_t n = 64; n < y0.size(); ++n) REQUIRE(y0[n] == s[n - 64]);
  REQUIRE(r->program->setCompensation(planWith(100.0)).ok());
  const std::vector<float> y = render(*r, 1600);
  const uint32_t len = 480;  // 10 ms
  const float inv = 1.0f / static_cast<float>(len);
  double err = 0.0;
  for (size_t f = 0; f < y.size(); ++f) {
    const size_t n = 4800 + f;
    const float from = s[n - 64], to = s[n - 100];
    const float want = f + 1 < len ? from + static_cast<float>(f + 1) * inv * (to - from) : to;
    err = std::max(err, static_cast<double>(std::fabs(y[f] - want)));
  }
  CHECK(err <= 1e-6);
  CHECK(r->rtViolations == 0);
}

// ---- M6 review (T6.x-fix-pdc) ---------------------------------------------------------------------------------------

namespace {

// A shifted tap lane against its unshifted twin over a looped playback: the shifted lane at engine frame t must be
// what the unshifted one gave at t - shift (what the unit's delay lines replay then), across every wrap. Returns the
// frames that differ.
size_t shiftedLoopMismatches(uint32_t shift, pq::Ticks loopStart, pq::Ticks loopEnd, pq::Ticks playFrom, int64_t frames,
                             uint64_t& rt) {
  eng::TimelineConfig tc;
  tc.sampleRate = kSr;
  tc.maxBlockFrames = kBlock;
  eng::Timeline tl(tc);
  const float* taps[2] = {nullptr, nullptr};
  std::vector<eng::AutomationLaneDesc> lanes(2);
  for (uint32_t l = 0; l < 2; ++l) {  // a step at the loop start (a fade-in of the loop), then a ramp to its end
    lanes[l].target = l;
    lanes[l].tap = &taps[l];
    lanes[l].points = {{0, 0.0f, 0.0f}, {loopStart, 0.0f, 0.0f}, {loopStart, 0.5f, 0.0f}, {loopEnd, 1.0f, 0.0f}};
  }
  lanes[1].shiftFrames = shift;
  auto img = eng::AutomationImage::build(lanes);
  REQUIRE(img.ok());
  REQUIRE(tl.publishAutomation(std::move(img.value())).ok());
  eng::TransportCommand loop;
  loop.kind = eng::TransportCommandKind::SetLoop;
  loop.loopEnabled = true;
  loop.loopStart = loopStart;
  loop.loopEnd = loopEnd;
  tl.apply(loop);
  eng::TransportCommand play;
  play.kind = eng::TransportCommandKind::Play;
  play.tick = playFrom;
  tl.apply(play);
  std::vector<float> params(2, -1.0f), v0, v1;
  for (int64_t t = 0; t < frames; t += kBlock) {
    rt += pq::alloc_trap::countRtViolations([&] {
      tl.process(t, kBlock);
      tl.writeAutomatedParams(params);
    });
    for (uint32_t i = 0; i < kBlock; ++i) {
      v0.push_back(taps[0] != nullptr ? taps[0][i] : params[0]);
      v1.push_back(taps[1] != nullptr ? taps[1][i] : params[1]);
    }
  }
  REQUIRE(tl.stats().loopWraps >= 3);
  size_t bad = 0;
  for (size_t t = shift; t < v1.size(); ++t) bad += std::fabs(v1[t] - v0[t - shift]) > 1e-6f;
  return bad;
}

}  // namespace

TEST_CASE("pdc (M6 review M1): a shifted lane after a loop wrap replays the end of the previous pass, over every wrap") {
  // Before the fix every wrap rendered the first `shift` frames from the loop start minus the shift: 480 of 480 wrong
  // per wrap.
  uint64_t rt = 0;
  const pq::Ticks S = pq::kPpq, E = 2 * pq::kPpq;  // [24000, 48000) at 120 BPM
  CHECK(shiftedLoopMismatches(480, S, E, S, 4 * 24000 + 4000, rt) == 0);
  CHECK(shiftedLoopMismatches(250, S, E, S, 4 * 24000 + 4000, rt) == 0);  // not a multiple of the block
  CHECK(shiftedLoopMismatches(480, S, E, 0, 5 * 24000 + 4000, rt) == 0);  // from before the loop: a longer first pass
  // A shift longer than the loop (6000 samples): the frames reach back over more than one pass.
  CHECK(shiftedLoopMismatches(7000, S, S + pq::kPpq / 4, S, 8 * 6000, rt) == 0);
  CHECK(shiftedLoopMismatches(7000, S, S + pq::kPpq / 4, 0, 30000 + 8 * 6000, rt) == 0);
  CHECK(rt == 0);
}

namespace {

// Probe C's project: a's sine delayed 64 at the group (q reported 64 late), then the line dropped (q 0 late).
struct DropRig {
  G g;
  uint32_t a = 0, q = 0, grp = 0;
  DropRig() {
    a = g.add("Sine");
    q = g.add("Quiet");
    grp = g.add("Sum", graph::TrackRole::Group);
    const uint32_t m = g.add("Sum", graph::TrackRole::Master);
    g.wire(a, "out", grp, "in");
    g.wire(q, "out", grp, "in");
    g.wire(grp, "out", m, "in");
    g.g.output = {m, "out"};
  }
  static eng::CompensationPlan planWith(Rig& r, uint32_t uq, double late) {
    std::vector<eng::UnitLatency> lat = r.compiler->latencies();
    for (auto& [bus, frames] : lat[uq].out) frames = late;
    return eng::planCompensation(r.program->layout(), lat);
  }
  // `replans` more plans dropping the line again after `after` frames of the fade (0: before the next block).
  std::vector<float> run(int replans, uint32_t after, uint64_t& rt) {
    auto r = makeRig(g.g, false);
    const uint32_t uq = r->compiler->partition().unitOf[q];
    REQUIRE(r->program->setCompensation(planWith(*r, uq, 64.0)).ok());
    (void)render(*r, 4800);
    REQUIRE(r->program->setCompensation(planWith(*r, uq, 0.0)).ok());  // a's line fades 64 -> 0 over 480 frames
    std::vector<float> y = render(*r, after);
    for (int k = 0; k < replans; ++k) REQUIRE(r->program->setCompensation(planWith(*r, uq, 0.0)).ok());
    const std::vector<float> y2 = render(*r, 1024);
    y.insert(y.end(), y2.begin(), y2.end());
    rt += r->rtViolations;
    return y;
  }
};

}  // namespace

TEST_CASE("pdc (M6 review M3): a plan within a dropped line's fade keeps fading it (a Track Delay drag)") {
  // Before the fix the second plan forgot the half-faded line: a step of 0.19 on a full-scale 100 Hz sine (0.048 for
  // the whole fade), or no fade at all when both plans came in one block.
  DropRig d;
  uint64_t rt = 0;
  const std::vector<float> ref = d.run(0, 128, rt);
  CHECK(d.run(1, 128, rt) == ref);  // 128 frames into the fade
  CHECK(d.run(3, 128, rt) == ref);  // three more plans at once
  CHECK(d.run(1, 0, rt) == d.run(0, 0, rt));  // both plans before the next block
  double jump = 0.0;
  for (size_t i = 1; i < ref.size(); ++i) jump = std::max(jump, static_cast<double>(std::fabs(ref[i] - ref[i - 1])));
  CHECK(jump < 0.05);
  CHECK(rt == 0);
}

TEST_CASE("pdc (M6 review M2): 63 delay lines that outgrow their rings copy their history over blocks, exactly") {
  // 64 tracks into the master; track 0's negative Track Delay makes the master delay the other 63 (mono lines). The
  // delay goes from 24000 to 24100: the rings (24064 frames) grow, and the copy of their history is spread over the
  // next blocks while the old lines play. Before the fix one block copied all 63 x 24064 floats, frame by frame (1179
  // us of a 1333 us budget at block 64 for 24000 -> 48000, probe D).
  G g;
  std::vector<uint32_t> tr;
  for (int i = 0; i < 64; ++i) tr.push_back(g.add("Sine"));
  const uint32_t m = g.add("Sum", graph::TrackRole::Master);
  for (uint32_t t : tr) g.wire(t, "out", m, "in");
  g.g.output = {m, "out"};
  auto r = makeRig(g.g, false);
  const graph::ProjectPartition& part = r->compiler->partition();
  auto planWithOffset = [&](int32_t frames) {
    std::vector<int32_t> off(part.units.size(), 0);
    off[part.unitOf[tr[0]]] = frames;
    return eng::planCompensation(r->program->layout(), r->compiler->latencies(), off);
  };
  auto ref = makeRig(g.g, false);  // the same sum without delays: s(n) = 64 sines
  REQUIRE(r->program->setCompensation(planWithOffset(24000)).ok());
  const std::vector<float> y0 = render(*r, 48000);
  const std::vector<float> s = render(*ref, 48000 + 24000);
  auto want = [&](size_t n, size_t delay) { return s[n] / 64.0f + 63.0f * s[n - delay] / 64.0f; };
  for (size_t n = 24064; n < y0.size(); ++n) REQUIRE(std::fabs(y0[n] - want(n, 24000)) < 1e-3f);
  const uint64_t floatsBefore = r->program->stats().lineCopyFloats;
  REQUIRE(r->program->setCompensation(planWithOffset(24100)).ok());
  const std::vector<float> y = render(*r, 24000);
  const eng::ProjectStats& st = r->program->stats();
  CHECK(st.lineCopies == 63);
  CHECK(st.lineCopyFloats - floatsBefore == 63ull * 24064);
  // The work of the worst block: the budget (1024 floats per frame) plus each line's minimum of two blocks' frames.
  CHECK(st.lineCopyBlockMax <= 1024ull * kBlock + 63ull * 2 * kBlock);
  // Every line's history arrived intact: once the copies and the fades are over, the master reads tap 24100 of the
  // copied history exactly where the sum says.
  for (size_t f = 64 * 48; f < y.size(); ++f) REQUIRE(std::fabs(y[f] - want(48000 + f, 24100)) < 1e-3f);
  // And no click while it moved: 64 unit sines step by at most 64 * 2 pi * 100 / 48000 = 0.84 per sample, and the
  // 63 fades from tap 24000 to 24100 (75 degrees apart) add at most 63 * 2 sin(37.5 deg) / 480 = 0.16.
  double jump = 0.0;
  for (size_t f = 1; f < y.size(); ++f) jump = std::max(jump, static_cast<double>(std::fabs(y[f] - y[f - 1])));
  CHECK(jump < 1.0);
  CHECK(r->rtViolations == 0);
}

TEST_CASE("pdc (M6 review M2): new long lines start on their fresh rings without clearing them") {
  // Probe D's first case: 63 new lines of 24000 frames at once (-500 ms). Their rings are zero when made: the block
  // that starts them neither copies nor clears (it cleared 63 x 24064 floats before), and the output is the sum
  // delayed 24000 behind a 10 ms fade from the bus.
  G g;
  std::vector<uint32_t> tr;
  for (int i = 0; i < 64; ++i) tr.push_back(g.add("Sine"));
  const uint32_t m = g.add("Sum", graph::TrackRole::Master);
  for (uint32_t t : tr) g.wire(t, "out", m, "in");
  g.g.output = {m, "out"};
  auto r = makeRig(g.g, false);
  auto ref = makeRig(g.g, false);
  const std::vector<float> s = render(*ref, 4800 + 30000);
  (void)render(*r, 4800);
  std::vector<int32_t> off(r->compiler->partition().units.size(), 0);
  off[r->compiler->partition().unitOf[tr[0]]] = 24000;
  REQUIRE(r->program->setCompensation(eng::planCompensation(r->program->layout(), r->compiler->latencies(), off)).ok());
  const std::vector<float> y = render(*r, 30000);
  CHECK(r->program->stats().lineCopyFloats == 0);
  for (size_t f = 480; f < y.size(); ++f) {
    const size_t n = 4800 + f;
    const float delayed = n >= 4800 + 24000 ? s[n - 24000] : 0.0f;  // (the ring's history starts at the switch)
    REQUIRE(std::fabs(y[f] - (s[n] / 64.0f + 63.0f * delayed / 64.0f)) < 1e-3f);
  }
  CHECK(r->rtViolations == 0);
}

TEST_CASE("pdc (M6 review L7): a topology switch drops the running plan's lines no image of their unit reads") {
  // a and q into the group (a delayed 64: q is reported 64 late). T2 sends a straight to the master; the group's new
  // image no longer reads a's bus, but its old one still did when T2 was published, so the line stays. T3 (no change)
  // is published before any new plan: the line, which would read a bus outside T3's run-order edges, goes at the
  // switch.
  DropRig d;
  auto r = makeRig(d.g.g, false);
  const uint32_t uq = r->compiler->partition().unitOf[d.q];
  REQUIRE(r->program->setCompensation(DropRig::planWith(*r, uq, 64.0)).ok());
  (void)render(*r, 4800);
  graph::ProjectGraph g2 = d.g.g;
  g2.wires[0].to = {g2.output.track, "in"};  // a -> master
  auto t2 = r->compiler->rebuild(g2, {});
  REQUIRE(t2.ok());
  REQUIRE(r->program->publishTopology(t2.value().layout, std::move(t2.value().units)).ok());
  (void)render(*r, 4800);
  const graph::ProjectPartition& part = r->compiler->partition();
  const uint32_t grp = part.unitOf[d.grp], ua = part.unitOf[d.a];
  uint32_t busA = 0;
  while (busA < r->program->layout().busProducer.size() && r->program->layout().busProducer[busA] != ua) ++busA;
  REQUIRE(busA < r->program->numBuses());
  CHECK(r->program->compensating(grp, busA));  // (T1's plan still runs the line: T2's run order has the edge)
  auto t3 = r->compiler->rebuild(g2, {});
  REQUIRE(t3.ok());
  REQUIRE(r->program->publishTopology(t3.value().layout, std::move(t3.value().units)).ok());
  (void)render(*r, 64);
  CHECK_FALSE(r->program->compensating(grp, busA));
  CHECK(r->rtViolations == 0);
}

TEST_CASE("pdc (M6 review L10): a full event delay drops note-ons, never a held note's note-off") {
  // The instrument waits 4800 frames for a latent track; 400 overlapping notes (one every 4 frames, 2000 long)
  // overflow its 256-event delay. Before the fix the offs of held notes were dropped with the rest: voices stuck on.
  G g;
  const uint32_t a = g.add("ImpulseLate4800"), inst = g.add("Inst", graph::TrackRole::Instrument, 0),
                 m = g.add("Sum", graph::TrackRole::Master);
  g.wire(a, "out", inst, "x");
  g.wire(inst, "out", m, "in");
  g.g.output = {m, "out"};
  std::vector<std::vector<eng::TimedNote>> notes(1);
  for (int k = 0; k < 400; ++k) notes[0].push_back({4 * k, 2000, 220.0f, 0.8f, 0.5f});
  auto r = makeRig(g.g, true, false, notes, {}, nullptr, 64);
  const uint32_t ui = r->compiler->partition().unitOf[inst];
  REQUIRE(r->plan.eventDelay[ui] == 4800);
  const std::vector<float> y = render(*r, 4800 + 8000);
  CHECK(r->program->stats().eventDelayDrops > 0);
  CHECK(y[4800 + 10] > 0.0f);  // the notes the delay held sound ...
  // ... and end: nothing sounds once the last off (3596 + 4800) is out.
  CHECK(std::all_of(y.begin() + 8400, y.end(), [](float v) { return v == 0.0f; }));
  CHECK(r->rtViolations == 0);
}
