// Per-track project runner (T1.x-arch1; topology changes T3.17): pq/engine/project_program.h describes the protocol.
#include "pq/engine/project_program.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "crossfade.h"
#include "pq/core/tap_ring.h"
#include "pq/core/rt_heap.h"
#include "pq/engine/latency_plan.h"

namespace pq::engine {

using detail::crossfade;
using detail::peakAbs;

// PF.4b: built on a compile thread (JIT delivery): on the RT heap, with its tables (pq/core/rt_heap.h).
struct ProjectProgram::Packet : RtHeapObject {
  template <class T>
  using Table = RtTable<T>;
  uint64_t seq = 0;
  Table<std::shared_ptr<const void>> keepAlive{rtHeap()};  // UnitImage::keepAlive (destroyed after the image)
  std::unique_ptr<ProgramImage> image;
  TransferPlan plan;                                      // from the unit's previous packet (empty for the first)
  SwapMode mode = SwapMode::Auto;
  uint32_t fadeFrames = 1;
  Table<float> params{rtHeap()};                          // indexed by io().params
  Table<std::pair<uint32_t, uint32_t>> paramMap{rtHeap()};  // (new slot, previous packet's slot), equal keys
  Table<std::pair<uint32_t, uint32_t>> paramLinks{rtHeap()};  // (slot, project param)
  Table<const float*> inPtrs{rtHeap()};                   // per global in slot: a bus channel or zeros
  Table<uint16_t> inChannel{rtHeap()};                    // T6.2: per global in slot: its bus channel
  Table<const float*> inLive{rtHeap()};                   // T6.2: per global in slot as a compensated block reads it
  Table<DelayLine*> inLine{rtHeap()};                     // T6.2: per global in slot: its delay line (null: none) …
  Table<DelayLine*> storeLine{rtHeap()};                  // … per inStores entry …
  Table<uint32_t> compSlots{rtHeap()};                    // … the global in slots that have one (numCompSlots of them) …
  uint32_t numCompSlots = 0;
  uint64_t lineSeq = 0;                                   // … under the plan with this sequence (0: not mapped yet)
  Table<float*> outPtrs{rtHeap()};                        // per global out slot: a bus channel or null
  Table<float*> fadePtrs{rtHeap()};                       // the same into `scratch` (while this packet fades out)
  AlignedBuffer<float> scratch;
  struct FadeLane {
    float* bus;                                           // written by the new image, faded in place
    const float* old;                                     // the previous packet's scratch for the same channel
  };
  Table<FadeLane> fade{rtHeap()};                         // when this packet replaces the previous one
  std::unique_ptr<VoiceBinding> binding;                  // voice units
  // Idle units (see the header).
  uint32_t idleAfter = 0;                                 // UnitImage::idleAfterBlocks
  Table<float> linked{rtHeap()};                          // the linked params' values in the last block
  uint64_t linkedGen = ~uint64_t{0};                      // the param generation `linked` was read at (L1)
  uint64_t paramsGen = ~uint64_t{0};                      // the generation `params` was last written at (T5.x-perf-audio)
  uint32_t paramsNeeded = 0;                              // 1 + the highest project param it links (M4)
  Table<const BusStore*> inStores{rtHeap()};              // the distinct buses it reads
  Table<uint32_t> clipSlots{rtHeap()};                    // T5.4: the clip slots it reads (io().clipSlots)
  // Silence (T1.x-silence): ProcessContext::inSilent / outSilent and the buses behind them.
  Table<const BusStore*> inStoreOf{rtHeap()};             // per global in slot (null: reads zeros)
  Table<uint8_t> inSilent{rtHeap()}, outSilent{rtHeap()};  // per global in / out slot
  Table<BusStore*> outStores{rtHeap()};                   // the distinct buses it produces
  Table<uint32_t> outStoreOf{rtHeap()};                   // per global out slot: index into outStores, or kNone
  Table<uint8_t> outStoreSilent{rtHeap()};                // scratch, per outStores entry
};

// T6.2: delay rings are a whole number of these frames.
constexpr uint32_t kRingGrain = 64;

// T6.2b: one channel's block into a delay ring, inline (a block is a few hundred bytes: the CRT's call and size
// dispatch cost as much as the copy).
static inline void copyBlock(float* d, const float* s, uint32_t n) noexcept {  // RT-safe
  uint32_t i = 0;
  for (; i + 16 <= n; i += 16) {
    const __m256 a = _mm256_loadu_ps(s + i), b = _mm256_loadu_ps(s + i + 8);
    _mm256_storeu_ps(d + i, a);
    _mm256_storeu_ps(d + i + 8, b);
  }
  for (; i < n; ++i) d[i] = s[i];
}

// T6.2: one compensated input of a reader unit (see the header's "Plugin-delay compensation"); T6.2b: or one of its
// clip slots (bus null, `clip` the slot: ProjectConfig::clipIn / clipActive).
// The ring of each channel is `size` frames plus a MIRROR of its first `mirror` (= stride ≥ any block) frames after
// them, kept equal by every write: a tap of up to `mirror` frames is always contiguous, so the reader's image reads it
// in place (outBase) and a steady line costs one copy per channel per block (the push). Only during a delay change's
// fade does the line write a block (`fade`: the crossfade of the two taps).
// T6.2b: what every block touches comes first, on the line's own cache lines (no pointer chase through the bus).
struct alignas(128) ProjectProgram::DelayLine {
  DelayLine(std::shared_ptr<BusStore> b, uint32_t clipSlot, uint16_t w, uint32_t frames, uint32_t stride);
  // Hot (audio thread, every block).
  AlignedBuffer<float> ring;      // width × (size + mirror), zeroed
  const float* src;               // the bus's channel 0 (null: a clip slot's line) …
  const bool* srcSilent;          // … its silence flag …
  uint32_t srcStride;             // … and its channel stride
  uint32_t size;                  // ring frames (≥ the largest tap + one block; a multiple of kRingGrain: as small as the
                                  // delay allows, so the history the readers touch stays in cache)
  uint32_t mirror;                // frames mirrored after the ring (the program's stride)
  uint32_t write = 0;             // ring frame of the next block's frame 0 (< size)
  uint32_t delay = 0, from = 0;   // the tap, and the tap it fades from
  uint32_t fadePos = 0, fadeLen = 0;
  uint32_t clip;                  // T6.2b: the clip slot (kNoSlot: a bus line)
  uint64_t silentRun = uint64_t{1} << 62;  // trailing frames of the ring known to be zero
  // Where the reader reads channel c of the delayed block (valid after runLine, while not outSilent).
  const float* outBase = nullptr;
  size_t outStride = 0;
  uint16_t width;
  bool active = false;            // false: bypassed (delay 0, no fade): the reader reads the bus; the ring is stale
  bool outSilent = true;          // the last block read only zeros (the reader then reads the program's zeros)
  // M6 review M2: while its history is copied from a smaller line (`copySrc`, over several blocks), that line plays
  // and this one only takes the pushes. The history's oldest-first frame `copyDone` of copySrc's ring from `copyFrom`.
  DelayLine* copySrc = nullptr;
  uint32_t copyFrom = 0, copyDone = 0;
  // Cold.
  std::shared_ptr<BusStore> bus;  // the input bus (kept alive while a plan names the line); null: a clip slot's line
  AlignedBuffer<float> fade;      // width × stride: the crossfaded block while a delay change fades
  DelayLine* prev = nullptr;      // a smaller line this one replaces (immutable): its history is copied (once)
  // Written by the audio thread, read by setCompensation (non-RT): the history is in (prev is no longer read), and
  // the line is active (M6 review M3: a dropped line is carried forward while it still fades).
  std::atomic<bool> copied{false};
  std::atomic<bool> live{false};
  void setActive(bool v) noexcept {  // RT-safe
    active = v;
    live.store(v, std::memory_order_release);
  }
  bool sameSource(const DelayLine& o) const noexcept { return bus == o.bus && clip == o.clip; }
  float* chan(uint16_t c) noexcept { return ring.data() + static_cast<size_t>(c) * (size + mirror); }
};

// T6.2: a voice unit's notes, `delay` frames late (preallocated FIFO; time order kept).
struct ProjectProgram::EventDelay {
  explicit EventDelay(uint32_t capacity) : items(capacity), droppedIds(capacity) {}
  struct Item {
    int64_t at;
    NoteEvent e;
  };
  std::vector<Item> items;  // ring
  uint32_t head = 0, count = 0;
  int64_t lastAt = std::numeric_limits<int64_t>::min();
  uint64_t dropped = 0;
  // M6 review L10: the notes whose note-on was dropped (their offs are dropped too, and take no room): a ring.
  std::vector<uint32_t> droppedIds;
  uint32_t numDroppedIds = 0, nextDroppedId = 0;
  void dropOn(uint32_t id) noexcept {  // RT-safe
    const auto cap = static_cast<uint32_t>(droppedIds.size());
    if (numDroppedIds < cap) {
      droppedIds[numDroppedIds++] = id;
    } else {
      droppedIds[nextDroppedId] = id;
      nextDroppedId = (nextDroppedId + 1) % cap;
    }
  }
  bool forgetDropped(uint32_t id) noexcept {  // RT-safe: true when the note's on was dropped
    for (uint32_t k = 0; k < numDroppedIds; ++k)
      if (droppedIds[k] == id) {
        droppedIds[k] = droppedIds[--numDroppedIds];
        nextDroppedId = 0;
        return true;
      }
    return false;
  }
};

struct ProjectProgram::Compensation {
  uint64_t seq = 0;
  uint64_t topoSeq = 0;  // the topology it was made for (the audio thread takes it once it runs that one)
  struct Line {
    DelayLine* line;
    uint32_t delay;      // its target tap (0: fade out, then bypass)
  };
  struct Entry {
    Unit* unit = nullptr;
    std::vector<Line> lines;     // bus lines and (T6.2b) clip slot lines
    EventDelay* events = nullptr;
    uint32_t eventDelay = 0;
    // T6.2b: with clip lines, the clip table its images read (ProcessContext::clipIn; numClipSlots · 2, refreshed for
    // the unit's slots each block on the audio thread). Empty: no clip line, the images read the program's.
    mutable std::vector<const float*> clipIn;
  };
  std::vector<Entry> entries;
  std::vector<std::shared_ptr<DelayLine>> owned;        // its lines (and the ones they copy their history from)
  std::vector<std::shared_ptr<EventDelay>> ownedEvents;
};

struct ProjectProgram::Unit {  // (128-aligned: its ring)
  explicit Unit(uint32_t pending) : ring(pending) {}
  ~Unit() {
    for (Packet* p : inflight) delete p;
  }
  // Audio thread, read by every block's runUnit (PF.4b: together, on the unit's first cache line).
  Packet* cur = nullptr;
  Packet* fadeFrom = nullptr;
  std::unique_ptr<VoiceEngine> voices;
  // T6.2: its entry in the plan the audio thread runs (valid while compSeq is that plan's sequence).
  const Compensation::Entry* compEntry = nullptr;
  uint64_t compSeq = 0;
  uint32_t fadePos = 0, fadeLen = 0;
  uint32_t quietBlocks = 0;  // consecutive blocks with silent inputs, no voice, no event and unchanged params
  // T6.7: its track input (setUnitInputs): first | channels << 16 (0: none).
  std::atomic<uint32_t> input{0};
  bool idle = false;         // skipped last block (its bus channels are zero)
  // T1.x-silence: what its image reported after the last block it ran (ProcessContext::dormant / tailsDone).
  bool dormant = false;
  bool tailsDone = true;
  // Audio thread, colder.
  EventBuffer eventBuf{1};
  UnitStats stats;
  UnitLayout cfg;
  // Non-RT (ProjectProgram::mutex_).
  std::vector<Packet*> inflight;     // published, not yet retired, in sequence order (owned)
  Packet* last = nullptr;
  uint64_t nextSeq = 1;
  bool lanesPrepared = false;
  // Hand-over.
  SpscRing<Packet*> ring;
  std::atomic<uint64_t> retiredThrough{0};
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
  bool metered;  // non-RT (readMeters / publishTopology, mutex_); the audio thread reads Topology::metered
  // Meter (audio thread accumulates and publishes; readMeters() reads and asks for a restart through meterGen_).
  uint32_t seenGen = 0;
  float accPeak[2] = {0.0f, 0.0f};
  double accSq[2] = {0.0, 0.0};
  uint64_t accN = 0;
  std::atomic<float> pubPeak[2] = {0.0f, 0.0f};
  std::atomic<float> pubMs[2] = {0.0f, 0.0f};
  float* channel(uint16_t c) noexcept { return data.data() + static_cast<size_t>(c) * stride; }
  bool silent = false;  // audio thread: its producer is idle and zeroed it (idle units)
};

ProjectProgram::DelayLine::DelayLine(std::shared_ptr<BusStore> b, uint32_t clipSlot, uint16_t w, uint32_t frames,
                                     uint32_t stride)
    : ring(static_cast<size_t>(w) * (frames + stride)),
      src(b != nullptr ? b->data.data() : nullptr),
      srcSilent(b != nullptr ? &b->silent : nullptr),
      srcStride(b != nullptr ? b->stride : 0u),
      size(frames),
      mirror(stride),
      clip(clipSlot),
      width(w),
      bus(std::move(b)),
      fade(static_cast<size_t>(w) * stride) {}

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
  // The metered buses of this topology (ProjectLayout::busMetered), fixed when it is made: the audio thread reads
  // only this list, never BusStore::metered, which a later publishTopology() rewrites (M3 review L12).
  std::vector<BusStore*> metered;
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
  // T5.x-perf-audio: the lanes reduced in registers (the max exactly; the 8 partial sums in double, pairwise), not
  // through memory one by one (one call per metered bus channel per block: 128 for 64 tracks).
  __m128 m4 = _mm_max_ps(_mm256_castps256_ps128(m), _mm256_extractf128_ps(m, 1));
  m4 = _mm_max_ps(m4, _mm_movehl_ps(m4, m4));
  m4 = _mm_max_ss(m4, _mm_shuffle_ps(m4, m4, 1));
  float p = _mm_cvtss_f32(m4);
  const __m256d d4 = _mm256_add_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(sq)), _mm256_cvtps_pd(_mm256_extractf128_ps(sq, 1)));
  const __m128d d2 = _mm_add_pd(_mm256_castpd256_pd128(d4), _mm256_extractf128_pd(d4, 1));
  double s = _mm_cvtsd_f64(_mm_add_sd(d2, _mm_unpackhi_pd(d2, d2)));
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
  {
    auto b = std::make_unique<ParamBlock>();
    b->size = cfg_.numProjectParams;
    b->data = std::make_unique<float[]>(std::max<size_t>(b->size, 1));  // zeros
    params_ = b->data.get();
    numParams_ = b->size;
    reservedParams_ = b->size;
    paramBlocks_.push_back(std::move(b));
  }
  reader_ = *reclaimer_.registerReader();
  silence_ = static_cast<float>(std::pow(10.0, static_cast<double>(cfg_.silenceDb) / 20.0));
  fadeFrames10ms_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(0.01 * cfg_.sampleRate)));
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
  for (size_t b = 0; b < stores_.size(); ++b)
    if (meteredBus(layout_, b)) t->metered.push_back(stores_[b].get());
  t->output = layout_.outputBus != kNoBus ? stores_[layout_.outputBus].get() : nullptr;
  topoInflight_.push_back(t);
  topo_ = t;
}

