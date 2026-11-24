#include "pq/engine/arp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <type_traits>
#include <utility>

#include "pq/tuning/midi_key.h"

namespace pq::engine {

static_assert(std::is_trivially_copyable_v<ArpParams>);  // ArpCore::process compares / copies it bytewise first

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

int64_t floorDiv(int64_t a, int64_t b) noexcept {
  const int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

uint64_t mix64(uint64_t x) noexcept {  // splitmix64's finalizer
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return x;
}

// The counter RNG: one value per (seed, grid step, draw).
uint64_t draw(uint32_t seed, int64_t step, uint32_t i) noexcept {
  return mix64(mix64(static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) ^
               mix64(static_cast<uint64_t>(step) + 0xD1B54A32D192ED03ull * (i + 1)));
}

int32_t clampi(float v, int32_t lo, int32_t hi) noexcept {
  if (!(v == v)) return lo;
  const float r = std::nearbyint(std::clamp(v, static_cast<float>(lo), static_cast<float>(hi)));
  return static_cast<int32_t>(r);
}

ArpGlideShape shapeOf(float v) noexcept {
  return static_cast<ArpGlideShape>(clampi(v, 0, static_cast<int32_t>(ArpGlideShape::Count) - 1));
}

ArpRate sanitizedRate(ArpRate r) noexcept {
  r.num = std::clamp(r.num, 1, 16);
  r.den = std::clamp(r.den, 1, kArpMaxRateDen);
  r.tupletN = std::clamp(r.tupletN, 1, kArpMaxTuplet);
  r.tupletM = std::clamp(r.tupletM, 1, kArpMaxTuplet);
  r.ms = std::isfinite(r.ms) ? std::clamp(r.ms, 1.0f, 10000.0f) : 125.0f;
  return r;
}

bool sameRate(const ArpRate& a, const ArpRate& b) noexcept {
  return a.synced == b.synced && a.num == b.num && a.den == b.den && a.tupletN == b.tupletN &&
         a.tupletM == b.tupletM && a.ms == b.ms;
}

// A synced length as an exact rational of ticks: whole · num · m / (den · n), reduced.
void rateTicks(const ArpRate& r, int64_t& num, int64_t& den) noexcept {
  num = kTicksPerWhole * r.num * r.tupletM;
  den = static_cast<int64_t>(r.den) * r.tupletN;
  const int64_t g = std::gcd(num, den);
  num /= g;
  den /= g;
}

}  // namespace

void setArpParam(ArpParams& p, ArpParamId id, float v) noexcept {  // RT-safe
  const bool on = v > 0.5f;
  switch (id) {
    case ArpParamId::Style: p.style = static_cast<ArpStyle>(clampi(v, 0, static_cast<int32_t>(ArpStyle::Count) - 1)); break;
    case ArpParamId::Synced: p.rate.synced = on; break;
    case ArpParamId::RateNum: p.rate.num = clampi(v, 1, 16); break;
    case ArpParamId::RateDen: p.rate.den = clampi(v, 1, kArpMaxRateDen); break;
    case ArpParamId::TupletN: p.rate.tupletN = clampi(v, 1, kArpMaxTuplet); break;
    case ArpParamId::TupletM: p.rate.tupletM = clampi(v, 1, kArpMaxTuplet); break;
    case ArpParamId::RateMs: p.rate.ms = v; break;
    case ArpParamId::Gate: p.gate = v; break;
    case ArpParamId::Groove: p.groove = v; break;
    case ArpParamId::Offset: p.offset = clampi(v, -4096, 4096); break;
    case ArpParamId::Repeats: p.repeats = clampi(v, 0, 1 << 20); break;
    case ArpParamId::Retrigger: p.retrigger = static_cast<ArpRetrigger>(clampi(v, 0, 2)); break;
    case ArpParamId::RetriggerNum: p.retriggerRate.num = clampi(v, 1, 16); break;
    case ArpParamId::RetriggerDen: p.retriggerRate.den = clampi(v, 1, kArpMaxRateDen); break;
    case ArpParamId::Hold: p.hold = on; break;
    case ArpParamId::Unit: p.unit = static_cast<ArpUnit>(clampi(v, 0, 2)); break;
    case ArpParamId::Transpose: p.transpose = static_cast<ArpTranspose>(clampi(v, 0, 1)); break;
    case ArpParamId::Distance: p.distance = clampi(v, -64, 64); break;
    case ArpParamId::Steps: p.steps = clampi(v, 1, kArpMaxSteps); break;
    case ArpParamId::RatioP: p.ratioP = clampi(v, 1, 64); break;
    case ArpParamId::RatioQ: p.ratioQ = clampi(v, 1, 64); break;
    case ArpParamId::VelocitySource: p.velocitySource = static_cast<ArpVelocitySource>(clampi(v, 0, 2)); break;
    case ArpParamId::FixedVelocity: p.fixedVelocity = v; break;
    case ArpParamId::VelocityDecayMs: p.velocityDecayMs = v; break;
    case ArpParamId::VelocityTarget: p.velocityTarget = v; break;
    case ArpParamId::VelocityRetrigger: p.velocityRetrigger = on; break;
    case ArpParamId::Seed: p.seed = static_cast<uint32_t>(clampi(v, 0, 1 << 24)); break;
    case ArpParamId::HarmonicBase: p.harmonicBase = clampi(v, 1, 64); break;
    case ArpParamId::WalkRange: p.walkRange = clampi(v, 0, kArpMaxWalkRange); break;
    case ArpParamId::WalkUnder: p.walkUnder = on; break;
    case ArpParamId::WalkOrder: p.walkOrder = static_cast<ArpWalkOrder>(clampi(v, 0, 3)); break;
    case ArpParamId::IntoOffset: p.into.offset = v; break;
    case ArpParamId::IntoPrev: p.into.step = on; break;
    case ArpParamId::IntoTime: p.into.time = v; break;
    case ArpParamId::IntoCurve: p.into.curve = v; break;
    case ArpParamId::IntoShape: p.into.shape = shapeOf(v); break;
    case ArpParamId::OutOffset: p.out.offset = v; break;
    case ArpParamId::OutNext: p.out.step = on; break;
    case ArpParamId::OutTime: p.out.time = v; break;
    case ArpParamId::OutCurve: p.out.curve = v; break;
    case ArpParamId::OutShape: p.out.shape = shapeOf(v); break;
    case ArpParamId::Pulse: p.pulse = on; break;
    case ArpParamId::PulseAuto: p.pulseAuto = on; break;
    case ArpParamId::PulseCycleNum: p.pulseCycleNum = clampi(v, 1, kArpMaxPulseBars); break;
    case ArpParamId::PulseCycleDen: p.pulseCycleDen = clampi(v, 1, kArpMaxPulseCycleDen); break;
    case ArpParamId::Layers: p.layers = clampi(v, 0, kArpMaxLayers); break;
    case ArpParamId::LayerLevel1:
    case ArpParamId::LayerLevel2:
    case ArpParamId::LayerLevel3:
      p.layerLevel[static_cast<size_t>(static_cast<int>(id) - static_cast<int>(ArpParamId::LayerLevel1))] = v;
      break;
    case ArpParamId::VelocityLane:
    case ArpParamId::GateLane:
    case ArpParamId::GlideLane:
    case ArpParamId::TransposeLane:
    case ArpParamId::ChanceLane:
    case ArpParamId::SkipTieLane:
      p.laneOn[static_cast<size_t>(static_cast<int>(id) - static_cast<int>(ArpParamId::VelocityLane))] = on;
      break;
    case ArpParamId::VelocityLength:
    case ArpParamId::GateLength:
    case ArpParamId::GlideLength:
    case ArpParamId::TransposeLength:
    case ArpParamId::ChanceLength:
    case ArpParamId::SkipTieLength:
      p.laneLength[static_cast<size_t>(static_cast<int>(id) - static_cast<int>(ArpParamId::VelocityLength))] =
          clampi(v, 1, kArpMaxLaneSteps);
      break;
    default: {  // AR.3: the per-row pulse params
      const auto i = static_cast<uint32_t>(id);
      auto row = [&](ArpParamId first) { return static_cast<size_t>(i - static_cast<uint32_t>(first)); };
      if (i >= static_cast<uint32_t>(ArpParamId::PulseDiv1) && i <= static_cast<uint32_t>(ArpParamId::PulseDivEnd)) {
        p.pulseDiv[row(ArpParamId::PulseDiv1)] = clampi(v, 1, kArpMaxPulseDiv);
      } else if (i >= static_cast<uint32_t>(ArpParamId::PulseShift1) && i <= static_cast<uint32_t>(ArpParamId::PulseShiftEnd)) {
        p.pulseShift[row(ArpParamId::PulseShift1)] = v;
      } else if (i >= static_cast<uint32_t>(ArpParamId::PulseMaskLo1) && i <= static_cast<uint32_t>(ArpParamId::PulseMaskLoEnd)) {
        uint32_t& m = p.pulseMask[row(ArpParamId::PulseMaskLo1)];
        m = (m & 0xFFFF0000u) | static_cast<uint32_t>(clampi(v, 0, 65535));
      } else if (i >= static_cast<uint32_t>(ArpParamId::PulseMaskHi1) && i <= static_cast<uint32_t>(ArpParamId::PulseMaskHiEnd)) {
        uint32_t& m = p.pulseMask[row(ArpParamId::PulseMaskHi1)];
        m = (m & 0x0000FFFFu) | (static_cast<uint32_t>(clampi(v, 0, 65535)) << 16);
      }
      break;
    }
  }
}

ArpLaneSteps::ArpLaneSteps() noexcept {  // every step at its lane's default
  for (uint32_t l = 0; l < kArpLaneCount; ++l) value[l].fill(kArpLaneRange[l].def);
}

bool setArpLaneStep(ArpLaneSteps& s, ArpParamId id, float v) noexcept {  // RT-safe
  if (!isArpLaneStep(id)) return false;
  const uint32_t i = static_cast<uint32_t>(id) - static_cast<uint32_t>(ArpParamId::LaneStep0);
  s.value[i / kArpMaxLaneSteps][i % kArpMaxLaneSteps] = v;
  return true;
}

namespace {
// The table as the core plays it: empty → 12-EDO, a bad equave → 1200 ¢, a bad reference → unknown (0).
ArpDegreeTable normalizedTable(ArpDegreeTable table) {
  if (table.cents.empty()) {
    table.cents.resize(12);
    for (int i = 0; i < 12; ++i) table.cents[static_cast<size_t>(i)] = 100.0 * i;
    table.equaveCents = 1200.0;
  }
  if (!(table.equaveCents > 0.0) || !std::isfinite(table.equaveCents)) table.equaveCents = 1200.0;
  for (double& c : table.cents)
    if (!std::isfinite(c)) c = 0.0;
  if (!(table.referenceHz > 0.0) || !std::isfinite(table.referenceHz)) table.referenceHz = 0.0;
  return table;
}
ArpTables normalizedTables(ArpTables t) {
  t.track = normalizedTable(std::move(t.track));
  for (ArpDegreeTable& n : t.nodes) n = normalizedTable(std::move(n));
  return t;
}
}  // namespace

// ---- ArpTableMailbox (AP.2b, AP.8a) ---------------------------------------------------------------------------------

ArpTableMailbox::~ArpTableMailbox() {
  delete pending_.load();
  for (auto& r : retired_) delete r.load();
  delete prev_;
  delete current_;
}

void ArpTableMailbox::collect() {  // non-RT: the audio thread no longer reads a retired one
  for (auto& r : retired_) delete r.exchange(nullptr, std::memory_order_acq_rel);
}

void ArpTableMailbox::post(ArpTables tables) {  // non-RT
  auto* t = new ArpTables(normalizedTables(std::move(tables)));
  collect();
  delete pending_.exchange(t, std::memory_order_acq_rel);  // never taken: nobody read it
}

const ArpTables* ArpTableMailbox::take() noexcept {  // RT-safe
  if (pending_.load(std::memory_order_acquire) == nullptr) return nullptr;
  if (prev_ != nullptr) {  // the one replaced at the last take: not read since that block; into a free slot
    for (auto& r : retired_)
      if (r.load(std::memory_order_acquire) == nullptr) {
        r.store(prev_, std::memory_order_release);
        prev_ = nullptr;
        break;
      }
    if (prev_ != nullptr) return nullptr;  // every slot waits for collect(): next block
  }
  ArpTables* t = pending_.exchange(nullptr, std::memory_order_acq_rel);
  if (t == nullptr) return nullptr;
  prev_ = current_;  // still read in this block (the re-resolution)
  current_ = t;
  return t;
}

ArpParams sanitized(const ArpParams& in) noexcept {  // RT-safe
  ArpParams p = in;
  if (static_cast<uint32_t>(p.style) >= static_cast<uint32_t>(ArpStyle::Count)) p.style = ArpStyle::Up;
  p.rate = sanitizedRate(p.rate);
  p.retriggerRate = sanitizedRate(p.retriggerRate);
  p.retriggerRate.synced = true;
  auto unit = [](float v, float lo, float hi, float def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; };
  p.gate = unit(p.gate, 0.01f, 2.0f, 0.5f);
  p.groove = unit(p.groove, 0.0f, 1.0f, 0.0f);
  p.repeats = std::max(p.repeats, 0);
  if (static_cast<uint32_t>(p.retrigger) > 2) p.retrigger = ArpRetrigger::Off;
  if (static_cast<uint32_t>(p.unit) > 2) p.unit = ArpUnit::Degrees;
  if (static_cast<uint32_t>(p.transpose) > 1) p.transpose = ArpTranspose::Shift;
  if (static_cast<uint32_t>(p.velocitySource) > 2) p.velocitySource = ArpVelocitySource::AsPlayed;
  p.distance = std::clamp(p.distance, -64, 64);
  p.steps = std::clamp(p.steps, 1, kArpMaxSteps);
  p.ratioP = std::clamp(p.ratioP, 1, 64);
  p.ratioQ = std::clamp(p.ratioQ, 1, 64);
  p.fixedVelocity = unit(p.fixedVelocity, 0.0f, 1.0f, 0.8f);
  p.velocityDecayMs = unit(p.velocityDecayMs, 0.0f, 60000.0f, 0.0f);
  p.velocityTarget = unit(p.velocityTarget, 0.0f, 1.0f, 0.0f);
  p.harmonicBase = std::clamp(p.harmonicBase, 1, 64);
  p.walkRange = std::clamp(p.walkRange, 0, kArpMaxWalkRange);
  if (static_cast<uint32_t>(p.walkOrder) > 3) p.walkOrder = ArpWalkOrder::Up;
  for (ArpGlideSide* g : {&p.into, &p.out}) {  // AP.10: each side its own
    g->offset = unit(g->offset, -kArpMaxGlideOffset, kArpMaxGlideOffset, 0.0f);
    if (std::fabs(g->offset) < 0.5f) g->offset = 0.0f;  // AR.2: under half a cent is none (a knob resting near 0: off)
    g->time = unit(g->time, 0.0f, kArpMaxGlideTime, 0.25f);  // AP.11
    if (static_cast<uint32_t>(g->shape) >= static_cast<uint32_t>(ArpGlideShape::Count)) g->shape = ArpGlideShape::Curve;
    g->curve = unit(g->curve, -1.0f, 1.0f, 0.0f);
  }
  p.pulseCycleNum = std::clamp(p.pulseCycleNum, 1, kArpMaxPulseBars);
  p.pulseCycleDen = std::clamp(p.pulseCycleDen, 1, kArpMaxPulseCycleDen);
  for (int32_t& x : p.pulseDiv) x = std::clamp(x, 1, kArpMaxPulseDiv);
  for (float& x : p.pulseShift) x = unit(x, -kArpMaxPulseShift, kArpMaxPulseShift, 0.0f);
  p.layers = std::clamp(p.layers, 0, kArpMaxLayers);
  for (float& x : p.layerLevel) x = unit(x, 0.0f, 1.0f, 0.0f);
  for (int32_t& x : p.laneLength) x = std::clamp(x, 1, kArpMaxLaneSteps);
  return p;
}

// ---- State ------------------------------------------------------------------------------------------------------

struct ArpCore::State {
  struct Held {
    NoteEvent on;           // the source note-on (its identity, base Hz, velocity, gain)
    uint64_t order = 0;     // arrival
    bool key = true;        // a key holds it (false: latched by Hold)
    double c0 = 0.0, c1 = 0.0;  // bend in cents: c0 → c1 over [cT, cT + cLen)
    int64_t cT = 0;
    uint32_t cLen = 0;
    float g0 = 1.0f, g1 = 1.0f;  // gain: g0 → g1 over [gT, gT + gLen)
    int64_t gT = 0;
    uint32_t gLen = 0;
    // AP.4: its pulse clock. The next pulse is (pc, pj): cycle, index in the cycle; pNext its onset (Song: song
    // sample; Free: engine time), pLast the last onset played. AR.3: ph its division n, and its row's mask / shift /
    // rank by Hz (set by pulseRefresh).
    int64_t ph = 1;
    uint32_t pMask = 0xFFFFFFFFu;
    double pShift = 0.0;
    uint32_t pRank = 0;
    int64_t pc = 0, pj = 0;
    int64_t pNext = kNever, pLast = 0;
    bool pHasLast = false, pImmediate = false;
    bool pFresh = false;   // added at the pending refresh's frame (its first pulse may sound at the note-on)
    float pPrevHz = 0.0f;  // its last pulse's own Hz (bend included; Into → Step)
    double centsAt(int64_t t) const noexcept {
      if (cLen == 0 || t >= cT + cLen) return c1;
      return c0 + (c1 - c0) * static_cast<double>(t - cT) / cLen;
    }
    float gainAt(int64_t t) const noexcept {
      if (gLen == 0 || t >= gT + gLen) return g1;
      return g0 + (g1 - g0) * static_cast<float>(static_cast<double>(t - gT) / gLen);
    }
  };
  // What a generated note plays: held note `slot` (walked m harmonics over / under it: Harmonic Walk), shifted by
  // `shift` in the unit and `wraps` equaves (Rotate).
  struct Elem {
    uint32_t slot = 0;
    int32_t shift = 0, wraps = 0;
    uint16_t walk = 0;
    bool under = false;
    uint8_t layer = 0;  // AP.4: × (p/q)^layer
  };
  struct Gen {
    uint32_t id, src;
    Elem el;
    float hz;        // its pitch without the source's bend
    int64_t off;     // engine time of its note-off
    double c;        // the source bend (cents) it was last aimed at
    int64_t intoEnd;  // AP.3: the end of its Into glide (kNever: none)
    int64_t outAt;    // AP.3: the start of its Out glide (kNever: none / started)
    bool outing;      // AP.3: inside its Out glide
    uint16_t rank;    // its rank in its step (Chord Trigger)
    int64_t stepK;    // its step's grid index (Pulse: its pulse's index in the cycle)
    float level;      // AP.4: its layer's level (1: layer 0)
    // AP.5: waiting for a tie (the next step continues it; its end is that step's onset + 1 until then), the key
    // of the step that set it, its Glide lane factor, its pulse's cycle.
    bool tieWait = false;
    int64_t tieKey = 0;
    float glideAmt = 1.0f;
    int64_t stepC = 0;
    // AP.11: its glide curves (arp.h "Glides"), additive offsets in cents from its own pitch: Into inC · (1 − s(u)) over
    // [inT, inT + inLen) (inC 0: none; inLen may pass its end), Out outC · s(v) over [outT, off) (outT may precede its
    // onset; outC is set when the Out starts: `outing`); segAt: the next linear piece of a curve no one ramp carries.
    int64_t inT = 0, inLen = 0;
    double inC = 0.0;
    int64_t outT = 0, outLen = 1;
    double outC = 0.0;
    int64_t segAt = kNever;
  };
  // AP.5: one step's lane values (lanes switched off: their defaults) and how it relates to the next step.
  struct StepCtx {
    float vel = 1.0f, gate = 0.5f, glide = 1.0f;
    int32_t trans = 0;
    ArpStepKind kind = ArpStepKind::Play;
    bool holdNext = false;  // the next step is a tie: hold the notes past its onset
    int64_t nextOn = 0;     // that onset (engine time)
    int64_t key = 0;        // the step's lane position key (pattern: kk; pulse: the note's pulse count)
    int64_t cyc = 0;        // pulse: its cycle
  };
  struct WalkEl {
    double hz;
    uint64_t order;
    uint8_t slot, m;
  };
  enum class Clock : uint8_t { None, Song, Free };

