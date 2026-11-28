// SM.10 (docs/plans/SM.md "Face (👁)" Closed): the Sampler group's CLOSED FACE — its panel's oval (152 × 84: the block
// is that oval), the engine face's `samplewave` section (the sample's waveform from Start to End, a glowing playhead per
// sounding voice), the mini knobs Transpose · Filter · Volume and the Expand chevron (a fold: view state, never an undo
// step; open, the SM.11 expanded panel shows beside and under the oval and the block is a rectangle again).
//   goldens   sampler-face-empty / -sample / -voices (three voices sounding), 1x and 2x
//   hidden    a face off the screen (the canvas scrolled away, the patch view hidden) leaves its tap without a reader
//             (pq::VoiceTap::wanted false: the audio side's write is one relaxed load); on screen again it has one
//   chevron   a click on the chevron through the window (window → canvas → panel face → fold element) opens the settings
//             (the block grows in place, the tabs show), a second one closes them; no undo step either way
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/core/grain_tap.h"
#include "pq/core/voice_tap.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/document/modules.h"
#include "pq/media/wav_file.h"
#include "pq/meta/sampler.h"
#include "pq/views/waveforms.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/radial_menu.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/meta/face_targets.h"
#include "pq/views/meta/tap_voices.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "views_test_util.h"
#include "../../src/views/src/meta/faces.h"  // WF.1: the Sampler face as it last painted (tests)

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

// A plucked tone: 0.6 s of a 220 Hz saw-ish tone decaying, mono 48 kHz (a recognisable envelope in the overview).
std::string pluckWav() {
  const std::filesystem::path dir(views::test::tempDir("sampler_face"));
  const size_t n = 28800;
  std::vector<std::vector<float>> ch(1, std::vector<float>(n, 0.0f));
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / 48000.0;
    const double ph = 2.0 * std::numbers::pi * 220.0 * t;
    ch[0][i] = static_cast<float>(0.8 * std::exp(-5.0 * t) * (std::sin(ph) + 0.4 * std::sin(2.0 * ph) + 0.2 * std::sin(3.0 * ph)) / 1.6);
  }
  const std::filesystem::path p = dir / "Pluck.wav";
  REQUIRE(media::writeWav(p, ch, 48000).ok());
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// One MIDI track whose path is the Sampler (its default instrument dissolved), its sample a plucked tone (or none).
struct SamplerProject {
  doc::TrackId track;
  doc::DeviceId group, engine;
  doc::SampleId sample;
};
SamplerProject buildSamplerProject(doc::DocumentStore& st, bool withSample) {
  SamplerProject r;
  auto t = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys");
  REQUIRE(t.ok());
  REQUIRE(st.dispatch(t->command).ok());
  r.track = t->tracks[0];
  {
    std::vector<doc::DeviceId> heads;
    for (const doc::Device* d : doc::mainPath(*st.current(), *doc::findTrackPtr(*st.current(), r.track)).blocks) heads.push_back(d->id);
    if (!heads.empty()) {
      auto e = doc::ops::dissolveDevices(*st.current(), st.ids(), nullptr, heads);
      REQUIRE(e.ok());
      REQUIRE(st.dispatch(e->command).ok());
    }
  }
  doc::SamplerOptions o;
  if (withSample) {
    const std::string path = pluckWav();
    doc::SampleRef s;
    s.id = st.ids().sample();
    s.path = path;
    s.size = std::filesystem::file_size(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())));
    s.frames = 28800;
    s.rate = 48000.0;
    s.channels = 1;
    s.displayName = "Pluck";
    REQUIRE(st.dispatch(doc::Command{doc::AddSample{s}}).ok());
    r.sample = s.id;
    o.sample = s.id.value;
  }
  doc::IdSource local;
  auto e = doc::ops::pasteBlocksAtNotes(*st.current(), st.ids(), r.track, doc::samplerBlocks(local, o));
  REQUIRE(e.ok());
  REQUIRE(st.dispatch(e->command).ok());
  r.group = e->devices.at(0);
  doc::forEachDevice(*st.current(), [&](const doc::Device& d, const doc::Track&) {
    if (d.patch == "meta:sampler" && d.group == r.group) r.engine = d.id;
  });
  REQUIRE(r.engine.valid());
  return r;
}

// The group's panel in a plain tree, the host the closed face's size (the goldens).
struct Standalone {
  FakeTransport port;
  ViewContext ctx{port};
  SyntheticVoices voices;
  SamplerProject ids;
  TestDriver d;
  panel::PanelHost* host = nullptr;
  Standalone(float scale, bool withSample, std::vector<SyntheticVoices::Voice> sounding, float w = doc::kSamplerFaceW,
             float h = doc::kSamplerFaceH)
      : d(static_cast<uint32_t>(w * scale), static_cast<uint32_t>(h * scale), scale) {
    ctx.setDeviceLibrary(&devLib());
    ids = buildSamplerProject(ctx.store(), withSample);
    voices.setVoices(std::move(sounding));
    ctx.setFaceVoices([this](doc::DeviceId dev, std::string_view) -> views::VoiceTapReader* { return dev == ids.engine ? &voices : nullptr; });
    auto face = std::make_unique<panel::GroupPanelFace>(panel::docPanelSource(ctx, ids.group),
                                                        panel::makeDocPanelTarget(ctx, ids.group, [this]() -> Widget* { return host; }));
    host = &d.setRoot<panel::PanelHost>("panel", std::move(face));
    d.tree().layoutNow();
  }
  ~Standalone() { d.tree().setRoot(std::make_unique<Widget>()); }
  panel::GroupPanelFace& face() const { return static_cast<panel::GroupPanelFace&>(host->face()); }
  Image render() {
    REQUIRE(d.enableGpu().ok());
    d.advance(0.1);
    auto img = d.render();
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    return std::move(*img);
  }
};

// The project in the shell's project patch view, the Sampler closed (drawn as its panel) at zoom 1; its engine's voices
// a real TapVoices over a real pq::VoiceTap (the app's reader, without the link).
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  std::shared_ptr<pq::VoiceTap> tap = std::make_shared<pq::VoiceTap>(pq::VoiceTapConfig{});
  std::shared_ptr<pq::GrainTap> grainTap = std::make_shared<pq::GrainTap>(pq::GrainTapConfig{1, 8});  // SM.13
  bool granular = false;  // SM.13: the grain tap is the instance's (a Granular Sampler has one)
  std::unique_ptr<TapVoices> reader;
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  SamplerProject ids;
  Ui() {
    ctx.setDeviceLibrary(&devLib());
    ids = buildSamplerProject(ctx.store(), true);
    reader = std::make_unique<TapVoices>([t = tap] { return t; }, TapVoices::TableResolve{}, TapVoices::RateResolve{},
                                         TapVoices::GenerationResolve{},
                                         [this] { return granular ? grainTap : std::shared_ptr<pq::GrainTap>{}; });
    ctx.setFaceVoices([this](doc::DeviceId dev, std::string_view) -> views::VoiceTapReader* { return dev == ids.engine ? reader.get() : nullptr; });
    ctx.setDetailVisible(false);  // only the canvas's face (the Device view's panel says nothing: shown while it exists)
    REQUIRE(d.enableGpu().ok());  // (frames are painted: the canvas tells its faces whether they are on screen)
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    view().setGroupOpen(ids.group, false);
    d.tree().layoutNow();
    frame();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }
  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const Node& node() const {
    const uint32_t n = view().patch().nodeOf(ids.group);
    REQUIRE(n != kNone);
    return view().patch().nodes[n];
  }
  void frame(float dx = 0.0f) {
    const Node& n = node();
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = n.x - 60.0 + dx;
    v.y = n.y - 60.0;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
  }
  panel::GroupPanelFace& face() const {
    const uint32_t mn = view().patch().nodeOf(ids.group);
    REQUIRE(mn != kNone);
    Face* f = view().canvas().panelOn(view().scene().sceneNode[mn]);
    REQUIRE(f != nullptr);
    auto* g = dynamic_cast<panel::GroupPanelFace*>(f);
    REQUIRE(g != nullptr);
    (void)g->sync();
    return *g;
  }
  Vec2 at(uint32_t id, float fx = 0.5f, float fy = 0.5f) const {
    panel::GroupPanelFace& f = face();
    const int32_t i = f.indexOf(id);
    REQUIRE(i >= 0);
    const Rect r = f.element(static_cast<size_t>(i)).rect();
    const Node& n = node();
    return view().canvas().toDevice({n.x + r.x0 + (r.x1 - r.x0) * fx, n.y + r.y0 + (r.y1 - r.y0) * fy});
  }
  std::optional<bool> foldState(uint32_t id) const {
    const auto& all = ctx.store().view().panelWindows;
    const auto it = all.find(ids.group.value);
    return it == all.end() ? std::nullopt : it->second.foldOpen(id);
  }
  uint32_t expandId() const {
    const doc::Device* g = doc::findDevice(*ctx.doc(), ids.group).device;
    REQUIRE(g != nullptr);
    REQUIRE(g->hasPanel());
    for (const doc::PanelElement& e : g->panel->elements)
      if (e.kind == doc::kFoldKind) return e.id;
    FAIL("no chevron");
    return 0;
  }
  uint32_t tabsId() const {
    for (const doc::PanelElement& e : doc::findDevice(*ctx.doc(), ids.group).device->panel->elements)
      if (e.kind == "tabs") return e.id;
    FAIL("no tabs");
    return 0;
  }
};

}  // namespace

TEST_CASE("render: sampler-face-empty / -sample / -voices (SM.10: the closed Sampler face, 1x and 2x)") {
  for (const float scale : {1.0f, 2.0f}) {
    CAPTURE(scale);
    const std::string sfx = scale == 1.0f ? "" : "-2x";
    {
      Standalone s(scale, false, {});
      const FaceShape sh = s.face().shape();
      CHECK(sh.round);  // the block is the oval
      CHECK(s.face().worldSize().x == doctest::Approx(doc::kSamplerFaceW));
      CHECK(s.face().worldSize().y == doctest::Approx(doc::kSamplerFaceH));  // the settings folded away
      checkImage("sampler-face-empty" + sfx, s.render());
    }
    {
      Standalone s(scale, true, {});
      checkImage("sampler-face-sample" + sfx, s.render());
    }
    {
      Standalone s(scale, true, {{0, 0.0, 0.12}, {1, 0.0, 0.41}, {2, 0.0, 0.73}});
      checkImage("sampler-face-voices" + sfx, s.render());
    }
  }
}

