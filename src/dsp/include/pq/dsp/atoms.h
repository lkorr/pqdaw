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
//   sqrt exp log sin tan tanh (x)     exact: SVML at every lane count (sqrt: the instruction), verified ≤ a few ulp
//                                     against double; 1 lane == 8 lanes bit for bit (Q94)
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
//                                     seconds by sr), fractional, clamped to [1, max] linear, [1, max] cubic
//                                     (4-point Hermite from 2 samples; in [1, 2) linear between the taps 1 and 2
//                                     samples old, continuous at 2), [1.5, max] allpass (first-order allpass,
//                                     fractional part kept in [0.5, 1.5) by moving one sample into the integer part);
//                                     NaN → the minimum. Every tap is ≥ 1 sample old, so x is a DELAYED input
//                                     (a feedback loop through a delay line is legal). max =
//                                     StateInit::maxDelayFrames.
//   delay_fade (x, delay, fade, crossfade)  (DT.2) delay_cubic's line and read (the same clamp and taps) whose read
//                                     head never jumps. `fade` in FRAMES (the compiler multiplies seconds by sr; Spl;
//                                     whole, ≥ 1, NaN → 1). `crossfade` (DT.4; Spl, per lane): < 0.5 REPITCH: a change
//                                     to t glides the head from where it is (with its velocity, clamped to the
//                                     monotone range) along a cubic Hermite arriving at t at rest, over
//                                     N = ⌊clamp(|Δ|·(Δ > 0 ? 2 : 1/2), G, ⌊fade·100/3⌋)⌋ frames (Δ = t − h: the
//                                     read rate stays within 1/4 … 4 unless the cap shortens it); never a fade.
//                                     ≥ 0.5 (and NaN) CROSSFADE, DT.2's rule. Per lane the head starts at the delay;
//                                     in Crossfade a later change of the (clamped) delay to t, from where the head is
//                                     (h), with G = fade frames for a Spl delay and 1 for a Blk one:
//                                       −G ≤ t − h ≤ G/2  the head glides linearly to t over G frames (restarted from
//                                                     where it is by a later change): a pitch within one octave
//                                       otherwise      a CROSSFADE: a second head stays at h and fades out while the
//                                                     main one, at t, fades in, equal power (sin / cos) over `fade`
//                                                     frames; a change that cannot glide during a fade waits for its
//                                                     end (then glides or fades from where the head is).
//                                     At rest it is delay_cubic at the head (bit for bit); the second read runs only
//                                     during a fade. Split read: delay_fade:read (delay, fade, crossfade) → out with delay:write.
//   Split kernels for feedback loops scheduled per sample (same state block as the atom; not atoms):
//     z1:read () → out, z1:write (x): read emits the stored value, write stores x (last frame).
//     delay:read / delay_cubic:read / delay_allpass:read (delay) → out, delay_fade:read (delay, fade, crossfade) → out, and
//     delay:write (x).
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
//   noise_step (rate, trig, beat, grid, reset, smooth, colour, density, steps, seed, dist, lanes)
//                                     NZ.1 (kernels_noise_step.cpp, pq/dsp/noise_step.h; the Noise module's engine):
//                                     a value DRAWN per step, held or glided between steps. A step happens on a frame
//                                     where any of: the rate's uint32 phase wraps (rate Hz, phase += round(rate ·
//                                     2^32/sr): fractional periods exact on average; ≥ sr: every frame; ≤ 0 / NaN:
//                                     never); trig rises; floor(beat / grid) changes (grid > 0 beats, the sync grid);
//                                     reset rises; or a lane runs its first frame (a fresh state, a voice's note-on).
//                                     A step by trig or a first frame puts the rate's phase at 0 (its next step a
//                                     period later); a rising reset also restarts the lane's stream (from its start)
//                                     and clears its colour history. The draw at frame t is
//                                     hash(key + t − origin) (lowbias32, t the state's frame counter, origin the
//                                     lane's last restart): uniform [−1, 1) (dist 0) or Irwin–Hall 4 at σ = 1/3,
//                                     |x| ≤ 1.155 (dist 1); key = hash(lane seed ^ voice group) (lanes 1) or the
//                                     node's seed for every lane (lanes 0), mixed with round(seed). Then per step:
//                                     colour (dB/oct, −6 … 6: the tilt cascade of noise_step.h on the drawn sequence;
//                                     0: none), fold into [−1, 1] by reflection (colours > 0: none — the cascade's
//                                     input is scaled by the design's bound, so it never leaves [−1, 1]: noise_step.h),
//                                     density (Hz: the step keeps its value
//                                     with probability density · period / sr, else outputs exactly 0; ≥ 100 kHz:
//                                     always), steps (round(steps) = N ≥ 2: N levels across [−1, 1]; 1: 0; ≤ 0: off),
//                                     then a glide from the current output over smooth · period frames (smooth 0 … 1,
//                                     smoothstep u²(3 − 2u): monotone, never past either value; 0: a jump; exactly
//                                     the new value once reached). The period: sr / rate where the rate runs, else
//                                     the frames since the lane's previous step (unknown before one: no glide,
//                                     density keeps). dist and lanes are consts; colour, steps, seed and grid are not
//                                     glided (TX.smooth: a colour change recomputes its design, bitwise compared);
//                                     every input any rate. Output in [−1, 1]. Any split of the frames into calls
//                                     gives the same bits.
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
//                                     until the next reset. While the playhead is inside the loop, the
//                                     interpolation taps past the seam come from the other end of the loop
//                                     (seamless loops; M1 review L3); taps outside the data read 0. No
//                                     crossfade. start/loopStart/loopEnd must be Spl.
//                                     Initially playing from 0.
//   sample_pos_linear/_cubic (pos)    stateless read at a sample position (float, in samples); 0 outside
//                                     [0, length). Float position: sub-sample accurate to ~2^24 samples.
// Cubic = 4-point, 3rd-order Hermite (Catmull-Rom); linear = 2-point.
// Tuning (T2.6, SPEC §3.3; pq/dsp/tuning_table.h):
//   tuning_hz (degree, equave, hz, cents, generator, period, stretch, reference) → (hz, cents)
//                                     the pitch of Degree `degree` of equave `equave` in the resource's EDO / MOS
//                                     linear form at the given (modulatable) generator, period (EDO: equave),
//                                     stretch and reference, computed in double and rounded once: out.hz =
//                                     reference · 2^(cents/1200), out.cents = the stretched cents from the reference.
//                                     A lane without a finite degree/equave, or whose `hz` input is 0 (a voice before
//                                     its onset), outputs its `hz` / `cents` inputs unchanged (the fallback). Region
//                                     crossings are found at the sample where the generator leaves its interval.
//                                     Reads KernelArgs::splMask at run time (one function per lane count).
//                                     T2.6-fix3: two more inputs, `select` (voice.tuning) and `node` (this node's
//                                     number): with node ≥ 1 a lane is this node's only if select == node, and a
//                                     lane of this node without a degree follows the reference (hz · reference /
//                                     reference₀); a Degree lane's hz input over its static degree pitch (a note's
//                                     ratio to its anchor, a glide) multiplies the computed pitch (kernels_tuning.cpp).
// Smoothed gains (T3.17-fix2, M3 review H1; the mixer's gain stages and sends, kernels_gain.cpp):
//   gain (x, g, time, start)          x · y, where y follows g as a one-pole with time constant `time` (samples;
//                                     ≤ 0: no smoothing): y_k = g + (y₀ − g)·e^(−k/time) over a block's frames
//                                     k = 0, 1, … (y₀ = the previous block's last value, so a frame reads y before
//                                     its update; 1 lane: frame by frame, y = g + d, d ← d·e^(−1/time), so any split
//                                     of the frames into calls gives the same bits, T4.x-fix-dsp). Within 3e-8 of g
//                                     (≈ −150 dB) y snaps onto g; a settled gain costs one multiply per sample (g = 0:
//                                     a zero fill).
//                                     `g` is normally a param (Spl): a block-rate target. A Blk (audio-rate) g is
//                                     used as it is plus the residual a param block left, decaying as above (TX.smooth:
//                                     a param ramp feeds g as a block while it moves; a ramp holding a constant g gives
//                                     the param path's bits; at g = 0 without residual the 1-lane output is exactly
//                                     acc or 0). Non-finite g counts as 0. With `start` 0, y starts
//                                     at 0, so a new instance (a voice lane after its reset) fades in from silence;
//                                     with `start` 1 (T4.x-fix-dsp: device params) its first block starts at g.
//                                     `time` and `start` must be Spl (start may be omitted: numIn 3 reads it as 0).
//   gain_add (acc, x, g, time, start) acc + x · y, y as in gain. Settled at g = 0 it is a copy of acc: nothing at
//                                     all when the output took the accumulator's buffer (in place), so a chain of
//                                     gain_adds (a return's sends) costs nothing for its closed inputs.
// Frame tables and voice taps (TMM.2; kernels_frame_table.cpp; resources frame_table_slot.h, pq/core/voice_tap.h):
//   wavetable_pos (phase, position)   the slot's frame table read by phase (frame_table.h: the level per lane and
//                                     sample from |Δphase|, frames blended at `position` ∈ [0, 1], 6-point read):
//                                     readFrameTableL1 / L8 exactly. One acquire load of the slot per call; when its
//                                     generation changed since the state's, the next 64 samples of this state's
//                                     frame count crossfade linearly from the previous table (out = old + g·(new −
//                                     old), g = (k + 1)/64, exactly new at g = 1; both read on the same phase history)
//                                     in a separate loop, taken only then. A fresh state (generation 0) does not
//                                     fade. Writes the slot's voice tap (the phases read, wrapped to [0, 1), the
//                                     position and |Δphase| per lane) only while a reader is registered. A Spl phase
//                                     is expanded in 64-frame chunks. Any split of a block into calls gives the same
//                                     bits. TMM.12-fix1: a sine table reads as a sine (frame_table.h "Sine tables"); each
//                                     call leaves WavetablePosState::sineOn for the JIT's inline frames.
//   voice_tap (x) → (nothing)         a sink: samples x into the VoiceTap resource (null: nothing) while a reader is
//                                     registered; the record's increment is x's last difference, position 0.
// The Wavetable's live path and Shape mode (TMM.8; kernels_wave_live.cpp; pq/dsp/wave_live.h has the formulas):
//   wavetable_live (phase, position, warp, fold, sync, mode, corr)
//                                     the slot's frame table read through sync → warp → fold applied per sample
//                                     (wave_warp.h), each tap at the level pair for its instantaneous speed
//                                     (crossfaded), param-rate controls ramped over the block, jumps and kinks
//                                     corrected with band-limited residuals when corr ≠ 0 (output delayed by
//                                     kLiveCorrHalf samples); mode and corr are consts. Every signal input may be
//                                     Spl or Blk (the kernel reads KernelArgs::splMask). Swaps and taps as
//                                     wavetable_pos (the tap at the base rate inside an oversample region).
//   wavetable_shape (x, position)     Shape mode: the table as a transfer curve of x ∈ [−1, 1] (clamped), first-order
//                                     ADAA from the table's antiderivative arrays (a table without them: silence).
// Oversample region converters (TMM.6; pq/dsp/oversample.h has the filters, the conventions and the numbers):
//   oversample_up / _down (x, factor)   polyphase IIR half-band cascade, ×factor up / down (factor Spl: 2, 4, 8, 16)
//   oversample_phase (x, factor)        a phase up by unwrapped linear interpolation (exact for ramps, keeps wraps)
//   oversample_hold / _any (x, factor)  bool up (each mask repeated) / down (any sub-sample high)
//                                     frames = the oversampled count: up-type kernels read frames / factor samples
//                                     and write frames, down-type read frames and write frames / factor.
// Param smoothing (TX.smooth; kernels_ramp.cpp; inserted by the compiler, pq/graph/schedule.h "Param ramps"):
//   param_ramp (x, frames) → out     x a param-rate value (Spl), frames the ramp length (Spl, a const ≥ 1). Per lane:
//                                     its first call starts at x; when x changes (bitwise) the lane ramps linearly
//                                     from where it is to x over `frames` samples (a later change restarts the ramp
//                                     from where it is then); a non-finite x or position jumps. The output (a block)
//                                     is written ONLY in a call where some lane ramps (ParamRampState::active), with
//                                     every lane's per-frame value: target − inc·(rem − k − 1) while frames remain,
//                                     then the target. A call where no lane ramps writes nothing (its consumers read
//                                     x as the pattern it is); paramRampHold fills the output with the lanes' values
//                                     (= x) when a consumer needs it as a block anyway. Any split of a block into
//                                     calls gives the same bits.
// Analysis taps (TX.spectrum; kernels_signal_tap.cpp; the ring pq/core/tap_ring.h, its readers pq/views/devices/
// analysis.h):
//   signal_tap (a, b, c, d) → (nothing)   a global sink: copies its four inputs (a device's input L / R and output
//                                     L / R) into the SignalTap resource's ring (null: nothing) while a reader is
//                                     registered (TapRing::wanted: unwatched one relaxed load per call). Every input
//                                     may be Spl or Blk (a param-rate input is repeated); 8 lanes: the lanes summed.
// Control curves (GU.1; kernels_curve.cpp; the model, its double evaluator and the coefficient form in pq/dsp/curve.h):
//   curve (x, convert) → value        a driven control's curve at knob position x (any rate): the slot's CurveData
//                                     (Resource::Curve, a dsp::CurveSlot: one acquire load per call), clamp / wrap /
//                                     signed by its mode, the segment found by comparison (several segments: the
//                                     coefficients gathered per lane), the shape (exact expm1 / log1p / pow), then the
//                                     step and — when the const `convert` ≠ 0 — the output conversion (dB → gain:
//                                     exactly db_to_gain's). Element-wise and stateless; equals curve.h evalCurve to
//                                     float rounding. Without a slot (a lowering without the link's curve slots) it is
//                                     the identity on x.
// Filters (EQ.1; kernels_svf.cpp; the designer, the modes and the exact response in pq/dsp/svf_design.h):
//   svf (x, cutoff, q, gain, mode, stages, prewarp)
//                                     `stages` cascaded TPT state-variable sections (1 … 64; bell, notch: 1), each
//                                     with an output mix over hp / bp / lp / x, so every mode is one recursion per
//                                     section. cutoff in Hz (clamped to [1e−3, 0.49·sr]), q, gain a linear amplitude
//                                     (bell and shelves; ≤ 0 reads −120 dB); mode 0 … 7 = lowpass, highpass, bandpass,
//                                     notch, bell, lowshelf, highshelf, allpass (SvfMode, rounded and clamped);
//                                     prewarp 0 = matched (poles impulse-invariant, numerator magnitude-matched:
//                                     no cramping near Nyquist), 1 = bilinear (plain Simper). mode, stages, prewarp are
//                                     consts (Spl); x, cutoff, q, gain any rate. Per lane the coefficients are designed
//                                     (double, svfDesign) when that lane's inputs change bitwise: once per change at
//                                     param rate, per frame at audio rate. The output is exactly svfResponse of the
//                                     design (to float rounding of the recursion); any split of the frames into calls
//                                     gives the same bits. A non-finite or > 1e30 state is reset to 0 after each call.

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
  // tuning as a signal (T2.6)
  TuningHz,
  // smoothed gains (T3.17-fix2: the mixer)
  Gain, GainAdd,
  // frame tables and voice taps (TMM.2: meta modules)
  WavetablePos, VoiceTap,
  // oversample region rate converters (TMM.6, pq/dsp/oversample.h; inserted by the compiler, not callable)
  OversampleUp, OversampleDown, OversamplePhase, OversampleHold, OversampleAny,
  // the Wavetable's live path and Shape mode (TMM.8, pq/dsp/wave_live.h)
  WavetableLive, WavetableShape,
  // TX.smooth: the compiler's param smoothing (inserted by schedule(), not callable; see ParamRamp below)
  ParamRamp,
  // TX.spectrum: the analysis-tap sink (see "Analysis taps" above)
  SignalTap,
  // GU.1: a driven control's curve (see "Control curves" above)
  Curve,
  // EQ.1: the state-variable filter cascade (see "Filters" above)
  Svf,
  // DT.2: the delay line whose read head glides or crossfades (see "Memory" above), and its split read
  DelayFade, DelayReadFade,
  // NZ.1: the noise module's stepped random source (see "Generators" above)
  NoiseStep,
  Count_
};
inline constexpr uint32_t kAtomCount = static_cast<uint32_t>(Atom::Count_);
inline constexpr uint32_t kSignalTapChannels = 4;  // signal_tap's inputs: in L, in R, out L, out R

