// Per-track project runner (T1.x-arch1): pq/engine/project_program.h describes the protocol.
#include "pq/engine/project_program.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include "crossfade.h"

namespace pq::engine {

using detail::crossfade;
using detail::peakAbs;

struct ProjectProgram::Packet {
  uint64_t seq = 0;
  std::unique_ptr<ProgramImage> image;
  TransferPlan plan;                                      // from the unit's previous packet (empty for the first)
  SwapMode mode = SwapMode::Auto;
  uint32_t fadeFrames = 1;
  std::vector<float> params;                              // indexed by io().params
  std::vector<std::pair<uint32_t, uint32_t>> paramMap;    // (new slot, previous packet's slot), equal keys
  std::vector<std::pair<uint32_t, uint32_t>> paramLinks;  // (slot, project param)
  std::vector<const float*> inPtrs;                       // per global in slot: a bus channel or zeros
  std::vector<float*> outPtrs;                            // per global out slot: a bus channel or null
  std::vector<float*> fadePtrs;                           // the same into `scratch` (while this packet fades out)
  AlignedBuffer<float> scratch;
  struct FadeLane {
    float* bus;                                           // written by the new image, faded in place
    const float* old;                                     // the previous packet's scratch for the same channel
  };
  std::vector<FadeLane> fade;                             // when this packet replaces the previous one
  std::unique_ptr<VoiceBinding> binding;                  // voice units
};

struct ProjectProgram::Unit {
  explicit Unit(uint32_t pending) : ring(pending) {}
  UnitLayout cfg;
  uint32_t index = 0;
  std::vector<float*> ownChannels;   // every channel of every bus this unit produces
  // Non-RT (ProjectProgram::mutex_).
  std::vector<Packet*> inflight;     // published, not yet retired, in sequence order (owned)
  Packet* last = nullptr;
  uint64_t nextSeq = 1;
  bool lanesPrepared = false;
  // Hand-over.
  SpscRing<Packet*> ring;
  std::atomic<uint64_t> retiredThrough{0};
  // Audio thread.
  Packet* cur = nullptr;
  Packet* fadeFrom = nullptr;
  uint32_t fadePos = 0, fadeLen = 0;
  EventSource events;
  EventBuffer eventBuf{1};
  std::unique_ptr<VoiceEngine> voices;
  UnitStats stats;
};

namespace {

Error projectError(ErrorCode code, const std::string& m) { return Error{code, "project: " + m, {}}; }

}  // namespace

Result<std::unique_ptr<ProjectProgram>> ProjectProgram::create(const ProjectConfig& config, ProjectLayout layout) {
  if (layout.units.empty()) return projectError(ErrorCode::InvalidArgument, "no units");
  if (layout.busWidth.size() != layout.busProducer.size())
    return projectError(ErrorCode::InvalidArgument, "busWidth and busProducer differ in size");
  for (size_t b = 0; b < layout.busWidth.size(); ++b) {
    if (layout.busWidth[b] == 0) return projectError(ErrorCode::InvalidArgument, "bus " + std::to_string(b) + " has no channels");
    if (layout.busProducer[b] >= layout.units.size())
      return projectError(ErrorCode::InvalidArgument, "bus " + std::to_string(b) + " has no valid producer");
  }
  if (layout.outputBus != kNoBus && layout.outputBus >= layout.busWidth.size())
    return projectError(ErrorCode::InvalidArgument, "the output bus does not exist");
  if (config.numOut > kMaxChannels) return projectError(ErrorCode::InvalidArgument, "too many device channels");
  for (const UnitLayout& u : layout.units)
    if (u.voices && u.eventSlot != kNoSlot && u.eventSlot >= config.eventSources.size())
      return projectError(ErrorCode::InvalidArgument, "unit '" + u.name + "': event slot out of range");
  return std::unique_ptr<ProjectProgram>(new ProjectProgram(config, std::move(layout)));
}

ProjectProgram::ProjectProgram(const ProjectConfig& config, ProjectLayout layout)
    : cfg_(config), layout_(std::move(layout)) {
  cfg_.maxBlockFrames = std::max(cfg_.maxBlockFrames, 1u);
  stride_ = (cfg_.maxBlockFrames + 15u) & ~15u;
  uint32_t channels = 0;
  for (uint16_t w : layout_.busWidth) {
    busFirst_.push_back(channels);
    channels += w;
  }
  buses_ = AlignedBuffer<float>(static_cast<size_t>(std::max(channels, 1u)) * stride_ + stride_);  // + one zero block
  projectParams_.assign(cfg_.numProjectParams, 0.0f);
  reader_ = *reclaimer_.registerReader();
  silence_ = static_cast<float>(std::pow(10.0, static_cast<double>(cfg_.silenceDb) / 20.0));
  const uint32_t pending = std::bit_ceil(std::max(2u, cfg_.maxPendingPerUnit));
  for (uint32_t i = 0; i < layout_.units.size(); ++i) {
    auto u = std::make_unique<Unit>(pending);
    u->cfg = layout_.units[i];
    u->index = i;
    for (uint32_t b = 0; b < layout_.busWidth.size(); ++b)
      if (layout_.busProducer[b] == i)
        for (uint16_t c = 0; c < layout_.busWidth[b]; ++c) u->ownChannels.push_back(busChannel(b, c));
    if (u->cfg.voices) {
      u->voices = std::make_unique<VoiceEngine>();
      u->eventBuf = EventBuffer(std::max(cfg_.eventCapacity, 1u));
      if (u->cfg.eventSlot != kNoSlot) u->events = cfg_.eventSources[u->cfg.eventSlot];
    }
    units_.push_back(std::move(u));
  }
}

ProjectProgram::~ProjectProgram() {
  for (auto& u : units_) {
    for (Packet* p : u->inflight) delete p;
    u->inflight.clear();
  }
  // reclaimer_'s destructor frees the retired packets.
}

Result<PublishInfo> ProjectProgram::publish(uint32_t unitIndex, UnitImage ui, const SwapPolicy& policy) {
  if (unitIndex >= units_.size()) return projectError(ErrorCode::InvalidArgument, "no unit " + std::to_string(unitIndex));
  if (ui.image == nullptr) return projectError(ErrorCode::InvalidArgument, "no image");
  collect();
  std::lock_guard lock(mutex_);
  Unit& U = *units_[unitIndex];
  const std::string who = "unit '" + U.cfg.name + "': ";
  ProgramImage& img = *ui.image;
  const ImageInterface& io = img.io();
  if (io.sampleRate != cfg_.sampleRate || io.maxBlockFrames != cfg_.maxBlockFrames)
    return projectError(ErrorCode::InvalidArgument, who + "the image's sample rate / maxBlockFrames differ from the project's");
  if (ui.in.size() != io.globalIn.size() || ui.out.size() != io.globalOut.size())
    return projectError(ErrorCode::InvalidArgument, who + "bindings do not match the image's slots");
  const auto numBuses = static_cast<uint32_t>(layout_.busWidth.size());
  auto badRef = [&](const BusRef& r) { return r.bus >= numBuses || r.channel >= layout_.busWidth[r.bus]; };

  auto p = std::make_unique<Packet>();
  p->seq = U.nextSeq;
  p->mode = policy.mode;
  const float fadeMs = std::clamp(std::isfinite(policy.fadeMs) ? policy.fadeMs : 10.0f, kMinFadeMs, kMaxFadeMs);
  p->fadeFrames = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(fadeMs * 0.001 * cfg_.sampleRate)));
  const float* zeros = buses_.data() + buses_.size() - stride_;  // never written

  for (const BusRef& r : ui.in) {
    if (r.bus == kNoBus) {
      p->inPtrs.push_back(zeros);
      continue;
    }
    if (badRef(r)) return projectError(ErrorCode::InvalidArgument, who + "an input reads a bus channel that does not exist");
    if (layout_.busProducer[r.bus] >= unitIndex)
      return projectError(ErrorCode::InvalidArgument, who + "an input reads bus " + std::to_string(r.bus) +
                                                          ", which is not produced by an earlier unit");
    p->inPtrs.push_back(busChannel(r.bus, r.channel));
  }
  std::vector<uint8_t> written(U.ownChannels.size(), 0);
  for (const BusRef& r : ui.out) {
    if (r.bus == kNoBus) {
      p->outPtrs.push_back(nullptr);
      continue;
    }
    if (badRef(r) || layout_.busProducer[r.bus] != unitIndex)
      return projectError(ErrorCode::InvalidArgument, who + "an output writes a bus channel the unit does not produce");
    float* ch = busChannel(r.bus, r.channel);
    const auto k = static_cast<size_t>(std::find(U.ownChannels.begin(), U.ownChannels.end(), ch) - U.ownChannels.begin());
    if (written[k]) return projectError(ErrorCode::InvalidArgument, who + "two outputs write the same bus channel");
    written[k] = 1;
    p->outPtrs.push_back(ch);
  }
  if (std::find(written.begin(), written.end(), uint8_t{0}) != written.end())
    return projectError(ErrorCode::InvalidArgument, who + "the image leaves a channel of a bus it produces unwritten");
  for (const auto& [slot, param] : ui.paramLinks)
    if (slot >= io.params.size() || param >= cfg_.numProjectParams)
      return projectError(ErrorCode::InvalidArgument, who + "a param link is out of range");
  p->paramLinks = std::move(ui.paramLinks);

  // Crossfade scratch: this packet's own outputs while it fades out later.
  const auto slots = static_cast<uint32_t>(p->outPtrs.size());
  p->scratch = AlignedBuffer<float>(static_cast<size_t>(std::max(slots, 1u)) * stride_);
  p->fadePtrs.assign(slots, nullptr);
  for (uint32_t s = 0; s < slots; ++s)
    if (p->outPtrs[s] != nullptr) p->fadePtrs[s] = p->scratch.data() + static_cast<size_t>(s) * stride_;

  for (const graph::ParamSlot& ps : io.params) p->params.push_back(ps.defaultValue);
  PublishInfo info;
  info.sequence = p->seq;
  if (U.last != nullptr) {
    Packet& prev = *U.last;
    ProgramImage& pi = *prev.image;
    auto plan = buildTransferPlan(pi.stateLayout(), pi.stateArena(), img.stateLayout(), img.stateArena());
    if (!plan.ok()) return plan.error();
    p->plan = std::move(plan.value());
    std::unordered_map<uint64_t, uint32_t> prevSlot;
    const auto& pp = pi.io().params;
    for (uint32_t i = 0; i < pp.size(); ++i) prevSlot.try_emplace(pp[i].key, i);
    for (uint32_t i = 0; i < io.params.size(); ++i)
      if (const auto it = prevSlot.find(io.params[i].key); it != prevSlot.end()) p->paramMap.emplace_back(i, it->second);
    info.plan = p->plan;
    info.paramsCarried = static_cast<uint32_t>(p->paramMap.size());
    // Fade lanes: every produced bus channel, from the previous packet's scratch slot for it.
    for (uint32_t s = 0; s < slots; ++s) {
      if (p->outPtrs[s] == nullptr) continue;
      const float* old = nullptr;
      for (size_t t = 0; t < prev.outPtrs.size(); ++t)
        if (prev.outPtrs[t] == p->outPtrs[s]) old = prev.fadePtrs[t];
      p->fade.push_back({p->outPtrs[s], old != nullptr ? old : zeros});
    }
  }

  if (U.cfg.voices) {
    if (!U.lanesPrepared) {
      if (Status st = U.voices->prepareLanes(io, U.cfg.voice); !st.ok()) return st.error();
      U.lanesPrepared = true;
    }
    auto b = U.voices->makeBinding(&img, ui.voiceSlots.doneSlot, ui.voiceSlots.monitorMask);
    if (!b.ok()) return b.error();
    p->binding = std::move(b.value());
  }
  p->image = std::move(ui.image);

  Packet* raw = p.get();
  U.inflight.push_back(raw);
  if (!U.ring.tryPush(raw)) {
    U.inflight.pop_back();
    return projectError(ErrorCode::Overflow, who + "too many images pending; retry after the audio thread has taken some");
  }
  p.release();  // owned through inflight (then the reclaimer)
  U.last = raw;
  ++U.nextSeq;
  return info;
}

