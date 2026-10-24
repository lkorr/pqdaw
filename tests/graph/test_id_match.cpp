// T1.x-text-ids (QUESTIONS Q68): a re-parsed text inherits the NodeIds of the previous parse (id_match.h), so
// hot-swap state keys survive text edits. Hand-written edits (export, rename, inserted lines, reordering,
// duplicated subexpressions, name swaps, changed constants/atoms, subcircuits), the whole corpus under every edit
// kind, and Patch::setNodeIds. The transfer plans and the click-free swap are in tests/engine/test_text_edits.cpp.

#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>

#include "pq/graph/id_match.h"
#include "pq/graph/parse.h"
#include "rename_edits.h"
#include "text_edits.h"
#include "text_util.h"

using namespace pq::graph;
namespace fs = std::filesystem;

namespace {

ParseResult parseText(const std::string& text, const PatchFile* previous = nullptr, const std::string& file = "edit.pqp") {
  text_util::DirLoader loader(text_util::casesDir());
  ParseOptions po;
  po.imports = &loader;
  po.previous = previous;
  ParseResult r = parseDetailed(text, (text_util::casesDir() / file).string(), po);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : formatDiagnostic(text, r.diagnostics.front())));
  return r;
}

NodeIndex named(const Patch& p, std::string_view name) {
  const auto n = p.findByName(name);
  REQUIRE_MESSAGE(n.has_value(), "no node named " << name);
  return *n;
}
NodeId idOf(const Patch& p, std::string_view name) { return p.node(named(p, name)).id; }

// The driver of input i of node v.
NodeIndex driverOf(const Patch& p, NodeIndex v, uint32_t i) {
  const Adjacency adj = p.adjacency();
  const WireIndex w = adj.driver[p.flatInput(v, i)];
  REQUIRE(w != kNone);
  return p.wire(w).from.node;
}

// Wires of `next` between two nodes that kept an id of `previous`, and how many of them are wires of `previous`
// (same ids, same ports): a pairing that is structurally right maps old wires to old wires.
// A subcircuit's ports are compared by name (a callee edit can insert a port before others); other nodes' by index.
std::pair<uint32_t, uint32_t> wireConsistency(const Patch& previous, const Patch& next) {
  using Key = std::tuple<uint32_t, std::string, uint32_t, std::string>;
  auto key = [](const Patch& p, const Wire& w) {
    const bool fs = p.node(w.from.node).kind == NodeKind::Subcircuit, ts = p.node(w.to.node).kind == NodeKind::Subcircuit;
    return Key{p.node(w.from.node).id.value, fs ? std::string(p.outputName(w.from.node, w.from.port)) : std::to_string(w.from.port),
               p.node(w.to.node).id.value, ts ? std::string(p.inputName(w.to.node, w.to.port)) : std::to_string(w.to.port)};
  };
  std::set<Key> old;
  for (const Wire& w : previous.wires()) old.insert(key(previous, w));
  const uint32_t limit = previous.nextId().value;
  uint32_t both = 0, same = 0;
  for (const Wire& w : next.wires()) {
    if (next.node(w.from.node).id.value >= limit || next.node(w.to.node).id.value >= limit) continue;
    ++both;
    same += old.count(key(next, w)) != 0;
  }
  return {both, same};
}

const char* kEcho = R"(patch Echo {
  param level = 0.3 [0..1]
  out y : audio
  osc = sine(phasor(220.5))
  e = delay(osc * 0.4 + e * 0.5, 2.7ms, 10ms)
  y = osc * level + e
}
)";

// Every node of `a` (the previous parse) kept its id in `b`, each paired node has the same name as before (or
// `renamed` maps it), and every old wire between kept nodes is still there.
void requireAllKept(const Patch& a, const Patch& b, const IdMatchStats& st, std::string_view from = {},
                    std::string_view to = {}) {
  CHECK(st.matched == a.nodeCount());
  for (NodeIndex v = 0; v < a.nodeCount(); ++v) {
    const auto w = b.indexOf(a.node(v).id);
    REQUIRE_MESSAGE(w.has_value(), "lost " << a.describe(v));
    const std::string& na = a.nodeName(v);
    CHECK_MESSAGE((b.nodeName(*w) == na || (na == from && b.nodeName(*w) == to)), a.describe(v) << " → " << b.describe(*w));
    CHECK(b.node(*w).kind == a.node(v).kind);
  }
  const auto [both, same] = wireConsistency(a, b);
  CHECK(both == same);
  CHECK(both == a.wireCount());
}

}  // namespace

