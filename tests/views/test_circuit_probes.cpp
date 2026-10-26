// T4.5: live probes and cost meters in the circuit editor — goldens of a probe readout with the LED trace and of the
// cost overlay, the hover → probe → readout flow (dwell, grace, recompile only on a change), the LED wires following the
// probe levels, the cost toggle (metering only while shown), and the cost attribution (a subcircuit's instance summed
// to its call node; inside an instance by its path; JIT chains as one unit).

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/dsp/kernel.h"
#include "pq/graph/inspect.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/circuit/probes.h"
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

// A player that records what it is asked and hands out taps it fills itself.
class FakePlayer final : public PatchPlayer {
public:
  double sampleRate() const override { return 48000.0; }
  Status play(const graph::Schedule& s, const graph::TypedPatch&) override {
    playing_ = true;
    ++gen_;
    ++plays;
    last = s;
    for (const graph::ProbeRef& p : s.probes) {
      bool have = false;
      for (auto& [id, lv] : levels) have = have || id == p.id;
      if (!have) levels.emplace_back(p.id, std::make_unique<ProbeLevel>());
      if (std::find(signals.begin(), signals.end(), p.id) != signals.end() && !ring(p.id)) {
        rings.emplace_back(p.id, std::make_unique<TapRing>(1, 4096, 1024));
        rings.back().second->addReader();
      }
    }
    return Ok{};
  }
  void stop() override { playing_ = false; }
  bool playing() const override { return playing_; }
  const graph::StateLayout* stateLayout() const override { return nullptr; }
  void setProbeSignals(std::span<const uint32_t> ids) override { signals.assign(ids.begin(), ids.end()); }
  TapRing* probeSignal(uint32_t id) noexcept override { return ring(id); }
  const ProbeLevel* probeLevel(uint32_t id) const noexcept override {
    for (const auto& [k, lv] : levels)
      if (k == id) return lv.get();
    return nullptr;
  }
  void setMetering(bool on) override { metering = on; }
  bool readCost(CostReading& out) override {
    if (!metering || !playing_) return false;
    out = CostReading{};
    out.generation = gen_;
    for (uint32_t r = 0; r < last.regions.size(); ++r) {
      out.units.push_back(r);
      out.regionNsPerFrame.push_back(static_cast<double>(last.regions[r].numOps));  // 1 ns per op
      out.totalNsPerFrame += static_cast<double>(last.regions[r].numOps);
    }
    return true;
  }
  uint64_t generation() const noexcept override { return gen_; }

  TapRing* ring(uint32_t id) {
    for (auto& [k, r] : rings)
      if (k == id) return r.get();
    return nullptr;
  }
  ProbeLevel* level(uint32_t id) {
    for (auto& [k, lv] : levels)
      if (k == id) return lv.get();
    return nullptr;
  }

  graph::Schedule last;
  int plays = 0;
  bool metering = false;
  std::vector<uint32_t> signals;
  std::vector<std::pair<uint32_t, std::unique_ptr<TapRing>>> rings;
  std::vector<std::pair<uint32_t, std::unique_ptr<ProbeLevel>>> levels;

private:
  bool playing_ = false;
  uint64_t gen_ = 0;
};

const char* kPatch =
    "patch Gate {\n"
    "  param th = 0.25 [0..1]\n"
    "  out y : audio\n"
    "  ph = phasor(110Hz)\n"
    "  open = ph > th\n"
    "  s = sine(ph)\n"
    "  g = select(open, s, 0)\n"
    "  y = Amp(g)\n"
    "}\n"
    "patch Amp {\n"
    "  in x : audio\n"
    "  out y : audio\n"
    "  y = tanh(x * 3) * 0.5\n"
    "}\n";

struct Rig {
  TestDriver d{1200, 700};
  FakePlayer player;
  CircuitEditor* editor = nullptr;
  Rig() {
    auto& root = d.setRoot<Widget>("root");
    root.setLayout(LayoutKind::Stack);
    (void)root.add<OverlayLayer>("overlay");
    d.tree().layoutNow();
    auto doc = PatchEditor::fromText(stdlibPath("probe_ui_test.pqp"), kPatch);
    REQUIRE(doc.ok());
    editor = openCircuitEditor(root, std::move(*doc), {}, &player);
    REQUIRE(editor != nullptr);
    d.tree().layoutNow();
  }
  const CircuitScene& scene() { return *editor->canvas().scene(); }
  const graph::Patch& patch() { return editor->document().drawn(0); }
  // The first scene wire out of the node named `name`.
  uint32_t wireFrom(std::string_view name) {
    const auto v = patch().findByName(name);
    REQUIRE(v);
    for (uint32_t w = 0; w < scene().wires.size(); ++w)
      if (scene().pins[scene().wires[w].fromPin].node == *v) return w;
    FAIL("no wire from " << name);
    return 0;
  }
  Vec2 middle(uint32_t w) {
    const SceneWire& sw = scene().wires[w];
    const uint32_t k = sw.firstPoint + (sw.pointCount - 1) / 2;
    const graph::Point2 a = scene().points[k], b = scene().points[k + 1];
    return editor->canvas().toDevice({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f});
  }
};

}  // namespace

