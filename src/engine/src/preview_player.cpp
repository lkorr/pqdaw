// T5.10: the browser's preview player (pq/engine/preview_player.h).

#include "pq/engine/preview_player.h"

#include <algorithm>
#include <cmath>

namespace pq::engine {

namespace {

ClipPlayerConfig clipConfig(const PreviewConfig& c) {
  ClipPlayerConfig cc;
  cc.slots = 1;
  cc.maxBlockFrames = std::max(c.maxBlockFrames, 64u);
  cc.sampleRate = c.sampleRate;
  cc.maxVoices = 8;        // the playing voice, plus ones fading out after scrubs
  cc.streamBuffers = 4;
  cc.historyFrames = 8192;
  cc.streams = c.streams;
  cc.lookaheadSeconds = 0.0;  // a preview starts where it is asked to: no voice before its start
  cc.declickSeconds = 0.002;
  return cc;
}

}  // namespace

PreviewPlayer::PreviewPlayer(const PreviewConfig& config) : cfg_(config), player_(clipConfig(config)) {
  cfg_.maxBlockFrames = std::max(cfg_.maxBlockFrames, 64u);
  held_.reserve(8);
}

PreviewPlayer::~PreviewPlayer() = default;

// ---- UI thread ----------------------------------------------------------------------------------------------------

Status PreviewPlayer::play(media::SampleRef sample, double startSeconds, const PreviewOptions& o) {
  if (!sample) return Error{ErrorCode::InvalidArgument, "preview: no sample", {}};
  const media::SampleView& v = sample->view();
  if (v.frames <= 0 || v.sampleRate == 0) return Error{ErrorCode::InvalidArgument, "preview: empty sample", {}};
  const double rate = static_cast<double>(v.sampleRate);
  const double seconds = static_cast<double>(v.frames) / rate;
  const bool sync = o.sync && o.fileBpm > 0.0 && o.projectBpm > 0.0;
  AudioClipDesc c;
  c.id = ++clipId_;
  c.sample = 0;
  c.start = 0;
  c.playable = true;
  c.gain = 1.0f;
  TempoMap tempo;
  double framesPerTick = 0.0;
  if (sync) {
    const TempoPoint pt{0, std::clamp(o.projectBpm, kMinBpm, kMaxBpm), TempoCurve::Step};
    auto t = TempoMap::create(std::span<const TempoPoint>(&pt, 1));
    if (!t) return std::move(t).error();
    tempo = std::move(*t);
    c.warp = ClipWarp::RePitch;
    framesPerTick = rate * 60.0 / (o.fileBpm * static_cast<double>(kPpq));
    c.framesPerTick = framesPerTick;
    c.contentStart = 0;
    c.length = std::max<Ticks>(1, static_cast<Ticks>(std::ceil(static_cast<double>(v.frames) / framesPerTick)));
    speed_ = o.projectBpm / o.fileBpm;
  } else {
    c.warp = ClipWarp::Off;
    c.speed = rate;
    c.contentStartFrame = 0.0;
    c.length = std::max<Ticks>(1, static_cast<Ticks>(std::ceil(tempo.secondsToTicks(seconds))));
    speed_ = 1.0;
  }
  AudioTrackDesc track;
  track.clips.push_back(c);
  std::vector<ClipSample> samples(1);
  samples[0].pooled = std::move(sample);
  AudioBuildOptions bo;
  bo.sampleRate = cfg_.sampleRate;
  bo.edgeDeclick = true;
  auto img = AudioSequenceImage::build(std::span<const AudioTrackDesc>(&track, 1), std::move(samples), tempo, bo);
  if (!img) return std::move(img).error();
  const AudioSequenceImage* raw = img->get();
  lengthSeconds_ = seconds;
  current_ = raw;
  Command cmd;
  cmd.cmd = Cmd::Play;
  cmd.image = raw;
  cmd.at = songAt(std::clamp(startSeconds, 0.0, seconds));
  cmd.end = raw->clips(0).empty() ? cmd.at : raw->clips(0)[0].playEnd;
  cmd.seq = ++seq_;
  held_.push_back({std::move(*img), cmd.seq});
  if (!ring_.tryPush(cmd)) {
    --seq_;
    current_ = nullptr;
    held_.pop_back();
    return Error{ErrorCode::Overflow, "preview: command ring full", {}};
  }
  (void)collect();
  return Ok{};
}

SampleTime PreviewPlayer::songAt(double fileSeconds) const noexcept {
  if (!current_) return 0;
  const AudioSequenceImage& img = *current_;
  if (img.clips(0).empty()) return 0;
  const AudioSequenceImage::Clip& c = img.clips(0)[0];
  if (c.warp == ClipWarp::RePitch) {
    const double rate = static_cast<double>(img.sample(0).view().sampleRate);
    const double contentTicks = fileSeconds * rate / c.framesPerTick;
    return img.tempo().ticksToSamples(static_cast<Ticks>(std::llround(contentTicks)), img.sampleRate());
  }
  return static_cast<SampleTime>(std::llround(fileSeconds * img.sampleRate()));
}

bool PreviewPlayer::scrub(double fileSeconds) {
  if (!current_) return false;
  Command cmd;
  cmd.cmd = Cmd::Scrub;
  cmd.image = current_;
  cmd.at = songAt(std::clamp(fileSeconds, 0.0, lengthSeconds_));
  cmd.end = current_->clips(0).empty() ? cmd.at : current_->clips(0)[0].playEnd;
  cmd.seq = ++seq_;
  if (!ring_.tryPush(cmd)) {
    --seq_;
    return false;
  }
  for (Held& h : held_)
    if (h.image.get() == current_) h.lastSeq = cmd.seq;
  return true;
}

void PreviewPlayer::stop() {
  Command cmd;
  cmd.cmd = Cmd::Stop;
  cmd.seq = ++seq_;
  if (!ring_.tryPush(cmd)) --seq_;
}

void PreviewPlayer::setGain(float linear) { targetGain_.store(std::max(0.0f, linear), std::memory_order_relaxed); }

PreviewPlayer::State PreviewPlayer::state() const noexcept {
  return busy_.load(std::memory_order_acquire) || acked_.load(std::memory_order_acquire) < seq_ ? State::Playing
                                                                                                : State::Stopped;
}

double PreviewPlayer::positionSeconds() const noexcept {
  const int64_t p = pos_.load(std::memory_order_acquire);
  if (p < 0) return -1.0;
  return static_cast<double>(p) / cfg_.sampleRate * speed_;
}

size_t PreviewPlayer::collect() {
  const uint64_t acked = acked_.load(std::memory_order_acquire);
  const AudioSequenceImage* u0 = using_[0].load(std::memory_order_acquire);
  const AudioSequenceImage* u1 = using_[1].load(std::memory_order_acquire);
  std::erase_if(held_, [&](const Held& h) {
    const AudioSequenceImage* p = h.image.get();
    return h.lastSeq <= acked && p != u0 && p != u1 && p != current_;
  });
  return held_.size();
}

// ---- audio thread -------------------------------------------------------------------------------------------------

void PreviewPlayer::callback(void* self, const AudioBlock& b) noexcept {  // RT-safe
  static_cast<PreviewPlayer*>(self)->process(b.out, b.numOut, b.frames);
}

bool PreviewPlayer::apply(const Command& c) noexcept {  // RT-safe
  switch (c.cmd) {
    case Cmd::Play:
      if (player_.swapping()) return false;
      player_.releaseAll(0);
      player_.setImage(c.image, c.at, true);
      s_ = c.at;
      end_ = c.end;
      playing_ = true;
      break;
    case Cmd::Scrub:
      if (player_.swapping()) return false;
      player_.releaseAll(0);
      if (c.image != player_.image()) player_.setImage(c.image, c.at, true);
      else player_.seek(c.at);
      s_ = c.at;
      end_ = c.end;
      playing_ = true;
      break;
    case Cmd::Stop:
      player_.releaseAll(0);
      playing_ = false;
      break;
  }
  // M5 review C1: publish the images the clip player now reads *before* acknowledging the command. collect() loads
  // acked_ (acquire) and then using_[], so once it sees this command acknowledged it also sees the new image (and the
  // one fading out) as in use, and never frees an image this block is about to render.
  using_[0].store(player_.image(), std::memory_order_release);
  using_[1].store(player_.previousImage(), std::memory_order_release);
  pos_.store(playing_ ? s_ : -1, std::memory_order_release);
  busy_.store(playing_ || player_.liveVoices() != 0, std::memory_order_release);
  acked_.store(c.seq, std::memory_order_release);
  return true;
}

void PreviewPlayer::process(float* const* out, uint32_t channels, uint32_t frames) noexcept {  // RT-safe
  ++stats_.blocks;
  if (hasPending_ && apply(pending_)) hasPending_ = false;
  Command c;
  while (!hasPending_ && ring_.tryPop(c)) {
    ++stats_.commands;
    if (!apply(c)) {
      pending_ = c;
      hasPending_ = true;
    }
  }
  if (!playing_ && player_.liveVoices() == 0) {  // asleep: nothing plays or fades
    ++stats_.sleptBlocks;
    return;
  }
  ++stats_.renderedBlocks;
  for (uint32_t done = 0; done < frames;) {
    const uint32_t n = std::min(frames - done, cfg_.maxBlockFrames);
    float* part[2] = {out[0] + done, channels > 1 ? out[1] + done : nullptr};
    render(part, channels, n);
    done += n;
  }
  using_[0].store(player_.image(), std::memory_order_release);
  using_[1].store(player_.previousImage(), std::memory_order_release);
  pos_.store(playing_ ? s_ : -1, std::memory_order_release);
  busy_.store(playing_ || player_.liveVoices() != 0, std::memory_order_release);
}

void PreviewPlayer::render(float* const* out, uint32_t channels, uint32_t n) noexcept {  // RT-safe
  player_.beginBlock(n);
  if (playing_) {
    player_.play(0, s_, n);
    s_ += n;
    if (s_ >= end_) playing_ = false;  // the file ended (its voice ended at the clip's end)
  } else {
    player_.idle(n);
  }
  player_.endBlock();
  const float g1 = targetGain_.load(std::memory_order_relaxed);
  const float g0 = gain_;
  gain_ = g1;
  if (!player_.active()[0] || channels == 0) return;
  const float* const* src = player_.channels();
  const float step = (g1 - g0) / static_cast<float>(n);
  for (uint32_t ch = 0; ch < std::min(channels, 2u); ++ch) {
    const float* in = src[ch];
    float* o = out[ch];
    if (g0 == g1) {
      for (uint32_t i = 0; i < n; ++i) o[i] += in[i] * g1;
    } else {
      float g = g0;
      for (uint32_t i = 0; i < n; ++i, g += step) o[i] += in[i] * g;
    }
  }
}

}  // namespace pq::engine
