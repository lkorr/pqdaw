// T6.3: the warp modes through the playback link (src/app/src/project_link_audio.cpp): a Tones clip transposed +3 steps
// of a 31-EDO clip tuning renders exactly 3·1200/31 ¢ above its 220 Hz source (the document's Steps resolution reaches
// the engine's pitch ratio), and so do Complex / Complex Pro (T6.5: the stretch pool's vocoder, resampled to the exact
// pitch); a Beats clip that slices at transients gets them from
// the loader thread's analysis, cached in <project>/cache/analysis and in memory, and republishes with them.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/document/audio_sequence.h"
#include "pq/document/edit_ops.h"
#include "pq/document/store.h"
#include "pq/engine/offline_render.h"
#include "pq/media/transients.h"
#include "pq/media/wav_file.h"
#include "pq/tuning/edo.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 48000.0;

std::string utf8(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

struct Song {
  DocumentStore store;
  fs::path dir;
  TrackId track;
  explicit Song(const char* name) : dir(fs::temp_directory_path() / name) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Audio);
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    track = t->tracks.at(0);
  }
  ~Song() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  SampleRef sample(const char* file, int64_t frames, const std::function<float(int64_t)>& fn) {
    std::vector<std::vector<float>> ch(1, std::vector<float>(static_cast<size_t>(frames)));
    for (int64_t i = 0; i < frames; ++i) ch[0][static_cast<size_t>(i)] = fn(i);
    const fs::path p = dir / file;
    REQUIRE(media::writeWav(p, ch, static_cast<uint32_t>(kRate)).ok());
    SampleRef s;
    s.id = store.ids().sample();
    s.path = utf8(p);
    s.size = fs::file_size(p);
    s.frames = frames;
    s.rate = kRate;
    s.channels = 1;
    s.displayName = file;
    return s;
  }
  ClipRef clip(Ticks start, const SampleRef& s) {
    const ops::NewAudioClip nc{track, start, s.id, "Take"};
    auto c = ops::addAudioClips(*store.current(), store.ids(), std::span<const SampleRef>(&s, 1),
                                std::span<const ops::NewAudioClip>(&nc, 1));
    REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
    REQUIRE(store.dispatch(c->command).ok());
    return c->clips.at(0);
  }
  void apply(const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
  }
  std::unique_ptr<ProjectLink> link() {
    LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.projectDir = utf8(dir);
    auto l = ProjectLink::create(c, store.current());
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
    return std::move(l.value());
  }
};

double dtftMag(const std::vector<float>& x, size_t from, size_t n, double f) {
  const double w = 2.0 * std::numbers::pi * f / kRate;
  double re = 0.0, im = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double h = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n));
    re += h * x[from + i] * std::cos(w * static_cast<double>(i));
    im -= h * x[from + i] * std::sin(w * static_cast<double>(i));
  }
  return std::sqrt(re * re + im * im);
}

double peakHz(const std::vector<float>& x, size_t from, size_t n, double guess) {
  double best = guess, bestM = -1.0;
  for (double f = guess - 3.0; f <= guess + 3.0; f += 0.05) {
    const double m = dtftMag(x, from, n, f);
    if (m > bestM) {
      bestM = m;
      best = f;
    }
  }
  double lo = best - 0.05, hi = best + 0.05;
  for (int i = 0; i < 40; ++i) {  // ternary search on the main lobe
    const double a = lo + (hi - lo) / 3.0, b = hi - (hi - lo) / 3.0;
    if (dtftMag(x, from, n, a) > dtftMag(x, from, n, b)) hi = b;
    else lo = a;
  }
  return 0.5 * (lo + hi);
}

std::vector<float> render(ProjectLink& link, uint64_t frames) {
  engine::BufferSink sink;
  auto st = link.renderOffline(0, frames, sink);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  return sink.channels[0];
}

}  // namespace

