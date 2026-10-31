// T5.10: the browser's preview through the app (pq/app/preview_service.h): a selected file's head is loaded, so a
// click produces its first nonzero sample within the next device callback (a manual NullDevice: the test makes each
// callback); a cold WAV start (no selection first) and the time it takes; the cue output pair (outputs 3/4 with the
// master on 1/2); stop = a fade, then the cue source is detached and the output may sleep; every device callback
// RT-clean. Also the user settings the browser adds (Cue Out, the library's folders, where its data lives).

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pq/app/audio_output.h"
#include "pq/app/preview_service.h"
#include "pq/app/project_link.h"
#include "pq/app/user_settings.h"
#include "pq/core/alloc_trap.h"
#include "pq/document/model.h"
#include "pq/engine/null_device.h"
#include "pq/media/wav_file.h"

using namespace pq;
using namespace pq::app;
namespace fs = std::filesystem;

namespace {

// A NullDevice the test drives (start() starts no thread; the stream counts as running).
class ManualDevice final : public engine::AudioDevice {
public:
  engine::Backend backend() const noexcept override { return engine::Backend::Wasapi; }
  Result<std::vector<engine::DeviceInfo>> enumerate() override { return dev_.enumerate(); }
  Result<engine::StreamInfo> open(const engine::StreamConfig& c, engine::AudioCallback cb) override { return dev_.open(c, cb); }
  Status start() override {
    running_ = true;
    return Ok{};
  }
  Status stop() override {
    running_ = false;
    return Ok{};
  }
  void close() noexcept override { dev_.close(); }
  bool isOpen() const noexcept override { return dev_.isOpen(); }
  bool isRunning() const noexcept override { return running_; }
  const engine::CallbackStats& stats() const noexcept override { return dev_.stats(); }
  engine::NullDevice& null() noexcept { return dev_; }

private:
  engine::NullDevice dev_;
  bool running_ = false;
};

struct Rig {
  fs::path dir;
  std::unique_ptr<ProjectLink> link;
  std::unique_ptr<AudioOutput> out;
  ManualDevice* dev = nullptr;
  uint64_t callbacks = 0;
  explicit Rig(uint32_t outputs = 2) {
    dir = fs::temp_directory_path() / "pq_preview_service";
    std::error_code ec;
    fs::create_directories(dir, ec);
    LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    doc::IdSource ids;
    auto l = ProjectLink::create(c, doc::makeDefaultProject(ids));
    REQUIRE(l.ok());
    link = std::move(*l);
    DeviceFactory f;
    f.create = [this](engine::Backend) -> Result<std::unique_ptr<engine::AudioDevice>> {
      auto d = std::make_unique<ManualDevice>();
      dev = d.get();
      return std::unique_ptr<engine::AudioDevice>(std::move(d));
    };
    f.watchHardware = false;
    f.allowEnvDisable = false;
    OutputConfig oc;
    oc.bufferFrames = 128;
    oc.outputChannels = outputs;
    out = std::make_unique<AudioOutput>(link->engine(), oc, f);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (out->status().state != OutputStatus::State::Running && std::chrono::steady_clock::now() < end)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(out->status().state == OutputStatus::State::Running);
    REQUIRE(dev != nullptr);
  }
  ~Rig() {
    out.reset();
    link.reset();
  }
  std::string wav(const char* name, double seconds, float level = 0.5f) {
    const auto n = static_cast<size_t>(seconds * 48000.0);
    std::vector<std::vector<float>> ch(2, std::vector<float>(n, level));
    const fs::path p = dir / name;
    REQUIRE(media::writeWav(p, ch, 48000).ok());
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
  }
  // One device callback (RT-clean); returns the peak of output channel `ch`.
  float callback(uint32_t ch = 0) {
    CHECK(alloc_trap::countRtViolations([&] { dev->null().processBlock(); }) == 0);
    ++callbacks;
    float pk = 0;
    for (uint32_t i = 0; i < 128; ++i) pk = std::max(pk, std::fabs(dev->null().output(ch)[i]));
    return pk;
  }
};

}  // namespace

