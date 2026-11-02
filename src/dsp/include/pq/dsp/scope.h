#pragma once

// The oscilloscope's analysis (TX.scope; SPEC §15 2026-11-01 "analysis modules"): which stretch of a signal's recent
// history a scope draws so that the trace stands still. PQ Meter's oscilloscope is the reference (pq meter/src/modules/
// oscilloscope.cpp: its timebases, pitch lock modes, McLeod pitch detector, shape alignment and edge trigger, ported
// in behaviour and numbers; our own code), plus pqdaw's note-locked period:
//
//   Fixed     a span of `windowMs`; the edge trigger (Rising / Falling / Off, `level`, `hysteresis`) chooses the start:
//             walking back from the newest candidate, the signal must first leave the level by the hysteresis, so
//             noise around the level cannot re-trigger; the newest such edge wins (the trace stays current).
//   Pitch     the period of the signal's pitch holds the trace still. The period comes from the NOTE when one is given
//             (ScopeInput::noteHz > 0 and PitchSource::Auto: the module runs per voice and a voice sounds — the exact
//             pitch pqdaw plays, no estimate; the lowest sounding voice when several do), else from the detector:
//               detector   McLeod's normalised square difference over a window of the newest samples (two real FFTs:
//                          the autocorrelation as the transform of the power spectrum, the normalisation from an energy
//                          prefix sum), the first peak ≥ 0.9 × the tallest (the fundamental, not a harmonic), walked down
//                          while the halved lag is still such a peak (no octave-low reading of a pure tone), parabolic
//                          interpolation; nothing below `clarity` (unpitched) or below −60 dBFS RMS. The window is 2.5
//                          periods of `minHz` until a period is known, then 6 of its own periods (≥ 2048 samples: vibrato
//                          does not smear the peak), with a retry over a third of it when that finds nothing.
//               smoothing  a one-pole toward each estimate (0.25 inside ±18 %, 0.06 outside); a move outside ±18 %
//                          held for 6 frames is a new note (taken at once); a lost pitch is held for 30 frames.
//             How far the period may move the span is the lock:
//               Constant   the span is `windowMs`, always (the pitch only aligns the phase and feeds the readout)
//               Stable     the span is `cycles` periods, re-derived only when the period sat ≥ 8 % away for 12 frames
//               Tracking   the span is `cycles` periods of every frame's period
//             The start: the offset within one period back from the newest whose shape (one period resampled to 256
//             points, unit RMS) correlates best with the previous frame's (64 coarse steps, then every sample around
//             the best); the first reference of a note is rotated to its steepest rising zero crossing (a fixed
//             polarity), later frames blend into it (0.3).
//   Beat      handled by ScopeBeat (below): the x axis is project time modulo a musical span.
//
// Not for the audio thread: the UI thread (a scope face) runs it once per displayed frame. Allocates while it grows to
// the largest window; nothing after that.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "pq/dsp/fft.h"

namespace pq::dsp {

struct ScopeSettings {
  enum class Timebase : uint8_t { Fixed, Pitch, Beat };
  enum class Lock : uint8_t { Stable, Tracking, Constant };
  enum class Trigger : uint8_t { Off, Rising, Falling };
  enum class PitchSource : uint8_t { Auto, Detect };  // Auto: the note when known, else the detector
  // PQ Meter's defaults: a constant 52.2 ms span aligned by the tracked pitch, a wide search, a high clarity floor.
  Timebase timebase = Timebase::Pitch;
  Lock lock = Lock::Constant;
  PitchSource source = PitchSource::Auto;
  float windowMs = 52.2f;   // 1 … 2000: Fixed's span, Constant's span, the others' span while nothing is pitched
  int cycles = 1;           // 1 … 16: periods across the width (Stable, Tracking)
  float minHz = 20.0f, maxHz = 7770.0f;  // the detector's range (20 … 500, 200 … 8000)
  float clarity = 0.80f;    // 0.1 … 0.95: an NSDF peak below this is unpitched
  Trigger trigger = Trigger::Rising;
  float level = 0.0f;       // −1 … 1 (full scale)
  float hysteresis = 0.02f; // full scale
};

// What one frame shows: samples [start, start + span) of the window it was given.
struct ScopeFrame {
  size_t start = 0, span = 0;
  float pitchHz = 0.0f;     // the period's frequency (0: none)
  float clarity = 0.0f;     // 0 … 1 (a note: 1)
  bool note = false;        // the period is the note's
  double period = 0.0;      // samples (0: none)
};

class ScopeTracker {
public:
  static constexpr size_t kShapePoints = 256;
  static constexpr size_t kMaxWindow = size_t{1} << 16;  // samples a frame may look at (and the longest span)

