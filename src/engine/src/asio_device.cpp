#include "pq/engine/asio_device.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include <windows.h>

#include <objbase.h>

#include "pq/core/aligned_buffer.h"
#include "pq/core/rt_scope.h"
#include "pq/engine/asio_abi.h"
#include "pq/engine/asio_convert.h"
#include "pq/engine/callback_runner.h"
#include "pq/engine/underrun_tracker.h"

namespace pq::engine {

namespace {

constexpr uint32_t kProbeRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
constexpr DWORD kPollMs = 50;  // request polling while the stream runs
// ... and while a driver is loaded but stopped (T3.18: the DAW keeps the driver loaded while its idle stream is
// suspended; 20 wakes/s cost an idle DAW ~0.05 % of a core, 4/s a fifth of that; a request then waits ≤ 250 ms).
constexpr DWORD kIdlePollMs = 250;

std::string narrow(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return {};
  std::wstring w(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

std::string hex(long v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(v));
  return buf;
}

const char* statusText(asio::Status s) noexcept {
  switch (s) {
    case asio::kOk: return "OK";
    case asio::kSuccess: return "success";
    case asio::kNotPresent: return "not present (hardware missing or in use)";
    case asio::kHwMalfunction: return "hardware malfunction";
    case asio::kInvalidParameter: return "invalid parameter";
    case asio::kInvalidMode: return "invalid mode";
    case asio::kSpNotAdvancing: return "sample position not advancing";
    case asio::kNoClock: return "no clock";
    case asio::kNoMemory: return "out of memory";
    default: return "unknown error";
  }
}

bool iequals(const std::string& a, const std::string& b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

// Request bits set by the driver's messages (any thread) and handled on the control thread.
constexpr uint32_t kReqReset = 1u << 0;
constexpr uint32_t kReqLatencies = 1u << 1;
constexpr uint32_t kReqRate = 1u << 2;

// Process-wide host state: the ASIO callbacks have no context pointer, and only one driver can be loaded.
struct HostState {
  std::atomic<AsioDevice::Impl*> active{nullptr};  // the device whose buffers exist (set around createBuffers)
  std::atomic<bool> driverLoaded{false};
  std::atomic<uint32_t> requests{0};
  std::atomic<uint64_t> newRateBits{0};
  std::atomic<uint64_t> resetRequests{0}, bufferSizeChanges{0}, resyncRequests{0}, latencyChanges{0}, rateChanges{0},
      overloads{0};
};
HostState g_host;

// --- Control thread: STA + hidden window + message pump + a job queue --------------------------------------------

class StaThread {
public:
  // poll() runs on the thread after every wake; it returns the wait timeout for the next one (ms or INFINITE).
  explicit StaThread(std::function<DWORD()> poll) : poll_(std::move(poll)) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
    std::unique_lock lock(m_);
    cv_.wait(lock, [this] { return ready_; });
  }
  ~StaThread() {
    {
      std::lock_guard lock(m_);
      quit_ = true;
    }
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
  }
  StaThread(const StaThread&) = delete;
  StaThread& operator=(const StaThread&) = delete;

  bool onThread() const noexcept { return GetCurrentThreadId() == tid_; }
  HWND window() const noexcept { return hwnd_; }
  bool comReady() const noexcept { return comOk_; }

  // Runs fn on the control thread and returns when it has finished (inline if already there).
  void call(const std::function<void()>& fn) {
    if (onThread()) {
      fn();
      return;
    }
    Job job{&fn};
    {
      std::lock_guard lock(m_);
      jobs_.push_back(&job);
    }
    SetEvent(wake_);
    std::unique_lock lock(m_);
    cv_.wait(lock, [&] { return job.done; });
  }

private:
  struct Job {
    const std::function<void()>* fn;
    bool done = false;
  };

  void run() {
    tid_ = GetCurrentThreadId();
    comOk_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"pqdaw.asio.host";
    RegisterClassW(&wc);  // fails harmlessly if another device registered it already
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"pqdaw ASIO host", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, nullptr, nullptr,
                            inst, nullptr);
    {
      std::lock_guard lock(m_);
      ready_ = true;
    }
    cv_.notify_all();
    DWORD timeout = INFINITE;
    for (;;) {
      MsgWaitForMultipleObjects(1, &wake_, FALSE, timeout, QS_ALLINPUT);
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      for (;;) {
        Job* job = nullptr;
        {
          std::lock_guard lock(m_);
          if (jobs_.empty()) break;
          job = jobs_.front();
          jobs_.pop_front();
        }
        (*job->fn)();
        {
          std::lock_guard lock(m_);
          job->done = true;
        }
        cv_.notify_all();
      }
      {
        std::lock_guard lock(m_);
        if (quit_ && jobs_.empty()) break;
      }
      timeout = poll_();
    }
    if (hwnd_) DestroyWindow(hwnd_);
    if (comOk_) CoUninitialize();
  }

  std::function<DWORD()> poll_;
  std::thread thread_;
  HANDLE wake_ = nullptr;
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<Job*> jobs_;
  bool ready_ = false;
  bool quit_ = false;
  DWORD tid_ = 0;
  HWND hwnd_ = nullptr;
  bool comOk_ = false;
};

}  // namespace

uint32_t asioChooseBufferFrames(int32_t minSize, int32_t maxSize, int32_t preferred, int32_t granularity,
                                uint32_t requested) noexcept {
  const uint32_t pref = preferred > 0 ? static_cast<uint32_t>(preferred) : 0;
  if (minSize <= 0 || maxSize < minSize) return pref ? pref : (maxSize > 0 ? static_cast<uint32_t>(maxSize) : 256u);
  const uint32_t mn = static_cast<uint32_t>(minSize), mx = static_cast<uint32_t>(maxSize);
  const uint32_t prefIn = std::clamp(pref ? pref : mn, mn, mx);
  if (requested == 0) return prefIn;
  if (mn == mx) return mn;
  if (granularity == -1) {
    uint32_t best = 0, largest = 0;
    for (uint32_t p = 1; p != 0 && p <= mx; p <<= 1) {
      if (p < mn) continue;
      largest = p;
      if (!best && p >= requested) best = p;
    }
    return best ? best : largest ? largest : prefIn;
  }
  if (granularity <= 0) return prefIn;
  const uint32_t g = static_cast<uint32_t>(granularity);
  const uint32_t top = mn + (mx - mn) / g * g;  // largest valid size
  if (requested <= mn) return mn;
  const uint64_t up = mn + (static_cast<uint64_t>(requested - mn) + g - 1) / g * g;
  return up <= top ? static_cast<uint32_t>(up) : top;
}

AsioDriverSource AsioDriverSource::registry() {
  AsioDriverSource s;
  s.list = [] {
    std::vector<AsioDriverEntry> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
      wchar_t name[256];
      DWORD len = 256;
      const LSTATUS r = RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr);
      if (r == ERROR_NO_MORE_ITEMS) break;
      if (r != ERROR_SUCCESS) continue;
      wchar_t clsid[128];
      DWORD bytes = sizeof clsid;
      if (RegGetValueW(root, name, L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &bytes) != ERROR_SUCCESS) continue;
      out.push_back({narrow(name), narrow(clsid)});
    }
    RegCloseKey(root);
    return out;
  };
  s.create = [](const AsioDriverEntry& e) -> Result<asio::Driver*> {
    CLSID clsid{};
    const std::wstring w = widen(e.clsid);
    if (FAILED(CLSIDFromString(w.c_str(), &clsid)))
      return Error{ErrorCode::Device, "ASIO driver '" + e.name + "': bad CLSID '" + e.clsid + "' in the registry"};
    void* p = nullptr;
    // ASIO drivers use their class id as the interface id.
    const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, &p);
    if (FAILED(hr) || !p)
      return Error{ErrorCode::Device, "ASIO driver '" + e.name + "' could not be created (CoCreateInstance " +
                                          hex(hr) + "; driver not installed for 64-bit?)"};
    return static_cast<asio::Driver*>(p);
  };
  return s;
}

