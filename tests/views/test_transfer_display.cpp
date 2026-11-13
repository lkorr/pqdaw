// DY.3 (docs/plans/DY.md "DY.3"; transfer_element.cpp, engine_elements.cpp Meter): the `transfer` display and the
// reduction meter.
//   curve      the drawn curve is the group's GroupTransfer: the Saturator's Hard Curve group in dB (units linear), a
//              hand-built gain-reduction group as output = input − reduction (units db, output reduction)
//   no curve   a group holding z1 between In and Out draws no curve ("no static curve")
//   counts     paints and document edits the curve does not read never re-evaluate it; a knob it reads does, once
//   goldens    (2x) transfer-saturator (the Hard Curve group, a live level dot), transfer-reduction (a gain computer
//              at −24 dB, 4:1, its dot, beside a reduction meter at 9 / 6 dB over 24 dB)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/document/panel.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/panel/group_responses.h"
#include "pq/views/panel/transfer_display.h"
#include "pq/views/view_context.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

const doc::DeviceLibrary& devLib() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

void checkImage(const std::string& name, const Image& img) {
  auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png");
  if (!ref) {
    (void)writePng(name + ".actual.png", img);
    FAIL_CHECK("no golden " << name << ": wrote " << name << ".actual.png");
    return;
  }
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(name, img, *ref, true, &d));
  MESSAGE(name << ": " << pq::ui::test::describe(d));
}

doc::ops::Edit run(doc::DocumentStore& st, const doc::ops::EditResult& e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(st.dispatch(e->command).ok());
  return *e;
}
void cmd(doc::DocumentStore& st, doc::Command c) {
  const Status s = st.dispatch(std::move(c));
  REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
}

doc::PanelElement element(uint32_t id, const char* kind, float x, float y, float w, float h, doc::Binding b) {
  doc::PanelElement e;
  e.id = id;
  e.kind = kind;
  e.x = x, e.y = y, e.w = w, e.h = h;
  e.bindings = {std::move(b)};
  return e;
}
doc::PanelElement plate(float w, float h) {
  doc::PanelElement e = element(1, "shape", 0, 0, w, h, {});
  e.bindings.clear();
  e.setField(doc::styleNumber("plate", 1.0));
  e.setField(doc::styleText("fill", "none"));
  e.setField(doc::styleText("stroke", "none"));
  e.setField(doc::styleNumber("radius", 6.0));
  return e;
}

// A track with a group whose panel is replaced by `panel` (set by the caller), shown alone at 2x.
struct Scene {
  FakeTransport port;
  ViewContext ctx{port};
  doc::TrackId track;
  doc::DeviceId shown;  // the group whose panel is shown
  std::unique_ptr<TestDriver> d;
  panel::PanelHost* host = nullptr;

  Scene() {
    ctx.setDeviceLibrary(&devLib());
    track = run(st(), doc::ops::createTrack(*ctx.doc(), st().ids(), doc::TrackKind::Audio, {}, "Audio")).tracks.at(0);
  }
  ~Scene() {
    if (d) d->tree().setRoot(std::make_unique<Widget>());
  }
  doc::DocumentStore& st() { return ctx.store(); }
  doc::DeviceId block(std::string patch, doc::DeviceId group) {
    const doc::DeviceId b = run(st(), doc::ops::addDevice(*ctx.doc(), st().ids(), track, std::move(patch), doc::DeviceKind::AudioEffect, doc::ops::kOffPath)).devices.at(0);
    cmd(st(), doc::SetDeviceGroup{b, group});
    return b;
  }
  void wire(doc::DeviceId from, std::string out, doc::DeviceId to, std::string param) {
    (void)run(st(), doc::ops::connectWire(*ctx.doc(), st().ids(), from, std::move(out), to, std::move(param)));
  }
  void setValue(doc::DeviceId v, double units, dsp::Curve curve) {
    cmd(st(), doc::SetParamCurve{v, std::string(doc::kValueParam), curve});
    cmd(st(), doc::SetDeviceParam{v, std::string(doc::kValueParam), dsp::curvePosition(curve, dsp::curveOutput(curve, units), 0.5)});
  }
  void setPanel(doc::DeviceId group, doc::Panel p) {
    shown = group;
    cmd(st(), doc::SetGroupPanel{group, std::make_shared<const doc::Panel>(std::move(p))});
  }
  void show(float scale = 2.0f) {
    const doc::Panel* p = doc::findDevice(*ctx.doc(), shown).device->panel.get();
    REQUIRE(p != nullptr);
    d = std::make_unique<TestDriver>(static_cast<uint32_t>(p->width * scale), static_cast<uint32_t>(p->height * scale), scale);
    auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, shown),
                                                        panel::makeDocPanelTarget(ctx, shown, [this]() -> Widget* { return host; }));
    host = &d->setRoot<panel::PanelHost>("panel", std::move(face));
    d->tree().layoutNow();
    REQUIRE(d->enableGpu().ok());  // (the theme: a panel paints, and its displays compute their curves, only with one)
    d->step();
  }
  panel::Element& element(std::string_view kind) {
    panel::GroupPanelFace& f = *host->panelFace();
    (void)f.sync();
    for (size_t i = 0; i < f.elementCount(); ++i)
      if (f.element(i).desc().kind == kind) return f.element(i);
    FAIL("no element " << kind);
    return f.element(0);
  }
  void repaint() {
    host->notifyChanged();
    d->step();
  }
  Image render() {
    REQUIRE(d->enableGpu().ok());
    host->notifyChanged();
    d->advance(0.1);
    auto img = d->render();
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    return std::move(*img);
  }
};

