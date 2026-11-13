// T3.18 audio device manager: the user settings file (round trip, other sections kept, lenient fields), the
// startup settings (file + command-line flags), and the device manager over a scripted device list (FakeDevice: a
// NullDevice behind a fake WASAPI endpoint list and fake ASIO driver names) driving the real ProjectLink:
//   - switching driver type / device / rate / buffer while playing reopens the stream, with no allocation on the
//     audio thread (the allocation trap counts every violation in the process), the Program and the transport kept;
//     a rate change rebuilds the engine at the new rate (ProjectLink::setSampleRate);
//   - settings persist across restarts;
//   - hot-plug: a device lost while running → silence on the null device; the hardware-change event reopens it;
//   - ASIO driver requests through the real AsioDevice and a fake driver (tests/engine/fake_asio_driver.h): a reset
//     with a new buffer size re-prepares the block adapter; a driver rate change silences the output and the
//     manager rebuilds the engine at the driver's rate;
//   - the test tone (RT-safe, level-clamped) and the CPU meter (0 while the stream is suspended);
//   - AU.stream: no suspend while the app is in the foreground (only after a background idle); every driver start
//     pre-rolls silence (the engine and its transport wait) until the driver has settled; an ASIO reset while audio
//     sounds fades out, drains, and pre-rolls again; the stream trace records it all.
// Real devices: opt-in with PQ_TEST_AUDIO_DEVICE=1 (short, quiet tones ≤ −30 dBFS), never in the default tier.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../engine/fake_asio_driver.h"
#include "../engine/loopback_probe.h"
#include "playback_fixture.h"
#include "pq/app/audio_manager.h"
#include "pq/app/audio_output.h"
#include "pq/app/cli.h"
#include "pq/app/project_link.h"
#include "pq/app/user_settings.h"
#include "pq/meta/user_defaults.h"
#include "pq/core/alloc_trap.h"
#include "pq/engine/asio_device.h"
#include "pq/engine/null_device.h"
#include "pq/engine/stream_trace.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"

using namespace pq;
using namespace pq::app;
using views::AudioDriverType;
using views::AudioSettings;

namespace {

namespace fs = std::filesystem;
using clock_t_ = std::chrono::steady_clock;

std::string tempDir(const char* name) {
  const fs::path p = fs::temp_directory_path() / (std::string("pq_audio_") + name);
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p.string();
}

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// ---- Fake hardware ------------------------------------------------------------------------------------------------

struct FakeHardware;

// A NullDevice (threaded: it calls back in real time; manual: processBlock() from the test) behind a scripted device
// list. Only endpoint ids / driver names in `present` open.
class FakeDevice final : public engine::AudioDevice {
public:
  FakeDevice(FakeHardware& hw, engine::Backend b) : hw_(hw), backend_(b) {}
  ~FakeDevice() override;
  engine::Backend backend() const noexcept override { return backend_; }
  Result<std::vector<engine::DeviceInfo>> enumerate() override;
  Result<engine::StreamInfo> open(const engine::StreamConfig& c, engine::AudioCallback cb) override;
  Status start() override;
  Status stop() override { return dev_.stop(); }
  void close() noexcept override { dev_.close(); }
  bool isOpen() const noexcept override { return dev_.isOpen(); }
  bool isRunning() const noexcept override { return dev_.isRunning(); }
  const engine::CallbackStats& stats() const noexcept override { return dev_.stats(); }
  engine::NullDevice& null() noexcept { return dev_; }
  // The device goes away: its stream stops on its own (what WASAPI's audio thread does on DEVICE_INVALIDATED).
  void unplug() { (void)dev_.stop(); }

private:
  FakeHardware& hw_;
  engine::Backend backend_;
  engine::NullDevice dev_;
};

struct FakeHardware {
  std::mutex m;
  std::vector<engine::DeviceInfo> wasapi;
  std::vector<std::string> asioDrivers{"Fake ASIO", "Other ASIO"};
  std::set<std::string> present{"", "{a}", "{b}", "Fake ASIO"};
  bool manual = false;  // devices do not start a thread (the test calls processBlock)
  uint32_t asioInputs = 4;  // T6.7b: the fake ASIO driver's inputs (a stream opens at most these)
  int opens = 0;
  std::vector<engine::StreamConfig> configs;
  FakeDevice* current = nullptr;