// --- The device -------------------------------------------------------------------------------------------------

struct AsioDevice::Impl {
  AsioDriverSource source;
  std::unique_ptr<StaThread> sta;

  // Control-thread state.
  asio::Driver* driver = nullptr;  // loaded driver (with our one reference)
  AsioDriverEntry entry;           // the loaded driver's registry entry
  StreamConfig config;
  AudioCallback cb{};
  std::vector<asio::BufferInfo> bufferInfos;
  bool buffersCreated = false;
  StreamChangedFn changedFn = nullptr;
  void* changedUser = nullptr;
  std::atomic<bool> open{false};
  std::atomic<bool> running{false};

  mutable std::mutex infoMutex;  // info, details, lastError: written on the control thread, read anywhere
  StreamInfo info;
  AsioStreamDetails details;
  std::string lastError;

  // Audio-path state, set up before createBuffers and read by the driver's audio thread.
  CallbackRunner runner;
  uint32_t numIn = 0, numOut = 0, frames = 0;
  ToDeviceFn toDevice[kMaxChannels] = {};
  FromDeviceFn fromDevice[kMaxChannels] = {};
  void* devOut[2][kMaxChannels] = {};
  const void* devIn[2][kMaxChannels] = {};
  float* outPtr[2][kMaxChannels] = {};        // what the callback writes: device buffer (float32) or scratch
  const float* inPtr[2][kMaxChannels] = {};
  float* scratchOut[kMaxChannels] = {};
  float* scratchIn[kMaxChannels] = {};
  uint32_t convertOut[kMaxChannels] = {};     // output channels that need a conversion pass
  uint32_t convertIn[kMaxChannels] = {};
  uint32_t numConvertOut = 0, numConvertIn = 0;
  uint32_t outBytes[kMaxChannels] = {};
  AlignedBuffer<float> scratch;
  bool postOutput = false;
  std::atomic<bool> streamActive{false};
  std::atomic<uint32_t> inCallback{0};
  int64_t lastPos = 0;
  bool havePos = false;
  // Lateness (M0 review L2): each buffer switch is a wake against the driver's sample clock. X = TSC time in frames
  // minus the switch's sample position (frames delivered when the driver reports none); UnderrunTracker keeps the
  // baseline (the minimum X), and lateness = X − baseline goes to CallbackStats::recordWake. Underruns are still
  // counted from position jumps only (`missed`); the tracker's own underrun flags are not used.
  UnderrunTracker lateness;
  double framesPerTick = 0;  // sample rate / TSC rate, set in startDriver()
  uint64_t tsc0 = 0;         // TSC at the first switch after a start (0: none yet)
  int64_t delivered = 0;     // frames handed to the driver since the start

