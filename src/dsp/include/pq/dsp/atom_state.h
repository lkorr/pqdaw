#pragma once

// POD state blocks of the stateful leaf atoms (T1.6). Sizes are static (sizeof) except for the delay line,
// whose ring buffer follows its header (size from stateBytes() in atoms.h).
//
// Every per-lane field holds 8 lanes, also for 1-lane (global) kernels, which use lane 0 only. The cost is
// ≤ 64 bytes per mono node; in exchange each atom has ONE state size and type, independent of polyphony, which
// keeps the T1.1 registry and hot-swap state transfer (SPEC §7.5, keyed by NodeId + type tag) simple.
// All states are 32-byte aligned (one __m256 per field). Bool fields hold masks (0 / 0xFFFFFFFF as float).
// States are trivially copyable; initState() (atoms.h) writes the documented initial values.

#include <cstddef>
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

// param_ramp (TX.smooth, the compiler's param smoothing; kernels_ramp.cpp): per lane the value it heads for, the
// step per frame and the frames left of the ramp (the value at frame k of the remaining ramp is target − inc·(rem −
// k − 1)), whether the lane has run (0 after initState and a voice's lane reset: its first block starts at its
// input); for the whole block whether this block's call wrote the output (some lane ramps).
struct alignas(32) ParamRampState {
  float target[8];
  float inc[8];
  float rem[8];      // frames left (an exact integer in float: ≤ 2^24)
  float started[8];
  uint32_t active;   // the last call wrote its output buffer (else the output holds nothing of this block)
  uint32_t pad_[7];
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
// lane), then the table generation it plays (0 = fresh: no fade), the crossfade frames still to run and the voice
// group (StateInit::voiceGroup) its tap record goes to; AU.fold: the generation it fades from (frame_table_slot.h
// "Publication").
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
  uint32_t pad_;
  uint64_t fromGen;
  float sineDc[8];
  float sineAmp[8];
  float sineTh[8];
};

// wavetable_live (TMM.8, pq/dsp/wave_live.h): per lane the previous phase, |Δphase|, synced phase ψ and its
// sub-period count, the controls of the previous block (a param-rate control ramps from it), whether the lane has a
// previous sample, and the correction ring (2·kLiveCorrHalf slots of the delayed output, residuals stamped ahead);
// then the group's table generation, crossfade, tap group, ring position and the state's sample rate; AU.fold: the
// generation it fades from.
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
  uint64_t fromGen;
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
  uint32_t pad_;
  uint64_t fromGen;
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
  // delay_fade's read heads (DT.2, kernels_memory.cpp DelayFade), per lane, in samples / frames; unused by the other
  // delay lines. The main head is fdEnd − fdInc·fdRem (gliding to fdEnd while fdRem > 0); while fdFadeRem > 0 a
  // second head at fdOld fades out (equal power over fdFadeLen frames). fdStarted is 0 until the lane's first frame.
  float fdTarget[8];   // the last time read (clamped)
  float fdEnd[8];      // where the main head glides to (= fdTarget at rest)
  float fdInc[8];      // glide step per frame
  float fdRem[8];      // glide frames left
  float fdOld[8];      // the fading head's delay
  float fdFadeRem[8];  // fade frames left (0: one head)
  float fdFadeLen[8];  // the running fade's length
  float fdStarted[8];
  // DT.4 Repitch (the `crossfade` input < 0.5): the main head is fdEnd + u²·(fdD2 + u·fdD3), u = fdRem·fdInvLen (a
  // cubic Hermite from where the head was, with its velocity, to fdEnd, arriving at rest); fdInc is 0. fdRepitch: the
  // mode of the last block (1 = Repitch), to convert a running glide when the mode changes.
  float fdD2[8];
  float fdD3[8];
  float fdInvLen[8];
  float fdRepitch[8];
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

