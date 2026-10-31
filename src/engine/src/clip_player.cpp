// The clip player (T5.4): pq/engine/clip_player.h.
#include "pq/engine/clip_player.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "pq/core/automation_curve.h"
#include "pq/dsp/resample.h"

namespace pq::engine {

namespace {

constexpr int64_t kOne = dsp::kFixedOne;
constexpr uint32_t kMaxPieces = 64;  // a run: up to maxBlock / kRePitchGrid + 1 cells, plus loop wraps
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr int64_t kSlack = 8;  // history frames kept below what the filter needs (rounding of re-anchored rates)

int64_t toFixed(double v) noexcept { return static_cast<int64_t>(std::llround(v * 4294967296.0)); }  // RT-safe

// anchor + d · step in 32.32, wrapping arithmetic (exact whenever the true result fits; it does for any frame < 2^31).
int64_t lawAt(int64_t anchor, SampleTime d, int64_t step) noexcept {  // RT-safe
  return static_cast<int64_t>(static_cast<uint64_t>(anchor) + static_cast<uint64_t>(d) * static_cast<uint64_t>(step));
}

double foldContent(double p, const ContentLoop& l) noexcept {  // RT-safe
  if (!contentLooped(l) || p < static_cast<double>(l.end)) return p;
  const auto span = static_cast<double>(l.end - l.start);
  return static_cast<double>(l.start) + std::fmod(p - static_cast<double>(l.end), span);
}

// Same source law (a swap keeps the voice): positions equal at s, same step / content mapping, same sample.
bool sameLaw(const AudioSequenceImage& ia, const AudioSequenceImage::Clip& a, const AudioSequenceImage& ib,
             const AudioSequenceImage::Clip& b, SampleTime s) noexcept {  // RT-safe
  if (a.warp != b.warp || a.reverse != b.reverse || a.sample == AudioSequenceImage::kNoSample ||
      b.sample == AudioSequenceImage::kNoSample || !ia.sample(a.sample).sameAs(ib.sample(b.sample)))
    return false;
  if (a.warp == ClipWarp::Off)
    return a.step == b.step && lawAt(a.anchor, s - a.bodyStart, a.step) == lawAt(b.anchor, s - b.bodyStart, b.step);
  return a.start == b.start && a.contentStart == b.contentStart && a.loop.enabled == b.loop.enabled &&
         a.loop.start == b.loop.start && a.loop.end == b.loop.end && a.framesPerTick == b.framesPerTick &&
         (!a.reverse || a.length == b.length) && ia.repitchOffset(a, s) == ib.repitchOffset(b, s);
}

// The static envelope's shape at song sample t (without the clip gain). RT-safe.
float staticShape(const AudioSequenceImage::Clip& c, SampleTime t) noexcept {
  double g = 1.0;
  if (t >= c.bodyStart && t < c.inEnd)
    g *= automationShape(static_cast<double>(t - c.bodyStart) / static_cast<double>(c.inEnd - c.bodyStart), c.inCurve);
  if (t >= c.outStart && t < c.bodyEnd)
    g *= 1.0 - automationShape(static_cast<double>(t - c.outStart) / static_cast<double>(c.bodyEnd - c.outStart), c.outCurve);
  if (t >= c.xinStart && t < c.xinEnd) {
    const double u = (static_cast<double>(t - c.xinStart) + 0.5) / static_cast<double>(c.xinEnd - c.xinStart);
    g *= c.xinLinear ? u : std::sin(0.5 * std::numbers::pi * u);
  }
  if (t >= c.xoutStart && t < c.xoutEnd) {
    const double u = (static_cast<double>(t - c.xoutStart) + 0.5) / static_cast<double>(c.xoutEnd - c.xoutStart);
    g *= c.xoutLinear ? 1.0 - u : std::cos(0.5 * std::numbers::pi * u);
  }
  return static_cast<float>(g);
}

// RePitch content positions are linear on a fixed grid of kRePitchGrid song samples (M5 review M3): each cell
// [g, g + G) of the song gets the law through its two ends, whatever the engine block or run boundaries are, so a
// render is bit-identical across block sizes. Inside a cell the position is base + j · step − m · span (32.32,
// wrapping), with m the loop folds since the cell start; pieces split where m changes (a wrap) and at cell edges.
constexpr SampleTime kRePitchGrid = 256;

struct RePitchCell {
  SampleTime g = 0;   // the cell's first song sample
  double pA = 0.0;    // unfolded content position (ticks) at g
  double dp = 0.0;    // per song sample
  int64_t base = 0;   // 32.32 source frame of the folded pA
  int64_t step = 0;   // 32.32 per song sample
  int64_t m0 = 0;     // folds of pA
};

// How many loop spans p is folded by (foldContent(p) = p − foldCount(p) · span). RT-safe.
int64_t foldCount(double p, const ContentLoop& l) noexcept {
  if (!contentLooped(l) || p < static_cast<double>(l.end)) return 0;
  return static_cast<int64_t>(std::floor((p - static_cast<double>(l.end)) / static_cast<double>(l.end - l.start))) + 1;
}

RePitchCell rePitchCell(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c, SampleTime t) noexcept {  // RT-safe
  RePitchCell k;
  k.g = (t >= 0 ? t : t - (kRePitchGrid - 1)) / kRePitchGrid * kRePitchGrid;
  const auto L = static_cast<double>(c.length);
  const double oA = img.repitchOffset(c, k.g), oB = img.repitchOffset(c, k.g + kRePitchGrid);
  k.pA = static_cast<double>(c.contentStart) + (c.reverse ? L - oA : oA);
  const double pB = static_cast<double>(c.contentStart) + (c.reverse ? L - oB : oB);
  k.dp = (pB - k.pA) / static_cast<double>(kRePitchGrid);
  k.step = toFixed(k.dp * c.framesPerTick);
  k.base = toFixed(foldContent(k.pA, c.loop) * c.framesPerTick);
  k.m0 = foldCount(k.pA, c.loop);
  return k;
}

int64_t cellFolds(const RePitchCell& k, const AudioSequenceImage::Clip& c, SampleTime t) noexcept {  // RT-safe
  return c.looped ? foldCount(k.pA + static_cast<double>(t - k.g) * k.dp, c.loop) : 0;
}

// The first sample after t (exclusive) in [t + 1, limit) where the fold count of the cell's law differs from m;
// limit when none. RT-safe.
SampleTime nextFoldChange(const RePitchCell& k, const AudioSequenceImage::Clip& c, SampleTime t, int64_t m,
                          SampleTime limit) noexcept {
  if (!c.looped || k.dp == 0.0 || t + 1 >= limit) return limit;
  const auto end = static_cast<double>(c.loop.end), span = static_cast<double>(c.loop.end - c.loop.start);
  double j = 0.0;
  if (k.dp > 0.0) {  // the count becomes m + 1 at p = end + m · span
    j = std::ceil((end + static_cast<double>(m) * span - k.pA) / k.dp);
  } else {  // the count becomes m − 1 below p = end + (m − 1) · span
    if (m == 0) return limit;
    j = std::floor((k.pA - (end + static_cast<double>(m - 1) * span)) / -k.dp) + 1.0;
  }
  const double rel = static_cast<double>(limit - k.g);
  SampleTime x = j >= rel ? limit : k.g + static_cast<SampleTime>(std::max(j, 0.0));
  if (x <= t) x = t + 1;
  // The formula can be one off where p lands on a boundary: settle it on the count itself (monotonic in t).
  while (x > t + 1 && cellFolds(k, c, x - 1) != m) --x;
  while (x < limit && cellFolds(k, c, x) == m) ++x;
  return x;
}

// RePitch: the source pieces of a run of n output frames from song sample s (wrap[i]: piece i starts at a loop wrap).
// Returns the count; *wrapAfter = the next wrap after the run, exact when it is within `horizon` samples of the run's
// end (the declick needs it), else an estimate beyond that (stream look-ahead only; INT64_MAX: none).
uint32_t repitchPieces(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c, SampleTime s, uint32_t n,
                       SampleTime horizon, int64_t* pos, int64_t* step, uint32_t* at, bool* wrap,
                       SampleTime* wrapAfter) noexcept {  // RT-safe
  const SampleTime end = s + n;
  const int64_t spanF = c.looped ? toFixed(static_cast<double>(c.loop.end - c.loop.start) * c.framesPerTick) : 0;
  RePitchCell k = rePitchCell(img, c, s);
  // The fold count of the sample before the run, by its own cell's law (a wrap may fall on the run's first frame).
  int64_t prevM = 0;
  if (c.looped) {
    const RePitchCell kp = s - 1 >= k.g ? k : rePitchCell(img, c, s - 1);
    prevM = cellFolds(kp, c, s - 1);
  }
  uint32_t count = 0;
  SampleTime t = s;
  while (t < end && count < kMaxPieces) {
    if (t >= k.g + kRePitchGrid) k = rePitchCell(img, c, t);
    const SampleTime cellEnd = std::min(k.g + kRePitchGrid, end);
    const int64_t m = cellFolds(k, c, t);
    const SampleTime next = count + 1 == kMaxPieces ? end : nextFoldChange(k, c, t, m, cellEnd);
    at[count] = static_cast<uint32_t>(t - s);
    pos[count] = static_cast<int64_t>(static_cast<uint64_t>(k.base) + static_cast<uint64_t>(t - k.g) * static_cast<uint64_t>(k.step) -
                                      static_cast<uint64_t>(m - k.m0) * static_cast<uint64_t>(spanF));
    step[count] = k.step;
    wrap[count] = c.looped && m != prevM;
    ++count;
    prevM = m;  // the count of the piece's last sample (constant over the piece)
    t = next;
  }
  *wrapAfter = INT64_MAX;
  if (!c.looped || k.dp == 0.0) return count;
  // Exactly, over the cells up to `horizon` past the run.
  const SampleTime limit = end + std::max<SampleTime>(horizon, 0);
  for (SampleTime u = end; u <= limit;) {
    if (u >= k.g + kRePitchGrid || u < k.g) k = rePitchCell(img, c, u);
    const int64_t m = cellFolds(k, c, u);
    if (m != prevM) {
      *wrapAfter = u;
      return count;
    }
    const SampleTime cellEnd = std::min(k.g + kRePitchGrid, limit + 1);
    const SampleTime x = nextFoldChange(k, c, u, m, cellEnd);
    if (x < cellEnd) {
      *wrapAfter = x;
      return count;
    }
    prevM = m;
    u = cellEnd;
  }
  // Beyond the horizon: the last cell's law extended (an estimate, kept past the horizon).
  const SampleTime far = nextFoldChange(k, c, limit, cellFolds(k, c, limit), INT64_MAX / 4);
  if (far < INT64_MAX / 4) *wrapAfter = std::max(far, limit + 1);
  return count;
}

}  // namespace

ClipPlayer::ClipPlayer(const ClipPlayerConfig& config) : cfg_(config) {
  cfg_.slots = std::max(cfg_.slots, 1u);
  cfg_.maxBlockFrames = std::max(cfg_.maxBlockFrames, 1u);
  cfg_.maxVoices = std::max(cfg_.maxVoices, 1u);
  cfg_.historyFrames = std::max(cfg_.historyFrames, 4096u);
  if (!(cfg_.sampleRate >= 1000.0)) cfg_.sampleRate = 48000.0;
  declick_ = static_cast<uint32_t>(std::lround(std::max(cfg_.declickSeconds, 0.0) * cfg_.sampleRate));
  lookahead_ = static_cast<SampleTime>(std::llround(std::max(cfg_.lookaheadSeconds, 0.0) * cfg_.sampleRate));
  voices_.resize(cfg_.maxVoices);
  free_.reserve(cfg_.maxVoices);
  for (uint32_t i = cfg_.maxVoices; i-- > 0;) free_.push_back(i);
  live_.reserve(cfg_.maxVoices);
  freeBuffers_.reserve(cfg_.streamBuffers);
  for (uint32_t i = cfg_.streamBuffers; i-- > 0;) freeBuffers_.push_back(i);
  history_ = AlignedBuffer<float>(std::max<size_t>(static_cast<size_t>(cfg_.streamBuffers) * 2 * cfg_.historyFrames, 16));
  scratch_ = AlignedBuffer<float>(static_cast<size_t>(2) * cfg_.historyFrames);
  out_ = AlignedBuffer<float>(static_cast<size_t>(cfg_.slots) * kClipChannels * cfg_.maxBlockFrames);
  env_ = AlignedBuffer<float>(cfg_.maxBlockFrames);
  zeros_ = AlignedBuffer<float>(static_cast<size_t>(cfg_.maxBlockFrames) + 16);
  std::memset(zeros_.data(), 0, sizeof(float) * zeros_.size());
  ptrs_.assign(static_cast<size_t>(cfg_.slots) * kClipChannels, zeros_.data());
  active_.assign(cfg_.slots, 0);
  touchedFlag_.assign(cfg_.slots, 0);
  touched_.reserve(cfg_.slots);
  touchedBefore_.reserve(cfg_.slots);
  cursor_.assign(cfg_.slots, 0);
  (void)dsp::prepareSincTable(dsp::ResampleQuality::High, 256);
}

ClipPlayer::~ClipPlayer() {
  for (uint32_t idx : live_) stopReader(voices_[idx].reader);
}

void ClipPlayer::stopReader(Reader& r) noexcept {  // RT-safe
  r.cursor.stop();
  r.ahead.stop();
  r.aheadValid = false;
}

// ---- block framing ----------------------------------------------------------------------------------------------

void ClipPlayer::beginBlock(uint32_t frames) noexcept {  // RT-safe
  frames_ = std::min(frames, cfg_.maxBlockFrames);
  std::swap(touched_, touchedBefore_);
  touched_.clear();
  for (uint32_t k : touchedBefore_) touchedFlag_[k] = 0;
}

float* ClipPlayer::touch(uint32_t slot) noexcept {  // RT-safe
  float* ch0 = out_.data() + static_cast<size_t>(slot) * kClipChannels * cfg_.maxBlockFrames;
  if (touchedFlag_[slot] == 0) {
    touchedFlag_[slot] = 1;
    touched_.push_back(slot);
    std::memset(ch0, 0, sizeof(float) * kClipChannels * cfg_.maxBlockFrames);
  }
  return ch0;
}

void ClipPlayer::endBlock() noexcept {  // RT-safe
  for (uint32_t k : touchedBefore_)
    if (touchedFlag_[k] == 0) {
      active_[k] = 0;
      for (uint32_t c = 0; c < kClipChannels; ++c) ptrs_[static_cast<size_t>(k) * kClipChannels + c] = zeros_.data();
    }
  for (uint32_t k : touched_) {
    active_[k] = 1;
    const float* ch0 = out_.data() + static_cast<size_t>(k) * kClipChannels * cfg_.maxBlockFrames;
    for (uint32_t c = 0; c < kClipChannels; ++c)
      ptrs_[static_cast<size_t>(k) * kClipChannels + c] = ch0 + static_cast<size_t>(c) * cfg_.maxBlockFrames;
  }
}

// ---- voices -----------------------------------------------------------------------------------------------------

bool ClipPlayer::hasVoice(uint32_t slot, const AudioSequenceImage::Clip* c) const noexcept {  // RT-safe
  for (uint32_t idx : live_) {
    const Voice& v = voices_[idx];
    if (v.slot == slot && v.clip == c && v.state != State::Releasing) return true;
  }
  return false;
}

void ClipPlayer::startVoice(uint32_t slot, const AudioSequenceImage::Clip& c, SampleTime s0) noexcept {  // RT-safe
  if (free_.empty()) {
    ++stats_.voicesRefused;
    return;
  }
  const uint32_t idx = free_.back();
  free_.pop_back();
  Voice& v = voices_[idx];
  v.img = img_;
  v.clip = &c;
  v.slot = slot;
  v.inPos = v.inLen = 0;
  v.relPos = v.relLen = 0;
  v.rampPos = v.rampLen = 0;
  v.gainFrom = c.gain;
  v.lastWrap = INT64_MIN;
  v.nextWrap = INT64_MAX;
  v.reader.buf = kNone;
  v.reader.started = false;
  v.reader.aheadValid = false;
  if (c.playStart >= s0) {
    v.state = State::Pending;
    v.vs = c.playStart;
  } else {  // the playhead is inside the clip (a locate, a wrap, a swap): start here with a fade in
    v.state = State::Playing;
    v.vs = s0;
    v.inLen = declick_;
  }
  // A streamed sample's voice claims its stream now (up to the look-ahead early), without reading yet.
  const ClipSample& cs = img_->sample(c.sample);
  if (cs.pooled != nullptr && cfg_.streams != nullptr && cs.view().streamed() && !freeBuffers_.empty()) {
    if (const auto st = streamStart(*img_, c, v.vs, cfg_.maxBlockFrames)) {
      Reader& r = v.reader;
      r.buf = freeBuffers_.back();
      freeBuffers_.pop_back();
      r.cursor.start(*cfg_.streams, *cs.pooled, st->frame, st->dir);
      r.dir = st->dir;
      r.lo = r.hi = st->frame;
      r.off = st->dir == media::Direction::Forward ? 0u : cfg_.historyFrames;
      r.started = true;
    }
  }
  live_.push_back(idx);
  ++stats_.voicesStarted;
}

void ClipPlayer::endVoice(uint32_t i) noexcept {  // RT-safe
  const uint32_t idx = live_[i];
  Voice& v = voices_[idx];
  stopReader(v.reader);
  if (v.reader.buf != kNone) freeBuffers_.push_back(v.reader.buf);
  v.reader.buf = kNone;
  v.reader.started = false;
  if (prevUsers_ != 0 && v.img == prev_ && v.img != img_) --prevUsers_;
  v.img = nullptr;
  v.clip = nullptr;
  live_[i] = live_.back();
  live_.pop_back();
  free_.push_back(idx);
  ++stats_.voicesEnded;
}

void ClipPlayer::releaseVoice(Voice& v) noexcept {  // RT-safe
  v.state = State::Releasing;
  v.relPos = 0;
  v.relLen = declick_;
}

void ClipPlayer::releaseAll(uint32_t /*frame*/) noexcept {  // RT-safe
  for (uint32_t i = 0; i < live_.size();) {
    Voice& v = voices_[live_[i]];
    if (v.state == State::Pending || (v.state == State::Playing && declick_ == 0)) {
      endVoice(i);
      continue;
    }
    if (v.state == State::Playing) releaseVoice(v);
    ++i;
  }
}

void ClipPlayer::seekCursors(SampleTime s) noexcept {  // RT-safe
  nextStart_ = INT64_MIN;
  if (img_ == nullptr) return;
  const uint32_t n = std::min(img_->numSlots(), cfg_.slots);
  for (uint32_t slot = 0; slot < n; ++slot) {
    const std::span<const AudioSequenceImage::Clip> cl = img_->clips(slot);
    auto it = std::upper_bound(cl.begin(), cl.end(), s,
                               [](SampleTime v, const AudioSequenceImage::Clip& c) noexcept { return v < c.playStart; });
    auto i = static_cast<uint32_t>(it - cl.begin());
    while (i > 0 && cl[i - 1].playEnd > s) --i;  // clips under s (a crossfade: two)
    cursor_[slot] = i;
  }
}

void ClipPlayer::seek(SampleTime s) noexcept { seekCursors(s); }  // RT-safe

void ClipPlayer::setImage(const AudioSequenceImage* next, SampleTime s, bool /*playing*/) noexcept {  // RT-safe
  const AudioSequenceImage* prev = img_;
  img_ = next;
  prev_ = prev;
  prevUsers_ = 0;
  ++stats_.swaps;
  for (uint32_t i = 0; i < live_.size();) {
    Voice& v = voices_[live_[i]];
    if (v.state == State::Releasing) {
      if (v.img == prev && prev != next) ++prevUsers_;
      ++i;
      continue;
    }
    const AudioSequenceImage::Clip* match = nullptr;
    if (next != nullptr && v.slot < next->numSlots())
      for (const AudioSequenceImage::Clip& c : next->clips(v.slot))
        if (c.id == v.clip->id) {
          match = &c;
          break;
        }
    const SampleTime at = v.state == State::Pending ? v.vs : s;
    if (match != nullptr && v.img != nullptr && sameLaw(*v.img, *v.clip, *next, *match, at)) {
      if (match->gain != v.clip->gain) {
        const float now = v.rampLen != 0 ? v.gainFrom + (v.clip->gain - v.gainFrom) *
                                                            std::min(1.0f, static_cast<float>(v.rampPos) / static_cast<float>(v.rampLen))
                                         : v.clip->gain;
        v.gainFrom = now;
        v.rampPos = 0;
        v.rampLen = std::max(declick_, 1u);
      }
      if (v.state == State::Pending) v.vs = match->playStart;  // the start may have moved with the law (a trim)
      v.clip = match;
      v.img = next;
      ++stats_.kept;
      ++i;
      continue;
    }
    if (v.state == State::Pending || declick_ == 0) {
      endVoice(i);
      continue;
    }
    releaseVoice(v);
    ++stats_.faded;
    if (prev != next) ++prevUsers_;
    ++i;
  }
  seekCursors(s);
}

void ClipPlayer::startDue(SampleTime s0, SampleTime s1) noexcept {  // RT-safe
  ++stats_.scans;
  SampleTime next = INT64_MAX;
  const uint32_t n = std::min(img_->numSlots(), cfg_.slots);
  for (uint32_t slot = 0; slot < n; ++slot) {
    const std::span<const AudioSequenceImage::Clip> cl = img_->clips(slot);
    uint32_t i = cursor_[slot];
    while (i < cl.size() && cl[i].playStart < s1 + lookahead_) {
      const AudioSequenceImage::Clip& c = cl[i];
      if (c.sample != AudioSequenceImage::kNoSample && c.playEnd > s0 && !hasVoice(slot, &c)) startVoice(slot, c, s0);
      ++i;
    }
    cursor_[slot] = i;
    if (i < cl.size()) next = std::min(next, cl[i].playStart);
  }
  nextStart_ = next;
}

void ClipPlayer::play(uint32_t frame, SampleTime s0, uint32_t len) noexcept {  // RT-safe
  const SampleTime s1 = s0 + len;
  if (img_ != nullptr && (nextStart_ == INT64_MIN || s1 + lookahead_ > nextStart_)) startDue(s0, s1);
  for (uint32_t i = 0; i < live_.size();) {
    Voice& v = voices_[live_[i]];
    bool alive = true;
    if (v.state == State::Releasing) {
      alive = renderVoice(v, frame, len);
    } else {
      if (v.state == State::Playing) {
        v.vs = s0;  // in step with the song (a playing voice never lags)
      } else if (v.vs < s0) {  // a pending voice whose start a swap moved behind the playhead: start here, faded in
        v.vs = s0;
        v.inLen = declick_;
        v.inPos = 0;
      }
      if (v.vs < s1) {
        const auto off = static_cast<uint32_t>(std::max<SampleTime>(v.vs - s0, 0));
        v.state = State::Playing;
        alive = renderVoice(v, frame + off, len - off);
      }
    }
    if (!alive) endVoice(i);
    else ++i;
  }
}

void ClipPlayer::idle(uint32_t frames) noexcept {  // RT-safe
  for (uint32_t i = 0; i < live_.size();) {
    Voice& v = voices_[live_[i]];
    const bool alive = v.state == State::Releasing && renderVoice(v, 0, frames);
    if (!alive) endVoice(i);
    else ++i;
  }
}

bool ClipPlayer::renderVoice(Voice& v, uint32_t frame, uint32_t len) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  SampleTime n = std::min<SampleTime>(len, c.playEnd - v.vs);
  if (v.state == State::Releasing) n = std::min<SampleTime>(n, static_cast<SampleTime>(v.relLen) - v.relPos);
  if (n <= 0) return false;
  renderRun(v, frame, v.vs, static_cast<uint32_t>(n));
  v.vs += n;
  if (v.state == State::Releasing) {
    v.relPos += static_cast<uint32_t>(n);
    return v.relPos < v.relLen && v.vs < c.playEnd;
  }
  return v.vs < c.playEnd;
}