  // --- audio thread ---

  void process(int32_t index, const asio::Time* t) noexcept {  // RT-safe
    // seq_cst pairs with stopDriver(): either it sees this callback in flight, or this callback sees the stop.
    inCallback.fetch_add(1, std::memory_order_seq_cst);
    if (streamActive.load(std::memory_order_seq_cst)) {
      RtScope rt;
      const uint32_t h = static_cast<uint32_t>(index) & 1u;
      int64_t pos = 0;
      bool posValid = false;
      if (t) {
        if (t->timeInfo.flags & asio::kSamplePositionValid) {
          pos = asio::toInt64(t->timeInfo.samplePosition);
          posValid = true;
        }
      } else {
        asio::Int64Halves p, ts;
        if (driver->getSamplePosition(&p, &ts) == asio::kOk) {
          pos = asio::toInt64(p);
          posValid = true;
        }
      }
      bool missed = false;
      if (posValid) {
        missed = havePos && pos - lastPos >= static_cast<int64_t>(frames) + frames / 2;
        lastPos = pos;
        havePos = true;
      }
      {
        const uint64_t now = readTsc();
        if (tsc0 == 0) tsc0 = now;
        UnderrunTracker::Wake w;
        w.nowFrames = static_cast<int64_t>(static_cast<double>(now - tsc0) * framesPerTick);
        w.written = posValid ? pos : delivered;
        runner.recordWake(lateness.onWake(w).latenessFrames, -1, false);
        delivered += frames;
      }
      for (uint32_t k = 0; k < numConvertIn; ++k) {
        const uint32_t c = convertIn[k];
        fromDevice[c](devIn[h][c], frames, scratchIn[c]);
      }
      runner.run(inPtr[h], outPtr[h], frames, false, missed);
      for (uint32_t k = 0; k < numConvertOut; ++k) {
        const uint32_t c = convertOut[k];
        toDevice[c](scratchOut[c], frames, devOut[h][c]);
      }
      if (postOutput) driver->outputReady();
    }
    inCallback.fetch_sub(1, std::memory_order_release);
  }

  static void onBufferSwitch(int32_t index, asio::Bool) noexcept {  // RT-safe
    if (Impl* s = g_host.active.load(std::memory_order_acquire)) s->process(index, nullptr);
  }
  static asio::Time* onBufferSwitchTimeInfo(asio::Time* params, int32_t index, asio::Bool) noexcept {  // RT-safe
    if (Impl* s = g_host.active.load(std::memory_order_acquire)) s->process(index, params);
    return nullptr;
  }
  static void onRateChanged(double rate) noexcept {  // RT-safe
    g_host.newRateBits.store(std::bit_cast<uint64_t>(rate), std::memory_order_relaxed);
    g_host.rateChanges.fetch_add(1, std::memory_order_relaxed);
    g_host.requests.fetch_or(kReqRate, std::memory_order_release);
  }
  static int32_t onMessage(int32_t selector, int32_t value, void*, double*) noexcept {  // RT-safe
    switch (selector) {
      case asio::kMsgSelectorSupported:
        return value == asio::kMsgEngineVersion || value == asio::kMsgResetRequest ||
                       value == asio::kMsgBufferSizeChange || value == asio::kMsgResyncRequest ||
                       value == asio::kMsgLatenciesChanged || value == asio::kMsgSupportsTimeInfo ||
                       value == asio::kMsgOverload
                   ? 1
                   : 0;
      case asio::kMsgEngineVersion: return 2;
      case asio::kMsgResetRequest:
        g_host.resetRequests.fetch_add(1, std::memory_order_relaxed);
        g_host.requests.fetch_or(kReqReset, std::memory_order_release);
        return 1;
      case asio::kMsgBufferSizeChange:  // handled as a reset: the stream is recreated at a size the driver accepts
        g_host.bufferSizeChanges.fetch_add(1, std::memory_order_relaxed);
        g_host.requests.fetch_or(kReqReset, std::memory_order_release);
        return 1;
      case asio::kMsgResyncRequest: g_host.resyncRequests.fetch_add(1, std::memory_order_relaxed); return 1;
      case asio::kMsgLatenciesChanged:
        g_host.latencyChanges.fetch_add(1, std::memory_order_relaxed);
        g_host.requests.fetch_or(kReqLatencies, std::memory_order_release);
        return 1;
      case asio::kMsgSupportsTimeInfo: return 1;
      case asio::kMsgOverload: g_host.overloads.fetch_add(1, std::memory_order_relaxed); return 1;
      default: return 0;
    }
  }

