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
constexpr uint32_t kMaxPieces = 16;
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
  if (t >= c.xinStart && t < c.xinEnd)
    g *= std::sin(0.5 * std::numbers::pi * (static_cast<double>(t - c.xinStart) + 0.5) / static_cast<double>(c.xinEnd - c.xinStart));
  if (t >= c.xoutStart && t < c.xoutEnd)
    g *= std::cos(0.5 * std::numbers::pi * (static_cast<double>(t - c.xoutStart) + 0.5) / static_cast<double>(c.xoutEnd - c.xoutStart));
  return static_cast<float>(g);
}

// RePitch: the source pieces of a run of n output frames from song sample s (block-linear content position, split
// where the loop wraps). Returns the count; *wrapAfter = the sample of the next wrap after the run (INT64_MAX: none).
uint32_t repitchPieces(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c, SampleTime s, uint32_t n,
                       int64_t* pos, int64_t* step, uint32_t* at, SampleTime* wrapAfter) noexcept {  // RT-safe
  const double oA = img.repitchOffset(c, s), oB = img.repitchOffset(c, s + n);
  const auto L = static_cast<double>(c.length);
  const double pA = static_cast<double>(c.contentStart) + (c.reverse ? L - oA : oA);
  const double pB = static_cast<double>(c.contentStart) + (c.reverse ? L - oB : oB);
  const double dp = (pB - pA) / static_cast<double>(n);
  const int64_t st = toFixed(dp * c.framesPerTick);
  *wrapAfter = INT64_MAX;
  if (!c.looped || dp == 0.0) {
    pos[0] = toFixed(pA * c.framesPerTick);
    step[0] = st;
    at[0] = 0;
    return 1;
  }
  const auto end = static_cast<double>(c.loop.end), span = static_cast<double>(c.loop.end - c.loop.start);
  // The first output index at or past the next boundary from index i (boundaries end + k·span, k ≥ 0).
  auto boundaryIndex = [&](double p) noexcept -> double {
    if (dp > 0.0) {
      const double b = p < end ? end : end + span * (std::floor((p - end) / span) + 1.0);
      return std::ceil((b - pA) / dp);
    }
    if (p <= end) return 1e300;
    const double b = end + span * (std::ceil((p - end) / span) - 1.0);
    return std::floor((pA - b) / -dp) + 1.0;
  };
  uint32_t k = 0, i = 0;
  while (i < n && k < kMaxPieces) {
    const double p = pA + static_cast<double>(i) * dp;
    double e = boundaryIndex(p);
    if (!(e > static_cast<double>(i))) e = static_cast<double>(i) + 1.0;
    const auto next = static_cast<uint32_t>(std::min<double>(e, n));
    at[k] = i;
    pos[k] = toFixed(foldContent(p, c.loop) * c.framesPerTick);
    step[k] = st;
    ++k;
    i = k == kMaxPieces ? n : next;
  }
  // The first wrap after the run's last frame (it may fall on the next run's first frame).
  const double e = boundaryIndex(pA + static_cast<double>(n - 1) * dp);
  if (e < 1e299) *wrapAfter = s + static_cast<SampleTime>(e);
  return k;
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
  for (uint32_t idx : live_) voices_[idx].reader.cursor.stop();
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
  v.reader.cursor.stop();
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
  const uint32_t k = repitchPieces(*v.img, c, s, n, pos, step, at, nextWrap);
  for (uint32_t i = 0; i < k; ++i) out[i] = {at[i], (i + 1 < k ? at[i + 1] : n) - at[i], pos[i], step[i]};
  return k;
}

void ClipPlayer::renderRun(Voice& v, uint32_t frame, SampleTime s, uint32_t n) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  const media::SampleView& view = v.img->sample(c.sample).view();
  if (v.nextWrap <= s) v.lastWrap = v.nextWrap;  // the wrap the last run saw coming fell on this run's first frame
  Piece pc[kMaxPieces];
  SampleTime wrapAfter = INT64_MAX;
  const uint32_t np = pieces(v, s, n, pc, &wrapAfter);
  const SampleTime D = declick_;
  const bool wrapNear = c.looped && D > 0 &&
                        (np > 1 || (v.lastWrap != INT64_MIN && s - v.lastWrap <= D) ||
                         (wrapAfter != INT64_MAX && wrapAfter - (s + n) <= D));
  const bool flat = s >= c.flatStart && s + n <= c.flatEnd && v.inLen == 0 && v.state != State::Releasing &&
                    v.rampLen == 0 && !wrapNear;
  const float* env = nullptr;
  if (!flat) {
    float* e = env_.data();
    for (uint32_t k = 0; k < np; ++k) {
      const SampleTime lastWrap = k == 0 ? v.lastWrap : s + pc[k].at;
      const SampleTime nextWrap = k + 1 < np ? s + pc[k + 1].at : wrapAfter;
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
  if (np > 1) v.lastWrap = s + pc[np - 1].at;
  v.nextWrap = wrapAfter;
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

void ClipPlayer::read(Voice& v, const media::SampleView& view, const Piece& p, float* const* out, const float* env,
                      float gain) noexcept {  // RT-safe
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
  if (forward) {
    if (!r.started || r.dir != media::Direction::Forward || first < r.lo || first > r.hi) {
      r.cursor.start(*cfg_.streams, sample, first, media::Direction::Forward);
      r.dir = media::Direction::Forward;
      r.lo = r.hi = first;
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
      r.cursor.start(*cfg_.streams, sample, last, media::Direction::Reverse);
      r.dir = media::Direction::Reverse;
      r.lo = r.hi = last;
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

std::optional<ClipPlayer::StreamStart> ClipPlayer::streamStart(const AudioSequenceImage& img,
                                                              const AudioSequenceImage::Clip& c, SampleTime s,
                                                              uint32_t blockFrames) {
  if (c.sample == AudioSequenceImage::kNoSample || s >= c.playEnd) return std::nullopt;
  const SampleTime vs = std::max(s, c.playStart);
  int64_t pos = 0, step = 0;
  if (c.warp == ClipWarp::Off) {
    pos = lawAt(c.anchor, vs - c.bodyStart, c.step);
    step = c.step;
  } else {
    int64_t ps[kMaxPieces], st[kMaxPieces];
    uint32_t at[kMaxPieces];
    SampleTime w = 0;
    (void)repitchPieces(img, c, vs, std::max(blockFrames, 1u), ps, st, at, &w);
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
