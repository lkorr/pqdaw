// The oscilloscope's analysis (pq/dsp/scope.h; TX.scope). The algorithms and their numbers are PQ Meter's
// (pq meter/src/modules/oscilloscope.cpp: detectPitch, settleSpan, sampleShape, anchorShapePolarity, alignToShape, the
// draw path's period smoothing and edge trigger), ported onto pq::dsp::RealFft; the note-locked period is pqdaw's.

#include "pq/dsp/scope.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace pq::dsp {

namespace {

constexpr int kPitchHoldFrames = 30;     // a lost pitch is held this many frames
constexpr double kDetectPeriods = 6.0;   // the detector's window once a period is known …
constexpr size_t kDetectMinLen = 2048;   // … but never shorter than this
constexpr double kPitchJumpLo = 0.82, kPitchJumpHi = 1.22;  // ±18 %: still the same note
constexpr int kPitchJumpFrames = 6;      // frames outside that band before it is a new note
constexpr double kSpanSettleRatio = 0.92;  // Stable: the same span within 8 % …
constexpr int kSpanSettleFrames = 12;      // … a new one after this many frames elsewhere
constexpr int kCoarseSteps = 64;

bool nearRatio(double a, double b) noexcept {
  if (a <= 0.0 || b <= 0.0) return false;
  const double r = a / b;
  return r > kSpanSettleRatio && r < 1.0 / kSpanSettleRatio;
}

}  // namespace

void ScopeTracker::setSettings(const ScopeSettings& s) noexcept {
  ScopeSettings c = s;
  c.windowMs = std::clamp(c.windowMs, 1.0f, 2000.0f);
  c.cycles = std::clamp(c.cycles, 1, 16);
  c.minHz = std::clamp(c.minHz, 20.0f, 500.0f);
  c.maxHz = std::clamp(c.maxHz, 200.0f, 8000.0f);
  c.clarity = std::clamp(c.clarity, 0.1f, 0.95f);
  c.level = std::clamp(c.level, -1.0f, 1.0f);
  c.hysteresis = std::clamp(c.hysteresis, 1e-5f, 0.5f);
  s_ = c;
}

void ScopeTracker::reset() noexcept {
  periodSmoothed_ = 0.0;
  clarityLast_ = 0.0f;
  holdFrames_ = pitchJumpFrames_ = 0;
  noteLast_ = false;
  stableSpan_ = 0;
  stableSpanPeriod_ = pendingSpanPeriod_ = 0.0;
  spanSettleFrames_ = 0;
  stableSpanCycles_ = -1;
  shapeValid_ = false;
  lastStart_ = lastSpan_ = 0;
}

size_t ScopeTracker::windowLen(double sr) const noexcept {
  const auto n = static_cast<size_t>(static_cast<double>(s_.windowMs) * 0.001 * sr);
  return std::clamp<size_t>(n, 16, kMaxWindow);
}

size_t ScopeTracker::analysisLen(double sr) const noexcept {
  const auto n = static_cast<size_t>(std::ceil(2.5 * sr / std::max(1.0f, s_.minHz)));
  return std::clamp<size_t>(n, 1024, kMaxWindow / 2);
}

size_t ScopeTracker::need(double sr) const noexcept {
  size_t win = windowLen(sr);
  const size_t search = s_.trigger != ScopeSettings::Trigger::Off ? win : 0;
  if (s_.timebase != ScopeSettings::Timebase::Pitch) return win * 2 + search;
  const size_t a = analysisLen(sr);
  if (s_.lock != ScopeSettings::Lock::Constant) win = std::min(a, win);
  // A span of `cycles` periods of the last period (a note may be longer than the detector's window), and one period
  // more for the shape search.
  size_t span = win;
  if (periodSmoothed_ > 1.0 && s_.lock != ScopeSettings::Lock::Constant)
    span = std::max(span, std::min<size_t>(kMaxWindow, static_cast<size_t>(periodSmoothed_ * s_.cycles + periodSmoothed_)));
  return a + std::max(search, span) + span;
}

