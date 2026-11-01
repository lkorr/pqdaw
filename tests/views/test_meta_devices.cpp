// TMM.10: meta modules as chain devices in the views — a chain meta device's Device-view panel IS its face (fitted to
// the view's height, interactive: drags are one undo step each, double-click resets, the info view names the control),
// automation LEDs on the face's controls, the device menu's Meta Modules (the kind follows the slot: a Wavetable after
// a Phase accumulator continues its voices), presets load with the slot's kind, hot-swap keeps wires, the generic
// knob's exp0 law (Q167 8), a panel description's "face" control. Goldens: device-meta-chain, project-meta-chain.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/core/time.h"
#include "pq/document/edit_ops.h"
#include "pq/document/library_files.h"
#include "pq/document/meta.h"
#include "pq/meta/phase_acc.h"
#include "pq/meta/registry.h"
#include "pq/meta/wavetable.h"
#include "pq/ui/controls.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/browser/library_load.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/devices/device_model.h"
#include "pq/views/devices/device_panel.h"
#include "pq/views/devices/panel_desc.h"
#include "pq/views/fake_transport.h"
#include "pq/views/meta/face_targets.h"
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

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + static_cast<float>(r.w) / 2.0f, static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f}; }

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
  CHECK(pq::ui::test::matchesGolden(name, *img, *ref, true, &d));
  MESSAGE(name << ": " << pq::ui::test::describe(d));
}

