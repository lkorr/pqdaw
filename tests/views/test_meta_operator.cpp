// TMM.12: the Meta Operator in the views — its Device-view panel is twelve meta-module faces bound to the device's
// nested instances (drags write the instance's state as one undo step and never fork the stock device), the algorithm
// picker sets the structural setting, the opened circuit shows only the chosen algorithm (T4.15 dead-branch hiding in
// the one circuit, every algorithm's visible wires checked, the node count against Operator's), presets carry the
// nested state. Goldens: meta-operator-device, meta-operator-circuit.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/edit_ops.h"
#include "pq/document/library_files.h"
#include "pq/document/meta.h"
#include "pq/meta/envelope.h"
#include "pq/meta/registry.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/browser/library_load.h"
#include "pq/views/circuit/chain_circuit.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/fake_transport.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

void checkScene(const std::string& name) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(name, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(name, {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png";
  auto ref = readPng(path);
  if (!ref) {
    (void)writePng(name + ".actual.png", *img);
    FAIL_CHECK("no golden " << name << ": wrote " << name << ".actual.png");
    return;
  }
  ImageDiff d;
  const bool same = pq::ui::test::matchesGolden(name, *img, *ref, true, &d);
  if (!same) (void)writePng(name + ".actual.png", *img);
  CHECK(same);
  MESSAGE(name << ": " << pq::ui::test::describe(d));
}

const graph::Patch& stockPatch(const char* name) {
  const graph::Patch* p = library().library().find(name);
  REQUIRE(p != nullptr);
  return *p;
}

// The shell over buildMetaOperatorProject, the Device view showing FM Keys.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1760, 900};
  std::unique_ptr<Shell> shell;
  project_patch::MetaOperatorIds ids;

  Ui() {
    ctx.setDeviceLibrary(&library());
    REQUIRE(project_patch::buildMetaOperatorProject(ctx.store(), &ids).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(520.0f);
    ctx.setSelectedTrack(ids.keys);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
    d.tree().layoutNow();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }
  DeviceChainView& view() const {
    DeviceChainView* v = findDeviceChain(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Device& dev() const {
    const doc::Device* x = doc::findDevice(*ctx.doc(), ids.op).device;
    REQUIRE(x != nullptr);
    return *x;
  }
  devices::DevicePanel& panel() {
    d.tree().layoutNow();
    devices::DevicePanel* p = view().panelOf(ids.op);
    REQUIRE(p != nullptr);
    return *p;
  }
  Widget& control(const std::string& id) {
    Widget* w = panel().control(id);
    REQUIRE_MESSAGE(w != nullptr, id);
    return *w;
  }
  // A face-local point (world units) of the face control `id`, in device px (the face is fitted into its control).
  Vec2 at(const std::string& id, float x, float y, float worldW, float worldH) {
    const IRect r = control(id).rect();
    const float s = std::min(static_cast<float>(r.w) / worldW, static_cast<float>(r.h) / worldH);
    const float ox = std::round(static_cast<float>(r.x) + (static_cast<float>(r.w) - worldW * s) * 0.5f);
    const float oy = std::round(static_cast<float>(r.y) + (static_cast<float>(r.h) - worldH * s) * 0.5f);
    return {ox + x * s, oy + y * s};
  }
  double envValue(const char* path, const char* p) const {
    const meta::Type& t = meta::envelopeType();
    return doc::metaParamValue(dev(), path, t, static_cast<size_t>(t.paramIndex(p)));
  }
};

// The visible wires into each face's pins of an opened Meta Operator: "accA.pm <- …" style facts, by source node name.
struct Visible {
  uint32_t nodes = 0, faces = 0;
  std::set<std::string> pmFrom[4];  // per carrier A, B, C, D: the visible nodes wired into its accumulator's pm
};

Visible visibleCircuit(double algorithm, double hq = 1.0, double fbOn = 0.0) {
  const graph::Patch& p = stockPatch("meta_operator");
  const std::vector<doc::DeviceValue> settings{{"algorithm", algorithm}, {"hq", hq}, {"fbOn", fbOn}};
  const auto sc = circuit::buildDeviceScene(p, settings);
  Visible v;
  v.nodes = sc->visibleNodes();
  for (const circuit::SceneFace& f : sc->faces)
    if (!sc->nodes[f.node].hidden) ++v.faces;
  // Follow each accumulator's pm back through visible muxes and adds to the envelopes that feed it.
  for (int o = 0; o < 4; ++o) {
    const std::string acc = std::string("acc") + static_cast<char>('A' + o);
    const std::optional<graph::NodeIndex> a = p.findByName(acc);
    REQUIRE(a);
    std::vector<graph::NodeIndex> stack;
    for (graph::WireIndex w = 0; w < p.wireCount(); ++w)
      if (p.wire(w).to.node == *a && p.wire(w).to.port == 1 && !sc->wires[w].hidden) stack.push_back(p.wire(w).from.node);
    while (!stack.empty()) {
      const graph::NodeIndex n = stack.back();
      stack.pop_back();
      const std::string& name = p.nodeName(n);
      if (name.starts_with("env")) {
        v.pmFrom[o].insert(name);
        continue;
      }
      for (graph::WireIndex w = 0; w < p.wireCount(); ++w)
        if (p.wire(w).to.node == n && !sc->wires[w].hidden) {
          const graph::NodeIndex src = p.wire(w).from.node;
          if (p.node(src).kind == graph::NodeKind::Input) continue;  // the algorithm's selector
          if (p.node(src).kind == graph::NodeKind::Atom && graph::atomSignature(p.node(src).atom).name == "z1") {
            v.pmFrom[o].insert("z1");
            continue;
          }
          stack.push_back(src);
        }
    }
  }
  return v;
}

}  // namespace

TEST_CASE("Meta Operator (views): the opened circuit shows only the chosen algorithm; node counts against Operator") {
  // Every algorithm: twelve faces, and exactly the algorithm's modulators wired into each phase (operator.pqp's table).
  const std::vector<std::vector<std::set<std::string>>> want = {
      {{"envB"}, {"envC"}, {"envD"}},          {{"envB"}, {"envC", "envD"}, {}},  {{"envB", "envC"}, {}, {"envD"}},
      {{"envB", "envD"}, {"envC"}, {}},        {{"envB", "envC", "envD"}, {}, {}}, {{}, {"envC"}, {"envD"}},
      {{"envB"}, {}, {"envD"}},                {{}, {"envC", "envD"}, {}},         {{}, {}, {"envD"}},
      {{"envD"}, {"envD"}, {"envD"}},          {{}, {}, {}}};
  uint32_t most = 0, fewest = 1000;
  for (int alg = 1; alg <= 11; ++alg) {
    CAPTURE(alg);
    const Visible v = visibleCircuit(alg);
    CHECK(v.faces == 12);
    for (int o = 0; o < 3; ++o) CHECK(v.pmFrom[o] == want[static_cast<size_t>(alg - 1)][static_cast<size_t>(o)]);
    // D's phase: nothing at the default (fbOn 0); its own output through one sample of delay with the loop on.
    CHECK(v.pmFrom[3].empty());
    CHECK(visibleCircuit(alg, 1.0, 1.0).pmFrom[3] == std::set<std::string>{"z1"});
    most = std::max(most, v.nodes);
    fewest = std::min(fewest, v.nodes);
    MESSAGE("algorithm " << alg << ": " << v.nodes << " visible nodes");
  }
  const graph::Patch& mo = stockPatch("meta_operator");
  const Visible def = visibleCircuit(1);
  const auto opScene = circuit::buildDeviceScene(stockPatch("operator"), {});
  const auto opAll = circuit::buildScene(stockPatch("operator"));
  MESSAGE("Meta Operator at its defaults: " << def.nodes << " visible of " << mo.nodeCount() << " (algorithms: " << fewest
                                            << " … " << most << "); Operator: " << opScene->visibleNodes() << " visible of "
                                            << opAll->nodes.size());
  // The counts the report and REVIEW quote (a change here is a change of the circuit's legibility: update both).
  CHECK(def.nodes == 41);
  CHECK(fewest == 41);
  CHECK(most == 50);  // algorithm 11: four carriers summed (three adds) and their dones (three ands)
  CHECK(opScene->visibleNodes() == 466);
  CHECK(opAll->nodes.size() == 720);
  // HQ: the operators' wash appears (×2); at 1 there is no region in the scene.
  CHECK(circuit::buildDeviceScene(mo, {{"hq", 1.0}})->regions.empty());
  const auto hq = circuit::buildDeviceScene(mo, {{"hq", 2.0}});
  REQUIRE(hq->regions.size() == 1);
  CHECK(hq->strings[hq->regions[0].text] == "\xC3\x97" "2");
}

TEST_CASE("Meta Operator (views): the panel is twelve faces bound to the nested instances, plus ratios and the algorithm") {
  Ui u;
  devices::DevicePanel& p = u.panel();
  CHECK(p.title() == "Meta Operator");
  CHECK_FALSE(p.desc().generated);
  for (const char* op : {"A", "B", "C", "D"})
    for (const char* stem : {"acc", "wt", "env", "n", "d"}) CHECK_MESSAGE(p.control(std::string(stem) + op) != nullptr, stem << op);
  CHECK(p.control("algorithm") != nullptr);
  // A drag on B's envelope amount (its depth): one undo step, written to the nested instance; the stock device stays
  // stock (a nested state edit does not fork it).
  const double before = u.envValue("envB", "amount");
  const size_t h0 = u.history();
  const float y = 110.0f + (18.0f - 110.0f) * static_cast<float>(before);
  const Vec2 a = u.at("envB", 192.0f, y, 208.0f, 128.0f);
  u.d.drag(a, {a.x, a.y + 24.0f}, 6);
  CHECK(u.history() == h0 + 1);
  CHECK(u.envValue("envB", "amount") < before);
  CHECK_FALSE(u.dev().edited());
  CHECK(u.dev().patch == "meta_operator");
  REQUIRE(u.ctx.undo());
  CHECK(u.envValue("envB", "amount") == doctest::Approx(before));
  // The algorithm picker: a click on algorithm 5 sets the structural setting (one step).
  const IRect r = u.control("algorithm").rect();
  const size_t h1 = u.history();
  // Cell 5 (index 4) of a 6 × 2 grid.
  const float pad = 3.0f * u.d.tree().scale();
  const float cw = (static_cast<float>(r.w) - 2 * pad) / 6.0f, ch = (static_cast<float>(r.h) - 2 * pad) / 2.0f;
  u.d.click({static_cast<float>(r.x) + pad + cw * 4.5f, static_cast<float>(r.y) + pad + ch * 0.5f});
  CHECK(u.history() == h1 + 1);
  double alg = 0.0;
  for (const doc::DeviceValue& s : u.dev().settings)
    if (s.name == "algorithm") alg = s.value;
  CHECK(alg == 5.0);
}

TEST_CASE("Meta Operator (views): a preset carries the nested state (ratios, envelopes, tables) and loads back equal") {
  Ui u;
  const doc::Device src = u.dev();
  REQUIRE(src.meta);
  auto text = doc::serializePreset(src, "FM Keys");
  REQUIRE(text.ok());
  const std::string dir = pq::views::test::tempDir("tmm12_presets");
  std::ofstream(std::filesystem::path(dir) / "FM Keys.pqpreset", std::ios::binary) << *text;
  const doc::TrackId other = [&] {
    auto e = doc::ops::createTrack(*u.ctx.doc(), u.ctx.store().ids(), doc::TrackKind::Midi, u.ids.keys, "Other", doc::Colour{0x8ED36C});
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    return e->tracks.at(0);
  }();
  auto id = loadDeviceItem(u.ctx, dir + "/FM Keys.pqpreset", DeviceSlot{other, 0, {}});
  REQUIRE_MESSAGE(id.ok(), (id.ok() ? "" : id.error().message));
  const doc::Device* loaded = doc::findDevice(*u.ctx.doc(), *id).device;
  REQUIRE(loaded != nullptr);
  CHECK(loaded->patch == "meta_operator");
  CHECK(loaded->kind == doc::DeviceKind::Instrument);
  CHECK(loaded->params == src.params);
  CHECK(loaded->settings == src.settings);
  REQUIRE(loaded->meta);
  CHECK(*loaded->meta == *src.meta);
}

TEST_CASE("render: meta-operator-device matches the golden image") { checkScene("meta-operator-device"); }

TEST_CASE("render: meta-operator-circuit matches the golden image") { checkScene("meta-operator-circuit"); }
