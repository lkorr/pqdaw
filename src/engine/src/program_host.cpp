// Hot swap (T1.9): pq/engine/program_host.h describes the protocol.
#include "pq/engine/program_host.h"

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace pq::engine {

struct ProgramHost::Packet {
  uint64_t seq = 0;
  std::unique_ptr<ProgramImage> image;
  TransferPlan plan;                                  // from the previous packet's image (empty for the first)
  SwapMode mode = SwapMode::Auto;
  uint32_t fadeFrames = 1;
  std::vector<float> params;                          // the param block, indexed by io().params
  std::vector<std::pair<uint32_t, uint32_t>> paramMap;  // (new slot, previous packet's slot) with equal keys
  AlignedBuffer<float> scratch;                       // one maxBlock buffer per global out/export slot
  std::vector<float*> slotPtrs;
  uint32_t chanSlot[kMaxChannels] = {};               // device channel → global out slot / mix index, or kNone
  bool playMixes = false;                             // chanSlot indexes the VoiceEngine's mixes
  std::unique_ptr<VoiceBinding> binding;              // with HostConfig::voices
};

namespace {

Error hostError(ErrorCode code, const std::string& m) { return Error{code, "hot swap: " + m, {}}; }

// dst = a + g · (b − a), g = (pos + f + 1) / len per frame f; exactly b from g = 1 on. Bit-exact where a == b.
void crossfade(float* dst, const float* a, const float* b, uint32_t n, uint32_t pos, uint32_t len) noexcept {  // RT-safe
  const float inv = 1.0f / static_cast<float>(len);
  const uint32_t ramp = pos >= len ? 0u : std::min(n, len - 1 - std::min(len - 1, pos));  // frames with g < 1
  uint32_t f = 0;
  const __m256 vinv = _mm256_set1_ps(inv);
  const __m256i step = _mm256_setr_epi32(1, 2, 3, 4, 5, 6, 7, 8);
  for (; f + 8 <= ramp; f += 8) {
    const __m256i k = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int>(pos + f)), step);
    const __m256 g = _mm256_mul_ps(_mm256_cvtepi32_ps(k), vinv);
    const __m256 va = _mm256_loadu_ps(a + f), vb = _mm256_loadu_ps(b + f);
    _mm256_storeu_ps(dst + f, _mm256_add_ps(va, _mm256_mul_ps(g, _mm256_sub_ps(vb, va))));
  }
  for (; f < ramp; ++f) {
    const float g = static_cast<float>(pos + f + 1) * inv;
    dst[f] = a[f] + g * (b[f] - a[f]);
  }
  if (f < n) std::memcpy(dst + f, b + f, sizeof(float) * (n - f));
}

float peakAbs(const float* x, uint32_t n) noexcept {  // RT-safe
  const __m256 absMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
  __m256 m = _mm256_setzero_ps();
  uint32_t f = 0;
  for (; f + 8 <= n; f += 8) m = _mm256_max_ps(m, _mm256_and_ps(_mm256_loadu_ps(x + f), absMask));
  alignas(32) float t[8];
  _mm256_store_ps(t, m);
  float p = 0.0f;
  for (float v : t) p = std::max(p, v);
  for (; f < n; ++f) p = std::max(p, std::fabs(x[f]));
  return p;
}

}  // namespace

ProgramHost::ProgramHost(const HostConfig& config)
    : cfg_(config), pending_(std::bit_ceil(std::max(2u, config.maxPending))) {
  cfg_.numIn = std::min(cfg_.numIn, kMaxChannels);
  cfg_.numOut = std::min(cfg_.numOut, kMaxChannels);
  cfg_.maxBlockFrames = std::max(cfg_.maxBlockFrames, 1u);
  reader_ = *reclaimer_.registerReader();  // one slot, registered once: cannot fail
  zeros_ = AlignedBuffer<float>(cfg_.maxBlockFrames + 8);
  if (cfg_.voices) events_ = EventBuffer(std::max(cfg_.eventCapacity, 1u));
  silence_ = static_cast<float>(std::pow(10.0, static_cast<double>(cfg_.silenceDb) / 20.0));
}

