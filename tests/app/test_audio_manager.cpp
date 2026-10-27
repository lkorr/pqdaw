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
//   - the test tone (RT-safe, level-clamped) and the CPU meter (0 while the stream is suspended).
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
#include "playback_fixture.h"
#include "pq/app/audio_manager.h"
#include "pq/app/audio_output.h"
#include "pq/app/cli.h"
#include "pq/app/project_link.h"
#include "pq/app/user_settings.h"
#include "pq/core/alloc_trap.h"
#include "pq/engine/asio_device.h"
#include "pq/engine/null_device.h"

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

Result<engine::StreamInfo> FakeDevice::open(const engine::StreamConfig& c, engine::AudioCallback cb) {
  std::string name;
  {
    std::lock_guard lock(hw_.m);
    const std::string id = c.outputEndpointId;
    if (!hw_.present.count(id)) return Error{ErrorCode::Device, "fake device '" + id + "' is not connected", {}};
    name = id.empty() ? std::string("Speakers (Fake USB)") : id;
    for (const engine::DeviceInfo& d : hw_.wasapi)
      if (d.endpointId == id) name = d.name;
    ++hw_.opens;
    hw_.configs.push_back(c);
    hw_.current = this;
  }
  auto r = dev_.open(c, cb);
  if (!r) return r;
  engine::StreamInfo si = *r;
  si.backend = backend_;
  si.outputDeviceName = name;
  si.latencyFrames = c.bufferFrames;
  si.inputLatencyFrames = c.inputChannels > 0 ? c.bufferFrames : 0;
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
  s.inputChannels = 2;
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
  REQUIRE(saveViewPrefs(path, v).ok());
  CHECK(loadViewPrefs(path) == v);
  REQUIRE(loadAudioSettings(path).ok());
  CHECK(loadAudioSettings(path)->settings.sampleRate == 96000);  // the audio section survives
  REQUIRE(saveAudioSettings(path, s).ok());
  CHECK(loadViewPrefs(path) == v);  // and the views section the audio save
  { std::ofstream(path) << R"({"views": {"circuitShowInactive": "yes"}})"; }
  CHECK_FALSE(loadViewPrefs(path).circuitShowInactive);  // the wrong type keeps the default
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
    n.inputChannels = 2;
    step(n, "ASIO, duplex");
    CHECK(hw.lastConfig().inputChannels == 2);
    CHECK(m.status().inputChannels == 2);
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
    n.inputChannels = 0;
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
  CHECK_FALSE(caps.inputSupported);
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

TEST_CASE("manager: the CPU meter reads the callback load while running and 0 once the idle stream is suspended") {
  FakeHardware hw;
  const fs::path dir = tempDir("cpu");
  auto link = makeLink(48000.0, 50.0f);
  AudioDeviceManager m(*link, AudioSettings{}, hw.hooks((dir / "settings.json").string()));
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Running; }));
  link->play(0);
  (void)m.cpuLoad();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const double load = m.cpuLoad();
  MESSAGE("callback load while playing on the null device: " << load << " %");
  CHECK(load > 0.0);
  CHECK(load < 100.0);
  link->stop();
  // Quiet → the engine sleeps (50 ms) → the output stops the stream after kSuspendAfterSeconds.
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Suspended; }, 15.0));
  CHECK(m.cpuLoad() == 0.0);
  CHECK(m.output().suspensions() >= 1);
  // A wake (play) restarts it.
  link->play(0);
  REQUIRE(waitUntil(m, [&] { return m.status().state == views::AudioStatus::State::Running; }));
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
  const uint64_t blocks2 = link->engine().stats().blocks;
  ticks(4);
  CHECK(link->engine().stats().blocks > blocks2);
  CHECK(link->playhead().playing);
  CHECK(engine::AsioDevice().drivers().ok());  // (the registry list still works with a driver loaded)
}

// ---- Real devices (opt-in) ----------------------------------------------------------------------------------------

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
