#pragma once

// The audio device manager (T3.18, QUESTIONS Q12): the app's side of the preferences window
// (views::AudioPrefsPort). It owns the AudioOutput, keeps the user's AudioSettings (loaded from and saved to the
// user settings file, pq/app/user_settings.h), turns them into an OutputConfig and keeps the engine's sample rate
// equal to the device's:
//
//   apply(s), same rate     AudioOutput::reconfigure: stop → close → open → start on the output's control thread;
//                           the engine, its Program and its state are untouched (playback goes on after the gap)
//   apply(s), other rate    AudioOutput::close (blocking) → ProjectLink::setSampleRate (engine + compiler rebuilt
//                           at the new rate, the document compiled again, the transport kept) → rebind → reconfigure
//   a driver that changed its rate on its own (ASIO sampleRateDidChange, a control panel), or refused ours: the
//                           output reports rateMismatch (and plays silence); poll() then follows the device's rate
//                           the same way and saves it
//   hot-plug                the output reopens a lost device or falls back to silence; a hardware change marks the
//                           device lists stale (re-enumerated the next time the window asks)
//
// Threads: everything here runs on the UI thread. The output notifies from its own threads through the hook given
// to setUiNotify (the app posts an SDL event); the UI thread then calls poll().

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pq/app/audio_output.h"
#include "pq/app/cli.h"
#include "pq/app/project_link.h"
#include "pq/engine/asio_device.h"
#include "pq/views/prefs/audio_prefs.h"

namespace pq::app {

struct ManagerHooks {
  DeviceFactory factory;  // devices for the stream (and for WASAPI enumeration, through create(Backend::Wasapi))
  // Device lists per backend (null: a fresh device's enumerate()); ASIO driver names (null: the registry).
  std::function<Result<std::vector<engine::DeviceInfo>>(engine::Backend)> enumerate;
  std::function<std::vector<engine::AsioDriverEntry>()> asioDrivers;
  std::string settingsPath;  // empty: userSettingsPath()
  bool persist = true;       // false: never write the settings file
};

class AudioDeviceManager final : public views::AudioPrefsPort {
public:
  // The settings to start with: the user settings file, then the command line's device flags on top (--asio /
  // --backend, --device, --exclusive, --buffer, --rate) for this run. `warnings` gets the settings file's.
  static views::AudioSettings startupSettings(const CliOptions& o, const std::string& path,
                                              std::vector<std::string>* warnings = nullptr);
  // The output configuration for `s` (rate = s.sampleRate). `deviceIndex`: a numeric --device id, if any.
  static OutputConfig outputConfigFor(const views::AudioSettings& s, uint32_t deviceIndex = engine::kDefaultDevice);

  // Starts the output on `link`'s engine (which must already run at initial.sampleRate). `link` must outlive the
  // manager.
  AudioDeviceManager(ProjectLink& link, const views::AudioSettings& initial, ManagerHooks hooks = {},
                     uint32_t cliDeviceIndex = engine::kDefaultDevice);
  ~AudioDeviceManager() override;

  // How the output's notifications reach the UI thread (any thread calls fn; it must post, not work).
  void setUiNotify(void (*fn)(void*), void* user);
  // UI thread, after a notification (or periodically in tests): follows a device rate change, refreshes stale device
  // lists, tells the window. Returns true when something changed.
  bool poll();

  AudioOutput& output() noexcept { return *output_; }
  ProjectLink& link() noexcept { return link_; }
  uint64_t rateChanges() const noexcept { return rateChanges_; }
  const std::string& lastError() const noexcept { return lastError_; }

  // ---- views::AudioPrefsPort (UI thread) ----
  std::vector<views::AudioDriverType> driverTypes() override;
  std::vector<views::AudioDeviceEntry> outputDevices(views::AudioDriverType t) override;
  std::vector<views::AudioDeviceEntry> inputDevices(views::AudioDriverType t) override;
  views::AudioDeviceCaps capabilities(const views::AudioSettings& s) override;
  views::AudioSettings settings() override { return settings_; }
  void apply(const views::AudioSettings& s) override;
  views::AudioStatus status() override;
  double cpuLoad() override;
  void setTestTone(bool on, float levelDb, float hz) override;
  bool testTone() override { return output_->testTone(); }
  bool openControlPanel() override;
  void setListener(std::function<void()> fn) override { listener_ = std::move(fn); }

private:
  static void onOutput(void* self);
  void open(const views::AudioSettings& s);  // rate change if needed, then reconfigure
  const std::vector<engine::DeviceInfo>& devices(engine::Backend b);
  void save();

  ProjectLink& link_;
  ManagerHooks hooks_;
  views::AudioSettings settings_;
  uint32_t cliDeviceIndex_;
  std::unique_ptr<AudioOutput> output_;
  std::function<void()> listener_;
  void (*uiNotify_)(void*) = nullptr;
  void* uiNotifyUser_ = nullptr;
  std::map<engine::Backend, std::vector<engine::DeviceInfo>> deviceCache_;
  std::optional<std::vector<engine::AsioDriverEntry>> asioCache_;
  uint64_t seenHardware_ = 0, seenRevision_ = 0;
  uint64_t rateChanges_ = 0;
  std::string lastError_;
  // cpuLoad's previous sample
  uint64_t loadOpens_ = ~0ull, loadCallbacks_ = 0;
  double loadBusyUs_ = 0.0;
  std::chrono::steady_clock::time_point loadAt_{};
};

}  // namespace pq::app
