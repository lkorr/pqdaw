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
    if (e.kind == doc::kFoldKind)
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
  void build(const std::vector<std::string>& open) {
    const doc::Device* dev = doc::findDevice(*ctx.doc(), pr.arp).device;
    REQUIRE(dev != nullptr);
    auto p = std::make_shared<const doc::Panel>(doc::noteDevicePanel(*dev));
    REQUIRE_FALSE(p->empty());
    CHECK(doc::checkPanel(*p).ok());
    setFolds(ctx, pr.arp, *p, open);
    if (status) ctx.setNoteStatusSource([s = status](doc::DeviceId) { return std::shared_ptr<const NoteStageStatus>(s); });
    const doc::FoldLayout fl = doc::foldLayout(*p, [&](const doc::PanelElement& e) {
      return ctx.store().view().panelWindows[pr.arp.value].foldOpen(e.id).value_or(doc::foldOpenByDefault(e));
    });
    d = std::make_unique<TestDriver>(static_cast<uint32_t>(p->width), static_cast<uint32_t>(std::ceil(fl.height)), 1.0f);
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
  setValue(s.ctx, s.pr.arp, "intoSource", 2.0);  // Offset
  setValue(s.ctx, s.pr.arp, "intoOffset", -2.0);
  setValue(s.ctx, s.pr.arp, "outTarget", 1.0);   // Next
  setValue(s.ctx, s.pr.arp, "intoShape", 3.0);  // Overshoot
  setValue(s.ctx, s.pr.arp, "intoBend", 0.6);
  setValue(s.ctx, s.pr.arp, "velSource", 1.0);   // Fixed
  s.build({"Glide"});
  checkImage("arp-panel-walk", s.render());
}

TEST_CASE("render: arp-panel-pulse (AP.6: Pulse on, Proportion, a Bars cycle)") {
  Standalone s;
  setValue(s.ctx, s.pr.arp, "pulse", 1.0);
  setValue(s.ctx, s.pr.arp, "pulseSource", 1.0);
  setValue(s.ctx, s.pr.arp, "pulseCycle", 1.0);
  setValue(s.ctx, s.pr.arp, "pulseBars", 2.0);
  setValue(s.ctx, s.pr.arp, "proportionLen", 4.0);
  setValue(s.ctx, s.pr.arp, "layers", 2.0);
  s.build({"Pulse"});
  checkImage("arp-panel-pulse", s.render());
}

