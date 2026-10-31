#pragma once

// Audio preferences (T3.18, QUESTIONS Q12: "it must work like every DAW, with the ability to choose audio drivers").
//
// AudioPrefsPort is the plain-data interface between the preferences window (views) and the app's audio device
// manager (app: pq/app/audio_manager.h), like TransportPort for the transport: views never include engine. The app
// implements it over the engine's devices (our ASIO host, our WASAPI backend); tests and the view scenes use
// FakeAudioPrefsPort (below), which has a scripted device list.
//
// PreferencesWindow is the window itself, laid out like Live 12's Preferences → Audio tab:
//
//   ┌ Preferences ─────────────────────────────────────────────────────────── × ┐
//   │ Audio  │ Audio Device                                                     │
//   │        │   Driver Type               [ASIO                          ▾]     │
//   │        │   Audio Device              [Focusrite USB ASIO            ▾]     │  (ASIO: one driver, duplex)
//   │        │   Hardware Setup            [Hardware Setup]                     │  (ASIO: the driver's panel)
//   │        │   Audio Input Device        [Microphone (USB)              ▾]     │  (WASAPI: separate devices)
//   │        │   Audio Output Device       [Speakers (Focusrite USB)      ▾]     │
//   │        │   Input Channels            [Off ▾]   Output Channels [2 ▾]      │
//   │        │ Sample Rate                                                      │
//   │        │   In/Out Sample Rate        [48000 ▾]                            │
//   │        │ Latency                                                          │
//   │        │   Buffer Size               [64 Samples ▾]                       │
//   │        │   Input Latency  1.33 ms · Output Latency  3.94 ms · Overall 5.27 ms │
//   │        │ Test                                                             │
//   │        │   Test Tone  [Off]   Tone Volume [-36 dB]   Tone Frequency [440 Hz] │
//   │        │   CPU Usage   2.1 %                                              │
//   │        │ status: Running · Focusrite USB ASIO · 48000 Hz · 64 samples      │
//   └────────┴──────────────────────────────────────────────────────────────────┘
//
// T5.x-library-prefs: with a ViewContext (the app, the scenes) a second tab, Library (Live's Library tab):
//
//   │ Audio   │ User Library                                                     │
//   │ Library │   Location        D:\Music\pqdaw User Library        [Browse…]   │
//   │         │ Places                                                           │
//   │         │   ┌ C:\Samples\Drums ───────────────────────────────┐            │
//   │         │   │ D:\Splice                                       │            │
//   │         │   └─────────────────────────────────────────────────┘            │
//   │         │   [Add Folder…] [Remove]                                         │
//
// Browse… and Add Folder… open the folder picker (FileDialogs::chooseFolder). The location applies at once
// (Library::changeUserLibrary: the browser's User Library place re-points and re-indexes; saves go there) and the app
// keeps it in the settings file; the Places list is the browser's added folders (Library::addPlace / removePlace).
//
// Every choice applies at once (as in Live): the port reopens the stream (stop → reopen → resume with the Program
// kept) and persists the settings. The window is a popover in the shell's overlay layer: closed, it does not exist
// (no widgets, no timers, no polling). Open, it polls the port's status and CPU load at 2 Hz on a timer, damaging
// only the readouts that changed; the CPU meter never wakes a stopped stream (the port reports the load of what is
// running, 0 when nothing is).
//
// UI thread only (the port's methods are UI-thread calls; the app marshals device work to its own thread).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"
#include "pq/views/view_context.h"

