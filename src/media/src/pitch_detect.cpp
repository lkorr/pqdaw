// Root pitch detection (SM.7): pq/media/pitch_detect.h.
#include "pq/media/pitch_detect.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "pq/dsp/fft.h"
#include "pq/media/decoder.h"
#include "pq/media/tempo_detect.h"

namespace pq::media {

namespace {

constexpr double kLoudRangeDb = 24.0;     // windows within this of the loudest are candidates
constexpr double kSteadyWeight = 1.5;     // score = mean dB − this × dB std
constexpr double kFrameGate = 1e-4;       // frames below −40 dB of the window's loudest frame are skipped
constexpr double kKeyMaximum = 0.9;       // McLeod's k: the first key maximum within this of the largest
constexpr double kKeepOctaves = 1.0 / 12; // frames within ±1 semitone of the median count
constexpr double kPercentile = 0.1;      // the centre is the midpoint of the 10th / 90th percentile
constexpr uint32_t kMinFrame = 256;
constexpr uint32_t kMaxFrame = 32768;     // the FFT is 2·W ≤ 65536

uint64_t cacheKey(uint64_t hash, int64_t frames) { return hash ^ (static_cast<uint64_t>(frames) * 0x9E3779B97F4A7C15ull); }

uint32_t pow2AtLeast(double v) {
  uint32_t n = kMinFrame;
  while (n < kMaxFrame && n < v) n <<= 1;
  return n;
}

PitchEstimate verdict(double hz, double confidence) {
  PitchEstimate e;
  e.confidence = static_cast<float>(std::clamp(confidence, 0.0, 1.0));
  e.pitched = hz > 0.0 && e.confidence >= kPitchConfidenceThreshold;
  e.hz = e.pitched ? static_cast<float>(hz) : kUnpitchedHz;
  return e;
}

int64_t scanFrames(int64_t frames, double sampleRate) {
  return std::min<int64_t>(frames, static_cast<int64_t>(kPitchScanSeconds * sampleRate));
}

}  // namespace

PitchWindow pickPitchWindow(std::span<const float> envelope, int64_t scannedFrames, double sampleRate,
                            const PitchOptions& options) {
  const int64_t want = std::max<int64_t>(1, static_cast<int64_t>(std::llround(options.windowSeconds * sampleRate)));
  if (scannedFrames <= want || envelope.empty()) return {0, scannedFrames};
  const std::size_t blocks = envelope.size();
  const std::size_t wb = std::clamp<std::size_t>(static_cast<std::size_t>(want / kEnvelopeBlock), 1, blocks);
  const std::size_t step = std::max<std::size_t>(1, wb / 4);
  // Prefix sums of dB and dB².
  std::vector<double> s1(blocks + 1, 0.0), s2(blocks + 1, 0.0);
  for (std::size_t i = 0; i < blocks; ++i) {
    const double db = 10.0 * std::log10(static_cast<double>(envelope[i]) + 1e-10);
    s1[i + 1] = s1[i] + db;
    s2[i + 1] = s2[i] + db * db;
  }
  struct Cand {
    std::size_t at;
    double mean, score;
  };
  std::vector<Cand> cands;
  double loudest = -1e300;
  for (std::size_t i = 0; i + wb <= blocks; i += step) {
    const double m = (s1[i + wb] - s1[i]) / static_cast<double>(wb);
    const double var = std::max(0.0, (s2[i + wb] - s2[i]) / static_cast<double>(wb) - m * m);
    cands.push_back({i, m, m - kSteadyWeight * std::sqrt(var)});
    loudest = std::max(loudest, m);
  }
  std::size_t best = 0;
  double bestScore = -1e300;
  for (const Cand& c : cands)
    if (c.mean >= loudest - kLoudRangeDb && c.score > bestScore) {
      bestScore = c.score;
      best = c.at;
    }
  const int64_t start = static_cast<int64_t>(best) * kEnvelopeBlock;
  return {start, std::min(want, scannedFrames - start)};
}

PitchEstimate analysePitch(std::span<const float> mono, double sampleRate, const PitchOptions& options) {
  const std::size_t size = mono.size();
  uint32_t w = pow2AtLeast(2.0 * sampleRate / options.minHz);
  while (w > kMinFrame && w > size) w >>= 1;
  if (size < w || sampleRate <= 0.0) return verdict(0.0, 0.0);
  const uint32_t n = 2 * w, hop = w / 4;
  const uint32_t maxLag = std::min<uint32_t>(w / 2, static_cast<uint32_t>(sampleRate / options.minHz) + 1);
  const uint32_t minLag = std::max<uint32_t>(2, static_cast<uint32_t>(sampleRate / options.maxHz));
  if (minLag + 2 >= maxLag) return verdict(0.0, 0.0);

  double mean = 0.0;
  for (float v : mono) mean += v;
  mean /= static_cast<double>(size);
  const float dc = static_cast<float>(mean);

  // Frame energies first (the gate is relative to the loudest frame).
  std::vector<std::size_t> starts;
  std::vector<double> energy;
  for (std::size_t s = 0; s + w <= size; s += hop) {
    double e = 0.0;
    for (uint32_t j = 0; j < w; ++j) {
      const double v = mono[s + j] - dc;
      e += v * v;
    }
    starts.push_back(s);
    energy.push_back(e);
  }
  const double loudest = *std::max_element(energy.begin(), energy.end());
  if (!(loudest > 1e-12)) return verdict(0.0, 0.0);

  dsp::RealFft fft(n);
  std::vector<float> x(n, 0.0f), p(n), re(n / 2 + 1), im(n / 2 + 1), nsdf(maxLag + 2);
  struct Frame {
    double log2Hz, clarity;
  };
  std::vector<Frame> voiced;
  std::size_t loud = 0;
  for (std::size_t f = 0; f < starts.size(); ++f) {
    if (energy[f] < loudest * kFrameGate) continue;
    ++loud;
    const float* src = mono.data() + starts[f];
    for (uint32_t j = 0; j < w; ++j) x[j] = src[j] - dc;  // x[w, n) stays 0: a linear autocorrelation
    fft.forward(x.data(), re.data(), im.data());
    for (uint32_t k = 0; k <= n / 2; ++k) p[k] = re[k] * re[k] + im[k] * im[k];
    for (uint32_t k = n / 2 + 1; k < n; ++k) p[k] = p[n - k];
    fft.forward(p.data(), re.data(), im.data());  // the power spectrum is real and even: re[τ] = n · r(τ)
    const double invN = 1.0 / n;
    double m = 2.0 * energy[f];
    for (uint32_t t = 0; t <= maxLag + 1; ++t) {
      if (t > 0) {
        const double a = x[t - 1], b = x[w - t];
        m -= a * a + b * b;
      }
      nsdf[t] = m > 1e-20 ? static_cast<float>(2.0 * re[t] * invN / m) : 0.0f;
    }
    // Key maxima: the largest value of each positive lobe after the first negative stretch.
    uint32_t t = 1;
    while (t <= maxLag && nsdf[t] > 0.0f) ++t;
    float top = 0.0f;
    uint32_t pick = 0;
    float lobeMax = 0.0f;
    uint32_t lobeAt = 0;
    std::vector<std::pair<uint32_t, float>> keys;  // small: one per lobe
    for (; t <= maxLag; ++t) {
      if (nsdf[t] > 0.0f) {
        if (nsdf[t] > lobeMax) {
          lobeMax = nsdf[t];
          lobeAt = t;
        }
      } else if (lobeAt != 0) {
        keys.emplace_back(lobeAt, lobeMax);
        lobeMax = 0.0f;
        lobeAt = 0;
      }
    }
    if (lobeAt != 0 && lobeAt < maxLag) keys.emplace_back(lobeAt, lobeMax);
    for (const auto& k : keys)
      if (k.first >= minLag) top = std::max(top, k.second);
    if (top <= 0.0f) continue;
    for (const auto& k : keys)
      if (k.first >= minLag && k.second >= kKeyMaximum * top) {
        pick = k.first;
        break;
      }
    const double a = nsdf[pick - 1], b = nsdf[pick], c = nsdf[pick + 1];
    const double den = a - 2.0 * b + c;
    const double d = std::abs(den) > 1e-12 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
    const double lag = pick + d;
    const double clarity = std::min(1.0, b - 0.25 * (a - c) * d);
    voiced.push_back({std::log2(sampleRate / lag), std::max(0.0, clarity)});
  }
  if (voiced.empty() || loud == 0) return verdict(0.0, 0.0);
  std::vector<double> lf(voiced.size());
  for (std::size_t i = 0; i < voiced.size(); ++i) lf[i] = voiced[i].log2Hz;
  std::nth_element(lf.begin(), lf.begin() + lf.size() / 2, lf.end());
  const double median = lf[lf.size() / 2];
  double sw = 0.0, swl = 0.0;
  std::vector<double> kept;
  for (const Frame& fr : voiced)
    if (std::abs(fr.log2Hz - median) <= kKeepOctaves) {
      sw += fr.clarity;
      swl += fr.clarity * fr.log2Hz;
      kept.push_back(fr.log2Hz);
    }
  if (sw <= 0.0) return verdict(0.0, 0.0);
  // The centre: the midpoint of the kept frames' 10th and 90th percentiles (a vibrato's centre even over a partial
  // cycle, where a mean leans towards the extra half cycle by up to depth / (π · cycles)); with few frames the
  // clarity-weighted mean.
  double centre = swl / sw;
  if (kept.size() >= 5) {
    std::sort(kept.begin(), kept.end());
    const auto pct = [&](double q) {
      const double at = q * static_cast<double>(kept.size() - 1);
      const std::size_t i = static_cast<std::size_t>(at);
      const double f = at - static_cast<double>(i);
      return i + 1 < kept.size() ? kept[i] + f * (kept[i + 1] - kept[i]) : kept[i];
    };
    centre = 0.5 * (pct(kPercentile) + pct(1.0 - kPercentile));
  }
  return verdict(std::exp2(centre), sw / static_cast<double>(loud));
}

PitchEstimate detectPitch(const float* const* ch, uint32_t channels, int64_t frames, uint32_t sampleRate,
                          const PitchOptions& options) {
  if (channels == 0 || frames <= 0 || sampleRate == 0) return verdict(0.0, 0.0);
  const int64_t scanned = scanFrames(frames, sampleRate);
  EnergyEnvelope env;
  constexpr int64_t kChunk = 65536;
  std::vector<const float*> p(channels);
  for (int64_t a = 0; a < scanned; a += kChunk) {
    for (uint32_t c = 0; c < channels; ++c) p[c] = ch[c] + a;
    env.push(p.data(), channels, static_cast<uint32_t>(std::min(kChunk, scanned - a)));
  }
  const std::vector<float> e = env.finish();
  const PitchWindow win = pickPitchWindow(e, scanned, sampleRate, options);
  std::vector<float> mono(static_cast<std::size_t>(win.frames), 0.0f);
  const float inv = 1.0f / static_cast<float>(channels);
  for (uint32_t c = 0; c < channels; ++c)
    for (int64_t i = 0; i < win.frames; ++i) mono[static_cast<std::size_t>(i)] += ch[c][win.start + i] * inv;
  return analysePitch(mono, sampleRate, options);
}

// ---- cache --------------------------------------------------------------------------------------------------------

PitchPtr PitchCache::find(uint64_t contentHash, int64_t frames) const {
  std::lock_guard lock(m_);
  const auto it = map_.find(cacheKey(contentHash, frames));
  if (it == map_.end() || it->second->contentHash != contentHash || it->second->frames != frames) return nullptr;
  return it->second;
}

void PitchCache::put(PitchPtr p) {
  if (p == nullptr) return;
  std::lock_guard lock(m_);
  map_[cacheKey(p->contentHash, p->frames)] = std::move(p);
}

void PitchCache::clear() {
  std::lock_guard lock(m_);
  map_.clear();
}

PitchCache& pitchCache() {
  static PitchCache c;
  return c;
}

Result<PitchPtr> loadOrDetectPitch(const TransientRequest& r, const std::atomic<bool>* cancel, const PitchOptions& options) {
  if (PitchPtr p = pitchCache().find(r.contentHash, r.frames)) return p;
  PitchEstimate est;
  if (r.resident != nullptr && !r.resident->streamed()) {
    const uint32_t ch = std::min<uint32_t>(r.resident->channels, kMaxStreamChannels);
    est = detectPitch(r.resident->data, ch, r.resident->frames, r.resident->sampleRate, options);
  } else {
    auto dec = Decoder::open(r.path);
    if (!dec) return std::move(dec).error();
    Decoder& d = **dec;
    const SampleInfo& info = d.info();
    if (info.channels == 0 || info.sampleRate == 0) return Error{ErrorCode::InvalidArgument, "pitch: no audio", {}};
    const uint32_t chans = info.channels;
    constexpr uint32_t kChunk = 32768;
    std::vector<std::vector<float>> buf(chans, std::vector<float>(kChunk));
    std::vector<float*> dst(chans);
    for (uint32_t c = 0; c < chans; ++c) dst[c] = buf[c].data();
    const auto cp = reinterpret_cast<const float* const*>(dst.data());
    // Pass 1: the envelope of the scan span.
    const int64_t scanLimit = scanFrames(static_cast<int64_t>(info.frames), info.sampleRate);
    EnergyEnvelope env;
    int64_t scanned = 0;
    while (scanned < scanLimit) {
      if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) return Error{ErrorCode::Unknown, "pitch: cancelled", {}};
      auto n = d.read(dst.data(), static_cast<uint64_t>(std::min<int64_t>(kChunk, scanLimit - scanned)));
      if (!n) return std::move(n).error();
      if (*n == 0) break;
      env.push(cp, chans, static_cast<uint32_t>(*n));
      scanned += static_cast<int64_t>(*n);
    }
    const std::vector<float> e = env.finish();
    const PitchWindow win = pickPitchWindow(e, scanned, info.sampleRate, options);
    // Pass 2: the window, mixed to mono.
    std::vector<float> mono;
    mono.reserve(static_cast<std::size_t>(win.frames));
    if (Status s = d.seek(static_cast<uint64_t>(win.start)); !s) return std::move(s).error();
    const float inv = 1.0f / static_cast<float>(chans);
    while (static_cast<int64_t>(mono.size()) < win.frames) {
      auto n = d.read(dst.data(), static_cast<uint64_t>(std::min<int64_t>(kChunk, win.frames - static_cast<int64_t>(mono.size()))));
      if (!n) return std::move(n).error();
      if (*n == 0) break;
      for (uint64_t i = 0; i < *n; ++i) {
        float m = 0.0f;
        for (uint32_t c = 0; c < chans; ++c) m += buf[c][i];
        mono.push_back(m * inv);
      }
    }
    est = analysePitch(mono, info.sampleRate, options);
  }
  auto p = std::make_shared<Pitch>();
  p->contentHash = r.contentHash;
  p->frames = r.frames;
  p->estimate = est;
  PitchPtr out = std::move(p);
  pitchCache().put(out);
  return out;
}

}  // namespace pq::media
