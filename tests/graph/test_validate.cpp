#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <cmath>
#include <limits>
#include <string>

#include "fm_voice.h"
#include "pq/graph/patch.h"

using namespace pq::graph;

namespace {

std::string errorsOf(const Patch& p) {
  std::string s;
  for (const pq::Error& e : p.validateAll()) s += e.message + "\n";
  return s;
}

// True if some error contains `needle` (and has the given code, if not Unknown).
bool hasError(const Patch& p, const std::string& needle, pq::ErrorCode code = pq::ErrorCode::Unknown) {
  for (const pq::Error& e : p.validateAll())
    if (e.message.find(needle) != std::string::npos && (code == pq::ErrorCode::Unknown || e.code == code)) return true;
  return false;
}

#define CHECK_ERROR(p, needle) CHECK_MESSAGE(hasError(p, needle), "expected '", needle, "' in:\n", errorsOf(p))
#define CHECK_ERROR_CODE(p, needle, code) \
  CHECK_MESSAGE(hasError(p, needle, code), "expected '", needle, "' in:\n", errorsOf(p))
#define CHECK_VALID(p) CHECK_MESSAGE((p).validateAll().empty(), errorsOf(p))

const PortType kRealT = portType(Kind::Real);
const PortType kBoolT = portType(Kind::Bool);

// out o : real = c (a const), the smallest valid patch.
Patch minimal() {
  Patch p("Min");
  const NodeIndex c = p.addConst(Constant::real(1));
  const NodeIndex o = p.addOutput("o", kRealT);
  p.connect(c, o, 0);
  return p;
}

}  // namespace

TEST_CASE("validate: a minimal patch is valid") {
  const Patch p = minimal();
  CHECK_VALID(p);
}

TEST_CASE("validate: dangling wires (endpoints that do not exist) are rejected") {
  SUBCASE("source node") {
    Patch p = minimal();
    p.connect(PortRef{57, 0}, PortRef{1, 0});
    CHECK_ERROR_CODE(p, "dangling wire #1: source node #57 does not exist", pq::ErrorCode::NotFound);
  }
  SUBCASE("source port") {
    Patch p = minimal();
    p.disconnect(0);
    p.connect(PortRef{0, 3}, PortRef{1, 0});
    CHECK_ERROR(p, "has no output #3");
  }
  SUBCASE("target node") {
    Patch p = minimal();
    p.connect(PortRef{0, 0}, PortRef{12, 0});
    CHECK_ERROR(p, "target node #12 does not exist");
  }
  SUBCASE("target port") {
    Patch p = minimal();
    p.connect(PortRef{0, 0}, PortRef{1, 1});
    CHECK_ERROR(p, "'o' (out, #1) has no input #1");
  }
  SUBCASE("a const has no inputs") {
    Patch p = minimal();
    const NodeIndex c2 = p.addConst(Constant::real(2));
    p.connect(PortRef{0, 0}, PortRef{c2, 0});
    CHECK_ERROR(p, "has no input #0");
  }
}

TEST_CASE("validate: dangling ports (required inputs left unconnected) are rejected") {
  SUBCASE("atom input without default") {
    Patch p = minimal();
    const NodeIndex m = p.addAtom(atomId("mul"));
    p.connect(0, m, 0);
    CHECK_ERROR(p, "dangling port: input 'b' of (atom mul, #2) is not connected");
    CHECK_FALSE(hasError(p, "input 'a'"));
  }
  SUBCASE("out declaration") {
    Patch p("P");
    p.addOutput("o", kRealT);
    CHECK_ERROR(p, "dangling port: input 'in' of 'o' (out, #0)");
  }
  SUBCASE("export") {
    Patch p = minimal();
    p.addExport("e");
    CHECK_ERROR(p, "input 'in' of 'e' (export, #2)");
  }
  SUBCASE("equation sides") {
    Patch p = minimal();
    const SolveIndex s = p.addSolve(2);
    const NodeIndex u = p.addUnknown("u", s);
    const NodeIndex e = p.addEquation(s);
    p.connect(u, e, 0);
    CHECK_ERROR(p, "input 'rhs' of (equation");
  }
  SUBCASE("subcircuit input without default in the callee") {
    Patch p = minimal();
    const NodeIndex env = p.addSubcircuit(fm_voice::adsrRef());
    (void)env;
    CHECK_ERROR(p, "input 'gate' of (subcircuit adsr");
    CHECK_FALSE(hasError(p, "input 'a' of"));  // params are optional
  }
  SUBCASE("optional inputs may stay unconnected") {
    Patch p = minimal();
    const NodeIndex ph = p.addAtom(atomId("phasor"));
    p.connect(0, ph, 0);                                 // hz connected, reset (optional) not
    const NodeIndex in = p.addInput("x", kRealT);        // an `in` without a default
    const SolveIndex s = p.addSolve(1);
    const NodeIndex u = p.addUnknown("u", s);            // no initial guess
    const NodeIndex e = p.addEquation(s);
    p.connect(u, e, 0);
    p.connect(in, e, 1);
    (void)ph;
    CHECK_VALID(p);
  }
}

