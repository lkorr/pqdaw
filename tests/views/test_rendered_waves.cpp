// SX.4: rendered clip waveforms in the views' waveform cache (pq/views/waveforms.h "RENDERED waveforms"): the content
// key (every param that changes the render moves it; the envelope never does), the synchronous path (the wave is the
// render: frames and overview), the render thread (debounced, a newer key cancels the running render, the previous
// render drawn meanwhile, the swap), the memory budget (frames dropped first, a deep zoom renders again), and the thread
// stopped cleanly (renderer removed mid-render, project closed mid-render, the context destroyed mid-render). The real
// render (the app's clip player) is checked against the offline render in tests/app/test_clip_waves.cpp.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/views/fake_transport.h"
#include "pq/views/view_context.h"
#include "pq/views/waveforms.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::views;
using namespace std::chrono_literals;

namespace {

struct QueuePoster {
  std::mutex m;
  std::vector<std::function<void()>> q;
  ViewContext::UiPoster poster() {
    return [this](std::function<void()> f) {
      std::lock_guard<std::mutex> l(m);
      q.push_back(std::move(f));
    };
  }
  int pump() {
    std::vector<std::function<void()>> run;
    {
      std::lock_guard<std::mutex> l(m);
      run.swap(q);
    }
    for (auto& f : run) f();
    return static_cast<int>(run.size());
  }
};

// The audio fixture (tests/views/fixtures/audio) with Drums' clips switched to Beats.
struct Rig {
  FakeTransport transport;
  std::unique_ptr<ViewContext> ctx = std::make_unique<ViewContext>(transport);
  std::vector<doc::ClipRef> drums;
  Rig() {
    REQUIRE(ctx->openProject(test::fixtureDir("audio")).ok());
    doc::forEachTrack(*ctx->doc(), [&](const doc::Track& t, const doc::Track*) {
      if (t.name == "Drums")
        for (const doc::ClipSlot& c : t.clips) drums.push_back({t.id, c.id});
    });
    REQUIRE(drums.size() == 3);
    apply(doc::ops::setAudioClipWarp(*ctx->doc(), drums, doc::AudioWarp::Beats, 120.0));
  }
  void apply(const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(ctx->store().dispatch(e->command).ok());
  }
  const doc::AudioClip& clip(size_t i) const {
    const doc::Track* t = doc::findTrackPtr(*ctx->doc(), drums[i].track);
    return *t->clips[static_cast<size_t>(doc::findClipIndex(*t, drums[i].clip))].audio;
  }
  uint64_t key(size_t i) const { return clipRenderKey(*ctx->doc(), drums[i].track, clip(i)); }
  RenderedLookup look(size_t i) { return ctx->waveforms().rendered(ctx->doc(), drums[i], clip(i), key(i)); }
};

// A stand-in render: the clip body's frames at the config rate, a sine whose frequency follows the key bits it is given
// (so two keys give two different waves); `gate` false blocks it (polling the cancel flag) until opened.
struct FakeRender {
  std::atomic<int> calls{0};
  std::atomic<int> cancelledSeen{0};
  std::atomic<bool> gate{true};
  std::atomic<int64_t> frames{0};
  ClipWaveFn fn() {
    return [this](const doc::Project& p, const ClipWaveRequest& q) -> Result<std::vector<std::vector<float>>> {
      ++calls;
      while (!gate.load()) {
        if (q.cancel != nullptr && q.cancel->load()) {
          ++cancelledSeen;
          return Error{ErrorCode::Unknown, "cancelled", {}};
        }
        std::this_thread::sleep_for(1ms);
      }
      const doc::Track* t = doc::findTrackPtr(p, q.clip.track);
      const doc::AudioClip& c = *t->clips[static_cast<size_t>(doc::findClipIndex(*t, q.clip.clip))].audio;
      const TempoMap& tm = p.timeline->tempoMap;
      const int64_t n = frames.load() > 0 ? frames.load()
                                          : tm.ticksToSamples(c.start + c.length, q.config.sampleRate) -
                                                tm.ticksToSamples(c.start, q.config.sampleRate);
      const double f = 50.0 + std::abs(c.transpose.cents) / 10.0;
      std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<size_t>(n)));
      for (int64_t i = 0; i < n; ++i) {
        out[0][static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(i * f * 6.283185307179586 / q.config.sampleRate));
        out[1][static_cast<size_t>(i)] = -out[0][static_cast<size_t>(i)];
      }
      return out;
    };
  }
};

template <class P>
bool waitFor(P pred, std::chrono::milliseconds limit = 5000ms) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

}  // namespace

