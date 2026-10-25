// Per-track project runner (T1.x-arch1; topology changes T3.17): pq/engine/project_program.h describes the protocol.
#include "pq/engine/project_program.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "crossfade.h"

namespace pq::engine {

using detail::crossfade;
using detail::peakAbs;

struct ProjectProgram::Packet {
  uint64_t seq = 0;
  std::vector<std::shared_ptr<const void>> keepAlive;     // UnitImage::keepAlive (destroyed after the image)
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
  ~Unit() {
    for (Packet* p : inflight) delete p;
  }
  UnitLayout cfg;
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
  EventBuffer eventBuf{1};
  std::unique_ptr<VoiceEngine> voices;
  UnitStats stats;
};

// A bus: `width` planar channels of `stride` floats at fixed addresses for its whole life (shared by every topology
// that keeps the bus).
struct ProjectProgram::BusStore {
  BusStore(uint16_t w, uint32_t s, std::string k, bool m)
      : data(static_cast<size_t>(w) * s), width(w), stride(s), key(std::move(k)), metered(m) {}
  AlignedBuffer<float> data;  // zeroed
  uint16_t width;
  uint32_t stride;
  std::string key;
  bool metered;
  // Meter (audio thread accumulates and publishes; readMeters() reads and asks for a restart through meterGen_).
  uint32_t seenGen = 0;
  float accPeak[2] = {0.0f, 0.0f};
  double accSq[2] = {0.0, 0.0};
  uint64_t accN = 0;
  std::atomic<float> pubPeak[2] = {0.0f, 0.0f};
  std::atomic<float> pubMs[2] = {0.0f, 0.0f};
  float* channel(uint16_t c) noexcept { return data.data() + static_cast<size_t>(c) * stride; }
};

// One entry of a topology's run order.
struct ProjectProgram::Slot {
  Unit* unit = nullptr;
  EventSource events;
  std::vector<float*> own;                           // the bus channels the unit produces (audibility peak)
  bool zombie = false;
  std::vector<std::pair<Unit*, uint64_t>> waitFor;  // zombie: (reader, sequence it must make current, then not fade)
};

struct ProjectProgram::Topology {
  uint64_t seq = 0;
  std::vector<Slot> order;
  std::unique_ptr<std::atomic<uint8_t>[]> alive;     // per order entry (zombies die: RT writes, collect reads)
  std::vector<std::shared_ptr<BusStore>> stores;     // per bus of the layout
  std::vector<std::shared_ptr<BusStore>> extra;      // buses only zombies still use
  BusStore* output = nullptr;
  std::vector<std::pair<uint32_t, Packet*>> swaps;   // (order index, packet) made current at the switch
  std::vector<BusStore*> zeroAtSwitch;               // buses whose zombies were dropped
};

namespace {

Error projectError(ErrorCode code, const std::string& m) { return Error{code, "project: " + m, {}}; }

bool meteredBus(const ProjectLayout& l, size_t b) { return !l.busMetered.empty() && l.busMetered[b] != 0; }

// Peak |x| and Σ x² of n samples (AVX2). RT-safe.
void levelOf(const float* x, uint32_t n, float& peak, double& sumSq) noexcept {
  const __m256 absMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
  __m256 m = _mm256_setzero_ps(), sq = _mm256_setzero_ps();
  uint32_t f = 0;
  for (; f + 8 <= n; f += 8) {
    const __m256 v = _mm256_loadu_ps(x + f);
    m = _mm256_max_ps(m, _mm256_and_ps(v, absMask));
    sq = _mm256_fmadd_ps(v, v, sq);
  }
  alignas(32) float tm[8], ts[8];
  _mm256_store_ps(tm, m);
  _mm256_store_ps(ts, sq);
  float p = 0.0f;
  double s = 0.0;
  for (int i = 0; i < 8; ++i) {
    p = std::max(p, tm[i]);
    s += ts[i];
  }
  for (; f < n; ++f) {
    p = std::max(p, std::fabs(x[f]));
    s += static_cast<double>(x[f]) * x[f];
  }
  peak = std::max(peak, p);
  sumSq += s;
}

Status validateLayout(const ProjectConfig& config, const ProjectLayout& layout) {
  if (layout.units.empty()) return projectError(ErrorCode::InvalidArgument, "no units");
  if (layout.busWidth.size() != layout.busProducer.size())
    return projectError(ErrorCode::InvalidArgument, "busWidth and busProducer differ in size");
  if (!layout.busKey.empty() && layout.busKey.size() != layout.busWidth.size())
    return projectError(ErrorCode::InvalidArgument, "busKey must be empty or have one key per bus");
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
  return Ok{};
}

}  // namespace

Result<std::unique_ptr<ProjectProgram>> ProjectProgram::create(const ProjectConfig& config, ProjectLayout layout) {
  if (Status st = validateLayout(config, layout); !st.ok()) return st.error();
  return std::unique_ptr<ProjectProgram>(new ProjectProgram(config, std::move(layout)));
}

std::unique_ptr<ProjectProgram::Unit> ProjectProgram::makeUnit(const UnitLayout& cfg) {
  auto u = std::make_unique<Unit>(std::bit_ceil(std::max(2u, cfg_.maxPendingPerUnit)));
  u->cfg = cfg;
  if (u->cfg.voices) {
    u->voices = std::make_unique<VoiceEngine>();
    u->eventBuf = EventBuffer(std::max(cfg_.eventCapacity, 1u));
  }
  return u;
}

ProjectProgram::ProjectProgram(const ProjectConfig& config, ProjectLayout layout)
    : cfg_(config), layout_(std::move(layout)) {
  cfg_.maxBlockFrames = std::max(cfg_.maxBlockFrames, 1u);
  stride_ = (cfg_.maxBlockFrames + 15u) & ~15u;
  zeros_ = AlignedBuffer<float>(stride_);
  projectParams_.assign(cfg_.numProjectParams, 0.0f);
  reader_ = *reclaimer_.registerReader();
  silence_ = static_cast<float>(std::pow(10.0, static_cast<double>(cfg_.silenceDb) / 20.0));
  for (size_t b = 0; b < layout_.busWidth.size(); ++b)
    stores_.push_back(std::make_shared<BusStore>(layout_.busWidth[b], stride_,
                                                 layout_.busKey.empty() ? std::string() : layout_.busKey[b],
                                                 meteredBus(layout_, b)));
  auto* t = new Topology;
  t->seq = 0;
  for (uint32_t i = 0; i < layout_.units.size(); ++i) {
    units_.push_back(makeUnit(layout_.units[i]));
    std::vector<float*> own;
    for (uint32_t b = 0; b < layout_.busWidth.size(); ++b)
      if (layout_.busProducer[b] == i)
        for (uint16_t c = 0; c < layout_.busWidth[b]; ++c) own.push_back(stores_[b]->channel(c));
    ownChannels_.push_back(own);
    Slot s;
    s.unit = units_.back().get();
    s.own = std::move(own);
    const UnitLayout& ul = layout_.units[i];
    if (ul.voices && ul.eventSlot != kNoSlot) s.events = cfg_.eventSources[ul.eventSlot];
    t->order.push_back(std::move(s));
  }
  t->alive = std::make_unique<std::atomic<uint8_t>[]>(t->order.size());
  for (size_t i = 0; i < t->order.size(); ++i) t->alive[i].store(1, std::memory_order_relaxed);
  t->stores = stores_;
  t->output = layout_.outputBus != kNoBus ? stores_[layout_.outputBus].get() : nullptr;
  topoInflight_.push_back(t);
  topo_ = t;
}

ProjectProgram::~ProjectProgram() {
  for (Topology* t : topoInflight_) delete t;
  topoInflight_.clear();
  units_.clear();    // each Unit frees its in-flight packets
  zombies_.clear();
  // reclaimer_'s destructor frees the retired packets, units and topologies.
}

Result<std::unique_ptr<ProjectProgram::Packet>> ProjectProgram::makePacket(
    const Unit& U, uint32_t unitIndex, const ProjectLayout& layout, const std::vector<std::shared_ptr<BusStore>>& stores,
    const std::vector<float*>& ownChannels, UnitImage ui, const SwapPolicy& policy, PublishInfo& info) {
  const std::string who = "unit '" + U.cfg.name + "': ";
  if (ui.image == nullptr) return projectError(ErrorCode::InvalidArgument, who + "no image");
  ProgramImage& img = *ui.image;
  const ImageInterface& io = img.io();
  if (io.sampleRate != cfg_.sampleRate || io.maxBlockFrames != cfg_.maxBlockFrames)
    return projectError(ErrorCode::InvalidArgument, who + "the image's sample rate / maxBlockFrames differ from the project's");
  if (ui.in.size() != io.globalIn.size() || ui.out.size() != io.globalOut.size())
    return projectError(ErrorCode::InvalidArgument, who + "bindings do not match the image's slots");
  const auto numBuses = static_cast<uint32_t>(layout.busWidth.size());
  auto badRef = [&](const BusRef& r) { return r.bus >= numBuses || r.channel >= layout.busWidth[r.bus]; };

  auto p = std::make_unique<Packet>();
  p->seq = U.nextSeq;
  p->mode = policy.mode;
  const float fadeMs = std::clamp(std::isfinite(policy.fadeMs) ? policy.fadeMs : 10.0f, kMinFadeMs, kMaxFadeMs);
  p->fadeFrames = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(fadeMs * 0.001 * cfg_.sampleRate)));
  const float* zeros = zeros_.data();

  for (const BusRef& r : ui.in) {
    if (r.bus == kNoBus) {
      p->inPtrs.push_back(zeros);
      continue;
    }
    if (badRef(r)) return projectError(ErrorCode::InvalidArgument, who + "an input reads a bus channel that does not exist");
    if (layout.busProducer[r.bus] >= unitIndex)
      return projectError(ErrorCode::InvalidArgument, who + "an input reads bus " + std::to_string(r.bus) +
                                                          ", which is not produced by an earlier unit");
    p->inPtrs.push_back(stores[r.bus]->channel(r.channel));
  }
  std::vector<uint8_t> written(ownChannels.size(), 0);
  for (const BusRef& r : ui.out) {
    if (r.bus == kNoBus) {
      p->outPtrs.push_back(nullptr);
      continue;
    }
    if (badRef(r) || layout.busProducer[r.bus] != unitIndex)
      return projectError(ErrorCode::InvalidArgument, who + "an output writes a bus channel the unit does not produce");
    float* ch = stores[r.bus]->channel(r.channel);
    const auto k = static_cast<size_t>(std::find(ownChannels.begin(), ownChannels.end(), ch) - ownChannels.begin());
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
  p->keepAlive = std::move(ui.keepAlive);

  // Crossfade scratch: this packet's own outputs while it fades out later.
  const auto slots = static_cast<uint32_t>(p->outPtrs.size());
  p->scratch = AlignedBuffer<float>(static_cast<size_t>(std::max(slots, 1u)) * stride_);
  p->fadePtrs.assign(slots, nullptr);
  for (uint32_t s = 0; s < slots; ++s)
    if (p->outPtrs[s] != nullptr) p->fadePtrs[s] = p->scratch.data() + static_cast<size_t>(s) * stride_;

  for (const graph::ParamSlot& ps : io.params) p->params.push_back(ps.defaultValue);
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
  p->image = std::move(ui.image);
  return p;
}