TEST_CASE("warp link: Tones / Complex / Complex Pro +3 steps of a 31-EDO clip tuning are 3·1200/31 ¢ above the source") {
  Song song("pq_warp_link_tones");
  const SampleRef s = song.sample("sine.wav", static_cast<int64_t>(kRate * 8.0), [](int64_t i) {
    return static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / kRate));
  });
  const ClipRef c = song.clip(0, s);
  const std::span<const ClipRef> one(&c, 1);
  song.apply(ops::setWarpMode(*song.store.current(), one, AudioWarp::Tones));
  song.apply(ops::setAudioClipTranspose(*song.store.current(), one, AudioTranspose{TransposeUnit::Steps, 0.0, 3, {1}}));
  TuningDef d;
  d.system = *tuning::Edo::make(31);
  REQUIRE(song.store.dispatch(SetClipTuning{c, TuningAssignment::privateTuning(d)}).ok());
  const AudioTrackDesc desc = lowerAudioTrack(*song.store.current(), *findTrack(*song.store.current(), song.track));
  REQUIRE(desc.clips.size() == 1);
  CHECK(desc.clips[0].pitchRatio == doctest::Approx(std::exp2(3.0 / 31.0)).epsilon(1e-12));
  const double expect = 3.0 * 1200.0 / 31.0;
  for (const AudioWarp w : {AudioWarp::Tones, AudioWarp::Complex, AudioWarp::ComplexPro}) {
    CAPTURE(static_cast<int>(w));
    song.apply(ops::setWarpMode(*song.store.current(), one, w));
    auto link = song.link();
    const std::vector<float> out = render(*link, static_cast<uint64_t>(kRate * 3.0));
    const double f = peakHz(out, 4800, 65536, 220.0 * std::exp2(3.0 / 31.0));
    const double cents = 1200.0 * std::log2(f / 220.0);
    MESSAGE("link warp mode ", static_cast<int>(w), ": ", f, " Hz = ", cents, " cents (expected ", expect, ")");
    CHECK(std::abs(cents - expect) <= 0.5);
  }
}

TEST_CASE("warp link: Beats at transients analyses the sample on the loader (cached on disk and in memory)") {
  Song song("pq_warp_link_beats");
  media::transientCache().clear();
  // Clicks every 7000 frames (irregular against the 1/16 grid of 6000 at 120 BPM).
  const SampleRef s = song.sample("clicks.wav", 96000, [](int64_t i) {
    const int64_t k = i % 7000;
    return static_cast<float>(k < 2000 ? 0.6 * std::exp(-static_cast<double>(k) / 300.0) * std::sin(0.3 * static_cast<double>(k)) : 0.0);
  });
  const ClipRef c = song.clip(0, s);
  const std::span<const ClipRef> one(&c, 1);
  song.apply(ops::setWarpMode(*song.store.current(), one, AudioWarp::Beats));
  WarpParams wp;
  wp.beatsGranulation = 0;  // transients
  song.apply(ops::setAudioClipWarpParams(*song.store.current(), one, wp));
  auto link = song.link();
  (void)render(*link, 48000);  // waits for the load and the analysis job, then republishes
  const fs::path cacheDir = song.dir / "cache" / "analysis";
  REQUIRE(fs::exists(cacheDir));
  int files = 0;
  for (const auto& e : fs::directory_iterator(cacheDir)) files += e.path().extension() == ".pqtr" ? 1 : 0;
  CHECK(files == 1);
  // The onsets: every click (13 of them in 2 s).
  media::TransientsPtr t;
  for (const auto& e : fs::directory_iterator(cacheDir)) {
    auto l = media::loadTransients(e.path());
    REQUIRE(l.ok());
    t = *l;
  }
  REQUIRE(t != nullptr);
  REQUIRE(t->onsets.size() == 14);
  for (size_t i = 0; i < t->onsets.size(); ++i) CHECK(std::abs(t->onsets[i] - 7000.0 * static_cast<double>(i)) <= 48.0);
  CHECK(media::transientCache().find(t->contentHash, t->frames) != nullptr);
  // With the onsets the clip slices there: at 120 BPM (the source's tempo) it plays the source unchanged in between.
  // Each click is the source's, from its slice's start (a slice starts on a whole sample: within one frame).
  const std::vector<float> out = render(*link, 48000);
  auto want = [](int64_t i) {
    const int64_t k = i % 7000;
    return k < 2000 && i >= 0 ? 0.6 * std::exp(-static_cast<double>(k) / 300.0) * std::sin(0.3 * static_cast<double>(k)) : 0.0;
  };
  for (int64_t cl = 0; cl < 6; ++cl) {
    CAPTURE(cl);
    double best = 1e9;
    for (int64_t shift = -1; shift <= 1; ++shift) {
      double err = 0.0;
      for (int64_t i = cl * 7000 + 150; i < cl * 7000 + 6850; ++i)
        err = std::max(err, std::abs(static_cast<double>(out[static_cast<size_t>(i)]) - want(i - shift)));
      best = std::min(best, err);
    }
    CHECK(best < 1e-5);
  }
  media::transientCache().clear();
}
