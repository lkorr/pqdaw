// OP.6 (OP.2 a): one oscilloscope frame (dsp::ScopeTracker::frame, the UI thread's work per painted scope while its
// tap moves): the newest `need` samples of a 48 kHz signal, a frame every 800 samples (60 fps), PQ Meter's default
// settings. ns per frame:
//   scope/frame tone     a bright 110 Hz tone: one detection window, its transform length stays
//   scope/frame noise    nothing pitched (noise; a chord or drums read the same): the detector runs on its whole
//                        window and again on a third of it, two padded lengths per frame. Each used to rebuild the
//                        tracker's single cached FFT (its sin / cos tables); it keeps one per length now.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "pq/dsp/scope.h"
#include "pq_bench.h"

namespace {
constexpr double kSr = 48000.0;
constexpr size_t kHop = 800, kLen = size_t{1} << 18;
}  // namespace

void addScopeBenches(pq::bench::Runner& runner) {
  runner.add("scope/frame", [](pq::bench::Context& ctx) {
    for (const bool noise : {false, true}) {
      std::vector<float> x(kLen);
      uint64_t s = 0x9E3779B97F4A7C15ull;
      for (size_t i = 0; i < kLen; ++i) {
        if (noise) {
          s ^= s << 13;
          s ^= s >> 7;
          s ^= s << 17;
          x[i] = 0.5f * (static_cast<float>(s >> 40) / static_cast<float>(1u << 23) - 1.0f);
        } else {
          const double ph = 110.0 * static_cast<double>(i) / kSr;
          x[i] = static_cast<float>(0.4 * std::sin(6.283185307179586 * ph) + 0.2 * std::sin(6.283185307179586 * 3.0 * ph));
        }
      }
      pq::dsp::ScopeTracker t;
      t.setSettings(pq::dsp::ScopeSettings{});
      size_t end = kLen / 2;
      ctx.measure([&] {
        const size_t need = std::min(t.need(kSr), end);
        const pq::dsp::ScopeFrame f = t.frame(std::span<const float>(x.data() + (end - need), need), kSr, 0.0f, false);
        pq::bench::doNotOptimize(f.start);
        end += kHop;
        if (end > kLen) end = kLen / 2;
      }, 1, "ns/frame", noise ? "noise" : "tone");
    }
  });
}
