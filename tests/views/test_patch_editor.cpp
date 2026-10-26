// T4.4: the circuit editor's document (PatchEditor): scripted edits in either pane update the other (the text is the
// one source of truth, re-parsed with the previous parse), one undo history across both, NodeIds and state kept across
// edits (the T1.x-text-ids criterion: ≥ 99 % of the state entries match), parse and check errors located and never
// handed to the player (the last good program keeps playing), layout-only edits keep hand formatting, save.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

#include "pq/graph/atoms.h"
#include "pq/graph/optimise.h"
#include "pq/graph/schedule.h"
#include "pq/views/circuit/patch_editor.h"

using namespace pq;
using namespace pq::views::circuit;
namespace fs = std::filesystem;
using graph::NodeIndex;

namespace {

fs::path stdlibDir() { return fs::path(PQ_TEST_DATA_DIR) / ".." / "stdlib"; }

// Records what the editor hands over; keeps the last schedule's state layout (ScheduleOptions::previous).
class FakePlayer final : public PatchPlayer {
public:
  double sampleRate() const override { return 48000.0; }
  Status play(const graph::Schedule& s, const graph::TypedPatch&) override {
    ++plays;
    layout = s.state;
    hasLayout = true;
    playing_ = true;
    return Ok{};
  }
  void stop() override { playing_ = false; }
  bool playing() const override { return playing_; }
  const graph::StateLayout* stateLayout() const override { return hasLayout ? &layout : nullptr; }
  uint32_t plays = 0;
  graph::StateLayout layout;
  bool hasLayout = false;

private:
  bool playing_ = false;
};

std::unique_ptr<PatchEditor> fromText(std::string text, std::string name = "editor_test.pqp") {
  auto e = PatchEditor::fromText((stdlibDir() / name).string(), std::move(text));
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
  return std::move(*e);
}

NodeIndex named(const graph::Patch& p, std::string_view n) {
  const auto v = p.findByName(n);
  REQUIRE_MESSAGE(v.has_value(), "no node " << std::string(n));
  return *v;
}

NodeIndex atomNode(const graph::Patch& p, std::string_view atom, uint32_t nth = 0) {
  for (NodeIndex v = 0; v < p.nodeCount(); ++v)
    if (p.node(v).kind == graph::NodeKind::Atom && graph::atomSignature(p.node(v).atom).name == atom && nth-- == 0) return v;
  return graph::kNone;
}

NodeIndex driverOf(const graph::Patch& p, NodeIndex n, uint32_t input) {
  for (const graph::Wire& w : p.wires())
    if (w.to.node == n && w.to.port == input) return w.from.node;
  return graph::kNone;
}

NodeIndex byId(const graph::Patch& p, graph::NodeId id) {
  const auto v = p.indexOf(id);
  REQUIRE(v.has_value());
  return *v;
}

std::string diagText(const PatchEditor& e) {
  std::string s;
  for (const EditorDiagnostic& d : e.diagnostics()) s += std::to_string(d.line) + ":" + std::to_string(d.col) + " " + d.message + "\n";
  return s;
}

// Old state entries that a transfer plan would carry into `to` (same key, channel, arena, tag and size).
std::pair<size_t, size_t> stateMatched(const graph::StateLayout& from, const graph::StateLayout& to) {
  std::set<std::tuple<uint64_t, uint16_t, uint8_t, uint32_t>> next;
  for (const graph::StateEntry& e : to.entries)
    next.insert({e.key, e.channel, static_cast<uint8_t>(e.arena), e.size});
  size_t matched = 0;
  for (const graph::StateEntry& e : from.entries) matched += next.count({e.key, e.channel, static_cast<uint8_t>(e.arena), e.size});
  return {matched, from.entries.size()};
}

const char* kVoice =
    "patch Voice {\n"
    "  param drive = 2 [1..10]\n"
    "  in x : audio\n"
    "  out y : audio\n"
    "\n"
    "  // a hand-written comment\n"
    "  s = z1(x * 0.5 + s * 0.25)\n"
    "  y = tanh(s * drive)\n"
    "}\n";

}  // namespace

