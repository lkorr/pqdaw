// T4.4: the circuit editor's IR edits (pq/graph/edit.h) and the parser options they rely on (deferMissingInputs,
// ParseResult::lowered, PatchTextSpans::begin, NodeIds inherited through validation errors). Every edit is printed and
// re-parsed, as the editor does, and must give back the same graph.

#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <string>

#include "pq/graph/edit.h"
#include "pq/graph/infer.h"
#include "pq/graph/parse.h"

using namespace pq::graph;

namespace {

ParseResult lenient(const std::string& text, const PatchFile* previous = nullptr) {
  ParseOptions o;
  o.deferMissingInputs = true;
  o.previous = previous;
  return parseDetailed(text, "edit.pqp", o);
}

PatchFile strict(const std::string& text) {
  ParseResult r = parseDetailed(text, "edit.pqp");
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : formatDiagnostic(text, r.diagnostics.front()) + "\n" + text));
  return std::move(r.file);
}

NodeIndex named(const Patch& p, std::string_view n) {
  const auto v = p.findByName(n);
  REQUIRE_MESSAGE(v.has_value(), "no node " << std::string(n));
  return *v;
}

NodeIndex atomNode(const Patch& p, std::string_view atom) {
  for (NodeIndex v = 0; v < p.nodeCount(); ++v)
    if (p.node(v).kind == NodeKind::Atom && atomSignature(p.node(v).atom).name == atom) return v;
  FAIL("no atom " << std::string(atom));
  return kNone;
}

// print → parse round trip of a whole file: the same graph back.
void roundTrip(const PatchFile& f) {
  const std::string text = print(f);
  ParseResult r = lenient(text);
  REQUIRE_MESSAGE(r.lowered, (r.diagnostics.empty() ? text : formatDiagnostic(text, r.diagnostics.front()) + "\n" + text));
  for (Patch& p : r.file.patches) edit::anonymize(p);
  CHECK_MESSAGE(r.file == f, text);
}

}  // namespace

TEST_CASE("edit: a call without a required argument lowers with deferMissingInputs; validation reports the port") {
  const std::string text = "patch P {\n  out y : audio\n  t = tanh()\n  y = t\n}\n";
  const ParseResult hard = parseDetailed(text, "edit.pqp");
  CHECK_FALSE(hard.lowered);
  REQUIRE_FALSE(hard.diagnostics.empty());
  const ParseResult soft = lenient(text);
  CHECK(soft.lowered);
  REQUIRE(soft.diagnostics.size() == 1);
  CHECK(soft.diagnostics[0].message.find("dangling port") != std::string::npos);
  // Located at the call.
  CHECK(text.substr(soft.diagnostics[0].offset, 4) == "tanh");
  CHECK(soft.patchSpans[0].begin == 0);
  CHECK(text[soft.patchSpans[0].close] == '}');
  // NodeIds survive a re-parse that still has the validation error.
  const ParseResult again = lenient("// moved\n" + text, &soft.file);
  CHECK(again.lowered);
  CHECK(again.idMatch.matched == again.idMatch.nodes);
}

TEST_CASE("edit: connect replaces the input's driver, refuses bool into real, prints back to the same graph") {
  PatchFile f = strict("patch P {\n  in x : audio\n  in g : bool\n  out y : audio\n  y = tanh(x)\n}\n");
  Patch& p = f.patches[0];
  const NodeIndex t = atomNode(p, "tanh"), x = named(p, "x"), g = named(p, "g");
  const NodeIndex m = *edit::addNode(p, "*", {});
  auto c = edit::connect(p, {x, 0}, {m, 0});
  REQUIRE(c.ok());
  CHECK_FALSE(c->replaced);
  CHECK(edit::connect(p, {x, 0}, {m, 1}).ok());
  c = edit::connect(p, {m, 0}, {t, 0});
  REQUIRE(c.ok());
  CHECK(c->replaced);
  CHECK(c->delay == kNone);
  CHECK_FALSE(edit::connect(p, {g, 0}, {m, 0}).ok());
  CHECK(p.validate().ok());
  roundTrip(f);
  CHECK(print(f).find("y = tanh(x * x)") != std::string::npos);
}

