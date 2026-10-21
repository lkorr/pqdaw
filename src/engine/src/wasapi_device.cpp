#include "pq/engine/wasapi_device.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <wrl/client.h>

#include "pq/core/aligned_buffer.h"
#include "pq/core/rt_log.h"
#include "pq/core/rt_scope.h"
#include "pq/engine/callback_runner.h"
#include "pq/engine/sample_convert.h"
#include "pq/engine/underrun_tracker.h"

namespace pq::engine {

using Microsoft::WRL::ComPtr;

namespace {

// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h needs the full <windows.h>, and its keys are
// declared extern without INITGUID).
constexpr PROPERTYKEY kFriendlyNameKey = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

constexpr uint32_t kProbeRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
constexpr SampleFormat kExclusiveOrder[] = {SampleFormat::Float32, SampleFormat::Int32, SampleFormat::Int24In32,
                                            SampleFormat::Int24, SampleFormat::Int16};

// COM for the calling thread, MTA. If the thread is already in an STA (RPC_E_CHANGED_MODE) the MMDevice API
// still works there, and we must not uninitialise what we did not initialise.
class ComScope {
public:
  ComScope() noexcept {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owns_ = SUCCEEDED(hr);
  }
  ~ComScope() {
    if (owns_) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;

private:
  bool owns_ = false;
};

std::string hrText(HRESULT hr) {
  const char* name = nullptr;
  switch (hr) {
    case AUDCLNT_E_DEVICE_IN_USE: name = "AUDCLNT_E_DEVICE_IN_USE (another application has the device in exclusive mode)"; break;
    case AUDCLNT_E_UNSUPPORTED_FORMAT: name = "AUDCLNT_E_UNSUPPORTED_FORMAT"; break;
    case AUDCLNT_E_INVALID_DEVICE_PERIOD: name = "AUDCLNT_E_INVALID_DEVICE_PERIOD"; break;
    case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED: name = "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED"; break;
    case AUDCLNT_E_BUFFER_SIZE_ERROR: name = "AUDCLNT_E_BUFFER_SIZE_ERROR"; break;
    case AUDCLNT_E_DEVICE_INVALIDATED: name = "AUDCLNT_E_DEVICE_INVALIDATED (device removed or reconfigured)"; break;
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: name = "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED (disabled in the device's properties)"; break;
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED: name = "AUDCLNT_E_ENDPOINT_CREATE_FAILED"; break;
    case AUDCLNT_E_SERVICE_NOT_RUNNING: name = "AUDCLNT_E_SERVICE_NOT_RUNNING"; break;
    case AUDCLNT_E_BUFFER_TOO_LARGE: name = "AUDCLNT_E_BUFFER_TOO_LARGE"; break;
    case AUDCLNT_E_ENGINE_PERIODICITY_LOCKED: name = "AUDCLNT_E_ENGINE_PERIODICITY_LOCKED"; break;
    case E_NOTFOUND: name = "E_NOTFOUND"; break;
    case E_INVALIDARG: name = "E_INVALIDARG"; break;
    case E_OUTOFMEMORY: name = "E_OUTOFMEMORY"; break;
    default: break;
  }
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
  return name ? std::string(name) + " (" + buf + ")" : std::string("HRESULT ") + buf;
}

Error comError(const std::string& what, HRESULT hr) { return Error{ErrorCode::Device, what + ": " + hrText(hr)}; }

std::string toUtf8(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring toWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

struct CoTaskString {
  LPWSTR p = nullptr;
  ~CoTaskString() { CoTaskMemFree(p); }
};

struct CoTaskFormat {
  WAVEFORMATEX* p = nullptr;
  ~CoTaskFormat() { CoTaskMemFree(p); }
};

std::string endpointIdOf(IMMDevice* dev) {
  CoTaskString id;
  if (FAILED(dev->GetId(&id.p))) return {};
  return toUtf8(id.p);
}

std::string friendlyName(IMMDevice* dev) {
  ComPtr<IPropertyStore> props;
  if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) return "(unknown)";
  PROPVARIANT v;
  PropVariantInit(&v);
  std::string name = "(unknown)";
  if (SUCCEEDED(props->GetValue(kFriendlyNameKey, &v)) && v.vt == VT_LPWSTR) name = toUtf8(v.pwszVal);
  PropVariantClear(&v);
  return name;
}

EDataFlow flowOf(IMMDevice* dev) {
  ComPtr<IMMEndpoint> ep;
  EDataFlow flow = eRender;
  if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&ep)))) ep->GetDataFlow(&flow);
  return flow;
}

