// Transient detection and its cache (T6.3): pq/media/transients.h.
#include "pq/media/transients.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numbers>
#include <string>

#include "pq/media/decoder.h"

namespace pq::media {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kFineBlock = 16;  // frames per refinement energy block
constexpr uint32_t kMagic = 0x52545150u;  // "PQTR"
constexpr uint32_t kVersion = 1;

uint32_t fftSizeFor(uint32_t sr) {
  uint32_t n = 256;
  while (n < 65536 && static_cast<double>(n) < 0.021 * static_cast<double>(sr)) n *= 2;
  return std::max<uint32_t>(n, 256);
}

}  // namespace

TransientDetector::TransientDetector(uint32_t sampleRate, const TransientOptions& options)
    : sr_(std::max<uint32_t>(sampleRate, 8000)), n_(fftSizeFor(sr_)), hop_(n_ / 4), opt_(options), fft_(n_) {
  win_.resize(n_);
  for (uint32_t i = 0; i < n_; ++i)
    win_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n_)));
  tmp_.resize(n_);
  re_.resize(n_ / 2 + 1);
  im_.resize(n_ / 2 + 1);
  // Bands: 12 per octave from 30 Hz (at least one bin each) up to Nyquist.
  const double binHz = static_cast<double>(sr_) / static_cast<double>(n_);
  uint32_t lo = 1;
  edges_.push_back(lo);
  for (double f = 30.0 * std::exp2(1.0 / 12.0); lo < n_ / 2; f *= std::exp2(1.0 / 12.0)) {
    const auto hi = std::min<uint32_t>(n_ / 2 + 1, std::max<uint32_t>(lo + 1, static_cast<uint32_t>(std::lround(f / binHz))));
    edges_.push_back(hi);
    lo = hi;
  }
  const size_t bands = edges_.size() - 1;
  mag_.assign(bands, 0.0f);
  prev_.assign(bands, 0.0f);
  buf_.assign(n_, 0.0f);  // leading silence: the first frames see "nothing before"
  bufStart_ = -static_cast<int64_t>(n_);
  next_ = bufStart_;
}

void TransientDetector::frame(const float* x) {
  for (uint32_t i = 0; i < n_; ++i) tmp_[i] = x[i] * win_[i];
  fft_.forward(tmp_.data(), re_.data(), im_.data());
  const float scale = 100.0f * 4.0f / static_cast<float>(n_);  // a sine of amplitude a peaks at 100·a
  const size_t bands = edges_.size() - 1;
  for (size_t b = 0; b < bands; ++b) {
    float m = 0.0f;
    for (uint32_t k = edges_[b]; k < edges_[b + 1]; ++k) m = std::max(m, re_[k] * re_[k] + im_[k] * im_[k]);
    mag_[b] = std::log1p(scale * std::sqrt(m));
  }
  // Rectified flux against the previous frame, each band against the neighbours' maximum.
  double flux = 0.0;
  for (size_t b = 0; b < bands; ++b) {
    float ref = prev_[b];
    if (b > 0) ref = std::max(ref, prev_[b - 1]);
    if (b + 1 < bands) ref = std::max(ref, prev_[b + 1]);
    flux += std::max(0.0f, mag_[b] - ref);
  }
  flux_.push_back(static_cast<float>(flux));
  std::swap(prev_, mag_);
}

void TransientDetector::push(const float* const* ch, uint32_t channels, uint32_t n) {
  if (channels == 0 || n == 0) return;
  const float inv = 1.0f / static_cast<float>(channels);
  const size_t at = buf_.size();
  buf_.resize(at + n);
  for (uint32_t i = 0; i < n; ++i) {
    float s = 0.0f;
    for (uint32_t c = 0; c < channels; ++c) s += ch[c][i];
    s *= inv;
    buf_[at + i] = s;
    fineAcc_ += static_cast<double>(s) * s;
    if (++fineFill_ == kFineBlock) {
      fine_.push_back(static_cast<float>(fineAcc_));
      fineAcc_ = 0.0;
      fineFill_ = 0;
    }
  }
  total_ += n;
  while (next_ + n_ <= bufStart_ + static_cast<int64_t>(buf_.size())) {
    frame(buf_.data() + (next_ - bufStart_));
    next_ += hop_;
  }
  if (next_ - bufStart_ > 65536) {
    buf_.erase(buf_.begin(), buf_.begin() + (next_ - bufStart_));
    bufStart_ = next_;
  }
}

