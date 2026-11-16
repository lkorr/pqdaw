#pragma once

// The loudness analysis (VZ.4; docs/plans/VZ.md "VZ.4"): the one front end behind the Level Meter, the LUFS Meter and
// the Loudness History. Ported from PQ Meter (pq meter/src/dsp/loudness.h: the K-weighting, the bounded gating
// histogram; modules/level_meter.cpp: the level ballistics and the clip latch; modules/lufs_meter.cpp: the 100 ms grid,
// momentary / short-term / integrated, LRA, sample and true peak, correlation, session maxima, the quiet auto-reset;
// modules/loudness_graph.cpp: the history of 100 ms points), with its numbers. What is computed is chosen by `parts`:
//
//   Levels     per channel: the RMS (one-pole of the mean square over `rmsWindowMs`), the instantaneous peak falling at
//              `peakFallDbS`, the peak hold (held `holdSeconds`, or forever, then falling at the peak fall), the clip
//              lamp (latched on the first sample ≥ `clipThreshDb`; an EVENT count: a run of overs is one clip; cleared
//              by resetClips or `clipResetS` after the last over). Sample peak, not true peak.
//   Loudness   ITU-R BS.1770-4: K-weighting (PQ's parametrised shelf + high-pass: the spec's 48 kHz table at 48 kHz),
//              the channels' weighted squares summed per 100 ms sub-block; momentary = the last 4 (400 ms), short-term
//              = the last 30 (3 s); integrated = every 400 ms block (75 % overlap) gated at −70 LUFS and −10 LU below
//              their mean (LoudnessHistogram: bounded, the mean of exact powers); EBU R128 LRA = the 3 s blocks on a 1 s
//              grid gated at −20 LU, 95th − 10th percentile (from 3 blocks). Unweighted RMS and the L/R correlation over
//              300 ms windows. Sample peak max since the reset. Session max momentary / short-term on a 10 ms grid (a
//              burst off the 100 ms grid reads its level; Tech 3341). After a reset a block counts once it holds
//              only audio from after it. Auto-reset: the
//              momentary ≤ target − `autoResetBelowLu` for `autoResetSec` clears the integrated measurement only.
//   TruePeak   the ITU-R BS.1770-4 Annex 2 4× estimate — the same filter and arithmetic as the `truepeak` atom (pq/dsp/
//              atoms.h, DY.5; tests compare them): the max since the reset, and a falling value (0.6 s).
//   History    one point (momentary, short-term, integrated at that moment) per 100 ms sub-block, kHistory of them.
//
// Not for the audio thread: the analysis worker runs it (pq/views/devices/analysis.h, kWantLoudness). Allocation-free
// in push() (everything is sized by configure). The worker reads it only while a face shows it; at rest (atRest: the
// input silent for 3.5 s, every falling level under −100 dB, no auto-reset or clip reset pending) more silence changes
// nothing, so the worker stops feeding it.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace pq::dsp {

// What a LoudnessMeter computes (LoudnessConfig::parts).
inline constexpr uint32_t kLoudLevels = 1u, kLoudLoudness = 2u, kLoudTruePeak = 4u, kLoudHistory = 8u;

struct LoudnessConfig {
  double sampleRate = 48000.0;
  uint32_t parts = kLoudLevels | kLoudLoudness | kLoudTruePeak | kLoudHistory;
  // Levels (PQ level_meter's defaults).
  float rmsWindowMs = 300.0f, peakFallDbS = 20.0f, holdSeconds = 2.0f;
  bool holdInfinite = false;
  float clipThreshDb = 0.0f, clipResetS = 0.0f;  // clipResetS 0: latched until reset
  // Loudness (PQ lufs_meter's): the auto-reset's quiet line is an offset below the target (it follows the target).
  float targetLufs = -8.0f;
  bool autoReset = true;
  float autoResetBelowLu = 12.0f, autoResetSec = 5.0f;
  bool operator==(const LoudnessConfig&) const = default;
};

inline constexpr float kNoLoudness = -120.0f;  // "nothing to report" (PQ's sentinel: silence, too short, gated out)