TEST_CASE("edit: a wire that closes a delay-free cycle gets a z1; one through a delayed input does not") {
  PatchFile f = strict("patch P {\n  in x : audio\n  out y : audio\n  s = x + 0\n  y = tanh(s)\n}\n");
  Patch& p = f.patches[0];
  const NodeIndex t = atomNode(p, "tanh"), s = named(p, "s");
  // tanh → s.b closes s → tanh → s with no delay.
  CHECK(edit::closesDelayFreeCycle(p, {t, 0}, {s, 1}));
  auto c = edit::connect(p, {t, 0}, {s, 1});
  REQUIRE(c.ok());
  REQUIRE(c->delay != kNone);
  CHECK(atomSignature(p.node(c->delay).atom).name == "z1");
  CHECK(p.validate().ok());
  // The text is valid feedback: it parses strictly and infers.
  const std::string text = print(f);
  CHECK(text.find("z1(") != std::string::npos);
  (void)strict(text);
  // Into z1.x itself: ordinary feedback, no second z1.
  const NodeIndex z = c->delay;
  CHECK_FALSE(edit::closesDelayFreeCycle(p, {t, 0}, {z, 0}));
  const uint32_t before = p.nodeCount();
  auto c2 = edit::connect(p, {s, 0}, {z, 0});
  REQUIRE(c2.ok());
  CHECK(c2->delay == kNone);
  CHECK(p.nodeCount() == before);
  // A self-loop on a delay-free atom.
  CHECK(edit::closesDelayFreeCycle(p, {t, 0}, {t, 0}));
}

TEST_CASE("edit: addNode reads literals with units, atoms, operators, callees and declarations") {
  PatchFile f = strict("patch Inner {\n  in a : audio\n  out b : audio\n  b = a\n}\npatch P {\n  out y : audio\n  y = 0\n}\n");
  Patch& p = f.patches[1];
  const auto sigs = signatures(f);
  auto n = edit::addNode(p, "440Hz", sigs);
  REQUIRE(n.ok());
  CHECK(p.node(*n).kind == NodeKind::Const);
  CHECK(p.constant(*n).unit == Unit::Hz);
  CHECK(p.constant(*n).value() == doctest::Approx(440.0));
  n = edit::addNode(p, "-3dB", sigs);
  REQUIRE(n.ok());
  CHECK(p.constant(*n).value() == doctest::Approx(0.70794578).epsilon(1e-6));
  n = edit::addNode(p, "true", sigs);
  REQUIRE(n.ok());
  CHECK(p.constant(*n).kind == Kind::Bool);
  n = edit::addNode(p, "fast_tanh", sigs);
  REQUIRE(n.ok());
  CHECK(atomSignature(p.node(*n).atom).name == "fast_tanh");
  n = edit::addNode(p, "<=", sigs);
  REQUIRE(n.ok());
  CHECK(atomSignature(p.node(*n).atom).name == "le");
  n = edit::addNode(p, "Inner", sigs);
  REQUIRE(n.ok());
  CHECK(p.node(*n).kind == NodeKind::Subcircuit);
  n = edit::addNode(p, "param cutoff = 1kHz [20Hz..20kHz] @label(\"Cutoff\")", sigs);
  REQUIRE(n.ok());
  CHECK(p.node(*n).kind == NodeKind::Param);
  CHECK(p.nodeName(*n) == "cutoff");
  CHECK(p.paramDecl(*n).hasRange);
  n = edit::addNode(p, "in gate : bool voice", sigs);
  REQUIRE(n.ok());
  CHECK(p.outputType(*n, 0).kind == Kind::Bool);
  CHECK(p.outputType(*n, 0).poly == Poly::Voice);
  n = edit::addNode(p, "export env", sigs);  // undriven: `export env` (T4.4)
  REQUIRE(n.ok());
  CHECK(p.node(*n).kind == NodeKind::Export);
  CHECK_FALSE(edit::addNode(p, "param cutoff = 2", sigs).ok());  // taken
  CHECK_FALSE(edit::addNode(p, "nosuchthing", sigs).ok());
  CHECK_FALSE(edit::addNode(p, "", sigs).ok());
  // Unconnected nodes and an undriven export still print and lower to the same graph.
  roundTrip(f);
}

