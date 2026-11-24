// AP.6 (docs/plans/AP.md "AP.6"; pq/document/note_devices.h noteDevicePanel; views panel/arp_elements.cpp): the
// Arpeggiator's device panel. Goldens of its main states drawn standalone at 1x (the Device view draws it 1:1): the
// default (every section folded, nothing held), the Ratio unit (with a free rate and Beat retrigger), Harmonic Walk (the
// walk row, Base, the Glide section open), Pulse on (its section open) and the step lanes on while a note stage plays
// (each lane's current step and the pattern's position lit from a NoteStageStatus). Real dispatch through the shell's
// Device view (window → chain → panel host → face → element): a lane bar dragged (one undo step, a param write only:
// not structural), a paint across steps, the length handle, the unit chips switching which controls show, a fold
// opened and closed (view state: no undo step, the frame follows the panel's height).
// AP.9: the same panel in the project patch view (the circuit): the block drawn as it (Node::typePanel), golden
// project-patch-arp; through the window: a control click (one undo step), a fold opening (the block grows in place), a
// cable into a param ending on its control, the notes pins on the band's spine.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "../ui/golden_check.h"
#include "pq/core/note_stage_status.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/note_devices.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/controls.h"
#include "pq/ui/test_driver.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/arp_elements.h"
#include "pq/views/panel/block_frame.h"
#include "pq/views/panel/element.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "pq/views/view_scenes.h"

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

void run(doc::DocumentStore& st, const doc::ops::EditResult& e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(st.dispatch(e->command).ok());
}

// A MIDI track "Keys" (the default instrument) with an Arpeggiator on its notes link.
struct Project {
  doc::TrackId keys;
  doc::DeviceId arp;
};
Project makeProject(ViewContext& ctx) {
  ctx.setDeviceLibrary(&devLib());
  doc::DocumentStore& st = ctx.store();
  Project p;
  auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys");
  run(st, t);
  p.keys = t->tracks.at(0);
  auto a = doc::ops::addDevice(*st.current(), st.ids(), p.keys, doc::noteDevicePatch("arpeggiator"), doc::DeviceKind::AudioEffect);
  run(st, a);
  p.arp = a->devices.at(0);
  return p;
}

const doc::NoteParamSpec& spec(std::string_view name) {
  const doc::NoteDeviceType* t = doc::findNoteDeviceType("arpeggiator");
  REQUIRE(t != nullptr);
  const doc::NoteParamSpec* s = t->param(name);
  REQUIRE(s != nullptr);
  return *s;
}
// Param `name` set to `value` in its units (a knob position on its declared line), or a setting's value.
void setValue(ViewContext& ctx, doc::DeviceId arp, std::string_view name, double value) {
  const doc::NoteParamSpec& s = spec(name);
  if (s.runtime) run(ctx.store(), doc::ops::setDeviceParam(*ctx.doc(), arp, std::string(name), doc::noteParamPosition(s, value)));
  else run(ctx.store(), doc::ops::setDeviceSetting(*ctx.doc(), arp, std::string(name), value));
}
double valueOf(const ViewContext& ctx, doc::DeviceId arp, std::string_view name) {
  const doc::Device* d = doc::findDevice(*ctx.doc(), arp).device;
  REQUIRE(d != nullptr);
  return doc::noteDeviceValue(*d, spec(name));
}

// The panel's element of `kind` whose title / text / label (or steplane lane) is `what`: its index in the face.
int32_t findElement(const panel::GroupPanelFace& f, std::string_view kind, std::string_view what) {
  for (size_t i = 0; i < f.elementCount(); ++i) {
    const doc::PanelElement& e = f.element(i).desc();
    if (e.kind != kind) continue;
    if (kind == "steplane" && std::to_string(std::lround(e.number("lane", -1.0))) == what) return static_cast<int32_t>(i);
    for (const char* field : {"title", "text", "label"})
      if (e.text(field, "") == what) return static_cast<int32_t>(i);
    if (const doc::Binding* b = e.binding(); b != nullptr && b->name == what) return static_cast<int32_t>(i);
  }
  return -1;
}

// The open state of the panel's folds (by title) set as view state before the panel is built.
void setFolds(ViewContext& ctx, doc::DeviceId arp, const doc::Panel& p, const std::vector<std::string>& open) {
  for (const doc::PanelElement& e : p.elements)
    if (e.kind == doc::kFoldKind && e.text("look", "") != "band")  // (AR.4: a band is no section header)
      ctx.store().view().panelWindows[arp.value].setFoldOpen(
          e.id, std::find(open.begin(), open.end(), std::string(e.text("title", ""))) != open.end());
}

// AP.10: what the note stage writes while it holds `held` (their identities, by Hz) and has just played pattern
// position `pos` of copy `cp`.
std::shared_ptr<NoteStageStatus> holding(const std::vector<NoteStageStatus::HeldNote>& held, int32_t pos, int32_t cp) {
  auto st = std::make_shared<NoteStageStatus>();
  const auto n = static_cast<uint32_t>(held.size());
  st->writePattern(n, n, false, [&](uint32_t i) { return held[i].hz; }, n, [&](uint32_t i) { return held[i]; });
  const int32_t lanes[6] = {0, 0, 0, 0, 0, 0};
  st->writeStep(lanes, 6, pos, cp);
  return st;
}
NoteStageStatus::HeldNote jiNote(float hz, float harmonic) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  return {hz, 0.8f, 1.0f, harmonic, nan, nan, 0.0f, nan};
}
NoteStageStatus::HeldNote degreeNote(float hz, float degree) { return {hz, 0.8f, 1.0f, 0.0f, degree, 0.0f, 0.0f, degree}; }
// AR.3: the face's first element of `kind` (to edit / ask through panel::test).
panel::Element& elementIn(panel::GroupPanelFace& f, std::string_view kind) {
  for (size_t i = 0; i < f.elementCount(); ++i)
    if (f.element(i).desc().kind == kind) return f.element(i);
  FAIL("no element of kind " << kind);
  return f.element(0);
}
const panel::Element* elementOf(const panel::GroupPanelFace& f, std::string_view kind) {
  for (size_t i = 0; i < f.elementCount(); ++i)
    if (f.element(i).desc().kind == kind) return &f.element(i);
  return nullptr;
}

// The arpeggiator's panel standalone (a PanelHost over the document's target), the driver its shown size at 1x.
struct Standalone {
  FakeTransport port;
  ViewContext ctx{port};
  Project pr;
  std::shared_ptr<NoteStageStatus> status;  // what the note stage would write (set by a test)
  std::unique_ptr<TestDriver> d;
  panel::PanelHost* host = nullptr;
  Standalone() { pr = makeProject(ctx); }
  ~Standalone() {
    if (d) d->tree().setRoot(std::make_unique<Widget>());
  }
  void build(const std::vector<std::string>& open, float scale = 1.0f) {
    const doc::Device* dev = doc::findDevice(*ctx.doc(), pr.arp).device;
    REQUIRE(dev != nullptr);
    auto p = std::make_shared<const doc::Panel>(doc::noteDevicePanel(*dev));
    REQUIRE_FALSE(p->empty());
    CHECK(doc::checkPanel(*p).ok());
    setFolds(ctx, pr.arp, *p, open);
    if (status) ctx.setNoteStatusSource([s = status](doc::DeviceId) { return std::shared_ptr<const NoteStageStatus>(s); });
    // (AR.4: a fold with a condition — a step lane's band — shows while its source's setting has the value)
    const doc::FoldLayout fl = doc::foldLayout(
        *p,
        [&](const doc::PanelElement& e) {
          return ctx.store().view().panelWindows[pr.arp.value].foldOpen(e.id).value_or(doc::foldOpenByDefault(e));
        },
        [&](const doc::PanelElement& e) {
          if (!e.when) return true;
          const doc::PanelElement* src = p->find(e.when->element);
          REQUIRE(src != nullptr);
          REQUIRE(src->binding() != nullptr);
          return valueOf(ctx, pr.arp, src->binding()->name) == e.when->values.at(0);
        });
    d = std::make_unique<TestDriver>(static_cast<uint32_t>(p->width * scale), static_cast<uint32_t>(std::ceil(fl.height * scale)), scale);
    auto face = std::make_unique<panel::GroupPanelFace>([p]() -> doc::PanelPtr { return p; },
                                                        panel::makeDocPanelTarget(ctx, pr.arp, [this]() -> Widget* { return host; }));
    host = &d->setRoot<panel::PanelHost>("panel", std::move(face));
    d->tree().layoutNow();
  }
  panel::GroupPanelFace& face() const { return static_cast<panel::GroupPanelFace&>(host->face()); }
  Image render() {
    REQUIRE(d->enableGpu().ok());
    host->wake();  // (the status: what a host frame tick reads)
    d->advance(0.1);
    auto img = d->render();
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    return std::move(*img);
  }
};

// The shell over the project, its Device view showing the Keys track (the arpeggiator left of the instrument).
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 1200};
  std::unique_ptr<Shell> shell;
  Project pr;
  Ui() {
    pr = makeProject(ctx);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(200.0f);
    ctx.setSelectedTrack(pr.keys);
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
  panel::PanelHost& host() {
    d.tree().layoutNow();
    panel::BlockFrame* f = view().panelOf(pr.arp);
    REQUIRE(f != nullptr);
    return f->host();
  }
  panel::GroupPanelFace& face() {
    panel::GroupPanelFace* g = host().panelFace();
    REQUIRE(g != nullptr);
    (void)g->sync();
    return *g;
  }
  int32_t index(std::string_view kind, std::string_view what) {
    const int32_t i = findElement(face(), kind, what);
    REQUIRE_MESSAGE(i >= 0, "no " << kind << " " << what);
    return i;
  }
  // A point of element `i` as the face shows it (fractions of its rect), device px.
  Vec2 at(int32_t i, float fx, float fy) {
    const Rect r = face().element(static_cast<size_t>(i)).rect();
    const Vec2 p = host().transform().map({r.x0 + (r.x1 - r.x0) * fx, r.y0 + (r.y1 - r.y0) * fy});
    const IRect hr = host().rect();
    const bool inside = p.x >= static_cast<float>(hr.x) && p.y >= static_cast<float>(hr.y) &&
                        p.x < static_cast<float>(hr.x + hr.w) && p.y < static_cast<float>(hr.y + hr.h);
    REQUIRE_MESSAGE(inside, "element off the shown panel");
    return p;
  }
  // AP.10: element-local point `l` of element `i`, device px.
  Vec2 atLocal(int32_t i, Vec2 l) {
    const Rect r = face().element(static_cast<size_t>(i)).rect();
    return host().transform().map({r.x0 + l.x, r.y0 + l.y});
  }
  // AR.4: step lane `l` (the status's lane index); its switch clicked; the middle of its step's cell at `fy` of its
  // height, device px.
  int32_t lane(int l) { return index("steplane", std::to_string(l)); }
  void laneOn(int32_t i) {
    d.click(atLocal(i, {10.0f, 7.0f}));
    d.step();
  }
  Vec2 cell(int32_t i, int step, float fy) {
    const std::optional<Rect> c = panel::test::stepLaneCell(face().element(static_cast<size_t>(i)), step);
    REQUIRE(c.has_value());
    return atLocal(i, {(c->x0 + c->x1) * 0.5f, c->y0 + (c->y1 - c->y0) * fy});
  }
  Menu* lastMenu() {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<Menu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  bool foldOpen(const char* title) {
    const int32_t f = index("fold", title);
    return face().element(static_cast<size_t>(f)).selection(face().target()).value_or(0.0) >= 0.5;
  }
  // A click on the header of fold `title` (through the window).
  void clickFold(const char* title) {
    const int32_t f = index("fold", title);
    d.click(at(f, 0.1f, 0.5f));
    d.step();
  }
  size_t history() const { return ctx.store().history().cursor(); }
};

}  // namespace

