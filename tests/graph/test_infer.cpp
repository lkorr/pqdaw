// T1.3: type inference. Table-driven: small .pqp snippets (parsed by T1.2) → expected inferred types per named
// node/port of the flattened patch, or an expected error (message fragment and file:line:col).

#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <set>
#include <string>
#include <vector>

#include "pq/graph/infer.h"
#include "pq/graph/parse.h"
#include "text_util.h"

using namespace pq::graph;

namespace {

struct Options {
  bool voiceContext = true;
  bool implicitVoiceSum = true;
  Poly freeRunningPoly = Poly::Global;
  uint16_t ioChannels = 2;
  std::vector<RootInput> inputs;
};

struct Expect {
  const char* port;  // "name" (output 0, or input 0 of a sink), "name.out" (output by name), "name:in" (input by name)
  const char* type;  // toText(PortType), e.g. "real audio voice mono"
};

struct Case {
  const char* name;
  const char* src;  // the first patch is the root; every patch of the file is in the library
  std::vector<Expect> expect;
  const char* error = nullptr;  // fragment of the (first) diagnostic, if inference must fail
  const char* where = nullptr;  // its "line:col"
  Options opt{};
};

struct Run {
  ParseResult parsed;
  PatchLibrary lib;
  InferResult inferred;
};

std::string diagText(const std::vector<Diagnostic>& ds, std::string_view src) {
  std::string s;
  for (const Diagnostic& d : ds) s += formatDiagnostic(src, d) + "\n";
  return s;
}

// Parses (requiring success) and infers the first patch.
void runCase(Run& r, std::string_view src, const Options& o) {
  r.parsed = parseDetailed(src, "test.pqp");
  REQUIRE_MESSAGE(r.parsed.ok(), diagText(r.parsed.diagnostics, src));
  r.lib.add(r.parsed.file);
  InferOptions opts;
  opts.library = &r.lib;
  opts.voiceContext = o.voiceContext;
  opts.implicitVoiceSum = o.implicitVoiceSum;
  opts.freeRunningPoly = o.freeRunningPoly;
  opts.ioChannels = o.ioChannels;
  opts.inputs = o.inputs;
  opts.source = SourceText{src, "test.pqp", r.parsed.nodeSpans[0]};
  r.inferred = inferDetailed(r.parsed.file.patches[0], opts);
}

std::string typeAt(const TypedPatch& tp, std::string_view spec) {
  const Patch& p = tp.patch;
  const size_t dot = spec.find('.'), colon = spec.find(':');
  const std::string_view name = spec.substr(0, std::min(dot, colon));
  const auto n = p.findByName(name);
  if (!n) return "<no node '" + std::string(name) + "'>";
  if (colon != std::string_view::npos) {
    const auto i = p.findInput(*n, spec.substr(colon + 1));
    return i ? toText(tp.inputType(*n, *i)) : "<no input>";
  }
  if (dot != std::string_view::npos) {
    const auto o = p.findOutput(*n, spec.substr(dot + 1));
    return o ? toText(tp.outputType(*n, *o)) : "<no output>";
  }
  if (p.node(*n).numOutputs > 0) return toText(tp.outputType(*n, 0));
  return toText(tp.inputType(*n, 0));
}

// Every port of the result is concrete, and the arrays are parallel.
void checkComplete(const TypedPatch& tp) {
  REQUIRE(tp.types.size() == tp.patch.portCount());
  REQUIRE(tp.stateKey.size() == tp.patch.nodeCount());
  REQUIRE(tp.origin.size() == tp.patch.nodeCount());
  for (const PortType& t : tp.types) {
    CHECK(t.kind != Kind::Infer);
    CHECK(t.rate != Rate::Infer);
    CHECK(t.poly != Poly::Infer);
    CHECK(t.width != kWidthInfer);
  }
  for (const Node& nd : tp.patch.nodes()) CHECK(nd.kind != NodeKind::Subcircuit);
  // Inputs carry their driver's type.
  for (const Wire& w : tp.patch.wires())
    CHECK(tp.types[tp.patch.flatInput(w.to.node, w.to.port)] == tp.types[tp.patch.flatOutput(w.from.node, w.from.port)]);
}

// clang-format off
const Case kCases[] = {
    // ---- rate ------------------------------------------------------------------------------------------------
    {"const arithmetic stays const",
     "patch R { out o : real\n y = 2 * 3 + sr\n o = y }",
     {{"y", "real const global mono"}, {"o", "real const global mono"}}},
    {"a param is param rate, and so is math on it",
     "patch R { param g = 0.5\n out o : real\n y = g * 2\n o = y }",
     {{"g", "real param global mono"}, {"y", "real param global mono"}}},
    {"param times an audio input is audio",
     "patch R { in x : audio\n param g = 0.5\n out o : real\n y = x * g\n o = y }",
     {{"y", "real audio global mono"}}},
    {"an unmodulated param input stays param (tan hoistable)",
     "patch R { in x : audio\n out o : audio\n f = Filt(x)\n o = f }\n"
     "patch Filt { in x : audio\n param cutoff = 1000 [20..20000]\n out y : audio\n g = tan(cutoff * 0.0001)\n y = x * g }",
     {{"f__g", "real param global mono"}, {"f__cutoff", "real param global mono"}, {"o", "real audio global mono"}}},
    {"a modulation wire raises the param input to audio",
     "patch R { in x : audio\n out o : audio\n lfo = sine(phasor(2Hz))\n f = Filt(x, cutoff: 1000 + lfo * 100)\n o = f }\n"
     "patch Filt { in x : audio\n param cutoff = 1000 [20..20000]\n out y : audio\n g = tan(cutoff * 0.0001)\n y = x * g }",
     {{"lfo", "real audio global mono"}, {"f__g", "real audio global mono"}, {"f__y", "<no node 'f__y'>"}}},
    {"per call site: one filter unmodulated, one modulated",
     "patch R { in x : audio\n out o : audio\n a = Filt(x)\n b = Filt(x, cutoff: 500 + sine(phasor(1Hz)) * 50)\n o = a + b }\n"
     "patch Filt { in x : audio\n param cutoff = 1000 [20..20000]\n out y : audio\n g = tan(cutoff * 0.0001)\n y = x * g }",
     {{"a__g", "real param global mono"}, {"b__g", "real audio global mono"}}},
    {"a param bound to a constant at the call site is const",
     "patch R { in x : audio\n out o : audio\n f = Filt(x, cutoff: 440Hz)\n o = f }\n"
     "patch Filt { in x : audio\n param cutoff = 1000 [20..20000]\n out y : audio\n g = tan(cutoff * 0.0001)\n y = x * g }",
     {{"f__g", "real const global mono"}}},
    {"stateful atoms output audio even from const inputs",
     "patch R { out o : audio\n p = phasor(220Hz)\n z = z1(0.5)\n n = noise()\n o = p + z + n }",
     {{"p", "real audio global mono"}, {"z", "real audio global mono"}, {"n", "real audio global mono"}}},
    {"source rates: voice.velocity param, transport.bpm param, transport.beat audio, sr const",
     "patch R { out o : audio voice\n v = voice.velocity * 2\n b = transport.bpm / 60\n t = transport.beat * 1\n s = sr * 0.5\n o = v + b + t + s }",
     {{"v", "real param voice mono"}, {"b", "real param global mono"}, {"t", "real audio global mono"},
      {"s", "real const global mono"}}},
    {"a declared param-rate root input",
     "patch R { in x : real param\n out o : real\n y = x * 2\n o = y }",
     {{"y", "real param global mono"}}},
    {"the engine connects a param-rate signal to an audio-bounded input",
     "patch R { in x : audio\n out o : real\n y = x * 2\n o = y }",
     {{"x", "real param global mono"}, {"y", "real param global mono"}}, nullptr, nullptr,
     {true, true, Poly::Global, 2, {RootInput{true, portType(Kind::Infer, Rate::Param)}}}},
    {"an unconnected root input reads its default",
     "patch R { in x : real = 0.25\n out o : real\n y = x * 2\n o = y }",
     {{"y", "real const global mono"}}, nullptr, nullptr,
     {true, true, Poly::Global, 2, {RootInput{false, {}}}}},
    {"an undeclared root rate defaults to audio",
     "patch R { in x : real\n out o : real\n y = x * 2\n o = y }",
     {{"y", "real audio global mono"}}},

    // ---- kind ------------------------------------------------------------------------------------------------
    {"comparisons give bools at the operands' rate",
     "patch R { in x : audio\n param t = 0.1\n out o : bool\n b = x > t\n c = t < 1\n o = b && c }",
     {{"b", "bool audio global mono"}, {"c", "bool param global mono"}, {"o", "bool audio global mono"}}},
    {"select takes the kind of its branches",
     "patch R { in x : audio\n in g : bool\n out o : real\n out q : bool\n s = select(g, x, 0)\n t = g ? g : false\n o = s\n q = t }",
     {{"s", "real audio global mono"}, {"t", "bool audio global mono"}}},
    {"z1 and latch of a bool are bools",
     "patch R { in g : bool param\n out o : bool\n z = z1(g)\n l = latch(g, g)\n o = z && l }",
     {{"z", "bool audio global mono"}, {"l", "bool audio global mono"}}},
    {"gate_to_real is the explicit bool → real conversion",
     "patch R { in g : bool audio voice\n out o : audio voice\n r = gate_to_real(g)\n o = r }",
     {{"r", "real audio voice mono"}}},
    {"eq / ne on reals stay comparators",
     "patch R { in x : audio\n out o : bool\n e = x == 1\n n = x != 2\n o = e || n }",
     {{"e", "bool audio global mono"}, {"n", "bool audio global mono"}}},
    {"a solve block's unknowns follow the equations' inputs",
     "patch R { in x : audio\n param drive = 1 [0..10]\n out o : audio\n solve(iterations: 4) {\n unknown v = 0\n v == x - drive * tanh(v) * 0.1\n }\n o = v }",
     {{"v", "real audio global mono"}}},
    {"a solve block over params only is param rate",
     "patch R { param a = 1 [0..10]\n out o : real\n solve(iterations: 2) {\n unknown v\n v * 2 == a\n }\n o = v }",
     {{"v", "real param global mono"}}},
    {"a z1 feedback loop is real audio",
     "patch R { in x : audio\n out o : audio\n lp = z1(lp + (x - lp) * 0.2)\n o = lp }",
     {{"lp", "real audio global mono"}}},
    {"kind mismatch inside an any-kind atom",
     "patch R { in x : audio\n in g : bool\n out o : real\n y = g ? x : x > 0\n o = y }",
     {}, "kind mismatch: real and bool meet at 'y' (atom select", "4:8"},
    {"inferred bool into a callee's real input",
     "patch R { in g : bool\n out o : real\n s = g ? g : false\n o = Gain(s) }\n"
     "patch Gain { in x : real\n out y : real\n y = x * 2 }",
     {}, "kind mismatch: bool into real input 'x' of", "4:6"},

    // ---- poly ------------------------------------------------------------------------------------------------
    {"voice sources make everything downstream per-voice",
     "patch R { param g = 0.5\n out o : audio voice\n y = voice.hz * g\n o = y }",
     {{"y", "real audio voice mono"}, {"g", "real param global mono"}}},
    {"stateful atoms on voice inputs are per-voice",
     "patch R { out o : audio voice\n p = phasor(voice.hz)\n z = z1(p)\n o = z }",
     {{"p", "real audio voice mono"}, {"z", "real audio voice mono"}}},
    {"a voice signal into a global output is summed",
     "patch R { out o : audio global\n y = sine(phasor(voice.hz))\n o = y }",
     {{"y", "real audio voice mono"}, {"o", "real audio global mono"}}},
    {"a bool voice signal into a global output is reduced too (any)",
     "patch R { out o : bool global\n o = voice.gate }",
     {{"o", "bool audio global mono"}}},
    {"the per-wire override is an explicit reducer",
     "patch R { out o : audio global\n env = voice.velocity * gate_to_real(voice.gate)\n h = voice_highest(env)\n o = h }",
     {{"env", "real audio voice mono"}, {"h", "real audio global mono"}}},
    {"a voice signal into a callee's global input is reduced at the boundary",
     "patch R { out o : audio\n f = Glob(voice.hz)\n o = f }\n"
     "patch Glob { in x : real global\n out y : real\n a = x * 2\n b = x + 1\n y = a + b }",
     {{"f__a", "real audio global mono"}, {"f__b", "real audio global mono"}}},
    {"a voice signal into a callee's param makes that instance per-voice",
     "patch R { in x : audio voice\n out o : audio voice\n f = Filt(x, cutoff: voice.hz * 2)\n o = f }\n"
     "patch Filt { in x : audio\n param cutoff = 1000 [20..20000]\n out y : audio\n g = tan(cutoff * 0.0001)\n y = x * g }",
     {{"f__g", "real audio voice mono"}}},
    {"free-running sources are global by default",
     "patch R { out o : audio voice\n n = noise()\n p = phasor(220Hz)\n o = n + p + voice.hz }",
     {{"n", "real audio global mono"}, {"p", "real audio global mono"}}},
    {"free-running sources per voice when asked",
     "patch R { out o : audio voice\n n = noise()\n p = pulse()\n o = n + gate_to_real(p) }",
     {{"n", "real audio voice mono"}, {"p", "bool audio voice mono"}}, nullptr, nullptr,
     {true, true, Poly::Voice, 2, {}}},
    {"voice into a global bound without implicit sums is an error",
     "patch R { out o : audio global\n o = voice.hz }",
     {}, "needs an explicit reducer", "1:15", {true, false, Poly::Global, 2, {}}},
    {"voice sources outside a voice context are an error",
     "patch R { out o : audio\n y = voice.hz * 2\n o = y }",
     {}, "voice.hz is only available in a voice context", "2:6", {false, true, Poly::Global, 2, {}}},

    // ---- width -----------------------------------------------------------------------------------------------
    {"mono broadcasts over stereo",
     "patch R { in x : audio stereo\n param g = 0.5\n out o : audio stereo\n y = x * g\n o = y }",
     {{"y", "real audio global stereo"}, {"o", "real audio global stereo"}}},
    {"a tuple packs channels; channel selects one",
     "patch R { in a : audio\n out o : audio\n p = (a, 2)\n q = (p, a)\n c = q[2]\n o = c }",
     {{"p", "real audio global stereo"}, {"q", "real audio global ch(3)"}, {"c", "real audio global mono"}}},
    {"a delay feedback loop keeps the input's width",
     "patch R { in x : audio stereo\n out o : audio stereo\n echo = delay(x + echo * 0.5, 100ms, 1s)\n o = echo }",
     {{"echo", "real audio global stereo"}}},
    {"engine I/O sources have ioChannels channels",
     "patch R { out o : audio ch(4)\n t = track.in * 0.5\n o = t }",
     {{"t", "real audio global ch(4)"}}, nullptr, nullptr, {true, true, Poly::Global, 4, {}}},
    {"mono into a stereo output stays mono (the engine broadcasts)",
     "patch R { in x : audio\n out o : audio stereo\n o = x }",
     {{"o", "real audio global mono"}}},
    {"stereo and ch(3) do not combine",
     "patch R { in a : audio stereo\n in b : audio ch(3)\n out o : real\n y = a + b\n o = y }",
     {}, "width mismatch: stereo and ch(3) meet at 'y'", "4:8"},
    {"a channel index beyond the width",
     "patch R { in a : audio stereo\n out o : real\n c = a[2]\n o = c }",
     {}, "channel index 2 is not a channel of a stereo signal", "3:7"},
    {"stereo into a callee's mono input",
     "patch R { in a : audio stereo\n out o : real\n s = a * 2\n o = M(s) }\n"
     "patch M { in x : real mono\n out y : real\n y = x }",
     {}, "width mismatch: stereo into mono input 'x' of", "4:6"},

    // ---- declared bounds ---------------------------------------------------------------------------------------
    {"a param into a const-bounded input (delay max)",
     "patch R { in x : audio\n param m = 1 [0..2]\n out o : audio\n d = delay(x, 10ms, m)\n o = d }",
     {}, "rate param exceeds the declared bound const of input 'max' of 'd' (atom delay", "4:6"},
    {"an audio signal into an output bounded to param",
     "patch R { in x : audio\n out flag : bool param global\n flag = x > 0 }",
     {}, "rate audio exceeds the declared bound param of 'flag'", "2:6"},
    {"an audio signal into the param-bounded start of a sample player",
     "patch R { in x : audio\n out o : audio\n s = sample_read_cubic(1, 1, start: x)\n o = s }",
     {}, "rate audio exceeds the declared bound param of input 'start'", "3:6"},
    {"an audio signal into a callee's param-bounded input is reported at the call",
     "patch R { in x : audio\n out o : audio\n\n  k = Coef(x)\n o = k }\n"
     "patch Coef { in c : real param\n out y : real\n y = tan(c)\n }",
     {}, "rate audio exceeds the declared bound param of input 'c' of 'k' (subcircuit Coef", "4:7"},
    {"a callee whose own output bound is exceeded",
     "patch R { out o : audio\n o = Bad() }\n"
     "patch Bad { out y : real param\n y = noise() }",
     {}, "rate audio exceeds the declared bound param of 'y' (out", "2:6"},

    // ---- subcircuits ---------------------------------------------------------------------------------------------
    {"nested subcircuits are flattened with path names",
     "patch R { in x : audio\n out o : audio\n a = Outer(x)\n o = a }\n"
     "patch Outer { in x : audio\n out y : audio\n b = Inner(x)\n y = b }\n"
     "patch Inner { in x : audio\n param k = 2\n out y : audio\n m = x * k\n y = m }",
     {{"a__b__m", "real audio global mono"}, {"a__b__k", "real param global mono"}}},
    {"an unconnected callee input uses its default",
     "patch R { out o : real\n f = D()\n o = f }\n"
     "patch D { in x : real = 0.5\n out y : real\n m = x * 3\n y = m }",
     {{"f__m", "real const global mono"}}},
    {"a callee's export is a second output",
     "patch R { out o : audio voice\n out e : audio voice\n v = Osc(voice.hz)\n o = v\n e = v.env }\n"
     "patch Osc { in hz : real\n out y : audio\n s = sine(phasor(hz))\n y = s\n export env = s * 0.5 }",
     {{"v__s", "real audio voice mono"}, {"e", "real audio voice mono"}}},
};
// clang-format on

}  // namespace

