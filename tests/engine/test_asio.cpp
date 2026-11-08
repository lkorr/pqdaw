// Own ASIO host (T0.6-fix2): sample conversion, buffer-size choice, and the full host path against a fake driver
// that implements the driver ABI (pq/engine/asio_abi.h) in this file: double-buffer switching, format conversion,
// zero-copy float32, missed-buffer detection, reset / resync / buffer-size / sample-rate / latency messages, the
// one-driver-per-process rule, init failures, control panel. Real drivers are opt-in: PQ_TEST_AUDIO_DEVICE=1
// (plays silence on the first driver that loads, or on PQ_TEST_ASIO_DRIVER=<name>).
#include <doctest/doctest.h>

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/rt_scope.h"
#include "pq/engine/asio_abi.h"
#include "pq/engine/asio_convert.h"
#include "pq/engine/asio_device.h"
#include "pq/engine/stream_trace.h"
#include "pq/engine/wasapi_device.h"
#include "fake_asio_driver.h"
#include "loopback_probe.h"

using namespace pq::engine;
namespace a = pq::engine::asio;
using pq::test::FakeDriver;
using pq::test::fakeSource;

namespace {

// --- Conversion reference: double math, explicit round-half-even, explicit byte layout -------------------------

struct TypeSpec {
  int32_t type;
  uint32_t bytes;
  int bits;  // 0 = float
};
constexpr TypeSpec kTypes[] = {
    {a::kInt16Lsb, 2, 16},   {a::kInt24Lsb, 3, 24},   {a::kInt32Lsb, 4, 32},   {a::kFloat32Lsb, 4, 0},
    {a::kFloat64Lsb, 8, 0},  {a::kInt32Lsb16, 4, 16}, {a::kInt32Lsb18, 4, 18}, {a::kInt32Lsb20, 4, 20},
    {a::kInt32Lsb24, 4, 24},
};

std::vector<uint8_t> refToDevice(const std::vector<float>& x, const TypeSpec& t) {
  std::vector<uint8_t> out;
  for (float v : x) {
    if (t.type == a::kFloat32Lsb) {
      uint8_t b[4];
      std::memcpy(b, &v, 4);
      out.insert(out.end(), b, b + 4);
      continue;
    }
    if (t.type == a::kFloat64Lsb) {
      const double d = v;
      uint8_t b[8];
      std::memcpy(b, &d, 8);
      out.insert(out.end(), b, b + 8);
      continue;
    }
    const double scale = std::ldexp(1.0, t.bits - 1);
    // Int32: the converter's largest value is the largest float below 2^31 (as WASAPI's), i.e. 2^31 - 128.
    const double hi = t.bits == 32 ? 2147483520.0 : scale - 1.0;
    double s = static_cast<double>(v) * scale;
    if (std::isnan(s) || s < -scale) s = -scale;
    if (s > hi) s = hi;
    const int64_t q = static_cast<int64_t>(std::nearbyint(s));
    const uint32_t u = static_cast<uint32_t>(q);
    for (uint32_t k = 0; k < t.bytes; ++k) out.push_back(static_cast<uint8_t>(u >> (8 * k)));
  }
  return out;
}

std::vector<float> testSignal(uint32_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-1.2f, 1.2f);
  std::vector<float> x(n);
  for (float& v : x) v = u(rng);
  if (n > 0) x[0] = 1.0f;
  if (n > 1) x[1] = -1.0f;
  if (n > 2) x[2] = std::nanf("");
  if (n > 3) x[3] = 0.0f;
  if (n > 4) x[4] = 0.99999994f;
  return x;
}

}  // namespace

TEST_CASE("asio abi: layout and constants of the x64 driver interface") {
  CHECK(sizeof(a::BufferInfo) == 24);
  CHECK(sizeof(a::ChannelInfo) == 52);
  CHECK(sizeof(a::ClockSource) == 48);
  CHECK(sizeof(a::Time) == 152);
  CHECK(sizeof(a::HostCallbacks) == 4 * sizeof(void*));
  CHECK(a::toInt64(a::Int64Halves{1, 2}) == ((int64_t{1} << 32) | 2));
  CHECK(asioSampleTypeSupported(a::kInt32Lsb));
  CHECK_FALSE(asioSampleTypeSupported(a::kInt32Msb));
  CHECK_FALSE(asioSampleTypeSupported(a::kDsdInt8Lsb1));
  CHECK(std::string(asioSampleTypeName(a::kInt24Lsb)) == "Int24LSB");
  CHECK(std::string(asioSampleTypeName(12345)) == "?");
}

TEST_CASE("asio convert: float to every supported type matches the reference, any length") {
  for (const TypeSpec& t : kTypes) {
    for (uint32_t n : {0u, 1u, 7u, 8u, 9u, 31u, 64u, 67u, 480u}) {
      const std::vector<float> x = testSignal(n, n * 31 + static_cast<uint32_t>(t.type));
      std::vector<uint8_t> dst(static_cast<size_t>(n) * t.bytes + 16, 0xEE);
      ToDeviceFn fn = asioToDeviceFn(t.type);
      REQUIRE(fn != nullptr);
      CHECK(asioSampleTypeBytes(t.type) == t.bytes);
      fn(x.data(), n, dst.data());
      const std::vector<uint8_t> ref = refToDevice(x, t);
      INFO("type " << std::string(asioSampleTypeName(t.type)) << " n " << n);
      bool same = true;
      for (size_t i = 0; i < ref.size(); ++i) same = same && dst[i] == ref[i];
      if (t.type == a::kFloat32Lsb && n > 2) {  // NaN passes through as float; compare the others bitwise
        same = true;
        for (uint32_t i = 0; i < n; ++i) {
          float v;
          std::memcpy(&v, dst.data() + 4 * i, 4);
          same = same && (std::isnan(x[i]) ? std::isnan(v) : v == x[i]);
        }
      }
      if (t.type == a::kFloat64Lsb && n > 2) {
        same = true;
        for (uint32_t i = 0; i < n; ++i) {
          double v;
          std::memcpy(&v, dst.data() + 8 * i, 8);
          same = same && (std::isnan(x[i]) ? std::isnan(v) : v == static_cast<double>(x[i]));
        }
      }
      CHECK(same);
      bool guard = true;  // nothing written past the end
      for (size_t i = ref.size(); i < dst.size(); ++i) guard = guard && dst[i] == 0xEE;
      CHECK(guard);
    }
  }
}

