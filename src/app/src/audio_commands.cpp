// Audio commands of pqdaw (T0.6): --list-devices and --sine. Parsing lives in cli.cpp.
#include <cstdint>

#include "pq/app/commands.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "pq/core/fast_math.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/rtaudio_device.h"

namespace pq::app {

static_assert(CliOptions{}.device == engine::kDefaultDevice, "CliOptions::device default must mean 'default device'");

namespace {

using engine::Backend;

// Test tone: uint32 phase accumulator (SPEC §3.1 / CLAUDE.md rule 3). The phase read as int32 maps to
// [-π, π) radians, inside fastSin's domain; one fastSin (~1.5 ns) per sample, copied to the other channels.
struct SineEngine {
  uint32_t phase = 0;
  uint32_t increment = 0;
  float amplitude = 0;

  static void process(void* user, const engine::AudioBlock& b) noexcept {  // RT-safe
    auto& s = *static_cast<SineEngine*>(user);
    constexpr float kPhaseToRadians = 6.28318530717958647692f / 4294967296.0f;
    float* out0 = b.out[0];
    uint32_t ph = s.phase;
    for (uint32_t i = 0; i < b.frames; ++i) {
      out0[i] = s.amplitude * fastSin(static_cast<float>(static_cast<int32_t>(ph)) * kPhaseToRadians);
      ph += s.increment;
    }
    s.phase = ph;
    for (uint32_t c = 1; c < b.numOut; ++c) std::memcpy(b.out[c], out0, sizeof(float) * b.frames);
  }
};

void printRates(const engine::DeviceInfo& d) {
  std::printf("        rates:");
  for (uint32_t r : d.sampleRates) std::printf(" %u", r);
  std::printf("\n");
}

}  // namespace

int runListDevices(const CliOptions& options) {
  int failures = 0;
  for (Backend b : {Backend::Wasapi, Backend::Asio}) {
    if (b == Backend::Asio && !options.asio && !engine::backendCompiled(Backend::Asio)) {
      std::printf("ASIO: not compiled in (PQ_WITH_ASIO=OFF; see docs/QUESTIONS.md, T0.6)\n");
      continue;
    }
    auto dev = engine::RtAudioDevice::create(b);
    if (!dev) {
      std::printf("%s: %s\n", engine::backendName(b), dev.error().message.c_str());
      ++failures;
      continue;
    }
    auto list = (*dev)->enumerate();
    if (!list) {
      std::printf("%s: %s\n", engine::backendName(b), list.error().message.c_str());
      ++failures;
      continue;
    }
    std::printf("%s%s: %zu device(s)\n", engine::backendName(b),
                b == Backend::Wasapi ? " (shared mode; exclusive mode is not available, docs/QUESTIONS.md, T0.6)" : "",
                list->size());
    std::printf("  %5s %4s %4s %7s  %-8s %s\n", "id", "out", "in", "rate", "default", "name");
    for (const engine::DeviceInfo& d : *list) {
      const char* def = d.isDefaultOutput && d.isDefaultInput ? "out+in"
                        : d.isDefaultOutput                   ? "out"
                        : d.isDefaultInput                    ? "in"
                                                              : "";
      std::printf("  %5u %4u %4u %7u  %-8s %s\n", d.id, d.outputChannels, d.inputChannels, d.preferredSampleRate,
                  def, d.name.c_str());
      printRates(d);
    }
    const std::string msg = (*dev)->lastBackendMessage();
    if (!msg.empty()) std::printf("  (backend message: %s)\n", msg.c_str());
  }
  return failures ? 1 : 0;
}

int runSine(const CliOptions& options) {
  const Backend backend = options.asio ? Backend::Asio : Backend::Wasapi;
  auto created = engine::RtAudioDevice::create(backend);
  if (!created) {
    std::fprintf(stderr, "pqdaw: %s\n", created.error().message.c_str());
    return 1;
  }
  engine::RtAudioDevice& dev = **created;

  engine::StreamConfig config;
  config.outputDevice = options.device;
  config.outputChannels = 2;
  config.inputChannels = 0;
  config.sampleRate = options.sampleRate;
  config.bufferFrames = options.bufferFrames;
  config.shareMode = options.exclusive ? engine::ShareMode::Exclusive : engine::ShareMode::Shared;

  // The device calls the adapter, which calls the engine in fixed blocks. Both outlive the stream.
  engine::BlockAdapter adapter;
  SineEngine sine;
  auto opened = dev.open(config, adapter.deviceCallback());
  if (!opened) {
    std::fprintf(stderr, "pqdaw: %s\n", opened.error().message.c_str());
    return 1;
  }
  const engine::StreamInfo& info = *opened;
  if (!adapter.prepare({&SineEngine::process, &sine}, info.inputChannels, info.outputChannels, options.blockFrames,
                       info.bufferFrames)) {
    std::fprintf(stderr, "pqdaw: invalid internal block size\n");
    return 1;
  }
  sine.increment = static_cast<uint32_t>(std::llround(options.sineHz / info.sampleRate * 4294967296.0) & 0xFFFFFFFFll);
  sine.amplitude = static_cast<float>(std::pow(10.0, -12.0 / 20.0));

  std::printf("device:   %s [%s, id %s]\n", info.outputDeviceName.c_str(), engine::backendName(info.backend),
              options.device == engine::kDefaultDevice ? "default" : std::to_string(options.device).c_str());
  std::printf("stream:   %u Hz, %u ch out, device buffer %u frames (%.3f ms), backend latency %u frames\n",
              info.sampleRate, info.outputChannels, info.bufferFrames, 1e3 * info.bufferFrames / info.sampleRate,
              info.latencyFrames);
  std::printf("engine:   internal block %u frames, adapter %s (+%u frames latency)\n", adapter.blockFrames(),
              adapter.directMode() ? "direct" : "FIFO", adapter.latencyFrames());
  std::printf("signal:   %.3f Hz sine at -12 dBFS for %.3f s\n", options.sineHz, options.seconds);
  if (info.backendResampling)
    std::printf("warning:  %u Hz differs from the device mix rate: RtAudio resamples on the audio thread "
                "(allocates; not RT-safe)\n",
                info.sampleRate);

  if (auto s = dev.start(); !s) {
    std::fprintf(stderr, "pqdaw: %s\n", s.error().message.c_str());
    return 1;
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::duration<double>(options.seconds));
  const auto stopStatus = dev.stop();
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const engine::CallbackStats::Snapshot st = dev.stats().snapshot();
  dev.close();
  if (!stopStatus) std::fprintf(stderr, "pqdaw: %s\n", stopStatus.error().message.c_str());

  std::printf("ran:      %.3f s wall clock\n", elapsed);
  std::printf("callbacks: %llu (%llu frames = %.3f s of audio; engine blocks %lld)\n",
              static_cast<unsigned long long>(st.callbacks), static_cast<unsigned long long>(st.frames),
              static_cast<double>(st.frames) / info.sampleRate,
              static_cast<long long>(adapter.engineFrames() / adapter.blockFrames()));
  // The device consumes sampleRate frames per second once running, so frames delivered over the span between the
  // first and last callback (+ the last buffer) should match it. A shortfall beyond a few buffers means output
  // went missing even when the backend reported no underflow.
  const double expectedFrames = st.activeSeconds * info.sampleRate + info.bufferFrames;
  std::printf("timing:   %.3f s from first to last callback; delivered %.0f frames vs %.0f consumed by the "
              "device clock in that span (%+.1f ms)\n",
              st.activeSeconds, static_cast<double>(st.frames), expectedFrames,
              1e3 * (static_cast<double>(st.frames) - expectedFrames) / info.sampleRate);
  std::printf("xruns:    %llu (input overflows %llu, output underflows %llu, as reported by the backend)\n",
              static_cast<unsigned long long>(st.xruns()), static_cast<unsigned long long>(st.inputOverflows),
              static_cast<unsigned long long>(st.outputUnderflows));
  std::printf("deadline misses: %llu (callbacks longer than the %.1f us buffer period)\n",
              static_cast<unsigned long long>(st.deadlineMisses), st.periodUs);
  std::printf("callback time: mean %.2f us, max %.2f us; max interval between callbacks %.1f us%s\n",
              st.meanDurationUs, st.maxDurationUs, st.maxIntervalUs,
              st.timingReliable ? "" : " (TSC not invariant: timing approximate)");
  if (info.backend == Backend::Wasapi)
    std::printf("note:     RtAudio's WASAPI backend never reports output underflows (docs/QUESTIONS.md, T0.6)\n");
  return (st.xruns() || st.deadlineMisses || !stopStatus) ? 3 : 0;
}

}  // namespace pq::app
