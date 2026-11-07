// GU.9 (docs/plans/GU.md "One hierarchy"): a rack (a group of parallel chain groups) in the Device view — its chain list
// (a click shows a chain, "+ Chain" adds one) and its panel of macros (a knob drag moves the macro's value block in one
// undo step) — through REAL pointer dispatch (window → frame → its rows / its panel's element).
#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/test_driver.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/element.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
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

struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  doc::TrackId track;
  doc::DeviceId rack;

  Ui() {
    ctx.setDeviceLibrary(&devLib());
    doc::DocumentStore& st = ctx.store();
    auto run = [&](const doc::ops::EditResult& e) {
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
      REQUIRE(st.dispatch(e->command).ok());
      return *e;
    };
    track = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks.at(0);
    rack = run(doc::ops::addRack(*st.current(), st.ids(), track, false)).devices.at(0);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    ctx.setSelectedTrack(track);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
    d.tree().layoutNow();
    d.step();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }
  DeviceChainView& view() const {
    DeviceChainView* v = findDeviceChain(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  Widget& frame() const {
    Widget* f = view().frameOf(rack);
    REQUIRE(f != nullptr);
    return *f;
  }
  size_t chains() const { return doc::rackChains(*ctx.doc(), *doc::findTrackPtr(*ctx.doc(), track), rack).size(); }
  // Row `i` of the chain list (20 px title, 20 px rows, 132 px wide at scale 1): its centre.
  Vec2 row(size_t i) const {
    const IRect r = frame().rect();
    return {static_cast<float>(r.x + 40), static_cast<float>(r.y + 20 + static_cast<int32_t>(i) * 20 + 10)};
  }
  size_t history() const { return ctx.store().history().cursor(); }
};

}  // namespace

TEST_CASE("racks view: a rack (a group with chain groups) is shown as a rack — its chain list adds and shows chains, "
          "its macro knob drags its value block (real dispatch)") {
  Ui u;
  Widget& f = u.frame();
  CHECK(f.name() == "rack[" + std::to_string(u.rack.value) + "]");
  REQUIRE(u.chains() == 1);
  const doc::DeviceId first = u.view().shownChain(u.rack);
  CHECK(first.valid());
  // "+ Chain" (the row after the last chain): one undo step, the new chain shown.
  const size_t h0 = u.history();
  u.d.click(u.row(1));
  u.d.step();
  CHECK(u.chains() == 2);
  CHECK(u.history() == h0 + 1);
  const doc::DeviceId second = u.view().shownChain(u.rack);
  CHECK(second != first);
  CHECK(doc::findDevice(*u.ctx.doc(), second).device->name == "Chain 2");
  // A click on the first row shows chain 1 again (view state: no undo step).
  u.d.click(u.row(0));
  u.d.step();
  CHECK(u.view().shownChain(u.rack) == first);
  CHECK(u.history() == h0 + 1);
  // Up / Down step through the chains.
  const doc::DeviceId one[1] = {u.rack};
  u.view().select(one);
  CHECK(u.view().showNeighbourChain(1));
  CHECK(u.view().shownChain(u.rack) == second);
  // The macros: Macro 1's knob on the rack's panel, dragged up: its value block moves, one undo step.
  auto* host = dynamic_cast<panel::PanelHost*>(u.view().frameOf(u.rack)->find("macros[" + std::to_string(u.rack.value) + "]"));
  REQUIRE(host != nullptr);
  auto& face = static_cast<panel::GroupPanelFace&>(host->face());
  REQUIRE(face.elementCount() == doc::kDefaultMacros);
  const panel::Element& knob = face.element(0);
  REQUIRE(knob.binding() != nullptr);
  const doc::DeviceId macro = knob.binding()->device;
  CHECK(doc::isValueDevice(*doc::findDevice(*u.ctx.doc(), macro).device));
  const doc::PanelElement& e = knob.desc();
  const Vec2 c = host->transform().map({e.x + e.w * 0.5f, e.y + e.h * 0.5f});
  const size_t h1 = u.history();
  u.d.drag(c, {c.x, c.y - 60.0f}, 6);
  u.d.step();
  const doc::DeviceValue* v = doc::findDevice(*u.ctx.doc(), macro).device->param(doc::kValueParam);
  REQUIRE(v != nullptr);
  CHECK(v->value > 0.1);
  CHECK(u.history() == h1 + 1);
}

TEST_CASE("racks view: the circuit shows the rack as the group it is (one capsule, its chains capsules inside)") {
  Ui u;
  project_patch::ProjectPatchView* pv = findProjectPatch(*u.shell);
  REQUIRE(pv != nullptr);
  pv->setShown(true, false);
  u.d.tree().layoutNow();
  const project_patch::ProjectPatch& pp = pv->patch();
  const uint32_t n = pp.nodeOf(u.rack);
  REQUIRE(n != project_patch::kNone);
  CHECK(pp.nodes[n].group);
  const doc::DeviceId chain = u.view().shownChain(u.rack);
  const uint32_t cn = pp.nodeOf(chain);
  REQUIRE(cn != project_patch::kNone);
  CHECK(pp.nodes[cn].group);
}
