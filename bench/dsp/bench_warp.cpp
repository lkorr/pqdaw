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
// SM.4 (the Sampler's sampler_warp atom, pq/dsp/warp_voice.h), ns per voice-frame of the kernel called as the
// interpreter does (64-frame blocks, a 60 s stereo slot with 3 mips, slices on the 1/16 grid, looped over 80 % of it,
// 120 → 97 BPM, voices triggered once, then sustained):
//   sampler_warp/beats L1 | L8        Beats, Loop Forward, rate 1 (param rate)
//   sampler_warp/tones L1 | L8        Tones, 30 ms grains, rate 1
//   sampler_warp/tones +3 st L8       rate 2^(3/12)
//   sampler_warp/tones glide L8       an audio-rate rate (1 → 1.5 ramps: per-sample positions)
//   sampler_warp/texture L8           Texture, 30 ms, Flux 40
//   L1: one voice (ns/frame), L8: an 8-voice group (ns per voice-frame = the group's ns/frame / 8).
// SM.5 (pq/dsp/sampler_stretch.h; plan: ≈ 1.6 % of a core per voice, the T6.5 row):
//   sampler_warp/complex 16v          Complex, 16 voices (two 8-lane groups, a stereo stretcher each), rate 1,
//                                     measured after priming: ns/frame of all 16 (per voice: / 16)
//   sampler_warp/complex +18/31 16v   the same at 2^(18/31) (the vocoder runs 1.5× the frames, the read resamples)
//   sampler_warp/complexPro 16v       Complex Pro at 2^(18/31), Formants 100
// SM.14 (KernelArgs::laneMask: freed lanes stop):
//   sampler_warp/complex 16v released 14   the 16 primed voices, then 14 of them freed (one held per group, the groups
//                                          still run): ns/frame of the two calls. Before SM.14 a freed lane kept its
//                                          vocoder running, i.e. this cost the "complex 16v" row.
// SM.6 (the granular atom, pq/dsp/granular.h), the same slot, 64-frame blocks, two 8-lane groups (16 voices):
//   granular/loop 16v×16g     Loop, 16 grains per voice live (density 200 Hz × 80 ms), rate 1.5: ns/frame of all 16
//   granular/cloud 16v×16g    Cloud with sprays (time, pitch ± 3 st, pan)
//   granular/loop 16v×4g      Loop, 4 grains per voice live (cost ∝ live grains)
//   granular/silence 16v      no voice triggered (the silent path)
// % of a core = ns/frame × 48000 / 1e7.
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/granular.h"
#include "pq/dsp/sample_slot.h"
#include "pq/dsp/sampler_stretch.h"
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