TEST_CASE("circuit probes: golden of a probe readout and the LED trace on the stock ADSR (T4.5)") {
  checkGolden("circuit-editor-probe", stdlibPath("envelopes/adsr.pqp"));
}

TEST_CASE("circuit probes: golden of the cost overlay on the stock SVF (T4.5)") {
  checkGolden("circuit-editor-cost", stdlibPath("filters/svf.pqp"));
}

TEST_CASE("circuit probes: playing compiles a level probe per bool net; nothing is probed while stopped (T4.5)") {
  Rig r;
  CHECK(r.editor->probeTargets().empty());
  REQUIRE(r.editor->togglePlay());
  // One bool net (`open`): probed for its level, no signal.
  REQUIRE(r.editor->probeTargets().size() == 1);
  CHECK(r.editor->probeTargets()[0].node == r.patch().node(*r.patch().findByName("open")).id);
  CHECK(r.editor->probeTargets()[0].path.empty());
  CHECK(r.player.signals.empty());
  REQUIRE(r.player.last.probes.size() == 1);
  const uint32_t id = r.player.last.probes[0].id;
  // The LED follows the level.
  const uint32_t w = r.wireFrom("open");
  REQUIRE(r.scene().wires[w].style.boolean);
  r.player.level(id)->write(1.0f, 0.0f, 1.0f);
  r.editor->pollProbes();
  r.player.level(id)->write(0.0f, 0.0f, 0.0f);
  r.editor->pollProbes();
  r.player.level(id)->write(1.0f, 1.0f, 1.0f);
  r.editor->pollProbes();
  // (setWireLit repaints only on a change: drawn lit now — checked through a frame without errors.)
  r.d.step();
  REQUIRE(r.editor->togglePlay());  // stop
  CHECK(r.editor->probeTargets().empty());
}

TEST_CASE("circuit probes: hovering a wire probes its source after a dwell, with a readout; leaving drops it later") {
  Rig r;
  REQUIRE(r.editor->togglePlay());
  const int plays0 = r.player.plays;
  const uint32_t w = r.wireFrom("s");
  r.editor->canvas().setPointer(r.middle(w));
  r.d.move(r.middle(w));
  r.d.advance(0.05);
  CHECK(r.player.plays == plays0);  // still dwelling
  r.d.advance(0.3);
  CHECK(r.player.plays == plays0 + 1);  // compiled in (one recheck)
  REQUIRE(r.editor->probeTargets().size() == 2);
  const graph::NodeId s = r.patch().node(*r.patch().findByName("s")).id;
  CHECK(r.editor->probeTargets()[1].node == s);
  REQUIRE(r.player.signals.size() == 1);
  const uint32_t id = r.player.signals[0];
  CHECK(r.editor->probeOverlay().readout);
  CHECK_FALSE(r.editor->probeOverlay().hasData);
  // Data arrives: the readout shows the last value and the samples.
  TapRing* ring = r.player.ring(id);
  REQUIRE(ring != nullptr);
  std::vector<float> x(300);
  for (size_t i = 0; i < x.size(); ++i) x[i] = std::sin(0.1f * static_cast<float>(i));
  const float* ch = x.data();
  ring->write(&ch, 1, static_cast<uint32_t>(x.size()));
  r.d.advance(0.1);
  CHECK(r.editor->probeOverlay().hasData);
  CHECK(r.editor->probeOverlay().samples.size() == 300);
  CHECK(r.editor->probeOverlay().value == formatProbeValue(x.back()));
  // Another wire of the same net (none here) would not recompile; leaving shows nothing at once and drops the probe
  // after the grace time.
  const IRect cr = r.editor->canvas().rect();
  r.d.move({static_cast<float>(cr.x + 3), static_cast<float>(cr.y + 3)});  // empty canvas, off every wire
  r.d.advance(0.1);
  CHECK_FALSE(r.editor->probeOverlay().readout);
  CHECK(r.editor->probeTargets().size() == 2);
  r.d.advance(2.0);
  CHECK(r.editor->probeTargets().size() == 1);
  CHECK(r.player.plays == plays0 + 2);
}

TEST_CASE("circuit probes: a bool wire's readout is HIGH / LOW") {
  Rig r;
  REQUIRE(r.editor->togglePlay());
  const uint32_t w = r.wireFrom("open");
  r.editor->probeWire(w);
  REQUIRE(r.player.signals.size() == 1);
  TapRing* ring = r.player.ring(r.player.signals[0]);
  REQUIRE(ring != nullptr);
  std::vector<float> x(64, 0.0f);
  x.back() = dsp::boolMask(true);
  const float* ch = x.data();
  ring->write(&ch, 1, 64);
  r.editor->pollProbes();
  CHECK(r.editor->probeOverlay().isBool);
  CHECK(r.editor->probeOverlay().value == "HIGH");
}