RealFft& ScopeTracker::fftOf(uint32_t n) {
  if (!fft_ || fft_->size() != n) fft_ = std::make_unique<RealFft>(n);
  return *fft_;
}

// McLeod's normalised square difference: autocorrelation normalised by the energy of the two overlapping parts, so
// short lags are not favoured, and the first peak near the tallest taken (the fundamental of a bright tone).
ScopeTracker::Pitch ScopeTracker::detectPitch(std::span<const float> src, size_t n, double sr) {
  Pitch out;
  if (n < 64 || src.size() < n) return out;
  const float* x = src.data() + (src.size() - n);
  const int minLag = std::max(2, static_cast<int>(std::floor(sr / std::max(1.0f, s_.maxHz))) - 1);
  const int maxLag = std::min(static_cast<int>(n / 2), static_cast<int>(std::ceil(sr / std::max(1.0f, s_.minHz))));
  if (maxLag <= minLag + 2) return out;
  double energy = 0.0;
  for (size_t i = 0; i < n; ++i) energy += static_cast<double>(x[i]) * x[i];
  if (energy / static_cast<double>(n) < 1e-6) return out;  // below −60 dBFS RMS: no pitch (and no FFTs)

  // ac(lag) = ifft(|X|²)(lag), linear when the padding covers n + maxLag. |X|² is real and even, so its inverse is its
  // forward transform's real part over L: two forward transforms of L.
  uint32_t L = 8;
  while (L < n + static_cast<size_t>(maxLag) + 1) L <<= 1;
  L = std::min<uint32_t>(L, 65536);
  if (n + static_cast<size_t>(maxLag) + 1 > L) return out;
  RealFft& fft = fftOf(L);
  pad_.assign(L, 0.0f);
  std::copy(x, x + n, pad_.begin());
  re_.resize(L / 2 + 1);
  im_.resize(L / 2 + 1);
  fft.forward(pad_.data(), re_.data(), im_.data());
  for (uint32_t k = 0; k <= L / 2; ++k) pad_[k] = re_[k] * re_[k] + im_[k] * im_[k];
  for (uint32_t k = 1; k < L / 2; ++k) pad_[L - k] = pad_[k];
  fft.forward(pad_.data(), re_.data(), im_.data());  // re_[lag] = L · ac(lag)

  cum_.resize(n + 1);
  cum_[0] = 0.0;
  for (size_t i = 0; i < n; ++i) cum_[i + 1] = cum_[i] + static_cast<double>(x[i]) * x[i];
  nsdf_.assign(static_cast<size_t>(maxLag) + 1, 0.0f);
  const double scale = 1.0 / static_cast<double>(L);
  for (int lag = minLag; lag <= maxLag; ++lag) {
    const double ac = static_cast<double>(re_[static_cast<size_t>(lag)]) * scale;
    const double sq = cum_[n - static_cast<size_t>(lag)] + (cum_[n] - cum_[static_cast<size_t>(lag)]);
    nsdf_[static_cast<size_t>(lag)] = sq > 1e-12 ? static_cast<float>(2.0 * ac / sq) : 0.0f;
  }
  const auto at = [&](int lag) { return nsdf_[static_cast<size_t>(lag)]; };

  float best = 0.0f;
  for (int lag = minLag; lag <= maxLag; ++lag) best = std::max(best, at(lag));
  if (best < s_.clarity) return out;
  const float thresh = 0.9f * best;
  int pick = -1;
  for (int lag = minLag + 1; lag < maxLag; ++lag)
    if (at(lag) > at(lag - 1) && at(lag) >= at(lag + 1) && at(lag) >= thresh) {
      pick = lag;
      break;
    }
  if (pick < 0) return out;
  // The rule above only ever moves to a shorter lag; a pure tone's sub-octave peak can still be the first one seen
  // (its true lag below minLag's neighbourhood): walk down the octaves while the halved lag is a strong peak.
  for (int half = pick / 2; half >= minLag; half = pick / 2) {
    int cand = -1;
    const int lo = std::max(minLag, half - 2), hi = std::min(maxLag - 1, half + 2);
    for (int lag = lo; lag <= hi; ++lag)
      if (cand < 0 || at(lag) > at(cand)) cand = lag;
    if (cand <= 0 || at(cand) < thresh) break;
    pick = cand;
  }
  if (pick <= minLag || pick >= maxLag) return out;
  const float y0 = at(pick - 1), y1 = at(pick), y2 = at(pick + 1);
  const float den = y0 - 2.0f * y1 + y2;
  const double frac = std::fabs(den) > 1e-9f ? 0.5 * (y0 - y2) / den : 0.0;
  out.period = static_cast<double>(pick) + std::clamp(frac, -1.0, 1.0);
  out.clarity = std::clamp(y1, 0.0f, 1.0f);
  return out;
}