TEST_CASE("patch editor: a text pane edit updates the circuit; unchanged nodes keep their NodeIds") {
  auto e = fromText(kVoice);
  REQUIRE(e->textLowered());
  const graph::Patch before = e->drawn(0);
  const graph::NodeId zid = before.node(atomNode(before, "z1")).id;
  int changes = 0;
  e->onChange = [&](const EditorChange& c) { changes += c.graph ? 1 : 0; };
  // Insert an export line (text pane).
  const size_t at = e->text().find("  y = tanh");
  e->replaceText(static_cast<uint32_t>(at), 0, "  export pre = s * drive\n", false);
  CHECK(changes == 1);
  REQUIRE(e->textLowered());
  const graph::Patch& after = e->drawn(0);
  CHECK(after.findByName("pre").has_value());
  CHECK(after.node(atomNode(after, "z1")).id == zid);
  CHECK(e->stats().lastIdMatch.matched + 3 >= e->stats().lastIdMatch.nodes);  // export + its mul (+ a const)
  // Positions carried: the z1 is drawn where it was.
  CHECK(after.layout(atomNode(after, "z1")).x == before.layout(atomNode(before, "z1")).x);
  CHECK(after.layout(atomNode(after, "z1")).y == before.layout(atomNode(before, "z1")).y);
}

TEST_CASE("patch editor: circuit edits rewrite the text; each is one undo step; undo/redo across both panes") {
  auto e = fromText(kVoice);
  const std::string original = e->text();
  // Text edit first.
  e->replaceText(static_cast<uint32_t>(e->text().find("0.25")), 4, "0.3", false);
  const std::string afterText = e->text();
  // Circuit: add a lowpass-ish chain: tanh → (×) → y, the multiplier's other input from a new constant.
  const graph::Patch& p0 = e->drawn(0);
  const NodeIndex t = atomNode(p0, "tanh"), y = named(p0, "y");
  auto mul = e->addNode(0, "*", {300, 200});
  REQUIRE(mul.ok());
  CHECK(e->text().find("_n") != std::string::npos);  // not wired yet: a generated name keeps it in the text
  {
    const graph::Patch& p = e->drawn(0);
    REQUIRE(e->connect(0, {atomNode(p, "tanh"), 0}, {byId(p, *mul), 0}).ok());
  }
  {
    const graph::Patch& p = e->drawn(0);
    REQUIRE(e->connect(0, {byId(p, *mul), 0}, {named(p, "y"), 0}).ok());
  }
  auto half = e->addNode(0, "0.5", {240, 260});
  REQUIRE(half.ok());
  {
    const graph::Patch& p = e->drawn(0);
    REQUIRE(e->connect(0, {byId(p, *half), 0}, {byId(p, *mul), 1}).ok());
  }
  // Wired once each: inlined again, no generated names left.
  CHECK(e->text().find("y = tanh(s * drive) * 0.5") != std::string::npos);
  CHECK(e->text().find("_n") == std::string::npos);
  CHECK(e->diagnostics().empty());
  // Rename and make-param from the circuit.
  {
    const graph::Patch& p = e->drawn(0);
    REQUIRE(e->rename(0, named(p, "s"), "state").ok());
  }
  CHECK(e->text().find("state = z1(") != std::string::npos);
  {
    const graph::Patch& p = e->drawn(0);
    auto k = e->makeParam(0, byId(p, *half));
    REQUIRE(k.ok());
    CHECK(e->drawn(0).node(byId(e->drawn(0), *k)).kind == graph::NodeKind::Param);
  }
  CHECK(e->text().find("param b = 0.5") != std::string::npos);
  // Delete the multiplier: y loses its driver (a validation error, located), the text still lowers.
  {
    const graph::Patch& p = e->drawn(0);
    const NodeIndex m = byId(p, *mul);
    REQUIRE(e->erase(0, std::span<const NodeIndex>(&m, 1), {}).ok());
  }
  CHECK(e->textLowered());
  REQUIRE_FALSE(e->diagnostics().empty());
  CHECK(e->diagnostics()[0].phase == EditorDiagnostic::Phase::Validation);
  // The hand-written comment did not survive the reprint of the patch (documented), but undo brings everything back.
  const size_t steps = e->historySize();
  CHECK(steps == 9);  // text edit, add, connect, connect, add, connect, rename, make param, delete
  for (size_t k = 0; k < steps - 1; ++k) REQUIRE(e->undo());
  CHECK(e->text() == afterText);
  REQUIRE(e->undo());
  CHECK(e->text() == original);
  CHECK_FALSE(e->undo());
  for (size_t k = 0; k < steps; ++k) REQUIRE(e->redo());
  CHECK_FALSE(e->redo());
  CHECK(e->text().find("param b = 0.5") != std::string::npos);
  (void)t;
  (void)y;
}

