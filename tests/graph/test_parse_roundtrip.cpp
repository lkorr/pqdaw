// T1.2: parse/print round trip over the whole corpus (tests/graph/cases/, stdlib/), plus the semantics of the
// text → IR mapping (units, aliases, forward references, output selection, folding, printer naming).

#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <string>

#include "fm_voice.h"
#include "pq/graph/parse.h"
#include "text_util.h"

using namespace pq::graph;
namespace fs = std::filesystem;

namespace {

std::string diagnostics(const ParseResult& r, std::string_view src) {
  std::string s;
  for (const Diagnostic& d : r.diagnostics) s += formatDiagnostic(src, d) + "\n";
  return s;
}

// Parses `src` (with imports from the cases directory) and requires success.
PatchFile parseOk(std::string_view src, const std::string& name = "test.pqp") {
  text_util::DirLoader loader(text_util::casesDir());
  ParseOptions opts;
  opts.imports = &loader;
  ParseResult r = parseDetailed(src, (text_util::casesDir() / name).string(), opts);
  REQUIRE_MESSAGE(r.ok(), diagnostics(r, src));
  return std::move(r.file);
}

const Patch& only(const PatchFile& f) {
  REQUIRE(f.patches.size() == 1);
  return f.patches[0];
}

NodeIndex named(const Patch& p, std::string_view name) {
  const auto n = p.findByName(name);
  REQUIRE_MESSAGE(n.has_value(), "no node named " << std::string(name));
  return *n;
}

PortRef driverOf(const Patch& p, NodeIndex n, uint32_t input) {
  for (const Wire& w : p.wires())
    if (w.to == PortRef{n, static_cast<uint16_t>(input)}) return w.from;
  return {};
}

// Round trip of one file: parse(print(f)) == f and print(parse(print(f))) == print(f).
void roundTrip(const fs::path& path, ImportLoader& loader) {
  const std::string src = text_util::readFile(path);
  ParseOptions opts;
  opts.imports = &loader;
  const ParseResult first = parseDetailed(src, path.string(), opts);
  REQUIRE_MESSAGE(first.ok(), diagnostics(first, src));
  const std::string printed = print(first.file);
  const ParseResult second = parseDetailed(printed, path.string(), opts);
  REQUIRE_MESSAGE(second.ok(), "printed text does not parse:\n" << diagnostics(second, printed) << printed);
  CHECK_MESSAGE(second.file == first.file, "parse(print(f)) != f for " << path.string() << ":\n" << printed);
  const std::string reprinted = print(second.file);
  CHECK_MESSAGE(reprinted == printed, "print is not stable for " << path.string());
}

}  // namespace

TEST_CASE("parse/print: round trip over tests/graph/cases and stdlib") {
  size_t files = 0;
  for (const fs::path& dir : {text_util::casesDir(), text_util::stdlibDir()}) {
    text_util::DirLoader loader(dir);
    for (const fs::path& path : text_util::pqpFiles(dir)) {
      CAPTURE(path.string());
      roundTrip(path, loader);
      ++files;
    }
  }
  MESSAGE("round-tripped " << files << " files");
  CHECK(text_util::pqpFiles(text_util::casesDir()).size() >= 13);
  CHECK(text_util::pqpFiles(text_util::stdlibDir()).size() >= 1);
}

TEST_CASE("parse: the PATCH_LANGUAGE §4 example is the FmVoice patch built by hand in T1.1") {
  const PatchFile f = parseOk(text_util::readFile(text_util::casesDir() / "fm_voice.pqp"), "fm_voice.pqp");
  CHECK(f.imports == std::vector<std::string>{"lib/envelopes.pqp"});
  const Patch& p = only(f);
  CHECK(p == fm_voice::build());
  CHECK(p == fm_voice::build(true));
  CHECK(p.validate().ok());
  // And the hand-built patch prints to text that parses back to it.
  const PatchFile g = parseOk("import \"lib/envelopes.pqp\"\n" + print(fm_voice::build()));
  CHECK(only(g) == fm_voice::build());
}

TEST_CASE("parse: SPEC §7.3 SimpleFM") {
  const PatchFile f = parseOk(text_util::readFile(text_util::casesDir() / "simple_fm.pqp"), "simple_fm.pqp");
  const Patch& p = only(f);
  CHECK(p.name() == "SimpleFM");
  const NodeIndex hz = named(p, "hz"), gate = named(p, "gate"), env = named(p, "env");
  CHECK(p.outputType(hz, 0) == portType(Kind::Real, Rate::Audio, Poly::Voice));
  CHECK(p.outputType(gate, 0) == portType(Kind::Bool, Rate::Infer, Poly::Voice));
  CHECK(p.node(env).kind == NodeKind::Subcircuit);
  CHECK(p.subcircuit(env).patch == "adsr");
  CHECK(driverOf(p, env, 0) == PortRef{gate, 0});
  // a: 5ms keeps its unit.
  const PortRef a = driverOf(p, env, 1);
  REQUIRE(p.node(a.node).kind == NodeKind::Const);
  CHECK(p.constant(a.node) == Constant::real(5, Unit::ms));
  // 2 params, 2 ins, 1 out; mod: mul, phasor, sine; osc: mul, mul, add, phasor, sine; env: adsr + 4 consts; out: mul.
  CHECK(p.nodeCount() == 19);
}