TEST_CASE("id match: Patch::setNodeIds keeps dense order; indexOf, add and erase work with unsorted ids") {
  Patch p("P");
  const NodeIndex a = p.addConst(Constant::real(1));
  const NodeIndex b = p.addConst(Constant::real(2));
  const NodeIndex c = p.addAtom(*AtomRegistry::builtin().find("add"));
  p.connect(a, c, 0);
  p.connect(b, c, 1);
  CHECK(p.idsSorted());
  const NodeId ids[] = {NodeId{40}, NodeId{7}, NodeId{12}};
  REQUIRE(p.setNodeIds(ids));
  CHECK_FALSE(p.idsSorted());
  CHECK(p.node(a).id == NodeId{40});
  CHECK(p.constant(a).literal == 1.0);  // nothing moved
  CHECK(*p.indexOf(NodeId{7}) == b);
  CHECK(*p.indexOf(NodeId{12}) == c);
  CHECK(*p.indexOf(NodeId{40}) == a);
  CHECK_FALSE(p.indexOf(NodeId{8}).has_value());
  CHECK_FALSE(p.indexOf(NodeId{1}).has_value());
  const NodeIndex d = p.addConst(Constant::real(3));
  CHECK(p.node(d).id == NodeId{41});  // above every id
  CHECK(*p.indexOf(NodeId{41}) == d);
  const NodeIndex eraseB[] = {b};
  p.eraseNodes(eraseB);
  CHECK_FALSE(p.indexOf(NodeId{7}).has_value());
  CHECK(*p.indexOf(NodeId{40}) == 0);
  CHECK(*p.indexOf(NodeId{12}) == 1);
  CHECK(*p.indexOf(NodeId{41}) == 2);
  // Invalid id lists change nothing.
  const NodeId dup[] = {NodeId{5}, NodeId{5}, NodeId{6}};
  CHECK_FALSE(p.setNodeIds(dup));
  const NodeId zero[] = {NodeId{5}, NodeId{}, NodeId{6}};
  CHECK_FALSE(p.setNodeIds(zero));
  const NodeId shortList[] = {NodeId{5}};
  CHECK_FALSE(p.setNodeIds(shortList));
  CHECK(p.node(0).id == NodeId{40});
  // Sparse ids (the sorting path): duplicates are still refused, order still found.
  const NodeId sparseDup[] = {NodeId{1000000}, NodeId{3}, NodeId{1000000}};
  CHECK_FALSE(p.setNodeIds(sparseDup));
  const NodeId sparse[] = {NodeId{1000000}, NodeId{3}, NodeId{70000}};
  REQUIRE(p.setNodeIds(sparse));
  CHECK(*p.indexOf(NodeId{3}) == 1);
  CHECK(*p.indexOf(NodeId{70000}) == 2);
  CHECK(*p.indexOf(NodeId{1000000}) == 0);
  CHECK(p.nextId() == NodeId{1000001});
  // Sorted again → the id table is dropped (the allocator never goes back: new ids stay above 1000000).
  const NodeId sorted[] = {NodeId{50}, NodeId{51}, NodeId{52}};
  REQUIRE(p.setNodeIds(sorted));
  CHECK(p.idsSorted());
  CHECK(*p.indexOf(NodeId{52}) == 2);
}