// svf (EQ.1, kernels_svf.cpp; pq/dsp/svf_design.h; EQ.1b: sized by its sections, param ramps interpolated): a header
// of per-lane [8] arrays, then `sections` (StateInit::svfSections: the count its mode / stages consts give, else 64)
// cascaded TPT sections. EVERY word is per lane (LaneState::Whole): the values the group shares (sample rate, layout,
// section capacity) are replicated in each lane, and the precision flag is per lane (the group's precision is any
// lane's: a lane reset to the initial state holds zeros, valid in either precision).
// Per lane: the inputs its coefficients are for and whether they are their exact design (0: interpolated along a ramp),
// a key (0: none yet; else 1 | mode << 1 | prewarp << 4 | stages << 8) with its section count, the inputs of the last
// two frames, and the ramp segment being interpolated (its first frame's inputs, the inputs' step per frame, its
// length and the frames left; 0: none).
// Per section kSvfFields fields: the two integrator states (float; in double, the 32-bit halves lo = the float field, hi
// = the next field, so a 1-lane double is one aligned 8-byte word), the six coefficients the kernel runs (d1 b1 b2 m0
// h1 h2), and their first and second forward differences per frame along a ramp segment (6 + 6). 8 lanes: section s,
// field f, lane l at sec[s·kSvfFields·8 + f·8 + l]; 1 lane: sec[s·kSvfFields + f]. Sections from a lane's count on hold
// the identity (m0 = 1, no recursion), so an 8-lane group runs the largest count of its lanes.
inline constexpr uint32_t kSvfStateSections = 64;  // = kSvfMaxStages (svf_design.h): the most a state can hold
inline constexpr uint32_t kSvfFields = 22;
struct alignas(32) SvfState {
  float cutoff[8];
  float q[8];
  float gain[8];         // 1 for the modes without a gain (the design ignores it)
  uint32_t key[8];
  uint32_t count[8];
  uint32_t exact[8];     // 1: the coefficients are svfDesign(cutoff, q, gain) exactly
  float prev[3][8];      // cutoff, q, gain of the last frame
  float prev2[3][8];     // and of the one before
  float base[3][8];      // ramp segment: the inputs at its first frame
  float slope[3][8];     // and their step per frame
  uint32_t len[8];       // its length in frames
  uint32_t rem[8];       // frames left (0: no segment)
  uint32_t primed[8];    // prev / prev2 hold inputs (0 after a reset)
  float sampleRate[8];   // replicated (initState)
  uint32_t precision[8]; // 1: the states are doubles
  uint32_t sections[8];  // replicated: the sections this block holds (initState)
  uint32_t lanes[8];     // replicated: 1 or 8, the section layout (initState)
  float sec[kSvfStateSections * kSvfFields * 8];
};
// The header: what stateBytes() adds sections to (kernelInfo(svf).stateSize).
inline constexpr uint32_t kSvfHeaderBytes = static_cast<uint32_t>(sizeof(SvfState) - sizeof(SvfState::sec));
static_assert(kSvfHeaderBytes % 32 == 0 && kSvfHeaderBytes == 25 * 32);

// follow (DY.1, kernels_dynamics.cpp): the envelope per lane (every word per lane).
struct alignas(32) FollowState {
  float y[8];
};

// window (DY.1, kernels_dynamics.cpp): a header of per-lane [8] arrays, then the values (stateBytes: L = length).
// EVERY word is per lane (LaneState::Whole): length, op and layout are replicated, and each lane has its own position,
// so the VoiceEngine's lane reset (the initial words) and move (another group's words) need no help; the 8-lane
// kernel re-aligns a lane whose position differs from its group's (kernels_dynamics.cpp windowRealign) before it runs.
//   max / min (van Herk–Gil-Werman): the time axis is cut into segments of L frames; pos = frames of the current
//     segment written so far. values[0, pos) hold its raw inputs, values[pos, L) the suffix extrema of the previous
//     segment (values[j] = max of its inputs j … L − 1), values[L] = NaN (an empty suffix), prefix = the extremum of
//     the current segment so far (NaN: empty). Out = ext(values[pos + 1], prefix) after the input is added.
//   mean: values[0, L) is a ring of the last L inputs (values[pos] the oldest), sum (a double, its 32-bit halves in
//     sumLo / sumHi) their running sum, recomputed from the ring when pos wraps (no drift).
// Initial state: zero history (values 0, values[L] NaN, prefix NaN, sum 0, pos 0). 8 lanes: values[i·8 + lane] for
// i ≤ L, then ⌈L / 8⌉ rows of scratch (the re-alignment's); 1 lane: values[i].
struct alignas(32) WindowState {
  uint32_t pos[8];
  float prefix[8];
  uint32_t sumLo[8];
  uint32_t sumHi[8];
  uint32_t length[8];  // L ≥ 1 (StateInit::windowFrames, clamped)
  uint32_t op[8];      // 0 max, 1 min, 2 mean (StateInit::windowOp, clamped)
  uint32_t lanes[8];   // 1 or 8: the layout of the values
  uint32_t pad_[8];
  float* values() noexcept { return reinterpret_cast<float*>(this + 1); }
  const float* values() const noexcept { return reinterpret_cast<const float*>(this + 1); }
};

