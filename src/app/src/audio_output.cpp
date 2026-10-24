// The DAW's audio output (T3.17, T3.18): pq/app/audio_output.h describes it.
#include "pq/app/audio_output.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pq/engine/asio_device.h"
#include "pq/engine/null_device.h"
#include "pq/engine/wasapi_device.h"

namespace pq::app {

namespace {

constexpr uint32_t kToneTableBits = 11;
constexpr uint32_t kToneTableSize = 1u << kToneTableBits;
constexpr uint32_t kToneFracBits = 32 - kToneTableBits;

const std::array<float, kToneTableSize + 1>& sineTable() {
  static const std::array<float, kToneTableSize + 1> t = [] {
    std::array<float, kToneTableSize + 1> a{};
    for (uint32_t i = 0; i <= kToneTableSize; ++i)
      a[i] = static_cast<float>(std::sin(6.283185307179586 * static_cast<double>(i) / kToneTableSize));
    return a;
  }();
  return t;
}

uint32_t toneIncrement(float hz, uint32_t rate) {
  if (rate == 0) return 0;
  const double inc = static_cast<double>(std::clamp(hz, 1.0f, 0.45f * static_cast<float>(rate))) / rate * 4294967296.0;
  return static_cast<uint32_t>(std::llround(inc));
}

std::string deviceError(engine::AudioDevice& d) {
  if (auto* w = dynamic_cast<engine::WasapiDevice*>(&d)) return w->threadError();
  if (auto* a = dynamic_cast<engine::AsioDevice*>(&d)) return a->lastError();
  return {};
}

}  // namespace

const float* toneSineTable() noexcept { return sineTable().data(); }

AudioOutput::AudioOutput(engine::Engine& engine, OutputConfig config, DeviceFactory factory)
    : engine_(&engine), factory_(std::move(factory)), config_(config) {
  (void)sineTable();  // built here, never on the audio thread
  engine.setWakeHook(&AudioOutput::onWake, this);
  pending_ = config;
  control_ = std::thread([this] { controlMain(); });
}

AudioOutput::~AudioOutput() {
  engine_.load()->setWakeHook(nullptr, nullptr);
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  cv_.notify_all();
  if (control_.joinable()) control_.join();
}

// ---- Requests (any thread) --------------------------------------------------------------------------------------

void AudioOutput::setNotify(void (*fn)(void*), void* user) {
  std::lock_guard lock(mutex_);
  notifyFn_ = fn;
  notifyUser_ = user;
}

void AudioOutput::notify() {
  void (*fn)(void*) = nullptr;
  void* user = nullptr;
  {
    std::lock_guard lock(mutex_);
    fn = notifyFn_;
    user = notifyUser_;
  }
  if (fn) fn(user);
}

void AudioOutput::reconfigure(const OutputConfig& config) {
  {
    std::lock_guard lock(mutex_);
    pending_ = config;
    status_.state = OutputStatus::State::Opening;
    ++status_.revision;
  }
  cv_.notify_all();
}

void AudioOutput::close() {
  std::unique_lock lock(mutex_);
  closeRequest_ = true;
  parked_ = false;
  cv_.notify_all();
  cv_.wait(lock, [&] { return parked_ || quit_; });
}

void AudioOutput::rebind(engine::Engine& engine) {
  engine_.store(&engine);  // the control thread is parked (close()): nothing reads it now
  engine.setWakeHook(&AudioOutput::onWake, this);
}

void AudioOutput::hardwareChanged() {
  {
    std::lock_guard lock(mutex_);
    hwChanged_ = true;
    ++status_.hardwareChanges;
    ++status_.revision;
  }
  cv_.notify_all();
  notify();
}

void AudioOutput::onWake(void* self) {
  auto& o = *static_cast<AudioOutput*>(self);
  {
    std::lock_guard lock(o.mutex_);
    o.wakeRequest_ = true;
  }
  o.cv_.notify_all();
}

void AudioOutput::onHardware(void* self) { static_cast<AudioOutput*>(self)->hardwareChanged(); }

OutputStatus AudioOutput::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

OutputConfig AudioOutput::config() const {
  std::lock_guard lock(mutex_);
  return config_;
}

engine::CallbackStats::Snapshot AudioOutput::callbackStats() const {
  std::lock_guard lock(mutex_);  // device_ is replaced under the lock
  return device_ ? device_->stats().snapshot() : engine::CallbackStats::Snapshot{};
}

void AudioOutput::setTestTone(bool on, float levelDb, float hz) {
  const float db = std::min(levelDb, kMaxToneDb);
  toneGain_.store(std::pow(10.0f, db / 20.0f), std::memory_order_relaxed);
  uint32_t rate = 0;
  {
    std::lock_guard lock(mutex_);
    rate = status_.stream.sampleRate ? status_.stream.sampleRate : config_.sampleRate;
    toneHz_ = hz;
  }
  toneInc_.store(toneIncrement(hz, rate), std::memory_order_relaxed);
  toneOn_.store(on, std::memory_order_relaxed);
  {
    std::lock_guard lock(mutex_);
    wakeRequest_ = wakeRequest_ || on;  // a suspended stream starts for the tone
  }
  cv_.notify_all();
}

Status AudioOutput::controlPanel() {
  std::unique_lock lock(mutex_);
  if (!device_ || device_->backend() != engine::Backend::Asio || fallback_)
    return Error{ErrorCode::InvalidArgument, "no ASIO driver is open", {}};
  panelRequest_ = true;
  cv_.notify_all();
  return Ok{};
}

// ---- Audio thread -----------------------------------------------------------------------------------------------

void AudioOutput::onDevice(void* self, const engine::AudioBlock& b) noexcept {  // RT-safe
  auto& o = *static_cast<AudioOutput*>(self);
  const uint32_t outs = std::min(b.numOut, o.engineOuts_);
  if (o.silence_.load(std::memory_order_relaxed)) {
    for (uint32_t c = 0; c < b.numOut; ++c) std::memset(b.out[c], 0, sizeof(float) * b.frames);
    return;
  }
  engine::AudioBlock e = b;
  e.in = nullptr;  // inputs are opened (ASIO duplex) but not read before recording (M6)
  e.numIn = 0;
  e.numOut = outs;
  o.adapter_.process(e);
  for (uint32_t c = outs; c < b.numOut; ++c) std::memset(b.out[c], 0, sizeof(float) * b.frames);
}

void AudioOutput::addTone(const engine::AudioBlock& b) noexcept {  // RT-safe
  const float* t = sineTable().data();
  const float g = toneGain_.load(std::memory_order_relaxed);
  const uint32_t inc = toneInc_.load(std::memory_order_relaxed);
  const uint32_t ch = std::min<uint32_t>(b.numOut, 2);
  uint32_t ph = tonePhase_;
  constexpr float kFrac = 1.0f / static_cast<float>(1u << kToneFracBits);
  for (uint32_t f = 0; f < b.frames; ++f) {
    const uint32_t i = ph >> kToneFracBits;
    const float frac = static_cast<float>(ph & ((1u << kToneFracBits) - 1)) * kFrac;
    const float s = g * (t[i] + frac * (t[i + 1] - t[i]));
    for (uint32_t c = 0; c < ch; ++c) b.out[c][f] += s;
    ph += inc;
  }
  tonePhase_ = ph;
}

// The engine's block (+ the test tone), clamped to [−1, 1] (a safety net for the speakers; offline renders are not
// clamped). RT-safe.
void AudioOutput::onEngine(void* self, const engine::AudioBlock& b) noexcept {
  auto& o = *static_cast<AudioOutput*>(self);
  engine::Engine& eng = *o.engine_.load(std::memory_order_relaxed);
  eng.process(b);
  const bool tone = o.toneOn_.load(std::memory_order_relaxed);
  if (tone) o.addTone(b);
  else if (eng.asleep()) return;  // silence: nothing to clamp
  for (uint32_t c = 0; c < b.numOut; ++c) {
    float* x = b.out[c];
    for (uint32_t f = 0; f < b.frames; ++f) x[f] = std::clamp(x[f], -1.0f, 1.0f);
  }
}

// ---- ASIO driver requests (AsioDevice's control thread, the driver stopped) ---------------------------------------

void AudioOutput::onStreamChanged(void* self, const engine::StreamInfo& info) {
  auto& o = *static_cast<AudioOutput*>(self);
  engine::Engine& eng = *o.engine_.load();
  const uint32_t engineRate = static_cast<uint32_t>(std::lround(eng.config().sampleRate));
  const uint32_t outs = std::min<uint32_t>(info.outputChannels, 2);
  bool prepared = true;
  if (outs != o.engineOuts_ || info.bufferFrames != o.status().stream.bufferFrames) {
    prepared = o.adapter_.prepare({&AudioOutput::onEngine, &o}, 0, std::max(outs, 1u), eng.config().blockFrames,
                                  info.bufferFrames);
    o.engineOuts_ = std::max(outs, 1u);
  }
  const bool mismatch = info.sampleRate != engineRate || !prepared;
  o.silence_.store(mismatch, std::memory_order_relaxed);
  {
    std::lock_guard lock(o.mutex_);
    char msg[160];
    std::snprintf(msg, sizeof msg, "the driver reset its stream (%u Hz, %u samples)", info.sampleRate, info.bufferFrames);
    o.status_.message = msg;
    o.status_.stream = info;
    o.status_.rateMismatch = info.sampleRate != engineRate;
    o.status_.adapterLatencyFrames = o.adapter_.latencyFrames();
    o.config_.bufferFrames = info.bufferFrames;  // follow the driver (its control panel changed it)
    ++o.status_.streamChanges;
    ++o.status_.revision;
  }
  o.toneInc_.store(toneIncrement(o.toneHzNow(), info.sampleRate), std::memory_order_relaxed);
  o.notify();
}

// ---- Control thread ---------------------------------------------------------------------------------------------

float AudioOutput::toneHzNow() const {
  std::lock_guard lock(mutex_);
  return toneHz_;
}

void AudioOutput::publish(OutputStatus::State s, std::string message, bool notifyUi) {
  {
    std::lock_guard lock(mutex_);
    status_.state = s;
    status_.message = std::move(message);
    ++status_.revision;
  }
  if (notifyUi) notify();
}

void AudioOutput::openFallback(const OutputConfig& c, std::string why) {
  engine::Engine& eng = *engine_.load();
  auto null = std::make_unique<engine::NullDevice>();
  engine::StreamConfig sc;
  sc.outputChannels = 2;
  sc.sampleRate = c.sampleRate;
  sc.bufferFrames = 256;
  silence_.store(false, std::memory_order_relaxed);
  engineOuts_ = 2;
  auto opened = null->open(sc, {&AudioOutput::onDevice, this});
  if (!opened || !adapter_.prepare({&AudioOutput::onEngine, this}, 0, 2, eng.config().blockFrames, 256)) {
    std::fprintf(stderr, "pqdaw: audio unavailable: playback is silent and does not advance\n");
    fallback_ = true;
    {
      std::lock_guard lock(mutex_);
      status_.stream = engine::StreamInfo{};
      status_.deviceName.clear();
    }
    publish(OutputStatus::State::Closed, std::move(why));
    return;
  }
  std::printf("pqdaw: audio: none (%s): the engine runs on a null device, silently\n", why.c_str());
  std::fflush(stdout);
  fallback_ = true;
  {
    std::lock_guard lock(mutex_);
    device_ = std::move(null);
    status_.stream = *opened;
    status_.deviceName = "none";
    status_.asioInfo.reset();
    status_.adapterLatencyFrames = 0;
    status_.rateMismatch = false;
  }
  publish(OutputStatus::State::Fallback, std::move(why));
}

bool AudioOutput::openDevice(const OutputConfig& c) {
  engine::Engine& eng = *engine_.load();
  const uint32_t block = eng.config().blockFrames;
  const uint32_t engineRate = static_cast<uint32_t>(std::lround(eng.config().sampleRate));
  std::string why;
  if (factory_.allowEnvDisable && std::getenv("PQ_NO_AUDIO_DEVICE") != nullptr) {
    why = "PQ_NO_AUDIO_DEVICE is set";
  } else if (c.sampleRate != engineRate) {
    why = "the engine runs at " + std::to_string(engineRate) + " Hz, not " + std::to_string(c.sampleRate);
  } else {
    auto created = factory_.create ? factory_.create(c.backend) : engine::createAudioDevice(c.backend);
    if (!created) {
      why = created.error().message;
    } else {
      std::unique_ptr<engine::AudioDevice> dev = std::move(*created);
      if (auto* asio = dynamic_cast<engine::AsioDevice*>(dev.get())) asio->setStreamChangedCallback(&AudioOutput::onStreamChanged, this);
      engine::StreamConfig sc;
      sc.outputDevice = c.deviceIndex;
      sc.outputEndpointId = c.device;
      sc.outputChannels = std::max(c.outputChannels, 1u);
      sc.inputChannels = c.backend == engine::Backend::Asio ? c.inputChannels : 0;  // WASAPI input: M6
      sc.sampleRate = c.sampleRate;
      sc.bufferFrames = c.bufferFrames;
      sc.shareMode = c.shareMode;
      auto opened = dev->open(sc, {&AudioOutput::onDevice, this});
      if (!opened) {
        why = opened.error().message;
      } else {
        const engine::StreamInfo& si = *opened;
        const uint32_t outs = std::max(std::min<uint32_t>(si.outputChannels, 2), 1u);
        if (!adapter_.prepare({&AudioOutput::onEngine, this}, 0, outs, block, si.bufferFrames)) {
          why = "invalid stream configuration";
          dev->close();
        } else {
          engineOuts_ = outs;
          std::optional<engine::DeviceInfo> asioInfo;
          if (c.backend == engine::Backend::Asio)  // only the open driver is queried while a stream is open
            if (auto list = dev->enumerate(); list.ok())
              for (engine::DeviceInfo& d : *list)
                if (d.name == si.outputDeviceName && d.probeError.empty()) asioInfo = std::move(d);
          // A driver that kept another rate (it refused ours): silence until the owner rebuilds the engine at it.
          silence_.store(si.sampleRate != engineRate, std::memory_order_relaxed);
          toneInc_.store(toneIncrement(toneHzNow(), si.sampleRate), std::memory_order_relaxed);
          std::printf("pqdaw: audio %s [%s], %u Hz, buffer %u frames, engine block %u\n", si.outputDeviceName.c_str(),
                      engine::backendName(si.backend), si.sampleRate, si.bufferFrames, block);
          std::fflush(stdout);
          fallback_ = false;
          {
            std::lock_guard lock(mutex_);
            device_ = std::move(dev);
            status_.stream = si;
            status_.deviceName = si.outputDeviceName;
            status_.adapterLatencyFrames = adapter_.latencyFrames();
            status_.rateMismatch = si.sampleRate != engineRate;
            status_.asioInfo = std::move(asioInfo);
            ++status_.opens;
          }
          publish(OutputStatus::State::Suspended, {});
          return true;
        }
      }
    }
  }
  openFallback(c, std::move(why));
  return false;
}

void AudioOutput::closeDevice() {
  std::unique_ptr<engine::AudioDevice> d;
  {
    std::lock_guard lock(mutex_);
    d = std::move(device_);
  }
  if (!d) return;
  if (streamRunning_.load(std::memory_order_relaxed)) {
    (void)d->stop();
    engine_.load()->audioOffline();
  }
  streamRunning_.store(false, std::memory_order_relaxed);
  running_.store(false, std::memory_order_release);
  d->close();
}

bool AudioOutput::startStream(bool notifyUi) {
  if (!device_ || streamRunning_.load(std::memory_order_relaxed)) return streamRunning_.load(std::memory_order_relaxed);
  engine::Engine& eng = *engine_.load();
  eng.audioOnline();
  const Status st = device_->start();
  if (!st.ok()) {
    eng.audioOffline();
    std::fprintf(stderr, "pqdaw: audio start failed: %s\n", st.error().message.c_str());
    return false;
  }
  streamRunning_.store(true, std::memory_order_relaxed);
  running_.store(true, std::memory_order_release);
  publish(fallback_ ? OutputStatus::State::Fallback : OutputStatus::State::Running, fallback_ ? status().message : std::string(),
          notifyUi);
  return true;
}

void AudioOutput::stopStream(bool prepareRestart) {
  if (!device_ || !streamRunning_.load(std::memory_order_relaxed)) return;
  (void)device_->stop();
  engine_.load()->audioOffline();
  if (prepareRestart)
    if (auto* w = dynamic_cast<engine::WasapiDevice*>(device_.get())) (void)w->prepareRestart();
  streamRunning_.store(false, std::memory_order_relaxed);
  running_.store(false, std::memory_order_release);
}

void AudioOutput::controlMain() {
  using clock = std::chrono::steady_clock;
  engine::DeviceChangeNotifier notifier;
  if (factory_.watchHardware) (void)notifier.start(&AudioOutput::onHardware, this);
  std::unique_lock lock(mutex_);
  clock::time_point asleepSince{}, openedAt{};
  bool wasAsleep = false;
  bool lostReopen = false;  // the device open now is a reopen after a loss
  // (Re)opens `c` and starts the stream; lock held on entry and exit.
  auto openAndStart = [&](const OutputConfig& c) {
    lock.unlock();
    closeDevice();
    const bool real = openDevice(c);
    const bool started = startStream();
    if (real && !started) {  // opened but would not start (busy in exclusive mode, unplugged meanwhile)
      const std::string err = "the device opened but did not start";
      closeDevice();
      openFallback(c, err);
      (void)startStream();
    }
    lock.lock();
    openedAt = clock::now();
    wasAsleep = false;
    ready_.store(true, std::memory_order_release);
  };
  while (!quit_) {
    if (closeRequest_) {
      closeRequest_ = false;
      lock.unlock();
      closeDevice();
      lock.lock();
      pending_.reset();
      parked_ = true;
      status_.state = OutputStatus::State::Closed;
      ++status_.revision;
      cv_.notify_all();
      continue;
    }
    if (pending_) {
      const OutputConfig c = *pending_;
      pending_.reset();
      parked_ = false;
      config_ = c;
      wakeRequest_ = false;
      lostReopen = false;
      openAndStart(c);
      continue;
    }
    if (parked_) {
      cv_.wait(lock, [&] { return quit_ || closeRequest_ || pending_.has_value(); });
      continue;
    }
    if (panelRequest_) {
      panelRequest_ = false;
      lock.unlock();
      if (auto* a = dynamic_cast<engine::AsioDevice*>(device_.get())) {
        if (Status st = a->controlPanel(); !st.ok()) std::fprintf(stderr, "pqdaw: %s\n", st.error().message.c_str());
      }
      lock.lock();
      continue;
    }
    if (hwChanged_) {
      hwChanged_ = false;
      if (fallback_) {  // the configured device may be back
        pending_ = config_;
        continue;
      }
    }
    if (!device_) {  // not even a null device: wait for a new configuration
      cv_.wait(lock, [&] { return quit_ || closeRequest_ || pending_.has_value() || hwChanged_; });
      continue;
    }
    if (!streamRunning_.load(std::memory_order_relaxed)) {  // suspended: block until a wake (no periodic work)
      cv_.wait(lock, [&] {
        return quit_ || closeRequest_ || pending_.has_value() || hwChanged_ || panelRequest_ || wakeRequest_ ||
               toneOn_.load(std::memory_order_relaxed);
      });
      if (quit_ || closeRequest_ || pending_ || hwChanged_ || panelRequest_) continue;
      wakeRequest_ = false;
      wasAsleep = false;
      lock.unlock();
      const bool ok = startStream(false);  // a resume: the UI is not told (an open window polls the status)
      lock.lock();
      if (!ok) pending_ = config_;  // the device went away while suspended: reopen (or fall back)
      continue;
    }
    cv_.wait_for(lock, std::chrono::milliseconds(200),
                 [&] { return quit_ || closeRequest_ || pending_.has_value() || hwChanged_ || panelRequest_; });
    if (quit_ || closeRequest_ || pending_ || panelRequest_) continue;
    // The stream stopped on its own: the device was removed (WASAPI: the audio thread ended with an error) or an ASIO
    // driver's reset failed (AsioDevice closes the stream; during a successful reset it stays open).
    const bool lost = device_->backend() == engine::Backend::Asio ? !device_->isOpen() : !device_->isRunning();
    if (lost) {
      std::string err = deviceError(*device_);
      if (err.empty()) err = "the device stopped";
      ++status_.deviceLosses;
      // One reopen per loss; a device that dies again within 2 s of such a reopen is not retried in a loop: the
      // output stays on the null device until the hardware changes (or the user picks another device).
      const bool retry = !(lostReopen && clock::now() - openedAt < std::chrono::seconds(2));
      lock.unlock();
      closeDevice();
      if (!retry) {
        openFallback(config_, "device lost: " + err);
        (void)startStream();
        lostReopen = false;
      } else {
        const bool real = openDevice(config_);
        (void)startStream();
        if (real) {
          publish(OutputStatus::State::Running, "reopened after: " + err);
        } else {
          const std::string why = status().message;
          publish(OutputStatus::State::Fallback, "device lost: " + err + "; reopening failed: " + why);
        }
        lostReopen = real;
      }
      lock.lock();
      openedAt = clock::now();
      continue;
    }
    if (wakeRequest_ || toneOn_.load(std::memory_order_relaxed) || !engine_.load()->asleep()) {
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
    stopStream(true);
    lock.lock();
    status_.state = fallback_ ? OutputStatus::State::Fallback : OutputStatus::State::Suspended;
    ++status_.suspensions;
    ++status_.revision;
    suspensions_.fetch_add(1, std::memory_order_relaxed);
    // Not notified: suspend / resume follow every play and stop, and an idle DAW must not wake its UI thread for
    // them (an open preferences window sees the state at its next 2 Hz poll).
  }
  lock.unlock();
  closeDevice();
  notifier.stop();
}

}  // namespace pq::app