std::vector<double> TransientDetector::finish() {
  const int64_t frames = total_;
  {  // trailing silence: the last frames are analysed whole
    std::vector<float> z(n_, 0.0f);
    const float* p = z.data();
    const size_t keepBlocks = fine_.size();
    const uint32_t keepFill = fineFill_;
    const double keepAcc = fineAcc_;
    push(&p, 1, n_);
    total_ = frames;
    fine_.resize(keepBlocks);  // the silence is not part of the file's energy blocks
    fineFill_ = keepFill;
    fineAcc_ = keepAcc;
  }
  if (fineFill_ != 0) fine_.push_back(static_cast<float>(fineAcc_ * kFineBlock / fineFill_));
  std::vector<double> out;
  const auto T = static_cast<int64_t>(flux_.size());
  if (T == 0) return out;
  std::vector<float> sorted(flux_);
  std::nth_element(sorted.begin(), sorted.begin() + T / 2, sorted.end());
  double maxF = 0.0;
  for (float f : flux_) maxF = std::max(maxF, static_cast<double>(f));
  if (maxF <= 1e-6) return out;
  // Above the local mean by `threshold` × the median flux (the material's background: most frames hold no onset), at
  // least 1 % of the largest (silence or a steady tone has a median of ~0).
  const double delta = std::max(opt_.threshold * static_cast<double>(sorted[static_cast<size_t>(T / 2)]), 0.01 * maxF);
  const auto minGap = static_cast<int64_t>(std::ceil(opt_.minGapSeconds * static_cast<float>(sr_)));
  struct Cand {
    double at;
    float strength;
  };
  std::vector<Cand> picks;
  const auto E = static_cast<int64_t>(fine_.size());
  std::vector<double> cum(static_cast<size_t>(E) + 1, 0.0);  // cumulative block energy at block boundaries
  for (int64_t k = 0; k < E; ++k) cum[static_cast<size_t>(k) + 1] = cum[static_cast<size_t>(k)] + fine_[static_cast<size_t>(k)];
  const auto blk = static_cast<int64_t>(kFineBlock);
  for (int64_t t = 1; t < T; ++t) {
    const float f = flux_[static_cast<size_t>(t)];
    bool peak = true;
    for (int64_t d = 1; d <= 3 && peak; ++d) {
      if (t - d >= 0 && !(f > flux_[static_cast<size_t>(t - d)])) peak = false;
      if (t + d < T && !(f >= flux_[static_cast<size_t>(t + d)])) peak = false;
    }
    if (!peak) continue;
    double mean = 0.0;
    int cnt = 0;
    for (int64_t d = t - 10; d <= t + 3; ++d)
      if (d >= 0 && d < T) {
        mean += flux_[static_cast<size_t>(d)];
        ++cnt;
      }
    mean /= std::max(cnt, 1);
    if (f - mean < delta) continue;
    // Refinement on the cumulative energy of 16-frame blocks over [a − 2·hop, a + n) (frame t and the one before):
    // the onset is the knee where its slope (the power) jumps, the breakpoint of the best two-line fit, then
    // the two lines' intersection (sub-block). Integrated energy has no zero-crossing dips, so a low note's attack is
    // found as precisely as a click's.
    const int64_t a = -static_cast<int64_t>(n_) + t * hop_;
    const int64_t w0 = std::max<int64_t>(0, (a - 2 * hop_) / blk);
    const int64_t w1 = std::min<int64_t>(E, (a + n_) / blk + 1);
    if (w1 - w0 < 6) continue;
    const double base = cum[static_cast<size_t>(w0)];
    auto fit = [&](int64_t lo, int64_t hi, double* slope, double* icept) {  // least squares over boundaries lo … hi
      double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
      const auto n = static_cast<double>(hi - lo + 1);
      for (int64_t k = lo; k <= hi; ++k) {
        const auto x = static_cast<double>(k - w0);
        const double y = cum[static_cast<size_t>(k)] - base;
        sx += x, sy += y, sxx += x * x, sxy += x * y, syy += y * y;
      }
      const double vxx = sxx - sx * sx / n, vxy = sxy - sx * sy / n, vyy = syy - sy * sy / n;
      const double b = vxx > 0.0 ? vxy / vxx : 0.0;
      if (slope != nullptr) *slope = b;
      if (icept != nullptr) *icept = (sy - b * sx) / n;
      return std::max(0.0, vyy - b * vxy);
    };
    int64_t m = w0 + 2;
    double bestErr = 1e300;
    for (int64_t k = w0 + 2; k <= w1 - 2; ++k) {
      const double err = fit(w0, k, nullptr, nullptr) + fit(k, w1, nullptr, nullptr);
      if (err < bestErr) {
        bestErr = err;
        m = k;
      }
    }
    double b1 = 0, c1 = 0, b2 = 0, c2 = 0;
    (void)fit(w0, m, &b1, &c1);
    (void)fit(m, w1, &b2, &c2);
    double knee = static_cast<double>(m - w0);
    if (b2 > b1) knee = std::clamp((c1 - c2) / (b2 - b1), knee - 1.0, knee + 1.0);
    // Then the attack itself near the knee: the first block within ±3 ms whose energy passes 20 % of the way from
    // the loudest block of the 3 ms before that span to the loudest of the 3 ms after the knee.
    const auto kb = static_cast<int64_t>(std::llround(static_cast<double>(w0) + knee));
    const auto r3 = static_cast<int64_t>(std::ceil(0.003 * static_cast<double>(sr_) / static_cast<double>(blk)));
    double pre = 0.0, post = 0.0;
    for (int64_t k = kb - 2 * r3; k < kb - r3; ++k) pre = std::max(pre, k >= 0 && k < E ? static_cast<double>(fine_[static_cast<size_t>(k)]) : 0.0);
    for (int64_t k = kb; k < kb + r3; ++k) post = std::max(post, k >= 0 && k < E ? static_cast<double>(fine_[static_cast<size_t>(k)]) : 0.0);
    double at = static_cast<double>(blk) * (static_cast<double>(w0) + knee);
    if (post > 2.0 * pre) {
      const double thr = pre + 0.2 * (post - pre);
      for (int64_t k = std::max<int64_t>(0, kb - r3); k < std::min(E, kb + r3); ++k) {
        const double ek = fine_[static_cast<size_t>(k)];
        if (ek <= thr) continue;
        // The fraction of the block the new energy fills (a constant level after the onset).
        const double nx = k + 1 < E ? std::max(ek, static_cast<double>(fine_[static_cast<size_t>(k + 1)])) : ek;
        const double frac = std::clamp((ek - pre) / std::max(nx - pre, 1e-30), 0.0, 1.0);
        at = static_cast<double>(blk) * (static_cast<double>(k + 1) - frac);
        break;
      }
    }
    at = std::clamp(at, 0.0, static_cast<double>(std::max<int64_t>(frames - 1, 0)));
    if (!picks.empty() && at - picks.back().at < static_cast<double>(minGap)) {
      if (f > picks.back().strength) picks.back() = {at, f};
      continue;
    }
    picks.push_back({at, f});
  }
  out.reserve(picks.size());
  for (const Cand& c : picks) out.push_back(std::round(c.at));
  return out;
}

