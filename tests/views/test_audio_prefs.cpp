// T3.18 preferences window (Audio tab) over a scripted AudioPrefsPort (FakeAudioPrefsPort::studio: an ASIO driver
// and WASAPI endpoints): Ctrl+, opens and closes it (disabled without a port); every chooser applies its choice at
// once (one apply per choice, the right AudioSettings); ASIO shows one device + Hardware Setup, WASAPI input and
// output devices; a device that is not connected stays visible; the readouts follow the port; a closed window leaves
// no timers (it costs nothing); the goldens audio-prefs / audio-prefs-wasapi.
#include <doctest/doctest.h>

#include <limits>
#include <memory>
#include <ostream>
#include <string>

#include "../ui/golden_check.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/prefs/audio_prefs.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "pq/views/view_scenes.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

struct Prefs {
  FakeTransport transport;
  ViewContext ctx{transport};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  FakeAudioPrefsPort port = FakeAudioPrefsPort::studio();

  explicit Prefs(bool withPort = true) {
    if (withPort) ctx.setAudioPrefs(&port);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
  }
  ~Prefs() { d.tree().setRoot(std::make_unique<Widget>()); }

  PreferencesWindow* window() const { return openPreferencesWindow(shell->overlay()); }
  PreferencesWindow& open() {
    d.press("Ctrl+,");
    d.step();
    PreferencesWindow* w = window();
    REQUIRE(w != nullptr);
    return *w;
  }
  // Picks item `i` of a dropdown through its menu, as a click would.
  void pick(Dropdown* dd, int32_t i) {
    REQUIRE(dd != nullptr);
    dd->openMenu();
    auto* menu = dynamic_cast<Menu*>(shell->overlay().top());
    REQUIRE(menu != nullptr);
    menu->choose(i);
    d.step();
  }
  int32_t itemIndex(Dropdown* dd, std::string_view text) {
    for (size_t i = 0; i < dd->items().size(); ++i)
      if (dd->items()[i] == text) return static_cast<int32_t>(i);
    FAIL("no item " << text);
    return -1;
  }
};

}  // namespace

TEST_CASE("prefs: Ctrl+, opens and closes the window; without a port the action is disabled") {
  {
    Prefs p;
    CHECK(p.window() == nullptr);
    PreferencesWindow& w = p.open();
    CHECK(w.shown() == p.port.current);
    p.d.press("Ctrl+,");  // again: closes it
    p.d.step();
    CHECK(p.window() == nullptr);
    p.open();
    p.d.press("Escape");
    p.d.step();
    CHECK(p.window() == nullptr);
  }
  {
    Prefs p(false);
    CHECK_FALSE(p.ctx.actions().enabled(p.ctx.actions().find("app.preferences")));
    p.d.press("Ctrl+,");
    p.d.step();
    CHECK(p.window() == nullptr);
  }
}

TEST_CASE("prefs: ASIO shows one device and Hardware Setup; WASAPI input and output devices") {
  Prefs p;
  PreferencesWindow& w = p.open();
  CHECK(w.find("body/audio/output/label") != nullptr);
  CHECK(static_cast<Label*>(w.find("body/audio/output/label"))->text() == "Audio Device");
  CHECK(w.find("body/audio/hardware")->visible());
  CHECK_FALSE(w.find("body/audio/input")->visible());
  CHECK(w.outputChooser()->items()[static_cast<size_t>(w.outputChooser()->index())] == "Focusrite USB ASIO");
  CHECK(w.rateChooser()->items()[static_cast<size_t>(w.rateChooser()->index())] == "48000");
  CHECK(w.bufferChooser()->items()[static_cast<size_t>(w.bufferChooser()->index())] == "64 Samples");
  CHECK(w.hardwareSetup()->enabled());
  w.hardwareSetup()->onClick();
  CHECK(p.port.panels == 1);
  // Driver type → WASAPI (exclusive): its first device (the system default), the input row, no Hardware Setup.
  p.pick(w.driverChooser(), p.itemIndex(w.driverChooser(), "WASAPI (exclusive)"));
  REQUIRE(p.port.applied.size() == 1);
  const AudioSettings& s = p.port.applied.back();
  CHECK(s.driver == AudioDriverType::WasapiExclusive);
  CHECK(s.outputDevice.empty());
  CHECK(s.sampleRate == 48000);
  CHECK(s.bufferFrames == 144);  // ASIO's 64 is below this device's exclusive minimum: the nearest valid size
  CHECK(static_cast<Label*>(w.find("body/audio/output/label"))->text() == "Audio Output Device");
  CHECK(w.find("body/audio/input")->visible());
  CHECK_FALSE(w.find("body/audio/hardware")->visible());
  CHECK_FALSE(w.inputChannelsChooser()->enabled());  // WASAPI input opens with recording (M6)
  CHECK(w.bufferChooser()->items().front() == "144 Samples");  // the exclusive-mode list of this device
}