// The shell over buildMetaChainProject, the Device view showing Meta Lead.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  project_patch::MetaChainIds ids;

  Ui() {
    ctx.setDeviceLibrary(&library());
    REQUIRE(project_patch::buildMetaChainProject(ctx.store(), &ids).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(300.0f);
    ctx.setSelectedTrack(ids.lead);
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
  const doc::Device& dev(doc::DeviceId id) const {
    const doc::Device* x = doc::findDevice(*ctx.doc(), id).device;
    REQUIRE(x != nullptr);
    return *x;
  }
  double wtValue(const char* p) const {
    return doc::metaParamValue(dev(ids.wt), "", meta::wavetableType(), static_cast<size_t>(meta::wavetableType().paramIndex(p)));
  }
  double accValue(const char* p) const {
    return doc::metaParamValue(dev(ids.acc), "", meta::phaseAccType(), static_cast<size_t>(meta::phaseAccType().paramIndex(p)));
  }
  devices::DevicePanel& panel(doc::DeviceId id) {
    d.tree().layoutNow();
    devices::DevicePanel* p = view().panelOf(id);
    REQUIRE(p != nullptr);
    return *p;
  }
  Widget& faceControl(doc::DeviceId id) {
    Widget* w = panel(id).control("face");
    REQUIRE(w != nullptr);
    return *w;
  }
  // A face-local point (world units) of device `id`'s face, in device px (the face is fitted into its control).
  Vec2 at(doc::DeviceId id, float x, float y, float worldW, float worldH) {
    const IRect r = faceControl(id).rect();
    const float s = std::min(static_cast<float>(r.w) / worldW, static_cast<float>(r.h) / worldH);
    const float ox = std::round(static_cast<float>(r.x) + (static_cast<float>(r.w) - worldW * s) * 0.5f);
    const float oy = std::round(static_cast<float>(r.y) + (static_cast<float>(r.h) - worldH * s) * 0.5f);
    return {ox + x * s, oy + y * s};
  }
  Vec2 wt(float x, float y) { return at(ids.wt, x, y, 204.0f, 248.0f); }
  Vec2 acc(float x, float y) { return at(ids.acc, x, y, 120.0f, 120.0f); }
  Menu* lastMenu() {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<Menu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
};

}  // namespace

TEST_CASE("meta devices (views): a chain meta device's panel is its face, fitted to the Device view's height") {
  Ui u;
  devices::DevicePanel& wp = u.panel(u.ids.wt);
  CHECK(wp.desc().fitFace);
  CHECK(wp.desc().generated);
  CHECK(wp.title() == "Wavetable");
  devices::DevicePanel& ap = u.panel(u.ids.acc);
  CHECK(ap.desc().fitFace);
  CHECK(ap.title() == "Phase Accumulator");
  // The face fills the body: its control's height is the panel's minus the title bar and the body's padding, and the
  // frame's width follows the face's aspect at that height.
  const IRect fr = u.faceControl(u.ids.wt).rect();
  const IRect pr = wp.rect();
  const float s = u.d.tree().scale();
  CHECK(std::abs(static_cast<float>(fr.h) - (static_cast<float>(pr.h) - (20.0f + 12.0f) * s)) <= 2.0f);
  const float faceScale = static_cast<float>(fr.h) / (248.0f * s);
  CHECK(faceScale > 0.7f);
  CHECK(faceScale <= devices::DevicePanel::kFaceMaxScale + 1e-3f);
  CHECK(std::abs(static_cast<float>(fr.w) - 204.0f * faceScale * s) <= 3.0f);
  // Auto Filter after them keeps its stock panel.
  CHECK_FALSE(u.panel(doc::findTrackPtr(*u.ctx.doc(), u.ids.lead)->devices.at(2)->id).desc().fitFace);
  // Another Device view height scales the face with it (the frame's width follows).
  dynamic_cast<Splitter&>(*u.d.find("main/body")).setPosition(200.0f);
  u.d.tree().layoutNow();
  const IRect fr2 = u.faceControl(u.ids.wt).rect();
  CHECK(fr2.h != fr.h);
  // The face fits whole (width at least its aspect's; a narrow face keeps the panel's minimum width for its title).
  CHECK(static_cast<float>(fr2.w) >= 204.0f * static_cast<float>(fr2.h) / 248.0f - 3.0f);
  CHECK(static_cast<float>(u.panel(u.ids.acc).rect().w) >= devices::DevicePanel::kFaceMinWidth * s - 1.0f);
}

TEST_CASE("meta devices (views): the face's input socket shows where the chain is wired (TMM.10-fix1)") {
  Ui u;
  CHECK(u.panel(u.ids.wt).signature().audioIn == "x");  // the Phase accumulator's voices into its phase
  CHECK(u.panel(u.ids.acc).signature().audioIn.empty());  // first: the note source, read inside (no audio socket)
  REQUIRE(u.view().setChainInput(u.ids.wt, "pos"));
  CHECK(u.dev(u.ids.wt).chainInput == "pos");
  CHECK(u.panel(u.ids.wt).signature().audioIn == "pos");
  const size_t h0 = u.history();
  REQUIRE(u.view().setChainInput(u.ids.wt, std::string(doc::kChainUnplugged)));
  CHECK(u.history() == h0 + 1);
  CHECK(u.panel(u.ids.wt).signature().audioIn.empty());  // unplugged: no socket, no wire
  REQUIRE(u.view().setChainInput(u.ids.wt, "x"));
  CHECK(u.dev(u.ids.wt).chainInput.empty());  // the type's own input: the default
}

TEST_CASE("meta devices (views): face drags in the Device view are one undo step each; double-click resets") {
  Ui u;
  const double s = u.d.tree().scale();
  // Wavetable fold knob: up 30 px (a full travel is the knob's on-screen extent, never less than 150 logical px).
  size_t h0 = u.history();
  Vec2 a = u.wt(38.0f, 212.0f);
  u.d.drag(a, {a.x, a.y - 30.0f}, 6);
  CHECK(u.history() == h0 + 1);
  CHECK(u.wtValue("fold") > 0.0);
  CHECK(u.wtValue("fold") <= 30.0 / (150.0 * s) + 1e-6);
  // Position slider: up 40 px.
  h0 = u.history();
  const double pos0 = u.wtValue("position");
  a = u.wt(178.0f, 172.0f - 94.0f * 0.2f);
  u.d.drag(a, {a.x, a.y - 40.0f}, 6);
  CHECK(u.history() == h0 + 1);
  CHECK(u.wtValue("position") > pos0);
  // Phase accumulator amplitude arc (right side): up 20 px.
  h0 = u.history();
  a = u.acc(60.0f + 43.0f, 64.0f);
  u.d.drag(a, {a.x, a.y - 20.0f}, 6);
  CHECK(u.history() == h0 + 1);
  CHECK(u.accValue("amp") > 1.0);
  // The panel follows (the face reads the document): nothing was rebuilt, the frames are the same widgets.
  devices::DevicePanel* before = u.view().panelOf(u.ids.wt);
  // Double-click the fold knob: reset, one step; undo brings it back.
  h0 = u.history();
  u.d.doubleClick(u.wt(38.0f, 212.0f));
  CHECK(u.history() == h0 + 1);
  CHECK(u.wtValue("fold") == 0.0);
  REQUIRE(u.ctx.undo());
  CHECK(u.wtValue("fold") > 0.0);
  CHECK(u.view().panelOf(u.ids.wt) == before);
  // Hovering a control names it in the info view.
  u.d.move(u.wt(89.0f, 212.0f));
  u.d.step();
  const InfoText info = u.faceControl(u.ids.wt).info();
  CHECK(std::string(info.title).find("Warp") != std::string::npos);
}

TEST_CASE("meta devices (views): automation LEDs and Show Automation sit on the face's controls") {
  Ui u;
  devices::DevicePanel& ap = u.panel(u.ids.acc);
  const std::optional<IRect> freq = ap.controlRect("freq");
  REQUIRE(freq.has_value());
  const IRect face = u.faceControl(u.ids.acc).rect();
  CHECK(freq->x >= face.x - 1);
  CHECK(freq->right() <= face.right() + 1);
  CHECK(freq->x + freq->w / 2 < face.x + face.w / 2);  // the frequency arc is on the left
  // An automation lane on the Phase accumulator's frequency: its LED.
  auto l = std::make_shared<doc::AutomationLane>();
  l->id = u.ctx.store().ids().lane();
  l->param = doc::ParamRef::device(u.ids.acc, "freq");
  l->paramName = "freq";
  l->points.push_back({0, 0.0f, 0.0f, u.ctx.store().ids().breakpoint()});
  l->points.push_back({4 * kPpq, 220.0f, 0.0f, u.ctx.store().ids().breakpoint()});
  REQUIRE(u.ctx.store().dispatch(doc::AddAutomationLane{u.ids.lead, l}).ok());
  u.d.tree().layoutNow();
  const auto marks = u.view().automationMarks(u.ids.acc);
  REQUIRE(marks.size() == 1);
  CHECK(marks[0].first == "freq");
  // A right-click on the arc offers its lane.
  u.d.click(mid(*freq), mod::kNone, PointerButton::Right);
  u.d.step();
  Menu* m = u.lastMenu();
  REQUIRE(m != nullptr);
  bool show = false;
  for (const MenuItem& it : m->items()) show = show || it.label == "Show Automation";
  CHECK(show);
}

TEST_CASE("meta devices (views): the device menu's Meta Modules — the kind follows the slot") {
  Ui u;
  // After the Phase accumulator: a Wavetable continues its voices.
  const doc::DeviceId one[1] = {u.ids.acc};
  u.view().select(one);
  std::vector<MenuItem> menu = u.view().deviceMenu();
  bool header = false;
  int32_t wtItem = -1;
  for (size_t i = 0; i < menu.size(); ++i) {
    header = header || menu[i].label == "Meta Modules";
    if (header && menu[i].label == "   Wavetable") wtItem = static_cast<int32_t>(i);
  }
  CHECK(header);
  REQUIRE(wtItem >= 0);
  CHECK(menu[static_cast<size_t>(wtItem)].enabled);
  const size_t h0 = u.history();
  menu[static_cast<size_t>(wtItem)].onChoose();
  CHECK(u.history() == h0 + 1);
  const doc::Track& t = *doc::findTrackPtr(*u.ctx.doc(), u.ids.lead);
  REQUIRE(t.devices.size() == 4);
  CHECK(t.devices[1]->patch == "meta:wavetable");
  CHECK(t.devices[1]->kind == doc::DeviceKind::AudioEffect);
  {  // TMM.10-fix1: the Phase accumulator's voice signal reaches both Wavetables (doc::chainStages)
    const std::vector<doc::ChainStage> st = doc::chainStages(t.devices, true);
    REQUIRE(st.size() >= 3);
    CHECK(st[1].in == doc::ChainSignal::Voices);
    CHECK(st[2].in == doc::ChainSignal::Voices);
  }
  // Every meta module fits every slot (no instrument-only or effect-only types).
  for (const MenuItem& m : menu)
    for (const meta::Type* x : metaModuleTypes())
      if (m.label == "   " + std::string(x->name)) CHECK(m.enabled);
  // Its panel is a face too.
  CHECK(u.panel(t.devices[1]->id).desc().fitFace);
  // The browser and the menus name it by its type.
  CHECK(deviceDisplayName(u.ctx, "meta:phase_acc") == "Phase Accumulator");
  const std::vector<const meta::Type*> types = metaModuleTypes();
  CHECK(std::find(types.begin(), types.end(), &meta::wavetableType()) != types.end());
  CHECK(std::find(types.begin(), types.end(), &meta::phaseAccType()) != types.end());
}

TEST_CASE("meta devices (views): a preset loads with its slot's kind (state kept); hot-swap keeps the wires") {
  Ui u;
  // A preset of the Lead's Wavetable (an audio-effect kind: it continues the voices).
  doc::Device src = u.dev(u.ids.wt);
  auto text = doc::serializePreset(src, "Lead Table");
  REQUIRE(text.ok());
  const std::string dir = pq::views::test::tempDir("tmm10_presets");
  const std::string path = dir + "/Lead Table.pqpreset";
  std::ofstream(std::filesystem::path(dir) / "Lead Table.pqpreset", std::ios::binary) << *text;
  // Loaded first on Meta Bass: it becomes the instrument there, with the same state.
  auto id = loadDeviceItem(u.ctx, path, DeviceSlot{u.ids.bass, 0, {}});
  REQUIRE_MESSAGE(id.ok(), (id.ok() ? "" : id.error().message));
  const doc::Device& loaded = u.dev(*id);
  CHECK(loaded.kind == doc::DeviceKind::Instrument);
  CHECK(loaded.params == src.params);
  REQUIRE(loaded.meta);
  CHECK(*loaded.meta == *src.meta);
  // Hot-swap the Lead's Phase accumulator for a fresh one: a wire into its frequency is kept.
  const doc::DeviceId lfo = [&] {
    auto e = doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.ids.bass, "lfo_device", doc::DeviceKind::AudioEffect);
    REQUIRE(e.ok());
    REQUIRE(u.ctx.store().dispatch(e->command).ok());
    return e->devices.at(0);
  }();
  auto w = doc::ops::connectWire(*u.ctx.doc(), u.ctx.store().ids(), lfo, "lfo", u.ids.acc, "freq");
  REQUIRE_MESSAGE(w.ok(), (w.ok() ? "" : w.error().message));
  REQUIRE(u.ctx.store().dispatch(w->command).ok());
  REQUIRE(hotSwapDevice(u.ctx, u.ids.acc, catalogItemPath("meta:phase_acc")).ok());
  const doc::Device& swapped = u.dev(u.ids.acc);
  CHECK(swapped.patch == "meta:phase_acc");
  CHECK(swapped.kind == doc::DeviceKind::Instrument);
  bool kept = false;
  for (const doc::Wire& x : doc::wiresOf(*u.ctx.doc())) kept = kept || (x.to == u.ids.acc && x.param == "freq");
  CHECK(kept);
}