namespace pq::views {

class Shell;

enum class AudioDriverType : uint8_t { Asio, WasapiShared, WasapiExclusive };
// "ASIO", "WASAPI (shared)", "WASAPI (exclusive)".
const char* audioDriverTypeName(AudioDriverType t) noexcept;
// The settings key: "asio", "wasapi-shared", "wasapi-exclusive"; parse returns false for anything else.
const char* audioDriverTypeKey(AudioDriverType t) noexcept;
bool parseAudioDriverType(std::string_view key, AudioDriverType& out) noexcept;

// One device in a list. `id` selects it in AudioSettings: a WASAPI endpoint id ("{0.0.0.00000000}.{…}"), an ASIO
// driver's registry name; "" = the system default output (WASAPI) / no input.
struct AudioDeviceEntry {
  std::string id;
  std::string name;
  uint32_t inputChannels = 0, outputChannels = 0;
  bool isDefault = false;
  bool operator==(const AudioDeviceEntry&) const = default;
};

// What a device can do, for the choosers (rates, buffer sizes, channel counts). Empty lists = unknown (the window
// then offers the common values).
struct AudioDeviceCaps {
  std::vector<uint32_t> sampleRates;
  std::vector<uint32_t> bufferSizes;
  uint32_t maxInputChannels = 0, maxOutputChannels = 2;
  bool hasControlPanel = false;  // ASIO: "Hardware Setup"
  bool inputSupported = true;    // false: the driver type cannot open inputs yet (WASAPI input arrives with M6)
  bool operator==(const AudioDeviceCaps&) const = default;
};

// The user's audio settings (persisted by the app in its user settings file).
struct AudioSettings {
  AudioDriverType driver = AudioDriverType::WasapiShared;
  std::string outputDevice;  // id (see AudioDeviceEntry); ASIO: the driver (input and output)
  std::string inputDevice;   // WASAPI only: a capture endpoint id, "" = none
  std::string outputName, inputName;  // display names of the two (shown while the device is not connected)
  uint32_t sampleRate = 48000;
  uint32_t bufferFrames = 256;
  uint32_t inputChannels = 0;   // 0 = no input
  uint32_t outputChannels = 2;  // the master plays on outputs 1/2; further outputs get silence
  // T5.10: where the browser's preview plays ("Cue Out"): 0 = with the master on outputs 1/2; k >= 1 = outputs 2k+1 /
  // 2k+2 (when the stream opens that many outputs; else 1/2).
  uint32_t cueOutput = 0;
  bool operator==(const AudioSettings&) const = default;
};

// What is running now.
struct AudioStatus {
  enum class State : uint8_t {
    Opening,    // the device is being opened (asynchronously)
    Running,    // the stream runs
    Suspended,  // open, stream stopped while the engine is idle (it restarts on the next play / edit)
    NoDevice,   // no device could be opened (or the device was lost): the engine runs silently on a null device
  };
  State state = State::Opening;
  std::string deviceName;  // the device actually open
  std::string message;     // why there is no device, what changed (driver reset, rate followed), empty otherwise
  uint32_t sampleRate = 0, bufferFrames = 0, inputChannels = 0, outputChannels = 0;
  double inputLatencyMs = 0, outputLatencyMs = 0;  // as the driver reports them (+ our block FIFO, when used)
  uint64_t revision = 0;  // bumps on every change of the fields above
  bool operator==(const AudioStatus&) const = default;
};

class AudioPrefsPort {
public:
  virtual ~AudioPrefsPort() = default;
  virtual std::vector<AudioDriverType> driverTypes() = 0;
  // Device lists of a driver type. The app caches them and refreshes on a hardware change (hot-plug).
  virtual std::vector<AudioDeviceEntry> outputDevices(AudioDriverType t) = 0;
  virtual std::vector<AudioDeviceEntry> inputDevices(AudioDriverType t) = 0;
  virtual AudioDeviceCaps capabilities(const AudioSettings& s) = 0;
  virtual AudioSettings settings() = 0;
  // Applies and persists `s`: reopens the stream (asynchronously; status() follows).
  virtual void apply(const AudioSettings& s) = 0;
  virtual AudioStatus status() = 0;
  // Audio callback time / buffer period over the interval since the previous call, in percent; 0 while no stream
  // runs. The window calls it at 2 Hz while open.
  virtual double cpuLoad() = 0;
  // T5.4: streamed sample reads that came late (played silence) since the start: the control bar's disk indicator.
  virtual uint64_t diskUnderruns() { return 0; }
  // Test tone (Live's Test Tone / Tone Volume / Tone Frequency): a sine added to outputs 1/2 while on.
  virtual void setTestTone(bool on, float levelDb, float hz) = 0;
  virtual bool testTone() = 0;
  // ASIO "Hardware Setup": the driver's control panel.
  virtual bool openControlPanel() = 0;
  // Called on the UI thread whenever status or device lists changed (null removes it).
  virtual void setListener(std::function<void()> fn) = 0;
};

// A scripted port: device lists, capabilities and a status set by the test or scene; apply() records the settings
// and reports them as running at once. Header-only behaviour, no devices.
class FakeAudioPrefsPort final : public AudioPrefsPort {
public:
  struct Driver {
    AudioDriverType type;
    std::vector<AudioDeviceEntry> outputs, inputs;
    AudioDeviceCaps caps;
  };
  std::vector<Driver> drivers;
  AudioSettings current;
  AudioStatus running;
  double load = 0.0;
  bool tone = false;
  float toneDb = -36.0f, toneHz = 440.0f;
  std::vector<AudioSettings> applied;  // every apply()
  int panels = 0;
  std::function<void()> listener;