uint32_t ClipPlayer::pieces(Voice& v, SampleTime s, uint32_t n, Piece* out, SampleTime* nextWrap) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  *nextWrap = INT64_MAX;
  if (c.warp == ClipWarp::Off) {
    out[0] = {0, n, lawAt(c.anchor, s - c.bodyStart, c.step), c.step};
    return 1;
  }
  int64_t pos[kMaxPieces], step[kMaxPieces];
  uint32_t at[kMaxPieces];
  bool wrap[kMaxPieces];
  const uint32_t k = repitchPieces(*v.img, c, s, n, static_cast<SampleTime>(declick_) + 1, pos, step, at, wrap, nextWrap);
  for (uint32_t i = 0; i < k; ++i) out[i] = {at[i], (i + 1 < k ? at[i + 1] : n) - at[i], pos[i], step[i], wrap[i]};
  return k;
}

void ClipPlayer::renderRun(Voice& v, uint32_t frame, SampleTime s, uint32_t n) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  const media::SampleView& view = v.img->sample(c.sample).view();
  Piece pc[kMaxPieces];
  SampleTime wrapAfter = INT64_MAX;
  const uint32_t np = pieces(v, s, n, pc, &wrapAfter);
  const SampleTime D = declick_;
  bool anyWrap = false;
  for (uint32_t k = 0; k < np; ++k) anyWrap = anyWrap || pc[k].wrap;
  const bool wrapNear = c.looped && D > 0 &&
                        (anyWrap || (v.lastWrap != INT64_MIN && s - v.lastWrap <= D) ||
                         (wrapAfter != INT64_MAX && wrapAfter - (s + n) <= D));
  const bool flat = s >= c.flatStart && s + n <= c.flatEnd && v.inLen == 0 && v.state != State::Releasing &&
                    v.rampLen == 0 && !wrapNear;
  const float* env = nullptr;
  if (!flat) {
    float* e = env_.data();
    SampleTime lastWrap = v.lastWrap;
    for (uint32_t k = 0; k < np; ++k) {
      if (pc[k].wrap) lastWrap = s + pc[k].at;
      SampleTime nextWrap = wrapAfter;
      for (uint32_t q = k + 1; q < np; ++q)
        if (pc[q].wrap) {
          nextWrap = s + pc[q].at;
          break;
        }
      for (uint32_t i = pc[k].at; i < pc[k].at + pc[k].frames; ++i) {
        const SampleTime t = s + i;
        float g = staticShape(c, t);
        g *= v.rampLen != 0 ? v.gainFrom + (c.gain - v.gainFrom) *
                                               std::min(1.0f, static_cast<float>(v.rampPos + i + 1) / static_cast<float>(v.rampLen))
                            : c.gain;
        if (v.inLen != 0) g *= std::min(1.0f, static_cast<float>(v.inPos + i) / static_cast<float>(v.inLen));
        if (v.state == State::Releasing)
          g *= static_cast<float>(v.relLen - v.relPos - i) / static_cast<float>(v.relLen);
        if (c.looped && D > 0) {
          const double before = nextWrap == INT64_MAX ? 1e30 : static_cast<double>(nextWrap - t);
          const double after = lastWrap == INT64_MIN ? 1e30 : static_cast<double>(t - lastWrap + 1);
          g *= static_cast<float>(std::min(1.0, std::min(before, after) / static_cast<double>(D + 1)));
        }
        e[i] = g;
      }
    }
    env = e;
  }
  for (uint32_t k = 0; k < np; ++k)
    if (pc[k].wrap) v.lastWrap = s + pc[k].at;
  v.nextWrap = wrapAfter;
  // Q151 item 7: a wrap within the look-ahead on a streamed sample: its next pass is claimed now.
  if (c.looped && wrapAfter != INT64_MAX && wrapAfter - (s + n) <= lookahead_ && view.streamed())
    claimAhead(v, view, pc[np - 1].step);
  if (v.inLen != 0) {
    v.inPos += n;
    if (v.inPos >= v.inLen) v.inLen = v.inPos = 0;
  }
  if (v.rampLen != 0) {
    v.rampPos += n;
    if (v.rampPos >= v.rampLen) v.rampLen = v.rampPos = 0;
  }
  if (view.channels == 0 || view.frames <= 0) return;
  float* ch0 = touch(v.slot);
  float* out[2] = {ch0 + frame, ch0 + cfg_.maxBlockFrames + frame};
  for (uint32_t k = 0; k < np; ++k) {
    float* o[2] = {out[0] + pc[k].at, out[1] + pc[k].at};
    read(v, view, pc[k], o, env != nullptr ? env + pc[k].at : nullptr, c.gain);
  }
}

