// GU.7 (docs/plans/GU.md "Phases" 7): the native engines' faces as rich elements (pq/views/src/panel/engine_elements.cpp)
// and the modules built from them (pq/document/modules.h engineModuleBlocks).
//   goldens   module-wavetable-panel and module-envelope-panel (2x: the closed modules, the old faces rebuilt from the
//             rich elements plus generic knobs / sliders), module-scope-panel, module-spectrum-panel,
//             module-spectrogram-panel (1x, no signal: their grids), panel-rich-elements (each rich kind ALONE on one
//             panel — harmonic editor, draw tools, warp mode, table display, envelope editor, meter, scope, spectrum,
//             spectrogram — bound to the modules' engines, the analysers scaled into smaller rects)
//   gestures  through real dispatch on the closed modules in the shell's project patch view: a drag across the
//             Wavetable's harmonic bars repaints the edited keyframe's harmonics (the engine's data: one undo step, no
//             recompile); a drag of the Envelope's decay breakpoint down lowers its sustain (one undo step, no recompile)
//   taps      placing a scope on a block's output is one structural edit (the lowering taps it); moving it is not
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/meta/envelope.h"
#include "pq/meta/registry.h"
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

constexpr doc::ModuleKind kKinds[] = {doc::ModuleKind::Wavetable, doc::ModuleKind::Envelope, doc::ModuleKind::Oscilloscope,
                                      doc::ModuleKind::Spectrum, doc::ModuleKind::Spectrogram};

// A MIDI track holding one module of each engine kind (pasted as the node menu does).
struct EngineDoc {
  doc::TrackId track;
  std::vector<doc::EngineRoles> m;  // in kKinds order
  const doc::EngineRoles& of(doc::ModuleKind k) const {
    for (size_t i = 0; i < std::size(kKinds); ++i)
      if (kKinds[i] == k) return m[i];
    FAIL("no module");
    return m[0];
  }
};
EngineDoc buildEngines(doc::DocumentStore& st) {
  EngineDoc d;
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(st.dispatch(e->command).ok());
    return *e;
  };
  d.track = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Engines")).tracks.at(0);
  for (const doc::ModuleKind k : kKinds) {
    doc::IdSource local;
    doc::EngineRoles l;
    const doc::ops::Edit e = run(doc::ops::pasteBlocks(*st.current(), st.ids(), d.track, doc::engineModuleBlocks(local, k, {}, &l)));
    d.m.push_back({e.devices.at(l.group.value - 1), e.devices.at(l.engine.value - 1)});
  }
  return d;
}

// A panel in a plain tree at `scale` (as Standalone shows one): the module's own (source: the document) or a fixed one.
Image panelImage(ViewContext& ctx, doc::DeviceId group, doc::PanelPtr fixed, float scale) {
  const doc::PanelPtr p = fixed ? fixed : doc::findDevice(*ctx.doc(), group).device->panel;
  REQUIRE(p != nullptr);
  TestDriver d(static_cast<uint32_t>(p->width * scale), static_cast<uint32_t>(p->height * scale), scale);
  panel::PanelHost* host = nullptr;
  panel::GroupPanelFace::Source src = fixed ? panel::GroupPanelFace::Source([fixed] { return fixed; }) : panel::docPanelSource(ctx, group);
  auto face = std::make_unique<panel::GroupPanelFace>(std::move(src),
                                                      panel::makeDocPanelTarget(ctx, group, [&host]() -> Widget* { return host; }));
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

doc::PanelElement el(uint32_t id, const char* kind, float x, float y, float w, float h, doc::Binding b) {
  doc::PanelElement e;
  e.id = id;
  e.kind = kind;
  e.x = x, e.y = y, e.w = w, e.h = h;
  e.bindings = {std::move(b)};
  return e;
}

// The modules in the shell's project patch view (they spawn closed), the view at zoom 1 on one.
struct EngineUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  EngineDoc m;
  int structural = 0;
  ViewContext::Subscription sub;

  explicit EngineUi(doc::ModuleKind on) {
    ctx.setDeviceLibrary(&devLib());
    m = buildEngines(ctx.store());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    frame(m.of(on).group);
    d.tree().setFocus(&view().canvas());
    sub = ctx.subscribe([this](const doc::StoreEvent& e) { structural += e.before != e.after && e.changes.structural() ? 1 : 0; });
  }
  ~EngineUi() {
    sub.reset();
    d.tree().setRoot(std::make_unique<Widget>());
  }
  void frame(doc::DeviceId g) {
    const Node& n = node(g);
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = n.x - 40.0;
    v.y = n.y - 40.0;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
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
  Vec2 at(doc::DeviceId g, Vec2 local) const {
    const Node& n = node(g);
    return view().canvas().toDevice({n.x + local.x * n.scale, n.y + local.y * n.scale});
  }
  size_t history() const { return ctx.store().history().cursor(); }
  const doc::Device& dev(doc::DeviceId id) const { return *doc::findDevice(*ctx.doc(), id).device; }
};

meta::DataPtr dataOf(const doc::Device& d) {
  const doc::MetaInstance* m = d.metaAt("");
  return m != nullptr ? m->data : nullptr;
}

}  // namespace

