#pragma once

// The native leaf atoms of SPEC §7.2 as kernels (T1.6): the list, their ports and semantics, kernel lookup and
// state initialisation. Calling convention, buffer layout and bool masks: kernel.h. State structs: atom_state.h.
// Tables: tables.h.
//
// Semantics (inputs in order; "edge" = rising edge, i.e. input high now and low on the previous sample; all
// edge-detecting atoms start with "previous = low"):
//
// Math (stateless, element-wise; float32 IEEE unless noted):
//   add sub mul div (a, b)            a+b, a−b, a·b, a/b
//   min max (a, b)                    a<b ? a : b, a>b ? a : b (so a NaN in either operand returns b)
//   mod (a, b)                        floored: a − b·floor(a/b) (sign of b; NaN for b = 0); a result that
//                                     rounds to exactly b (mod(−1e−9, 1)) is 0, so it lies in [0, b) for b > 0
//   pow (a, b) / fast_pow             a^b. fast_pow = exp2(b·log2 a): a < 0 → NaN, a = 0 → 0 for b > 0
//   cents_to_hz (cents, ref)          ref · 2^(cents/1200); exact form evaluated in double
//   hz_to_cents (hz, ref)             1200·log2(hz/ref); exact form in double
//   neg abs (x)                       −x, |x|
//   sign (x)                          1, −1 or 0 (also 0 for NaN)
//   floor (x)                         floor
//   frac (x)                          x − floor(x), clamped to the largest float below 1
//   sqrt exp log sin tan tanh (x)     exact: CRT / SVML (verified ≤ a few ulp against double)
//   fast_sqrt (x)                     = sqrt: the hardware square root is exact and already cheaper
//                                     than an rsqrt + Newton step with zero/inf fix-ups
//   fast_exp fast_log fast_sin fast_tan fast_tanh   pq/core/fast_math.h (error bounds documented there).
//                                     fast_sin is accurate for |x| ≤ 65536 rad; the atom clamps its result to
//                                     [−1, 1], so any finite x gives a finite value (garbage beyond the domain:
//                                     use sine(phase) for unbounded phases); NaN and ±inf give NaN
//   db_to_gain gain_to_db (x)         exact in double; fast_ variants from pq/core/units.h
//   clip (x, lo, hi)                  min(max(x, lo), hi) in that order (NaN x → lo)
//   lerp (a, b, t)                    a + t·(b − a) (one FMA)
//   select (cond:bool, a, b)          cond ? a : b
//   muladd (a, b, c)                  a·b + c, fused (one rounding); for the optimiser's op fusion
//   const (x)                         copy (fills a block from a Spl pattern)
// Logic (bool = lane mask):
//   and or xor nand nor (a, b), not (a)
//   gt lt ge le eq ne (a:real, b:real) → bool   ordered compares (false on NaN), ne is true on NaN
//   gate_to_real (b) → 0.0 / 1.0
//   mux (index, in0 … in{n−1})        in[clamp(floor(index), 0, n−1)] (NaN → in0); numIn = n + 1, n ≥ 1
//   demux (index, x) → out0 … out{n−1}  out_k = (k == clamp(floor(index), 0, n−1)) ? x : 0; numOut = n
//   schmitt (x, lo, hi)               q = x > hi ? true : x < lo ? false : q
//   rise fall change (b)              b∧¬prev, ¬b∧prev, b⊕prev
//   srff (s, r)                       q = r ? false : s ? true : q (reset wins)
//   dff (d, clk)                      on clk edge q = d; output q (the updated value at the edge sample)
//   tff (t, clk)                      on clk edge, if t then q = ¬q
//   counter (up, down, reset, n) → real   edges of up/down step ±1 (both at once: no change); reset (level)
//                                     forces 0 and wins. If n ≥ 1 the count wraps within [0, n−1] (stepping
//                                     past n−1 → 0, below 0 → n−1; a count left ≥ n by shrinking n → 0).
//                                     n < 1 or NaN: no wrap.
//   clockdiv (clk, n)                 pulse divider: out = clk ∧ (k == 0), where k counts clk edges mod n
//                                     (first edge → 0). n = 1 passes clk through; n < 1 or NaN acts as 1.
//   pulse (period)                    one-sample pulses: high on the first sample, then every P = round(period)
//                                     samples. P < 1, NaN or ≥ 2^31: a single pulse on the first sample.
//                                     A new period takes effect after the next pulse.
//   timer (trig, duration)            on trig edge: high for N = round(duration) samples, starting at the edge
//                                     sample (retriggerable; N ≤ 0 or NaN: no output; N ≤ 2147483520)
// Memory:
//   z1 (x)                            x delayed by one sample (initially StateInit::z1Init, default 0)
//   delay / delay_cubic / delay_allpass (x, delay)  delay in SAMPLES (the compiler multiplies a time in
//                                     seconds by sr), fractional, clamped to [1, max] linear, [2, max] cubic
//                                     (4-point Hermite), [1.5, max] allpass (first-order allpass, fractional
//                                     part kept in [0.5, 1.5) by moving one sample into the integer part);
//                                     NaN → the minimum. Every tap is ≥ 1 sample old, so x is a DELAYED input
//                                     (a feedback loop through a delay line is legal). max =
//                                     StateInit::maxDelayFrames.
//   Split kernels for feedback loops scheduled per sample (same state block as the atom; not atoms):
//     z1:read () → out, z1:write (x): read emits the stored value, write stores x (last frame).
//     delay:read / delay_cubic:read / delay_allpass:read (delay) → out, and delay:write (x).
//     Per sample: read at the start, write at the end; read(frames = k) requires delay ≥ k (it reads frames
//     not yet written otherwise — memory-safe, but stale). Reading then writing frame by frame produces
//     exactly the atom's output (tested bit-for-bit).
//   samplehold (x, trig)             on trig edge held = x; output held (initially 0)
//   latch (x, enable)                 while enable is high held = x; output held (initially 0)
// Generators:
//   phasor (hz, reset)                uint32 phase; output phase/2^32 in [0, 1) (top 24 bits, truncated);
//                                     increment = round(hz · 2^32 / sr) with hz clamped to [−sr/2, sr/2]
//                                     (negative = backwards; NaN hz = 0 Hz). While reset is high the phase is 0. The output at
//                                     sample t is the phase before adding that sample's increment.
//   sine (phase)                      sin(2π·phase) for any real phase (fast_sin accuracy, ~1.5e-7 abs)
//   sine_hz (hz, reset)               sin(2π·phasor(hz, reset)) fused (same phase accumulator as phasor)
//   wavetable (phase)                 band-limited mipmapped table read, adjacent half-octave levels crossfaded
//                                     by the phase increment (tables.h)
//   noise ()                          uniform white noise in [−1, 1): hash(counter + seed[lane]) (lowbias32),
//                                     independent of block partitioning; lanes use different seeds
// Tables:
//   shaper (x)                        transfer curve (tables.h)
//   sample_read_linear/_cubic (rate, reset, start, loopStart, loopEnd) → (out, playing)
//                                     32.32 fixed-point playhead, rate converted exactly (floor(rate·2^32);
//                                     |rate| ≤ 2^20). The output at t is
//                                     the interpolated sample at the playhead, then the playhead moves by rate.
//                                     While reset is high the playhead is set to `start` and playing. Loop when
//                                     loopEnd > loopStart (floored to samples): passing loopEnd forwards wraps
//                                     to loopStart + (overshoot mod length); passing loopStart backwards wraps
//                                     to loopEnd − 1 − (overshoot mod length), for any rate (also |rate| >
//                                     the loop length).
//                                     Without a loop, leaving [0, length) stops playback (out 0, playing false)
//                                     until the next reset. Interpolation taps around the loop seam read the
//                                     buffer as stored (no crossfade). start/loopStart/loopEnd must be Spl.
//                                     Initially playing from 0.
//   sample_pos_linear/_cubic (pos)    stateless read at a sample position (float, in samples); 0 outside
//                                     [0, length). Float position: sub-sample accurate to ~2^24 samples.
// Cubic = 4-point, 3rd-order Hermite (Catmull-Rom); linear = 2-point.