TEST_CASE("id match: the Q68 edits keep every NodeId (export, rename, inserted lines)") {
  const ParseResult a = parseText(kEcho);
  const Patch& pa = a.file.patches[0];

  SUBCASE("an export line before everything (shifts every parse id: the Q68 case)") {
    const std::string t = text_edits::insertAtTop(kEcho, "  export spare = sine(phasor(331))");
    // Without the previous parse, the ids move (what Q68 found).
    const ParseResult plain = parseText(t);
    CHECK(plain.file.patches[0].node(named(plain.file.patches[0], "e")).id != idOf(pa, "e"));
    const ParseResult b = parseText(t, &a.file);
    const Patch& pb = b.file.patches[0];
    requireAllKept(pa, pb, b.idMatch);
    CHECK(b.idMatch.fresh() == 4);  // export, sine, phasor, literal
    CHECK(pb.node(named(pb, "spare")).id.value >= pa.nextId().value);
    CHECK(b.file == plain.file);  // the same patch, only ids differ
  }
  SUBCASE("renaming a binding") {
    std::string t = text_edits::renameIdent(kEcho, "osc", "carrier", 0, 100);
    const ParseResult b = parseText(t, &a.file);
    const Patch& pb = b.file.patches[0];
    requireAllKept(pa, pb, b.idMatch, "osc", "carrier");
    CHECK(idOf(pb, "carrier") == idOf(pa, "osc"));
  }
  SUBCASE("renaming the delay line and the output") {
    std::string t = text_edits::renameIdent(kEcho, "e", "echo", 0, 100);
    t = text_edits::renameIdent(t, "y", "wet", 0, 100);
    const ParseResult b = parseText(t, &a.file);
    const Patch& pb = b.file.patches[0];
    CHECK(b.idMatch.matched == pa.nodeCount());
    CHECK(idOf(pb, "echo") == idOf(pa, "e"));
    CHECK(idOf(pb, "wet") == idOf(pa, "y"));
  }
  SUBCASE("inserted lines: a param on top, a binding in the middle") {
    std::string t = text_edits::insertAtTop(kEcho, "  param extra = 0.5");
    t = text_edits::insertInMiddle(t, "  lfo = phasor(0.25)");
    const ParseResult b = parseText(t, &a.file);
    requireAllKept(pa, b.file.patches[0], b.idMatch);
    CHECK(b.idMatch.fresh() == 3);
  }
  SUBCASE("reordered lines and a canonical reprint") {
    for (uint32_t seed = 1; seed <= 6; ++seed) {
      const ParseResult b = parseText(text_edits::shuffleBindings(kEcho, seed), &a.file);
      REQUIRE(b.file == a.file);
      requireAllKept(pa, b.file.patches[0], b.idMatch);
    }
    const ParseResult c = parseText(print(a.file), &a.file);
    requireAllKept(pa, c.file.patches[0], c.idMatch);
  }
}

TEST_CASE("id match: ParseOptions::previous changes only ids (dense order, spans, printing, equality)") {
  const ParseResult a = parseText(kEcho);
  const std::string t = text_edits::insertAtTop(kEcho, "  export spare = sine(phasor(331))");
  const ParseResult plain = parseText(t);
  const ParseResult b = parseText(t, &a.file);
  CHECK(b.file == plain.file);
  CHECK(print(b.file) == print(plain.file));
  REQUIRE(b.nodeSpans.size() == plain.nodeSpans.size());
  for (size_t k = 0; k < b.nodeSpans[0].size(); ++k) {
    CHECK(b.nodeSpans[0][k].offset == plain.nodeSpans[0][k].offset);
    CHECK(b.file.patches[0].nodeName(static_cast<NodeIndex>(k)) == plain.file.patches[0].nodeName(static_cast<NodeIndex>(k)));
  }
  CHECK(plain.idMatch.nodes == 0);  // no previous: nothing matched or counted
  // A chain of edits: each parse takes the previous one's ids, fresh ids never repeat an old one.
  const std::string t2 = text_edits::insertAtTop(t, "  export spare2 = sine(phasor(441))");
  const ParseResult c = parseText(t2, &b.file);
  CHECK(c.idMatch.matched == b.file.patches[0].nodeCount());
  CHECK(idOf(c.file.patches[0], "e") == idOf(a.file.patches[0], "e"));
  CHECK(idOf(c.file.patches[0], "spare") == idOf(b.file.patches[0], "spare"));
  CHECK(idOf(c.file.patches[0], "spare2").value >= b.file.patches[0].nextId().value);
}

