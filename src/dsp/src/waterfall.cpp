// The waterfall's analysis (VZ.7): pq/dsp/waterfall.h. Ported from PQ Meter (modules/waterfall.cpp).

#include "pq/dsp/waterfall.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pq::dsp {

WaterfallAnalyzer::WaterfallAnalyzer(const WaterfallConfig& c) {
  cfg_.sampleRate = 0.0;
  configure(c);
}
WaterfallAnalyzer::~WaterfallAnalyzer() = default;

float WaterfallAnalyzer::maxSpanSec(double sampleRate, uint32_t fftSize) noexcept {
  return static_cast<float>(kMaxSlices * static_cast<double>(fftSize) / std::max(1.0, sampleRate));
}

void WaterfallAnalyzer::configure(const WaterfallConfig& c) {
  WaterfallConfig n = c;
  n.fftSize = std::clamp<uint32_t>(c.fftSize, 512, 16384);
  n.spanSec = std::clamp(c.spanSec, 2.0f, std::clamp(maxSpanSec(n.sampleRate, n.fftSize), 2.0f, 18.0f));
  const bool restart = n.sampleRate != cfg_.sampleRate || n.fftSize != cfg_.fftSize || n.multiRes != cfg_.multiRes ||
                       n.scale != cfg_.scale || n.channel != cfg_.channel || !an_;
  const double sr = std::max(1000.0, n.sampleRate);
  // The pitch fills the span with about kTargetSlices slices (the window its ceiling, 256 its floor); the analysis
  // runs at ~90 Hz under it.
  pitch_ = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(n.spanSec * sr / kTargetSlices)), 256, n.fftSize);
  aHop_ = std::clamp<uint32_t>(static_cast<uint32_t>(sr / 90.0), 256, pitch_);
  SpectrumConfig s;
  s.sampleRate = sr;
  s.fftSize = n.fftSize;
  s.hop = aHop_;
  s.sharpLows = n.multiRes;
  s.points = kPoints;
  s.scale = n.scale;
  s.fMin = kFMin;
  s.fMax = kFMax;
  s.tiltDbPerOct = n.slopeDbPerOct;
  s.tiltPivotHz = 1000.0f;
  s.attackMs = n.attackMs;
  s.releaseMs = n.releaseMs;
  const bool hopChanged = an_ && an_->config().hop != aHop_;
  const bool tiltChanged = an_ && an_->config().tiltDbPerOct != n.slopeDbPerOct;
  cfg_ = n;
  if (restart || hopChanged || tiltChanged) {
    an_ = std::make_unique<SpectrumAnalyzer>(s);  // (off the audio thread: the worker's)
  } else {
    SpectrumLive live;
    live.attackMs = n.attackMs;
    live.releaseMs = n.releaseMs;
    an_->setLive(live);
  }
  if (restart) {
    ring_.assign(static_cast<size_t>(kMaxSlices) * kPoints, -140.0f);
    mono_.resize(16384);
    slices_ = 0;
    since_ = 0;
    ++gen_;
  }
  since_ = std::min(since_, pitch_);
}

void WaterfallAnalyzer::push(const float* l, const float* r, uint32_t n) noexcept {  // RT-safe
  float m = 0.0f;
  for (uint32_t i = 0; i < n; ++i) m = std::max(m, std::max(std::fabs(l[i]), std::fabs(r[i])));
  quiet_ = m < 1e-7f ? quiet_ + n : 0;
  for (uint32_t i = 0; i < n;) {
    // One analysis hop at most per push (a commit is judged after each).
    const uint32_t k = std::min({n - i, aHop_, static_cast<uint32_t>(mono_.size())});
    for (uint32_t j = 0; j < k; ++j) {
      const float a = l[i + j], b = r[i + j];
      switch (cfg_.channel) {
        case WaterfallChannel::Left: mono_[j] = a; break;
        case WaterfallChannel::Right: mono_[j] = b; break;
        case WaterfallChannel::Side: mono_[j] = 0.5f * (a - b); break;
        default: mono_[j] = 0.5f * (a + b); break;
      }
    }
    an_->push(mono_.data(), k);
    i += k;
    since_ += k;
    if (since_ >= pitch_) {
      since_ -= pitch_;  // the remainder carries: an exact long-term pitch
      const auto lv = an_->levelsDb();
      std::memcpy(ring_.data() + static_cast<size_t>(slices_ % kMaxSlices) * kPoints, lv.data(), kPoints * sizeof(float));
      ++slices_;
    }
  }
}

const float* WaterfallAnalyzer::slice(uint64_t s) const noexcept {
  if (s >= slices_ || slices_ - s > kMaxSlices) return nullptr;
  return ring_.data() + static_cast<size_t>(s % kMaxSlices) * kPoints;
}

std::span<const float> WaterfallAnalyzer::live() const noexcept { return an_->levelsDb(); }

bool WaterfallAnalyzer::atRest() const noexcept {
  // Silent for the whole history and the curve at the floor: more silence changes nothing drawn (the oldest slices
  // scroll out as silence, which is still silence).
  return quiet_ > static_cast<uint64_t>(cfg_.spanSec * cfg_.sampleRate) + cfg_.fftSize && an_->allBelow(-139.0f);
}

}  // namespace pq::dsp
