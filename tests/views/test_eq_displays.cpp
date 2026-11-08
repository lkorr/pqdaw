// EQ.4 (docs/plans/EQ.md "Displays (EQ.4)"; response_elements.cpp): the `response` and `phase` displays and the EQ
// panel.
//   curve      the total the response display draws is the chain group's GroupResponse (In 0 → Out 0) at its grid
//              points; a drag re-reads the knobs without lowering again, a structural edit lowers once
//   gestures   (real dispatch: the Shell's Device view hosts the EQ's panel) double-click empty space adds a bell there,
//              a node drags frequency + gain (Q for a gainless type), Shift is fine, the wheel turns Q (a run = one
//              step), Alt + wheel steps Stages, double-click a node resets its gain, right-click: Type / Bypass / Solo /
//              Delete, a click selects, Delete removes the selected band, the Mode menu splits through setEqMode (A
//              copied into B) — each one undo step
//   reuse      the response element on a lone EQ Band's panel draws the band's own response
//   goldens    (2x) eq-default, eq-eight-bands (a 16-stage low cut, a Q 40 bell, a spectrum behind, a band selected),
//              eq-lr-mode (chain B translucent behind A), eq-phase-delay (the phase strip in group delay),
//              eq-band-response (the element on a lone band's panel)
//   perf       a drag on a 32-band EQ re-evaluates and redraws the curve within a frame (< 2 ms, optimised builds)
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "../ui/golden_check.h"
#include "pq/document/devices.h"
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

  EqUi() {
    ctx.setDeviceLibrary(&devLib());
    doc::DocumentStore& st = ctx.store();
    track = run(st, doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "Audio")).tracks.at(0);
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

TEST_CASE("EQ.4 response: the element on a lone EQ Band's panel draws the band's own response (reuse)") {
  Alone a("eq_band");
  const doc::BandInfo b = *doc::bandInfo(*a.ctx.doc(), a.group);
  setUnits(a.st(), b.gain, 9.0);
  setUnits(a.st(), b.q, 2.0);
  // Its panel with a response display above the controls.
  doc::Panel p = *dev(*a.ctx.doc(), a.group).panel;
  for (doc::PanelElement& x : p.elements)
    if (x.number("plate", 0.0) == 0.0) x.y += 112.0f;
  p.height += 112.0f;
  p.width = 240.0f;
  for (doc::PanelElement& x : p.elements)
    if (x.number("plate", 0.0) != 0.0) x.w = p.width, x.h = p.height;
  doc::PanelElement r;
  r.id = p.nextId();
  r.kind = "response";
  r.x = 8, r.y = 8, r.w = p.width - 16.0f, r.h = 104.0f;
  r.bindings = {doc::signalBinding(a.group, "y")};
  p.elements.push_back(r);
  run(a.st(), doc::ops::setGroupPanel(*a.ctx.doc(), a.group, std::make_shared<doc::Panel>(std::move(p))));
  a.show(2.0f);
  panel::Element& e = a.element("response");
  std::vector<double> hz;
  const std::vector<uint32_t> ks = {100, 380, 450, 520, 700};
  for (const uint32_t k : ks) hz.push_back(panel::test::responseHz(k));
  const std::vector<double> want = groupDb(*a.ctx.doc(), a.group, hz, a.ctx.sampleRate());
  for (size_t i = 0; i < ks.size(); ++i)
    CHECK(*panel::test::drawnDb(e, panel::test::responseX(e, ks[i])) == doctest::Approx(want[i]).epsilon(1e-4).scale(1.0));
  CHECK(panel::test::nodeOf(e, a.group).has_value());  // its one node
  checkImage("eq-band-response", a.render());
}

TEST_CASE("render: eq-default, eq-eight-bands, eq-lr-mode, eq-phase-delay (EQ.4, 2x)") {
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
  {  // The phase strip in group delay (a 16-stage low cut: its delay peak), the bell selected.
    Alone a;
    doc::DocumentStore& st = a.st();
    const doc::EqInfo e = a.info();
    run(st, doc::ops::setBandType(*a.ctx.doc(), e.bands[0][0].group, 1));
    run(st, doc::ops::setBandStages(*a.ctx.doc(), e.bands[0][0].group, 16));
    setUnits(st, e.bands[0][0].frequency, 60.0);
    setUnits(st, e.bands[0][2].gain, 9.0);
    setUnits(st, e.bands[0][2].q, 3.0);
    a.show();
    panel::Element& ph = a.element("phase");
    a.face().target().setViewValue(ph.id(), "delay", 1.0);
    a.face().target().selectGroup(e.bands[0][2].group);
    checkImage("eq-phase-delay", a.render());
  }
}

TEST_CASE("EQ.4 perf: a drag on a 32-band EQ re-evaluates and draws the curve within a frame" * doctest::test_suite("perf")) {
  Alone a;
  doc::DocumentStore& st = a.st();
  for (int i = 0; i < 28; ++i)
    run(st, doc::ops::addBand(*a.ctx.doc(), st.ids(), a.group, 0, static_cast<uint32_t>(i % 8), 30.0 * std::pow(1.2, i), (i % 5) - 2.0));
  REQUIRE(a.info().bands[0].size() == 32);
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
  // A structural edit (a band's type): the next frame lowers the project again and prepares the 32 bands (one
  // inference of the track patch).
  run(st, doc::ops::setBandType(*a.ctx.doc(), a.info().bands[0][3].group, 4));
  (void)a.face().sync();
  panel::Element& r2 = a.element("response");
  dl.reset(4096, 4096);
  const auto s0 = Clock::now();
  r2.paintLive(p, env);
  const double structuralMs = std::chrono::duration<double, std::milli>(Clock::now() - s0).count();
  double lowerMs = 0.0;
  {
    doc::LowerOptions lo;
    lo.library = &devLib().library();
    lo.devices = &devLib();
    lo.clipSlotOf = [](doc::TrackId) { return 0u; };
    const auto l0 = Clock::now();
    auto low = doc::lower(*a.ctx.doc(), lo);
    lowerMs = std::chrono::duration<double, std::milli>(Clock::now() - l0).count();
    REQUIRE(low.ok());
  }
  MESSAGE("32-band EQ: first paint " << prepareMs << " ms; the frame after a structural edit " << structuralMs
          << " ms (of it the project's lowering " << lowerMs << " ms; the rest one inference and 32 bands prepared); a drag frame: mean "
          << sum / n << " ms, worst " << worst << " ms");
#if defined(PQ_OPTIMIZED)
  CHECK(sum / n < 2.0);
#endif
}
