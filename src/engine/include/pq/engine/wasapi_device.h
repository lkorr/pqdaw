#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pq/engine/audio_device.h"

namespace pq {
class RtLog;
}

namespace pq::engine {

// Our own WASAPI output backend (T0.6-fix1, review finding H1). <windows.h>/COM headers stay in the .cpp.
//
// Stream setup (open(), control thread):
// - Exclusive: IAudioClient::Initialize(EXCLUSIVE, EVENTCALLBACK) with period = buffer duration = the requested
//   bufferFrames, rounded up to the device's minimum period (GetDevicePeriod) and re-tried at the driver's
//   alignment on AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED. Format negotiation at the stream rate: float32, int32,
//   24-in-32, packed int24, int16 (first one IsFormatSupported accepts), first with the requested channel count,
//   then with the device's mix channel count (extra device channels get silence).
// - Shared: IAudioClient3::InitializeSharedAudioStream with the requested period clamped to the engine's
//   [min, max] and rounded to its fundamental period (low-latency shared periods where the driver offers them),
//   in the mix format. A rate other than the mix rate uses AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM: the conversion
//   then runs in the Windows audio engine's process, not on our thread.
// - Output only: StreamConfig::inputChannels > 0 returns ErrorCode::Unsupported (Backend::RtAudioWasapi does
//   input until duplex is added; docs/QUESTIONS.md).
//
// Audio thread (start()): joins MMCSS "Pro Audio" and COM (MTA) once, pre-fills the endpoint buffer by running
// the callback, starts the client, then per device event: one WaitForSingleObject (with a timeout of 4 periods),
// GetCurrentPadding (shared), IAudioClock::GetPosition, UnderrunTracker, GetBuffer, the callback via
// CallbackRunner once per period (planar float scratch → one SIMD conversion pass into the device buffer: no
// ring, no extra copies), ReleaseBuffer. The callback always sees exactly bufferFrames frames, so BlockAdapter
// runs in direct mode when bufferFrames is a multiple of the engine block.
//
// RT safety: nothing on the per-event path allocates through operator new (the whole wake is inside an RtScope,
// so the allocation trap covers our code). The trap cannot see allocations inside the WASAPI calls themselves
// (they would use the OS heap directly); GetBuffer/ReleaseBuffer/GetCurrentPadding/GetPosition are the calls
// Microsoft documents for the render loop.
//
// Underruns: UnderrunTracker (lateness vs the wall-clock schedule, empty shared buffer, device clock past the
// frames written, event timeouts), counted as CallbackStats::outputUnderflows, one per episode. Exclusive mode
// then restarts the stream with one extra period queued (Stop, GetBuffer/ReleaseBuffer, Start: ~0.4 ms of driver
// calls, CallbackStats::restarts): measured on a Focusrite USB driver, a device that once ran dry otherwise
// stays without a reserve and keeps dropping audio whenever an event comes late.
class WasapiDevice final : public AudioDevice {
public:
  WasapiDevice();
  ~WasapiDevice() override;

  Backend backend() const noexcept override { return Backend::Wasapi; }
  // Every active render and capture endpoint. ids are 1-based positions in the system's endpoint collection,
  // valid within this process while the device set is unchanged; endpointId is stable.
  Result<std::vector<DeviceInfo>> enumerate() override;
  // Must be called on the thread that later calls close() (it initialises COM there for the stream's lifetime).
  Result<StreamInfo> open(const StreamConfig& config, AudioCallback cb) override;
  Status start() override;
  Status stop() override;
  // After stop(): resets the client now (what the next start() would do first; ~350 ms on some drivers), so a
  // later start() is quick (T3.17: the DAW stops an idle stream and restarts it on the next play).
  Status prepareRestart();
  void close() noexcept override;
  bool isOpen() const noexcept override;
  bool isRunning() const noexcept override;
  const CallbackStats& stats() const noexcept override;

  // Test hook for underrun detection: once, right after the wake number `wake` (1-based) has written its data,
  // the audio thread sleeps for 3 device periods (outside its RtScope). 0 disables. Call before start().
  void injectStallAfterWake(uint64_t wake) noexcept;

  // Optional diagnostics: the audio thread pushes one record per underrun episode (code kLogUnderrun) and per
  // recovery restart (kLogRestart) into `log` (one producer: this device's audio thread). Call before start();
  // the log must outlive the stream.
  static constexpr uint32_t kLogUnderrun = 0x57A1;
  static constexpr uint32_t kLogRestart = 0x57A2;
  void setRtLog(RtLog* log) noexcept;

  // The error that ended the audio thread early (e.g. the device was removed), empty if none.
  std::string threadError() const;

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

// Shared-mode WASAPI loopback capture of a render endpoint (what the Windows audio engine mixed for it), for
// latency measurement. Non-realtime: a polling thread copies packets into preallocated storage. Mono view: the
// first channel only.
class LoopbackCapture {
public:
  struct Packet {
    uint64_t firstFrame = 0;  // index into samples() of the packet's first frame
    uint32_t frames = 0;
    uint64_t qpc100ns = 0;    // QPC time of the packet's first frame, in 100 ns units (as WASAPI reports it)
    uint32_t flags = 0;       // AUDCLNT_BUFFERFLAGS_*
  };

  LoopbackCapture();
  ~LoopbackCapture();
  // endpointId empty = the default render endpoint. Captures at most maxSeconds of audio.
  Status start(const std::string& endpointId, double maxSeconds);
  Status stop();
  uint32_t sampleRate() const noexcept;
  const std::vector<float>& samples() const noexcept;  // valid after stop()
  const std::vector<Packet>& packets() const noexcept;

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

// Audio endpoint changes (T3.18: device hot-plug): an IMMNotificationClient registered with the system's device
// enumerator. `fn(user)` is called on a system thread whenever an endpoint is added, removed, enabled / disabled or
// becomes a default device, and whenever an endpoint's properties change (a USB interface that also has an ASIO
// driver shows up here as well, so the DAW uses it as its generic "the hardware changed" signal). Event driven:
// nothing runs between changes. fn must be thread-safe and return quickly (set a flag, notify a thread).
// start()/stop() must be called on the same thread (COM is initialised there for the registration's lifetime).
class DeviceChangeNotifier {
public:
  using ChangedFn = void (*)(void* user);
  DeviceChangeNotifier();
  ~DeviceChangeNotifier();  // stop()
  DeviceChangeNotifier(const DeviceChangeNotifier&) = delete;
  DeviceChangeNotifier& operator=(const DeviceChangeNotifier&) = delete;
  Status start(ChangedFn fn, void* user);
  void stop() noexcept;  // unregisters; returns once no fn call is running (none starts afterwards)
  bool active() const noexcept;

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

// QueryPerformanceCounter in 100 ns units (the time base WASAPI uses for its QPC positions). Not RT-unsafe (a
// user-mode counter read), but kept out of the engine's hot path; used by measurement code.
uint64_t qpcNow100ns() noexcept;

}  // namespace pq::engine