TEST_CASE("sampler face (SM.10): off the screen its tap has no reader; on screen again it has one") {
  Ui u;
  CHECK(u.tap->wanted());  // on screen: the reader is registered (the audio side samples into the tap)
  CHECK_FALSE(u.reader->hidden());
  // Scrolled far away: the face is off the canvas — the reader lets go of the tap.
  u.frame(6000.0f);
  CAPTURE(u.reader->faces());
  CAPTURE(u.reader->facesShown());
  CHECK(u.reader->hidden());
  CHECK_FALSE(u.tap->wanted());
  // Back: registered again at the next read.
  u.frame();
  CHECK_FALSE(u.reader->hidden());
  (void)u.reader->serial();  // (a read or poll resolves the tap)
  CHECK(u.tap->wanted());
  // The patch view hidden (the arrangement shown): no reader.
  u.view().setShown(false, false);
  u.d.step();
  CHECK(u.reader->hidden());
  CHECK_FALSE(u.tap->wanted());
  u.view().setShown(true, false);
  u.frame();
  (void)u.reader->serial();
  CHECK(u.tap->wanted());
}

TEST_CASE("sampler face (SM.10, real dispatch): the chevron opens the settings under the oval (the block grows in place, "
          "the tabs show) and closes them; never an undo step") {
  Ui u;
  const uint32_t chev = u.expandId(), tabs = u.tabsId();
  const Node n0 = u.node();
  CHECK(n0.panelRound);
  CHECK(n0.h == doctest::Approx(doc::kSamplerFaceH));
  CHECK_FALSE(u.face().shown(static_cast<size_t>(u.face().indexOf(tabs))));
  const size_t h0 = u.ctx.store().history().cursor();
  u.d.click(u.at(chev));
  u.d.step();
  CHECK(u.foldState(chev) == std::optional<bool>(true));
  CHECK(u.ctx.store().history().cursor() == h0);
  CHECK(u.node().h > doc::kSamplerFaceH + 100.0f);
  CHECK(u.node().x == doctest::Approx(n0.x));
  CHECK(u.node().y == doctest::Approx(n0.y));
  CHECK_FALSE(u.node().panelRound);  // a rectangle while the settings show (their controls take presses)
  CHECK(u.face().shown(static_cast<size_t>(u.face().indexOf(tabs))));
  // Closed again: the oval.
  u.d.click(u.at(chev));
  u.d.step();
  CHECK(u.foldState(chev) == std::optional<bool>(false));
  CHECK(u.node().h == doctest::Approx(doc::kSamplerFaceH));
  CHECK(u.node().panelRound);
  CHECK(u.ctx.store().history().cursor() == h0);
}

// ---- SM.11: the expanded panel and the waveform editor ----------------------------------------------------------------
// (docs/plans/SM.md "Face (👁)" Expanded): the chevron or a double-click on the closed waveform grows the Sampler in place
// into the mode row, the root field, the waveform editor and the fold sections (only Sample open; Slices only in Slice
// mode); every drag in the editor is one undo step; the root takes Hz or an identity of the track's tuning; the Filter's
// and the LFO's switches toggle their blocks (one step each).
//   goldens   sampler-panel-classic / -oneshot / -slice (expanded), 1x and 2x

namespace {

const doc::PanelElement* elementOf(const doc::Project& p, doc::DeviceId group, std::string_view kind, std::string_view title = {}) {
  const doc::Device* g = doc::findDevice(p, group).device;
  if (g == nullptr || !g->hasPanel()) return nullptr;
  for (const doc::PanelElement& e : g->panel->elements)
    if (e.kind == kind && (title.empty() || e.text("title", "") == title)) return &e;
  return nullptr;
}
// The id of the nth element of `kind` whose first binding is `name`.
uint32_t boundElement(const doc::Project& p, doc::DeviceId group, std::string_view kind, std::string_view name, int nth = 0) {
  const doc::Device* g = doc::findDevice(p, group).device;
  REQUIRE(g != nullptr);
  for (const doc::PanelElement& e : g->panel->elements)
    if (e.kind == kind && !e.bindings.empty() && e.bindings.front().name == name && nth-- == 0) return e.id;
  FAIL("no " << kind << " bound to " << name);
  return 0;
}
const doc::Device& engineOf(const doc::Project& p, doc::DeviceId engine) {
  const doc::Device* d = doc::findDevice(p, engine).device;
  REQUIRE(d != nullptr);
  return *d;
}
double engineValue(const doc::Project& p, doc::DeviceId engine, meta::SamplerParam param) {
  return doc::metaParamValue(engineOf(p, engine), "", meta::samplerType(), param);
}
meta::DataPtr engineData(const doc::Project& p, doc::DeviceId engine) {
  const doc::MetaInstance* m = engineOf(p, engine).metaAt("");
  return m != nullptr ? m->data : nullptr;
}
void setSetting(doc::DocumentStore& st, doc::DeviceId engine, const char* name, double v) {
  auto e = doc::ops::setMetaSetting(*st.current(), engine, "", "sampler", name, v);
  REQUIRE(e.ok());
  REQUIRE(st.dispatch(e->command).ok());
}
void setValue(doc::DocumentStore& st, doc::DeviceId engine, const char* name, double v) {
  auto e = doc::ops::setMetaParamValue(*st.current(), engine, "", "sampler", name, v);
  REQUIRE(e.ok());
  REQUIRE(st.dispatch(e->command).ok());
}
void setFloats(doc::DocumentStore& st, doc::DeviceId engine, const char* field, std::vector<float> v) {
  const meta::DataPtr d = engineData(*st.current(), engine);
  auto e = doc::ops::setMetaData(*st.current(), engine, "", "sampler",
                                 meta::samplerWith(d.get(), field, meta::DataValue::floatArray(std::move(v))));
  REQUIRE(e.ok());
  REQUIRE(st.dispatch(e->command).ok());
}

// The editor's geometry (sampler_face.cpp kEditor …): element-local points of the `sampleedit` element (its face section
// is 328 × 120 from face y 200: k = 1).
constexpr float kEdX0 = 6.0f, kEdX1 = 322.0f;
constexpr float kRulerY = 207.0f - 200.0f, kLoopBarY = 218.0f - 200.0f, kWaveMidY = 257.0f - 200.0f, kFlagY = 305.0f - 200.0f;
float edLocalX(double u) { return kEdX0 + static_cast<float>(u) * (kEdX1 - kEdX0); }

// The Ui with the expanded panel open (by the chevron, through the window).
struct Expanded : Ui {
  Expanded() {
    d.click(at(expandId()));
    d.step();
    REQUIRE(foldState(expandId()) == std::optional<bool>(true));
  }
  uint32_t idOf(std::string_view kind, std::string_view title = {}) const {
    const doc::PanelElement* e = elementOf(*ctx.doc(), ids.group, kind, title);
    REQUIRE(e != nullptr);
    return e->id;
  }
  // A device point of the editor at sample position u (0 … 1), element-local y.
  Vec2 ed(double u, float ly) const {
    panel::GroupPanelFace& f = face();
    const int32_t i = f.indexOf(idOf("sampleedit"));
    REQUIRE(i >= 0);
    const Rect r = f.element(static_cast<size_t>(i)).rect();
    const Node& n = node();
    return view().canvas().toDevice({n.x + r.x0 + edLocalX(u), n.y + r.y0 + ly});
  }
  bool shownEl(uint32_t id) const { return face().shown(static_cast<size_t>(face().indexOf(id))); }
  size_t cursor() const { return ctx.store().history().cursor(); }
  double value(meta::SamplerParam p) const { return engineValue(*ctx.doc(), ids.engine, p); }
  meta::DataPtr data() const { return engineData(*ctx.doc(), ids.engine); }
};

}  // namespace

TEST_CASE("sampler panel (SM.11, real dispatch): a double-click on the closed waveform expands it in place; the mode row, "
          "the root and the editor show; only Sample is open; Slices only in Slice mode") {
  Ui u;
  const Node n0 = u.node();
  const size_t h0 = u.ctx.store().history().cursor();
  // A single click on the waveform selects the block (no expand); the double-click expands.
  u.d.doubleClick(u.at(2));
  u.d.step();
  CHECK(u.foldState(u.expandId()) == std::optional<bool>(true));
  CHECK(u.ctx.store().history().cursor() == h0);
  CHECK(u.node().x == doctest::Approx(n0.x));
  CHECK(u.node().y == doctest::Approx(n0.y));
  CHECK(u.node().w == doctest::Approx(doc::kSamplerPanelW));
  CHECK(u.face().worldSize().x == doctest::Approx(doc::kSamplerPanelW));
  const auto id = [&](std::string_view kind, std::string_view title = {}) {
    const doc::PanelElement* e = elementOf(*u.ctx.doc(), u.ids.group, kind, title);
    REQUIRE(e != nullptr);
    return static_cast<size_t>(u.face().indexOf(e->id));
  };
  for (const size_t i : {id("tabs"), id("sampleroot"), id("sampleedit"), id("fold", "Sample"), id("fold", "Envelope"),
                         id("fold", "Filter"), id("fold", "LFO"), id("knob"), id("menu")})  // ("menu": the Warp pill)
    CHECK(u.face().shown(i));
  CHECK_FALSE(u.face().shown(id("fold", "Slices")));  // Classic: no Slices section
  // SM.17: the warp mode sits beside the oval; the Warp SECTION exists only for a stretch mode (warp is Off: none).
  CHECK_FALSE(u.face().shown(id("fold", "Warp")));
  const float hClassic = u.node().h;
  // The mode row (Classic | 1-Shot | Slice | Gran, SM.6): Slice (one undo step) — the Slices section appears, the loop controls give way to the fades.
  const panel::GroupPanelFace& f = u.face();
  const Rect tabs = f.element(id("tabs")).rect();
  const Node& n = u.node();
  u.d.click(u.view().canvas().toDevice({n.x + tabs.x0 + tabs.width() * (2.5f / 4.0f), n.y + (tabs.y0 + tabs.y1) * 0.5f}));
  u.d.step();
  CHECK(u.ctx.store().history().cursor() == h0 + 1);
  CHECK(std::lround(engineOf(*u.ctx.doc(), u.ids.engine).setting("mode")->value) == 2);
  CHECK(u.face().shown(id("fold", "Slices")));
  CHECK(u.node().h > hClassic);  // the block grew by the Slices section (SM.17: open by default)
  // The chevron closes it all again: the oval.
  u.d.click(u.at(u.expandId()));
  u.d.step();
  CHECK(u.node().h == doctest::Approx(doc::kSamplerFaceH));
  CHECK(u.node().w == doctest::Approx(doc::kSamplerFaceW));
  CHECK(u.node().panelRound);
}

