// T4.5: live probes through the compiler (pq/graph/inspect.h) — locating a written node's output in the flattened patch
// (InferOptions::probes), following it through the optimiser (OptimiseOptions::valueMap), keeping it readable in the
// schedule (ScheduleOptions::probes) without changing anything else — and the cost weights.

#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <string>
#include <vector>

#include "pq/graph/infer.h"
#include "pq/graph/inspect.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "text_util.h"

using namespace pq::graph;

namespace {

const char* kText = R"(
patch Top {
  in x : audio
  param k = 0.5 [0..1]
  out y : audio
  out z : audio
  a = x * k
  f = Filt(a, 0.25)
  b = x * k
  y = f.lo + b
  m = x * 3
  w = m + 1
  z = w
}

patch Filt {
  in s : audio
  param c = 0.1
  out lo : audio
  out hi : audio
  t = z1(s) * c
  l = t + s
  h = s - t
  lo = l
  hi = h
}
)";

struct Compiled {
  ParseResult parsed;
  PatchLibrary lib;
  InferResult inf;
  TypedPatch opt;
  std::vector<uint32_t> valueMap;
  std::vector<ProbePoint> points;
  Schedule sched;
};

NodeId idOf(const Patch& p, std::string_view name) {
  for (NodeIndex n = 0; n < p.nodeCount(); ++n)
    if (p.nodeName(n) == name) return p.node(n).id;
  FAIL("no node " << name);
  return {};
}

void compile(Compiled& c, std::span<const ProbeTarget> targets, std::string_view text = kText) {
  c.parsed = parseDetailed(text, "test.pqp");
  REQUIRE(c.parsed.ok());
  c.lib.add(c.parsed.file);
  InferOptions io;
  io.library = &c.lib;
  io.probes = targets;
  c.inf = inferDetailed(c.parsed.file.patches[0], io);
  REQUIRE(c.inf.ok());
  REQUIRE(c.inf.probeSources.size() == targets.size());
  OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  oo.valueMap = &c.valueMap;
  const std::vector<uint32_t> sources = c.inf.probeSources;
  auto opt = optimise(c.inf.typed, oo);
  REQUIRE(opt.ok());
  c.opt = std::move(opt.value());
  c.points = probePoints(c.opt.patch, sources, c.valueMap);
  ScheduleOptions so;
  so.sampleRate = 48000.0;
  so.probes = c.points;
  auto s = schedule(c.opt, so);
  REQUIRE(s.ok());
  c.sched = std::move(s.value());
}

// The flattened (inferred) node a source port belongs to.
std::string inferredName(const Compiled& c, uint32_t port) {
  const Patch& p = c.inf.typed.patch;
  for (NodeIndex n = 0; n < p.nodeCount(); ++n)
    for (uint32_t o = 0; o < p.node(n).numOutputs; ++o)
      if (p.flatOutput(n, o) == port) return p.nodeName(n);
  return "?";
}

}  // namespace

TEST_CASE("probe: infer locates written outputs through subcircuits and pass-throughs (T4.5)") {
  ParseResult pr = parseDetailed(kText, "test.pqp");
  REQUIRE(pr.ok());
  const Patch& top = pr.file.patches[0];
  const Patch& filt = pr.file.patches[1];
  std::vector<ProbeTarget> t(7);
  t[0].node = idOf(top, "a");                          // a root atom
  t[1].node = idOf(top, "f");                          // a call's first output: the callee's `lo` (= t + s)
  t[2].node = idOf(top, "f");
  t[2].output = 1;                                     // … its second: `hi`
  t[3].path = {idOf(top, "f")};                        // inside the call:
  t[3].node = idOf(filt, "t");                         //   an atom of the instance
  t[4].path = {idOf(top, "f")};
  t[4].node = idOf(filt, "s");                         //   the callee's `in`: the caller's argument a
  t[5].node = idOf(top, "k");                          // a root param
  t[6].path = {NodeId{987654}};                        // no such call
  t[6].node = idOf(filt, "t");
  Compiled c;
  compile(c, t);
  const std::vector<uint32_t>& src = c.inf.probeSources;
  CHECK(inferredName(c, src[0]) == "a");
  CHECK(inferredName(c, src[1]) == "f__l");
  CHECK(inferredName(c, src[2]) == "f__h");
  CHECK(inferredName(c, src[3]) == "f__t");
  CHECK(src[4] == src[0]);  // the pass-through is the argument's value
  CHECK(inferredName(c, src[5]) == "k");
  CHECK(src[6] == kNone);
}

