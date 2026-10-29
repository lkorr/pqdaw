// T4.4: the circuit editor window — goldens (the editor on the stock one-pole; a typing slip with its inline error and
// the veiled canvas), scripted gestures on the canvas updating the text pane and typing in the text pane updating the
// canvas, one undo history across both, the add / rename type-ins, delete, idle after an edit, the .pqp syntax colours,
// and the code editor widget's own editing keys.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>

#include "../ui/golden_check.h"
#include "pq/graph/atoms.h"
#include "pq/ui/code_editor.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/view_scenes.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::circuit;
namespace fs = std::filesystem;

namespace {

std::string stdlibPath(const char* rel) { return (fs::path(PQ_TEST_DATA_DIR) / ".." / "stdlib" / rel).string(); }

void checkGolden(const char* scene, const std::string& file) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, file, w, h, UiScale(1.0f), o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png");
  if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &d));
  MESSAGE(scene << ": " << pq::ui::test::describe(d));
}

const char* kPatch =
    "patch Drive {\n"
    "  param amount = 2 [1..10]\n"
    "  in x : audio\n"
    "  out y : audio\n"
    "  y = tanh(x * amount)\n"
    "}\n";

struct Rig {
  TestDriver d{1200, 700};
  CircuitEditor* editor = nullptr;
  explicit Rig(const std::string& text = kPatch) {
    auto& root = d.setRoot<Widget>("root");
    root.setLayout(LayoutKind::Stack);
    (void)root.add<OverlayLayer>("overlay");
    d.tree().layoutNow();
    auto doc = PatchEditor::fromText(stdlibPath("editor_ui_test.pqp"), text);
    REQUIRE(doc.ok());
    editor = openCircuitEditor(root, std::move(*doc));
    REQUIRE(editor != nullptr);
    d.tree().layoutNow();
  }
  EditCanvas& canvas() { return editor->canvas(); }
  const CircuitScene& scene() { return *canvas().scene(); }
  // Device position of pin `k` (input or output) of the node named / atom `what`.
  Vec2 pin(graph::NodeIndex node, bool input, uint32_t k) {
    const SceneNode& n = scene().nodes[node];
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n.pinCount; ++i) {
      const ScenePin& p = scene().pins[n.firstPin + i];
      if (p.input == input && seen++ == k) return canvas().toDevice({p.x, p.y});
    }
    FAIL("no such pin");
    return {};
  }
  Vec2 centre(graph::NodeIndex node) {
    const SceneNode& n = scene().nodes[node];
    return canvas().toDevice({n.x + n.w * 0.5f, n.y + n.h * 0.5f});
  }
  const graph::Patch& patch() { return editor->document().drawn(0); }
  graph::NodeIndex atom(std::string_view name) {
    const graph::Patch& p = patch();
    for (graph::NodeIndex v = 0; v < p.nodeCount(); ++v)
      if (p.node(v).kind == graph::NodeKind::Atom && graph::atomSignature(p.node(v).atom).name == name) return v;
    return graph::kNone;
  }
  graph::NodeIndex named(std::string_view n) { return *patch().findByName(n); }
};

}  // namespace

TEST_CASE("circuit editor: golden of the editor on the stock one-pole (canvas + text pane)") {
  checkGolden("circuit-editor", stdlibPath("filters/onepole.pqp"));
}

TEST_CASE("circuit editor: golden of a typing slip (inline error, canvas veiled over the last good version)") {
  checkGolden("circuit-editor-error", stdlibPath("filters/onepole.pqp"));
}

