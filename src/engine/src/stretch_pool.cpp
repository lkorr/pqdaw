// The stretch pool (T6.5): pq/engine/stretch_pool.h. The only TU that includes signalsmith-stretch (its warnings are
// off for this file, src/engine/CMakeLists.txt).
#include "pq/engine/stretch_pool.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "pq/core/aligned_buffer.h"
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

float rateOf(double sampleRate) noexcept { return static_cast<float>(sampleRate >= 1000.0 ? sampleRate : 48000.0); }

// The configuration's buffers (an estimate from the preset's sizes: signalsmith's members are private). Per stretcher:
// the STFT's input history (block + interval, ×2 with the stashed copy), output (block + window products, ×2), spectra
// and FFT work buffers (~4 × block complex), and per band (block / 2 + 1, per channel): 2 Band (28 B), a Prediction
// (12 B), and ~6 floats of maps and energies. The M6 review (L15) measured ~25 % more in process private bytes.
size_t stretcherBytes(const Stretch& probe, size_t ch) noexcept {
  const auto block = static_cast<size_t>(probe.blockSamples()), interval = static_cast<size_t>(probe.intervalSamples());
  const size_t bands = block / 2 + 1;
  const size_t floats = ch * 2 * (block + interval) + ch * (2 * block + block) + 4 * block * ch + block + interval +
                        static_cast<size_t>(probe.outputLatency()) * ch;
  return floats * sizeof(float) + bands * ch * (28 + 12) + bands * 6 * sizeof(float);
}

}  // namespace

struct StretchPool::Slot {
  Stretch s{0};
  uint64_t rng = 0;
  uint32_t channels = 2;
  size_t bytes = 0;
  AlignedBuffer<float> out;  // 2 × outFrames
  Slot* next = nullptr;      // build() lists
};

StretchPool::Slot* StretchPool::build(uint32_t stereo, uint32_t mono, double sampleRate, uint32_t outFrames) {
  const float sr = rateOf(sampleRate);
  Slot* head = nullptr;
  Slot** tail = &head;
  for (uint32_t i = 0; i < stereo + mono; ++i) {
    auto slot = std::make_unique<Slot>();
    slot->channels = i < stereo ? 2u : 1u;
    slot->s.presetDefault(static_cast<int>(slot->channels), sr, true);
    if (outFrames > 0) {
      slot->out = AlignedBuffer<float>(static_cast<size_t>(2) * outFrames);
      std::fill(slot->out.data(), slot->out.data() + slot->out.size(), 0.0f);
    }
    slot->bytes = stretcherBytes(slot->s, slot->channels) + sizeof(float) * 2 * static_cast<size_t>(outFrames);
    *tail = slot.release();
    tail = &(*tail)->next;
  }
  return head;
}

void StretchPool::destroy(Slot* list) noexcept {
  while (list != nullptr) {
    Slot* next = list->next;
    delete list;
    list = next;
  }
}

StretchPool::Slot* StretchPool::append(Slot* list, Slot* more) noexcept {
  if (list == nullptr) return more;
  Slot* t = list;
  while (t->next != nullptr) t = t->next;
  t->next = more;
  return list;
}

void StretchPool::push(std::atomic<Slot*>& head, Slot* list) noexcept {
  if (list == nullptr) return;
  Slot* last = list;
  while (last->next != nullptr) last = last->next;
  Slot* h = head.load(std::memory_order_relaxed);
  do {
    last->next = h;
  } while (!head.compare_exchange_weak(h, list, std::memory_order_seq_cst, std::memory_order_relaxed));
}

StretchPool::StretchPool(uint32_t voices, double sampleRate, uint32_t monoVoices, uint32_t capacity, uint32_t outFrames)
    : capacity_(std::max(capacity, voices + monoVoices)) {
  slots_.reserve(capacity_);
  free_.reserve(capacity_);
  freeMono_.reserve(capacity_);
  Stretch probe{0};
  probe.presetDefault(2, rateOf(sampleRate), true);
  latency_ = {probe.inputLatency(), probe.outputLatency(), probe.seekLength(), probe.blockSamples(), probe.intervalSamples()};
  destroy(adopt(build(voices, monoVoices, sampleRate, outFrames)));  // the capacity covers them all
}

StretchPool::Latency StretchPool::latencyFor(double sampleRate) noexcept {  // RT-safe
  // presetDefault: block 0.12 s, interval 0.03 s; the Kaiser window's analysis / synthesis offsets are half a block;
  // split computation delays the output by one more interval.
  const float sr = rateOf(sampleRate);
  const auto block = static_cast<int>(sr * 0.12f), interval = static_cast<int>(sr * 0.03f);
  return {block - block / 2, block / 2 + interval, block + interval, block, interval};
}

StretchPool::~StretchPool() = default;

StretchPool::Slot* StretchPool::adopt(Slot* list) noexcept {  // RT-safe
  while (list != nullptr && slots_.size() < capacity_) {
    Slot* next = list->next;
    list->next = nullptr;
    const auto i = static_cast<uint32_t>(slots_.size());
    memory_ += list->bytes;
    // Claims pop from the back: the stretchers adopted first are claimed first. Within the reserved capacity, so
    // neither the insert nor the emplace allocates.
    std::vector<uint32_t>& f = list->channels == 1 ? freeMono_ : free_;
    f.insert(f.begin(), i);
    slots_.emplace_back(list);
    list = next;
  }
  return list;
}

uint32_t StretchPool::channels(uint32_t i) const noexcept { return slots_[i]->channels; }  // RT-safe

float* StretchPool::output(uint32_t i) noexcept { return slots_[i]->out.data(); }  // RT-safe

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
  if (i < slots_.size()) (slots_[i]->channels == 1 ? freeMono_ : free_).push_back(i);  // reserved: no allocation
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
