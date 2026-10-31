// M5 review probe: the clip player driven through the Timeline (as tests/engine/test_clip_player.cpp does).
//  1. RePitch at a tempo ramp: bit-identical across ENGINE blocks 32/64/256/1024? (the T5.4 test only checks Off clips;
//     the app test keeps the engine block at 64 behind a BlockAdapter)
//  2. Off clips reading outside the file (negative contentStartFrame, reverse at both ends, rate 1.37): finite, bounded
//  3. crossfadeIn longer than the previous clip / zero-length fades / huge gain: finite
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/engine/audio_sequence.h"
#include "pq/engine/clip_player.h"
#include "pq/engine/timeline.h"

namespace e = pq::engine;
using pq::kPpq;
using pq::SampleTime;
using pq::Ticks;

constexpr double kSr = 48000.0;
constexpr int64_t kG = pq::media::kSampleGuardFrames;

struct Frames {
  pq::AlignedBuffer<float> buf;
};

e::ClipSample makeSample(int64_t frames, uint32_t channels, const std::function<float(uint32_t, int64_t)>& fn,
                         uint32_t rate = 48000) {
  auto o = std::make_shared<Frames>();
  const int64_t stride = frames + 2 * kG;
  o->buf = pq::AlignedBuffer<float>(static_cast<size_t>(stride) * channels);
  std::fill(o->buf.data(), o->buf.data() + o->buf.size(), 0.0f);
  e::ClipSample s;
  for (uint32_t c = 0; c < channels; ++c) {
    float* d = o->buf.data() + static_cast<size_t>(c) * stride + kG;
    for (int64_t i = 0; i < frames; ++i) d[i] = fn(c, i);
    s.fixed.data[c] = d;
  }
  s.fixed.frames = s.fixed.residentFrames = frames;
  s.fixed.channels = channels;
  s.fixed.sampleRate = rate;
  s.owner = o;
  return s;
}

std::vector<float> run(uint32_t block, const std::vector<pq::TempoPoint>& pts, std::vector<e::AudioTrackDesc> slots,
                       std::vector<e::ClipSample> samples, uint64_t frames, Ticks from = 0) {
  e::TimelineConfig cfg;
  cfg.sampleRate = kSr;
  cfg.maxBlockFrames = block;
  cfg.maxTracks = 8;
  e::Timeline tl(cfg);
  auto tmap = pq::TempoMap::create(pts);
  auto timg = e::TempoImage::create(pts, {});
  (void)tl.publishTempo(std::move(timg.value()));
  e::AudioBuildOptions o;
  o.sampleRate = kSr;
  auto img = e::AudioSequenceImage::build(slots, std::move(samples), tmap.value(), o);
  if (!img.ok()) {
    std::printf("build failed: %s\n", img.error().message.c_str());
    return {};
  }
  (void)tl.publishAudio(std::move(img.value()));
  e::TransportCommand c;
  c.kind = e::TransportCommandKind::Play;
  c.tick = from;
  tl.apply(c);
  std::vector<float> out;
  int64_t time = 0;
  for (uint64_t done = 0; done < frames;) {
    const auto n = static_cast<uint32_t>(std::min<uint64_t>(block, frames - done));
    tl.process(time, n);
    time += n;
    const float* p = tl.clipPlayer().channels()[0];
    out.insert(out.end(), p, p + n);
    done += n;
  }
  return out;
}