std::vector<double> detectTransients(const float* const* ch, uint32_t channels, int64_t frames, uint32_t sampleRate,
                                     const TransientOptions& options) {
  TransientDetector d(sampleRate, options);
  constexpr int64_t kChunk = 65536;
  std::vector<const float*> p(channels);
  for (int64_t at = 0; at < frames; at += kChunk) {
    for (uint32_t c = 0; c < channels; ++c) p[c] = ch[c] + at;
    d.push(p.data(), channels, static_cast<uint32_t>(std::min(kChunk, frames - at)));
  }
  return d.finish();
}

Result<std::vector<double>> detectTransients(Decoder& dec, const std::atomic<bool>* cancel, const TransientOptions& options) {
  const SampleInfo& info = dec.info();
  if (info.channels == 0) return Error{ErrorCode::InvalidArgument, "transients: no channels", {}};
  if (Status s = dec.seek(0); !s.ok()) return s.error();
  TransientDetector d(info.sampleRate, options);
  constexpr uint32_t kChunk = 32768;
  std::vector<std::vector<float>> buf(info.channels, std::vector<float>(kChunk));
  std::vector<float*> dst(info.channels);
  for (uint32_t c = 0; c < info.channels; ++c) dst[c] = buf[c].data();
  for (;;) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) return Error{ErrorCode::Unknown, "transients: cancelled", {}};
    auto r = dec.read(dst.data(), kChunk);
    if (!r) return std::move(r).error();
    if (*r == 0) break;
    d.push(reinterpret_cast<const float* const*>(dst.data()), info.channels, static_cast<uint32_t>(*r));
  }
  return d.finish();
}

fs::path transientCacheFile(const fs::path& dir, uint64_t contentHash, int64_t frames) {
  char name[64];
  std::snprintf(name, sizeof name, "%016llx-%llx.pqtr", static_cast<unsigned long long>(contentHash),
                static_cast<unsigned long long>(frames));
  return dir / name;
}

