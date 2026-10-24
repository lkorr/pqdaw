#include "pq/engine/image_runner.h"

#include <algorithm>
#include <cstring>

namespace pq::engine {

bool ImageRunner::prepare(ProgramImage* image, uint32_t numIn, uint32_t numOut, float gain, bool limit) {
  if (image == nullptr || numIn > kMaxChannels || numOut > kMaxChannels) return false;
  image_ = image;
  numIn_ = numIn;
  numOut_ = numOut;
  gain_ = gain;
  limit_ = limit;
  blocks_ = 0;
  voices_ = nullptr;
  source_ = {};
  hook_ = {};
  playMixes_ = false;
  const ImageInterface& io = image->io();
  params_.clear();
  for (const graph::ParamSlot& p : io.params) params_.push_back(p.defaultValue);
  const uint32_t slots = static_cast<uint32_t>(io.globalOut.size());
  const uint32_t stride = (io.maxBlockFrames + 15u) & ~15u;
  scratch_ = AlignedBuffer<float>(static_cast<size_t>(std::max(slots, 1u)) * stride);
  slotPtrs_.assign(slots, nullptr);
  for (uint32_t s = 0; s < slots; ++s) slotPtrs_[s] = scratch_.data() + static_cast<size_t>(s) * stride;
  std::vector<uint32_t> played;  // the `out` slots, in slot order
  for (uint32_t s = 0; s < slots; ++s)
    if (!io.globalOut[s].isExport) played.push_back(s);
  for (uint32_t c = 0; c < kMaxChannels; ++c)
    chanSlot_[c] = played.empty() ? graph::kNone : played.size() == 1 ? played[0] : c < played.size() ? played[c] : graph::kNone;
  return true;
}

void ImageRunner::setVoices(VoiceEngine* voices, EventSource source, uint32_t eventCapacity) {
  voices_ = voices;
  source_ = source;
  events_ = EventBuffer(std::max(eventCapacity, 1u));
  bool anyOut = false;
  for (const IoSlot& s : image_->io().globalOut) anyOut = anyOut || !s.isExport;
  playMixes_ = voices != nullptr && !anyOut && voices->numMixes() > 0;
  if (!playMixes_) return;
  const uint32_t mixes = voices->numMixes();
  for (uint32_t c = 0; c < kMaxChannels; ++c) chanSlot_[c] = mixes == 1 ? 0u : c < mixes ? c : graph::kNone;
}

void ImageRunner::process(const AudioBlock& block) noexcept {  // RT-safe
  const uint32_t maxBlock = image_->io().maxBlockFrames;
  const float* in[kMaxChannels];
  ProcessContext ctx;
  ctx.sampleRate = image_->io().sampleRate;
  ctx.numIn = std::min(numIn_, block.numIn);
  ctx.in = in;
  ctx.out = slotPtrs_.data();
  ctx.numOut = static_cast<uint32_t>(slotPtrs_.size());
  ctx.params = params_.data();
  ctx.numParams = static_cast<uint32_t>(params_.size());
  for (uint32_t off = 0; off < block.frames; off += maxBlock) {
    const uint32_t n = std::min(maxBlock, block.frames - off);
    for (uint32_t c = 0; c < ctx.numIn; ++c) in[c] = block.in[c] + off;
    ctx.frames = n;
    ctx.sampleTime = block.frameTime + off;
    if (hook_.fn != nullptr) hook_.fn(hook_.user, ctx.sampleTime, n, transport_, params_);
    ctx.transport = transport_;
    if (voices_ != nullptr) {
      events_.clear();
      if (source_.fn != nullptr) source_.fn(source_.user, ctx.sampleTime, n, events_);
      ctx.events = &events_.list();
      voices_->process(ctx);
    } else {
      image_->process(ctx);
    }
    if (hook_.fn == nullptr && transport_.playing) transport_.beat += static_cast<double>(n) * transport_.beatsPerFrame;
    for (uint32_t c = 0; c < std::min(numOut_, block.numOut); ++c) {
      float* dst = block.out[c] + off;
      const uint32_t s = chanSlot_[c];
      if (s == graph::kNone) {
        std::memset(dst, 0, sizeof(float) * n);
        continue;
      }
      const float* src = playMixes_ ? voices_->mix(s) : slotPtrs_[s];
      if (limit_) {
        for (uint32_t f = 0; f < n; ++f) {
          const float v = gain_ * src[f];
          dst[f] = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v == v ? v : 0.0f;  // NaN → silence
        }
      } else {
        for (uint32_t f = 0; f < n; ++f) dst[f] = gain_ * src[f];
      }
    }
    ++blocks_;
  }
}

}  // namespace pq::engine
