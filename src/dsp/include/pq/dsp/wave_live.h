#pragma once

// The Wavetable's live path and Shape mode (TMM.8; docs/plans/MM.md Decisions → DSP, SPEC §15 2026-11-01 "absolutely
// no aliasing"). Two atoms over a live frame-table slot (frame_table_slot.h; swaps crossfade over kFrameTableFadeSeconds and the
// slot's voice tap is written exactly as wavetable_pos does), meant to run inside an `oversample(n)` region:
//
// ---- wavetable_live (phase, position, warp, fold, sync; table, mode, corr) ------------------------------------------
// Warp / fold / sync applied per sample with the shared formulas (wave_warp.h, evaluated in float):
//   ψ = frac(φ·s), s = 1 + 15·sync;  taps a (and b, Mirror) = warpPhase(mode, ψ, warp);
//   v = (1 − m)·x(a) + m·x(b) (m = |warp| in Mirror, else 0);  y = foldValue(v, fold).
// Controls are clamped to their ranges (warp [−1, 1], fold and sync [0, 1]); `mode` is a const (0 Bend, 1 Mirror,
// 2 Pulse width). A param-rate (Spl) control ramps linearly from its previous block's value over the block (a
// lane's first block starts at the value; AU.fold: the previous block's LAST value, also when that block had it per
// sample — the compiler's param ramp, TX.smooth — so the end of a ramp never jumps back), so automation and macros
// never step the waveform; an audio-rate one is used per sample.
//
// Level choice (TMM.8-fix: the same rule as the live-band bake, so baked and live agree): per sample from each tap's
// instantaneous BASE-RATE speed |a′·ψ′ + ∂a/∂warp·Δwarp| with ψ′ = e·s + φ·Δs, where e is the representative
// increment of the base level the phase increment falls in (FrameTableView::bucketTop of n × the region-rate
// increment, the smaller of the last two |Δφ|; e is constant over a mip level, so within a level the band depends
// only on the controls), the deltas taken per base sample (Mirror's second tap: |ψ′|), times liveSpeedScale (½ ·
// the fold's spread 1 + θ). A level then keeps harmonics whose instantaneous frequency stays below 2·(sr − P)/sr·sr
// (56 kHz at 48 kHz) at any factor n ≥ 2: far above 20 kHz and far below n·sr − 20 kHz, where the warp's and fold's
// spread would fold back. Each read crossfades the bucket's level j with the next richer level j − 1 by a weight linear in the speed,
// 1 where the buckets switch to level j and 0 where they switch to j + 1 (FrameTableView::bucketRich / bucketTop /
// bucketInvSpan), so a speed that changes inside a cycle (bend, pulse width, a modulated control) changes the harmonic
// content continuously instead of in steps.
// Cost: two reads per tap (Mirror: four).
//
// Corrections (`corr` 1; the expansion sets it when warp or sync is live): the composed read has jumps and kinks at
// known places — a sync reset (φ wraps while s is not an integer: a value jump and a slope jump), the cycle wrap of
// ψ (Bend and Pulse width have different slopes at its two ends; Mirror's second tap turns), Mirror's turn at ½ and
// Pulse width's at w (slope jumps). Each one found between two samples is corrected with band-limited residuals:
// value jump J·r1 + slope jump K·r2 over the 2·kLiveCorrHalf samples around it (r1 / r2: a Kaiser-windowed sinc,
// cutoff 0.495, β = 9.5, half-width 4, integrated once / twice, minus the ideal step / ramp: linear phase, so the
// step's in-band content stays aligned with the waveform), J and K evaluated in double at the event (the left side at
// the previous sample's level pair, the right side at this sample's: a band change at a kink is corrected too; and
// Mirror's corner where the warp crosses 0). For this the output is delayed by kLiveCorrHalf samples
// (inside a region: kLiveCorrHalf / n base samples). With `corr` 0 there is no ring and no delay.
// TMM.18: φ may run backwards (a negative frequency): the speeds are signed by the direction of the wrapped Δφ, a
// wrap is either way, a kink is crossed either way, and each event steps from the value just before it in time to
// the value just after it (backwards: from the right limit to the left one), so a backward phase at static controls
// gives the forward output time-reversed (equal spectra within −130 dB).
//
// ---- wavetable_shape (x, position; table) ----------------------------------------------------------------------
// Shape mode: the table as a transfer curve. u = (x + 1)/2, the curve g(u) = T(clamp(u, 0, 1)) (T the frame's cycle;
// T(1) = T(0): the reader's phase wraps, so the curve is continuous where the clamp takes over). First-order ADAA:
//   y = (G(u₁) − G(u₀)) / (u₁ − u₀),  G(u) = c₀·cu + P(cu) + T(0)·(u − cu),  cu = clamp(u, 0, 1),
// with c₀ the frame's DC and P the periodic part of its antiderivative (FrameTableView::adData, built with
// FrameTableSpec::antiderivative). ΔG is formed from the parts (c₀·Δcu + ΔP + T(0)·(Δu − Δcu)), so the DC and the
// clamped ramps are exact; both endpoints are read with the same level pair and frame. |u₁ − u₀| < kShapeAdaaMin:
// the midpoint read T(clamp((u₀ + u₁)/2)) instead. Level: from the held speed h = max(|Δcu|, h·e^(−1/(τ·sr))),
// τ = kShapeHoldSeconds, crossfaded as wavetable_live does, so a steady input keeps a steady level (a level that
// followed |Δu| through every cycle of a sine would step its content twice per cycle). Cost: four P reads (two
// levels × two endpoints) + one value read when a lane falls back.
//
// wavetable_live chooses levels from base-rate speeds × liveSpeedScale; wavetable_shape from its held speed × liveHeadroom.
// RT: both kernels are RT-safe (no allocation; the residual tables are built at static initialisation).

