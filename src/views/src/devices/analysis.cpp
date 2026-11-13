// SpectrumFeed and AnalysisHub (pq/views/devices/analysis.h; T4.6, TX.spectrum).

#include "pq/views/devices/analysis.h"

#include <algorithm>
#include <optional>
#include <chrono>
#include <cmath>

namespace pq::views::devices {

namespace {
constexpr uint32_t kChunk = 4096;          // frames read from the tap per pass
constexpr double kNoDataSeconds = 0.15;    // then the levels fall as for silence (PQ Meter: 150 ms)
constexpr double kFrameSeconds = 1.0 / 60.0;
constexpr double kWatchSeconds = 0.1;      // a settled watched feed looks at its tap this often
constexpr float kSettledDb = -100.0f;      // below the display floor (−78 dB) with margin
constexpr float kSilent = 1e-7f;           // |x| below this (−140 dBFS): a chunk counts as silence
constexpr double kScopeRestSeconds = 2.2;  // TX.scope: silent this long, the history's longest window is flat

double clockNow() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

dsp::SpectrumConfig axisOnly(dsp::SpectrumConfig c) {
  c.sharpLows = false;  // the axis only
  return c;
}

bool silent(const float* x, uint32_t n) noexcept {
  float m = 0.0f;
  for (uint32_t i = 0; i < n; ++i) m = std::max(m, std::fabs(x[i]));
  return m < kSilent;
}
}  // namespace

SpectrumFeed::SpectrumFeed(TapRing& tap, const dsp::SpectrumConfig& config, Manual)
    : tap_(tap), cfg_(config), out_(std::make_unique<dsp::SpectrumAnalyzer>(config)),
      in_(std::make_unique<dsp::SpectrumAnalyzer>(tap.channels() >= 4 ? config : axisOnly(config))), axis_(axisOnly(config)) {
  const uint32_t nch = std::min<uint32_t>(4, tap_.channels());
  for (uint32_t c = 0; c < nch; ++c) ch_[c].resize(kChunk);
  mono_.resize(std::max<uint32_t>(kChunk, cfg_.hop * 32));
  if (tap_.channels() >= 4) monoIn_.resize(mono_.size());  // the transfer analyser: made when first wanted (work)
  snap_.out.assign(cfg_.points, cfg_.floorDb);
  snap_.peaks.assign(cfg_.points, cfg_.floorDb);
  tuning_ = {config.fftSize, config.sharpLows, config.tiltDbPerOct, config.smoothingOct, config.attackMs, config.releaseMs,
             config.holdSeconds, config.fallDbPerSecond};
  runFft_ = config.fftSize;
}

SpectrumFeed::SpectrumFeed(TapRing& tap, const dsp::SpectrumConfig& config) : SpectrumFeed(tap, config, Manual{}) {
  thread_ = std::thread([this] { run(); });
}

SpectrumFeed::SpectrumFeed(std::shared_ptr<TapRing> owner, const dsp::SpectrumConfig& config)
    : SpectrumFeed(*owner, config, Manual{}) {
  owner_ = std::move(owner);
  thread_ = std::thread([this] { run(); });
}

SpectrumFeed::~SpectrumFeed() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  for (uint32_t c = consumers_.load(); c > 0; --c) tap_.removeReader();
}

