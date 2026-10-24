#pragma once

// The DAW window's audio output (T3.17): the default device of the chosen backend (our WASAPI by default; ASIO with
// --asio / --backend asio; T3.18 adds the preferences UI), a BlockAdapter at the engine's block, and the engine's
// callback behind a safety clamp to [−1, 1]. When no device can be opened, a NullDevice keeps the engine running in
// real time (compiles are taken, the playhead moves, silently).
//
// One control thread owns the device (COM objects stay on the thread that made them):
//   1. opens and starts it in the background, so the first frame does not wait for the driver;
//   2. idle: once the engine has been asleep (engine.h's idle gate) for kSuspendAfter, it stops the stream and
//      prepares the restart (WASAPI's client reset, which can take ~350 ms, happens now rather than on play); a
//      stopped stream costs nothing: no device callbacks, and the control thread blocks without a timeout;
//   3. any engine wake (transport, param write, audition, publish: Engine::setWakeHook) restarts it at once.
// So a stopped DAW uses no CPU for audio, and play after idle starts within one stream start (a few ms).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "pq/app/cli.h"
#include "pq/engine/audio_device.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/engine.h"

namespace pq::app {

class AudioOutput {
public:
  static constexpr double kSuspendAfterSeconds = 1.0;

  // Starts the control thread; `engine` must outlive the output.
  AudioOutput(const CliOptions& options, engine::Engine& engine);
  ~AudioOutput();  // stops and closes the device (the engine is no longer called afterwards)
  AudioOutput(const AudioOutput&) = delete;
  AudioOutput& operator=(const AudioOutput&) = delete;

  bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }  // the open finished
  bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  uint64_t suspensions() const noexcept { return suspensions_.load(std::memory_order_relaxed); }

private:
  static void onDevice(void* self, const engine::AudioBlock& b) noexcept;  // RT-safe
  static void onEngine(void* self, const engine::AudioBlock& b) noexcept;  // RT-safe
  static void onWake(void* self);
  void controlMain(CliOptions options);
  bool open(const CliOptions& o);

  engine::Engine& engine_;
  std::unique_ptr<engine::AudioDevice> device_;
  engine::BlockAdapter adapter_;
  std::thread control_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool quit_ = false;
  bool wakeRequest_ = false;
  std::atomic<bool> ready_{false}, running_{false};
  std::atomic<uint64_t> suspensions_{0};
  std::string description_;
};

}  // namespace pq::app