Result<PublishInfo> ProjectProgram::publish(uint32_t unitIndex, UnitImage ui, const SwapPolicy& policy) {
  std::lock_guard lock(mutex_);
  collectLocked();
  if (unitIndex >= units_.size()) return projectError(ErrorCode::InvalidArgument, "no unit " + std::to_string(unitIndex));
  if (ui.image == nullptr) return projectError(ErrorCode::InvalidArgument, "no image");
  if (!topoInflight_.empty() && topoInflight_.back()->seq > topoCurrent_.load(std::memory_order_acquire))
    return projectError(ErrorCode::Overflow, "a topology change is pending; retry after the audio thread has switched");
  Unit& U = *units_[unitIndex];
  const VoiceSlots vs = ui.voiceSlots;
  PublishInfo info;
  auto made = makePacket(U, unitIndex, layout_, stores_, ownChannels_[unitIndex], std::move(ui), policy, info);
  if (!made.ok()) return made.error();
  std::unique_ptr<Packet> p = std::move(made.value());
  if (U.cfg.voices) {
    if (!U.lanesPrepared) {
      if (Status st = U.voices->prepareLanes(p->image->io(), U.cfg.voice); !st.ok()) return st.error();
      U.lanesPrepared = true;
    }
    auto b = U.voices->makeBinding(p->image.get(), vs.doneSlot, vs.monitorMask);
    if (!b.ok()) return b.error();
    p->binding = std::move(b.value());
  }
  Packet* raw = p.get();
  U.inflight.push_back(raw);
  if (!U.ring.tryPush(raw)) {
    U.inflight.pop_back();
    return projectError(ErrorCode::Overflow, "unit '" + U.cfg.name + "': too many images pending; retry after the audio thread has taken some");
  }
  p.release();  // owned through inflight (then the reclaimer)
  U.last = raw;
  ++U.nextSeq;
  return info;
}

