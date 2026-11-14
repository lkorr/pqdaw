// The clip player (T5.4): pq/engine/clip_player.h.
#include "pq/engine/clip_player.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

#include "pq/core/automation_curve.h"
#include "pq/dsp/resample.h"
#include "pq/dsp/warp.h"

namespace pq::engine {

namespace {

constexpr int64_t kOne = dsp::kFixedOne;
constexpr uint32_t kMaxPieces = 128;  // a run: up to maxBlock / kRePitchGrid + 1 cells, plus loop wraps and segment changes
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

bool sameSegments(const AudioSequenceImage& ia, const AudioSequenceImage::Clip& a, const AudioSequenceImage& ib,
                  const AudioSequenceImage::Clip& b) noexcept {  // RT-safe
  const std::span<const WarpSegment> x = ia.segments(a), y = ib.segments(b);
  if (x.size() != y.size()) return false;
  for (size_t i = 0; i < x.size(); ++i)
    if (!(x[i] == y[i])) return false;
  return true;
}

bool sameSlices(const AudioSequenceImage& ia, const AudioSequenceImage::Clip& a, const AudioSequenceImage& ib,
                const AudioSequenceImage::Clip& b) noexcept {  // RT-safe
  const std::span<const double> x = ia.sliceContent(a), y = ib.sliceContent(b);
  const std::span<const double> fx = ia.sliceFrame(a), fy = ib.sliceFrame(b);
  if (x.size() != y.size()) return false;
  for (size_t i = 0; i < x.size(); ++i)
    if (x[i] != y[i] || fx[i] != fy[i]) return false;
  return true;
}

// Same source law (a swap keeps the voice): positions equal at s, same step / content mapping, same sample. Complex
// Pro's formant settings are not part of it: the kept voice's stretcher takes them at once (AC.fix: a formant edit
// used to replace the voice, which re-primed: silent for its priming, and for a whole knob drag).
bool sameLaw(const AudioSequenceImage& ia, const AudioSequenceImage::Clip& a, const AudioSequenceImage& ib,
             const AudioSequenceImage::Clip& b, SampleTime s) noexcept {  // RT-safe
  if (a.warp != b.warp || a.reverse != b.reverse || a.sample == AudioSequenceImage::kNoSample ||
      b.sample == AudioSequenceImage::kNoSample || !ia.sample(a.sample).sameAs(ib.sample(b.sample)))
    return false;
  if (a.warp == ClipWarp::Off)
    return a.step == b.step && lawAt(a.anchor, s - a.bodyStart, a.step) == lawAt(b.anchor, s - b.bodyStart, b.step);
  if (!sameSegments(ia, a, ib, b)) return false;
  if (clipStretches(a.warp))  // T6.3: grains and slices are anchored on the clip's own samples
    return a.bodyStart == b.bodyStart && a.start == b.start && a.length == b.length &&
           a.contentStart == b.contentStart && a.loop.enabled == b.loop.enabled && a.loop.start == b.loop.start &&
           a.loop.end == b.loop.end && a.pitchStep == b.pitchStep && a.grain == b.grain && a.alignLags == b.alignLags &&
           a.alignLen == b.alignLen && a.jitter == b.jitter && a.seed == b.seed && a.sliceLoop == b.sliceLoop &&
           a.envelope == b.envelope && a.playStart == b.playStart && sameSlices(ia, a, ib, b);
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
constexpr SampleTime kRePitchGrid = 64;  // 1.3 ms: a ramp's chord error stays ~1e-3 frames (256 gave ~0.02)

struct RePitchCell {
  SampleTime g = 0;   // the cell's first song sample
  double pA = 0.0;    // unfolded content position (ticks) at g
  double dp = 0.0;    // per song sample
  double qA = 0.0;    // pA folded
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
  k.qA = foldContent(k.pA, c.loop);
  k.m0 = foldCount(k.pA, c.loop);
  return k;
}

// T6.3: the warp segment of folded content q (the last with content0 ≤ q, else the first). RT-safe.
uint32_t segIndex(std::span<const WarpSegment> g, double q) noexcept {
  uint32_t lo = 1, hi = static_cast<uint32_t>(g.size());
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (static_cast<double>(g[mid].content0) <= q) lo = mid + 1;
    else hi = mid;
  }
  return lo - 1;
}

// The folded content at t of a cell whose fold count there is m. RT-safe.
double cellContent(const RePitchCell& k, const AudioSequenceImage::Clip& c, SampleTime t, int64_t m) noexcept {
  const double span = c.looped ? static_cast<double>(c.loop.end - c.loop.start) : 0.0;
  return k.qA + static_cast<double>(t - k.g) * k.dp - static_cast<double>(m - k.m0) * span;
}

// The first sample in [t + 1, limit) where the content (fold count m throughout) leaves segment `seg`; limit when
// none. RT-safe.
SampleTime nextSegChange(const RePitchCell& k, const AudioSequenceImage::Clip& c, std::span<const WarpSegment> g,
                         SampleTime t, int64_t m, uint32_t seg, SampleTime limit) noexcept {
  if (g.size() < 2 || k.dp == 0.0 || t + 1 >= limit) return limit;
  const double span = c.looped ? static_cast<double>(c.loop.end - c.loop.start) : 0.0;
  const double shift = static_cast<double>(m - k.m0) * span;
  double j = 0.0;
  if (k.dp > 0.0) {
    if (seg + 1 >= g.size()) return limit;
    j = std::ceil((static_cast<double>(g[seg + 1].content0) + shift - k.qA) / k.dp);
  } else {
    if (seg == 0) return limit;
    j = std::floor((k.qA - shift - static_cast<double>(g[seg].content0)) / -k.dp) + 1.0;
  }
  const double rel = static_cast<double>(limit - k.g);
  SampleTime x = j >= rel ? limit : k.g + static_cast<SampleTime>(std::max(j, 0.0));
  if (x <= t) x = t + 1;
  while (x > t + 1 && segIndex(g, cellContent(k, c, x - 1, m)) != seg) --x;
  while (x < limit && segIndex(g, cellContent(k, c, x, m)) == seg) ++x;
  return x;
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
  const std::span<const WarpSegment> segs = img.segments(c);
  const bool multi = segs.size() > 1;
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
    const bool last = count + 1 == kMaxPieces;
    const SampleTime nextFold = last ? end : nextFoldChange(k, c, t, m, cellEnd);
    // T6.3: the segment the piece reads in; it ends where the content leaves it (the law is linear inside).
    const uint32_t sg = multi ? segIndex(segs, cellContent(k, c, t, m)) : 0;
    const SampleTime next = multi && !last ? nextSegChange(k, c, segs, t, m, sg, nextFold) : nextFold;
    const WarpSegment& w = segs[sg];
    // The segment's law at the cell start (extrapolated): exact for the one implied segment as M5's base.
    const int64_t base = toFixed(w.frame0 + (k.qA - static_cast<double>(w.content0)) * w.framesPerTick);
    const int64_t st = toFixed(k.dp * w.framesPerTick);
    const int64_t spanF = c.looped ? toFixed(static_cast<double>(c.loop.end - c.loop.start) * w.framesPerTick) : 0;
    at[count] = static_cast<uint32_t>(t - s);
    pos[count] = static_cast<int64_t>(static_cast<uint64_t>(base) + static_cast<uint64_t>(t - k.g) * static_cast<uint64_t>(st) -
                                      static_cast<uint64_t>(m - k.m0) * static_cast<uint64_t>(spanF));
    step[count] = st;
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
  gain_ = AlignedBuffer<float>(static_cast<size_t>(cfg_.maxBlockFrames) + 16);
  corrA_ = AlignedBuffer<float>(AudioSequenceImage::kMaxAlignLen + 64);
  corrB_ = AlignedBuffer<float>(AudioSequenceImage::kMaxAlignLen + 2 * AudioSequenceImage::kMaxAlignLags + 64);
  zeros_ = AlignedBuffer<float>(static_cast<size_t>(cfg_.maxBlockFrames) + 16);
  std::memset(zeros_.data(), 0, sizeof(float) * zeros_.size());
  if (cfg_.stretchCapacity + cfg_.stretchVoices + cfg_.stretchMonoVoices > 0) {  // T6.5 (T6.x-fix-warp: grows)
    stretch_ = std::make_unique<StretchPool>(cfg_.stretchVoices, cfg_.sampleRate, cfg_.stretchMonoVoices,
                                             cfg_.stretchCapacity, kStretchFrames);
    stretchInCap_ = std::max<uint32_t>(32768u, static_cast<uint32_t>(stretch_->latency().seek) + kStretchCell);
    stretchIn_ = AlignedBuffer<float>(static_cast<size_t>(2) * stretchInCap_);
  }
  ptrs_.assign(static_cast<size_t>(cfg_.slots) * kClipChannels, zeros_.data());
  active_.assign(cfg_.slots, 0);
  touchedFlag_.assign(cfg_.slots, 0);
  touched_.reserve(cfg_.slots);
  touchedBefore_.reserve(cfg_.slots);
  cursor_.assign(cfg_.slots, 0);
  (void)dsp::prepareSincTable(dsp::ResampleQuality::High, 256);
}

ClipPlayer::~ClipPlayer() {
  for (uint32_t idx : live_) {
    stopReader(voices_[idx].reader);
    stopReader(voices_[idx].reader2);
  }
  StretchPool::destroy(stretchSpill_);
}

ClipPlayer::StretchDemand ClipPlayer::stretchDemand(const AudioSequenceImage& img) {
  StretchDemand d;
  const auto need = [](uint32_t n) { return n == 0 ? 0u : n == 1 ? 2u : 3u; };
  for (uint32_t slot = 0; slot < img.numSlots(); ++slot) {
    uint32_t stereo = 0, mono = 0;
    for (const AudioSequenceImage::Clip& c : img.clips(slot)) {
      if (!clipComplex(c.warp) || c.sample == AudioSequenceImage::kNoSample) continue;
      ++(img.sample(c.sample).view().channels == 1 ? mono : stereo);
    }
    d.stereo += need(stereo);
    d.mono += need(mono);
  }
  return d;
}

void ClipPlayer::adoptStretchers(StretchPool::Slot* list) noexcept {  // RT-safe
  if (list == nullptr) return;
  stretchSpill_ = StretchPool::append(stretchSpill_, stretch_ != nullptr ? stretch_->adopt(list) : list);
  cueScanned_ = false;  // a cue may now find stretchers for clips it skipped
}

void ClipPlayer::stopReader(Reader& r) noexcept {  // RT-safe
  r.cursor.stop();
  r.ahead.stop();
  r.aheadValid = false;
}

// ---- block framing ----------------------------------------------------------------------------------------------

void ClipPlayer::beginBlock(uint32_t frames) noexcept {  // RT-safe
  frames_ = std::min(frames, cfg_.maxBlockFrames);
  blockCells_ = 0;
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
  stats_.stretchCellsBlockMax = std::max(stats_.stretchCellsBlockMax, blockCells_);
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
    if (v.slot == slot && v.clip == c && v.state != State::Releasing && !v.cue) return true;
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
  v.reader.pass = INT64_MIN;
  v.reader2.buf = kNone;
  v.reader2.started = false;
  v.reader2.aheadValid = false;
  v.reader2.pass = INT64_MIN;
  v.grainIdx = INT64_MIN;
  v.stretch = StretchPool::kNone;
  v.takeover = v.replaces = kNone;
  v.soundFrom = 0;
  v.vHi = INT64_MIN;
  v.cue = false;
  if (clipComplex(c.warp)) {  // T6.5: a stretcher, or Tones when none is free (T6.5b: a mono one for a mono source)
    if (stretch_ != nullptr) v.stretch = stretch_->claim(c.seed, img_->sample(c.sample).view().channels == 1 ? 1u : 2u);
    if (v.stretch != StretchPool::kNone) {
      ++stats_.stretchClaims;
      if (stretch_->channels(v.stretch) == 1) ++stats_.stretchMonoClaims;
      // T6.5b: its priming schedule starts at its look-ahead (or now, when that has passed: Play, a locate).
      v.primeFrom = std::max(s0, c.playStart - lookahead_);
      v.primeBase = complexFirstFrame(c);
    } else {
      fallbacksShared_.store(++stats_.stretchFallbacks, std::memory_order_relaxed);
    }
  }
  if (c.playStart >= s0) {
    v.state = State::Pending;
    v.vs = c.playStart;
  } else if (v.stretch != StretchPool::kNone) {  // T6.5b: inside the clip: pending until primed (gateComplex)
    v.state = State::Pending;
    v.vs = s0;
    v.inLen = declick_;
  } else {  // the playhead is inside the clip (a locate, a wrap, a swap): start here with a fade in
    v.state = State::Playing;
    v.vs = s0;
    v.inLen = declick_;
  }
  // A streamed sample's voice claims its stream now (up to the look-ahead early), without reading yet.
  const ClipSample& cs = img_->sample(c.sample);
  if (cs.pooled != nullptr && cfg_.streams != nullptr && cs.view().streamed() && !freeBuffers_.empty()) {
    std::optional<StreamStart> st;
    if (clipComplex(c.warp)) {  // where this voice reads first: its stretcher's priming, or the Tones fallback's grains
      bool fwd = true;
      st = StreamStart{stretchStreamFrame(*img_, c, v.vs, &fwd, v.stretch != StretchPool::kNone),
                       fwd ? media::Direction::Forward : media::Direction::Reverse};
    } else {
      st = streamStart(*img_, c, v.vs, cfg_.maxBlockFrames);
    }
    if (st) {
      Reader& r = v.reader;
      r.buf = freeBuffers_.back();
      freeBuffers_.pop_back();
      r.cursor.start(*cfg_.streams, *cs.pooled, st->frame, st->dir);
      r.dir = st->dir;
      r.lo = r.hi = st->frame;
      r.off = st->dir == media::Direction::Forward ? 0u : cfg_.historyFrames;
      r.started = true;
      if (c.warp == ClipWarp::Beats) r.pass = passAt(*img_, c, v.vs).T;  // the slice the claim is for
    }
  }
  live_.push_back(idx);
  ++stats_.voicesStarted;
}

void ClipPlayer::endVoice(uint32_t i) noexcept {  // RT-safe
  const uint32_t idx = live_[i];
  Voice& v = voices_[idx];
  for (Reader* r : {&v.reader, &v.reader2}) {
    stopReader(*r);
    if (r->buf != kNone) freeBuffers_.push_back(r->buf);
    r->buf = kNone;
    r->started = false;
    r->pass = INT64_MIN;
  }
  if (v.stretch != StretchPool::kNone) {
    stretch_->release(v.stretch);
    v.stretch = StretchPool::kNone;
  }
  unlinkTakeover(v);
  if (prevUsers_ != 0 && v.img == prev_ && v.img != img_) --prevUsers_;
  v.img = nullptr;
  v.clip = nullptr;
  live_[i] = live_.back();
  live_.pop_back();
  free_.push_back(idx);
  ++stats_.voicesEnded;
}

// T6.x-fix-warp (H2). A Complex voice playing as Tones (no stretcher was free at its start): when one is free now,
// a second voice of the clip starts here like a locate's start (pending, primed on its schedule, late, faded in) and
// takes over (takeoverAt). Streamed samples also need a free history buffer for it.
void ClipPlayer::retryStretch(Voice& v, SampleTime s0) noexcept {  // RT-safe
  if (v.img != img_ || v.clip->sample == AudioSequenceImage::kNoSample || free_.empty()) return;
  const ClipSample& cs = img_->sample(v.clip->sample);
  if (!stretch_->canClaim(cs.view().channels == 1 ? 1u : 2u)) return;
  if (cs.pooled != nullptr && cfg_.streams != nullptr && cs.view().streamed() && freeBuffers_.empty()) return;
  const auto self = static_cast<uint32_t>(&v - voices_.data());
  startVoice(v.slot, *v.clip, s0);
  const uint32_t r = live_.back();
  if (voices_[r].stretch == StretchPool::kNone) {  // not expected (canClaim): drop it, retry next block
    endVoice(static_cast<uint32_t>(live_.size() - 1));
    return;
  }
  voices_[r].replaces = self;
  v.takeover = r;
}

// AC.fix: a playing Complex voice whose clip `match` (in the new image) reads another law plays on until a voice of
// `match` started here (pending, primed on its schedule) sounds, as a fallen-back voice's takeover does. Only with a
// stretcher free for it, and the clip still under the playhead; false: the caller fades the voice out.
bool ClipPlayer::handOver(Voice& v, const AudioSequenceImage::Clip* match, SampleTime s) noexcept {  // RT-safe
  if (match == nullptr || v.stretch == StretchPool::kNone || v.takeover != kNone || !clipComplex(match->warp) ||
      match->sample == AudioSequenceImage::kNoSample || match->playStart > s || match->playEnd <= s || free_.empty())
    return false;
  const ClipSample& cs = img_->sample(match->sample);
  if (!stretch_->canClaim(cs.view().channels == 1 ? 1u : 2u)) return false;
  if (cs.pooled != nullptr && cfg_.streams != nullptr && cs.view().streamed() && freeBuffers_.empty()) return false;
  const auto self = static_cast<uint32_t>(&v - voices_.data());
  startVoice(v.slot, *match, s);
  const uint32_t r = live_.back();
  if (voices_[r].stretch == StretchPool::kNone) {  // not expected (canClaim)
    endVoice(static_cast<uint32_t>(live_.size() - 1));
    return false;
  }
  voices_[r].replaces = self;
  v.takeover = r;
  return true;
}

// The song sample the voice's replacement starts sounding at, if before s1 (it gates the replacement's start first,
// as its own turn in play() would: idempotent); INT64_MAX otherwise.
SampleTime ClipPlayer::takeoverAt(Voice& v, SampleTime s1) noexcept {  // RT-safe
  Voice& r = voices_[v.takeover];
  if (r.state == State::Playing) return r.soundFrom;
  if (r.state != State::Pending) return INT64_MAX;
  if (r.vs < s1) gateComplex(r, r.img->sample(r.clip->sample).view());
  return r.vs < s1 ? r.vs : INT64_MAX;
}

void ClipPlayer::unlinkTakeover(Voice& v) noexcept {  // RT-safe
  if (v.takeover != kNone) voices_[v.takeover].replaces = kNone;
  if (v.replaces != kNone) voices_[v.replaces].takeover = kNone;
  v.takeover = v.replaces = kNone;
}

void ClipPlayer::releaseVoice(Voice& v) noexcept {  // RT-safe
  v.state = State::Releasing;
  v.relPos = 0;
  v.relLen = declick_;
}

void ClipPlayer::releaseAll(uint32_t /*frame*/) noexcept {  // RT-safe
  for (uint32_t i = 0; i < live_.size();) {
    Voice& v = voices_[live_[i]];
    if (v.cue) {  // T6.5b: a cue waits for its seek (a loop wrap releases, then seeks to the cue)
      ++i;
      continue;
    }
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

void ClipPlayer::seek(SampleTime s) noexcept {  // RT-safe
  if (cueAt_ != INT64_MIN) {  // T6.5b
    if (s == cueAt_) {
      // The cue replaces any other voice of its clip that still waits or plays (one made before the cue was primed).
      for (uint32_t i = 0; i < live_.size();) {
        const Voice& v = voices_[live_[i]];
        bool dup = false;
        if (!v.cue && v.state != State::Releasing)
          for (uint32_t idx : live_) {
            const Voice& q = voices_[idx];
            if (q.cue && q.slot == v.slot && q.clip == v.clip) {
              dup = true;
              break;
            }
          }
        if (dup) endVoice(i);
        else ++i;
      }
      for (uint32_t idx : live_) {
        Voice& v = voices_[idx];
        if (!v.cue) continue;
        v.cue = false;
        v.primeFrom = s;
        v.primeBase = v.vHi != INT64_MIN ? v.vHi : complexFirstFrame(*v.clip);
        ++stats_.stretchCuesAdopted;
      }
    } else {
      endCues();
    }
    cueAt_ = INT64_MIN;
    cueScanned_ = false;
  }
  seekCursors(s);
}

void ClipPlayer::endCues() noexcept {  // RT-safe
  for (uint32_t i = 0; i < live_.size();) {
    if (voices_[live_[i]].cue) endVoice(i);
    else ++i;
  }
}

void ClipPlayer::cue(SampleTime at, uint32_t frames) noexcept {  // RT-safe
  if (stretch_ == nullptr || img_ == nullptr) return;
  if (at != cueAt_) {
    endCues();
    cueAt_ = at;
    cueScanned_ = false;
    cueFrames_ = 0;
  }
  if (!cueScanned_) {  // once per position (and after a swap): the Complex clips a playback from `at` starts soon
    cueScanned_ = true;
    const uint32_t n = std::min(img_->numSlots(), cfg_.slots);
    for (uint32_t slot = 0; slot < n; ++slot) {
      const std::span<const AudioSequenceImage::Clip> cl = img_->clips(slot);
      auto it = std::upper_bound(cl.begin(), cl.end(), at,
                                 [](SampleTime v, const AudioSequenceImage::Clip& c) noexcept { return v < c.playStart; });
      auto i = static_cast<size_t>(it - cl.begin());
      while (i > 0 && cl[i - 1].playEnd > at) --i;
      for (; i < cl.size() && cl[i].playStart < at + lookahead_; ++i) {
        const AudioSequenceImage::Clip& c = cl[i];
        if (!clipComplex(c.warp) || c.sample == AudioSequenceImage::kNoSample || c.playEnd <= at) continue;
        bool cued = false;
        for (uint32_t idx : live_) {
          const Voice& q = voices_[idx];
          cued = cued || (q.cue && q.slot == slot && q.clip == &c);
        }
        if (cued || free_.empty()) continue;
        const bool mono = img_->sample(c.sample).view().channels == 1;
        if (!stretch_->canClaim(mono ? 1u : 2u)) continue;
        startVoice(slot, c, at);
        voices_[live_.back()].cue = true;
        ++stats_.stretchCues;
      }
    }
  }
  for (uint32_t idx : live_) {
    Voice& v = voices_[idx];
    if (v.cue) advanceComplex(v, v.img->sample(v.clip->sample).view(), at, frames);
  }
  cueFrames_ += frames;
}

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
    if (v.replaces != kNone && v.img == next && prev != next) {  // a handover's new voice, started by this swap
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
      // The start may have moved with the law (a trim); a start inside the clip (T6.5b) stays.
      if (v.state == State::Pending) v.vs = v.vs == v.clip->playStart ? match->playStart : std::max(v.vs, match->playStart);
      if (v.stretch != StretchPool::kNone &&
          (match->formantFactor != v.clip->formantFactor || match->formantBase != v.clip->formantBase))
        stretch_->setParams(v.stretch, StretchParams{1.0, match->formantFactor, false, match->formantBase});
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
    if (handOver(v, match, s)) {
      if (prev != next) ++prevUsers_;
      ++stats_.handovers;
      ++i;
      continue;
    }
    releaseVoice(v);
    ++stats_.faded;
    if (prev != next) ++prevUsers_;
    ++i;
  }
  cueScanned_ = false;  // T6.5b: a cue looks for the new image's clips
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
    if (v.cue) {  // T6.5b: a cue sounds only once a seek adopts it
      ++i;
      continue;
    }
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
      // T6.5b: a Complex voice about to sound: primed far enough, or its start moves to where it will be.
      if (v.vs < s1 && v.state == State::Pending && v.stretch != StretchPool::kNone)
        gateComplex(v, v.img->sample(v.clip->sample).view());
      if (v.vs < s1) {
        const auto off = static_cast<uint32_t>(std::max<SampleTime>(v.vs - s0, 0));
        if (v.state == State::Pending) v.soundFrom = v.vs;
        v.state = State::Playing;
        // T6.x-fix-warp (H2): a Complex voice playing as Tones claims again; once its replacement sounds, it fades out
        // there (the replacement fades in: a 2 ms crossfade).
        // AC.fix: a handed-over voice (a swap changed its law) does the same with the voice of its new clip.
        SampleTime h = INT64_MAX;
        if (v.stretch == StretchPool::kNone && v.takeover == kNone && stretch_ != nullptr && clipComplex(v.clip->warp))
          retryStretch(v, s0);
        if (v.takeover != kNone) h = takeoverAt(v, s1);
        if (h < s1) {
          const auto cut = static_cast<uint32_t>(std::clamp<SampleTime>(h - v.vs, 0, len - off));
          alive = cut == 0 || renderVoice(v, frame + off, cut);
          unlinkTakeover(v);
          if (v.stretch == StretchPool::kNone) ++stats_.stretchRetries;
          if (alive && declick_ != 0) {
            releaseVoice(v);
            alive = renderVoice(v, frame + off + cut, len - off - cut);
          } else {
            alive = false;
          }
        } else {
          alive = renderVoice(v, frame + off, len - off);
        }
      } else if (v.stretch != StretchPool::kNone) {  // T6.5: a pending Complex voice primes in step with the song
        advanceComplex(v, v.img->sample(v.clip->sample).view(), s1, len);
      }
    }
    if (!alive) endVoice(i);
    else ++i;
  }
}

void ClipPlayer::idle(uint32_t frames) noexcept {  // RT-safe
  for (uint32_t i = 0; i < live_.size();) {
    Voice& v = voices_[live_[i]];
    if (v.cue) {  // T6.5b: cues wait
      ++i;
      continue;
    }
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
  if (clipStretches(c.warp)) {
    renderStretch(v, frame, s, n);
    return;
  }
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
    read(v, v.reader, view, pc[k], o, env != nullptr ? env + pc[k].at : nullptr, c.gain, kSlack);
  }
}

void ClipPlayer::read(Voice& v, Reader& r, const media::SampleView& sampleView, const Piece& piece, float* const* out,
                      const float* env, float gain, int64_t keep) noexcept {  // RT-safe
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
  // T6.3: a stretch voice keeps `keep` frames behind its reads and may read a quarter of the history ahead (ensure).
  const int64_t hist = static_cast<int64_t>(cfg_.historyFrames);
  const int64_t cap = hist - 2 * kSlack - (keep > kSlack ? keep + hist / 4 : 0);
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
      const dsp::SincReach rc = dsp::sincReach(*tb, pos, p.step, n);
      first = rc.first;
      last = rc.last;
    }
    float* o[2] = {out[0] + done, out[1] + done};
    const float* e = env != nullptr ? env + done : nullptr;
    done += n;
    if (last <= 0 || first >= view.frames) continue;  // entirely outside the file: silence
    if (copy) stats_.copyFrames += n;
    else stats_.sincFrames += n;
    const float* base[2] = {nullptr, nullptr};
    int64_t baseFrame = 0;
    if (!source(v, r, view, first, last, p.step > 0, keep, base, &baseFrame)) continue;
    if (copy) {
      dsp::gainCopy(base, ch, (pos >> 32) - baseFrame, p.step > 0 ? 1 : -1, n, e, gain, o);
    } else {
      dsp::sincResample(*tb, base, ch, pos - baseFrame * kOne, p.step, n, e, gain, o);
    }
  }
}

