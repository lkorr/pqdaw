// Audio commands of pqdaw (T0.6, T0.6-fix1): --list-devices, --sine and --latency-test. Parsing lives in cli.cpp.
#include <cstdint>

#include "pq/app/commands.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pq/core/fast_math.h"
#include "pq/core/rt_log.h"
#include "pq/engine/asio_convert.h"
#include "pq/engine/asio_device.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/rtaudio_device.h"
#include "pq/engine/wasapi_device.h"

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

Backend chosenBackend(const CliOptions& o) {
  if (o.asio) return Backend::Asio;
  return o.wasapi == WasapiImpl::Own ? Backend::Wasapi : Backend::RtAudioWasapi;
}

void printRates(const engine::DeviceInfo& d) {
  std::printf("        rates:");
  for (uint32_t r : d.sampleRates) std::printf(" %u", r);
  std::printf("\n");
}

const char* defaultMark(const engine::DeviceInfo& d) {
  return d.isDefaultOutput && d.isDefaultInput ? "out+in" : d.isDefaultOutput ? "out" : d.isDefaultInput ? "in" : "";
}

int listOwnWasapi() {
  engine::WasapiDevice dev;
  auto list = dev.enumerate();
  if (!list) {
    std::printf("WASAPI: %s\n", list.error().message.c_str());
    return 1;
  }
  std::printf("WASAPI (pqdaw backend: event-driven exclusive + shared, output only): %zu endpoint(s)\n", list->size());
  std::printf("  %5s %4s %4s %7s  %-8s %s\n", "id", "out", "in", "rate", "default", "name");
  for (const engine::DeviceInfo& d : *list) {
    std::printf("  %5u %4u %4u %7u  %-8s %s\n", d.id, d.outputChannels, d.inputChannels, d.preferredSampleRate,
                defaultMark(d), d.name.c_str());
    std::printf("        endpoint: %s\n", d.endpointId.c_str());
    if (d.outputChannels) {
      std::printf("        exclusive at %u Hz:", d.preferredSampleRate);
      if (d.exclusiveFormats.empty()) std::printf(" (no format accepted)");
      for (engine::SampleFormat f : d.exclusiveFormats) std::printf(" %s", engine::sampleFormatName(f));
      std::printf("; min period %u frames; shared engine period %u..%u frames\n", d.minPeriodFrames,
                  d.sharedMinPeriodFrames, d.sharedMaxPeriodFrames);
      printRates(d);
    }
  }
  return 0;
}

int listRtAudio(Backend b) {
  auto dev = engine::RtAudioDevice::create(b);
  if (!dev) {
    std::printf("%s: %s\n", engine::backendName(b), dev.error().message.c_str());
    return 1;
  }
  auto list = (*dev)->enumerate();
  if (!list) {
    std::printf("%s: %s\n", engine::backendName(b), list.error().message.c_str());
    return 1;
  }
  std::printf("%s%s: %zu device(s)\n", engine::backendName(b),
              b == Backend::RtAudioWasapi ? " (shared mode only, docs/QUESTIONS.md Q12)" : "", list->size());
  std::printf("  %5s %4s %4s %7s  %-8s %s\n", "id", "out", "in", "rate", "default", "name");
  for (const engine::DeviceInfo& d : *list) {
    std::printf("  %5u %4u %4u %7u  %-8s %s\n", d.id, d.outputChannels, d.inputChannels, d.preferredSampleRate,
                defaultMark(d), d.name.c_str());
    printRates(d);
  }
  const std::string msg = (*dev)->lastBackendMessage();
  if (!msg.empty()) std::printf("  (backend message: %s)\n", msg.c_str());
  return 0;
}

std::string granularityText(int32_t g) {
  if (g == -1) return "powers of 2";
  if (g == 0) return "preferred only";
  return "steps of " + std::to_string(g);
}