// A gain computer: In (level dB) → sub ← Threshold → max ← 0 → mul ← Slope → Out (reduction dB); its panel: the
// transfer display (units db, output reduction) and a reduction meter on its Out.
struct Reduction {
  doc::DeviceId group, threshold, slope;
};
Reduction reductionScene(Scene& s) {
  Reduction r;
  r.group = run(s.st(), doc::ops::addDevice(*s.ctx.doc(), s.st().ids(), s.track, std::string(doc::kGroupDevice), doc::DeviceKind::AudioEffect)).devices.at(0);
  const doc::DeviceId sub = s.block("atom:sub", r.group), mx = s.block("atom:max", r.group), mul = s.block("atom:mul", r.group);
  r.threshold = s.block(std::string(doc::kValueDevice), r.group);
  const doc::DeviceId zero = s.block(std::string(doc::kValueDevice), r.group);
  r.slope = s.block(std::string(doc::kValueDevice), r.group);
  s.setValue(r.threshold, -24.0, dsp::curvePreset(dsp::CurvePreset::Linear, -60.0, 0.0));
  s.setValue(zero, 0.0, dsp::curvePreset(dsp::CurvePreset::Linear, -1.0, 1.0));
  s.setValue(r.slope, 0.75, dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 1.0));  // 4:1
  s.wire(r.group, std::string(doc::kGroupInPort), sub, "a");
  s.wire(r.threshold, "y", sub, "b");
  s.wire(sub, "out", mx, "a");
  s.wire(zero, "y", mx, "b");
  s.wire(mx, "out", mul, "a");
  s.wire(r.slope, "y", mul, "b");
  s.wire(mul, "out", r.group, std::string(doc::kGroupOutPort));
  doc::Panel p;
  p.width = 168.0f;
  p.height = 132.0f;
  p.elements.push_back(plate(p.width, p.height));
  doc::PanelElement t = element(2, "transfer", 6, 6, 120, 120, doc::signalBinding(r.group, std::string(doc::kGroupInPort)));
  t.setField(doc::styleText("units", "db"));
  t.setField(doc::styleText("output", "reduction"));
  p.elements.push_back(std::move(t));
  doc::PanelElement m = element(3, "meter", 134, 6, 28, 120, doc::signalBinding(r.group, "y"));
  m.setField(doc::styleText("mode", "reduction"));
  m.setField(doc::styleNumber("range", 24.0));
  p.elements.push_back(std::move(m));
  s.setPanel(r.group, std::move(p));
  return r;
}

// The Saturator with its Hard Curve group's panel showing the curve (units linear).
doc::DeviceId saturatorScene(Scene& s) {
  const doc::DeviceId sat = test_blocks::addModule(s.st(), s.track, "saturator");
  const doc::DeviceId hard = test_blocks::member(*s.ctx.doc(), sat, "Hard Curve");
  REQUIRE(hard.valid());
  doc::Panel p;
  p.width = 132.0f;
  p.height = 132.0f;
  p.elements.push_back(plate(p.width, p.height));
  p.elements.push_back(element(2, "transfer", 6, 6, 120, 120, doc::signalBinding(hard, std::string(doc::kGroupInPort))));
  s.setPanel(hard, std::move(p));
  return hard;
}

// Input dB values on the display's grid (128 points over the default −60 … +6 dB: between them it draws lines).
std::vector<double> gridDb() {
  std::vector<double> x;
  for (const int k : {0, 23, 70, 74, 100, 116, 120, 127}) x.push_back(-60.0 + 66.0 * k / 127.0);
  return x;
}

}  // namespace