void SpectrumFeed::acquire(uint32_t wants) {
  if (wants & kWantScope) {
    {
      std::lock_guard lock(scopeMutex_);
      for (uint32_t c = 0; c < scopeChannels(); ++c)
        if (scope_[c].empty()) scope_[c].assign(kScopeFrames, c < 2 && tap_.channels() >= 4 ? std::nanf("") : 0.0f);
    }
    wantScope_.fetch_add(1, std::memory_order_relaxed);
  }
  if (wants & kWantSpectrogram) {
    {
      std::lock_guard lock(gramMutex_);
      if (gramRows_.empty()) {  // the ring, allocated by the first spectrogram consumer
        gramRows_.assign(static_cast<size_t>(kGramRing) * kGramRows, 0.0f);
        gramInfo_.assign(kGramRing, GramColumn{});
      }
    }
    wantGram_.fetch_add(1, std::memory_order_relaxed);
  }
  // A consumer of the history or the columns only: no spectrum is computed for it.
  if (wants != 0 && (wants & ~(kWantScope | kWantSpectrogram)) == 0) noOutput_.fetch_add(1, std::memory_order_relaxed);
  if (wants & kWantInput) wantIn_.fetch_add(1, std::memory_order_relaxed);
  if (wants & kWantResponse) wantResp_.fetch_add(1, std::memory_order_relaxed);
  if (consumers_.fetch_add(1, std::memory_order_relaxed) == 0) {
    tap_.addReader();
    {
      std::lock_guard lock(mutex_);
      settled_ = false;
    }
  }
  cv_.notify_all();
}

void SpectrumFeed::release(uint32_t wants) {
  if (wants & kWantScope) {
    wantScope_.fetch_sub(1, std::memory_order_relaxed);
  }
  if (wants & kWantSpectrogram) wantGram_.fetch_sub(1, std::memory_order_relaxed);
  if (wants != 0 && (wants & ~(kWantScope | kWantSpectrogram)) == 0) noOutput_.fetch_sub(1, std::memory_order_relaxed);
  if (wants & kWantInput) wantIn_.fetch_sub(1, std::memory_order_relaxed);
  if (wants & kWantResponse) wantResp_.fetch_sub(1, std::memory_order_relaxed);
  if (consumers_.fetch_sub(1, std::memory_order_relaxed) == 1) {
    tap_.removeReader();
    cv_.notify_all();
  }
}

uint32_t SpectrumFeed::wants() const noexcept {
  // The output spectrum for every consumer but a scope-only or spectrogram-only one (TX.scope, TX.spectrogram).
  uint32_t w = consumers_.load(std::memory_order_relaxed) > noOutput_.load(std::memory_order_relaxed) ? kWantOutput : 0u;
  if (wantScope_.load(std::memory_order_relaxed) > 0) w |= kWantScope;
  if (wantGram_.load(std::memory_order_relaxed) > 0) w |= kWantSpectrogram;
  if (tap_.channels() >= 4) {
    if (wantIn_.load(std::memory_order_relaxed) > 0) w |= kWantInput;
    if (wantResp_.load(std::memory_order_relaxed) > 0) w |= kWantResponse;
  }
  return w;
}

void SpectrumFeed::setSourceLive(bool live) {
  if (live_.exchange(live) == live) return;
  {
    std::lock_guard lock(mutex_);
    settled_ = false;
  }
  cv_.notify_all();
}

void SpectrumFeed::addWaker(const void* key, std::function<void()> wake) { wakers_[key] = std::move(wake); }
void SpectrumFeed::removeWaker(const void* key) { wakers_.erase(key); }
void SpectrumFeed::runWakers() {
  // A waker may remove itself (a display that is gone from the tree lets go): run copies.
  std::vector<std::function<void()>> w;
  w.reserve(wakers_.size());
  for (const auto& [k, f] : wakers_) w.push_back(f);
  for (const auto& f : w) f();
}

uint64_t SpectrumFeed::snapshot(uint64_t seq, std::vector<float>& levels, bool& settled) const {
  std::lock_guard lock(mutex_);
  settled = settled_;
  if (seq != seq_) levels.assign(snap_.out.begin(), snap_.out.end());
  return seq_;
}

uint64_t SpectrumFeed::snapshot(uint64_t seq, SpectrumSnapshot& out, bool& settled) const {
  std::lock_guard lock(mutex_);
  settled = settled_;
  if (seq != seq_) {
    out.out = snap_.out;
    out.peaks = snap_.peaks;
    out.in = snap_.in;
    out.response = snap_.response;
    out.confidence = snap_.confidence;
  }
  return seq_;
}

uint64_t SpectrumFeed::scopeEnd(bool& settled) const {
  {
    std::lock_guard lock(mutex_);
    settled = settled_;
  }
  std::lock_guard lock(scopeMutex_);
  return scopeEnd_;
}