int listAsio() {
  engine::AsioDevice dev;
  auto list = dev.enumerate();
  if (!list) {
    std::printf("ASIO: %s\n", list.error().message.c_str());
    return 1;
  }
  std::printf("ASIO (pqdaw's own host; select with --backend asio --device <id|name>): %zu driver(s)\n", list->size());
  std::printf("  %5s %4s %4s %7s  %s\n", "id", "out", "in", "rate", "name");
  for (const engine::DeviceInfo& d : *list) {
    if (!d.probeError.empty()) {
      std::printf("  %5u %4s %4s %7s  %s\n        not available: %s\n", d.id, "-", "-", "-", d.name.c_str(),
                  d.probeError.c_str());
      continue;
    }
    std::printf("  %5u %4u %4u %7u  %s\n", d.id, d.outputChannels, d.inputChannels, d.preferredSampleRate,
                d.name.c_str());
    std::printf("        buffer %d..%d frames (%s), preferred %d; sample type %s\n", d.minBufferFrames,
                d.maxBufferFrames, granularityText(d.bufferGranularity).c_str(), d.preferredBufferFrames,
                d.sampleTypeName.c_str());
    printRates(d);
  }
  return 0;
}

// Writes silence (the --asio-panel stream never starts; this is only its callback).
struct SilenceEngine {
  static void process(void*, const engine::AudioBlock& b) noexcept {  // RT-safe
    for (uint32_t c = 0; c < b.numOut; ++c) std::memset(b.out[c], 0, sizeof(float) * b.frames);
  }
};

}  // namespace

int runListDevices(const CliOptions& options) {
  int failures = options.wasapi == WasapiImpl::Own ? listOwnWasapi() : listRtAudio(Backend::RtAudioWasapi);
  failures += listAsio();
  return failures ? 1 : 0;
}

int runAsioPanel(const CliOptions& options) {
  engine::AsioDevice dev;
  // The driver is loaded by opening a stream (not started): one output, or one input for input-only drivers.
  engine::StreamConfig config;
  config.outputDevice = options.device;
  config.outputEndpointId = options.deviceEndpoint;
  config.outputChannels = 1;
  config.inputChannels = 0;
  SilenceEngine silence;
  auto opened = dev.open(config, {&SilenceEngine::process, &silence});
  if (!opened && opened.error().code == ErrorCode::InvalidArgument) {
    config.outputChannels = 0;
    config.inputChannels = 1;
    opened = dev.open(config, {&SilenceEngine::process, &silence});
  }
  if (!opened) {
    std::fprintf(stderr, "pqdaw: %s\n", opened.error().message.c_str());
    return 1;
  }
  std::printf("driver:   %s (loaded; stream not started)\n", opened->outputDeviceName.c_str());
  if (auto s = dev.controlPanel(); !s) {
    std::fprintf(stderr, "pqdaw: %s\n", s.error().message.c_str());
    dev.close();
    return 1;
  }
  if (options.secondsGiven) {
    std::printf("panel:    opened; keeping the driver loaded for %.1f s\n", options.seconds);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::duration<double>(options.seconds));
  } else {
    std::printf("panel:    opened; press Enter to unload the driver\n");
    std::fflush(stdout);
    (void)std::getchar();
  }
  const engine::AsioEvents ev = dev.events();
  const engine::StreamInfo now = dev.streamInfo();
  std::printf("events:   %llu reset request(s), %llu rate change(s), %llu buffer-size change(s), %llu latency "
              "change(s); %llu reopen(s), %llu failed; stream now %u Hz, %u frames%s\n",
              static_cast<unsigned long long>(ev.resetRequests), static_cast<unsigned long long>(ev.rateChanges),
              static_cast<unsigned long long>(ev.bufferSizeChanges), static_cast<unsigned long long>(ev.latencyChanges),
              static_cast<unsigned long long>(ev.reopens), static_cast<unsigned long long>(ev.failedReopens),
              now.sampleRate, now.bufferFrames, dev.isOpen() ? "" : " (closed after a failed reopen)");
  if (!dev.lastError().empty()) std::printf("error:    %s\n", dev.lastError().c_str());
  dev.close();
  return 0;
}