void ScopeTracker::settleSpan(double period, size_t cap) noexcept {
  if (stableSpanCycles_ != s_.cycles) {  // a deliberate setting takes effect at once
    stableSpanCycles_ = s_.cycles;
    stableSpan_ = 0;
  }
  const auto spanFor = [&](double p) {
    return std::clamp<size_t>(static_cast<size_t>(std::llround(p * static_cast<double>(std::max(1, s_.cycles)))), 16,
                              std::max<size_t>(cap, 16));
  };
  if (stableSpan_ == 0) {
    stableSpanPeriod_ = period;
    stableSpan_ = spanFor(period);
    spanSettleFrames_ = 0;
  } else if (nearRatio(period, stableSpanPeriod_)) {
    spanSettleFrames_ = 0;
  } else if (nearRatio(period, pendingSpanPeriod_)) {
    if (++spanSettleFrames_ >= kSpanSettleFrames) {
      stableSpanPeriod_ = pendingSpanPeriod_;
      stableSpan_ = spanFor(pendingSpanPeriod_);
      spanSettleFrames_ = 0;
    }
  } else {
    pendingSpanPeriod_ = period;
    spanSettleFrames_ = 1;
  }
}

void ScopeTracker::sampleShape(std::span<const float> src, size_t start, double period, std::vector<float>& out) {
  out.resize(kShapePoints);
  const double step = period / static_cast<double>(kShapePoints);
  double sumSq = 0.0;
  for (size_t i = 0; i < kShapePoints; ++i) {
    const double pos = static_cast<double>(start) + step * static_cast<double>(i);
    const auto i0 = static_cast<size_t>(pos);
    const double f = pos - static_cast<double>(i0);
    const float a = i0 < src.size() ? src[i0] : 0.0f;
    const float b = i0 + 1 < src.size() ? src[i0 + 1] : a;
    const auto v = static_cast<float>(a + (b - a) * f);
    out[i] = v;
    sumSq += static_cast<double>(v) * v;
  }
  const double rms = std::sqrt(sumSq / static_cast<double>(kShapePoints));
  if (rms > 1e-9) {
    const auto inv = static_cast<float>(1.0 / rms);
    for (float& v : out) v *= inv;
  }
}

void ScopeTracker::anchorShapePolarity(std::vector<float>& shape) {
  const size_t n = shape.size();
  if (n < 4) return;
  size_t best = n;
  float bestSlope = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    const float a = shape[i], b = shape[(i + 1) % n];
    if (a <= 0.0f && b > 0.0f && (best == n || b - a > bestSlope)) {
      best = i;
      bestSlope = b - a;
    }
  }
  if (best == n || best == 0) return;
  std::rotate(shape.begin(), shape.begin() + static_cast<std::ptrdiff_t>(best), shape.end());
}