struct LoudnessReadings {
  // Levels (dBFS; −120: nothing).
  float rmsDb[2] = {kNoLoudness, kNoLoudness}, peakDb[2] = {kNoLoudness, kNoLoudness}, holdDb[2] = {kNoLoudness, kNoLoudness};
  bool clip[2] = {false, false};
  uint32_t clipCount[2] = {0, 0};
  // Loudness (LUFS / LU / dBFS / dBTP; −120: nothing).
  float momentary = kNoLoudness, shortTerm = kNoLoudness, integrated = kNoLoudness;
  float lra = 0.0f;                    // LU (0 until 3 short-term blocks survive the gate)
  bool hasLra = false;                 // ... they have: lra is a measurement (a steady tone's 0.0 too)
  float samplePeakDb = kNoLoudness;    // max |x| since the reset
  float truePeakDb = kNoLoudness;      // max true peak since the reset (TruePeak off: the sample peak)
  float truePeakNowDb = kNoLoudness;   // the falling true peak
  float correlation = 0.0f;            // −1 … +1 over the last 300 ms (0: silence)
  float rmsDbMono = kNoLoudness;       // unweighted (L² + R²) / 2 over the last 300 ms
  float maxMomentary = kNoLoudness, maxShortTerm = kNoLoudness;
  float minShortTerm = kNoLoudness;    // the session's quietest short-term (the History's S range)
  uint64_t points = 0;                 // history points made so far (free-running)
  uint64_t historyStart = 0;           // the first point since the last resetMeasurement
  bool operator==(const LoudnessReadings&) const = default;
};

struct LoudnessPoint {
  float momentary = kNoLoudness, shortTerm = kNoLoudness, integrated = kNoLoudness;
};

// PQ Meter's bounded accumulator for gated BS.1770 statistics (dsp/loudness.h LoudnessHistogram): per 0.01 LU bin the
// exact sum of the blocks' mean squares and their count, so the gated MEAN is exact and only gate membership is
// quantised (≤ one bin). Blocks at or below −70 LUFS (the absolute gate) are dropped by add().
class LoudnessHistogram {
public:
  static constexpr double kMinLufs = -70.0, kMaxLufs = 5.0, kBinLu = 0.01;
  static constexpr int kBins = static_cast<int>((kMaxLufs - kMinLufs) / kBinLu + 0.5);  // 7500
  LoudnessHistogram() : bins_(kBins) {}
  void add(double meanSquare) noexcept;  // RT-safe
  bool empty() const noexcept { return n_ == 0; }
  uint64_t count() const noexcept { return n_; }
  void clear() noexcept;  // RT-safe
  // The mean loudness of the blocks above (mean of all − offsetLu): offsetLu 10 is the integrated gate. −120: none.
  float gatedLoudness(double offsetLu) const noexcept;
  // EBU R128 LRA: 95th − 10th percentile of the blocks above (mean − 20 LU); 0 until 2 survive.
  float loudnessRange() const noexcept;

private:
  struct Bin {
    double sum = 0.0;
    uint64_t n = 0;
  };
  int gateStartBin(double offsetLu) const noexcept;
  double percentile(double q, int start, uint64_t n) const noexcept;
  std::vector<Bin> bins_;
  double sum_ = 0.0;
  uint64_t n_ = 0;
};

// Loudness of a mean square (LUFS): −0.691 + 10 log10(ms); −120 at or below 1e−15.
double loudnessOf(double meanSquare) noexcept;
// The K-weighting pair at `sampleRate` (PQ's parametrised design): b0 b1 b2 a1 a2 each.
void kWeighting(double sampleRate, double shelf[5], double highPass[5]) noexcept;
// The `truepeak` atom's estimate over x[0, n) given the 11 samples before it (x[−11 … −1] readable): the max of its
// per-frame outputs about x[−6 … n − 7] — identical arithmetic (AVX2, the standard's mirrored phases). n ≥ 0.
float truePeakMax(const float* x, uint32_t n) noexcept;  // RT-safe

class LoudnessMeter {
public:
  static constexpr uint32_t kHistory = 2048;  // points held (204.8 s: the History's longest window, 180 s, and more)
  explicit LoudnessMeter(const LoudnessConfig& c = {});
  // New options: the measurements continue (a new rate restarts them; parts newly wanted start from nothing).
  void configure(const LoudnessConfig& c);
  const LoudnessConfig& config() const noexcept { return cfg_; }