TEST_CASE("infer: table of cases (inputs → inferred types, or the error and its position)") {
  int n = 0;
  for (const Case& c : kCases) {
    CAPTURE(c.name);
    Run r;
    runCase(r, c.src, c.opt);
    ++n;
    if (c.error) {
      REQUIRE_MESSAGE(!r.inferred.ok(), "expected an error containing: " << c.error);
      const Diagnostic& d = r.inferred.diagnostics.front();
      CHECK_MESSAGE(d.message.find(c.error) != std::string::npos, diagText(r.inferred.diagnostics, c.src));
      if (c.where) {
        const std::string at = std::to_string(d.location.line) + ":" + std::to_string(d.location.col);
        CHECK_MESSAGE(at == c.where, diagText(r.inferred.diagnostics, c.src));
        CHECK(d.location.file == "test.pqp");
      }
      continue;
    }
    REQUIRE_MESSAGE(r.inferred.ok(), diagText(r.inferred.diagnostics, c.src));
    checkComplete(r.inferred.typed);
    for (const Expect& e : c.expect) {
      CAPTURE(e.port);
      CHECK(typeAt(r.inferred.typed, e.port) == e.type);
    }
  }
  CHECK(n >= 40);
}

TEST_CASE("infer: an unknown subcircuit and a missing library are errors at the call") {
  // `Lib` exists while parsing, but the library given to infer() does not have it.
  const char* src = "patch R { in x : audio\n out o : audio\n f = Lib(x)\n o = f }\npatch Lib { in x : audio\n out y : audio\n y = x }";
  ParseResult p = parseDetailed(src, "t.pqp");
  REQUIRE(p.ok());
  InferOptions opts;
  opts.source = SourceText{src, "t.pqp", p.nodeSpans[0]};
  InferResult r = inferDetailed(p.file.patches[0], opts);
  REQUIRE(!r.ok());
  CHECK(r.diagnostics[0].message.find("unknown subcircuit 'Lib'") != std::string::npos);
  CHECK(r.diagnostics[0].location.line == 3);
  // infer() formats the same with the source line and a caret.
  auto e = infer(p.file.patches[0], opts);
  REQUIRE(!e.ok());
  CHECK(e.error().message.find("t.pqp:3:6: error: patch 'R': unknown subcircuit 'Lib'") != std::string::npos);
  CHECK(e.error().message.find(" f = Lib(x)\n     ^~~") != std::string::npos);
}

