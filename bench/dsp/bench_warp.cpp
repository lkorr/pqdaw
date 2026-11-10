// T6.3: the warp modes' cost per stereo voice (docs/plans/M6.md T6.3: Beats ≤ 0.3 %, Tones / Texture ≤ 0.6 % of a core
// at 48 kHz), ns per output frame of one clip played by engine::ClipPlayer (resident stereo noise + tone source,
// 256-frame blocks, the clip in its flat middle, 120 → 97 BPM: a stretch of 1.24):
//   warp/beats          Beats on the 1/16 grid, Loop Forward, no transpose (slices read as straight copies)
//   warp/beats +3 st    the same transposed (sinc reads)
//   warp/tones          Tones, 30 ms grains (aligned: the WSOLA search per grain plus two sinc reads per frame)
//   warp/texture        Texture, 30 ms grains, Flux 40 (two straight copies per frame)
//   warp/repitch markers  Re-Pitch through 16 warp markers (reference: the M5 path, segment splits)
// T6.5 (docs/plans/M6.md T6.5: Complex ≤ 3 %, Complex Pro ≤ 5 % per stereo voice):
//   warp/complex        Complex, no transpose (the vocoder, its output copied)
//   warp/complex +3 st  the same transposed (the vocoder runs 1.19× as many frames, then the sinc resampler)
//   warp/complexPro     Complex Pro +3 st, Formants 100 (the vocoder's formant pass on top)
// T6.5b:
//   warp/complex mono   Complex on a mono source (a mono stretcher; before T6.5b: a stereo one fed twice)
//   warp/complex start  the worst block after a locate into the middle of the clip (a mid-clip start primes the
//                       stretcher), µs per block, over 64 blocks after each start; with 4 voices at once (4 clips)
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
  explicit Source(uint32_t channels = 2) {
    const int64_t stride = kFrames + 2 * kG;
    buf = pq::AlignedBuffer<float>(static_cast<size_t>(stride) * 2);
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-0.3f, 0.3f);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = 0.0f;
    for (uint32_t c = 0; c < channels; ++c) {
      float* d = buf.data() + static_cast<size_t>(c) * stride + kG;
      for (int64_t i = 0; i < kFrames; ++i)
        d[i] = u(rng) + 0.4f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 196.0 * static_cast<double>(i) / kSr + c));
      sample.fixed.data[c] = d;
    }
    sample.fixed.frames = sample.fixed.residentFrames = kFrames;
    sample.fixed.channels = channels;
    sample.fixed.sampleRate = 48000;
  }
};

std::unique_ptr<e::AudioSequenceImage> image(e::ClipWarp warp, double ratio, bool markers, const Source& src,
                                            uint32_t clips) {
  auto tempo = pq::TempoMap::create(std::vector<pq::TempoPoint>{{0, 97.0, pq::TempoCurve::Step}});
  std::vector<e::AudioTrackDesc> tracks(clips);
  for (uint32_t k = 0; k < clips; ++k) {
    e::AudioClipDesc d;
    d.id = 1 + k;
    d.warp = warp;
    d.start = 0;
    d.length = pq::kPpq * 90;  // ≈ 56 s at 97 BPM
    d.framesPerTick = kSr * 60.0 / (120.0 * static_cast<double>(pq::kPpq));
    d.stretch.pitchRatio = ratio;
    d.stretch.flux = warp == e::ClipWarp::Texture ? 40.0f : 0.0f;
    d.stretch.seed = 1 + k;
    if (markers)
      for (int q = 0; q < 16; ++q)
        d.segments.push_back({pq::kPpq * 5 * q, d.framesPerTick * pq::kPpq * 5 * q * (1.0 + 0.002 * q), d.framesPerTick * (0.9 + 0.02 * (q % 7))});
    tracks[k].clips = {d};
  }
  e::AudioBuildOptions o;
  o.sampleRate = kSr;
  auto img = e::AudioSequenceImage::build(tracks, std::vector<e::ClipSample>{src.sample}, tempo.value(), o);
  return std::move(img.value());
}