#include <cstddef>
#include <cstdint>

#include "pq/dsp/atom_state.h"
#include "pq/dsp/kernel.h"

namespace pq::dsp {

enum class Atom : uint16_t {
  // math, binary
  Add, Sub, Mul, Div, Min, Max, Mod, Pow, PowFast, CentsToHz, CentsToHzFast, HzToCents, HzToCentsFast,
  // math, unary
  Neg, Abs, Sign, Floor, Frac, Sqrt, SqrtFast, Exp, ExpFast, Log, LogFast, Sin, SinFast, Tan, TanFast, Tanh, TanhFast,
  DbToGain, DbToGainFast, GainToDb, GainToDbFast, Const,
  // math, ternary
  Clip, Lerp, Select, MulAdd,
  // logic, stateless
  And, Or, Xor, Nand, Nor, Not, Gt, Lt, Ge, Le, Eq, Ne, GateToReal, Mux, Demux,
  // logic, stateful
  Schmitt, Rise, Fall, Change, SrFlipFlop, DFlipFlop, TFlipFlop, Counter, ClockDiv, Pulse, Timer,
  // memory
  Z1, DelayLinear, DelayCubic, DelayAllpass, SampleHold, Latch,
  // split kernels for per-sample feedback scheduling (not atoms; see above)
  Z1Read, Z1Write, DelayReadLinear, DelayReadCubic, DelayReadAllpass, DelayWrite,
  // generators
  Phasor, SinePhase, SineHz, Wavetable, Noise,
  // tables
  Shaper, SampleReadLinear, SampleReadCubic, SamplePosLinear, SamplePosCubic,
  Count_
};
inline constexpr uint32_t kAtomCount = static_cast<uint32_t>(Atom::Count_);

enum class Resource : uint8_t { None, Wavetable, Shaper, Sample };

// Static description of an atom kernel (for the T1.1 registry and the interpreter).
struct KernelInfo {
  const char* name;       // patch-language name
  uint8_t numIn;          // fixed input count (mux: 1 + n, given at call time; this is the minimum, 2)
  uint8_t numOut;         // fixed output count (demux: n at call time; this is the minimum, 1)
  bool variadicIn;        // mux
  bool variadicOut;       // demux
  uint32_t boolIn;        // bit i: input i is bool
  uint32_t boolOut;       // bit i: output i is bool
  uint32_t splSpecMask;   // inputs whose Blk/Spl choice is a template specialisation (findKernel's mask bits)
  uint32_t splRequired;   // inputs that must be Spl (param rate only)
  uint32_t stateSize;     // sizeof the state struct (0: stateless; DelayState: header only, see stateBytes)
  Resource resource;
  bool inPlaceOk;         // output 0 may alias an input (element-wise stateless atoms)
};

const KernelInfo& kernelInfo(Atom a) noexcept;  // RT-safe

// Looks up the kernel for `lanes` (1 or 8) and the Spl input mask (bit i = input i is a Spl pattern). Inputs
// in KernelInfo::splRequired must have their bit set; for mux all inputs may be either (any mask). Returns
// nullptr for an unsupported combination. RT-safe (table lookup), though callers normally do it off-thread.
KernelFn findKernel(Atom a, uint32_t lanes, uint32_t splMask) noexcept;

struct StateInit {
  double sampleRate = 48000.0;
  uint32_t lanes = 1;            // 1 or 8 (only the delay line's ring layout depends on it)
  uint32_t maxDelayFrames = 0;   // delay lines: largest delay in samples (≤ maxDelayFramesLimit(lanes))
  uint32_t seed = 0;             // noise: per-node seed (e.g. the NodeId), mixed with the lane index
  float z1Init = 0.0f;           // z1, z1:read, z1:write: the output before the first input (the language's `init`
                                 // input, a const; for a bool z1 pass boolMask(true) or 0)
};

// Largest delay-line ring a state block may hold: 1 GiB. Keeps every ring float index (frame · lanes) far inside
// int32 (the gathers' index type) and every state block inside the compiler's uint32 arena offsets.
inline constexpr size_t kMaxDelayRingBytes = size_t{1} << 30;

// Largest StateInit::maxDelayFrames whose ring fits kMaxDelayRingBytes for `lanes` (1 or 8): 2^28 − 10 frames
// mono (93 min at 48 kHz), 2^25 − 10 per voice group (11.6 min). The compiler rejects a larger `max` with an
// error naming the node; stateBytes()/initState() clamp to it as a last line of defence, so the ring a state
// block is sized for is always the ring its kernels address.
uint32_t maxDelayFramesLimit(uint32_t lanes) noexcept;  // RT-safe

// Bytes of state (0 for stateless atoms). For delay lines: header + ring (≤ sizeof(DelayState) + kMaxDelayRingBytes), computed in
// size_t. Not RT-relevant.
size_t stateBytes(Atom a, const StateInit& init) noexcept;

// Writes the initial state into `state` (stateBytes bytes, kStateAlign-aligned). No allocation: RT-safe, so it
// can also reset a voice's state on the audio thread (cost: proportional to the delay ring for delay lines; the
// VoiceEngine resets one lane of an 8-lane delay line in O(1) through DelayState::start instead).
void initState(Atom a, void* state, const StateInit& init) noexcept;  // RT-safe

}  // namespace pq::dsp