  std::vector<AudioDriverType> driverTypes() override {
    std::vector<AudioDriverType> v;
    for (const Driver& d : drivers) v.push_back(d.type);
    return v;
  }
  std::vector<AudioDeviceEntry> outputDevices(AudioDriverType t) override {
    const Driver* d = find(t);
    return d ? d->outputs : std::vector<AudioDeviceEntry>{};
  }
  std::vector<AudioDeviceEntry> inputDevices(AudioDriverType t) override {
    const Driver* d = find(t);
    return d ? d->inputs : std::vector<AudioDeviceEntry>{};
  }
  AudioDeviceCaps capabilities(const AudioSettings& s) override {
    const Driver* d = find(s.driver);
    return d ? d->caps : AudioDeviceCaps{};
  }
  AudioSettings settings() override { return current; }
  void apply(const AudioSettings& s) override {
    applied.push_back(s);
    current = s;
    running.state = AudioStatus::State::Running;
    running.sampleRate = s.sampleRate;
    running.bufferFrames = s.bufferFrames;
    running.inputChannels = s.inputChannels;
    running.outputChannels = s.outputChannels;
    ++running.revision;
    if (listener) listener();
  }
  AudioStatus status() override { return running; }
  double cpuLoad() override { return load; }
  void setTestTone(bool on, float db, float hz) override {
    tone = on;
    toneDb = db;
    toneHz = hz;
  }
  bool testTone() override { return tone; }
  bool openControlPanel() override {
    ++panels;
    return true;
  }
  void setListener(std::function<void()> fn) override { listener = std::move(fn); }

  const Driver* find(AudioDriverType t) const {
    for (const Driver& d : drivers)
      if (d.type == t) return &d;
    return nullptr;
  }
  // A Focusrite-like machine: an ASIO driver plus WASAPI endpoints (the preferences golden and tests).
  static FakeAudioPrefsPort studio();
};

// The window (see the file comment). Owned by the overlay layer while open.
class PreferencesWindow : public ui::Popover {
public:
  static constexpr float kWidth = 640.0f, kHeight = 460.0f;  // logical px
  // `ctx` null: the Audio tab only (no Library tab).
  explicit PreferencesWindow(AudioPrefsPort& port, ViewContext* ctx = nullptr);
  ~PreferencesWindow() override;
  // Starts the 2 Hz readout timer (openPreferences calls it once the window is in a tree).
  void startPolling();

  // The settings shown (what apply() received last, or the port's at open).
  const AudioSettings& shown() const noexcept { return shown_; }
  // Re-reads lists, capabilities and status from the port (the listener does this; tests call it directly).
  void refresh();
  // The current readouts (tests).
  const std::string& statusText() const;
  const std::string& latencyText() const;
  const std::string& cpuText() const;

  bool onKey(const ui::KeyEvent& e) override;
  void onTimer(uint32_t id) override;
  ui::InfoText info() const override;

