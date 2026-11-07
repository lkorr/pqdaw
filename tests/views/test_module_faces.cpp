// GU.5b (docs/plans/GU.md "Phases" 5a): the modules look as before, an opened group is a circuit.
//   - CLOSED: the Phase Accumulator and the VCA spawn closed, drawn as their panels — the old faces rebuilt from the
//     registered rich elements (pq/views/panel/rich_elements.cpp: dial, arc, orbit): goldens module-phase-acc-panel
//     (with voices on the orbit through a voice reader) and module-vca-panel (2x).
//   - The VCA's MARKERS are points of its curve (dsp::CurvePreset::Vca splits its outers at ±10 / ±100): a marker's tick
//     dragged round the dial through real dispatch moves that point, its opposite mirrored, in one undo step and no
//     recompile (the curve's class kept); the chevron opens the floating popup and a preset chip sets the curve.
//   - The PA's frequency arc dragged up through real dispatch: one undo step, no recompile.
//   - OPEN: a double-click on a closed module's rim opens it (and on the capsule's title strip closes it again), through
//     real dispatch; opened, its blocks are laid out as a circuit (In left, Out right, every block between, atoms drawn
//     as their circuit symbols, value blocks as knobs, the wires among them orthogonal traces): golden
//     project-module-circuits (both modules open).
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/element.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::project_patch;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

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

// A MIDI track holding a Phase Accumulator module and a VCA module (doc::modules.h), pasted as the node menu does.
struct ModuleDoc {
  doc::TrackId track;
  doc::PhaseAccumulatorRoles pa;
  doc::VcaRoles vca;
};
ModuleDoc buildModules(doc::DocumentStore& st) {
  ModuleDoc m;
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(st.dispatch(e->command).ok());
    return *e;
  };
  m.track = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Modules")).tracks.at(0);
  {
    doc::IdSource local;
    doc::PhaseAccumulatorRoles l;
    const doc::ops::Edit e = run(doc::ops::pasteBlocks(*st.current(), st.ids(), m.track, doc::phaseAccumulatorBlocks(local, {}, &l)));
    auto id = [&](doc::DeviceId x) { return x.valid() ? e.devices.at(x.value - 1) : doc::DeviceId{}; };
    m.pa = doc::PhaseAccumulatorRoles{id(l.group), id(l.freq), id(l.phase), id(l.amp), id(l.pitch), id(l.ref), id(l.sum),
                                      id(l.ratio), id(l.pitched), id(l.phasor), id(l.addPhase), id(l.pm), id(l.frac), id(l.out)};
  }
  {
    doc::IdSource local;
    doc::VcaRoles l;
    const doc::ops::Edit e = run(doc::ops::pasteBlocks(*st.current(), st.ids(), m.track, doc::vcaBlocks(local, {}, &l)));
    m.vca = doc::VcaRoles{e.devices.at(l.group.value - 1), e.devices.at(l.value.value - 1), e.devices.at(l.mul.value - 1)};
  }
  return m;
}

// Three voices of a chord on the Phase Accumulator's orbit (110, 220 and 330 Hz at 48 kHz), each with a short trail.
class ChordVoices final : public VoiceTapReader {
public:
  uint32_t read(std::span<FaceVoice> out) override {
    constexpr std::array<double, 3> kHz = {110.0, 220.0, 330.0};
    constexpr std::array<float, 3> kPhase = {0.12f, 0.47f, 0.81f};
    uint32_t n = 0;
    for (size_t v = 0; v < kHz.size() && n < out.size(); ++v) {
      FaceVoice& f = out[n++];
      f.slot = static_cast<uint32_t>(v);
      f.rate = 48000.0f;
      f.increment = static_cast<float>(kHz[v] / 48000.0);
      f.sampleCount = 8;
      for (uint32_t k = 0; k < 8; ++k) f.samples[k] = kPhase[v] - 0.006f * static_cast<float>(7 - k);
      f.phase = kPhase[v];
    }
    return n;
  }
  uint64_t serial() const noexcept override { return 1; }
  bool live() const noexcept override { return false; }
};