TEST_CASE("edit: generated names are anonymized so a single consumer inlines the node again") {
  PatchFile f = strict("patch P {\n  in x : audio\n  out y : audio\n  y = x\n}\n");
  Patch& p = f.patches[0];
  const NodeIndex t = *edit::addNode(p, "tanh", {});
  std::string text = print(f);
  CHECK(text.find("_n") != std::string::npos);  // unconnected: the printer must name it
  ParseResult r = lenient(text);
  REQUIRE(r.lowered);
  Patch& q = r.file.patches[0];
  CHECK(edit::anonymize(q) == 1);
  const NodeIndex t2 = atomNode(q, "tanh");
  REQUIRE(edit::connect(q, {named(q, "x"), 0}, {t2, 0}).ok());
  REQUIRE(edit::connect(q, {t2, 0}, {named(q, "y"), 0}).ok());
  text = print(r.file);
  CHECK(text.find("y = tanh(x)") != std::string::npos);
  CHECK(text.find("_n") == std::string::npos);
  (void)t;
  CHECK(edit::isGeneratedName("_n12"));
  CHECK_FALSE(edit::isGeneratedName("_n"));
  CHECK_FALSE(edit::isGeneratedName("_nx"));
  CHECK_FALSE(edit::checkName(q, "_n3").ok());
  CHECK_FALSE(edit::checkName(q, "voice").ok());
  CHECK_FALSE(edit::checkName(q, "x").ok());
  CHECK(edit::checkName(q, "x", named(q, "x")).ok());
}

TEST_CASE("edit: renaming a declaration carries into the callers in the same file") {
  PatchFile f = strict(
      "patch Gain {\n  in x : audio\n  param amount = 0.5\n  param bias = 0\n  out y : audio\n  y = x * amount + bias\n}\n"
      "patch P {\n  in a : audio\n  out b : audio\n  b = Gain(a, bias: 0.25)\n}\n");
  auto changed = edit::rename(f, 0, named(f.patches[0], "bias"), "level");
  REQUIRE(changed.ok());
  CHECK(changed->size() == 2);
  const std::string text = print(f);
  CHECK_MESSAGE(text.find("level: 0.25") != std::string::npos, text);
  const PatchFile back = strict(text);
  CHECK(back == f);
  // Bad names are refused, a declaration cannot lose its name, a binding can.
  CHECK_FALSE(edit::rename(f, 0, named(f.patches[0], "level"), "2x").ok());
  CHECK_FALSE(edit::rename(f, 0, named(f.patches[0], "level"), "").ok());
}

TEST_CASE("edit: removing or adding a declaration refreshes the callers' copies by port name") {
  PatchFile f = strict(
      "patch Gain {\n  in x : audio\n  param amount = 0.5\n  out y : audio\n  y = x * amount\n}\n"
      "patch P {\n  in a : audio\n  out b : audio\n  b = Gain(a, amount: 0.25)\n}\n");
  Patch& g = f.patches[0];
  // A new optional input (with a default) before the params: every param port of the call moves up by one.
  REQUIRE(edit::addNode(g, "in side : audio = 0", {}).ok());
  CHECK_FALSE(f.validate().ok());  // the call's copy of the signature is stale
  auto changed = edit::refreshCallers(f, 0);
  REQUIRE(changed.size() == 1);
  CHECK(f.validate().ok());
  // 0.25 still drives `amount`, `a` still drives `x`.
  const Patch& p = f.patches[1];
  for (const Wire& w : p.wires())
    if (p.node(w.from.node).kind == NodeKind::Const) CHECK(p.inputName(w.to.node, w.to.port) == "amount");
    else if (p.node(w.to.node).kind == NodeKind::Subcircuit) CHECK(p.inputName(w.to.node, w.to.port) == "x");
  const std::string text = print(f);
  CHECK(strict(text) == f);
}