TEST_CASE("parse: literals keep their unit; -literal is a negative literal; constant defaults are folded") {
  const PatchFile f = parseOk(R"(
    patch P {
      param a = 3/2
      param b = 2 * 5ms
      param c = -12dB [-60dB..0dB]
      param d : bool = false
      param e = -(4 - 1)
      out y : real
      x = -3
      z = -x
      w = 2.5kHz
      y = x + z + w + a + b + c + e + gate_to_real(d)
    })");
  const Patch& p = only(f);
  CHECK(p.paramDecl(named(p, "a")).defaultValue == Constant::real(1.5));
  CHECK(p.paramDecl(named(p, "b")).defaultValue == Constant::real(0.01));
  const ParamDecl& c = p.paramDecl(named(p, "c"));
  CHECK(c.defaultValue == Constant::real(-12, Unit::dB));
  CHECK(c.rangeMin == Constant::real(-60, Unit::dB));
  CHECK(c.rangeMax == Constant::real(0, Unit::dB));
  CHECK(p.paramDecl(named(p, "d")).kind == Kind::Bool);
  CHECK(p.paramDecl(named(p, "e")).defaultValue == Constant::real(-3));
  const NodeIndex x = named(p, "x"), z = named(p, "z"), w = named(p, "w");
  CHECK(p.node(x).kind == NodeKind::Const);
  CHECK(p.constant(x) == Constant::real(-3));
  CHECK(p.node(z).kind == NodeKind::Atom);
  CHECK(atomSignature(p.node(z).atom).name == "neg");
  CHECK(p.constant(w) == Constant::real(2.5, Unit::kHz));
  CHECK(p.constant(w).value() == doctest::Approx(2500.0));
}

TEST_CASE("parse: bindings are order-free; aliases have no node; a name can select an output") {
  const PatchFile f = parseOk(R"(
    patch P {
      in x : audio
      out y : audio
      out q : audio
      y = b * 2          // b is bound below
      b = a
      a = z1(a + x)       // feedback through z1
      s = Split(x).hi
      q = s + s.lo
    }
    patch Split {
      in x : audio
      out lo : audio
      out hi : audio
      half = x * 0.5
      lo = half
      hi = x - half
    })");
  const Patch& p = f.patches[0];
  CHECK_FALSE(p.findByName("b").has_value());  // alias
  const NodeIndex a = named(p, "a");
  CHECK(atomSignature(p.node(a).atom).name == "z1");
  const NodeIndex y = named(p, "y");
  const PortRef mulOut = driverOf(p, y, 0);
  CHECK(driverOf(p, mulOut.node, 0) == PortRef{a, 0});
  const NodeIndex s = named(p, "s");
  CHECK(p.node(s).kind == NodeKind::Subcircuit);
  CHECK(p.node(s).nameOutput == 1);  // hi
  const PortRef add = driverOf(p, named(p, "q"), 0);
  CHECK(driverOf(p, add.node, 0) == PortRef{s, 1});
  CHECK(driverOf(p, add.node, 1) == PortRef{s, 0});
}

TEST_CASE("parse: `node.input = expr` is the same graph as passing the argument") {
  const PatchFile a = parseOk("patch P { in x : audio  in g : bool  out y : audio\n h = samplehold(x)\n h.trig = g\n y = h }");
  const PatchFile b = parseOk("patch P { in x : audio  in g : bool  out y : audio\n h = samplehold(x, g)\n y = h }");
  CHECK(a == b);
}

TEST_CASE("parse: `in`/`out` are names where a name is expected (QUESTIONS Q7)") {
  const PatchFile f = parseOk(R"(
    patch P {
      in in : real
      out out : real
      t = track.in
      out = in + t[0]
    })");
  const Patch& p = only(f);
  CHECK(p.node(named(p, "in")).kind == NodeKind::Input);
  CHECK(p.node(named(p, "out")).kind == NodeKind::Output);
  CHECK(atomSignature(p.node(named(p, "t")).atom).name == "track.in");
}

TEST_CASE("parse: imports are recorded; calls into imported files resolve through the loader") {
  const PatchFile f = parseOk("import \"lib/envelopes.pqp\";\nimport \"multi_patch.pqp\"\n"
                              "patch P { out y : audio\n y = ar(voice.gate, r: 1s) * Osc(1).saw }");
  CHECK(f.imports == std::vector<std::string>{"lib/envelopes.pqp", "multi_patch.pqp"});
  const Patch& p = only(f);
  CHECK(p.validate().ok());
  // Without a loader the import is still recorded, but `ar` is unknown.
  const ParseResult r = parseDetailed("import \"lib/envelopes.pqp\"\npatch P { out y : audio\n y = ar(voice.gate) }", "x.pqp");
  REQUIRE_FALSE(r.ok());
  CHECK(r.diagnostics[0].message.find("unknown function 'ar'") != std::string::npos);
  // Externals (a prelude) need no import.
  const std::vector<SubcircuitRef> prelude = {fm_voice::adsrRef()};
  ParseOptions opts;
  opts.externals = prelude;
  const ParseResult e = parseDetailed("patch P { out y : audio\n y = adsr(voice.gate) }", "x.pqp", opts);
  CHECK_MESSAGE(e.ok(), (e.ok() ? std::string() : e.diagnostics[0].message));
}

