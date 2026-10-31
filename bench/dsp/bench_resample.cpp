// T5.4: the clip player's resampler (pq/dsp/resample.h), ns per output frame (stereo unless named), 64-frame calls
// reading a stereo noise buffer, the envelope fused (a constant gain; `env` = a per-frame envelope buffer):
//   resample/copy                 gainCopy at rate 1 from an integer position (the clip player's straight-copy path)
//   resample/copy reverse         the same, reading backwards
//   resample/sinc rate R          sincResample at R source frames per output frame (q = 256 up to 1, stretched above)
//   resample/sinc rate 1.06 mono  one input channel added to both outputs
//   resample/sinc rate 1.06 env   with a per-frame envelope
//   resample/draft rate 0.92      the 16-tap browser-scrub quality
//   resample/mip rate R           T5.x-perf-audio (Q151 item 1): a read at R > 2 as the clip player does it on a resident
//                                 sample with mips: on level L = mipLevelFor(R) at R / 2^L (ns per output frame)
//   resample/decimate2            building a mip level: ns per output frame and channel (off the audio thread)
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/resample.h"
#include "pq_bench.h"

namespace {

constexpr uint32_t kOut = 64;
constexpr uint32_t kIn = 1u << 16;

struct Buffers {
  pq::AlignedBuffer<float> in[2];
  pq::AlignedBuffer<float> out[2];
  pq::AlignedBuffer<float> env;
  Buffers() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (auto& b : in) {
      b = pq::AlignedBuffer<float>(kIn);
      for (uint32_t i = 0; i < kIn; ++i) b[i] = u(rng);
    }
    for (auto& b : out) b = pq::AlignedBuffer<float>(kOut);
    env = pq::AlignedBuffer<float>(kOut);
    for (uint32_t i = 0; i < kOut; ++i) env[i] = 0.5f + 0.5f * static_cast<float>(i) / kOut;
  }
};

}  // namespace

void addResampleBenches(pq::bench::Runner& runner) {
  static Buffers b;
  using pq::dsp::ResampleQuality;
  auto sinc = [](const std::string& name, ResampleQuality quality, double rate, uint32_t channels, bool env) {
    return [=](pq::bench::Context& c) {
      const pq::dsp::SincTable& t = pq::dsp::prepareSincTable(quality, pq::dsp::sincStretchQ(rate));
      const auto step = static_cast<int64_t>(std::llround(rate * 4294967296.0));
      const float* in[2] = {b.in[0].data(), b.in[1].data()};
      float* out[2] = {b.out[0].data(), b.out[1].data()};
      int64_t pos = (int64_t{4096} << 32) + 0x3456789A;  // fractional start
      const int64_t wrapAt = static_cast<int64_t>(kIn - 4096) << 32;
      c.measure(
          [&] {
            pq::dsp::sincResample(t, in, channels, pos, step, kOut, env ? b.env.data() : nullptr, 0.7f, out);
            pos += step * kOut;
            if (pos > wrapAt) pos = (int64_t{4096} << 32) + (pos & 0xFFFFFFFF);
            pq::bench::doNotOptimize(out[0][kOut - 1]);
          },
          kOut, "ns/frame");
      (void)name;
    };
  };
  runner.add("resample/copy", [](pq::bench::Context& c) {
    const float* in[2] = {b.in[0].data(), b.in[1].data()};
    float* out[2] = {b.out[0].data(), b.out[1].data()};
    int64_t first = 4096;
    c.measure(
        [&] {
          pq::dsp::gainCopy(in, 2, first, 1, kOut, nullptr, 0.7f, out);
          first = first + kOut < kIn - 4096 ? first + kOut : 4096;
          pq::bench::doNotOptimize(out[0][kOut - 1]);
        },
        kOut, "ns/frame");
  });
  runner.add("resample/copy reverse", [](pq::bench::Context& c) {
    const float* in[2] = {b.in[0].data(), b.in[1].data()};
    float* out[2] = {b.out[0].data(), b.out[1].data()};
    int64_t first = kIn - 4096;
    c.measure(
        [&] {
          pq::dsp::gainCopy(in, 2, first, -1, kOut, nullptr, 0.7f, out);
          first = first - kOut > 4096 ? first - kOut : kIn - 4096;
          pq::bench::doNotOptimize(out[0][kOut - 1]);
        },
        kOut, "ns/frame");
  });
  for (double r : {0.25, 0.5, 0.91875, 1.0, 1.0594630943592953, 1.4983070768766815, 2.0, 4.0, 16.0}) {
    char name[64];
    std::snprintf(name, sizeof name, "resample/sinc rate %.4g", r);
    runner.add(name, sinc(name, ResampleQuality::High, r, 2, false));
  }
  runner.add("resample/sinc rate 1.06 mono", sinc("", ResampleQuality::High, 1.0594630943592953, 1, false));
  runner.add("resample/sinc rate 1.06 env", sinc("", ResampleQuality::High, 1.0594630943592953, 2, true));
  runner.add("resample/sinc rate 0.92 reverse", [](pq::bench::Context& c) {
    const pq::dsp::SincTable& t = pq::dsp::prepareSincTable(ResampleQuality::High, 256);
    const auto step = -static_cast<int64_t>(std::llround(0.91875 * 4294967296.0));
    const float* in[2] = {b.in[0].data(), b.in[1].data()};
    float* out[2] = {b.out[0].data(), b.out[1].data()};
    int64_t pos = (static_cast<int64_t>(kIn - 4096) << 32) + 0x3456789A;
    c.measure(
        [&] {
          pq::dsp::sincResample(t, in, 2, pos, step, kOut, nullptr, 0.7f, out);
          pos += step * kOut;
          if (pos < (int64_t{4096} << 32)) pos = (static_cast<int64_t>(kIn - 4096) << 32) + (pos & 0xFFFFFFFF);
          pq::bench::doNotOptimize(out[0][kOut - 1]);
        },
        kOut, "ns/frame");
  });
  runner.add("resample/draft rate 0.92", sinc("", ResampleQuality::Draft, 0.91875, 2, false));
  for (double r : {2.8284271247461903, 4.0, 8.0, 16.0}) {
    char name[64];
    std::snprintf(name, sizeof name, "resample/mip rate %.4g", r);
    runner.add(name, sinc(name, ResampleQuality::High, std::ldexp(r, -static_cast<int>(pq::dsp::mipLevelFor(r, pq::dsp::kMaxMipLevels))), 2, false));
  }
  runner.add("resample/decimate2", [](pq::bench::Context& c) {
    constexpr int64_t kM = 1024;
    std::vector<float> y(kM);
    int64_t m = 2048;
    c.measure(
        [&] {
          pq::dsp::decimate2(b.in[0].data(), 0, kIn, y.data(), m, kM);
          m = m + 2 * kM < kIn / 2 - 2048 ? m + kM : 2048;
          pq::bench::doNotOptimize(y[kM - 1]);
        },
        kM, "ns/frame");
  });
}