  FakeHardware() {
    auto dev = [](uint32_t id, const char* ep, const char* name, uint32_t outs, uint32_t ins, bool def) {
      engine::DeviceInfo d;
      d.id = id;
      d.endpointId = ep;
      d.name = name;
      d.outputChannels = outs;
      d.inputChannels = ins;
      d.isDefaultOutput = def && outs > 0;
      d.isDefaultInput = def && ins > 0;
      d.preferredSampleRate = 48000;
      d.sampleRates = {44100, 48000, 96000};
      d.minPeriodFrames = 144;
      d.sharedMinPeriodFrames = 128;
      d.sharedMaxPeriodFrames = 1024;
      return d;
    };
    wasapi = {dev(1, "{a}", "Speakers (Fake USB)", 2, 0, true), dev(2, "{b}", "Headphones (Fake HDA)", 2, 0, false),
              dev(3, "{c}", "Line In (Fake USB)", 0, 2, true)};
  }
  DeviceFactory factory() {
    DeviceFactory f;
    f.create = [this](engine::Backend b) -> Result<std::unique_ptr<engine::AudioDevice>> {
      return std::unique_ptr<engine::AudioDevice>(std::make_unique<FakeDevice>(*this, b));
    };
    f.watchHardware = false;
    f.allowEnvDisable = false;
    return f;
  }
  ManagerHooks hooks(const std::string& settingsPath) {
    ManagerHooks h;
    h.factory = factory();
    h.asioDrivers = [this] {
      std::lock_guard lock(m);
      std::vector<engine::AsioDriverEntry> v;
      for (const std::string& n : asioDrivers) v.push_back({n, "{00000000-0000-0000-0000-000000000000}"});
      return v;
    };
    h.settingsPath = settingsPath;
    return h;
  }
  int openCount() {
    std::lock_guard lock(m);
    return opens;
  }
  engine::StreamConfig lastConfig() {
    std::lock_guard lock(m);
    return configs.empty() ? engine::StreamConfig{} : configs.back();
  }
};

FakeDevice::~FakeDevice() {
  std::lock_guard lock(hw_.m);
  if (hw_.current == this) hw_.current = nullptr;
}

Result<std::vector<engine::DeviceInfo>> FakeDevice::enumerate() {
  std::lock_guard lock(hw_.m);
  return hw_.wasapi;
}

Result<engine::StreamInfo> FakeDevice::open(const engine::StreamConfig& cIn, engine::AudioCallback cb) {
  std::string name;
  engine::StreamConfig c = cIn;
  uint32_t deviceIns = 0;  // T6.7b: inputs are bounded by the device, like the real backends
  {
    std::lock_guard lock(hw_.m);
    const std::string id = c.outputEndpointId;
    if (!hw_.present.count(id)) return Error{ErrorCode::Device, "fake device '" + id + "' is not connected", {}};
    name = id.empty() ? std::string("Speakers (Fake USB)") : id;
    for (const engine::DeviceInfo& d : hw_.wasapi)
      if (d.endpointId == id) name = d.name;
    if (backend_ == engine::Backend::Asio) {
      deviceIns = hw_.asioInputs;
    } else if (c.inputChannels > 0) {
      for (const engine::DeviceInfo& d : hw_.wasapi)
        if (d.inputChannels > 0 && (c.inputEndpointId.empty() ? d.isDefaultInput : d.endpointId == c.inputEndpointId))
          deviceIns = d.inputChannels;
    }
    c.inputChannels = std::min(c.inputChannels, deviceIns);
    ++hw_.opens;
    hw_.configs.push_back(cIn);
    hw_.current = this;
  }
  auto r = dev_.open(c, cb);
  if (!r) return r;
  engine::StreamInfo si = *r;
  si.backend = backend_;
  si.outputDeviceName = name;
  si.latencyFrames = c.bufferFrames;
  si.inputLatencyFrames = c.inputChannels > 0 ? c.bufferFrames : 0;
  si.inputDeviceChannels = deviceIns;
  return si;
}

Status FakeDevice::start() {
  bool manual;
  {
    std::lock_guard lock(hw_.m);
    manual = hw_.manual;
  }
  return manual ? Status(Ok{}) : dev_.start();
}

// ---- Helpers ------------------------------------------------------------------------------------------------------

std::unique_ptr<ProjectLink> makeLink(double rate = 48000.0, float sleepMs = 0.0f) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = sleepMs;
  c.sampleRate = rate;
  auto l = ProjectLink::create(c, playback_fixture::Fixture().snap());
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

// Waits (polling the manager like the UI thread does) until pred holds, at most `seconds`.
template <class Pred>
bool waitUntil(AudioDeviceManager& m, Pred pred, double seconds = 10.0) {
  const auto end = clock_t_::now() + std::chrono::duration<double>(seconds);
  while (clock_t_::now() < end) {
    m.poll();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  m.poll();
  return pred();
}

bool runningWith(AudioDeviceManager& m, const AudioSettings& s) {
  const views::AudioStatus st = m.status();
  return st.state == views::AudioStatus::State::Running && st.sampleRate == s.sampleRate && st.bufferFrames == s.bufferFrames;
}

// AU.stream: a cold start plays silence until the driver has settled; pumps a manual device through it (each tick
// is one device callback). Returns the callbacks it took.
template <class Tick>
int throughPreroll(AudioOutput& out, Tick tick, int maxCallbacks = 4000) {
  int n = 0;
  while (out.prerolling() && n < maxCallbacks) {
    tick();
    ++n;
  }
  CHECK_FALSE(out.prerolling());
  return n;
}

// The playhead moves (the engine runs on the device's thread) by at least `ticks` within 3 s.
bool advances(ProjectLink& link, Ticks ticks = kPpq / 8) {
  const Ticks t0 = link.playhead().tick;
  const auto end = clock_t_::now() + std::chrono::seconds(3);
  while (clock_t_::now() < end) {
    if (link.playhead().tick >= t0 + ticks) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

}  // namespace

// ---- Settings file ------------------------------------------------------------------------------------------------

TEST_CASE("settings: round trip; other sections are kept; invalid fields fall back with warnings; no file = NotFound") {
  const fs::path dir = tempDir("settings");
  const std::string path = (dir / "settings.json").string();
  CHECK(loadAudioSettings(path).error().code == ErrorCode::NotFound);
  AudioSettings s;
  s.driver = AudioDriverType::Asio;
  s.outputDevice = s.outputName = "Focusrite USB ASIO";
  s.sampleRate = 96000;
  s.bufferFrames = 64;
  s.inputDevice = "{c}";
  s.outputChannels = 4;
  { std::ofstream(path) << R"({"schemaVersion": 1, "look": {"zoom": 125}, "audio": {"driver": "wasapi-shared"}})"; }
  REQUIRE(saveAudioSettings(path, s).ok());
  auto back = loadAudioSettings(path);
  REQUIRE(back.ok());
  CHECK(back->settings == s);
  CHECK(back->warnings.empty());
  CHECK(readFile(path).find("\"zoom\": 125") != std::string::npos);  // a later section survives
  CHECK_FALSE(fs::exists(path + ".tmp"));
  // Lenient: a bad field keeps its default, the others load.
  { std::ofstream(path) << R"({"audio": {"driver": "coreaudio", "sampleRate": 12, "bufferFrames": 128, "outputDevice": 7}})"; }
  back = loadAudioSettings(path);
  REQUIRE(back.ok());
  CHECK(back->settings.driver == AudioSettings{}.driver);
  CHECK(back->settings.sampleRate == 48000);
  CHECK(back->settings.bufferFrames == 128);
  CHECK(back->warnings.size() == 3);
  { std::ofstream(path) << "not json"; }
  CHECK(loadAudioSettings(path).error().code == ErrorCode::Parse);
}

TEST_CASE("settings: the metaDefaults section (VZ.1) round-trips; unknown types and refused data are skipped") {
  const fs::path dir = tempDir("metadefaults");
  const std::string path = (dir / "settings.json").string();
  loadMetaDefaults(path);  // no file: nothing
  CHECK(meta::userDefaultData("spectrogram") == nullptr);
  AudioSettings s;
  REQUIRE(saveAudioSettings(path, s).ok());
  const meta::DataValue rec = meta::DataValue::record({{"map", meta::DataValue::num(2.0)}, {"gamma", meta::DataValue::num(1.2)}});
  REQUIRE(saveMetaDefault(path, "spectrogram", rec).ok());
  REQUIRE(saveMetaDefault(path, "oscilloscope", meta::DataValue::record({{"lw", meta::DataValue::num(2.5)}})).ok());
  CHECK(loadAudioSettings(path).ok());  // the other sections stay
  {  // a type that does not exist and a field that is not a number
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    const auto at = text.find("\"metaDefaults\": {");
    REQUIRE(at != std::string::npos);
    text.insert(at + 17, "\"nosuchtype\": {\"x\": 1}, \"spectrum\": {\"floor\": -90, \"name\": \"x\"}, ");
    std::ofstream(path) << text;
  }
  loadMetaDefaults(path);
  const meta::DataPtr g = meta::userDefaultData("spectrogram");
  REQUIRE(g != nullptr);
  CHECK(*g == rec);
  const meta::DataPtr o = meta::userDefaultData("oscilloscope");
  REQUIRE(o != nullptr);
  CHECK(o->field("lw")->number == 2.5);
  const meta::DataPtr sp = meta::userDefaultData("spectrum");  // the string field is dropped, the number kept
  REQUIRE(sp != nullptr);
  CHECK(sp->fields.size() == 1);
  CHECK(meta::userDefaultData("nosuchtype") == nullptr);
  for (const char* t : {"spectrogram", "oscilloscope", "spectrum"}) meta::setUserDefaultData(t, nullptr);
}

TEST_CASE("settings: the view prefs section (T4.15) round-trips next to the audio section; defaults when absent") {
  const fs::path dir = tempDir("viewprefs");
  const std::string path = (dir / "settings.json").string();
  CHECK_FALSE(loadViewPrefs(path).circuitShowInactive);  // no file
  AudioSettings s;
  s.sampleRate = 96000;
  REQUIRE(saveAudioSettings(path, s).ok());
  CHECK(loadViewPrefs(path) == views::ViewPrefs{});  // no section
  views::ViewPrefs v;
  v.circuitShowInactive = true;
  v.freeRackOpen = true;  // TX.free-rack: the Device view's free-device rack, expanded and its height
  v.freeRackHeight = 260.0f;
  v.recordOffsetMs = -3.25f;  // T6.7b: Preferences → Record
  v.midiExportMode = doc::MidiExportMode::Degrees;  // MF.3: the clip menu's last Export MIDI mode
  REQUIRE(saveViewPrefs(path, v).ok());
  CHECK(loadViewPrefs(path) == v);
  REQUIRE(loadAudioSettings(path).ok());
  CHECK(loadAudioSettings(path)->settings.sampleRate == 96000);  // the audio section survives
  REQUIRE(saveAudioSettings(path, s).ok());
  CHECK(loadViewPrefs(path) == v);  // and the views section the audio save
  { std::ofstream(path) << R"({"views": {"circuitShowInactive": "yes"}})"; }
  CHECK_FALSE(loadViewPrefs(path).circuitShowInactive);  // the wrong type keeps the default
  { std::ofstream(path) << R"({"views": {"recordOffsetMs": 1000}})"; }
  CHECK(loadViewPrefs(path).recordOffsetMs == views::ViewPrefs::kMaxRecordOffsetMs);  // clamped
  { std::ofstream(path) << "not json"; }
  CHECK_FALSE(loadViewPrefs(path).circuitShowInactive);
}

TEST_CASE("startup settings: the settings file, then this run's command-line device flags") {
  const fs::path dir = tempDir("startup");
  const std::string path = (dir / "settings.json").string();
  AudioSettings saved;
  saved.driver = AudioDriverType::Asio;
  saved.outputDevice = "Focusrite USB ASIO";
  saved.sampleRate = 44100;
  saved.bufferFrames = 96;
  REQUIRE(saveAudioSettings(path, saved).ok());
  auto parse = [](std::vector<std::string_view> a) {
    auto o = parseCli(a);
    REQUIRE(o.ok());
    return *o;
  };
  CHECK(AudioDeviceManager::startupSettings(parse({}), path) == saved);
  AudioSettings s = AudioDeviceManager::startupSettings(parse({"--buffer", "128", "--rate", "48000"}), path);
  CHECK(s.driver == AudioDriverType::Asio);
  CHECK(s.bufferFrames == 128);
  CHECK(s.sampleRate == 48000);
  s = AudioDeviceManager::startupSettings(parse({"--exclusive"}), path);
  CHECK(s.driver == AudioDriverType::WasapiExclusive);
  CHECK(s.outputDevice.empty());  // the ASIO driver name is no WASAPI endpoint
  s = AudioDeviceManager::startupSettings(parse({"--device", "{x}"}), path);
  CHECK(s.driver == AudioDriverType::WasapiShared);
  CHECK(s.outputDevice == "{x}");
  s = AudioDeviceManager::startupSettings(parse({"--asio", "--device", "Other ASIO"}), path);
  CHECK(s.driver == AudioDriverType::Asio);
  CHECK(s.outputDevice == "Other ASIO");
  // No file: the defaults (WASAPI shared, the default device, 48 kHz, 256).
  CHECK(AudioDeviceManager::startupSettings(parse({}), (dir / "none.json").string()) == AudioSettings{});
}

// ---- Device manager -----------------------------------------------------------------------------------------------

TEST_CASE("manager: switching driver type, device, rate and buffer while playing reopens the stream; no audio-thread "
          "allocation; the Program and the transport are kept") {
  FakeHardware hw;
  const fs::path dir = tempDir("switch");
  const std::string path = (dir / "settings.json").string();
  auto link = makeLink();
  AudioSettings s;  // WASAPI shared, system default, 48 kHz, 256
  AudioDeviceManager m(*link, s, hw.hooks(path));
  REQUIRE(waitUntil(m, [&] { return runningWith(m, s); }));
  const uint64_t violations0 = alloc_trap::violationCount();
  link->play(0);
  REQUIRE(advances(*link));
  const uint64_t compiles0 = link->stats().compiles;

  auto step = [&](AudioSettings next, const char* what) {
    INFO(what);
    const int opens0 = hw.openCount();
    const Ticks before = link->playhead().tick;
    m.apply(next);
    REQUIRE(waitUntil(m, [&] { return runningWith(m, next) && hw.openCount() > opens0; }));
    CHECK(link->playhead().playing);
    CHECK(advances(*link));
    CHECK(link->playhead().tick >= before);  // the transport carried on (it may have moved on while reopening)
    s = next;
  };
  {
    AudioSettings n = s;
    n.bufferFrames = 128;
    step(n, "buffer 256 -> 128");
    CHECK(hw.lastConfig().bufferFrames == 128);
  }
  {
    AudioSettings n = s;
    n.driver = AudioDriverType::WasapiExclusive;
    n.outputDevice = "{b}";
    n.bufferFrames = 192;
    step(n, "WASAPI exclusive on the second device");
    CHECK(hw.lastConfig().outputEndpointId == "{b}");
    CHECK(hw.lastConfig().shareMode == engine::ShareMode::Exclusive);
    CHECK(m.status().deviceName == "Headphones (Fake HDA)");
  }
  {
    AudioSettings n = s;
    n.driver = AudioDriverType::Asio;
    n.outputDevice = "Fake ASIO";
    n.bufferFrames = 64;
    step(n, "ASIO");
    CHECK(hw.lastConfig().inputChannels == 0);  // T6.7b: no track uses an input
    CHECK(m.status().inputChannels == 0);
  }
  CHECK(link->stats().compiles == compiles0);  // a device change compiles nothing
  CHECK(link->engine().config().sampleRate == 48000.0);
  {
    AudioSettings n = s;
    n.sampleRate = 96000;
    step(n, "rate 48 -> 96 kHz: the engine is rebuilt at 96 kHz");
    CHECK(link->engine().config().sampleRate == 96000.0);
    CHECK(link->config().sampleRate == 96000.0);
    CHECK(m.rateChanges() == 1);
    CHECK(hw.lastConfig().sampleRate == 96000);
  }
  {
    AudioSettings n = s;
    n.sampleRate = 44100;
    n.driver = AudioDriverType::WasapiShared;
    n.outputDevice.clear();
    n.bufferFrames = 512;
    step(n, "everything at once, back to WASAPI shared at 44.1 kHz");
    CHECK(link->engine().config().sampleRate == 44100.0);
  }
  CHECK(alloc_trap::violationCount() == violations0);
  // Every apply was saved; the file holds the last.
  auto saved = loadAudioSettings(path);
  REQUIRE(saved.ok());
  CHECK(saved->settings == m.settings());
  CHECK(saved->settings.sampleRate == 44100);
}

TEST_CASE("manager T6.7b: the stream opens the inputs the tracks use (bounded by the device), reopens only when that "
          "changes and never while recording; the strips see the device's inputs") {
  FakeHardware hw;
  const fs::path dir = tempDir("inputs");
  doc::DocumentStore store{playback_fixture::unwiredDefaultProject()};
  LinkConfig lc;
  lc.compileThread = false;
  lc.sleepAfterMs = 0.0f;
  lc.unsavedDir = (fs::path(dir) / "recorded").string();
  auto made = ProjectLink::create(lc, store.current());
  REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
  std::unique_ptr<ProjectLink> link = std::move(made.value());
  link->setRecordStore(&store);
  const doc::ListenerId sub = store.subscribe([&](const doc::StoreEvent& e) { link->onDocumentChange(e); });
  AudioSettings s;
  s.driver = AudioDriverType::Asio;
  s.outputDevice = "Fake ASIO";
  AudioDeviceManager m(*link, s, hw.hooks((fs::path(dir) / "settings.json").string()));
  REQUIRE(waitUntil(m, [&] { return runningWith(m, s); }));
  CHECK(hw.lastConfig().inputChannels == 0);  // no track uses an input
  CHECK(link->audioInputChannels() == 4);     // the strips list the driver's 4 inputs
  auto opensSettle = [&](int opens0, double wait = 3.0) {  // waits for a reopen (if any) to finish
    (void)waitUntil(m, [&] { return hw.openCount() > opens0 && runningWith(m, s); }, wait);
    return hw.openCount() - opens0;
  };
  // An audio track on input 3: 3 inputs open.
  auto t = doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Audio);
  REQUIRE(t.ok());
  REQUIRE(store.dispatch(t->command).ok());
  const doc::TrackId id = t->tracks.at(0);
  int opens0 = hw.openCount();
  REQUIRE(store.dispatch(doc::SetTrackAudioInput{id, doc::AudioInput{2, 1}}).ok());
  CHECK(opensSettle(opens0) == 1);
  CHECK(hw.lastConfig().inputChannels == 3);
  CHECK(m.status().inputChannels == 3);
  // Inputs 7/8 on a 4-input driver: bounded to 4.
  opens0 = hw.openCount();
  REQUIRE(store.dispatch(doc::SetTrackAudioInput{id, doc::AudioInput{6, 2}}).ok());
  CHECK(opensSettle(opens0) == 1);
  CHECK(hw.lastConfig().inputChannels == 4);
  // Arming, the monitor, or a change that keeps the count: no reopen.
  opens0 = hw.openCount();
  auto armed = doc::ops::armTrack(*store.current(), id, true);
  REQUIRE(armed.ok());
  REQUIRE(store.dispatch(armed->command).ok());
  REQUIRE(store.dispatch(doc::SetTrackMonitor{id, doc::MonitorMode::In}).ok());
  REQUIRE(store.dispatch(doc::SetTrackAudioInput{id, doc::AudioInput{3, 1}}).ok());
  CHECK(opensSettle(opens0, 0.3) == 0);
  // While recording, a change waits for the session's end.
  opens0 = hw.openCount();
  REQUIRE(store.dispatch(doc::SetTrackAudioInput{id, doc::AudioInput{1, 1}}).ok());  // 2 inputs from now
  CHECK(opensSettle(opens0) == 1);
  link->record(0);
  REQUIRE(link->recordingActive());
  opens0 = hw.openCount();
  REQUIRE(store.dispatch(doc::SetTrackAudioInput{id, doc::AudioInput{0, 1}}).ok());
  CHECK(opensSettle(opens0, 0.3) == 0);
  CHECK(hw.lastConfig().inputChannels == 2);
  link->stop();
  REQUIRE(waitUntil(m, [&] {
    (void)link->playhead();  // the UI's poll: the session ends and commits
    return !link->recordingActive();
  }));
  CHECK(opensSettle(opens0) == 1);
  CHECK(hw.lastConfig().inputChannels == 1);
  // WASAPI: inputs only with an input device; then bounded by its channels (2).
  opens0 = hw.openCount();
  REQUIRE(store.dispatch(doc::SetTrackAudioInput{id, doc::AudioInput{2, 2}}).ok());  // 4 used
  CHECK(opensSettle(opens0) == 1);
  AudioSettings w;
  w.driver = AudioDriverType::WasapiShared;
  s = w;
  m.apply(w);
  REQUIRE(waitUntil(m, [&] { return runningWith(m, w) && hw.lastConfig().outputEndpointId.empty(); }));
  CHECK(hw.lastConfig().inputChannels == 0);
  w.inputDevice = "{c}";
  s = w;
  opens0 = hw.openCount();
  m.apply(w);
  // The first open asks for 4 (the endpoint's channels are not known yet); the device opens 2 and tells: then 2.
  REQUIRE(waitUntil(m, [&] {
    return hw.lastConfig().inputEndpointId == "{c}" && hw.lastConfig().inputChannels == 2 && runningWith(m, w) &&
           m.status().inputChannels == 2;
  }));
  CHECK(hw.openCount() - opens0 <= 2);
  CHECK(link->audioInputChannels() == 2);
  store.unsubscribe(sub);
}

TEST_CASE("manager: the settings persist across restarts") {
  FakeHardware hw;
  const fs::path dir = tempDir("persist");
  const std::string path = (dir / "settings.json").string();
  AudioSettings chosen;
  {
    auto link = makeLink();
    AudioDeviceManager m(*link, AudioDeviceManager::startupSettings(CliOptions{}, path), hw.hooks(path));
    REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Running; }));
    chosen = m.settings();
    chosen.driver = AudioDriverType::WasapiExclusive;
    chosen.outputDevice = "{a}";
    chosen.outputName = "Speakers (Fake USB)";
    chosen.bufferFrames = 288;
    m.apply(chosen);
    REQUIRE(waitUntil(m, [&] { return runningWith(m, chosen); }));
  }
  // "Restart": a new link and manager start from the file.
  const AudioSettings s = AudioDeviceManager::startupSettings(CliOptions{}, path);
  CHECK(s == chosen);
  auto link = makeLink(s.sampleRate);
  const int opens0 = hw.openCount();
  AudioDeviceManager m(*link, s, hw.hooks(path));
  REQUIRE(waitUntil(m, [&] { return runningWith(m, chosen) && hw.openCount() > opens0; }));
  CHECK(hw.lastConfig().outputEndpointId == "{a}");
  CHECK(hw.lastConfig().bufferFrames == 288);
  CHECK(hw.lastConfig().shareMode == engine::ShareMode::Exclusive);
}