Result<TopologyInfo> ProjectProgram::publishTopology(ProjectLayout layout, std::vector<TopologyUnit> units) {
  if (Status st = validateLayout(cfg_, layout); !st.ok()) return st.error();
  std::lock_guard lock(mutex_);
  collectLocked();
  if (!topoInflight_.empty() && topoInflight_.back()->seq > topoCurrent_.load(std::memory_order_acquire))
    return projectError(ErrorCode::Overflow, "a topology change is already pending");
  const auto n = static_cast<uint32_t>(layout.units.size());
  if (units.size() != n) return projectError(ErrorCode::InvalidArgument, "topology: one TopologyUnit per layout unit");
  std::vector<uint8_t> taken(units_.size(), 0);
  for (uint32_t i = 0; i < n; ++i) {
    const TopologyUnit& tu = units[i];
    if (tu.from == kNoUnit) {
      if (!tu.hasImage) return projectError(ErrorCode::InvalidArgument, "topology: new unit '" + layout.units[i].name + "' has no image");
      continue;
    }
    if (tu.from >= units_.size() || taken[tu.from])
      return projectError(ErrorCode::InvalidArgument, "topology: unit '" + layout.units[i].name + "' continues an invalid or reused unit");
    taken[tu.from] = 1;
    if (units_[tu.from]->cfg.voices != layout.units[i].voices)
      return projectError(ErrorCode::InvalidArgument, "topology: unit '" + layout.units[i].name + "' changes between voices and none");
  }
  TopologyInfo info;
  // Buses: keep the storage of a bus with the same key and width.
  std::vector<std::shared_ptr<BusStore>> stores;
  std::vector<uint8_t> oldKept(stores_.size(), 0);
  for (size_t b = 0; b < layout.busWidth.size(); ++b) {
    const std::string key = layout.busKey.empty() ? std::string() : layout.busKey[b];
    std::shared_ptr<BusStore> s;
    if (!key.empty())
      for (size_t o = 0; o < stores_.size(); ++o)
        if (!oldKept[o] && stores_[o]->key == key && stores_[o]->width == layout.busWidth[b]) {
          s = stores_[o];
          oldKept[o] = 1;
          break;
        }
    if (s) {
      s->metered = meteredBus(layout, b);  // read by the audio thread only after the switch (ring release/acquire)
      ++info.reusedBuses;
    } else {
      s = std::make_shared<BusStore>(layout.busWidth[b], stride_, key, meteredBus(layout, b));
      ++info.newBuses;
    }
    stores.push_back(std::move(s));
  }
  std::vector<std::vector<float*>> own(n);
  for (uint32_t b = 0; b < layout.busWidth.size(); ++b)
    for (uint16_t c = 0; c < layout.busWidth[b]; ++c) own[layout.busProducer[b]].push_back(stores[b]->channel(c));

  // Units: continued or new; packets for the new images (not yet committed to the units).
  std::vector<Unit*> unitPtr(n, nullptr);
  std::vector<std::unique_ptr<Unit>> fresh(n);
  for (uint32_t i = 0; i < n; ++i) {
    if (units[i].from == kNoUnit) {
      fresh[i] = makeUnit(layout.units[i]);
      unitPtr[i] = fresh[i].get();
    } else {
      unitPtr[i] = units_[units[i].from].get();
    }
  }
  std::vector<std::unique_ptr<Packet>> packets(n);
  for (uint32_t i = 0; i < n; ++i) {
    if (!units[i].hasImage) continue;
    Unit& U = *unitPtr[i];
    const VoiceSlots vs = units[i].image.voiceSlots;
    PublishInfo pi;
    auto made = makePacket(U, i, layout, stores, own[i], std::move(units[i].image), units[i].policy, pi);
    if (!made.ok()) return made.error();
    packets[i] = std::move(made.value());
    if (U.cfg.voices) {
      if (!U.lanesPrepared) {
        if (Status st = U.voices->prepareLanes(packets[i]->image->io(), U.cfg.voice); !st.ok()) return st.error();
        U.lanesPrepared = true;
      }
      auto b = U.voices->makeBinding(packets[i]->image.get(), vs.doneSlot, vs.monitorMask);
      if (!b.ok()) return b.error();
      packets[i]->binding = std::move(b.value());
    }
  }

  // Run order: the new units plus the removed ones (zombies), ordered so that every unit runs after whatever writes
  // a bus it reads — both through its new image and through the image it still runs until its swap is over.
  std::vector<uint32_t> zombieOld;
  for (uint32_t o = 0; o < units_.size(); ++o)
    if (!taken[o]) zombieOld.push_back(o);
  const auto z = static_cast<uint32_t>(zombieOld.size());
  const uint32_t total = n + z;
  auto storeOf = [&](const float* ch) -> BusStore* {
    for (const auto& s : stores)
      if (ch >= s->data.data() && ch < s->data.data() + s->data.size()) return s.get();
    for (const auto& s : stores_)
      if (ch >= s->data.data() && ch < s->data.data() + s->data.size()) return s.get();
    return nullptr;
  };
  std::vector<std::unordered_set<const BusStore*>> reads(total), writes(total);
  auto addPacket = [&](uint32_t node, const Packet* p) {
    if (p == nullptr) return;
    for (const float* ch : p->inPtrs)
      if (const BusStore* s = storeOf(ch)) reads[node].insert(s);
    for (const float* ch : p->outPtrs)
      if (ch != nullptr)
        if (const BusStore* s = storeOf(ch)) writes[node].insert(s);
  };
  for (uint32_t i = 0; i < n; ++i) {
    addPacket(i, packets[i].get());
    addPacket(i, unitPtr[i]->last);  // the image that runs until the swap (or keeps running)
    for (float* ch : own[i])
      if (const BusStore* s = storeOf(ch)) writes[i].insert(s);
  }
  for (uint32_t k = 0; k < z; ++k) {
    addPacket(n + k, units_[zombieOld[k]]->last);
    for (float* ch : ownChannels_[zombieOld[k]])
      if (const BusStore* s = storeOf(ch)) writes[n + k].insert(s);
  }
  std::vector<std::vector<uint32_t>> succ(total);
  std::vector<uint32_t> indeg(total, 0);
  for (uint32_t w = 0; w < total; ++w)
    for (uint32_t r = 0; r < total; ++r) {
      if (w == r) continue;
      bool edge = false;
      for (const BusStore* s : writes[w])
        if (reads[r].count(s)) {
          edge = true;
          break;
        }
      if (edge) {
        succ[w].push_back(r);
        ++indeg[r];
      }
    }
  // Kahn: among ready nodes, zombies first (they feed old images), then new units in layout order.
  std::vector<uint32_t> order;
  std::vector<uint8_t> done(total, 0);
  while (order.size() < total) {
    uint32_t pick = kNoUnit;
    for (uint32_t k = 0; k < z && pick == kNoUnit; ++k)
      if (!done[n + k] && indeg[n + k] == 0) pick = n + k;
    for (uint32_t i = 0; i < n && pick == kNoUnit; ++i)
      if (!done[i] && indeg[i] == 0) pick = i;
    if (pick == kNoUnit) break;  // a cycle
    done[pick] = 1;
    order.push_back(pick);
    for (uint32_t s : succ[pick]) --indeg[s];
  }
  auto* t = new Topology;
  std::unique_ptr<Topology> guard(t);
  if (order.size() < total) {  // no feed-forward order with the zombies: drop them
    order.clear();
    for (uint32_t i = 0; i < n; ++i) order.push_back(i);
    info.zombiesDropped = true;
    for (uint32_t k = 0; k < z; ++k)
      for (float* ch : ownChannels_[zombieOld[k]])
        if (BusStore* s = storeOf(ch)) t->zeroAtSwitch.push_back(s);
  }
  for (uint32_t node : order) {
    Slot s;
    if (node < n) {
      s.unit = unitPtr[node];
      s.own = own[node];
      const UnitLayout& ul = layout.units[node];
      if (ul.voices && ul.eventSlot != kNoSlot) s.events = cfg_.eventSources[ul.eventSlot];
      if (packets[node]) t->swaps.emplace_back(static_cast<uint32_t>(t->order.size()), packets[node].get());
    } else {
      const uint32_t o = zombieOld[node - n];
      Unit& Z = *units_[o];
      s.unit = &Z;
      s.own = ownChannels_[o];
      s.zombie = true;
      const UnitLayout& ul = Z.cfg;
      if (ul.voices && ul.eventSlot != kNoSlot) s.events = cfg_.eventSources[ul.eventSlot];
      // Readers of the zombie's buses (through the image they run now) wait for their new image and its fade.
      for (uint32_t r = 0; r < n; ++r) {
        bool reads0 = false;
        if (const Packet* lp = unitPtr[r]->last)
          for (const float* ch : lp->inPtrs)
            for (float* zc : ownChannels_[o]) reads0 |= ch == zc;
        if (reads0 && packets[r]) s.waitFor.emplace_back(unitPtr[r], unitPtr[r]->nextSeq);
      }
      ++info.zombies;
    }
    t->order.push_back(std::move(s));
  }
  t->alive = std::make_unique<std::atomic<uint8_t>[]>(t->order.size());
  for (size_t i = 0; i < t->order.size(); ++i) t->alive[i].store(1, std::memory_order_relaxed);
  t->stores = stores;
  if (!info.zombiesDropped)
    for (uint32_t k = 0; k < z; ++k)
      for (float* ch : ownChannels_[zombieOld[k]])
        if (const BusStore* s = storeOf(ch)) {
          bool listed = false;
          for (const auto& x : stores) listed |= x.get() == s;
          for (const auto& x : t->extra) listed |= x.get() == s;
          if (!listed)
            for (const auto& x : stores_)
              if (x.get() == s) t->extra.push_back(x);
        }
  t->output = layout.outputBus != kNoBus ? stores[layout.outputBus].get() : nullptr;
  t->seq = nextTopoSeq_;

  // Commit (nothing below fails): packets join their units' in-flight lists, units and buses become the published
  // layout, the topology goes to the audio thread.
  for (uint32_t i = 0; i < n; ++i) {
    if (!packets[i]) continue;
    Unit& U = *unitPtr[i];
    Packet* raw = packets[i].release();
    U.inflight.push_back(raw);
    U.last = raw;
    ++U.nextSeq;
    ++info.newImages;
  }
  std::vector<std::unique_ptr<Unit>> next(n);
  for (uint32_t i = 0; i < n; ++i) {
    if (units[i].from == kNoUnit) {
      next[i] = std::move(fresh[i]);
      ++info.newUnits;
    } else {
      next[i] = std::move(units_[units[i].from]);
      next[i]->cfg.name = layout.units[i].name;
      next[i]->cfg.eventSlot = layout.units[i].eventSlot;
      ++info.continued;
    }
  }
  for (uint32_t o : zombieOld) zombies_.emplace_back(t->seq, std::move(units_[o]));
  units_ = std::move(next);
  ownChannels_ = std::move(own);
  stores_ = std::move(stores);
  layout_ = std::move(layout);
  topoInflight_.push_back(guard.release());
  if (!topoRing_.tryPush(t)) {  // cannot happen: at most one topology is pending
    topoInflight_.pop_back();
    delete t;
    return projectError(ErrorCode::Overflow, "topology ring full");
  }
  info.sequence = nextTopoSeq_++;
  return info;
}