  explicit State(ArpTables t) : tables(normalizedTables(std::move(t))) {
    for (auto& l : laneSlot) l.fill(kNone);
  }

  ArpTables tables;
  const ArpTables* tabs = &tables;  // AP.2b: the tables played (own, or the mailbox's latest)
  std::shared_ptr<ArpTableMailbox> box;
  bool retable = false;  // AP.2b: a new table: the sounding notes re-derive their pitch at the next block's frame 0
  // AP.5: step lanes: fixed values, per step a param slot (kNone: the fixed value), per lane the slot's line.
  ArpLaneSteps laneFixed;
  std::array<std::array<uint32_t, kArpMaxLaneSteps>, kArpLaneCount> laneSlot{};
  std::array<float, kArpLaneCount> laneScale{1, 1, 1, 1, 1, 1}, laneOffset{};
  // AP.8b (review M8): cables into a lane step: per step [begin, begin + count) of laneMods, the bus values added to
  // the step's value (in the lane's units) when it plays — read only then, like the step's slot.
  std::array<std::array<uint16_t, kArpMaxLaneSteps>, kArpLaneCount> laneModBegin{}, laneModCount{};
  std::vector<uint32_t> laneMods;
  std::array<Held, kArpMaxHeld> held{};
  uint32_t numHeld = 0, keysDown = 0;
  uint64_t nextOrder = 0;
  std::array<uint8_t, kArpMaxHeld> sorted{}, arrival{};  // held slots by Hz / by arrival
  std::array<uint8_t, kArpMaxPattern> pattern{};
  uint32_t patternLen = 0;
  std::array<uint8_t, kArpMaxHeld> perm{};
  bool permValid = false;
  bool dirty = true;
  ArpStyle builtStyle = ArpStyle::Count;

  std::array<Gen, kArpMaxSounding> gens{};
  uint32_t numGens = 0;
  int64_t minOff = kNever, minAct = kNever;
  uint32_t nextId = 0;
  // AP.8a (review L4): at a 2^24 wrap of nextId, the live ids' counters (sorted) the next pass skips.
  std::array<uint32_t, kArpMaxSounding> skipIds{};
  uint32_t numSkip = 0, skipAt = 0;
  // AP.8a (review M4): scratch of the one-pass room making; a tie step's lookup (rank · 4 + layer → gen index + 1).
  std::array<uint16_t, kArpMaxSounding> roomIdx{};
  std::array<uint16_t, kArpMaxHeld * (kArpMaxLayers + 1)> tieIdx{};
  bool tieIdxValid = false;
  // Harmonic Walk (AP.3): the merged elements, by Hz.
  std::array<WalkEl, kArpMaxWalk> walk{};
  uint32_t numWalk = 0;
  int32_t builtBase = 0, builtRange = -1;
  bool builtUnder = false;
  std::array<uint8_t, kArpMaxHeld> scratchPerm{};
  // Glides (AP.3): the latest step's notes (Hz at their onset, bend included) by rank, and the one being played.
  std::array<float, kArpMaxHeld> prevHz{}, curHz{};
  uint32_t prevCount = 0, curCount = 0;
  int64_t prevStepK = 0;
  bool immediate = false;  // the next Song step sounds at the note-on (nextSong), off the grid (AP.3)

  // Clock.
  Clock clock = Clock::None;
  int64_t k = 0;            // the next step's grid index
  SampleTime nextSong = 0;  // Song: its onset (song sample)
  double t0 = 0.0;          // Free: engine time of step 0 (without groove)
  double period = 0.0;      // Free: samples per step
  int64_t nextTime = 0;     // Free: its onset (engine time)
  int64_t lastBeat = 0;
  int64_t pos = 0;          // steps since the pattern start
  int64_t decayStart = 0;
  // Transport tracking.
  bool prevValid = false, prevSong = false;
  SampleTime expectPos = 0;
  double lastBpm = 0.0;  // AP.8a (M3): the last block's tempo (a remap's ratio)
  // Grid cache.
  ArpRate gridRate{}, gridRetrig{};
  bool gridValid = false;
  int64_t D = 1, Lsub = 1, Rsub = 1;  // the step and retrigger lengths in 1/D ticks
  bool hadHold = false;
  // Pulse mode (AP.4): the geometry every clock shares (rebuilt by pulseRefresh), the earliest pending onset.
  bool wasPulse = false, pulseStale = true;
  int64_t pD = 1, pCycSub = 1;     // Song: the Cycle in 1/pD ticks (pD = its den; pVar: cycles follow the bars)
  bool pVar = false;               // AP.2b: Song, a signature map with changes: cycle c starts at bar c · num / den
  const TimeSigMap* sigSeen = nullptr;
  double pCyc = 0.0;               // Free: the Cycle in samples
  double pBpm = 0.0, pBpb = 0.0;
  int64_t minPulse = kNever;
  bool pulsePending = false;  // held-set changes at frame pulsePendingFrame: one refresh after that frame's events
  uint32_t pulsePendingFrame = 0;

  // AP.6: the panel status (null: none) and what its pattern list was last written from.
  std::shared_ptr<NoteStageStatus> status;
  bool patternNew = true, statusIdle = false;
  ArpStyle statusStyle = ArpStyle::Count;
  ArpWalkOrder statusOrder = ArpWalkOrder::Up;
  bool statusPulse = false;

  std::vector<ArpTraceNote>* trace = nullptr;  // AP.7: offline only (setTrace)

  ArpParams p;
  ArpParams raw;  // the unsanitized params p came from
  bool rawValid = false;
  const NoteStageBlock* b = nullptr;
  EventBuffer* out = nullptr;
  ArpStats stats;
  uint32_t stageIndex = 0;

  // ---- Held set ----
  uint32_t find(uint32_t id) const noexcept {
    for (uint32_t i = 0; i < numHeld; ++i)
      if (held[i].on.noteId == id) return i;
    return kNone;
  }
  void remove(uint32_t i) noexcept {
    if (held[i].key) --keysDown;
    held[i] = held[--numHeld];
    dirty = true;
    permValid = false;
    pulseStale = true;
    if (numHeld == 0) clock = Clock::None;
  }
  void dropLatched() noexcept {
    for (uint32_t i = 0; i < numHeld;)
      if (!held[i].key) remove(i);
      else ++i;
  }

  void rebuild() noexcept {
    dirty = false;
    patternNew = true;
    const uint32_t n = numHeld;
    for (uint32_t i = 0; i < n; ++i) sorted[i] = arrival[i] = static_cast<uint8_t>(i);
    std::sort(sorted.begin(), sorted.begin() + n, [&](uint8_t a, uint8_t b) {
      const float ha = held[a].on.hz, hb = held[b].on.hz;
      return ha < hb || (ha == hb && held[a].order < held[b].order);
    });
    std::sort(arrival.begin(), arrival.begin() + n, [&](uint8_t a, uint8_t b) { return held[a].order < held[b].order; });
    buildPattern(n);
    builtBase = p.harmonicBase;
    builtRange = p.walkRange;
    builtUnder = p.walkUnder;
    numWalk = 0;
    if (p.style == ArpStyle::HarmonicWalk) buildWalk(n);
  }
  bool stale() const noexcept {
    return dirty || builtStyle != p.style ||
           (p.style == ArpStyle::HarmonicWalk &&
            (builtBase != p.harmonicBase || builtRange != p.walkRange || builtUnder != p.walkUnder));
  }

  // Harmonic Walk: every held note's walk (h … h + Range over, or the undertones), merged by Hz, duplicates once.
  void buildWalk(uint32_t n) noexcept {
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const Held& h = held[sorted[i]];
      const double hh = static_cast<double>(harmonicOf(h.on));
      for (int32_t m = 0; m <= p.walkRange; ++m) {
        if (w == kArpMaxWalk) {
          ++stats.droppedWalk;
          continue;
        }
        const double r = p.walkUnder ? hh / (hh + m) : (hh + m) / hh;
        walk[w++] = WalkEl{static_cast<double>(h.on.hz) * r, h.order, sorted[i], static_cast<uint8_t>(m)};
      }
    }
    std::sort(walk.begin(), walk.begin() + w, [](const WalkEl& a, const WalkEl& b) {
      return a.hz < b.hz || (a.hz == b.hz && (a.order < b.order || (a.order == b.order && a.m < b.m)));
    });
    uint32_t kept = 0;
    for (uint32_t i = 0; i < w; ++i) {  // within 0.01 ¢ of the last kept one: a duplicate
      if (kept > 0 && walk[i].hz <= walk[kept - 1].hz * 1.000005776243187) continue;
      walk[kept++] = walk[i];
    }
    numWalk = kept;
  }

  void buildPattern(uint32_t n) noexcept {
    builtStyle = p.style;
    uint32_t L = 0;
    auto put = [&](int i) { pattern[L++] = static_cast<uint8_t>(i); };
    const int N = static_cast<int>(n);
    auto upDown = [&](int lo, int hi, bool pinkyTop, bool thumbLow) {  // lo … hi, then hi − 1 … lo + 1
      auto one = [&](int i) {
        if (thumbLow) put(0);
        put(i);
        if (pinkyTop) put(N - 1);
      };
      for (int i = lo; i <= hi; ++i) one(i);
      for (int i = hi - 1; i > lo; --i) one(i);
    };
    auto converge = [&](uint8_t* dst) {
      uint32_t m = 0;
      for (int lo = 0, hi = N - 1; lo <= hi; ++lo, --hi) {
        dst[m++] = static_cast<uint8_t>(lo);
        if (hi != lo) dst[m++] = static_cast<uint8_t>(hi);
      }
    };
    switch (p.style) {
      case ArpStyle::Up:
      case ArpStyle::PlayOrder:
        for (int i = 0; i < N; ++i) put(i);
        break;
      case ArpStyle::Down:
        for (int i = N - 1; i >= 0; --i) put(i);
        break;
      case ArpStyle::UpDown: upDown(0, N - 1, false, false); break;
      case ArpStyle::DownUp:
        for (int i = N - 1; i >= 0; --i) put(i);
        for (int i = 1; i < N - 1; ++i) put(i);
        break;
      case ArpStyle::UpAndDown:
        for (int i = 0; i < N; ++i) put(i);
        for (int i = N - 1; i >= 0; --i) put(i);
        break;
      case ArpStyle::DownAndUp:
        for (int i = N - 1; i >= 0; --i) put(i);
        for (int i = 0; i < N; ++i) put(i);
        break;
      case ArpStyle::Converge:
        converge(pattern.data());
        L = n;
        break;
      case ArpStyle::Diverge:
        converge(pattern.data());
        std::reverse(pattern.begin(), pattern.begin() + n);
        L = n;
        break;
      case ArpStyle::ConAndDiverge:
        converge(pattern.data());
        L = n;
        for (int i = N - 2; i >= 1; --i) pattern[L++] = pattern[static_cast<uint32_t>(i)];  // diverge, ends once
        break;
      case ArpStyle::PinkyUp:
        if (N == 1) put(0);
        for (int i = 0; i < N - 1; ++i) {
          put(i);
          put(N - 1);
        }
        break;
      case ArpStyle::PinkyUpDown:
        if (N == 1) put(0);
        else upDown(0, N - 2, true, false);
        break;
      case ArpStyle::ThumbUp:
        if (N == 1) put(0);
        for (int i = 1; i < N; ++i) {
          put(0);
          put(i);
        }
        break;
      case ArpStyle::ThumbUpDown:
        if (N == 1) put(0);
        else upDown(1, N - 1, false, true);
        break;
      default:  // Chord Trigger, Random*: no table
        L = n;
        for (int i = 0; i < N; ++i) pattern[static_cast<uint32_t>(i)] = static_cast<uint8_t>(i);
        break;
    }
    patternLen = std::max(L, 1u);
  }

