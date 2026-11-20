// bug-mod-ghost (pq/views/live_modulation.h; element.h "ghost"; meta_face.h Face::paintGhosts): every control that shows
// a param live modulation moves draws a translucent GHOST of itself at the modulated value, the solid control at the
// knob; a response display draws the response there as a silhouette (test_eq_displays.cpp).
//   generic    an element kind registered here, which knows nothing of ghosts, gets its ghost pass (its position() the
//              modulated one) — knobs and sliders too; a stopped source clears every ghost
//   polling    the PanelHost reads the modulation at its frame ticks while it is live and a modulated control is shown
//              (real timers: a new value repaints the ghost with no notify), one source read per frame for every
//              panel; nothing is read with no wire or while the source sleeps
//   meta       every native engine face (MetaFace::paintGhosts) paints its ghost pass when a param has a ghost, nothing
//              otherwise
//   goldens    (2x) panel-mod-ghosts (the panel project's knobs in four pointer styles and its sliders, modulated),
//              eq-mod-ghost (an EQ band's Frequency modulated: the silhouette)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/document/panel.h"
#include "pq/meta/registry.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/theme.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/fake_transport.h"
#include "pq/views/live_modulation.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/panel/element.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/panel/response_display.h"
#include "pq/views/project_patch/project_patch_view.h"
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

// The clock the modulation's once-per-frame read goes by (MetaFace::setClock): the test's.
double gNow = 1000.0;
double testNow() { return gNow; }

// ---- an element kind that knows nothing about ghosts ------------------------------------------------------------------

struct ProbeLog {
  std::vector<double> solid, ghost;  // the position each pass read
};
ProbeLog& probeLog() {
  static ProbeLog l;
  return l;
}

// It declares its value (one Control binding) and draws a disc where it reads it: nothing else.
class Probe final : public panel::Element {
public:
  using Element::Element;
  void paintLive(FacePainter& p, const panel::ElementEnv& env) override {
    const doc::Binding* b = binding();
    if (b == nullptr) return;
    const double v = env.target->position(*b);
    (env.ghost ? probeLog().ghost : probeLog().solid).push_back(v);
    p.disc({desc_.w * static_cast<float>(v), desc_.h * 0.5f}, 3.0f, env.theme->face.value);
  }
};
constexpr std::string_view kProbeSlots[] = {"value"};
void registerProbe() {
  static const bool done = [] {
    panel::ElementSchema s;
    s.kind = "test-ghost-probe";
    s.label = "Ghost Probe";
    s.slots = kProbeSlots;
    return panel::registerElement(s, [](const doc::PanelElement& d) -> std::unique_ptr<panel::Element> {
      return std::make_unique<Probe>(d);
    });
  }();
  REQUIRE(done);
}

// The GU.3 panel project (knobs in four styles, sliders, …) in a PanelHost, with an LFO's cables into Level, Cutoff,
// Drive and Mix, a probe element bound to Level, and a fixed modulation source.
struct Ghosts {
  FakeTransport port;
  ViewContext ctx{port};
  project_patch::PanelProjectIds ids;
  TestDriver d;
  panel::PanelHost* host = nullptr;
  FixedModulationSource src;
  doc::DeviceId lfo;
  const std::string vp{doc::kValueParam};