WAVEFORMATEXTENSIBLE makeFormat(SampleFormat f, uint32_t rate, uint32_t channels) {
  WAVEFORMATEXTENSIBLE w{};
  const uint32_t bytes = sampleFormatBytes(f);
  w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  w.Format.nChannels = static_cast<WORD>(channels);
  w.Format.nSamplesPerSec = rate;
  w.Format.wBitsPerSample = static_cast<WORD>(8 * bytes);
  w.Format.nBlockAlign = static_cast<WORD>(channels * bytes);
  w.Format.nAvgBytesPerSec = rate * w.Format.nBlockAlign;
  w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  w.Samples.wValidBitsPerSample = static_cast<WORD>(f == SampleFormat::Int24In32 ? 24 : 8 * bytes);
  w.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                    : channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                    : 0;
  w.SubFormat = f == SampleFormat::Float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
  return w;
}

// The SampleFormat of a (mix) format, or false if it is none of ours.
bool formatOf(const WAVEFORMATEX* wf, SampleFormat& out) {
  bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
  bool isPcm = wf->wFormatTag == WAVE_FORMAT_PCM;
  uint32_t valid = wf->wBitsPerSample;
  if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
    const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
    isFloat = x->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    isPcm = x->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
    valid = x->Samples.wValidBitsPerSample ? x->Samples.wValidBitsPerSample : wf->wBitsPerSample;
  }
  if (isFloat && wf->wBitsPerSample == 32) out = SampleFormat::Float32;
  else if (isPcm && wf->wBitsPerSample == 32) out = valid == 24 ? SampleFormat::Int24In32 : SampleFormat::Int32;
  else if (isPcm && wf->wBitsPerSample == 24) out = SampleFormat::Int24;
  else if (isPcm && wf->wBitsPerSample == 16) out = SampleFormat::Int16;
  else return false;
  return true;
}

uint32_t hnsToFramesCeil(REFERENCE_TIME hns, uint32_t rate) {
  return static_cast<uint32_t>((static_cast<uint64_t>(hns) * rate + 9'999'999) / 10'000'000);
}

REFERENCE_TIME framesToHns(uint32_t frames, uint32_t rate) {
  return static_cast<REFERENCE_TIME>(std::llround(10'000'000.0 * frames / rate));
}

Result<ComPtr<IMMDeviceEnumerator>> makeEnumerator() {
  ComPtr<IMMDeviceEnumerator> en;
  const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
  if (FAILED(hr)) return comError("WASAPI: cannot create the device enumerator", hr);
  return en;
}

}  // namespace

uint64_t qpcNow100ns() noexcept {
  static const int64_t freq = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER c{};
  QueryPerformanceCounter(&c);
  return static_cast<uint64_t>(static_cast<double>(c.QuadPart) * (1e7 / static_cast<double>(freq)));
}

// ---------------------------------------------------------------------------------------------------------------
// WasapiDevice

struct WasapiDevice::Impl {
  // Control-thread state.
  bool comOwned = false;
  bool open = false;
  bool needsReset = false;  // the stream ran since open(): reset the client before the next start()
  ComPtr<IMMDevice> device;
  ComPtr<IAudioClient> client;
  ComPtr<IAudioRenderClient> render;
  ComPtr<IAudioClock> clock;
  HANDLE event = nullptr;
  std::thread thread;
  HANDLE startedEvent = nullptr;
  std::atomic<HRESULT> startHr{S_OK};
  mutable std::mutex errorMutex;  // the audio thread writes threadErrorText only after leaving its loop
  std::string threadErrorText;

  // Stream parameters (written by open(), read by the audio thread after start()).
  bool exclusive = false;
  uint32_t rate = 0;
  uint32_t period = 0;          // frames per callback
  uint32_t endpointFrames = 0;  // endpoint buffer capacity
  uint32_t numOut = 0;
  uint32_t deviceChannels = 0;
  uint32_t frameBytes = 0;
  SampleFormat format = SampleFormat::Float32;
  uint64_t clockFreq = 1;
  AlignedBuffer<float> scratch;
  float* out[kMaxChannels] = {};

  CallbackRunner runner;
  std::atomic<bool> running{false};
  std::atomic<bool> threadAlive{false};
  std::atomic<uint64_t> stallAfterWake{0};
  RtLog* rtLog = nullptr;  // optional diagnostics ring (set before start())

  void threadMain() noexcept;
  void renderPeriods(BYTE* data, uint32_t periods, bool& pendingUnderrun) noexcept;
  void setThreadError(std::string text) {
    std::lock_guard lock(errorMutex);
    threadErrorText = std::move(text);
  }
  void release() noexcept {
    clock.Reset();
    render.Reset();
    client.Reset();
    device.Reset();
    if (event) CloseHandle(event);
    event = nullptr;
    scratch = AlignedBuffer<float>();
    for (auto& p : out) p = nullptr;
    needsReset = false;
  }
};

WasapiDevice::WasapiDevice() : impl_(std::make_unique<Impl>()) {}

WasapiDevice::~WasapiDevice() { close(); }

