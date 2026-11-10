// Tempo detection and its cache (T6.8): pq/media/tempo_detect.h.
#include "pq/media/tempo_detect.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include "pq/media/decoder.h"

namespace pq::media {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kMagic = 0x50545150u;  // "PQTP"
constexpr uint32_t kVersion = 1;
constexpr double kBinSeconds = 0.001;     // histogram resolution
constexpr double kSigmaBins = 2.0;        // Gaussian smoothing (2 ms)
constexpr double kMinLagSeconds = 0.06;   // shorter intervals are flams, not rhythm
constexpr double kBpmStep = 0.02;
constexpr double kPriorCentre = 120.0;
constexpr double kPriorOctaves = 0.8;
constexpr double kConfidentRatio = 30.0;  // best / median comb score that reads as confidence 1
constexpr double kStrengthWindow = 0.04;  // seconds on each side of an onset
constexpr float kMinStrength = 0.05f;
constexpr double kStrongWeight = 0.2;  // a loud hit (of the largest weight), for the prior's centre  // a loud hit, for the prior's centre

uint64_t cacheKey(uint64_t hash, int64_t frames) { return hash ^ (static_cast<uint64_t>(frames) * 0x9E3779B97F4A7C15ull); }

}  // namespace

// ---- envelope and weights ----------------------------------------------------------------------------------------

void EnergyEnvelope::push(const float* const* ch, uint32_t channels, uint32_t n) {
  if (channels == 0) return;
  const float inv = 1.0f / static_cast<float>(channels);
  for (uint32_t i = 0; i < n; ++i) {
    float m = 0.0f;
    for (uint32_t c = 0; c < channels; ++c) m += ch[c][i];
    m *= inv;
    acc_ += static_cast<double>(m) * m;
    if (++fill_ == kEnvelopeBlock) {
      blocks_.push_back(static_cast<float>(acc_ / kEnvelopeBlock));
      acc_ = 0.0;
      fill_ = 0;
    }
  }
}

std::vector<float> EnergyEnvelope::finish() {
  if (fill_ > 0) blocks_.push_back(static_cast<float>(acc_ / fill_));
  acc_ = 0.0;
  fill_ = 0;
  return std::move(blocks_);
}

std::vector<float> onsetStrengths(std::span<const double> onsets, std::span<const float> env, double sampleRate) {
  std::vector<float> s(onsets.size(), 1.0f);
  if (env.empty() || onsets.empty()) return s;
  const auto win = std::max<int64_t>(1, std::llround(kStrengthWindow * sampleRate / kEnvelopeBlock));
  const auto nb = static_cast<int64_t>(env.size());
  float top = 0.0f;
  for (size_t i = 0; i < onsets.size(); ++i) {
    const int64_t b = std::clamp<int64_t>(static_cast<int64_t>(onsets[i] / kEnvelopeBlock), 0, nb - 1);
    double after = 0.0, before = 0.0;
    int64_t na = 0, nbf = 0;
    for (int64_t k = b; k < std::min(nb, b + win); ++k, ++na) after += env[static_cast<size_t>(k)];
    for (int64_t k = std::max<int64_t>(0, b - win); k < b; ++k, ++nbf) before += env[static_cast<size_t>(k)];
    const double ra = na > 0 ? std::sqrt(after / static_cast<double>(na)) : 0.0;
    const double rb = nbf > 0 ? std::sqrt(before / static_cast<double>(nbf)) : 0.0;
    s[i] = static_cast<float>(std::max(0.0, ra - rb));
    top = std::max(top, s[i]);
  }
  for (float& v : s) v = top > 0.0f ? std::max(kMinStrength, v / top) : 1.0f;
  return s;
}

// ---- the estimator -----------------------------------------------------------------------------------------------

