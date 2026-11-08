// EQ.4 (docs/plans/EQ.md "Displays (EQ.4)"; response_elements.cpp): the `response` display (its magnitude, phase and
// delay areas) and the EQ panel.
//   curve      the total the response display draws is the chain group's GroupResponse (In 0 → Out 0) at its grid
//              points; a drag re-reads the knobs without lowering again, a structural edit lowers once
//   gestures   (real dispatch: the Shell's Device view hosts the EQ's panel) double-click empty space adds a bell there,
//              a node drags frequency + gain (Q for a gainless type), Shift is fine, the wheel turns Q (a run = one
//              step), Alt + wheel steps Stages, double-click a node resets its gain, right-click: Type / Bypass / Solo /
//              Delete, a click selects, Delete removes the selected band, the Mode menu splits through setEqMode (A
//              copied into B) — each one undo step
//   reuse      (EQ.4b) a lone EQ Band's own panel: its response, phase and analyser
//   EQ.4b      the curve after the circuit view opens and closes the EQ (real dispatch); the phase area wrapped by
//              default, unwrapped by its chip; the strip's "Q 0.71" / "×16"; with the app's lowering a structural edit
//              lowers nothing on the UI thread (the curve stays until the compile arrives)
//   EQ.4d      the delay area: a 23-stage Q 5.16 allpass's group delay is the analytic one everywhere (no fold; the
//              unwrapped phase keeps every turn), 0 ms at the floor, a cut's true negative delay below it; the three
//              areas open / close by their title chips (real dispatch), all 8 combinations stacked in order
//   goldens    (2x) eq-default, eq-eight-bands (a 16-stage low cut, a Q 40 bell, a spectrum behind, a band selected),
//              eq-lr-mode (chain B translucent behind A), eq-phase-delay (magnitude and delay open, phase closed),
//              eq-all-views (all three areas: the 23-stage allpass and a cut bell), eq-band-response (a lone band's)
//   perf       a drag on a 31-band EQ re-evaluates and redraws the curve within a frame (< 2 ms, optimised builds); the
//              frame after a 32nd band is added, lowered here (before EQ.4b) and with the app's lowering
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/dsp/svf_design.h"
#include "pq/document/edit_ops.h"
#include "pq/document/group_response.h"
#include "pq/document/lower.h"
#include "pq/document/modules.h"
#include "pq/document/panel.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/panel/group_responses.h"
#include "pq/views/panel/response_display.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using Cx = std::complex<double>;

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

const doc::Device& dev(const doc::Project& p, doc::DeviceId id) {
  const doc::Device* d = doc::findDevice(p, id).device;
  REQUIRE(d != nullptr);
  return *d;
}
double units(const doc::Project& p, doc::DeviceId block) { return doc::valueBlockUnits(dev(p, block)); }
// Sets value block `block` to `v` in its curve's units (one step).
void setUnits(doc::DocumentStore& st, doc::DeviceId block, double v) {
  const doc::Device& d = dev(*st.current(), block);
  const dsp::Curve* c = d.curve(doc::kValueParam);
  REQUIRE(c != nullptr);
  run(st, doc::ops::setDeviceParam(*st.current(), block, std::string(doc::kValueParam), dsp::curvePosition(*c, dsp::curveOutput(*c, v), 0.5)));
}

doc::EqInfo eqOf(const doc::Project& p, doc::DeviceId eq) {
  std::optional<doc::EqInfo> e = doc::eqInfo(p, eq);
  REQUIRE(e.has_value());
  return *e;
}

panel::Element* elementOf(panel::GroupPanelFace& f, std::string_view kind) {
  for (size_t i = 0; i < f.elementCount(); ++i)
    if (f.element(i).desc().kind == kind) return &f.element(i);
  return nullptr;
}

// |H| in dB of `group` (In 0 → Out 0) at `hz`, from its own GroupResponse on a fresh lowering.
std::vector<double> groupDb(const doc::Project& p, doc::DeviceId group, const std::vector<double>& hz, double sr) {
  doc::LowerOptions lo;
  lo.library = &devLib().library();
  lo.devices = &devLib();
  lo.clipSlotOf = [](doc::TrackId) { return 0u; };
  auto low = doc::lower(p, lo);
  REQUIRE(low.ok());
  doc::GroupResponse r;
  const Status st = r.prepare(p, *low, devLib(), group);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  std::vector<Cx> h(hz.size() * r.inChannels() * r.outChannels());
  r.evaluate(hz, sr, h);
  std::vector<double> db(hz.size());
  for (size_t k = 0; k < hz.size(); ++k)
    db[k] = 20.0 * std::log10(std::max(std::abs(h[k * r.inChannels() * r.outChannels()]), 1e-9));
  return db;
}

// ---- a panel alone in a tree (goldens, the curve check, perf) --------------------------------------------------------

struct Alone {
  FakeTransport port;
  ViewContext ctx{port};
  doc::TrackId track;
  doc::DeviceId group;
  std::unique_ptr<TestDriver> d;
  panel::PanelHost* host = nullptr;