Status saveTransients(const Transients& t, const fs::path& file) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::path part = file;
  part += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetCurrentThreadId()) + ".part";
  {
    std::ofstream o(part, std::ios::binary | std::ios::trunc);
    if (!o) return Error{ErrorCode::Io, "transients: cannot write " + part.string(), {}};
    const uint64_t count = t.onsets.size();
    o.write(reinterpret_cast<const char*>(&kMagic), 4);
    o.write(reinterpret_cast<const char*>(&kVersion), 4);
    o.write(reinterpret_cast<const char*>(&t.contentHash), 8);
    o.write(reinterpret_cast<const char*>(&t.frames), 8);
    o.write(reinterpret_cast<const char*>(&count), 8);
    o.write(reinterpret_cast<const char*>(t.onsets.data()), static_cast<std::streamsize>(count * sizeof(double)));
    if (!o) return Error{ErrorCode::Io, "transients: write failed " + part.string(), {}};
  }
  fs::rename(part, file, ec);
  if (ec) {
    fs::remove(part, ec);
    return Error{ErrorCode::Io, "transients: cannot rename to " + file.string(), {}};
  }
  return Ok{};
}

Result<TransientsPtr> loadTransients(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return Error{ErrorCode::NotFound, "transients: no " + file.string(), {}};
  uint32_t magic = 0, version = 0;
  uint64_t count = 0;
  auto t = std::make_shared<Transients>();
  in.read(reinterpret_cast<char*>(&magic), 4);
  in.read(reinterpret_cast<char*>(&version), 4);
  in.read(reinterpret_cast<char*>(&t->contentHash), 8);
  in.read(reinterpret_cast<char*>(&t->frames), 8);
  in.read(reinterpret_cast<char*>(&count), 8);
  if (!in || magic != kMagic || version != kVersion || count > (uint64_t{1} << 28))
    return Error{ErrorCode::InvalidArgument, "transients: bad file " + file.string(), {}};
  t->onsets.resize(static_cast<size_t>(count));
  in.read(reinterpret_cast<char*>(t->onsets.data()), static_cast<std::streamsize>(count * sizeof(double)));
  if (!in) return Error{ErrorCode::InvalidArgument, "transients: truncated " + file.string(), {}};
  for (size_t i = 1; i < t->onsets.size(); ++i)
    if (!(t->onsets[i] > t->onsets[i - 1])) return Error{ErrorCode::InvalidArgument, "transients: unordered " + file.string(), {}};
  return TransientsPtr(std::move(t));
}

namespace {
uint64_t cacheKey(uint64_t hash, int64_t frames) { return hash ^ (static_cast<uint64_t>(frames) * 0x9E3779B97F4A7C15ull); }
}  // namespace

TransientsPtr TransientCache::find(uint64_t contentHash, int64_t frames) const {
  std::lock_guard lock(m_);
  const auto it = map_.find(cacheKey(contentHash, frames));
  if (it == map_.end() || it->second->contentHash != contentHash || it->second->frames != frames) return nullptr;
  return it->second;
}

void TransientCache::put(TransientsPtr t) {
  if (t == nullptr) return;
  std::lock_guard lock(m_);
  map_[cacheKey(t->contentHash, t->frames)] = std::move(t);
}

void TransientCache::clear() {
  std::lock_guard lock(m_);
  map_.clear();
}

TransientCache& transientCache() {
  static TransientCache c;
  return c;
}

Result<TransientsPtr> loadOrDetectTransients(const TransientRequest& r, const std::atomic<bool>* cancel) {
  if (TransientsPtr t = transientCache().find(r.contentHash, r.frames)) return t;
  const fs::path file = r.cacheDir.empty() ? fs::path() : transientCacheFile(r.cacheDir, r.contentHash, r.frames);
  if (!file.empty()) {
    auto l = loadTransients(file);
    if (l && (*l)->contentHash == r.contentHash && (*l)->frames == r.frames) {
      transientCache().put(*l);
      return *l;
    }
  }
  auto t = std::make_shared<Transients>();
  t->contentHash = r.contentHash;
  t->frames = r.frames;
  if (r.resident != nullptr && !r.resident->streamed()) {
    const uint32_t ch = std::min<uint32_t>(r.resident->channels, kMaxStreamChannels);
    t->onsets = detectTransients(r.resident->data, ch, r.resident->frames, r.resident->sampleRate);
  } else {
    auto dec = Decoder::open(r.path);
    if (!dec) return std::move(dec).error();
    auto o = detectTransients(**dec, cancel);
    if (!o) return std::move(o).error();
    t->onsets = std::move(*o);
  }
  if (!file.empty()) (void)saveTransients(*t, file);  // a failed save only costs a later re-detection
  TransientsPtr p = std::move(t);
  transientCache().put(p);
  return p;
}

}  // namespace pq::media
