// The DAW's audio output (T3.17, T3.18): pq/app/audio_output.h describes it.
#include "pq/app/audio_output.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

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
  ticksPerUs_ = engine::tscFrequencyHz() / 1e6;  // calibrated here (~20 ms once), never on the audio thread
  traceCursor_ = engine::streamTrace().head();
  traceT0_ = engine::readTsc();
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

void AudioOutput::setKeepRunning(bool on) {  // T6.4
  keepRunning_.store(on, std::memory_order_relaxed);
  if (on) {
    {
      std::lock_guard lock(mutex_);
      wakeRequest_ = true;  // a suspended stream restarts
    }
    cv_.notify_all();
  }
}

void AudioOutput::setForeground(bool foreground) {
  {
    std::lock_guard lock(mutex_);
    if (foreground_ == foreground) return;
    foreground_ = foreground;
    if (foreground) wakeRequest_ = true;  // a suspended stream starts now (its pre-roll runs before Play)
  }
  cv_.notify_all();
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

void AudioOutput::setExtraSource(const engine::AudioCallback* cb) {
  extra_.store(cb);
  while (inExtra_.load() != 0) std::this_thread::yield();  // a callback still using the previous source finishes
  {
    std::lock_guard lock(mutex_);
    wakeRequest_ = wakeRequest_ || cb != nullptr;  // a suspended stream starts for the preview
  }
  cv_.notify_all();
}

bool AudioOutput::prepareAdapter(uint32_t deviceIns, uint32_t deviceOuts, uint32_t blockFrames,
                                 uint32_t deviceFrames) {  // control thread
  engineIns_ = std::min(deviceIns, engine::kMaxChannels);  // T6.7: the inputs reach the engine (tracks, recording)
  if (engineIns_ != 0 && zeroIn_.size() < deviceFrames) zeroIn_ = AlignedBuffer<float>(deviceFrames);  // silence for a
  // block the device delivers without its inputs
  engineOuts_ = std::max(std::min<uint32_t>(deviceOuts, 2), 1u);
  clickOuts_ = deviceOuts >= 4 ? 2u : 0u;  // T6.6: a cue pair exists
  if (clickOuts_ != 0) {
    clickCap_ = std::max<uint32_t>(deviceFrames, 4096);
    clickBuf_ = AlignedBuffer<float>(static_cast<size_t>(clickCap_) * 2);
  }
  return adapter_.prepare({&AudioOutput::onEngine, this}, engineIns_, engineOuts_ + clickOuts_, blockFrames, deviceFrames);
}

void AudioOutput::setCueSource(const engine::AudioCallback* cb) {
  cue_.store(cb);
  while (inCue_.load() != 0) std::this_thread::yield();  // a callback still using the previous source finishes
  {
    std::lock_guard lock(mutex_);
    wakeRequest_ = wakeRequest_ || cb != nullptr;  // a suspended stream starts for the preview
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
  auto silence = [&] {
    for (uint32_t c = 0; c < b.numOut; ++c) std::memset(b.out[c], 0, sizeof(float) * b.frames);
  };
  // AU.stream: the cold-start pre-roll. Silence (the engine is not run: it is audioOffline, so its transport waits)
  // until the driver has taken enough clean buffers; then the engine goes online and runs from the next buffer.
  if (o.prerollArmed_.load(std::memory_order_acquire)) {
    o.prerollArmed_.store(false, std::memory_order_relaxed);
    const engine::CallbackStats* st = o.devStats_.load(std::memory_order_relaxed);
    o.pre_ = Preroll{true, 0, 0, 0, st ? st->xrunsNow() : 0, engine::readTsc()};
  }
  if (o.pre_.active) {
    silence();
    Preroll& p = o.pre_;
    ++p.buffers;
    p.frames += b.frames;
    const engine::CallbackStats* st = o.devStats_.load(std::memory_order_relaxed);
    const uint64_t x = st ? st->xrunsNow() : 0;
    if (x != p.xruns) {
      p.xruns = x;
      p.clean = 0;
    } else {
      ++p.clean;
    }
    const bool settled = p.frames >= o.prerollTarget_ && p.clean >= kPrerollCleanBuffers;
    if (settled || p.frames >= o.prerollMax_) {
      p.active = false;
      const auto us = static_cast<int64_t>(static_cast<double>(engine::readTsc() - p.t0) / o.ticksPerUs_);
      engine::streamTrace().push(engine::TraceKind::PrerollEnd, settled ? 0 : 1, static_cast<int32_t>(p.buffers), us);
      o.engine_.load(std::memory_order_relaxed)->audioOnline();  // this thread is the engine's reader from here
      o.fadeInLeft_ = o.fadeInLen_;  // what resumes (a voice still sounding after a driver reset) ramps in
      o.prerolling_.store(false, std::memory_order_release);
    }
    return;
  }
  // AU.stream: after a fade-out, silence until the stop (the engine is not run), counting what has drained.
  if (const uint32_t fs = o.fade_.load(std::memory_order_acquire); fs >= kFadeMuted) {
    silence();
    if (fs == kFadeMuted) {
      o.fadeSilent_ += b.frames;
      if (o.fadeSilent_ >= o.fadeDrain_) o.fade_.store(kFadeDrained, std::memory_order_release);
    }
    return;
  }
  if (o.silence_.load(std::memory_order_relaxed)) {
    silence();
    return;
  }
  // T6.7b: a backend that measures its input's round trip (WASAPI capture: two clocks): the record latency follows it.
  // The round trip is device in → device out; each path also passes the adapter's FIFO.
  if (o.engineIns_ != 0)
    if (const engine::CallbackStats* st = o.devStats_.load(std::memory_order_relaxed); st != nullptr)
      if (const int64_t rt = st->roundTripNow(); rt >= 0 && rt != o.lastRoundTrip_) {
        o.lastRoundTrip_ = rt;
        const int64_t in = rt + 2 * int64_t{o.recAdapter_} - o.recOut_;
        o.engine_.load(std::memory_order_relaxed)->recorder().setDeviceLatency(static_cast<uint32_t>(std::max<int64_t>(0, in)),
                                                                               o.recOut_);
      }
  engine::AudioBlock e = b;
  // T6.7: the device's inputs (ASIO duplex) reach the engine: the tracks' `track.in`, recording.
  const float* ins[engine::kMaxChannels] = {};
  const bool haveIn = b.in != nullptr && b.numIn >= o.engineIns_;
  for (uint32_t c = 0; c < o.engineIns_; ++c) ins[c] = haveIn ? b.in[c] : o.zeroIn_.data();
  e.in = o.engineIns_ != 0 ? ins : nullptr;
  e.numIn = o.engineIns_;
  e.numOut = outs;
  for (uint32_t c = outs; c < b.numOut; ++c) std::memset(b.out[c], 0, sizeof(float) * b.frames);
  if (o.clickOuts_ == 0) {
    o.adapter_.process(e);
  } else {  // T6.6: the engine's click pair, into the cue pair while the metronome plays there
    engine::Engine& eng = *o.engine_.load(std::memory_order_relaxed);
    const uint32_t first = 2 * o.cuePair_.load(std::memory_order_relaxed);
    const bool cue = eng.metronome().cue() && first >= 2 && first + 2 <= b.numOut;
    float* click[2] = {o.clickBuf_.data(), o.clickBuf_.data() + o.clickCap_};
    for (uint32_t off = 0; off < b.frames; off += o.clickCap_) {  // one pass unless the device block is huge
      const uint32_t n = std::min(o.clickCap_, b.frames - off);
      float* ptrs[engine::kMaxChannels];
      for (uint32_t c = 0; c < outs; ++c) ptrs[c] = b.out[c] + off;
      ptrs[outs] = click[0];
      ptrs[outs + 1] = click[1];
      engine::AudioBlock sub = e;
      const float* subIn[engine::kMaxChannels];
      for (uint32_t c = 0; c < o.engineIns_; ++c) subIn[c] = ins[c] + (haveIn ? off : 0);
      sub.in = o.engineIns_ != 0 ? subIn : nullptr;
      sub.out = ptrs;
      sub.numOut = outs + 2;
      sub.frames = n;
      sub.frameTime = b.frameTime + off;
      eng.setClickPair(cue);
      o.adapter_.process(sub);
      if (cue)
        for (uint32_t c = 0; c < 2; ++c) {
          float* dst = b.out[first + c] + off;
          for (uint32_t f = 0; f < n; ++f) dst[f] += click[c][f];
        }
    }
  }
  // T5.10: the cue source at the device's buffer, into its output pair (announced as in setExtraSource's handshake).
  o.inCue_.store(1);
  if (const engine::AudioCallback* cue = o.cue_.load()) {
    uint32_t first = 2 * o.cuePair_.load(std::memory_order_relaxed);
    if (first + 2 > b.numOut) first = 0;
    const uint32_t n = b.numOut > first + 1 ? 2u : 1u;
    float* pair[2] = {b.out[first], b.out[first + n - 1]};
    cue->fn(cue->user, engine::AudioBlock{nullptr, 0, pair, n, b.frames, b.frameTime});
    for (uint32_t c = 0; c < n; ++c)
      for (uint32_t f = 0; f < b.frames; ++f) pair[c][f] = std::clamp(pair[c][f], -1.0f, 1.0f);
  }
  o.inCue_.store(0, std::memory_order_release);
  // AU.stream: a requested fade-out: a linear ramp to 0 over fadeLen_ frames (across buffers), then silence.
  if (o.fade_.load(std::memory_order_acquire) == kFadeRequested) {
    const float inv = 1.0f / static_cast<float>(o.fadeLen_);
    uint32_t f = 0;
    for (; f < b.frames && o.fadePos_ < o.fadeLen_; ++f, ++o.fadePos_) {
      const float g = 1.0f - static_cast<float>(o.fadePos_ + 1) * inv;
      for (uint32_t c = 0; c < b.numOut; ++c) b.out[c][f] *= g;
    }
    if (o.fadePos_ >= o.fadeLen_) {
      for (uint32_t c = 0; c < b.numOut; ++c) std::memset(b.out[c] + f, 0, sizeof(float) * (b.frames - f));
      o.fadeSilent_ = b.frames - f;
      o.fade_.store(o.fadeSilent_ >= o.fadeDrain_ ? kFadeDrained : kFadeMuted, std::memory_order_release);
    }
  }
  // AU.stream: the first frames after a pre-roll ramp in from 0 (kFadeSeconds), so audio resuming after a driver
  // reset does not step out of the silence. (After a Play the engine starts at the note's own onset anyway.)
  if (o.fadeInLeft_ > 0) {
    const float inv = 1.0f / static_cast<float>(o.fadeInLen_);
    for (uint32_t f = 0; f < b.frames && o.fadeInLeft_ > 0; ++f, --o.fadeInLeft_) {
      const float g = static_cast<float>(o.fadeInLen_ - o.fadeInLeft_ + 1) * inv;
      for (uint32_t c = 0; c < b.numOut; ++c) b.out[c][f] *= g;
    }
  }
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
  // The extra source (T4.4): announced before the pointer is read (sequentially consistent, paired with
  // setExtraSource), so a replaced source is never called after setExtraSource returned.
  o.inExtra_.store(1);
  const engine::AudioCallback* extra = o.extra_.load();
  if (extra) extra->fn(extra->user, b);
  o.inExtra_.store(0, std::memory_order_release);
  if (!tone && !extra && eng.asleep()) return;  // silence: nothing to clamp
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
  const uint32_t outs = std::max(std::min<uint32_t>(info.outputChannels, 2), 1u);
  const uint32_t clickOuts = info.outputChannels >= 4 ? 2u : 0u;
  bool prepared = true;
  if (outs != o.engineOuts_ || clickOuts != o.clickOuts_ || info.bufferFrames != o.status().stream.bufferFrames ||
      std::min(info.inputChannels, engine::kMaxChannels) != o.engineIns_)
    prepared = o.prepareAdapter(info.inputChannels, info.outputChannels, eng.config().blockFrames, info.bufferFrames);
  // T6.4: live notes play one device period (plus the adapter's FIFO) after their stamp: a constant latency.
  eng.liveInput().setLatency(info.bufferFrames + o.adapter_.latencyFrames());
  // T6.7: the record latency's device part (input and output paths, each with the adapter's FIFO).
  eng.recorder().setDeviceLatency(info.inputLatencyFrames + o.adapter_.latencyFrames(),
                                  (info.latencyFrames > 0 ? info.latencyFrames : info.bufferFrames) + o.adapter_.latencyFrames());
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
  // AU.stream: the driver restarts cold: the engine goes offline (the audio thread is stopped) and a pre-roll runs
  // before it is run again.
  eng.audioOffline();
  o.armPreroll(info);
  o.notify();
}

// AsioDevice's control thread, the driver still running (it may or may not call back any more).
void AudioOutput::onBeforeDriverStop(void* self) { static_cast<AudioOutput*>(self)->fadeOut(true); }

void AudioOutput::armPreroll(const engine::StreamInfo& si) {
  const double rate = si.sampleRate ? si.sampleRate : 48000.0;
  const auto minFrames = static_cast<uint64_t>(std::llround(kPrerollSeconds * rate));
  prerollTarget_ = std::max<uint64_t>(static_cast<uint64_t>(si.latencyFrames) + uint64_t{kPrerollCleanBuffers} * si.bufferFrames,
                                      minFrames);
  prerollMax_ = std::max<uint64_t>(prerollTarget_, static_cast<uint64_t>(std::llround(kPrerollMaxSeconds * rate)));
  fadeInLen_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(kFadeSeconds * rate)));
  fade_.store(kFadeNone, std::memory_order_relaxed);
  prerolling_.store(true, std::memory_order_relaxed);
  prerolls_.fetch_add(1, std::memory_order_relaxed);
  engine::streamTrace().push(engine::TraceKind::PrerollBegin, 0, static_cast<int32_t>(prerollTarget_),
                             static_cast<int64_t>(prerollMax_));
  prerollArmed_.store(true, std::memory_order_release);
}