TEST_CASE("asio convert: device to float is exact for every supported type, any length") {
  std::mt19937 rng(7);
  for (const TypeSpec& t : kTypes) {
    for (uint32_t n : {0u, 1u, 7u, 8u, 9u, 33u, 64u, 71u}) {
      std::vector<uint8_t> src(static_cast<size_t>(n) * t.bytes);
      std::vector<double> expect(n);
      for (uint32_t i = 0; i < n; ++i) {
        uint8_t* p = src.data() + static_cast<size_t>(i) * t.bytes;
        if (t.type == a::kFloat32Lsb) {
          const float v = std::uniform_real_distribution<float>(-1.0f, 1.0f)(rng);
          std::memcpy(p, &v, 4);
          expect[i] = v;
        } else if (t.type == a::kFloat64Lsb) {
          const double v = std::uniform_real_distribution<double>(-1.0, 1.0)(rng);
          std::memcpy(p, &v, 8);
          expect[i] = static_cast<float>(v);
        } else {
          const int64_t lo = -(int64_t{1} << (t.bits - 1)), hi = (int64_t{1} << (t.bits - 1)) - 1;
          int64_t q = std::uniform_int_distribution<int64_t>(lo, hi)(rng);
          if (i == 0) q = lo;
          if (i == 1) q = hi;
          const uint32_t u = static_cast<uint32_t>(static_cast<int32_t>(q));  // sign-extended container
          for (uint32_t k = 0; k < t.bytes; ++k) p[k] = static_cast<uint8_t>(u >> (8 * k));
          expect[i] = static_cast<float>(static_cast<double>(q) / std::ldexp(1.0, t.bits - 1));
        }
      }
      std::vector<float> dst(n + 4, 123.0f);
      FromDeviceFn fn = asioFromDeviceFn(t.type);
      REQUIRE(fn != nullptr);
      fn(src.data(), n, dst.data());
      INFO("type " << std::string(asioSampleTypeName(t.type)) << " n " << n);
      bool same = true;
      for (uint32_t i = 0; i < n; ++i) same = same && dst[i] == static_cast<float>(expect[i]);
      CHECK(same);
      CHECK(dst[n] == 123.0f);
    }
  }
}

TEST_CASE("asio convert: round trip and RT safety") {
  const std::vector<float> x = testSignal(256, 3);
  for (const TypeSpec& t : kTypes) {
    std::vector<uint8_t> dev(256 * t.bytes);
    std::vector<float> back(256);
    CHECK(pq::alloc_trap::countRtViolations([&] {
            asioToDeviceFn(t.type)(x.data(), 256, dev.data());
            asioFromDeviceFn(t.type)(dev.data(), 256, back.data());
          }) == 0);
    const double tol = t.bits ? std::ldexp(1.0, -(t.bits - 1)) : 0.0;
    bool ok = true;
    for (uint32_t i = 5; i < 256; ++i) {
      // Float types pass any value through; integer types clamp to full scale.
      const double clamped = t.bits ? std::clamp(static_cast<double>(x[i]), -1.0, 1.0) : static_cast<double>(x[i]);
      ok = ok && std::fabs(back[i] - clamped) <= tol + 1e-7;
    }
    INFO(std::string(asioSampleTypeName(t.type)));
    CHECK(ok);
  }
}

TEST_CASE("asio: buffer size choice follows the driver's granularity") {
  // Powers of two in [32, 2048].
  CHECK(asioChooseBufferFrames(32, 2048, 256, -1, 64) == 64);
  CHECK(asioChooseBufferFrames(32, 2048, 256, -1, 100) == 128);
  CHECK(asioChooseBufferFrames(32, 2048, 256, -1, 16) == 32);
  CHECK(asioChooseBufferFrames(32, 2048, 256, -1, 5000) == 2048);
  CHECK(asioChooseBufferFrames(32, 2048, 256, -1, 0) == 256);
  // Steps of 8 from 16.
  CHECK(asioChooseBufferFrames(16, 1024, 128, 8, 64) == 64);
  CHECK(asioChooseBufferFrames(16, 1024, 128, 8, 65) == 72);
  CHECK(asioChooseBufferFrames(16, 1020, 128, 8, 2000) == 1016);
  CHECK(asioChooseBufferFrames(16, 1024, 128, 8, 1) == 16);
  // Preferred only / fixed.
  CHECK(asioChooseBufferFrames(64, 2048, 512, 0, 64) == 512);
  CHECK(asioChooseBufferFrames(256, 256, 256, 0, 64) == 256);
  CHECK(asioChooseBufferFrames(256, 256, 256, -1, 64) == 256);
  // Nonsense answers fall back to preferred.
  CHECK(asioChooseBufferFrames(0, 0, 192, 0, 64) == 192);
  CHECK(asioChooseBufferFrames(512, 64, 128, -1, 64) == 128);
  // Powers of two but none in range → preferred.
  CHECK(asioChooseBufferFrames(100, 120, 110, -1, 64) == 110);
}

// --- Fake driver -------------------------------------------------------------------------------------------------

namespace {

// Engine: out[c][i] = in[c][i] * 0.5 + c * 0.125 (so each channel and both directions are checked).
struct PassEngine {
  std::atomic<uint64_t> blocks{0};
  uint32_t lastFrames = 0;
  static void process(void* user, const AudioBlock& b) noexcept {  // RT-safe
    auto& e = *static_cast<PassEngine*>(user);
    for (uint32_t c = 0; c < b.numOut; ++c)
      for (uint32_t i = 0; i < b.frames; ++i)
        b.out[c][i] = (c < b.numIn ? b.in[c][i] * 0.5f : 0.0f) + static_cast<float>(c) * 0.125f;
    e.lastFrames = b.frames;
    e.blocks.fetch_add(1, std::memory_order_relaxed);
  }
};

void writeInput(FakeDriver& f, int32_t half, uint32_t frames, float base) {
  for (int32_t c = 0; c < f.numIn; ++c) {
    void* p = f.buffer(true, c, half);
    if (!p) continue;
    std::vector<float> x(frames);
    for (uint32_t i = 0; i < frames; ++i) x[i] = base + 0.001f * static_cast<float>(i) - 0.1f * static_cast<float>(c);
    asioToDeviceFn(f.inType)(x.data(), frames, p);
  }
}

std::vector<float> readOutput(FakeDriver& f, int32_t ch, int32_t half, uint32_t frames) {
  std::vector<float> y(frames);
  asioFromDeviceFn(f.outType)(f.buffer(false, ch, half), frames, y.data());
  return y;
}

}  // namespace