TEST_CASE("validate: an input may have only one driver") {
  Patch p = minimal();
  const NodeIndex c2 = p.addConst(Constant::real(2));
  p.connect(c2, 1, 0);
  CHECK_ERROR(p, "input 'in' of 'o' (out, #1) is driven by more than one wire");
}

TEST_CASE("validate: kind mismatches need explicit conversion (no implicit bool <-> real)") {
  SUBCASE("bool into real") {
    Patch p("P");
    const NodeIndex gate = p.addAtom(atomId("voice.gate"));
    const NodeIndex m = p.addAtom(atomId("mul"));
    const NodeIndex o = p.addOutput("o", kRealT);
    p.connect(gate, m, 0);
    p.connect(gate, m, 1);
    p.connect(m, o, 0);
    CHECK_ERROR_CODE(p, "kind mismatch: bool into real: output 'out' of (atom voice.gate, #0) → input 'a' of (atom mul",
                     pq::ErrorCode::Type);
    CHECK_ERROR(p, "(convert with gate_to_real)");
  }
  SUBCASE("bool into real through gate_to_real is fine") {
    Patch p("P");
    const NodeIndex gate = p.addAtom(atomId("voice.gate"));
    const NodeIndex conv = p.addAtom(atomId("gate_to_real"));
    const NodeIndex o = p.addOutput("o", kRealT);
    p.connect(gate, conv, 0);
    p.connect(conv, o, 0);
    CHECK_VALID(p);
  }
  SUBCASE("real into bool") {
    Patch p("P");
    const NodeIndex x = p.addInput("x", kRealT);
    const NodeIndex n = p.addAtom(atomId("not"));
    const NodeIndex o = p.addOutput("o", kBoolT);
    p.connect(x, n, 0);
    p.connect(n, o, 0);
    CHECK_ERROR(p, "kind mismatch: real into bool");
    CHECK_ERROR(p, "(convert with a comparison)");
  }
  SUBCASE("bool param into a real out") {
    Patch p("P");
    ParamDecl d;
    d.kind = Kind::Bool;
    d.defaultValue = Constant::boolean(true);
    const NodeIndex b = p.addParam("on", d);
    const NodeIndex o = p.addOutput("o", kRealT);
    p.connect(b, o, 0);
    CHECK_ERROR(p, "kind mismatch: bool into real");
  }
  SUBCASE("kind-polymorphic atoms accept either kind") {
    Patch p("P");
    const NodeIndex gate = p.addAtom(atomId("voice.gate"));
    const NodeIndex z = p.addAtom(atomId("z1"));
    const NodeIndex o = p.addOutput("o", kBoolT);
    p.connect(gate, z, 0);
    p.connect(z, o, 0);
    CHECK_VALID(p);
  }
}

TEST_CASE("validate: event rate only connects to event rate") {
  Patch p("P");
  const NodeIndex ev = p.addInput("notes", portType(Kind::Real, Rate::Event));
  const NodeIndex o = p.addOutput("o", portType(Kind::Real, Rate::Audio));
  const NodeIndex o2 = p.addOutput("o2", portType(Kind::Real, Rate::Event));
  p.connect(ev, o, 0);
  p.connect(ev, o2, 0);
  CHECK_ERROR_CODE(p, "rate mismatch: event into audio", pq::ErrorCode::Type);
  CHECK(p.validateAll().size() == 1);  // event → event is fine
}