uint32_t SpectrumFeed::scopeRead(uint64_t from, uint32_t n, float* const* out) const {
  std::lock_guard lock(scopeMutex_);
  const uint64_t oldest = scopeEnd_ > kScopeFrames ? scopeEnd_ - kScopeFrames : 0;
  // The held part [a, b) of the request, copied in at most two runs per channel; the rest reads "none".
  const uint64_t a = std::clamp(from, oldest, scopeEnd_), b = std::clamp(from + n, oldest, scopeEnd_);
  const uint32_t held = b > a ? static_cast<uint32_t>(b - a) : 0;
  for (uint32_t c = 0; c < scopeChannels(); ++c) {
    if (out[c] == nullptr) continue;
    const float none = c < 2 && tap_.channels() >= 4 ? std::nanf("") : 0.0f;
    const std::vector<float>& h = scope_[c];
    if (h.empty() || held == 0) {
      std::fill(out[c], out[c] + n, none);
      continue;
    }
    const auto lead = static_cast<uint32_t>(a - from);
    std::fill(out[c], out[c] + lead, none);
    const auto pos = static_cast<uint32_t>(a & (kScopeFrames - 1));
    const uint32_t first = std::min(held, kScopeFrames - pos);
    std::copy(h.begin() + pos, h.begin() + pos + first, out[c] + lead);
    std::copy(h.begin(), h.begin() + (held - first), out[c] + lead + first);
    std::fill(out[c] + lead + held, out[c] + n, none);
  }
  return held;
}

void SpectrumFeed::scopeAppend(const float* const* ch, uint32_t n, bool quiet) {
  std::lock_guard lock(scopeMutex_);
  for (uint32_t c = 0; c < scopeChannels(); ++c) {
    std::vector<float>& h = scope_[c];
    if (h.empty()) return;
    const auto pos = static_cast<uint32_t>(scopeEnd_ & (kScopeFrames - 1));
    const uint32_t first = std::min(n, kScopeFrames - pos);
    if (ch[c] == nullptr) {  // no data for this channel (silence appended while nothing arrives): "none"
      const float none = c < 2 && tap_.channels() >= 4 ? std::nanf("") : 0.0f;
      std::fill(h.begin() + pos, h.begin() + pos + first, none);
      std::fill(h.begin(), h.begin() + (n - first), none);
      continue;
    }
    std::copy(ch[c], ch[c] + first, h.begin() + pos);
    std::copy(ch[c] + first, ch[c] + n, h.begin());
  }
  scopeEnd_ += n;
  scopeQuiet_ = quiet ? scopeQuiet_ + n : 0;
}

// ---- TX.spectrogram ----------------------------------------------------------------------------------------------------

struct SpectrumFeed::GramSink final : dsp::Spectrogram::Sink {
  SpectrumFeed& f;
  explicit GramSink(SpectrumFeed& feed) : f(feed) {}
  // The worker, without gramMutex_ (gramPush, configure): into the stage; gramPublish copies it into the ring.
  void column(const float* rows, uint64_t index, double centre) override {
    float peak = 0.0f;
    for (uint32_t r = 0; r < kGramRows; ++r) peak = std::max(peak, rows[r]);
    f.gramStageRows_.insert(f.gramStageRows_.end(), rows, rows + kGramRows);
    f.gramStageIndex_.push_back(index);
    GramColumn& info = f.gramStageInfo_.emplace_back();
    info.centre = centre;
    info.peak = peak;
    // The tapped beat at the column's centre (the analysis stream is the tap's frames, in order).
    const auto at = static_cast<uint64_t>(std::max(0.0, centre));
    const uint64_t written = f.gram_ ? f.gram_->samples() : 0;
    info.beat = !f.gramBeat_.empty() && at < written && written - at < f.gramBeat_.size()
                    ? f.gramBeat_[at & (f.gramBeat_.size() - 1)]
                    : std::nanf("");
    f.gramQuiet_ = peak > 0.0f ? 0 : f.gramQuiet_ + 1;
  }
};