TEST_CASE("infer: voice → global sums are inserted once per boundary") {
  Run r;
  runCase(r,
          "patch R { out o : audio global\n f = Glob(voice.hz)\n o = f + sine(phasor(voice.hz)) }\n"
          "patch Glob { in x : real global\n out y : real\n a = x * 2\n b = x + 1\n y = a + b }",
          {});
  REQUIRE_MESSAGE(r.inferred.ok(), diagText(r.inferred.diagnostics, ""));
  const TypedPatch& tp = r.inferred.typed;
  const AtomId sum = atomId("voice_sum");
  int sums = 0;
  for (NodeIndex n = 0; n < tp.patch.nodeCount(); ++n) {
    const Node& nd = tp.patch.node(n);
    if (nd.kind != NodeKind::Atom || nd.atom != sum) continue;
    ++sums;
    CHECK(tp.inputType(n, 0).poly == Poly::Voice);
    CHECK(tp.outputType(n, 0).poly == Poly::Global);
    CHECK(tp.origin[n].what == NodeOrigin::What::Reducer);
  }
  // One at the callee's `in x : real global` (shared by a and b) and one in front of the global output.
  CHECK(sums == 2);
  const NodeIndex o = *tp.patch.findByName("o");
  const Adjacency adj = tp.patch.adjacency();
  const Wire& w = tp.patch.wire(adj.driver[tp.patch.flatInput(o, 0)]);
  CHECK(tp.patch.node(w.from.node).atom == sum);
  checkComplete(tp);
}