bool ClipPlayer::source(Voice& v, Reader& r, const media::SampleView& view, int64_t first, int64_t last, bool forward,
                        int64_t keep, const float** base, int64_t* baseFrame) noexcept {  // RT-safe
  const uint32_t ch = std::min<uint32_t>(view.channels, 2);
  base[0] = view.data[0];
  base[1] = view.data[ch == 2 ? 1 : 0];
  *baseFrame = 0;
  const int64_t G = media::kSampleGuardFrames;
  const int64_t limit = view.streamed() ? view.residentFrames : view.frames + G;
  if (first >= -G && last <= limit) return true;
  const ClipSample& cs = v.img->sample(v.clip->sample);
  if (view.streamed() && cs.pooled != nullptr && cfg_.streams != nullptr)
    return ensure(v, r, view, first, last, forward, keep, base, baseFrame);
  // Gather what the file has into the scratch buffer, zeros around it.
  if (last - first > static_cast<int64_t>(cfg_.historyFrames)) return false;
  const int64_t avail = view.streamed() ? view.residentFrames : view.frames;
  const int64_t a = std::max<int64_t>(first, 0), b = std::min<int64_t>(last, avail);
  for (uint32_t c = 0; c < ch; ++c) {
    float* dst = scratch_.data() + static_cast<size_t>(c) * cfg_.historyFrames;
    std::memset(dst, 0, sizeof(float) * static_cast<size_t>(last - first));
    if (b > a) std::memcpy(dst + (a - first), view.data[c] + a, sizeof(float) * static_cast<size_t>(b - a));
  }
  base[0] = scratch_.data();
  base[1] = scratch_.data() + (ch == 2 ? cfg_.historyFrames : 0u);
  *baseFrame = first;
  return true;
}

