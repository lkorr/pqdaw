#pragma once

#include <memory>
#include <string>

#include "pq/engine/audio_device.h"

namespace pq::engine {

// AudioDevice on RtAudio 6.0.1 (WASAPI shared mode; ASIO when built with PQ_WITH_ASIO=ON). RtAudio's headers
// (and <windows.h>) stay inside the .cpp.
//
// Audio path: RtAudio's callback thread (WASAPI: MMCSS "Pro Audio") calls a static trampoline with
// non-interleaved float32 buffers (RTAUDIO_NONINTERLEAVED, so no conversion of ours), which builds the channel
// pointer arrays in fixed member arrays and calls CallbackRunner::run(). Nothing on that path allocates or locks.
//
// xruns: the counter adds up RtAudio's per-callback status flags. Caveat, verified in the RtAudio 6.0.1 source:
// the WASAPI backend only ever reports RTAUDIO_INPUT_OVERFLOW (capture data discontinuity) and never
// RTAUDIO_OUTPUT_UNDERFLOW, so an output underrun on WASAPI is not counted there. CallbackStats::deadlineMisses
// (a callback that took longer than one buffer period) and maxIntervalUs are the backend-independent signals.
// RtAudio's ASIO backend does flag xruns (when the driver sends kAsioResyncRequest). The --sine command also
// compares frames delivered against the device clock. See docs/QUESTIONS.md, T0.6.
class RtAudioDevice final : public AudioDevice {
public:
  // backend must be RtAudioWasapi or Asio. Errors: Unsupported if not compiled in.
  static Result<std::unique_ptr<RtAudioDevice>> create(Backend backend);
  ~RtAudioDevice() override;

  Backend backend() const noexcept override;
  Result<std::vector<DeviceInfo>> enumerate() override;
  Result<StreamInfo> open(const StreamConfig& config, AudioCallback cb) override;
  Status start() override;
  Status stop() override;
  void close() noexcept override;
  bool isOpen() const noexcept override;
  bool isRunning() const noexcept override;
  const CallbackStats& stats() const noexcept override;

  // The last error or warning text RtAudio reported (empty if none).
  std::string lastBackendMessage() const;

  struct Impl;

private:
  explicit RtAudioDevice(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace pq::engine
