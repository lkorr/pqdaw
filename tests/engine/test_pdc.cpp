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

struct Rig {
  std::unique_ptr<eng::ProjectCompiler> compiler;
  std::unique_ptr<eng::ProjectProgram> program;
  std::vector<std::unique_ptr<eng::NoteListSource>> notes;
  eng::CompensationPlan plan;
  int64_t time = 0;
  uint64_t rtViolations = 0;
};

// Builds and starts the project; `compensate` false: no plan (what main did before T6.2).
std::unique_ptr<Rig> makeRig(const graph::ProjectGraph& g, bool compensate = true, bool whole = false,
                             std::vector<std::vector<eng::TimedNote>> notes = {}) {
  auto r = std::make_unique<Rig>();
  r->compiler = std::make_unique<eng::ProjectCompiler>(options());
  auto b = whole ? r->compiler->buildWhole(g) : r->compiler->build(g);
  REQUIRE_MESSAGE(b.ok(), (b.ok() ? "" : b.error().message));
  eng::ProjectConfig pc;
  pc.sampleRate = kSr;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 1;
  for (auto& n : notes) {
    r->notes.push_back(std::make_unique<eng::NoteListSource>(n));
    pc.eventSources.push_back(r->notes.back()->source());
  }
  auto prog = eng::ProjectProgram::create(pc, b.value().layout);
  REQUIRE(prog.ok());
  r->program = std::move(prog.value());
  for (uint32_t u = 0; u < b.value().images.size(); ++u) REQUIRE(r->program->publish(u, std::move(b.value().images[u])).ok());
  r->plan = eng::planCompensation(b.value().layout, r->compiler->latencies());
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