bool ClipPlayer::ensure(Voice& v, Reader& r, const media::SampleView& view, int64_t first, int64_t last, bool forward,
                        int64_t keep, const float** base, int64_t* baseFrame) noexcept {  // RT-safe
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
  auto serve = [&]() noexcept {
    base[0] = b[0] + r.off;
    base[1] = ch == 2 ? b[1] + r.off : b[0] + r.off;
    *baseFrame = r.lo;
    return true;
  };
  // T6.3: what the history holds is served in either direction (grains and slice loops read back and forth).
  if (r.started && first >= r.lo && last <= r.hi) return serve();
  const int64_t need = last - first;
  if (need > static_cast<int64_t>(cap)) return false;
  // A stretch voice (keep > kSlack) reads through forward gaps of up to a quarter of the history; what it keeps behind
  // shrinks when a request is large, so the window always fits.
  int64_t gap = keep > kSlack ? static_cast<int64_t>(cap) / 4 : 0;
  if (need + gap > static_cast<int64_t>(cap)) gap = 0;
  const int64_t keepN = std::clamp<int64_t>(std::min<int64_t>(keep, static_cast<int64_t>(cap) - need - gap), 0, keep);
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
    if (!r.started || r.dir != media::Direction::Forward || first < r.lo || first > r.hi + gap) {
      if (!adopt(media::Direction::Forward, first >= r.aheadFrame && first - r.aheadFrame <= cap / 2)) {
        if (r.started) ++stats_.streamRestarts;
        r.cursor.start(*cfg_.streams, sample, first, media::Direction::Forward);
        r.lo = r.hi = first;
      }
      r.dir = media::Direction::Forward;
      r.off = 0;
      r.started = true;
    }
    const int64_t keepFrom = std::clamp(first - keepN, r.lo, r.hi);
    if (static_cast<int64_t>(r.off) + (r.hi - r.lo) + std::max<int64_t>(last - r.hi, 0) > cap) {
      const int64_t count = r.hi - keepFrom;
      for (uint32_t c = 0; c < ch; ++c)
        std::memmove(b[c], b[c] + r.off + (keepFrom - r.lo), sizeof(float) * static_cast<size_t>(count));
      r.off = 0;
      r.lo = keepFrom;
    }
    while (r.hi < last) {
      const auto k = static_cast<uint32_t>(std::min<int64_t>(last - r.hi, media::kMaxSpanFrames));
      if (fill_ != nullptr && r.cursor.streaming()) {
        const int64_t needF = r.hi + k - std::max(r.hi, view.residentFrames);
        if (needF > 0) fill_(fillUser_, r.cursor.stream(), needF);
      }
      float* dst[2] = {b[0] + r.off + (r.hi - r.lo), b[1] + r.off + (r.hi - r.lo)};
      put(r.cursor.next(k), dst);
      r.hi += k;
    }
  } else {
    if (!r.started || r.dir != media::Direction::Reverse || last > r.hi || last < r.lo - gap) {
      if (!adopt(media::Direction::Reverse, last <= r.aheadFrame && r.aheadFrame - last <= cap / 2)) {
        if (r.started) ++stats_.streamRestarts;
        r.cursor.start(*cfg_.streams, sample, last, media::Direction::Reverse);
        r.lo = r.hi = last;
      }
      r.dir = media::Direction::Reverse;
      r.off = cap;
      r.started = true;
    }
    const int64_t keepTo = std::clamp(last + keepN, r.lo, r.hi);
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
        const int64_t needF = r.lo - std::max(r.lo - k, view.residentFrames);
        if (needF > 0) fill_(fillUser_, r.cursor.stream(), needF);
      }
      float* dst[2] = {b[0] + r.off - k, b[1] + r.off - k};
      put(r.cursor.next(k), dst);
      r.off -= k;
      r.lo -= k;
    }
  }
  return serve();
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
    frame = static_cast<int64_t>(std::floor(v.img->frameAtContent(c, static_cast<double>(c.loop.start)))) -
            static_cast<int64_t>(tb.center) - 2 * kSlack;
  } else {
    frame = static_cast<int64_t>(std::floor(v.img->frameAtContent(c, static_cast<double>(c.loop.end)))) -
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
  if (clipStretches(c.warp)) {
    bool fwd = true;
    StreamStart out;
    out.frame = stretchStreamFrame(img, c, vs, &fwd, clipComplex(c.warp));
    out.dir = fwd ? media::Direction::Forward : media::Direction::Reverse;
    return out;
  }
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


// ---- T6.3: the time-stretch modes (audio_sequence.h "Beats, Tones, Texture") -------------------------------------------

namespace {

// How many loop spans p is folded by when a position exactly on a fold boundary belongs to the pass below it (a
// reverse clip's passes are (entry, exit]). RT-safe.
int64_t foldCountBelow(double p, const ContentLoop& l) noexcept {
  if (!contentLooped(l) || p <= static_cast<double>(l.end)) return 0;
  return static_cast<int64_t>(std::ceil((p - static_cast<double>(l.end)) / static_cast<double>(l.end - l.start)));
}

// A 32.32 source position; at pitch rate 1 on a whole frame (a slice / grain then reads as a straight copy). RT-safe.
int64_t stretchFixed(double x, int64_t pitchStep) noexcept {
  return pitchStep == kOne ? static_cast<int64_t>(std::llround(x)) * kOne : toFixed(x);
}

int64_t floorDiv(int64_t a, int64_t b) noexcept {  // RT-safe; b > 0
  return a >= 0 ? a / b : -((-a + b - 1) / b);
}

constexpr double kContentEps = 1e-6;  // ticks: "just before" a slice boundary

// A gain factor clamp(base + (t − t0)·dg, 0, 1) (pq/dsp/warp.h: anchored on a sample, so block independent).
struct Ramp {
  SampleTime t0;
  float base, dg;
};

int32_t rampIndex(SampleTime t, SampleTime t0) noexcept {  // RT-safe
  return static_cast<int32_t>(std::clamp<SampleTime>(t - t0, -(SampleTime{1} << 30), SampleTime{1} << 30));
}

}  // namespace

const float* ClipPlayer::stretchEnvelope(Voice& v, SampleTime s, uint32_t n) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  const bool flat = s >= c.flatStart && s + n <= c.flatEnd && v.inLen == 0 && v.state != State::Releasing && v.rampLen == 0;
  const float* env = nullptr;
  if (!flat) {
    float* e = env_.data();
    for (uint32_t i = 0; i < n; ++i) {
      float g = staticShape(c, s + i);
      g *= v.rampLen != 0 ? v.gainFrom + (c.gain - v.gainFrom) *
                                             std::min(1.0f, static_cast<float>(v.rampPos + i + 1) / static_cast<float>(v.rampLen))
                          : c.gain;
      if (v.inLen != 0) g *= std::min(1.0f, static_cast<float>(v.inPos + i) / static_cast<float>(v.inLen));
      if (v.state == State::Releasing) g *= static_cast<float>(v.relLen - v.relPos - i) / static_cast<float>(v.relLen);
      e[i] = g;
    }
    env = e;
  }
  if (v.inLen != 0) {
    v.inPos += n;
    if (v.inPos >= v.inLen) v.inLen = v.inPos = 0;
  }
  if (v.rampLen != 0) {
    v.rampPos += n;
    if (v.rampPos >= v.rampLen) v.rampLen = v.rampPos = 0;
  }
  return env;
}

