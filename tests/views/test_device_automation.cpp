// T4.18 device automation in the views, through the real shell: the lane chooser's device sections (device params,
// rack macros and selector, chain volume / pan, wire amounts), a device param's lane drawn in its knob's scale and named
// "<device>: <param>", the first click on its line creating the lane (with the param's name) and a breakpoint in one
// undo step, a knob's "Show Automation" opening its lane in the arrangement, automated knobs showing the envelope at
// the playhead with an LED (grey and their own value once overridden by hand; the 30 Hz timer only while playing),
// lanes surviving a device move in the Device view and leaving with a deleted device (one undo step each), and the
// golden `device-automation`.

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

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
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
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

// Keys: BasicSynth -> LFO -> Auto Filter -> a rack holding a Utility; the LFO wired into the filter's frequency. Bass:
// BasicSynth. The arrangement above, the Device view below (Keys).
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  doc::TrackId keys, bass;
  doc::DeviceId inst, lfo, filter, util, rack;
  doc::ChainId chain;
  doc::WireId wire;

  Ui() {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks[0];
    bass = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Bass")).tracks[0];
    inst = track(keys).devices.at(0)->id;
    lfo = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "lfo_device", doc::DeviceKind::AudioEffect)).devices[0];
    filter = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "auto_filter", doc::DeviceKind::AudioEffect)).devices[0];
    util = run(doc::ops::addDevice(*st.current(), st.ids(), keys, "utility", doc::DeviceKind::AudioEffect)).devices[0];
    rack = run(doc::ops::groupDevices(*st.current(), st.ids(), std::vector<doc::DeviceId>{util})).devices[0];
    chain = doc::findDevice(*ctx.doc(), rack).device->rack->chains.at(0)->id;
    wire = run(doc::ops::connectWire(*st.current(), st.ids(), lfo, "lfo", filter, "freq", 0.3)).wires[0];
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
  // A lane on Keys with points ({bars, value}).
  void addLane(doc::ParamRef r, std::string name, std::vector<std::pair<double, float>> pts) {
    auto l = std::make_shared<doc::AutomationLane>();
    l->id = ctx.store().ids().lane();
    l->param = r;
    l->paramName = std::move(name);
    for (const auto& [bars, v] : pts)
      l->points.push_back({static_cast<Ticks>(bars * static_cast<double>(kBar)), v, 0.0f, ctx.store().ids().breakpoint()});
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

TEST_CASE("device automation (views): the chooser has a section per device, rack chain and the wires; names and scales") {
  Ui u;
  const doc::Project& p = *u.ctx.doc();
  const std::vector<ParamChoice> choices = paramChoices(p, u.track(u.keys), &library());
  auto find = [&](const doc::ParamRef& r) -> const ParamChoice* {
    for (const ParamChoice& c : choices)
      if (c.param == r) return &c;
    return nullptr;
  };
  const ParamChoice* freq = find(doc::ParamRef::device(u.filter, "freq"));
  REQUIRE(freq != nullptr);
  CHECK(freq->group == "Auto Filter");
  CHECK(freq->label == "Frequency");
  const ParamChoice* macro = find(doc::ParamRef::device(u.rack, "macro1"));
  REQUIRE(macro != nullptr);
  CHECK(macro->group == "Audio Effect Rack");
  CHECK(find(doc::ParamRef::device(u.rack, "select")) != nullptr);
  const ParamChoice* vol = find(doc::ParamRef::chain(u.chain, doc::kChainVolume));
  REQUIRE(vol != nullptr);
  CHECK(vol->group == "Audio Effect Rack | Chain 1");
  CHECK(find(doc::ParamRef::chain(u.chain, doc::kChainPan)) != nullptr);
  CHECK(find(doc::ParamRef::device(u.util, "gain")) != nullptr);  // inside the rack
  CHECK(find(doc::ParamRef::wireAmount(u.wire)) != nullptr);
  CHECK(find(doc::ParamRef::device(u.inst, "bright")) != nullptr);
  // Bass offers none of Keys' devices.
  for (const ParamChoice& c : paramChoices(p, u.track(u.bass), &library()))
    if (c.param.kind == doc::ParamKind::DeviceParam) CHECK(c.param.owner != u.filter.value);
  // Names, the knob's scale (exponential frequency, dB gain), the knob's text.
  CHECK(paramName(p, doc::ParamRef::device(u.filter, "freq"), &library()) == "Auto Filter: Frequency");
  CHECK(paramName(p, doc::ParamRef::device(u.rack, "macro1"), &library()) == "Audio Effect Rack: Macro 1");
  CHECK(paramName(p, doc::ParamRef::chain(u.chain, doc::kChainPan), &library()) == "Chain 1: Pan");
  CHECK(paramName(p, doc::ParamRef::wireAmount(u.wire), &library()) == "LFO > Frequency: Amount");
  CHECK(paramDisplay(p, doc::ParamRef::device(u.filter, "freq"), &library()).scale == ParamDisplay::Scale::Log);
  CHECK(paramDisplay(p, doc::ParamRef::device(u.util, "gain"), &library()).scale == ParamDisplay::Scale::Db);
  CHECK(paramDisplay(p, doc::ParamRef::chain(u.chain, doc::kChainVolume), &library()).scale == ParamDisplay::Scale::Fader);
  CHECK(formatParamValue(p, doc::ParamRef::device(u.filter, "freq"), 1500.0, &library()) == "1.50 kHz");
  const ParamDisplay g = paramDisplay(p, doc::ParamRef::device(u.util, "gain"), &library());
  CHECK(displayNorm(g, displayValue(g, 0.37)) == doctest::Approx(0.37));
  CHECK(currentParamValue(p, doc::ParamRef::device(u.filter, "freq"), &library()) == doctest::Approx(1000.0));
}

TEST_CASE("device automation (views): the first click on a device param's line makes its lane and a point (one undo step)") {
  Ui u;
  const doc::ParamRef freq = doc::ParamRef::device(u.filter, "freq");
  u.au().showParam(u.keys, freq);
  const AutomationBand b = u.band(freq);
  CHECK(b.lane == nullptr);
  const size_t h0 = u.history();
  // The dashed line at the knob's value (1 kHz), at bar 2.
  u.d.click({u.arr->xOfTick(2.0 * kBar), u.au().yOfValue(b, 1000.0)});
  REQUIRE(u.history() == h0 + 1);
  const doc::AutomationLane* l = u.lane(freq);
  REQUIRE(l != nullptr);
  CHECK(l->paramName == "freq");
  REQUIRE(l->points.size() == 1);
  CHECK(l->points[0].tick == 2 * kBar);
  CHECK(l->points[0].value == doctest::Approx(1000.0).epsilon(0.02));
  CHECK(doc::laneHome(*u.ctx.doc(), freq)->id == u.keys);
  // A drag moves it in the knob's (exponential) scale, as one more step; the readout is the knob's text.
  const AutomationBand b2 = u.band(freq);
  const Vec2 at = *u.au().pointPos(b2, l->points[0].id);
  u.d.move(at);
  u.d.down(at);
  u.d.move({at.x, at.y - 5.0f});
  u.d.move({at.x, at.y - 10.0f});
  CHECK(u.au().readout().find("Hz") != std::string::npos);
  u.d.up({at.x, at.y - 10.0f});
  CHECK(u.history() == h0 + 2);
  CHECK(u.lane(freq)->points[0].value > 1000.0f);
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
  Widget* knob = panel->controlFor("freq");
  REQUIRE(knob != nullptr);
  u.d.click(mid(knob->rect()), mod::kNone, PointerButton::Right);
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
  CHECK(std::find(shown.begin(), shown.end(), doc::ParamRef::device(u.filter, "freq")) != shown.end());
  CHECK(u.history() == h0);  // showing a lane is view state
  // A right-click on the title bar (no knob) has no Show Automation.
  u.d.press("Escape");
  u.d.step();

  // Automated: the knob shows the envelope at the playhead and carries the LED; it moves with the playhead.
  u.addLane(doc::ParamRef::device(u.filter, "freq"), "freq", {{0.0, 200.0f}, {2.0, 5000.0f}});
  CHECK(panel->paramValue("freq") == doctest::Approx(200.0));
  u.port.setPlayheadTick(kBar);
  u.view().applyAutomation();
  CHECK(panel->paramValue("freq") == doctest::Approx(2600.0));
  auto* frame = u.view().frameOf(u.filter);
  REQUIRE(frame != nullptr);
  CHECK_FALSE(u.view().automationPolling());  // stopped: no timer
  u.ctx.togglePlay();
  CHECK(u.view().automationPolling());
  u.ctx.togglePlay();
  u.port.setPlayheadTick(kBar);  // playing started from the start marker: back to bar 2
  u.view().applyAutomation();
  CHECK_FALSE(u.view().automationPolling());
  // A hand edit overrides it (Live): the knob keeps its own value; Re-enable brings the envelope back.
  REQUIRE(u.ctx.store().dispatch(doc::SetDeviceParam{u.filter, "freq", 300.0}).ok());
  CHECK(u.ctx.automationOverridden(doc::ParamRef::device(u.filter, "freq")));
  CHECK(panel->paramValue("freq") == doctest::Approx(300.0));
  u.ctx.reenableAutomation();
  CHECK(panel->paramValue("freq") == doctest::Approx(2600.0));
}

TEST_CASE("device automation (views): moving the device to another track takes its lane along; deleting it removes the lane") {
  Ui u;
  const doc::ParamRef freq = doc::ParamRef::device(u.filter, "freq");
  u.addLane(freq, "freq", {{0.0, 200.0f}, {2.0, 5000.0f}});
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