TEST_CASE("manager: a device lost while playing falls back to silence; the hardware-change event reopens it") {
  FakeHardware hw;
  const fs::path dir = tempDir("hotplug");
  auto link = makeLink();
  AudioSettings s;
  s.outputDevice = "{a}";
  AudioDeviceManager m(*link, s, hw.hooks((dir / "settings.json").string()));
  REQUIRE(waitUntil(m, [&] { return runningWith(m, s); }));
  link->play(0);
  REQUIRE(advances(*link));
  {
    std::lock_guard lock(hw.m);
    hw.present.erase("{a}");
    REQUIRE(hw.current != nullptr);
    hw.current->unplug();
  }
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::NoDevice; }));
  CHECK(m.status().message.find("not connected") != std::string::npos);
  CHECK(m.output().status().deviceLosses == 1);
  CHECK(advances(*link));  // the engine keeps running on the null device (playhead, compiles), silently
  // The interface is plugged back in: the notifier's event (hardwareChanged) retries the configured device.
  {
    std::lock_guard lock(hw.m);
    hw.present.insert("{a}");
  }
  m.output().hardwareChanged();
  REQUIRE(waitUntil(m, [&] { return runningWith(m, s) && m.status().deviceName == "Speakers (Fake USB)"; }));
  CHECK(link->playhead().playing);
  CHECK(advances(*link));
  CHECK(m.settings().outputDevice == "{a}");  // the user's choice was kept throughout
}