void SpectrumFeed::setSpectrogram(const void* key, const SpectrogramRequest& request) {
  std::lock_guard lock(gramMutex_);
  gramRequests_[key] = request;
  pickGram();
}

void SpectrumFeed::clearSpectrogram(const void* key) {
  std::lock_guard lock(gramMutex_);
  if (gramRequests_.erase(key) > 0 && !gramRequests_.empty()) pickGram();
}

void SpectrumFeed::pickGram() {
  // The request with the most columns decides (ties: the one in force stays, so equal displays never flip it).
  const SpectrogramRequest* best = nullptr;
  for (const auto& [k, r] : gramRequests_) {
    SpectrogramRequest rr = r;
    rr.config.sampleRate = cfg_.sampleRate;
    if (best == nullptr || r.columns > best->columns || (r.columns == best->columns && rr == gramWant_)) best = &r;
  }
  if (best == nullptr) return;
  SpectrogramRequest want = *best;
  want.config.sampleRate = cfg_.sampleRate;
  if (!(want == gramWant_) || !gram_) {
    gramWant_ = want;
    gramDirty_ = true;
  }
}

GramState SpectrumFeed::gramState() const {
  bool settled;
  {
    std::lock_guard lock(mutex_);
    settled = settled_;
  }
  std::lock_guard lock(gramMutex_);
  GramState g = gramPub_;
  g.settled = settled;
  return g;
}

void SpectrumFeed::gramPublish() {
  GramState g;
  g.running = true;
  g.end = gram_->columns();
  g.oldest = g.end > kGramRing ? g.end - kGramRing : 0;
  g.position = gram_->position();
  g.mapping = gram_->mapping();
  g.hop = gram_->hop();
  g.fft = gram_->fftSize();
  g.lowFft = gram_->lowFftSize();
  g.fMin = gram_->fMin();
  g.fMax = gram_->fMax();
  {
    std::lock_guard lock(gramMutex_);
    if (!gramRows_.empty())
      for (size_t i = 0; i < gramStageIndex_.size(); ++i) {
        const auto slot = static_cast<size_t>(gramStageIndex_[i] % kGramRing);
        std::copy_n(gramStageRows_.begin() + static_cast<std::ptrdiff_t>(i * kGramRows), kGramRows,
                    gramRows_.begin() + static_cast<std::ptrdiff_t>(slot * kGramRows));
        gramInfo_[slot] = gramStageInfo_[i];
      }
    gramPub_ = g;
  }
  gramStageRows_.clear();  // (capacity kept: no allocation per push once warm)
  gramStageInfo_.clear();
  gramStageIndex_.clear();
}

uint32_t SpectrumFeed::gramRead(uint64_t from, uint32_t n, float* rows, GramColumn* info) const {
  std::lock_guard lock(gramMutex_);
  if (!gramPub_.running || gramRows_.empty()) return 0;
  const uint64_t end = gramPub_.end, oldest = gramPub_.oldest;
  if (from < oldest || from >= end) return 0;
  const auto k = static_cast<uint32_t>(std::min<uint64_t>(n, end - from));
  for (uint32_t i = 0; i < k; ++i) {
    const auto slot = static_cast<size_t>((from + i) % kGramRing);
    if (rows != nullptr)
      std::copy_n(gramRows_.begin() + static_cast<std::ptrdiff_t>(slot * kGramRows), kGramRows, rows + static_cast<size_t>(i) * kGramRows);
    if (info != nullptr) info[i] = gramInfo_[slot];
  }
  return k;
}

void SpectrumFeed::gramPush(const float* mono, const float* beat, uint32_t n) {
  if (!gram_) return;
  // The beat by analysis-stream sample, for the columns' centres (they trail the newest sample by up to a window and
  // the reach: well inside the ring).
  const uint64_t at = gram_->samples();
  const size_t mask = gramBeat_.size() - 1;
  for (uint32_t i = 0; i < n; ++i) gramBeat_[(at + i) & mask] = beat != nullptr ? beat[i] : std::nanf("");
  GramSink sink(*this);
  gram_->push(mono, n, sink);  // (analysis: no lock)
  gramPublish();
}

