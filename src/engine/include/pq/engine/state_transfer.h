#pragma once

// Hot-swap state transfer (T1.9, SPEC §7.5): copies the state of every node that survives a recompile from the old
// ProgramImage's arena into the new one's, so phases, filter memories, delay lines, envelope stages and solver
// guesses continue where they were. Backend-neutral: it only uses graph::StateLayout and the StateArena of the
// ProgramImage ABI (program_image.h), so interpreter → interpreter, interpreter → JIT (T1b.5) and JIT → JIT swaps
// all work the same.
//
// Matching: a new entry takes the old entry with the same (stateKey, channel, arena) when the type tag (kernel,
// lanes, ring frames) and the byte size are equal (SPEC §7.5: "by node ID"; the key is the hash of the NodeId call
// path, TypedPatch::stateKey). Anything else — a new node, a changed atom, a resized delay ring — keeps the state
// the new image was built with (StateInit). Old entries without a match are dropped.
//
// Plan (off the audio thread) vs execution (on it): buildTransferPlan() resolves the matches into byte ranges and
// coalesces adjacent ones (src and dst both contiguous). Because schedule() keeps unchanged entries at their old
// offsets (ScheduleOptions::previous), the ranges of an edit that adds or removes a few nodes merge into a handful
// of large memcpys. executeTransfer() runs on the audio thread at the swap's block boundary, right before the new
// image's first block, so it copies the old image's CURRENT state (never a snapshot). Voice ranges are relative to
// one voice group's block and are copied for every group (or only the occupied groups the caller names: a lane
// that starts later is reset to StateInit anyway when VoiceConfig::resetOnNoteOn is on).
// Cost: memcpy of the transferred bytes (bench/RESULTS.md T1.9); nothing else.

#include <cstdint>
#include <vector>

#include "pq/core/result.h"
#include "pq/engine/program_image.h"
#include "pq/graph/schedule.h"

namespace pq::engine {

struct StateCopy {
  uint32_t src = 0, dst = 0, size = 0;  // byte offsets into the old / new arena (or voice block), byte count
};

struct TransferPlan {
  std::vector<StateCopy> global;  // into StateArena::global
  std::vector<StateCopy> voice;   // into one voice block (StateArena::voice + g · voiceStride), per group
  uint32_t voiceGroups = 0;       // groups copied at most: min(old, new) (0 when either has no voice arena)
  // What matched (for tests, tools and the T3.17 controller's diagnostics).
  uint32_t matched = 0;           // new entries that take old state
  uint32_t fresh = 0;             // new entries without an old entry of the same key/channel/arena
  uint32_t mismatched = 0;        // same key/channel/arena but another tag or size: fresh as well
  uint32_t dropped = 0;           // old entries the new layout does not have
  uint64_t globalBytes = 0;       // bytes copied per swap in the global arena
  uint64_t voiceBytes = 0;        // bytes copied per voice group

  bool empty() const noexcept { return global.empty() && voice.empty(); }
};

// Not RT-safe (allocates). The arenas are only checked for size (the layouts must fit them); their memory is not
// touched. Errors: a layout entry outside its arena.
Result<TransferPlan> buildTransferPlan(const graph::StateLayout& from, const StateArena& fromArena,
                                       const graph::StateLayout& to, const StateArena& toArena);

// Copies the planned ranges from `from` into `to`. Voice ranges go to groups g < plan.voiceGroups whose
// groupMask[g] is non-zero (groupMask null: every group). RT-safe: memcpy only, bounded by the plan.
void executeTransfer(const TransferPlan& plan, const StateArena& from, const StateArena& to,
                     const uint32_t* groupMask = nullptr) noexcept;

}  // namespace pq::engine