TEST_CASE("manager: device lists and capabilities from the scripted list; lists refresh after a hardware change") {
  FakeHardware hw;
  const fs::path dir = tempDir("lists");
  auto link = makeLink();
  AudioDeviceManager m(*link, AudioSettings{}, hw.hooks((dir / "settings.json").string()));
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Running; }));
  auto outs = m.outputDevices(AudioDriverType::WasapiShared);
  REQUIRE(outs.size() == 3);
  CHECK(outs[0].id.empty());
  CHECK(outs[0].name == "System default (Speakers (Fake USB))");
  CHECK(outs[2].name == "Headphones (Fake HDA)");
  auto ins = m.inputDevices(AudioDriverType::WasapiShared);
  REQUIRE(ins.size() == 2);
  CHECK(ins[1].name == "Line In (Fake USB)");
  auto asio = m.outputDevices(AudioDriverType::Asio);
  REQUIRE(asio.size() == 2);
  CHECK(asio[0].id == "Fake ASIO");
  AudioSettings ex;
  ex.driver = AudioDriverType::WasapiExclusive;
  ex.outputDevice = "{a}";
  auto caps = m.capabilities(ex);
  CHECK(caps.bufferSizes.front() == 144);  // the exclusive minimum period
  CHECK(caps.sampleRates == std::vector<uint32_t>{44100, 48000, 96000});
  // Hot-plug: a new endpoint appears; the lists are stale until the next hardware-change event.
  {
    std::lock_guard lock(hw.m);
    engine::DeviceInfo d = hw.wasapi[1];
    d.endpointId = "{d}";
    d.name = "Monitors (New USB)";
    hw.wasapi.push_back(d);
  }
  CHECK(m.outputDevices(AudioDriverType::WasapiShared).size() == 3);  // cached
  m.output().hardwareChanged();
  m.poll();
  CHECK(m.outputDevices(AudioDriverType::WasapiShared).size() == 4);
}

