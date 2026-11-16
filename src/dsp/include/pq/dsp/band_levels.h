#pragma once

// Band Levels (VZ.6; ported from PQ Meter's modules/multiband_rms.cpp): the signal split into 3, 4 or 5 bands, each
// band's RMS level tracked over time — a level-versus-time meter of the lows, mids and highs, not a spectrum.
//
//   split      Crossovers: a Linkwitz–Riley tree — the input's lowest band is LR4 low-pass at the first crossover, the
//              rest goes on through the LR4 high-pass and is split again (band k = HP(x0 … xk−1) · LP(xk), the top band
//              all high-passes). LR4 = two Butterworth biquads, −6 dB at the crossover, so the bands sum to an all-pass
//              (BandSplit::process with `compensate`: each band through the 2nd-order all-pass of every crossover above
//              it — the level meter skips it: an all-pass changes no band's level). Independent (3 bands only, PQ's
//              default): a low-pass, a constant-peak band-pass and a high-pass placed freely (the mid's width in octaves,
//              Q = √(2ⁿ) / (2ⁿ − 1)); they may overlap or leave a gap and do not sum flat
//   channel    mono (L + R) / 2, L, R, mid (L + R) / 2, side (L − R) / 2
//   columns    `columnRate` per second: each band's mean square over the column → dB → ballistics (VU: 300 ms up and down;
//              Peak: 5 ms up, 650 ms down; Custom) → one history column of kBands levels (inactive bands at the floor), a
//              ring of kHistory columns
//
// Threads: the analysis worker pushes, the UI reads (read / level); configure from either. An internal mutex guards
// everything (held per pushed chunk and per read: microseconds). push allocates nothing; the constructor does.

#include <cstdint>
#include <mutex>
#include <vector>

namespace pq::dsp {

struct BandLevelsConfig {
  enum class Channel : uint8_t { Mono, Left, Right, Mid, Side };
  enum class Ballistics : uint8_t { Vu, Peak, Custom };
  double sampleRate = 48000.0;
  int bands = 3;                                       // 3 … 5
  bool independent3 = true;                            // 3 bands: independent LP / BP / HP (else crossovers)
  float indepHz[3] = {100.0f, 550.0f, 5000.0f};        // their cutoff / centre
  float indepOctaves = 2.0f;                           // the mid band-pass's −3 dB width
  float crossovers[4] = {250.0f, 4000.0f, 0.0f, 0.0f};  // the active count's bands − 1, ascending
  Channel channel = Channel::Mono;
  Ballistics ballistics = Ballistics::Vu;
  float attackMs = 30.0f, releaseMs = 300.0f;          // Custom
  float columnRate = 60.0f;                            // history columns per second
  bool operator==(const BandLevelsConfig&) const = default;
};

// A biquad section (transposed direct form II; RBJ designs through the bilinear transform).
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1 = 0, z2 = 0;
  float process(float x) noexcept {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void reset() noexcept { z1 = z2 = 0.0f; }
  void lowpass(float fc, float sr, float q = 0.70710678f) noexcept;
  void highpass(float fc, float sr, float q = 0.70710678f) noexcept;
  void bandpass(float fc, float sr, float q) noexcept;  // constant peak gain: unity at fc whatever Q
  void allpass(float fc, float sr, float q = 0.70710678f) noexcept;
};

// The filter bank alone (the meter's split; tests).
class BandSplit {
public:
  static constexpr int kBands = 5;
  void configure(const BandLevelsConfig& c, bool compensate = false) noexcept;
  int bands() const noexcept { return n_; }
  void reset() noexcept;
  // One sample into the active bands' outputs (out[0 … bands − 1]). RT-safe.
  void process(float x, float* out) noexcept;

private:
  struct Lr4 {
    Biquad a, b;
    float process(float x) noexcept { return b.process(a.process(x)); }
    void reset() noexcept {
      a.reset();
      b.reset();
    }
  };
  int n_ = 3;
  bool indep_ = true, compensate_ = false;
  Lr4 lp_[kBands - 1], hp_[kBands - 1];
  Biquad ind_[3];
  Biquad ap_[kBands - 1][kBands - 1];  // ap_[k][j]: band k's all-pass of crossover j (j > k), when compensating
};

class BandLevels {
public:
  static constexpr int kBands = BandSplit::kBands;
  static constexpr uint32_t kHistory = 4096;
  static constexpr float kFloorDb = -140.0f;

  explicit BandLevels(const BandLevelsConfig& c = {});
  // A new configuration (the filters are rebuilt; the history stays). Any thread.
  void configure(const BandLevelsConfig& c);
  BandLevelsConfig config() const;
  // Stereo frames (r == l for mono). Allocation-free; the worker's.
  void push(const float* l, const float* r, uint32_t n) noexcept;
  // Silence: the levels fall in time (the worker, with no data).
  void pushSilence(uint32_t n) noexcept;

  // UI: columns written so far (the ring holds the last kHistory); copies columns [from, from + n) ∩ the held ones
  // into out (kBands floats each, dB) from its start; returns the columns copied.
  uint64_t columns() const;
  uint32_t read(uint64_t from, uint32_t n, float* out) const;
  float level(int band) const;
  // Every active band's level is at the floor (nothing more to show).
  bool atRest() const;

private:
  void rebuild() noexcept;  // mutex held
  void pushColumn() noexcept;
  void addFrame(float s) noexcept;

  mutable std::mutex mutex_;
  BandLevelsConfig cfg_;
  BandSplit split_;
  double acc_[kBands] = {};
  uint32_t accN_ = 0, colSamples_ = 800;
  float level_[kBands] = {kFloorDb, kFloorDb, kFloorDb, kFloorDb, kFloorDb};
  std::vector<float> hist_;  // kHistory × kBands
  uint64_t written_ = 0;
};

}  // namespace pq::dsp
