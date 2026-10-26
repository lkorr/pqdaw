// T1.x-silence (SPEC §7.7): the scheduler's sleep plan (pq/graph/schedule.h "Sleep plan"): which regions of the global
// stages may sleep, grouped into segments with their sources, states, waking params and fills.
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"

using namespace pq::graph;

namespace {

struct Compiled {
  std::string text;
  ParseResult parsed;
  PatchLibrary lib;
  TypedPatch typed;
  Schedule sched;
};

void compile(Compiled& c, std::string_view src, bool sleep = true, bool voiceContext = false) {
  c.text = std::string(src);
  c.parsed = parseDetailed(c.text, "test.pqp");
  REQUIRE(c.parsed.ok());
  c.lib.add(c.parsed.file);
  InferOptions io;
  io.library = &c.lib;
  io.voiceContext = voiceContext;
  InferResult inf = inferDetailed(c.parsed.file.patches[0], io);
  REQUIRE(inf.ok());
  OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  auto opt = optimise(std::move(inf.typed), oo);
  REQUIRE(opt.ok());
  c.typed = std::move(opt.value());
  ScheduleOptions so;
  so.sleep = sleep;
  ScheduleResult r = scheduleDetailed(c.typed, so);
  REQUIRE(r.ok());
  c.sched = std::move(r.schedule);
}

uint32_t sleepingRegions(const Schedule& s) {
  uint32_t n = 0;
  for (const RegionSleep& r : s.sleep.regions) n += r.segment != kNone ? 1u : 0u;
  return n;
}

std::vector<SleepStateKind> kinds(const Schedule& s, const SleepSegment& g) {
  std::vector<SleepStateKind> k;
  for (uint32_t i = g.firstState; i < g.firstState + g.numStates; ++i) k.push_back(s.sleep.states[i].kind);
  std::sort(k.begin(), k.end());
  return k;
}

}  // namespace

TEST_CASE("sleep plan: a feedback delay sleeps as one segment fed by its input (T1.x-silence)") {
  Compiled c;
  compile(c, R"(
patch Echo {
  in    x : audio
  param fb = 0.5 [0..0.95]
  out   y : audio
  d = delay(x + d * fb, 10ms, 100ms)
  y = x + d * 0.3
}
)");
  const Schedule& s = c.sched;
  REQUIRE(s.sleep.segments.size() == 1);
  const SleepSegment& g = s.sleep.segments[0];
  CHECK(g.feedback);
  CHECK(g.numSources == 1);  // the root input
  CHECK(s.sleep.sources[g.firstSource].kind == Operand::Kind::External);
  REQUIRE(g.numStates == 1);
  CHECK(s.sleep.states[g.firstState].kind == SleepStateKind::Delay);
  CHECK(s.sleep.states[g.firstState].tail == 4800 + 3);
  CHECK(g.tailFrames == 4803);
  CHECK(g.numParams == 0);  // a coefficient (fb) does not wake it
  CHECK(s.sleep.dormantCapable);
  // Every Block / Loop region sleeps; the one read by the output fills its buffer.
  uint32_t fills = 0;
  for (uint32_t r = 0; r < s.regions.size(); ++r) {
    if (s.regions[r].kind == RegionKind::Control) {
      CHECK(s.sleep.regions[r].segment == kNone);
      continue;
    }
    CHECK(s.sleep.regions[r].segment == 0);
    fills += s.sleep.regions[r].numFills;
  }
  CHECK(fills == 1);
}

TEST_CASE("sleep plan: generators and offsets keep a region awake (T1.x-silence)") {
  {
    Compiled c;  // an LFO inside the region: the phasor's state moves on its own
    compile(c, R"(
patch Trem {
  in    x : audio
  out   y : audio
  y = x * (0.5 + 0.5 * sine(phasor(5Hz)))
}
)");
    CHECK(sleepingRegions(c.sched) == 0);
    CHECK_FALSE(c.sched.sleep.dormantCapable);
  }
  {
    Compiled c;  // an output that is not 0 at silence
    compile(c, R"(
patch Offset {
  in    x : audio
  out   y : audio
  y = z1(x) + 1
}
)");
    CHECK(sleepingRegions(c.sched) == 0);
  }
  {
    Compiled c;  // sleep switched off: no plan
    compile(c, "patch P {\n  in x : audio\n  out y : audio\n  y = z1(x) * 0.5\n}\n", false);
    CHECK(c.sched.sleep.segments.empty());
    CHECK(c.sched.sleep.regions.empty());
  }
}

TEST_CASE("sleep plan: smoothed gains and de-zippered coefficients wake on their params (T1.x-silence)") {
  {
    Compiled c;
    compile(c, R"(
patch Fader {
  in    x : audio
  param vol = 0.5 [0..1]
  out   y : audio
  y = gain(x, vol, 2ms)
}
)");
    REQUIRE(c.sched.sleep.segments.size() == 1);
    const SleepSegment& g = c.sched.sleep.segments[0];
    CHECK(kinds(c.sched, g) == std::vector<SleepStateKind>{SleepStateKind::Smoother});
    CHECK(g.numParams == 1);
  }
  {
    Compiled c;  // Utility's smoothing form: the distance to a param-rate target decays, x − z1(x) is 0 once settled
    compile(c, R"(
patch Smooth {
  in    x : audio
  param a = 0.5 [0..1]
  out   y : audio
  k  = 0.97
  dd = z1(dn)
  dn = dd * k + (z1(a) - a) * k
  y  = x * (a + dn)
}
)");
    REQUIRE(c.sched.sleep.segments.size() == 1);
    const SleepSegment& g = c.sched.sleep.segments[0];
    const std::vector<SleepStateKind> k = kinds(c.sched, g);
    CHECK(std::count(k.begin(), k.end(), SleepStateKind::Memory) == 1);   // dd
    CHECK(std::count(k.begin(), k.end(), SleepStateKind::Settled) == 1);  // z1(a)
    CHECK(g.numParams == 1);
    CHECK(c.sched.sleep.dormantCapable);
  }
}

TEST_CASE("sleep plan: the voice tail is the voice stage's longest delay line (T1.x-silence)") {
  Compiled c;
  compile(c, R"(
patch Pluck {
  out y : audio voice mono
  e = delay(voice.gate ? 1.0 : 0.0, 20ms, 50ms)
  y = e * 0.5 + z1(e) * 0.1
}
)", true, true);
  CHECK(c.sched.sleep.voiceTailFrames == 2400 + 3);
  Compiled off;
  compile(off, R"(
patch Pluck {
  out y : audio voice mono
  e = delay(voice.gate ? 1.0 : 0.0, 20ms, 50ms)
  y = e * 0.5
}
)", false, true);
  CHECK(off.sched.sleep.voiceTailFrames == 2403);  // kept with sleep off: voice end does not depend on the switch
}

TEST_CASE("sleep plan: values the solver reads count as read outside every segment (T1.x-silence)") {
  // 1.5 + x·0.2 is not 0 at silence and only the solver reads it (the equation's side): its region must run.
  Compiled c;
  compile(c, R"(
patch Solved {
  in x : audio
  out a : audio
  solve(iterations: 4) {
    unknown g = 0
    g * 0.5 + g * g * 0.1 == 1.5 + x * 0.2
  }
  a = g
}
)");
  CHECK(sleepingRegions(c.sched) == 0);
  CHECK_FALSE(c.sched.sleep.dormantCapable);  // the unknown's guess is global state
}

