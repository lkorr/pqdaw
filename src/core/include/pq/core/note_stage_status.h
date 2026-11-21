#pragma once

// AP.6 (docs/plans/AP.md "AP.6"): what a NOTE STAGE (pq/engine/note_stage.h; the arpeggiator) shows its device panel —
// the step it played last and the order its pattern plays the held notes in. The audio thread writes it; the UI thread
// reads it (the panel's step-lane editor lights each lane's current step, the pattern view the held notes and the
// current one). The status of a probe (pq/core/probe_level.h) in the same style: one writer, any readers, no waiting.
//
//   step fields   written at every step the stage plays (relaxed stores, a handful per step): each lane's position
//                 (−1: none yet / idle), the pattern position played (an index into the pattern list, −1: none), the
//                 copy (Steps). `steps` counts the writes (release), so a reader sees whether anything moved.
//   the pattern   written when the held set or what orders it changed (a seqlock: `seq` odd while it is written; a
//                 reader copies, then checks `seq` did not move, else it keeps what it had): up to kNotes entries in the
//                 order the pattern plays them (their Hz), how many notes are held, and `chord` (every entry plays at each
//                 step: Chord Trigger).
//                 AP.10: with it the HELD NOTES themselves (by Hz, the first kHeld): each note-on's identity, so the
//                 panel can sketch the whole resolved cycle over what is held (pq/engine/arp_sketch.h).
//   idle          held 0, the list empty, every position −1.
//
// RT-safe writer (noexcept, no allocation, no waiting); a reader never blocks the writer. Fixed size (≈ 1 KB).

#include <atomic>
#include <cstdint>

#include "pq/core/cache_line.h"

namespace pq {

struct alignas(kHotLineSeparation) NoteStageStatus {
  static constexpr uint32_t kLanes = 8;   // step lanes (the arpeggiator has 6)
  static constexpr uint32_t kNotes = 64;  // pattern entries shown
  static constexpr uint32_t kHeld = 16;   // AP.10: held notes carried (by Hz)
  // AP.10: a held note's identity (its note-on's: pq/engine/note_events.h NoteEvent).
  struct HeldNote {
    float hz = 0.0f, velocity = 0.0f, ratio = 1.0f, harmonic = 0.0f, degree = 0.0f, equave = 0.0f, tuning = 0.0f,
          noteDegree = 0.0f;
  };
  static constexpr uint32_t kHeldFields = 8;

  // ---- step fields ----
  std::atomic<int32_t> lane[kLanes] = {-1, -1, -1, -1, -1, -1, -1, -1};
  std::atomic<int32_t> position{-1};
  std::atomic<int32_t> copy{0};
  std::atomic<uint64_t> steps{0};
  // ---- the pattern (seqlock) ----
  std::atomic<uint32_t> seq{0};
  std::atomic<uint32_t> count{0};
  std::atomic<uint32_t> held{0};
  std::atomic<uint32_t> chord{0};
  std::atomic<float> hz[kNotes] = {};
  std::atomic<uint32_t> heldCount{0};
  std::atomic<float> heldNote[kHeld][kHeldFields] = {};
  // Every write moves it (release): a reader compares it with the last it saw.
  std::atomic<uint64_t> serial{0};

  // ---- writer (the stage's audio thread). RT-safe ----
  // One step: lane positions (`n` ≤ kLanes values; the rest untouched), the pattern position and copy.
  void writeStep(const int32_t* lanes, uint32_t n, int32_t pos, int32_t cp) noexcept {
    for (uint32_t i = 0; i < n && i < kLanes; ++i) lane[i].store(lanes[i], std::memory_order_relaxed);
    position.store(pos, std::memory_order_relaxed);
    copy.store(cp, std::memory_order_relaxed);
    steps.store(steps.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    bump();
  }
  // The pattern: `num` entries (clamped to kNotes) from `at(i)` → Hz; AP.10: `nh` held notes (clamped to kHeld) from
  // `heldAt(i)` → HeldNote.
  template <class F>
  void writePattern(uint32_t num, uint32_t numHeld, bool isChord, F&& at) noexcept {
    writePattern(num, numHeld, isChord, at, 0, [](uint32_t) { return HeldNote{}; });
  }
  template <class F, class G>
  void writePattern(uint32_t num, uint32_t numHeld, bool isChord, F&& at, uint32_t nh, G&& heldAt) noexcept {
    const uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const uint32_t c = num < kNotes ? num : kNotes;
    for (uint32_t i = 0; i < c; ++i) hz[i].store(static_cast<float>(at(i)), std::memory_order_relaxed);
    count.store(c, std::memory_order_relaxed);
    held.store(numHeld, std::memory_order_relaxed);
    chord.store(isChord ? 1u : 0u, std::memory_order_relaxed);
    const uint32_t h = nh < kHeld ? nh : kHeld;
    for (uint32_t i = 0; i < h; ++i) {
      const HeldNote n = heldAt(i);
      const float v[kHeldFields] = {n.hz, n.velocity, n.ratio, n.harmonic, n.degree, n.equave, n.tuning, n.noteDegree};
      for (uint32_t f = 0; f < kHeldFields; ++f) heldNote[i][f].store(v[f], std::memory_order_relaxed);
    }
    heldCount.store(h, std::memory_order_relaxed);
    seq.store(s + 2, std::memory_order_release);
    bump();
  }
  // Idle: nothing held, nothing playing.
  void writeIdle() noexcept {
    for (std::atomic<int32_t>& l : lane) l.store(-1, std::memory_order_relaxed);
    position.store(-1, std::memory_order_relaxed);
    writePattern(0, 0, false, [](uint32_t) { return 0.0f; });
  }

  // ---- reader (any thread) ----
  struct Pattern {
    uint32_t count = 0, held = 0;
    bool chord = false;
    float hz[kNotes] = {};
    uint32_t heldCount = 0;  // AP.10
    HeldNote heldNotes[kHeld] = {};
  };
  // A consistent copy of the pattern into `out` (false: a write was in progress or overlapped; `out` unchanged).
  bool readPattern(Pattern& out) const noexcept {
    const uint32_t s0 = seq.load(std::memory_order_acquire);
    if ((s0 & 1u) != 0) return false;
    Pattern p;
    p.count = count.load(std::memory_order_relaxed);
    p.held = held.load(std::memory_order_relaxed);
    p.chord = chord.load(std::memory_order_relaxed) != 0;
    if (p.count > kNotes) p.count = kNotes;
    for (uint32_t i = 0; i < p.count; ++i) p.hz[i] = hz[i].load(std::memory_order_relaxed);
    p.heldCount = heldCount.load(std::memory_order_relaxed);
    if (p.heldCount > kHeld) p.heldCount = kHeld;
    for (uint32_t i = 0; i < p.heldCount; ++i) {
      float v[kHeldFields];
      for (uint32_t f = 0; f < kHeldFields; ++f) v[f] = heldNote[i][f].load(std::memory_order_relaxed);
      p.heldNotes[i] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]};
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq.load(std::memory_order_relaxed) != s0) return false;
    out = p;
    return true;
  }

private:
  void bump() noexcept { serial.store(serial.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
};

}  // namespace pq