bool WasapiDevice::isOpen() const noexcept { return impl_->open; }
bool WasapiDevice::isRunning() const noexcept { return impl_->threadAlive.load(std::memory_order_acquire); }
const CallbackStats& WasapiDevice::stats() const noexcept { return impl_->runner.stats(); }
void WasapiDevice::setRtLog(RtLog* log) noexcept { impl_->rtLog = log; }

void WasapiDevice::injectStallAfterWake(uint64_t wake) noexcept {
  impl_->stallAfterWake.store(wake, std::memory_order_relaxed);
}

std::string WasapiDevice::threadError() const {
  std::lock_guard lock(impl_->errorMutex);
  return impl_->threadErrorText;
}

Result<std::vector<DeviceInfo>> WasapiDevice::enumerate() {
  ComScope com;
  auto en = makeEnumerator();
  if (!en) return std::move(en).error();
  std::string defaultOut, defaultIn;
  {
    ComPtr<IMMDevice> d;
    if (SUCCEEDED((*en)->GetDefaultAudioEndpoint(eRender, eConsole, &d))) defaultOut = endpointIdOf(d.Get());
    d.Reset();
    if (SUCCEEDED((*en)->GetDefaultAudioEndpoint(eCapture, eConsole, &d))) defaultIn = endpointIdOf(d.Get());
  }
  ComPtr<IMMDeviceCollection> coll;
  HRESULT hr = (*en)->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &coll);
  if (FAILED(hr)) return comError("WASAPI: cannot enumerate endpoints", hr);
  UINT count = 0;
  coll->GetCount(&count);
  std::vector<DeviceInfo> out;
  out.reserve(count);
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> dev;
    if (FAILED(coll->Item(i, &dev))) continue;
    DeviceInfo d;
    d.id = i + 1;
    d.endpointId = endpointIdOf(dev.Get());
    d.name = friendlyName(dev.Get());
    const bool render = flowOf(dev.Get()) == eRender;
    d.isDefaultOutput = render && d.endpointId == defaultOut;
    d.isDefaultInput = !render && d.endpointId == defaultIn;
    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
      out.push_back(std::move(d));
      continue;
    }
    CoTaskFormat mix;
    if (SUCCEEDED(client->GetMixFormat(&mix.p))) {
      (render ? d.outputChannels : d.inputChannels) = mix.p->nChannels;
      d.preferredSampleRate = mix.p->nSamplesPerSec;
    }
    if (render && mix.p) {
      const uint32_t ch = mix.p->nChannels;
      for (uint32_t r : kProbeRates) {
        bool any = r == d.preferredSampleRate;  // shared mode always runs at the mix rate
        for (SampleFormat f : kExclusiveOrder) {
          WAVEFORMATEXTENSIBLE w = makeFormat(f, r, ch);
          if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr) == S_OK) {
            any = true;
            if (r == d.preferredSampleRate) d.exclusiveFormats.push_back(f);
          }
        }
        if (any) d.sampleRates.push_back(r);
      }
      REFERENCE_TIME defPeriod = 0, minPeriod = 0;
      if (SUCCEEDED(client->GetDevicePeriod(&defPeriod, &minPeriod)))
        d.minPeriodFrames = hnsToFramesCeil(minPeriod, d.preferredSampleRate);
      ComPtr<IAudioClient3> c3;
      if (SUCCEEDED(client.As(&c3))) {
        UINT32 def = 0, fund = 0, mn = 0, mx = 0;
        if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix.p, &def, &fund, &mn, &mx))) {
          d.sharedMinPeriodFrames = mn;
          d.sharedMaxPeriodFrames = mx;
        }
      }
    } else if (d.preferredSampleRate) {
      d.sampleRates.push_back(d.preferredSampleRate);
    }
    out.push_back(std::move(d));
  }
  return out;
}