  void shuffleInto(uint8_t* dst, int64_t step) const noexcept {
    const uint32_t n = numHeld;
    for (uint32_t i = 0; i < n; ++i) dst[i] = static_cast<uint8_t>(i);
    for (uint32_t i = n; i-- > 1;) std::swap(dst[i], dst[draw(p.seed, step, i) % (i + 1)]);
  }
  void shuffle(int64_t step) noexcept {
    shuffleInto(perm.data(), step);
    permValid = true;
  }

  // ---- Pitch units ----
  // AP.8a (H4): the table of a note's tuning (its clip's).
  const ArpDegreeTable& tabOf(const NoteEvent& n) const noexcept { return tabs->of(n.tuning); }
  static int32_t sizeOf(const ArpDegreeTable& t) noexcept { return static_cast<int32_t>(t.cents.size()); }
  static double centsOf(const ArpDegreeTable& t, int64_t deg) noexcept {
    const int64_t N = sizeOf(t);
    const int64_t q = floorDiv(deg, N);
    return static_cast<double>(q) * t.equaveCents + t.cents[static_cast<size_t>(deg - q * N)];
  }
  // A float harmonic number only while it is an exact integer (review L3: at most 2^24), else 0 (none).
  static float harmonicNumber(int64_t h) noexcept {
    return h >= 1 && h <= (int64_t{1} << 24) ? static_cast<float>(h) : 0.0f;
  }
  // Harmonics: the note's harmonic number (its JI group's, an integer ≥ 1), else Base.
  int64_t harmonicOf(const NoteEvent& src) const noexcept {
    const float h = src.harmonic;
    if (h >= 1.0f && h < 1e6f && h == std::floor(h)) return static_cast<int64_t>(h);
    return p.harmonicBase;
  }
  // `src` moved by the exact fraction num / den (Harmonics, Harmonic Walk, Layers): Hz, ratio, harmonic h · num / den
  // when an integer (else 0), the source's degree identity and tuning kept (AP.8a H1: the tuning node moves it with
  // the source's degree, at the ratio), noteDegree moved fractionally.
  void scaled(const NoteEvent& src, int64_t num, int64_t den, int64_t h, NoteEvent& e) const noexcept {
    const int64_t g = std::gcd(num, den);
    num /= g;
    den /= g;
    const double r = static_cast<double>(num) / static_cast<double>(den);
    e.hz = static_cast<float>(static_cast<double>(src.hz) * r);
    e.ratio = static_cast<float>(static_cast<double>(src.ratio) * r);
    e.harmonic = (h % den == 0 && h / den <= (int64_t{1} << 24) / num) ? harmonicNumber(h / den * num) : 0.0f;
    e.degree = src.degree;
    e.equave = src.equave;
    e.tuning = src.tuning;
    const ArpDegreeTable& t = tabOf(src);
    e.noteDegree = std::isfinite(src.noteDegree)
                       ? src.noteDegree + static_cast<float>(1200.0 * std::log2(r) * sizeOf(t) / t.equaveCents)
                       : kNaN;
  }
  // Element `el` of held note `src` (its walk, then its shift / wraps in the unit): fills e's hz and identity.
  // Returns the Hz (0: a rest).
  float pitchOf(const NoteEvent& src, const Elem& el, int32_t extra, NoteEvent& e) const noexcept {
    const float hz = unlayered(src, el, extra, e);
    e.key = stepKey(src, el.walk == 0 && el.layer == 0 && el.shift + extra == 0 && el.wraps == 0, e);
    if (el.layer == 0 || !(hz > 0.0f)) return hz;
    // AP.4: layer L at (p/q)^L of the note, exact (scaled: ratio, harmonic h · P^L / Q^L when an integer).
    int64_t num = 1, den = 1;
    for (uint8_t i = 0; i < el.layer; ++i) {
      num *= p.ratioP;
      den *= p.ratioQ;
    }
    const NoteEvent base = e;
    const float h = base.harmonic;
    const int64_t hi = (h >= 1.0f && h < 1e6f && h == std::floor(h)) ? static_cast<int64_t>(h) : 0;
    scaled(base, num, den, hi, e);
    e.key = stepKey(src, false, e);
    return e.hz;
  }
  // SM.1: a step's voice.key (doc::midiKeyOf's rule on the new pitch): a copy keeps the source's key; a Degree identity
  // moves it by the degrees moved (from the source's key: a .kbm's keys stay its own; unknown: 60 + noteDegree);
  // anything else (Ratio, Harmonics, a static shift) takes the nearest 12-TET key of the new Hz.
  static int16_t stepKey(const NoteEvent& src, bool same, const NoteEvent& e) noexcept {  // RT-safe
    if (same) return src.key;
    if (std::isfinite(e.degree) && std::isfinite(e.noteDegree) && std::fabs(e.noteDegree) < 1e6f) {
      if (src.key >= 0 && std::isfinite(src.noteDegree) && std::fabs(src.noteDegree) < 1e6f)
        return tuning::midiKeyOrNone(src.key + std::llround(e.noteDegree) - std::llround(src.noteDegree));
      return tuning::midiKeyOrNone(60 + std::llround(e.noteDegree));
    }
    return tuning::nearestMidiKeyOrNone(e.hz);
  }
  double layerRatio(uint8_t layer) const noexcept {
    int64_t num = 1, den = 1;
    for (uint8_t i = 0; i < layer; ++i) {
      num *= p.ratioP;
      den *= p.ratioQ;
    }
    return static_cast<double>(num) / static_cast<double>(den);
  }
  float unlayered(const NoteEvent& src, const Elem& el, int32_t extra, NoteEvent& e) const noexcept {
    if (el.walk == 0) return pitch(src, el.shift + extra, el.wraps, e);
    NoteEvent w = src;
    const int64_t h = harmonicOf(src), m = el.walk;
    if (el.under) {
      scaled(src, h, h + m, h, w);
      w.harmonic = 0.0f;  // an undertone is not a harmonic of the anchor
    } else {
      scaled(src, h + m, h, h, w);
    }
    return pitch(w, el.shift + extra, el.wraps, e);
  }
  // The pitch of `src` shifted by `shift` in the unit and `wraps` equaves (Rotate): fills e's hz and identity.
  // Returns the Hz (0: a rest, Harmonics below the first).
  float pitch(const NoteEvent& src, int32_t shift, int32_t wraps, NoteEvent& e) const noexcept {
    e.hz = src.hz;
    e.ratio = src.ratio;
    e.harmonic = src.harmonic;
    e.degree = src.degree;
    e.equave = src.equave;
    e.tuning = src.tuning;
    e.noteDegree = src.noteDegree;
    e.unisonCount = src.unisonCount;
    e.unisonIndex = src.unisonIndex;
    if (shift == 0 && wraps == 0) return e.hz;
    const ArpDegreeTable& tab = tabOf(src);
    const int64_t N = sizeOf(tab);
    if (p.unit == ArpUnit::Harmonics) {
      const int64_t h = harmonicOf(src), h1 = h + shift;
      if (h1 < 1) return e.hz = 0.0f;
      const int32_t w = std::clamp(wraps, -24, 24);
      const int64_t num = w >= 0 ? h1 << w : h1, den = w >= 0 ? h : h << -w;
      scaled(src, num, den, h, e);
      return e.hz;
    }
    if (p.unit == ArpUnit::Degrees) {
      shift += static_cast<int32_t>(wraps * N);
      const bool deg = std::isfinite(src.degree) && std::isfinite(src.equave);
      const int64_t d0 = deg ? static_cast<int64_t>(std::llround(src.equave)) * N + std::llround(src.degree)
                             : (std::isfinite(src.noteDegree) ? std::llround(src.noteDegree) : 0);
      const int64_t d1 = d0 + shift;
      const double r = std::exp2((centsOf(tab, d1) - centsOf(tab, d0)) / 1200.0);
      e.hz = static_cast<float>(static_cast<double>(src.hz) * r);
      e.ratio = static_cast<float>(static_cast<double>(src.ratio) * r);
      e.harmonic = 0.0f;
      if (deg) {
        const int64_t q = floorDiv(d1, N);
        e.equave = static_cast<float>(q);
        e.degree = static_cast<float>(d1 - q * N);
      }  // else: no degree; its tuning kept (a reference-relative note still follows the reference)
      if (std::isfinite(src.noteDegree)) e.noteDegree = src.noteDegree + static_cast<float>(shift);
      return e.hz;
    }
    // Ratio: (p/q)^shift, exact while it fits.
    shift += wraps;
    if (shift == 0) return e.hz;
    const int64_t a = shift > 0 ? p.ratioP : p.ratioQ, c = shift > 0 ? p.ratioQ : p.ratioP;
    int64_t num = 1, den = 1;
    bool exact = true;
    for (int32_t i = 0; i < std::abs(shift); ++i) {
      if (num > (int64_t{1} << 46) / a || den > (int64_t{1} << 46) / c) {
        exact = false;
        break;
      }
      num *= a;
      den *= c;
    }
    const double r = exact ? static_cast<double>(num) / static_cast<double>(den)
                           : std::pow(static_cast<double>(a) / static_cast<double>(c), std::abs(shift));
    e.hz = static_cast<float>(static_cast<double>(src.hz) * r);
    e.ratio = static_cast<float>(static_cast<double>(src.ratio) * r);
    e.harmonic = 0.0f;
    if (exact && src.harmonic > 0.0f && src.harmonic == std::floor(src.harmonic) && src.harmonic < 1e6f) {
      const auto h = static_cast<int64_t>(src.harmonic);
      if (h <= (int64_t{1} << 46) / num && (h * num) % den == 0) e.harmonic = harmonicNumber(h * num / den);
    }
    // AP.8a (H1): the source's degree / equave / tuning stay (copied above): the node moves it at its ratio.
    e.noteDegree = std::isfinite(src.noteDegree)
                       ? src.noteDegree + static_cast<float>(1200.0 * std::log2(r) * N / tab.equaveCents)
                       : kNaN;
    return e.hz;
  }

  // AP.7: what element `el` of held note `src` plays relative to it, exactly (ArpExact; the rules of pitchOf /
  // unlayered / pitch above, as integers). Offline only (the trace).
  ArpExact exactOf(const NoteEvent& src, const Elem& el) const noexcept {
    ArpExact x;
    int64_t num = 1, den = 1;
    bool exact = true;
    auto mul = [&](int64_t a, int64_t c) {  // × a / c, reduced; inexact when it no longer fits
      if (!exact) return;
      const int64_t g1 = std::gcd(a, den), g2 = std::gcd(c, num);
      a /= g1;
      const int64_t d1 = den / g1, c1 = c / g2, n1 = num / g2;
      if (a > (int64_t{1} << 46) / std::max<int64_t>(1, n1) || c1 > (int64_t{1} << 46) / std::max<int64_t>(1, d1)) {
        exact = false;
        return;
      }
      num = n1 * a;
      den = d1 * c1;
    };
    NoteEvent w = src;  // the walked note: its harmonic (what harmonicOf reads next)
    bool walked = false;
    if (el.walk != 0) {
      const int64_t h = harmonicOf(src), m = el.walk;
      if (el.under) {
        mul(h, h + m);
        w.harmonic = 0.0f;
      } else {
        mul(h + m, h);
        w.harmonic = static_cast<float>(h + m);
      }
      walked = true;
    }
    int64_t degrees = 0;
    if (el.shift != 0 || el.wraps != 0) {
      if (p.unit == ArpUnit::Harmonics) {
        const int64_t h = harmonicOf(w), h1 = h + el.shift;
        const int32_t wr = std::clamp(el.wraps, -24, 24);
        if (h1 >= 1) mul(wr >= 0 ? h1 << wr : h1, wr >= 0 ? h : h << -wr);
      } else if (p.unit == ArpUnit::Degrees) {
        degrees = el.shift + static_cast<int64_t>(el.wraps) * sizeOf(tabOf(src));
        if (walked) exact = false;  // a walked note has no degree: shifted from round(noteDegree)
      } else {
        const int32_t sh = el.shift + el.wraps;
        const int64_t a = sh > 0 ? p.ratioP : p.ratioQ, c = sh > 0 ? p.ratioQ : p.ratioP;
        for (int32_t i = 0; i < std::abs(sh) && exact; ++i) mul(a, c);
      }
    }
    for (uint8_t i = 0; i < el.layer && exact; ++i) mul(p.ratioP, p.ratioQ);
    if (!exact || (degrees != 0 && (num != 1 || den != 1))) {
      x.kind = ArpExact::Kind::Inexact;
    } else if (degrees != 0) {
      x.kind = ArpExact::Kind::Degrees;
      x.degrees = degrees;
    } else if (num != den) {
      x.kind = ArpExact::Kind::Ratio;
      x.num = num;
      x.den = den;
    }
    return x;
  }

  // ---- Generated notes ----
  void recomputeMinOff() noexcept {
    minOff = minAct = kNever;
    for (uint32_t i = 0; i < numGens; ++i) {
      minOff = std::min(minOff, gens[i].off);
      minAct = std::min({minAct, gens[i].intoEnd, gens[i].outAt, gens[i].segAt});
    }
  }
  void endDue(int64_t t, uint32_t frame) noexcept {
    for (uint32_t i = 0; i < numGens;) {
      if (gens[i].off <= t) {
        out->push(noteOff(frame, gens[i].id));
        gens[i] = gens[--numGens];
      } else {
        ++i;
      }
    }
    recomputeMinOff();
  }
  void endAll(uint32_t frame, uint8_t flags = 0) noexcept {
    for (uint32_t i = 0; i < numGens; ++i) {
      NoteEvent o = noteOff(frame, gens[i].id);
      o.flags = flags;
      out->push(o);
    }
    numGens = 0;
    minOff = minAct = kNever;
    prevCount = curCount = 0;
  }

  // AP.8a (review M4): room for `need` new notes when the sounding table is full: the earliest-ending ones (waiting
  // ties kept when `keepTies`: the step continues them) end at `frame`, chosen in ONE partial selection — a step or a
  // frame of pulses costs O(sounding) however many notes it starts, never O(sounding) per note.
  void makeRoom(uint32_t need, uint32_t frame, bool keepTies) noexcept {
    if (numGens + need <= kArpMaxSounding) return;
    uint32_t n = 0;
    for (uint32_t i = 0; i < numGens; ++i)
      if (!(keepTies && gens[i].tieWait)) roomIdx[n++] = static_cast<uint16_t>(i);
    const uint32_t cut = std::min(numGens + need - kArpMaxSounding, n);
    if (cut == 0) return;
    if (cut < n)
      std::nth_element(roomIdx.begin(), roomIdx.begin() + cut, roomIdx.begin() + n,
                       [&](uint16_t a, uint16_t c) { return gens[a].off < gens[c].off; });
    for (uint32_t i = 0; i < cut; ++i) {
      Gen& g = gens[roomIdx[i]];
      out->push(noteOff(frame, g.id));
      g.id = kNone;  // (removed below)
    }
    uint32_t w = 0;
    for (uint32_t i = 0; i < numGens; ++i)
      if (gens[i].id != kNone) gens[w++] = gens[i];
    numGens = w;
    stats.cutSounding += cut;
    tieIdxValid = false;
    recomputeMinOff();
  }
  // AP.8a (review L4): a generated id (stageNoteId) no sounding note has: the 24-bit counter skips, after each wrap,
  // the counters still live then (a long Tie chain can outlive 2^24 notes). O(1) per note; a wrap sorts ≤ 512 ids.
  uint32_t newId() noexcept {
    for (;;) {
      const uint32_t n = nextId++ & 0x00FFFFFFu;
      if (n == 0) {
        numSkip = 0;
        for (uint32_t i = 0; i < numGens; ++i) skipIds[numSkip++] = gens[i].id & 0x00FFFFFFu;
        std::sort(skipIds.begin(), skipIds.begin() + numSkip);
        skipAt = 0;
      }
      while (skipAt < numSkip && skipIds[skipAt] < n) ++skipAt;
      if (skipAt < numSkip && skipIds[skipAt] == n) continue;
      return stageNoteId(stageIndex, n);
    }
  }
  // AP.8a (review M4): the waiting notes (tieWait) by (key, layer) — pattern: key = rank; pulse: the held slot of its
  // source — in one pass, so a tie step's notes find theirs in O(1). Valid until gens move (makeRoom) or the step /
  // frame ends.
  void buildTies() noexcept {
    std::fill(tieIdx.begin(), tieIdx.end(), uint16_t{0});
    for (uint32_t i = 0; i < numGens; ++i) {
      const Gen& g = gens[i];
      if (!g.tieWait || g.el.layer > kArpMaxLayers) continue;
      uint32_t key = kNone;
      if (p.pulse) key = g.el.slot < numHeld && held[g.el.slot].on.noteId == g.src ? g.el.slot : find(g.src);
      else if (g.rank < kArpMaxHeld) key = g.rank;
      if (key == kNone) continue;
      uint16_t& e = tieIdx[key * (kArpMaxLayers + 1) + g.el.layer];
      if (e == 0) e = static_cast<uint16_t>(i + 1);
    }
    tieIdxValid = true;
  }

