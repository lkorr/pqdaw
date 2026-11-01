// TMM.6: the `oversample(n) { … }` region in the language and the compiler front end: parse → print → parse round
// trips (phase inputs, outputs inside, inlined expressions), the checker's errors (factor, nesting, declarations,
// phase inputs, the converters are not callable), infer()'s rate converters (which values, which kind, shared per
// value and region, none for params), sr = n·sr inside, n = 1 = the plain nodes, regions through subcircuits, and the
// scheduler (regions and buffers at n×, the state tags, a loop across the boundary, buffer reuse replayed).
#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <map>
#include <string>
#include <vector>

#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "sched_replay.h"

using namespace pq::graph;

namespace {

std::string diagText(const std::vector<Diagnostic>& ds, std::string_view src) {
  std::string s;
  for (const Diagnostic& d : ds) s += formatDiagnostic(src, d) + "\n";
  return s;
}

ParseResult parseOk(const std::string& src) {
  ParseResult r = parseDetailed(src, "os.pqp");
  REQUIRE_MESSAGE(r.ok(), diagText(r.diagnostics, src));
  return r;
}

std::string parseErrors(const std::string& src) {
  ParseResult r = parseDetailed(src, "os.pqp");
  REQUIRE_FALSE(r.ok());
  return diagText(r.diagnostics, src);
}

struct Front {
  ParseResult parsed;
  PatchLibrary lib;
  TypedPatch typed;
};

// parse → infer → optimise (48 kHz) of the first patch.
void front(Front& f, const std::string& src, bool voiceContext = false) {
  f.parsed = parseOk(src);
  f.lib.add(f.parsed.file);
  InferOptions io;
  io.library = &f.lib;
  io.voiceContext = voiceContext;
  InferResult inf = inferDetailed(f.parsed.file.patches[0], io);
  REQUIRE_MESSAGE(inf.ok(), diagText(inf.diagnostics, src));
  OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  auto opt = optimise(std::move(inf.typed), oo);
  REQUIRE_MESSAGE(opt.ok(), (opt.ok() ? std::string() : opt.error().message));
  f.typed = std::move(opt.value());
}

std::string inferErrors(const std::string& src) {
  ParseResult pr = parseOk(src);
  PatchLibrary lib;
  lib.add(pr.file);
  InferOptions io;
  io.library = &lib;
  InferResult inf = inferDetailed(pr.file.patches[0], io);
  REQUIRE_FALSE(inf.ok());
  return diagText(inf.diagnostics, src);
}

std::map<std::string, int> atomCounts(const TypedPatch& tp) {
  std::map<std::string, int> m;
  for (NodeIndex n = 0; n < tp.patch.nodeCount(); ++n)
    if (tp.patch.node(n).kind == NodeKind::Atom) ++m[std::string(atomSignature(tp.patch.node(n).atom).name)];
  return m;
}

const char* kTanh = R"(
patch Tanh {
  in    x : audio
  param drive = 8 [1..32]
  out   y : audio
  oversample(4) {
    y = tanh(x * drive)
  }
}
)";

}  // namespace

TEST_CASE("oversample: parse → print → parse round trip") {
  const std::string src = R"(
patch P {
  param k = 2 [1..8]
  in    x : audio
  in    hz : audio
  out   y : audio
  out   z : audio

  ph = phasor(hz)
  pair = svf2(x)
  oversample(8, phase: ph, pair.hp) {
    a = tanh(x * k) + sine(ph) + pair.hp
    b = z1(a) * 0.5
    y = a - b
  }
  oversample(2) {
    w = sr * 1
  }
  z = w * x
}

patch svf2 {
  in  x : audio
  out lp : audio
  out hp : audio
  l = x * 0.5
  lp = l
  hp = x - l
}
)";
  const ParseResult first = parseOk(src);
  const Patch& p = first.file.patches[0];
  REQUIRE(p.regions().size() == 2);
  CHECK(p.regions()[0].factor == 8);
  CHECK(p.regions()[0].phase == std::vector<std::string>{"ph", "pair.hp"});
  CHECK(p.regions()[1].factor == 2);
  // Every node a region's bindings create is in it; declarations and outer bindings are not.
  CHECK(p.node(*p.findByName("a")).region == 1);
  CHECK(p.node(*p.findByName("b")).region == 1);
  CHECK(p.node(*p.findByName("w")).region == 2);
  CHECK(p.node(*p.findByName("ph")).region == 0);
  CHECK(p.node(*p.findByName("z")).region == 0);
  CHECK(p.node(*p.findByName("y")).region == 0);  // the output itself
  const std::string printed = print(first.file);
  CAPTURE(printed);
  CHECK(printed.find("  oversample(8, phase: ph, pair.hp) {\n") != std::string::npos);
  CHECK(printed.find("    y = a - b\n") != std::string::npos);  // an output's statement goes where it is computed
  const ParseResult second = parseOk(printed);
  CHECK(second.file.patches[0] == p);
  CHECK(print(second.file) == printed);
  // An alias as the phase input resolves to the value it names.
  const ParseResult alias = parseOk(R"(
patch A {
  in hz : audio
  out y : audio
  acc = phasor(hz)
  p = acc
  oversample(4, phase: p) {
    y = sine(p)
  }
}
)");
  CHECK(alias.file.patches[0].regions()[0].phase == std::vector<std::string>{"acc"});
}