void ProjectProgram::readMeters(std::span<const uint32_t> buses, std::span<BusLevel> out) {
  std::lock_guard lock(mutex_);
  for (size_t i = 0; i < buses.size() && i < out.size(); ++i) {
    BusLevel l;
    const uint32_t b = buses[i];
    if (b < stores_.size() && stores_[b]->metered) {
      BusStore& s = *stores_[b];
      l.channels = static_cast<uint8_t>(std::min<uint16_t>(s.width, 2));
      for (uint32_t c = 0; c < l.channels; ++c) {
        l.peak[c] = s.pubPeak[c].load(std::memory_order_relaxed);
        l.meanSquare[c] = s.pubMs[c].load(std::memory_order_relaxed);
      }
    }
    out[i] = l;
  }
  meterGen_.fetch_add(1, std::memory_order_release);
}

bool ProjectProgram::quiet() const noexcept {  // RT-safe
  if (topoPending_ != nullptr || !topoRing_.emptyApprox()) return false;
  for (const Slot& s : topo_->order) {
    const Unit& U = *s.unit;
    if (U.fadeFrom != nullptr || !U.ring.emptyApprox()) return false;
    if (U.voices && U.voices->liveVoices() > 0) return false;
  }
  return true;
}

bool ProjectProgram::canPublish(uint32_t unit) const {
  std::lock_guard lock(mutex_);
  if (!topoInflight_.empty() && topoInflight_.back()->seq > topoCurrent_.load(std::memory_order_acquire)) return false;
  if (unit >= units_.size()) return true;  // publish() reports the bad index
  const Unit& U = *units_[unit];
  return U.ring.sizeApprox() + 1 < std::bit_ceil(std::max(2u, cfg_.maxPendingPerUnit));
}

