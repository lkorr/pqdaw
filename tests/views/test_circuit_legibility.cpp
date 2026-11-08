// T4.15: circuit-view legibility (SPEC §15 2026-10-26) — node tooltips (every node kind; every atom has its line;
// shown after the overlay's delay, built on hover, nothing compiled, idle afterwards), the recognised-form table of
// constants (and no false match on arbitrary values), branches switched off by structural constants hidden by default
// and drawn dimmed with Inactive (msenv at its defaults, a call's arguments), a device's settings deciding
// what is hidden, circuit edits while hidden keeping the shown layout, and the goldens circuit-inactive-hidden,
// circuit-inactive-shown and circuit-tooltip.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <random>
#include <string>

#include "../ui/golden_check.h"
#include "pq/graph/atoms.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/circuit/circuit_viewer.h"
#include "pq/views/circuit/node_info.h"
#include "pq/views/fake_transport.h"
#include "pq/views/view_context.h"
#include "pq/views/view_scenes.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::circuit;
namespace fs = std::filesystem;

namespace {

std::string stdlibPath(const char* rel) { return (fs::path(PQ_TEST_DATA_DIR) / ".." / "stdlib" / rel).string(); }

const char* kPatch =
    "// A drive stage with a mode switch. The second sentence is not shown.\n"
    "patch Drive {\n"
    "  param amount = 2 [1..10] @label(\"Amount\")\n"
    "  in    x    : audio\n"
    "  in    mode : real const = 1   // 0 clean, 1 shaped\n"
    "  out   y : audio\n"
    "  k  = 0.15915494309189535\n"
    "  sh = tanh(x * amount * k)\n"
    "  cl = abs(x)\n"
    "  y  = mode >= 0.5 ? sh : cl\n"
    "  h  = Helper(x)\n"
    "  export e = h\n"
    "}\n"
    "\n"
    "// Doubles its input.\n"
    "patch Helper {\n"
    "  in  v : audio\n"
    "  out o : audio\n"
    "  o = v * 2\n"
    "}\n";

struct Rig {
  TestDriver d{1200, 700};
  CircuitEditor* editor = nullptr;
  explicit Rig(const std::string& text = kPatch, const std::string& path = stdlibPath("legibility_test.pqp")) {
    auto& root = d.setRoot<Widget>("root");
    root.setLayout(LayoutKind::Stack);
    (void)root.add<OverlayLayer>("overlay");
    d.tree().layoutNow();
    auto doc = PatchEditor::fromText(path, text);
    REQUIRE(doc.ok());
    editor = openCircuitEditor(root, std::move(*doc));
    REQUIRE(editor != nullptr);
    d.tree().layoutNow();
  }
  explicit Rig(int, const std::string& file) {  // a stock file from disk
    auto& root = d.setRoot<Widget>("root");
    root.setLayout(LayoutKind::Stack);
    (void)root.add<OverlayLayer>("overlay");
    d.tree().layoutNow();
    auto doc = PatchEditor::open(file);
    REQUIRE_MESSAGE(doc.ok(), (doc.ok() ? std::string() : doc.error().message));
    editor = openCircuitEditor(root, std::move(*doc));
    REQUIRE(editor != nullptr);
    d.tree().layoutNow();
  }
  EditCanvas& canvas() { return editor->canvas(); }
  const CircuitScene& scene() { return *canvas().scene(); }
  const graph::Patch& patch() { return editor->document().drawn(editor->shownIndex()); }
  graph::NodeIndex named(std::string_view n) {
    const auto v = patch().findByName(n);
    REQUIRE_MESSAGE(v.has_value(), n);
    return *v;
  }
  Vec2 centre(graph::NodeIndex node) {
    const SceneNode& n = scene().nodes[node];
    return canvas().toDevice({n.x + n.w * 0.5f, n.y + n.h * 0.5f});
  }
  OverlayLayer& overlay() { return *OverlayLayer::of(*editor); }
  void open(std::string_view call) { REQUIRE(editor->openNode(named(call))); }
};

uint32_t countIf(const CircuitScene& sc, bool hidden) {
  uint32_t n = 0;
  for (const SceneNode& s : sc.nodes) n += s.hidden == hidden ? 1u : 0u;
  return n;
}

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

}  // namespace

