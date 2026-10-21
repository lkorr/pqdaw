// Atom table, kernel lookup and state initialisation (T1.6).

#include "pq/dsp/atoms.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "kernels_common.h"

namespace pq::dsp {
namespace {

constexpr uint32_t kB0 = 1u << 0, kB1 = 1u << 1, kB2 = 1u << 2, kB3 = 1u << 3, kB4 = 1u << 4;

// name, numIn, numOut, variadicIn, variadicOut, boolIn, boolOut, splSpecMask, splRequired, stateSize, resource,
// inPlaceOk
constexpr KernelInfo bin(const char* n) { return {n, 2, 1, false, false, 0, 0, kB0 | kB1, 0, 0, Resource::None, true}; }
constexpr KernelInfo un(const char* n) { return {n, 1, 1, false, false, 0, 0, kB0, 0, 0, Resource::None, true}; }
constexpr KernelInfo ter(const char* n, uint32_t boolIn = 0) {
  return {n, 3, 1, false, false, boolIn, 0, kB0 | kB1 | kB2, 0, 0, Resource::None, true};
}
constexpr KernelInfo logic2(const char* n) {
  return {n, 2, 1, false, false, kB0 | kB1, kB0, kB0 | kB1, 0, 0, Resource::None, true};
}
constexpr KernelInfo cmp(const char* n) { return {n, 2, 1, false, false, 0, kB0, kB0 | kB1, 0, 0, Resource::None, true}; }
constexpr KernelInfo st(const char* n, uint8_t nin, uint32_t boolIn, uint32_t boolOut, uint32_t size,
                        uint32_t spec) {
  return {n, nin, 1, false, false, boolIn, boolOut, spec, 0, size, Resource::None, false};
}

constexpr KernelInfo kInfo[kAtomCount] = {
    bin("add"), bin("sub"), bin("mul"), bin("div"), bin("min"), bin("max"), bin("mod"), bin("pow"),
    bin("fast_pow"), bin("cents_to_hz"), bin("fast_cents_to_hz"), bin("hz_to_cents"), bin("fast_hz_to_cents"),
    un("neg"), un("abs"), un("sign"), un("floor"), un("frac"), un("sqrt"), un("fast_sqrt"), un("exp"), un("fast_exp"), un("log"),
    un("fast_log"), un("sin"), un("fast_sin"), un("tan"), un("fast_tan"), un("tanh"), un("fast_tanh"),
    un("db_to_gain"), un("fast_db_to_gain"), un("gain_to_db"), un("fast_gain_to_db"), un("const"),
    ter("clip"), ter("lerp"), ter("select", kB0), ter("muladd"),
    logic2("and"), logic2("or"), logic2("xor"), logic2("nand"), logic2("nor"),
    {"not", 1, 1, false, false, kB0, kB0, kB0, 0, 0, Resource::None, true},
    cmp("gt"), cmp("lt"), cmp("ge"), cmp("le"), cmp("eq"), cmp("ne"),
    {"gate_to_real", 1, 1, false, false, kB0, 0, kB0, 0, 0, Resource::None, true},
    {"mux", 2, 1, true, false, 0, 0, kB0, 0, 0, Resource::None, false},
    {"demux", 2, 1, false, true, 0, 0, kB0 | kB1, 0, 0, Resource::None, false},
    st("schmitt", 3, 0, kB0, sizeof(SchmittState), kB0 | kB1 | kB2),
    st("rise", 1, kB0, kB0, sizeof(EdgeState), kB0),
    st("fall", 1, kB0, kB0, sizeof(EdgeState), kB0),
    st("change", 1, kB0, kB0, sizeof(EdgeState), kB0),
    st("srff", 2, kB0 | kB1, kB0, sizeof(FlipFlopState), kB0 | kB1),
    st("dff", 2, kB0 | kB1, kB0, sizeof(FlipFlopState), kB0 | kB1),
    st("tff", 2, kB0 | kB1, kB0, sizeof(FlipFlopState), kB0 | kB1),
    st("counter", 4, kB0 | kB1 | kB2, 0, sizeof(CounterState), kB0 | kB1 | kB2 | kB3),
    st("clockdiv", 2, kB0, kB0, sizeof(ClockDivState), kB0 | kB1),
    st("pulse", 1, 0, kB0, sizeof(PulseState), kB0),
    st("timer", 2, kB0, kB0, sizeof(TimerState), kB0 | kB1),
    st("z1", 1, 0, 0, sizeof(Z1State), kB0),
    st("delay", 2, 0, 0, sizeof(DelayState), kB0 | kB1),
    st("delay_cubic", 2, 0, 0, sizeof(DelayState), kB0 | kB1),
    st("delay_allpass", 2, 0, 0, sizeof(DelayState), kB0 | kB1),
    st("samplehold", 2, kB1, 0, sizeof(SampleHoldState), kB0 | kB1),
    st("latch", 2, kB1, 0, sizeof(LatchState), kB0 | kB1),
    {"z1:read", 0, 1, false, false, 0, 0, 0, 0, sizeof(Z1State), Resource::None, false},
    {"z1:write", 1, 0, false, false, 0, 0, kB0, 0, sizeof(Z1State), Resource::None, false},
    {"delay:read", 1, 1, false, false, 0, 0, kB0, 0, sizeof(DelayState), Resource::None, false},
    {"delay_cubic:read", 1, 1, false, false, 0, 0, kB0, 0, sizeof(DelayState), Resource::None, false},
    {"delay_allpass:read", 1, 1, false, false, 0, 0, kB0, 0, sizeof(DelayState), Resource::None, false},
    {"delay:write", 1, 0, false, false, 0, 0, kB0, 0, sizeof(DelayState), Resource::None, false},
    st("phasor", 2, kB1, 0, sizeof(PhasorState), kB0 | kB1),
    {"sine", 1, 1, false, false, 0, 0, kB0, 0, 0, Resource::None, true},
    st("sine_hz", 2, kB1, 0, sizeof(PhasorState), kB0 | kB1),
    {"wavetable", 1, 1, false, false, 0, 0, kB0, 0, sizeof(WavetableState), Resource::Wavetable, false},
    {"noise", 0, 1, false, false, 0, 0, 0, 0, sizeof(NoiseState), Resource::None, false},
    {"shaper", 1, 1, false, false, 0, 0, kB0, 0, 0, Resource::Shaper, true},
    {"sample_read_linear", 5, 2, false, false, kB1, kB1, kB0 | kB1, kB2 | kB3 | kB4, sizeof(SampleReadState),
     Resource::Sample, false},
    {"sample_read_cubic", 5, 2, false, false, kB1, kB1, kB0 | kB1, kB2 | kB3 | kB4, sizeof(SampleReadState),
     Resource::Sample, false},
    {"sample_pos_linear", 1, 1, false, false, 0, 0, kB0, 0, 0, Resource::Sample, true},
    {"sample_pos_cubic", 1, 1, false, false, 0, 0, kB0, 0, 0, Resource::Sample, true},
};

bool isDelay(Atom a) noexcept {
  switch (a) {
    case Atom::DelayLinear: case Atom::DelayCubic: case Atom::DelayAllpass: case Atom::DelayReadLinear:
    case Atom::DelayReadCubic: case Atom::DelayReadAllpass: case Atom::DelayWrite:
      return true;
    default:
      return false;
  }
}

// Max delay clamped to [2, 2^30]: ≥ 2 keeps every interpolator's minimum delay (≤ 1) below the maximum.
uint32_t clampMaxDelay(uint32_t d) noexcept { return d < 2u ? 2u : d > (1u << 30) ? (1u << 30) : d; }

// Ring frames: the 4-point read reaches maxDelay + 2 back, and the 1-lane kernel writes 8 frames ahead of its
// reads (vectorised over time), so 10 frames of headroom.
uint32_t delayCapacity(uint32_t maxDelay) noexcept {
  const uint64_t need = static_cast<uint64_t>(maxDelay) + 10;
  return static_cast<uint32_t>(std::bit_ceil(need));
}

// Seeds for the noise lanes: well-mixed and distinct for every (seed, lane).
uint32_t mixSeed(uint32_t seed, uint32_t lane) noexcept {
  uint32_t x = seed ^ (0x9E3779B9u * (lane + 1u));
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

}  // namespace

const KernelInfo& kernelInfo(Atom a) noexcept { return kInfo[static_cast<uint32_t>(a)]; }

KernelFn findKernel(Atom a, uint32_t lanes, uint32_t splMask) noexcept {
  if (static_cast<uint32_t>(a) >= kAtomCount) return nullptr;
  const KernelInfo& info = kernelInfo(a);
  if ((splMask & info.splRequired) != info.splRequired) return nullptr;
  if (!info.variadicIn && (splMask & ~(info.splSpecMask | info.splRequired)) != 0) return nullptr;
  const uint32_t spec = splMask & info.splSpecMask;
  using namespace detail;
  if (KernelFn f = findMathKernel(a, lanes, spec)) return f;
  if (KernelFn f = findLogicKernel(a, lanes, spec)) return f;
  if (KernelFn f = findMemoryKernel(a, lanes, spec)) return f;
  if (KernelFn f = findGeneratorKernel(a, lanes, spec)) return f;
  return findTableKernel(a, lanes, spec);
}

uint32_t stateBytes(Atom a, const StateInit& init) noexcept {
  if (static_cast<uint32_t>(a) >= kAtomCount) return 0;
  const uint32_t base = kernelInfo(a).stateSize;
  if (!isDelay(a)) return base;
  const uint32_t lanes = init.lanes == kGroupLanes ? kGroupLanes : 1u;
  const uint32_t maxDelay = clampMaxDelay(init.maxDelayFrames);
  return base + delayCapacity(maxDelay) * lanes * static_cast<uint32_t>(sizeof(float));
}

void initState(Atom a, void* state, const StateInit& init) noexcept {  // RT-safe
  const uint32_t bytes = stateBytes(a, init);
  if (bytes == 0 || state == nullptr) return;
  std::memset(state, 0, bytes);  // every field's initial value is 0 / false unless set below
  switch (a) {
    case Atom::Z1:
    case Atom::Z1Read:
    case Atom::Z1Write: {
      auto& s = *static_cast<Z1State*>(state);
      for (float& v : s.last) v = init.z1Init;
      break;
    }
    case Atom::ClockDiv: {
      auto& s = *static_cast<ClockDivState*>(state);
      for (int32_t& k : s.k) k = -1;
      break;
    }
    case Atom::Phasor:
    case Atom::SineHz: {
      auto& s = *static_cast<PhasorState*>(state);
      s.incScale = 4294967296.0 / init.sampleRate;
      s.halfSr = static_cast<float>(0.5 * init.sampleRate);
      break;
    }
    case Atom::Noise: {
      auto& s = *static_cast<NoiseState*>(state);
      for (uint32_t l = 0; l < 8; ++l) s.seed[l] = mixSeed(init.seed, l);
      break;
    }
    case Atom::DelayLinear:
    case Atom::DelayCubic:
    case Atom::DelayAllpass:
    case Atom::DelayReadLinear:
    case Atom::DelayReadCubic:
    case Atom::DelayReadAllpass:
    case Atom::DelayWrite: {
      auto& s = *static_cast<DelayState*>(state);
      const uint32_t maxDelay = clampMaxDelay(init.maxDelayFrames);
      s.capacity = delayCapacity(maxDelay);
      s.mask = s.capacity - 1;
      s.writePos = 0;
      s.lanes = init.lanes == kGroupLanes ? kGroupLanes : 1u;
      s.maxDelay = static_cast<float>(maxDelay);
      break;
    }
    case Atom::SampleReadLinear:
    case Atom::SampleReadCubic: {
      auto& s = *static_cast<SampleReadState*>(state);
      for (float& m : s.active) m = boolMask(true);
      break;
    }
    default:
      break;
  }
}

}  // namespace pq::dsp