  // ---- Glides (AP.3) ----
  // AP.10: side `g` (p.into / p.out): its shape and curve, its length for a note `len` frames long (0: none).
  static NoteEvent glide(const ArpGlideSide& g, uint32_t frame, uint32_t id, double hz, int64_t len) noexcept {
    return notePitch(frame, id, static_cast<float>(hz), static_cast<uint32_t>(len), g.ramp(), rampBendOf(g.curve));
  }
  // AR.2 (arp.h "Step lanes", Glide): the Glide lane on with no side on slides a painted step in from the previous
  // step; so the Into is on, and whether it comes from the neighbouring step.
  bool intoOn() const noexcept { return p.into.on() || (laneOn(ArpLane::Glide) && !p.out.on()); }
  bool intoStep() const noexcept { return p.into.step || p.into.offset == 0.0f; }
  // AP.11: Time · the note's length (0 … 2 of it; ≥ 1 frame when on).
  static int64_t glideFrames(const ArpGlideSide& g, int64_t len) noexcept {
    const int64_t n = std::llround(static_cast<double>(g.time) * static_cast<double>(len));
    return n <= 0 || len <= 0 ? 0 : std::min(n, 2 * len);
  }
  static bool linearSide(const ArpGlideSide& g) noexcept { return g.ramp() == RampShape::Linear; }
  static double baseHz(const Gen& g) noexcept { return static_cast<double>(g.hz) * std::exp2(g.c / 1200.0); }
  // AP.11: g's glide offset (cents from its own pitch) at engine time x: the Into and Out curves added.
  double glideCents(const Gen& g, int64_t x) const noexcept {
    double c = 0.0;
    if (g.inC != 0.0 && x < g.inT + g.inLen) {
      const double u = static_cast<double>(x - g.inT) / static_cast<double>(std::max<int64_t>(g.inLen, 1));
      c += g.inC * (1.0 - rampShapeAt(p.into.ramp(), rampBendOf(p.into.curve), u));
    }
    if (g.outing) {
      const double v = static_cast<double>(x - g.outT) / static_cast<double>(std::max<int64_t>(g.outLen, 1));
      c += g.outC * rampShapeAt(p.out.ramp(), rampBendOf(p.out.curve), v);
    }
    return c;
  }
  // AP.11: from engine time T (frame `frame`) on, the pitch g's glides give. One side running from its start (or
  // Linear): its one shaped ramp (Into: to the note's pitch, Out: to the target at the note's end); both Linear: a
  // linear piece to the Into's end (exact); else a linear-in-cents piece to the curves' value ≥ 1 ms on (32 per the
  // shorter curve), the next at g.segAt.
  void aim(Gen& g, int64_t T, uint32_t frame) noexcept {
    g.segAt = kNever;
    if (g.off <= T) return;
    const int64_t inEnd = g.inT + g.inLen;
    const bool inOn = g.inC != 0.0 && T < inEnd;
    const double base = baseHz(g);
    if (!g.outing) {
      if (inOn) out->push(glide(p.into, frame, g.id, base, inEnd - T));
      return;
    }
    const bool outLin = linearSide(p.out);
    if (!inOn && (outLin || T == g.outT)) {
      out->push(glide(p.out, frame, g.id, base * std::exp2(g.outC / 1200.0), g.off - T));
      return;
    }
    int64_t n = inOn ? std::min(inEnd, g.off) : g.off;
    if (!(inOn && outLin && linearSide(p.into))) {
      const int64_t minSeg = std::max<int64_t>(1, std::llround(1e-3 * b->clock.sampleRate));
      const int64_t span = inOn ? std::min(g.inLen, g.outLen) : g.outLen;
      const int64_t seg = std::max<int64_t>(minSeg, (span + 31) / 32);
      if (n - T > seg + seg / 2) n = T + seg;  // (no sliver at the end)
      if (n < g.off) g.segAt = n;
      minAct = std::min(minAct, g.segAt);
    }
    out->push(notePitch(frame, g.id, static_cast<float>(base * std::exp2(glideCents(g, n) / 1200.0)),
                        static_cast<uint32_t>(n - T)));
  }
  // AP.11: the Out's distance (cents) toward its target as known at `at` (the Glide lane applied); NaN: none.
  double outCentsOf(const Gen& g, int64_t at) noexcept {
    if (!p.out.step)  // its offset (cents; off since the note started: none)
      return p.out.offset != 0.0f ? static_cast<double>(p.out.offset) * static_cast<double>(g.glideAmt)
                                  : std::numeric_limits<double>::quiet_NaN();
    const double to = nextHz(g, at);
    const double base = baseHz(g);
    if (!(to > 0.0) || !std::isfinite(to) || !(base > 0.0)) return std::numeric_limits<double>::quiet_NaN();
    return 1200.0 * std::log2(to / base) * static_cast<double>(g.glideAmt);
  }
  // The pitch the pattern plays next for rank `rank` (Out -> Step), as known now: the bend-free Hz times the bend of
  // its source at `at`. 0: nothing next.
  double nextHz(const Gen& g, int64_t at) noexcept {
    if (p.pulse) {  // AP.4: the same clock's next pulse (copy (j + 1) mod Steps, back to 0 at the cycle's end)
      const uint32_t s = find(g.src);
      if (s == kNone) return 0.0;
      const Held& h = held[s];
      int64_t nc = g.stepC, nj = g.stepK;
      pulseAdvance(h.ph, nc, nj);
      Elem el = pulseElem(s, nj, g.el.layer);
      el.shift += transAt(pulseLanePos(h.ph, nc, nj));  // AP.5
      NoteEvent tmp;
      const double hz = pitchOf(h.on, el, 0, tmp);
      return hz * std::exp2(h.centsAt(at) / 1200.0);
    }
    if (prevCount > 0 && prevStepK != g.stepK)
      return prevHz[std::min<uint32_t>(g.rank, prevCount - 1)] * layerRatio(g.el.layer);
    if (numHeld == 0) return 0.0;
    if (stale()) rebuild();
    int64_t here = pos;
    if (p.retrigger == ArpRetrigger::Beat && beatIndex(k) != lastBeat) here = 0;
    const uint32_t n = elemCount();
    if (n == 0) return 0.0;
    const uint32_t rank = p.style == ArpStyle::ChordTrigger ? std::min<uint32_t>(g.rank, n - 1) : 0u;
    uint32_t idx = 0;
    int32_t j = 0;
    if (!pick(here, k, rank, false, idx, j)) return 0.0;
    Elem el = element(idx, j);
    el.layer = g.el.layer;
    el.shift += transAt(k);  // AP.5
    const Held& h = held[el.slot];
    NoteEvent tmp;
    const double hz = pitchOf(h.on, el, 0, tmp);
    return hz * std::exp2(h.centsAt(at) / 1200.0);
  }
  // Generated notes' glide actions due at engine time `t` (frame `frame`): an Into glide's end (a source bend still
  // running continues from there; an Out running: re-aimed), an Out glide's start (AP.11: begun before the onset, the
  // note jumps to where the curves are at its onset), a linear piece's end (aim).
  void actDue(int64_t t, uint32_t frame) noexcept {
    for (uint32_t i = 0; i < numGens; ++i) {
      Gen& g = gens[i];
      bool re = false;
      if (g.intoEnd <= t) {
        g.intoEnd = kNever;
        if (g.outing || g.outAt <= t) {
          re = true;
        } else if (const uint32_t s = find(g.src); s != kNone) {
          const Held& h = held[s];
          if (h.cLen != 0 && t < h.cT + h.cLen)
            out->push(notePitch(frame, g.id, static_cast<float>(g.hz * std::exp2(h.c1 / 1200.0)),
                                static_cast<uint32_t>(h.cT + h.cLen - t)));
        }
      }
      if (g.outAt <= t) {
        g.outAt = kNever;
        const double oc = outCentsOf(g, g.off);
        if (std::isfinite(oc) && g.off > t) {
          g.outC = oc;
          g.outing = true;
          if (t > g.outT)
            out->push(notePitch(frame, g.id, static_cast<float>(baseHz(g) * std::exp2(glideCents(g, t) / 1200.0)), 0));
          re = true;
          ++stats.glides;
        }
      }
      if (g.segAt <= t) {
        g.segAt = kNever;
        re = true;
      }
      if (re) aim(g, t, frame);
    }
    recomputeMinOff();
  }

  // ---- Clock ----
  // The song sample of 1/D-tick position `sub` + `extra` (fractional 1/D ticks), through the tempo map.
  double songAt(int64_t sub, double extra) const noexcept { return songAtD(sub, extra, D); }
  // (AR.3: in 1/den ticks; `extra` may be negative: a pulse row's shift.)
  double songAtD(int64_t sub, double extra, int64_t den) const noexcept {
    int64_t q = floorDiv(sub, den);
    double frac = (static_cast<double>(sub - q * den) + extra) / static_cast<double>(den);
    if (frac >= 1.0 || frac < 0.0) {
      const double w = std::floor(frac);
      q += static_cast<int64_t>(w);
      frac -= w;
    }
    const TempoMap& tm = *b->clock.tempo;
    const double sr = b->clock.sampleRate;
    const double s0 = tm.ticksToSamplesF(q, sr);
    if (frac == 0.0) return s0;
    return s0 + frac * (tm.ticksToSamplesF(q + 1, sr) - s0);
  }
  static SampleTime roundSample(double s) noexcept { return static_cast<SampleTime>(std::floor(s + 0.5)); }
  double grooveSub(int64_t step) const noexcept {
    return (step & 1) != 0 ? static_cast<double>(p.groove) * 0.5 * static_cast<double>(Lsub) : 0.0;
  }
  SampleTime songOnset(int64_t step) const noexcept { return roundSample(songAt(step * Lsub, grooveSub(step))); }
  double grooveFree(int64_t step) const noexcept {
    return (step & 1) != 0 ? static_cast<double>(p.groove) * 0.5 * period : 0.0;
  }
  int64_t freeOnset(int64_t step) const noexcept {
    return static_cast<int64_t>(std::floor(t0 + static_cast<double>(step) * period + grooveFree(step) + 0.5));
  }
  // Samples per tick at the free clock's tempo.
  double samplesPerTick() const noexcept {
    const double bpm = b->clock.bpm > 0.0 && std::isfinite(b->clock.bpm) ? b->clock.bpm : 120.0;
    return 60.0 * b->clock.sampleRate / (bpm * static_cast<double>(kPpq));
  }
  // AP.2b: the free clock's bar in quarter notes (the bar at the playhead).
  double barQuarters() const noexcept {
    const double q = b->clock.beatsPerBar;
    return q > 0.0 && std::isfinite(q) ? q : 4.0;
  }
  double freePeriod() const noexcept {
    if (!p.rate.synced) return static_cast<double>(p.rate.ms) * 1e-3 * b->clock.sampleRate;
    return std::max(1.0, static_cast<double>(Lsub) / static_cast<double>(D) * samplesPerTick());
  }
  int64_t beatIndex(int64_t step) const noexcept {
    if (clock == Clock::Song) return floorDiv(step * Lsub, Rsub);
    const double rp = static_cast<double>(Rsub) / static_cast<double>(D) * samplesPerTick();
    return static_cast<int64_t>(std::floor(static_cast<double>(step) * period / rp + 1e-9));
  }

  void updateGrid() noexcept {
    if (gridValid && sameRate(gridRate, p.rate) && sameRate(gridRetrig, p.retriggerRate)) return;
    const bool had = gridValid;
    gridValid = true;
    gridRate = p.rate;
    gridRetrig = p.retriggerRate;
    int64_t ln, ld, rn, rd;
    rateTicks(p.rate, ln, ld);
    rateTicks(p.retriggerRate, rn, rd);
    D = ld / std::gcd(ld, rd) * rd;
    Lsub = ln * (D / ld);
    Rsub = rn * (D / rd);
    if (had && clock == Clock::Song) clock = Clock::None;  // re-seek on the new grid at the next segment
  }

  // Starts the clock at block frame `frame` of segment `seg` (the next step: Song, the first grid step at or after
  // it — or, `atNote` (AP.3: a note-on into an idle arp), a step right there; Free, now).
  void startClock(const ClockSegment& seg, uint32_t frame, bool atNote = false) noexcept {
    if (p.pulse) {
      startPulse(seg, frame);
      return;
    }
    immediate = false;
    if (p.rate.synced && seg.song && b->clock.tempo != nullptr) {
      clock = Clock::Song;
      const SampleTime at = seg.pos + (frame - seg.frame);
      const double ticks = b->clock.tempo->samplesToTicks(at, b->clock.sampleRate);
      int64_t step = static_cast<int64_t>(std::floor(ticks * static_cast<double>(D) / static_cast<double>(Lsub))) - 1;
      while (songOnset(step) < at) ++step;
      while (songOnset(step - 1) >= at) --step;
      k = step;
      nextSong = songOnset(k);
      if (atNote && nextSong != at) {  // between grid steps: this step sounds now, as grid step k − 1
        --k;
        nextSong = at;
        immediate = true;
      }
    } else {
      clock = Clock::Free;
      period = freePeriod();
      t0 = static_cast<double>(b->time + frame);
      k = 0;
      nextTime = freeOnset(0);
    }
    lastBeat = beatIndex(k);
  }

  void resetPattern(int64_t t, bool fill) noexcept {
    pos = 0;
    permValid = false;
    if (fill || p.velocityRetrigger) decayStart = t;
  }

  // ---- Events ----
  void pushExpression(const NoteEvent& e) noexcept { out->push(e); }