TEST_CASE("oversample: checker errors") {
  CHECK(parseErrors("patch P {\n  out y : audio\n  oversample(3) {\n    y = 1\n  }\n}\n")
            .find("the oversampling factor must be 1, 2, 4, 8 or 16") != std::string::npos);
  CHECK(parseErrors("patch P {\n  out y : audio\n  oversample(2) {\n    oversample(2) {\n      y = 1\n    }\n  }\n}\n")
            .find("oversample regions cannot be nested") != std::string::npos);
  CHECK(parseErrors("patch P {\n  out y : audio\n  oversample(2) {\n    param k = 1\n    y = k\n  }\n}\n")
            .find("only bindings") != std::string::npos);
  CHECK(parseErrors("patch P {\n  in x : audio\n  out y : audio\n  oversample(2, phase: q) {\n    y = x\n  }\n}\n")
            .find("unknown phase input 'q'") != std::string::npos);
  CHECK(parseErrors("patch P {\n  in x : audio\n  out y : audio\n  oversample(2, phase: q) {\n    q = phasor(x)\n    y = q\n  }\n}\n")
            .find("is computed inside its oversample region") != std::string::npos);
  CHECK(parseErrors("patch P {\n  in x : audio\n  out y : audio\n  y = oversample_up(x, 2)\n}\n")
            .find("unknown function 'oversample_up'") != std::string::npos);
  // A region inside a callee that is called inside a region.
  CHECK(inferErrors(R"(
patch P {
  in x : audio
  out y : audio
  oversample(2) {
    y = inner(x)
  }
}
patch inner {
  in x : audio
  out y : audio
  oversample(4) {
    y = tanh(x)
  }
}
)")
            .find("oversample regions cannot be nested") != std::string::npos);
  // A feedback loop through the boundary.
  Front f;
  front(f, R"(
patch L {
  in x : audio
  out y : audio
  fb = z1(v)
  oversample(2) {
    v = tanh(x + fb)
  }
  y = v
}
)");
  const ScheduleResult s = scheduleDetailed(f.typed, ScheduleOptions{});
  REQUIRE_FALSE(s.ok());
  CHECK(diagText(s.diagnostics, "").find("crosses the boundary of an oversample region") != std::string::npos);
}

TEST_CASE("oversample: infer inserts one converter per value and region; params, constants and n = 1 get none") {
  Front f;
  front(f, kTanh);
  auto m = atomCounts(f.typed);
  CHECK(m["oversample_up"] == 1);
  CHECK(m["oversample_down"] == 1);
  CHECK(m["oversample_phase"] == 0);
  REQUIRE(f.typed.oversample.size() == 1);
  CHECK(f.typed.oversample[0].factor == 4);
  for (NodeIndex n = 0; n < f.typed.patch.nodeCount(); ++n) {
    const Node& nd = f.typed.patch.node(n);
    if (nd.kind != NodeKind::Atom) continue;
    const std::string_view name = atomSignature(nd.atom).name;
    if (name == "tanh" || name == "mul" || name.starts_with("oversample_")) CHECK(f.typed.origin[n].region == 1);
  }
  // The same value read twice inside: one up converter; a phase input: oversample_phase; a bool: hold / any.
  Front g;
  front(g, R"(
patch P {
  in x : audio
  in hz : audio
  in gate : bool
  out y : audio
  out g2 : bool
  ph = phasor(hz)
  oversample(2, phase: ph) {
    y = tanh(x) * x + sine(ph)
    g2 = rise(gate)
  }
}
)");
  m = atomCounts(g.typed);
  CHECK(m["oversample_up"] == 1);
  CHECK(m["oversample_phase"] == 1);
  CHECK(m["oversample_hold"] == 1);
  CHECK(m["oversample_any"] == 1);
  CHECK(m["oversample_down"] == 1);
  // n = 1: the plain nodes.
  Front one;
  front(one, "patch P {\n  in x : audio\n  out y : audio\n  oversample(1) {\n    y = tanh(x)\n  }\n}\n");
  CHECK(one.typed.oversample.empty());
  for (const auto& [name, count] : atomCounts(one.typed)) CHECK_FALSE(name.starts_with("oversample_"));
}

