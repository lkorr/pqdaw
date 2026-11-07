#pragma once

// Instrument groups (IG.1-2; SPEC §15 2026-11-04 "instrument groups", 2026-11-06 "single-signal cables"; docs/plans/IG.md;
// model.h "Instrument groups"): which blocks are VOICE CONTEXTS — where a MIDI track's notes are played per note — and
// the schema 17 → 18 migration that wraps an existing track's instrument into one.
//
// THE RULE. A voice context is a block whose main input is fed notes:
//   an instrument group   a group device (model.h "Groups") heading a MIDI track's main path, or one a notes cable (a
//                         wire from a MIDI track's source, port "", into its main input "" / `x`) enters. Everything
//                         inside it (its members) runs per note of that source; its Out sums the voices.
//   a lone instrument     a stock instrument or an Instrument Rack (their own code is per voice) fed the same way while
//                         it is not a member of an instrument group: it is a context of its own (one block), as if
//                         grouped alone, so a stock instrument plays its notes wherever it sits.
// Its NOTES SOURCE: a notes cable from ANOTHER MIDI track (the lowest wire id) wins over the block's own track (its main
// path's head, a notes cable from its own source) — an explicit cable is the user's choice; any other notes into it are
// not played (one source per context: to layer, use another group). The context is lowered in its source's track patch
// (lower.h "Voice contexts"), so a group on track B played by A's notes runs with A's voices.
//
// Library-free (document only), O(blocks + wires). Not RT code.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "pq/document/model.h"

namespace pq::doc {

struct VoiceContext {
  const Device* block = nullptr;  // the group, or the lone instrument
  const Track* home = nullptr;    // the block's own track
  TrackId source;                 // the MIDI track whose notes play it
  WireId feed;                    // the notes cable that feeds it (invalid: its track's main path)
  bool group = false;             // an instrument group (else a lone stock instrument / Instrument Rack)
  bool onPath = false;            // the block is on its home track's main path
};

struct VoiceContexts {
  std::vector<VoiceContext> list;
  std::unordered_map<uint32_t, uint32_t> byBlock;   // block device id → index
  std::unordered_map<uint32_t, uint32_t> byMember;  // a member of an instrument group → its group's index
  // The context `d` is the block of, or a member of (nullptr: neither).
  const VoiceContext* blockOf(DeviceId d) const noexcept;
  const VoiceContext* memberOf(DeviceId d) const noexcept;
  // The context `d` runs in (its own or its group's), or nullptr: `d` is outside every instrument group.
  const VoiceContext* of(DeviceId d) const noexcept;
  bool empty() const noexcept { return list.empty(); }
};

// Every voice context of `p` (active blocks of active... every track; a block of a deactivated track is listed too: the
// lowering leaves its track out anyway). In track order (forEachTrack), each track's blocks in list order.
VoiceContexts voiceContexts(const Project& p);

// Whether device `d` is an instrument group (a group whose main input is fed notes).
bool isInstrumentGroup(const Project& p, DeviceId d);


// ---- Schema 18 → 19 (serialize.h): wrap each MIDI track's instrument into an instrument group ----------------------
// Sound-identical (tests/app/test_instrument_groups.cpp, every fixture: diff 0, interpreter == JIT). Per MIDI track:
//   * the main path's first active block, when it takes the notes: a stock instrument or an Instrument Rack (alone), or
//     a meta voice run (a meta module whose chain input takes the notes and the meta modules after it the voices reach;
//     switched-off blocks between them pass the voices on) becomes the members of a new group G that takes its place on
//     the path (the path links keep their ids), the series as chain links (In → … → Out, the chain's flow: groupInCircuit's
//     rule); a group there already is an instrument group (TX.groups-fix1) and is left as it is;
//   * the meta modules off the path that ran per voice join G (they keep their wires): those a per-voice signal reached
//     (a notes cable, a note output, a wire from a member) and those that only fed members (a free-running source the
//     compiler made per voice); without a head context but with such modules, G is a group off the path fed by a notes
//     cable from the track's source;
//   * an off-path meta module another MIDI track's notes cable played (TX.cross-track's guest) gets a group of its own
//     on its track, fed by a notes cable from that source.
// No Envelope is added (it would change the sound); no polyphony is written (absent = the link's default, as before).
// Lone stock instruments fed by notes cables are contexts already and stay as they are. New ids continue above the
// project's largest. Returns whether anything changed.
bool migrateToInstrumentGroups(Project& p);

}  // namespace pq::doc