  // The controls (tests drive them like a user would).
  ui::Dropdown* driverChooser() const noexcept { return driver_; }
  ui::Dropdown* outputChooser() const noexcept { return output_; }
  ui::Dropdown* inputChooser() const noexcept { return input_; }
  ui::Dropdown* rateChooser() const noexcept { return rate_; }
  ui::Dropdown* bufferChooser() const noexcept { return buffer_; }
  ui::Dropdown* inputChannelsChooser() const noexcept { return inChannels_; }
  ui::Dropdown* outputChannelsChooser() const noexcept { return outChannels_; }
  ui::Dropdown* cueChooser() const noexcept { return cue_; }  // T5.10
  ui::Toggle* toneToggle() const noexcept { return tone_; }
  ui::DragBox* toneVolume() const noexcept { return toneDb_; }
  ui::DragBox* toneFrequency() const noexcept { return toneHz_; }
  ui::Button* hardwareSetup() const noexcept { return panel_; }

  // T5.x-library-prefs: the tabs (0 Audio, 1 Library) and the Library tab's controls (null without a context).
  enum class Tab : uint8_t { Audio, Library };
  void showTab(Tab t);
  Tab tab() const noexcept { return tab_; }
  ui::Button* browseUserLibrary() const noexcept { return browse_; }
  ui::Button* addFolder() const noexcept { return addFolder_; }
  ui::Button* removeFolder() const noexcept { return removeFolder_; }
  const std::string& userLibraryText() const;
  // The selected row of the Places list (-1 none).
  int32_t selectedFolder() const noexcept;
  void selectFolder(int32_t row);

  static constexpr uint32_t kPollTimer = 1;
  static constexpr double kPollSeconds = 0.5;

private:
  void rebuildChoosers();
  void updateReadouts(bool force);
  void choose(AudioSettings s);
  void sendTone();
  void buildLibraryTab(ui::Widget& body);
  void updateLibrary();

  AudioPrefsPort& port_;
  AudioSettings shown_;
  AudioDeviceCaps caps_;
  AudioStatus status_;
  std::vector<AudioDriverType> drivers_;
  std::vector<AudioDeviceEntry> outputs_, inputs_;
  std::vector<uint32_t> rates_, buffers_, inCounts_, outCounts_;
  ui::Dropdown *driver_ = nullptr, *output_ = nullptr, *input_ = nullptr, *rate_ = nullptr, *buffer_ = nullptr,
               *inChannels_ = nullptr, *outChannels_ = nullptr, *cue_ = nullptr;
  ui::Widget *outputRow_ = nullptr, *inputRow_ = nullptr, *panelRow_ = nullptr;
  ui::Label *outputLabel_ = nullptr, *statusLabel_ = nullptr, *latency_ = nullptr, *cpu_ = nullptr;
  ui::Button* panel_ = nullptr;
  ui::Toggle* tone_ = nullptr;
  ui::DragBox *toneDb_ = nullptr, *toneHz_ = nullptr;
  ViewContext* ctx_ = nullptr;
  Tab tab_ = Tab::Audio;
  ui::Widget *tabs_ = nullptr, *audioPage_ = nullptr, *libraryPage_ = nullptr;
  ui::Label *audioTab_ = nullptr, *libraryTab_ = nullptr, *userLibrary_ = nullptr;
  ui::Button *browse_ = nullptr, *addFolder_ = nullptr, *removeFolder_ = nullptr;
  ui::Widget* folderList_ = nullptr;
  ViewContext::Subscription librarySub_;
  uint32_t timer_ = 0;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// Opens the window centred in `from`'s overlay layer (one at a time: an open one is returned as it is). nullptr
// without an overlay layer.
PreferencesWindow* openPreferences(ui::Widget& from, AudioPrefsPort& port, ViewContext* ctx = nullptr);
PreferencesWindow* openPreferencesWindow(const ui::Widget& w);  // the open one, if any
// Installs the app.preferences action handler (Ctrl+,): opens the window over ctx.audioPrefs() (disabled while the
// context has no port). One line in installDefaultContent.
void installPreferences(Shell& shell);

}  // namespace pq::views
