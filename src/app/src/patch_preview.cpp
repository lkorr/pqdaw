// The circuit editor's audio preview (T4.4): pq/app/patch_preview.h.

#include "pq/app/patch_preview.h"

#include <algorithm>
#include <cmath>

#include "pq/engine/interpreter.h"
#include "pq/engine/voices.h"

namespace pq::app {

namespace {
constexpr float kPreviewArpHz[] = {261.626f, 329.628f, 391.995f, 523.251f};

}

PatchPreview::PatchPreview(const PreviewConfig& config)
    : cfg_(config), arp_(std::vector<float>(std::begin(kPreviewArpHz), std::end(kPreviewArpHz)), config.sampleRate) {
  gain_ = static_cast<float>(std::pow(10.0, static_cast<double>(cfg_.gainDb) / 20.0));
  scratch_ = AlignedBuffer<float>(static_cast<size_t>(cfg_.maxDeviceFrames) * cfg_.numOut);
  outs_.resize(cfg_.numOut);
  for (uint32_t c = 0; c < cfg_.numOut; ++c) outs_[c] = scratch_.data() + static_cast<size_t>(c) * cfg_.maxDeviceFrames;
  // T4.5: the probe taps (fixed pools: nothing is allocated while probes come and go).
  for (uint32_t i = 0; i < kProbeRings; ++i)
    rings_.push_back(std::make_unique<TapRing>(1, kProbeRingFrames, std::max<uint32_t>(cfg_.maxBlockFrames, 1024)));
  levels_ = std::make_unique<ProbeLevel[]>(kProbeLevels);
  ringSlots_.resize(kProbeRings);
  levelSlots_.resize(kProbeLevels);
  if (cfg_.jit.fn) {
    engine::BackgroundCompilerConfig bc;
    bc.backend = cfg_.jit;
    bc.deliver = &PatchPreview::deliver;
    bc.user = this;
    jit_ = std::make_unique<engine::BackgroundCompiler>(bc);
  }
}

void PatchPreview::makeHost(int kind) {
  engine::HostConfig hc;
  hc.sampleRate = cfg_.sampleRate;
  hc.maxBlockFrames = cfg_.maxBlockFrames;
  hc.numOut = cfg_.numOut;
  if (kind == 1) {
    hc.voices = true;
    hc.voice.maxVoices = kPlayVoices;
    hc.events = arp_.source();
  }
  hosts_[kind] = std::make_unique<engine::ProgramHost>(hc);
}

void PatchPreview::setSampleRate(double rate) {
  std::lock_guard lock(mutex_);
  active_.store(false, std::memory_order_release);
  fresh_ = true;
  latest_.reset();
  if (rate == cfg_.sampleRate) return;
  cfg_.sampleRate = rate;
  arp_ = Arpeggio(std::vector<float>(std::begin(kPreviewArpHz), std::end(kPreviewArpHz)), rate);
  current_.store(nullptr, std::memory_order_release);
  currentKind_ = -1;
  hosts_[0].reset();
  hosts_[1].reset();
}

PatchPreview::~PatchPreview() {
  jit_.reset();  // joins the worker (a running delivery finishes first)
}

const graph::StateLayout* PatchPreview::stateLayout() const {
  if (currentKind_ < 0) return nullptr;
  const engine::ProgramImage* last = hosts_[currentKind_]->lastPublished();
  return last ? &last->stateLayout() : nullptr;
}

Status PatchPreview::play(const graph::Schedule& schedule, const graph::TypedPatch& scheduled) {
  engine::InterpretOptions io;
  io.maxBlockFrames = cfg_.maxBlockFrames;
  io.maxVoiceGroups = engine::voiceGroupsFor(kPlayVoices);
  auto image = engine::buildInterpreted(schedule, io);
  if (!image) return std::move(image).error();
  uint32_t outs = 0, voiceOuts = 0;
  for (const engine::IoSlot& s : (*image)->io().globalOut) outs += s.isExport ? 0u : 1u;
  for (const engine::IoSlot& s : (*image)->io().voiceOut) voiceOuts += s.isExport ? 0u : 1u;
  if (outs == 0 && voiceOuts == 0)
    return Error{ErrorCode::Unsupported, "the patch has no `out` (global or voice) to play", {}};
  engine::VoiceSlots slots;
  slots.doneSlot = engine::findVoiceSlot((*image)->io(), scheduled.patch, "done");
  const int kind = (*image)->io().hasVoiceStages ? 1 : 0;
  std::lock_guard lock(mutex_);
  if (!hosts_[kind]) makeHost(kind);
  // T4.5: taps for this schedule's probes, and the cost meter (tagged with the generation this play() makes). Retired
  // images are freed first, so their taps' leases are released (M4 review L4).
  for (auto& h : hosts_)
    if (h) h->collect();
  const uint64_t gen = generation_.load(std::memory_order_relaxed) + 1;
  bindProbes(schedule);
  std::vector<std::shared_ptr<const void>> leases = bind(**image, gen);
  engine::SwapPolicy policy;
  // Starting (or switching between a global and a voice patch): at once; an edit of what plays: hot swap.
  policy.mode = fresh_ || kind != currentKind_ ? engine::SwapMode::Instant : engine::SwapMode::Auto;
  auto published = hosts_[kind]->publish(std::move(*image), policy, slots, std::move(leases));
  if (!published) {
    ++stats_.failures;
    return std::move(published).error();
  }
  ++stats_.published;
  generation_.store(gen, std::memory_order_release);
  stats_.lastMatched = published->plan.matched;
  stats_.lastFresh = published->plan.fresh + published->plan.mismatched;
  stats_.lastDropped = published->plan.dropped;
  // T4.6: the names of the params setParam() writes (root params keep their names through inference).
  paramKeys_.clear();
  for (const graph::ParamSlot& ps : schedule.params)
    if (ps.node < scheduled.patch.nodeCount()) paramKeys_.emplace_back(scheduled.patch.nodeName(ps.node), ps.key);
  fresh_ = false;
  slots_ = slots;
  currentKind_ = kind;
  jitKind_ = kind;
  current_.store(hosts_[kind].get(), std::memory_order_release);
  const bool started = !active_.exchange(true, std::memory_order_acq_rel);
  if (started && onActiveChanged) onActiveChanged(true);
  if (jit_) {
    latest_ = std::make_shared<const graph::Schedule>(schedule);
    engine::BackgroundJob job;
    job.schedule = latest_;
    job.maxBlockFrames = cfg_.maxBlockFrames;
    job.maxVoiceGroups = io.maxVoiceGroups;
    jit_->want({job});
  }
  return Ok{};
}

void PatchPreview::stop() {
  bool was = false;
  {
    std::lock_guard lock(mutex_);
    was = active_.exchange(false, std::memory_order_acq_rel);
    fresh_ = true;
    latest_.reset();  // a JIT image still on its way is dropped
  }
  if (was && onActiveChanged) onActiveChanged(false);
}

void PatchPreview::deliver(void* self, const engine::BackgroundJob& job,
                           Result<std::unique_ptr<engine::ProgramImage>> image) {
  auto& p = *static_cast<PatchPreview*>(self);
  std::lock_guard lock(p.mutex_);
  if (!image || job.schedule != p.latest_) {  // failed (the interpreter keeps playing) or superseded
    ++p.stats_.staleJit;
    return;
  }
  // T4.5: the same taps as the interpreter's image (and their leases, M4 review L4).
  std::vector<std::shared_ptr<const void>> leases = p.bind(**image, p.generation_.load(std::memory_order_relaxed));
  engine::SwapPolicy policy;
  policy.mode = engine::SwapMode::Instant;  // bit-identical to the interpreter's image: full state transfer, no fade
  // A full ring (a burst of edits the audio thread has not taken yet) drops this image: the interpreter's keeps
  // playing, and the next edit compiles again.
  if (p.hosts_[p.jitKind_]->publish(std::move(*image), policy, p.slots_, std::move(leases))) ++p.stats_.jitPublished;
  else ++p.stats_.failures;
}

bool PatchPreview::setParam(std::string_view name, float value) {
  uint64_t key = 0;
  {
    std::lock_guard lock(mutex_);
    if (!active_.load(std::memory_order_acquire)) return false;
    const auto it = std::find_if(paramKeys_.begin(), paramKeys_.end(), [&](const auto& p) { return p.first == name; });
    if (it == paramKeys_.end()) return false;
    key = it->second;
  }
  return paramWrites_.tryPush(ParamWrite{key, value});  // the UI thread is the ring's only producer
}

// RT-safe: a few writes per block at most, each a scan of the current image's params (a device has ≤ ~100).
void PatchPreview::applyParamWrites(engine::ProgramHost& host) noexcept {
  ParamWrite w;
  while (paramWrites_.tryPop(w)) {
    const engine::ProgramImage* img = host.current();
    if (img == nullptr) continue;
    const auto& ps = img->io().params;
    const std::span<float> block = host.params();
    for (size_t i = 0; i < ps.size() && i < block.size(); ++i)
      if (ps[i].key == w.key) {
        block[i] = w.value;
        break;
      }
  }
}

// ---- T4.5: probes and cost meters -------------------------------------------------------------------------------------

void PatchPreview::setProbeSignals(std::span<const uint32_t> ids) {
  std::lock_guard lock(mutex_);
  wantSignals_.assign(ids.begin(), ids.end());
}

// Under mutex_. Frees the slots of probes that are gone and binds a slot per probe of `schedule`. A freed slot is
// reused only once no image that was bound to it is alive (M4 review L4, as DeviceInspector): each image holds the
// leases of the slots it writes (the host's keepAlive, released after the audio thread has let go of the image), so a
// lease count back at 1 (ours) means nothing writes the slot any more. When every ring is still leased the pool grows,
// up to kMaxProbeRings (a probe beyond that gets no ring: its signal is not shown); the levels' pool does not need to.
void PatchPreview::bindProbes(const graph::Schedule& schedule) {
  auto has = [](const auto& v, uint32_t id) { return std::find(v.begin(), v.end(), id) != v.end(); };
  std::vector<uint32_t> ids;
  for (const graph::ProbeRef& p : schedule.probes) ids.push_back(p.id);
  for (uint32_t i = 0; i < ringSlots_.size(); ++i) {
    TapSlot& sl = ringSlots_[i];
    if (sl.bound && (!has(ids, sl.id) || !has(wantSignals_, sl.id))) {
      sl.bound = false;
      rings_[i]->removeReader();
    }
  }
  for (TapSlot& sl : levelSlots_)
    if (sl.bound && !has(ids, sl.id)) sl.bound = false;
  auto slotFor = [&](std::vector<TapSlot>& slots, uint32_t id, bool& fresh, bool rings) -> int {
    fresh = false;
    for (uint32_t i = 0; i < slots.size(); ++i)
      if (slots[i].bound && slots[i].id == id) return static_cast<int>(i);
    int best = -1;
    for (uint32_t i = 0; i < slots.size() && best < 0; ++i)
      if (!slots[i].bound && slots[i].lease.use_count() == 1) best = static_cast<int>(i);  // no image holds it
    if (best < 0 && rings && slots.size() < kMaxProbeRings) {  // all leased: one more ring (allocates; not RT)
      rings_.push_back(std::make_unique<TapRing>(1, kProbeRingFrames, std::max<uint32_t>(cfg_.maxBlockFrames, 1024)));
      slots.emplace_back();
      best = static_cast<int>(slots.size() - 1);
    }
    if (best >= 0) {
      slots[static_cast<uint32_t>(best)].bound = true;
      slots[static_cast<uint32_t>(best)].id = id;
      fresh = true;
    }
    return best;
  };
  binding_.clear();
  bindingLeases_.clear();
  for (const uint32_t id : ids) {
    engine::ProbeTap t;
    t.id = id;
    bool fresh = false;
    if (const int l = slotFor(levelSlots_, id, fresh, false); l >= 0) {
      t.level = &levels_[static_cast<uint32_t>(l)];
      bindingLeases_.push_back(levelSlots_[static_cast<uint32_t>(l)].lease);
    }
    if (has(wantSignals_, id))
      if (const int r = slotFor(ringSlots_, id, fresh, true); r >= 0) {
        t.ring = rings_[static_cast<uint32_t>(r)].get();
        bindingLeases_.push_back(ringSlots_[static_cast<uint32_t>(r)].lease);
        if (fresh) t.ring->addReader();
      }
    binding_.push_back(t);
  }
}

std::vector<std::shared_ptr<const void>> PatchPreview::bind(engine::ProgramImage& image, uint64_t gen) {
  meter_.setTag(gen);
  engine::InspectBinding b;
  b.probes = binding_;
  b.meter = &meter_;
  if (!image.bindInspect(b)) return {};  // an image that cannot be inspected plays without probes (writes nothing)
  return bindingLeases_;
}

TapRing* PatchPreview::probeSignal(uint32_t id) noexcept {
  std::lock_guard lock(mutex_);
  for (uint32_t i = 0; i < ringSlots_.size(); ++i)
    if (ringSlots_[i].bound && ringSlots_[i].id == id) return rings_[i].get();
  return nullptr;
}

const ProbeLevel* PatchPreview::probeLevel(uint32_t id) const noexcept {
  std::lock_guard lock(mutex_);
  for (uint32_t i = 0; i < levelSlots_.size(); ++i)
    if (levelSlots_[i].bound && levelSlots_[i].id == id) return &levels_[i];
  return nullptr;
}

bool PatchPreview::readCost(CostReading& out) {
  engine::CostMeter::Reading r;
  if (!meter_.read(r)) return false;
  out.generation = r.tag;
  out.totalNsPerFrame = r.totalNsPerFrame;
  out.regionNsPerFrame = std::move(r.regionNsPerFrame);
  out.units = std::move(r.units);
  return true;
}

bool PatchPreview::waitForJit(double seconds) { return !jit_ || jit_->waitIdle(seconds); }

PatchPreview::Stats PatchPreview::stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

void PatchPreview::onBlock(void* self, const engine::AudioBlock& b) noexcept {
  static_cast<PatchPreview*>(self)->process(b);
}

// RT-safe: the host renders into the preallocated scratch, which is added to the device block at the preview's level.
void PatchPreview::process(const engine::AudioBlock& b) noexcept {
  if (!active_.load(std::memory_order_acquire)) return;
  engine::ProgramHost* host = current_.load(std::memory_order_acquire);
  if (!host) return;
  uint32_t done = 0;
  while (done < b.frames) {
    const uint32_t n = std::min(b.frames - done, cfg_.maxDeviceFrames);
    engine::AudioBlock local;
    local.out = outs_.data();
    local.numOut = cfg_.numOut;
    local.frames = n;
    local.frameTime = b.frameTime + done;
    applyParamWrites(*host);
    host->process(local);
    tap_.write(outs_.data(), cfg_.numOut, n);  // one relaxed load while no display reads it
    for (uint32_t c = 0; c < b.numOut; ++c) {
      const float* src = outs_[std::min(c, cfg_.numOut - 1)];
      float* dst = b.out[c] + done;
      for (uint32_t f = 0; f < n; ++f) dst[f] += gain_ * src[f];
    }
    done += n;
  }
}

}  // namespace pq::app