Result<StreamInfo> WasapiDevice::open(const StreamConfig& config, AudioCallback cb) {
  Impl& m = *impl_;
  if (m.open) return Error{ErrorCode::InvalidArgument, "WasapiDevice: a stream is already open"};
  if (!cb) return Error{ErrorCode::InvalidArgument, "WasapiDevice: no callback"};
  if (config.outputChannels == 0 && config.inputChannels == 0)
    return Error{ErrorCode::InvalidArgument, "WasapiDevice: a stream needs output channels"};
  if (config.outputChannels > kMaxChannels || config.inputChannels > kMaxChannels)
    return Error{ErrorCode::InvalidArgument, "WasapiDevice: too many channels"};
  if (config.bufferFrames == 0) return Error{ErrorCode::InvalidArgument, "WasapiDevice: bufferFrames must be >= 1"};
  if (config.inputChannels > 0)
    return Error{ErrorCode::Unsupported,
                 "WasapiDevice: input streams are not implemented yet (output only); use the RtAudio WASAPI backend "
                 "(--backend rtaudio) for input"};

  const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  m.comOwned = SUCCEEDED(comHr);
  auto fail = [&](Error e) -> Result<StreamInfo> {
    m.release();
    if (m.comOwned) CoUninitialize();
    m.comOwned = false;
    return e;
  };

  auto en = makeEnumerator();
  if (!en) return fail(std::move(en).error());
  HRESULT hr = S_OK;
  if (!config.outputEndpointId.empty()) {
    hr = (*en)->GetDevice(toWide(config.outputEndpointId).c_str(), &m.device);
    if (FAILED(hr)) return fail(Error{ErrorCode::NotFound, "WasapiDevice: no endpoint with id " + config.outputEndpointId});
  } else if (config.outputDevice == kDefaultDevice) {
    hr = (*en)->GetDefaultAudioEndpoint(eRender, eConsole, &m.device);
    if (FAILED(hr)) return fail(Error{ErrorCode::NotFound, "WasapiDevice: no default output device"});
  } else {
    ComPtr<IMMDeviceCollection> coll;
    UINT count = 0;
    if (SUCCEEDED((*en)->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &coll))) coll->GetCount(&count);
    if (config.outputDevice == 0 || config.outputDevice > count || FAILED(coll->Item(config.outputDevice - 1, &m.device)))
      return fail(Error{ErrorCode::NotFound, "WasapiDevice: unknown device id (see --list-devices)"});
  }
  if (flowOf(m.device.Get()) != eRender)
    return fail(Error{ErrorCode::InvalidArgument, "WasapiDevice: not an output device"});

  hr = m.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m.client);
  if (FAILED(hr)) return fail(comError("WasapiDevice: cannot activate the audio client", hr));
  CoTaskFormat mix;
  hr = m.client->GetMixFormat(&mix.p);
  if (FAILED(hr)) return fail(comError("WasapiDevice: GetMixFormat", hr));

  const uint32_t rate = config.sampleRate ? config.sampleRate : mix.p->nSamplesPerSec;
  m.exclusive = config.shareMode == ShareMode::Exclusive;
  m.rate = rate;
  WAVEFORMATEXTENSIBLE fmt{};

  if (m.exclusive) {
    // Format: our order of preference, requested channel count first, then the device's.
    bool found = false;
    for (uint32_t ch : {config.outputChannels, static_cast<uint32_t>(mix.p->nChannels)}) {
      if (ch < config.outputChannels || found) continue;
      for (SampleFormat f : kExclusiveOrder) {
        fmt = makeFormat(f, rate, ch);
        if (m.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr) == S_OK) {
          m.format = f;
          m.deviceChannels = ch;
          found = true;
          break;
        }
      }
    }
    if (!found)
      return fail(Error{ErrorCode::Unsupported, "WasapiDevice: the device accepts none of float32/int32/24-in-32/"
                                                "int24/int16 in exclusive mode at " + std::to_string(rate) + " Hz"});
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    hr = m.client->GetDevicePeriod(&defPeriod, &minPeriod);
    if (FAILED(hr)) return fail(comError("WasapiDevice: GetDevicePeriod", hr));
    uint32_t frames = std::max(config.bufferFrames, hnsToFramesCeil(minPeriod, rate));
    REFERENCE_TIME period = framesToHns(frames, rate);
    if (period < minPeriod) period = minPeriod;
    hr = m.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                              &fmt.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
      // The driver wants another size: ask for exactly the aligned size it reports, on a fresh client.
      UINT32 aligned = 0;
      m.client->GetBufferSize(&aligned);
      m.client.Reset();
      hr = m.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m.client);
      if (FAILED(hr)) return fail(comError("WasapiDevice: cannot re-activate the audio client", hr));
      period = framesToHns(aligned, rate);
      hr = m.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                &fmt.Format, nullptr);
    }
    if (FAILED(hr))
      return fail(comError("WasapiDevice: exclusive Initialize (" + std::to_string(frames) + " frames, " +
                               sampleFormatName(m.format) + ")",
                           hr));
    UINT32 bufferFrames = 0;
    m.client->GetBufferSize(&bufferFrames);
    m.period = bufferFrames;
    m.endpointFrames = bufferFrames;
  } else {
    SampleFormat mixFormat{};
    if (!formatOf(mix.p, mixFormat))
      return fail(Error{ErrorCode::Unsupported, "WasapiDevice: the shared-mode mix format is not float32/int PCM"});
    m.deviceChannels = mix.p->nChannels;
    if (config.outputChannels > m.deviceChannels)
      return fail(Error{ErrorCode::InvalidArgument, "WasapiDevice: the device has only " +
                                                        std::to_string(m.deviceChannels) + " output channels"});
    ComPtr<IAudioClient3> c3;
    const bool atMixRate = rate == mix.p->nSamplesPerSec;
    if (atMixRate && SUCCEEDED(m.client.As(&c3))) {
      m.format = mixFormat;
      UINT32 def = 0, fund = 0, mn = 0, mx = 0;
      hr = c3->GetSharedModeEnginePeriod(mix.p, &def, &fund, &mn, &mx);
      if (FAILED(hr)) return fail(comError("WasapiDevice: GetSharedModeEnginePeriod", hr));
      uint32_t p = std::clamp<uint32_t>(config.bufferFrames, mn, mx);
      if (fund && p > mn) p = std::min<uint32_t>(mx, mn + (p - mn + fund - 1) / fund * fund);
      hr = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, p, mix.p, nullptr);
      if (FAILED(hr)) return fail(comError("WasapiDevice: InitializeSharedAudioStream", hr));
      WAVEFORMATEX* cur = nullptr;
      UINT32 curPeriod = p;
      if (SUCCEEDED(c3->GetCurrentSharedModeEnginePeriod(&cur, &curPeriod))) CoTaskMemFree(cur);
      m.period = curPeriod;
    } else {
      // Other rate (the Windows audio engine converts, in its own process) or no IAudioClient3.
      m.format = SampleFormat::Float32;
      fmt = makeFormat(SampleFormat::Float32, rate, m.deviceChannels);
      const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                          (atMixRate ? 0u : AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);
      hr = m.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, atMixRate ? mix.p : &fmt.Format, nullptr);
      if (FAILED(hr)) return fail(comError("WasapiDevice: shared Initialize", hr));
      REFERENCE_TIME defPeriod = 0, minPeriod = 0;
      m.client->GetDevicePeriod(&defPeriod, &minPeriod);
      m.period = hnsToFramesCeil(defPeriod, rate);
    }
    UINT32 bufferFrames = 0;
    m.client->GetBufferSize(&bufferFrames);
    m.endpointFrames = bufferFrames;
    if (m.period == 0 || m.period > bufferFrames)
      return fail(Error{ErrorCode::Device, "WasapiDevice: unexpected shared period " + std::to_string(m.period) +
                                               " for a " + std::to_string(bufferFrames) + "-frame buffer"});
  }

  m.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m.event) return fail(Error{ErrorCode::Device, "WasapiDevice: CreateEvent failed"});
  hr = m.client->SetEventHandle(m.event);
  if (FAILED(hr)) return fail(comError("WasapiDevice: SetEventHandle", hr));
  hr = m.client->GetService(IID_PPV_ARGS(&m.render));
  if (FAILED(hr)) return fail(comError("WasapiDevice: IAudioRenderClient", hr));
  hr = m.client->GetService(IID_PPV_ARGS(&m.clock));
  if (FAILED(hr)) return fail(comError("WasapiDevice: IAudioClock", hr));
  UINT64 freq = 0;
  m.clock->GetFrequency(&freq);
  m.clockFreq = freq ? freq : 1;
  REFERENCE_TIME streamLatency = 0;
  m.client->GetStreamLatency(&streamLatency);

  m.numOut = config.outputChannels;
  m.frameBytes = m.deviceChannels * sampleFormatBytes(m.format);
  const size_t stride = (static_cast<size_t>(m.period) + 15u) & ~size_t{15};
  m.scratch = AlignedBuffer<float>(stride * std::max<uint32_t>(m.numOut, 1));
  for (uint32_t c = 0; c < kMaxChannels; ++c) m.out[c] = c < m.numOut ? m.scratch.data() + stride * c : nullptr;
  m.runner.prepare(cb, 0, m.numOut, m.period, rate);
  m.open = true;

  StreamInfo info;
  info.backend = Backend::Wasapi;
  info.outputDeviceName = friendlyName(m.device.Get());
  info.outputEndpointId = endpointIdOf(m.device.Get());
  info.sampleRate = rate;
  info.bufferFrames = m.period;
  info.inputChannels = 0;
  info.outputChannels = m.numOut;
  info.shareMode = config.shareMode;
  info.deviceFormat = m.format;
  info.deviceChannels = m.deviceChannels;
  info.endpointBufferFrames = m.endpointFrames;
  // Frames from the start of a callback's write until the device clock reaches its first frame, as far as the
  // API reports it: exclusive = the period still playing + the driver's stream latency; shared = the endpoint
  // buffer we keep full + the engine's stream latency. The run-time measurement is CallbackStats' queue
  // (frames written − IAudioClock position) and pqdaw --latency-test (loopback).
  const uint32_t streamLatencyFrames = hnsToFramesCeil(streamLatency, rate);
  info.latencyFrames = (m.exclusive ? m.period : m.endpointFrames) + streamLatencyFrames;
  return info;
}