TEST_CASE("patch editor: a wire that closes a delay-free loop gets a z1 and a note") {
  auto e = fromText("patch L {\n  in x : audio\n  out y : audio\n  a = x + 0\n  y = tanh(a)\n}\n");
  const graph::Patch& p = e->drawn(0);
  REQUIRE(e->connect(0, {atomNode(p, "tanh"), 0}, {named(p, "a"), 1}).ok());
  CHECK(e->note().find("z") != std::string::npos);
  CHECK(e->text().find("z1(") != std::string::npos);
  CHECK(e->diagnostics().empty());
  CHECK(e->check(0).ok());
}

TEST_CASE("patch editor: typing coalesces into one undo step; a new line starts another") {
  auto e = fromText(kVoice);
  const std::string original = e->text();
  uint32_t at = static_cast<uint32_t>(e->text().find("  y = tanh"));
  for (const char c : std::string("  w = 1")) {
    e->replaceText(at++, 0, std::string(1, c));
  }
  e->replaceText(at - 1, 1, "");  // backspace
  e->replaceText(at - 1, 0, "2");
  e->replaceText(at, 0, "\n");
  CHECK(e->text().find("  w = 2\n") != std::string::npos);
  CHECK(e->historySize() == 2);
  REQUIRE(e->undo());
  REQUIRE(e->undo());
  CHECK(e->text() == original);
}

TEST_CASE("patch editor: a syntax error keeps the last good circuit and program; it is located; fixing it swaps") {
  auto e = fromText(kVoice);
  FakePlayer player;
  e->setPlayer(&player);
  REQUIRE(e->play(0).ok());
  CHECK(player.plays == 1);
  CHECK(e->playingPatch() == "Voice");
  const graph::NodeId zid = e->drawn(0).node(atomNode(e->drawn(0), "z1")).id;
  // Break it: `tanh(s * drive` without its ')'.
  const uint32_t close = static_cast<uint32_t>(e->text().find("drive)") + 5);
  e->replaceText(close, 1, "");
  CHECK_FALSE(e->textLowered());
  REQUIRE_FALSE(e->diagnostics().empty());
  CHECK(e->diagnostics()[0].phase == EditorDiagnostic::Phase::Syntax);
  CHECK(e->diagnostics()[0].line >= 7);
  CHECK_FALSE(e->check().ok());
  CHECK(player.plays == 1);                       // nothing handed over: the last good program plays on
  CHECK(e->drawn(0).findByName("s").has_value());  // the circuit shows the last good parse
  CHECK_FALSE(e->connect(0, {0, 0}, {1, 0}).ok()); // circuit edits wait for the text
  e->replaceText(close, 0, ")");
  REQUIRE(e->textLowered());
  REQUIRE(e->check().ok());
  CHECK(player.plays == 2);
  CHECK(e->drawn(0).node(atomNode(e->drawn(0), "z1")).id == zid);
  // A check error (the text lowers and validates; scheduling finds a loop with no delay).
  e->replaceText(static_cast<uint32_t>(e->text().find("z1(")), 2, "tanh", false);
  CHECK(e->textLowered());
  CHECK_FALSE(e->check().ok());
  CHECK(player.plays == 2);
  bool located = false;
  for (const EditorDiagnostic& d : e->diagnostics())
    if (d.phase == EditorDiagnostic::Phase::Check && d.line == 7) located = true;
  CHECK_MESSAGE(located, diagText(*e));
  REQUIRE(e->undo());
  REQUIRE(e->check().ok());
  CHECK(player.plays == 3);
  e->stop();
  CHECK_FALSE(player.playing());
  CHECK_FALSE(e->playing());
}

