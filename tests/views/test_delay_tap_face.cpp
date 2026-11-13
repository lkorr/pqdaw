// DT.1 (docs/plans/DT.md "Face"): the Delay Tap's face — the tempo slider (pq/views/src/panel/tempo_slider.cpp) — in the
// shell's project patch view, through real window dispatch:
//   - gravity: a drag of the time knob within 6 px of a marking (the note values at the tempo, 120 BPM: 1/4 = 500 ms
//     at the middle of the travel) lands on it; past the zone it moves freely; one undo step, no recompile
//   - the magnet button turns gravity off (the element's own style field: one "Edit Panel" step); the drag then goes
//     where the pointer says; Shift (fine) ignores gravity too
//   - the ms / ♪ switch sets Mode (one step); in sync mode a drag steps the Sync index to the marking nearest the pointer
//   - the markings follow the tempo (90 BPM moves 1/4 to 667 ms)
// DT.3 (docs/plans/DT.md "DT.3"):
//   - the markings are whole-note fractions (1/12, 3/16), shown by simplicity and screen room (tempo_marks.h)
//   - a press-drag on the face's body (readout, markings) moves the block; the groove stays the knob's
//   - an edge drag stretches the face (one undo step, Esc restores; the left edge keeps the right one in place); wide,
//     1/7 is shown and pulls the knob; at the default width it does not
// Goldens: module-delay-tap-panel (time mode on 1/4), module-delay-tap-sync (sync mode, 3/16) and
// module-delay-tap-stretched (640 wide, on 1/7), 2x.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
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
#include "pq/views/panel/tempo_marks.h"
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
  return doc::DelayTapRoles{id(l.group), id(l.time), id(l.sync), id(l.mode), id(l.delay), id(l.glide)};
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

// `width` > 0: the face stretched to it first (GroupPanelFace::stretchTo, the edge drag's edit). `micro`: the micro bar
// open (DT.4: the magnifier's edit — the slider's "micro" field, it, the plate and the panel 24 taller).
Image panelImage(const doc::DelayTapOptions& o, float width = 0.0f, bool micro = false) {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  const doc::DelayTapRoles r = buildDelayTap(ctx.store(), o);
  if (micro) {
    auto p = std::make_shared<doc::Panel>(*doc::findDevice(*ctx.doc(), r.group).device->panel);
    p->elements.at(1).setField(doc::styleNumber("micro", 1.0));
    p->elements.at(1).h += 24.0f;
    p->elements.at(0).h += 24.0f;
    p->height += 24.0f;
    auto e = doc::ops::setGroupPanel(*ctx.doc(), r.group, std::move(p));
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
  }
  if (width > 0.0f) {
    panel::GroupPanelFace f(panel::docPanelSource(ctx, r.group), panel::makeDocPanelTarget(ctx, r.group, {}));
    f.setOwner(r.group);
    REQUIRE(f.stretchX().has_value());
    REQUIRE(f.stretchTo(width));
  }
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
  sync.sync = 12;  // 3/16
  checkImage("module-delay-tap-sync", panelImage(sync));
}

TEST_CASE("delay tap face: gravity — a drag near a marking lands on it, past the zone it is free; one undo step, no "
          "recompile — real dispatch") {
  DelayUi u;
  REQUIRE(u.position(u.r.time) == doctest::Approx(0.5));  // 500 ms = 1/4 at 120 BPM
  const double halfT = placeOf(2.0 / 3.0);                 // 1/3 (a half triplet): 667 ms
  const size_t h0 = u.history();
  // +7 units: the pointer at 0.5473, 1/3's marking (0.5591) within 6 px: it sticks there.
  u.dragTime(7.0f);
  CHECK(u.position(u.r.time) == doctest::Approx(halfT).epsilon(1e-12));
  CHECK(u.seconds() == doctest::Approx(2.0 / 3.0).epsilon(1e-9));
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);
  // Within one drag: near 3/8 (750 ms, x 96.4) it sticks, 3 units past it still, then at 4.5 past it — between 3/8's
  // zone and 1/2's (x 105.4; zones 40 % of the 9-unit gap there) — it is the pointer's. (1/3 at 92.7 is 3.6 units
  // below 3/8: their zones there are 1.5 units each.)
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
  // Sync is 1/4 (index 13): the knob sits at 1/4's marking (0.5). A drag to 3/8's marking picks it (index 15).
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
  // The markings are at 90 BPM now: from 500 ms (3/16 there) +7 units lands on 1/4 = 667 ms.
  u.dragTime(7.0f);
  CHECK(u.seconds() == doctest::Approx(2.0 / 3.0).epsilon(1e-9));
  u.ctx.stopButton();
  u.d.advance(0.2);
  CHECK_FALSE(canvas.animatingFaces());  // stopped: no poll
}