TEST_CASE("meta devices (views): the generic knob follows exp0 (Q167 8); a panel description's face control parses") {
  const meta::Type& t = meta::phaseAccType();
  auto pre = meta::prelude();
  const meta::Prelude::Entry* e = pre->find(&t);
  REQUIRE(e != nullptr);
  REQUIRE(e->notes.has_value());
  const devices::DeviceSignature sig = devices::signatureOf(*e->notes);
  const devices::ParamInfo* f = sig.findParam("freq");
  REQUIRE(f != nullptr);
  CHECK(f->exp0 == doctest::Approx(meta::kPhaseAccCurveK));
  for (const double v : {0.0, 1.0, 100.0, 1000.0, 20000.0}) {
    CHECK(devices::toPosition(*f, v) == doctest::Approx(meta::phaseAccTravelFromFreq(v)).epsilon(1e-9));
    CHECK(devices::fromPosition(*f, meta::phaseAccTravelFromFreq(v)) == doctest::Approx(v).epsilon(1e-9));
  }
  CHECK(devices::toPosition(*f, 0.0) == 0.0);
  // {"type": "face", "meta": "<path>"}: parsed with its path; a path that is not one is refused.
  const std::string ok = R"({"format": "pqdaw-panel 1", "patch": ")" + sig.patch +
                         R"(", "title": "T", "size": [120, 120], "controls": [{"type": "face", "meta": "", "rect": [0, 0, 120, 120]},
                         {"type": "face", "meta": "opA.wt", "rect": [0, 0, 60, 60]}]})";
  auto p = devices::parsePanelDesc(ok, sig);
  REQUIRE_MESSAGE(p.ok(), (p.ok() ? "" : p.error().message));
  REQUIRE(p->controls.size() == 2);
  CHECK(p->controls[0].kind == devices::ControlKind::Face);
  CHECK(p->controls[1].meta == "opA.wt");
  CHECK(p->controls[0].id != p->controls[1].id);
  const std::string bad = R"({"format": "pqdaw-panel 1", "patch": ")" + sig.patch +
                          R"(", "title": "T", "size": [120, 120], "controls": [{"type": "face", "meta": "a..b", "rect": [0, 0, 1, 1]}]})";
  CHECK_FALSE(devices::parsePanelDesc(bad, sig).ok());
}

TEST_CASE("render: device-meta-chain matches the golden image") { checkScene("device-meta-chain"); }

TEST_CASE("render: project-meta-chain matches the golden image") { checkScene("project-meta-chain"); }