  explicit Ghosts(float scale = 1.0f, bool wires = true)
      : d(static_cast<uint32_t>(480 * scale), static_cast<uint32_t>(220 * scale), scale) {
    registerProbe();
    probeLog() = {};
    MetaFace::setClock(&testNow);
    ctx.setDeviceLibrary(&devLib());
    doc::DocumentStore& st = ctx.store();
    REQUIRE(project_patch::buildPanelProject(st, &ids).ok());
    if (wires) {
      lfo = test_blocks::addModule(st, ids.keys, "lfo", doc::ops::kOffPath);
      for (const doc::DeviceId t : {ids.level, ids.cutoff, ids.drive, ids.mix})
        run(st, doc::ops::connectWire(*st.current(), st.ids(), lfo, "y", t, vp));
    }
    {  // the probe, bound to Level, along the panel's bottom edge
      const doc::Device* g = doc::findDevice(*st.current(), ids.group).device;
      REQUIRE(g != nullptr);
      REQUIRE(g->hasPanel());
      auto p = std::make_shared<doc::Panel>(*g->panel);
      doc::PanelElement e;
      e.kind = "test-ghost-probe";
      e.x = 250.0f, e.y = 196.0f, e.w = 100.0f, e.h = 18.0f;
      e.bindings.push_back(doc::controlBinding(ids.level, vp));
      e.id = 0;
      for (const doc::PanelElement& x : p->elements) e.id = std::max(e.id, x.id + 1);
      p->elements.push_back(std::move(e));
      run(st, doc::ops::setGroupPanel(*st.current(), ids.group, p));
    }
    ctx.setModulationSource(&src);
    auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, ids.group),
                                                        panel::makeDocPanelTarget(ctx, ids.group, [this]() -> Widget* { return host; }));
    host = &d.setRoot<panel::PanelHost>("panel", std::move(face));
    d.tree().layoutNow();
    REQUIRE(d.enableGpu().ok());
    d.step();
  }
  ~Ghosts() {
    d.tree().setRoot(std::make_unique<Widget>());
    ctx.setModulationSource(nullptr);
    MetaFace::setClock(nullptr);
  }
  // A frame later: the modulation read now (as a host's frame tick would).
  void read() {
    gNow += 0.02;
    (void)ctx.liveModulation().refresh();
  }
  // `s` seconds of the host's clock and the modulation's.
  void advance(double s) {
    gNow += s;
    d.advance(s);
  }
  panel::GroupPanelFace& face() const { return *host->panelFace(); }
  double knob(doc::DeviceId block) const { return face().target().position(doc::controlBinding(block, vp)); }
  // The source has every modulated param `delta` (knob position) off its knob now.
  void modulate(float delta, bool live = true) {
    for (const doc::DeviceId t : {ids.level, ids.cutoff, ids.drive, ids.mix})
      src.set(t, vp, ModSweep{true, std::max(delta, 0.0f), std::max(-delta, 0.0f), delta});
    src.setLive(live);
  }
  void repaint() {
    host->notifyChanged();
    d.step();
  }
  Image render() {
    REQUIRE(d.enableGpu().ok());
    d.advance(0.1);
    auto img = d.render();
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    return std::move(*img);
  }
};

}  // namespace

TEST_CASE("bug-mod-ghost: an element kind that knows nothing of ghosts gets its ghost at the modulated position; knobs and "
          "sliders too; the stopped source clears them") {
  Ghosts g;
  const double level = g.knob(g.ids.level);
  // Idle (the source asleep): no ghost, no read of the source.
  probeLog() = {};
  g.repaint();
  CHECK(g.face().stats().ghosts == 0);
  CHECK(probeLog().ghost.empty());
  REQUIRE_FALSE(probeLog().solid.empty());
  CHECK(probeLog().solid.back() == doctest::Approx(level));
  // Live: +0.15 of the travel now (the tap's latest value; Level's linear 0 … 1 curve: its units are its travel).
  g.modulate(0.15f);
  g.read();
  probeLog() = {};
  g.repaint();
  REQUIRE_FALSE(probeLog().ghost.empty());
  CHECK(probeLog().ghost.back() == doctest::Approx(std::min(1.0, level + 0.15)).epsilon(1e-5));
  CHECK(probeLog().solid.back() == doctest::Approx(level));  // the solid one at the knob
  // Every element showing a modulated control has its ghost: the four knobs, the two sliders, the probe (the number
  // box, the button and the curve tab's editor show them too: at least 7).
  const panel::GroupPanelFace& f = g.face();
  uint32_t knobs = 0, sliders = 0;
  for (size_t i = 0; i < f.elementCount(); ++i) {
    if (!f.shown(i) || !f.element(i).ghosted(f.target())) continue;
    knobs += f.element(i).desc().kind == "knob";
    sliders += f.element(i).desc().kind == "slider";
  }
  CHECK(knobs >= 4);
  CHECK(sliders == 2);
  CHECK(f.stats().ghosts >= 7);
  // A ghost pass edits nothing (its target's edits do nothing): the document is as it was.
  CHECK(g.knob(g.ids.level) == doctest::Approx(level));
  // Stopped: the next read clears them (the engine's taps read 0 while it sleeps).
  g.modulate(0.15f, false);
  g.read();
  probeLog() = {};
  g.repaint();
  CHECK(g.face().stats().ghosts == 0);
  CHECK(probeLog().ghost.empty());
}