size_t ScopeTracker::alignToShape(std::span<const float> src, size_t base, double period) {
  if (!shapeValid_ || shapeRef_.size() != kShapePoints || period < 4.0) return base;
  const auto per = static_cast<size_t>(std::llround(period));
  if (base < per || per < 4) return base;
  const auto score = [&](size_t start) {
    sampleShape(src, start, period, cand_);
    double dot = 0.0;
    for (size_t i = 0; i < kShapePoints; ++i) dot += static_cast<double>(cand_[i]) * shapeRef_[i];
    return dot;
  };
  const size_t lo = base - per;
  size_t bestStart = base;
  double bestScore = -1e30;
  for (int s = 0; s < kCoarseSteps; ++s) {
    const size_t start = lo + static_cast<size_t>(static_cast<double>(per) * s / kCoarseSteps);
    const double sc = score(start);
    if (sc > bestScore) {
      bestScore = sc;
      bestStart = start;
    }
  }
  const size_t half = std::max<size_t>(1, per / kCoarseSteps);
  const size_t fLo = bestStart > lo + half ? bestStart - half : lo;
  const size_t fHi = std::min(bestStart + half, base);
  for (size_t start = fLo; start <= fHi; ++start) {
    const double sc = score(start);
    if (sc > bestScore) {
      bestScore = sc;
      bestStart = start;
    }
  }
  return bestStart;
}

size_t ScopeTracker::triggerStart(std::span<const float> x, size_t winLen) const noexcept {
  const size_t def = x.size() >= winLen ? x.size() - winLen : 0;
  if (s_.trigger == ScopeSettings::Trigger::Off || x.size() < winLen * 2) return def;
  const size_t sEnd = x.size() - winLen;
  const size_t sBeg = sEnd > winLen ? sEnd - winLen : 1;
  const float lvl = s_.level, hys = std::max(1e-5f, s_.hysteresis);
  const bool rising = s_.trigger == ScopeSettings::Trigger::Rising;
  // A Schmitt trigger forward over the candidates; the newest edge wins (the trace stays current). Armed: the signal
  // left the level by the hysteresis on the far side. (PQ Meter's backward walk fires on the opposite edge to the one
  // it names — "rising" started on a falling crossing; this is the edge as named.)
  size_t edge = def;
  bool armed = false;
  for (size_t i = sBeg; i <= sEnd; ++i) {
    const float v = x[i];
    if (rising) {
      if (v < lvl - hys) armed = true;
      else if (armed && v >= lvl) {
        edge = i;
        armed = false;
      }
    } else {
      if (v > lvl + hys) armed = true;
      else if (armed && v <= lvl) {
        edge = i;
        armed = false;
      }
    }
  }
  return edge;
}