ProgramHost::~ProgramHost() {
  for (Packet* p : inflight_) delete p;
  inflight_.clear();
  // reclaimer_'s destructor frees the retired packets.
}

Result<PublishInfo> ProgramHost::publish(std::unique_ptr<ProgramImage> image, const SwapPolicy& policy,
                                         const VoiceSlots& voiceSlots) {
  if (image == nullptr) return hostError(ErrorCode::InvalidArgument, "no image");
  collect();
  std::lock_guard lock(mutex_);
  const ImageInterface& io = image->io();
  if (io.sampleRate != cfg_.sampleRate || io.maxBlockFrames != cfg_.maxBlockFrames)
    return hostError(ErrorCode::InvalidArgument, "the image's sample rate / maxBlockFrames differ from the host's");
  auto p = std::make_unique<Packet>();
  p->seq = nextSeq_;
  p->image = std::move(image);
  ProgramImage& img = *p->image;
  p->mode = policy.mode;
  const float fadeMs = std::clamp(std::isfinite(policy.fadeMs) ? policy.fadeMs : 10.0f, kMinFadeMs, kMaxFadeMs);
  p->fadeFrames = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(fadeMs * 0.001 * cfg_.sampleRate)));

  // Params: defaults, then the previous image's values for the same keys (copied at the swap).
  for (const graph::ParamSlot& ps : io.params) p->params.push_back(ps.defaultValue);
  PublishInfo info;
  info.sequence = p->seq;
  if (last_ != nullptr) {
    ProgramImage& prev = *last_->image;
    auto plan = buildTransferPlan(prev.stateLayout(), prev.stateArena(), img.stateLayout(), img.stateArena());
    if (!plan.ok()) return plan.error();
    p->plan = std::move(plan.value());
    std::unordered_map<uint64_t, uint32_t> prevSlot;
    const auto& pp = prev.io().params;
    for (uint32_t i = 0; i < pp.size(); ++i) prevSlot.try_emplace(pp[i].key, i);
    for (uint32_t i = 0; i < io.params.size(); ++i)
      if (const auto it = prevSlot.find(io.params[i].key); it != prevSlot.end()) p->paramMap.emplace_back(i, it->second);
    info.plan = p->plan;
    info.paramsCarried = static_cast<uint32_t>(p->paramMap.size());
  }

  // Global output scratch and the channel map.
  const uint32_t slots = static_cast<uint32_t>(io.globalOut.size());
  const uint32_t stride = (cfg_.maxBlockFrames + 15u) & ~15u;
  p->scratch = AlignedBuffer<float>(static_cast<size_t>(std::max(slots, 1u)) * stride);
  p->slotPtrs.assign(slots, nullptr);
  for (uint32_t s = 0; s < slots; ++s) p->slotPtrs[s] = p->scratch.data() + static_cast<size_t>(s) * stride;
  std::vector<uint32_t> played;  // the `out` slots, in slot order
  for (uint32_t s = 0; s < slots; ++s)
    if (!io.globalOut[s].isExport) played.push_back(s);

  // Voices: lanes prepared once (with the first image), a binding per image.
  if (cfg_.voices) {
    if (last_ == nullptr) {
      if (Status st = voices_.prepareLanes(io, cfg_.voice); !st.ok()) return st.error();
    }
    auto b = voices_.makeBinding(&img, voiceSlots.doneSlot, voiceSlots.monitorMask);
    if (!b.ok()) return b.error();
    p->binding = std::move(b.value());
    const uint32_t mixes = static_cast<uint32_t>(p->binding->mixSlots.size());
    if (played.empty() && mixes > 0) {
      p->playMixes = true;
      played.clear();
      for (uint32_t i = 0; i < mixes; ++i) played.push_back(i);
    }
  }
  for (uint32_t c = 0; c < kMaxChannels; ++c)
    p->chanSlot[c] = played.empty() ? graph::kNone : played.size() == 1 ? played[0] : c < played.size() ? played[c] : graph::kNone;

  Packet* raw = p.get();
  inflight_.push_back(raw);
  if (!pending_.tryPush(raw)) {
    inflight_.pop_back();
    return hostError(ErrorCode::Overflow, "too many images pending; retry after the audio thread has taken some");
  }
  p.release();  // owned through inflight_ (then the reclaimer)
  last_ = raw;
  ++nextSeq_;
  return info;
}

