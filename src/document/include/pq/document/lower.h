#pragma once

// Project → graph lowering (T3.17; IR lowering with stable ids T3.17-fix2): a document snapshot as the per-track
// ProjectGraph (pq/graph/project.h) the engine compiles, plus the param map. `document` depends on `graph` for this
// (ARCHITECTURE §1); the note sequences and automation lanes are lowered by the app (they are engine plain-data
// types, and `document` never includes `engine`; pq/app/project_link.h).
//
// ---- How a track becomes a patch (T3.17-fix2, M3 review H2) ---------------------------------------------------------
// Every track's patch is built as graph::Patch IR directly (no generated text): each node gets a NodeId derived from
// the document — a fixed id for the role's own nodes (the mixer stage, the instrument call, the ports), and for nodes
// that exist per document entity (a tuning node per tuning, a send input per sending track) an id computed from that
// entity's document id (nodeIdFor). State keys are hashes of NodeId call paths (TypedPatch::stateKey), so a node's
// state follows its document entity: adding, removing or reordering other entities never moves it. A lowering that
// produces the same IR for a track (equal structure and ids: patchHash + operator==) shares the previous patch, so
// the link knows exactly which tracks changed. The printed form (patchText) is only a debug dump.
// Device chains (T4.1, QUESTIONS Q129): a device instance is a subcircuit call named `d<device id>` whose NodeId
// is nodeIdFor(NodeRole::Device, device id, 0); its ports are that callee's ports (T4.7's modulation wires feed their
// param pins: "Modulation wires" below), so editing a chain keeps the state of every device it does not touch. The chain is
// wired left → right: the signal so far → the device's first `in` (through `pack(x, x)`, k = 1, when a mono signal
// meets a stereo input), its first `out` → the next device. Its params are left unconnected, so each is a runtime
// param `d<id>__<name>` of the track patch, linked to the project param `dev<id>:<name>`: a param edit is a param
// write. Its settings (`real const` inputs the user set) are literal constants at the call (stateless; ids from a
// per-track pool below kEntityNodeBase): a change recompiles that track's unit. Deactivated devices are left out; a
// device whose patch is not in the library (or not of its kind) is bypassed and listed in missingDevices.
//
// ---- Signal flow --------------------------------------------------------------------------------------------------
//   MIDI track    its instrument (the first device, normally BasicSynth, the M3 default instrument,
//                 stdlib/instruments/basic_synth.pqp; QUESTIONS Q15 A) → voice_sum → its audio effects = x (mono, or
//                 stereo after a stereo effect; the pan law stays constant power) → (without an active instrument:
//                 x = 0, no voices, no event slot)
//                   out  = gain(x, (pl·g, pr·g)),  g  = vg · gate · m         (constant-power pan, centre −3 dB/side)
//                   post = gain(x, (pl·s, pr·s)),  s  = vg · sendGate · m     (export: what post-fader sends read)
//                   pre  = gain(x, (pl·q, pr·q)),  q  = sendGate · m          (export: what pre-fader sends read)
//                 vg = volume ≤ −70 dB ? 0 : 10^(volume/20); m = mute < 0.5 (the activator, a param: its lane plays).
//   Group         x = `in` (stereo: the sum of what is routed into it) → its audio effects, balance pan (centre
//                 0 dB), the same three stages.
//   Return        x = the sum of its sends: for each track k that sends to it, `in s<k>` (that track's post or pre
//                 export) → gain_add(acc, s<k>, sg_k), sg_k = send<k> ≤ −70 ? 0 : 10^(send<k>/20). A send at −∞
//                 costs nothing (gain_add passes the accumulator through, in place). Then its audio effects, balance
//                 → out / post / pre as a group.
//   Master        x = `in` → its audio effects → balance → out = gain(x, (pl·vg, pr·vg)) (the device output).
//   gain / gain_add (pq/dsp/atoms.h) apply a param gain at block rate and smooth it per sample (one-pole,
//   smoothingMs) only while it moves, so a mixer move, a mute or a solo is click-free and a settled mixer costs one
//   multiply per sample per channel (M3 review H1).
//   The return set is NOT part of a track's patch: a track's patch has the same exports whatever returns exist, and
//   the sends live in the returns. Adding a return compiles the return and the master (whose input gains a wire);
//   no track changes (H2). Adding a track compiles the track, its destination and every return (each gains an
//   input).
//   Routing: Output::Default → the parent group, else the master; Master → the master; Track → that group if it is
//   active (a deactivated group: no output at all, M3 review L1; a return may output to a group, L2); None → no
//   output wire (sends only).
//   Sends: every main track (MIDI, group) has its implicit send to every active return (Live): a send level is a
//   param, so turning one up never recompiles. Return → return sends are lowered only when stored in the document
//   (Live keeps a return's sends to other returns disabled until used; QUESTIONS Q127). A send whose
//   target can reach the sender through the lowered routing (a return → return loop, a return routed into a group
//   that sends to it) would close a delay-free cycle: it is lowered only when stored, and gets an automatic 1-sample
//   delay (z1) on that send (SPEC §15 2026-10-24: a wire that closes a delay-free cycle gets an automatic z⁻¹);
//   LoweredProject::delayedSends lists them for the UI. Sends are added main tracks first, then returns, each in
//   track order: the send that closes a loop is the one delayed.
//   Deactivated tracks (Track::active = false, structural, Q15) and their members are left out of the graph.
//   Mute / solo: computeAudibility with mute ignored (audibility.h) → the `gate` (output) and `sendGate` params; the
//   mute is its own param `mute` (0 / 1; ≥ 0.5 = muted), so a mute lane plays (M3 review M3).
//   Tuning signal (T2.6-fix1, SPEC §3.3; QUESTIONS Q100, Q105): a MIDI track gets one tuning node per main tuning
//                 with a linear form its clips follow (trackTuningSignals: the track-level tuning, then clip
//                 overrides to other main tunings, T2.6-fix3), chained before the instrument:
//                   tun_i = tuning_hz(voice.degree, voice.equave, hz_{i−1}, cents_{i−1}, <gen_i>, tper_i, <st_i>,
//                                     tref_i, table: k_i, select: voice.tuning, node: i);  tune_voices(hz_N, cents_N)
//                 Node i takes the voices whose voice.tuning is i and passes the others through (MOS params: tgen,
//                 tper, tst, tref; EDO: tper = the equave, tref). k = the table's index in Resources::tunings, from
//                 LowerOptions::tuningTableOf. The node's NodeIds come from its tuning's id, so a clip override
//                 added or removed elsewhere in the track keeps every other node's state. Degree notes, Ratio notes
//                 anchored (through notes) to them, and reference-relative Ratio / Cents notes follow the node
//                 (doc::PitchFollow); private, Scala and MODMOS tunings keep their static Hz.
//
// ---- Params ----------------------------------------------------------------------------------------------------
// One LoweredParam per (track, patch param) with a stable string key (a device param "dev<device id>:<name>", T4.1;
// a wire's amount "wire<wire id>:amount" (patch name w<wire id>, on the target's track), T4.7;
// "<track id>:volume", ":pan", ":gate",
// ":sendGate", ":mute"; a send "<sender id>:send:<return id>", a param `send<sender id>` of the RETURN's patch; a
// tuning node's params "tuning:<tuning id>:reference", ":equave", ":generator", ":period", ":stretch" (patch names
// tref<i>, tper<i>, tgen<i>, tst<i> for node i), one entry per track that follows the tuning, all with the same key)
// that the app maps to a project param slot that never moves, and the current value; `ref` is the document ParamRef
// where there is one (automation lanes target it; gates are derived).
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
  // The device library (devices.h: every patch a device may name, and what those import). Required.
  const graph::PatchLibrary* library = nullptr;
  std::span<const graph::SubcircuitRef> externals; // unused since T3.17-fix2 (the IR calls library patches directly)
  // The note-event slot of a MIDI track (Timeline::eventSource(k)); stable across lowerings (the app keeps the map).
  // kNone (graph::kNone): the track has no slot (more MIDI tracks than the engine has slots): it is lowered without
  // its notes.
  std::function<uint32_t(TrackId)> slotOf;
  float smoothingMs = 2.0f;                        // gain smoothing time constant
  // T2.6-fix1: the index into Resources::tunings of the tuning node's table for main tuning `id` with definition
  // `def` (always one with a signal). An index whose entry is null is allowed (the node then passes the static
  // pitches through). Unset: no tuning nodes (every note plays its static Hz, as before T2.6-fix1).
  std::function<uint32_t(TuningId, const TuningDef&)> tuningTableOf;
};