TEST_CASE("audio output: the test tone is RT-safe, clamped to -12 dBFS and phase-continuous; it keeps the stream "
          "running while the engine sleeps") {
  FakeHardware hw;
  hw.manual = true;
  auto link = makeLink(48000.0, 50.0f);  // the engine sleeps after 50 ms of quiet
  OutputConfig c;
  c.bufferFrames = 256;
  AudioOutput out(link->engine(), c, hw.factory());
  const auto end = clock_t_::now() + std::chrono::seconds(10);
  while (!(out.status().state == OutputStatus::State::Running) && clock_t_::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  REQUIRE(out.status().state == OutputStatus::State::Running);
  FakeDevice* dev = nullptr;
  {
    std::lock_guard lock(hw.m);
    dev = hw.current;
  }
  REQUIRE(dev != nullptr);
  CHECK(throughPreroll(out, [&] { dev->null().processBlock(); }) > 0);
  auto peakOf = [&](int blocks, std::vector<float>* keep = nullptr) {
    float pk = 0.0f;
    for (int b = 0; b < blocks; ++b) {
      CHECK(alloc_trap::countRtViolations([&] { dev->null().processBlock(); }) == 0);
      for (uint32_t i = 0; i < 256; ++i) {
        pk = std::max(pk, std::fabs(dev->null().output(0)[i]));
        if (keep) keep->push_back(dev->null().output(0)[i]);
      }
    }
    return pk;
  };
  CHECK(peakOf(20) == 0.0f);  // the empty fixture start: silence (and the engine falls asleep)
  CHECK(link->engine().asleep());
  out.setTestTone(true, -24.0f, 1000.0f);
  std::vector<float> x;
  const float pk = peakOf(40, &x);
  CHECK(pk == doctest::Approx(std::pow(10.0f, -24.0f / 20.0f)).epsilon(0.002));
  CHECK(dev->null().output(1)[17] == dev->null().output(0)[17]);  // both master outputs
  // 1 kHz at 48 kHz: 48 samples per period; continuous across block boundaries (no step above the slope bound).
  int crossings = 0;
  float maxStep = 0.0f;
  for (size_t i = 1; i < x.size(); ++i) {
    crossings += (x[i - 1] < 0.0f) != (x[i] < 0.0f);
    maxStep = std::max(maxStep, std::fabs(x[i] - x[i - 1]));
  }
  CHECK(std::abs(crossings - static_cast<int>(2.0 * x.size() / 48.0)) <= 2);
  CHECK(maxStep <= pk * 2.0f * 3.14159265f / 48.0f * 1.01f);
  out.setTestTone(true, 0.0f, 1000.0f);  // too loud: clamped
  CHECK(peakOf(10) == doctest::Approx(std::pow(10.0f, AudioOutput::kMaxToneDb / 20.0f)).epsilon(0.002));
  out.setTestTone(false, -24.0f, 1000.0f);
  CHECK(peakOf(5) == 0.0f);
}

TEST_CASE("manager: the stream keeps running while the app is in the foreground; it suspends only after the "
          "background idle (the CPU meter then reads 0); the foreground restarts it") {
  FakeHardware hw;
  const fs::path dir = tempDir("cpu");
  auto link = makeLink(48000.0, 50.0f);
  ManagerHooks hooks = hw.hooks((dir / "settings.json").string());
  hooks.factory.backgroundSuspendSeconds = 0.5;
  AudioDeviceManager m(*link, AudioSettings{}, hooks);
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Running; }));
  link->play(0);
  (void)m.cpuLoad();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const double load = m.cpuLoad();
  MESSAGE("callback load while playing on the null device: " << load << " %");
  CHECK(load > 0.0);
  CHECK(load < 100.0);
  link->stop();
  // AU.stream: quiet → the engine sleeps (50 ms); in the foreground the stream keeps running (it used to stop after
  // 1 s, so every Play restarted the driver).
  REQUIRE(waitUntil(m, [&] { return link->engine().asleep(); }));
  CHECK_FALSE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Suspended; }, 2.0));
  CHECK(m.status().state == views::AudioStatus::State::Running);
  CHECK(m.output().suspensions() == 0);
  // In the background, after backgroundSuspendSeconds of the engine asleep: suspended.
  const auto bg = clock_t_::now();
  m.output().setForeground(false);
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Suspended; }, 15.0));
  CHECK(std::chrono::duration<double>(clock_t_::now() - bg).count() >= 0.5);
  CHECK(m.cpuLoad() == 0.0);
  CHECK(m.output().suspensions() == 1);
  // Back in the foreground: it restarts at once (and pre-rolls while the user reaches for Play).
  m.output().setForeground(true);
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Running; }));
  // In the background again, a wake (play) restarts a suspended stream too.
  m.output().setForeground(false);
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Suspended; }, 15.0));
  link->play(0);
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Running; }));
  CHECK(advances(*link));
}

// KB.3: the user's "after the DAW stops for a few seconds, playing the keyboard shows no audio in the mixer". The views
// poll the link's meters only while live() and stop once the engine sleeps; only the transport re-armed them. Through the
// real link + AudioOutput on the fake device's real-time thread: the poll the views make (read, then live()) stops; a
// computer-keyboard key then notifies the views exactly once (setSignalNotify → the shell's kSignal), live() is true at
// once (before the audio thread has run a block), and the track's meter lights; released, the meter falls to the floor
// and live() ends. The same after the background suspend (the key restarts the stream), and no held peak survives it.
TEST_CASE("KB.3: a keyboard note after the engine slept, or after the stream suspended, lights the track meter; the meter "
          "falls back to silence after") {
  FakeHardware hw;
  const fs::path dir = tempDir("kb3");
  doc::DocumentStore store{playback_fixture::unwiredDefaultProject()};
  const doc::TrackId track = store.ids().track();
  {
    doc::Batch b{"kb3", {}};
    b.commands.push_back(doc::AddTrack{playback_fixture::track(track, doc::TrackKind::Midi, "Keys"), {}, doc::kAppend});
    (void)playback_fixture::instrument(b, store.ids(), track, store.ids().device());
    REQUIRE(store.dispatch(b).ok());
    playback_fixture::wireSeries(store);
    auto arm = doc::ops::armTrack(*store.current(), track, false);
    REQUIRE(arm.ok());
    REQUIRE(store.dispatch(arm->command).ok());
  }
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 50.0f;
  auto made = ProjectLink::create(c, store.current());
  REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
  ProjectLink& link = *made.value();
  std::atomic<int> notified{0};
  link.setSignalNotify([](void* u) { static_cast<std::atomic<int>*>(u)->fetch_add(1); }, &notified);
  ManagerHooks hooks = hw.hooks((dir / "settings.json").string());
  hooks.factory.backgroundSuspendSeconds = 0.3;
  AudioDeviceManager m(link, AudioSettings{}, hooks);
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Running; }));
  const doc::TrackId ids[1] = {track};
  constexpr float kMeterFloorDb = -60.0f;  // ui::signal_theme::kMeterFloorDb (theme.h clashes with the Windows headers here)
  ui::MeterLevels lv[1];
  auto peakDb = [&] {  // one poll of the views (MixerController::pollMeters)
    link.read(ids, lv);
    return std::max(lv[0].peakDb[0], lv[0].peakDb[1]);
  };
  auto viewsStop = [&] { return waitUntil(m, [&] { return (void)peakDb(), !link.live(); }, 15.0); };
  auto keySounds = [&](const char* when) {
    INFO(when);
    notified = 0;
    link.liveKey(1, 0, 100.0f, true);
    CHECK(notified.load() == 1);  // the views re-arm their polls
    CHECK(link.live());          // and keep polling until the audio thread has woken the engine
    CHECK(waitUntil(m, [&] { return peakDb() > -48.0f; }, 5.0));
    link.liveKey(1, 2, 100.0f, true);  // while the polls run: no second notify
    CHECK(notified.load() == 1);
    link.liveKey(1, 0, 0.0f, false);
    link.liveKey(1, 2, 0.0f, false);
    REQUIRE(viewsStop());  // the release decays, the engine sleeps, the meter falls below the floor
    CHECK(peakDb() <= kMeterFloorDb);
    CHECK(link.engine().asleep());
  };

  // The user's case: stopped for a few seconds (the engine asleep, the stream running in the foreground).
  link.play(0);
  REQUIRE(advances(link));
  link.stop();
  REQUIRE(viewsStop());
  keySounds("engine asleep, stream running");
  CHECK(m.output().suspensions() == 0);

  // In the background, after the suspend: the key restarts the stream (pre-roll included) and the meter lights.
  m.output().setForeground(false);
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Suspended; }, 15.0));
  CHECK_FALSE(link.live());
  keySounds("stream suspended");
  // Suspended again: no peak held from before (the views' last poll and any later one read the floor).
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Suspended; }, 15.0));
  CHECK(m.output().suspensions() == 2);
  CHECK(peakDb() <= kMeterFloorDb);
  CHECK_FALSE(link.live());
  link.setSignalNotify(nullptr, nullptr);
}