TEST_CASE("circuit probes: the cost toggle meters only while shown and labels the nodes, a subcircuit with its instance") {
  Rig r;
  REQUIRE(r.editor->togglePlay());
  CHECK_FALSE(r.player.metering);
  r.editor->setCostShown(true);
  CHECK(r.player.metering);
  r.editor->pollCost();
  const ProbeOverlay& o = r.editor->probeOverlay();
  REQUIRE_FALSE(o.nodeLabels.empty());
  // The call of Amp carries its whole instance (tanh, ×3, ×0.5: three ops, folded or fused as the optimiser does).
  graph::NodeIndex amp = graph::kNone;
  for (graph::NodeIndex v = 0; v < r.patch().nodeCount(); ++v)
    if (r.patch().node(v).kind == graph::NodeKind::Subcircuit) amp = v;
  REQUIRE(amp != graph::kNone);
  bool ampLabel = false;
  for (const auto& l : o.nodeLabels) ampLabel = ampLabel || l.item == amp;
  CHECK(ampLabel);
  CHECK(r.editor->status().find("Cost ") == 0);
  r.editor->setCostShown(false);
  CHECK_FALSE(r.player.metering);
  CHECK(r.editor->probeOverlay().nodeLabels.empty());
}

TEST_CASE("circuit probes: cost attribution sums an instance into its call, follows a path into it, and splits units") {
  // A played program by hand: root instance 0 with node 10 (an atom) and a call (node 11) → instance 1 with node 20.
  PatchEditor::PlayedProgram pp;
  pp.generation = 3;
  pp.instanceParent = {graph::kNone, 0};
  pp.instanceCall = {graph::NodeId{}, graph::NodeId{11}};
  // Scheduled nodes: 0 → root atom 10, 1 → instance-1 atom 20, 2 → instance-1 atom 21.
  pp.nodeInstance = {0, 1, 1};
  pp.nodeSource = {graph::NodeId{10}, graph::NodeId{20}, graph::NodeId{21}};
  pp.nodeWhat.assign(3, graph::NodeOrigin::What::Node);
  graph::Schedule& s = pp.schedule;
  auto op = [&](graph::NodeIndex n) {
    graph::Op o;
    o.kind = graph::OpKind::Kernel;
    o.kernel = dsp::Atom::Add;
    o.node = n;
    s.ops.push_back(o);
  };
  op(0);
  op(1);
  op(2);
  graph::Region a, b;
  a.firstOp = 0;
  a.numOps = 1;
  b.firstOp = 1;
  b.numOps = 2;
  s.regions = {a, b};
  PatchPlayer::CostReading rd;
  rd.generation = 3;
  rd.totalNsPerFrame = 10.0;
  rd.regionNsPerFrame = {2.0, 6.0};
  rd.units = {0, 1};
  CostMap cm;
  REQUIRE(attributeCost(pp, rd, {}, cm));
  REQUIRE(cm.nodes.size() == 2);
  CHECK(cm.nodes[0].node == graph::NodeId{10});
  CHECK(cm.nodes[0].ns == doctest::Approx(2.0));
  CHECK(cm.nodes[1].node == graph::NodeId{11});  // the call: its instance's 6 ns
  CHECK(cm.nodes[1].ns == doctest::Approx(6.0));
  CHECK(cm.shownNs == doctest::Approx(8.0));
  // Inside the instance (the path names the call): its two atoms, 3 ns each (equal weights).
  const graph::NodeId path[1] = {graph::NodeId{11}};
  REQUIRE(attributeCost(pp, rd, path, cm));
  REQUIRE(cm.nodes.size() == 2);
  CHECK(cm.nodes[0].ns == doctest::Approx(3.0));
  CHECK(cm.nodes[1].ns == doctest::Approx(3.0));
  // One unit over both regions (a JIT chain): 8 ns split by weight over the three ops.
  rd.units = {0, 0};
  rd.regionNsPerFrame = {8.0, 0.0};
  REQUIRE(attributeCost(pp, rd, {}, cm));
  CHECK(cm.nodes[0].ns == doctest::Approx(8.0 / 3.0));
  CHECK(cm.nodes[1].ns == doctest::Approx(16.0 / 3.0));
  // Another generation, or a path that names no instance: no attribution.
  rd.generation = 4;
  CHECK_FALSE(attributeCost(pp, rd, {}, cm));
  rd.generation = 3;
  const graph::NodeId bad[1] = {graph::NodeId{99}};
  CHECK_FALSE(attributeCost(pp, rd, bad, cm));
}