TEST_CASE("validate: rate and poly lifts are left to inference") {
  // An audio-rate, per-voice signal into a param-declared global input: modulation (T1.3 raises the rate) and a
  // poly→global sum (T1.3 inserts the sum). Neither is a T1.1 error.
  Patch p("P");
  const NodeIndex hz = p.addAtom(atomId("voice.hz"));
  const NodeIndex gate = p.addAtom(atomId("voice.gate"));
  const NodeIndex env = p.addSubcircuit(fm_voice::adsrRef());
  const NodeIndex o = p.addOutput("o", portType(Kind::Real, Rate::Audio, Poly::Global, kWidthMono));
  p.connect(gate, env, 0);
  p.connect(hz, env, 1);  // a: param global ← audio voice
  p.connect(env, o, 0);   // voice → global
  CHECK_VALID(p);
}

TEST_CASE("validate: width mismatches (mono broadcasts, others must match)") {
  auto patchWith = [](uint16_t from, uint16_t to) {
    Patch p("P");
    const NodeIndex i = p.addInput("x", portType(Kind::Real, Rate::Audio, Poly::Infer, from));
    const NodeIndex o = p.addOutput("o", portType(Kind::Real, Rate::Audio, Poly::Infer, to));
    p.connect(i, o, 0);
    return p;
  };
  CHECK_VALID(patchWith(kWidthMono, kWidthStereo));
  CHECK_VALID(patchWith(kWidthStereo, kWidthStereo));
  CHECK_VALID(patchWith(kWidthInfer, kWidthStereo));
  CHECK_VALID(patchWith(kWidthStereo, kWidthInfer));
  CHECK_ERROR_CODE(patchWith(kWidthStereo, kWidthMono), "width mismatch: 2 channels into 1", pq::ErrorCode::Type);
  CHECK_ERROR(patchWith(4, kWidthStereo), "width mismatch: 4 channels into 2");

  // x[0] (channel select) makes a stereo input mono.
  Patch p("P");
  const NodeIndex i = p.addInput("x", portType(Kind::Real, Rate::Audio, Poly::Infer, kWidthStereo));
  const NodeIndex idx = p.addConst(Constant::real(0));
  const NodeIndex ch = p.addAtom(atomId("channel"));
  const NodeIndex o = p.addOutput("o", portType(Kind::Real, Rate::Audio, Poly::Infer, kWidthMono));
  p.connect(i, ch, 0);
  p.connect(idx, ch, 1);
  p.connect(ch, o, 0);
  CHECK_VALID(p);
}

TEST_CASE("validate: names") {
  SUBCASE("duplicate") {
    Patch p = minimal();
    const NodeIndex c = p.addConst(Constant::real(3));
    p.setNodeName(c, "o");
    CHECK_ERROR(p, "name 'o' is used by 'o' (out, #1) and 'o' (const, #2)");
  }
  SUBCASE("keyword") {
    Patch p = minimal();
    p.setNodeName(0, "export");
    CHECK_ERROR(p, "'export' is a keyword");
  }
  SUBCASE("built-in namespace") {
    Patch p = minimal();
    p.setNodeName(0, "voice");
    CHECK_ERROR(p, "'voice' would shadow a built-in namespace");
    p.setNodeName(0, "sr");
    CHECK_ERROR(p, "'sr' would shadow");
  }
  SUBCASE("not an identifier") {
    Patch p = minimal();
    p.setNodeName(0, "2x");
    CHECK_ERROR(p, "'2x' is not a valid identifier");
  }
  SUBCASE("declarations must be named; equations cannot be") {
    Patch p("P");
    const NodeIndex c = p.addConst(Constant::real(1));
    const NodeIndex o = p.addOutput("", kRealT);
    p.connect(c, o, 0);
    CHECK_ERROR(p, "(out, #1) must be named");
    const SolveIndex s = p.addSolve(1);
    const NodeIndex u = p.addUnknown("u", s);
    const NodeIndex e = p.addEquation(s);
    p.connect(u, e, 0);
    p.connect(c, e, 1);
    p.setNodeName(e, "eq1");
    CHECK_ERROR(p, "equations cannot be named");
  }
  SUBCASE("a name bound to one output") {
    Patch p = minimal();
    const NodeIndex demux = p.addAtom(atomId("demux"));
    p.connect(0, demux, 0);
    p.connect(0, demux, 1);
    p.setNodeName(demux, "lo", 1);  // `lo = demux(i, x).out` style binding to output 1
    CHECK_VALID(p);
    p.setNodeName(demux, "lo", 2);
    CHECK_ERROR(p, "name 'lo' binds a nonexistent output");
    p.setNodeName(demux, "lo", 1);
    p.setNodeName(1, "o", 0);  // declarations name the whole node
    CHECK_ERROR(p, "name 'o' binds a nonexistent output");
  }
  SUBCASE("patch name") {
    Patch p = minimal();
    p.setName("my patch");
    CHECK_ERROR(p, "patch name 'my patch' is not a valid identifier");
  }
}

