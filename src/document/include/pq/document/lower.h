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
//
// ---- Params ----------------------------------------------------------------------------------------------------
// One LoweredParam per (track, patch param) with a stable string key ("<track id>:volume", ":pan", ":gate",
// ":sendGate", ":send:<return id>") that the app maps to a project param slot that never moves, and the current
// value; `ref` is the document ParamRef where there is one (automation lanes target it; gates are derived).
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
std::vector<LoweredParam> lowerParams(const Project& project);

// The key of a document param, or "" for params that are not lowered (tuning params: T2.6).
std::string paramKey(const ParamRef& ref);

}  // namespace pq::doc
