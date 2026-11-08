// T4.18 device automation in the views, through the real shell: the lane chooser's device sections (device params,
// rack macros and selector, chain volume / pan, wire amounts), a device param's lane drawn in its knob's scale and named
// "<device>: <param>", the first click on its line creating the lane (with the param's name) and a breakpoint in one
// undo step, a knob's "Show Automation" opening its lane in the arrangement, automated knobs showing the envelope at
// the playhead with an LED (grey and their own value once overridden by hand; the 30 Hz timer only while playing),
// lanes surviving a device move in the Device view and leaving with a deleted device (one undo step each), and the
// golden `device-automation`.

#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/main_path.h"
#include "pq/views/devices/device_model.h"
#include "pq/core/automation_curve.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/arrangement/automation.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/devices/knob_curve_menu.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../support/knob_positions.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr Ticks kBar = kTicksPerWhole;
constexpr const char* kArr = "main/body/top/track-area/arrangement";

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + static_cast<float>(r.w) / 2.0f, static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f}; }

// GU.11 (one hierarchy): Keys: its instrument group -> a Utility module -> an Envelope (a native engine: a leaf, its
// frame a face panel); a free LFO module wired into the Envelope's attack. Bass: its instrument group. The arrangement
// above, the Device view below (Keys). (Was BasicSynth -> LFO -> Auto Filter, the filter's frequency automated: the
// Envelope's attack is a knob of the same exponential, octave-default law.)
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  doc::TrackId keys, bass;
  doc::DeviceId inst, lfo, filter, util, rack;
  doc::WireId wire;

  Ui() {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks[0];
    bass = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Bass")).tracks[0];
    inst = doc::mainPath(*st.current(), track(keys)).blocks.at(0)->id;
    lfo = test_blocks::addModule(st, keys, "lfo", doc::ops::kOffPath);
    util = test_blocks::addModule(st, keys, "utility");
    // (after the Utility: appended to a path that holds only the instrument group, a native engine goes into it)
    filter = run(doc::ops::addMetaDevice(*st.current(), st.ids(), keys, "envelope")).devices[0];
    wire = run(doc::ops::connectTravelWire(*st.current(), st.ids(), lfo, "y", filter, "attack", 0.3)).wires[0];
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(330.0f);
    GridSpec g;
    g.mode = GridMode::Fixed;
    g.fixedExp = 2;
    ctx.setGrid(g);
    ctx.setSelectedTrack(keys);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
    d.tree().layoutNow();
    arr->setTimeView(0.0, 120.0 / static_cast<double>(kBar));
    d.tree().layoutNow();
    d.step();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  doc::ops::Edit run(const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(ctx.store().dispatch(e->command).ok());
    return *e;
  }
  const doc::Track& track(doc::TrackId id) const { return *doc::findTrackPtr(*ctx.doc(), id); }
  DeviceChainView& view() const {
    DeviceChainView* v = findDeviceChain(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  ArrangementAutomation& au() const { return arr->automation(); }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::AutomationLane* lane(const doc::ParamRef& r) const { return findAutomationLane(*ctx.doc(), r).lane; }
  // GU.1: the knob position of a device param's value in units (lanes and params store positions).
  double pos(doc::DeviceId dev, std::string_view param, double v) const {
    return knob_positions::positionOf(*ctx.doc(), dev, param, v, library());
  }
  // A lane on Keys with points ({bars, value}); a device param's values in units (stored as knob positions).
  void addLane(doc::ParamRef r, std::string name, std::vector<std::pair<double, float>> pts) {
    auto l = std::make_shared<doc::AutomationLane>();
    l->id = ctx.store().ids().lane();
    l->param = r;
    l->paramName = std::move(name);
    for (const auto& [bars, v] : pts)
      l->points.push_back({static_cast<Ticks>(bars * static_cast<double>(kBar)), v, 0.0f, ctx.store().ids().breakpoint()});
    knob_positions::lanePositions(*ctx.doc(), r, l->paramName, l->points, library());
    REQUIRE(ctx.store().dispatch(doc::AddAutomationLane{keys, l}).ok());
  }
  AutomationBand band(const doc::ParamRef& r) {
    d.tree().layoutNow();
    const auto b = au().band(keys, r, false);
    REQUIRE(b.has_value());
    return *b;
  }
  Menu* lastMenu() {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<Menu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
};

}  // namespace

TEST_CASE("device automation (views): the first click on a device param's line makes its lane and a point (one undo step)") {
  Ui u;
  const doc::ParamRef freq = doc::ParamRef::device(u.filter, "attack");
  u.au().showParam(u.keys, freq);
  const AutomationBand b = u.band(freq);
  CHECK(b.lane == nullptr);
  const size_t h0 = u.history();
  // The dashed line at the knob's value (the attack's default; GU.1: lanes are positions), at bar 2.
  REQUIRE(u.view().panelOf(u.filter) != nullptr);
  const double p1k = u.pos(u.filter, "attack", u.view().panelOf(u.filter)->paramValue("attack"));
  u.d.click({u.arr->xOfTick(2.0 * kBar), u.au().yOfValue(b, p1k)});
  REQUIRE(u.history() == h0 + 1);
  const doc::AutomationLane* l = u.lane(freq);
  REQUIRE(l != nullptr);
  CHECK(l->paramName == "attack");
  REQUIRE(l->points.size() == 1);
  CHECK(l->points[0].tick == 2 * kBar);
  CHECK(l->points[0].value == doctest::Approx(p1k).epsilon(0.02));
  CHECK(doc::laneHome(*u.ctx.doc(), freq)->id == u.keys);
  // A drag moves it in the knob's (exponential) scale, as one more step; the readout is the knob's text.
  const AutomationBand b2 = u.band(freq);
  const Vec2 at = *u.au().pointPos(b2, l->points[0].id);
  u.d.move(at);
  u.d.down(at);
  u.d.move({at.x, at.y - 5.0f});
  u.d.move({at.x, at.y - 10.0f});
  MESSAGE("readout: " << u.au().readout());
  CHECK(u.au().readout().find("s") != std::string::npos);
  u.d.up({at.x, at.y - 10.0f});
  CHECK(u.history() == h0 + 2);
  CHECK(u.lane(freq)->points[0].value > p1k);
  REQUIRE(u.ctx.undo());
  REQUIRE(u.ctx.undo());
  CHECK(u.lane(freq) == nullptr);
}

TEST_CASE("device automation (views): a knob's Show Automation opens its lane; automated knobs show the envelope and an LED") {
  Ui u;
  devices::DevicePanel* panel = u.view().panelOf(u.filter);
  REQUIRE(panel != nullptr);
  u.view().revealDevice(u.filter);
  u.d.tree().layoutNow();
  const std::optional<IRect> knob = panel->controlRect("attack");  // the face's control
  REQUIRE(knob.has_value());
  u.d.click(mid(*knob), mod::kNone, PointerButton::Right);
  Menu* m = u.lastMenu();
  REQUIRE(m != nullptr);
  int32_t show = -1;
  for (size_t i = 0; i < m->items().size(); ++i)
    if (m->items()[i].label == "Show Automation") show = static_cast<int32_t>(i);
  REQUIRE(show >= 0);
  const size_t h0 = u.history();
  m->choose(show);
  u.d.step();
  const std::vector<doc::ParamRef> shown = u.au().shownLanes(u.keys);
  CHECK(std::find(shown.begin(), shown.end(), doc::ParamRef::device(u.filter, "attack")) != shown.end());
  CHECK(u.history() == h0);  // showing a lane is view state
  // A right-click on the title bar (no knob) has no Show Automation.
  u.d.press("Escape");
  u.d.step();

  // Automated: the knob shows the envelope at the playhead and carries the LED; it moves with the playhead.
  u.addLane(doc::ParamRef::device(u.filter, "attack"), "attack", {{0.0, 0.01f}, {2.0, 2.0f}});
  CHECK(panel->paramValue("attack") == doctest::Approx(0.01).epsilon(1e-4));
  u.port.setPlayheadTick(kBar);
  u.view().applyAutomation();
  // GU.1: halfway in knob position (the lane stores positions), not halfway in seconds.
  const devices::ParamInfo* attack = panel->signature().findParam("attack");
  REQUIRE(attack != nullptr);
  const double halfway = devices::fromPosition(*attack, 0.5 * (u.pos(u.filter, "attack", 0.01) + u.pos(u.filter, "attack", 2.0)));
  CHECK(halfway < 0.5 * (0.01 + 2.0));
  CHECK(panel->paramValue("attack") == doctest::Approx(halfway).epsilon(1e-4));
  const auto marks = u.view().automationMarks(u.filter);
  REQUIRE(marks.size() == 1);
  CHECK(marks[0].first == "attack");
  CHECK_FALSE(marks[0].second);  // red: automated, not overridden
  CHECK_FALSE(u.view().automationPolling());  // stopped: no timer
  u.ctx.togglePlay();
  CHECK(u.view().automationPolling());
  u.ctx.togglePlay();
  u.port.setPlayheadTick(kBar);  // playing started from the start marker: back to bar 2
  u.view().applyAutomation();
  CHECK_FALSE(u.view().automationPolling());
  // A hand edit overrides it (Live): the knob keeps its own value; Re-enable brings the envelope back.
  REQUIRE(u.ctx.store().dispatch(doc::SetDeviceParam{u.filter, "attack", u.pos(u.filter, "attack", 0.3)}).ok());
  CHECK(u.ctx.automationOverridden(doc::ParamRef::device(u.filter, "attack")));
  CHECK(panel->paramValue("attack") == doctest::Approx(0.3).epsilon(1e-6));
  CHECK(u.view().automationMarks(u.filter).at(0).second);  // its LED grey
  u.ctx.reenableAutomation();
  CHECK_FALSE(u.view().automationMarks(u.filter).at(0).second);
  CHECK(panel->paramValue("attack") == doctest::Approx(halfway).epsilon(1e-4));
}

TEST_CASE("device automation (views): moving the device to another track takes its lane along; deleting it removes the lane") {
  Ui u;
  const doc::ParamRef freq = doc::ParamRef::device(u.filter, "attack");
  u.addLane(freq, "attack", {{0.0, 0.01f}, {2.0, 2.0f}});
  u.au().showLanes(u.keys, true);
  CHECK(findAutomationLane(*u.ctx.doc(), freq).owner == u.keys);
  const size_t h0 = u.history();
  u.run(doc::ops::moveDevice(*u.ctx.doc(), u.filter, u.bass, doc::kAppend));
  CHECK(u.history() == h0 + 1);
  CHECK(findAutomationLane(*u.ctx.doc(), freq).owner == u.bass);
  u.au().showLanes(u.bass, true);
  CHECK(u.au().shownLanes(u.bass).size() >= 1);
  REQUIRE(u.ctx.undo());
  u.ctx.setSelectedTrack(u.keys);
  u.d.tree().layoutNow();
  const doc::DeviceId one[1] = {u.filter};
  u.view().select(one);
  REQUIRE(u.view().deleteSelected());
  CHECK(u.history() == h0 + 1);
  CHECK(u.lane(freq) == nullptr);
  CHECK_FALSE(paramValid(*u.ctx.doc(), freq));
  REQUIRE(u.ctx.undo());
  CHECK(u.lane(freq) != nullptr);
}

TEST_CASE("render: device-automation matches the golden image") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize("device-automation", w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene("device-automation", {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/device-automation.png";
  auto ref = readPng(path);
  if (!ref) (void)writePng("device-automation.actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff diff;
  CHECK(pq::ui::test::matchesGolden("device-automation", *img, *ref, true, &diff));
  MESSAGE("device-automation: " << pq::ui::test::describe(diff));
}

// ---- TX.knob-curves: the Knob Curve submenu on a knob, the knob re-mapped, one undo step; meta faces; the golden ----

TEST_CASE("knob curves (views): a knob's Knob Curve submenu switches its curve, the knob re-maps, the value stays, one undo") {
  Ui u;
  devices::DevicePanel* panel = u.view().panelOf(u.filter);
  REQUIRE(panel != nullptr);
  u.view().revealDevice(u.filter);
  u.d.tree().layoutNow();
  const std::optional<IRect> knob = panel->controlRect("attack");  // the Envelope face's attack control
  REQUIRE(knob.has_value());
  const devices::ParamInfo* info = panel->signature().findParam("attack");
  REQUIRE(info != nullptr);
  const double value = panel->paramValue("attack");
  const double octavePos = devices::toPosition(*info, value);
  u.d.click(mid(*knob), mod::kNone, PointerButton::Right);
  Menu* m = u.lastMenu();
  REQUIRE(m != nullptr);
  const MenuItem* curves = nullptr;
  for (const MenuItem& it : m->items())
    if (it.label == "Knob Curve") curves = &it;
  REQUIRE(curves != nullptr);
  std::vector<std::string> labels;
  std::function<void()> chooseLinear;
  for (const MenuItem& it : curves->submenu) {
    labels.push_back(it.label + (it.checked ? " *" : ""));
    if (it.label == "Linear") chooseLinear = it.onChoose;
  }
  CHECK(labels == std::vector<std::string>{"Octave (default) *", "Exponential", "Logarithmic", "Linear"});
  REQUIRE(chooseLinear);
  const size_t h0 = u.history();
  u.d.press("Escape");
  chooseLinear();
  u.d.step();
  CHECK(u.history() == h0 + 1);
  // GU.1: the knob stays where it is; the value follows the new curve.
  devices::DevicePanel* after = u.view().panelOf(u.filter);
  REQUIRE(after != nullptr);
  const devices::ParamInfo* linear = after->signature().findParam("attack");
  REQUIRE(linear != nullptr);
  const double lo = devices::fromPosition(*linear, 0.0), hi = devices::fromPosition(*linear, 1.0);
  CHECK(after->paramValue("attack") == doctest::Approx(lo + octavePos * (hi - lo)).epsilon(1e-6));
  CHECK(after->paramValue("attack") != doctest::Approx(value));
  REQUIRE(u.ctx.undo());
  u.d.step();
  CHECK(u.view().panelOf(u.filter)->paramValue("attack") == doctest::Approx(value));
  // A rack's macro has none (no rack here: no menu).
  CHECK_FALSE(knobCurveMenu(u.ctx, u.rack, "macro0").has_value());
  // A meta module's continuous param has one (the Envelope's attack: exp0, listed as Octave, by default; GU.4: it was
  // the Phase accumulator's frequency bar). The VCA (a module now) has no knob of its own law any more: its value
  // block's knob takes the curve menu like any control (its curve is edited in its panel's curve editor too).
  const doc::DeviceId env = u.run(doc::ops::addMetaDevice(*u.ctx.doc(), u.ctx.store().ids(), u.bass, "envelope", 0)).devices[0];
  const std::optional<MenuItem> envMenu = knobCurveMenu(u.ctx, env, "attack");
  REQUIRE(envMenu.has_value());
  CHECK(envMenu->submenu.front().label == "Octave (default)");
  CHECK(knobCurveMenu(u.ctx, env, "sustain").has_value());  // linear by default
  const doc::DeviceId valueBlock =
      u.run(doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.bass, std::string(doc::kValueDevice), doc::DeviceKind::AudioEffect,
                                doc::ops::kOffPath))
          .devices[0];
  CHECK(knobCurveMenu(u.ctx, valueBlock, std::string(doc::kValueParam)).has_value());
}

TEST_CASE("render: device-knob-curves matches the golden image") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize("device-knob-curves", w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene("device-knob-curves", {}, w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/device-knob-curves.png";
  auto ref = readPng(path);
  if (!ref) (void)writePng("device-knob-curves.actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff diff;
  CHECK(pq::ui::test::matchesGolden("device-knob-curves", *img, *ref, true, &diff));
  MESSAGE("device-knob-curves: " << pq::ui::test::describe(diff));
}