TEST_CASE("bug-mod-ghost polling: while live the panel's frame ticks move its ghosts (no notify), one read per frame; "
          "nothing reads without a wire or while the source sleeps") {
  SUBCASE("live") {
    Ghosts g;
    const double level = g.knob(g.ids.level);
    g.modulate(0.1f);
    g.host->wake();  // (KB.3: kSignal when the engine wakes)
    g.advance(0.05);
    REQUIRE_FALSE(probeLog().ghost.empty());
    CHECK(probeLog().ghost.back() == doctest::Approx(level + 0.1).epsilon(1e-5));
    // A new value arrives: the host's timer reads it and repaints the ghost — no notifyChanged, no document change.
    g.modulate(-0.2f);
    const uint64_t reads = g.ctx.liveModulation().reads();
    probeLog() = {};
    g.advance(0.1);
    REQUIRE_FALSE(probeLog().ghost.empty());
    CHECK(probeLog().ghost.back() == doctest::Approx(std::max(0.0, level - 0.2)).epsilon(1e-5));
    const uint64_t frames = g.ctx.liveModulation().reads() - reads;
    CHECK(frames >= 1);
    CHECK(frames <= 13);  // ≤ one read per 1/120 s whoever asks (here 0.1 s of 60 Hz ticks)
    // Stopped: one more read clears the ghost, then the polling ends.
    g.src.setLive(false);
    g.advance(0.1);
    CHECK(g.face().stats().ghosts == 0);
    const uint64_t after = g.ctx.liveModulation().reads();
    g.advance(0.3);
    CHECK(g.ctx.liveModulation().reads() == after);
  }
  SUBCASE("no wire") {
    Ghosts g(1.0f, false);
    g.modulate(0.1f);  // (values for params no wire drives)
    g.host->wake();
    g.advance(0.3);
    CHECK(g.ctx.liveModulation().reads() == 0);
    CHECK(g.face().stats().ghosts == 0);
    CHECK_FALSE(g.face().ghostsLive());
  }
  SUBCASE("asleep") {
    Ghosts g;
    g.modulate(0.1f, false);
    g.host->wake();
    g.advance(0.3);
    CHECK(g.ctx.liveModulation().reads() == 0);
    CHECK(g.src.nowReads() == 0);
    CHECK_FALSE(g.face().ghostsLive());
  }
}

namespace {
// A meta target with a ghost on one param (its stored values the type's defaults).
class GhostOne final : public MetaTarget {
public:
  GhostOne(const meta::Type& t, int32_t param) : type_(t), param_(param) {}
  double value(size_t i) const override { return i < type_.params.size() ? type_.params[i].def : 0.0; }
  void begin(const char*) override {}
  void set(size_t, double) override {}
  void end() override {}
  void cancel() override {}
  void reset(size_t) override {}
  const meta::Type* targetType() const noexcept override { return &type_; }
  std::optional<double> ghostPosition(size_t i) const override {
    if (static_cast<int32_t>(i) != param_) return std::nullopt;
    const double pos = faceValueToTravel(*this, type_, i, value(i));
    return pos < 0.5 ? pos + 0.3 : pos - 0.3;
  }
  bool modulated(size_t i) const override { return static_cast<int32_t>(i) == param_; }

private:
  const meta::Type& type_;
  int32_t param_;
};
}  // namespace

TEST_CASE("bug-mod-ghost meta faces: every native engine face paints its ghost pass while a param has a ghost, nothing "
          "otherwise") {
  auto theme = loadTheme(defaultThemePath());
  REQUIRE(theme.ok());
  FaceEnv env;
  env.theme = &*theme;
  uint32_t faces = 0;
  for (const meta::Type* t : meta::types()) {
    if (!hasFace(*t) || t->params.empty()) continue;
    CAPTURE(t->id);
    MESSAGE("a native face with params: " << t->id << " (" << t->params.size() << ")");
    ++faces;
    for (int32_t param : {-1, 0, static_cast<int32_t>(t->params.size()) - 1}) {
      CAPTURE(param);
      std::unique_ptr<MetaFace> f = makeFace(*t);
      REQUIRE(f != nullptr);
      f->bind(std::make_unique<GhostOne>(*t, param), nullptr);
      DrawList dl;
      dl.reset(1024, 1024);
      FacePainter p(dl, {{0.0f, 0.0f}, 2.0f});
      f->paintLive(p, env);
      const size_t solid = dl.vertices().size();
      f->paintGhosts(p, env);
      CHECK(f->hasGhost() == (param >= 0));
      if (param < 0) CHECK(dl.vertices().size() == solid);
      else CHECK(dl.vertices().size() > solid);
    }
  }
  // Wavetable, Envelope, Noise (the analysers — Spectrum, Oscilloscope, Spectrogram — have settings, no params: nothing a
  // wire can drive).
  CHECK(faces >= 3);
}