void WasapiDevice::Impl::renderPeriods(BYTE* data, uint32_t periods, bool& pendingUnderrun) noexcept {  // RT-safe
  for (uint32_t k = 0; k < periods; ++k) {
    runner.run(nullptr, out, period, false, pendingUnderrun);
    pendingUnderrun = false;
    planarToInterleaved(out, numOut, deviceChannels, period, format, data + static_cast<size_t>(k) * period * frameBytes);
  }
}

void WasapiDevice::Impl::threadMain() noexcept {
  ComScope com;
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
  bool pendingUnderrun = false;

  // Pre-fill: exclusive event mode wants one full buffer before Start; shared fills whole periods.
  UINT32 pad = 0;
  uint32_t prefill = period;
  if (!exclusive) {
    client->GetCurrentPadding(&pad);
    prefill = (endpointFrames - pad) / period * period;
  }
  int64_t written = 0;
  if (prefill) {
    BYTE* data = nullptr;
    HRESULT hr = render->GetBuffer(prefill, &data);
    if (SUCCEEDED(hr)) {
      renderPeriods(data, prefill / period, pendingUnderrun);
      hr = render->ReleaseBuffer(prefill, 0);
    }
    if (FAILED(hr)) {
      startHr.store(hr);
      SetEvent(startedEvent);
      if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
      threadAlive.store(false, std::memory_order_release);
      return;
    }
    written = prefill;
  }
  const HRESULT hrStart = client->Start();
  startHr.store(hrStart);
  SetEvent(startedEvent);  // start() may now return; nothing below touches startHr/startedEvent
  if (FAILED(hrStart)) {
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    threadAlive.store(false, std::memory_order_release);
    return;
  }

  LARGE_INTEGER qpcFreq{}, qpc0{};
  QueryPerformanceFrequency(&qpcFreq);
  QueryPerformanceCounter(&qpc0);
  const double framesPerTick = static_cast<double>(rate) / static_cast<double>(qpcFreq.QuadPart);
  const DWORD timeoutMs = std::max<DWORD>(20, static_cast<DWORD>(4000ull * period / rate));
  // Only for the stall test hook; created here, before the loop, so the loop itself never creates handles.
  HANDLE stallTimer = stallAfterWake.load(std::memory_order_relaxed)
                          ? CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)
                          : nullptr;
  UnderrunTracker tracker;
  tracker.reset({period, exclusive ? period : endpointFrames, !exclusive});
  HRESULT failure = S_OK;
  const char* failedCall = "";
  uint64_t wake = 0;
  bool restart = false;

  while (running.load(std::memory_order_acquire)) {
    const bool timedOut = WaitForSingleObject(event, timeoutMs) != WAIT_OBJECT_0;
    if (!running.load(std::memory_order_acquire)) break;
    ++wake;
    {
      RtScope rt;
      LARGE_INTEGER now{};
      QueryPerformanceCounter(&now);
      UnderrunTracker::Wake w;
      w.nowFrames = static_cast<int64_t>(static_cast<double>(now.QuadPart - qpc0.QuadPart) * framesPerTick);
      w.written = written;
      w.timedOut = timedOut;
      if (!exclusive) {
        if (HRESULT hr = client->GetCurrentPadding(&pad); FAILED(hr)) {
          failure = hr;
          failedCall = "GetCurrentPadding";
          break;
        }
        w.padding = static_cast<int32_t>(pad);
      }
      UINT64 pos = 0, posQpc = 0;
      if (SUCCEEDED(clock->GetPosition(&pos, &posQpc))) w.position = static_cast<int64_t>(pos * rate / clockFreq);
      const UnderrunTracker::Outcome o = tracker.onWake(w);
      runner.recordWake(o.latenessFrames, o.queueFrames, timedOut);
      pendingUnderrun = pendingUnderrun || o.newEpisode;
      // Restart only when the device holds nothing beyond what it is playing (position caught up with the frames
      // written): then it has lost its reserve. With data still queued the reserve is intact and a restart
      // would only add a period of latency.
      restart = exclusive && o.newEpisode && !timedOut && (w.position < 0 || w.position >= written);
      if (o.newEpisode && rtLog)
        rtLog->log(kLogUnderrun, "underrun at wake {}: {} frames late, queue {} frames, padding {}", wake,
                 o.latenessFrames, o.queueFrames, w.padding);

      const uint32_t frames = exclusive ? (timedOut ? 0 : period) : (endpointFrames - pad) / period * period;
      if (frames) {
        BYTE* data = nullptr;
        if (HRESULT hr = render->GetBuffer(frames, &data); FAILED(hr)) {
          failure = hr;
          failedCall = "GetBuffer";
          break;
        }
        renderPeriods(data, frames / period, pendingUnderrun);
        if (HRESULT hr = render->ReleaseBuffer(frames, 0); FAILED(hr)) {
          failure = hr;
          failedCall = "ReleaseBuffer";
          break;
        }
        written += frames;
        tracker.onWritten(frames);
      }
    }
    if (restart) {
      // Exclusive-mode recovery (measured on a Focusrite USB driver): after the device has run dry it no longer
      // holds a buffer in reserve, takes each new buffer just in time, and glitches again whenever an event
      // comes late (~3 % of the audio lost per second after one 30 ms stall). Stop, queue one extra period, and
      // restart restores the reserve the pre-fill gave at start (0.4 ms of driver calls). Only on this path,
      // after an underrun has already happened: the driver calls here are not realtime-clean.
      restart = false;
      client->Stop();
      BYTE* data = nullptr;
      if (SUCCEEDED(render->GetBuffer(period, &data))) {
        {
          RtScope rt;
          renderPeriods(data, 1, pendingUnderrun);
        }
        if (SUCCEEDED(render->ReleaseBuffer(period, 0))) {
          written += period;
          tracker.onWritten(period);
        }
      }
      if (HRESULT hr = client->Start(); FAILED(hr)) {
        failure = hr;
        failedCall = "Start (underrun recovery)";
        break;
      }
      runner.recordRestart();
      if (rtLog)
        rtLog->log(kLogRestart, "stream restarted after wake {} with one period ({} frames) in reserve", wake, period);
    }
    if (const uint64_t s = stallAfterWake.load(std::memory_order_relaxed); s != 0 && s == wake && stallTimer) {
      // Test hook (not part of normal operation): sleep for 3 periods on a high-resolution timer. A sleep, not a
      // spin: spinning for that long would also trip MMCSS's CPU-share throttling and degrade the stream after.
      LARGE_INTEGER due{};
      due.QuadPart = -static_cast<LONGLONG>(30'000'000.0 * period / rate);  // 100 ns units, relative
      if (SetWaitableTimer(stallTimer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(stallTimer, INFINITE);
    }
  }
  if (stallTimer) CloseHandle(stallTimer);
  client->Stop();
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  if (FAILED(failure)) setThreadError(std::string("WASAPI ") + failedCall + " failed: " + hrText(failure));
  threadAlive.store(false, std::memory_order_release);
}

Status WasapiDevice::start() {
  Impl& m = *impl_;
  if (!m.open) return Error{ErrorCode::InvalidArgument, "WasapiDevice: start() without an open stream"};
  if (m.threadAlive.load(std::memory_order_acquire) || m.thread.joinable())
    return Error{ErrorCode::InvalidArgument, "WasapiDevice: already started (stop() first)"};
  if (m.needsReset) {
    if (HRESULT hr = m.client->Reset(); FAILED(hr)) return comError("WasapiDevice: Reset before restart", hr);
    m.needsReset = false;
  }
  m.startedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!m.startedEvent) return Error{ErrorCode::Device, "WasapiDevice: CreateEvent failed"};
  m.setThreadError({});
  m.startHr.store(S_OK);
  m.running.store(true, std::memory_order_release);
  m.threadAlive.store(true, std::memory_order_release);
  try {
    m.thread = std::thread([&m] { m.threadMain(); });
  } catch (const std::system_error& e) {
    m.running.store(false, std::memory_order_release);
    m.threadAlive.store(false, std::memory_order_release);
    CloseHandle(m.startedEvent);
    m.startedEvent = nullptr;
    return Error{ErrorCode::Device, std::string("WasapiDevice: cannot start the audio thread: ") + e.what()};
  }
  const DWORD w = WaitForSingleObject(m.startedEvent, 5000);
  CloseHandle(m.startedEvent);
  m.startedEvent = nullptr;
  const HRESULT hr = m.startHr.load();
  if (w != WAIT_OBJECT_0 || FAILED(hr)) {
    m.running.store(false, std::memory_order_release);
    SetEvent(m.event);
    m.thread.join();
    if (w != WAIT_OBJECT_0) return Error{ErrorCode::Device, "WasapiDevice: the audio thread did not start in 5 s"};
    return comError("WasapiDevice: starting the stream", hr);
  }
  return Ok{};
}

