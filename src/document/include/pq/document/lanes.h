#pragma once

// FD.2 (docs/plans/FD.md "Design"; SPEC §15 "lanes and the matrix"): LANE CABLES in the document. A lane cable is a
// cable whose signal is wider than stereo (N = 4, 8, 16 … channels: graph/infer.h's width; made by `pack` blocks — the
// Lanes and Pack modules). The compiler's type inference is the truth about widths, but it runs after the lowering,
// and two rules of the lowering depend on whether a cable is a lane cable (lower.h "Lane cables"), as does the look of
// a cable in the views. laneWidths ESTIMATES every wire's width from the document alone:
//
//   a wire          carries what its source gives: a block's output, a group's In terminal (what reaches the group's
//                   main input) or its output (what reaches its Out terminal), a track's own signal (2; a MIDI
//                   track's notes: 1), a mixer's output (2)
//   pack block      the sum of its inputs' widths (an unwired input: 1)
//   channel block   1
//   a native engine its outputs' declared widths (meta::OutputSpec: the Wavetable, Envelope and Noise mono, the Sampler
//                   stereo), or — an analyser, which passes its input on — what reaches it
//   any other atom block   the widest thing wired into it (atoms broadcast); nothing wired: 1
//   a value block   the widest LANE CABLE wired into its control from its own track (the control is then per lane), else
//                   1: a mono or stereo cable, and any cable from another track, gives the control its first channel
//                   (lower.h "Lane cables")
//   anything else   2 (a stock patch)
//
// solved as a fixpoint over the project's wires (a feedback loop converges: widths only grow, capped at
// graph::kMaxWidth). It is an upper estimate — where inference would refuse a mix of two widths it takes the larger —
// and exact for every circuit inference accepts whose blocks are the ones above.
//
// Cost: nothing when the project has no pack block and no channel block reading past the second channel (no cable can
// be wider than stereo and no group needs more: one pass over the devices); else linear in devices + wires per round,
// a handful of rounds. Not RT code.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pq/document/ids.h"

namespace pq::doc {

struct Project;

struct LaneWidths {
  // The wires estimated wider than stereo with their width, sorted by wire id.
  std::vector<std::pair<WireId, uint16_t>> wide;
  // The estimated width of wire `w` when it is a lane cable (> 2), else 0.
  uint16_t of(WireId w) const noexcept;
  // BOUND GROUPS (sorted by device id): the groups whose main input gets a cable their inside cannot take, with the
  // width N that input takes — the lowering has such a group play its input-bound variant (model.h
  // groupInWidthPatchText), so the compiler refuses the cable where the user sees it (the group's own input) instead of
  // some of its channels being read silently. A group has an input width when EVERY cable out of its In terminal ends
  //   - on a `channel` block (it reads In channel by channel: N = its highest index + 1 — the Utility's L and R: 2;
  //     Lanes: 2; a Merge / Unpack of N lanes: N), or
  //   - on an input of a native engine whose output width is declared (the Wavetable, Envelope, Noise, Sampler: they
  //     take a mono or stereo signal: 2),
  // and that N is ≥ 2; it is bound when what reaches its main input is wider than mono and not N wide. (A group that
  // also passes In on — an EQ in every mode, a Compressor — has none: lanes pass.)
  std::vector<std::pair<DeviceId, uint16_t>> bound;
  uint16_t boundOf(DeviceId group) const noexcept;  // N, or 0: not bound
  bool empty() const noexcept { return wide.empty() && bound.empty(); }
  friend bool operator==(const LaneWidths&, const LaneWidths&) = default;
};
LaneWidths laneWidths(const Project& p);

// The reason shown on a cable the compiler cut for its width (lower.h "Cut wires"), in the document's words, from the
// compiler's diagnostic `message` (graph/infer.cpp: "width mismatch: ch(8) into stereo input 'x' of …", "… ch(4) into
// ch(8) …", "… ch(8) and stereo meet at …"); "" when `message` is no width error (the caller keeps the diagnostic).
//   N lanes into a stereo input   "8 lanes into a stereo input: merge the lanes first (a Merge module)"
//   N lanes into a mono input     "8 lanes into a mono input: merge the lanes first (Merge), or take one lane (Unpack)"
//   into a module set to M lanes  "4 lanes into a module set to 8 lanes: set its lane count to 4"
//   two widths at one block       "8 lanes and a stereo signal meet here: only a mono signal joins a lane cable …"
std::string laneRefusal(std::string_view message);

}  // namespace pq::doc
