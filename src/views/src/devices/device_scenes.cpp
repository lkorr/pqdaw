// The device panel scenes (T4.6; `device-utility`, `device-saturator`, `device-eq-eight`; T4.11
// `device-auto-filter`; T4.12 `device-operator`; T4.12-fix1 `device-operator-filter`): a stock device opened in the
// Device tab with a few
// non-default settings, and for EQ Eight a spectrum analysed
// (synchronously, deterministic) from a fixed test signal, so the goldens show every layer of the panel.

#include <cmath>
#include <complex>
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

#include "device_math.h"
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

// TX.spectrum: what the panel's device does to a signal, as its display's exact curve says (EQ Eight's bands, Auto
// Filter's unmodulated filter): |H(f)|.
std::function<double(double)> exactResponse(const DevicePanel& p, double sr) {
  if (p.signature().patch == "eq_eight") {
    std::vector<math::EqBand> bands(8);
    std::vector<math::EqCoef> coefs(8);
    const double scale = p.paramValue("scale"), os = p.constValue("hq") >= 1.5 ? 2.0 : 1.0;
    const bool adaptive = p.paramValue("adaptive") >= 0.5;
    for (int i = 0; i < 8; ++i) {
      const std::string n = std::to_string(i + 1);
      bands[i].on = p.constValue("on" + n) >= 0.5;
      bands[i].type = static_cast<int>(std::lround(p.constValue("type" + n)));
      bands[i].freq = p.paramValue("freq" + n);
      bands[i].gain = p.paramValue("gain" + n);
      bands[i].q = p.paramValue("q" + n);
      coefs[i] = math::eqCoef(bands[i], adaptive, scale, sr, os);
    }
    const double out = p.paramValue("outGain");
    return [bands, coefs, sr, os, out](double f) {
      std::complex<double> h = out;
      const double wf = math::eqWarp(f, sr, os);
      for (size_t b = 0; b < bands.size(); ++b) h *= math::eqBandResponseWarped(bands[b], coefs[b], wf);
      return std::abs(h);
    };
  }
  math::AutoFilterState s;
  s.type = static_cast<int>(std::lround(p.constValue("type")));
  s.circuit = static_cast<int>(std::lround(p.constValue("circuit")));
  s.slope = static_cast<int>(std::lround(p.constValue("slope")));
  s.freq = p.paramValue("freq");
  s.res = p.paramValue("res");
  s.morph = p.paramValue("morph");
  s.dj = p.paramValue("dj");
  return [s, sr](double f) { return std::abs(math::autoFilterResponse(s, f, sr)); };
}

}  // namespace

// The test signal through a zero-phase FIR whose magnitude is `mag` (frequency sampling, 2049 taps, Hann).
AnalysedFeed makeAnalysedFeed(const std::function<double(double)>& mag, bool response) {
  dsp::SpectrumConfig sc;
  sc.peakHold = true;
  constexpr int kHalf = 1024, kM = 8192;
  constexpr double kPi = 3.14159265358979323846;
  std::vector<double> H(kM / 2 + 1);
  for (int k = 0; k <= kM / 2; ++k) H[k] = mag(static_cast<double>(k) * sc.sampleRate / kM);
  std::vector<float> h(2 * kHalf + 1);
  for (int n = -kHalf; n <= kHalf; ++n) {
    double v = H[0] + H[kM / 2] * std::cos(kPi * n);
    for (int k = 1; k < kM / 2; ++k) v += 2.0 * H[k] * std::cos(2.0 * kPi * k * n / kM);
    const double w = 0.5 * (1.0 + std::cos(kPi * n / (kHalf + 1)));
    h[n + kHalf] = static_cast<float>(v / kM * w);
  }
  constexpr uint32_t kFrames = 98304;  // 2 s: the long (bass) window fills and averages
  const std::vector<float> x = testSignal(kFrames + 2 * kHalf, sc.sampleRate);
  std::vector<float> in(kFrames), out(kFrames);
  for (uint32_t i = 0; i < kFrames; ++i) {
    in[i] = x[i + kHalf];
    float acc = 0.0f;
    for (int j = 0; j <= 2 * kHalf; ++j) acc += h[j] * x[i + 2 * kHalf - j];
    out[i] = acc;
  }
  auto tap = std::make_unique<TapRing>(4, 32768, 4096);
  auto feed = std::make_unique<SpectrumFeed>(*tap, sc, SpectrumFeed::Manual{});
  const uint32_t wants = kWantOutput | (response ? kWantResponse : 0u);
  feed->acquire(wants);
  for (uint32_t i = 0; i < kFrames; i += 4096) {
    const float* ch[4] = {in.data() + i, in.data() + i, out.data() + i, out.data() + i};
    tap->write(ch, 4, std::min<uint32_t>(4096, kFrames - i));
    feed->step(1.0 + i / sc.sampleRate);
  }
  feed->hold();
  feed->release(wants);
  return AnalysedFeed{std::move(tap), std::move(feed)};
}

