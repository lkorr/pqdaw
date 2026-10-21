#pragma once

// ASIO driver ABI, as seen by a host (T0.6-fix2).
//
// This is an independent reimplementation of the binary interface between an ASIO host and an ASIO driver,
// written for interoperability with the drivers installed on a user's machine. It is not derived from, and
// contains no part of, Steinberg's ASIO SDK (no SDK file was copied, downloaded or consulted): names here are
// our own, and only the binary layout (vtable slot order, struct layouts, numeric constants), which every ASIO
// driver must implement to work with any host, is reproduced. ASIO is a trademark of Steinberg Media
// Technologies GmbH. Licensing position: docs/QUESTIONS.md (Q?-T0.6-fix2-a), docs/DEPENDENCIES.md.
//
// The layout is the 64-bit Windows one only (MSVC x64): there, the C++ member functions of the driver object
// use the one x64 calling convention, so a COM-style class with virtual functions in the right order is the
// whole contract. (32-bit hosts needed a thiscall workaround; pqdaw is x64-only.) `long` is 32 bits on
// Windows, so the ABI's 32-bit integers are int32_t here. Validated against real drivers (Focusrite USB ASIO,
// ASIO4ALL v2, FL Studio ASIO, Voicemeeter Virtual ASIO): see docs/PROGRESS.md, T0.6-fix2.
//
// Driver discovery: each driver registers HKEY_LOCAL_MACHINE\SOFTWARE\ASIO\<driver name> with a string value
// "CLSID" (and usually "Description"). The host creates the in-process COM object with CoCreateInstance,
// passing the CLSID both as the class id and as the interface id (ASIO drivers have no separate IID), on a
// thread in a single-threaded apartment. Only one driver can be loaded per process: the host callbacks carry
// no context pointer.

#include <cstddef>
#include <cstdint>

#include <unknwn.h>

#if !defined(_WIN64)
#error "pq/engine/asio_abi.h describes the x64 Windows ASIO ABI only"
#endif