void ClipPlayer::renderStretch(Voice& v, uint32_t frame, SampleTime s, uint32_t n) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  const media::SampleView& view = v.img->sample(c.sample).view();
  const float* env = stretchEnvelope(v, s, n);
  if (view.channels == 0 || view.frames <= 0) return;
  float* ch0 = touch(v.slot);
  float* out[2] = {ch0 + frame, ch0 + cfg_.maxBlockFrames + frame};
  if (c.warp == ClipWarp::Beats) renderSlices(v, view, s, n, out, env);
  else if (v.stretch != StretchPool::kNone) renderComplex(v, view, s, n, out, env);
  else renderGrains(v, view, s, n, out, env);  // Tones / Texture, and Complex without a stretcher (Tones)
}

// ---- Tones / Texture ----------------------------------------------------------------------------------------------

void ClipPlayer::renderGrains(Voice& v, const media::SampleView& view, SampleTime s, uint32_t n, float* const* out,
                              const float* env) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  const auto H = static_cast<int64_t>(c.hop), N = static_cast<int64_t>(c.grain);
  const SampleTime g0 = c.bodyStart - H;  // grain k covers [g0 + k·H, g0 + k·H + N)
  const int64_t kLo = floorDiv(s - g0 - N, H) + 1;
  const int64_t kHi = floorDiv(s + n - 1 - g0, H);
  const int64_t dir = c.reverse ? -1 : 1;
  const int64_t step = dir * c.pitchStep;
  const double hr = static_cast<double>(H) * c.pitchRate;
  const float* win = v.img->window(c);
  const int64_t keep = static_cast<int64_t>(cfg_.historyFrames) / 2;
  for (int64_t k = kLo; k <= kHi; ++k) {
    const double P = grainCentre(v, view, k);
    const SampleTime gk = g0 + k * H;
    const SampleTime a = std::max(s, gk), b = std::min(s + n, gk + N);
    if (a >= b) continue;
    // The grain reads P + dir·(t − C_k)·r: anchored at its first sample (it depends on the grain alone).
    const double a0 = P - static_cast<double>(dir) * hr;
    const int64_t anchor = c.alignLags != 0 ? toFixed(a0) : stretchFixed(a0, c.pitchStep);
    Piece p;
    p.at = static_cast<uint32_t>(a - s);
    p.frames = static_cast<uint32_t>(b - a);
    p.pos = static_cast<int64_t>(static_cast<uint64_t>(anchor) + static_cast<uint64_t>(a - gk) * static_cast<uint64_t>(step));
    p.step = step;
    p.wrap = false;
    float* gb = gain_.data() + p.at;
    dsp::warpWindowGain(win + (a - gk), env != nullptr ? env + p.at : nullptr, c.gain, gb, p.frames);
    float* o[2] = {out[0] + p.at, out[1] + p.at};
    read(v, v.reader, view, p, o, gb, c.gain, keep);
  }
}

// The source frame grain k is centred on: the warp law at its centre (Texture: jittered), Tones: moved by the lag that
// continues grain k − 1. Grains are positioned once, in order (the voice keeps the last two).
double ClipPlayer::grainCentre(Voice& v, const media::SampleView& view, int64_t k) noexcept {  // RT-safe
  if (v.grainIdx != INT64_MIN && k <= v.grainIdx && k >= v.grainIdx - 1) return v.grainPos[k & 1];
  const AudioSequenceImage::Clip& c = *v.clip;
  const int64_t from = v.grainIdx != INT64_MIN && k > v.grainIdx && k - v.grainIdx <= 64 ? v.grainIdx + 1 : k;
  for (int64_t j = from; j <= k; ++j) {
    const SampleTime centre = c.bodyStart + j * static_cast<int64_t>(c.hop);
    double P = v.img->warpFrame(c, centre);
    if (c.jitter > 0.0) P += dsp::warpJitter(c.seed, j) * c.jitter;
    const int64_t q = std::llround(P);
    // Tones: fractional (an aligned grain continues the one before to a fraction of a frame); Texture: whole frames.
    P = c.alignLags != 0 && v.grainIdx == j - 1 ? alignGrain(v, view, v.grainPos[(j - 1) & 1], q) : static_cast<double>(q);
    v.grainPos[j & 1] = P;
    v.grainIdx = j;
    ++stats_.grains;
  }
  return v.grainPos[k & 1];
}