namespace {

void analysedSource(DevicePreview& pv, const std::function<double(double)>& mag, bool response) {
  AnalysedFeed a = makeAnalysedFeed(mag, response);
  pv.setSpectrumSource(std::move(a.tap), std::move(a.feed));
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
  } else if (patch == "auto_filter") {
    // MS2 at 24 dB with some drive and resonance; LFO (triangle) and envelope on, so every section is lit.
    panel->setConstValue("circuit", 2.0);
    panel->setConstValue("slope", 1.0);
    panel->setConstValue("lfo", 1.0);
    panel->setConstValue("lfoShape", 1.0);
    panel->setConstValue("env", 1.0);
    panel->setParamValue("freq", 1200.0);
    panel->setParamValue("res", 0.7);
    panel->setParamValue("drive", db(6.0));
    panel->setParamValue("lfoAmount", 0.3);
    panel->setParamValue("lfoRate", 0.5);
    panel->setParamValue("lfoPhase", 0.25);
    panel->setParamValue("envAmount", 0.25);
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
    // The output spectrum: the test signal through the bands, analysed now (a manual feed), then held. TX.spectrum: as a
    // project device's tap (input and output), so the "Measured" chip is offered (off).
    if (pv) {
      analysedSource(*pv, exactResponse(*panel, 48000.0), false);
      panel->wakeDisplays();
    }
  }
  // TX.spectrum: Auto Filter over its output spectrum, with the measured response shown (it sits on the exact curve:
  // the scene's signal goes through a linear filter of that response).
  if (patch == "auto_filter" && pv) {
    analysedSource(*pv, exactResponse(*panel, 48000.0), true);
    panel->showMeasuredResponse(true);
    panel->wakeDisplays();
  }
  shell.tree().layoutNow();
  return Ok{};
}

Status setupOperatorFilterScene(Shell& shell, std::string_view file) {
  if (Status s = setupDeviceScene(shell, file); !s.ok()) return s;
  DevicePanel* panel = openPanel(shell);
  if (panel == nullptr || panel->signature().patch != "operator")
    return Error{ErrorCode::InvalidArgument, "operator filter scene: not Operator", {}};
  // The filter on: MS2 at 24 dB with +9 dB drive, resonance 70 %, a decaying filter envelope.
  panel->setConstValue("filterOn", 1.0);
  panel->setConstValue("filterCircuit", 2.0);
  panel->setConstValue("filterSlope", 1.0);
  panel->setParamValue("filterDrive", db(9.0));
  panel->setParamValue("filterFreq", 900.0);
  panel->setParamValue("filterRes", 0.7);
  panel->setParamValue("fenvAmount", 3600.0);
  panel->setParamValue("fenvDecay", 0.35);
  panel->setParamValue("fenvSustain", 0.2);
  panel->select("sec", 2);  // Filter
  shell.tree().layoutNow();
  return Ok{};
}

}  // namespace pq::views::devices