TEST_CASE("legibility: the recognised-form table") {
  const double pi = std::numbers::pi;
  CHECK(recognisedForm(0.14814814814814814) == "4/27");
  CHECK(recognisedForm(4.0 / 27.0) == "4/27");
  CHECK(recognisedForm(static_cast<double>(static_cast<float>(4.0 / 27.0))) == "4/27");  // written in float precision
  CHECK(recognisedForm(0.15915494309189535) == "1/(2\xCF\x80)");
  CHECK(recognisedForm(-0.15915494309189535) == "\xE2\x88\x92" "1/(2\xCF\x80)");
  CHECK(recognisedForm(0.7071067811865476) == "1/\xE2\x88\x9A" "2");
  CHECK(recognisedForm(std::numbers::sqrt2) == "\xE2\x88\x9A" "2");
  CHECK(recognisedForm(pi) == "\xCF\x80");
  CHECK(recognisedForm(2 * pi) == "2\xCF\x80");
  CHECK(recognisedForm(pi / 2) == "\xCF\x80/2");
  CHECK(recognisedForm(3 * pi / 4) == "3\xCF\x80/4");
  CHECK(recognisedForm(1 / pi) == "1/\xCF\x80");
  CHECK(recognisedForm(std::numbers::ln2) == "ln 2");
  CHECK(recognisedForm(1 / std::numbers::ln2) == "1/ln 2");
  CHECK(recognisedForm(std::exp2(1.0 / 12.0)).starts_with("2^(1/12)"));
  CHECK(recognisedForm(std::pow(10.0, -6.0 / 20.0)) == "\xE2\x88\x92" "6 dB");
  CHECK(recognisedForm(std::pow(10.0, -3.0 / 20.0)) == "\xE2\x88\x92" "3 dB");
  CHECK(recognisedForm(std::pow(10.0, 12.0 / 20.0)) == "+12 dB");
  CHECK(recognisedForm(0.14285714285714285) == "1/7");
  CHECK(recognisedForm(0.007874015748031496).empty());  // 1/127: 127 > 32 is not a small denominator
  // Values that read clearly as written get none; near misses neither.
  for (const double v : {0.5, 0.25, 0.125, 1.5, 3.0, 0.001, 0.1, 440.0, 0.7071, 3.14159, 0.14814, 1e-30, 0.0})
    CHECK_MESSAGE(recognisedForm(v).empty(), v);
  CHECK(recognisedForm(0.1481481).empty());  // 7 digits, 5e-7 relative off 4/27: beyond float precision
  CHECK(recognisedForm(std::nan("")).empty());
  CHECK(recognisedForm(INFINITY).empty());
}

TEST_CASE("legibility: no recognised form for arbitrary values") {
  std::mt19937_64 rng(0x7415);
  std::uniform_real_distribution<double> e(-4.0, 4.0), u(0.0, 1.0);
  uint32_t matched = 0;
  std::string first;
  for (int i = 0; i < 20000; ++i) {
    const double v = (u(rng) < 0.5 ? -1.0 : 1.0) * std::pow(10.0, e(rng));
    const std::string f = recognisedForm(v);
    if (!f.empty()) {
      if (first.empty()) first = std::to_string(v) + " → " + f;
      ++matched;
    }
  }
  CHECK_MESSAGE(matched == 0, first);
}

TEST_CASE("legibility: every atom has its one-line description in its tooltip") {
  const graph::AtomRegistry& reg = graph::AtomRegistry::builtin();
  for (graph::AtomId id = 0; id < reg.size(); ++id) {
    const graph::AtomSignature& sig = reg.get(id);
    graph::Patch p("T");
    const graph::NodeIndex v = p.addAtom(id);
    TooltipContext c;
    c.patch = &p;
    const std::string t = nodeTooltip(c, v);
    CHECK_MESSAGE(!sig.summary.empty(), sig.name);
    CHECK_MESSAGE(t.starts_with(std::string(sig.name) + " \xC2\xB7 atom ("), t);
    CHECK_MESSAGE(t.find(std::string(sig.summary)) != std::string::npos, sig.name);
  }
}