bool ProjectProgram::topologyPending() const {
  std::lock_guard lock(mutex_);
  return !topoInflight_.empty() && topoInflight_.back()->seq > topoCurrent_.load(std::memory_order_acquire);
}

void ProjectProgram::collectLocked() {
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
  const uint64_t current = topoCurrent_.load(std::memory_order_acquire);
  // Topologies older than the current one are no longer read.
  size_t k = 0;
  while (k < topoInflight_.size() && topoInflight_[k]->seq < current) {
    reclaimer_.retire(topoInflight_[k], [](void* q) noexcept { delete static_cast<Topology*>(q); });
    ++k;
  }
  topoInflight_.erase(topoInflight_.begin(), topoInflight_.begin() + static_cast<std::ptrdiff_t>(k));
  // Zombies: gone once a later topology is current, or dead in the current one.
  const Topology* cur = nullptr;
  for (const Topology* t : topoInflight_)
    if (t->seq == current) cur = t;
  for (size_t i = 0; i < zombies_.size();) {
    auto& [seq, unit] = zombies_[i];
    bool gone = current > seq;
    if (!gone && current == seq && cur != nullptr)
      for (size_t s = 0; s < cur->order.size(); ++s)
        if (cur->order[s].unit == unit.get() && cur->alive[s].load(std::memory_order_acquire) == 0) gone = true;
    if (gone) {
      reclaimer_.retire(unit.release(), [](void* q) noexcept { delete static_cast<Unit*>(q); });
      zombies_.erase(zombies_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
  const size_t freed = reclaimer_.collect();
  freed_.fetch_add(freed, std::memory_order_relaxed);
}

size_t ProjectProgram::collect() {
  std::lock_guard lock(mutex_);
  const uint64_t before = freed_.load(std::memory_order_relaxed);
  collectLocked();
  return static_cast<size_t>(freed_.load(std::memory_order_relaxed) - before);
}

size_t ProjectProgram::imagesAlive() const {
  std::lock_guard lock(mutex_);
  size_t n = reclaimer_.pending();
  for (const auto& u : units_) n += u->inflight.size();
  for (const auto& z : zombies_) n += z.second->inflight.size();
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

const float* ProjectProgram::bus(uint32_t b, uint16_t channel) const noexcept {  // RT-safe
  return topo_->stores[b]->channel(channel);
}

uint32_t ProjectProgram::numBuses() const noexcept { return static_cast<uint32_t>(topo_->stores.size()); }  // RT-safe

float ProjectProgram::unitPeak(const Slot& s) const noexcept {  // RT-safe
  float peak = 0.0f;
  for (const float* ch : s.own) peak = std::max(peak, peakAbs(ch, lastFrames_));
  return peak;
}

void ProjectProgram::install(Unit& U, const Slot& s, Packet* p) noexcept {  // RT-safe
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
    fade = transport_.playing || (U.voices && U.voices->activeGroups() > 0) || unitPeak(s) > silence_;
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

void ProjectProgram::trySwap(Unit& U, const Slot& s) noexcept {  // RT-safe
  Packet* p = nullptr;
  if (U.ring.tryPop(p)) install(U, s, p);
}

void ProjectProgram::maybeSwitchTopology() noexcept {  // RT-safe
  if (topoPending_ == nullptr && !topoRing_.tryPop(topoPending_)) return;
  Topology& t = *topoPending_;
  for (const auto& [idx, p] : t.swaps) {
    const Unit& U = *t.order[idx].unit;
    if (U.fadeFrom != nullptr || !U.ring.emptyApprox()) {  // its earlier images first
      ++stats_.topologyWaitBlocks;
      return;
    }
  }
  for (BusStore* b : t.zeroAtSwitch) std::memset(b->data.data(), 0, sizeof(float) * b->data.size());
  topo_ = topoPending_;
  topoPending_ = nullptr;
  for (const auto& [idx, p] : t.swaps) install(*t.order[idx].unit, t.order[idx], p);
  ++stats_.topologySwitches;
  topoCurrent_.store(t.seq, std::memory_order_release);
}

bool ProjectProgram::runUnit(Unit& U, const Slot& s, uint32_t frames, int64_t time) noexcept {  // RT-safe
  if (U.fadeFrom == nullptr) trySwap(U, s);
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
    if (s.events.fn != nullptr) s.events.fn(s.events.user, time, frames, U.eventBuf);
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
    maybeSwitchTopology();
    Topology& t = *topo_;
    uint32_t fading = 0;
    for (size_t i = 0; i < t.order.size(); ++i) {
      if (t.alive[i].load(std::memory_order_relaxed) == 0) continue;
      const Slot& s = t.order[i];
      if (s.zombie) {
        bool ready = true;
        for (const auto& [r, seq] : s.waitFor) ready &= r->stats.sequence >= seq && r->fadeFrom == nullptr;
        if (ready) {  // its readers no longer run an image that reads it
          for (float* ch : s.own) std::memset(ch, 0, sizeof(float) * stride_);
          t.alive[i].store(0, std::memory_order_release);
          continue;
        }
        ++stats_.zombieRuns;
      }
      fading += runUnit(*s.unit, s, n, time) ? 1u : 0u;
    }
    const uint32_t gen = meterGen_.load(std::memory_order_acquire);
    for (const auto& sp : t.stores) {
      BusStore& b = *sp;
      if (!b.metered) continue;
      if (b.seenGen != gen) {
        b.seenGen = gen;
        b.accPeak[0] = b.accPeak[1] = 0.0f;
        b.accSq[0] = b.accSq[1] = 0.0;
        b.accN = 0;
      }
      const uint16_t ch = std::min<uint16_t>(b.width, 2);
      for (uint16_t c = 0; c < ch; ++c) levelOf(b.channel(c), n, b.accPeak[c], b.accSq[c]);
      b.accN += n;
      const double inv = 1.0 / static_cast<double>(b.accN);
      for (uint16_t c = 0; c < ch; ++c) {
        b.pubPeak[c].store(b.accPeak[c], std::memory_order_relaxed);
        b.pubMs[c].store(static_cast<float>(b.accSq[c] * inv), std::memory_order_relaxed);
      }
    }
    for (uint32_t c = 0; c < numOut; ++c) {
      float* dst = block.out[c] + off;
      BusStore* ob = t.output;
      if (ob == nullptr || (ob->width > 1 && c >= ob->width)) {
        std::memset(dst, 0, sizeof(float) * n);
        continue;
      }
      std::memcpy(dst, ob->channel(ob->width == 1 ? uint16_t{0} : static_cast<uint16_t>(c)), sizeof(float) * n);
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