// One track's patch (immutable once made; shared between lowerings while its IR is equal, so the link sees which
// tracks changed).
struct LoweredPatch {
  graph::Patch patch;
  uint64_t hash = 0;  // patchHash(patch)
};

// The canonical text of a lowered patch (graph::print): a debug dump (tools, tests), not what the engine compiles.
std::string patchText(const LoweredPatch& p);
// A hash of everything in a patch the program depends on, NodeIds included (sharing between lowerings).
uint64_t patchHash(const graph::Patch& p) noexcept;

// Which node a NodeId of a lowered patch stands for (T3.17-fix2). Fixed nodes of a role's own structure have ids
// below kEntityNodeBase; nodes that exist per document entity have nodeIdFor(role, entity id, k).
enum class NodeRole : uint8_t {
  TuningNode = 0,  // entity: the tuning id; k: its params, the tuning_hz node, its table / node constants
  SendInput = 1,   // entity: the sending track's id; k: its input port, level param, dB → gain, gain_add, z1
  Device = 2,      // T4.1: entity: a device id; k = 0: the device's call node, k = 1: its mono → stereo adapter
  // T4.7: entity: a wire id; k < kWireNodeSlots. Every entity kind draws its id from the one IdSource, so a wire id is
  // never a tuning, track or device id: a wire's nodes take the k slots of all three role spaces (k / 16 picks it).
  // k 0–15: the wire's own nodes; k 16–47: the nodes of the param it modulates (keyed by its lowest wire id).
  Wire = 3,
};
inline constexpr uint32_t kEntityNodeBase = 1024;
inline constexpr uint32_t kEntityNodeSlots = 16;  // k < 16 per (role, entity)
inline constexpr uint32_t kWireNodeSlots = 48;    // NodeRole::Wire: k < 48
// The NodeId (raw) of node k of `entity` in `role`, or 0 when it does not fit 32 bits (entity ids above
// ≈ 89 million: lower() then fails with an error).
uint32_t nodeIdFor(NodeRole role, uint32_t entity, uint32_t k) noexcept;