TEST_CASE("rendered waves (SX.4): the content key moves with every param that changes the render, never with the envelope") {
  Rig r;
  const doc::Project& p = *r.ctx->doc();
  const doc::AudioClip& base = r.clip(0);
  const uint64_t k0 = r.key(0);
  CHECK(k0 != 0);
  auto key = [&](const doc::AudioClip& c) { return clipRenderKey(p, r.drums[0].track, c); };
  // Off and Re-Pitch draw their source.
  doc::AudioClip off = base;
  off.warp = doc::AudioWarp::Off;
  CHECK(key(off) == 0);
  off.warp = doc::AudioWarp::RePitch;
  CHECK(key(off) == 0);
  std::vector<std::pair<const char*, std::function<void(doc::AudioClip&)>>> moves = {
      {"Tones", [](doc::AudioClip& c) { c.warp = doc::AudioWarp::Tones; }},
      {"Texture", [](doc::AudioClip& c) { c.warp = doc::AudioWarp::Texture; }},
      {"Complex", [](doc::AudioClip& c) { c.warp = doc::AudioWarp::Complex; }},
      {"ComplexPro", [](doc::AudioClip& c) { c.warp = doc::AudioWarp::ComplexPro; }},
      {"transpose", [](doc::AudioClip& c) { c.transpose.cents = 700.0; }},
      {"detune", [](doc::AudioClip& c) { c.detuneCents = 12.0f; }},
      {"reverse", [](doc::AudioClip& c) { c.reverse = true; }},
      {"start", [](doc::AudioClip& c) { c.start += kPpq; }},
      {"length", [](doc::AudioClip& c) { c.length -= kPpq; }},
      {"contentStart", [](doc::AudioClip& c) { c.contentStart += kPpq / 4; }},
      {"loop", [](doc::AudioClip& c) { c.loop.enabled = true; }},
      {"sourceBpm", [](doc::AudioClip& c) { c.sourceBpm = 100.0; }},
      {"marker", [](doc::AudioClip& c) { c.markers.push_back({1000.0, kPpq}); }},
      {"granulation", [](doc::AudioClip& c) { c.warpParams.beatsGranulation = 0; }},
      {"beatsLoop", [](doc::AudioClip& c) { c.warpParams.beatsLoop = doc::BeatsLoop::Off; }},
      {"beatsEnvelope", [](doc::AudioClip& c) { c.warpParams.beatsEnvelope = 40.0f; }},
      {"grain", [](doc::AudioClip& c) { c.warpParams.grainSizeMs = 80.0f; }},
      {"flux", [](doc::AudioClip& c) { c.warpParams.flux = 50.0f; }},
      {"formants", [](doc::AudioClip& c) { c.warpParams.formants = 20.0f; }},
      {"envelope order", [](doc::AudioClip& c) { c.warpParams.envelope = 64.0f; }},
      {"id (Texture's seed)", [](doc::AudioClip& c) { c.id = doc::ClipId{c.id.value + 1000}; }},
  };
  std::vector<uint64_t> seen = {k0};
  for (auto& [name, f] : moves) {
    doc::AudioClip c = base;
    f(c);
    const uint64_t k = key(c);
    CAPTURE(name);
    CHECK(k != 0);
    CHECK(std::find(seen.begin(), seen.end(), k) == seen.end());
    seen.push_back(k);
  }
  std::vector<std::pair<const char*, std::function<void(doc::AudioClip&)>>> stays = {
      {"gain", [](doc::AudioClip& c) { c.gainDb = -12.0f; }},
      {"fade in", [](doc::AudioClip& c) { c.fadeIn = {kPpq, -1.0f}; }},
      {"fade out", [](doc::AudioClip& c) { c.fadeOut = {kPpq, 1.0f}; }},
      {"crossfade", [](doc::AudioClip& c) { c.crossfadeIn = kPpq / 2; }},
      {"name", [](doc::AudioClip& c) { c.name = "other"; }},
      {"active", [](doc::AudioClip& c) { c.active = false; }},
      {"noEdgeFade", [](doc::AudioClip& c) { c.noEdgeFade = true; }},
  };
  for (auto& [name, f] : stays) {
    doc::AudioClip c = base;
    f(c);
    CAPTURE(name);
    CHECK(key(c) == k0);
  }
  // The tempo over the clip moves it.
  doc::Project p2 = p;
  auto tl = std::make_shared<doc::Timeline>(*p.timeline);
  tl->tempo = {TempoPoint{0, 140.0}};
  auto tm = TempoMap::create(tl->tempo);
  REQUIRE(tm.ok());
  tl->tempoMap = std::move(*tm);
  p2.timeline = tl;
  CHECK(clipRenderKey(p2, r.drums[0].track, base) != k0);
  // The render config and the sample's path are part of the cache's key: a rate change renders again.
  FakeRender fr;
  ClipWaveConfig cfg;
  r.ctx->waveforms().setClipRenderer(fr.fn(), [&] { return cfg; });
  REQUIRE(r.look(0).wave != nullptr);
  CHECK(fr.calls == 1);
  REQUIRE(r.look(0).wave != nullptr);
  CHECK(fr.calls == 1);  // cached
  cfg.sampleRate = 44100.0;
  const RenderedLookup at44 = r.look(0);
  REQUIRE(at44.wave != nullptr);
  CHECK(fr.calls == 2);
  CHECK(at44.wave->rate == 44100.0);
}