TEST_CASE("oversample: sr inside is n·sr; durations count the region's samples") {
  Front f;
  front(f, R"(
patch P {
  in x : audio
  out y : audio
  out s : audio
  oversample(4) {
    y = x * sr
  }
  s = x * sr
}
)");
  // Optimised: the two products have literals 192000 and 48000.
  std::vector<double> lits;
  for (NodeIndex n = 0; n < f.typed.patch.nodeCount(); ++n)
    if (f.typed.patch.node(n).kind == NodeKind::Const) lits.push_back(f.typed.patch.constant(n).value());
  CHECK(std::find(lits.begin(), lits.end(), 192000.0) != lits.end());
  CHECK(std::find(lits.begin(), lits.end(), 48000.0) != lits.end());
  // A delay line inside: its ring is sized at the region's rate.
  Front d;
  front(d, R"(
patch D {
  in x : audio
  out y : audio
  oversample(2) {
    y = delay(x, 1ms, 10ms)
  }
}
)");
  const ScheduleResult s = scheduleDetailed(d.typed, ScheduleOptions{});
  REQUIRE(s.ok());
  bool found = false;
  for (const StateEntry& e : s.schedule.state.entries)
    if (e.tag.kernel == static_cast<uint16_t>(pq::dsp::Atom::DelayLinear)) {
      CHECK(e.init.maxDelayFrames == 960);  // 10 ms at 96 kHz
      CHECK(e.tag.factor == 2);
      found = true;
    }
  CHECK(found);
}

TEST_CASE("oversample: regions through subcircuits; voice regions") {
  Front f;
  front(f, R"(
patch P {
  in x : audio
  out y : audio
  oversample(4) {
    y = shape(x)
  }
}
patch shape {
  in x : audio
  out y : audio
  y = tanh(x * 3) + z1(x)
}
)");
  auto m = atomCounts(f.typed);
  CHECK(m["oversample_up"] == 1);
  CHECK(m["oversample_down"] == 1);
  for (NodeIndex n = 0; n < f.typed.patch.nodeCount(); ++n)
    if (f.typed.patch.node(n).kind == NodeKind::Atom && atomSignature(f.typed.patch.node(n).atom).name == "z1")
      CHECK(f.typed.origin[n].region == 1);
  // A per-voice region: the converters run per voice (voice.hz is an engine source: it is upsampled).
  Front v;
  front(v, R"(
patch V {
  out y : audio voice
  oversample(2) {
    y = tanh(sine(phasor(voice.hz)) * 4)
  }
}
)",
        true);
  const ScheduleResult s = scheduleDetailed(v.typed, ScheduleOptions{});
  REQUIRE_MESSAGE(s.ok(), diagText(s.diagnostics, ""));
  bool voiceRegion = false;
  for (const Region& r : s.schedule.regions)
    if (r.factor == 2) {
      CHECK(r.poly == Poly::Voice);
      voiceRegion = true;
    }
  CHECK(voiceRegion);
}

TEST_CASE("oversample: the schedule runs the region at n× (regions, buffers, state tags, buffer reuse)") {
  Front f;
  front(f, kTanh);
  const ScheduleResult s = scheduleDetailed(f.typed, ScheduleOptions{});
  REQUIRE(s.ok());
  const Schedule& sc = s.schedule;
  uint32_t oversampled = 0;
  for (const Region& r : sc.regions) {
    if (r.factor == 1) continue;
    ++oversampled;
    CHECK(r.factor == 4);
    CHECK(r.kind != RegionKind::Control);
    // Every op of the region: its block operands hold 4 samples per frame, except the converters' base-rate side.
    for (uint32_t i = r.firstOp; i < r.firstOp + r.numOps; ++i) {
      const Op& op = sc.ops[i];
      const bool up = op.kernel == pq::dsp::Atom::OversampleUp, down = op.kernel == pq::dsp::Atom::OversampleDown;
      for (const Operand& o : sc.inputs(op))
        if (o.kind == Operand::Kind::Buffer && sc.buffers[o.index].cls == Buffer::Class::Block)
          CHECK(sc.buffers[o.index].factor == (up ? 1 : 4));
      for (const Operand& o : sc.outputsOf(op))
        if (o.kind == Operand::Kind::Buffer) CHECK(sc.buffers[o.index].factor == (down ? 1 : 4));
    }
  }
  CHECK(oversampled == 1);  // up, the region's ops and down stay together
  for (const StateEntry& e : sc.state.entries)
    if (e.tag.kernel == static_cast<uint16_t>(pq::dsp::Atom::OversampleUp)) CHECK(e.tag.factor == 4);
  for (const OutputRef& o : sc.outputs)
    if (o.value.kind == Operand::Kind::Buffer) CHECK(sc.buffers[o.value.index].factor == 1);
  CHECK(dump(sc, f.typed).find("oversampled x4") != std::string::npos);
  CHECK(sched_replay::check(f.typed, ScheduleOptions{}) == "");
  // Never asleep.
  for (uint32_t r = 0; r < sc.regions.size() && r < sc.sleep.regions.size(); ++r)
    if (sc.regions[r].factor > 1) CHECK(sc.sleep.regions[r].segment == kNone);
}