TEST_CASE("asio host: enumerate, open, double-buffer switching with conversion, stats, RtScope") {
  for (int32_t type : {a::kInt32Lsb, a::kFloat32Lsb, a::kInt24Lsb, a::kInt16Lsb, a::kInt32Lsb24}) {
    INFO("sample type " << std::string(asioSampleTypeName(type)));
    FakeDriver f;
    f.inType = f.outType = type;
    AsioDevice dev(fakeSource(f));
    CHECK(dev.backend() == Backend::Asio);

    auto list = dev.enumerate();
    REQUIRE(list.ok());
    REQUIRE(list->size() == 2);
    CHECK((*list)[0].id == 1);
    CHECK((*list)[0].name == "Fake ASIO");
    CHECK((*list)[0].endpointId == "Fake ASIO");
    CHECK((*list)[0].outputChannels == 2);
    CHECK((*list)[0].inputChannels == 2);
    CHECK((*list)[0].preferredSampleRate == 48000);
    CHECK((*list)[0].sampleRates == std::vector<uint32_t>{44100, 48000, 96000});
    CHECK((*list)[0].minBufferFrames == 32);
    CHECK((*list)[0].maxBufferFrames == 1024);
    CHECK((*list)[0].bufferGranularity == -1);
    CHECK((*list)[0].sampleTypeName == asioSampleTypeName(type));
    CHECK((*list)[0].probeError.empty());
    CHECK_FALSE((*list)[1].probeError.empty());
    CHECK(f.refs == 0);  // probed and released

    PassEngine eng;
    StreamConfig cfg;
    cfg.outputEndpointId = "fake asio";  // case-insensitive name
    cfg.outputChannels = 2;
    cfg.inputChannels = 2;
    cfg.bufferFrames = 64;
    auto info = dev.open(cfg, {&PassEngine::process, &eng});
    REQUIRE(info.ok());
    CHECK(info->backend == Backend::Asio);
    CHECK(info->outputDeviceName == "Fake ASIO");
    CHECK(info->sampleRate == 48000);
    CHECK(info->bufferFrames == 64);
    CHECK(info->latencyFrames == 200);
    CHECK(info->inputLatencyFrames == 100);
    CHECK(info->inputChannels == 2);
    CHECK(info->outputChannels == 2);
    CHECK(f.lastBufferSize == 64);
    CHECK(f.refs == 1);
    CHECK(dev.isOpen());
    CHECK_FALSE(dev.isRunning());
    const AsioStreamDetails det = dev.details();
    CHECK(det.driverVersion == 7);
    CHECK(det.outputSampleType == type);
    CHECK(det.outputReady);
    // Output halves start silent.
    for (int32_t h = 0; h < 2; ++h)
      for (float v : readOutput(f, 0, h, 64)) CHECK(v == 0.0f);

    REQUIRE(dev.start().ok());
    CHECK(dev.isRunning());
    CHECK(f.started);
    for (int k = 0; k < 10; ++k) {
      const int32_t half = k & 1;
      writeInput(f, half, 64, 0.01f * static_cast<float>(k));
      CHECK(pq::alloc_trap::countRtViolations([&] { f.tick(half, k % 3 != 0); }) == 0);
      const double tol = type == a::kInt16Lsb ? 1.0 / 16384 : type == a::kInt24Lsb || type == a::kInt32Lsb24 ? 1e-6 : 1e-7;
      for (int32_t c = 0; c < 2; ++c) {
        const std::vector<float> y = readOutput(f, c, half, 64);
        bool ok = true;
        for (uint32_t i = 0; i < 64; ++i) {
          const float x = 0.01f * static_cast<float>(k) + 0.001f * static_cast<float>(i) - 0.1f * static_cast<float>(c);
          const float expect = x * 0.5f + static_cast<float>(c) * 0.125f;
          ok = ok && std::fabs(y[i] - expect) <= tol;
        }
        CHECK(ok);
      }
    }
    CHECK(eng.blocks == 10);
    CHECK(eng.lastFrames == 64);
    CHECK(f.outputReadies == 10);
    REQUIRE(dev.stop().ok());
    CHECK_FALSE(dev.isRunning());
    CHECK_FALSE(f.started);
    // A late switch after stop() is ignored (no callback, no crash).
    f.tick(0);
    CHECK(eng.blocks == 10);
    // AU.voices: a restart (the app's resume after an idle suspend) starts both halves silent again, not with the
    // last two buffers of the previous run.
    REQUIRE(dev.start().ok());
    for (int32_t h = 0; h < 2; ++h)
      for (float v : readOutput(f, 0, h, 64)) CHECK(v == 0.0f);
    REQUIRE(dev.stop().ok());
    const CallbackStats::Snapshot st = dev.stats().snapshot();
    CHECK(st.callbacks == 10);
    CHECK(st.frames == 640);
    CHECK(st.xruns() == 0);
    dev.close();
    CHECK_FALSE(dev.isOpen());
    CHECK(f.disposes == 1);
    CHECK(f.refs == 0);
  }
}

TEST_CASE("asio host: a sample-position jump is counted as a missed buffer") {
  FakeDriver f;
  AsioDevice dev(fakeSource(f));
  PassEngine eng;
  StreamConfig cfg;
  cfg.bufferFrames = 128;
  REQUIRE(dev.open(cfg, {&PassEngine::process, &eng}).ok());  // default: the first driver that loads
  REQUIRE(dev.start().ok());
  f.tick(0);
  f.tick(1);
  f.tick(0, true, 3 * 128);  // the driver skipped two buffers after this one
  f.tick(1);
  f.tick(0);
  f.tick(1, false);  // bufferSwitch without time info: the host asks getSamplePosition
  CHECK(dev.stats().snapshot().outputUnderflows == 1);
  dev.close();
}

TEST_CASE("asio host: a buffer switch that comes late against the sample clock is reported as lateness (M0 L2)") {
  FakeDriver f;
  AsioDevice dev(fakeSource(f));
  PassEngine eng;
  StreamConfig cfg;
  cfg.bufferFrames = 128;
  REQUIRE(dev.open(cfg, {&PassEngine::process, &eng}).ok());
  REQUIRE(dev.start().ok());
  // The fake's switches come back to back (microseconds apart) while its position advances 128 frames each: early
  // against the sample clock.
  for (int i = 0; i < 40; ++i) f.tick(i & 1);
  CallbackStats::Snapshot st = dev.stats().snapshot();
  CHECK(st.wakes == 40);  // one wake per buffer switch
  CHECK(st.callbacks == 40);
  // A switch 60 ms late (2880 frames at 48 kHz): reported as at least 50 ms. A sleep only ever gets longer, so this
  // lower bound holds under load.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  f.tick(0);
  st = dev.stats().snapshot();
  CHECK(st.wakes == 41);
  CHECK(st.maxLatenessFrames >= 2400);
  CHECK(st.maxLatenessUs >= 50000.0);
  CHECK(st.outputUnderflows == 0);  // lateness is reported, not counted as an underrun (no position jump)
  dev.close();
}

