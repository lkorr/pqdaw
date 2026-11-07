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