void SpectrumFeed::hold() {
  std::lock_guard lock(mutex_);
  settled_ = true;
}

uint32_t SpectrumFeed::step(double now) { return work(now); }

void SpectrumFeed::setTuning(const SpectrumTuning& t) {
  {
    std::lock_guard lock(mutex_);
    if (t == tuning_) return;
    tuning_ = t;
    tuningDirty_ = true;
  }
  cv_.notify_all();
}

SpectrumTuning SpectrumFeed::tuning() const {
  std::lock_guard lock(mutex_);
  return tuning_;
}

uint32_t SpectrumFeed::work(double now) {
  {  // VZ.1: a new tuning remakes the analysers (off the audio thread: the worker's own)
    std::optional<SpectrumTuning> t;
    {
      std::lock_guard lock(mutex_);
      if (tuningDirty_) {
        tuningDirty_ = false;
        t = tuning_;
      }
    }
    if (t) {
      dsp::SpectrumConfig c = cfg_;
      c.fftSize = t->fftSize;
      c.sharpLows = t->sharpLows;
      c.tiltDbPerOct = t->tiltDbPerOct;
      c.smoothingOct = t->smoothingOct;
      c.attackMs = t->attackMs;
      c.releaseMs = t->releaseMs;
      c.holdSeconds = t->holdSeconds;
      c.fallDbPerSecond = t->fallDbPerSecond;
      out_ = std::make_unique<dsp::SpectrumAnalyzer>(c);
      in_ = std::make_unique<dsp::SpectrumAnalyzer>(tap_.channels() >= 4 ? c : axisOnly(c));
      runFft_ = c.fftSize;
    }
  }
  const uint32_t want = wants();
  // A newly wanted analysis starts from silence (what it held is stale).
  if ((want & kWantInput) && !(ranWants_ & kWantInput)) in_->reset();
  if ((want & kWantResponse) && !(ranWants_ & kWantResponse) && transfer_) transfer_->reset();
  ranWants_ = want;
  const bool four = tap_.channels() >= 4;
  const uint32_t oL = four ? 2 : 0, oR = four ? 3 : (tap_.channels() > 1 ? 1 : 0);
  const bool scope = (want & kWantScope) != 0;
  const bool readMonoIn = four && (want & (kWantInput | kWantResponse)) != 0;
  const bool gram = (want & kWantSpectrogram) != 0;
  // A scope keeps every channel of the tap; a spectrogram reads the beat in channel 0 (TX.spectrogram).
  const bool readIn = readMonoIn || (four && (scope || gram));
  if ((want & kWantResponse) && four && !transfer_) {  // made when first wanted (a scope's feed never needs it)
    dsp::TransferConfig tc;
    tc.sampleRate = cfg_.sampleRate;
    std::vector<float> hz(cfg_.points);
    for (uint32_t p = 0; p < cfg_.points; ++p) hz[p] = std::max(axis_.pointHz(p), 1.0f);
    transfer_ = std::make_unique<dsp::TransferAnalyzer>(tc, hz);
  }
  if (gram) {  // TX.spectrogram: a new request (re)configures the analyser; the column sequence continues
    std::optional<SpectrogramRequest> request;
    {
      std::lock_guard lock(gramMutex_);
      if (gramDirty_ || !gram_) {
        gramDirty_ = false;
        request = gramWant_;
      }
    }
    if (request) {  // (outside the lock: the worker's analyser)
      gramChannel_ = request->channel;
      gramCols_ = std::max<uint32_t>(1, request->columns);
      dsp::SpectrogramConfig c = request->config;
      c.sampleRate = cfg_.sampleRate;
      if (!gram_) {
        gram_ = std::make_unique<dsp::Spectrogram>(c);
        gramBeat_.assign(size_t{1} << 16, std::nanf(""));
        gramIn_.resize(std::max<size_t>(kChunk, mono_.size()));
      } else {
        GramSink sink(*this);
        gram_->configure(c, sink);
      }
      gramPublish();
    }
  }
  bool wasSettled;
  {
    std::lock_guard lock(mutex_);
    wasSettled = settled_;
  }
  uint32_t ran = 0;
  bool data = false, sound = false;
  for (int guard = 0; guard < 64; ++guard) {
    const uint64_t w = tap_.written();
    const uint64_t oldest = tap_.oldestReadable();
    if (pos_ < oldest || pos_ > w) pos_ = std::max(oldest, w > runFft_ * 4u ? w - runFft_ * 4u : 0);  // resync
    if (pos_ >= w) break;
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(w - pos_, kChunk));
    uint32_t k = tap_.read(oL, pos_, n, ch_[oL].data());
    if (oR != oL) k = std::min(k, tap_.read(oR, pos_, n, ch_[oR].data()));
    if (readIn) k = std::min({k, tap_.read(0, pos_, n, ch_[0].data()), tap_.read(1, pos_, n, ch_[1].data())});
    if (k == 0) {  // overwritten while reading: start again from what is still there
      pos_ = tap_.oldestReadable();
      continue;
    }
    for (uint32_t i = 0; i < k; ++i) mono_[i] = 0.5f * (ch_[oL][i] + ch_[oR][i]);
    if (readMonoIn)
      for (uint32_t i = 0; i < k; ++i) monoIn_[i] = 0.5f * (ch_[0][i] + ch_[1][i]);
    // (A scope's channels 0 and 1 are a clock and a pitch, not a signal: they never count as sound.)
    // A scope shows L, R and side too: silence there is both channels silent (a pure side signal's mono is 0).
    const bool quiet = (scope || gram ? silent(ch_[oL].data(), k) && silent(ch_[oR].data(), k) : silent(mono_.data(), k)) &&
                       (!readMonoIn || silent(monoIn_.data(), k));
    pos_ += k;
    data = true;
    sound = sound || !quiet;
    // Settled and silent: nothing to show, nothing to compute (the analysers already rest at the floor).
    if (quiet && wasSettled && !sound) continue;
    if (scope) {
      const float* sc[4] = {ch_[oL].data(), ch_[oR].data(), nullptr, nullptr};
      if (four) {
        sc[0] = ch_[0].data();
        sc[1] = ch_[1].data();
        sc[2] = ch_[2].data();
        sc[3] = ch_[3].data();
      }
      scopeAppend(sc, k, quiet);
    }
    if (gram) {
      const float* src = mono_.data();  // mid
      if (gramChannel_ == 1) src = ch_[oL].data();
      else if (gramChannel_ == 2) src = ch_[oR].data();
      else if (gramChannel_ == 3) {
        for (uint32_t i = 0; i < k; ++i) gramIn_[i] = 0.5f * (ch_[oL][i] - ch_[oR][i]);
        src = gramIn_.data();
      }
      gramPush(src, four ? ch_[0].data() : nullptr, k);
    }
    if (want & kWantOutput) ran += out_->push(mono_.data(), k);
    if (want & kWantInput) in_->push(monoIn_.data(), k);
    if ((want & kWantResponse) && transfer_) transfer_->push(monoIn_.data(), mono_.data(), k);
  }
  if (data) {
    lastData_ = now;
    lastSilence_ = now;
    decaying_ = false;
  } else if (now - lastData_ > kNoDataSeconds &&
             (((want & kWantOutput) && !out_->allBelow(kSettledDb)) ||
              (scope && static_cast<double>(scopeQuiet_) < kScopeRestSeconds * cfg_.sampleRate) ||
              (gram && gramQuiet_ < gramCols_))) {
    // No input: silence paced in real time, so the release falls as it would on a silent signal.
    if (!decaying_) {
      decaying_ = true;
      lastSilence_ = now;
    }
    const double due = (now - lastSilence_) * cfg_.sampleRate;
    const uint32_t frames = static_cast<uint32_t>(std::min<double>(due, static_cast<double>(mono_.size())));
    if (frames >= cfg_.hop) {
      std::fill(mono_.begin(), mono_.begin() + frames, 0.0f);
      if (scope) {  // the trace flattens (no clock, no note: nothing played)
        const float* sc[4] = {mono_.data(), mono_.data(), mono_.data(), mono_.data()};
        if (four) sc[0] = sc[1] = nullptr;
        scopeAppend(sc, frames, true);
      }
      if (gram) gramPush(mono_.data(), nullptr, frames);  // the picture scrolls on in time (no clock: no beat)
      if (want & kWantOutput) ran += out_->push(mono_.data(), frames);
      if (want & kWantInput) in_->push(mono_.data(), frames);
      if ((want & kWantResponse) && transfer_) {
        if (monoIn_.size() >= frames) std::fill(monoIn_.begin(), monoIn_.begin() + frames, 0.0f);
        transfer_->push(mono_.data(), mono_.data(), frames);
      }
      lastSilence_ += static_cast<double>(frames) / cfg_.sampleRate;
    }
  }
  // Settled: everything shown rests at the floor and nothing that sounds arrived (silent data may still flow).
  const bool rest = (!(want & kWantOutput) || out_->allBelow(kSettledDb)) &&
                    (!(want & kWantInput) || in_->allBelow(kSettledDb)) &&
                    (!(want & kWantResponse) || !transfer_ || !transfer_->anyConfident()) &&
                    (!scope || static_cast<double>(scopeQuiet_) >= kScopeRestSeconds * cfg_.sampleRate) &&
                    (!gram || gramQuiet_ >= gramCols_);
  const bool settledNow = !sound && rest && (data || now - lastData_ > kNoDataSeconds);
  bool post = false;
  if (ran > 0 || settledNow != wasSettled) {
    hops_.fetch_add(ran, std::memory_order_relaxed);
    std::lock_guard lock(mutex_);
    if (ran > 0) {
      const auto lv = out_->levelsDb();
      snap_.out.assign(lv.begin(), lv.end());
      const auto pk = out_->peaksDb();
      snap_.peaks.assign(pk.begin(), pk.end());
      if (want & kWantInput) {
        const auto li = in_->levelsDb();
        snap_.in.assign(li.begin(), li.end());
      } else {
        snap_.in.clear();
      }
      if ((want & kWantResponse) && transfer_) {
        snap_.response.assign(transfer_->magnitudeDb().begin(), transfer_->magnitudeDb().end());
        snap_.confidence.assign(transfer_->confidence().begin(), transfer_->confidence().end());
      } else {
        snap_.response.clear();
        snap_.confidence.clear();
      }
      ++seq_;
    }
    settled_ = settledNow;
    post = wasSettled && !settledNow && sound;
  }
  if (post && watch_.load(std::memory_order_relaxed) && wakePost_) {
    posts_.fetch_add(1, std::memory_order_relaxed);
    wakePost_();
  }
  return ran;
}