  explicit Alone(std::string_view module = "eq") {
    ctx.setDeviceLibrary(&devLib());
    doc::DocumentStore& st = ctx.store();
    track = run(st, doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "Audio")).tracks.at(0);
    group = test_blocks::addModule(st, track, module);
  }
  ~Alone() {
    if (d) d->tree().setRoot(std::make_unique<Widget>());
  }
  doc::DocumentStore& st() { return ctx.store(); }
  doc::EqInfo info() { return eqOf(*ctx.doc(), group); }
  // The panel in a tree of its size at `scale`.
  void show(float scale = 2.0f) {
    const doc::Panel* p = dev(*ctx.doc(), group).panel.get();
    REQUIRE(p != nullptr);
    CHECK(doc::bindingsProblem(doc::findTrackPtr(*ctx.doc(), track)->devices, group, *p) == "");
    d = std::make_unique<TestDriver>(static_cast<uint32_t>(p->width * scale), static_cast<uint32_t>(p->height * scale), scale);
    auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, group),
                                                        panel::makeDocPanelTarget(ctx, group, [this]() -> Widget* { return host; }));
    host = &d->setRoot<panel::PanelHost>("panel", std::move(face));
    d->tree().layoutNow();
    REQUIRE(d->enableGpu().ok());  // (the theme: a panel paints, and its displays compute their curves, only with one)
    d->step();
  }
  panel::GroupPanelFace& face() { return *host->panelFace(); }
  panel::Element& element(std::string_view kind) {
    (void)face().sync();
    panel::Element* e = elementOf(face(), kind);
    REQUIRE(e != nullptr);
    return *e;
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

// A plausible analyser picture (dB per point of the analyser's log 10 Hz … 22 kHz axis): pink-ish with a low bump.
devices::SpectrumSnapshot fakeSpectrum() {
  devices::SpectrumSnapshot s;
  const uint32_t n = 256;
  for (uint32_t k = 0; k < n; ++k) {
    const double hz = 10.0 * std::pow(2200.0, static_cast<double>(k) / (n - 1));
    const double o = std::log2(std::max(hz, 30.0) / 150.0);
    double db = -30.0 - 4.2 * o + 7.0 * std::exp(-o * o / 0.5) - 2.0 * std::sin(o * 3.1) - (hz < 30.0 ? (30.0 - hz) * 1.5 : 0.0);
    if (hz > 12000.0) db -= (hz - 12000.0) / 600.0;
    s.out.push_back(static_cast<float>(db));
  }
  s.peaks = s.out;
  for (float& v : s.peaks) v += 4.0f;
  return s;
}

// ---- the Device view (gestures through real dispatch) ----------------------------------------------------------------

struct EqUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{2600, 900};
  std::unique_ptr<Shell> shell;
  doc::TrackId track;
  doc::DeviceId eq;

  explicit EqUi(bool midi = false) {
    ctx.setDeviceLibrary(&devLib());
    doc::DocumentStore& st = ctx.store();
    track = run(st, doc::ops::createTrack(*st.current(), st.ids(), midi ? doc::TrackKind::Midi : doc::TrackKind::Audio, {}, midi ? "MIDI" : "Audio")).tracks.at(0);
    eq = test_blocks::addModule(st, track, "eq");
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(300.0f);
    ctx.setSelectedTrack(track);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
    d.tree().layoutNow();
    REQUIRE(d.enableGpu().ok());  // (the theme: the displays compute their curves when they paint)
    d.step();
  }
  ~EqUi() { d.tree().setRoot(std::make_unique<Widget>()); }

  doc::DocumentStore& st() { return ctx.store(); }
  const doc::Project& doc() const { return *ctx.doc(); }
  doc::EqInfo info() const { return eqOf(*ctx.doc(), eq); }
  size_t history() const { return ctx.store().history().cursor(); }
  DeviceChainView& view() const {
    DeviceChainView* v = findDeviceChain(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  panel::PanelHost& host() {
    d.tree().layoutNow();
    panel::PanelHost* h = view().controlHost(info().output, doc::kValueParam);
    REQUIRE(h != nullptr);
    return *h;
  }
  panel::Element& element(std::string_view kind) {
    panel::GroupPanelFace& f = *host().panelFace();
    (void)f.sync();
    panel::Element* e = elementOf(f, kind);
    REQUIRE(e != nullptr);
    return *e;
  }
  // Element-local `l` of the element of `kind` in device px.
  Vec2 at(std::string_view kind, Vec2 l) {
    const panel::Element& e = element(kind);
    return host().transform().map({e.rect().x0 + l.x, e.rect().y0 + l.y});
  }
  // Where band `band`'s node is drawn now (device px).
  Vec2 node(doc::DeviceId band) {
    d.step();
    const std::optional<Vec2> n = panel::test::nodeOf(element("response"), band);
    REQUIRE(n.has_value());
    return at("response", *n);
  }
  // A point of the response display at `hz` and `db` (device px).
  Vec2 point(double hz, double db) {
    d.step();
    const panel::Element& e = element("response");
    return at("response", {static_cast<float>(panel::test::axisT(hz)) * e.desc().w, *panel::test::yOfDb(e, db)});
  }
  Menu* lastMenu() {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<Menu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  int32_t item(Menu& m, std::string_view label) {
    for (size_t i = 0; i < m.items().size(); ++i)
      if (m.items()[i].label == label) return static_cast<int32_t>(i);
    return -1;
  }
  panel::PanelTarget& target() { return host().panelFace()->target(); }
};

}  // namespace

TEST_CASE("EQ.4 response: the drawn total is the chain's GroupResponse; a drag never lowers again, a structural edit once") {
  Alone a;
  const doc::EqInfo e = a.info();
  setUnits(a.st(), e.bands[0][1].gain, 7.5);   // the 400 Hz bell
  setUnits(a.st(), e.bands[0][2].gain, -11.0);  // the 2.5 kHz bell
  setUnits(a.st(), e.bands[0][2].q, 4.0);
  setUnits(a.st(), e.bands[0][3].gain, 4.0);   // the high shelf
  a.show(1.0f);
  panel::Element& r = a.element("response");
  std::vector<double> hz;
  std::vector<uint32_t> ks = {0, 60, 200, 400, 523, 610, 700, panel::test::responsePoints() - 1};
  for (const uint32_t k : ks) hz.push_back(panel::test::responseHz(k));
  const std::vector<double> want = groupDb(*a.ctx.doc(), e.chain[0], hz, a.ctx.sampleRate());
  for (size_t i = 0; i < ks.size(); ++i) {
    CAPTURE(hz[i]);
    const std::optional<float> got = panel::test::drawnDb(r, panel::test::responseX(r, ks[i]));
    REQUIRE(got.has_value());
    CHECK(*got == doctest::Approx(want[i]).epsilon(1e-4).scale(1.0));
  }
  CHECK(std::fabs(want[3]) > 1.0);  // (the points are not all 0 dB)
  CHECK(panel::test::drawsSeries(r));  // (the chain: the product of its bands)
  // A knob change: re-read without a lowering; a band added: one lowering.
  GroupResponses& gr = a.ctx.groupResponses();
  const uint32_t n0 = gr.lowerings();
  setUnits(a.st(), e.bands[0][1].gain, -3.0);
  a.host->notifyChanged();
  a.d->step();
  CHECK(gr.lowerings() == n0);
  const std::vector<double> after = groupDb(*a.ctx.doc(), e.chain[0], hz, a.ctx.sampleRate());
  for (size_t i = 0; i < ks.size(); ++i)
    CHECK(*panel::test::drawnDb(a.element("response"), panel::test::responseX(r, ks[i])) == doctest::Approx(after[i]).epsilon(1e-4).scale(1.0));
  run(a.st(), doc::ops::addBand(*a.ctx.doc(), a.st().ids(), a.group, 0, 4, 5000.0, 6.0));
  a.host->notifyChanged();
  a.d->step();
  CHECK(gr.lowerings() == n0 + 1);
}

TEST_CASE("EQ.4b response: the curve stays after the EQ is opened and closed in the circuit view (real dispatch)") {
  bool midi = false;
  SUBCASE("an audio track") {}
  SUBCASE("a MIDI track, after its instrument") { midi = true; }
  CAPTURE(midi);
  EqUi u(midi);
  const doc::EqInfo e = u.info();
  setUnits(u.st(), e.bands[0][1].gain, 7.5);
  setUnits(u.st(), e.bands[0][2].gain, -6.0);
  u.d.step();
  std::vector<double> hz;
  const std::vector<uint32_t> ks = {60, 200, 400, 523, 700};
  for (const uint32_t k : ks) hz.push_back(panel::test::responseHz(k));
  auto checkCurve = [&] {
    u.d.step();
    panel::Element& r = u.element("response");
    const std::vector<double> want = groupDb(u.doc(), e.chain[0], hz, u.ctx.sampleRate());
    for (size_t i = 0; i < ks.size(); ++i) {
      CAPTURE(hz[i]);
      const std::optional<float> got = panel::test::drawnDb(r, panel::test::responseX(r, ks[i]));
      REQUIRE(got.has_value());
      CHECK(*got == doctest::Approx(want[i]).epsilon(1e-4).scale(1.0));
    }
    for (const doc::BandInfo& b : e.bands[0]) CHECK(u.ctx.groupResponses().get(b.group) != nullptr);
  };
  checkCurve();
  // The circuit view, as the app opens it (Open in Circuit View on the device), the EQ opened and closed there by a
  // double-click (its closed block; its title strip), then back to the Device view.
  REQUIRE(u.view().openInCircuitView(u.eq));
  auto* pv = findProjectPatch(*u.shell);
  REQUIRE(pv != nullptr);
  pv->setPushAnimated(false);
  u.d.advance(1.0);
  u.d.tree().layoutNow();
  u.d.step();
  pv->setGroupOpen(u.eq, true);
  u.d.advance(1.0);
  u.d.step();
  pv->setGroupOpen(u.eq, false);
  u.d.advance(1.0);
  u.d.step();
  {  // The closed EQ in the circuit: its panel face draws the same curve.
    const uint32_t mn = pv->patch().nodeOf(u.eq);
    REQUIRE(mn != project_patch::kNone);
    REQUIRE(mn < pv->scene().sceneNode.size());
    auto* f = dynamic_cast<panel::GroupPanelFace*>(pv->canvas().panelOn(pv->scene().sceneNode[mn]));
    REQUIRE(f != nullptr);
    (void)f->sync();
    panel::Element* r = elementOf(*f, "response");
    REQUIRE(r != nullptr);
    u.d.step();
    const std::vector<double> want = groupDb(u.doc(), e.chain[0], hz, u.ctx.sampleRate());
    for (size_t i = 0; i < ks.size(); ++i) {
      const std::optional<float> got = panel::test::drawnDb(*r, panel::test::responseX(*r, ks[i]));
      REQUIRE(got.has_value());
      CHECK(*got == doctest::Approx(want[i]).epsilon(1e-4).scale(1.0));
    }
  }
  pv->setShown(false, false);
  u.d.advance(1.0);
  u.d.tree().layoutNow();
  u.d.step();
  checkCurve();
}

TEST_CASE("EQ.4b phase: wrapped to ±180° by default; WRAP / UNWRAP unwraps (auto-scaled); the delay area apart (EQ.4d)") {
  EqUi u;
  const doc::EqInfo e = u.info();
  // A 16-stage low cut at 60 Hz: thousands of degrees unwrapped.
  run(u.st(), doc::ops::setBandType(u.doc(), e.bands[0][0].group, 1));
  run(u.st(), doc::ops::setBandStages(u.doc(), e.bands[0][0].group, 16));
  setUnits(u.st(), e.bands[0][0].frequency, 60.0);
  setUnits(u.st(), e.bands[0][2].gain, 9.0);
  u.d.step();
  panel::Element& ph = u.element("response");
  panel::PanelTarget& t = u.target();
  using Chip = panel::test::ResponseChip;
  // The chain's H at a few grid points (its own response on a fresh lowering).
  doc::LowerOptions lo;
  lo.library = &devLib().library();
  lo.devices = &devLib();
  lo.clipSlotOf = [](doc::TrackId) { return 0u; };
  auto low = doc::lower(u.doc(), lo);
  REQUIRE(low.ok());
  doc::GroupResponse gr;
  REQUIRE(gr.prepare(u.doc(), *low, devLib(), e.chain[0]).ok());
  const std::vector<uint32_t> ks = {40, 120, 300, 520, 700};
  std::vector<double> hz;
  for (const uint32_t k : ks) hz.push_back(panel::test::responseHz(k));
  std::vector<Cx> h(hz.size() * gr.inChannels() * gr.outChannels());
  gr.evaluate(hz, u.ctx.sampleRate(), h);
  auto xOf = [&](uint32_t k) { return ph.desc().w * static_cast<float>(k) / static_cast<float>(panel::test::responsePoints() - 1); };
  // Wrapped (the default): arg H in degrees, the scale ±180°.
  CHECK(!panel::test::phaseUnwrapped(ph, t));
  CHECK(panel::test::phaseScale(ph) == std::pair<double, double>{-180.0, 180.0});
  for (size_t i = 0; i < ks.size(); ++i) {
    CAPTURE(hz[i]);
    const double want = std::arg(h[i * gr.inChannels() * gr.outChannels()]) * 180.0 / 3.141592653589793;
    const float got = panel::test::phaseAt(ph, xOf(ks[i]));
    CHECK(got >= -180.0f);
    CHECK(got <= 180.0f);
    CHECK(std::fabs(std::remainder(got - want, 360.0)) < 0.01);
  }
  // The WRAP chip (real dispatch): unwrapped, auto-scaled over the cut's thousands of degrees; the same angles mod 360.
  u.d.click(u.at("response", panel::test::chip(ph, Chip::Wrap)));
  u.d.step();
  CHECK(panel::test::phaseUnwrapped(ph, t));
  const auto [lo1, hi1] = panel::test::phaseScale(ph);
  CHECK(hi1 - lo1 > 1000.0);
  for (size_t i = 0; i < ks.size(); ++i) {
    const double want = std::arg(h[i * gr.inChannels() * gr.outChannels()]) * 180.0 / 3.141592653589793;
    CHECK(std::fabs(std::remainder(panel::test::phaseAt(ph, xOf(ks[i])) - want, 360.0)) < 0.01);
  }
  // EQ.4d: the delay area opens beside it (its bar's chip); the phase stays unwrapped, the delay has its own scale.
  u.d.click(u.at("response", panel::test::chip(ph, Chip::Delay)));
  u.d.step();
  CHECK(panel::test::area(ph, 2)->open);
  CHECK(panel::test::phaseUnwrapped(ph, t));
  CHECK(panel::test::delayScale(ph).second > 0.0);
  // The WRAP chip wraps again.
  u.d.click(u.at("response", panel::test::chip(ph, Chip::Wrap)));
  u.d.step();
  CHECK(!panel::test::phaseUnwrapped(ph, t));
}

TEST_CASE("EQ.4c phase: a dotted wrap marker at each wrap of the wrapped curve, at the crossing; none unwrapped") {
  EqUi u;
  const doc::EqInfo e = u.info();
  run(u.st(), doc::ops::setBandType(u.doc(), e.bands[0][0].group, 1));
  run(u.st(), doc::ops::setBandStages(u.doc(), e.bands[0][0].group, 16));
  setUnits(u.st(), e.bands[0][0].frequency, 60.0);
  u.d.step();
  panel::Element& ph = u.element("response");
  panel::PanelTarget& t = u.target();
  REQUIRE(!panel::test::phaseUnwrapped(ph, t));
  // The reference: the chain's own H on a grid 8× finer than the strip's, unwrapped; each crossing of an odd multiple
  // of 180° (linear between samples) is one wrap, at its x on the axis.
  doc::LowerOptions lo;
  lo.library = &devLib().library();
  lo.devices = &devLib();
  lo.clipSlotOf = [](doc::TrackId) { return 0u; };
  auto low = doc::lower(u.doc(), lo);
  REQUIRE(low.ok());
  doc::GroupResponse gr;
  REQUIRE(gr.prepare(u.doc(), *low, devLib(), e.chain[0]).ok());
  const uint32_t n = (panel::test::responsePoints() - 1) * 8 + 1;
  std::vector<double> hz(n);
  for (uint32_t i = 0; i < n; ++i) hz[i] = panel::test::axisHz(static_cast<double>(i) / (n - 1));
  hz[0] = panel::test::responseHz(0);  // (as the strip: not DC itself, where a cut's H is 0)
  const size_t stride = gr.inChannels() * gr.outChannels();
  std::vector<Cx> h(n * stride);
  gr.evaluate(hz, u.ctx.sampleRate(), h);
  constexpr double kPi = 3.141592653589793;
  std::vector<float> want;
  double prev = std::arg(h[0]) * 180.0 / kPi, acc = prev;
  for (uint32_t i = 1; i < n; ++i) {
    const double a = std::arg(h[i * stride]) * 180.0 / kPi;
    const double next = acc + std::remainder(a - prev, 360.0);
    prev = a;
    // Odd multiples of 180° in (acc, next] or [next, acc).
    const double lo2 = std::min(acc, next), hi2 = std::max(acc, next);
    for (double m = std::ceil((lo2 - 180.0) / 360.0) * 360.0 + 180.0; m <= hi2; m += 360.0)
      if (m > lo2)
        want.push_back(ph.desc().w * static_cast<float>((i - 1 + (m - acc) / (next - acc)) / (n - 1)));
    acc = next;
  }
  const std::vector<float> got = panel::test::phaseWraps(ph);
  CAPTURE(want.size());
  REQUIRE(want.size() == 8);  // 16 two-pole stages: 16 × 180° = 2880° from DC to the top, eight wraps
  REQUIRE(got.size() == want.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i);
    CHECK(std::fabs(got[i] - want[i]) <= 1.0f);
  }
  // Unwrapped: no markers.
  u.d.click(u.at("response", panel::test::chip(ph, panel::test::ResponseChip::Wrap)));
  u.d.step();
  REQUIRE(panel::test::phaseUnwrapped(ph, t));
  CHECK(panel::test::phaseWraps(ph).empty());
}

TEST_CASE("EQ.4b strip and band panel: Q boxes read \"Q 0.71\", Stages boxes \"×16\"; other number boxes keep their readout") {
  Alone a;
  const doc::EqInfo e = a.info();
  run(a.st(), doc::ops::setBandType(*a.ctx.doc(), e.bands[0][0].group, 1));
  run(a.st(), doc::ops::setBandStages(*a.ctx.doc(), e.bands[0][0].group, 16));
  setUnits(a.st(), e.bands[0][1].q, 4.0);
  a.show(1.0f);
  panel::GroupPanelFace& f = a.face();
  (void)f.sync();
  panel::PanelTarget& t = f.target();
  auto textOf = [&](doc::DeviceId block) -> std::string {
    for (size_t i = 0; i < f.elementCount(); ++i) {
      const panel::Element& x = f.element(i);
      if (x.desc().kind == "number" && x.binding() != nullptr && x.binding()->device == block) return panel::test::numberText(x, t).value_or("?");
    }
    return "none";
  };
  CHECK(textOf(e.bands[0][0].stages) == "\xC3\x97" "16");
  CHECK(textOf(e.bands[0][0].q) == "Q 0.71");
  CHECK(textOf(e.bands[0][1].q) == "Q 4.00");
  CHECK(textOf(e.bands[0][1].frequency) == "400 Hz");  // (the Frequency box: the control's own readout)
  // The lone band's own panel too.
  Alone b("eq_band");
  const doc::BandInfo bi = *doc::bandInfo(*b.ctx.doc(), b.group);
  run(b.st(), doc::ops::setBandType(*b.ctx.doc(), b.group, 0));
  run(b.st(), doc::ops::setBandStages(*b.ctx.doc(), b.group, 4));
  b.show(1.0f);
  panel::GroupPanelFace& g = b.face();
  (void)g.sync();
  std::vector<std::string> texts;
  for (size_t i = 0; i < g.elementCount(); ++i)
    if (g.element(i).desc().kind == "number") texts.push_back(panel::test::numberText(g.element(i), g.target()).value_or("?"));
  CHECK(std::find(texts.begin(), texts.end(), "Q 0.71") != texts.end());
  CHECK(std::find(texts.begin(), texts.end(), "\xC3\x97" "4") != texts.end());
  (void)bi;
}

TEST_CASE("EQ.4b response: with the app's lowering a structural edit lowers nothing here; the curve stays until the compile arrives") {
  Alone a;
  a.ctx.setLoweringProvided(true);
  // The app's compile (here: a lowering made by the test, handed over as the link does).
  auto compile = [&] {
    doc::LowerOptions lo;
    lo.library = &devLib().library();
    lo.devices = &devLib();
    lo.clipSlotOf = [](doc::TrackId) { return 0u; };
    auto low = doc::lower(*a.ctx.doc(), lo);
    REQUIRE(low.ok());
    a.ctx.setLowering(a.ctx.docSerial(), a.ctx.doc(), std::make_shared<const doc::LoweredProject>(std::move(*low)));
  };
  const doc::EqInfo e = a.info();
  setUnits(a.st(), e.bands[0][1].gain, 6.0);
  a.show(1.0f);
  GroupResponses& gr = a.ctx.groupResponses();
  panel::Element* r = &a.element("response");
  CHECK(gr.waiting());  // (nothing compiled yet: no curve)
  CHECK(std::isnan(*panel::test::drawnDb(*r, panel::test::responseX(*r, 400))));
  compile();
  a.host->notifyChanged();
  a.d->step();
  std::vector<double> hz;
  const std::vector<uint32_t> ks = {100, 300, 400, 523, 700};
  for (const uint32_t k : ks) hz.push_back(panel::test::responseHz(k));
  auto check = [&](const std::vector<double>& want) {
    r = &a.element("response");
    for (size_t i = 0; i < ks.size(); ++i)
      CHECK(*panel::test::drawnDb(*r, panel::test::responseX(*r, ks[i])) == doctest::Approx(want[i]).epsilon(1e-4).scale(1.0));
  };
  const std::vector<double> before = groupDb(*a.ctx.doc(), e.chain[0], hz, a.ctx.sampleRate());
  check(before);
  // A band added (structural): the next frames draw the previous curve (no lowering here) …
  run(a.st(), doc::ops::addBand(*a.ctx.doc(), a.st().ids(), a.group, 0, 4, 5000.0, 9.0));
  a.host->notifyChanged();
  a.d->step();
  CHECK(gr.waiting());
  check(before);
  // … a knob of a band it had still moves it …
  setUnits(a.st(), e.bands[0][1].gain, -4.0);
  a.host->notifyChanged();
  a.d->step();
  CHECK(*panel::test::drawnDb(*r, panel::test::responseX(*r, 400)) < -1.0f);
  // … and the compile of the new structure draws the new band.
  compile();
  a.host->notifyChanged();
  a.d->step();
  CHECK_FALSE(gr.waiting());
  check(groupDb(*a.ctx.doc(), e.chain[0], hz, a.ctx.sampleRate()));
  CHECK(gr.lowerings() == 0);
}

TEST_CASE("EQ.4 response: a node drags frequency and gain (one undo step); Shift is fine; a gainless type's y is Q") {
  EqUi u;
  doc::BandInfo bell = u.info().bands[0][1];  // 400 Hz
  const size_t h0 = u.history();
  // A click selects (nothing recorded).
  u.d.click(u.node(bell.group));
  u.d.step();
  CHECK(u.target().selectedGroup() == bell.group);
  CHECK(u.history() == h0);
  // A drag right and up: higher, louder; one step.
  const Vec2 n = u.node(bell.group);
  u.d.drag(n, {n.x + 60.0f, n.y - 30.0f}, 6);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  const double f1 = units(u.doc(), bell.frequency), g1 = units(u.doc(), bell.gain);
  CHECK(f1 > 450.0);
  CHECK(g1 > 1.0);
  REQUIRE(u.ctx.undo());
  CHECK(units(u.doc(), bell.frequency) == doctest::Approx(400.0).epsilon(1e-3));
  CHECK(units(u.doc(), bell.gain) == doctest::Approx(0.0).scale(1.0).epsilon(1e-3));
  // Shift: the same travel moves it a tenth as far.
  const Vec2 n2 = u.node(bell.group);
  u.d.drag(n2, {n2.x + 60.0f, n2.y - 30.0f}, 6, mod::kShift);
  u.d.step();
  const double f2 = units(u.doc(), bell.frequency), g2 = units(u.doc(), bell.gain);
  CHECK(f2 > 400.0);
  CHECK(std::log(f2 / 400.0) < 0.25 * std::log(f1 / 400.0));
  CHECK(g2 < 0.25 * g1);
  REQUIRE(u.ctx.undo());
  // A Low Cut (no gain): y turns Q, the gain stays.
  doc::BandInfo low = u.info().bands[0][0];
  run(u.st(), doc::ops::setBandType(u.doc(), low.group, 1));
  const double q0 = units(u.doc(), low.q), lg0 = units(u.doc(), low.gain);
  const size_t h1 = u.history();
  const Vec2 m = u.node(low.group);
  u.d.drag(m, {m.x, m.y - 40.0f}, 6);
  u.d.step();
  CHECK(u.history() == h1 + 1);
  CHECK(units(u.doc(), low.q) > q0 * 1.2);
  CHECK(units(u.doc(), low.gain) == doctest::Approx(lg0));
}

TEST_CASE("EQ.4 response: wheel → Q (a run = one step), Alt + wheel → Stages, double-click a node → 0 dB") {
  EqUi u;
  doc::BandInfo bell = u.info().bands[0][1];
  const double q0 = units(u.doc(), bell.q);
  const size_t h0 = u.history();
  const Vec2 n = u.node(bell.group);
  u.d.move(n);
  u.d.wheel(n, 1.0f);
  u.d.wheel(n, 1.0f);
  CHECK(units(u.doc(), bell.q) > q0);
  u.d.advance(panel::PanelHost::kWheelRunSeconds + 0.1);
  CHECK(u.history() == h0 + 1);
  // Alt + wheel on a Low Cut: one more stage per notch (structural: one step each).
  doc::BandInfo low = u.info().bands[0][0];
  run(u.st(), doc::ops::setBandType(u.doc(), low.group, 1));
  const size_t h1 = u.history();
  const Vec2 m = u.node(low.group);
  u.d.move(m);
  u.d.wheel(m, 1.0f, 0.0f, mod::kAlt);
  u.d.step();
  CHECK(units(u.doc(), low.stages) == doctest::Approx(2.0));
  CHECK(u.history() == h1 + 1);
  // Double-click a node: its gain back to 0 dB (one step).
  setUnits(u.st(), bell.gain, 6.0);
  const size_t h2 = u.history();
  u.d.doubleClick(u.node(bell.group));
  u.d.step();
  CHECK(units(u.doc(), bell.gain) == doctest::Approx(0.0).scale(1.0).epsilon(1e-6));
  CHECK(u.history() == h2 + 1);
}

TEST_CASE("EQ.4 response: double-click empty space adds a bell there (selected); Delete removes the selected band") {
  EqUi u;
  const size_t h0 = u.history();
  u.d.doubleClick(u.point(1200.0, -9.0));
  u.d.step();
  CHECK(u.history() == h0 + 1);
  const doc::EqInfo e = u.info();
  REQUIRE(e.bands[0].size() == 5);
  const doc::BandInfo* added = nullptr;
  for (const doc::BandInfo& b : e.bands[0])
    if (std::fabs(units(u.doc(), b.frequency) - 1200.0) < 60.0) added = &b;
  REQUIRE(added != nullptr);
  CHECK(units(u.doc(), added->gain) == doctest::Approx(-9.0).epsilon(0.05));
  CHECK(units(u.doc(), added->type) == doctest::Approx(4.0));  // a bell
  CHECK(u.target().selectedGroup() == added->group);
  // Delete (the display holds the keys after the click on its node).
  const doc::DeviceId g = added->group;
  u.d.click(u.node(g));
  u.d.press("Delete");
  u.d.step();
  CHECK(u.history() == h0 + 2);
  CHECK(u.info().bands[0].size() == 4);
  CHECK(doc::findDevice(u.doc(), g).device == nullptr);
}

TEST_CASE("EQ.4 response: right-click a node — Type, Bypass, Solo, Delete (one step each)") {
  EqUi u;
  doc::BandInfo bell = u.info().bands[0][2];  // 2.5 kHz
  auto menu = [&]() -> Menu& {
    u.d.click(u.node(bell.group), mod::kNone, PointerButton::Right);
    u.d.step();
    Menu* m = u.lastMenu();
    REQUIRE(m != nullptr);
    return *m;
  };
  Menu& m = menu();
  CHECK(u.item(m, "Type") >= 0);
  CHECK(u.item(m, "Stages") < 0);  // (a bell has none)
  CHECK(u.item(m, "Bypass") >= 0);
  CHECK(u.item(m, "Solo") >= 0);
  CHECK(u.item(m, "Delete") >= 0);
  size_t h = u.history();
  m.choose(u.item(m, "Bypass"));
  u.d.step();
  CHECK(!dev(u.doc(), bell.group).active);
  CHECK(u.history() == ++h);
  Menu& m2 = menu();
  m2.choose(u.item(m2, "Bypass"));
  u.d.step();
  CHECK(dev(u.doc(), bell.group).active);
  CHECK(u.history() == ++h);
  Menu& m3 = menu();
  m3.choose(u.item(m3, "Solo"));
  u.d.step();
  CHECK(u.history() == ++h);
  for (const doc::BandInfo& b : u.info().bands[0]) CHECK(dev(u.doc(), b.group).active == (b.group == bell.group));
  REQUIRE(u.ctx.undo());
  --h;
  // Type: the submenu's items (Low Shelf).
  Menu& m4 = menu();
  const int32_t ti = u.item(m4, "Type");
  REQUIRE(ti >= 0);
  const std::vector<MenuItem>& types = m4.items()[static_cast<size_t>(ti)].submenu;
  REQUIRE(types.size() == 8);
  CHECK(types[4].checked);
  types[5].onChoose();
  u.d.press("Escape");  // (a submenu item chosen by its callback: the menu is still open)
  u.d.step();
  CHECK(units(u.doc(), bell.type) == doctest::Approx(5.0));
  CHECK(u.history() == ++h);
  Menu& m5 = menu();
  m5.choose(u.item(m5, "Delete"));
  u.d.step();
  CHECK(u.history() == ++h);
  CHECK(u.info().bands[0].size() == 3);
}

TEST_CASE("EQ.4 EQ panel: the Mode menu splits through setEqMode (chain A copied into B: one step); the chain tabs") {
  EqUi u;
  const doc::EqInfo e0 = u.info();
  CHECK(e0.bands[1].empty());
  // The Mode menu: the menu element bound to the Mode block.
  panel::GroupPanelFace& f = *u.host().panelFace();
  (void)f.sync();
  const panel::Element* mode = nullptr;
  for (size_t i = 0; i < f.elementCount(); ++i)
    if (f.element(i).desc().kind == "menu" && f.element(i).binding() != nullptr && f.element(i).binding()->device == e0.mode) mode = &f.element(i);
  REQUIRE(mode != nullptr);
  const size_t h0 = u.history();
  u.d.click(u.host().transform().map({(mode->rect().x0 + mode->rect().x1) * 0.5f, (mode->rect().y0 + mode->rect().y1) * 0.5f}));
  u.d.step();
  Menu* m = u.lastMenu();
  REQUIRE(m != nullptr);
  m->choose(u.item(*m, "L/R"));
  u.d.step();
  CHECK(u.history() == h0 + 1);
  const doc::EqInfo e1 = u.info();
  CHECK(units(u.doc(), e1.mode) == doctest::Approx(1.0));
  CHECK(e1.bands[1].size() == 4);
  // The response display follows the Edit Chain block: B's nodes now (A's translucent behind).
  setUnits(u.st(), e1.editChain, 1.0);
  u.d.step();
  CHECK(panel::test::nodeOf(u.element("response"), e1.bands[1][0].group).has_value());
  CHECK(!panel::test::nodeOf(u.element("response"), e1.bands[0][0].group).has_value());
}

TEST_CASE("EQ.4b band panel: a lone EQ Band's own panel draws its curve, its phase and its analyser") {
  Alone a("eq_band");
  const doc::BandInfo b = *doc::bandInfo(*a.ctx.doc(), a.group);
  setUnits(a.st(), b.gain, 9.0);
  setUnits(a.st(), b.q, 2.0);
  a.show(2.0f);
  panel::Element& e = a.element("response");
  // Bound to the band: its output (the analyser's tap) and its input (the dimmed input spectrum).
  REQUIRE(e.binding(0) != nullptr);
  CHECK(e.binding(0)->device == a.group);
  REQUIRE(e.binding(1) != nullptr);
  CHECK(e.binding(1)->device == a.group);
  CHECK(e.binding(1)->kind == doc::BindingKind::Signal);
  std::vector<double> hz;
  const std::vector<uint32_t> ks = {100, 380, 450, 520, 700};
  for (const uint32_t k : ks) hz.push_back(panel::test::responseHz(k));
  const std::vector<double> want = groupDb(*a.ctx.doc(), a.group, hz, a.ctx.sampleRate());
  for (size_t i = 0; i < ks.size(); ++i)
    CHECK(*panel::test::drawnDb(e, panel::test::responseX(e, ks[i])) == doctest::Approx(want[i]).epsilon(1e-4).scale(1.0));
  CHECK(panel::test::nodeOf(e, a.group).has_value());  // its one node
  // The phase area draws the band's phase (wrapped); EQ.4d: the delay area opens on it too.
  CHECK(std::isfinite(panel::test::phaseAt(e, e.desc().w * 0.5f)));
  a.face().target().setViewValue(e.id(), "delay", 1.0);
  (void)a.render();
  CHECK(panel::test::area(e, 2)->open);
  CHECK(std::isfinite(panel::test::delayAt(e, e.desc().w * 0.5f)));
  a.face().target().setViewValue(e.id(), "delay", 0.0);
  // The analyser: the display shows a spectrum on its own axis (a golden's, without an engine).
  CHECK(panel::test::showSpectrum(e, fakeSpectrum()));
  checkImage("eq-band-response", a.render());
}

// ---- EQ.4d: the delay area -------------------------------------------------------------------------------------------

namespace {

// The analytic group delay (ms) of an svf design at `hz`: the sum over its sections (as the kernel runs them: float
// coefficients) of −d/dω arg(N / D), N and D quadratics in u = e^jω − 1 with du/dω = j·e^jω. Exact: no differences.
double svfDelayMs(const dsp::SvfParams& sp, double hz, double sr) {
  dsp::SvfSection s[dsp::kSvfMaxStages];
  const uint32_t n = dsp::svfDesign(sp, s);
  const Cx u = dsp::svfEjwMinus1(2.0 * 3.141592653589793 * hz / sr), du = Cx(0.0, 1.0) * (u + 1.0);
  double tau = 0.0;  // samples
  for (uint32_t i = 0; i < n; ++i) {
    const dsp::SvfSectionPoly p = dsp::svfSectionPoly(dsp::svfKernelCoefs(s[i]));
    if (p.e1 == 0.0 && p.e0 == 0.0) continue;
    const Cx num = p.n2 * u * u + p.n1 * u + p.n0, den = u * u + p.e1 * u + p.e0;
    tau += -std::imag((2.0 * p.n2 * u + p.n1) * du / num) + std::imag((2.0 * u + p.e1) * du / den);
  }
  return tau / sr * 1000.0;
}

// A lone EQ Band as the bug report's: an allpass, 23 stages, 1.27 kHz, Q 5.16.
struct Allpass23 {
  Alone a{"eq_band"};
  doc::BandInfo b;
  Allpass23() {
    run(a.st(), doc::ops::setBandType(*a.ctx.doc(), a.group, 7));
    run(a.st(), doc::ops::setBandStages(*a.ctx.doc(), a.group, 23));
    b = *doc::bandInfo(*a.ctx.doc(), a.group);
    setUnits(a.st(), b.frequency, 1270.0);
    setUnits(a.st(), b.q, 5.16);
  }
  dsp::SvfParams params() {
    return dsp::svfParamsFromInputs(static_cast<float>(units(*a.ctx.doc(), b.frequency)), static_cast<float>(units(*a.ctx.doc(), b.q)),
                                    1.0f, 7.0f, 23.0f, 0.0f, a.ctx.sampleRate());
  }
};

}  // namespace

TEST_CASE("EQ.4d delay: a 23-stage Q 5.16 allpass at 1.27 kHz draws its analytic group delay at every point (no fold); 0 ms at the floor") {
  Allpass23 ap;
  Alone& a = ap.a;
  a.show(1.0f);
  panel::Element& r = a.element("response");
  panel::PanelTarget& t = a.face().target();
  t.setViewValue(r.id(), "delay", 1.0);
  t.setViewValue(r.id(), "unwrap", 1.0);
  (void)a.render();
  REQUIRE(panel::test::area(r, 2)->open);
  const dsp::SvfParams sp = ap.params();
  const double sr = a.ctx.sampleRate();
  const uint32_t n = panel::test::responsePoints();
  auto xOf = [&](uint32_t k) { return r.desc().w * static_cast<float>(k) / static_cast<float>(n - 1); };
  // Every grid point: the drawn delay is the analytic one within 0.1 % + 1e-4 ms, and never negative (an allpass's
  // group delay is positive everywhere). The old neighbouring-point difference drew −36 ms at the 59.6 ms peak.
  double peak = 0.0, worstErr = 0.0;
  uint32_t bad = 0, negative = 0;
  for (uint32_t k = 0; k < n; ++k) {
    const double want = svfDelayMs(sp, panel::test::responseHz(k), sr);
    const float got = panel::test::delayAt(r, xOf(k));
    const double err = std::fabs(got - want), tol = 1e-3 * std::fabs(want) + 1e-4;
    peak = std::max(peak, want);
    worstErr = std::max(worstErr, err / tol);
    if (!(err <= tol)) {
      if (bad++ < 5) MESSAGE("point " << k << " (" << panel::test::responseHz(k) << " Hz): drawn " << got << " ms, analytic " << want << " ms");
    }
    if (got < 0.0f) ++negative;
  }
  MESSAGE("peak " << peak << " ms; worst error " << worstErr << " of the tolerance");
  CHECK(peak > 50.0);  // (23 stages at Q 5.16: ≈ 60 ms at the cutoff)
  CHECK(bad == 0);
  CHECK(negative == 0);
  // The axis: 0 at the floor (the area's bottom inset), the peak inside the top with a little headroom.
  const panel::test::ResponseArea A = *panel::test::area(r, 2);
  const auto [lo, hi] = panel::test::delayScale(r);
  CHECK(lo == 0.0);
  CHECK(hi >= peak);
  CHECK(hi <= peak * 1.6);
  CHECK(panel::test::delayY(r, 0.0) == doctest::Approx(A.y1 - 4.0f));
  CHECK(panel::test::delayY(r, hi) == doctest::Approx(A.y0 + 4.0f));
  // The unwrapped phase keeps every turn (it took the principal step between neighbours before, losing a turn wherever
  // the phase moved more than 180° per point): a fine-grid unwrap of the same design, 171 points per strip point.
  const uint32_t sub = 171, m = (n - 1) * sub + 1;
  double prev = 0.0, acc = 0.0;
  uint32_t phaseBad = 0;
  for (uint32_t i = 0; i < m; ++i) {
    const double hz = i == 0 ? panel::test::responseHz(0) : panel::test::axisHz(static_cast<double>(i) / (m - 1));
    const double ph = std::arg(dsp::svfResponse(sp, 2.0 * 3.141592653589793 * hz / sr));
    acc = i == 0 ? ph : acc + std::remainder(ph - prev, 2.0 * 3.141592653589793);
    prev = ph;
    if (i % sub != 0) continue;
    const uint32_t k = i / sub;
    const double want = acc * 180.0 / 3.141592653589793;
    const float got = panel::test::phaseAt(r, xOf(k));
    if (!(std::fabs(got - want) <= 0.5) && phaseBad++ < 5) MESSAGE("phase at point " << k << ": drawn " << got << "°, reference " << want << "°");
  }
  CHECK(phaseBad == 0);
  CHECK(acc * 180.0 / 3.141592653589793 < -22.0 * 360.0);  // (23 turns from DC to Nyquist: almost all of them by 20 kHz)
}

TEST_CASE("EQ.4d delay: a cut bell's true negative group delay is drawn below the 0 ms floor, in at most a quarter of the area") {
  Alone a("eq_band");
  const doc::BandInfo b = *doc::bandInfo(*a.ctx.doc(), a.group);
  setUnits(a.st(), b.gain, -12.0);
  setUnits(a.st(), b.q, 2.0);
  a.show(1.0f);
  panel::Element& r = a.element("response");
  a.face().target().setViewValue(r.id(), "delay", 1.0);
  (void)a.render();
  // The reference: the band's own response (a fresh lowering), a central difference over ±10⁻⁴ of the grid step.
  doc::LowerOptions lo;
  lo.library = &devLib().library();
  lo.devices = &devLib();
  lo.clipSlotOf = [](doc::TrackId) { return 0u; };
  auto low = doc::lower(*a.ctx.doc(), lo);
  REQUIRE(low.ok());
  doc::GroupResponse gr;
  REQUIRE(gr.prepare(*a.ctx.doc(), *low, devLib(), a.group).ok());
  const uint32_t n = panel::test::responsePoints();
  std::vector<double> hz(2 * n);
  std::vector<double> eps(n);
  for (uint32_t k = 0; k < n; ++k) {
    const double f = panel::test::responseHz(k), step = panel::test::responseHz(std::min(k + 1, n - 1)) - panel::test::responseHz(k == n - 1 ? k - 1 : k);
    eps[k] = 1e-4 * std::fabs(step);
    hz[2 * k] = f - eps[k];
    hz[2 * k + 1] = f + eps[k];
  }
  const size_t stride = gr.inChannels() * gr.outChannels();
  std::vector<Cx> h(hz.size() * stride);
  gr.evaluate(hz, a.ctx.sampleRate(), h);
  double minWant = 0.0;
  float minGot = 0.0f;
  uint32_t bad = 0;
  for (uint32_t k = 1; k < n; ++k) {
    const double want = -std::arg(h[(2 * k + 1) * stride] * std::conj(h[2 * k * stride])) / (2.0 * 3.141592653589793 * 2.0 * eps[k]) * 1000.0;
    const float got = panel::test::delayAt(r, r.desc().w * static_cast<float>(k) / static_cast<float>(n - 1));
    if (!(std::fabs(got - want) <= 2e-3 * std::fabs(want) + 1e-4) && bad++ < 5) MESSAGE("point " << k << ": drawn " << got << ", reference " << want);
    minWant = std::min(minWant, want);
    minGot = std::min(minGot, got);
  }
  CHECK(bad == 0);
  MESSAGE("most negative delay: reference " << minWant << " ms, drawn " << minGot << " ms");
  REQUIRE(minWant < -0.01);  // (a real negative delay: a minimum-phase cut near its centre)
  CHECK(minGot < 0.0f);
  // The floor rises to make room for it: the most negative value at the bottom, 0 at most a quarter of the area up.
  const panel::test::ResponseArea A = *panel::test::area(r, 2);
  const auto [lo2, hi2] = panel::test::delayScale(r);
  CHECK(lo2 == doctest::Approx(minGot));
  CHECK(hi2 > 0.0);
  const float floorY = panel::test::delayY(r, 0.0), bottom = A.y1 - 4.0f;
  CHECK(floorY < bottom);
  CHECK(bottom - floorY <= 0.25f * (A.y1 - A.y0 - 8.0f) + 1e-3f);
  CHECK(panel::test::delayY(r, lo2) == doctest::Approx(bottom));
}

TEST_CASE("EQ.4d views: magnitude, phase and delay open and close by their title chips (real dispatch); all 8 stack in order") {
  EqUi u;
  panel::Element& r = u.element("response");
  using Chip = panel::test::ResponseChip;
  const Chip title[3] = {Chip::Magnitude, Chip::Phase, Chip::Delay};
  u.d.step();
  const size_t h0 = u.history();
  // The default: magnitude and phase open, delay a bar under them.
  CHECK(panel::test::area(r, 0)->open);
  CHECK(panel::test::area(r, 1)->open);
  CHECK(!panel::test::area(r, 2)->open);
  const float h = r.desc().h;
  for (uint32_t m = 0; m < 8; ++m) {
    CAPTURE(m);
    for (int32_t a = 0; a < 3; ++a) {
      const bool want = ((m >> a) & 1u) != 0;
      if (panel::test::area(r, a)->open != want) {
        u.d.click(u.at("response", panel::test::chip(r, title[a])));
        u.d.step();
      }
      REQUIRE(panel::test::area(r, a)->open == want);
    }
    // Top to bottom, no overlap; a closed area a 15-unit bar; the open ones fill the rest (the element keeps its size).
    float y = 0.0f;
    bool anyOpen = false;
    for (int32_t a = 0; a < 3; ++a) {
      const panel::test::ResponseArea ar = *panel::test::area(r, a);
      CHECK(ar.open == (((m >> a) & 1u) != 0));
      CHECK(ar.y0 >= y - 1e-3f);
      CHECK(ar.y1 > ar.y0);
      if (!ar.open) CHECK(ar.y1 - ar.y0 == doctest::Approx(15.0f));
      else CHECK(ar.y1 - ar.y0 > 15.0f);
      anyOpen = anyOpen || ar.open;
      y = ar.y1;
    }
    CHECK(y <= h + 1e-3f);
    if (anyOpen) CHECK(y == doctest::Approx(h));
  }
  // A click on a closed bar anywhere along it opens it (the delay's, at its right end).
  REQUIRE(panel::test::area(r, 2)->open);
  u.d.click(u.at("response", panel::test::chip(r, Chip::Delay)));
  u.d.step();
  REQUIRE(!panel::test::area(r, 2)->open);
  const panel::test::ResponseArea bar = *panel::test::area(r, 2);
  u.d.click(u.at("response", {r.desc().w - 20.0f, (bar.y0 + bar.y1) * 0.5f}));
  u.d.step();
  CHECK(panel::test::area(r, 2)->open);
  CHECK(u.history() == h0);  // (view state: no document edit)
}

TEST_CASE("render: eq-default, eq-eight-bands, eq-lr-mode, eq-phase-delay, eq-all-views (EQ.4, 2x)") {
  {
    Alone a;
    a.show();
    checkImage("eq-default", a.render());
  }
  {  // Eight varied bands: a 16-stage low cut, a Q 40 bell, a notch, a 2-stage high cut; a spectrum behind; one selected.
    Alone a;
    doc::DocumentStore& st = a.st();
    for (const auto& [type, hz, db] : std::vector<std::tuple<uint32_t, double, double>>{{1, 40.0, 0.0}, {4, 1200.0, 8.0}, {3, 3200.0, 0.0}, {0, 17000.0, 0.0}})
      run(st, doc::ops::addBand(*a.ctx.doc(), st.ids(), a.group, 0, type, hz, db));
    const doc::EqInfo e = a.info();
    REQUIRE(e.bands[0].size() == 8);
    auto bandAt = [&](double hz) {
      for (const doc::BandInfo& b : e.bands[0])
        if (std::fabs(units(*a.ctx.doc(), b.frequency) - hz) < 1.0) return b;
      FAIL("no band at " << hz);
      return doc::BandInfo{};
    };
    run(st, doc::ops::setBandStages(*a.ctx.doc(), bandAt(40.0).group, 16));
    setUnits(st, bandAt(1200.0).q, 40.0);
    setUnits(st, bandAt(100.0).gain, 3.0);
    setUnits(st, bandAt(400.0).gain, -4.5);
    setUnits(st, bandAt(400.0).q, 1.0);
    setUnits(st, bandAt(2500.0).gain, 2.5);
    setUnits(st, bandAt(8000.0).gain, -3.0);
    setUnits(st, bandAt(3200.0).q, 3.0);
    run(st, doc::ops::setBandStages(*a.ctx.doc(), bandAt(17000.0).group, 2));
    a.show();
    a.face().target().selectGroup(bandAt(1200.0).group);
    CHECK(panel::test::showSpectrum(a.element("response"), fakeSpectrum()));
    checkImage("eq-eight-bands", a.render());
  }
  {  // L/R: chain B (a copy with other gains) translucent behind chain A.
    Alone a;
    doc::DocumentStore& st = a.st();
    run(st, doc::ops::setEqMode(*a.ctx.doc(), st.ids(), a.group, doc::EqMode::LeftRight));
    const doc::EqInfo e = a.info();
    REQUIRE(e.bands[1].size() == 4);
    setUnits(st, e.bands[0][1].gain, 6.0);
    setUnits(st, e.bands[0][3].gain, -4.0);
    setUnits(st, e.bands[1][1].gain, -7.0);
    setUnits(st, e.bands[1][2].gain, 5.0);
    a.show();
    checkImage("eq-lr-mode", a.render());
  }
  {  // The delay area open, the phase closed (a 16-stage low cut: its delay peak), the bell selected.
    Alone a;
    doc::DocumentStore& st = a.st();
    const doc::EqInfo e = a.info();
    run(st, doc::ops::setBandType(*a.ctx.doc(), e.bands[0][0].group, 1));
    run(st, doc::ops::setBandStages(*a.ctx.doc(), e.bands[0][0].group, 16));
    setUnits(st, e.bands[0][0].frequency, 60.0);
    setUnits(st, e.bands[0][2].gain, 9.0);
    setUnits(st, e.bands[0][2].q, 3.0);
    a.show();
    panel::Element& r = a.element("response");
    a.face().target().setViewValue(r.id(), "delay", 1.0);
    a.face().target().setViewValue(r.id(), "phase", 0.0);
    a.face().target().selectGroup(e.bands[0][2].group);
    checkImage("eq-phase-delay", a.render());
  }
  {  // EQ.4d: all three areas: the bug report's 23-stage Q 5.16 allpass at 1.27 kHz (a tall delay peak, no fold) and a
     // cut bell (its true negative delay below the floor), the allpass selected.
    Alone a;
    doc::DocumentStore& st = a.st();
    const doc::EqInfo e = a.info();
    run(st, doc::ops::setBandType(*a.ctx.doc(), e.bands[0][2].group, 7));
    run(st, doc::ops::setBandStages(*a.ctx.doc(), e.bands[0][2].group, 23));
    setUnits(st, e.bands[0][2].frequency, 1270.0);
    setUnits(st, e.bands[0][2].q, 5.16);
    setUnits(st, e.bands[0][1].gain, -12.0);
    setUnits(st, e.bands[0][1].q, 2.0);
    a.show();
    panel::Element& r = a.element("response");
    a.face().target().setViewValue(r.id(), "delay", 1.0);
    a.face().target().selectGroup(e.bands[0][2].group);
    checkImage("eq-all-views", a.render());
  }
}

TEST_CASE("EQ.4 perf: a drag on a 32-band EQ re-evaluates and draws the curve within a frame" * doctest::test_suite("perf")) {
  Alone a;
  doc::DocumentStore& st = a.st();
  for (int i = 0; i < 27; ++i)
    run(st, doc::ops::addBand(*a.ctx.doc(), st.ids(), a.group, 0, static_cast<uint32_t>(i % 8), 30.0 * std::pow(1.2, i), (i % 5) - 2.0));
  REQUIRE(a.info().bands[0].size() == 31);
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  a.show(1.0f);  // (the lowering and the preparation of the chain and its 32 bands)
  const double prepareMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  panel::Element& r = a.element("response");
  panel::PanelTarget& t = a.face().target();
  const doc::BandInfo b = a.info().bands[0][16];
  const doc::Binding fb = doc::controlBinding(b.frequency, std::string(doc::kValueParam));
  DrawList dl;
  dl.reset(4096, 4096);
  FacePainter p(dl, {{0.0f, 0.0f}, 1.0f});
  panel::ElementEnv env;
  env.theme = a.d->resources().theme;
  REQUIRE(env.theme != nullptr);
  env.target = &t;
  r.paintLive(p, env);  // warm
  CHECK(panel::test::drawsSeries(r));
  t.begin(fb, "Drag");
  double worst = 0.0, sum = 0.0;
  const int n = 40;
  for (int i = 0; i < n; ++i) {
    t.setPosition(fb, 0.3 + 0.01 * i);  // one drag step: a param write …
    dl.reset(4096, 4096);
    const auto s = Clock::now();
    r.paintLive(p, env);  // … then the display's frame: the curves re-read, re-evaluated, drawn
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - s).count();
    worst = std::max(worst, ms);
    sum += ms;
  }
  t.end();
  // EQ.4d: the same drag with all three areas open (the delay area's second evaluation, on the shifted grid).
  t.setViewValue(r.id(), "delay", 1.0);
  r.paintLive(p, env);  // warm
  t.begin(fb, "Drag");
  double worstAll = 0.0, sumAll = 0.0;
  for (int i = 0; i < n; ++i) {
    t.setPosition(fb, 0.3 + 0.01 * i);
    dl.reset(4096, 4096);
    const auto s = Clock::now();
    r.paintLive(p, env);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - s).count();
    worstAll = std::max(worstAll, ms);
    sumAll += ms;
  }
  t.end();
  t.setViewValue(r.id(), "delay", 0.0);
  // EQ.4b: the frame after a structural edit (a 32nd band added). Before: the display lowered the project on the UI
  // thread and prepared every group again in that frame. After: the app's compile lowers; the frame after the edit
  // draws the curve it has (the new band waits), the frame its lowering arrives in prepares the groups.
  auto frame = [&] {
    (void)a.face().sync();
    panel::Element& rr = a.element("response");
    dl.reset(4096, 4096);
    const auto s0 = Clock::now();
    rr.paintLive(p, env);
    return std::chrono::duration<double, std::milli>(Clock::now() - s0).count();
  };
  auto addOne = [&] { run(st, doc::ops::addBand(*a.ctx.doc(), st.ids(), a.group, 0, 4, 9000.0, 3.0)); };
  addOne();
  const double beforeMs = frame();  // (no app: lowered here)
  REQUIRE(a.ctx.undo());
  (void)frame();
  a.ctx.setLoweringProvided(true);
  auto compile = [&] {
    doc::LowerOptions lo;
    lo.library = &devLib().library();
    lo.devices = &devLib();
    lo.clipSlotOf = [](doc::TrackId) { return 0u; };
    const auto l0 = Clock::now();
    auto low = doc::lower(*a.ctx.doc(), lo);
    REQUIRE(low.ok());
    a.ctx.setLowering(a.ctx.docSerial(), a.ctx.doc(), std::make_shared<const doc::LoweredProject>(std::move(*low)));
    return std::chrono::duration<double, std::milli>(Clock::now() - l0).count();
  };
  (void)compile();
  (void)frame();
  const uint32_t lowerings = a.ctx.groupResponses().lowerings();
  addOne();
  const double afterMs = frame();  // (the app's compile not here yet: the curve as it was)
  CHECK(a.ctx.groupResponses().waiting());
  const double lowerMs = compile();  // (the app's compile thread)
  const double arrivalMs = frame();
  CHECK(a.ctx.groupResponses().lowerings() == lowerings);
  MESSAGE("32-band EQ: first paint " << prepareMs << " ms; a drag frame: mean " << sum / n << " ms, worst " << worst
          << " ms (all three areas open: mean " << sumAll / n << " ms, worst " << worstAll << " ms)"
          << "; the frame after addBand: before (lowered on the UI thread) " << beforeMs << " ms, after (the app's compile) "
          << afterMs << " ms, the frame its lowering arrives in " << arrivalMs << " ms (the lowering, off the UI thread: " << lowerMs << " ms)");
#if defined(PQ_OPTIMIZED)
  CHECK(sum / n < 2.0);
  CHECK(sumAll / n < 2.0);
#endif
}