TEST_CASE("infer: bool eq / ne become not(xor) / xor; real ones stay comparators") {
  Run r;
  runCase(r, "patch R { in g : bool\n in h : bool\n in x : audio\n out o : bool\n e = g == h\n n = g != true\n c = x == 1\n o = e && n && c }",
          {});
  REQUIRE(r.inferred.ok());
  const TypedPatch& tp = r.inferred.typed;
  const Patch& p = tp.patch;
  const NodeIndex e = *p.findByName("e"), n = *p.findByName("n"), c = *p.findByName("c");
  CHECK(p.node(e).atom == atomId("not"));
  CHECK(tp.origin[e].what == NodeOrigin::What::Rewrite);
  const Adjacency adj = p.adjacency();
  const NodeIndex x = p.wire(adj.driver[p.flatInput(e, 0)]).from.node;
  CHECK(p.node(x).atom == atomId("xor"));
  CHECK(tp.outputType(x, 0).kind == Kind::Bool);
  CHECK(p.node(n).atom == atomId("xor"));
  CHECK(p.node(c).atom == atomId("eq"));
  checkComplete(tp);
}

TEST_CASE("infer: flattening keeps unbound callee params as knobs and wires through the boundaries") {
  Run r;
  runCase(r,
          "patch R { in x : audio\n out o : audio\n f = Filt(x)\n o = f }\n"
          "patch Filt { in x : audio\n param cutoff = 1000 [20..20000] @label(\"Cutoff\")\n out y : audio\n g = tan(cutoff * 0.0001)\n y = x * g }",
          {});
  REQUIRE(r.inferred.ok());
  const TypedPatch& tp = r.inferred.typed;
  const Patch& p = tp.patch;
  const NodeIndex k = *p.findByName("f__cutoff");
  CHECK(p.node(k).kind == NodeKind::Param);
  CHECK(p.paramDecl(k).attrs.size() == 1);
  // No in/out nodes of the callee survive; the root's do.
  int ins = 0, outs = 0;
  for (const Node& nd : p.nodes()) {
    ins += nd.kind == NodeKind::Input;
    outs += nd.kind == NodeKind::Output;
  }
  CHECK(ins == 1);
  CHECK(outs == 1);
  // x * g reads the root input directly.
  const Adjacency adj = p.adjacency();
  NodeIndex mul = kNone;
  for (NodeIndex n = 0; n < p.nodeCount(); ++n)
    if (p.node(n).kind == NodeKind::Atom && p.node(n).atom == atomId("mul") && tp.instances[tp.origin[n].instance].patch->name() == "Filt" &&
        p.wire(adj.driver[p.flatInput(n, 0)]).from.node == *p.findByName("x"))
      mul = n;
  CHECK(mul != kNone);
  REQUIRE(tp.instances.size() == 2);
  CHECK(tp.instances[1].parent == 0);
  CHECK(tp.instances[1].patch->name() == "Filt");
}