TEST_CASE("render: panel-mod-ghosts (2x) — the panel project's knobs and sliders with their ghosts") {
  Ghosts g(2.0f);
  g.modulate(0.22f);
  g.read();
  g.repaint();
  checkImage("panel-mod-ghosts", g.render());
}

TEST_CASE("render: eq-mod-ghost (2x) — an EQ band's Frequency modulated: its node and curve at the knob, the silhouette "
          "where the modulation has it") {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  doc::DocumentStore& st = ctx.store();
  const doc::TrackId track = run(st, doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "Audio")).tracks.at(0);
  const doc::DeviceId eq = test_blocks::addModule(st, track, "eq");
  const std::optional<doc::EqInfo> e = doc::eqInfo(*st.current(), eq);
  REQUIRE(e.has_value());
  const doc::BandInfo band = e->bands[0][1];  // the 400 Hz bell, +9 dB
  {
    const doc::Device* gd = doc::findDevice(*st.current(), band.gain).device;
    const dsp::Curve* c = gd->curve(doc::kValueParam);
    REQUIRE(c != nullptr);
    run(st, doc::ops::setDeviceParam(*st.current(), band.gain, std::string(doc::kValueParam), dsp::curvePosition(*c, dsp::curveOutput(*c, 9.0), 0.5)));
  }
  const doc::DeviceId lfo = test_blocks::addModule(st, track, "lfo", doc::ops::kOffPath);
  run(st, doc::ops::connectWire(*st.current(), st.ids(), lfo, "y", band.frequency, std::string(doc::kValueParam)));
  const doc::Device* fd = doc::findDevice(*st.current(), band.frequency).device;
  const dsp::Curve fc = *fd->curve(doc::kValueParam);
  const double at400 = dsp::curvePosition(fc, dsp::curveOutput(fc, 400.0), 0.5);
  const double at2k5 = dsp::curvePosition(fc, dsp::curveOutput(fc, 2500.0), 0.5);
  FixedModulationSource src;
  const float d = static_cast<float>(at2k5 - at400);
  // (the tap of an Add wire into a curved control is in knob position: Domain::Position)
  src.set(band.frequency, std::string(doc::kValueParam), ModSweep{true, d, 0.0f, d});
  src.setLive(true);
  ctx.setModulationSource(&src);
  (void)ctx.liveModulation().refresh();
  const doc::Panel* p = doc::findDevice(*st.current(), eq).device->panel.get();
  REQUIRE(p != nullptr);
  constexpr float kScale = 2.0f;
  TestDriver drv(static_cast<uint32_t>(p->width * kScale), static_cast<uint32_t>(p->height * kScale), kScale);
  panel::PanelHost* host = nullptr;
  auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, eq),
                                                      panel::makeDocPanelTarget(ctx, eq, [&]() -> Widget* { return host; }));
  host = &drv.setRoot<panel::PanelHost>("panel", std::move(face));
  drv.tree().layoutNow();
  REQUIRE(drv.enableGpu().ok());
  drv.step();
  host->notifyChanged();
  drv.advance(0.1);
  // The silhouette peaks at 2.5 kHz, the solid curve at 400 Hz.
  panel::GroupPanelFace& gf = *host->panelFace();
  const panel::Element* r = nullptr;
  for (size_t i = 0; i < gf.elementCount(); ++i)
    if (gf.element(i).desc().kind == "response") r = &gf.element(i);
  REQUIRE(r != nullptr);
  auto peak = [&](bool ghost) {
    float best = -1e9f;
    double hz = 0.0;
    for (uint32_t k = 0; k < panel::test::responsePoints(); ++k) {
      const float x = panel::test::responseX(*r, k);
      const float v = *(ghost ? panel::test::ghostDb(*r, x) : panel::test::drawnDb(*r, x));
      if (v > best) best = v, hz = panel::test::responseHz(k);
    }
    return hz;
  };
  CHECK(peak(false) == doctest::Approx(400.0).epsilon(0.03));
  CHECK(peak(true) == doctest::Approx(2500.0).epsilon(0.03));
  // BR.A (bug report 2026-11-19_19-35-48): the phase area follows the modulation too — a bell's phase is 0° at its
  // centre: the solid curve's at 400 Hz, the silhouette's at 2.5 kHz (≈ −13° there at the knob).
  const std::optional<panel::test::ResponseArea> ph = panel::test::area(*r, 1);
  REQUIRE(ph.has_value());
  REQUIRE(ph->open);
  auto xAt = [&](double hz) {
    uint32_t best = 0;
    for (uint32_t k = 0; k < panel::test::responsePoints(); ++k)
      if (std::fabs(std::log(panel::test::responseHz(k) / hz)) < std::fabs(std::log(panel::test::responseHz(best) / hz))) best = k;
    return panel::test::responseX(*r, best);
  };
  CHECK(std::fabs(panel::test::phaseAt(*r, xAt(400.0))) < 3.0f);
  CHECK(std::fabs(panel::test::phaseAt(*r, xAt(2500.0))) > 6.0f);
  CHECK(std::fabs(panel::test::ghostPhaseAt(*r, xAt(2500.0))) < 3.0f);
  CHECK(std::fabs(panel::test::ghostPhaseAt(*r, xAt(400.0))) > 6.0f);
  auto img = drv.render();
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  checkImage("eq-mod-ghost", *img);
  drv.tree().setRoot(std::make_unique<Widget>());
  ctx.setModulationSource(nullptr);
}