std::unique_ptr<e::AudioSequenceImage> image(pq::dsp::AudioWarp warp, double ratio, bool markers, const Source& src,
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
    d.stretch.flux = warp == pq::dsp::AudioWarp::Texture ? 40.0f : 0.0f;
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
  const auto img = image(pq::dsp::AudioWarp::Complex, 1.0, false, src, clips);
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

void voiceBench(pq::bench::Context& c, pq::dsp::AudioWarp warp, double ratio, bool markers, uint32_t channels = 2) {
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

// SM.4: sampler_warp per voice (see the top).
struct SamplerSource {
  pq::EpochReclaimer reclaimer{1};
  pq::dsp::SampleSlot slot;
  SamplerSource() {
    std::vector<float> l(static_cast<size_t>(kFrames)), r(l.size());
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-0.3f, 0.3f);
    for (size_t i = 0; i < l.size(); ++i) {
      const double ph = 2.0 * 3.141592653589793 * 196.0 * static_cast<double>(i) / kSr;
      l[i] = u(rng) + 0.4f * static_cast<float>(std::sin(ph));
      r[i] = u(rng) + 0.4f * static_cast<float>(std::sin(ph + 1.0));
    }
    std::vector<float> slices;
    for (int64_t f = 0; f < kFrames; f += 6000) slices.push_back(static_cast<float>(f));  // 1/16 at 120 BPM
    auto data = pq::dsp::SampleData::build(l, r, 3).value();
    (void)slot.publish(pq::dsp::SampleData::source(data, kSr, slices), kSr, reclaimer);
  }
};

void samplerWarpBench(pq::bench::Context& c, pq::dsp::AudioWarp warp, float rate, bool glide, uint32_t lanes, uint32_t groups = 1,
                      uint32_t measuredMask = 0xFFu) {
  static SamplerSource src;
  constexpr uint32_t kN = 64, kIn = 16;
  std::vector<pq::AlignedBuffer<float>> in;
  for (uint32_t i = 0; i < kIn; ++i) in.emplace_back(static_cast<size_t>(kN) * 8);
  const auto mode = static_cast<float>(warp);  // the atom's mode input is the enum's value (pq/dsp/warp.h)
  const float vals[kIn] = {rate, 0.0f, 0.05f, 1.0f, 0.1f, 0.9f, 97.0f, 120.0f, mode, 0.03f,
                           warp == pq::dsp::AudioWarp::Texture ? 40.0f : 0.0f,
                           1.0f, 100.0f, static_cast<float>(kSr), 100.0f, 128.0f};
  for (uint32_t i = 0; i < kIn; ++i)
    for (uint32_t k = 0; k < kN * 8; ++k) in[i][k] = vals[i];
  const uint32_t mask = (glide ? 0u : 1u) | 2u | pq::dsp::kernelInfo(pq::dsp::Atom::SamplerWarp).splRequired;
  std::vector<pq::AlignedBuffer<float>> out;
  for (uint32_t o = 0; o < 4; ++o) out.emplace_back(static_cast<size_t>(kN) * 8);
  const float* ip[kIn];
  for (uint32_t i = 0; i < kIn; ++i) ip[i] = in[i].data();
  float* op[4] = {out[0].data(), out[1].data(), out[2].data(), out[3].data()};
  const bool complex = pq::dsp::warpComplex(warp);
  if (complex) src.slot.setStretch(pq::dsp::SamplerStretch::build(groups * 8, 2, kSr), src.reclaimer);
  std::vector<pq::AlignedBuffer<float>> states;
  const pq::dsp::KernelFn fn = pq::dsp::findKernel(pq::dsp::Atom::SamplerWarp, lanes, mask);
  for (uint32_t g = 0; g < groups; ++g) {
    states.emplace_back(sizeof(pq::dsp::SamplerWarpState) / sizeof(float));
    pq::dsp::StateInit init;
    init.lanes = lanes;
    init.voiceGroup = g;
    pq::dsp::initState(pq::dsp::Atom::SamplerWarp, states[g].data(), init);
  }
  pq::dsp::KernelArgs args{ip, op, states[0].data(), &src.slot, 1, kIn, 4, mask};
  auto runGroups = [&] {
    for (uint32_t g = 0; g < groups; ++g) {
      args.state = states[g].data();
      fn(args);
    }
  };
  // Trigger every lane (a one-frame call with reset high), then sustain.
  for (uint32_t k = 0; k < 8; ++k) in[1][k] = pq::dsp::boolMask(true);
  runGroups();
  for (uint32_t k = 0; k < 8; ++k) in[1][k] = pq::dsp::boolMask(false);
  args.frames = kN;
  uint64_t calls = 0;
  if (complex)
    for (uint32_t b = 0; b < 48000 / kN; ++b) runGroups();  // 1 s: every voice primed and crossed to the vocoder
  args.laneMask = measuredMask;  // SM.14: the occupied lanes while measured (all: the default)
  c.measure(
      [&] {
        if (glide) {  // per-sample rates: a ramp 1 → 1.5 over 1 s, repeated
          for (uint32_t f = 0; f < kN; ++f) {
            const float x = 1.0f + 0.5f * static_cast<float>((calls * kN + f) % 48000) / 48000.0f;
            for (uint32_t l = 0; l < lanes; ++l) in[0][f * lanes + l] = x * (1.0f + 0.01f * static_cast<float>(l));
          }
        }
        runGroups();
        ++calls;
        pq::bench::doNotOptimize(out[0][kN * lanes - 1]);
      },
      kN, groups > 1 ? "ns/frame(16 voices)" : lanes == 1 ? "ns/frame" : "ns/frame(8 voices)", lanes == 1 ? "L1" : "L8");
  if (complex) src.slot.setStretch(nullptr, src.reclaimer);
}

// SM.6: granular, `groups` 8-lane groups, every lane triggered (unless silent), `live` grains per voice.
void granularBench(pq::bench::Context& c, float mode, uint32_t live, bool spray, bool silent) {
  static SamplerSource src;
  constexpr uint32_t kN = 64, kIn = 19, kGroups = 2;
  std::vector<pq::AlignedBuffer<float>> in;
  for (uint32_t i = 0; i < kIn; ++i) in.emplace_back(static_cast<size_t>(kN) * 8);
  const float density = 200.0f, size = static_cast<float>(live) / density;
  const float vals[kIn] = {392.4f, 0.0f, 261.63f, 1.0f, 0.05f, 0.95f, 0.2f, 1.0f, size, density, spray ? 0.02f : 0.0f,
                           spray ? 3.0f : 0.0f, spray ? 1.0f : 0.0f, 0.0f, 0.0f, mode, 32.0f, 0.0f, static_cast<float>(kSr)};
  for (uint32_t i = 0; i < kIn; ++i)
    for (uint32_t k = 0; k < kN * 8; ++k) in[i][k] = vals[i];
  const uint32_t mask = pq::dsp::kernelInfo(pq::dsp::Atom::Granular).splRequired;  // hz, reset per sample
  std::vector<pq::AlignedBuffer<float>> out;
  for (uint32_t o = 0; o < 4; ++o) out.emplace_back(static_cast<size_t>(kN) * 8);
  const float* ip[kIn];
  for (uint32_t i = 0; i < kIn; ++i) ip[i] = in[i].data();
  float* op[4] = {out[0].data(), out[1].data(), out[2].data(), out[3].data()};
  std::vector<pq::AlignedBuffer<float>> states;
  for (uint32_t g = 0; g < kGroups; ++g) {
    states.emplace_back(sizeof(pq::dsp::GranularState) / sizeof(float));
    pq::dsp::StateInit init;
    init.lanes = 8;
    init.voiceGroup = g;
    init.seed = 7;
    pq::dsp::initState(pq::dsp::Atom::Granular, states[g].data(), init);
  }
  const pq::dsp::KernelFn fn = pq::dsp::findKernel(pq::dsp::Atom::Granular, 8, mask);
  pq::dsp::KernelArgs args{ip, op, states[0].data(), &src.slot, kN, kIn, 4, mask};
  auto runGroups = [&] {
    for (uint32_t g = 0; g < kGroups; ++g) {
      args.state = states[g].data();
      fn(args);
    }
  };
  if (!silent) {
    for (uint32_t k = 0; k < 8; ++k) in[1][k] = pq::dsp::boolMask(true);  // a trigger at frame 0 of every lane
    runGroups();
    for (uint32_t k = 0; k < 8; ++k) in[1][k] = pq::dsp::boolMask(false);
    for (uint32_t b = 0; b < 48000 / kN / 4; ++b) runGroups();  // 250 ms: the grain population settled
  }
  c.measure(
      [&] {
        runGroups();
        pq::bench::doNotOptimize(out[0][kN * 8 - 1]);
      },
      kN, "ns/frame(16 voices)", "L8");
}

}  // namespace