  void handle(const NoteEvent& e, const ClockSegment& seg, uint32_t frame) noexcept {
    const int64_t t = b->time + frame;
    switch (e.kind) {
      case NoteEventKind::NoteOn: {
        uint32_t i = find(e.noteId);
        const bool fresh = i == kNone;
        if (i == kNone) {
          if (p.hold && keysDown == 0) {
            while (numHeld > 0) remove(numHeld - 1);  // a new chord replaces the latched one
          }
          if (numHeld == kArpMaxHeld) {
            ++stats.droppedHeld;
            return;
          }
          i = numHeld++;
          held[i] = Held{};
          ++keysDown;
        } else if (!held[i].key) {
          ++keysDown;
        }
        Held& h = held[i];
        h.on = e;
        h.on.frame = 0;
        h.order = nextOrder++;
        h.key = true;
        h.c0 = h.c1 = 0.0;
        h.cLen = 0;
        h.g0 = h.g1 = std::isfinite(e.gain) ? std::max(e.gain, 0.0f) : 0.0f;
        h.gLen = 0;
        dirty = true;
        permValid = false;
        if (p.pulse) {  // AP.4: this note's clock starts (at once when off its grid), every other one re-derived
          h.pFresh = fresh;
          if (clock == Clock::None) {
            startPulse(seg, frame);
            resetPattern(t, true);
          } else {
            if (p.retrigger == ArpRetrigger::Note) resetPattern(t, false);
            pulseLater(frame);
          }
          return;
        }
        if (clock == Clock::None) {
          startClock(seg, frame, numGens == 0);
          resetPattern(t, true);
          prevCount = 0;
        } else if (p.retrigger == ArpRetrigger::Note) {
          resetPattern(t, false);
        }
        return;
      }
      case NoteEventKind::NoteOff: {
        const uint32_t i = find(e.noteId);
        if (i == kNone) {
          NoteEvent o = e;
          o.frame = frame;
          out->push(o);
        } else if (p.hold && (e.flags & kNoteEventGone) == 0) {  // AP.8a: a gone note (handover) is not a key release
          if (held[i].key) --keysDown;
          held[i].key = false;
        } else {
          remove(i);
          if (p.pulse && clock != Clock::None) pulseLater(frame);  // AP.4
        }
        return;
      }
      case NoteEventKind::Pitch: {
        const uint32_t i = find(e.noteId);
        if (i == kNone) {
          NoteEvent o = e;
          o.frame = frame;
          out->push(o);
          return;
        }
        Held& h = held[i];
        if ((e.flags & kNoteEventRetune) != 0) {
          if (h.on.hz > 0.0f && e.hz > 0.0f) h.pPrevHz *= e.hz / h.on.hz;  // AP.8a (L1): its pulse's Into source
          h.on.hz = e.hz;
          h.on.ratio = e.ratio;
          h.on.harmonic = e.harmonic;
          h.on.degree = e.degree;
          h.on.equave = e.equave;
          h.on.tuning = e.tuning;
          if (std::isfinite(e.noteDegree)) h.on.noteDegree = e.noteDegree;  // AP.8a (L1)
          h.c0 = h.c1 = 0.0;
          h.cLen = 0;
          dirty = true;
          for (uint32_t g = 0; g < numGens; ++g)
            if (gens[g].src == e.noteId) rederive(gens[g], h, t, frame, e.rampFrames, true);
          if (p.pulse && clock != Clock::None) pulseLater(frame);  // AP.4: its h may change
          return;
        }
        if (!(e.hz > 0.0f) || !(h.on.hz > 0.0f)) return;
        h.c0 = h.centsAt(t);
        h.c1 = 1200.0 * std::log2(static_cast<double>(e.hz) / static_cast<double>(h.on.hz));
        h.cT = t;
        h.cLen = e.rampFrames;
        const double r = std::exp2(h.c1 / 1200.0);
        for (uint32_t g = 0; g < numGens; ++g) {
          Gen& gn = gens[g];
          if (gn.src != e.noteId || gn.outing) continue;  // AP.3: an Out glide goes to its target
          gn.c = h.c1;
          if (gn.intoEnd != kNever && t < gn.intoEnd)  // AP.3: inside the Into glide: re-aimed, same end
            pushExpression(glide(p.into, frame, gn.id, gn.hz * std::exp2(h.centsAt(gn.intoEnd) / 1200.0), gn.intoEnd - t));
          else
            pushExpression(notePitch(frame, gn.id, static_cast<float>(gn.hz * r), e.rampFrames));
        }
        return;
      }
      case NoteEventKind::Gain: {
        const uint32_t i = find(e.noteId);
        if (i == kNone) {
          if (find(e.sourceNoteId) != kNone && (e.flags & kNoteEventIfSynced) != 0) return;
          NoteEvent o = e;
          o.frame = frame;
          out->push(o);
          return;
        }
        if ((e.flags & kNoteEventIfSynced) != 0) return;
        Held& h = held[i];
        h.g0 = h.gainAt(t);
        h.g1 = std::isfinite(e.gain) ? std::max(e.gain, 0.0f) : 0.0f;
        h.gT = t;
        h.gLen = e.rampFrames;
        for (uint32_t g = 0; g < numGens; ++g)
          if (gens[g].src == e.noteId) pushExpression(noteGain(frame, gens[g].id, h.g1 * gens[g].level, e.rampFrames));
        return;
      }
      case NoteEventKind::Sync: {
        if (find(e.noteId) != kNone || find(e.sourceNoteId) != kNone) return;
        NoteEvent o = e;
        o.frame = frame;
        out->push(o);
        return;
      }
      case NoteEventKind::AllNotesOff:
      case NoteEventKind::AllSoundOff: {
        NoteEvent o = e;
        o.frame = frame;
        out->push(o);
        numHeld = keysDown = 0;
        numGens = 0;
        minOff = minAct = kNever;
        minPulse = kNever;
        prevCount = curCount = 0;
        clock = Clock::None;
        dirty = true;
        permValid = false;
        return;
      }
    }
  }

  // ---- Steps ----
  uint32_t elemCount() const noexcept { return p.style == ArpStyle::HarmonicWalk ? numWalk : numHeld; }
  // Element `idx` of the ordered list (held notes by Hz / arrival, or the walk), copy j: what it plays.
  Elem element(uint32_t idx, int32_t j) const noexcept {
    const auto n = static_cast<int64_t>(elemCount());
    int64_t i2 = idx, w = 0;
    int32_t shift = j * p.distance;
    if (p.transpose == ArpTranspose::Rotate) {
      const int64_t r = static_cast<int64_t>(idx) + static_cast<int64_t>(j) * p.distance;
      w = floorDiv(r, n);
      i2 = r - w * n;
      shift = 0;
    }
    Elem el;
    el.shift = shift;
    el.wraps = static_cast<int32_t>(w);
    if (p.style == ArpStyle::HarmonicWalk) {
      el.slot = walk[static_cast<size_t>(i2)].slot;
      el.walk = walk[static_cast<size_t>(i2)].m;
      el.under = p.walkUnder;
    } else {
      el.slot = (p.style == ArpStyle::PlayOrder ? arrival : sorted)[static_cast<size_t>(i2)];
    }
    return el;
  }
  // The cycle's period (elements per copy).
  int64_t periodLen() const noexcept {
    const auto n = static_cast<int64_t>(elemCount());
    switch (p.style) {
      case ArpStyle::ChordTrigger: return 1;
      case ArpStyle::Random:
      case ArpStyle::RandomOther:
      case ArpStyle::RandomOnce: return n;
      case ArpStyle::HarmonicWalk: return p.walkOrder == ArpWalkOrder::UpDown && n > 2 ? 2 * n - 2 : n;
      default: return patternLen;
    }
  }
  // Pattern step `here` (grid step kk), rank `rank` of a chord: the list index and copy. `commit`: a Random Other /
  // Once shuffle it needs is kept (a step); else drawn into scratch (a look ahead). False: beyond Repeats / empty.
  bool pick(int64_t here, int64_t kk, uint32_t rank, bool commit, uint32_t& idx, int32_t& j) noexcept {
    const auto n = elemCount();
    if (n == 0) return false;
    const int64_t P = std::max<int64_t>(periodLen(), 1);
    const int64_t cycle = P * p.steps;
    if (p.repeats > 0 && here >= static_cast<int64_t>(p.repeats) * cycle) return false;
    int64_t c = (here + p.offset) % cycle;
    if (c < 0) c += cycle;
    j = static_cast<int32_t>(c / P);
    const auto e = static_cast<uint32_t>(c % P);
    auto permuted = [&](bool reshuffle) {
      if (!reshuffle) return static_cast<uint32_t>(perm[e]);
      if (commit) {
        shuffle(kk);
        return static_cast<uint32_t>(perm[e]);
      }
      shuffleInto(scratchPerm.data(), kk);
      return static_cast<uint32_t>(scratchPerm[e]);
    };
    switch (p.style) {
      case ArpStyle::ChordTrigger: idx = rank; break;
      case ArpStyle::Random: idx = static_cast<uint32_t>(draw(p.seed, kk, 0) % n); break;
      case ArpStyle::RandomOther: idx = permuted(e == 0 || !permValid); break;
      case ArpStyle::RandomOnce: idx = permuted(!permValid); break;
      case ArpStyle::HarmonicWalk:
        switch (p.walkOrder) {
          case ArpWalkOrder::Up: idx = e; break;
          case ArpWalkOrder::Down: idx = n - 1 - e; break;
          case ArpWalkOrder::UpDown: idx = e < n ? e : 2 * n - 2 - e; break;
          default: idx = static_cast<uint32_t>(draw(p.seed, kk, 0) % n); break;
        }
        break;
      default: idx = pattern[e]; break;
    }
    return true;
  }

  // ---- AP.6: the panel status (arp.h "Panel status") ----
  bool randomOrder() const noexcept {
    switch (p.style) {
      case ArpStyle::Random:
      case ArpStyle::RandomOther:
      case ArpStyle::RandomOnce: return true;
      case ArpStyle::HarmonicWalk: return p.walkOrder == ArpWalkOrder::Random;
      default: return false;
    }
  }
  // The source Hz of element `idx` of the ordered list.
  float listHz(uint32_t idx) const noexcept {
    if (p.style == ArpStyle::HarmonicWalk) return idx < numWalk ? static_cast<float>(walk[idx].hz) : 0.0f;
    if (idx >= numHeld) return 0.0f;
    return held[(p.style == ArpStyle::PlayOrder ? arrival : sorted)[idx]].on.hz;
  }
  // The list element position e of a period plays (an order that is not random; pick's mapping).
  uint32_t idxAt(uint32_t e) const noexcept {
    if (p.style != ArpStyle::HarmonicWalk) return e < kArpMaxPattern ? pattern[e] : 0u;
    const uint32_t n = numWalk;
    switch (p.walkOrder) {
      case ArpWalkOrder::Down: return n - 1 - e;
      case ArpWalkOrder::UpDown: return e < n ? e : 2 * n - 2 - e;
      default: return e;
    }
  }
  // The pattern list, after a block in which it may have changed (one O(entries) write).
  void statusBlock() noexcept {
    const bool idle = numHeld == 0;
    if (idle) {
      if (!statusIdle) {
        status->writeIdle();
        statusIdle = true;
      }
      patternNew = false;
      return;
    }
    if (stale()) rebuild();
    if (!patternNew && !statusIdle && statusStyle == p.style && statusPulse == p.pulse && statusOrder == p.walkOrder)
      return;
    patternNew = statusIdle = false;
    statusStyle = p.style;
    statusPulse = p.pulse;
    statusOrder = p.walkOrder;
    const bool chord = !p.pulse && p.style == ArpStyle::ChordTrigger;
    // AP.10: the held notes' identities by Hz (what the panel sketches the cycle over).
    auto heldAt = [&](uint32_t i) {
      const NoteEvent& e = held[sorted[i]].on;
      return NoteStageStatus::HeldNote{e.hz, e.velocity, e.ratio, e.harmonic, e.degree, e.equave, e.tuning, e.noteDegree};
    };
    if (p.pulse || chord) {
      status->writePattern(numHeld, numHeld, chord, [&](uint32_t i) { return held[sorted[i]].on.hz; }, numHeld, heldAt);
    } else if (randomOrder()) {
      status->writePattern(elemCount(), numHeld, false, [&](uint32_t i) { return listHz(i); }, numHeld, heldAt);
    } else {
      const auto len = static_cast<uint32_t>(std::max<int64_t>(periodLen(), 1));
      status->writePattern(len, numHeld, false, [&](uint32_t i) { return listHz(idxAt(i)); }, numHeld, heldAt);
    }
  }
  // A step that is due: each lane's position (lane position `lp` mod its length), the pattern position (−1: a rest).
  void statusStep(int64_t lp, int32_t position, int32_t copy) noexcept {
    int32_t l[kArpLaneCount];
    for (size_t i = 0; i < kArpLaneCount; ++i) {
      const int64_t len = p.laneLength[i];
      l[i] = static_cast<int32_t>(lp - floorDiv(lp, len) * len);
    }
    status->writeStep(l, kArpLaneCount, position, copy);
  }
  // The pattern position step `here` played element `idx` at (statusBlock's list).
  int32_t statusPosition(int64_t here, uint32_t idx) const noexcept {
    if (p.style == ArpStyle::ChordTrigger) return 0;
    if (randomOrder()) return static_cast<int32_t>(idx);
    const int64_t P = std::max<int64_t>(periodLen(), 1);
    const int64_t cycle = P * p.steps;
    int64_t c = (here + p.offset) % cycle;
    if (c < 0) c += cycle;
    return static_cast<int32_t>(c % P);
  }
  // AP.10: the cycle in steps (Rate periods) as the held set and params stand: period × Steps (Chord Trigger:
  // Steps); Pulse: the Cycle over the Base period. 0: nothing held.
  double cycleSteps() const noexcept {
    if (numHeld == 0) return 0.0;
    if (p.pulse) {  // AR.3: the Cycle in Rate periods (the sketch's time unit)
      if (clock == Clock::Song)
        return Lsub > 0 ? static_cast<double>(pCycSub) * static_cast<double>(D) / (static_cast<double>(pD) * static_cast<double>(Lsub)) : 0.0;
      const double fp = b != nullptr ? freePeriod() : 0.0;
      return fp > 0.0 ? pCyc / fp : 0.0;
    }
    return static_cast<double>(std::max<int64_t>(periodLen(), 1)) * p.steps;
  }

