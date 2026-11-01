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

// gain / gain_add (T3.17-fix2): per lane the target the gain heads for and the residual d (gain = target + d; 0 when
// settled), the time constant its coefficient was computed for, the coefficient r = e^(−1/τ), whether the lane
// has run (T4.x-fix-dsp: with `start` 1 its first block starts at the target instead of at 0), and for the 1-lane
// closed form r^0 … r^7 and r^8 (lane 0's).
struct alignas(32) GainState {
  float target[8];
  float d[8];
  float tau[8];
  float r[8];
  float started[8];  // 0 until the lane's first block (initState and a voice's lane reset clear it)
  float rpow[8];
  float r8;
  float pad_[7];
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

// wavetable_pos (TMM.2): the frame-table reader's state (FrameReadState's layout: previous phase and |Δphase| per
// lane), then the table generation last read (0 = fresh: no fade), the crossfade frames still to run and the voice
// group (StateInit::voiceGroup) its tap record goes to.
// TMM.12-fix1: then the JIT's inline sine path (jit/src/codegen.cpp, a wavetable_pos in a per-sample loop): every
// call leaves sineOn = 1 when the frames after it in this block may be computed inline as the readers' sine path
// (frame_table.h "Sine tables": a sine table, no crossfade running, no tap reader, every lane on one frame), with the
// lanes' coefficients; else 0. The generated code calls the kernel at a block's first frame and whenever sineOn = 0.
struct alignas(32) WavetablePosState {
  float prevPhase[8];
  float prevDelta[8];
  uint64_t generation;
  uint32_t fadeLeft;
  uint32_t group;
  uint32_t sineOn;
  uint32_t pad_[3];
  float sineDc[8];
  float sineAmp[8];
  float sineTh[8];
};

// wavetable_live (TMM.8, pq/dsp/wave_live.h): per lane the previous phase, |Δphase|, synced phase ψ and its
// sub-period count, the controls of the previous block (a param-rate control ramps from it), whether the lane has a
// previous sample, and the correction ring (2·kLiveCorrHalf slots of the delayed output, residuals stamped ahead);
// then the group's table generation, crossfade, tap group, ring position and the state's sample rate.
struct alignas(32) WavetableLiveState {
  float prevPhase[8];
  float prevDelta[8];
  float prevPsi[8];
  float prevSub[8];
  float prevCtl[3][8];  // warp, fold, sync
  float primed[8];      // mask: the lane has a previous sample
  float ring[8][8];     // [slot][lane]
  float prevW[8], prevS[8];        // the previous sample's warp and sync ratio (their per-sample change)
  float prevSpA[8], prevSpB[8];    // the previous sample's level speeds (the left side of an event)
  uint64_t generation;
  uint32_t fadeLeft;
  uint32_t group;
  uint32_t ringPos;
  float sampleRate;
  uint32_t pad_[2];
};

// wavetable_shape (TMM.8): per lane the previous input position u (unclamped), the held |Δu| that picks the level,
// whether the lane has a previous sample; then as wavetable_live.
struct alignas(32) WavetableShapeState {
  float prevU[8];
  float hold[8];
  float primed[8];
  float pad0_[8];
  uint64_t generation;
  uint32_t fadeLeft;
  uint32_t group;
  float sampleRate;
  uint32_t pad_[3];
};

// voice_tap (TMM.2): the previous input per lane (the record's increment), the voice group.
struct alignas(32) VoiceTapState {
  float prev[8];
  uint32_t group;
  uint32_t pad_[7];
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

// TMM.6: oversample_up / oversample_down (pq/dsp/oversample.h): per allpass of the half-band cascade its previous
// output, per stage and path the path's previous input (18 allpasses over 4 stages; a lower factor uses the first
// stages). Every field is per lane.
struct alignas(32) OversampleState {
  float y1[18][8];
  float xin[8][8];  // [stage · 2 + path]
};

// TMM.6: oversample_phase: the previous base-rate phase and whether there is one (0 after a reset: the first frame
// holds its value).
struct alignas(32) OversamplePhaseState {
  float prev[8];
  float primed[8];
};

static_assert(sizeof(OversampleState) == 26 * 32);
static_assert(sizeof(OversamplePhaseState) == 64);
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
static_assert(sizeof(WavetablePosState) == 192);
static_assert(sizeof(VoiceTapState) == 64);
static_assert(sizeof(WavetableLiveState) == 672);
static_assert(sizeof(WavetableShapeState) == 160);
static_assert(sizeof(SampleReadState) == 96);
static_assert(std::is_trivially_copyable_v<DelayState> && std::is_trivially_copyable_v<PhasorState>);

}  // namespace pq::dsp
