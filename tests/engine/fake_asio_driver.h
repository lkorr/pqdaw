#pragma once

// A fake ASIO driver implementing the driver ABI (pq/engine/asio_abi.h), driven by hand from a test: double-buffer
// switching (tick), the host callbacks it was given (cb), configurable channels / sample types / buffer-size rules /
// rates / latencies, and the driver requests (reset, rate change) a real driver sends. Shared by the ASIO host tests
// (tests/engine/test_asio.cpp, T0.6-fix2) and the audio output's driver-reset tests (tests/app, T3.18).

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pq/engine/asio_abi.h"
#include "pq/engine/asio_convert.h"
#include "pq/engine/asio_device.h"

namespace pq::test {

namespace a = pq::engine::asio;
using pq::engine::AsioDriverEntry;
using pq::engine::AsioDriverSource;
using pq::engine::asioSampleTypeBytes;

class FakeDriver final : public a::Driver {
public:
  // Configuration (set before open).
  int32_t numIn = 2, numOut = 2;
  int32_t inType = a::kInt32Lsb, outType = a::kInt32Lsb;
  int32_t minSize = 32, maxSize = 1024, prefSize = 256, granularity = -1;
  double rate = 48000;
  std::vector<double> rates = {44100, 48000, 96000};
  int32_t inLatency = 100, outLatency = 200;
  bool supportsOutputReady = true;
  bool failInit = false;
  bool sendRateChangeOnSet = true;  // like many drivers: sampleRateDidChange in answer to setSampleRate