// ---- goldens ---------------------------------------------------------------------------------------------------------

TEST_CASE("render: arp-panel-default (AP.6, 1x)") {
  Standalone s;
  s.build({});
  checkImage("arp-panel-default", s.render());
}

TEST_CASE("render: arp-panel-ratio (AP.6: Ratio unit, a free rate, Beat retrigger)") {
  Standalone s;
  setValue(s.ctx, s.pr.arp, "unit", 1.0);
  setValue(s.ctx, s.pr.arp, "ratioP", 3.0);
  setValue(s.ctx, s.pr.arp, "ratioQ", 2.0);
  setValue(s.ctx, s.pr.arp, "sync", 0.0);
  setValue(s.ctx, s.pr.arp, "rateMs", 180.0);
  setValue(s.ctx, s.pr.arp, "retrigger", 2.0);
  setValue(s.ctx, s.pr.arp, "steps", 3.0);
  s.build({});
  checkImage("arp-panel-ratio", s.render());
}

TEST_CASE("render: arp-panel-walk (AP.6: Harmonic Walk in Harmonics, the Glide section open)") {
  Standalone s;
  setValue(s.ctx, s.pr.arp, "style", 18.0);  // Harmonic Walk
  setValue(s.ctx, s.pr.arp, "unit", 2.0);
  setValue(s.ctx, s.pr.arp, "walkUnder", 1.0);
  setValue(s.ctx, s.pr.arp, "intoOffset", 1200.0 * std::log2(6.0 / 8.0));  // AR.2: cents; h8 → h6 (what a drag snaps to)
  setValue(s.ctx, s.pr.arp, "outNext", 1.0);
  setValue(s.ctx, s.pr.arp, "intoShape", 2.0);  // Spring
  setValue(s.ctx, s.pr.arp, "intoCurve", 0.6);
  setValue(s.ctx, s.pr.arp, "velSource", 1.0);   // Fixed
  s.build({"Glide"});
  checkImage("arp-panel-walk", s.render());
}

TEST_CASE("render: arp-panel-pulse (AR.3: Pulse on, nothing held — four nominal rows, a Euclid row, a shifted row)") {
  Standalone s;
  setValue(s.ctx, s.pr.arp, "pulse", 1.0);
  setValue(s.ctx, s.pr.arp, "pulseDiv2", 8.0);
  setValue(s.ctx, s.pr.arp, "pulseMaskLo2", static_cast<double>(0x4949));  // Euclid 3 of 8
  setValue(s.ctx, s.pr.arp, "pulseMaskHi2", static_cast<double>(0x4949));
  setValue(s.ctx, s.pr.arp, "pulseShift3", 0.25);
  setValue(s.ctx, s.pr.arp, "layers", 2.0);
  s.build({"Pulse"});
  const Image img = s.render();
  panel::Element& pv = elementIn(s.face(), "arppulse");
  const panel::PanelTarget& t = s.face().target();
  REQUIRE(panel::test::pulseRows(pv, t) == 4);
  CHECK(panel::test::pulseRow(pv, t, 0).division == 3);
  CHECK_FALSE(panel::test::pulseRow(pv, t, 0).held);
  CHECK(panel::test::pulseRow(pv, t, 1).division == 8);
  CHECK(panel::test::pulseRow(pv, t, 1).mask == 0x49494949u);
  CHECK(panel::test::pulseRow(pv, t, 2).shift == doctest::Approx(0.25));
  CHECK(panel::test::pulseLit(pv, 0) == -1);  // nothing plays
  CHECK(panel::test::pulsePlayhead(pv) == -1.0f);
  checkImage("arp-panel-pulse", img);
}

TEST_CASE("render: arp-panel-pulse-chord (AR.3: a held 8:9:10:12 chord with Auto — rows locked to the harmonics, "
          "coinciding onsets joined, each row's last pulse lit, the playhead)") {
  Standalone s;
  setValue(s.ctx, s.pr.arp, "pulse", 1.0);
  const std::vector<NoteStageStatus::HeldNote> held = {jiNote(200.0f, 8.0f), jiNote(225.0f, 9.0f), jiNote(250.0f, 10.0f),
                                                       jiNote(300.0f, 12.0f)};
  s.status = std::make_shared<NoteStageStatus>();
  s.status->writePattern(4, 4, false, [&](uint32_t i) { return held[i].hz; }, 4, [&](uint32_t i) { return held[i]; });
  // What the stage writes as it plays: each row's last pulse, the newest (row 3's pulse 5 of 12) the playhead.
  s.status->writePulse(0, 3, 3.0f / 8.0f);
  s.status->writePulse(1, 3, 3.0f / 9.0f);
  s.status->writePulse(2, 4, 4.0f / 10.0f);
  s.status->writePulse(3, 5, 5.0f / 12.0f);
  const int32_t lanes[6] = {0, 0, 0, 0, 0, 0};
  s.status->writeStep(lanes, 6, 3, 0);
  s.build({"Pulse"});
  const Image img = s.render();
  panel::Element& pv = elementIn(s.face(), "arppulse");
  const panel::PanelTarget& t = s.face().target();
  REQUIRE(panel::test::pulseRows(pv, t) == 4);
  const int32_t want[4] = {8, 9, 10, 12};
  for (uint32_t r = 0; r < 4; ++r) {
    const panel::test::PulseRow row = panel::test::pulseRow(pv, t, r);
    CHECK(row.division == want[r]);
    CHECK(row.locked);  // Auto: its harmonic
    CHECK(row.held);
  }
  CHECK(panel::test::pulseLit(pv, 0) == 3);
  CHECK(panel::test::pulseLit(pv, 3) == 5);
  CHECK(panel::test::pulsePlayhead(pv) == doctest::Approx(5.0f / 12.0f));
  // Joined onsets: the cycle start (all four), 1/4, 1/2, 3/4 (8 with 12), 1/2 also 10; 1/3, 2/3 (9 with 12): 6 places.
  CHECK(panel::test::pulseLinks(pv) == 6);
  checkImage("arp-panel-pulse-chord", img);
  // Auto off: the same notes pulse at their rows' divisions (3, 4, 5, 6), unlocked.
  setValue(s.ctx, s.pr.arp, "pulseAuto", 0.0);
  CHECK(panel::test::pulseRow(pv, t, 0).division == 3);
  CHECK_FALSE(panel::test::pulseRow(pv, t, 0).locked);
  CHECK(panel::test::pulseRow(pv, t, 3).division == 6);
}

TEST_CASE("arp panel (AR.3): the pulse rows recompute only when their inputs change, not per frame") {
  Standalone s;
  setValue(s.ctx, s.pr.arp, "pulse", 1.0);
  s.status = holding({jiNote(200.0f, 4.0f), jiNote(250.0f, 5.0f)}, 0, 0);
  s.build({"Pulse"});
  (void)s.render();
  panel::Element& pv = elementIn(s.face(), "arppulse");
  const uint64_t c0 = panel::test::pulseComputes(pv);
  CHECK(c0 >= 1);
  for (int i = 0; i < 5; ++i) {
    s.host->wake();
    s.d->advance(0.05);
    (void)s.d->render();
  }
  CHECK(panel::test::pulseComputes(pv) == c0);
  setValue(s.ctx, s.pr.arp, "pulseDiv3", 7.0);
  s.host->wake();
  s.d->advance(0.05);
  (void)s.d->render();
  CHECK(panel::test::pulseComputes(pv) == c0 + 1);
  CHECK(panel::test::pulseRow(pv, s.face().target(), 2).division == 7);
}

namespace {
// AR.4: Velocity, Gate, Transpose and Skip / Tie on with values (Glide and Chance off: their strips), the stage playing
// grid step 5.
void lanesScene(Standalone& s) {
  setValue(s.ctx, s.pr.arp, "velLane", 1.0);
  setValue(s.ctx, s.pr.arp, "gateLane", 1.0);
  setValue(s.ctx, s.pr.arp, "transLane", 1.0);
  setValue(s.ctx, s.pr.arp, "tieLane", 1.0);
  setValue(s.ctx, s.pr.arp, "velLen", 8.0);
  setValue(s.ctx, s.pr.arp, "transLen", 12.0);
  setValue(s.ctx, s.pr.arp, "tieLen", 6.0);
  for (int i = 0; i < 8; ++i) setValue(s.ctx, s.pr.arp, "velStep" + std::to_string(i + 1), 0.25 + 0.1 * i);
  for (int i = 0; i < 16; ++i) setValue(s.ctx, s.pr.arp, "gateStep" + std::to_string(i + 1), i % 4 == 0 ? 1.6 : (i % 2 == 0 ? 1.0 : 0.5));
  const int trans[12] = {0, 7, 3, 0, -5, 12, 2, 0, -12, -2, 5, 0};
  for (int i = 0; i < 12; ++i) setValue(s.ctx, s.pr.arp, "transStep" + std::to_string(i + 1), trans[i]);
  setValue(s.ctx, s.pr.arp, "tieStep3", 1.0);  // Skip
  setValue(s.ctx, s.pr.arp, "tieStep4", 2.0);  // Tie
  // What the stage writes while it plays: grid step 5 (each lane at 5 mod its length), four notes held, the pattern
  // (UpDown: 6 positions) at position 2.
  s.status = std::make_shared<NoteStageStatus>();
  const float hz[6] = {261.63f, 327.03f, 392.0f, 523.25f, 392.0f, 327.03f};
  const NoteStageStatus::HeldNote held[4] = {degreeNote(261.63f, 0.0f), degreeNote(327.03f, 4.0f), degreeNote(392.0f, 7.0f),
                                             degreeNote(523.25f, 12.0f)};
  s.status->writePattern(6, 4, false, [&](uint32_t i) { return hz[i]; }, 4, [&](uint32_t i) { return held[i]; });
  const int32_t lanes[6] = {5, 5, 5, 5, 5, 5};
  s.status->writeStep(lanes, 6, 2, 0);
}
}  // namespace