void ClipPlayer::read(Voice& v, const media::SampleView& sampleView, const Piece& piece, float* const* out,
                      const float* env, float gain) noexcept {  // RT-safe
  // Q151 item 1: faster than rate 2, a fully resident sample with decimated copies is read on the level L that brings
  // the rate to rate / 2^L ≤ 2 (media::SampleMips: frame m of level L is the sample at 2^L·m, so the 32.32 position and
  // step scale by 2^−L): the cost of a +12 st read at most.
  const media::SampleView* vp = &sampleView;
  Piece p = piece;
  if ((p.step > 2 * kOne || p.step < -2 * kOne) && !sampleView.streamed()) {
    const ClipSample& cs = v.img->sample(v.clip->sample);
    if (cs.pooled != nullptr)
      if (const media::SampleMips* m = cs.pooled->mips(); m != nullptr && m->level[0].channels == sampleView.channels) {
        const uint32_t L = dsp::mipLevelFor(static_cast<double>(p.step) / 4294967296.0, m->levels);
        vp = &m->level[L - 1];
        p.pos >>= L;  // arithmetic: positions may be negative (a reverse read near the start)
        p.step >>= L;
        stats_.mipFrames += p.frames;
      }
  }
  const media::SampleView& view = *vp;
  const bool copy = (p.step == kOne || p.step == -kOne) && (p.pos & 0xFFFFFFFF) == 0;
  const dsp::SincTable* tb = copy ? nullptr
                                  : &dsp::sincTableAtLeast(dsp::ResampleQuality::High,
                                                           dsp::sincStretchQ(static_cast<double>(p.step) / 4294967296.0));
  const uint32_t ch = std::min<uint32_t>(view.channels, 2);
  const int64_t cap = static_cast<int64_t>(cfg_.historyFrames) - 2 * kSlack;
  // Frames one output frame of the run advances, and the filter's extra reach.
  const double rate = std::abs(static_cast<double>(p.step)) / 4294967296.0;
  const int64_t extra = copy ? 1 : static_cast<int64_t>(tb->taps) + 2;
  const auto chunk = static_cast<uint32_t>(std::max<double>(1.0, std::floor(static_cast<double>(cap - extra) / std::max(rate, 1e-9))));
  for (uint32_t done = 0; done < p.frames;) {
    const uint32_t n = std::min(p.frames - done, chunk);
    const int64_t pos = p.pos + static_cast<int64_t>(done) * p.step;
    int64_t first = 0, last = 0;
    if (copy) {
      const int64_t i0 = pos >> 32;
      first = p.step > 0 ? i0 : i0 - n + 1;
      last = first + n;
    } else {
      const dsp::SincReach r = dsp::sincReach(*tb, pos, p.step, n);
      first = r.first;
      last = r.last;
    }
    float* o[2] = {out[0] + done, out[1] + done};
    const float* e = env != nullptr ? env + done : nullptr;
    done += n;
    if (last <= 0 || first >= view.frames) continue;  // entirely outside the file: silence
    if (copy) stats_.copyFrames += n;
    else stats_.sincFrames += n;
    const float* base[2] = {view.data[0], view.data[ch == 2 ? 1 : 0]};
    int64_t baseFrame = 0;
    const int64_t G = media::kSampleGuardFrames;
    const int64_t limit = view.streamed() ? view.residentFrames : view.frames + G;
    if (!(first >= -G && last <= limit)) {
      const ClipSample& cs = v.img->sample(v.clip->sample);
      if (view.streamed() && cs.pooled != nullptr && cfg_.streams != nullptr) {
        if (!ensure(v, view, first, last, p.step > 0, base, &baseFrame)) continue;
      } else {  // gather what the file has into the scratch buffer, zeros around it
        const int64_t avail = view.streamed() ? view.residentFrames : view.frames;
        const int64_t a = std::max<int64_t>(first, 0), b = std::min<int64_t>(last, avail);
        for (uint32_t c = 0; c < ch; ++c) {
          float* dst = scratch_.data() + static_cast<size_t>(c) * cfg_.historyFrames;
          std::memset(dst, 0, sizeof(float) * static_cast<size_t>(last - first));
          if (b > a) std::memcpy(dst + (a - first), view.data[c] + a, sizeof(float) * static_cast<size_t>(b - a));
        }
        base[0] = scratch_.data();
        base[1] = scratch_.data() + (ch == 2 ? cfg_.historyFrames : 0u);
        baseFrame = first;
      }
    }
    if (copy) {
      dsp::gainCopy(base, ch, (pos >> 32) - baseFrame, p.step > 0 ? 1 : -1, n, e, gain, o);
    } else {
      dsp::sincResample(*tb, base, ch, pos - baseFrame * kOne, p.step, n, e, gain, o);
    }
  }
}