size_t ProgramHost::collect() {
  std::lock_guard lock(mutex_);
  const uint64_t through = retiredThrough_.load(std::memory_order_acquire);
  size_t k = 0;
  while (k < inflight_.size() && inflight_[k]->seq <= through) {
    reclaimer_.retire(inflight_[k], [](void* q) noexcept { delete static_cast<Packet*>(q); });
    ++k;
  }
  inflight_.erase(inflight_.begin(), inflight_.begin() + static_cast<std::ptrdiff_t>(k));
  const size_t n = reclaimer_.collect();
  freed_.fetch_add(n, std::memory_order_relaxed);
  return n;
}

size_t ProgramHost::imagesAlive() const {
  std::lock_guard lock(mutex_);
  return inflight_.size() + reclaimer_.pending();
}

uint64_t ProgramHost::imagesFreed() const { return freed_.load(std::memory_order_relaxed); }

const ProgramImage* ProgramHost::lastPublished() const {
  std::lock_guard lock(mutex_);
  return last_ != nullptr ? last_->image.get() : nullptr;
}

std::span<float> ProgramHost::params() noexcept {  // RT-safe
  return cur_ != nullptr ? std::span<float>(cur_->params) : std::span<float>();
}

const ProgramImage* ProgramHost::current() const noexcept { return cur_ != nullptr ? cur_->image.get() : nullptr; }

uint64_t ProgramHost::currentSequence() const noexcept { return cur_ != nullptr ? cur_->seq : 0; }

const float* ProgramHost::channelSource(const Packet& p, bool fading, uint32_t c) const noexcept {  // RT-safe
  const uint32_t s = p.chanSlot[c];
  if (s == graph::kNone) return nullptr;
  if (p.playMixes) return fading ? voices_.fadeMix(s) : voices_.mix(s);
  return p.slotPtrs[s];
}

float ProgramHost::playedPeak(const Packet& p, bool fading, uint32_t frames) const noexcept {  // RT-safe
  float peak = 0.0f;
  for (uint32_t c = 0; c < cfg_.numOut; ++c)
    if (const float* src = channelSource(p, fading, c)) peak = std::max(peak, peakAbs(src, frames));
  return peak;
}

void ProgramHost::retire(Packet* p) noexcept {  // RT-safe
  retiredThrough_.store(p->seq, std::memory_order_release);
}

void ProgramHost::trySwap() noexcept {  // RT-safe
  Packet* p = nullptr;
  if (!pending_.tryPop(p)) return;
  ++stats_.swaps;
  Packet* old = cur_;
  if (old == nullptr) {  // the first image: nothing to carry over
    if (cfg_.voices) voices_.attach(p->binding.get(), nullptr);
    cur_ = p;
    ++stats_.instantSwaps;
    stats_.transferBytes = 0;
    return;
  }
  bool fade = p->mode == SwapMode::Crossfade;
  if (p->mode == SwapMode::Auto)
    fade = transport_.playing || (cfg_.voices && voices_.activeGroups() > 0) || playedPeak(*old, false, lastFrames_) > silence_;
  // State: the old image's current state, right before the new image's first block.
  const StateArena from = old->image->stateArena(), to = p->image->stateArena();
  const uint32_t* groupMask = cfg_.voices && cfg_.voice.resetOnNoteOn ? voices_.laneMasks() : nullptr;
  executeTransfer(p->plan, from, to, groupMask);
  uint64_t bytes = p->plan.globalBytes;
  const uint32_t groups = std::min({p->plan.voiceGroups, from.voiceGroups, to.voiceGroups});
  for (uint32_t g = 0; g < groups; ++g)
    if (groupMask == nullptr || groupMask[g] != 0) bytes += p->plan.voiceBytes;
  stats_.transferBytes = bytes;
  for (const auto& [dst, src] : p->paramMap) p->params[dst] = old->params[src];
  if (cfg_.voices) voices_.attach(p->binding.get(), fade ? old->binding.get() : nullptr);
  cur_ = p;
  if (fade) {
    fadeFrom_ = old;
    fadePos_ = 0;
    fadeLen_ = p->fadeFrames;
    ++stats_.crossfades;
  } else {
    retire(old);
    ++stats_.instantSwaps;
  }
}