TEST_CASE("patch editor: moving nodes and tidy change only the layout block (comments stay); undo restores") {
  auto e = fromText(kVoice);
  const std::string original = e->text();
  const graph::Patch& p = e->drawn(0);
  const NodeIndex z = atomNode(p, "z1");
  const float x0 = p.layout(z).x;
  REQUIRE(e->moveNodes(0, std::span<const NodeIndex>(&z, 1), 40, 8).ok());
  CHECK(e->text().find("// a hand-written comment") != std::string::npos);
  CHECK(e->text().find("layout {") != std::string::npos);
  const size_t brace = original.rfind('}');
  CHECK(e->text().compare(0, brace, original, 0, brace) == 0);  // everything before the patch's '}' unchanged
  CHECK(e->drawn(0).layout(atomNode(e->drawn(0), "z1")).x == x0 + 40);
  REQUIRE(e->tidy(0).ok());
  CHECK(e->text().find("// a hand-written comment") != std::string::npos);
  REQUIRE(e->undo());
  REQUIRE(e->undo());
  CHECK(e->text() == original);
}

TEST_CASE("patch editor: make subcircuit adds a patch after the edited one; the file checks") {
  auto e = fromText(kVoice);
  const graph::Patch& p = e->drawn(0);
  const NodeIndex t = atomNode(p, "tanh");
  const NodeIndex sel[2] = {t, driverOf(p, t, 0)};  // tanh(s * drive)
  auto r = e->makeSubcircuit(0, sel);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
  CHECK(*r == 1);
  REQUIRE(e->parsed().file.patches.size() == 2);
  CHECK(e->parsed().file.patches[1].name() == "Sub1");
  CHECK_MESSAGE(e->diagnostics().empty(), diagText(*e));
  CHECK(e->check(0).ok());
  CHECK(e->check(1).ok());
  CHECK(e->findCallee("Sub1") != nullptr);
  REQUIRE(e->undo());
  CHECK(e->parsed().file.patches.size() == 1);
}

TEST_CASE("patch editor: save writes the text with the drawn layout; reopening shows the same circuit") {
  const fs::path dir = fs::temp_directory_path() / "pq_patch_editor_save";
  fs::create_directories(dir);
  const fs::path file = dir / "voice.pqp";
  {
    std::ofstream out(file, std::ios::binary);
    out << kVoice;
  }
  auto opened = PatchEditor::open(file.string());
  REQUIRE(opened.ok());
  auto& e = *opened;
  CHECK_FALSE(e->dirty());
  const graph::Patch drawnBefore = e->drawn(0);
  REQUIRE(e->save().ok());
  CHECK_FALSE(e->dirty());
  std::stringstream ss;
  {
    std::ifstream in(file, std::ios::binary);
    ss << in.rdbuf();
  }
  CHECK(ss.str() == e->text());
  CHECK(ss.str().find("layout {") != std::string::npos);
  CHECK(ss.str().find("// a hand-written comment") != std::string::npos);
  auto again = PatchEditor::open(file.string());
  REQUIRE(again.ok());
  const graph::Patch& d = (*again)->drawn(0);
  for (NodeIndex v = 0; v < d.nodeCount(); ++v) {
    CHECK(d.layout(v).x == drawnBefore.layout(v).x);
    CHECK(d.layout(v).y == drawnBefore.layout(v).y);
  }
  fs::remove_all(dir);
}