void SpectrumFeed::run() {
  std::unique_lock lock(mutex_);
  while (!quit_) {
    // Hidden, or nothing more to show: block (no periodic work at all). A watched feed keeps looking (slowly).
    cv_.wait(lock, [&] {
      return quit_ || (consumers_.load(std::memory_order_relaxed) > 0 &&
                       (!settled_ || live_.load(std::memory_order_relaxed) || watch_.load(std::memory_order_relaxed)));
    });
    if (quit_) break;
    wakeups_.fetch_add(1, std::memory_order_relaxed);
    lock.unlock();
    const double now = clockNow();
    if (lastData_ == 0.0) lastData_ = now;
    work(now);
    lock.lock();
    const bool slow = settled_ && !live_.load(std::memory_order_relaxed);
    cv_.wait_for(lock, std::chrono::duration<double>(slow ? kWatchSeconds : kFrameSeconds), [&] { return quit_; });
  }
}

// ---- AnalysisHub ------------------------------------------------------------------------------------------------------

AnalysisHub::AnalysisHub(TapOf tapOf, double sampleRate, Poster post)
    : tapOf_(std::move(tapOf)), rate_(sampleRate), post_(std::move(post)) {}

AnalysisHub::~AnalysisHub() = default;

void AnalysisHub::adopt(doc::DeviceId device, std::shared_ptr<TapRing> tap, std::shared_ptr<SpectrumFeed> feed) {
  feeds_[device.value] = Entry{std::move(tap), std::move(feed)};
}