  // Observations.
  std::atomic<int> refs{0};
  int inits = 0, creates = 0, disposes = 0, starts = 0, stops = 0, panels = 0, outputReadies = 0, setRates = 0;
  int32_t lastBufferSize = 0;
  bool started = false;
  const a::HostCallbacks* cb = nullptr;
  std::vector<a::BufferInfo> infos;
  std::vector<std::vector<uint8_t>> storage;  // [channel*2 + half]
  int64_t position = 0;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** p) override {
    *p = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++refs); }
  ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(--refs); }

  a::Bool init(void*) override {
    ++inits;
    return failInit ? 0 : 1;
  }
  void getDriverName(char* name) override { std::strcpy(name, "Fake"); }
  int32_t getDriverVersion() override { return 7; }
  void getErrorMessage(char* text) override { std::strcpy(text, "fake hardware unplugged"); }
  a::Status start() override {
    ++starts;
    started = true;
    position = 0;
    return a::kOk;
  }
  a::Status stop() override {
    ++stops;
    started = false;
    return a::kOk;
  }
  a::Status getChannels(int32_t* i, int32_t* o) override {
    *i = numIn;
    *o = numOut;
    return a::kOk;
  }
  a::Status getLatencies(int32_t* i, int32_t* o) override {
    *i = inLatency;
    *o = outLatency;
    return a::kOk;
  }
  a::Status getBufferSize(int32_t* mn, int32_t* mx, int32_t* pref, int32_t* g) override {
    *mn = minSize;
    *mx = maxSize;
    *pref = prefSize;
    *g = granularity;
    return a::kOk;
  }
  a::Status canSampleRate(double r) override {
    for (double x : rates)
      if (x == r) return a::kOk;
    return a::kNoClock;
  }
  a::Status getSampleRate(double* r) override {
    *r = rate;
    return a::kOk;
  }
  a::Status setSampleRate(double r) override {
    if (canSampleRate(r) != a::kOk) return a::kNoClock;
    ++setRates;
    rate = r;
    if (sendRateChangeOnSet && cb) cb->sampleRateDidChange(r);
    return a::kOk;
  }
  a::Status getClockSources(a::ClockSource* c, int32_t* n) override {
    if (*n < 1) return a::kInvalidParameter;
    c[0] = a::ClockSource{};
    std::strcpy(c[0].name, "Internal");
    c[0].isCurrentSource = 1;
    *n = 1;
    return a::kOk;
  }
  a::Status setClockSource(int32_t) override { return a::kOk; }
  a::Status getSamplePosition(a::Int64Halves* pos, a::Int64Halves* ts) override {
    pos->hi = static_cast<uint32_t>(static_cast<uint64_t>(position) >> 32);
    pos->lo = static_cast<uint32_t>(position);
    *ts = a::Int64Halves{};
    return a::kOk;
  }
  a::Status getChannelInfo(a::ChannelInfo* ci) override {
    if (ci->channel < 0 || ci->channel >= (ci->isInput ? numIn : numOut)) return a::kInvalidParameter;
    ci->isActive = 0;
    ci->channelGroup = 0;
    ci->type = ci->isInput ? inType : outType;
    std::snprintf(ci->name, sizeof ci->name, "%s %d", ci->isInput ? "In" : "Out", ci->channel + 1);
    return a::kOk;
  }
  a::Status createBuffers(a::BufferInfo* bi, int32_t n, int32_t size, const a::HostCallbacks* callbacks) override {
    if (size < minSize || size > maxSize) return a::kInvalidMode;
    ++creates;
    lastBufferSize = size;
    cb = callbacks;
    infos.assign(bi, bi + n);
    storage.assign(static_cast<size_t>(n) * 2, {});
    for (int32_t k = 0; k < n; ++k) {
      if (bi[k].channelNum >= (bi[k].isInput ? numIn : numOut)) return a::kInvalidParameter;
      const uint32_t bytes = asioSampleTypeBytes(bi[k].isInput ? inType : outType);
      for (int h = 0; h < 2; ++h) {
        // Garbage-filled, so the test sees that the host zeroes output halves.
        storage[static_cast<size_t>(k) * 2 + h].assign(static_cast<size_t>(size) * bytes, 0x5A);
        bi[k].buffers[h] = storage[static_cast<size_t>(k) * 2 + h].data();
      }
      infos[static_cast<size_t>(k)] = bi[k];
    }
    // Drivers may ask about host capabilities during createBuffers.
    CHECK(callbacks->message(a::kMsgSupportsTimeInfo, 0, nullptr, nullptr) == 1);
    CHECK(callbacks->message(a::kMsgSelectorSupported, a::kMsgResetRequest, nullptr, nullptr) == 1);
    CHECK(callbacks->message(a::kMsgSelectorSupported, a::kMsgSupportsTimeCode, nullptr, nullptr) == 0);
    CHECK(callbacks->message(a::kMsgEngineVersion, 0, nullptr, nullptr) == 2);
    return a::kOk;
  }
  a::Status disposeBuffers() override {
    ++disposes;
    infos.clear();
    storage.clear();
    return a::kOk;
  }
  a::Status controlPanel() override {
    ++panels;
    return a::kOk;
  }
  a::Status future(int32_t, void*) override { return a::kInvalidParameter; }
  a::Status outputReady() override {
    if (!supportsOutputReady) return a::kNotPresent;
    if (started) ++outputReadies;
    return a::kOk;
  }

  // --- test driving ---
  void* buffer(bool input, int32_t ch, int32_t half) {
    for (size_t k = 0; k < infos.size(); ++k)
      if ((infos[k].isInput != 0) == input && infos[k].channelNum == ch) return infos[k].buffers[half];
    return nullptr;
  }
  // One buffer switch on the "driver thread" (the calling thread), with time info unless timeInfo = false.
  void tick(int32_t half, bool timeInfo = true, int64_t advance = -1) {
    a::Time t{};
    t.timeInfo.flags = a::kSamplePositionValid | a::kSampleRateValid;
    t.timeInfo.samplePosition.hi = static_cast<uint32_t>(static_cast<uint64_t>(position) >> 32);
    t.timeInfo.samplePosition.lo = static_cast<uint32_t>(position);
    t.timeInfo.sampleRate = rate;
    if (timeInfo)
      cb->bufferSwitchTimeInfo(&t, half, 1);
    else
      cb->bufferSwitch(half, 1);
    position += advance < 0 ? lastBufferSize : advance;
  }
};

inline AsioDriverSource fakeSource(FakeDriver& f, std::vector<std::string> names = {"Fake ASIO", "Other ASIO"}) {
  AsioDriverSource s;
  s.list = [names] {
    std::vector<AsioDriverEntry> v;
    for (const std::string& n : names) v.push_back({n, "{00000000-0000-0000-0000-000000000000}"});
    return v;
  };
  s.create = [&f](const AsioDriverEntry& e) -> pq::Result<a::Driver*> {
    if (e.name != "Fake ASIO") return pq::Error{pq::ErrorCode::Device, "no such fake driver: " + e.name};
    f.AddRef();
    return static_cast<a::Driver*>(&f);
  };
  return s;
}

}  // namespace pq::test