void AudioOutput::fadeOut(bool deviceRunning) {
  if (!deviceRunning || !streamRunning_.load(std::memory_order_relaxed) || fallback_ ||
      prerolling_.load(std::memory_order_acquire) || silence_.load(std::memory_order_relaxed))
    return;
  engine::Engine& eng = *engine_.load();
  if (eng.asleep() && !toneOn_.load(std::memory_order_relaxed) && extra_.load() == nullptr && cue_.load() == nullptr)
    return;  // nothing sounds
  // One fade at a time (the control thread's stop and an ASIO reset's hook may race): claim it.
  uint32_t idle = kFadeNone;
  if (!fade_.compare_exchange_strong(idle, kFadeArming, std::memory_order_acq_rel)) return;  // faded or fading
  const engine::StreamInfo si = status().stream;
  const double rate = si.sampleRate ? si.sampleRate : 48000.0;
  fadeLen_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(kFadeSeconds * rate)));
  fadePos_ = 0;
  fadeSilent_ = 0;
  fadeDrain_ = uint64_t{si.latencyFrames} + si.bufferFrames;  // what the driver still holds, then one buffer
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const auto bound = std::chrono::duration<double>(static_cast<double>(fadeLen_ + fadeDrain_ + 3ull * si.bufferFrames) / rate + 0.1);
  fade_.store(kFadeRequested, std::memory_order_release);
  bool drained = false;
  while (!(drained = fade_.load(std::memory_order_acquire) == kFadeDrained) && clock::now() - t0 < bound)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (!drained) fade_.store(kFadeMuted, std::memory_order_release);  // a driver that stopped calling back: mute anyway
  fades_.fetch_add(1, std::memory_order_relaxed);
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count();
  engine::streamTrace().push(engine::TraceKind::FadeOut, drained ? 0 : 1, static_cast<int32_t>(fadeLen_), us);
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
    status_.message = message.empty() ? inputNote_ : std::move(message);  // T6.7b: an input that did not open
    ++status_.revision;
  }
  if (notifyUi) notify();
}

