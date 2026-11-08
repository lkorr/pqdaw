#pragma once

// Live frame-table slots (TMM.2; docs/plans/MM.md Decisions → DSP, "Table slots"): the resource a `wavetable_pos`
// atom reads (Resource::FrameTable). A new table reaches the audio thread without a recompile: the owner (the
// playback link, TMM.5's builder) publishes it, and every kernel reading the slot crossfades from the table it was
// playing to the new one over kFrameTableFadeSeconds.
//
// ---- Publication ------------------------------------------------------------------------------------------------
// The slot holds one atomic pointer to the newest immutable Entry {the table's view, its owner, a generation, the
// entry it replaced}: a chain, newest first. A kernel does one acquire load per call and keeps in its state the
// generation it plays (and, while it fades, the generation it fades from and how far it is); it finds those entries
// on the chain (FrameTableSlot::find). A kernel never keeps an entry pointer past its call.
// AU.fold: a drag of a baked control publishes a table every ≈ 15 – 35 ms (TMM.9's previews); a 64-sample fade per
// publish was a click each time (the waveform stepped between the tables in 1.3 ms: a fold drag's worst 85 ms window
// −29 dB non-harmonic energy, against −48 for the same steps on a live control, ramped over TX.smooth's 20 ms; −46 with
// this fade: tests/app/test_au_fold.cpp). So the fade is long (kFrameTableFadeSeconds, about a drag's step) and never
// cut: a publish during a fade waits — the kernel finishes the fade it runs, then fades from the table it reached to the
// newest one (skipping any published in between). Every step of the output is then a crossfade between two tables
// the kernel played or plays: never a jump.
//   For that, old entries stay reachable for a while: publish() unlinks (prev ← null on the newer entry) and retires
// to the EpochReclaimer only entries at least two publishes back AND superseded at least kFrameTableKeepSeconds ago
// (wall clock, ≫ a fade plus an audio buffer), keeping at most kFrameTableMaxEntries; a retired entry is freed after
// the audio thread's next quiescent state (QSBR: a kernel that found it in the block running now may still read it).
// A kernel whose generation is no longer on the chain (it was idle for a long time) reads the newest table at once;
// a fresh state (generation 0) does not fade.
//
// ---- Threads ----------------------------------------------------------------------------------------------------
// publish(): any non-RT thread (serialised internally). The reclaimer must have every thread that runs images reading
// this slot as a reader (the link's: the engine timeline's audio thread). acquire(): RT-safe. The slot itself must
// outlive every image that reads it (the link's images hold it through UnitImage::keepAlive).
//
// ---- Voice tap --------------------------------------------------------------------------------------------------
// Each slot carries the VoiceTap its `wavetable_pos` kernels write (the phases read, the position, the increment;
// pq/core/voice_tap.h), so a Wavetable face shows its voices through the same resource.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "pq/core/epoch_reclaimer.h"
#include "pq/core/voice_tap.h"
#include "pq/dsp/frame_table.h"

namespace pq::dsp {

// AU.fold: the swap crossfade's length (seconds of the kernel's own rate: the same time inside an oversample region)
// and how long a superseded entry stays reachable (see "Publication"); kFrameTableMaxEntries bounds both the entries
// kept and a kernel's search.
inline constexpr double kFrameTableFadeSeconds = 0.025;
inline constexpr double kFrameTableKeepSeconds = 0.5;
inline constexpr uint32_t kFrameTableMaxEntries = 64;
// The fade's frames at a kernel rate (≥ 1). RT-safe.
inline uint32_t frameTableFadeFrames(double rate) noexcept {
  const double f = kFrameTableFadeSeconds * (rate > 0.0 ? rate : 48000.0);
  return f >= 1.0 ? static_cast<uint32_t>(f + 0.5) : 1u;
}

class FrameTableSlot {
public:
  struct Entry {
    FrameTableView view{};
    std::atomic<const Entry*> prev{nullptr};  // the entry this one replaced; null once that one is unlinked
    uint64_t generation = 0;                  // 1 for the initial table, +1 per publish
    std::shared_ptr<const FrameTable> table;
    std::chrono::steady_clock::time_point superseded{};  // publish() only (writeMutex_): when the next one replaced it
  };

  // Allocates. `initial` must not be null.
  FrameTableSlot(std::shared_ptr<const FrameTable> initial, const VoiceTapConfig& tap = {});
  ~FrameTableSlot();
  FrameTableSlot(const FrameTableSlot&) = delete;
  FrameTableSlot& operator=(const FrameTableSlot&) = delete;