bool ClipPlayer::ensure(Voice& v, const media::SampleView& view, int64_t first, int64_t last, bool forward,
                        const float** base, int64_t* baseFrame) noexcept {  // RT-safe
  Reader& r = v.reader;
  if (r.buf == kNone) {
    if (freeBuffers_.empty()) {
      ++stats_.noHistory;
      return false;
    }
    r.buf = freeBuffers_.back();
    freeBuffers_.pop_back();
    r.started = false;
  }
  const media::PooledSample& sample = *v.img->sample(v.clip->sample).pooled;
  const uint32_t cap = cfg_.historyFrames;
  const uint32_t ch = std::min<uint32_t>(view.channels, 2);
  float* b[2] = {history_.data() + static_cast<size_t>(r.buf) * 2 * cap, history_.data() + (static_cast<size_t>(r.buf) * 2 + 1) * cap};
  auto put = [&](const media::FrameSpan& sp, float* const* dst) noexcept {
    uint32_t w = 0;
    for (uint32_t i = 0; i < sp.pieces; ++i) {
      for (uint32_t c = 0; c < ch; ++c) std::memcpy(dst[c] + w, sp.piece[i].ch[c], sizeof(float) * sp.piece[i].frames);
      w += sp.piece[i].frames;
    }
    stats_.missingFrames += sp.missing;
  };
  // A jump (a loop wrap) that lands where the claimed next pass starts: adopt it (its frames are being fetched since
  // the claim), reading from its first frame (the few frames before `first` are read and kept).
  auto adopt = [&](media::Direction dir, bool lands) noexcept {
    if (!r.aheadValid || r.aheadDir != dir || !lands) return false;
    r.cursor.swap(r.ahead);
    r.ahead.stop();
    r.aheadValid = false;
    r.lo = r.hi = r.aheadFrame;
    ++stats_.loopAdopts;
    return true;
  };
  if (forward) {
    if (!r.started || r.dir != media::Direction::Forward || first < r.lo || first > r.hi) {
      if (!adopt(media::Direction::Forward, first >= r.aheadFrame && first - r.aheadFrame <= cap / 2)) {
        r.cursor.start(*cfg_.streams, sample, first, media::Direction::Forward);
        r.lo = r.hi = first;
      }
      r.dir = media::Direction::Forward;
      r.off = 0;
      r.started = true;
    }
    const int64_t keep = std::max(r.lo, first - kSlack);
    if (static_cast<int64_t>(r.off) + (r.hi - r.lo) + std::max<int64_t>(last - r.hi, 0) > cap) {
      const int64_t count = r.hi - keep;
      for (uint32_t c = 0; c < ch; ++c)
        std::memmove(b[c], b[c] + r.off + (keep - r.lo), sizeof(float) * static_cast<size_t>(count));
      r.off = 0;
      r.lo = keep;
    }
    while (r.hi < last) {
      const auto k = static_cast<uint32_t>(std::min<int64_t>(last - r.hi, media::kMaxSpanFrames));
      if (fill_ != nullptr && r.cursor.streaming()) {
        const int64_t need = r.hi + k - std::max(r.hi, view.residentFrames);
        if (need > 0) fill_(fillUser_, r.cursor.stream(), need);
      }
      float* dst[2] = {b[0] + r.off + (r.hi - r.lo), b[1] + r.off + (r.hi - r.lo)};
      put(r.cursor.next(k), dst);
      r.hi += k;
    }
  } else {
    if (!r.started || r.dir != media::Direction::Reverse || last > r.hi || last < r.lo) {
      if (!adopt(media::Direction::Reverse, last <= r.aheadFrame && r.aheadFrame - last <= cap / 2)) {
        r.cursor.start(*cfg_.streams, sample, last, media::Direction::Reverse);
        r.lo = r.hi = last;
      }
      r.dir = media::Direction::Reverse;
      r.off = cap;
      r.started = true;
    }
    const int64_t keepTo = std::min(r.hi, last + kSlack);
    if (static_cast<int64_t>(r.off) < std::max<int64_t>(r.lo - first, 0)) {
      const int64_t count = keepTo - r.lo;
      for (uint32_t c = 0; c < ch; ++c)
        std::memmove(b[c] + cap - count, b[c] + r.off, sizeof(float) * static_cast<size_t>(count));
      r.off = static_cast<uint32_t>(cap - count);
      r.hi = keepTo;
    }
    while (r.lo > first) {
      const auto k = static_cast<uint32_t>(std::min<int64_t>(r.lo - first, media::kMaxSpanFrames));
      if (fill_ != nullptr && r.cursor.streaming() && r.lo > view.residentFrames) {
        const int64_t need = r.lo - std::max(r.lo - k, view.residentFrames);
        if (need > 0) fill_(fillUser_, r.cursor.stream(), need);
      }
      float* dst[2] = {b[0] + r.off - k, b[1] + r.off - k};
      put(r.cursor.next(k), dst);
      r.off -= k;
      r.lo -= k;
    }
  }
  base[0] = b[0] + r.off;
  base[1] = ch == 2 ? b[1] + r.off : b[0] + r.off;
  *baseFrame = r.lo;
  return true;
}

