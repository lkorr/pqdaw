#pragma once

// Live frame-table slots (TMM.2; docs/plans/MM.md Decisions → DSP, "Table slots"): the resource a `wavetable_pos`
// atom reads (Resource::FrameTable). A new table reaches the audio thread without a recompile: the owner (the
// playback link, TMM.5's builder) publishes it, and every kernel reading the slot crossfades from the table it was
// playing to the new one over kFrameTableFadeFrames samples.
//
// ---- Publication ------------------------------------------------------------------------------------------------
// The slot holds one atomic pointer to an immutable Entry {the table's view, its owner, a generation, the entry it
// replaced}. A kernel does one acquire load per call (Entry), compares the generation with the one its state played,
// and on a change fades from entry->prev to entry over the next 64 samples of its own frame count (a fade spanning
// several calls re-reads the slot each call: while the generation is unchanged, entry->prev is the same table). A
// kernel never keeps an entry pointer past its call.
//   publish(B) over A (whose prev is P): the new entry's prev is A; P can no longer be reached by a kernel that
//   loads the slot after this, so P is retired to the EpochReclaimer at once and freed after the audio thread's next
//   quiescent state (QSBR: a kernel that loaded A in the block running now may still be reading A->prev = P; it is
//   done by the next quiescent point). A stays alive as B->prev.
// Consequences: two publishes inside one fade restart the fade from the newer previous table (the output steps from
// the old mix to that table: never a dangling read); a kernel that did not run while several tables were published
// (a voice group without voices) fades from the newest previous one; a fresh state (generation 0) does not fade.
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
#include <cstdint>
#include <memory>
#include <mutex>

#include "pq/core/epoch_reclaimer.h"
#include "pq/core/voice_tap.h"
#include "pq/dsp/frame_table.h"

namespace pq::dsp {

inline constexpr uint32_t kFrameTableFadeFrames = 64;

class FrameTableSlot {
public:
  struct Entry {
    FrameTableView view{};
    const Entry* prev = nullptr;   // the entry this one replaced (alive while this one is current)
    uint64_t generation = 0;       // 1 for the initial table, +1 per publish
    std::shared_ptr<const FrameTable> table;
  };

  // Allocates. `initial` must not be null.
  FrameTableSlot(std::shared_ptr<const FrameTable> initial, const VoiceTapConfig& tap = {});
  ~FrameTableSlot();
  FrameTableSlot(const FrameTableSlot&) = delete;
  FrameTableSlot& operator=(const FrameTableSlot&) = delete;

  // Non-RT. Makes `table` (not null) current; retires the entry two publishes back to `reclaimer`.
  void publish(std::shared_ptr<const FrameTable> table, EpochReclaimer& reclaimer);
  // Non-RT: the newest table and its generation.
  std::shared_ptr<const FrameTable> table() const;
  uint64_t generation() const noexcept { return entry_.load(std::memory_order_acquire)->generation; }

  // RT-safe: the current entry (valid until the caller's next quiescent state).
  const Entry* acquire() const noexcept { return entry_.load(std::memory_order_acquire); }
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

}  // namespace pq::dsp