TEST_CASE("circuit editor: a wire dragged on the canvas rewrites the text pane; typing updates the canvas; one undo") {
  Rig r;
  CodeEditor& text = r.editor->textPane();
  const std::string original = text.text();
  CHECK(original == r.editor->document().text());
  // Add a z1 by command, then drag its output into tanh's input from the canvas: the multiply goes, z1 comes in.
  REQUIRE(r.editor->addAt({40, 120}, "z1"));
  r.canvas().fitAll();
  CHECK(text.text() == r.editor->document().text());
  r.d.tree().layoutNow();
  const graph::NodeIndex z = r.atom("z1"), t = r.atom("tanh");
  REQUIRE(z != graph::kNone);
  // x → z1.x (input pin 0 of z1), then z1 → tanh.x.
  r.d.drag(r.pin(r.named("x"), false, 0), r.pin(z, true, 0));
  r.d.tree().layoutNow();
  r.d.drag(r.pin(r.atom("z1"), false, 0), r.pin(r.atom("tanh"), true, 0));
  r.d.tree().layoutNow();
  CHECK(text.text() == r.editor->document().text());
  CHECK_MESSAGE(text.text().find("y = tanh(z1(x))") != std::string::npos, text.text());
  (void)t;
  // Type in the text pane: change tanh to fast_tanh. The canvas follows.
  const uint32_t at = static_cast<uint32_t>(text.text().find("tanh(z1"));
  r.d.tree().setFocus(&text);
  text.setCaret(at);
  REQUIRE(r.d.type("fast_"));
  CHECK(r.editor->document().text().find("y = fast_tanh(z1(x))") != std::string::npos);
  CHECK(r.atom("fast_tanh") != graph::kNone);
  CHECK(r.scene().nodes.size() == r.patch().nodeCount());
  // One history: Ctrl+Z from the canvas undoes the typing, then the two wires, then the add.
  r.d.tree().setFocus(&r.canvas());
  REQUIRE(r.d.press("Ctrl+Z") == KeyResult::Widget);
  CHECK(text.text().find("y = tanh(z1(x))") != std::string::npos);
  REQUIRE(r.d.press("Ctrl+Z") == KeyResult::Widget);
  REQUIRE(r.d.press("Ctrl+Z") == KeyResult::Widget);
  REQUIRE(r.d.press("Ctrl+Z") == KeyResult::Widget);
  CHECK(text.text() == original);
  CHECK(r.editor->document().text() == original);
  // Redo from the text pane.
  r.d.tree().setFocus(&text);
  REQUIRE(r.d.press("Ctrl+Shift+Z") == KeyResult::Widget);
  CHECK(r.atom("z1") != graph::kNone);
}

TEST_CASE("circuit editor: drag a node to move it (layout only), Delete removes the selection, rename by type-in") {
  Rig r;
  const graph::NodeIndex t = r.atom("tanh");
  const float x0 = r.patch().layout(t).x;
  const Vec2 c = r.centre(t);
  r.d.drag(c, {c.x + 40 * r.canvas().deviceScale(), c.y}, 6);
  CHECK(r.patch().layout(r.atom("tanh")).x == doctest::Approx(x0 + 40));
  CHECK(r.editor->document().text().find("layout {") != std::string::npos);
  CHECK(r.editor->document().text().find("y = tanh(x * amount)") != std::string::npos);
  // Rename the multiply through the type-in (double-click it).
  r.d.tree().layoutNow();
  graph::NodeIndex mul = graph::kNone;
  for (graph::NodeIndex v = 0; v < r.patch().nodeCount(); ++v)
    if (r.patch().node(v).kind == graph::NodeKind::Atom && graph::atomSignature(r.patch().node(v).atom).name == "mul") mul = v;
  REQUIRE(mul != graph::kNone);
  r.d.doubleClick(r.centre(mul));
  r.d.step();
  REQUIRE(r.d.type("pre"));
  r.d.press("Enter");
  r.d.step();
  CHECK(r.editor->document().text().find("pre = x * amount") != std::string::npos);
  // Select tanh and delete it: y loses its driver, an inline validation error appears in the text pane.
  r.d.tree().setFocus(&r.canvas());
  r.d.click(r.centre(r.atom("tanh")));
  REQUIRE(r.canvas().selection().nodes.size() == 1);
  r.d.press("Delete");
  CHECK(r.atom("tanh") == graph::kNone);
  CHECK_FALSE(r.editor->textPane().marks().empty());
  r.d.press("Ctrl+Z");
  CHECK(r.atom("tanh") != graph::kNone);
  CHECK(r.editor->textPane().marks().empty());
}

TEST_CASE("circuit editor: double-click empty space adds a node from a spec; a syntax slip veils the canvas") {
  Rig r;
  const uint32_t before = r.patch().nodeCount();
  // Empty space well below the patch.
  const SceneNode& any = r.scene().nodes[0];
  const Vec2 empty = r.canvas().toDevice({any.x, r.scene().y1 + 60});
  REQUIRE(r.canvas().rect().contains(empty));
  r.d.doubleClick(empty);
  r.d.step();
  REQUIRE(r.d.type("440Hz"));
  r.d.press("Enter");
  r.d.step();
  CHECK(r.patch().nodeCount() == before + 1);
  CHECK(r.editor->document().text().find("440Hz") != std::string::npos);
  // A typing slip: the text does not parse; the canvas keeps the last good version.
  CodeEditor& text = r.editor->textPane();
  r.d.tree().setFocus(&text);
  text.setCaret(static_cast<uint32_t>(text.text().find("tanh(")) + 5);
  REQUIRE(r.d.type("("));
  CHECK_FALSE(r.editor->document().textLowered());
  CHECK_FALSE(text.marks().empty());
  CHECK(r.patch().nodeCount() == before + 1);
  CHECK_FALSE(r.editor->addAt({0, 0}, "tanh"));  // circuit edits wait for the text
  r.d.press("Backspace");
  CHECK(r.editor->document().textLowered());
}