// noise_step (NZ.1, kernels_noise_step.cpp): everything up to `counter` is [8] arrays reset per voice lane (a note-on
// restarts the lane: a first value is drawn at its next frame); `counter` (frames this block has run), `base` (the
// node's seed) and `group` (its voice group's mix) are the group's. A draw at frame t hashes key + (t − origin): the
// stream position is the frame, so the shared counter is the only clock and a lane's restart only moves its origin.
struct alignas(32) NoiseStepState {
  uint32_t phase[8];     // rate phase, 2^32 = one step
  uint32_t origin[8];    // the counter at the lane's last restart
  uint32_t lastStep[8];  // the counter at its last step
  uint32_t seed[8];      // mixSeed(node seed, lane): constant (initState and the lane reset write the same)
  float hasLast[8];      // mask: lastStep is valid (a period can be measured)
  float started[8];      // mask: the lane has drawn its first value
  float prevTrig[8];     // masks: trig / reset on the previous frame
  float prevReset[8];
  float prevCell[8];     // floor(beat / grid) on the previous frame (NaN: none)
  float from[8];         // the glide: from → to, u ∈ [0, 1] advancing du per frame
  float to[8];
  float u[8];
  float du[8];
  float out[8];          // the last output
  float w[5][8];         // the colour cascade's history: w[k] = section k's previous input (w[4]: the last output)
  float colourKey[8];    // the colour the coefficients below are for (NaN: none yet)
  float z[4][8];         // its zeros (kNoiseColourPoles are fixed)
  float gain[8];
  float white[8];        // mask: colour 0, the identity
  float pos[8];          // mask: colour > 0 (scaled, not folded: Q244)
  float keyTag[8];       // the seed (and lanes const) the keys below are for (kNoiseStepNoKey: none yet)
  uint32_t keys[8];      // the draw keys
  float rateKey[8];      // the rate the increment below is for (kNoiseStepNoKey: none yet)
  uint32_t inc[8];       // round(rate · 2^32 / sr)
  uint32_t counter;
  uint32_t base;
  uint32_t group;
  float sampleRate;
  uint32_t pad_[4];
};
static_assert(offsetof(NoiseStepState, counter) == 31 * 32 && sizeof(NoiseStepState) == 32 * 32);
inline constexpr uint32_t kNoiseStepNoKey = 0x7FBADBADu;  // a NaN no input carries: a cache entry not made yet
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
static_assert(sizeof(DelayState) == 480);
static_assert(sizeof(FollowState) == 32 && sizeof(WindowState) == 256);
static_assert(sizeof(WavetablePosState) == 192);
static_assert(sizeof(VoiceTapState) == 64);
static_assert(sizeof(WavetableLiveState) == 672);
static_assert(sizeof(WavetableShapeState) == 160);
static_assert(sizeof(SampleReadState) == 96);
static_assert(sizeof(ParamRampState) == 160);
static_assert(std::is_trivially_copyable_v<DelayState> && std::is_trivially_copyable_v<PhasorState>);

}  // namespace pq::dsp