namespace pq::engine::asio {

using Bool = int32_t;    // 0 = false, non-zero = true
using Status = int32_t;  // result codes below

// Result codes returned by driver methods.
inline constexpr Status kOk = 0;                     // the call succeeded
inline constexpr Status kSuccess = 0x3f4847a0;       // future(): the selector is supported and succeeded
inline constexpr Status kNotPresent = -1000;         // hardware input or output not present / not available
inline constexpr Status kHwMalfunction = -999;       // hardware malfunction
inline constexpr Status kInvalidParameter = -998;    // an input parameter is invalid
inline constexpr Status kInvalidMode = -997;         // the hardware is in a bad mode or used in a bad mode
inline constexpr Status kSpNotAdvancing = -996;      // the hardware's sample position is not advancing
inline constexpr Status kNoClock = -995;             // the sample clock or rate cannot be determined / not present
inline constexpr Status kNoMemory = -994;            // not enough memory for the request

// A 64-bit count split into two 32-bit halves, high half first (sample positions, nanosecond time stamps).
struct Int64Halves {
  uint32_t hi = 0;
  uint32_t lo = 0;
};
inline int64_t toInt64(const Int64Halves& v) noexcept {
  return static_cast<int64_t>((static_cast<uint64_t>(v.hi) << 32) | v.lo);
}

// Sample types of a channel's buffer (ChannelInfo::type). LSB = little-endian. The "Int32Lsb<N>" types hold an
// N-bit sample right-justified (sign-extended) in a 32-bit container. Big-endian types do not occur on Windows
// drivers in practice and are not supported by pqdaw (open() reports them).
enum SampleType : int32_t {
  kInt16Msb = 0,
  kInt24Msb = 1,
  kInt32Msb = 2,
  kFloat32Msb = 3,
  kFloat64Msb = 4,
  kInt32Msb16 = 8,
  kInt32Msb18 = 9,
  kInt32Msb20 = 10,
  kInt32Msb24 = 11,
  kInt16Lsb = 16,
  kInt24Lsb = 17,  // packed: 3 bytes per sample
  kInt32Lsb = 18,
  kFloat32Lsb = 19,
  kFloat64Lsb = 20,
  kInt32Lsb16 = 24,
  kInt32Lsb18 = 25,
  kInt32Lsb20 = 26,
  kInt32Lsb24 = 27,
  kDsdInt8Lsb1 = 32,
  kDsdInt8Msb1 = 33,
  kDsdInt8Ner8 = 40,
};

// getClockSources(): one entry per clock source.
struct ClockSource {
  int32_t index = 0;
  int32_t associatedChannel = 0;
  int32_t associatedGroup = 0;
  Bool isCurrentSource = 0;
  char name[32] = {};
};

// getChannelInfo(): the host fills channel and isInput, the driver the rest.
struct ChannelInfo {
  int32_t channel = 0;
  Bool isInput = 0;
  Bool isActive = 0;
  int32_t channelGroup = 0;
  int32_t type = 0;  // SampleType
  char name[32] = {};
};

// createBuffers(): one entry per channel the host wants. The host fills isInput and channelNum; the driver
// fills buffers[0] and buffers[1], the two halves of the channel's double buffer, each bufferSize samples.
struct BufferInfo {
  Bool isInput = 0;
  int32_t channelNum = 0;
  void* buffers[2] = {nullptr, nullptr};
};

// Flags of TimeInfo::flags.
inline constexpr uint32_t kSystemTimeValid = 1u << 0;
inline constexpr uint32_t kSamplePositionValid = 1u << 1;
inline constexpr uint32_t kSampleRateValid = 1u << 2;
inline constexpr uint32_t kSpeedValid = 1u << 3;
inline constexpr uint32_t kSampleRateChanged = 1u << 4;
inline constexpr uint32_t kClockSourceChanged = 1u << 5;

struct TimeInfo {
  double speed = 0;             // 1.0 = nominal
  Int64Halves systemTime;       // nanoseconds, the system time of the buffer's first sample
  Int64Halves samplePosition;   // samples since start() of the buffer's first sample
  double sampleRate = 0;
  uint32_t flags = 0;
  char reserved[12] = {};
};

struct TimeCode {
  double speed = 0;
  Int64Halves timeCodeSamples;
  uint32_t flags = 0;
  char future[64] = {};
};

// Passed to the host's bufferSwitchTimeInfo callback.
struct Time {
  int32_t reserved[4] = {};
  TimeInfo timeInfo;
  TimeCode timeCode;
};

// Host callbacks, handed to the driver in createBuffers(). Called on the driver's own thread(s) (bufferSwitch*
// on its audio thread; messages on any thread, possibly from inside a driver method the host called).
struct HostCallbacks {
  // Process half `index` (0 or 1) of every channel's double buffer. `directProcess` is informational.
  void (*bufferSwitch)(int32_t index, Bool directProcess) noexcept;
  // The driver's sample rate changed (e.g. a new external clock).
  void (*sampleRateDidChange)(double sampleRate) noexcept;
  // Generic messages (selectors below); returns a selector-specific answer, 0 = not supported.
  int32_t (*message)(int32_t selector, int32_t value, void* data, double* opt) noexcept;
  // As bufferSwitch, plus timing for the buffer; used when the host answers kMsgSupportsTimeInfo with 1.
  // Returns the Time pointer or nullptr (drivers ignore it).
  Time* (*bufferSwitchTimeInfo)(Time* params, int32_t index, Bool directProcess) noexcept;
};

// Message selectors (HostCallbacks::message).
inline constexpr int32_t kMsgSelectorSupported = 1;   // value = a selector: does the host support it?
inline constexpr int32_t kMsgEngineVersion = 2;       // host's supported ASIO interface version (answer 2)
inline constexpr int32_t kMsgResetRequest = 3;        // the driver needs the host to stop, release and reload it
inline constexpr int32_t kMsgBufferSizeChange = 4;    // value = new buffer size; return 1 if handled (else reset)
inline constexpr int32_t kMsgResyncRequest = 5;       // the driver lost samples (dropouts): resync timing
inline constexpr int32_t kMsgLatenciesChanged = 6;    // re-query getLatencies()
inline constexpr int32_t kMsgSupportsTimeInfo = 7;    // does the host use bufferSwitchTimeInfo?
inline constexpr int32_t kMsgSupportsTimeCode = 8;
inline constexpr int32_t kMsgMmcCommand = 9;
inline constexpr int32_t kMsgSupportsInputMonitor = 10;
inline constexpr int32_t kMsgSupportsInputGain = 11;
inline constexpr int32_t kMsgSupportsInputMeter = 12;
inline constexpr int32_t kMsgSupportsOutputGain = 13;
inline constexpr int32_t kMsgSupportsOutputMeter = 14;
inline constexpr int32_t kMsgOverload = 15;           // the driver detected an overload

// The driver object (vtable order is the ABI). Methods are called from the thread that created the object,
// except where noted.
struct Driver : public IUnknown {
  virtual Bool init(void* systemHandle) = 0;  // systemHandle: an HWND owned by the host; true on success
  virtual void getDriverName(char* name) = 0;  // writes up to 32 chars including the terminator
  virtual int32_t getDriverVersion() = 0;
  virtual void getErrorMessage(char* text) = 0;  // writes up to 124 chars including the terminator
  virtual Status start() = 0;
  virtual Status stop() = 0;
  virtual Status getChannels(int32_t* numInputs, int32_t* numOutputs) = 0;
  virtual Status getLatencies(int32_t* inputLatency, int32_t* outputLatency) = 0;  // samples
  // granularity: -1 = powers of two between min and max; 0 = only preferred; n > 0 = min + k·n
  virtual Status getBufferSize(int32_t* minSize, int32_t* maxSize, int32_t* preferredSize, int32_t* granularity) = 0;
  virtual Status canSampleRate(double sampleRate) = 0;
  virtual Status getSampleRate(double* sampleRate) = 0;
  virtual Status setSampleRate(double sampleRate) = 0;
  virtual Status getClockSources(ClockSource* clocks, int32_t* numSources) = 0;  // in: capacity, out: count
  virtual Status setClockSource(int32_t index) = 0;
  // May be called from the host's bufferSwitch (driver audio thread).
  virtual Status getSamplePosition(Int64Halves* samplePosition, Int64Halves* systemTimeNs) = 0;
  virtual Status getChannelInfo(ChannelInfo* info) = 0;
  virtual Status createBuffers(BufferInfo* infos, int32_t numChannels, int32_t bufferSize,
                               const HostCallbacks* callbacks) = 0;
  virtual Status disposeBuffers() = 0;
  virtual Status controlPanel() = 0;
  virtual Status future(int32_t selector, void* opt) = 0;
  // The host has written the output half for this buffer switch (lets the driver send it early); called from the
  // host's bufferSwitch. Returns kOk when the driver supports it, which the host checks once before start().
  virtual Status outputReady() = 0;
};

// Layout checks for the x64 ABI.
static_assert(sizeof(Int64Halves) == 8 && alignof(Int64Halves) == 4);
static_assert(sizeof(ClockSource) == 48);
static_assert(sizeof(ChannelInfo) == 52 && offsetof(ChannelInfo, type) == 16 && offsetof(ChannelInfo, name) == 20);
static_assert(sizeof(BufferInfo) == 24 && offsetof(BufferInfo, buffers) == 8);
static_assert(sizeof(TimeInfo) == 48 && offsetof(TimeInfo, samplePosition) == 16 && offsetof(TimeInfo, flags) == 32);
static_assert(sizeof(TimeCode) == 88);
static_assert(sizeof(Time) == 152 && offsetof(Time, timeInfo) == 16 && offsetof(Time, timeCode) == 64);
static_assert(sizeof(HostCallbacks) == 32);

}  // namespace pq::engine::asio
