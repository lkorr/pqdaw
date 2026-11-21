// SM.8 (docs/plans/SM.md "Decisions", Drop; pq/views/project_patch/project_patch_view.h "audio files dropped on the
// circuit"): audio files dropped on a band's circuit in the project patch, through the window's drag events (an
// Explorer drag: UiTree::dragEnter / dragMove / dragDrop; the browser's: a press on its row, moves, the release / Esc):
//   empty MIDI path   the Sampler goes onto the path (notes → sampler → mixer), its sample the file's, its root the
//                     detected pitch (a 440 Hz tone), slices at 0 + the transients — one undo step
//   Basic Synth       on a MIDI band whose path has an instrument: placed unwired, the path as it was
//   audio track       placed unwired at the drop point
//   replace           dropped on a Sampler: its sample is the new file, no block added, one step
//   several files     one Sampler each, the second under the first, one step
//   Esc               a browser drag over the circuit shows the ghost; Esc ends it, nothing placed
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"
#include "pq/document/modules.h"
#include "pq/media/wav_file.h"
#include "pq/meta/sampler.h"
#include "pq/ui/drag.h"
#include "pq/ui/test_driver.h"
#include "pq/views/browser/browser_panel.h"
#include "pq/views/browser/demo_index.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/preview_port.h"
#include "pq/views/fake_transport.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/sample_analysis.h"
#include "pq/views/shell/shell.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::project_patch;

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