TEST_CASE("asio host: rate selection, unsupported rate, channel count, sample type and init errors") {
  FakeDriver f;
  AsioDevice dev(fakeSource(f));
  PassEngine eng;
  StreamConfig cfg;
  cfg.outputDevice = 1;
  cfg.sampleRate = 96000;
  auto info = dev.open(cfg, {&PassEngine::process, &eng});
  REQUIRE(info.ok());
  CHECK(info->sampleRate == 96000);
  CHECK(f.setRates == 1);
  CHECK(info->bufferFrames == 256);  // config default 256: a power of two in range
  dev.handlePendingRequests();       // the driver's sampleRateDidChange answer to setSampleRate is not a change
  CHECK(dev.events().reopens == 0);
  CHECK(dev.open(cfg, {&PassEngine::process, &eng}).error().code == pq::ErrorCode::InvalidArgument);  // already open
  dev.close();

  cfg.sampleRate = 22050;
  CHECK(dev.open(cfg, {&PassEngine::process, &eng}).error().code == pq::ErrorCode::Unsupported);
  CHECK(f.refs == 0);
  cfg.sampleRate = 0;
  cfg.outputChannels = 3;
  CHECK(dev.open(cfg, {&PassEngine::process, &eng}).error().code == pq::ErrorCode::InvalidArgument);
  cfg.outputChannels = 2;
  cfg.outputDevice = 3;
  CHECK(dev.open(cfg, {&PassEngine::process, &eng}).error().code == pq::ErrorCode::NotFound);
  cfg.outputDevice = kDefaultDevice;
  cfg.outputEndpointId = "nope";
  CHECK(dev.open(cfg, {&PassEngine::process, &eng}).error().code == pq::ErrorCode::NotFound);
  cfg.outputEndpointId.clear();
  CHECK(dev.open(cfg, AudioCallback{}).error().code == pq::ErrorCode::InvalidArgument);

  f.outType = a::kInt32Msb;
  auto bad = dev.open(cfg, {&PassEngine::process, &eng});
  REQUIRE_FALSE(bad.ok());
  CHECK(bad.error().code == pq::ErrorCode::Unsupported);
  CHECK(bad.error().message.find("Int32MSB") != std::string::npos);
  f.outType = a::kInt32Lsb;

  f.failInit = true;
  cfg.outputDevice = 1;
  auto failed = dev.open(cfg, {&PassEngine::process, &eng});
  REQUIRE_FALSE(failed.ok());
  CHECK(failed.error().message.find("fake hardware unplugged") != std::string::npos);
  CHECK(f.refs == 0);
  CHECK_FALSE(dev.isOpen());
  CHECK(dev.start().error().code == pq::ErrorCode::InvalidArgument);
  CHECK(dev.stop().ok());
  CHECK(dev.controlPanel().error().code == pq::ErrorCode::InvalidArgument);
}

TEST_CASE("asio host: one driver per process; control panel") {
  FakeDriver f;
  AsioDevice dev(fakeSource(f));
  AsioDevice other(fakeSource(f));
  PassEngine eng;
  StreamConfig cfg;
  REQUIRE(dev.open(cfg, {&PassEngine::process, &eng}).ok());
  auto second = other.open(cfg, {&PassEngine::process, &eng});
  REQUIRE_FALSE(second.ok());
  CHECK(second.error().message.find("another ASIO driver") != std::string::npos);
  // Enumerating while a stream is open queries only the open driver.
  auto list = dev.enumerate();
  REQUIRE(list.ok());
  CHECK((*list)[0].probeError.empty());
  CHECK((*list)[0].outputChannels == 2);
  CHECK_FALSE((*list)[1].probeError.empty());
  REQUIRE(dev.controlPanel().ok());
  CHECK(f.panels == 1);
  dev.close();
  REQUIRE(other.open(cfg, {&PassEngine::process, &eng}).ok());  // free again
  other.close();
  CHECK(f.refs == 0);
}

namespace {
struct ChangeLog {
  int calls = 0;
  StreamInfo last;
  static void fn(void* user, const StreamInfo& si) {
    auto& c = *static_cast<ChangeLog*>(user);
    ++c.calls;
    c.last = si;
  }
};
}  // namespace

TEST_CASE("asio host: reset request → stop, release, reload, recreate, restart (off the audio thread)") {
  FakeDriver f;
  AsioDevice dev(fakeSource(f));
  PassEngine eng;
  ChangeLog changes;
  dev.setStreamChangedCallback(&ChangeLog::fn, &changes);
  StreamConfig cfg;
  cfg.bufferFrames = 64;
  REQUIRE(dev.open(cfg, {&PassEngine::process, &eng}).ok());
  REQUIRE(dev.start().ok());
  f.tick(0);
  f.tick(1);
  // The driver asks for a reset from its audio thread: the message only sets a flag (RT-safe).
  int32_t answer = 0;
  CHECK(pq::alloc_trap::countRtViolations([&] { answer = f.cb->message(a::kMsgResetRequest, 0, nullptr, nullptr); }) == 0);
  CHECK(answer == 1);
  CHECK(f.inits == 1);
  dev.handlePendingRequests();
  CHECK(f.stops == 1);
  CHECK(f.disposes == 1);
  CHECK(f.inits == 2);  // released and initialised again
  CHECK(f.creates == 2);
  CHECK(f.starts == 2);
  CHECK(f.refs == 1);
  CHECK(dev.isRunning());
  const AsioEvents ev = dev.events();
  CHECK(ev.resetRequests == 1);
  CHECK(ev.reopens == 1);
  CHECK(ev.failedReopens == 0);
  CHECK(changes.calls == 1);
  CHECK(changes.last.bufferFrames == 64);
  // Same rate and size: the stats keep counting across the reset.
  f.tick(0);
  CHECK(dev.stats().snapshot().callbacks == 3);
  CHECK(eng.blocks == 3);

  // Buffer-size change: handled as a reset; the driver now only offers 128.
  f.minSize = f.maxSize = f.prefSize = 128;
  CHECK(f.cb->message(a::kMsgBufferSizeChange, 128, nullptr, nullptr) == 1);
  dev.handlePendingRequests();
  CHECK(dev.events().bufferSizeChanges == 1);
  CHECK(dev.events().reopens == 2);
  CHECK(dev.streamInfo().bufferFrames == 128);
  CHECK(changes.last.bufferFrames == 128);
  f.tick(1);
  CHECK(eng.lastFrames == 128);
  CHECK(dev.stats().snapshot().callbacks == 1);  // new period: stats restarted
  dev.close();
  CHECK(f.refs == 0);
}