TEST_CASE("validate: params") {
  auto withParam = [](ParamDecl d) {
    Patch p = minimal();
    p.addParam("k", std::move(d));
    return p;
  };
  ParamDecl ok;
  ok.defaultValue = Constant::real(300, Unit::ms);
  ok.hasRange = true;
  ok.rangeMin = Constant::real(1, Unit::ms);
  ok.rangeMax = Constant::real(30, Unit::s);
  ok.attrs = {Attr{"curve", {AttrArg{"", AttrValue::ident("exp")}}},
              Attr{"hidden", {}},
              Attr{"steps", {AttrArg{"n", AttrValue::num(4)}}}};
  CHECK_VALID(withParam(ok));

  ParamDecl d = ok;
  d.rangeMin = Constant::real(40, Unit::s);
  CHECK_ERROR(withParam(d), "range min is above range max");
  d = ok;
  d.defaultValue = Constant::real(31, Unit::s);
  CHECK_ERROR(withParam(d), "default is outside the range");
  d = ok;
  d.defaultValue = Constant::real(0.5);  // 0.5 s: inside [1 ms, 30 s] after normalisation
  CHECK_VALID(withParam(d));
  d = ok;
  d.defaultValue = Constant::boolean(true);
  CHECK_ERROR_CODE(withParam(d), "default is bool, expected real", pq::ErrorCode::Type);
  d = ok;
  d.kind = Kind::Bool;
  d.defaultValue = Constant::boolean(false);
  CHECK_ERROR(withParam(d), "only real params have a range");
  d = ok;
  d.kind = Kind::Infer;
  CHECK_ERROR(withParam(d), "must be real or bool");
  d = ok;
  d.defaultValue = Constant::real(std::numeric_limits<double>::infinity());
  CHECK_ERROR(withParam(d), "default is not finite");
  d = ok;
  d.attrs.push_back(Attr{"bad name", {}});
  CHECK_ERROR(withParam(d), "attribute @bad name: the name is invalid");
  d = ok;
  d.attrs.push_back(Attr{"unit", {AttrArg{"", AttrValue::ident("not ident")}}});
  CHECK_ERROR(withParam(d), "invalid identifier argument 'not ident'");
  d = ok;
  d.attrs.push_back(Attr{"x", {AttrArg{"1a", AttrValue::num(1)}}});
  CHECK_ERROR(withParam(d), "invalid argument name '1a'");
  d = ok;
  d.attrs.push_back(Attr{"flag", {AttrArg{"", AttrValue{AttrValue::Type::Bool, Constant::real(2), {}}}}});
  CHECK_ERROR(withParam(d), "argument is real, expected bool");
  // Never in samples (M1 review L1): a param is read as seconds at a duration input, so 100smp would be 100 s.
  d = ok;
  d.defaultValue = Constant::real(0.01, Unit::smp);
  CHECK_ERROR_CODE(withParam(d), "cannot be in samples (smp)", pq::ErrorCode::InvalidArgument);
  d = ok;
  d.rangeMax = Constant::real(48000, Unit::smp);
  CHECK_ERROR(withParam(d), "cannot be in samples (smp)");
  d = ok;
  d.rangeMin = Constant::real(0, Unit::smp);
  CHECK_ERROR(withParam(d), "cannot be in samples (smp)");
}

TEST_CASE("validate: constants and declared kinds") {
  Patch p("P");
  const NodeIndex c = p.addConst(Constant::real(std::nan("")));
  const NodeIndex b = p.addConst(Constant{Kind::Bool, Unit::None, 2.0});
  const NodeIndex o = p.addOutput("o", kRealT);
  const NodeIndex ob = p.addOutput("ob", kBoolT);
  p.connect(c, o, 0);
  p.connect(b, ob, 0);
  CHECK_ERROR(p, "constant (const, #0) is not finite");
  CHECK_ERROR(p, "constant (const, #1) is not a bool literal");

  Patch q("Q");
  const NodeIndex in = q.addInput("x", portType(Kind::Infer, Rate::Audio));
  const NodeIndex out = q.addOutput("y", portType(Kind::Infer));
  q.connect(in, out, 0);
  CHECK_ERROR(q, "'x' (in, #0) needs a declared kind");
  CHECK_ERROR(q, "'y' (out, #1) needs a declared kind");
}