// Q151 item 7: the next pass of a looped RePitch voice whose wrap is near (renderRun: within the look-ahead). After a
// forward wrap the content reads from just past the loop start, after a reverse one from just below the loop end;
// the cursor starts a little outside the filter's reach there (2 kSlack frames: the per-block rate rounding), so the
// jump lands inside what it fetched. One claim at a time (the predicted wrap sample moves by a frame or so from block to
// block with the re-anchored rate: re-claiming per block would restart the fetch every time).
void ClipPlayer::claimAhead(Voice& v, const media::SampleView& view, int64_t step) noexcept {  // RT-safe
  Reader& r = v.reader;
  if (r.aheadValid || cfg_.streams == nullptr || step == 0) return;
  const ClipSample& cs = v.img->sample(v.clip->sample);
  if (cs.pooled == nullptr) return;
  const AudioSequenceImage::Clip& c = *v.clip;
  const dsp::SincTable& tb =
      dsp::sincTableAtLeast(dsp::ResampleQuality::High, dsp::sincStretchQ(static_cast<double>(step) / 4294967296.0));
  const bool fwd = step > 0;
  int64_t frame = 0;
  if (fwd) {
    frame = static_cast<int64_t>(std::floor(static_cast<double>(c.loop.start) * c.framesPerTick)) -
            static_cast<int64_t>(tb.center) - 2 * kSlack;
  } else {
    frame = static_cast<int64_t>(std::floor(static_cast<double>(c.loop.end) * c.framesPerTick)) -
            static_cast<int64_t>(tb.center) + 1 + static_cast<int64_t>(tb.taps) + 2 * kSlack;
  }
  frame = std::clamp<int64_t>(frame, 0, view.frames);
  const media::Direction dir = fwd ? media::Direction::Forward : media::Direction::Reverse;
  r.ahead.start(*cfg_.streams, *cs.pooled, frame, dir);
  r.aheadFrame = frame;
  r.aheadDir = dir;
  r.aheadValid = true;
  ++stats_.loopClaims;
}

