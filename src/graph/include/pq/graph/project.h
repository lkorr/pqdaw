#pragma once

// Per-track compile units (T1.x-arch1, SPEC §7.4/§7.6, §15 2026-10-21 "compile granularity is per track"): the
// plain-data description of a whole project's signal graph, its partition into compile units, and the composition of
// one unit (or of the whole project) into a single Patch that the ordinary pipeline (infer → optimise → schedule →
// interpreter/JIT) compiles. Not RT-safe: compiler thread. The engine side (pq/engine/project_program.h) runs the
// units; pq/engine/project_compile.h drives the pipeline per unit.
//
// ---- The description (T3.17 lowers the document into it; graph never includes document) -----------------------
// A ProjectGraph is a list of tracks and the wires between their ports:
//   ProjectTrack  one track's device chain as one Patch (devices are subcircuits from `library`); `id` is the
//                 document's stable track id. Its `in`s / `param`s are the ports wires can drive, its `out`s /
//                 `export`s the ports wires can read. An Instrument track's patch may use voice.*; every other role
//                 is global only. Group, return and master tracks are tracks like any other: a summing bus is a
//                 patch whose `in` receives several Audio wires.
//   ProjectWire   from (track, out/export name) to (track, in/param name), of a kind:
//                   Audio       track routing, sends, group/return/master summing: feed-forward between units
//                               through a buffer (no latency, same block)
//                   Sidechain   a sidechain input             } SPEC §15: the two tracks are MERGED into one unit
//                   Modulation  cross-track modulation        } (one Patch, sample-accurate, optimised together)
//                 Several wires into one port are summed (an `add` chain in wire order). A modulation wire drives
//                 the port it names (its mode/depth math belongs to the lowering: an `in` → math → the param).
//   output        the (track, port) the device plays (the master's `out`).
// Cross-track signals are GLOBAL: a per-voice source is summed over the voices where it leaves its track (the same
// implicit voice_sum as a root `out … global`), whether the consumer is in the same unit or not, so merging never
// changes what a consumer sees.
//
// ---- Partition (partitionProject) ------------------------------------------------------------------------------
// 1. One unit per track. 2. Union over every Sidechain / Modulation wire (SPEC §15), except a union that would put
// two Instrument tracks into one unit: the ProgramImage ABI has one voice table per image, so two instruments in one
// image would share their voices. Such a wire stays a feed-forward bus, which is exactly as sample-accurate when it is
// acyclic (a note is recorded; QUESTIONS Q84). 3. Tarjan SCCs on the unit graph (every wire between two
// units): a cycle between units is merged into one unit (SPEC §7.4 "cross-track SCCs pull the involved tracks into
// one scheduling region"); the scheduler then treats the loop as any feedback: an explicit delay in it is used, a
// delay-free one gets the usual implicit z1 with a warning (ScheduleOptions::visualMode). A cycle through two
// instruments is an error (Unsupported). 4. Units in dependency order (Kahn, ties by the lowest track index), so
// every unit runs after every unit it reads, within the same block.
// Buses: one per distinct (track, out/export) read by another unit, plus the project output. Bus k is written by
// exactly one unit (a root `out` named `bus<k>`) and read by any later unit (a root `in` named `bus<k>`).
//
// ---- Composition (composeUnit) --------------------------------------------------------------------------------
// The unit's Patch: one subcircuit node per member track, whose NodeId is the track's id (so every state key in the
// track — TypedPatch::stateKey = hash of the NodeId call path — is the same whatever unit the track is in: merges and
// splits keep state keys, and a per-unit program and the whole-project program agree on noise seeds), named
// `t<id>` (flattened names: `t<id>__<name>`, e.g. the param `t7__cutoff`); every wire inside the unit becomes a wire
// through an identity subcircuit with a global bound (the voice reduction above); every bus the unit reads becomes a
// root `in bus<k>` (global audio, the producer's kind and width), every bus it writes a root `out bus<k>`. An
// Instrument member also gets a root voice `export vmon_<port>` per voice out/export of its patch (the VoiceEngine
// monitors them for voice end; a voice port named `done` becomes `export done`, VoiceConfig::doneSlot).
// wholeProject() + composeUnit(…, 0) is the single whole-project program of the same graph (tests compare it with
// the per-unit project; M12's background whole-project re-optimisation starts from it).

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/graph/infer.h"
#include "pq/graph/patch.h"