TEST_CASE("render: arp-panel-lanes (AP.6 / AR.4: lanes on — bars, the Transpose grid, cells —, two off, the playing step lit)") {
  Standalone s;
  lanesScene(s);
  s.build({"Step Lanes"});
  {  // the pointer over Transpose step 2: its cell outlined, the readout under the lane's name
    const int32_t trans = findElement(s.face(), "steplane", "3");
    REQUIRE(trans >= 0);
    const panel::Element& e = s.face().element(static_cast<size_t>(trans));
    const Rect c = *panel::test::stepLaneCell(e, 1);
    (void)s.d->move({e.rect().x0 + (c.x0 + c.x1) * 0.5f, e.rect().y0 + 70.0f});
  }
  const Image img = s.render();
  const int32_t vel = findElement(s.face(), "steplane", "0");
  REQUIRE(vel >= 0);
  CHECK(panel::test::stepLaneLit(s.face().element(static_cast<size_t>(vel))) == 5);
  const int32_t glide = findElement(s.face(), "steplane", "2");
  REQUIRE(glide >= 0);
  CHECK(panel::test::stepLaneLit(s.face().element(static_cast<size_t>(glide))) == -1);  // off: it reads no status
  CHECK(s.face().element(static_cast<size_t>(glide)).rect().height() == doctest::Approx(14.0f));
  for (size_t i = 0; i < s.face().elementCount(); ++i)
    if (s.face().element(i).desc().kind == "arppattern") {
      CHECK(panel::test::patternPosition(s.face().element(i)) == 2);
      CHECK(panel::test::patternCount(s.face().element(i)) == 6);
    }
  checkImage("arp-panel-lanes", img);
}

TEST_CASE("render: arp-panel-lanes-2x (AR.4: the same at 2x, the rate's n:m numbers shown, Harmonic Walk's row)") {
  Standalone s;
  lanesScene(s);
  setValue(s.ctx, s.pr.arp, "rateNum", 3.0);
  setValue(s.ctx, s.pr.arp, "tupletN", 5.0);
  setValue(s.ctx, s.pr.arp, "tupletM", 4.0);
  setValue(s.ctx, s.pr.arp, "retrigger", 2.0);
  setValue(s.ctx, s.pr.arp, "velSource", 1.0);
  setValue(s.ctx, s.pr.arp, "style", 18.0);
  setValue(s.ctx, s.pr.arp, "unit", 2.0);
  for (int i = 0; i < 12; ++i) setValue(s.ctx, s.pr.arp, "transStep" + std::to_string(i + 1), (i * 5) % 9 - 4);
  s.build({"Step Lanes"}, 2.0f);
  checkImage("arp-panel-lanes-2x", s.render());
}

// ---- AP.10: the resolved pattern, the bends, the bend view --------------------------------------------------------------

TEST_CASE("render: arp-panel-walk-chord (AP.10: Harmonic Walk over a held JI chord, the generated harmonics dotted)") {
  Standalone s;
  setValue(s.ctx, s.pr.arp, "style", 18.0);  // Harmonic Walk
  setValue(s.ctx, s.pr.arp, "unit", 2.0);    // Harmonics
  setValue(s.ctx, s.pr.arp, "walkRange", 4.0);
  setValue(s.ctx, s.pr.arp, "walkOrder", 2.0);  // UpDown
  // h4 h5 h6 of 50 Hz held: the walks 4…8, 5…9, 6…10 merge to 4 … 10 (7 elements); UpDown: 12 steps.
  s.status = holding({jiNote(200.0f, 4.0f), jiNote(250.0f, 5.0f), jiNote(300.0f, 6.0f)}, 3, 0);
  s.build({});
  const Image img = s.render();
  const panel::Element* pv = elementOf(s.face(), "arppattern");
  REQUIRE(pv != nullptr);
  CHECK(panel::test::resolvedCount(*pv) == 12);
  CHECK(panel::test::resolvedGenerated(*pv) == 12 - 5);  // the held h4 (once), h5 and h6 (up and down) are sources
  CHECK(panel::test::resolvedLit(*pv) == 3);              // position 3 (h7) of copy 0
  checkImage("arp-panel-walk-chord", img);
}

TEST_CASE("render: arp-panel-degrees-bends (AP.10: Distance / Steps in Degrees with Into + Out bends on)") {
  Standalone s;
  setValue(s.ctx, s.pr.arp, "distance", 7.0);
  setValue(s.ctx, s.pr.arp, "steps", 3.0);
  setValue(s.ctx, s.pr.arp, "gate", 0.85);
  setValue(s.ctx, s.pr.arp, "intoOffset", -200.0);  // AR.2: cents (−2 degrees of 12-EDO)
  setValue(s.ctx, s.pr.arp, "intoTime", 0.3);
  setValue(s.ctx, s.pr.arp, "intoCurve", 0.7);  // exponential
  setValue(s.ctx, s.pr.arp, "outNext", 1.0);
  setValue(s.ctx, s.pr.arp, "outTime", 0.3);
  setValue(s.ctx, s.pr.arp, "outShape", 1.0);  // S
  s.status = holding({degreeNote(261.63f, 0.0f), degreeNote(329.63f, 4.0f)}, 1, 1);
  s.build({"Glide"});
  const Image img = s.render();
  const panel::Element* pv = elementOf(s.face(), "arppattern");
  REQUIRE(pv != nullptr);
  CHECK(panel::test::resolvedCount(*pv) == 6);      // 2 notes × Steps 3
  CHECK(panel::test::resolvedGenerated(*pv) == 4);  // copies 1 and 2: +7, +14 degrees
  CHECK(panel::test::resolvedLit(*pv) == 3);        // position 1 of copy 1
  checkImage("arp-panel-degrees-bends", img);
}

TEST_CASE("render: arp-panel-bend-view (AP.10: asymmetric Into / Out — shapes, free offsets; AP.11: overlapping % "
          "times, the Into past the note's end; nothing held)") {
  Standalone s;
  setValue(s.ctx, s.pr.arp, "gate", 1.0);
  setValue(s.ctx, s.pr.arp, "intoOffset", -300.0);
  setValue(s.ctx, s.pr.arp, "intoTime", 1.2);  // past the note's end: drawn faint there
  setValue(s.ctx, s.pr.arp, "intoCurve", 0.8);  // exponential
  setValue(s.ctx, s.pr.arp, "outOffset", 150.0);  // (off the 12-EDO grid: read in cents)
  setValue(s.ctx, s.pr.arp, "outTime", 0.5);  // overlapping the Into: the curves add
  setValue(s.ctx, s.pr.arp, "outShape", 2.0);  // Spring
  setValue(s.ctx, s.pr.arp, "outCurve", 0.6);
  s.build({"Glide"});
  const Image img = s.render();
  const panel::Element* pv = elementOf(s.face(), "arppattern");
  REQUIRE(pv != nullptr);
  CHECK(panel::test::resolvedCount(*pv) == 1);  // the nominal note
  checkImage("arp-panel-bend-view", img);
}

TEST_CASE("arp panel (AP.10): the pattern recomputes only when its inputs change, not per frame") {
  Standalone s;
  s.status = holding({degreeNote(261.63f, 0.0f), degreeNote(329.63f, 4.0f)}, 0, 0);
  s.build({});
  (void)s.render();
  const panel::Element* pv = elementOf(s.face(), "arppattern");
  REQUIRE(pv != nullptr);
  const uint64_t c0 = panel::test::resolvedComputes(*pv);
  CHECK(c0 >= 1);
  for (int i = 0; i < 5; ++i) {
    s.host->wake();
    s.d->advance(0.05);
    (void)s.d->render();
  }
  CHECK(panel::test::resolvedComputes(*pv) == c0);
  CHECK(panel::test::resolvedCount(*pv) == 2);
  setValue(s.ctx, s.pr.arp, "steps", 2.0);
  s.host->wake();
  s.d->advance(0.05);
  (void)s.d->render();
  CHECK(panel::test::resolvedComputes(*pv) == c0 + 1);
  CHECK(panel::test::resolvedCount(*pv) == 4);
}

TEST_CASE("arp panel: a condition of several sources with all-of shows only while every term holds") {
  Standalone s;
  const doc::Device* dev = doc::findDevice(*s.ctx.doc(), s.pr.arp).device;
  REQUIRE(dev != nullptr);
  doc::Panel p = doc::noteDevicePanel(*dev);
  doc::PanelElement* base = nullptr;
  for (doc::PanelElement& e : p.elements)
    if (const doc::Binding* b = e.binding(); b != nullptr && b->name == "harmonicBase") base = &e;
  REQUIRE(base != nullptr);
  REQUIRE(base->when.has_value());
  REQUIRE(base->when->more.size() == 1);
  base->when->all = true;
  CHECK(doc::checkPanel(p).ok());
  auto shared = std::make_shared<const doc::Panel>(p);
  TestDriver d(800, 600);
  panel::PanelHost* host = nullptr;
  auto face = std::make_unique<panel::GroupPanelFace>([shared]() -> doc::PanelPtr { return shared; },
                                                      panel::makeDocPanelTarget(s.ctx, s.pr.arp, [&host]() -> Widget* { return host; }));
  host = &d.setRoot<panel::PanelHost>("panel", std::move(face));
  auto& f = static_cast<panel::GroupPanelFace&>(host->face());
  const int32_t i = findElement(f, "number", "harmonicBase");
  REQUIRE(i >= 0);
  auto shownNow = [&] {
    (void)f.sync();
    return f.shown(static_cast<size_t>(i));
  };
  CHECK_FALSE(shownNow());
  setValue(s.ctx, s.pr.arp, "unit", 2.0);
  CHECK_FALSE(shownNow());  // Harmonics alone
  setValue(s.ctx, s.pr.arp, "style", 18.0);
  CHECK(shownNow());  // and Harmonic Walk
  setValue(s.ctx, s.pr.arp, "unit", 0.0);
  CHECK_FALSE(shownNow());  // Harmonic Walk alone
  d.tree().setRoot(std::make_unique<Widget>());
}

// ---- real dispatch in the Device view ----------------------------------------------------------------------------------

TEST_CASE("arp panel (AR.4): a lane switched off is its name strip; its switch grows it (one undo step) and the panel follows") {
  Ui u;
  u.clickFold("Glide");  // (folded: every lane is in the window)
  u.clickFold("Step Lanes");
  const int32_t vel = u.lane(0), trans = u.lane(3);
  CHECK(u.face().element(static_cast<size_t>(vel)).rect().height() == doctest::Approx(14.0f));
  CHECK(u.face().element(static_cast<size_t>(trans)).rect().height() == doctest::Approx(14.0f));
  const float h0 = u.host().face().worldSize().y;
  const float transY = u.face().element(static_cast<size_t>(trans)).rect().y0;
  const size_t hist = u.history();
  u.laneOn(vel);
  CHECK(valueOf(u.ctx, u.pr.arp, "velLane") == doctest::Approx(1.0));
  CHECK(u.history() == hist + 1);
  CHECK(u.face().element(static_cast<size_t>(vel)).rect().height() == doctest::Approx(44.0f));
  CHECK(u.host().face().worldSize().y == doctest::Approx(h0 + 30.0f));
  CHECK(u.face().element(static_cast<size_t>(trans)).rect().y0 == doctest::Approx(transY + 30.0f));  // the lanes below move
  CHECK(u.view().panelOf(u.pr.arp)->logicalHeight() >= h0 + 30.0f);
  u.laneOn(trans);
  CHECK(u.face().element(static_cast<size_t>(trans)).rect().height() == doctest::Approx(96.0f));
  // A press on a lane that is off, right of its name column, is not the lane's (nothing there).
  const int32_t gate = u.lane(1);
  const size_t h1 = u.history();
  u.d.click(u.atLocal(gate, {300.0f, 7.0f}));
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "gateStep9") == doctest::Approx(1.0));
  CHECK(u.history() == h1);
  u.laneOn(vel);  // off again
  CHECK(valueOf(u.ctx, u.pr.arp, "velLane") == doctest::Approx(0.0));
  CHECK(u.face().element(static_cast<size_t>(vel)).rect().height() == doctest::Approx(14.0f));
}