size_t ProjectProgram::collect() {
  std::lock_guard lock(mutex_);
  for (auto& up : units_) {
    Unit& U = *up;
    const uint64_t through = U.retiredThrough.load(std::memory_order_acquire);
    size_t k = 0;
    while (k < U.inflight.size() && U.inflight[k]->seq <= through) {
      reclaimer_.retire(U.inflight[k], [](void* q) noexcept { delete static_cast<Packet*>(q); });
      ++k;
    }
    U.inflight.erase(U.inflight.begin(), U.inflight.begin() + static_cast<std::ptrdiff_t>(k));
  }
  const size_t n = reclaimer_.collect();
  freed_.fetch_add(n, std::memory_order_relaxed);
  return n;
}

size_t ProjectProgram::imagesAlive() const {
  std::lock_guard lock(mutex_);
  size_t n = reclaimer_.pending();
  for (const auto& u : units_) n += u->inflight.size();
  return n;
}

uint64_t ProjectProgram::imagesFreed() const { return freed_.load(std::memory_order_relaxed); }

const ProgramImage* ProjectProgram::lastPublished(uint32_t unit) const {
  std::lock_guard lock(mutex_);
  const Unit& U = *units_[unit];
  return U.last != nullptr ? U.last->image.get() : nullptr;
}