TEST_CASE("rendered waves (SX.4): without a poster the render runs at once; the wave holds its frames and their overview") {
  Rig r;
  FakeRender fr;
  r.ctx->waveforms().setClipRenderer(fr.fn());
  const RenderedLookup l = r.look(0);
  REQUIRE(l.wave != nullptr);
  CHECK_FALSE(l.rendering);
  CHECK_FALSE(l.stale);
  const RenderedWave& w = *l.wave;
  REQUIRE(w.overview);
  REQUIRE(w.frames);
  const TempoMap& tm = r.ctx->doc()->timeline->tempoMap;
  const int64_t n = tm.ticksToSamples(r.clip(0).start + r.clip(0).length, 48000.0) - tm.ticksToSamples(r.clip(0).start, 48000.0);
  CHECK(w.frames->frames == n);
  CHECK(w.overview->frames() == static_cast<uint64_t>(n));
  CHECK(w.overview->complete());
  CHECK(w.overview->channels() == 2);
  // Level 0 bucket 3 == the quantised min / max of its frames.
  const float* ch1 = w.frames->channel(1);
  float lo = 1, hi = -1;
  for (uint32_t i = 3 * media::kOverviewBucketFrames; i < 4 * media::kOverviewBucketFrames; ++i) {
    lo = std::min(lo, ch1[i]);
    hi = std::max(hi, ch1[i]);
  }
  CHECK(w.overview->minMax(0, 1)[6] == media::quantiseMin(lo));
  CHECK(w.overview->minMax(0, 1)[7] == media::quantiseMax(hi));
  // A renderer that fails: no wave (the mapped source is drawn), not retried on the same snapshot.
  r.ctx->waveforms().setClipRenderer([](const doc::Project&, const ClipWaveRequest&) -> Result<std::vector<std::vector<float>>> {
    return Error{ErrorCode::NotFound, "no", {}};
  });
  CHECK(r.look(1).wave == nullptr);
  CHECK_FALSE(r.look(1).rendering);
  CHECK(r.ctx->waveforms().rendersStarted() == 1);
}

TEST_CASE("rendered waves (SX.4): the thread debounces, a newer key cancels the running render, the old render stands in, then swaps") {
  Rig r;
  QueuePoster qp;
  r.ctx->setUiPoster(qp.poster());
  FakeRender fr;
  fr.gate = false;
  Waveforms& wf = r.ctx->waveforms();
  wf.setClipRenderer(fr.fn());
  const auto t0 = std::chrono::steady_clock::now();
  RenderedLookup l = r.look(0);
  CHECK(l.rendering);
  CHECK(l.wave == nullptr);
  if (std::chrono::steady_clock::now() - t0 < 100ms) CHECK(wf.rendersStarted() == 0);  // the debounce (unless stalled)
  REQUIRE(waitFor([&] { return wf.rendersStarted() == 1; }));
  CHECK(std::chrono::steady_clock::now() - t0 >= Waveforms::kRenderDebounce);
  // An edit mid-render: the running render is cancelled, the new key queued.
  doc::AudioTranspose tr;
  tr.cents = 700.0;
  r.apply(doc::ops::setAudioClipTranspose(*r.ctx->doc(), std::span<const doc::ClipRef>(&r.drums[0], 1), tr));
  l = r.look(0);
  CHECK(l.rendering);
  REQUIRE(waitFor([&] { return fr.cancelledSeen == 1; }));
  REQUIRE(waitFor([&] { return wf.rendersStarted() == 2; }));
  fr.gate = true;
  REQUIRE(wf.waitRendersIdle());
  CHECK(qp.pump() >= 1);
  CHECK(wf.rendersDone() == 1);  // the cancelled one was never stored
  l = r.look(0);
  REQUIRE(l.wave != nullptr);
  CHECK_FALSE(l.stale);
  CHECK_FALSE(l.rendering);
  const RenderedWave* first = l.wave;
  CHECK(wf.lastSwapSeconds() < 0.05);
  // Another edit: the previous render is drawn (stale) until the new one arrives; then it swaps.
  tr.cents = 1200.0;
  r.apply(doc::ops::setAudioClipTranspose(*r.ctx->doc(), std::span<const doc::ClipRef>(&r.drums[0], 1), tr));
  l = r.look(0);
  CHECK(l.rendering);
  CHECK(l.stale);
  CHECK(l.wave == first);
  REQUIRE(wf.waitRendersIdle());
  REQUIRE(waitFor([&] { return wf.rendersStarted() == 3; }));
  REQUIRE(wf.waitRendersIdle());
  qp.pump();
  l = r.look(0);
  REQUIRE(l.wave != nullptr);
  CHECK_FALSE(l.stale);
  CHECK(l.wave != first);
  // Undo: the earlier render is still cached (no render).
  REQUIRE(r.ctx->store().undo());
  l = r.look(0);
  CHECK(l.wave == first);
  CHECK_FALSE(l.rendering);
  CHECK(fr.calls == 3);
  // Nothing renders while idle.
  std::this_thread::sleep_for(250ms);
  CHECK(fr.calls == 3);
  r.ctx->setUiPoster({});
}