// ---- DT.3 -----------------------------------------------------------------------------------------------------------

namespace {

const panel::TempoMark* markOf(const std::vector<panel::TempoMark>& m, std::string_view label) {
  for (const panel::TempoMark& k : m)
    if (k.label == label) return &k;
  return nullptr;
}
// The markings at 120 BPM for a delay tap face `w` wide at `ppu` logical px per unit (tempo_slider.cpp's layout).
std::vector<panel::TempoMark> marksAt(float w, float ppu = 1.0f) {
  std::vector<panel::TempoMark> m = panel::tempoMarkCandidates(doc::delayTimeCurve(), 120.0);
  (void)panel::layoutTempoMarks(m, kX0, w - 10.0f, w, ppu);
  return m;
}

}  // namespace

TEST_CASE("delay tap face: the markings are whole-note fractions, shown by simplicity and room on screen (DT.3)") {
  // The sync list's names.
  const auto v = doc::noteValues();
  CHECK(v[8].label == "1/12");   // was 1/8T
  CHECK(v[12].label == "3/16");  // was 1/8.
  CHECK(v[3].label == "3/128");  // was 1/64.
  // Default width: the straight values labelled, dotted and triplets as ticks, no fives or sevens.
  const auto def = marksAt(168.0f);
  REQUIRE(markOf(def, "1/4") != nullptr);
  CHECK(markOf(def, "1/4")->labelled);
  CHECK(markOf(def, "3/16")->shown);
  CHECK(markOf(def, "1/6")->shown);
  CHECK_FALSE(markOf(def, "1/7")->shown);
  CHECK_FALSE(markOf(def, "1/5")->shown);
  // Wider (a stretched face) or closer (the circuit zoomed in 4×): the sevens appear; wide enough, 1/7 is labelled.
  const auto wide = marksAt(800.0f);
  CHECK(markOf(wide, "1/7")->shown);
  CHECK(markOf(wide, "1/7")->labelled);
  CHECK(markOf(wide, "2/7")->shown);
  CHECK(markOf(marksAt(168.0f, 4.0f), "1/7")->shown);
  // Simplicity order: powers of two, then dotted, triplets, fives, sevens, other numerators.
  CHECK(wide.front().label == "1/1");
  CHECK(markOf(wide, "3/16")->tier == 1);
  CHECK(markOf(wide, "1/12")->tier == 1);
  CHECK(markOf(wide, "1/20")->tier == 2);
  CHECK(markOf(wide, "2/7")->tier == 3);
  CHECK(markOf(wide, "5/16")->tier == 4);
}

TEST_CASE("delay tap face: a press-drag on the face's body moves the block; on the slider it moves the knob (DT.3) — "
          "real dispatch") {
  DelayUi u;
  const float x0 = u.node(u.r.group).x, y0 = u.node(u.r.group).y;
  const double t0 = u.position(u.r.time);
  // The readout (below the groove, left) and the markings (above it) are the block's.
  u.d.drag(u.at({30.0f, 44.0f}), u.at({90.0f, 74.0f}), 6);
  u.d.step();
  CHECK(u.node(u.r.group).x == doctest::Approx(x0 + 60.0f).epsilon(0.02));
  CHECK(u.node(u.r.group).y == doctest::Approx(y0 + 30.0f).epsilon(0.02));
  CHECK(u.position(u.r.time) == t0);
  const float x1 = u.node(u.r.group).x;
  u.d.drag(u.at({60.0f, 8.0f}), u.at({20.0f, 8.0f}), 6);
  u.d.step();
  CHECK(u.node(u.r.group).x == doctest::Approx(x1 - 40.0f).epsilon(0.02));
  CHECK(u.position(u.r.time) == t0);
  // The groove is the knob's.
  const float x2 = u.node(u.r.group).x;
  u.dragTime(30.0f);
  CHECK(u.node(u.r.group).x == x2);
  CHECK(u.position(u.r.time) != t0);
}

