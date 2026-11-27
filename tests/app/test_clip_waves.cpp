// SX.4: the arrangement's rendered waveforms through the app's render (pq/app/clip_waves.h): renderClip without the
// envelope == the clip rendered with no gain / fades / declick (the same clip player, no second warp implementation),
// a deactivated clip renders the same, the views' cached wave holds exactly those frames; the golden
// arrangement-audio-rendered (a Beats clip +7 st beside its Off copy); (perf, release) the render time of a 4-min Beats and
// a 4-min Complex Pro clip and the UI-thread cost of the swap.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/app/clip_waves.h"
#include "pq/app/project_link.h"
#include "pq/document/edit_ops.h"
#include "pq/media/wav_file.h"
#include "pq/ui/image.h"
#include "pq/views/fake_transport.h"
#include "pq/views/view_context.h"
#include "pq/views/view_scenes.h"
#include "pq/views/waveforms.h"

using namespace pq;
using namespace pq::doc;
using pq::app::ProjectLink;
namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// A context with one audio track playing a drum-like file (a decaying noise hit every quarter second).
struct Rig {
  views::FakeTransport transport;
  views::ViewContext ctx{transport};
  fs::path dir;
  std::string file;
  TrackId track;
  ClipRef clip;
  Rig(const char* name, double seconds) : dir(fs::temp_directory_path() / name) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const auto frames = static_cast<int64_t>(seconds * 48000.0);
    std::vector<std::vector<float>> ch(2, std::vector<float>(static_cast<size_t>(frames)));
    uint32_t rng = 7;
    for (int64_t i = 0; i < frames; ++i) {
      rng = rng * 1664525u + 1013904223u;
      const double env = std::exp(-static_cast<double>(i % 12000) / 1500.0);
      const double x = env * (0.6 * (static_cast<double>(rng >> 8) / 8388608.0 - 1.0) + 0.3 * std::sin(i * 0.02));
      ch[0][static_cast<size_t>(i)] = static_cast<float>(x);
      ch[1][static_cast<size_t>(i)] = static_cast<float>(0.8 * x);
    }
    const fs::path p = dir / "hits.wav";
    REQUIRE(media::writeWav(p, ch, 48000).ok());
    file = utf8(p);
    DocumentStore& st = ctx.store();
    auto t = ops::createTrack(*st.current(), st.ids(), TrackKind::Audio);
    REQUIRE(t.ok());
    REQUIRE(st.dispatch(t->command).ok());
    track = t->tracks.at(0);
    SampleRef s;
    s.id = st.ids().sample();
    s.path = file;
    s.size = fs::file_size(p);
    s.frames = frames;
    s.rate = 48000.0;
    s.channels = 2;
    s.displayName = "hits";
    const ops::NewAudioClip nc{track, kPpq / 2 + 1234, s.id, "Hits"};
    auto c = ops::addAudioClips(*st.current(), st.ids(), std::span<const SampleRef>(&s, 1), std::span<const ops::NewAudioClip>(&nc, 1));
    REQUIRE(c.ok());
    REQUIRE(st.dispatch(c->command).ok());
    clip = c->clips.at(0);
  }
  ~Rig() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void apply(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(ctx.store().dispatch(e->command).ok());
  }
  const AudioClip& audio() const {
    const Track* t = findTrackPtr(*ctx.doc(), track);
    return *t->clips[static_cast<size_t>(findClipIndex(*t, clip.clip))].audio;
  }
  void warp(AudioWarp w, double cents) {
    apply(ops::setAudioClipWarp(*ctx.doc(), std::span<const ClipRef>(&clip, 1), w, 120.0));
    apply(ops::setAudioClipTranspose(*ctx.doc(), std::span<const ClipRef>(&clip, 1), AudioTranspose{TransposeUnit::Cents, cents, 0, tuning::Rational(1)}));
  }
};

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

size_t differing(const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b) {
  if (a.size() != b.size()) return SIZE_MAX;
  size_t n = 0;
  for (size_t c = 0; c < a.size(); ++c) {
    if (a[c].size() != b[c].size()) return SIZE_MAX;
    for (size_t i = 0; i < a[c].size(); ++i) n += a[c][i] != b[c][i] ? 1 : 0;
  }
  return n;
}

}  // namespace