// Tones (WSOLA): the centre near p whose overlap with grain k − 1 (centred at prev) continues its waveform best: the
// mono sum of the source over the overlap, compared at a coarse stride (≈ 12 kHz) over ±alignLags, then refined frame
// by frame around the best coarse lag.
double ClipPlayer::alignGrain(Voice& v, const media::SampleView& view, double prev, int64_t p) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  const int64_t dir = c.reverse ? -1 : 1;
  const auto hr = static_cast<int64_t>(std::llround(static_cast<double>(c.hop) * c.pitchRate));
  const auto Lc = static_cast<int64_t>(c.alignLen), D = static_cast<int64_t>(c.alignLags);
  const int64_t prevI = std::llround(prev);
  // The match is between whole frames: the exact continuation is prev + dir·H·r, so the fractions are added back.
  const double fix = (prev - static_cast<double>(prevI)) +
                     static_cast<double>(dir) * (static_cast<double>(c.hop) * c.pitchRate - static_cast<double>(hr));
  // Grain k − 1 reads [prev, prev + hr) over the overlap (reverse: below prev), grain k [p − hr, p) (reverse: above p).
  const int64_t refStart = (dir > 0 ? prevI : prevI - hr) + (hr - Lc) / 2;
  const int64_t candStart = (dir > 0 ? p - hr : p) + (hr - Lc) / 2;
  const auto stride = static_cast<int64_t>(std::clamp<uint32_t>(view.sampleRate / 12000u, 1u, 8u));
  const int64_t Dc = D / stride * stride;
  const auto refN = static_cast<uint32_t>(Lc / stride);
  const auto lags = static_cast<uint32_t>(2 * Dc / stride + 1);
  const uint32_t ch = std::min<uint32_t>(view.channels, 2);
  const int64_t keep = static_cast<int64_t>(cfg_.historyFrames) / 2;
  const float* base[2] = {nullptr, nullptr};
  int64_t bf = 0;
  const auto pd = static_cast<double>(p);
  if (refN < 4) return pd;
  if (!source(v, v.reader, view, refStart, refStart + (refN - 1) * stride + 1, dir > 0, keep, base, &bf)) return pd;
  dsp::warpMonoDecimate(base, ch, refStart - bf, static_cast<uint32_t>(stride), refN, corrA_.data());
  const int64_t cFirst = candStart - Dc;
  const uint32_t candN = refN + lags - 1;
  if (!source(v, v.reader, view, cFirst, cFirst + (static_cast<int64_t>(candN) - 1) * stride + 1, dir > 0, keep, base, &bf))
    return pd;
  dsp::warpMonoDecimate(base, ch, cFirst - bf, static_cast<uint32_t>(stride), candN, corrB_.data());
  int64_t d = static_cast<int64_t>(dsp::warpBestLag(corrA_.data(), corrB_.data(), refN, lags, static_cast<uint32_t>(Dc / stride))) *
                  stride - Dc;
  // Frame by frame around it (a frame further each way, so the parabola has both neighbours), then the fraction.
  const int64_t lo = std::max(-D, d - stride), hi = std::min(D, d + stride);
  const auto len = static_cast<uint32_t>(Lc);
  const double coarse = pd + static_cast<double>(d) + fix;
  if (!source(v, v.reader, view, refStart, refStart + Lc, dir > 0, keep, base, &bf)) return coarse;
  dsp::warpMonoDecimate(base, ch, refStart - bf, 1, len, corrA_.data());
  const auto fineN = static_cast<uint32_t>(Lc + (hi - lo));
  if (!source(v, v.reader, view, candStart + lo, candStart + lo + fineN, dir > 0, keep, base, &bf)) return coarse;
  dsp::warpMonoDecimate(base, ch, candStart + lo - bf, 1, fineN, corrB_.data());
  double frac = 0.0;
  d = lo + static_cast<int64_t>(dsp::warpBestLag(corrA_.data(), corrB_.data(), len, static_cast<uint32_t>(hi - lo + 1),
                                                 static_cast<uint32_t>(d - lo), &frac));
  return pd + static_cast<double>(d) + frac + fix;
}

// ---- Beats -----------------------------------------------------------------------------------------------------------

ClipPlayer::SlicePass ClipPlayer::passFromU(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c,
                                            double u) noexcept {  // RT-safe
  constexpr double kInf = std::numeric_limits<double>::infinity();
  const double cs = static_cast<double>(c.contentStart), len = static_cast<double>(c.length);
  const double span = c.looped ? static_cast<double>(c.loop.end - c.loop.start) : 0.0;
  // Forward passes are [entry, exit), reverse ones (entry, exit] (their first sample is the exit).
  const int64_t m = c.reverse ? foldCountBelow(u, c.loop) : foldCount(u, c.loop);
  const double q = u - static_cast<double>(m) * span;
  const std::span<const double> B = img.sliceContent(c), Fr = img.sliceFrame(c);
  const auto S = static_cast<int64_t>(B.size());
  const int64_t j = (c.reverse ? std::lower_bound(B.begin(), B.end(), q) : std::upper_bound(B.begin(), B.end(), q)) - B.begin() - 1;
  const double shift = static_cast<double>(m) * span;
  SlicePass p;
  // The forward-time bounds in this pass (unfolded), clamped to the clip's content.
  double entryU = j >= 0 ? B[static_cast<size_t>(j)] + shift : -kInf;
  double Fentry = j >= 0 ? Fr[static_cast<size_t>(j)] : 0.0;
  const bool low = entryU <= cs;  // the slice reaches the content start
  if (low) {
    entryU = cs;
    Fentry = img.frameAtContent(c, cs - shift);
  }
  const bool hasExit = j + 1 < S;
  double exitU = hasExit ? B[static_cast<size_t>(j + 1)] + shift : kInf;
  const double Fexit = hasExit ? Fr[static_cast<size_t>(j + 1)] : kInf;
  p.entryU = entryU;
  p.exitU = exitU;
  if (!c.reverse) {
    if (low) {  // the clip's first slice: from where the clip starts playing (a crossfade starts it early)
      p.T = c.playStart;
      p.F = img.warpFrame(c, c.playStart);
      p.fadeIn = false;
    } else {
      p.T = img.sampleAtOffset(c, entryU - cs);
      p.F = Fentry;
      p.fadeIn = true;
    }
    p.next = hasExit ? img.sampleAtOffset(c, exitU - cs) : INT64_MAX;
    p.L = hasExit ? Fexit - p.F : kInf;
    p.dir = 1;
  } else {
    if (exitU >= cs + len) {  // the clip's first slice in reverse time
      p.T = c.playStart;
      p.F = img.warpFrame(c, c.playStart);
      p.fadeIn = false;
    } else {
      p.T = img.sampleAtOffset(c, len - (exitU - cs));
      p.F = Fexit;
      p.fadeIn = true;
    }
    p.next = low ? INT64_MAX : img.sampleAtOffset(c, len - (entryU - cs));
    p.L = p.F - Fentry;
    p.dir = -1;
  }
  if (p.fadeIn && p.T <= c.playStart) {
    p.T = c.playStart;
    p.fadeIn = false;
  }
  if (p.next != INT64_MAX && p.next <= p.T) p.next = p.T + 1;
  if (!(p.L >= 0.0)) p.L = 0.0;
  return p;
}

ClipPlayer::SlicePass ClipPlayer::nextPass(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c,
                                           const SlicePass& p) noexcept {  // RT-safe
  return passFromU(img, c, c.reverse ? p.entryU : p.exitU);
}

bool ClipPlayer::prevPass(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c, const SlicePass& p,
                          SlicePass* out) noexcept {  // RT-safe
  if (!p.fadeIn) return false;
  *out = passFromU(img, c, c.reverse ? p.exitU + kContentEps : p.entryU - kContentEps);
  return out->T < p.T;
}

ClipPlayer::SlicePass ClipPlayer::passAt(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c,
                                         SampleTime s) noexcept {  // RT-safe
  const double cs = static_cast<double>(c.contentStart), len = static_cast<double>(c.length);
  double u = img.contentAt(c, s);
  u = c.reverse ? std::min(u, cs + len) : std::max(u, cs);
  SlicePass p = passFromU(img, c, u);
  // The content ↔ sample conversions round: settle on the pass whose [T, next) holds s.
  for (int i = 0; i < 8; ++i) {
    SlicePass q;
    if (s < p.T && prevPass(img, c, p, &q)) p = q;
    else if (p.next != INT64_MAX && s >= p.next) p = nextPass(img, c, p);
    else break;
  }
  return p;
}

ClipPlayer::Reader& ClipPlayer::passReader(Voice& v, const SlicePass& p, SampleTime /*s*/) noexcept {  // RT-safe
  if (v.reader.pass == p.T) return v.reader;
  if (v.reader2.pass == p.T) return v.reader2;
  // The reader of the oldest pass (the one before the pass still fading out has ended).
  Reader& r = v.reader.pass == INT64_MIN ? v.reader
              : v.reader2.pass == INT64_MIN ? v.reader2
              : (v.reader.pass < v.reader2.pass ? v.reader : v.reader2);
  r.pass = p.T;
  return r;
}

void ClipPlayer::renderSlices(Voice& v, const media::SampleView& view, SampleTime s, uint32_t n, float* const* out,
                              const float* env) noexcept {  // RT-safe
  const AudioSequenceImage& img = *v.img;
  const AudioSequenceImage::Clip& c = *v.clip;
  if (c.sliceCount == 0) return;
  const SampleTime X = img.sliceFadeFrames();
  SlicePass p = passAt(img, c, s);
  SlicePass q;
  if (s < p.T + X && prevPass(img, c, p, &q)) renderPass(v, view, q, s, n, out, env);  // still fading out
  for (uint32_t guard = 0; guard < 4096; ++guard) {
    renderPass(v, view, p, s, n, out, env);
    if (p.next == INT64_MAX || p.next >= s + n) break;
    p = nextPass(img, c, p);
  }
  claimSlice(v, view, p, s + n);
}