TEST_CASE("arp panel: a lane bar dragged is one undo step and a param write (no structural change)") {
  Ui u;
  REQUIRE_FALSE(u.foldOpen("Step Lanes"));
  u.clickFold("Glide");
  u.clickFold("Step Lanes");
  REQUIRE(u.foldOpen("Step Lanes"));
  const int32_t lane = u.lane(0);  // Velocity
  u.laneOn(lane);
  const size_t h0 = u.history();
  // Step 4 (index 3): press at its bottom, drag to its top.
  u.d.drag(u.cell(lane, 3, 0.95f), u.cell(lane, 3, 0.0f), 6);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "velStep4") == doctest::Approx(1.0));
  // Then to the bottom: 0.
  u.d.drag(u.cell(lane, 3, 0.2f), u.cell(lane, 3, 1.0f), 6);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "velStep4") == doctest::Approx(0.0));
  REQUIRE(u.history() == h0 + 2);
  CHECK_FALSE(u.ctx.store().history().entries()[h0].changes.structural());  // a runtime param: a slot write
  CHECK_FALSE(u.ctx.store().history().entries()[h0 + 1].changes.structural());
  CHECK(valueOf(u.ctx, u.pr.arp, "velStep3") == doctest::Approx(1.0));  // its neighbours untouched (the default)
  CHECK(valueOf(u.ctx, u.pr.arp, "velStep5") == doctest::Approx(1.0));
  REQUIRE(u.ctx.undo());
  CHECK(valueOf(u.ctx, u.pr.arp, "velStep4") == doctest::Approx(1.0));
}

TEST_CASE("arp panel (AR.4): a drag across steps paints them in one undo step; double-click types a value; Alt+click resets; "
          "the wheel fine-adjusts (a run: one undo step)") {
  Ui u;
  u.clickFold("Glide");
  u.clickFold("Step Lanes");
  const int32_t lane = u.lane(1);  // Gate (0 … 2, default 1)
  u.laneOn(lane);
  const size_t h0 = u.history();
  // Press on step 1 at three quarters down its height (a quarter of the range), sweep to step 8 at the same height
  // (a fast drag: 2 moves, the steps between painted too).
  u.d.drag(u.cell(lane, 0, 0.75f), u.cell(lane, 7, 0.75f), 2);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  for (int i = 1; i <= 8; ++i) CHECK(valueOf(u.ctx, u.pr.arp, "gateStep" + std::to_string(i)) == doctest::Approx(0.5).epsilon(0.1));
  CHECK(valueOf(u.ctx, u.pr.arp, "gateStep9") == doctest::Approx(1.0));
  // A double-click on step 2 types its value in percent; Enter sets it.
  u.d.doubleClick(u.cell(lane, 1, 0.75f));
  u.d.step();
  REQUIRE(panel::test::stepLaneTyping(u.face().element(static_cast<size_t>(lane))));
  CHECK(u.d.type("150"));
  (void)u.d.press("Enter");
  u.d.step();
  CHECK_FALSE(panel::test::stepLaneTyping(u.face().element(static_cast<size_t>(lane))));
  CHECK(valueOf(u.ctx, u.pr.arp, "gateStep2") == doctest::Approx(1.5));
  CHECK(valueOf(u.ctx, u.pr.arp, "gateStep3") == doctest::Approx(0.5).epsilon(0.1));
  // Escape leaves the value.
  u.d.doubleClick(u.cell(lane, 1, 0.25f));
  u.d.step();
  CHECK(u.d.type("20"));
  (void)u.d.press("Escape");
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "gateStep2") == doctest::Approx(1.5));
  // Alt+click: the default.
  u.d.click(u.cell(lane, 1, 0.9f), mod::kAlt);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "gateStep2") == doctest::Approx(1.0));
  // The wheel over step 2: 1 % a notch, Shift: 10 %; the run is one undo step.
  const size_t h1 = u.history();
  CHECK(u.d.wheel(u.cell(lane, 1, 0.5f), 1.0f));
  CHECK(u.d.wheel(u.cell(lane, 1, 0.5f), 1.0f));
  CHECK(valueOf(u.ctx, u.pr.arp, "gateStep2") == doctest::Approx(1.02));
  CHECK(u.d.wheel(u.cell(lane, 1, 0.5f), -1.0f, 0.0f, mod::kShift));
  CHECK(valueOf(u.ctx, u.pr.arp, "gateStep2") == doctest::Approx(0.92));
  u.d.click(u.atLocal(lane, {40.0f, 30.0f}));  // (a press ends the run; the name column below the strip takes nothing)
  u.d.step();
  CHECK(u.history() == h1 + 1);
  REQUIRE(u.ctx.undo());
  CHECK(valueOf(u.ctx, u.pr.arp, "gateStep2") == doctest::Approx(1.0));
}

TEST_CASE("arp panel: the length handle drags a lane's length (one undo step); Skip / Tie cells cycle") {
  Ui u;
  u.clickFold("Glide");  // (AP.10: open by default; folded, the last lane is in the window)
  u.clickFold("Step Lanes");
  const int32_t lane = u.lane(4);  // Chance
  u.laneOn(lane);
  REQUIRE(valueOf(u.ctx, u.pr.arp, "chanceLen") == doctest::Approx(16.0));
  const size_t h0 = u.history();
  // From the grip after step 16 to the boundary after step 7.
  const Rect c16 = *panel::test::stepLaneCell(u.face().element(static_cast<size_t>(lane)), 15);
  const Rect c7 = *panel::test::stepLaneCell(u.face().element(static_cast<size_t>(lane)), 6);
  u.d.drag(u.atLocal(lane, {c16.x1 + 1.0f, 20.0f}), u.atLocal(lane, {c7.x1 + 1.0f, 20.0f}), 5);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "chanceLen") == doctest::Approx(7.0));
  CHECK(u.history() == h0 + 1);
  // Skip / Tie: a click on a cell cycles Play → Skip → Tie; the wheel steps back.
  const int32_t tie = u.lane(5);
  u.laneOn(tie);
  const Vec2 c = u.cell(tie, 2, 0.5f);
  u.d.click(c);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "tieStep3") == doctest::Approx(1.0));
  u.d.click(c);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "tieStep3") == doctest::Approx(2.0));
  CHECK(u.d.wheel(c, -1.0f));
  CHECK(valueOf(u.ctx, u.pr.arp, "tieStep3") == doctest::Approx(1.0));
}

TEST_CASE("arp panel (AR.4): the Transpose lane is a grid — a row per step of the unit, the rows fit the values in whole "
          "equaves; wheel one step, Shift+wheel an equave; a typed value") {
  Ui u;
  u.clickFold("Glide");
  u.clickFold("Step Lanes");
  const int32_t lane = u.lane(3);
  u.laneOn(lane);
  auto el = [&]() -> panel::Element& { return u.face().element(static_cast<size_t>(lane)); };
  auto row = [&](int step, int value) {
    const Rect c = *panel::test::stepLaneCell(el(), step);
    return u.atLocal(lane, {(c.x0 + c.x1) * 0.5f, panel::test::stepLaneRowY(el(), u.face().target(), value)});
  };
  CHECK(panel::test::stepLaneRange(el(), u.face().target()) == 7);  // nothing set: ± 7
  const size_t h0 = u.history();
  // A click on a row sets the step to it; a drag paints the row across steps.
  u.d.click(row(1, 5));
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "transStep2") == doctest::Approx(5.0));
  u.d.drag(row(3, -3), row(6, -3), 3);
  u.d.step();
  for (int i = 4; i <= 7; ++i) CHECK(valueOf(u.ctx, u.pr.arp, "transStep" + std::to_string(i)) == doctest::Approx(-3.0));
  CHECK(u.history() == h0 + 2);
  CHECK_FALSE(u.ctx.store().history().entries()[h0].changes.structural());
  // Dragged past the top edge the rows go on (one row per row height): beyond ± 7, and the grid then shows a whole
  // equave (the default tuning: 12 degrees).
  const Vec2 from = row(0, 6);
  const float rowPx = row(0, 5).y - row(0, 6).y;
  u.d.drag(from, {from.x, from.y - 4.0f * rowPx}, 4);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "transStep1") == doctest::Approx(10.0));
  CHECK(panel::test::stepLaneRange(el(), u.face().target()) == 12);
  // The wheel: one step; Shift: an equave. One undo step for the run.
  const size_t h1 = u.history();
  CHECK(u.d.wheel(row(1, 5), 1.0f));
  CHECK(valueOf(u.ctx, u.pr.arp, "transStep2") == doctest::Approx(6.0));
  CHECK(u.d.wheel(row(1, 6), 1.0f, 0.0f, mod::kShift));
  CHECK(valueOf(u.ctx, u.pr.arp, "transStep2") == doctest::Approx(18.0));
  CHECK(u.d.wheel(row(1, 6), -1.0f, 0.0f, mod::kShift));
  CHECK(u.d.wheel(row(1, 6), -1.0f, 0.0f, mod::kShift));
  CHECK(valueOf(u.ctx, u.pr.arp, "transStep2") == doctest::Approx(-6.0));
  u.d.click(u.atLocal(lane, {40.0f, 60.0f}));
  u.d.step();
  CHECK(u.history() == h1 + 1);
  CHECK(panel::test::stepLaneRange(el(), u.face().target()) == 12);
  // A typed value (beyond the rows shown): the grid grows to hold it.
  u.d.doubleClick(row(9, 0));
  u.d.step();
  REQUIRE(panel::test::stepLaneTyping(el()));
  CHECK(u.d.type("-19"));
  (void)u.d.press("Enter");
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "transStep10") == doctest::Approx(-19.0));
  CHECK(panel::test::stepLaneRange(el(), u.face().target()) == 24);
  // Alt+click: 0.
  u.d.click(row(9, -19), mod::kAlt);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "transStep10") == doctest::Approx(0.0));
  // Ratio has no equave: the rows grow in sevens.
  setValue(u.ctx, u.pr.arp, "unit", 1.0);
  u.d.step();
  CHECK(panel::test::stepLaneRange(el(), u.face().target()) == 14);
}

