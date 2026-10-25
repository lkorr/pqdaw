#pragma once

// Project → graph lowering (T3.17): a document snapshot as the per-track ProjectGraph (pq/graph/project.h) the
// engine compiles, plus the param map. `document` depends on `graph` for this (ARCHITECTURE §1); the note sequences
// and automation lanes are lowered by the app (they are engine plain-data types, and `document` never includes
// `engine`; pq/app/project_link.h).
//
// ---- Signal flow (every track's patch is generated text, parsed once and reused while its text is unchanged) ------
//   MIDI track    BasicSynth (the M3 default instrument, stdlib/instruments/basic_synth.pqp; QUESTIONS Q15 A) →
//                 voice_sum → mono × constant-power pan (centre −3 dB per side) → × volume × output gate → `out`
//                 (stereo); per return a send tap `tap<return id>` = the panned signal × send level × sends gate
//                 (× volume when post-fader).
//   Group         `in` (stereo: the sum of what is routed into it) → balance pan (centre 0 dB) → volume → gate →
//                 `out`; sends as above.
//   Return        `in` (the sum of every send tap into it) → balance → volume → gate → `out` (returns do not send).
//   Master        `in` → balance → volume → `out` (the device output).
//   Every gain (pan, volume, gate, sends) is a param at param rate, so mixer moves, mute and solo never recompile
//   (SPEC §2, Q15); the product of the gains is smoothed per sample by one one-pole (2 ms) over all of a track's
//   gain channels at once, so a mute or a jump is click-free. Volume and send levels are in dB (the document's
//   unit, so automation renders straight into them); −70 dB (kMinVolumeDb) is −∞.
//   Routing: Output::Default → the parent group, else the master; Master → the master; Track → that group (the
//   master if it is missing or inactive); None → no output wire (sends only). Every track has a send wire to every
//   return (Ableton's implicit sends): a send level is a param, so turning a send up never recompiles either.
//   Deactivated tracks (Track::active = false, `0`: structural, Q15) and their members are left out of the graph.
//   Mute / solo: computeAudibility (audibility.h) → the `gate` (output) and `sendGate` params.
//   Tuning signal (T2.6-fix1, SPEC §3.3; QUESTIONS Q100, Q105): a MIDI track gets one tuning node per main tuning
//                 with a linear form its clips follow (trackTuningSignals: the track-level tuning, then clip
//                 overrides to other main tunings, T2.6-fix3), chained before the instrument:
//                   tun1 = tuning_hz(voice.degree, voice.equave, voice.hz, voice.cents, <gen1>, tper1, <st1>, tref1,
//                                    table: k1, select: voice.tuning, node: 1)
//                   tun2 = tuning_hz(voice.degree, voice.equave, tun1.hz, tun1.cents, …, node: 2) …
//                   tunv = tune_voices(tunN.hz, tunN.cents)
//                 Node i takes the voices whose voice.tuning is i (a note's PitchSource::node) and passes the others
//                 through, so every note follows its own clip's tuning (MOS params: tgen, tper, tst, tref; EDO:
//                 tper = the equave, tref; the EDO's generator / stretch inputs are literals) and their automation
//                 lanes. k = the table's index in Resources::tunings, from LowerOptions::tuningTableOf (the owner of
//                 the tables is the app, so the text only changes when a table does). Degree notes, Ratio notes
//                 anchored (through notes) to them, and reference-relative Ratio / Cents notes follow the node
//                 (doc::PitchFollow); private, Scala and MODMOS tunings keep their static Hz.
//
// ---- Params ----------------------------------------------------------------------------------------------------
// One LoweredParam per (track, patch param) with a stable string key ("<track id>:volume", ":pan", ":gate",
// ":sendGate", ":send:<return id>"; a tuning node's params "tuning:<tuning id>:reference", ":equave", ":generator",
// ":period", ":stretch" (patch names tref<i>, tper<i>, tgen<i>, tst<i> for node i), one entry per track that follows
// the tuning, all with the same key) that the app maps to a
// project param slot that never moves, and the current value; `ref` is the document ParamRef where there is one
// (automation lanes target it; gates are derived).
//
// Not RT code.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/model.h"
#include "pq/graph/infer.h"
#include "pq/graph/parse.h"
#include "pq/graph/project.h"

namespace pq::doc {

struct LowerOptions {
  const graph::PatchLibrary* library = nullptr;    // the instrument patch (and whatever track patches call)
  std::span<const graph::SubcircuitRef> externals; // its signatures (parse externals of the generated patches)
  std::string instrument = "BasicSynth";
  // The note-event slot of a MIDI track (Timeline::eventSource(k)); stable across lowerings (the app keeps the map).
  std::function<uint32_t(TrackId)> slotOf;
  float smoothingMs = 2.0f;                        // gain smoothing time constant
  // T2.6-fix1: the index into Resources::tunings of the tuning node's table for main tuning `id` with definition
  // `def` (always one with a signal). An index whose entry is null is allowed (the node then passes the static
  // pitches through). Unset: no tuning nodes (every note plays its static Hz, as before T2.6-fix1).
  std::function<uint32_t(TuningId, const TuningDef&)> tuningTableOf;
};

// One track's generated patch (immutable once made; shared between lowerings while the text is equal, so its
// NodeIds — the state keys — stay put).
struct LoweredPatch {
  std::string text;
  graph::Patch patch;
};

struct LoweredParam {
  std::string key;      // "<track id>:<name>"
  uint32_t track = 0;   // index into graph.tracks
  std::string name;     // the param's name in the track patch
  float value = 0.0f;   // current value in the param's unit
  std::optional<ParamRef> ref;
};

struct LoweredProject {
  std::vector<std::shared_ptr<const LoweredPatch>> patches;  // per graph track
  std::vector<TrackId> trackIds;                              // per graph track
  graph::ProjectGraph graph;                                  // points into `patches` and LowerOptions::library
  std::vector<LoweredParam> params;
  uint32_t master = 0;                                        // graph index of the master
};

// Errors: no master, a track patch that does not parse (a bug), an instrument missing from the library. `previous`
// (optional): patches with equal text are shared from it.
Result<LoweredProject> lower(const Project& project, const LowerOptions& options,
                             const LoweredProject* previous = nullptr);

// Every lowered param of a snapshot (keys, names, current values) without lowering the graph: the paramOnly fast
// path (O(tracks + sends): solo changes other tracks' gates). `track` is the index the track would have in lower().
// `tuningNodes`: as lower() with LowerOptions::tuningTableOf set (the tuning node params are included).
std::vector<LoweredParam> lowerParams(const Project& project, bool tuningNodes = false);

// The key of a document param, or "" for params that are not lowered (mute / solo: derived into the gates).
std::string paramKey(const ParamRef& ref);

// The main tuning of a MIDI track's track-level effective tuning (effectiveTuningRef without a clip) when that is a
// main tuning with a linear form (hasTuningSignal); invalid otherwise.
TuningId trackTuningSignal(const Project& project, const TrackPath& path) noexcept;
// T2.6-fix3 (M3 review H4a): the tuning signals a MIDI track's clips follow, one tuning node each, in node order:
// the track-level one (trackTuningSignal) first, then the main tunings with a signal that clips override to, by
// tuning id. A note following signal t is voice.tuning = tuningNodeIndex(signals, t) (1-based; 0: none).
std::vector<TuningId> trackTuningSignals(const Project& project, const TrackPath& path);
uint32_t tuningNodeIndex(std::span<const TuningId> signals, TuningId tuning) noexcept;

}  // namespace pq::doc