int runSine(const CliOptions& options) {
  const Backend backend = chosenBackend(options);
  if (!options.deviceEndpoint.empty() && backend != Backend::Wasapi && backend != Backend::Asio) {
    std::fprintf(stderr, "pqdaw: endpoint ids select devices on --backend wasapi only\n");
    return 1;
  }
  auto created = engine::createAudioDevice(backend);
  if (!created) {
    std::fprintf(stderr, "pqdaw: %s\n", created.error().message.c_str());
    return 1;
  }
  engine::AudioDevice& dev = **created;
  auto* wasapi = backend == Backend::Wasapi ? static_cast<engine::WasapiDevice*>(&dev) : nullptr;
  auto* asioDev = backend == Backend::Asio ? static_cast<engine::AsioDevice*>(&dev) : nullptr;
  if (wasapi && options.stallAfterWake) wasapi->injectStallAfterWake(options.stallAfterWake);
  // Audio-thread diagnostics (underrun episodes, recovery restarts) through the RT log ring, printed as they come.
  pq::RtLog rtLog(1024);
  pq::RtLogger logger([](void*, const std::string& line) { std::printf("log:      %s\n", line.c_str()); }, nullptr, 50);
  if (wasapi) {
    wasapi->setRtLog(&rtLog);
    logger.add(rtLog, "wasapi");
  }

  engine::StreamConfig config;
  config.outputDevice = options.device;
  config.outputEndpointId = options.deviceEndpoint;
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
  sine.amplitude = static_cast<float>(std::pow(10.0, options.levelDb / 20.0));
  // ASIO: a driver reset may recreate the stream with another buffer size or rate. The device calls this on its
  // control thread while the stream is stopped, so re-preparing the adapter and the tone there is safe.
  struct Rig {
    engine::BlockAdapter* adapter;
    SineEngine* sine;
    uint32_t block;
    double hz;
  } rig{&adapter, &sine, options.blockFrames, options.sineHz};
  if (asioDev)
    asioDev->setStreamChangedCallback(
        [](void* user, const engine::StreamInfo& si) {
          auto& r = *static_cast<Rig*>(user);
          (void)r.adapter->prepare({&SineEngine::process, r.sine}, si.inputChannels, si.outputChannels, r.block,
                                   si.bufferFrames);
          r.sine->increment = static_cast<uint32_t>(std::llround(r.hz / si.sampleRate * 4294967296.0) & 0xFFFFFFFFll);
        },
        &rig);
  const double periodUs = 1e6 * info.bufferFrames / info.sampleRate;

  std::printf("device:   %s [%s, %s]\n", info.outputDeviceName.c_str(), engine::backendName(info.backend),
              !options.deviceEndpoint.empty()           ? options.deviceEndpoint.c_str()
              : options.device == engine::kDefaultDevice ? "default"
                                                         : ("id " + std::to_string(options.device)).c_str());
  std::printf("stream:   %u Hz, %u ch out, device buffer %u frames (%.3f ms), open-time latency estimate %u frames "
              "(%.3f ms)\n",
              info.sampleRate, info.outputChannels, info.bufferFrames, periodUs / 1e3, info.latencyFrames,
              1e3 * info.latencyFrames / info.sampleRate);
  if (backend == Backend::Wasapi)
    std::printf("wasapi:   %s mode, device format %s x %u ch, endpoint buffer %u frames%s\n",
                info.shareMode == engine::ShareMode::Exclusive ? "exclusive" : "shared",
                engine::sampleFormatName(info.deviceFormat), info.deviceChannels, info.endpointBufferFrames,
                info.bufferFrames != options.bufferFrames ? " (requested buffer adjusted to the device's period)" : "");
  if (asioDev) {
    const engine::AsioStreamDetails d = asioDev->details();
    std::printf("asio:     driver version %d, %u out / %u in channels, output type %s; buffer %d..%d (%s), "
                "preferred %d%s; outputReady %s\n",
                d.driverVersion, d.driverOutputs, d.driverInputs, engine::asioSampleTypeName(d.outputSampleType),
                d.minBufferFrames, d.maxBufferFrames, granularityText(d.bufferGranularity).c_str(),
                d.preferredBufferFrames,
                info.bufferFrames != options.bufferFrames ? " (requested buffer adjusted to the driver's sizes)" : "",
                d.outputReady ? "yes" : "no");
    std::printf("latency:  driver-reported output %u frames (%.3f ms), input %u frames (%.3f ms)\n", info.latencyFrames,
                1e3 * info.latencyFrames / info.sampleRate, info.inputLatencyFrames,
                1e3 * info.inputLatencyFrames / info.sampleRate);
  }
  std::printf("engine:   internal block %u frames, adapter %s (+%u frames latency)\n", adapter.blockFrames(),
              adapter.directMode() ? "direct" : "FIFO", adapter.latencyFrames());
  std::printf("signal:   %.3f Hz sine at %.1f dBFS for %.3f s%s\n", options.sineHz, options.levelDb, options.seconds,
              options.stallAfterWake ? (" (test: 3-period stall after wake " + std::to_string(options.stallAfterWake) + ")").c_str() : "");
  if (info.backendResampling)
    std::printf("warning:  %u Hz differs from the device mix rate: RtAudio resamples on the audio thread "
                "(allocates; not RT-safe)\n",
                info.sampleRate);

  logger.start();
  if (auto s = dev.start(); !s) {
    std::fprintf(stderr, "pqdaw: %s\n", s.error().message.c_str());
    return 1;
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::duration<double>(options.seconds));
  const auto stopStatus = dev.stop();
  logger.stop();
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const engine::CallbackStats::Snapshot st = dev.stats().snapshot();
  engine::AsioEvents asioEvents;
  std::string asioError;
  if (asioDev) {
    asioEvents = asioDev->events();
    asioError = asioDev->lastError();
  }
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
  if (backend == Backend::Wasapi) {
    std::printf("wakes:    %llu device events, %llu wait timeouts; max lateness %lld frames (%.1f us) vs the "
                "device schedule; %llu stream restart(s) after underruns\n",
                static_cast<unsigned long long>(st.wakes), static_cast<unsigned long long>(st.waitTimeouts),
                static_cast<long long>(st.maxLatenessFrames), st.maxLatenessUs,
                static_cast<unsigned long long>(st.restarts));
    std::printf("latency:  device clock (frames written ahead of the IAudioClock position at a wake): mean %.1f "
                "frames (%.3f ms), min %lld, max %lld\n",
                st.meanQueueFrames, 1e3 * st.meanQueueFrames / info.sampleRate,
                static_cast<long long>(st.minQueueFrames), static_cast<long long>(st.maxQueueFrames));
  }
  std::printf("deadline misses: %llu (callbacks longer than the %.1f us buffer period)\n",
              static_cast<unsigned long long>(st.deadlineMisses), st.periodUs);
  std::printf("callback time: mean %.2f us, max %.2f us; max interval between callbacks %.1f us%s\n",
              st.meanDurationUs, st.maxDurationUs, st.maxIntervalUs,
              st.timingReliable ? "" : " (TSC not invariant: timing approximate)");
  const bool intervalOk = st.maxIntervalUs <= 1.5 * periodUs;
  std::printf("interval check: max %.1f us vs 1.5 x period = %.1f us: %s\n", st.maxIntervalUs, 1.5 * periodUs,
              intervalOk ? "PASS" : "FAIL");
  if (backend == Backend::RtAudioWasapi)
    std::printf("note:     RtAudio's WASAPI backend never reports output underflows (docs/QUESTIONS.md Q12)\n");
  uint64_t asioProblems = 0;
  if (asioDev) {
    std::printf("asio:     %llu resync request(s) (driver lost samples), %llu overload(s), %llu reset request(s), "
                "%llu buffer-size change(s), %llu rate change(s), %llu latency change(s); %llu reopen(s), %llu "
                "failed; output underflows above = missed buffers (sample position jumps)\n",
                static_cast<unsigned long long>(asioEvents.resyncRequests),
                static_cast<unsigned long long>(asioEvents.overloads),
                static_cast<unsigned long long>(asioEvents.resetRequests),
                static_cast<unsigned long long>(asioEvents.bufferSizeChanges),
                static_cast<unsigned long long>(asioEvents.rateChanges),
                static_cast<unsigned long long>(asioEvents.latencyChanges),
                static_cast<unsigned long long>(asioEvents.reopens),
                static_cast<unsigned long long>(asioEvents.failedReopens));
    if (!asioError.empty()) std::printf("error:    %s\n", asioError.c_str());
    asioProblems = asioEvents.resyncRequests + asioEvents.overloads + asioEvents.failedReopens;
  }
  if (options.stallAfterWake) {
    const bool one = st.outputUnderflows == 1;
    std::printf("stall check: %llu underrun(s) reported for one injected stall: %s\n",
                static_cast<unsigned long long>(st.outputUnderflows), one ? "PASS" : "FAIL");
    return (one && st.inputOverflows == 0 && stopStatus) ? 0 : 3;
  }
  return (st.xruns() || st.deadlineMisses || asioProblems || !stopStatus) ? 3 : 0;
}

