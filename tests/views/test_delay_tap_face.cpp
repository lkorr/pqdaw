// DT.1 (docs/plans/DT.md "Face"): the Delay Tap's face — the tempo slider (pq/views/src/panel/tempo_slider.cpp) — in the
// shell's project patch view, through real window dispatch:
//   - gravity: a drag of the time knob within 6 px of a marking (the note values at the tempo, 120 BPM: 1/4 = 500 ms
//     at the middle of the travel) lands on it; past the zone it moves freely; one undo step, no recompile
//   - the magnet button turns gravity off (the element's own style field: one "Edit Panel" step); the drag then goes
//     where the pointer says; Shift (fine) ignores gravity too
//   - the ms / ♪ switch sets Mode (one step); in sync mode a drag steps the Sync index to the marking nearest the pointer
//   - the markings follow the tempo (90 BPM moves 1/4 to 667 ms)
// Goldens: module-delay-tap-panel (time mode on 1/4) and module-delay-tap-sync (sync mode, 1/8.), 2x.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <ostream>
#include <string>

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

// An audio track holding a Delay Tap (pasted as the node menu does); its roles in the document.
doc::DelayTapRoles buildDelayTap(doc::DocumentStore& st, const doc::DelayTapOptions& o = {}) {
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(st.dispatch(e->command).ok());
    return *e;
  };
  const doc::TrackId track = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "Echo")).tracks.at(0);
  doc::IdSource local;
  doc::DelayTapRoles l;
  const doc::BlockSet set = doc::delayTapBlocks(local, o, &l);
  const doc::ops::Edit e = run(doc::ops::pasteBlocks(*st.current(), st.ids(), track, set));
  REQUIRE(e.devices.size() == set.devices.size());
  auto id = [&](doc::DeviceId x) {  // the pasted block at x's place in the set
    for (size_t i = 0; i < set.devices.size(); ++i)
      if (set.devices[i]->id == x) return e.devices[i];
    FAIL("not in the set");
    return doc::DeviceId{};
  };
  return doc::DelayTapRoles{id(l.group), id(l.time), id(l.sync), id(l.mode), id(l.delay)};
}

// The panel's layout (tempo_slider.cpp, a 168 × 64 element at the panel's origin): the groove 10 … 158 at y 26, the
// ms / ♪ switch 110 … 140 and the magnet 144 … 158 at y 34 … 46.
constexpr float kX0 = 10.0f, kX1 = 158.0f, kGy = 26.0f;
float grooveX(double position) { return kX0 + (kX1 - kX0) * static_cast<float>(position); }
constexpr Vec2 kMagnet{151.0f, 40.0f}, kMs{117.0f, 40.0f}, kNote{133.0f, 40.0f};

struct DelayUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  doc::DelayTapRoles r;
  int structural = 0;
  ViewContext::Subscription sub;

  DelayUi() {
    ctx.setDeviceLibrary(&devLib());
    r = buildDelayTap(ctx.store());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    const Node& n = node(r.group);
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = n.x - 40.0;
    v.y = n.y - 40.0;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
    d.tree().setFocus(&view().canvas());
    sub = ctx.subscribe([this](const doc::StoreEvent& e) { structural += e.before != e.after && e.changes.structural() ? 1 : 0; });
  }
  ~DelayUi() {
    sub.reset();
    d.tree().setRoot(std::make_unique<Widget>());
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
  Vec2 at(Vec2 local) const {
    const Node& n = node(r.group);
    return view().canvas().toDevice({n.x + local.x * n.scale, n.y + local.y * n.scale});
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Device& dev(doc::DeviceId id) const { return *doc::findDevice(*ctx.doc(), id).device; }
  double position(doc::DeviceId id) const { return dev(id).param(doc::kValueParam)->value; }
  double seconds() const { return dsp::evalCurve(*dev(r.time).curve(doc::kValueParam), position(r.time)); }
  double gravity() const {
    const doc::Panel& p = *dev(r.group).panel;
    return p.elements.at(1).number("gravity", 1.0);
  }
  // A time drag along the groove from the knob by `dx` panel units (in `steps` moves).
  void dragTime(float dx, Mods m = mod::kNone, int steps = 6) {
    const float x = grooveX(position(r.time));
    d.drag(at({x, kGy}), at({x + dx, kGy}), steps, m);
    d.step();
  }
};

double placeOf(double seconds) { return dsp::curvePosition(doc::delayTimeCurve(), seconds, 0.5); }

Image panelImage(const doc::DelayTapOptions& o) {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  const doc::DelayTapRoles r = buildDelayTap(ctx.store(), o);
  const doc::Panel* p = doc::findDevice(*ctx.doc(), r.group).device->panel.get();
  REQUIRE(p != nullptr);
  const float scale = 2.0f;
  TestDriver d(static_cast<uint32_t>(p->width * scale), static_cast<uint32_t>(p->height * scale), scale);
  panel::PanelHost* host = nullptr;
  auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, r.group),
                                                      panel::makeDocPanelTarget(ctx, r.group, [&host]() -> Widget* { return host; }));
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