  static const asio::HostCallbacks kHostCallbacks;  // must stay valid while the driver's buffers exist

  // --- control thread ---

  Result<asio::Driver*> load(const AsioDriverEntry& e) {
    if (!sta->comReady()) return Error{ErrorCode::Device, "ASIO: COM could not be initialised on the control thread"};
    bool expected = false;
    if (!g_host.driverLoaded.compare_exchange_strong(expected, true))
      return Error{ErrorCode::Device, "ASIO driver '" + e.name +
                                          "' not loaded: another ASIO driver is loaded in this process (ASIO allows one)"};
    auto created = source.create(e);
    if (!created) {
      g_host.driverLoaded.store(false);
      return std::move(created).error();
    }
    asio::Driver* d = *created;
    if (!d->init(sta->window())) {
      char msg[256] = {};
      d->getErrorMessage(msg);
      msg[sizeof msg - 1] = 0;
      for (size_t n = std::strlen(msg); n > 0 && std::isspace(static_cast<unsigned char>(msg[n - 1])); --n) msg[n - 1] = 0;
      d->Release();
      g_host.driverLoaded.store(false);
      return Error{ErrorCode::Device, "ASIO driver '" + e.name + "' failed to initialise" +
                                          (msg[0] ? std::string(": ") + msg : std::string())};
    }
    return d;
  }

  static void unload(asio::Driver*& d) {
    if (!d) return;
    d->Release();
    d = nullptr;
    g_host.driverLoaded.store(false);
  }

  static DeviceInfo query(asio::Driver* d, uint32_t id, const AsioDriverEntry& e) {
    DeviceInfo info;
    info.id = id;
    info.name = e.name;
    info.endpointId = e.name;
    int32_t ni = 0, no = 0;
    if (d->getChannels(&ni, &no) == asio::kOk) {
      info.inputChannels = static_cast<uint32_t>(std::max(0, ni));
      info.outputChannels = static_cast<uint32_t>(std::max(0, no));
    }
    double rate = 0;
    if (d->getSampleRate(&rate) == asio::kOk && rate > 0) info.preferredSampleRate = static_cast<uint32_t>(rate + 0.5);
    for (uint32_t r : kProbeRates)
      if (d->canSampleRate(r) == asio::kOk) info.sampleRates.push_back(r);
    int32_t mn = 0, mx = 0, pref = 0, gran = 0;
    if (d->getBufferSize(&mn, &mx, &pref, &gran) == asio::kOk) {
      info.minBufferFrames = mn;
      info.maxBufferFrames = mx;
      info.preferredBufferFrames = pref;
      info.bufferGranularity = gran;
    }
    if (no <= 0 && ni <= 0) info.probeError = "the driver loads but reports no channels (hardware not connected?)";
    if (no > 0 || ni > 0) {
      asio::ChannelInfo ci{};
      ci.channel = 0;
      ci.isInput = no > 0 ? 0 : 1;
      if (d->getChannelInfo(&ci) == asio::kOk) info.sampleTypeName = asioSampleTypeName(ci.type);
    }
    return info;
  }

  Result<std::vector<DeviceInfo>> enumerate() {
    std::vector<DeviceInfo> out;
    const std::vector<AsioDriverEntry> list = source.list();
    for (size_t i = 0; i < list.size(); ++i) {
      const AsioDriverEntry& e = list[i];
      const uint32_t id = static_cast<uint32_t>(i + 1);
      if (driver) {
        if (e.name == entry.name) {
          out.push_back(query(driver, id, e));
        } else {
          DeviceInfo d;
          d.id = id;
          d.name = e.name;
          d.endpointId = e.name;
          d.probeError = "not queried: another ASIO driver is loaded by this device";
          out.push_back(std::move(d));
        }
        continue;
      }
      auto loaded = load(e);
      if (!loaded) {
        DeviceInfo d;
        d.id = id;
        d.name = e.name;
        d.endpointId = e.name;
        d.probeError = loaded.error().message;
        out.push_back(std::move(d));
        continue;
      }
      asio::Driver* d = *loaded;
      out.push_back(query(d, id, e));
      unload(d);
    }
    return out;
  }

  // Both halves of every output start silent (the driver may play them before the first switch).
  void zeroOutputBuffers() {
    for (uint32_t c = 0; c < numOut; ++c)
      for (uint32_t h = 0; h < 2; ++h) std::memset(devOut[h][c], 0, static_cast<size_t>(frames) * outBytes[c]);
  }

