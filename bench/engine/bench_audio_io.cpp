// Overhead of the audio callback path (T0.6): CallbackRunner (RtScope + DenormalGuard + 2 rdtsc + stats) and the
// BlockAdapter in direct and FIFO modes. The engine is a minimal kernel (copy in → out) so the numbers are the
// path's own cost. Run from the release build.
#include <cstdint>
#include <cstring>
#include <vector>

#include "pq_bench.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/null_device.h"

using namespace pq::engine;

namespace {

void copyEngine(void*, const AudioBlock& b) noexcept {
  for (uint32_t c = 0; c < b.numOut; ++c) std::memcpy(b.out[c], b.in[c], sizeof(float) * b.frames);
}

// Device-level: runner + adapter + engine, through NullDevice::processBlock (the same path RtAudio's callback
// takes after building its channel pointers).
void benchDevice(const char* name, uint32_t deviceFrames, uint32_t blockFrames, bool useAdapter) {
  NullDevice dev;
  BlockAdapter adapter;
  StreamConfig cfg;
  cfg.bufferFrames = deviceFrames;
  cfg.inputChannels = 2;
  cfg.outputChannels = 2;
  cfg.sampleRate = 48000;
  const AudioCallback cb = useAdapter ? adapter.deviceCallback() : AudioCallback{&copyEngine, nullptr};
  if (!dev.open(cfg, cb).ok()) return;
  if (useAdapter) adapter.prepare({&copyEngine, nullptr}, 2, 2, blockFrames, deviceFrames);
  for (uint32_t c = 0; c < 2; ++c)
    for (uint32_t i = 0; i < deviceFrames; ++i) dev.input(c)[i] = 0.001f * static_cast<float>(i);
  const auto s = pq::bench::measure([&] { dev.processBlock(); }, 1);
  pq::bench::report(name, s, "ns/callback");
  const pq::bench::Stats perFrame{s.medianNs / deviceFrames, s.minNs / deviceFrames};
  pq::bench::report(name, perFrame, "ns/frame");
}

}  // namespace

int main() {
  // Baseline: the engine kernel called directly, no runner.
  {
    std::vector<float> in(2 * 480, 0.5f), out(2 * 480);
    const float* ip[2] = {in.data(), in.data() + 480};
    float* op[2] = {out.data(), out.data() + 480};
    const AudioBlock b{ip, 2, op, 2, 480, 0};
    const auto s = pq::bench::measure([&] { copyEngine(nullptr, b); pq::bench::doNotOptimize(out[7]); }, 1);
    pq::bench::report("engine copy only, 480 frames 2ch", s, "ns/call");
  }
  benchDevice("runner, 480 frames, no adapter", 480, 0, false);
  benchDevice("runner, 64 frames, no adapter", 64, 0, false);
  benchDevice("runner+adapter 480/32 (direct)", 480, 32, true);
  benchDevice("runner+adapter 256/32 (direct)", 256, 32, true);
  benchDevice("runner+adapter 480/64 (FIFO)", 480, 64, true);
  benchDevice("runner+adapter 37/32 (FIFO)", 37, 32, true);
  benchDevice("runner+adapter 1/32 (FIFO)", 1, 32, true);
  return 0;
}
