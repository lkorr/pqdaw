#pragma once

// The Spectrogram's analysis (TX.spectrogram; SPEC §15 2026-11-01 "analysis modules", spectrogram added by the user):
// PQ Meter's reassigned spectrogram (pq meter/src/modules/spectrogram.cpp: analyzeFrame, analyzeChain, splat), ported
// with its numbers, producing finished COLUMNS instead of loose points so the analysis thread does all of it and the UI
// only colours and uploads what is new.
//
//   frames     every `hop` samples, centred on the same instant: the main chain (Kaiser β 6.2, `fftSize` scaled with the
//              rate to keep the 48 kHz window's duration, widened while the hop needs more than half of it) owns
//              ≥ 200 Hz; the low chain (the smallest power of two covering 85 ms, when that is longer than the main
//              window) owns < 200 Hz (PQ Meter's measured fix for sub bass reading as a cloud)
//   per chain  three FFTs (h, t·h, h'), each bin's reassigned frequency and time (Auger–Flandrin), the sidelobe
//              leakage mask (octave pyramid: max(58 dB, 36 dB + 20·log10 d) below any other bin), the deviation limits
//              (`maxBinDev` bins, half a window), the consensus test (a neighbour reassigns to the same frequency OR the
//              same time, `consensus` bins / hops), the display floor
//   splat      each point into a separable tent (1.1 columns × 0.6 rows), peak-normalised, combined by MAX (a cell is
//              the loudest thing that landed in it), on kRows rows of the display axis (log with a linear 0 … 40 Hz
//              foot when fMin is 0, mel or linear; row 0 = fMax at the top)
//   columns    column c = floor(t / hop) + offset; a column is final once the newest frame's column is more than
//              `ahead` = span / (2 hop) + 1 past it (no later frame can reach it), and is handed to the sink in order.
//              Columns are a sequence that continues across reconfigurations (the open ones are flushed), so a display
//              keeps its picture when the hop or window changes, as PQ Meter's column rebase does.
//
// Values are linear power of the equivalent sine amplitude (a full-scale sine reads 1 = 0 dB). Not for the audio
// thread (allocates in the constructor and configure()); push() allocates nothing.

#include <cstdint>
#include <memory>
#include <vector>

#include "pq/dsp/fft.h"

namespace pq::dsp {

enum class GramScale : uint8_t { Log, Mel, Linear };

// The display axis: the normalised height of `hz` in [fMin, fMax] (0 bottom … 1 top; PQ Meter's freqToUnit), and its
// inverse. Log with fMin < 0.5 Hz is the hybrid axis: linear 0 … 40 Hz, log above, slope-continuous at the corner.
float gramUnit(GramScale scale, float hz, float fMin, float fMax) noexcept;
float gramHz(GramScale scale, float unit, float fMin, float fMax) noexcept;

struct SpectrogramConfig {
  double sampleRate = 48000.0;
  uint32_t fftSize = 2048;  // the user's window, in samples at 48 kHz (256 … 16384)
  uint32_t hop = 256;       // samples per column (clamped: ≥ 32, ≥ span / 120 so `ahead` fits, ≤ 8192)
  bool reassign = true;
  float maxBinDev = 3.5f;   // bins
  float consensus = 0.5f;   // bins (frequency) / hops (time); 0: off
  float floorDb = -100.0f;  // points a quarter below it are dropped
  GramScale scale = GramScale::Log;
  float fMin = 0.0f, fMax = 24000.0f;  // fMax is clamped to the Nyquist frequency
  bool operator==(const SpectrogramConfig&) const = default;
};

class Spectrogram {
public:
  static constexpr uint32_t kRows = 1024;
  static constexpr uint32_t kMaxFft = 16384;
  static constexpr uint32_t kOpen = 128;  // open columns (≥ 2·ahead + 2)

