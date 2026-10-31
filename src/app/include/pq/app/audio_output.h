#pragma once

// The DAW's audio output (T3.17, T3.18): one device of the chosen backend (our ASIO host, our WASAPI backend shared
// or exclusive), a BlockAdapter at the engine's block, the engine's callback behind a safety clamp to [−1, 1], and
// the preferences' test tone. When no device can be opened (or the device is lost), a NullDevice keeps the engine
// running in real time (compiles are taken, the playhead moves, silently).
//
// One control thread owns the device (COM objects stay on the thread that made them):
//   1. opens and starts it in the background (startup, and every reconfigure()), so the UI never waits for a driver;
//   2. idle: once the engine has been asleep (engine.h's idle gate) for kSuspendAfter, it stops the stream and
//      prepares the restart (WASAPI's client reset, which can take ~350 ms, happens now rather than on play); a
//      stopped stream costs nothing: no device callbacks, and the control thread blocks without a timeout;
//   3. any engine wake (transport, param write, audition, publish: Engine::setWakeHook) restarts it at once;
//   4. device changes (T3.18): reconfigure() = stop → close → open the new configuration → start (the Program, the
//      engine and its state are untouched: only the device changes); a device that stops on its own while running
//      (WASAPI device removed / invalidated, an ASIO driver whose reset failed) is reopened once, else the output
//      falls back to the NullDevice until the hardware changes (DeviceChangeNotifier: an endpoint added, removed or
//      re-enabled), when the configured device is tried again. No polling for hot-plug: the notifier is an event;
//   5. ASIO driver requests (reset, buffer-size or sample-rate change) are handled by AsioDevice on its own
//      control thread; its stream-changed callback (onStreamChanged) re-prepares the BlockAdapter for a new buffer
//      size, and a new rate that differs from the engine's makes the output play silence (the engine is not run at
//      the wrong rate) and reports `rateMismatch` + the device's rate: the owner (AudioDeviceManager) then rebuilds
//      the engine at that rate (ProjectLink::setSampleRate) and reopens.
// So a stopped DAW uses no CPU for audio, play after idle starts within one stream start (a few ms), and nothing
// here runs on the audio thread except onDevice / onEngine (RT-safe: no allocation, no locks, no syscalls).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "pq/core/result.h"
#include "pq/engine/audio_device.h"
#include "pq/engine/block_adapter.h"
#include "pq/engine/callback_stats.h"
#include "pq/engine/engine.h"

namespace pq::app {

// What to open. The rate must be the engine's (AudioDeviceManager keeps them equal).
struct OutputConfig {
  engine::Backend backend = engine::Backend::Wasapi;
  engine::ShareMode shareMode = engine::ShareMode::Shared;
  std::string device;                         // WASAPI endpoint id / ASIO driver name; empty: deviceIndex
  uint32_t deviceIndex = engine::kDefaultDevice;  // the backend's numeric id (--device <n>), or its default
  uint32_t sampleRate = 48000;
  uint32_t bufferFrames = 256;
  uint32_t inputChannels = 0;  // opened (ASIO duplex) but not read yet: recording is M6
  uint32_t outputChannels = 2;  // the master plays on outputs 1/2, the rest get silence
  bool operator==(const OutputConfig&) const = default;
};

struct OutputStatus {
  enum class State : uint8_t { Opening, Running, Suspended, Fallback, Closed };
  State state = State::Opening;
  std::string deviceName;
  std::string message;      // why there is no device / what happened last (a reset, a lost device)
  engine::StreamInfo stream;  // the open stream (Fallback: the null device's)
  uint32_t adapterLatencyFrames = 0;  // the BlockAdapter's FIFO (0 in direct mode)
  bool rateMismatch = false;          // the device now runs at stream.sampleRate ≠ the engine's rate (silence)
  uint64_t revision = 0;              // bumps on every change
  uint64_t opens = 0, deviceLosses = 0, streamChanges = 0, suspensions = 0;
  uint64_t hardwareChanges = 0;  // DeviceChangeNotifier events (device lists are stale after one)
  // ASIO: the open driver as AsioDevice::enumerate() reports it (rates, buffer-size rules, channels); the window's
  // choosers come from it. Empty for the other backends (their lists come from enumerate() without a stream).
  std::optional<engine::DeviceInfo> asioInfo;
};

// Where devices come from (tests substitute fakes). Called on the control thread.
struct DeviceFactory {
  std::function<Result<std::unique_ptr<engine::AudioDevice>>(engine::Backend)> create;  // null: createAudioDevice
  bool watchHardware = true;  // register a DeviceChangeNotifier (tests drive hardwareChanged() themselves)
  bool allowEnvDisable = true;  // PQ_NO_AUDIO_DEVICE=1 opens the NullDevice (CI, headless runs)
};

class AudioOutput {
public:
  static constexpr double kSuspendAfterSeconds = 1.0;
  static constexpr float kMaxToneDb = -12.0f;

  // Starts the control thread, which opens `config` in the background. `engine` must outlive the output (or be
  // replaced with rebind()).
  AudioOutput(engine::Engine& engine, OutputConfig config, DeviceFactory factory = {});
  ~AudioOutput();  // stops and closes the device (the engine is no longer called afterwards)
  AudioOutput(const AudioOutput&) = delete;
  AudioOutput& operator=(const AudioOutput&) = delete;