// The document target with a voice reader for every Signal binding (as GU.7's lowering tap will give one).
class VoicedTarget final : public panel::PanelTarget {
public:
  VoicedTarget(std::unique_ptr<panel::PanelTarget> inner, VoiceTapReader* voices) : in_(std::move(inner)), voices_(voices) {}
  const panel::BoundInfo& info(const doc::Binding& b) const override { return in_->info(b); }
  double position(const doc::Binding& b) const override { return in_->position(b); }
  std::string format(const doc::Binding& b, double p) const override { return in_->format(b, p); }
  std::optional<double> parse(const doc::Binding& b, std::string_view t) const override { return in_->parse(b, t); }
  void begin(const doc::Binding& b, const char* l) override { in_->begin(b, l); }
  void setPosition(const doc::Binding& b, double p) override { in_->setPosition(b, p); }
  void setCurve(const doc::Binding& b, const dsp::Curve& c) override { in_->setCurve(b, c); }
  void end() override { in_->end(); }
  void cancel() override { in_->cancel(); }
  void setOnce(const doc::Binding& b, double p, const char* l) override { in_->setOnce(b, p, l); }
  void reset(const doc::Binding& b) override { in_->reset(b); }
  uint64_t revision() const override { return in_->revision(); }
  VoiceTapReader* voices(const doc::Binding& b) override { return b.kind == doc::BindingKind::Signal ? voices_ : nullptr; }

private:
  std::unique_ptr<panel::PanelTarget> in_;
  VoiceTapReader* voices_;
};

// One module's panel in a plain tree (as Standalone does a panel): its golden at 2x (the PA's with its voices).
Image modulePanelImage(bool vca) {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  const ModuleDoc m = buildModules(ctx.store());
  const doc::DeviceId g = vca ? m.vca.group : m.pa.group;
  const doc::Panel* p = doc::findDevice(*ctx.doc(), g).device->panel.get();
  REQUIRE(p != nullptr);
  const float scale = 2.0f;
  TestDriver d(static_cast<uint32_t>(p->width * scale), static_cast<uint32_t>(p->height * scale), scale);
  panel::PanelHost* host = nullptr;
  ChordVoices chord;
  auto face = std::make_unique<panel::GroupPanelFace>(
      panel::docPanelSource(ctx, g),
      std::make_unique<VoicedTarget>(panel::makeDocPanelTarget(ctx, g, [&host]() -> Widget* { return host; }), &chord));
  panel::GroupPanelFace* f = face.get();
  host = &d.setRoot<panel::PanelHost>("panel", std::move(face));
  d.tree().layoutNow();
  REQUIRE(d.enableGpu().ok());
  (void)f->pollVoices();
  CHECK(f->voices() == (vca ? nullptr : &chord));
  d.advance(0.1);
  auto img = d.render();
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  Image out = std::move(*img);
  d.tree().setRoot(std::make_unique<Widget>());
  return out;
}

// The modules in the shell's project patch view (they spawn closed), the view at zoom 1 on one.
struct ModuleUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  ModuleDoc m;
  int structural = 0;
  ViewContext::Subscription sub;

  explicit ModuleUi(bool onVca, bool gpu = false) {
    ctx.setDeviceLibrary(&devLib());
    m = buildModules(ctx.store());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    if (gpu) REQUIRE(d.enableGpu().ok());
    view().setPanelsShown(false);
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    frame(onVca ? m.vca.group : m.pa.group);
    d.tree().setFocus(&view().canvas());
    sub = ctx.subscribe([this](const doc::StoreEvent& e) { structural += e.before != e.after && e.changes.structural() ? 1 : 0; });
  }
  ~ModuleUi() {
    sub.reset();
    d.tree().setRoot(std::make_unique<Widget>());
  }
  void frame(doc::DeviceId g) {
    const Node& n = node(g);
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = n.x - 40.0;
    v.y = n.y - 40.0;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
  }
  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const Node& node(doc::DeviceId id) const {
    const uint32_t n = view().patch().nodeOf(id);
    REQUIRE(n != kNone);
    return view().patch().nodes[n];
  }
  // A point of group g's closed panel (panel units), device px.
  Vec2 at(doc::DeviceId g, Vec2 local) const {
    const Node& n = node(g);
    return view().canvas().toDevice({n.x + local.x * n.scale, n.y + local.y * n.scale});
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Device& dev(doc::DeviceId id) const { return *doc::findDevice(*ctx.doc(), id).device; }
  double position(doc::DeviceId id) const {
    const doc::DeviceValue* v = dev(id).param(doc::kValueParam);
    return v != nullptr ? v->value : 0.0;
  }
  // The control's curve (the declared linear 0 … 1 when the block stores none).
  dsp::Curve curve(doc::DeviceId id) const {
    const dsp::Curve* c = dev(id).curve(doc::kValueParam);
    return c != nullptr ? *c : dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 1.0);
  }
};