TempoEstimate estimateTempo(std::span<const double> onsets, std::span<const float> strengths, double sampleRate,
                            int64_t frames, const TempoOptions& opt) {
  TempoEstimate out;
  if (onsets.size() < 4 || !(sampleRate > 0.0) || !(opt.maxBpm > opt.minBpm) || !(opt.minBpm > 0.0)) return out;
  const double dur = std::max(frames > 0 ? static_cast<double>(frames) / sampleRate : 0.0, onsets.back() / sampleRate);
  const double lagMax = std::min(kMaxLagSeconds, dur);
  if (!(lagMax > 60.0 / opt.maxBpm)) return out;
  const auto nbins = static_cast<size_t>(std::ceil(lagMax / kBinSeconds)) + 2;
  std::vector<double> h(nbins, 0.0);
  auto w = [&](size_t i) { return strengths.size() == onsets.size() ? static_cast<double>(strengths[i]) : 1.0; };
  // 1. The weighted inter-onset-interval histogram (linear splat into 1 ms bins).
  for (size_t i = 0; i < onsets.size(); ++i) {
    const double ti = onsets[i] / sampleRate, wi = w(i);
    for (size_t j = i + 1; j < onsets.size(); ++j) {
      const double d = onsets[j] / sampleRate - ti;
      if (d > lagMax) break;
      if (d < kMinLagSeconds) continue;
      const double x = d / kBinSeconds;
      const auto b = static_cast<size_t>(x);
      const double f = x - static_cast<double>(b);
      const double v = wi * w(j);
      h[b] += v * (1.0 - f);
      if (b + 1 < nbins) h[b + 1] += v * f;
    }
  }
  // 2. Smoothed (Gaussian, σ = 2 ms) and normalised by the overlap the lag leaves.
  const int32_t reach = static_cast<int32_t>(std::ceil(3.0 * kSigmaBins));
  std::vector<double> kern(static_cast<size_t>(2 * reach + 1));
  for (int32_t k = -reach; k <= reach; ++k)
    kern[static_cast<size_t>(k + reach)] = std::exp(-0.5 * (k * k) / (kSigmaBins * kSigmaBins));
  std::vector<double> hs(nbins, 0.0);
  for (size_t b = 0; b < nbins; ++b) {
    double acc = 0.0;
    for (int32_t k = -reach; k <= reach; ++k) {
      const auto j = static_cast<int64_t>(b) + k;
      if (j >= 0 && j < static_cast<int64_t>(nbins)) acc += h[static_cast<size_t>(j)] * kern[static_cast<size_t>(k + reach)];
    }
    const double lag = static_cast<double>(b) * kBinSeconds;
    hs[b] = acc / std::max(dur - lag, 0.25 * dur);
  }
  auto at = [&](double lag) {
    const double x = lag / kBinSeconds;
    const auto b = static_cast<size_t>(x);
    if (b + 1 >= nbins) return 0.0;
    const double f = x - static_cast<double>(b);
    return hs[b] * (1.0 - f) + hs[b + 1] * f;
  };
  // 3. The prior's centre: the spacing of the loud hits (consecutive intervals between onsets of at least 0.2 of the
  //    largest weight; their median, folded by octaves into the range), else kPriorCentre.
  double centre = kPriorCentre;
  {
    std::vector<double> iois;
    double last = -1.0;
    for (size_t i = 0; i < onsets.size(); ++i) {
      if (w(i) < kStrongWeight) continue;
      const double t = onsets[i] / sampleRate;
      if (last >= 0.0 && t - last >= kMinLagSeconds) iois.push_back(t - last);
      if (last < 0.0 || t - last >= kMinLagSeconds) last = t;
    }
    if (iois.size() >= 3) {
      std::nth_element(iois.begin(), iois.begin() + static_cast<std::ptrdiff_t>(iois.size() / 2), iois.end());
      double c = 60.0 / iois[iois.size() / 2];
      while (c > opt.maxBpm) c *= 0.5;
      while (c < opt.minBpm) c *= 2.0;
      centre = c;
    }
  }
  // 4. The comb score of each candidate tempo, and the prior.
  const auto nc = static_cast<size_t>(std::floor((opt.maxBpm - opt.minBpm) / kBpmStep)) + 1;
  std::vector<double> raw(nc), scored(nc);
  size_t best = 0;
  for (size_t c = 0; c < nc; ++c) {
    const double bpm = opt.minBpm + static_cast<double>(c) * kBpmStep;
    const double period = 60.0 / bpm;
    const auto kmax = static_cast<int32_t>(std::floor(lagMax / period));
    double s = 0.0;
    for (int32_t k = 1; k <= kmax; ++k) s += at(k * period);
    raw[c] = kmax > 0 ? s / kmax : 0.0;
    const double oct = std::log2(bpm / centre) / kPriorOctaves;
    scored[c] = raw[c] * std::exp(-0.5 * oct * oct);
    if (scored[c] > scored[best]) best = c;
  }
  if (!(raw[best] > 0.0)) return out;
  // 5. Refinement (a parabola through the neighbours) and confidence.
  double bpm = opt.minBpm + static_cast<double>(best) * kBpmStep;
  if (best > 0 && best + 1 < nc) {
    const double a = scored[best - 1], b = scored[best], c = scored[best + 1];
    const double den = a - 2.0 * b + c;
    if (den < 0.0) bpm += std::clamp(0.5 * (a - c) / den, -0.5, 0.5) * kBpmStep;
  }
  std::vector<double> sorted = raw;
  std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(nc / 2), sorted.end());
  const double median = sorted[nc / 2];
  out.bpm = bpm;
  const double ratio = raw[best] / std::max(median, 1e-3 * raw[best]);
  out.confidence = static_cast<float>(std::clamp(std::log(ratio) / std::log(kConfidentRatio), 0.0, 1.0));
  return out;
}

