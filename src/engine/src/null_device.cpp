#include "pq/engine/null_device.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <system_error>

#include <windows.h>

namespace pq::engine {

namespace {
constexpr uint32_t kNullDeviceChannels = kMaxChannels;
constexpr uint32_t kNullDefaultRate = 48000;
}  // namespace

NullDevice::~NullDevice() { close(); }

Result<std::vector<DeviceInfo>> NullDevice::enumerate() {
  DeviceInfo d;
  d.id = 0;
  d.name = "Null device";
  d.outputChannels = kNullDeviceChannels;
  d.inputChannels = kNullDeviceChannels;
  d.isDefaultOutput = true;
  d.isDefaultInput = true;
  d.preferredSampleRate = kNullDefaultRate;
  d.sampleRates = {44100, 48000, 88200, 96000, 176400, 192000};
  return std::vector<DeviceInfo>{d};
}

Result<StreamInfo> NullDevice::open(const StreamConfig& config, AudioCallback cb) {
  if (open_) return Error{ErrorCode::InvalidArgument, "NullDevice: a stream is already open"};
  if (!cb) return Error{ErrorCode::InvalidArgument, "NullDevice: no callback"};
  if ((config.outputDevice != kDefaultDevice && config.outputDevice != 0) ||
      (config.inputDevice != kDefaultDevice && config.inputDevice != 0))
    return Error{ErrorCode::NotFound, "NullDevice: only device 0 exists"};
  if (config.outputChannels > kNullDeviceChannels || config.inputChannels > kNullDeviceChannels)
    return Error{ErrorCode::InvalidArgument, "NullDevice: too many channels"};
  if (config.bufferFrames == 0) return Error{ErrorCode::InvalidArgument, "NullDevice: bufferFrames must be >= 1"};

  bufferFrames_ = config.bufferFrames;
  sampleRate_ = config.sampleRate ? config.sampleRate : kNullDefaultRate;
  const uint32_t numIn = config.inputChannels;
  const uint32_t numOut = config.outputChannels;
  const std::size_t stride = (static_cast<std::size_t>(bufferFrames_) + 15u) & ~std::size_t{15};
  storage_ = AlignedBuffer<float>(stride * (numIn + numOut));
  for (uint32_t c = 0; c < kMaxChannels; ++c) {
    inPtrs_[c] = c < numIn ? storage_.data() + stride * c : nullptr;
    outPtrs_[c] = c < numOut ? storage_.data() + stride * (numIn + c) : nullptr;
  }
  runner_.prepare(cb, numIn, numOut, bufferFrames_, sampleRate_);
  open_ = true;

  StreamInfo info;
  info.backend = Backend::Null;
  info.outputDeviceName = "Null device";
  info.sampleRate = sampleRate_;
  info.bufferFrames = bufferFrames_;
  info.inputChannels = numIn;
  info.outputChannels = numOut;
  return info;
}

Status NullDevice::start() {
  if (!open_) return Error{ErrorCode::InvalidArgument, "NullDevice: start() without an open stream"};
  if (running_.load(std::memory_order_acquire)) return Error{ErrorCode::InvalidArgument, "NullDevice: already running"};
  running_.store(true, std::memory_order_release);
  try {
    thread_ = std::thread([this] { threadMain(); });
  } catch (const std::system_error& e) {
    running_.store(false, std::memory_order_release);
    return Error{ErrorCode::Device, std::string("NullDevice: cannot start thread: ") + e.what()};
  }
  return Ok{};
}

Status NullDevice::stop() {
  if (running_.exchange(false, std::memory_order_acq_rel) && thread_.joinable()) thread_.join();
  return Ok{};
}

void NullDevice::close() noexcept {
  (void)stop();
  storage_ = AlignedBuffer<float>();
  for (uint32_t c = 0; c < kMaxChannels; ++c) inPtrs_[c] = outPtrs_[c] = nullptr;
  open_ = false;
}

void NullDevice::processBlock(uint32_t frames, bool inputOverflow, bool outputUnderflow) noexcept {  // RT-safe
  const uint32_t n = frames == 0 ? bufferFrames_ : std::min(frames, bufferFrames_);
  runner_.run(inPtrs_, outPtrs_, n, inputOverflow, outputUnderflow);
}

void NullDevice::threadMain() noexcept {
  // Absolute deadlines k · bufferFrames / sampleRate after the start, measured with QPC: a late wake-up is followed
  // by an immediate catch-up callback, so the long-run callback rate is exact even with a coarse timer.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  LARGE_INTEGER freq{}, start{}, now{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  const double qpcPerFrame = static_cast<double>(freq.QuadPart) / sampleRate_;
  uint64_t framesDone = 0;
  while (running_.load(std::memory_order_acquire)) {
    processBlock();
    framesDone += bufferFrames_;
    const int64_t deadline = start.QuadPart + static_cast<int64_t>(static_cast<double>(framesDone) * qpcPerFrame);
    QueryPerformanceCounter(&now);
    const int64_t waitQpc = deadline - now.QuadPart;
    if (waitQpc <= 0) continue;
    const int64_t wait100ns = waitQpc * 10'000'000 / freq.QuadPart;
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<int64_t>(wait100ns, 1);  // negative = relative
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
      WaitForSingleObject(timer, INFINITE);
    else
      std::this_thread::sleep_for(std::chrono::nanoseconds(wait100ns * 100));
  }
  if (timer) CloseHandle(timer);
}

}  // namespace pq::engine