Status WasapiDevice::stop() {
  Impl& m = *impl_;
  m.running.store(false, std::memory_order_release);
  if (m.event) SetEvent(m.event);
  if (m.thread.joinable()) m.thread.join();
  // A later start() resets the client (drops queued audio, rewinds the clock) first. Not here: Reset takes
  // ~350 ms on some drivers (Focusrite USB), and stop() should return promptly.
  m.needsReset = m.client != nullptr;
  const std::string err = threadError();
  if (!err.empty()) return Error{ErrorCode::Device, err};
  return Ok{};
}

void WasapiDevice::close() noexcept {
  if (!impl_) return;
  (void)stop();
  Impl& m = *impl_;
  m.release();
  if (m.comOwned) CoUninitialize();
  m.comOwned = false;
  m.open = false;
}

// ---------------------------------------------------------------------------------------------------------------
// LoopbackCapture

struct LoopbackCapture::Impl {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  bool comOwned = false;
  uint32_t rate = 0;
  uint32_t channels = 0;
  SampleFormat format = SampleFormat::Float32;
  std::vector<float> samples;
  std::vector<Packet> packets;
  std::thread thread;
  std::atomic<bool> running{false};
  std::atomic<HRESULT> error{S_OK};

  void poll() noexcept {
    ComScope com;
    while (running.load(std::memory_order_acquire)) {
      Sleep(1);
      UINT32 next = 0;
      while (SUCCEEDED(capture->GetNextPacketSize(&next)) && next > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 devPos = 0, qpc = 0;
        const HRESULT hr = capture->GetBuffer(&data, &frames, &flags, &devPos, &qpc);
        if (FAILED(hr)) {
          error.store(hr);
          return;
        }
        const size_t first = samples.size();
        if (first + frames <= samples.capacity() && packets.size() < packets.capacity()) {
          packets.push_back({first, frames, qpc, flags});
          for (UINT32 i = 0; i < frames; ++i) {
            float v = 0.0f;
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
              const BYTE* s = data + static_cast<size_t>(i) * channels * sampleFormatBytes(format);
              switch (format) {
                case SampleFormat::Float32: std::memcpy(&v, s, 4); break;
                case SampleFormat::Int16: {
                  int16_t x;
                  std::memcpy(&x, s, 2);
                  v = x / 32768.0f;
                  break;
                }
                default: {
                  int32_t x;
                  std::memcpy(&x, s, 4);
                  v = static_cast<float>(x / 2147483648.0);
                  break;
                }
              }
            }
            samples.push_back(v);
          }
        }
        capture->ReleaseBuffer(frames);
      }
    }
  }
};