std::optional<ClipPlayer::StreamStart> ClipPlayer::streamStart(const AudioSequenceImage& img,
                                                              const AudioSequenceImage::Clip& c, SampleTime s,
                                                              uint32_t blockFrames) noexcept {  // RT-safe
  if (c.sample == AudioSequenceImage::kNoSample || s >= c.playEnd) return std::nullopt;
  const SampleTime vs = std::max(s, c.playStart);
  int64_t pos = 0, step = 0;
  if (c.warp == ClipWarp::Off) {
    pos = lawAt(c.anchor, vs - c.bodyStart, c.step);
    step = c.step;
  } else {
    int64_t ps[kMaxPieces], st[kMaxPieces];
    uint32_t at[kMaxPieces];
    bool wr[kMaxPieces];
    SampleTime w = 0;
    (void)repitchPieces(img, c, vs, std::max(blockFrames, 1u), 0, ps, st, at, wr, &w);
    pos = ps[0];
    step = st[0];
  }
  StreamStart out;
  out.dir = step >= 0 ? media::Direction::Forward : media::Direction::Reverse;
  const bool copy = (step == kOne || step == -kOne) && (pos & 0xFFFFFFFF) == 0;
  if (copy) {
    out.frame = step > 0 ? pos >> 32 : (pos >> 32) + 1;
  } else {
    const dsp::SincTable& tb = dsp::sincTableAtLeast(dsp::ResampleQuality::High,
                                                     dsp::sincStretchQ(static_cast<double>(step) / 4294967296.0));
    const dsp::SincReach r = dsp::sincReach(tb, pos, step, 1);
    out.frame = step > 0 ? r.first : r.last;
  }
  return out;
}

}  // namespace pq::engine