void ClipPlayer::renderPass(Voice& v, const media::SampleView& view, const SlicePass& p, SampleTime s, uint32_t n,
                            float* const* out, const float* env) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  const SampleTime Xi = v.img->sliceFadeFrames();
  const auto X = static_cast<float>(Xi);
  const SampleTime audEnd = p.next == INT64_MAX ? INT64_MAX : p.next + Xi;
  const SampleTime a0 = std::max(s, p.T), b0 = std::min(s + n, audEnd);
  if (a0 >= b0) return;
  ++stats_.slicePasses;
  const double r = c.pitchRate;
  const int64_t step = p.dir * c.pitchStep;
  const bool finite = std::isfinite(p.L);
  const double tE = finite ? p.L / r : 0.0;  // the natural end, output frames after T
  const double Lh = 0.5 * p.L;
  const double lam = finite ? Lh / r : 0.0;
  // A slice the next one cuts before (or within the crossfade of) its natural end plays on naturally through its
  // fade-out: at the source's own tempo the two read the same frames there, so the crossfade is seamless.
  const SampleTime natEnd0 = finite ? p.T + static_cast<SampleTime>(std::ceil(tE)) : INT64_MAX;
  const bool cut = !finite || (p.next != INT64_MAX && natEnd0 >= p.next - Xi);
  const bool loops = !cut && c.sliceLoop != SliceLoop::Off && lam >= 2.0 * static_cast<double>(Xi);
  const SampleTime natEnd = cut ? INT64_MAX : natEnd0;
  Reader& rd = passReader(v, p, s);
  const int64_t keep = static_cast<int64_t>(cfg_.historyFrames) / 2;
  const float envGd = c.envelope < 100.0f && p.next != INT64_MAX
                          ? std::max(1.0f, static_cast<float>(p.next - p.T) * (1.0f - c.envelope / 100.0f))
                          : 0.0f;
  // Renders [a, b) reading from 32.32 `pos` by `st`, with the pass's gains and `extra` ramps.
  auto emit = [&](SampleTime a, SampleTime b, int64_t pos, int64_t st, const Ramp* extra, int nExtra) noexcept {
    const auto len = static_cast<uint32_t>(b - a);
    Ramp rs[8];
    int nr = 0;
    auto add = [&](Ramp q) noexcept {
      const float first = dsp::rampValue(q.base, q.dg, rampIndex(a, q.t0));
      const float last = dsp::rampValue(q.base, q.dg, rampIndex(b - 1, q.t0));
      if (std::min(first, last) >= 1.0f) return true;   // 1 over the piece: no factor
      if (std::max(first, last) <= 0.0f) return false;  // 0 over the piece: silent
      rs[nr++] = q;
      return true;
    };
    bool audible = true;
    if (p.fadeIn) audible = audible && add({p.T, 0.5f / X, 1.0f / X});
    if (p.next != INT64_MAX) audible = audible && add({p.next, (X - 0.5f) / X, -1.0f / X});
    if (envGd > 0.0f) audible = audible && add({p.next, 0.0f, -1.0f / envGd});
    for (int i = 0; i < nExtra; ++i) audible = audible && add(extra[i]);
    if (!audible) return;
    const auto at = static_cast<uint32_t>(a - s);
    const float* g = env != nullptr ? env + at : nullptr;
    if (nr > 0) {
      float* gb = gain_.data() + at;
      dsp::warpRampGain(rs[0].base, rs[0].dg, rampIndex(a, rs[0].t0), g, c.gain, gb, len);
      for (int i = 1; i < nr; ++i) dsp::warpRampMul(rs[i].base, rs[i].dg, rampIndex(a, rs[i].t0), gb, len);
      g = gb;
    }
    Piece pc;
    pc.at = at;
    pc.frames = len;
    pc.pos = pos;
    pc.step = st;
    pc.wrap = false;
    float* o[2] = {out[0] + at, out[1] + at};
    read(v, rd, view, pc, o, g, c.gain, keep);
  };
  // The natural part: from F at the pitch rate, anchored at the entry.
  {
    const SampleTime a = a0, b = std::min(b0, natEnd);
    if (a < b) {
      const int64_t pos = static_cast<int64_t>(static_cast<uint64_t>(stretchFixed(p.F, c.pitchStep)) +
                                               static_cast<uint64_t>(a - p.T) * static_cast<uint64_t>(step));
      Ramp ex[1];
      int ne = 0;
      // Off: fade out into the natural end; Forward: declick into the first loop jump.
      if (!cut && (!loops || c.sliceLoop == SliceLoop::Forward))
        ex[ne++] = {p.T, static_cast<float>((tE - 0.5) / static_cast<double>(Xi)), -1.0f / X};
      emit(a, b, pos, step, ex, ne);
    }
  }
  if (!loops) return;
  // Loop iterations over the slice's second half: iteration i starts tE + i·λ after the entry.
  const double from = std::max(0.0, std::floor((static_cast<double>(a0 - p.T) - tE) / lam));
  for (auto i = static_cast<int64_t>(from); i < static_cast<int64_t>(from) + 64; ++i) {
    const double ti0 = tE + static_cast<double>(i) * lam, ti1 = tE + static_cast<double>(i + 1) * lam;
    const SampleTime t0 = p.T + static_cast<SampleTime>(std::ceil(ti0)), t1 = p.T + static_cast<SampleTime>(std::ceil(ti1));
    if (t0 >= b0) break;
    const SampleTime a = std::max(a0, t0), b = std::min(b0, t1);
    if (a >= b) continue;
    const double frac = std::ceil(ti0) - ti0;  // output frames between the iteration's start and its first sample
    double off = Lh + frac * r;
    int64_t st = step;
    if (c.sliceLoop == SliceLoop::BackForward && (i & 1) == 0) {  // even iterations run back from the end
      off = p.L - frac * r;
      st = -step;
    }
    const int64_t anchor = stretchFixed(p.F + static_cast<double>(p.dir) * off, c.pitchStep);
    const int64_t pos = static_cast<int64_t>(static_cast<uint64_t>(anchor) + static_cast<uint64_t>(a - t0) * static_cast<uint64_t>(st));
    Ramp ex[2];
    int ne = 0;
    if (c.sliceLoop == SliceLoop::Forward) {  // declick both sides of each jump
      ex[ne++] = {t0, static_cast<float>((static_cast<double>(t0 - p.T) - ti0 + 0.5) / static_cast<double>(Xi)), 1.0f / X};
      ex[ne++] = {t0, static_cast<float>((ti1 - static_cast<double>(t0 - p.T) - 0.5) / static_cast<double>(Xi)), -1.0f / X};
    }
    emit(a, b, pos, st, ex, ne);
  }
}

// A streamed Beats voice: the next slice's stream is claimed on the idle reader `lookaheadSeconds` before it starts
// (once the slice before the current one has faded out), so the jump finds its frames fetched.
void ClipPlayer::claimSlice(Voice& v, const media::SampleView& view, const SlicePass& cur, SampleTime runEnd) noexcept {  // RT-safe
  if (!view.streamed() || cfg_.streams == nullptr || cur.next == INT64_MAX || cur.next - runEnd > lookahead_) return;
  const AudioSequenceImage::Clip& c = *v.clip;
  const ClipSample& cs = v.img->sample(c.sample);
  if (cs.pooled == nullptr || runEnd < cur.T + static_cast<SampleTime>(v.img->sliceFadeFrames())) return;
  const SlicePass np = nextPass(*v.img, c, cur);
  if (v.reader.pass == np.T || v.reader2.pass == np.T) return;
  Reader& r = v.reader.pass == cur.T ? v.reader2 : v.reader;
  if (r.buf == kNone) {
    if (freeBuffers_.empty()) return;
    r.buf = freeBuffers_.back();
    freeBuffers_.pop_back();
  }
  const dsp::SincTable& tb = dsp::sincTableAtLeast(dsp::ResampleQuality::High, dsp::sincStretchQ(c.pitchRate));
  const int64_t reach = static_cast<int64_t>(tb.taps) + 2 * kSlack;
  const bool fwd = np.dir > 0;
  int64_t frame = fwd ? static_cast<int64_t>(std::floor(np.F)) - reach : static_cast<int64_t>(std::ceil(np.F)) + reach;
  frame = std::clamp<int64_t>(frame, 0, view.frames);
  r.aheadValid = false;
  r.ahead.stop();
  r.cursor.start(*cfg_.streams, *cs.pooled, frame, fwd ? media::Direction::Forward : media::Direction::Reverse);
  r.dir = fwd ? media::Direction::Forward : media::Direction::Reverse;
  r.lo = r.hi = frame;
  r.off = fwd ? 0u : cfg_.historyFrames;
  r.started = true;
  r.pass = np.T;
  ++stats_.sliceClaims;
}

// ---- T6.5: Complex / Complex Pro (audio_sequence.h "Complex") ----------------------------------------------------------