// ---- Modulation wires (T4.7; model.h "Modulation wires") ----------------------------------------------------------
// How a wire is lowered:
//   Lowered   both ends on lowered devices (active, in the library, on active tracks), the source an `export` of its
//             patch, the target a real `param` of its patch.
//   Delayed   lowered, with an automatic 1-sample delay (z1) on the wire: it would close a delay-free cycle (SPEC §15
//             2026-10-24). Wires are taken in id order, so the wire that closes a loop is the one delayed. Whether a
//             path is delay-free is read from the device patches (an atom's PortSpec::delayed inputs — z1, delay
//             lines, phasor's frequency — break it; an LFO's output does not depend on its audio input, so a wire
//             from an LFO into an earlier device of its chain is not a loop), through the chains, the routing and the
//             sends (a delayed send breaks a loop).
//   Inactive  an end's device (or track) is deactivated, or its patch is missing: left out until it plays again.
//   Invalid   the port or param is not one of the patch's (an edited patch lost it), or the param is a bool.
//   Refused   across tracks, it would close a cycle through two instrument tracks, which the engine cannot run (one
//             voice table per image, QUESTIONS Q84): left out, and the status line says so.
// The math, per modulated param of a device `d<id>` (all wires into it summed; the knob is the centre):
//   knob = param d<id>__<name> (the device's param, now a root param of the track patch: the same key and value as
//          an unmodulated param, dev<id>:<name>);  pk = its knob position (param rate: once per block)
//   s    = Σ w<wire>·src_i  (w<wire> = the wire's amount, a param "wire<id>:amount"; src = the source export's first
//          channel, reduced over voices by the wire's reducer when a voice signal meets a global target, through a z1
//          when delayed, arriving as the root `in m<wire>` / leaving as the root `export m<wire>` across tracks)
//   p    = clip(pk + s, 0, 1);  value = lo + p·(hi − lo)  (Linear)  or  lo·e^(p·ln(hi/lo))  (Log: @curve(exp), dB
//          gains with a positive floor), rounded for @steps params → the device's param pin
//   tap  = export tap_d<id>_<name> = (max(p − pk, 0), max(pk − p, 0)): how far the modulation moved the param up and
//          down; a metered bus (LoweredProject::taps) the views read for the knob's live ring. Zero exactly when the
//          modulation is, so a silent modulation keeps its unit idle.
enum class WireStatus : uint8_t { Lowered, Delayed, Inactive, Invalid, Refused };
struct WireState {
  WireId id;
  WireStatus status = WireStatus::Lowered;
  friend bool operator==(const WireState&, const WireState&) = default;
};
// A modulated param's tap: the export `port` of graph track `track`.
struct ModulationTap {
  DeviceId device;
  std::string param;
  uint32_t track = 0;  // index into graph.tracks
  std::string port;    // "tap_d<id>_<param>"
};

