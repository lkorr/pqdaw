// AP.6 (docs/plans/AP.md "AP.6"; pq/document/note_devices.h noteDevicePanel; views panel/arp_elements.cpp): the
// Arpeggiator's device panel. Goldens of its main states drawn standalone at 1x (the Device view draws it 1:1): the
// default (every section folded, nothing held), the Ratio unit (with a free rate and Beat retrigger), Harmonic Walk (the
// walk row, Base, the Glide section open), Pulse on (its section open) and the step lanes on while a note stage plays
// (each lane's current step and the pattern's position lit from a NoteStageStatus). Real dispatch through the shell's
// Device view (window → chain → panel host → face → element): a lane bar dragged (one undo step, a param write only:
// not structural), a paint across steps, the length handle, the unit chips switching which controls show, a fold
// opened and closed (view state: no undo step, the frame follows the panel's height).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/core/note_stage_status.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/note_devices.h"
#include "pq/ui/image.h"
#include "pq/ui/controls.h"
#include "pq/ui/test_driver.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/arp_elements.h"
#include "pq/views/panel/block_frame.h"
#include "pq/views/panel/element.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/shell/shell.h"
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
  setValue(s.ctx, s.pr.arp, "glideShape", 3.0);  // Overshoot
  setValue(s.ctx, s.pr.arp, "glideBend", 0.6);
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
  s.status->writePattern(6, 4, false, [&](uint32_t i) { return hz[i]; });
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
  // The tuplet shortcuts set n and m together (one undo step): Triplet = 3 : 2.
  const int32_t pre = u.index("presets", "tupletN");
  const size_t h0 = u.history();
  u.d.click(u.at(pre, 0.85f, 0.5f));
  u.d.step();
  CHECK(valueOf(u.ctx, u.pr.arp, "tupletN") == doctest::Approx(3.0));
  CHECK(valueOf(u.ctx, u.pr.arp, "tupletM") == doctest::Approx(2.0));
  CHECK(u.history() == h0 + 1);
}

TEST_CASE("arp panel: a fold opens and closes (view state, no undo step), the frame follows its height") {
  Ui u;
  const size_t h0 = u.history();
  const float closed = u.host().face().worldSize().y;
  u.clickFold("Glide");
  CHECK(u.foldOpen("Glide"));
  const float open = u.host().face().worldSize().y;
  CHECK(open > closed + 50.0f);
  CHECK(u.view().panelOf(u.pr.arp)->logicalHeight() >= open);
  const int32_t knob = u.index("knob", "glideBend");
  CHECK(u.face().shown(static_cast<size_t>(knob)));
  u.clickFold("Glide");
  CHECK_FALSE(u.foldOpen("Glide"));
  CHECK(u.host().face().worldSize().y == doctest::Approx(closed));
  CHECK_FALSE(u.face().shown(static_cast<size_t>(knob)));
  CHECK(u.history() == h0);
}
