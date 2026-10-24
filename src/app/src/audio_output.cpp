// The DAW window's audio output (T3.17): audio_output.h describes it.
#include "audio_output.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "pq/engine/null_device.h"
#include "pq/engine/wasapi_device.h"

namespace pq::app {

AudioOutput::AudioOutput(const CliOptions& options, engine::Engine& engine) : engine_(engine) {
  engine_.setWakeHook(&AudioOutput::onWake, this);
  control_ = std::thread([this, options] { controlMain(options); });
}

AudioOutput::~AudioOutput() {
  engine_.setWakeHook(nullptr, nullptr);
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  cv_.notify_all();
  if (control_.joinable()) control_.join();
}

void AudioOutput::onWake(void* self) {
  auto& o = *static_cast<AudioOutput*>(self);
  {
    std::lock_guard lock(o.mutex_);
    o.wakeRequest_ = true;
  }
  o.cv_.notify_one();
}

void AudioOutput::onDevice(void* self, const engine::AudioBlock& b) noexcept {  // RT-safe
  static_cast<AudioOutput*>(self)->adapter_.process(b);
}

// The engine's block, clamped to [−1, 1] (a safety net for the speakers; offline renders are not clamped). RT-safe.
void AudioOutput::onEngine(void* self, const engine::AudioBlock& b) noexcept {
  auto& o = *static_cast<AudioOutput*>(self);
  o.engine_.process(b);
  if (o.engine_.asleep()) return;  // silence: nothing to clamp
  for (uint32_t c = 0; c < b.numOut; ++c) {
    float* x = b.out[c];
    for (uint32_t f = 0; f < b.frames; ++f) x[f] = std::clamp(x[f], -1.0f, 1.0f);
  }
}

bool AudioOutput::open(const CliOptions& o) {
  const engine::AudioCallback engineCb{&AudioOutput::onEngine, this};
  const double rate = engine_.config().sampleRate;
  const uint32_t block = engine_.config().blockFrames;
  std::string why;
  if (std::getenv("PQ_NO_AUDIO_DEVICE") == nullptr) {
    const engine::Backend backend = o.asio                        ? engine::Backend::Asio
                                    : o.wasapi == WasapiImpl::Own ? engine::Backend::Wasapi
                                                                  : engine::Backend::RtAudioWasapi;
    auto created = engine::createAudioDevice(backend);
    if (created.ok()) {
      device_ = std::move(created.value());
      engine::StreamConfig sc;
      sc.outputDevice = o.device;
      sc.outputEndpointId = o.deviceEndpoint;
      sc.outputChannels = 2;
      sc.sampleRate = static_cast<uint32_t>(std::lround(rate));
      sc.bufferFrames = o.bufferFrames;
      sc.shareMode = o.exclusive ? engine::ShareMode::Exclusive : engine::ShareMode::Shared;
      auto opened = device_->open(sc, {&AudioOutput::onDevice, this});
      if (!opened.ok()) {
        why = opened.error().message;
      } else if (opened.value().sampleRate != sc.sampleRate) {
        why = "the device runs at " + std::to_string(opened.value().sampleRate) + " Hz, the engine at " +
              std::to_string(sc.sampleRate) + " Hz (rate switching comes with the audio preferences, T3.18)";
        device_->close();
      } else if (!adapter_.prepare(engineCb, 0, opened.value().outputChannels, block, opened.value().bufferFrames)) {
        why = "invalid stream configuration";
        device_->close();
      } else {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s [%s], %u Hz, buffer %u frames, engine block %u",
                      opened.value().outputDeviceName.c_str(), engine::backendName(opened.value().backend),
                      opened.value().sampleRate, opened.value().bufferFrames, block);
        description_ = buf;
        return true;
      }
    } else {
      why = created.error().message;
    }
  } else {
    why = "PQ_NO_AUDIO_DEVICE is set";
  }
  // No device: a NullDevice runs the engine in real time, silently.
  auto null = std::make_unique<engine::NullDevice>();
  engine::StreamConfig sc;
  sc.outputChannels = 2;
  sc.sampleRate = static_cast<uint32_t>(std::lround(rate));
  sc.bufferFrames = 256;
  if (!null->open(sc, {&AudioOutput::onDevice, this}).ok() || !adapter_.prepare(engineCb, 0, 2, block, 256)) {
    device_.reset();
    return false;
  }
  device_ = std::move(null);
  description_ = "none (" + why + "): the engine runs on a null device, silently";
  return true;
}

void AudioOutput::controlMain(CliOptions o) {
  using clock = std::chrono::steady_clock;
  const bool ok = open(o);
  std::printf("pqdaw: audio %s\n", ok ? description_.c_str() : "unavailable: playback is silent and does not advance");
  std::fflush(stdout);
  ready_.store(true, std::memory_order_release);
  if (!ok) return;
  auto* wasapi = dynamic_cast<engine::WasapiDevice*>(device_.get());
  std::unique_lock lock(mutex_);
  bool running = false;
  clock::time_point asleepSince{};
  bool wasAsleep = false;
  auto start = [&] {
    lock.unlock();
    engine_.audioOnline();
    const Status st = device_->start();
    lock.lock();
    if (!st.ok()) {
      std::fprintf(stderr, "pqdaw: audio start failed: %s\n", st.error().message.c_str());
      engine_.audioOffline();
      return false;
    }
    running_.store(true, std::memory_order_release);
    return true;
  };
  running = start();
  while (!quit_) {
    if (!running) {  // suspended: block until a wake (no periodic work while idle)
      cv_.wait(lock, [&] { return quit_ || wakeRequest_; });
      if (quit_) break;
      wakeRequest_ = false;
      wasAsleep = false;
      running = start();
      continue;
    }
    cv_.wait_for(lock, std::chrono::milliseconds(200), [&] { return quit_; });
    if (quit_) break;
    if (wakeRequest_ || !engine_.asleep()) {
      wakeRequest_ = false;
      wasAsleep = false;
      continue;
    }
    const auto now = clock::now();
    if (!wasAsleep) {
      wasAsleep = true;
      asleepSince = now;
      continue;
    }
    if (std::chrono::duration<double>(now - asleepSince).count() < kSuspendAfterSeconds) continue;
    // Idle long enough: stop the stream (the next wake restarts it).
    lock.unlock();
    (void)device_->stop();
    engine_.audioOffline();
    if (wasapi) (void)wasapi->prepareRestart();
    lock.lock();
    running_.store(false, std::memory_order_release);
    running = false;
    suspensions_.fetch_add(1, std::memory_order_relaxed);
  }
  lock.unlock();
  (void)device_->stop();
  device_->close();
  engine_.audioOffline();
  running_.store(false, std::memory_order_release);
}

}  // namespace pq::app