TEST_CASE("infer: state keys are stable across runs and distinct per instance") {
  const char* src =
      "patch R { in x : audio\n out o : audio\n a = Lp(x)\n b = Lp(x)\n o = a + b }\n"
      "patch Lp { in x : audio\n out y : audio\n s = z1(s + (x - s) * 0.1)\n y = s }";
  Run r1, r2;
  runCase(r1, src, {});
  runCase(r2, src, {});
  REQUIRE(r1.inferred.ok());
  REQUIRE(r2.inferred.ok());
  const TypedPatch& t1 = r1.inferred.typed;
  CHECK(t1.stateKey == r2.inferred.typed.stateKey);
  const uint64_t ka = t1.stateKey[*t1.patch.findByName("a__s")], kb = t1.stateKey[*t1.patch.findByName("b__s")];
  CHECK(ka != kb);
  std::set<uint64_t> keys(t1.stateKey.begin(), t1.stateKey.end());
  CHECK(keys.size() == t1.stateKey.size());
}

TEST_CASE("infer: recursion through the library and nesting depth") {
  // Two files' worth of patches calling each other: the parser checks recursion only within one file.
  ParseResult a = parseDetailed("patch A { in x : real\n out y : real\n y = B(x) }\npatch B { in x : real\n out y : real\n y = x }", "a.pqp");
  ParseResult b = parseDetailed("patch B { in x : real\n out y : real\n y = A(x) }\npatch A { in x : real\n out y : real\n y = x }", "b.pqp");
  REQUIRE(a.ok());
  REQUIRE(b.ok());
  PatchLibrary lib;
  lib.add(a.file.patches[0]);  // A calls B
  lib.add(b.file.patches[0]);  // B calls A
  InferOptions opts;
  opts.library = &lib;
  InferResult r = inferDetailed(a.file.patches[0], opts);
  REQUIRE(!r.ok());
  CHECK(r.diagnostics[0].message.find("instantiates itself") != std::string::npos);

  // A chain deeper than kMaxSubcircuitDepth.
  std::string src;
  for (uint32_t i = 0; i <= kMaxSubcircuitDepth + 1; ++i) {
    src += "patch P" + std::to_string(i) + " { in x : real\n out y : real\n y = ";
    src += i <= kMaxSubcircuitDepth ? "P" + std::to_string(i + 1) + "(x) }\n" : "x * 2 }\n";
  }
  Run deep;
  runCase(deep, src, {});
  REQUIRE(!deep.inferred.ok());
  CHECK(deep.inferred.diagnostics[0].message.find("nest deeper than") != std::string::npos);
}