TEST_CASE("asio host: resync, overload, latency change, sample-rate change, failed reopen") {
  FakeDriver f;
  AsioDevice dev(fakeSource(f));
  PassEngine eng;
  ChangeLog changes;
  dev.setStreamChangedCallback(&ChangeLog::fn, &changes);
  StreamConfig cfg;
  cfg.bufferFrames = 64;
  REQUIRE(dev.open(cfg, {&PassEngine::process, &eng}).ok());
  REQUIRE(dev.start().ok());
  CHECK(f.cb->message(a::kMsgResyncRequest, 0, nullptr, nullptr) == 1);
  CHECK(f.cb->message(a::kMsgOverload, 0, nullptr, nullptr) == 1);
  CHECK(f.cb->message(999, 0, nullptr, nullptr) == 0);
  dev.handlePendingRequests();
  CHECK(dev.events().resyncRequests == 1);
  CHECK(dev.events().overloads == 1);
  CHECK(dev.events().reopens == 0);  // nothing to recreate for those

  f.outLatency = 333;
  f.inLatency = 222;
  CHECK(f.cb->message(a::kMsgLatenciesChanged, 0, nullptr, nullptr) == 1);
  dev.handlePendingRequests();
  CHECK(dev.streamInfo().latencyFrames == 333);
  CHECK(dev.streamInfo().inputLatencyFrames == 222);
  CHECK(dev.events().reopens == 0);

  // External clock moved the driver to 44.1 kHz: the stream follows the driver's new rate.
  f.rate = 44100;
  CHECK(pq::alloc_trap::countRtViolations([&] { f.cb->sampleRateDidChange(44100); }) == 0);
  dev.handlePendingRequests();
  CHECK(dev.events().rateChanges == 1);
  CHECK(dev.events().reopens == 1);
  CHECK(dev.streamInfo().sampleRate == 44100);
  CHECK(changes.last.sampleRate == 44100);
  CHECK(dev.isRunning());
  f.tick(0);
  CHECK(dev.stats().snapshot().callbacks == 1);

  // Device unplugged: the reset's reload fails → the stream closes with an error, no crash.
  f.failInit = true;
  f.cb->message(a::kMsgResetRequest, 0, nullptr, nullptr);
  dev.handlePendingRequests();
  CHECK(dev.events().failedReopens == 1);
  CHECK_FALSE(dev.isOpen());
  CHECK_FALSE(dev.isRunning());
  CHECK(dev.lastError().find("fake hardware unplugged") != std::string::npos);
  CHECK(f.refs == 0);
  dev.close();
}