int main() {
  auto noise = [](uint32_t c, int64_t i) {
    return static_cast<float>(std::sin(0.0137 * static_cast<double>(i) + c) * 0.5 + 0.3 * std::sin(0.71 * static_cast<double>(i)));
  };
  // ---- 1. RePitch at a ramp across engine blocks ----
  {
    const std::vector<pq::TempoPoint> ramp = {{0, 100.0, pq::TempoCurve::Linear}, {kPpq * 8, 160.0, pq::TempoCurve::Step}};
    e::AudioClipDesc c;
    c.id = 1;
    c.warp = e::ClipWarp::RePitch;
    c.start = kPpq / 3;
    c.length = kPpq * 6;
    c.loop = {true, kPpq / 8, kPpq * 5 / 4};
    c.framesPerTick = kSr * 60.0 / (110.0 * static_cast<double>(kPpq));
    std::vector<std::vector<float>> res;
    for (uint32_t b : {32u, 64u, 256u, 1024u})
      res.push_back(run(b, ramp, {{1, {c}}}, {makeSample(200000, 1, noise)}, static_cast<uint64_t>(kSr * 3)));
    for (size_t k = 1; k < res.size(); ++k) {
      double maxd = 0.0;
      size_t diff = 0;
      for (size_t i = 0; i < res[0].size(); ++i) {
        const double d = std::abs(static_cast<double>(res[k][i]) - res[0][i]);
        if (d != 0.0) ++diff;
        maxd = std::max(maxd, d);
      }
      std::printf("RePitch ramp, block %s vs 32: %zu of %zu samples differ, max |diff| = %.3g (%.1f dBFS)\n",
                  k == 1 ? "64" : k == 2 ? "256" : "1024", diff, res[0].size(), maxd, 20 * std::log10(maxd + 1e-30));
    }
    // Constant tempo, RePitch rate != 1.
    std::vector<std::vector<float>> r2;
    for (uint32_t b : {32u, 1024u})
      r2.push_back(run(b, {{0, 128.0, pq::TempoCurve::Step}}, {{1, {c}}}, {makeSample(200000, 1, noise)}, static_cast<uint64_t>(kSr * 3)));
    size_t diff = 0;
    double maxd = 0;
    for (size_t i = 0; i < r2[0].size(); ++i) {
      const double d = std::abs(static_cast<double>(r2[1][i]) - r2[0][i]);
      diff += d != 0.0;
      maxd = std::max(maxd, d);
    }
    std::printf("RePitch constant 128 BPM, block 1024 vs 32: %zu samples differ, max %.3g\n", diff, maxd);
  }
  // ---- 2. Off clips reading outside the file ----
  {
    const std::vector<pq::TempoPoint> t120 = {{0, 120.0, pq::TempoCurve::Step}};
    auto dc = [](uint32_t, int64_t) { return 1.0f; };
    struct Case {
      const char* name;
      double csf;
      double speed;
      bool reverse;
    } cases[] = {{"fwd csf=-3000.5 rate 1.37", -3000.5, kSr * 1.37, false},
                 {"rev csf=-3000.5 rate 1.37", -3000.5, kSr * 1.37, true},
                 {"fwd csf=-3000 rate 1 (copy)", -3000.0, kSr, false},
                 {"rev csf=-3000 rate 1 (copy)", -3000.0, kSr, true},
                 {"fwd past end rate 0.71", 9000.25, kSr * 0.71, false},
                 {"rev past end rate 3.3", 2000.0, kSr * 3.3, true},
                 {"fwd rate 15.9", 0.0, kSr * 15.9, false}};
    for (const Case& k : cases) {
      e::AudioClipDesc c;
      c.id = 1;
      c.start = kPpq / 2;
      c.length = kPpq;
      c.speed = k.speed;
      c.contentStartFrame = k.csf;
      c.reverse = k.reverse;
      const std::vector<float> o = run(64, t120, {{1, {c}}}, {makeSample(10000, 1, dc)}, 60000);
      double mx = 0;
      int nan = 0;
      size_t nz = 0;
      for (float v : o) {
        if (!std::isfinite(v)) ++nan;
        mx = std::max(mx, static_cast<double>(std::abs(v)));
        nz += v != 0.0f;
      }
      std::printf("%-30s: nonfinite %d, peak %.6f, nonzero %zu\n", k.name, nan, mx, nz);
    }
  }
  // ---- 3. crossfade longer than the previous clip, fades of 0 ----
  {
    const std::vector<pq::TempoPoint> t120 = {{0, 120.0, pq::TempoCurve::Step}};
    e::AudioClipDesc a, b;
    a.id = 1;
    a.start = kPpq;
    a.length = kPpq / 16;
    a.speed = kSr;
    b = a;
    b.id = 2;
    b.start = kPpq + kPpq / 16;
    b.length = kPpq;
    b.crossfadeIn = kPpq;  // 16x the previous clip
    const std::vector<float> o = run(64, t120, {{1, {a, b}}}, {makeSample(100000, 1, [](uint32_t, int64_t) { return 1.0f; })}, 80000);
    double mx = 0;
    int nan = 0;
    for (float v : o) {
      if (!std::isfinite(v)) ++nan;
      mx = std::max(mx, static_cast<double>(std::abs(v)));
    }
    std::printf("crossfade > previous clip: nonfinite %d peak %.4f\n", nan, mx);
  }
  return 0;
}