TEST_CASE("sampler editor (SM.11, real dispatch): Start / End / loop drags, slice add / drag / remove, warp marker add / "
          "drag — one undo step each") {
  Expanded u;
  REQUIRE(u.ctx.waveforms().overview(u.ids.sample) != nullptr);
  // The loop's end (Classic loops Forward by default: its bar along the waveform's top) from 1 to 0.6.
  size_t h = u.cursor();
  u.d.drag(u.ed(1.0, kLoopBarY), u.ed(0.6, kLoopBarY), 6);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpLoopStart) + u.value(meta::kSmpLoopLength) == doctest::Approx(0.6).epsilon(0.01));
  // Start 0 → 0.25, End 1 → 0.8 (at the waveform's middle).
  h = u.cursor();
  u.d.drag(u.ed(0.0, kWaveMidY), u.ed(0.25, kWaveMidY), 6);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpStart) == doctest::Approx(0.25).epsilon(0.01));
  h = u.cursor();
  u.d.drag(u.ed(1.0, kFlagY), u.ed(0.8, kFlagY), 6);  // (by its flag)
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpEnd) == doctest::Approx(0.8).epsilon(0.01));
  // Undo: the End back.
  REQUIRE(u.ctx.store().undo());
  CHECK(u.value(meta::kSmpEnd) == doctest::Approx(1.0));
  // Slice mode: a double-click on the waveform adds a slice (one step), a drag moves it (one step), a double-click on
  // its line removes it (one step).
  setSetting(u.ctx.store(), u.ids.engine, "mode", 2.0);
  u.d.step();
  const size_t before = meta::samplerSlices(u.data().get()).size();
  h = u.cursor();
  u.d.doubleClick(u.ed(0.5, kWaveMidY));
  u.d.step();
  CHECK(u.cursor() == h + 1);
  std::vector<float> sl = meta::samplerSlices(u.data().get());
  REQUIRE(sl.size() == before + 1);
  const float frames = 28800.0f;
  auto nearest = [&](float f) {
    float best = -1e9f;
    for (const float x : meta::samplerSlices(u.data().get()))
      if (std::fabs(x - f) < std::fabs(best - f)) best = x;
    return best;
  };
  CHECK(nearest(0.5f * frames) == doctest::Approx(0.5f * frames).epsilon(0.01));
  h = u.cursor();
  u.d.drag(u.ed(0.5, kWaveMidY), u.ed(0.62, kWaveMidY), 6);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(nearest(0.62f * frames) == doctest::Approx(0.62f * frames).epsilon(0.01));
  CHECK(meta::samplerSlices(u.data().get()).size() == before + 1);
  h = u.cursor();
  u.d.doubleClick(u.ed(0.62, kWaveMidY));
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(meta::samplerSlices(u.data().get()).size() == before);
  // Classic + Beats: a double-click on the ruler adds a warp marker at the beat it reads there (one step); a drag along
  // the ruler moves its frame, its beat stays (one step).
  setSetting(u.ctx.store(), u.ids.engine, "mode", 0.0);
  setSetting(u.ctx.store(), u.ids.engine, "warp", 2.0);
  u.d.step();
  h = u.cursor();
  u.d.doubleClick(u.ed(0.5, kRulerY));
  u.d.step();
  CHECK(u.cursor() == h + 1);
  std::vector<float> m = meta::samplerMarkers(u.data().get());
  REQUIRE(m.size() == 2);
  CHECK(m[0] == doctest::Approx(0.5 * frames).epsilon(0.01));
  const double fpb = 60.0 * 48000.0 / 120.0;  // sampleBpm 120
  CHECK(m[1] == doctest::Approx(m[0] / fpb).epsilon(1e-4));
  const float beat = m[1];
  h = u.cursor();
  u.d.drag(u.ed(0.5, kRulerY), u.ed(0.4, kRulerY), 6);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  m = meta::samplerMarkers(u.data().get());
  REQUIRE(m.size() == 2);
  CHECK(m[0] == doctest::Approx(0.4 * frames).epsilon(0.01));
  CHECK(m[1] == beat);
}

TEST_CASE("sampler root field (SM.11, real dispatch): typed as Hz and as an identity of the track's tuning; the Hz | ID "
          "cells switch the display (one undo step each)") {
  Expanded u;
  const uint32_t root = u.idOf("sampleroot");
  const auto cell = [&](float fx) { return u.at(root, fx, 0.5f); };  // (the text box: 0 … 0.68, Hz, ID)
  // Hz.
  size_t h = u.cursor();
  u.d.click(cell(0.3f));
  u.d.type("440");
  (void)u.d.keyDown(ui::key::kReturn);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpRoot) == doctest::Approx(440.0));
  CHECK(meta::samplerRootText(u.data().get()).empty());
  // The ID cell: the identity shown, the root unchanged (one step).
  h = u.cursor();
  u.d.click(cell(0.93f));
  u.d.step();
  CHECK(u.cursor() == h + 1);
  const std::string named = meta::samplerRootText(u.data().get());
  MESSAGE("440 Hz names as '" << named << "'");
  CHECK_FALSE(named.empty());
  CHECK(u.value(meta::kSmpRoot) == doctest::Approx(440.0));
  // An identity typed with cents: the root moves to it (one step) and the text is kept.
  const std::string base = named.substr(0, named.find(" +") != std::string::npos ? named.find(" +") : named.find(" -"));
  h = u.cursor();
  u.d.click(cell(0.3f));
  u.d.type(base + " +50c");
  (void)u.d.keyDown(ui::key::kReturn);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpRoot) == doctest::Approx(440.0 * std::exp2(50.0 / 1200.0)).epsilon(1e-4));
  CHECK(meta::samplerRootText(u.data().get()) == base + " +50.0\xC2\xA2");
  // Not a pitch: nothing changes.
  h = u.cursor();
  u.d.click(cell(0.3f));
  u.d.type("banana");
  (void)u.d.keyDown(ui::key::kReturn);
  u.d.step();
  CHECK(u.cursor() == h);
  // The Hz cell: back to Hz (one step).
  u.d.click(cell(0.79f));
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(meta::samplerRootText(u.data().get()).empty());
}

TEST_CASE("sampler panel (SM.11, real dispatch): the Filter's and the LFO's switches beside their headers turn the blocks "
          "on and off (one undo step each; Q?-SM.10-a)") {
  Expanded u;
  // The switches, bound to the blocks' activators.
  std::vector<std::pair<uint32_t, doc::DeviceId>> sw;
  for (const doc::PanelElement& e : doc::findDevice(*u.ctx.doc(), u.ids.group).device->panel->elements)
    if (e.kind == "toggle" && e.binding() != nullptr && e.binding()->name == doc::kActiveSetting) sw.emplace_back(e.id, e.binding()->device);
  REQUIRE(sw.size() == 2);
  for (const auto& [el, dev] : sw) {
    CAPTURE(el);
    CHECK(u.shownEl(el));  // (beside a folded header: shown with the header)
    CHECK_FALSE(doc::findDevice(*u.ctx.doc(), dev).device->active);
    const size_t h = u.cursor();
    u.d.click(u.at(el));
    u.d.step();
    CHECK(u.cursor() == h + 1);
    CHECK(doc::findDevice(*u.ctx.doc(), dev).device->active);
    u.d.click(u.at(el));
    u.d.step();
    CHECK(u.cursor() == h + 2);
    CHECK_FALSE(doc::findDevice(*u.ctx.doc(), dev).device->active);
  }
}

TEST_CASE("render: sampler-panel-classic / -oneshot / -slice (SM.11: the expanded Sampler per mode, 1x and 2x)") {
  for (const float scale : {1.0f, 2.0f}) {
    CAPTURE(scale);
    const std::string sfx = scale == 1.0f ? "" : "-2x";
    for (const int mode : {0, 1, 2}) {
      CAPTURE(mode);
      Standalone s(scale, true, {{0, 0.0, 0.3}}, doc::kSamplerPanelW, 420.0f);  // (SM.17: Classic + Beats is 414 high)
      doc::DocumentStore& st = s.ctx.store();
      const doc::PanelElement* chev = elementOf(*st.current(), s.ids.group, "fold", "Settings");
      REQUIRE(chev != nullptr);
      st.view().panelWindows[s.ids.group.value].setFoldOpen(chev->id, true);
      if (mode == 0) {  // a forward loop with its fade, warp Beats with two markers (the ruler in beats)
        setValue(st, s.ids.engine, "loopStart", 0.35);
        setValue(st, s.ids.engine, "loopLength", 0.4);
        setValue(st, s.ids.engine, "loopFade", 0.04);
        setSetting(st, s.ids.engine, "warp", 2.0);
        setFloats(st, s.ids.engine, "markers", {4800.0f, 0.4f, 21600.0f, 2.0f});
      } else if (mode == 1) {  // One-Shot: Start / End in, the fades
        setSetting(st, s.ids.engine, "mode", 1.0);
        setValue(st, s.ids.engine, "start", 0.1);
        setValue(st, s.ids.engine, "end", 0.85);
        setValue(st, s.ids.engine, "fadeIn", 0.03);
        setValue(st, s.ids.engine, "fadeOut", 0.1);
      } else {  // Slice: six slices
        setSetting(st, s.ids.engine, "mode", 2.0);
        setFloats(st, s.ids.engine, "slices", {0.0f, 3000.0f, 7000.0f, 12000.0f, 17500.0f, 23000.0f});
      }
      (void)s.face().sync();
      CHECK(s.face().worldSize().x == doctest::Approx(doc::kSamplerPanelW));
      static constexpr const char* kNames[] = {"classic", "oneshot", "slice"};
      checkImage(std::string("sampler-panel-") + kNames[mode] + sfx, s.render());
    }
  }
}