TEST_CASE("legibility: a tooltip for each node kind") {
  Rig r;
  auto tip = [&](std::string_view name) { return r.editor->nodeTooltipText(r.named(name)); };
  // Atom (named), with its rate / poly.
  const std::string sh = tip("sh");
  CHECK(sh.starts_with("sh = tanh \xC2\xB7 atom (math)"));
  CHECK(sh.find("tanh(x)") != std::string::npos);
  CHECK(sh.find("real \xC2\xB7 audio rate \xC2\xB7 global") != std::string::npos);
  // Subcircuit: the first sentence of the comment above the callee.
  const std::string h = tip("h");
  CHECK(h.starts_with("h = Helper \xC2\xB7 subcircuit"));
  CHECK(h.find("Doubles its input.") != std::string::npos);
  // Param: label, range, value.
  const std::string a = tip("amount");
  CHECK(a.starts_with("Amount (amount) \xC2\xB7 param"));
  CHECK(a.find("1.00 \xE2\x80\xA6 10.0 \xC2\xB7 now 2.00 (the default)") != std::string::npos);
  // Input: a structural constant with its value and the declaration's comment.
  const std::string m = tip("mode");
  CHECK(m.starts_with("mode \xC2\xB7 in (real const)"));
  CHECK(m.find("= 1 (a structural constant)") != std::string::npos);
  CHECK(m.find("0 clean, 1 shaped") != std::string::npos);
  CHECK(tip("x").starts_with("x \xC2\xB7 in (real audio)"));
  // Output and export.
  CHECK(tip("y").starts_with("y \xC2\xB7 out"));
  CHECK(tip("e").starts_with("e \xC2\xB7 export"));
  // Constant: its value and recognised form.
  const std::string k = tip("k");
  CHECK(k.starts_with("0.15915494309189535 \xC2\xB7 constant"));
  CHECK(k.find("= 1/(2\xCF\x80)") != std::string::npos);
  // Inactive: abs is switched off by mode 1.
  CHECK(tip("cl").find("Inactive:") != std::string::npos);
  CHECK(tip("sh").find("Inactive:") == std::string::npos);
  // The file's comment for the patch shown at the top: its first sentence only.
  CHECK(patchDescription(r.editor->document().text(), r.editor->document().parsed().patchSpans[0].begin) ==
        "A drive stage with a mode switch.");
}

TEST_CASE("legibility: a hover shows the tooltip after the overlay's delay; nothing compiles; idle afterwards") {
  Rig r;
  r.d.advance(0.5);
  const uint64_t checks = r.editor->document().stats().checks;
  r.editor->setShowInactive(true);  // `cl` drawn (dimmed)
  r.d.advance(0.2);
  const Vec2 at = r.centre(r.named("sh"));
  r.d.move(at);
  r.d.advance(0.3);
  CHECK_FALSE(r.overlay().tooltipVisible());
  r.d.advance(0.5);  // > 0.6 s at rest
  REQUIRE(r.overlay().tooltipVisible());
  const Widget* tipW = r.overlay().find("tooltip");
  REQUIRE(tipW != nullptr);
  CHECK(tipW->rect().h > 40);  // several lines
  // Resting there costs nothing: no frames, no timers.
  r.d.advance(0.3);
  const uint64_t frames = r.d.framesDrawn();
  r.d.advance(2.0);
  CHECK(r.d.framesDrawn() == frames);
  CHECK(std::isinf(r.d.tree().nextWakeTime()));
  // Another node: the tooltip goes at once and comes back for that node after the delay.
  r.d.move(r.centre(r.named("cl")));
  CHECK_FALSE(r.overlay().tooltipVisible());
  r.d.advance(0.8);
  CHECK(r.overlay().tooltipVisible());
  // Empty canvas: gone. A press hides it too.
  r.d.move({5, 300});
  r.d.advance(0.1);
  CHECK_FALSE(r.overlay().tooltipVisible());
  r.d.move(r.centre(r.named("sh")));
  r.d.advance(0.8);
  CHECK(r.overlay().tooltipVisible());
  r.d.down(r.centre(r.named("sh")));
  CHECK_FALSE(r.overlay().tooltipVisible());
  r.d.up(r.centre(r.named("sh")));
  // Toggling and hovering compiled nothing and left the text alone.
  const std::string text = r.editor->document().text();
  r.editor->setShowInactive(false);
  r.editor->setShowInactive(true);
  r.d.advance(0.5);
  CHECK(r.editor->document().stats().checks == checks);
  CHECK(r.editor->document().text() == text);
}

TEST_CASE("legibility: hidden by default, dimmed with Inactive; the text, its layout and selection by NodeId survive") {
  Rig r;
  const std::string text = r.editor->document().text();
  CHECK_FALSE(r.editor->showInactive());
  CHECK(r.scene().nodes[r.named("cl")].hidden);
  CHECK_FALSE(r.scene().nodes[r.named("sh")].hidden);
  CHECK(r.scene().hiddenNodes == r.editor->structure().inactiveNodes);
  CHECK(r.scene().hiddenWires == r.editor->structure().inactiveWires);
  // Nothing hidden is hit or drawn.
  const SceneNode& cl = r.scene().nodes[r.named("cl")];
  const CanvasHit hidHit = r.canvas().hitTest(r.canvas().toDevice({cl.x + cl.w * 0.5f, cl.y + cl.h * 0.5f}));
  const bool hitsHidden = hidHit.kind == CanvasHit::Kind::Node && hidHit.index == r.named("cl");
  CHECK_FALSE(hitsHidden);
  const uint32_t one[1] = {r.named("sh")};
  r.canvas().select(one, {});
  r.editor->setShowInactive(true);
  CHECK(r.scene().hiddenNodes == 0);
  CHECK(r.scene().nodes[r.named("cl")].inactive);
  CHECK_FALSE(r.scene().nodes[r.named("cl")].hidden);
  REQUIRE(r.canvas().selection().nodes.size() == 1);
  CHECK(r.canvas().selection().nodes[0] == r.named("sh"));
  CHECK(r.editor->document().text() == text);
}