void addWarpBenches(pq::bench::Runner& runner) {
  runner.add("granular/loop 16v×16g", [](pq::bench::Context& c) { granularBench(c, 1.0f, 16, false, false); });
  runner.add("granular/cloud 16v×16g", [](pq::bench::Context& c) { granularBench(c, 2.0f, 16, true, false); });
  runner.add("granular/loop 16v×4g", [](pq::bench::Context& c) { granularBench(c, 1.0f, 4, false, false); });
  runner.add("granular/silence 16v", [](pq::bench::Context& c) { granularBench(c, 1.0f, 16, false, true); });
  for (const uint32_t lanes : {1u, 8u}) {
    const std::string l = lanes == 1 ? " L1" : " L8";
    runner.add("sampler_warp/beats" + l, [lanes](pq::bench::Context& c) { samplerWarpBench(c, pq::dsp::AudioWarp::Beats, 1.0f, false, lanes); });
    runner.add("sampler_warp/tones" + l, [lanes](pq::bench::Context& c) { samplerWarpBench(c, pq::dsp::AudioWarp::Tones, 1.0f, false, lanes); });
  }
  runner.add("sampler_warp/tones +3 st L8",
             [](pq::bench::Context& c) { samplerWarpBench(c, pq::dsp::AudioWarp::Tones, static_cast<float>(std::exp2(3.0 / 12.0)), false, 8); });
  runner.add("sampler_warp/tones glide L8", [](pq::bench::Context& c) { samplerWarpBench(c, pq::dsp::AudioWarp::Tones, 1.0f, true, 8); });
  runner.add("sampler_warp/texture L8", [](pq::bench::Context& c) { samplerWarpBench(c, pq::dsp::AudioWarp::Texture, 1.0f, false, 8); });
  const auto fifth31 = static_cast<float>(std::exp2(18.0 / 31.0));
  runner.add("sampler_warp/complex 16v", [](pq::bench::Context& c) { samplerWarpBench(c, pq::dsp::AudioWarp::Complex, 1.0f, false, 8, 2); });
  runner.add("sampler_warp/complex +18/31 16v",
             [fifth31](pq::bench::Context& c) { samplerWarpBench(c, pq::dsp::AudioWarp::Complex, fifth31, false, 8, 2); });
  runner.add("sampler_warp/complexPro 16v",
             [fifth31](pq::bench::Context& c) { samplerWarpBench(c, pq::dsp::AudioWarp::ComplexPro, fifth31, false, 8, 2); });
  runner.add("sampler_warp/complex 16v released 14",
             [](pq::bench::Context& c) { samplerWarpBench(c, pq::dsp::AudioWarp::Complex, 1.0f, false, 8, 2, 0x01u); });
  runner.add("warp/beats", [](pq::bench::Context& c) { voiceBench(c, pq::dsp::AudioWarp::Beats, 1.0, false); });
  runner.add("warp/beats +3 st", [](pq::bench::Context& c) { voiceBench(c, pq::dsp::AudioWarp::Beats, std::exp2(3.0 / 12.0), false); });
  runner.add("warp/tones", [](pq::bench::Context& c) { voiceBench(c, pq::dsp::AudioWarp::Tones, 1.0, false); });
  runner.add("warp/texture", [](pq::bench::Context& c) { voiceBench(c, pq::dsp::AudioWarp::Texture, 1.0, false); });
  runner.add("warp/repitch markers", [](pq::bench::Context& c) { voiceBench(c, pq::dsp::AudioWarp::RePitch, 1.0, true); });
  runner.add("warp/complex", [](pq::bench::Context& c) { voiceBench(c, pq::dsp::AudioWarp::Complex, 1.0, false); });
  runner.add("warp/complex +3 st", [](pq::bench::Context& c) { voiceBench(c, pq::dsp::AudioWarp::Complex, std::exp2(3.0 / 12.0), false); });
  runner.add("warp/complexPro", [](pq::bench::Context& c) { voiceBench(c, pq::dsp::AudioWarp::ComplexPro, std::exp2(3.0 / 12.0), false); });
  runner.add("warp/complex mono", [](pq::bench::Context& c) { voiceBench(c, pq::dsp::AudioWarp::Complex, 1.0, false, 1); });
  runner.add("warp/complex start", [](pq::bench::Context& c) {
    startBench(c, 1);
    startBench(c, 4);
  });
}