// ---- SM.12: the slicing row (real dispatch) ----------------------------------------------------------------------------

TEST_CASE("sampler slicing (SM.12, real dispatch): Region / Beat / Manual cells re-slice (one undo step each), the amount "
          "drag re-slices in one step, Slice to Notes writes a clip at the start marker (one step)") {
  Expanded u;
  setSetting(u.ctx.store(), u.ids.engine, "mode", 2.0);
  u.d.step();
  REQUIRE(u.shownEl(u.idOf("fold", "Slices")));  // (SM.17: its section is open by default in Slice mode)
  const uint32_t row = u.idOf("sampleslice");
  REQUIRE(u.shownEl(row));
  // Element-local points of the row (its section 328 × 30 from face y 360: k = 1).
  const auto local = [&](float lx, float ly) {
    panel::GroupPanelFace& f = u.face();
    const Rect r = f.element(static_cast<size_t>(f.indexOf(row))).rect();
    const Node& n = u.node();
    return u.view().canvas().toDevice({n.x + r.x0 + lx, n.y + r.y0 + ly});
  };
  const auto cell = [&](int k) { return local(25.0f + 50.0f * static_cast<float>(k), 8.0f); };  // Transient Beat Region Manual
  const auto slices = [&] { return meta::samplerSlices(u.data().get()); };
  // Region: 8 equal parts (the default count).
  size_t h = u.cursor();
  u.d.click(cell(2));
  u.d.step();
  CHECK(u.cursor() == h + 1);
  REQUIRE(slices().size() == 8);
  CHECK(slices()[1] == doctest::Approx(28800.0 / 8.0));
  // The amount dragged up 30 px: 5 parts more, one undo step for the whole drag.
  h = u.cursor();
  u.d.drag(local(235.0f, 8.0f), local(235.0f, -22.0f), 6);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(slices().size() == 13);
  // Beat: every beat at sampleBpm 120 (24000 frames): 0.6 s holds two.
  h = u.cursor();
  u.d.click(cell(1));
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(slices() == std::vector<float>{0.0f, 24000.0f});
  // Manual: the slices stay, the way is Manual.
  h = u.cursor();
  u.d.click(cell(3));
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(slices() == std::vector<float>{0.0f, 24000.0f});
  CHECK(u.data()->field("sliceBy")->number == 0.0);
  // Slice to Notes: a clip of two notes at the start marker.
  u.ctx.setStartMarker(2 * kPpq);
  h = u.cursor();
  u.d.click(local(296.0f, 8.0f));
  u.d.step();
  CHECK(u.cursor() == h + 1);
  const doc::Track& t = *doc::findTrackPtr(*u.ctx.doc(), u.ids.track);
  REQUIRE(t.clips.size() == 1);
  REQUIRE(t.clips.front().clip != nullptr);
  CHECK(t.clips.front().clip->start == 2 * kPpq);
  CHECK(t.clips.front().clip->notes.size() == 2);
}

// ---- SM.13: Granular — the section, the grain region, the grain dots, the ratio chips, the closed face's knobs ----------
// (docs/plans/SM.md "Granular", "Face"): in Gran mode the expanded panel shows a Granular section (and no Warp); the grain
// region on the closed well and on the editor drags Position (its body) and the grain size (its right edge), one undo
// step per drag; the ratio chips add / remove / take a typed n/d (one step each, data only); the closed face's mini knobs
// are Position · Size · Density; the grain tap has no reader while the face is hidden.
//   goldens   sampler-face-grains (the closed face, grain dots), sampler-panel-gran (expanded), 1x and 2x

namespace {

// A still frame of a granulator: 14 grains around Position, levels and pans spread (fed through the synthetic reader).
std::vector<FaceGrain> stillGrains(double from) {
  std::vector<FaceGrain> g;
  for (uint32_t i = 0; i < 14; ++i) {
    const double t = static_cast<double>(i) / 13.0;
    g.push_back({static_cast<float>(from + 0.16 * t + 0.01 * std::sin(7.0 * t)),
                 static_cast<float>(0.15 + 0.85 * std::sin(std::numbers::pi * t)), static_cast<float>(std::sin(5.3 * t + 0.4)),
                 i % 3});
  }
  return g;
}

void granularState(doc::DocumentStore& st, doc::DeviceId engine) {
  setSetting(st, engine, "mode", 3.0);
  setValue(st, engine, "grainPosition", 0.35);
  setValue(st, engine, "grainSize", 0.1);
  setValue(st, engine, "sprayTime", 0.03);
}

// The closed face's mini knob bound to engine param `param` (readout none: not the Sample section's).
uint32_t miniKnob(const doc::Project& p, doc::DeviceId group, std::string_view param) {
  const doc::Device* g = doc::findDevice(p, group).device;
  REQUIRE(g != nullptr);
  for (const doc::PanelElement& e : g->panel->elements)
    if (e.kind == "knob" && e.text("readout", "") == "none" && !e.bindings.empty() && e.bindings.front().name == param) return e.id;
  FAIL("no mini knob " << param);
  return 0;
}

}  // namespace

TEST_CASE("render: sampler-face-grains / sampler-panel-gran (SM.13: Gran mode, the grain region and dots, 1x and 2x)") {
  for (const float scale : {1.0f, 2.0f}) {
    CAPTURE(scale);
    const std::string sfx = scale == 1.0f ? "" : "-2x";
    {
      Standalone s(scale, true, {{0, 0.0, 0.4}});
      granularState(s.ctx.store(), s.ids.engine);
      s.voices.setGrains(stillGrains(0.33));
      (void)s.face().sync();
      checkImage("sampler-face-grains" + sfx, s.render());
    }
    {
      Standalone s(scale, true, {{0, 0.0, 0.4}}, doc::kSamplerPanelW, 580.0f);
      doc::DocumentStore& st = s.ctx.store();
      granularState(st, s.ids.engine);
      setSetting(st, s.ids.engine, "harmonic", 1.0);
      setFloats(st, s.ids.engine, "harmonics", {1, 1, 3, 2, 5, 4, 2, 1, 7, 4, 3, 1});
      for (const char* title : {"Settings", "Granular"})
        st.view().panelWindows[s.ids.group.value].setFoldOpen(elementOf(*st.current(), s.ids.group, "fold", title)->id, true);
      s.voices.setGrains(stillGrains(0.33));
      (void)s.face().sync();
      checkImage("sampler-panel-gran" + sfx, s.render());
    }
    {  // GV.1: transposed up a fifth, Cloud, a Gauss window, pitch and pan spray, harmonic ratios, too few grains (skips)
      Standalone s(scale, true, {{0, 0.0, 0.4}}, doc::kSamplerPanelW, 580.0f);
      doc::DocumentStore& st = s.ctx.store();
      granularState(st, s.ids.engine);
      setSetting(st, s.ids.engine, "granMode", 2.0);
      setSetting(st, s.ids.engine, "shape", 2.0);
      setSetting(st, s.ids.engine, "grains", 4.0);
      setSetting(st, s.ids.engine, "harmonic", 1.0);
      setValue(st, s.ids.engine, "transpose", 7.0);
      setValue(st, s.ids.engine, "density", 60.0);
      setValue(st, s.ids.engine, "sprayPitch", 3.0);
      setValue(st, s.ids.engine, "sprayPan", 0.8);
      setValue(st, s.ids.engine, "scan", -0.25);
      for (const char* title : {"Settings", "Granular"})
        st.view().panelWindows[s.ids.group.value].setFoldOpen(elementOf(*st.current(), s.ids.group, "fold", title)->id, true);
      (void)s.face().sync();
      checkImage("sampler-panel-gran-stream" + sfx, s.render());
    }
  }
}

namespace {
// A click on mode-row cell k (Classic | One-Shot | Slice | Granular) through the window.
void clickMode(Expanded& u, int k) {
  panel::GroupPanelFace& f = u.face();
  const Rect tabs = f.element(static_cast<size_t>(f.indexOf(u.tabsId()))).rect();
  const Node& n = u.node();
  u.d.click(u.view().canvas().toDevice(
      {n.x + tabs.x0 + tabs.width() * ((static_cast<float>(k) + 0.5f) / 4.0f), n.y + (tabs.y0 + tabs.y1) * 0.5f}));
  u.d.step();
  REQUIRE(std::lround(engineOf(*u.ctx.doc(), u.ids.engine).setting("mode")->value) == k);
}
}  // namespace

