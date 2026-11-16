#pragma once

// The Stereo Spectrogram's analysis (VZ.7; docs/plans/VZ.md "VZ.7"), ported from PQ Meter (pq meter/src/modules/
// stereo_spectrogram.cpp: processAudio, analyzeChannel, capBatchByBand, ribbonFrame, ribbonLoFrame), its numbers kept:
//
//   the cloud   every hop (N / 8) the newest N samples of L and R are windowed (Kaiser β 6.2) and, with reassignment on,
//               also by the time-ramped and the derivative window: per channel, every bin over the analysis floor and
//               over the frame's own sidelobe-leakage envelope is reassigned in frequency (−Im(X_dh X_h*) / |X_h|²) and
//               time (Re(X_th X_h*) / |X_h|²), rejected when it moves more than 3.5 bins / half a window, or when
//               neither neighbour agrees with it in frequency or in time (the consensus test: leakage skirts and mixed
//               bins agree with nothing); each survivor is a point (time, Hz, pan (R − L) / (R + L) of the plain spectra
//               at its bin, the STEREO power). A hop's points are capped at kMaxPointsPerHop, spent per band of the
//               display's axis (8 bands, 8 each, the leftover handed to the bands that want more).
//   the ribbon  one slice per hop: the Stereo Image's per-band pan / width / level (pq/dsp/stereo.h's maths) from the
//               same plain spectra over kRibBands points, the bands under ~700 Hz from a `loFactor` × longer window
//               (blended over an octave around 500 Hz).
// Not for the audio thread (the analysis worker); push() never allocates.

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "pq/dsp/freq_axis.h"

namespace pq::dsp {

class RealFft;

struct StereoCloudConfig {
  double sampleRate = 48000.0;
  uint32_t fftSize = 2048;  // 512 … 8192; the hop is fftSize / 8
  bool reassign = true;
  float consensus = 0.5f;   // bins (0: off)
  FreqScale scale = FreqScale::Log;
  float fMin = 10.0f, fMax = 24000.0f;
  uint32_t loFactor = 2;    // the ribbon's low-band window: 1, 2, 4, 8 × fftSize (≤ 16384)
  float ribAttackMs = 60.0f, ribReleaseMs = 400.0f, ribCoherenceMs = 400.0f;
  float floorDb = -100.0f;  // the ribbon holds its position below this
  float restSec = 6.5f;     // the display's span (+ a margin): silent this long, nothing drawn moves any more
  bool operator==(const StereoCloudConfig&) const = default;
};

struct CloudPoint {
  double tSamples = 0.0;  // absolute stream time (reassigned)
  float freqHz = 0.0f, pan = 0.0f, power = 0.0f;
};

class StereoCloud {
public:
  static constexpr uint32_t kRibBands = 160, kPointBands = 8, kMaxPointsPerBand = 8;
  static constexpr uint32_t kMaxPointsPerHop = kPointBands * kMaxPointsPerBand;
  struct RibSlice {
    double tSamples = 0.0;
    std::array<float, kRibBands> pos{}, width{}, db{};
  };

  explicit StereoCloud(const StereoCloudConfig& c = {});
  ~StereoCloud();
  // A new size, low-band factor, axis or rate restarts (generation() bumps); the stream index keeps counting.
  void configure(const StereoCloudConfig& c);
  const StereoCloudConfig& config() const noexcept { return cfg_; }
  uint32_t hop() const noexcept { return cfg_.fftSize / 8; }
  uint32_t generation() const noexcept { return gen_; }

  // Feeds L and R; per completed hop calls points(batch) (≤ kMaxPointsPerHop) and slice(ribbon slice). RT-safe.
  template <class OnPoints, class OnSlice>
  void push(const float* l, const float* r, uint32_t n, OnPoints&& points, OnSlice&& slice) noexcept {
    for (uint32_t i = 0; i < n;) {
      const uint32_t k = std::min(n - i, hop() - since_);
      write(l + i, r + i, k);
      i += k;
      since_ += k;
      if (since_ >= hop()) {
        since_ = 0;
        frame();
        points(batch_.data(), static_cast<uint32_t>(batch_.size()));
        slice(rib_);
      }
    }
  }
  double head() const noexcept { return head_; }  // the newest frame's centre (stream samples)
  bool silent(double seconds) const noexcept { return static_cast<double>(quiet_) >= seconds * cfg_.sampleRate; }
  bool atRest() const noexcept { return silent(cfg_.restSec); }

private:
  void write(const float* l, const float* r, uint32_t n) noexcept;
  void frame() noexcept;
  void analyzeChannel(int ch) noexcept;
  void capBatch() noexcept;
  void ribbon() noexcept;

  StereoCloudConfig cfg_;
  FreqAxis axis_;
  std::unique_ptr<RealFft> fft_, loFft_;
  std::vector<float> winH_, winTh_, winDh_, loWin_;
  float winSum_ = 1.0f, enbw_ = 1.0f, loWinSum_ = 1.0f, loEnbw_ = 1.0f;
  std::vector<float> hist_[2];  // a power-of-two ring of the newest samples (≥ the long window)
  uint32_t mask_ = 0, at_ = 0, since_ = 0, gen_ = 0;
  uint64_t written_ = 0, quiet_ = 0;
  double head_ = -1.0;
  std::vector<float> in_;
  std::vector<float> hRe_[2], hIm_[2], tRe_[2], tIm_[2], dRe_[2], dIm_[2], loRe_[2], loIm_[2];
  std::vector<float> binP_, binDw_, binDt_, maskP_, maskLvl_;
  std::vector<CloudPoint> batch_, scratch_;
  std::vector<uint8_t> band_;
  struct Accum {
    double sL = 0.0, sR = 0.0, xRe = 0.0, xIm = 0.0;
  };
  std::array<Accum, kRibBands> acc_{}, accLo_{};
  std::array<float, kRibBands> ribHz_{}, ribPrevHz_{}, ribNextHz_{};
  RibSlice rib_;
};

}  // namespace pq::dsp
