// EQ.4b: the EQ's response displays in the app's wiring (a ViewContext on the ProjectLink, the Shell, the Device view
// and the project circuit view).
//   circuit    the drawn total stays the chain's GroupResponse after the EQ is opened and closed in the circuit view
//   lowering   the displays draw from the link's own lowering: no lowering on the UI thread; after a band is added the
//              curve is the one it had until the link's compile of the new structure, then includes the band
#include <doctest/doctest.h>

#include <cmath>
#include <complex>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/app/project_link.h"
#include "pq/document/edit_ops.h"
#include "pq/document/group_response.h"
#include "pq/document/lower.h"
#include "pq/document/modules.h"
#include "pq/ui/test_driver.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/panel/group_responses.h"
#include "pq/views/panel/response_display.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

using namespace pq;
using Cx = std::complex<double>;

namespace {

void run(doc::DocumentStore& st, const doc::ops::EditResult& e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(st.dispatch(e->command).ok());
}

struct EqApp {
  std::unique_ptr<app::ProjectLink> link;
  std::unique_ptr<views::ViewContext> ctx;
  ui::TestDriver d{2600, 900};
  std::unique_ptr<views::Shell> shell;
  doc::TrackId track;
  doc::DeviceId eq;

  EqApp() {
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    doc::IdSource ids;
    auto l = app::ProjectLink::create(c, doc::makeDefaultProject(ids));
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
    link = std::move(*l);
    ctx = std::make_unique<views::ViewContext>(*link);
    link->attach(*ctx);
    ctx->setDeviceLibrary(&link->devices());
    doc::DocumentStore& st = ctx->store();
    auto t = doc::ops::createTrack(*ctx->doc(), st.ids(), doc::TrackKind::Audio, {}, "Audio");
    REQUIRE(t.ok());
    REQUIRE(st.dispatch(t->command).ok());
    track = t->tracks[0];
    eq = test_blocks::addModule(st, track, "eq");
    link->flush();
    auto s = views::Shell::create(d.tree(), *ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<ui::Splitter&>(*d.find("main/body")).setPosition(300.0f);
    ctx->setSelectedTrack(track);
    ctx->setDetailVisible(true);
    ctx->setDetailMode(views::DetailMode::Device);
    d.tree().layoutNow();
    REQUIRE(d.enableGpu().ok());
    d.step();
  }
  ~EqApp() {
    d.tree().setRoot(std::make_unique<ui::Widget>());
    shell.reset();
    link->detach();
    ctx.reset();
    link.reset();
  }
  doc::EqInfo info() const { return *doc::eqInfo(*ctx->doc(), eq); }
  void frame() {
    link->flush();
    d.advance(0.5);
    d.tree().layoutNow();
    d.step();
  }
  views::panel::Element* response() {
    views::DeviceChainView* v = views::findDeviceChain(*shell);
    REQUIRE(v != nullptr);
    d.tree().layoutNow();
    views::panel::PanelHost* h = v->controlHost(info().output, doc::kValueParam);
    REQUIRE(h != nullptr);
    views::panel::GroupPanelFace& f = *h->panelFace();
    (void)f.sync();
    for (size_t i = 0; i < f.elementCount(); ++i)
      if (f.element(i).desc().kind == "response") return &f.element(i);
    return nullptr;
  }
  // |H| (dB) of the active chain on a fresh lowering of the current document.
  std::vector<double> want(const std::vector<double>& hz) {
    doc::LowerOptions lo;
    lo.library = &link->devices().library();
    lo.devices = &link->devices();
    lo.clipSlotOf = [](doc::TrackId) { return 0u; };
    auto low = doc::lower(*ctx->doc(), lo);
    REQUIRE(low.ok());
    doc::GroupResponse r;
    REQUIRE(r.prepare(*ctx->doc(), *low, link->devices(), info().chain[0]).ok());
    std::vector<Cx> h(hz.size() * r.inChannels() * r.outChannels());
    r.evaluate(hz, ctx->sampleRate(), h);
    std::vector<double> db(hz.size());
    for (size_t k = 0; k < hz.size(); ++k) db[k] = 20.0 * std::log10(std::max(std::abs(h[k * r.inChannels() * r.outChannels()]), 1e-9));
    return db;
  }
  void checkCurve() {
    frame();
    views::panel::Element* r = response();
    REQUIRE(r != nullptr);
    std::vector<double> hz;
    const std::vector<uint32_t> ks = {60, 200, 400, 523, 700};
    for (const uint32_t k : ks) hz.push_back(views::panel::test::responseHz(k));
    const std::vector<double> w = want(hz);
    for (size_t i = 0; i < ks.size(); ++i) {
      CAPTURE(hz[i]);
      const std::optional<float> got = views::panel::test::drawnDb(*r, views::panel::test::responseX(*r, ks[i]));
      REQUIRE(got.has_value());
      CHECK(*got == doctest::Approx(w[i]).epsilon(1e-4).scale(1.0));
    }
  }
};

void setUnits(doc::DocumentStore& st, doc::DeviceId block, double v) {
  const doc::Device* d = doc::findDevice(*st.current(), block).device;
  REQUIRE(d != nullptr);
  const dsp::Curve* c = d->curve(doc::kValueParam);
  REQUIRE(c != nullptr);
  run(st, doc::ops::setDeviceParam(*st.current(), block, std::string(doc::kValueParam), dsp::curvePosition(*c, dsp::curveOutput(*c, v), 0.5)));
}

}  // namespace

TEST_CASE("EQ.4b app: the EQ's curve stays after the circuit view opens and closes it") {
  EqApp a;
  const doc::EqInfo e = a.info();
  setUnits(a.ctx->store(), e.bands[0][1].gain, 7.5);
  setUnits(a.ctx->store(), e.bands[0][2].gain, -6.0);
  a.checkCurve();
  views::DeviceChainView* dv = views::findDeviceChain(*a.shell);
  REQUIRE(dv != nullptr);
  REQUIRE(dv->openInCircuitView(a.eq));
  auto* pv = views::findProjectPatch(*a.shell);
  REQUIRE(pv != nullptr);
  a.frame();
  a.frame();
  pv->setDeviceExpanded(a.eq, true);
  a.frame();
  a.frame();
  pv->setDeviceExpanded(a.eq, false);
  a.frame();
  a.frame();
  pv->setShown(false, false);
  a.frame();
  a.checkCurve();
}

TEST_CASE("EQ.4b app: the response displays draw from the link's lowering (none on the UI thread); a structural edit waits for it") {
  EqApp a;
  a.checkCurve();
  views::GroupResponses& gr = a.ctx->groupResponses();
  CHECK(a.ctx->loweringProvided());
  const uint32_t n0 = gr.lowerings();
  CHECK(n0 == 0);
  // A band added: until the link compiles, the curve it had (a structural compile waits for flush here).
  const std::vector<double> hz = {views::panel::test::responseHz(600)};
  const std::vector<double> before = a.want(hz);
  run(a.ctx->store(), doc::ops::addBand(*a.ctx->doc(), a.ctx->store().ids(), a.eq, 0, 4, 5000.0, 9.0));
  a.d.advance(0.1);
  a.d.step();
  CHECK(gr.waiting());
  views::panel::Element* r = a.response();
  REQUIRE(r != nullptr);
  const float x = views::panel::test::responseX(*r, 600);
  CHECK(*views::panel::test::drawnDb(*r, x) == doctest::Approx(before[0]).epsilon(1e-4).scale(1.0));
  // The compile: its lowering is handed over; the band is drawn.
  a.checkCurve();
  CHECK_FALSE(gr.waiting());
  CHECK(*views::panel::test::drawnDb(*a.response(), x) > before[0] + 3.0);
  CHECK(gr.lowerings() == n0);
}