TEST_CASE("render: module-delay-tap-panel and module-delay-tap-sync (DT.1: the tempo slider, 120 BPM, 2x)") {
  checkImage("module-delay-tap-panel", panelImage({}));
  doc::DelayTapOptions sync;
  sync.mode = 1;
  sync.sync = 12;  // 1/8.
  checkImage("module-delay-tap-sync", panelImage(sync));
}

TEST_CASE("delay tap face: gravity — a drag near a marking lands on it, past the zone it is free; one undo step, no "
          "recompile — real dispatch") {
  DelayUi u;
  REQUIRE(u.position(u.r.time) == doctest::Approx(0.5));  // 500 ms = 1/4 at 120 BPM
  const double halfT = placeOf(2.0 / 3.0);                 // 1/2T: 667 ms
  const size_t h0 = u.history();
  // +7 units: the pointer at 0.5473, 1/2T's marking (0.5591) within 6 px: it sticks there.
  u.dragTime(7.0f);
  CHECK(u.position(u.r.time) == doctest::Approx(halfT).epsilon(1e-12));
  CHECK(u.seconds() == doctest::Approx(2.0 / 3.0).epsilon(1e-9));
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);
  // Within one drag: near 1/4. (750 ms, x 96.4) it sticks, 3 units past it still, then at 4.5 past it — between 1/4.'s
  // zone and 1/2's (x 105.4; zones 40 % of the 9-unit gap there) — it is the pointer's. (1/2T at 92.7 is 3.6 units
  // below 1/4.: their zones there are 1.5 units each.)
  const double dotted = placeOf(0.75);
  {
    const float x0 = grooveX(u.position(u.r.time));
    const float xd = grooveX(dotted);
    u.d.move(u.at({x0, kGy}));
    u.d.down(u.at({x0, kGy}));
    u.d.move(u.at({xd - 1.0f, kGy}));
    u.d.step();
    CHECK(u.position(u.r.time) == doctest::Approx(dotted).epsilon(1e-12));
    u.d.move(u.at({xd + 3.0f, kGy}));
    u.d.step();
    CHECK(u.position(u.r.time) == doctest::Approx(dotted).epsilon(1e-12));  // still in its zone
    u.d.move(u.at({xd + 4.5f, kGy}));
    u.d.up(u.at({xd + 4.5f, kGy}));
    u.d.step();
    CHECK(u.position(u.r.time) == doctest::Approx(halfT + (xd + 4.5f - grooveX(halfT)) / (kX1 - kX0)).epsilon(1e-6));
  }
  CHECK(u.history() == h0 + 2);
  // Shift (fine) ignores gravity: a tenth of the travel, no snap.
  const double p1 = u.position(u.r.time);
  u.dragTime(-20.0f, mod::kShift);
  CHECK(u.position(u.r.time) == doctest::Approx(p1 - 2.0 / (kX1 - kX0)).epsilon(1e-6));
}

