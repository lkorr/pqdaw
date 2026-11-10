// M6 review (recording, WASAPI shared capture): what does CaptureFifo do to a recorded input when the capture and
// render clocks differ (two devices, e.g. a USB mic and the laptop's speakers)? Packets of 480 frames (10 ms) are pushed
// at the capture clock, periods of 480 pulled at the render clock, with the same target / high WasapiDevice::Impl::
// openCapture uses (target = capPeriod + period, high = target + capPeriod + period). The pushed signal is a frame
// counter; every pulled frame that is not the previous + 1 is a discontinuity in what the recorder captures.
#include <cmath>
#include <cstdio>
#include <vector>

#include "pq/engine/capture_fifo.h"

using namespace pq::engine;

int main() {
  const uint32_t period = 480, capPeriod = 480, capBuffer = 4800;
  const double rate = 48000.0;
  const double ppms[] = {+20.0, -20.0, +100.0, -100.0};
  for (double ppm : ppms) {
    CaptureFifo fifo;
    const uint32_t target = capPeriod + period;
    fifo.prepare(1, 2 * capBuffer + 4 * (capPeriod + period), target, target + capPeriod + period, rate);
    std::vector<float> packet(capPeriod), out(period);
    float* outs[1] = {out.data()};
    double capClock = 0.0;  // capture frames produced so far (fractional: the clock ratio)
    uint64_t pushed = 0;
    const double ratio = 1.0 + ppm * 1e-6;
    const uint64_t periods = static_cast<uint64_t>(600.0 * rate / period);  // 10 minutes
    int64_t last = -1;
    uint64_t jumps = 0, silentPeriods = 0, skippedFrames = 0;
    double firstJumpS = -1.0;
    for (uint64_t k = 0; k < periods; ++k) {
      capClock += period * ratio;
      while (pushed + capPeriod <= capClock) {
        for (uint32_t i = 0; i < capPeriod; ++i) packet[i] = static_cast<float>((pushed + i) % 16777216);
        fifo.push(packet.data(), SampleFormat::Float32, 1, capPeriod, 0);
        pushed += capPeriod;
      }
      (void)fifo.pull(outs, period);
      if (!fifo.primed() && out[0] == 0.0f && out[period - 1] == 0.0f) {
        if (last >= 0) ++silentPeriods;
        continue;
      }
      for (uint32_t i = 0; i < period; ++i) {
        const int64_t v = static_cast<int64_t>(out[i]);
        const int64_t d = ((v - last - 1) % 16777216 + 16777216) % 16777216;  // the counter wraps at 2^24
        if (last >= 0 && d != 0) {
          ++jumps;
          skippedFrames += static_cast<uint64_t>(d < 8388608 ? d : 16777216 - d);
          if (firstJumpS < 0) firstJumpS = static_cast<double>(k) * period / rate;
        }
        last = v;
      }
    }
    std::printf("clock %+6.0f ppm over 10 min: %llu discontinuities (first at %.1f s), %llu frames skipped/repeated, "
                "%llu silent periods, fifo dropped %llu, underflows %llu\n",
                ppm, static_cast<unsigned long long>(jumps), firstJumpS, static_cast<unsigned long long>(skippedFrames),
                static_cast<unsigned long long>(silentPeriods), static_cast<unsigned long long>(fifo.dropped()),
                static_cast<unsigned long long>(fifo.underflows()));
  }
  return 0;
}
