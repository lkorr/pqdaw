// GU.8 (docs/plans/GU.md "Phases" 8): the Wavetable Synth's panel (pq/document/modules.h wavetableSynthBlocks) — tabs
// over its modules' embedded panels, the Pitch knob always shown.
//   goldens   wavetable-synth-osc / -env / -lfo1 / -lfo2 (2x): the panel on each tab, each tab chosen by a click on its
//             cell (real dispatch: window → host → face → the tabs element)
//   gesture   in the shell's project patch view, the synth spawned closed at the track's notes: a click on "Env" hides
//             the Wavetable's embedded panel and shows the Envelope's (no undo step: an unbound tab is the panel's own
//             state); the Pitch knob stays shown
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
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

void run(doc::DocumentStore& st, const doc::ops::EditResult& e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(st.dispatch(e->command).ok());
}

// A MIDI track whose path is the synth (its default instrument dissolved, the synth spawned at the notes).
doc::DeviceId buildSynth(doc::DocumentStore& st) {
  auto tr = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Synth");
  run(st, tr);
  const doc::TrackId t = tr->tracks.at(0);
  std::vector<doc::DeviceId> heads;
  for (const doc::Device* d : doc::mainPath(*st.current(), *doc::findTrackPtr(*st.current(), t)).blocks) heads.push_back(d->id);
  if (!heads.empty()) run(st, doc::ops::dissolveDevices(*st.current(), st.ids(), nullptr, heads));
  const doc::ops::EditResult e = doc::ops::pasteBlocksAtNotes(*st.current(), st.ids(), t, doc::moduleBlocks(doc::ModuleKind::WavetableSynth));
  run(st, e);
  REQUIRE(doc::mainPath(*st.current(), *doc::findTrackPtr(*st.current(), t)).contains(e->devices.at(0)));
  return e->devices.at(0);
}

int32_t embedOf(const panel::GroupPanelFace& f, const doc::Project& p, std::string_view name) {
  for (size_t i = 0; i < f.elementCount(); ++i)
    if (const doc::DeviceId g = doc::embeddedGroup(f.element(i).desc()); g.valid())
      if (const doc::Device* d = doc::findDevice(p, g).device; d != nullptr && d->name == name) return static_cast<int32_t>(i);
  FAIL("no embedded " << name);
  return -1;
}

}  // namespace

TEST_CASE("render: wavetable-synth — the Wavetable Synth's panel on each of its tabs (2x), each chosen by a click") {
  constexpr float kScale = 2.0f;
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  const doc::DeviceId synth = buildSynth(ctx.store());
  const doc::PanelPtr p = doc::findDevice(*ctx.doc(), synth).device->panel;
  REQUIRE(p != nullptr);
  TestDriver d(static_cast<uint32_t>(p->width * kScale), static_cast<uint32_t>(p->height * kScale), kScale);
  panel::PanelHost* host = nullptr;
  auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, synth),
                                                      panel::makeDocPanelTarget(ctx, synth, [&host]() -> Widget* { return host; }));
  host = &d.setRoot<panel::PanelHost>("panel", std::move(face));
  d.tree().layoutNow();
  REQUIRE(d.enableGpu().ok());
  auto& f = static_cast<panel::GroupPanelFace&>(host->face());
  (void)f.sync();
  const int32_t tabs = f.indexOf(2);
  REQUIRE(tabs >= 0);
  const doc::PanelElement& t = f.element(static_cast<size_t>(tabs)).desc();
  const char* const names[] = {"wavetable-synth-osc", "wavetable-synth-env", "wavetable-synth-lfo1", "wavetable-synth-lfo2"};
  for (int k = 0; k < 4; ++k) {
    CAPTURE(k);
    d.click({(t.x + t.w * (static_cast<float>(k) + 0.5f) / 4.0f) * kScale, (t.y + t.h * 0.5f) * kScale});
    d.advance(0.1);
    CHECK(f.element(static_cast<size_t>(tabs)).selection(f.target()).value_or(-1.0) == doctest::Approx(k));
    auto img = d.render();
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    checkImage(names[k], *img);
  }
  d.tree().setRoot(std::make_unique<Widget>());
}

TEST_CASE("Wavetable Synth: in the project patch view (closed at the notes), a click on the Env tab swaps the embedded "
          "panels; no undo step; the Pitch knob stays") {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  TestDriver d{1600, 900};
  const doc::DeviceId synth = buildSynth(ctx.store());
  auto s = Shell::create(d.tree(), ctx);
  REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
  std::unique_ptr<Shell> shell = std::move(*s);
  d.tree().layoutNow();
  ProjectPatchView* v = findProjectPatch(*shell);
  REQUIRE(v != nullptr);
  v->setPushAnimated(false);
  v->setShown(true, false);
  d.tree().layoutNow();
  const uint32_t mn = v->patch().nodeOf(synth);
  REQUIRE(mn != kNone);
  {
    const Node& n = v->patch().nodes[mn];
    circuit::CircuitCanvas::View cv;
    cv.zoom = 1.0;
    cv.x = n.x - 40.0;
    cv.y = n.y - 40.0;
    v->canvas().setView(cv);
  }
  d.tree().layoutNow();
  d.step();
  auto face = [&]() -> panel::GroupPanelFace& {
    Face* f = v->canvas().panelOn(v->scene().sceneNode[v->patch().nodeOf(synth)]);
    REQUIRE(f != nullptr);  // spawned closed: drawn as its panel
    auto* g = dynamic_cast<panel::GroupPanelFace*>(f);
    REQUIRE(g != nullptr);
    (void)g->sync();
    return *g;
  };
  panel::GroupPanelFace& f = face();
  const int32_t tabs = f.indexOf(2), pitch = f.indexOf(3);
  const int32_t wt = embedOf(f, *ctx.doc(), "Wavetable"), env = embedOf(f, *ctx.doc(), "Envelope");
  REQUIRE(tabs >= 0);
  REQUIRE(pitch >= 0);
  CHECK(f.shown(static_cast<size_t>(wt)));
  CHECK_FALSE(f.shown(static_cast<size_t>(env)));
  const size_t h0 = ctx.store().history().cursor();
  const doc::PanelElement& t = f.element(static_cast<size_t>(tabs)).desc();
  const Node& n = v->patch().nodes[v->patch().nodeOf(synth)];
  d.click(v->canvas().toDevice({n.x + (t.x + t.w * 1.5f / 4.0f) * n.scale, n.y + (t.y + t.h * 0.5f) * n.scale}));
  d.step();
  CHECK(ctx.store().history().cursor() == h0);
  CHECK_FALSE(face().shown(static_cast<size_t>(wt)));
  CHECK(face().shown(static_cast<size_t>(env)));
  CHECK(face().shown(static_cast<size_t>(pitch)));
  d.tree().setRoot(std::make_unique<Widget>());
}
