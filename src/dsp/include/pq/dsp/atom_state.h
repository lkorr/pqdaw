#pragma once

// POD state blocks of the stateful leaf atoms (T1.6). Sizes are static (sizeof) except for the delay line,
// whose ring buffer follows its header (size from stateBytes() in atoms.h).
//
// Every per-lane field holds 8 lanes, also for 1-lane (global) kernels, which use lane 0 only. The cost is
// ≤ 64 bytes per mono node; in exchange each atom has ONE state size and type, independent of polyphony, which
// keeps the T1.1 registry and hot-swap state transfer (SPEC §7.5, keyed by NodeId + type tag) simple.
// All states are 32-byte aligned (one __m256 per field). Bool fields hold masks (0 / 0xFFFFFFFF as float).
// States are trivially copyable; initState() (atoms.h) writes the documented initial values.

#include <cstdint>
#include <type_traits>

namespace pq::dsp {

inline constexpr uint32_t kStateAlign = 32;

struct alignas(32) Z1State {  // z1: previous input
  float last[8];
};

struct alignas(32) EdgeState {  // rise / fall / change: previous input level
  float prev[8];
};

struct alignas(32) SampleHoldState {  // sample&hold: held value, previous trigger level
  float held[8];
  float prevTrig[8];
};

struct alignas(32) LatchState {  // latch: held value
  float held[8];
};

struct alignas(32) SchmittState {  // schmitt: current output level
  float q[8];
};

struct alignas(32) FlipFlopState {  // sr / d / t flip-flops: q, previous clock (unused by sr)
  float q[8];
  float prevClk[8];
};

struct alignas(32) CounterState {  // counter: count, previous up / down levels
  int32_t count[8];
  float prevUp[8];
  float prevDown[8];
};

struct alignas(32) ClockDivState {  // clockdiv: index of the current input period mod n (-1 before the first edge)
  int32_t k[8];
  float prevClk[8];
};

struct alignas(32) PulseState {  // pulse: samples until the next one-sample pulse (-1 = one-shot already fired)
  int32_t remaining[8];
};

struct alignas(32) TimerState {  // timer: samples left high, previous trigger level
  int32_t remaining[8];
  float prevTrig[8];
};

// phasor and sine-from-frequency: uint32 fixed-point phase (SPEC §3.1). 2^32 = one cycle.
struct alignas(32) PhasorState {
  uint32_t phase[8];
  double incScale;  // 2^32 / sampleRate: increment = round(hz · incScale)
  float halfSr;     // sampleRate / 2: |hz| is clamped to this
  float pad_[5];
};

// noise: counter-based generator (out[t] = hash(counter_t + seed[lane])), so the output does not depend on how
// the timeline is split into blocks.
struct alignas(32) NoiseState {
  uint32_t seed[8];
  uint32_t counter;
  uint32_t pad_[7];
};

// wavetable: previous phase and previous |Δphase| (the increment estimate → mip level is the smaller of the last
// two deltas, so a single phase jump never picks the level; tables.h).
struct alignas(32) WavetableState {
  float prevPhase[8];
  float prevDelta[8];
};

// delayline: header, then `capacity * lanes` floats of ring buffer (frame-major, lanes interleaved).
struct alignas(32) DelayState {
  uint32_t capacity;  // frames, power of two ≥ maxDelay + 10
  uint32_t mask;      // capacity − 1
  uint32_t writePos;  // next frame to write (wraps through mask)
  uint32_t lanes;     // 1 or 8: the ring layout this state was initialised for
  float maxDelay;     // largest accepted delay in samples (inputs are clamped)
  float pad_[3];
  float apY1[8];      // allpass interpolator: previous output per lane
  // 8-lane rings only (voice delay lines, T1.8-fix1): per lane, the ring frame (in writePos's wrapping count) from
  // which the lane's history is valid. A tap older than it reads 0, as if the lane's ring had been zeroed when
  // start[l] was set, so the VoiceEngine resets a lane's delay line in O(1) (start[l] = writePos) instead of
  // clearing its ring (O(capacity), M1 review M9). The write kernel keeps writePos − start[l] ≤ capacity, so the
  // difference never wraps. Mono rings ignore it (never reset per lane; zeroed by initState).
  uint32_t start[8];
  float* ring() noexcept { return reinterpret_cast<float*>(this + 1); }
  const float* ring() const noexcept { return reinterpret_cast<const float*>(this + 1); }
};

// sampleRead: 32.32 fixed-point playhead per lane (index + fraction), active mask.
struct alignas(32) SampleReadState {
  int32_t index[8];
  uint32_t frac[8];
  float active[8];
};

static_assert(sizeof(Z1State) == 32);
static_assert(sizeof(EdgeState) == 32);
static_assert(sizeof(SampleHoldState) == 64);
static_assert(sizeof(LatchState) == 32);
static_assert(sizeof(SchmittState) == 32);
static_assert(sizeof(FlipFlopState) == 64);
static_assert(sizeof(CounterState) == 96);
static_assert(sizeof(ClockDivState) == 64);
static_assert(sizeof(PulseState) == 32);
static_assert(sizeof(TimerState) == 64);
static_assert(sizeof(PhasorState) == 64);
static_assert(sizeof(NoiseState) == 64);
static_assert(sizeof(WavetableState) == 64);
static_assert(sizeof(DelayState) == 96);
static_assert(sizeof(SampleReadState) == 96);
static_assert(std::is_trivially_copyable_v<DelayState> && std::is_trivially_copyable_v<PhasorState>);

}  // namespace pq::dsp