TEST_CASE("render: arp-panel-lanes (AP.6: lanes on, the playing step lit)") {
  Standalone s;
  setValue(s.ctx, s.pr.arp, "velLane", 1.0);
  setValue(s.ctx, s.pr.arp, "transLane", 1.0);
  setValue(s.ctx, s.pr.arp, "tieLane", 1.0);
  setValue(s.ctx, s.pr.arp, "velLen", 8.0);
  setValue(s.ctx, s.pr.arp, "transLen", 12.0);
  setValue(s.ctx, s.pr.arp, "tieLen", 6.0);
  for (int i = 0; i < 8; ++i) setValue(s.ctx, s.pr.arp, "velStep" + std::to_string(i + 1), 0.25 + 0.1 * i);
  setValue(s.ctx, s.pr.arp, "transStep2", 7.0);
  setValue(s.ctx, s.pr.arp, "transStep5", -5.0);
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
  s.build({"Step Lanes"});
  const Image img = s.render();
  const int32_t vel = findElement(s.face(), "steplane", "0");
  REQUIRE(vel >= 0);
  CHECK(panel::test::stepLaneLit(s.face().element(static_cast<size_t>(vel))) == 5);
  for (size_t i = 0; i < s.face().elementCount(); ++i)
    if (s.face().element(i).desc().kind == "arppattern") {
      CHECK(panel::test::patternPosition(s.face().element(i)) == 2);
      CHECK(panel::test::patternCount(s.face().element(i)) == 6);
    }
  checkImage("arp-panel-lanes", img);
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
  setValue(s.ctx, s.pr.arp, "intoSource", 2.0);  // Offset
  setValue(s.ctx, s.pr.arp, "intoOffset", -2.0);
  setValue(s.ctx, s.pr.arp, "intoMs", 30.0);
  setValue(s.ctx, s.pr.arp, "intoShape", 1.0);  // Exponential
  setValue(s.ctx, s.pr.arp, "intoBend", 0.7);
  setValue(s.ctx, s.pr.arp, "outTarget", 1.0);  // Next
  setValue(s.ctx, s.pr.arp, "outSync", 1.0);
  setValue(s.ctx, s.pr.arp, "outFrac", 0.3);
  setValue(s.ctx, s.pr.arp, "outShape", 2.0);  // S-curve
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

TEST_CASE("render: arp-panel-bend-view (AP.10: asymmetric Into / Out — times, shapes, offsets in cents; nothing held)") {
  Standalone s;
  setValue(s.ctx, s.pr.arp, "gate", 1.0);
  setValue(s.ctx, s.pr.arp, "glideCents", 1.0);
  setValue(s.ctx, s.pr.arp, "intoSource", 2.0);
  setValue(s.ctx, s.pr.arp, "intoOffset", -300.0);
  setValue(s.ctx, s.pr.arp, "intoMs", 25.0);
  setValue(s.ctx, s.pr.arp, "intoShape", 1.0);  // Exponential
  setValue(s.ctx, s.pr.arp, "intoBend", 0.8);
  setValue(s.ctx, s.pr.arp, "outTarget", 2.0);
  setValue(s.ctx, s.pr.arp, "outOffset", 150.0);
  setValue(s.ctx, s.pr.arp, "outSync", 1.0);
  setValue(s.ctx, s.pr.arp, "outFrac", 0.5);
  setValue(s.ctx, s.pr.arp, "outShape", 3.0);  // Overshoot
  setValue(s.ctx, s.pr.arp, "outBend", 0.6);
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

TEST_CASE("arp panel: a lane bar dragged is one undo step and a param write (no structural change)") {
  Ui u;
  REQUIRE_FALSE(u.foldOpen("Step Lanes"));
  u.clickFold("Step Lanes");
  REQUIRE(u.foldOpen("Step Lanes"));
  const int32_t lane = u.index("steplane", "0");  // Velocity
  const size_t h0 = u.history();
  // Step 4 (index 3): press at its bottom, drag to its top.
  const float fx = (3.5f * 18.0f) / u.face().element(static_cast<size_t>(lane)).rect().width();
  u.d.drag(u.at(lane, fx, 0.95f), u.at(lane, fx, 0.0f), 6);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "velStep4") == doctest::Approx(1.0));
  // Then to the bottom: 0.
  u.d.drag(u.at(lane, fx, 0.2f), u.at(lane, fx, 1.0f), 6);
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

TEST_CASE("arp panel: a drag across steps paints them in one undo step; double-click resets one") {
  Ui u;
  u.clickFold("Step Lanes");
  const int32_t lane = u.index("steplane", "1");  // Gate (0 … 2, default 1)
  const float w = u.face().element(static_cast<size_t>(lane)).rect().width();
  const size_t h0 = u.history();
  // Press on step 1 at three quarters down its height (a quarter of the range), sweep to step 8 at the same height
  // (a fast drag: 2 moves, the steps between painted too).
  u.d.drag(u.at(lane, 0.5f * 18.0f / w, 0.75f), u.at(lane, 7.5f * 18.0f / w, 0.75f), 2);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  for (int i = 1; i <= 8; ++i) CHECK(valueOf(u.ctx, u.pr.arp, "gateStep" + std::to_string(i)) == doctest::Approx(0.5).epsilon(0.1));
  CHECK(valueOf(u.ctx, u.pr.arp, "gateStep9") == doctest::Approx(1.0));
  // A double-click on step 2: back to its default (one step).
  u.d.doubleClick(u.at(lane, 1.5f * 18.0f / w, 0.5f));
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "gateStep2") == doctest::Approx(1.0));
  CHECK(valueOf(u.ctx, u.pr.arp, "gateStep3") == doctest::Approx(0.5).epsilon(0.1));
}

TEST_CASE("arp panel: the length handle drags a lane's length (one undo step); Skip / Tie cells cycle") {
  Ui u;
  u.clickFold("Glide");  // (AP.10: open by default; folded, the last lane is in the window)
  u.clickFold("Step Lanes");
  const int32_t lane = u.index("steplane", "4");  // Chance
  const float w = u.face().element(static_cast<size_t>(lane)).rect().width();
  REQUIRE(valueOf(u.ctx, u.pr.arp, "chanceLen") == doctest::Approx(16.0));
  const size_t h0 = u.history();
  u.d.drag(u.at(lane, 16.0f * 18.0f / w, 0.5f), u.at(lane, 7.0f * 18.0f / w, 0.5f), 5);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "chanceLen") == doctest::Approx(7.0));
  CHECK(u.history() == h0 + 1);
  // Skip / Tie: a click on a cell cycles Play → Skip → Tie.
  const int32_t tie = u.index("steplane", "5");
  const Vec2 c = u.at(tie, 2.5f * 18.0f / w, 0.5f);
  u.d.click(c);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "tieStep3") == doctest::Approx(1.0));
  u.d.click(c);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "tieStep3") == doctest::Approx(2.0));
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

