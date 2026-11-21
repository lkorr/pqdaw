#include "pq/engine/arp.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace pq::engine {

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
    case ArpParamId::Unit: p.unit = static_cast<ArpUnit>(clampi(v, 0, 1)); break;
    case ArpParamId::Transpose: p.transpose = static_cast<ArpTranspose>(clampi(v, 0, 1)); break;
    case ArpParamId::Distance: p.distance = clampi(v, -64, 64); break;
    case ArpParamId::Steps: p.steps = clampi(v, 1, kArpMaxSteps); break;
    case ArpParamId::RatioP: p.ratioP = clampi(v, 1, 64); break;
    case ArpParamId::RatioQ: p.ratioQ = clampi(v, 1, 64); break;
    case ArpParamId::VelocitySource: p.velocitySource = static_cast<ArpVelocitySource>(clampi(v, 0, 1)); break;
    case ArpParamId::FixedVelocity: p.fixedVelocity = v; break;
    case ArpParamId::VelocityDecayMs: p.velocityDecayMs = v; break;
    case ArpParamId::VelocityTarget: p.velocityTarget = v; break;
    case ArpParamId::VelocityRetrigger: p.velocityRetrigger = on; break;
    case ArpParamId::Seed: p.seed = static_cast<uint32_t>(clampi(v, 0, 1 << 24)); break;
    case ArpParamId::Count: break;
  }
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
  if (static_cast<uint32_t>(p.unit) > 1) p.unit = ArpUnit::Degrees;
  if (static_cast<uint32_t>(p.transpose) > 1) p.transpose = ArpTranspose::Shift;
  if (static_cast<uint32_t>(p.velocitySource) > 1) p.velocitySource = ArpVelocitySource::AsPlayed;
  p.distance = std::clamp(p.distance, -64, 64);
  p.steps = std::clamp(p.steps, 1, kArpMaxSteps);
  p.ratioP = std::clamp(p.ratioP, 1, 64);
  p.ratioQ = std::clamp(p.ratioQ, 1, 64);
  p.fixedVelocity = unit(p.fixedVelocity, 0.0f, 1.0f, 0.8f);
  p.velocityDecayMs = unit(p.velocityDecayMs, 0.0f, 60000.0f, 0.0f);
  p.velocityTarget = unit(p.velocityTarget, 0.0f, 1.0f, 0.0f);
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
    double centsAt(int64_t t) const noexcept {
      if (cLen == 0 || t >= cT + cLen) return c1;
      return c0 + (c1 - c0) * static_cast<double>(t - cT) / cLen;
    }
    float gainAt(int64_t t) const noexcept {
      if (gLen == 0 || t >= gT + gLen) return g1;
      return g0 + (g1 - g0) * static_cast<float>(static_cast<double>(t - gT) / gLen);
    }
  };
  struct Gen {
    uint32_t id, src;
    int32_t shift;  // Degrees: degrees; Ratio: the exponent of p/q
    float hz;       // its pitch without the source's bend
    int64_t off;    // engine time of its note-off
  };
  enum class Clock : uint8_t { None, Song, Free };

  explicit State(ArpDegreeTable t) : table(std::move(t)) {
    if (table.cents.empty()) {
      table.cents.resize(12);
      for (int i = 0; i < 12; ++i) table.cents[static_cast<size_t>(i)] = 100.0 * i;
      table.equaveCents = 1200.0;
    }
    if (!(table.equaveCents > 0.0) || !std::isfinite(table.equaveCents)) table.equaveCents = 1200.0;
  }

  ArpDegreeTable table;
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
  int64_t minOff = kNever;
  uint32_t nextId = 0;

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
  // Grid cache.
  ArpRate gridRate{}, gridRetrig{};
  bool gridValid = false;
  int64_t D = 1, Lsub = 1, Rsub = 1;  // the step and retrigger lengths in 1/D ticks
  bool hadHold = false;

  ArpParams p;
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
    if (numHeld == 0) clock = Clock::None;
  }
  void dropLatched() noexcept {
    for (uint32_t i = 0; i < numHeld;)
      if (!held[i].key) remove(i);
      else ++i;
  }

  void rebuild() noexcept {
    dirty = false;
    const uint32_t n = numHeld;
    for (uint32_t i = 0; i < n; ++i) sorted[i] = arrival[i] = static_cast<uint8_t>(i);
    std::sort(sorted.begin(), sorted.begin() + n, [&](uint8_t a, uint8_t b) {
      const float ha = held[a].on.hz, hb = held[b].on.hz;
      return ha < hb || (ha == hb && held[a].order < held[b].order);
    });
    std::sort(arrival.begin(), arrival.begin() + n, [&](uint8_t a, uint8_t b) { return held[a].order < held[b].order; });
    buildPattern(n);
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

  void shuffle(int64_t step) noexcept {
    const uint32_t n = numHeld;
    for (uint32_t i = 0; i < n; ++i) perm[i] = static_cast<uint8_t>(i);
    for (uint32_t i = n; i-- > 1;) std::swap(perm[i], perm[draw(p.seed, step, i) % (i + 1)]);
    permValid = true;
  }

  // ---- Pitch units ----
  int32_t tableSize() const noexcept { return static_cast<int32_t>(table.cents.size()); }
  double centsOf(int64_t deg) const noexcept {
    const int64_t N = tableSize();
    const int64_t q = floorDiv(deg, N);
    return static_cast<double>(q) * table.equaveCents + table.cents[static_cast<size_t>(deg - q * N)];
  }
  // The pitch of held note `h` shifted by `shift` (in the unit): fills e's hz and identity. Returns the Hz.
  float pitch(const NoteEvent& src, int32_t shift, NoteEvent& e) const noexcept {
    e.hz = src.hz;
    e.ratio = src.ratio;
    e.harmonic = src.harmonic;
    e.degree = src.degree;
    e.equave = src.equave;
    e.tuning = src.tuning;
    e.noteDegree = src.noteDegree;
    e.unisonCount = src.unisonCount;
    e.unisonIndex = src.unisonIndex;
    if (shift == 0) return e.hz;
    const int64_t N = tableSize();
    if (p.unit == ArpUnit::Degrees) {
      const bool deg = std::isfinite(src.degree) && std::isfinite(src.equave);
      const int64_t d0 = deg ? static_cast<int64_t>(std::llround(src.equave)) * N + std::llround(src.degree)
                             : (std::isfinite(src.noteDegree) ? std::llround(src.noteDegree) : 0);
      const int64_t d1 = d0 + shift;
      const double r = std::exp2((centsOf(d1) - centsOf(d0)) / 1200.0);
      e.hz = static_cast<float>(static_cast<double>(src.hz) * r);
      e.ratio = static_cast<float>(static_cast<double>(src.ratio) * r);
      e.harmonic = 0.0f;
      if (deg) {
        const int64_t q = floorDiv(d1, N);
        e.equave = static_cast<float>(q);
        e.degree = static_cast<float>(d1 - q * N);
      } else {
        e.degree = e.equave = kNaN;
        e.tuning = 0.0f;
      }
      if (std::isfinite(src.noteDegree)) e.noteDegree = src.noteDegree + static_cast<float>(shift);
      return e.hz;
    }
    // Ratio: (p/q)^shift, exact while it fits.
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
      if (h <= (int64_t{1} << 46) / num && (h * num) % den == 0) e.harmonic = static_cast<float>(h * num / den);
    }
    e.degree = e.equave = kNaN;
    e.tuning = 0.0f;
    e.noteDegree = std::isfinite(src.noteDegree)
                       ? src.noteDegree + static_cast<float>(1200.0 * std::log2(r) * N / table.equaveCents)
                       : kNaN;
    return e.hz;
  }

  // ---- Generated notes ----
  void recomputeMinOff() noexcept {
    minOff = kNever;
    for (uint32_t i = 0; i < numGens; ++i) minOff = std::min(minOff, gens[i].off);
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
  void endAll(uint32_t frame) noexcept {
    for (uint32_t i = 0; i < numGens; ++i) out->push(noteOff(frame, gens[i].id));
    numGens = 0;
    minOff = kNever;
  }

  // ---- Clock ----
  // The song sample of 1/D-tick position `sub` + `extra` (fractional 1/D ticks), through the tempo map.
  double songAt(int64_t sub, double extra) const noexcept {
    int64_t q = floorDiv(sub, D);
    double frac = (static_cast<double>(sub - q * D) + extra) / static_cast<double>(D);
    if (frac >= 1.0) {
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
  // it; Free, now).
  void startClock(const ClockSegment& seg, uint32_t frame) noexcept {
    if (p.rate.synced && seg.song && b->clock.tempo != nullptr) {
      clock = Clock::Song;
      const SampleTime at = seg.pos + (frame - seg.frame);
      const double ticks = b->clock.tempo->samplesToTicks(at, b->clock.sampleRate);
      int64_t step = static_cast<int64_t>(std::floor(ticks * static_cast<double>(D) / static_cast<double>(Lsub))) - 1;
      while (songOnset(step) < at) ++step;
      while (songOnset(step - 1) >= at) --step;
      k = step;
      nextSong = songOnset(k);
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
        if (clock == Clock::None) {
          startClock(seg, frame);
          resetPattern(t, true);
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
        } else if (p.hold) {
          if (held[i].key) --keysDown;
          held[i].key = false;
        } else {
          remove(i);
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
          h.on.hz = e.hz;
          h.on.ratio = e.ratio;
          h.on.harmonic = e.harmonic;
          h.on.degree = e.degree;
          h.on.equave = e.equave;
          h.on.tuning = e.tuning;
          h.c0 = h.c1 = 0.0;
          h.cLen = 0;
          dirty = true;
          for (uint32_t g = 0; g < numGens; ++g) {
            if (gens[g].src != e.noteId) continue;
            NoteEvent r = notePitch(frame, gens[g].id, 0.0f, e.rampFrames);
            r.flags = kNoteEventRetune;
            gens[g].hz = pitch(h.on, gens[g].shift, r);
            pushExpression(r);
          }
          return;
        }
        if (!(e.hz > 0.0f) || !(h.on.hz > 0.0f)) return;
        h.c0 = h.centsAt(t);
        h.c1 = 1200.0 * std::log2(static_cast<double>(e.hz) / static_cast<double>(h.on.hz));
        h.cT = t;
        h.cLen = e.rampFrames;
        const double r = std::exp2(h.c1 / 1200.0);
        for (uint32_t g = 0; g < numGens; ++g)
          if (gens[g].src == e.noteId)
            pushExpression(notePitch(frame, gens[g].id, static_cast<float>(gens[g].hz * r), e.rampFrames));
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
          if (gens[g].src == e.noteId) pushExpression(noteGain(frame, gens[g].id, h.g1, e.rampFrames));
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
        minOff = kNever;
        clock = Clock::None;
        dirty = true;
        permValid = false;
        return;
      }
    }
  }

  // ---- Steps ----
  void emit(uint32_t slot, int32_t shift, float velocity, int64_t t, uint32_t frame, int64_t off) noexcept {
    const Held& h = held[slot];
    if (numGens == kArpMaxSounding) {  // the earliest-ending one makes room
      uint32_t m = 0;
      for (uint32_t i = 1; i < numGens; ++i)
        if (gens[i].off < gens[m].off) m = i;
      out->push(noteOff(frame, gens[m].id));
      gens[m] = gens[--numGens];
      ++stats.cutSounding;
    }
    NoteEvent on;
    on.frame = frame;
    on.kind = NoteEventKind::NoteOn;
    on.noteId = stageNoteId(stageIndex, nextId++);
    on.velocity = velocity;
    const float hz = pitch(h.on, shift, on);
    const double c = h.centsAt(t);
    on.hz = c != 0.0 ? static_cast<float>(hz * std::exp2(c / 1200.0)) : hz;
    on.gain = h.gainAt(t);
    out->push(on);
    if (h.cLen != 0 && t < h.cT + h.cLen)
      out->push(notePitch(frame, on.noteId, static_cast<float>(hz * std::exp2(h.c1 / 1200.0)),
                          static_cast<uint32_t>(h.cT + h.cLen - t)));
    if (h.gLen != 0 && t < h.gT + h.gLen)
      out->push(noteGain(frame, on.noteId, h.g1, static_cast<uint32_t>(h.gT + h.gLen - t)));
    gens[numGens++] = Gen{on.noteId, h.on.noteId, shift, hz, off};
    minOff = std::min(minOff, off);
    ++stats.notes;
  }

  // Plays element (list index `idx`, copy `j`) of the cycle.
  void playElement(uint32_t idx, int32_t j, float velocity, int64_t t, uint32_t frame, int64_t off) noexcept {
    const uint8_t* list = p.style == ArpStyle::PlayOrder ? arrival.data() : sorted.data();
    const auto n = static_cast<int64_t>(numHeld);
    if (p.transpose == ArpTranspose::Shift) {
      emit(list[idx], j * p.distance, velocity, t, frame, off);
      return;
    }
    const int64_t r = static_cast<int64_t>(idx) + static_cast<int64_t>(j) * p.distance;
    const int64_t w = floorDiv(r, n);
    const auto i2 = static_cast<uint32_t>(r - w * n);
    const auto shift = static_cast<int32_t>(p.unit == ArpUnit::Degrees ? w * tableSize() : w);
    emit(list[i2], shift, velocity, t, frame, off);
  }

  void step(const ClockSegment& seg, uint32_t frame) noexcept {
    const int64_t t = b->time + frame;
    const int64_t kk = k;
    int64_t off;
    if (clock == Clock::Song) {
      const SampleTime onS = nextSong;
      const SampleTime offS = roundSample(songAt(kk * Lsub, grooveSub(kk) + static_cast<double>(p.gate) * static_cast<double>(Lsub)));
      off = t + std::max<int64_t>(1, offS - onS);
      ++k;
      nextSong = songOnset(k);
    } else {
      off = t + std::max<int64_t>(1, std::llround(static_cast<double>(p.gate) * period));
      ++k;
      nextTime = freeOnset(k);
    }
    (void)seg;
    if (numHeld == 0) {
      clock = Clock::None;
      return;
    }
    if (dirty || builtStyle != p.style) rebuild();
    if (p.retrigger == ArpRetrigger::Beat) {
      const int64_t beat = beatIndex(kk);
      if (beat != lastBeat) {
        lastBeat = beat;
        resetPattern(t, false);
      }
    }
    const uint32_t n = numHeld;
    const bool chord = p.style == ArpStyle::ChordTrigger;
    const int64_t P = chord ? 1 : (p.style >= ArpStyle::Random ? static_cast<int64_t>(n) : patternLen);
    const int64_t cycle = P * p.steps;
    const int64_t here = pos++;
    if (p.repeats > 0 && here >= static_cast<int64_t>(p.repeats) * cycle) return;
    int64_t c = (here + p.offset) % cycle;
    if (c < 0) c += cycle;
    const auto j = static_cast<int32_t>(c / P);
    const auto e = static_cast<uint32_t>(c % P);
    float v = 0.0f;
    ++stats.steps;
    for (uint32_t m = 0; m < (chord ? n : 1u); ++m) {
      uint32_t idx = m;
      switch (p.style) {
        case ArpStyle::ChordTrigger: break;
        case ArpStyle::Random: idx = static_cast<uint32_t>(draw(p.seed, kk, 0) % n); break;
        case ArpStyle::RandomOther:
          if (e == 0 || !permValid) shuffle(kk);
          idx = perm[e];
          break;
        case ArpStyle::RandomOnce:
          if (!permValid) shuffle(kk);
          idx = perm[e];
          break;
        default: idx = pattern[e]; break;
      }
      const uint8_t* list = p.style == ArpStyle::PlayOrder ? arrival.data() : sorted.data();
      const Held& h = held[list[idx]];
      v = p.velocitySource == ArpVelocitySource::Fixed ? p.fixedVelocity : h.on.velocity;
      if (p.velocityDecayMs > 0.0f) {
        const double len = static_cast<double>(p.velocityDecayMs) * 1e-3 * b->clock.sampleRate;
        const double f = std::min(1.0, static_cast<double>(t - decayStart) / len);
        v = static_cast<float>(v + (p.velocityTarget - v) * f);
      }
      playElement(idx, j, v, t, frame, off);
    }
  }

  // A loop wrap, a locate (stop: also drops the latched notes no key holds) at `frame`.
  void transport(uint32_t frame, bool stop) noexcept {
    if (numGens > 0 || numHeld > 0) ++stats.transportJumps;
    endAll(frame);
    if (stop) dropLatched();
    clock = Clock::None;
  }

  bool run(const EventList& in) noexcept {
    const NoteStageBlock& blk = *b;
    if (p.hold != hadHold) {
      hadHold = p.hold;
      if (!p.hold) dropLatched();
    }
    updateGrid();
    ClockSegment whole{0, blk.frames, false, 0};
    const ClockSegment* segs = blk.clock.numSegments > 0 ? blk.clock.segments : &whole;
    const uint32_t numSegs = blk.clock.numSegments > 0 ? blk.clock.numSegments : 1u;
    const uint32_t last = blk.frames > 0 ? blk.frames - 1 : 0;
    uint32_t ei = 0;
    for (uint32_t si = 0; si < numSegs; ++si) {
      const ClockSegment& seg = segs[si];
      const bool song = seg.song && blk.clock.tempo != nullptr;
      if (prevValid && (song != prevSong || (song && seg.pos != expectPos))) transport(seg.frame, prevSong && !song);
      prevValid = true;
      prevSong = song;
      expectPos = seg.pos + seg.len;
      const bool wantSong = song && p.rate.synced;
      if (clock != Clock::None && (clock == Clock::Song) != wantSong) clock = Clock::None;
      if (clock == Clock::Free) {  // the period follows the rate / tempo; the next step stays where it is
        const double np = freePeriod();
        if (np != period) {
          t0 += static_cast<double>(k) * (period - np);
          period = np;
          nextTime = freeOnset(k);
        }
      }
      if (clock == Clock::None && numHeld > 0) startClock(seg, seg.frame);
      const uint32_t end = seg.frame + seg.len;
      const bool lastSeg = si + 1 == numSegs;
      for (;;) {
        uint32_t fe = UINT32_MAX;
        if (ei < in.count) {
          const uint32_t f = std::min(in.events[ei].frame, last);
          if (f < end || lastSeg) fe = std::max(f, seg.frame);
        }
        uint32_t fo = UINT32_MAX;
        if (minOff != kNever && minOff < blk.time + end)
          fo = static_cast<uint32_t>(std::max<int64_t>(minOff - blk.time, seg.frame));
        uint32_t fs = UINT32_MAX;
        if (clock == Clock::Song) {
          if (nextSong < seg.pos + seg.len) fs = seg.frame + static_cast<uint32_t>(std::max<SampleTime>(nextSong - seg.pos, 0));
        } else if (clock == Clock::Free) {
          if (nextTime < blk.time + end) fs = static_cast<uint32_t>(std::max<int64_t>(nextTime - blk.time, seg.frame));
        }
        const uint32_t m = std::min({fe, fo, fs});
        if (m == UINT32_MAX || (m >= end && !(m == fe && lastSeg))) break;
        if (m == fo) {
          endDue(blk.time + m, m);
        } else if (m == fe) {
          handle(in.events[ei++], seg, m);
        } else {
          step(seg, m);
        }
      }
    }
    return numHeld > 0 || numGens > 0;
  }
};

// ---- ArpCore ------------------------------------------------------------------------------------------------------

ArpCore::ArpCore(uint32_t stageIndex, ArpDegreeTable table)
    : s_(std::make_unique<State>(std::move(table))), stageIndex_(stageIndex) {}
ArpCore::~ArpCore() = default;

bool ArpCore::process(const ArpParams& params, const NoteStageBlock& block, const EventList& in,
                      EventBuffer& out) noexcept {  // RT-safe
  State& s = *s_;
  if (in.count == 0 && s.numHeld == 0 && s.numGens == 0) {  // idle: nothing held, nothing sounding
    s.prevValid = false;
    s.clock = State::Clock::None;
    return false;
  }
  s.p = sanitized(params);
  s.b = &block;
  s.out = &out;
  s.stageIndex = stageIndex_;
  return s.run(in);
}

void ArpCore::flush(uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept {  // RT-safe
  State& s = *s_;
  s.out = &out;
  s.endAll(frame);
  for (uint32_t i = 0; i < s.numHeld; ++i) {
    if (!s.held[i].key) continue;
    NoteEvent on = s.held[i].on;
    on.frame = frame;
    held.push(on);
  }
  s.numHeld = s.keysDown = 0;
  s.clock = State::Clock::None;
  s.prevValid = false;
}

uint32_t ArpCore::heldCount() const noexcept { return s_->numHeld; }
uint32_t ArpCore::soundingCount() const noexcept { return s_->numGens; }
const ArpStats& ArpCore::stats() const noexcept { return s_->stats; }

// ---- ArpStage -----------------------------------------------------------------------------------------------------

ArpStage::ArpStage(const ArpParams& params, std::vector<Mapping> map, ArpDegreeTable table)
    : params_(params), map_(std::move(map)), core_(0, std::move(table)) {
  processFn = &ArpStage::processStage;
  flushFn = &ArpStage::flushStage;
}

bool ArpStage::processStage(void* self, const NoteStageBlock& block, const EventList& in, EventBuffer& out) noexcept {  // RT-safe
  auto& st = *static_cast<ArpStage*>(self);
  if (st.map_.empty()) return st.core_.process(st.params_, block, in, out);
  ArpParams p = st.params_;
  for (const Mapping& m : st.map_)
    if (m.param < block.params.size()) setArpParam(p, m.id, block.params[m.param]);
  return st.core_.process(p, block, in, out);
}

void ArpStage::flushStage(void* self, uint32_t frame, EventBuffer& out, EventBuffer& held) noexcept {  // RT-safe
  static_cast<ArpStage*>(self)->core_.flush(frame, out, held);
}

}  // namespace pq::engine
