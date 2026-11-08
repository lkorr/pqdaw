// GU.10 (docs/plans/GU.md "One hierarchy"): the stock devices as group presets (pq/document/modules.h) are drawn
// closed as their panels — the old faces rebuilt from elements. Goldens at 2x: stock-utility-panel,
// stock-saturator-panel, stock-lfo-panel, stock-operator-panel (REVIEW.md 👁 puts them beside the retired faces).
// Every element binds a block of its group (bindingsProblem says nothing).
#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/document/panel.h"
#include "pq/views/panel/element.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/view_context.h"

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

// The preset `k` pasted onto a MIDI track (as the node menu pastes it), its panel in a plain tree at 2x.
Image presetPanelImage(doc::ModuleKind k) {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  doc::DocumentStore& st = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(st.dispatch(e->command).ok());
    return *e;
  };
  const doc::TrackId t = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks.at(0);
  const doc::DeviceId g = run(doc::ops::pasteBlocks(*st.current(), st.ids(), t, doc::moduleBlocks(k))).devices.at(0);
  const doc::Device* gd = doc::findDevice(*ctx.doc(), g).device;
  REQUIRE(gd != nullptr);
  const doc::Panel* p = gd->panel.get();
  REQUIRE(p != nullptr);
  CHECK(doc::bindingsProblem(doc::findTrackPtr(*ctx.doc(), t)->devices, g, *p) == "");
  const float scale = 2.0f;
  TestDriver d(static_cast<uint32_t>(p->width * scale), static_cast<uint32_t>(p->height * scale), scale);
  panel::PanelHost* host = nullptr;
  auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, g),
                                                      panel::makeDocPanelTarget(ctx, g, [&host]() -> Widget* { return host; }));
  host = &d.setRoot<panel::PanelHost>("panel", std::move(face));
  d.tree().layoutNow();
  REQUIRE(d.enableGpu().ok());
  d.advance(0.1);
  auto img = d.render();
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  Image out = std::move(*img);
  d.tree().setRoot(std::make_unique<Widget>());
  return out;
}

}  // namespace

TEST_CASE("render: stock-utility-panel, stock-saturator-panel, stock-lfo-panel, stock-operator-panel (GU.10, 2x)") {
  checkImage("stock-utility-panel", presetPanelImage(doc::ModuleKind::Utility));
  checkImage("stock-saturator-panel", presetPanelImage(doc::ModuleKind::Saturator));
  checkImage("stock-lfo-panel", presetPanelImage(doc::ModuleKind::Lfo));
  checkImage("stock-operator-panel", presetPanelImage(doc::ModuleKind::Operator));
}

// EQ.3: PanelTarget::run — a structural edit an element asks for (the EQ's band ops) goes through the host's history as
// one undo step, a refused one leaves a notice; the EQ's band strip follows Mode and the chain tabs (chain A's columns
// show in Stereo whatever the tabs hold: panel.h kWhenHidden).
TEST_CASE("EQ.3 doc target: run() dispatches an op as one undo step (a refusal is a notice); the strip follows the tabs") {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  doc::DocumentStore& st = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(st.dispatch(e->command).ok());
    return *e;
  };
  const doc::TrackId t = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "A")).tracks.at(0);
  const doc::DeviceId eq = run(doc::ops::pasteBlocks(*st.current(), st.ids(), t, doc::moduleBlocks(doc::ModuleKind::Eq))).devices.at(0);
  panel::GroupPanelFace face(panel::docPanelSource(ctx, eq), panel::makeDocPanelTarget(ctx, eq, []() -> Widget* { return nullptr; }));
  face.sync();
  panel::PanelTarget& target = face.target();
  // The Type menus of chain c's bands: their elements' indices.
  auto typeMenus = [&](uint32_t c) {
    std::vector<size_t> out;
    const std::optional<doc::EqInfo> e = doc::eqInfo(*ctx.doc(), eq);
    REQUIRE(e.has_value());
    for (const doc::BandInfo& b : e->bands[c])
      for (size_t i = 0; i < face.elementCount(); ++i)
        if (face.element(i).desc().kind == "menu" && face.element(i).desc().binding() != nullptr &&
            face.element(i).desc().binding()->device == b.type)
          out.push_back(i);
    CHECK(out.size() == e->bands[c].size());
    return out;
  };
  auto allShown = [&](const std::vector<size_t>& v, bool want) {
    for (const size_t i : v) CHECK(face.shown(i) == want);
  };
  REQUIRE(typeMenus(0).size() == 4);
  allShown(typeMenus(0), true);
  // run(): one step through the store.
  const size_t h0 = st.history().cursor();
  CHECK(target.run([&](const doc::Project& p, doc::IdSource& ids) { return doc::ops::addBand(p, ids, eq, 0, 4, 3000.0, 2.0); }));
  CHECK(st.history().cursor() == h0 + 1);
  face.sync();
  CHECK(typeMenus(0).size() == 5);
  // Split to L/R through run(): B is A's copy; the tabs (Edit Chain) at A show A's columns only.
  CHECK(target.run([&](const doc::Project& p, doc::IdSource& ids) { return doc::ops::setEqMode(p, ids, eq, doc::EqMode::LeftRight); }));
  CHECK(st.history().cursor() == h0 + 2);
  face.sync();
  allShown(typeMenus(0), true);
  allShown(typeMenus(1), false);
  const doc::DeviceId editChain = doc::eqInfo(*ctx.doc(), eq)->editChain;
  const doc::Binding tabs = doc::controlBinding(editChain, std::string(doc::kValueParam));
  target.setOnce(tabs, 1.0, "tab B");
  face.sync();
  allShown(typeMenus(0), false);
  allShown(typeMenus(1), true);
  // Stereo: the tabs hide, chain A's columns show although Edit Chain still says B.
  CHECK(target.run([&](const doc::Project& p, doc::IdSource& ids) { return doc::ops::setEqMode(p, ids, eq, doc::EqMode::Stereo); }));
  face.sync();
  allShown(typeMenus(0), true);
  allShown(typeMenus(1), false);
  // A refused op: false, nothing recorded, the reason in the status line.
  for (uint32_t i = 5; i < doc::kMaxBandsPerChain; ++i)
    CHECK(target.run([&](const doc::Project& p, doc::IdSource& ids) { return doc::ops::addBand(p, ids, eq, 0, 4, 100.0 * i, 0.0); }));
  const size_t full = st.history().cursor();
  CHECK_FALSE(target.run([&](const doc::Project& p, doc::IdSource& ids) { return doc::ops::addBand(p, ids, eq, 0, 4, 500.0, 0.0); }));
  CHECK(st.history().cursor() == full);
  CHECK(ctx.status().find("32") != std::string::npos);
  // The panel binds only blocks of the EQ.
  CHECK(doc::bindingsProblem(doc::findTrackPtr(*ctx.doc(), t)->devices, eq, *doc::findDevice(*ctx.doc(), eq).device->panel) == "");
}