TEST_CASE("id match: duplicated subexpressions keep their own ids (context decides, not text order)") {
  SUBCASE("two identical chains into different outputs, lines swapped, a third copy added") {
    const char* text = R"(patch Dup {
  out a : audio
  out b : audio
  a = delay(sine(phasor(220)), 1ms, 5ms) * 0.5
  b = delay(sine(phasor(220)), 1ms, 5ms) * 0.25
}
)";
    const char* swapped = R"(patch Dup {
  out a : audio
  out b : audio
  b = delay(sine(phasor(220)), 1ms, 5ms) * 0.25
  a = delay(sine(phasor(220)), 1ms, 5ms) * 0.5
}
)";
    const ParseResult a = parseText(text);
    const ParseResult b = parseText(text_edits::insertAtTop(swapped, "  export c = delay(sine(phasor(220)), 1ms, 5ms)"), &a.file);
    const Patch& pa = a.file.patches[0];
    const Patch& pb = b.file.patches[0];
    requireAllKept(pa, pb, b.idMatch);
    for (const char* out : {"a", "b"}) {
      CAPTURE(out);
      NodeIndex va = driverOf(pa, named(pa, out), 0), vb = driverOf(pb, named(pb, out), 0);  // the mul
      for (int depth = 0; depth < 3; ++depth) {                                               // delay, sine, phasor
        va = driverOf(pa, va, 0);
        vb = driverOf(pb, vb, 0);
        CHECK(pb.node(vb).id == pa.node(va).id);
      }
    }
    CHECK(b.idMatch.fresh() == 7);  // export, delay, sine, phasor, 220, 1ms, 5ms
  }
  SUBCASE("identical operands told apart by their input port") {
    const char* text = "patch D {\n  out y : audio\n  y = phasor(220) - phasor(220) * 0.5\n}\n";
    const ParseResult a = parseText(text);
    const ParseResult b = parseText(text_edits::insertAtTop(text, "  export s = phasor(220)"), &a.file);
    requireAllKept(a.file.patches[0], b.file.patches[0], b.idMatch);
  }
  SUBCASE("a deleted export: everything that is left keeps its id") {
    const std::string longer = text_edits::insertAtTop(kEcho, "  export spare = sine(phasor(331))");
    const ParseResult a = parseText(longer);
    const ParseResult b = parseText(kEcho, &a.file);
    CHECK(b.idMatch.matched == b.file.patches[0].nodeCount());
    const auto [both, same] = wireConsistency(a.file.patches[0], b.file.patches[0]);
    CHECK(both == same);
    CHECK(both == b.file.patches[0].wireCount());
  }
}

TEST_CASE("id match: names win over position; edited constants and atoms keep their node") {
  SUBCASE("two bindings swap names: state follows the names") {
    const ParseResult a = parseText("patch S {\n  out y : audio\n  a = phasor(1)\n  b = phasor(2)\n  y = a - b\n}\n");
    const ParseResult b = parseText("patch S {\n  out y : audio\n  b = phasor(1)\n  a = phasor(2)\n  y = b - a\n}\n", &a.file);
    CHECK(idOf(b.file.patches[0], "a") == idOf(a.file.patches[0], "a"));
    CHECK(idOf(b.file.patches[0], "b") == idOf(a.file.patches[0], "b"));
  }
  SUBCASE("a commuted sum of two named oscillators") {
    const ParseResult a = parseText("patch S {\n  out y : audio\n  a = phasor(1)\n  b = phasor(2)\n  y = a + b\n}\n");
    const ParseResult b = parseText("patch S {\n  out y : audio\n  a = phasor(1)\n  b = phasor(2)\n  y = b + a\n}\n", &a.file);
    CHECK(idOf(b.file.patches[0], "a") == idOf(a.file.patches[0], "a"));
    CHECK(idOf(b.file.patches[0], "b") == idOf(a.file.patches[0], "b"));
  }
  SUBCASE("an edited constant: the oscillator keeps its id") {
    const ParseResult a = parseText(kEcho);
    std::string t = kEcho;
    t.replace(t.find("220.5"), 5, "330");
    const ParseResult b = parseText(t, &a.file);
    const Patch& pa = a.file.patches[0];
    const Patch& pb = b.file.patches[0];
    CHECK(pb.node(driverOf(pb, named(pb, "osc"), 0)).id == pa.node(driverOf(pa, named(pa, "osc"), 0)).id);
    CHECK(b.idMatch.matched == pa.nodeCount());
  }
  SUBCASE("changed atoms: a named one by name, an anonymous one by position") {
    const ParseResult a = parseText(kEcho);
    std::string t = kEcho;
    t.replace(t.find("delay("), 6, "delay_cubic(");
    t.replace(t.find("level + e"), 9, "level - e");
    const ParseResult b = parseText(t, &a.file);
    const Patch& pa = a.file.patches[0];
    const Patch& pb = b.file.patches[0];
    requireAllKept(pa, pb, b.idMatch);
    CHECK(b.idMatch.weak == 2);
    CHECK(idOf(pb, "e") == idOf(pa, "e"));
    CHECK(pb.node(driverOf(pb, named(pb, "y"), 0)).id == pa.node(driverOf(pa, named(pa, "y"), 0)).id);
  }
  SUBCASE("a binding that becomes another kind of node is not paired; its neighbours are") {
    const ParseResult a = parseText("patch K {\n  out y : audio\n  g = 0.5\n  y = phasor(220) * g\n}\n");
    const ParseResult b = parseText("patch K {\n  out y : audio\n  g = phasor(1)\n  y = phasor(220) * g\n}\n", &a.file);
    const Patch& pa = a.file.patches[0];
    const Patch& pb = b.file.patches[0];
    REQUIRE(pa.node(named(pa, "g")).kind == NodeKind::Const);
    CHECK(idOf(pb, "g") != idOf(pa, "g"));
    CHECK(pb.node(driverOf(pb, driverOf(pb, named(pb, "y"), 0), 0)).id ==
          pa.node(driverOf(pa, driverOf(pa, named(pa, "y"), 0), 0)).id);  // phasor(220)
  }
}