std::span<float> ProjectProgram::params(uint32_t unit) noexcept {  // RT-safe
  Packet* p = units_[unit]->cur;
  return p != nullptr ? std::span<float>(p->params) : std::span<float>();
}

const ProgramImage* ProjectProgram::current(uint32_t unit) const noexcept {  // RT-safe
  const Packet* p = units_[unit]->cur;
  return p != nullptr ? p->image.get() : nullptr;
}

bool ProjectProgram::crossfading(uint32_t unit) const noexcept { return units_[unit]->fadeFrom != nullptr; }  // RT-safe

VoiceEngine* ProjectProgram::voices(uint32_t unit) noexcept { return units_[unit]->voices.get(); }  // RT-safe

const UnitStats& ProjectProgram::unitStats(uint32_t unit) const noexcept { return units_[unit]->stats; }  // RT-safe

float ProjectProgram::unitPeak(const Unit& u) const noexcept {  // RT-safe
  float peak = 0.0f;
  for (const float* ch : u.ownChannels) peak = std::max(peak, peakAbs(ch, lastFrames_));
  return peak;
}

void ProjectProgram::trySwap(Unit& U) noexcept {  // RT-safe
  Packet* p = nullptr;
  if (!U.ring.tryPop(p)) return;
  ++U.stats.swaps;
  U.stats.sequence = p->seq;
  Packet* old = U.cur;
  if (old == nullptr) {  // the unit's first image
    if (U.voices) U.voices->attach(p->binding.get(), nullptr);
    U.cur = p;
    ++U.stats.instantSwaps;
    U.stats.transferBytes = 0;
    return;
  }
  bool fade = p->mode == SwapMode::Crossfade;
  if (p->mode == SwapMode::Auto)
    fade = transport_.playing || (U.voices && U.voices->activeGroups() > 0) || unitPeak(U) > silence_;
  const StateArena from = old->image->stateArena(), to = p->image->stateArena();
  const uint32_t* groupMask = U.voices && U.cfg.voice.resetOnNoteOn ? U.voices->laneMasks() : nullptr;
  executeTransfer(p->plan, from, to, groupMask);
  uint64_t bytes = p->plan.globalBytes;
  const uint32_t groups = std::min({p->plan.voiceGroups, from.voiceGroups, to.voiceGroups});
  for (uint32_t g = 0; g < groups; ++g)
    if (groupMask == nullptr || groupMask[g] != 0) bytes += p->plan.voiceBytes;
  U.stats.transferBytes = bytes;
  for (const auto& [dst, src] : p->paramMap) p->params[dst] = old->params[src];
  if (U.voices) U.voices->attach(p->binding.get(), fade ? old->binding.get() : nullptr);
  U.cur = p;
  if (fade) {
    U.fadeFrom = old;
    U.fadePos = 0;
    U.fadeLen = p->fadeFrames;
    ++U.stats.crossfades;
  } else {
    U.retiredThrough.store(old->seq, std::memory_order_release);
    ++U.stats.instantSwaps;
  }
}