  // Any thread: called (on the control thread, or the notifier's / ASIO's threads) after every status change. Must
  // be thread-safe and quick (the app posts an SDL event to the UI thread). Null removes it.
  void setNotify(void (*fn)(void*), void* user);
  // Asynchronous: stop → close → open `config` → start. config.sampleRate must equal the engine's.
  void reconfigure(const OutputConfig& config);
  // Blocking: stops and closes the device; afterwards the engine is not called until the next reconfigure() (a
  // rate change rebuilds the engine in between).
  void close();
  // After close(): the engine to drive from now on. The old one is not touched again (it may already be destroyed).
  void rebind(engine::Engine& engine);
  // The hardware changed (a device arrived or left): retry the configured device if it is not open. Thread-safe.
  void hardwareChanged();

  OutputStatus status() const;
  OutputConfig config() const;
  // The open device's callback statistics (zeros while closed). Any thread.
  engine::CallbackStats::Snapshot callbackStats() const;
  // The test tone: a sine at hz on outputs 1/2 at levelDb (clamped to ≤ kMaxToneDb). While on, the stream is not
  // suspended. Any thread.
  void setTestTone(bool on, float levelDb, float hz);
  bool testTone() const noexcept { return toneOn_.load(std::memory_order_relaxed); }
  // T4.4: an extra source added into every device block after the engine and the test tone (the circuit editor's
  // patch preview, pq/app/patch_preview.h); while one is set the stream is not suspended. Returns once no device
  // callback uses the previous source any more (so it may be destroyed or rebuilt afterwards). `cb` must stay valid
  // until it is replaced. Any non-audio thread.
  void setExtraSource(const engine::AudioCallback* cb);
  // T5.10: the cue source (the browser's preview): added at the device's buffer, after everything else, into output
  // pair `pair` (0 = outputs 1/2; k = outputs 2k+1/2k+2, falling back to 1/2 when the stream has fewer outputs),
  // clamped there. While one is set the stream is not suspended. Same handshake as setExtraSource. Any non-audio thread.
  void setCueSource(const engine::AudioCallback* cb);
  void setCuePair(uint32_t pair) noexcept { cuePair_.store(pair, std::memory_order_relaxed); }
  uint32_t cuePair() const noexcept { return cuePair_.load(std::memory_order_relaxed); }
  // ASIO: the driver's control panel. Errors when no ASIO stream is open.
  Status controlPanel();

  bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }  // the first open finished
  bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  uint64_t suspensions() const noexcept { return suspensions_.load(std::memory_order_relaxed); }

private:
  static void onDevice(void* self, const engine::AudioBlock& b) noexcept;  // RT-safe
  static void onEngine(void* self, const engine::AudioBlock& b) noexcept;  // RT-safe
  static void onWake(void* self);
  static void onHardware(void* self);
  static void onStreamChanged(void* self, const engine::StreamInfo& info);
  void controlMain();
  bool openDevice(const OutputConfig& c);  // control thread, lock not held
  void openFallback(const OutputConfig& c, std::string why);
  void closeDevice();                      // control thread, lock not held
  bool startStream(bool notifyUi = true);  // control thread, lock not held
  void stopStream(bool prepareRestart);
  void publish(OutputStatus::State s, std::string message, bool notifyUi = true);
  void notify();
  void addTone(const engine::AudioBlock& b) noexcept;  // RT-safe
  float toneHzNow() const;

  std::atomic<engine::Engine*> engine_;
  DeviceFactory factory_;
  // Device (control thread; adapter_ also from AsioDevice's control thread in onStreamChanged, while the driver's
  // audio thread is stopped).
  std::unique_ptr<engine::AudioDevice> device_;
  engine::BlockAdapter adapter_;
  uint32_t engineOuts_ = 2;  // channels the adapter / engine produce (≤ 2); the device's others are zeroed
  bool fallback_ = false;
  std::atomic<bool> silence_{false};  // rate mismatch: the engine is not run
  std::atomic<bool> streamRunning_{false};

  std::thread control_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool quit_ = false, wakeRequest_ = false, hwChanged_ = false, closeRequest_ = false, parked_ = false;
  bool panelRequest_ = false;
  float toneHz_ = 440.0f;
  std::optional<OutputConfig> pending_;
  OutputConfig config_;
  OutputStatus status_;
  void (*notifyFn_)(void*) = nullptr;
  void* notifyUser_ = nullptr;
  std::atomic<bool> ready_{false}, running_{false};
  std::atomic<uint64_t> suspensions_{0};

  // Test tone: written by any thread, read by the audio thread (relaxed: a tone change may take one block).
  std::atomic<bool> toneOn_{false};
  std::atomic<const engine::AudioCallback*> extra_{nullptr};
  std::atomic<uint32_t> inExtra_{0};  // the audio thread is inside the extra source (setExtraSource's handshake)
  std::atomic<const engine::AudioCallback*> cue_{nullptr};  // T5.10
  std::atomic<uint32_t> inCue_{0};
  std::atomic<uint32_t> cuePair_{0};
  std::atomic<float> toneGain_{0.0f};
  std::atomic<uint32_t> toneInc_{0};
  uint32_t tonePhase_ = 0;  // audio thread
};

// The sine table the tone reads (2048 points + 1 guard, built once). Exposed for the RT test.
const float* toneSineTable() noexcept;

}  // namespace pq::app