TEST_CASE("arp panel (AR.4): the lane menu — Reset, Randomize (from the seed), Shift, Reverse, Copy to: each one undo step") {
  Ui u;
  u.clickFold("Glide");
  u.clickFold("Step Lanes");
  const int32_t lane = u.lane(0);  // Velocity
  u.laneOn(lane);
  setValue(u.ctx, u.pr.arp, "velLen", 4.0);
  for (int i = 0; i < 4; ++i) setValue(u.ctx, u.pr.arp, "velStep" + std::to_string(i + 1), 0.2 * (i + 1));
  u.d.step();
  auto vel = [&](int i) { return valueOf(u.ctx, u.pr.arp, "velStep" + std::to_string(i)); };
  auto pick = [&](std::string_view label, bool dots = false) {
    // (the dots at the end of the name strip, or a right-click anywhere on the name column)
    if (dots) u.d.click(u.atLocal(lane, {74.0f, 7.0f}));
    else u.d.click(u.atLocal(lane, {30.0f, 30.0f}), mod::kNone, PointerButton::Right);
    Menu* m = u.lastMenu();
    REQUIRE(m != nullptr);
    for (size_t i = 0; i < m->items().size(); ++i)
      if (m->items()[i].label == label) {
        m->choose(static_cast<int32_t>(i));
        u.d.step();
        return;
      }
    FAIL("no menu item " << label);
  };
  size_t h = u.history();
  pick("Shift Right", true);
  CHECK(vel(1) == doctest::Approx(0.8));
  CHECK(vel(2) == doctest::Approx(0.2));
  CHECK(vel(4) == doctest::Approx(0.6));
  CHECK(vel(5) == doctest::Approx(1.0));  // past the length: untouched
  CHECK(u.history() == ++h);
  pick("Shift Left");
  CHECK(vel(1) == doctest::Approx(0.2));
  CHECK(vel(4) == doctest::Approx(0.8));
  CHECK(u.history() == ++h);
  pick("Reverse");
  CHECK(vel(1) == doctest::Approx(0.8));
  CHECK(vel(4) == doctest::Approx(0.2));
  CHECK(u.history() == ++h);
  CHECK_FALSE(u.ctx.store().history().entries()[h - 1].changes.structural());
  // Copy to Gate: the values and the length.
  {
    u.d.click(u.atLocal(lane, {30.0f, 30.0f}), mod::kNone, PointerButton::Right);
    Menu* m = u.lastMenu();
    REQUIRE(m != nullptr);
    int32_t copy = -1;
    for (size_t i = 0; i < m->items().size(); ++i)
      if (m->items()[i].label == "Copy to") copy = static_cast<int32_t>(i);
    REQUIRE(copy >= 0);
    m->setHighlighted(copy);
    Menu* sub = m->openSubmenu();
    REQUIRE(sub != nullptr);
    REQUIRE(sub->items().size() == 3);
    CHECK(sub->items()[0].label == "Gate");
    sub->choose(0);
    u.d.step();
    CHECK(valueOf(u.ctx, u.pr.arp, "gateStep1") == doctest::Approx(0.8));
    CHECK(valueOf(u.ctx, u.pr.arp, "gateStep4") == doctest::Approx(0.2));
    CHECK(valueOf(u.ctx, u.pr.arp, "gateLen") == doctest::Approx(4.0));
    CHECK(u.history() == ++h);
    REQUIRE(u.ctx.undo());
    CHECK(valueOf(u.ctx, u.pr.arp, "gateStep1") == doctest::Approx(1.0));
    CHECK(valueOf(u.ctx, u.pr.arp, "gateLen") == doctest::Approx(16.0));
    REQUIRE(u.ctx.redo());
  }
  // Randomize: the steps in play, one undo step; again: other values.
  pick("Randomize");
  CHECK(u.history() == ++h);
  const double r1[4] = {vel(1), vel(2), vel(3), vel(4)};
  CHECK((r1[0] != doctest::Approx(0.8) || r1[3] != doctest::Approx(0.2)));
  CHECK(vel(5) == doctest::Approx(1.0));
  pick("Randomize");
  CHECK(u.history() == ++h);
  CHECK((vel(1) != doctest::Approx(r1[0]) || vel(2) != doctest::Approx(r1[1])));
  // Reset: every step its default.
  pick("Reset");
  CHECK(u.history() == ++h);
  for (int i = 1; i <= 4; ++i) CHECK(vel(i) == doctest::Approx(1.0));
  REQUIRE(u.ctx.undo());
  CHECK(vel(1) != doctest::Approx(1.0));
  // A right-click on a step is not the lane's menu (the host's: the step's automation).
  const size_t menus = [&] {
    size_t n = 0;
    for (size_t i = 0; i < u.shell->overlay().childCount(); ++i) n += dynamic_cast<Menu*>(&u.shell->overlay().child(i)) != nullptr;
    return n;
  }();
  CHECK(menus == 0);
  u.d.click(u.cell(lane, 2, 0.5f), mod::kNone, PointerButton::Right);
  Menu* host = u.lastMenu();
  REQUIRE(host != nullptr);
  bool reset = false;
  for (const MenuItem& it : host->items()) reset = reset || it.label == "Randomize";
  CHECK_FALSE(reset);
}

TEST_CASE("arp panel (AR.4): one Rate control — the note value's menu and wheel (one undo step), the feel chips, n:m for "
          "the numbers themselves") {
  Ui u;
  const int32_t rate = u.index("rate", "rateNum");
  const int32_t feel = u.index("presets", "tupletN");
  const int32_t num = u.index("number", "rateNum"), tupN = u.index("number", "tupletN");
  auto shown = [&](int32_t i) { return u.face().shown(static_cast<size_t>(i)); };
  CHECK(shown(rate));
  CHECK(shown(feel));
  CHECK_FALSE(shown(num));
  CHECK_FALSE(shown(tupN));
  // The menu: 1/8 (the numerator back to 1, the tuplet kept).
  setValue(u.ctx, u.pr.arp, "tupletN", 3.0);
  setValue(u.ctx, u.pr.arp, "tupletM", 2.0);
  u.d.step();
  size_t h = u.history();
  u.d.click(u.atLocal(rate, {20.0f, 19.0f}));
  Menu* m = u.lastMenu();
  REQUIRE(m != nullptr);
  REQUIRE(m->items().size() == 8);
  CHECK(m->items()[4].label == "1/16");
  CHECK(m->items()[4].checked);
  m->choose(3);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "rateDen") == doctest::Approx(8.0));
  CHECK(valueOf(u.ctx, u.pr.arp, "rateNum") == doctest::Approx(1.0));
  CHECK(valueOf(u.ctx, u.pr.arp, "tupletN") == doctest::Approx(3.0));
  CHECK(u.history() == ++h);
  // The wheel: the next note value (up: shorter); a run is one undo step.
  CHECK(u.d.wheel(u.atLocal(rate, {20.0f, 19.0f}), 1.0f));
  CHECK(u.d.wheel(u.atLocal(rate, {20.0f, 19.0f}), 1.0f));
  CHECK(valueOf(u.ctx, u.pr.arp, "rateDen") == doctest::Approx(32.0));
  u.d.click(u.at(feel, 0.15f, 0.5f));  // Straight (the press ends the wheel run first)
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "tupletN") == doctest::Approx(1.0));
  CHECK(valueOf(u.ctx, u.pr.arp, "tupletM") == doctest::Approx(1.0));
  CHECK(u.history() == h + 2);
  h += 2;
  // n:m: the four numbers show in place of the feel chips (view state: no undo step).
  u.d.click(u.atLocal(rate, {70.0f, 19.0f}));
  u.d.step();
  CHECK(u.history() == h);
  CHECK(shown(num));
  CHECK(shown(tupN));
  CHECK_FALSE(shown(feel));
  u.d.click(u.atLocal(rate, {70.0f, 19.0f}));
  u.d.step();
  CHECK_FALSE(shown(num));
  CHECK(shown(feel));
  // A rate that is none of the menu's shows its numbers anyway.
  setValue(u.ctx, u.pr.arp, "rateNum", 3.0);
  u.d.step();
  CHECK(shown(num));
  CHECK_FALSE(shown(feel));
  // Free: the ms box alone.
  setValue(u.ctx, u.pr.arp, "sync", 0.0);
  u.d.step();
  CHECK_FALSE(shown(rate));
  CHECK_FALSE(shown(num));
  CHECK(shown(u.index("number", "rateMs")));
}

TEST_CASE("arp panel: the unit chips show Ratio p / q only in Ratio and Base only in Harmonics") {
  Ui u;
  const int32_t unit = u.index("tabs", "unit"), p = u.index("number", "ratioP"), base = u.index("number", "harmonicBase");
  CHECK_FALSE(u.face().shown(static_cast<size_t>(p)));
  CHECK_FALSE(u.face().shown(static_cast<size_t>(base)));
  u.d.click(u.at(unit, 0.5f, 0.5f));  // Ratio (the middle chip)
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "unit") == doctest::Approx(1.0));
  CHECK(u.face().shown(static_cast<size_t>(p)));
  CHECK_FALSE(u.face().shown(static_cast<size_t>(base)));
  u.d.click(u.at(unit, 0.85f, 0.5f));  // Harmonics
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "unit") == doctest::Approx(2.0));
  CHECK_FALSE(u.face().shown(static_cast<size_t>(p)));
  CHECK(u.face().shown(static_cast<size_t>(base)));
  // Base also shows for the Harmonic Walk style in another unit (a condition of two sources: any of them).
  u.d.click(u.at(unit, 0.15f, 0.5f));  // Degrees
  u.d.step();
  CHECK_FALSE(u.face().shown(static_cast<size_t>(base)));
  setValue(u.ctx, u.pr.arp, "style", 18.0);  // Harmonic Walk
  u.d.step();
  CHECK(u.face().shown(static_cast<size_t>(base)));
  CHECK_FALSE(u.face().shown(static_cast<size_t>(p)));
  setValue(u.ctx, u.pr.arp, "style", 0.0);
  u.d.step();
  CHECK_FALSE(u.face().shown(static_cast<size_t>(base)));
  // The tuplet shortcuts set n and m together (one undo step): Triplet = 3 : 2.
  const int32_t pre = u.index("presets", "tupletN");
  const size_t h0 = u.history();
  u.d.click(u.at(pre, 0.85f, 0.5f));
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "tupletN") == doctest::Approx(3.0));
  CHECK(valueOf(u.ctx, u.pr.arp, "tupletM") == doctest::Approx(2.0));
  CHECK(u.history() == h0 + 1);
}

TEST_CASE("arp panel (AP.8b, review L11; AR.3): a layer's level shows only within Layers — in both modes; the pulse "
          "view only while Pulse is on") {
  // AR.3 (rewritten: Layers moved from the Pulse fold into the Pitch block — they apply in both modes — and the
  // Proportion row is gone with the old pulse model).
  Ui u;
  auto shown = [&](const char* kind, const char* name) { return u.face().shown(static_cast<size_t>(u.index(kind, name))); };
  CHECK(shown("number", "layers"));  // Pulse off, its fold closed: Layers are in the Pitch block
  CHECK_FALSE(shown("number", "layerLevel1"));  // Layers 0: no levels
  setValue(u.ctx, u.pr.arp, "layers", 2.0);
  u.d.step();
  CHECK(shown("number", "layerLevel1"));
  CHECK(shown("number", "layerLevel2"));
  CHECK_FALSE(shown("number", "layerLevel3"));
  setValue(u.ctx, u.pr.arp, "layers", 3.0);
  u.d.step();
  CHECK(shown("number", "layerLevel3"));
  u.clickFold("Pulse");
  REQUIRE(u.foldOpen("Pulse"));
  auto pulseView = [&] {
    for (size_t i = 0; i < u.face().elementCount(); ++i)
      if (u.face().element(i).desc().kind == "arppulse") return u.face().shown(i);
    FAIL("no arppulse");
    return false;
  };
  CHECK_FALSE(pulseView());  // Pulse off: only its switch
  CHECK_FALSE(shown("toggle", "pulseAuto"));
  setValue(u.ctx, u.pr.arp, "pulse", 1.0);
  u.d.step();
  CHECK(pulseView());
  CHECK(shown("toggle", "pulseAuto"));
  CHECK(shown("number", "pulseCycleNum"));
  CHECK(shown("number", "layerLevel1"));
  // L12: the two retriggers are told apart.
  CHECK(findElement(u.face(), "tabs", "retrigger") >= 0);
  CHECK(u.face().element(static_cast<size_t>(u.index("toggle", "velRetrig"))).desc().text("text", "") == "On retrigger");
}

