// T4.4-fix1: the circuit editor on the project's devices (SPEC §15 2026-10-26), through the real shell — "Open in Circuit
// View" in a device's context menu and on a double-click of its title; edits from the text pane and the canvas become
// document commands (the first one forks the stock device: one undo step; a typing run is one step); Ctrl+Z in the
// editor is the document's undo and the editor follows any change of the device's text (undo, redo, Revert to Stock);
// the device removed closes the editor; an external player gets the editor's probes with device-relative paths and
// the cost path in front; View > Open Patch… is a developer option now. Golden: circuit-editor-device.

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/circuit/device_circuit.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/fake_transport.h"
#include "../../src/views/src/shell/shell_widgets.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../ui/golden_check.h"
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

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + static_cast<float>(r.w) / 2.0f, static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f}; }

// The shell over two MIDI tracks (Basic Synth each), the first shown in the Device view.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  doc::TrackId keys, other;
  doc::DeviceId inst, otherInst;

  Ui() {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    auto run = [&](const doc::ops::EditResult& e) {
      REQUIRE(e.ok());
      REQUIRE(st.dispatch(e->command).ok());
      return *e;
    };
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks[0];
    other = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Other")).tracks[0];
    inst = doc::findTrackPtr(*st.current(), keys)->devices.at(0)->id;
    otherInst = doc::findTrackPtr(*st.current(), other)->devices.at(0)->id;
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(300.0f);
    ctx.setSelectedTrack(keys);
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
  const doc::Device& dev(doc::DeviceId id) const { return *doc::findDevice(*ctx.doc(), id).device; }
  size_t history() const { return ctx.store().history().cursor(); }
  circuit::CircuitEditor* editor() const {
    for (size_t i = 0; i < shell->overlay().childCount(); ++i)
      if (auto* e = dynamic_cast<circuit::CircuitEditor*>(&shell->overlay().child(i))) return e;
    return nullptr;
  }
  Menu* openMenu() const {
    for (size_t i = 0; i < shell->overlay().childCount(); ++i)
      if (auto* m = dynamic_cast<Menu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  int32_t itemIndex(const Menu& m, const std::string& label) const {
    for (size_t i = 0; i < m.items().size(); ++i)
      if (m.items()[i].label == label) return static_cast<int32_t>(i);
    return -1;
  }
  // Types `with` over [offset, offset + removed) as the text pane reports a keystroke.
  void type(circuit::CircuitEditor& ed, uint32_t offset, uint32_t removed, std::string_view with) {
    ed.textPane().replace(offset, removed, with);
    ed.textPane().onEdit(offset, removed, with, true);
  }
};

std::string stockText() { return library().stockText("BasicSynth"); }

}  // namespace

TEST_CASE("device circuit view: open from the context menu; the first edit forks; undo is the document's") {
  Ui u;
  // The device's context menu: Open in Circuit View, and Revert to Stock (greyed: a stock device).
  Widget* frame = u.view().frameOf(u.inst);
  REQUIRE(frame != nullptr);
  u.d.tree().layoutNow();
  u.d.click(mid(frame->rect()), mod::kNone, PointerButton::Right);
  Menu* menu = u.openMenu();
  REQUIRE(menu != nullptr);
  const int32_t open = u.itemIndex(*menu, "Open in Circuit View"), revert = u.itemIndex(*menu, "Revert to Stock");
  REQUIRE(open >= 0);
  REQUIRE(revert >= 0);
  CHECK_FALSE(menu->items()[static_cast<size_t>(revert)].enabled);
  menu->choose(open);
  u.d.step();
  circuit::CircuitEditor* ed = u.editor();
  REQUIRE(ed != nullptr);
  REQUIRE(ed->host() != nullptr);
  CHECK(ed->title().rfind("Basic Synth \xC2\xB7 Keys", 0) == 0);
  CHECK(ed->shownPatch() == "BasicSynth");
  CHECK(ed->document().text() == stockText());
  CHECK_FALSE(ed->save());  // the project holds the text: nothing is written
  // The editor opened with the library's NodeIds (the ones the project plays).
  const graph::Patch* stockPatch = library().library().find("BasicSynth");
  const graph::Patch& shown = ed->document().parsed().file.patches.at(0);
  REQUIRE(shown.nodeCount() == stockPatch->nodeCount());
  for (graph::NodeIndex v = 0; v < shown.nodeCount(); ++v) CHECK(shown.node(v).id == stockPatch->node(v).id);

  // Typing in the text pane: the first keystroke that parses forks the device (one step); the run is one step.
  const size_t h0 = u.history();
  const uint32_t at = static_cast<uint32_t>(ed->document().text().find("param level   = 0.2") + 18);
  u.type(*ed, at + 1, 0, "5");  // 0.25
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.dev(u.inst).edited());
  CHECK(u.dev(u.inst).source->text == ed->document().text());
  CHECK(u.dev(u.inst).source->ids != nullptr);  // the editor's parse goes with it (the ids the project plays)
  CHECK(ed->title().find("(edited)") != std::string::npos);
  CHECK(ed->status().find("track's own copy") != std::string::npos);
  u.type(*ed, at + 2, 0, "5");  // 0.255: the same typing run
  CHECK(u.history() == h0 + 1);
  CHECK(u.dev(u.inst).source->text.find("param level   = 0.255") != std::string::npos);
  // The other instance and the stock file are untouched; the panel shows the marker.
  CHECK_FALSE(u.dev(u.otherInst).edited());
  CHECK(library().stockText("BasicSynth") == stockText());
  u.d.step();
  REQUIRE(u.view().panelOf(u.inst) != nullptr);
  CHECK(u.view().panelOf(u.inst)->edited());
  // A circuit edit (a move: a layout edit) is a step of its own.
  REQUIRE(ed->document().moveNodes(0, std::vector<graph::NodeIndex>{0}, 16.0f, 8.0f).ok());
  CHECK(u.history() == h0 + 2);
  const std::string moved = ed->document().text();
  CHECK(u.dev(u.inst).source->text == moved);

  // Ctrl+Z in the editor: the document's undo; the editor follows.
  REQUIRE(ed->undo());
  CHECK(u.history() == h0 + 1);
  CHECK(ed->document().text() == u.dev(u.inst).source->text);
  CHECK(ed->document().text().find("param level   = 0.255") != std::string::npos);
  REQUIRE(ed->undo());  // the fork: the stock reference again
  CHECK(u.history() == h0);
  CHECK_FALSE(u.dev(u.inst).edited());
  CHECK(ed->document().text() == stockText());
  CHECK(ed->title().find("(edited)") == std::string::npos);
  REQUIRE(ed->redo());
  REQUIRE(ed->redo());
  CHECK(ed->document().text() == moved);

  // Revert to Stock (the device's menu) while the editor is open: one step, the editor shows the stock text.
  const doc::DeviceId one[1] = {u.inst};
  u.view().select(one);
  REQUIRE(u.view().revertSelectedToStock());
  CHECK_FALSE(u.dev(u.inst).edited());
  CHECK(ed->document().text() == stockText());
  REQUIRE(u.ctx.undo());
  CHECK(ed->document().text() == moved);

  // A text that does not parse stays in the editor; the project keeps the last text that did.
  const size_t h1 = u.history();
  const std::string before = u.dev(u.inst).source->text;
  u.type(*ed, 0, 0, "patch {");
  CHECK(u.history() == h1);
  CHECK(u.dev(u.inst).source->text == before);
  u.type(*ed, 0, 7, "");
  CHECK(u.history() == h1);  // back to the same text: nothing new

  // The device deleted: the editor closes.
  REQUIRE(u.ctx.store().dispatch(doc::ops::deleteDevices(*u.ctx.doc(), std::vector<doc::DeviceId>{u.inst})->command).ok());
  u.d.advance(0.1);
  CHECK(u.editor() == nullptr);
}