struct LoweredParam {
  std::string key;      // "<track id>:<name>"
  uint32_t track = 0;   // index into graph.tracks
  std::string name;     // the param's name in the track patch
  float value = 0.0f;   // current value in the param's unit
  std::optional<ParamRef> ref;
};

// A send lowered with an automatic 1-sample delay because it closes a delay-free loop (see the file comment).
struct DelayedSend {
  TrackId from, to;
};

struct LoweredProject {
  std::vector<std::shared_ptr<const LoweredPatch>> patches;  // per graph track
  std::vector<TrackId> trackIds;                              // per graph track
  graph::ProjectGraph graph;                                  // points into `patches` and LowerOptions::library
  std::vector<LoweredParam> params;
  uint32_t master = 0;                                        // graph index of the master
  std::vector<DelayedSend> delayedSends;
  std::vector<TrackId> withoutSlot;                           // MIDI tracks lowered without notes (no event slot)
  std::vector<DeviceId> missingDevices;                       // bypassed: patch not in the library (or not its kind)
  std::vector<WireState> wires;                               // T4.7: every wire of the project, in id order
  std::vector<ModulationTap> taps;                            // T4.7: one per modulated param (also graph.taps)
};

// T4.7: how every wire of `project` is lowered with `library` (the same answer lower() gives, without building the
// graph; the views draw the delayed and inactive ones). In id order.
std::vector<WireState> wireStates(const Project& project, const graph::PatchLibrary& library);

// T4.7: the knob travel of a device param — what its panel knob shows (pq/views/devices/device_model.h toPosition /
// fromPosition follow the same rule) and what modulation adds to: Log for @curve(exp) with positive ends and for dB
// gains with a positive floor (linear in dB), else Linear; @steps (and integer ranges with them) round the value.
struct ParamTravel {
  bool log = false;
  double lo = 0.0, hi = 1.0;
  bool round = false;
};
ParamTravel paramTravel(const graph::ParamDecl& decl) noexcept;
double travelPosition(const ParamTravel& t, double value) noexcept;  // [0, 1]
double travelValue(const ParamTravel& t, double position) noexcept;  // position clamped to [0, 1]

// Errors: no master, no library, an entity id too large for nodeIdFor, more setting constants in one track than the
// pool holds. `previous` (optional): patches with equal IR are shared from it.
Result<LoweredProject> lower(const Project& project, const LowerOptions& options,
                             const LoweredProject* previous = nullptr);

// Every lowered param of a snapshot (keys, names, current values) without lowering the graph: the paramOnly fast
// path (O(tracks + sends): solo changes other tracks' gates). `track` is the index the track would have in lower().
// `tuningNodes`: as lower() with LowerOptions::tuningTableOf set (the tuning node params are included). `library`:
// the device library lower() used (device params are included only with it; T4.1).
std::vector<LoweredParam> lowerParams(const Project& project, bool tuningNodes = false,
                                      const graph::PatchLibrary* library = nullptr);

// What, besides structural changes, decides the lowered graph: the sends that are lowered only while stored (return
// → return sends, sends that close a loop) and which of them are delayed. A send level edit that changes this key
// (a return's send to another return turned up from −∞, or back) needs a compile (the link compares it on every
// Param change).
std::vector<uint64_t> conditionalSendsKey(const Project& project);
// The sends lower() gives an automatic 1-sample delay (LoweredProject::delayedSends), without lowering the graph.
std::vector<DelayedSend> delayedSends(const Project& project);

// The key of a document param, or "" for params that are not lowered (solo: derived into the gates).
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