TEST_CASE("DY.3 transfer: the Hard Curve group's curve is drawn in dB; a gain computer's as input − reduction") {
  {
    Scene s;
    saturatorScene(s);
    s.show();
    const panel::Element& e = s.element("transfer");
    for (const double x : gridDb()) {
      const std::optional<double> y = panel::test::transferDrawn(e, x);
      REQUIRE(y.has_value());
      const double v = std::pow(10.0, x / 20.0);
      CHECK(*y == doctest::Approx(20.0 * std::log10(v / std::pow(1.0 + v * v * v * v, 0.25))).epsilon(1e-4));
    }
  }
  {
    Scene s;
    reductionScene(s);
    s.show();
    const panel::Element& e = s.element("transfer");
    for (const double x : gridDb()) {
      const std::optional<double> y = panel::test::transferDrawn(e, x);
      REQUIRE(y.has_value());
      CHECK(*y == doctest::Approx(x - 0.75 * std::max(0.0, x + 24.0)).epsilon(1e-4));
    }
  }
}

TEST_CASE("DY.3 transfer: a group holding z1 between In and Out draws no static curve") {
  Scene s;
  const doc::DeviceId g = run(s.st(), doc::ops::addDevice(*s.ctx.doc(), s.st().ids(), s.track, std::string(doc::kGroupDevice), doc::DeviceKind::AudioEffect)).devices.at(0);
  const doc::DeviceId add = s.block("atom:add", g), z1 = s.block("atom:z1", g);
  s.wire(g, std::string(doc::kGroupInPort), add, "a");
  s.wire(g, std::string(doc::kGroupInPort), z1, "x");
  s.wire(z1, "out", add, "b");
  s.wire(add, "out", g, std::string(doc::kGroupOutPort));
  doc::Panel p;
  p.width = 132.0f;
  p.height = 132.0f;
  p.elements.push_back(plate(p.width, p.height));
  p.elements.push_back(element(2, "transfer", 6, 6, 120, 120, doc::signalBinding(g, std::string(doc::kGroupInPort))));
  s.setPanel(g, std::move(p));
  s.show();
  const std::optional<double> y = panel::test::transferDrawn(s.element("transfer"), -12.0);
  REQUIRE(y.has_value());
  CHECK(std::isnan(*y));
  CHECK(s.ctx.groupResponses().transfer(g) != nullptr);  // (prepared: Unavailable, not an error)
}

TEST_CASE("DY.3 transfer: repaints and edits the curve does not read never re-evaluate it; a knob it reads does, once") {
  Scene s;
  const Reduction r = reductionScene(s);
  s.show();
  GroupResponses& gr = s.ctx.groupResponses();
  const uint64_t first = gr.transferEvaluations();
  CHECK(first == 1);
  for (int i = 0; i < 3; ++i) s.repaint();
  CHECK(gr.transferEvaluations() == first);
  cmd(s.st(), doc::RenameDevice{r.group, "Gain Computer"});  // (meta: no value the curve reads)
  s.repaint();
  CHECK(gr.transferEvaluations() == first);
  s.setValue(r.slope, 0.5, dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 1.0));  // (a curve edit: one structural revision)
  s.repaint();
  s.repaint();
  const uint64_t afterCurve = gr.transferEvaluations();
  CHECK(afterCurve == first + 1);
  const dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 1.0);
  cmd(s.st(), doc::SetDeviceParam{r.slope, std::string(doc::kValueParam), dsp::curvePosition(c, 0.25, 0.5)});  // a knob turn
  s.repaint();
  s.repaint();
  CHECK(gr.transferEvaluations() == afterCurve + 1);
  const double x = gridDb()[5];  // (≈ +0.3 dB)
  const std::optional<double> y = panel::test::transferDrawn(s.element("transfer"), x);
  REQUIRE(y.has_value());
  CHECK(*y == doctest::Approx(x - 0.25 * (x + 24.0)).epsilon(1e-4));
}

TEST_CASE("render: transfer-saturator, transfer-reduction (DY.3, 2x)") {
  {
    Scene s;
    saturatorScene(s);
    s.show();
    REQUIRE(panel::test::showTransferLevel(s.element("transfer"), -6.0));
    checkImage("transfer-saturator", s.render());
  }
  {
    Scene s;
    reductionScene(s);
    s.show();
    REQUIRE(panel::test::showTransferLevel(s.element("transfer"), -12.0));
    REQUIRE(panel::test::showMeter(s.element("meter"), 9.0f, 6.0f));
    checkImage("transfer-reduction", s.render());
  }
}