TEST_CASE("delay tap face: an edge drag stretches it (one undo step); at a wide stretch 1/7 pulls the knob, at the "
          "default width it does not (DT.3) — real dispatch") {
  DelayUi u;
  auto width = [&] { return u.dev(u.r.group).panel->width; };
  auto slider = [&] { return u.dev(u.r.group).panel->elements.at(1).w; };  // (0: the plate)
  auto plate = [&] { return u.dev(u.r.group).panel->elements.at(0).w; };
  REQUIRE(width() == 168.0f);
  REQUIRE(slider() == 168.0f);
  const float right0 = u.node(u.r.group).x + u.node(u.r.group).w;
  // At the default width a drag that ends on 1/7's place (2/7 s) is the pointer's: 1/7 is not shown.
  const double seventh = placeOf(2.0 / 7.0);
  u.dragTime(static_cast<float>((seventh - 0.5) * (kX1 - kX0)));
  CHECK(u.position(u.r.time) == doctest::Approx(seventh).epsilon(1e-3));
  CHECK(u.position(u.r.time) != doctest::Approx(seventh).epsilon(1e-9));
  REQUIRE(u.ctx.undo());
  // Hover on the right edge: the ↔ cursor; a drag there widens the face to 800 (its height stays).
  const size_t h0 = u.history();
  u.d.move(u.at({166.5f, 12.0f}));
  u.d.step();
  CHECK(u.view().canvas().cursorAt(u.at({166.5f, 12.0f})) == Cursor::ResizeEW);
  u.d.drag(u.at({166.5f, 12.0f}), u.at({166.5f + 632.0f, 12.0f}), 8);
  u.d.step();
  CHECK(width() == 800.0f);
  CHECK(slider() == 800.0f);
  CHECK(plate() == 800.0f);  // the plate spans the panel: it widens with it
  CHECK(u.dev(u.r.group).panel->height == 64.0f);
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);
  CHECK(u.node(u.r.group).w == doctest::Approx(800.0f));
  // The knob at 0.5, a drag to 2 units past 1/7's marking lands on it (gravity: 6 px).
  const float x1w = 800.0f - 10.0f;
  auto grooveW = [&](double p) { return kX0 + (x1w - kX0) * static_cast<float>(p); };
  {
    const float x = grooveW(u.position(u.r.time));
    u.d.drag(u.at({x, kGy}), u.at({grooveW(seventh) + 2.0f, kGy}), 8);
    u.d.step();
  }
  CHECK(u.seconds() == doctest::Approx(2.0 / 7.0).epsilon(1e-9));
  REQUIRE(u.ctx.undo());  // the drag
  // Undo the stretch: the default width again.
  REQUIRE(u.ctx.undo());
  CHECK(width() == 168.0f);
  CHECK(slider() == 168.0f);
  // The left edge: the face widens leftwards, its right edge stays; Esc mid-drag puts everything back. (The view moved
  // so the widened face stays on the canvas.)
  {
    circuit::CircuitCanvas::View cv = u.view().canvas().view();
    cv.x -= 300.0;
    u.view().canvas().setView(cv);
    u.d.tree().layoutNow();
    u.d.step();
  }
  u.d.drag(u.at({1.5f, 12.0f}), u.at({1.5f - 100.0f, 12.0f}), 6);
  u.d.step();
  CHECK(width() == 268.0f);
  CHECK(std::fabs(u.node(u.r.group).x + u.node(u.r.group).w - right0) <= 1.0f);  // (a nudge is whole units)
  const size_t h1 = u.history();
  u.d.down(u.at({1.5f, 12.0f}));
  u.d.move(u.at({-60.0f, 12.0f}));
  u.d.step();
  CHECK(width() > 268.0f);
  u.d.keyDown(key::kEscape);
  u.d.keyUp(key::kEscape);
  u.d.up(u.at({-60.0f, 12.0f}));
  u.d.step();
  CHECK(width() == 268.0f);
  CHECK(u.history() == h1);
  CHECK(std::fabs(u.node(u.r.group).x + u.node(u.r.group).w - right0) <= 1.0f);  // (a nudge is whole units)
  // Never narrower than the slider's own width.
  u.d.drag(u.at({1.5f, 12.0f}), u.at({1.5f + 400.0f, 12.0f}), 6);
  u.d.step();
  CHECK(width() == 168.0f);
}

