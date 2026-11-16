// SpectrumFeed and AnalysisHub (pq/views/devices/analysis.h; T4.6, TX.spectrum).

#include "pq/views/devices/analysis.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <chrono>
#include <cmath>

#include "pq/core/denormal_guard.h"

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
  static std::atomic<uint64_t> serials{0};
  serial_ = serials.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t nch = std::min<uint32_t>(4, tap_.channels());
  for (uint32_t c = 0; c < nch; ++c) ch_[c].resize(kChunk);
  mono_.resize(std::max<uint32_t>(kChunk, cfg_.hop * 32));
  if (tap_.channels() >= 4) monoIn_.resize(mono_.size());  // the transfer analyser: made when first wanted (work)
  snap_.axis = out_->axis();
  snap_.out.assign(cfg_.points, cfg_.floorDb);
  snap_.peaks.assign(cfg_.points, cfg_.floorDb);
  tuning_ = {config.fftSize, config.sharpLows, config.tiltDbPerOct, config.smoothingOct, config.attackMs, config.releaseMs,
             config.holdSeconds, config.fallDbPerSecond, config.scale, config.fMin, config.fMax, config.average,
             config.averageSeconds};
  ranTuning_ = tuning_;
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
  delete bands_.load();
  delete wave_.load();
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
  if (wants & kWantLoudness) {  // VZ.4
    {
      std::lock_guard lock(loudMutex_);
      if (loudRing_.empty()) loudRing_.assign(kLoudRing, dsp::LoudnessPoint{});  // allocated by the first loudness consumer
    }
    wantLoud_.fetch_add(1, std::memory_order_relaxed);
  }
  if (wants & kWantStereo) wantStereo_.fetch_add(1, std::memory_order_relaxed);  // VZ.5
  // A consumer of the history, the columns, the loudness or the cross-spectrum only: no spectrum is computed for it.
  if (wants & kWantBands) {  // VZ.6: the stages exist before the worker can see them wanted
    makeBands();
    wantBands_.fetch_add(1, std::memory_order_relaxed);
  }
  if (wants & kWantWave) {
    makeWave();
    wantWave_.fetch_add(1, std::memory_order_relaxed);
  }
  if (wants & (kWantWaterfall | kWantCloud)) {  // VZ.7: the published rings, allocated by the first consumer
    std::lock_guard lock(wfMutex_);
    if ((wants & kWantWaterfall) && wfRing_.empty()) {
      wfRing_.assign(static_cast<size_t>(dsp::WaterfallAnalyzer::kMaxSlices) * dsp::WaterfallAnalyzer::kPoints, -140.0f);
      wfLive_.assign(dsp::WaterfallAnalyzer::kPoints, -140.0f);
    }
    if ((wants & kWantCloud) && clRing_.empty()) {
      clRing_.assign(kCloudRing, dsp::CloudPoint{});
      clSliceRing_.assign(kCloudSlices, dsp::StereoCloud::RibSlice{});
    }
  }
  if (wants & kWantWaterfall) wantWf_.fetch_add(1, std::memory_order_relaxed);
  if (wants & kWantCloud) wantCloud_.fetch_add(1, std::memory_order_relaxed);
  // A consumer of the history, the columns, the loudness, the cross-spectrum, the waterfall or the cloud only: no spectrum
  // is computed for it.
  constexpr uint32_t kOwnWork = kWantScope | kWantSpectrogram | kWantLoudness | kWantStereo | kWantBands | kWantWave | kWantWaterfall | kWantCloud;
  if (wants != 0 && (wants & ~kOwnWork) == 0) noOutput_.fetch_add(1, std::memory_order_relaxed);
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
  if (wants & kWantLoudness) wantLoud_.fetch_sub(1, std::memory_order_relaxed);
  if (wants & kWantStereo) wantStereo_.fetch_sub(1, std::memory_order_relaxed);
  if (wants & kWantBands) wantBands_.fetch_sub(1, std::memory_order_relaxed);  // VZ.6
  if (wants & kWantWave) wantWave_.fetch_sub(1, std::memory_order_relaxed);
  if (wants & kWantWaterfall) wantWf_.fetch_sub(1, std::memory_order_relaxed);
  if (wants & kWantCloud) wantCloud_.fetch_sub(1, std::memory_order_relaxed);
  constexpr uint32_t kOwnWork = kWantScope | kWantSpectrogram | kWantLoudness | kWantStereo | kWantBands | kWantWave | kWantWaterfall | kWantCloud;
  if (wants != 0 && (wants & ~kOwnWork) == 0) noOutput_.fetch_sub(1, std::memory_order_relaxed);
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
  if (wantLoud_.load(std::memory_order_relaxed) > 0) w |= kWantLoudness;
  if (wantStereo_.load(std::memory_order_relaxed) > 0) w |= kWantStereo;
  if (wantBands_.load(std::memory_order_acquire) > 0) w |= kWantBands;  // VZ.6 (acquire: the stage made before the count)
  if (wantWave_.load(std::memory_order_acquire) > 0) w |= kWantWave;
  if (wantWf_.load(std::memory_order_relaxed) > 0) w |= kWantWaterfall;
  if (wantCloud_.load(std::memory_order_relaxed) > 0) w |= kWantCloud;
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
    out.axis = snap_.axis;
    out.out = snap_.out;
    out.peaks = snap_.peaks;
    out.average = snap_.average;
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
  // (Against the last request, never the worker's stage: gram_ is the worker's, written outside the lock. Until the
  // stage is made the worker takes gramWant_ anyway.)
  if (!(want == gramWant_) || !gramAsked_) {
    gramWant_ = want;
    gramDirty_ = true;
    gramAsked_ = true;
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

// ---- VZ.4: the loudness ----------------------------------------------------------------------------------------------

dsp::LoudnessConfig SpectrumFeed::loudnessRequest() const {
  std::lock_guard lock(loudMutex_);
  return loudWant_;
}

void SpectrumFeed::setLoudness(const dsp::LoudnessConfig& c) {
  {
    std::lock_guard lock(loudMutex_);
    dsp::LoudnessConfig want = c;
    want.sampleRate = cfg_.sampleRate;
    if (want == loudWant_) return;
    loudWant_ = want;
    loudDirty_ = true;
  }
  {
    std::lock_guard lock(mutex_);
    settled_ = false;  // the worker applies it (and publishes) at its next step
  }
  cv_.notify_all();
}

void SpectrumFeed::resetLoudness() {
  {
    std::lock_guard lock(loudMutex_);
    loudResets_ |= 1u;
  }
  {
    std::lock_guard lock(mutex_);
    settled_ = false;
  }
  cv_.notify_all();
}

void SpectrumFeed::resetClips() {
  {
    std::lock_guard lock(loudMutex_);
    loudResets_ |= 2u;
  }
  {
    std::lock_guard lock(mutex_);
    settled_ = false;
  }
  cv_.notify_all();
}

uint64_t SpectrumFeed::loudnessRead(dsp::LoudnessReadings& r, bool& settled, uint64_t from, uint32_t n, dsp::LoudnessPoint* out,
                                    uint32_t* copied) const {
  {
    std::lock_guard lock(mutex_);
    settled = settled_;
  }
  std::lock_guard lock(loudMutex_);
  r = loudPub_;
  uint32_t k = 0;
  if (out != nullptr && !loudRing_.empty()) {
    const uint64_t end = loudPub_.points, oldest = end > kLoudRing ? end - kLoudRing : 0;
    if (from >= oldest && from < end) {
      k = static_cast<uint32_t>(std::min<uint64_t>(n, end - from));
      for (uint32_t i = 0; i < k; ++i) out[i] = loudRing_[static_cast<size_t>((from + i) % kLoudRing)];
    }
  }
  if (copied != nullptr) *copied = k;
  return loudSeq_;
}

// ---- VZ.6: Band Levels and the Waveform's capture ---------------------------------------------------------------------

void SpectrumFeed::makeBands() {
  if (bands_.load(std::memory_order_acquire) != nullptr) return;
  dsp::BandLevelsConfig c;
  c.sampleRate = cfg_.sampleRate;
  bands_.store(new dsp::BandLevels(c), std::memory_order_release);
}

void SpectrumFeed::makeWave() {
  if (wave_.load(std::memory_order_acquire) != nullptr) return;
  dsp::WaveConfig c;
  c.sampleRate = cfg_.sampleRate;
  wave_.store(new dsp::WaveCapture(c), std::memory_order_release);
}

void SpectrumFeed::setBands(const dsp::BandLevelsConfig& c) {
  makeBands();
  dsp::BandLevelsConfig want = c;
  want.sampleRate = cfg_.sampleRate;
  dsp::BandLevels* b = bands_.load(std::memory_order_acquire);
  if (b->config() == want) return;
  b->configure(want);
  {
    std::lock_guard lock(mutex_);
    settled_ = false;  // the worker shows it at its next step
  }
  cv_.notify_all();
}

void SpectrumFeed::setWave(const dsp::WaveConfig& c, float restSeconds) {
  makeWave();
  dsp::WaveConfig want = c;
  want.sampleRate = cfg_.sampleRate;
  wave_.load(std::memory_order_acquire)->configure(want);
  waveRestFrames_.store(static_cast<uint64_t>(std::max(0.5f, restSeconds) * cfg_.sampleRate), std::memory_order_relaxed);
}

bool SpectrumFeed::settled() const {
  std::lock_guard lock(mutex_);
  return settled_;
}

void SpectrumFeed::loudPush(const float* l, const float* r, uint32_t n) {
  if (loud_) loud_->push(l, r, n);  // (analysis: no lock)
}

void SpectrumFeed::loudPublish() {
  const dsp::LoudnessReadings r = loud_->readings();
  loudStage_.clear();  // (capacity kLoudRing: no allocation once warm)
  const uint64_t end = r.points;
  uint64_t from = std::max(loudCopied_, end > kLoudRing ? end - kLoudRing : 0);
  if (end > from) {
    loudStage_.resize(static_cast<size_t>(end - from));
    (void)loud_->history(from, static_cast<uint32_t>(end - from), loudStage_.data());
  }
  {
    std::lock_guard lock(loudMutex_);
    if (!loudRing_.empty())
      for (size_t i = 0; i < loudStage_.size(); ++i) loudRing_[static_cast<size_t>((from + i) % kLoudRing)] = loudStage_[i];
    loudPub_ = r;
    ++loudSeq_;
  }
  loudCopied_ = end;
}

// ---- VZ.5: the cross-spectrum ----------------------------------------------------------------------------------------

void SpectrumFeed::setStereo(const dsp::CrossSpectrumConfig& c) {
  {
    std::lock_guard lock(stereoMutex_);
    dsp::CrossSpectrumConfig want = c;
    want.sampleRate = cfg_.sampleRate;
    if (want == stereoWant_ && stereoAsked_) return;  // (stereo_ is the worker's: never read here)
    stereoWant_ = want;
    stereoDirty_ = stereoAsked_ = true;
  }
  {
    std::lock_guard lock(mutex_);
    settled_ = false;  // the worker applies it at its next step
  }
  cv_.notify_all();
}

uint64_t SpectrumFeed::stereoRead(uint64_t seq, StereoSnapshot& out, bool& settled) const {
  {
    std::lock_guard lock(mutex_);
    settled = settled_;
  }
  std::lock_guard lock(stereoMutex_);
  if (seq != stereoSeq_) {
    out.config = stereoPub_.config;
    out.points.assign(stereoPub_.points.begin(), stereoPub_.points.end());  // (capacity kept: no allocation once warm)
    out.heat.assign(stereoPub_.heat.begin(), stereoPub_.heat.end());
    out.particles.assign(stereoPub_.particles.begin(), stereoPub_.particles.end());
    out.frames = stereoPub_.frames;
  }
  return stereoSeq_;
}

void SpectrumFeed::stereoPublish() {
  std::lock_guard lock(stereoMutex_);
  stereoPub_.config = stereo_->config();
  stereoPub_.points.assign(stereo_->points().begin(), stereo_->points().end());
  stereoPub_.heat.assign(stereo_->heat().begin(), stereo_->heat().end());
  stereoPub_.particles.assign(stereo_->particles().begin(), stereo_->particles().end());
  stereoPub_.frames = stereo_->frames();
  ++stereoSeq_;
}

// ---- VZ.7: the waterfall and the stereo cloud ------------------------------------------------------------------------

void SpectrumFeed::setWaterfall(const dsp::WaterfallConfig& c) {
  {
    std::lock_guard lock(wfMutex_);
    dsp::WaterfallConfig want = c;
    want.sampleRate = cfg_.sampleRate;
    if (want == wfWant_ && wfAsked_) return;  // (wf_ is the worker's: never read here)
    wfWant_ = want;
    wfDirty_ = wfAsked_ = true;
  }
  {
    std::lock_guard lock(mutex_);
    settled_ = false;
  }
  cv_.notify_all();
}

void SpectrumFeed::setCloud(const dsp::StereoCloudConfig& c) {
  {
    std::lock_guard lock(wfMutex_);
    dsp::StereoCloudConfig want = c;
    want.sampleRate = cfg_.sampleRate;
    if (want == clWant_ && clAsked_) return;  // (cloud_ is the worker's: never read here)
    clWant_ = want;
    clDirty_ = clAsked_ = true;
  }
  {
    std::lock_guard lock(mutex_);
    settled_ = false;
  }
  cv_.notify_all();
}

uint64_t SpectrumFeed::waterfallRead(uint64_t seq, WaterfallState& st, std::vector<float>& live, bool& settled) const {
  {
    std::lock_guard lock(mutex_);
    settled = settled_;
  }
  std::lock_guard lock(wfMutex_);
  if (seq != wfSeq_) {
    st = wfPub_;
    live.assign(wfLive_.begin(), wfLive_.end());
  }
  return wfSeq_;
}

uint32_t SpectrumFeed::waterfallSlices(uint64_t from, uint32_t n, float* out) const {
  constexpr uint32_t P = dsp::WaterfallAnalyzer::kPoints, M = dsp::WaterfallAnalyzer::kMaxSlices;
  std::lock_guard lock(wfMutex_);
  const uint64_t end = wfPub_.slices, oldest = end > M ? end - M : 0;
  if (wfRing_.empty() || from < oldest || from >= end) return 0;
  const auto k = static_cast<uint32_t>(std::min<uint64_t>(n, end - from));
  for (uint32_t i = 0; i < k; ++i)
    std::copy_n(wfRing_.begin() + static_cast<std::ptrdiff_t>(((from + i) % M) * P), P, out + static_cast<size_t>(i) * P);
  return k;
}

uint64_t SpectrumFeed::cloudRead(uint64_t seq, CloudState& st, bool& settled) const {
  {
    std::lock_guard lock(mutex_);
    settled = settled_;
  }
  std::lock_guard lock(wfMutex_);
  if (seq != clSeq_) st = clPub_;
  return clSeq_;
}

uint32_t SpectrumFeed::cloudPoints(uint64_t from, uint32_t n, dsp::CloudPoint* out) const {
  std::lock_guard lock(wfMutex_);
  const uint64_t end = clPub_.points, oldest = end > kCloudRing ? end - kCloudRing : 0;
  if (clRing_.empty() || from < oldest || from >= end) return 0;
  const auto k = static_cast<uint32_t>(std::min<uint64_t>(n, end - from));
  for (uint32_t i = 0; i < k; ++i) out[i] = clRing_[static_cast<size_t>((from + i) % kCloudRing)];
  return k;
}

uint32_t SpectrumFeed::cloudSlices(uint64_t from, uint32_t n, dsp::StereoCloud::RibSlice* out) const {
  std::lock_guard lock(wfMutex_);
  const uint64_t end = clPub_.slices, oldest = end > kCloudSlices ? end - kCloudSlices : 0;
  if (clSliceRing_.empty() || from < oldest || from >= end) return 0;
  const auto k = static_cast<uint32_t>(std::min<uint64_t>(n, end - from));
  for (uint32_t i = 0; i < k; ++i) out[i] = clSliceRing_[static_cast<size_t>((from + i) % kCloudSlices)];
  return k;
}

void SpectrumFeed::wfPublish() {
  constexpr uint32_t P = dsp::WaterfallAnalyzer::kPoints, M = dsp::WaterfallAnalyzer::kMaxSlices;
  std::lock_guard lock(wfMutex_);
  const uint64_t end = wf_->slices();
  if (wfPub_.generation != wf_->generation()) wfCopied_ = 0;
  for (uint64_t s = std::max(wfCopied_, end > M ? end - M : 0); s < end; ++s)
    if (const float* src = wf_->slice(s)) std::copy_n(src, P, wfRing_.begin() + static_cast<std::ptrdiff_t>((s % M) * P));
  wfCopied_ = end;
  const auto live = wf_->live();
  std::copy(live.begin(), live.end(), wfLive_.begin());
  wfPub_.config = wf_->config();
  wfPub_.slices = end;
  wfPub_.generation = wf_->generation();
  wfPub_.pitch = wf_->slicePitch();
  wfPub_.phase = wf_->phase();
  ++wfSeq_;
}

void SpectrumFeed::clPublish() {
  std::lock_guard lock(wfMutex_);
  if (clPub_.generation != cloud_->generation()) {
    clPub_.points = clPub_.slices = 0;  // a restart: the old cloud is gone (the face clears on the generation)
  }
  for (const dsp::CloudPoint& p : clStage_) clRing_[static_cast<size_t>(clPub_.points++ % kCloudRing)] = p;
  for (const auto& s : clStageSlices_) clSliceRing_[static_cast<size_t>(clPub_.slices++ % kCloudSlices)] = s;
  clPub_.config = cloud_->config();
  clPub_.head = cloud_->head();
  clPub_.generation = cloud_->generation();
  clPub_.hop = cloud_->hop();
  ++clSeq_;
  clStage_.clear();  // (capacity kept)
  clStageSlices_.clear();
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
    if (t && t->structural(ranTuning_)) {
      dsp::SpectrumConfig c = cfg_;
      c.fftSize = t->fftSize;
      c.sharpLows = t->sharpLows;
      c.tiltDbPerOct = t->tiltDbPerOct;
      c.smoothingOct = t->smoothingOct;
      c.attackMs = t->attackMs;
      c.releaseMs = t->releaseMs;
      c.holdSeconds = t->holdSeconds;
      c.fallDbPerSecond = t->fallDbPerSecond;
      c.scale = t->scale;
      c.fMin = t->fMin;
      c.fMax = t->fMax;
      c.average = t->average;
      c.averageSeconds = t->averageSeconds;
      out_ = std::make_unique<dsp::SpectrumAnalyzer>(c);
      in_ = std::make_unique<dsp::SpectrumAnalyzer>(tap_.channels() >= 4 ? c : axisOnly(c));
      transfer_.reset();  // its points were the old axis' (made again when wanted)
      runFft_ = c.fftSize;
      // The displays see the new axis at once (from silence), not the old picture until the next hop.
      std::lock_guard lock(mutex_);
      snap_.axis = out_->axis();
      snap_.out.assign(c.points, c.floorDb);
      snap_.peaks.assign(c.points, c.floorDb);
      snap_.in.clear();
      snap_.average.clear();
      snap_.response.clear();
      snap_.confidence.clear();
      ++seq_;
    } else if (t) {
      dsp::SpectrumLive l;
      l.attackMs = t->attackMs;
      l.releaseMs = t->releaseMs;
      l.peakHold = out_->config().peakHold;
      l.holdSeconds = t->holdSeconds;
      l.fallDbPerSecond = t->fallDbPerSecond;
      l.average = t->average;
      l.averageSeconds = t->averageSeconds;
      out_->setLive(l);
      l.average = false;
      in_->setLive(l);
    }
    if (t) ranTuning_ = *t;
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
  // VZ.6: the Waveform's loop reads the beat (channel 0) too.
  dsp::BandLevels* bands = (want & kWantBands) != 0 ? bands_.load(std::memory_order_acquire) : nullptr;
  dsp::WaveCapture* wave = (want & kWantWave) != 0 ? wave_.load(std::memory_order_acquire) : nullptr;
  const bool readIn = readMonoIn || (four && (scope || gram || wave != nullptr));
  if ((want & kWantResponse) && four && !transfer_) {  // made when first wanted (a scope's feed never needs it)
    dsp::TransferConfig tc;
    tc.sampleRate = cfg_.sampleRate;
    std::vector<float> hz(cfg_.points);
    for (uint32_t p = 0; p < cfg_.points; ++p) hz[p] = std::max(out_->pointHz(p), 1.0f);
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
  const bool loud = (want & kWantLoudness) != 0;
  bool loudPublishNow = false;
  if (loud) {  // VZ.4: a new request (re)configures the meter (the measurement continues); resets apply here
    std::optional<dsp::LoudnessConfig> config;
    uint32_t resets = 0;
    {
      std::lock_guard lock(loudMutex_);
      if (loudDirty_ || !loud_) {
        loudDirty_ = false;
        loudWant_.sampleRate = cfg_.sampleRate;
        config = loudWant_;
      }
      resets = std::exchange(loudResets_, 0u);
    }
    if (config) {  // (outside the lock: the worker's meter)
      if (!loud_) {
        loud_ = std::make_unique<dsp::LoudnessMeter>(*config);
        loudStage_.reserve(kLoudRing);
      } else {
        loud_->configure(*config);
      }
    }
    if (resets & 1u) loud_->resetMeasurement();
    if (resets & 2u) loud_->resetClips();
    loudPublishNow = config.has_value() || resets != 0;
  }
  const bool wfOn = (want & kWantWaterfall) != 0, clOn = (want & kWantCloud) != 0;
  bool wfPublishNow = false, clPublishNow = false;
  if (wfOn || clOn) {  // VZ.7: new requests (re)configure the analysers (worker-owned; made outside the lock)
    std::optional<dsp::WaterfallConfig> wc;
    std::optional<dsp::StereoCloudConfig> cc;
    {
      std::lock_guard lock(wfMutex_);
      if (wfOn && (wfDirty_ || !wf_)) {
        wfDirty_ = false;
        wfWant_.sampleRate = cfg_.sampleRate;
        wc = wfWant_;
      }
      if (clOn && (clDirty_ || !cloud_)) {
        clDirty_ = false;
        clWant_.sampleRate = cfg_.sampleRate;
        cc = clWant_;
      }
    }
    if (wc) {
      if (!wf_) wf_ = std::make_unique<dsp::WaterfallAnalyzer>(*wc);
      else wf_->configure(*wc);
      wfPublishNow = true;
    }
    if (cc) {
      if (!cloud_) {
        cloud_ = std::make_unique<dsp::StereoCloud>(*cc);
        clStage_.reserve(kCloudRing / 2);
        clStageSlices_.reserve(kCloudSlices);
      } else {
        cloud_->configure(*cc);
      }
      clPublishNow = true;
    }
  }
  auto cloudPush = [&](const float* l, const float* r, uint32_t n) {
    cloud_->push(
        l, r, n,
        [&](const dsp::CloudPoint* p, uint32_t k) {
          // Full (a long stall before the next publish): the oldest quarter goes, the newest stay (in place, no
          // allocation; a quarter at a time so a stall moves the stage a few times, not at every hop).
          const size_t cap = clStage_.capacity(), take = std::min<size_t>(k, cap);
          if (clStage_.size() + take > cap) {
            const size_t drop = std::min(clStage_.size(), std::max(clStage_.size() + take - cap, cap / 4));
            clStage_.erase(clStage_.begin(), clStage_.begin() + static_cast<std::ptrdiff_t>(drop));
          }
          clStage_.insert(clStage_.end(), p + (k - take), p + k);
        },
        [&](const dsp::StereoCloud::RibSlice& s) {
          if (clStageSlices_.size() == clStageSlices_.capacity())
            clStageSlices_.erase(clStageSlices_.begin(), clStageSlices_.begin() + static_cast<std::ptrdiff_t>(clStageSlices_.size() / 4));
          clStageSlices_.push_back(s);
        });
    clPublishNow = true;
  };
  const bool stereo = (want & kWantStereo) != 0;
  bool stereoPublishNow = false;
  if (stereo) {  // VZ.5: a new request (re)configures the analyser (worker-owned; made outside the lock)
    std::optional<dsp::CrossSpectrumConfig> config;
    {
      std::lock_guard lock(stereoMutex_);
      if (stereoDirty_ || !stereo_) {
        stereoDirty_ = false;
        stereoWant_.sampleRate = cfg_.sampleRate;
        config = stereoWant_;
      }
    }
    if (config) {
      if (!stereo_) stereo_ = std::make_unique<dsp::CrossSpectrum>(*config);
      else stereo_->configure(*config);
      stereoPublishNow = true;
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
    if (loud) {
      loudPush(ch_[oL].data(), ch_[oR].data(), k);
      loudPublishNow = true;
    }
    if (stereo && stereo_->push(ch_[oL].data(), ch_[oR].data(), k) > 0) stereoPublishNow = true;
    if (bands != nullptr) bands->push(ch_[oL].data(), ch_[oR].data(), k);  // VZ.6
    if (wave != nullptr) wave->push(four ? ch_[0].data() : nullptr, ch_[oL].data(), ch_[oR].data(), k);
    if (wfOn) {
      wf_->push(ch_[oL].data(), ch_[oR].data(), k);
      wfPublishNow = true;
    }
    if (clOn) cloudPush(ch_[oL].data(), ch_[oR].data(), k);
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
              (gram && gramQuiet_ < gramCols_) || (loud && !loud_->atRest()) ||
              (stereo && !stereo_->atRest()) || (bands != nullptr && !bands->atRest()) ||
              (wave != nullptr && wave->quietFrames() < waveRestFrames_.load(std::memory_order_relaxed)) ||
              (wfOn && !wf_->atRest()) || (clOn && !cloud_->atRest()))) {
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
      if (loud) {  // the levels fall and the windows empty in time
        loudPush(mono_.data(), mono_.data(), frames);
        loudPublishNow = true;
      }
      if (stereo && stereo_->push(mono_.data(), mono_.data(), frames) > 0) stereoPublishNow = true;  // fades in time
      if (bands != nullptr) bands->pushSilence(frames);  // VZ.6: the levels fall, the picture scrolls on in time
      if (wave != nullptr) wave->pushSilence(frames);
      if (wfOn) {  // the history scrolls on in time
        wf_->push(mono_.data(), mono_.data(), frames);
        wfPublishNow = true;
      }
      if (clOn) cloudPush(mono_.data(), mono_.data(), frames);
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
                    (!gram || gramQuiet_ >= gramCols_) && (!loud || loud_->atRest()) &&
                    (!stereo || stereo_->atRest()) && (bands == nullptr || bands->atRest()) &&
                    (wave == nullptr || wave->quietFrames() >= waveRestFrames_.load(std::memory_order_relaxed)) &&
                    (!wfOn || wf_->atRest()) && (!clOn || cloud_->atRest());
  if (loudPublishNow) loudPublish();
  if (stereoPublishNow) stereoPublish();
  if (wfPublishNow) wfPublish();
  if (clPublishNow) clPublish();
  const bool settledNow = !sound && rest && (data || now - lastData_ > kNoDataSeconds);
  bool post = false;
  if (ran > 0 || settledNow != wasSettled) {
    hops_.fetch_add(ran, std::memory_order_relaxed);
    std::lock_guard lock(mutex_);
    if (ran > 0) {
      snap_.axis = out_->axis();
      if (out_->config().average) {
        snap_.average.resize(cfg_.points);
        out_->averageDb(snap_.average.data());
      } else {
        snap_.average.clear();
      }
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
  // The stages' recursive state (biquads, ballistics, decays) runs into subnormals over played silence: flush them on
  // this thread as on the audio threads (CLAUDE.md rule 3).
  const pq::DenormalGuard denormals;
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