namespace {

constexpr auto kChunk = static_cast<int64_t>(ClipPlayer::kStretchCell);
constexpr auto kVoc = static_cast<int64_t>(ClipPlayer::kStretchFrames);

// A looped clip's loop in whole source frames [ls, le) (on: a loop of at least one frame).
struct LoopFrames {
  bool on = false;
  int64_t ls = 0, le = 0, span = 0;
};

LoopFrames loopFrames(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c) noexcept {  // RT-safe
  LoopFrames L;
  if (!c.looped) return L;
  L.ls = std::llround(img.frameAtContent(c, static_cast<double>(c.loop.start)));
  L.le = std::llround(img.frameAtContent(c, static_cast<double>(c.loop.end)));
  L.span = L.le - L.ls;
  L.on = L.span > 0;
  return L;
}

// φ(s): the law's source frame at s unfolded through the loop wraps (each fold adds the loop's span), so a stretcher
// is fed one continuous stream; foldFrame maps a stream frame back to the file. RT-safe.
double unfoldedFrame(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c, const LoopFrames& L,
                     SampleTime s) noexcept {
  const double u = img.contentAt(c, s);
  if (!L.on) return img.frameAtContent(c, foldContent(u, c.loop));
  return img.frameAtContent(c, foldContent(u, c.loop)) + static_cast<double>(foldCount(u, c.loop) * L.span);
}

// φ at a fractional song time (linear between samples). RT-safe.
double unfoldedAt(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c, const LoopFrames& L, double t) noexcept {
  const double f = std::floor(t);
  const auto s = static_cast<SampleTime>(f);
  const double a = unfoldedFrame(img, c, L, s);
  return t == f ? a : a + (t - f) * (unfoldedFrame(img, c, L, s + 1) - a);
}

int64_t foldFrame(int64_t phi, const LoopFrames& L) noexcept {  // RT-safe
  if (!L.on || phi < L.le) return phi;
  return L.ls + (phi - L.le) % L.span;
}

double pitchRateOf(const AudioSequenceImage::Clip& c) noexcept { return static_cast<double>(c.pitchStep) / 4294967296.0; }

// The vocoder frames the resampler reads before the one a song sample maps to (0 for a copy at rate 1). RT-safe.
int64_t complexReach(const AudioSequenceImage::Clip& c) noexcept {
  if (c.pitchStep == kOne) return 0;
  return static_cast<int64_t>(
             dsp::sincTableAtLeast(dsp::ResampleQuality::High, dsp::sincStretchQ(pitchRateOf(c))).taps) + 2;
}

// A voice primed for its first output at song sample t0 (tA = t0): its vocoder starts at frame ys (an output latency
// plus a whole block and the resampler's reach before frame 0, so every synthesis frame frame 0 overlaps is computed);
// its stream's frame 0 is phi0 = φ(t(ys + outputLatency)); stream frames below zeroBelow (before the clip plays) are
// zeros; firstFrame: where its first file read starts (forward) or ends (reverse), for the stream claim.
struct ComplexStart {
  int64_t ys = 0, phi0 = 0, zeroBelow = 0, firstFrame = 0;
};

// M6 review M10: how many vocoder frames the clip's voice keeps computed ahead of what its reads need, within one
// analysis interval, from a golden-ratio sequence over its seed (consecutive seeds land far apart). The vocoder's
// frames do not depend on when they are computed, so this moves only the blocks the work lands in: voices started on
// the same sample do their heavy spectrum steps in different blocks, and the audio is the same. RT-safe.
int64_t stretchLead(const AudioSequenceImage::Clip& c, const StretchPool::Latency& lat) noexcept {
  const double g = static_cast<double>(c.seed) * 0.6180339887498949;
  return std::min<int64_t>(static_cast<int64_t>((g - std::floor(g)) * static_cast<double>(lat.interval)),
                           std::max(lat.interval - 1, 0));
}

int64_t primeStart(const AudioSequenceImage::Clip& c, const StretchPool::Latency& lat) noexcept {  // RT-safe
  return floorDiv(-(lat.output + lat.block + complexReach(c)), kChunk) * kChunk;
}

// T6.5b, L14: vocoder frames a voice may prime per song sample: kStretchPrimeRate × max(1, its pitch rate). RT-safe.
double primeRate(double pitchRate) noexcept {
  return static_cast<double>(ClipPlayer::kStretchPrimeRate) * std::max(1.0, pitchRate);
}

ComplexStart complexStart(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c, SampleTime t0,
                          const StretchPool::Latency& lat) noexcept {  // RT-safe
  const LoopFrames L = loopFrames(img, c);
  const int64_t dir = c.reverse ? -1 : 1;
  const double r = pitchRateOf(c);
  ComplexStart st;
  st.ys = primeStart(c, lat);
  st.phi0 = std::llround(unfoldedAt(img, c, L, static_cast<double>(t0) + static_cast<double>(st.ys + lat.output) / r));
  st.zeroBelow = dir * (std::llround(unfoldedFrame(img, c, L, c.playStart)) - st.phi0);
  const int64_t src = foldFrame(st.phi0 + dir * std::max<int64_t>(lat.input - lat.seek, st.zeroBelow), L);
  const int64_t frames = img.sample(c.sample).view().frames;
  st.firstFrame = std::clamp<int64_t>(dir > 0 ? src : src + 1, 0, std::max<int64_t>(frames, 0));
  return st;
}

}  // namespace

void ClipPlayer::primeComplex(Voice& v, const media::SampleView& view, SampleTime t0) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  const StretchPool::Latency& lat = stretch_->latency();
  const ComplexStart st = complexStart(*v.img, c, t0, lat);
  stretch_->reset(v.stretch, c.seed);
  stretch_->setParams(v.stretch, StretchParams{1.0, c.formantFactor, false, c.formantBase});
  v.tA = t0;
  v.phi0 = st.phi0;
  v.zeroBelow = st.zeroBelow;
  complexInput(v, view, lat.input - lat.seek, lat.input);
  const LoopFrames L = loopFrames(*v.img, c);
  const double r = pitchRateOf(c);
  const double q = static_cast<double>(t0) + static_cast<double>(st.ys + lat.output) / r;
  const double rate = std::abs(unfoldedAt(*v.img, c, L, q + static_cast<double>(kChunk) / r) - unfoldedAt(*v.img, c, L, q)) /
                      static_cast<double>(kChunk);
  const float* in[2] = {stretchIn_.data(), stretchIn_.data() + (view.channels >= 2 ? stretchInCap_ : 0u)};
  stretch_->seek(v.stretch, in, lat.seek, rate);  // a mono stretcher reads in[0] only
  v.head = lat.input;
  v.vLo = v.vHi = st.ys;
}

int64_t ClipPlayer::complexFirstFrame(const AudioSequenceImage::Clip& c) const noexcept {  // RT-safe
  return primeStart(c, stretch_->latency());
}

// A pending voice: its vocoder runs in step with the song from its first chunk's time on (so a clip's start costs no
// more than its playing), keeping what the first read will need; never ahead of its priming schedule (T6.5b: at most
// kStretchPrimeRate × its playing rate per song sample, so no block does a burst). A cue voice (time stands still,
// or is elsewhere): up to what its start reads, `frames` × kStretchPrimeRate × its playing rate per call.
void ClipPlayer::advanceComplex(Voice& v, const media::SampleView& view, SampleTime upTo, uint32_t frames) noexcept {  // RT-safe
  if (view.channels == 0 || view.frames <= 0) return;
  const AudioSequenceImage::Clip& c = *v.clip;
  const double r = pitchRateOf(c);
  const double k = primeRate(r);
  const int64_t lead = stretchLead(c, stretch_->latency());
  if (v.vHi == INT64_MIN) {
    const int64_t ys = complexFirstFrame(c);
    // M10: voices that would start priming on the same sample (a cue's, clips starting together) are staggered by
    // their lead, so their stretchers' seeks and first chunks do not land in one block.
    if (v.cue ? cueFrames_ < lead
              : static_cast<double>(upTo) < static_cast<double>(v.vs) + static_cast<double>(ys + kChunk - lead) / r)
      return;
    primeComplex(v, view, v.vs);
  }
  const int64_t reach = complexReach(c);
  const auto yStart = static_cast<int64_t>(std::floor(static_cast<double>(v.vs - v.tA) * r));
  int64_t target = 0;
  if (v.cue) {
    const auto budget = static_cast<int64_t>(std::ceil(static_cast<double>(frames) * k / static_cast<double>(kChunk)));
    target = std::min(yStart + reach + kChunk + lead, v.vHi + budget * kChunk);
  } else {
    target = static_cast<int64_t>(std::floor(static_cast<double>(upTo - v.tA) * r)) + lead;
    if (fill_ == nullptr)
      target = std::min(target, static_cast<int64_t>(std::floor(
                                    static_cast<double>(v.primeBase) + static_cast<double>(upTo - v.primeFrom) * k)));
  }
  if (target > v.vHi) complexEnsure(v, view, yStart - reach, target);
}

// A pending voice due to sound at v.vs. Its first block reads up to a block plus the resampler's reach past y(vs),
// and keeps its lead (stretchLead) computed ahead: it sounds at vs if its schedule had reached y(vs) + reach + a chunk
// + lead by then (the rest is that block's own work, at most (kStretchPrimeRate + 1) × a playing block's); otherwise it
// starts at the first song sample t whose schedule has, primeBase + (t − primeFrom)·k ≥ (t − tA)·r + reach + chunk +
// lead (k = primeRate(r)), faded in. Song-time arithmetic only: the start does not depend on the engine block. Offline
// renders (a stream fill is set) prime at once instead.
void ClipPlayer::gateComplex(Voice& v, const media::SampleView& view) noexcept {  // RT-safe
  if (fill_ != nullptr || view.channels == 0 || view.frames <= 0) return;
  const AudioSequenceImage::Clip& c = *v.clip;
  const double r = pitchRateOf(c);
  const double k = primeRate(r);
  const int64_t reach = complexReach(c);
  if (v.vHi == INT64_MIN) {
    primeComplex(v, view, v.vs);  // a reset and a seek (an input copy): no vocoder work
  } else if (static_cast<int64_t>(std::floor(static_cast<double>(v.vs - v.tA) * r)) - reach < v.vLo) {
    primeComplex(v, view, v.vs);  // its clock left what was primed (a swap moved its start behind): start afresh
    v.primeFrom = v.vs;
    v.primeBase = v.vHi;
  }
  const double R = static_cast<double>(reach + kChunk + stretchLead(c, stretch_->latency()));
  const double tA = static_cast<double>(v.tA), from = static_cast<double>(v.primeFrom);
  const double base = static_cast<double>(v.primeBase);
  const double vs = static_cast<double>(v.vs);
  if (base + (vs - from) * k >= (vs - tA) * r + R) return;
  const double t = (R - base + k * from - r * tA) / (k - r);
  v.vs = std::max(v.vs + 1, static_cast<SampleTime>(std::ceil(t)));
  v.inLen = declick_;
  v.inPos = 0;
  ++stats_.stretchLateStarts;
}

