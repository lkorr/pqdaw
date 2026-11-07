// GU.3 (docs/plans/GU.md "Phases" 3): a group panel's edits through the playback link — the panel's own target (what
// the elements call: pq/views/panel/group_panel_face.h makeDocPanelTarget) on a document the link plays: a setting
// (the instrument group's voices) dragged through several positions recompiles exactly ONCE (at the end of the
// gesture); a control drag recompiles nothing (param writes); a curve point moved within the curve's class recompiles
// nothing, a shape change once; a panel edit (Meta) never.
#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "pq/app/project_link.h"
#include "pq/document/edit_ops.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/view_context.h"

using namespace pq;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

struct Rig {
  views::FakeTransport port;
  views::ViewContext ctx{port};
  views::project_patch::PanelProjectIds ids;
  std::unique_ptr<ProjectLink> link;
  views::ViewContext::Subscription sub;
  std::unique_ptr<views::panel::PanelTarget> target;
  Rig() {
    ctx.setDeviceLibrary(&library());
    REQUIRE(views::project_patch::buildPanelProject(ctx.store(), &ids).ok());
    LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    auto l = ProjectLink::create(c, ctx.doc());
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
    link = std::move(l.value());
    sub = ctx.subscribe([this](const doc::StoreEvent& e) { link->onDocumentChange(e); });
    link->flush();
    target = views::panel::makeDocPanelTarget(ctx, ids.group, [] { return nullptr; });
  }
  uint64_t compiles() {
    link->flush();
    return link->stats().compiles;
  }
};

}  // namespace

TEST_CASE("panel link: a setting dragged on a panel recompiles once; a control drag, a curve point and a panel edit "
          "never; a curve shape once") {
  Rig r;
  views::panel::PanelTarget& t = *r.target;
  const doc::Binding voices = doc::settingBinding(r.ids.group, std::string(doc::kGroupVoices));
  const doc::Binding level = doc::controlBinding(r.ids.level, std::string(doc::kValueParam));
  const doc::Binding cutoff = doc::controlBinding(r.ids.cutoff, std::string(doc::kValueParam));
  REQUIRE(t.info(voices).valid);
  REQUIRE(t.info(voices).setting);
  REQUIRE(t.info(level).valid);
  // A setting: five positions previewed, one commit, one compile.
  uint64_t c0 = r.compiles();
  t.begin(voices, "Panel Number");
  for (int k = 1; k <= 5; ++k) t.setPosition(voices, 0.05 * k);
  CHECK(r.compiles() == c0);  // nothing yet
  t.end();
  CHECK(r.compiles() == c0 + 1);
  CHECK(doc::groupPolyphony(*doc::findDevice(*r.ctx.doc(), r.ids.group).device).voices > 8u);
  // A control drag: param writes only.
  c0 = r.compiles();
  const uint64_t w0 = r.link->stats().paramWrites;
  t.begin(level, "Panel Knob");
  for (int k = 1; k <= 5; ++k) t.setPosition(level, 0.1 * k);
  t.end();
  CHECK(r.compiles() == c0);
  CHECK(r.link->stats().paramWrites > w0);
  // A curve point moved within its class: no compile; a shape change: one.
  dsp::Curve c = t.info(cutoff).curve;
  c.preset = dsp::CurvePreset::Custom;
  c.points[1].y = 0.8;
  c0 = r.compiles();
  t.begin(cutoff, "Edit Curve");
  t.setCurve(cutoff, c);
  t.end();
  CHECK(r.compiles() == c0);
  c.shapes[0].shape = dsp::CurveShape::Linear;
  t.begin(cutoff, "Curve Shape");
  t.setCurve(cutoff, c);
  t.end();
  CHECK(r.compiles() == c0 + 1);
  // A panel edit (Meta): never a compile.
  c0 = r.compiles();
  REQUIRE(r.ctx.store().dispatch(doc::ops::setPanelSize(*r.ctx.doc(), r.ids.group, 500.0f, 240.0f)->command).ok());
  CHECK(r.compiles() == c0);
}