TEST_CASE("arp panel: a fold opens and closes (view state, no undo step), the frame follows its height") {
  Ui u;
  const size_t h0 = u.history();
  REQUIRE(u.foldOpen("Glide"));  // AP.10: the Glide section (and its bend view) is open by default
  const float open = u.host().face().worldSize().y;
  const int32_t knob = u.index("knob", "intoTime");
  CHECK(u.face().shown(static_cast<size_t>(knob)));
  u.clickFold("Glide");
  CHECK_FALSE(u.foldOpen("Glide"));
  const float closed = u.host().face().worldSize().y;
  CHECK(open > closed + 50.0f);
  CHECK_FALSE(u.face().shown(static_cast<size_t>(knob)));
  u.clickFold("Glide");
  CHECK(u.foldOpen("Glide"));
  CHECK(u.host().face().worldSize().y == doctest::Approx(open));
  CHECK(u.view().panelOf(u.pr.arp)->logicalHeight() >= open);
  CHECK(u.face().shown(static_cast<size_t>(knob)));
  CHECK(u.history() == h0);
}

TEST_CASE("arp panel (AP.7): the pattern view's ROLL chip flips the roll preview (one view setting, no undo step)") {
  Ui u;
  int32_t pv = -1;
  for (size_t i = 0; i < u.face().elementCount(); ++i)
    if (u.face().element(i).desc().kind == "arppattern") pv = static_cast<int32_t>(i);
  REQUIRE(pv >= 0);
  const Rect r = u.face().element(static_cast<size_t>(pv)).rect();
  const float fx = 1.0f - 19.0f / (r.x1 - r.x0), fy = 8.0f / (r.y1 - r.y0);  // the chip's centre (arp_elements.cpp)
  const size_t h0 = u.history();
  REQUIRE(u.ctx.notePreview());
  u.d.click(u.at(pv, fx, fy));
  u.d.step();
  CHECK_FALSE(u.ctx.notePreview());
  u.d.click(u.at(pv, fx, fy));
  u.d.step();
  CHECK(u.ctx.notePreview());
  CHECK(u.history() == h0);
  // A press on the rest of the display is not the chip's.
  u.d.click(u.at(pv, 0.3f, 0.6f));
  u.d.step();
  CHECK(u.ctx.notePreview());
}

// ---- AP.9: the panel in the project patch view ---------------------------------------------------------------------

namespace {

// The shell's project patch view over the project, zoom 1 on the arpeggiator's block (drawn as its panel).
struct PatchUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 1000};
  std::unique_ptr<Shell> shell;
  Project pr;
  explicit PatchUi(bool lfo = false) {
    pr = makeProject(ctx);
    if (lfo) {  // an LFO off the path, cabled into the arp's Gate
      const doc::DeviceId l = test_blocks::addModule(ctx.store(), pr.keys, "lfo", doc::ops::kOffPath);
      run(ctx.store(), doc::ops::connectWire(*ctx.doc(), ctx.store().ids(), l, "y", pr.arp, "gate"));
    }
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    const project_patch::Node& n = node();
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = n.x - 60.0;
    v.y = n.y - 60.0;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
    d.tree().setFocus(&view().canvas());
  }
  ~PatchUi() { d.tree().setRoot(std::make_unique<Widget>()); }
  project_patch::ProjectPatchView& view() const {
    project_patch::ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const project_patch::Node& node() const {
    const uint32_t n = view().patch().nodeOf(pr.arp);
    REQUIRE(n != project_patch::kNone);
    return view().patch().nodes[n];
  }
  panel::GroupPanelFace& face() const {
    const uint32_t mn = view().patch().nodeOf(pr.arp);
    REQUIRE(mn != project_patch::kNone);
    Face* f = view().canvas().panelOn(view().scene().sceneNode[mn]);
    REQUIRE(f != nullptr);
    auto* g = dynamic_cast<panel::GroupPanelFace*>(f);
    REQUIRE(g != nullptr);
    (void)g->sync();
    return *g;
  }
  int32_t index(std::string_view kind, std::string_view what) const {
    const int32_t i = findElement(face(), kind, what);
    REQUIRE_MESSAGE(i >= 0, "no " << kind << " " << what);
    return i;
  }
  // A point of element `i` as the block shows it (fractions of its rect), device px.
  Vec2 at(int32_t i, float fx, float fy) const {
    const Rect r = face().element(static_cast<size_t>(i)).rect();
    const project_patch::Node& n = node();
    const float k = n.scale;
    return view().canvas().toDevice({n.x + (r.x0 + (r.x1 - r.x0) * fx) * k, n.y + (r.y0 + (r.y1 - r.y0) * fy) * k});
  }
  size_t history() const { return ctx.store().history().cursor(); }
};

}  // namespace

TEST_CASE("render: project-patch-arp (AP.9: the arp drawn as its panel between the tuning block and the instrument)") {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize("project-patch-arp", w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene("project-patch-arp", {}, w, h, {}, o, &devLib());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  checkImage("project-patch-arp", *img);
}

TEST_CASE("arp panel (AP.9, project patch): the block is the type's panel; the band's notes cables run into and out of it") {
  PatchUi u;
  const project_patch::Node& n = u.node();
  CHECK(n.typePanel);
  CHECK(n.groupPanel);
  CHECK(u.face().owner() == u.pr.arp);
  // The Device view's panel (the same elements), at its shown size (every fold closed: the panel's fit height).
  const doc::Panel p = doc::noteDevicePanel(*doc::findDevice(*u.ctx.doc(), u.pr.arp).device);
  CHECK(u.face().elementCount() == p.elements.size());
  CHECK(n.w == doctest::Approx(std::ceil(p.width / 4.0f) * 4.0f));
  CHECK(n.h == doctest::Approx(std::ceil(u.face().worldSize().y / 4.0f) * 4.0f));
  // The band's notes cable: from the tuning block into its main input, and from its output on to the instrument. Its main
  // ports orbit its outline toward those cables (TX.orbit-ports, as any closed block's): the input on its left edge, the
  // output on its right edge.
  const project_patch::ProjectPatch& pp = u.view().patch();
  const uint32_t b = pp.bandOf(u.pr.keys), me = pp.nodeOf(u.pr.arp);
  REQUIRE(b != project_patch::kNone);
  REQUIRE(pp.bands[b].tuning != project_patch::kNone);
  REQUIRE(n.mainIn >= 0);
  bool in = false, out = false;
  for (const project_patch::Edge& e : pp.edges) {
    if (e.kind != project_patch::EdgeKind::Notes) continue;
    in = in || (e.to == me && e.toPort == static_cast<uint32_t>(n.mainIn) && e.from == pp.bands[b].tuning);
    out = out || (e.from == me && e.fromPort == 0 && e.to != project_patch::kNone && pp.nodes[e.to].instrument);
  }
  CHECK(in);
  CHECK(out);
  CHECK(n.orbitIn.on);
  CHECK(n.orbitOut.on);
  const graph::Point2 pi = project_patch::inPortPoint(n, static_cast<size_t>(n.mainIn)), po = project_patch::outPortPoint(n, 0);
  CHECK(std::abs(pi.x - n.x) <= 8.0f);  // (just outside the plate's edge)
  CHECK(std::abs(po.x - (n.x + n.w)) <= 8.0f);
  CHECK(pi.y >= n.y);
  CHECK(pi.y <= n.y + n.h);
  CHECK(po.y >= n.y);
  CHECK(po.y <= n.y + n.h);
}

TEST_CASE("arp panel (AP.9, project patch, real dispatch): a unit chip click sets the param in one undo step") {
  PatchUi u;
  const int32_t unit = u.index("tabs", "unit"), ratioP = u.index("number", "ratioP");
  CHECK_FALSE(u.face().shown(static_cast<size_t>(ratioP)));
  const size_t h0 = u.history();
  u.d.click(u.at(unit, 0.5f, 0.5f));  // Ratio (the middle chip)
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "unit") == doctest::Approx(1.0));
  CHECK(u.history() == h0 + 1);
  CHECK(u.face().shown(static_cast<size_t>(ratioP)));  // the panel's conditions follow, as in the Device view
  REQUIRE(u.ctx.undo());
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "unit") == doctest::Approx(0.0));
  CHECK_FALSE(u.face().shown(static_cast<size_t>(ratioP)));
}

TEST_CASE("arp panel (AP.9, project patch, real dispatch): a fold click grows the block in place (view state, no undo step)") {
  PatchUi u;
  const project_patch::Node n0 = u.node();  // (AP.10: Glide open by default)
  const size_t h0 = u.history();
  const int32_t fold = u.index("fold", "Glide");
  const uint32_t foldId = u.face().element(static_cast<size_t>(fold)).desc().id;
  u.d.click(u.at(fold, 0.1f, 0.5f));  // closed
  u.d.step();
  u.d.tree().layoutNow();
  CHECK(u.ctx.store().view().panelWindows[u.pr.arp.value].foldOpen(foldId) == std::optional<bool>(false));
  const project_patch::Node n1 = u.node();
  CHECK(n1.h < n0.h - 50.0f);
  u.d.click(u.at(u.index("fold", "Glide"), 0.1f, 0.5f));  // open again: it grows in place
  u.d.step();
  u.d.tree().layoutNow();
  CHECK(u.ctx.store().view().panelWindows[u.pr.arp.value].foldOpen(foldId) == std::optional<bool>(true));
  CHECK(u.node().h > n1.h + 50.0f);
  CHECK(u.node().h == doctest::Approx(n0.h));
  CHECK(u.node().x == doctest::Approx(n1.x));
  CHECK(u.node().y == doctest::Approx(n1.y));
  CHECK(u.node().w == doctest::Approx(n1.w));
  const int32_t bend = u.index("knob", "intoTime");
  CHECK(u.face().shown(static_cast<size_t>(bend)));
  CHECK(u.history() == h0);
  // A control in the opened section takes a drag where it is shown (one step).
  const double b0 = valueOf(u.ctx, u.pr.arp, "intoTime");
  u.d.drag(u.at(bend, 0.5f, 0.5f), u.at(bend, 0.5f, -1.0f), 6);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "intoTime") != doctest::Approx(b0));
  CHECK(u.history() == h0 + 1);
}

TEST_CASE("arp panel (AP.9, project patch): a cable into an arp param ends on its control (no pin)") {
  PatchUi u(true);
  const project_patch::Node& n = u.node();
  const int32_t k = n.input("gate");
  REQUIRE(k >= 0);
  const project_patch::Port& q = n.inputs[static_cast<size_t>(k)];
  CHECK(q.anchored);
  const Rect r = u.face().element(static_cast<size_t>(u.index("knob", "gate"))).rect();
  CHECK(q.ax >= r.x0 * n.scale);
  CHECK(q.ax <= r.x1 * n.scale);
  CHECK(q.ay >= r.y0 * n.scale);
  CHECK(q.ay <= r.y1 * n.scale);
}