TEST_CASE("id match: subcircuits — a callee's new export and a renamed call keep the call node and the callee") {
  const char* text = R"(patch Osc {
  param hz = 220
  out o : audio
  o = sine(phasor(hz))
}
patch Main {
  out y : audio
  v = Osc(hz: 330)
  y = v * 0.5
}
)";
  const ParseResult a = parseText(text);
  std::string t = text_edits::insertAtTop(text, "  export ph = phasor(1)");  // both patches; Osc's signature grows
  t = text_edits::renameIdent(t, "v", "tone", 6, 100);
  const ParseResult b = parseText(t, &a.file);
  const Patch& osc0 = *a.file.find("Osc");
  const Patch& osc1 = *b.file.find("Osc");
  const Patch& main0 = *a.file.find("Main");
  const Patch& main1 = *b.file.find("Main");
  REQUIRE(main1.subcircuit(named(main1, "tone")).outputs.size() == 2);  // o, ph
  CHECK(idOf(main1, "tone") == idOf(main0, "v"));
  CHECK(osc1.node(driverOf(osc1, driverOf(osc1, named(osc1, "o"), 0), 0)).id ==
        osc0.node(driverOf(osc0, driverOf(osc0, named(osc0, "o"), 0), 0)).id);  // the phasor
  CHECK(b.idMatch.matched == osc0.nodeCount() + main0.nodeCount());
  CHECK(b.idMatch.weak == 1);  // the call node: its port list changed
}

namespace {

std::vector<fs::path> corpusFiles() {
  const fs::path tests = fs::path(PQ_TEST_DATA_DIR);
  std::vector<fs::path> files;
  for (const fs::path& dir : {text_util::casesDir(), text_util::stdlibDir(), tests / "golden" / "patches",
                              tests / "engine" / "patches", tests / "jit" / "patches",
                              tests / ".." / "docs" / "reviews" / "M1" / "probes"})
    for (const fs::path& p : text_util::pqpFiles(dir)) files.push_back(p);
  return files;
}

std::optional<ParseResult> tryParse(const std::string& text, const fs::path& path, const PatchFile* previous) {
  text_util::DirLoader loader(path.parent_path());
  ParseOptions po;
  po.imports = &loader;
  po.previous = previous;
  ParseResult r = parseDetailed(text, path.string(), po);
  if (!r.ok()) return std::nullopt;
  return r;
}

struct EditTally {
  uint64_t previousNodes = 0, kept = 0, wires = 0, consistentWires = 0, edits = 0;
  void add(const PatchFile& a, const PatchFile& b) {
    ++edits;
    for (const Patch& pa : a.patches) {
      const Patch* pb = b.find(pa.name());
      REQUIRE(pb != nullptr);
      std::set<uint32_t> ids;
      for (const Node& n : pb->nodes()) ids.insert(n.id.value);
      previousNodes += pa.nodeCount();
      for (const Node& n : pa.nodes()) kept += ids.count(n.id.value);
      const auto [both, same] = wireConsistency(pa, *pb);
      wires += both;
      consistentWires += same;
    }
  }
};

}  // namespace