TEST_CASE("delay tap face: the magnet turns gravity off (one Edit Panel step, undoable); the drag is the pointer's — "
          "real dispatch") {
  DelayUi u;
  CHECK(u.gravity() == 1.0);
  const size_t h0 = u.history();
  u.d.click(u.at(kMagnet));
  u.d.step();
  CHECK(u.gravity() == 0.0);
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);
  u.dragTime(7.0f);
  CHECK(u.position(u.r.time) == doctest::Approx(0.5 + 7.0 / (kX1 - kX0)).epsilon(1e-6));
  REQUIRE(u.ctx.undo());  // the drag
  REQUIRE(u.ctx.undo());  // the magnet
  CHECK(u.gravity() == 1.0);
  CHECK(u.position(u.r.time) == doctest::Approx(0.5));
}

TEST_CASE("delay tap face: the ms / note switch sets Mode; in sync mode a drag steps through the note values at their "
          "markings; the markings follow the tempo — real dispatch") {
  DelayUi u;
  const size_t h0 = u.history();
  u.d.click(u.at(kNote));
  u.d.step();
  CHECK(u.position(u.r.mode) == 1.0);
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);  // (Mode and Sync reach no choice: runtime params, never a recompile)
  // Sync is 1/4 (index 13): the knob sits at 1/4's marking (0.5). A drag to 1/4.'s marking picks it (index 15).
  CHECK(u.position(u.r.sync) == doctest::Approx(13.0 / 20.0));
  const float from = grooveX(0.5), to = grooveX(placeOf(0.75)) + 1.0f;
  u.d.drag(u.at({from, kGy}), u.at({to, kGy}), 6);
  u.d.step();
  CHECK(u.position(u.r.sync) == doctest::Approx(15.0 / 20.0));
  CHECK(u.position(u.r.time) == doctest::Approx(0.5));  // the time is untouched
  CHECK(u.history() == h0 + 2);
  CHECK(u.structural == 0);
  // Back to ms.
  u.d.click(u.at(kMs));
  u.d.step();
  CHECK(u.position(u.r.mode) == 0.0);
  // At 90 BPM 1/4 is 667 ms: a drag from 500 ms by +7 units now lands on 1/4's marking (0.5591), named 1/4 there.
  REQUIRE(u.ctx.store().dispatch(doc::SetTempoMap{{TempoPoint{0, 90.0}}}).ok());
  u.d.step();
  u.dragTime(7.0f);
  CHECK(u.seconds() == doctest::Approx(2.0 / 3.0).epsilon(1e-9));
}

TEST_CASE("delay tap face: while the transport plays the markings follow the tempo at the playhead — a repaint when the "
          "playhead crosses a tempo change, none at a steady tempo") {
  DelayUi u;
  REQUIRE(u.d.enableGpu().ok());  // (frames are painted only with the GPU)
  // 120 BPM, 90 from bar 2 (a step: no document change happens while the playhead crosses it).
  REQUIRE(u.ctx.store().dispatch(doc::SetTempoMap{{TempoPoint{0, 120.0}, TempoPoint{8 * kPpq, 90.0}}}).ok());
  u.d.step();
  u.ctx.play();  // through the view context: kTransport wakes the faces (the playhead at 0: 120 BPM)
  u.d.advance(0.2);
  const auto& canvas = u.view().canvas();
  const uint64_t steady0 = canvas.stats().paints;  // (cumulative canvas paints)
  u.d.advance(0.3);
  CHECK(canvas.stats().paints == steady0);  // steady tempo: compares only, nothing repainted
  CHECK(canvas.animatingFaces());                  // the ≤ 60 Hz poll runs while playing
  u.port.locate(9 * kPpq);                         // the engine's playhead report moves past the change (no notify)
  u.d.advance(0.1);
  CHECK(canvas.stats().paints > steady0);
  CHECK(canvas.stats().panelsPainted >= 1);  // (the last paint's: the Delay Tap's panel)
  const uint64_t after = canvas.stats().paints;
  u.d.advance(0.3);
  CHECK(canvas.stats().paints == after);  // steady again
  // The markings are at 90 BPM now: from 500 ms (1/8. there) +7 units lands on 1/4 = 667 ms.
  u.dragTime(7.0f);
  CHECK(u.seconds() == doctest::Approx(2.0 / 3.0).epsilon(1e-9));
  u.ctx.stopButton();
  u.d.advance(0.2);
  CHECK_FALSE(canvas.animatingFaces());  // stopped: no poll
}