#include <cmath>
#include <cstdint>

#include "pq/dsp/frame_table.h"
#include "pq/dsp/wave_warp.h"

namespace pq::dsp {

inline constexpr uint32_t kLiveCorrHalf = 4;         // residual half-width (samples) = the output delay with corr
inline constexpr uint32_t kLiveResidualSteps = 1024;  // residual table resolution per sample
inline constexpr float kShapeAdaaMin = 1.0f / 4096.0f;  // |Δu| below which Shape mode reads the midpoint
inline constexpr double kShapeHoldSeconds = 0.05;

// Shape mode's input → table position: u = clamp((x + 1)/2, 0, 1) (NaN → 0). The one mapping the kernel's voice tap
// writes and the Wavetable face draws its x axis with (x = −1 … 1 spans the display; BR.wt).
inline float shapeTablePosition(float x) noexcept {  // RT-safe
  return std::fmin(std::fmax(std::fma(x, 0.5f, 0.5f), 0.0f), 1.0f);
}


// The level pair a read at `speed` (cycles per sample, ≥ 0) uses: the bucket's level info, the richer level's info
// and the richer level's weight in [0, 1]. Exactly as the kernels compute it (float). RT-safe.
struct LiveLevels {
  uint32_t info = 0;
  uint32_t rich = 0;
  float wRich = 0;
};
LiveLevels liveLevels(const FrameTableView& t, float speed) noexcept;

// One read in double at a level pair: value and derivative per cycle of the read phase. The frame pair and weight
// come from the float position as the readers choose them. RT-safe.
double liveRead(const FrameTableView& t, float position, const LiveLevels& lv, double phase, double* slope) noexcept;

struct LiveControls {
  WarpMode mode = WarpMode::Bend;
  double warp = 0, fold = 0, sync = 0;  // clamped by the functions
};

// The live path's value at input phase φ ∈ [0, 1) in double (tests' reference, and what the kernel's corrections
// evaluate): ψ, the taps, each tap read at the level pair for speed inc·s·|tap′|, the fold. *dydphi: dy/dφ (side:
// −1 = the left limit, +1 = the right limit at a breakpoint; ψ itself is passed as computed, so a caller asks for the
// left limit at a wrap with phase → 1). RT-safe.
double liveValue(const FrameTableView& t, float position, const LiveControls& c, double phase, double inc,
                 int side = 0, double* dydphi = nullptr) noexcept;
// The same from ψ (after sync) instead of φ, with s given: what the corrections use at a reset (ψ = frac(s) from the
// left, 0 from the right). dydpsi: dy/dψ.
// `inc` is the BASE-rate phase increment (the default speeds use its representative liveRepIncrement); speedA /
// speedB ≥ 0: the taps' level speeds as given (what the kernel chose for the sample, scaled by liveSpeedScale).
double liveValueAtPsi(const FrameTableView& t, float position, const LiveControls& c, double psi, double inc,
                      int side, double* dydpsi, double speedA = -1, double speedB = -1) noexcept;

// The warp's taps at ψ ∈ [0, 1] with their slopes (side −1 / +1: the left / right branch at a breakpoint).
struct LiveTaps {
  double a = 0, da = 1, b = 0, db = 1, m = 0;
  double dadw = 0;  // ∂a/∂warp (a modulated warp moves the read: part of its speed)
};
LiveTaps liveTaps(WarpMode mode, double psi, double warp, int side) noexcept;
// The factor a tap's base-rate speed is multiplied by before its level is chosen: kLiveBandScale times the fold's
// spread 1 + θ (a sine folder widens its input's band about that much).
inline constexpr double kLiveBandScale = 0.5;
double liveSpeedScale(double fold) noexcept;
// The representative increment of the base mip level `baseInc` falls in (the top of its last bucket). RT-safe.
float liveRepIncrement(const FrameTableView& t, float baseInc) noexcept;
// The live read of frame `frame` at static controls with the levels of representative increment incRep, at n phases
// (float, 8 at a time): what the live-band bake (FrameTableSpec::liveBand) samples. RT-safe.
void liveRenderStatic(const FrameTableView& t, uint32_t frame, const LiveControls& c, float incRep,
                      const float* phase, uint32_t n, float* out) noexcept;
// Shape mode's band headroom at an oversampling factor: log2(n) (1 at n ≤ 2), on its region-rate held speed.
float liveHeadroom(uint32_t factor) noexcept;
// The oversampling factor a kernel runs at: its state's rate over the table's (1, 2, 4, 8, 16).
uint32_t liveFactor(float stateRate, const FrameTableView& t) noexcept;

// Mirror only: the output's slope jump (per unit of warp change) where the warp crosses 0 at ψ — the mix |warp| has
// a corner there and the second tap changes side: K = Δwarp·sign(new)·(x(b_new) + x(b_old) − 2·x(a))·fold′, the
// taps read at the given level speeds. RT-safe.
double liveMirrorCrossing(const FrameTableView& t, float position, const LiveControls& c, double psi, bool toPositive,
                          double speedA, double speedB) noexcept;

// The residuals at offset k + d from an event (k ∈ [−kLiveCorrHalf, kLiveCorrHalf), d ∈ [0, 1]): the band-limited
// step minus the ideal step (r1) and the band-limited ramp minus the ideal ramp (r2), per unit jump / unit slope
// jump per sample. RT-safe.
void liveResidual(int32_t k, double d, double* r1, double* r2) noexcept;

// Shape mode's curve at u (clamped) in double at a level pair (the reference). RT-safe.
double shapeCurve(const FrameTableView& t, float position, const LiveLevels& lv, double u) noexcept;

}  // namespace pq::dsp