// TMM.2: FrameTable = a dsp::FrameTableSlot (frame_table_slot.h), VoiceTap = a pq::VoiceTap (pq/core/voice_tap.h).
// TX.spectrum: SignalTap = a pq::TapRing of kSignalTapChannels channels (pq/core/tap_ring.h).
// GU.1: Curve = a dsp::CurveSlot (curve.h).
enum class Resource : uint8_t { None, Wavetable, Shaper, Sample, Tuning, FrameTable, VoiceTap, SignalTap, Curve };

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

// gain / gain_add, 1 lane, param g (T4.x-fix-dsp): what a call does before its frames (prepare the coefficient for
// `tau` samples, retarget to g, honour `start`), so that generated code can run the frames inline (the per-frame
// definition in kernels_gain.cpp: y = target + d; out = (d == 0 && target == 0) ? (acc or 0) : fma(x, y, acc or 0);
// d = d·r, snapped to 0 below kGainSettled). Idempotent within a block (same g, tau, start). RT-safe.
inline constexpr float kGainSettled = 3e-8f;  // |residual| below this (≈ −150 dB): settled (exactly the target)
void gainPrologue(void* state, float g, float tau, float start) noexcept;
// TX.smooth: the same for a call whose g is a block (a param ramp, or audio rate): prepare; a first call honours
// `start` towards g0 (its first frame's g); a started gain keeps its residual (no retarget). RT-safe.
void gainAudioPrologue(void* state, float g0, float tau, float start) noexcept;