TempoEstimate detectTempo(const float* const* ch, uint32_t channels, int64_t frames, uint32_t sampleRate,
                          const TempoOptions& options) {
  const std::vector<double> onsets = detectTransients(ch, channels, frames, sampleRate);
  EnergyEnvelope env;
  constexpr int64_t kChunk = 65536;
  std::vector<const float*> p(channels);
  for (int64_t a = 0; a < frames; a += kChunk) {
    for (uint32_t c = 0; c < channels; ++c) p[c] = ch[c] + a;
    env.push(p.data(), channels, static_cast<uint32_t>(std::min(kChunk, frames - a)));
  }
  const std::vector<float> e = env.finish();
  return estimateTempo(onsets, onsetStrengths(onsets, e, sampleRate), sampleRate, frames, options);
}

// ---- cache -------------------------------------------------------------------------------------------------------

fs::path tempoCacheFile(const fs::path& dir, uint64_t contentHash, int64_t frames) {
  char name[64];
  std::snprintf(name, sizeof name, "%016llx-%llx.pqtp", static_cast<unsigned long long>(contentHash),
                static_cast<unsigned long long>(frames));
  return dir / name;
}

Status saveTempo(const Tempo& t, const fs::path& file) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::path part = file;
  part += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetCurrentThreadId()) + ".part";
  {
    std::ofstream o(part, std::ios::binary | std::ios::trunc);
    if (!o) return Error{ErrorCode::Io, "tempo: cannot write " + part.string(), {}};
    o.write(reinterpret_cast<const char*>(&kMagic), 4);
    o.write(reinterpret_cast<const char*>(&kVersion), 4);
    o.write(reinterpret_cast<const char*>(&t.contentHash), 8);
    o.write(reinterpret_cast<const char*>(&t.frames), 8);
    o.write(reinterpret_cast<const char*>(&t.estimate.bpm), 8);
    o.write(reinterpret_cast<const char*>(&t.estimate.confidence), 4);
    if (!o) return Error{ErrorCode::Io, "tempo: write failed " + part.string(), {}};
  }
  fs::rename(part, file, ec);
  if (ec) {
    fs::remove(part, ec);
    return Error{ErrorCode::Io, "tempo: cannot rename to " + file.string(), {}};
  }
  return Ok{};
}

Result<TempoPtr> loadTempo(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return Error{ErrorCode::NotFound, "tempo: no " + file.string(), {}};
  uint32_t magic = 0, version = 0;
  auto t = std::make_shared<Tempo>();
  in.read(reinterpret_cast<char*>(&magic), 4);
  in.read(reinterpret_cast<char*>(&version), 4);
  in.read(reinterpret_cast<char*>(&t->contentHash), 8);
  in.read(reinterpret_cast<char*>(&t->frames), 8);
  in.read(reinterpret_cast<char*>(&t->estimate.bpm), 8);
  in.read(reinterpret_cast<char*>(&t->estimate.confidence), 4);
  if (!in || magic != kMagic || version != kVersion || !std::isfinite(t->estimate.bpm) ||
      !(t->estimate.confidence >= 0.0f && t->estimate.confidence <= 1.0f))
    return Error{ErrorCode::InvalidArgument, "tempo: bad file " + file.string(), {}};
  return TempoPtr(std::move(t));
}

TempoPtr TempoCache::find(uint64_t contentHash, int64_t frames) const {
  std::lock_guard lock(m_);
  const auto it = map_.find(cacheKey(contentHash, frames));
  if (it == map_.end() || it->second->contentHash != contentHash || it->second->frames != frames) return nullptr;
  return it->second;
}