LoopbackCapture::LoopbackCapture() : impl_(std::make_unique<Impl>()) {}
LoopbackCapture::~LoopbackCapture() { (void)stop(); }
uint32_t LoopbackCapture::sampleRate() const noexcept { return impl_->rate; }
const std::vector<float>& LoopbackCapture::samples() const noexcept { return impl_->samples; }
const std::vector<LoopbackCapture::Packet>& LoopbackCapture::packets() const noexcept { return impl_->packets; }

Status LoopbackCapture::start(const std::string& endpointId, double maxSeconds) {
  Impl& m = *impl_;
  if (m.running.load()) return Error{ErrorCode::InvalidArgument, "LoopbackCapture: already running"};
  const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  m.comOwned = SUCCEEDED(comHr);
  auto en = makeEnumerator();
  if (!en) return std::move(en).error();
  ComPtr<IMMDevice> dev;
  HRESULT hr = endpointId.empty() ? (*en)->GetDefaultAudioEndpoint(eRender, eConsole, &dev)
                                  : (*en)->GetDevice(toWide(endpointId).c_str(), &dev);
  if (FAILED(hr)) return comError("LoopbackCapture: endpoint", hr);
  hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m.client);
  if (FAILED(hr)) return comError("LoopbackCapture: Activate", hr);
  CoTaskFormat mix;
  hr = m.client->GetMixFormat(&mix.p);
  if (FAILED(hr)) return comError("LoopbackCapture: GetMixFormat", hr);
  if (!formatOf(mix.p, m.format) || m.format == SampleFormat::Int24)
    return Error{ErrorCode::Unsupported, "LoopbackCapture: unsupported mix format"};
  m.rate = mix.p->nSamplesPerSec;
  m.channels = mix.p->nChannels;
  hr = m.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2'000'000, 0, mix.p, nullptr);
  if (FAILED(hr)) return comError("LoopbackCapture: Initialize", hr);
  hr = m.client->GetService(IID_PPV_ARGS(&m.capture));
  if (FAILED(hr)) return comError("LoopbackCapture: IAudioCaptureClient", hr);
  m.samples.clear();
  m.packets.clear();
  m.samples.reserve(static_cast<size_t>(maxSeconds * m.rate) + 1);
  m.packets.reserve(static_cast<size_t>(maxSeconds * 1000.0) + 16);
  hr = m.client->Start();
  if (FAILED(hr)) return comError("LoopbackCapture: Start", hr);
  m.running.store(true, std::memory_order_release);
  m.thread = std::thread([&m] { m.poll(); });
  return Ok{};
}

Status LoopbackCapture::stop() {
  Impl& m = *impl_;
  m.running.store(false, std::memory_order_release);
  if (m.thread.joinable()) m.thread.join();
  if (m.client) m.client->Stop();
  m.capture.Reset();
  m.client.Reset();
  if (m.comOwned) CoUninitialize();
  m.comOwned = false;
  if (FAILED(m.error.load())) return comError("LoopbackCapture: GetBuffer", m.error.load());
  return Ok{};
}

}  // namespace pq::engine