TEST_CASE("render: the engine modules' closed panels — the old faces from rich elements (wavetable, envelope 2x; analysers)") {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  const EngineDoc m = buildEngines(ctx.store());
  checkImage("module-wavetable-panel", panelImage(ctx, m.of(doc::ModuleKind::Wavetable).group, nullptr, 2.0f));
  checkImage("module-envelope-panel", panelImage(ctx, m.of(doc::ModuleKind::Envelope).group, nullptr, 2.0f));
  checkImage("module-scope-panel", panelImage(ctx, m.of(doc::ModuleKind::Oscilloscope).group, nullptr, 1.0f));
  checkImage("module-spectrum-panel", panelImage(ctx, m.of(doc::ModuleKind::Spectrum).group, nullptr, 1.0f));
  checkImage("module-spectrogram-panel", panelImage(ctx, m.of(doc::ModuleKind::Spectrogram).group, nullptr, 1.0f));
}

TEST_CASE("render: panel-rich-elements — every rich element alone on a panel, bound to an engine") {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  const EngineDoc m = buildEngines(ctx.store());
  const doc::DeviceId wt = m.of(doc::ModuleKind::Wavetable).engine, env = m.of(doc::ModuleKind::Envelope).engine;
  auto p = std::make_shared<doc::Panel>();
  p->width = 640.0f;
  p->height = 360.0f;
  p->elements = {
      el(1, "harmonics", 8, 8, 184, 42, doc::dataBinding(wt)),
      el(2, "drawtools", 8, 60, 188, 21, doc::dataBinding(wt)),
      el(3, "warpmode", 8, 90, 168, 16, doc::dataBinding(wt)),
      el(4, "wavetable", 8, 116, 192, 100, doc::dataBinding(wt)),
      el(5, "envelope", 212, 8, 178, 116, doc::dataBinding(env)),
      el(6, "meter", 400, 12, 24, 108, doc::signalBinding(wt, "out")),
      el(7, "scope", 212, 136, 195, 120, doc::dataBinding(m.of(doc::ModuleKind::Oscilloscope).engine)),
      el(8, "spectrum", 8, 230, 192, 120, doc::dataBinding(m.of(doc::ModuleKind::Spectrum).engine)),
      el(9, "spectrogram", 420, 136, 212, 120, doc::dataBinding(m.of(doc::ModuleKind::Spectrogram).engine)),
  };
  // Each kind is registered (none is drawn as the unknown-kind placeholder).
  for (const doc::PanelElement& e : p->elements) CHECK(panel::elementSchema(e.kind) != nullptr);
  checkImage("panel-rich-elements", panelImage(ctx, m.of(doc::ModuleKind::Wavetable).group, p, 1.0f));
}

TEST_CASE("engine elements: a drag across the Wavetable's harmonic bars edits the keyframe — one undo step, no recompile "
          "— real dispatch") {
  EngineUi u(doc::ModuleKind::Wavetable);
  const doc::EngineRoles& r = u.m.of(doc::ModuleKind::Wavetable);
  CHECK_FALSE(u.view().groupOpen(r.group));  // spawned closed: drawn as its panel
  CHECK(u.node(r.group).groupPanel);
  const meta::DataPtr before = dataOf(u.dev(r.engine));
  const size_t h0 = u.history();
  // The amplitude row (panel 36 … 58): from the 3rd bar to the 15th, near the top (loud).
  u.d.drag(u.at(r.group, {35.0f, 39.0f}), u.at(r.group, {110.0f, 39.0f}), 10);
  u.d.step();
  const meta::DataPtr after = dataOf(u.dev(r.engine));
  REQUIRE(after != nullptr);
  CHECK(after != before);
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);
  REQUIRE(u.ctx.undo());
  CHECK(dataOf(u.dev(r.engine)) == before);
  // The fold knob (a generic knob bound to the engine's control) dragged up: one step, a runtime param.
  const size_t h1 = u.history();
  const Vec2 k = u.at(r.group, {38.0f, 212.0f});
  u.d.drag(k, {k.x, k.y - 40.0f}, 6);
  u.d.step();
  const doc::DeviceValue* fold = u.dev(r.engine).param("fold");
  REQUIRE(fold != nullptr);
  CHECK(fold->value > 0.05);
  CHECK(u.history() == h1 + 1);
  CHECK(u.structural == 0);
}