// The dial's geometry (rich_elements.cpp, a 120-unit dial at its panel's origin): the knob's centre, a marker tick's
// middle at knob position t.
constexpr Vec2 kDialC{60.0f, 61.0f};
Vec2 tickAt(double t) {
  const float a = (90.0f + 360.0f * static_cast<float>(t)) * kPi / 180.0f, r = 46.5f + 2.25f + 1.0f + 2.0f;
  return {kDialC.x + r * std::cos(a), kDialC.y + r * std::sin(a)};
}
// The point of curve c whose value is v (its index), or 0.
uint32_t pointOf(const dsp::Curve& c, double v) {
  for (uint32_t i = 1; i < c.segments; ++i)
    if (std::abs(c.min + c.points[i].y * (c.max - c.min) - v) < 1e-6 * std::max(1.0, std::abs(v))) return i;
  return 0;
}

}  // namespace

TEST_CASE("render: module-phase-acc-panel and module-vca-panel (GU.5b: the old faces from rich elements, 2x)") {
  checkImage("module-phase-acc-panel", modulePanelImage(false));
  checkImage("module-vca-panel", modulePanelImage(true));
}

TEST_CASE("module faces: modules spawn closed as their panels; the VCA is round (its ports orbit the dial), the PA too") {
  ModuleUi u(true);
  for (const doc::DeviceId g : {u.m.vca.group, u.m.pa.group}) {
    CHECK_FALSE(u.view().groupOpen(g));
    const Node& n = u.node(g);
    CHECK(n.groupPanel);
    CHECK(n.panelRound);
    CHECK(n.outline.round);
  }
  CHECK(u.node(u.m.vca.group).w == doctest::Approx(120.0f));  // the old face's size
}

TEST_CASE("module faces: the VCA's markers are its curve's points — a tick dragged round the dial moves the point and its "
          "opposite, one undo step, no recompile; the chevron's popup sets a preset — real dispatch") {
  ModuleUi u(true);
  const doc::DeviceId value = u.m.vca.value, g = u.m.vca.group;
  const dsp::Curve before = u.curve(value);
  const uint32_t p10 = pointOf(before, 10.0), m10 = pointOf(before, -10.0);
  REQUIRE(p10 != 0);
  REQUIRE(m10 != 0);
  const double t10 = before.points[p10].t, tm10 = before.points[m10].t;
  const size_t h0 = u.history();
  // The +10 tick, round the track by 0.04 of the travel.
  const Vec2 from = u.at(g, tickAt(t10)), to = u.at(g, tickAt(t10 + 0.04));
  u.d.drag(from, to, 8);
  u.d.step();
  const dsp::Curve after = u.curve(value);
  CHECK(after.points[p10].t == doctest::Approx(t10 + 0.04).epsilon(0.02));
  CHECK(after.points[m10].t == doctest::Approx(tm10 - (after.points[p10].t - t10)).epsilon(1e-9));  // mirrored
  CHECK(after.points[p10].y == before.points[p10].y);  // the marker keeps its value: it moves along the knob
  CHECK(dsp::curveClass(after) == dsp::curveClass(before));
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);
  // The knob still plays the law (x1 at 0.7 is untouched by a marker beyond +1).
  CHECK(dsp::evalCurve(after, u.position(value)) == doctest::Approx(1.0).epsilon(1e-9));
  REQUIRE(u.ctx.undo());
  CHECK(u.curve(value) == before);
  // The knob: dragged down, less; one undo step, no recompile.
  const double p0 = u.position(value);
  const Vec2 k = u.at(g, {kDialC.x + 25.0f, kDialC.y});
  u.d.drag(k, {k.x, k.y + 60.0f}, 6);
  u.d.step();
  CHECK(u.position(value) < p0 - 0.1);
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);
  // The chevron opens the popup under the dial; its "0 -> 1" chip sets that curve (one step).
  u.d.click(u.at(g, {60.0f, 85.0f}));
  u.d.step();
  const size_t h1 = u.history();
  u.d.click(u.at(g, {33.0f, 126.0f + 68.0f}));  // chip 1 (x 25 … 41), its row 63 … 73 under the popup's top (126)
  u.d.step();
  // (linear 0 … 1 is the value block's declared curve: the block stores none then)
  const dsp::Curve* now = u.dev(value).curve(doc::kValueParam);
  CHECK((now == nullptr || *now == dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 1.0)));
  CHECK_FALSE(u.curve(value) == before);
  CHECK(u.history() == h1 + 1);
}