TEST_CASE("circuit editor: after an edit the check timer fires once, then the editor is idle") {
  Rig r;
  r.d.advance(0.5);
  REQUIRE(r.editor->addAt({40, 120}, "z1"));
  r.canvas().fitAll();
  CHECK(r.editor->document().checkPending());
  r.d.advance(0.2);  // the 50 ms debounce
  CHECK_FALSE(r.editor->document().checkPending());
  r.d.advance(0.2);
  const uint64_t frames = r.d.framesDrawn();
  r.d.advance(2.0);
  CHECK(r.d.framesDrawn() == frames);
  CHECK_FALSE(r.d.tree().needsFrame());
  CHECK(std::isinf(r.d.tree().nextWakeTime()));
  // A focused text pane does not blink.
  r.d.tree().setFocus(&r.editor->textPane());
  r.d.advance(0.1);
  const uint64_t f2 = r.d.framesDrawn();
  r.d.advance(2.0);
  CHECK(r.d.framesDrawn() == f2);
}

TEST_CASE("circuit editor: .pqp syntax colours") {
  std::vector<CodeSpan> s;
  CHECK(highlightPatchLine("  param cutoff = 1kHz [20Hz..20kHz] @label(\"Cutoff\") // knob", 0, s) == 0);
  auto styleAt = [&](size_t byte) {
    for (const CodeSpan& sp : s)
      if (byte >= sp.begin && byte < sp.end) return sp.style;
    return CodeStyle::Text;
  };
  const std::string_view line = "  param cutoff = 1kHz [20Hz..20kHz] @label(\"Cutoff\") // knob";
  CHECK(styleAt(line.find("param")) == CodeStyle::Keyword);
  CHECK(styleAt(line.find("cutoff")) == CodeStyle::Text);
  CHECK(styleAt(line.find("1kHz") + 2) == CodeStyle::Number);
  CHECK(styleAt(line.find("@label")) == CodeStyle::Name);
  CHECK(styleAt(line.find("\"Cutoff\"") + 1) == CodeStyle::String);
  CHECK(styleAt(line.find("// knob") + 3) == CodeStyle::Comment);
  s.clear();
  CHECK(highlightPatchLine("  y = svf(x) /* a", 0, s) == 1);
  CHECK(s.back().style == CodeStyle::Comment);
  s.clear();
  CHECK(highlightPatchLine("still */ z1(y)", 1, s) == 0);
  CHECK(s.front().style == CodeStyle::Comment);
}

TEST_CASE("code editor: typing, Enter keeps the indent, word moves, selection, clipboard, undo keys bubble up") {
  TestDriver d(600, 400);
  auto& ed = d.setRoot<CodeEditor>("code");
  ed.setText("patch P {\n  y = 1\n}\n");
  std::vector<std::tuple<uint32_t, uint32_t, std::string, bool>> edits;
  ed.onEdit = [&](uint32_t o, uint32_t r, std::string_view i, bool typing) { edits.emplace_back(o, r, std::string(i), typing); };
  d.tree().setFocus(&ed);
  ed.setCaret(static_cast<uint32_t>(ed.text().find("1") + 1));
  REQUIRE(d.type("2"));
  CHECK(ed.text().find("y = 12") != std::string::npos);
  CHECK(std::get<3>(edits.back()));
  d.press("Enter");
  CHECK(ed.text().find("y = 12\n  \n}") != std::string::npos);  // the line's indentation carried
  REQUIRE(d.type("z"));
  d.press("Ctrl+Backspace");
  CHECK(ed.text().find("y = 12\n  \n}") != std::string::npos);
  // Shift+Home selects to the line's first character; Ctrl+C / Ctrl+V.
  ed.setCaret(static_cast<uint32_t>(ed.text().find("12") + 2));
  d.press("Shift+Home");
  CHECK(ed.hasSelection());
  CHECK(ed.text().substr(ed.selBegin(), ed.selEnd() - ed.selBegin()) == "y = 12");
  d.press("Ctrl+C");
  d.press("End");
  d.press("Ctrl+V");
  CHECK(ed.text().find("y = 12y = 12") != std::string::npos);
  // Undo is the owner's: the key is not handled here.
  CHECK(d.press("Ctrl+Z") == KeyResult::Unhandled);
  // Up / Down keep the column.
  ed.setCaret(static_cast<uint32_t>(ed.text().find("patch") + 3));
  d.press("Down");
  CHECK(ed.lineOf(ed.caret()) == 1);
  // Tab inserts two spaces; Shift+Tab takes them back.
  const std::string before = ed.text();
  d.press("Home");
  d.press("Tab");
  CHECK(ed.text().size() == before.size() + 2);
  d.press("Shift+Tab");
  CHECK(ed.text() == before);
  // Programmatic replace keeps the caret where it was in the text after the edit.
  ed.setCaret(static_cast<uint32_t>(ed.text().find("}")));
  const uint32_t c0 = ed.caret();
  ed.replace(0, 5, "patch!!");  // before the caret: shifts it by 2
  CHECK(ed.caret() == c0 + 2);
  CHECK(ed.lineCount() == 5);
}