void ClipPlayer::complexEnsure(Voice& v, const media::SampleView& view, int64_t keepFrom, int64_t last) noexcept {  // RT-safe
  float* b0 = stretch_->output(v.stretch);
  float* b1 = b0 + kStretchFrames;
  const bool stereo = stretch_->channels(v.stretch) == 2;
  while (v.vHi < last) {
    if (v.vHi + kChunk - v.vLo > kVoc) {  // make room: drop what is below keepFrom
      const int64_t from = std::clamp(keepFrom, v.vLo, v.vHi);
      const auto keep = static_cast<size_t>(v.vHi - from);
      std::memmove(b0, b0 + (from - v.vLo), sizeof(float) * keep);
      if (stereo) std::memmove(b1, b1 + (from - v.vLo), sizeof(float) * keep);
      v.vLo = from;
      if (v.vHi + kChunk - v.vLo > kVoc) v.vLo = v.vHi;  // a read wider than the buffer (runs are cut to fit): none
    }
    complexChunk(v, view);
  }
}

// The vocoder frames [vHi, vHi + kStretchCell): its input up to the head they want (φ(t(end + outputLatency)) +
// inputLatency).
void ClipPlayer::complexChunk(Voice& v, const media::SampleView& view) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  const StretchPool::Latency& lat = stretch_->latency();
  const LoopFrames L = loopFrames(*v.img, c);
  const int64_t dir = c.reverse ? -1 : 1;
  const double tEnd = static_cast<double>(v.tA) + static_cast<double>(v.vHi + kChunk + lat.output) / pitchRateOf(c);
  const int64_t target = dir * (std::llround(unfoldedAt(*v.img, c, L, tEnd)) - v.phi0) + lat.input;
  const int64_t inN = std::clamp<int64_t>(target - v.head, 0, int64_t{1} << 24);
  const int64_t lead = std::max<int64_t>(0, inN - static_cast<int64_t>(stretchInCap_));
  complexInput(v, view, v.head + lead, v.head + inN);
  const float* in[2] = {stretchIn_.data(), stretchIn_.data() + (view.channels >= 2 ? stretchInCap_ : 0u)};
  float* b0 = stretch_->output(v.stretch) + (v.vHi - v.vLo);
  float* out[2] = {b0, b0 + kStretchFrames};
  stretch_->process(v.stretch, in, static_cast<int>(inN), static_cast<int>(lead), out, static_cast<int>(kChunk));
  v.head += inN;
  v.vHi += kChunk;
  ++stats_.stretchCells;
  ++blockCells_;
}

// The voice's stream frames [k0, k1) into stretchIn_ (frame k0 at index 0): folded into the file through the loop,
// read through source() (in place, or the voice's stream), zeros outside the file and before the clip plays.
void ClipPlayer::complexInput(Voice& v, const media::SampleView& view, int64_t k0, int64_t k1) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  const uint32_t ch = std::min<uint32_t>(view.channels, 2);
  float* d[2] = {stretchIn_.data(), stretchIn_.data() + stretchInCap_};
  const int64_t dir = c.reverse ? -1 : 1;
  const LoopFrames L = loopFrames(*v.img, c);
  const auto chunkMax = static_cast<int64_t>(cfg_.historyFrames / 2);
  const int64_t z = std::clamp(v.zeroBelow, k0, k1);
  for (uint32_t q = 0; q < ch; ++q) std::memset(d[q], 0, sizeof(float) * static_cast<size_t>(z - k0));
  for (int64_t k = z; k < k1;) {
    const int64_t phi = v.phi0 + dir * k;
    const int64_t src = foldFrame(phi, L);
    int64_t run = k1 - k;  // stream frames until the file position jumps (a loop wrap)
    if (L.on) {
      if (dir > 0) run = std::min(run, phi < L.le ? L.le - phi : L.span - (src - L.ls));
      else if (phi >= L.le) run = std::min(run, src - L.ls + 1);
    }
    const int64_t a = dir > 0 ? src : src - run + 1, b = a + run;  // the file frames [a, b) of this run
    const int64_t at = k - k0;
    const int64_t fa = std::max<int64_t>(a, 0), fb = std::min<int64_t>(b, view.frames);
    if (fa > a || fb < b || fa >= fb)
      for (uint32_t q = 0; q < ch; ++q) std::memset(d[q] + at, 0, sizeof(float) * static_cast<size_t>(run));
    const float* base[2] = {nullptr, nullptr};
    int64_t bf = 0;
    if (dir > 0) {
      for (int64_t x = fa; x < fb;) {
        const int64_t y = std::min(fb, x + chunkMax);
        const bool ok = source(v, v.reader, view, x, y, true, 2 * kSlack, base, &bf);
        for (uint32_t q = 0; q < ch; ++q) {
          if (ok) std::memcpy(d[q] + at + (x - a), base[q] + (x - bf), sizeof(float) * static_cast<size_t>(y - x));
          else std::memset(d[q] + at + (x - a), 0, sizeof(float) * static_cast<size_t>(y - x));
        }
        x = y;
      }
    } else {
      for (int64_t y = fb; y > fa;) {
        const int64_t x = std::max(fa, y - chunkMax);
        const bool ok = source(v, v.reader, view, x, y, false, 2 * kSlack, base, &bf);
        for (uint32_t q = 0; q < ch; ++q) {
          float* dq = d[q] + at + (src - (y - 1));  // stream order: file frame i lands at at + (src − i)
          if (ok) {
            const float* sq = base[q] + (y - 1 - bf);
            for (int64_t i = 0; i < y - x; ++i) dq[i] = sq[-i];
          } else {
            std::memset(dq, 0, sizeof(float) * static_cast<size_t>(y - x));
          }
        }
        y = x;
      }
    }
    k += run;
  }
}

// Song samples [s, s + n): the vocoder output read at rate r from (s − tA) · r (a copy at r = 1).
void ClipPlayer::renderComplex(Voice& v, const media::SampleView& view, SampleTime s, uint32_t n, float* const* out,
                               const float* env) noexcept {  // RT-safe
  const AudioSequenceImage::Clip& c = *v.clip;
  const StretchPool::Latency& lat = stretch_->latency();
  const int64_t step = c.pitchStep;
  const bool copy = step == kOne;
  const dsp::SincTable* tb = copy ? nullptr : &dsp::sincTableAtLeast(dsp::ResampleQuality::High, dsp::sincStretchQ(pitchRateOf(c)));
  const int64_t reach = complexReach(c);
  auto posAt = [&](SampleTime t) noexcept {
    return static_cast<int64_t>(static_cast<uint64_t>(t - v.tA) * static_cast<uint64_t>(step));
  };
  // Not primed yet, or its clock left what was primed (a pending start that a swap moved behind the playhead, far):
  // prime for an output at s now. The test is on the first frame this read needs (what complexEnsure keeps from the
  // run before: its own first frame), not on y − reach: a read that moved less than the resampler's reach since the
  // last run (pitch rate × block < reach, M6 review C2) must not reset the stretcher.
  if (v.vHi == INT64_MIN) {
    primeComplex(v, view, s);
  } else {
    const int64_t pos = posAt(s);
    const int64_t y = pos >> 32;
    const int64_t need = copy ? y : dsp::sincReach(*tb, pos, step, 1).first;
    if (need < v.vLo || y - v.vHi > lat.output + lat.block + 4 * kChunk) primeComplex(v, view, s);
  }
  const float* b0 = stretch_->output(v.stretch);
  const float* in[2] = {b0, b0 + kStretchFrames};
  const uint32_t ch = view.channels >= 2 ? 2u : 1u;
  const double r = pitchRateOf(c);
  const int64_t lead = stretchLead(c, lat);  // M10: computed this far ahead of the reads
  const auto maxRun = static_cast<uint32_t>(
      std::max(1.0, std::floor(static_cast<double>(kVoc / 2 - 2 * reach - kChunk - lead) / std::max(r, 1e-9))));
  for (uint32_t done = 0; done < n;) {
    const uint32_t m = std::min(n - done, maxRun);
    const int64_t pos = posAt(s + done);
    int64_t first = 0, last = 0;
    if (copy) {
      first = pos >> 32;
      last = first + m;
    } else {
      const dsp::SincReach rc = dsp::sincReach(*tb, pos, step, m);
      first = rc.first;
      last = rc.last;
    }
    complexEnsure(v, view, first, last + lead);
    float* o[2] = {out[0] + done, out[1] + done};
    const float* e = env != nullptr ? env + done : nullptr;
    if (first >= v.vLo) {
      if (copy) dsp::gainCopy(in, ch, first - v.vLo, 1, m, e, c.gain, o);
      else dsp::sincResample(*tb, in, ch, pos - v.vLo * kOne, step, m, e, c.gain, o);
    }
    done += m;
  }
}

// Where a stretch voice's stream starts at s: a little before the first frame its slice / grains read there.
int64_t ClipPlayer::stretchStreamFrame(const AudioSequenceImage& img, const AudioSequenceImage::Clip& c, SampleTime s,
                                       bool* forward, bool complex) noexcept {  // RT-safe
  if (complex) {  // T6.5: the first frame the stretcher's priming reads (complexInput)
    const ComplexStart st = complexStart(img, c, s, StretchPool::latencyFor(img.sampleRate()));
    *forward = !c.reverse;
    return st.firstFrame;
  }
  const dsp::SincTable& tb = dsp::sincTableAtLeast(dsp::ResampleQuality::High, dsp::sincStretchQ(c.pitchRate));
  const double reach = static_cast<double>(tb.taps) + 2.0 * static_cast<double>(kSlack);
  const bool fwd = !c.reverse;
  *forward = fwd;
  double lo = 0.0, hi = 0.0;
  if (c.warp == ClipWarp::Beats) {
    const SlicePass p = passAt(img, c, s);
    const double off = std::isfinite(p.L) ? std::min(static_cast<double>(s - p.T) * c.pitchRate, p.L)
                                          : static_cast<double>(s - p.T) * c.pitchRate;
    lo = hi = p.F + static_cast<double>(p.dir) * std::max(off, 0.0);
  } else {
    const double P = img.warpFrame(c, s);
    const double spread = 2.0 * static_cast<double>(c.hop) * c.pitchRate + static_cast<double>(c.alignLags) + c.jitter;
    lo = P - spread;
    hi = P + spread;
  }
  const int64_t frames = img.sample(c.sample).view().frames;
  const auto f = fwd ? static_cast<int64_t>(std::floor(lo - reach)) : static_cast<int64_t>(std::ceil(hi + reach));
  return std::clamp<int64_t>(f, 0, frames);
}

}  // namespace pq::engine