bool ProjectProgram::runUnit(Unit& U, uint32_t frames, int64_t time) noexcept {  // RT-safe
  if (U.fadeFrom == nullptr) trySwap(U);
  Packet* p = U.cur;
  if (p == nullptr) return false;  // no image yet: its buses keep reading zeros
  Packet* old = U.fadeFrom;
  for (const auto& [slot, param] : p->paramLinks) p->params[slot] = projectParams_[param];
  ProcessContext ctx;
  ctx.frames = frames;
  ctx.sampleRate = cfg_.sampleRate;
  ctx.sampleTime = time;
  ctx.transport = transport_;
  ctx.in = p->inPtrs.data();
  ctx.numIn = static_cast<uint32_t>(p->inPtrs.size());
  ctx.out = p->outPtrs.data();
  ctx.numOut = static_cast<uint32_t>(p->outPtrs.size());
  ctx.params = p->params.data();
  ctx.numParams = static_cast<uint32_t>(p->params.size());
  ProcessContext fctx;
  if (old != nullptr) {
    for (const auto& [slot, param] : old->paramLinks) old->params[slot] = projectParams_[param];
    fctx = ctx;
    fctx.in = old->inPtrs.data();
    fctx.numIn = static_cast<uint32_t>(old->inPtrs.size());
    fctx.out = old->fadePtrs.data();
    fctx.numOut = static_cast<uint32_t>(old->fadePtrs.size());
    fctx.params = old->params.data();
    fctx.numParams = static_cast<uint32_t>(old->params.size());
  }
  if (U.voices) {
    U.eventBuf.clear();
    if (U.events.fn != nullptr) U.events.fn(U.events.user, time, frames, U.eventBuf);
    ctx.events = &U.eventBuf.list();
    U.voices->process(ctx, old != nullptr ? &fctx : nullptr);
  } else {
    p->image->process(ctx);
    if (old != nullptr) old->image->process(fctx);
  }
  if (old == nullptr) return false;
  for (const Packet::FadeLane& l : p->fade) crossfade(l.bus, l.old, l.bus, frames, U.fadePos, U.fadeLen);
  ++U.stats.fadeBlocks;
  U.fadePos += frames;
  if (U.fadePos >= U.fadeLen) {  // the fade is over: the old image is no longer used
    U.fadeFrom = nullptr;
    if (U.voices) U.voices->attach(p->binding.get(), nullptr);
    U.retiredThrough.store(old->seq, std::memory_order_release);
  }
  return true;
}