ScopeFrame ScopeTracker::frame(std::span<const float> x, double sr, float noteHz, bool frozen) {
  ScopeFrame f;
  const bool pitchMode = s_.timebase == ScopeSettings::Timebase::Pitch;
  size_t winLen = windowLen(sr);
  const size_t aLen = analysisLen(sr);
  if (pitchMode && s_.lock != ScopeSettings::Lock::Constant) winLen = std::min(aLen, winLen);
  if (x.size() < 18) return f;
  winLen = std::min(winLen, x.size() - 2);
  const bool pinned = s_.lock != ScopeSettings::Lock::Tracking;
  bool note = false;
  if (pitchMode && !frozen) {
    note = s_.source == ScopeSettings::PitchSource::Auto && noteHz > 0.0f && std::isfinite(noteHz) && sr > 0.0;
    if (note) {
      // The note's own period: exact, nothing to estimate or smooth (a glide is followed as it moves).
      const double period = sr / static_cast<double>(noteHz);
      const double ratio = periodSmoothed_ > 0.0 ? period / periodSmoothed_ : 0.0;
      if (!noteLast_ || ratio < kPitchJumpLo || ratio > kPitchJumpHi) {
        if (!pinned) shapeValid_ = false;  // a new note: its shape has nothing to do with the last one's
      }
      periodSmoothed_ = period;
      clarityLast_ = 1.0f;
      holdFrames_ = kPitchHoldFrames;
      pitchJumpFrames_ = 0;
    } else {
      size_t detectLen = aLen;
      if (periodSmoothed_ > 1.0)
        detectLen = std::clamp<size_t>(static_cast<size_t>(std::ceil(periodSmoothed_ * kDetectPeriods)), kDetectMinLen, aLen);
      Pitch pit = detectPitch(x, std::min(detectLen, x.size()), sr);
      if (pit.period <= 0.0 && detectLen > kDetectMinLen) {  // cold start on a strongly modulated note
        const size_t retry = std::max(kDetectMinLen, detectLen / 3);
        if (retry < detectLen) pit = detectPitch(x, std::min(retry, x.size()), sr);
      }
      if (pit.period > 0.0) {
        const double ratio = periodSmoothed_ > 0.0 ? pit.period / periodSmoothed_ : 0.0;
        const bool outside = ratio < kPitchJumpLo || ratio > kPitchJumpHi;
        if (outside) ++pitchJumpFrames_;
        else pitchJumpFrames_ = 0;
        if (periodSmoothed_ <= 0.0 || (outside && pitchJumpFrames_ >= kPitchJumpFrames)) {
          pitchJumpFrames_ = 0;
          periodSmoothed_ = pit.period;
          if (!pinned) shapeValid_ = false;
        } else {
          periodSmoothed_ += (pit.period - periodSmoothed_) * (outside ? 0.06 : 0.25);
        }
        clarityLast_ = pit.clarity;
        holdFrames_ = kPitchHoldFrames;
      } else if (holdFrames_ > 0 && --holdFrames_ == 0) {
        if (!pinned) {  // a pinned span never gives the period up (that would rescale: the flash it exists to prevent)
          periodSmoothed_ = 0.0;
          shapeValid_ = false;
        }
        clarityLast_ = 0.0f;
      }
    }
    noteLast_ = note;
  }
  if (frozen && lastSpan_ > 0 && lastStart_ + lastSpan_ <= x.size()) {
    f.start = lastStart_;
    f.span = lastSpan_;
  } else {
    if (pitchMode && periodSmoothed_ > 1.0 && s_.lock != ScopeSettings::Lock::Constant) {
      const size_t cap = std::max<size_t>(16, std::min(noteLast_ ? kMaxWindow : aLen, x.size() / 2));
      const auto want = static_cast<size_t>(std::llround(periodSmoothed_ * static_cast<double>(s_.cycles)));
      if (s_.lock == ScopeSettings::Lock::Tracking) {
        winLen = std::clamp<size_t>(want, 16, cap);
      } else {
        if (!frozen) settleSpan(periodSmoothed_, cap);
        if (stableSpan_ > 0) winLen = std::clamp<size_t>(stableSpan_, 16, cap);
      }
    }
    size_t start = x.size() - winLen;
    const bool shapeLock = pitchMode && periodSmoothed_ > 3.0 && static_cast<double>(start) >= periodSmoothed_ && !frozen;
    if (shapeLock) {
      start = alignToShape(x, start, periodSmoothed_);
      sampleShape(x, start, periodSmoothed_, shape_);
      if (!shapeValid_ || shapeRef_.size() != kShapePoints) {
        shapeRef_ = shape_;
        anchorShapePolarity(shapeRef_);  // the first frame of a note chooses the phase: anchor its polarity
        shapeValid_ = true;
      } else {
        for (size_t i = 0; i < kShapePoints; ++i) shapeRef_[i] += (shape_[i] - shapeRef_[i]) * 0.3f;
      }
    } else if (!pitchMode || periodSmoothed_ <= 3.0) {
      start = triggerStart(x, winLen);
    }
    f.start = start;
    f.span = winLen;
    if (!frozen) {
      lastStart_ = start;
      lastSpan_ = winLen;
    }
  }
  if (pitchMode && periodSmoothed_ > 1.0) {
    f.period = periodSmoothed_;
    f.pitchHz = static_cast<float>(sr / periodSmoothed_);
    f.clarity = clarityLast_;
    f.note = noteLast_;
  }
  return f;
}

// ---- ScopeBeat ---------------------------------------------------------------------------------------------------------

ScopeBeat::ScopeBeat() : slots_(kSlots) { clear(); }