TEST_CASE("circuit editor: a file whose text does not parse opens in the text pane; fixing it brings the circuit") {
  TestDriver d(1200, 700);
  auto& root = d.setRoot<Widget>("root");
  root.setLayout(LayoutKind::Stack);
  (void)root.add<OverlayLayer>("overlay");
  d.tree().layoutNow();
  auto doc = PatchEditor::fromText(stdlibPath("editor_ui_test.pqp"), "patch P {\n  out y : audio\n  y = tanh(\n}\n");
  REQUIRE(doc.ok());
  CircuitEditor* e = openCircuitEditor(root, std::move(*doc));
  REQUIRE(e != nullptr);
  CHECK_FALSE(e->textPane().marks().empty());
  CHECK(e->canvas().scene() == nullptr);
  CodeEditor& text = e->textPane();
  d.tree().setFocus(&text);
  text.setCaret(static_cast<uint32_t>(text.text().find("tanh(") + 5));
  REQUIRE(d.type("0)"));
  REQUIRE(e->document().textLowered());
  REQUIRE(e->canvas().scene() != nullptr);
  CHECK(e->shownPatch() == "P");
  CHECK(e->canvas().editable());
}

TEST_CASE("circuit editor cursors (T4.16c): node Move, pin Crosshair (a wire), empty Arrow, pan Move, text pane its own") {
  Rig r;
  r.d.tree().layoutNow();
  const graph::NodeIndex t = r.atom("tanh");
  auto at = [&](Vec2 p) {
    r.d.move(p);
    return r.d.tree().cursor();
  };
  CHECK(at(r.centre(t)) == Cursor::Move);
  CHECK(r.canvas().hovered().kind == CanvasHit::Kind::Node);  // and drawn in the hover colour
  const Vec2 out = r.pin(r.named("x"), false, 0);
  CHECK(at(out) == Cursor::Crosshair);
  CHECK(r.canvas().hovered().kind == CanvasHit::Kind::Pin);  // the hovered pin is ringed
  const IRect cr = r.canvas().rect();
  const Vec2 empty{static_cast<float>(cr.x + 6), static_cast<float>(cr.bottom() - 6)};
  CHECK(at(empty) == Cursor::Arrow);
  // A press on the pin drags a wire: Crosshair all the way; the drop connects (the cursor promised a wire).
  const std::string before = r.editor->document().text();
  r.d.down(out);
  r.d.move({out.x + 20, out.y + 20});
  CHECK(r.d.tree().cursor() == Cursor::Crosshair);
  r.d.move(r.pin(t, true, 0));
  r.d.up(r.pin(t, true, 0));
  CHECK(r.editor->document().text() != before);
  // A node drag: Move while it runs.
  r.d.tree().layoutNow();
  const Vec2 c = r.centre(r.atom("tanh"));
  r.d.down(c);
  r.d.move({c.x + 30, c.y});
  CHECK(r.d.tree().cursor() == Cursor::Move);
  r.d.up({c.x + 30, c.y});
  // A middle drag pans: Move while it runs.
  r.d.down(empty, PointerButton::Middle);
  r.d.move({empty.x + 20, empty.y - 10});
  CHECK(r.d.tree().cursor() == Cursor::Move);
  r.d.up({empty.x + 20, empty.y - 10}, PointerButton::Middle);
  // The text pane's cursor is the code editor's own (nothing of the canvas leaks into it).
  const Vec2 tp{static_cast<float>(r.editor->textPane().rect().x + 40), static_cast<float>(r.editor->textPane().rect().y + 40)};
  const Cursor own = r.editor->textPane().cursorAt(tp);
  CHECK(at(tp) == (own == Cursor::Inherit ? Cursor::Arrow : own));
}