  void setSettings(const ScopeSettings& s) noexcept;
  const ScopeSettings& settings() const noexcept { return s_; }
  // Forget the period, the span and the shape (a new signal source, a seek).
  void reset() noexcept;

  // How many of the newest samples the next frame wants (≤ 2 · kMaxWindow + …: the caller copies that many).
  size_t need(double sampleRate) const noexcept;
  // One displayed frame over x[0 … n) (oldest first, the newest last), at `sampleRate`. noteHz > 0: the note's pitch.
  // frozen: nothing moves (the period, the reference and the span stay; the frame is placed as before).
  ScopeFrame frame(std::span<const float> x, double sampleRate, float noteHz, bool frozen);

  // ---- the parts (tests drive them directly) ----
  struct Pitch {
    double period = 0.0;  // samples per cycle (0: none found)
    float clarity = 0.0f;
  };
  // McLeod NSDF over the newest n samples of x.
  Pitch detectPitch(std::span<const float> x, size_t n, double sampleRate);
  // The start in [base − period, base] whose one-period shape best matches the reference (base when there is none).
  size_t alignToShape(std::span<const float> x, size_t base, double period);
  // One period from `start` resampled to kShapePoints values of unit RMS.
  static void sampleShape(std::span<const float> x, size_t start, double period, std::vector<float>& out);
  // Rotates a one-period shape to begin on its steepest rising zero crossing.
  static void anchorShapePolarity(std::vector<float>& shape);
  // The edge trigger's start for a span of winLen (the newest qualifying edge; the default start when none).
  size_t triggerStart(std::span<const float> x, size_t winLen) const noexcept;
  // Stable's pinned span: one frame's step with period `period`, capped at `cap` samples.
  void settleSpan(double period, size_t cap) noexcept;
  size_t stableSpan() const noexcept { return stableSpan_; }
  double smoothedPeriod() const noexcept { return periodSmoothed_; }

private:
  size_t windowLen(double sampleRate) const noexcept;
  size_t analysisLen(double sampleRate) const noexcept;
  RealFft& fftOf(uint32_t n);

  ScopeSettings s_;
  double periodSmoothed_ = 0.0;
  float clarityLast_ = 0.0f;
  int holdFrames_ = 0, pitchJumpFrames_ = 0;
  bool noteLast_ = false;
  size_t stableSpan_ = 0;
  double stableSpanPeriod_ = 0.0, pendingSpanPeriod_ = 0.0;
  int spanSettleFrames_ = 0, stableSpanCycles_ = -1;
  std::vector<float> shapeRef_, shape_, cand_;
  bool shapeValid_ = false;
  size_t lastStart_ = 0, lastSpan_ = 0;  // frozen frames repeat these
  // The detector's scratch.
  std::unique_ptr<RealFft> fft_;
  std::vector<float> pad_, re_, im_, nsdf_;
  std::vector<double> cum_;
};

// The beat-synced timebase: the x axis is project time modulo `beats` (the transport's beat position, tapped with the
// signal: the downbeat on the left edge, so a groove that repeats repaints in place and only the playhead sweeps).
// 4096 phase slots each keep the min / max of L, R, mid and side of the last pass through them (PQ Meter's beat slots;
// a slot entered anew is cleared, so each pass overwrites the picture as it goes). Frames without a clock (NaN beat:
// nothing played) are skipped.
class ScopeBeat {
public:
  static constexpr uint32_t kSlots = 4096;
  enum Channel : uint32_t { kL, kR, kM, kS, kChannels };
  ScopeBeat();
  void setSpan(double beats) noexcept;  // a change clears the picture
  double span() const noexcept { return span_; }
  void clear() noexcept;
  // Frames of the beat position and the stereo signal (r == l for mono).
  void ingest(const float* beat, const float* l, const float* r, size_t n) noexcept;
  // The playhead as a phase 0 … 1 (−1: none yet); the tempo the clock moved at (bpm; 0: unknown) at `sampleRate`.
  double phase() const noexcept { return phase_; }
  double bpm(double sampleRate) const noexcept { return beatPerSample_ * sampleRate * 60.0; }
  bool moving() const noexcept { return moving_; }
  // Re-binned into `cols` columns: per column min, max of `channel` and whether a pass filled it.
  void columns(uint32_t channel, uint32_t cols, std::vector<float>& mn, std::vector<float>& mx,
               std::vector<uint8_t>& ok) const;

private:
  struct Slot {
    float mn[kChannels], mx[kChannels];
    uint32_t n;
  };
  std::vector<Slot> slots_;
  double span_ = 4.0, phase_ = -1.0, beatPerSample_ = 0.0;
  int64_t cur_ = -1;
  float lastBeat_ = 0.0f;
  bool haveLast_ = false, moving_ = false;
};

}  // namespace pq::dsp