std::string u8(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// A mono 48 kHz WAV: two 440 Hz notes (0 … 0.6 s, 0.8 … 1.4 s), so it has a pitch and an onset after the start.
std::string toneWav(std::string_view test, const std::string& name, double hz = 440.0) {
  const std::filesystem::path dir(views::test::tempDir("sample_place_" + std::string(test) + "_" + name));
  const size_t n = 72000;
  std::vector<std::vector<float>> ch(1, std::vector<float>(n, 0.0f));
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / 48000.0;
    if (t < 0.6 || (t >= 0.8 && t < 1.4)) ch[0][i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * t));
  }
  const std::filesystem::path p = dir / name;
  REQUIRE(media::writeWav(p, ch, 48000).ok());
  return u8(p);
}

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  FakePreviewPort preview;
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  BrowserPanel* b = nullptr;
  doc::TrackId keys, empty, loop;
  Fixture() {
    ctx.setDeviceLibrary(&library());
    ctx.library().setPreview(&preview);
    ctx.library().setIndex(browserDemoIndex(views::test::fixtureDir("audio") + "/samples", 0));
    keys = add(doc::TrackKind::Midi, "Keys");
    empty = add(doc::TrackKind::Midi, "Empty");
    {  // Empty's instrument dissolved: its notes path holds no block
      std::vector<doc::DeviceId> heads;
      for (const doc::Device* x : path(empty)) heads.push_back(x->id);
      REQUIRE(!heads.empty());
      auto e = doc::ops::dissolveDevices(*ctx.doc(), ctx.store().ids(), nullptr, heads);
      REQUIRE(e.ok());
      REQUIRE(ctx.store().dispatch(e->command).ok());
      REQUIRE(path(empty).empty());
    }
    loop = add(doc::TrackKind::Audio, "Loop");
    ctx.setSelectedTrack(keys);
    ctx.setBrowserVisible(true);
    ctx.setDetailVisible(false);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    b = findBrowser(*shell);
    REQUIRE(b != nullptr);
    view().setShown(true, false);
    d.tree().layoutNow();
    d.step();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }
  doc::TrackId add(doc::TrackKind k, const char* name) {
    auto t = doc::ops::createTrack(*ctx.doc(), ctx.store().ids(), k, {}, name);
    REQUIRE(t.ok());
    REQUIRE(ctx.store().dispatch(t->command).ok());
    return t->tracks[0];
  }
  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  std::vector<const doc::Device*> path(doc::TrackId t) const {
    return doc::mainPath(*ctx.doc(), *doc::findTrackPtr(*ctx.doc(), t)).blocks;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  size_t devices(doc::TrackId t) const { return doc::findTrackPtr(*ctx.doc(), t)->devices.size(); }
  // Band `t` framed at zoom 1, and a device point on its circuit (not its lane, no block under it) with room around.
  Vec2 onCircuit(doc::TrackId t) {
    ProjectCanvas& c = view().canvas();
    const ProjectPatch& pp = view().patch();
    const uint32_t bi = pp.bandOf(t);
    REQUIRE(bi != kNone);
    const Band& band = pp.bands[bi];
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = static_cast<double>(band.x0) - 20.0;
    v.y = static_cast<double>(band.y) - 20.0;
    c.setView(v);
    d.tree().layoutNow();
    const IRect cr = c.rect();
    const float bottom = band.laneH > 0.0f ? band.laneY : band.y + band.h;
    for (float y = band.y + metrics::kTitle + 4.0f; y < bottom - 4.0f; y += 4.0f)
      for (float x = band.x0 + 60.0f; x < band.x0 + 1200.0f; x += 10.0f) {
        const Vec2 p = c.toDevice({x, y});
        if (!cr.contains(p) || c.bandAt(p) != bi || c.nodeAt(p) != kNone) continue;
        bool clear = true;  // nothing within ±30 px across, ±10 px up and down
        for (float dx = -30.0f; dx <= 30.0f && clear; dx += 10.0f)
          for (float dy = -10.0f; dy <= 10.0f && clear; dy += 10.0f) clear = c.nodeAt({p.x + dx, p.y + dy}) == kNone;
        if (clear) return p;
      }
    FAIL("no free point on the band's circuit");
    return {};
  }
  Vec2 onNode(doc::DeviceId device) {
    const ProjectPatch& pp = view().patch();
    const uint32_t n = pp.nodeOf(device);
    REQUIRE(n != kNone);
    const Node& nd = pp.nodes[n];
    const Vec2 p = view().canvas().toDevice({nd.x + nd.w * 0.5f, nd.y + nd.h * 0.5f});
    REQUIRE(view().canvas().nodeAt(p) == n);
    return p;
  }
  // Placed where it was dropped: its centre there, but never above its band's title strip (the spawn rule: a block
  // taller than the room above the point has its top there).
  void checkPlacedAt(doc::DeviceId device, Vec2 at) {
    const ProjectPatch& pp = view().patch();
    const Node& nd = pp.nodes[pp.nodeOf(device)];
    const graph::Point2 w = view().canvas().toWorld(at);
    CHECK(std::fabs(nd.x + nd.w * 0.5f - w.x) < 2.0f);
    const float top = pp.bands[nd.band].y + metrics::kTitle;
    CHECK((std::fabs(nd.y + nd.h * 0.5f - w.y) < 2.0f || (std::fabs(nd.y - top) < 1.0f && nd.y + nd.h * 0.5f > w.y)));
  }
  DropEffect dropFiles(std::vector<std::string> paths, Vec2 pos) {
    DragPayload p;
    p.kind = DragKind::Files;
    p.paths = std::move(paths);
    p.label = "sample";
    d.tree().dragEnter(std::move(p), {pos.x, pos.y - 3.0f}, 0);
    (void)d.tree().dragMove(pos, 0);
    CHECK(d.tree().dropTarget() == &view().canvas());
    CHECK(d.tree().dropFeedback().effect == DropEffect::Copy);
    CHECK(view().sampleDropHover().on);
    d.step();  // the ghost painted (the canvas's overlay) without a fault
    const DropEffect e = d.tree().dragDrop(pos, 0);
    CHECK_FALSE(view().sampleDropHover().on);
    d.step();
    return e;
  }
  // The Sampler engine of group `g` and its sample / root / slices.
  const doc::Device* engine(doc::DeviceId g) const {
    const doc::DeviceLoc l = doc::findDevice(*ctx.doc(), g);
    REQUIRE(l.track != nullptr);
    const doc::Device* e = nullptr;
    doc::forEachDeviceIn(l.track->devices, [&](const doc::Device& x) {
      if (x.group == g && x.patch == "meta:sampler") e = &x;
    });
    REQUIRE(e != nullptr);
    return e;
  }
  uint32_t sampleOf(doc::DeviceId g) const { return meta::samplerSample(engine(g)->metaAt("")->data.get()); }
  double rootOf(doc::DeviceId g) const {
    const meta::Type* t = meta::findType("sampler");
    REQUIRE(t != nullptr);
    return doc::metaParamValue(*engine(g), "", *t, meta::kSmpRoot);
  }
  // The new top-level blocks of `t` since `before` (its devices then).
  std::vector<doc::DeviceId> newBlocks(doc::TrackId t, const std::vector<doc::DeviceId>& before) const {
    std::vector<doc::DeviceId> out;
    for (const doc::DevicePtr& x : doc::findTrackPtr(*ctx.doc(), t)->devices)
      if (!x->group.valid() && std::find(before.begin(), before.end(), x->id) == before.end()) out.push_back(x->id);
    return out;
  }
  std::vector<doc::DeviceId> blocks(doc::TrackId t) const {
    std::vector<doc::DeviceId> out;
    for (const doc::DevicePtr& x : doc::findTrackPtr(*ctx.doc(), t)->devices) out.push_back(x->id);
    return out;
  }
};

}  // namespace