  // Sets up the stream on the loaded driver: rate, buffer size, channel types, createBuffers, latencies.
  // rateOverride (Hz, 0 = none) replaces config.sampleRate (after a driver-side rate change).
  Result<StreamInfo> setup(const StreamConfig& cfg, uint32_t rateOverride, bool prepareRunner) {
    asio::Driver* d = driver;
    int32_t ni = 0, no = 0;
    if (asio::Status s = d->getChannels(&ni, &no); s != asio::kOk)
      return Error{ErrorCode::Device, std::string("ASIO getChannels failed: ") + statusText(s)};
    if (cfg.outputChannels > static_cast<uint32_t>(std::max(0, no)) ||
        cfg.inputChannels > static_cast<uint32_t>(std::max(0, ni)))
      return Error{ErrorCode::InvalidArgument, "ASIO driver '" + entry.name + "' has " + std::to_string(no) +
                                                   " outputs and " + std::to_string(ni) + " inputs; " +
                                                   std::to_string(cfg.outputChannels) + " out / " +
                                                   std::to_string(cfg.inputChannels) + " in requested"};
    double rate = 0;
    if (asio::Status s = d->getSampleRate(&rate); s != asio::kOk)
      return Error{ErrorCode::Device, std::string("ASIO getSampleRate failed: ") + statusText(s)};
    const uint32_t want = rateOverride ? rateOverride : cfg.sampleRate;
    if (want && static_cast<uint32_t>(rate + 0.5) != want) {
      if (d->canSampleRate(want) != asio::kOk)
        return Error{ErrorCode::Unsupported, "ASIO driver '" + entry.name + "' does not support " +
                                                 std::to_string(want) + " Hz"};
      if (asio::Status s = d->setSampleRate(want); s != asio::kOk)
        return Error{ErrorCode::Device, "ASIO setSampleRate(" + std::to_string(want) + ") failed: " + statusText(s)};
      if (d->getSampleRate(&rate) != asio::kOk) rate = want;
    }
    if (!(rate > 0)) return Error{ErrorCode::Device, "ASIO driver '" + entry.name + "' reports no sample rate"};
    int32_t mn = 0, mx = 0, pref = 0, gran = 0;
    if (asio::Status s = d->getBufferSize(&mn, &mx, &pref, &gran); s != asio::kOk)
      return Error{ErrorCode::Device, std::string("ASIO getBufferSize failed: ") + statusText(s)};
    const uint32_t bufferFrames = asioChooseBufferFrames(mn, mx, pref, gran, cfg.bufferFrames);
    if (bufferFrames == 0) return Error{ErrorCode::Device, "ASIO driver reports no valid buffer size"};

    AsioStreamDetails det;
    det.driverName = entry.name;
    det.driverVersion = d->getDriverVersion();
    det.minBufferFrames = mn;
    det.maxBufferFrames = mx;
    det.preferredBufferFrames = pref;
    det.bufferGranularity = gran;
    det.driverInputs = static_cast<uint32_t>(std::max(0, ni));
    det.driverOutputs = static_cast<uint32_t>(std::max(0, no));

    const uint32_t nIn = cfg.inputChannels, nOut = cfg.outputChannels;
    int32_t inTypes[kMaxChannels] = {}, outTypes[kMaxChannels] = {};
    for (uint32_t k = 0; k < nIn + nOut; ++k) {
      const bool isIn = k < nIn;
      const uint32_t c = isIn ? k : k - nIn;
      asio::ChannelInfo ci{};
      ci.channel = static_cast<int32_t>(c);
      ci.isInput = isIn ? 1 : 0;
      if (asio::Status s = d->getChannelInfo(&ci); s != asio::kOk)
        return Error{ErrorCode::Device, std::string("ASIO getChannelInfo failed: ") + statusText(s)};
      if (!asioSampleTypeSupported(ci.type))
        return Error{ErrorCode::Unsupported, std::string("ASIO ") + (isIn ? "input" : "output") + " channel " +
                                                 std::to_string(c) + " uses sample type " +
                                                 asioSampleTypeName(ci.type) + " (" + std::to_string(ci.type) +
                                                 "), which pqdaw does not convert"};
      (isIn ? inTypes : outTypes)[c] = ci.type;
    }
    det.inputSampleType = nIn ? inTypes[0] : -1;
    det.outputSampleType = nOut ? outTypes[0] : -1;

    // Audio-path tables (no callbacks can run yet: buffers do not exist).
    numIn = nIn;
    numOut = nOut;
    frames = bufferFrames;
    scratch = AlignedBuffer<float>(static_cast<size_t>(nIn + nOut) * bufferFrames);
    numConvertIn = numConvertOut = 0;
    bufferInfos.assign(nIn + nOut, asio::BufferInfo{});
    for (uint32_t k = 0; k < nIn + nOut; ++k) {
      bufferInfos[k].isInput = k < nIn ? 1 : 0;
      bufferInfos[k].channelNum = static_cast<int32_t>(k < nIn ? k : k - nIn);
    }

    g_host.requests.store(0, std::memory_order_relaxed);
    streamActive.store(false, std::memory_order_release);
    g_host.active.store(this, std::memory_order_release);
    if (asio::Status s = d->createBuffers(bufferInfos.data(), static_cast<int32_t>(nIn + nOut),
                                          static_cast<int32_t>(bufferFrames), &kHostCallbacks);
        s != asio::kOk) {
      g_host.active.store(nullptr, std::memory_order_release);
      return Error{ErrorCode::Device, "ASIO createBuffers(" + std::to_string(nIn + nOut) + " channels, " +
                                          std::to_string(bufferFrames) + " frames) failed: " + statusText(s)};
    }
    buffersCreated = true;
    for (uint32_t k = 0; k < nIn + nOut; ++k) {
      const asio::BufferInfo& b = bufferInfos[k];
      if (!b.buffers[0] || !b.buffers[1]) {
        teardownBuffers();
        return Error{ErrorCode::Device, "ASIO createBuffers returned a null buffer"};
      }
    }
    for (uint32_t c = 0; c < nIn; ++c) {
      scratchIn[c] = scratch.data() + static_cast<size_t>(c) * bufferFrames;
      fromDevice[c] = asioFromDeviceFn(inTypes[c]);
      const bool direct = inTypes[c] == asio::kFloat32Lsb;
      if (!direct) convertIn[numConvertIn++] = c;
      for (uint32_t h = 0; h < 2; ++h) {
        devIn[h][c] = bufferInfos[c].buffers[h];
        inPtr[h][c] = direct ? static_cast<const float*>(bufferInfos[c].buffers[h]) : scratchIn[c];
      }
    }
    for (uint32_t c = 0; c < nOut; ++c) {
      scratchOut[c] = scratch.data() + static_cast<size_t>(nIn + c) * bufferFrames;
      toDevice[c] = asioToDeviceFn(outTypes[c]);
      outBytes[c] = asioSampleTypeBytes(outTypes[c]);
      const bool direct = outTypes[c] == asio::kFloat32Lsb;
      if (!direct) convertOut[numConvertOut++] = c;
      for (uint32_t h = 0; h < 2; ++h) {
        devOut[h][c] = bufferInfos[nIn + c].buffers[h];
        outPtr[h][c] = direct ? static_cast<float*>(bufferInfos[nIn + c].buffers[h]) : scratchOut[c];
      }
    }
    zeroOutputBuffers();

    int32_t inLat = 0, outLat = 0;
    d->getLatencies(&inLat, &outLat);  // after createBuffers: latencies depend on the buffer size
    postOutput = d->outputReady() == asio::kOk;
    det.outputReady = postOutput;
    // Rate/reset messages a driver sends while being configured (e.g. in answer to setSampleRate) describe the
    // state just set up, not a later change.
    g_host.requests.store(0, std::memory_order_relaxed);

    StreamInfo si;
    si.backend = Backend::Asio;
    si.outputDeviceName = entry.name;
    si.sampleRate = static_cast<uint32_t>(rate + 0.5);
    si.bufferFrames = bufferFrames;
    si.inputChannels = nIn;
    si.outputChannels = nOut;
    si.latencyFrames = static_cast<uint32_t>(std::max(0, outLat));
    si.inputLatencyFrames = static_cast<uint32_t>(std::max(0, inLat));
    si.shareMode = ShareMode::Exclusive;
    si.deviceChannels = nOut;
    si.endpointBufferFrames = bufferFrames;
    si.outputEndpointId = entry.name;
    if (prepareRunner) runner.prepare(cb, nIn, nOut, bufferFrames, si.sampleRate);
    havePos = false;
    {
      std::lock_guard lock(infoMutex);
      info = si;
      details = det;
    }
    return si;
  }

