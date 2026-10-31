// The circuit editor on a device of the project, played by the project (T4.4-fix1): pq/app/device_player.h.
#include "pq/app/device_player.h"

#include <algorithm>

#include "pq/app/project_link.h"
#include "pq/document/lower.h"

namespace pq::app {

// ---- DeviceInspector -------------------------------------------------------------------------------------------------

DeviceInspector::DeviceInspector() {
  for (uint32_t i = 0; i < kProbeRings; ++i) rings_.push_back(std::make_unique<TapRing>(1, kProbeRingFrames, 1024));
  levels_ = std::make_unique<ProbeLevel[]>(kProbeLevels);
  ringSlots_.resize(kProbeRings);
  levelSlots_.resize(kProbeLevels);
}

DeviceInspector::~DeviceInspector() = default;

void DeviceInspector::setSignals(std::span<const uint32_t> ids) {
  std::lock_guard lock(mutex_);
  wantSignals_.assign(ids.begin(), ids.end());
}

// Under mutex_: frees the slots of probes that are gone and binds a slot per probe. A freed slot is reused only once
// no image that was bound to it is alive (M4 review L4): each image holds the leases of the slots it writes
// (bind()'s return value goes to UnitImage::keepAlive, released by the program's epoch reclaimer after the audio thread
// has left that image), so a slot's lease count back at 1 (ours) means nothing can write it any more. When every ring
// is still leased (a re-inspection while an older image plays on, e.g. an edit held for its JIT image) the pool grows,
// up to kMaxProbeRings; the levels' pool (kProbeLevels) is large enough not to need that.
void DeviceInspector::bindProbes(const graph::Schedule& schedule) {
  bindingLeases_.reset();  // the previous binding's images hold their own copies
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
  std::vector<std::shared_ptr<const void>> leases;  // the slots this binding writes
  auto slotFor = [&](std::vector<TapSlot>& slots, uint32_t id, bool& fresh, bool rings) -> int {
    fresh = false;
    for (uint32_t i = 0; i < slots.size(); ++i)
      if (slots[i].bound && slots[i].id == id) return static_cast<int>(i);
    int best = -1;
    for (uint32_t i = 0; i < slots.size() && best < 0; ++i)
      if (!slots[i].bound && slots[i].lease.use_count() == 1) best = static_cast<int>(i);  // no image holds it
    if (best < 0 && rings && slots.size() < kMaxProbeRings) {  // all leased: one more ring (allocates; not RT)
      rings_.push_back(std::make_unique<TapRing>(1, kProbeRingFrames, 1024));
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
  for (const uint32_t id : ids) {
    engine::ProbeTap t;
    t.id = id;
    bool fresh = false;
    if (const int l = slotFor(levelSlots_, id, fresh, false); l >= 0) {
      t.level = &levels_[static_cast<uint32_t>(l)];
      leases.push_back(levelSlots_[static_cast<uint32_t>(l)].lease);
    }
    if (has(wantSignals_, id))
      if (const int r = slotFor(ringSlots_, id, fresh, true); r >= 0) {
        t.ring = rings_[static_cast<uint32_t>(r)].get();
        leases.push_back(ringSlots_[static_cast<uint32_t>(r)].lease);
        if (fresh) t.ring->addReader();
      }
    binding_.push_back(t);
  }
  bindingLeases_ = std::make_shared<const std::vector<std::shared_ptr<const void>>>(std::move(leases));
}

std::shared_ptr<const void> DeviceInspector::bind(const engine::CompiledUnit& cu, engine::ProgramImage& image) {
  if (!cu.inspect || !cu.schedule) return nullptr;  // not the inspected unit: nothing bound, nothing measured
  std::lock_guard lock(mutex_);
  if (cu.schedule.get() != lastSchedule_) {  // a new program of the unit: a new generation (the JIT image: the same)
    lastSchedule_ = cu.schedule.get();
    lastHold_ = cu.schedule;
    const uint64_t gen = generation_.load(std::memory_order_relaxed) + 1;
    auto pp = std::make_shared<views::circuit::PlayedProgram>();
    pp->generation = gen;
    pp->schedule = *cu.schedule;
    pp->instanceParent = cu.inspect->instanceParent;
    pp->instanceCall = cu.inspect->instanceCall;
    pp->nodeInstance = cu.inspect->nodeInstance;
    pp->nodeSource = cu.inspect->nodeSource;
    pp->nodeWhat = cu.inspect->nodeWhat;
    for (const graph::ProbeRef& pr : cu.schedule->probes) pp->probeIds.push_back(pr.id);
    played_ = std::move(pp);
    trackId_.store(cu.inspect->trackId, std::memory_order_release);
    bindProbes(*cu.schedule);
    generation_.store(gen, std::memory_order_release);
  }
  meter_.setTag(generation_.load(std::memory_order_relaxed));
  engine::InspectBinding b;
  b.probes = binding_;
  b.meter = &meter_;
  if (!image.bindInspect(b)) return nullptr;  // an image that cannot be inspected plays without probes (writes nothing)
  return bindingLeases_;  // the image holds the leases of the tap slots it writes (M4 review L4)
}

TapRing* DeviceInspector::probeSignal(uint32_t id) noexcept {
  std::lock_guard lock(mutex_);
  for (uint32_t i = 0; i < ringSlots_.size(); ++i)
    if (ringSlots_[i].bound && ringSlots_[i].id == id) return rings_[i].get();
  return nullptr;
}

const ProbeLevel* DeviceInspector::probeLevel(uint32_t id) const noexcept {
  std::lock_guard lock(mutex_);
  for (uint32_t i = 0; i < levelSlots_.size(); ++i)
    if (levelSlots_[i].bound && levelSlots_[i].id == id) return &levels_[i];
  return nullptr;
}

bool DeviceInspector::readCost(views::circuit::PatchPlayer::CostReading& out) {
  engine::CostMeter::Reading r;
  if (!meter_.read(r)) return false;
  out.generation = r.tag;
  out.totalNsPerFrame = r.totalNsPerFrame;
  out.regionNsPerFrame = std::move(r.regionNsPerFrame);
  out.units = std::move(r.units);
  return true;
}

std::shared_ptr<const views::circuit::PlayedProgram> DeviceInspector::played() const {
  std::lock_guard lock(mutex_);
  return played_;
}

// ---- DevicePlayer ----------------------------------------------------------------------------------------------------

DevicePlayer::DevicePlayer(ProjectLink& link, doc::DeviceId device) : link_(link), device_(device) {}

DevicePlayer::~DevicePlayer() {
  link_.inspector().setMetering(false);
  link_.setDeviceInspect(device_, {}, {}, false);
}

double DevicePlayer::sampleRate() const { return link_.config().sampleRate; }

Status DevicePlayer::play(const graph::Schedule&, const graph::TypedPatch&) { return Ok{}; }  // the project plays it

void DevicePlayer::stop() {
  link_.inspector().setMetering(false);
  link_.setDeviceInspect(device_, {}, {}, false);
}

void DevicePlayer::setProbeSignals(std::span<const uint32_t> ids) { link_.inspector().setSignals(ids); }
TapRing* DevicePlayer::probeSignal(uint32_t id) noexcept { return link_.inspector().probeSignal(id); }
const ProbeLevel* DevicePlayer::probeLevel(uint32_t id) const noexcept { return link_.inspector().probeLevel(id); }
void DevicePlayer::setMetering(bool on) { link_.inspector().setMetering(on); }
bool DevicePlayer::readCost(CostReading& out) { return link_.inspector().readCost(out); }
uint64_t DevicePlayer::generation() const noexcept { return link_.inspector().generation(); }

void DevicePlayer::setProbeTargets(std::span<const graph::ProbeTarget> targets, std::span<const uint32_t> ids) {
  link_.setDeviceInspect(device_, std::vector<graph::ProbeTarget>(targets.begin(), targets.end()),
                         std::vector<uint32_t>(ids.begin(), ids.end()), true);
}

const views::circuit::PlayedProgram* DevicePlayer::playedProgram() const {
  hold_ = link_.inspector().played();
  return hold_.get();
}

std::vector<graph::NodeId> DevicePlayer::pathPrefix() const {
  // The unit's call node of the track (its NodeId is the track id), then the device's call in the track's patch.
  return {graph::NodeId{link_.inspector().trackId()},
          graph::NodeId{doc::nodeIdFor(doc::NodeRole::Device, device_.value, 0)}};
}

}  // namespace pq::app