// The worst block after each locate into the middle of `clips` Complex clips (64 blocks: the start and what follows).
void startBench(pq::bench::Context& c, uint32_t clips) {
  static Source src;
  const auto img = image(e::ClipWarp::Complex, 1.0, false, src, clips);
  e::ClipPlayerConfig pc;
  pc.slots = clips;
  pc.maxBlockFrames = kBlock;
  pc.sampleRate = kSr;
  pc.stretchVoices = 2 * clips;
  pc.stretchMonoVoices = 0;
  e::ClipPlayer player(pc);
  pq::SampleTime s = static_cast<pq::SampleTime>(kSr * 2.0);
  player.beginBlock(kBlock);
  player.setImage(img.get(), s, true);
  player.seek(s);
  player.endBlock();
  const double nsPerTick = 1.0 / pq::bench::tscTicksPerNs();
  std::vector<double> worst;
  const int starts = std::max(8, c.options().repeats * 4);
  for (int k = -2; k < starts; ++k) {  // two untimed starts first
    s = static_cast<pq::SampleTime>(kSr * (3.0 + 0.37 * (k + 2)));
    player.beginBlock(kBlock);
    player.releaseAll(0);
    player.idle(kBlock);
    player.seek(s);
    player.endBlock();
    double w = 0.0;
    for (int b = 0; b < 64; ++b) {
      const uint64_t t0 = pq::bench::rdtsc();
      player.beginBlock(kBlock);
      player.play(0, s, kBlock);
      player.endBlock();
      const uint64_t t1 = pq::bench::rdtsc();
      pq::bench::doNotOptimize(player.channels()[0][kBlock - 1]);
      s += kBlock;
      w = std::max(w, static_cast<double>(t1 - t0) * nsPerTick / 1000.0);
    }
    if (k >= 0) worst.push_back(w);
  }
  c.record(pq::bench::summarize(std::move(worst)), "us/block", clips == 1 ? "1 voice" : "4 voices");
}

void voiceBench(pq::bench::Context& c, e::ClipWarp warp, double ratio, bool markers, uint32_t channels = 2) {
  static Source stereo(2), mono(1);
  const Source& src = channels == 1 ? mono : stereo;
  const auto img = image(warp, ratio, markers, src, 1);
  e::ClipPlayerConfig pc;
  pc.slots = 1;
  pc.maxBlockFrames = kBlock;
  pc.sampleRate = kSr;
  pc.stretchVoices = 2;  // T6.5: the playing voice and the one fading after the locate
  pc.stretchMonoVoices = 2;
  e::ClipPlayer player(pc);
  const e::AudioSequenceImage* im = img.get();
  pq::SampleTime s = static_cast<pq::SampleTime>(kSr * 2.0);  // past the clip's fade in
  const pq::SampleTime wrapAt = static_cast<pq::SampleTime>(kSr * 50.0);
  player.beginBlock(kBlock);
  player.setImage(im, s, true);
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
  runner.add("warp/complex", [](pq::bench::Context& c) { voiceBench(c, e::ClipWarp::Complex, 1.0, false); });
  runner.add("warp/complex +3 st", [](pq::bench::Context& c) { voiceBench(c, e::ClipWarp::Complex, std::exp2(3.0 / 12.0), false); });
  runner.add("warp/complexPro", [](pq::bench::Context& c) { voiceBench(c, e::ClipWarp::ComplexPro, std::exp2(3.0 / 12.0), false); });
  runner.add("warp/complex mono", [](pq::bench::Context& c) { voiceBench(c, e::ClipWarp::Complex, 1.0, false, 1); });
  runner.add("warp/complex start", [](pq::bench::Context& c) {
    startBench(c, 1);
    startBench(c, 4);
  });
}
