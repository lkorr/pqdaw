// Overhead of the audio callback path (T0.6): CallbackRunner (RtScope + DenormalGuard + 2 rdtsc + stats) and the
// BlockAdapter in direct and FIFO modes. The engine is a minimal kernel (copy in → out) so the numbers are the
// path's own cost. Run from the release build.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pq_bench.h"
#include "pq/engine/asio_abi.h"
#include "pq/engine/asio_convert.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/null_device.h"
#include "pq/engine/sample_convert.h"

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

// WASAPI device-buffer conversion (T0.6-fix1): planar float → interleaved device format, one pass per period.
// Stereo uses the AVX2 path; 3 channels uses the scalar generic path (for comparison).
void benchConvert(SampleFormat f, uint32_t channels, uint32_t frames) {
  std::vector<std::vector<float>> in(channels, std::vector<float>(frames));
  std::vector<const float*> ptrs;
  for (uint32_t c = 0; c < channels; ++c) {
    for (uint32_t i = 0; i < frames; ++i) in[c][i] = 0.9f * static_cast<float>((i * 7 + c * 13) % 101) / 101.0f - 0.45f;
    ptrs.push_back(in[c].data());
  }
  std::vector<uint8_t> out(static_cast<size_t>(frames) * channels * 4 + 64);
  const auto s = pq::bench::measure(
      [&] {
        planarToInterleaved(ptrs.data(), channels, channels, frames, f, out.data());
        pq::bench::doNotOptimize(out[5]);
      },
      1);
  char name[96];
  std::snprintf(name, sizeof name, "convert %s %uch %u frames", sampleFormatName(f), channels, frames);
  pq::bench::report(name, s, "ns/call");
  const pq::bench::Stats perFrame{s.medianNs / frames, s.minNs / frames};
  pq::bench::report(name, perFrame, "ns/frame");
}

// ASIO channel conversion (T0.6-fix2): one channel buffer per call, both directions, as the host does per channel
// per buffer switch (float32 channels are zero-copy and need no call at all).
void benchAsio(int32_t type, uint32_t frames) {
  std::vector<float> in(frames), back(frames);
  for (uint32_t i = 0; i < frames; ++i) in[i] = 0.9f * static_cast<float>((i * 7) % 101) / 101.0f - 0.45f;
  std::vector<uint8_t> dev(static_cast<size_t>(frames) * 8 + 64);
  const ToDeviceFn to = asioToDeviceFn(type);
  const FromDeviceFn from = asioFromDeviceFn(type);
  char name[96];
  std::snprintf(name, sizeof name, "asio float -> %s 1ch %u frames", asioSampleTypeName(type), frames);
  pq::bench::report(name, pq::bench::measure([&] { to(in.data(), frames, dev.data()); pq::bench::doNotOptimize(dev[5]); }, 1),
                    "ns/call");
  std::snprintf(name, sizeof name, "asio %s -> float 1ch %u frames", asioSampleTypeName(type), frames);
  pq::bench::report(name, pq::bench::measure([&] { from(dev.data(), frames, back.data()); pq::bench::doNotOptimize(back[5]); }, 1),
                    "ns/call");
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
  for (SampleFormat f : {SampleFormat::Float32, SampleFormat::Int32, SampleFormat::Int24In32, SampleFormat::Int24,
                         SampleFormat::Int16})
    benchConvert(f, 2, 144);
  benchConvert(SampleFormat::Int24, 2, 480);
  benchConvert(SampleFormat::Int24, 3, 144);
  benchConvert(SampleFormat::Float32, 3, 144);
  for (int32_t t : {pq::engine::asio::kInt32Lsb, pq::engine::asio::kInt24Lsb, pq::engine::asio::kInt16Lsb,
                    pq::engine::asio::kInt32Lsb24, pq::engine::asio::kFloat64Lsb})
    benchAsio(t, 64);
  benchAsio(pq::engine::asio::kInt32Lsb, 256);
  return 0;
}