TEST_CASE("sampler granular (SM.13, real dispatch): the mode row shows Granular in Gran mode only (Warp everywhere else)") {
  Expanded u;
  const uint32_t gran = u.idOf("fold", "Granular"), slices = u.idOf("fold", "Slices");
  // SM.17: "Warp" is the row beside the oval (the pill; in Slice mode the cells Off | Re-Pitch), no longer a section
  // that shows in every mode but Gran.
  const uint32_t warpPill = boundElement(*u.ctx.doc(), u.ids.group, "menu", "warp");
  const uint32_t warpCells = boundElement(*u.ctx.doc(), u.ids.group, "tabs", "warp");
  const uint32_t harm = u.idOf("sampleharm");
  for (const int k : {3, 0, 1, 2, 3}) {
    CAPTURE(k);
    clickMode(u, k);
    CHECK(u.shownEl(gran) == (k == 3));
    CHECK(u.shownEl(warpPill) == (k == 0 || k == 1));
    CHECK(u.shownEl(warpCells) == (k == 2));
    CHECK(u.shownEl(slices) == (k == 2));
  }
  // SM.17: open by default (the mode's own section): its controls and the ratio chips show; its header folds it (view
  // state, no undo step) and opens it again.
  const uint32_t gm = boundElement(*u.ctx.doc(), u.ids.group, "tabs", "granMode");  // (SM.17: cells, was a menu)
  CHECK(u.shownEl(harm));
  CHECK(u.shownEl(gm));
  const size_t h = u.cursor();
  u.d.click(u.at(gran, 0.1f, 0.5f));
  u.d.step();
  CHECK_FALSE(u.shownEl(harm));
  CHECK_FALSE(u.shownEl(gm));
  u.d.click(u.at(gran, 0.1f, 0.5f));
  u.d.step();
  CHECK(u.shownEl(harm));
  CHECK(u.cursor() == h);
}

TEST_CASE("sampler granular (SM.13, real dispatch): the closed face's knobs are Transpose · Filter · Volume, in Gran mode "
          "Position · Size · Density (the Position knob drags Position)") {
  Expanded u;
  const auto shownKnob = [&](std::string_view param) { return u.shownEl(miniKnob(*u.ctx.doc(), u.ids.group, param)); };
  const auto closedKnobs = [&](bool gran) {
    u.d.click(u.at(u.expandId()));  // closed: the oval
    u.d.step();
    REQUIRE(u.node().panelRound);
    for (const char* p : {"transpose", "gain"}) CHECK(shownKnob(p) == !gran);
    for (const char* p : {"grainPosition", "grainSize", "density"}) CHECK(shownKnob(p) == gran);
  };
  closedKnobs(false);
  u.d.click(u.at(u.expandId()));  // open again, Gran through the mode row
  u.d.step();
  clickMode(u, 3);
  closedKnobs(true);
  // Closed, the Position knob dragged up: Position rises (one undo step).
  const uint32_t pos = miniKnob(*u.ctx.doc(), u.ids.group, "grainPosition");
  const size_t h = u.cursor();
  const Vec2 c = u.at(pos);
  u.d.drag(c, {c.x, c.y - 60.0f}, 6);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpGrainPosition) > 0.1);
}

TEST_CASE("sampler granular (SM.13, real dispatch): the grain region drags Position (its body) and the grain size (its "
          "right edge) on the editor and on the closed well, one undo step per drag") {
  Expanded u;
  setSetting(u.ctx.store(), u.ids.engine, "mode", 3.0);
  u.d.step();
  REQUIRE(u.ctx.waveforms().overview(u.ids.sample) != nullptr);
  const double sizeU = 0.08 / 0.6;  // the default grain size over the sample's 0.6 s
  // The editor: the body by half the sample (Start … End is the whole: Position 0.5).
  size_t h = u.cursor();
  u.d.drag(u.ed(sizeU * 0.5, kWaveMidY), u.ed(sizeU * 0.5 + 0.5, kWaveMidY), 6);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpGrainPosition) == doctest::Approx(0.5).epsilon(0.02));
  // Its right edge from 0.5 + sizeU to 0.8: the grain size 0.3 × 0.6 s.
  h = u.cursor();
  u.d.drag(u.ed(0.5 + sizeU, kWaveMidY), u.ed(0.8, kWaveMidY), 6);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpGrainSize) == doctest::Approx(0.18).epsilon(0.03));
  // Undo takes the size drag back whole.
  REQUIRE(u.ctx.store().undo());
  CHECK(u.value(meta::kSmpGrainSize) == doctest::Approx(0.08));
  // The closed well (its lane: Start … End over face x 27 … 125): the body from Position 0.5 back to ≈ 0.25.
  u.d.click(u.at(u.expandId()));
  u.d.step();
  REQUIRE(u.node().panelRound);
  const auto well = [&](double v) {  // the well element (face x 24 … 128) at sample fraction v
    return u.at(2, (27.0f + static_cast<float>(v) * 98.0f - 24.0f) / 104.0f, 0.5f);
  };
  h = u.cursor();
  u.d.drag(well(0.5 + sizeU * 0.5), well(0.25 + sizeU * 0.5), 6);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpGrainPosition) == doctest::Approx(0.25).epsilon(0.04));
  CHECK(u.node().panelRound);  // (a drag never toggles the settings)
}

TEST_CASE("sampler granular (GV.1, real dispatch): the grain lens drags the grain size (sideways) and the transpose (up / "
          "down), the stream the density and the pitch spray, each drag one undo step on one axis; the shape cells pick the "
          "window; the Sample section gives way to the Granular one") {
  Expanded u;
  clickMode(u, 3);
  REQUIRE(u.ctx.waveforms().overview(u.ids.sample) != nullptr);
  CHECK_FALSE(u.shownEl(u.idOf("fold", "Sample")));
  const uint32_t lens = u.idOf("grainlens"), stream = u.idOf("grainstream");
  REQUIRE(u.shownEl(lens));
  REQUIRE(u.shownEl(stream));
  // Sideways right: a larger grain, the transpose untouched (the axis is the first one moved along).
  size_t h = u.cursor();
  const double size0 = u.value(meta::kSmpGrainSize);
  Vec2 c = u.at(lens, 0.5f, 0.55f);
  u.d.drag(c, {c.x + 80.0f, c.y + 6.0f}, 8);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpGrainSize) > size0 * 1.5);
  CHECK(u.value(meta::kSmpTranspose) == 0.0);
  // Up: whole semitones higher, the size as it was.
  h = u.cursor();
  const double size1 = u.value(meta::kSmpGrainSize);
  u.d.drag(c, {c.x + 3.0f, c.y - 40.0f}, 8);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpGrainSize) == size1);
  CHECK(u.value(meta::kSmpTranspose) >= 3.0);
  CHECK(u.value(meta::kSmpTranspose) == std::round(u.value(meta::kSmpTranspose)));
  // The Gauss cell (third of five at the lens's top right).
  u.d.click(u.at(lens, (60.0f + 2.0f * 15.5f + 7.0f) / 140.0f, 6.5f / 96.0f));
  u.d.step();
  CHECK(std::lround(engineOf(*u.ctx.doc(), u.ids.engine).setting("shape")->value) == 2);
  // The stream: sideways left a lower density; up more pitch spray.
  h = u.cursor();
  const double density0 = u.value(meta::kSmpDensity);
  c = u.at(stream, 0.5f, 0.4f);
  u.d.drag(c, {c.x - 80.0f, c.y}, 8);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpDensity) < density0 * 0.7);
  CHECK(u.value(meta::kSmpSprayPitch) == 0.0);
  h = u.cursor();
  u.d.drag(c, {c.x, c.y - 50.0f}, 8);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpSprayPitch) >= 3.0);
  REQUIRE(u.ctx.store().undo());  // (the drag back whole)
  CHECK(u.value(meta::kSmpSprayPitch) == 0.0);
  // Classic (one grain per period: no density): a sideways drag moves nothing but the pitch spray's axis.
  setSetting(u.ctx.store(), u.ids.engine, "granMode", 0.0);
  u.d.step();
  const double density1 = u.value(meta::kSmpDensity);
  u.d.drag(c, {c.x + 80.0f, c.y}, 8);
  u.d.step();
  CHECK(u.value(meta::kSmpDensity) == density1);
}

TEST_CASE("sampler granular (SM.13, real dispatch): ratio chips — + adds the next whole number, × removes, a click types "
          "n/d (one undo step each, the data's harmonics; integers only)") {
  Expanded u;
  setSetting(u.ctx.store(), u.ids.engine, "mode", 3.0);
  u.d.step();
  const uint32_t harm = u.idOf("sampleharm");
  REQUIRE(u.shownEl(harm));  // (SM.17: the Granular section is open by default)
  const auto local = [&](float lx, float ly) {
    panel::GroupPanelFace& f = u.face();
    const Rect r = f.element(static_cast<size_t>(f.indexOf(harm))).rect();
    const Node& n = u.node();
    return u.view().canvas().toDevice({n.x + r.x0 + lx, n.y + r.y0 + ly});
  };
  const auto chipX = [](size_t i) { return 40.0f + static_cast<float>(i % 9) * 30.0f; };
  const auto chipY = [](size_t i) { return 6.0f + (i >= 9 ? 12.0f : 0.0f); };
  const auto ratios = [&] {
    const meta::DataPtr d = u.data();
    const meta::DataValue* x = d != nullptr ? d->field("harmonics") : nullptr;
    return x != nullptr ? x->floats : std::vector<float>{};
  };
  CHECK(ratios().empty());  // the default list (1, 2, 3/2, 3, 4, 5) until edited
  // +: after the default six, 6.
  size_t h = u.cursor();
  u.d.click(local(chipX(6) + 14.0f, chipY(6)));
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(ratios() == std::vector<float>{1, 1, 2, 1, 3, 2, 3, 1, 4, 1, 5, 1, 6, 1});
  // × of the second chip (2).
  h = u.cursor();
  u.d.click(local(chipX(1) + 24.0f, chipY(1)));
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(ratios() == std::vector<float>{1, 1, 3, 2, 3, 1, 4, 1, 5, 1, 6, 1});
  // The first chip typed 7/4.
  h = u.cursor();
  u.d.click(local(chipX(0) + 8.0f, chipY(0)));
  u.d.type("7/4");
  (void)u.d.keyDown(ui::key::kReturn);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(ratios() == std::vector<float>{7, 4, 3, 2, 3, 1, 4, 1, 5, 1, 6, 1});
  // Not whole numbers in range: nothing changes, no undo step.
  h = u.cursor();
  for (const char* bad : {"0", "3/", "70000"}) {
    u.d.click(local(chipX(2) + 8.0f, chipY(2)));
    u.d.type(bad);
    (void)u.d.keyDown(ui::key::kReturn);
    u.d.step();
  }
  CHECK(ratios() == std::vector<float>{7, 4, 3, 2, 3, 1, 4, 1, 5, 1, 6, 1});
  CHECK(u.cursor() == h);
}

