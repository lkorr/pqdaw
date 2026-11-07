// TX.spectrum: an analysis feed made from a fixed test signal (makeAnalysedFeed), for the scenes and tests. (GU.10: the
// stock device panel scenes it was written for — device-utility … device-auto-filter — are retired with the devices.)

#include <cmath>
#include <complex>
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

#include "pq/views/devices/device_view.h"
#include "pq/views/shell/shell.h"

namespace pq::views::devices {

namespace {

// A music-like test signal: harmonics of 110 Hz falling with frequency over pink-ish noise (deterministic).
std::vector<float> testSignal(uint32_t n, double sr) {
  std::vector<float> x(n);
  uint32_t seed = 0x1234567u;
  float b0 = 0, b1 = 0, b2 = 0;
  constexpr double kPi = 3.14159265358979323846;
  for (uint32_t i = 0; i < n; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const float w = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
    // Paul Kellet's economy pink filter.
    b0 = 0.99765f * b0 + w * 0.0990460f;
    b1 = 0.96300f * b1 + w * 0.2965164f;
    b2 = 0.57000f * b2 + w * 1.0526913f;
    double v = 0.05 * (b0 + b1 + b2 + w * 0.1848f);
    for (int h = 1; h <= 24; ++h) v += 0.25 / h * std::sin(2.0 * kPi * 110.0 * h * i / sr + 0.7 * h);
    x[i] = static_cast<float>(0.5 * v);
  }
  return x;
}

}  // namespace

// The test signal through a zero-phase FIR whose magnitude is `mag` (frequency sampling, 2049 taps, Hann).
AnalysedFeed makeAnalysedFeed(const std::function<double(double)>& mag, bool response) {
  dsp::SpectrumConfig sc;
  sc.peakHold = true;
  constexpr int kHalf = 1024, kM = 8192;
  constexpr double kPi = 3.14159265358979323846;
  std::vector<double> H(kM / 2 + 1);
  for (int k = 0; k <= kM / 2; ++k) H[k] = mag(static_cast<double>(k) * sc.sampleRate / kM);
  std::vector<float> h(2 * kHalf + 1);
  for (int n = -kHalf; n <= kHalf; ++n) {
    double v = H[0] + H[kM / 2] * std::cos(kPi * n);
    for (int k = 1; k < kM / 2; ++k) v += 2.0 * H[k] * std::cos(2.0 * kPi * k * n / kM);
    const double w = 0.5 * (1.0 + std::cos(kPi * n / (kHalf + 1)));
    h[n + kHalf] = static_cast<float>(v / kM * w);
  }
  constexpr uint32_t kFrames = 98304;  // 2 s: the long (bass) window fills and averages
  const std::vector<float> x = testSignal(kFrames + 2 * kHalf, sc.sampleRate);
  std::vector<float> in(kFrames), out(kFrames);
  for (uint32_t i = 0; i < kFrames; ++i) {
    in[i] = x[i + kHalf];
    float acc = 0.0f;
    for (int j = 0; j <= 2 * kHalf; ++j) acc += h[j] * x[i + 2 * kHalf - j];
    out[i] = acc;
  }
  auto tap = std::make_unique<TapRing>(4, 32768, 4096);
  auto feed = std::make_unique<SpectrumFeed>(*tap, sc, SpectrumFeed::Manual{});
  const uint32_t wants = kWantOutput | (response ? kWantResponse : 0u);
  feed->acquire(wants);
  for (uint32_t i = 0; i < kFrames; i += 4096) {
    const float* ch[4] = {in.data() + i, in.data() + i, out.data() + i, out.data() + i};
    tap->write(ch, 4, std::min<uint32_t>(4096, kFrames - i));
    feed->step(1.0 + i / sc.sampleRate);
  }
  feed->hold();
  feed->release(wants);
  return AnalysedFeed{std::move(tap), std::move(feed)};
}

}  // namespace pq::views::devices