namespace pq::graph {

enum class TrackRole : uint8_t { Instrument, Audio, Group, Return, Master };

struct ProjectTrack {
  uint32_t id = 0;               // stable, unique, ≥ 1 (the call node's NodeId: keys state, see above)
  std::string name;              // for diagnostics and unit names
  const Patch* patch = nullptr;  // must outlive every use of the graph, partition and composed unit
  TrackRole role = TrackRole::Audio;
  uint32_t eventSlot = kNone;    // Instrument: the note-event slot (Timeline::eventSource(k)); kNone: none
  uint32_t maxVoices = 0;        // Instrument: its polyphony (T4.12-fix1: the instrument device's `voices` setting);
                                 // 0: the compiler's default (engine::ProjectCompileOptions::voice.maxVoices)
};

enum class WireKind : uint8_t { Audio, Sidechain, Modulation };

struct ProjectPort {
  uint32_t track = kNone;  // index into ProjectGraph::tracks
  std::string port;        // from: an `out`/`export` of the track's patch; to: an `in`/`param`
  friend bool operator==(const ProjectPort&, const ProjectPort&) = default;
};

struct ProjectWire {
  ProjectPort from, to;
  WireKind kind = WireKind::Audio;
};

struct ProjectGraph {
  std::vector<ProjectTrack> tracks;
  std::vector<ProjectWire> wires;
  ProjectPort output;                     // what the device plays
  const PatchLibrary* library = nullptr;  // callees of the track patches (devices, stdlib); may be null
  // T4.7: out/export ports that get a bus even when no other unit reads them (analysis taps: a modulated param's
  // range, read by the views as a metered bus). Their buses come after the output's, so adding a tap never renumbers
  // another bus. wholeProject() ignores them.
  std::vector<ProjectPort> taps;
};

struct ProjectUnit {
  std::vector<uint32_t> tracks;  // member track indices, ascending by track id
  uint32_t voiceTrack = kNone;   // the Instrument member (at most one), or kNone
  std::string name;              // member names joined with '+'
  bool merged() const noexcept { return tracks.size() > 1; }
};

struct ProjectBus {
  ProjectPort source;              // the track port it carries
  uint32_t producer = kNone;       // unit index
  std::vector<uint32_t> readers;   // unit indices (ascending), all after `producer` in unit order
  bool output = false;             // the project output
};

struct ProjectPartition {
  std::vector<ProjectUnit> units;  // dependency order: a unit only reads buses of earlier units
  std::vector<uint32_t> unitOf;    // per track
  std::vector<ProjectBus> buses;
  uint32_t outputBus = kNone;
  std::vector<std::string> notes;  // merges that were not made (two instruments), cycles merged
};

// Errors: invalid track ids / patches, a wire to an unknown track or port (a `from` must be an out/export, a `to` an
// in/param), no output, a cycle between units through two instruments (Unsupported).
Result<ProjectPartition> partitionProject(const ProjectGraph& graph);
// The same graph as ONE unit (every track; buses: only the output). Same errors as partitionProject except the
// two-instrument cycle (an image has one voice table: a whole-project program of several instruments plays the same
// notes on all of them; tests feed every instrument the same events).
Result<ProjectPartition> wholeProject(const ProjectGraph& graph);

// One unit as a Patch (see "Composition"). `library` holds the track patches, the identity helper and
// graph.library's callees: pass it as InferOptions::library. Root `in`s / `out`s are in bus order.
struct UnitPatch {
  Patch patch;
  PatchLibrary library;
  std::vector<uint32_t> inBus;    // per root `in` (node order = IoSlot::ordinal): the bus it reads
  std::vector<uint32_t> outBus;   // per root `out` (node order): the bus it writes
  bool voices = false;            // has an Instrument member (InferOptions::voiceContext)
  uint32_t eventSlot = kNone;     // the Instrument member's ProjectTrack::eventSlot
};
// busTypes[k]: the type of bus k as its producer compiled it (busType()); every bus the unit reads must be known
// (compile producers first: unit order guarantees it). A root `in bus<k>` is declared with that kind and width as an
// AUDIO-rate global signal, so a consumer never has to be recompiled because its producer's rate changed.
Result<UnitPatch> composeUnit(const ProjectGraph& graph, const ProjectPartition& partition, uint32_t unit,
                              std::span<const PortType> busTypes);
// The inferred type of the value arriving at root `out bus<k>` of a typed unit (kind Infer if it has no such out).
PortType busType(const TypedPatch& typed, uint32_t bus);
// "bus<k>" → k, or kNone.
uint32_t parseBusName(std::string_view name) noexcept;
std::string busName(uint32_t bus);

// The unit's compile-cache key (SPEC §7.4 "keyed by graph hash"): a 64-bit hash of everything in a typed (optimised)
// patch that the program depends on — nodes (kind, atom, literal bits, param declarations, declaration names),
// inferred port types, wires, state keys — mixed with `salt` (sample rate, block size, backend …). Equal programs
// have equal keys; T1b.4's on-disk cache adds CPU features and the compiler version.
uint64_t unitHash(const TypedPatch& typed, uint64_t salt = 0) noexcept;

}  // namespace pq::graph