TEST_CASE("sampler granular (SM.13): the grain tap is read while the face shows, and has no reader while it is hidden") {
  Ui u;
  setSetting(u.ctx.store(), u.ids.engine, "mode", 3.0);
  u.granular = true;  // (the link would give the Granular instance its tap now)
  u.frame();
  (void)u.reader->serial();
  REQUIRE(u.reader->grainTap() == u.grainTap.get());
  CHECK(u.grainTap->wanted());
  // A record of three grains: the reader takes them at its next read (the face's poll; the app wakes it through
  // FaceVoiceHub::anyFresh, which this test has not: it reads as the face would).
  pq::GrainTapRecord* r = u.grainTap->begin(0);
  REQUIRE(r != nullptr);
  for (uint32_t i = 0; i < 3; ++i) r->grain[i] = {0.2f + 0.1f * static_cast<float>(i), 0.8f, 0.0f, i};
  u.grainTap->commit(0, 3);
  CHECK(u.reader->fresh());
  std::array<FaceVoice, 4> voices{};
  (void)u.reader->read(voices);
  std::array<FaceGrain, 8> got{};
  CHECK(u.reader->grains(got) == 3);
  CHECK(got[1].position == doctest::Approx(0.3f));
  // Scrolled away: no reader of the grain tap (the kernel's write is one relaxed load), the grains gone.
  u.frame(6000.0f);
  CHECK(u.reader->hidden());
  CHECK_FALSE(u.grainTap->wanted());
  CHECK(u.reader->grains(got) == 0);
  CHECK(u.reader->grainTap() == nullptr);
  // Back on screen: read again.
  u.frame();
  (void)u.reader->serial();
  CHECK(u.grainTap->wanted());
}

// ---- SM.17: the panel pass — radial choices, the Warp row beside the oval, the conditional Warp section, the editor ----
// (docs/plans/SM.md "SM.17"): the warp mode is a radial menu on a pill beside the oval (the generic `menu` element, look
// "radial"): a click opens it centred on the pill, the current mode marked; a pick by pointer, by keys or by a typed
// filter is one undo step; the wheel over the pill steps it (a run of notches one step). In Slice mode the row is the
// cells Off | Re-Pitch (a stored stretch mode shows as Off and is kept until a cell is clicked). The Warp section exists
// only while Beats / Tones / Texture / Complex Pro is chosen, with that mode's controls. A press anywhere on the
// waveform editor is the editor's: the block never moves.
//   goldens   sampler-panel-warp (Texture: the Warp section open), sampler-panel-radial (the menu open on the pill)

namespace {

// The expanded Sampler in the patch view, with the radial menu's helpers (the overlay the canvas opens it in).
struct WarpUi : Expanded {
  uint32_t pill() const { return boundElement(*ctx.doc(), ids.group, "menu", "warp"); }
  long warp() const {
    const doc::DeviceValue* v = engineOf(*ctx.doc(), ids.engine).setting("warp");
    return v != nullptr ? std::lround(v->value) : 0;
  }
  RadialMenu* menu() const {
    OverlayLayer* o = OverlayLayer::of(view().canvas());
    return o != nullptr ? dynamic_cast<RadialMenu*>(o->top()) : nullptr;
  }
  RadialMenu& open() {
    d.click(at(pill()));
    RadialMenu* m = menu();
    REQUIRE(m != nullptr);
    d.advance(0.2);  // (past the open animation)
    return *m;
  }
  Vec2 bubble(std::string_view label) {
    RadialMenu* m = menu();
    REQUIRE(m != nullptr);
    const int32_t i = m->find(label);
    REQUIRE_MESSAGE(i >= 0, label);
    REQUIRE_MESSAGE(m->shown(i), label);
    return m->centreOf(i);
  }
};

}  // namespace

TEST_CASE("sampler warp (SM.17, real dispatch): a click on the Warp pill opens the radial menu centred on it, the current "
          "mode marked; a click on a mode picks it — one undo step, undo restores") {
  WarpUi u;
  REQUIRE(u.shownEl(u.pill()));
  REQUIRE(u.warp() == 0);
  const size_t h0 = u.cursor();
  RadialMenu& m = u.open();
  CHECK(u.cursor() == h0);  // opening is no edit
  CHECK(m.itemCount() == 7);  // the seven modes, one ring
  for (const char* mode : {"Off", "Re-Pitch", "Beats", "Tones", "Texture", "Complex", "Complex Pro"}) {
    CAPTURE(mode);
    const int32_t i = m.find(mode);
    REQUIRE(i >= 0);
    CHECK(m.shown(i));
  }
  CHECK(m.highlighted() == m.find("Off"));  // the current mode
  const Vec2 pill = u.at(u.pill());
  CHECK(std::fabs(m.centre().x - pill.x) < 2.0f);  // centred on the pill
  CHECK(std::fabs(m.centre().y - pill.y) < 2.0f);
  const Vec2 beats = u.bubble("Beats");
  u.d.move(beats);
  u.d.click(beats);
  u.d.step();
  CHECK(u.menu() == nullptr);
  CHECK(u.warp() == 2);
  CHECK(u.cursor() == h0 + 1);
  REQUIRE(u.ctx.store().undo());
  u.d.step();
  CHECK(u.warp() == 0);
  CHECK(u.cursor() == h0);
  // A press outside every bubble closes it without an edit.
  (void)u.open();
  u.d.click({5.0f, 5.0f});
  u.d.step();
  CHECK(u.menu() == nullptr);
  CHECK(u.cursor() == h0);
}

TEST_CASE("sampler warp (SM.17, real dispatch): the radial menu by keys (Down round the ring, Enter picks) and by a typed "
          "filter — one undo step each; Esc closes without an edit") {
  WarpUi u;
  const size_t h0 = u.cursor();
  // Keys: from Off three steps clockwise is Tones.
  (void)u.open();
  u.d.press("Down");
  u.d.press("Down");
  u.d.press("Down");
  u.d.press("Enter");
  u.d.step();
  CHECK(u.menu() == nullptr);
  CHECK(u.warp() == 3);
  CHECK(u.cursor() == h0 + 1);
  // Typed: "tex" leaves Texture alone; Enter picks it. (The panel was rebuilt by the pick: the pill answers again.)
  RadialMenu& m = u.open();
  CHECK(m.highlighted() == m.find("Tones"));
  u.d.type("tex");
  REQUIRE(m.resultCount() == 1);
  CHECK(m.labelOf(m.result(0)) == "Texture");
  u.d.press("Enter");
  u.d.step();
  CHECK(u.warp() == 4);
  CHECK(u.cursor() == h0 + 2);
  // "complex" matches two: the exact one first.
  RadialMenu& m2 = u.open();
  u.d.type("complex");
  REQUIRE(m2.resultCount() == 2);
  CHECK(m2.labelOf(m2.result(0)) == "Complex");
  u.d.press("Down");
  u.d.press("Enter");
  u.d.step();
  CHECK(u.warp() == 6);
  CHECK(u.cursor() == h0 + 3);
  // Esc: closed, nothing changed.
  (void)u.open();
  u.d.press("Escape");
  u.d.step();
  CHECK(u.menu() == nullptr);
  CHECK(u.warp() == 6);
  CHECK(u.cursor() == h0 + 3);
}