TEST_CASE("validate: atom port counts") {
  Patch p = minimal();
  const NodeIndex mux = p.addAtom(atomId("mux"), 1);  // below the minimum of 2 repeated inputs
  p.connect(0, mux, 0);
  p.connect(0, mux, 1);
  CHECK_ERROR(p, "(atom mux, #2) has the wrong number of ports for its atom");
}

TEST_CASE("validate: subcircuit references") {
  Patch p = minimal();
  SubcircuitRef r = fm_voice::adsrRef();
  r.patch = "patch";
  r.inputs[1].name = "gate";
  const NodeIndex s = p.addSubcircuit(r);
  (void)s;
  CHECK_ERROR(p, "calls a patch with an invalid name");
  CHECK_ERROR(p, "has duplicate input names");
}

TEST_CASE("validate: layout") {
  Patch p = minimal();
  NodeLayout l;
  l.placed = true;
  l.x = 10;
  l.y = 20;
  l.rotation = 90;
  l.flip = true;
  p.setLayout(1, l);  // the named out
  CHECK_VALID(p);
  p.setLayout(0, l);  // the anonymous const
  CHECK_ERROR(p, "(const, #0) has a layout position but no name");
  p.setNodeName(0, "k");
  CHECK_VALID(p);
  l.rotation = 45;
  p.setLayout(0, l);
  CHECK_ERROR(p, "has rotation 45 (must be 0, 90, 180 or 270)");
  l.rotation = 0;
  l.x = std::numeric_limits<float>::quiet_NaN();
  p.setLayout(0, l);
  CHECK_ERROR(p, "non-finite layout position");

  Patch q = minimal();
  q.setWireVia(0, {{1, 2}, {std::numeric_limits<float>::infinity(), 0}});
  CHECK_ERROR(q, "wire #0 has a non-finite routing point");
  q.setWireVia(0, {{1, 2}, {3, 4}});
  q.addComment({"hello", {std::numeric_limits<float>::quiet_NaN(), 0}});
  CHECK_ERROR(q, "layout comment 'hello' has a non-finite position");
}

TEST_CASE("validate: solve blocks") {
  auto solvePatch = [](uint32_t iterations, int unknowns, int equations) {
    Patch p = minimal();
    const SolveIndex s = p.addSolve(iterations);
    for (int i = 0; i < unknowns; ++i) p.addUnknown("u" + std::to_string(i), s);
    for (int i = 0; i < equations; ++i) {
      const NodeIndex e = p.addEquation(s);
      p.connect(0, e, 0);
      p.connect(0, e, 1);
    }
    return p;
  };
  CHECK_VALID(solvePatch(1, 1, 1));
  CHECK_VALID(solvePatch(kMaxSolveIterations, 2, 2));
  CHECK_ERROR(solvePatch(0, 1, 1), "solve block #0 has 0 iterations (must be 1..64)");
  CHECK_ERROR(solvePatch(65, 1, 1), "has 65 iterations");
  CHECK_ERROR(solvePatch(4, 2, 1), "solve block #0 has 2 unknowns but 1 equations");
  CHECK_ERROR(solvePatch(4, 0, 0), "solve block #0 has no unknowns");

  Patch p = minimal();
  p.addUnknown("u", 3);
  CHECK_ERROR_CODE(p, "'u' (unknown, #2) belongs to a nonexistent solve block", pq::ErrorCode::NotFound);
}

TEST_CASE("validate: validateAll reports every problem; validate returns the first") {
  Patch p("P");
  const NodeIndex gate = p.addAtom(atomId("voice.gate"));
  const NodeIndex m = p.addAtom(atomId("mul"));
  p.connect(gate, m, 0);  // kind mismatch
  p.connect(PortRef{gate, 0}, PortRef{40, 0});  // dangling wire
  p.addOutput("o", kRealT);  // dangling port, and m.b dangling
  const auto all = p.validateAll();
  CHECK(all.size() == 4);
  const pq::Status first = p.validate();
  REQUIRE_FALSE(first.ok());
  CHECK(first.error().message == all.front().message);
  CHECK(first.error().message.starts_with("patch 'P': "));
}