  void teardownBuffers() {
    if (buffersCreated) driver->disposeBuffers();
    buffersCreated = false;
    g_host.active.store(nullptr, std::memory_order_release);
  }

  Status startDriver() {
    havePos = false;
    {
      std::lock_guard lock(infoMutex);
      framesPerTick = static_cast<double>(info.sampleRate) / tscFrequencyHz();
    }
    lateness.reset({frames, frames, false});
    tsc0 = 0;
    delivered = 0;
    streamActive.store(true, std::memory_order_release);
    if (asio::Status s = driver->start(); s != asio::kOk) {
      streamActive.store(false, std::memory_order_release);
      return Error{ErrorCode::Device, std::string("ASIO start failed: ") + statusText(s)};
    }
    running.store(true, std::memory_order_release);
    return Ok{};
  }

  void stopDriver() {
    if (!running.load(std::memory_order_acquire)) return;
    driver->stop();
    streamActive.store(false, std::memory_order_seq_cst);
    // A callback that was already inside process() finishes; none runs the engine after streamActive is false.
    for (int i = 0; i < 2000 && inCallback.load(std::memory_order_seq_cst) != 0; ++i) Sleep(1);
    running.store(false, std::memory_order_release);
  }

  void closeAll() {
    stopDriver();
    teardownBuffers();
    unload(driver);
    open.store(false, std::memory_order_release);
  }

  void setError(std::string e) {
    std::lock_guard lock(infoMutex);
    lastError = std::move(e);
  }

  // Polled on the control thread; returns the next wait timeout.
  DWORD poll() {
    if (!driver) return INFINITE;
    handleRequests();
    return running.load(std::memory_order_relaxed) ? kPollMs : kIdlePollMs;
  }

