// T5.12: audio clips rendered to WAV for drag-out (pq/app/clip_export.h, ProjectLink::renderClip): the frames equal the
// clip's solo offline render (the clip alone on an audio track of the project, rendered through the link) bit for bit —
// plain, with gain, fades and reverse, and transposed (the sinc path) — the WAV file holds exactly those frames (32-bit
// float; 24-bit PCM as the preference asks), and (perf, release) a 60-s stereo clip renders in ≤ 100 ms.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "pq/app/clip_export.h"
#include "pq/app/project_link.h"
#include "pq/document/edit_ops.h"
#include "pq/document/store.h"
#include "pq/engine/offline_render.h"
#include "pq/media/wav_file.h"

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

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
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
  SampleRef sample(const char* file, int64_t frames, uint32_t channels, double rate) {
    std::vector<std::vector<float>> ch(channels, std::vector<float>(static_cast<size_t>(frames)));
    for (uint32_t c = 0; c < channels; ++c)
      for (int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        ch[c][static_cast<size_t>(i)] = static_cast<float>(0.4 * std::sin(2 * std::numbers::pi * (220.0 + 110.0 * c) * t) +
                                                          0.1 * std::sin(2 * std::numbers::pi * 3000.0 * t));
      }
    const fs::path p = dir / file;
    REQUIRE(media::writeWav(p, ch, static_cast<uint32_t>(rate)).ok());
    SampleRef s;
    s.id = store.ids().sample();
    s.path = utf8(p);
    s.size = fs::file_size(p);
    s.frames = frames;
    s.rate = rate;
    s.channels = channels;
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
};

// The clip's frames in the project's own offline render (the clip alone on its track: what "solo" means here).
std::vector<std::vector<float>> projectFrames(ProjectLink& link, const Project& p, const ClipRef& c) {
  const Track& t = *findTrackPtr(p, c.track);
  const AudioClip& a = *t.clips.at(static_cast<size_t>(findClipIndex(t, c.clip))).audio;
  const TempoMap& tm = p.timeline->tempoMap;
  const SampleTime s0 = tm.ticksToSamples(a.start, kRate), s1 = tm.ticksToSamples(a.start + a.length, kRate);
  engine::BufferSink sink;
  auto st = link.renderOffline(0, static_cast<uint64_t>(s1), sink);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  std::vector<std::vector<float>> out(2);
  for (size_t ch = 0; ch < 2; ++ch) out[ch].assign(sink.channels[ch].begin() + s0, sink.channels[ch].begin() + s1);
  return out;
}

void checkSolo(Song& song, const ClipRef& c, uint32_t channels) {
  auto link = makeLink(song.store.current());
  const std::vector<std::vector<float>> ref = projectFrames(*link, *song.store.current(), c);
  auto clip = link->renderClip(*song.store.current(), c);
  REQUIRE_MESSAGE(clip.ok(), (clip.ok() ? std::string() : clip.error().message));
  REQUIRE(clip->size() == channels);
  float peak = 0;
  for (uint32_t ch = 0; ch < channels; ++ch) {
    REQUIRE((*clip)[ch].size() == ref[ch].size());
    size_t diff = 0;
    for (size_t i = 0; i < ref[ch].size(); ++i) {
      if ((*clip)[ch][i] != ref[ch][i]) ++diff;
      peak = std::max(peak, std::fabs(ref[ch][i]));
    }
    CAPTURE(ch);
    CHECK(diff == 0);
  }
  CHECK(peak > 0.05f);
}

}  // namespace

