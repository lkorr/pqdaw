// NZ.1: the Noise module in a project (pq/document/modules.h "Noise", pq/meta/noise.h), through the real lowering and
// the link:
//   free     a Noise module on an audio track, its Out into the mixer, nothing into its In: plain white noise (the
//            unwired clock folds away), interpreter == JIT bit for bit; the blocks run under RtScope
//   sync     Clock Sync at the project's tempo: a new value on every 1/16 line (6000 frames at 120 bpm), none between
//   restart  On play: a second playback from the start repeats the first bit for bit; Free: it does not
//   panel    the module's display reads the engine's voice tap: its values while it plays (in [−1, 1])
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/face_voices.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/document/store.h"
#include "pq/meta/noise.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

struct Rig {
  DocumentStore store;
  TrackId track;
  DeviceId group, engine;
  void apply(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
  }
  Rig() {
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Audio, {}, "Noise");
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    track = t->tracks.at(0);
    auto e = ops::pasteBlocks(*store.current(), store.ids(), track, moduleBlocks(ModuleKind::Noise));
    REQUIRE(e.ok());
    REQUIRE(store.dispatch(e->command).ok());
    group = e->devices.at(0);
    for (const Device* d : groupMembersDeep(findTrackPtr(*store.current(), track)->devices, group))
      if (d->patch == "meta:noise") engine = d->id;
    REQUIRE(engine.valid());
    Wire w;
    w.from = group;
    w.port = std::string(kMainPort);
    w.mixer = track;
    apply(ops::connect(*store.current(), store.ids(), w));
  }
  void setting(const char* name, double v) { apply(ops::setMetaSetting(*store.current(), engine, "", "noise", name, v)); }
  void param(const char* name, double v) { apply(ops::setMetaParamValue(*store.current(), engine, "", "noise", name, v)); }
};

std::unique_ptr<app::ProjectLink> makeLink(const Snapshot& s, bool jit) {
  app::LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  c.sampleRate = kRate;
  auto l = app::ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  if (jit) REQUIRE((*l)->waitForJit(180.0));
  return std::move(*l);
}

// `blocks` blocks of the left channel (the module is mono into both sides).
std::vector<float> run(app::ProjectLink& link, int64_t& t, uint32_t blocks) {
  std::vector<float> out;
  std::vector<float> L(kBlock), R(kBlock);
  for (uint32_t b = 0; b < blocks; ++b) {
    float* ch[2] = {L.data(), R.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, t});
    }
    t += kBlock;
    out.insert(out.end(), L.begin(), L.end());
  }
  CHECK(link.lastError().empty());
  return out;
}

std::vector<float> play(const Snapshot& s, bool jit, uint32_t blocks) {
  auto link = makeLink(s, jit);
  link->play(0);
  int64_t t = 0;
  return run(*link, t, blocks);
}

}  // namespace

TEST_CASE("NZ.1 Noise module: unwired it is white noise on an audio track; interpreter == JIT") {
  Rig r;
  const std::vector<float> a = play(r.store.current(), false, 300), b = play(r.store.current(), true, 300);
  REQUIRE(a == b);
  size_t changed = 0;
  float peak = 0.0f;
  for (size_t i = 2400; i < a.size(); ++i) {
    changed += a[i] != a[i - 1];
    peak = std::max(peak, std::fabs(a[i]));
  }
  CHECK(changed > (a.size() - 2400) * 99 / 100);
  CHECK(peak > 0.5f);
  CHECK(peak <= 1.0f);
}

TEST_CASE("NZ.1 Noise module: Sync steps on the 1/16 grid while playing") {
  Rig r;
  r.setting("clock", 1.0);
  const std::vector<float> a = play(r.store.current(), false, 1000);
  // (the mixer's gain ramps in over the first frames: look past it)
  std::vector<size_t> at;
  for (size_t i = 2400; i < a.size(); ++i)
    if (a[i] != a[i - 1]) at.push_back(i);
  REQUIRE(at.size() >= 8);
  for (size_t k : at) CHECK(k % 6000 == 0);  // 1/16 at 120 bpm
  CHECK(at.size() >= (a.size() - 2400) / 6000 - 1);
}

TEST_CASE("NZ.1 Noise module: On play repeats every playback bit for bit; Free does not") {
  for (const double restart : {0.0, 1.0}) {
    CAPTURE(restart);
    Rig r;
    r.setting("restart", restart);
    r.param("rate", 300.0);
    r.param("smooth", 0.5);
    r.param("colour", -3.0);
    auto link = makeLink(r.store.current(), false);
    int64_t t = 0;
    link->play(0);
    const std::vector<float> first = run(*link, t, 200);
    link->stop();
    (void)run(*link, t, 20);
    link->play(0);
    const std::vector<float> second = run(*link, t, 200);
    // past the mixer's ramp in (the first playback's) the two agree exactly with On play
    size_t diff = 0;
    for (size_t i = 2400; i < first.size(); ++i) diff += first[i] != second[i];
    if (restart == 0.0) CHECK(diff == 0);
    else CHECK(diff > (first.size() - 2400) / 2);
  }
}

TEST_CASE("NZ.1 Noise module: the display reads the engine's voice tap while it plays") {
  Rig r;
  r.param("rate", 20.0);
  auto link = makeLink(r.store.current(), false);
  app::FaceVoiceHub hub(*link);
  views::VoiceTapReader* reader = hub.voices(r.engine, "");
  REQUIRE(reader != nullptr);
  std::array<views::FaceVoice, 16> v{};
  (void)reader->read(v);  // registers the tap's reader
  link->play(0);
  int64_t t = 0;
  uint32_t seen = 0;
  for (uint32_t b = 0; b < 200 && seen == 0; ++b) {
    (void)run(*link, t, 1);
    if (b % 8 == 7) seen = reader->read(v);
  }
  REQUIRE(seen == 1);  // one instance (an audio track: global)
  CHECK(v[0].phase >= -1.0f);
  CHECK(v[0].phase <= 1.0f);
  CHECK(v[0].sampleCount > 0);
}