TEST_CASE("BR.A: a driven band's curve follows the position its control SHOWS (an automated knob at its envelope's value "
          "while playing), not only the stored one") {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  doc::DocumentStore& st = ctx.store();
  const doc::TrackId track = run(st, doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "Audio")).tracks.at(0);
  const doc::DeviceId eq = test_blocks::addModule(st, track, "eq");
  const std::optional<doc::EqInfo> e = doc::eqInfo(*st.current(), eq);
  REQUIRE(e.has_value());
  const doc::BandInfo band = e->bands[0][1];  // the 400 Hz bell
  {
    const doc::Device* gd = doc::findDevice(*st.current(), band.gain).device;
    const dsp::Curve* c = gd->curve(doc::kValueParam);
    REQUIRE(c != nullptr);
    run(st, doc::ops::setDeviceParam(*st.current(), band.gain, std::string(doc::kValueParam), dsp::curvePosition(*c, dsp::curveOutput(*c, 9.0), 0.5)));
  }
  // A wire makes Frequency a driven control (as a lane does); no modulation source: no ghost.
  const doc::DeviceId lfo = test_blocks::addModule(st, track, "lfo", doc::ops::kOffPath);
  run(st, doc::ops::connectWire(*st.current(), st.ids(), lfo, "y", band.frequency, std::string(doc::kValueParam)));
  const dsp::Curve fc = *doc::findDevice(*st.current(), band.frequency).device->curve(doc::kValueParam);
  const doc::Panel* p = doc::findDevice(*st.current(), eq).device->panel.get();
  REQUIRE(p != nullptr);
  TestDriver drv(static_cast<uint32_t>(p->width), static_cast<uint32_t>(p->height), 1.0f);
  panel::PanelHost* host = nullptr;
  auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, eq),
                                                      panel::makeDocPanelTarget(ctx, eq, [&]() -> Widget* { return host; }));
  host = &drv.setRoot<panel::PanelHost>("panel", std::move(face));
  drv.tree().layoutNow();
  REQUIRE(drv.enableGpu().ok());
  drv.step();
  panel::GroupPanelFace& gf = *host->panelFace();
  const panel::Element* r = nullptr;
  for (size_t i = 0; i < gf.elementCount(); ++i)
    if (gf.element(i).desc().kind == "response") r = &gf.element(i);
  REQUIRE(r != nullptr);
  auto peak = [&] {
    host->notifyChanged();
    drv.advance(0.1);
    float best = -1e9f;
    double hz = 0.0;
    for (uint32_t k = 0; k < panel::test::responsePoints(); ++k) {
      const float v = *panel::test::drawnDb(*r, panel::test::responseX(*r, k));
      if (v > best) best = v, hz = panel::test::responseHz(k);
    }
    return hz;
  };
  CHECK(peak() == doctest::Approx(400.0).epsilon(0.03));
  doc::Binding freq;
  freq.kind = doc::BindingKind::Control;
  freq.device = band.frequency;
  freq.name = std::string(doc::kValueParam);
  CHECK(gf.target().showPosition(freq, dsp::curvePosition(fc, dsp::curveOutput(fc, 2500.0), 0.5)));
  CHECK(peak() == doctest::Approx(2500.0).epsilon(0.03));
  CHECK(gf.target().showPosition(freq, std::nullopt));  // the stored one again
  CHECK(peak() == doctest::Approx(400.0).epsilon(0.03));
  drv.tree().setRoot(std::make_unique<Widget>());
}