  void handleRequests() {
    const uint32_t req = g_host.requests.exchange(0, std::memory_order_acquire);
    if (!req || !driver || !open.load()) return;
    if (req & (kReqReset | kReqRate)) {
      uint32_t newRate = 0;
      if (req & kReqRate) {
        const double r = std::bit_cast<double>(g_host.newRateBits.load(std::memory_order_relaxed));
        if (r > 0) newRate = static_cast<uint32_t>(r + 0.5);
        uint32_t cur;
        {
          std::lock_guard lock(infoMutex);
          cur = info.sampleRate;
        }
        if (!(req & kReqReset) && newRate == cur) return;  // nothing changed for us
      }
      reopen(newRate);
      return;
    }
    if (req & kReqLatencies) {
      int32_t inLat = 0, outLat = 0;
      if (driver->getLatencies(&inLat, &outLat) == asio::kOk) {
        std::lock_guard lock(infoMutex);
        info.latencyFrames = static_cast<uint32_t>(std::max(0, outLat));
        info.inputLatencyFrames = static_cast<uint32_t>(std::max(0, inLat));
      }
    }
  }

  uint64_t reopens = 0, failedReopens = 0;

  // Driver reset: stop → disposeBuffers → Release → reload → init → recreate (→ start if it was running).
  void reopen(uint32_t newRate) {
    const bool wasRunning = running.load();
    StreamInfo before;
    {
      std::lock_guard lock(infoMutex);
      before = info;
    }
    stopDriver();
    teardownBuffers();
    unload(driver);
    auto fail = [&](const std::string& why) {
      ++failedReopens;
      setError("ASIO driver '" + entry.name + "' asked for a reset and could not be reopened: " + why);
      teardownBuffers();
      unload(driver);
      open.store(false, std::memory_order_release);
    };
    auto loaded = load(entry);
    if (!loaded) return fail(loaded.error().message);
    driver = *loaded;
    auto s = setup(config, newRate, false);
    if (!s) return fail(s.error().message);
    if (s->sampleRate != before.sampleRate || s->bufferFrames != before.bufferFrames)
      runner.prepare(cb, numIn, numOut, s->bufferFrames, s->sampleRate);
    if (newRate) config.sampleRate = newRate;  // follow the driver's clock from now on (Q70)
    ++reopens;
    if (changedFn) changedFn(changedUser, *s);
    if (wasRunning)
      if (auto st = startDriver(); !st) return fail(st.error().message);
  }
};

const asio::HostCallbacks AsioDevice::Impl::kHostCallbacks = {&onBufferSwitch, &onRateChanged, &onMessage,
                                                              &onBufferSwitchTimeInfo};

AsioDevice::AsioDevice() : AsioDevice(AsioDriverSource::registry()) {}

AsioDevice::AsioDevice(AsioDriverSource source) : impl_(std::make_unique<Impl>()) {
  impl_->source = std::move(source);
  Impl* impl = impl_.get();
  impl_->sta = std::make_unique<StaThread>([impl] { return impl->poll(); });
}

AsioDevice::~AsioDevice() {
  close();
  impl_->sta.reset();  // joins the control thread
}

Result<std::vector<AsioDriverEntry>> AsioDevice::drivers() {
  std::vector<AsioDriverEntry> out;
  impl_->sta->call([&] { out = impl_->source.list(); });
  return out;
}

Result<std::vector<DeviceInfo>> AsioDevice::enumerate() {
  Result<std::vector<DeviceInfo>> r = std::vector<DeviceInfo>{};
  impl_->sta->call([&] { r = impl_->enumerate(); });
  return r;
}