void ProjectProgram::process(const AudioBlock& block) noexcept {  // RT-safe
  const uint32_t maxBlock = cfg_.maxBlockFrames;
  const uint32_t numOut = std::min(cfg_.numOut, block.numOut);
  for (uint32_t off = 0; off < block.frames; off += maxBlock) {
    const uint32_t n = std::min(maxBlock, block.frames - off);
    const int64_t time = block.frameTime + off;
    if (cfg_.hook.fn != nullptr) cfg_.hook.fn(cfg_.hook.user, time, n, transport_, projectParams_);
    uint32_t fading = 0;
    for (auto& u : units_) fading += runUnit(*u, n, time) ? 1u : 0u;
    for (uint32_t c = 0; c < numOut; ++c) {
      float* dst = block.out[c] + off;
      const uint32_t ob = layout_.outputBus;
      if (ob == kNoBus || (layout_.busWidth[ob] > 1 && c >= layout_.busWidth[ob])) {
        std::memset(dst, 0, sizeof(float) * n);
        continue;
      }
      const uint16_t ch = layout_.busWidth[ob] == 1 ? uint16_t{0} : static_cast<uint16_t>(c);
      std::memcpy(dst, bus(ob, ch), sizeof(float) * n);
    }
    ++stats_.blocks;
    if (fading > 0) ++stats_.fadeBlocks;
    stats_.maxFadingUnits = std::max(stats_.maxFadingUnits, fading);
    if (cfg_.hook.fn == nullptr && transport_.playing)
      transport_.beat += static_cast<double>(n) * transport_.beatsPerFrame;
    lastFrames_ = n;
  }
  reclaimer_.quiescent(reader_);
}

}  // namespace pq::engine