TEST_CASE("legibility: a circuit edit while hidden stores the layout that is shown") {
  Rig r;
  REQUIRE(r.scene().hiddenNodes > 0);
  const graph::NodeIndex sh = r.named("sh");
  const SceneNode before = r.scene().nodes[sh];
  const std::vector<uint32_t> nodes{sh};
  REQUIRE(r.editor->document().moveNodes(r.editor->shownIndex(), nodes, 16.0f, 8.0f).ok());
  // The moved node lands where the view had it plus the move; the others stay where they were shown.
  const SceneNode& after = r.scene().nodes[r.named("sh")];
  CHECK(after.x == before.x + 16.0f);
  CHECK(after.y == before.y + 8.0f);
  const graph::Patch& stored = r.editor->document().parsed().file.patches[r.editor->shownIndex()];
  CHECK(stored.layout(*stored.findByName("sh")).placed);
  // Tidy while hidden re-places the visible nodes only.
  const SceneNode clBefore = r.scene().nodes[r.named("cl")];
  REQUIRE(r.editor->tidy());
  CHECK(r.scene().nodes[r.named("cl")].x == clBefore.x);
  CHECK(r.scene().nodes[r.named("cl")].y == clBefore.y);
}

// GU.11: the stock Saturator and Operator patches are retired (their devices are group presets now): the multi-stage
// envelope (stdlib/envelopes/msenv.pqp: `mode` and `soft`, structural constants) stands for a stock file at its
// defaults, and a two-level patch here for the arguments a call passes down (what Saturator's sat_shape and Operator's
// op_bank / op_wave showed).
TEST_CASE("legibility: msenv at its defaults shows only its active logic; Inactive shows every branch") {
  Rig r(0, stdlibPath("envelopes/msenv.pqp"));
  REQUIRE(r.editor->shownPatch() == "msenv");
  const CircuitScene& top = r.scene();
  const uint32_t total = static_cast<uint32_t>(top.nodes.size());
  MESSAGE("msenv: " << total << " nodes, " << countIf(top, true) << " hidden at mode 0");
  CHECK(countIf(top, true) == r.editor->structure().inactiveNodes);
  CHECK(countIf(top, true) > 0);
  // mode 0 (None): the Loop / Beat / Sync retriggers and Trigger's latch are switched off; soft 1: the hard start.
  for (const char* off : {"loopTrig", "beatTrig", "syncTrig", "beatPh", "songPh", "settled"})
    CHECK_MESSAGE(top.nodes[r.named(off)].hidden, off);
  for (const char* on : {"start0", "held", "start", "next", "out", "done", "b"})
    CHECK_FALSE_MESSAGE(top.nodes[r.named(on)].hidden, on);
  CHECK(top.visibleNodes() == total - countIf(top, true));
  r.editor->setShowInactive(true);
  CHECK(r.scene().hiddenNodes == 0);
  CHECK(r.scene().visibleNodes() == total);
  for (const char* off : {"loopTrig", "beatTrig", "syncTrig", "settled"}) {
    CHECK_FALSE(r.scene().nodes[r.named(off)].hidden);
    CHECK_MESSAGE(r.scene().nodes[r.named(off)].inactive, off);  // drawn dimmed
  }
}

const char* kNested =
    "// A shaper whose curve is chosen by a structural constant, called with it.\n"
    "patch Shaper {\n"
    "  in    x    : audio\n"
    "  in    type : real const = 1\n"
    "  in    soft : real const = 0\n"
    "  param amount = 2 [1..10] @label(\"Amount\")\n"
    "  out   y : audio\n"
    "  pre = x * amount\n"
    "  ya  = Curve(pre, type)\n"
    "  yb  = Curve(pre * 2, 0)\n"
    "  y   = soft >= 0.5 ? yb : ya\n"
    "}\n"
    "\n"
    "// One of three curves.\n"
    "patch Curve {\n"
    "  in    v    : audio\n"
    "  in    type : real const = 0\n"
    "  out   o : audio\n"
    "  clean = abs(v)\n"
    "  sh    = tanh(v)\n"
    "  cl    = clip(v, -1, 1)\n"
    "  o     = type >= 1.5 ? cl : type >= 0.5 ? sh : clean\n"
    "}\n";