TEST_CASE("prefs: each chooser applies exactly its choice, once") {
  Prefs p;
  PreferencesWindow& w = p.open();
  p.pick(w.outputChooser(), p.itemIndex(w.outputChooser(), "Voicemeeter Virtual ASIO"));
  REQUIRE(p.port.applied.size() == 1);
  CHECK(p.port.applied[0].outputDevice == "Voicemeeter Virtual ASIO");
  CHECK(p.port.applied[0].outputName == "Voicemeeter Virtual ASIO");
  p.pick(w.rateChooser(), p.itemIndex(w.rateChooser(), "96000"));
  REQUIRE(p.port.applied.size() == 2);
  CHECK(p.port.applied[1].sampleRate == 96000);
  CHECK(p.port.applied[1].outputDevice == "Voicemeeter Virtual ASIO");
  p.pick(w.bufferChooser(), p.itemIndex(w.bufferChooser(), "128 Samples"));
  REQUIRE(p.port.applied.size() == 3);
  CHECK(p.port.applied[2].bufferFrames == 128);
  CHECK(p.port.applied[2].sampleRate == 96000);
  p.pick(w.inputChannelsChooser(), p.itemIndex(w.inputChannelsChooser(), "No inputs"));
  REQUIRE(p.port.applied.size() == 4);
  CHECK(p.port.applied[3].inputChannels == 0);
  // Choosing what is already chosen applies nothing.
  p.pick(w.bufferChooser(), p.itemIndex(w.bufferChooser(), "128 Samples"));
  CHECK(p.port.applied.size() == 4);
  // The status line follows the port (FakeAudioPrefsPort reports the applied settings as running).
  CHECK(w.statusText().find("96000 Hz") != std::string::npos);
  CHECK(w.statusText().find("128 samples") != std::string::npos);
}

TEST_CASE("prefs: a device that is not connected stays visible; device lists refresh on the port's listener") {
  Prefs p;
  p.port.current.outputDevice = "Old Interface ASIO";
  p.port.current.outputName = "Old Interface ASIO";
  PreferencesWindow& w = p.open();
  const std::string shown = w.outputChooser()->items()[static_cast<size_t>(w.outputChooser()->index())];
  CHECK(shown == "Old Interface ASIO (not connected)");
  // Hot-plug: the interface arrives; the app's manager calls the listener.
  p.port.drivers[0].outputs.push_back({"Old Interface ASIO", "Old Interface ASIO", 2, 2, false});
  REQUIRE(p.port.listener);
  p.port.listener();
  CHECK(w.outputChooser()->items()[static_cast<size_t>(w.outputChooser()->index())] == "Old Interface ASIO");
}

TEST_CASE("prefs: test tone, latency and CPU readouts; the poll timer exists only while the window is open") {
  Prefs p;
  const double idleWake = p.d.tree().nextWakeTime();
  PreferencesWindow& w = p.open();
  CHECK(w.latencyText() == "1.65 ms  \xC2\xB7  3.94 ms  \xC2\xB7  5.58 ms");
  CHECK(w.cpuText() == "2.1 %");
  CHECK(w.statusText().find("Running") == 0);
  w.toneToggle()->setOn(true);  // as a click: the toggle flips, then reports it
  w.toneToggle()->onToggle(true);
  CHECK(p.port.tone);
  w.toneVolume()->setValue(-30.0);
  w.toneVolume()->onChange(-30.0);
  CHECK(p.port.toneDb == -30.0f);
  CHECK(p.port.toneHz == 440.0f);
  // Readouts poll at 2 Hz while open.
  CHECK(p.d.tree().nextWakeTime() <= p.d.now() + PreferencesWindow::kPollSeconds + 1e-9);
  p.port.load = 7.26;
  p.d.advance(0.6);
  CHECK(w.cpuText() == "7.3 %");
  p.port.running.state = AudioStatus::State::NoDevice;
  p.port.running.message = "Focusrite USB ASIO: not connected";
  ++p.port.running.revision;
  p.d.advance(0.6);
  CHECK(w.statusText() == "No audio device: the engine runs silently \xE2\x80\x94 Focusrite USB ASIO: not connected");
  CHECK(w.latencyText() == "\xE2\x80\x94");
  // Closed: no timer left (an idle DAW with closed preferences does no periodic work).
  p.d.press("Escape");
  p.d.step();
  p.d.step();
  CHECK(p.window() == nullptr);
  CHECK(p.d.tree().nextWakeTime() == idleWake);
  CHECK_FALSE(p.port.listener);  // the window unhooked itself
}

namespace {

void checkGolden(const char* scene) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, {}, w, h, {}, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &d));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(d));
}

}  // namespace

TEST_CASE("render: audio-prefs (ASIO, Focusrite-like device list) matches the golden image") { checkGolden("audio-prefs"); }

TEST_CASE("render: audio-prefs-wasapi (WASAPI exclusive, idle stream) matches the golden image") { checkGolden("audio-prefs-wasapi"); }