// ---- ASIO driver requests through the real AsioDevice -----------------------------------------------------------------

TEST_CASE("asio: a driver reset with a new buffer size re-prepares the block adapter; a driver rate change silences "
          "the output until the manager rebuilds the engine at the driver's rate") {
  test::FakeDriver drv;
  drv.minSize = 32;
  drv.maxSize = 1024;
  drv.prefSize = 256;
  drv.sendRateChangeOnSet = false;
  const fs::path dir = tempDir("asio");
  auto link = makeLink(48000.0, 0.0f);
  ManagerHooks h;
  h.factory.create = [&drv](engine::Backend) -> Result<std::unique_ptr<engine::AudioDevice>> {
    return std::unique_ptr<engine::AudioDevice>(std::make_unique<engine::AsioDevice>(test::fakeSource(drv)));
  };
  h.factory.watchHardware = false;
  h.factory.allowEnvDisable = false;
  h.asioDrivers = [] { return std::vector<engine::AsioDriverEntry>{{"Fake ASIO", "{0}"}}; };
  h.settingsPath = (dir / "settings.json").string();
  AudioSettings s;
  s.driver = AudioDriverType::Asio;
  s.outputDevice = "Fake ASIO";
  s.bufferFrames = 256;
  AudioDeviceManager m(*link, s, h);
  REQUIRE(waitUntil(m, [&] { return runningWith(m, s); }));
  CHECK(drv.started);
  link->play(0);
  // The driver's audio thread: buffer switches (the host converts, runs the engine through the adapter).
  auto ticks = [&](int n) {
    for (int i = 0; i < n; ++i) drv.tick(i & 1);
  };
  const uint64_t blocks0 = link->engine().stats().blocks;
  int half = 0;
  CHECK(throughPreroll(m.output(), [&] { drv.tick(half++ & 1); }) > 0);
  CHECK(link->engine().stats().blocks == blocks0);  // AU.stream: the pre-roll does not run the engine
  ticks(8);
  CHECK(link->engine().stats().blocks == blocks0 + 8 * 256 / 64);
  // Capabilities come from the open driver.
  const auto caps = m.capabilities(s);
  CHECK(caps.hasControlPanel);
  CHECK(caps.bufferSizes.front() == 32);
  CHECK(caps.bufferSizes.back() == 1024);
  CHECK(caps.sampleRates == std::vector<uint32_t>{44100, 48000, 96000});

  // 1. The control panel changed the buffer size: the driver asks for a reset; AsioDevice reloads it with 128 frames
  //    (the only size it now allows) and calls the output's stream-changed callback.
  drv.minSize = drv.maxSize = drv.prefSize = 128;
  REQUIRE(drv.cb != nullptr);
  drv.cb->message(test::a::kMsgResetRequest, 0, nullptr, nullptr);
  REQUIRE(waitUntil(m, [&] { return m.output().status().streamChanges == 1; }));
  CHECK(m.status().bufferFrames == 128);
  CHECK(m.output().config().bufferFrames == 128);
  CHECK_FALSE(m.output().status().rateMismatch);
  CHECK(m.output().prerolls() == 2);  // the driver's reset is a cold start
  (void)throughPreroll(m.output(), [&] { drv.tick(half++ & 1); });
  const uint64_t blocks1 = link->engine().stats().blocks;
  ticks(8);
  CHECK(link->engine().stats().blocks == blocks1 + 8 * 128 / 64);  // direct mode at the new size

  // 2. The driver switched itself to 96 kHz (sampleRateDidChange): the stream is recreated at 96 kHz; until the
  //    engine runs at 96 kHz the output is silence and the engine is not run.
  drv.rate = 96000;
  drv.cb->sampleRateDidChange(96000);
  REQUIRE(waitUntil(m, [&] { return m.output().status().streamChanges == 2; }));
  // The manager (poll, on the UI thread) follows: close → engine rebuilt at 96 kHz → reopen.
  REQUIRE(waitUntil(m, [&] {
    return link->engine().config().sampleRate == 96000.0 && m.status().state == views::AudioStatus::State::Running &&
           m.status().sampleRate == 96000 && !m.output().status().rateMismatch;
  }));
  CHECK(m.settings().sampleRate == 96000);
  CHECK(m.rateChanges() == 1);
  (void)throughPreroll(m.output(), [&] { drv.tick(half++ & 1); });
  const uint64_t blocks2 = link->engine().stats().blocks;
  ticks(4);
  CHECK(link->engine().stats().blocks > blocks2);
  CHECK(link->playhead().playing);
  CHECK(engine::AsioDevice().drivers().ok());  // (the registry list still works with a driver loaded)
}

// ---- AU.stream: the cold-start pre-roll and the fade before a driver reset --------------------------------------------