Result<StreamInfo> AsioDevice::open(const StreamConfig& config, AudioCallback cb) {
  Result<StreamInfo> result = Error{ErrorCode::Unknown, "ASIO open: not run"};
  impl_->sta->call([&] {
    Impl& m = *impl_;
    if (m.open.load()) {
      result = Error{ErrorCode::InvalidArgument, "AsioDevice: a stream is already open"};
      return;
    }
    if (!cb) {
      result = Error{ErrorCode::InvalidArgument, "AsioDevice: no callback"};
      return;
    }
    if (config.outputChannels == 0 && config.inputChannels == 0) {
      result = Error{ErrorCode::InvalidArgument, "AsioDevice: a stream needs input or output channels"};
      return;
    }
    if (config.outputChannels > kMaxChannels || config.inputChannels > kMaxChannels) {
      result = Error{ErrorCode::InvalidArgument, "AsioDevice: too many channels"};
      return;
    }
    if (config.bufferFrames == 0) {
      result = Error{ErrorCode::InvalidArgument, "AsioDevice: bufferFrames must be >= 1"};
      return;
    }
    const std::vector<AsioDriverEntry> list = m.source.list();
    if (list.empty()) {
      result = Error{ErrorCode::NotFound, "no ASIO drivers are registered (HKLM\\SOFTWARE\\ASIO)"};
      return;
    }
    std::vector<size_t> candidates;
    if (!config.outputEndpointId.empty()) {
      for (size_t i = 0; i < list.size(); ++i)
        if (iequals(list[i].name, config.outputEndpointId)) candidates.push_back(i);
      if (candidates.empty()) {
        result = Error{ErrorCode::NotFound, "no ASIO driver named '" + config.outputEndpointId + "' (see --list-devices)"};
        return;
      }
    } else if (config.outputDevice != kDefaultDevice) {
      if (config.outputDevice == 0 || config.outputDevice > list.size()) {
        result = Error{ErrorCode::NotFound, "AsioDevice: unknown driver id " + std::to_string(config.outputDevice) +
                                                " (ids are 1.." + std::to_string(list.size()) + ", see --list-devices)"};
        return;
      }
      candidates.push_back(config.outputDevice - 1);
    } else {
      for (size_t i = 0; i < list.size(); ++i) candidates.push_back(i);  // default: the first that loads
    }
    Error firstError{ErrorCode::NotFound, "no ASIO driver could be loaded"};
    bool haveError = false;
    for (size_t i : candidates) {
      auto loaded = m.load(list[i]);
      if (!loaded) {
        if (!haveError) firstError = loaded.error();
        haveError = true;
        if (loaded.error().message.find("another ASIO driver") != std::string::npos) break;
        continue;
      }
      m.driver = *loaded;
      m.entry = list[i];
      for (auto* c : {&g_host.resetRequests, &g_host.bufferSizeChanges, &g_host.resyncRequests, &g_host.latencyChanges,
                      &g_host.rateChanges, &g_host.overloads})
        c->store(0, std::memory_order_relaxed);
      m.reopens = m.failedReopens = 0;
      m.config = config;
      m.cb = cb;
      {
        std::lock_guard lock(m.infoMutex);
        m.lastError.clear();
      }
      auto s = m.setup(config, 0, true);
      if (!s) {
        m.teardownBuffers();
        Impl::unload(m.driver);
        if (!haveError) firstError = s.error();
        haveError = true;
        continue;
      }
      m.open.store(true, std::memory_order_release);
      result = *s;
      return;
    }
    result = firstError;
  });
  return result;
}

Status AsioDevice::start() {
  Status st = Ok{};
  impl_->sta->call([&] {
    if (!impl_->open.load()) {
      st = Error{ErrorCode::InvalidArgument, "AsioDevice: no open stream"};
      return;
    }
    if (impl_->running.load()) return;
    st = impl_->startDriver();
  });
  return st;
}

Status AsioDevice::stop() {
  impl_->sta->call([&] {
    if (impl_->driver) impl_->stopDriver();
  });
  return Ok{};
}

void AsioDevice::close() noexcept {
  if (!impl_ || !impl_->sta) return;
  impl_->sta->call([&] {
    if (impl_->driver) impl_->closeAll();
    impl_->open.store(false, std::memory_order_release);
  });
}

bool AsioDevice::isOpen() const noexcept { return impl_->open.load(std::memory_order_acquire); }
bool AsioDevice::isRunning() const noexcept { return impl_->running.load(std::memory_order_acquire); }
const CallbackStats& AsioDevice::stats() const noexcept { return impl_->runner.stats(); }

Status AsioDevice::controlPanel() {
  Status st = Ok{};
  impl_->sta->call([&] {
    if (!impl_->driver) {
      st = Error{ErrorCode::InvalidArgument, "AsioDevice: open a stream first (the driver must be loaded)"};
      return;
    }
    if (asio::Status s = impl_->driver->controlPanel(); s != asio::kOk)
      st = Error{ErrorCode::Device, std::string("ASIO controlPanel failed: ") + statusText(s)};
  });
  return st;
}

StreamInfo AsioDevice::streamInfo() const {
  std::lock_guard lock(impl_->infoMutex);
  return impl_->info;
}

AsioStreamDetails AsioDevice::details() const {
  std::lock_guard lock(impl_->infoMutex);
  return impl_->details;
}

AsioEvents AsioDevice::events() const {
  AsioEvents e;
  e.resetRequests = g_host.resetRequests.load(std::memory_order_relaxed);
  e.bufferSizeChanges = g_host.bufferSizeChanges.load(std::memory_order_relaxed);
  e.resyncRequests = g_host.resyncRequests.load(std::memory_order_relaxed);
  e.latencyChanges = g_host.latencyChanges.load(std::memory_order_relaxed);
  e.rateChanges = g_host.rateChanges.load(std::memory_order_relaxed);
  e.overloads = g_host.overloads.load(std::memory_order_relaxed);
  impl_->sta->call([&] {
    e.reopens = impl_->reopens;
    e.failedReopens = impl_->failedReopens;
  });
  return e;
}

std::string AsioDevice::lastError() const {
  std::lock_guard lock(impl_->infoMutex);
  return impl_->lastError;
}

void AsioDevice::setStreamChangedCallback(StreamChangedFn fn, void* user) {
  impl_->sta->call([&] {
    impl_->changedFn = fn;
    impl_->changedUser = user;
  });
}

void AsioDevice::handlePendingRequests() {
  impl_->sta->call([&] { impl_->handleRequests(); });
}

}  // namespace pq::engine
