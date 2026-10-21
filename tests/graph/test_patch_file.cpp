#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <string>

#include "fm_voice.h"
#include "pq/graph/patch_file.h"

using namespace pq::graph;

namespace {

std::string errorsOf(const PatchFile& f) {
  std::string s;
  for (const pq::Error& e : f.validateAll()) s += e.message + "\n";
  return s;
}

bool hasError(const PatchFile& f, const std::string& needle) {
  for (const pq::Error& e : f.validateAll())
    if (e.message.find(needle) != std::string::npos) return true;
  return false;
}

// A stand-in `adsr` with the signature fm_voice::adsrRef() expects: out = gate_to_real(gate) * s.
Patch adsrPatch() {
  Patch p("adsr");
  const NodeIndex gate = p.addInput("gate", portType(Kind::Bool, Rate::Audio, Poly::Voice, kWidthMono));
  ParamDecl d;
  d.defaultValue = Constant::real(0.5);
  const NodeIndex a = p.addParam("a", d);
  p.addParam("d", d);
  const NodeIndex s = p.addParam("s", d);
  p.addParam("r", d);
  const NodeIndex out = p.addOutput("out", portType(Kind::Real, Rate::Audio, Poly::Voice, kWidthMono));
  const NodeIndex g2r = p.addAtom(atomId("gate_to_real"));
  const NodeIndex m = p.addAtom(atomId("mul"));
  p.connect(gate, g2r, 0);
  p.connect(g2r, m, 0);
  p.connect(s, m, 1);
  p.connect(m, out, 0);
  (void)a;
  return p;
}

PatchFile fileWithBoth() {
  PatchFile f;
  f.imports = {"stdlib/filters.pqp"};
  f.patches.push_back(adsrPatch());
  f.patches.push_back(fm_voice::build());
  return f;
}

}  // namespace

TEST_CASE("patch file: subcircuitSignature lists ins, params, outs and exports in order") {
  const SubcircuitRef sig = subcircuitSignature(adsrPatch());
  CHECK(sig == fm_voice::adsrRef());

  const SubcircuitRef fm = subcircuitSignature(fm_voice::build());
  CHECK(fm.patch == "FmVoice");
  REQUIRE(fm.inputs.size() == 4);  // no `in`s, four params
  CHECK(fm.inputs[0].name == "ratio");
  CHECK(fm.inputs[3].name == "release");
  CHECK(fm.inputs[0].optional);
  REQUIRE(fm.outputs.size() == 3);  // out, then exports
  CHECK(fm.outputs[0].name == "out");
  CHECK(fm.outputs[1].name == "modulator");
  CHECK(fm.outputs[2].name == "envelope");

  // An `in` with a default (a wire into its "default" input) is optional for callers.
  Patch p("WithDefault");
  const NodeIndex x = p.addInput("x", portType(Kind::Real));
  const NodeIndex y = p.addInput("y", portType(Kind::Real));
  const NodeIndex hz = p.addAtom(atomId("voice.hz"));
  p.connect(hz, x, 0);  // in x : real = voice.hz
  const NodeIndex o = p.addOutput("o", portType(Kind::Real));
  const NodeIndex add = p.addAtom(atomId("add"));
  p.connect(x, add, 0);
  p.connect(y, add, 1);
  p.connect(add, o, 0);
  CHECK(p.validate().ok());
  const SubcircuitRef w = subcircuitSignature(p);
  CHECK(w.inputs[0].optional);
  CHECK_FALSE(w.inputs[1].optional);
}

TEST_CASE("patch file: a file whose calls match their callees validates") {
  const PatchFile f = fileWithBoth();
  CHECK_MESSAGE(f.validateAll().empty(), errorsOf(f));
  CHECK(f.find("adsr") == &f.patches[0]);
  CHECK(f.find("nope") == nullptr);
  CHECK(f == fileWithBoth());
  PatchFile g = fileWithBoth();
  g.imports.push_back("x.pqp");
  CHECK_FALSE(f == g);
}

TEST_CASE("patch file: a call into an imported patch is not checked locally") {
  PatchFile f;
  f.patches.push_back(fm_voice::build());  // calls adsr, which is not in this file
  CHECK_MESSAGE(f.validateAll().empty(), errorsOf(f));
}

TEST_CASE("patch file: stale subcircuit signatures, duplicates, recursion, bad imports") {
  SUBCASE("callee changed after instantiation") {
    PatchFile f = fileWithBoth();
    f.patches[0].addParam("extra", ParamDecl{});
    CHECK(hasError(f, "patch 'FmVoice': 'env' (subcircuit adsr, #19) does not match the current ports of patch 'adsr'"));
  }
  SUBCASE("duplicate patch names") {
    PatchFile f = fileWithBoth();
    f.patches.push_back(adsrPatch());
    CHECK(hasError(f, "patch 'adsr' is defined twice"));
  }
  SUBCASE("empty import") {
    PatchFile f = fileWithBoth();
    f.imports.push_back("");
    CHECK(hasError(f, "empty import path"));
  }
  SUBCASE("a patch error is reported through the file") {
    PatchFile f = fileWithBoth();
    f.patches[1].addOutput("dangling", portType(Kind::Real));
    CHECK(hasError(f, "patch 'FmVoice': dangling port: input 'in' of 'dangling'"));
  }
  SUBCASE("direct recursion") {
    PatchFile f;
    Patch p("Loop");
    const NodeIndex o = p.addOutput("o", portType(Kind::Real));
    f.patches.push_back(p);
    const NodeIndex self = f.patches[0].addSubcircuit(subcircuitSignature(f.patches[0]));
    f.patches[0].connect(self, o, 0);
    CHECK(hasError(f, "patch 'Loop' instantiates itself"));
  }
  SUBCASE("indirect recursion") {
    // A calls B, B calls A (signatures: each has one out o).
    PatchFile f;
    f.patches.emplace_back("A");
    f.patches.emplace_back("B");
    for (Patch& p : f.patches) p.addOutput("o", portType(Kind::Real));
    const SubcircuitRef sigA = subcircuitSignature(f.patches[0]), sigB = subcircuitSignature(f.patches[1]);
    const NodeIndex callB = f.patches[0].addSubcircuit(sigB);
    f.patches[0].connect(callB, 0, 0);
    const NodeIndex callA = f.patches[1].addSubcircuit(sigA);
    f.patches[1].connect(callA, 0, 0);
    CHECK(hasError(f, "instantiates itself"));
    CHECK_FALSE(f.validate().ok());
  }
}