TEST_CASE("output: a cold start pre-rolls silence until the driver has settled; Play pressed meanwhile starts with the "
          "first block the engine runs; an xrun restarts the settling; the pre-roll is bounded") {
  FakeHardware hw;
  hw.manual = true;
  auto link = makeLink(48000.0, 0.0f);
  OutputConfig c;
  c.bufferFrames = 256;
  AudioOutput out(link->engine(), c, hw.factory());
  const auto end = clock_t_::now() + std::chrono::seconds(10);
  while (out.status().state != OutputStatus::State::Running && clock_t_::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  REQUIRE(out.status().state == OutputStatus::State::Running);
  FakeDevice* dev = nullptr;
  {
    std::lock_guard lock(hw.m);
    dev = hw.current;
  }
  REQUIRE(dev != nullptr);
  REQUIRE(out.prerolling());
  CHECK(link->engine().audioStopped());  // the engine waits offline
  out.setTestTone(true, -24.0f, 1000.0f);  // sounding sources wait for the pre-roll too
  link->play(0);                           // Play during the cold start: deferred, not lost
  const uint64_t blocks0 = link->engine().stats().blocks;
  const uint64_t trace0 = engine::streamTrace().head();
  // The fake reports one buffer of latency: target = max(256 + 4 × 256, 0.25 s) = 12000 frames = 47 callbacks, the
  // last kPrerollCleanBuffers of them clean. An xrun in the 45th (seen by the 46th: the device records it after the
  // callback) restarts the clean count: settled at the 50th.
  int n = 0;
  float peak = 0;
  while (out.prerolling() && n < 1000) {
    CHECK(alloc_trap::countRtViolations([&] { dev->null().processBlock(0, false, n == 44); }) == 0);
    for (uint32_t i = 0; i < 256; ++i) peak = std::max(peak, std::fabs(dev->null().output(0)[i]));
    ++n;
  }
  CHECK(peak == 0.0f);                                  // silence throughout
  CHECK(link->engine().stats().blocks == blocks0);      // the engine (and its transport) did not run
  CHECK(n == 50);
  CHECK(n * 256 >= std::lround(AudioOutput::kPrerollSeconds * 48000));
  CHECK_FALSE(link->engine().audioStopped());           // online from the pre-roll's end
  // The next callback runs the engine: the transport starts at 0 now, and the tone sounds.
  dev->null().processBlock();
  CHECK(link->engine().stats().blocks == blocks0 + 256 / 64);
  float after = 0;
  for (uint32_t i = 0; i < 256; ++i) after = std::max(after, std::fabs(dev->null().output(0)[i]));
  CHECK(after > 0.01f);
  CHECK(link->playhead().playing);
  CHECK(link->playhead().tick < kPpq / 4);  // from 0, one callback in (not ahead by the pre-roll's length)
  // The trace: PrerollBegin (before this window), PrerollEnd settled after 50 buffers.
  std::vector<engine::TraceEvent> ev;
  (void)engine::streamTrace().read(trace0, ev);
  bool ended = false;
  for (const engine::TraceEvent& e : ev)
    if (e.kind == engine::TraceKind::PrerollEnd) {
      ended = true;
      CHECK(e.a == 50);
      CHECK(e.flags == 0);
    }
  CHECK(ended);

  // Bounded: a device that xruns on every buffer settles never; the engine starts after kPrerollMaxSeconds anyway.
  out.reconfigure(c);
  const auto end2 = clock_t_::now() + std::chrono::seconds(10);
  FakeDevice* dev2 = nullptr;
  while (clock_t_::now() < end2) {
    {
      std::lock_guard lock(hw.m);
      dev2 = hw.opens >= 2 ? hw.current : nullptr;
    }
    if (dev2 && out.status().state == OutputStatus::State::Running) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE(dev2 != nullptr);
  REQUIRE(out.prerolling());
  int m = 0;
  while (out.prerolling() && m < 1000) {
    dev2->null().processBlock(0, false, true);
    ++m;
  }
  CHECK(m == static_cast<int>((std::lround(AudioOutput::kPrerollMaxSeconds * 48000) + 255) / 256));  // 188 callbacks
  out.setTestTone(false, -24.0f, 1000.0f);
}

TEST_CASE("asio: a driver reset while audio sounds fades out and drains before the driver stops, then pre-rolls "
          "silence before the engine runs again") {
  test::FakeDriver drv;
  drv.outType = test::a::kFloat32Lsb;  // read the driver's halves directly
  drv.minSize = drv.maxSize = drv.prefSize = 256;
  drv.outLatency = 512;
  drv.sendRateChangeOnSet = false;
  auto link = makeLink(48000.0, 0.0f);
  DeviceFactory f;
  f.create = [&drv](engine::Backend) -> Result<std::unique_ptr<engine::AudioDevice>> {
    return std::unique_ptr<engine::AudioDevice>(std::make_unique<engine::AsioDevice>(test::fakeSource(drv)));
  };
  f.watchHardware = false;
  f.allowEnvDisable = false;
  OutputConfig c;
  c.backend = engine::Backend::Asio;
  c.device = "Fake ASIO";
  c.bufferFrames = 256;
  AudioOutput out(link->engine(), c, f);
  const auto end = clock_t_::now() + std::chrono::seconds(10);
  while (out.status().state != OutputStatus::State::Running && clock_t_::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  REQUIRE(out.status().state == OutputStatus::State::Running);
  REQUIRE(drv.started);
  int half = 0;
  (void)throughPreroll(out, [&] { drv.tick(half++ & 1); });
  out.setTestTone(true, -24.0f, 1000.0f);  // audio sounding
  const float level = std::pow(10.0f, -24.0f / 20.0f);
  // The driver's audio thread: a switch every ~1 ms (the test's own clock), recording what each half got.
  std::atomic<bool> stopTicking{false};
  std::vector<std::vector<float>> got;
  got.reserve(1 << 14);
  std::thread driverThread([&] {
    std::vector<float> one(256);
    while (!stopTicking.load()) {
      if (drv.tickIfStarted(half & 1, one.data(), 256)) {
        ++half;
        if (got.size() < got.capacity()) got.push_back(one);
      }
      // About 1 ms per switch (a sleep would last a 15.6 ms timer tick).
      for (const auto until = clock_t_::now() + std::chrono::milliseconds(1); clock_t_::now() < until;)
        std::this_thread::yield();
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const uint64_t trace0 = engine::streamTrace().head();
  drv.cb->message(test::a::kMsgResetRequest, 0, nullptr, nullptr);
  const auto t0 = clock_t_::now();
  while (out.status().streamChanges == 0 && clock_t_::now() - t0 < std::chrono::seconds(10))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE(out.status().streamChanges == 1);
  CHECK(out.fades() == 1);
  // Through the new pre-roll and a little beyond, then stop ticking.
  while (out.prerolling() && clock_t_::now() - t0 < std::chrono::seconds(10))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  stopTicking = true;
  driverThread.join();
  out.setTestTone(false, -24.0f, 1000.0f);
  // What the driver was given: tone → a ramp to 0 (no step) → silence for ≥ latency + one buffer → (the stop, the
  // reopen) → silence for the pre-roll → a ramp back up into the tone. Never a jump between the tone and 0.
  REQUIRE(got.size() > 10);
  std::vector<float> x;
  for (const auto& b : got) x.insert(x.end(), b.begin(), b.end());
  float maxStep = 0;
  for (size_t i = 1; i < x.size(); ++i) maxStep = std::max(maxStep, std::fabs(x[i] - x[i - 1]));
  // The tone's own slope (1 kHz) is level·2π/48 per sample; a 5 ms ramp adds level/240 at most.
  CHECK(maxStep <= level * (2.0f * 3.14159265f / 48.0f + 1.0f / 240.0f) * 1.05f);
  // Silence between the fade and the restart: the longest zero run covers the drain (512 + 256) and the pre-roll.
  size_t run = 0, longest = 0;
  for (float v : x) {
    run = v == 0.0f ? run + 1 : 0;
    longest = std::max(longest, run);
  }
  CHECK(longest >= 512 + 256 + static_cast<size_t>(std::lround(AudioOutput::kPrerollSeconds * 48000)));
  // The trace tells the story in order.
  std::vector<engine::TraceEvent> ev;
  (void)engine::streamTrace().read(trace0, ev);
  std::vector<engine::TraceKind> story;
  for (const engine::TraceEvent& e : ev)
    if (e.kind != engine::TraceKind::Buffer && e.kind != engine::TraceKind::Message) story.push_back(e.kind);
  const std::vector<engine::TraceKind> want = {engine::TraceKind::Reopen,   engine::TraceKind::FadeOut,
                                               engine::TraceKind::Stop,     engine::TraceKind::Stopped,
                                               engine::TraceKind::Open,     engine::TraceKind::Reopened,
                                               engine::TraceKind::PrerollBegin, engine::TraceKind::Start,
                                               engine::TraceKind::Started,  engine::TraceKind::PrerollEnd};
  CHECK(story == want);
  for (const engine::TraceEvent& e : ev)
    if (e.kind == engine::TraceKind::FadeOut) CHECK(e.flags == 0);  // drained, not timed out
  engine::TraceFormat fmt;
  MESSAGE(engine::describeStreamTrace(ev, 0, fmt));
}


// ---- Real devices (opt-in) ----------------------------------------------------------------------------------------

// AU.stream (opt-in, PQ_TEST_ASIO_LOOPBACK=1; real hardware, inaudible): through the app's own AudioOutput on a real
// driver (PQ_TEST_ASIO_DRIVER, default "FL Studio ASIO"), every cold start pre-rolls past the driver's start-up
// glitch (FL Studio ASIO dips its output ≈ 125 ms into every stream: tests/engine/test_asio.cpp's loopback probe);
// a -90 dBFS 1500 Hz test tone switched on during the pre-roll is tracked on the endpoint's loopback. Then the idle cost
// of a running stream with the engine asleep: process CPU time over 10 s and the callback's own time.
TEST_CASE("live: a real driver's cold start through AudioOutput pre-rolls past its start-up glitch; the idle cost of "
          "a running stream (PQ_TEST_ASIO_LOOPBACK=1)") {
  const char* env = std::getenv("PQ_TEST_ASIO_LOOPBACK");
  if (env == nullptr || std::string(env) != "1") {
    MESSAGE("skipped: set PQ_TEST_ASIO_LOOPBACK=1 to start a real ASIO driver with a -90 dBFS tone and record its endpoint");
    return;
  }
  float peak = 0;
  std::string err;
  if (!test::endpointQuiet(&peak, &err)) {
    MESSAGE("skipped: the default endpoint is playing other audio (peak " << peak << ") " << err);
    return;
  }
  auto link = makeLink(48000.0, 50.0f);
  OutputConfig c;
  const char* be = std::getenv("PQ_TEST_LOOPBACK_BACKEND");
  const bool wasapi = be && std::string(be) == "wasapi";
  c.backend = wasapi ? engine::Backend::Wasapi : engine::Backend::Asio;
  const char* drv = std::getenv("PQ_TEST_ASIO_DRIVER");
  if (!wasapi) c.device = drv ? drv : "FL Studio ASIO";
  c.bufferFrames = 256;
  DeviceFactory f;
  f.watchHardware = false;
  f.allowEnvDisable = false;
  for (int run = 0; run < 3; ++run) {
    std::unique_ptr<AudioOutput> out;
    test::ProbeCapture cap;
    cap.run(2500, [&] {
      out = std::make_unique<AudioOutput>(link->engine(), c, f);
      out->setTestTone(true, -90.0f, 1500.0f);
    });
    REQUIRE(cap.error.empty());
    REQUIRE(out->status().state == OutputStatus::State::Running);
    const OutputStatus st = out->status();
    out.reset();  // the tone sounds: a fade, then the stop
    int anomalies = 0;
    std::vector<double> times;
    double toneAt = -1;
    const std::string text = test::analyseTone(cap, 1500.0, std::pow(10.0f, -90.0f / 20.0f), &anomalies, &times, &toneAt);
    MESSAGE("run " << run << ": " << st.deviceName << ", " << st.stream.sampleRate << " Hz, " << st.stream.bufferFrames
                   << " frames, output latency " << st.stream.latencyFrames << "\n" << text);
    CHECK(toneAt > 0);
    // FL Studio ASIO's start-up dip sits at a fixed place in its output: samples ≈ 6016-6272 (125 ms) of every stream.
    // The pre-roll (≥ 12000 frames of output) puts the tone's first sample after it, so the dip falls in the silence.
    // Its steady-state dropouts (5-10 ms of zeros every few seconds, whether or not the stream just started) can land
    // anywhere, near the start too: they are listed above, not checked.
    CHECK(toneAt > 250.0);
  }
  // Idle: a running stream with the engine asleep, against the same process with no stream.
  auto cpu = [] {
    FILETIME a, b, k, u;
    GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
    auto v = [](FILETIME t) { return static_cast<double>(static_cast<uint64_t>(t.dwHighDateTime) << 32 | t.dwLowDateTime) * 1e-7; };
    return v(k) + v(u);
  };
  double baseline = 0;
  {
    const double c0 = cpu();
    const auto w0 = clock_t_::now();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    baseline = 100.0 * (cpu() - c0) / std::chrono::duration<double>(clock_t_::now() - w0).count();
  }
  {
    AudioOutput out(link->engine(), c, f);
    const auto end = clock_t_::now() + std::chrono::seconds(10);
    while ((out.status().state != OutputStatus::State::Running || out.prerolling()) && clock_t_::now() < end)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(out.status().state == OutputStatus::State::Running);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    REQUIRE(link->engine().asleep());
    const engine::CallbackStats::Snapshot s0 = out.callbackStats();
    const double c0 = cpu();
    const auto w0 = clock_t_::now();
    std::this_thread::sleep_for(std::chrono::seconds(10));
    const double used = cpu() - c0;
    const double wall = std::chrono::duration<double>(clock_t_::now() - w0).count();
    const engine::CallbackStats::Snapshot s1 = out.callbackStats();
    MESSAGE("idle, engine asleep, stream running (" << out.status().deviceName << ", " << out.status().stream.bufferFrames
                                                    << " frames): process CPU " << 100.0 * used / wall
                                                    << " % of one core over " << wall << " s (no stream: "
                                                    << baseline << " %); callbacks "
                                                    << (s1.callbacks - s0.callbacks) << ", mean callback "
                                                    << s1.meanDurationUs << " us (period " << s1.periodUs << " us)");
    CHECK(link->engine().asleep());
    CHECK(out.suspensions() == 0);
  }
}


TEST_CASE("live: switch WASAPI shared -> exclusive -> ASIO on this machine's devices with a quiet test tone "
          "(PQ_TEST_AUDIO_DEVICE=1)") {
  const char* env = std::getenv("PQ_TEST_AUDIO_DEVICE");
  if (env == nullptr || *env == '\0' || *env == '0') {
    MESSAGE("skipped: set PQ_TEST_AUDIO_DEVICE=1 to play short tones at -30 dBFS on the real devices");
    return;
  }
  const fs::path dir = tempDir("live");
  auto link = makeLink();
  ManagerHooks h;
  h.settingsPath = (dir / "settings.json").string();
  h.persist = false;
  AudioDeviceManager m(*link, AudioSettings{}, h);
  m.setTestTone(true, -30.0f, 440.0f);
  auto settle = [&](const std::string& what) {
    (void)waitUntil(m, [&] { return m.status().state != views::AudioStatus::State::Opening; }, 10.0);
    const bool ok = waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Running; }, 0.5);
    const views::AudioStatus st = m.status();
    MESSAGE(what << ": " << std::string(ok ? "running" : "NOT running") << " on '" << st.deviceName << "' " << st.sampleRate << " Hz, "
                 << st.bufferFrames << " frames, out latency " << st.outputLatencyMs << " ms, cpu " << m.cpuLoad() << " % "
                 << st.message);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    return ok;
  };
  const uint64_t violations0 = alloc_trap::violationCount();
  CHECK(settle("WASAPI shared"));
  AudioSettings s = m.settings();
  s.driver = AudioDriverType::WasapiExclusive;
  s.bufferFrames = 288;
  m.apply(s);
  settle("WASAPI exclusive");
  const auto drivers = m.outputDevices(AudioDriverType::Asio);
  const char* want = std::getenv("PQ_TEST_ASIO_DRIVER");
  for (const views::AudioDeviceEntry& d : drivers) {
    if (want && d.id != want) continue;
    s.driver = AudioDriverType::Asio;
    s.outputDevice = d.id;
    s.bufferFrames = 64;
    m.apply(s);
    if (settle("ASIO " + d.name)) break;
  }
  CHECK(alloc_trap::violationCount() == violations0);
  m.setTestTone(false, -30.0f, 440.0f);
}
