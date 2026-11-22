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

#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
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
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/meta/face_targets.h"
#include "pq/views/meta/tap_voices.h"
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
  std::unique_ptr<TapVoices> reader;
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  SamplerProject ids;
  Ui() {
    ctx.setDeviceLibrary(&devLib());
    ids = buildSamplerProject(ctx.store(), true);
    reader = std::make_unique<TapVoices>([t = tap] { return t; });
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
  for (const size_t i : {id("tabs"), id("sampleroot"), id("sampleedit"), id("fold", "Sample"), id("fold", "Warp"),
                         id("fold", "Envelope"), id("fold", "Filter"), id("fold", "LFO"), id("knob")})
    CHECK(u.face().shown(i));
  CHECK_FALSE(u.face().shown(id("fold", "Slices")));  // Classic: no Slices section
  CHECK(u.foldState(elementOf(*u.ctx.doc(), u.ids.group, "fold", "Warp")->id) == std::nullopt);  // folded by default
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
  CHECK(u.node().h > hClassic);  // the block grew by the Slices header (its section folded)
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
      Standalone s(scale, true, {{0, 0.0, 0.3}}, doc::kSamplerPanelW, 400.0f);
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
  const uint32_t fold = u.idOf("fold", "Slices");
  REQUIRE(u.shownEl(fold));
  u.d.click(u.at(fold, 0.1f, 0.5f));  // open the Slices section (view state)
  u.d.step();
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