  // ---- Step lanes (AP.5) ----
  bool laneOn(ArpLane l) const noexcept { return p.laneOn[static_cast<size_t>(l)]; }
  // Lane `l` at lane position `lp` (mod its length): its step's slot (the block's value) or fixed value, clamped.
  float laneAt(ArpLane l, int64_t lp) const noexcept {
    const auto li = static_cast<size_t>(l);
    const int64_t len = p.laneLength[li];
    const auto i = static_cast<size_t>(lp - floorDiv(lp, len) * len);
    const uint32_t slot = laneSlot[li][i];
    float v = slot < b->params.size() ? laneOffset[li] + laneScale[li] * b->params[slot] : laneFixed.value[li][i];
    for (uint32_t m = laneModBegin[li][i], e = m + laneModCount[li][i]; m < e; ++m)  // AP.8b (M8): cables into it
      if (laneMods[m] < b->mods.size()) v += b->mods[laneMods[m]];
    const ArpLaneRange& r = kArpLaneRange[li];
    return std::isfinite(v) ? std::clamp(v, r.min, r.max) : r.def;
  }
  ArpStepKind kindAt(int64_t lp) const noexcept {
    if (!laneOn(ArpLane::SkipTie)) return ArpStepKind::Play;
    return static_cast<ArpStepKind>(std::clamp<long>(std::lround(laneAt(ArpLane::SkipTie, lp)), 0, 2));
  }
  int32_t transAt(int64_t lp) const noexcept {
    return laneOn(ArpLane::Transpose) ? static_cast<int32_t>(std::lround(laneAt(ArpLane::Transpose, lp))) : 0;
  }
  // The lanes of the step at lane position `lp` (only the lanes that are on are read).
  StepCtx lanesAt(int64_t lp) const noexcept {
    StepCtx c;
    c.key = lp;
    c.gate = p.gate;
    if (laneOn(ArpLane::Velocity)) c.vel = laneAt(ArpLane::Velocity, lp);
    if (laneOn(ArpLane::Gate)) c.gate = std::clamp(p.gate * laneAt(ArpLane::Gate, lp), 0.01f, 2.0f);
    if (laneOn(ArpLane::Glide)) c.glide = laneAt(ArpLane::Glide, lp);
    c.trans = transAt(lp);
    c.kind = kindAt(lp);
    return c;
  }
  // Whether the step plays by the Chance lane: a counter-RNG draw keyed by (seed, key, salt).
  bool chancePlays(int64_t lp, int64_t key, uint32_t salt) const noexcept {
    if (!laneOn(ArpLane::Chance)) return true;
    const float c = laneAt(ArpLane::Chance, lp);
    if (c >= 1.0f) return true;
    if (c <= 0.0f) return false;
    const double u = static_cast<double>(draw(p.seed, key, salt) >> 40) * (1.0 / 16777216.0);
    return u < static_cast<double>(c);
  }
  // The waiting notes (tieWait) not continued by the step with key `key` (pulse: of source `src` only) end at `t`.
  void endUntied(int64_t t, int64_t key, uint32_t src, bool pulseOnly, uint32_t slot = kNone) noexcept {
    bool any = false;
    if (pulseOnly && tieIdxValid && slot < kArpMaxHeld) {  // AP.8a (M4): this clock's waiting notes, by layer
      for (uint32_t L = 0; L <= static_cast<uint32_t>(kArpMaxLayers); ++L) {
        const uint16_t e = tieIdx[slot * (kArpMaxLayers + 1) + L];
        if (e == 0) continue;
        Gen& g = gens[e - 1u];
        if (!g.tieWait || g.tieKey == key || g.src != src) continue;
        g.tieWait = false;
        g.off = std::min(g.off, t);
        any = true;
      }
      if (any) minOff = std::min(minOff, t);  // (earlier ends only: a bound)
      return;
    }
    for (uint32_t i = 0; i < numGens; ++i) {
      Gen& g = gens[i];
      if (!g.tieWait || g.tieKey == key || (pulseOnly && g.src != src)) continue;
      g.tieWait = false;
      g.off = std::min(g.off, t);
      any = true;
    }
    if (any) minOff = std::min(minOff, t);  // (earlier ends only: a bound)
  }
  // A waiting note a tie step continues: the same rank and layer (pulse: the same clock and layer).
  Gen* tieWaiting(const Held& h, const Elem& el, uint32_t rank) noexcept {
    const auto r = static_cast<uint16_t>(std::min<uint32_t>(rank, 0xFFFFu));
    const auto key = p.pulse ? static_cast<uint32_t>(&h - held.data()) : rank;
    if (tieIdxValid && key < kArpMaxHeld && el.layer <= kArpMaxLayers) {  // AP.8a (M4): the step's table
      const uint16_t e = tieIdx[key * (kArpMaxLayers + 1) + el.layer];
      if (e == 0) return nullptr;
      Gen& g = gens[e - 1u];
      if (g.tieWait && g.el.layer == el.layer && (p.pulse ? g.src == h.on.noteId : g.rank == r)) return &g;
    }
    for (uint32_t i = 0; i < numGens; ++i) {
      Gen& g = gens[i];
      if (!g.tieWait || g.el.layer != el.layer) continue;
      if (p.pulse ? g.src == h.on.noteId : g.rank == r) return &g;
    }
    return nullptr;
  }
  // The tie: `g` continues as this step's note (its end at `off`; with Into on it glides to this step's pitch over
  // the glide time x the Glide lane, else keeps its pitch). Returns its own Hz now (bend included).
  float continueTie(Gen& g, const Elem& el, uint32_t rank, int64_t kk, int64_t t, uint32_t frame, int64_t off,
                    const StepCtx& sc) noexcept {
    const Held& h = held[el.slot];
    const int64_t len = off - t;
    g.tieWait = sc.holdNext;
    g.tieKey = sc.key;
    g.off = sc.holdNext ? std::max(off, sc.nextOn + 1) : off;
    g.stepK = kk;
    g.stepC = sc.cyc;
    g.glideAmt = sc.glide;
    g.outing = false;
    g.outAt = g.intoEnd = g.segAt = kNever;
    g.inC = 0.0;
    ++stats.ties;
    double own = g.hz * std::exp2(g.c / 1200.0);
    if (p.into.on() || (intoOn() && sc.glide > 0.0f)) {  // (the lane's slide alone: only a painted step moves it)
      NoteEvent tmp;
      const float hz = pitchOf(h.on, el, 0, tmp);
      if (hz > 0.0f) {
        const double c = h.centsAt(t);
        const bool bending = h.cLen != 0 && t < h.cT + h.cLen;
        g.src = h.on.noteId;
        g.el = el;
        g.hz = hz;
        g.c = bending ? h.c1 : c;
        const int64_t ti = std::llround(static_cast<double>(glideFrames(p.into, len)) * sc.glide);
        const double was = own;  // (the tie glides from where it is)
        own = static_cast<double>(hz) * std::exp2(c / 1200.0);
        if (ti > 0) {
          out->push(glide(p.into, frame, g.id, static_cast<double>(hz) * std::exp2(h.centsAt(t + ti) / 1200.0), ti));
          g.intoEnd = t + ti;
          g.inT = t;
          g.inLen = ti;
          g.inC = was > 0.0 && std::isfinite(was) ? 1200.0 * std::log2(was / own) : 0.0;
          ++stats.glides;
        } else {
          out->push(notePitch(frame, g.id, static_cast<float>(own), 0));
        }
      }
    }
    if (!g.tieWait && p.out.on()) {  // the merged note's Out glide over the end of its gate
      const int64_t to = glideFrames(p.out, len);
      if (to > 0) {  // AP.11: additive (no shrink); begun before the tie's onset: from there
        g.outT = g.off - to;
        g.outLen = to;
        g.outAt = std::max(g.outT, t);
      }
    }
    if (rank < kArpMaxHeld && el.layer == 0) {
      curHz[rank] = static_cast<float>(own);
      curCount = std::max(curCount, rank + 1);
    }
    // AP.8a (M4): a bound, not a rescan per tie (a later end only makes minOff early: one extra endDue pass)
    minOff = std::min(minOff, g.off);
    minAct = std::min({minAct, g.intoEnd, g.outAt});
    return static_cast<float>(own);
  }

  // One generated note. `stepFrom`: the Into -> Step source of layer 0 (< 0: the pattern's previous step by rank);
  // `level`: its layer's gain; `sc`: the step's lanes (AP.5: a tie continues a waiting note instead). Returns its own
  // Hz at the onset (bend included; 0: a rest).
  float emit(const Elem& el, uint32_t rank, int64_t kk, float velocity, int64_t t, uint32_t frame, int64_t off,
             double stepFrom, float level, const StepCtx& sc) noexcept {
    const Held& h = held[el.slot];
    if (sc.kind == ArpStepKind::Tie)
      if (Gen* w = tieWaiting(h, el, rank)) return continueTie(*w, el, rank, kk, t, frame, off, sc);
    NoteEvent on;
    on.frame = frame;
    on.kind = NoteEventKind::NoteOn;
    on.velocity = velocity;
    const float hz = pitchOf(h.on, el, 0, on);
    if (!(hz > 0.0f)) return 0.0f;  // a rest (Harmonics below the first)
    if (numGens == kArpMaxSounding) makeRoom(1, frame, false);  // (the step made room for its notes: a rare tie)
    on.noteId = newId();
    const double c = h.centsAt(t);
    on.hz = c != 0.0 ? static_cast<float>(hz * std::exp2(c / 1200.0)) : hz;
    const float own = on.hz;
    on.gain = h.gainAt(t) * level;
    const bool bending = h.cLen != 0 && t < h.cT + h.cLen;
    const int64_t end = sc.holdNext ? std::max(off, sc.nextOn + 1) : off;  // AP.5: held for the tie
    Gen g{on.noteId, h.on.noteId, el,    hz, end, bending ? h.c1 : c, kNever, kNever, false,
          static_cast<uint16_t>(std::min<uint32_t>(rank, 0xFFFFu)), kk, level};
    g.tieWait = sc.holdNext;
    g.tieKey = sc.key;
    g.glideAmt = sc.glide;
    g.stepC = sc.cyc;
    // AP.3 glides: Into from the previous step / an offset, Out over the end of the gate (AP.5: not while it waits
    // for a tie; distances x the Glide lane).
    const int64_t len = off - t;
    int64_t ti = intoOn() ? glideFrames(p.into, len) : 0;
    int64_t to = p.out.on() && !sc.holdNext ? glideFrames(p.out, len) : 0;
    double from = 0.0;
    if (ti > 0) {
      if (intoStep()) {
        if (stepFrom >= 0.0) from = stepFrom;
        else if (prevCount > 0) from = prevHz[std::min(rank, prevCount - 1)];
        from *= layerRatio(el.layer);
      } else {  // its offset in cents from its own pitch (the source's bend included)
        from = static_cast<double>(own) * std::exp2(static_cast<double>(p.into.offset) / 1200.0);
      }
      if (sc.glide != 1.0f && from > 0.0 && std::isfinite(from))
        from = sc.glide > 0.0f ? static_cast<double>(own) * std::pow(from / static_cast<double>(own), sc.glide) : 0.0;
      if (!(from > 0.0) || !std::isfinite(from) || static_cast<float>(from) == own) from = 0.0;
    }
    if (from == 0.0) ti = 0;
    if (to > 0 && sc.glide <= 0.0f) to = 0;
    if (from > 0.0) on.hz = static_cast<float>(from);
    out->push(on);
    if (trace != nullptr) trace->push_back({on.noteId, h.on.noteId, exactOf(h.on, el), own});  // AP.7: offline only
    if (from > 0.0) {
      out->push(glide(p.into, frame, on.noteId, static_cast<double>(hz) * std::exp2(h.centsAt(t + ti) / 1200.0), ti));
      g.intoEnd = t + ti;
      g.inT = t;
      g.inLen = ti;
      g.inC = 1200.0 * std::log2(from / static_cast<double>(own));
      ++stats.glides;
    } else if (bending) {
      out->push(notePitch(frame, on.noteId, static_cast<float>(hz * std::exp2(h.c1 / 1200.0)),
                          static_cast<uint32_t>(h.cT + h.cLen - t)));
    }
    if (h.gLen != 0 && t < h.gT + h.gLen)
      out->push(noteGain(frame, on.noteId, h.g1 * level, static_cast<uint32_t>(h.gT + h.gLen - t)));
    if (to > 0) {  // AP.11: additive with the Into (no shrink); Out > 100 %: begun before the onset (aimed at it)
      g.outT = off - to;
      g.outLen = to;
      g.outAt = std::max(g.outT, t);
    }
    if (rank < kArpMaxHeld && el.layer == 0) {
      curHz[rank] = own;
      curCount = std::max(curCount, rank + 1);
    }
    gens[numGens++] = g;
    minOff = std::min(minOff, end);
    minAct = std::min({minAct, g.intoEnd, g.outAt});
    ++stats.notes;
    return own;
  }

  // The velocity of a step of held note `h` at `t` with the step's Velocity lane factor `lane` (AP.5).
  float velocityOf(const Held& h, int64_t t, float lane) const noexcept {
    float v;
    if (p.velocitySource == ArpVelocitySource::Lane) v = laneOn(ArpLane::Velocity) ? lane : h.on.velocity;
    else v = (p.velocitySource == ArpVelocitySource::Fixed ? p.fixedVelocity : h.on.velocity) * lane;
    if (p.velocityDecayMs > 0.0f) {
      const double len = static_cast<double>(p.velocityDecayMs) * 1e-3 * b->clock.sampleRate;
      const double f = std::min(1.0, static_cast<double>(t - decayStart) / len);
      v = static_cast<float>(v + (p.velocityTarget - v) * f);
    }
    return v;
  }

  void step(const ClockSegment& seg, uint32_t frame) noexcept {
    const int64_t t = b->time + frame;
    const int64_t kk = k;
    StepCtx sc = lanesAt(kk);  // AP.5: lane position = the grid step index
    const float gate = sc.gate;
    int64_t off;
    if (clock == Clock::Song) {
      const SampleTime onS = nextSong;
      const SampleTime offS = roundSample(songAt(kk * Lsub, grooveSub(kk) + static_cast<double>(gate) * static_cast<double>(Lsub)));
      if (immediate) {  // AP.3: the first step at the note-on, off the grid: its grid step's gate length from here
        immediate = false;
        SampleTime len = std::max<SampleTime>(1, offS - songOnset(kk));
        if (gate <= 1.0f) len = std::min<SampleTime>(len, songOnset(kk + 1) - onS);
        off = t + std::max<int64_t>(1, len);
      } else {
        off = t + std::max<int64_t>(1, offS - onS);
      }
      ++k;
      nextSong = songOnset(k);
      sc.nextOn = t + (nextSong - onS);
    } else {
      off = t + std::max<int64_t>(1, std::llround(static_cast<double>(gate) * period));
      ++k;
      nextTime = freeOnset(k);
      sc.nextOn = nextTime;
    }
    (void)seg;
    if (numHeld == 0) {
      clock = Clock::None;
      endUntied(t, kk, 0, false);
      return;
    }
    if (stale()) rebuild();
    if (p.retrigger == ArpRetrigger::Beat) {
      const int64_t beat = beatIndex(kk);
      if (beat != lastBeat) {
        lastBeat = beat;
        resetPattern(t, false);
      }
    }
    const int64_t here = pos++;
    uint32_t idx = 0;
    int32_t j = 0;
    const bool picked = pick(here, kk, 0, true, idx, j);
    const bool plays = picked && sc.kind != ArpStepKind::Skip && chancePlays(kk, kk, 0x100u);
    if (status) statusStep(kk, plays ? statusPosition(here, idx) : -1, j);  // AP.6
    if (!plays) {
      if (sc.kind == ArpStepKind::Skip || laneOn(ArpLane::Chance)) ++stats.laneRests;
      endUntied(t, kk, 0, false);
      return;
    }
    sc.holdNext = kindAt(kk + 1) == ArpStepKind::Tie;
    ++stats.steps;
    const uint32_t count = p.style == ArpStyle::ChordTrigger ? elemCount() : 1u;
    // AP.8a (M4): room for the step's notes in one pass; a tie step's waiting notes by (rank, layer).
    makeRoom(count * static_cast<uint32_t>(1 + p.layers), frame, sc.kind == ArpStepKind::Tie);
    if (sc.kind == ArpStepKind::Tie) buildTies();
    curCount = 0;
    for (uint32_t m = 0; m < count; ++m) {
      if (m > 0) pick(here, kk, m, true, idx, j);
      Elem el = element(idx, j);
      el.shift += sc.trans;
      const float v = velocityOf(held[el.slot], t, sc.vel);
      emit(el, m, kk, v, t, frame, off, -1.0, 1.0f, sc);
      for (int32_t L = 1; L <= p.layers; ++L) {  // AP.4
        Elem le = el;
        le.layer = static_cast<uint8_t>(L);
        emit(le, m, kk, v, t, frame, off, -1.0, p.layerLevel[static_cast<size_t>(L - 1)], sc);
      }
    }
    tieIdxValid = false;
    endUntied(t, kk, 0, false);
    if (curCount > 0) {  // this step's notes are the next one's "previous step"
      std::copy(curHz.begin(), curHz.begin() + curCount, prevHz.begin());
      prevCount = curCount;
      prevStepK = kk;
    }
  }