void TempoCache::put(TempoPtr t) {
  if (t == nullptr) return;
  std::lock_guard lock(m_);
  map_[cacheKey(t->contentHash, t->frames)] = std::move(t);
}

void TempoCache::clear() {
  std::lock_guard lock(m_);
  map_.clear();
}

TempoCache& tempoCache() {
  static TempoCache c;
  return c;
}

Result<TempoPtr> loadOrDetectTempo(const TransientRequest& r, const std::atomic<bool>* cancel, const TempoOptions& options) {
  if (TempoPtr t = tempoCache().find(r.contentHash, r.frames)) return t;
  const fs::path file = r.cacheDir.empty() ? fs::path() : tempoCacheFile(r.cacheDir, r.contentHash, r.frames);
  if (!file.empty()) {
    auto l = loadTempo(file);
    if (l && (*l)->contentHash == r.contentHash && (*l)->frames == r.frames) {
      tempoCache().put(*l);
      return *l;
    }
  }
  // The transients when cached already (memory, then disk), else detected in the same pass as the envelope.
  TransientsPtr tr = transientCache().find(r.contentHash, r.frames);
  if (tr == nullptr && !r.cacheDir.empty()) {
    auto l = loadTransients(transientCacheFile(r.cacheDir, r.contentHash, r.frames));
    if (l && (*l)->contentHash == r.contentHash && (*l)->frames == r.frames) {
      tr = *l;
      transientCache().put(tr);
    }
  }
  EnergyEnvelope env;
  std::vector<double> onsets;
  uint32_t rate = 0;
  int64_t frames = r.frames;
  if (r.resident != nullptr && !r.resident->streamed()) {
    const uint32_t ch = std::min<uint32_t>(r.resident->channels, kMaxStreamChannels);
    rate = r.resident->sampleRate;
    frames = r.resident->frames;
    constexpr int64_t kChunk = 65536;
    const float* p[kMaxStreamChannels]{};
    for (int64_t a = 0; a < frames; a += kChunk) {
      for (uint32_t c = 0; c < ch; ++c) p[c] = r.resident->data[c] + a;
      env.push(p, ch, static_cast<uint32_t>(std::min(kChunk, frames - a)));
    }
    if (tr == nullptr) onsets = detectTransients(r.resident->data, ch, frames, rate);
  } else {
    auto dec = Decoder::open(r.path);
    if (!dec) return std::move(dec).error();
    Decoder& d = **dec;
    const SampleInfo& info = d.info();
    if (info.channels == 0) return Error{ErrorCode::InvalidArgument, "tempo: no channels", {}};
    rate = info.sampleRate;
    std::unique_ptr<TransientDetector> det;
    if (tr == nullptr) det = std::make_unique<TransientDetector>(rate);
    constexpr uint32_t kChunk = 32768;
    std::vector<std::vector<float>> buf(info.channels, std::vector<float>(kChunk));
    std::vector<float*> dst(info.channels);
    for (uint32_t c = 0; c < info.channels; ++c) dst[c] = buf[c].data();
    for (;;) {
      if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) return Error{ErrorCode::Unknown, "tempo: cancelled", {}};
      auto n = d.read(dst.data(), kChunk);
      if (!n) return std::move(n).error();
      if (*n == 0) break;
      const auto cp = reinterpret_cast<const float* const*>(dst.data());
      env.push(cp, info.channels, static_cast<uint32_t>(*n));
      if (det) det->push(cp, info.channels, static_cast<uint32_t>(*n));
    }
    if (det) onsets = det->finish();
  }
  if (tr == nullptr) {
    auto t = std::make_shared<Transients>();
    t->contentHash = r.contentHash;
    t->frames = r.frames;
    t->onsets = std::move(onsets);
    if (!r.cacheDir.empty()) (void)saveTransients(*t, transientCacheFile(r.cacheDir, r.contentHash, r.frames));
    tr = std::move(t);
    transientCache().put(tr);
  }
  const std::vector<float> e = env.finish();
  auto t = std::make_shared<Tempo>();
  t->contentHash = r.contentHash;
  t->frames = r.frames;
  t->estimate = estimateTempo(tr->onsets, onsetStrengths(tr->onsets, e, rate), rate, frames, options);
  if (!file.empty()) (void)saveTempo(*t, file);  // a failed save only costs a later re-analysis
  TempoPtr p = std::move(t);
  tempoCache().put(p);
  return p;
}

}  // namespace pq::media