TEST_CASE("engine elements: the Envelope's decay breakpoint dragged down lowers its sustain — one undo step, no recompile "
          "— real dispatch") {
  EngineUi u(doc::ModuleKind::Envelope);
  const doc::EngineRoles& r = u.m.of(doc::ModuleKind::Envelope);
  CHECK_FALSE(u.view().groupOpen(r.group));
  // The decay's end (envelope_face.cpp layout): x = 20 + 44·fit·(t_attack + t_decay), y at the sustain level.
  const meta::Type& t = meta::envelopeType();
  auto travel = [&](uint32_t i) { return static_cast<float>(meta::valueToTravel(t.params[i], t.params[i].def)); };
  const float ta = travel(meta::kEnvAttack), td = travel(meta::kEnvDecay), tr = travel(meta::kEnvRelease);
  const float fit = std::clamp(136.0f / (44.0f * (ta + td + tr)), 1.0f, 4.0f);
  const Vec2 decayEnd{20.0f + 44.0f * fit * (ta + td), 104.0f + (26.0f - 104.0f) * 0.6f};
  const double sus0 = meta::valueToTravel(t.params[meta::kEnvSustain], 0.6);
  const size_t h0 = u.history();
  const Vec2 from = u.at(r.group, decayEnd);
  u.d.drag(from, {from.x, from.y + 24.0f}, 8);
  u.d.step();
  const doc::DeviceValue* sus = u.dev(r.engine).param("sustain");
  REQUIRE(sus != nullptr);
  CHECK(sus->value < sus0 - 0.1);
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);
  REQUIRE(u.ctx.undo());
  CHECK(u.dev(r.engine).param("sustain") == nullptr);
}

TEST_CASE("engine elements: placing a scope on a block's output is one structural edit (its tap); moving it is not") {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  const EngineDoc m = buildEngines(ctx.store());
  const doc::EngineRoles& w = m.of(doc::ModuleKind::Wavetable);
  int structural = 0;
  auto sub = ctx.subscribe([&](const doc::StoreEvent& e) { structural += e.before != e.after && e.changes.structural() ? 1 : 0; });
  doc::PanelElement scope = el(0, "scope", 0, 100, 130, 80, doc::signalBinding(w.engine, "out"));
  auto add = doc::ops::addPanelElements(*ctx.doc(), w.group, {scope});
  REQUIRE(add.ok());
  REQUIRE(ctx.store().dispatch(add->command).ok());
  CHECK(structural == 1);
  const doc::Panel& p = *doc::findDevice(*ctx.doc(), w.group).device->panel;
  const std::vector<doc::PanelTap> taps = doc::panelTaps(p);
  REQUIRE(taps.size() == 1);
  CHECK(taps[0].kind == doc::ElementTap::Signal);
  CHECK(taps[0].device == w.engine);
  doc::PanelElement moved = p.elements.back();
  moved.x += 10.0f;
  auto mv = doc::ops::setPanelElement(*ctx.doc(), w.group, moved);
  REQUIRE(mv.ok());
  REQUIRE(ctx.store().dispatch(mv->command).ok());
  CHECK(structural == 1);
}

// ---- NZ.1: the Noise module -------------------------------------------------------------------------------------------

namespace {

// One audio track holding a Noise module (pasted as the node menu does).
doc::EngineRoles buildNoise(doc::DocumentStore& st) {
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(st.dispatch(e->command).ok());
    return *e;
  };
  const doc::TrackId t = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, {}, "Noise")).tracks.at(0);
  doc::IdSource local;
  doc::EngineRoles l;
  const doc::ops::Edit e = run(doc::ops::pasteBlocks(*st.current(), st.ids(), t, doc::engineModuleBlocks(local, doc::ModuleKind::Noise, {}, &l)));
  return {e.devices.at(l.group.value - 1), e.devices.at(l.engine.value - 1)};
}

// A voice tap that plays a deterministic random walk (four samples per read) for `reads` reads, then stands.
struct WalkVoices final : VoiceTapReader {
  uint32_t reads = 0, maxReads = 0;
  double v = 0.0;
  uint64_t s = 12345;
  uint32_t read(std::span<FaceVoice> out) override {
    if (out.empty()) return 0;
    FaceVoice& f = out[0];
    f.slot = 0;
    f.sampleCount = 0;
    if (reads < maxReads) {
      ++reads;
      for (uint32_t k = 0; k < 4; ++k) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        v = std::clamp(v + (static_cast<double>(s >> 40) / 16777216.0 - 0.5) * 0.12, -1.0, 1.0);
        f.samples[f.sampleCount++] = static_cast<float>(v);
      }
    }
    f.phase = static_cast<float>(v);
    if (out.size() > 1) {  // a second voice, held, its dot only
      out[1] = FaceVoice{};
      out[1].slot = 1;
      out[1].phase = 0.55f;
      return 2;
    }
    return 1;
  }
  uint64_t serial() const noexcept override { return live() ? reads + 1u : reads; }  // new records while it moves
  bool live() const noexcept override { return reads < maxReads; }
};

}  // namespace