void ProgramHost::prepareContext(ProcessContext& ctx, Packet& p, const float* const* in, uint32_t numIn,
                                 uint32_t frames, int64_t sampleTime) noexcept {  // RT-safe
  ctx.frames = frames;
  ctx.sampleRate = cfg_.sampleRate;
  ctx.sampleTime = sampleTime;
  ctx.transport = transport_;
  ctx.in = in;
  ctx.numIn = numIn;
  ctx.out = p.slotPtrs.data();
  ctx.numOut = static_cast<uint32_t>(p.slotPtrs.size());
  ctx.params = p.params.data();
  ctx.numParams = static_cast<uint32_t>(p.params.size());
}

void ProgramHost::process(const AudioBlock& block) noexcept {  // RT-safe
  const uint32_t maxBlock = cfg_.maxBlockFrames;
  const uint32_t numIn = std::min(cfg_.numIn, block.numIn);
  const uint32_t numOut = std::min(cfg_.numOut, block.numOut);
  const float* in[kMaxChannels];
  for (uint32_t off = 0; off < block.frames; off += maxBlock) {
    const uint32_t n = std::min(maxBlock, block.frames - off);
    const int64_t time = block.frameTime + off;
    if (fadeFrom_ == nullptr) trySwap();
    if (cfg_.hook.fn != nullptr) cfg_.hook.fn(cfg_.hook.user, time, n, transport_, params());
    for (uint32_t c = 0; c < numIn; ++c) in[c] = block.in[c] + off;
    if (cur_ == nullptr) {
      for (uint32_t c = 0; c < numOut; ++c) std::memset(block.out[c] + off, 0, sizeof(float) * n);
    } else {
      Packet* old = fadeFrom_;
      ProcessContext ctx, fctx;
      prepareContext(ctx, *cur_, in, numIn, n, time);
      if (old != nullptr) prepareContext(fctx, *old, in, numIn, n, time);
      if (cfg_.voices) {
        events_.clear();
        if (cfg_.events.fn != nullptr) cfg_.events.fn(cfg_.events.user, time, n, events_);
        ctx.events = &events_.list();
        voices_.process(ctx, old != nullptr ? &fctx : nullptr);
      } else {
        cur_->image->process(ctx);
        if (old != nullptr) old->image->process(fctx);
      }
      for (uint32_t c = 0; c < numOut; ++c) {
        float* dst = block.out[c] + off;
        const float* b = channelSource(*cur_, false, c);
        if (old != nullptr) {
          const float* a = channelSource(*old, true, c);
          crossfade(dst, a != nullptr ? a : zeros_.data(), b != nullptr ? b : zeros_.data(), n, fadePos_, fadeLen_);
        } else if (b != nullptr) {
          std::memcpy(dst, b, sizeof(float) * n);
        } else {
          std::memset(dst, 0, sizeof(float) * n);
        }
      }
      if (old != nullptr) {
        ++stats_.fadeBlocks;
        fadePos_ += n;
        if (fadePos_ >= fadeLen_) {  // the fade is over: the old image is no longer used
          fadeFrom_ = nullptr;
          if (cfg_.voices) voices_.attach(cur_->binding.get(), nullptr);
          retire(old);
        }
      }
      ++stats_.blocks;
    }
    if (cfg_.hook.fn == nullptr && transport_.playing)
      transport_.beat += static_cast<double>(n) * transport_.beatsPerFrame;
    lastFrames_ = n;
  }
  reclaimer_.quiescent(reader_);
}

}  // namespace pq::engine