void AudioOutput::openFallback(const OutputConfig& c, std::string why) {
  {
    std::lock_guard lock(mutex_);
    inputNote_.clear();  // T6.7b
  }
  engine::Engine& eng = *engine_.load();
  auto null = std::make_unique<engine::NullDevice>();
  engine::StreamConfig sc;
  sc.outputChannels = 2;
  sc.sampleRate = c.sampleRate;
  sc.bufferFrames = 256;
  silence_.store(false, std::memory_order_relaxed);
  auto opened = null->open(sc, {&AudioOutput::onDevice, this});
  if (!opened || !prepareAdapter(0, 2, eng.config().blockFrames, 256)) {
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
  devStats_.store(&null->stats(), std::memory_order_relaxed);
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
      if (auto* asio = dynamic_cast<engine::AsioDevice*>(dev.get())) {
        asio->setStreamChangedCallback(&AudioOutput::onStreamChanged, this);
        asio->setBeforeDriverStopCallback(&AudioOutput::onBeforeDriverStop, this);
      }
      engine::StreamConfig sc;
      sc.outputDevice = c.deviceIndex;
      sc.outputEndpointId = c.device;
      sc.outputChannels = std::max(c.outputChannels, 1u);
      sc.inputChannels = c.inputChannels;  // T6.7b: ASIO duplex, WASAPI shared capture (bounded by the device)
      sc.inputEndpointId = c.inputDevice;
      if (c.backend == engine::Backend::Wasapi && c.inputDevice.empty()) sc.inputChannels = 0;
      sc.sampleRate = c.sampleRate;
      sc.bufferFrames = c.bufferFrames;
      sc.shareMode = c.shareMode;
      auto opened = dev->open(sc, {&AudioOutput::onDevice, this});
      if (!opened) {
        why = opened.error().message;
      } else {
        const engine::StreamInfo& si = *opened;
        if (!prepareAdapter(si.inputChannels, si.outputChannels, block, si.bufferFrames)) {
          why = "invalid stream configuration";
          dev->close();
        } else {
          eng.liveInput().setLatency(si.bufferFrames + adapter_.latencyFrames());  // T6.4: one device period
          eng.recorder().setDeviceLatency(si.inputLatencyFrames + adapter_.latencyFrames(),  // T6.7
                                          (si.latencyFrames > 0 ? si.latencyFrames : si.bufferFrames) + adapter_.latencyFrames());
          // T6.7b: a measured round trip (WASAPI capture) replaces the input figure once the audio thread has it.
          recOut_ = (si.latencyFrames > 0 ? si.latencyFrames : si.bufferFrames) + adapter_.latencyFrames();
          recAdapter_ = adapter_.latencyFrames();
          lastRoundTrip_ = -1;
          if (!si.inputError.empty()) std::printf("pqdaw: audio input: %s\n", si.inputError.c_str());
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
          devStats_.store(&dev->stats(), std::memory_order_relaxed);
          {
            std::lock_guard lock(mutex_);
            device_ = std::move(dev);
            status_.stream = si;
            status_.deviceName = si.outputDeviceName;
            status_.adapterLatencyFrames = adapter_.latencyFrames();
            status_.rateMismatch = si.sampleRate != engineRate;
            status_.asioInfo = std::move(asioInfo);
            inputNote_ = si.inputError.empty() ? std::string() : "input: " + si.inputError;  // T6.7b
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
  fadeOut(device_ && device_->isRunning());  // AU.stream: no cut while audio sounds
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
  devStats_.store(nullptr, std::memory_order_relaxed);
}

bool AudioOutput::startStream(bool notifyUi) {
  if (!device_ || streamRunning_.load(std::memory_order_relaxed)) return streamRunning_.load(std::memory_order_relaxed);
  engine::Engine& eng = *engine_.load();
  if (fallback_) {  // the null device has no driver to settle
    fade_.store(kFadeNone, std::memory_order_relaxed);
    eng.audioOnline();
  } else {  // AU.stream: a cold start: the engine stays offline until the pre-roll ends (on the audio thread)
    eng.audioOffline();
    armPreroll(status().stream);
  }
  const Status st = device_->start();
  if (!st.ok()) {
    prerollArmed_.store(false, std::memory_order_relaxed);
    prerolling_.store(false, std::memory_order_release);
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
  fadeOut(device_->isRunning());  // AU.stream: no cut while audio sounds (a suspend is silent: it returns at once)
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
  clock::time_point idleSince{}, openedAt{};
  bool wasIdle = false;  // the engine asleep, nothing else sounding, the window in the background
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
    wasIdle = false;
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
               toneOn_.load(std::memory_order_relaxed) || extra_.load(std::memory_order_relaxed) != nullptr ||
               cue_.load(std::memory_order_relaxed) != nullptr;
      });
      if (quit_ || closeRequest_ || pending_ || hwChanged_ || panelRequest_) continue;
      wakeRequest_ = false;
      wasIdle = false;
      lock.unlock();
      engine::streamTrace().push(engine::TraceKind::Resume);
      const bool ok = startStream(false);  // a resume: the UI is not told (an open window polls the status)
      lock.lock();
      if (!ok) pending_ = config_;  // the device went away while suspended: reopen (or fall back)
      continue;
    }
    cv_.wait_for(lock, std::chrono::milliseconds(200),
                 [&] { return quit_ || closeRequest_ || pending_.has_value() || hwChanged_ || panelRequest_; });
    drainTrace(lock);
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
    // AU.stream: no suspend while the app is in use: only after backgroundSuspendSeconds of the engine asleep with the
    // window in the background.
    if (foreground_ || wakeRequest_ || toneOn_.load(std::memory_order_relaxed) ||
        extra_.load(std::memory_order_relaxed) != nullptr || cue_.load(std::memory_order_relaxed) != nullptr ||
        keepRunning_.load(std::memory_order_relaxed) ||
        !engine_.load()->asleep()) {
      wakeRequest_ = false;
      wasIdle = false;
      continue;
    }
    const auto now = clock::now();
    if (!wasIdle) {
      wasIdle = true;
      idleSince = now;
      continue;
    }
    if (std::chrono::duration<double>(now - idleSince).count() < factory_.backgroundSuspendSeconds) continue;
    // Idle in the background long enough: stop the stream (the next wake or the foreground restarts it).
    lock.unlock();
    engine::streamTrace().push(engine::TraceKind::Suspend);
    stopStream(true);
    lock.lock();
    drainTrace(lock);
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

void AudioOutput::drainTrace(std::unique_lock<std::mutex>& lock) {
  engine::StreamTrace& tr = engine::streamTrace();
  if (tr.head() == traceCursor_) return;
  lock.unlock();
  std::vector<engine::TraceEvent> ev;
  uint64_t lost = 0;
  const uint64_t from = traceCursor_;
  traceCursor_ = tr.read(from, ev, &lost);
  traceFmt_.summarise = false;
  traceFmt_.buffersAfterStart = 3;
  const std::string text = engine::describeStreamTrace(ev, traceT0_, traceFmt_);
  if (!text.empty() || lost) {
    if (lost) std::printf("pqdaw: audio trace: %llu events overwritten before they were read\n", static_cast<unsigned long long>(lost));
    size_t at = 0;
    while (at < text.size()) {
      const size_t nl = text.find('\n', at);
      const size_t end = nl == std::string::npos ? text.size() : nl;
      std::printf("pqdaw: audio trace: %.*s\n", static_cast<int>(end - at), text.data() + at);
      at = end + 1;
    }
    std::fflush(stdout);
  }
  lock.lock();
}

}  // namespace pq::app