TEST_CASE("render: module-delay-tap-stretched (DT.3: the face stretched to 640, more markings labelled, 2x)") {
  doc::DelayTapOptions o;
  o.time = placeOf(2.0 / 7.0);  // on 1/7
  checkImage("module-delay-tap-stretched", panelImage(o, 640.0f));
}

// ---- DT.4 -----------------------------------------------------------------------------------------------------------

namespace {

// The DT.4 controls (a 168-wide element): the Glide toggle 92 … 106 and the magnifier 74 … 88 at y 34 … 46; with the
// micro bar open (the element 88 high) its groove 10 … 110 at y 66, its number box 116 … 158 at y 62 … 74.
constexpr Vec2 kGlide{99.0f, 40.0f}, kMicro{81.0f, 40.0f}, kBox{137.0f, 68.0f};
constexpr float kMx0 = 10.0f, kMx1 = 110.0f, kMy = 66.0f;
constexpr double kRate = 48000.0;  // the view context's sample rate without an engine
// (The time is stored as the knob's position: seconds = curve(position) is N / rate to within an ulp or two — the exp
// curve's positions do not reach every double — so a set of N samples is checked as N to 1e-9 samples.)

// The hover info's body over the groove (the readout's text: TempoSlider::describe), hovering through the window.
std::string hoverReadout(DelayUi& u) {
  u.d.move(u.at({30.0f, kGy}));
  u.d.step();
  return std::string(u.view().canvas().info().body);
}

}  // namespace

TEST_CASE("delay tap face: on a marking the readout names it (\"667 ms | 1/3\"), off one it does not (DT.4) — real dispatch") {
  DelayUi u;
  CHECK(hoverReadout(u).starts_with("500 ms | 1/4."));  // the default: 500 ms is 1/4 at 120 BPM
  u.dragTime(7.0f);                                      // gravity: onto 1/3's marking
  CHECK(hoverReadout(u).starts_with("667 ms | 1/3."));
  u.dragTime(-1.0f, mod::kShift);                         // fine: off it (a tenth of a unit), no snap
  const std::string off = hoverReadout(u);
  CHECK(off.starts_with("66"));
  CHECK(off.find(" | 1/") == std::string::npos);
  // A marking that is not shown at this width still names the time when it is exactly on it (2/7 s = 1/7).
  REQUIRE(u.ctx.store().dispatch(doc::SetDeviceParam{u.r.time, std::string(doc::kValueParam), placeOf(2.0 / 7.0)}).ok());
  u.d.step();
  CHECK(hoverReadout(u).starts_with("286 ms | 1/7."));
}

