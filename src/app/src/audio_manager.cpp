// The audio device manager (T3.18): pq/app/audio_manager.h describes it.
#include "pq/app/audio_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "pq/app/user_settings.h"

namespace pq::app {

using views::AudioDriverType;

namespace {

engine::Backend backendOf(AudioDriverType t) {
  return t == AudioDriverType::Asio ? engine::Backend::Asio : engine::Backend::Wasapi;
}

uint32_t engineRate(ProjectLink& link) { return static_cast<uint32_t>(std::lround(link.engine().config().sampleRate)); }

void addUnique(std::vector<uint32_t>& v, uint32_t x) {
  if (x != 0 && std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

// Valid ASIO buffer sizes from getBufferSize's answer (asioChooseBufferFrames' rules), at most ~24 of them.
std::vector<uint32_t> asioBufferSizes(const engine::DeviceInfo& d) {
  std::vector<uint32_t> v;
  const int32_t mn = d.minBufferFrames, mx = d.maxBufferFrames, g = d.bufferGranularity;
  if (mn <= 0 || mx < mn) {
    addUnique(v, static_cast<uint32_t>(std::max(0, d.preferredBufferFrames)));
    return v;
  }
  if (g == -1) {
    for (int64_t n = 1; n <= mx; n *= 2)
      if (n >= mn) v.push_back(static_cast<uint32_t>(n));
  } else if (g > 0) {
    const int64_t step = std::max<int64_t>(g, (static_cast<int64_t>(mx) - mn) / 23 / g * g);
    for (int64_t n = mn; n <= mx && v.size() < 24; n += step) v.push_back(static_cast<uint32_t>(n));
  }
  addUnique(v, static_cast<uint32_t>(std::max(0, d.preferredBufferFrames)));
  std::sort(v.begin(), v.end());
  return v;
}

}  // namespace

views::AudioSettings AudioDeviceManager::startupSettings(const CliOptions& o, const std::string& path,
                                                         std::vector<std::string>* warnings) {
  views::AudioSettings s;
  if (auto loaded = loadAudioSettings(path); loaded.ok()) {
    s = loaded->settings;
    if (warnings) *warnings = loaded->warnings;
  } else if (loaded.error().code != ErrorCode::NotFound && warnings) {
    warnings->push_back(loaded.error().message);
  }
  // Command-line device flags win for this run.
  if (o.asio) {
    if (s.driver != AudioDriverType::Asio) s.outputDevice.clear();
    s.driver = AudioDriverType::Asio;
    if (!o.deviceEndpoint.empty()) s.outputDevice = s.outputName = o.deviceEndpoint;
  } else if (o.backendGiven || o.exclusive || (!o.deviceEndpoint.empty() && o.deviceEndpoint.front() == '{')) {
    if (s.driver == AudioDriverType::Asio) s.outputDevice.clear();
    s.driver = o.exclusive ? AudioDriverType::WasapiExclusive : AudioDriverType::WasapiShared;
    if (!o.deviceEndpoint.empty()) s.outputDevice = o.deviceEndpoint;
  }
  if (o.device != engine::kDefaultDevice) s.outputDevice.clear();  // a numeric id (cliDeviceIndex)
  if (o.bufferGiven) s.bufferFrames = o.bufferFrames;
  if (o.rateGiven) s.sampleRate = o.sampleRate;
  return s;
}

OutputConfig AudioDeviceManager::outputConfigFor(const views::AudioSettings& s, uint32_t deviceIndex) {
  OutputConfig c;
  c.backend = backendOf(s.driver);
  c.shareMode = s.driver == AudioDriverType::WasapiExclusive ? engine::ShareMode::Exclusive : engine::ShareMode::Shared;
  c.device = s.outputDevice;
  c.deviceIndex = s.outputDevice.empty() ? deviceIndex : engine::kDefaultDevice;
  c.sampleRate = s.sampleRate;
  c.bufferFrames = s.bufferFrames;
  c.inputChannels = s.driver == AudioDriverType::Asio ? s.inputChannels : 0;
  c.outputChannels = std::max(s.outputChannels, 1u);
  return c;
}

AudioDeviceManager::AudioDeviceManager(ProjectLink& link, const views::AudioSettings& initial, ManagerHooks hooks,
                                       uint32_t cliDeviceIndex)
    : link_(link), hooks_(std::move(hooks)), settings_(initial), cliDeviceIndex_(cliDeviceIndex) {
  OutputConfig c = outputConfigFor(settings_, cliDeviceIndex_);
  c.sampleRate = engineRate(link_);
  settings_.sampleRate = c.sampleRate;
  output_ = std::make_unique<AudioOutput>(link_.engine(), c, hooks_.factory);
  output_->setNotify(&AudioDeviceManager::onOutput, this);
}

AudioDeviceManager::~AudioDeviceManager() {
  output_->setNotify(nullptr, nullptr);
  output_.reset();
}

void AudioDeviceManager::setUiNotify(void (*fn)(void*), void* user) {
  std::lock_guard lock(notifyMutex_);
  uiNotify_ = fn;
  uiNotifyUser_ = user;
}

void AudioDeviceManager::onOutput(void* self) {  // the output's threads (never the audio thread)
  auto& m = *static_cast<AudioDeviceManager*>(self);
  std::lock_guard lock(m.notifyMutex_);
  if (m.uiNotify_) m.uiNotify_(m.uiNotifyUser_);
}

bool AudioDeviceManager::poll() {
  const OutputStatus st = output_->status();
  bool changed = st.revision != seenRevision_;
  seenRevision_ = st.revision;
  if (st.hardwareChanges != seenHardware_) {
    seenHardware_ = st.hardwareChanges;
    deviceCache_.clear();
    asioCache_.reset();
    changed = true;
  }
  // The device runs at another rate than the engine (an ASIO driver changed its clock, or kept its own): follow it.
  if (st.rateMismatch && st.stream.sampleRate >= 8000 && st.stream.sampleRate != engineRate(link_) &&
      st.state != OutputStatus::State::Opening) {
    char msg[128];
    std::snprintf(msg, sizeof msg, "the device runs at %u Hz: the engine follows", st.stream.sampleRate);
    lastError_ = msg;
    std::printf("pqdaw: audio: %s\n", msg);
    settings_.sampleRate = st.stream.sampleRate;
    open(settings_);
    save();
    changed = true;
  }
  if (changed && listener_) listener_();
  return changed;
}

void AudioDeviceManager::open(const views::AudioSettings& s) {
  if (s.sampleRate != engineRate(link_)) {
    output_->close();  // the old engine is not called again
    if (Status st = link_.setSampleRate(s.sampleRate); !st.ok()) {
      lastError_ = st.error().message;
      std::fprintf(stderr, "pqdaw: audio: %s\n", lastError_.c_str());
    } else {
      ++rateChanges_;
    }
    output_->rebind(link_.engine());
  }
  OutputConfig c = outputConfigFor(s, cliDeviceIndex_);
  c.sampleRate = engineRate(link_);  // what the engine actually runs at (a failed rebuild kept the old rate)
  settings_.sampleRate = c.sampleRate;
  output_->reconfigure(c);
}

void AudioDeviceManager::apply(const views::AudioSettings& in) {
  views::AudioSettings s = in;
  s.outputChannels = std::max(s.outputChannels, 1u);
  if (s.driver != AudioDriverType::Asio) s.inputChannels = 0;  // WASAPI input opens with recording (M6)
  const OutputStatus st = output_->status();
  const bool noDevice = st.state == OutputStatus::State::Fallback || st.state == OutputStatus::State::Closed;
  if (s == settings_ && !noDevice) return;
  if (s.driver != settings_.driver || s.outputDevice != settings_.outputDevice) cliDeviceIndex_ = engine::kDefaultDevice;
  lastError_.clear();
  settings_ = s;
  open(settings_);
  save();
}

void AudioDeviceManager::save() {
  if (!hooks_.persist) return;
  const std::string path = hooks_.settingsPath.empty() ? userSettingsPath() : hooks_.settingsPath;
  if (Status st = saveAudioSettings(path, settings_); !st.ok()) {
    lastError_ = st.error().message;
    std::fprintf(stderr, "pqdaw: %s\n", lastError_.c_str());
  }
}

// ---- Device lists -------------------------------------------------------------------------------------------------

const std::vector<engine::DeviceInfo>& AudioDeviceManager::devices(engine::Backend b) {
  auto it = deviceCache_.find(b);
  if (it != deviceCache_.end()) return it->second;
  std::vector<engine::DeviceInfo> list;
  Result<std::vector<engine::DeviceInfo>> r = std::vector<engine::DeviceInfo>{};
  if (hooks_.enumerate) {
    r = hooks_.enumerate(b);
  } else {
    auto dev = hooks_.factory.create ? hooks_.factory.create(b) : engine::createAudioDevice(b);
    if (dev) r = (*dev)->enumerate();
    else r = std::move(dev).error();
  }
  if (r) list = std::move(*r);
  return deviceCache_.emplace(b, std::move(list)).first->second;
}

std::vector<AudioDriverType> AudioDeviceManager::driverTypes() {
  return {AudioDriverType::Asio, AudioDriverType::WasapiShared, AudioDriverType::WasapiExclusive};
}

std::vector<views::AudioDeviceEntry> AudioDeviceManager::outputDevices(AudioDriverType t) {
  std::vector<views::AudioDeviceEntry> out;
  if (t == AudioDriverType::Asio) {
    if (!asioCache_) asioCache_ = hooks_.asioDrivers ? hooks_.asioDrivers() : engine::AsioDriverSource::registry().list();
    const OutputStatus st = output_->status();
    for (const engine::AsioDriverEntry& e : *asioCache_) {
      views::AudioDeviceEntry d{e.name, e.name, 0, 0, false};
      if (st.asioInfo && st.asioInfo->name == e.name) {
        d.inputChannels = st.asioInfo->inputChannels;
        d.outputChannels = st.asioInfo->outputChannels;
      }
      out.push_back(std::move(d));
    }
    return out;
  }
  const auto& list = devices(engine::Backend::Wasapi);
  std::string def;
  for (const engine::DeviceInfo& d : list)
    if (d.isDefaultOutput) def = d.name;
  out.push_back({"", def.empty() ? std::string("System default") : "System default (" + def + ")", 0, 2, true});
  for (const engine::DeviceInfo& d : list)
    if (d.outputChannels > 0) out.push_back({d.endpointId, d.name, 0, d.outputChannels, d.isDefaultOutput});
  return out;
}

std::vector<views::AudioDeviceEntry> AudioDeviceManager::inputDevices(AudioDriverType t) {
  std::vector<views::AudioDeviceEntry> out;
  if (t == AudioDriverType::Asio) return out;  // the driver is the input too
  out.push_back({"", "No input", 0, 0, false});
  for (const engine::DeviceInfo& d : devices(engine::Backend::Wasapi))
    if (d.inputChannels > 0) out.push_back({d.endpointId, d.name, d.inputChannels, 0, d.isDefaultInput});
  return out;
}

views::AudioDeviceCaps AudioDeviceManager::capabilities(const views::AudioSettings& s) {
  views::AudioDeviceCaps c;
  if (s.driver == AudioDriverType::Asio) {
    const OutputStatus st = output_->status();
    const bool open = st.asioInfo && (s.outputDevice.empty() || st.asioInfo->name == s.outputDevice);
    if (open) {
      const engine::DeviceInfo& d = *st.asioInfo;
      c.sampleRates = d.sampleRates;
      c.bufferSizes = asioBufferSizes(d);
      c.maxInputChannels = d.inputChannels;
      c.maxOutputChannels = std::max(d.outputChannels, 1u);
      c.hasControlPanel = true;
    }
    return c;  // a driver that is not open yet: the window offers the common values (one driver per process)
  }
  c.inputSupported = false;  // WASAPI input opens with recording (M6)
  const engine::DeviceInfo* dev = nullptr;
  for (const engine::DeviceInfo& d : devices(engine::Backend::Wasapi))
    if (d.outputChannels > 0 && (s.outputDevice.empty() ? d.isDefaultOutput : d.endpointId == s.outputDevice)) dev = &d;
  if (!dev) return c;
  c.maxOutputChannels = std::max(dev->outputChannels, 1u);
  if (s.driver == AudioDriverType::WasapiExclusive) {
    c.sampleRates = dev->sampleRates;
    const uint32_t mn = std::max<uint32_t>(dev->minPeriodFrames, 16);
    for (uint32_t b : {mn, 128u, 144u, 192u, 256u, 288u, 384u, 480u, 512u, 960u, 1024u, 2048u})
      if (b >= mn) addUnique(c.bufferSizes, b);
  } else {
    // Shared mode runs at any rate (the Windows audio engine converts to the mix rate, in its own process).
    addUnique(c.sampleRates, dev->preferredSampleRate);
    for (uint32_t r : {44100u, 48000u, 88200u, 96000u}) addUnique(c.sampleRates, r);
    if (dev->sharedMinPeriodFrames > 0) {
      for (uint32_t k = 1; k <= 8; k *= 2)
        if (dev->sharedMinPeriodFrames * k <= std::max(dev->sharedMaxPeriodFrames, dev->sharedMinPeriodFrames))
          addUnique(c.bufferSizes, dev->sharedMinPeriodFrames * k);
    }
    for (uint32_t b : {256u, 480u, 512u, 1024u}) addUnique(c.bufferSizes, b);
  }
  std::sort(c.sampleRates.begin(), c.sampleRates.end());
  std::sort(c.bufferSizes.begin(), c.bufferSizes.end());
  return c;
}

// ---- Status -------------------------------------------------------------------------------------------------------

views::AudioStatus AudioDeviceManager::status() {
  const OutputStatus st = output_->status();
  views::AudioStatus a;
  switch (st.state) {
    case OutputStatus::State::Opening: a.state = views::AudioStatus::State::Opening; break;
    case OutputStatus::State::Running: a.state = views::AudioStatus::State::Running; break;
    case OutputStatus::State::Suspended: a.state = views::AudioStatus::State::Suspended; break;
    case OutputStatus::State::Fallback:
    case OutputStatus::State::Closed: a.state = views::AudioStatus::State::NoDevice; break;
  }
  a.deviceName = st.deviceName;
  a.message = st.message.empty() ? lastError_ : st.message;
  const engine::StreamInfo& si = st.stream;
  a.sampleRate = si.sampleRate;
  a.bufferFrames = si.bufferFrames;
  a.inputChannels = si.inputChannels;
  a.outputChannels = si.outputChannels;
  if (si.sampleRate > 0) {
    const double msPerFrame = 1000.0 / si.sampleRate;
    double outFrames = si.latencyFrames > 0 ? si.latencyFrames : si.bufferFrames;
    if (si.backend == engine::Backend::Wasapi && output_->running()) {  // measured on the device clock while running
      const auto cs = output_->callbackStats();
      if (cs.meanQueueFrames > 0) outFrames = cs.meanQueueFrames;
    }
    a.outputLatencyMs = (outFrames + st.adapterLatencyFrames) * msPerFrame;
    a.inputLatencyMs = si.inputChannels > 0 ? (si.inputLatencyFrames + st.adapterLatencyFrames) * msPerFrame : 0.0;
  }
  a.revision = st.revision;
  return a;
}

double AudioDeviceManager::cpuLoad() {
  const auto now = std::chrono::steady_clock::now();
  if (!output_->running()) {
    loadOpens_ = ~0ull;
    return 0.0;
  }
  const uint64_t opens = output_->status().opens;
  const auto cs = output_->callbackStats();
  const double busy = cs.meanDurationUs * static_cast<double>(cs.callbacks);
  double load = 0.0;
  if (opens == loadOpens_ && cs.callbacks >= loadCallbacks_) {
    const double dt = std::chrono::duration<double, std::micro>(now - loadAt_).count();
    if (dt > 0.0) load = std::clamp((busy - loadBusyUs_) / dt * 100.0, 0.0, 999.0);
  }
  loadOpens_ = opens;
  loadCallbacks_ = cs.callbacks;
  loadBusyUs_ = busy;
  loadAt_ = now;
  return load;
}

void AudioDeviceManager::setTestTone(bool on, float levelDb, float hz) { output_->setTestTone(on, levelDb, hz); }

bool AudioDeviceManager::openControlPanel() { return output_->controlPanel().ok(); }

}  // namespace pq::app