TEST_CASE("clip export: a clip renders exactly as it plays alone in the project (plain, gain / fades / reverse, transposed)") {
  Song song("pq_clip_export_solo");
  const SampleRef st = song.sample("st.wav", 24000, 2, 48000.0);
  const ClipRef c = song.clip(kPpq / 2 + 1234, st);
  checkSolo(song, c, 2);
  song.apply(ops::setAudioClipGain(*song.store.current(), std::span<const ClipRef>(&c, 1), -4.5f));
  song.apply(ops::setAudioClipFades(*song.store.current(), c, AudioFade{kPpq / 8, 0.4f}, AudioFade{kPpq / 6, -0.3f}));
  song.apply(ops::setAudioClipsReverse(*song.store.current(), std::span<const ClipRef>(&c, 1), true));
  checkSolo(song, c, 2);
  song.apply(ops::setAudioClipTranspose(*song.store.current(), std::span<const ClipRef>(&c, 1),
                                        AudioTranspose{TransposeUnit::Cents, 700.0, 0, tuning::Rational(1)}));
  checkSolo(song, c, 2);
  // A mono file at another rate (the resampler, one channel out).
  Song mono("pq_clip_export_mono");
  const SampleRef m = mono.sample("m.wav", 30000, 1, 44100.0);
  checkSolo(mono, mono.clip(kPpq, m), 1);
}

TEST_CASE("clip export: the WAV file holds the rendered frames (32-bit float, or 24-bit PCM)") {
  Song song("pq_clip_export_wav");
  const SampleRef s = song.sample("w.wav", 12000, 2, 48000.0);
  const ClipRef c = song.clip(0, s);
  auto link = makeLink(song.store.current());
  auto frames = link->renderClip(*song.store.current(), c);
  REQUIRE(frames.ok());
  const fs::path out = app::clipWavPath(*song.store.current(), c, song.dir);
  CHECK(out.filename() == "Take.wav");
  REQUIRE(app::writeClipWav(*link, *song.store.current(), c, out, false).ok());
  auto back = media::readWav(out);
  REQUIRE(back.ok());
  CHECK(back->isFloat);
  CHECK(back->sampleRate == 48000u);
  CHECK(back->channels == *frames);
  // The name is taken now: the next one gets " 2".
  CHECK(app::clipWavPath(*song.store.current(), c, song.dir).filename() == "Take 2.wav");
  const fs::path out24 = song.dir / "t24.wav";
  REQUIRE(app::writeClipWav(*link, *song.store.current(), c, out24, true).ok());
  auto b24 = media::readWav(out24);
  REQUIRE(b24.ok());
  CHECK_FALSE(b24->isFloat);
  CHECK(b24->bitsPerSample == 24);
  REQUIRE(b24->channels.size() == 2);
  float worst = 0;
  for (size_t ch = 0; ch < 2; ++ch)
    for (size_t i = 0; i < (*frames)[ch].size(); ++i) worst = std::max(worst, std::fabs(b24->channels[ch][i] - (*frames)[ch][i]));
  CHECK(worst <= 1.0f / 8388608.0f);  // half a 24-bit step each way, plus the reader's scaling
  CHECK(app::clipSeconds(*song.store.current(), c) == doctest::Approx(0.25).epsilon(1e-3));
  // A note clip is not an audio clip.
  CHECK_FALSE(link->renderClip(*song.store.current(), ClipRef{song.track, ClipId{999999}}).ok());
}

TEST_CASE("clip export: a 60-s stereo clip renders in 100 ms or less" * doctest::test_suite("perf")) {
  Song song("pq_clip_export_perf");
  const SampleRef s = song.sample("long.wav", 48000 * 60, 2, 48000.0);
  const ClipRef c = song.clip(0, s);
  auto link = makeLink(song.store.current());
  REQUIRE(link->renderClip(*song.store.current(), c).ok());  // warm: the sample is in the pool
  std::vector<double> ms;
  for (int k = 0; k < 5; ++k) {
    const auto t0 = std::chrono::steady_clock::now();
    auto r = link->renderClip(*song.store.current(), c);
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    REQUIRE(r.ok());
    REQUIRE(r->front().size() == static_cast<size_t>(48000 * 60));
  }
  std::sort(ms.begin(), ms.end());
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(app::writeClipWav(*link, *song.store.current(), c, song.dir / "out.wav", false).ok());
  const double withFile = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  MESSAGE("60-s stereo clip: render median " << ms[2] << " ms (min " << ms[0] << "), render + 32-bit WAV file " << withFile
                                             << " ms");
  CHECK(ms[2] <= 100.0);
}
