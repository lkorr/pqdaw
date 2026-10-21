#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/engine/audio_block.h"
#include "pq/engine/callback_stats.h"

namespace pq::engine {

enum class Backend : uint8_t {
  Null,    // NullDevice: no hardware; driven manually or by a timer thread (tests, offline use)
  Wasapi,  // RtAudio WASAPI (shared mode only, see ShareMode)
  Asio,    // RtAudio ASIO (only when built with PQ_WITH_ASIO=ON)
};

const char* backendName(Backend b) noexcept;

// True if this binary can open devices on the backend (Null and Wasapi always; Asio only with PQ_WITH_ASIO).
bool backendCompiled(Backend b) noexcept;

enum class ShareMode : uint8_t {
  Shared,
  // WASAPI exclusive mode. Not available: RtAudio 6.0.1 (and its master branch) initialises WASAPI streams in
  // shared mode only. open() returns ErrorCode::Unsupported rather than silently opening a shared stream.
  // docs/QUESTIONS.md, T0.6 lists the options.
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
};

struct StreamConfig {
  uint32_t outputDevice = kDefaultDevice;
  uint32_t outputChannels = 2;
  uint32_t inputDevice = kDefaultDevice;
  uint32_t inputChannels = 0;  // 0 = output-only stream
  // 0 = the output device's preferred rate. On WASAPI shared mode, any other rate makes RtAudio resample with
  // Media Foundation on the audio thread, which allocates per callback: StreamInfo::backendResampling says so.
  uint32_t sampleRate = 0;
  uint32_t bufferFrames = 256;  // frames per device callback (SPEC §2: 64–256 typical); the backend may adjust
  ShareMode shareMode = ShareMode::Shared;
};

struct StreamInfo {
  Backend backend = Backend::Null;
  std::string outputDeviceName;
  uint32_t sampleRate = 0;
  uint32_t bufferFrames = 0;  // actual frames per callback
  uint32_t inputChannels = 0;
  uint32_t outputChannels = 0;
  uint32_t latencyFrames = 0;      // as reported by the backend (0 if it does not report)
  bool backendResampling = false;  // the backend converts sample rate on the audio thread (not RT-safe)
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