TEST_CASE("sampler warp (SM.17, real dispatch): the wheel over the Warp pill steps the mode; a run of notches is one undo "
          "step") {
  Standalone s(1.0f, true, {}, doc::kSamplerPanelW, 400.0f);
  doc::DocumentStore& st = s.ctx.store();
  st.view().panelWindows[s.ids.group.value].setFoldOpen(elementOf(*st.current(), s.ids.group, "fold", "Settings")->id, true);
  (void)s.face().sync();
  s.d.tree().layoutNow();
  const uint32_t pill = boundElement(*st.current(), s.ids.group, "menu", "warp");
  const int32_t i = s.face().indexOf(pill);
  REQUIRE(i >= 0);
  REQUIRE(s.face().shown(static_cast<size_t>(i)));
  const Rect r = s.face().element(static_cast<size_t>(i)).rect();
  const Vec2 c = s.host->transform().map({(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f});
  const auto warp = [&] {
    const doc::DeviceValue* v = engineOf(*st.current(), s.ids.engine).setting("warp");
    return v != nullptr ? std::lround(v->value) : 0l;
  };
  const size_t h0 = st.history().cursor();
  s.d.move(c);
  s.d.wheel(c, 1.0f);
  s.d.wheel(c, 1.0f);
  s.d.advance(panel::PanelHost::kWheelRunSeconds + 0.1);
  CHECK(warp() == 2);  // Off → Re-Pitch → Beats
  CHECK(st.history().cursor() == h0 + 1);
  s.d.wheel(c, -1.0f);
  s.d.advance(panel::PanelHost::kWheelRunSeconds + 0.1);
  CHECK(warp() == 1);
  CHECK(st.history().cursor() == h0 + 2);
  REQUIRE(st.undo());
  CHECK(warp() == 2);
}

TEST_CASE("sampler warp (SM.17, real dispatch): the Warp section exists only for a stretch mode that has controls, open, "
          "with that mode's controls; Sample BPM shows while the sample warps") {
  WarpUi u;
  const uint32_t fold = u.idOf("fold", "Warp");
  const doc::Project& p0 = *u.ctx.doc();
  const uint32_t bpm = boundElement(p0, u.ids.group, "number", "sampleBpm");
  const uint32_t envelope = boundElement(p0, u.ids.group, "knob", "envelope"), beatsLoop = boundElement(p0, u.ids.group, "tabs", "beatsLoop");
  const uint32_t grain = boundElement(p0, u.ids.group, "knob", "grain"), flux = boundElement(p0, u.ids.group, "knob", "flux");
  const uint32_t formants = boundElement(p0, u.ids.group, "knob", "formants"), order = boundElement(p0, u.ids.group, "knob", "formantOrder");
  const uint32_t loopFade = boundElement(p0, u.ids.group, "knob", "loopFade");
  struct Want {
    const char* mode;
    bool section, bpm, envelope, grain, flux, formants;
  };
  float hOff = 0.0f;
  for (const Want& w : {Want{"Off", false, false, false, false, false, false}, Want{"Re-Pitch", false, true, false, false, false, false},
                        Want{"Beats", true, true, true, false, false, false}, Want{"Tones", true, true, false, true, false, false},
                        Want{"Texture", true, true, false, true, true, false}, Want{"Complex", false, true, false, false, false, false},
                        Want{"Complex Pro", true, true, false, false, false, true}, Want{"Off", false, false, false, false, false, false}}) {
    CAPTURE(w.mode);
    if (std::string_view(w.mode) != "Off" || u.warp() != 0) {  // picked through the menu
      (void)u.open();
      const Vec2 b = u.bubble(w.mode);
      u.d.move(b);
      u.d.click(b);
      u.d.step();
    }
    CHECK(u.shownEl(fold) == w.section);
    CHECK(u.shownEl(bpm) == w.bpm);
    CHECK(u.shownEl(envelope) == w.envelope);  // (shown: the section is open by default)
    CHECK(u.shownEl(beatsLoop) == w.envelope);
    CHECK(u.shownEl(grain) == w.grain);
    CHECK(u.shownEl(flux) == w.flux);
    CHECK(u.shownEl(formants) == w.formants);
    CHECK(u.shownEl(order) == w.formants);
    CHECK(u.shownEl(loopFade) == (u.warp() <= 1));  // (Classic loops Forward by default; a stretch mode has no loop crossfade)
    if (hOff == 0.0f) hOff = u.node().h;
    CHECK((u.node().h > hOff + 40.0f) == w.section);  // the block grows by the section, and gives it back
  }
}

TEST_CASE("sampler warp (SM.17, real dispatch): Slice mode offers only Off | Re-Pitch (cells); a stored stretch mode shows "
          "as Off there and is kept until a cell is clicked; Granular has no Warp row") {
  WarpUi u;
  setSetting(u.ctx.store(), u.ids.engine, "warp", 4.0);  // Texture, in Classic
  u.d.step();
  const uint32_t pill = u.pill(), cells = boundElement(*u.ctx.doc(), u.ids.group, "tabs", "warp");
  const uint32_t fold = u.idOf("fold", "Warp");
  const uint32_t sliceBpm = boundElement(*u.ctx.doc(), u.ids.group, "number", "sampleBpm", 1);  // (Slice's BPM box)
  CHECK(u.shownEl(pill));
  CHECK_FALSE(u.shownEl(cells));
  CHECK(u.shownEl(fold));
  clickMode(u, 2);  // Slice
  CHECK_FALSE(u.shownEl(pill));
  CHECK(u.shownEl(cells));
  CHECK_FALSE(u.shownEl(fold));  // no stretch controls in Slice
  CHECK_FALSE(u.shownEl(sliceBpm));
  {
    panel::GroupPanelFace& f = u.face();
    const panel::Element& e = f.element(static_cast<size_t>(f.indexOf(cells)));
    CHECK(e.desc().text("items", "") == "Off|Re-Pitch");
    CHECK(e.selection(f.target()) == std::optional<double>(0.0));  // Texture plays unwarped in Slice: shown as Off
  }
  CHECK(u.warp() == 4);  // kept
  clickMode(u, 0);
  CHECK(u.shownEl(fold));  // Classic again: Texture is back
  clickMode(u, 2);
  // The Re-Pitch cell (one undo step), then Off.
  const size_t h = u.cursor();
  u.d.click(u.at(cells, 0.75f, 0.5f));
  u.d.step();
  CHECK(u.warp() == 1);
  CHECK(u.cursor() == h + 1);
  CHECK(u.shownEl(sliceBpm));
  u.d.click(u.at(cells, 0.25f, 0.5f));
  u.d.step();
  CHECK(u.warp() == 0);
  CHECK(u.cursor() == h + 2);
  CHECK_FALSE(u.shownEl(sliceBpm));
  clickMode(u, 3);  // Granular: no Warp row at all
  CHECK_FALSE(u.shownEl(pill));
  CHECK_FALSE(u.shownEl(cells));
  CHECK_FALSE(u.shownEl(fold));
}

TEST_CASE("sampler editor (SM.17, real dispatch): a press or drag on the waveform editor never moves the block (empty "
          "waveform, ruler, flag strip), and a hovered handle says what it is") {
  Expanded u;
  const Node n0 = u.node();
  const size_t h0 = u.cursor();
  for (const float y : {kWaveMidY, kRulerY, kFlagY, 116.0f}) {  // the waveform, the ruler (warp Off), the flag strip, the info line
    CAPTURE(y);
    const Vec2 to = u.ed(0.7, y);
    u.d.drag(u.ed(0.5, y), {to.x, to.y + 30.0f}, 6);
    u.d.step();
    CHECK(u.node().x == doctest::Approx(n0.x));
    CHECK(u.node().y == doctest::Approx(n0.y));
  }
  CHECK(u.cursor() == h0);  // and nothing was edited
  // WF.1: the drag on the ruler scrolled and zoomed the editor's view (view state); a double-click on the strip under
  // the waveform shows the whole sample again (the hovers below are placed on the whole sample).
  u.d.doubleClick(u.ed(0.5, kFlagY));
  u.d.step();
  CHECK(u.cursor() == h0);
  // Hover: the handles name themselves (the info view's text), with a resize cursor; the empty waveform an arrow.
  const size_t ed = static_cast<size_t>(u.face().indexOf(u.idOf("sampleedit")));
  const auto hover = [&](Vec2 at) {
    u.d.move(at);
    u.d.step();
    std::string title, body;
    panel::GroupPanelFace& f = u.face();
    f.element(ed).describe(f.target(), title, body);
    return title;
  };
  const auto cursor = [&] { return u.face().element(ed).cursor(); };
  CHECK(hover(u.ed(0.0, kWaveMidY)) == "Sample Start");
  CHECK(cursor() == Cursor::ResizeEW);
  CHECK(hover(u.ed(1.0, kFlagY)) == "Sample End");
  CHECK(hover(u.ed(1.0, kLoopBarY)) == "Loop End");
  CHECK(hover(u.ed(0.5, kLoopBarY)) == "Loop");
  CHECK(cursor() == Cursor::Move);
  CHECK(hover(u.ed(0.5, kWaveMidY)) == "Sample Editor");
  CHECK(cursor() == Cursor::Arrow);
}

TEST_CASE("sampler panel (SM.17): no drop-down list is left — every choice is cells (2 – 3 values) or a radial menu; the "
          "closed face names its knobs") {
  Ui u;
  const doc::Device* g = doc::findDevice(*u.ctx.doc(), u.ids.group).device;
  REQUIRE(g != nullptr);
  size_t radial = 0, names = 0;
  for (const doc::PanelElement& e : g->panel->elements) {
    if (e.kind == "menu") {
      CHECK(e.text("look", "list") == "radial");
      ++radial;
    }
    if (e.kind == "label")
      for (const char* n : {"Transpose", "Filter", "Volume", "Position", "Size", "Density"})
        if (e.text("text", "") == n && e.y < doc::kSamplerFaceH) ++names;
  }
  CHECK(radial == 1);  // Warp (GV.1: the grain Shape is five window glyphs in the grain lens, one click each)
  CHECK(names == 6);
  for (const char* setting : {"loop", "trigger", "beatsLoop", "granMode"}) CHECK(boundElement(*u.ctx.doc(), u.ids.group, "tabs", setting) != 0);
}

TEST_CASE("render: sampler-panel-warp / sampler-panel-radial (SM.17: a stretch mode's Warp section; the radial menu open "
          "on the Warp pill, 1x and 2x)") {
  for (const float scale : {1.0f, 2.0f}) {
    CAPTURE(scale);
    const std::string sfx = scale == 1.0f ? "" : "-2x";
    {
      Standalone s(scale, true, {{0, 0.0, 0.3}}, doc::kSamplerPanelW, 420.0f);
      doc::DocumentStore& st = s.ctx.store();
      st.view().panelWindows[s.ids.group.value].setFoldOpen(elementOf(*st.current(), s.ids.group, "fold", "Settings")->id, true);
      setSetting(st, s.ids.engine, "mode", 1.0);
      setSetting(st, s.ids.engine, "warp", 4.0);  // Texture: Grain Size and Flux
      (void)s.face().sync();
      checkImage("sampler-panel-warp" + sfx, s.render());
    }
  }
  // The menu, opened by a click on the pill in the patch view (the canvas's overlay); the image cropped to the menu.
  WarpUi u;
  setSetting(u.ctx.store(), u.ids.engine, "warp", 2.0);  // Beats: opened to and marked
  u.d.step();
  RadialMenu& m = u.open();
  u.d.move(u.bubble("Texture"));  // (hovered: its name tag shows)
  u.d.advance(0.3);
  auto img = u.d.render();
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const IRect b = m.bounds();
  const int32_t x0 = std::max(0, b.x - 60), y0 = std::max(0, b.y - 12);
  const int32_t x1 = std::min<int32_t>(static_cast<int32_t>(img->width), b.x + b.w + 60), y1 = std::min<int32_t>(static_cast<int32_t>(img->height), b.y + b.h + 12);
  Image cut;
  cut.width = static_cast<uint32_t>(x1 - x0);
  cut.height = static_cast<uint32_t>(y1 - y0);
  cut.rgba.resize(static_cast<size_t>(cut.width) * cut.height * 4);
  for (uint32_t y = 0; y < cut.height; ++y)
    std::copy_n(img->pixel(static_cast<uint32_t>(x0), static_cast<uint32_t>(y0) + y), static_cast<size_t>(cut.width) * 4, cut.pixel(0, y));
  checkImage("sampler-panel-radial", cut);
}

// ---- WF.1: the Sampler's waveform through the shared renderer; the editor's view ---------------------------------------

namespace {

// The engine's face behind the panel's `sampleedit` element (the one the well shares), as it last painted.
SamplerFaceWave waveOf(const Expanded& u, std::string_view kind = "sampleedit") {
  panel::GroupPanelFace& f = u.face();
  const int32_t i = f.indexOf(u.idOf(kind));
  REQUIRE(i >= 0);
  const doc::Binding* b = f.element(static_cast<size_t>(i)).desc().binding(0);
  REQUIRE(b != nullptr);
  const std::shared_ptr<MetaFace> face = f.target().engineFace(*b, "", nullptr);
  REQUIRE(face != nullptr);
  SamplerFaceWave w;
  REQUIRE(samplerFaceWave(*face, w));
  return w;
}

struct QueuePoster {
  std::mutex m;
  std::vector<std::function<void()>> q;
  ViewContext::UiPoster poster() {
    return [this](std::function<void()> f) {
      std::lock_guard<std::mutex> l(m);
      q.push_back(std::move(f));
    };
  }
  void pump() {
    std::vector<std::function<void()>> run;
    {
      std::lock_guard<std::mutex> l(m);
      run.swap(q);
    }
    for (auto& f : run) f();
  }
};

constexpr double kPluckFrames = 28800.0;
// Device px per sample of the editor's view (316 face units wide at zoom 1, scale 1).
double pxPerSample(const SamplerFaceWave& w) { return 316.0 / ((w.viewB - w.viewA) * kPluckFrames); }

}  // namespace

TEST_CASE("sampler waveform (WF.1): a short sample at fit zoom is drawn from its samples, never the overview stretched — "
          "stretched only until the samples arrive, then repainted") {
  Expanded u;
  // 0.6 s (28,800 frames) across the well (98 px in the panel) and the editor (316 px): 294 and 91 frames per px, both below the
  // overview's resolution (512 frames per px: two 256-frame buckets).
  SamplerFaceWave w = waveOf(u);
  const SamplerFaceWave well = waveOf(u, "samplewave");
  CHECK(well.well.columns > 80);
  CHECK(well.well.level == -1);
  CHECK_FALSE(well.well.approx);
  CHECK(w.editor.columns > 300);
  CHECK(w.editor.level == -1);
  CHECK_FALSE(w.editor.approx);
  CHECK(w.viewA == 0.0);
  CHECK(w.viewB == 1.0);
  // As the app reads them (a poster: overviews and sample windows come from the background workers): the first paint
  // after the overview has no samples yet (the overview, stretched: approx), the arrival repaints the faces, and the
  // settled picture is the samples. A settled view reads nothing more.
  QueuePoster qp;
  u.ctx.setUiPoster(qp.poster());
  u.ctx.waveforms().clear();
  u.d.tree().invalidateAll();
  u.d.step();  // (asks for the overview)
  bool sawApprox = false;
  for (int i = 0; i < 8; ++i) {
    u.ctx.waveforms().waitIdle();
    qp.pump();
    u.d.step();
    w = waveOf(u);
    sawApprox = sawApprox || (w.editor.columns > 0 && w.editor.approx);
  }
  CHECK(sawApprox);
  CHECK(w.editor.level == -1);
  CHECK_FALSE(w.editor.approx);
  CHECK(waveOf(u, "samplewave").well.level == -1);
  CHECK_FALSE(waveOf(u, "samplewave").well.approx);
  const uint64_t reads = u.ctx.waveforms().windowReads();
  u.d.tree().invalidateAll();
  u.d.step();
  u.ctx.waveforms().waitIdle();
  qp.pump();
  CHECK(u.ctx.waveforms().windowReads() == reads);
  u.ctx.setUiPoster({});
}

TEST_CASE("sampler editor (WF.1, real dispatch): the wheel zooms about the pointer down to single samples (the curve, a "
          "dot per sample); Shift + wheel and a ruler drag scroll; a double-click on the ruler shows the whole sample; "
          "nothing is an undo step") {
  Expanded u;
  const size_t h0 = u.cursor();
  const Node n0 = u.node();
  // The wheel over the waveform at a quarter of the editor: zooms in about that point, down to its limit.
  for (int i = 0; i < 6; ++i) CHECK(u.d.wheel(u.ed(0.25, kWaveMidY), 1.0f));
  u.d.step();
  SamplerFaceWave w = waveOf(u);
  CHECK(w.viewB - w.viewA == doctest::Approx(std::pow(0.8, 6.0)).epsilon(1e-6));
  CHECK(w.viewA + 0.25 * (w.viewB - w.viewA) == doctest::Approx(0.25).epsilon(1e-6));  // the frame under the pointer stays
  for (int i = 0; i < 60; ++i) (void)u.d.wheel(u.ed(0.25, kWaveMidY), 1.0f);
  u.d.step();
  w = waveOf(u);
  CHECK(pxPerSample(w) >= 32.0);   // to the samples
  CHECK(pxPerSample(w) <= 64.01);  // (the limit: 64 device px per sample)
  CHECK(w.viewA + 0.25 * (w.viewB - w.viewA) == doctest::Approx(0.25).epsilon(1e-4));
  CHECK(w.editor.level == -1);
  CHECK(w.editor.curve);
  CHECK_FALSE(w.editor.approx);
  CHECK(w.editor.dots >= 4);
  CHECK(waveOf(u, "samplewave").well.columns > 80);  // the closed well stays the fit view
  // Wheel down zooms out again about the pointer; Shift + wheel scrolls (up: left), the span kept.
  for (int i = 0; i < 20; ++i) (void)u.d.wheel(u.ed(0.25, kWaveMidY), -1.0f);
  u.d.step();
  w = waveOf(u);
  const double span = w.viewB - w.viewA;
  CHECK(span > 0.01);
  CHECK(span < 0.2);
  CHECK(u.d.wheel(u.ed(0.5, kWaveMidY), -1.0f, 0.0f, mod::kShift));
  u.d.step();
  SamplerFaceWave s = waveOf(u);
  CHECK(s.viewA == doctest::Approx(w.viewA + span * 0.125).epsilon(1e-6));
  CHECK(s.viewB - s.viewA == doctest::Approx(span).epsilon(1e-9));
  // A drag along the ruler (warp Off: no markers) scrolls: the sample follows the pointer.
  u.d.drag(u.ed(0.6, kRulerY), u.ed(0.4, kRulerY), 5);
  u.d.step();
  w = waveOf(u);
  CHECK(w.viewA == doctest::Approx(s.viewA + 0.2 * span).epsilon(1e-3));
  CHECK(w.viewB - w.viewA == doctest::Approx(span).epsilon(1e-6));
  // Up / down on the ruler zooms (down: in).
  const Vec2 from = u.ed(0.5, kRulerY);
  u.d.drag(from, {from.x, from.y + 60.0f}, 5);
  u.d.step();
  CHECK(waveOf(u).viewB - waveOf(u).viewA < span * 0.7);
  // A double-click on the ruler: the whole sample.
  u.d.doubleClick(u.ed(0.5, kRulerY));
  u.d.step();
  w = waveOf(u);
  CHECK(w.viewA == 0.0);
  CHECK(w.viewB == 1.0);
  // View state only: no undo step, the block where it was.
  CHECK(u.cursor() == h0);
  CHECK(u.node().x == doctest::Approx(n0.x));
  CHECK(u.node().y == doctest::Approx(n0.y));
}

TEST_CASE("sampler editor (WF.1, real dispatch): handles drag while zoomed — a flag out of view waits at the edge, a drag "
          "lands where the view shows it, and one held past the edge scrolls the view (one undo step each)") {
  Expanded u;
  for (int i = 0; i < 10; ++i) (void)u.d.wheel(u.ed(0.5, kWaveMidY), 1.0f);
  u.d.step();
  SamplerFaceWave w = waveOf(u);
  const double span = w.viewB - w.viewA;
  REQUIRE(span < 0.12);
  REQUIRE(w.viewA > 0.4);  // Start (0) and End (1) are out of view
  // The Start flag waits at the left edge: dragged to the editor's middle, Start is the sample position shown there.
  size_t h = u.cursor();
  u.d.drag(u.ed(0.0, kFlagY), u.ed(0.5, kFlagY), 6);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpStart) == doctest::Approx(w.viewA + 0.5 * span).epsilon(2e-3));
  // The End flag from the right edge to three quarters.
  h = u.cursor();
  u.d.drag(u.ed(1.0, kFlagY), u.ed(0.75, kFlagY), 6);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpEnd) == doctest::Approx(w.viewA + 0.75 * span).epsilon(2e-3));
  // Start's line (now in view, at the middle) dragged within the view: sample-fine (a pixel is span / 316 of the sample).
  h = u.cursor();
  u.d.drag(u.ed(0.5, kWaveMidY), u.ed(0.3, kWaveMidY), 6);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  CHECK(u.value(meta::kSmpStart) == doctest::Approx(w.viewA + 0.3 * span).epsilon(2e-3));
  CHECK(waveOf(u).viewA == doctest::Approx(w.viewA));  // (inside the view nothing scrolls)
  // Held past the left edge, the view scrolls with every move and Start follows the edge.
  h = u.cursor();
  const Vec2 a = u.ed(0.3, kWaveMidY), out = u.ed(-0.08, kWaveMidY);
  u.d.move(a);
  u.d.down(a);
  for (int i = 1; i <= 12; ++i) u.d.move({out.x - static_cast<float>(i % 2), out.y});
  u.d.up(out);
  u.d.step();
  CHECK(u.cursor() == h + 1);
  const SamplerFaceWave s = waveOf(u);
  CHECK(s.viewA < w.viewA - 0.5 * span);  // scrolled left by more than half a view
  CHECK(s.viewB - s.viewA == doctest::Approx(span).epsilon(1e-9));
  CHECK(u.value(meta::kSmpStart) == doctest::Approx(s.viewA).epsilon(1e-6));
  // Undo: Start back at 0.3 of the old view; the view (view state) stays.
  REQUIRE(u.ctx.store().undo());
  CHECK(u.value(meta::kSmpStart) == doctest::Approx(w.viewA + 0.3 * span).epsilon(2e-3));
  u.d.step();
  CHECK(waveOf(u).viewA == doctest::Approx(s.viewA));
}