  // ---- Pulse mode (AP.4; AR.3: arp.h "Pulse mode") ----
  // Cycle c's start and length in 1/pD ticks (Song). pVar: bar position c · num / den of the signature map (a
  // fraction of a bar is that fraction of ITS length): exact integers.
  int64_t cycStart(int64_t c) const noexcept {
    if (!pVar) return c * pCycSub;
    const int64_t x = c * p.pulseCycleNum, q = floorDiv(x, pD), r = x - q * pD;
    const TimeSigMap& sm = *b->clock.signatures;
    const Ticks bt = sm.barToTicks(q);
    return bt * pD + (r != 0 ? r * (sm.barToTicks(q + 1) - bt) : 0);
  }
  int64_t cycLen(int64_t c) const noexcept { return pVar ? cycStart(c + 1) - cycStart(c) : pCycSub; }
  void pulseAdvance(int64_t n, int64_t& c, int64_t& j) const noexcept {
    if (++j >= n) {
      j = 0;
      ++c;
    }
  }
  void pulseBack(int64_t n, int64_t& c, int64_t& j) const noexcept {
    if (--j < 0) {
      --c;
      j = n - 1;
    }
  }
  // Song: pulse (c, j) of clock `H` (division n) at 1/pD-tick position sub + frac: start(c) + j · Cycle(c) / n, exact
  // (Cycle = a · n + r, so j · Cycle / n = j · a + j · r / n with j · r small), + the row's shift of a period in frac
  // (it may leave [0, 1): songAtD floors it).
  void pulseSub(const Held& H, int64_t c, int64_t j, int64_t& sub, double& frac, double& per) const noexcept {
    const int64_t n = H.ph, len = cycLen(c);
    const int64_t a = len / n, r = len - a * n, jr = j * r, q = jr / n;
    sub = cycStart(c) + j * a + q;
    per = static_cast<double>(len) / static_cast<double>(n);
    frac = static_cast<double>(jr - q * n) / static_cast<double>(n) + H.pShift * per;
  }
  double pulseFreeAt(const Held& H, int64_t c, int64_t j) const noexcept {
    return t0 + static_cast<double>(c) * pCyc + (static_cast<double>(j) + H.pShift) * pCyc / static_cast<double>(H.ph);
  }
  // The onset of pulse (c, j): Song: a song sample; Free: engine time.
  int64_t pulseOnset(const Held& H, int64_t c, int64_t j) const noexcept {
    if (clock == Clock::Song) {
      int64_t sub;
      double frac, per;
      pulseSub(H, c, j, sub, frac, per);
      return roundSample(songAtD(sub, frac, pD));
    }
    return static_cast<int64_t>(std::floor(pulseFreeAt(H, c, j) + 0.5));
  }
  // What pulse j (in its cycle) of held slot `s` plays: copy j mod Steps, layer `layer`.
  Elem pulseElem(uint32_t s, int64_t j, uint8_t layer) const noexcept {
    Elem el;
    el.slot = s;
    el.shift = p.steps > 1 ? static_cast<int32_t>(j % p.steps) * p.distance : 0;
    el.layer = layer;
    return el;
  }
  // Clock of held[slot] to its first onset at or after `at` that it has not played; `atNote`: a note-on at `at`
  // (an onset there, else an immediate pulse there as the grid's previous pulse).
  void pulseSeek(uint32_t slot, int64_t at, bool atNote) noexcept {
    Held& H = held[slot];
    atNote = atNote || (H.pImmediate && H.pNext == at);  // a pending immediate pulse stays one
    const int64_t n = H.ph;
    int64_t c, j;
    if (clock == Clock::Song) {
      const double ticks = b->clock.tempo->samplesToTicks(at, b->clock.sampleRate);
      const double sub = ticks * static_cast<double>(pD);
      if (pVar) {  // the cycle of the bar at `at`, then on to the one holding it (≤ den cycles start inside a bar)
        c = floorDiv(b->clock.signatures->ticksToBarBeat(static_cast<Ticks>(std::floor(ticks))).bar * pD, p.pulseCycleNum);
        for (int i = 0; i < kArpMaxPulseCycleDen && static_cast<double>(cycStart(c + 1)) <= sub; ++i) ++c;
      } else {
        c = static_cast<int64_t>(std::floor(sub / static_cast<double>(pCycSub)));
      }
      const double rel = sub - static_cast<double>(cycStart(c));
      j = static_cast<int64_t>(std::floor(rel * static_cast<double>(n) / static_cast<double>(cycLen(c)) - H.pShift));
    } else {
      const double rel = static_cast<double>(at) - t0;
      c = static_cast<int64_t>(std::floor(rel / pCyc));
      j = static_cast<int64_t>(std::floor((rel - static_cast<double>(c) * pCyc) * static_cast<double>(n) / pCyc - H.pShift));
    }
    if (j < 0) {  // (a positive shift: the cycle's first onset is after its start)
      --c;
      j = n - 1;
    }
    j = std::clamp<int64_t>(j, 0, n - 1);
    for (int i = 0; i < 64 && pulseOnset(H, c, j) < at; ++i) pulseAdvance(n, c, j);
    for (int i = 0; i < 64; ++i) {
      int64_t bc = c, bj = j;
      pulseBack(n, bc, bj);
      if (pulseOnset(H, bc, bj) < at) break;
      c = bc;
      j = bj;
    }
    if (H.pHasLast)
      for (int i = 0; i < 64 && pulseOnset(H, c, j) <= H.pLast; ++i) pulseAdvance(n, c, j);
    H.pc = c;
    H.pj = j;
    H.pNext = pulseOnset(H, c, j);
    H.pImmediate = false;
    if (atNote && H.pNext != at) {
      pulseBack(n, H.pc, H.pj);
      H.pNext = at;
      H.pImmediate = true;
    }
  }
  // Re-derives every clock's row (its rank by Hz mod kArpPulseRows: mask, shift), division and the Cycle at block
  // frame `frame` of `seg`, and re-seeks every clock there (a note added there: pFresh). O(held) per call: held-set
  // changes at one frame share one call (pulseLater).
  void pulseRefresh(const ClockSegment& seg, uint32_t frame) noexcept {
    pulseStale = false;
    pulsePending = false;
    minPulse = kNever;
    if (numHeld == 0 || clock == Clock::None) return;
    if (stale()) rebuild();
    for (uint32_t r = 0; r < numHeld; ++r) {
      Held& H = held[sorted[r]];
      const uint32_t row = r % static_cast<uint32_t>(kArpPulseRows);
      H.ph = arpPulseDiv(p, H.on.harmonic, r);
      H.pMask = p.pulseMask[row];
      H.pShift = static_cast<double>(p.pulseShift[row]);
      H.pRank = r;
    }
    int64_t at;
    if (clock == Clock::Song) {
      at = seg.pos + (frame - seg.frame);
      // The Cycle: num / den bars of the song's signature (one signature: a constant cycle; changes: per cycle).
      const TimeSigMap* sm = b->clock.signatures;
      pD = p.pulseCycleDen;
      pVar = sm != nullptr && sm->changeCount() > 1;
      const Ticks bar = sm != nullptr ? ticksPerBar(sm->changeSignature(0)) : kTicksPerWhole;
      pCycSub = std::max<int64_t>(1, p.pulseCycleNum * bar);
    } else {
      at = b->time + frame;
      pVar = false;
      pBpm = b->clock.bpm;
      pBpb = b->clock.beatsPerBar;
      pCyc = std::max(1.0, static_cast<double>(p.pulseCycleNum) / static_cast<double>(p.pulseCycleDen) * barQuarters() *
                               static_cast<double>(kPpq) * samplesPerTick());
    }
    for (uint32_t i = 0; i < numHeld; ++i) {
      pulseSeek(i, at, held[i].pFresh);
      held[i].pFresh = false;
      minPulse = std::min(minPulse, held[i].pNext);
    }
  }
  void pulseLater(uint32_t frame) noexcept {
    pulsePending = true;
    pulsePendingFrame = frame;
  }
  void startPulse(const ClockSegment& seg, uint32_t frame) noexcept {
    immediate = false;
    clock = p.rate.synced && seg.song && b->clock.tempo != nullptr ? Clock::Song : Clock::Free;
    t0 = static_cast<double>(b->time + frame);
    for (uint32_t i = 0; i < numHeld; ++i) {
      held[i].pHasLast = held[i].pImmediate = false;
      held[i].pPrevHz = 0.0f;
    }
    (void)seg;
    pulseLater(frame);
  }
  // AP.5: a clock's lane position for pulse (c, j): its pulse count c · n + j.
  int64_t pulseLanePos(int64_t n, int64_t c, int64_t j) const noexcept { return c * n + j; }
  // Pulse (pc, pj) of held slot `s`, due at `frame`: its notes (layers included), then the clock moves on.
  void pulseFire(uint32_t s, uint32_t frame) noexcept {
    Held& H = held[s];
    const int64_t h = H.ph, t = b->time + frame, c = H.pc, j = H.pj, onS = H.pNext;
    StepCtx sc = lanesAt(pulseLanePos(h, c, j));  // AP.5: per-note lane position
    sc.cyc = c;
    const float gate = sc.gate;
    int64_t nc = c, nj = j;
    pulseAdvance(h, nc, nj);
    const int64_t nextOn = pulseOnset(H, nc, nj);
    int64_t len;
    if (clock == Clock::Song) {
      int64_t sub;
      double frac, per;
      pulseSub(H, c, j, sub, frac, per);
      const SampleTime offS = roundSample(songAtD(sub, frac + static_cast<double>(gate) * per, pD));
      len = offS - (H.pImmediate ? roundSample(songAtD(sub, frac, pD)) : onS);
    } else {
      len = std::llround(static_cast<double>(gate) * pCyc / static_cast<double>(h));
    }
    if (H.pImmediate && gate <= 1.0f) len = std::min<int64_t>(len, nextOn - onS);
    const int64_t off = t + std::max<int64_t>(1, len);
    H.pLast = onS;
    H.pHasLast = true;
    H.pImmediate = false;
    H.pc = nc;
    H.pj = nj;
    H.pNext = nextOn;
    for (int i = 0; i < 64 && H.pNext <= onS; ++i) {  // pulses closer than a sample merge
      pulseAdvance(h, H.pc, H.pj);
      H.pNext = pulseOnset(H, H.pc, H.pj);
    }
    if (H.pNext <= onS) pulseSeek(s, onS + 1, false);
    const uint32_t src = H.on.noteId;
    const bool open = ((H.pMask >> (static_cast<uint32_t>(j) & 31u)) & 1u) != 0;  // AR.3: the row's mask, one AND
    const bool plays = open && sc.kind != ArpStepKind::Skip && chancePlays(sc.key, sc.key, 0x10000u + static_cast<uint32_t>(h));
    if (status) {  // AP.6: the pulsing note's rank by Hz (the pattern list of Pulse mode); AR.3: its pulse, its place
      double ph = (static_cast<double>(j) + H.pShift) / static_cast<double>(h);
      ph -= std::floor(ph);
      status->writePulse(H.pRank, static_cast<int32_t>(j), static_cast<float>(ph));
      statusStep(sc.key, plays ? static_cast<int32_t>(H.pRank) : -1, static_cast<int32_t>(j % std::max(p.steps, 1)));
    }
    if (!plays) {
      if (open) ++stats.laneRests;
      else ++stats.pulseMutes;
      endUntied(t, sc.key, src, true, s);
      return;
    }
    sc.holdNext = kindAt(pulseLanePos(h, H.pc, H.pj)) == ArpStepKind::Tie;
    sc.nextOn = clock == Clock::Song ? t + (H.pNext - onS) : H.pNext;
    ++stats.pulses;
    ++stats.steps;
    const float v = velocityOf(H, t, sc.vel);
    Elem el = pulseElem(s, j, 0);
    el.shift += sc.trans;
    const float own = emit(el, 0xFFFFu, j, v, t, frame, off, H.pPrevHz, 1.0f, sc);
    for (int32_t L = 1; L <= p.layers; ++L) {
      Elem le = el;
      le.layer = static_cast<uint8_t>(L);
      emit(le, 0xFFFFu, j, v, t, frame, off, H.pPrevHz, p.layerLevel[static_cast<size_t>(L - 1)], sc);
    }
    endUntied(t, sc.key, src, true, s);
    if (own > 0.0f) H.pPrevHz = own;
  }
  // The clocks due at `frame` of `seg` (in Hz order), then the next pending onset.
  void pulseDue(const ClockSegment& seg, uint32_t frame) noexcept {
    const int64_t at = clock == Clock::Song ? seg.pos + (frame - seg.frame) : b->time + frame;
    if (stale()) rebuild();
    // AP.8a (M4): room for every due clock's notes in one pass; the waiting ties by (clock, layer).
    const auto perClock = static_cast<uint32_t>(1 + p.layers);
    if (numGens + numHeld * perClock > kArpMaxSounding) {  // (only near a full table is the count worth it)
      uint32_t due = 0;
      for (uint32_t i = 0; i < numHeld; ++i) due += held[i].pNext <= at ? 1u : 0u;
      makeRoom(due * perClock, frame, laneOn(ArpLane::SkipTie));
    }
    if (laneOn(ArpLane::SkipTie)) buildTies();
    for (uint32_t r = 0; r < numHeld; ++r)
      if (held[sorted[r]].pNext <= at) pulseFire(sorted[r], frame);
    tieIdxValid = false;
    minPulse = kNever;
    for (uint32_t i = 0; i < numHeld; ++i) minPulse = std::min(minPulse, held[i].pNext);
  }

  // AP.8a (review M3): a new tempo map moved the song position under the playhead (Timeline::swapTempo keeps the
  // tick; StageClock::remapped): not a locate. Nothing ends: the clock keeps its grid index (pattern: the next step
  // re-seeks on the new map; pulse: every clock resumes at its first unplayed onset), and what the sounding notes have
  // left (gate, glide starts / ends) scales by the tempo ratio, so a gate still ends where the new tempo puts it.
  void remap(const ClockSegment& seg) noexcept {
    const int64_t t = b->time + seg.frame;
    const double scale = lastBpm > 0.0 && b->clock.bpm > 0.0 && std::isfinite(b->clock.bpm) ? lastBpm / b->clock.bpm : 1.0;
    auto moved = [&](int64_t x) { return x == kNever || x <= t ? x : t + std::max<int64_t>(1, std::llround(static_cast<double>(x - t) * scale)); };
    for (uint32_t i = 0; i < numGens; ++i) {
      Gen& g = gens[i];
      g.off = moved(g.off);
      if (g.inC != 0.0 && g.inT + g.inLen > t) g.inLen = moved(g.inT + g.inLen) - g.inT;  // AP.11: the curves too
      if (g.outT > t) g.outT = moved(g.outT);
      g.outLen = std::max<int64_t>(1, g.off - g.outT);
      g.intoEnd = moved(g.intoEnd);
      g.outAt = moved(g.outAt);
      g.segAt = moved(g.segAt);
    }
    recomputeMinOff();
    auto songMoved = [&](SampleTime x) { return seg.pos + static_cast<SampleTime>(std::llround(static_cast<double>(x - expectPos) * scale)); };
    if (clock == Clock::Song && !p.pulse) {
      nextSong = immediate ? std::max(seg.pos, songMoved(nextSong)) : songOnset(k);
    } else if (clock == Clock::Song) {
      for (uint32_t i = 0; i < numHeld; ++i) {
        Held& H = held[i];
        if (H.pHasLast) H.pLast = std::min(seg.pos - 1, songMoved(H.pLast));
        if (H.pImmediate) H.pNext = std::max(seg.pos, songMoved(H.pNext));
      }
      pulseStale = true;
    }
    ++stats.tempoRemaps;
  }

  // A loop wrap, a locate (stop: also drops the latched notes no key holds) at `frame`.
  void transport(uint32_t frame, bool stop) noexcept {
    if (numGens > 0 || numHeld > 0) ++stats.transportJumps;
    endAll(frame);
    if (stop) dropLatched();
    clock = Clock::None;
  }

  // AP.8a (H2): the held notes re-resolved from tables `from` to `to` (arp.h "Tuning edits"): a Degree note keeps its
  // degree and its ratio to it, a reference-relative note moves with the reference, a static note stays. RT-safe.
  void reresolve(const ArpTables& from, const ArpTables& to) noexcept {
    bool any = false;
    for (uint32_t i = 0; i < numHeld; ++i) {
      NoteEvent& on = held[i].on;
      if (!(on.tuning >= 1.0f) && !std::isfinite(on.degree)) continue;  // static (no tuning node): its Hz stays
      const ArpDegreeTable& ta = from.of(on.tuning);
      const ArpDegreeTable& tb = to.of(on.tuning);
      if (!(ta.referenceHz > 0.0) || !(tb.referenceHz > 0.0) || ta == tb) continue;
      double cents = 1200.0 * std::log2(tb.referenceHz / ta.referenceHz);
      if (std::isfinite(on.degree) && std::isfinite(on.equave)) {
        const int64_t q = std::llround(on.equave), d = std::llround(on.degree);
        cents += centsOf(tb, q * sizeOf(tb) + d) - centsOf(ta, q * sizeOf(ta) + d);
      }
      if (cents == 0.0) continue;
      const double hz = static_cast<double>(on.hz) * std::exp2(cents / 1200.0);
      if (!(hz > 0.0) || !std::isfinite(static_cast<float>(hz))) continue;
      held[i].pPrevHz = static_cast<float>(static_cast<double>(held[i].pPrevHz) * hz / static_cast<double>(on.hz));
      on.hz = static_cast<float>(hz);
      any = true;
    }
    if (any) {
      dirty = true;  // the order by Hz, a walk's elements
      pulseStale = true;
    }
  }

