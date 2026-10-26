// The device panel scenes (T4.6; `device-utility`, `device-saturator`, `device-eq-eight`; T4.12 `device-operator`): a
// stock device opened in the Device tab with a few non-default settings, and for EQ Eight a spectrum analysed
// (synchronously, deterministic) from a fixed test signal, so the goldens show every layer of the panel.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "pq/views/devices/device_view.h"
#include "pq/views/shell/shell.h"

namespace pq::views::devices {

namespace {

double db(double v) { return std::pow(10.0, v / 20.0); }

// A music-like test signal: harmonics of 110 Hz falling with frequency over pink-ish noise (deterministic).
std::vector<float> testSignal(uint32_t n, double sr) {
  std::vector<float> x(n);
  uint32_t seed = 0x1234567u;
  float b0 = 0, b1 = 0, b2 = 0;
  constexpr double kPi = 3.14159265358979323846;
  for (uint32_t i = 0; i < n; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const float w = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
    // Paul Kellet's economy pink filter.
    b0 = 0.99765f * b0 + w * 0.0990460f;
    b1 = 0.96300f * b1 + w * 0.2965164f;
    b2 = 0.57000f * b2 + w * 1.0526913f;
    double v = 0.05 * (b0 + b1 + b2 + w * 0.1848f);
    for (int h = 1; h <= 24; ++h) v += 0.25 / h * std::sin(2.0 * kPi * 110.0 * h * i / sr + 0.7 * h);
    x[i] = static_cast<float>(0.5 * v);
  }
  return x;
}

}  // namespace

Status setupDeviceScene(Shell& shell, std::string_view file) {
  std::string err;
  DevicePanel* panel = openDevice(shell, std::string(file), &err);
  if (!panel) return Error{ErrorCode::NotFound, "device scene: " + err, {}};
  DevicePreview* pv = openPreview(shell);
  const std::string& patch = panel->signature().patch;
  if (patch == "utility") {
    panel->setParamValue("width", 1.2);
    panel->setParamValue("gain", db(-3.0));
    panel->setConstValue("bassMono", 1.0);
  } else if (patch == "saturator") {
    panel->setParamValue("drive", db(9.0));
    panel->setParamValue("output", db(-4.0));
    panel->setConstValue("type", 0.0);  // Analog Clip
    panel->setConstValue("color", 1.0);
  } else if (patch == "operator") {
    // T4.12: four operators in algorithm 3, A playing the User waveform (the partial editor shows), its envelope
    // looping; C a just fifth (3/2), D slightly detuned with feedback.
    panel->setConstValue("algorithm", 3.0);
    for (const char* on : {"onB", "onC", "onD"}) panel->setConstValue(on, 1.0);
    panel->setConstValue("waveA", 22.0);
    panel->setConstValue("loopA", 1.0);
    panel->setParamValue("levelB", db(-6.0));
    panel->setParamValue("levelC", db(-12.0));
    panel->setParamValue("levelD", db(-18.0));
    panel->setParamValue("coarseB", 2.0);
    panel->setParamValue("coarseC", 3.0);
    panel->setParamValue("denC", 2.0);
    panel->setParamValue("coarseD", 7.0);
    panel->setParamValue("fineD", 13.0);
    panel->setParamValue("feedback", 0.35);
    panel->setParamValue("attackA", 0.012);
    panel->setParamValue("decayA", 0.45);
    panel->setParamValue("sustainA", db(-9.0));
    panel->setParamValue("releaseA", 0.3);
    panel->setParamValue("repeatA", 0.2);
    const double partials[16] = {1.0, 0.5, 0.62, 0.25, 0.4, 0.12, 0.3, 0.08, 0.18, 0.05, 0.1, 0.0, 0.06, 0.0, 0.03, 0.0};
    for (int h = 0; h < 16; ++h) panel->setParamValue("uA" + std::to_string(h + 1), partials[h]);
  } else if (patch == "eq_eight") {
    panel->setConstValue("on1", 1.0);
    panel->setParamValue("freq1", 40.0);
    panel->setParamValue("gain2", db(4.0));
    panel->setParamValue("gain3", db(-3.0));
    panel->setParamValue("q3", 1.2);
    panel->setParamValue("gain4", db(6.0));
    panel->setParamValue("q4", 2.0);
    panel->setParamValue("freq5", 3200.0);
    panel->setParamValue("gain6", db(-5.0));
    panel->setConstValue("on8", 1.0);
    panel->setParamValue("freq8", 16000.0);
    // The output spectrum: the test signal analysed now (a manual feed), then held.
    if (pv) {
      dsp::SpectrumConfig sc;
      auto tap = std::make_unique<TapRing>(2, 32768, 2048);
      auto feed = std::make_unique<SpectrumFeed>(*tap, sc, SpectrumFeed::Manual{});
      tap->addReader();
      const std::vector<float> x = testSignal(16384, sc.sampleRate);
      const float* ch[2] = {x.data(), x.data()};
      tap->write(ch, 2, static_cast<uint32_t>(x.size()));
      feed->step(1.0);
      feed->hold();
      tap->removeReader();
      pv->setSpectrumSource(std::move(tap), std::move(feed));
      panel->wakeDisplays();
    }
  }
  shell.tree().layoutNow();
  return Ok{};
}

}  // namespace pq::views::devices
