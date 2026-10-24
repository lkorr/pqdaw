#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pq/engine/audio_device.h"

namespace pq::engine {

namespace asio {
struct Driver;
}

// One registered ASIO driver: HKLM\SOFTWARE\ASIO\<name>, value CLSID.
struct AsioDriverEntry {
  std::string name;
  std::string clsid;  // "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"
};

// Where AsioDevice finds and creates drivers. The default (registry()) reads HKLM\SOFTWARE\ASIO and creates the
// driver with CoCreateInstance; tests substitute a fake driver. Both functions run on AsioDevice's control
// thread (never on the audio thread).
struct AsioDriverSource {
  std::function<std::vector<AsioDriverEntry>()> list;
  // Returns the driver object with one reference owned by the caller (released with Release()).
  std::function<Result<asio::Driver*>(const AsioDriverEntry&)> create;

  static AsioDriverSource registry();
};

// Counters of driver requests and how they were handled (process-wide: ASIO host callbacks carry no context).
struct AsioEvents {
  uint64_t resetRequests = 0;      // kMsgResetRequest
  uint64_t bufferSizeChanges = 0;  // kMsgBufferSizeChange (handled as a reset)
  uint64_t resyncRequests = 0;     // kMsgResyncRequest: the driver lost samples (counted as xruns by --sine)
  uint64_t latencyChanges = 0;     // kMsgLatenciesChanged (latencies re-queried)
  uint64_t rateChanges = 0;        // sampleRateDidChange
  uint64_t overloads = 0;          // kMsgOverload
  uint64_t reopens = 0;            // successful stop → release → reload → recreate cycles
  uint64_t failedReopens = 0;      // reopens that failed (the stream is then closed; lastError() says why)
};

// Stream details only an ASIO driver has.
struct AsioStreamDetails {
  std::string driverName;            // registry name
  int32_t driverVersion = 0;
  int32_t outputSampleType = -1;     // asio::SampleType of output channel 0 (-1: no outputs)
  int32_t inputSampleType = -1;
  int32_t minBufferFrames = 0, maxBufferFrames = 0, preferredBufferFrames = 0, bufferGranularity = 0;
  bool outputReady = false;          // the driver supports outputReady(): called after every output write
  uint32_t driverOutputs = 0, driverInputs = 0;  // channels the driver has (the stream uses the first N)
};

// Picks the buffer size to request from the driver's getBufferSize() answer: the smallest valid size ≥
// `requested` (0 = preferred), or the largest valid size if none is ≥ it. Valid sizes: granularity -1 = powers
// of two in [min, max]; 0 = preferred only; n > 0 = min + k·n ≤ max. Falls back to preferred on a nonsensical
// answer.
uint32_t asioChooseBufferFrames(int32_t minSize, int32_t maxSize, int32_t preferred, int32_t granularity,
                                uint32_t requested) noexcept;

// Our own ASIO host (T0.6-fix2): an independent reimplementation of the host side of the ASIO driver ABI
// (pq/engine/asio_abi.h; no Steinberg SDK files), behind the same AudioDevice interface and CallbackRunner as
// WasapiDevice. <windows.h>/COM stay in the .cpp.
//
// Threads:
// - A control thread owned by the device: COM single-threaded apartment (drivers are apartment-threaded), a
//   hidden window (the driver's system handle) and a message pump (drivers that show their control panel inside
//   the process need it). Every driver call except the audio-path ones runs there: enumerate/open/start/stop/
//   close/controlPanel marshal to it and block until done. It also polls (every 50 ms while the stream runs,
//   250 ms while a driver is loaded but stopped) the request flags the driver's messages set, and handles them
//   off the audio thread: a reset request, a
//   buffer-size change or a sample-rate change → stop, disposeBuffers, Release, reload, init, recreate the
//   stream (same configuration; after a rate change, at the driver's new rate), restart if it was running;
//   latencies changed → re-query.
// - The driver's audio thread calls bufferSwitchTimeInfo (or bufferSwitch) per buffer half: inside an RtScope,
//   input halves are converted to planar float (float32 channels are passed by pointer, no copy), the callback
//   runs once via CallbackRunner, output is converted into the half the driver gave (float32 outputs are written
//   in place), then outputReady() if the driver supports it. A sample position that jumped by more than 1.5
//   buffers since the previous switch is counted as an output underflow (a missed buffer).
//   The message callback is RT-safe: it only sets flags and bumps counters.
//
// Only one ASIO driver can be loaded per process (the ABI's callbacks have no context pointer): a second
// AsioDevice that tries to load one gets ErrorCode::Device. Device ids are 1-based positions in the driver
// list; StreamConfig::outputEndpointId may name the driver instead (the registry name, case-insensitive).
// kDefaultDevice = the first driver in the list that loads. The stream's channels are the driver's first
// outputChannels outputs and first inputChannels inputs (duplex on one driver; inputDevice is ignored).
// shareMode is ignored (ASIO drivers are exclusive by nature).
class AsioDevice final : public AudioDevice {
public:
  AsioDevice();  // AsioDriverSource::registry()
  explicit AsioDevice(AsioDriverSource source);
  ~AsioDevice() override;

  Backend backend() const noexcept override { return Backend::Asio; }
  // Every registered driver. Each one is loaded briefly to read its channels, rates, buffer sizes and sample
  // type (DeviceInfo::probeError says why one could not be). While this device has a stream open, only the open
  // driver is queried (another cannot be loaded).
  Result<std::vector<DeviceInfo>> enumerate() override;
  // The registered drivers without loading any.
  Result<std::vector<AsioDriverEntry>> drivers();
  Result<StreamInfo> open(const StreamConfig& config, AudioCallback cb) override;
  Status start() override;
  Status stop() override;
  void close() noexcept override;
  bool isOpen() const noexcept override;
  bool isRunning() const noexcept override;
  const CallbackStats& stats() const noexcept override;

  // Opens the driver's control panel (an open stream is needed: the driver must be loaded). Non-blocking for
  // most drivers; the control thread keeps pumping messages for in-process panels.
  Status controlPanel();
  // The current stream (it changes when a driver request made the device recreate the stream).
  StreamInfo streamInfo() const;
  AsioStreamDetails details() const;
  AsioEvents events() const;
  // Why the stream closed itself (a failed reopen), empty if it did not.
  std::string lastError() const;

  // Called on the control thread after the device recreated the stream (driver reset / rate change), before it
  // restarts it, with the new stream info: the owner can re-prepare for a new rate or buffer size there. The
  // audio callback is not running during the call.
  using StreamChangedFn = void (*)(void* user, const StreamInfo& info);
  void setStreamChangedCallback(StreamChangedFn fn, void* user);

  // Handles pending driver requests now (normally the control thread polls them every 50 / 250 ms). For tests.
  void handlePendingRequests();

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace pq::engine