void ScopeBeat::clear() noexcept {
  for (Slot& s : slots_) {
    for (uint32_t c = 0; c < kChannels; ++c) {
      s.mn[c] = 1e9f;
      s.mx[c] = -1e9f;
    }
    s.n = 0;
  }
  cur_ = -1;
  phase_ = -1.0;
}

void ScopeBeat::setSpan(double beats) noexcept {
  beats = std::max(beats, 1.0 / 16.0);
  if (std::fabs(beats - span_) < 1e-9) return;
  span_ = beats;
  clear();
}

void ScopeBeat::ingest(const float* beat, const float* l, const float* r, size_t n) noexcept {
  size_t first = n, last = n;
  for (size_t i = 0; i < n; ++i) {
    const float b = beat[i];
    if (!std::isfinite(b)) continue;
    if (first == n) first = i;
    last = i;
    double ph = std::fmod(static_cast<double>(b), span_);
    if (ph < 0.0) ph += span_;
    phase_ = ph / span_;
    int64_t slot = static_cast<int64_t>(phase_ * kSlots);
    slot = std::clamp<int64_t>(slot, 0, kSlots - 1);
    Slot& s = slots_[static_cast<size_t>(slot)];
    if (slot != cur_) {  // the playhead entered a new slot: this pass owns it now
      for (uint32_t c = 0; c < kChannels; ++c) {
        s.mn[c] = 1e9f;
        s.mx[c] = -1e9f;
      }
      s.n = 0;
      cur_ = slot;
    }
    const float v[kChannels] = {l[i], r[i], 0.5f * (l[i] + r[i]), 0.5f * (l[i] - r[i])};
    for (uint32_t c = 0; c < kChannels; ++c) {
      s.mn[c] = std::min(s.mn[c], v[c]);
      s.mx[c] = std::max(s.mx[c], v[c]);
    }
    ++s.n;
  }
  if (first == n) {
    moving_ = false;
    return;
  }
  // The tempo the clock moved at, over the chunk's last 4096 frames (a float beat position steps by its ulp: a long
  // stretch averages it), or since the previous chunk when this one is short.
  const bool within = last - first >= 64;
  const size_t from = within ? std::max(first, last > 4096 ? last - 4096 : size_t{0}) : 0;
  const float b0 = within ? beat[from] : (haveLast_ ? lastBeat_ : beat[first]);
  const size_t frames = within ? last - from : (haveLast_ ? last + 1 : last - first);
  const float d = beat[last] - b0;
  moving_ = beat[last] != (haveLast_ ? lastBeat_ : beat[first]) || d != 0.0f;
  if (frames >= 64 && d > 0.0f && d < 4.0f) {
    const double bps = static_cast<double>(d) / static_cast<double>(frames);
    beatPerSample_ = beatPerSample_ > 0.0 ? beatPerSample_ + (bps - beatPerSample_) * 0.25 : bps;
  }
  lastBeat_ = beat[last];
  haveLast_ = true;
}

void ScopeBeat::columns(uint32_t channel, uint32_t cols, std::vector<float>& mn, std::vector<float>& mx,
                        std::vector<uint8_t>& ok) const {
  channel = std::min(channel, kChannels - 1);
  mn.assign(cols, 0.0f);
  mx.assign(cols, 0.0f);
  ok.assign(cols, 0);
  for (uint32_t c = 0; c < cols; ++c) {
    const auto s0 = static_cast<size_t>(static_cast<uint64_t>(c) * kSlots / cols);
    auto s1 = static_cast<size_t>(static_cast<uint64_t>(c + 1) * kSlots / cols);
    if (s1 <= s0) s1 = s0 + 1;
    bool any = false;
    for (size_t s = s0; s < s1 && s < slots_.size(); ++s) {
      const Slot& sl = slots_[s];
      if (sl.n == 0) continue;
      mn[c] = any ? std::min(mn[c], sl.mn[channel]) : sl.mn[channel];
      mx[c] = any ? std::max(mx[c], sl.mx[channel]) : sl.mx[channel];
      any = true;
    }
    ok[c] = any ? 1 : 0;
  }
}

}  // namespace pq::dsp