TEST_CASE("module faces: the Phase Accumulator's frequency arc dragged up — one undo step, no recompile — real dispatch") {
  ModuleUi u(false);
  const doc::DeviceId g = u.m.pa.group;
  const double p0 = u.position(u.m.pa.freq);
  // The arc's handle at the value (0: the arc's start, 150° round (60, 64) at radius 39).
  const float a = (150.0f + 90.0f * static_cast<float>(p0)) * kPi / 180.0f;
  const Vec2 h = u.at(g, {60.0f + 39.0f * std::cos(a), 64.0f + 39.0f * std::sin(a)});
  const size_t h0 = u.history();
  u.d.drag(h, {h.x, h.y - 40.0f}, 6);
  u.d.step();
  CHECK(u.position(u.m.pa.freq) > p0 + 0.05);
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);
  // The pitch arc's scale: the middle half of the travel holds ±12 semitones (the old face's), straight segments.
  const dsp::Curve pitch = u.curve(u.m.pa.pitch);
  CHECK(dsp::evalCurve(pitch, 0.75) == doctest::Approx(1200.0));
  CHECK(dsp::evalCurve(pitch, 0.5) == doctest::Approx(0.0).epsilon(1e-12));
  CHECK(dsp::curveIsLinear(pitch));
}

TEST_CASE("module faces: a double-click opens a module into its circuit (atoms as symbols, values as knobs, traces) and "
          "on its title strip closes it — real dispatch") {
  ModuleUi u(true);
  const doc::DeviceId g = u.m.vca.group;
  u.d.doubleClick(u.at(g, {60.0f, 116.0f}));  // the dial's rim: the block's
  u.d.step();
  REQUIRE(u.view().groupOpen(g));
  const ProjectPatch& pp = u.view().patch();
  const Node& cap = u.node(g);
  CHECK_FALSE(cap.groupPanel);
  const Node& in = pp.nodes[cap.groupIn];
  const Node& out = pp.nodes[cap.groupOut];
  const Node& mul = u.node(u.m.vca.mul);
  const Node& val = u.node(u.m.vca.value);
  // The atom is the circuit's multiplier symbol; the value block a knob (its control anchored on it).
  REQUIRE(mul.symbol != nullptr);
  CHECK(mul.symbol->symbol == circuit::Symbol::Multiplier);
  CHECK(mul.w < 60.0f);
  CHECK(val.valueKnob);
  CHECK(val.w == doctest::Approx(metrics::kValueKnobW));
  // Signal flow left to right: In, the knob and the multiplier, Out; all inside the capsule.
  CHECK(in.x < mul.x);
  CHECK(val.x + val.w <= mul.x);
  CHECK(mul.x + mul.w < out.x);
  for (const Node* k : {&in, &out, &mul, &val}) {
    CHECK(k->x >= cap.x);
    CHECK(k->x + k->w <= cap.x + cap.w);
    CHECK(k->y >= cap.y + metrics::kCapsuleTitle);
    CHECK(k->y + k->h <= cap.y + cap.h);
  }
  // The wires among them are orthogonal traces (no hanging cables).
  size_t traces = 0;
  for (const Edge& e : pp.edges) {
    if (pp.nodes[e.from].capsule != u.view().patch().nodeOf(g) || pp.nodes[e.to].capsule != u.view().patch().nodeOf(g)) continue;
    ++traces;
    CHECK_FALSE(e.cable);
    for (size_t k = 1; k < e.points.size(); ++k)
      CHECK((e.points[k].x == e.points[k - 1].x || e.points[k].y == e.points[k - 1].y));
  }
  CHECK(traces == 3);  // In → ×, value → ×, × → Out
  // The title strip: a double-click closes it again (its panel).
  u.d.doubleClick(u.view().canvas().toDevice({cap.x + 40.0f, cap.y + metrics::kCapsuleTitle * 0.5f}));
  u.d.step();
  CHECK_FALSE(u.view().groupOpen(g));
  CHECK(u.node(g).groupPanel);
}

TEST_CASE("render: project-module-circuits (GU.5b: the Phase Accumulator and the VCA opened, each a circuit)") {
  ModuleUi u(false, true);
  u.view().setGroupOpen(u.m.pa.group, true);
  u.view().setGroupOpen(u.m.vca.group, true);
  u.d.tree().layoutNow();
  u.frame(u.m.pa.group);
  u.d.advance(0.1);
  auto img = u.d.render();
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  checkImage("project-module-circuits", *img);
}