TEST_CASE("edit: a constant becomes a param pin named after the input it drives") {
  PatchFile f = strict("patch P {\n  in x : audio\n  out y : audio\n  y = x * 0.5\n}\n");
  Patch& p = f.patches[0];
  NodeIndex c = kNone;
  for (NodeIndex v = 0; v < p.nodeCount(); ++v)
    if (p.node(v).kind == NodeKind::Const) c = v;
  REQUIRE(c != kNone);
  auto k = edit::constToParam(p, c);
  REQUIRE(k.ok());
  CHECK(p.node(*k).kind == NodeKind::Param);
  CHECK(p.nodeName(*k) == "b");
  CHECK(p.paramDecl(*k).defaultValue.value() == doctest::Approx(0.5));
  const std::string text = print(f);
  CHECK(text.find("param b = 0.5") != std::string::npos);
  CHECK(text.find("y = x * b") != std::string::npos);
  CHECK_FALSE(edit::constToParam(p, named(p, "x")).ok());
}

TEST_CASE("edit: make subcircuit moves the selection into a new patch with ins, param pins and outs") {
  PatchFile f = strict(
      "patch P {\n  param drive = 2 [1..10]\n  in x : audio\n  out y : audio\n"
      "  pre = x * drive\n  sat = tanh(pre)\n  y = sat * 0.5 + pre * 0.1\n}\n");
  Patch& p = f.patches[0];
  const NodeIndex sel[2] = {named(p, "pre"), named(p, "sat")};
  auto r = edit::makeSubcircuit(f, 0, sel);
  REQUIRE(r.ok());
  REQUIRE(f.patches.size() == 2);
  const Patch& sub = f.patches[r->patch];
  CHECK(sub.name() == "Sub1");
  // x → in, drive → param pin with its declaration; pre and sat leave → two outs.
  uint32_t ins = 0, params = 0, outs = 0;
  for (NodeIndex v = 0; v < sub.nodeCount(); ++v) {
    ins += sub.node(v).kind == NodeKind::Input;
    params += sub.node(v).kind == NodeKind::Param;
    outs += sub.node(v).kind == NodeKind::Output;
  }
  CHECK(ins == 1);
  CHECK(params == 1);
  CHECK(outs == 2);
  CHECK(sub.paramDecl(named(sub, "drive")).hasRange);
  CHECK(f.validate().ok());
  const std::string text = print(f);
  PatchFile back = strict(text);
  for (Patch& q : back.patches) edit::anonymize(q);  // the two-output call is printed under a generated name
  CHECK(back == f);
  // The result infers (the library holds both patches).
  PatchLibrary lib;
  lib.add(back);
  InferOptions io;
  io.library = &lib;
  CHECK(inferDetailed(back.patches[0], io).ok());
  // Refusals.
  const NodeIndex decl[1] = {named(f.patches[0], "x")};
  CHECK_FALSE(edit::makeSubcircuit(f, 0, decl).ok());
  CHECK_FALSE(edit::makeSubcircuit(f, 0, {}).ok());
}

TEST_CASE("edit: an IR edit printed and re-parsed with the previous parse keeps every NodeId") {
  const std::string text =
      "patch P {\n  param k = 0.5\n  in x : audio\n  out y : audio\n  s = z1(x + s * k)\n  y = tanh(s)\n}\n";
  const ParseResult first = lenient(text);
  REQUIRE(first.ok());
  PatchFile edited = first.file;
  Patch& p = edited.patches[0];
  const NodeIndex t = atomNode(p, "tanh");
  const NodeIndex m = *edit::addNode(p, "*", {});
  REQUIRE(edit::connect(p, {t, 0}, {m, 0}).ok());
  REQUIRE(edit::connect(p, {named(p, "k"), 0}, {m, 1}).ok());
  REQUIRE(edit::connect(p, {m, 0}, {named(p, "y"), 0}).ok());
  const std::string next = print(edited);
  const ParseResult second = lenient(next, &first.file);
  REQUIRE(second.ok());
  // Every node that existed keeps its id; the new multiplier is the only fresh one.
  CHECK(second.idMatch.matched == first.file.patches[0].nodeCount());
  CHECK(second.idMatch.fresh() == 1);
}