TEST_CASE("print: canonical text of a small patch") {
  const PatchFile f = parseOk(R"(import "lib/envelopes.pqp"
patch Tone {
  out o : audio voice mono
  param amp = -6dB [-60dB..0dB] @label("Amp") @hidden
  in x : real = 3/2
  m = sine(phasor(voice.hz)) * amp
  o = (m + x) * -(m - 1) + (x > 0 ? 1 : 0) * ar(voice.gate, r: 50ms)
  export lfo = m
  solve(iterations: 2) { unknown v = 0
    v == m - tanh(v) }
  layout { node m at (1.5, -2) rot 90 flip
    comment "c\"q" at (0, 0) }
})");
  const std::string expected = R"(import "lib/envelopes.pqp"

patch Tone {
  param amp = -6dB [-60dB..0dB] @label("Amp") @hidden
  in x : real = 3 / 2
  out o : audio voice mono

  m = sine(phasor(voice.hz)) * amp
  o = (m + x) * -(m - 1) + (x > 0 ? 1 : 0) * ar(voice.gate, r: 50ms)

  export lfo = m

  solve(iterations: 2) {
    unknown v = 0
    v == m - tanh(v)
  }

  layout {
    node m at (1.5, -2) rot 90 flip
    comment "c\"q" at (0, 0)
  }
}
)";
  CHECK(print(f) == expected);
}

TEST_CASE("print: graphs text cannot express get generated names and still parse") {
  // An anonymous node with two consumers, a dead anonymous node, and an anonymous loop.
  Patch p("Gen");
  const NodeIndex x = p.addInput("x", portType(Kind::Real));
  const NodeIndex y = p.addOutput("y", portType(Kind::Real));
  const NodeIndex sq = p.addAtom(atomId("mul"));
  p.connect(x, sq, 0);
  p.connect(x, sq, 1);
  const NodeIndex sum = p.addAtom(atomId("add"));
  p.connect(sq, sum, 0);
  p.connect(sq, sum, 1);
  p.connect(sum, y, 0);
  const NodeIndex dead = p.addAtom(atomId("abs"));
  p.connect(x, dead, 0);
  const NodeIndex z = p.addAtom(atomId("z1"));
  const NodeIndex t = p.addAtom(atomId("tanh"));
  p.connect(z, t, 0);
  p.connect(t, z, 0);
  REQUIRE(p.validate().ok());
  const std::string text = print(p);
  CHECK(text.find("_n2 = x * x") != std::string::npos);
  CHECK(text.find("_n4 = abs(x)") != std::string::npos);
  const auto r = parse(text, "gen.pqp");
  REQUIRE_MESSAGE(r.ok(), r.error().message << "\n" << text);
  const Patch& q = r.value().patches[0];
  CHECK(q.nodeCount() == p.nodeCount());
  CHECK(q.wireCount() == p.wireCount());
  CHECK(print(q) == text);  // the generated names are now real names; printing is stable
}

TEST_CASE("print: a long anonymous chain is cut into named pieces; a parsed long chain is kept") {
  // Programmatic: 1000 nested negations of x (each node has one consumer).
  Patch p("Deep");
  const NodeIndex x = p.addInput("x", portType(Kind::Real));
  NodeIndex prev = x;
  for (int i = 0; i < 1000; ++i) {
    const NodeIndex n = p.addAtom(atomId("abs"));
    p.connect(prev, n, 0);
    prev = n;
  }
  const NodeIndex y = p.addOutput("y", portType(Kind::Real));
  p.connect(prev, y, 0);
  const std::string text = print(p);
  const auto r = parse(text, "deep.pqp");
  REQUIRE_MESSAGE(r.ok(), r.error().message);
  CHECK(r.value().patches[0].nodeCount() == p.nodeCount());

  // Parsed: a sum of kMaxExprDepth terms is the deepest expression the parser accepts; it round-trips as is.
  std::string src = "patch S { in x : real\n out y : real\n y = x";
  for (uint32_t i = 1; i < kMaxExprDepth; ++i) src += " + x";
  src += " }";
  const PatchFile f = parseOk(src);
  const std::string printed = print(f);
  CHECK(printed.find("_n") == std::string::npos);
  CHECK(parseOk(printed) == f);
}

TEST_CASE("print: empty file and imports only") {
  CHECK(print(PatchFile{}).empty());
  const PatchFile f = parseOk("import \"multi_patch.pqp\"");
  CHECK(print(f) == "import \"multi_patch.pqp\"\n");
  CHECK(parseOk(print(f)) == f);
}