  // AP.8a (L1): generated note `g` (of held `h`) re-derives its pitch from h at `t` (frame `frame`): a retune Pitch
  // event (identity included) over `ramp` frames — inside its Into glide over the rest of it, with the glide's shape —
  // then a running Out glide re-aimed at its target over the rest of the note; the glide state (bend aimed at, the
  // previous / current step's Hz by rank) moves with it. `bendReset`: the source's bend was
  // reset (a producer's retune). False when nothing moved.
  bool rederive(Gen& g, const Held& h, int64_t t, uint32_t frame, uint32_t ramp, bool bendReset) noexcept {
    NoteEvent r = notePitch(frame, g.id, 0.0f, ramp);
    r.flags = kNoteEventRetune;
    const float hz = pitchOf(h.on, g.el, 0, r);
    if (!(hz > 0.0f)) return false;
    if (bendReset) g.c = 0.0;
    if (hz == g.hz && !bendReset) return false;  // (an Out offset is cents from the note: it moves with it)
    const float old = g.hz;
    g.hz = hz;
    const double bend = std::exp2(g.c / 1200.0);
    r.hz = static_cast<float>(static_cast<double>(hz) * bend);
    if (g.intoEnd != kNever && t < g.intoEnd) {  // the Into glide goes on to the new pitch, same end
      r.rampFrames = static_cast<uint32_t>(g.intoEnd - t);
      r.rampShape = static_cast<uint8_t>(p.into.ramp());
      r.rampBend = static_cast<uint8_t>(rampBendOf(p.into.curve));
    }
    out->push(r);
    if (g.outing && g.off > t)  // the Out glide re-aimed at its (retuned) target (AP.11: with the Into's rest)
      if (const double oc = outCentsOf(g, g.off); std::isfinite(oc)) {
        g.outC = oc;
        aim(g, t, frame);
      }
    if (g.rank < kArpMaxHeld && g.el.layer == 0 && old > 0.0f) {  // Into → Step of the next step starts here
      const float f = static_cast<float>(static_cast<double>(hz) / static_cast<double>(old));
      if (g.stepK == prevStepK && g.rank < prevCount) prevHz[g.rank] *= f;
      if (g.rank < curCount) curHz[g.rank] *= f;
    }
    return true;
  }

  // AP.2b: new degree tables: every sounding note whose held source is known re-derives its pitch (identity
  // included) at frame 0, gliding 5 ms like a timeline retune, so a tuning edit moves the steps at once.
  void retableNow() noexcept {
    retable = false;
    const auto g5 = static_cast<uint32_t>(std::max(1.0, 0.005 * b->clock.sampleRate));
    const uint32_t ramp = std::min<uint32_t>(g5, std::max<uint32_t>(b->frames, 1));
    for (uint32_t i = 0; i < numGens; ++i) {
      Gen& g = gens[i];
      const uint32_t s = find(g.src);
      if (s != kNone) rederive(g, held[s], b->time, 0, ramp, false);
    }
  }

  bool run(const EventList& in) noexcept {
    const NoteStageBlock& blk = *b;
    if (retable) retableNow();
    if (p.hold != hadHold) {
      hadHold = p.hold;
      if (!p.hold) dropLatched();
    }
    updateGrid();
    if (blk.clock.signatures != sigSeen) {  // AP.2b: a new signature map moves the pulse cycles
      sigSeen = blk.clock.signatures;
      pulseStale = true;
    }
    if (p.pulse != wasPulse) {  // AP.4: a mode switch re-seeks the clock
      wasPulse = p.pulse;
      clock = Clock::None;
    }
    ClockSegment whole{0, blk.frames, false, 0};
    const ClockSegment* segs = blk.clock.numSegments > 0 ? blk.clock.segments : &whole;
    const uint32_t numSegs = blk.clock.numSegments > 0 ? blk.clock.numSegments : 1u;
    const uint32_t last = blk.frames > 0 ? blk.frames - 1 : 0;
    uint32_t ei = 0;
    for (uint32_t si = 0; si < numSegs; ++si) {
      const ClockSegment& seg = segs[si];
      const bool song = seg.song && blk.clock.tempo != nullptr;
      if (prevValid && (song != prevSong || (song && seg.pos != expectPos))) {
        if (song && prevSong && si == 0 && seg.frame == 0 && blk.clock.remapped) remap(seg);  // AP.8a (M3)
        else transport(seg.frame, prevSong && !song);
      }
      prevValid = true;
      prevSong = song;
      expectPos = seg.pos + seg.len;
      const bool wantSong = song && p.rate.synced;
      if (clock != Clock::None && (clock == Clock::Song) != wantSong) clock = Clock::None;
      if (clock == Clock::Free && p.pulse && (blk.clock.bpm != pBpm || blk.clock.beatsPerBar != pBpb)) pulseStale = true;
      if (clock == Clock::Free && !p.pulse) {  // the period follows the rate / tempo; the next step stays where it is
        const double np = freePeriod();
        if (np != period) {
          t0 += static_cast<double>(k) * (period - np);
          period = np;
          nextTime = freeOnset(k);
        }
      }
      if (clock == Clock::None && numHeld > 0) startClock(seg, seg.frame);
      else if (p.pulse && clock != Clock::None && pulseStale) pulseRefresh(seg, seg.frame);
      const uint32_t end = seg.frame + seg.len;
      const bool lastSeg = si + 1 == numSegs;
      for (;;) {
        uint32_t fe = UINT32_MAX;
        if (ei < in.count) {
          const uint32_t f = std::min(in.events[ei].frame, last);
          if (f < end || lastSeg) fe = std::max(f, seg.frame);
        }
        if (pulsePending && fe != pulsePendingFrame) {  // AP.4: that frame's events are in: one refresh
          if (p.pulse && clock != Clock::None) pulseRefresh(seg, pulsePendingFrame);
          else pulsePending = false;
        }
        uint32_t fo = UINT32_MAX;
        if (minOff != kNever && minOff < blk.time + end)
          fo = static_cast<uint32_t>(std::max<int64_t>(minOff - blk.time, seg.frame));
        uint32_t fa = UINT32_MAX;
        if (minAct != kNever && minAct < blk.time + end)
          fa = static_cast<uint32_t>(std::max<int64_t>(minAct - blk.time, seg.frame));
        uint32_t fs = UINT32_MAX;
        if (p.pulse) {  // AP.4: the earliest clock
          if (clock == Clock::Song && minPulse < seg.pos + seg.len)
            fs = seg.frame + static_cast<uint32_t>(std::max<int64_t>(minPulse - seg.pos, 0));
          else if (clock == Clock::Free && minPulse < blk.time + end)
            fs = static_cast<uint32_t>(std::max<int64_t>(minPulse - blk.time, seg.frame));
        } else if (clock == Clock::Song) {
          if (nextSong < seg.pos + seg.len) fs = seg.frame + static_cast<uint32_t>(std::max<SampleTime>(nextSong - seg.pos, 0));
        } else if (clock == Clock::Free) {
          if (nextTime < blk.time + end) fs = static_cast<uint32_t>(std::max<int64_t>(nextTime - blk.time, seg.frame));
        }
        const uint32_t m = std::min({fe, fo, fa, fs});
        if (m == UINT32_MAX || (m >= end && !(m == fe && lastSeg))) break;
        if (m == fo) {
          endDue(blk.time + m, m);
        } else if (m == fa) {
          actDue(blk.time + m, m);
        } else if (m == fe) {
          handle(in.events[ei++], seg, m);
        } else if (p.pulse) {
          pulseDue(seg, m);
        } else {
          step(seg, m);
        }
      }
    }
    lastBpm = blk.clock.bpm;
    return numHeld > 0 || numGens > 0;
  }
};

// ---- ArpCore ------------------------------------------------------------------------------------------------------

ArpCore::ArpCore(uint32_t stageIndex, ArpTables tables)
    : s_(std::make_unique<State>(std::move(tables))), stageIndex_(stageIndex) {}
ArpCore::~ArpCore() = default;

bool ArpCore::process(const ArpParams& params, const NoteStageBlock& block, const EventList& in,
                      EventBuffer& out) noexcept {  // RT-safe
  State& s = *s_;
  if (s.box != nullptr && s.box->pending())  // AP.2b: a tuning edit's degree tables, taken at a block start
    if (const ArpTables* t = s.box->take()) {
      s.reresolve(*s.tabs, *t);  // AP.8a (H2): the old tables stay valid through this block (ArpTableMailbox)
      s.tabs = t;
      s.retable = true;
    }
  if (in.count == 0 && s.numHeld == 0 && s.numGens == 0) {  // idle: nothing held, nothing sounding
    s.prevValid = false;
    s.clock = State::Clock::None;
    return false;
  }
  // AP.3: the params are sanitized only when they change. Equal bytes are equal params (the fast path, most blocks);
  // AP.8a (L5): differing bytes compare memberwise, so padding never forces a re-sanitize.
  if (!s.rawValid || (std::memcmp(&params, &s.raw, sizeof(ArpParams)) != 0 && !(params == s.raw))) {
    std::memcpy(&s.raw, &params, sizeof(ArpParams));
    s.rawValid = true;
    s.p = sanitized(params);
    s.pulseStale = true;  // AP.4: re-derive the pulse clocks at the next segment
  }
  s.b = &block;
  s.out = &out;
  s.stageIndex = stageIndex_;
  const bool busy = s.run(in);
  if (s.status) s.statusBlock();  // AP.6
  return busy;
}

void ArpCore::flush(uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept {  // RT-safe
  State& s = *s_;
  // AP.8a (M2): the key-held notes first, then the offs (kNoteEventGone): a stage that adopts the notes this one fed
  // gets the new ones before it loses the old ones (its held set never runs empty: its pattern does not restart).
  for (uint32_t i = 0; i < s.numHeld; ++i) {
    if (!s.held[i].key) continue;
    NoteEvent on = s.held[i].on;
    on.frame = frame;
    held.push(on);
  }
  s.out = &out;
  s.endAll(frame, kNoteEventGone);
  s.numHeld = s.keysDown = 0;
  s.clock = State::Clock::None;
  s.prevValid = false;
  if (s.status) s.statusBlock();  // AP.6: idle
}

void ArpCore::adopt(ArpCore& old) noexcept {  // RT-safe
  std::swap(s_, old.s_);  // this core now runs old's state; then each takes its own structure back
  State& a = *s_;
  State& b = *old.s_;
  const ArpTables* ta = a.tabs == &a.tables ? nullptr : a.tabs;
  const ArpTables* tb = b.tabs == &b.tables ? nullptr : b.tabs;
  std::swap(a.tables, b.tables);
  std::swap(a.box, b.box);
  a.tabs = tb != nullptr ? tb : &a.tables;
  b.tabs = ta != nullptr ? ta : &b.tables;
  a.reresolve(*b.tabs, *a.tabs);  // AP.8a (H2): the running notes into the new stage's tuning (the old one is alive)
  std::swap(a.laneFixed, b.laneFixed);
  std::swap(a.laneSlot, b.laneSlot);
  std::swap(a.laneScale, b.laneScale);
  std::swap(a.laneOffset, b.laneOffset);
  std::swap(a.laneModBegin, b.laneModBegin);  // AP.8b (M8)
  std::swap(a.laneModCount, b.laneModCount);
  std::swap(a.laneMods, b.laneMods);
  std::swap(a.status, b.status);  // AP.6
  a.patternNew = true;
  a.rawValid = false;  // the new stage's params: re-sanitized at the next block
  a.pulseStale = true;
  a.retable = true;    // its table may differ: the sounding notes re-derive their pitch
  b.numHeld = b.keysDown = b.numGens = 0;
}

void ArpCore::setLaneSteps(const ArpLaneSteps& steps) noexcept { s_->laneFixed = steps; }

void ArpCore::mapLaneStep(ArpLane lane, int32_t step, uint32_t param, float scale, float offset) noexcept {
  const auto l = static_cast<size_t>(lane);
  if (l >= kArpLaneCount || step < 0 || step >= kArpMaxLaneSteps) return;
  s_->laneSlot[l][static_cast<size_t>(step)] = param;
  s_->laneScale[l] = scale;
  s_->laneOffset[l] = offset;
}

void ArpCore::modLaneStep(ArpLane lane, int32_t step, std::span<const uint32_t> buses) {
  const auto l = static_cast<size_t>(lane);
  if (l >= kArpLaneCount || step < 0 || step >= kArpMaxLaneSteps || buses.empty()) return;
  State& s = *s_;
  const auto i = static_cast<size_t>(step);
  s.laneModBegin[l][i] = static_cast<uint16_t>(s.laneMods.size());
  s.laneModCount[l][i] = static_cast<uint16_t>(buses.size());
  s.laneMods.insert(s.laneMods.end(), buses.begin(), buses.end());
}

void ArpCore::setTableMailbox(std::shared_ptr<ArpTableMailbox> box) noexcept { s_->box = std::move(box); }

void ArpCore::setStatus(std::shared_ptr<NoteStageStatus> status) noexcept {
  s_->status = std::move(status);
  s_->patternNew = true;
  s_->statusIdle = false;
}

void ArpCore::setTrace(std::vector<ArpTraceNote>* trace) noexcept { s_->trace = trace; }  // AP.7

uint32_t ArpCore::heldCount() const noexcept { return s_->numHeld; }
double ArpCore::cycleSteps() const noexcept { return s_->cycleSteps(); }  // AP.10
bool ArpCore::idle() const noexcept { return s_->numHeld == 0 && s_->numGens == 0; }
void ArpCore::setNoteCounter(uint32_t n) noexcept { s_->nextId = n; }  // AP.8a: tests (the 2^24 wrap)
uint32_t ArpCore::soundingCount() const noexcept { return s_->numGens; }
const ArpStats& ArpCore::stats() const noexcept { return s_->stats; }

// ---- ArpStage -----------------------------------------------------------------------------------------------------

ArpStage::ArpStage(const ArpParams& params, std::vector<Mapping> map, ArpTables tables, const ArpLaneSteps& steps,
                   std::shared_ptr<ArpTableMailbox> box, std::shared_ptr<NoteStageStatus> status)
    : params_(params), core_(0, std::move(tables)) {
  core_.setStatus(std::move(status));
  processFn = &ArpStage::processStage;
  flushFn = &ArpStage::flushStage;
  adoptFn = &ArpStage::adoptStage;
  core_.setLaneSteps(steps);
  core_.setTableMailbox(std::move(box));
  // AP.5: lane steps are read by the core when their step plays; the rest at every block start, each id's base
  // mapping followed by its add mappings (AP.2b: modulation, in the param's units).
  std::vector<Mapping> base, add;
  std::vector<uint32_t> buses;
  for (size_t k = 0; k < map.size(); ++k) {
    const Mapping& m = map[k];
    if (isArpLaneStep(m.id)) {
      const uint32_t i = static_cast<uint32_t>(m.id) - static_cast<uint32_t>(ArpParamId::LaneStep0);
      const auto lane = static_cast<ArpLane>(i / kArpMaxLaneSteps);
      const auto step = static_cast<int32_t>(i % kArpMaxLaneSteps);
      if (m.add) continue;  // (taken with the step's base mapping below)
      core_.mapLaneStep(lane, step, m.param, m.scale, m.offset);
      // AP.8b (review M8): the cables into this step (its add mappings anywhere in the list).
      buses.clear();
      for (const Mapping& a : map)
        if (a.add && a.id == m.id) buses.push_back(a.param);
      core_.modLaneStep(lane, step, buses);
      continue;
    }
    (m.add ? add : base).push_back(m);
  }
  for (const Mapping& b : base) {
    map_.push_back(b);
    for (const Mapping& a : add)
      if (a.id == b.id) map_.push_back(a);
  }
}

bool ArpStage::processStage(void* self, const NoteStageBlock& block, const EventList& in, EventBuffer& out) noexcept {  // RT-safe
  auto& st = *static_cast<ArpStage*>(self);
  // AP.8a (L5): an idle arp (no event, nothing held or sounding) maps nothing: the core returns after its table check.
  if (st.map_.empty() || (in.count == 0 && st.core_.idle())) return st.core_.process(st.params_, block, in, out);
  ArpParams p = st.params_;
  const size_t n = st.map_.size();
  for (size_t i = 0; i < n;) {
    const Mapping& m = st.map_[i++];
    const bool have = m.param < block.params.size();
    float v = have ? m.offset + m.scale * arpTaper(m.taper, block.params[m.param]) : 0.0f;
    for (; i < n && st.map_[i].add; ++i)
      if (st.map_[i].param < block.mods.size()) v += block.mods[st.map_[i].param];
    if (have) setArpParam(p, m.id, v);
  }
  return st.core_.process(p, block, in, out);
}

void ArpStage::adoptStage(void* self, void* old) noexcept {  // RT-safe
  static_cast<ArpStage*>(self)->core_.adopt(static_cast<ArpStage*>(old)->core_);
}

void ArpStage::flushStage(void* self, uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept {  // RT-safe
  static_cast<ArpStage*>(self)->core_.flush(frame, out, held);
}

}  // namespace pq::engine