TEST_CASE("legibility: inside a call only the branch its arguments choose is shown; the arguments follow the stack") {
  Rig r(kNested, stdlibPath("legibility_nested.pqp"));
  REQUIRE(r.editor->shownPatch() == "Shaper");
  // soft 0: the second call (yb) is switched off.
  CHECK(r.scene().nodes[r.named("yb")].hidden);
  CHECK_FALSE(r.scene().nodes[r.named("ya")].hidden);
  MESSAGE("Shaper: " << r.scene().nodes.size() << " nodes, " << r.scene().hiddenNodes << " hidden");
  // Into ya = Curve(pre, type): type 1 arrives there from the root's default — only tanh is drawn.
  r.open("ya");
  REQUIRE(r.editor->shownPatch() == "Curve");
  CHECK(r.editor->shownArguments().size() == 1);  // type 1 from the root's default
  CHECK_FALSE(r.scene().nodes[r.named("sh")].hidden);
  CHECK(r.scene().nodes[r.named("cl")].hidden);
  CHECK(r.scene().nodes[r.named("clean")].hidden);
  r.editor->setShowInactive(true);
  CHECK(r.scene().hiddenNodes == 0);
  r.editor->setShowInactive(false);
  // Back up: the arguments follow the stack.
  REQUIRE(r.editor->back());
  CHECK(r.editor->shownPatch() == "Shaper");
  CHECK(r.editor->shownArguments().empty());
}

TEST_CASE("legibility: a device's settings decide what is hidden; the view prefs remember the toggle") {
  Rig r(0, stdlibPath("envelopes/msenv.pqp"));
  const uint32_t at0 = r.scene().hiddenNodes;
  CHECK(r.scene().nodes[r.named("loopTrig")].hidden);
  r.editor->setInstance("msenv", {{"mode", 1.0}, {"soft", 1.0}}, {{"rep", 2.0}});  // Loop
  CHECK_FALSE(r.scene().nodes[r.named("loopTrig")].hidden);
  CHECK(r.scene().nodes[r.named("beatTrig")].hidden);
  CHECK(r.scene().hiddenNodes != at0);
  // Its params' current values in the tooltips.
  MESSAGE("rep tooltip: " << r.editor->nodeTooltipText(r.named("rep")));
  CHECK(r.editor->nodeTooltipText(r.named("rep")).find("now 2") != std::string::npos);
  // A call's arguments come from the instance too: Shaper with type 2 shows the clip curve inside.
  {
    Rig n(kNested, stdlibPath("legibility_nested.pqp"));
    n.editor->setInstance("Shaper", {{"type", 2.0}, {"soft", 0.0}}, {{"amount", 3.0}});
    n.open("ya");
    REQUIRE(n.editor->shownPatch() == "Curve");
    CHECK_FALSE(n.scene().nodes[n.named("cl")].hidden);
    CHECK(n.scene().nodes[n.named("sh")].hidden);
  }

  FakeTransport port;
  ViewContext ctx(port);
  ViewPrefs saved;
  int saves = 0;
  ctx.setViewPrefsListener([&](const ViewPrefs& v) {
    saved = v;
    ++saves;
  });
  followViewPrefs(*r.editor, ctx);
  CHECK_FALSE(r.editor->showInactive());
  Widget* toggle = r.editor->find("bar/inactive");
  REQUIRE(toggle != nullptr);
  r.d.click({static_cast<float>(toggle->rect().x + toggle->rect().w / 2), static_cast<float>(toggle->rect().y + toggle->rect().h / 2)});
  CHECK(r.editor->showInactive());
  CHECK(saves == 1);
  CHECK(saved.circuitShowInactive);
  CHECK(ctx.viewPrefs().circuitShowInactive);
}

TEST_CASE("render: circuit-inactive-hidden matches the golden image") { checkGolden("circuit-inactive-hidden", stdlibPath("envelopes/msenv.pqp")); }
TEST_CASE("render: circuit-inactive-shown matches the golden image") { checkGolden("circuit-inactive-shown", stdlibPath("envelopes/msenv.pqp")); }
TEST_CASE("render: circuit-tooltip matches the golden image") { checkGolden("circuit-tooltip", stdlibPath("envelopes/msenv.pqp")); }
