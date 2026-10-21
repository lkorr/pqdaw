#include "pq/engine/rtaudio_device.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#include <RtAudio.h>

#include "pq/engine/callback_runner.h"

namespace pq::engine {

struct RtAudioDevice::Impl {
  Backend backend = Backend::RtAudioWasapi;
  RtAudio::Api api = RtAudio::WINDOWS_WASAPI;
  std::unique_ptr<RtAudio> rt;
  CallbackRunner runner;
  uint32_t numIn = 0;
  uint32_t numOut = 0;

  mutable std::mutex messageMutex;  // RtAudio's error callback runs on the control thread, or on RtAudio's own
  std::string lastMessage;          // thread after its stream loop has exited: never inside our audio callback.

  std::string message() const {
    std::lock_guard lock(messageMutex);
    return lastMessage;
  }
  Error deviceError(const char* what) const {
    std::string text = std::string(what);
    if (const std::string m = message(); !m.empty()) text += ": " + m;
    return Error{ErrorCode::Device, std::move(text)};
  }
};

namespace {

RtAudio::Api toApi(Backend b) noexcept {
  switch (b) {
    case Backend::RtAudioWasapi: return RtAudio::WINDOWS_WASAPI;
    case Backend::Asio:  // our own host (AsioDevice); RtAudio's ASIO backend is not built
    case Backend::Null:
    case Backend::Wasapi: break;
  }
  return RtAudio::UNSPECIFIED;
}

bool apiCompiled(RtAudio::Api api) {
  std::vector<RtAudio::Api> apis;
  RtAudio::getCompiledApi(apis);
  return std::find(apis.begin(), apis.end(), api) != apis.end();
}

// RtAudio's callback, on its audio thread. With RTAUDIO_NONINTERLEAVED the buffers hold each channel's nFrames
// samples back to back, so the planar pointers are base + c · nFrames: no conversion or copy of ours. RtAudio
// itself still converts and copies around this call (WASAPI: interleave, resampler pass-through, its own ring
// buffer; review M0 H1/L1, docs/QUESTIONS.md Q12): Backend::Wasapi (WasapiDevice) avoids all of that.
int rtAudioCallback(void* outputBuffer, void* inputBuffer, unsigned int nFrames, double /*streamTime*/,
                    RtAudioStreamStatus status, void* user) noexcept {  // RT-safe
  auto* impl = static_cast<RtAudioDevice::Impl*>(user);
  const float* in[kMaxChannels];
  float* out[kMaxChannels];
  const auto* inBase = static_cast<const float*>(inputBuffer);
  auto* outBase = static_cast<float*>(outputBuffer);
  for (uint32_t c = 0; c < impl->numIn; ++c) in[c] = inBase + static_cast<std::size_t>(c) * nFrames;
  for (uint32_t c = 0; c < impl->numOut; ++c) out[c] = outBase + static_cast<std::size_t>(c) * nFrames;
  impl->runner.run(in, out, nFrames, (status & RTAUDIO_INPUT_OVERFLOW) != 0,
                   (status & RTAUDIO_OUTPUT_UNDERFLOW) != 0);
  return 0;
}

}  // namespace

bool backendCompiled(Backend b) noexcept {
  if (b == Backend::Null || b == Backend::Wasapi || b == Backend::Asio) return true;
  return apiCompiled(toApi(b));
}

Result<std::unique_ptr<RtAudioDevice>> RtAudioDevice::create(Backend backend) {
  const RtAudio::Api api = toApi(backend);
  if (api == RtAudio::UNSPECIFIED)
    return Error{ErrorCode::InvalidArgument, "RtAudioDevice: backend must be RtAudioWasapi"};
  if (!apiCompiled(api))
    return Error{ErrorCode::Unsupported, std::string("RtAudio was built without ") + backendName(backend)};
  auto impl = std::make_unique<Impl>();
  impl->backend = backend;
  impl->api = api;
  Impl* raw = impl.get();
  impl->rt = std::make_unique<RtAudio>(api, [raw](RtAudioErrorType, const std::string& text) {
    std::lock_guard lock(raw->messageMutex);
    raw->lastMessage = text;
  });
  if (impl->rt->getCurrentApi() != api)
    return Error{ErrorCode::Device, std::string("RtAudio could not start the ") + backendName(backend) + " API"};
  return std::unique_ptr<RtAudioDevice>(new RtAudioDevice(std::move(impl)));
}

RtAudioDevice::RtAudioDevice(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RtAudioDevice::~RtAudioDevice() { close(); }

Backend RtAudioDevice::backend() const noexcept { return impl_->backend; }

std::string RtAudioDevice::lastBackendMessage() const { return impl_->message(); }

Result<std::vector<DeviceInfo>> RtAudioDevice::enumerate() {
  std::vector<DeviceInfo> out;
  for (unsigned int id : impl_->rt->getDeviceIds()) {
    const RtAudio::DeviceInfo ri = impl_->rt->getDeviceInfo(id);
    DeviceInfo d;
    d.id = ri.ID;
    d.name = ri.name;
    d.outputChannels = ri.outputChannels;
    d.inputChannels = ri.inputChannels;
    d.isDefaultOutput = ri.isDefaultOutput;
    d.isDefaultInput = ri.isDefaultInput;
    d.preferredSampleRate = ri.preferredSampleRate ? ri.preferredSampleRate : ri.currentSampleRate;
    d.sampleRates.assign(ri.sampleRates.begin(), ri.sampleRates.end());
    out.push_back(std::move(d));
  }
  return out;
}

Result<StreamInfo> RtAudioDevice::open(const StreamConfig& config, AudioCallback cb) {
  RtAudio& rt = *impl_->rt;
  if (rt.isStreamOpen()) return Error{ErrorCode::InvalidArgument, "RtAudioDevice: a stream is already open"};
  if (!cb) return Error{ErrorCode::InvalidArgument, "RtAudioDevice: no callback"};
  if (config.shareMode == ShareMode::Exclusive)
    return Error{ErrorCode::Unsupported,
                 "WASAPI exclusive mode is not available through RtAudio 6.0.1 (shared mode only); use "
                 "Backend::Wasapi (--backend wasapi) for exclusive mode"};
  if (config.outputChannels == 0 && config.inputChannels == 0)
    return Error{ErrorCode::InvalidArgument, "RtAudioDevice: a stream needs input or output channels"};
  if (config.outputChannels > kMaxChannels || config.inputChannels > kMaxChannels)
    return Error{ErrorCode::InvalidArgument, "RtAudioDevice: too many channels"};
  if (config.bufferFrames == 0) return Error{ErrorCode::InvalidArgument, "RtAudioDevice: bufferFrames must be >= 1"};

  // RtAudio device ids start at 1; getDefault*Device() returns 0 when there is none.
  const unsigned int outId = config.outputChannels == 0 ? 0
                             : config.outputDevice == kDefaultDevice ? rt.getDefaultOutputDevice()
                                                                     : config.outputDevice;
  const unsigned int inId = config.inputChannels == 0 ? 0
                            : config.inputDevice == kDefaultDevice ? rt.getDefaultInputDevice()
                                                                   : config.inputDevice;
  if (config.outputChannels && outId == 0) return Error{ErrorCode::NotFound, "no output device"};
  if (config.inputChannels && inId == 0) return Error{ErrorCode::NotFound, "no input device"};

  const unsigned int refId = config.outputChannels ? outId : inId;
  const std::vector<unsigned int> ids = rt.getDeviceIds();
  if (std::find(ids.begin(), ids.end(), refId) == ids.end() ||
      (inId && std::find(ids.begin(), ids.end(), inId) == ids.end()))
    return Error{ErrorCode::NotFound, "RtAudioDevice: unknown device id (see --list-devices)"};
  const RtAudio::DeviceInfo ref = rt.getDeviceInfo(refId);
  const uint32_t deviceRate = ref.preferredSampleRate ? ref.preferredSampleRate : ref.currentSampleRate;
  const uint32_t rate = config.sampleRate ? config.sampleRate : (deviceRate ? deviceRate : 48000u);

  RtAudio::StreamParameters outParams{};
  outParams.deviceId = outId;
  outParams.nChannels = config.outputChannels;
  RtAudio::StreamParameters inParams{};
  inParams.deviceId = inId;
  inParams.nChannels = config.inputChannels;
  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_NONINTERLEAVED | RTAUDIO_SCHEDULE_REALTIME;
  options.priority = THREAD_PRIORITY_TIME_CRITICAL;  // WASAPI also joins MMCSS "Pro Audio"
  options.streamName = "pqdaw";

  impl_->numIn = config.inputChannels;
  impl_->numOut = config.outputChannels;
  unsigned int frames = config.bufferFrames;
  {
    std::lock_guard lock(impl_->messageMutex);
    impl_->lastMessage.clear();
  }
  const RtAudioErrorType err =
      rt.openStream(config.outputChannels ? &outParams : nullptr, config.inputChannels ? &inParams : nullptr,
                    RTAUDIO_FLOAT32, rate, &frames, &rtAudioCallback, impl_.get(), &options);
  if (err != RTAUDIO_NO_ERROR) return impl_->deviceError("RtAudio openStream failed");

  StreamInfo info;
  info.backend = impl_->backend;
  info.outputDeviceName = ref.name;
  info.sampleRate = rt.getStreamSampleRate();
  info.bufferFrames = frames;
  info.inputChannels = config.inputChannels;
  info.outputChannels = config.outputChannels;
  info.latencyFrames = static_cast<uint32_t>(std::max(0L, rt.getStreamLatency()));
  // WASAPI shared mode runs at the engine mix rate; RtAudio converts anything else with Media Foundation.
  info.backendResampling = impl_->backend == Backend::RtAudioWasapi && deviceRate != 0 && info.sampleRate != deviceRate;
  // Callbacks begin only after startStream(), so preparing the runner now (with the actual buffer size) is safe.
  impl_->runner.prepare(cb, config.inputChannels, config.outputChannels, frames, info.sampleRate);
  return info;
}

Status RtAudioDevice::start() {
  if (!impl_->rt->isStreamOpen()) return Error{ErrorCode::InvalidArgument, "RtAudioDevice: no open stream"};
  if (impl_->rt->startStream() != RTAUDIO_NO_ERROR) return impl_->deviceError("RtAudio startStream failed");
  return Ok{};
}

Status RtAudioDevice::stop() {
  if (!impl_->rt->isStreamRunning()) return Ok{};
  if (impl_->rt->stopStream() != RTAUDIO_NO_ERROR) return impl_->deviceError("RtAudio stopStream failed");
  return Ok{};
}

void RtAudioDevice::close() noexcept {
  if (impl_ && impl_->rt && impl_->rt->isStreamOpen()) impl_->rt->closeStream();  // stops a running stream first
}

bool RtAudioDevice::isOpen() const noexcept { return impl_->rt->isStreamOpen(); }
bool RtAudioDevice::isRunning() const noexcept { return impl_->rt->isStreamRunning(); }
const CallbackStats& RtAudioDevice::stats() const noexcept { return impl_->runner.stats(); }

}  // namespace pq::engine