TEST_CASE("SM.8 sample drop: on a MIDI band with an empty notes path the Sampler goes onto it; root and slices from the "
          "analysis; one undo step") {
  Fixture f;
  const std::string a = toneWav("empty", "A440.wav");
  const Vec2 at = f.onCircuit(f.empty);
  const std::vector<doc::DeviceId> before = f.blocks(f.empty);
  const size_t h = f.history();
  CHECK(f.dropFiles({a}, at) == DropEffect::Copy);
  CHECK(f.history() == h + 1);
  const std::vector<doc::DeviceId> made = f.newBlocks(f.empty, before);
  REQUIRE(made.size() == 1);
  CHECK(f.view().samplerOf(made[0]) == made[0]);
  const auto p = f.path(f.empty);
  REQUIRE(p.size() == 1);
  CHECK(p[0]->id == made[0]);  // notes → sampler → mixer
  CHECK(p[0]->name == "A440");
  // Its sample: the file's (a new table entry), its root the detected 440 Hz, its slices 0 + the transients.
  const uint32_t sid = f.sampleOf(made[0]);
  REQUIRE(sid != 0);
  const doc::SampleRef* s = doc::findSample(*f.ctx.doc(), doc::SampleId{sid});
  REQUIRE(s != nullptr);
  CHECK(s->displayName == "A440");
  CHECK(std::fabs(1200.0 * std::log2(f.rootOf(made[0]) / 440.0)) < 5.0);
  const SampleAnalysisResult* an = f.ctx.sampleAnalysis().get(doc::SampleId{sid});
  REQUIRE(an != nullptr);
  REQUIRE(an->transients != nullptr);
  const std::vector<float> slices = meta::samplerSlices(f.engine(made[0])->metaAt("")->data.get());
  REQUIRE(!slices.empty());
  CHECK(slices.front() == 0.0f);
  bool late = false;  // the second note's onset (≈ 0.8 s) is a slice
  for (const float x : slices) late = late || std::fabs(x - 38400.0f) < 2400.0f;
  CHECK(late);
  f.checkPlacedAt(made[0], at);  // (as a spawn)
  // One undo step takes all of it (the analysis's root and slices included).
  REQUIRE(f.ctx.undo());
  CHECK(f.history() == h);
  CHECK(f.blocks(f.empty) == before);
  CHECK(f.path(f.empty).empty());
}