TEST_CASE("id match: every corpus patch keeps every node through each text edit kind" * doctest::test_suite("slow")) {
  std::map<std::string, EditTally> tally;
  uint32_t files = 0, renames = 0;
  size_t allRenamed = 0;
  for (const fs::path& path : corpusFiles()) {
    const std::string text = text_util::readFile(path);
    const auto a = tryParse(text, path, nullptr);
    if (!a) continue;  // the error-case corpus
    ++files;
    CAPTURE(path.string());
    auto edit = [&](const std::string& kind, const std::string& edited) {
      CAPTURE(kind);
      const auto b = tryParse(edited, path, &a->file);
      REQUIRE_MESSAGE(b.has_value(), "edited text does not parse:\n" << edited);
      EditTally one;
      one.add(a->file, b->file);
      CHECK_MESSAGE(one.kept == one.previousNodes, kind << ": kept " << one.kept << " of " << one.previousNodes);
      CHECK_MESSAGE(one.consistentWires == one.wires, kind << ": " << one.wires - one.consistentWires << " wires changed");
      tally[kind].add(a->file, b->file);
      return *b;
    };
    edit("export at the top", text_edits::insertAtTop(text, "  export edit_spare = sine(phasor(331))"));
    // A param after the other declarations (one before them would take positional call arguments: another graph).
    edit("inserted lines", text_edits::insertInMiddle(text_edits::insertAtTop(text_edits::insertAtBottom(text, "  param edit_p = 0.5"),
                                                                              "  edit_top = phasor(0.37)"),
                                                      "  edit_mid = phasor(0.41)"));
    for (uint32_t seed = 1; seed <= 3; ++seed) {
      const std::string shuffled = text_edits::shuffleBindings(text, seed);
      const ParseResult b = edit("reordered lines", shuffled);
      CHECK(b.file == a->file);
    }
    edit("canonical reprint", print(a->file));
    // Renames: up to 6 named bindings per file, spread over it, one at a time; then up to 64 at once (names give
    // no help: only structure pairs them). Each edit is checked to be exactly that rename of the IR.
    const auto candidates = rename_edits::bindings(a->file);
    const size_t step = std::max<size_t>(1, candidates.size() / 6);
    for (size_t c = 0; c < candidates.size(); c += step) {
      const auto [k, v] = candidates[c];
      const std::string edited = rename_edits::renameOne(text, path, a->file, k, v);
      if (edited.empty()) continue;  // the simple text rename hit something else (a named argument)
      const ParseResult b = edit("renamed binding", edited);
      const std::string to = a->file.patches[k].nodeName(v) + "_renamed";
      CHECK(b.file.patches[k].node(*b.file.patches[k].findByName(to)).id == a->file.patches[k].node(v).id);
      ++renames;
    }
    size_t renamedAtOnce = 0;
    const std::string all = rename_edits::renameAll(text, path, a->file, 64, &renamedAtOnce);
    if (renamedAtOnce > 1) edit("all bindings renamed at once", all);
    allRenamed += renamedAtOnce;
  }
  CHECK(files >= 40);
  CHECK(renames >= 60);
  CHECK(allRenamed >= 300);
  for (const auto& [kind, t] : tally)
    MESSAGE(kind << ": " << t.edits << " edits, " << t.kept << " / " << t.previousNodes << " nodes kept their id, "
                 << t.consistentWires << " / " << t.wires << " wires between kept nodes unchanged");
}

TEST_CASE("id match: cost on the largest corpus patch and a 1010-node chain (it runs on every edit)") {
  const fs::path big = fs::path(PQ_TEST_DATA_DIR) / ".." / "docs" / "reviews" / "M1" / "probes" / "mux1200.pqp";
  const std::string text = text_util::readFile(big);
  const auto a = tryParse(text, big, nullptr);
  REQUIRE(a.has_value());
  const std::string edited = text_edits::insertAtTop(text, "  export edit_spare = sine(phasor(331))");
  auto b = tryParse(edited, big, nullptr);
  REQUIRE(b.has_value());
  double best = 1e30;
  IdMatchStats st;
  for (int r = 0; r < 5; ++r) {
    PatchFile f = b->file;
    const auto t0 = std::chrono::steady_clock::now();
    st = inheritNodeIds(f, a->file);
    best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
  }
  CHECK(st.matched == a->file.patches[0].nodeCount());
  MESSAGE("inheritNodeIds on mux1200.pqp (" << a->file.patches[0].nodeCount() << " nodes, " << a->file.patches[0].wireCount()
                                            << " wires): best of 5 " << best << " us (this build; release numbers in bench/RESULTS.md)");
}