void AnalysisHub::setSource(TapOf tapOf, double sampleRate) {
  tapOf_ = std::move(tapOf);
  rate_ = sampleRate;
  for (auto& [id, e] : feeds_)
    if (e.feed && e.feed->consumers() > 0) retired_.push_back(std::move(e.feed));
  feeds_.clear();
  for (auto& [id, e] : pathFeeds_)
    if (e.feed && e.feed->consumers() > 0) retired_.push_back(std::move(e.feed));
  pathFeeds_.clear();
}

SpectrumFeed* AnalysisHub::feed(doc::DeviceId device, std::string_view path) {
  std::shared_ptr<TapRing> tap = pathTapOf_ ? pathTapOf_(device, path) : nullptr;
  const auto it = pathFeeds_.find(std::make_pair(device.value, std::string(path)));
  if (it != pathFeeds_.end() && it->second.tap == tap) return it->second.feed.get();
  if (it != pathFeeds_.end()) {
    if (it->second.feed && it->second.feed->consumers() > 0) retired_.push_back(std::move(it->second.feed));
    pathFeeds_.erase(it);
  }
  if (!tap) return nullptr;
  Entry& e = pathFeeds_[std::make_pair(device.value, std::string(path))];
  return feedOn(nullptr, std::move(tap), e);
}