struct StateInit {
  double sampleRate = 48000.0;
  uint32_t lanes = 1;            // 1 or 8 (the delay line's ring layout and the svf's section layout depend on it)
  uint32_t maxDelayFrames = 0;   // delay lines: largest delay in samples (≤ maxDelayFramesLimit(lanes))
  uint32_t seed = 0;             // noise: per-node seed (e.g. the NodeId), mixed with the lane index
  float z1Init = 0.0f;           // z1, z1:read, z1:write: the output before the first input (the language's `init`
                                 // input, a const; for a bool z1 pass boolMask(true) or 0)
  uint32_t voiceGroup = 0;       // TMM.2: the voice group a voice-arena state block belongs to (the image host sets
                                 // it per group; 0 in schedules): wavetable_pos / voice_tap write their tap record there
  uint32_t svfSections = kSvfStateSections;  // EQ.1b, svf: the sections its state holds (1 … 64; the compiler gives
                                             // svfSectionCount of its mode / stages consts; the kernel clamps to it)
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
// size_t. svf (EQ.1b): kSvfHeaderBytes + svfSections sections of `lanes` lanes (≤ sizeof(SvfState)). Not RT-relevant.
size_t stateBytes(Atom a, const StateInit& init) noexcept;

// Which bytes of an atom's 8-lane state block belong to one voice lane (M1 review L7): what the VoiceEngine resets at
// a note-on and moves when a voice changes group, without knowing the state structs. Per-lane fields are [8] arrays,
// so inside them word w belongs to lane w mod 8; they come first in every struct (atom_state.h). Declared for every
// stateful atom (tests/dsp/test_lane_state.cpp checks that, and checks each PerLane declaration by running the
// kernel on lane-permuted inputs).
enum class LaneState : uint8_t {
  None,       // stateless (or not declared: a stateful atom must never return this)
  PerLane,    // bytes [0, perLaneBytes) are per-lane [8] arrays; the rest is shared by the group's lanes (phasor's
              // incScale/halfSr, noise's counter; noise's per-lane seeds are constant from StateInit, so 0 bytes)
  DelayLine,  // DelayState: shared header, [8] arrays per lane (delay_fade heads, apY1), start[8] + the ring (VoiceEngine, O(1))
  Whole,      // every word of the entry (whatever its size: svf, sized by its sections) is per lane; the values the
              // group shares are replicated in each lane
};
struct LaneStateInfo {
  LaneState kind = LaneState::None;
  uint32_t perLaneBytes = 0;  // PerLane only
};
LaneStateInfo laneStateInfo(Atom a) noexcept;  // RT-safe

// ---- Silence (SPEC §7.7, T1.x-silence) ---------------------------------------------------------------------------
// Each atom's "silent in → silent out after tail T" declaration, from which the scheduler builds a program's sleep plan
// (pq/graph/schedule.h, "Sleep"). Silent = exactly ±0 (bools: false). Inputs a rule does not name are coefficients
// (any finite value). Checked for every atom by tests/dsp/test_silence.cpp, which runs each kernel on silent inputs.
enum class SilenceRule : uint8_t {
  None,      // no guarantee: generators, values that are not 0 at 0 (exp, cos, db_to_gain, ge/le/eq, not, nand),
             // tables, held values (sample&hold, latch, flip-flops, counters, schmitt)
  Any,       // the output is ±0 when ANY input named in `inputs` is ±0 (mul, and)
  All,       // the output is ±0 when ALL inputs named in `inputs` are ±0 (add, sub, min, max, neg, tanh, lerp(a, b, t), …)
  Clip,      // clip(x, lo, hi): All over {x} when lo ≤ 0 ≤ hi (the scheduler checks constant bounds)
  MulAdd,    // muladd(a, b, c): (a or b) and c
  Memory,    // z1, delay lines, rise/fall/change: the output is the stored history of `inputs` (the split reads: of the
             // write's input). Silent once `inputs` have been silent for silenceTail() frames: the TAIL. svf (EQ.1b):
             // its history decays (hasDecayState): silent once `inputs` are silent and decayStateSmall()
  Smoother,  // gain / gain_add: All over `inputs`; the state (target, residual) is settled once its residual is
             // exactly 0, and stays settled while the `settle` inputs (target, time) keep their values
};
struct SilenceInfo {
  SilenceRule rule = SilenceRule::None;
  uint32_t inputs = 0;  // bit i: input i (mux: every input after the index)
  uint32_t settle = 0;  // Smoother: the inputs whose change moves the state
};
SilenceInfo silenceInfo(Atom a) noexcept;  // RT-safe
// The tail: frames of history a stateful atom keeps (z1 and edge detectors 1; a delay line floor(max) + 3, its
// oldest tap; stateless atoms and smoothers 0).
uint32_t silenceTail(Atom a, const StateInit& init) noexcept;  // RT-safe
// EQ.1b: an atom whose history DECAYS rather than ends (svf: an IIR) is a Memory atom with tail 0 whose state the host
// checks instead: decayStateSmall = every state word (of every lane; the host's are global 1-lane blocks) is ≤ eps (NaN:
// false); decayStateFlush sets those words to exactly 0 (the block's output is then exactly 0 for a silent input) and
// returns whether one was not 0 yet. False / nothing for other atoms. RT-safe.
bool hasDecayState(Atom a) noexcept;
bool decayStateSmall(Atom a, const void* state, float eps) noexcept;
bool decayStateFlush(Atom a, void* state) noexcept;

// Writes the initial state into `state` (stateBytes bytes, kStateAlign-aligned). No allocation: RT-safe, so it
// can also reset a voice's state on the audio thread (cost: proportional to the delay ring for delay lines; the
// VoiceEngine resets one lane of an 8-lane delay line in O(1) through DelayState::start instead).
void initState(Atom a, void* state, const StateInit& init) noexcept;  // RT-safe

// ---- Param smoothing (TX.smooth) ---------------------------------------------------------------------------------
// How long a param-rate value takes to ramp to a new value at an audio-rate input (pq/graph/schedule.h "Param
// ramps"; chosen by measurement, bench/RESULTS.md TX.smooth and QUESTIONS Q?-TX.smooth-a): 20 ms, linear.
inline constexpr double kParamRampSeconds = 0.020;
// The inputs of atom `a` (numIn inputs, for mux the actual count) that the compiler may feed a ramp instead of a
// param-rate value: every real input that can be audio rate, except the ones whose value is a choice or a count
// (mux / demux index, counter / clockdiv n, pulse period, timer duration, tuning_hz entirely (pitch identities, and tuning automation is already per sample)),
// state-init or required-Spl inputs (times, factors, modes). Bit i = input i. RT-safe.
uint32_t rampableInputs(Atom a, uint32_t numIn) noexcept;
// Fills `out` (frames · lanes floats, lanes 1 or 8) with each lane's current value of a param_ramp state that did not
// ramp in this block (its target, i.e. its input): what a consumer reading the ramp as a block sees then. RT-safe.
void paramRampHold(const void* state, float* out, uint32_t frames, uint32_t lanes) noexcept;

}  // namespace pq::dsp