TEST_CASE("preview service: with the head loaded on selection, the next device callback carries the sample") {
  Rig r;
  PreviewService svc(r.link->samplePool(), r.link->engine().config().sampleRate, r.out.get());
  const std::string a = r.wav("a.wav", 1.0);
  svc.select(a);
  for (int i = 0; i < 1000 && !svc.loaded(a); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE(svc.loaded(a));
  CHECK(r.callback() == 0.0f);  // nothing plays yet
  CHECK_FALSE(svc.attached());   // no cue source while stopped (the output may suspend)
  const uint64_t before = r.callbacks;
  views::PreviewRequest req;
  req.path = a;
  svc.setVolumeDb(0.0f);
  svc.play(req);
  CHECK(svc.attached());
  const float first = r.callback();
  CHECK(r.callbacks == before + 1);
  CHECK(first > 0.1f);  // the very next callback (after the 2 ms declick's first frames)
  CHECK(svc.playing());
  // Stop: a 2 ms fade (within the next callback), then the cue source goes.
  svc.stop();
  (void)r.callback();
  CHECK(r.callback() == 0.0f);
  CHECK_FALSE(svc.playing());
  CHECK_FALSE(svc.attached());
}

TEST_CASE("preview service: a cold start (no selection first) and the cue pair") {
  Rig r(4);
  PreviewService svc(r.link->samplePool(), r.link->engine().config().sampleRate, r.out.get());
  const std::string b = r.wav("b.wav", 2.0);
  views::PreviewRequest req;
  req.path = b;
  const auto t0 = std::chrono::steady_clock::now();
  svc.play(req);  // no UI poster: loads here, then plays
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  MESSAGE("cold WAV start (2 s stereo, load + start): " << ms << " ms");
  CHECK(svc.playing());
  CHECK(r.callback(0) > 0.1f);
  // Cue Out 3/4: the preview leaves 1/2 (the master) alone.
  r.out->setCuePair(1);
  CHECK(r.callback(0) == 0.0f);
  CHECK(r.callback(2) > 0.1f);
  svc.stop();
  (void)r.callback(2);
  (void)svc.playing();
}

TEST_CASE("preview service: a cold WAV start within 30 ms" * doctest::test_suite("perf")) {
  Rig r;
  PreviewService svc(r.link->samplePool(), r.link->engine().config().sampleRate, r.out.get());
  double best = 1e9;
  for (int i = 0; i < 5; ++i) {
    const std::string p = r.wav(("cold" + std::to_string(i) + ".wav").c_str(), 3.0);
    views::PreviewRequest req;
    req.path = p;
    const auto t0 = std::chrono::steady_clock::now();
    svc.play(req);
    best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    svc.stop();
    (void)r.callback();
    (void)svc.playing();
  }
  MESSAGE("cold WAV start, best of 5: " << best << " ms");
  CHECK(best <= 30.0);
}

TEST_CASE("user settings: Cue Out and the browser's folders round-trip; the library's data stays out of %LOCALAPPDATA%") {
  const fs::path dir = fs::temp_directory_path() / "pq_settings_browser";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const std::string path = (dir / "settings.json").string();
  views::AudioSettings s;
  s.outputChannels = 4;
  s.cueOutput = 1;
  REQUIRE(saveAudioSettings(path, s).ok());
  REQUIRE(saveLibraryFolders(path, {"C:\\Samples", "D:\\Kits"}).ok());
  auto loaded = loadAudioSettings(path);
  REQUIRE(loaded.ok());
  CHECK(loaded->settings.cueOutput == 1);
  CHECK(loadLibraryFolders(path) == std::vector<std::string>{"C:\\Samples", "D:\\Kits"});
  // Under $PQ_SETTINGS_DIR (how the exe is tested) the index and tags live there, and no User Library is made.
  const std::string d = dir.string();
  _putenv_s("PQ_SETTINGS_DIR", d.c_str());
  const std::string lib = libraryDataDir();
  CHECK(lib.find(d) == 0);
  CHECK(userLibraryDir().empty());
  _putenv_s("PQ_SETTINGS_DIR", "");
  fs::remove_all(dir, ec);
}