TEST_CASE("delay tap face: the micro bar — opens below (the face grows), a drag sets whole samples, a typed count, small "
          "times read in samples; one undo step each (DT.4) — real dispatch") {
  DelayUi u;
  auto panelH = [&] { return u.dev(u.r.group).panel->height; };
  auto sliderH = [&] { return u.dev(u.r.group).panel->elements.at(1).h; };
  auto plateH = [&] { return u.dev(u.r.group).panel->elements.at(0).h; };
  const float nodeH0 = u.node(u.r.group).h;
  REQUIRE(panelH() == 64.0f);
  const size_t h0 = u.history();
  // Open: one Edit Panel step; the element, the plate and the panel 24 taller, the block too; nothing recompiles.
  u.d.click(u.at(kMicro));
  u.d.step();
  CHECK(u.dev(u.r.group).panel->elements.at(1).number("micro", 0.0) == 1.0);
  CHECK(panelH() == 88.0f);
  CHECK(sliderH() == 88.0f);
  CHECK(plateH() == 88.0f);
  CHECK(u.node(u.r.group).h == doctest::Approx(nodeH0 + 24.0f));
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);
  // 500 ms is past the bar's 256 samples: the knob waits at its end. A drag of −40 units (of 100 for 255 samples) from
  // anywhere on it: 256 − 102 = 154 samples (154 / 48000 s); one undo step.
  u.d.drag(u.at({60.0f, kMy}), u.at({20.0f, kMy}), 8);
  u.d.step();
  CHECK(std::fabs(u.seconds() * kRate - 154.0) < 1e-9);
  CHECK(u.history() == h0 + 2);
  CHECK(u.structural == 0);
  CHECK(hoverReadout(u).starts_with("3.21 ms | 154 smp."));
  // Fine (Shift): a tenth — −10 units is −2.55 → −3 samples.
  u.d.drag(u.at({60.0f, kMy}), u.at({50.0f, kMy}), 4, mod::kShift);
  u.d.step();
  CHECK(std::fabs(u.seconds() * kRate - 151.0) < 1e-9);
  // Type a count into the number box: click it, type, Enter — one undo step.
  const size_t h1 = u.history();
  u.d.click(u.at(kBox));
  u.d.step();
  REQUIRE(u.d.type("48"));
  u.d.keyDown(key::kReturn);
  u.d.keyUp(key::kReturn);
  u.d.step();
  CHECK(std::fabs(u.seconds() * kRate - 48.0) < 1e-9);
  CHECK(u.history() == h1 + 1);
  CHECK(hoverReadout(u).starts_with("1.00 ms | 48 smp."));
  // Esc cancels a type-in.
  u.d.click(u.at(kBox));
  u.d.step();
  REQUIRE(u.d.type("7"));
  u.d.keyDown(key::kEscape);
  u.d.keyUp(key::kEscape);
  u.d.step();
  CHECK(std::fabs(u.seconds() * kRate - 48.0) < 1e-9);
  CHECK(u.history() == h1 + 1);
  // The delay line's floor: a time of 0 reads as its 1 sample.
  REQUIRE(u.ctx.store().dispatch(doc::SetDeviceParam{u.r.time, std::string(doc::kValueParam), 0.0}).ok());
  u.d.step();
  CHECK(hoverReadout(u).starts_with("0.02 ms | 1 smp."));
  // Undo: the type-in, the two drags, the opening.
  REQUIRE(u.ctx.undo());  // the SetParam
  REQUIRE(u.ctx.undo());  // the typed 48
  CHECK(std::fabs(u.seconds() * kRate - 151.0) < 1e-9);
  REQUIRE(u.ctx.undo());
  CHECK(std::fabs(u.seconds() * kRate - 154.0) < 1e-9);
  REQUIRE(u.ctx.undo());
  CHECK(u.position(u.r.time) == doctest::Approx(0.5));
  REQUIRE(u.ctx.undo());
  CHECK(panelH() == 64.0f);
  CHECK(sliderH() == 64.0f);
  CHECK(u.node(u.r.group).h == doctest::Approx(nodeH0));
  // Closing it again is the magnifier too.
  u.d.click(u.at(kMicro));
  u.d.step();
  CHECK(panelH() == 88.0f);
  u.d.click(u.at(kMicro));
  u.d.step();
  CHECK(panelH() == 64.0f);
  CHECK(u.dev(u.r.group).panel->elements.at(1).number("micro", 0.0) == 0.0);
}

TEST_CASE("delay tap face: the Glide toggle flips Repitch / Crossfade (one undo step, no recompile) (DT.4) — real dispatch") {
  DelayUi u;
  CHECK(u.position(u.r.glide) == 0.0);  // Repitch by default
  const size_t h0 = u.history();
  u.d.click(u.at(kGlide));
  u.d.step();
  CHECK(u.position(u.r.glide) == 1.0);
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);
  u.d.click(u.at(kGlide));
  u.d.step();
  CHECK(u.position(u.r.glide) == 0.0);
  CHECK(u.history() == h0 + 2);
  REQUIRE(u.ctx.undo());
  CHECK(u.position(u.r.glide) == 1.0);
  REQUIRE(u.ctx.undo());
  CHECK(u.position(u.r.glide) == 0.0);
}

TEST_CASE("render: module-delay-tap-micro and module-delay-tap-crossfade (DT.4: the micro bar open at 48 samples; Crossfade, "
          "2x)") {
  doc::DelayTapOptions micro;
  micro.time = placeOf(48.0 / kRate);
  checkImage("module-delay-tap-micro", panelImage(micro, 0.0f, true));
  doc::DelayTapOptions xf;
  xf.glide = 1;
  xf.time = placeOf(0.31);  // off every marking
  checkImage("module-delay-tap-crossfade", panelImage(xf));
}