TEST_CASE("render: module-noise-panel (2x) — the display, the chips, the knobs; Sync clock + pink colour; the trace") {
  FakeTransport port;
  ViewContext ctx{port};
  ctx.setDeviceLibrary(&devLib());
  const doc::EngineRoles r = buildNoise(ctx.store());
  checkImage("module-noise-panel", panelImage(ctx, r.group, nullptr, 2.0f));
  // Sync clock (the note-value menu instead of the Rate knob), Colour −3 dB/oct (the pink slope line), and a trace.
  auto set = [&](const doc::ops::EditResult& e) {
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
  };
  set(doc::ops::setMetaSetting(*ctx.doc(), r.engine, "", "noise", "clock", 1.0));
  set(doc::ops::setMetaParamValue(*ctx.doc(), r.engine, "", "noise", "colour", -3.0));
  WalkVoices walk;
  walk.maxReads = 60;
  ctx.setFaceVoices([&walk](doc::DeviceId, std::string_view) -> VoiceTapReader* { return &walk; });
  const doc::PanelPtr p = doc::findDevice(*ctx.doc(), r.group).device->panel;
  TestDriver d(static_cast<uint32_t>(p->width * 2.0f), static_cast<uint32_t>(p->height * 2.0f), 2.0f);
  panel::PanelHost* host = nullptr;
  auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, r.group),
                                                      panel::makeDocPanelTarget(ctx, r.group, [&host]() -> Widget* { return host; }));
  host = &d.setRoot<panel::PanelHost>("panel", std::move(face));
  d.tree().layoutNow();
  REQUIRE(d.enableGpu().ok());
  REQUIRE(d.render().ok());  // the first paint starts the panel's voice timer (polled while the reader is live)
  for (int i = 0; i < 90; ++i) d.advance(1.0 / 60.0);
  CHECK(walk.reads == walk.maxReads);  // the panel polled the engine's voices while they moved
  auto img = d.render();
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  checkImage("module-noise-panel-sync-trace", *img);
  d.tree().setRoot(std::make_unique<Widget>());
}

namespace {
struct NoiseUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  doc::EngineRoles r;
  int structural = 0;
  ViewContext::Subscription sub;
  NoiseUi() {
    ctx.setDeviceLibrary(&devLib());
    r = buildNoise(ctx.store());
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
  ~NoiseUi() {
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
  const doc::Device& engine() const { return *doc::findDevice(*ctx.doc(), r.engine).device; }
  double setting(const char* name) const {  // a chain device's own setting (unset: the type's default, 0)
    const doc::DeviceValue* v = engine().setting(name);
    return v != nullptr ? v->value : 0.0;
  }
};
}  // namespace

TEST_CASE("engine elements: the Noise panel's Clock chip and Colour knob — real dispatch (a setting: one structural step; a "
          "knob: one step, no recompile)") {
  NoiseUi u;
  CHECK_FALSE(u.view().groupOpen(u.r.group));  // spawned closed: drawn as its panel
  CHECK(u.node(u.r.group).groupPanel);
  CHECK(u.setting("clock") == 0.0);
  const size_t h0 = u.ctx.store().history().cursor();
  // The Clock chips (8, 68, 112 × 16): "Sync", the middle cell.
  u.d.click(u.at({64.0f, 76.0f}));
  u.d.step();
  CHECK(u.setting("clock") == 1.0);
  CHECK(u.ctx.store().history().cursor() == h0 + 1);
  CHECK(u.structural == 1);  // a setting: the expansion changes (one recompile)
  // The Colour knob (120, 90, 52 × 58) dragged up: a runtime param, one step.
  const size_t h1 = u.ctx.store().history().cursor();
  const Vec2 k = u.at({146.0f, 116.0f});
  u.d.drag(k, {k.x, k.y - 40.0f}, 6);
  u.d.step();
  const doc::DeviceValue* colour = u.engine().param("colour");
  REQUIRE(colour != nullptr);
  CHECK(colour->value > 0.55);  // above the middle (0 dB/oct)
  CHECK(u.ctx.store().history().cursor() == h1 + 1);
  CHECK(u.structural == 1);
  REQUIRE(u.ctx.undo());
  REQUIRE(u.ctx.undo());
  CHECK(u.setting("clock") == 0.0);
}
