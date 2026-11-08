#pragma once

// AU.stream: a probe for what reaches the default render endpoint (tests only; real hardware, opt-in): a WASAPI
// loopback capture (float, before the DAC) and a tracker for a quiet test tone's amplitude and phase in 128-sample
// windows (an integer number of cycles: 1500 Hz at 48 kHz), so a dropped / repeated / zeroed stretch shows as an
// amplitude dip or a phase step at its time; loopback packets the audio engine flags (discontinuity, silent) are
// timed against the start. Used by tests/engine/test_asio.cpp and tests/app/test_audio_manager.cpp.

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace pq::test {


struct ProbeCapture {
  struct Packet {
    uint64_t devPos, qpc;  // qpc: 100 ns units
    uint32_t frames, flags;
    size_t at;             // index of its first frame in `mono`
  };
  std::vector<Packet> packets;
  std::vector<float> mono;  // channel 1, concatenated in arrival order
  std::string error;
  uint32_t rate = 0, channels = 0;
  uint64_t startQpc = 0;  // 100 ns units, when atStart ran

  // Captures for `ms` on this thread (polled every 2 ms); atStart runs once the capture is running.
  void run(int ms, const std::function<void()>& atStart) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(hr);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioCaptureClient* cc = nullptr;
    WAVEFORMATEX* wf = nullptr;
    auto done = [&] {
      if (cc) cc->Release();
      if (ac) {
        ac->Stop();
        ac->Release();
      }
      if (wf) CoTaskMemFree(wf);
      if (dev) dev->Release();
      if (en) en->Release();
      if (uninit) CoUninitialize();
    };
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&en));
    if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac));
    if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&wf);
    if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, wf, nullptr);
    if (SUCCEEDED(hr)) hr = ac->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cc));
    if (SUCCEEDED(hr)) hr = ac->Start();
    if (FAILED(hr)) {
      error = "loopback setup failed: " + std::to_string(hr);
      done();
      return;
    }
    rate = wf->nSamplesPerSec;
    channels = wf->nChannels;
    const bool isFloat = wf->wBitsPerSample == 32;  // the shared mix format is float32 (extensible)
    LARGE_INTEGER q, fq;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&fq);
    startQpc = static_cast<uint64_t>(static_cast<double>(q.QuadPart) * 1e7 / static_cast<double>(fq.QuadPart));
    if (atStart) atStart();
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      UINT32 n = 0;
      while (SUCCEEDED(cc->GetNextPacketSize(&n)) && n > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 pos = 0, qpc = 0;
        if (FAILED(cc->GetBuffer(&data, &frames, &flags, &pos, &qpc))) break;
        packets.push_back({pos, qpc, frames, flags, mono.size()});
        for (UINT32 i = 0; i < frames; ++i)
          mono.push_back((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !isFloat ? 0.0f
                                                                          : reinterpret_cast<const float*>(data)[i * channels]);
        cc->ReleaseBuffer(frames);
      }
      Sleep(2);
    }
    done();
  }
};

// Amplitude / phase track of a tone at hz (an integer number of cycles per 128 samples) from its first sample.
// `times` (if given) receives each anomalous window's time in ms after the tone's first sample; `toneAtMs` its arrival
// after the start (-1 if it never arrived).
inline std::string analyseTone(const ProbeCapture& cap, double hz, float amp, int* anomaliesOut,
                               std::vector<double>* times = nullptr, double* toneAtMs = nullptr) {
  std::string s;
  char line[240];
  size_t first = 0;
  while (first < cap.mono.size() && std::fabs(cap.mono[first]) < amp * 0.3f) ++first;
  if (toneAtMs) *toneAtMs = -1;
  if (first == cap.mono.size()) return "the tone never arrived on the endpoint\n";
  constexpr size_t W = 128;
  const double kTwoPi = 6.283185307179586;
  const double w = kTwoPi * hz / cap.rate;
  // The tone's first sample, against the driver start (the loopback packet holding it).
  double firstMs = -1;
  for (const auto& p : cap.packets)
    if (first >= p.at && first < p.at + p.frames)
      firstMs = (static_cast<double>(p.qpc - cap.startQpc) / 1e4) + 1000.0 * static_cast<double>(first - p.at) / cap.rate;
  if (toneAtMs) *toneAtMs = firstMs;
  std::snprintf(line, sizeof line, "the tone reaches the endpoint %.1f ms after the driver start\n", firstMs);
  s += line;
  double prevPhase = 0;
  bool have = false;
  int anomalies = 0;
  // Skip the onset (3 windows, 8 ms: it straddles silence and the tone, and the app's 5 ms ramp in); the last window
  // of the capture may be partial.
  for (size_t k = first + 3 * W; k + W <= cap.mono.size(); k += W) {
    double c = 0, si = 0;
    for (size_t i = 0; i < W; ++i) {
      const double ph = w * static_cast<double>(k - first + i);
      c += cap.mono[k + i] * std::cos(ph);
      si += cap.mono[k + i] * std::sin(ph);
    }
    const double a = 2.0 * std::sqrt(c * c + si * si) / W;
    const double phase = std::atan2(c, si);  // 0 for an undisturbed tone that started at phase 0 on `first`
    const double dPhase = have ? std::remainder(phase - prevPhase, kTwoPi) : 0.0;
    if (!have) prevPhase = phase;
    const bool bad = (have && std::fabs(dPhase) > 0.02) || a < amp * 0.9 || a > amp * 1.1;
    if (bad && anomalies < 60) {
      std::snprintf(line, sizeof line,
                    "  %8.2f ms into the tone (%.1f ms after the driver start): amplitude %.3g (want %.3g), phase step "
                    "%+.3f rad (%+.1f samples)\n",
                    1000.0 * static_cast<double>(k - first) / cap.rate,
                    firstMs + 1000.0 * static_cast<double>(k - first) / cap.rate, a, static_cast<double>(amp), dPhase,
                    dPhase / w);
      s += line;
    }
    anomalies += bad;
    if (bad && times) times->push_back(1000.0 * static_cast<double>(k - first) / cap.rate);
    if (have) prevPhase = phase;
    have = true;
  }
  std::snprintf(line, sizeof line, "%d anomalous windows of %zu\n", anomalies, (cap.mono.size() - first) / W);
  s += line;
  if (anomaliesOut) *anomaliesOut = anomalies;
  for (const auto& p : cap.packets)
    if (p.flags & (AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY | AUDCLNT_BUFFERFLAGS_SILENT)) {
      std::snprintf(line, sizeof line, "  loopback packet %s%s, %u frames, %.1f ms after the driver start (device position %llu)\n",
                    p.flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY ? "DISCONTINUITY " : "",
                    p.flags & AUDCLNT_BUFFERFLAGS_SILENT ? "SILENT" : "", p.frames,
                    static_cast<double>(p.qpc - cap.startQpc) / 1e4, static_cast<unsigned long long>(p.devPos));
      s += line;
    }
  return s;
}

// The default endpoint plays nothing else for 0.8 s (anything else would spoil the analysis, and is not ours to
// record). peak: the largest sample seen.
inline bool endpointQuiet(float* peak, std::string* error) {
  ProbeCapture quiet;
  quiet.run(800, nullptr);
  if (error) *error = quiet.error;
  float pk = 0;
  for (float x : quiet.mono) pk = std::max(pk, std::fabs(x));
  if (peak) *peak = pk;
  return quiet.error.empty() && pk <= 1e-6f;
}

}  // namespace pq::test