  class Sink {
  public:
    virtual ~Sink() = default;
    // Column `index` of the sequence is final: kRows values, row 0 = fMax. `centre`: the stream sample at its middle.
    virtual void column(const float* rows, uint64_t index, double centre) = 0;
  };

  explicit Spectrogram(const SpectrogramConfig& config);
  ~Spectrogram();
  Spectrogram(const Spectrogram&) = delete;
  Spectrogram& operator=(const Spectrogram&) = delete;

  // A new configuration: the open columns up to the newest frame's are emitted to `sink`, the rest dropped; the next
  // frame continues the column sequence. A change of the row mapping (scale, fMin, fMax) bumps mapping().
  void configure(const SpectrogramConfig& config, Sink& sink);
  const SpectrogramConfig& config() const noexcept { return cfg_; }
  // What the analysis runs now: the main window (scaled, widened), the low chain's (0: off), the hop, the reach.
  uint32_t fftSize() const noexcept { return fft_; }
  uint32_t lowFftSize() const noexcept { return lowFft_; }
  uint32_t hop() const noexcept { return hop_; }
  uint32_t ahead() const noexcept { return ahead_; }
  uint32_t mapping() const noexcept { return mapping_; }
  float fMin() const noexcept { return fMinEff_; }
  float fMax() const noexcept { return fMaxEff_; }

  // Mono samples; every finished column goes to `sink`.
  void push(const float* x, uint32_t n, Sink& sink) noexcept;
  uint64_t samples() const noexcept { return written_; }  // pushed so far (the stream index)
  uint64_t columns() const noexcept { return emitted_; }  // emitted so far
  // The scroll position in columns: where the next column's boundary will be as far as the samples pushed reach
  // (fractional, in [columns(), columns() + 1]): what a display's smooth scroll follows.
  double position() const noexcept;

  // Tests / benches: frames analysed, points splatted.
  uint64_t frames() const noexcept { return frames_; }
  uint64_t points() const noexcept { return points_; }

private:
  struct Chain {
    uint32_t n = 0;
    std::unique_ptr<RealFft> fft;
    std::vector<float> h, th, dh;
    float wSum = 1.0f;
    std::vector<float> hr, hi, tr, ti, dr, di;  // the three spectra
  };
  void build();
  void buildChain(Chain& c, uint32_t n);
  void analyseFrame(const float* frame, double centre, Sink& sink) noexcept;
  void analyseChain(Chain& c, const float* x, double centre, float fLo, float fHi) noexcept;
  void splat(double t, float hz, float power) noexcept;
  void emitThrough(int64_t last, Sink& sink) noexcept;  // emits open columns up to `last` (inclusive)
  uint32_t span() const noexcept { return fft_ > lowFft_ ? fft_ : lowFft_; }

  SpectrogramConfig cfg_;
  uint32_t fft_ = 2048, lowFft_ = 0, hop_ = 256, ahead_ = 5, mapping_ = 0;
  float fMinEff_ = 0.0f, fMaxEff_ = 24000.0f;  // the range the rows map (fMax ≤ Nyquist, fMin ≤ fMax / 2)
  Chain main_, low_;
  // The input FIFO (stream-indexed: position i lives at i & mask).
  std::vector<float> fifo_;
  uint64_t fifoMask_ = 0, written_ = 0, read_ = 0;
  std::vector<float> frame_, in_;
  std::vector<float> binP_, binDw_, binDt_, maskP_, maskLvl_;
  // Open columns: index c lives at (c % kOpen) · kRows.
  std::vector<float> open_;
  int64_t offset_ = 0;          // column of time t: floor(t / hop) + offset_
  bool anchored_ = false;       // offset_ set for the current geometry (the first frame after a build sets it)
  int64_t head_ = -1;           // the newest frame's column
  int64_t cleared_ = -1;        // open columns ≤ this are zeroed for use
  uint64_t emitted_ = 0;
  uint64_t frames_ = 0, points_ = 0;
  double lastCentre_ = -1.0;
};

}  // namespace pq::dsp