// AR.2 (replaces the AP.10 handle test: the modes, the unit / cents switch and the Alt bend drag are gone — the note is
// what is dragged). Everything through the window (real dispatch).
TEST_CASE("arp panel (AR.2): the bend view is the editor — the note's head / tail, a knee, a curve, the ghosts; the "
          "knobs follow; one undo step each") {
  Ui u;
  REQUIRE(u.foldOpen("Glide"));
  int32_t bv = -1;
  for (size_t i = 0; i < u.face().elementCount(); ++i)
    if (u.face().element(i).desc().kind == "arpbend") bv = static_cast<int32_t>(i);
  REQUIRE(bv >= 0);
  auto el = [&]() -> panel::Element& { return u.face().element(static_cast<size_t>(bv)); };
  auto part = [&](int32_t h) { return panel::test::bendHandle(el(), u.face().target(), h); };
  enum : int32_t { kIntoKnee, kHead, kOutKnee, kTail, kIntoCurve, kOutCurve, kPrev, kNext };
  auto val = [&](const char* name) { return valueOf(u.ctx, u.pr.arp, name); };
  // Both sides off (the default): the head and the tail are there to grab, no knee, no curve handle.
  CHECK(val("intoOffset") == 0.0);
  REQUIRE(part(kHead).has_value());
  REQUIRE(part(kTail).has_value());
  CHECK_FALSE(part(kIntoKnee).has_value());
  CHECK_FALSE(part(kIntoCurve).has_value());
  // The head dragged straight down: an Into from below, snapped to whole 12-EDO degrees (the Degrees unit), the time
  // untouched (a straight pull) — one undo step, param writes only.
  size_t h0 = u.history();
  const double t0 = val("intoTime");
  Vec2 head = *part(kHead);
  u.d.drag(u.atLocal(bv, head), u.atLocal(bv, {head.x + 2.0f, head.y + 30.0f}), 6);
  u.d.step();
  const double o1 = val("intoOffset");
  CHECK(o1 <= -100.0);
  CHECK(std::fabs(o1 / 100.0 - std::round(o1 / 100.0)) < 1e-6);
  CHECK(val("intoPrev") == 0.0);
  CHECK(val("intoTime") == doctest::Approx(t0));
  CHECK(val("outOffset") == 0.0);
  REQUIRE(u.history() == h0 + 1);
  CHECK_FALSE(u.ctx.store().history().entries()[h0].changes.structural());
  {  // the Amount knob beside the view follows
    const panel::Element& kn = u.face().element(static_cast<size_t>(u.index("knob", "intoOffset")));
    REQUIRE(kn.binding() != nullptr);
    const std::string txt = u.face().target().format(*kn.binding(), u.face().target().position(*kn.binding()));
    CAPTURE(txt);
    CHECK(std::stod(txt) == doctest::Approx(o1).epsilon(0.01));
  }
  // The knee (where the Into arrives) dragged right: a longer Into, nothing else; the Time knob reads it in %.
  REQUIRE(part(kIntoKnee).has_value());
  const Vec2 knee = *part(kIntoKnee);
  h0 = u.history();
  u.d.drag(u.atLocal(bv, knee), u.atLocal(bv, {knee.x + 40.0f, knee.y}), 6);
  u.d.step();
  const double t1 = val("intoTime");
  CHECK(t1 > t0 + 0.05);
  CHECK(t1 <= 2.0);
  CHECK(val("intoOffset") == doctest::Approx(o1));
  {
    const panel::Element& kn = u.face().element(static_cast<size_t>(u.index("knob", "intoTime")));
    REQUIRE(kn.binding() != nullptr);
    const std::string txt = u.face().target().format(*kn.binding(), u.face().target().position(*kn.binding()));
    CAPTURE(txt);
    CHECK(txt.find('%') != std::string::npos);
    CHECK(std::stod(txt) == doctest::Approx(t1 * 100.0).epsilon(0.01));  // the knob's taper = the value's
  }
  CHECK(u.history() == h0 + 1);
  // The head dragged around (down and right): amount AND time in one drag, one undo step.
  head = *part(kHead);
  h0 = u.history();
  u.d.drag(u.atLocal(bv, head), u.atLocal(bv, {head.x + 30.0f, head.y + 16.0f}), 6);
  u.d.step();
  const double o2 = val("intoOffset"), t2 = val("intoTime");
  CHECK(o2 < o1);
  CHECK(t2 > t1 + 0.02);
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.ctx.undo());
  CHECK(val("intoOffset") == doctest::Approx(o1));
  CHECK(val("intoTime") == doctest::Approx(t1));
  // Shift: free cents (off the unit's grid).
  head = *part(kHead);
  u.d.drag(u.atLocal(bv, head), u.atLocal(bv, {head.x, head.y + 11.0f}), 6, ui::mod::kShift);
  u.d.step();
  const double o3 = val("intoOffset");
  CHECK(o3 < o1);
  CHECK(std::fabs(o3 / 100.0 - std::round(o3 / 100.0)) > 1e-3);
  // The middle of the Into curve dragged up: its Curve (from below: more bend), the time and amount untouched.
  REQUIRE(part(kIntoCurve).has_value());
  const Vec2 mid = *part(kIntoCurve);
  h0 = u.history();
  u.d.drag(u.atLocal(bv, mid), u.atLocal(bv, {mid.x, mid.y - 14.0f}), 4);
  u.d.step();
  CHECK(val("intoCurve") > 0.2);
  CHECK(val("intoTime") == doctest::Approx(t1));
  CHECK(val("intoOffset") == doctest::Approx(o3));
  CHECK(u.history() == h0 + 1);
  // The head dragged onto the previous step's ghost: Into from that step (its time stays).
  head = *part(kHead);
  const Vec2 prev = *part(kPrev);
  h0 = u.history();
  u.d.drag(u.atLocal(bv, head), u.atLocal(bv, prev), 8);
  u.d.step();
  CHECK(val("intoPrev") == 1.0);
  CHECK(val("intoTime") == doctest::Approx(t1));
  CHECK(u.history() == h0 + 1);
  // A double-click on the head: that side off.
  h0 = u.history();
  u.d.doubleClick(u.atLocal(bv, *part(kHead)));
  u.d.step();
  CHECK(val("intoPrev") == 0.0);
  CHECK(val("intoOffset") == 0.0);
  CHECK_FALSE(part(kIntoKnee).has_value());
  CHECK(u.history() == h0 + 1);
  // The tail dragged up: an Out to above; then a click on the next step's ghost: Out to that step, and again: off.
  Vec2 tail = *part(kTail);
  h0 = u.history();
  u.d.drag(u.atLocal(bv, tail), u.atLocal(bv, {tail.x, tail.y - 24.0f}), 6);
  u.d.step();
  CHECK(val("outOffset") >= 100.0);
  CHECK(val("intoOffset") == 0.0);
  CHECK(u.history() == h0 + 1);
  h0 = u.history();
  u.d.click(u.atLocal(bv, *part(kNext)));
  u.d.step();
  CHECK(val("outNext") == 1.0);
  CHECK(u.history() == h0 + 1);
  u.d.click(u.atLocal(bv, *part(kNext)));
  u.d.step();
  CHECK(val("outNext") == 0.0);
  // A press on the note's body (its left half, off any handle) takes the head.
  head = *part(kHead);
  tail = *part(kTail);
  const Vec2 body{head.x + (tail.x - head.x) * 0.3f, head.y};
  u.d.drag(u.atLocal(bv, body), u.atLocal(bv, {body.x, body.y - 24.0f}), 6);
  u.d.step();
  CHECK(val("intoOffset") >= 100.0);
}

TEST_CASE("arp panel (AR.2): a note of the pattern view takes the same drags (head: Into, tail: Out; every note conforms)") {
  Ui u;
  setValue(u.ctx, u.pr.arp, "rateDen", 4.0);  // (one long nominal note: room to grab it)
  u.d.step();
  int32_t pv = -1;
  for (size_t i = 0; i < u.face().elementCount(); ++i)
    if (u.face().element(i).desc().kind == "arppattern") pv = static_cast<int32_t>(i);
  REQUIRE(pv >= 0);
  auto end = [&](bool tail) {
    return panel::test::patternNoteEnd(u.face().element(static_cast<size_t>(pv)), u.face().target(), 0, tail);
  };
  auto val = [&](const char* name) { return valueOf(u.ctx, u.pr.arp, name); };
  REQUIRE(end(false).has_value());
  REQUIRE(end(true).has_value());
  // The head (just inside the note) dragged down: the Into amount, snapped; one undo step.
  size_t h0 = u.history();
  Vec2 a = *end(false);
  a.x += 2.0f;
  u.d.drag(u.atLocal(pv, a), u.atLocal(pv, {a.x, a.y + 20.0f}), 6);
  u.d.step();
  const double o = val("intoOffset");
  CHECK(o <= -100.0);
  CHECK(std::fabs(o / 100.0 - std::round(o / 100.0)) < 1e-6);
  CHECK(val("outOffset") == 0.0);
  CHECK(u.history() == h0 + 1);
  // The tail dragged up and left: the Out amount and a longer Out.
  const double t0 = val("outTime");
  Vec2 b = *end(true);
  b.x -= 2.0f;
  h0 = u.history();
  u.d.drag(u.atLocal(pv, b), u.atLocal(pv, {b.x - 30.0f, b.y - 20.0f}), 6);
  u.d.step();
  CHECK(val("outOffset") >= 100.0);
  CHECK(val("outTime") > t0 + 0.02);
  CHECK(val("intoOffset") == doctest::Approx(o));
  CHECK(u.history() == h0 + 1);
  // A double-click on the head: Into off.
  a = *end(false);
  a.x += 2.0f;
  u.d.doubleClick(u.atLocal(pv, a));
  u.d.step();
  CHECK(val("intoOffset") == 0.0);
  CHECK(val("outOffset") >= 100.0);
}

// ---- AR.3: the pulse view ------------------------------------------------------------------------------------------