TEST_CASE("arp panel (AP.8b, review L11): a layer's level shows only within Layers, a proportion entry only within Length") {
  Ui u;
  setValue(u.ctx, u.pr.arp, "pulse", 1.0);
  setValue(u.ctx, u.pr.arp, "pulseSource", 1.0);  // Proportion
  u.clickFold("Pulse");
  REQUIRE(u.foldOpen("Pulse"));
  auto shown = [&](const char* kind, const char* name) { return u.face().shown(static_cast<size_t>(u.index(kind, name))); };
  CHECK_FALSE(shown("knob", "layerLevel1"));  // Layers 0: no levels
  setValue(u.ctx, u.pr.arp, "layers", 2.0);
  u.d.step();
  CHECK(shown("knob", "layerLevel1"));
  CHECK(shown("knob", "layerLevel2"));
  CHECK_FALSE(shown("knob", "layerLevel3"));
  setValue(u.ctx, u.pr.arp, "layers", 3.0);
  u.d.step();
  CHECK(shown("knob", "layerLevel3"));
  setValue(u.ctx, u.pr.arp, "proportionLen", 4.0);
  u.d.step();
  CHECK(shown("number", "proportion4"));
  CHECK_FALSE(shown("number", "proportion5"));
  CHECK_FALSE(shown("number", "proportion8"));
  setValue(u.ctx, u.pr.arp, "pulseSource", 0.0);  // Harmonic: the proportion row goes (its Length with it)
  u.d.step();
  CHECK_FALSE(shown("number", "proportion1"));
  setValue(u.ctx, u.pr.arp, "pulse", 0.0);  // Pulse off: the layer levels go too (a hidden source hides what follows it)
  u.d.step();
  CHECK_FALSE(shown("knob", "layerLevel1"));
  // L12: the two retriggers are told apart.
  CHECK(findElement(u.face(), "tabs", "retrigger") >= 0);
  CHECK(u.face().element(static_cast<size_t>(u.index("toggle", "velRetrig"))).desc().text("text", "") == "Vel Retrig");
}

TEST_CASE("arp panel: a fold opens and closes (view state, no undo step), the frame follows its height") {
  Ui u;
  const size_t h0 = u.history();
  REQUIRE(u.foldOpen("Glide"));  // AP.10: the Glide section (and its bend view) is open by default
  const float open = u.host().face().worldSize().y;
  const int32_t knob = u.index("knob", "intoBend");
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
  const int32_t bend = u.index("knob", "intoBend");
  CHECK(u.face().shown(static_cast<size_t>(bend)));
  CHECK(u.history() == h0);
  // A control in the opened section takes a drag where it is shown (one step).
  const double b0 = valueOf(u.ctx, u.pr.arp, "intoBend");
  u.d.drag(u.at(bend, 0.5f, 0.5f), u.at(bend, 0.5f, -1.0f), 6);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "intoBend") != doctest::Approx(b0));
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

TEST_CASE("arp panel (AP.10): the bend view's handles drag Into time / offset and (Alt) bend, one undo step each") {
  Ui u;
  setValue(u.ctx, u.pr.arp, "intoSource", 2.0);  // Offset
  setValue(u.ctx, u.pr.arp, "intoOffset", -2.0);
  setValue(u.ctx, u.pr.arp, "intoMs", 20.0);
  setValue(u.ctx, u.pr.arp, "outTarget", 1.0);  // Next
  u.d.step();
  REQUIRE(u.foldOpen("Glide"));
  int32_t bv = -1;
  for (size_t i = 0; i < u.face().elementCount(); ++i)
    if (u.face().element(i).desc().kind == "arpbend") bv = static_cast<int32_t>(i);
  REQUIRE(bv >= 0);
  panel::Element& el = u.face().element(static_cast<size_t>(bv));
  const std::optional<Vec2> time = panel::test::bendHandle(el, u.face().target(), 0);
  REQUIRE(time.has_value());
  CHECK_FALSE(panel::test::bendHandle(el, u.face().target(), 3).has_value());  // Out → Next: no offset handle
  // The Into time handle dragged 40 units right (through the window): a longer Into in ms, one undo step, a param
  // write only.
  size_t h0 = u.history();
  u.d.drag(u.atLocal(bv, *time), u.atLocal(bv, {time->x + 40.0f, time->y}), 6);
  u.d.step();
  const double ms = valueOf(u.ctx, u.pr.arp, "intoMs");
  CHECK(ms > 30.0);
  REQUIRE(u.history() == h0 + 1);
  CHECK_FALSE(u.ctx.store().history().entries()[h0].changes.structural());
  // The Into offset handle dragged up: a smaller downward offset (whole degrees).
  const std::optional<Vec2> off = panel::test::bendHandle(el, u.face().target(), 1);
  REQUIRE(off.has_value());
  h0 = u.history();
  u.d.drag(u.atLocal(bv, *off), u.atLocal(bv, {off->x, off->y - 14.0f}), 4);
  u.d.step();
  const double o = valueOf(u.ctx, u.pr.arp, "intoOffset");
  CHECK(o > -2.0);
  CHECK(o == std::round(o));
  CHECK(u.history() == h0 + 1);
  // Alt + drag up on the time handle: the Into bend, nothing else.
  const std::optional<Vec2> t2 = panel::test::bendHandle(el, u.face().target(), 0);
  REQUIRE(t2.has_value());
  h0 = u.history();
  const double bend0 = valueOf(u.ctx, u.pr.arp, "intoBend");
  u.d.drag(u.atLocal(bv, *t2), u.atLocal(bv, {t2->x, t2->y - 10.0f}), 4, ui::mod::kAlt);
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "intoBend") > bend0 + 0.1);
  CHECK(valueOf(u.ctx, u.pr.arp, "intoMs") == doctest::Approx(ms));
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.ctx.undo());
  CHECK(valueOf(u.ctx, u.pr.arp, "intoBend") == doctest::Approx(bend0));
}
