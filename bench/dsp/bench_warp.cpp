// T6.3: the warp modes' cost per stereo voice (docs/plans/M6.md T6.3: Beats ≤ 0.3 %, Tones / Texture ≤ 0.6 % of a core
// at 48 kHz), ns per output frame of one clip played by engine::ClipPlayer (resident stereo noise + tone source,
// 256-frame blocks, the clip in its flat middle, 120 → 97 BPM: a stretch of 1.24):
//   warp/beats          Beats on the 1/16 grid, Loop Forward, no transpose (slices read as straight copies)
//   warp/beats +3 st    the same transposed (sinc reads)
//   warp/tones          Tones, 30 ms grains (aligned: the WSOLA search per grain plus two sinc reads per frame)
//   warp/texture        Texture, 30 ms grains, Flux 40 (two straight copies per frame)
//   warp/repitch markers  Re-Pitch through 16 warp markers (reference: the M5 path, segment splits)
// % of a core = ns/frame × 48000 / 1e7.
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/engine/audio_sequence.h"
#include "pq/engine/clip_player.h"
#include "pq_bench.h"

namespace {

namespace e = pq::engine;
constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 256;
constexpr int64_t kFrames = 48000 * 60;  // a minute of source
constexpr int64_t kG = 64;

struct Source {
  pq::AlignedBuffer<float> buf;
  e::ClipSample sample;
  Source() {
    const int64_t stride = kFrames + 2 * kG;
    buf = pq::AlignedBuffer<float>(static_cast<size_t>(stride) * 2);
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-0.3f, 0.3f);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = 0.0f;
    for (uint32_t c = 0; c < 2; ++c) {
      float* d = buf.data() + static_cast<size_t>(c) * stride + kG;
      for (int64_t i = 0; i < kFrames; ++i)
        d[i] = u(rng) + 0.4f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 196.0 * static_cast<double>(i) / kSr + c));
      sample.fixed.data[c] = d;
    }
    sample.fixed.frames = sample.fixed.residentFrames = kFrames;
    sample.fixed.channels = 2;
    sample.fixed.sampleRate = 48000;
  }
};

void voiceBench(pq::bench::Context& c, e::ClipWarp warp, double ratio, bool markers) {
  static Source src;
  auto tempo = pq::TempoMap::create(std::vector<pq::TempoPoint>{{0, 97.0, pq::TempoCurve::Step}});
  e::AudioClipDesc d;
  d.id = 1;
  d.warp = warp;
  d.start = 0;
  d.length = pq::kPpq * 90;  // ≈ 56 s at 97 BPM
  d.framesPerTick = kSr * 60.0 / (120.0 * static_cast<double>(pq::kPpq));
  d.stretch.pitchRatio = ratio;
  d.stretch.flux = warp == e::ClipWarp::Texture ? 40.0f : 0.0f;
  if (markers)
    for (int k = 0; k < 16; ++k)
      d.segments.push_back({pq::kPpq * 5 * k, d.framesPerTick * pq::kPpq * 5 * k * (1.0 + 0.002 * k), d.framesPerTick * (0.9 + 0.02 * (k % 7))});
  e::AudioTrackDesc tr;
  tr.clips = {d};
  e::AudioBuildOptions o;
  o.sampleRate = kSr;
  auto img = e::AudioSequenceImage::build(std::span<const e::AudioTrackDesc>(&tr, 1), {src.sample}, tempo.value(), o);
  e::ClipPlayerConfig pc;
  pc.slots = 1;
  pc.maxBlockFrames = kBlock;
  pc.sampleRate = kSr;
  e::ClipPlayer player(pc);
  const e::AudioSequenceImage* image = img.value().get();
  pq::SampleTime s = static_cast<pq::SampleTime>(kSr * 2.0);  // past the clip's fade in
  const pq::SampleTime wrapAt = static_cast<pq::SampleTime>(kSr * 50.0);
  player.beginBlock(kBlock);
  player.setImage(image, s, true);
  player.seek(s);
  player.endBlock();
  c.measure(
      [&] {
        player.beginBlock(kBlock);
        player.play(0, s, kBlock);
        player.endBlock();
        s += kBlock;
        if (s > wrapAt) {  // back to the start (a locate: outside the timed region's steady state only once a minute)
          s = static_cast<pq::SampleTime>(kSr * 2.0);
          player.releaseAll(0);
          player.idle(kBlock);
          player.seek(s);
        }
        pq::bench::doNotOptimize(player.channels()[0][kBlock - 1]);
      },
      kBlock, "ns/frame");
}

}  // namespace

void addWarpBenches(pq::bench::Runner& runner) {
  runner.add("warp/beats", [](pq::bench::Context& c) { voiceBench(c, e::ClipWarp::Beats, 1.0, false); });
  runner.add("warp/beats +3 st", [](pq::bench::Context& c) { voiceBench(c, e::ClipWarp::Beats, std::exp2(3.0 / 12.0), false); });
  runner.add("warp/tones", [](pq::bench::Context& c) { voiceBench(c, e::ClipWarp::Tones, 1.0, false); });
  runner.add("warp/texture", [](pq::bench::Context& c) { voiceBench(c, e::ClipWarp::Texture, 1.0, false); });
  runner.add("warp/repitch markers", [](pq::bench::Context& c) { voiceBench(c, e::ClipWarp::RePitch, 1.0, true); });
}