TEST_CASE("device circuit view: a double-click on the device's title opens it") {
  Ui u;
  devices::DevicePanel* panel = u.view().panelOf(u.inst);
  REQUIRE(panel != nullptr);
  REQUIRE(panel->onTitleDoubleClick);
  Widget* title = panel->find("title");
  REQUIRE(title != nullptr);
  u.d.tree().layoutNow();
  const IRect r = title->rect();
  u.d.doubleClick({static_cast<float>(r.x + r.w / 2), static_cast<float>(r.y + r.h / 2)});
  u.d.step();
  circuit::CircuitEditor* ed = u.editor();
  REQUIRE(ed != nullptr);
  CHECK_FALSE(panel->folded());  // a double-click no longer folds a chain device (the triangle does)
  ed->closeSoon();
  u.d.advance(0.1);
  CHECK(u.editor() == nullptr);
}

namespace {

// An external player that records what the editor gives it (the app's is app::DevicePlayer over the project).
class FakeExternal final : public circuit::PatchPlayer {
public:
  double sampleRate() const override { return 48000.0; }
  Status play(const graph::Schedule&, const graph::TypedPatch&) override {
    ++plays;
    return Ok{};
  }
  void stop() override { ++stops; }
  bool playing() const override { return true; }
  const graph::StateLayout* stateLayout() const override { return nullptr; }
  bool external() const override { return true; }
  void setProbeTargets(std::span<const graph::ProbeTarget> t, std::span<const uint32_t> i) override {
    targets.assign(t.begin(), t.end());
    ids.assign(i.begin(), i.end());
    ++targetCalls;
  }
  std::vector<graph::NodeId> pathPrefix() const override { return {graph::NodeId{7}, graph::NodeId{9}}; }
  std::vector<graph::ProbeTarget> targets;
  std::vector<uint32_t> ids;
  int plays = 0, stops = 0, targetCalls = 0;
};

}  // namespace