SpectrumFeed* AnalysisHub::feedOn(Entry* /*found*/, std::shared_ptr<TapRing> tap, Entry& slot) {
  std::erase_if(retired_, [](const std::shared_ptr<SpectrumFeed>& f) { return f->consumers() == 0; });
  auto f = std::make_shared<SpectrumFeed>(tap, config(rate_));
  f->setWatch(true);
  std::weak_ptr<SpectrumFeed> weak = f;
  if (post_)
    f->setWakePost([post = post_, weak] {
      post([weak] {
        if (std::shared_ptr<SpectrumFeed> s = weak.lock()) s->runWakers();
      });
    });
  SpectrumFeed* raw = f.get();
  slot = Entry{std::move(tap), std::move(f)};
  return raw;
}

dsp::SpectrumConfig AnalysisHub::config(double sampleRate) {
  dsp::SpectrumConfig c;
  c.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
  c.peakHold = true;  // the displays draw it when asked; computing it is a compare per point per hop
  return c;
}

SpectrumFeed* AnalysisHub::feed(doc::DeviceId device) {
  std::shared_ptr<TapRing> tap = tapOf_ ? tapOf_(device) : nullptr;
  auto it = feeds_.find(device.value);
  if (it != feeds_.end() && it->second.tap == tap) return it->second.feed.get();
  // Superseded (or gone): keep the old feed until no display holds it.
  if (it != feeds_.end()) {
    if (it->second.feed && it->second.feed->consumers() > 0) retired_.push_back(std::move(it->second.feed));
    feeds_.erase(it);
  }
  std::erase_if(retired_, [](const std::shared_ptr<SpectrumFeed>& f) { return f->consumers() == 0; });
  if (!tap) return nullptr;
  auto f = std::make_shared<SpectrumFeed>(tap, config(rate_));
  f->setWatch(true);
  std::weak_ptr<SpectrumFeed> weak = f;
  if (post_)
    f->setWakePost([post = post_, weak] {
      post([weak] {
        if (std::shared_ptr<SpectrumFeed> s = weak.lock()) s->runWakers();
      });
    });
  SpectrumFeed* raw = f.get();
  feeds_[device.value] = Entry{std::move(tap), std::move(f)};
  return raw;
}

}  // namespace pq::views::devices