TEST_CASE("infer: the PATCH_LANGUAGE §4 FmVoice with its adsr") {
  text_util::DirLoader loader(text_util::casesDir());
  ParseOptions po;
  po.imports = &loader;
  const auto fmPath = text_util::casesDir() / "fm_voice.pqp";
  const std::string fmText = text_util::readFile(fmPath);
  ParseResult fm = parseDetailed(fmText, fmPath.string(), po);
  REQUIRE(fm.ok());
  const std::string envText = text_util::readFile(text_util::casesDir() / "lib" / "envelopes.pqp");
  ParseResult env = parseDetailed(envText, "envelopes.pqp");
  REQUIRE(env.ok());
  PatchLibrary lib;
  lib.add(env.file);
  InferOptions opts;
  opts.library = &lib;
  InferResult r = inferDetailed(fm.file.patches[0], opts);
  REQUIRE_MESSAGE(r.ok(), diagText(r.diagnostics, fmText));
  const TypedPatch& tp = r.typed;
  checkComplete(tp);
  CHECK(typeAt(tp, "mod") == "real audio voice mono");
  CHECK(typeAt(tp, "car") == "real audio voice mono");
  CHECK(typeAt(tp, "out") == "real audio voice mono");
  CHECK(typeAt(tp, "env__level") == "real audio voice mono");  // the adsr's state is per voice
  CHECK(typeAt(tp, "env__coef") == "real audio voice mono");   // its time constant switches per sample with the stage
  CHECK(typeAt(tp, "ratio") == "real param global mono");
  CHECK(typeAt(tp, "modulator") == "real audio voice mono");
}