TEST_CASE("clip waves (SX.4): the render without the envelope is the clip player's, and the views' cached wave holds it") {
  Rig r("pq_clip_waves_eq", 2.0);
  r.warp(AudioWarp::Beats, 700.0);
  auto link = makeLink(r.ctx.doc());
  // The content alone: the same clip with no gain, fades or edge declick renders the same frames.
  const Snapshot plain = r.ctx.doc();
  r.apply(ops::setAudioClipGain(*r.ctx.doc(), std::span<const ClipRef>(&r.clip, 1), -6.0f));
  r.apply(ops::setAudioClipFades(*r.ctx.doc(), r.clip, AudioFade{kPpq / 2, 0.5f}, AudioFade{kPpq, -0.5f}));
  const Snapshot shaped = r.ctx.doc();
  auto content = link->renderClip(*shaped, r.clip, ProjectLink::ClipRenderOptions{false, nullptr});
  REQUIRE_MESSAGE(content.ok(), (content.ok() ? std::string() : content.error().message));
  app::ClipRenderSettings s = link->clipRenderSettings();
  s.edgeDeclick = false;
  auto ref = app::renderClipAlone(*plain, r.clip, r.file, link->samplePool(), s);
  REQUIRE(ref.ok());
  REQUIRE(content->size() == 2);
  float peak = 0;
  for (float x : (*content)[0]) peak = std::max(peak, std::fabs(x));
  CHECK(peak > 0.1f);
  CHECK(differing(*content, *ref) == 0);
  // With the envelope it differs (gain, fades), so the option is what removed it.
  auto played = link->renderClip(*shaped, r.clip);
  REQUIRE(played.ok());
  CHECK(differing(*played, *content) > 1000);
  // A deactivated clip is drawn too: the same frames.
  REQUIRE(r.ctx.store().dispatch(Command{SetClipActive{r.clip, false}}).ok());
  auto inactive = link->renderClip(*r.ctx.doc(), r.clip, ProjectLink::ClipRenderOptions{false, nullptr});
  REQUIRE(inactive.ok());
  CHECK(differing(*inactive, *content) == 0);
  REQUIRE(r.ctx.store().undo());
  // The views' cache (no poster: at once) through the app's source holds exactly these frames.
  r.ctx.waveforms().setClipRenderer(app::clipWaveSource(*link), [&] { return app::clipWaveConfig(*link); });
  const uint64_t key = views::clipRenderKey(*r.ctx.doc(), r.track, r.audio());
  const views::RenderedLookup l = r.ctx.waveforms().rendered(r.ctx.doc(), r.clip, r.audio(), key);
  REQUIRE(l.wave != nullptr);
  REQUIRE(l.wave->frames);
  CHECK(l.wave->rate == link->sampleRate());
  const media::SampleWindow& w = *l.wave->frames;
  REQUIRE(w.channels == 2);
  REQUIRE(w.frames == static_cast<int64_t>((*content)[0].size()));
  size_t diff = 0;
  for (uint32_t c = 0; c < 2; ++c)
    for (int64_t i = 0; i < w.frames; ++i) diff += w.channel(c)[i] != (*content)[c][static_cast<size_t>(i)] ? 1 : 0;
  CHECK(diff == 0);
  CHECK(l.wave->overview->frames() == static_cast<uint64_t>(w.frames));
  // A fade edit keeps the key (the wave is reused; the arrangement applies the fade at draw time).
  r.apply(ops::setAudioClipFades(*r.ctx.doc(), r.clip, AudioFade{kPpq, 0.0f}, AudioFade{}));
  CHECK(views::clipRenderKey(*r.ctx.doc(), r.track, r.audio()) == key);
  CHECK(r.ctx.waveforms().rendered(r.ctx.doc(), r.clip, r.audio(), key).wave == l.wave);
  r.ctx.waveforms().setClipRenderer({});  // before the link goes
}

TEST_CASE("clip waves (SX.4): golden arrangement-audio-rendered — a Beats clip +7 st drawn from its render beside its Off copy") {
  const char* scene = "arrangement-audio-rendered";
  views::setSceneClipWaveSource(app::sceneClipWaveSource());
  uint32_t w = 0, h = 0;
  views::viewSceneDefaultSize(scene, w, h);
  ui::GpuOptions o;
  o.validation = true;
  auto img = views::renderViewScene(scene, std::string(PQ_TEST_DATA_DIR) + "/views/fixtures/audio", w, h, {}, o);
  views::setSceneClipWaveSource({});
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
  auto ref = ui::readPng(path);
  if (!ref) (void)ui::writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ui::ImageDiff diff;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &diff));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(diff));
}

TEST_CASE("clip waves (SX.4): a 4-min Beats and a 4-min Complex Pro clip render off the UI thread; the swap is cheap" *
          doctest::test_suite("perf")) {
  Rig r("pq_clip_waves_perf", 240.0);
  auto link = makeLink(r.ctx.doc());
  for (const AudioWarp mode : {AudioWarp::Beats, AudioWarp::ComplexPro}) {
    r.warp(mode, 700.0);
    (void)link->renderClip(*r.ctx.doc(), r.clip, ProjectLink::ClipRenderOptions{false, nullptr});  // warm: in the pool
    const auto t0 = std::chrono::steady_clock::now();
    auto f = link->renderClip(*r.ctx.doc(), r.clip, ProjectLink::ClipRenderOptions{false, nullptr});
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(f.ok());
    // The cache: render + overview on this thread (no poster), the swap's UI-thread part measured by the cache.
    r.ctx.waveforms().setClipRenderer(app::clipWaveSource(*link), [&] { return app::clipWaveConfig(*link); });
    const uint64_t key = views::clipRenderKey(*r.ctx.doc(), r.track, r.audio());
    const auto t1 = std::chrono::steady_clock::now();
    const views::RenderedLookup l = r.ctx.waveforms().rendered(r.ctx.doc(), r.clip, r.audio(), key);
    const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    REQUIRE(l.wave != nullptr);
    MESSAGE(std::string(mode == AudioWarp::Beats ? "Beats" : "Complex Pro") << " 4-min stereo clip +7 st: render " << ms << " ms; render + overview "
                                                                 << total << " ms; UI-thread swap " << 1000.0 * r.ctx.waveforms().lastSwapSeconds()
                                                                 << " ms");
    CHECK(r.ctx.waveforms().lastSwapSeconds() < 0.005);
    r.ctx.waveforms().setClipRenderer({});
  }
}