TEST_CASE("arp panel (AR.3, real dispatch): the pulse view is the editor — a dot click mutes, a row drag sets its "
          "division, Shift+drag its shift, the wheel, the row menu; one undo step each") {
  Ui u;
  setValue(u.ctx, u.pr.arp, "pulse", 1.0);
  u.clickFold("Glide");  // (closed: the whole Pulse section is inside the shown part of the Device view)
  u.clickFold("Pulse");
  REQUIRE(u.foldOpen("Pulse"));
  int32_t pv = -1;
  for (size_t i = 0; i < u.face().elementCount(); ++i)
    if (u.face().element(i).desc().kind == "arppulse") pv = static_cast<int32_t>(i);
  REQUIRE(pv >= 0);
  REQUIRE(u.face().shown(static_cast<size_t>(pv)));
  (void)u.at(pv, 0.5f, 0.98f);  // (its bottom row is on the shown panel)
  auto el = [&]() -> panel::Element& { return u.face().element(static_cast<size_t>(pv)); };
  auto row = [&](uint32_t r) { return panel::test::pulseRow(el(), u.face().target(), r); };
  auto dot = [&](uint32_t r, int32_t j) {
    const std::optional<Vec2> p = panel::test::pulseDot(el(), u.face().target(), r, j);
    REQUIRE(p.has_value());
    return *p;
  };
  auto val = [&](const char* name) { return valueOf(u.ctx, u.pr.arp, name); };
  auto structural = [&](size_t h) { return u.ctx.store().history().entries()[h].changes.structural(); };
  // Nothing held: four nominal rows at the rows' divisions 3, 4, 5, 6, every pulse on.
  REQUIRE(panel::test::pulseRows(el(), u.face().target()) == 4);
  for (uint32_t r = 0; r < 4; ++r) {
    CHECK(row(r).division == static_cast<int32_t>(r) + 3);
    CHECK(row(r).mask == 0xFFFFFFFFu);
    CHECK_FALSE(row(r).locked);
  }
  // A click on pulse 1 of row 1 (÷4) mutes it: the mask's bit 1 — a setting (structural), one undo step.
  size_t h0 = u.history();
  u.d.click(u.atLocal(pv, dot(1, 1)));
  u.d.step();
  CHECK(val("pulseMaskLo2") == static_cast<double>(0xFFFD));
  CHECK(val("pulseMaskHi2") == static_cast<double>(0xFFFF));
  CHECK(row(1).mask == 0xFFFFFFFDu);
  CHECK(row(0).mask == 0xFFFFFFFFu);
  REQUIRE(u.history() == h0 + 1);
  CHECK(structural(h0));
  u.d.click(u.atLocal(pv, dot(1, 1)));  // again: it plays
  u.d.step();
  CHECK(row(1).mask == 0xFFFFFFFFu);
  CHECK(u.history() == h0 + 2);
  // Row 0 dragged right by three division steps (12 px each) from between its dots: ÷3 → ÷6; a param write only.
  h0 = u.history();
  const Vec2 a0 = dot(0, 0), a1 = dot(0, 1);
  const Vec2 from{a0.x + (a1.x - a0.x) * 0.4f, a0.y};
  u.d.drag(u.atLocal(pv, from), u.atLocal(pv, {from.x + 38.0f, from.y + 1.0f}), 6);
  u.d.step();
  CHECK(val("pulseDiv1") == 6.0);
  CHECK(row(0).division == 6);
  CHECK(row(0).mask == 0xFFFFFFFFu);  // (no dot was toggled by the drag)
  REQUIRE(u.history() == h0 + 1);
  CHECK_FALSE(structural(h0));
  u.d.drag(u.atLocal(pv, from), u.atLocal(pv, {from.x - 500.0f, from.y}), 6);  // far left: clamped at ÷1
  u.d.step();
  CHECK(val("pulseDiv1") == 1.0);
  CHECK(u.history() == h0 + 2);
  // Shift+drag row 1 by a quarter of its period: its dots follow the pointer, shift +25 % (snapped to 1/24).
  h0 = u.history();
  const Vec2 b0 = dot(1, 0), b1 = dot(1, 1);
  const float period = b1.x - b0.x;
  const Vec2 sf{b0.x + period * 0.5f, b0.y};
  u.d.drag(u.atLocal(pv, sf), u.atLocal(pv, {sf.x + period * 0.25f, sf.y}), 6, mod::kShift);
  u.d.step();
  CHECK(val("pulseShift2") == doctest::Approx(0.25).epsilon(1e-4));
  CHECK(val("pulseDiv2") == 4.0);
  CHECK(dot(1, 0).x == doctest::Approx(b0.x + period * 0.25f).epsilon(0.01));
  REQUIRE(u.history() == h0 + 1);
  CHECK_FALSE(structural(h0));
  u.d.drag(u.atLocal(pv, sf), u.atLocal(pv, {sf.x - period * 2.0f, sf.y}), 6, mod::kShift);  // clamped at −50 %
  u.d.step();
  CHECK(val("pulseShift2") == doctest::Approx(-0.5));
  // The wheel over row 2 (÷5): a notch up is ÷6, two down ÷4; a run of notches is one undo step.
  h0 = u.history();
  const Vec2 w = u.atLocal(pv, {dot(2, 1).x + 10.0f, dot(2, 1).y});
  CHECK(u.d.wheel(w, 1.0f));
  CHECK(val("pulseDiv3") == 6.0);
  CHECK(u.d.wheel(w, -1.0f));
  CHECK(u.d.wheel(w, -1.0f));
  u.d.advance(1.0);  // the run ends
  CHECK(val("pulseDiv3") == 4.0);
  CHECK(u.history() == h0 + 1);
  // The row menu (right-click a row, or its ⋯ chip): Euclid 3 of 4 on row 3 (÷6 → set ÷4 first), Invert, Reset.
  setValue(u.ctx, u.pr.arp, "pulseDiv4", 4.0);
  setValue(u.ctx, u.pr.arp, "pulseShift4", 0.125);
  u.d.step();
  auto menu = [&]() -> Menu* {
    OverlayLayer* layer = OverlayLayer::of(u.host());
    REQUIRE(layer != nullptr);
    auto* m = dynamic_cast<Menu*>(layer->top());
    REQUIRE(m != nullptr);
    return m;
  };
  auto item = [](const Menu& m, std::string_view label) {
    for (size_t i = 0; i < m.items().size(); ++i)
      if (m.items()[i].label == label) return static_cast<int32_t>(i);
    FAIL("no menu item " << label);
    return -1;
  };
  h0 = u.history();
  const Vec2 d3 = dot(3, 2);
  u.d.click(u.atLocal(pv, {d3.x + 12.0f, d3.y}), mod::kNone, PointerButton::Right);
  u.d.step();
  {
    Menu* m = menu();
    m->setHighlighted(item(*m, "Euclid"));
    Menu* s = m->openSubmenu();
    REQUIRE(s != nullptr);
    CHECK(s->items().size() == 4);  // 1 … 4 of 4
    CHECK(s->items()[3].checked);   // all on now: 4 of 4
    s->choose(item(*s, "3 of 4"));
    u.d.step();
  }
  CHECK(row(3).mask == 0xDDDDDDDDu);  // pulses 0, 2, 3 of every 4
  CHECK(val("pulseMaskLo4") == static_cast<double>(0xDDDD));
  CHECK(val("pulseMaskHi4") == static_cast<double>(0xDDDD));
  REQUIRE(u.history() == h0 + 1);  // both halves: one step
  CHECK(structural(h0));
  const std::optional<Vec2> chip = panel::test::pulseMenu(el(), u.face().target(), 3);
  REQUIRE(chip.has_value());
  u.d.click(u.atLocal(pv, *chip));  // the chip: a left click opens it
  u.d.step();
  {
    Menu* m = menu();
    m->choose(item(*m, "Invert"));
    u.d.step();
  }
  CHECK(row(3).mask == 0x22222222u);
  CHECK(u.history() == h0 + 2);
  u.d.click(u.atLocal(pv, *chip));
  u.d.step();
  {
    Menu* m = menu();
    m->choose(item(*m, "Rotate Later"));
    u.d.step();
  }
  CHECK(row(3).mask == 0x44444444u);  // pulse 1 → pulse 2
  u.d.click(u.atLocal(pv, *chip));
  u.d.step();
  {
    Menu* m = menu();
    m->choose(item(*m, "Reset Row (all on, no shift)"));
    u.d.step();
  }
  CHECK(row(3).mask == 0xFFFFFFFFu);
  CHECK(val("pulseShift4") == doctest::Approx(0.0));
  CHECK(u.history() == h0 + 4);  // the reset — mask and shift — is one step
}

TEST_CASE("arp panel (AR.3, real dispatch): a row Auto clocks by its note's harmonic is division-locked; its dots and "
          "its shift still edit") {
  Ui u;
  auto st = std::make_shared<NoteStageStatus>();
  const std::vector<NoteStageStatus::HeldNote> held = {jiNote(200.0f, 4.0f), jiNote(250.0f, 5.0f), degreeNote(300.0f, 7.0f)};
  st->writePattern(3, 3, false, [&](uint32_t i) { return held[i].hz; }, 3, [&](uint32_t i) { return held[i]; });
  u.ctx.setNoteStatusSource([st](doc::DeviceId) { return std::shared_ptr<const NoteStageStatus>(st); });
  setValue(u.ctx, u.pr.arp, "pulse", 1.0);
  u.clickFold("Glide");  // (closed: the whole Pulse section is inside the shown part of the Device view)
  u.clickFold("Pulse");
  REQUIRE(u.foldOpen("Pulse"));
  int32_t pv = -1;
  for (size_t i = 0; i < u.face().elementCount(); ++i)
    if (u.face().element(i).desc().kind == "arppulse") pv = static_cast<int32_t>(i);
  REQUIRE(pv >= 0);
  u.host().wake();  // (the status: what a host frame tick reads)
  u.d.advance(0.1);
  (void)u.at(pv, 0.5f, 0.98f);  // (its bottom row is on the shown panel)
  auto el = [&]() -> panel::Element& { return u.face().element(static_cast<size_t>(pv)); };
  auto row = [&](uint32_t r) { return panel::test::pulseRow(el(), u.face().target(), r); };
  auto dot = [&](uint32_t r, int32_t j) {
    const std::optional<Vec2> p = panel::test::pulseDot(el(), u.face().target(), r, j);
    REQUIRE(p.has_value());
    return *p;
  };
  REQUIRE(panel::test::pulseRows(el(), u.face().target()) == 4);  // three held + one more (at least four rows)
  CHECK(row(0).division == 4);  // h4
  CHECK(row(0).locked);
  CHECK(row(1).division == 5);  // h5
  CHECK(row(2).division == 5);  // no harmonic: its row's division (row 3: 5)
  CHECK_FALSE(row(2).locked);
  CHECK(row(2).held);
  CHECK_FALSE(row(3).held);
  // A plain drag on the locked row changes nothing (no undo step); the same drag on the unlocked row does.
  size_t h0 = u.history();
  Vec2 p0 = dot(0, 1);
  u.d.drag(u.atLocal(pv, {p0.x + 20.0f, p0.y}), u.atLocal(pv, {p0.x + 60.0f, p0.y}), 6);
  u.d.step();
  CHECK(row(0).division == 4);
  CHECK(valueOf(u.ctx, u.pr.arp, "pulseDiv1") == 3.0);
  CHECK(u.history() == h0);
  CHECK(u.d.wheel(u.atLocal(pv, {p0.x + 20.0f, p0.y}), 1.0f));  // (consumed; nothing changes)
  u.d.advance(1.0);
  CHECK(u.history() == h0);
  p0 = dot(2, 1);
  u.d.drag(u.atLocal(pv, {p0.x + 20.0f, p0.y}), u.atLocal(pv, {p0.x + 45.0f, p0.y}), 6);
  u.d.step();
  CHECK(row(2).division == 7);
  CHECK(u.history() == h0 + 1);
  // The locked row's dots and shift are its own.
  h0 = u.history();
  u.d.click(u.atLocal(pv, dot(0, 2)));
  u.d.step();
  CHECK(row(0).mask == 0xFFFFFFFBu);
  p0 = dot(0, 1);
  const float period = dot(0, 1).x - dot(0, 0).x;
  u.d.drag(u.atLocal(pv, {p0.x + 20.0f, p0.y}), u.atLocal(pv, {p0.x + 20.0f + period / 3.0f, p0.y}), 6, mod::kShift);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "pulseShift1") == doctest::Approx(1.0 / 3.0).epsilon(1e-4));
  CHECK(u.history() == h0 + 2);
}