  // L and R, n frames (a mono signal: the same pointer twice).
  void push(const float* l, const float* r, uint32_t n) noexcept;  // RT-safe
  LoudnessReadings readings() const noexcept;
  // History points [from, from + n) ∩ the held ones into out (from its start); returns the points copied.
  uint32_t history(uint64_t from, uint32_t n, LoudnessPoint* out) const noexcept;
  uint64_t points() const noexcept { return points_; }

  // The LUFS Meter's / History's reset: integrated, LRA, history, peaks, maxima (PQ resetIntegrated / resetMeasurement).
  void resetMeasurement() noexcept;  // RT-safe
  // The Level Meter's: the clip lamps, their counts and the holds.
  void resetClips() noexcept;  // RT-safe
  bool atRest() const noexcept;

private:
  void subBlock() noexcept;
  void fineBlock() noexcept;
  void pushLevels(const float* l, const float* r, uint32_t n) noexcept;
  void pushLoudness(const float* l, const float* r, uint32_t n) noexcept;
  void pushTruePeak(const float* x, uint32_t n, uint32_t c) noexcept;
  double meanOfLast(uint32_t n) const noexcept;

  LoudnessConfig cfg_;
  // Levels.
  float aRms_ = 0.0f, fall_ = 1.0f, clipLin_ = 1.0f;
  double rmsSq_[2] = {0.0, 0.0};
  float peak_[2] = {0.0f, 0.0f}, hold_[2] = {0.0f, 0.0f};
  double holdAge_[2] = {0.0, 0.0}, clipAge_[2] = {0.0, 0.0};
  bool clip_[2] = {false, false}, wasOver_[2] = {false, false};
  uint32_t clipCount_[2] = {0, 0};
  // Loudness: the K-weighting (L, R; transposed direct form II in double), the sub-block grid.
  double shelf_[5] = {}, hp_[5] = {};
  double zs_[2][2] = {}, zh_[2][2] = {};
  uint32_t blockLen_ = 4800, blockPos_ = 0;
  double blockSum_ = 0.0;
  std::array<double, 30> sub_{};
  uint32_t subCount_ = 0, subHead_ = 0;  // sub-blocks made (saturates at 30 for the window), the next slot
  uint64_t stCounter_ = 0;  // sub-blocks since the measurement (re)started: a block joins the gate / LRA when it holds
                            // only audio from after it
  // The session maxima are evaluated on a finer, 10 ms grid (EBU Tech 3341: a 0.4 s burst 50 ms off the 100 ms grid
  // read 0.58 LU low): the last 300 10 ms energies, their count since the reset.
  uint32_t fineLen_ = 480, finePos_ = 0;
  double fineSum_ = 0.0;
  std::array<double, 300> fine_{};
  uint32_t fineCount_ = 0, fineHead_ = 0;
  uint64_t fineSinceReset_ = 0;
  std::unique_ptr<LoudnessHistogram> gated_, lra_;
  float integrated_ = kNoLoudness, lraLu_ = 0.0f;  // cached at each sub-block
  double corrLR_ = 0.0, corrLL_ = 0.0, corrRR_ = 0.0;
  uint32_t corrCount_ = 0, corrLen_ = 14400;
  float correlation_ = 0.0f, rmsMonoDb_ = kNoLoudness;
  float samplePeak_ = 0.0f;
  float maxM_ = kNoLoudness, maxS_ = kNoLoudness, minS_ = kNoLoudness;
  float quietFor_ = 0.0f;
  bool armed_ = false;
  // True peak: per channel the 11 samples before the next block, then the block (kTpChunk at a time).
  static constexpr uint32_t kTpChunk = 1024;
  std::vector<float> tpBuf_[2];
  float truePeak_ = 0.0f, truePeakNow_ = 0.0f;
  // History.
  std::vector<LoudnessPoint> hist_;
  uint64_t points_ = 0, histStart_ = 0;
  // Rest.
  uint64_t quietFrames_ = 0;
};

}  // namespace pq::dsp
