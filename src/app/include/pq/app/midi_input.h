#pragma once

// MIDI input ports (T6.4; docs/plans/M6.md "### T6.4"): the app's side of live MIDI input.
//
// MidiSource is the system's MIDI input API (makeWinMmSource(): RtMidi with the WinMM backend; tests: a fake). Each
// open port calls a plain callback on the source's own thread with one complete message (no running status). The
// service's callback does exactly two things: stamp the message with the host clock (engine::qpcNow100ns) and push a
// 16-byte engine::RawMidi into that port's SPSC ring (engine::LivePorts, owned by the ProjectLink and consumed by the
// engine). SysEx, MIDI clock and other system messages are ignored. No lock, no allocation on that path.
//
// MidiInputService (UI thread) keeps the user's port settings (Preferences → MIDI: a port's "Track" switch, its pitch-bend
// range; persisted in the user settings file, "midi" section), enumerates the source's ports, opens the enabled ones
// that are connected (each gets one of the kLivePorts ring indices while open), closes the rest, and tells the link
// (ProjectLink::setLivePorts: the routes name ports). refresh() runs at start, when the device list may have changed
// (a hardware change: AudioDeviceManager::poll) and whenever the preferences ask for the list. A port that goes away
// (unplugged, disabled) gets All Notes Off on every channel pushed after its close: none of its notes stays stuck.
// A new port (not in the settings) is enabled, as Live does for a controller it sees first.
//
// While a port is open the audio output does not suspend in the background (AudioOutput::setKeepRunning): a MIDI note
// played into a backgrounded pqdaw must sound, and waking a suspended stream from the MIDI thread would need a lock.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/engine/live_input.h"
#include "pq/views/prefs/audio_prefs.h"

namespace pq::app {

class ProjectLink;

// One message as a port delivers it: status byte first. Called on the source's thread; must be wait-free.
using MidiCallback = void (*)(void* user, const uint8_t* bytes, size_t size) noexcept;

class MidiInPort {
public:
  virtual ~MidiInPort() = default;  // closes the port: no callback runs after it returns
};

class MidiSource {
public:
  virtual ~MidiSource() = default;
  // The input ports now (names as the system shows them; duplicates get " 2", " 3" appended by the source).
  virtual std::vector<std::string> portNames() = 0;
  virtual Result<std::unique_ptr<MidiInPort>> open(const std::string& name, MidiCallback cb, void* user) = 0;
};

// RtMidi 6.0.0, WinMM backend (the only one built).
std::unique_ptr<MidiSource> makeWinMmSource();

struct MidiPortSetting {
  std::string name;
  bool enabled = true;          // Live's "Track" switch: its notes reach the tracks that listen
  float bendSemitones = 2.0f;   // pitch-bend range
  friend bool operator==(const MidiPortSetting&, const MidiPortSetting&) = default;
};

class MidiInputService {
public:
  // `save` persists the settings after every change (null: not persisted). `link` must outlive the service.
  MidiInputService(ProjectLink& link, std::unique_ptr<MidiSource> source, std::vector<MidiPortSetting> settings,
                   std::function<void(const std::vector<MidiPortSetting>&)> save = nullptr);
  ~MidiInputService();  // closes every port (All Notes Off for each)
  MidiInputService(const MidiInputService&) = delete;
  MidiInputService& operator=(const MidiInputService&) = delete;

  // Re-enumerates, opens / closes ports to match the settings, updates the link. UI thread.
  void refresh();
  // Preferences → MIDI: every port connected or remembered, in the source's order (remembered ones after).
  std::vector<views::MidiPortEntry> ports();
  void setPort(const std::string& name, bool enabled, float bendSemitones);
  // Called with true when the first port opens and false when the last closes (AudioOutput::setKeepRunning).
  void setOpenListener(std::function<void(bool)> fn) { onOpen_ = std::move(fn); }

  const std::vector<MidiPortSetting>& settings() const noexcept { return settings_; }
  std::vector<std::string> openPorts() const;  // by ring index ("" = free)
  const std::string& lastError() const noexcept { return lastError_; }

private:
  struct Open {
    std::string name;
    uint32_t index = 0;            // LivePorts ring
    std::unique_ptr<MidiInPort> port;
    struct Ctx {
      engine::LivePorts* rings;
      uint8_t index;
    };
    std::unique_ptr<Ctx> ctx;      // the callback's user pointer (stable address)
  };
  static void onMessage(void* user, const uint8_t* bytes, size_t size) noexcept;
  MidiPortSetting& settingFor(const std::string& name);
  void close(Open& o);
  void publish();

  ProjectLink& link_;
  std::unique_ptr<MidiSource> source_;
  std::vector<MidiPortSetting> settings_;
  std::function<void(const std::vector<MidiPortSetting>&)> save_;
  std::function<void(bool)> onOpen_;
  std::vector<std::string> present_;  // the source's ports at the last refresh
  std::vector<Open> open_;
  bool anyOpen_ = false;
  std::string lastError_;
};

}  // namespace pq::app