  // Non-RT. Makes `table` (not null) current; unlinks and retires to `reclaimer` the entries that are old enough
  // ("Publication").
  void publish(std::shared_ptr<const FrameTable> table, EpochReclaimer& reclaimer);
  // Non-RT: the newest table and its generation.
  std::shared_ptr<const FrameTable> table() const;
  uint64_t generation() const noexcept { return entry_.load(std::memory_order_acquire)->generation; }

  // RT-safe: the current entry (valid until the caller's next quiescent state).
  const Entry* acquire() const noexcept { return entry_.load(std::memory_order_acquire); }
  // RT-safe: the entry of `generation` on the chain from `newest` (an acquired entry), or null (gone, or 0). Valid
  // until the caller's next quiescent state.
  static const Entry* find(const Entry* newest, uint64_t generation) noexcept {
    if (generation == 0) return nullptr;
    const Entry* e = newest;
    for (uint32_t i = 0; e != nullptr && i <= kFrameTableMaxEntries; ++i) {
      if (e->generation == generation) return e;
      if (e->generation < generation) return nullptr;
      e = e->prev.load(std::memory_order_acquire);
    }
    return nullptr;
  }
  // The tap its kernels write (never null).
  VoiceTap* tap() const noexcept { return tap_.get(); }
  const std::shared_ptr<VoiceTap>& sharedTap() const noexcept { return tap_; }

  // Tests (use-after-free checks): how retired and final entries are freed. Default: delete.
  using EntryDeleter = void (*)(void*) noexcept;
  void setEntryDeleter(EntryDeleter d) noexcept { deleter_ = d; }

private:
  static void deleteEntry(void* p) noexcept { delete static_cast<Entry*>(p); }
  std::atomic<const Entry*> entry_{nullptr};
  std::shared_ptr<VoiceTap> tap_;
  mutable std::mutex writeMutex_;
  EntryDeleter deleter_ = &deleteEntry;
};

// AU.fold: the tables a kernel reads next ("Publication"). From the newest entry, updates the state's generation /
// fade (fields generation, fromGen, fadeLeft; the fade is `fadeFrames` long, constant per state) and returns `to`, the table it plays, and `from`, the table it
// fades from (null: no fade). A kernel calls it per chunk of at most kFrameTableFadeChunk frames while it fades, so a
// fade that ends inside a call can be followed by the next one. RT-safe.
inline constexpr uint32_t kFrameTableFadeChunk = 64;
struct FrameTableFade {
  const FrameTableSlot::Entry* from = nullptr;
  const FrameTableSlot::Entry* to = nullptr;
};
template <class St>
inline FrameTableFade frameTableFade(const FrameTableSlot::Entry* newest, St& s, uint32_t fadeFrames) noexcept {
  if (s.fadeLeft > fadeFrames) s.fadeLeft = fadeFrames;  // (a rate change mid-fade)
  if (s.fadeLeft > 0) {
    const FrameTableSlot::Entry* to = FrameTableSlot::find(newest, s.generation);
    const FrameTableSlot::Entry* from = FrameTableSlot::find(newest, s.fromGen);
    if (to != nullptr && from != nullptr) return {from, to};
    s.fadeLeft = 0;  // an entry outlived its keep time (a stalled audio thread): no fade from it
    if (to != nullptr && to == newest) return {nullptr, to};
  }
  if (s.generation != newest->generation) {
    const FrameTableSlot::Entry* from = FrameTableSlot::find(newest, s.generation);
    s.fromGen = from != nullptr ? s.generation : 0;
    s.generation = newest->generation;
    s.fadeLeft = from != nullptr ? fadeFrames : 0;
    return {from, newest};
  }
  return {nullptr, newest};
}
// One chunk of a fade: out[f] = old[f] + g·(out[f] − old[f]) over m frames of `lanes` interleaved lanes, the new
// table's weight g = k / fadeLen at fade frame k (1 … fadeLen; the chunk starts with `fadeLeft` frames to go; at
// k = fadeLen the new table exactly). RT-safe.
inline void frameTableFadeMix(float* out, const float* old, uint32_t m, uint32_t lanes, uint32_t fadeLen,
                              uint32_t fadeLeft) noexcept {
  const float inv = 1.0f / static_cast<float>(fadeLen);
  const uint32_t k0 = fadeLen - fadeLeft;
  for (uint32_t f = 0; f < m; ++f) {
    const uint32_t k = k0 + f + 1;
    if (k >= fadeLen) continue;
    const float g = static_cast<float>(k) * inv;
    float* o = out + static_cast<size_t>(f) * lanes;
    const float* q = old + static_cast<size_t>(f) * lanes;
    for (uint32_t l = 0; l < lanes; ++l) o[l] = std::fma(g, o[l] - q[l], q[l]);
  }
}

}  // namespace pq::dsp
