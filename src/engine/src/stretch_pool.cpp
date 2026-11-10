// The stretch pool (T6.5): pq/engine/stretch_pool.h. The only TU that includes signalsmith-stretch (its warnings are
// off for this file, src/engine/CMakeLists.txt).
#include "pq/engine/stretch_pool.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "signalsmith-stretch/signalsmith-stretch.h"

namespace pq::engine {

namespace {

// The library's generator (std::uniform_real_distribution draws from it beyond a 2× stretch). Its state lives in the
// pool's slot: the pool points tRng at it around every call into the stretcher, so a claim can reseed it (the
// library's own engine member is private) and nothing is shared between stretchers.
thread_local uint64_t* tRng = nullptr;

struct PoolRng {
  using result_type = uint32_t;
  explicit PoolRng(long /*seed*/) noexcept {}
  static constexpr result_type min() noexcept { return 0; }
  static constexpr result_type max() noexcept { return 0xFFFFFFFFu; }
  result_type operator()() noexcept {  // RT-safe: splitmix64
    uint64_t z = (*tRng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return static_cast<result_type>((z ^ (z >> 31)) >> 32);
  }
};

using Stretch = signalsmith::stretch::SignalsmithStretch<float, PoolRng>;

struct In {  // in[c][k] = frame `lead` + k; frames before `lead` read as zeros
  const float* const* p;
  int lead;
  struct Ch {
    const float* p;
    int lead;
    float operator[](int i) const noexcept { return i < lead ? 0.0f : p[i - lead]; }
  };
  Ch operator[](int c) const noexcept { return {p[c], lead}; }
};

}  // namespace

struct StretchPool::Slot {
  Stretch s{0};
  uint64_t rng = 0;
};

StretchPool::StretchPool(uint32_t voices, double sampleRate, uint32_t monoVoices) : stereo_(voices) {
  const auto sr = static_cast<float>(sampleRate >= 1000.0 ? sampleRate : 48000.0);
  slots_.reserve(voices + monoVoices);
  free_.reserve(voices);
  freeMono_.reserve(monoVoices);
  for (uint32_t i = 0; i < voices + monoVoices; ++i) {
    auto slot = std::make_unique<Slot>();
    slot->s.presetDefault(i < voices ? 2 : 1, sr, true);
    slots_.push_back(std::move(slot));
  }
  for (uint32_t i = voices; i-- > 0;) free_.push_back(i);
  for (uint32_t i = voices + monoVoices; i-- > voices;) freeMono_.push_back(i);
  // The configuration's buffers (an estimate from the preset's sizes: signalsmith's members are private). Per
  // stretcher: the STFT's input history (block + interval, ×2 with the stashed copy), output (block + window products,
  // ×2), spectra and FFT work buffers (~4 × block complex), and per band (block / 2 + 1, two channels): 2 Band (28 B),
  // a Prediction (12 B), and ~6 floats of maps and energies.
  Stretch probe{0};
  probe.presetDefault(2, sr, true);
  const auto block = static_cast<size_t>(probe.blockSamples()), interval = static_cast<size_t>(probe.intervalSamples());
  const size_t bands = block / 2 + 1;
  auto perStretcher = [&](size_t ch) {
    const size_t floats = ch * 2 * (block + interval) + ch * (2 * block + block) + 4 * block * ch + block + interval +
                          static_cast<size_t>(probe.outputLatency()) * ch;
    return floats * sizeof(float) + bands * ch * (28 + 12) + bands * 6 * sizeof(float);
  };
  memory_ = static_cast<size_t>(voices) * perStretcher(2) + static_cast<size_t>(monoVoices) * perStretcher(1);
  latency_ = {probe.inputLatency(), probe.outputLatency(), probe.seekLength(), probe.blockSamples()};
}

StretchPool::Latency StretchPool::latencyFor(double sampleRate) noexcept {  // RT-safe
  // presetDefault: block 0.12 s, interval 0.03 s; the Kaiser window's analysis / synthesis offsets are half a block;
  // split computation delays the output by one more interval.
  const auto sr = static_cast<float>(sampleRate >= 1000.0 ? sampleRate : 48000.0);
  const auto block = static_cast<int>(sr * 0.12f), interval = static_cast<int>(sr * 0.03f);
  return {block - block / 2, block / 2 + interval, block + interval, block};
}

StretchPool::~StretchPool() = default;

uint32_t StretchPool::claim(uint32_t seed, uint32_t channels) noexcept {  // RT-safe
  std::vector<uint32_t>& list = channels == 1 && !freeMono_.empty() ? freeMono_ : free_;
  if (list.empty()) return kNone;
  const uint32_t i = list.back();
  list.pop_back();
  reset(i, seed);
  return i;
}

void StretchPool::reset(uint32_t i, uint32_t seed) noexcept {  // RT-safe
  Slot& sl = *slots_[i];
  sl.rng = 0x5EED5EED00000000ull ^ seed;
  tRng = &sl.rng;
  sl.s.reset();
  sl.s.setTransposeFactor(1.0f);
  sl.s.setFormantFactor(1.0f, false);
  sl.s.setFormantBase(0.0f);
}

void StretchPool::release(uint32_t i) noexcept {  // RT-safe
  if (i < slots_.size()) (i < stereo_ ? free_ : freeMono_).push_back(i);
}

void StretchPool::setParams(uint32_t i, const StretchParams& p) noexcept {  // RT-safe
  Stretch& s = slots_[i]->s;
  s.setTransposeFactor(static_cast<float>(p.transpose));
  s.setFormantFactor(static_cast<float>(p.formant), p.compensate);
  s.setFormantBase(static_cast<float>(p.formantBase));
}

void StretchPool::seek(uint32_t i, const float* const* in, int frames, double rate) noexcept {  // RT-safe
  Slot& sl = *slots_[i];
  tRng = &sl.rng;
  sl.s.seek(In{in, 0}, frames, rate);
}

void StretchPool::process(uint32_t i, const float* const* in, int inFrames, int inLead, float* const* out,
                          int outFrames) noexcept {  // RT-safe
  Slot& sl = *slots_[i];
  tRng = &sl.rng;
  sl.s.process(In{in, std::max(inLead, 0)}, inFrames, out, outFrames);
}

uint64_t StretchPool::generatorState(uint32_t i) const noexcept { return slots_[i]->rng; }

}  // namespace pq::engine
