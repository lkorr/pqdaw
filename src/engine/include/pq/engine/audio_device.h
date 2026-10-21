#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/callback_stats.h"
#include "pq/engine/sample_convert.h"

namespace pq::engine {

enum class Backend : uint8_t {
  Null,           // NullDevice: no hardware; driven manually or by a timer thread (tests, offline use)
  Wasapi,         // our WASAPI backend (WasapiDevice): event-driven, exclusive and shared, output only
  RtAudioWasapi,  // RtAudio WASAPI (shared mode only; kept for comparison and for input streams)
  Asio,           // RtAudio ASIO (only when built with PQ_WITH_ASIO=ON)
};

const char* backendName(Backend b) noexcept;

// True if this binary can open devices on the backend (Null, Wasapi and RtAudioWasapi always; Asio only with
// PQ_WITH_ASIO).
bool backendCompiled(Backend b) noexcept;

enum class ShareMode : uint8_t {
  Shared,
  // WASAPI exclusive mode: Backend::Wasapi only. RtAudio 6.0.1 initialises WASAPI streams in shared mode only, so
  // RtAudioDevice::open() returns ErrorCode::Unsupported for it rather than silently opening a shared stream (Q12).
  Exclusive,
};

// Device id meaning "the backend's default device" in StreamConfig.
inline constexpr uint32_t kDefaultDevice = 0xFFFFFFFFu;

struct DeviceInfo {
  uint32_t id = 0;  // backend-specific id, valid for StreamConfig within this process
  std::string name;
  uint32_t outputChannels = 0;
  uint32_t inputChannels = 0;
  bool isDefaultOutput = false;
  bool isDefaultInput = false;
  uint32_t preferredSampleRate = 0;  // WASAPI: the shared-mode mix rate
  std::vector<uint32_t> sampleRates;
  // Backend::Wasapi only (empty / 0 otherwise):
  std::string endpointId;                      // IMMDevice id: stable across processes and reboots
  std::vector<SampleFormat> exclusiveFormats;  // exclusive-mode formats accepted at preferredSampleRate
  uint32_t minPeriodFrames = 0;                // exclusive-mode minimum period at preferredSampleRate
  uint32_t sharedMinPeriodFrames = 0;          // shared-mode (IAudioClient3) period range at the mix rate
  uint32_t sharedMaxPeriodFrames = 0;
};

struct StreamConfig {
  uint32_t outputDevice = kDefaultDevice;
  std::string outputEndpointId;  // Backend::Wasapi: when non-empty, selects the device instead of outputDevice
  uint32_t outputChannels = 2;
  uint32_t inputDevice = kDefaultDevice;
  uint32_t inputChannels = 0;  // 0 = output-only stream
  // 0 = the output device's preferred rate. On WASAPI shared mode, any other rate makes RtAudio resample with
  // Media Foundation on the audio thread, which allocates per callback: StreamInfo::backendResampling says so.
  uint32_t sampleRate = 0;
  uint32_t bufferFrames = 256;  // frames per device callback (SPEC §2: 64–256 typical); the backend may adjust
                                // (WASAPI: rounded up to the device's minimum period and alignment)
  ShareMode shareMode = ShareMode::Shared;
};

struct StreamInfo {
  Backend backend = Backend::Null;
  std::string outputDeviceName;
  uint32_t sampleRate = 0;
  uint32_t bufferFrames = 0;  // actual frames per callback
  uint32_t inputChannels = 0;
  uint32_t outputChannels = 0;
  uint32_t latencyFrames = 0;      // output latency as the backend can tell before running (0 if unknown);
                                   // WASAPI measures the real figure while running: CallbackStats meanQueueFrames
  bool backendResampling = false;  // the backend converts sample rate on the audio thread (not RT-safe)
  // Backend::Wasapi only:
  ShareMode shareMode = ShareMode::Shared;
  SampleFormat deviceFormat = SampleFormat::Float32;  // the device buffer's interleaved format
  uint32_t deviceChannels = 0;                        // channels in the device buffer (≥ outputChannels)
  uint32_t endpointBufferFrames = 0;                  // endpoint buffer capacity (shared) or one period (exclusive)
  std::string outputEndpointId;
};

// An audio device backend. open/start/stop/enumerate are non-RT virtuals called from a control thread.
// The audio path is not virtual: the backend calls a CallbackRunner, which calls the AudioCallback once per
// device buffer. A device object holds at most one open stream.
class AudioDevice {
public:
  virtual ~AudioDevice() = default;

  virtual Backend backend() const noexcept = 0;
  virtual Result<std::vector<DeviceInfo>> enumerate() = 0;
  // Opens a stream that calls cb once per device buffer with planar float buffers. cb.user must outlive the
  // stream (until close()).
  virtual Result<StreamInfo> open(const StreamConfig& config, AudioCallback cb) = 0;
  virtual Status start() = 0;
  virtual Status stop() = 0;  // returns after the last callback has finished
  virtual void close() noexcept = 0;
  virtual bool isOpen() const noexcept = 0;
  virtual bool isRunning() const noexcept = 0;
  // Stats of the current (or last) stream. Readable from any thread while the stream runs.
  virtual const CallbackStats& stats() const noexcept = 0;
};

// Creates a device for a backend. Errors: ErrorCode::Unsupported if the backend is not compiled in.
Result<std::unique_ptr<AudioDevice>> createAudioDevice(Backend backend);

}  // namespace pq::engine