TEST_CASE("asio host: requests are picked up by the control thread's poll") {
  FakeDriver f;
  AsioDevice dev(fakeSource(f));
  PassEngine eng;
  REQUIRE(dev.open(StreamConfig{}, {&PassEngine::process, &eng}).ok());
  REQUIRE(dev.start().ok());
  f.cb->message(a::kMsgResetRequest, 0, nullptr, nullptr);
  const auto t0 = std::chrono::steady_clock::now();
  while (dev.events().reopens == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(dev.events().reopens == 1);
  CHECK(dev.isRunning());
  dev.close();
}

TEST_CASE("asio host: driver audio thread runs callbacks concurrently with stop()") {
  FakeDriver f;
  AsioDevice dev(fakeSource(f));
  PassEngine eng;
  StreamConfig cfg;
  cfg.bufferFrames = 32;
  REQUIRE(dev.open(cfg, {&PassEngine::process, &eng}).ok());
  REQUIRE(dev.start().ok());
  std::atomic<bool> quit{false};
  std::thread driverThread([&] {
    int32_t h = 0;
    while (!quit.load()) {
      f.tick(h);
      h ^= 1;
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(dev.stop().ok());
  const uint64_t after = eng.blocks.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(eng.blocks.load() == after);  // no engine callback after stop() returned
  quit = true;
  driverThread.join();
  CHECK(after > 0);
  dev.close();
}

TEST_CASE("asio device (opt-in, PQ_TEST_AUDIO_DEVICE=1): a real driver plays silence") {
  const char* v = std::getenv("PQ_TEST_AUDIO_DEVICE");
  if (!v || std::string(v) != "1") {
    MESSAGE("skipped: set PQ_TEST_AUDIO_DEVICE=1 (and optionally PQ_TEST_ASIO_DRIVER=<name>) to open an ASIO driver");
    return;
  }
  AsioDevice dev;
  auto list = dev.enumerate();
  REQUIRE(list.ok());
  for (const DeviceInfo& d : *list)
    MESSAGE(d.id << " " << d.name << ": " << (d.probeError.empty() ? d.sampleTypeName : d.probeError));
  // Silence, plus the TSC at every callback (preallocated) for an interval histogram.
  struct Silence {
    std::vector<uint64_t> tsc = std::vector<uint64_t>(1 << 16);
    std::atomic<uint32_t> n{0};
    int priority = 0;  // the driver thread's priority (diagnostic; read once, on the first callback)
    static void process(void* user, const AudioBlock& b) noexcept {  // RT-safe
      auto& s = *static_cast<Silence*>(user);
      const uint32_t i = s.n.load(std::memory_order_relaxed);
      if (i == 0) s.priority = GetThreadPriority(GetCurrentThread());  // test diagnostic only (a syscall)
      if (i < s.tsc.size()) {
        s.tsc[i] = readTsc();
        s.n.store(i + 1, std::memory_order_relaxed);
      }
      for (uint32_t c = 0; c < b.numOut; ++c) std::memset(b.out[c], 0, sizeof(float) * b.frames);
    }
  } silence;
  StreamConfig cfg;
  if (const char* n = std::getenv("PQ_TEST_ASIO_DRIVER")) cfg.outputEndpointId = n;
  cfg.bufferFrames = 64;
  if (const char* f = std::getenv("PQ_TEST_ASIO_BUFFER")) cfg.bufferFrames = static_cast<uint32_t>(std::atoi(f));
  cfg.inputChannels = 2;  // duplex where the driver has inputs
  auto info = dev.open(cfg, {&Silence::process, &silence});
  if (!info && info.error().code == pq::ErrorCode::InvalidArgument) {
    cfg.inputChannels = 0;
    info = dev.open(cfg, {&Silence::process, &silence});
  }
  if (!info) {
    MESSAGE("no ASIO driver could be opened: " << info.error().message);
    return;
  }
  MESSAGE("opened " << info->outputDeviceName << " (" << info->outputChannels << " out, " << info->inputChannels
                    << " in) at " << info->sampleRate << " Hz, " << info->bufferFrames
                    << " frames, latency out " << info->latencyFrames << " in " << info->inputLatencyFrames);
  REQUIRE(dev.start().ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(3000));
  REQUIRE(dev.stop().ok());
  const CallbackStats::Snapshot st = dev.stats().snapshot();
  dev.close();
  MESSAGE("callbacks " << st.callbacks << ", underflows " << st.outputUnderflows << ", max interval "
                       << st.maxIntervalUs << " us");
  CHECK(st.callbacks > 0);
  // Interval histogram (250 us bins) and the first intervals, to tell driver scheduling from host problems.
  const double usPerTick = 1e6 / tscFrequencyHz();
  const uint32_t n = std::min<uint32_t>(silence.n.load(), static_cast<uint32_t>(silence.tsc.size()));
  std::vector<uint32_t> bins(40, 0);
  std::string first;
  for (uint32_t i = 1; i < n; ++i) {
    const double us = static_cast<double>(silence.tsc[i] - silence.tsc[i - 1]) * usPerTick;
    ++bins[std::min<size_t>(static_cast<size_t>(us / 250.0), bins.size() - 1)];
    if (i < 24) first += std::to_string(static_cast<int>(us)) + " ";
  }
  std::string hist;
  for (size_t b = 0; b < bins.size(); ++b)
    if (bins[b]) hist += "[" + std::to_string(b * 250) + "," + std::to_string((b + 1) * 250) + ") us: " + std::to_string(bins[b]) + "\n";
  MESSAGE("driver thread priority " << silence.priority << "; first intervals (us): " << first << "\n" << hist);
}

// AU.stream: the stream's flight recorder.
TEST_CASE("stream trace: RT-safe push from several threads, in order, overwriting the oldest; the ASIO host records "
          "starts, switches (half, position, filled, output samples), messages, a position jump and the stop") {
  StreamTrace& tr = streamTrace();
  {  // RT-safe: no allocation inside an RtScope.
    const uint64_t h0 = tr.head();
    CHECK(pq::alloc_trap::countRtViolations([&] {
            pq::RtScope rt;
            for (int i = 0; i < 100; ++i) tr.push(TraceKind::Buffer, kBufFilled, i & 1, i * 64, 0.5f, -0.25f, 0.125f);
          }) == 0);
    std::vector<TraceEvent> ev;
    uint64_t lost = 1;
    CHECK(tr.read(h0, ev, &lost) == h0 + 100);
    CHECK(lost == 0);
    REQUIRE(ev.size() == 100);
    for (int i = 0; i < 100; ++i) {
      CHECK(ev[i].seq == h0 + i);
      CHECK(ev[i].kind == TraceKind::Buffer);
      CHECK(ev[i].a == (i & 1));
      CHECK(ev[i].b == i * 64);
      CHECK(ev[i].x == 0.5f);
      CHECK(ev[i].y == -0.25f);
      CHECK(ev[i].z == 0.125f);
      if (i) CHECK(ev[i].tsc >= ev[i - 1].tsc);
    }
  }
  {  // Four writers at once: every event arrives once, intact.
    const uint64_t h0 = tr.head();
    std::vector<std::thread> w;
    for (int t = 0; t < 4; ++t)
      w.emplace_back([&tr, t] {
        for (int i = 0; i < 1000; ++i) tr.push(TraceKind::Message, 0, t, i, static_cast<float>(t * 1000 + i));
      });
    for (auto& t : w) t.join();
    std::vector<TraceEvent> ev;
    (void)tr.read(h0, ev);
    REQUIRE(ev.size() == 4000);
    std::vector<int> next(4, 0);
    for (const TraceEvent& e : ev) {
      REQUIRE(e.a >= 0);
      REQUIRE(e.a < 4);
      CHECK(e.b == next[e.a]);  // each writer's events in its own order
      CHECK(e.x == static_cast<float>(e.a * 1000 + e.b));
      ++next[e.a];
    }
  }
  {  // Overwrite: a reader that fell behind by more than the capacity gets the newest kCapacity and a lost count.
    const uint64_t h0 = tr.head();
    for (uint32_t i = 0; i < StreamTrace::kCapacity + 10; ++i) tr.push(TraceKind::Buffer, 0, 0, i);
    std::vector<TraceEvent> ev;
    uint64_t lost = 0;
    (void)tr.read(h0, ev, &lost);
    CHECK(lost == 10);
    REQUIRE(ev.size() == StreamTrace::kCapacity);
    CHECK(ev.front().b == 10);
  }
  // The host path (fake driver): start, switches with the half / position / samples, a resync message, a jump.
  FakeDriver f;
  f.outType = a::kFloat32Lsb;
  AsioDevice dev(fakeSource(f));
  PassEngine eng;
  StreamConfig cfg;
  cfg.outputChannels = 2;
  cfg.inputChannels = 2;
  cfg.bufferFrames = 64;
  REQUIRE(dev.open(cfg, {&PassEngine::process, &eng}).ok());
  const uint64_t h0 = tr.head();
  REQUIRE(dev.start().ok());
  for (int k = 0; k < 4; ++k) {
    writeInput(f, k & 1, 64, 0.1f);
    CHECK(pq::alloc_trap::countRtViolations([&] { f.tick(k & 1, true, k == 2 ? 192 : -1); }) == 0);
  }
  CHECK(f.cb->message(a::kMsgResyncRequest, 0, nullptr, nullptr) == 1);
  REQUIRE(dev.stop().ok());
  f.tick(0);  // after the stop: recorded as not filled
  std::vector<TraceEvent> ev;
  (void)tr.read(h0, ev);
  std::vector<TraceKind> kinds;
  for (const TraceEvent& e : ev) kinds.push_back(e.kind);
  const std::vector<TraceKind> want = {TraceKind::Start,  TraceKind::Started, TraceKind::Buffer,
                                       TraceKind::Buffer, TraceKind::Buffer,  TraceKind::Buffer,
                                       TraceKind::Message, TraceKind::Stop,   TraceKind::Stopped, TraceKind::Buffer};
  REQUIRE(kinds == want);
  CHECK(ev[2].a == 0);
  CHECK(ev[2].b == 0);
  CHECK(ev[3].a == 1);
  CHECK(ev[3].b == 64);
  CHECK(ev[5].b == 128 + 192);  // the third switch advanced by 192: the fourth sees the jump
  CHECK((ev[5].flags & kBufMissed) != 0);
  for (int i = 2; i < 6; ++i) {
    CHECK((ev[i].flags & kBufFilled) != 0);
    CHECK((ev[i].flags & kBufTimeInfo) != 0);
    // PassEngine: out = in · 0.5 (+ c · 0.125): output 1 = 0.1·0.5 + 0.001·i·0.5 at sample i.
    CHECK(ev[i].y == doctest::Approx(0.05f));
    CHECK(ev[i].z == doctest::Approx(0.05f + 0.0005f * 63));
    CHECK(ev[i].x == doctest::Approx(0.05f + 0.0005f * 63));
  }
  CHECK(ev[6].a == a::kMsgResyncRequest);
  CHECK((ev[9].flags & kBufFilled) == 0);
  TraceFormat fmt;
  fmt.frames = 64;  // the Open event came before this window
  fmt.rate = 48000;
  const std::string text = describeStreamTrace(ev, 0, fmt);
  INFO(text);
  CHECK(text.find("POSITION-STEP 192") != std::string::npos);
  CHECK(text.find("MISSED") != std::string::npos);
  CHECK(text.find("resyncRequest") != std::string::npos);
  CHECK(text.find("NOT-FILLED") != std::string::npos);
  dev.close();
}

// AU.stream: what a real driver does when the stream restarts after an idle suspend (the app's old behaviour: stop
// after 1 s of engine sleep, start on Play). Opt-in: PQ_TEST_ASIO_PROBE=1 (PQ_TEST_ASIO_DRIVER=<name>, default "FL
// Studio ASIO"; PQ_TEST_ASIO_IDLE_MS, default 10000). Plays silence only; prints the flight recorder around both
// starts, every buffer of the first 500 ms after the resume.
TEST_CASE("asio probe (opt-in, PQ_TEST_ASIO_PROBE=1): a real driver's restart after an idle suspend") {
  const char* v = std::getenv("PQ_TEST_ASIO_PROBE");
  if (!v || std::string(v) != "1") {
    MESSAGE("skipped: set PQ_TEST_ASIO_PROBE=1 to restart a real ASIO driver (silence)");
    return;
  }
  struct Silence {
    static void process(void*, const AudioBlock& b) noexcept {  // RT-safe
      for (uint32_t c = 0; c < b.numOut; ++c) std::memset(b.out[c], 0, sizeof(float) * b.frames);
    }
  };
  AsioDevice dev;
  StreamConfig cfg;
  const char* n = std::getenv("PQ_TEST_ASIO_DRIVER");
  cfg.outputEndpointId = n ? n : "FL Studio ASIO";
  cfg.bufferFrames = 256;  // what the app asks for
  if (const char* f = std::getenv("PQ_TEST_ASIO_BUFFER")) cfg.bufferFrames = static_cast<uint32_t>(std::atoi(f));
  cfg.outputChannels = 2;
  cfg.inputChannels = 0;
  cfg.sampleRate = 48000;  // the app's engine rate
  if (const char* r = std::getenv("PQ_TEST_ASIO_RATE")) cfg.sampleRate = static_cast<uint32_t>(std::atoi(r));
  int idleMs = 10000;
  if (const char* i = std::getenv("PQ_TEST_ASIO_IDLE_MS")) idleMs = std::atoi(i);
  StreamTrace& tr = streamTrace();
  const uint64_t c0 = tr.head();
  auto info = dev.open(cfg, {&Silence::process, nullptr});
  REQUIRE_MESSAGE(info.ok(), (info.ok() ? std::string() : info.error().message));
  MESSAGE("opened " << info->outputDeviceName << " at " << info->sampleRate << " Hz, " << info->bufferFrames
                    << " frames, output latency " << info->latencyFrames);
  REQUIRE(dev.start().ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(3000));
  REQUIRE(dev.stop().ok());  // the idle suspend
  std::vector<TraceEvent> first;
  TraceFormat fmt;
  fmt.buffersAfterStart = 24;
  const uint64_t c1 = tr.read(c0, first);
  MESSAGE("first run:\n" << describeStreamTrace(first, 0, fmt));
  std::this_thread::sleep_for(std::chrono::milliseconds(idleMs));
  REQUIRE(dev.start().ok());  // Play after the idle
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  REQUIRE(dev.stop().ok());
  std::vector<TraceEvent> resume;
  (void)tr.read(c1, resume);
  // Every buffer of the first 500 ms after the resume, then the summary.
  REQUIRE_FALSE(resume.empty());
  const double ticksPerMs = tscFrequencyHz() / 1000.0;
  std::vector<TraceEvent> early, late;
  for (const TraceEvent& e : resume)
    (static_cast<double>(e.tsc - resume.front().tsc) < 500.0 * ticksPerMs ? early : late).push_back(e);
  TraceFormat all = fmt;
  all.allBuffers = true;
  const std::string earlyText = describeStreamTrace(early, resume.front().tsc, all);
  TraceFormat rest = all;
  rest.allBuffers = false;
  MESSAGE("resume after " << idleMs << " ms idle, first 500 ms:\n"
                          << earlyText << "rest:\n"
                          << describeStreamTrace(late, resume.front().tsc, rest));
  const AsioEvents e = dev.events();
  MESSAGE("driver messages: resets " << e.resetRequests << ", buffer-size " << e.bufferSizeChanges << ", resyncs "
                                     << e.resyncRequests << ", latencies " << e.latencyChanges << ", rate "
                                     << e.rateChanges << ", overloads " << e.overloads);
  dev.close();
  CHECK_FALSE(dev.isOpen());
}

// AU.stream: what reaches the default render endpoint when a real ASIO driver (FL Studio ASIO is a WASAPI-shared
// wrapper) starts. Plays a -90 dBFS 1500 Hz tone (inaudible: about 0 dB SPL at normal monitor levels) through the
// driver, optionally after `preroll` ms of silence, and records the endpoint's loopback (float, before the DAC) for
// 2 s after each start; the tone's amplitude and phase are measured in 128-sample windows (4 whole cycles), so a
// dropped / repeated / zeroed stretch shows as an amplitude dip or a phase step at its time, and the loopback packets
// the audio engine flags (discontinuity, silent) are timed against the driver start. Skips if anything else is playing
// on the endpoint. Opt-in: PQ_TEST_ASIO_LOOPBACK=1 (PQ_TEST_ASIO_DRIVER, PQ_TEST_ASIO_IDLE_MS as above;
// PQ_TEST_ASIO_PREROLL_MS: comma-separated pre-roll lengths, default "0,0,250,500"; PQ_TEST_ASIO_CAPTURE_MS, 2000).

using pq::test::analyseTone;
using pq::test::ProbeCapture;

TEST_CASE("asio loopback probe (opt-in, PQ_TEST_ASIO_LOOPBACK=1): what the endpoint plays after a driver start") {
  const char* v = std::getenv("PQ_TEST_ASIO_LOOPBACK");
  if (!v || std::string(v) != "1") {
    MESSAGE("skipped: set PQ_TEST_ASIO_LOOPBACK=1 to start a real ASIO driver with a -90 dBFS tone and record its endpoint");
    return;
  }
  static constexpr double kHz = 1500.0;   // 4 cycles per 128 samples at 48 kHz
  static constexpr float kAmp = 3.1623e-5f;  // -90 dBFS
  struct Tone {
    double phase = 0, inc = 0;
    std::atomic<int64_t> silentFrames{0};  // the pre-roll: silence first
    std::atomic<bool> on{false};
    static void process(void* user, const AudioBlock& b) noexcept {  // RT-safe
      auto& t = *static_cast<Tone*>(user);
      const bool on = t.on.load(std::memory_order_relaxed);
      int64_t silent = t.silentFrames.load(std::memory_order_relaxed);
      for (uint32_t f = 0; f < b.frames; ++f) {
        float x = 0.0f;
        if (on && silent > 0) {
          --silent;
        } else if (on) {
          x = kAmp * static_cast<float>(std::sin(t.phase));
          t.phase += t.inc;
          if (t.phase > 6.283185307179586) t.phase -= 6.283185307179586;
        }
        for (uint32_t c = 0; c < b.numOut; ++c) b.out[c][f] = x;
      }
      t.silentFrames.store(silent, std::memory_order_relaxed);
    }
  } tone;
  {  // anything else playing on the endpoint spoils the analysis (and is not ours to record)
    ProbeCapture quiet;
    quiet.run(800, nullptr);
    REQUIRE_MESSAGE(quiet.error.empty(), quiet.error);
    float peak = 0;
    for (float x : quiet.mono) peak = std::max(peak, std::fabs(x));
    if (peak > 1e-6f) {
      MESSAGE("skipped: the default endpoint is playing other audio (peak " << peak << ")");
      return;
    }
  }
  std::vector<int> prerolls = {0, 0, 250, 500};
  if (const char* p = std::getenv("PQ_TEST_ASIO_PREROLL_MS")) {
    prerolls.clear();
    for (const char* c = p; *c;) {
      prerolls.push_back(std::atoi(c));
      while (*c && *c != ',') ++c;
      if (*c == ',') ++c;
    }
  }
  // PQ_TEST_LOOPBACK_BACKEND=wasapi: our WASAPI shared backend on the default endpoint instead (the comparison).
  const char* be = std::getenv("PQ_TEST_LOOPBACK_BACKEND");
  const bool wasapi = be && std::string(be) == "wasapi";
  std::unique_ptr<AudioDevice> devPtr;
  if (wasapi)
    devPtr = std::make_unique<WasapiDevice>();
  else
    devPtr = std::make_unique<AsioDevice>();
  AudioDevice& dev = *devPtr;
  StreamConfig cfg;
  const char* n = std::getenv("PQ_TEST_ASIO_DRIVER");
  if (!wasapi) cfg.outputEndpointId = n ? n : "FL Studio ASIO";
  cfg.bufferFrames = wasapi ? 480 : 256;
  cfg.shareMode = ShareMode::Shared;
  cfg.outputChannels = 2;
  cfg.sampleRate = 48000;
  int idleMs = 10000;
  if (const char* i = std::getenv("PQ_TEST_ASIO_IDLE_MS")) idleMs = std::atoi(i);
  int captureMs = 2000;
  if (const char* c = std::getenv("PQ_TEST_ASIO_CAPTURE_MS")) captureMs = std::atoi(c);
  auto info = dev.open(cfg, {&Tone::process, &tone});
  REQUIRE_MESSAGE(info.ok(), (info.ok() ? std::string() : info.error().message));
  tone.inc = 6.283185307179586 * kHz / info->sampleRate;
  StreamTrace& tr = streamTrace();
  for (size_t run = 0; run < prerolls.size(); ++run) {
    // The first start, then each after idleMs with the stream stopped (the app's old idle suspend).
    if (run > 0) std::this_thread::sleep_for(std::chrono::milliseconds(idleMs));
    tone.phase = 0;
    tone.silentFrames = static_cast<int64_t>(prerolls[run]) * info->sampleRate / 1000;
    tone.on = true;
    const uint64_t c0 = tr.head();
    ProbeCapture cap;
    bool started = false;
    cap.run(captureMs, [&] { started = dev.start().ok(); });
    REQUIRE(started);
    REQUIRE(dev.stop().ok());
    tone.on = false;
    REQUIRE_MESSAGE(cap.error.empty(), cap.error);
    std::vector<TraceEvent> ev;
    (void)tr.read(c0, ev);
    uint32_t buffers = 0;
    for (const TraceEvent& e : ev) buffers += e.kind == TraceKind::Buffer;
    int anomalies = 0;
    const std::string analysis = analyseTone(cap, kHz, kAmp, &anomalies);
    TraceFormat fmt;
    fmt.frames = info->bufferFrames;
    fmt.rate = info->sampleRate;
    fmt.buffersAfterStart = 12;
    const std::string label = run == 0 ? std::string("first start") : "after " + std::to_string(idleMs) + " ms stopped";
    MESSAGE("run " << run << " (" << label << ", pre-roll " << prerolls[run] << " ms): " << buffers
                   << " buffer switches; endpoint " << cap.rate << " Hz x " << cap.channels << "\n"
                   << analysis << describeStreamTrace(ev, 0, fmt));
  }
  dev.close();
  CHECK_FALSE(dev.isOpen());
}