TEST_CASE("device circuit view: an external player gets the probes; the editor never hands it a schedule") {
  Ui u;
  FakeExternal* fake = nullptr;
  u.ctx.setDevicePlayers([&](doc::DeviceId) -> std::unique_ptr<circuit::PatchPlayer> {
    auto p = std::make_unique<FakeExternal>();
    fake = p.get();
    return p;
  });
  circuit::CircuitEditor* ed = circuit::openDeviceInCircuitView(u.view(), u.ctx, u.inst);
  REQUIRE(ed != nullptr);
  REQUIRE(fake != nullptr);
  CHECK(ed->document().playing());  // it follows the project from the start
  CHECK(fake->targetCalls >= 1);
  // BasicSynth's bool nets (gate, attacking, done…) are LED probes, paths from the device's patch (the root: empty).
  CHECK_FALSE(fake->targets.empty());
  for (const graph::ProbeTarget& t : fake->targets) CHECK(t.path.empty());
  // An edit is checked by the editor and played by the project: no schedule ever reaches the player.
  const uint32_t at = static_cast<uint32_t>(ed->document().text().find("param level   = 0.2") + 18);
  u.type(*ed, at, 1, "3");
  ed->checkNow();
  CHECK(fake->plays == 0);
  CHECK(u.dev(u.inst).edited());
  // Closing the editor stops the inspection.
  ed->closeSoon();
  u.d.advance(0.1);
  CHECK(u.editor() == nullptr);
  u.ctx.setDevicePlayers(nullptr);
}

TEST_CASE("device circuit view: loose .pqp files are a developer option (Options > Developer)") {
  Ui u;
  auto* bar = dynamic_cast<shell::MenuBar*>(u.d.find("main/menu-bar"));
  REQUIRE(bar != nullptr);
  const std::vector<MenuItem> view = bar->items(3);
  CHECK(std::none_of(view.begin(), view.end(), [](const MenuItem& m) { return m.action == "view.openPatch"; }));
  CHECK(std::none_of(view.begin(), view.end(), [](const MenuItem& m) { return m.action == "view.openDevice"; }));
  const std::vector<MenuItem> options = bar->items(4);
  const auto dev = std::find_if(options.begin(), options.end(), [](const MenuItem& m) { return m.label == "Developer"; });
  REQUIRE(dev != options.end());
  CHECK(std::any_of(dev->submenu.begin(), dev->submenu.end(), [](const MenuItem& m) { return m.action == "view.openPatch"; }));
  CHECK(std::any_of(dev->submenu.begin(), dev->submenu.end(), [](const MenuItem& m) { return m.action == "view.openDevice"; }));
}

TEST_CASE("render: circuit-editor-device matches the golden image") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize("circuit-editor-device", w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene("circuit-editor-device", {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/circuit-editor-device.png";
  auto ref = readPng(path);
  if (!ref) (void)writePng("circuit-editor-device.actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden("circuit-editor-device", *img, *ref, true, &d));
  MESSAGE("circuit-editor-device: " << pq::ui::test::describe(d));
}