namespace {

// Emits a single-sample impulse on every channel at the start of every `every`-th callback (after `first`
// callbacks), and records the QPC time of each emitting callback. Silence otherwise.
struct ImpulseEngine {
  static constexpr uint32_t kMax = 64;
  float amplitude = 0;
  uint64_t first = 0;
  uint64_t every = 1;
  uint64_t blocks = 0;
  uint64_t emitQpc[kMax] = {};
  std::atomic<uint32_t> emitted{0};

  static void process(void* user, const engine::AudioBlock& b) noexcept {  // RT-safe
    auto& e = *static_cast<ImpulseEngine*>(user);
    for (uint32_t c = 0; c < b.numOut; ++c) std::memset(b.out[c], 0, sizeof(float) * b.frames);
    const uint32_t n = e.emitted.load(std::memory_order_relaxed);
    if (e.blocks >= e.first && (e.blocks - e.first) % e.every == 0 && n < kMax) {
      e.emitQpc[n] = engine::qpcNow100ns();  // measurement-only engine: a user-mode counter read
      for (uint32_t c = 0; c < b.numOut; ++c) b.out[c][0] = e.amplitude;
      e.emitted.store(n + 1, std::memory_order_release);
    }
    ++e.blocks;
  }
};

}  // namespace

int runLatencyTest(const CliOptions& options) {
  engine::WasapiDevice dev;
  engine::StreamConfig config;
  config.outputDevice = options.device;
  config.outputEndpointId = options.deviceEndpoint;
  config.outputChannels = 2;
  config.bufferFrames = options.bufferFrames;
  config.shareMode = engine::ShareMode::Shared;  // loopback capture exists for shared-mode streams only

  ImpulseEngine imp;
  auto opened = dev.open(config, {&ImpulseEngine::process, &imp});
  if (!opened) {
    std::fprintf(stderr, "pqdaw: %s\n", opened.error().message.c_str());
    return 1;
  }
  const engine::StreamInfo info = *opened;
  const double rate = info.sampleRate;
  imp.amplitude = static_cast<float>(std::pow(10.0, options.levelDb / 20.0));
  imp.first = static_cast<uint64_t>(0.3 * rate / info.bufferFrames) + 1;
  imp.every = std::max<uint64_t>(1, static_cast<uint64_t>(0.2 * rate / info.bufferFrames));

  engine::LoopbackCapture cap;
  if (auto s = cap.start(info.outputEndpointId, options.seconds + 2.0); !s) {
    std::fprintf(stderr, "pqdaw: %s\n", s.error().message.c_str());
    return 1;
  }
  std::printf("device:   %s [WASAPI shared, %s]\n", info.outputDeviceName.c_str(), info.outputEndpointId.c_str());
  std::printf("stream:   %u Hz, period %u frames (%.3f ms), endpoint buffer %u frames, %s x %u ch; loopback %u Hz\n",
              info.sampleRate, info.bufferFrames, 1e3 * info.bufferFrames / rate, info.endpointBufferFrames,
              engine::sampleFormatName(info.deviceFormat), info.deviceChannels, cap.sampleRate());
  if (auto s = dev.start(); !s) {
    std::fprintf(stderr, "pqdaw: %s\n", s.error().message.c_str());
    (void)cap.stop();
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::duration<double>(options.seconds));
  const auto stopStatus = dev.stop();
  const engine::CallbackStats::Snapshot st = dev.stats().snapshot();
  dev.close();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const auto capStatus = cap.stop();
  if (!stopStatus || !capStatus) {
    std::fprintf(stderr, "pqdaw: %s\n", (!stopStatus ? stopStatus : capStatus).error().message.c_str());
    return 1;
  }

  // Impulse arrival times in the loopback stream (QPC of the packet's first frame + offset).
  const std::vector<float>& x = cap.samples();
  const float threshold = imp.amplitude * 0.5f;
  const uint64_t refractory = static_cast<uint64_t>(cap.sampleRate() / 10);
  std::vector<double> arrivals;  // 100 ns units
  uint64_t lastHit = 0;
  bool any = false;
  uint32_t discontinuities = 0;
  for (const engine::LoopbackCapture::Packet& p : cap.packets()) {
    if (p.flags & 0x1) ++discontinuities;  // AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY
    for (uint64_t i = p.firstFrame; i < p.firstFrame + p.frames; ++i) {
      if (std::fabs(x[i]) < threshold || (any && i - lastHit < refractory)) continue;
      arrivals.push_back(static_cast<double>(p.qpc100ns) + 1e7 * static_cast<double>(i - p.firstFrame) / cap.sampleRate());
      lastHit = i;
      any = true;
    }
  }
  const uint32_t emitted = imp.emitted.load(std::memory_order_acquire);
  std::printf("impulses: %u emitted at %.1f dBFS, %zu found in the loopback capture (%zu frames, %u discontinuities)\n",
              emitted, options.levelDb, arrivals.size(), x.size(), discontinuities);
  if (arrivals.size() != emitted || emitted == 0) {
    std::printf("result:   FAIL (could not pair every impulse; other audio on the endpoint?)\n");
    return 3;
  }
  double sum = 0, mn = 1e30, mx = -1e30;
  for (uint32_t i = 0; i < emitted; ++i) {
    const double frames = (arrivals[i] - static_cast<double>(imp.emitQpc[i])) * rate / 1e7;
    sum += frames;
    mn = std::min(mn, frames);
    mx = std::max(mx, frames);
  }
  const double mean = sum / emitted;
  std::printf("measured: callback start -> loopback capture: mean %.1f frames (%.3f ms), min %.1f, max %.1f\n", mean,
              1e3 * mean / rate, mn, mx);
  // The backend's latency report: frames written ahead of the IAudioClock position at each wake (mean after
  // warm-up). The impulse starts the block written at a wake, so that queue is exactly the audio ahead of it.
  std::printf("reported: device-clock latency (frames written - IAudioClock position at a wake) mean %.1f frames "
              "(%.3f ms), range %lld..%lld; open-time estimate (StreamInfo) %u frames\n",
              st.meanQueueFrames, 1e3 * st.meanQueueFrames / rate, static_cast<long long>(st.minQueueFrames),
              static_cast<long long>(st.maxQueueFrames), info.latencyFrames);
  const double diff = st.meanQueueFrames - mean;
  const bool ok = st.meanQueueFrames >= 0 && std::fabs(diff) <= info.bufferFrames;
  std::printf("result:   reported - measured = %+.1f frames vs 1 period = %u frames: %s\n", diff, info.bufferFrames,
              ok ? "PASS" : "FAIL");
  std::printf("note:     loopback is the Windows audio engine's digital mix for the endpoint, time-stamped on the "
              "endpoint clock; the driver's USB transfer and the converters after it are not included\n");
  return ok ? 0 : 3;
}

}  // namespace pq::app