TEST_CASE("rendered waves (SX.4): the memory budget drops the least recently drawn frames first; a deep zoom renders again") {
  Rig r;
  FakeRender fr;
  Waveforms& wf = r.ctx->waveforms();
  wf.setClipRenderer(fr.fn());
  fr.frames = 100000;  // 2 ch x 4 B: 800 kB of frames per render
  wf.setRenderBudget(2u << 20);
  for (size_t i = 0; i < 3; ++i) REQUIRE(r.look(i).wave != nullptr);
  CHECK(wf.renderBytes() <= wf.renderBudget());
  const RenderedLookup l0 = r.look(0);  // the least recently drawn before this lookup
  REQUIRE(l0.wave != nullptr);
  CHECK_FALSE(l0.wave->frames);  // its frames went
  REQUIRE(l0.wave->overview);    // its overview stayed
  CHECK(r.look(2).wave->frames);
  // A deep zoom on it renders it again with its frames (here at once: no poster).
  const int calls = fr.calls;
  const media::SampleWindow* w = wf.renderedFrames(r.ctx->doc(), r.drums[0], r.clip(0), *l0.wave);
  REQUIRE(w != nullptr);
  CHECK(w->frames == 100000);
  CHECK(fr.calls == calls + 1);
  CHECK(wf.renderBytes() <= wf.renderBudget());
  // A render longer than half the budget keeps only its overview (never re-rendered for frames).
  fr.frames = 400000;
  doc::AudioTranspose tr;
  tr.cents = 100.0;
  r.apply(doc::ops::setAudioClipTranspose(*r.ctx->doc(), std::span<const doc::ClipRef>(&r.drums[1], 1), tr));
  const RenderedLookup big = r.look(1);
  REQUIRE(big.wave != nullptr);
  CHECK_FALSE(big.wave->frames);
  const int before = fr.calls;
  CHECK(wf.renderedFrames(r.ctx->doc(), r.drums[1], r.clip(1), *big.wave) == nullptr);
  CHECK(fr.calls == before);
}

TEST_CASE("rendered waves (SX.4): the thread stops cleanly: renderer removed, project closed, context destroyed mid-render") {
  for (int mode = 0; mode < 3; ++mode) {
    CAPTURE(mode);
    Rig r;
    QueuePoster qp;
    r.ctx->setUiPoster(qp.poster());
    FakeRender fr;
    fr.gate = false;
    r.ctx->waveforms().setClipRenderer(fr.fn());
    (void)r.look(0);
    REQUIRE(waitFor([&] { return r.ctx->waveforms().rendersStarted() == 1; }));
    const auto t0 = std::chrono::steady_clock::now();
    if (mode == 0) {
      r.ctx->waveforms().setClipRenderer({});
      CHECK(fr.cancelledSeen == 1);
      CHECK_FALSE(r.ctx->waveforms().hasClipRenderer());
    } else if (mode == 1) {
      r.ctx->newProject();  // Waveforms::clear: the render cancelled, its result dropped
      REQUIRE(waitFor([&] { return fr.cancelledSeen == 1; }));
      REQUIRE(r.ctx->waveforms().waitRendersIdle());
      qp.pump();
      CHECK(r.ctx->waveforms().rendersDone() == 0);
      CHECK(r.ctx->waveforms().renderBytes() == 0);
    } else {
      r.ctx.reset();  // joins
      CHECK(fr.cancelledSeen == 1);
    }
    CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    qp.pump();  // a late post finds no owner
  }
}