TEST_CASE("SM.8 sample drop: on a MIDI band whose path has Basic Synth the Sampler lands unwired") {
  Fixture f;
  const std::string a = toneWav("synth", "Pluck.wav");
  const auto path0 = f.path(f.keys);
  REQUIRE(path0.size() == 1);
  const doc::DeviceId synth = path0[0]->id;
  const std::vector<doc::DeviceId> before = f.blocks(f.keys);
  const size_t wires = doc::wiresOf(*f.ctx.doc()).size();
  const size_t h = f.history();
  CHECK(f.dropFiles({a}, f.onCircuit(f.keys)) == DropEffect::Copy);
  CHECK(f.history() == h + 1);
  const std::vector<doc::DeviceId> made = f.newBlocks(f.keys, before);
  REQUIRE(made.size() == 1);
  CHECK(f.view().samplerOf(made[0]) == made[0]);
  const auto p = f.path(f.keys);
  REQUIRE(p.size() == 1);
  CHECK(p[0]->id == synth);
  // Unwired: no wire touches the group from outside it (its own inner wires only).
  for (const doc::Wire& w : doc::wiresOf(*f.ctx.doc())) {
    const doc::Device* from = w.from.valid() ? doc::findDevice(*f.ctx.doc(), w.from).device : nullptr;
    const doc::Device* to = doc::findDevice(*f.ctx.doc(), w.to).device;
    const bool fromIn = from != nullptr && (from->id == made[0] || doc::groupContains(doc::findTrackPtr(*f.ctx.doc(), f.keys)->devices, made[0], from->id));
    const bool toIn = to != nullptr && (to->id == made[0] || doc::groupContains(doc::findTrackPtr(*f.ctx.doc(), f.keys)->devices, made[0], to->id));
    CHECK_MESSAGE(fromIn == toIn, "a wire crosses the dropped group's edge");
  }
  CHECK(doc::wiresOf(*f.ctx.doc()).size() > wires);  // (its inner circuit)
  CHECK(f.view().patch().nodes[f.view().patch().nodeOf(made[0])].offPath);
}

TEST_CASE("SM.8 sample drop: on an audio track the Sampler is placed unwired at the drop point") {
  Fixture f;
  const std::string a = toneWav("audio", "Vox.wav");
  const Vec2 at = f.onCircuit(f.loop);
  const std::vector<doc::DeviceId> before = f.blocks(f.loop);
  const size_t clips = doc::findTrackPtr(*f.ctx.doc(), f.loop)->clips.size();
  const size_t h = f.history();
  CHECK(f.dropFiles({a}, at) == DropEffect::Copy);
  CHECK(f.history() == h + 1);
  const std::vector<doc::DeviceId> made = f.newBlocks(f.loop, before);
  REQUIRE(made.size() == 1);
  CHECK(f.path(f.loop).empty());  // unwired (an audio track's path holds no block)
  CHECK(doc::findTrackPtr(*f.ctx.doc(), f.loop)->clips.size() == clips);  // not a clip
  CHECK(f.view().patch().nodes[f.view().patch().nodeOf(made[0])].offPath);
  f.checkPlacedAt(made[0], at);
  CHECK(f.sampleOf(made[0]) != 0);
}

