// SM.10 (docs/plans/SM.md "Face (👁)" Closed): the Sampler group's CLOSED FACE — its panel's oval (152 × 84: the block
// is that oval), the engine face's `samplewave` section (the sample's waveform from Start to End, a glowing playhead per
// sounding voice), the mini knobs Transpose · Filter · Volume and the Expand chevron (a fold: view state, never an undo
// step; open, the SM.3 settings show under the oval and the block is a rectangle again).
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
#include "pq/document/modules.h"
#include "pq/media/wav_file.h"
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
  Standalone(float scale, bool withSample, std::vector<SyntheticVoices::Voice> sounding)
      : d(static_cast<uint32_t>(doc::kSamplerFaceW * scale), static_cast<uint32_t>(doc::kSamplerFaceH * scale), scale) {
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