ProjectProgram::~ProjectProgram() {
  for (Topology* t : topoInflight_) delete t;
  topoInflight_.clear();
  for (Compensation* c : compInflight_) delete c;
  compInflight_.clear();
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
    p->inChannel.push_back(r.bus == kNoBus ? uint16_t{0} : r.channel);
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
    if (slot >= io.params.size() || param >= reservedParams_)
      return projectError(ErrorCode::InvalidArgument, who + "a param link is out of range");
  for (const auto& [slot, param] : ui.paramLinks) p->paramsNeeded = std::max(p->paramsNeeded, param + 1);
  p->paramLinks.assign(ui.paramLinks.begin(), ui.paramLinks.end());
  p->keepAlive.assign(std::make_move_iterator(ui.keepAlive.begin()), std::make_move_iterator(ui.keepAlive.end()));
  ui.keepAlive.clear();
  p->idleAfter = ui.idleAfterBlocks;
  for (uint32_t k : io.clipSlots)  // T5.4
    if (k < cfg_.numClipSlots) p->clipSlots.push_back(k);
  p->linked.assign(p->paramLinks.size(), std::numeric_limits<float>::quiet_NaN());
  for (const BusRef& r : ui.in)
    if (r.bus != kNoBus && std::find(p->inStores.begin(), p->inStores.end(), stores[r.bus].get()) == p->inStores.end())
      p->inStores.push_back(stores[r.bus].get());
  for (const BusRef& r : ui.in) p->inStoreOf.push_back(r.bus != kNoBus ? stores[r.bus].get() : nullptr);
  p->inLive = p->inPtrs;
  p->inLine.assign(p->inPtrs.size(), nullptr);
  p->compSlots.assign(p->inPtrs.size(), 0u);
  p->inSilent.assign(ui.in.size(), 0);
  p->storeLine.assign(p->inStores.size(), nullptr);
  p->outSilent.assign(ui.out.size(), 0);
  for (const BusRef& r : ui.out) {
    if (r.bus == kNoBus) {
      p->outStoreOf.push_back(graph::kNone);
      continue;
    }
    BusStore* b = stores[r.bus].get();
    auto it = std::find(p->outStores.begin(), p->outStores.end(), b);
    if (it == p->outStores.end()) {
      p->outStores.push_back(b);
      it = p->outStores.end() - 1;
    }
    p->outStoreOf.push_back(static_cast<uint32_t>(it - p->outStores.begin()));
  }
  p->outStoreSilent.assign(p->outStores.size(), 0);

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
      s->metered = meteredBus(layout, b);  // non-RT (readMeters, under mutex_); the audio thread reads Topology::metered
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
  for (size_t b = 0; b < stores.size(); ++b)
    if (meteredBus(layout, b)) t->metered.push_back(stores[b].get());
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
  // M6 review L7: the running plan, for this topology, without the lines that read a bus no image of their unit reads
  // here (or one whose writers do not all run before it): taken in the same block as the topology, so no line reads a
  // bus outside the run order's edges (PF.5) until the link's plan for this layout arrives. Dropping them is
  // inaudible: nothing reads them. A unit that is gone loses its entry.
  std::unique_ptr<Compensation> pruned;
  if (compLast_ != nullptr && !compLast_->entries.empty() && compInflight_.size() < 6) {
    std::vector<uint32_t> pos(total, kNoUnit);
    for (uint32_t k = 0; k < order.size(); ++k) pos[order[k]] = k;
    pruned = std::make_unique<Compensation>();
    pruned->seq = nextCompSeq_;
    pruned->topoSeq = t->seq;
    auto keep = [&](const void* x) {
      for (const auto& sp : compLast_->owned)
        if (sp.get() == x && std::none_of(pruned->owned.begin(), pruned->owned.end(), [&](const auto& o) { return o.get() == x; }))
          pruned->owned.push_back(sp);
      for (const auto& sp : compLast_->ownedEvents)
        if (sp.get() == x) pruned->ownedEvents.push_back(sp);
    };
    for (const Compensation::Entry& e : compLast_->entries) {
      uint32_t i = 0;
      while (i < n && (units[i].from == kNoUnit || unitPtr[i] != e.unit)) ++i;  // (a continued unit: alive)
      if (i == n || pos[i] == kNoUnit) continue;  // removed
      Compensation::Entry ne;
      ne.unit = e.unit;
      ne.events = e.events;
      ne.eventDelay = e.eventDelay;
      if (!e.clipIn.empty()) ne.clipIn.assign(e.clipIn.size(), nullptr);  // (refreshed every block before it is read)
      if (e.events != nullptr) keep(e.events);
      for (const Compensation::Line& l : e.lines) {
        const BusStore* b = l.line->bus.get();
        bool ok = b == nullptr;  // a clip slot's line reads no bus
        if (!ok && reads[i].count(b) != 0) {
          ok = true;
          for (uint32_t w = 0; w < total && ok; ++w)
            if (writes[w].count(b) != 0 && (pos[w] == kNoUnit || pos[w] >= pos[i])) ok = false;
        }
        if (!ok) continue;
        for (DelayLine* x = l.line; x != nullptr; x = x->copied.load(std::memory_order_acquire) ? nullptr : x->prev) keep(x);
        ne.lines.push_back(l);
      }
      if (!ne.lines.empty() || ne.events != nullptr) pruned->entries.push_back(std::move(ne));
    }
  }

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
  if (pruned && compRing_.tryPush(pruned.get())) {  // before the topology: the audio thread takes both in one block
    compInflight_.push_back(pruned.get());
    compLast_ = pruned.release();
    ++nextCompSeq_;
  }
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

// RT-safe (audio thread, inside process() before the reclaimer hears that this reader is quiescent: after that, a
// concurrent collect() may free the units a dead slot of the current topology still names, M3 review C1). Dead
// zombie slots (alive == 0) are skipped: their units are retired, possibly already freed.
bool ProjectProgram::computeQuiet() const noexcept {
  if (topoPending_ != nullptr || !topoRing_.emptyApprox()) return false;
  const Topology& t = *topo_;
  for (size_t i = 0; i < t.order.size(); ++i) {
    if (t.alive[i].load(std::memory_order_relaxed) == 0) continue;
    const Unit& U = *t.order[i].unit;
    if (U.fadeFrom != nullptr || !U.ring.emptyApprox()) return false;
    if (U.voices && U.voices->liveVoices() > 0) return false;
    if (!U.tailsDone) return false;  // T1.x-silence: a reverb or delay tail is still ringing out
  }
  if (comp_ != nullptr)  // T6.2: audio or notes still held in a delay
    for (const Compensation::Entry& e : comp_->entries) {
      for (const Compensation::Line& l : e.lines)
        if (l.line->copySrc != nullptr ||
            (l.line->active && (l.line->fadeLen != 0 || l.line->silentRun < static_cast<uint64_t>(l.line->delay) + lastFrames_)))
          return false;
      if (e.events != nullptr && e.events->count > 0) return false;
    }
  return true;
}

void ProjectProgram::reserveProjectParams(uint32_t count) {
  std::lock_guard lock(mutex_);
  if (count <= reservedParams_) return;
  auto b = std::make_unique<ParamBlock>();
  b->size = count;
  b->data = std::make_unique<float[]>(count);  // zeros; the current values are copied over when it is adopted
  ParamBlock* raw = b.get();
  paramBlocks_.push_back(std::move(b));
  reservedParams_ = count;
  // A block reserved earlier and not taken yet is superseded (it stays owned by paramBlocks_ until destruction).
  pendingParams_.exchange(raw, std::memory_order_acq_rel);
}

uint32_t ProjectProgram::projectParamCapacity() const {
  std::lock_guard lock(mutex_);
  return reservedParams_;
}

void ProjectProgram::adoptProjectParams() noexcept {  // RT-safe
  ParamBlock* b = pendingParams_.exchange(nullptr, std::memory_order_acq_rel);
  if (b == nullptr || b->size <= numParams_) return;
  std::memcpy(b->data.get(), params_, sizeof(float) * numParams_);
  params_ = b->data.get();
  numParams_ = b->size;
  ++paramGen_;
}

bool ProjectProgram::canPublish(uint32_t unit) const {
  std::lock_guard lock(mutex_);
  if (!topoInflight_.empty() && topoInflight_.back()->seq > topoCurrent_.load(std::memory_order_acquire)) return false;
  if (unit >= units_.size()) return true;  // publish() reports the bad index
  const Unit& U = *units_[unit];
  return U.ring.sizeApprox() + 1 < std::bit_ceil(std::max(2u, cfg_.maxPendingPerUnit));
}

uint32_t ProjectProgram::queued(uint32_t unit) const {
  std::lock_guard lock(mutex_);
  return unit < units_.size() ? static_cast<uint32_t>(units_[unit]->ring.sizeApprox()) : 0u;
}

bool ProjectProgram::topologyPending() const {
  std::lock_guard lock(mutex_);
  return !topoInflight_.empty() && topoInflight_.back()->seq > topoCurrent_.load(std::memory_order_acquire);
}

void ProjectProgram::setUnitInputs(std::span<const UnitInput> perUnit) {  // non-RT
  std::lock_guard lock(mutex_);
  for (size_t u = 0; u < units_.size(); ++u) {
    const UnitInput in = u < perUnit.size() ? perUnit[u] : UnitInput{};
    const uint32_t word = in.channels == 0 ? 0u : (uint32_t{in.first} | uint32_t{std::min<uint8_t>(in.channels, 2)} << 16);
    units_[u]->input.store(word, std::memory_order_relaxed);
  }
}

bool ProjectProgram::readsLocked(const Unit& U, const BusStore* bus) const {
  if (bus == nullptr) return true;
  for (const Packet* p : U.inflight)
    if (std::find(p->inStores.begin(), p->inStores.end(), bus) != p->inStores.end()) return true;
  return false;
}

Status ProjectProgram::setCompensation(const CompensationPlan& plan) {
  std::lock_guard lock(mutex_);
  collectLocked();
  const auto numUnits = static_cast<uint32_t>(units_.size());
  if (plan.delays.size() != numUnits || plan.unitLatency.size() != numUnits || plan.clipDelays.size() != numUnits ||
      plan.eventDelay.size() != numUnits)
    return projectError(ErrorCode::InvalidArgument, "compensation: the plan is not for the published layout");
  for (uint32_t u = 0; u < numUnits; ++u)
    for (const SourceDelay& d : plan.clipDelays[u])
      if (d.frames > (1u << 26)) return projectError(ErrorCode::InvalidArgument, "compensation: a delay above 2^26 frames");
  for (uint32_t u = 0; u < numUnits; ++u)
    for (const InputDelay& d : plan.delays[u]) {
      if (d.bus >= stores_.size() || layout_.busProducer[d.bus] >= u)
        return projectError(ErrorCode::InvalidArgument, "compensation: unit " + std::to_string(u) + " delays a bus it cannot read");
      if (d.frames > (1u << 26))
        return projectError(ErrorCode::InvalidArgument, "compensation: a delay above 2^26 frames");
    }
  outputLatency_.store(plan.total, std::memory_order_release);
  if (compInflight_.size() >= 6) return projectError(ErrorCode::Overflow, "compensation: too many plans pending");
  auto c = std::make_unique<Compensation>();
  c->seq = nextCompSeq_;
  c->topoSeq = nextTopoSeq_ - 1;  // the last published topology
  const Compensation* last = compLast_;
  for (uint32_t u = 0; u < numUnits; ++u) {
    Unit* U = units_[u].get();
    const Compensation::Entry* prev = nullptr;
    if (last != nullptr)
      for (const Compensation::Entry& e : last->entries)
        if (e.unit == U) prev = &e;
    Compensation::Entry e;
    e.unit = U;
    auto prevLine = [&](const BusStore* b, uint32_t clip) -> const Compensation::Line* {
      if (prev != nullptr)
        for (const Compensation::Line& l : prev->lines)
          if (l.line->bus.get() == b && l.line->clip == clip) return &l;
      return nullptr;
    };
    // A line the plan names, and (M6 review M2) the lines it still copies its history from.
    auto keep = [&](DelayLine* l) {
      for (DelayLine* x = l; x != nullptr; x = x->copied.load(std::memory_order_acquire) ? nullptr : x->prev)
        for (const auto& sp : last->owned)
          if (sp.get() == x && std::none_of(c->owned.begin(), c->owned.end(), [&](const auto& o) { return o.get() == x; }))
            c->owned.push_back(sp);
    };
    // A bus (store) or a clip slot (null store) delayed by `delay`.
    auto addLine = [&](const std::shared_ptr<BusStore>& store, uint32_t clip, uint16_t width, uint32_t delay) {
      const Compensation::Line* pl = prevLine(store.get(), clip);
      const uint32_t need = delay + stride_;
      if (pl != nullptr && pl->line->size >= need) {  // the same line: its history and taps carry over
        keep(pl->line);
        e.lines.push_back({pl->line, delay});
        return;
      }
      // A new line (or a larger one copying the old one's history at the switch). M6 review M2: a line that grows
      // grows by half at least, so a Track Delay drag copies its history a few times, not at every step.
      const uint32_t grown = pl != nullptr ? std::max(pl->delay + stride_, pl->line->size + pl->line->size / 2) : 0u;
      const uint32_t frames = (std::max(need, grown) + kRingGrain - 1) / kRingGrain * kRingGrain;
      auto line = std::make_shared<DelayLine>(store, clip, width, frames, stride_);
      if (pl != nullptr) {
        line->prev = pl->line;
        keep(pl->line);
      }
      e.lines.push_back({line.get(), delay});
      c->owned.push_back(std::move(line));
    };
    for (const InputDelay& d : plan.delays[u]) addLine(stores_[d.bus], kNoSlot, stores_[d.bus]->width, d.frames);
    // T6.2b: its clip slots (a program without clip players has nothing to delay).
    if (cfg_.clipIn != nullptr && cfg_.clipActive != nullptr)
      for (const SourceDelay& d : plan.clipDelays[u])
        if (d.slot < cfg_.numClipSlots) addLine(nullptr, d.slot, static_cast<uint16_t>(kClipChannels), d.frames);
    // A line the plan drops fades to 0 first (then it is bypassed, and a later plan forgets it). M6 review M3: also a
    // line an earlier plan dropped while it still fades (or has not started to), so a second plan within the fade
    // (a Track Delay drag) does not jump to the bus. M6 review L7: only while an image of the unit reads its bus (a
    // line never reads a bus its unit does not: no edge PF.5 would not see).
    if (prev != nullptr)
      for (const Compensation::Line& l : prev->lines)
        if ((l.delay != 0 || l.line->live.load(std::memory_order_acquire)) && readsLocked(*U, l.line->bus.get()) &&
            std::none_of(e.lines.begin(), e.lines.end(), [&](const Compensation::Line& x) { return x.line->sameSource(*l.line); })) {
          keep(l.line);
          e.lines.push_back({l.line, 0u});
        }
    if (std::any_of(e.lines.begin(), e.lines.end(), [](const Compensation::Line& l) { return l.line->bus == nullptr; }))
      e.clipIn.assign(static_cast<size_t>(cfg_.numClipSlots) * kClipChannels, nullptr);
    // A unit's event delay stays once made (events it holds must still come out, note-offs above all).
    if (U->cfg.voices && (plan.eventDelay[u] > 0 || (prev != nullptr && prev->events != nullptr))) {
      e.eventDelay = plan.eventDelay[u];
      if (prev != nullptr && prev->events != nullptr) {
        e.events = prev->events;
        for (const auto& sp : last->ownedEvents)
          if (sp.get() == e.events) c->ownedEvents.push_back(sp);
      } else {
        auto ed = std::make_shared<EventDelay>(std::max(cfg_.eventCapacity, 64u) * 4);
        e.events = ed.get();
        c->ownedEvents.push_back(std::move(ed));
      }
    }
    if (!e.lines.empty() || e.events != nullptr) c->entries.push_back(std::move(e));
  }
  if (c->entries.empty() && (last == nullptr || last->entries.empty())) return Ok{};  // nothing runs, nothing to stop
  Compensation* raw = c.get();
  compInflight_.push_back(raw);
  if (!compRing_.tryPush(raw)) {
    compInflight_.pop_back();
    return projectError(ErrorCode::Overflow, "compensation: too many plans pending");
  }
  c.release();
  compLast_ = raw;
  ++nextCompSeq_;
  return Ok{};
}

bool ProjectProgram::compensating(uint32_t unit, uint32_t bus) const noexcept {  // RT-safe
  if (comp_ == nullptr || unit >= units_.size() || bus >= topo_->stores.size()) return false;
  const Unit& U = *units_[unit];
  if (U.compSeq != comp_->seq || U.compEntry == nullptr) return false;
  for (const Compensation::Line& l : U.compEntry->lines)
    if (l.line->bus.get() == topo_->stores[bus].get() && l.line->active) return true;
  return false;
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
  // T6.2: plans older than the one the audio thread runs are no longer read (a plan it dropped unused is older too).
  const uint64_t compNow = compCurrent_.load(std::memory_order_acquire);
  k = 0;
  while (k < compInflight_.size() && compInflight_[k]->seq < compNow) {
    reclaimer_.retire(compInflight_[k], [](void* q) noexcept { delete static_cast<Compensation*>(q); });
    ++k;
  }
  compInflight_.erase(compInflight_.begin(), compInflight_.begin() + static_cast<std::ptrdiff_t>(k));
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
  if (p == nullptr) return {};
  p->paramsGen = ~uint64_t{0};  // the caller may write: the next block reads them all
  return std::span<float>(p->params);
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
  if (p->paramsNeeded > numParams_) adoptProjectParams();  // M4: links reserved after this block's adoption check
  ++U.stats.swaps;
  U.quietBlocks = 0;  // a new image runs (idle units)
  U.idle = false;
  U.dormant = false;  // T1.x-silence: until the new image says so itself
  U.tailsDone = false;
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
  p->paramsGen = ~uint64_t{0};
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

void ProjectProgram::maybeSwitchCompensation() noexcept {  // RT-safe
  for (;;) {
    if (compPending_ == nullptr && !compRing_.tryPop(compPending_)) return;
    Compensation& c = *compPending_;
    if (c.topoSeq > topo_->seq) return;  // made for a topology the audio thread has not switched to yet
    if (c.topoSeq == topo_->seq) {
      for (const Compensation::Entry& e : c.entries) {
        e.unit->compEntry = &e;
        e.unit->compSeq = c.seq;
        for (const Compensation::Line& l : e.lines) {
          DelayLine& L = *l.line;
          // A new line of a unit that has not played yet (the first plan of a program) starts at its tap: nothing to
          // fade from.
          if (L.prev == nullptr && !L.active && L.delay == 0 && (e.unit->cur == nullptr || stats_.blocks == 0)) {
            L.delay = l.delay;
            L.setActive(l.delay != 0);
            continue;
          }
          if (L.prev == nullptr || L.copySrc != nullptr || L.copied.load(std::memory_order_relaxed)) continue;
          // A larger ring replacing a line: its history, taps and fade (M6 review M2: the history over the next blocks,
          // runLine / copyStep). The line that plays now: the one it replaces, or, while that one still copies its
          // own history, the one that one copies from (a second growth within the copy).
          DelayLine* P = L.prev;
          while (P->prev != nullptr && !P->copied.load(std::memory_order_relaxed)) {
            if (P->copySrc != nullptr) {
              P = P->copySrc;
              break;
            }
            P = P->prev;  // never ran (its plan was dropped unused): what it would have replaced
          }
          L.write = 0;  // the history ends just before frame 0 of the larger ring (laid out by copyStep)
          if (!P->active || P->silentRun >= P->size) {  // nothing to copy: bypassed, or only zeros (L's ring is zeros)
            adoptLine(L, *P);
            L.silentRun = uint64_t{1} << 62;
            L.copied.store(true, std::memory_order_release);
            continue;
          }
          L.copySrc = P;
          L.copyFrom = P->write;
          L.copyDone = 0;
          L.silentRun = P->silentRun;
          L.setActive(P->active);  // (the reader reads P's block through L meanwhile)
        }
      }
      comp_ = compPending_;
      compCurrent_.store(c.seq, std::memory_order_release);
    }
    // (A plan for an older topology is dropped unused: its units may be gone.)
    compPending_ = nullptr;
  }
}

// A larger line takes over the state of the line it replaces (its history is in, or there is none).
void ProjectProgram::adoptLine(DelayLine& L, const DelayLine& P) noexcept {  // RT-safe
  L.setActive(P.active);
  L.delay = P.delay;
  L.from = P.from;
  L.fadePos = P.fadePos;
  L.fadeLen = P.fadeLen;
  L.outSilent = P.outSilent;
}

// The push half of runLine: this block of the line's source into its ring (and mirror).
void ProjectProgram::pushLine(DelayLine& L, uint32_t frames) noexcept {  // RT-safe
  // Its source: a bus, or (T6.2b) a clip slot of the timeline's clip player.
  const float* const* clipCh = L.src == nullptr ? cfg_.clipIn + static_cast<size_t>(L.clip) * kClipChannels : nullptr;
  const bool silent = L.src != nullptr ? *L.srcSilent : cfg_.clipActive[L.clip] == 0;
  // Push: [at, at + first) and the wrapped [0, rest); what lands below `mirror` is written to the mirror too.
  const uint32_t at = L.write, first = std::min(frames, L.size - at), rest = frames - first;
  const uint32_t mirrored = at < L.mirror ? std::min(first, L.mirror - at) : 0u;
  if (silent) {
    if (L.silentRun < L.size)
      for (uint16_t c = 0; c < L.width; ++c) {
        float* r = L.chan(c);
        std::memset(r + at, 0, sizeof(float) * first);
        if (mirrored != 0) std::memset(r + L.size + at, 0, sizeof(float) * mirrored);
        if (rest != 0) {
          std::memset(r, 0, sizeof(float) * rest);
          std::memset(r + L.size, 0, sizeof(float) * rest);
        }
      }
    if (L.silentRun < (uint64_t{1} << 62)) L.silentRun += frames;
  } else {
    for (uint16_t c = 0; c < L.width; ++c) {
      float* r = L.chan(c);
      const float* x = L.src != nullptr ? L.src + static_cast<size_t>(c) * L.srcStride
                       : clipCh[c] != nullptr ? clipCh[c]
                                              : zeros_.data();
      copyBlock(r + at, x, first);
      if (mirrored != 0) copyBlock(r + L.size + at, x, mirrored);
      if (rest != 0) {
        copyBlock(r, x + first, rest);
        copyBlock(r + L.size, x + first, rest);
      }
    }
    L.silentRun = 0;
  }
  L.write = rest != 0 || at + frames == L.size ? rest : at + frames;
}

// M6 review M2: one block of a grown line's history copy. The history (copySrc's ring, oldest first from copyFrom)
// lands at the end of L's ring, before frame 0 where L's pushes started at the switch. Each block copies at least
// twice its frames (so the copy stays ahead of copySrc's pushes, which overwrite its oldest frames, and of L's, which
// overwrite the slots below the history), more while the block's budget (kCopyFloatsPerFrame per frame, shared by every
// line) lasts. True: the history is in and L took copySrc's state (it plays from this block on). False: copySrc played
// this block, L took the push, the reader reads copySrc's block through L.
bool ProjectProgram::copyStep(DelayLine& L, uint32_t frames) noexcept {  // RT-safe
  DelayLine& P = *L.copySrc;
  if (copyBlockSeq_ != stats_.blocks) {  // the first copy of this block
    copyBlockSeq_ = stats_.blocks;
    copyLeft_ = static_cast<uint64_t>(frames) * kCopyFloatsPerFrame;
    copyThisBlock_ = 0;
  }
  if (P.active) {
    const uint32_t left = P.size - L.copyDone;
    const uint64_t budget = copyLeft_ / std::max<uint16_t>(L.width, 1);
    const auto n = static_cast<uint32_t>(std::min<uint64_t>(left, std::max<uint64_t>(2ull * frames, budget)));
    const uint32_t s0 = L.copyFrom + L.copyDone < P.size ? L.copyFrom + L.copyDone : L.copyFrom + L.copyDone - P.size;
    const uint32_t a = std::min(n, P.size - s0), b = n - a;  // two spans of P's ring
    const uint32_t d0 = L.size - P.size + L.copyDone;
    for (uint16_t ch = 0; ch < std::min(L.width, P.width); ++ch) {
      const float* src = P.chan(ch);
      float* dst = L.chan(ch);
      std::memcpy(dst + d0, src + s0, sizeof(float) * a);
      if (b != 0) std::memcpy(dst + d0 + a, src, sizeof(float) * b);
      if (d0 < L.mirror)  // (a history that starts inside the mirrored frames)
        std::memcpy(dst + L.size + d0, dst + d0, sizeof(float) * (std::min(d0 + n, L.mirror) - d0));
    }
    const uint64_t floats = static_cast<uint64_t>(n) * L.width;
    copyLeft_ -= std::min(copyLeft_, floats);
    copyThisBlock_ += floats;
    stats_.lineCopyFloats += floats;
    stats_.lineCopyBlockMax = std::max(stats_.lineCopyBlockMax, copyThisBlock_);
    L.copyDone += n;
  }
  // Done (or copySrc was bypassed meanwhile: L is too, and its ring is cleared when it starts again).
  if (!P.active || L.copyDone >= P.size) {
    adoptLine(L, P);
    L.copySrc = nullptr;
    L.copied.store(true, std::memory_order_release);
    ++stats_.lineCopies;
    return true;
  }
  runLine(P, P.delay, frames);  // (P copies nothing: it is the line that plays)
  pushLine(L, frames);
  L.setActive(P.active);
  L.outSilent = P.outSilent;
  L.outBase = P.outBase;
  L.outStride = P.outStride;
  return false;
}

void ProjectProgram::runLine(DelayLine& L, uint32_t target, uint32_t frames) noexcept {  // RT-safe
  if (L.copySrc != nullptr && !copyStep(L, frames)) return;  // M6 review M2: its history is still being copied
  if (L.fadeLen == 0 && target != L.delay) {
    // A bypassed line's ring is stale: zeros from here (bounded by the ring), unless it holds only zeros already (a
    // fresh ring, or one that was silent through its whole length: M6 review M2).
    if (!L.active) {
      if (L.silentRun < L.size) std::memset(L.ring.data(), 0, sizeof(float) * L.ring.size());
      L.silentRun = uint64_t{1} << 62;
    }
    L.from = L.delay;
    L.delay = target;
    L.fadePos = 0;
    L.fadeLen = fadeFrames10ms_;
    L.setActive(true);
  }
  if (!L.active) return;
  const uint32_t at = L.write;
  pushLine(L, frames);
  const uint32_t maxTap = std::max(L.delay, L.fadeLen != 0 ? L.from : 0u);
  L.outSilent = L.silentRun >= static_cast<uint64_t>(maxTap) + frames;
  if (!L.outSilent) {
    // Read: the tap is contiguous (the mirror), so the reader reads the ring in place; a fade writes its blend.
    // (taps ≤ size − one block: no wrap below 0 after one add)
    const uint32_t i = at >= L.delay ? at - L.delay : at + L.size - L.delay;
    if (L.fadeLen == 0) {
      L.outBase = L.ring.data() + i;
      L.outStride = static_cast<size_t>(L.size) + L.mirror;
    } else {  // from the old tap to the new one (ProgramHost's linear fade)
      const uint32_t j = at >= L.from ? at - L.from : at + L.size - L.from;
      for (uint16_t c = 0; c < L.width; ++c) {
        const float* r = L.chan(c);
        crossfade(L.fade.data() + static_cast<size_t>(c) * stride_, r + j, r + i, frames, L.fadePos, L.fadeLen);
      }
      L.outBase = L.fade.data();
      L.outStride = stride_;
    }
  }
  if (L.fadeLen != 0) {
    L.fadePos += frames;
    if (L.fadePos >= L.fadeLen) {
      L.fadeLen = 0;
      if (L.delay == 0) L.setActive(false);  // faded out: bypassed from the next block on
    }
  }
}

void ProjectProgram::delayEvents(Unit& U, EventDelay& d, uint32_t delay, uint32_t frames, int64_t time) noexcept {  // RT-safe
  const EventList in = U.eventBuf.list();
  if (delay == 0 && d.count == 0) return;  // nothing held: the events pass as they are
  const auto cap = static_cast<uint32_t>(d.items.size());
  for (uint32_t i = 0; i < in.count; ++i) {
    const NoteEvent& ev = in.events[i];
    const bool off = ev.kind == NoteEventKind::NoteOff || ev.kind == NoteEventKind::AllNotesOff ||
                     ev.kind == NoteEventKind::AllSoundOff;
    // M6 review L10: the off of a note whose on was dropped is dropped too (it would take the room of a held note's).
    if (ev.kind == NoteEventKind::NoteOff && d.numDroppedIds != 0 && d.forgetDropped(ev.noteId)) continue;
    if (d.count == cap) {
      // Full. An off takes the place of the newest held event that is not one (a note-on, which then never sounds,
      // or a pitch change): an off is never dropped while anything else is held. Anything else is dropped.
      bool room = false;
      if (off)
        for (uint32_t k = d.count; k-- > 0;) {
          const NoteEvent& h = d.items[(d.head + k) % cap].e;
          if (h.kind == NoteEventKind::NoteOff || h.kind == NoteEventKind::AllNotesOff || h.kind == NoteEventKind::AllSoundOff)
            continue;
          if (h.kind == NoteEventKind::NoteOn) d.dropOn(h.noteId);
          for (uint32_t m = k; m + 1 < d.count; ++m) d.items[(d.head + m) % cap] = d.items[(d.head + m + 1) % cap];
          --d.count;
          room = true;
          break;
        }
      ++d.dropped;
      ++stats_.eventDelayDrops;
      if (!room) {
        if (ev.kind == NoteEventKind::NoteOn) d.dropOn(ev.noteId);
        continue;
      }
    }
    // Time order is kept (a shorter delay than the held events' never moves an event before them).
    const int64_t at = std::max(d.lastAt, time + static_cast<int64_t>(ev.frame) + delay);
    d.items[(d.head + d.count) % cap] = {at, ev};
    ++d.count;
    d.lastAt = at;
  }
  U.eventBuf.clear();
  const int64_t end = time + frames;
  while (d.count > 0 && d.items[d.head].at < end) {
    NoteEvent e = d.items[d.head].e;
    e.frame = static_cast<uint32_t>(std::max<int64_t>(0, d.items[d.head].at - time));
    U.eventBuf.push(e);
    d.head = (d.head + 1) % cap;
    --d.count;
  }
}

bool ProjectProgram::runUnit(Unit& U, const Slot& s, uint32_t frames, int64_t time) noexcept {  // RT-safe
  if (U.fadeFrom == nullptr) trySwap(U, s);
  Packet* p = U.cur;
  if (p == nullptr) return false;  // no image yet: its buses keep reading zeros
  Packet* old = U.fadeFrom;
  // T6.2: this unit's compensation (its delay lines run first, in its own slot: see the header).
  const Compensation::Entry* ce = comp_ != nullptr && U.compSeq == comp_->seq ? U.compEntry : nullptr;
  if (ce != nullptr)
    for (const Compensation::Line& l : ce->lines) runLine(*l.line, l.delay, frames);
  if (U.voices) {
    U.eventBuf.clear();
    if (s.events.fn != nullptr) s.events.fn(s.events.user, time, frames, U.eventBuf);
    if (ce != nullptr && ce->events != nullptr) delayEvents(U, *ce->events, ce->eventDelay, frames, time);
  }
  // T6.2: the packets' slot → delay line maps, rebuilt when the plan changed (O(slots · lines), once per plan): every
  // other block costs one lookup per slot.
  auto mapLines = [&](Packet& q) noexcept {
    if (q.lineSeq == comp_->seq) return;
    q.lineSeq = comp_->seq;
    auto lineOf = [&](const BusStore* b) noexcept -> DelayLine* {
      for (const Compensation::Line& l : ce->lines)
        if (l.line->bus.get() == b) return l.line;
      return nullptr;
    };
    q.numCompSlots = 0;
    for (size_t k = 0; k < q.inStoreOf.size(); ++k) {
      q.inLine[k] = q.inStoreOf[k] != nullptr ? lineOf(q.inStoreOf[k]) : nullptr;  // (clip lines have no store)
      q.inLive[k] = q.inPtrs[k];
      if (q.inLine[k] != nullptr) q.compSlots[q.numCompSlots++] = static_cast<uint32_t>(k);
    }
    for (size_t k = 0; k < q.inStores.size(); ++k) q.storeLine[k] = lineOf(q.inStores[k]);
  };
  if (ce != nullptr) {
    mapLines(*p);
    if (old != nullptr) mapLines(*old);
  }
  // T6.7: the unit's track input (device channels of this block; null: silence).
  const float* trackIn[2] = {nullptr, nullptr};
  if (const uint32_t inp = U.input.load(std::memory_order_relaxed); inp != 0 && blockIn_ != nullptr) {
    const uint32_t first = inp & 0xFFFFu, n = inp >> 16;
    for (uint32_t c = 0; c < 2; ++c) {
      const uint32_t ch = first + std::min(c, n - 1);
      trackIn[c] = ch < blockNumIn_ && blockIn_[ch] != nullptr ? blockIn_[ch] + blockInOff_ : nullptr;
    }
  }
  const bool liveInput = trackIn[0] != nullptr || trackIn[1] != nullptr;
  // Whether a bus (through its delay line, if one runs) reads silent for this unit.
  auto silentVia = [](const BusStore* b, const DelayLine* l) noexcept {
    return l != nullptr && l->active ? l->outSilent : b->silent;
  };
  // T6.2b: a clip slot's line in this unit's plan entry (null: the slot is not delayed), and whether the slot reads
  // silent for it.
  const bool clipLines = ce != nullptr && !ce->clipIn.empty();
  auto clipLine = [&](uint32_t k) noexcept -> const DelayLine* {
    for (const Compensation::Line& l : ce->lines)
      if (l.line->bus == nullptr && l.line->clip == k) return l.line->active ? l.line : nullptr;
    return nullptr;
  };
  auto clipSilent = [&](uint32_t k) noexcept {
    const DelayLine* l = clipLines ? clipLine(k) : nullptr;
    return l != nullptr ? l->outSilent : cfg_.clipActive[k] == 0;
  };
  if (cfg_.idleUnits) {
    // Idle units (see the header): bit-exact skip of a unit that would change nothing — proven by the compiler
    // (idleAfterBlocks: settled gains only), or reported by its image after its last block (T1.x-silence: dormant,
    // every sleepable region asleep, nothing else keeping state, outputs silent).
    bool quiet = old == nullptr && !liveInput && (!U.voices || (U.voices->liveVoices() == 0 && U.eventBuf.list().count == 0));
    if (ce != nullptr) {
      for (size_t k = 0; k < p->inStores.size(); ++k) quiet = quiet && silentVia(p->inStores[k], p->storeLine[k]);
    } else {
      for (const BusStore* b : p->inStores) quiet = quiet && b->silent;
    }
    for (uint32_t k : p->clipSlots) quiet = quiet && clipSilent(k);  // T5.4: no audio from its clips (T6.2b: delayed)
    // M4 review L1: the links are compared only when a param may have changed since they were last read.
    if (p->linkedGen != blockGen_) {
      p->linkedGen = blockGen_;
      for (size_t i = 0; i < p->paramLinks.size(); ++i) {
        const float v = params_[p->paramLinks[i].second];
        quiet = quiet && std::bit_cast<uint32_t>(v) == std::bit_cast<uint32_t>(p->linked[i]);
        p->linked[i] = v;
      }
    }
    U.quietBlocks = quiet ? U.quietBlocks + 1 : 0;
    if ((p->idleAfter != 0 && U.quietBlocks > p->idleAfter) || (U.dormant && quiet)) {
      U.tailsDone = true;  // settled gains (idleAfterBlocks' proof), or every sleepable region asleep (dormant)
      if (!U.idle) {
        for (float* ch : s.own) std::memset(ch, 0, sizeof(float) * stride_);
        for (const auto& b : topo_->stores)
          for (float* ch : s.own)
            if (ch >= b->data.data() && ch < b->data.data() + b->data.size()) b->silent = true;
        U.idle = true;
      }
      if (U.voices) U.voices->advanceIdle(frames);
      ++U.stats.idleBlocks;
      return false;
    }
  }
  if (U.idle) {
    U.idle = false;
    for (const auto& b : topo_->stores)
      for (float* ch : s.own)
        if (ch >= b->data.data() && ch < b->data.data() + b->data.size()) b->silent = false;
  }
  // T5.x-perf-audio: no project param written since this packet's params were last refreshed (the same generation the
  // idle-unit scan uses): they are unchanged, and the image is told so (it skips its param pass).
  const bool paramsSame = p->paramsGen == blockGen_;
  if (!paramsSame) {
    for (const auto& [slot, param] : p->paramLinks) p->params[slot] = params_[param];
    p->paramsGen = blockGen_;
  }
  // T6.2: a compensated input reads its delay line's block (or the zeros, once the line reads only silence); the other
  // slots keep their bus (mapLines). O(compensated slots).
  auto live = [&](Packet& q) noexcept {
    for (uint32_t i = 0; i < q.numCompSlots; ++i) {
      const uint32_t k = q.compSlots[i];
      const DelayLine* l = q.inLine[k];
      q.inLive[k] = !l->active    ? q.inPtrs[k]
                    : l->outSilent ? zeros_.data()
                                   : l->outBase + static_cast<size_t>(q.inChannel[k]) * l->outStride;
    }
  };
  // T6.2b: the clip table of a unit with delayed clip slots: its slots' channels as the lines read them.
  auto liveClips = [&](const Packet& q) noexcept {
    for (uint32_t k : q.clipSlots) {
      const DelayLine* l = clipLine(k);
      for (uint32_t c = 0; c < kClipChannels; ++c) {
        const size_t i = static_cast<size_t>(k) * kClipChannels + c;
        ce->clipIn[i] = l == nullptr ? cfg_.clipIn[i] : l->outSilent ? zeros_.data() : l->outBase + c * l->outStride;
      }
    }
  };
  if (clipLines) {
    liveClips(*p);
    if (old != nullptr) liveClips(*old);
  }
  for (size_t k = 0; k < p->inStoreOf.size(); ++k) p->inSilent[k] = p->inStoreOf[k] == nullptr || p->inStoreOf[k]->silent;
  if (ce != nullptr) {
    live(*p);
    if (old != nullptr) live(*old);
    for (uint32_t i = 0; i < p->numCompSlots; ++i) {
      const uint32_t k = p->compSlots[i];
      if (p->inLine[k]->active) p->inSilent[k] = p->inLine[k]->outSilent;
    }
  }
  // PF.4b: the block's shared fields are in unitCtx_ already (process()); this unit's are set here, and every field an
  // image or a voice engine writes is reset.
  ProcessContext& ctx = unitCtx_;
  ctx.inSilent = p->inSilent.data();
  ctx.outSilent = p->outSilent.data();
  ctx.in = ce != nullptr ? p->inLive.data() : p->inPtrs.data();
  ctx.numIn = static_cast<uint32_t>(p->inPtrs.size());
  ctx.out = p->outPtrs.data();
  ctx.numOut = static_cast<uint32_t>(p->outPtrs.size());
  ctx.params = p->params.data();
  ctx.numParams = static_cast<uint32_t>(p->params.size());
  ctx.paramsUnchanged = paramsSame && !U.voices;  // a voice engine's images get their own contexts
  ctx.clipIn = clipLines ? ce->clipIn.data() : cfg_.clipIn;  // T5.4 (T6.2b: delayed)
  ctx.trackIn = trackIn;  // T6.7
  ctx.events = nullptr;
  ctx.voices = nullptr;
  ctx.tailsDone = false;
  ctx.dormant = false;
  if (old == nullptr) {  // PF.4b: no fade context to build (a ProcessContext is ~4 cache lines of stores)
    if (U.voices) {
      ctx.events = &U.eventBuf.list();
      U.voices->process(ctx, nullptr);
    } else {
      p->image->process(ctx);
    }
  } else {
    for (const auto& [slot, param] : old->paramLinks) old->params[slot] = params_[param];
    ProcessContext fctx = ctx;
    fctx.in = ce != nullptr ? old->inLive.data() : old->inPtrs.data();
    fctx.numIn = static_cast<uint32_t>(old->inPtrs.size());
    fctx.out = old->fadePtrs.data();
    fctx.numOut = static_cast<uint32_t>(old->fadePtrs.size());
    fctx.params = old->params.data();
    fctx.numParams = static_cast<uint32_t>(old->params.size());
    fctx.paramsUnchanged = false;
    fctx.inSilent = nullptr;
    fctx.outSilent = nullptr;
    if (U.voices) {
      ctx.events = &U.eventBuf.list();
      U.voices->process(ctx, &fctx);
    } else {
      p->image->process(ctx);
      TapWriteMute mute;  // TMM.13-fix1 L3: only the incoming image writes analysis taps during the fade
      old->image->process(fctx);
    }
  }
  // T1.x-silence: the image's silence report. A bus is silent when every slot writing it was (never during a fade:
  // the faded bus is old + g·(new − old)).
  U.dormant = old == nullptr && ctx.dormant;
  U.tailsDone = old == nullptr && ctx.tailsDone;
  for (uint8_t& z : p->outStoreSilent) z = old == nullptr ? 1 : 0;
  for (size_t k = 0; k < p->outStoreOf.size(); ++k)
    if (p->outStoreOf[k] != graph::kNone && p->outSilent[k] == 0) p->outStoreSilent[p->outStoreOf[k]] = 0;
  for (size_t k = 0; k < p->outStores.size(); ++k) p->outStores[k]->silent = p->outStoreSilent[k] != 0;
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
  if (pendingParams_.load(std::memory_order_relaxed) != nullptr) adoptProjectParams();  // M4: a grown param block
  const uint32_t maxBlock = cfg_.maxBlockFrames;
  const uint32_t numOut = std::min(cfg_.numOut, block.numOut);
  blockIn_ = block.numIn != 0 ? block.in : nullptr;  // T6.7: the tracks' inputs
  blockNumIn_ = blockIn_ != nullptr ? block.numIn : 0u;
  for (uint32_t off = 0; off < block.frames; off += maxBlock) {
    const uint32_t n = std::min(maxBlock, block.frames - off);
    const int64_t time = block.frameTime + off;
    blockInOff_ = off;
    if (cfg_.hook.fn != nullptr) cfg_.hook.fn(cfg_.hook.user, time, n, transport_, std::span<float>(params_, numParams_));
    // L1: this block's param generation (a hook without a counter may have written anything: a new generation).
    const uint64_t hookWrites =
        cfg_.hook.fn == nullptr ? 0 : cfg_.hook.paramWrites != nullptr ? *cfg_.hook.paramWrites : ++hookGen_;
    blockGen_ = paramGen_ + hookWrites;
    maybeSwitchTopology();
    if (compPending_ != nullptr || !compRing_.emptyApprox()) maybeSwitchCompensation();  // T6.2
    unitCtx_.frames = n;  // PF.4b: what every unit's context shares this block (runUnit sets the rest)
    unitCtx_.sampleRate = cfg_.sampleRate;
    unitCtx_.sampleTime = time;
    unitCtx_.transport = transport_;
    unitCtx_.numClipSlots = cfg_.clipIn != nullptr ? cfg_.numClipSlots : 0u;
    unitCtx_.numTrackIn = 2;
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
    for (BusStore* bp : t.metered) {
      BusStore& b = *bp;
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
  quiet_ = computeQuiet();  // before quiescent(): see computeQuiet
  reclaimer_.quiescent(reader_);
}

}  // namespace pq::engine