TEST_CASE("infer: every corpus patch infers, standalone (root inputs with a default read it)") {
  text_util::DirLoader loader(text_util::casesDir());
  PatchLibrary lib;
  std::vector<ParseResult> files;
  const auto paths = text_util::pqpFiles(text_util::casesDir());
  files.reserve(paths.size());
  for (const auto& path : paths) {
    ParseOptions po;
    po.imports = &loader;
    files.push_back(parseDetailed(text_util::readFile(path), path.string(), po));
    REQUIRE(files.back().ok());
  }
  for (const ParseResult& f : files) lib.add(f.file);
  int inferred = 0;
  for (const ParseResult& f : files)
    for (const Patch& p : f.file.patches) {
      CAPTURE(p.name());
      // Standalone: an `in` with a default is unconnected (so `in k : bool = transport.playing` is param rate);
      // the others are connected with their declared type.
      std::vector<RootInput> inputs;
      const Adjacency adj = p.adjacency();
      for (NodeIndex n = 0; n < p.nodeCount(); ++n)
        if (p.node(n).kind == NodeKind::Input) inputs.push_back(RootInput{adj.driver[p.flatInput(n, 0)] == kNone, {}});
      InferOptions opts;
      opts.library = &lib;
      opts.inputs = inputs;
      InferResult r = inferDetailed(p, opts);
      CHECK_MESSAGE(r.ok(), diagText(r.diagnostics, ""));
      if (r.ok()) {
        checkComplete(r.typed);
        ++inferred;
      }
    }
  CHECK(inferred >= 15);
}