TEST_CASE("probe: the optimiser's value map follows CSE, keeps survivors, and drops fused values (T4.5)") {
  ParseResult pr = parseDetailed(kText, "test.pqp");
  REQUIRE(pr.ok());
  const Patch& top = pr.file.patches[0];
  std::vector<ProbeTarget> t(4);
  t[0].node = idOf(top, "a");  // a = x · k
  t[1].node = idOf(top, "b");  // b = x · k: merged with a by CSE
  t[2].node = idOf(top, "m");  // m = x · 3, consumed only by z = m + 1: fused into a muladd
  t[3].node = idOf(top, "w");  // the muladd carries w's value
  Compiled c;
  compile(c, t);
  REQUIRE(c.points.size() == 4);
  CHECK(c.points[0].node != kNone);
  CHECK(c.points[0].node == c.points[1].node);  // one value now
  CHECK(c.points[0].output == c.points[1].output);
  CHECK(c.points[2].node == kNone);             // no node computes x · 3 alone any more
  REQUIRE(c.points[3].node != kNone);
  CHECK(c.opt.patch.node(c.points[3].node).kind == NodeKind::Atom);
  CHECK(atomSignature(c.opt.patch.node(c.points[3].node).atom).name == "muladd");
  // Every surviving value maps to a node output; inputs map to nothing.
  const Patch& in = c.inf.typed.patch;
  for (NodeIndex n = 0; n < in.nodeCount(); ++n)
    for (uint32_t i = 0; i < in.node(n).numInputs; ++i) CHECK(c.valueMap[in.flatInput(n, i)] == kNone);
}

TEST_CASE("probe: a schedule with probes has the same ops, regions, sleep plan and state; probes stay readable (T4.5)") {
  ParseResult pr = parseDetailed(kText, "test.pqp");
  REQUIRE(pr.ok());
  const Patch& top = pr.file.patches[0];
  const Patch& filt = pr.file.patches[1];
  std::vector<ProbeTarget> t(4);
  t[0].node = idOf(top, "a");
  t[1].path = {idOf(top, "f")};
  t[1].node = idOf(filt, "l");  // (t itself is fused into l's muladd once the unused `hi` is dead)
  t[2].node = idOf(top, "k");
  t[3].node = idOf(top, "m");  // fused away: no probe
  Compiled with, without;
  compile(with, t);
  compile(without, {});
  const Schedule& a = with.sched;
  const Schedule& b = without.sched;
  REQUIRE(a.ops.size() == b.ops.size());
  for (size_t i = 0; i < a.ops.size(); ++i) {
    CHECK(a.ops[i].kernel == b.ops[i].kernel);
    CHECK(a.ops[i].node == b.ops[i].node);
    CHECK(a.ops[i].state == b.ops[i].state);
  }
  REQUIRE(a.regions.size() == b.regions.size());
  for (size_t r = 0; r < a.regions.size(); ++r) {
    CHECK(a.regions[r].firstOp == b.regions[r].firstOp);
    CHECK(a.regions[r].numOps == b.regions[r].numOps);
    CHECK(a.regions[r].kind == b.regions[r].kind);
  }
  CHECK(a.sleep.segments.size() == b.sleep.segments.size());
  REQUIRE(a.sleep.regions.size() == b.sleep.regions.size());
  for (size_t r = 0; r < a.sleep.regions.size(); ++r) CHECK(a.sleep.regions[r].segment == b.sleep.regions[r].segment);
  CHECK(a.state.globalBytes == b.state.globalBytes);
  CHECK(a.state.entries.size() == b.state.entries.size());
  // Three probes have a value (ids kept); the fused one is left out.
  REQUIRE(a.probes.size() == 3);
  CHECK(a.probes[0].id == 0);
  CHECK(a.probes[1].id == 1);
  CHECK(a.probes[2].id == 2);
  CHECK(a.probes[2].value.kind == Operand::Kind::Param);
  // A global probe's buffer is last written by the op that computes it, in the probe's region (the host reads it
  // after the last stage). Its value is the program's: tests/engine/test_probes.cpp.
  for (const ProbeRef& p : a.probes) {
    if (p.value.kind != Operand::Kind::Buffer) continue;
    REQUIRE(p.region != kNone);
    uint32_t last = kNone;
    for (uint32_t i = 0; i < a.ops.size(); ++i)
      for (const Operand& o : a.outputsOf(a.ops[i]))
        if (o == p.value) last = i;
    REQUIRE(last != kNone);
    CHECK(last >= a.regions[p.region].firstOp);
    CHECK(last < a.regions[p.region].firstOp + a.regions[p.region].numOps);
  }
  CHECK(dump(a, with.opt).find("probes:") != std::string::npos);
  CHECK(dump(b, without.opt).find("probes:") == std::string::npos);
}

TEST_CASE("probe: cost weights rank expensive ops above cheap ones (T4.5)") {
  Compiled c;
  compile(c, {});
  float add = 0.0f, z1 = 0.0f;
  for (uint32_t i = 0; i < c.sched.ops.size(); ++i) {
    const float w = opCostWeight(c.sched, i);
    CHECK(w > 0.0f);
    if (c.sched.ops[i].kind != OpKind::Kernel) continue;
    if (c.sched.ops[i].kernel == pq::dsp::Atom::Add) add = w;
    if (c.sched.ops[i].kernel == pq::dsp::Atom::Z1 || c.sched.ops[i].kernel == pq::dsp::Atom::Z1Read) z1 = w;
  }
  CHECK(add == 1.0f);
  CHECK(z1 > add);
}