TEST_CASE("patch editor: state is kept across editor edits over the stdlib (>= 99 % of state entries)") {
  size_t matched = 0, total = 0, patches = 0;
  for (const auto& entry : fs::recursive_directory_iterator(stdlibDir())) {
    if (!entry.is_regular_file() || entry.path().extension() != ".pqp") continue;
    std::ifstream in(entry.path(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    auto made = PatchEditor::fromText(entry.path().string(), ss.str());
    REQUIRE(made.ok());
    auto& e = *made;
    for (size_t k = 0; k < e->parsed().file.patches.size(); ++k) {
      CAPTURE(entry.path().string());
      CAPTURE(e->parsed().file.patches[k].name());
      FakePlayer player;
      e->setPlayer(&player);
      if (!e->play(k).ok()) continue;  // a patch that needs a context it cannot have alone
      const graph::StateLayout first = player.layout;
      if (first.entries.empty()) {
        e->stop();
        continue;
      }
      ++patches;
      // A sequence of edits from both panes: an export added in the circuit and wired, a comment line typed, the
      // whole layout tidied, a node moved, a binding renamed in the text.
      const graph::Patch& p = e->drawn(k);
      NodeIndex src = graph::kNone;
      for (NodeIndex v = 0; v < p.nodeCount() && src == graph::kNone; ++v)
        if ((p.node(v).kind == graph::NodeKind::Atom || p.node(v).kind == graph::NodeKind::Subcircuit) &&
            p.node(v).numOutputs > 0)
          src = v;
      REQUIRE(src != graph::kNone);
      const graph::NodeId srcId = p.node(src).id;  // indices follow the text order, which an edit may change
      auto ex = e->addNode(k, "export probe_", {0, -80});
      REQUIRE_MESSAGE(ex.ok(), (ex.ok() ? "" : ex.error().message));
      {
        const graph::Patch& q = e->drawn(k);
        REQUIRE(e->connect(k, {byId(q, srcId), 0}, {byId(q, *ex), 0}).ok());
      }
      e->replaceText(e->parsed().patchSpans[k].begin, 0, "// edited\n", false);
      REQUIRE(e->tidy(k).ok());
      const NodeIndex one = 0;
      REQUIRE(e->moveNodes(k, std::span<const NodeIndex>(&one, 1), 16, 0).ok());
      REQUIRE_MESSAGE(e->check(k).ok(), diagText(*e));
      const auto [m, t] = stateMatched(first, player.layout);
      matched += m;
      total += t;
      CHECK(m == t);
      e->stop();
    }
  }
  MESSAGE(patches << " stdlib patches, " << matched << " / " << total << " state entries kept");
  CHECK(patches >= 10);
  REQUIRE(total > 0);
  CHECK(static_cast<double>(matched) >= 0.99 * static_cast<double>(total));
}

TEST_CASE("patch editor: an edit of a comment or whitespace keeps the graph (no canvas rebuild)") {
  auto e = fromText(kVoice);
  int graphChanges = 0, textChanges = 0;
  e->onChange = [&](const EditorChange& c) {
    graphChanges += c.graph ? 1 : 0;
    textChanges += c.text ? 1 : 0;
  };
  const graph::Patch& before = e->drawn(0);
  const float zx = before.layout(atomNode(before, "z1")).x;
  e->replaceText(static_cast<uint32_t>(e->text().find("hand-written")), 0, "carefully ");
  e->replaceText(static_cast<uint32_t>(e->text().find("  y = tanh")), 0, "\n");
  CHECK(textChanges == 2);
  CHECK(graphChanges == 0);
  CHECK(e->drawn(0).layout(atomNode(e->drawn(0), "z1")).x == zx);
  // The parse moved with the text: node spans point at the new offsets.
  e->replaceText(static_cast<uint32_t>(e->text().find("0.25")), 4, "0.3");
  CHECK(graphChanges == 1);
}