TEST_CASE("SM.8 sample drop: a file dropped on a Sampler replaces its sample (no block added; one undo step)") {
  Fixture f;
  const std::string a = toneWav("replace", "A440.wav"), b = toneWav("replace", "E330.wav", 330.0);
  const std::vector<doc::DeviceId> before = f.blocks(f.loop);
  REQUIRE(f.dropFiles({a}, f.onCircuit(f.loop)) == DropEffect::Copy);
  const std::vector<doc::DeviceId> made = f.newBlocks(f.loop, before);
  REQUIRE(made.size() == 1);
  const uint32_t first = f.sampleOf(made[0]);
  const size_t n = f.devices(f.loop);
  const size_t h = f.history();
  const Vec2 on = f.onNode(made[0]);
  {  // hovering it: the Sampler is the target (its outline)
    DragPayload p;
    p.kind = DragKind::Files;
    p.paths = {b};
    f.d.tree().dragEnter(p, on, 0);
    (void)f.d.tree().dragMove(on, 0);
    CHECK(f.view().sampleDropHover().replace == made[0]);
    f.d.tree().dragExit();
    CHECK_FALSE(f.view().sampleDropHover().on);
  }
  CHECK(f.dropFiles({b}, on) == DropEffect::Copy);
  CHECK(f.history() == h + 1);
  CHECK(f.devices(f.loop) == n);
  const uint32_t second = f.sampleOf(made[0]);
  CHECK(second != first);
  CHECK(doc::findSample(*f.ctx.doc(), doc::SampleId{second})->displayName == "E330");
  CHECK(std::fabs(1200.0 * std::log2(f.rootOf(made[0]) / 330.0)) < 5.0);
  REQUIRE(f.ctx.undo());
  CHECK(f.sampleOf(made[0]) == first);
  CHECK(std::fabs(1200.0 * std::log2(f.rootOf(made[0]) / 440.0)) < 5.0);
}

TEST_CASE("SM.8 sample drop: several files make one Sampler each, the second under the first; one undo step") {
  Fixture f;
  const std::string a = toneWav("multi", "One.wav"), b = toneWav("multi", "Two.wav", 330.0);
  const std::vector<doc::DeviceId> before = f.blocks(f.loop);
  const size_t h = f.history();
  CHECK(f.dropFiles({a, b}, f.onCircuit(f.loop)) == DropEffect::Copy);
  CHECK(f.history() == h + 1);
  const std::vector<doc::DeviceId> made = f.newBlocks(f.loop, before);
  REQUIRE(made.size() == 2);
  CHECK(f.sampleOf(made[0]) != f.sampleOf(made[1]));
  const ProjectPatch& pp = f.view().patch();
  const Node& one = pp.nodes[pp.nodeOf(made[0])];
  const Node& two = pp.nodes[pp.nodeOf(made[1])];
  CHECK(two.y >= one.y + one.h);  // under it, apart
  CHECK(std::fabs((two.x + two.w * 0.5f) - (one.x + one.w * 0.5f)) < 2.0f);
  REQUIRE(f.ctx.undo());
  CHECK(f.blocks(f.loop) == before);
}

TEST_CASE("SM.8 sample drop: a browser sample dragged over the circuit shows where it goes; Esc ends the drag, nothing "
          "placed") {
  Fixture f;
  f.b->setSource({BrowserSource::Kind::Place, 0});
  int32_t row = -1;
  for (size_t i = 0; i < f.b->rows().size(); ++i)
    if (!f.b->rows()[i].dir && !f.b->rows()[i].device && f.b->index()->name(f.b->rows()[i].id) == "beat.wav") row = static_cast<int32_t>(i);
  REQUIRE(row >= 0);
  f.d.tree().layoutNow();
  const IRect rr = f.b->rowRect(row);
  const Vec2 from{static_cast<float>(rr.x + rr.w / 3), static_cast<float>(rr.y + rr.h / 2)};
  const Vec2 to = f.onCircuit(f.loop);
  const std::vector<doc::DeviceId> before = f.blocks(f.loop);
  const size_t h = f.history();
  f.d.move(from);
  f.d.down(from);
  f.d.move({from.x + 20, from.y});
  REQUIRE(f.d.tree().dragging());
  REQUIRE(f.d.tree().dragPayload()->kind == DragKind::BrowserItems);
  f.d.move(to);
  CHECK(f.d.tree().dropFeedback().effect == DropEffect::Copy);
  CHECK(f.view().sampleDropHover().on);
  CHECK(f.view().sampleDropHover().band == f.view().patch().bandOf(f.loop));
  f.d.step();
  CHECK(f.d.press("Escape") == KeyResult::Widget);
  CHECK_FALSE(f.d.tree().dragging());
  CHECK_FALSE(f.view().sampleDropHover().on);
  f.d.up(to);
  CHECK(f.history() == h);
  CHECK(f.blocks(f.loop) == before);
}
